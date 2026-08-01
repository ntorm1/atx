// surface_archive_bench.cpp — the serialize/deserialize benchmark for the ATXVSA2
// priced-surface archive (WS-M task M1, class: infra).
//
// The hot-path map found that the backtest deserializes the whole board every step
// (MarketSnapshot::load reconstructs every surface: record CRC + make_unique/slice),
// and yet there was NO benchmark measuring what a serialize or a deserialize actually
// costs. This TU lands that measurement so the scoreboard's Serialize / Deserialize
// ★ratify rows have a concrete µs/surface baseline (feeds M4).
//
// The ATXVSA03 rows this file opened with are GONE with the ATXVSA03 format itself
// (release-v1 plan 3.5/3.6): the writer/reader they called no longer exists. Every
// surviving row measures the SAME operation on v2 bytes and carries an explicit
// `_v2` name, so no row silently reads a v1 baseline number as if it were v2.
//
// ── What is measured ──────────────────────────────────────────────────────────
//
//   serialize_v2 — write_surface_archive_v2(items) in memory (memcpy-bound, no I/O).
//                  Headline: µs/surface (items/s) + partition write MB/s (bytes/s).
//   deserialize  — five modes over a pre-serialized v2 archive; the first three
//                  rebuild OWNED PricedSurfaces, the last two build zero-copy views:
//                  * open_reconstruct_all_v2 : SurfaceArchiveV2::open(copy) +
//                    reconstruct_all_with_provenance() — the full "bytes-in-memory ->
//                    N ready-to-price owned surfaces" whole-board path.
//                  * reconstruct_all_v2      : reconstruct_all_with_provenance() on a
//                    pre-opened archive — isolates per-record reconstruct from open().
//                  * reconstruct_one_v2      : reconstruct_symbol(sym0) on a pre-opened
//                    archive — the SINGLE-surface owned reconstruct, i.e. the baseline
//                    the subset-map row is ratified against. Independent of archive
//                    size (hash probe + one record parse), which the sweep shows.
//                  * mmap_open               : open + map_all() — N zero-copy views.
//                  * subset_map_zero_copy    : map_symbol(sym0) — ONE zero-copy view.
//
// ── Sweep ─────────────────────────────────────────────────────────────────────
//
// Surface-count sweep {1, 4, 16, 50, 100} × payload {Essvi, ConvexDense}. Essvi is
// the light single-name payload (fixed-POD slice records); ConvexDense is the heavy
// index/SPY payload (a variable-length node array per slice) — the two extremes of
// per-surface payload weight the sweep spans. Fixtures are built ONCE per
// (payload, count) in a process-lifetime cache (the bench-suite convention) OUTSIDE
// the timed loop, and self-verify their round-trip before any timing is taken.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/american.hpp"        // al_fast_opts, AmericanMethod, AlOpts
#include "atx/vol/black76.hpp"         // black76_price (ConvexDense node prices)
#include "atx/vol/dense_slice.hpp"     // ConvexSliceFit
#include "atx/vol/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/surface_archive.hpp" // write_surface_archive_v2, SurfaceArchiveV2
#include "atx/vol/surface_parity.hpp"  // SliceContext
#include "atx/vol/types.hpp"           // Side
#include "atx/vol/vol_curve.hpp"       // CurveSurface, EssviCurve, ConvexDenseCurve
#include "atx/vol/vol_surface.hpp"     // EssviParams

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

constexpr double kS = 100.0;
constexpr double kR = 0.043;
constexpr std::int64_t kNowTs = 1700000000000000000LL;

// A fitted board's shape. 6 ascending-T slices is a typical listed front (the
// backtest synthetic grid uses 7); 40 ConvexDense nodes/slice is a realistic dense
// strike ladder. These fix the per-surface byte size; the count sweep varies how
// many such surfaces one partition holds.
constexpr int kSlices = 6;
constexpr int kConvexNodes = 40;

enum class Payload : std::uint8_t { Essvi = 0, ConvexDense = 1 };

// Deserialize modes. All five run on ATXVSA2 bytes: the first three rebuild OWNED
// PricedSurfaces, the last two build borrowed zero-copy views.
enum class DeserMode : std::uint8_t {
  OpenReconstructAll = 0, // open(copy) + reconstruct_all_with_provenance (owned)
  ReconstructAll = 1,     // reconstruct_all_with_provenance on a pre-opened archive
  ReconstructOne = 2,     // reconstruct_symbol(sym0) on a pre-opened archive (owned)
  MmapOpen = 3,           // open + map_all: N zero-copy views, no per-surface alloc
  SubsetMapZeroCopy = 4,  // PricedSurfaceView over ONE symbol's mapped bytes
};

[[nodiscard]] const char *payload_name(Payload p) noexcept {
  return p == Payload::Essvi ? "essvi" : "convexdense";
}

// Setup failures happen once, outside the timed region (static fixture init), so an
// abort is the honest response — a benchmark of an error return measures nothing.
[[noreturn]] void bench_fatal(const std::string &msg) {
  std::fprintf(stderr, "FATAL(surface-archive-bench): %s\n", msg.c_str());
  std::abort();
}

// ── Surface builders (mirror surface_archive_test.cpp's make_essvi/make_convex) ──

[[nodiscard]] PricingContext make_pricing(std::uint32_t uid) {
  PricingContext pc;
  pc.S = kS;
  pc.r = kR;
  pc.now_ts_ns = kNowTs;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return pc;
}

[[nodiscard]] PricedSurface make_essvi_surface(std::uint32_t uid) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < kSlices; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kS;
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i);
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = F;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, 250, 7});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  if (!ps.has_value()) {
    bench_fatal(ps.error().to_string());
  }
  return std::move(*ps);
}

[[nodiscard]] PricedSurface make_convex_surface(std::uint32_t uid) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < kSlices; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    const double F = kS;
    const double df = std::exp(-kR * T);
    const double sigma = 0.20 + 0.01 * static_cast<double>(i);
    ConvexSliceFit fit;
    fit.T = T;
    fit.F = F;
    fit.df = df;
    fit.rmse_price = 0.25;
    fit.n_obs = static_cast<std::size_t>(kConvexNodes);
    fit.n_active = 3;
    fit.u.resize(static_cast<std::size_t>(kConvexNodes));
    fit.C.resize(static_cast<std::size_t>(kConvexNodes));
    for (int j = 0; j < kConvexNodes; ++j) {
      const double K =
          F * (0.7 + 0.6 * static_cast<double>(j) / static_cast<double>(kConvexNodes - 1));
      fit.u[static_cast<std::size_t>(j)] = K;
      fit.C[static_cast<std::size_t>(j)] = black76_price(F, K, T, sigma, df, Side::Call);
    }
    cs.push(std::make_unique<ConvexDenseCurve>(std::move(fit)));
    ctx.push_back(SliceContext{T, F, 0.0, 0.02, static_cast<std::size_t>(kConvexNodes), 2});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  if (!ps.has_value()) {
    bench_fatal(ps.error().to_string());
  }
  return std::move(*ps);
}

// ── Per-(payload,count) fixture ──────────────────────────────────────────────
//
// Owns the `count` distinct surfaces (distinct uids/symbols), the item list that
// references them, the once-serialized archive bytes, and one pre-opened archive
// for the reconstruct-from-opened deserialize modes. Built once, self-verified.

struct Fixture {
  std::vector<PricedSurface> surfaces; // owns the reconstruction inputs
  std::vector<std::string> symbols;    // stable storage backing the item symbol_views
  std::vector<SurfaceArchiveItem> items;
  std::vector<std::byte> bytes_v2;            // the serialized ATXVSA2 partition
  std::unique_ptr<SurfaceArchiveV2> opened_v2; // pre-opened, for the from-opened modes
  std::size_t count{0};
};

[[nodiscard]] Fixture build_fixture(Payload payload, int count) {
  Fixture fx;
  fx.count = static_cast<std::size_t>(count);
  fx.surfaces.reserve(fx.count);
  fx.symbols.reserve(fx.count);
  for (int i = 0; i < count; ++i) {
    const auto uid = static_cast<std::uint32_t>(1000 + i);
    fx.surfaces.push_back(payload == Payload::Essvi ? make_essvi_surface(uid)
                                                    : make_convex_surface(uid));
    fx.symbols.push_back("SYM" + std::to_string(i));
  }
  fx.items.reserve(fx.count);
  for (std::size_t i = 0; i < fx.count; ++i) {
    SurfaceArchiveItem item;
    item.symbol = fx.symbols[i];
    item.surface = &fx.surfaces[i];
    fx.items.push_back(item);
  }

  // Serialize the items to ATXVSA2 and pre-open one archive, self-verifying the
  // round-trip before any timing is taken (the bench's testable invariant, §3 TDD):
  // a fresh open must expose exactly `count` surfaces, resolve sym0 as a zero-copy
  // view, and rebuild it as an owned surface.
  {
    Result<std::vector<std::byte>> serialized = write_surface_archive_v2(fx.items);
    if (!serialized.has_value()) {
      bench_fatal("v2 serialize failed: " + serialized.error().to_string());
    }
    fx.bytes_v2 = std::move(*serialized);
    std::vector<std::byte> copy = fx.bytes_v2;
    Result<SurfaceArchiveV2> arch = SurfaceArchiveV2::open(std::move(copy));
    if (!arch.has_value()) {
      bench_fatal("v2 round-trip open failed: " + arch.error().to_string());
    }
    if (arch->count() != fx.count) {
      bench_fatal("v2 round-trip surface-count mismatch");
    }
    Result<PricedSurfaceView> one = arch->map_symbol(fx.symbols.front());
    if (!one.has_value()) {
      bench_fatal("v2 round-trip map_symbol failed: " + one.error().to_string());
    }
    Result<PricedSurface> owned = arch->reconstruct_symbol(fx.symbols.front());
    if (!owned.has_value()) {
      bench_fatal("v2 round-trip reconstruct_symbol failed: " + owned.error().to_string());
    }
    fx.opened_v2 = std::make_unique<SurfaceArchiveV2>(std::move(*arch));
  }
  return fx;
}

// Process-lifetime fixture cache. Google Benchmark runs registrations sequentially,
// so a lazily-populated map needs no synchronization.
[[nodiscard]] const Fixture &fixture(Payload payload, int count) {
  static std::map<std::pair<std::uint8_t, int>, Fixture> cache;
  const auto key = std::make_pair(static_cast<std::uint8_t>(payload), count);
  auto it = cache.find(key);
  if (it == cache.end()) {
    it = cache.emplace(key, build_fixture(payload, count)).first;
  }
  return it->second;
}

// ── Serialize ────────────────────────────────────────────────────────────────

// The v2 serialize row (write_surface_archive_v2, memcpy-bound): µs/surface +
// partition write MB/s. v2 packs surfaces on 64 B (no 4096-B blob pad), so
// bytes_per_surface is the padding amplification the format removed.
//
// items/s == surfaces/s (µs/surface = 1e6 / surfaces_per_second). bytes/s is the
// partition write throughput (SetBytesProcessed => Google Benchmark bytes_per_second).
void run_serialize_v2(benchmark::State &state, Payload payload, int count) {
  const Fixture &fx = fixture(payload, count);
  for (auto _ : state) {
    Result<std::vector<std::byte>> out = write_surface_archive_v2(fx.items);
    if (!out.has_value()) {
      state.SkipWithError(out.error().to_string().c_str());
      break;
    }
    benchmark::DoNotOptimize(out->data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(fx.bytes_v2.size()));
  state.counters["surfaces"] = static_cast<double>(count);
  state.counters["archive_bytes"] = static_cast<double>(fx.bytes_v2.size());
  state.counters["bytes_per_surface"] =
      static_cast<double>(fx.bytes_v2.size()) / static_cast<double>(count);
}

// ── Deserialize ──────────────────────────────────────────────────────────────

void run_deserialize(benchmark::State &state, Payload payload, int count, DeserMode mode) {
  const Fixture &fx = fixture(payload, count);
  const SurfaceArchiveV2 &opened = *fx.opened_v2;

  std::size_t surfaces_per_iter = 0;
  for (auto _ : state) {
    switch (mode) {
    case DeserMode::OpenReconstructAll: {
      // The full source-buffer -> ready-to-price path. open() consumes its buffer, so
      // copy inline (a few-KB memcpy, dwarfed by the reconstruct at count>=4). Kept in
      // the timed region rather than PauseTiming/ResumeTiming, whose per-iteration
      // overhead is a known antipattern at the µs scale of the small-count rows; the
      // clean per-record reconstruct number is `reconstruct_all_v2` (pre-opened, no copy).
      Result<SurfaceArchiveV2> arch = SurfaceArchiveV2::open(std::vector<std::byte>(fx.bytes_v2));
      if (!arch.has_value()) {
        state.SkipWithError(arch.error().to_string().c_str());
        return;
      }
      Result<std::vector<ArchivedSurface>> all = arch->reconstruct_all_with_provenance();
      if (!all.has_value()) {
        state.SkipWithError(all.error().to_string().c_str());
        return;
      }
      surfaces_per_iter = all->size();
      benchmark::DoNotOptimize(all->data());
      break;
    }
    case DeserMode::ReconstructAll: {
      Result<std::vector<ArchivedSurface>> all = opened.reconstruct_all_with_provenance();
      if (!all.has_value()) {
        state.SkipWithError(all.error().to_string().c_str());
        return;
      }
      surfaces_per_iter = all->size();
      benchmark::DoNotOptimize(all->data());
      break;
    }
    case DeserMode::ReconstructOne: {
      Result<PricedSurface> one = opened.reconstruct_symbol(fx.symbols.front());
      if (!one.has_value()) {
        state.SkipWithError(one.error().to_string().c_str());
        return;
      }
      surfaces_per_iter = 1;
      benchmark::DoNotOptimize(one->n_slices());
      break;
    }
    case DeserMode::MmapOpen: {
      // v2 analogue of open_reconstruct_all: bytes -> N ready-to-price zero-copy
      // views (SurfaceArchiveV2::open + map_all). No per-surface reconstruct;
      // parametric surfaces allocate nothing, the heavy kinds materialize once.
      Result<SurfaceArchiveV2> arch = SurfaceArchiveV2::open(std::vector<std::byte>(fx.bytes_v2));
      if (!arch.has_value()) {
        state.SkipWithError(arch.error().to_string().c_str());
        return;
      }
      Result<std::vector<PricedSurfaceView>> all = arch->map_all();
      if (!all.has_value()) {
        state.SkipWithError(all.error().to_string().c_str());
        return;
      }
      surfaces_per_iter = all->size();
      benchmark::DoNotOptimize(all->data());
      break;
    }
    case DeserMode::SubsetMapZeroCopy: {
      // v2 analogue of reconstruct_one: map ONE symbol on a pre-opened archive —
      // an O(1) hash-probe + a PricedSurfaceView over only that record's extent,
      // touching no other surface's bytes. The headline subset-map row.
      Result<PricedSurfaceView> one = fx.opened_v2->map_symbol(fx.symbols.front());
      if (!one.has_value()) {
        state.SkipWithError(one.error().to_string().c_str());
        return;
      }
      surfaces_per_iter = 1;
      benchmark::DoNotOptimize(one->n_slices());
      break;
    }
    }
    benchmark::ClobberMemory();
  }
  const auto per_iter = static_cast<std::int64_t>(surfaces_per_iter);
  state.SetItemsProcessed(state.iterations() * per_iter);
  state.counters["surfaces_in_archive"] = static_cast<double>(count);
  state.counters["surfaces_per_iter"] = static_cast<double>(surfaces_per_iter);
  state.counters["archive_bytes"] = static_cast<double>(fx.bytes_v2.size());
}

// ── Registration ─────────────────────────────────────────────────────────────

constexpr int kCounts[] = {1, 4, 16, 50, 100};

[[nodiscard]] const char *deser_mode_name(DeserMode mode) noexcept {
  switch (mode) {
  case DeserMode::OpenReconstructAll:
    return "open_reconstruct_all_v2";
  case DeserMode::ReconstructAll:
    return "reconstruct_all_v2";
  case DeserMode::ReconstructOne:
    return "reconstruct_one_v2";
  case DeserMode::MmapOpen:
    return "mmap_open";
  case DeserMode::SubsetMapZeroCopy:
    return "subset_map_zero_copy";
  }
  return "unknown";
}

// Repetitions override (review fix, finding #2): apply_common's default 5 reps is
// too few for a stable CV under shared-host contention. These rows are fast (µs),
// so more repetition-means is cheap and makes the CV estimate robust — two noisy
// reps can no longer swing it. Overrides apply_common's Repetitions(5) (last wins).
constexpr int kSurfaceArchiveReps = 15;

void register_serialize_v2(Payload payload, int count) {
  const std::string name = std::string("surface_archive/serialize_v2/") + payload_name(payload) +
                           "/count:" + std::to_string(count);
  apply_common(benchmark::RegisterBenchmark(
                   name, [payload, count](benchmark::State &st) { run_serialize_v2(st, payload, count); }))
      ->Unit(benchmark::kMicrosecond)
      ->Repetitions(kSurfaceArchiveReps)
      ->UseRealTime();
}

void register_deserialize(Payload payload, int count, DeserMode mode) {
  const std::string name = std::string("surface_archive/deserialize/") + payload_name(payload) +
                           "/" + deser_mode_name(mode) + "/count:" + std::to_string(count);
  apply_common(benchmark::RegisterBenchmark(name, [payload, count, mode](benchmark::State &st) {
    run_deserialize(st, payload, count, mode);
  }))
      ->Unit(benchmark::kMicrosecond)
      ->Repetitions(kSurfaceArchiveReps)
      ->UseRealTime();
}

const int kRegistered = [] {
  for (const Payload payload : {Payload::Essvi, Payload::ConvexDense}) {
    for (const int count : kCounts) {
      register_serialize_v2(payload, count);
      register_deserialize(payload, count, DeserMode::OpenReconstructAll);
      register_deserialize(payload, count, DeserMode::ReconstructAll);
      register_deserialize(payload, count, DeserMode::ReconstructOne);
      // The zero-copy rows: the headline deserialize win. mmap_open is open+map_all
      // vs OpenReconstructAll; subset_map_zero_copy is the one-symbol view vs
      // ReconstructOne — owned-vs-view on the SAME format and the same bytes.
      register_deserialize(payload, count, DeserMode::MmapOpen);
      register_deserialize(payload, count, DeserMode::SubsetMapZeroCopy);
    }
  }
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
