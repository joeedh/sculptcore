#pragma once

#ifdef SBRUSH_GPU_DISPATCH

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sculptcore::vulkan {
struct BrushComputeDispatch;
struct GpuNormalPass;
struct VulkanBackend;
}
namespace sculptcore::spatial {
struct SpatialNode;
}

namespace sculptcore::debug_app {

struct Scene;

/* Persistent GPU brush-stroke session over a Vulkan compute pipeline. Uploads
 * the full mesh once (begin), dispatches one dab per call (dab), then reads the
 * result back and logs it for undo (end). The kernel is resolved from
 * scene.currentTool. Shared by the batch script path (runBrushStrokeGPU) and
 * interactive mode so both drive the identical marshaling and stay bit-modulo-fp
 * with the C++ executor — that parity is what `make.mjs sbrush-verify` asserts.
 *
 * One session per stroke: BrushComputeDispatch keeps co/no/mask across dabs so
 * each dab reads the previous dab's result, matching CommandExecutor. */
class GpuStrokeSession {
 public:
  GpuStrokeSession() = default;
  ~GpuStrokeSession();
  GpuStrokeSession(const GpuStrokeSession &) = delete;
  GpuStrokeSession &operator=(const GpuStrokeSession &) = delete;

  /* Write a --gpu-capture JSON fixture for this stroke (batch path only). Call
   * before begin(); `prefix` is the file-name stem. Interactive mode never
   * captures. */
  void enableCapture(const std::string &prefix) { capturePrefix_ = prefix; }

  /* Opt into the GPU-resident live-render path (interactive mode only). Call
   * before begin() with the backend that renders the scene (swapchain in a
   * window). When set, dabs recompute normals + scatter into the render VBOs on
   * the GPU and read back only the touched verts per dab, so the mesh deforms
   * live. When NOT set (the batch/verify path), begin/dab/end run the original
   * full-readback-at-end code, keeping sbrush-verify byte-identical. */
  void enableLiveRender(vulkan::VulkanBackend *backend) { liveBackend_ = backend; }

  /* Resolve the kernel from scene.currentTool, bring up the device, upload the
   * mesh + (if needed) neighbor topology + bound texture, and open a meshlog
   * step. Returns false (with `err` set) if the tool has no GPU kernel or GPU
   * init fails. */
  bool begin(Scene &scene, std::string &err);

  /* Dispatch one dab at `origin` with surface `normal`. A dab that touches no
   * nodes is a no-op returning true. */
  bool dab(Scene &scene, litestl::math::float3 origin,
           litestl::math::float3 normal, std::string &err);

  /* Read the result back into the mesh, snapshot the touched nodes for undo,
   * write the capture fixture (if enabled), and close the meshlog step. Safe to
   * call once after begin() even if no dab landed. */
  void end(Scene &scene);

 private:
  vulkan::BrushComputeDispatch *disp_ = nullptr;
  const char *kernel_ = nullptr;
  bool needsNeighbors_ = false;
  bool writesMask_ = false;
  int vcount_ = 0;
  litestl::util::Vector<spatial::SpatialNode *> touched_;

  /* GPU-resident live-render path (interactive only; null in batch/verify). */
  vulkan::VulkanBackend *liveBackend_ = nullptr;
  vulkan::GpuNormalPass *normalPass_ = nullptr;

  /* Host copy of the normal topology (see buildNormalTopology) + per-dab work
   * scratch. topoTriVerts_ is 3*topoTriCount_ global vertex indices; topoMeta_
   * is uvec2 (offset,count) per vertex into topoList_, the incident-tri CSR.
   * tri/vertStamp_ are generation stamps for O(1) dedup in buildDabWork. */
  litestl::util::Vector<uint32_t> topoTriVerts_, topoMeta_, topoList_;
  litestl::util::Vector<uint32_t> triStamp_, vertStamp_;
  litestl::util::Vector<uint32_t> workTris_, workVerts_;
  int topoTriCount_ = 0;
  uint32_t stampGen_ = 0;

  /* Build global triangle topology + vertex->incident-tri CSR and upload it to
   * normalPass_ (once, at begin — the mesh is static during a stroke). A copy
   * is kept on the session (topo*_ below) so each dab can derive its localized
   * normal work set without re-triangulating. */
  void buildNormalTopology(Scene &scene);
  /* Fill workTris_/workVerts_ with the normals work set for a dab that moved
   * `uverts`: every incident triangle of a moved vert (its face normal changes)
   * plus the three verts of each such triangle (their vertex normal changes).
   * Deduped via the per-element stamp arrays so cost scales with the dab, not
   * the mesh. */
  void buildDabWork(const litestl::util::Vector<uint32_t> &uverts);
  /* Snapshot a leaf's pre-dab co/no/f.no for undo, once per node per stroke. */
  void snapshotNode(Scene &scene, spatial::SpatialNode *node);
  /* Finalize the live stroke (sync CPU mesh, resolve normals per end-mode, give
   * the render VBOs back to the CPU path). coOut/maskOut are the final readback
   * end() already fetched. */
  void finishLive(Scene &scene, litestl::util::Vector<float> &coOut,
                  litestl::util::Vector<float> &maskOut);

  /* --gpu-capture state (populated only when enableCapture was called). Held
   * from begin() through end(), where one JSON fixture is emitted. */
  std::string capturePrefix_;
  bool cap_ = false;
  std::string capCo_, capNo_, capMask_, capNbrMeta_, capNbrVerts_, capTexture_;
  std::vector<std::string> capDabs_;
};

} // namespace sculptcore::debug_app

#endif // SBRUSH_GPU_DISPATCH
