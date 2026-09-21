#include "brush/brush.h"
#include "brush/compiler/emit_cpp.h"
#include "brush/compiler/lexer.h"
#include "brush/compiler/parser.h"
#include "test_util.h"

#include <cstring>

test_init;
using namespace sculptcore::brush::sbrush;

static ParseResult parseField(const char *field)
{
  string source = string("@brush(\"typed_test\") brush TypedTest { ") + field +
                  " vertex void apply(inout Vertex v) {} }";
  auto tokens = lex(stringref(source.c_str()), stringref("typed_test"));
  test_assert(tokens.errors.size() == 0);
  return parse(tokens.tokens, stringref("typed_test"));
}

static void expectEmit(const char *field, const char *error = nullptr, bool extra = false)
{
  auto parsed = parseField(field);
  test_assert(parsed.errors.size() == 0 && parsed.brush);
  if (!parsed.brush) {
    return;
  }
  auto emitted = emitCpp(*parsed.brush, CppEmitOptions{extra});
  if (!error) {
    test_assert(emitted.errors.size() == 0);
    for (const auto &message : emitted.errors) {
      fprintf(stderr, "%s: %s\n", field, message.c_str());
    }
    return;
  }
  bool found = false;
  for (const auto &message : emitted.errors) {
    found |= std::strstr(message.c_str(), error) != nullptr;
  }
  test_assert(found);
  test_assert(emitted.text.size() == 0);
}

static void preludeCase(const char *body, bool safe, bool hostNoop)
{
  string source = string("@brush(\"prelude\") brush Prelude { ") + body + " }";
  auto tokens = lex(stringref(source.c_str()), stringref("prelude"));
  auto parsed = parse(tokens.tokens, stringref("prelude"));
  test_assert(tokens.errors.size() == 0 && parsed.errors.size() == 0 && parsed.brush);
  if (!parsed.brush)
    return;
  auto emitted = emitCpp(*parsed.brush, CppEmitOptions{true});
  for (const auto &error : emitted.errors)
    fprintf(stderr, "prelude %s: %s\n", body, error.c_str());
  test_assert(emitted.errors.size() == 0);
  if (std::strstr(emitted.text.c_str(),
                  safe ? "def.preparedScalarSafe = true;"
                       : "def.preparedScalarSafe = false;") == nullptr)
    fprintf(stderr, "unexpected prepared certificate: %s\n", body);
  test_assert(std::strstr(emitted.text.c_str(),
                          safe ? "def.preparedScalarSafe = true;"
                               : "def.preparedScalarSafe = false;") != nullptr);
  test_assert(std::strstr(emitted.text.c_str(),
                          hostNoop ? "def.preparedHostNoop = true;"
                                   : "def.preparedHostNoop = false;") != nullptr);
}

static void preparedPreludes()
{
  for (const char *body :
       {"reduce void prep(out float any_moved){any_moved=1.0;} "
        "vertex void apply(inout Vertex v,in float any_moved){v.co.x+=any_moved;}",
        "vertex void apply(inout Vertex v){float any_moved=0.0;v.co.x+=1.0;}",
        "vertex void apply(inout Vertex v){for(float "
        "any_moved=0.0;any_moved<1.0;any_moved+=1.0){}}",
        "vertex void apply(inout Vertex v){for_neighbor(__nb_v in v){v.co+=__nb_v.co;}}",
        "vertex void apply(inout Vertex sb_texctx){}",
        "texture T { float eval(float3 p,float3 n){float sb_tex_params=0.0;return "
        "sb_tex_params;} }"
        "vertex void apply(inout Vertex v){}"})
  {
    string source = string("@brush(\"reserved\") brush Reserved { ") + body + " }";
    auto tokens = lex(stringref(source.c_str()), stringref("reserved"));
    auto parsed = parse(tokens.tokens, stringref("reserved"));
    test_assert(tokens.errors.size() == 0 && parsed.errors.size() == 0 && parsed.brush);
    if (!parsed.brush)
      continue;
    auto emitted = emitCpp(*parsed.brush, CppEmitOptions{true});
    test_assert(emitted.text.size() == 0 && emitted.errors.size() > 0);
    bool reserved = false;
    for (const auto &error : emitted.errors)
      reserved |=
          std::strstr(error.c_str(), "reserved by C++ code generation") != nullptr;
    test_assert(reserved);
  }
  preludeCase(
      "uniform float mu @range(1e-6,100.0); uniform float nu @range(0.0,0.499);"
      "host void clampParams() { if (nu > 0.499) nu = 0.499; if(nu < 0.0)nu=0.0;"
      "if(mu < 1e-6)mu=1e-6; } reduce void prep(out float a, out float b) {"
      "a=(1.0+nu)/(2.0*mu); b=a/(4.0*(1.0-nu)); }"
      "vertex void apply(inout Vertex v, in float a, in float b) { v.co.x += a+b; }",
      true,
      true);
  for (const char *host : {"if (mu < 0.1) mu=0.1; if (mu > 0.9) mu=0.9;",
                           "if ((mu) < (0.1)) { (mu)=(0.1); }",
                           ""})
  {
    string body = string("uniform float mu @range(0.1,0.9); host void h() { ") + host +
                  " } vertex void apply(inout Vertex v) {}";
    preludeCase(body.c_str(), true, true);
  }
  for (const char *host : {"if(mu < 0.2)mu=0.2;",
                           "if(mu <= 0.1)mu=0.1;",
                           "if(mu < 0.1)mu=0.2;",
                           "if(mu < 0.1)mu+=0.1;",
                           "if(mu < 0.1)mu=0.1; else mu=0.5;",
                           "if(mu < 0.1){mu=0.1;mu=0.1;}",
                           "mu=0.5;",
                           "if(mu < 0.1)mu=unknown();"})
  {
    string body = string("uniform float mu @range(0.1,0.9); host void h() { ") + host +
                  " } vertex void apply(inout Vertex v) {}";
    preludeCase(body.c_str(), false, false);
  }
  preludeCase("uniform float mu; host void h(){ if(mu<0.0)mu=0.0; }"
              "vertex void apply(inout Vertex v) {}",
              false,
              false);
  preludeCase("uniform float mu @range(0.0,1.0); host void bad(){mu=0.5;}"
              "host void good(){if(mu<0.0)mu=0.0;} vertex void apply(inout Vertex v){}",
              false,
              false);
  preludeCase("uniform float mu @range(0.0,1.0); host void good(){if(mu<0.0)mu=0.0;}"
              "host void bad(){mu=0.5;} vertex void apply(inout Vertex v){}",
              false,
              false);
  preludeCase("uniform float mu @range(0.0,1.0); host void good(){if(mu<0.0)mu=0.0;}"
              "vertex void apply(inout Vertex v){ mu=0.5; }",
              false,
              true);
  preludeCase(
      "uniform float mu; reduce void prep(out float a){ float local=mu+1.0; a=local; }"
      "vertex void apply(inout Vertex v, in float a){}",
      true,
      true);
  for (const char *reduce : {"a=a+1.0;",
                             "a+=1.0;",
                             "float x; a=x;",
                             "float a=1.0;",
                             "float mu=1.0; a=mu;",
                             "a=float(1);",
                             "a=1/0;",
                             "a=unknown();",
                             "a=1.0; mu=2.0;",
                             "{float x=1.0; a=x;}",
                             "a=1.0; return;"})
  {
    string body = string("uniform float mu; reduce void prep(out float a){") + reduce +
                  "} vertex void apply(inout Vertex v, in float a){}";
    preludeCase(body.c_str(), false, true);
  }
  // Floating exceptional arithmetic is not an initialization/state violation.
  preludeCase("reduce void prep(out float a){a=1.0/0.0;}"
              "vertex void apply(inout Vertex v,in float a){}",
              true,
              true);
  preludeCase("reduce void prep(out int a){a=2147483647+1;}"
              "vertex void apply(inout Vertex v,in int a){}",
              false,
              true);
  preludeCase("reduce void prep(in float a,out float b){b=1.0;}"
              "vertex void apply(inout Vertex v,in float a,in float b){}",
              false,
              true);
  preludeCase("reduce void prep(out float a){a=1.0;}"
              "vertex void apply(inout Vertex v,in float a,in float missing){}",
              false,
              true);
  preludeCase("vertex void apply(inout Vertex v,in float missing){}", false, true);
  preludeCase(
      "reduce void good(out float a){a=1.0;} reduce void bad(out float b){b=b+1.0;}"
      "vertex void apply(inout Vertex v,in float a,in float b){}",
      false,
      true);
  preludeCase("reduce void bad(in float a,out float b){b=a;} reduce void good(out float "
              "a){a=1.0;}"
              "vertex void apply(inout Vertex v,in float a,in float b){}",
              false,
              true);
  preludeCase("reduce void good(out float a){a=1.0;} reduce void consumer(in float a,out "
              "float b){b=a;}"
              "vertex void apply(inout Vertex v,in float a,in float b){}",
              false,
              true);
  preludeCase("uniform float mu; reduce void good(out float a){a=1.0;}"
              "vertex void apply(inout Vertex v,in float a){v.co.x=unknown();}",
              false,
              true);
}

int main()
{
  preparedPreludes();
  {
    expectEmit("uniform float projection @static;");
    expectEmit("uniform bool invert = true @dynamic;");
    expectEmit("uniform int activeGroup = 16777217;");
    expectEmit("uniform int activeGroup = 2147483647;");
    expectEmit("uniform int activeGroup = -2147483648;");
    expectEmit("uniform int activeGroup = 2147483648;", "invalid scalar");
    expectEmit("uniform bool invert = 0.5 @dynamic;", "invalid scalar");
    expectEmit("uniform float radius = 1e999;", "invalid scalar");
    expectEmit("uniform float radius = 0.1 @range(0.1, 0.1);", "invalid scalar");
    auto exact =
        parseField("uniform int activeGroup = 16777217 @range(0.1, 2147483647);");
    auto exactCode = emitCpp(*exact.brush);
    test_assert(std::strstr(exactCode.text.c_str(), "16777217.0,") != nullptr);
    test_assert(std::strstr(exactCode.text.c_str(), "Prop::INT32, true") != nullptr);
    auto boolean = parseField("uniform bool invert = true @dynamic;");
    auto boolCode = emitCpp(*boolean.brush);
    test_assert(std::strstr(boolCode.text.c_str(),
                            "lookupValue<bool>(\"invert\", true, ctx)") != nullptr);
    expectEmit("uniform int mixMode;");
    expectEmit("uniform int activeGroup;");
    expectEmit("ctx Array<float3, 4> poseCageRest;");
    expectEmit("uniform bool projection @dynamic;", "type does not match");
    expectEmit("uniform float invert;", "type does not match");
    expectEmit("ctx Array<float3, 3> poseCageRest;", "type does not match");
    expectEmit("ctx Array<float4, 4> poseCageRest;", "type does not match");
    expectEmit("uniform int mixMode @dynamic;", "semantics do not allow");
    expectEmit("uniform int activeGroup @dynamic;", "semantics do not allow");
    expectEmit("uniform float planeSide @dynamic;", "semantics do not allow");
    expectEmit("ctx float3 strokeDir @dynamic;", "semantics do not allow");
    expectEmit("uniform float falloff_kind;", "reserved execution field", true);
    expectEmit("uniform float surfacePos;", "reserved execution field", true);
    expectEmit("uniform float nonaccum;", "reserved execution field", true);
    expectEmit("uniform float grab_dab_gen;", "reserved execution field", true);
    auto side = parseField("uniform float planeSide;");
    test_assert(side.brush != nullptr);
    if (side.brush) {
      auto emitted = emitCpp(*side.brush);
      test_assert(emitted.errors.size() == 0);
      test_assert(std::strstr(emitted.text.c_str(), "\"planeSide\", true, false,") !=
                  nullptr);
      test_assert(std::strstr(emitted.text.c_str(), "sd.Float32(\"planeSide\"") ==
                  nullptr);
      test_assert(std::strstr(emitted.text.c_str(), "lookupValue<float>(\"planeSide\"") ==
                  nullptr);
    }
    for (const char *field : {"uniform float radius @static @dynamic;",
                              "uniform float radius @dynamic @static;"})
    {
      auto parsed = parseField(field);
      test_assert(parsed.errors.size() > 0);
    }
    auto plain = parseField("uniform int count;");
    auto dynamic = parseField("uniform int count @dynamic;");
    test_assert(plain.brush && !plain.brush->fields[0].dynamicExplicit);
    test_assert(dynamic.brush && dynamic.brush->fields[0].dynamicExplicit);
    litestl::util::Vector<sculptcore::brush::BrushMemberDescriptor> members;
    sculptcore::brush::Brush::builtinPropDescriptors(members);
    test_assert(members.size() == 38);
    bool sawBool = false, sawId = false, sawArray = false;
    for (const auto &member : members) {
      if (std::strcmp(member.name, "invert") == 0) {
        sawBool = member.type == sculptcore::props::Prop::BOOL && member.dynamic;
      }
      if (std::strcmp(member.name, "activeGroup") == 0) {
        sawId = member.type == sculptcore::props::Prop::INT32 && !member.dynamic;
      }
      if (std::strcmp(member.name, "poseCageRest") == 0) {
        sawArray = member.type == sculptcore::props::Prop::ARRAYBUFFER &&
                   member.arrayElement == sculptcore::props::Prop::VEC3F &&
                   member.arraySize == 4;
      }
    }
    test_assert(sawBool && sawId && sawArray);
  }
  return test_end();
}
