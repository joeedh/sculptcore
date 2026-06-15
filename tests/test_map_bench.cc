/* Microbenchmark for litestl::util::Map<int,int> vs absl::flat_hash_map and
 * std::unordered_map. Not a pass/fail gate — prints ns/op per phase per size and
 * returns 0. Run with `node make.mjs test test_map_bench` (optionally pass sizes
 * as argv, e.g. `... 1000 100000 5000000`).
 *
 * The cross-container helpers are overloaded on litestl::util::Map vs a generic
 * STL-like map so one templated phase body drives all three. */

#include "litestl/platform/time.h"
#include "litestl/util/map.h"

#include "absl/container/flat_hash_map.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

using litestl::time::now_ns;

/* Keep the optimizer from deleting lookups/iteration. */
static volatile uint64_t g_sink = 0;

static inline uint64_t splitmix64(uint64_t &state)
{
  uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

/* ---- cross-container helpers (litestl::util::Map overloads win by partial
 * ordering; the generic template serves absl + std). ----------------------- */

template <class K, class V, int S>
static inline void kv_put(litestl::util::Map<K, V, S> &m, int k, int v)
{
  m[k] = v;
}
template <class M> static inline void kv_put(M &m, int k, int v)
{
  m[k] = v;
}

template <class K, class V, int S>
static inline bool kv_contains(litestl::util::Map<K, V, S> &m, int k)
{
  return m.lookup_ptr(k) != nullptr;
}
template <class M> static inline bool kv_contains(M &m, int k)
{
  return m.find(k) != m.end();
}

template <class K, class V, int S>
static inline void kv_erase(litestl::util::Map<K, V, S> &m, int k)
{
  m.remove(k);
}
template <class M> static inline void kv_erase(M &m, int k)
{
  m.erase(k);
}

template <class K, class V, int S>
static inline uint64_t kv_sum_values(litestl::util::Map<K, V, S> &m)
{
  uint64_t s = 0;
  for (auto &p : m) {
    s += uint64_t(p.value);
  }
  return s;
}
template <class M> static inline uint64_t kv_sum_values(M &m)
{
  uint64_t s = 0;
  for (auto &kv : m) {
    s += uint64_t(kv.second);
  }
  return s;
}

template <class K, class V, int S>
static inline void kv_reserve(litestl::util::Map<K, V, S> &m, size_t n)
{
  m.reserve(n);
}
template <class M> static inline void kv_reserve(M &m, size_t n)
{
  m.reserve(n);
}

/* Repeat count so a phase touches ~2M ops total (steady_clock resolution). */
static inline int reps_for(size_t n)
{
  size_t target = 2'000'000;
  int r = int(target / (n ? n : 1));
  return r < 1 ? 1 : r;
}

struct Keys {
  std::vector<int> present; // n distinct-ish random keys that get inserted
  std::vector<int> absent;  // n keys from a disjoint stream (negative lookups)
};

static Keys make_keys(size_t n, bool sequential)
{
  Keys k;
  k.present.resize(n);
  k.absent.resize(n);
  uint64_t s1 = 0x1234567, s2 = 0xdeadbeefcafe;
  for (size_t i = 0; i < n; i++) {
    k.present[i] = sequential ? int(i) : int(uint32_t(splitmix64(s1)));
    /* High bit set keeps the absent stream disjoint from sequential present. */
    k.absent[i] = int(uint32_t(splitmix64(s2)) | 0x80000000u);
  }
  return k;
}

/* C++ has no portable type name; pass a label per container. */
template <class MakeFn>
static void bench_container(const char *name, size_t n, const Keys &keys, MakeFn make)
{
  const int reps = reps_for(n);
  const double ops = double(n);

  /* insert (no reserve): construct fresh each rep, time only the inserts. */
  uint64_t t_ins = 0;
  for (int r = 0; r < reps; r++) {
    auto m = make();
    uint64_t t0 = now_ns();
    for (size_t i = 0; i < n; i++) {
      kv_put(m, keys.present[i], int(i));
    }
    t_ins += now_ns() - t0;
    g_sink += m.size();
  }

  /* insert (reserved): same, but reserve up front. */
  uint64_t t_ins_res = 0;
  for (int r = 0; r < reps; r++) {
    auto m = make();
    kv_reserve(m, n);
    uint64_t t0 = now_ns();
    for (size_t i = 0; i < n; i++) {
      kv_put(m, keys.present[i], int(i));
    }
    t_ins_res += now_ns() - t0;
    g_sink += m.size();
  }

  /* Build one populated map for the read-only phases. */
  auto m = make();
  kv_reserve(m, n);
  for (size_t i = 0; i < n; i++) {
    kv_put(m, keys.present[i], int(i));
  }

  /* positive lookup. */
  uint64_t t_hit = 0;
  {
    uint64_t hits = 0;
    uint64_t t0 = now_ns();
    for (int r = 0; r < reps; r++) {
      for (size_t i = 0; i < n; i++) {
        hits += kv_contains(m, keys.present[i]);
      }
    }
    t_hit = now_ns() - t0;
    g_sink += hits;
  }

  /* negative lookup. */
  uint64_t t_miss = 0;
  {
    uint64_t hits = 0;
    uint64_t t0 = now_ns();
    for (int r = 0; r < reps; r++) {
      for (size_t i = 0; i < n; i++) {
        hits += kv_contains(m, keys.absent[i]);
      }
    }
    t_miss = now_ns() - t0;
    g_sink += hits;
  }

  /* iterate. */
  uint64_t t_iter = 0;
  {
    uint64_t t0 = now_ns();
    for (int r = 0; r < reps; r++) {
      g_sink += kv_sum_values(m);
    }
    t_iter = now_ns() - t0;
  }

  /* erase: rebuild each rep (outside timing), time erasing all present keys. */
  uint64_t t_erase = 0;
  for (int r = 0; r < reps; r++) {
    auto me = make();
    kv_reserve(me, n);
    for (size_t i = 0; i < n; i++) {
      kv_put(me, keys.present[i], int(i));
    }
    uint64_t t0 = now_ns();
    for (size_t i = 0; i < n; i++) {
      kv_erase(me, keys.present[i]);
    }
    t_erase += now_ns() - t0;
    g_sink += me.size();
  }

  /* churn: a sliding window of ~n/2 live keys; each iter inserts one new key and
   * erases the oldest, so the live set stays bounded. Fixed op budget (not scaled
   * by reps) so it terminates even when the container handles churn poorly. */
  uint64_t t_churn = 0;
  const size_t churn_iters = 100'000; // each iter = 1 insert + 1 erase
  {
    auto mc = make();
    const size_t window = n >= 2 ? n / 2 : 1;
    for (size_t i = 0; i < window; i++) {
      kv_put(mc, keys.present[i % n], int(i));
    }
    uint64_t t0 = now_ns();
    for (size_t i = 0; i < churn_iters; i++) {
      kv_put(mc, keys.present[(window + i) % n], int(i));
      kv_erase(mc, keys.present[i % n]);
    }
    t_churn = now_ns() - t0;
    g_sink += mc.size();
  }
  const double churn_ops = double(churn_iters) * 2.0;

  printf("  %-16s ins %7.1f  insRes %7.1f  hit %7.1f  miss %7.1f  iter %7.1f  "
         "erase %7.1f  churn %7.1f\n",
         name, double(t_ins) / (reps * ops), double(t_ins_res) / (reps * ops),
         double(t_hit) / (reps * ops), double(t_miss) / (reps * ops),
         double(t_iter) / (reps * ops), double(t_erase) / (reps * ops),
         double(t_churn) / churn_ops);
}

static void run_size(size_t n, bool sequential)
{
  printf("N=%zu (%s keys), ns/op:\n", n, sequential ? "sequential" : "random");
  Keys keys = make_keys(n, sequential);

  bench_container(
      "litestl::Map", n, keys, [] { return litestl::util::Map<int, int>(); });
  bench_container(
      "absl::flat_hash", n, keys, [] { return absl::flat_hash_map<int, int>(); });
  bench_container(
      "std::unordered", n, keys, [] { return std::unordered_map<int, int>(); });
  printf("\n");
}

int main(int argc, char **argv)
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  std::vector<size_t> sizes;
  for (int i = 1; i < argc; i++) {
    sizes.push_back(size_t(strtoull(argv[i], nullptr, 10)));
  }
  if (sizes.empty()) {
    sizes = {1'000, 100'000, 1'000'000};
  }

  for (size_t n : sizes) {
    run_size(n, /*sequential=*/false);
    run_size(n, /*sequential=*/true);
  }

  return int(g_sink & 0); // always 0; keeps g_sink live
}
