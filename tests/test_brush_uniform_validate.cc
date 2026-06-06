// Wave 4 gate for codegen-driven dynamic sbrush uniforms: the per-stroke
// `validateUniformDynamics()` pass. Scoped to the active brush's manifest, it
// rejects the shared-`Brush`-struct traps (a stray dynamic from another kernel,
// a dynamic on a `@static` uniform), an unbaked 1-entry response curve, an
// out-of-range authored default, and an inverted `@range` — and passes the happy
// path. The integration tail proves a failing validation skips the stroke
// without mutating the mesh.
#include "test_util.h"

#include "brush/brush_executor.h"
#include "debug/scene.h"
#include "debug/script.h"
#include "mesh/mesh.h"

#include <cstdio>

test_init;

using namespace sculptcore::brush;
using namespace sculptcore::debug_app;
using sculptcore::mesh::Mesh;
using litestl::math::float3;

static bool msgContains(const UniformValidationResult &r, const char *needle)
{
  for (const auto &m : r.messages) {
    if (strstr(m.c_str(), needle) != nullptr) return true;
  }
  return false;
}

static const int PRESSURE = (int)sculptcore::props::DeviceType::PRESSURE; // 0
static const int MULTIPLY = (int)litestl::math::BasicMix::MULTIPLY;       // 1

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // --- happy path: a valid pressure dynamic on kelvinlet mu validates OK ---
  {
    Brush brush;
    CommandExecutor exec(/*tree=*/nullptr, &brush);
    auto cmd = exec.createCommand(SculptBrushes::KELVINLET);
    cmd.registerProps(*brush.props.struct_def);

    brush.addPropDynamicByName("mu", PRESSURE, MULTIPLY, 1.0f);
    for (int j = 0; j < 4; j++) {
      brush.setPropDynamicSampleByName("mu", PRESSURE, j, 4, float(j) / 3.0f);
    }

    auto res = exec.validateUniformDynamics(cmd);
    fprintf(stderr, "happy: ok=%d msgs=%d\n", res.ok, int(res.messages.size()));
    test_assert(res.ok);
    test_assert(res.messages.size() == 0);
  }

  // --- stray dynamic: mu bound, but active brush (DRAW) has no mu uniform ---
  {
    Brush brush;
    CommandExecutor exec(/*tree=*/nullptr, &brush);
    // Register kelvinlet's mu onto the shared props, attach a dynamic...
    auto kcmd = exec.createCommand(SculptBrushes::KELVINLET);
    kcmd.registerProps(*brush.props.struct_def);
    brush.addPropDynamicByName("mu", PRESSURE, MULTIPLY, 1.0f); // empty curve = identity

    // ...then validate against DRAW, whose manifest does not declare mu.
    auto dcmd = exec.createCommand(SculptBrushes::DRAW);
    auto res = exec.validateUniformDynamics(dcmd);
    fprintf(stderr, "stray: ok=%d contains=%d\n", res.ok,
            msgContains(res, "stray dynamic"));
    test_assert(!res.ok);
    test_assert(msgContains(res, "stray dynamic"));
  }

  // --- @static target: a dynamic on wingscrape's @static wingAngle uniform ---
  {
    Brush brush;
    CommandExecutor exec(/*tree=*/nullptr, &brush);
    auto cmd = exec.createCommand(SculptBrushes::WINGSCRAPE);
    // wingAngle is @static so registerProps skips it; register it by hand to
    // construct the bad state (a dynamic on a non-dynamic-capable uniform).
    brush.props.struct_def->Float32("wingAngle", "wingAngle");
    brush.addPropDynamicByName("wingAngle", PRESSURE, MULTIPLY, 1.0f);

    auto res = exec.validateUniformDynamics(cmd);
    fprintf(stderr, "static: ok=%d contains=%d\n", res.ok,
            msgContains(res, "@static"));
    test_assert(!res.ok);
    test_assert(msgContains(res, "@static"));
  }

  // --- unbaked curve: a device response curve with exactly 1 entry ---
  {
    Brush brush;
    CommandExecutor exec(/*tree=*/nullptr, &brush);
    auto cmd = exec.createCommand(SculptBrushes::KELVINLET);
    cmd.registerProps(*brush.props.struct_def);
    brush.addPropDynamicByName("mu", PRESSURE, MULTIPLY, 1.0f);
    brush.setPropDynamicSampleByName("mu", PRESSURE, 0, 1, 0.5f); // 1-entry table

    auto res = exec.validateUniformDynamics(cmd);
    fprintf(stderr, "unbaked: ok=%d contains=%d\n", res.ok,
            msgContains(res, "1 entry"));
    test_assert(!res.ok);
    test_assert(msgContains(res, "1 entry"));
  }

  // --- out-of-range default: a manifest entry whose default escapes its range ---
  {
    Brush brush;
    CommandExecutor exec(/*tree=*/nullptr, &brush);
    auto cmd = exec.createCommand(SculptBrushes::KELVINLET);
    // def=5 outside [0,1] (BrushUniformManifestEntry: name,isFloat,dynamic,def,
    // hasRange,rangeMin,rangeMax).
    cmd.uniforms.append(
        BrushUniformManifestEntry{"badDef", true, true, 5.0f, true, 0.0f, 1.0f});

    auto res = exec.validateUniformDynamics(cmd);
    fprintf(stderr, "oorange: ok=%d contains=%d\n", res.ok,
            msgContains(res, "outside @range"));
    test_assert(!res.ok);
    test_assert(msgContains(res, "outside @range"));
  }

  // --- inverted range: rangeMin > rangeMax ---
  {
    Brush brush;
    CommandExecutor exec(/*tree=*/nullptr, &brush);
    auto cmd = exec.createCommand(SculptBrushes::KELVINLET);
    cmd.uniforms.append(
        BrushUniformManifestEntry{"badRange", true, true, 0.5f, true, 1.0f, 0.0f});

    auto res = exec.validateUniformDynamics(cmd);
    fprintf(stderr, "inverted: ok=%d contains=%d\n", res.ok,
            msgContains(res, "invalid @range"));
    test_assert(!res.ok);
    test_assert(msgContains(res, "invalid @range"));
  }

  // --- Wave 5: the by-index manifest surface the TS bridge drives ---
  {
    Brush brush;
    CommandExecutor exec(/*tree=*/nullptr, &brush);

    // queryUniformManifest caches the active kernel's manifest and registers its
    // props; kelvinlet declares mu, nu, radius.
    int n = exec.queryUniformManifest((int)SculptBrushes::KELVINLET);
    fprintf(stderr, "wave5: count=%d (expect 3)\n", n);
    test_assert(n == 3);

    // Entries are exposed by pointer; the bridge reads name/range/dynamic.
    auto *e0 = exec.queriedUniformEntry(0);
    test_assert(e0 != nullptr);
    test_assert(e0->name == string("mu"));
    test_assert(e0->isFloat && e0->dynamic && e0->hasRange);
    test_assert(exec.queriedUniformEntry(99) == nullptr); // out of range -> null

    // Driving dynamics by index resolves to the by-name API: configure mu, bake
    // a 4-entry curve, and validation passes.
    exec.addUniformDynamic(0, PRESSURE, MULTIPLY, 1.0f);
    for (int j = 0; j < 4; j++) {
      exec.setUniformDynamicSample(0, PRESSURE, j, 4, float(j) / 3.0f);
    }
    auto kcmd = exec.createCommand(SculptBrushes::KELVINLET);
    auto res = exec.validateUniformDynamics(kcmd);
    fprintf(stderr, "wave5: configured ok=%d\n", res.ok);
    test_assert(res.ok);
    sculptcore::props::Dynamics *dyn = brush.propDynamics(string("mu"));
    test_assert(dyn && dyn->devices.size() == 1);

    // clearUniformDynamics by index drops the stack again.
    exec.clearUniformDynamics(0);
    dyn = brush.propDynamics(string("mu"));
    fprintf(stderr, "wave5: cleared devices=%d (expect 0)\n",
            dyn ? int(dyn->devices.size()) : -1);
    test_assert(dyn && dyn->devices.size() == 0);

    // Out-of-range index is a silent no-op (doesn't touch any prop).
    exec.addUniformDynamic(99, PRESSURE, MULTIPLY, 1.0f);
    test_assert(brush.propDynamics(string("mu"))->devices.size() == 0);
  }

  // --- integration: a failing validation must not mutate the mesh ---
  {
    Scene scene(256, 256, /*headless=*/true);
    auto r = script::run(scene,
                         "make_cube subdivs=12 size=0.5\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_brush_tool tool=draw\n"
                         "set_brush radius=0.25 strength=1.0\n"
                         "set_backend backend=cpp\n",
                         ".");
    test_assert(r.ok);
    Mesh *m = scene.mesh;
    test_assert(m != nullptr);

    // Attach a stray kelvinlet-mu dynamic to the shared brush (mu ∉ DRAW's
    // manifest) — validation must reject it before the first dab.
    CommandExecutor tmp(scene.tree, &scene.brush);
    auto kcmd = tmp.createCommand(SculptBrushes::KELVINLET);
    kcmd.registerProps(*scene.brush.props.struct_def);
    scene.brush.addPropDynamicByName("mu", PRESSURE, MULTIPLY, 1.0f);

    Vector<float3> before;
    for (int i = 0; i < m->v.count; i++) before.append(m->v.co[i]);

    r = script::run(scene, "stroke origin=0,0,0.25 normal=0,0,1\n", ".");
    test_assert(r.ok); // the verb runs; the brush no-ops on failed validation

    int changed = 0;
    for (int i = 0; i < m->v.count; i++) {
      float3 a = m->v.co[i], b = before[i];
      if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) changed++;
    }
    fprintf(stderr, "integration-skip: changed=%d (expect 0)\n", changed);
    test_assert(changed == 0);
  }

  // --- control: the same DRAW stroke with no bad dynamic DOES mutate ---
  {
    Scene scene(256, 256, /*headless=*/true);
    auto r = script::run(scene,
                         "make_cube subdivs=12 size=0.5\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_brush_tool tool=draw\n"
                         "set_brush radius=0.25 strength=1.0\n"
                         "set_backend backend=cpp\n",
                         ".");
    test_assert(r.ok);
    Mesh *m = scene.mesh;

    Vector<float3> before;
    for (int i = 0; i < m->v.count; i++) before.append(m->v.co[i]);

    r = script::run(scene, "stroke origin=0,0,0.25 normal=0,0,1\n", ".");
    test_assert(r.ok);

    int changed = 0;
    for (int i = 0; i < m->v.count; i++) {
      float3 a = m->v.co[i], b = before[i];
      if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) changed++;
    }
    fprintf(stderr, "integration-mutate: changed=%d (expect >0)\n", changed);
    test_assert(changed > 0);
  }

  return test_end();
}
