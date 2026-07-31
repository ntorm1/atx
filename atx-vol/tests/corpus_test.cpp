// Surface-archive CORPUS integrity + throughput.
//
// A corpus is a grid of (date, symbol) fitted surfaces laid out as ONE
// SurfaceArchive per date plus a manifest. These tests prove, with NO external
// data (synthetic known-truth boards only, so they run everywhere and are NOT
// GTEST_SKIP-gated):
//
//   1. layout      — build_corpus writes one archive per date, and the manifest
//                    indexes the right dates/entries/counts;
//   2. curve mix   — the corpus round-trips BOTH curve families: a board left on
//                    the default policy AUTO-SELECTS its curve, and on the smooth
//                    synthetic S3 truth that correctly picks the parsimonious
//                    eSSVI backbone (it generalizes ~100% out-of-sample with far
//                    fewer DoF than the dense fit — measured in the probe run and
//                    matching breadth_regime_test's note). A genuine ConvexDense
//                    auto-selection needs REAL microstructure the fixture cannot
//                    synthesize, so the dense family is exercised through an
//                    explicit per-board PIN (the lifecycle index recipe) — the
//                    point is that the archive serializes + reprices BOTH the
//                    ConvexDense (variable-length node blobs) and eSSVI (fixed
//                    POD blobs) surfaces bit-for-bit;
//   3. integrity   — every Ok entry's reloaded surface reproduces a fresh fit of
//                    the same board BIT-for-BIT (iv / fair_value / every Greek);
//   4. manifest    — parse(serialize(m)) == m and write->read round-trips.
//
// (Corpus build throughput lives in bench/corpus_build_bench.cpp.)

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "atx/vol/american.hpp"             // AmericanGreeks
#include "atx/vol/backtest.hpp"             // Clock, MarketSnapshot, run_backtest
#include "atx/vol/chain.hpp"                // OptionChain
#include "atx/vol/corpus.hpp"               // build_corpus, CorpusManifest, ...
#include "atx/vol/data.hpp"                 // iso_to_ns, year_fraction
#include "atx/vol/detail/fit_scheduler.hpp" // run_bounded_fit_tasks
#include "atx/vol/dispersion.hpp"           // DispersionUniverse, DroppedName
#include "atx/vol/research/dispersion_run.hpp"       // format_corpus_phase_line (T-I4 probe gate)
#include "atx/vol/market_env.hpp"           // MarketEnv
#include "atx/vol/panel.hpp"                // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/priced_surface.hpp"       // PricedSurface
#include "atx/vol/priced_surface_view.hpp"  // PricedSurfaceView (v2 zero-copy view)
#include "atx/vol/pricer_fitter.hpp"        // PricerFitter, PricerConfig
#include "atx/vol/session.hpp"              // VolaSession::to_priced_surface
#include "support/spy_fixture.hpp"          // make_spy_synthetic_spec
#include "atx/vol/strategy.hpp"             // DispersionStrategy
#include "atx/vol/surface_archive.hpp"      // SurfaceArchive
#include "atx/vol/types.hpp"                // Side
#include "atx/vol/vol_curve.hpp"            // CurveConfig, VolCurveKind, to_string

#include "support/isa_golden_tol.hpp" // kLanedGreeksRelBand (WS-P1a route band)

#include <iomanip> // std::setprecision (route-parity failure messages)

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

class BoundedRendezvous {
public:
  explicit BoundedRendezvous(unsigned target) : target_{target} {}

  [[nodiscard]] bool arrive_and_wait() {
    constexpr unsigned kMaxAttempts = 1'000'000u;
    const unsigned arrived = arrived_.fetch_add(1u) + 1u;
    if (arrived >= target_) {
      return true;
    }
    for (unsigned attempt = 0u; attempt < kMaxAttempts; ++attempt) {
      if (arrived_.load() >= target_) {
        return true;
      }
      std::this_thread::yield();
    }
    return false;
  }

private:
  unsigned target_{};
  std::atomic_uint arrived_{0u};
};

TEST(FitScheduler, PeakConcurrentTasksNeverExceedsExplicitBudget) {
  constexpr std::size_t kTaskCount = 6u;
  constexpr unsigned kBudget = 3u;
  std::array<std::atomic_uint, kTaskCount> visits{};
  std::atomic_uint active{0u};
  unsigned peak = 0u;
  std::mutex peak_mutex;
  BoundedRendezvous rendezvous{kBudget};

  const Status status =
      detail::run_bounded_fit_tasks(kTaskCount, kBudget, [&](std::size_t index) -> Status {
        visits[index].fetch_add(1u);
        const unsigned now = active.fetch_add(1u) + 1u;
        {
          const std::lock_guard lock{peak_mutex};
          peak = std::max(peak, now);
        }
        if (!rendezvous.arrive_and_wait()) {
          active.fetch_sub(1u);
          return atx::core::Err(ErrorCode::Internal, "scheduler underprovisioned");
        }
        active.fetch_sub(1u);
        return atx::core::Ok();
      });

  ASSERT_TRUE(status) << status.error().to_string();
  EXPECT_EQ(peak, kBudget);
  EXPECT_EQ(active.load(), 0u);
  for (const std::atomic_uint &visit : visits) {
    EXPECT_EQ(visit.load(), 1u);
  }
}

TEST(FitScheduler, WorkerExceptionReturnsInternalAfterJoiningAllWork) {
  constexpr std::size_t kTaskCount = 8u;
  constexpr unsigned kBudget = 4u;
  std::array<std::atomic_uint, kTaskCount> visits{};
  BoundedRendezvous rendezvous{kBudget};
  const std::thread::id caller_id = std::this_thread::get_id();
  std::atomic_bool injected{false};

  const Status status =
      detail::run_bounded_fit_tasks(kTaskCount, kBudget, [&](std::size_t index) -> Status {
        visits[index].fetch_add(1u);
        if (!rendezvous.arrive_and_wait()) {
          return atx::core::Err(ErrorCode::Internal, "scheduler underprovisioned");
        }
        if (std::this_thread::get_id() != caller_id && !injected.exchange(true)) {
          throw std::runtime_error{"forced worker failure"};
        }
        return atx::core::Ok();
      });

  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::Internal);
  EXPECT_TRUE(injected.load());
  for (const std::atomic_uint &visit : visits) {
    EXPECT_EQ(visit.load(), 1u);
  }
}

TEST(FitScheduler, IndexedOutputAndLowestFailureAreDeterministicAcrossBudgets) {
  constexpr std::size_t kTaskCount = 7u;
  std::array<std::size_t, kTaskCount> serial_output{};
  std::array<std::size_t, kTaskCount> parallel_output{};
  const auto run = [](unsigned budget, std::array<std::size_t, kTaskCount> &output) -> Status {
    return detail::run_bounded_fit_tasks(
        kTaskCount, budget, [&output](std::size_t index) -> Status {
          output[index] = (index + 1u) * 17u;
          if (index == 1u) {
            return atx::core::Err(ErrorCode::NotFound, "first indexed failure");
          }
          if (index == 5u) {
            return atx::core::Err(ErrorCode::InvalidArgument, "later indexed failure");
          }
          return atx::core::Ok();
        });
  };

  const Status serial = run(1u, serial_output);
  const Status parallel = run(4u, parallel_output);

  ASSERT_FALSE(serial);
  ASSERT_FALSE(parallel);
  EXPECT_EQ(serial.error().code(), ErrorCode::NotFound);
  EXPECT_EQ(parallel.error().code(), ErrorCode::NotFound);
  EXPECT_EQ(serial.error().message(), "first indexed failure");
  EXPECT_EQ(parallel.error().message(), "first indexed failure");
  EXPECT_EQ(parallel_output, serial_output);
}

TEST(FitScheduler, PartialWorkerLaunchFailureAbortsWaitingWorkersWithoutRunningTasks) {
  std::atomic_uint visits{0u};
  detail::FitSchedulerTestHooks hooks;
  hooks.before_worker_launch = [](std::size_t ordinal) {
    if (ordinal == 1u) {
      throw std::runtime_error{"forced jthread construction failure"};
    }
  };

  const Status status = detail::run_bounded_fit_tasks(
      8u, 4u,
      [&visits](std::size_t) -> Status {
        visits.fetch_add(1u);
        return atx::core::Ok();
      },
      detail::FitAffinity::None, &hooks);

  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::Internal);
  EXPECT_EQ(visits.load(), 0u);
}

// C4 (perf): the PerformanceCores affinity path must be a pure scheduling steer —
// every index still runs exactly once and produces the SAME per-index output as the
// unpinned None path (pinning changes only WHICH logical CPU a context runs on).
// performance_core_count() must be queryable without crashing (it is 0 when P-core
// discovery is unavailable, in which case the pinned path transparently runs
// unpinned). This is the byte-identity gate for C4's affinity primitive.
TEST(FitScheduler, PerformanceCoresAffinityIsAPureSchedulingSteer) {
  // Discovery must never throw or crash; 0 is a valid answer (unavailable).
  const unsigned p_cores = detail::performance_core_count();
  EXPECT_GE(p_cores, 0u);

  constexpr std::size_t kTaskCount = 32u;
  constexpr unsigned kBudget = 8u;
  const auto run = [](detail::FitAffinity affinity,
                      std::array<std::size_t, kTaskCount> &output) -> Status {
    return detail::run_bounded_fit_tasks(
        kTaskCount, kBudget,
        [&output](std::size_t index) -> Status {
          output[index] = (index * 2654435761u) ^ (index + 1u); // deterministic per-index value
          return atx::core::Ok();
        },
        affinity);
  };

  std::array<std::size_t, kTaskCount> unpinned{};
  std::array<std::size_t, kTaskCount> pinned{};
  const Status s_unpinned = run(detail::FitAffinity::None, unpinned);
  const Status s_pinned = run(detail::FitAffinity::PerformanceCores, pinned);

  ASSERT_TRUE(s_unpinned) << s_unpinned.error().to_string();
  ASSERT_TRUE(s_pinned) << s_pinned.error().to_string();
  EXPECT_EQ(pinned, unpinned); // byte-identical per-index outputs across affinity
}

// P3.1 regression guard #1 — the DEFAULT-OFF contract for the E-core second tier.
//
// This is the gate that keeps the tier from silently appearing underneath a
// benchmark holding the P-core lease. `efficiency_core_tier_enabled()` must be
// false unless ATX_VOL_FIT_ECORE_TIER is explicitly armed in the environment, and
// the test suite never arms it — so on every CI host, hybrid or not, the answer is
// false and FitAffinity::PerformanceThenEfficiencyCores degrades to exactly
// FitAffinity::PerformanceCores. efficiency_core_count() must be queryable without
// crashing; 0 is a valid answer (homogeneous host or discovery unavailable).
TEST(FitScheduler, EfficiencyCoreTierIsOptInAndOffByDefault) {
  EXPECT_GE(detail::efficiency_core_count(), 0u);
  EXPECT_FALSE(detail::efficiency_core_tier_enabled())
      << "the E-core tier must stay disarmed unless ATX_VOL_FIT_ECORE_TIER is set; "
         "an armed default would oversubscribe a P-core-pinned benchmark";
}

// P3.1 regression guard #2 — the two-tier affinity is a pure scheduling steer.
//
// Mirrors PerformanceCoresAffinityIsAPureSchedulingSteer for the P-then-E value:
// every index runs exactly once and yields the SAME per-index output as both the
// unpinned and the P-core-only paths, at three different worker budgets. Core
// assignment, worker count, and (when armed) thread priority must never reach a
// task's value. This holds whether or not the tier is armed on the host running
// the test, which is what makes it a usable gate on both hybrid and homogeneous CI.
TEST(FitScheduler, EfficiencyTierAffinityIsAPureSchedulingSteerAcrossWorkerCounts) {
  constexpr std::size_t kTaskCount = 64u;
  const auto run = [](detail::FitAffinity affinity, unsigned budget,
                      std::array<std::size_t, kTaskCount> &output) -> Status {
    return detail::run_bounded_fit_tasks(
        kTaskCount, budget,
        [&output](std::size_t index) -> Status {
          output[index] = (index * 2654435761u) ^ (index + 1u); // deterministic per-index value
          return atx::core::Ok();
        },
        affinity);
  };

  std::array<std::size_t, kTaskCount> reference{};
  const Status s_reference = run(detail::FitAffinity::None, 4u, reference);
  ASSERT_TRUE(s_reference) << s_reference.error().to_string();

  for (const unsigned budget : {1u, 4u, 16u}) {
    std::array<std::size_t, kTaskCount> pcore{};
    std::array<std::size_t, kTaskCount> two_tier{};
    const Status s_pcore = run(detail::FitAffinity::PerformanceCores, budget, pcore);
    const Status s_two_tier =
        run(detail::FitAffinity::PerformanceThenEfficiencyCores, budget, two_tier);
    ASSERT_TRUE(s_pcore) << s_pcore.error().to_string();
    ASSERT_TRUE(s_two_tier) << s_two_tier.error().to_string();
    EXPECT_EQ(pcore, reference) << "P-core affinity changed output at budget " << budget;
    EXPECT_EQ(two_tier, reference) << "P-then-E affinity changed output at budget " << budget;
  }
}

// Bit-for-bit double equality via the raw uint64 pattern (the round-trip gate is
// BIT-identical, not merely close).
[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// A fresh unique output directory for one test (removed if it lingers).
[[nodiscard]] fs::path fresh_out_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-corpus-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

// The ConvexDense "index recipe" pin (from lifecycle_integration_test): the
// arb-free 99.5%-in-band dense fit. Used to exercise the dense archive path.
[[nodiscard]] CurveConfig convex_dense_pin() {
  CurveConfig c;
  c.kind = VolCurveKind::ConvexDense;
  c.convex.node_cap = 40;
  return c;
}

// A penny-dense INDEX board. Reuses the canonical SPY fixture, rescaled to `spot`
// and re-tagged, at valuation date `snapshot`. Left on the default (auto) policy
// it selects eSSVI on this smooth truth; pinned it fits ConvexDense.
[[nodiscard]] SynthPanelSpec make_index_spec(const std::string &uid, const std::string &snapshot,
                                             double spot) {
  SynthPanelSpec s = make_spy_synthetic_spec(snapshot);
  s.uid = uid;
  const double scale = spot / s.spot;
  s.spot = spot;
  for (double &k : s.strikes) {
    k *= scale;
  }
  return s;
}

// A wider-spread, single-name-style board (moderate strike ladder, higher vol,
// wide two-sided markets). On the default policy it auto-selects the eSSVI
// backbone.
[[nodiscard]] SynthPanelSpec make_singlename_spec(const std::string &uid,
                                                  const std::string &snapshot, double spot) {
  SynthPanelSpec s;
  s.uid = uid;
  s.snapshot_iso = snapshot;
  s.spot = spot;
  s.r = 0.043;
  s.borrow = 0.0;

  struct Row {
    const char *iso;
    double sigma0;
    double skew_k;
    double c2;
  };
  const Row rows[] = {
      {"2026-07-17", 0.36, -0.55, 0.6},
      {"2026-08-21", 0.33, -0.52, 0.7},
      {"2026-09-18", 0.31, -0.50, 0.8},
      {"2026-12-18", 0.29, -0.46, 0.9},
  };
  for (const Row &r : rows) {
    SynthExpiry e;
    e.expiry_iso = r.iso;
    e.T = year_fraction(snapshot, r.iso);
    const double s2 = 2.0 * std::sqrt(e.T) * r.skew_k;
    e.truth = S3Params{r.sigma0, s2, r.c2};
    s.expiries.push_back(e);
  }
  // A moderate ladder (13 strikes) over a wide band — enough to fit robustly, far
  // from penny-dense.
  for (const double m :
       {0.80, 0.83, 0.87, 0.91, 0.95, 0.98, 1.0, 1.02, 1.05, 1.09, 1.13, 1.17, 1.20}) {
    s.strikes.push_back(spot * m);
  }
  s.half_spread_frac = 0.05;
  s.min_half_spread = 0.05;
  return s;
}

// Materialize a CorpusBoard from a spec (copies the frame; builds the env). An
// optional per-board curve pin is carried onto the board.
[[nodiscard]] CorpusBoard board_from_spec(const SynthPanelSpec &spec, std::string date,
                                          std::string symbol,
                                          std::optional<CurveConfig> curve = std::nullopt) {
  auto panel = make_synthetic_american_panel(spec);
  EXPECT_TRUE(panel.has_value()) << (panel ? "" : panel.error().to_string());
  CorpusBoard b;
  b.date = std::move(date);
  b.symbol = std::move(symbol);
  if (panel.has_value()) {
    b.frame = panel->frame;
  }
  b.env = MarketEnv::flat(spec.spot, spec.r, iso_to_ns(spec.snapshot_iso), spec.cash_divs);
  b.curve = std::move(curve);
  return b;
}

// Fit a board through the corpus's blessed path (default Robust template,
// single-threaded, honouring the board's curve pin) into a PricedSurface — the
// deterministic reference the reloaded archive surface must reproduce bit-for-bit.
[[nodiscard]] PricedSurface fit_reference(const CorpusBoard &board) {
  auto chain = OptionChain::from_frame(board.frame, board.env);
  EXPECT_TRUE(chain.has_value());
  PricerConfig cfg; // Robust default
  cfg.n_threads = 1;
  if (board.curve.has_value()) {
    cfg.curve = *board.curve;
  }
  PricerFitter fitter{cfg};
  const Status st = fitter.fit(*chain);
  EXPECT_TRUE(st.has_value());
  auto ps = fitter.surface()->session().to_priced_surface();
  EXPECT_TRUE(ps.has_value());
  return std::move(*ps);
}

// Fit a board through the SAME blessed path as fit_reference, but return the
// served surface's admitted SurfaceHealth (mirrors the purpose-selection
// build_corpus's fit_board now performs for archive provenance: risk_health
// when the served surface is the risk surface, market_mark_health otherwise).
[[nodiscard]] SurfaceHealth health_reference(const CorpusBoard &board) {
  auto chain = OptionChain::from_frame(board.frame, board.env);
  EXPECT_TRUE(chain.has_value());
  PricerConfig cfg; // Robust default
  cfg.n_threads = 1;
  if (board.curve.has_value()) {
    cfg.curve = *board.curve;
  }
  PricerFitter fitter{cfg};
  const Status st = fitter.fit(*chain);
  EXPECT_TRUE(st.has_value());
  const FittedSurface *fitted = fitter.surface();
  EXPECT_NE(fitted, nullptr);
  const SurfaceBundle bundle = fitter.bundle();
  return fitted->purpose() == SurfacePurpose::Risk ? bundle.risk_health : bundle.market_mark_health;
}

// Assert two PricedSurfaces are bit-identical over a (K, T, side) grid straddling
// each slice forward. Accumulates the number of priced grid points into `n_fv`
// (void return so the ASSERT_* fatal guards are legal here).
void expect_surfaces_bit_identical(const PricedSurface &a, const PricedSurface &b,
                                   std::size_t &n_fv) {
  EXPECT_EQ(a.n_slices(), b.n_slices());
  for (const SliceContext &c : a.context()) {
    const double T = c.T;
    const double F = c.forward;
    for (const double m : {0.90, 0.95, 0.98, 1.0, 1.02, 1.05, 1.10}) {
      const double K = F * m;
      const Side side = (m <= 1.0) ? Side::Put : Side::Call;

      EXPECT_TRUE(bits_equal(a.iv(K, T), b.iv(K, T))) << "iv K=" << K << " T=" << T;
      if (!std::isfinite(a.iv(K, T))) {
        continue;
      }
      const auto fa = a.fair_value(K, T, side);
      const auto fb = b.fair_value(K, T, side);
      ASSERT_EQ(fa.has_value(), fb.has_value());
      if (fa.has_value()) {
        EXPECT_TRUE(bits_equal(*fa, *fb)) << "fv K=" << K << " T=" << T;
        ++n_fv;
      }
      const auto ga = a.greeks(K, T, side);
      const auto gb = b.greeks(K, T, side);
      ASSERT_EQ(ga.has_value(), gb.has_value());
      if (ga.has_value()) {
        EXPECT_TRUE(bits_equal(ga->price, gb->price)) << "price K=" << K;
        EXPECT_TRUE(bits_equal(ga->delta, gb->delta)) << "delta K=" << K;
        EXPECT_TRUE(bits_equal(ga->gamma, gb->gamma)) << "gamma K=" << K;
        EXPECT_TRUE(bits_equal(ga->vega, gb->vega)) << "vega K=" << K;
        EXPECT_TRUE(bits_equal(ga->theta, gb->theta)) << "theta K=" << K;
        EXPECT_TRUE(bits_equal(ga->rho, gb->rho)) << "rho K=" << K;
        EXPECT_TRUE(bits_equal(ga->vanna, gb->vanna)) << "vanna K=" << K;
        EXPECT_TRUE(bits_equal(ga->volga, gb->volga)) << "volga K=" << K;
        EXPECT_TRUE(bits_equal(ga->charm, gb->charm)) << "charm K=" << K;
      }
    }
  }
}

// F-a end-to-end proof (WS-F): assert the ZERO-COPY v2 view (map_symbol ->
// PricedSurfaceView — the deserialize path the backtest hot loop actually takes,
// per the priced-surface-view seam) reproduces a fresh fit of the same board
// bit-for-bit. This closes the leg the RoundTrip test above only proves
// transitively: RoundTrip compares the OWNED reconstruct (reconstruct_symbol) to a
// fresh fit; surface_archive_v2_test compares a view to its source PricedSurface;
// this test compares the view DIRECTLY to the freshly-fitted surface, driven by
// the real fit PIPELINE (build_corpus -> write_surface_archive_v2_file). The view
// exposes no context() accessor, so the (K,T) grid is driven off the fresh
// surface's slice contexts (same grid as expect_surfaces_bit_identical). uid is
// intentionally NOT compared: build_corpus restamps the archived surface with a
// symbol-derived uid (uid_for_symbol) while the fresh single-symbol fit keeps
// uid=1 — pricing math is uid-independent (as the RoundTrip gate already relies on).
void expect_view_reproduces_fresh_fit_bit_identical(const PricedSurface &fresh,
                                                    const PricedSurfaceView &v, std::size_t &n_fv) {
  ASSERT_EQ(fresh.n_slices(), v.n_slices());
  for (const SliceContext &c : fresh.context()) {
    const double T = c.T;
    const double F = c.forward;
    EXPECT_TRUE(bits_equal(fresh.forward_at(T), v.forward_at(T))) << "fwd T=" << T;
    EXPECT_TRUE(bits_equal(fresh.q_eff_at(T), v.q_eff_at(T))) << "qeff T=" << T;
    EXPECT_TRUE(bits_equal(fresh.rate_at(T), v.rate_at(T))) << "rate T=" << T;
    for (const double m : {0.90, 0.95, 0.98, 1.0, 1.02, 1.05, 1.10}) {
      const double K = F * m;
      const Side side = (m <= 1.0) ? Side::Put : Side::Call;

      EXPECT_TRUE(bits_equal(fresh.iv(K, T), v.iv(K, T))) << "iv K=" << K << " T=" << T;
      EXPECT_TRUE(bits_equal(fresh.total_variance(K, T), v.total_variance(K, T)))
          << "w K=" << K << " T=" << T;
      if (!std::isfinite(fresh.iv(K, T))) {
        continue;
      }
      const auto fa = fresh.fair_value(K, T, side);
      const auto fv = v.fair_value(K, T, side);
      ASSERT_EQ(fa.has_value(), fv.has_value());
      if (fa.has_value()) {
        EXPECT_TRUE(bits_equal(*fa, *fv)) << "fv K=" << K << " T=" << T;
        ++n_fv;
      }
      const auto ga = fresh.greeks(K, T, side);
      const auto gv = v.greeks(K, T, side);
      ASSERT_EQ(ga.has_value(), gv.has_value());
      if (ga.has_value()) {
        EXPECT_TRUE(bits_equal(ga->price, gv->price)) << "price K=" << K;
        EXPECT_TRUE(bits_equal(ga->delta, gv->delta)) << "delta K=" << K;
        EXPECT_TRUE(bits_equal(ga->gamma, gv->gamma)) << "gamma K=" << K;
        EXPECT_TRUE(bits_equal(ga->vega, gv->vega)) << "vega K=" << K;
        EXPECT_TRUE(bits_equal(ga->theta, gv->theta)) << "theta K=" << K;
        EXPECT_TRUE(bits_equal(ga->rho, gv->rho)) << "rho K=" << K;
        EXPECT_TRUE(bits_equal(ga->vanna, gv->vanna)) << "vanna K=" << K;
        EXPECT_TRUE(bits_equal(ga->volga, gv->volga)) << "volga K=" << K;
        EXPECT_TRUE(bits_equal(ga->charm, gv->charm)) << "charm K=" << K;
      }
    }
  }
}

// evaluate_batch parity (the pricer's primary hot kernel) through the v2 view:
// the fused-batch SoA path a portfolio price/greeks call takes must match the
// fresh surface bit-for-bit. Grid is built off the fresh surface's slice forwards.
void expect_view_batch_reproduces_fresh_fit(const PricedSurface &fresh, const PricedSurfaceView &v) {
  using EF = PricedSurface::EvalField;
  std::vector<double> K, T;
  std::vector<Side> side;
  bool flip = false;
  for (const SliceContext &c : fresh.context()) {
    for (const double m : {0.95, 1.0, 1.05}) {
      K.push_back(c.forward * m);
      T.push_back(c.T);
      side.push_back(flip ? Side::Call : Side::Put);
      flip = !flip;
    }
  }
  const std::size_t n = K.size();
  ASSERT_GT(n, 0u);
  std::vector<double> iv_a(n), iv_v(n), px_a(n), px_v(n);
  std::vector<AmericanGreeks> gr_a(n), gr_v(n);
  std::vector<atx::vol::Status> st_a(n), st_v(n);
  PricedSurface::EvaluationSoA out_a{iv_a, px_a, gr_a, st_a, {}, {}};
  PricedSurface::EvaluationSoA out_v{iv_v, px_v, gr_v, st_v, {}, {}};
  const EF fields = EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder;
  const auto sa = fresh.evaluate_batch(K, T, side, fields, false, out_a);
  const auto sv = v.evaluate_batch(K, T, side, fields, false, out_v);
  ASSERT_EQ(sa.has_value(), sv.has_value());
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(st_a[i].has_value(), st_v[i].has_value()) << "batch status i=" << i;
    EXPECT_TRUE(bits_equal(iv_a[i], iv_v[i])) << "batch iv i=" << i;
    EXPECT_TRUE(bits_equal(px_a[i], px_v[i])) << "batch px i=" << i;
    EXPECT_TRUE(bits_equal(gr_a[i].delta, gr_v[i].delta)) << "batch delta i=" << i;
    EXPECT_TRUE(bits_equal(gr_a[i].gamma, gr_v[i].gamma)) << "batch gamma i=" << i;
    EXPECT_TRUE(bits_equal(gr_a[i].vega, gr_v[i].vega)) << "batch vega i=" << i;
  }
}

// The layout board set: 2 dates x 2 symbols. "SPY" pins ConvexDense (dense index
// recipe); "XOM" auto-selects (=> eSSVI on the smooth truth). A genuine mix of
// curve families in the archive.
[[nodiscard]] std::vector<CorpusBoard> make_mixed_boards(const std::vector<std::string> &dates) {
  std::vector<CorpusBoard> boards;
  for (const std::string &d : dates) {
    boards.push_back(
        board_from_spec(make_index_spec("SPY", d, 600.0), d, "SPY", convex_dense_pin()));
    boards.push_back(board_from_spec(make_singlename_spec("XOM", d, 110.0), d, "XOM"));
  }
  return boards;
}

} // namespace

// ── 1. Layout + curve-family mix ────────────────────────────────────────────
TEST(Corpus, BuildCorpus_MultiDateMultiSymbol_LaysOutOneArchivePerDate) {
  const fs::path out = fresh_out_dir("layout");
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18"};

  auto man_res = build_corpus(make_mixed_boards(dates), out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest &man = *man_res;

  // Counts + dates.
  EXPECT_EQ(man.n_boards, 4u);
  EXPECT_EQ(man.n_ok, 4u);
  EXPECT_EQ(man.n_failed, 0u);
  EXPECT_EQ(man.n_skipped, 0u);
  ASSERT_EQ(man.dates.size(), 2u);
  EXPECT_EQ(man.dates[0], "2026-06-17");
  EXPECT_EQ(man.dates[1], "2026-06-18");

  // One archive file per date + the manifest.
  for (const std::string &d : dates) {
    EXPECT_TRUE(fs::exists(out / (d + ".atxvsa"))) << d;
  }
  EXPECT_TRUE(fs::exists(out / "manifest.tsv"));

  // Entries sorted (date asc, symbol asc): (d, SPY), (d, XOM) per date, each
  // curve family as expected (SPY pinned dense; XOM auto -> eSSVI).
  ASSERT_EQ(man.entries.size(), 4u);
  for (const CorpusEntry &e : man.entries) {
    EXPECT_EQ(e.status, CorpusFitStatus::Ok) << e.symbol;
    EXPECT_GT(e.n_slices, 0u);
    EXPECT_FALSE(e.archive_path.empty());
    if (e.symbol == "SPY") {
      EXPECT_EQ(e.chosen_kind, VolCurveKind::ConvexDense) << "SPY pinned ConvexDense";
    } else if (e.symbol == "XOM") {
      EXPECT_EQ(e.chosen_kind, VolCurveKind::Essvi) << "XOM auto-selects eSSVI";
    }
  }
  EXPECT_EQ(man.entries[0].date, "2026-06-17");
  EXPECT_EQ(man.entries[0].symbol, "SPY");
  EXPECT_EQ(man.entries[1].symbol, "XOM");

  std::printf("[corpus] boards=%u dates=%zu ok=%u failed=%u skipped=%u | "
              "SPY=%s XOM=%s\n",
              man.n_boards, man.dates.size(), man.n_ok, man.n_failed, man.n_skipped,
              to_string(man.entries[0].chosen_kind), to_string(man.entries[1].chosen_kind));
}

// ── 2/3. Bit-identical reload (both curve families) ─────────────────────────
TEST(Corpus, RoundTrip_ReloadedSurfaceReproducesFreshFitBitIdentical) {
  const fs::path out = fresh_out_dir("roundtrip");
  const std::vector<CorpusBoard> boards = make_mixed_boards({"2026-06-17"});

  auto man_res = build_corpus(boards, out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest &man = *man_res;

  std::size_t n_checked = 0;
  std::size_t n_points = 0;
  for (const CorpusEntry &e : man.entries) {
    if (e.status != CorpusFitStatus::Ok) {
      continue;
    }
    // Reopen the date's archive and reconstruct the surface.
    auto arch = SurfaceArchiveV2::open_file(e.archive_path);
    ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
    auto reloaded = arch->reconstruct_symbol(e.symbol);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error().to_string();

    // Reproduce the board's fit inline (deterministic) and compare bit-for-bit.
    const CorpusBoard *board = nullptr;
    for (const CorpusBoard &b : boards) {
      if (b.date == e.date && b.symbol == e.symbol) {
        board = &b;
        break;
      }
    }
    ASSERT_NE(board, nullptr);
    const PricedSurface fresh = fit_reference(*board);
    expect_surfaces_bit_identical(*reloaded, fresh, n_points);
    ++n_checked;
  }
  EXPECT_EQ(n_checked, 2u);
  EXPECT_GT(n_points, 20u) << "too few priced points to be a meaningful gate";
}

// ── F-a (WS-F): fit PIPELINE output re-opened through the v2 ZERO-COPY VIEW ──
// The end-to-end proof the sprint's F-a task asks for: fit a board through the
// real pipeline (build_corpus -> write_surface_archive_v2_file), re-open the
// serialized partition, and read each surface via map_symbol -> PricedSurfaceView
// (the reconstruct-FREE deserialize the backtest hot loop uses), asserting the
// view prices bit-for-bit with a fresh fit. Both curve families (ConvexDense pin +
// eSSVI auto) so the parametric zero-heap view path AND the eager-materialize view
// path (ConvexDense) are both proven off genuine fit output.
TEST(Corpus, FitPipelineOutput_ReadThroughV2View_ReproducesFreshFitBitIdentical) {
  const fs::path out = fresh_out_dir("v2view");
  const std::vector<CorpusBoard> boards = make_mixed_boards({"2026-06-17"});

  auto man_res = build_corpus(boards, out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest &man = *man_res;

  std::size_t n_checked = 0;
  std::size_t n_points = 0;
  for (const CorpusEntry &e : man.entries) {
    if (e.status != CorpusFitStatus::Ok) {
      continue;
    }
    // Re-open the date's v2 partition and map ONLY this symbol's record (subset
    // map — no whole-board reconstruct).
    auto arch = SurfaceArchiveV2::open_file(e.archive_path);
    ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
    auto view = arch->map_symbol(e.symbol);
    ASSERT_TRUE(view.has_value()) << view.error().to_string();

    const CorpusBoard *board = nullptr;
    for (const CorpusBoard &b : boards) {
      if (b.date == e.date && b.symbol == e.symbol) {
        board = &b;
        break;
      }
    }
    ASSERT_NE(board, nullptr);
    const PricedSurface fresh = fit_reference(*board);
    EXPECT_EQ(view->kind_at(0), e.chosen_kind) << e.symbol;
    expect_view_reproduces_fresh_fit_bit_identical(fresh, *view, n_points);
    expect_view_batch_reproduces_fresh_fit(fresh, *view);
    ++n_checked;
  }
  EXPECT_EQ(n_checked, 2u);
  EXPECT_GT(n_points, 20u) << "too few priced points to be a meaningful gate";
}

// Unwired C-2: the one production archive writer (build_corpus -> fit_board's
// write path) must plumb the fitter's OWN admitted health/validation digest
// into the archive instead of leaving every entry to decode as
// legacy_surface_provenance() on reload. Build a real corpus, reload each Ok
// entry's provenance, and check it against a fresh fit's own bundle() health
// for the SAME board (health_reference) — not merely "non-default", the exact
// admitted state/digest the fitter computed for that board.
TEST(Corpus, ArchivedProvenanceReflectsFitterHealthNotLegacyDefault) {
  const fs::path out = fresh_out_dir("provenance");
  const std::vector<CorpusBoard> boards = make_mixed_boards({"2026-06-17"});

  auto man_res = build_corpus(boards, out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest &man = *man_res;

  std::size_t n_checked = 0;
  for (const CorpusEntry &e : man.entries) {
    if (e.status != CorpusFitStatus::Ok) {
      continue;
    }
    auto arch = SurfaceArchiveV2::open_file(e.archive_path);
    ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
    auto provenance = arch->provenance(e.symbol);
    ASSERT_TRUE(provenance.has_value()) << provenance.error().to_string();
    EXPECT_FALSE(provenance->legacy_format) << e.symbol;

    const CorpusBoard *board = nullptr;
    for (const CorpusBoard &b : boards) {
      if (b.date == e.date && b.symbol == e.symbol) {
        board = &b;
        break;
      }
    }
    ASSERT_NE(board, nullptr);
    const SurfaceHealth expected = health_reference(*board);
    EXPECT_EQ(provenance->purpose, expected.purpose) << e.symbol;
    EXPECT_EQ(provenance->quality_mode, expected.quality_mode) << e.symbol;
    EXPECT_EQ(provenance->state, expected.state) << e.symbol;
    EXPECT_EQ(provenance->validation.failures, expected.validation.failures) << e.symbol;
    EXPECT_EQ(provenance->validation.validation_id, expected.validation.validation_id) << e.symbol;
    EXPECT_EQ(provenance->source_generation, expected.candidate_generation) << e.symbol;
    EXPECT_EQ(provenance->served_generation, expected.served_generation) << e.symbol;
    ++n_checked;
  }
  EXPECT_EQ(n_checked, 2u);
}

// ── 4. Manifest round-trips ─────────────────────────────────────────────────
//
// Mirrors Manifest_RoundTripsEveryCurveKind below: the manifest is hand-built
// rather than produced by an actual `build_corpus` fit. This test asserts only
// the serializer/parser/file round-trip, so fitting a live corpus (the layout
// BuildCorpus_MultiDateMultiSymbol_LaysOutOneArchivePerDate test above already
// covers the fit -> manifest path) was incidental setup cost, not something
// this test itself exercises. The hand-built manifest carries the same shape
// `make_mixed_boards` would (2 dates, SPY pinned ConvexDense + XOM auto-eSSVI
// per date) so a diff against that test's asserted fields stays meaningful.
TEST(Corpus, Manifest_RoundTrips) {
  const fs::path out = fresh_out_dir("manifest");

  CorpusManifest man;
  man.dates = {"2026-06-17", "2026-06-18"};
  for (const std::string &d : man.dates) {
    CorpusEntry spy;
    spy.date = d;
    spy.symbol = "SPY";
    spy.status = CorpusFitStatus::Ok;
    spy.chosen_kind = VolCurveKind::ConvexDense;
    spy.n_slices = 4u;
    spy.oos_in_band = 0.0; // curve pinned -> 0, matching build_corpus's convention
    spy.archive_path = (out / (d + ".atxvsa")).string();
    man.entries.push_back(spy);

    CorpusEntry xom;
    xom.date = d;
    xom.symbol = "XOM";
    xom.status = CorpusFitStatus::Ok;
    xom.chosen_kind = VolCurveKind::Essvi;
    xom.n_slices = 4u;
    xom.oos_in_band = 0.99;
    xom.archive_path = (out / (d + ".atxvsa")).string();
    man.entries.push_back(xom);
  }
  man.n_boards = static_cast<std::uint32_t>(man.entries.size());
  man.n_ok = man.n_boards;

  // serialize -> parse.
  const std::string tsv = serialize_manifest(man);
  auto parsed = parse_manifest(tsv);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().to_string();
  EXPECT_EQ(*parsed, man);

  // write file -> read file.
  ASSERT_TRUE(write_manifest_file((out / "manifest.tsv").string(), man).has_value());
  auto readback = read_manifest_file((out / "manifest.tsv").string());
  ASSERT_TRUE(readback.has_value()) << readback.error().to_string();
  EXPECT_EQ(*readback, man);

  // A malformed document is rejected.
  EXPECT_FALSE(parse_manifest("not a manifest").has_value());
}

// The writer emits whatever kind the selector chose, and the selector enumerates
// EVERY VolCurveKind. A reader that knows fewer kinds than the writer rejects the
// corpus's own output, so pin the full set rather than the ones in today's
// fixture. (C8 shipped write-only: serialize emitted kind 4, parse rejected it.)
TEST(Corpus, Manifest_RoundTripsEveryCurveKind) {
  constexpr VolCurveKind kAllKinds[]{VolCurveKind::ConvexDense, VolCurveKind::Essvi,
                                     VolCurveKind::Svi, VolCurveKind::LinearVariance,
                                     VolCurveKind::C8};

  CorpusManifest man;
  man.dates = {"2026-06-17"};
  for (const VolCurveKind kind : kAllKinds) {
    CorpusEntry e;
    e.date = "2026-06-17";
    // Sorted (date asc, symbol asc) matches ascending enum value.
    e.symbol = "SYM" + std::to_string(static_cast<int>(kind));
    e.status = CorpusFitStatus::Ok;
    e.chosen_kind = kind;
    e.n_slices = 3u;
    e.oos_in_band = 0.99;
    e.archive_path = "2026-06-17.atxvsa";
    man.entries.push_back(e);
  }
  man.n_boards = static_cast<std::uint32_t>(man.entries.size());
  man.n_ok = man.n_boards;

  auto parsed = parse_manifest(serialize_manifest(man));
  ASSERT_TRUE(parsed.has_value()) << parsed.error().to_string();
  EXPECT_EQ(*parsed, man);
  for (std::size_t i = 0; i < std::size(kAllKinds); ++i) {
    EXPECT_EQ(parsed->entries[i].chosen_kind, kAllKinds[i])
        << "kind " << to_string(kAllKinds[i]) << " did not survive the manifest round-trip";
  }
}

// ── 7. Resilience: one bad board must not sink the corpus ───────────────────
TEST(Corpus, BadBoardsAreRecordedAndSkippedNotFatal) {
  const fs::path out = fresh_out_dir("resilient");

  const auto empty_board = [](std::string date, std::string symbol) {
    CorpusBoard b;
    b.date = std::move(date);
    b.symbol = std::move(symbol); // default frame => rows empty => Skipped
    b.env = MarketEnv::flat(100.0, 0.043, iso_to_ns("2026-06-17"), {});
    return b;
  };

  std::vector<CorpusBoard> boards;
  // Date with one Ok + one Skipped board.
  boards.push_back(board_from_spec(make_index_spec("SPY", "2026-06-17", 600.0), "2026-06-17", "SPY",
                                   convex_dense_pin()));
  boards.push_back(empty_board("2026-06-17", "EMPTY"));
  // Date with ZERO Ok boards => no archive file written for it.
  boards.push_back(empty_board("2026-06-30", "ONLYEMPTY"));

  auto man_res = build_corpus(boards, out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest &man = *man_res;

  EXPECT_EQ(man.n_boards, 3u);
  EXPECT_EQ(man.n_ok, 1u);
  EXPECT_EQ(man.n_skipped, 2u);
  EXPECT_EQ(man.n_failed, 0u);

  // The Ok date has an archive; the zero-Ok date does not.
  EXPECT_TRUE(fs::exists(out / "2026-06-17.atxvsa"));
  EXPECT_FALSE(fs::exists(out / "2026-06-30.atxvsa"));

  for (const CorpusEntry &e : man.entries) {
    if (e.symbol == "SPY") {
      EXPECT_EQ(e.status, CorpusFitStatus::Ok);
      EXPECT_FALSE(e.archive_path.empty());
    } else {
      EXPECT_EQ(e.status, CorpusFitStatus::Skipped) << e.symbol;
      EXPECT_TRUE(e.archive_path.empty()) << e.symbol;
    }
  }

  // Empty inputs are rejected at the boundary.
  EXPECT_FALSE(build_corpus({}, out.string()).has_value());
  EXPECT_FALSE(build_corpus(boards, "").has_value());
}

// ── 6. Determinism across thread counts ─────────────────────────────────────
TEST(Corpus, Deterministic_AcrossThreadCounts) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18"};

  CorpusConfig serial;
  serial.n_threads = 1;
  CorpusConfig parallel;
  parallel.n_threads = 8;

  const fs::path out1 = fresh_out_dir("det-serial");
  const fs::path out8 = fresh_out_dir("det-parallel");
  auto m1 = build_corpus(make_mixed_boards(dates), out1.string(), serial);
  auto m8 = build_corpus(make_mixed_boards(dates), out8.string(), parallel);
  ASSERT_TRUE(m1.has_value()) << m1.error().to_string();
  ASSERT_TRUE(m8.has_value()) << m8.error().to_string();

  // Same counts + dates + per-entry outcome regardless of worker count (only the
  // archive_path differs by output directory).
  EXPECT_EQ(m1->n_boards, m8->n_boards);
  EXPECT_EQ(m1->n_ok, m8->n_ok);
  EXPECT_EQ(m1->dates, m8->dates);
  ASSERT_EQ(m1->entries.size(), m8->entries.size());
  for (std::size_t i = 0; i < m1->entries.size(); ++i) {
    const CorpusEntry &a = m1->entries[i];
    const CorpusEntry &b = m8->entries[i];
    EXPECT_EQ(a.date, b.date);
    EXPECT_EQ(a.symbol, b.symbol);
    EXPECT_EQ(a.status, b.status);
    EXPECT_EQ(a.chosen_kind, b.chosen_kind);
    EXPECT_EQ(a.n_slices, b.n_slices);
    EXPECT_TRUE(bits_equal(a.oos_in_band, b.oos_in_band)) << i;
  }

  // The reloaded surfaces are bit-identical across the two runs too.
  std::size_t n_points = 0;
  for (std::size_t i = 0; i < m1->entries.size(); ++i) {
    const CorpusEntry &a = m1->entries[i];
    const CorpusEntry &b = m8->entries[i];
    if (a.status != CorpusFitStatus::Ok) {
      continue;
    }
    auto arch_a = SurfaceArchiveV2::open_file(a.archive_path);
    auto arch_b = SurfaceArchiveV2::open_file(b.archive_path);
    ASSERT_TRUE(arch_a.has_value() && arch_b.has_value());
    auto sa = arch_a->reconstruct_symbol(a.symbol);
    auto sb = arch_b->reconstruct_symbol(b.symbol);
    ASSERT_TRUE(sa.has_value() && sb.has_value());
    expect_surfaces_bit_identical(*sa, *sb, n_points);
  }
  EXPECT_GT(n_points, 20u);
}

// C2 (perf): the cross-date warm-start chain (CorpusConfig::warm_start_chain)
// groups boards into per-SYMBOL chains fit in date order, carrying correction
// caches forward. Determinism must survive: each chain runs on ONE worker over its
// own date sequence, so the result is INDEPENDENT of worker count. Build the same
// multi-date corpus with warm chains at 1 vs 8 workers and assert bit-identical
// reconstructed surfaces (the C2 chain-determinism gate). This also exercises the
// chain driver end-to-end (grouping, sequencing, reuse via the session stale-gate).
TEST(Corpus, WarmChain_DeterministicAcrossThreadCounts) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19"};

  CorpusConfig serial;
  serial.n_threads = 1;
  serial.warm_start_chain = true;
  CorpusConfig parallel;
  parallel.n_threads = 8;
  parallel.warm_start_chain = true;

  const fs::path out1 = fresh_out_dir("warm-serial");
  const fs::path out8 = fresh_out_dir("warm-parallel");
  auto m1 = build_corpus(make_mixed_boards(dates), out1.string(), serial);
  auto m8 = build_corpus(make_mixed_boards(dates), out8.string(), parallel);
  ASSERT_TRUE(m1.has_value()) << m1.error().to_string();
  ASSERT_TRUE(m8.has_value()) << m8.error().to_string();

  EXPECT_EQ(m1->n_boards, m8->n_boards);
  EXPECT_EQ(m1->n_ok, m8->n_ok);
  EXPECT_EQ(m1->dates, m8->dates);
  ASSERT_EQ(m1->entries.size(), m8->entries.size());
  for (std::size_t i = 0; i < m1->entries.size(); ++i) {
    const CorpusEntry &a = m1->entries[i];
    const CorpusEntry &b = m8->entries[i];
    EXPECT_EQ(a.date, b.date);
    EXPECT_EQ(a.symbol, b.symbol);
    EXPECT_EQ(a.status, b.status);
    EXPECT_EQ(a.chosen_kind, b.chosen_kind);
    EXPECT_EQ(a.n_slices, b.n_slices);
    EXPECT_TRUE(bits_equal(a.oos_in_band, b.oos_in_band)) << i;
  }

  std::size_t n_points = 0;
  for (std::size_t i = 0; i < m1->entries.size(); ++i) {
    const CorpusEntry &a = m1->entries[i];
    const CorpusEntry &b = m8->entries[i];
    if (a.status != CorpusFitStatus::Ok) {
      continue;
    }
    auto arch_a = SurfaceArchiveV2::open_file(a.archive_path);
    auto arch_b = SurfaceArchiveV2::open_file(b.archive_path);
    ASSERT_TRUE(arch_a.has_value() && arch_b.has_value());
    auto sa = arch_a->reconstruct_symbol(a.symbol);
    auto sb = arch_b->reconstruct_symbol(b.symbol);
    ASSERT_TRUE(sa.has_value() && sb.has_value());
    expect_surfaces_bit_identical(*sa, *sb, n_points);
  }
  EXPECT_GT(n_points, 20u);
}

// ── 5. Throughput smoke ─────────────────────────────────────────────────────
// -- Qualified-corpus admission policy ------------------------------------

namespace {

[[nodiscard]] CorpusQualityMetrics passing_quality_metrics() {
  CorpusQualityMetrics q;
  q.profile = ProfileKind::LiquidSingleName;
  q.decision_source = FitDecisionSource::BoardFeatures;
  q.preset = FitPreset::Robust;
  q.primary_kind = VolCurveKind::Essvi;
  q.final_kind = VolCurveKind::Essvi;
  q.provenance_complete = true;
  q.source_schema_version = 2u;
  q.source_fingerprint = 0x1234u;
  q.market_input_fingerprint = 0x5678u;
  q.n_cash_dividends = 2u;
  q.n_raw_quotes = 800u;
  q.n_two_sided = 360u;
  q.n_slices = 6u;
  q.n_holdout = 120u;
  q.n_fit_scorable = 360u;
  q.n_fit_in_band = 345u;
  q.n_oos_in_band = 112u;
  q.fit_in_band = 345.0 / 360.0;
  q.oos_in_band = 112.0 / 120.0;
  q.oos_vega_weighted = 0.95;
  q.oos_vega_weight_in_band = 95.0;
  q.oos_vega_weight_total = 100.0;
  q.mean_vol_rmse = 0.012;
  q.mean_reduced_chi2 = 1.10;
  q.calendar_violations = 0u;
  return q;
}

[[nodiscard]] CorpusAdmissionRule ordinary_liquid_rule() {
  CorpusAdmissionRule rule;
  rule.min_quotes = 300u;
  rule.min_slices = 3u;
  rule.min_holdout = 40u;
  rule.min_fit_in_band = 0.90;
  rule.min_oos_in_band = 0.88;
  rule.min_oos_vega_weighted = 0.90;
  rule.max_mean_vol_rmse = 0.03;
  rule.max_mean_reduced_chi2 = 3.0;
  rule.require_calendar_arb_free = true;
  rule.require_source_provenance = true;
  return rule;
}

[[nodiscard]] CorpusAdmissionPolicy provenance_policy() {
  CorpusAdmissionPolicy policy;
  policy.enabled = true;
  CorpusAdmissionRule rule;
  rule.require_source_provenance = true;
  for (CorpusAdmissionRule &profile_rule : policy.by_profile) {
    profile_rule = rule;
  }
  CorpusAdmissionRule &liquid =
      policy.by_profile[static_cast<std::size_t>(ProfileKind::LiquidSingleName)];
  liquid.min_holdout = 1u;
  liquid.min_oos_in_band = 0.0;
  return policy;
}

} // namespace

TEST(CorpusAdmission, CompleteQualityInsideProfileRuleIsAdmitted) {
  const CorpusAdmissionDecision decision =
      evaluate_corpus_admission(passing_quality_metrics(), ordinary_liquid_rule());

  EXPECT_EQ(decision.disposition, CorpusDisposition::Admitted);
  EXPECT_EQ(decision.primary_reason, CorpusAdmissionReason::None);
  EXPECT_EQ(decision.failed_checks, 0u);
}

TEST(CorpusAdmission, RequiredMetricUnavailableIsNotFabricatedAsZero) {
  CorpusQualityMetrics quality = passing_quality_metrics();
  quality.oos_in_band.reset();

  const CorpusAdmissionDecision decision =
      evaluate_corpus_admission(quality, ordinary_liquid_rule());

  EXPECT_EQ(decision.disposition, CorpusDisposition::Quarantined);
  EXPECT_EQ(decision.primary_reason, CorpusAdmissionReason::QualityUnavailable);
  EXPECT_TRUE(decision.failed(CorpusAdmissionReason::QualityUnavailable));
  EXPECT_FALSE(decision.failed(CorpusAdmissionReason::OosInBandBelowFloor));
}

TEST(CorpusAdmission, NonFiniteMeasuredMetricIsQuarantined) {
  CorpusQualityMetrics quality = passing_quality_metrics();
  quality.mean_vol_rmse = std::numeric_limits<double>::quiet_NaN();

  const CorpusAdmissionDecision decision =
      evaluate_corpus_admission(quality, ordinary_liquid_rule());

  EXPECT_EQ(decision.disposition, CorpusDisposition::Quarantined);
  EXPECT_EQ(decision.primary_reason, CorpusAdmissionReason::NonFiniteMetric);
  EXPECT_TRUE(decision.failed(CorpusAdmissionReason::NonFiniteMetric));
}

TEST(CorpusAdmission, PrimaryReasonPriorityIsStableAndAllFailuresRemainVisible) {
  CorpusQualityMetrics quality = passing_quality_metrics();
  quality.provenance_complete = false;
  quality.n_two_sided = 2u;
  quality.n_slices = 1u;
  quality.n_holdout = 1u;
  quality.fit_in_band = 0.20;
  quality.oos_in_band = 0.10;
  quality.oos_vega_weighted = 0.10;
  quality.mean_vol_rmse = 0.50;
  quality.mean_reduced_chi2 = 50.0;
  quality.calendar_violations = 7u;

  const CorpusAdmissionDecision decision =
      evaluate_corpus_admission(quality, ordinary_liquid_rule());

  EXPECT_EQ(decision.disposition, CorpusDisposition::Quarantined);
  EXPECT_EQ(decision.primary_reason, CorpusAdmissionReason::SourceProvenanceUnavailable);
  for (const CorpusAdmissionReason reason : {
           CorpusAdmissionReason::SourceProvenanceUnavailable,
           CorpusAdmissionReason::TooFewQuotes,
           CorpusAdmissionReason::TooFewSlices,
           CorpusAdmissionReason::TooFewHoldouts,
           CorpusAdmissionReason::CalendarArbitrage,
           CorpusAdmissionReason::InBandBelowFloor,
           CorpusAdmissionReason::OosInBandBelowFloor,
           CorpusAdmissionReason::OosVegaWeightedBelowFloor,
           CorpusAdmissionReason::VolRmseAboveCeiling,
           CorpusAdmissionReason::ReducedChi2AboveCeiling,
       }) {
    EXPECT_TRUE(decision.failed(reason)) << static_cast<unsigned>(reason);
  }
}

TEST(CorpusAdmission, InvalidRulesAndOutOfRangeMeasurementsAreQuarantined) {
  CorpusQualityMetrics quality = passing_quality_metrics();
  CorpusAdmissionRule invalid_rule = ordinary_liquid_rule();
  invalid_rule.min_fit_in_band = 1.01;

  const CorpusAdmissionDecision invalid = evaluate_corpus_admission(quality, invalid_rule);
  EXPECT_EQ(invalid.primary_reason, CorpusAdmissionReason::InvalidRule);
  EXPECT_TRUE(invalid.failed(CorpusAdmissionReason::InvalidRule));

  invalid_rule = ordinary_liquid_rule();
  invalid_rule.calendar_abs_k = 0.0;
  const CorpusAdmissionDecision invalid_calendar = evaluate_corpus_admission(quality, invalid_rule);
  EXPECT_EQ(invalid_calendar.primary_reason, CorpusAdmissionReason::InvalidRule);

  quality.fit_in_band = 1.01;
  const CorpusAdmissionDecision out_of_range =
      evaluate_corpus_admission(quality, ordinary_liquid_rule());
  EXPECT_EQ(out_of_range.primary_reason, CorpusAdmissionReason::MetricOutOfRange);
  EXPECT_TRUE(out_of_range.failed(CorpusAdmissionReason::MetricOutOfRange));

  quality = passing_quality_metrics();
  quality.n_holdout = 0u;
  quality.n_oos_in_band = 0u;
  const CorpusAdmissionDecision inconsistent =
      evaluate_corpus_admission(quality, ordinary_liquid_rule());
  EXPECT_EQ(inconsistent.primary_reason, CorpusAdmissionReason::MetricOutOfRange);
  EXPECT_TRUE(inconsistent.failed(CorpusAdmissionReason::MetricOutOfRange));
}

TEST(CorpusAdmission, SparseProfileCanPassItsOwnEvidenceFloor) {
  CorpusQualityMetrics sparse = passing_quality_metrics();
  sparse.profile = ProfileKind::IlliquidSmallCap;
  sparse.n_two_sided = 24u;
  sparse.n_slices = 3u;
  sparse.n_holdout = 4u;
  sparse.n_fit_scorable = 24u;
  sparse.n_fit_in_band = 23u;
  sparse.n_oos_in_band = 4u;
  sparse.fit_in_band = 23.0 / 24.0;
  sparse.oos_in_band = 1.0;

  CorpusAdmissionRule sparse_rule;
  sparse_rule.min_quotes = 20u;
  sparse_rule.min_slices = 2u;
  sparse_rule.min_holdout = 2u;
  sparse_rule.require_calendar_arb_free = true;
  const CorpusAdmissionDecision sparse_decision = evaluate_corpus_admission(sparse, sparse_rule);
  EXPECT_EQ(sparse_decision.disposition, CorpusDisposition::Admitted);

  CorpusAdmissionRule liquid_rule = sparse_rule;
  liquid_rule.min_quotes = 300u;
  const CorpusAdmissionDecision liquid_decision = evaluate_corpus_admission(sparse, liquid_rule);
  EXPECT_EQ(liquid_decision.primary_reason, CorpusAdmissionReason::TooFewQuotes);
}

TEST(CorpusQualityReport, RoundTripPreservesAbsentMetricsAndQuarantineEvidence) {
  CorpusQualityReport report;
  report.input_fingerprint = 0x0123'4567'89AB'CDEFull;
  report.policy_fingerprint = 0x0FED'CBA9'7654'3210ull;

  QualifiedCorpusEntry entry;
  entry.date = "2026-06-17";
  entry.symbol = "THIN";
  entry.disposition = CorpusDisposition::Quarantined;
  entry.primary_reason = CorpusAdmissionReason::QualityUnavailable;
  entry.failed_checks = CorpusAdmissionFailureMask{1u}
                        << static_cast<unsigned>(CorpusAdmissionReason::QualityUnavailable);
  entry.quality = passing_quality_metrics();
  entry.quality.profile = ProfileKind::IlliquidSmallCap;
  entry.quality.oos_in_band.reset();
  entry.quality.oos_vega_weighted.reset();
  entry.quality.oos_vega_weight_in_band.reset();
  entry.quality.oos_vega_weight_total.reset();
  entry.quality.n_holdout = 0u;
  entry.quality.n_oos_in_band = 0u;
  report.entries.push_back(entry);
  report.n_planned = 1u;
  report.n_quarantined = 1u;

  const std::string text = serialize_quality_report(report);
  EXPECT_NE(text.find("\tNA\tNA\t"), std::string::npos);

  auto parsed = parse_quality_report(text);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().to_string();
  EXPECT_EQ(*parsed, report);
  ASSERT_EQ(parsed->entries.size(), 1u);
  EXPECT_FALSE(parsed->entries[0].quality.oos_in_band.has_value());
  EXPECT_TRUE((parsed->entries[0].failed_checks & entry.failed_checks) != 0u);
}

TEST(CorpusQualityReport, RejectsMalformedCountsAndUnknownEnums) {
  CorpusQualityReport report;
  QualifiedCorpusEntry entry;
  entry.date = "2026-06-17";
  entry.symbol = "GOOD";
  entry.disposition = CorpusDisposition::Admitted;
  entry.primary_reason = CorpusAdmissionReason::None;
  entry.quality = passing_quality_metrics();
  report.entries.push_back(entry);
  report.n_planned = 1u;
  report.n_admitted = 1u;

  std::string bad_counts = serialize_quality_report(report);
  const std::string counts = "counts\t1\t1\t0\t0\t0\t0";
  const std::size_t counts_pos = bad_counts.find(counts);
  ASSERT_NE(counts_pos, std::string::npos);
  bad_counts.replace(counts_pos, counts.size(), "counts\t2\t1\t0\t0\t0\t0");
  EXPECT_FALSE(parse_quality_report(bad_counts).has_value());

  std::string bad_enum = serialize_quality_report(report);
  const std::string row_prefix = "2026-06-17\tGOOD\t0\t0\t";
  const std::size_t row_pos = bad_enum.find(row_prefix);
  ASSERT_NE(row_pos, std::string::npos);
  bad_enum.replace(row_pos, row_prefix.size(), "2026-06-17\tGOOD\t255\t0\t");
  EXPECT_FALSE(parse_quality_report(bad_enum).has_value());
}

TEST(QualifiedCorpus, QuarantinedFitStaysReportedAndCannotLeakIntoADateArchive) {
  const fs::path out = fresh_out_dir("qualified-admission");
  std::vector<CorpusBoard> boards;
  boards.push_back(board_from_spec(make_index_spec("SPY", "2026-06-17", 600.0), "2026-06-17", "SPY",
                                   convex_dense_pin()));
  boards.push_back(
      board_from_spec(make_singlename_spec("XOM", "2026-06-18", 110.0), "2026-06-18", "XOM"));
  boards[0].source_provenance_complete = true;
  boards[1].source_provenance_complete = false;

  QualifiedCorpusConfig cfg;
  cfg.admission = provenance_policy();
  cfg.input_fingerprint = 0x1234u;
  cfg.policy_fingerprint = 0x5678u;
  auto built = build_qualified_corpus(boards, out.string(), cfg);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();

  EXPECT_EQ(built->manifest.n_boards, 2u);
  EXPECT_EQ(built->manifest.n_ok, 1u);
  EXPECT_EQ(built->manifest.n_failed, 1u);
  ASSERT_EQ(built->manifest.entries.size(), 2u);
  EXPECT_EQ(built->manifest.entries[0].symbol, "SPY");
  EXPECT_EQ(built->manifest.entries[0].status, CorpusFitStatus::Ok);
  EXPECT_FALSE(built->manifest.entries[0].archive_path.empty());
  EXPECT_EQ(built->manifest.entries[1].symbol, "XOM");
  EXPECT_EQ(built->manifest.entries[1].status, CorpusFitStatus::Failed);
  EXPECT_EQ(built->manifest.entries[1].error_code, ErrorCode::Unavailable);
  EXPECT_TRUE(built->manifest.entries[1].archive_path.empty());

  const CorpusQualityReport &quality = built->quality;
  EXPECT_EQ(quality.input_fingerprint, 0x1234u);
  EXPECT_EQ(quality.policy_fingerprint, 0x5678u);
  EXPECT_EQ(quality.n_planned, 2u);
  EXPECT_EQ(quality.n_admitted, 1u);
  EXPECT_EQ(quality.n_quarantined, 1u);
  ASSERT_EQ(quality.entries.size(), 2u);
  EXPECT_EQ(quality.entries[0].disposition, CorpusDisposition::Admitted);
  EXPECT_TRUE(quality.entries[0].quality.provenance_complete);
  EXPECT_FALSE(quality.entries[0].quality.oos_in_band.has_value())
      << "pinned route with OOS disabled must report NA";
  EXPECT_EQ(quality.entries[1].disposition, CorpusDisposition::Quarantined);
  EXPECT_EQ(quality.entries[1].primary_reason, CorpusAdmissionReason::SourceProvenanceUnavailable);
  EXPECT_FALSE(quality.entries[1].quality.provenance_complete);
  EXPECT_GT(quality.entries[1].quality.n_slices, 0u)
      << "quarantine must preserve successful-fit evidence";
  EXPECT_EQ(quality.entries[1].quality.decision_source, FitDecisionSource::TickerPrior);
  EXPECT_TRUE(quality.entries[1].quality.oos_in_band.has_value())
      << "direct route with required OOS must run one-family scoring";
  EXPECT_GT(quality.entries[1].quality.n_holdout, 0u);

  EXPECT_TRUE(fs::exists(out / "2026-06-17.atxvsa"));
  EXPECT_FALSE(fs::exists(out / "2026-06-18.atxvsa"));
  EXPECT_TRUE(fs::exists(out / "manifest.tsv"));
  EXPECT_TRUE(fs::exists(out / "quality.tsv"));
  auto readback = read_quality_report_file((out / "quality.tsv").string());
  ASSERT_TRUE(readback.has_value()) << readback.error().to_string();
  EXPECT_EQ(*readback, quality);
}

TEST(QualifiedCorpus, PinnedMarkFitRetainsParityConsumedByActiveQualityRule) {
  const fs::path out = fresh_out_dir("qualified-pinned-fit-parity");
  CorpusBoard board = board_from_spec(make_index_spec("SPY", "2026-06-17", 600.0), "2026-06-17",
                                      "SPY", convex_dense_pin());
  board.source_provenance_complete = true;

  QualifiedCorpusConfig cfg;
  cfg.build.fit_template.use_correction_cache = false;
  cfg.build.fit_template.use_deam_cache_for_fit = false;
  cfg.admission.enabled = true;
  CorpusAdmissionRule rule;
  rule.min_fit_in_band = 0.0;
  rule.max_mean_vol_rmse = 1.0;
  rule.max_mean_reduced_chi2 = 1.0e9;
  rule.require_calendar_arb_free = false;
  for (CorpusAdmissionRule &profile_rule : cfg.admission.by_profile) {
    profile_rule = rule;
  }

  auto built = build_qualified_corpus(std::span<const CorpusBoard>(&board, 1u), out.string(), cfg);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  ASSERT_EQ(built->quality.entries.size(), 1u);
  const QualifiedCorpusEntry &entry = built->quality.entries.front();
  EXPECT_EQ(entry.disposition, CorpusDisposition::Admitted);
  EXPECT_EQ(entry.primary_reason, CorpusAdmissionReason::None);
  EXPECT_EQ(entry.failed_checks, 0u);
  EXPECT_GT(entry.quality.n_fit_scorable, 0u);
  EXPECT_TRUE(entry.quality.fit_in_band.has_value());
  EXPECT_TRUE(entry.quality.mean_vol_rmse.has_value());
  EXPECT_TRUE(entry.quality.mean_reduced_chi2.has_value());
}

TEST(QualifiedCorpus, SuccessfulOneSidedBoardIsQuarantinedWithExactEvidence) {
  const fs::path out = fresh_out_dir("qualified-one-sided");
  CurveConfig essvi;
  essvi.kind = VolCurveKind::Essvi;
  CorpusBoard board =
      board_from_spec(make_singlename_spec("XOM", "2026-06-17", 110.0), "2026-06-17", "XOM", essvi);
  board.source_provenance_complete = true;
  for (std::size_t i = 0; i < board.frame.rows.size(); i += 4u) {
    board.frame.rows[i].bid = 0.0;
  }

  QualifiedCorpusConfig cfg;
  cfg.admission.enabled = true;
  CorpusAdmissionRule rule;
  rule.min_quotes = 90u;
  rule.min_slices = 3u;
  rule.require_calendar_arb_free = false;
  for (CorpusAdmissionRule &profile_rule : cfg.admission.by_profile) {
    profile_rule = rule;
  }

  auto built = build_qualified_corpus(std::span<const CorpusBoard>(&board, 1u), out.string(), cfg);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  ASSERT_EQ(built->quality.entries.size(), 1u);
  EXPECT_NE(built->quality.input_fingerprint, 0u);
  EXPECT_NE(built->quality.policy_fingerprint, 0u);
  const QualifiedCorpusEntry &entry = built->quality.entries.front();
  EXPECT_EQ(entry.disposition, CorpusDisposition::Quarantined);
  EXPECT_EQ(entry.primary_reason, CorpusAdmissionReason::TooFewQuotes);
  EXPECT_EQ(entry.quality.n_raw_quotes, 104u);
  EXPECT_EQ(entry.quality.n_two_sided, 78u);
  EXPECT_GT(entry.quality.n_slices, 0u);
  EXPECT_EQ(built->manifest.n_failed, 1u);
  EXPECT_FALSE(fs::exists(out / "2026-06-17.atxvsa"));
}

TEST(CorpusAdmission, FixedSeedPropertyBatteryProducesOneDispositionPerCell) {
  std::mt19937_64 rng{0xA7C0'2026u};
  CorpusQualityReport report;
  report.input_fingerprint = 1u;
  report.policy_fingerprint = 2u;
  constexpr std::size_t kBoards = 250u;
  report.entries.reserve(kBoards);
  for (std::size_t i = 0u; i < kBoards; ++i) {
    CorpusQualityMetrics quality = passing_quality_metrics();
    quality.profile = static_cast<ProfileKind>(i % kProfileKindCount);
    quality.n_two_sided = static_cast<std::uint32_t>(10u + rng() % 500u);
    quality.n_slices = static_cast<std::uint32_t>(1u + rng() % 8u);
    quality.n_holdout = static_cast<std::uint32_t>(1u + rng() % 100u);
    quality.n_oos_in_band =
        static_cast<std::uint32_t>(rng() % (static_cast<std::uint64_t>(quality.n_holdout) + 1u));
    quality.oos_in_band =
        static_cast<double>(quality.n_oos_in_band) / static_cast<double>(quality.n_holdout);
    if (i % 11u == 0u) {
      quality.oos_in_band.reset();
      quality.n_holdout = 0u;
      quality.n_oos_in_band = 0u;
      quality.oos_vega_weighted.reset();
      quality.oos_vega_weight_in_band.reset();
      quality.oos_vega_weight_total.reset();
    }
    if (i % 17u == 0u) {
      quality.mean_vol_rmse = std::numeric_limits<double>::quiet_NaN();
    }

    CorpusAdmissionRule rule;
    rule.min_quotes = quality.profile == ProfileKind::IlliquidSmallCap ? 20u : 100u;
    rule.min_slices = 2u;
    rule.min_holdout = 5u;
    rule.min_oos_in_band = 0.50;
    rule.max_mean_vol_rmse = 0.10;
    rule.require_calendar_arb_free = true;
    const CorpusAdmissionDecision decision = evaluate_corpus_admission(quality, rule);
    ASSERT_TRUE(decision.disposition == CorpusDisposition::Admitted ||
                decision.disposition == CorpusDisposition::Quarantined);
    if (decision.disposition == CorpusDisposition::Admitted) {
      EXPECT_EQ(decision.primary_reason, CorpusAdmissionReason::None);
      EXPECT_EQ(decision.failed_checks, 0u);
      ++report.n_admitted;
    } else {
      EXPECT_NE(decision.primary_reason, CorpusAdmissionReason::None);
      EXPECT_TRUE(decision.failed(decision.primary_reason));
      ++report.n_quarantined;
    }

    QualifiedCorpusEntry entry;
    entry.date = "2026-06-17";
    entry.symbol = "FUZZ" + std::to_string(i);
    entry.disposition = decision.disposition;
    entry.primary_reason = decision.primary_reason;
    entry.failed_checks = decision.failed_checks;
    entry.quality = std::move(quality);
    report.entries.push_back(std::move(entry));
  }
  report.n_planned = static_cast<std::uint32_t>(report.entries.size());
  const std::string serialized = serialize_quality_report(report);
  auto parsed = parse_quality_report(serialized);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().to_string();
  EXPECT_EQ(serialize_quality_report(*parsed), serialized)
      << "NaN evidence is compared through canonical artifact bytes";
}

namespace {

[[nodiscard]] Result<std::vector<CorpusBoard>> make_generated_property_boards(std::size_t count) {
  constexpr std::string_view snapshot = "2026-06-17";
  constexpr std::array<std::string_view, 3> expiries = {"2026-07-17", "2026-08-21", "2026-09-18"};
  constexpr std::array<ProfileKind, kProfileKindCount> profiles = {
      ProfileKind::IndexEtfUltraLiquid, ProfileKind::MegaCapEvent,
      ProfileKind::LiquidSingleName,    ProfileKind::OrdinarySingleName,
      ProfileKind::IlliquidSmallCap,    ProfileKind::HtbDividendName,
      ProfileKind::VolProduct};

  std::mt19937_64 rng{0xC0A5'2026u};
  std::uniform_real_distribution<double> unit{0.0, 1.0};
  std::vector<CorpusBoard> boards;
  boards.reserve(count);
  for (std::size_t i = 0u; i < count; ++i) {
    const ProfileKind profile = profiles[i % profiles.size()];
    const double spot = std::exp(std::log(2.0) + unit(rng) * std::log(400.0));
    const double rate = -0.01 + 0.09 * unit(rng);
    const double rate_slope = -0.04 + 0.08 * unit(rng);
    const std::size_t strike_count = 9u + static_cast<std::size_t>(rng() % 5u);

    char symbol_buffer[16]{};
    std::snprintf(symbol_buffer, sizeof symbol_buffer, "F%05zu", i);
    SynthPanelSpec spec;
    spec.uid = symbol_buffer;
    spec.snapshot_iso = std::string(snapshot);
    spec.spot = spot;
    spec.r = rate;
    spec.borrow =
        profile == ProfileKind::HtbDividendName ? 0.03 + 0.12 * unit(rng) : 0.01 * unit(rng);
    spec.half_spread_frac = 0.002 + 0.10 * unit(rng);
    spec.min_half_spread = 0.001 * spot + 0.02 * unit(rng);
    if (profile == ProfileKind::HtbDividendName || i % 7u == 0u) {
      spec.cash_divs.push_back(
          DividendEvent{iso_to_ns("2026-07-01"), spot * (0.001 + 0.02 * unit(rng))});
    }

    const double base_sigma = 0.12 + 0.68 * unit(rng);
    const double skew = -0.9 + 1.1 * unit(rng);
    const double curvature = 0.2 + 1.8 * unit(rng);
    for (std::size_t expiry_index = 0u; expiry_index < expiries.size(); ++expiry_index) {
      const double event_jump = profile == ProfileKind::MegaCapEvent && expiry_index == 0u
                                    ? 0.05 + 0.25 * unit(rng)
                                    : 0.0;
      SynthExpiry expiry;
      expiry.expiry_iso = std::string(expiries[expiry_index]);
      expiry.T = year_fraction(snapshot, expiries[expiry_index]);
      expiry.truth = S3Params{base_sigma + event_jump - 0.02 * expiry_index,
                              2.0 * std::sqrt(expiry.T) * skew, curvature};
      spec.expiries.push_back(std::move(expiry));
    }
    for (std::size_t strike_index = 0u; strike_index < strike_count; ++strike_index) {
      const double fraction = strike_count == 1u ? 0.5
                                                 : static_cast<double>(strike_index) /
                                                       static_cast<double>(strike_count - 1u);
      spec.strikes.push_back(spot * (0.72 + 0.56 * fraction));
    }

    auto panel = make_synthetic_american_panel(spec);
    if (!panel) {
      return Err(panel.error());
    }
    CorpusBoard board;
    board.date = std::string(snapshot);
    board.symbol = symbol_buffer;
    board.frame = std::move(panel->frame);
    board.frame.yc_pillar_t = {0.05, 0.5, 1.5};
    board.frame.yc_pillar_r = {rate, rate + 0.35 * rate_slope, rate + rate_slope};
    board.env = MarketEnv::flat(spot, rate, iso_to_ns(snapshot), spec.cash_divs);
    auto yield = YieldCurve::create(board.frame.yc_pillar_t, board.frame.yc_pillar_r);
    if (!yield) {
      return Err(yield.error());
    }
    board.env.yield = std::move(*yield);
    board.fit_context.profile_override = profile;
    board.fit_context.event_phase =
        profile == ProfileKind::MegaCapEvent ? EventPhase::PreAnnouncement : EventPhase::None;
    board.fit_context.event_distance_days =
        profile == ProfileKind::MegaCapEvent ? std::optional<std::uint32_t>{2u} : std::nullopt;
    board.fit_context.htb = profile == ProfileKind::HtbDividendName;
    board.fit_context.vol_product = profile == ProfileKind::VolProduct;
    board.source_provenance_complete = true;
    board.source_schema_version = 2u;
    board.source_fingerprint = 0x1000u + i;
    board.market_input_fingerprint = 0x2000u + i;
    CurveConfig curve;
    curve.kind = VolCurveKind::ConvexDense;
    board.curve = curve;

    for (std::size_t row_index = 0u; row_index < board.frame.rows.size(); ++row_index) {
      QuoteRow &row = board.frame.rows[row_index];
      row.bid_size = static_cast<std::uint32_t>(1u + rng() % 500u);
      row.ask_size = static_cast<std::uint32_t>(1u + rng() % 500u);
      if ((row_index + i) % 13u == 0u) {
        row.bid = 0.0;
      } else if ((row_index + 3u * i) % 29u == 0u) {
        row.ask = 0.0;
      }
    }

    if (i % 37u == 0u) {
      board.frame.spot = std::numeric_limits<double>::quiet_NaN();
      board.env.spot = std::numeric_limits<double>::quiet_NaN();
    } else if (i % 41u == 0u && !board.frame.rows.empty()) {
      board.frame.rows.front().ask = std::numeric_limits<double>::infinity();
    } else if (i % 43u == 0u) {
      board.frame.rows.clear();
    }
    boards.push_back(std::move(board));
  }
  return atx::core::Ok(std::move(boards));
}

[[nodiscard]] std::vector<char> read_binary_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size <= 0) {
    return {};
  }
  input.seekg(0, std::ios::beg);
  std::vector<char> bytes(static_cast<std::size_t>(size));
  input.read(bytes.data(), size);
  if (!input) {
    return {};
  }
  return bytes;
}

void normalize_output_paths(CorpusManifest &manifest, CorpusQualityReport &quality) {
  for (CorpusEntry &entry : manifest.entries) {
    if (!entry.archive_path.empty()) {
      entry.archive_path = fs::path(entry.archive_path).filename().generic_string();
    }
  }
  for (QualifiedCorpusEntry &entry : quality.entries) {
    if (!entry.archive_path.empty()) {
      entry.archive_path = fs::path(entry.archive_path).filename().generic_string();
    }
  }
}

void exercise_generated_property_corpus(std::size_t count, const char *tag) {
  auto generated = make_generated_property_boards(count);
  ASSERT_TRUE(generated.has_value()) << generated.error().to_string();
  ASSERT_EQ(generated->size(), count);
  std::array<std::size_t, kProfileKindCount> profile_counts{};
  for (const CorpusBoard &board : *generated) {
    ASSERT_TRUE(board.fit_context.profile_override.has_value());
    ++profile_counts[static_cast<std::size_t>(*board.fit_context.profile_override)];
  }
  for (const std::size_t profile_count : profile_counts) {
    EXPECT_GT(profile_count, 0u);
  }

  QualifiedCorpusConfig serial_cfg;
  serial_cfg.build.n_threads = 1u;
  serial_cfg.build.write_opts.created_ts_ns = 1;
  serial_cfg.admission.enabled = true;
  CorpusAdmissionRule rule;
  rule.min_quotes = 6u;
  rule.min_slices = 2u;
  rule.require_calendar_arb_free = true;
  rule.require_source_provenance = true;
  for (CorpusAdmissionRule &profile_rule : serial_cfg.admission.by_profile) {
    profile_rule = rule;
  }
  QualifiedCorpusConfig parallel_cfg = serial_cfg;
  parallel_cfg.build.n_threads = 4u;

  const fs::path serial_out = fresh_out_dir((std::string(tag) + "-serial").c_str());
  const fs::path parallel_out = fresh_out_dir((std::string(tag) + "-parallel").c_str());
  auto serial = build_qualified_corpus(*generated, serial_out.string(), serial_cfg);
  auto parallel = build_qualified_corpus(*generated, parallel_out.string(), parallel_cfg);
  ASSERT_TRUE(serial.has_value()) << serial.error().to_string();
  ASSERT_TRUE(parallel.has_value()) << parallel.error().to_string();

  ASSERT_EQ(serial->quality.entries.size(), count);
  EXPECT_EQ(serial->quality.n_planned, count);
  EXPECT_EQ(static_cast<std::uint64_t>(serial->quality.n_admitted) + serial->quality.n_quarantined +
                serial->quality.n_source_failed + serial->quality.n_fit_failed +
                serial->quality.n_empty,
            count);
  EXPECT_GT(serial->quality.n_admitted, 0u);
  EXPECT_GT(serial->quality.n_fit_failed + serial->quality.n_empty, 0u);
  for (const QualifiedCorpusEntry &entry : serial->quality.entries) {
    if (entry.disposition == CorpusDisposition::Admitted) {
      EXPECT_EQ(entry.primary_reason, CorpusAdmissionReason::None) << entry.symbol;
      ASSERT_TRUE(entry.quality.calendar_violations.has_value()) << entry.symbol;
      EXPECT_EQ(*entry.quality.calendar_violations, 0u) << entry.symbol;
      EXPECT_TRUE(entry.quality.final_kind_consistent) << entry.symbol;
    } else {
      EXPECT_NE(entry.primary_reason, CorpusAdmissionReason::None) << entry.symbol;
    }
  }

  CorpusManifest serial_manifest = serial->manifest;
  CorpusManifest parallel_manifest = parallel->manifest;
  CorpusQualityReport serial_quality = serial->quality;
  CorpusQualityReport parallel_quality = parallel->quality;
  normalize_output_paths(serial_manifest, serial_quality);
  normalize_output_paths(parallel_manifest, parallel_quality);
  EXPECT_EQ(serial_manifest, parallel_manifest);
  EXPECT_EQ(serial_quality, parallel_quality);

  const fs::path serial_archive = serial_out / "2026-06-17.atxvsa";
  const fs::path parallel_archive = parallel_out / "2026-06-17.atxvsa";
  const std::vector<char> serial_bytes = read_binary_file(serial_archive);
  const std::vector<char> parallel_bytes = read_binary_file(parallel_archive);
  ASSERT_FALSE(serial_bytes.empty());
  EXPECT_EQ(serial_bytes, parallel_bytes);

  auto serial_open = SurfaceArchiveV2::open_file(serial_archive.generic_string());
  auto parallel_open = SurfaceArchiveV2::open_file(parallel_archive.generic_string());
  ASSERT_TRUE(serial_open.has_value()) << serial_open.error().to_string();
  ASSERT_TRUE(parallel_open.has_value()) << parallel_open.error().to_string();
  auto serial_surfaces = serial_open->reconstruct_all();
  auto parallel_surfaces = parallel_open->reconstruct_all();
  ASSERT_TRUE(serial_surfaces.has_value()) << serial_surfaces.error().to_string();
  ASSERT_TRUE(parallel_surfaces.has_value()) << parallel_surfaces.error().to_string();
  ASSERT_EQ(serial_surfaces->size(), serial->quality.n_admitted);
  ASSERT_EQ(serial_surfaces->size(), parallel_surfaces->size());
  std::size_t query_points = 0u;
  for (std::size_t surface_index = 0u; surface_index < serial_surfaces->size(); ++surface_index) {
    const PricedSurface &surface = (*serial_surfaces)[surface_index];
    const PricedSurface &parallel_surface = (*parallel_surfaces)[surface_index];
    for (const double k : {-0.10, 0.0, 0.10}) {
      double previous_w = 0.0;
      for (const auto &slice : surface.surface().slices()) {
        const double w = slice->w(k);
        const double iv = surface.iv(slice->F() * std::exp(k), slice->T());
        EXPECT_TRUE(std::isfinite(w) && w > 0.0);
        EXPECT_TRUE(std::isfinite(iv) && iv > 0.0);
        EXPECT_GE(w + 1.0e-12, previous_w);
        previous_w = w;
      }
    }
    expect_surfaces_bit_identical(surface, parallel_surface, query_points);
  }
  EXPECT_GT(query_points, serial_surfaces->size());
}

[[nodiscard]] bool long_corpus_enabled() noexcept {
#if defined(_WIN32)
  char *value = nullptr;
  std::size_t size = 0u;
  const bool present = ::_dupenv_s(&value, &size, "ATX_VOL_LONG_CORPUS") == 0 && value != nullptr;
  const bool enabled = present && value[0] != '\0' && std::strcmp(value, "0") != 0;
  std::free(value);
  return enabled;
#else
  const char *value = std::getenv("ATX_VOL_LONG_CORPUS");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
#endif
}

} // namespace

TEST(CorpusGeneratedProperty, FixedSeedGeneratedPropertyGate) {
  // Default fast gate: 56 boards covers all 7 profile kinds + all failure-injection
  // classes + parallel==serial determinism. The 250/10k scale sweeps live behind
  // ATX_VOL_LONG_CORPUS.
  exercise_generated_property_corpus(56u, "generated-56");
}

TEST(CorpusGeneratedProperty, LongFixedSeedTenThousandBoardGate) {
  if (!long_corpus_enabled()) {
    GTEST_SKIP() << "long corpus; set ATX_VOL_LONG_CORPUS=1 to run the 250 + 10,000 board sweeps";
  }
  // Preserve the old 250-board coverage on demand, then the full 10k scale sweep.
  exercise_generated_property_corpus(250u, "generated-250");
  exercise_generated_property_corpus(10'000u, "generated-10000");
}

TEST(CorpusBuildSession, StreamsDatesRetainsSourceFailuresAndBoundsLiveSurfaces) {
  const fs::path out = fresh_out_dir("streaming-qualified");
  QualifiedCorpusConfig cfg;
  cfg.admission = provenance_policy();
  cfg.build.write_opts.created_ts_ns = 1;
  auto session = CorpusBuildSession::create(out.string(), cfg);
  ASSERT_TRUE(session.has_value()) << session.error().to_string();

  CorpusBoard spy = board_from_spec(make_index_spec("SPY", "2026-06-17", 600.0), "2026-06-17",
                                    "SPY", convex_dense_pin());
  spy.source_provenance_complete = true;
  CorpusSourceFailure missing;
  missing.date = "2026-06-17";
  missing.symbol = "missing";
  missing.reason = CorpusAdmissionReason::MissingSource;
  missing.error_code = ErrorCode::NotFound;
  missing.source_schema_version = 2u;
  missing.market_input_fingerprint = 0xA1u;
  const std::array<CorpusCellInput, 2> first = {CorpusCellInput{spy}, CorpusCellInput{missing}};
  ASSERT_TRUE(session->append_date("2026-06-17", first).has_value());

  EXPECT_FALSE(session->append_date("2026-06-17", first).has_value())
      << "date commits must be strictly ascending";

  CorpusBoard xom =
      board_from_spec(make_singlename_spec("XOM", "2026-06-18", 110.0), "2026-06-18", "XOM");
  xom.source_provenance_complete = false;
  const std::array<CorpusCellInput, 1> second = {CorpusCellInput{xom}};
  ASSERT_TRUE(session->append_date("2026-06-18", second).has_value());

  auto built = session->finish();
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  EXPECT_EQ(built->manifest.n_boards, 3u);
  EXPECT_EQ(built->manifest.n_ok, 1u);
  EXPECT_EQ(built->manifest.n_failed, 2u);
  EXPECT_EQ(built->quality.n_planned, 3u);
  EXPECT_EQ(built->quality.n_admitted, 1u);
  EXPECT_EQ(built->quality.n_quarantined, 1u);
  EXPECT_EQ(built->quality.n_source_failed, 1u);
  EXPECT_EQ(built->peak_live_fitted_surfaces, 1u);
  EXPECT_NE(built->quality.input_fingerprint, 0u);
  EXPECT_NE(built->quality.policy_fingerprint, 0u);
  ASSERT_EQ(built->quality.entries.size(), 3u);
  EXPECT_EQ(built->quality.entries[0].symbol, "MISSING");
  EXPECT_EQ(built->quality.entries[0].disposition, CorpusDisposition::SourceFailed);
  EXPECT_EQ(built->quality.entries[0].primary_reason, CorpusAdmissionReason::MissingSource);
  EXPECT_EQ(built->quality.entries[1].symbol, "SPY");
  EXPECT_EQ(built->quality.entries[2].symbol, "XOM");
  EXPECT_EQ(built->quality.entries[2].disposition, CorpusDisposition::Quarantined);

  EXPECT_TRUE(fs::exists(out / "2026-06-17.atxvsa"));
  EXPECT_FALSE(fs::exists(out / "2026-06-18.atxvsa"));
  auto quality = read_quality_report_file((out / "quality.tsv").string());
  auto manifest = read_manifest_file((out / "manifest.tsv").string());
  ASSERT_TRUE(quality.has_value() && manifest.has_value());
  EXPECT_EQ(*quality, built->quality);
  EXPECT_EQ(*manifest, built->manifest);
  EXPECT_FALSE(session->append_date("2026-06-19", second).has_value());
}

// ── B1 (perf): batched multi-date fan-out ───────────────────────────────────
namespace {

// Whole-file bytes, for archive-level identity assertions. Byte comparison is
// deliberate: the entries/manifest agreeing is NOT evidence the fitted surface
// bytes agree, and an iterative solver reaching a value "to tolerance" is
// exactly the failure this gate exists to catch.
[[nodiscard]] std::string read_all_bytes(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Drive a session over `dates`, `per_flush` dates per append_dates call.
// per_flush == 1 is exactly the historical one-pool-per-date path.
[[nodiscard]] Result<QualifiedCorpusManifest>
build_batched(const fs::path &out, const std::vector<std::string> &dates, std::size_t per_flush,
              unsigned n_threads, std::int64_t created_ts = 1, bool scrub = true,
              const CorpusFitTestHooks *hooks = nullptr) {
  QualifiedCorpusConfig cfg;
  cfg.build.n_threads = n_threads;
  cfg.build.test_hooks = hooks;
  cfg.verify_checkpoint_payload_crc = scrub;
  // 1 pins an explicit stamp (the historical byte-identity tests); 0 leaves the
  // production sentinel, which the writer now fills from an archive-content hash.
  cfg.build.write_opts.created_ts_ns = created_ts;
  auto session = CorpusBuildSession::create(out.string(), cfg);
  if (!session) {
    return Err(session.error());
  }
  const std::vector<CorpusBoard> boards = make_mixed_boards(dates);
  std::vector<std::vector<CorpusCellInput>> cells_by_date(dates.size());
  for (const CorpusBoard &board : boards) {
    const std::size_t d = static_cast<std::size_t>(
        std::find(dates.begin(), dates.end(), board.date) - dates.begin());
    cells_by_date[d].emplace_back(board);
  }
  for (std::size_t first = 0; first < dates.size(); first += per_flush) {
    const std::size_t last = std::min(first + per_flush, dates.size());
    std::vector<CorpusBuildSession::DateCells> window;
    for (std::size_t i = first; i < last; ++i) {
      window.push_back(CorpusBuildSession::DateCells{dates[i], cells_by_date[i]});
    }
    const Status appended = session->append_dates(window);
    if (!appended) {
      return Err(appended.error());
    }
  }
  return session->finish();
}

// Replace every occurrence of `dir` with a fixed token. The manifest and quality
// indexes embed each archive's ABSOLUTE path, so two builds of identical content
// into different out_dirs necessarily differ in those bytes -- a difference that
// says nothing about the fit. Normalizing the directory (and only the directory)
// keeps the comparison honest: the archive FILENAME, and every other field, still
// has to match exactly.
[[nodiscard]] std::string with_dir_normalized(const std::string &text, const fs::path &dir) {
  const std::string needle = dir.generic_string();
  std::string out = text;
  for (std::size_t at = out.find(needle); at != std::string::npos;
       at = out.find(needle, at + 5u)) {
    out.replace(at, needle.size(), "<OUT>");
  }
  return out;
}

void expect_corpora_byte_identical(const fs::path &lhs_dir, const fs::path &rhs_dir,
                                   const std::vector<std::string> &dates) {
  // The archives carry the fitted surfaces; these are the bytes that matter and
  // they are compared raw, with no normalization of any kind.
  for (const std::string &date : dates) {
    const fs::path a = lhs_dir / (date + ".atxvsa");
    const fs::path b = rhs_dir / (date + ".atxvsa");
    ASSERT_TRUE(fs::exists(a)) << a.string();
    ASSERT_TRUE(fs::exists(b)) << b.string();
    const std::string bytes_a = read_all_bytes(a);
    const std::string bytes_b = read_all_bytes(b);
    EXPECT_FALSE(bytes_a.empty()) << date;
    EXPECT_EQ(bytes_a, bytes_b) << "archive bytes diverged on " << date;
  }
  for (const char *index : {"manifest.tsv", "quality.tsv"}) {
    EXPECT_EQ(with_dir_normalized(read_all_bytes(lhs_dir / index), lhs_dir),
              with_dir_normalized(read_all_bytes(rhs_dir / index), rhs_dir))
        << index << " diverged beyond its out_dir prefix";
  }
}

// In-memory manifest/quality equality with the same out_dir caveat: compare every
// field, but compare archive paths by FILENAME rather than absolute path.
void expect_manifests_equivalent(const QualifiedCorpusManifest &lhs,
                                 const QualifiedCorpusManifest &rhs) {
  EXPECT_EQ(lhs.manifest.dates, rhs.manifest.dates);
  EXPECT_EQ(lhs.manifest.n_boards, rhs.manifest.n_boards);
  EXPECT_EQ(lhs.manifest.n_ok, rhs.manifest.n_ok);
  EXPECT_EQ(lhs.manifest.n_failed, rhs.manifest.n_failed);
  EXPECT_EQ(lhs.manifest.n_skipped, rhs.manifest.n_skipped);
  EXPECT_EQ(lhs.quality.input_fingerprint, rhs.quality.input_fingerprint);
  EXPECT_EQ(lhs.quality.policy_fingerprint, rhs.quality.policy_fingerprint);
  EXPECT_EQ(lhs.quality.n_admitted, rhs.quality.n_admitted);
  EXPECT_EQ(lhs.quality.n_quarantined, rhs.quality.n_quarantined);
  EXPECT_EQ(lhs.quality.n_source_failed, rhs.quality.n_source_failed);

  ASSERT_EQ(lhs.manifest.entries.size(), rhs.manifest.entries.size());
  for (std::size_t i = 0; i < lhs.manifest.entries.size(); ++i) {
    const CorpusEntry &a = lhs.manifest.entries[i];
    const CorpusEntry &b = rhs.manifest.entries[i];
    EXPECT_EQ(a.date, b.date) << i;
    EXPECT_EQ(a.symbol, b.symbol) << i;
    EXPECT_EQ(a.status, b.status) << i;
    EXPECT_EQ(a.chosen_kind, b.chosen_kind) << i;
    EXPECT_EQ(a.n_slices, b.n_slices) << i;
    EXPECT_EQ(a.error_code, b.error_code) << i;
    EXPECT_TRUE(bits_equal(a.oos_in_band, b.oos_in_band)) << i;
    EXPECT_EQ(fs::path(a.archive_path).filename(), fs::path(b.archive_path).filename()) << i;
  }
  ASSERT_EQ(lhs.quality.entries.size(), rhs.quality.entries.size());
  for (std::size_t i = 0; i < lhs.quality.entries.size(); ++i) {
    const QualifiedCorpusEntry &a = lhs.quality.entries[i];
    const QualifiedCorpusEntry &b = rhs.quality.entries[i];
    EXPECT_EQ(a.date, b.date) << i;
    EXPECT_EQ(a.symbol, b.symbol) << i;
    EXPECT_EQ(a.disposition, b.disposition) << i;
    EXPECT_EQ(a.primary_reason, b.primary_reason) << i;
    EXPECT_EQ(a.failed_checks, b.failed_checks) << i;
  }
}

} // namespace

// B1: batching several dates into ONE fit fan-out must not move a single output
// byte. `fit_board` is pure w.r.t. shared state and this path arms no warm-start
// chain, so a board's bytes cannot depend on which other boards share its pool --
// but that is an argument, not a measurement, so assert the archive bytes.
TEST(CorpusBuildSession, BatchedAppendIsByteIdenticalToPerDate) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19", "2026-06-22"};
  const fs::path per_date = fresh_out_dir("batch-per-date");
  const fs::path batched = fresh_out_dir("batch-all-dates");

  auto one = build_batched(per_date, dates, 1u, 4u);           // one pool per date
  auto all = build_batched(batched, dates, dates.size(), 4u);  // one pool, all dates
  ASSERT_TRUE(one.has_value()) << one.error().to_string();
  ASSERT_TRUE(all.has_value()) << all.error().to_string();

  expect_manifests_equivalent(*one, *all);
  expect_corpora_byte_identical(per_date, batched, dates);
}

// B1: the sprint's headline guarantee -- output is invariant to worker count --
// must survive the restructuring. With a batched fan-out the pool now interleaves
// boards from DIFFERENT dates, so if any cross-board state had leaked in, worker
// count would change completion order and therefore the bytes. 1 vs 8 workers.
TEST(CorpusBuildSession, BatchedAppendDeterministicAcrossThreadCounts) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19", "2026-06-22"};
  const fs::path serial = fresh_out_dir("batch-serial");
  const fs::path parallel = fresh_out_dir("batch-parallel");

  auto s = build_batched(serial, dates, dates.size(), 1u);
  auto p = build_batched(parallel, dates, dates.size(), 8u);
  ASSERT_TRUE(s.has_value()) << s.error().to_string();
  ASSERT_TRUE(p.has_value()) << p.error().to_string();

  expect_manifests_equivalent(*s, *p);
  expect_corpora_byte_identical(serial, parallel, dates);
}

// Corpus reproducibility on the PRODUCTION path: write_opts.created_ts_ns left at
// the 0 sentinel. Before the content-derived fill, that sentinel stamped each
// container from the WALL CLOCK, so two identical builds diverged in the archive
// header timestamp (and ONLY there) -> corpus builds were not byte-reproducible
// run-to-run. The writer now fills the sentinel from a deterministic hash of the
// archive content, so two identical builds are byte-identical AND share a policy
// fingerprint. This fails on the pre-fix writer and passes on the fixed one.
TEST(CorpusBuildSession, DefaultStampBuildsAreByteReproducible) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19", "2026-06-22"};
  const fs::path a = fresh_out_dir("repro-default-a");
  const fs::path b = fresh_out_dir("repro-default-b");

  auto first = build_batched(a, dates, dates.size(), 4u, 0);  // 0 => production sentinel
  auto second = build_batched(b, dates, dates.size(), 4u, 0);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  ASSERT_TRUE(second.has_value()) << second.error().to_string();

  // Archive containers (created_ts_ns header field included) are byte-identical.
  expect_corpora_byte_identical(a, b, dates);
  // And the corpus policy fingerprint is stable run-to-run (and non-trivial).
  EXPECT_EQ(first->quality.policy_fingerprint, second->quality.policy_fingerprint);
  EXPECT_NE(first->quality.policy_fingerprint, 0u);
}

// B1: a partial window must still resume from per-date checkpoints. Build the
// first two dates, then re-drive the WHOLE range in one batch: the already-built
// dates come back from their checkpoints (not refitted) and the result still
// matches a clean single-pass build byte for byte.
TEST(CorpusBuildSession, BatchedAppendResumesFromPerDateCheckpoints) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19", "2026-06-22"};
  const std::vector<std::string> prefix = {"2026-06-17", "2026-06-18"};
  const fs::path resumed = fresh_out_dir("batch-resume");
  const fs::path clean = fresh_out_dir("batch-clean");

  auto partial = build_batched(resumed, prefix, 1u, 4u);
  ASSERT_TRUE(partial.has_value()) << partial.error().to_string();
  // A fresh session over the same out_dir picks the checkpoints up.
  fs::remove(resumed / "manifest.tsv");
  fs::remove(resumed / "quality.tsv");
  auto full = build_batched(resumed, dates, dates.size(), 4u);
  ASSERT_TRUE(full.has_value()) << full.error().to_string();

  auto reference = build_batched(clean, dates, dates.size(), 4u);
  ASSERT_TRUE(reference.has_value()) << reference.error().to_string();
  expect_manifests_equivalent(*full, *reference);
  expect_corpora_byte_identical(resumed, clean, dates);
}

// ── T1 (BT-T1): the DRAINING fan-out reclaims inner fit workers ─────────────
//
// Two rules compose into the BT-T1 worker cap. `run_bounded_fit_tasks` clamps
// its worker count at the TASK count, so an across-board pool wider than the
// board list simply cannot use the surplus. And `build_corpus_core` pins every
// board's INNER fit to a single worker whenever the outer arm is parallel — the
// guard that stops the two levels multiplying into H^2 runnable threads while
// the pool IS saturated. Together: the last boards standing hold the whole
// machine and run on one core each.
//
// A 2-board batch on an 8-wide outer budget is that state for its entire life:
// 2 outer workers busy, 6 idle, both boards pinned to one inner worker. The fix
// hands the idle share of the outer budget to the boards still in flight.
//
// Gated on the phase-timing counters (process-global, never serialized, so they
// cannot move an output byte) rather than a wall clock — this box is shared, and
// a throughput assertion here would be noise. The counter statement is exact:
// every board on the parallel arm reports the inner budget it was offered.
TEST(CorpusBuildSession, DrainingFanOutReclaimsInnerFitWorkers) {
  reset_corpus_phase_timings();
  const std::vector<std::string> dates = {"2026-06-17"}; // make_mixed_boards => 2 boards
  const fs::path out = fresh_out_dir("t1-inner-reclaim");
  auto built = build_batched(out, dates, 1u, 8u); // 8-wide outer budget, 2 boards
  ASSERT_TRUE(built.has_value()) << built.error().to_string();

  const CorpusPhaseTimings t = corpus_phase_timings();
  ASSERT_EQ(t.boards_fitted, 2u);
  EXPECT_EQ(t.reclaimed_inner_boards, 2u)
      << "both boards of a 2-board / 8-worker fan-out must reclaim the 6 outer "
         "workers the pool cannot use as INNER fit parallelism";
  EXPECT_GE(t.inner_worker_slots, 8u)
      << "the reclaimed inner budgets must add up to at least the outer budget";
}

// T1 companion gate: the reclaim changes only HOW MANY workers a board's inner
// fit is offered, never a fitted value. Same boards, outer-serial (which keeps
// the caller's inner budget) vs an 8-wide pool whose surplus the boards reclaim
// — archive bytes must match exactly. This is the thread-count-invariance gate
// for the reclaim; `BatchedAppendDeterministicAcrossThreadCounts` covers the
// saturated-pool case, this one covers the maximally-reclaimed case.
TEST(CorpusBuildSession, InnerWorkerReclaimIsByteIdenticalToSerial) {
  const std::vector<std::string> dates = {"2026-06-17"};
  const fs::path serial = fresh_out_dir("t1-reclaim-serial");
  const fs::path wide = fresh_out_dir("t1-reclaim-wide");

  auto s = build_batched(serial, dates, 1u, 1u);
  auto w = build_batched(wide, dates, 1u, 8u);
  ASSERT_TRUE(s.has_value()) << s.error().to_string();
  ASSERT_TRUE(w.has_value()) << w.error().to_string();

  expect_manifests_equivalent(*s, *w);
  expect_corpora_byte_identical(serial, wide, dates);
}

// ── T-I1 / T-I2 (review rev-ws-t): the DRAIN regime, measured ───────────────
namespace {

// Four dates x make_mixed_boards => 8 boards in ONE fan-out. Against the 4-wide
// outer budget below that is a genuinely SATURATED pool (n > budget), which is
// the regime BT-T1 is about and which no test covered: every gate that existed
// ran 2 boards against an 8-wide budget, i.e. left <= 2 < 8 from the very first
// claim, so the pool was never full and never drained.
const std::vector<std::string> kDrainDates = {"2026-06-17", "2026-06-18", "2026-06-19",
                                              "2026-06-22"};
constexpr unsigned kDrainOuterBudget = 4u;
constexpr std::size_t kDrainBoards = 8u;

} // namespace

// T-I1: "last-board-STANDING", not "last-board-CLAIMED".
//
// BT-T1's symptom is a straggler — the SPY index board — holding the machine at
// low occupancy at the end of a phase. That board is claimed EARLY, while the
// pool is still saturated, so a reclaim evaluated once at claim time offers it
// exactly one inner worker and it keeps exactly one for its entire life no
// matter how empty the machine gets around it. The boards that reclaim anything
// are the ones claimed DURING the drain — which are, by construction, the short
// ones that were never the problem.
//
// So the gate is not "somebody reclaimed": it is that a board claimed while the
// pool was saturated is offered MORE inner workers later in its own life, once
// its siblings have drained. Made deterministic (no sleeps, no timing) with the
// two test hooks: board 0 — the SPY board, first task claimed — blocks inside
// its first budget resolution until `on_board_fit_done` reports that exactly one
// task is left standing (itself), then continues and is re-resolved.
TEST(CorpusBuildSession, StragglerReclaimsInnerWorkersWhileStillRunning) {
  const fs::path out = fresh_out_dir("t1-straggler-growth");

  std::mutex mu;
  std::condition_variable cv;
  bool siblings_drained = false;
  std::vector<unsigned> straggler_offers; // guarded by mu
  std::vector<std::size_t> straggler_left;

  CorpusFitTestHooks hooks;
  hooks.on_board_fit_done = [&](std::size_t, std::size_t unfinished) {
    if (unfinished > 1u) {
      return; // somebody other than the straggler is still running
    }
    const std::lock_guard<std::mutex> lk(mu);
    siblings_drained = true;
    cv.notify_all();
  };
  hooks.on_inner_fit_workers = [&](std::size_t board, std::size_t unfinished, unsigned workers) {
    if (board != 0u) {
      return; // only the straggler is instrumented
    }
    std::unique_lock<std::mutex> lk(mu);
    straggler_offers.push_back(workers);
    straggler_left.push_back(unfinished);
    if (straggler_offers.size() == 1u) {
      // Hold this board IN FLIGHT while every sibling finishes. One of the four
      // outer workers is parked here; the other three drain the remaining seven
      // boards, so this cannot deadlock.
      cv.wait(lk, [&] { return siblings_drained; });
    }
  };

  auto built = build_batched(out, kDrainDates, kDrainDates.size(), kDrainOuterBudget, 1,
                             /*scrub=*/true, &hooks);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();

  const std::lock_guard<std::mutex> lk(mu);
  ASSERT_GE(straggler_offers.size(), 2u)
      << "the straggler's inner fit-worker budget was resolved " << straggler_offers.size()
      << " time(s) for its whole life: it is frozen at CLAIM time, so a board claimed while "
         "the pool is saturated can never pick up the workers its siblings release";
  EXPECT_GE(straggler_left.front(), kDrainOuterBudget)
      << "the straggler must be claimed while the pool is still saturated";
  EXPECT_EQ(straggler_offers.front(), 1u)
      << "a saturated pool must still offer exactly one inner worker per board";
  EXPECT_GT(straggler_offers.back(), straggler_offers.front())
      << "after every sibling drained, the last board STANDING must be offered more inner "
         "workers than it was at claim time (front=" << straggler_offers.front()
      << " back=" << straggler_offers.back() << ")";
  EXPECT_EQ(straggler_offers.back(), kDrainOuterBudget)
      << "the last board standing should be offered the whole outer budget";
}

// T-I2: the drain regime itself, with a pool that genuinely SATURATES.
//
// `DrainingFanOutReclaimsInnerFitWorkers` is named for this regime but runs 2
// boards against an 8-wide budget — `left <= 2 < 8` from the very first claim,
// so the pool is never full and never drains. That is the small-book case, not
// BT-T1's. No test anywhere used n > outer_budget, which is why the claim-time
// reclaim's total inertness on a saturated pool went unnoticed.
//
// 8 boards against a 4-wide budget, asserting both halves of the contract per
// individual offer rather than over process-global sums:
//
//   (a) while the pool is SATURATED (`left >= budget`) every board is offered
//       exactly one inner worker — the "the saturated regime is bit-for-bit
//       unchanged" claim, previously carried only by argument;
//   (b) the budget is RE-RESOLVED as the pool drains rather than frozen at
//       claim, so every board resolves more than once and there are strictly
//       more offers than boards. This is the half a claim-time-only reclaim
//       fails, and the gate that would have caught T-I1.
TEST(CorpusBuildSession, SaturatedFanOutOffersOneWorkerUntilItDrains) {
  reset_corpus_phase_timings();
  const fs::path out = fresh_out_dir("t1-saturated-drain");

  std::mutex mu;
  std::vector<std::pair<std::size_t, unsigned>> offers; // (unfinished, workers)
  std::map<std::size_t, std::size_t> per_board;         // board -> resolutions
  CorpusFitTestHooks hooks;
  hooks.on_inner_fit_workers = [&](std::size_t board, std::size_t unfinished, unsigned workers) {
    const std::lock_guard<std::mutex> lk(mu);
    offers.emplace_back(unfinished, workers);
    ++per_board[board];
  };

  auto built = build_batched(out, kDrainDates, kDrainDates.size(), kDrainOuterBudget, 1,
                             /*scrub=*/true, &hooks);
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  ASSERT_EQ(corpus_phase_timings().boards_fitted, kDrainBoards);

  const std::lock_guard<std::mutex> lk(mu);
  ASSERT_FALSE(offers.empty());
  ASSERT_EQ(per_board.size(), kDrainBoards);
  for (const auto &[board, resolutions] : per_board) {
    EXPECT_GT(resolutions, 1u) << "board " << board
                               << " resolved its inner budget once for its entire life";
  }
  std::size_t saturated = 0;
  std::size_t reclaimed_while_draining = 0;
  for (const auto &[left, workers] : offers) {
    if (left >= kDrainOuterBudget) {
      ++saturated;
      EXPECT_EQ(workers, 1u) << "a SATURATED pool (" << left << " tasks left, " << kDrainOuterBudget
                             << "-wide budget) must keep every board's inner fit serial";
    } else if (workers > 1u) {
      ++reclaimed_while_draining;
    }
  }
  EXPECT_GT(saturated, 0u) << "the fixture must actually saturate the pool (" << kDrainBoards
                           << " boards vs a " << kDrainOuterBudget << "-wide budget)";
  // This is the only assertion in the fixture that pins the drain-time property
  // itself. The two around it are necessary but not sufficient: `saturated`
  // shows the fixture reached saturation, and `offers.size() > kDrainBoards`
  // shows the budget was re-resolved more than once per board -- but neither
  // distinguishes a re-resolution that handed back a WIDER budget from one that
  // returned the same width. Only a width > 1 observed while tasks remain is
  // evidence that a board which was already claimed and still running actually
  // reclaimed. Relax this line and the fixture stops testing T-I1 and starts
  // testing only that the resolver gets called more than once, which is the
  // shape of the vacuous guard the first review rejected.
  EXPECT_GT(reclaimed_while_draining, 0u) << "no board reclaimed anything as the pool drained";
  EXPECT_GT(offers.size(), kDrainBoards)
      << "only " << offers.size() << " budget resolutions for " << kDrainBoards
      << " boards: the inner budget is frozen at claim time instead of being re-resolved as "
         "the fan-out drains";
}

// T-I4: the reclaim counters must be readable from the probe that is supposed
// to read them.
//
// `reclaimed_inner_boards` / `inner_worker_slots` live on `CorpusPhaseTimings`,
// but the only production surface that reports phase timings is the one `PHASE`
// line `dispersion_build_corpus` prints under `ATX_VOL_CORPUS_PHASE_TIMING` —
// and that line did not carry them. The quiet-window probe recommended for the
// utilization row was therefore inert: it could not report either counter, so
// the deterministic substitute offered in place of the "≥ 14/16 mean workers"
// gate was unobtainable, and no evidence could be captured that the reclaim
// fired at all during a production run.
//
// Gated on the pure formatter rather than by driving a corpus build, so the
// assertion is about the line's CONTENTS and cannot be satisfied by a value
// that merely exists in the struct.
TEST(CorpusPhaseLine, ReportsTheInnerWorkerReclaimCounters) {
  CorpusPhaseTimings phases;
  phases.fit_fanout_s = 12.5;
  phases.archive_write_s = 2.0;
  phases.checkpoint_s = 0.5;
  phases.fanout_calls = 11;
  phases.boards_fitted = 902;
  phases.reclaimed_inner_boards = 137;
  phases.inner_worker_slots = 4242;
  phases.fit_fanout_process_cpu_s = 150.0;

  const std::string line = format_corpus_phase_line(/*ingest_s=*/30.0, /*build_s=*/20.0, phases,
                                                    /*date_batch=*/8u,
                                                    /*ingest_process_cpu_s=*/45.0);
  // The pre-existing fields must survive verbatim: this line is parsed by hand
  // and by scripts.
  EXPECT_EQ(line.rfind("PHASE ", 0), 0u) << line;
  for (const char *field : {"ingest_s=30.00", "build_s=20.00", "fit_fanout_s=12.50",
                            "archive_write_s=2.00", "checkpoint_s=0.50", "other_s=5.00",
                            "fanout_calls=11", "boards=902", "date_batch=8"}) {
    EXPECT_NE(line.find(field), std::string::npos) << field << " missing from: " << line;
  }
  // And the two counters the probe exists to report.
  EXPECT_NE(line.find("reclaimed=137"), std::string::npos)
      << "the quiet-window probe cannot see reclaimed_inner_boards: " << line;
  EXPECT_NE(line.find("inner_slots=4242"), std::string::npos)
      << "the quiet-window probe cannot see inner_worker_slots: " << line;
  EXPECT_NE(line.find("fit_fanout_cpu_s=150.00"), std::string::npos)
      << "fit occupancy has no phase-local process-CPU numerator: " << line;
  EXPECT_NE(line.find("ingest_cpu_s=45.00"), std::string::npos)
      << "parallel OPRA ingest CPU is not separately attributable: " << line;
}

// MINORS M9: the PHASE line's field LAYOUT is a contract, and the way it stays
// one is that new fields are APPENDED.
//
// `9cfcbc3` added `reclaimed=` and `inner_slots=` in the middle of the line,
// between `boards=` and `date_batch=`, which moved `date_batch` from field 10
// to field 12. Every in-tree consumer keys by name — `CorpusPhaseLine.
// ReportsTheInnerWorkerReclaimCounters` above is a substring search, and no
// fixture, golden or `.tsv` in the tree mentions the line at all — so nothing
// here broke. What broke is out-of-tree: this line is printed to stdout under
// `ATX_VOL_CORPUS_PHASE_TIMING` and is scraped positionally by operator scripts
// that no change in this repo can reach.
//
// The insertion bought nothing (the two counters read no better beside
// `boards=` than at the end), so this restores field 10 and states the rule
// that keeps it stable: NEW FIELDS APPEND. Position is a courtesy for `awk`;
// the NAME is the contract, and this gate pins both so the next addition
// cannot silently take the courtesy away.
TEST(CorpusPhaseLine, FieldLayoutIsAppendOnlyAndEveryFieldIsSelfDescribing) {
  CorpusPhaseTimings phases;
  phases.fit_fanout_s = 12.5;
  phases.archive_write_s = 2.0;
  phases.checkpoint_s = 0.5;
  phases.fanout_calls = 11;
  phases.boards_fitted = 902;
  phases.reclaimed_inner_boards = 137;
  phases.inner_worker_slots = 4242;
  phases.fit_fanout_process_cpu_s = 150.0;

  const std::string line = format_corpus_phase_line(/*ingest_s=*/30.0, /*build_s=*/20.0, phases,
                                                    /*date_batch=*/8u,
                                                    /*ingest_process_cpu_s=*/45.0);
  std::vector<std::string> fields;
  for (std::size_t cursor = 0; cursor <= line.size();) {
    const std::size_t space = line.find(' ', cursor);
    if (space == std::string::npos) {
      fields.push_back(line.substr(cursor));
      break;
    }
    fields.push_back(line.substr(cursor, space - cursor));
    cursor = space + 1;
  }

  // The layout, in order. The first nine entries are the ORIGINAL line
  // (`6fddfba`); the last two are `9cfcbc3`'s counters, appended.
  static constexpr const char *kLayout[] = {
      "ingest_s",     "build_s", "fit_fanout_s", "archive_write_s", "checkpoint_s", "other_s",
      "fanout_calls", "boards",  "date_batch",   "reclaimed",       "inner_slots",
      "fit_fanout_cpu_s", "ingest_cpu_s"};
  constexpr std::size_t kFieldCount = sizeof(kLayout) / sizeof(kLayout[0]);

  ASSERT_EQ(fields.size(), kFieldCount + 1u) << line;
  EXPECT_EQ(fields[0], "PHASE") << line;
  for (std::size_t i = 0; i < kFieldCount; ++i) {
    const std::string &field = fields[i + 1u];
    const std::size_t equals = field.find('=');
    ASSERT_NE(equals, std::string::npos)
        << "field " << (i + 1u) << " is positional, not name=value: '" << field
        << "' — a bare value makes the ORDER load-bearing for every consumer, which is the "
           "failure this gate exists to prevent: "
        << line;
    EXPECT_EQ(field.substr(0, equals), kLayout[i])
        << "field " << (i + 1u) << " is '" << field.substr(0, equals) << "', expected '"
        << kLayout[i] << "'. New PHASE fields APPEND — inserting one shifts every field after "
        << "it under a positional reader. Line: " << line;
    EXPECT_FALSE(field.substr(equals + 1u).empty()) << "empty value in field: " << field;
  }
  // `date_batch` is field 10 of the record (index 9 counting the PHASE tag as
  // field 1), which is where `6fddfba` put it and where an `awk '{print $10}'`
  // still finds it.
  EXPECT_EQ(fields[9], "date_batch=8") << line;
}

// rev2-ws-t N-M2: `format_corpus_phase_line` formats into a fixed 512-byte
// buffer, and `std::snprintf` returns the length it WOULD have written, not the
// length it did. `std::string(buf, written)` therefore reads `written - 511`
// bytes PAST the end of `buf` the moment the line truncates — an out-of-bounds
// read in a public pure function that runs on EVERY corpus build, whose six
// `double` arguments are all caller-supplied. `/W4 /permissive- /WX` does not
// diagnose it and no sanitizer runs in this suite, so the observable proxy is
// the one invariant a correct implementation cannot violate: the returned
// string can never be longer than the buffer that produced it, and its size
// must agree with its own NUL terminator.
//
// Two `%.2f` conversions of 1e300 are ~304 characters each, so the line
// truncates on its second field with no help from the counters.
TEST(CorpusPhaseLine, TruncatedLineDoesNotOverreadTheFormatBuffer) {
  CorpusPhaseTimings phases;
  phases.fit_fanout_s = 1.0;
  phases.archive_write_s = 2.0;
  phases.checkpoint_s = 3.0;
  phases.fanout_calls = 11;
  phases.boards_fitted = 902;
  phases.reclaimed_inner_boards = 137;
  phases.inner_worker_slots = 4242;

  constexpr std::size_t kFormatBuffer = 512;  // dispersion_run.cpp: char buf[512]
  const std::string line = format_corpus_phase_line(/*ingest_s=*/1e300, /*build_s=*/1e300, phases,
                                                    /*date_batch=*/8u);
  EXPECT_LE(line.size(), kFormatBuffer - 1u)
      << "format_corpus_phase_line returned " << line.size()
      << " bytes from a " << kFormatBuffer
      << "-byte buffer: snprintf's return value is the UNTRUNCATED length, so the "
         "std::string ctor read past the end of the buffer";
  EXPECT_EQ(line.size(), std::strlen(line.c_str()))
      << "the returned string extends past its own NUL terminator — it carries bytes "
         "that were never written by snprintf";
  EXPECT_EQ(line.rfind("PHASE ", 0), 0u) << "truncation must still yield a well-formed prefix";
}

// ── T2 (SE-P2-6 / SE-P2-3): framing-only checkpoint resume + payload scrub ───
namespace {

// Overwrite every surface RECORD BODY (the data section) while leaving the
// header + metadata section (lookup ‖ directory) byte-intact. `open()` is lazy —
// header CRC and metadata CRC still verify, the directory still parses — so an
// enumeration that reads ONLY framing keeps working, while anything that
// materializes a record (map_all's per-record PricedSurfaceView, or a payload
// CRC check) hits the poison. This is the same discriminator WS-C's C6 unit test
// uses (SurfaceArchiveV2.EntriesEnumerateWithoutMaterializingViews).
void poison_archive_record_bodies(const fs::path &archive) {
  std::uint64_t data_offset = 0;
  {
    auto arch = SurfaceArchiveV2::open_file(archive.generic_string());
    ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
    data_offset = arch->header().data_offset;
  }
  std::string bytes = read_all_bytes(archive);
  ASSERT_LT(static_cast<std::size_t>(data_offset), bytes.size());
  for (std::size_t i = static_cast<std::size_t>(data_offset); i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>(0xFF);
  }
  std::ofstream out(archive, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.good());
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// T-I3: media bit-rot INSIDE an otherwise perfectly well-formed record — the
// scenario SE-P2-3 is actually about, and the one `poison_archive_record_bodies`
// above cannot express.
//
// That poison 0xFF-fills from `header().data_offset` to EOF. Records begin AT
// `data_offset`, so it destroys each record's own `ArchiveV2SurfaceHeader.magic`
// too, and `validate_record` rejects on the magic compare BEFORE it ever
// computes the CRC. A verifier that only walked framing would pass that test.
//
// This flips ONE bit deep inside a record's payload instead: the low bit of the
// first slice's `forward` (a f64 in the columnar section). Everything the
// structure depends on is byte-intact — magic, `record_size`, `n_slices`, every
// column offset, the stored `payload_crc32c`, the lookup table and the whole
// directory (so `metadata_crc32c` still verifies and `open()` still succeeds).
// A low mantissa bit also keeps the value a finite, sane forward (relative
// change ~1e-16), so nothing downstream can reject it structurally either. The
// per-record CRC is the ONLY thing in the format that can see it.
void bitrot_one_record_payload_bit(const fs::path &archive) {
  std::uint64_t record_offset = 0;
  {
    auto arch = SurfaceArchiveV2::open_file(archive.generic_string());
    ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
    ASSERT_GT(arch->entry_count(), 0u);
    record_offset = arch->entries()[0].surface_offset;
  }
  std::string bytes = read_all_bytes(archive);
  ASSERT_LE(static_cast<std::size_t>(record_offset) + sizeof(ArchiveV2SurfaceHeader), bytes.size());

  ArchiveV2SurfaceHeader before{};
  std::memcpy(&before, bytes.data() + record_offset, sizeof before);
  ASSERT_EQ(std::memcmp(before.magic, "ATXVSR20", 8), 0);
  ASSERT_GT(before.n_slices, 0u);
  ASSERT_GE(before.col_forward_off, sizeof(ArchiveV2SurfaceHeader))
      << "the forward column must live past the record header, not inside it";

  const std::size_t victim =
      static_cast<std::size_t>(record_offset) + static_cast<std::size_t>(before.col_forward_off);
  ASSERT_LT(victim, bytes.size());
  bytes[victim] = static_cast<char>(static_cast<unsigned char>(bytes[victim]) ^ 0x01u);

  // Prove the poison is payload-only: the record header must come back byte-for
  // byte identical, or this fixture is testing framing again.
  ArchiveV2SurfaceHeader after{};
  std::memcpy(&after, bytes.data() + record_offset, sizeof after);
  ASSERT_EQ(std::memcmp(&before, &after, sizeof before), 0)
      << "the record header must be untouched: this poison exists to reach the CRC compare";

  std::ofstream out(archive, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.good());
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

// T2a (SE-P2-6): checkpoint resume must be O(framing), not O(heavy payload).
//
// `read_date_checkpoint` cross-checks only the surface COUNT and the admitted
// symbols, but reached for `map_all()` to do it — which builds a
// `PricedSurfaceView` per record and EAGERLY materializes ConvexDense/SplineVol
// curves (allocations, node-array copies, spline second-derivative solves). The
// in-code comment claiming the v2 subset-map "touches nothing but framing" was
// simply false for the heavy kinds, and this corpus fixture writes one of each
// (SPY is pinned to the dense recipe).
//
// Observed the way WS-C's own C6 test observes it — by poisoning every record
// body. A framing-only resume does not read those bytes and succeeds; a resume
// that materializes views cannot. The scrub added in T2b is switched OFF here
// precisely so this asserts materialization and nothing else.
TEST(CorpusBuildSession, CheckpointResumeIsFramingOnly) {
  const std::vector<std::string> dates = {"2026-06-17"};
  const fs::path out = fresh_out_dir("t2-framing-resume");

  auto first = build_batched(out, dates, 1u, 4u, 1, /*scrub=*/false);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  ASSERT_TRUE(fs::exists(out / "2026-06-17.atxvsa"));
  poison_archive_record_bodies(out / "2026-06-17.atxvsa");

  // A fresh session over the same out_dir resumes from the per-date checkpoint.
  fs::remove(out / "manifest.tsv");
  fs::remove(out / "quality.tsv");
  auto resumed = build_batched(out, dates, 1u, 4u, 1, /*scrub=*/false);
  ASSERT_TRUE(resumed.has_value())
      << "checkpoint resume materialized record payloads instead of reading "
         "framing only: "
      << resumed.error().to_string();
  expect_manifests_equivalent(*first, *resumed);
}

// T2b (SE-P2-3): the lazy per-record CRC finally gets ONE scheduled verifier.
//
// `validate_symbol`/`validate_all` had zero non-test callers; `open` checks only
// header + metadata; neither the snapshot load nor SurfaceDb validates. So a
// corrupted record body was SERVED — it flowed into prices with nothing in the
// pipeline able to notice. Checkpoint verification is the natural scheduling
// point (already re-opening the archive, once per resumed date), and it is where
// a corrupt payload costs a refit instead of a wrong price.
//
// T-I3: the poison here is ONE FLIPPED BIT inside an otherwise well-formed
// record, not a wiped data section. The wiped-section poison destroys each
// record's own magic, so `validate_record` rejects at the framing compare
// (surface_archive.cpp:1398) and never reaches the CRC compare at :1401 — a
// framing-only walk would have satisfied it, which is not what SE-P2-3 is about.
//
// The test discriminates in three steps, so it can only pass because the CRC is
// actually computed:
//   (i)   the poisoned archive still OPENS and still MATERIALIZES every record —
//         header CRC, metadata CRC, directory and framing all verify, so
//         everything short of the per-record payload CRC is blind to it, while
//         `validate_all()` (which does compute it) rejects;
//   (ii)  with the scrub OFF the corrupt checkpoint is SERVED. That is SE-P2-3
//         as shipped behaviour, observed rather than argued;
//   (iii) with the scrub ON — the `--qualify` default — the resume FAILS with an
//         error naming the site.
TEST(CorpusBuildSession, CheckpointScrubRejectsCorruptedPayload) {
  const std::vector<std::string> dates = {"2026-06-17"};
  const fs::path out = fresh_out_dir("t2-scrub-corrupt");
  const fs::path archive = out / "2026-06-17.atxvsa";

  auto first = build_batched(out, dates, 1u, 4u);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  bitrot_one_record_payload_bit(archive);

  // (i) Everything except the payload CRC is blind to this poison.
  {
    auto arch = SurfaceArchiveV2::open_file(archive.generic_string());
    ASSERT_TRUE(arch.has_value())
        << "the poisoned archive must still OPEN (header + metadata CRC intact): "
        << arch.error().to_string();
    auto mapped = arch->map_all();
    EXPECT_TRUE(mapped.has_value())
        << "the poisoned record must still MATERIALIZE — otherwise this fixture is testing "
           "framing again, not bit-rot: "
        << (mapped.has_value() ? std::string{} : mapped.error().to_string());
    const Status scrub = arch->validate_all();
    ASSERT_FALSE(scrub.has_value()) << "validate_all must catch a flipped payload bit";
    EXPECT_NE(scrub.error().to_string().find("checksum"), std::string::npos)
        << "the rejection must come from the CRC compare, not the framing compare: "
        << scrub.error().to_string();
  }

  // (ii) Scrub OFF: the corrupt checkpoint is served. This is the SE-P2-3 defect.
  fs::remove(out / "manifest.tsv");
  fs::remove(out / "quality.tsv");
  auto unverified = build_batched(out, dates, 1u, 4u, 1, /*scrub=*/false);
  ASSERT_TRUE(unverified.has_value())
      << "without the scrub the corrupt checkpoint must be SERVED — if it is not, this test "
         "is not discriminating on the CRC: "
      << unverified.error().to_string();

  // (iii) Scrub ON (the default): rejected, by name.
  fs::remove(out / "manifest.tsv");
  fs::remove(out / "quality.tsv");
  auto resumed = build_batched(out, dates, 1u, 4u); // scrub defaults ON
  ASSERT_FALSE(resumed.has_value())
      << "a checkpoint whose archive payload is corrupt was served unverified";
  EXPECT_NE(resumed.error().to_string().find("payload CRC scrub"), std::string::npos)
      << "corruption must be reported at the checkpoint, by name: "
      << resumed.error().to_string();
}

// T2b companion: the scrub must not cry wolf. An INTACT archive resumes exactly
// as before with the scrub on (this is the default every qualified run takes),
// and the resumed corpus still matches a clean single-pass build byte for byte.
TEST(CorpusBuildSession, CheckpointScrubAcceptsIntactArchive) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18"};
  const fs::path resumed_dir = fresh_out_dir("t2-scrub-intact");
  const fs::path clean = fresh_out_dir("t2-scrub-clean");

  auto partial = build_batched(resumed_dir, dates, 1u, 4u);
  ASSERT_TRUE(partial.has_value()) << partial.error().to_string();
  fs::remove(resumed_dir / "manifest.tsv");
  fs::remove(resumed_dir / "quality.tsv");
  auto again = build_batched(resumed_dir, dates, 1u, 4u);
  ASSERT_TRUE(again.has_value()) << again.error().to_string();

  auto reference = build_batched(clean, dates, dates.size(), 4u);
  ASSERT_TRUE(reference.has_value()) << reference.error().to_string();
  expect_manifests_equivalent(*again, *reference);
  expect_corpora_byte_identical(resumed_dir, clean, dates);
}

TEST(CorpusBuildSession, RejectsDuplicateCanonicalSymbolsBeforeFitting) {
  const fs::path out = fresh_out_dir("streaming-duplicate");
  QualifiedCorpusConfig cfg;
  auto session = CorpusBuildSession::create(out.string(), cfg);
  ASSERT_TRUE(session.has_value()) << session.error().to_string();

  CorpusBoard board = board_from_spec(make_index_spec("SPY", "2026-06-17", 600.0), "2026-06-17",
                                      "SPY", convex_dense_pin());
  CorpusSourceFailure duplicate;
  duplicate.date = "2026-06-17";
  duplicate.symbol = "spy";
  duplicate.reason = CorpusAdmissionReason::MissingSource;
  const std::array<CorpusCellInput, 2> cells = {CorpusCellInput{board}, CorpusCellInput{duplicate}};
  const Status status = session->append_date("2026-06-17", cells);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::AlreadyExists);
  EXPECT_FALSE(fs::exists(out / "2026-06-17.atxvsa"));
}

TEST(CorpusBuildSession, InterruptedBuildLeavesDateArchiveButNoPartialIndexes) {
  const fs::path out = fresh_out_dir("streaming-interrupted");
  QualifiedCorpusConfig cfg;
  cfg.build.write_opts.created_ts_ns = 1;
  CorpusBoard board = board_from_spec(make_index_spec("SPY", "2026-06-17", 600.0), "2026-06-17",
                                      "SPY", convex_dense_pin());
  const std::array<CorpusCellInput, 1> cells = {CorpusCellInput{board}};
  {
    auto session = CorpusBuildSession::create(out.string(), cfg);
    ASSERT_TRUE(session.has_value()) << session.error().to_string();
    ASSERT_TRUE(session->append_date("2026-06-17", cells).has_value());
  }
  EXPECT_TRUE(fs::exists(out / "2026-06-17.atxvsa"));
  EXPECT_TRUE(fs::exists(out / ".checkpoints" / "2026-06-17.manifest.tsv"));
  EXPECT_TRUE(fs::exists(out / ".checkpoints" / "2026-06-17.quality.tsv"));
  EXPECT_TRUE(fs::exists(out / ".checkpoints" / "2026-06-17.commit"));
  EXPECT_FALSE(fs::exists(out / "manifest.tsv"));
  EXPECT_FALSE(fs::exists(out / "quality.tsv"));
  EXPECT_FALSE(fs::exists(out / "manifest.tsv.pending"));
  EXPECT_FALSE(fs::exists(out / "quality.tsv.pending"));

  auto resumed = CorpusBuildSession::create(out.string(), cfg);
  ASSERT_TRUE(resumed.has_value()) << resumed.error().to_string();
  ASSERT_TRUE(resumed->append_date("2026-06-17", cells).has_value());
  auto finished = resumed->finish();
  ASSERT_TRUE(finished.has_value()) << finished.error().to_string();
  EXPECT_EQ(finished->peak_live_fitted_surfaces, 0u)
      << "a matching checkpoint must not refit the date";
  EXPECT_TRUE(fs::exists(out / "manifest.tsv"));
  EXPECT_TRUE(fs::exists(out / "quality.tsv"));
  EXPECT_TRUE(fs::exists(out / "indexes.commit"));

  auto cached = CorpusBuildSession::create(out.string(), cfg);
  ASSERT_TRUE(cached.has_value());
  ASSERT_TRUE(cached->append_date("2026-06-17", cells).has_value());
  auto cached_result = cached->finish();
  ASSERT_TRUE(cached_result.has_value()) << cached_result.error().to_string();
  EXPECT_EQ(cached_result->manifest, finished->manifest);
  EXPECT_EQ(cached_result->quality, finished->quality);
  EXPECT_EQ(cached_result->peak_live_fitted_surfaces, 0u);

  CorpusBoard changed_board = board;
  changed_board.frame.spot += 1.0;
  const std::array<CorpusCellInput, 1> changed_cells = {CorpusCellInput{changed_board}};
  auto changed_input = CorpusBuildSession::create(out.string(), cfg);
  ASSERT_TRUE(changed_input.has_value());
  const Status changed_input_status = changed_input->append_date("2026-06-17", changed_cells);
  ASSERT_FALSE(changed_input_status.has_value());
  EXPECT_EQ(changed_input_status.error().code(), ErrorCode::AlreadyExists);

  QualifiedCorpusConfig changed_cfg = cfg;
  changed_cfg.admission.enabled = true;
  auto changed_policy = CorpusBuildSession::create(out.string(), changed_cfg);
  ASSERT_TRUE(changed_policy.has_value());
  const Status changed_policy_status = changed_policy->append_date("2026-06-17", cells);
  ASSERT_FALSE(changed_policy_status.has_value());
  EXPECT_EQ(changed_policy_status.error().code(), ErrorCode::AlreadyExists);
}

TEST(CorpusBuildSession, RecoversAbandonedCheckpointAndFinalIndexGenerations) {
  const std::vector<std::string> dates = {"2026-06-17"};
  const fs::path out = fresh_out_dir("streaming-transaction-recovery");
  const fs::path checkpoints = out / ".checkpoints";
  const fs::path checkpoint_manifest = checkpoints / "2026-06-17.manifest.tsv";
  const fs::path checkpoint_quality = checkpoints / "2026-06-17.quality.tsv";
  const fs::path checkpoint_commit = checkpoints / "2026-06-17.commit";

  auto first = build_batched(out, dates, 1u, 2u);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  ASSERT_TRUE(fs::exists(checkpoint_manifest));
  ASSERT_TRUE(fs::exists(checkpoint_quality));
  ASSERT_TRUE(fs::exists(checkpoint_commit));
  ASSERT_TRUE(fs::exists(out / "indexes.commit"));

  const auto remove_final_generation = [&] {
    std::error_code ignored;
    fs::remove(out / "manifest.tsv", ignored);
    fs::remove(out / "quality.tsv", ignored);
    fs::remove(out / "indexes.commit", ignored);
  };

  // Kill point 1: manifest became visible, quality and the authoritative marker
  // did not. Restart discards the uncommitted archive/pair and refits.
  remove_final_generation();
  fs::remove(checkpoint_quality);
  fs::remove(checkpoint_commit);
  auto after_manifest = build_batched(out, dates, 1u, 2u);
  ASSERT_TRUE(after_manifest.has_value()) << after_manifest.error().to_string();
  EXPECT_GT(after_manifest->peak_live_fitted_surfaces, 0u);
  EXPECT_TRUE(fs::exists(checkpoint_manifest));
  EXPECT_TRUE(fs::exists(checkpoint_quality));
  EXPECT_TRUE(fs::exists(checkpoint_commit));

  // Kill point 2: both data files became visible but the commit marker did not.
  // They are still abandoned and must not be accepted as a checkpoint.
  remove_final_generation();
  fs::remove(checkpoint_commit);
  auto before_marker = build_batched(out, dates, 1u, 2u);
  ASSERT_TRUE(before_marker.has_value()) << before_marker.error().to_string();
  EXPECT_GT(before_marker->peak_live_fitted_surfaces, 0u);
  EXPECT_TRUE(fs::exists(checkpoint_commit));

  // Final-index pair uses the same protocol. A one-file final generation is
  // rebuilt from the committed date checkpoint without refitting the date.
  fs::remove(out / "quality.tsv");
  fs::remove(out / "indexes.commit");
  auto final_partial = build_batched(out, dates, 1u, 2u);
  ASSERT_TRUE(final_partial.has_value()) << final_partial.error().to_string();
  EXPECT_EQ(final_partial->peak_live_fitted_surfaces, 0u);
  EXPECT_TRUE(fs::exists(out / "manifest.tsv"));
  EXPECT_TRUE(fs::exists(out / "quality.tsv"));
  EXPECT_TRUE(fs::exists(out / "indexes.commit"));
  expect_manifests_equivalent(*first, *final_partial);
}

TEST(CorpusBuildSession, SyntheticThirteenNameThreeDateBreadthScoreboard) {
  const fs::path out = fresh_out_dir("streaming-breadth");
  QualifiedCorpusConfig cfg;
  cfg.admission.enabled = true;
  CorpusAdmissionRule rule;
  rule.min_quotes = 90u;
  rule.min_slices = 3u;
  rule.require_calendar_arb_free = false;
  rule.require_source_provenance = true;
  for (CorpusAdmissionRule &profile_rule : cfg.admission.by_profile) {
    profile_rule = rule;
  }
  cfg.admission.by_profile[static_cast<std::size_t>(ProfileKind::MegaCapEvent)].min_quotes = 20u;
  cfg.build.fit_template.policy.sparse_validation_floor = 20u;
  // This scoreboard exercises the risk-contract surface-admission fallback (the
  // event board's C8 primary is rejected and republished as eSSVI). Pin the
  // strict risk contract explicitly; the default fit template now serves marks,
  // which would admit the C8 primary directly and never fall back.
  cfg.build.fit_template.admission = risk_admission_policy();
  cfg.build.write_opts.created_ts_ns = 1;
  auto session = CorpusBuildSession::create(out.string(), cfg);
  ASSERT_TRUE(session.has_value()) << session.error().to_string();

  const auto source_failure = [](const std::string &date, std::string symbol,
                                 CorpusAdmissionReason reason) {
    CorpusSourceFailure failure;
    failure.date = date;
    failure.symbol = std::move(symbol);
    failure.reason = reason;
    failure.error_code = reason == CorpusAdmissionReason::MissingSource ? ErrorCode::NotFound
                                                                        : ErrorCode::ParseError;
    failure.source_schema_version = 2u;
    return CorpusCellInput{std::move(failure)};
  };
  const auto make_empty = [](const std::string &date, std::string symbol) {
    CorpusBoard board;
    board.date = date;
    board.symbol = std::move(symbol);
    board.env = MarketEnv::flat(100.0, 0.04, iso_to_ns(date), {});
    board.source_provenance_complete = true;
    return board;
  };

  const std::vector<std::string> dates = {"2026-06-15", "2026-06-16", "2026-06-17"};
  for (std::size_t date_index = 0u; date_index < dates.size(); ++date_index) {
    const std::string &date = dates[date_index];
    std::vector<CorpusCellInput> cells;
    CorpusBoard spy =
        board_from_spec(make_index_spec("SPY", date, 600.0), date, "SPY", convex_dense_pin());
    spy.source_provenance_complete = true;
    cells.emplace_back(std::move(spy));

    if (date_index == 2u) {
      for (const std::string &symbol : {"XOM", "LOW", "AAPL", "HTB", "THIN", "SPARSE", "MISSING",
                                        "CORRUPT", "AMBIG", "EMPTY", "BADFIT", "ABSENT"}) {
        cells.push_back(source_failure(date, symbol, CorpusAdmissionReason::MissingSource));
      }
      ASSERT_TRUE(session->append_date(date, cells).has_value());
      continue;
    }

    CorpusBoard xom = board_from_spec(make_singlename_spec("XOM", date, 110.0), date, "XOM");
    xom.source_provenance_complete = true;
    cells.emplace_back(std::move(xom));

    CorpusBoard low = board_from_spec(make_singlename_spec("LOW", date, 5.0), date, "LOW");
    low.source_provenance_complete = true;
    cells.emplace_back(std::move(low));

    SynthPanelSpec event_spec;
    event_spec.uid = "AAPL";
    event_spec.snapshot_iso = date;
    event_spec.spot = 200.0;
    event_spec.r = 0.043;
    for (const std::string_view expiry_iso :
         {"2026-07-17", "2026-08-21", "2026-09-18", "2026-10-16", "2026-11-20", "2026-12-18"}) {
      const double T = year_fraction(date, expiry_iso);
      event_spec.expiries.push_back(
          SynthExpiry{std::string(expiry_iso), T, S3Params{0.42, -0.9 * std::sqrt(T), 0.8}});
    }
    event_spec.strikes = {160.0, 180.0, 200.0, 220.0, 240.0};
    event_spec.half_spread_frac = 0.03;
    event_spec.min_half_spread = 0.05;
    CorpusBoard event = board_from_spec(event_spec, date, "AAPL");
    event.fit_context.profile_override = ProfileKind::MegaCapEvent;
    event.fit_context.event_phase = EventPhase::PreAnnouncement;
    event.fit_context.event_distance_days = 2u;
    event.source_provenance_complete = true;
    cells.emplace_back(std::move(event));

    SynthPanelSpec htb_spec = make_singlename_spec("HTB", date, 40.0);
    htb_spec.cash_divs = {{iso_to_ns("2026-07-01"), 0.75}};
    CorpusBoard htb = board_from_spec(htb_spec, date, "HTB");
    htb.fit_context.profile_override = ProfileKind::HtbDividendName;
    htb.fit_context.htb = true;
    htb.source_provenance_complete = true;
    cells.emplace_back(std::move(htb));

    CurveConfig essvi;
    essvi.kind = VolCurveKind::Essvi;
    CorpusBoard thin =
        board_from_spec(make_singlename_spec("THIN", date, 75.0), date, "THIN", essvi);
    thin.source_provenance_complete = true;
    for (std::size_t i = 0u; i < thin.frame.rows.size(); i += 4u) {
      thin.frame.rows[i].bid = 0.0;
    }
    cells.emplace_back(std::move(thin));

    CorpusBoard sparse =
        board_from_spec(make_singlename_spec("SPARSE", date, 25.0), date, "SPARSE", essvi);
    sparse.fit_context.profile_override = ProfileKind::IlliquidSmallCap;
    sparse.source_provenance_complete = true;
    for (std::size_t i = 1u; i < sparse.frame.rows.size(); i += 4u) {
      sparse.frame.rows[i].ask = 0.0;
    }
    cells.emplace_back(std::move(sparse));

    cells.push_back(source_failure(date, "MISSING", CorpusAdmissionReason::MissingSource));
    cells.push_back(source_failure(date, "CORRUPT", CorpusAdmissionReason::InvalidSourceSchema));
    cells.push_back(source_failure(date, "AMBIG", CorpusAdmissionReason::AmbiguousSourceIdentity));
    cells.emplace_back(make_empty(date, "EMPTY"));

    CorpusBoard bad_fit = make_empty(date, "BADFIT");
    bad_fit.frame.uid = "BADFIT";
    bad_fit.frame.snapshot_iso = date;
    bad_fit.frame.spot = 100.0;
    bad_fit.frame.rows.push_back(QuoteRow{});
    cells.emplace_back(std::move(bad_fit));
    cells.push_back(source_failure(date, "ABSENT", CorpusAdmissionReason::MissingSource));
    ASSERT_EQ(cells.size(), 13u);
    ASSERT_TRUE(session->append_date(date, cells).has_value());
  }

  auto built = session->finish();
  ASSERT_TRUE(built.has_value()) << built.error().to_string();
  EXPECT_EQ(built->quality.n_planned, 39u);
  EXPECT_EQ(built->quality.entries.size(), 39u);
  // MERGE: these are main's pins, restored. The merge auto-took the branch's
  // rebaselined counts (9 admitted / 6 quarantined) next to MAIN's config line
  // above (`fit_template.admission = risk_admission_policy()`, which the branch
  // did not have) — two edits to the same test that were never valid together.
  // The branch's numbers were captured with a DEFAULT fit template, i.e. under
  // its v2 risk pipeline and its per-mode carry budgets. The template the merged
  // test actually builds pins main's strict risk CONSUMER on main's
  // single-surface transactional path, which is what these counts describe.
  EXPECT_EQ(built->quality.n_admitted, 11u);
  EXPECT_EQ(built->quality.n_quarantined, 4u);
  EXPECT_EQ(built->quality.n_source_failed, 20u);
  EXPECT_EQ(built->quality.n_fit_failed, 2u);
  EXPECT_EQ(built->quality.n_empty, 2u);
  EXPECT_LE(built->peak_live_fitted_surfaces, 7u);
  EXPECT_TRUE(fs::exists(out / "2026-06-15.atxvsa"));
  EXPECT_TRUE(fs::exists(out / "2026-06-16.atxvsa"));
  EXPECT_TRUE(fs::exists(out / "2026-06-17.atxvsa"));

  const std::size_t last_date_admitted = static_cast<std::size_t>(std::count_if(
      built->quality.entries.begin(), built->quality.entries.end(),
      [](const QualifiedCorpusEntry &entry) {
        return entry.date == "2026-06-17" && entry.disposition == CorpusDisposition::Admitted;
      }));
  EXPECT_EQ(last_date_admitted, 1u)
      << "the final date must exercise the below-minimum-survivor regime";
  const auto fallback_entry =
      std::find_if(built->quality.entries.begin(), built->quality.entries.end(),
                   [](const QualifiedCorpusEntry &entry) {
                     return entry.date == "2026-06-15" && entry.symbol == "AAPL";
                   });
  ASSERT_NE(fallback_entry, built->quality.entries.end());
  // Rebaselined (same I1/C3 budget fix): the sparse AAPL event board now
  // resolves a confident carry, its primary candidate is rejected by
  // independent admission, and a fallback rung is admitted — so the persisted
  // provenance must say Admitted WITH used_fallback (the I6 provenance fix),
  // where the clobbered-budget behavior was FitFailed with no fallback record.
  EXPECT_EQ(fallback_entry->disposition, CorpusDisposition::Admitted);
  EXPECT_TRUE(fallback_entry->quality.used_fallback);
  EXPECT_NE(fallback_entry->quality.final_kind, fallback_entry->quality.primary_kind);

  auto clock = Clock::from_manifest(built->manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  ASSERT_EQ(clock->size(), dates.size());

  DispersionUniverse universe;
  universe.index = DispersionMember{"SPY", 0u, 0.0};
  for (const char *symbol : {"XOM", "LOW", "AAPL", "HTB"}) {
    universe.names.push_back(DispersionMember{symbol, 0u, 0.25});
  }
  DispersionConfig dispersion_cfg;
  dispersion_cfg.missing.policy = MissingNamePolicy::DropRenormalize;
  dispersion_cfg.record_diagnostics = true; // opt into n_names_dropped series (now off by default)

  RunConfig serial_cfg;
  serial_cfg.price.n_threads = 1u;
  // WS-F F1(c): the RunConfig default is now UnpricedLotPolicy::Error. This
  // scoreboard deliberately drives the below-minimum-survivor regime (the final
  // date drops 4 of 5 names) and asserts the EXCLUSION counts below, so it must
  // opt into the lenient policy explicitly.
  serial_cfg.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  DispersionStrategy serial_strategy{universe, dispersion_cfg};
  auto serial = run_backtest(*clock, serial_strategy, serial_cfg);
  ASSERT_TRUE(serial.has_value()) << serial.error().to_string();
  ASSERT_EQ(serial->size(), dates.size());

  const auto dropped_signal =
      std::find_if(serial->signals.begin(), serial->signals.end(),
                   [](const auto &signal) { return signal.first == "n_names_dropped"; });
  ASSERT_NE(dropped_signal, serial->signals.end());
  ASSERT_EQ(dropped_signal->second.size(), dates.size());
  // MERGE: main's pins, restored alongside the admission counts above (the
  // branch's 1.0/1.0 belonged to its own default-template rebaseline).
  EXPECT_EQ(dropped_signal->second[0], 0.0);
  EXPECT_EQ(dropped_signal->second[1], 0.0);
  EXPECT_EQ(dropped_signal->second[2], 4.0);
  EXPECT_GT(serial->n_unpriced_lots[2], 0.0);
  EXPECT_GT(serial->n_unpriced_greeks[2], 0.0);

  auto final_snapshot = MarketSnapshot::load((out / "2026-06-17.atxvsa").string());
  ASSERT_TRUE(final_snapshot.has_value()) << final_snapshot.error().to_string();
  const std::vector<DroppedName> dropped = serial_strategy.dropped_on(*final_snapshot);
  ASSERT_EQ(dropped.size(), universe.names.size());
  for (const DroppedName &name : dropped) {
    EXPECT_EQ(name.reason, DropReason::NotInSnapshot) << name.symbol;
    EXPECT_FALSE(name.detail.empty()) << name.symbol;
  }

  for (std::size_t i = 0u; i < serial->size(); ++i) {
    const double attribution = serial->pnl_delta[i] + serial->pnl_gamma[i] + serial->pnl_vega[i] +
                               serial->pnl_vanna[i] + serial->pnl_volga[i] + serial->pnl_theta[i] +
                               serial->pnl_rho[i] + serial->pnl_charm[i] +
                               serial->pnl_unexplained[i] + serial->pnl_settlement[i] +
                               serial->pnl_shares[i] + serial->financing[i] - serial->cost[i];
    EXPECT_NEAR(attribution, serial->pnl_total[i], 1.0e-6 * (std::fabs(serial->pnl_total[i]) + 1.0))
        << "row " << i;
  }

  RunConfig parallel_cfg = serial_cfg;
  parallel_cfg.price.n_threads = 4u;
  DispersionStrategy parallel_strategy{universe, dispersion_cfg};
  auto parallel = run_backtest(*clock, parallel_strategy, parallel_cfg);
  ASSERT_TRUE(parallel.has_value()) << parallel.error().to_string();

  // Labelled so a failure names WHICH claim broke: the two blocks below assert
  // different things and only one of them may ever be relaxed.
  const auto expect_column_bits_equal = [](const char *what, const std::vector<double> &lhs,
                                           const std::vector<double> &rhs) {
    ASSERT_EQ(lhs.size(), rhs.size()) << what;
    for (std::size_t i = 0u; i < lhs.size(); ++i) {
      EXPECT_TRUE(bits_equal(lhs[i], rhs[i])) << what << " row " << i;
    }
  };

  // (1) THREAD-COUNT DETERMINISM: 1 thread vs 4 threads on the SAME greek route.
  // Laned greeks cannot move this — both runs take the identical route — so this
  // stays BIT-EXACT. It is a headline guarantee of the sprint; never relax it.
  expect_column_bits_equal("serial-vs-parallel pnl_total", serial->pnl_total, parallel->pnl_total);
  expect_column_bits_equal("serial-vs-parallel nav", serial->nav, parallel->nav);
  expect_column_bits_equal("serial-vs-parallel gross_vega", serial->gross_vega,
                           parallel->gross_vega);
  expect_column_bits_equal("serial-vs-parallel n_unpriced_lots", serial->n_unpriced_lots,
                           parallel->n_unpriced_lots);
  expect_column_bits_equal("serial-vs-parallel n_unpriced_greeks", serial->n_unpriced_greeks,
                           parallel->n_unpriced_greeks);

  RunConfig fd_cfg = parallel_cfg;
  fd_cfg.price.analytic_greeks = false;
  DispersionStrategy fd_strategy{universe, dispersion_cfg};
  auto fd = run_backtest(*clock, fd_strategy, fd_cfg);
  ASSERT_TRUE(fd.has_value()) << fd.error().to_string();
  // (2) GREEK-ROUTE PARITY: analytic vs FD. Since WS-P1a the analytic route runs
  // the laned AVX2 greeks kernel while FD stays scalar, so these agree to the
  // documented economic band rather than to the bit (support/isa_golden_tol.hpp).
  // Scaled by the COLUMN's magnitude, not the element's. These columns are
  // portfolio aggregates: at inception the book is empty, so gross_vega row 0 is
  // numerically zero (-1.6e-12 vs +1.1e-12 — pure float noise) and an
  // element-relative test on it is meaningless. The column scale is the honest
  // denominator for an aggregate.
  //
  // KNOWN LIMITATION — this is an AGGREGATE band, NOT an element-wise guarantee.
  // Because every row is measured against the column's max, a numerically SMALL
  // row may absorb up to tol * (column scale) in absolute terms, which can be an
  // arbitrarily large RELATIVE move on that row alone. That is deliberate and is
  // the price of having a meaningful denominator for a book that starts empty:
  // the claim being made here is "the two greek routes produce the same portfolio
  // trajectory to within a band set by the trajectory's own size", not "every
  // individual cell agrees to 1e-9 relative".
  //
  // Consequently: do NOT reuse this column band anywhere the compared values are
  // not aggregates. For per-contract / per-element quantities use the element-wise
  // laned_greeks_close(), whose scale is max(|a|,|b|) of the pair being compared.
  const auto expect_column_route_close = [](const char *what, const std::vector<double> &lhs,
                                            const std::vector<double> &rhs) {
    ASSERT_EQ(lhs.size(), rhs.size()) << what;
    double col = 0.0;
    for (std::size_t i = 0u; i < lhs.size(); ++i) {
      col = std::fmax(col, std::fmax(std::fabs(lhs[i]), std::fabs(rhs[i])));
    }
    const double tol = atx::vol::test::kLanedGreeksRelBand * std::fmax(col, 1.0);
    for (std::size_t i = 0u; i < lhs.size(); ++i) {
      EXPECT_LE(std::fabs(lhs[i] - rhs[i]), tol)
          << what << " row " << i << ": " << std::setprecision(17) << lhs[i] << " vs " << rhs[i]
          << " (column scale " << col << ")";
    }
  };
  expect_column_route_close("analytic-vs-fd pnl_total", parallel->pnl_total, fd->pnl_total);
  expect_column_route_close("analytic-vs-fd nav", parallel->nav, fd->nav);
  expect_column_route_close("analytic-vs-fd gross_delta", parallel->gross_delta, fd->gross_delta);
  expect_column_route_close("analytic-vs-fd gross_gamma", parallel->gross_gamma, fd->gross_gamma);
  expect_column_route_close("analytic-vs-fd gross_vega", parallel->gross_vega, fd->gross_vega);
}

// Throughput relocated to bench/corpus_build_bench.cpp.
