#include "brush/semantic_scalars.h"
#include "test_util.h"
#include <array>
#include <limits>

test_init;
using namespace sculptcore;

int main()
{
  auto *collection = SemanticScalars_create();
  test_assert(SemanticScalars_add(collection, int(props::Prop::FLOAT32), .25, 0, 1, 1) ==
              0);
  test_assert(SemanticScalars_add(collection, int(props::Prop::INT32), 11, 1, 1000, 1) ==
              1);
  test_assert(SemanticScalars_add(collection, int(props::Prop::BOOL), 1, 0, 1, 1) == 2);
  test_assert(SemanticScalars_add(collection, int(props::Prop::FLOAT32), 0, 0, 100, 1) ==
              3);
  test_assert(SemanticScalars_add(collection, int(props::Prop::FLOAT32), .5, 0, 1, 0) ==
              4);
  test_assert(SemanticScalars_add(collection, int(props::Prop::INT32), 1.5, 0, 100, 1) <
              0);
  test_assert(SemanticScalars_add(collection, int(props::Prop::FLOAT64), .5, 0, 1, 1) <
              0);
  props::Dynamics dynamics;
  test_assert(
      dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1));
  for (int index = 0; index < 4; index++)
    test_assert(collection->replace(index, dynamics) == 0);
  test_assert(collection->replace(4, dynamics) != 0);
  test_assert(collection->replace(-1, dynamics) != 0);
  float inputs[] = {.5, 0, 0, 0, 1, .25, 0, 0, 0, 0, .499f, 0, 0, 0, 1};
  float radii[] = {2, 4, 6};
  std::array<double, 15> output;
  output.fill(-999);
  test_assert(SemanticScalars_evaluate(
                  collection, 3, inputs, radii, 3, output.data(), output.size()) == 0);
  test_assert(output[0] == .125 && output[1] == 6 && output[2] == 1 && output[3] == 1 &&
              output[4] == .5);
  test_assert(output[5] == .25 && output[6] == 11 && output[7] == 1 && output[8] == 4);
  test_assert(output[12] == 0);
  auto previous = output;
  inputs[14] = 16;
  test_assert(SemanticScalars_evaluate(
                  collection, 3, inputs, radii, 3, output.data(), output.size()) != 0);
  test_assert(output == previous);
  inputs[14] = 1;
  radii[2] = std::numeric_limits<float>::quiet_NaN();
  test_assert(SemanticScalars_evaluate(
                  collection, 3, inputs, radii, 3, output.data(), output.size()) != 0);
  test_assert(output == previous);
  test_assert(SemanticScalars_evaluate(
                  collection, 3, inputs, radii, 1, output.data(), output.size()) != 0);
  test_assert(SemanticScalars_evaluate(
                  collection, 3, inputs, radii, 3, output.data(), output.size() - 1) !=
              0);
  test_assert(SemanticScalars_evaluate(collection, 0, nullptr, nullptr, 3, nullptr, 0) ==
              0);
  SemanticScalars_free(collection);
  fprintf(stderr,
          "semantic typed domains, input presence and atomic evaluation passed\n");
  return test_end();
}
