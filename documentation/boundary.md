## Tool Definitions

Tool definitions we'll need later:

1. Poly brush.  At start of stroke, assigns an unused integer id to be applied to
   a face integer attribut OR if shift is held, an existing id in the attribute 
  under the brush will instead be cached at the start of the stroke.  This id is
  assigned to faces under the brush during stroke.  Poly groups can be backed by 
  8- 16- and 64-bit integer attributes.
2. Color brush.  Can paint into any 1, 3 or 4 component attribute, e.g. byte3, int4, 
   float4.

A note on masks: a sculpt mask is a 1-component float attribute assigned to vertices; 
any such attribute can be used, the current attribute used as the sculpt mask is stored
as a string in the mesh.

## Boundary Conditions

A very important part of digital sculpting is boundary conditions.
Boundary conditions are everywhere--sculpting, even with dynamic topology
remeshing, should properly preserve:

* UV map charts for an arbitrary number of charts (users can have multiple float2 uv attributes)
 - UV chart boundaries (via the real edges and vertices they map to) are preserved on the mesh similarly
   to poly groups or projected boundaries.
 - Smooth brushes must either
   + Run a solver to preserve the relationship between the UV areas and the boundaries
     (most vulgar implementation is laplacian smoothing the charts).
   + Cancel out the smooth brush's displacement alone the tangent plane using localized raycast 
     reprojection of the pre-smoothed vertex positions. Thoeretically correct but can be numerically unstable.
* Edges marked as projected boundaries, used to create 'curves' on the mesh.  Rules:
  - Collapsing a projected edge into a projected edge is allowed
  - Collapsing a projected edge into an unprojected edge is not allowed
  - Subdividing a projected edge is allowed
  - Vertices with projected edges should only consider projected edges during smoothing;
    however this smoothing should be projected into the normal plane.  The exception is
    verts with only one projected edge, they should consider all neighbors.
* Poly group boundaries (poly groups are just integer user-defined ids assigned to faces).
  The smoothing and remeshing rules for these are similar to projected edge boundaries.
* Edges marked as sharp; smooth brushes produce geometric discontinuities across these edges.

