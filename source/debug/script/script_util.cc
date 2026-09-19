#include "script_util.h"

#include "../roughness.h"
#include "../scene.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sculptcore::debug_app::script {

using litestl::math::float3;
using litestl::util::Vector;

std::string trim(const std::string &s)
{
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
    a++;
  }
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
    b--;
  }
  return s.substr(a, b - a);
}

bool parseLine(const std::string &line, std::string &verb, ArgMap &args)
{
  std::string t = trim(line);
  if (t.empty() || t[0] == '#') {
    return false;
  }
  size_t n = t.size(), i = 0;
  while (i < n && !std::isspace(static_cast<unsigned char>(t[i]))) {
    i++;
  }
  verb = t.substr(0, i);

  while (i < n) {
    while (i < n && std::isspace(static_cast<unsigned char>(t[i]))) {
      i++;
    }
    if (i >= n) {
      break;
    }
    size_t key_start = i;
    while (i < n && t[i] != '=' && !std::isspace(static_cast<unsigned char>(t[i]))) {
      i++;
    }
    std::string key = t.substr(key_start, i - key_start);
    std::string val;
    if (i < n && t[i] == '=') {
      i++;
      size_t val_start = i;
      while (i < n && !std::isspace(static_cast<unsigned char>(t[i]))) {
        i++;
      }
      val = t.substr(val_start, i - val_start);
    }
    args[key] = val;
  }
  return true;
}

const char *getArg(ArgMap &args, const char *key, const char *defv)
{
  auto it = args.find(key);
  return it == args.end() ? defv : it->second.c_str();
}

int getInt(ArgMap &args, const char *key, int defv)
{
  const char *s = getArg(args, key);
  return s ? std::atoi(s) : defv;
}

float getFloat(ArgMap &args, const char *key, float defv)
{
  const char *s = getArg(args, key);
  return s ? float(std::atof(s)) : defv;
}

bool getBool(ArgMap &args, const char *key, bool defv)
{
  const char *s = getArg(args, key);
  if (!s) {
    return defv;
  }
  return s[0] == '1' || s[0] == 't' || s[0] == 'T' || s[0] == 'y' || s[0] == 'Y';
}

/* Parse "a,b,c" into ints; returns defv when the arg is absent/empty. */
std::vector<int> parseCsvInts(const char *s, std::vector<int> defv)
{
  if (!s || !s[0]) {
    return defv;
  }
  std::vector<int> out;
  for (const char *p = s; *p;) {
    out.push_back(std::atoi(p));
    while (*p && *p != ',') {
      p++;
    }
    if (*p == ',') {
      p++;
    }
  }
  return out.empty() ? defv : out;
}

bool parseFloat2(const char *s, litestl::math::float2 &out)
{
  if (!s) {
    return false;
  }
  float a = 0, b = 0;
  if (std::sscanf(s, "%f,%f", &a, &b) != 2) {
    return false;
  }
  out = litestl::math::float2(a, b);
  return true;
}

bool parseFloat3(const char *s, float3 &out)
{
  if (!s) {
    return false;
  }
  float a = 0, b = 0, c = 0;
  if (std::sscanf(s, "%f,%f,%f", &a, &b, &c) != 3) {
    return false;
  }
  out = float3(a, b, c);
  return true;
}

std::string joinPath(const char *base, const char *rel)
{
  if (!rel) {
    return std::string();
  }
  if (!base || base[0] == 0) {
    return std::string(rel);
  }
  if (rel[0] == '/' || rel[0] == '\\' || (std::strlen(rel) > 1 && rel[1] == ':')) {
    return std::string(rel);
  }
  std::string out(base);
  if (!out.empty()) {
    char last = out.back();
    if (last != '/' && last != '\\') {
      out += '/';
    }
  }
  out += rel;
  return out;
}

/** Print the brush-noise metrics over the region swept by `centers`/`radius`
 * (plan 2026-07-26-0909 §9.1): one-ring normal roughness of the live surface
 * *and* of the derived displacement base, plus the live-only fidelity guard so
 * a quieter base can't be won by depositing less displacement. */
static void reportRoughness(Scene &scene,
                            const char *tag,
                            const Vector<float3> &centers,
                            float radius,
                            float3 up,
                            float rest)
{
  Vector<int> region;
  collectRegion(scene.mesh, centers, radius, region);
  RoughnessResult live = computeRoughness(
      scene.mesh, region, RoughnessPoints::Live, scene.strokeGen, up, rest);
  RoughnessResult base = computeRoughness(
      scene.mesh, region, RoughnessPoints::Base, scene.strokeGen, up, rest);
  std::fprintf(stdout,
               "[roughness] %s verts=%d edges=%d | live rms=%.6g p95=%.6g max=%.6g "
               "dih=%.6g | base rms=%.6g p95=%.6g max=%.6g dih=%.6g | maxdisp=%.6g "
               "vol=%.6g\n",
               tag,
               live.verts,
               live.edges,
               live.rms,
               live.p95,
               live.maxr,
               live.dihedral,
               base.rms,
               base.p95,
               base.maxr,
               base.dihedral,
               live.maxDisp,
               live.volume);
  std::fflush(stdout);
}

} // namespace sculptcore::debug_app::script
