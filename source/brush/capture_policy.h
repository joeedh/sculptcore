#pragma once

/** Undo-capture policy for generated kernel Pre stages (grids-native brush
 * path, G2). A kernel's `save` set lowers to per-domain CaptureSaveDesc
 * arrays; the Pre stage hands them to `TYPES::capture_policy::capture<Domain>`
 * so the same generated code serves any executor domain. MeshCapturePolicy is
 * the historical inline AttrSaver + parallelCapture block, verbatim; the grid
 * executor supplies its own policy over GridStrokeLog. */

#include "brush_command.h"
#include "meshlog/parallel_capture.h"

namespace sculptcore::brush {

/** What one `save` entry captures. Attr entries resolve their live layer
 * through ctx.boundAttrRef(handle) — the layer actually written this dab. */
enum class CaptureField : int { Co, No, Mask, Attr };

struct CaptureSaveDesc {
  CaptureField field;
  const char *handle; // Attr only, else null
  int flags;          // meshlog undo-flag bit(s), fixed at codegen
};

struct MeshCapturePolicy {
  template <mesh::ElemType Domain>
  static void capture(CommandCtxBase &ctx,
                      std::span<spatial::SpatialNode *> nodes,
                      std::span<const CaptureSaveDesc> saves)
  {
    if (!ctx.meshLog || nodes.empty() || saves.empty()) {
      return;
    }
    auto *m = nodes[0]->data->m;
    const int sid = ctx.meshLog->curStrokeId();

    sculptcore::meshlog::AttrSaver<Domain> saver;
    saver.ensure(*m);
    // A kernel's per-domain save set is tiny (defaults are 2 vert + 1 face).
    constexpr int kMaxSaves = 8;
    mesh::AttrRef refs[kMaxSaves];
    int n = 0, mask = 0;
    for (const CaptureSaveDesc &sv : saves) {
      if (n >= kMaxSaves) {
        break;
      }
      const mesh::AttrRef *r = nullptr;
      mesh::AttrRef maskRef;
      switch (sv.field) {
      case CaptureField::Co:
        r = &m->v.co;
        break;
      case CaptureField::No:
        if constexpr (Domain == mesh::ElemType::VERTEX) {
          r = &m->v.no;
        } else {
          r = &m->f.no;
        }
        break;
      case CaptureField::Mask:
        if constexpr (Domain == mesh::ElemType::VERTEX) {
          maskRef = m->v.attrs.find_attribute(mesh::AttrType::FLOAT, ".spatial.v.mask");
        } else {
          maskRef = m->f.attrs.find_attribute(mesh::AttrType::FLOAT, ".spatial.f.mask");
        }
        r = &maskRef;
        break;
      case CaptureField::Attr:
        r = ctx.boundAttrRef(sv.handle);
        break;
      }
      if (r && r->data) {
        refs[n] = *r;
        mask |= saver.add(refs[n], sv.flags);
        n++;
      }
    }
    if (!mask) {
      return;
    }
    auto *store = ctx.meshLog->elemStore(Domain);
    litestl::util::span<const mesh::AttrRef> span(refs, n);
    if constexpr (Domain == mesh::ElemType::VERTEX) {
      sculptcore::meshlog::parallelCapture<Domain>(
          *store, m->v.attrs, nodes, saver, span, sid, mask);
    } else {
      sculptcore::meshlog::parallelCapture<Domain>(
          *store, m->f.attrs, nodes, saver, span, sid, mask);
    }
  }
};

} // namespace sculptcore::brush
