#pragma once

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <map>
#include <string>
#include <vector>

namespace sculptcore::debug_app {

struct Scene;

namespace script {

using ArgMap = std::map<std::string, std::string>;

std::string trim(const std::string &s);
bool parseLine(const std::string &line, std::string &verb, ArgMap &args);
const char *getArg(ArgMap &args, const char *key, const char *defv = nullptr);
int getInt(ArgMap &args, const char *key, int defv);
float getFloat(ArgMap &args, const char *key, float defv);
bool getBool(ArgMap &args, const char *key, bool defv);
/* Parse "a,b,c" into ints; returns defv when the arg is absent/empty. */
std::vector<int> parseCsvInts(const char *s, std::vector<int> defv);
bool parseFloat2(const char *s, litestl::math::float2 &out);
bool parseFloat3(const char *s, litestl::math::float3 &out);
std::string joinPath(const char *base, const char *rel);

/** Print the brush-noise metrics over the region swept by `centers`/`radius`
 * (plan 2026-07-26-0909 par 9.1): one-ring normal roughness of the live surface
 * *and* of the derived displacement base, plus the live-only fidelity guard so
 * a quieter base can't be won by depositing less displacement. */
void reportRoughness(Scene &scene,
                     const char *tag,
                     const litestl::util::Vector<litestl::math::float3> &centers,
                     float radius,
                     litestl::math::float3 up,
                     float rest);

} // namespace script
} // namespace sculptcore::debug_app
