// Wave 2 gate for codegen-driven dynamic sbrush uniforms: the per-kernel
// scalar-float `uniform`s must register as props (with their authored defaults)
// and load back into the cached Brush members — replacing the old hand-written
// structDef_/loadProps lists. Also exercises the `@static` opt-out: a host-set
// uniform (plane `planeSide`) must NOT be registered or overwritten by load.
//
// Drives the generated `registerProps` / `loadUniformProps` slots directly off a
// BrushCommandDef (no mesh/stroke needed); createCommand only fills function
// pointers, so a null SpatialTree is fine.
#include "test_util.h"

#include "brush/brush_executor.h"

#include <cstdio>

test_init;

using namespace sculptcore::brush;

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // --- kelvinlet: mu/nu register with defaults, then load into members ---
  {
    Brush brush;
    CommandExecutor exec(/*tree=*/nullptr, &brush);
    auto cmd = exec.createCommand(SculptBrushes::KELVINLET);

    test_assert(cmd.registerProps != nullptr);
    test_assert(cmd.loadUniformProps != nullptr);
    test_assert(brush.props.struct_def != nullptr);

    // mu/nu are no longer registered by the Brush ctor — they belong to the
    // active kernel's generated wiring.
    test_assert(!brush.props.struct_def->has("mu"));
    test_assert(!brush.props.struct_def->has("nu"));

    cmd.registerProps(*brush.props.struct_def);
    test_assert(brush.props.struct_def->has("mu"));
    test_assert(brush.props.struct_def->has("nu"));

    // Registration seeds the authored defaults (kelvinlet.sbrush: mu=1.0,
    // nu=0.4). lookupValue returns the stored value when the prop exists, so the
    // -999 fallback is only returned if seeding failed.
    float mu_def = brush.props.lookupValue<float>("mu", -999.0f);
    float nu_def = brush.props.lookupValue<float>("nu", -999.0f);
    test_assert(mu_def == 1.0f);
    test_assert(nu_def == 0.4f);

    // loadUniformProps writes the resolved prop value into the cached members
    // the kernel reads. Sentinel first to prove it actually loads.
    brush.mu = -1.0f;
    brush.nu = -1.0f;
    cmd.loadUniformProps(brush, &brush.deviceInputCtx);
    test_assert(brush.mu == 1.0f);
    test_assert(brush.nu == 0.4f);

    fprintf(stderr, "kelvinlet: mu=%g nu=%g\n", brush.mu, brush.nu);
  }

  // --- plane @static opt-out: planeSide is host-set, never a prop ---
  {
    Brush brush;
    CommandExecutor exec(/*tree=*/nullptr, &brush);
    // CLAY shares the plane kernel.
    auto cmd = exec.createCommand(SculptBrushes::CLAY);

    cmd.registerProps(*brush.props.struct_def);
    // planeoff is a dynamic uniform; planeSide is @static and must be excluded.
    test_assert(brush.props.struct_def->has("planeoff"));
    test_assert(!brush.props.struct_def->has("planeSide"));

    // load must not clobber the host-set planeSide member.
    brush.planeSide = 7.0f;
    cmd.loadUniformProps(brush, &brush.deviceInputCtx);
    test_assert(brush.planeSide == 7.0f);

    fprintf(stderr,
            "plane: planeSide=%g (untouched), planeoff registered\n",
            brush.planeSide);
  }

  return test_end();
}
