// sbrushc — Sculptcore brush DSL compiler.
//
// Usage:
//   sbrushc --backend=cpp --in=<input.sbrush> --out=<output.gen.h>
//
// Wave 1: single brush per invocation, single backend (cpp).
// Wave 2+ extends to multi-backend dispatch and additional emitters.

#include "emit_cpp.h"
#include "emit_cuda.h"
#include "emit_opencl.h"
#include "emit_registry.h"
#include "emit_wgsl.h"
#include "ir.h"
#include "lexer.h"
#include "parser.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace sculptcore::brush::sbrush;

namespace {

struct Args {
  litestl::util::string backend;
  litestl::util::string inPath;
  litestl::util::string outPath;
  bool dryRun = false;
  bool dumpTokens = false;
  // Extra (out-of-repo) kernel: unlisted float uniforms use the named store.
  bool extras = false;
  // --registry mode (extra-kernel registry generation).
  bool registry = false;
  litestl::util::string outDir;
  litestl::util::Vector<litestl::util::string> inPaths;
  litestl::util::Vector<litestl::util::string> builtinPaths;
  litestl::util::string reserved;
};

void printUsage()
{
  std::fprintf(stderr,
    "Usage: sbrushc --backend=<cpp|wgsl|spirv|cuda|hip|opencl> --in=<input.sbrush> --out=<output>\n"
    "  --dry-run     do not write output\n"
    "  --dump-tokens print token stream and exit\n"
    "  --extras      extra (out-of-repo) kernel: unlisted float uniforms use the\n"
    "                Brush.namedFloats store instead of erroring (cpp backend)\n"
    "Registry mode (extra-kernel enum/factory registration):\n"
    "  sbrushc --registry --out-dir=<dir> --in=<extra.sbrush>...\n"
    "          --builtin=<builtin.sbrush>... --reserved=<NAME,NAME,...>\n");
}

bool parseArgs(int argc, char **argv, Args &out)
{
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (std::strncmp(a, "--backend=", 10) == 0) out.backend = a + 10;
    else if (std::strncmp(a, "--in=", 5) == 0) out.inPaths.append(a + 5);
    else if (std::strncmp(a, "--out=", 6) == 0) out.outPath = a + 6;
    else if (std::strncmp(a, "--out-dir=", 10) == 0) out.outDir = a + 10;
    else if (std::strncmp(a, "--builtin=", 10) == 0) out.builtinPaths.append(a + 10);
    else if (std::strncmp(a, "--reserved=", 11) == 0) out.reserved = a + 11;
    else if (std::strcmp(a, "--registry") == 0) out.registry = true;
    else if (std::strcmp(a, "--extras") == 0) out.extras = true;
    else if (std::strcmp(a, "--dry-run") == 0) out.dryRun = true;
    else if (std::strcmp(a, "--dump-tokens") == 0) out.dumpTokens = true;
    else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
      printUsage();
      std::exit(0);
    } else {
      std::fprintf(stderr, "sbrushc: unknown arg '%s'\n", a);
      return false;
    }
  }
  if (out.registry) {
    if (out.outDir.size() == 0) {
      std::fprintf(stderr, "sbrushc: --registry requires --out-dir\n");
      return false;
    }
    return true;
  }
  if (out.backend.size() == 0) out.backend = "cpp";
  if (out.inPaths.size() != 1) {
    std::fprintf(stderr, "sbrushc: exactly one --in is required\n");
    return false;
  }
  out.inPath = out.inPaths[0];
  if (out.outPath.size() == 0 && !out.dryRun && !out.dumpTokens) {
    std::fprintf(stderr, "sbrushc: --out is required (or pass --dry-run)\n");
    return false;
  }
  return true;
}

bool readFile(const char *path, std::string &dst)
{
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  dst = ss.str();
  return true;
}

bool writeFileIfChanged(const char *path, const litestl::util::string &content)
{
  // Avoid touching mtime if content matches — otherwise CMake regenerates
  // the whole world on every invocation.
  std::ifstream existing(path, std::ios::binary);
  if (existing) {
    std::ostringstream ss;
    ss << existing.rdbuf();
    std::string s = ss.str();
    if ((int)s.size() == (int)content.size() &&
        std::memcmp(s.data(), content.c_str(), s.size()) == 0) {
      return true;
    }
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  std::printf("sbrushc: %s\n", path);
  out.write(content.c_str(), content.size());
  return out.good();
}

// Lex + parse one .sbrush file; errors go to stderr, null on failure.
// `dumpTokens` prints the token stream and returns null without parsing.
std::unique_ptr<Brush> parseKernelFile(const litestl::util::string &path,
                                       bool dumpTokens = false)
{
  std::string src;
  if (!readFile(path.c_str(), src)) {
    std::fprintf(stderr, "sbrushc: cannot read '%s'\n", path.c_str());
    return nullptr;
  }

  litestl::util::string srcLst(src.c_str());
  auto lex_r = lex(litestl::util::stringref(srcLst.c_str()),
                   litestl::util::stringref(path.c_str()));
  if (lex_r.errors.size() > 0) {
    for (const auto &e : lex_r.errors) {
      std::fprintf(stderr, "%s:%d:%d: lex error: %s\n",
                   path.c_str(), e.line, e.col, e.message.c_str());
    }
    return nullptr;
  }

  if (dumpTokens) {
    for (const auto &t : lex_r.tokens) {
      std::fprintf(stderr, "%d:%d %s '%s'\n",
                   t.line, t.col, tokKindName(t.kind), t.text.c_str());
    }
    return nullptr;
  }

  auto parse_r = parse(lex_r.tokens, litestl::util::stringref(path.c_str()));
  if (parse_r.errors.size() > 0) {
    for (const auto &e : parse_r.errors) {
      std::fprintf(stderr, "%s:%d:%d: parse error: %s\n",
                   path.c_str(), e.line, e.col, e.message.c_str());
    }
    return nullptr;
  }
  if (!parse_r.brush) {
    std::fprintf(stderr, "sbrushc: '%s' parsed but produced no brush\n", path.c_str());
    return nullptr;
  }
  return std::move(parse_r.brush);
}

// Filename minus directory and extension ("a/b/nudge.sbrush" -> "nudge").
litestl::util::string stemOf(const litestl::util::string &path)
{
  const char *s = path.c_str();
  const char *base = s;
  for (const char *p = s; *p; p++) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  const char *dot = std::strrchr(base, '.');
  std::string stem = dot ? std::string(base, dot - base) : std::string(base);
  return litestl::util::string(stem.c_str());
}

int runRegistryMode(const Args &args)
{
  litestl::util::Vector<RegistryEntry> extras;
  for (const auto &p : args.inPaths) {
    auto brush = parseKernelFile(p);
    if (!brush) {
      return 1;
    }
    RegistryEntry e;
    e.stem = stemOf(p);
    e.attrName = brush->attrName;
    e.cppName = brush->cppName;
    e.usesNeighbor = brushUsesNeighborLoop(*brush);
    for (const auto &f : brush->fields) {
      // Scalar floats not backed by a Brush member take namedFloats slots;
      // non-float unlisted uniforms are rejected by the per-kernel cpp emit.
      if (f.type == TypeKind::Float && fieldUsesStore(f)) {
        StoreUniform su;
        su.name = f.name;
        su.def = f.hasDefault ? f.defaultValue : 0.0;
        e.storeUniforms.append(su);
      }
    }
    extras.append(e);
  }

  litestl::util::Vector<litestl::util::string> builtinStems, builtinNames;
  for (const auto &p : args.builtinPaths) {
    auto brush = parseKernelFile(p);
    if (!brush) {
      return 1;
    }
    builtinStems.append(stemOf(p));
    builtinNames.append(brush->attrName);
  }

  litestl::util::Vector<litestl::util::string> reserved;
  {
    std::string cur;
    for (const char *p = args.reserved.c_str();; p++) {
      if (*p == ',' || *p == '\0') {
        if (!cur.empty()) {
          reserved.append(litestl::util::string(cur.c_str()));
        }
        cur.clear();
        if (*p == '\0') {
          break;
        }
      } else {
        cur += *p;
      }
    }
  }

  auto rr = emitRegistry(extras, builtinStems, builtinNames, reserved);
  if (rr.errors.size() > 0) {
    for (const auto &e : rr.errors) {
      std::fprintf(stderr, "sbrushc: registry error: %s\n", e.c_str());
    }
    return 1;
  }

  litestl::util::string incPath = args.outDir + "/sculptcore_extra_brushes_enum.inc";
  litestl::util::string hdrPath = args.outDir + "/sculptcore_extra_brushes.gen.h";
  if (!writeFileIfChanged(incPath.c_str(), rr.enumInc)) {
    std::fprintf(stderr, "sbrushc: cannot write '%s'\n", incPath.c_str());
    return 1;
  }
  if (!writeFileIfChanged(hdrPath.c_str(), rr.genHeader)) {
    std::fprintf(stderr, "sbrushc: cannot write '%s'\n", hdrPath.c_str());
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv)
{
  Args args;
  if (!parseArgs(argc, argv, args)) {
    printUsage();
    return 2;
  }

  if (args.registry) {
    return runRegistryMode(args);
  }

  auto brush = parseKernelFile(args.inPath, args.dumpTokens);
  if (!brush) {
    return args.dumpTokens ? 0 : 1;
  }

  litestl::util::string backend = args.backend;
  EmitResult er;
  if (litestl::util::string(backend.c_str()) == litestl::util::string("cpp")) {
    CppEmitOptions cppOpts;
    cppOpts.extras = args.extras;
    er = emitCpp(*brush, cppOpts);
  } else if (litestl::util::string(backend.c_str()) == litestl::util::string("wgsl") ||
             litestl::util::string(backend.c_str()) == litestl::util::string("spirv")) {
    // SPIR-V is reached by lowering the WGSL through tint (--format=spirv);
    // sbrushc's job for the spirv backend is to emit the WGSL that tint
    // consumes, so both backends share emitWgsl. A future slice can swap in a
    // direct emit_spirv.cc here without touching the build wiring.
    er = emitWgsl(*brush);
  } else if (litestl::util::string(backend.c_str()) == litestl::util::string("cuda")) {
    er = emitCuda(*brush, BackendKind::Cuda);
  } else if (litestl::util::string(backend.c_str()) == litestl::util::string("hip")) {
    er = emitCuda(*brush, BackendKind::Hip);
  } else if (litestl::util::string(backend.c_str()) == litestl::util::string("opencl")) {
    er = emitOpencl(*brush);
  } else {
    std::fprintf(stderr, "sbrushc: backend '%s' not implemented yet\n", backend.c_str());
    return 1;
  }

  if (er.errors.size() > 0) {
    for (const auto &e : er.errors) {
      std::fprintf(stderr, "sbrushc: emit error: %s\n", e.c_str());
    }
    return 1;
  }
  if (args.dryRun) {
    std::fwrite(er.text.c_str(), 1, er.text.size(), stdout);
    return 0;
  }
  if (!writeFileIfChanged(args.outPath.c_str(), er.text)) {
    std::fprintf(stderr, "sbrushc: cannot write '%s'\n", args.outPath.c_str());
    return 1;
  }
  return 0;
}
