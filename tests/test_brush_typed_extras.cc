#include "brush/brush_executor.h"
#include "brush/brush_preparation.h"
#include "brush/c-api/dab_inputs.h"
#include "brush/c-api/stroke_inputs.h"
#include "brush/compiler/emit_cpp.h"
#include "brush/compiler/lexer.h"
#include "brush/compiler/parser.h"
#include "brush/grid_executor.h"
#include "mesh/mesh_shapes.h"
#include "subdiv/multires.h"
#include "test_util.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>
#include <vector>

test_init;
using namespace sculptcore;
using namespace sculptcore::brush;
using props::Prop;
using props::PropError;

extern "C" {
GridStrokeSession *GridStroke_new(subdiv::Multires *, int, Brush *);
void GridStroke_free(GridStrokeSession *);
int GridStroke_begin(GridStrokeSession *);
void GridStroke_end(GridStrokeSession *);
int GridStroke_undo(GridStrokeSession *);
int GridStroke_redo(GridStrokeSession *);
void GridStroke_setMirror(GridStrokeSession *, int);
}

static void compilerBoundaries()
{
  using namespace sculptcore::brush::sbrush;
  auto emit = [](const char *fields) {
    string src = string("@brush(\"limit\") brush Limit { ") + fields +
                 " vertex void apply(inout Vertex v) {} }";
    auto tokens = lex(stringref(src.c_str()), stringref("limit"));
    auto parsed = parse(tokens.tokens, stringref("limit"));
    test_assert(parsed.errors.size() == 0 && parsed.brush);
    return emitCpp(*parsed.brush, CppEmitOptions{true});
  };
  auto full = emit("uniform int a,b,c,d,e,f,g,h,i; uniform bool j;");
  test_assert(full.errors.size() == 0);
  test_assert(std::strstr(full.text.c_str(), "out + 108") != nullptr);
  auto overflow = emit("uniform int a,b,c,d,e,f,g,h,i; uniform bool j,k;");
  test_assert(overflow.errors.size() > 0);
  for (const char *bad : {"uniform int a; uniform int a;",
                          "uniform int a; ctx bool a;",
                          "ctx int a @dynamic;",
                          "ctx int a = 2147483648;",
                          "ctx bool a = 0.5;",
                          "uniform float3 a;",
                          "uniform int a = 1.5;",
                          "uniform int a @range(0.1, 0.9);"})
  {
    test_assert(emit(bad).errors.size() > 0);
  }
}

static void preparedCompilerCapabilities()
{
  using namespace sculptcore::brush::sbrush;
  auto check =
      [](const char *fields, const char *body, bool safe, const char *host = "") {
        string src = string("@brush(\"eligibility\") brush Eligibility { ") + fields +
                     host + " vertex void apply(inout Vertex v) { " + body + " } }";
        auto tokens = lex(stringref(src.c_str()), stringref("eligibility"));
        auto parsed = parse(tokens.tokens, stringref("eligibility"));
        test_assert(parsed.errors.size() == 0 && parsed.brush);
        if (!parsed.brush || parsed.errors.size()) {
          fprintf(stderr, "capability fixture parse failed: %s\n", src.c_str());
          for (const auto &error : parsed.errors)
            fprintf(stderr, "%s\n", error.message.c_str());
          return;
        }
        auto result = emitCpp(*parsed.brush, CppEmitOptions{true});
        test_assert(result.errors.size() == 0);
        const char *expected =
            safe ? "def.preparedScalarSafe = true;" : "def.preparedScalarSafe = false;";
        test_assert(std::strstr(result.text.c_str(), expected) != nullptr);
      };
  check("uniform float radius;", "radius *= 2.0;", false);
  check("uniform int count;", "count += 1;", false);
  check("ctx float3 surfacePos;", "surfacePos = float3(1.0, 0.0, 0.0);", false);
  check("ctx float3 surfaceNo;", "surfaceNo.x = 0.0;", false);
  check("ctx Array<float3, 4> poseCageNow;", "poseCageNow[0] = float3(0.0);", false);
  check("", "v.executor.state.surfacePos = float3(100.0, 0.0, 0.0);", false);
  check("", "for_neighbor (nb in v) { nb.no = float3(0.0); }", false);
  check("", "v.co += float3(unknown(), 0.0, 0.0);", false);
  check("", "v.co += float3(1.0, 0.0, 0.0);", true);
  check("save vertex co, mask;", "v.co.x = 1.0; v.co[1] += 1.0; v.mask = 0.0;", true);
  check("", "v.mask = 0.0;", false);
  check("attr vertex float4 color; save vertex color;", "v.co.x = 1.0;", false);
  check("uniform float radius;",
        "float radius = 1.0; radius += 2.0; v.co *= radius;",
        true);
  check("", "float x = 0.0; for_neighbor (nb in v) { x += nb.no.x; } v.co *= x;", true);
  check("", "v.co += float3(1.0);", true, "host void prepare() {} ");
  check("attr vertex float4 color; save vertex color;", "v.color = float4(0.5);", true);
  check("attr vertex float4 color;", "v.color = float4(0.5);", false);
  check("attr vertex float4 color; save vertex color;",
        "for_neighbor (nb in v) { nb.color = float4(0.5); }",
        false);
  for (bool saved : {false, true}) {
    string src = string("@brush(\"face\") brush FaceTest { attr face int group; ") +
                 (saved ? "save face group; " : "") +
                 "face void apply(inout Face f) { f.group = 3; } }";
    auto tokens = lex(stringref(src.c_str()), stringref("face"));
    auto parsed = parse(tokens.tokens, stringref("face"));
    test_assert(parsed.errors.size() == 0 && parsed.brush);
    auto result = emitCpp(*parsed.brush, CppEmitOptions{true});
    test_assert(result.errors.size() == 0);
    test_assert(std::strstr(result.text.c_str(),
                            saved ? "def.preparedScalarSafe = true;"
                                  : "def.preparedScalarSafe = false;") != nullptr);
  }
  // ctx is a reserved DSL token, so this concrete C++ bundle escape is
  // already rejected by parsing. The emitter also rejects general nested
  // member chains independently of that keyword restriction.
  string escape = "@brush(\"escape\") brush Escape { vertex void apply(inout Vertex v) { "
                  "v.ctx.ctx.surfacePos = float3(0.0); } }";
  auto tokens = lex(stringref(escape.c_str()), stringref("escape"));
  auto parsed = parse(tokens.tokens, stringref("escape"));
  test_assert(parsed.errors.size() != 0);
  fprintf(stderr, "prepared scalar compiler capability guards passed\n");
}

#ifdef SCULPTCORE_TYPED_EXTRA_FIXTURE
static int fixtureTool(const char *name = "TYPEDPROBE")
{
  std::unique_ptr<const binding::types::Enum> values(
      binding::Binder<SculptBrushes>::bind());
  for (const auto &entry : values->items) {
    if (entry.name == string(name))
      return entry.value;
  }
  test_assert(false);
  return -1;
}

template <typename P> static P *property(Brush &brush, const char *name)
{
  auto *base = brush.props.struct_def->lookupLocal(name);
  test_assert(base != nullptr);
  return static_cast<P *>(static_cast<props::detail::PropBaseType *>(base));
}

static void configure(Brush &brush, int tool)
{
  brush.radius = 10;
  brush.writeProps();
  CommandExecutor executor(nullptr, &brush);
  test_assert(executor.queryUniformManifest(tool) == 8);
  test_assert(property<props::Int32Prop>(brush, "typed_count")
                  ->dynamics.configure(props::DeviceType::TILTX, math::BasicMix::ADD, 1));
  test_assert(
      property<props::BoolProp>(brush, "typed_enabled")
          ->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1));
  test_assert(
      property<props::Float32Prop>(brush, "typed_gain")
          ->dynamics.configure(props::DeviceType::TILTY, math::BasicMix::MULTIPLY, 1));
  brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 1);
  brush.pushDeviceInput(int(props::DeviceType::TILTX), 0);
  brush.pushDeviceInput(int(props::DeviceType::TILTY), 0.5f);
}

static double candidateValue(const PreparedBrushScalars &prepared, const char *name)
{
  for (const auto &value : prepared.values()) {
    if (value.name == string(name))
      return value.value;
  }
  test_assert(false);
  return -999;
}

static void preparedExtraCandidates(int tool)
{
  Brush brush, reference;
  CommandExecutor::brush_command command;
  test_assert(CommandExecutor::createDeclarationCommand(SculptBrushes(tool), command));
  PreparedBrushScalars prepared;
  auto run = [&] {
    return prepareBrushScalars(brush,
                               {command.uniforms.data(), command.uniforms.size()},
                               brush.deviceInputCtx,
                               prepared);
  };
  test_assert(run().error == PropError::ERROR_NONE);
  // Six common values, seven probe extras and nine shared executor settings.
  // ENHANCE's two neighborhood settings only belong to kernels declaring them.
  test_assert(prepared.values().size() == 22 && prepared.declarations().size() == 8);
  test_assert(candidateValue(prepared, "typed_count") == 16777217);
  test_assert(candidateValue(prepared, "typed_low") == INT32_MIN);
  test_assert(candidateValue(prepared, "typed_high") == INT32_MAX);
  test_assert(candidateValue(prepared, "typed_limited") == 2);
  test_assert(candidateValue(prepared, "invert") == 0);
  ensureExtraUniformDefaults(reference);
  for (const auto &value : prepared.values()) {
    test_assert(value.name != string("enhance_rings") &&
                value.name != string("enhance_inner"));
    if (value.storeSlot < 0)
      continue;
    const double expected =
        value.type == Prop::FLOAT32 ? double(reference.getNamedFloat(value.storeSlot))
        : value.type == Prop::INT32 ? double(reference.getNamedInt(value.storeSlot))
                                    : double(reference.getNamedBool(value.storeSlot));
    test_assert(value.value == expected);
    props::ScalarDeclaration retained;
    test_assert(!brush.props.struct_def->lookupLocal(value.name));
    test_assert(!brush.props.struct_def->scalarDeclaration(value.name, retained));
  }
  test_assert(brush.namedFloats.size() == 0 && brush.namedInts.size() == 0 &&
              brush.namedBools.size() == 0);
  test_assert(brush.configurationGeneration() == 0);
  test_assert(command.registerProps(*brush.props.struct_def).error ==
              PropError::ERROR_NONE);
  auto *count = property<props::Int32Prop>(brush, "typed_count");
  auto *enabled = property<props::BoolProp>(brush, "typed_enabled");
  int owner = 71;
  count->owner = &owner;
  count->flag = props::PropFlag::READ_ONLY;
  count->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
  count->dynamics.devices[0].curveTable.append(0.2f);
  count->dynamics.devices[0].curveTable.append(0.9f);
  auto *table = count->dynamics.devices[0].curveTable.data();
  count->dynamics.devices[0].curDeviceValue = 0.8f;
  count->dynamics.devices[0].hasDeviceValue = false;
  enabled->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
  brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0.1f);
  brush.setNamedBool(kExtraSlot_typed_static, false);
  test_assert(run().error == PropError::ERROR_NONE);
  test_assert(candidateValue(prepared, "typed_count") == 4529849);
  test_assert(candidateValue(prepared, "typed_enabled") == 0);
  test_assert(candidateValue(prepared, "typed_static") == 0);
  test_assert(!brush.namedInts.initialized(kExtraSlot_typed_count));
  test_assert(!brush.namedBools.initialized(kExtraSlot_typed_enabled));
  test_assert(count->owner == &owner && *count->internal_value() == 16777217);
  test_assert(count->dynamics.devices[0].curveTable.data() == table &&
              count->dynamics.devices[0].curDeviceValue == 0.8f &&
              !count->dynamics.devices[0].hasDeviceValue);
  brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0.49f);
  test_assert(run().error == PropError::ERROR_NONE &&
              candidateValue(prepared, "typed_enabled") == 0);
  brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0.5f);
  test_assert(run().error == PropError::ERROR_NONE &&
              candidateValue(prepared, "typed_enabled") == 1);
  auto *oldValues = prepared.values().data();
  auto generation = brush.configurationGeneration();
  brush.setNamedInt(kExtraSlot_typed_limited,
                    0); // invalid static value after earlier dynamic candidates
  test_assert(run().error == PropError::ERROR_INVALID_VALUE &&
              prepared.values().data() == oldValues);
  test_assert(brush.getNamedInt(kExtraSlot_typed_limited) == 0 && count->owner == &owner);
  test_assert(!brush.namedInts.initialized(kExtraSlot_typed_count));
  test_assert(count->dynamics.devices[0].curveTable.data() == table &&
              *count->internal_value() == 16777217);
  test_assert(brush.configurationGeneration() == generation);
  brush.setNamedInt(kExtraSlot_typed_limited, 1);
  count->flag = props::PropFlag::NONE;
  test_assert(brush.setDynamicSampleChecked("typed_count",
                                            int(Prop::INT32),
                                            int(props::DeviceType::PRESSURE),
                                            0,
                                            3,
                                            0.1f) == 0);
  count->dynamics.devices[0].flag = props::DynamicFlags::DISABLED;
  generation = brush.configurationGeneration();
  test_assert(run().error == PropError::ERROR_INVALID_DYNAMICS &&
              prepared.values().data() == oldValues);
  test_assert(brush.configurationGeneration() == generation &&
              !brush.getNamedBool(kExtraSlot_typed_static));
  test_assert(count->dynamics.devices[0].curveTable.data() == table &&
              count->owner == &owner);
  fprintf(stderr, "typed extra candidate defaults and readonly evaluation passed\n");
}

static void generatedStores(int tool)
{
  {
    Brush empty;
    CommandExecutor::brush_command declaration;
    test_assert(
        CommandExecutor::createDeclarationCommand(SculptBrushes(tool), declaration));
    test_assert(declaration.registerProps(*empty.props.struct_def).error ==
                PropError::ERROR_NONE);
    test_assert(!empty.namedInts.initialized(kExtraSlot_typed_limited));
    test_assert(
        empty.readScalarChecked("typed_limited", int(Prop::INT32), false).status ==
        int(PropError::ERROR_NOT_EXISTS));
    test_assert(empty.writeScalarChecked("typed_limited", int(Prop::INT32), 0) != 0);
    test_assert(!empty.namedInts.initialized(kExtraSlot_typed_limited));
    test_assert(empty.writeScalarChecked("typed_limited", int(Prop::INT32), 1) == 0);
    ensureExtraUniformDefaults(empty);
    test_assert(empty.getNamedInt(kExtraSlot_typed_limited) == 1);
  }
  Brush brush;
  brush.setNamedInt(100, 123);
  brush.setNamedBool(100, false);
  configure(brush, tool);
  test_assert(brush.getNamedInt(kExtraSlot_typed_count) == 16777217);
  test_assert(brush.getNamedInt(kExtraSlot_typed_low) == INT32_MIN);
  test_assert(brush.getNamedInt(kExtraSlot_typed_high) == INT32_MAX);
  test_assert(brush.getNamedBool(kExtraSlot_typed_enabled));
  test_assert(brush.getNamedBool(kExtraSlot_typed_static));
  test_assert(brush.getNamedBool(kExtraSlot_typed_gate));
  test_assert(brush.getNamedInt(100) == 123 && !brush.getNamedBool(100));
  test_assert(kExtraSlot_typed_count == 0 && kExtraSlot_typed_enabled == 0);
  test_assert(kExtraSlot_nudgeProjection == 0 && kExtraSlot_typed_gain == 1);
  test_assert(extraNamedUniformDescriptor(Prop::INT32, 0)->dynamic);
  test_assert(!extraNamedUniformDescriptor(Prop::BOOL, kExtraSlot_typed_static)->dynamic);
  test_assert(!extraNamedUniformDescriptor(Prop::BOOL, kExtraSlot_typed_gate)->dynamic);
  test_assert(!brush.props.struct_def->lookupLocal("typed_gate"));

  test_assert(brush.writeScalarChecked("typed_static", int(Prop::BOOL), 0) == 0);
  test_assert(!brush.getNamedBool(kExtraSlot_typed_static));
  test_assert(brush.props.lookupValue<bool>("typed_static", false));
  ensureExtraUniformDefaults(brush);
  test_assert(!brush.getNamedBool(kExtraSlot_typed_static));
  brush.setNamedBool(kExtraSlot_typed_static, true);
  test_assert(brush.readScalarChecked("typed_static", int(Prop::BOOL), false).value == 1);
  test_assert(brush.writeScalarChecked("typed_high", int(Prop::INT32), 16777219) == 0);
  test_assert(brush.getNamedInt(kExtraSlot_typed_high) == 16777219);
  test_assert(brush.readScalarChecked("typed_high", int(Prop::INT32), false).value ==
              16777219);
  auto generation = brush.configurationGeneration();
  test_assert(brush.writeScalarChecked("typed_limited", int(Prop::INT32), 0) != 0);
  test_assert(brush.configurationGeneration() == generation);
  brush.setNamedInt(kExtraSlot_typed_limited, INT32_MIN);
  test_assert(brush.readScalarChecked("typed_limited", int(Prop::INT32), false).status ==
              int(PropError::ERROR_INVALID_VALUE));
  test_assert(brush.writeScalarChecked("typed_limited", int(Prop::INT32), 1) == 0);
  brush.setNamedInt(kExtraSlot_typed_high, INT32_MAX);

  CommandExecutor executor(nullptr, &brush);
  auto cmd = executor.createCommand(SculptBrushes(tool));
  cmd.loadUniformProps(brush, &brush.deviceInputCtx);
  test_assert(brush.getNamedFloat(kExtraSlot_typed_gain) == 0.125f);
  test_assert(brush.getNamedInt(kExtraSlot_typed_count) == 16777217);
  test_assert(brush.setNamedScalar(Prop::INT32, kExtraSlot_typed_count, 16777219) ==
              PropError::ERROR_NONE);
  test_assert(brush.props.lookupValue<int>("typed_count", -1) == 16777219);
  property<props::Int32Prop>(brush, "typed_count")->flag = props::PropFlag::READ_ONLY;
  test_assert(brush.setNamedScalar(Prop::INT32, kExtraSlot_typed_count, 7) ==
              PropError::ERROR_READ_ONLY);
  test_assert(brush.getNamedInt(kExtraSlot_typed_count) == 16777219);
  property<props::Int32Prop>(brush, "typed_count")->flag = props::PropFlag::NONE;
  brush.setNamedInt(kExtraSlot_typed_count, 16777217);
  brush.pushDeviceInput(int(props::DeviceType::TILTX), 1);
  brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0);
  cmd.loadUniformProps(brush, &brush.deviceInputCtx);
  test_assert(brush.getNamedInt(kExtraSlot_typed_count) == 16777218);
  test_assert(!brush.getNamedBool(kExtraSlot_typed_enabled));
  test_assert(brush.props.lookupValue<int>("typed_count", -1) == 16777217);
  test_assert(brush.props.lookupValue<bool>("typed_enabled", false));
  CommandCtxBase ctx;
  cmd.execHost(ctx, brush);
  test_assert(brush.getNamedInt(kExtraSlot_typed_work) == 16777219);
  test_assert(!brush.getNamedBool(kExtraSlot_typed_gate));
  ensureExtraUniformDefaults(brush);
  test_assert(!brush.getNamedBool(kExtraSlot_typed_gate));

  for (bool enabled : {false, true}) {
    brush.setEvaluatedNamedBool(kExtraSlot_typed_enabled, enabled);
    brush.setEvaluatedNamedBool(kExtraSlot_typed_static, !enabled);
    brush.setEvaluatedNamedInt(kExtraSlot_typed_count, 16777217);
    brush.setEvaluatedNamedInt(kExtraSlot_typed_limited, enabled ? INT32_MAX : INT32_MIN);
    unsigned char packed[120];
    std::memset(packed, 0xcd, sizeof(packed));
    command::packTypedprobeGpuUniforms(brush, packed);
    int32_t count, low, high, limited;
    uint32_t active, fixed;
    float gain;
    std::memcpy(&gain, packed + 72, 4);
    std::memcpy(&count, packed + 76, 4);
    std::memcpy(&active, packed + 80, 4);
    std::memcpy(&low, packed + 84, 4);
    std::memcpy(&high, packed + 88, 4);
    std::memcpy(&fixed, packed + 92, 4);
    std::memcpy(&limited, packed + 96, 4);
    test_assert(gain == 0.125f && count == 16777217);
    test_assert(active == uint32_t(enabled) && fixed == uint32_t(!enabled));
    test_assert(low == INT32_MIN && high == INT32_MAX && limited == (enabled ? 2 : 1));
    for (int i = 0; i < 72; i++) {
      test_assert(packed[i] == 0xcd);
    }
    for (int i = 100; i < 120; i++) {
      test_assert(packed[i] == 0xcd);
    }
  }
  fprintf(stderr,
          "typed extra registry, authored/cache writes and packed bytes passed\n");
}

static void resolvedExecution(int tool, int hostTool)
{
  auto *mesh = mesh::createCube(4, 2.0f);
  {
    spatial::SpatialTree tree(mesh);
    tree.leaf_limit = 32;
    tree.buildAll();
    Brush brush;
    brush.radius = 10;
    brush.strength = 1;
    brush.writeProps();
    brush.setNamedInt(kExtraSlot_typed_limited, 0);
    CommandExecutor executor(&tree, &brush);
    executor.beginStep(false);
    auto run = [&](float3 center = float3(0.0f)) {
      return executor.applyResolvedDab(SculptBrushes(tool), center, float3(0, 0, 1));
    };
    test_assert(run().error == PropError::ERROR_INVALID_VALUE);
    test_assert(!brush.props.struct_def->lookupLocal("typed_count"));
    props::ScalarDeclaration retained;
    test_assert(!brush.props.struct_def->scalarDeclaration("typed_count", retained));
    test_assert(!brush.namedInts.initialized(kExtraSlot_typed_count));
    test_assert(!brush.namedFloats.initialized(kExtraSlot_typed_gain));
    test_assert(brush.strokePathCount == 0 && !mesh->topo_frozen);
    // Valid scalars still cannot authorize a host stage after node selection.
    brush.setNamedInt(kExtraSlot_typed_limited, 1);
    test_assert(
        executor.applyResolvedDab(SculptBrushes(hostTool), float3(0.0f), float3(0, 0, 1))
            .error != PropError::ERROR_NONE);
    test_assert(!brush.props.struct_def->lookupLocal("typed_count"));
    test_assert(!brush.namedInts.initialized(kExtraSlot_typed_count));
    test_assert(executor
                    .applyResolvedDab(SculptBrushes(fixtureTool("TYPEDUNSAFE")),
                                      float3(0.0f),
                                      float3(0, 0, 1))
                    .error != PropError::ERROR_NONE);
    test_assert(!brush.props.struct_def->scalarDeclaration("radius", retained));
    test_assert(brush.radius == 10 && brush.strokePathCount == 0);
    test_assert(run(float3(100)).error == PropError::ERROR_NONE);
    test_assert(brush.strokePathCount == 0 && executor.lastDabNodeCount == 0);
    test_assert(brush.getNamedInt(kExtraSlot_typed_count) == 16777217);
    test_assert(brush.getNamedInt(kExtraSlot_typed_low) == INT32_MIN);
    test_assert(brush.getNamedInt(kExtraSlot_typed_high) == INT32_MAX);
    test_assert(brush.getNamedInt(kExtraSlot_typed_limited) == 1);
    auto *count = property<props::Int32Prop>(brush, "typed_count");
    auto *enabled = property<props::BoolProp>(brush, "typed_enabled");
    auto *gain = property<props::Float32Prop>(brush, "typed_gain");
    count->dynamics.configure(props::DeviceType::TILTX, math::BasicMix::ADD, 1);
    enabled->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
    gain->dynamics.configure(props::DeviceType::TILTY, math::BasicMix::MULTIPLY, 1);
    count->flag = props::PropFlag::READ_ONLY;
    int owner = 5;
    count->owner = &owner;
    enabled->owner = &owner;
    gain->owner = &owner;
    gain->dynamics.devices[0].curveTable.append(0);
    gain->dynamics.devices[0].curveTable.append(1);
    auto *table = gain->dynamics.devices[0].curveTable.data();
    gain->dynamics.devices[0].curDeviceValue = 0.9f;
    gain->dynamics.devices[0].hasDeviceValue = false;
    brush.pushDeviceInput(int(props::DeviceType::TILTY), 0.5f);
    Vector<float3> before;
    for (int v : mesh->v)
      before.append(mesh->v.co[v]);
    for (int dab = 0; dab < 6; dab++) {
      brush.pushDeviceInput(int(props::DeviceType::PRESSURE), dab == 1 ? 0.49f : 0.5f);
      brush.pushDeviceInput(int(props::DeviceType::TILTX), dab == 2 ? 1 : 0);
      test_assert(brush.writeScalarChecked(
                      "typed_static", int(Prop::BOOL), dab == 4 ? 0 : 1) == 0);
      auto generation = brush.configurationGeneration();
      test_assert(run().error == PropError::ERROR_NONE);
      test_assert(brush.configurationGeneration() == generation);
      const float moved = dab == 5 ? 0.375f : dab >= 3 ? 0.25f : 0.125f;
      for (int v : mesh->v)
        test_assert((mesh->v.co[v] - before[v] - float3(moved, 0, 0)).length() < 1e-6f);
      test_assert(*count->internal_value() == 16777217 && *enabled->internal_value());
      test_assert(*gain->internal_value() == 0.25f && count->owner == &owner &&
                  enabled->owner == &owner && gain->owner == &owner);
      test_assert(gain->dynamics.devices[0].curveTable.data() == table &&
                  gain->dynamics.devices[0].curDeviceValue == 0.9f &&
                  !gain->dynamics.devices[0].hasDeviceValue);
    }
    // A later pending disabled stack rejects before any working cache changes.
    test_assert(
        brush.setDynamicSampleChecked(
            "typed_gain", int(Prop::FLOAT32), int(props::DeviceType::TILTY), 0, 3, 0) ==
        0);
    gain->dynamics.devices[0].flag = props::DynamicFlags::DISABLED;
    test_assert(run().error == PropError::ERROR_INVALID_DYNAMICS);
    test_assert(brush.getNamedFloat(kExtraSlot_typed_gain) == 0.125f &&
                brush.strokePathCount == 6);
    for (int v : mesh->v)
      test_assert((mesh->v.co[v] - before[v] - float3(0.375f, 0, 0)).length() < 1e-6f);
    gain->dynamics.devices.clear();
    *gain->internal_value() = 20;
    test_assert(run().error == PropError::ERROR_NONE);
    // Without post-execution updateQueries, the next filter still sees the
    // original [-1,1] bounds and silently drops the entire translated mesh.
    *gain->internal_value() = 0.25f;
    test_assert(run(float3(20, 0, 0)).error == PropError::ERROR_NONE);
    test_assert(executor.lastDabNodeCount > 0);
    for (int v : mesh->v)
      test_assert((mesh->v.co[v] - before[v] - float3(20.625f, 0, 0)).length() < 1e-5f);
    executor.endStep();
  }
  alloc::Delete(mesh);
  fprintf(stderr, "typed extra resolved mesh geometry and atomic publication passed\n");
}

static void resolvedGridExecution(int tool, int hostTool)
{
  auto *cage = mesh::createCube(2, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 2);
    auto *domain = mr.gridDomain(2);
    domain->ensureTree(20);
    Brush brush;
    brush.radius = 10;
    brush.strength = 1;
    brush.writeProps();
    brush.setNamedInt(kExtraSlot_typed_limited, 0);
    GridBrushExecutor ex(domain, &brush);
    ex.beginStep();
    auto run = [&](float3 center = float3(0.0f)) {
      return ex.applyResolvedDab(SculptBrushes(tool), center, float3(0, 0, 1));
    };
    test_assert(run().error == PropError::ERROR_INVALID_VALUE);
    props::ScalarDeclaration retained;
    test_assert(!brush.props.struct_def->lookupLocal("typed_count"));
    test_assert(!brush.props.struct_def->scalarDeclaration("typed_count", retained));
    test_assert(!brush.namedInts.initialized(kExtraSlot_typed_count));
    test_assert(!brush.namedFloats.initialized(kExtraSlot_typed_gain));
    test_assert(ex.isFirstOfStep && ex.stats.dabs == 0 && brush.strokePathCount == 0);
    brush.setNamedInt(kExtraSlot_typed_limited, 1);
    for (int rejected : {hostTool, fixtureTool("TYPEDUNSAFE")}) {
      test_assert(
          ex.applyResolvedDab(SculptBrushes(rejected), float3(0.0f), float3(0, 0, 1))
              .error != PropError::ERROR_NONE);
      test_assert(!brush.props.struct_def->lookupLocal("typed_count"));
      test_assert(!brush.namedInts.initialized(kExtraSlot_typed_count));
    }
    test_assert(run(float3(100)).error == PropError::ERROR_NONE);
    test_assert(brush.strokePathCount == 0 && ex.lastDabMoved().size() == 0);
    test_assert(brush.getNamedInt(kExtraSlot_typed_count) == 16777217);
    test_assert(brush.getNamedInt(kExtraSlot_typed_low) == INT32_MIN);
    test_assert(brush.getNamedInt(kExtraSlot_typed_high) == INT32_MAX);
    test_assert(brush.getNamedInt(kExtraSlot_typed_limited) == 1);
    auto *count = property<props::Int32Prop>(brush, "typed_count");
    auto *enabled = property<props::BoolProp>(brush, "typed_enabled");
    auto *gain = property<props::Float32Prop>(brush, "typed_gain");
    count->dynamics.configure(props::DeviceType::TILTX, math::BasicMix::ADD, 1);
    enabled->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
    gain->dynamics.configure(props::DeviceType::TILTY, math::BasicMix::MULTIPLY, 1);
    int owner = 5;
    count->flag = props::PropFlag::READ_ONLY;
    count->owner = enabled->owner = gain->owner = &owner;
    gain->dynamics.devices[0].curveTable.append(0);
    gain->dynamics.devices[0].curveTable.append(1);
    auto *table = gain->dynamics.devices[0].curveTable.data();
    gain->dynamics.devices[0].curDeviceValue = 0.9f;
    gain->dynamics.devices[0].hasDeviceValue = false;
    brush.pushDeviceInput(int(props::DeviceType::TILTY), 0.5f);
    std::vector<float3> before;
    for (int v = 0; v < domain->vertCount(); v++)
      before.push_back(domain->pos()[v]);
    for (int dab = 0; dab < 6; dab++) {
      brush.pushDeviceInput(int(props::DeviceType::PRESSURE), dab == 1 ? 0.49f : 0.5f);
      brush.pushDeviceInput(int(props::DeviceType::TILTX), dab == 2 ? 1 : 0);
      test_assert(brush.writeScalarChecked(
                      "typed_static", int(Prop::BOOL), dab == 4 ? 0 : 1) == 0);
      auto generation = brush.configurationGeneration();
      test_assert(run().error == PropError::ERROR_NONE);
      test_assert(brush.configurationGeneration() == generation);
      const float moved = dab == 5 ? 0.375f : dab >= 3 ? 0.25f : 0.125f;
      for (int v = 0; v < domain->vertCount(); v++)
        test_assert((domain->pos()[v] - before[v] - float3(moved, 0, 0)).length() <
                    1e-6f);
      test_assert(*count->internal_value() == 16777217 && *enabled->internal_value());
      test_assert(*gain->internal_value() == 0.25f && count->owner == &owner &&
                  enabled->owner == &owner && gain->owner == &owner);
      test_assert(gain->dynamics.devices[0].curveTable.data() == table &&
                  gain->dynamics.devices[0].curDeviceValue == 0.9f &&
                  !gain->dynamics.devices[0].hasDeviceValue);
    }
    test_assert(
        brush.setDynamicSampleChecked(
            "typed_gain", int(Prop::FLOAT32), int(props::DeviceType::TILTY), 0, 3, 0) ==
        0);
    gain->dynamics.devices[0].flag = props::DynamicFlags::DISABLED;
    const auto movedCount = ex.lastDabMoved().size();
    test_assert(run().error == PropError::ERROR_INVALID_DYNAMICS);
    test_assert(brush.getNamedFloat(kExtraSlot_typed_gain) == 0.125f &&
                brush.strokePathCount == 6 && ex.lastDabMoved().size() == movedCount &&
                ex.stats.dabs == 7);
    for (int v = 0; v < domain->vertCount(); v++)
      test_assert((domain->pos()[v] - before[v] - float3(0.375f, 0, 0)).length() < 1e-6f);
    gain->dynamics.devices.clear();
    *gain->internal_value() = 20;
    test_assert(run().error == PropError::ERROR_NONE);
    *gain->internal_value() = 0.25f;
    test_assert(run(float3(20, 0, 0)).error == PropError::ERROR_NONE);
    test_assert(ex.lastDabMoved().size() > 0);
    for (int v = 0; v < domain->vertCount(); v++)
      test_assert((domain->pos()[v] - before[v] - float3(20.625f, 0, 0)).length() <
                  1e-5f);
    ex.endStep();
  }
  alloc::Delete(cage);
  fprintf(stderr, "typed extra resolved grid geometry and atomic publication passed\n");
}

static void gridSeamBounds(int tool)
{
  auto *cage = mesh::createCube(2, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 2);
    auto *domain = mr.gridDomain(2);
    auto *tree = domain->ensureTree(1);
    // Find an interior query whose selected owner has an unqueried seam alias.
    float3 center(0.0f);
    int seam = -1, incident = -1;
    Vector<int> selected;
    for (int q = 0; q < domain->vertCount() && seam < 0; q++) {
      selected.clear();
      tree->query(domain->pos()[q], 0.001f, selected);
      auto hasLeaf = [&](int leaf) {
        for (int li : selected)
          if (li == leaf)
            return true;
        return false;
      };
      for (int li : selected) {
        for (int v : tree->leaves[li].ownedVerts) {
          const auto occs = domain->occurrences(v);
          for (size_t i = 0; i < occs.size(); i += 3) {
            int alias = tree->leafOfGrid[occs[i]];
            if (!hasLeaf(alias)) {
              center = domain->pos()[q];
              seam = v;
              incident = alias;
              break;
            }
          }
          if (seam >= 0)
            break;
        }
        if (seam >= 0)
          break;
      }
    }
    test_assert(seam >= 0 && incident >= 0);
    Brush brush;
    brush.radius = 0.001f;
    brush.strength = 1;
    brush.writeProps();
    subdiv::GridStrokeLog log;
    GridBrushExecutor ex(domain, &brush, &log);
    auto blob = [&]() {
      std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
      mr.store.write(stream);
      return stream.str();
    };
    const auto beforeBlob = blob();
    std::vector<float3> before;
    for (int v = 0; v < domain->vertCount(); v++)
      before.push_back(domain->pos()[v]);
    ex.beginStep();
    test_assert(ex.applyResolvedDab(SculptBrushes(tool), center, float3(0, 0, 1)).error ==
                PropError::ERROR_NONE);
    test_assert((domain->pos()[seam] - before[seam] - float3(20, 0, 0)).length() < 1e-5f);
    auto checkBounds = [&]() {
      std::vector<float3> bounds;
      for (const auto &leaf : tree->leaves) {
        bounds.push_back(leaf.aabb.min);
        bounds.push_back(leaf.aabb.max);
      }
      tree->refreshAllBounds();
      int i = 0;
      for (const auto &leaf : tree->leaves) {
        test_assert((leaf.aabb.min - bounds[i++]).lengthSqr() == 0);
        test_assert((leaf.aabb.max - bounds[i++]).lengthSqr() == 0);
      }
    };
    auto hitsIncident = [&](float3 p) {
      Vector<int> leaves;
      tree->query(p, 0.001f, leaves);
      for (int li : leaves)
        if (li == incident)
          return true;
      return false;
    };
    test_assert(hitsIncident(domain->pos()[seam]));
    checkBounds();
    ex.endStep();
    const auto afterBlob = blob();
    std::vector<float3> after;
    for (int v = 0; v < domain->vertCount(); v++)
      after.push_back(domain->pos()[v]);
    test_assert(log.undo() && blob() == beforeBlob);
    for (int v = 0; v < domain->vertCount(); v++)
      test_assert((domain->pos()[v] - before[v]).lengthSqr() == 0);
    test_assert(!hitsIncident(before[seam] + float3(20, 0, 0)));
    checkBounds();
    test_assert(log.redo() && blob() == afterBlob);
    for (int v = 0; v < domain->vertCount(); v++)
      test_assert((domain->pos()[v] - after[v]).lengthSqr() == 0);
    test_assert(hitsIncident(domain->pos()[seam]));
    checkBounds();
  }
  alloc::Delete(cage);
  fprintf(stderr, "resolved grid unqueried seam bounds and undo passed\n");
}

static std::string workingBytes(Brush &brush)
{
  std::string result;
  auto append = [&](const auto &value) {
    result.append(reinterpret_cast<const char *>(&value), sizeof(value));
  };
  append(brush.radius);
  append(brush.strength);
  append(brush.autosmooth);
  append(brush.planeoff);
  append(brush.spacing);
  append(brush.invert);
  auto store = [&](const auto &values) {
    append(values.size());
    for (int i = 0; i < int(values.size()); i++) {
      append(values.initialized(i));
      append(values.get(i));
    }
  };
  store(brush.namedFloats);
  store(brush.namedInts);
  store(brush.namedBools);
  return result;
}

static void resolvedTypedProgram(int tool, bool grid)
{
  auto *cage = mesh::createCube(2, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 2);
    auto *domain = mr.gridDomain(2);
    spatial::SpatialTree tree(cage);
    tree.buildAll();
    Brush brush;
    brush.radius = 10;
    brush.strength = 1;
    brush.writeProps();
    brush.radius = 0.01f;
    brush.strength = -0.25f;
    brush.invert = true;
    CommandExecutor meshEx(&tree, &brush);
    GridBrushExecutor gridEx(domain, &brush);
    if (grid)
      gridEx.beginStep();
    else
      meshEx.beginStep(false);
    BrushProgram program;
    program.addCommand(tool);
    program.addCommand(tool);
    auto set = [&](int i, const char *name, Prop type, double value) {
      test_assert(program.setCommandScalarChecked(i, name, type, value) ==
                  PropError::ERROR_NONE);
    };
    set(0, "typed_gain", Prop::FLOAT32, 0.125);
    set(1, "typed_gain", Prop::FLOAT32, 0.25);
    test_assert(program.setCommandScalarChecked(99, "typed_count", Prop::INT32, 1) !=
                PropError::ERROR_NONE);
    test_assert(program.setCommandScalarChecked(1, "typed_count", Prop::INT32, 1.5) !=
                PropError::ERROR_NONE);
    test_assert(program.setCommandScalarChecked(1, "typed_enabled", Prop::BOOL, 0.5) !=
                PropError::ERROR_NONE);
    auto run = [&]() {
      return grid ? gridEx.applyResolvedProgram(&program, float3(0.0f), float3(0, 0, 1))
                  : meshEx.applyResolvedProgram(&program, float3(0.0f), float3(0, 0, 1));
    };
    std::vector<float3> before;
    if (grid) {
      for (int v = 0; v < domain->vertCount(); v++)
        before.push_back(domain->pos()[v]);
    } else {
      for (int v : cage->v)
        before.push_back(cage->v.co[v]);
    }
    auto geometry = [&](float offset) {
      for (size_t v = 0; v < before.size(); v++) {
        auto co = grid ? domain->pos()[v] : cage->v.co[v];
        test_assert((co - before[v] - float3(offset, 0, 0)).length() < 1e-6f);
      }
    };
    // Cold command stacks must evaluate without creating authored properties.
    for (int index = 0; index < 2; index++) {
      BrushDynamicsOverride gainStack{"typed_gain", Prop::FLOAT32, {}};
      gainStack.dynamics.configure(
          props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
      gainStack.dynamics.devices[0].responseKind = 1;
      gainStack.dynamics.devices[0].responseLow = 0.5;
      program.commands[index].dynamicsOverrides.append(gainStack);
      BrushDynamicsOverride countStack{"typed_count", Prop::INT32, {}};
      countStack.dynamics.configure(props::DeviceType::TILTX, math::BasicMix::ADD, 1);
      countStack.dynamics.devices[0].curveTable.append(1);
      countStack.dynamics.devices[0].curveTable.append(0);
      program.commands[index].dynamicsOverrides.append(countStack);
      BrushDynamicsOverride boolStack{"typed_enabled", Prop::BOOL, {}};
      boolStack.dynamics.configure(props::DeviceType::TILTY, math::BasicMix::MULTIPLY, 1);
      boolStack.dynamics.devices[0].responseKind = 2;
      boolStack.dynamics.devices[0].responseThreshold = 0.5;
      boolStack.dynamics.devices[0].responseHigh = 1;
      program.commands[index].dynamicsOverrides.append(boolStack);
    }
    brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0.1f);
    brush.pushDeviceInput(int(props::DeviceType::TILTX), 1);
    brush.pushDeviceInput(int(props::DeviceType::TILTY), 0.5f);
    program.commands[1].scalarOverrides.append({"typed_count", Prop::INT32, 16777217.5});
    const auto initialWorking = workingBytes(brush);
    test_assert(run().error == PropError::ERROR_INVALID_VALUE);
    test_assert(workingBytes(brush) == initialWorking && brush.strokePathCount == 0);
    geometry(0);
    program.commands[1].scalarOverrides.pop_back();
    test_assert(run().error == PropError::ERROR_NONE);
    geometry(0.1875f);
    test_assert(workingBytes(brush) == initialWorking && brush.strokePathCount == 1);
    props::ScalarDeclaration declaration;
    test_assert(!brush.props.struct_def->lookupLocal("typed_count"));
    test_assert(!brush.props.struct_def->scalarDeclaration("typed_count", declaration));
    test_assert(brush.namedFloats.size() == 0 && brush.namedInts.size() == 0 &&
                brush.namedBools.size() == 0);
    brush.setNamedFloat(100, 0.75f);
    brush.setNamedInt(100, -7);
    brush.setNamedBool(100, false);
    const auto holes = workingBytes(brush);
    test_assert(!brush.namedFloats.initialized(kExtraSlot_typed_gain) &&
                !brush.namedInts.initialized(kExtraSlot_typed_count) &&
                !brush.namedBools.initialized(kExtraSlot_typed_enabled));
    test_assert(run().error == PropError::ERROR_NONE);
    test_assert(workingBytes(brush) == holes);
    geometry(0.375f);
    brush.pushDeviceInput(int(props::DeviceType::TILTY), 0.49f);
    test_assert(run().error == PropError::ERROR_NONE);
    geometry(0.375f);
    brush.pushDeviceInput(int(props::DeviceType::TILTY), 0.5f);
    brush.pushDeviceInput(int(props::DeviceType::TILTX), 0);
    test_assert(run().error == PropError::ERROR_NONE);
    geometry(0.375f);
    brush.clearDeviceInputs();
    test_assert(run().error == PropError::ERROR_NONE);
    geometry(0.75f);
    test_assert(workingBytes(brush) == holes);
    brush.clearDeviceInputs();
    for (auto &command : program.commands)
      command.dynamicsOverrides.clear();
    test_assert(meshEx.queryUniformManifest(tool) == 8);
    auto *count = property<props::Int32Prop>(brush, "typed_count");
    auto *enabled = property<props::BoolProp>(brush, "typed_enabled");
    auto *gain = property<props::Float32Prop>(brush, "typed_gain");
    auto *staticValue = property<props::BoolProp>(brush, "typed_static");
    auto *radius = property<props::Float32Prop>(brush, "radius");
    count->dynamics.configure(props::DeviceType::TILTX, math::BasicMix::ADD, 1);
    enabled->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
    gain->dynamics.configure(props::DeviceType::TILTY, math::BasicMix::MULTIPLY, 1);
    gain->dynamics.devices[0].curveTable.append(0);
    gain->dynamics.devices[0].curveTable.append(1);
    const auto *table = gain->dynamics.devices[0].curveTable.data();
    gain->dynamics.devices[0].curDeviceValue = 0.9f;
    gain->dynamics.devices[0].hasDeviceValue = false;
    int owner = 17, getterCalls = 0;
    count->owner = enabled->owner = gain->owner = &owner;
    count->flag = props::PropFlag::READ_ONLY;
    staticValue->getter = [&](bool *value, void *) {
      getterCalls++;
      return value;
    };
    *gain->internal_value() = std::numeric_limits<float>::quiet_NaN();
    brush.setEvaluatedNamedFloat(kExtraSlot_typed_gain,
                                 std::bit_cast<float>(uint32_t(0x7fc12345)));
    brush.setEvaluatedNamedInt(kExtraSlot_typed_count, 0);
    brush.setEvaluatedNamedBool(kExtraSlot_typed_enabled, false);
    brush.setNamedBool(kExtraSlot_typed_static, false);
    brush.setNamedInt(kExtraSlot_typed_limited, 1);
    brush.setNamedFloat(100, 0.75f);
    brush.setNamedInt(100, -7);
    brush.setNamedBool(100, false);
    const auto cacheState = workingBytes(brush);
    for (int i = 0; i < 2; i++) {
      set(i, "typed_gain", Prop::FLOAT32, i == 0 ? 0.25 : 0.5);
      set(i, "typed_count", Prop::INT32, 16777217);
      set(i, "typed_enabled", Prop::BOOL, 1);
      set(i, "typed_static", Prop::BOOL, 1);
    }
    brush.pushDeviceInput(int(props::DeviceType::TILTY), 0.5f);
    float offset = 0.75f;
    for (int dab = 0; dab < 5; dab++) {
      brush.pushDeviceInput(int(props::DeviceType::PRESSURE), dab == 0 ? 0.49f : 0.5f);
      brush.pushDeviceInput(int(props::DeviceType::TILTX), dab == 1 ? 1 : 0);
      program.setCommandInvert(1, dab == 3);
      set(1, "typed_static", Prop::BOOL, dab == 4 ? 0 : 1);
      auto generation = brush.configurationGeneration();
      test_assert(run().error == PropError::ERROR_NONE);
      offset += dab < 2 ? 0 : dab == 2 ? 0.375f : 0.125f;
      geometry(offset);
      test_assert(workingBytes(brush) == cacheState &&
                  brush.configurationGeneration() == generation);
      test_assert(std::bit_cast<uint32_t>(brush.getNamedFloat(kExtraSlot_typed_gain)) ==
                  0x7fc12345);
      test_assert(*count->internal_value() == 16777217 && *enabled->internal_value());
      test_assert(std::isnan(*gain->internal_value()) && count->owner == &owner &&
                  enabled->owner == &owner && gain->owner == &owner && getterCalls == 0);
      test_assert(gain->dynamics.devices[0].curveTable.data() == table &&
                  gain->dynamics.devices[0].curDeviceValue == 0.9f &&
                  !gain->dynamics.devices[0].hasDeviceValue);
    }
    set(0, "radius", Prop::FLOAT32, 10);
    set(1, "radius", Prop::FLOAT32, 10);
    radius->getter = [&](float *value, void *) {
      getterCalls++;
      return value;
    };
    test_assert(run().error == PropError::ERROR_INVALID_OWNER && getterCalls == 0);
    test_assert(workingBytes(brush) == cacheState && brush.strokePathCount == 10);
    radius->getter = {};
    test_assert(
        brush.setDynamicSampleChecked(
            "typed_gain", int(Prop::FLOAT32), int(props::DeviceType::TILTY), 0, 3, 0) ==
        0);
    gain->dynamics.devices[0].flag = props::DynamicFlags::DISABLED;
    test_assert(run().error == PropError::ERROR_INVALID_DYNAMICS);
    test_assert(workingBytes(brush) == cacheState && brush.strokePathCount == 10);
    geometry(offset);
    gain->dynamics.devices.clear();
    gain->dynamics.configure(props::DeviceType::TILTY, math::BasicMix::MULTIPLY, 1);
    program.setCommandInvert(1, false);
    set(1, "typed_static", Prop::BOOL, 1);
    test_assert(run().error == PropError::ERROR_NONE);
    offset += 0.375f;
    geometry(offset);
    test_assert(workingBytes(brush) == cacheState);
    program.commands[0].type = SculptBrushes::DRAW;
    program.commands[0].scalarOverrides.clear();
    set(0, "strength", Prop::FLOAT32, 0);
    test_assert(run().error == PropError::ERROR_NONE);
    offset += 0.25f;
    geometry(offset);
    test_assert(workingBytes(brush) == cacheState);
    const auto samples = brush.strokePathCount;
    test_assert(
        brush.setDynamicSampleChecked(
            "typed_gain", int(Prop::FLOAT32), int(props::DeviceType::TILTY), 0, 3, 0) ==
        0);
    auto invalid = run();
    test_assert(invalid.error == PropError::ERROR_INVALID_DYNAMICS &&
                invalid.name == util::string("command[1].typed_gain"));
    test_assert(workingBytes(brush) == cacheState && brush.strokePathCount == samples);
    geometry(offset);
    gain->dynamics.devices.clear();
    auto &stray = brush.props.struct_def->Float32("unrelated", "Unrelated", -1);
    stray.dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
    test_assert(run().error == PropError::ERROR_INVALID_DYNAMICS);
    test_assert(workingBytes(brush) == cacheState && brush.strokePathCount == samples);
    geometry(offset);
    if (grid)
      gridEx.endStep();
    else
      meshEx.endStep();
  }
  alloc::Delete(cage);
  fprintf(stderr,
          "typed extra resolved %s program geometry and exact restoration passed\n",
          grid ? "grid" : "mesh");
}

static void publicInputs(int tool, bool grid, bool programMode, bool batch)
{
  auto *cage = mesh::createCube(2, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 2);
    auto *slot = mr.setActiveLevel(2);
    auto *domain = mr.gridDomain(2);
    Brush brush;
    brush.radius = 10;
    brush.strength = 1;
    brush.writeProps();
    brush.falloff_kind = FalloffKind::Curve;
    for (int i = 0; i < kFalloffCurveSize; i++)
      brush.falloff_curve[i] = float(i) / float(kFalloffCurveSize - 1);
    CommandExecutor ex(slot->tree, &brush);
    test_assert(ex.queryUniformManifest(tool) > 0);
    auto *count = property<props::Int32Prop>(brush, "typed_count");
    auto *enabled = property<props::BoolProp>(brush, "typed_enabled");
    auto *gain = property<props::Float32Prop>(brush, "typed_gain");
    count->dynamics.configure(props::DeviceType::TILTX, math::BasicMix::ADD, 1);
    enabled->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
    gain->dynamics.configure(props::DeviceType::TILTY, math::BasicMix::MULTIPLY, 1);
    BrushProgram program;
    program.addCommand(tool);
    test_assert(program.setCommandScalarChecked(
                    0, "typed_count", Prop::INT32, 16777217) == PropError::ERROR_NONE);
    if (programMode) {
      auto &command = program.commands[0];
      command.dynamicsOverrides.append({"typed_count", Prop::INT32, count->dynamics});
      command.dynamicsOverrides.append({"typed_enabled", Prop::BOOL, enabled->dynamics});
      command.dynamicsOverrides.append({"typed_gain", Prop::FLOAT32, gain->dynamics});
      // A different authored stack must not leak into the explicit command.
      gain->dynamics.devices[0].mixFactor = 0.5f;
    }
    meshlog::MeshLog log;
    log.setActiveMesh(slot->mesh);
    ex.meshLog = &log;
    GridStrokeSession *session = grid ? GridStroke_new(&mr, 2, &brush) : nullptr;
    if (grid) {
      GridStroke_setMirror(session, 1);
      test_assert(GridStroke_begin(session) == 1);
    } else {
      ex.beginStep(false);
    }
    const int vertices = grid ? domain->vertCount() : slot->mesh->v.count;
    Vector<float3> before;
    for (int i = 0; i < vertices; i++)
      before.append(grid ? domain->pos()[i] : slot->mesh->v.co[i]);
    float dabs[28];
    for (int i = 0; i < 4; i++) {
      float row[] = {0, 0, 0, 0, 0, 1, 10};
      std::memcpy(dabs + i * 7, row, sizeof(row));
    }
    float samples[] = {.5f, 0, .5f, 0, 7, 0, .49f, 0, .5f, 0, 7, 0,
                       1,   1, .5f, 0, 7, 0, 0,    0, 0,   0, 0, 0};
    auto runBatch = [&](int n, const float *d, const float *s, int policy = 1) {
      if (grid)
        return programMode ? GridStroke_dabBatchProgramInputs(
                                 session, &program, n, d, 1, s, nullptr, 0, policy)
                           : GridStroke_dabBatchInputs(
                                 session, tool, n, d, 1, s, nullptr, 0, policy);
      return programMode ? MeshStroke_dabBatchProgramInputs(&ex,
                                                            slot->tree,
                                                            slot->mesh,
                                                            &brush,
                                                            &program,
                                                            n,
                                                            d,
                                                            1,
                                                            s,
                                                            1,
                                                            nullptr,
                                                            0,
                                                            policy)
                         : MeshStroke_dabBatchInputs(&ex,
                                                     slot->tree,
                                                     slot->mesh,
                                                     &brush,
                                                     tool,
                                                     n,
                                                     d,
                                                     1,
                                                     s,
                                                     1,
                                                     nullptr,
                                                     0,
                                                     policy);
    };
    auto unchanged = [&]() {
      for (int i = 0; i < vertices; i++)
        test_assert(
            ((grid ? domain->pos()[i] : slot->mesh->v.co[i]) - before[i]).lengthSqr() ==
            0);
    };
    if (programMode) {
      // With scalar overrides removed, stack presence alone must force rejection.
      auto saved = program.commands[0];
      program.commands[0].scalarOverrides.clear();
      program.commands[0].dynamicsOverrides.clear();
      program.commands[0].dynamicsOverrides.append({"radius", Prop::FLOAT32, {}});
      const auto working = workingBytes(brush);
      for (int policy : {0, 2}) {
        program.commands[0].type =
            SculptBrushes(policy == 2 ? int(SculptBrushes::KELVINLET) : tool);
        test_assert(runBatch(0, nullptr, nullptr, policy) == -1);
        test_assert(runBatch(1, dabs, samples, policy) == -1);
        test_assert(workingBytes(brush) == working && brush.strokePathCount == 0);
        unchanged();
      }
      program.commands[0] = saved;
    }
    samples[22] = 16;
    test_assert(runBatch(4, dabs, samples) == -1);
    test_assert(brush.strokePathCount == 0);
    unchanged();
    samples[22] = 0;
    if (batch) {
      test_assert(runBatch(4, dabs, samples) >= 0);
    } else {
      for (int i = 0; i < 4; i++) {
        setDabInputs(brush, 10, 1, samples + i * 6);
        int result;
        if (grid)
          result =
              programMode
                  ? GridStroke_dabProgramResolved(session, &program, 0, 0, 0, 0, 0, 1)
                  : GridStroke_dabResolved(session, tool, 0, 0, 0, 0, 0, 1);
        else
          result = programMode
                       ? MeshStroke_dabProgramResolved(&ex, &program, 0, 0, 0, 0, 0, 1)
                       : MeshStroke_dabResolved(&ex, tool, 0, 0, 0, 0, 0, 1);
        test_assert(result >= 0);
      }
    }
    for (int i = 0; i < vertices; i++)
      test_assert(((grid ? domain->pos()[i] : slot->mesh->v.co[i]) - before[i] -
                   float3(.375f, 0, 0))
                      .length() < 1e-6f);
    test_assert(*count->internal_value() == 16777217 && *enabled->internal_value());
    gain->dynamics.devices[0].curveTable.append(1);
    const float priorRadius = brush.radius;
    const float authoredRadius = brush.props.lookupFloat("radius", -1);
    dabs[6] = 20;
    test_assert(runBatch(1, dabs, samples) == -1);
    test_assert(brush.radius == priorRadius &&
                brush.props.lookupFloat("radius", -1) == authoredRadius);
    test_assert(brush.deviceInputCtx.inputs.size() == 0);
    test_assert(runBatch(0, nullptr, nullptr) == -1);
    dabs[6] = 10;
    gain->dynamics.devices[0].curveTable.clear();
    // A repaired configuration works even after the first dab has advanced.
    float empty[] = {100, 100, 100, 0, 0, 1, 10};
    test_assert(runBatch(1, empty, samples) == 0);
    if (grid) {
      GridStroke_end(session);
      test_assert(GridStroke_undo(session) == 1);
      unchanged();
      test_assert(GridStroke_redo(session) == 1);
      GridStroke_free(session);
    } else {
      ex.endStep();
      log.undo(slot->mesh, slot->tree);
      unchanged();
      log.redo(slot->mesh, slot->tree);
    }
  }
  alloc::Delete(cage);
  fprintf(stderr,
          "public typed inputs %s %s %s geometry and undo passed\n",
          grid ? "grid" : "mesh",
          programMode ? "program" : "standalone",
          batch ? "batch" : "single");
}

static void execution(int tool, bool grid)
{
  auto *cage = mesh::createCube(2, 1.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 2);
    Brush brush;
    configure(brush, tool);
    BrushProgram program;
    program.addCommand(tool);
    auto *domain = grid ? mr.gridDomain(2) : nullptr;
    auto *slot = grid ? nullptr : mr.setActiveLevel(2);
    auto gridExecutor =
        grid ? std::make_unique<GridBrushExecutor>(domain, &brush, nullptr) : nullptr;
    CommandExecutor meshExecutor(grid ? nullptr : slot->tree, &brush);
    if (grid) {
      gridExecutor->beginStep();
    } else {
      meshExecutor.beginStep(false);
    }
    const int count = grid ? domain->vertCount() : slot->mesh->v.count;
    Vector<float3> before;
    for (int i = 0; i < count; i++) {
      before.append(grid ? domain->pos()[i] : slot->mesh->v.co[i]);
    }
    for (int dab = 0; dab < 6; dab++) {
      // Pressure disables dab 1, exact integer comparison disables dab 2, and
      // the checked static boolean write disables dab 4. Dabs 0, 3 and 5 move.
      brush.pushDeviceInput(int(props::DeviceType::PRESSURE), dab == 1 ? 0 : 1);
      brush.pushDeviceInput(int(props::DeviceType::TILTX), dab == 2 ? 1 : 0);
      test_assert(brush.writeScalarChecked(
                      "typed_static", int(Prop::BOOL), dab == 4 ? 0 : 1) == 0);
      if (grid) {
        test_assert(gridExecutor->applyProgram(&program, float3(0.0f), float3(0, 0, 1)) >=
                    0);
      } else {
        Vector<spatial::SpatialNode *> nodes;
        slot->tree->filterNodes(float3(0.0f), brush.radius, nodes);
        test_assert(nodes.size() > 0);
        meshExecutor.execProgram(&program, &nodes, float3(0.0f), float3(0, 0, 1));
        test_assert(meshExecutor.lastUniformValidationOk());
        meshExecutor.clearIsFirstOfStep();
      }
      const float moved = dab == 5 ? 0.375f : dab >= 3 ? 0.25f : 0.125f;
      for (int i = 0; i < count; i++) {
        float3 position = grid ? domain->pos()[i] : slot->mesh->v.co[i];
        test_assert(std::fabs(position[0] - before[i][0] - moved) < 1e-6f);
        test_assert(position[1] == before[i][1] && position[2] == before[i][2]);
      }
      test_assert(brush.props.lookupValue<int>("typed_count", -1) == 16777217);
      test_assert(brush.props.lookupValue<bool>("typed_enabled", false));
    }
    if (grid) {
      gridExecutor->endStep();
    } else {
      meshExecutor.endStep();
    }
  }
  alloc::Delete(cage);
  fprintf(stderr, "typed extra %s program geometry passed\n", grid ? "grid" : "mesh");
}
#endif

#ifdef SCULPTCORE_TYPED_EXTRA_FIXTURE
#include "prepared_extent_fixture.h"
#endif

int main()
{
  compilerBoundaries();
  preparedCompilerCapabilities();
#ifdef SCULPTCORE_TYPED_EXTRA_FIXTURE
  int tool = fixtureTool();
  test_assert(tool >= 0 && fixtureTool("TYPEDRESOLVED") >= 0 &&
              fixtureTool("TYPEDUNSAFE") >= 0 && fixtureTool("SEAMTRANSLATE") >= 0);
  if (tool >= 0) {
    extentStageMatrix(fixtureTool("EXTENTSTAGE"));
    preparedExtraCandidates(tool);
    generatedStores(tool);
    execution(tool, false);
    execution(tool, true);
    resolvedExecution(fixtureTool("TYPEDRESOLVED"), tool);
    resolvedGridExecution(fixtureTool("TYPEDRESOLVED"), tool);
    gridSeamBounds(fixtureTool("SEAMTRANSLATE"));
    resolvedTypedProgram(fixtureTool("TYPEDRESOLVED"), false);
    resolvedTypedProgram(fixtureTool("TYPEDRESOLVED"), true);
    for (bool grid : {false, true})
      for (bool program : {false, true})
        for (bool batch : {false, true})
          publicInputs(fixtureTool("TYPEDRESOLVED"), grid, program, batch);
  }
#else
  fprintf(stderr, "typed extra fixture not compiled; compiler boundaries only\n");
#endif
  return test_end();
}
