#include "brush/brush_executor.h"

namespace sculptcore::brush {

  int CommandExecutor::checkedUniformName(int token, int index, int scalarType, string &name) const
  {
    if (token <= 0 || token != uniformQueryToken_ || querySource_ != this || !brush ||
        brush != queryBrush_ || brush->props.struct_def != queryStruct_)
    {
      return int(props::PropError::ERROR_STALE_QUERY);
    }
    if (index < 0 || index >= int(canonicalUniforms_.size())) {
      return int(props::PropError::ERROR_NOT_EXISTS);
    }
    const auto &entry = canonicalUniforms_[index];
    if (scalarType != int(entry.scalarType)) {
      return int(props::PropError::ERROR_INVALID_TYPE);
    }
    name = entry.name;
    return 0;
  }

  BrushUniformManifestEntry CommandExecutor::uniformSnapshotChecked(int token, int uniformIndex) const
  {
    BrushUniformManifestEntry result;
    string name;
    int type = uniformIndex >= 0 && uniformIndex < int(canonicalUniforms_.size())
                   ? int(canonicalUniforms_[uniformIndex].scalarType)
                   : int(props::Prop::INVALID_TYPE);
    result.status = checkedUniformName(token, uniformIndex, type, name);
    if (!result.status) {
      result = canonicalUniforms_[uniformIndex];
    }
    return result;
  }

  bool CommandExecutor::isCommonFloatProp(const string &name)
  {
    return name == string("strength") || name == string("radius") ||
           name == string("spacing") || name == string("planeoff") ||
           name == string("autosmooth");
  }

  const BrushUniformManifestEntry *CommandExecutor::findUniformEntry(brush_command &cmd,
                                                           const string &name)
  {
    for (const auto &u : cmd.uniforms) {
      if (u.name == name)
        return &u;
    }
    return nullptr;
  }

  UniformValidationResult CommandExecutor::validateUniformDynamics(brush_command &cmd)
  {
    UniformValidationResult res;
    char buf[256];

    // (A) Static manifest checks â€” independent of any configured dynamic.
    for (const auto &u : cmd.uniforms) {
      if (!u.hasRange)
        continue;
      if (std::isnan(u.rangeMin) || std::isnan(u.rangeMax) || u.rangeMin > u.rangeMax) {
        res.ok = false;
        snprintf(buf,
                 sizeof(buf),
                 "uniform '%s': invalid @range [%g, %g]",
                 u.name.c_str(),
                 u.rangeMin,
                 u.rangeMax);
        res.messages.append(string(buf));
        continue; // a broken range makes the default check meaningless
      }
      if (u.isFloat && u.hasDefault && (u.def < u.rangeMin || u.def > u.rangeMax)) {
        res.ok = false;
        snprintf(buf,
                 sizeof(buf),
                 "uniform '%s': default %g outside @range [%g, %g]",
                 u.name.c_str(),
                 u.def,
                 u.rangeMin,
                 u.rangeMax);
        res.messages.append(string(buf));
      }
    }

    // (B) Dynamics checks â€” every prop carrying a configured device stack must
    // be a valid, dynamic-capable target of the active brush.
    if (brush && brush->props.struct_def) {
      for (props::Property *p : brush->props.struct_def->properties()) {
        props::Dynamics *dyn = brush->propDynamics(p->name);
        if (!dyn || dyn->devices.size() == 0)
          continue;

        const BrushUniformManifestEntry *entry = findUniformEntry(cmd, p->name);
        if (!entry && !isCommonFloatProp(p->name) && p->name != string("invert")) {
          res.ok = false;
          snprintf(buf,
                   sizeof(buf),
                   "stray dynamic on '%s': not a uniform of the active brush",
                   p->name.c_str());
          res.messages.append(string(buf));
          continue;
        }
        if (entry && !(entry->dynamic &&
                       (entry->isFloat || entry->scalarType == props::Prop::INT32 ||
                        entry->scalarType == props::Prop::BOOL)))
        {
          res.ok = false;
          snprintf(buf,
                   sizeof(buf),
                   "dynamic on '%s': uniform is @static / unsupported (not "
                   "dynamic-capable)",
                   p->name.c_str());
          res.messages.append(string(buf));
          continue;
        }
        if (!props::Dynamics::validStack(dyn->devices)) {
          res.ok = false;
          res.messages.append(string("uniform '") + p->name +
                              "': invalid or pending device stack");
        }
        for (const auto &dev : dyn->devices) {
          if (dev.curveTable.size() == 1) {
            res.ok = false;
            snprintf(buf,
                     sizeof(buf),
                     "uniform '%s': device response curve has 1 entry "
                     "(unbaked; need 0 or >=2)",
                     p->name.c_str());
            res.messages.append(string(buf));
          }
          int dt = (int)dev.type;
          if (dt < 0 || dt > (int)props::DeviceType::TWIST) {
            res.ok = false;
            snprintf(buf,
                     sizeof(buf),
                     "uniform '%s': invalid device type %d",
                     p->name.c_str(),
                     dt);
            res.messages.append(string(buf));
          }
        }
      }
    }

    return res;
  }

  int CommandExecutor::queryUniformManifest(int brushType)
  {
    queriedUniforms.clear();
    canonicalUniforms_.clear();
    querySource_ = this;
    queryBrush_ = nullptr;
    queryStruct_ = nullptr;
    uniformQueryToken_ = allocateUniformQueryToken();
    if (!uniformQueryToken_ || !brush || !brush->props.struct_def) {
      lastRegistration = {props::PropError::ERROR_INVALID_OWNER, "query"};
      return -1;
    }
    brush_command cmd;
    if (!createDeclarationCommand(SculptBrushes(brushType), cmd)) {
      lastRegistration = {props::PropError::ERROR_NOT_EXISTS, "kernel"};
      return -1;
    }
    if (brush && brush->props.struct_def) {
      lastRegistration = cmd.registerProps(*brush->props.struct_def);
      if (!registrationSucceeded()) {
        return -1;
      }
      // Initialize legacy working slots only after successful registration.
      createCommand(SculptBrushes(brushType));
    }
    for (const auto &u : cmd.uniforms) {
      queriedUniforms.append(u);
      canonicalUniforms_.append(u);
    }
    queryBrush_ = brush;
    queryStruct_ = brush->props.struct_def;
    return int(queriedUniforms.size());
  }

  int CommandExecutor::writeUniformScalarChecked(int token, int uniformIndex, int scalarType, double value)
  {
    string name;
    int error = checkedUniformName(token, uniformIndex, scalarType, name);
    if (error) {
      return error;
    }
    return brush->writeScalarChecked(name, scalarType, value);
  }

  int CommandExecutor::configureUniformDynamicChecked(
      int token, int uniformIndex, int scalarType, int device, int mode, float factor)
  {
    string name;
    int error = checkedUniformName(token, uniformIndex, scalarType, name);
    if (error) {
      return error;
    }
    return brush->configureDynamicChecked(name, scalarType, device, mode, factor);
  }

  int CommandExecutor::enableUniformDynamicChecked(
      int token, int uniformIndex, int scalarType, int device, int enabled)
  {
    string name;
    int error = checkedUniformName(token, uniformIndex, scalarType, name);
    if (error) {
      return error;
    }
    return brush->enableDynamicChecked(name, scalarType, device, enabled);
  }

  int CommandExecutor::moveUniformDynamicChecked(
      int token, int uniformIndex, int scalarType, int device, int index)
  {
    string name;
    int error = checkedUniformName(token, uniformIndex, scalarType, name);
    if (error) {
      return error;
    }
    return brush->moveDynamicChecked(name, scalarType, device, index);
  }

  int CommandExecutor::clearUniformDynamicsChecked(int token, int uniformIndex, int scalarType)
  {
    string name;
    int error = checkedUniformName(token, uniformIndex, scalarType, name);
    if (error) {
      return error;
    }
    return brush->clearDynamicsChecked(name, scalarType);
  }

  int CommandExecutor::replaceUniformDynamicTableChecked(int token,
                                        int uniformIndex,
                                        int scalarType,
                                        int device,
                                        util::Vector<float> &samples)
  {
    string name;
    int error = checkedUniformName(token, uniformIndex, scalarType, name);
    if (error) {
      return error;
    }
    return brush->replaceDynamicTableChecked(name, scalarType, device, samples);
  }

  int CommandExecutor::setUniformDynamicSampleChecked(int token,
                                     int uniformIndex,
                                     int scalarType,
                                     int device,
                                     int index,
                                     int count,
                                     float value)
  {
    string name;
    int error = checkedUniformName(token, uniformIndex, scalarType, name);
    if (error) {
      return error;
    }
    return brush->setDynamicSampleChecked(name, scalarType, device, index, count, value);
  }

  int CommandExecutor::replaceUniformDynamicsChecked(int token,
                                    int uniformIndex,
                                    int scalarType,
                                    util::Vector<int> &devices,
                                    util::Vector<int> &modes,
                                    util::Vector<float> &factors,
                                    util::Vector<int> &enabled,
                                    util::Vector<int> &offsets,
                                    util::Vector<float> &samples)
  {
    string name;
    int error = checkedUniformName(token, uniformIndex, scalarType, name);
    if (error) {
      return error;
    }
    return brush->replaceDynamicsChecked(
        name, scalarType, devices, modes, factors, enabled, offsets, samples);
  }

  int CommandExecutor::replaceUniformResponseDynamicsChecked(int token,
                                            int uniformIndex,
                                            int scalarType,
                                            util::Vector<int> &devices,
                                            util::Vector<int> &modes,
                                            util::Vector<float> &factors,
                                            util::Vector<int> &enabled,
                                            util::Vector<int> &offsets,
                                            util::Vector<float> &samples,
                                            util::Vector<int> &kinds,
                                            util::Vector<double> &parameters)
  {
    string name;
    int error = checkedUniformName(token, uniformIndex, scalarType, name);
    if (error) {
      return error;
    }
    return brush->replaceResponseDynamicsChecked(name,
                                                 scalarType,
                                                 devices,
                                                 modes,
                                                 factors,
                                                 enabled,
                                                 offsets,
                                                 samples,
                                                 kinds,
                                                 parameters);
  }

  BrushUniformManifestEntry *CommandExecutor::queriedUniformEntry(int idx)
  {
    if (idx < 0 || idx >= int(queriedUniforms.size())) {
      return nullptr;
    }
    return &queriedUniforms[idx];
  }

  void CommandExecutor::clearUniformDynamics(int idx)
  {
    auto entry = uniformSnapshotChecked(uniformQueryToken(), idx);
    if (entry.status) {
      return;
    }
    brush->clearPropDynamicsByName(entry.name);
  }

  void CommandExecutor::addUniformDynamic(int idx, int deviceType, int mixMode, float mixFactor)
  {
    auto entry = uniformSnapshotChecked(uniformQueryToken(), idx);
    if (entry.status) {
      return;
    }
    brush->addPropDynamicByName(entry.name, deviceType, mixMode, mixFactor);
  }

  void CommandExecutor::setUniformDynamicSample(int idx, int deviceType, int i, int n, float value)
  {
    auto entry = uniformSnapshotChecked(uniformQueryToken(), idx);
    if (entry.status) {
      return;
    }
    brush->setPropDynamicSampleByName(entry.name, deviceType, i, n, value);
  }

} // namespace sculptcore::brush
