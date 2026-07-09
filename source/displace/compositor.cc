#include "compositor.h"

namespace sculptcore::displace {

using mesh::AttrData;
using mesh::AttrRef;
using mesh::AttrType;

static AttrData<float3> *layerColumn(mesh::Mesh &m, const util::string &name)
{
  AttrRef ref = m.v.attrs.find_attribute(AttrType::FLOAT3, name);
  if (!ref.exists()) {
    return nullptr;
  }
  return static_cast<AttrData<float3> *>(ref.data);
}

/** The edit target's rest snapshot, or nullptr when no layer is targeted. */
static AttrData<float3> *restColumn(mesh::Mesh &m)
{
  return layerColumn(m, util::string(mesh::SCULPT_LAYER_REST_ATTR));
}

static void removeRestColumn(mesh::Mesh &m)
{
  const util::string name(mesh::SCULPT_LAYER_REST_ATTR);
  for (int i = 0; i < int(m.v.attrs.attrs.size()); i++) {
    if (m.v.attrs.attrs[i].type == AttrType::FLOAT3 && m.v.attrs.attrs[i].name == name) {
      m.v.attrs.remove_attr(i);
      break;
    }
  }
}

LayerView resolveLayer(mesh::Mesh &m, int settingsIdx)
{
  LayerView view;
  if (settingsIdx < 0 || settingsIdx >= int(m.sculptLayers.size())) {
    return view;
  }
  mesh::SculptLayerSettings &st = m.sculptLayers[settingsIdx];
  view.data = layerColumn(m, st.name);
  view.weight = st.weight;
  view.enabled = st.enabled;
  view.frozen = st.frozen;
  view.settingsIdx = settingsIdx;
  return view;
}

util::Vector<LayerView> resolveStack(mesh::Mesh &m)
{
  util::Vector<LayerView> stack;
  for (int i = 0; i < int(m.sculptLayers.size()); i++) {
    LayerView view = resolveLayer(m, i);
    if (view.data) {
      stack.append(view);
    }
  }
  return stack;
}

bool LayerEditScope::begin(mesh::Mesh &m,
                           const util::string &layerName,
                           std::span<const int> verts)
{
  return begin(m, m.findSculptLayer(layerName), verts);
}

bool LayerEditScope::begin(mesh::Mesh &m, int settingsIdx, std::span<const int> verts)
{
  m_ = nullptr;
  view_ = resolveLayer(m, settingsIdx);
  if (!view_.data) {
    return false;
  }
  m_ = &m;
  verts_.resize(verts.size());
  old_.resize(verts.size());
  for (size_t i = 0; i < verts.size(); i++) {
    verts_[int(i)] = verts[i];
    old_[int(i)] = view_.data->safe_get(verts[i]);
  }
  return true;
}

void LayerEditScope::end()
{
  if (!m_) {
    return;
  }
  if (view_.frozen) {
    // Frozen layers are excluded from editing: revert the writes.
    for (int i = 0; i < int(verts_.size()); i++) {
      view_.data->materialize(verts_[i]);
      (*view_.data)[verts_[i]] = old_[i];
    }
  } else if (view_.enabled && view_.weight != 0.0f) {
    // Writes to a non-target layer move co like any settings mutation and
    // must mirror into the target's rest snapshot; writes to the target
    // itself are sculpting (rest fixed, a later fold absorbs them).
    AttrData<float3> *rest =
        view_.settingsIdx != m_->activeEditLayer ? restColumn(*m_) : nullptr;
    for (int i = 0; i < int(verts_.size()); i++) {
      int v = verts_[i];
      float3 d = view_.data->safe_get(v);
      float3 dd = (d - old_[i]) * view_.weight;
      m_->v.co[v] += dd;
      if (rest) {
        rest->materialize(v);
        (*rest)[v] += dd;
      }
    }
  }
  m_ = nullptr;
}

/** co += scale · dᵥ over every live vert (the enabled-contribution adjuster
 * shared by the settings mutators). The adjustment is mirrored into the edit
 * target's rest snapshot when one exists — mutators never run this on the
 * target itself (they clear the target first), so any co motion here belongs
 * to the rest surface, not the target's derived delta. */
static void addScaledLayer(mesh::Mesh &m, LayerView &view, float scale)
{
  if (!view.data || scale == 0.0f) {
    return;
  }
  AttrData<float3> *rest = restColumn(m);
  for (int v : m.v) {
    float3 dv = view.data->safe_get(v) * scale;
    m.v.co[v] += dv;
    if (rest) {
      rest->materialize(v);
      (*rest)[v] += dv;
    }
  }
}

void foldActiveLayer(mesh::Mesh &m, std::span<const int> verts)
{
  m.foldActiveSculptLayer(verts);
}

int setActiveEditLayer(mesh::Mesh &m, int settingsIdx)
{
  if (settingsIdx == m.activeEditLayer) {
    return m.activeEditLayer;
  }
  if (m.activeEditLayer >= 0) {
    m.foldActiveSculptLayer();
    m.activeEditLayer = -1;
    removeRestColumn(m);
  }
  LayerView view = resolveLayer(m, settingsIdx);
  if (!view.data || view.frozen) {
    return -1;
  }
  if (!view.enabled) {
    setLayerEnabled(m, settingsIdx, true);
  }
  if (view.weight != 1.0f) {
    setLayerWeight(m, settingsIdx, 1.0f);
  }
  // Snapshot rest = co − d. The column is current here (the invariant only
  // breaks once sculpting writes co with the target set).
  AttrRef &rref = m.v.attrs.ensure(AttrType::FLOAT3,
                                   util::string(mesh::SCULPT_LAYER_REST_ATTR),
                                   /*materialize=*/true);
  rref.flag = mesh::AttrFlag::TEMP;
  AttrData<float3> *rest = rref.get_data<float3>();
  rest->materialize_all();
  AttrData<float3> *d = view.data;
  for (int v : m.v) {
    (*rest)[v] = m.v.co[v] - d->safe_get(v);
  }
  m.activeEditLayer = settingsIdx;
  return settingsIdx;
}

void setLayerWeight(mesh::Mesh &m, int settingsIdx, float weight)
{
  if (settingsIdx == m.activeEditLayer) {
    // The target's weight is pinned to 1 — re-weighting it ends the edit
    // (fold + clear) first, so the fold never divides by the new weight.
    setActiveEditLayer(m, -1);
  }
  LayerView view = resolveLayer(m, settingsIdx);
  if (!view.data) {
    return;
  }
  if (view.enabled) {
    addScaledLayer(m, view, weight - view.weight);
  }
  m.sculptLayers[settingsIdx].weight = weight;
}

void setLayerEnabled(mesh::Mesh &m, int settingsIdx, bool enabled)
{
  if (!enabled && settingsIdx == m.activeEditLayer) {
    setActiveEditLayer(m, -1);
  }
  LayerView view = resolveLayer(m, settingsIdx);
  if (!view.data || view.enabled == enabled) {
    return;
  }
  addScaledLayer(m, view, enabled ? view.weight : -view.weight);
  m.sculptLayers[settingsIdx].enabled = enabled;
}

void setLayerFrozen(mesh::Mesh &m, int settingsIdx, bool frozen)
{
  if (settingsIdx < 0 || settingsIdx >= int(m.sculptLayers.size())) {
    return;
  }
  if (frozen && settingsIdx == m.activeEditLayer) {
    // A frozen layer cannot be the edit target.
    setActiveEditLayer(m, -1);
  }
  m.sculptLayers[settingsIdx].frozen = frozen;
}

void removeLayer(mesh::Mesh &m, int settingsIdx)
{
  if (settingsIdx == m.activeEditLayer) {
    // Fold first (via deactivation) so the subtraction below removes the
    // sculpted shape, not the stale column.
    setActiveEditLayer(m, -1);
  }
  LayerView view = resolveLayer(m, settingsIdx);
  if (view.settingsIdx < 0) {
    return;
  }
  if (view.data && view.enabled) {
    addScaledLayer(m, view, -view.weight);
  }
  util::string name = m.sculptLayers[settingsIdx].name;
  for (int i = 0; i < int(m.v.attrs.attrs.size()); i++) {
    if (m.v.attrs.attrs[i].type == AttrType::FLOAT3 && m.v.attrs.attrs[i].name == name) {
      m.v.attrs.remove_attr(i);
      break;
    }
  }
  m.sculptLayers.remove_at(settingsIdx, /*swap_end_only=*/false);
  if (settingsIdx < m.activeEditLayer) {
    m.activeEditLayer--;
  }
}

} // namespace sculptcore::displace
