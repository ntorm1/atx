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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
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
#include "atx/vol/market_env.hpp"           // MarketEnv
#include "atx/vol/panel.hpp"                // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/priced_surface.hpp"       // PricedSurface
#include "atx/vol/pricer_fitter.hpp"        // PricerFitter, PricerConfig
#include "atx/vol/session.hpp"              // VolaSession::to_priced_surface
#include "atx/vol/spy_fixture.hpp"          // make_spy_synthetic_spec
#include "atx/vol/strategy.hpp"             // DispersionStrategy
#include "atx/vol/surface_archive.hpp"      // SurfaceArchive
#include "atx/vol/types.hpp"                // Side
#include "atx/vol/vol_curve.hpp"            // CurveConfig, VolCurveKind, to_string

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
      &hooks);

  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::Internal);
  EXPECT_EQ(visits.load(), 0u);
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

  const auto expect_column_bits_equal = [](const std::vector<double> &lhs,
                                           const std::vector<double> &rhs) {
    ASSERT_EQ(lhs.size(), rhs.size());
    for (std::size_t i = 0u; i < lhs.size(); ++i) {
      EXPECT_TRUE(bits_equal(lhs[i], rhs[i])) << "row " << i;
    }
  };
  expect_column_bits_equal(serial->pnl_total, parallel->pnl_total);
  expect_column_bits_equal(serial->nav, parallel->nav);
  expect_column_bits_equal(serial->gross_vega, parallel->gross_vega);
  expect_column_bits_equal(serial->n_unpriced_lots, parallel->n_unpriced_lots);
  expect_column_bits_equal(serial->n_unpriced_greeks, parallel->n_unpriced_greeks);

  RunConfig fd_cfg = parallel_cfg;
  fd_cfg.price.analytic_greeks = false;
  DispersionStrategy fd_strategy{universe, dispersion_cfg};
  auto fd = run_backtest(*clock, fd_strategy, fd_cfg);
  ASSERT_TRUE(fd.has_value()) << fd.error().to_string();
  expect_column_bits_equal(parallel->pnl_total, fd->pnl_total);
  expect_column_bits_equal(parallel->nav, fd->nav);
  expect_column_bits_equal(parallel->gross_delta, fd->gross_delta);
  expect_column_bits_equal(parallel->gross_gamma, fd->gross_gamma);
  expect_column_bits_equal(parallel->gross_vega, fd->gross_vega);
}

// Throughput relocated to bench/corpus_build_bench.cpp.
