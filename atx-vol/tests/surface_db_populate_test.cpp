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
#include <cstdint>
#include <cstdio>
#include <filesystem>
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
  EXPECT_EQ(cov_b->cells_refit, 2u);         // AAPL/NVDA re-fit by the same-date rewrite
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

  std::filesystem::remove_all(root);
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
  EXPECT_NE(
      text.find("symbol,n_attempted,n_ok,n_failed,n_disabled,success_rate,mean_oos_in_band\n"),
      std::string::npos)
      << text;
  // AAA: n_attempted=2, n_ok=2, n_failed=0, n_disabled=0 -> success_rate=1;
  // pinned curve -> no OOS score -> "nan" (the NaN-when-unavailable rule).
  EXPECT_NE(text.find("AAA,2,2,0,0,1,nan\n"), std::string::npos) << text;
  // BBB: same counts (a directly-routed, non-ambiguous board also has no
  // selector OOS score even though its curve isn't pinned -- fit_board's
  // `oos_in_band_available` is tied to the selector having run at all, not
  // to pin_curve specifically; mirrors corpus.cpp's CorpusEntry.oos_in_band).
  EXPECT_NE(text.find("BBB,2,2,0,0,1,nan\n"), std::string::npos) << text;

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

    unsigned captured_inner = 0u; // hook fires once on the caller thread
    bool hook_seen = false;
    PopulateTestHooks hooks;
    hooks.on_inner_fit_workers = [&](unsigned inner) {
      captured_inner = inner;
      hook_seen = true;
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
  PopulateTestHooks hooks;
  hooks.on_inner_fit_workers = [&](unsigned inner) { captured_inner = inner; };
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

} // namespace atx::vol
