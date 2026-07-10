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
//   4. manifest    — parse(serialize(m)) == m and write->read round-trips;
//   5. throughput  — a 20-board corpus builds under a generous wall ceiling.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"        // AmericanGreeks
#include "atx/vol/chain.hpp"           // OptionChain
#include "atx/vol/corpus.hpp"          // build_corpus, CorpusManifest, ...
#include "atx/vol/data.hpp"            // iso_to_ns, year_fraction
#include "atx/vol/market_env.hpp"      // MarketEnv
#include "atx/vol/panel.hpp"           // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/priced_surface.hpp"  // PricedSurface
#include "atx/vol/pricer_fitter.hpp"   // PricerFitter, PricerConfig
#include "atx/vol/session.hpp"         // VolaSession::to_priced_surface
#include "atx/vol/spy_fixture.hpp"     // make_spy_synthetic_spec
#include "atx/vol/surface_archive.hpp" // SurfaceArchive
#include "atx/vol/types.hpp"           // Side
#include "atx/vol/vol_curve.hpp"       // CurveConfig, VolCurveKind, to_string
#include "support/bench_gate.hpp"      // ATX_VOL_SKIP_UNLESS_BENCH

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

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
[[nodiscard]] fs::path fresh_out_dir(const char* tag) {
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
[[nodiscard]] SynthPanelSpec make_index_spec(const std::string& uid,
                                             const std::string& snapshot, double spot) {
  SynthPanelSpec s = make_spy_synthetic_spec(snapshot);
  s.uid = uid;
  const double scale = spot / s.spot;
  s.spot = spot;
  for (double& k : s.strikes) {
    k *= scale;
  }
  return s;
}

// A wider-spread, single-name-style board (moderate strike ladder, higher vol,
// wide two-sided markets). On the default policy it auto-selects the eSSVI
// backbone.
[[nodiscard]] SynthPanelSpec make_singlename_spec(const std::string& uid,
                                                  const std::string& snapshot, double spot) {
  SynthPanelSpec s;
  s.uid = uid;
  s.snapshot_iso = snapshot;
  s.spot = spot;
  s.r = 0.043;
  s.borrow = 0.0;

  struct Row {
    const char* iso;
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
  for (const Row& r : rows) {
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
[[nodiscard]] CorpusBoard board_from_spec(const SynthPanelSpec& spec, std::string date,
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
[[nodiscard]] PricedSurface fit_reference(const CorpusBoard& board) {
  auto chain = OptionChain::from_frame(board.frame, board.env);
  EXPECT_TRUE(chain.has_value());
  PricerConfig cfg;  // Robust default
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

// Assert two PricedSurfaces are bit-identical over a (K, T, side) grid straddling
// each slice forward. Accumulates the number of priced grid points into `n_fv`
// (void return so the ASSERT_* fatal guards are legal here).
void expect_surfaces_bit_identical(const PricedSurface& a, const PricedSurface& b,
                                   std::size_t& n_fv) {
  EXPECT_EQ(a.n_slices(), b.n_slices());
  for (const SliceContext& c : a.context()) {
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
[[nodiscard]] std::vector<CorpusBoard> make_mixed_boards(const std::vector<std::string>& dates) {
  std::vector<CorpusBoard> boards;
  for (const std::string& d : dates) {
    boards.push_back(
        board_from_spec(make_index_spec("SPY", d, 600.0), d, "SPY", convex_dense_pin()));
    boards.push_back(board_from_spec(make_singlename_spec("XOM", d, 110.0), d, "XOM"));
  }
  return boards;
}

}  // namespace

// ── 1. Layout + curve-family mix ────────────────────────────────────────────
TEST(Corpus, BuildCorpus_MultiDateMultiSymbol_LaysOutOneArchivePerDate) {
  const fs::path out = fresh_out_dir("layout");
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18"};

  auto man_res = build_corpus(make_mixed_boards(dates), out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest& man = *man_res;

  // Counts + dates.
  EXPECT_EQ(man.n_boards, 4u);
  EXPECT_EQ(man.n_ok, 4u);
  EXPECT_EQ(man.n_failed, 0u);
  EXPECT_EQ(man.n_skipped, 0u);
  ASSERT_EQ(man.dates.size(), 2u);
  EXPECT_EQ(man.dates[0], "2026-06-17");
  EXPECT_EQ(man.dates[1], "2026-06-18");

  // One archive file per date + the manifest.
  for (const std::string& d : dates) {
    EXPECT_TRUE(fs::exists(out / (d + ".atxvsa"))) << d;
  }
  EXPECT_TRUE(fs::exists(out / "manifest.tsv"));

  // Entries sorted (date asc, symbol asc): (d, SPY), (d, XOM) per date, each
  // curve family as expected (SPY pinned dense; XOM auto -> eSSVI).
  ASSERT_EQ(man.entries.size(), 4u);
  for (const CorpusEntry& e : man.entries) {
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
  const CorpusManifest& man = *man_res;

  std::size_t n_checked = 0;
  std::size_t n_points = 0;
  for (const CorpusEntry& e : man.entries) {
    if (e.status != CorpusFitStatus::Ok) {
      continue;
    }
    // Reopen the date's archive and reconstruct the surface.
    auto arch = SurfaceArchive::open_file(e.archive_path);
    ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
    auto reloaded = arch->map_symbol(e.symbol);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error().to_string();

    // Reproduce the board's fit inline (deterministic) and compare bit-for-bit.
    const CorpusBoard* board = nullptr;
    for (const CorpusBoard& b : boards) {
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

// ── 4. Manifest round-trips ─────────────────────────────────────────────────
TEST(Corpus, Manifest_RoundTrips) {
  const fs::path out = fresh_out_dir("manifest");
  const std::vector<CorpusBoard> boards = make_mixed_boards({"2026-06-17", "2026-06-18"});

  auto man_res = build_corpus(boards, out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest& man = *man_res;

  // serialize -> parse.
  const std::string tsv = serialize_manifest(man);
  auto parsed = parse_manifest(tsv);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().to_string();
  EXPECT_EQ(*parsed, man);

  // write file -> read file (build_corpus already wrote out/manifest.tsv).
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
    b.symbol = std::move(symbol);  // default frame => rows empty => Skipped
    b.env = MarketEnv::flat(100.0, 0.043, iso_to_ns("2026-06-17"), {});
    return b;
  };

  std::vector<CorpusBoard> boards;
  // Date with one Ok + one Skipped board.
  boards.push_back(
      board_from_spec(make_index_spec("SPY", "2026-06-17", 600.0), "2026-06-17", "SPY",
                      convex_dense_pin()));
  boards.push_back(empty_board("2026-06-17", "EMPTY"));
  // Date with ZERO Ok boards => no archive file written for it.
  boards.push_back(empty_board("2026-06-30", "ONLYEMPTY"));

  auto man_res = build_corpus(boards, out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest& man = *man_res;

  EXPECT_EQ(man.n_boards, 3u);
  EXPECT_EQ(man.n_ok, 1u);
  EXPECT_EQ(man.n_skipped, 2u);
  EXPECT_EQ(man.n_failed, 0u);

  // The Ok date has an archive; the zero-Ok date does not.
  EXPECT_TRUE(fs::exists(out / "2026-06-17.atxvsa"));
  EXPECT_FALSE(fs::exists(out / "2026-06-30.atxvsa"));

  for (const CorpusEntry& e : man.entries) {
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
    const CorpusEntry& a = m1->entries[i];
    const CorpusEntry& b = m8->entries[i];
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
    const CorpusEntry& a = m1->entries[i];
    const CorpusEntry& b = m8->entries[i];
    if (a.status != CorpusFitStatus::Ok) {
      continue;
    }
    auto arch_a = SurfaceArchive::open_file(a.archive_path);
    auto arch_b = SurfaceArchive::open_file(b.archive_path);
    ASSERT_TRUE(arch_a.has_value() && arch_b.has_value());
    auto sa = arch_a->map_symbol(a.symbol);
    auto sb = arch_b->map_symbol(b.symbol);
    ASSERT_TRUE(sa.has_value() && sb.has_value());
    expect_surfaces_bit_identical(*sa, *sb, n_points);
  }
  EXPECT_GT(n_points, 20u);
}

// ── 5. Throughput smoke ─────────────────────────────────────────────────────
TEST(Corpus, Throughput_FitsUnderCeiling) {
  ATX_VOL_SKIP_UNLESS_BENCH();
  const fs::path out = fresh_out_dir("throughput");

  // 10 dates x 2 symbols = 20 boards. Snapshots are all before the earliest
  // listed expiry so every year-fraction is positive.
  std::vector<std::string> dates;
  for (int day = 8; day <= 17; ++day) {
    char date[16];
    std::snprintf(date, sizeof date, "2026-06-%02d", day);
    dates.emplace_back(date);
  }
  const std::vector<CorpusBoard> boards = make_mixed_boards(dates);
  ASSERT_EQ(boards.size(), 20u);

  const auto t0 = std::chrono::steady_clock::now();
  auto man_res = build_corpus(boards, out.string());  // fan-out across boards
  const auto t1 = std::chrono::steady_clock::now();
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest& man = *man_res;

  const double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  EXPECT_EQ(man.n_boards, 20u);
  EXPECT_EQ(man.n_ok, 20u);
  EXPECT_EQ(man.dates.size(), 10u);
  EXPECT_LT(wall_ms, 60000.0) << "throughput ceiling exceeded";

  std::printf("[corpus] throughput: boards=%u dates=%zu ok=%u wall=%.0f ms\n",
              man.n_boards, man.dates.size(), man.n_ok, wall_ms);
}
