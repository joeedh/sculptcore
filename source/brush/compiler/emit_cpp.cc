#include "emit_cpp_internal.h"

namespace sculptcore::brush::sbrush {

using cpp_emit::Emit;

string kernelCppName(const string &attrName, const string &cppName)
{
  string name = attrName.size() ? attrName : cppName;
  for (size_t i = 0; i < name.size(); i++) {
    name[i] = char(i == 0 ? std::toupper((unsigned char)name[i])
                          : std::tolower((unsigned char)name[i]));
  }
  return name;
}

bool isCtxBaseName(const char *n)
{
  return (std::strcmp(n, "mouse") == 0) || (std::strcmp(n, "mousePos") == 0) ||
         (std::strcmp(n, "surfacePos") == 0) || (std::strcmp(n, "surfaceNo") == 0) ||
         (std::strcmp(n, "mouseDir") == 0) || (std::strcmp(n, "renderMatrix") == 0) ||
         (std::strcmp(n, "isFirstOfStep") == 0) || (std::strcmp(n, "meshLog") == 0);
}

static const ::sculptcore::brush::BrushMemberDescriptor *
memberDescriptor(const char *name)
{
  // Permanent: a process-lifetime cache, so it must not read as a leak in a test
  // that calls alloc::print_blocks before static destructors run.
  static Vector<::sculptcore::brush::BrushMemberDescriptor> descriptors = [] {
    litestl::alloc::pushPermanentAlloc();
    Vector<::sculptcore::brush::BrushMemberDescriptor> v;
    ::sculptcore::brush::Brush::builtinPropDescriptors(v);
    litestl::alloc::popPermanentAlloc();
    return v;
  }();
  for (const auto &descriptor : descriptors) {
    if (std::strcmp(descriptor.name, name) == 0) {
      return &descriptor;
    }
  }
  return nullptr;
}

bool isMemberBackedName(const char *name)
{
  return memberDescriptor(name) != nullptr;
}

static props::Prop fieldStorageType(TypeKind type)
{
  switch (type) {
  case TypeKind::Bool:
    return props::Prop::BOOL;
  case TypeKind::Int:
    return props::Prop::INT32;
  case TypeKind::Float:
    return props::Prop::FLOAT32;
  case TypeKind::Float2:
    return props::Prop::VEC2F;
  case TypeKind::Float3:
    return props::Prop::VEC3F;
  case TypeKind::Float4:
    return props::Prop::VEC4F;
  case TypeKind::Array:
    return props::Prop::ARRAYBUFFER;
  default:
    return props::Prop::INVALID_TYPE;
  }
}

bool fieldDynamicCapable(const Field &field)
{
  const auto *member = memberDescriptor(field.name.c_str());
  const bool scalar = field.type == TypeKind::Float ||
                      ((field.type == TypeKind::Bool || field.type == TypeKind::Int) &&
                       field.dynamicExplicit);
  return scalar && field.dynamicCapable && (!member || member->dynamic);
}

static bool reservedWireName(const char *name)
{
  for (const char *reserved : {"falloff_kind",
                               "falloff_shape",
                               "falloff_dir",
                               "falloff_extent",
                               "unbounded_extent",
                               "coord_space",
                               "tex_repeat",
                               "stroke_path_count",
                               "nonaccum",
                               "grab_dab_gen"})
  {
    if (std::strcmp(name, reserved) == 0) {
      return true;
    }
  }
  return false;
}

bool fieldUsesStore(const Field &f)
{
  if (f.kind == FieldKind::Attr) {
    return false;
  }
  if (f.kind == FieldKind::Ctx && isCtxBaseName(f.name.c_str())) {
    return false;
  }
  return !isMemberBackedName(f.name.c_str());
}

namespace cpp_emit {

void Emit::validateGeneratedName(const string &name)
{
  if (name == string("any_moved") || name == string("ctx") || name == string("brush") ||
      std::strncmp(name.c_str(), "sb_", 3) == 0 ||
      std::strncmp(name.c_str(), "__", 2) == 0)
    errf("name '%s' is reserved by C++ code generation", name.c_str());
}

void Emit::validateGeneratedLocals(const Stmt *stmt)
{
  if (!stmt)
    return;
  if (stmt->kind == StmtKind::DeclLocal || stmt->kind == StmtKind::NeighborLoop)
    validateGeneratedName(stmt->name);
  for (const auto &child : stmt->stmts)
    validateGeneratedLocals(child.get());
  validateGeneratedLocals(stmt->thenBranch.get());
  validateGeneratedLocals(stmt->elseBranch.get());
  validateGeneratedLocals(stmt->forInit.get());
  validateGeneratedLocals(stmt->forStep.get());
}

void Emit::validateGeneratedTextureNames(const TextureDef &texture)
{
  for (const auto &param : texture.params)
    validateGeneratedName(param.name);
  for (const auto &param : texture.texParams)
    validateGeneratedName(param.name);
  validateGeneratedLocals(texture.body.get());
}

} // namespace cpp_emit


Vector<string> validateCppFields(const Brush &brush, const CppEmitOptions &opts)
{
  Emit em;
  em.brush = &brush;
  em.extrasMode = opts.extras;
  for (const auto &stage : brush.stages) {
    for (const auto &param : stage.params)
      em.validateGeneratedName(param.name);
    em.validateGeneratedLocals(stage.body.get());
  }
  for (const auto &texture : brush.textures)
    em.validateGeneratedTextureNames(texture);

  // Uniform resolution gate. Built-in kernels: every uniform / non-builtin
  // ctx field must be member-backed (Brush::builtinPropNames) — the honesty
  // tripwire that keeps the list in sync with what kernels read. Extra
  // kernels: unlisted float/int/bool fields use typed named stores.
  Vector<string> names;
  for (const auto &f : brush.fields) {
    em.validateGeneratedName(f.name);
    for (const auto &name : names) {
      if (name == f.name) {
        em.errors.append(string("duplicate field '") + f.name + "'");
      }
    }
    names.append(f.name);
    if (f.kind == FieldKind::Uniform || (opts.extras && fieldUsesStore(f))) {
      const auto type = fieldStorageType(f.type);
      if (type == props::Prop::FLOAT32 || type == props::Prop::INT32 ||
          type == props::Prop::BOOL)
      {
        props::ScalarDeclaration declaration{f.name,
                                             type,
                                             f.hasDefault,
                                             f.defaultValue,
                                             f.hasRange,
                                             f.rangeMin,
                                             f.rangeMax,
                                             fieldDynamicCapable(f)};
        if (props::validateScalarDeclaration(declaration) != props::PropError::ERROR_NONE)
        {
          em.errors.append(string("uniform '") + f.name +
                           "': invalid scalar default or range");
        }
      } else if (f.hasDefault || f.hasRange) {
        em.errors.append(string("uniform '") + f.name +
                         "': numeric metadata requires a scalar type");
      }
    }
    if (f.kind != FieldKind::Attr) {
      const auto *member = memberDescriptor(f.name.c_str());
      if (!member && f.kind == FieldKind::Uniform &&
          (reservedWireName(f.name.c_str()) || isCtxBaseName(f.name.c_str())))
      {
        em.errors.append(string("field '") + f.name +
                         "': reserved execution field cannot be a uniform");
      }
      if (member && (member->type != fieldStorageType(f.type) ||
                     (f.type == TypeKind::Array &&
                      (member->arrayElement != fieldStorageType(f.arrayElem) ||
                       member->arraySize != f.arraySize))))
      {
        em.errors.append(string("field '") + f.name +
                         "': type does not match native Brush member");
      }
      if (f.dynamicExplicit && (f.kind != FieldKind::Uniform ||
                                (f.type != TypeKind::Float && f.type != TypeKind::Int &&
                                 f.type != TypeKind::Bool) ||
                                (member && !member->dynamic)))
      {
        em.errors.append(string("field '") + f.name +
                         "': native semantics do not allow dynamics");
      }
    }
    if (!fieldUsesStore(f)) {
      continue;
    }
    char buf[256];
    if (!opts.extras) {
      std::snprintf(buf,
                    sizeof(buf),
                    "uniform '%s' does not resolve to a Brush member — add the member "
                    "and list it in Brush::builtinPropNames (brush.h)",
                    f.name.c_str());
      em.errors.append(string(buf));
    } else if (f.type != TypeKind::Float && f.type != TypeKind::Int &&
               f.type != TypeKind::Bool)
    {
      std::snprintf(
          buf,
          sizeof(buf),
          "extra kernel uniform '%s': only float/int/bool scalars can use the named "
          "store; a %s uniform needs an existing Brush member",
          f.name.c_str(),
          typeKindName(f.type));
      em.errors.append(string(buf));
    }
  }
  return std::move(em.errors);
}

EmitResult emitCpp(const Brush &brush, const CppEmitOptions &opts)
{
  Emit em;
  em.brush = &brush;
  em.extrasMode = opts.extras;
  em.errors = validateCppFields(brush, opts);
  if (em.errors.size() > 0) {
    EmitResult r;
    r.errors = std::move(em.errors);
    return r;
  }

  for (const auto &st : brush.stages) {
    if (st.kind == StageKind::Vertex) {
      em.vertexStage = &st;
      break;
    }
  }
  for (const auto &st : brush.stages) {
    if (st.kind == StageKind::Face) {
      em.faceStage = &st;
      break;
    }
  }
  if (em.vertexStage && em.vertexStage->params.size() > 0) {
    em.vertexParamName = em.vertexStage->params[0].name;
  } else {
    em.vertexParamName = string("v");
  }
  if (em.faceStage && em.faceStage->params.size() > 0) {
    em.faceParamName = em.faceStage->params[0].name;
  } else {
    em.faceParamName = string("f");
  }
  em.run();
  EmitResult r;
  r.text = std::move(em.out);
  r.errors = std::move(em.errors);
  r.warnings = std::move(em.warnings);
  return r;
}

EmitResult emitCppTextureDefs(const Brush &brush)
{
  Emit em;
  em.brush = &brush;
  for (const auto &td : brush.textures)
    em.validateGeneratedTextureNames(td);
  if (em.errors.size()) {
    EmitResult result;
    result.errors = std::move(em.errors);
    return result;
  }
  for (const auto &td : brush.textures) {
    em.emitTextureBlock(td);
    em.emitTextureManifest(td);
  }
  EmitResult r;
  r.text = std::move(em.out);
  r.errors = std::move(em.errors);
  return r;
}

} // namespace sculptcore::brush::sbrush
