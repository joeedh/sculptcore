#pragma once

#include "props/prop_struct.h"

namespace sculptcore::props {
/* Handles inheritance between parent and child. */
void resolveStruct(StructProp *parent, StructProp *child);
}
