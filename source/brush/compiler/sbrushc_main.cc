// sbrushc — Sculptcore brush DSL compiler.
//
// Usage:
//   sbrushc --backend=cpp --in=<input.sbrush> --out=<output.gen.h>
//
// Wave 1: single brush per invocation, single backend (cpp).
// Wave 2+ extends to multi-backend dispatch and additional emitters.

#include "emit_c.h"
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

#include <cctype>
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
  // --builtin-registry mode (built-in enum items + factory dispatch).
  bool builtinRegistry = false;
  // --texture-unit mode (.stex -> <stem>.tex.gen.h).
  bool textureUnit = false;
  // --texture-registry mode (all .stex units -> sculptcore_textures.gen.h).
  bool textureRegistry = false;
  // Line endings for written files: "auto" (git's working-tree setting),
  // "lf" or "crlf".
  litestl::util::string eol = "auto";
  litestl::util::string outDir;
  litestl::util::Vector<litestl::util::string> inPaths;
  litestl::util::Vector<litestl::util::string> builtinPaths;
  // .stex units that `use texture <Name>;` imports resolve against.
  litestl::util::Vector<litestl::util::string> texturePaths;
  litestl::util::string reserved;
  // brushes/tools.txt: the built-in SculptBrushes item names in id order.
  litestl::util::string toolsPath;
};

void printUsage()
{
  std::fprintf(stderr,
    "Usage: sbrushc --backend=<cpp|wgsl|spirv|cuda|hip|opencl> --in=<input.sbrush> --out=<output>\n"
    "  --dry-run     do not write output\n"
    "  --dump-tokens print token stream and exit\n"
    "  --extras      extra (out-of-repo) kernel: unlisted float uniforms use the\n"
    "                Brush.namedFloats store instead of erroring (cpp backend)\n"
    "  --texture=<unit.stex>  texture unit whose textures satisfy the brush's\n"
    "                `use texture <Name>;` imports (repeatable)\n"
    "  --eol=<auto|lf|crlf>  line endings for written files; auto (default)\n"
    "                follows git's core.eol / core.autocrlf working-tree setting\n"
    "Registry mode (extra-kernel enum/factory registration):\n"
    "  sbrushc --registry --out-dir=<dir> --in=<extra.sbrush>...\n"
    "          --builtin=<builtin.sbrush>... --reserved=<NAME,NAME,...>\n"
    "Builtin-registry mode (built-in enum items + id-keyed factory dispatch):\n"
    "  sbrushc --builtin-registry --out-dir=<dir> --tools=<brushes/tools.txt>\n"
    "          --in=<builtin.sbrush>...\n"
    "Texture-unit mode (precompile one .stex unit):\n"
    "  sbrushc --texture-unit --in=<unit.stex> --out=<stem.tex.gen.h>\n"
    "          --backend=c emits a freestanding C99 TU (JIT input) instead\n"
    "Texture-registry mode (registry over all precompiled units):\n"
    "  sbrushc --texture-registry --out-dir=<dir> [--in=<unit.stex>...]\n");
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
    else if (std::strncmp(a, "--texture=", 10) == 0) out.texturePaths.append(a + 10);
    else if (std::strncmp(a, "--reserved=", 11) == 0) out.reserved = a + 11;
    else if (std::strncmp(a, "--tools=", 8) == 0) out.toolsPath = a + 8;
    else if (std::strncmp(a, "--eol=", 6) == 0) out.eol = a + 6;
    else if (std::strcmp(a, "--registry") == 0) out.registry = true;
    else if (std::strcmp(a, "--builtin-registry") == 0) out.builtinRegistry = true;
    else if (std::strcmp(a, "--texture-unit") == 0) out.textureUnit = true;
    else if (std::strcmp(a, "--texture-registry") == 0) out.textureRegistry = true;
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
  if (std::strcmp(out.eol.c_str(), "auto") != 0 && std::strcmp(out.eol.c_str(), "lf") != 0 &&
      std::strcmp(out.eol.c_str(), "crlf") != 0) {
    std::fprintf(stderr, "sbrushc: --eol must be auto, lf or crlf\n");
    return false;
  }
  if ((int)out.registry + (int)out.builtinRegistry + (int)out.textureUnit +
          (int)out.textureRegistry >
      1) {
    std::fprintf(stderr, "sbrushc: --registry, --builtin-registry, --texture-unit and "
                         "--texture-registry are exclusive\n");
    return false;
  }
  if (out.builtinRegistry && out.toolsPath.size() == 0) {
    std::fprintf(stderr, "sbrushc: --builtin-registry requires --tools\n");
    return false;
  }
  if (out.registry || out.builtinRegistry || out.textureRegistry) {
    if (out.outDir.size() == 0) {
      const char *mode = out.registry ? "registry"
                                      : (out.builtinRegistry ? "builtin-registry"
                                                             : "texture-registry");
      std::fprintf(stderr, "sbrushc: --%s requires --out-dir\n", mode);
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

// Line ending written by writeFileIfChanged; set from --eol in main().
std::string g_eol = "\n";

// `git config --get <key>`, trimmed; empty when unset or git is unavailable.
std::string gitConfigValue(const char *key)
{
  std::string cmd = "git config --get ";
  cmd += key;
#ifdef _WIN32
  cmd += " 2>NUL";
  FILE *pipe = _popen(cmd.c_str(), "r");
#else
  cmd += " 2>/dev/null";
  FILE *pipe = popen(cmd.c_str(), "r");
#endif
  if (!pipe) return "";

  std::string out;
  char buf[256];
  while (std::fgets(buf, sizeof(buf), pipe)) {
    out += buf;
  }
#ifdef _WIN32
  _pclose(pipe);
#else
  pclose(pipe);
#endif

  while (out.size() > 0 && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' ||
                            out.back() == '\t')) {
    out.pop_back();
  }
  return out;
}

// The working-tree line ending git would check these files out with; mirrors
// tools/genTS.ts so every generator in the repo agrees.
const std::string &nativeEOL()
{
  static const std::string eol = [] {
    // core.eol pins the working-tree ending outright and wins over core.autocrlf.
    std::string coreEol = gitConfigValue("core.eol");
    if (coreEol == "lf") return std::string("\n");
    if (coreEol == "crlf") return std::string("\r\n");

    std::string autocrlf = gitConfigValue("core.autocrlf");
    if (autocrlf == "input" || autocrlf == "false") return std::string("\n");
    // 'true', or unset with core.eol=native → OS-native EOL
#ifdef _WIN32
    return std::string("\r\n");
#else
    return std::string("\n");
#endif
  }();
  return eol;
}

// Rewrite every line ending in `text` as `eol` (emitters always produce \n).
std::string applyEOL(const litestl::util::string &text, const std::string &eol)
{
  std::string out;
  out.reserve(text.size() + (eol.size() > 1 ? text.size() / 8 : 0));
  const char *p = text.c_str();
  for (size_t i = 0, n = (size_t)text.size(); i < n; i++) {
    if (p[i] == '\r') {
      if (i + 1 < n && p[i + 1] == '\n') i++;
      out += eol;
    } else if (p[i] == '\n') {
      out += eol;
    } else {
      out += p[i];
    }
  }
  return out;
}

bool writeFileIfChanged(const char *path, const litestl::util::string &content)
{
  std::string text = applyEOL(content, g_eol);

  // Avoid touching mtime if content matches — otherwise CMake regenerates
  // the whole world on every invocation.
  std::ifstream existing(path, std::ios::binary);
  if (existing) {
    std::ostringstream ss;
    ss << existing.rdbuf();
    std::string s = ss.str();
    if (s == text) {
      return true;
    }
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  std::printf("sbrushc: %s\n", path);
  out.write(text.data(), (std::streamsize)text.size());
  return out.good();
}

// Lex + parse one DSL source file (brush or texture unit); errors go to
// stderr, false on failure. `dumpTokens` prints the token stream and returns
// false without parsing.
bool parseSourceFile(const litestl::util::string &path, ParseResult &out,
                     bool dumpTokens = false)
{
  std::string src;
  if (!readFile(path.c_str(), src)) {
    std::fprintf(stderr, "sbrushc: cannot read '%s'\n", path.c_str());
    return false;
  }

  litestl::util::string srcLst(src.c_str());
  auto lex_r = lex(litestl::util::stringref(srcLst.c_str()),
                   litestl::util::stringref(path.c_str()));
  if (lex_r.errors.size() > 0) {
    for (const auto &e : lex_r.errors) {
      std::fprintf(stderr, "%s:%d:%d: lex error: %s\n",
                   path.c_str(), e.line, e.col, e.message.c_str());
    }
    return false;
  }

  if (dumpTokens) {
    for (const auto &t : lex_r.tokens) {
      std::fprintf(stderr, "%d:%d %s '%s'\n",
                   t.line, t.col, tokKindName(t.kind), t.text.c_str());
    }
    return false;
  }

  out = parse(lex_r.tokens, litestl::util::stringref(path.c_str()));
  if (out.errors.size() > 0) {
    for (const auto &e : out.errors) {
      std::fprintf(stderr, "%s:%d:%d: parse error: %s\n",
                   path.c_str(), e.line, e.col, e.message.c_str());
    }
    return false;
  }
  return true;
}

// Parse one .sbrush file; null on failure (or after --dump-tokens).
std::unique_ptr<Brush> parseKernelFile(const litestl::util::string &path,
                                       bool dumpTokens = false)
{
  ParseResult r;
  if (!parseSourceFile(path, r, dumpTokens)) {
    return nullptr;
  }
  if (!r.brush) {
    std::fprintf(stderr, "sbrushc: '%s' parsed but produced no brush\n", path.c_str());
    return nullptr;
  }
  return std::move(r.brush);
}

// Parse one .stex texture unit; null on failure.
std::unique_ptr<TextureUnit> parseTextureUnitFile(const litestl::util::string &path)
{
  ParseResult r;
  if (!parseSourceFile(path, r)) {
    return nullptr;
  }
  if (!r.unit) {
    std::fprintf(stderr, "sbrushc: '%s' parsed but is not a texture unit\n", path.c_str());
    return nullptr;
  }
  return std::move(r.unit);
}

// Resolve the brush's `use texture <Name>;` imports against the --texture=
// units. Each resolved TextureDef is moved into brush.textures so the
// backends treat it exactly like an inline texture.
bool resolveUseTextures(Brush &brush,
                        litestl::util::Vector<std::unique_ptr<TextureUnit>> &units)
{
  using litestl::util::string;
  bool ok = true;
  for (int i = 0; i < (int)brush.useTextures.size(); i++) {
    const string &name = brush.useTextures[i];

    bool skip = false;
    for (int j = 0; j < i; j++) {
      if (string(brush.useTextures[j].c_str()) == string(name.c_str())) {
        std::fprintf(stderr, "sbrushc: duplicate 'use texture %s'\n", name.c_str());
        ok = false;
        skip = true;
        break;
      }
    }
    for (const auto &t : brush.textures) {
      if (!skip && string(t.name.c_str()) == string(name.c_str())) {
        std::fprintf(stderr,
                     "sbrushc: 'use texture %s' collides with an inline texture of the same name\n",
                     name.c_str());
        ok = false;
        skip = true;
      }
    }
    if (skip) {
      continue;
    }

    TextureUnit *foundIn = nullptr;
    TextureDef *found = nullptr;
    for (auto &up : units) {
      for (auto &td : up->textures) {
        if (string(td.name.c_str()) != string(name.c_str())) {
          continue;
        }
        if (found) {
          std::fprintf(stderr, "sbrushc: texture '%s' defined in both '%s' and '%s'\n",
                       name.c_str(), foundIn->sourceFile.c_str(), up->sourceFile.c_str());
          ok = false;
          skip = true;
        } else {
          found = &td;
          foundIn = up.get();
        }
      }
    }
    if (skip) {
      continue;
    }
    if (!found) {
      std::fprintf(stderr, "sbrushc: 'use texture %s': no such texture in any --texture= unit\n",
                   name.c_str());
      ok = false;
      continue;
    }
    found->imported = true;
    brush.textures.append(std::move(*found));
  }
  return ok;
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
    e.fullTopo = brush->isFullTopo;
    e.faceStage = brushHasFaceStage(*brush);
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

int runBuiltinRegistryMode(const Args &args)
{
  std::string toolsText;
  if (!readFile(args.toolsPath.c_str(), toolsText)) {
    std::fprintf(stderr, "sbrushc: cannot read tool id table '%s'\n", args.toolsPath.c_str());
    return 1;
  }
  litestl::util::Vector<litestl::util::string> toolIds;
  {
    std::string cur;
    bool comment = false;
    for (const char *p = toolsText.c_str();; p++) {
      if (*p == '\n' || *p == '\r' || *p == '\0') {
        if (!cur.empty()) {
          toolIds.append(litestl::util::string(cur.c_str()));
        }
        cur.clear();
        comment = false;
        if (*p == '\0') {
          break;
        }
      } else if (*p == '#') {
        comment = true;
      } else if (!comment && !std::isspace((unsigned char)*p)) {
        cur += *p;
      }
    }
  }

  litestl::util::Vector<BuiltinEntry> kernels;
  for (const auto &p : args.inPaths) {
    auto brush = parseKernelFile(p);
    if (!brush) {
      return 1;
    }
    BuiltinEntry e;
    e.stem = stemOf(p);
    e.usesNeighbor = brushUsesNeighborLoop(*brush);
    e.fullTopo = brush->isFullTopo;
    e.gpu = brush->isGpu;
    e.faceStage = brushHasFaceStage(*brush);
    for (const auto &t : brush->tools) {
      e.tools.append(t);
    }
    kernels.append(std::move(e));
  }

  auto rr = emitBuiltinRegistry(kernels, toolIds);
  if (rr.errors.size() > 0) {
    for (const auto &e : rr.errors) {
      std::fprintf(stderr, "sbrushc: builtin registry error: %s\n", e.c_str());
    }
    return 1;
  }

  litestl::util::string incPath = args.outDir + "/builtin_brushes_enum.inc";
  litestl::util::string hdrPath = args.outDir + "/builtin_brushes.gen.h";
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

int runTextureUnitMode(const Args &args)
{
  auto unit = parseTextureUnitFile(args.inPath);
  if (!unit) {
    return 1;
  }
  EmitResult er;
  if (std::strcmp(args.backend.c_str(), "c") == 0) {
    // C99 backend: raw .c text for the runtime tinycc JIT rather than a
    // precompiled header, so the textures stay unguarded (imported = false).
    Brush scratch;
    scratch.sourceFile = unit->sourceFile;
    for (auto &td : unit->textures) {
      scratch.textures.append(std::move(td));
    }
    for (auto &sd : unit->samplers) {
      scratch.samplers.append(std::move(sd));
    }
    er = emitCTextureDefs(scratch);
  } else {
    er = emitTextureUnitHeader(*unit, stemOf(args.inPath));
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

int runTextureRegistryMode(const Args &args)
{
  litestl::util::Vector<std::unique_ptr<TextureUnit>> units;
  litestl::util::Vector<litestl::util::string> stems;
  litestl::util::Vector<const TextureUnit *> unitPtrs;
  for (const auto &p : args.inPaths) {
    auto unit = parseTextureUnitFile(p);
    if (!unit) {
      return 1;
    }
    stems.append(stemOf(p));
    units.append(std::move(unit));
  }
  for (const auto &up : units) {
    unitPtrs.append(up.get());
  }

  EmitResult er = emitTextureRegistry(stems, unitPtrs);
  if (er.errors.size() > 0) {
    for (const auto &e : er.errors) {
      std::fprintf(stderr, "sbrushc: texture registry error: %s\n", e.c_str());
    }
    return 1;
  }
  litestl::util::string outPath = args.outDir + "/sculptcore_textures.gen.h";
  if (!writeFileIfChanged(outPath.c_str(), er.text)) {
    std::fprintf(stderr, "sbrushc: cannot write '%s'\n", outPath.c_str());
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

  if (std::strcmp(args.eol.c_str(), "lf") == 0) {
    g_eol = "\n";
  } else if (std::strcmp(args.eol.c_str(), "crlf") == 0) {
    g_eol = "\r\n";
  } else {
    g_eol = nativeEOL();
  }

  if (args.registry) {
    return runRegistryMode(args);
  }
  if (args.builtinRegistry) {
    return runBuiltinRegistryMode(args);
  }
  if (args.textureRegistry) {
    return runTextureRegistryMode(args);
  }
  if (args.textureUnit) {
    return runTextureUnitMode(args);
  }

  auto brush = parseKernelFile(args.inPath, args.dumpTokens);
  if (!brush) {
    return args.dumpTokens ? 0 : 1;
  }

  litestl::util::Vector<std::unique_ptr<TextureUnit>> units;
  for (const auto &p : args.texturePaths) {
    auto unit = parseTextureUnitFile(p);
    if (!unit) {
      return 1;
    }
    units.append(std::move(unit));
  }
  if (!resolveUseTextures(*brush, units)) {
    return 1;
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
  for (const auto &w : er.warnings) {
    std::fprintf(stderr, "sbrushc: %s: warning: %s\n", args.inPath.c_str(), w.c_str());
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
