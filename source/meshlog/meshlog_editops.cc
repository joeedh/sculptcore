#include "meshlog_base.h"

namespace sculptcore::meshlog {

  void MeshLog::extrudeRegion(mesh::Mesh *m, util::Vector<float> &outNormal)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::ExtrudeResult res;
    mesh::ops::extrudeRegion(*m, callbacks(), res, selectFlushPreferOpDomain);
    endStep();
    outNormal.append(res.normal[0]);
    outNormal.append(res.normal[1]);
    outNormal.append(res.normal[2]);
  }


  void MeshLog::extrudeIndividual(mesh::Mesh *m, util::Vector<float> &outNormal)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::ExtrudeResult res;
    mesh::ops::extrudeIndividual(*m, callbacks(), res, selectFlushPreferOpDomain);
    endStep();
    outNormal.append(res.normal[0]);
    outNormal.append(res.normal[1]);
    outNormal.append(res.normal[2]);
  }


  void MeshLog::extrudeWireVerts(mesh::Mesh *m, util::Vector<float> &outNormal)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::ExtrudeResult res;
    mesh::ops::extrudeWireVerts(*m, callbacks(), res, selectFlushPreferOpDomain);
    endStep();
    outNormal.append(res.normal[0]);
    outNormal.append(res.normal[1]);
    outNormal.append(res.normal[2]);
  }


  void MeshLog::splitFacesOff(mesh::Mesh *m, util::Vector<float> &outNormal)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::ExtrudeResult res;
    mesh::ops::splitFacesOff(*m, callbacks(), res, selectFlushPreferOpDomain);
    endStep();
    outNormal.append(res.normal[0]);
    outNormal.append(res.normal[1]);
    outNormal.append(res.normal[2]);
  }


  void MeshLog::subdivideEdges(mesh::Mesh *m, int numCuts, util::Vector<int> &outVerts)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::subdivideEdges(
        *m, callbacks(), numCuts, outVerts, selectFlushPreferOpDomain);
    endStep();
  }


  void MeshLog::loopCut(mesh::Mesh *m, int seedEdge, util::Vector<int> &outVerts)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::loopCut(*m, callbacks(), seedEdge, outVerts);
    endStep();
  }


  void MeshLog::loopCutAtRay(mesh::Mesh *m,
                    spatial::SpatialTree *tree,
                    const math::float3 &origin,
                    const math::float3 &dir,
                    util::Vector<int> &outVerts)
  {
    if (!m || !tree) {
      return;
    }
    spatial::CastRayIsect isect;
    if (!tree->castRay(origin, dir, isect) || isect.faceIndex == ELEM_NONE) {
      return;
    }
    int seed = mesh::faceEdgeNearestPoint(*m, isect.faceIndex, isect.p);
    if (seed == ELEM_NONE) {
      return;
    }
    setActiveMesh(m);
    beginStep(false);
    mesh::ops::loopCut(*m, callbacks(), seed, outVerts);
    endStep();
  }


  void MeshLog::insetRegion(mesh::Mesh *m,
                   util::Vector<int> &insetVerts,
                   util::Vector<float> &baseCo,
                   util::Vector<float> &tangent)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    mesh::ops::insetRegion(
        *m, callbacks(), insetVerts, baseCo, tangent, selectFlushPreferOpDomain);
  }


  void MeshLog::bevelVerts(mesh::Mesh *m,
                  util::Vector<int> &verts,
                  util::Vector<float> &baseCo,
                  util::Vector<float> &tangent)
  {
    if (!m) {
      return;
    }
    setActiveMesh(m);
    mesh::ops::bevelVerts(
        *m, callbacks(), verts, baseCo, tangent, selectFlushPreferOpDomain);
  }

} // namespace sculptcore::meshlog
