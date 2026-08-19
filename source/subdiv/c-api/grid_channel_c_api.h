/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** The GridsStore channel surface, for an embedding host.
 *
 * This is the whole interface a host has to a multires-domain attribute: what
 * channels the store carries, what shape each one is, and the bytes of one
 * level of one. Blender is the outlier that cannot store most of them; a host
 * that *can* saves a channel by enumerating, reading each level out at load
 * time, and writing it back after Multires_new — which is the only order that
 * works, because building the store from a cage is destructive (see
 * GridsStore::buildFromCage).
 *
 * Two flags, and they are independent (GridLevelRule in grids.h):
 *
 *   levelRule  0 = Delta (a per-level correction: a new level starts blank and
 *              a dropped level's values go with it), 1 = Authored (the
 *              surface's own value: prolonged up, restricted back down, so an
 *              add/drop round trip is the identity). Everything the engine
 *              does keys off this.
 *   persist    the host's own contract: "I have a container for this". The
 *              engine never branches on it. A host declares it once through
 *              Ensure and reads it back through Info to find the channels it
 *              is responsible for.
 *
 * Levels are 1-based. Channel indices are stable only until a Remove.
 *
 * Element layout of one grid's block is the store's: elemsPerGrid elements,
 * row-major with v the row, floatsPerElem floats each — so a whole grid is
 * contiguous and a grid range is one blit. Grid ids are the cage-corner
 * enumeration everything else in this module uses. */

namespace sculptcore::subdiv {
struct Multires;
}

extern "C" {

/** Channels in the store, or 0. Index 0 is always "disp". */
int Multires_gridChannelCount(sculptcore::subdiv::Multires *mr);

/** Copy channel `index`'s name into `out` (always NUL-terminated when
 * `outSize` > 0). Returns the name's length, so a short buffer is detectable;
 * -1 for a bad index. */
int Multires_gridChannelName(sculptcore::subdiv::Multires *mr,
                             int index,
                             char *out,
                             int outSize);

/** The channel named `name`, or -1. */
int Multires_gridChannelFind(sculptcore::subdiv::Multires *mr, const char *name);

/** Describe `channel` into the caller's out-params (any may be null):
 * floatsPerElem, domain (GridElemDomain), type (mesh::AttrType), persist,
 * levelRule (GridLevelRule). Returns 1, or 0 for a bad channel. */
int Multires_gridChannelInfo(sculptcore::subdiv::Multires *mr,
                             int channel,
                             int *floatsPerElem,
                             int *domain,
                             int *type,
                             int *persist,
                             int *levelRule);

/** Find-or-add `name`, returning its index or -1. An existing channel keeps
 * its width and domain — a mismatch is a failure, not a re-declaration — but
 * DOES take the new `persist`, which is how a host that gains a container for
 * a layer mid-session claims it. */
int Multires_gridChannelEnsure(sculptcore::subdiv::Multires *mr,
                               const char *name,
                               int floatsPerElem,
                               int domain,
                               int type,
                               int persist,
                               int levelRule);

/** Drop `channel` and every level of its data. Refuses channel 0 (disp).
 * Returns 1 on success. Later indices shift down by one. */
int Multires_gridChannelRemove(sculptcore::subdiv::Multires *mr, int channel);

/** Floats in one grid's block at `level` — elemsPerGrid · floatsPerElem, the
 * unit Read/Write move. 0 for a bad channel or level. */
int Multires_gridChannelGridFloats(sculptcore::subdiv::Multires *mr, int level, int channel);

/** Whether `channel` holds anything at `level`. False for an Authored channel
 * nothing has written yet — Read reports it as zeros, and a host may skip
 * saving it entirely. */
int Multires_gridChannelLevelAllocated(sculptcore::subdiv::Multires *mr,
                                       int level,
                                       int channel);

/** Read grids [gridStart, gridStart+gridCount) of `channel` at `level` into
 * `out`, which must hold at least gridCount · GridChannelGridFloats floats.
 * An unallocated level reads back as zeros WITHOUT allocating it. Returns the
 * floats written, or 0. */
int Multires_gridChannelRead(sculptcore::subdiv::Multires *mr,
                             int level,
                             int channel,
                             int gridStart,
                             int gridCount,
                             float *out,
                             int outFloats);

/** The inverse of Read, and the only way a host restores a saved channel.
 * Also republishes what it wrote: the derived draw samples of those grids are
 * refreshed and the draw source marks them, so a write mid-session is visible
 * without a full rebuild. Returns the floats consumed, or 0. */
int Multires_gridChannelWrite(sculptcore::subdiv::Multires *mr,
                              int level,
                              int channel,
                              int gridStart,
                              int gridCount,
                              const float *in,
                              int inFloats);
}
