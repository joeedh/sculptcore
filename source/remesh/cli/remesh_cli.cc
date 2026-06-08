// Standalone quad-remesh CLI: load one OBJ, run QuadRemesh, write the result
// mesh + a JSON manifest to an output dir, and stream coarse progress + result
// lines to stdout for a parent process (the interactive debug app) to read.
//
// Protocol (one record per line, stdout, unbuffered):
//   PROGRESS <pct> <stage>      monotonic 0..100; stages copy..done, or "failed"
//   RESULT <obj-path>           written quad mesh
//   MANIFEST <json-path>        written manifest
//   STATS k=v k=v ...           summary (verts/quads/tris/manifold/euler/spiral/ms)
//   ERROR <message>             fatal (also exit != 0)
//
// It is a fresh process per run, so it does no global init beyond what the remesh
// libs do statically (mirrors tests/test_remesh_extract.cc, which also just calls
// QuadRemesh directly).

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"

#include "mesh/mesh.h"
#include "mesh/utils/mesh_validate.h"
#include "mesh/utils/obj_io.h"

#include "remesh/remesh.h"
#include "remesh/remesh_params.h"

#include "remesh_cli_config.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

using namespace sculptcore;
using litestl::math::float3;

namespace {

namespace fs = std::filesystem;

void progressCb(void * /*user*/, int pct, const char *stage)
{
  std::printf("PROGRESS %d %s\n", pct, stage);
}

// "YYYYMMDD-HHMMSS" local time. A fresh CLI process; no determinism concern.
std::string timestamp()
{
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
  return buf;
}

// Basename with no extension, e.g. ".../AnimeGirl2.obj" -> "AnimeGirl2".
std::string stem(const std::string &path)
{
  return fs::path(path).stem().string();
}

// Manual min/max AABB — mesh::calcAABB seeds max to FLT_MIN and mis-handles
// all-negative meshes, so compute it explicitly here.
void inputAABB(mesh::Mesh &m, float3 &bmin, float3 &bmax)
{
  bool have = false;
  for (int v : m.v) {
    float3 co = m.v.co[v];
    if (!have) {
      bmin = bmax = co;
      have = true;
      continue;
    }
    for (int i = 0; i < 3; i++) {
      if (co[i] < bmin[i])
        bmin[i] = co[i];
      if (co[i] > bmax[i])
        bmax[i] = co[i];
    }
  }
  if (!have)
    bmin = bmax = float3(0, 0, 0);
}

const char *jb(bool b) { return b ? "true" : "false"; }

// Escape a string for embedding in a JSON double-quoted value. Critically this
// turns Windows path backslashes into `\\` (a bare `\s` is an invalid JSON
// escape that breaks strict parsers like JS JSON.parse).
std::string jstr(const std::string &s)
{
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '\\': o += "\\\\"; break;
    case '"': o += "\\\""; break;
    case '\n': o += "\\n"; break;
    case '\r': o += "\\r"; break;
    case '\t': o += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        o += buf;
      } else {
        o += c;
      }
    }
  }
  return o;
}

bool writeManifest(const char *path, const std::string &jsonName,
                   const std::string &objPath, const std::string &inName,
                   const std::string &inPath, int inVerts, int inFaces,
                   const float3 &amin, const float3 &amax,
                   const remesh::RemeshParams &p, mesh::Mesh &out,
                   const mesh::RemeshReport &r, long long durationMs)
{
  std::FILE *f = std::fopen(path, "wb");
  if (!f)
    return false;

  auto f3 = [&](const float3 &v) {
    std::fprintf(f, "[%.9g, %.9g, %.9g]", v[0], v[1], v[2]);
  };

  std::fprintf(f, "{\n");
  std::fprintf(f, "  \"schema\": \"remesh-manifest/1\",\n");
  std::fprintf(f, "  \"timestamp\": \"%s\",\n", jstr(jsonName).c_str());
  std::fprintf(f, "  \"git_commit\": \"%s\",\n", jstr(REMESH_CLI_GIT_COMMIT).c_str());
  std::fprintf(f, "  \"duration_ms\": %lld,\n", durationMs);

  std::fprintf(f, "  \"input\": {\n");
  std::fprintf(f, "    \"name\": \"%s\",\n", jstr(inName).c_str());
  std::fprintf(f, "    \"path\": \"%s\",\n", jstr(inPath).c_str());
  std::fprintf(f, "    \"verts\": %d,\n", inVerts);
  std::fprintf(f, "    \"faces\": %d,\n", inFaces);
  std::fprintf(f, "    \"aabb_min\": ");
  f3(amin);
  std::fprintf(f, ",\n    \"aabb_max\": ");
  f3(amax);
  std::fprintf(f, "\n  },\n");

  std::fprintf(f, "  \"params\": {\n");
  std::fprintf(f, "    \"target_edge_length\": %.9g,\n", p.target_edge_length);
  std::fprintf(f, "    \"solve_edge_length\": %.9g,\n", p.solve_edge_length);
  std::fprintf(f, "    \"use_curvature\": %s,\n", jb(p.use_curvature));
  std::fprintf(f, "    \"use_sharp_features\": %s,\n", jb(p.use_sharp_features));
  std::fprintf(f, "    \"sharp_angle\": %.9g,\n", p.sharp_angle);
  std::fprintf(f, "    \"use_density\": %s,\n", jb(p.use_density));
  std::fprintf(f, "    \"reproject\": %s,\n", jb(p.reproject));
  std::fprintf(f, "    \"cap_odd_holes\": %s,\n", jb(p.cap_odd_holes));
  std::fprintf(f, "    \"smooth_iterations\": %d,\n", p.smooth_iterations);
  std::fprintf(f, "    \"smooth_strength\": %.9g,\n", p.smooth_strength);
  std::fprintf(f, "    \"seed\": %u\n", p.seed);
  std::fprintf(f, "  },\n");

  std::fprintf(f, "  \"output\": {\n");
  std::fprintf(f, "    \"path\": \"%s\",\n", jstr(objPath).c_str());
  std::fprintf(f, "    \"verts\": %d,\n", r.vert_count);
  std::fprintf(f, "    \"edges\": %d,\n", r.edge_count);
  std::fprintf(f, "    \"faces\": %d,\n", r.face_count);
  std::fprintf(f, "    \"quads\": %d,\n", r.quad_count);
  std::fprintf(f, "    \"tris\": %d,\n", r.tri_count);
  std::fprintf(f, "    \"ngons\": %d\n", r.ngon_count);
  std::fprintf(f, "  },\n");

  std::fprintf(f, "  \"validation\": {\n");
  std::fprintf(f, "    \"manifold\": %s,\n", jb(r.manifold));
  std::fprintf(f, "    \"manifold_error\": \"%s\",\n", jstr(r.manifold_error).c_str());
  std::fprintf(f, "    \"euler\": %d,\n", r.euler);
  std::fprintf(f, "    \"consistent_winding\": %s,\n", jb(r.consistent_winding));
  std::fprintf(f, "    \"non_manifold_edges\": %d,\n", r.non_manifold_edges);
  std::fprintf(f, "    \"boundary_edges\": %d,\n", r.boundary_edges);
  std::fprintf(f, "    \"degenerate_faces\": %d,\n", r.degenerate_faces);
  std::fprintf(f, "    \"inverted_faces\": %d,\n", r.inverted_faces);
  std::fprintf(f, "    \"all_quad\": %s,\n", jb(r.all_quad));
  std::fprintf(f, "    \"irregular_interior_verts\": %d,\n",
               r.irregular_interior_verts);
  std::fprintf(f, "    \"isolines_checked\": %s,\n", jb(r.isolines_checked));
  std::fprintf(f, "    \"spiral_isolines\": %d,\n", r.spiral_isolines);
  std::fprintf(f, "    \"open_isolines\": %d,\n", r.open_isolines);
  std::fprintf(f, "    \"closed_isolines\": %d\n", r.closed_isolines);
  std::fprintf(f, "  }\n");
  std::fprintf(f, "}\n");

  std::fclose(f);
  (void)out;
  return true;
}

void usage()
{
  std::printf(
      "remesh_cli --input <obj> [options]\n"
      "  --input <path>          input OBJ (bare name resolves against assets dir)\n"
      "  --outdir <dir>          output dir (default: tests/remesher-results)\n"
      "  --name <base>           output basename (default: input stem)\n"
      "  --target <float>        target quad edge length (default 0.1)\n"
      "  --solve <float>         solve-mesh edge length, 0=off (default 0)\n"
      "  --curvature <0|1>       align field to curvature (default 1)\n"
      "  --sharp <0|1>           pin field to sharp edges/boundaries (default 1)\n"
      "  --sharp-angle <float>   sharp dihedral threshold, radians (default 0.785)\n"
      "  --density <0|1>         use per-vertex density map (default 0)\n"
      "  --reproject <0|1>       snap output onto input surface (default 1)\n"
      "  --cap-odd <0|1>         close odd holes w/ one tri each (default 0)\n"
      "  --smooth <int>          reprojection smoothing iterations (default 2)\n"
      "  --smooth-strength <f>   per-iteration smoothing step 0..1 (default 0.5)\n"
      "  --seed <uint>           determinism seed (default 1)\n");
}

bool toBool(const char *s) { return std::atoi(s) != 0; }

} // namespace

int main(int argc, char **argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0); // parent reads progress live

  std::string input, outdir = REMESH_CLI_RESULTS_DIR, name;
  remesh::RemeshParams params;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](const char *flag) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "ERROR missing value for %s\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else if (a == "--input")
      input = next("--input");
    else if (a == "--outdir")
      outdir = next("--outdir");
    else if (a == "--name")
      name = next("--name");
    else if (a == "--target")
      params.target_edge_length = float(std::atof(next("--target")));
    else if (a == "--solve")
      params.solve_edge_length = float(std::atof(next("--solve")));
    else if (a == "--curvature")
      params.use_curvature = toBool(next("--curvature"));
    else if (a == "--sharp")
      params.use_sharp_features = toBool(next("--sharp"));
    else if (a == "--sharp-angle")
      params.sharp_angle = float(std::atof(next("--sharp-angle")));
    else if (a == "--density")
      params.use_density = toBool(next("--density"));
    else if (a == "--reproject")
      params.reproject = toBool(next("--reproject"));
    else if (a == "--cap-odd")
      params.cap_odd_holes = toBool(next("--cap-odd"));
    else if (a == "--smooth")
      params.smooth_iterations = std::atoi(next("--smooth"));
    else if (a == "--smooth-strength")
      params.smooth_strength = float(std::atof(next("--smooth-strength")));
    else if (a == "--seed")
      params.seed = uint32_t(std::strtoul(next("--seed"), nullptr, 10));
    else {
      std::fprintf(stderr, "ERROR unknown arg %s\n", a.c_str());
      return 2;
    }
  }

  if (input.empty()) {
    std::printf("ERROR no --input given\n");
    usage();
    return 2;
  }

  // Bare filename resolves against the assets dir.
  std::string inPath = input;
  if (!fs::exists(inPath)) {
    std::string alt = std::string(REMESH_CLI_ASSETS_DIR) + "/" + input;
    if (fs::exists(alt))
      inPath = alt;
  }
  if (name.empty())
    name = stem(inPath);

  // Load with quads preserved so the manifest records the asset's real face
  // count; QuadRemesh triangulates its own internal copy anyway.
  mesh::Mesh *in = mesh::loadObj(inPath.c_str(), /*keepNgons=*/true);
  if (!in) {
    std::printf("ERROR could not open input %s\n", inPath.c_str());
    return 1;
  }
  int inVerts = in->v.count, inFaces = in->f.count;
  float3 amin, amax;
  inputAABB(*in, amin, amax);

  auto t0 = std::chrono::steady_clock::now();
  mesh::Mesh *out = remesh::QuadRemesh(*in, params, progressCb, nullptr);
  auto t1 = std::chrono::steady_clock::now();
  long long durationMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  if (!out) {
    std::printf("ERROR QuadRemesh returned no mesh (extraction failed)\n");
    litestl::alloc::Delete<mesh::Mesh>(in);
    return 1;
  }

  mesh::RemeshReport r = mesh::remeshValidate(*out);

  std::string ts = timestamp();
  std::string base = name + "_" + ts;
  std::error_code ec;
  fs::create_directories(outdir, ec);
  // generic_string() → forward slashes, so the emitted paths are valid in JSON
  // and uniform for the parent process regardless of platform separator.
  std::string objPath = (fs::path(outdir) / (base + ".obj")).generic_string();
  std::string jsonPath = (fs::path(outdir) / (base + ".json")).generic_string();

  if (!mesh::writeObj(*out, objPath.c_str()))
    std::printf("ERROR could not write %s\n", objPath.c_str());
  else
    std::printf("RESULT %s\n", objPath.c_str());

  if (writeManifest(jsonPath.c_str(), ts, objPath, name, inPath, inVerts,
                    inFaces, amin, amax, params, *out, r, durationMs))
    std::printf("MANIFEST %s\n", jsonPath.c_str());
  else
    std::printf("ERROR could not write %s\n", jsonPath.c_str());

  std::printf("STATS verts=%d edges=%d faces=%d quads=%d tris=%d ngons=%d "
              "allquad=%d manifold=%d euler=%d inverted=%d boundary=%d "
              "spiral=%d irr=%d duration_ms=%lld\n",
              r.vert_count, r.edge_count, r.face_count, r.quad_count,
              r.tri_count, r.ngon_count, int(r.all_quad), int(r.manifold),
              r.euler, r.inverted_faces, r.boundary_edges, r.spiral_isolines,
              r.irregular_interior_verts, durationMs);

  litestl::alloc::Delete<mesh::Mesh>(out);
  litestl::alloc::Delete<mesh::Mesh>(in);
  return 0;
}
