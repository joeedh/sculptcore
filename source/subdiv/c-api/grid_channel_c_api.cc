/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "grid_channel_c_api.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"
#include "subdiv/grid_draw_source.h"
#include "subdiv/grids.h"
#include "subdiv/multires.h"

#include <cstring>
#include <span>

using namespace sculptcore;
using sculptcore::subdiv::GridElemDomain;
using sculptcore::subdiv::GridLevelRule;
using sculptcore::subdiv::GridsStore;
using sculptcore::subdiv::Multires;

namespace {

bool validChannel(Multires *mr, int channel)
{
  return mr && channel >= 0 && channel < mr->store.channelCount();
}

bool validLevel(Multires *mr, int level)
{
  return mr && level >= 1 && level <= mr->store.levelCount();
}

/** Floats in one grid's block, or 0 when the (level, channel) pair is bad. */
int gridFloats(Multires *mr, int level, int channel)
{
  if (!validChannel(mr, channel) || !validLevel(mr, level)) {
    return 0;
  }
  return mr->store.channelElemsPerGrid(level, channel) * mr->store.channelElemSize(channel);
}

} // namespace

extern "C" {

int Multires_gridChannelCount(Multires *mr)
{
  return mr ? mr->store.channelCount() : 0;
}

int Multires_gridChannelName(Multires *mr, int index, char *out, int outSize)
{
  if (!validChannel(mr, index)) {
    return -1;
  }
  const litestl::util::string &name = mr->store.channelName(index);
  const int len = int(std::strlen(name.c_str()));
  if (out && outSize > 0) {
    const int copy = len < outSize - 1 ? len : outSize - 1;
    std::memcpy(out, name.c_str(), size_t(copy));
    out[copy] = 0;
  }
  return len;
}

int Multires_gridChannelFind(Multires *mr, const char *name)
{
  if (!mr || !name || !name[0]) {
    return -1;
  }
  return mr->store.findChannel(litestl::util::string(name));
}

int Multires_gridChannelInfo(Multires *mr,
                             int channel,
                             int *floatsPerElem,
                             int *domain,
                             int *type,
                             int *persist,
                             int *levelRule)
{
  if (!validChannel(mr, channel)) {
    return 0;
  }
  GridsStore &store = mr->store;
  if (floatsPerElem) {
    *floatsPerElem = store.channelElemSize(channel);
  }
  if (domain) {
    *domain = int(store.channelDomain(channel));
  }
  if (type) {
    *type = int(store.channelType(channel));
  }
  if (persist) {
    *persist = store.channelPersist(channel) ? 1 : 0;
  }
  if (levelRule) {
    *levelRule = int(store.channelLevelRule(channel));
  }
  return 1;
}

int Multires_gridChannelEnsure(Multires *mr,
                               const char *name,
                               int floatsPerElem,
                               int domain,
                               int type,
                               int persist,
                               int levelRule)
{
  if (!mr || !name || !name[0] || floatsPerElem < 1 || floatsPerElem > 4) {
    return -1;
  }
  GridsStore &store = mr->store;
  const litestl::util::string key(name);
  const int existing = store.findChannel(key);
  if (existing >= 0) {
    if (store.channelElemSize(existing) != floatsPerElem ||
        store.channelDomain(existing) != GridElemDomain(domain))
    {
      return -1;
    }
    store.setChannelPersist(existing, persist != 0);
    return existing;
  }
  return store.addChannel(key,
                          floatsPerElem,
                          GridElemDomain(domain),
                          mesh::AttrType(type),
                          persist != 0,
                          GridLevelRule(levelRule));
}

int Multires_gridChannelRemove(Multires *mr, int channel)
{
  if (!validChannel(mr, channel) || channel == 0) {
    return 0;
  }
  mr->store.removeChannel(channel);
  return 1;
}

int Multires_gridChannelGridFloats(Multires *mr, int level, int channel)
{
  return gridFloats(mr, level, channel);
}

int Multires_gridChannelLevelAllocated(Multires *mr, int level, int channel)
{
  if (!validChannel(mr, channel) || !validLevel(mr, level)) {
    return 0;
  }
  return mr->store.channelLevelAllocated(level, channel) ? 1 : 0;
}

int Multires_gridChannelRead(
    Multires *mr, int level, int channel, int gridStart, int gridCount, float *out, int outFloats)
{
  const int perGrid = gridFloats(mr, level, channel);
  if (!perGrid || !out || gridStart < 0 || gridCount < 1 ||
      gridStart + gridCount > mr->store.gridCount())
  {
    return 0;
  }
  const int total = perGrid * gridCount;
  if (outFloats < total) {
    return 0;
  }
  // Reading must not materialize storage: an Authored channel nothing has
  // written yet has no chunks, and elem() would allocate them.
  if (!mr->store.channelLevelAllocated(level, channel)) {
    std::memset(out, 0, size_t(total) * sizeof(float));
    return total;
  }
  mr->store.ensureLevelResident(level);
  for (int g = 0; g < gridCount; g++) {
    const float *src = mr->store.elem(level, channel, gridStart + g, 0, 0);
    std::memcpy(out + size_t(g) * perGrid, src, size_t(perGrid) * sizeof(float));
  }
  return total;
}

int Multires_gridChannelWrite(Multires *mr,
                              int level,
                              int channel,
                              int gridStart,
                              int gridCount,
                              const float *in,
                              int inFloats)
{
  const int perGrid = gridFloats(mr, level, channel);
  if (!perGrid || !in || gridStart < 0 || gridCount < 1 ||
      gridStart + gridCount > mr->store.gridCount())
  {
    return 0;
  }
  const int total = perGrid * gridCount;
  if (inFloats < total) {
    return 0;
  }
  mr->store.ensureLevelResident(level);
  litestl::util::Vector<int> grids;
  grids.resize(gridCount);
  for (int g = 0; g < gridCount; g++) {
    float *dst = mr->store.elem(level, channel, gridStart + g, 0, 0);
    std::memcpy(dst, in + size_t(g) * perGrid, size_t(perGrid) * sizeof(float));
    grids[g] = gridStart + g;
  }
  // Republish: the draw path reads derived samples, not the store.
  mr->gridAttrs().refreshSamplesFromChannel(
      mr->store.channelName(channel), level, grids.data(), gridCount);
  if (subdiv::GridDrawSource *ds = mr->drawSource()) {
    ds->markGrids(std::span<const int>(grids.data(), grids.size()));
  }
  return total;
}
}
