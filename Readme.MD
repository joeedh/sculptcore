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

## Key features:

+ Dynamic topology with uv/color/etc preservation
+ Boundary constraint system to preserve boundaries (edge seams, sharp edges, uv chart boundaries, etc).
+ Multires subdivision surface editing
+ Vector displacement maps
+ WASM target
