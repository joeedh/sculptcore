#pragma once

#include "props/prop_dynamics.h"
#include "props/prop_struct.h"

namespace sculptcore::brush {
/** Typed authoring domains and device stacks, independent of kernel units. */
class SemanticScalars {
public:
  int add(int type, double value, double minimum, double maximum, int dynamic);
  int replace(int index, const props::Dynamics &dynamics);
  int evaluate(int count,
               const float *inputs,
               const float *radii,
               int radiusIndex,
               double *output,
               size_t outputCount) const;

private:
  struct Entry {
    props::Prop type;
    props::ScalarDomain domain;
    bool dynamic;
    props::Dynamics dynamics;
  };
  util::Vector<Entry> entries_;
};
} // namespace sculptcore::brush

extern "C" {
sculptcore::brush::SemanticScalars *SemanticScalars_create();
void SemanticScalars_free(sculptcore::brush::SemanticScalars *collection);
int SemanticScalars_add(sculptcore::brush::SemanticScalars *collection,
                        int type,
                        double value,
                        double minimum,
                        double maximum,
                        int dynamic);
int SemanticScalars_replaceDynamics(sculptcore::brush::SemanticScalars *collection,
                                    int index,
                                    util::Vector<int> *devices,
                                    util::Vector<int> *modes,
                                    util::Vector<float> *factors,
                                    util::Vector<int> *enabled,
                                    util::Vector<int> *offsets,
                                    util::Vector<float> *samples,
                                    util::Vector<int> *kinds,
                                    util::Vector<double> *parameters);
int SemanticScalars_evaluate(const sculptcore::brush::SemanticScalars *collection,
                             int count,
                             const float *inputs,
                             const float *radii,
                             int radiusIndex,
                             double *output,
                             size_t outputCount);
}
