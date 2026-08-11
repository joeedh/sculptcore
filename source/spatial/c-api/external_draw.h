/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>

/* External draw provider: the native side of Blender's custom-mode external
 * draw seam (Blender's `BKE_object_draw_provider.hh`). These structs mirror
 * Blender's ABI by layout — the provider function pointer is handed to Blender,
 * which calls it and reads the node array back. The ABI version guards drift
 * across the repo boundary. */

extern "C" {

#define SC_EXTERNAL_DRAW_ABI_VERSION 3

enum ScExternalDrawUpdate {
  SC_EXTERNAL_DRAW_UPDATE_NONE = 0,
  SC_EXTERNAL_DRAW_UPDATE_DATA = (1 << 0),
  SC_EXTERNAL_DRAW_UPDATE_TOPOLOGY = (1 << 1),
};

struct ScExternalDrawNode {
  const float (*positions)[3];
  const float (*normals)[3];
  const void **attrs;
  int verts_num;
  int material_index;
  uint32_t update_flags;
  /** Stable per-node identity (SpatialNode::id) so the consumer can key its
   * per-node GPU caches across list reorders (dyntopo merges/repartitions). */
  uint32_t node_id;
  float bounds_min[3];
  float bounds_max[3];
  /** Optional node-local triangle index stream: 3 indices per triangle into
   * this node's vertex streams. Null -> non-indexed soup (verts_num is then a
   * multiple of 3, every 3 verts a triangle). When set, normals must be
   * provided (the consumer's flat-normal soup fallback does not apply). */
  const uint32_t *indices;
  int indices_num;
};

struct ScExternalDrawAttrRequest {
  int attrs_num;
  const char *const *attr_names;
};

struct ScExternalDrawProvider {
  int abi_version;
  int (*nodes_get)(void *user_data,
                   unsigned int object_key,
                   const ScExternalDrawAttrRequest *req,
                   ScExternalDrawNode **r_nodes);
  void (*nodes_release)(void *user_data, unsigned int object_key);
  void *user_data;
};

/** Register/unregister a `SpatialTree` under an object key (Blender's original
 * `ID.session_uid`). The tree is borrowed; unregister on mode exit. */
void sc_external_draw_register(unsigned int object_key, void *spatial_tree);

/** Custom (non-SpatialTree) geometry sources — e.g. the multires grids draw
 * source (subdiv/c-api) — register through a small vtable so this module
 * stays independent of theirs. node_id values of custom sources must live at
 * or above SC_EXTERNAL_DRAW_CUSTOM_ID_BASE; SpatialNode ids stay below it. */
#define SC_EXTERNAL_DRAW_CUSTOM_ID_BASE 0x40000000u

typedef struct ScExternalDrawSourceOps {
  int (*nodes_get)(void *src, const ScExternalDrawAttrRequest *req,
                   ScExternalDrawNode **r_nodes);
  void (*update)(void *src);
  void (*destroy)(void *src);
} ScExternalDrawSourceOps;

/** Register `src` (owned by the registry from here on: unregister / a
 * replacing register calls ops->destroy). `ops` must outlive the entry. */
void sc_external_draw_register_custom(unsigned int object_key,
                                      void *src,
                                      const ScExternalDrawSourceOps *ops);
void sc_external_draw_unregister(unsigned int object_key);

/** Refresh the registered tree's per-GPU-node CPU buffers (headless fill via a
 * shared GPUManager) so the next `nodes_get` reads current geometry. Call after
 * mode enter and after each stroke. No-op for an unregistered key. */
void sc_external_draw_update(unsigned int object_key);

/** Switch a tree to the dynamic per-attribute GPU layout with a fixed
 * color@0 (vertex float4) + uv@1 (corner float2) slot set, so the provider
 * exposes those attributes to Blender. Load the matching mesh attribute layers
 * (`color`, `uv`) before calling; a missing one draws its default. */
void sc_external_draw_enable_dynamic(void *spatial_tree);

/** Set the face `group` id the fset stream displays as "no face set"
 * (untinted), alongside group 0 — Blender's face_sets_color_default. */
void sc_external_draw_set_default_group(void *spatial_tree, int group);

/** The provider Blender registers on the mode via
 * `BKE_object_mode_draw_provider_set`. Stable address for the session. */
const ScExternalDrawProvider *sc_external_draw_provider(void);
}
