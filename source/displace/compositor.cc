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
    for (int i = 0; i < int(verts_.size()); i++) {
      int v = verts_[i];
      float3 d = view_.data->safe_get(v);
      m_->v.co[v] += (d - old_[i]) * view_.weight;
    }
  }
  m_ = nullptr;
}

/* co += scale · dᵥ over every live vert (the enabled-contribution adjuster
 * shared by the settings mutators). */
static void addScaledLayer(mesh::Mesh &m, LayerView &view, float scale)
{
  if (!view.data || scale == 0.0f) {
    return;
  }
  for (int v : m.v) {
    float3 d = view.data->safe_get(v);
    m.v.co[v] += d * scale;
  }
}

void setLayerWeight(mesh::Mesh &m, int settingsIdx, float weight)
{
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
  m.sculptLayers[settingsIdx].frozen = frozen;
}

void removeLayer(mesh::Mesh &m, int settingsIdx)
{
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
}

} // namespace sculptcore::displace
