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
//
// ── Point-read study (lane SER, 2026-10-23) ───────────────────────────────────
//
// Registered ONLY when ATX_SURFACE_ARCHIVE_STUDY_DIR names an output directory; run
// with --benchmark_filter=pointread_study. One pass, not a repetition schedule: it
// builds a 6,000-symbol partition (Essvi/Svi bulk + 25 ConvexDense + 25 SplineVol)
// through SurfaceDb::write_partition and measures, through the SurfaceDb API only (so
// the same TU builds against the pre-SER and post-SER library): write wall, file size,
// cold single-symbol map (fresh SurfaceDb per read, 200 seeded random symbols) p50/p99,
// the same through load_surface, warm repeated map on one handle, and whole-board
// open_partition + map_all. Bytes and read calls per operation come from the process
// I/O counters (GetProcessIoCounters: every ReadFile, cached or not), which needs no
// reader instrumentation and so counts the old stream read and the new point read the
// same way. Results go to <dir>/pointread_study.tsv and stdout.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <benchmark/benchmark.h>

#include "atx/vol/api/pricing/american.hpp"        // al_fast_opts, AmericanMethod, AlOpts
#include "atx/vol/api/pricing/black76.hpp"         // black76_price (ConvexDense node prices)
#include "atx/vol/api/fitting/dense_slice.hpp"     // ConvexSliceFit
#include "atx/vol/api/backtest/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/api/storage/surface_archive.hpp" // write_surface_archive_v2, SurfaceArchiveV2
#include "atx/vol/api/storage/surface_db.hpp"      // SurfaceDb (point-read study)
#include "atx/vol/api/fitting/spline_curve.hpp"    // SplineVolParams (point-read study)
#include "atx/vol/api/fitting/surface_parity.hpp"  // SliceContext
#include "atx/vol/api/core/types.hpp"           // Side
#include "atx/vol/api/fitting/vol_curve.hpp"       // CurveSurface, EssviCurve, ConvexDenseCurve
#include "atx/vol/api/fitting/vol_surface.hpp"     // EssviParams

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

// ── Point-read study ──────────────────────────────────────────────────────────

constexpr int kStudySymbols = 6000;
constexpr int kStudyReads = 200;
constexpr int kStudyWholeBoardRuns = 5;
constexpr std::uint64_t kStudySeed = 20261023u;
constexpr const char *kStudyKey = "2026-10-23";

[[nodiscard]] PricedSurface make_svi_surface(std::uint32_t uid) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < kSlices; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    SviParams s{};
    s.a = 0.02 + 0.002 * static_cast<double>(i);
    s.b = 0.35;
    s.rho = -0.3;
    s.m = 0.0;
    s.sigma = 0.35;
    s.T = T;
    s.F = kS;
    s.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<SviCurve>(s, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, 180, 4});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  if (!ps.has_value()) {
    bench_fatal(ps.error().to_string());
  }
  return std::move(*ps);
}

[[nodiscard]] PricedSurface make_spline_surface(std::uint32_t uid) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < kSlices; ++i) {
    const double T = 0.05 + 0.10 * static_cast<double>(i);
    SplineVolParams p;
    p.atm_vol = 0.20 + 0.005 * static_cast<double>(i);
    p.z = {-3.0, -2.0, -1.0, -0.5, 0.0, 0.5, 1.0, 2.0, 3.0};
    p.mult = {1.45, 1.25, 1.08, 1.03, 1.0, 0.99, 1.01, 1.08, 1.2};
    p.z_lo_valid = -3.0;
    p.z_hi_valid = 3.0;
    p.mult_cap = 1.4;
    p.w_offset = 0.0005 * static_cast<double>(i);
    cs.push(std::make_unique<SplineVolCurve>(std::move(p), T, kS, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, kS, 0.0, 0.02, 60, 2});
  }
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), make_pricing(uid));
  if (!ps.has_value()) {
    bench_fatal(ps.error().to_string());
  }
  return std::move(*ps);
}

struct IoSample {
  std::uint64_t bytes{0};
  std::uint64_t ops{0};
};

[[nodiscard]] IoSample io_now() {
#if defined(_WIN32)
  IO_COUNTERS c{};
  if (::GetProcessIoCounters(::GetCurrentProcess(), &c) == 0) {
    return {};
  }
  return IoSample{static_cast<std::uint64_t>(c.ReadTransferCount),
                  static_cast<std::uint64_t>(c.ReadOperationCount)};
#else
  return {};
#endif
}

[[nodiscard]] double pct(std::vector<double> v, double q) {
  if (v.empty()) {
    return 0.0;
  }
  std::sort(v.begin(), v.end());
  const auto idx = static_cast<std::size_t>(std::ceil(q * static_cast<double>(v.size()))) - 1u;
  return v[std::min(idx, v.size() - 1u)];
}

[[nodiscard]] double mean_of(const std::vector<double> &v) {
  double s = 0.0;
  for (const double x : v) {
    s += x;
  }
  return v.empty() ? 0.0 : s / static_cast<double>(v.size());
}

[[nodiscard]] double us_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

struct StudyRow {
  std::string name;
  double value{0.0};
};

// _dupenv_s under clang-cl (plain std::getenv trips /WX), as bench_main.cpp does.
[[nodiscard]] std::string study_dir() {
#if defined(_MSC_VER)
  char *raw = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&raw, &n, "ATX_SURFACE_ARCHIVE_STUDY_DIR") != 0 || raw == nullptr) {
    return {};
  }
  std::string out(raw);
  std::free(raw);
  return out;
#else
  const char *raw = std::getenv("ATX_SURFACE_ARCHIVE_STUDY_DIR");
  return raw == nullptr ? std::string{} : std::string(raw);
#endif
}

void run_pointread_study(benchmark::State &state) {
  const std::string dir = study_dir();
  if (dir.empty()) {
    state.SkipWithError("ATX_SURFACE_ARCHIVE_STUDY_DIR is not set");
    return;
  }
  const char *out_dir = dir.c_str();
  for (auto _ : state) {
    const std::filesystem::path root = std::filesystem::path(out_dir) / "db";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    std::vector<PricedSurface> surfaces;
    std::vector<std::string> names;
    surfaces.reserve(kStudySymbols);
    names.reserve(kStudySymbols);
    int n_heavy = 0;
    for (int i = 0; i < kStudySymbols; ++i) {
      const auto uid = static_cast<std::uint32_t>(10000 + i);
      if (i % 240 == 0) {
        surfaces.push_back(make_convex_surface(uid));
        ++n_heavy;
      } else if (i % 240 == 120) {
        surfaces.push_back(make_spline_surface(uid));
        ++n_heavy;
      } else if (i % 2 == 0) {
        surfaces.push_back(make_essvi_surface(uid));
      } else {
        surfaces.push_back(make_svi_surface(uid));
      }
      char buf[16];
      std::snprintf(buf, sizeof buf, "S%04d", i);
      names.emplace_back(buf);
    }
    std::vector<SurfaceArchiveItem> items;
    items.reserve(surfaces.size());
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
      SurfaceArchiveItem item;
      item.symbol = names[i];
      item.surface = &surfaces[i];
      items.push_back(item);
    }

    std::vector<StudyRow> rows;
    rows.push_back({"symbols", static_cast<double>(kStudySymbols)});
    rows.push_back({"heavy_surfaces", static_cast<double>(n_heavy)});

    const auto t_ser = std::chrono::steady_clock::now();
    auto in_memory = write_surface_archive_v2(items);
    rows.push_back({"serialize_in_memory_us", us_since(t_ser)});
    if (!in_memory) {
      bench_fatal("serialize: " + in_memory.error().to_string());
    }

    auto created = SurfaceDb::create(root.string());
    if (!created) {
      bench_fatal("create: " + created.error().to_string());
    }
    const auto t_write = std::chrono::steady_clock::now();
    const Status wrote = created->write_partition(kStudyKey, items);
    rows.push_back({"write_partition_us", us_since(t_write)});
    if (!wrote) {
      bench_fatal("write_partition: " + wrote.error().to_string());
    }
    const std::filesystem::path part =
        root / "partitions" / (std::string(kStudyKey) + ".atxvsa");
    rows.push_back(
        {"partition_file_bytes", static_cast<double>(std::filesystem::file_size(part, ec))});

    std::mt19937_64 rng(kStudySeed);
    std::uniform_int_distribution<int> pick(0, kStudySymbols - 1);
    std::vector<std::string> probe;
    for (int i = 0; i < kStudyReads; ++i) {
      probe.push_back(names[static_cast<std::size_t>(pick(rng))]);
    }

    // Cold map: a fresh SurfaceDb per read, timed open+map and map-only.
    std::vector<double> open_map_us;
    std::vector<double> map_us;
    std::vector<double> map_bytes;
    std::vector<double> map_ops;
    double sink = 0.0;
    for (const std::string &sym : probe) {
      const auto t0 = std::chrono::steady_clock::now();
      auto db = SurfaceDb::open(root.string());
      if (!db) {
        bench_fatal("open: " + db.error().to_string());
      }
      const IoSample io0 = io_now();
      const auto t1 = std::chrono::steady_clock::now();
      auto mapped = db->map_surface(kStudyKey, sym);
      if (!mapped) {
        bench_fatal("map_surface " + sym + ": " + mapped.error().to_string());
      }
      sink += mapped->view.iv(kS, 0.25);
      map_us.push_back(us_since(t1));
      open_map_us.push_back(us_since(t0));
      const IoSample io1 = io_now();
      map_bytes.push_back(static_cast<double>(io1.bytes - io0.bytes));
      map_ops.push_back(static_cast<double>(io1.ops - io0.ops));
    }
    rows.push_back({"cold_open_map_p50_us", pct(open_map_us, 0.50)});
    rows.push_back({"cold_open_map_p99_us", pct(open_map_us, 0.99)});
    rows.push_back({"cold_map_p50_us", pct(map_us, 0.50)});
    rows.push_back({"cold_map_p99_us", pct(map_us, 0.99)});
    rows.push_back({"cold_map_mean_bytes_read", mean_of(map_bytes)});
    rows.push_back({"cold_map_max_bytes_read", pct(map_bytes, 1.0)});
    rows.push_back({"cold_map_mean_read_calls", mean_of(map_ops)});

    // Cold owned load: fresh SurfaceDb per read, load_surface only.
    std::vector<double> load_us;
    std::vector<double> load_bytes;
    for (const std::string &sym : probe) {
      auto db = SurfaceDb::open(root.string());
      if (!db) {
        bench_fatal("open: " + db.error().to_string());
      }
      const IoSample io0 = io_now();
      const auto t0 = std::chrono::steady_clock::now();
      auto loaded = db->load_surface(kStudyKey, sym);
      if (!loaded) {
        bench_fatal("load_surface " + sym + ": " + loaded.error().to_string());
      }
      sink += loaded->iv(kS, 0.25);
      load_us.push_back(us_since(t0));
      load_bytes.push_back(static_cast<double>(io_now().bytes - io0.bytes));
    }
    rows.push_back({"cold_load_p50_us", pct(load_us, 0.50)});
    rows.push_back({"cold_load_p99_us", pct(load_us, 0.99)});
    rows.push_back({"cold_load_mean_bytes_read", mean_of(load_bytes)});

    // Warm: one SurfaceDb handle, repeated maps (pre-SER: partition-cache hits).
    {
      auto db = SurfaceDb::open(root.string());
      if (!db) {
        bench_fatal("open: " + db.error().to_string());
      }
      auto first = db->map_surface(kStudyKey, probe.front());
      if (!first) {
        bench_fatal("warm prime: " + first.error().to_string());
      }
      std::vector<double> warm_us;
      std::vector<double> warm_bytes;
      for (const std::string &sym : probe) {
        const IoSample io0 = io_now();
        const auto t0 = std::chrono::steady_clock::now();
        auto mapped = db->map_surface(kStudyKey, sym);
        if (!mapped) {
          bench_fatal("warm map " + sym + ": " + mapped.error().to_string());
        }
        sink += mapped->view.iv(kS, 0.25);
        warm_us.push_back(us_since(t0));
        warm_bytes.push_back(static_cast<double>(io_now().bytes - io0.bytes));
      }
      rows.push_back({"warm_map_p50_us", pct(warm_us, 0.50)});
      rows.push_back({"warm_map_p99_us", pct(warm_us, 0.99)});
      rows.push_back({"warm_map_mean_bytes_read", mean_of(warm_bytes)});
    }

    // Whole board: open_partition + map_all, median of 5.
    {
      auto db = SurfaceDb::open(root.string());
      if (!db) {
        bench_fatal("open: " + db.error().to_string());
      }
      std::vector<double> board_us;
      std::vector<double> board_bytes;
      for (int run = 0; run < kStudyWholeBoardRuns; ++run) {
        const IoSample io0 = io_now();
        const auto t0 = std::chrono::steady_clock::now();
        auto arch = db->open_partition(kStudyKey);
        if (!arch) {
          bench_fatal("open_partition: " + arch.error().to_string());
        }
        auto all = arch->map_all();
        if (!all) {
          bench_fatal("map_all: " + all.error().to_string());
        }
        sink += static_cast<double>(all->size());
        board_us.push_back(us_since(t0));
        board_bytes.push_back(static_cast<double>(io_now().bytes - io0.bytes));
      }
      rows.push_back({"whole_board_open_map_all_p50_us", pct(board_us, 0.50)});
      rows.push_back({"whole_board_mean_bytes_read", mean_of(board_bytes)});
    }
    benchmark::DoNotOptimize(sink);

    std::ofstream tsv(std::filesystem::path(out_dir) / "pointread_study.tsv",
                      std::ios::binary | std::ios::trunc);
    for (const StudyRow &row : rows) {
      tsv << row.name << '\t' << row.value << '\n';
      std::printf("%s\t%.3f\n", row.name.c_str(), row.value);
      state.counters[row.name] = row.value;
    }
    std::filesystem::remove_all(root, ec);
  }
}

const int kStudyRegistered = [] {
  if (!study_dir().empty()) {
    benchmark::RegisterBenchmark("surface_archive/pointread_study", run_pointread_study)
        ->Iterations(1)
        ->Repetitions(1)
        ->Unit(benchmark::kMicrosecond)
        ->UseRealTime();
  }
  return 0;
}();

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
