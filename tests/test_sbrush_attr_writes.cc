// Golden for the sbrush attr write inference (grid-domain-attributes P0a).
//
// BrushAttrManifestEntry::materialize is set for every attr entry — it only
// means "the executor must ensure the layer exists". The bit that says whether a
// kernel *stores* to an attr is `kernelWrites`, inferred here from the stage
// bodies. Any capability rule keyed on the wrong one routes bsmooth — i.e. every
// autosmooth stroke — as a painting brush.
//
// Three gates:
//  1. The seven-row table below, pinned per (kernel, attr).
//  2. A store through a swizzle or index of an attr is a hard codegen error, not
//     a silently-missed write.
//  3. The `save` list cross-check emits no warnings over the real kernels: every
//     body-written attr is saved (else its stroke would not undo) and no
//     read-only attr is (dead capture).
#include "test_util.h"
#include "test_config.h"

#include "brush/compiler/emit_cpp.h"
#include "brush/compiler/lexer.h"
#include "brush/compiler/parser.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

test_init;

using namespace sculptcore::brush::sbrush;

static bool readWholeFile(const char *path, std::string &out)
{
  FILE *f = std::fopen(path, "rb");
  if (!f) {
    return false;
  }
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out.append(buf, n);
  }
  std::fclose(f);
  return true;
}

static std::unique_ptr<Brush> parseBrushSource(const char *src, const char *name)
{
  litestl::util::string owned(src);
  auto lr = lex(litestl::util::stringref(owned.c_str()), litestl::util::stringref(name));
  if (lr.errors.size() > 0) {
    for (const auto &e : lr.errors) {
      std::printf("%s:%d: lex error: %s\n", name, e.line, e.message.c_str());
    }
    return nullptr;
  }
  ParseResult pr = parse(lr.tokens, litestl::util::stringref(name));
  if (pr.errors.size() > 0) {
    for (const auto &e : pr.errors) {
      std::printf("%s:%d: parse error: %s\n", name, e.line, e.message.c_str());
    }
    return nullptr;
  }
  return std::move(pr.brush);
}

static std::unique_ptr<Brush> parseKernel(const char *stem)
{
  char path[2048];
  std::snprintf(path, sizeof(path), "%s/%s.sbrush", SCULPTCORE_KERNELS_DIR, stem);
  std::string src;
  if (!readWholeFile(path, src)) {
    std::printf("cannot read '%s'\n", path);
    return nullptr;
  }
  return parseBrushSource(src.c_str(), stem);
}

struct GoldenRow {
  const char *kernel;
  const char *attr;
  bool kernelWrites;
};

// §4.1 of claudeMemory/plans/grid-domain-attributes.md. The three read-only
// attrs are host-pre-pass scratch (no write-back destination is ever needed);
// the four writes are exactly the handles a capability rule must route.
static const GoldenRow kGolden[] = {
    {"bsmooth", "vclass", false},
    {"featurealign", "field", false},
    {"featurealign", "vclass", false},
    {"enhance", "edisp", false},
    {"color", "color", true},
    {"colorsmooth", "color", true},
    {"layerdraw", "slayer", true},
    {"polygroup", "group", true},
};

// A kernel storing through a swizzle of its attr: the top-level-name test reads
// the lvalue's name as "x", so without the hard error this would report
// kernelWrites == false and route a painting stroke as a read-only one.
static const char *kSwizzleSrc = R"(
@brush("color")
@paint
brush BadSwizzle {
  attr vertex float4 color @use(color);

  save vertex color;

  vertex void apply(inout Vertex v) {
    float s = strength(v.co) * masks();
    v.color.x = s;
  }
}
)";

static void runTests()
{
  // --- Gate 1: the golden table ---
  for (const auto &row : kGolden) {
    auto brush = parseKernel(row.kernel);
    test_assert(brush != nullptr);
    if (!brush) {
      continue;
    }
    MemberWriteKind wk = brushScanMemberWrites(*brush, row.attr);
    if (wk == MemberWriteKind::Nested) {
      std::printf("%s.%s: unexpected nested (swizzle/index) store\n", row.kernel, row.attr);
    }
    bool writes = wk == MemberWriteKind::TopLevel;
    if (writes != row.kernelWrites) {
      std::printf("%s.%s: kernelWrites %d, expected %d\n",
                  row.kernel, row.attr, int(writes), int(row.kernelWrites));
    }
    test_assert(writes == row.kernelWrites);

    // The inferred bit must reach the emitted manifest entry, in the position
    // the runtime struct reads it from.
    EmitResult er = emitCpp(*brush);
    test_assert(er.errors.size() == 0);
    char needle[512];
    std::snprintf(needle, sizeof(needle),
                  "BrushAttrManifestEntry{\"%s\", ", row.attr);
    const char *line = std::strstr(er.text.c_str(), needle);
    test_assert(line != nullptr);
    if (line) {
      const char *end = std::strchr(line, '\n');
      std::string entry(line, end ? size_t(end - line) : std::strlen(line));
      // ..., domain, materialize, kernelWrites, use});  — materialize is always
      // true, so the pair pins which of the two the emitter wrote.
      bool sawTrueTrue = entry.find("Vertex, true, true,") != std::string::npos ||
                         entry.find("Face, true, true,") != std::string::npos;
      bool sawTrueFalse = entry.find("Vertex, true, false,") != std::string::npos ||
                          entry.find("Face, true, false,") != std::string::npos;
      test_assert(sawTrueTrue != sawTrueFalse);
      test_assert(sawTrueTrue == row.kernelWrites);
    }
  }

  // --- Gate 2: a swizzled store is a hard error ---
  {
    auto brush = parseBrushSource(kSwizzleSrc, "BadSwizzle");
    test_assert(brush != nullptr);
    if (brush) {
      test_assert(brushScanMemberWrites(*brush, "color") == MemberWriteKind::Nested);
      EmitResult er = emitCpp(*brush);
      bool reported = false;
      for (const auto &e : er.errors) {
        if (std::strstr(e.c_str(), "swizzle") && std::strstr(e.c_str(), "color")) {
          reported = true;
        }
      }
      if (!reported) {
        std::printf("swizzled attr store was not rejected (%d errors)\n",
                    int(er.errors.size()));
        for (const auto &e : er.errors) {
          std::printf("  %s\n", e.c_str());
        }
      }
      test_assert(reported);
    }
  }

  // --- Gate 3: write/save agreement over every attr-carrying kernel ---
  {
    static const char *kAttrKernels[] = {"bsmooth", "color",        "colorsmooth",
                                         "enhance", "featurealign", "layerdraw",
                                         "polygroup"};
    for (const char *stem : kAttrKernels) {
      auto brush = parseKernel(stem);
      test_assert(brush != nullptr);
      if (!brush) {
        continue;
      }
      EmitResult er = emitCpp(*brush);
      test_assert(er.errors.size() == 0);
      for (const auto &w : er.warnings) {
        std::printf("%s: unexpected codegen warning: %s\n", stem, w.c_str());
      }
      test_assert(er.warnings.size() == 0);
    }
  }
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  runTests();
  return test_end();
}
