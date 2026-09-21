/** Displacement layers: the composite mix, the layer table and its edit target. */

#include "multires.h"

#include "grid_domain.h"
#include "grid_draw_source.h"
#include "multires_tuning.h"

#include "vdm/vdm_store.h"

#include "displace/frames.h"
#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"

#include "litestl/util/alloc.h"
#include "litestl/util/assert.h"
#include "litestl/util/task.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace litestl;
using litestl::math::float2;
using litestl::math::float3;
using litestl::math::float4;
using litestl::util::Assert;
using litestl::util::Vector;

namespace sculptcore::subdiv {

void Multires::compositeMix(Vector<ChannelMix> &out) const
{
  out.clear();
  out.append({0, 1.0f});
  if (!cage_) {
    return;
  }
  for (int i = 0; i < int(cage_->sculptLayers.size()); i++) {
    const mesh::SculptLayerSettings &st = cage_->sculptLayers[i];
    if (!st.enabled || st.weight == 0.0f) {
      continue;
    }
    int ch = store.findChannel(st.name);
    if (ch > 0) {
      // Every consumer of the mix weights and subtracts these, so this is the
      // chokepoint for the store's "no float math on a typed channel" rule.
      Assert(store.channelDomain(ch) == GridElemDomain::Vertex &&
                 store.channelInterpolatable(ch),
             "composited grid channel takes float math");
      out.append({ch, st.weight});
    }
  }
}

int Multires::channelForLayer(int li) const
{
  if (!cage_ || li < 0 || li >= int(cage_->sculptLayers.size())) {
    return -1;
  }
  int ch = store.findChannel(cage_->sculptLayers[li].name);
  return ch > 0 ? ch : -1;
}

bool Multires::dispNonZero(int level)
{
  Vector<ChannelMix> mix;
  compositeMix(mix);
  int S = GridsStore::sideForLevel(level), w = S + 1;
  for (const ChannelMix &m : mix) {
    for (int g = 0; g < store.gridCount(); g++) {
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          const float *d = store.elem(level, m.channel, g, u, v);
          if (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

void Multires::refreshAfterLayerChange()
{
  dropDomains(0);
  for (int l = 1; l <= maxLevel(); l++) {
    posCache_[l - 1].reset();
  }
  for (int i = int(slots_.size()) - 1; i >= 0; i--) {
    evictSlot(i);
  }
  if (activeLevel_ >= 1) {
    materialize(activeLevel_);
  }
}

int Multires::layerAdd()
{
  if (!cage_) {
    return -1;
  }
  // Unique against both settings rows and store channels.
  util::string name;
  for (int n = 0;; n++) {
    char buf[32];
    if (n == 0) {
      std::snprintf(buf, sizeof(buf), "slayer");
    } else {
      std::snprintf(buf, sizeof(buf), "slayer.%03d", n);
    }
    name = util::string(buf);
    if (cage_->findSculptLayer(name) < 0 && store.findChannel(name) < 0) {
      break;
    }
  }
  mesh::SculptLayerSettings st;
  st.name = name;
  cage_->sculptLayers.append(std::move(st));
  // Delta, like "disp": a sculpt layer holds a per-level displacement
  // correction, so a new finest level starts at zero and a dropped one takes
  // its correction with it.
  store.addChannel(name,
                   3,
                   GridElemDomain::Vertex,
                   mesh::AttrType::FLOAT,
                   /*persist=*/true,
                   GridLevelRule::Delta);
  // A fresh zero channel at weight 1 changes no level positions: no refresh.
  return int(cage_->sculptLayers.size()) - 1;
}

void Multires::layerRemove(int li)
{
  if (!cage_ || li < 0 || li >= int(cage_->sculptLayers.size())) {
    return;
  }
  if (li == cage_->activeEditLayer) {
    setEditTarget(-1); // folds pending edits into the layer first
  } else if (activeLevel_ >= 1) {
    writeback(activeLevel_); // pending edits keep their old attribution
  }
  int ch = store.findChannel(cage_->sculptLayers[li].name);
  if (ch > 0) {
    store.removeChannel(ch);
  }
  cage_->sculptLayers.remove_at(li, /*swap_end_only=*/false);
  if (li < cage_->activeEditLayer) {
    cage_->activeEditLayer--;
  }
  refreshAfterLayerChange();
}

void Multires::layerSetWeight(int li, float weight)
{
  if (!cage_ || li < 0 || li >= int(cage_->sculptLayers.size())) {
    return;
  }
  if (li == cage_->activeEditLayer) {
    // The target's weight is pinned to 1 — re-weighting it ends the edit.
    setEditTarget(-1);
  } else if (activeLevel_ >= 1) {
    writeback(activeLevel_);
  }
  mesh::SculptLayerSettings &st = cage_->sculptLayers[li];
  if (st.weight == weight) {
    return;
  }
  st.weight = weight;
  if (st.enabled) {
    refreshAfterLayerChange();
  }
}

void Multires::layerSetEnabled(int li, int enabled)
{
  if (!cage_ || li < 0 || li >= int(cage_->sculptLayers.size())) {
    return;
  }
  if (!enabled && li == cage_->activeEditLayer) {
    setEditTarget(-1);
  } else if (activeLevel_ >= 1) {
    writeback(activeLevel_);
  }
  mesh::SculptLayerSettings &st = cage_->sculptLayers[li];
  if (st.enabled == (enabled != 0)) {
    return;
  }
  st.enabled = enabled != 0;
  refreshAfterLayerChange();
}

void Multires::layerSetFrozen(int li, int frozen)
{
  if (!cage_ || li < 0 || li >= int(cage_->sculptLayers.size())) {
    return;
  }
  if (frozen && li == cage_->activeEditLayer) {
    // A frozen layer cannot be the edit target.
    setEditTarget(-1);
  }
  cage_->sculptLayers[li].frozen = frozen != 0;
}

int Multires::setEditTarget(int li)
{
  if (!cage_) {
    return -1;
  }
  if (li == cage_->activeEditLayer) {
    return li;
  }
  // Pending level edits belong to the OLD target: fold them first.
  if (activeLevel_ >= 1) {
    writeback(activeLevel_);
  }
  cage_->activeEditLayer = -1;
  if (li < 0 || li >= int(cage_->sculptLayers.size())) {
    return -1;
  }
  mesh::SculptLayerSettings &st = cage_->sculptLayers[li];
  if (st.frozen || channelForLayer(li) < 0) {
    return -1;
  }
  bool changed = !st.enabled || st.weight != 1.0f;
  st.enabled = true;
  st.weight = 1.0f; // pin: writeback must never divide by the target weight
  cage_->activeEditLayer = li;
  if (changed) {
    refreshAfterLayerChange();
  }
  return li;
}

int Multires::editTarget() const
{
  return cage_ ? cage_->activeEditLayer : -1;
}

int Multires::layerCount() const
{
  return cage_ ? int(cage_->sculptLayers.size()) : 0;
}

float Multires::layerWeight(int li) const
{
  return cage_ ? cage_->sculptLayerWeight(li) : 0.0f;
}

int Multires::layerEnabled(int li) const
{
  return cage_ ? cage_->sculptLayerEnabled(li) : 0;
}

int Multires::layerFrozen(int li) const
{
  return cage_ ? cage_->sculptLayerFrozen(li) : 0;
}

void Multires::layerTableOut(Vector<float> &out)
{
  out.clear();
  if (!cage_) {
    return;
  }
  for (const mesh::SculptLayerSettings &st : cage_->sculptLayers) {
    out.append(st.weight);
    out.append(st.enabled ? 1.0f : 0.0f);
    out.append(st.frozen ? 1.0f : 0.0f);
  }
}

void Multires::layerTableRestore(Vector<float> &table)
{
  if (!cage_) {
    return;
  }
  cage_->activeEditLayer = -1;
  cage_->sculptLayers.clear();
  for (int ch = 1; ch < store.channelCount(); ch++) {
    mesh::SculptLayerSettings st;
    st.name = store.channelName(ch);
    int k = (ch - 1) * 3;
    if (k + 2 < int(table.size())) {
      st.weight = table[k];
      st.enabled = table[k + 1] != 0.0f;
      st.frozen = table[k + 2] != 0.0f;
    }
    cage_->sculptLayers.append(std::move(st));
  }
  refreshAfterLayerChange();
}

} // namespace sculptcore::subdiv
