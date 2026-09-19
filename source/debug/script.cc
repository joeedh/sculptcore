#include "script.h"

#include "roughness.h"
#include "scene.h"
#include "state_dump.h"

#include "brush/brush_executor.h"
#include "brush/brushes/all.h"
#include "brush/grid_executor.h"
#include "brush/grid_gpu_session.h"
#include "brush/stroke_driver.h"
#include "brush/stroke_spacing.h"
#include "displace/compositor.h"
#include "displace/frames.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/attr_weights.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh_serialize.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/closest_point.h"
#include "mesh/utils/mesh_validate.h"
#include "mesh/utils/triangulate.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/curvature.h"
#include "remesh/field/feature_tag.h"
#include "remesh/field/singularity_adjust.h"
#include "remesh/param/seamless_param.h"
#include "remesh/quantize/quantize_ilp.h"
#include "remesh/remesh.h"
#include "remesh/remesh_params.h"
#include "spatial/spatial.h"
#include "stb/stb_image.h"
#include "vdm/vdm_promote.h"
#include "vdm/vdm_splat.h"
#include "vdm/vdm_undo.h"
#ifdef SBRUSH_WEBGPU_COMPUTE
#include "webgpu/wgpu_compute.h"
#include "webgpu/wgpu_context.h"
#endif
#ifdef SBRUSH_GPU_DISPATCH
#include "vulkan/vk_compute.h"
#include "vulkan/vk_context.h"
#endif
#include "subdiv/grid_domain.h"
#include "subdiv/grid_stroke_log.h"
#include "subdiv/grid_tree.h"
#include "subdiv/grids.h"
#include "subdiv/multires.h"

#ifdef SBRUSH_GPU_DISPATCH
#include "gpu_stroke.h"
#endif

#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "script/script_util.h"

namespace sculptcore::debug_app::script {

bool execSetupVerb(Scene &scene,
                   const std::string &verb,
                   ArgMap &args,
                   const char *out_dir,
                   std::string &err,
                   bool &handled);
bool execVdmVerb(Scene &scene,
                 const std::string &verb,
                 ArgMap &args,
                 const char *out_dir,
                 std::string &err,
                 bool &handled);
bool execStrokeVerb(Scene &scene,
                    const std::string &verb,
                    ArgMap &args,
                    const char *out_dir,
                    std::string &err,
                    bool &handled);
bool execMultiresVerb(Scene &scene,
                      const std::string &verb,
                      ArgMap &args,
                      const char *out_dir,
                      std::string &err,
                      bool &handled);
bool execBenchVerb(Scene &scene,
                   const std::string &verb,
                   ArgMap &args,
                   const char *out_dir,
                   std::string &err,
                   bool &handled);
bool execRemeshVerb(Scene &scene,
                    const std::string &verb,
                    ArgMap &args,
                    const char *out_dir,
                    std::string &err,
                    bool &handled);

namespace {

/* Dispatches a parsed verb to its group handler in file order (setup, vdm,
 * stroke, multires, bench, remesh — matching the original single-function
 * if-chain's layout). Each group handler sets `handled` to report whether the
 * verb was one of its own; execVerb stops at the first group that claims it. */
bool execVerb(Scene &scene,
              const std::string &verb,
              ArgMap &args,
              const char *out_dir,
              std::string &err)
{
  bool handled = false;
  bool ok = false;

  ok = execSetupVerb(scene, verb, args, out_dir, err, handled);
  if (handled) {
    return ok;
  }
  ok = execVdmVerb(scene, verb, args, out_dir, err, handled);
  if (handled) {
    return ok;
  }
  ok = execStrokeVerb(scene, verb, args, out_dir, err, handled);
  if (handled) {
    return ok;
  }
  ok = execMultiresVerb(scene, verb, args, out_dir, err, handled);
  if (handled) {
    return ok;
  }
  ok = execBenchVerb(scene, verb, args, out_dir, err, handled);
  if (handled) {
    return ok;
  }
  ok = execRemeshVerb(scene, verb, args, out_dir, err, handled);
  if (handled) {
    return ok;
  }

  err = "unknown verb: " + verb;
  return false;
}

} // namespace

RunResult run(Scene &scene, const char *source, const char *out_dir)
{
  RunResult r;
  if (!source) {
    r.ok = false;
    r.error = "null source";
    return r;
  }
  const char *p = source;
  int line_no = 0;
  while (*p) {
    line_no++;
    const char *eol = std::strchr(p, '\n');
    size_t len = eol ? size_t(eol - p) : std::strlen(p);
    std::string line(p, len);

    std::string verb;
    ArgMap args;
    if (parseLine(line, verb, args)) {
      std::string err;
      if (!execVerb(scene, verb, args, out_dir, err)) {
        r.ok = false;
        r.line_no = line_no;
        r.error = err.c_str();
        return r;
      }
    }

    if (!eol) {
      break;
    }
    p = eol + 1;
  }
  return r;
}

RunResult runFile(Scene &scene, const char *path, const char *out_dir)
{
  RunResult r;
  std::FILE *f = std::fopen(path, "rb");
  if (!f) {
    r.ok = false;
    std::string e = std::string("cannot open script: ") + path;
    r.error = e.c_str();
    return r;
  }
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::string buf;
  if (n > 0) {
    buf.resize(size_t(n));
    size_t got = std::fread(buf.data(), 1, size_t(n), f);
    buf.resize(got);
  }
  std::fclose(f);
  return run(scene, buf.c_str(), out_dir);
}

} // namespace sculptcore::debug_app::script
