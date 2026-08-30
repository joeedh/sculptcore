#pragma once

#include <chrono>
#include <cstdio>

namespace sculptcore::debug_app {

/* Wall-clock timing accumulator for one named phase: count + total/min/max so a
 * "random lag" spike shows up as a max far above the average. */
struct PhaseStat {
  long count = 0;
  double total_ms = 0.0;
  double min_ms = 0.0;
  double max_ms = 0.0;

  void add(double ms)
  {
    if (count == 0 || ms < min_ms) {
      min_ms = ms;
    }
    if (count == 0 || ms > max_ms) {
      max_ms = ms;
    }
    total_ms += ms;
    count++;
  }

  void merge(const PhaseStat &o)
  {
    if (o.count == 0) {
      return;
    }
    if (count == 0 || o.min_ms < min_ms) {
      min_ms = o.min_ms;
    }
    if (count == 0 || o.max_ms > max_ms) {
      max_ms = o.max_ms;
    }
    total_ms += o.total_ms;
    count += o.count;
  }

  double avg_ms() const
  {
    return count > 0 ? total_ms / double(count) : 0.0;
  }
};

/* Lightweight wall-clock profiler for the debug app's stroke paths, enabled by
 * --profile. When disabled every method early-outs, so callers can leave the
 * timing calls in unconditionally. Times are wall-clock milliseconds; the GPU
 * dab phase deliberately spans the runOneShot queue-wait (the suspected source
 * of the residual WGSL lag), so a stall shows up directly in dab-gpu max.
 *
 * Per-stroke stats are printed at endStroke(); the session-cumulative table is
 * printed once at app exit via printSummary(). */
struct StrokeProfiler {
  using Clock = std::chrono::steady_clock;

  bool enabled = false;

  static double ms(Clock::time_point a, Clock::time_point b)
  {
    return std::chrono::duration<double, std::milli>(b - a).count();
  }
  static Clock::time_point now()
  {
    return Clock::now();
  }

  /* ---- per-stroke (reset in beginStroke) ---- */
  PhaseStat dab_;     // whole dab()
  PhaseStat dabCpu_;  // host marshal / work-list / target resolve
  PhaseStat dabGpu_;  // GPU submit + queue-wait (runOneShot)
  PhaseStat dabRead_; // readback + node bounds update
  double begin_ms_ = 0.0;
  double end_ms_ = 0.0;
  Clock::time_point strokeStart_;

  /* ---- session cumulative ---- */
  long strokeCount_ = 0;
  PhaseStat allDab_, allDabCpu_, allDabGpu_, allDabRead_;
  PhaseStat strokeWall_, beginWall_, endWall_;

  void beginStroke()
  {
    if (!enabled) {
      return;
    }
    dab_ = dabCpu_ = dabGpu_ = dabRead_ = PhaseStat{};
    begin_ms_ = end_ms_ = 0.0;
    strokeStart_ = now();
  }

  void addBegin(double m)
  {
    if (!enabled) {
      return;
    }
    begin_ms_ = m;
    beginWall_.add(m);
  }

  /* Record one dab. C++ (CPU-only) callers pass the whole dab time as `cpu` and
   * leave gpu/readback zero; the WGSL path passes the three phases separately. */
  void addDab(double cpu, double gpu, double readback)
  {
    if (!enabled) {
      return;
    }
    double total = cpu + gpu + readback;
    dab_.add(total);
    allDab_.add(total);
    if (cpu > 0.0) {
      dabCpu_.add(cpu);
      allDabCpu_.add(cpu);
    }
    if (gpu > 0.0) {
      dabGpu_.add(gpu);
      allDabGpu_.add(gpu);
    }
    if (readback > 0.0) {
      dabRead_.add(readback);
      allDabRead_.add(readback);
    }
  }

  void addEnd(double m)
  {
    if (!enabled) {
      return;
    }
    end_ms_ = m;
    endWall_.add(m);
  }

  void endStroke()
  {
    if (!enabled) {
      return;
    }
    double wall = ms(strokeStart_, now());
    strokeCount_++;
    strokeWall_.add(wall);

    std::printf("[profile] stroke %ld: %.2f ms wall, %ld dabs (begin %.2f, end %.2f)\n",
                strokeCount_,
                wall,
                dab_.count,
                begin_ms_,
                end_ms_);
    if (dab_.count > 0) {
      std::printf("[profile]   dab      avg %.3f  min %.3f  max %.3f  ms\n",
                  dab_.avg_ms(),
                  dab_.min_ms,
                  dab_.max_ms);
      printPhase("    cpu  ", dabCpu_);
      printPhase("    gpu  ", dabGpu_);
      printPhase("    read ", dabRead_);
    }
    std::fflush(stdout);
  }

  void printSummary()
  {
    if (!enabled || strokeCount_ == 0) {
      return;
    }
    std::printf("\n[profile] === session summary: %ld strokes ===\n", strokeCount_);
    printPhase("  stroke ", strokeWall_);
    printPhase("  begin  ", beginWall_);
    printPhase("  end    ", endWall_);
    printPhase("  dab    ", allDab_);
    printPhase("    cpu  ", allDabCpu_);
    printPhase("    gpu  ", allDabGpu_);
    printPhase("    read ", allDabRead_);
    std::fflush(stdout);
  }

private:
  static void printPhase(const char *name, const PhaseStat &s)
  {
    if (s.count == 0) {
      return;
    }
    std::printf("[profile] %s n=%-5ld total %8.2f  avg %7.3f  min %7.3f  "
                "max %7.3f  ms\n",
                name,
                s.count,
                s.total_ms,
                s.avg_ms(),
                s.min_ms,
                s.max_ms);
  }
};

} // namespace sculptcore::debug_app
