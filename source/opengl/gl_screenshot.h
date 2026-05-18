#pragma once

namespace sculptcore::opengl {

/** Reads the currently-bound framebuffer (entire viewport) and writes it
 *  to `path` as a PNG. Flips rows so the output is top-down. Returns true
 *  on success. */
bool captureToPNG(const char *path, int width, int height);

} // namespace sculptcore::opengl
