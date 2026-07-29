# Sculptcore

Sculptcore is a library for implementing digital sculpting tools.  It is meant to run in a host digital content 
creator (like e.g. Blender, Maya).  Sculpt brushes are written in a small domain-specific language and can
run on the CPU and the GPU via WebGPU or Vulkan.

## Why Sculptcore 

The motivation for this project is to provide a solid foundation to build digital sculpting systems in a variety of
host software.

## Example Integration

You can find an example of sculptcore integrated in a small web DCC at [https://github.com/joeedh/webgl-app-framework](https://github.com/joeedh/webgl-app-framework).
([Try it out here](https://joeedh.github.io/webgl-app-framework/)).

([Blender Addon Here](https://github.com/joeedh/sculptcore-blender-addon).

## Key features:

+ Dynamic topology with uv/color/etc preservation
+ Boundary constraint system to preserve boundaries (edge seams, sharp edges, uv chart boundaries, etc).
+ Multires subdivision surface editing
+ Vector displacement maps
+ WASM target

## Technical Details

### Mesh representation

Sculptcore uses a paged index-based BREP mesh structure; it is a full BREP (with support for holes in faces)
with some optional performance optimizations.  The structure supports incremental defragmentation.
The goal of the structure is to support a variety of workflows, handle large amounts of data, limit memory
bandwidth and improve cpu cache coherency.

### Dynamic remeshing

It has a triangle-based dynamic topology remesher similar to blender's, but with better support for handling
various kinds of edge constraints.  It can use edge flipping to preserve topology quality and/or recursive 
splitting (which is what Blender does).  It interpolates attributes including UVs.

### Multires

Multires is handled differently from PTex vector displacement maps to avoid numerical instability.
