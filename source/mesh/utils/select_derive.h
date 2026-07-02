#pragma once

#include "litestl/util/set.h"
#include "litestl/util/vector.h"

/** On-demand selection-domain derivation (documentation/plans/selectFlush.md).
 * The three `select` bool columns stay independent; these are pure, read-only
 * queries an op calls at invocation time to fill in a domain the user didn't
 * select directly (e.g. vert-only selection -> extrude face regions). No
 * MeshLog, no mutation, no undo surface. */

namespace sculptcore::mesh {

struct Mesh;

enum class DeriveRule {
  All, // strict: every corner/endpoint of the target element must be selected
  Any, // touching: any corner/endpoint selected is enough
};

/** Faces derived from vert/edge selection. All: every corner vert selected, or
 * every edge selected. Any: any corner vert or edge selected. */
litestl::util::Set<int> deriveFaceSelection(Mesh &m, DeriveRule rule = DeriveRule::All);

/** Edges derived from vert/face selection. All: both endpoint verts selected.
 * Any: either endpoint selected, or the edge belongs to a selected face. */
litestl::util::Set<int> deriveEdgeSelection(Mesh &m, DeriveRule rule = DeriveRule::All);

/** Union of all touched verts: selected verts, endpoints of selected edges,
 * corners of selected faces (incl. hole loops). The one sane rule. */
litestl::util::Set<int> deriveVertSelection(Mesh &m);

/** What a box-modeling op actually consumes (with the Edge/Vert siblings
 * below). With `preferOpDomain` (the sculptcore.select_flush_prefer_op_domain
 * feature flag, default on) an explicit selection in the op's own domain wins
 * outright and derivation only fills an empty domain; with it off, explicit
 * and derived are merged (set union). */
litestl::util::Set<int> resolveFaceSelection(Mesh &m, bool preferOpDomain = true);
litestl::util::Set<int> resolveEdgeSelection(Mesh &m, bool preferOpDomain = true);
litestl::util::Set<int> resolveVertSelection(Mesh &m, bool preferOpDomain = true);

/** Boundary edges of the current selected-face region: an edge with exactly one
 * selected radial face. Covers interior, mesh-boundary, and single-face cases.
 * Appends edge indices to `out`. */
void regionBoundaryEdges(Mesh &m, litestl::util::Vector<int> &out);

/** Every vertex touched by any selected element, deduped — the transform
 * bridge's "movable" set. deriveVertSelection flattened into a Vector. */
void gatherMovableVerts(Mesh &m, litestl::util::Vector<int> &out);

} // namespace sculptcore::mesh
