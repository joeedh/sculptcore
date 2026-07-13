#pragma once

// CLAUDENOTE: CB-M0 throwaway profiling scaffolding for the meshlog
// callback-batching plan (documentation/plans/2026-07-12-2141-meshlog-
// callback-batching.md). Decomposes the ~36% cb_meshlog share by event
// outcome: per-(kind × outcome) counts, 1-in-64 sampled inside-event timing
// attributed per outcome, sampled stampUndoGate timing, and executor-level
// outside totals (whole std::function dispatch included) so dispatch
// overhead falls out by subtraction. Enabled by debug_app --profile; inert
// otherwise. Ripped out in plan M4.

#include "mesh/mesh_callbacks.h"

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace sculptcore::meshlog::cbprof {

enum Outcome {
  O_CREATED = 0,   /* onCreate (always builds a record) */
  O_FIRST_TOUCH,   /* onChange, no prior record: row + span capture */
  O_NOOP,          /* onChange, already recorded: hash lookup only */
  O_KILL_REC,      /* onKill with a prior record (drop or mark Dead) */
  O_KILL_UNREC,    /* onKill, first touch at kill: row capture */
  O_COUNT
};

inline const char *outcomeName(int o)
{
  static const char *names[O_COUNT] = {"created", "first-touch", "noop",
                                       "kill-rec", "kill-unrec"};
  return names[o];
}

inline const char *kindName(int k)
{
  static const char *names[5] = {"vert", "edge", "corner", "list", "face"};
  return names[k];
}

struct CbProf {
  using Clock = std::chrono::steady_clock;

  bool enabled = false;

  uint64_t counts[5][O_COUNT] = {};
  /* Set by LogChunkTopo::on* so the forward-lambda sampler can attribute its
   * measured event to an outcome after the fact. */
  int last_kind = 0;
  int last_outcome = 0;

  /* 1-in-64 sampled whole-inside-event time (forward lambda body: chunk
   * lookup + hash + capture + gate), per outcome. */
  uint64_t event_serial = 0;
  uint64_t sampled_n[O_COUNT] = {};
  double sampled_ms[O_COUNT] = {};

  /* Sampled stampUndoGate cost (subset of the inside time above). */
  uint64_t gate_sampled_n = 0;
  double gate_sampled_ms = 0.0;

  /* Executor-level outside totals via wrapTimed (includes the std::function
   * dispatch chain the inside timer cannot see). */
  uint64_t outside_calls[2] = {}; /* 0 = meshlog, 1 = spatial */
  double outside_ms[2] = {};

  long dabs = 0;

  static double ms(Clock::time_point a, Clock::time_point b)
  {
    return std::chrono::duration<double, std::milli>(b - a).count();
  }

  bool sampleThisEvent()
  {
    return (event_serial++ & 63) == 0;
  }

  void reset()
  {
    for (int k = 0; k < 5; k++) {
      for (int o = 0; o < O_COUNT; o++) {
        counts[k][o] = 0;
      }
    }
    for (int o = 0; o < O_COUNT; o++) {
      sampled_n[o] = 0;
      sampled_ms[o] = 0.0;
    }
    gate_sampled_n = 0;
    gate_sampled_ms = 0.0;
    outside_calls[0] = outside_calls[1] = 0;
    outside_ms[0] = outside_ms[1] = 0.0;
    event_serial = 0;
    dabs = 0;
  }

  void print(const char *tag) const
  {
    if (!enabled) {
      return;
    }
    double d = dabs > 0 ? double(dabs) : 1.0;
    std::printf("[cb-prof] === %s: %ld dabs ===\n", tag, dabs);
    uint64_t kindTot[5] = {}, outTot[O_COUNT] = {}, all = 0;
    for (int k = 0; k < 5; k++) {
      for (int o = 0; o < O_COUNT; o++) {
        kindTot[k] += counts[k][o];
        outTot[o] += counts[k][o];
        all += counts[k][o];
      }
    }
    std::printf("[cb-prof] events=%llu (%.0f/dab)\n", (unsigned long long)all,
                double(all) / d);
    std::printf("[cb-prof] %-8s %12s %12s %12s %12s %12s %12s\n", "kind", "created",
                "first-touch", "noop", "kill-rec", "kill-unrec", "total");
    for (int k = 0; k < 5; k++) {
      if (kindTot[k] == 0) {
        continue;
      }
      std::printf("[cb-prof] %-8s %12llu %12llu %12llu %12llu %12llu %12llu\n",
                  kindName(k), (unsigned long long)counts[k][0],
                  (unsigned long long)counts[k][1], (unsigned long long)counts[k][2],
                  (unsigned long long)counts[k][3], (unsigned long long)counts[k][4],
                  (unsigned long long)kindTot[k]);
    }
    std::printf("[cb-prof] sampled inside-event cost (1/64), scaled totals:\n");
    double insideTotal = 0.0;
    for (int o = 0; o < O_COUNT; o++) {
      if (sampled_n[o] == 0) {
        continue;
      }
      double mean_us = sampled_ms[o] * 1000.0 / double(sampled_n[o]);
      double est_total = mean_us * double(outTot[o]) / 1000.0;
      insideTotal += est_total;
      std::printf("[cb-prof]   %-12s n=%-9llu mean=%.3fus  est_total=%.1fms "
                  "(share of events %.1f%%)\n",
                  outcomeName(o), (unsigned long long)sampled_n[o], mean_us, est_total,
                  100.0 * double(outTot[o]) / double(all ? all : 1));
    }
    if (gate_sampled_n > 0) {
      double gmean_us = gate_sampled_ms * 1000.0 / double(gate_sampled_n);
      /* Gate fires on vert/face change+create events only; scale by those. */
      uint64_t gateEvents = counts[0][O_CREATED] + counts[0][O_FIRST_TOUCH] +
                            counts[0][O_NOOP] + counts[4][O_CREATED] +
                            counts[4][O_FIRST_TOUCH] + counts[4][O_NOOP];
      std::printf("[cb-prof]   stampUndoGate: mean=%.3fus  est_total=%.1fms\n",
                  gmean_us, gmean_us * double(gateEvents) / 1000.0);
    }
    std::printf("[cb-prof] outside (wrapped) totals: meshlog=%.1fms/%llu calls  "
                "spatial=%.1fms/%llu calls\n",
                outside_ms[0], (unsigned long long)outside_calls[0], outside_ms[1],
                (unsigned long long)outside_calls[1]);
    std::printf("[cb-prof] est dispatch overhead (outside-meshlog minus scaled "
                "inside) = %.1fms\n",
                outside_ms[0] - insideTotal);
    std::fflush(stdout);
  }
};

inline CbProf &get()
{
  static thread_local CbProf p;
  return p;
}

/* Wrap every set callback with an outside timer into outside_ms[slot]. */
inline mesh::MeshCallbacks wrapTimed(const mesh::MeshCallbacks &in, int slot)
{
  mesh::MeshCallbacks out;
  auto wrap = [slot](const litestl::util::function<void(int)> &f)
      -> litestl::util::function<void(int)> {
    if (!f) {
      return f;
    }
    return [slot, f](int id) {
      CbProf &p = get();
      auto t0 = CbProf::Clock::now();
      f(id);
      p.outside_ms[slot] += CbProf::ms(t0, CbProf::Clock::now());
      p.outside_calls[slot]++;
    };
  };
  out.onVertCreate = wrap(in.onVertCreate);
  out.onVertKill = wrap(in.onVertKill);
  out.onVertChange = wrap(in.onVertChange);
  out.onEdgeCreate = wrap(in.onEdgeCreate);
  out.onEdgeKill = wrap(in.onEdgeKill);
  out.onEdgeChange = wrap(in.onEdgeChange);
  out.onCornerCreate = wrap(in.onCornerCreate);
  out.onCornerKill = wrap(in.onCornerKill);
  out.onCornerChange = wrap(in.onCornerChange);
  out.onListCreate = wrap(in.onListCreate);
  out.onListKill = wrap(in.onListKill);
  out.onListChange = wrap(in.onListChange);
  out.onFaceCreate = wrap(in.onFaceCreate);
  out.onFaceKill = wrap(in.onFaceKill);
  out.onFaceChange = wrap(in.onFaceChange);
  return out;
}

} // namespace sculptcore::meshlog::cbprof
