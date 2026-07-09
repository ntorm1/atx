// S1-1 — distinct per-symbol uids across a corpus archive (THE northstar
// blocker for the multi-name dispersion pipeline).
//
// `Universe` reserves uid slot 0 then assigns uid = unders_.size() to each
// newly interned ticker (universe.cpp), so the SOLE ticker of every fresh
// single-symbol `Universe` gets uid=1. `build_corpus` fits one board per
// (date, symbol), each in its OWN `Universe`, so every single-name
// `PricedSurface` out of the corpus pipeline carries uid=1. Loading a date
// with more than one symbol therefore used to fail: `MarketSnapshot::load`
// builds a `SurfaceSet` keyed by uid (portfolio_pricer.cpp), which rejects
// duplicate uids with `Err(InvalidArgument, "SurfaceSet: duplicate uid")`.
// This was masked because every pre-existing test was single-symbol-per-date
// or manually hand-stamped distinct uids on synthetic surfaces, bypassing the
// corpus/Universe path entirely.
//
// The fix (corpus.cpp): stamp a stable, symbol-derived `uid_for_symbol(...)`
// (universe.hpp) onto each surface's ARCHIVED copy at write time (via
// `with_uid`), so a date's archive holds DISTINCT uids per symbol without
// touching the in-memory single-symbol served/session path (which keeps
// uid=1).
//
// Synthetic-only (no OPRA pull, no paid data): a 2-date corpus of
// {index "SPY" + 3 names "AAA"/"BBB"/"CCC"}, reusing corpus_test.cpp's
// synthetic-board construction pattern (make_index_spec / make_singlename_spec
// analogues) so this test runs everywhere and is not GTEST_SKIP-gated.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "atx/vol/backtest.hpp"        // MarketSnapshot
#include "atx/vol/corpus.hpp"          // build_corpus, CorpusBoard, CorpusManifest
#include "atx/vol/data.hpp"            // iso_to_ns, year_fraction
#include "atx/vol/market_env.hpp"      // MarketEnv
#include "atx/vol/panel.hpp"           // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/s3.hpp"              // S3Params
#include "atx/vol/spy_fixture.hpp"     // make_spy_synthetic_spec
#include "atx/vol/universe.hpp"        // uid_for_symbol
#include "atx/vol/vol_curve.hpp"       // CurveConfig, VolCurveKind

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] fs::path fresh_out_dir(const char* tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-multiname-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

[[nodiscard]] CurveConfig convex_dense_pin() {
  CurveConfig c;
  c.kind = VolCurveKind::ConvexDense;
  c.convex.node_cap = 40;
  return c;
}

// A penny-dense INDEX board (mirrors corpus_test.cpp's make_index_spec): the
// canonical SPY fixture rescaled to `spot`.
[[nodiscard]] SynthPanelSpec make_index_spec(const std::string& snapshot, double spot) {
  SynthPanelSpec s = make_spy_synthetic_spec(snapshot);
  const double scale = spot / s.spot;
  s.spot = spot;
  for (double& k : s.strikes) {
    k *= scale;
  }
  return s;
}

// A wider-spread, single-name-style board (mirrors corpus_test.cpp's
// make_singlename_spec exactly, parameterized only by spot, so this reuses a
// PROVEN-robust synthetic fit recipe for 3 distinct name boards).
[[nodiscard]] SynthPanelSpec make_singlename_spec(const std::string& snapshot, double spot) {
  SynthPanelSpec s;
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
  for (const double m :
       {0.80, 0.83, 0.87, 0.91, 0.95, 0.98, 1.0, 1.02, 1.05, 1.09, 1.13, 1.17, 1.20}) {
    s.strikes.push_back(spot * m);
  }
  s.half_spread_frac = 0.05;
  s.min_half_spread = 0.05;
  return s;
}

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

// A 2-date corpus of {index "SPY" + names "AAA"/"BBB"/"CCC"} — 4 boards/date,
// 8 total. SPY pins ConvexDense (the dense index recipe, matching
// corpus_test.cpp); the names auto-select (eSSVI on this smooth truth).
[[nodiscard]] std::vector<CorpusBoard> make_multiname_boards(
    const std::vector<std::string>& dates) {
  std::vector<CorpusBoard> boards;
  for (const std::string& d : dates) {
    boards.push_back(
        board_from_spec(make_index_spec(d, 600.0), d, "SPY", convex_dense_pin()));
    boards.push_back(board_from_spec(make_singlename_spec(d, 110.0), d, "AAA"));
    boards.push_back(board_from_spec(make_singlename_spec(d, 85.0), d, "BBB"));
    boards.push_back(board_from_spec(make_singlename_spec(d, 220.0), d, "CCC"));
  }
  return boards;
}

}  // namespace

// ── S1-1 gate: a multi-symbol date loads Ok with 4 distinct uids ────────────
TEST(MultinamePipeline, MultiSymbolDateLoadsWithDistinctUids) {
  const fs::path out = fresh_out_dir("s1-1");
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18"};

  auto man_res = build_corpus(make_multiname_boards(dates), out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest& man = *man_res;
  ASSERT_EQ(man.n_boards, 8u);
  ASSERT_EQ(man.n_ok, 8u) << "every synthetic board must fit Ok for this gate to be meaningful";

  for (const std::string& d : dates) {
    const std::string archive_path = (out / (d + ".atxvsa")).string();
    ASSERT_TRUE(fs::exists(archive_path)) << d;

    // THE gate: before the S1-1 fix this returns
    // Err(InvalidArgument, "SurfaceSet: duplicate uid") because every board's
    // surface carries uid=1 out of its own single-symbol Universe.
    auto snap = MarketSnapshot::load(archive_path);
    ASSERT_TRUE(snap.has_value()) << d << ": " << snap.error().to_string();

    // 4 distinct, non-zero uids in this date's SurfaceSet, each resolving to
    // exactly uid_for_symbol(symbol) and to the right surface via find().
    std::vector<std::uint32_t> uids;
    for (const char* sym : {"SPY", "AAA", "BBB", "CCC"}) {
      const std::optional<std::uint32_t> u = snap->uid_of(sym);
      ASSERT_TRUE(u.has_value()) << d << " " << sym;
      EXPECT_EQ(*u, uid_for_symbol(sym)) << d << " " << sym;
      EXPECT_NE(*u, 0u) << d << " " << sym << ": uid 0 is the reserved invalid sentinel";
      EXPECT_NE(snap->find(*u), nullptr) << d << " " << sym << ": find() must resolve the surface";
      uids.push_back(*u);
    }
    std::sort(uids.begin(), uids.end());
    EXPECT_EQ(std::adjacent_find(uids.begin(), uids.end()), uids.end())
        << d << ": the 4 uids must be pairwise distinct";
  }
}

// ── uid_for_symbol: stable, case-insensitive, non-zero, and injective enough
//    for a small basket ──────────────────────────────────────────────────────
TEST(MultinamePipeline, UidForSymbol_StableAndCaseInsensitive) {
  // Canonicalization: upper/lower/mixed case all agree.
  EXPECT_EQ(uid_for_symbol("SPY"), uid_for_symbol("spy"));
  EXPECT_EQ(uid_for_symbol("SPY"), uid_for_symbol("Spy"));

  // Stable across repeated calls (no process- or time-seeded state).
  EXPECT_EQ(uid_for_symbol("SPY"), uid_for_symbol("SPY"));
  EXPECT_EQ(uid_for_symbol("AAA"), uid_for_symbol("AAA"));

  // Non-zero (uid 0 is the reserved sentinel).
  EXPECT_NE(uid_for_symbol("SPY"), 0u);
  EXPECT_NE(uid_for_symbol("AAA"), 0u);
  EXPECT_NE(uid_for_symbol("BBB"), 0u);
  EXPECT_NE(uid_for_symbol("CCC"), 0u);

  // Distinct symbols map to distinct uids (small basket; collisions would be
  // astronomically unlikely and are backstopped by SurfaceSet::create's
  // duplicate-uid rejection at load).
  EXPECT_NE(uid_for_symbol("SPY"), uid_for_symbol("AAA"));
  EXPECT_NE(uid_for_symbol("AAA"), uid_for_symbol("BBB"));
  EXPECT_NE(uid_for_symbol("BBB"), uid_for_symbol("CCC"));
}
