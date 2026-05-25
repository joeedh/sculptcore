// sbrushc — Sculptcore brush DSL compiler.
//
// Usage:
//   sbrushc --backend=cpp --in=<input.sbrush> --out=<output.gen.h>
//
// Wave 1: single brush per invocation, single backend (cpp).
// Wave 2+ extends to multi-backend dispatch and additional emitters.

#include "emit_cpp.h"
#include "emit_cuda.h"
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
};

void printUsage()
{
  std::fprintf(stderr,
    "Usage: sbrushc --backend=<cpp|wgsl|spirv|cuda|hip> --in=<input.sbrush> --out=<output>\n"
    "  --dry-run     do not write output\n"
    "  --dump-tokens print token stream and exit\n");
}

bool parseArgs(int argc, char **argv, Args &out)
{
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (std::strncmp(a, "--backend=", 10) == 0) out.backend = a + 10;
    else if (std::strncmp(a, "--in=", 5) == 0) out.inPath = a + 5;
    else if (std::strncmp(a, "--out=", 6) == 0) out.outPath = a + 6;
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
  if (out.backend.size() == 0) out.backend = "cpp";
  if (out.inPath.size() == 0) {
    std::fprintf(stderr, "sbrushc: --in is required\n");
    return false;
  }
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
  out.write(content.c_str(), content.size());
  return out.good();
}

} // namespace

int main(int argc, char **argv)
{
  Args args;
  if (!parseArgs(argc, argv, args)) {
    printUsage();
    return 2;
  }

  std::string src;
  if (!readFile(args.inPath.c_str(), src)) {
    std::fprintf(stderr, "sbrushc: cannot read '%s'\n", args.inPath.c_str());
    return 1;
  }

  litestl::util::string srcLst(src.c_str());
  auto lex_r = lex(litestl::util::stringref(srcLst.c_str()),
                   litestl::util::stringref(args.inPath.c_str()));
  if (lex_r.errors.size() > 0) {
    for (const auto &e : lex_r.errors) {
      std::fprintf(stderr, "%s:%d:%d: lex error: %s\n",
                   args.inPath.c_str(), e.line, e.col, e.message.c_str());
    }
    return 1;
  }

  if (args.dumpTokens) {
    for (const auto &t : lex_r.tokens) {
      std::fprintf(stderr, "%d:%d %s '%s'\n",
                   t.line, t.col, tokKindName(t.kind), t.text.c_str());
    }
    return 0;
  }

  auto parse_r = parse(lex_r.tokens, litestl::util::stringref(args.inPath.c_str()));
  if (parse_r.errors.size() > 0) {
    for (const auto &e : parse_r.errors) {
      std::fprintf(stderr, "%s:%d:%d: parse error: %s\n",
                   args.inPath.c_str(), e.line, e.col, e.message.c_str());
    }
    return 1;
  }
  if (!parse_r.brush) {
    std::fprintf(stderr, "sbrushc: parsed but produced no brush\n");
    return 1;
  }

  litestl::util::string backend = args.backend;
  EmitResult er;
  if (litestl::util::string(backend.c_str()) == litestl::util::string("cpp")) {
    er = emitCpp(*parse_r.brush);
  } else if (litestl::util::string(backend.c_str()) == litestl::util::string("wgsl") ||
             litestl::util::string(backend.c_str()) == litestl::util::string("spirv")) {
    // SPIR-V is reached by lowering the WGSL through tint (--format=spirv);
    // sbrushc's job for the spirv backend is to emit the WGSL that tint
    // consumes, so both backends share emitWgsl. A future slice can swap in a
    // direct emit_spirv.cc here without touching the build wiring.
    er = emitWgsl(*parse_r.brush);
  } else if (litestl::util::string(backend.c_str()) == litestl::util::string("cuda")) {
    er = emitCuda(*parse_r.brush, BackendKind::Cuda);
  } else if (litestl::util::string(backend.c_str()) == litestl::util::string("hip")) {
    er = emitCuda(*parse_r.brush, BackendKind::Hip);
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
