// Wave 3 gate for codegen-driven dynamic sbrush uniforms: a per-kernel scalar
// uniform (kelvinlet `mu`) must be drivable by a name-keyed device dynamic.
// Configures a pressure dynamic on `mu`, pushes device samples across a
// synthetic stroke, and asserts the resolved per-dab `brush.mu` follows the
// baked response curve. Also asserts the no-device path is bit-identical to the
// plain static prop read (dynamics must be a perfect no-op when unconfigured).
//
// Drives registerProps / loadUniformProps directly off a BrushCommandDef; no
// mesh/stroke needed (createCommand only fills function pointers).
#include "test_util.h"

#include "brush/brush_executor.h"

#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore::brush;
namespace props = sculptcore::props;

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  constexpr int PRESSURE = (int)props::DeviceType::PRESSURE; // 0
  constexpr int MULTIPLY = (int)litestl::math::BasicMix::MULTIPLY; // 1

  // 5-entry response curve. Its sample points sit at pressure j/4 (j=0..4), so a
  // device value landing exactly on one resolves to curve[j] with no
  // interpolation — the per-dab assertion is then exact, not approximate.
  const int N = 5;
  const float curve[N] = {0.10f, 0.30f, 0.45f, 0.80f, 1.00f};

  // --- pressure dynamic on mu: resolved mu follows stored_mu * curve ---
  {
    Brush brush;
    CommandExecutor exec(/*tree=*/nullptr, &brush);
    auto cmd = exec.createCommand(SculptBrushes::KELVINLET);
    cmd.registerProps(*brush.props.struct_def);
    test_assert(brush.props.struct_def->has("mu"));

    // Store a non-default base value so the test exercises both the stored prop
    // and the curve (MULTIPLY: resolved = stored * deviceFactor).
    const float stored_mu = 2.0f;
    brush.props.setFloat("mu", stored_mu);

    // Configure the pressure dynamic by NAME (the Wave 3 surface) and bake the
    // curve sample-by-sample.
    brush.clearPropDynamicsByName("mu");
    brush.addPropDynamicByName("mu", PRESSURE, MULTIPLY, 1.0f);
    for (int j = 0; j < N; j++) {
      brush.setPropDynamicSampleByName("mu", PRESSURE, j, N, curve[j]);
    }

    // Synthetic stroke: one dab per curve knot. Push the pressure sample, then
    // resolve the kernel uniforms (applies the dynamic via deviceInputCtx).
    for (int j = 0; j < N; j++) {
      float pressure = float(j) / float(N - 1);
      brush.clearDeviceInputs();
      brush.pushDeviceInput(PRESSURE, pressure);
      cmd.loadUniformProps(brush, &brush.deviceInputCtx);

      float expected = stored_mu * curve[j];
      fprintf(stderr, "dab %d: pressure=%g mu=%g expected=%g\n", j, pressure,
              brush.mu, expected);
      test_assert(std::fabs(brush.mu - expected) < 1e-6f);
    }
  }

  // --- no device configured: load is bit-identical to the static prop read ---
  {
    Brush brush;
    CommandExecutor exec(/*tree=*/nullptr, &brush);
    auto cmd = exec.createCommand(SculptBrushes::KELVINLET);
    cmd.registerProps(*brush.props.struct_def);

    const float stored_mu = 2.0f;
    brush.props.setFloat("mu", stored_mu);

    // No addPropDynamic — but push a device sample anyway. With no dynamic
    // layer configured it must be ignored entirely.
    brush.clearDeviceInputs();
    brush.pushDeviceInput(PRESSURE, 0.5f);
    cmd.loadUniformProps(brush, &brush.deviceInputCtx);
    float dynamic_path = brush.mu;

    // The static path: same prop, read with no device ctx.
    float static_path = brush.props.lookupValue<float>("mu", -999.0f);

    fprintf(stderr, "no-device: dynamic_path=%g static_path=%g\n", dynamic_path,
            static_path);
    test_assert(std::memcmp(&dynamic_path, &static_path, sizeof(float)) == 0);
    test_assert(dynamic_path == stored_mu);
  }

  return test_end();
}
