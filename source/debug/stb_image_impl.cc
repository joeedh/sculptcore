// Single TU that compiles the stb_image implementation for the debug app
// (set_texture image=<path>). Kept separate so the ~8k-line header isn't
// recompiled with every script.cc edit. The write side lives in
// vulkan/vk_screenshot.cc under the distinct STB_IMAGE_WRITE_IMPLEMENTATION.
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
