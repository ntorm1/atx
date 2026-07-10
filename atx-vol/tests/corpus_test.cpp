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
#include <limits>
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
    auto arch = SurfaceArchive::open_file(e.archive_path);
    ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
    auto reloaded = arch->map_symbol(e.symbol);
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

// ── 4. Manifest round-trips ─────────────────────────────────────────────────
TEST(Corpus, Manifest_RoundTrips) {
  const fs::path out = fresh_out_dir("manifest");
  const std::vector<CorpusBoard> boards = make_mixed_boards({"2026-06-17", "2026-06-18"});

  auto man_res = build_corpus(boards, out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest &man = *man_res;

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

  quality.fit_in_band = 1.01;
  const CorpusAdmissionDecision out_of_range =
      evaluate_corpus_admission(quality, ordinary_liquid_rule());
  EXPECT_EQ(out_of_range.primary_reason, CorpusAdmissionReason::MetricOutOfRange);
  EXPECT_TRUE(out_of_range.failed(CorpusAdmissionReason::MetricOutOfRange));
}

TEST(CorpusAdmission, SparseProfileCanPassItsOwnEvidenceFloor) {
  CorpusQualityMetrics sparse = passing_quality_metrics();
  sparse.profile = ProfileKind::IlliquidSmallCap;
  sparse.n_two_sided = 24u;
  sparse.n_slices = 3u;
  sparse.n_holdout = 4u;

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
  const QualifiedCorpusEntry &entry = built->quality.entries.front();
  EXPECT_EQ(entry.disposition, CorpusDisposition::Quarantined);
  EXPECT_EQ(entry.primary_reason, CorpusAdmissionReason::TooFewQuotes);
  EXPECT_EQ(entry.quality.n_raw_quotes, 104u);
  EXPECT_EQ(entry.quality.n_two_sided, 78u);
  EXPECT_GT(entry.quality.n_slices, 0u);
  EXPECT_EQ(built->manifest.n_failed, 1u);
  EXPECT_FALSE(fs::exists(out / "2026-06-17.atxvsa"));
}

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
  auto man_res = build_corpus(boards, out.string()); // fan-out across boards
  const auto t1 = std::chrono::steady_clock::now();
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest &man = *man_res;

  const double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  EXPECT_EQ(man.n_boards, 20u);
  EXPECT_EQ(man.n_ok, 20u);
  EXPECT_EQ(man.dates.size(), 10u);
  EXPECT_LT(wall_ms, 60000.0) << "throughput ceiling exceeded";

  std::printf("[corpus] throughput: boards=%u dates=%zu ok=%u wall=%.0f ms\n", man.n_boards,
              man.dates.size(), man.n_ok, wall_ms);
}
