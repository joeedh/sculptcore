#include "displace/compositor.h"
#include "mesh/mesh.h"

using namespace sculptcore;

extern "C" {

/* Sculpt-layer settings mutators for the app's layer-stack UI (V5). Each one
 * keeps evaluated `v.co` current through the compositor (co += Δcontribution);
 * the caller owns undo (a ToolOp re-applies the previous value on undo — the
 * adjustment is its own inverse up to fp rounding). Reads live on the bound
 * Mesh surface (sculptLayerWeight/Enabled/Frozen). */

void Mesh_layerSetWeight(mesh::Mesh *m, int li, float weight)
{
  if (m) {
    displace::setLayerWeight(*m, li, weight);
  }
}

void Mesh_layerSetEnabled(mesh::Mesh *m, int li, int enabled)
{
  if (m) {
    displace::setLayerEnabled(*m, li, enabled != 0);
  }
}

void Mesh_layerSetFrozen(mesh::Mesh *m, int li, int frozen)
{
  if (m) {
    displace::setLayerFrozen(*m, li, frozen != 0);
  }
}

void Mesh_layerRemove(mesh::Mesh *m, int li)
{
  if (m) {
    displace::removeLayer(*m, li);
  }
}
}
