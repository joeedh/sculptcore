
#include "test_util.h"
#include <cstdio>
#include <cstring>

#include "binding/binding.h"
#include "brush/props.h"
#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"

test_init;

int main()
{
  using namespace sculptcore::brush;
  using namespace litestl::binding;
  using namespace sculptcore::mesh;
  using namespace litestl::util;

#if 1
  {
    static_assert(validate_prop("strength"));
    static_assert(!validate_prop("sdfsf"));

    const BindingBase *binding = Bind<Mesh>();
    Vector<const BindingBase *> types = {binding};

    auto result = generators::generateTypescript(types);
    for (auto &key : result->keys()) {
      printf("\n======= %s =======\n", key.c_str());
      printf(result->lookup(key).c_str());
    }
    
  }
#endif

  return test_end();
}
