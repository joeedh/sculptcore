namespace sculptcore::mesh {
struct Mesh;

Mesh *createCube(int dimen, float size = 0.5f, float sphereFac = 0.0f);

/* Procedural test/debug fixtures (outward-facing windings; quad-dominant with
 * triangle caps/poles where unavoidable). Used by the remesh tests + the
 * `make_shape` debug verb. See documentation/plans/quad-remeshing.md. */

/* Flat XY grid, nx*ny verts spanning [-size/2, size/2], (nx-1)*(ny-1) quads,
 * +Z normal. */
Mesh *makeGrid(int nx, int ny, float size = 1.0f);

/* Z-axis cylinder: `radialSegs` around, `heightSegs` along, `radius`/`height`
 * extents. `capped` adds triangle-fan end caps. The side gives the M1 curvature
 * fixture (kmax ~ 1/radius circumferential, kmin ~ 0 axial). */
Mesh *makeCylinder(int radialSegs,
                   int heightSegs,
                   float radius = 0.5f,
                   float height = 2.0f,
                   bool capped = true);

/* Torus around +Z: `majorSegs` around the ring, `minorSegs` around the tube,
 * radii `majorRadius`/`minorRadius`. All quads, genus 1 → the zero-singularity
 * (chi = 0) case. */
Mesh *makeTorus(int majorSegs,
                int minorSegs,
                float majorRadius = 1.0f,
                float minorRadius = 0.3f);

/* UV sphere of `radius`: `rings` latitude divisions (pole to pole), `segs`
 * longitude divisions. Quad bands + triangle-fan poles; analytic curvature
 * (kmin ~ kmax ~ 1/radius). */
Mesh *makeUVSphere(int rings, int segs, float radius = 1.0f);

} // namespace sculptcore::mesh
