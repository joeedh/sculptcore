#include "test_util.h"
#include "litestl/util/map.h"
#include "litestl/util/rand.h"
#include "litestl/util/vector.h"
#include <cstdio>
#include "props/prop_types.h"
#include "props/prop_struct.h"

test_init;

int main()
{
  using namespace litestl::util;
  using namespace sculptcore::props;

  struct Test {
    float f;
    int i;
  };

  {
    Float32Prop prop;

    prop.Name("bleh").Min(0.0f).Max(1.0f);

    StructDef st;
    st.Float32("f", "f", offsetof(Test, f));

    Test test;
    test.f = 3;

    StructProp sprop(&st);
    sprop.Owner(&test);

    printf("f value: %f\n", sprop.lookupFloat("f", 0.5f));
  }

  return test_end();
}
