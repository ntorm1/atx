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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "atx/vol/backtest.hpp"   // MarketSnapshot
#include "atx/vol/corpus.hpp"     // build_corpus, CorpusBoard, CorpusManifest
#include "atx/vol/data.hpp"       // iso_to_ns, year_fraction
#include "atx/vol/dispersion.hpp" // DispersionUniverse, dispersion_signal, resolve_universe_uids
#include "atx/vol/market_env.hpp" // MarketEnv
#include "atx/vol/panel.hpp"      // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/portfolio_pricer.hpp" // Portfolio, PortfolioPricer, PriceStatus, PnlFrame
#include "atx/vol/s3.hpp"               // S3Params
#include "atx/vol/spy_fixture.hpp"      // make_spy_synthetic_spec
#include "atx/vol/strategy.hpp"         // DispersionStrategy
#include "atx/vol/tools/tearsheet.hpp"        // write_backtest_tsv
#include "atx/vol/universe.hpp"         // uid_for_symbol
#include "atx/vol/vol_curve.hpp"        // CurveConfig, VolCurveKind
#include "support/cached_artifacts.hpp" // cached_corpus

using namespace atx::vol;
namespace fs = std::filesystem;
using atx::vol::test::cached_corpus;

namespace {

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// Fitted synthetic artifacts are numerically stable but not bit-stable across
// compiler modes and fit-cache generations. These bounds retain an economic
// regression gate while allowing the exact integer-expiry tenor path: at most
// $0.0077 P&L/NAV, $0.02 vega, 0.001 delta, 0.002 gamma, and $1/year theta.
constexpr double kMoneyTolerance = 0.0077;
constexpr double kVegaTolerance = 0.02;
constexpr double kDeltaTolerance = 0.001;
constexpr double kGammaTolerance = 0.002;
constexpr double kAnnualThetaTolerance = 1.0;

// ── E1 / AN-P1-1 DOCUMENTED DRIFT ──────────────────────────────────────────
//
// `DispersionConfig::target_vega` is now dollars of index gross vega per VOL
// POINT rather than per UNIT vol (the canonical convention, shared with the
// listed route). A default-configured dispersion book therefore carries exactly
// 1/0.01 = 100x the contracts it used to, and every $-denominated and
// risk-denominated series a backtest over that book emits scales by that same
// factor. Nothing else about these runs changed: same strikes, same expiries,
// same fitted vols, same per-share marks, same lot cardinality.
//
// The captured baselines below are LEFT EXACTLY AS CAPTURED and scaled at the
// comparison site, so "the 100x is the only thing that moved" stays a property
// of the test rather than a claim in a comment. The tolerances scale with them,
// which keeps every assertion exactly as economically strict as it was — an
// unscaled tolerance against a 100x book would silently become a 100x TIGHTER
// relative gate and go flaky.
constexpr double kE1BookScale = 100.0;

[[nodiscard]] fs::path fresh_out_dir(const char *tag) {
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

[[nodiscard]] CurveConfig essvi_pin() {
  CurveConfig c;
  c.kind = VolCurveKind::Essvi;
  return c;
}

// A penny-dense INDEX board (mirrors corpus_test.cpp's make_index_spec): the
// canonical SPY fixture rescaled to `spot`.
[[nodiscard]] SynthPanelSpec make_index_spec(const std::string &snapshot, double spot) {
  SynthPanelSpec s = make_spy_synthetic_spec(snapshot);
  const double scale = spot / s.spot;
  s.spot = spot;
  for (double &k : s.strikes) {
    k *= scale;
  }
  return s;
}

// A wider-spread, single-name-style board (mirrors corpus_test.cpp's
// make_singlename_spec exactly, parameterized only by spot, so this reuses a
// PROVEN-robust synthetic fit recipe for 3 distinct name boards).
[[nodiscard]] SynthPanelSpec make_singlename_spec(const std::string &snapshot, double spot) {
  SynthPanelSpec s;
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
  for (const double m :
       {0.80, 0.83, 0.87, 0.91, 0.95, 0.98, 1.0, 1.02, 1.05, 1.09, 1.13, 1.17, 1.20}) {
    s.strikes.push_back(spot * m);
  }
  s.half_spread_frac = 0.05;
  s.min_half_spread = 0.05;
  return s;
}

[[nodiscard]] CorpusBoard board_from_spec(const SynthPanelSpec &spec, std::string date,
                                          std::string symbol,
                                          std::optional<CurveConfig> curve = essvi_pin()) {
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
// corpus_test.cpp); names pin eSSVI so backtest baselines do not depend on the
// evolving auto-fit policy.
[[nodiscard]] std::vector<CorpusBoard>
make_multiname_boards(const std::vector<std::string> &dates) {
  std::vector<CorpusBoard> boards;
  for (const std::string &d : dates) {
    boards.push_back(board_from_spec(make_index_spec(d, 600.0), d, "SPY", convex_dense_pin()));
    boards.push_back(board_from_spec(make_singlename_spec(d, 110.0), d, "AAA"));
    boards.push_back(board_from_spec(make_singlename_spec(d, 85.0), d, "BBB"));
    boards.push_back(board_from_spec(make_singlename_spec(d, 220.0), d, "CCC"));
  }
  return boards;
}

} // namespace

// ── S1-1 gate: a multi-symbol date loads Ok with 4 distinct uids ────────────
TEST(MultinamePipeline, MultiSymbolDateLoadsWithDistinctUids) {
  const fs::path out = fresh_out_dir("s1-1");
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18"};

  auto man_res = build_corpus(make_multiname_boards(dates), out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  const CorpusManifest &man = *man_res;
  ASSERT_EQ(man.n_boards, 8u);
  ASSERT_EQ(man.n_ok, 8u) << "every synthetic board must fit Ok for this gate to be meaningful";

  for (const std::string &d : dates) {
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
    for (const char *sym : {"SPY", "AAA", "BBB", "CCC"}) {
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

// ── S1-2: a symbol-authored dispersion universe resolves on every date ───────
//
// A real basket is authored in SYMBOLS (uid=0); the uid scheme is an archive
// detail nobody hand-writes. `resolve_universe_uids` binds every leg by symbol
// against the loaded snapshot's directory so the same universe works across
// dates regardless of the uid scheme.
TEST(MultinamePipeline, UniverseAuthoredBySymbolResolvesOnEveryDate) {
  const fs::path out = fresh_out_dir("s1-2-resolve");
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18"};

  auto man_res = build_corpus(make_multiname_boards(dates), out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  ASSERT_EQ(man_res->n_ok, 8u) << "every synthetic board must fit Ok";

  // Authored in symbols only; every uid left at the reserved 0.
  DispersionUniverse u;
  u.index = DispersionMember{"SPY", 0u, 0.0};
  u.names.push_back(DispersionMember{"AAA", 0u, 1.0});
  u.names.push_back(DispersionMember{"BBB", 0u, 1.0});
  u.names.push_back(DispersionMember{"CCC", 0u, 1.0});
  const double T = 30.0 / 365.25;

  std::optional<std::uint32_t> spy_uid_across_dates;
  for (const std::string &d : dates) {
    const std::string archive_path = (out / (d + ".atxvsa")).string();
    auto snap = MarketSnapshot::load(archive_path);
    ASSERT_TRUE(snap.has_value()) << d << ": " << snap.error().to_string();

    // Motivating failure (RED): the unresolved (uid=0) universe cannot price —
    // uid 0 is never registered in the SurfaceSet, so the index leg is NotFound.
    auto unresolved = dispersion_signal(u, snap->set(), T);
    ASSERT_FALSE(unresolved.has_value()) << d;
    EXPECT_EQ(unresolved.error().code(), ErrorCode::NotFound) << d;
    std::printf("[multiname] %s unresolved dispersion_signal -> %s\n", d.c_str(),
                unresolved.error().to_string().c_str());

    // Resolution binds each leg by symbol against the snapshot directory.
    auto ru = resolve_universe_uids(u, [&](std::string_view s) { return snap->uid_of(s); });
    ASSERT_TRUE(ru.has_value()) << d << ": " << ru.error().to_string();

    // Every leg's uid is non-zero and equal to uid_for_symbol(symbol).
    EXPECT_EQ(ru->index.uid, uid_for_symbol("SPY")) << d;
    EXPECT_EQ(ru->names[0].uid, uid_for_symbol("AAA")) << d;
    EXPECT_EQ(ru->names[1].uid, uid_for_symbol("BBB")) << d;
    EXPECT_EQ(ru->names[2].uid, uid_for_symbol("CCC")) << d;

    std::vector<std::uint32_t> uids = {ru->index.uid, ru->names[0].uid, ru->names[1].uid,
                                       ru->names[2].uid};
    for (const std::uint32_t uid : uids) {
      EXPECT_NE(uid, 0u) << d << ": resolved uid must be non-zero";
    }
    std::sort(uids.begin(), uids.end());
    EXPECT_EQ(std::adjacent_find(uids.begin(), uids.end()), uids.end())
        << d << ": the 4 resolved uids must be pairwise distinct";

    // The resolved uid for a symbol is identical across dates.
    if (spy_uid_across_dates.has_value()) {
      EXPECT_EQ(*spy_uid_across_dates, ru->index.uid) << d;
    } else {
      spy_uid_across_dates = ru->index.uid;
    }

    // dispersion_signal on the RESOLVED universe now succeeds.
    auto sig = dispersion_signal(*ru, snap->set(), T);
    ASSERT_TRUE(sig.has_value()) << d << ": " << sig.error().to_string();
  }
}

// ── S1-2: resolution fails loudly on unknown and duplicate symbols ───────────
TEST(MultinamePipeline, ResolveUniverseRejectsUnknownAndDuplicateSymbols) {
  const fs::path out = fresh_out_dir("s1-2-reject");
  auto man_res = build_corpus(make_multiname_boards({"2026-06-17"}), out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();

  auto snap = MarketSnapshot::load((out / "2026-06-17.atxvsa").string());
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();
  const auto lookup = [&](std::string_view s) { return snap->uid_of(s); };

  // A name absent from the directory -> NotFound naming it.
  {
    DispersionUniverse u;
    u.index = DispersionMember{"SPY", 0u, 0.0};
    u.names.push_back(DispersionMember{"AAA", 0u, 1.0});
    u.names.push_back(DispersionMember{"ZZZ", 0u, 1.0}); // never archived
    auto r = resolve_universe_uids(u, lookup);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
    EXPECT_NE(r.error().to_string().find("ZZZ"), std::string::npos) << r.error().to_string();
  }
  // The same symbol listed twice -> InvalidArgument.
  {
    DispersionUniverse u;
    u.index = DispersionMember{"SPY", 0u, 0.0};
    u.names.push_back(DispersionMember{"AAA", 0u, 1.0});
    u.names.push_back(DispersionMember{"AAA", 0u, 1.0}); // duplicate leg
    auto r = resolve_universe_uids(u, lookup);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(r.error().to_string().find("AAA"), std::string::npos) << r.error().to_string();
  }
}

// ── S1-2: uid_of canonicalizes its query (case-insensitive) ──────────────────
TEST(MultinamePipeline, UidOfIsCaseInsensitive) {
  const fs::path out = fresh_out_dir("s1-2-case");
  auto man_res = build_corpus(make_multiname_boards({"2026-06-17"}), out.string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();

  auto snap = MarketSnapshot::load((out / "2026-06-17.atxvsa").string());
  ASSERT_TRUE(snap.has_value()) << snap.error().to_string();

  const std::optional<std::uint32_t> lo = snap->uid_of("spy");
  const std::optional<std::uint32_t> hi = snap->uid_of("SPY");
  ASSERT_TRUE(hi.has_value());
  ASSERT_TRUE(lo.has_value()) << "uid_of must canonicalize its query (case-insensitive)";
  EXPECT_EQ(*lo, *hi);
  EXPECT_EQ(*hi, uid_for_symbol("SPY"));
}

// ── S1-2: the on-disk uid scheme is frozen (canonical_symbol refactor guard) ─
//
// Pinned to the literal values produced at c7721aa: FNV-1a32 over the canonical
// (ASCII-upper, <=32-byte) symbol, mapping a 0 digest to 1. The canonical_symbol
// refactor must NOT move these — a change here means the persisted uid scheme
// moved and every existing archive would fail to resolve.
TEST(MultinamePipeline, UidForSymbolValuesArePinned) {
  EXPECT_EQ(uid_for_symbol("SPY"), 1478221309u);
  EXPECT_EQ(uid_for_symbol("AAA"), 3061902210u);
  EXPECT_EQ(uid_for_symbol("BBB"), 2641672453u);
  EXPECT_EQ(uid_for_symbol("CCC"), 1716134816u);
  // Case folds at the source, matching the pinned upper-case value.
  EXPECT_EQ(uid_for_symbol("spy"), 1478221309u);
}

// ── S1-3: a corpus with one name absent on one date runs to completion ────────
//
// The inception date omits CCC (its board is absent from that date's archive);
// every later date carries it. Under the Error policy the missing name aborts the
// whole run (RED); under DropRenormalize CCC is dropped on the inception date, the
// surviving basket is renormalized and the backtest runs to completion. The engine
// cannot MTM a lot whose surface later vanishes, so the demonstrable drop is at an
// OPEN where the name is not subsequently held — here the inception open, which
// never puts a CCC lot into the book.
TEST(MultinamePipeline, CorpusWithMissingNameOnOneDateRunsToCompletion) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19"};

  std::vector<CorpusBoard> boards;
  for (std::size_t di = 0; di < dates.size(); ++di) {
    const std::string &d = dates[di];
    boards.push_back(board_from_spec(make_index_spec(d, 600.0), d, "SPY", convex_dense_pin()));
    boards.push_back(board_from_spec(make_singlename_spec(d, 110.0), d, "AAA"));
    boards.push_back(board_from_spec(make_singlename_spec(d, 85.0), d, "BBB"));
    if (di != 0) { // inception date omits CCC
      boards.push_back(board_from_spec(make_singlename_spec(d, 220.0), d, "CCC"));
    }
  }
  // Distinct board set from missing_bbb_boards (this omits CCC at inception,
  // not BBB mid-run) -- its own cache key.
  const fs::path out =
      cached_corpus("multiname-missing-ccc-inception", [&boards] { return boards; });
  auto man_res = read_manifest_file((out / "manifest.tsv").string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  ASSERT_EQ(man_res->n_ok, boards.size()) << "every synthetic board must fit Ok";

  auto clock = Clock::from_manifest(*man_res);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionUniverse u;
  u.index = DispersionMember{"SPY", 0u, 0.0};
  u.names.push_back(DispersionMember{"AAA", 0u, 0.5});
  u.names.push_back(DispersionMember{"BBB", 0u, 0.3});
  u.names.push_back(DispersionMember{"CCC", 0u, 0.2});

  // RED: under the Error policy the missing CCC on the inception date aborts the
  // whole run with NotFound (the pre-S1-3 behaviour this task removes).
  {
    DispersionConfig cfg_err; // default Error
    DispersionStrategy strat_err{u, cfg_err};
    auto res = run_backtest(*clock, strat_err);
    ASSERT_FALSE(res.has_value()) << "Error policy must abort on the missing name";
    EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
    std::printf("[s1-3] Error-policy run aborts on missing name: %s\n",
                res.error().to_string().c_str());
  }

  // GREEN: DropRenormalize drops CCC on the inception date and runs to completion.
  DispersionConfig cfg;
  cfg.missing.policy = MissingNamePolicy::DropRenormalize;
  cfg.record_diagnostics = true; // opt into implied_corr / n_names_dropped (now off by default)
  DispersionStrategy strat{u, cfg};
  auto res = run_backtest(*clock, strat);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), dates.size());

  const std::vector<double> *dropped = nullptr;
  const std::vector<double> *corr = nullptr;
  for (const auto &s : res->signals) {
    if (s.first == "n_names_dropped")
      dropped = &s.second;
    if (s.first == "implied_corr")
      corr = &s.second;
  }
  ASSERT_NE(dropped, nullptr) << "n_names_dropped series not recorded";
  ASSERT_NE(corr, nullptr) << "implied_corr series not recorded";
  ASSERT_EQ(dropped->size(), res->size());
  ASSERT_EQ(corr->size(), res->size());

  // Exactly one drop on the inception date, none elsewhere.
  EXPECT_EQ((*dropped)[0], 1.0);
  for (std::size_t i = 1; i < dropped->size(); ++i) {
    EXPECT_EQ((*dropped)[i], 0.0) << "row " << i;
  }
  // A full-length implied_corr series, finite on every row (survivors always trade).
  for (std::size_t i = 0; i < corr->size(); ++i) {
    EXPECT_TRUE(std::isfinite((*corr)[i])) << "row " << i;
  }

  // dropped_on(inception) names CCC with reason NotInSnapshot.
  auto snap0 = MarketSnapshot::load((out / (dates[0] + ".atxvsa")).string());
  ASSERT_TRUE(snap0.has_value()) << snap0.error().to_string();
  const std::vector<DroppedName> dn = strat.dropped_on(*snap0);
  ASSERT_EQ(dn.size(), 1u);
  EXPECT_EQ(dn[0].symbol, "CCC");
  EXPECT_EQ(dn[0].reason, DropReason::NotInSnapshot);
  std::printf("[s1-3] drop-renorm run completes: rows=%zu drops=[%.0f,%.0f,%.0f]\n", res->size(),
              (*dropped)[0], (*dropped)[1], (*dropped)[2]);
}

// ── S1-3: a date below min_names is a no-trade step, not an abort ─────────────
//
// The inception date carries ONLY the index (all names vanish), so the surviving
// basket falls under min_names: that step opens no lots and emits a NaN
// implied_corr, but the run continues and later full-basket dates trade normally.
TEST(MultinamePipeline, AllNamesMissingIsNoTradeStepNotAbort) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19"};

  std::vector<CorpusBoard> boards;
  for (std::size_t di = 0; di < dates.size(); ++di) {
    const std::string &d = dates[di];
    boards.push_back(board_from_spec(make_index_spec(d, 600.0), d, "SPY", convex_dense_pin()));
    if (di != 0) { // inception date has only the index
      boards.push_back(board_from_spec(make_singlename_spec(d, 110.0), d, "AAA"));
      boards.push_back(board_from_spec(make_singlename_spec(d, 85.0), d, "BBB"));
      boards.push_back(board_from_spec(make_singlename_spec(d, 220.0), d, "CCC"));
    }
  }
  // Distinct board set (inception carries only the index) -- its own key.
  const fs::path out =
      cached_corpus("multiname-all-missing-inception", [&boards] { return boards; });
  auto man_res = read_manifest_file((out / "manifest.tsv").string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  ASSERT_EQ(man_res->n_ok, boards.size()) << "every synthetic board must fit Ok";
  auto clock = Clock::from_manifest(*man_res);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionUniverse u;
  u.index = DispersionMember{"SPY", 0u, 0.0};
  u.names.push_back(DispersionMember{"AAA", 0u, 0.5});
  u.names.push_back(DispersionMember{"BBB", 0u, 0.3});
  u.names.push_back(DispersionMember{"CCC", 0u, 0.2});

  DispersionConfig cfg;
  cfg.missing.policy = MissingNamePolicy::DropRenormalize;
  cfg.record_diagnostics = true; // opt into implied_corr / n_names_dropped (now off by default)
  DispersionStrategy strat{u, cfg};
  auto res = run_backtest(*clock, strat);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), dates.size());

  // Inception is a no-trade step: no lots opened, but the run continues.
  ASSERT_EQ(res->n_open_lots.size(), res->size());
  EXPECT_EQ(res->n_open_lots[0], 0.0) << "the no-trade inception step must open no lots";
  bool traded_later = false;
  for (std::size_t i = 1; i < res->size(); ++i) {
    if (res->n_open_lots[i] > 0.0)
      traded_later = true;
  }
  EXPECT_TRUE(traded_later) << "a later full-basket date must trade";

  // PnL is well-defined on every row (no blow-up from the no-trade step).
  for (std::size_t i = 0; i < res->size(); ++i) {
    EXPECT_TRUE(std::isfinite(res->pnl_total[i])) << "row " << i;
    EXPECT_TRUE(std::isfinite(res->nav[i])) << "row " << i;
  }

  // The inception no-trade step drops the full basket; implied_corr is NaN there.
  const std::vector<double> *dropped = nullptr;
  const std::vector<double> *corr = nullptr;
  for (const auto &s : res->signals) {
    if (s.first == "n_names_dropped")
      dropped = &s.second;
    if (s.first == "implied_corr")
      corr = &s.second;
  }
  ASSERT_NE(dropped, nullptr);
  ASSERT_NE(corr, nullptr);
  EXPECT_EQ((*dropped)[0], 3.0);
  EXPECT_TRUE(std::isnan((*corr)[0])) << (*corr)[0];
}

// ── S1-3a: a no-trade step on a ROLL date must not force-close the held book ───
//
// This is the scenario the S1-3 e2e tests missed: both put the missing-name date
// at INCEPTION, where the book is empty and the roll (`d.clear`) is moot. Here the
// full basket is opened first, then on the NEXT date (a) the surviving basket has
// fallen below `min_names` (a no-trade date) AND (b) the front cohort has decayed
// inside `roll_at_T`, so the lifecycle wants to ROLL. Pre-fix (9db4484) `on_step`
// cleared the book at the top of the roll branch BEFORE discovering the build was
// Unavailable, and returned Ok with an EMPTY book — silently force-closing a held
// basket the contract says must be held flat through the gap. `on_step` is public,
// so this drives it directly to observe `book.lots` and inject the shortfall.
TEST(MultinamePipeline, NoTradeOnRollDateLeavesBookIntact) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18"};

  std::vector<CorpusBoard> boards;
  // date 1: the FULL basket (index + 3 names) -> inception opens 2*(1+3) = 8 lots.
  boards.push_back(
      board_from_spec(make_index_spec(dates[0], 600.0), dates[0], "SPY", convex_dense_pin()));
  boards.push_back(board_from_spec(make_singlename_spec(dates[0], 110.0), dates[0], "AAA"));
  boards.push_back(board_from_spec(make_singlename_spec(dates[0], 85.0), dates[0], "BBB"));
  boards.push_back(board_from_spec(make_singlename_spec(dates[0], 220.0), dates[0], "CCC"));
  // date 2: only index + AAA -> one survivor < min_names(2) => Unavailable/no-trade.
  boards.push_back(
      board_from_spec(make_index_spec(dates[1], 600.0), dates[1], "SPY", convex_dense_pin()));
  boards.push_back(board_from_spec(make_singlename_spec(dates[1], 110.0), dates[1], "AAA"));

  // Distinct 2-date board set (6 boards, one date below min_names) -- its own key.
  const fs::path out = cached_corpus("multiname-roll-notrade-2d", [&boards] { return boards; });
  auto man_res = read_manifest_file((out / "manifest.tsv").string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  ASSERT_EQ(man_res->n_ok, boards.size()) << "every synthetic board must fit Ok";

  auto snap1 = MarketSnapshot::load((out / (dates[0] + ".atxvsa")).string());
  ASSERT_TRUE(snap1.has_value()) << snap1.error().to_string();
  auto snap2 = MarketSnapshot::load((out / (dates[1] + ".atxvsa")).string());
  ASSERT_TRUE(snap2.has_value()) << snap2.error().to_string();

  DispersionUniverse u;
  u.index = DispersionMember{"SPY", 0u, 0.0};
  u.names.push_back(DispersionMember{"AAA", 0u, 0.5});
  u.names.push_back(DispersionMember{"BBB", 0u, 0.3});
  u.names.push_back(DispersionMember{"CCC", 0u, 0.2});

  DispersionConfig cfg; // target_T = 30/365.25
  cfg.missing.policy = MissingNamePolicy::DropRenormalize;
  LifecycleSpec lc;             // RollAtHorizon (default holding)
  lc.roll_at_T = 29.5 / 365.25; // just below target_T => the one-day-later date rolls
  DispersionStrategy strat{u, cfg, lc};

  PortfolioState book;
  std::uint64_t next_id = 1;

  // Inception: the full basket opens 2*(1+3) = 8 lots.
  ASSERT_TRUE(strat.on_step(*snap1, 0, book, next_id).has_value());
  ASSERT_EQ(book.lots.size(), 2u * (1u + 3u));
  const std::vector<Lot> before = book.lots; // the held book, snapshotted
  const std::uint64_t next_id_before = next_id;

  // On date 2 the surviving basket is one name < min_names(2), so the book build is
  // Unavailable — a no-trade date. And the front cohort (residual 29d) is inside
  // roll_at_T (29.5d), so the lifecycle wants to ROLL (clear then reopen).
  auto build2 = strat.build_book(*snap2);
  ASSERT_FALSE(build2.has_value());
  EXPECT_EQ(build2.error().code(), ErrorCode::Unavailable) << build2.error().to_string();

  // THE no-trade-on-roll step.
  const Status st = strat.on_step(*snap2, 1, book, next_id);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();

  // RED against 9db4484: the book comes back EMPTY. Post-fix it is byte-identical to
  // the held basket — the no-trade step leaves it untouched.
  ASSERT_EQ(book.lots.size(), before.size()) << "a no-trade roll step must not clear the held book";
  for (std::size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ(book.lots[i].id, before[i].id) << i;
    EXPECT_EQ(book.lots[i].cohort, before[i].cohort) << i;
    EXPECT_EQ(book.lots[i].expiry_ts_ns, before[i].expiry_ts_ns) << i;
    EXPECT_EQ(book.lots[i].contract.uid, before[i].contract.uid) << i;
    EXPECT_EQ(static_cast<int>(book.lots[i].contract.side),
              static_cast<int>(before[i].contract.side))
        << i;
    EXPECT_TRUE(bits_equal(book.lots[i].contract.K, before[i].contract.K)) << i;
    EXPECT_TRUE(bits_equal(book.lots[i].qty, before[i].qty)) << i;
    EXPECT_TRUE(bits_equal(book.lots[i].entry_price, before[i].entry_price)) << i;
  }
  // No lots opened on a no-trade step => the monotonic id counter did not advance.
  EXPECT_EQ(next_id, next_id_before);
}

// ── S1-3a: the plan's ACTUAL gate — a HELD name goes missing mid-run ──────────
//
// The plan's S1-3 gate is "a corpus where one name is absent on one date runs to
// completion." The northstar reading — and the one S1-3 never tested — is a name
// that is HELD across the gap: present on date 1 (so the strategy opens a straddle
// in it), absent on date 2 (so the engine must mark a HELD lot whose surface is
// gone), present again on date 3. S1-3 only tested a name missing at inception,
// where no lot in it is ever held, on the (verified-false) justification that "the
// backtest can't MTM a lot whose surface later vanishes."
//
// It CAN: for a lot that is alive (not expiring this step) portfolio_pricer marks a
// missing surface as ModelUnavailable and SILENTLY drops it from the total (it does
// NOT Err). So the run completes — it just truncates that lot's PnL for the gap
// step. That silent truncation is a PRE-EXISTING engine defect tracked as S1-3b;
// this test pins it loudly (backtest.cpp / portfolio_pricer.cpp are untouched here)
// and does NOT assert it is correct.
TEST(MultinamePipeline, HeldNameGoesMissingMidRunAndRunCompletes) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19"};

  // BBB present on dates 1 and 3, ABSENT from date 2; every other name present each
  // date. Default lifecycle (RollAtHorizon, roll_at_T 7/365.25, target_T 30/365.25):
  // the date-1 straddles have ~29-28d residual on dates 2-3, well above roll_at_T,
  // so NO roll fires and the date-1 basket (incl. BBB) is HELD across all three
  // dates. No lot expires (30d out), so no settlement path is hit on the gap date.
  std::vector<CorpusBoard> boards;
  for (std::size_t di = 0; di < dates.size(); ++di) {
    const std::string &d = dates[di];
    boards.push_back(board_from_spec(make_index_spec(d, 600.0), d, "SPY", convex_dense_pin()));
    boards.push_back(board_from_spec(make_singlename_spec(d, 110.0), d, "AAA"));
    if (di != 1) { // date 2 (index 1) omits BBB
      boards.push_back(board_from_spec(make_singlename_spec(d, 85.0), d, "BBB"));
    }
    boards.push_back(board_from_spec(make_singlename_spec(d, 220.0), d, "CCC"));
  }
  // Identical construction to missing_bbb_boards(dates) below (BBB absent at
  // the middle date) -- shares that key with its other consumers.
  const fs::path out = cached_corpus("multiname-missing-bbb-3d-v2", [&boards] { return boards; });
  auto man_res = read_manifest_file((out / "manifest.tsv").string());
  ASSERT_TRUE(man_res.has_value()) << man_res.error().to_string();
  ASSERT_EQ(man_res->n_ok, boards.size()) << "every synthetic board must fit Ok";
  auto clock = Clock::from_manifest(*man_res);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionUniverse u;
  u.index = DispersionMember{"SPY", 0u, 0.0};
  u.names.push_back(DispersionMember{"AAA", 0u, 0.5});
  u.names.push_back(DispersionMember{"BBB", 0u, 0.3});
  u.names.push_back(DispersionMember{"CCC", 0u, 0.2});

  DispersionConfig cfg;
  cfg.missing.policy = MissingNamePolicy::DropRenormalize;
  cfg.record_diagnostics = true;    // opt into implied_corr / n_names_dropped (now off by default)
  DispersionStrategy strat{u, cfg}; // default lifecycle: RollAtHorizon

  // THE gate: the run completes with a full-length result (one row per date).
  // WS-F F1(c): the RunConfig default is now UnpricedLotPolicy::Error, and this
  // gate is precisely about SURVIVING a mid-run surface gap, so it opts into the
  // lenient policy explicitly.
  RunConfig rc_lenient;
  rc_lenient.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  auto res = run_backtest(*clock, strat, rc_lenient);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), dates.size());

  const std::vector<double> *dropped = nullptr;
  const std::vector<double> *corr = nullptr;
  for (const auto &s : res->signals) {
    if (s.first == "n_names_dropped")
      dropped = &s.second;
    if (s.first == "implied_corr")
      corr = &s.second;
  }
  ASSERT_NE(dropped, nullptr) << "n_names_dropped series not recorded";
  ASSERT_NE(corr, nullptr) << "implied_corr series not recorded";
  ASSERT_EQ(dropped->size(), dates.size());
  ASSERT_EQ(corr->size(), dates.size());

  // BBB drops out of the SIGNAL only on the gap date: 0,1,0.
  EXPECT_EQ((*dropped)[0], 0.0);
  EXPECT_EQ((*dropped)[1], 1.0);
  EXPECT_EQ((*dropped)[2], 0.0);
  // implied_corr finite on every date: dates 1/3 the full 3-name basket, date 2 the
  // 2-name survivor basket {AAA,CCC} (2 >= min_names).
  for (std::size_t i = 0; i < corr->size(); ++i) {
    EXPECT_TRUE(std::isfinite((*corr)[i])) << "row " << i;
  }

  // The held basket is NOT force-closed on the gap date: 2*(1+3)=8 lots on EVERY
  // row (inception opens 8; no roll fires on dates 2-3, so all 8 persist, including
  // the two BBB straddle legs whose surface has vanished on date 2).
  ASSERT_EQ(res->n_open_lots.size(), dates.size());
  for (std::size_t i = 0; i < dates.size(); ++i) {
    EXPECT_EQ(res->n_open_lots[i], 8.0) << "row " << i << ": the date-1 basket is held";
  }

  // dropped_on(date 2) names BBB with reason NotInSnapshot.
  auto snap2 = MarketSnapshot::load((out / (dates[1] + ".atxvsa")).string());
  ASSERT_TRUE(snap2.has_value()) << snap2.error().to_string();
  const std::vector<DroppedName> dn = strat.dropped_on(*snap2);
  ASSERT_EQ(dn.size(), 1u);
  EXPECT_EQ(dn[0].symbol, "BBB");
  EXPECT_EQ(dn[0].reason, DropReason::NotInSnapshot);

  // ── Pin the PRE-EXISTING silent-PnL-truncation (S1-3b; NOT fixed, NOT asserted
  //    correct) ────────────────────────────────────────────────────────────────
  // Reconstruct the exact date-1 basket and PnL-explain it onto date 2's (BBB-less)
  // surface set — the same operation the engine's compute_step runs internally. The
  // two BBB legs go ModelUnavailable and are dropped from the reduction; the total
  // stays FINITE. That is the silent truncation: the run completes, BBB's held PnL
  // is simply omitted for the gap step. Making it visible is the whole point.
  auto snap1 = MarketSnapshot::load((out / (dates[0] + ".atxvsa")).string());
  ASSERT_TRUE(snap1.has_value()) << snap1.error().to_string();
  auto book1 = strat.build_book(*snap1); // the exact basket opened at inception
  ASSERT_TRUE(book1.has_value()) << book1.error().to_string();
  ASSERT_EQ(book1->positions.size(), 8u);
  auto pf = Portfolio::create(book1->positions);
  ASSERT_TRUE(pf.has_value()) << pf.error().to_string();
  const PortfolioPricer pricer{std::move(*pf)};
  auto frame = pricer.pnl_explain(snap1->set(), snap2->set());
  ASSERT_TRUE(frame.has_value()) << frame.error().to_string(); // completes, does NOT Err

  const std::uint32_t bbb_uid = uid_for_symbol("BBB");
  std::size_t n_unavailable = 0;
  for (std::size_t i = 0; i < frame->size(); ++i) {
    if (frame->status[i] != PriceStatus::Ok) {
      ++n_unavailable;
      EXPECT_EQ(frame->status[i], PriceStatus::ModelUnavailable) << i;
      EXPECT_EQ(frame->uid[i], bbb_uid) << i;            // exactly the BBB legs
      EXPECT_TRUE(std::isnan(frame->pnl_total[i])) << i; // per-leg PnL is NaN...
    }
  }
  EXPECT_EQ(n_unavailable, 2u) << "the two BBB straddle legs go ModelUnavailable";
  EXPECT_EQ(frame->total.n_ok, 6u) << "only the six surviving legs enter the total";
  // ...yet the portfolio total is FINITE — the missing legs are SILENTLY dropped
  // from the reduction, not NaN-propagated. The run's gap-step PnL is likewise a
  // finite (BBB-truncated) number. This truncation is S1-3b; do not fix here.
  EXPECT_TRUE(std::isfinite(frame->total.pnl_total));
  EXPECT_TRUE(std::isfinite(res->pnl_total[1])) << "gap step completes with finite PnL (row 1)";
  std::printf("[s1-3a] held-BBB gap step: %zu/8 legs ModelUnavailable, total.n_ok=%u, "
              "run pnl_total[1]=%.6f (BBB contribution silently truncated; S1-3b)\n",
              n_unavailable, frame->total.n_ok, res->pnl_total[1]);
}

// ── S1-3b: the silent-truncation defect the S1-3a test pinned is now COUNTED ──
//
// Shared fixtures for the S1-3b gates: the exact present->absent->present corpus
// S1-3a used (BBB held across a gap where its board vanishes on the middle date)
// and the basket that trades it.
namespace {
[[nodiscard]] std::vector<CorpusBoard> missing_bbb_boards(const std::vector<std::string> &dates) {
  std::vector<CorpusBoard> boards;
  for (std::size_t di = 0; di < dates.size(); ++di) {
    const std::string &d = dates[di];
    boards.push_back(board_from_spec(make_index_spec(d, 600.0), d, "SPY", convex_dense_pin()));
    boards.push_back(board_from_spec(make_singlename_spec(d, 110.0), d, "AAA"));
    if (di != 1) { // the middle date omits BBB
      boards.push_back(board_from_spec(make_singlename_spec(d, 85.0), d, "BBB"));
    }
    boards.push_back(board_from_spec(make_singlename_spec(d, 220.0), d, "CCC"));
  }
  return boards;
}
[[nodiscard]] DispersionUniverse basket_universe() {
  DispersionUniverse u;
  u.index = DispersionMember{"SPY", 0u, 0.0};
  u.names.push_back(DispersionMember{"AAA", 0u, 0.5});
  u.names.push_back(DispersionMember{"BBB", 0u, 0.3});
  u.names.push_back(DispersionMember{"CCC", 0u, 0.2});
  return u;
}
// Extract one named double column from a `write_backtest_tsv` file (header-driven).
[[nodiscard]] std::vector<double> tsv_column(const std::string &path, const std::string &name) {
  std::ifstream is(path, std::ios::binary);
  EXPECT_TRUE(is.good()) << path;
  std::string content((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
  std::vector<std::vector<std::string>> rows;
  std::size_t start = 0;
  while (start <= content.size()) {
    const std::size_t nl = content.find('\n', start);
    if (nl == std::string::npos)
      break;
    const std::string line = content.substr(start, nl - start);
    std::vector<std::string> cells;
    std::size_t cs = 0;
    while (true) {
      const std::size_t tab = line.find('\t', cs);
      if (tab == std::string::npos) {
        cells.push_back(line.substr(cs));
        break;
      }
      cells.push_back(line.substr(cs, tab - cs));
      cs = tab + 1;
    }
    rows.push_back(std::move(cells));
    start = nl + 1;
  }
  std::vector<double> out;
  if (rows.empty())
    return out;
  std::size_t ci = rows.front().size();
  for (std::size_t i = 0; i < rows.front().size(); ++i) {
    if (rows.front()[i] == name) {
      ci = i;
      break;
    }
  }
  EXPECT_LT(ci, rows.front().size()) << "column not found: " << name;
  if (ci >= rows.front().size())
    return out;
  for (std::size_t r = 1; r < rows.size(); ++r) {
    out.push_back(std::strtod(rows[r][ci].c_str(), nullptr));
  }
  return out;
}
} // namespace

// ── S1-3b gate 1: a held lot with no surface is COUNTED, not silently hidden ──
//
// Same corpus S1-3a pinned as silently truncating (BBB held, board absent on the
// middle date). Under the default policy (ExcludeAndReport) the run still returns
// Ok and every pre-existing column is pinned to the V2 correctness-first
// engine — but the excluded legs are now surfaced in `n_unpriced_lots`.
//
// NOTE — the S1-3b brief predicted n_unpriced_lots == {0,2,0}. The engine actually
// produces {0,2,2}: the two BBB legs are unpriced on BOTH the date1->date2 step
// (BBB absent from the SHIFTED snapshot) AND the date2->date3 step (BBB absent from
// the BASE snapshot, and the basket is HELD so the BBB legs are still in the book).
// Verified directly: pnl_explain n_ok is 6/8 on each of those two steps. We assert
// the engine's real behaviour, per "trust the SOURCE". (See report.)
TEST(MultinamePipeline, HeldLotWithoutSurfaceIsCountedNotHidden) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19"};
  const fs::path out =
      cached_corpus("multiname-missing-bbb-3d-v2", [&dates] { return missing_bbb_boards(dates); });
  auto man = read_manifest_file((out / "manifest.tsv").string());
  ASSERT_TRUE(man.has_value()) << man.error().to_string();
  auto clock = Clock::from_manifest(*man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionUniverse u = basket_universe();
  DispersionConfig cfg;
  cfg.missing.policy = MissingNamePolicy::DropRenormalize;
  DispersionStrategy strat{u, cfg};

  // WS-F F1(c): ExcludeAndReport is now an explicit opt-in (the default is Error).
  RunConfig rc_lenient;
  rc_lenient.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  auto res = run_backtest(*clock, strat, rc_lenient);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), dates.size());

  // The count is now visible: 0 at inception, 2 on each step where the held BBB
  // straddle (call+put) is off a BBB-less surface.
  ASSERT_EQ(res->n_unpriced_lots.size(), dates.size());
  EXPECT_EQ(res->n_unpriced_lots[0], 0.0);
  EXPECT_EQ(res->n_unpriced_lots[1], 2.0);
  EXPECT_EQ(res->n_unpriced_lots[2], 2.0);

  // Preserve the V2 economic capture without requiring fitted artifacts to be
  // byte-identical across compiler/cache generations. Exact count, settlement,
  // and lot-cardinality invariants remain strict below.
  const double base_settle[3] = {0.0, 0.0, 0.0};
  // A1 REPIN (core-review finding 1): gross_vega is a near-cancelling net dispersion
  // aggregate, so the corrected-BAW-seed per-leg vega shift (~1e-6) nets to ~0.025 on
  // the small residual — only gross_vega[2] cleared the 0.02 economic band. Both ISA
  // variants recaptured (NDEBUG from rel-avx2, #else from dev/Debug). pnl/nav stayed
  // inside kMoneyTolerance and are left as-is.
#if defined(NDEBUG)
  // The exact optimized-fit baseline differs from Debug because the surface and
  // finite-difference kernels are floating-point optimization sensitive.
  const double base_pnl[3] = {0.0, -23.744716582360475, -24.202360552831159};
  const double base_nav[3] = {0.0, -23.744716582360475, -47.947077135191634};
  const double base_gvega[3] = {-1.3119378473778653e-12, -2949.8154923379057, 0.29051315518298704};
  const double base_gdelta[3] = {15.146697780845908, 9.2260306540096977, 14.349744680261736};
  const double base_ggamma[3] = {26.41128251135758, 12.475918614420866, 27.348456305928352};
  const double base_gtheta[3] = {-15152.17270563155, -8707.6189957414917, -15696.856655668636};
#else
  const double base_pnl[3] = {0.0, -23.744716582360294, -24.202360552831102};
  const double base_nav[3] = {0.0, -23.744716582360294, -47.947077135191392};
  const double base_gvega[3] = {2.2737367544323206e-13, -2949.8154923409302, 0.2905131461535575};
  const double base_gdelta[3] = {15.146697780845923, 9.2260306540096408, 14.34974468026175};
  const double base_ggamma[3] = {26.411282511357911, 12.475918614421015, 27.348456305928316};
  const double base_gtheta[3] = {-15152.172705632258, -8707.6189957418119, -15696.856655668571};
#endif
  for (std::size_t i = 0; i < dates.size(); ++i) {
    EXPECT_NEAR(res->pnl_total[i], base_pnl[i] * kE1BookScale,
                kMoneyTolerance * kE1BookScale)
        << "pnl_total row " << i;
    EXPECT_TRUE(bits_equal(res->pnl_settlement[i], base_settle[i])) << "settle row " << i;
    EXPECT_NEAR(res->nav[i], base_nav[i] * kE1BookScale, kMoneyTolerance * kE1BookScale)
        << "nav row " << i;
    EXPECT_NEAR(res->gross_vega[i], base_gvega[i] * kE1BookScale,
                kVegaTolerance * kE1BookScale)
        << "gvega row " << i;
    EXPECT_NEAR(res->gross_delta[i], base_gdelta[i] * kE1BookScale,
                kDeltaTolerance * kE1BookScale)
        << "gdelta row " << i;
    EXPECT_NEAR(res->gross_gamma[i], base_ggamma[i] * kE1BookScale,
                kGammaTolerance * kE1BookScale)
        << "ggamma row " << i;
    EXPECT_NEAR(res->gross_theta[i], base_gtheta[i] * kE1BookScale,
                kAnnualThetaTolerance * kE1BookScale)
        << "gtheta row " << i;
    EXPECT_EQ(res->n_open_lots[i], 8.0) << "nlots row " << i;
  }

  // The new column round-trips through the TSV export bit-exactly. Written to
  // a per-test scratch dir, NOT the shared corpus cache dir -- other consumers
  // of the same cached corpus run concurrently under ctest -j and must never
  // race on a write into that shared directory.
  const fs::path scratch = fresh_out_dir("s1-3b-counted-out");
  std::error_code ec;
  fs::create_directories(scratch, ec);
  const std::string path = (scratch / "run.tsv").string();
  ASSERT_TRUE(write_backtest_tsv(*res, path).has_value());
  const std::vector<double> col = tsv_column(path, "n_unpriced_lots");
  ASSERT_EQ(col.size(), res->size());
  for (std::size_t i = 0; i < col.size(); ++i) {
    EXPECT_TRUE(bits_equal(col[i], res->n_unpriced_lots[i])) << "tsv n_unpriced_lots row " << i;
  }
  std::printf("[s1-3b] counted (not hidden): n_unpriced_lots=[%.0f,%.0f,%.0f]\n",
              res->n_unpriced_lots[0], res->n_unpriced_lots[1], res->n_unpriced_lots[2]);
}

// ── S1-3b gate 2: the strict Error policy aborts, naming the count + first uid ─
TEST(MultinamePipeline, UnpricedLotPolicyErrorAborts) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19"};
  const fs::path out =
      cached_corpus("multiname-missing-bbb-3d-v2", [&dates] { return missing_bbb_boards(dates); });
  auto man = read_manifest_file((out / "manifest.tsv").string());
  ASSERT_TRUE(man.has_value()) << man.error().to_string();
  auto clock = Clock::from_manifest(*man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionUniverse u = basket_universe();
  DispersionConfig cfg;
  cfg.missing.policy = MissingNamePolicy::DropRenormalize;
  DispersionStrategy strat{u, cfg};

  RunConfig rc;
  rc.unpriced = UnpricedLotPolicy::Error; // the mode a production QIS run would use
  auto res = run_backtest(*clock, strat, rc);
  ASSERT_FALSE(res.has_value()) << "Error policy must abort when a held lot has no surface";
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
  const std::string msg = res.error().to_string();
  EXPECT_NE(msg.find("2 held lot"), std::string::npos) << msg;                          // the count
  EXPECT_NE(msg.find(std::to_string(uid_for_symbol("BBB"))), std::string::npos) << msg; // uid
  std::printf("[s1-3b] Error policy aborts: %s\n", msg.c_str());
}

// ── S1-3b gate 3: default/error policies are identical on a clean corpus ──────
//
// A full basket present on every date: nothing is ever unpriced, every column
// equals the V2 correctness-first run, and Error policy also completes (no abort).
TEST(MultinamePipeline, DefaultPolicyFullBasketBitIdentical) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19"};
  const fs::path out =
      cached_corpus("multiname-full-3d-v2", [&dates] { return make_multiname_boards(dates); });
  auto man = read_manifest_file((out / "manifest.tsv").string());
  ASSERT_TRUE(man.has_value()) << man.error().to_string();
  auto clock = Clock::from_manifest(*man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionUniverse u = basket_universe();
  DispersionConfig cfg;
  cfg.missing.policy = MissingNamePolicy::DropRenormalize;
  DispersionStrategy strat{u, cfg};

  auto res = run_backtest(*clock, strat); // default: ExcludeAndReport
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), dates.size());

  ASSERT_EQ(res->n_unpriced_lots.size(), dates.size());
  for (std::size_t i = 0; i < dates.size(); ++i) {
    EXPECT_EQ(res->n_unpriced_lots[i], 0.0) << "row " << i;
  }

  // Historical economic capture. The fitted artifact is allowed the tight
  // cross-build tolerances above; policy equivalence is checked exactly below.
#if defined(NDEBUG)
  const double base_pnl[3] = {0.0, -42.153969329712808, -42.921431468497737};
  const double base_nav[3] = {0.0, -42.153969329712808, -85.075400798210552};
  const double base_gvega[3] = {-1.3119378473778653e-12, 0.083957165928409322, 0.29051315518298704};
#else
  const double base_pnl[3] = {0.0, -42.153969329712623, -42.92143146849768};
  const double base_nav[3] = {0.0, -42.153969329712623, -85.075400798210296};
  const double base_gvega[3] = {2.2737367544323206e-13, 0.083957163111676891, 0.2905131461535575};
#endif
  // A1 REPIN (core-review finding 1): gross_vega[2] shifted ~0.025 past the 0.02
  // band from the corrected BAW seed (net-cancelling aggregate); both ISA variants
  // recaptured. pnl/nav stayed inside kMoneyTolerance.
  for (std::size_t i = 0; i < dates.size(); ++i) {
    EXPECT_NEAR(res->pnl_total[i], base_pnl[i] * kE1BookScale,
                kMoneyTolerance * kE1BookScale)
        << "pnl_total row " << i;
    EXPECT_NEAR(res->nav[i], base_nav[i] * kE1BookScale, kMoneyTolerance * kE1BookScale)
        << "nav row " << i;
    EXPECT_NEAR(res->gross_vega[i], base_gvega[i] * kE1BookScale,
                kVegaTolerance * kE1BookScale)
        << "gvega row " << i;
  }

  // Error policy must NOT abort a clean corpus (nothing unpriced on any step).
  RunConfig rc;
  rc.unpriced = UnpricedLotPolicy::Error;
  DispersionStrategy strat_e{u, cfg};
  auto res_e = run_backtest(*clock, strat_e, rc);
  ASSERT_TRUE(res_e.has_value()) << res_e.error().to_string();
  ASSERT_EQ(res_e->size(), res->size());
  for (std::size_t i = 0; i < res->size(); ++i) {
    EXPECT_TRUE(bits_equal(res_e->pnl_total[i], res->pnl_total[i])) << "policy pnl row " << i;
    EXPECT_TRUE(bits_equal(res_e->nav[i], res->nav[i])) << "policy nav row " << i;
    EXPECT_TRUE(bits_equal(res_e->gross_vega[i], res->gross_vega[i])) << "policy gvega row " << i;
    EXPECT_EQ(res_e->n_unpriced_lots[i], 0.0) << "policy count row " << i;
  }
  std::printf("[s1-3b] full basket policy-equivalent, all n_unpriced_lots == 0\n");
}

// ── S1-3c gate 1: the book-greeks under-count is REPORTED (was silent) ─────────
//
// `book_greeks` prices the held book against THIS row's date ALONE (a single-date
// snapshot). On the missing-BBB corpus (BBB held across the gap, its board absent
// on the middle date) the two BBB straddle legs cannot be priced on the gap date,
// so that row's gross_* SILENTLY omitted them. `n_unpriced_greeks` now surfaces it:
// 2 on the row whose date lacks BBB, 0 on the rows whose dates carry it.
//
// This DIVERGES from `n_unpriced_lots`, which measures the STEP's PnL completeness
// (pnl_explain needs the surface on BOTH base and shifted). n_unpriced_lots is
// {0,2,2}: the date2->date3 step reads BBB off the (absent) date-2 BASE, whereas
// book_greeks on date 3 reads the (present) date-3 surface and prices all 8 lots.
// The two counts are two different signals; row 2 is where they part.
TEST(MultinamePipeline, BookGreeksUnderCountIsReported) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19"};
  const fs::path out =
      cached_corpus("multiname-missing-bbb-3d-v2", [&dates] { return missing_bbb_boards(dates); });
  auto man = read_manifest_file((out / "manifest.tsv").string());
  ASSERT_TRUE(man.has_value()) << man.error().to_string();
  auto clock = Clock::from_manifest(*man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionUniverse u = basket_universe();
  DispersionConfig cfg;
  cfg.missing.policy = MissingNamePolicy::DropRenormalize;
  DispersionStrategy strat{u, cfg};

  // WS-F F1(c): ExcludeAndReport is now an explicit opt-in (the default is Error).
  RunConfig rc_lenient;
  rc_lenient.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  auto res = run_backtest(*clock, strat, rc_lenient);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), dates.size());

  // book_greeks prices row i against date i: BBB present on d1/d3 (0 unpriced),
  // absent on d2 (its call+put straddle legs -> 2 unpriced greeks).
  ASSERT_EQ(res->n_unpriced_greeks.size(), dates.size());
  EXPECT_EQ(res->n_unpriced_greeks[0], 0.0);
  EXPECT_EQ(res->n_unpriced_greeks[1], 2.0);
  EXPECT_EQ(res->n_unpriced_greeks[2], 0.0);

  // The step-completeness count (S1-3b) is a DIFFERENT signal: {0,2,2}.
  ASSERT_EQ(res->n_unpriced_lots.size(), dates.size());
  EXPECT_EQ(res->n_unpriced_lots[0], 0.0);
  EXPECT_EQ(res->n_unpriced_lots[1], 2.0);
  EXPECT_EQ(res->n_unpriced_lots[2], 2.0);

  // THE divergence that justifies a separate count: row 2's greeks snapshot is
  // COMPLETE (BBB back on d3) while its step is INCOMPLETE (BBB absent from d2 base).
  EXPECT_NE(res->n_unpriced_greeks[2], res->n_unpriced_lots[2]);
  EXPECT_EQ(res->n_unpriced_greeks[2], 0.0);
  EXPECT_EQ(res->n_unpriced_lots[2], 2.0);

  // The new column round-trips through the TSV export bit-exactly. Written to
  // a per-test scratch dir, not the shared corpus cache dir (see the same note
  // in HeldLotWithoutSurfaceIsCountedNotHidden).
  const fs::path scratch = fresh_out_dir("s1-3c-greeks-count-out");
  std::error_code ec;
  fs::create_directories(scratch, ec);
  const std::string path = (scratch / "run.tsv").string();
  ASSERT_TRUE(write_backtest_tsv(*res, path).has_value());
  const std::vector<double> col = tsv_column(path, "n_unpriced_greeks");
  ASSERT_EQ(col.size(), res->size());
  for (std::size_t i = 0; i < col.size(); ++i) {
    EXPECT_TRUE(bits_equal(col[i], res->n_unpriced_greeks[i])) << "tsv row " << i;
  }
  std::printf("[s1-3c] greeks under-count reported: n_unpriced_greeks=[%.0f,%.0f,%.0f] "
              "vs n_unpriced_lots=[%.0f,%.0f,%.0f] (diverge on row 2)\n",
              res->n_unpriced_greeks[0], res->n_unpriced_greeks[1], res->n_unpriced_greeks[2],
              res->n_unpriced_lots[0], res->n_unpriced_lots[1], res->n_unpriced_lots[2]);
}

// ── S1-3c gate 2: the false-vega-flat trap — gross_vega under-reports a leg ─────
//
// On the gap date the held BBB straddle is EXCLUDED from gross_vega (its surface
// is absent). A vega-flat dispersion claim reads gross_vega directly, so a silently
// omitted leg is a FALSE vega-flat reading. This does NOT assert the truncated value
// is correct — only that the run WITHOUT BBB reports strictly less gross_vega on the
// gap row than the identical run WITH BBB present on every date, and that the miss is
// isolated to that row (inception, where both corpora carry BBB, is bit-identical).
TEST(MultinamePipeline, GrossVegaIsUnderReportedWhenALegIsUnpriced) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19"};

  DispersionUniverse u = basket_universe();
  DispersionConfig cfg;
  cfg.missing.policy = MissingNamePolicy::DropRenormalize;

  // Missing-BBB corpus (BBB absent on the middle date).
  const fs::path out_missing =
      cached_corpus("multiname-missing-bbb-3d-v2", [&dates] { return missing_bbb_boards(dates); });
  auto man_m = read_manifest_file((out_missing / "manifest.tsv").string());
  ASSERT_TRUE(man_m.has_value()) << man_m.error().to_string();
  auto clock_m = Clock::from_manifest(*man_m);
  ASSERT_TRUE(clock_m.has_value()) << clock_m.error().to_string();
  DispersionStrategy strat_m{u, cfg};
  // WS-F F1(c): ExcludeAndReport is now an explicit opt-in (the default is Error);
  // the whole point of this gate is the truncated (lenient) reading.
  RunConfig rc_lenient;
  rc_lenient.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  auto res_m = run_backtest(*clock_m, strat_m, rc_lenient);
  ASSERT_TRUE(res_m.has_value()) << res_m.error().to_string();

  // Full basket (BBB present on every date), same universe + strategy.
  const fs::path out_full =
      cached_corpus("multiname-full-3d-v2", [&dates] { return make_multiname_boards(dates); });
  auto man_f = read_manifest_file((out_full / "manifest.tsv").string());
  ASSERT_TRUE(man_f.has_value()) << man_f.error().to_string();
  auto clock_f = Clock::from_manifest(*man_f);
  ASSERT_TRUE(clock_f.has_value()) << clock_f.error().to_string();
  DispersionStrategy strat_f{u, cfg};
  auto res_f = run_backtest(*clock_f, strat_f, rc_lenient);
  ASSERT_TRUE(res_f.has_value()) << res_f.error().to_string();

  ASSERT_EQ(res_m->size(), dates.size());
  ASSERT_EQ(res_f->size(), dates.size());

  // Row 1 (the gap date) is exactly where the greeks under-count is 2 (missing) vs 0.
  ASSERT_EQ(res_m->n_unpriced_greeks[1], 2.0);
  ASSERT_EQ(res_f->n_unpriced_greeks[1], 0.0);

  // Both finite; the BBB-truncated reading is strictly less (the long BBB straddle
  // carries large positive vega, so omitting it drops gross_vega on this row).
  EXPECT_TRUE(std::isfinite(res_m->gross_vega[1]));
  EXPECT_TRUE(std::isfinite(res_f->gross_vega[1]));
  EXPECT_LT(res_m->gross_vega[1], res_f->gross_vega[1])
      << "missing=" << res_m->gross_vega[1] << " full=" << res_f->gross_vega[1];

  // Inception (row 0): BBB present in BOTH corpora, so the greeks snapshot is
  // complete and gross_vega is bit-identical — the under-report is isolated to the
  // gap row, not a corpus-wide difference.
  EXPECT_EQ(res_m->n_unpriced_greeks[0], 0.0);
  EXPECT_EQ(res_f->n_unpriced_greeks[0], 0.0);
  EXPECT_TRUE(bits_equal(res_m->gross_vega[0], res_f->gross_vega[0]));

  std::printf("[s1-3c] false-vega-flat: gap-row gross_vega missing=%.6f < full=%.6f "
              "(BBB leg silently omitted, now counted=%.0f)\n",
              res_m->gross_vega[1], res_f->gross_vega[1], res_m->n_unpriced_greeks[1]);
}

// ── S1-3c gate 3: the Error policy also aborts on a book-greeks under-count ─────
//
// The step-level Error guard (S1-3b) fires whenever a HELD lot is unpriced across a
// step (base OR shifted surface absent). For every row i>=1 that guard PREEMPTS the
// greeks guard: book_greeks on date i under-counts only when date i's surface is
// absent, which also breaks that step's pnl_explain. The one row with NO preceding
// step is INCEPTION (row 0) — a book-greeks snapshot with nothing behind it. The
// dispersion strategy never opens a lot in an absent name, so only the FIXED-book
// overload can hand inception a lot whose surface is absent; this drives exactly
// that (a BBB straddle over a corpus whose inception date has no BBB board). See
// report: this is why the greeks Error is exercised through the fixed-book overload
// rather than replaying the strategy corpus the step guard already covers.
TEST(MultinamePipeline, UnpricedGreeksPolicyErrorAborts) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18"};

  // Inception (d1) omits BBB; d2 carries it.
  std::vector<CorpusBoard> boards;
  for (std::size_t di = 0; di < dates.size(); ++di) {
    const std::string &d = dates[di];
    boards.push_back(board_from_spec(make_index_spec(d, 600.0), d, "SPY", convex_dense_pin()));
    boards.push_back(board_from_spec(make_singlename_spec(d, 110.0), d, "AAA"));
    if (di != 0) { // inception omits BBB
      boards.push_back(board_from_spec(make_singlename_spec(d, 85.0), d, "BBB"));
    }
    boards.push_back(board_from_spec(make_singlename_spec(d, 220.0), d, "CCC"));
  }
  // Distinct 2-date board set (inception omits BBB, not the middle of 3) --
  // its own key.
  const fs::path out = cached_corpus("multiname-greeks-error-2d", [&boards] { return boards; });
  auto man = read_manifest_file((out / "manifest.tsv").string());
  ASSERT_TRUE(man.has_value()) << man.error().to_string();
  auto clock = Clock::from_manifest(*man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // A fixed BBB straddle (call+put, K=85 ATM) surviving past d2 (expiry 2026-07-17).
  const std::uint32_t bbb_uid = uid_for_symbol("BBB");
  const std::int64_t expiry = iso_to_ns("2026-07-17");
  const auto bbb_book = [&]() {
    PortfolioState st;
    st.lots.push_back(
        Lot{1, OptionContract{bbb_uid, 85.0, 0.0, Side::Call}, +1.0, 100.0, expiry, 0, 0.0});
    st.lots.push_back(
        Lot{2, OptionContract{bbb_uid, 85.0, 0.0, Side::Put}, +1.0, 100.0, expiry, 0, 0.0});
    return st;
  };

  // Error policy: the inception book greeks cannot price the BBB straddle (d1 has
  // no BBB) -> abort naming the count, the DATE, and the BBB uid.
  RunConfig rc;
  rc.unpriced = UnpricedLotPolicy::Error;
  auto res = run_backtest(*clock, bbb_book(), rc);
  ASSERT_FALSE(res.has_value())
      << "Error policy must abort on an inception book-greeks under-count";
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
  const std::string msg = res.error().to_string();
  EXPECT_NE(msg.find("2 held lot"), std::string::npos) << msg;            // the count
  EXPECT_NE(msg.find(dates[0]), std::string::npos) << msg;                // the date
  EXPECT_NE(msg.find(std::to_string(bbb_uid)), std::string::npos) << msg; // the uid
  std::printf("[s1-3c] greeks Error aborts at inception: %s\n", msg.c_str());

  // It must NOT fire at inception on an EMPTY book (nothing to price) — the run
  // completes with one row per date and a zero greeks count on every row.
  RunConfig rc_empty;
  rc_empty.unpriced = UnpricedLotPolicy::Error;
  auto res_empty = run_backtest(*clock, PortfolioState{}, rc_empty);
  ASSERT_TRUE(res_empty.has_value()) << res_empty.error().to_string();
  ASSERT_EQ(res_empty->size(), dates.size());
  ASSERT_EQ(res_empty->n_unpriced_greeks.size(), dates.size());
  for (std::size_t i = 0; i < dates.size(); ++i) {
    EXPECT_EQ(res_empty->n_unpriced_greeks[i], 0.0) << "row " << i;
  }
  std::printf("[s1-3c] empty book: Error policy does not fire at inception\n");
}

// ── S1-3c gate 4: default stays numerically pinned; new columns are 0 ──────────
//
// A full basket present on every date: nothing is ever unpriced, every pre-existing
// column equals the V2 run (values pinned in DefaultPolicyFullBasketBitIdentical
// above), and BOTH the S1-3b and S1-3c count columns are all zeros and round-trip.
TEST(MultinamePipeline, DefaultPolicyStillBitIdentical) {
  const std::vector<std::string> dates = {"2026-06-17", "2026-06-18", "2026-06-19"};
  const fs::path out =
      cached_corpus("multiname-full-3d-v2", [&dates] { return make_multiname_boards(dates); });
  auto man = read_manifest_file((out / "manifest.tsv").string());
  ASSERT_TRUE(man.has_value()) << man.error().to_string();
  auto clock = Clock::from_manifest(*man);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DispersionUniverse u = basket_universe();
  DispersionConfig cfg;
  cfg.missing.policy = MissingNamePolicy::DropRenormalize;
  DispersionStrategy strat{u, cfg};

  auto res = run_backtest(*clock, strat); // default: ExcludeAndReport
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), dates.size());

  // Both count columns are all zero on a clean corpus.
  ASSERT_EQ(res->n_unpriced_lots.size(), dates.size());
  ASSERT_EQ(res->n_unpriced_greeks.size(), dates.size());
  for (std::size_t i = 0; i < dates.size(); ++i) {
    EXPECT_EQ(res->n_unpriced_lots[i], 0.0) << "n_unpriced_lots row " << i;
    EXPECT_EQ(res->n_unpriced_greeks[i], 0.0) << "n_unpriced_greeks row " << i;
  }

  // Pre-existing columns remain economically pinned to the V2 full-basket
  // capture; exact zero-count and TSV round-trip invariants remain strict.
#if defined(NDEBUG)
  const double base_pnl[3] = {0.0, -42.153969329712808, -42.921431468497737};
  const double base_nav[3] = {0.0, -42.153969329712808, -85.075400798210552};
  const double base_gvega[3] = {-1.3119378473778653e-12, 0.083957165928409322, 0.29051315518298704};
#else
  const double base_pnl[3] = {0.0, -42.153969329712623, -42.92143146849768};
  const double base_nav[3] = {0.0, -42.153969329712623, -85.075400798210296};
  const double base_gvega[3] = {2.2737367544323206e-13, 0.083957163111676891, 0.2905131461535575};
#endif
  // A1 REPIN (core-review finding 1): same net-dispersion gross_vega[2] band clear
  // as the full-basket test above; both ISA variants recaptured.
  for (std::size_t i = 0; i < dates.size(); ++i) {
    EXPECT_NEAR(res->pnl_total[i], base_pnl[i] * kE1BookScale,
                kMoneyTolerance * kE1BookScale)
        << "pnl_total row " << i;
    EXPECT_NEAR(res->nav[i], base_nav[i] * kE1BookScale, kMoneyTolerance * kE1BookScale)
        << "nav row " << i;
    EXPECT_NEAR(res->gross_vega[i], base_gvega[i] * kE1BookScale,
                kVegaTolerance * kE1BookScale)
        << "gvega row " << i;
  }

  // Both new columns round-trip through the TSV export as all-zero. Written
  // to a per-test scratch dir, not the shared corpus cache dir (see the same
  // note in HeldLotWithoutSurfaceIsCountedNotHidden).
  const fs::path scratch = fresh_out_dir("s1-3c-bit-identical-out");
  std::error_code ec;
  fs::create_directories(scratch, ec);
  const std::string path = (scratch / "run.tsv").string();
  ASSERT_TRUE(write_backtest_tsv(*res, path).has_value());
  for (const char *name : {"n_unpriced_lots", "n_unpriced_greeks"}) {
    const std::vector<double> col = tsv_column(path, name);
    ASSERT_EQ(col.size(), res->size()) << name;
    for (std::size_t i = 0; i < col.size(); ++i) {
      EXPECT_EQ(col[i], 0.0) << name << " row " << i;
    }
  }
  std::printf("[s1-3c] default policy numerically pinned; n_unpriced_lots and "
              "n_unpriced_greeks both all-zero\n");
}
