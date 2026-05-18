#pragma once

namespace sculptcore::debug_app {

struct Scene;

namespace state_dump {

struct Options {
  bool mesh = true;
  bool spatial = true;
  bool brush = true;
};

/** Write a JSON dump of the scene to `path`. Returns false on I/O failure. */
bool writeJSON(Scene &scene, const char *path, const Options &opts);

} // namespace state_dump
} // namespace sculptcore::debug_app
