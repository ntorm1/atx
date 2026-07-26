// SurfaceDbPopulate suite — proves populate_surface_db fits genuinely
// fittable synthetic boards (the make_board_spec/fit_board pattern from
// dispersion_test.cpp) into a SurfaceDb, honoring per-symbol manifest
// configs (enabled/pin_curve/...), grouping by date, skip-existing resume,
// non-fatal per-board failure recording, and the stats CSV shape.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <utility>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "atx/vol/american.hpp" // AlOpts, al_fast_opts, al_default_opts
#include "atx/vol/corpus.hpp"
#include "atx/vol/counters.hpp" // F1: AlBoundarySolves ledger (fast-AL tier quantification)
#include "atx/vol/data.hpp" // iso_to_ns, year_fraction
#include "atx/vol/market_env.hpp"
#include "atx/vol/opra_batch.hpp" // load_opra_daterange, corpus_board_from_opra (F-c real hive)
#include "atx/vol/panel.hpp" // SynthPanelSpec, make_synthetic_american_panel
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/priced_surface_view.hpp" // PricedSurfaceView (S5 map_surface)
#include "atx/vol/run_report.hpp" // MetaKv
#include "atx/vol/s3.hpp"         // S3Params
#include "atx/vol/session.hpp"    // FitPreset
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_db.hpp"
#include "atx/vol/surface_db_build.hpp" // is_total_fit_failure (FIX-D exit-code shape)
#include "atx/vol/detail/fit_scheduler.hpp" // performance_core_count (C4 wave-2 scaling diagnostic)
#include "atx/vol/surface_db_populate.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp"

namespace atx::vol {
namespace {

std::filesystem::path test_root(std::string_view name) {
  auto p =
      std::filesystem::temp_directory_path() / ("atx_surface_db_populate_" + std::string(name));
  std::filesystem::remove_all(p);
  return p;
}

constexpr const char *kDate0 = "2026-03-02";
constexpr const char *kDate1 = "2026-03-03";

// A genuinely fittable board spec (the make_board_spec pattern from
// dispersion_test.cpp:76-111): four expiries with a mild declining term
// structure and a 13-strike ladder, robustly fittable.
[[nodiscard]] SynthPanelSpec make_board_spec(const std::string &symbol, const std::string &date,
                                             double spot, double sigma0) {
  SynthPanelSpec s;
  s.uid = symbol;
  s.snapshot_iso = date;
  s.spot = spot;
  s.r = 0.03;
  s.borrow = 0.0;

  struct Row {
    const char *iso;
    double sig;
    double skew_k;
    double c2;
  };
  const Row rows[] = {
      {"2026-04-17", sigma0, -0.55, 0.6},
      {"2026-05-15", sigma0 - 0.02, -0.52, 0.7},
      {"2026-06-19", sigma0 - 0.04, -0.50, 0.8},
      {"2026-09-18", sigma0 - 0.06, -0.46, 0.9},
  };
  for (const Row &r : rows) {
    SynthExpiry e;
    e.expiry_iso = r.iso;
    e.T = year_fraction(date, r.iso);
    const double s2 = 2.0 * std::sqrt(e.T) * r.skew_k;
    e.truth = S3Params{r.sig, s2, r.c2};
    s.expiries.push_back(e);
  }
  for (const double m :
       {0.80, 0.83, 0.87, 0.91, 0.95, 0.98, 1.0, 1.02, 1.05, 1.09, 1.13, 1.17, 1.20}) {
    s.strikes.push_back(spot * m);
  }
  s.half_spread_frac = 0.05;
  s.min_half_spread = 0.05;
  return s;
}

[[nodiscard]] CorpusBoard make_board(const std::string &date, const std::string &symbol,
                                     double spot, double sigma0) {
  const SynthPanelSpec spec = make_board_spec(symbol, date, spot, sigma0);
  auto panel = make_synthetic_american_panel(spec);
  EXPECT_TRUE(panel.has_value()) << (panel ? "" : panel.error().to_string());
  CorpusBoard b;
  b.date = date;
  b.symbol = symbol;
  b.frame = panel->frame;
  b.env = MarketEnv::flat(spec.spot, spec.r, iso_to_ns(date), spec.cash_divs);
  return b;
}

// A fittable board whose frame row count scales with a caller-chosen strike
// count (an evenly-spaced [0.80, 1.20] moneyness ladder over the same four
// expiries as make_board_spec). Used by the U2 LPT claim-order tests to build
// boards of genuinely different sizes plus a deliberate size tie (two boards
// with identical numeric parameters -> structurally identical row counts).
[[nodiscard]] CorpusBoard make_board_n_strikes(const std::string &date, const std::string &symbol,
                                               double spot, double sigma0, int n_strikes) {
  SynthPanelSpec s = make_board_spec(symbol, date, spot, sigma0);
  s.strikes.clear();
  for (int i = 0; i < n_strikes; ++i) {
    const double m =
        0.80 + (1.20 - 0.80) * static_cast<double>(i) / static_cast<double>(n_strikes - 1);
    s.strikes.push_back(spot * m);
  }
  auto panel = make_synthetic_american_panel(s);
  EXPECT_TRUE(panel.has_value()) << (panel ? "" : panel.error().to_string());
  CorpusBoard b;
  b.date = date;
  b.symbol = symbol;
  b.frame = panel->frame;
  b.env = MarketEnv::flat(s.spot, s.r, iso_to_ns(date), s.cash_divs);
  return b;
}

// 2 symbols ("AAA","BBB") x 2 dates ("2026-03-02","2026-03-03").
[[nodiscard]] std::vector<CorpusBoard> make_boards() {
  std::vector<CorpusBoard> boards;
  boards.push_back(make_board(kDate0, "AAA", 100.0, 0.28));
  boards.push_back(make_board(kDate0, "BBB", 60.0, 0.34));
  boards.push_back(make_board(kDate1, "AAA", 101.0, 0.27));
  boards.push_back(make_board(kDate1, "BBB", 61.0, 0.33));
  return boards;
}

[[nodiscard]] std::string read_file(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void expect_surface_bits_equal(const PricedSurface &actual, const PricedSurface &expected) {
  const auto expect_double_bits = [](double lhs, double rhs) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(lhs), std::bit_cast<std::uint64_t>(rhs));
  };
  EXPECT_EQ(actual.uid(), expected.uid());
  EXPECT_EQ(actual.n_slices(), expected.n_slices());
  expect_double_bits(actual.pricing().S, expected.pricing().S);
  expect_double_bits(actual.pricing().r, expected.pricing().r);
  ASSERT_EQ(actual.context().size(), expected.context().size());
  for (std::size_t i = 0u; i < actual.context().size(); ++i) {
    const SliceContext &a = actual.context()[i];
    const SliceContext &b = expected.context()[i];
    expect_double_bits(a.T, b.T);
    expect_double_bits(a.forward, b.forward);
    expect_double_bits(a.borrow, b.borrow);
    expect_double_bits(a.q_eff, b.q_eff);
    EXPECT_EQ(a.n_used, b.n_used);
    EXPECT_EQ(a.n_dropped, b.n_dropped);
    for (const double moneyness : {0.85, 1.0, 1.15}) {
      const double K = actual.pricing().S * moneyness;
      expect_double_bits(actual.iv(K, a.T), expected.iv(K, b.T));
      for (const Side side : {Side::Call, Side::Put}) {
        const auto actual_price = actual.fair_value(K, a.T, side);
        const auto expected_price = expected.fair_value(K, b.T, side);
        ASSERT_TRUE(actual_price.has_value());
        ASSERT_TRUE(expected_price.has_value());
        expect_double_bits(*actual_price, *expected_price);
      }
    }
  }
}

// F-a end-to-end proof (WS-F), populate write-site: the zero-copy S5 view
// (SurfaceDb::map_surface -> LoadedSurface/PricedSurfaceView — the reconstruct-free
// deserialize the backtest reaches through SurfaceDb) must price the fit-populated
// partition bit-for-bit with the owned reconstruct path (load_surface). populate
// writes v2 via write_surface_archive_v2_file (surface_db.cpp); this proves that
// serialized fit output is read identically through the production zero-copy view.
void expect_view_matches_reconstruct(const PricedSurfaceView &view,
                                     const PricedSurface &owned) {
  const auto eq = [](double lhs, double rhs) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(lhs), std::bit_cast<std::uint64_t>(rhs));
  };
  ASSERT_EQ(view.n_slices(), owned.n_slices());
  for (const SliceContext &c : owned.context()) {
    const double T = c.T;
    eq(view.forward_at(T), owned.forward_at(T));
    for (const double moneyness : {0.85, 1.0, 1.15}) {
      const double K = owned.pricing().S * moneyness;
      eq(view.iv(K, T), owned.iv(K, T));
      for (const Side side : {Side::Call, Side::Put}) {
        const auto pv = view.fair_value(K, T, side);
        const auto po = owned.fair_value(K, T, side);
        ASSERT_EQ(pv.has_value(), po.has_value());
        if (pv.has_value()) {
          eq(*pv, *po);
        }
        const auto gv = view.greeks(K, T, side);
        const auto go = owned.greeks(K, T, side);
        ASSERT_EQ(gv.has_value(), go.has_value());
        if (gv.has_value()) {
          eq(gv->delta, go->delta);
          eq(gv->gamma, go->gamma);
          eq(gv->vega, go->vega);
          eq(gv->theta, go->theta);
        }
      }
    }
  }
}

} // namespace

TEST(SurfaceDbPopulate, MapSurfaceViewReproducesReconstructedFitOutput) {
  const auto root = test_root("v2view");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  const std::vector<CorpusBoard> boards = make_boards();
  auto result = populate_surface_db(*db, boards, SurfaceDbPopulateConfig{});
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
  ASSERT_EQ(result->n_ok, 4u);

  std::size_t n_checked = 0;
  for (const char *date : {kDate0, kDate1}) {
    for (const char *sym : {"AAA", "BBB"}) {
      auto owned = db->load_surface(date, sym);
      ASSERT_TRUE(owned.has_value()) << (owned ? "" : owned.error().to_string());
      auto loaded = db->map_surface(date, sym);
      ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().to_string());
      expect_view_matches_reconstruct(**loaded, *owned);
      ++n_checked;
    }
  }
  EXPECT_EQ(n_checked, 4u);
}

// ── F-c: universe populate driver over the REAL OPRA hive ───────────────────
// Drives populate_universe_streaming (the F-c driver core) over the WS-D parquet
// hive. GTEST_SKIPs cleanly when the hive (or the requested cells) is absent, so it
// is safe in every environment and develops against whatever the pull has written.
namespace fc {

constexpr const char *kHiveRoot = "C:/atx-data/spy-dispersion/opra";

// Load the available boards for `symbols` over [lo,hi] from the read-only hive.
// Missing/partial cells are silently dropped (load_opra_daterange non-fatal), so
// the returned vector is exactly the cells on disk right now.
[[nodiscard]] std::vector<CorpusBoard> load_hive_boards(const std::vector<std::string> &symbols,
                                                        const std::string &lo,
                                                        const std::string &hi) {
  OpraBatchSpec spec;
  spec.symbols = symbols;
  spec.date_lo = lo;
  spec.date_hi = hi;
  spec.root_dir = kHiveRoot;
  spec.r = 0.043;
  std::vector<CorpusBoard> boards;
  const Result<OpraBatchResult> batch = load_opra_daterange(spec);
  if (!batch.has_value()) {
    return boards; // malformed spec -> treat as no data (test SKIPs)
  }
  for (const OpraBatchEntry &e : batch->entries) {
    if (e.panel.has_value()) {
      boards.push_back(corpus_board_from_opra(e.date, e.symbol, *e.panel));
    }
  }
  return boards;
}

[[nodiscard]] std::size_t count_distinct_dates(const std::vector<CorpusBoard> &boards) {
  std::vector<std::string> d;
  for (const CorpusBoard &b : boards) {
    if (std::find(d.begin(), d.end(), b.date) == d.end()) {
      d.push_back(b.date);
    }
  }
  return d.size();
}

} // namespace fc

// Deterministic core gate (no hive, synthetic boards that reliably fit): the
// cell-aware resume mechanism — run 1 fits every cell, run 2 fits ZERO and finds
// all present. Runs everywhere (no GTEST_SKIP).
TEST(SurfaceDbPopulate, UniverseStreamingIdempotentSynthetic) {
  const auto root = test_root("universe_synth");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  const std::vector<CorpusBoard> boards = make_boards(); // AAA,BBB x kDate0,kDate1 = 4
  UniversePopulateSpec spec;
  spec.index_symbol = ""; // no index pin — let the auto-selector fit both
  spec.preset = FitPreset::Fast;
  spec.fit_workers = 0;

  auto cov1 = populate_universe_streaming(*db, boards, spec);
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  EXPECT_EQ(cov1->cells_loaded, 4u);
  EXPECT_EQ(cov1->cells_to_fit, 4u);
  EXPECT_EQ(cov1->cells_ok, 4u) << "synthetic boards must all fit";
  EXPECT_EQ(cov1->cells_failed, 0u);
  EXPECT_EQ(cov1->dates_written, 2u);

  auto cov2 = populate_universe_streaming(*db, boards, spec);
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());
  EXPECT_EQ(cov2->cells_to_fit, 0u) << "idempotent: nothing new to fit";
  EXPECT_EQ(cov2->cells_already_present, 4u);
  EXPECT_EQ(cov2->dates_written, 0u);
  EXPECT_EQ(cov2->dates_skipped_complete, 2u);

  // Round-trip every produced surface through the zero-copy view.
  std::size_t n_checked = 0;
  for (const CorpusBoard &b : boards) {
    auto owned = db->load_surface(b.date, b.symbol);
    ASSERT_TRUE(owned.has_value()) << (owned ? "" : owned.error().to_string());
    auto loaded = db->map_surface(b.date, b.symbol);
    ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().to_string());
    expect_view_matches_reconstruct(**loaded, *owned);
    ++n_checked;
  }
  EXPECT_EQ(n_checked, 4u);
}

// Main F-c gate: fit the universe into a SurfaceDb, prove idempotent resume (second
// run fits ZERO), coverage accounting, and that every produced surface round-trips
// through the zero-copy map_surface view bit-exactly (reuses the F-a machinery).
TEST(SurfaceDbPopulate, UniverseStreamingResumeOverRealHive) {
  // Single trading day + reliably-fitting, cleanly-loading large caps (no SPY: the
  // dense-pinned index leg legitimately fails admission on some real snapshots; no
  // XOM: it also fails on this day — both are logged skips, not test flakes). This
  // keeps the idempotency accounting deterministic on live data. index_symbol="" so
  // every symbol uses the auto-selector (which fits these penny-dense names).
  const std::vector<std::string> symbols{"AAPL", "GOOGL", "NVDA"};
  const std::vector<CorpusBoard> boards = fc::load_hive_boards(symbols, "2026-01-02", "2026-01-02");
  if (boards.empty()) {
    GTEST_SKIP() << "OPRA hive not found under " << fc::kHiveRoot;
  }
  const std::size_t n = boards.size();

  const auto root = test_root("universe_resume");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  UniversePopulateSpec spec;
  spec.index_symbol = ""; // no index pin
  spec.preset = FitPreset::Fast;
  spec.fit_workers = 0;

  // ── Run 1: cold populate. Every loaded cell is NEW work. ──
  auto cov1 = populate_universe_streaming(*db, boards, spec);
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  EXPECT_EQ(cov1->cells_loaded, n);
  EXPECT_EQ(cov1->cells_to_fit, n);
  EXPECT_EQ(cov1->cells_already_present, 0u);
  EXPECT_EQ(cov1->cells_ok + cov1->cells_failed, n);
  EXPECT_GT(cov1->cells_ok, 0u) << "no real board fit — check the hive/preset";
  std::printf("[F-c universe] run1 loaded=%u to_fit=%u ok=%u failed=%u dates_written=%u\n",
              cov1->cells_loaded, cov1->cells_to_fit, cov1->cells_ok, cov1->cells_failed,
              cov1->dates_written);

  // ── Run 2: idempotent resume. Every SUCCESSFUL cell is found present; only the
  //    cells that FAILED to fit are re-attempted (a failed cell is never in the db,
  //    so a resume legitimately retries it). This is the exact idempotency invariant. ──
  auto cov2 = populate_universe_streaming(*db, boards, spec);
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());
  EXPECT_EQ(cov2->cells_already_present, cov1->cells_ok) << "successes must resume as present";
  EXPECT_EQ(cov2->cells_to_fit, cov1->cells_failed) << "only failures are retried";
  if (cov1->cells_failed == 0u) {
    EXPECT_EQ(cov2->dates_written, 0u); // all cells fit -> nothing to rewrite -> true no-op
  }

  // ── Every produced surface round-trips through the zero-copy view bit-exactly. ──
  std::size_t n_checked = 0;
  for (const CorpusBoard &b : boards) {
    auto owned = db->load_surface(b.date, b.symbol);
    if (!owned.has_value()) {
      continue; // a cell that failed to fit is absent — accounted by cells_failed
    }
    auto loaded = db->map_surface(b.date, b.symbol);
    ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().to_string());
    expect_view_matches_reconstruct(**loaded, *owned);
    ++n_checked;
  }
  EXPECT_EQ(n_checked, cov1->cells_ok);
  EXPECT_GT(n_checked, 0u);
}

// Cell-aware incremental resume: populate a SUBSET of a date, then re-run with the
// full set — only the newly-arrived symbol is NEW work; the already-present cells
// are re-fit by the whole-date rewrite (never dropped), and the new one lands.
TEST(SurfaceDbPopulate, UniverseStreamingCellAwareIncrementalResume) {
  // GOOGL is the newly-arrived cell (all three fit + load cleanly on this day; no
  // SPY, whose dense-pinned fit fails admission).
  const std::vector<CorpusBoard> sub = fc::load_hive_boards({"AAPL", "NVDA"}, "2026-01-02", "2026-01-02");
  const std::vector<CorpusBoard> full =
      fc::load_hive_boards({"AAPL", "NVDA", "GOOGL"}, "2026-01-02", "2026-01-02");
  if (sub.size() != 2u || full.size() != 3u) {
    GTEST_SKIP() << "OPRA hive (AAPL/NVDA/GOOGL 2026-01-02) not fully available under "
                 << fc::kHiveRoot;
  }

  const auto root = test_root("universe_incremental");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  UniversePopulateSpec spec;
  spec.index_symbol = ""; // no index pin
  spec.preset = FitPreset::Fast;
  spec.fit_workers = 0;

  auto cov_a = populate_universe_streaming(*db, sub, spec);
  ASSERT_TRUE(cov_a.has_value()) << (cov_a ? "" : cov_a.error().to_string());
  EXPECT_EQ(cov_a->cells_to_fit, 2u);
  EXPECT_EQ(cov_a->dates_written, 1u);
  ASSERT_EQ(cov_a->cells_failed, 0u) << "AAPL/NVDA must fit on this day";

  // GOOGL is the one full-set symbol not in the subset (the newly-arrived cell).
  auto cov_b = populate_universe_streaming(*db, full, spec);
  ASSERT_TRUE(cov_b.has_value()) << (cov_b ? "" : cov_b.error().to_string());
  EXPECT_EQ(cov_b->cells_to_fit, 1u);        // only GOOGL is new work
  // FIX-D: AAPL/NVDA are CARRIED (their stored surfaces re-emitted verbatim),
  // not dragged back through the fitter. This assertion previously read
  // `cells_refit == 2` and was the amplification itself: one new symbol forced a
  // re-fit of every healthy sibling on the date, at 49x the useful work on the
  // production universe.
  EXPECT_EQ(cov_b->cells_refit, 0u);
  EXPECT_EQ(cov_b->cells_carried, 2u);
  EXPECT_EQ(cov_b->dates_written, 1u);       // the one date is rewritten
  EXPECT_EQ(cov_b->dates_skipped_complete, 0u);

  // The newly-arrived symbol (GOOGL) is now present and round-trips through the view.
  auto owned = db->load_surface("2026-01-02", "GOOGL");
  ASSERT_TRUE(owned.has_value()) << (owned ? "" : owned.error().to_string());
  auto loaded = db->map_surface("2026-01-02", "GOOGL");
  ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().to_string());
  expect_view_matches_reconstruct(**loaded, *owned);
}

TEST(SurfaceDbPopulate, FitsAndStoresPartitionsPerDate) {
  const auto root = test_root("basic");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  const std::vector<CorpusBoard> boards = make_boards();
  const SurfaceDbPopulateConfig cfg;
  auto result = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

  EXPECT_EQ(result->n_boards, 4u);
  EXPECT_EQ(result->n_ok, 4u);
  EXPECT_EQ(result->n_failed, 0u);
  EXPECT_EQ(result->n_dates_written, 2u);
  EXPECT_EQ(result->n_dates_skipped_existing, 0u);
  EXPECT_EQ(db->partitions().size(), 2u);

  auto s = db->load_surface(kDate0, "AAA");
  EXPECT_TRUE(s.has_value()) << (s ? "" : s.error().to_string());
  auto s2 = db->load_surface(kDate1, "BBB");
  EXPECT_TRUE(s2.has_value()) << (s2 ? "" : s2.error().to_string());

  ASSERT_EQ(result->per_symbol.size(), 2u);
  EXPECT_EQ(result->per_symbol[0].symbol, "AAA");
  EXPECT_EQ(result->per_symbol[0].n_attempted, 2u);
  EXPECT_EQ(result->per_symbol[0].n_ok, 2u);
  EXPECT_EQ(result->per_symbol[0].n_failed, 0u);
  EXPECT_EQ(result->per_symbol[0].n_disabled, 0u);
  EXPECT_EQ(result->per_symbol[1].symbol, "BBB");
  EXPECT_EQ(result->per_symbol[1].n_ok, 2u);

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPopulate, GlobalParallelQueuePreservesDeterministicPartitions) {
  const auto serial_root = test_root("global_queue_serial");
  const auto parallel_root = test_root("global_queue_parallel");
  auto serial_db = SurfaceDb::create(serial_root.string());
  auto parallel_db = SurfaceDb::create(parallel_root.string());
  ASSERT_TRUE(serial_db.has_value());
  ASSERT_TRUE(parallel_db.has_value());

  const std::vector<CorpusBoard> boards = make_boards();
  SurfaceDbPopulateConfig serial_cfg;
  serial_cfg.n_threads = 1u;
  auto serial = populate_surface_db(*serial_db, boards, serial_cfg);
  ASSERT_TRUE(serial.has_value()) << (serial ? "" : serial.error().to_string());

  SurfaceDbPopulateConfig parallel_cfg;
  parallel_cfg.n_threads = 4u;
  auto parallel = populate_surface_db(*parallel_db, boards, parallel_cfg);
  ASSERT_TRUE(parallel.has_value()) << (parallel ? "" : parallel.error().to_string());

  EXPECT_EQ(parallel->n_ok, serial->n_ok);
  EXPECT_EQ(parallel->n_failed, serial->n_failed);
  EXPECT_EQ(parallel->n_dates_written, serial->n_dates_written);
  EXPECT_EQ(parallel->per_symbol.size(), serial->per_symbol.size());
  for (const char *date : {kDate0, kDate1}) {
    for (const char *symbol : {"AAA", "BBB"}) {
      const auto serial_surface = serial_db->load_surface(date, symbol);
      const auto parallel_surface = parallel_db->load_surface(date, symbol);
      ASSERT_TRUE(serial_surface.has_value());
      ASSERT_TRUE(parallel_surface.has_value());
      // Archive creation timestamps and provenance generations intentionally
      // differ between independent writes; the served numerical state must not.
      expect_surface_bits_equal(*parallel_surface, *serial_surface);
    }
  }

  std::filesystem::remove_all(serial_root);
  std::filesystem::remove_all(parallel_root);
}

// C4 wave-2: pinning the outer workers to P-cores (+ capping the budget at the
// P-core count) is a pure scheduling steer — it changes only WHICH logical CPU a
// board fits on, never which board or how it is fit. The served surfaces must be
// byte-identical to the unpinned/uncapped populate. This is the C4 wave-2 hard
// gate (the speedup itself is provisional / quiet-window; byte-identity is not).
TEST(SurfaceDbPopulate, PinnedOuterWorkersByteIdenticalToUnpinned) {
  const auto pinned_root = test_root("pin_on");
  const auto unpinned_root = test_root("pin_off");
  auto pinned_db = SurfaceDb::create(pinned_root.string());
  auto unpinned_db = SurfaceDb::create(unpinned_root.string());
  ASSERT_TRUE(pinned_db.has_value());
  ASSERT_TRUE(unpinned_db.has_value());

  const std::vector<CorpusBoard> boards = make_boards();
  SurfaceDbPopulateConfig pinned_cfg;
  pinned_cfg.n_threads = 8u;
  pinned_cfg.pin_outer_workers = true; // cap at P-cores + pin (the C4 wave-2 default)
  SurfaceDbPopulateConfig unpinned_cfg;
  unpinned_cfg.n_threads = 8u;
  unpinned_cfg.pin_outer_workers = false; // historical: no cap, no pin

  auto pinned = populate_surface_db(*pinned_db, boards, pinned_cfg);
  auto unpinned = populate_surface_db(*unpinned_db, boards, unpinned_cfg);
  ASSERT_TRUE(pinned.has_value()) << (pinned ? "" : pinned.error().to_string());
  ASSERT_TRUE(unpinned.has_value()) << (unpinned ? "" : unpinned.error().to_string());

  EXPECT_EQ(pinned->n_ok, unpinned->n_ok);
  for (const char *date : {kDate0, kDate1}) {
    for (const char *symbol : {"AAA", "BBB"}) {
      const auto ps = pinned_db->load_surface(date, symbol);
      const auto us = unpinned_db->load_surface(date, symbol);
      ASSERT_TRUE(ps.has_value());
      ASSERT_TRUE(us.has_value());
      expect_surface_bits_equal(*ps, *us);
    }
  }

  std::filesystem::remove_all(pinned_root);
  std::filesystem::remove_all(unpinned_root);
}

// C4 wave-2 scaling-curve DIAGNOSTIC (DISABLED — run with
// --gtest_also_run_disabled_tests; PROVISIONAL, shared host). Populates a modest
// multi-symbol multi-date universe at workers 1/2/4/8 with pinning ON, plus 8
// unpinned, printing the median wall so the P-core cap/pin scaling knee is
// observable. The RATIFIED curve is quiet-window domain (G4 / V3); the hard gate
// is byte-identity above, not these numbers.
TEST(SurfaceDbPopulate, DISABLED_ScalingCurveDiagnostic) {
  const char *dates[] = {"2026-03-02", "2026-03-03", "2026-03-04"};
  std::vector<CorpusBoard> boards;
  for (const char *d : dates) {
    for (int s = 0; s < 16; ++s) {
      const std::string sym = "S" + std::to_string(s);
      boards.push_back(make_board(d, sym, 80.0 + 4.0 * s, 0.24 + 0.01 * (s % 5)));
    }
  }
  const auto run = [&](unsigned threads, bool pin) -> double {
    double best = std::numeric_limits<double>::infinity();
    for (int rep = 0; rep < 3; ++rep) {
      const auto root = test_root("scale_t" + std::to_string(threads) + (pin ? "_pin" : "_nopin") +
                                  "_r" + std::to_string(rep));
      auto db = SurfaceDb::create(root.string());
      SurfaceDbPopulateConfig cfg;
      cfg.n_threads = threads;
      cfg.pin_outer_workers = pin;
      const auto t0 = std::chrono::steady_clock::now();
      auto res = populate_surface_db(*db, boards, cfg);
      const auto t1 = std::chrono::steady_clock::now();
      EXPECT_TRUE(res.has_value());
      best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
      std::filesystem::remove_all(root);
    }
    return best;
  };
  const double serial = run(1u, true);
  for (const unsigned t : {1u, 2u, 4u, 8u}) {
    const double ms = run(t, true);
    std::printf("C4SCALE\tthreads=%u\tpin=1\tms=%.1f\tspeedup=%.2fx\n", t, ms, serial / ms);
  }
  const double t8_nopin = run(8u, false);
  std::printf("C4SCALE\tthreads=8\tpin=0\tms=%.1f\tspeedup=%.2fx\n", t8_nopin, serial / t8_nopin);
  std::printf("C4SCALE\tp_cores=%u\n", atx::vol::detail::performance_core_count());
}

// U2 (R-13) [pure-refactor]: the outer fit queue claims boards in
// Longest-Processing-Time order -- largest frame first -- so the heavy tail
// starts early rather than stranding cores at the end of the run. With a single
// worker the bounded queue runs tasks in claim order, so before_board_fit
// observes exactly the claim order. Sizes are chosen so the largest board (DDD)
// is NOT the date/symbol-first board (a naive date/symbol claim order would
// fail this test), and BBB/CCC are a deliberate size tie whose break must be
// the deterministic original (date,symbol) order.
TEST(SurfaceDbPopulate, LptClaimsLargestBoardsFirstDeterministically) {
  const auto root = test_root("lpt_order");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  std::vector<CorpusBoard> boards;
  boards.push_back(make_board_n_strikes(kDate0, "AAA", 100.0, 0.28, 11)); // smallest
  boards.push_back(make_board_n_strikes(kDate0, "BBB", 60.0, 0.34, 13));  // tie group
  boards.push_back(make_board_n_strikes(kDate0, "CCC", 60.0, 0.34, 13));  // tie group
  boards.push_back(make_board_n_strikes(kDate0, "DDD", 90.0, 0.29, 15));  // largest

  // The sizes must be genuinely distinct (else the ordering claim is vacuous)
  // and the tie group must really tie.
  const std::size_t rows_aaa = boards[0].frame.rows.size();
  const std::size_t rows_bbb = boards[1].frame.rows.size();
  const std::size_t rows_ccc = boards[2].frame.rows.size();
  const std::size_t rows_ddd = boards[3].frame.rows.size();
  ASSERT_EQ(rows_bbb, rows_ccc) << "BBB/CCC must be a genuine size tie";
  ASSERT_GT(rows_ddd, rows_bbb) << "DDD must be strictly largest";
  ASSERT_GT(rows_bbb, rows_aaa) << "AAA must be strictly smallest";

  std::vector<std::string> claim_order;
  std::mutex claim_mu;
  PopulateTestHooks hooks;
  hooks.before_board_fit = [&](const std::string & /*date*/, const std::string &symbol) {
    std::lock_guard<std::mutex> lk(claim_mu);
    claim_order.push_back(symbol);
  };

  SurfaceDbPopulateConfig cfg;
  cfg.n_threads = 1u; // single worker => the bounded queue runs tasks in claim order
  auto result = populate_surface_db(*db, boards, cfg, &hooks);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
  ASSERT_EQ(result->n_ok, 4u);

  // Largest-first; the BBB/CCC size tie breaks to ascending (date,symbol) order.
  const std::vector<std::string> expected{"DDD", "BBB", "CCC", "AAA"};
  EXPECT_EQ(claim_order, expected);

  std::filesystem::remove_all(root);
}

// U2 (R-13) [pure-refactor]: LPT reorders *scheduling* only, never output. A
// full multi-board populate (whose LPT claim order DDD,BBB,CCC,AAA is a
// non-trivial reorder of the date/symbol order) must write, for every symbol, a
// surface bit-identical to that symbol fit alone (a single-board populate has a
// trivial one-task claim order, so its output is the pre-LPT reference). Equal
// bits across the two schedules is the operational meaning of "byte-identical to
// the pre-LPT launch order": each board's fit is independent and deterministic,
// so claim order cannot move a single bit of any surface.
TEST(SurfaceDbPopulate, LptReorderingKeepsOutputByteIdentical) {
  struct Spec {
    const char *symbol;
    double spot;
    double sigma0;
    int n_strikes;
  };
  const Spec specs[] = {
      {"AAA", 100.0, 0.28, 11},
      {"BBB", 60.0, 0.34, 13},
      {"CCC", 60.0, 0.34, 13},
      {"DDD", 90.0, 0.29, 15},
  };

  const auto full_root = test_root("lpt_identity_full");
  auto full_db = SurfaceDb::create(full_root.string());
  ASSERT_TRUE(full_db.has_value());

  std::vector<CorpusBoard> boards;
  for (const Spec &sp : specs) {
    boards.push_back(make_board_n_strikes(kDate0, sp.symbol, sp.spot, sp.sigma0, sp.n_strikes));
  }

  SurfaceDbPopulateConfig full_cfg;
  full_cfg.n_threads = 1u; // serial => execution follows the LPT claim order
  auto full = populate_surface_db(*full_db, boards, full_cfg);
  ASSERT_TRUE(full.has_value()) << (full ? "" : full.error().to_string());
  ASSERT_EQ(full->n_ok, 4u);

  // Per-symbol reference: fit each board ALONE (one task => no reorder), then
  // require the multi-board LPT-scheduled surface to match it bit-for-bit.
  for (const Spec &sp : specs) {
    const auto ref_root = test_root(std::string("lpt_identity_ref_") + sp.symbol);
    auto ref_db = SurfaceDb::create(ref_root.string());
    ASSERT_TRUE(ref_db.has_value());
    const std::vector<CorpusBoard> one = {
        make_board_n_strikes(kDate0, sp.symbol, sp.spot, sp.sigma0, sp.n_strikes)};
    SurfaceDbPopulateConfig ref_cfg;
    ref_cfg.n_threads = 1u;
    auto ref = populate_surface_db(*ref_db, one, ref_cfg);
    ASSERT_TRUE(ref.has_value()) << (ref ? "" : ref.error().to_string());
    ASSERT_EQ(ref->n_ok, 1u);

    const auto full_surface = full_db->load_surface(kDate0, sp.symbol);
    const auto ref_surface = ref_db->load_surface(kDate0, sp.symbol);
    ASSERT_TRUE(full_surface.has_value()) << sp.symbol;
    ASSERT_TRUE(ref_surface.has_value()) << sp.symbol;
    expect_surface_bits_equal(*full_surface, *ref_surface);

    std::filesystem::remove_all(ref_root);
  }

  std::filesystem::remove_all(full_root);
}

// Streaming / per-date-release guard (R-03 / U1). Proves populate writes and
// releases each date's partition as that date's fits complete -- streamed
// across the still-running shared queue -- rather than deferring every write to
// a single global join (peak RSS O(all dates), the 519-name OOM). Mechanism: a
// later-date (kDate1) board blocks in its fit until the earlier date's (kDate0)
// partition is on disk. A streaming populate lets the drain write kDate0 while
// kDate1's boards are still pending, so the block clears and populate finishes;
// a "join every fit, then write" populate cannot write kDate0 until this very
// board finishes, so the block would spin to its deadline and leave the witness
// flag false. A finite deadline turns that would-be deadlock into a clean
// assertion failure instead of a hang.
TEST(SurfaceDbPopulate, StreamsPartitionsBeforeGlobalJoin) {
  const auto root = test_root("streaming");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  const std::vector<CorpusBoard> boards = make_boards(); // 2 dates x 2 symbols

  std::atomic<bool> date0_written{false};
  std::atomic<bool> date0_written_before_date1_finished{false};

  PopulateTestHooks hooks;
  hooks.after_partition_write = [&](const std::string &date) {
    if (date == kDate0) {
      date0_written.store(true, std::memory_order_release);
    }
  };
  hooks.before_board_fit = [&](const std::string &date, const std::string & /*symbol*/) {
    if (date != kDate1) {
      return; // only the later date's boards gate on the earlier partition
    }
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(5);
    while (clock::now() < deadline) {
      if (date0_written.load(std::memory_order_acquire)) {
        date0_written_before_date1_finished.store(true, std::memory_order_release);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };

  SurfaceDbPopulateConfig cfg;
  cfg.n_threads = 4u; // >= n_boards so kDate1 boards start alongside kDate0's
  auto result = populate_surface_db(*db, boards, cfg, &hooks);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

  EXPECT_EQ(result->n_dates_written, 2u);
  EXPECT_EQ(result->n_ok, 4u);
  EXPECT_TRUE(date0_written_before_date1_finished.load())
      << "kDate0 partition was not written until kDate1 fits finished -- populate "
         "deferred all writes to a single global join (peak RSS O(all dates))";

  std::filesystem::remove_all(root);
}

// U3 (R-12) [correctness]: a fit-worker exception mid-run must NOT discard the
// dates that already completed and were written. The midpoint review flagged
// that a single throwing board (bad_alloc in fit_board / the slot move) made
// run_bounded_fit_tasks return Internal and populate return having written zero
// partitions -- hours of finished fits gone. The streaming per-date writer
// restores date-granular durability: kDate0 is committed to disk (archive +
// generation-bumped manifest) before the global join, so a later kDate1 throw
// cannot roll it back. The throw is injected via before_board_fit -- a faithful,
// deterministic proxy for a worker exception (both land in run_next's catch(...)
// -> FailureKind::Exception -> Internal). The kDate1 boards first wait until
// kDate0 is durably written, so the exception is genuinely "mid-run, after some
// dates completed". Durability is asserted from a FRESH SurfaceDb::open of the
// same root (crash-resume: a new process reads only what was committed to disk,
// never this run's in-memory state).
TEST(SurfaceDbPopulate, CompletedDatesSurviveLaterWorkerThrow) {
  const auto root = test_root("durability_worker_throw");
  {
    auto db = SurfaceDb::create(root.string());
    ASSERT_TRUE(db.has_value());

    const std::vector<CorpusBoard> boards = make_boards(); // 2 dates x 2 symbols

    std::atomic<bool> date0_written{false};
    PopulateTestHooks hooks;
    hooks.after_partition_write = [&](const std::string &date) {
      if (date == kDate0) {
        date0_written.store(true, std::memory_order_release);
      }
    };
    hooks.before_board_fit = [&](const std::string &date, const std::string & /*symbol*/) {
      if (date != kDate1) {
        return; // only the later date throws; the earlier date completes cleanly
      }
      // Wait until kDate0 is durably on disk, THEN throw -- guaranteeing the
      // worker exception fires strictly after an earlier date completed.
      using clock = std::chrono::steady_clock;
      const auto deadline = clock::now() + std::chrono::seconds(5);
      while (clock::now() < deadline && !date0_written.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      throw std::runtime_error("injected mid-run worker exception (kDate1)");
    };

    SurfaceDbPopulateConfig cfg;
    cfg.n_threads = 4u; // >= n_boards so kDate1 boards run alongside kDate0's
    auto result = populate_surface_db(*db, boards, cfg, &hooks);

    // The worker exception surfaces as a top-level Internal error ...
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Internal);
    // ... and kDate0 really was written before the throw was allowed to fire.
    EXPECT_TRUE(date0_written.load())
        << "kDate0 never completed before the injected throw -- test would not be "
           "exercising the mid-run durability path";
  } // drop the in-process SurfaceDb handle: only on-disk state remains below

  // Crash-resume: a fresh open sees kDate0 durably committed even though
  // populate returned an error; kDate1 never completed so it is absent.
  auto reopened = SurfaceDb::open(root.string());
  ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());

  auto part0 = reopened->open_partition(kDate0);
  ASSERT_TRUE(part0.has_value())
      << "kDate0 partition was discarded by the later kDate1 worker throw -- date-"
         "granular durability regressed (R-12)";
  for (const char *symbol : {"AAA", "BBB"}) {
    auto s = reopened->load_surface(kDate0, symbol);
    EXPECT_TRUE(s.has_value()) << "kDate0/" << symbol
                               << " not durable: " << (s ? "" : s.error().to_string());
  }
  // kDate1 never produced a successful fit (both boards threw), so no partition.
  EXPECT_EQ(reopened->open_partition(kDate1).error().code(), ErrorCode::NotFound);

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPopulate, PropagatesStoredSurfacePolicyAndPersistsServedProvenance) {
  const auto root = test_root("surface_policy_provenance");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  SymbolFitConfig symbol_cfg = symbol_config_from_preset(FitPreset::Hft);
  symbol_cfg.surface_policy.quality_mode = FitQualityMode::Accuracy;
  symbol_cfg.surface_policy.outputs = SurfaceOutputs::MarketMark;
  symbol_cfg.surface_policy.risk_admission = RiskAdmission::NotApplicable;
  symbol_cfg.surface_policy.fallback = SurfaceFallback::None;
  ASSERT_TRUE(db->upsert_symbol("AAA", symbol_cfg).has_value());

  const std::vector<CorpusBoard> boards = {make_board(kDate0, "AAA", 100.0, 0.28)};
  auto result = populate_surface_db(*db, boards, SurfaceDbPopulateConfig{});
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
  ASSERT_EQ(result->n_ok, 1u);

  auto archive = db->open_partition(kDate0);
  ASSERT_TRUE(archive.has_value()) << (archive ? "" : archive.error().to_string());
  auto archived = archive->provenance("AAA");
  ASSERT_TRUE(archived.has_value()) << (archived ? "" : archived.error().to_string());
  EXPECT_FALSE(archived->legacy_format);
  EXPECT_EQ(archived->purpose, SurfacePurpose::MarketMark);
  EXPECT_EQ(archived->quality_mode, FitQualityMode::Accuracy);
  EXPECT_EQ(archived->state, SurfaceState::Healthy);

  auto manifested = db->surface_provenance("AAA");
  ASSERT_TRUE(manifested.has_value()) << (manifested ? "" : manifested.error().to_string());
  ASSERT_TRUE(manifested->has_value());
  EXPECT_FALSE((*manifested)->legacy_format);
  EXPECT_EQ((*manifested)->purpose, archived->purpose);
  EXPECT_EQ((*manifested)->quality_mode, archived->quality_mode);
  EXPECT_EQ((*manifested)->state, archived->state);
  EXPECT_EQ((*manifested)->validation.failures, archived->validation.failures);
  EXPECT_EQ((*manifested)->served_generation, archived->served_generation);

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPopulate, HonorsDisabledSymbol) {
  const auto root = test_root("disabled");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  auto bbb_cfg = symbol_config_from_preset(FitPreset::Fast);
  bbb_cfg.enabled = false;
  ASSERT_TRUE(db->upsert_symbol("BBB", bbb_cfg).has_value());

  const std::vector<CorpusBoard> boards = make_boards();
  const SurfaceDbPopulateConfig cfg;
  auto result = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

  EXPECT_EQ(result->n_dates_written, 2u);
  for (const char *date : {kDate0, kDate1}) {
    auto archive = db->open_partition(date);
    ASSERT_TRUE(archive.has_value()) << date;
    EXPECT_TRUE(archive->map_symbol("AAA").has_value());
    EXPECT_EQ(archive->map_symbol("BBB").error().code(), ErrorCode::NotFound);
  }

  const auto bbb_it = std::find_if(result->per_symbol.begin(), result->per_symbol.end(),
                                   [](const PopulateSymbolStats &s) { return s.symbol == "BBB"; });
  ASSERT_NE(bbb_it, result->per_symbol.end());
  EXPECT_EQ(bbb_it->n_attempted, 2u);
  EXPECT_EQ(bbb_it->n_disabled, 2u);
  EXPECT_EQ(bbb_it->n_ok, 0u);
  EXPECT_EQ(bbb_it->n_failed, 0u);

  const auto aaa_it = std::find_if(result->per_symbol.begin(), result->per_symbol.end(),
                                   [](const PopulateSymbolStats &s) { return s.symbol == "AAA"; });
  ASSERT_NE(aaa_it, result->per_symbol.end());
  EXPECT_EQ(aaa_it->n_ok, 2u);

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPopulate, SkipExistingResumes) {
  const auto root = test_root("resume");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const std::vector<CorpusBoard> boards = make_boards();
  const SurfaceDbPopulateConfig cfg;

  auto first = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
  EXPECT_EQ(first->n_dates_written, 2u);
  EXPECT_EQ(first->n_dates_skipped_existing, 0u);
  const std::uint64_t gen_after_first = db->generation();

  auto second = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
  EXPECT_EQ(second->n_dates_skipped_existing, 2u);
  EXPECT_EQ(second->n_dates_written, 0u);
  EXPECT_EQ(db->generation(), gen_after_first);

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPopulate, FailedFitRecordedNotFatal) {
  const auto root = test_root("failed");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  std::vector<CorpusBoard> boards = make_boards();
  // Corrupt the (kDate0, "BBB") board: empty frame -> fit_board Skipped ->
  // populate counts n_failed (no separate "skipped" bucket in populate stats).
  boards[1].frame = QuoteFrame{};

  const SurfaceDbPopulateConfig cfg;
  auto result = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

  EXPECT_EQ(result->n_boards, 4u);
  EXPECT_EQ(result->n_ok, 3u);
  EXPECT_EQ(result->n_failed, 1u);
  EXPECT_EQ(result->n_dates_written, 2u); // kDate0 still written w/ AAA

  auto archive0 = db->open_partition(kDate0);
  ASSERT_TRUE(archive0.has_value());
  EXPECT_TRUE(archive0->map_symbol("AAA").has_value());
  EXPECT_EQ(archive0->map_symbol("BBB").error().code(), ErrorCode::NotFound);

  const auto bbb_it = std::find_if(result->per_symbol.begin(), result->per_symbol.end(),
                                   [](const PopulateSymbolStats &s) { return s.symbol == "BBB"; });
  ASSERT_NE(bbb_it, result->per_symbol.end());
  EXPECT_EQ(bbb_it->n_attempted, 2u);
  EXPECT_EQ(bbb_it->n_failed, 1u);
  EXPECT_EQ(bbb_it->n_ok, 1u);

  // The counter is not the whole story: the cell must NAME itself. Even this
  // no-Error path (an empty board is Skipped, which populate counts as failed)
  // carries a reason rather than contributing an anonymous +1.
  ASSERT_EQ(result->failed_cells.size(), std::size_t{1});
  EXPECT_EQ(result->failed_cells[0].date, kDate0);
  EXPECT_EQ(result->failed_cells[0].symbol, "BBB");
  EXPECT_NE(result->failed_cells[0].detail.find("empty board"), std::string::npos)
      << result->failed_cells[0].detail;

  std::filesystem::remove_all(root);
}

// A symbol config the risk pipeline refuses up front: `pin_curve` + a
// LinearVariance family. PricerFitter::fit rejects that combination in its input
// validation (pricer_fitter.cpp's correctness-policy guard) with a real
// Error{InvalidArgument, "invalid correctness policy for requested risk surface"}.
//
// The point of this test is the MESSAGE, not the count. `fit_board` used to keep
// `st.error().code()` and drop `st.error().message()`, and the populate then
// dropped the code too, so a lost cell reached the operator as nothing but a +1 on
// `n_failed`. Asserting the fitter's own text (not merely that some string is
// present) is what makes a future refactor that re-drops the message fail here.
[[nodiscard]] SymbolFitConfig rejected_risk_config() {
  SymbolFitConfig cfg = symbol_config_from_preset(FitPreset::Populate);
  cfg.pin_curve = true;
  cfg.curve.kind = VolCurveKind::LinearVariance;
  return cfg;
}

TEST(SurfaceDbPopulate, FailedCellCarriesTheFittersOwnReason) {
  const auto root = test_root("failed_reason");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  ASSERT_TRUE(db->upsert_symbol("BBB", rejected_risk_config()).has_value());

  const std::vector<CorpusBoard> boards = make_boards(); // AAA + BBB x 2 dates
  auto result = populate_surface_db(*db, boards, SurfaceDbPopulateConfig{});
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

  EXPECT_EQ(result->n_ok, 2u);     // AAA on both dates
  EXPECT_EQ(result->n_failed, 2u); // BBB on both dates
  ASSERT_EQ(result->failed_cells.size(), std::size_t{2});

  const std::string expected_dates[] = {kDate0, kDate1};
  for (std::size_t i = 0; i < result->failed_cells.size(); ++i) {
    const FailedCell &f = result->failed_cells[i];
    EXPECT_EQ(f.date, expected_dates[i]);
    EXPECT_EQ(f.symbol, "BBB");
    // The fit Error's code survived...
    EXPECT_EQ(f.code, ErrorCode::InvalidArgument);
    // ...and so did its text, verbatim from pricer_fitter.cpp.
    EXPECT_EQ(f.detail, "invalid correctness policy for requested risk surface") << f.detail;
  }

  std::filesystem::remove_all(root);
}

// The failed-cell list is ordered by (date, symbol) — NOT by the order the boards
// were handed in, and NOT by the order the fits happened to finish.
//
// Structural reason: the list is appended by the SINGLE drain thread as it walks
// dates ascending and, inside a date, boards in the populate's (date asc, symbol
// asc) sort order. No fit worker ever touches it, so completion order cannot leak
// in. The two worker budgets below gate that claim the way this suite's other
// determinism tests do: identical output for any thread count is a repo invariant.
//
// The input order is deliberately scrambled and the failing symbols deliberately
// interleave with a succeeding one, so an implementation that pushed from the
// workers (or that appended in input order) produces a visibly different list.
//
// The four failing boards (AAA/MMM x kDate0/kDate1) deliberately use DIFFERENT
// strike counts (make_board_n_strikes), not the identical-size make_board used
// everywhere else in this suite. With identical sizes the U2 LPT claim-order
// stable_sort (surface_db_populate.cpp:233, descending frame rows) has nothing
// to reorder -- all four tie, so the tie-break (original position) happens to
// preserve (date,symbol) order, and a regression that pushed from fit_task
// (worker-side) instead of the single drain thread would only be caught
// *racily*, by concurrently-claimed instant failures under 8 workers. Sizing
// them 11/13/15/17 (see LptClaimsLargestBoardsFirstDeterministically above for
// why n_strikes controls frame.rows.size() monotonically) makes the LPT claim
// order the EXACT REVERSE of (date,symbol) order, so a worker-side push would
// now produce the wrong sequence DETERMINISTICALLY, at every position, under
// any worker count -- not just under a race.
TEST(SurfaceDbPopulate, FailedCellsSortedByDateThenSymbolForAnyWorkerBudget) {
  const auto run = [](std::string_view name, unsigned threads) {
    const auto root = test_root(name);
    auto db = SurfaceDb::create(root.string());
    EXPECT_TRUE(db.has_value());
    // AAA and MMM are refused by the risk pipeline (instant failures); ZZZ fits.
    EXPECT_TRUE(db->upsert_symbol("AAA", rejected_risk_config()).has_value());
    EXPECT_TRUE(db->upsert_symbol("MMM", rejected_risk_config()).has_value());

    std::vector<CorpusBoard> boards; // scrambled: neither date- nor symbol-sorted
    boards.push_back(make_board(kDate1, "ZZZ", 70.0, 0.31));
    boards.push_back(make_board_n_strikes(kDate0, "MMM", 60.0, 0.34, 13));
    boards.push_back(make_board_n_strikes(kDate1, "AAA", 101.0, 0.27, 15));
    boards.push_back(make_board(kDate0, "ZZZ", 69.0, 0.32));
    boards.push_back(make_board_n_strikes(kDate1, "MMM", 61.0, 0.33, 17));
    boards.push_back(make_board_n_strikes(kDate0, "AAA", 100.0, 0.28, 11));

    // The sizes must be genuinely, strictly ordered (else the LPT-contradiction
    // claim above is vacuous): descending row count is [MMM/date1, AAA/date1,
    // MMM/date0, AAA/date0] -- the exact reverse of the pinned (date,symbol)
    // order asserted below ([AAA/date0, MMM/date0, AAA/date1, MMM/date1]).
    EXPECT_GT(boards[4].frame.rows.size(), boards[2].frame.rows.size()) << "MMM/date1 > AAA/date1";
    EXPECT_GT(boards[2].frame.rows.size(), boards[1].frame.rows.size()) << "AAA/date1 > MMM/date0";
    EXPECT_GT(boards[1].frame.rows.size(), boards[5].frame.rows.size()) << "MMM/date0 > AAA/date0";

    SurfaceDbPopulateConfig cfg;
    cfg.n_threads = threads;
    auto result = populate_surface_db(*db, boards, cfg);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    std::vector<FailedCell> failed;
    if (result.has_value()) {
      EXPECT_EQ(result->n_failed, 4u);
      failed = result->failed_cells;
    }
    std::filesystem::remove_all(root);
    return failed;
  };

  const std::vector<FailedCell> serial = run("failed_order_serial", 1u);
  const std::vector<FailedCell> parallel = run("failed_order_par", 8u);

  // The pinned order: date-major, then symbol ascending.
  ASSERT_EQ(serial.size(), std::size_t{4});
  EXPECT_EQ(serial[0].date, kDate0);
  EXPECT_EQ(serial[0].symbol, "AAA");
  EXPECT_EQ(serial[1].date, kDate0);
  EXPECT_EQ(serial[1].symbol, "MMM");
  EXPECT_EQ(serial[2].date, kDate1);
  EXPECT_EQ(serial[2].symbol, "AAA");
  EXPECT_EQ(serial[3].date, kDate1);
  EXPECT_EQ(serial[3].symbol, "MMM");

  // ...and a wide worker budget reproduces it entry for entry, reason included.
  ASSERT_EQ(parallel.size(), serial.size());
  for (std::size_t i = 0; i < serial.size(); ++i) {
    EXPECT_EQ(parallel[i].date, serial[i].date) << "entry " << i;
    EXPECT_EQ(parallel[i].symbol, serial[i].symbol) << "entry " << i;
    EXPECT_EQ(parallel[i].code, serial[i].code) << "entry " << i;
    EXPECT_EQ(parallel[i].detail, serial[i].detail) << "entry " << i;
  }
}

TEST(SurfaceDbPopulate, DateWithZeroSuccessfulFitsWritesNoPartition) {
  const auto root = test_root("empty_date");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  std::vector<CorpusBoard> boards = make_boards();
  // Corrupt BOTH kDate0 boards (indices 0="AAA", 1="BBB") -> zero Ok fits for
  // kDate0 -> no partition written for it (the archive writer rejects an
  // empty item list); kDate1 is untouched and still writes normally.
  boards[0].frame = QuoteFrame{};
  boards[1].frame = QuoteFrame{};

  const SurfaceDbPopulateConfig cfg;
  auto result = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

  EXPECT_EQ(result->n_boards, 4u);
  EXPECT_EQ(result->n_ok, 2u);
  EXPECT_EQ(result->n_failed, 2u);
  EXPECT_EQ(result->n_dates_written, 1u); // only kDate1
  EXPECT_EQ(db->partitions().size(), 1u);
  EXPECT_EQ(db->open_partition(kDate0).error().code(), ErrorCode::NotFound);
  EXPECT_TRUE(db->open_partition(kDate1).has_value());

  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPopulate, StatsCsvShape) {
  const auto root = test_root("stats");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  // Pin AAA (no OOS score -> mean_oos_in_band NaN in its row); BBB uses the
  // default fallback (auto-selected curve -> a real OOS score).
  auto aaa_cfg = symbol_config_from_preset(FitPreset::Fast);
  aaa_cfg.pin_curve = true;
  aaa_cfg.curve = CurveConfig{}; // ConvexDense, node_cap 40
  ASSERT_TRUE(db->upsert_symbol("AAA", aaa_cfg).has_value());

  const std::vector<CorpusBoard> boards = make_boards();
  const SurfaceDbPopulateConfig cfg;
  auto result = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
  ASSERT_EQ(result->n_ok, 4u);

  const auto csv_path = root / "stats.csv";
  const MetaKv meta{{"run", "test"}};
  const Status w = write_populate_stats_csv(*result, meta, csv_path.string());
  ASSERT_TRUE(w.has_value()) << (w ? "" : w.error().to_string());

  const std::string text = read_file(csv_path);
  EXPECT_NE(text.find("# run=test\n"), std::string::npos);
  // FIX-D fix-1 (I2). The header is a pinned contract and it MOVED: `n_carried`
  // is APPENDED (so every older column keeps its position) because a carried
  // symbol's row otherwise names no disposition at all.
  EXPECT_NE(
      text.find(
          "symbol,n_attempted,n_ok,n_failed,n_disabled,success_rate,mean_oos_in_band,n_carried\n"),
      std::string::npos)
      << text;
  EXPECT_NE(text.find("# n_carried=0\n"), std::string::npos) << text;
  // AAA: n_attempted=2, n_ok=2, n_failed=0, n_disabled=0 -> success_rate=1;
  // pinned curve -> no OOS score -> "nan" (the NaN-when-unavailable rule).
  EXPECT_NE(text.find("AAA,2,2,0,0,1,nan,0\n"), std::string::npos) << text;
  // BBB: same counts (a directly-routed, non-ambiguous board also has no
  // selector OOS score even though its curve isn't pinned -- fit_board's
  // `oos_in_band_available` is tied to the selector having run at all, not
  // to pin_curve specifically; mirrors corpus.cpp's CorpusEntry.oos_in_band).
  EXPECT_NE(text.find("BBB,2,2,0,0,1,nan,0\n"), std::string::npos) << text;

  std::filesystem::remove_all(root);
}

// Discriminates "the session_overlay lambda populate_surface_db passes into
// fit_board actually reached PricerFitter::fit's SessionInputs" from "the
// per-symbol config was resolved but silently dropped before the fit ran"
// (the finding: PinnedConfigHonored's node_cap=48/kind_at(0) probe passes
// even though ConvexDense would have been auto-selected anyway, and no test
// exercises a field PricerConfig cannot carry at all).
//
// al_override/al is exactly such a field: pricer_config_for_symbol's
// PricerConfig translation (this file, above) has no al_opts member
// whatsoever, so the ONLY way a manifest's al_override can ever reach
// SessionInputs::deam.al_opts is apply_symbol_config running inside the
// session_overlay hook (surface_db.cpp's apply_symbol_config,
// pricer_fitter.cpp:166-168). Both configs below pin the SAME preset
// (FitPreset::Fast) so PricerConfig(A) and PricerConfig(B) are IDENTICAL --
// isolating the comparison to al_override/al alone (a preset mismatch would
// also change PricerConfig::preset, confounding "did the overlay run" with
// "was a different preset selected").
TEST(SurfaceDbPopulate, SymbolConfigOverlayReachesFit) {
  const std::vector<CorpusBoard> single_board = {make_board(kDate0, "AAA", 100.0, 0.28)};
  const double probe_K = 100.0;
  const double probe_T = year_fraction(kDate0, "2026-04-17");

  // A distinctive AlOpts (all 4 fields differ from al_fast_opts()'s
  // {7,16,4,1e-8} -- see american.hpp) that only apply_symbol_config's
  // al_override branch can install.
  AlOpts distinctive_al;
  distinctive_al.n_collocation = 10;
  distinctive_al.n_quadrature = 20;
  distinctive_al.max_newton_iter = 6;
  distinctive_al.tol = 1.0e-9;

  // Start from the documented preset identity, then explicitly request the
  // market-mark product path. FitPreset::Fast is otherwise a legacy risk
  // request, whose mandatory risk policy deliberately owns Andersen-Lake
  // resolution and would make a mark-only numerical overlay irrelevant.
  SymbolFitConfig fallback_a = symbol_config_from_preset(FitPreset::Fast);
  fallback_a.surface_policy.outputs = SurfaceOutputs::MarketMark;
  fallback_a.surface_policy.risk_admission = RiskAdmission::NotApplicable;
  fallback_a.surface_policy.fallback = SurfaceFallback::None;
  fallback_a.al_override = false; // baseline: preset's own al_opts stands

  SymbolFitConfig cfg_b = fallback_a;
  cfg_b.preset = FitPreset::Fast; // SAME preset as fallback_a
  cfg_b.al_override = true;
  cfg_b.al = distinctive_al;

  // db A: AAA absent from the manifest -> resolves to `fallback_a`.
  const auto root_a = test_root("overlay_a");
  auto db_a = SurfaceDb::create(root_a.string());
  ASSERT_TRUE(db_a.has_value());
  SurfaceDbPopulateConfig cfg_a;
  cfg_a.fallback = fallback_a;
  auto result_a = populate_surface_db(*db_a, single_board, cfg_a);
  ASSERT_TRUE(result_a.has_value()) << (result_a ? "" : result_a.error().to_string());
  ASSERT_EQ(result_a->n_ok, 1u);
  auto s_a = db_a->load_surface(kDate0, "AAA");
  ASSERT_TRUE(s_a.has_value()) << (s_a ? "" : s_a.error().to_string());

  // db B: AAA's manifest entry carries the al_override.
  const auto root_b = test_root("overlay_b");
  auto db_b = SurfaceDb::create(root_b.string());
  ASSERT_TRUE(db_b.has_value());
  ASSERT_TRUE(db_b->upsert_symbol("AAA", cfg_b).has_value());
  auto result_b = populate_surface_db(*db_b, single_board, SurfaceDbPopulateConfig{});
  ASSERT_TRUE(result_b.has_value()) << (result_b ? "" : result_b.error().to_string());
  ASSERT_EQ(result_b->n_ok, 1u);
  auto s_b = db_b->load_surface(kDate0, "AAA");
  ASSERT_TRUE(s_b.has_value()) << (s_b ? "" : s_b.error().to_string());

  // ── Discriminating assertions: A (no override) must differ from B ───────
  // Structural: the stored PricedSurface's PricingContext::al_opts (which
  // to_priced_surface() stamps straight from the fit's resolved
  // SessionInputs::deam.al_opts) differs bit-exactly.
  const AlOpts &al_a = s_a->pricing().al_opts;
  const AlOpts &al_b = s_b->pricing().al_opts;
  EXPECT_NE(al_a.n_collocation, al_b.n_collocation);
  EXPECT_NE(al_a.n_quadrature, al_b.n_quadrature);
  EXPECT_NE(al_a.max_newton_iter, al_b.max_newton_iter);
  EXPECT_NE(al_a.tol, al_b.tol);
  // B's stored al_opts equal exactly the manifest's distinctive value -- the
  // VALUE reached the fit, not just "some field changed".
  EXPECT_EQ(al_b.n_collocation, distinctive_al.n_collocation);
  EXPECT_EQ(al_b.n_quadrature, distinctive_al.n_quadrature);
  EXPECT_EQ(al_b.max_newton_iter, distinctive_al.max_newton_iter);
  EXPECT_EQ(al_b.tol, distinctive_al.tol);

  // Behavioral: a different Andersen-Lake discretization re-prices the SAME
  // (K, T, side) on the SAME fitted board to a genuinely different American
  // value -- not just a different label on an otherwise-identical surface.
  auto fv_a = s_a->fair_value(probe_K, probe_T, Side::Call);
  auto fv_b = s_b->fair_value(probe_K, probe_T, Side::Call);
  ASSERT_TRUE(fv_a.has_value()) << (fv_a ? "" : fv_a.error().to_string());
  ASSERT_TRUE(fv_b.has_value()) << (fv_b ? "" : fv_b.error().to_string());
  EXPECT_NE(*fv_a, *fv_b);

  // ── Flake guard: the SAME manifest config into a fresh db reproduces the
  // SAME surface (rules out "A and B just happened to differ this run"). ──
  const auto root_c = test_root("overlay_c");
  auto db_c = SurfaceDb::create(root_c.string());
  ASSERT_TRUE(db_c.has_value());
  ASSERT_TRUE(db_c->upsert_symbol("AAA", cfg_b).has_value());
  auto result_c = populate_surface_db(*db_c, single_board, SurfaceDbPopulateConfig{});
  ASSERT_TRUE(result_c.has_value()) << (result_c ? "" : result_c.error().to_string());
  auto s_c = db_c->load_surface(kDate0, "AAA");
  ASSERT_TRUE(s_c.has_value()) << (s_c ? "" : s_c.error().to_string());

  const AlOpts &al_c = s_c->pricing().al_opts;
  EXPECT_EQ(al_b.n_collocation, al_c.n_collocation);
  EXPECT_EQ(al_b.n_quadrature, al_c.n_quadrature);
  EXPECT_EQ(al_b.max_newton_iter, al_c.max_newton_iter);
  EXPECT_EQ(al_b.tol, al_c.tol);
  auto fv_c = s_c->fair_value(probe_K, probe_T, Side::Call);
  ASSERT_TRUE(fv_c.has_value()) << (fv_c ? "" : fv_c.error().to_string());
  EXPECT_EQ(*fv_b, *fv_c);

  std::filesystem::remove_all(root_a);
  std::filesystem::remove_all(root_b);
  std::filesystem::remove_all(root_c);
}

// U4 (R-14) [pure-refactor]: shared worker budget for small books. When a book
// is smaller than the worker budget, the outer fans every board across the pool
// but the OLD fixed split pinned each board's inner fit to a single worker
// (fit_workers = 1), leaving budget - n_boards cores idle (2 boards on a 12-wide
// pool used 2 cores, 10 idle). U4 instead sizes each board's inner budget as
// budget / min(budget, n_boards), so the per-board slices sum to the whole
// budget: a 1-board run claims all 12, a 4-board run gets 3 each -- every core
// busy. This asserts the resolved per-board budget (observed via the
// on_inner_fit_workers hook) for books of 1..4 boards under a 12-wide budget,
// and that outer_threads * inner_budget covers the pool (no idle cores).
TEST(SurfaceDbPopulate, SharedWorkerBudgetSizesInnerFromSharedPool) {
  struct Spec {
    const char *symbol;
    double spot;
    double sigma0;
  };
  const Spec specs[] = {
      {"AAA", 100.0, 0.28},
      {"BBB", 60.0, 0.34},
      {"CCC", 80.0, 0.30},
      {"DDD", 120.0, 0.26},
  };
  constexpr unsigned kBudget = 12u; // wider than any book below -> a real split

  // budget / min(budget, n): 12/1, 12/2, 12/3, 12/4.
  const unsigned expected_inner[] = {12u, 6u, 4u, 3u};

  for (std::size_t n = 1u; n <= 4u; ++n) {
    const auto root = test_root(std::string("shared_budget_n") + std::to_string(n));
    auto db = SurfaceDb::create(root.string());
    ASSERT_TRUE(db.has_value());

    std::vector<CorpusBoard> boards;
    for (std::size_t b = 0u; b < n; ++b) {
      boards.push_back(make_board(kDate0, specs[b].symbol, specs[b].spot, specs[b].sigma0));
    }

    // FIX-4: the budget is live, so the hook fires once per board CLAIM plus once
    // per surface-build request, from the fit worker threads. The offer resolved
    // against the FULL book (outstanding == n) is the deterministic one: nothing
    // can have completed before the first claim, so such an offer always exists
    // and always equals the U4 sizing rule for this book size.
    unsigned captured_inner = 0u;
    bool hook_seen = false;
    std::mutex hook_mu;
    PopulateTestHooks hooks;
    hooks.on_inner_fit_workers = [&](const std::string &, unsigned inner, std::size_t left) {
      const std::lock_guard<std::mutex> lock(hook_mu);
      if (left == n) {
        captured_inner = inner;
        hook_seen = true;
      }
    };

    SurfaceDbPopulateConfig cfg;
    cfg.n_threads = kBudget;
    // C4 wave-2: this test verifies the inner-worker SIZING math from the requested
    // outer budget; disable the P-core cap so `worker_budget` stays at kBudget=12
    // (the cap, tested separately, would otherwise clamp it to the P-core count).
    cfg.pin_outer_workers = false;
    auto result = populate_surface_db(*db, boards, cfg, &hooks);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    EXPECT_EQ(result->n_ok, static_cast<std::uint32_t>(n));

    ASSERT_TRUE(hook_seen) << "on_inner_fit_workers never fired for n=" << n;
    // Each board's inner fit is offered the shared-pool slice, strictly more
    // than the old fixed split of 1 -- the recovered idle cores.
    EXPECT_EQ(captured_inner, expected_inner[n - 1u]) << "n=" << n;
    EXPECT_GT(captured_inner, 1u) << "n=" << n;

    // The outer runs min(budget, n_boards) boards concurrently; each takes an
    // inner slice, and the slices cover the whole pool -- no idle cores.
    const unsigned outer_threads = std::min<unsigned>(kBudget, static_cast<unsigned>(n));
    EXPECT_EQ(outer_threads * captured_inner, kBudget) << "n=" << n;

    std::filesystem::remove_all(root);
  }
}

// U4 (R-14) [pure-refactor]: sizing the inner budget from the shared pool is a
// SCHEDULING change, never a numeric one. A 2-board book fit under a split
// budget (each board's inner fit offered 4 workers) must produce surfaces
// bit-identical to the serial reference (one outer worker, inner auto-sized).
// This is the same determinism-across-worker-counts contract that
// GlobalParallelQueuePreservesDeterministicPartitions locks in for the
// fit_workers = 1 case (a book at/above the budget); here the book is SMALLER
// than the budget, so U4 takes the new split path (inner = 4, not 1) -- the
// captured budget proves the comparison is not vacuously two identical runs.
TEST(SurfaceDbPopulate, SharedWorkerBudgetKeepsOutputByteIdentical) {
  const std::vector<CorpusBoard> boards = {
      make_board(kDate0, "AAA", 100.0, 0.28),
      make_board(kDate0, "BBB", 60.0, 0.34),
  };

  // Reference: serial outer (budget 1) -> inner auto-sized.
  const auto ref_root = test_root("shared_budget_identity_ref");
  auto ref_db = SurfaceDb::create(ref_root.string());
  ASSERT_TRUE(ref_db.has_value());
  SurfaceDbPopulateConfig ref_cfg;
  ref_cfg.n_threads = 1u;
  auto ref = populate_surface_db(*ref_db, boards, ref_cfg);
  ASSERT_TRUE(ref.has_value()) << (ref ? "" : ref.error().to_string());
  ASSERT_EQ(ref->n_ok, 2u);

  // U4 split path: 2 boards on an 8-wide budget -> 8 / min(8, 2) = 4 each.
  const auto split_root = test_root("shared_budget_identity_split");
  auto split_db = SurfaceDb::create(split_root.string());
  ASSERT_TRUE(split_db.has_value());
  unsigned captured_inner = 0u;
  std::mutex captured_mu;
  PopulateTestHooks hooks;
  // FIX-4: pin the offer resolved against the full 2-board book (the first claim
  // always sees outstanding == 2); later offers legitimately widen as the book
  // drains, and that widening is exactly what this byte-identity gate now covers.
  hooks.on_inner_fit_workers = [&](const std::string &, unsigned inner, std::size_t left) {
    const std::lock_guard<std::mutex> lock(captured_mu);
    if (left == 2u) {
      captured_inner = inner;
    }
  };
  SurfaceDbPopulateConfig split_cfg;
  split_cfg.n_threads = 8u;
  auto split = populate_surface_db(*split_db, boards, split_cfg, &hooks);
  ASSERT_TRUE(split.has_value()) << (split ? "" : split.error().to_string());
  ASSERT_EQ(split->n_ok, 2u);
  ASSERT_EQ(captured_inner, 4u) << "expected the small-book split path (inner = 4), not the "
                                   "fixed split of 1 -- else the identity check is vacuous";

  for (const char *symbol : {"AAA", "BBB"}) {
    const auto ref_surface = ref_db->load_surface(kDate0, symbol);
    const auto split_surface = split_db->load_surface(kDate0, symbol);
    ASSERT_TRUE(ref_surface.has_value()) << symbol;
    ASSERT_TRUE(split_surface.has_value()) << symbol;
    expect_surface_bits_equal(*split_surface, *ref_surface);
  }

  std::filesystem::remove_all(ref_root);
  std::filesystem::remove_all(split_root);
}

// ── FIX-4 gate: the inner budget must be reclaimed for a board that is ALREADY
// CLAIMED and STILL RUNNING, not merely at claim time ───────────────────────
//
// The defect this pins: `inner_fit_workers` used to be ONE constant for the whole
// populate call, derived from the TOTAL enabled book. The straggler — the last
// board still fitting while every other outer worker sits idle at the join
// barrier — was never re-offered a wider slice, because after the claim there was
// no further decision point at all.
//
// The gate holds ONE board inside its fit — deterministically, on a condition
// variable released by `on_board_fit_done`; no sleeps, no wall-clock assertion —
// until every other board has retired, then asserts that board's offers move from
// 1 (claimed while the pool was saturated) to the WHOLE budget (re-offered while
// it is still running). Non-vacuous by construction:
//   * saturation is OBSERVED, not assumed: every offer resolved against
//     `left >= kBudget` is asserted == 1 INDIVIDUALLY, and `saturated_offers > 0`
//     fails outright if no offer ever saw a full pool;
//   * `held.back() == kBudget` is unreachable without a post-claim re-offer. With
//     the production change reverted the held board gets exactly ONE offer whose
//     value is 1, so `held.size() > 1u`, `held.back() == kBudget` and
//     `reclaimed_offers > 0` all fail together.
// Byte-identity rides along in the same run: the held board is fitted at width
// kBudget while its siblings ran at width 1, and every surface must still match
// the serial reference bit-for-bit — worker count is a perf knob, never a value.
TEST(SurfaceDbPopulate, StragglerReclaimsInnerWorkersWhileStillRunning) {
  constexpr unsigned kBudget = 4u;
  const char *const kSymbols[] = {"AAA", "BBB", "CCC", "DDD", "EEE"};
  constexpr std::size_t kBoards = std::size(kSymbols); // > kBudget: claims saturate

  std::vector<CorpusBoard> boards;
  boards.reserve(kBoards);
  for (std::size_t b = 0; b < kBoards; ++b) {
    boards.push_back(make_board(kDate0, kSymbols[b], 100.0 + 10.0 * static_cast<double>(b),
                                0.26 + 0.02 * static_cast<double>(b)));
  }

  struct Offer {
    std::string symbol;
    unsigned workers;
    std::size_t left;
  };
  std::mutex mu;
  std::condition_variable cv;
  std::vector<Offer> offers;
  std::string held_symbol;     // the board we pin inside its fit
  bool drained = false;        // every OTHER board has retired
  bool released_by_drain = false;

  PopulateTestHooks hooks;
  hooks.on_inner_fit_workers = [&](const std::string &symbol, unsigned workers, std::size_t left) {
    const std::lock_guard<std::mutex> lock(mu);
    offers.push_back(Offer{symbol, workers, left});
  };
  hooks.on_board_fit_done = [&](std::size_t left) {
    const std::lock_guard<std::mutex> lock(mu);
    if (left <= 1u) {
      drained = true;
      cv.notify_all();
    }
  };
  hooks.before_board_fit = [&](const std::string &, const std::string &symbol) {
    std::unique_lock<std::mutex> lock(mu);
    if (!held_symbol.empty()) {
      return; // one straggler is enough; everyone else fits normally
    }
    held_symbol = symbol;
    // Hang guard only — NOT a timing assertion. The pass condition is `drained`,
    // which `released_by_drain` records; a timeout releases the board so the test
    // fails on its assertions instead of hanging the suite.
    released_by_drain = cv.wait_for(lock, std::chrono::seconds(300), [&] { return drained; });
  };

  const auto root = test_root("straggler_reclaim");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  SurfaceDbPopulateConfig cfg;
  cfg.n_threads = kBudget;
  cfg.pin_outer_workers = false; // keep worker_budget == kBudget (no P-core cap)
  auto populated = populate_surface_db(*db, boards, cfg, &hooks);
  ASSERT_TRUE(populated.has_value()) << (populated ? "" : populated.error().to_string());
  ASSERT_EQ(populated->n_ok, static_cast<std::uint32_t>(kBoards));

  ASSERT_FALSE(held_symbol.empty()) << "before_board_fit never fired";
  EXPECT_TRUE(released_by_drain)
      << "the straggler was released by the hang guard, not by the drain — the "
         "on_board_fit_done release seam did not fire";

  // Per-offer saturation observation: while at least kBudget boards are
  // outstanding the pool is full and NO board may be offered more than one worker.
  std::size_t saturated_offers = 0u;
  std::size_t reclaimed_offers = 0u;
  std::vector<unsigned> held;
  for (const Offer &o : offers) {
    if (o.left >= kBudget) {
      ++saturated_offers;
      EXPECT_EQ(o.workers, 1u) << "offer to " << o.symbol << " with " << o.left
                               << " boards outstanding oversubscribed the pool";
    } else if (o.workers > 1u) {
      ++reclaimed_offers;
    }
    if (o.symbol == held_symbol) {
      held.push_back(o.workers);
    }
  }
  EXPECT_GT(saturated_offers, 0u) << "no offer was ever resolved against a full pool — the test "
                                     "did not observe saturation and proves nothing";
  EXPECT_GT(reclaimed_offers, 0u) << "no board was ever offered more than one worker after the "
                                     "pool drained — nothing was reclaimed";

  // The straggler itself: claimed while saturated, re-offered the whole budget
  // while STILL RUNNING. This is the pair the fix exists to produce.
  ASSERT_GT(held.size(), 1u) << held_symbol
                             << " was offered a budget exactly once — the reclaim is claim-time "
                                "only, which is the defect";
  EXPECT_EQ(held.front(), 1u) << held_symbol << " was not claimed against a saturated pool";
  EXPECT_EQ(held.back(), kBudget)
      << held_symbol << " never reclaimed the drained pool while still running";

  // Determinism: the widths above varied WITHIN one run, so every surface must
  // still be byte-identical to the serial reference.
  const auto ref_root = test_root("straggler_reclaim_ref");
  auto ref_db = SurfaceDb::create(ref_root.string());
  ASSERT_TRUE(ref_db.has_value());
  SurfaceDbPopulateConfig ref_cfg;
  ref_cfg.n_threads = 1u;
  auto ref = populate_surface_db(*ref_db, boards, ref_cfg);
  ASSERT_TRUE(ref.has_value()) << (ref ? "" : ref.error().to_string());
  ASSERT_EQ(ref->n_ok, static_cast<std::uint32_t>(kBoards));
  for (const char *symbol : kSymbols) {
    const auto got = db->load_surface(kDate0, symbol);
    const auto want = ref_db->load_surface(kDate0, symbol);
    ASSERT_TRUE(got.has_value()) << symbol;
    ASSERT_TRUE(want.has_value()) << symbol;
    expect_surface_bits_equal(*got, *want);
  }

  std::filesystem::remove_all(root);
  std::filesystem::remove_all(ref_root);
}

// F1 (WS-F): the bulk `Populate` preset must honor the cheaper Andersen-Lake
// de-Am block (al_fast_opts) on the served RISK surface, while the reference
// `Robust` preset keeps al_default_opts — the C3 tier gap that apply_risk_policy
// silently collapsed until the preset-keyed `risk_deam_al` was added. Both presets
// map to the SAME quality mode (Balanced/Risk via map_legacy_fit_preset), so the
// ONLY intended difference between the two served surfaces is the AL de-Am block.
// Also asserts served-surface determinism: same inputs + same config => identical
// archive bytes on a re-populate.
TEST(SurfaceDbPopulate, PopulatePresetHonorsFastAlDeAmTierAndStaysDeterministic) {
  namespace led = atx::vol::counters::ledger;
  const std::vector<CorpusBoard> boards = {
      make_board(kDate0, "AAA", 100.0, 0.28),
      make_board(kDate0, "BBB", 60.0, 0.34),
  };

  // ── Populate tier: F1 routes the de-Am inversions through al_fast_opts ──
  const auto pop_root = test_root("fastal_populate");
  auto pop_db = SurfaceDb::create(pop_root.string());
  ASSERT_TRUE(pop_db.has_value());
  SurfaceDbPopulateConfig pop_cfg;
  pop_cfg.fallback = symbol_config_from_preset(FitPreset::Populate); // canonical bulk config
  pop_cfg.n_threads = 1u; // serial => the ledger delta is an exact per-run attribution
  const led::Counts pop_before = led::snapshot();
  auto pop = populate_surface_db(*pop_db, boards, pop_cfg);
  const led::Counts pop_after = led::snapshot();
  ASSERT_TRUE(pop.has_value()) << (pop ? "" : pop.error().to_string());
  ASSERT_EQ(pop->n_ok, 2u);
  const std::uint64_t pop_solves =
      pop_after.get(led::Solve::AlBoundarySolves) - pop_before.get(led::Solve::AlBoundarySolves);

  // ── Robust tier: reference al_default_opts on the de-Am inversions ──
  const auto rob_root = test_root("fastal_robust");
  auto rob_db = SurfaceDb::create(rob_root.string());
  ASSERT_TRUE(rob_db.has_value());
  SurfaceDbPopulateConfig rob_cfg;
  rob_cfg.fallback = symbol_config_from_preset(FitPreset::Robust);
  rob_cfg.n_threads = 1u;
  const led::Counts rob_before = led::snapshot();
  auto rob = populate_surface_db(*rob_db, boards, rob_cfg);
  const led::Counts rob_after = led::snapshot();
  ASSERT_TRUE(rob.has_value()) << (rob ? "" : rob.error().to_string());
  ASSERT_EQ(rob->n_ok, 2u);
  const std::uint64_t rob_solves =
      rob_after.get(led::Solve::AlBoundarySolves) - rob_before.get(led::Solve::AlBoundarySolves);

  // The served bytes carry the resolved SessionInputs::deam.al_opts. Populate now
  // stamps the FAST block {7,16,4,1e-8}; Robust the reference block {12,24,8,1e-10}.
  const AlOpts fast = al_fast_opts();
  const AlOpts ref = al_default_opts();
  ASSERT_NE(fast.n_collocation, ref.n_collocation); // guards the fixtures are distinct
  for (const char *sym : {"AAA", "BBB"}) {
    const auto ps_pop = pop_db->load_surface(kDate0, sym);
    const auto ps_rob = rob_db->load_surface(kDate0, sym);
    ASSERT_TRUE(ps_pop.has_value()) << sym;
    ASSERT_TRUE(ps_rob.has_value()) << sym;
    EXPECT_EQ(ps_pop->pricing().al_opts.n_collocation, fast.n_collocation) << sym;
    EXPECT_EQ(ps_pop->pricing().al_opts.n_quadrature, fast.n_quadrature) << sym;
    EXPECT_EQ(ps_pop->pricing().al_opts.max_newton_iter, fast.max_newton_iter) << sym;
    EXPECT_EQ(ps_rob->pricing().al_opts.n_collocation, ref.n_collocation) << sym;
    EXPECT_EQ(ps_rob->pricing().al_opts.n_quadrature, ref.n_quadrature) << sym;
  }

  // Per-seed AL work: al_fast does 7 collocation x <=4 Newton per boundary seed vs
  // al_default's 12 x <=8 (~3.4x fewer boundary-node solves) and 16 vs 24 quadrature
  // nodes. The ledger counts SEEDS (carry/inversion budget), so the seed COUNT is
  // preserved; the win is per-seed work plus Populate's looser iv_tol (1e-5).
  std::printf("[F1 fast-AL tier] Populate(al_fast 7x16x4) AlBoundarySolves=%llu  "
              "Robust(al_default 12x24x8)=%llu\n",
              static_cast<unsigned long long>(pop_solves),
              static_cast<unsigned long long>(rob_solves));

  // ── Determinism: same inputs + same Populate config => byte-identical archive ──
  const auto repro_root = test_root("fastal_populate_repro");
  auto repro_db = SurfaceDb::create(repro_root.string());
  ASSERT_TRUE(repro_db.has_value());
  auto repro = populate_surface_db(*repro_db, boards, pop_cfg);
  ASSERT_TRUE(repro.has_value()) << (repro ? "" : repro.error().to_string());
  ASSERT_EQ(repro->n_ok, 2u);
  for (const char *sym : {"AAA", "BBB"}) {
    const auto a = pop_db->load_surface(kDate0, sym);
    const auto b = repro_db->load_surface(kDate0, sym);
    ASSERT_TRUE(a.has_value()) << sym;
    ASSERT_TRUE(b.has_value()) << sym;
    expect_surface_bits_equal(*b, *a);
  }

  std::filesystem::remove_all(pop_root);
  std::filesystem::remove_all(rob_root);
  std::filesystem::remove_all(repro_root);
}

TEST(SurfaceDbPopulate, PinnedConfigHonored) {
  const auto root = test_root("pinned");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  auto aaa_cfg = symbol_config_from_preset(FitPreset::Fast);
  aaa_cfg.pin_curve = true;
  aaa_cfg.curve.kind = VolCurveKind::ConvexDense;
  aaa_cfg.curve.convex.node_cap = 48;
  ASSERT_TRUE(db->upsert_symbol("AAA", aaa_cfg).has_value());

  const std::vector<CorpusBoard> boards = make_boards();
  const SurfaceDbPopulateConfig cfg;
  auto result = populate_surface_db(*db, boards, cfg);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());

  auto s = db->load_surface(kDate0, "AAA");
  ASSERT_TRUE(s.has_value()) << (s ? "" : s.error().to_string());
  EXPECT_EQ(s->kind_at(0), VolCurveKind::ConvexDense);

  std::filesystem::remove_all(root);
}

// ── FIX-D: carry-over instead of re-fit on a whole-partition rewrite ─────────
//
// A date-keyed partition must be written whole, so adding one cell rewrites the
// file. It used to also RE-FIT every cell already in it: on the production
// universe, 3 permanently-failing cells dragged 147 healthy siblings back through
// the fitter on every resume. These tests pin the fix and its safety gates.

namespace {

[[nodiscard]] std::vector<std::byte> read_file_bytes(const std::filesystem::path &p) {
  std::ifstream is(p, std::ios::binary | std::ios::ate);
  EXPECT_TRUE(is.good()) << p.string();
  if (!is.good()) {
    return {};
  }
  const auto n = static_cast<std::size_t>(is.tellg());
  is.seekg(0);
  std::vector<std::byte> out(n);
  is.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(n));
  EXPECT_TRUE(is.good()) << p.string();
  return out;
}

[[nodiscard]] std::filesystem::path partition_file(const std::filesystem::path &root,
                                                   std::string_view key) {
  return root / std::string(kSurfaceDbPartitionDir) /
         (std::string(key) + std::string(kSurfaceDbPartitionExt));
}

// The ACTUAL stored bytes of one symbol's record, sliced straight out of the
// partition file at the offset/size the archive directory reports. Deliberately
// not a checksum or a summary: a carried record could round-trip a stale CRC
// field unchanged and still have lost a payload byte.
[[nodiscard]] std::vector<std::byte> stored_record_bytes(const std::filesystem::path &root,
                                                         std::string_view key,
                                                         std::string_view symbol) {
  const std::vector<std::byte> file = read_file_bytes(partition_file(root, key));
  EXPECT_FALSE(file.empty());
  auto arch = SurfaceArchiveV2::open(std::vector<std::byte>(file));
  EXPECT_TRUE(arch.has_value());
  if (!arch.has_value()) {
    return {};
  }
  auto entry = arch->find(symbol);
  EXPECT_TRUE(entry.has_value()) << symbol;
  if (!entry.has_value()) {
    return {};
  }
  const auto off = static_cast<std::size_t>(entry->surface_offset);
  const auto size = static_cast<std::size_t>(entry->surface_size);
  EXPECT_LE(off + size, file.size());
  return std::vector<std::byte>(file.begin() + static_cast<std::ptrdiff_t>(off),
                                file.begin() + static_cast<std::ptrdiff_t>(off + size));
}

[[nodiscard]] UniversePopulateSpec carry_spec(unsigned workers = 0u) {
  UniversePopulateSpec spec;
  spec.index_symbol = ""; // no index pin -- let the auto-selector fit
  spec.preset = FitPreset::Fast;
  spec.fit_workers = workers;
  return spec;
}

// Two healthy symbols on one date; the partition they produce is the thing a
// later rewrite must not disturb.
[[nodiscard]] std::vector<CorpusBoard> carry_seed_boards() {
  return {make_board(kDate0, "AAA", 100.0, 0.28), make_board(kDate0, "BBB", 60.0, 0.34)};
}

} // namespace

// FIX-D fix-1 (I3), on a REAL stats struct rather than a hand-built one: a
// carried symbol must not report a 0% fit success rate.
//
// `success_rate` is a FIT rate. A carried cell was never offered to the fitter,
// so it belongs in neither half of it. Left in the denominator it read
// n_ok=0 / n_attempted=1 = 0 on every healthy symbol of a converged resume --
// i.e. the report said the database had totally failed while it was working
// perfectly, the same false verdict as C1 in a different column. Excluding it
// EMPTIES the denominator, and an empty denominator means the rate is undefined,
// not zero: the row now says "nan", the same convention it already uses for an
// unavailable mean_oos_in_band.
TEST(SurfaceDbPopulate, CarriedCellsLeaveTheFitSuccessRateUndefinedNotZero) {
  const auto root = test_root("carry_stats_rate");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u);

  // CCC fails forever, so kDate0 is rewritten on every run and AAA/BBB are
  // carried: the production shape, in miniature.
  ASSERT_TRUE(db->upsert_symbol("CCC", rejected_risk_config()).has_value());
  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "CCC", 80.0, 0.30));
  auto cov2 = populate_universe_streaming(*db, full, carry_spec());
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());
  ASSERT_EQ(cov2->cells_carried, 2u) << "the carry must actually have engaged";
  ASSERT_EQ(cov2->cells_ok, 0u);

  SurfaceDbPopulateStats stats;
  stats.n_boards = cov2->cells_loaded;
  stats.n_ok = cov2->cells_ok;
  stats.n_failed = cov2->cells_failed;
  stats.n_carried = cov2->cells_carried;
  stats.n_dates_written = cov2->dates_written;
  stats.per_symbol = cov2->per_symbol;

  const auto csv_path = root / "stats.csv";
  const Status w = write_populate_stats_csv(stats, MetaKv{}, csv_path.string());
  ASSERT_TRUE(w.has_value()) << (w ? "" : w.error().to_string());
  const std::string text = read_file(csv_path);

  // AAA and BBB: attempted=1, ok=0, failed=0, disabled=0, carried=1 -> the fit
  // denominator is empty -> nan, NOT 0.
  for (const char *symbol : {"AAA", "BBB"}) {
    const std::string row = std::string(symbol) + ",1,0,0,0,nan,nan,1\n";
    EXPECT_NE(text.find(row), std::string::npos)
        << symbol << " must report an UNDEFINED fit rate and a carried count\n"
        << text;
    EXPECT_EQ(text.find(std::string(symbol) + ",1,0,0,0,0,"), std::string::npos)
        << symbol << " still reports a 0% fit success rate\n"
        << text;
  }
  // CCC genuinely failed its one fit: a real 0% over a non-empty denominator,
  // which must stay 0 and must be distinguishable from the carried rows above.
  EXPECT_NE(text.find("CCC,1,0,1,0,0,nan,0\n"), std::string::npos) << text;
  // The run-level counter reaches the meta block too.
  EXPECT_NE(text.find("# n_carried=2\n"), std::string::npos) << text;

  std::filesystem::remove_all(root);
}

// FIX-D newly COUPLES the carry set to the inner worker count, and carry-over's
// whole premise ("a resume equals a fresh build for the cells it does re-fit")
// therefore now rests on the byte-identity-across-worker-counts invariant.
//
// The mechanism: carried cells are excluded from `fit_positions`, which shrinks
// `n_fit_boards`, which is what the shared budget is divided by. A date whose 3
// boards are all fit at budget 8 gives each board's inner fit 8/3; on a resume
// where 2 of the 3 are carried, the ONE refit board is fit with the whole 8. Same
// cell, different inner allocation, and nothing but that invariant says the bytes
// come out the same.
//
// The invariant IS tested directly (SharedWorkerBudgetKeepsOutputByteIdentical),
// but never THROUGH the carry path -- every FIX-D/E/F test runs at `carry_spec()`,
// i.e. fit_workers 0. This closes that gap: same symbol, same board, reached once
// as one of three fresh fits and once as the sole survivor of a carry, and the
// surfaces must be bit-identical.
TEST(SurfaceDbPopulate, CarryPathKeepsTheRefitCellBitIdenticalAcrossWorkerBudgets) {
  std::vector<CorpusBoard> all = carry_seed_boards();
  all.push_back(make_board(kDate0, "CCC", 80.0, 0.30));

  // Reference: one fresh pass, all three boards offered to the fitter together.
  const auto ref_root = test_root("carry_worker_identity_ref");
  auto ref_db = SurfaceDb::create(ref_root.string());
  ASSERT_TRUE(ref_db.has_value());
  auto ref = populate_universe_streaming(*ref_db, all, carry_spec(8u));
  ASSERT_TRUE(ref.has_value()) << (ref ? "" : ref.error().to_string());
  ASSERT_EQ(ref->cells_ok, 3u);
  ASSERT_EQ(ref->cells_carried, 0u) << "the reference must be a FRESH fit, not a carry";

  // Resume: AAA/BBB are seeded first, so the second pass carries them and CCC is
  // the only board the fitter ever sees on the rewrite.
  const auto res_root = test_root("carry_worker_identity_resume");
  auto res_db = SurfaceDb::create(res_root.string());
  ASSERT_TRUE(res_db.has_value());
  auto seed = populate_universe_streaming(*res_db, carry_seed_boards(), carry_spec(8u));
  ASSERT_TRUE(seed.has_value()) << (seed ? "" : seed.error().to_string());
  ASSERT_EQ(seed->cells_ok, 2u);
  auto resume = populate_universe_streaming(*res_db, all, carry_spec(8u));
  ASSERT_TRUE(resume.has_value()) << (resume ? "" : resume.error().to_string());
  ASSERT_EQ(resume->cells_carried, 2u) << "the carry must actually have engaged";
  ASSERT_EQ(resume->cells_ok, 1u) << "CCC must be the only cell fit on the rewrite";
  ASSERT_EQ(resume->cells_refit, 0u);

  // The cell that was FIT under two different inner allocations.
  const auto ref_ccc = ref_db->load_surface(kDate0, "CCC");
  const auto res_ccc = res_db->load_surface(kDate0, "CCC");
  ASSERT_TRUE(ref_ccc.has_value()) << (ref_ccc ? "" : ref_ccc.error().to_string());
  ASSERT_TRUE(res_ccc.has_value()) << (res_ccc ? "" : res_ccc.error().to_string());
  expect_surface_bits_equal(*res_ccc, *ref_ccc);

  // And the carried pair, which the rewrite must have re-emitted rather than
  // re-fitted -- same comparison, so a silent re-fit at a different budget would
  // show up here too.
  for (const char *symbol : {"AAA", "BBB"}) {
    const auto ref_s = ref_db->load_surface(kDate0, symbol);
    const auto res_s = res_db->load_surface(kDate0, symbol);
    ASSERT_TRUE(ref_s.has_value()) << symbol;
    ASSERT_TRUE(res_s.has_value()) << symbol;
    expect_surface_bits_equal(*res_s, *ref_s);
  }

  std::filesystem::remove_all(ref_root);
  std::filesystem::remove_all(res_root);
}


// THE acceptance gate. A resume over an unchanged database and hive must re-fit
// NOTHING that already succeeded, while still retrying the cell that failed.
//
// `cells_to_fit == 0` is deliberately NOT asserted: it is unachievable by design
// on data containing permanent failures. There is no persisted known-failed state
// (surface_db_build.hpp), precisely so a TRANSIENT failure stays retryable, so a
// permanently-failing cell is re-attempted on every run forever. The honest
// invariant is `cells_refit == 0`.
TEST(SurfaceDbPopulate, ResumeCarriesHealthySiblingsAndStillRetriesTheFailingCell) {
  const auto root = test_root("carry_retry");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u) << "the seed boards must fit for this test to mean anything";

  // CCC is a PERMANENTLY failing cell: the risk pipeline refuses this config in
  // its input validation, on every run, forever.
  ASSERT_TRUE(db->upsert_symbol("CCC", rejected_risk_config()).has_value());
  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "CCC", 80.0, 0.30));

  // Twice: the steady state must be steady, not merely reached once.
  for (int run = 1; run <= 2; ++run) {
    auto cov = populate_universe_streaming(*db, full, carry_spec());
    ASSERT_TRUE(cov.has_value()) << "run " << run << ": " << (cov ? "" : cov.error().to_string());
    EXPECT_EQ(cov->cells_refit, 0u)
        << "run " << run << ": a resume must re-fit nothing that already succeeded";
    EXPECT_EQ(cov->cells_carried, 2u) << "run " << run;
    EXPECT_EQ(cov->cells_to_fit, 1u)
        << "run " << run << ": the failing cell is retried forever, by design";
    EXPECT_EQ(cov->cells_failed, 1u) << "run " << run;
    EXPECT_EQ(cov->dates_written, 1u) << "run " << run;
    // The carried cells are not counted as fits: cells_ok keeps meaning "cells
    // this run FITTED", which is what is_total_fit_failure depends on.
    EXPECT_EQ(cov->cells_ok, 0u) << "run " << run;
    // The failing cell still names itself (FIX-A's channel must survive).
    ASSERT_EQ(cov->failed_cells.size(), std::size_t{1}) << "run " << run;
    EXPECT_EQ(cov->failed_cells[0].symbol, "CCC") << "run " << run;

    // ── The exit code this shape produces ────────────────────────────────────
    // END-TO-END, on a REAL coverage struct rather than a hand-built one: this
    // steady state (cells_to_fit > 0, cells_ok == 0, cells_carried > 0) must NOT
    // read as a total fit failure. It did before the carry clause was added to
    // `is_total_fit_failure`, and the CLI's diagnostic for that verdict tells the
    // operator to re-run with a different --r -- which on this healthy converged
    // database would invalidate every surface in it. The predicate is unit-tested
    // on synthetic reports; nothing previously fed it the shape FIX-D creates.
    SurfaceDbBuildReport report;
    report.coverage = *cov;
    EXPECT_FALSE(is_total_fit_failure(report))
        << "run " << run << ": a converged carry resume must not exit as TOTAL FIT FAILURE";
  }

  // The carried surfaces are still there and still load.
  for (const char *symbol : {"AAA", "BBB"}) {
    EXPECT_TRUE(db->load_surface(kDate0, symbol).has_value()) << symbol;
  }
  std::filesystem::remove_all(root);
}

// The safety gate for the whole change: a carried record is byte-identical to the
// one it replaced. Compares the stored bytes of the record extent itself, so a
// dropped payload field cannot hide behind a recomputed CRC or a summary field.
TEST(SurfaceDbPopulate, CarriedRecordBytesAreIdenticalToWhatTheyReplaced) {
  const auto root = test_root("carry_bytes");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u);

  const std::vector<std::byte> aaa_before = stored_record_bytes(root, kDate0, "AAA");
  const std::vector<std::byte> bbb_before = stored_record_bytes(root, kDate0, "BBB");
  ASSERT_FALSE(aaa_before.empty());
  ASSERT_FALSE(bbb_before.empty());

  // A new symbol that genuinely FITS forces the whole-partition rewrite, so the
  // records physically move within the file (the directory grows) -- which is
  // exactly why this compares extracted extents rather than file offsets.
  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "CCC", 80.0, 0.30));
  auto cov2 = populate_universe_streaming(*db, full, carry_spec());
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());
  ASSERT_EQ(cov2->cells_carried, 2u) << "the carry must actually have engaged";
  ASSERT_EQ(cov2->cells_ok, 1u) << "CCC must have fit, so the partition really was rewritten";

  const std::vector<std::byte> aaa_after = stored_record_bytes(root, kDate0, "AAA");
  const std::vector<std::byte> bbb_after = stored_record_bytes(root, kDate0, "BBB");
  ASSERT_EQ(aaa_before.size(), aaa_after.size());
  ASSERT_EQ(bbb_before.size(), bbb_after.size());
  EXPECT_EQ(0, std::memcmp(aaa_before.data(), aaa_after.data(), aaa_before.size()))
      << "carried record for AAA is not byte-identical to the one it replaced";
  EXPECT_EQ(0, std::memcmp(bbb_before.data(), bbb_after.data(), bbb_before.size()))
      << "carried record for BBB is not byte-identical to the one it replaced";

  std::filesystem::remove_all(root);
}

// A changed fit config must INVALIDATE the carry. The predicate is deliberately
// whole-partition: changing one symbol's config re-fits every cell on the date,
// because a partition is rewritten whole anyway and per-cell granularity would
// let one symbol's change hide another's staleness in the same file.
TEST(SurfaceDbPopulate, ChangedSymbolConfigInvalidatesTheCarryAndForcesRefit) {
  const auto root = test_root("carry_cfg_change");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u);
  const std::uint64_t fp_before = db->partition_config_fingerprint(kDate0);
  ASSERT_NE(fp_before, 0u) << "a partition written by this code must carry a fingerprint";

  // Change exactly ONE field of ONE symbol's config -- the fingerprint must move.
  auto aaa_cfg = db->symbol_config("AAA");
  ASSERT_TRUE(aaa_cfg.has_value());
  SymbolFitConfig changed = *aaa_cfg;
  changed.band_k += 0.25;
  ASSERT_TRUE(db->upsert_symbol("AAA", changed).has_value());

  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "CCC", 80.0, 0.30));
  auto cov2 = populate_universe_streaming(*db, full, carry_spec());
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());
  EXPECT_EQ(cov2->cells_carried, 0u) << "a changed config must not be carried over";
  EXPECT_EQ(cov2->cells_refit, 2u) << "both present cells must be re-fit, not just the changed one";
  EXPECT_EQ(cov2->cells_to_fit, 1u);

  std::filesystem::remove_all(root);
}

// FIX-D fix-2 (I-3). The fail-closed chain must reach the frame that runs the
// GATE, not stop one frame short of it.
//
// `populate_surface_db` used to stamp `FitterProduced` unconditionally, on the
// strength of a comment asserting that every carried item had already passed a
// fingerprint check. That check is real but it lives in
// `populate_universe_streaming`, and `SurfaceDbPopulateConfig::carry_over` says in
// as many words that the struct carries no predicate -- so a direct caller who
// filled `carry_over` itself got its stored surfaces re-emitted verbatim AND
// stamped with a current-config fingerprint, making the staleness STICKY (blessed
// again by every later resume) instead of one-shot.
//
// The claim now travels with the decision: `cfg.attest` defaults to None (no
// stamp -> fold 0 -> UNKNOWN -> never carried), and only a caller that can vouch
// for the whole write sets it.
TEST(SurfaceDbPopulate, AttestationTravelsWithTheCarryDecisionAndDefaultsClosed) {
  const auto root = test_root("attest_chain_direct");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  // Both symbols configured, so the fold over them is well-defined and non-zero.
  // (A symbol absent from the manifest collapses the fold to 0 for reasons that
  // have nothing to do with attesting, which would make this test vacuous.)
  for (const char *sym : {"AAA", "BBB"}) {
    ASSERT_TRUE(db->upsert_symbol(sym, symbol_config_from_preset(FitPreset::Fast)).has_value())
        << sym;
  }

  SurfaceDbPopulateConfig direct;
  direct.fallback = symbol_config_from_preset(FitPreset::Fast);
  direct.skip_existing = false; // rewrite the same date below

  // DEFAULT: no claim, no stamp. A direct caller of this function cannot vouch for
  // a carry set it may have supplied, so the write is not blessed for reuse.
  const auto first = populate_surface_db(*db, carry_seed_boards(), direct);
  ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
  ASSERT_EQ(first->n_ok, 2u);
  EXPECT_EQ(db->partition_config_fingerprint(kDate0), 0u)
      << "an unattested populate must not bless its partition for carry-over";

  // The identical call with the caller making the claim stamps exactly the fold
  // over the symbols written -- which is what a resume compares against.
  SurfaceDbPopulateConfig attested = direct;
  attested.attest = DbConfigAttestation::FitterProduced;
  const auto second = populate_surface_db(*db, carry_seed_boards(), attested);
  ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().to_string());
  ASSERT_EQ(second->n_ok, 2u);
  const std::vector<std::string> written{"AAA", "BBB"};
  const std::uint64_t stamped = db->partition_config_fingerprint(kDate0);
  EXPECT_NE(stamped, 0u) << "an attesting caller must be stamped";
  EXPECT_EQ(stamped, db->config_fingerprint(written));

  std::filesystem::remove_all(root);
}

// The other end of the same chain: the frame that RUNS the gate (`carry_valid`)
// is the frame that attests, so a streaming populate is stamped without its
// caller doing anything -- and the carry it enables on the next run still works.
// Without this, moving the attestation onto the config would have silently turned
// carry-over off everywhere.
TEST(SurfaceDbPopulate, StreamingPopulateAttestsBecauseItRanTheGate) {
  const auto root = test_root("attest_chain_streaming");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u);
  EXPECT_NE(db->partition_config_fingerprint(kDate0), 0u)
      << "the driver that ran the carry gate must stamp what it wrote";

  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "CCC", 80.0, 0.30));
  auto cov2 = populate_universe_streaming(*db, full, carry_spec());
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());
  EXPECT_EQ(cov2->cells_carried, 2u) << "the stamp must still enable the carry it exists for";
  EXPECT_EQ(cov2->cells_refit, 0u);

  std::filesystem::remove_all(root);
}

// The backward-compatibility default, tested rather than asserted: a partition
// written before the fingerprint existed stores 0, and 0 means UNKNOWN, and
// unknown must NEVER carry. This is what makes the first resume of the existing
// production database re-fit once (and only once) instead of silently reusing
// surfaces whose provenance cannot be established.
TEST(SurfaceDbPopulate, ZeroConfigFingerprintNeverCarries) {
  const auto root = test_root("carry_zero_fp");
  {
    auto db = SurfaceDb::create(root.string());
    ASSERT_TRUE(db.has_value());
    auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
    ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
    ASSERT_EQ(cov1->cells_ok, 2u);
    ASSERT_NE(db->partition_config_fingerprint(kDate0), 0u);

    // Rewrite the manifest exactly as a pre-FIX-D writer would have: same symbols,
    // same partitions, fingerprint field left 0.
    const std::vector<std::string> names = db->symbols();
    std::vector<SymbolFitConfig> cfgs;
    std::vector<std::optional<SurfaceProvenance>> provs;
    for (const std::string &sym : names) {
      auto c = db->symbol_config(sym);
      ASSERT_TRUE(c.has_value()) << sym;
      cfgs.push_back(*c);
      auto p = db->surface_provenance(sym);
      ASSERT_TRUE(p.has_value()) << sym;
      provs.push_back(*p);
    }
    std::vector<DbSymbolEntry> entries;
    for (std::size_t i = 0; i < names.size(); ++i) {
      entries.push_back(DbSymbolEntry{names[i], cfgs[i], provs[i]});
    }
    std::vector<DbPartitionInfo> parts = db->partitions();
    for (DbPartitionInfo &p : parts) {
      p.config_fingerprint = 0u; // the pre-FIX-D on-disk state
    }
    SurfaceDbManifestWriteOpts opts;
    opts.generation = db->generation() + 1u;
    auto bytes = write_db_manifest(entries, parts, opts);
    ASSERT_TRUE(bytes.has_value()) << (bytes ? "" : bytes.error().to_string());
    std::ofstream os(root / std::string(kSurfaceDbManifestName), std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(os.good());
    os.write(reinterpret_cast<const char *>(bytes->data()),
             static_cast<std::streamsize>(bytes->size()));
    ASSERT_TRUE(os.good());
  }

  auto db = SurfaceDb::open(root.string());
  ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
  ASSERT_EQ(db->partition_config_fingerprint(kDate0), 0u)
      << "the pre-FIX-D manifest simulation did not take effect";

  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "CCC", 80.0, 0.30));
  auto cov = populate_universe_streaming(*db, full, carry_spec());
  ASSERT_TRUE(cov.has_value()) << (cov ? "" : cov.error().to_string());
  EXPECT_EQ(cov->cells_carried, 0u) << "an unknown (0) fingerprint must never carry";
  EXPECT_EQ(cov->cells_refit, 2u);

  std::filesystem::remove_all(root);
}

// Determinism through the carry path: the stored bytes must not depend on the
// worker count. The carried records are written by the single drain thread while
// the newly-fitted one comes off the shared pool, so this covers the interleaving
// the change actually introduces.
TEST(SurfaceDbPopulate, CarryOverIsByteIdenticalAcrossWorkerCounts) {
  const auto serial_root = test_root("carry_det_serial");
  const auto parallel_root = test_root("carry_det_parallel");

  for (const auto &[root, workers] : std::vector<std::pair<std::filesystem::path, unsigned>>{
           {serial_root, 1u}, {parallel_root, 8u}}) {
    auto db = SurfaceDb::create(root.string());
    ASSERT_TRUE(db.has_value());
    auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec(workers));
    ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
    ASSERT_EQ(cov1->cells_ok, 2u);

    std::vector<CorpusBoard> full = carry_seed_boards();
    full.push_back(make_board(kDate0, "CCC", 80.0, 0.30));
    auto cov2 = populate_universe_streaming(*db, full, carry_spec(workers));
    ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());
    ASSERT_EQ(cov2->cells_carried, 2u) << "workers=" << workers;
    ASSERT_EQ(cov2->cells_ok, 1u) << "workers=" << workers;
  }

  for (const char *symbol : {"AAA", "BBB", "CCC"}) {
    const std::vector<std::byte> a = stored_record_bytes(serial_root, kDate0, symbol);
    const std::vector<std::byte> b = stored_record_bytes(parallel_root, kDate0, symbol);
    ASSERT_FALSE(a.empty()) << symbol;
    ASSERT_EQ(a.size(), b.size()) << symbol;
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size()))
        << "record bytes for " << symbol << " differ between 1 and 8 workers";
  }

  std::filesystem::remove_all(serial_root);
  std::filesystem::remove_all(parallel_root);
}

// ── FIX-E: a present-but-DISABLED symbol survives a rewrite ─────────────────
//
// Shared setup for the four tests below: seed {AAA, BBB} on kDate0, then disable
// BBB in the manifest AFTER it has already fitted and been stored. That is the
// documented operator remedy for a permanently-failing name
// (`docs/surface-db-build.md`), and it is the act that used to arm the deletion.
namespace {

[[nodiscard]] SymbolFitConfig disabled_copy_of(SurfaceDb &db, const char *symbol) {
  const auto stored = db.symbol_config(symbol);
  EXPECT_TRUE(stored.has_value()) << symbol;
  SymbolFitConfig cfg = stored.has_value() ? *stored : symbol_config_from_preset(FitPreset::Fast);
  cfg.enabled = false;
  return cfg;
}

} // namespace

// THE defect gate. A symbol present in a stored partition and DISABLED in the
// current config was silently DELETED when its date was rewritten -- and the
// rewrite was triggered by something entirely unrelated (any new enabled symbol
// arriving on that date).
//
// Why the would-drop guard could not save it: the disabled cell was counted into
// `present` at the top of the filter loop BEFORE `enabled` was consulted, so
// `present == part->count()` and `present < part->count()` was false. The one
// guard that exists precisely to refuse a rewrite that drops a stored surface was
// structurally blind to this case.
//
// This test is also the Step-2 gate: disabling BBB CHANGES the manifest config
// fold, so the stored partition fingerprint no longer matches and `carry_valid`
// is FALSE on this run (asserted below). If the disabled carry were gated on
// `carry_valid` -- the obvious way to write this fix -- BBB would still be
// deleted here, and on the first resume of every pre-FIX-D database, which stores
// a 0 (unknown) fingerprint. The alternative to carrying a disabled cell is not
// re-fitting it, it is deleting it, so the fit-config predicate does not apply.
TEST(SurfaceDbPopulate, DisabledSymbolStoredSurfaceSurvivesARewrite) {
  const auto root = test_root("disabled_survives_rewrite");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u) << "the seed boards must fit for this test to mean anything";
  ASSERT_TRUE(db->load_surface(kDate0, "BBB").has_value()) << "BBB must be STORED before it is "
                                                              "disabled, or nothing is at risk";

  ASSERT_TRUE(db->upsert_symbol("BBB", disabled_copy_of(*db, "BBB")).has_value());

  // The unrelated trigger: a NEW, enabled, genuinely fittable symbol on the same
  // date. Nothing about it concerns BBB.
  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "CCC", 80.0, 0.30));
  auto cov2 = populate_universe_streaming(*db, full, carry_spec());
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());

  // The rewrite really happened -- without this the test could pass vacuously
  // because the date was skipped.
  ASSERT_EQ(cov2->dates_written, 1u) << "the date must actually have been rewritten";
  ASSERT_EQ(cov2->dates_skipped_would_drop, 0u) << "the date must not have been skipped";
  // CCC (new) plus AAA (re-fit, because disabling BBB moved the config fold).
  ASSERT_EQ(cov2->cells_ok, 2u) << "CCC must have fit, so the partition really was replaced";

  // Step 2, asserted rather than assumed: disabling BBB moved the config fold, so
  // the enabled carry path is OFF on this run. BBB's survival cannot depend on it.
  ASSERT_EQ(cov2->cells_carried, 0u)
      << "disabling BBB must have invalidated the fingerprint; if this is nonzero the test no "
         "longer exercises the carry_valid == false path it exists to cover";
  EXPECT_EQ(cov2->cells_refit, 1u) << "AAA is re-fit (invalidated fingerprint); BBB is not";
  EXPECT_EQ(cov2->cells_carried_disabled, 1u) << "BBB must be counted as PRESERVED, not refit";

  // ── The bite ──────────────────────────────────────────────────────────────
  EXPECT_TRUE(db->load_surface(kDate0, "BBB").has_value())
      << "the stored surface of a present-but-disabled symbol was DELETED by an unrelated rewrite";
  // ...and the rewrite still did its job.
  EXPECT_TRUE(db->load_surface(kDate0, "CCC").has_value());
  EXPECT_TRUE(db->load_surface(kDate0, "AAA").has_value());

  // Preserving is not re-enabling: BBB was still not FITTED this run.
  const auto bbb = std::find_if(cov2->per_symbol.begin(), cov2->per_symbol.end(),
                                [](const PopulateSymbolStats &s) { return s.symbol == "BBB"; });
  ASSERT_NE(bbb, cov2->per_symbol.end());
  EXPECT_EQ(bbb->n_disabled, 1u) << "a preserved cell is still a DISABLED cell";
  EXPECT_EQ(bbb->n_ok, 0u);
  EXPECT_EQ(bbb->n_carried, 0u) << "a disabled cell must not read as a healthy carried one";

  std::filesystem::remove_all(root);
}

// Preservation must be BYTE-preservation, not mere presence: a re-fit under the
// disabled config (or any lossy re-emit) would satisfy `load_surface` while
// silently changing the stored values. Mirrors
// CarriedRecordBytesAreIdenticalToWhatTheyReplaced for the disabled cell.
TEST(SurfaceDbPopulate, PreservedDisabledRecordBytesAreIdenticalToWhatTheyReplaced) {
  const auto root = test_root("disabled_bytes");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u);

  const std::vector<std::byte> bbb_before = stored_record_bytes(root, kDate0, "BBB");
  ASSERT_FALSE(bbb_before.empty());

  ASSERT_TRUE(db->upsert_symbol("BBB", disabled_copy_of(*db, "BBB")).has_value());
  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "CCC", 80.0, 0.30));
  auto cov2 = populate_universe_streaming(*db, full, carry_spec());
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());
  ASSERT_EQ(cov2->cells_carried_disabled, 1u);
  ASSERT_EQ(cov2->cells_ok, 2u) << "the partition really was rewritten (AAA re-fit + CCC new)";

  const std::vector<std::byte> bbb_after = stored_record_bytes(root, kDate0, "BBB");
  ASSERT_EQ(bbb_before.size(), bbb_after.size());
  EXPECT_EQ(0, std::memcmp(bbb_before.data(), bbb_after.data(), bbb_before.size()))
      << "the preserved record for the disabled symbol BBB is not byte-identical";

  std::filesystem::remove_all(root);
}

// Step 2's other route, and the population most likely to hit this defect: a
// database written BEFORE the carry-over fingerprint existed stores 0, which means
// UNKNOWN, which never carries. `ZeroConfigFingerprintNeverCarries` pins that the
// healthy cells are re-fit here. The disabled cell must STILL survive -- it is not
// being reused as this run's fitted output, so the fit-config predicate has no say
// over it. Gating the disabled carry on `carry_valid` leaves the bug fully alive
// on exactly this database, which is the fix-shaped no-op this test exists to
// catch.
TEST(SurfaceDbPopulate, PreFixDDatabaseStillPreservesADisabledSymbolsSurface) {
  const auto root = test_root("disabled_zero_fp");
  {
    auto db = SurfaceDb::create(root.string());
    ASSERT_TRUE(db.has_value());
    auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
    ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
    ASSERT_EQ(cov1->cells_ok, 2u);
    ASSERT_TRUE(db->upsert_symbol("BBB", disabled_copy_of(*db, "BBB")).has_value());

    // Rewrite the manifest exactly as a pre-FIX-D writer would have: same symbols
    // (BBB already disabled), same partitions, fingerprint field left 0.
    const std::vector<std::string> names = db->symbols();
    std::vector<DbSymbolEntry> entries;
    std::vector<SymbolFitConfig> cfgs;
    std::vector<std::optional<SurfaceProvenance>> provs;
    cfgs.reserve(names.size());
    provs.reserve(names.size());
    for (const std::string &sym : names) {
      auto c = db->symbol_config(sym);
      ASSERT_TRUE(c.has_value()) << sym;
      cfgs.push_back(*c);
      auto p = db->surface_provenance(sym);
      ASSERT_TRUE(p.has_value()) << sym;
      provs.push_back(*p);
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
      entries.push_back(DbSymbolEntry{names[i], cfgs[i], provs[i]});
    }
    std::vector<DbPartitionInfo> parts = db->partitions();
    for (DbPartitionInfo &p : parts) {
      p.config_fingerprint = 0u; // the pre-FIX-D on-disk state
    }
    SurfaceDbManifestWriteOpts opts;
    opts.generation = db->generation() + 1u;
    auto bytes = write_db_manifest(entries, parts, opts);
    ASSERT_TRUE(bytes.has_value()) << (bytes ? "" : bytes.error().to_string());
    std::ofstream os(root / std::string(kSurfaceDbManifestName), std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(os.good());
    os.write(reinterpret_cast<const char *>(bytes->data()),
             static_cast<std::streamsize>(bytes->size()));
    ASSERT_TRUE(os.good());
  }

  auto db = SurfaceDb::open(root.string());
  ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
  ASSERT_EQ(db->partition_config_fingerprint(kDate0), 0u)
      << "the pre-FIX-D manifest simulation did not take effect";

  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "CCC", 80.0, 0.30));
  auto cov = populate_universe_streaming(*db, full, carry_spec());
  ASSERT_TRUE(cov.has_value()) << (cov ? "" : cov.error().to_string());
  ASSERT_EQ(cov->dates_written, 1u);
  EXPECT_EQ(cov->cells_carried, 0u) << "an unknown (0) fingerprint must never carry a FITTED cell";
  EXPECT_EQ(cov->cells_refit, 1u) << "AAA is re-fit; the disabled BBB is not";
  EXPECT_EQ(cov->cells_carried_disabled, 1u);
  EXPECT_TRUE(db->load_surface(kDate0, "BBB").has_value())
      << "a disabled symbol was deleted on the first resume of a pre-FIX-D database";

  std::filesystem::remove_all(root);
}

// Convergence: preserving must be a FIXED POINT, not a one-shot rescue. DDD fails
// permanently (there is no persisted known-failed state, by design), so kDate0 is
// rewritten on EVERY run forever -- the shape that turns a one-run bug into
// unbounded drift. Across three consecutive rewrites the disabled surface must
// stay present, stay byte-identical, and the counters must not move.
TEST(SurfaceDbPopulate, PreservedDisabledSymbolSurvivesRepeatedRewrites) {
  const auto root = test_root("disabled_converges");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u);
  const std::vector<std::byte> bbb_seed = stored_record_bytes(root, kDate0, "BBB");
  ASSERT_FALSE(bbb_seed.empty());

  ASSERT_TRUE(db->upsert_symbol("BBB", disabled_copy_of(*db, "BBB")).has_value());
  // A PERMANENTLY failing cell keeps this date in the rewrite set forever.
  ASSERT_TRUE(db->upsert_symbol("DDD", rejected_risk_config()).has_value());
  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "DDD", 80.0, 0.30));

  for (int run = 1; run <= 3; ++run) {
    auto cov = populate_universe_streaming(*db, full, carry_spec());
    ASSERT_TRUE(cov.has_value()) << "run " << run << ": " << (cov ? "" : cov.error().to_string());
    EXPECT_EQ(cov->dates_written, 1u) << "run " << run << ": the failing cell keeps this date hot";
    EXPECT_EQ(cov->cells_to_fit, 1u) << "run " << run;
    EXPECT_EQ(cov->cells_failed, 1u) << "run " << run;
    EXPECT_EQ(cov->cells_carried_disabled, 1u) << "run " << run << ": BBB preserved, every run";
    // Run 1 re-fits AAA (disabling BBB moved the fold); runs 2+ carry it, because
    // run 1 re-stamped the fingerprint. Either way BBB is untouched by that
    // decision -- which is the whole point.
    EXPECT_EQ(cov->cells_carried, run == 1 ? 0u : 1u) << "run " << run;
    EXPECT_EQ(cov->cells_refit, run == 1 ? 1u : 0u) << "run " << run;

    ASSERT_TRUE(db->load_surface(kDate0, "BBB").has_value()) << "run " << run;
    const std::vector<std::byte> bbb_now = stored_record_bytes(root, kDate0, "BBB");
    ASSERT_EQ(bbb_seed.size(), bbb_now.size()) << "run " << run;
    EXPECT_EQ(0, std::memcmp(bbb_seed.data(), bbb_now.data(), bbb_seed.size()))
        << "run " << run << ": the preserved record drifted across rewrites";
  }

  std::filesystem::remove_all(root);
}

// The safety guard this defect defeated, tested DIRECTLY for the first time.
// `dates_skipped_would_drop` had ZERO coverage anywhere in the repo: the counter
// that refuses a rewrite which would drop a stored surface had never been
// exercised in either direction.
//
// This is the RESIDUAL case FIX-E deliberately does not change: BBB is present in
// the partition but absent from this run's LOADED BOARDS entirely, so it is in
// neither `present` nor any carry list, and the only thing that can save it is
// `present < part->count()`. That path must keep working after the fix -- a fix
// that widened the carry set must not also have widened `present`.
TEST(SurfaceDbPopulate, NarrowerRerunSkipsTheDateRatherThanDroppingAnAbsentSymbol) {
  const auto root = test_root("would_drop_guard");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u);

  // A NARROWER re-run: BBB is not loaded at all this time, and CCC is new. A
  // whole-partition rewrite from these boards alone would drop BBB.
  const std::vector<CorpusBoard> narrower = {make_board(kDate0, "AAA", 100.0, 0.28),
                                             make_board(kDate0, "CCC", 80.0, 0.30)};
  auto cov2 = populate_universe_streaming(*db, narrower, carry_spec());
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());

  EXPECT_EQ(cov2->dates_skipped_would_drop, 1u) << "the guard must have refused the rewrite";
  EXPECT_EQ(cov2->dates_written, 0u) << "nothing may be written when the guard fires";
  EXPECT_EQ(cov2->cells_already_present, 1u) << "only AAA was both loaded and present";
  EXPECT_EQ(cov2->cells_carried, 0u);
  EXPECT_EQ(cov2->cells_carried_disabled, 0u);

  // Both stored surfaces survive, including the one this run never mentioned.
  EXPECT_TRUE(db->load_surface(kDate0, "AAA").has_value());
  EXPECT_TRUE(db->load_surface(kDate0, "BBB").has_value())
      << "the whole point of the guard: a symbol absent from the loaded set is not dropped";
  // ...and the price of that safety, stated: the new symbol was NOT added.
  EXPECT_FALSE(db->load_surface(kDate0, "CCC").has_value())
      << "a skipped date adds nothing; that is the documented cost of the guard";

  std::filesystem::remove_all(root);
}

// ── FIX-F (FIX-E review I-2): the last member of the data-loss family ────────
//
// This test PINS CURRENT BEHAVIOUR, INCLUDING A REAL DEFECT. It is not an
// aspiration and it must not be "made to pass" by preserving the bytes without
// first reading the argument below (and `.superpowers/sdd/surface-db-prod/
// fixF-report.md`). Two facts are asserted, and the second is why the obvious
// fix is REFUSED rather than merely unwritten.
//
// FACT 1 (the defect, legs 1-2). A cell that fitted ONCE and later DEGRADES
// loses its stored surface. AAA fits and is stored; its config then changes so
// the fit fails; an unrelated new symbol on the same date forces the rewrite.
// `items` is appended only for `CorpusFitStatus::Ok`, `write_partition` replaces
// the whole file, and the would-drop guard cannot fire because AAA was counted
// into `present` before its fit outcome was known — the identical structural
// cause FIX-E repaired for the DISABLED case. Note the population: not the cells
// that always failed (those were never stored, so nothing is at risk), but the
// ones that worked and stopped.
//
// FACT 2 (why preserving the bytes is not a local fix, legs 3-5). PRESENCE, not
// the config fingerprint, is what keeps a date in the rewrite set:
// `populate_universe_streaming` counts a cell into `to_add` only when it is NOT
// in the partition, and a date with `to_add == 0` is `dates_skipped_complete` —
// never rewritten, so its cells are never offered to the fitter again. Leg 3
// shows the failing cell being retried and re-reported EXACTLY BECAUSE it is
// absent; legs 4-5 show that the moment the cell is present the date leaves the
// rewrite set for good. Preserving a failed cell's bytes therefore does not just
// risk blessing them via the fingerprint (the attestation could be withheld) — it
// removes the cell from the retry loop on ANY attestation, silently freezing a
// surface the fitter has just rejected and retiring the failure from every future
// report. That trades a one-shot data loss for permanent silent staleness, which
// is the worse of the two, and it contradicts the "a failing cell is retried
// forever / there is no persisted known-failed state" contract that
// `is_total_fit_failure`, `is_carry_masked_fit_failure` and `build_surface_db`
// all document and depend on. Closing it needs PERSISTED per-cell state saying
// "these bytes are a preserved failure: re-attempt this cell, never carry it" —
// a manifest/archive format change, specified in the report.
//
// ── REV-R3 UPDATE (review C-02/F-02): THE DEFAULT PATH NO LONGER DOES THIS ───
//
// Everything above still holds and every assertion below is unchanged, but the
// behaviour it pins is now reachable ONLY when the operator explicitly asks for
// it. By default `populate_surface_db` REFUSES to commit a partition whose symbol
// set is not a superset of the stored one, so leg 2 below leaves the date
// untouched instead of destroying AAA — see
// `RefusedRewriteLeavesTheDegradedCellsStoredSurfaceByteIdentical`, the
// regression test for the 95-surface incident.
//
// So leg 2 runs with `allow_coverage_regression = true`: the destructive
// behaviour, on demand, for a run that intends retirement. That is exactly what
// this test has always pinned — a whole-file rewrite assembled from what the run
// has — and pinning it is still worth doing, because the retirement path must
// keep working and FACT 2 is still the reason the bytes are not simply preserved.
// The OTHER legs deliberately keep the default spec: once AAA is absent, the
// candidate stops losing anything and the guard is silent, which is the converged
// steady state and is asserted here for free.
TEST(SurfaceDbPopulate, DegradedCellLosesItsStoredSurfaceAndPresenceIsWhatDrivesTheRetry) {
  const auto root = test_root("degraded_cell_loss");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  // ── Leg 1: AAA and BBB fit and are stored. AAA is now a REAL asset. ────────
  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u) << "the seed boards must fit or nothing is at risk";
  ASSERT_TRUE(db->load_surface(kDate0, "AAA").has_value())
      << "AAA must be STORED before it degrades, or this tests the wrong population";
  const auto aaa_healthy = db->symbol_config("AAA");
  ASSERT_TRUE(aaa_healthy.has_value());

  // ── Leg 2: AAA degrades, an unrelated new symbol rewrites the date ────────
  // The config change is one way in; a thinned board or a fitter regression are
  // the others. All three land on the same branch: a present, ENABLED cell whose
  // re-fit fails contributes nothing to `items`.
  ASSERT_TRUE(db->upsert_symbol("AAA", rejected_risk_config()).has_value());
  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "CCC", 80.0, 0.30)); // new, enabled, fits
  // REV-R3: the destructive rewrite is now opt-in. Without this the write path
  // refuses the date and AAA survives -- which is the whole point of the guard,
  // and is asserted directly by the REV-R3 tests below.
  UniversePopulateSpec retiring = carry_spec();
  retiring.allow_coverage_regression = true;
  auto cov2 = populate_universe_streaming(*db, full, retiring);
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());

  // REV-R3: the destruction is now DETECTED and RECORDED even when it is allowed.
  // The incident this guard exists to prevent was invisible precisely because
  // nothing counted or named what a run removed.
  EXPECT_EQ(cov2->dates_refused_coverage_regression, 0u) << "the opt-out was given";
  EXPECT_EQ(cov2->dates_dropped_coverage_regression, 1u);
  ASSERT_EQ(cov2->coverage_regression_cells.size(), std::size_t{1});
  EXPECT_EQ(cov2->coverage_regression_cells[0].date, kDate0);
  EXPECT_EQ(cov2->coverage_regression_cells[0].symbol, "AAA");

  ASSERT_EQ(cov2->dates_written, 1u) << "the date must really have been rewritten";
  ASSERT_EQ(cov2->dates_skipped_would_drop, 0u)
      << "the guard must NOT have fired: AAA was counted into `present` before its fit ran, "
         "which is exactly why it cannot protect this cell";
  ASSERT_EQ(cov2->cells_carried, 0u) << "changing AAA's config moved the fold, so nothing carries";
  EXPECT_EQ(cov2->cells_refit, 2u) << "AAA and BBB were both dragged back through the fitter";
  EXPECT_EQ(cov2->cells_ok, 2u) << "BBB re-fit and CCC fit";
  EXPECT_EQ(cov2->cells_failed, 1u);

  // The failure is reported with the fitter's own reason (069669e's channel).
  ASSERT_EQ(cov2->failed_cells.size(), std::size_t{1});
  EXPECT_EQ(cov2->failed_cells[0].symbol, "AAA");
  EXPECT_EQ(cov2->failed_cells[0].code, ErrorCode::InvalidArgument);
  EXPECT_EQ(cov2->failed_cells[0].detail, "invalid correctness policy for requested risk surface");

  // ── THE BITE ──────────────────────────────────────────────────────────────
  // A surface that existed, loaded, and was servable is gone, destroyed by a
  // rewrite triggered by an unrelated symbol.
  EXPECT_FALSE(db->load_surface(kDate0, "AAA").has_value())
      << "CURRENT BEHAVIOUR, and the defect: a degraded cell's stored surface is deleted. If this "
         "now passes, the preserve landed -- re-read FACT 2 above and the FIX-F report before "
         "deleting this assertion";
  EXPECT_TRUE(db->load_surface(kDate0, "BBB").has_value());
  EXPECT_TRUE(db->load_surface(kDate0, "CCC").has_value());

  // ── Leg 3: the retry loop, and what powers it ─────────────────────────────
  // Same boards, same configs. AAA is retried and re-reported ONLY because it is
  // ABSENT: `to_add` counts cells that are not in the partition. Its healthy
  // siblings are carried, so this is the documented converged steady state.
  auto cov3 = populate_universe_streaming(*db, full, carry_spec());
  ASSERT_TRUE(cov3.has_value()) << (cov3 ? "" : cov3.error().to_string());
  EXPECT_EQ(cov3->cells_to_fit, 1u) << "the absent failing cell is what keeps the date pending";
  EXPECT_EQ(cov3->dates_written, 1u);
  EXPECT_EQ(cov3->cells_carried, 2u) << "BBB and CCC are carried, not re-fit";
  EXPECT_EQ(cov3->cells_refit, 0u);
  EXPECT_EQ(cov3->cells_ok, 0u);
  EXPECT_EQ(cov3->cells_failed, 1u) << "and the failure is REPORTED AGAIN -- every run, forever";
  ASSERT_EQ(cov3->failed_cells.size(), std::size_t{1});
  EXPECT_EQ(cov3->failed_cells[0].symbol, "AAA");
  {
    // The shape a74eb92's warning exists to name: nothing fitted, something
    // failed, something carried. Exit stays 0; the warning does the talking.
    SurfaceDbBuildReport report;
    report.coverage = *cov3;
    EXPECT_FALSE(is_total_fit_failure(report));
    EXPECT_TRUE(is_carry_masked_fit_failure(report));
  }

  // ── Legs 4-5: presence ENDS the retry, permanently ────────────────────────
  // Restore AAA to the config it fitted under; it is added back, and from then
  // on the date has nothing to add. This is the state a preserved failed cell
  // would land in -- except the cell would still be failing and its surface
  // would be the one the fitter just rejected, with no run ever revisiting it.
  ASSERT_TRUE(db->upsert_symbol("AAA", *aaa_healthy).has_value());
  auto cov4 = populate_universe_streaming(*db, full, carry_spec());
  ASSERT_TRUE(cov4.has_value()) << (cov4 ? "" : cov4.error().to_string());
  ASSERT_EQ(cov4->cells_ok, 1u) << "AAA must fit again for leg 5 to mean anything";
  ASSERT_TRUE(db->load_surface(kDate0, "AAA").has_value());

  auto cov5 = populate_universe_streaming(*db, full, carry_spec());
  ASSERT_TRUE(cov5.has_value()) << (cov5 ? "" : cov5.error().to_string());
  EXPECT_EQ(cov5->cells_to_fit, 0u) << "a PRESENT cell is never counted as pending work";
  EXPECT_EQ(cov5->dates_skipped_complete, 1u)
      << "presence alone retires the date from the rewrite set -- no fingerprint involved";
  EXPECT_EQ(cov5->dates_written, 0u);
  EXPECT_EQ(cov5->cells_failed, 0u) << "and with the date skipped, no cell on it is ever offered "
                                       "to the fitter again -- which is why preserving a FAILED "
                                       "cell's bytes would silence its failure forever";
  EXPECT_EQ(cov5->cells_already_present, 3u);

  std::filesystem::remove_all(root);
}

// ── REV-R3 (review C-02 / F-02): the write path refuses to destroy a stored
// surface ────────────────────────────────────────────────────────────────────
//
// The measured incident: a partition rewrite is whole-file, so a present, ENABLED
// cell whose re-fit fails is simply not in the new file and is deleted by the
// commit. One production-shaped run at the wrong `--r` destroyed 95 stored
// surfaces this way and the database reported success. Neither existing guard can
// see it coming -- the filter's `present < part->count()` compares COUNTS and the
// cell was counted into `present` before its fit outcome existed, and FIX-E's
// preserve covers only the DISABLED case.
//
// So the question is now asked in the write path, on the real candidate, as a SET
// comparison against the existing partition's own directory.
namespace {

// The whole partition file, byte for byte. `stored_record_bytes` proves ONE
// record survived; this proves the FILE was not touched at all -- no re-pack, no
// re-CRC, no new `created_ts`. A refusal must leave the commit unattempted, not
// produce an equivalent file.
[[nodiscard]] std::vector<std::byte> partition_file_bytes(const std::filesystem::path &root,
                                                          std::string_view key) {
  return read_file_bytes(partition_file(root, key));
}

} // namespace

// TEST 1 -- the regression test for the 95-surface incident. The assertion that
// matters is not a count: it is that the surface which was there is STILL THERE
// and is byte-identical.
TEST(SurfaceDbPopulate, RefusedRewriteLeavesTheDegradedCellsStoredSurfaceByteIdentical) {
  const auto root = test_root("coverage_regression_refused");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  // Leg 1: AAA and BBB fit and are stored. AAA is the asset at risk.
  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u) << "the seed boards must fit or nothing is at risk";
  ASSERT_TRUE(db->load_surface(kDate0, "AAA").has_value());
  const std::vector<std::byte> aaa_before = stored_record_bytes(root, kDate0, "AAA");
  const std::vector<std::byte> bbb_before = stored_record_bytes(root, kDate0, "BBB");
  const std::vector<std::byte> file_before = partition_file_bytes(root, kDate0);
  ASSERT_FALSE(aaa_before.empty());
  ASSERT_FALSE(file_before.empty());

  // Leg 2: AAA degrades and an unrelated NEW symbol forces the rewrite. This is
  // the exact shape of the incident -- the trigger is never the failing cell.
  ASSERT_TRUE(db->upsert_symbol("AAA", rejected_risk_config()).has_value());
  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "CCC", 80.0, 0.30));
  auto cov2 = populate_universe_streaming(*db, full, carry_spec()); // guard ON by default
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());

  EXPECT_EQ(cov2->dates_refused_coverage_regression, 1u)
      << "the write path must have refused this date";
  // REV-R3 fix-2 (review N-3). The NEGATIVE half of the cause discriminator, and
  // the half a test is likelier to get wrong: this refusal is a genuine fit
  // failure on a partition the manifest DOES list, so the CLI must print its
  // --r advice for it. A discriminator that reported "unlisted" here would
  // suppress the one diagnosis that is correct.
  EXPECT_EQ(cov2->dates_refused_partition_unlisted, 0u)
      << "the manifest lists this partition -- this refusal IS the wrong-rate shape";
  EXPECT_EQ(cov2->dates_dropped_coverage_regression, 0u);
  EXPECT_EQ(cov2->dates_written, 0u)
      << "dates_written must report the COMMIT, not the filter's intention";
  EXPECT_EQ(cov2->dates_skipped_would_drop, 0u)
      << "the FILTER's guard still cannot see this -- that is why the write guard exists";
  ASSERT_EQ(cov2->coverage_regression_cells.size(), std::size_t{1});
  EXPECT_EQ(cov2->coverage_regression_cells[0].date, kDate0);
  EXPECT_EQ(cov2->coverage_regression_cells[0].symbol, "AAA");
  // The cell still FAILED and is still reported as a failure; refusing the write
  // is not the same as pretending the fit worked.
  EXPECT_EQ(cov2->cells_failed, 1u);
  ASSERT_EQ(cov2->failed_cells.size(), std::size_t{1});
  EXPECT_EQ(cov2->failed_cells[0].symbol, "AAA");

  // ── THE ASSERTION THIS WHOLE TASK EXISTS FOR ──────────────────────────────
  ASSERT_TRUE(db->load_surface(kDate0, "AAA").has_value())
      << "the degraded cell's stored surface was DESTROYED -- this is the 95-surface incident";
  const std::vector<std::byte> aaa_after = stored_record_bytes(root, kDate0, "AAA");
  ASSERT_EQ(aaa_before.size(), aaa_after.size());
  EXPECT_EQ(0, std::memcmp(aaa_before.data(), aaa_after.data(), aaa_before.size()))
      << "AAA's stored record survived but its bytes changed";
  const std::vector<std::byte> bbb_after = stored_record_bytes(root, kDate0, "BBB");
  ASSERT_EQ(bbb_before.size(), bbb_after.size());
  EXPECT_EQ(0, std::memcmp(bbb_before.data(), bbb_after.data(), bbb_before.size()));
  // Stronger still: the file was never rewritten at all.
  const std::vector<std::byte> file_after = partition_file_bytes(root, kDate0);
  ASSERT_EQ(file_before.size(), file_after.size()) << "the partition file was rewritten";
  EXPECT_EQ(0, std::memcmp(file_before.data(), file_after.data(), file_before.size()))
      << "the partition file was rewritten to equivalent-but-different bytes; a refusal must "
         "leave the commit unattempted";

  // The price of the refusal, stated: the new symbol did NOT land. That is the
  // deal -- one run's new work deferred, versus a stored surface destroyed.
  EXPECT_FALSE(db->load_surface(kDate0, "CCC").has_value())
      << "a refused date adds nothing; that is the documented cost of the guard";

  std::filesystem::remove_all(root);
}

// TEST 2 -- the opt-out really does the destructive thing, and says what it did.
// (The full behavioural pin lives in
// `DegradedCellLosesItsStoredSurfaceAndPresenceIsWhatDrivesTheRetry`, which now
// runs its destructive leg through this flag. This test is the direct A/B against
// TEST 1: same database, same boards, same everything but the flag.)
TEST(SurfaceDbPopulate, AllowCoverageRegressionWritesTheDateAndNamesWhatItDestroyed) {
  const auto root = test_root("coverage_regression_allowed");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u);
  ASSERT_TRUE(db->load_surface(kDate0, "AAA").has_value());

  ASSERT_TRUE(db->upsert_symbol("AAA", rejected_risk_config()).has_value());
  std::vector<CorpusBoard> full = carry_seed_boards();
  full.push_back(make_board(kDate0, "CCC", 80.0, 0.30));
  UniversePopulateSpec retiring = carry_spec();
  retiring.allow_coverage_regression = true;
  auto cov2 = populate_universe_streaming(*db, full, retiring);
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());

  EXPECT_EQ(cov2->dates_refused_coverage_regression, 0u);
  EXPECT_EQ(cov2->dates_dropped_coverage_regression, 1u);
  EXPECT_EQ(cov2->dates_written, 1u);
  // Detection runs on BOTH sides of the opt-out: a retirement run still produces
  // the audit trail the incident had no way to produce.
  ASSERT_EQ(cov2->coverage_regression_cells.size(), std::size_t{1});
  EXPECT_EQ(cov2->coverage_regression_cells[0].date, kDate0);
  EXPECT_EQ(cov2->coverage_regression_cells[0].symbol, "AAA");

  EXPECT_FALSE(db->load_surface(kDate0, "AAA").has_value()) << "the opt-out must really destroy it";
  EXPECT_TRUE(db->load_surface(kDate0, "BBB").has_value());
  EXPECT_TRUE(db->load_surface(kDate0, "CCC").has_value());

  std::filesystem::remove_all(root);
}

// TEST 3 -- the guard must be SILENT on the healthy converged production shape,
// or defaulting it ON would break every real build.
//
// `prod-2026-07` reports 9 permanently ABSENT cells of 867 on every run and is
// completely healthy: those cells never fitted, so nothing was ever stored for
// them, so a candidate that lacks them loses nothing. Their dates are still
// rewritten every run (an absent enabled cell keeps its date pending forever),
// which is exactly why this must be tested over REPEATED runs and not just one.
// DDD here is that population.
TEST(SurfaceDbPopulate, ConvergedRunWithPermanentlyAbsentCellsNeverTriggersTheGuard) {
  const auto root = test_root("coverage_regression_converged");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  // DDD can never fit and is stored disabled-by-nothing: it is simply refused by
  // the fitter, every run, forever. It is never written, so it is never at risk.
  ASSERT_TRUE(db->upsert_symbol("DDD", rejected_risk_config()).has_value());
  std::vector<CorpusBoard> boards = carry_seed_boards();
  boards.push_back(make_board(kDate0, "DDD", 80.0, 0.30));

  std::vector<std::byte> aaa_seed;
  std::vector<std::byte> bbb_seed;
  for (int run = 1; run <= 3; ++run) {
    auto cov = populate_universe_streaming(*db, boards, carry_spec());
    ASSERT_TRUE(cov.has_value()) << "run " << run << ": " << (cov ? "" : cov.error().to_string());
    EXPECT_EQ(cov->dates_refused_coverage_regression, 0u)
        << "run " << run
        << ": the guard fired on a HEALTHY converged run -- defaulting it ON would break "
           "every production build";
    EXPECT_EQ(cov->dates_dropped_coverage_regression, 0u) << "run " << run;
    EXPECT_TRUE(cov->coverage_regression_cells.empty()) << "run " << run;
    EXPECT_EQ(cov->dates_written, 1u)
        << "run " << run << ": the permanently-absent cell keeps this date in the rewrite set";
    EXPECT_EQ(cov->cells_failed, 1u) << "run " << run << ": DDD, every run, forever";

    ASSERT_TRUE(db->load_surface(kDate0, "AAA").has_value()) << "run " << run;
    ASSERT_TRUE(db->load_surface(kDate0, "BBB").has_value()) << "run " << run;
    if (run == 1) {
      aaa_seed = stored_record_bytes(root, kDate0, "AAA");
      bbb_seed = stored_record_bytes(root, kDate0, "BBB");
      ASSERT_FALSE(aaa_seed.empty());
      continue;
    }
    // Run 2 onward carries them, so they must also be byte-stable: a rewrite that
    // the guard permits must still not disturb what it re-emits.
    const std::vector<std::byte> aaa_now = stored_record_bytes(root, kDate0, "AAA");
    const std::vector<std::byte> bbb_now = stored_record_bytes(root, kDate0, "BBB");
    ASSERT_EQ(aaa_seed.size(), aaa_now.size()) << "run " << run;
    EXPECT_EQ(0, std::memcmp(aaa_seed.data(), aaa_now.data(), aaa_seed.size())) << "run " << run;
    ASSERT_EQ(bbb_seed.size(), bbb_now.size()) << "run " << run;
    EXPECT_EQ(0, std::memcmp(bbb_seed.data(), bbb_now.data(), bbb_seed.size())) << "run " << run;
  }

  std::filesystem::remove_all(root);
}

// TEST 4 -- the ordinary incremental run. A candidate that is a strict SUPERSET
// of the stored set writes normally: the guard is a superset test, not an
// equality test, or every growing database would jam.
//
// The seeded symbol `bbb` is lower case ON PURPOSE. The partition's directory
// stores CANONICAL keys, so a guard that compared raw board symbols against
// stored keys would see "BBB" missing from {"AAA","bbb","CCC"} and refuse every
// single rewrite of any date holding a non-canonical symbol.
TEST(SurfaceDbPopulate, AddingCellsWithoutLosingAnyWritesNormally) {
  const auto root = test_root("coverage_regression_grow");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  const std::vector<CorpusBoard> seed = {make_board(kDate0, "AAA", 100.0, 0.28),
                                         make_board(kDate0, "bbb", 60.0, 0.34)};
  auto cov1 = populate_universe_streaming(*db, seed, carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u);
  ASSERT_EQ(cov1->dates_refused_coverage_regression, 0u)
      << "a FIRST write has no existing partition and can lose nothing";

  std::vector<CorpusBoard> grown = seed;
  grown.push_back(make_board(kDate0, "CCC", 80.0, 0.30));
  auto cov2 = populate_universe_streaming(*db, grown, carry_spec());
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());

  EXPECT_EQ(cov2->dates_refused_coverage_regression, 0u)
      << "a strict superset must write; a lower-case stored symbol must not read as dropped";
  EXPECT_EQ(cov2->dates_dropped_coverage_regression, 0u);
  EXPECT_TRUE(cov2->coverage_regression_cells.empty());
  EXPECT_EQ(cov2->dates_written, 1u);
  EXPECT_EQ(cov2->cells_ok, 1u) << "only CCC was fitted; the other two were carried";

  EXPECT_TRUE(db->load_surface(kDate0, "AAA").has_value());
  EXPECT_TRUE(db->load_surface(kDate0, "BBB").has_value()) << "canonical lookup of the seeded bbb";
  EXPECT_TRUE(db->load_surface(kDate0, "CCC").has_value());

  std::filesystem::remove_all(root);
}

// TEST 5 -- a refusal is PER-DATE. The run must keep going, including for dates
// that come AFTER the refused one in the drain's ascending walk, or one bad cell
// would cost the whole window.
TEST(SurfaceDbPopulate, RefusingOneDateDoesNotStopTheOtherDatesInTheSameRun) {
  const auto root = test_root("coverage_regression_per_date");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  // Disjoint symbol sets per date, because a config change is per SYMBOL: this is
  // how one date is made to lose a cell while the other is untouched.
  const std::vector<CorpusBoard> seed = {make_board(kDate0, "AAA", 100.0, 0.28),
                                         make_board(kDate0, "BBB", 60.0, 0.34),
                                         make_board(kDate1, "CCC", 90.0, 0.26),
                                         make_board(kDate1, "DDD", 70.0, 0.31)};
  auto cov1 = populate_universe_streaming(*db, seed, carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 4u);
  const std::vector<std::byte> aaa_before = stored_record_bytes(root, kDate0, "AAA");
  const std::vector<std::byte> d0_file_before = partition_file_bytes(root, kDate0);
  ASSERT_FALSE(aaa_before.empty());

  // AAA degrades (kDate0 only). Both dates get a new symbol so both are rewritten.
  ASSERT_TRUE(db->upsert_symbol("AAA", rejected_risk_config()).has_value());
  std::vector<CorpusBoard> full = seed;
  full.push_back(make_board(kDate0, "EEE", 80.0, 0.30));
  full.push_back(make_board(kDate1, "FFF", 85.0, 0.29));
  auto cov2 = populate_universe_streaming(*db, full, carry_spec());
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());

  EXPECT_EQ(cov2->dates_refused_coverage_regression, 1u);
  EXPECT_EQ(cov2->dates_written, 1u) << "the OTHER date must still have been committed";
  ASSERT_EQ(cov2->coverage_regression_cells.size(), std::size_t{1});
  EXPECT_EQ(cov2->coverage_regression_cells[0].date, kDate0);
  EXPECT_EQ(cov2->coverage_regression_cells[0].symbol, "AAA");

  // kDate0: untouched, down to the file bytes. EEE did not land.
  ASSERT_TRUE(db->load_surface(kDate0, "AAA").has_value());
  const std::vector<std::byte> aaa_after = stored_record_bytes(root, kDate0, "AAA");
  ASSERT_EQ(aaa_before.size(), aaa_after.size());
  EXPECT_EQ(0, std::memcmp(aaa_before.data(), aaa_after.data(), aaa_before.size()));
  const std::vector<std::byte> d0_file_after = partition_file_bytes(root, kDate0);
  ASSERT_EQ(d0_file_before.size(), d0_file_after.size());
  EXPECT_EQ(0, std::memcmp(d0_file_before.data(), d0_file_after.data(), d0_file_before.size()));
  EXPECT_FALSE(db->load_surface(kDate0, "EEE").has_value());

  // kDate1 (which the drain reaches AFTER the refusal): fully built.
  EXPECT_TRUE(db->load_surface(kDate1, "CCC").has_value());
  EXPECT_TRUE(db->load_surface(kDate1, "DDD").has_value());
  EXPECT_TRUE(db->load_surface(kDate1, "FFF").has_value())
      << "a refusal on an EARLIER date stopped a later date from being written";

  std::filesystem::remove_all(root);
}

// TEST 6 -- the reported list is COMPLETE, CORRECT and DETERMINISTICALLY ORDERED,
// and the order does not depend on the worker count.
//
// The repo's standing invariant is byte-identical results for any thread count.
// The list is built on the single drain thread, from a `set_difference` of two
// SORTED ranges (the partition's directory and the candidate item list), so
// neither fit-completion order nor an unordered container can reach it. This test
// is what makes that claim falsifiable: the same scenario is run at 1 and 4
// workers into separate roots and the two lists must match exactly.
TEST(SurfaceDbPopulate, DroppedSymbolListIsCorrectAndOrderedIndependentlyOfWorkerCount) {
  const auto scenario = [](const std::filesystem::path &root,
                           unsigned workers) -> std::vector<CoverageRegressionCell> {
    auto db = SurfaceDb::create(root.string());
    EXPECT_TRUE(db.has_value());
    if (!db.has_value()) {
      return {};
    }
    // Seeded OUT of sorted order so a list that merely echoed insertion order
    // would come back as DDD-then-BBB and fail the ordering assertion.
    const std::vector<CorpusBoard> seed = {
        make_board(kDate0, "DDD", 70.0, 0.31), make_board(kDate0, "CCC", 90.0, 0.26),
        make_board(kDate0, "BBB", 60.0, 0.34), make_board(kDate0, "AAA", 100.0, 0.28)};
    auto cov1 = populate_universe_streaming(*db, seed, carry_spec(workers));
    EXPECT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
    EXPECT_EQ(cov1 ? cov1->cells_ok : 0u, 4u);

    // Two of the four degrade; the other two still fit and are re-emitted.
    EXPECT_TRUE(db->upsert_symbol("BBB", rejected_risk_config()).has_value());
    EXPECT_TRUE(db->upsert_symbol("DDD", rejected_risk_config()).has_value());
    std::vector<CorpusBoard> full = seed;
    full.push_back(make_board(kDate0, "EEE", 80.0, 0.30)); // forces the rewrite
    auto cov2 = populate_universe_streaming(*db, full, carry_spec(workers));
    EXPECT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());
    if (!cov2.has_value()) {
      return {};
    }
    EXPECT_EQ(cov2->dates_refused_coverage_regression, 1u);
    EXPECT_EQ(cov2->dates_written, 0u);
    // Nothing on the date moved, including the two that would have survived.
    EXPECT_TRUE(db->load_surface(kDate0, "AAA").has_value());
    EXPECT_TRUE(db->load_surface(kDate0, "BBB").has_value());
    EXPECT_TRUE(db->load_surface(kDate0, "CCC").has_value());
    EXPECT_TRUE(db->load_surface(kDate0, "DDD").has_value());
    return cov2->coverage_regression_cells;
  };

  const auto serial_root = test_root("coverage_regression_order_w1");
  const auto parallel_root = test_root("coverage_regression_order_w4");
  const std::vector<CoverageRegressionCell> serial = scenario(serial_root, 1u);
  const std::vector<CoverageRegressionCell> parallel = scenario(parallel_root, 4u);

  ASSERT_EQ(serial.size(), std::size_t{2}) << "exactly the two degraded cells are at risk";
  EXPECT_EQ(serial[0].date, kDate0);
  EXPECT_EQ(serial[0].symbol, "BBB");
  EXPECT_EQ(serial[1].date, kDate0);
  EXPECT_EQ(serial[1].symbol, "DDD") << "ascending canonical-symbol order, not seed order";

  ASSERT_EQ(parallel.size(), serial.size()) << "the list size depends on the worker count";
  for (std::size_t i = 0; i < serial.size(); ++i) {
    EXPECT_EQ(parallel[i].date, serial[i].date) << "entry " << i;
    EXPECT_EQ(parallel[i].symbol, serial[i].symbol) << "entry " << i;
  }

  std::filesystem::remove_all(serial_root);
  std::filesystem::remove_all(parallel_root);
}

// ── REV-R3 fix-1 (review I-1): the manifest does not get a vote ──────────────
//
// The guard's comments claimed the existing set was "never inferred from the
// manifest", but only the CONTENTS were. The existence decision went through
// `SurfaceDb::open_partition`, which returns NotFound from the MANIFEST LOOKUP
// before it ever touches the file -- and NotFound is the guard's "no existing
// coverage, proceed" branch. So a partition file holding surfaces but absent from
// the manifest was treated as empty and overwritten: exactly the outcome the
// guard exists to prevent, on exactly the disagreement its comment named.
//
// The window is real. `write_partition` renames the archive first and persists
// the manifest second (surface_db.cpp), so a crash between the two leaves this
// state; so does a manifest restored from an older copy, a hand-assembled root,
// and a `drop_partition` interrupted after its manifest commit. The fix is
// `SurfaceDb::open_partition_file`, which skips the manifest and asks the
// filesystem.
namespace {

// Reproduce the crash window WITHOUT a crash: snapshot the manifest before the
// write, then restore it afterwards. The partition file stays; the manifest
// forgets it. Byte-for-byte the state a crash between rename and persist leaves.
void restore_manifest(const std::filesystem::path &root, const std::vector<std::byte> &saved) {
  const std::filesystem::path p = root / std::string(kSurfaceDbManifestName);
  std::ofstream os(p, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(os.good()) << p.string();
  os.write(reinterpret_cast<const char *>(saved.data()),
           static_cast<std::streamsize>(saved.size()));
  ASSERT_TRUE(os.good()) << p.string();
}

} // namespace

// TEST 7 -- a partition file present on disk but ABSENT from the manifest must be
// READ, not overwritten.
//
// Before the fix this run destroyed AAA and BBB and reported success: the
// manifest said the date did not exist, so the filter scheduled every loaded cell
// as new, the guard's `open_partition` came back NotFound, and the whole-file
// write committed a partition containing only CCC.
TEST(SurfaceDbPopulate, PartitionOnDiskButMissingFromTheManifestIsNotOverwritten) {
  const auto root = test_root("coverage_regression_unlisted_partition");
  std::vector<std::byte> manifest_before;
  std::vector<std::byte> file_before;
  {
    auto db = SurfaceDb::create(root.string());
    ASSERT_TRUE(db.has_value());

    // The manifest as it is BEFORE kDate0 is ever written -- no partition record.
    manifest_before = read_file_bytes(root / std::string(kSurfaceDbManifestName));
    ASSERT_FALSE(manifest_before.empty());

    // Write kDate0 normally: AAA and BBB land, the file exists, the manifest lists it.
    auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
    ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
    ASSERT_EQ(cov1->cells_ok, 2u);
    file_before = partition_file_bytes(root, kDate0);
    ASSERT_FALSE(file_before.empty());
    // Scoped so this handle -- and its S5 partition mapping -- is gone before the
    // manifest is rolled back underneath the root.
  }

  // Roll the manifest back. The .atxvsa is untouched and still holds both cells.
  restore_manifest(root, manifest_before);
  auto reopened = SurfaceDb::open(root.string());
  ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
  ASSERT_FALSE(reopened->open_partition(kDate0).has_value())
      << "the scenario is not armed: the manifest still lists the partition";
  ASSERT_TRUE(reopened->open_partition_file(kDate0).has_value())
      << "the scenario is not armed: the partition FILE must still be on disk";

  // A run over a NARROWER board set for the same date. Nothing here is degraded
  // and nothing fails -- the candidate is simply smaller than what is stored,
  // which is all a whole-file write needs to destroy a surface.
  const std::vector<CorpusBoard> narrower = {make_board(kDate0, "CCC", 90.0, 0.26)};
  auto cov2 = populate_universe_streaming(*reopened, narrower, carry_spec());
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());

  EXPECT_EQ(cov2->dates_refused_coverage_regression, 1u)
      << "an UNLISTED partition file was treated as no coverage and overwritten";
  // REV-R3 fix-2 (review N-3). The POSITIVE half of the cause discriminator. The
  // CLI keys its advice on this counter, and it must be able to tell this state
  // apart from a wrong-rate refusal: here NOTHING failed to fit (cells_failed is
  // 0 below), so "check --r" is the wrong instruction and the escape it offers
  // -- --allow-coverage-regression -- would delete AAA and BBB, the two surfaces
  // this run just saved.
  EXPECT_EQ(cov2->dates_refused_partition_unlisted, 1u)
      << "the refusal was on a partition the manifest does not list";
  EXPECT_EQ(cov2->cells_failed, 0u)
      << "no cell failed here: a wrong-rate diagnosis would be false on its face";
  EXPECT_EQ(cov2->dates_written, 0u);
  ASSERT_EQ(cov2->coverage_regression_cells.size(), std::size_t{2});
  EXPECT_EQ(cov2->coverage_regression_cells[0].symbol, "AAA");
  EXPECT_EQ(cov2->coverage_regression_cells[1].symbol, "BBB")
      << "the existing set came from the FILE's directory, not from the manifest";

  // The bytes are the assertion that matters: the file was not touched at all.
  const std::vector<std::byte> file_after = partition_file_bytes(root, kDate0);
  ASSERT_EQ(file_before.size(), file_after.size()) << "the unlisted partition was rewritten";
  EXPECT_EQ(0, std::memcmp(file_before.data(), file_after.data(), file_before.size()))
      << "the unlisted partition file was rewritten; AAA and BBB are gone";

  std::filesystem::remove_all(root);
}

// TEST 8 -- and the OTHER half of the same branch: documented remedy #1 still
// works. `surface_db.hpp` tells an operator whose database was poisoned by a
// wrong `--r` to DELETE the partition file and re-run, and that instruction must
// keep working -- it is the only repair the tool offers, and the guard sits
// directly on its path. A file-EXISTENCE probe is what separates the two cases:
// remedy #1 has no file, the I-1 window has one.
TEST(SurfaceDbPopulate, DeletingThePartitionFileStillLetsTheDateBeRebuilt) {
  const auto root = test_root("coverage_regression_remedy_delete");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
  ASSERT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());
  ASSERT_EQ(cov1->cells_ok, 2u);
  ASSERT_TRUE(db->load_surface(kDate0, "AAA").has_value());

  // The remedy, verbatim: delete the partition file. The MANIFEST STILL LISTS IT
  // -- that is the state the instruction produces, and the case the fix must not
  // confuse with "a file is there and the manifest forgot it".
  ASSERT_TRUE(std::filesystem::remove(partition_file(root, kDate0)));
  ASSERT_FALSE(db->open_partition_file(kDate0).has_value())
      << "the file must really be gone for this to test the remedy";

  // Re-run over a NARROWER set than what used to be stored. Nothing is on disk to
  // lose, so this must proceed -- refusing here would make the documented repair
  // impossible and leave a poisoned database unrepairable.
  const std::vector<CorpusBoard> narrower = {make_board(kDate0, "AAA", 100.0, 0.28)};
  auto cov2 = populate_universe_streaming(*db, narrower, carry_spec());
  ASSERT_TRUE(cov2.has_value()) << (cov2 ? "" : cov2.error().to_string());
  EXPECT_EQ(cov2->dates_refused_coverage_regression, 0u)
      << "remedy #1 (delete the partition file and re-run) must still work";
  EXPECT_EQ(cov2->dates_dropped_coverage_regression, 0u);
  EXPECT_TRUE(cov2->coverage_regression_cells.empty());
  EXPECT_EQ(cov2->dates_written, 1u);
  EXPECT_TRUE(db->load_surface(kDate0, "AAA").has_value()) << "the date was rebuilt";
  EXPECT_FALSE(db->load_surface(kDate0, "BBB").has_value())
      << "deleting a partition deletes it: BBB is not restored, exactly as documented";

  std::filesystem::remove_all(root);
}

// TEST 9 -- an existing partition file that will not OPEN aborts the build, and
// `--allow-coverage-regression` does NOT waive that (REV-R3 fix-1, review I-3).
//
// Two unrelated safety properties used to ride one flag. "I have read the named
// list and I want those surfaces gone" says nothing about "and please also
// overwrite any partition you could not parse" -- and the flag is whole-RUN, so
// an operator retiring one cell was opted into the second waiver for every date.
TEST(SurfaceDbPopulate, UnreadableExistingPartitionAbortsEvenWithTheOptOut) {
  // true iff the populate returned Ok.
  const auto rewrite_succeeded = [](const std::filesystem::path &root, bool allow) -> bool {
    auto db = SurfaceDb::create(root.string());
    EXPECT_TRUE(db.has_value());
    if (!db.has_value()) {
      return false;
    }
    auto cov1 = populate_universe_streaming(*db, carry_seed_boards(), carry_spec());
    EXPECT_TRUE(cov1.has_value()) << (cov1 ? "" : cov1.error().to_string());

    // Corrupt the partition in place: keep the file, destroy its bytes. It is
    // PRESENT (so not the NotFound path) and unreadable (so its contents cannot
    // be compared), which is the whole point.
    {
      std::ofstream os(partition_file(root, kDate0), std::ios::binary | std::ios::trunc);
      EXPECT_TRUE(os.good());
      const char junk[] = "this is not an ATXVSA2 archive";
      os.write(junk, static_cast<std::streamsize>(sizeof junk));
    }
    EXPECT_FALSE(db->open_partition_file(kDate0).has_value());

    std::vector<CorpusBoard> full = carry_seed_boards();
    full.push_back(make_board(kDate0, "CCC", 90.0, 0.26)); // forces the rewrite
    UniversePopulateSpec spec = carry_spec();
    spec.allow_coverage_regression = allow;
    return populate_universe_streaming(*db, full, spec).has_value();
  };

  const auto guarded_root = test_root("unreadable_partition_guard_on");
  EXPECT_FALSE(rewrite_succeeded(guarded_root, /*allow=*/false))
      << "an unreadable existing partition must abort rather than be overwritten";

  const auto opted_out_root = test_root("unreadable_partition_opt_out");
  EXPECT_FALSE(rewrite_succeeded(opted_out_root, /*allow=*/true))
      << "--allow-coverage-regression authorises destroying a NAMED list; it cannot "
         "authorise overwriting contents nobody could read";

  std::filesystem::remove_all(guarded_root);
  std::filesystem::remove_all(opted_out_root);
}

// ── R1-a (review C-06): a PRE-TASK fit-scheduler failure must TERMINATE the
// populate, never wedge the per-date drain ──────────────────────────────────
//
// `populate_surface_db` initialises `remaining[date]` to a positive count and
// drains each date by sleeping on that counter. The counter is decremented by a
// scope guard INSIDE the fit task, so it only ever moves for a task that actually
// STARTED. `run_bounded_fit_tasks` has two failure returns that happen before any
// task starts -- a background worker-launch failure and a scratch-allocation
// failure (the std::bad_alloc that killed this project's production run twice) --
// and on both, every counter stayed frozen, nothing ever notified, and the main
// thread slept forever without ever reaching the point where it could observe the
// scheduler's Status. The process hung with no output.
namespace {

// The populate call under test runs on its OWN thread and is waited for with a
// BOUNDED timeout, because the defect is a HANG: on a regression the call never
// returns, and a test that simply called it inline would wedge the entire ctest
// run instead of failing. 10s is ~100x these synthetic boards' runtime.
//
// On timeout the thread is deliberately LEAKED rather than joined -- it is stuck
// in the very deadlock under test, so joining would reproduce the hang inside the
// harness. Everything it touches lives in a shared_ptr it holds BY VALUE, so the
// leak cannot dangle after the test body returns.
struct StreamingRun {
  std::optional<SurfaceDb> db;
  std::vector<CorpusBoard> boards;
  UniversePopulateSpec spec;
  PopulateTestHooks hooks;
  std::optional<Result<UniversePopulateCoverage>> cov;
};

[[nodiscard]] bool run_streaming_bounded(const std::shared_ptr<StreamingRun> &run,
                                         std::chrono::seconds timeout) {
  auto finished = std::make_shared<std::promise<void>>();
  std::future<void> done = finished->get_future();
  std::thread([run, finished] {
    run->cov = populate_universe_streaming(*run->db, run->boards, run->spec, &run->hooks);
    finished->set_value();
  }).detach();
  // `done` dies with this frame; the promise outlives it via the captured
  // shared_ptr, and setting a value on a state with no reader is well-defined.
  return done.wait_for(timeout) == std::future_status::ready;
}

constexpr std::chrono::seconds kDrainDeadlockTimeout{10};

} // namespace

// Path 1: the background-worker LAUNCH failure (fit_scheduler.cpp's inner
// catch(...) -> "worker launch failed"). Injected through the full
// populate_universe_streaming path. Asserts three things: the call TERMINATES
// inside the bounded wait, it returns the SCHEDULER'S OWN non-Ok Status rather
// than a fresh code, and the date it could not fit is absent from disk.
TEST(SurfaceDbPopulate, SchedulerWorkerLaunchFailureTerminatesTheDrainInsteadOfHanging) {
  const auto root = test_root("sched_launch_abort");

  auto run = std::make_shared<StreamingRun>();
  {
    auto db = SurfaceDb::create(root.string());
    ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
    run->db.emplace(std::move(*db));
  }
  run->boards = make_boards(); // AAA,BBB x kDate0,kDate1 = 4 enabled fits
  run->spec.index_symbol = "";
  run->spec.preset = FitPreset::Fast;
  run->spec.fit_workers = 4u; // >= 2 so the scheduler launches background workers

  std::atomic<bool> hook_fired{false};
  run->hooks.before_worker_launch = [&hook_fired](std::size_t /*ordinal*/) {
    hook_fired.store(true, std::memory_order_release);
    throw std::runtime_error("injected worker-launch failure");
  };

  ASSERT_TRUE(run_streaming_bounded(run, kDrainDeadlockTimeout))
      << "populate_universe_streaming did not return within " << kDrainDeadlockTimeout.count()
      << "s after a pre-task scheduler failure -- the per-date drain is deadlocked on a "
         "counter no task will ever decrement (review C-06)";
  ASSERT_TRUE(hook_fired.load(std::memory_order_acquire))
      << "the scheduler launched no background worker, so nothing was injected and this "
         "run proves nothing (worker_budget collapsed to 1?)";

  ASSERT_TRUE(run->cov.has_value());
  ASSERT_FALSE(run->cov->has_value()) << "a scheduler that fitted NOTHING must not report success";
  EXPECT_EQ(run->cov->error().code(), ErrorCode::Internal);
  EXPECT_NE(run->cov->error().to_string().find("worker launch failed"), std::string::npos)
      << "the scheduler's own message must be propagated, not replaced: "
      << run->cov->error().to_string();

  // Nothing was fitted, so no partition may exist -- in particular the drain must
  // not have written a partition for a date whose cells never ran.
  run->db.reset();
  auto reopened = SurfaceDb::open(root.string());
  ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
  EXPECT_EQ(reopened->open_partition(kDate0).error().code(), ErrorCode::NotFound);
  EXPECT_EQ(reopened->open_partition(kDate1).error().code(), ErrorCode::NotFound);

  std::filesystem::remove_all(root);
}

// Path 2: the pre-launch SCRATCH-ALLOCATION failure (fit_scheduler.cpp's outer
// catch(...) -> "scheduler setup failed"). This is the std::bad_alloc path the
// production run actually took. A real bad_alloc is not injectable on demand, so
// the throw comes from a hook placed at the top of the same try block; it reaches
// the identical catch and the identical return, with no worker ever created --
// which makes it a STRICTLY earlier pre-task failure than path 1.
TEST(SurfaceDbPopulate, SchedulerSetupFailureTerminatesTheDrainInsteadOfHanging) {
  const auto root = test_root("sched_setup_abort");

  auto run = std::make_shared<StreamingRun>();
  {
    auto db = SurfaceDb::create(root.string());
    ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
    run->db.emplace(std::move(*db));
  }
  run->boards = make_boards();
  run->spec.index_symbol = "";
  run->spec.preset = FitPreset::Fast;
  run->spec.fit_workers = 1u; // even outer-SERIAL must not hang: no worker is launched

  std::atomic<bool> hook_fired{false};
  run->hooks.before_scheduler_setup = [&hook_fired]() {
    hook_fired.store(true, std::memory_order_release);
    throw std::bad_alloc();
  };

  ASSERT_TRUE(run_streaming_bounded(run, kDrainDeadlockTimeout))
      << "populate_universe_streaming did not return within " << kDrainDeadlockTimeout.count()
      << "s after a scheduler setup failure -- the per-date drain is deadlocked (review C-06)";
  ASSERT_TRUE(hook_fired.load(std::memory_order_acquire));

  ASSERT_TRUE(run->cov.has_value());
  ASSERT_FALSE(run->cov->has_value());
  EXPECT_EQ(run->cov->error().code(), ErrorCode::Internal);
  EXPECT_NE(run->cov->error().to_string().find("scheduler setup failed"), std::string::npos)
      << run->cov->error().to_string();

  run->db.reset();
  auto reopened = SurfaceDb::open(root.string());
  ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
  EXPECT_EQ(reopened->open_partition(kDate0).error().code(), ErrorCode::NotFound);

  std::filesystem::remove_all(root);
}

// The durability half of R1-a: a date already fitted and WRITTEN before the
// scheduler failure is still on disk afterwards, and the aborted run neither
// rewrote it partially nor deleted it.
//
// The DATE is the resume unit, so the proof is staged across two runs, which is
// also the only shape a PRE-TASK failure can produce (no task runs, so no date
// can complete inside the failing run itself): run 1 fits kDate0 clean; run 2
// adds a new symbol CCC to kDate0 -- which puts kDate0 back in the rewrite set,
// so its whole partition is due to be rebuilt -- plus a new kDate1, and fails the
// scheduler. Afterwards kDate0 must still serve AAA and BBB from a FRESH open
// (crash-resume: only committed on-disk state), CCC must be absent (never
// fitted), and kDate1 must not exist.
TEST(SurfaceDbPopulate, DatesWrittenBeforeASchedulerFailureStayOnDisk) {
  const auto root = test_root("sched_abort_durability");

  // ── Run 1: kDate0 populated cleanly. ──
  {
    auto db = SurfaceDb::create(root.string());
    ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
    const std::vector<CorpusBoard> first = {make_board(kDate0, "AAA", 100.0, 0.28),
                                            make_board(kDate0, "BBB", 60.0, 0.34)};
    UniversePopulateSpec spec;
    spec.index_symbol = "";
    spec.preset = FitPreset::Fast;
    spec.fit_workers = 2u;
    auto cov = populate_universe_streaming(*db, first, spec);
    ASSERT_TRUE(cov.has_value()) << (cov ? "" : cov.error().to_string());
    ASSERT_EQ(cov->cells_ok, 2u);
    ASSERT_EQ(cov->dates_written, 1u);
  }

  // ── Run 2: kDate0 gains CCC (so the date is rewritten) and kDate1 arrives;
  //    the scheduler dies before either can be fitted. ──
  auto run = std::make_shared<StreamingRun>();
  {
    auto db = SurfaceDb::open(root.string());
    ASSERT_TRUE(db.has_value()) << (db ? "" : db.error().to_string());
    run->db.emplace(std::move(*db));
  }
  run->boards = {make_board(kDate0, "AAA", 100.0, 0.28), make_board(kDate0, "BBB", 60.0, 0.34),
                 make_board(kDate0, "CCC", 80.0, 0.31), make_board(kDate1, "AAA", 101.0, 0.27)};
  run->spec.index_symbol = "";
  run->spec.preset = FitPreset::Fast;
  run->spec.fit_workers = 4u;
  run->hooks.before_worker_launch = [](std::size_t /*ordinal*/) {
    throw std::runtime_error("injected worker-launch failure");
  };

  ASSERT_TRUE(run_streaming_bounded(run, kDrainDeadlockTimeout))
      << "the drain deadlocked (review C-06)";
  ASSERT_TRUE(run->cov.has_value());
  ASSERT_FALSE(run->cov->has_value());

  run->db.reset(); // drop the in-process handle: only on-disk state is asserted below
  auto reopened = SurfaceDb::open(root.string());
  ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
  for (const char *symbol : {"AAA", "BBB"}) {
    EXPECT_TRUE(reopened->load_surface(kDate0, symbol).has_value())
        << kDate0 << "/" << symbol
        << " was lost by an aborted rewrite -- a scheduler failure must not destroy an "
           "already-committed date";
  }
  EXPECT_EQ(reopened->load_surface(kDate0, "CCC").error().code(), ErrorCode::NotFound)
      << "CCC was never fitted, so it must not appear in the partition";
  EXPECT_EQ(reopened->open_partition(kDate1).error().code(), ErrorCode::NotFound);

  std::filesystem::remove_all(root);
}

} // namespace atx::vol
