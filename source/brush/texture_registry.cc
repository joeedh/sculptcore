#include "texture_registry.h"

#include "sculptcore_textures.gen.h"

#include <cstring>

namespace sculptcore::brush {

int textureRegistryCount()
{
  return command::kTextureRegistryCount;
}

const TextureRegistryEntry *textureRegistryEntry(int i)
{
  if (i < 0 || i >= command::kTextureRegistryCount) {
    return nullptr;
  }
  return &command::kTextureRegistry[i];
}

const TextureRegistryEntry *findTexture(litestl::util::stringref name)
{
  for (int i = 0; i < command::kTextureRegistryCount; i++) {
    if (std::strcmp(command::kTextureRegistry[i].name, name.c_str()) == 0) {
      return &command::kTextureRegistry[i];
    }
  }
  return nullptr;
}

} // namespace sculptcore::brush
