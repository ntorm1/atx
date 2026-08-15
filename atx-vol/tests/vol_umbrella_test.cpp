#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Umbrella compile check: this translation unit includes ONLY the umbrella
// header. It proves (a) the aggregate public surface is self-consistent — no
// ODR clashes across the co-included headers (e.g. the PricingRoute enum,
// formerly duplicated in the legacy portfolio engine and profile.hpp, now from
// types.hpp), and (b) the quickstart symbols are reachable through the one
// include.
//
// S4-T18 (plan 3.8 + 4.1) makes it do a third thing: this file is now the
// CONTRACT TEST for the public-surface tiering. `vol.hpp` must include EXACTLY
// the Tier-A set — the API atx-vol is willing to freeze for v1 — so the file is
// both a compile probe (it includes the umbrella) and a parser of it (it reads
// the umbrella's own text back off disk and checks the include list). The two
// halves catch different regressions:
//
//   * dropping a Tier-A header from the umbrella silently shrinks the shipped
//     one-include API — caught by UmbrellaIsExactlyTierA;
//   * adding a Tier-B / detail / tools / research / test header to the umbrella
//     silently widens what v1 has promised to freeze — caught by the same test
//     plus UmbrellaAdmitsNoNonShippedTier;
//   * letting a Tier-A header depend on a Tier-B one makes "frozen" a lie,
//     because the Tier-B declaration is then part of a frozen signature —
//     caught by TierAIsClosedUnderInclusion.
//
// The manifest below is therefore the single, reviewable source of truth for
// "what is Tier-A", and any tier change is a deliberate edit to this list.
#include "atx/vol/api/vol.hpp"

#include "support/test_paths.hpp" // testkit::repo_file, tests_root

namespace {

namespace testkit = atx::vol::testkit;

namespace fs = std::filesystem;

// ── The Tier-A manifest ─────────────────────────────────────────────────────
//
// Tier-A = the shipped, frozen v1 surface, and exactly what `vol.hpp` includes.
// It is CLOSED UNDER INCLUSION (TierAIsClosedUnderInclusion below): if a Tier-A
// header includes another `atx/vol/` header, that header is Tier-A too, or it
// is one of the test's permitted escapes (the one generated `detail/` header,
// a Tier-B `api/simd/` kernel, or one of the two promoted-public auxiliaries
// in `kTierBEscapeAuxiliaries` — see that test). Several entries here are on
// the list for precisely the closure reason rather than because a caller
// reaches for them directly — e.g. query_pricing.hpp is pulled in by backtest
// / portfolio_pricer / priced_surface / session, so its declarations are
// frozen whether or not it is named. Pretending otherwise would be the
// dishonest tiering.
//
// ── api/ is the whole public surface; src/ is private (api-restructure, 2026-08-14) ──
//
// The public surface lives entirely under include/atx/vol/api/, an 8-module
// tree (analytics, backtest, core, fitting, marketdata, pricing, simd,
// storage) plus the umbrella api/vol.hpp itself. Everything under
// include/atx/vol/api/ that is NOT in the manifest below is Tier-B: public and
// includable, but deliberately OUTSIDE the frozen umbrella (advanced
// calibrators, the SoA/SIMD batch kernels, the dispersion domain vocabulary,
// harness-facing panels/fixtures). Everything under src/<module>/ is PRIVATE —
// no include/ path, no stability promise, not installed — which replaces the
// old flat include/atx/vol/*.hpp + detail/ + simd/ layout this manifest used
// to describe; that layout no longer exists.
constexpr std::string_view kTierA[] = {
    "atx/vol/api/pricing/adjusted_greeks.hpp",
    "atx/vol/api/pricing/american.hpp",
    "atx/vol/api/pricing/american_iv.hpp",
    "atx/vol/api/analytics/analytics.hpp",
    "atx/vol/api/fitting/arb.hpp",
    "atx/vol/api/backtest/backtest.hpp",
    "atx/vol/api/pricing/black76.hpp",
    "atx/vol/api/fitting/c8.hpp",
    "atx/vol/api/fitting/calib.hpp",
    "atx/vol/api/core/chain.hpp",
    "atx/vol/api/analytics/contract_projection.hpp",
    "atx/vol/api/marketdata/corpus.hpp",
    "atx/vol/api/fitting/correction.hpp",
    "atx/vol/api/fitting/curve_fit.hpp",
    "atx/vol/api/fitting/curve_selector.hpp",
    "atx/vol/api/marketdata/data.hpp",
    "atx/vol/api/fitting/deamer.hpp",
    "atx/vol/api/fitting/dense_slice.hpp",
    "atx/vol/api/backtest/deriv_book.hpp", // vol-derivatives sprint: swap-book pricing joined the umbrella
    "atx/vol/api/pricing/derivatives.hpp",
    "atx/vol/api/backtest/dispersion.hpp",
    "atx/vol/api/backtest/dispersion_strangle.hpp",
    "atx/vol/api/pricing/dividend.hpp",
    "atx/vol/api/analytics/earnings_term_fit.hpp",
    "atx/vol/api/analytics/event_vol.hpp",
    "atx/vol/api/fitting/fit_metrics.hpp",
    "atx/vol/api/fitting/fit_policy.hpp",
    "atx/vol/api/pricing/greeks.hpp",
    "atx/vol/api/pricing/implied_vol.hpp",
    "atx/vol/api/core/market_env.hpp",
    "atx/vol/api/marketdata/opra_panel.hpp",
    "atx/vol/api/fitting/parity.hpp",
    "atx/vol/api/analytics/pnl_attribution.hpp",
    "atx/vol/api/backtest/portfolio_pricer.hpp",
    "atx/vol/api/backtest/priced_surface.hpp",
    "atx/vol/api/backtest/priced_surface_view.hpp",
    "atx/vol/api/fitting/pricer_fitter.hpp",
    "atx/vol/api/fitting/profile.hpp",
    "atx/vol/api/fitting/projection.hpp",
    "atx/vol/api/backtest/query_pricing.hpp",
    "atx/vol/api/pricing/rates_curve.hpp",
    "atx/vol/api/analytics/scenario_grid.hpp",
    "atx/vol/api/fitting/session.hpp",
    "atx/vol/api/fitting/spline_curve.hpp",
    "atx/vol/api/fitting/sr_tenor_grid.hpp",
    "atx/vol/api/backtest/strategy.hpp",
    "atx/vol/api/fitting/surface.hpp",
    "atx/vol/api/storage/surface_archive.hpp",
    "atx/vol/api/storage/surface_db.hpp",
    "atx/vol/api/fitting/surface_parity.hpp",
    "atx/vol/api/fitting/surface_policy.hpp",
    "atx/vol/api/pricing/swap_leg.hpp",
    "atx/vol/api/core/types.hpp",
    "atx/vol/api/marketdata/universe.hpp",
    "atx/vol/api/core/version.hpp",
    "atx/vol/api/fitting/vol_curve.hpp",
    "atx/vol/api/fitting/vol_surface.hpp",
    "atx/vol/api/core/vol_time.hpp",
};

// Direct `atx/vol/...` includes of `path`, in source order, skipping comment
// lines. Both quote forms are accepted so the contract does not hinge on which
// one the umbrella happens to use.
std::vector<std::string> direct_atx_vol_includes(const fs::path& path) {
  std::vector<std::string> out;
  std::ifstream in(path);
  EXPECT_TRUE(in.good()) << "cannot open " << path.string();
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    // A commented-out or documented include is prose, not a dependency: the
    // umbrella's own header comment quotes `#include "atx/vol/api/vol.hpp"`.
    if (line.compare(first, 2, "//") == 0) continue;
    if (line[first] != '#') continue;
    const std::size_t inc = line.find("include", first);
    if (inc == std::string::npos) continue;
    const std::size_t open = line.find_first_of("\"<", inc);
    if (open == std::string::npos) continue;
    const char close = line[open] == '"' ? '"' : '>';
    const std::size_t end = line.find(close, open + 1);
    if (end == std::string::npos) continue;
    std::string target = line.substr(open + 1, end - open - 1);
    if (target.rfind("atx/vol/", 0) == 0) out.push_back(std::move(target));
  }
  return out;
}

// ATX_VOL_INCLUDE_ROOT / ATX_VOL_UMBRELLA_HPP are baked in by tests/CMakeLists
// so the contract holds under any ctest CWD.
//
// DELIBERATELY NOT testkit::test_paths, and the boundary is worth stating since
// "one resolver" is claimed for this tree. test_paths resolves REPO RESOURCES:
// give it a relative name, get the file. These two name the LIBRARY'S PUBLIC
// INCLUDE ROOT and its umbrella header -- a build-configuration fact that the
// tier contract below exists to check, not a resource lookup. Routing them
// through repo_file("atx-vol/include") would restate the build layout in the
// test and let the two drift apart silently, which is the failure this whole
// area is about; the answer would be to derive them from the atx-vol target's
// include property, which is a build-system change, not a path-helper one.
// Two anchors, two rules, on purpose.
fs::path include_root() { return fs::path{ATX_VOL_INCLUDE_ROOT}; }
fs::path umbrella_path() { return fs::path{ATX_VOL_UMBRELLA_HPP}; }

TEST(VolUmbrella, PublicSurfaceIsReachableAndConsistent) {
  using namespace atx::vol;

  // Core vocabulary (types.hpp) — including the de-duplicated PricingRoute.
  EXPECT_EQ(static_cast<int>(Side::Call), 0);
  EXPECT_EQ(static_cast<int>(PricingRoute::B76AlCache), 1);

  // European primitive (black76.hpp).
  const double px = black76_price(100.0, 100.0, 1.0, 0.2, 1.0, Side::Call);
  EXPECT_GT(px, 0.0);

  // Named preset factory (session.hpp — config / API layer).
  const SessionInputs in = make_session_inputs(FitPreset::Robust, 100.0, 0.03);
  EXPECT_EQ(in.calendar_repair, CalendarRepair::MonotoneFit);

  // Calibration-grade slice type (vol_surface.hpp) is reachable.
  EssviParams slice{};
  slice.theta = 0.04;
  EXPECT_GT(slice.theta, 0.0);

  // S4-T22: the portfolio stack's two risk post-processes are Tier-A, so the
  // one-include API can reach scenario P&L and P&L attribution. Naming a symbol
  // from each makes a future demotion fail to COMPILE rather than silently
  // shrink the frozen risk surface (the same discipline the E5 block uses).
  const ScenarioGridSpec grid_spec;
  EXPECT_EQ(grid_spec.taylor_radius_spot, kDefaultTaylorRadiusSpot);
  EXPECT_EQ(static_cast<int>(ScenarioRoute::Taylor), 0);
  const AttributionOptions attribution_opts;
  EXPECT_GT(attribution_opts.k_ref, 0.0);
}

// E5 / AN-W. The analytics flagship and its neighbours were absent from the
// umbrella, so the one-include public API could not reach
// `compute_surface_analytics`, the RND/BKM stack, the earnings/event-vol model,
// the vol-time clock, the SR tenor grid, dense slices, the strangle DSL or the
// strategy adapters. This test names a symbol from EACH of them, so a future
// re-ordering of vol.hpp that drops one fails to COMPILE here rather than
// silently shrinking the public surface again.
//
// S4-T18: the tearsheet / run_report assertions left this test with their
// headers — those are `atx-vol-tools` CLI-support headers now, outside the
// frozen umbrella surface.
TEST(VolUmbrella, AnalyticsAndReportingSurfaceIsReachable) {
  using namespace atx::vol;

  // analytics.hpp — the flagship bundle + E5's delta-convention contract.
  const AnalyticsConfig acfg;
  EXPECT_EQ(acfg.delta_convention, DeltaConvention::American);
  EXPECT_FALSE(acfg.tenors.tenors_years.empty());

  // event_vol.hpp — the SpiderRock event decomposition + E3a's joint-solve tag.
  EXPECT_EQ(static_cast<int>(EmoveMethod::TwoPillar), 0);
  EXPECT_GT(censored_total_variance(0.04, 1, 0.05), 0.0);

  // earnings_term_fit.hpp — the joint {eMove, st, lt, decay} fit vocabulary.
  const EarningsFitConfig efc;
  EXPECT_GT(efc.emove_hi, efc.emove_lo);

  // vol_time.hpp — the hybrid business/vol-time clock.
  EXPECT_GT(kCalendarYearNs, 0.0);

  // sr_tenor_grid.hpp — SpiderRock's 12-point native tenor grid.
  EXPECT_EQ(SrTenorGrid::kTradingDays.size(), std::size_t{12});
  EXPECT_EQ(SrTenorGrid::kTradingDays.front(), 5);

  // dense_slice.hpp — the densified convex slice fit.
  const ConvexFitOpts dopts;
  EXPECT_GE(kMaxIntervalSlackRows, 1);
  EXPECT_GE(sizeof dopts, sizeof(double));

  // dispersion_strangle.hpp — the strangle DSL.
  const DispersionStrangleConfig strangle;
  EXPECT_GE(sizeof strangle, sizeof(double));

  // strategy.hpp — the lifecycle/roll vocabulary the dispersion strategies use.
  const LifecycleSpec lifecycle;
  EXPECT_GE(sizeof lifecycle, sizeof(double));
}

// ── The tiering contract (S4-T18 / plan 3.8 + 4.1) ──────────────────────────

// THE gate: the umbrella's direct include list and the Tier-A manifest are the
// same set. Reported as two directed differences rather than one set compare so
// a failure says WHICH way the surface drifted — a shrunk shipped API and a
// widened frozen promise are opposite mistakes with opposite fixes.
TEST(VolUmbrella, UmbrellaIsExactlyTierA) {
  const std::vector<std::string> includes = direct_atx_vol_includes(umbrella_path());
  ASSERT_FALSE(includes.empty()) << "parsed no includes out of " << umbrella_path().string();

  const std::set<std::string> got(includes.begin(), includes.end());
  const std::set<std::string> want(std::begin(kTierA), std::end(kTierA));

  for (const std::string& w : want) {
    EXPECT_TRUE(got.count(w) == 1)
        << "Tier-A header missing from the umbrella (the shipped one-include API "
           "just shrank): "
        << w;
  }
  for (const std::string& g : got) {
    EXPECT_TRUE(want.count(g) == 1)
        << "umbrella includes a header that is NOT Tier-A (v1 would be freezing "
           "more than it promised): "
        << g;
  }
  // A duplicate include would leave the set compare green while the file is
  // wrong, so pin the count too.
  EXPECT_EQ(includes.size(), got.size()) << "duplicate include in the umbrella";
  EXPECT_EQ(got.size(), want.size());
}

// The umbrella may not reach into any non-shipped tier. Structural, so it keeps
// holding for headers added long after this task: every umbrella include must
// resolve under include/atx/vol/ and must not carry a detail/, tools/,
// research/ or support/ path segment.
TEST(VolUmbrella, UmbrellaAdmitsNoNonShippedTier) {
  const std::vector<std::string> includes = direct_atx_vol_includes(umbrella_path());
  ASSERT_FALSE(includes.empty());

  for (const std::string& inc : includes) {
    EXPECT_EQ(inc.find("atx/vol/detail/"), std::string::npos)
        << "umbrella exposes internal machinery: " << inc;
    EXPECT_EQ(inc.find("atx/vol/tools/"), std::string::npos)
        << "umbrella exposes a CLI-support header: " << inc;
    EXPECT_EQ(inc.find("atx/vol/research/"), std::string::npos)
        << "umbrella exposes a research orchestration header: " << inc;
    EXPECT_EQ(inc.find("support/"), std::string::npos)
        << "umbrella exposes a test fixture: " << inc;
    EXPECT_TRUE(fs::exists(include_root() / inc))
        << "umbrella include does not resolve under the public include root: " << inc;
  }
}

// Header-only, dependency-free auxiliaries that api_restructure_measure.py's
// `PROMOTED_PUBLIC` moved from `detail/` straight to `api/fitting/` on
// 2026-08-14 (Task 2 review, fix round 1) — NOT because a consumer reaches for
// them by name, but because several Tier-A headers below (american.hpp,
// backtest.hpp, session.hpp, surface_parity.hpp, pricer_fitter.hpp) already
// bare-`#include`d them with the private `"fitting/<name>.hpp"` spelling, and
// atx-options-engine (an external root) reaches them transitively through
// those same Tier-A headers. Pre-restructure they were under `detail/` and
// silently exempted by that tier's blanket escape; the promotion to `api/`
// (required for external reachability) took away that escape without
// changing what they are: no `.cpp`, no ODR/linkage surface, nothing but std
// includes of their own (docs/api-placement.md). Tier-B and public, but — like
// `api/simd/` — carrying no stability promise of their own, so a Tier-A header
// depending on either does not smuggle a frozen promise onto it.
constexpr std::string_view kTierBEscapeAuxiliaries[] = {
    "atx/vol/api/fitting/aggregate_arity.hpp",
    "atx/vol/api/fitting/prepared_policy.hpp",
};

// Tier-A is closed under inclusion: a frozen header's dependencies are frozen
// too, whether or not a caller names them. The only permitted escapes are the
// one generated detail/ header (version_generated.hpp, configure_file'd from
// project(VERSION) and included by version.hpp), the public api/simd/
// dispatch/kernel headers (e.g. priced_surface.hpp / priced_surface_view.hpp
// resolve their SIMD route through simd/cpu.hpp), and the two promoted-public
// auxiliaries in kTierBEscapeAuxiliaries above — none of which carries a
// stability promise of its own, so depending on any of them does not smuggle
// a Tier-B declaration into a frozen signature.
TEST(VolUmbrella, TierAIsClosedUnderInclusion) {
  const std::set<std::string> tier_a(std::begin(kTierA), std::end(kTierA));
  const std::set<std::string_view> escape_auxiliaries(std::begin(kTierBEscapeAuxiliaries),
                                                       std::end(kTierBEscapeAuxiliaries));

  for (const std::string& header : tier_a) {
    const fs::path path = include_root() / header;
    ASSERT_TRUE(fs::exists(path)) << "Tier-A header does not exist: " << header;
    for (const std::string& dep : direct_atx_vol_includes(path)) {
      if (dep.rfind("atx/vol/detail/", 0) == 0) continue;
      if (dep.rfind("atx/vol/api/simd/", 0) == 0) continue;
      if (escape_auxiliaries.count(dep) == 1) continue;
      EXPECT_TRUE(tier_a.count(dep) == 1)
          << header << " is Tier-A but depends on non-Tier-A " << dep
          << " — either promote " << dep << " to Tier-A, demote " << header
          << ", or break the dependency";
    }
  }
}

// ── The README tier table, parsed back off disk ─────────────────────────────
//
// `TierCountsMatchTheReadmeTable` below reads the Count cells out of the
// README's tier table (under `## API stability policy (1.x)`) and compares them
// to the live header tree. The table is therefore the ONLY place each of those
// numbers is written down, and it is machine-checked.
//
// It used to work the other way: three integer literals lived HERE, and the
// failure messages asked the reader to go update the README. That is a request,
// not a mechanism, and it failed every time it was tested. The README sentence
// restating the same triple had to be corrected at 222b379, 1e0b708, 738c9b4
// and once again after -- four times, always because someone fixed the copy
// they were shown and never learned the other existed.
//
// api-restructure (2026-08-14) changed WHAT is counted, not how it is known.
// The flat include/atx/vol/*.hpp + detail/ + simd/ layout this parser was
// written against is GONE: the public surface is include/atx/vol/api/, an
// 8-module tree (analytics, backtest, core, fitting, marketdata, pricing, simd,
// storage) plus the umbrella api/vol.hpp itself, and the private implementation
// moved to src/<module>/, off the include/ tree entirely. `detail/` is
// therefore no longer a tier this test can walk -- it carries no path under
// include/atx/vol/ at all -- so its row left the README table and its lookup
// left this file, leaving TWO rows counted off the SAME tree: Tier-A (`kTierA`,
// machine-checked above) and Tier-B (every other public header under api/).
// `s3.hpp` moved storage -> fitting in that same commit (Klassen S3/SSVI is a
// fitting-family curve, not AWS storage); a module reassignment changes no
// header's tier and no count here.
//
// These helpers exist to make every way the parse can go wrong a LOUD, SPECIFIC
// failure, which matters more than it looks. The literals had exactly one
// virtue: when one was wrong, it was wrong in public. A parser that quietly
// stops matching is strictly worse -- it reports success while checking
// nothing, which is the same shape as a grep that asked the wrong question and
// returned a clean all-clear. So there is no default, no "skip if absent" and
// no partial match: an unreadable file, a missing row, a DUPLICATE row and a
// cell with no integer are four distinct errors, each naming the file and what
// to go look at.
struct ReadmeCount {
  bool ok = false;
  std::size_t value = 0;
  std::string error;  // set iff !ok; phrased as something to go fix
};

// atx-vol/README.md. This IS a repo-resource lookup -- unlike include_root()
// above, which is why it goes through the shared resolver and that does not.
// It was `include_root() / ".." / "README.md"`: a second spelling of the same
// rule, riding on an anchor that means something else.
fs::path readme_path() { return testkit::repo_file("atx-vol/README.md"); }

std::string trim_cell(std::string_view s) {
  const std::size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return std::string{};
  return std::string{s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1)};
}

// `| a | b | c |` -> {"a", "b", "c"}. The outer pipes yield empty leading and
// trailing fields; dropping them makes cell indices match what a reader counts.
std::vector<std::string> markdown_cells(std::string_view line) {
  std::vector<std::string> cells;
  std::size_t start = 0;
  while (true) {
    const std::size_t bar = line.find('|', start);
    if (bar == std::string_view::npos) {
      cells.push_back(trim_cell(line.substr(start)));
      break;
    }
    cells.push_back(trim_cell(line.substr(start, bar - start)));
    start = bar + 1;
  }
  if (!cells.empty() && cells.front().empty()) cells.erase(cells.begin());
  if (!cells.empty() && cells.back().empty()) cells.pop_back();
  return cells;
}

// LEADING integer only: "34 + 9" -> 34, "31 (+1 generated)" -> 31. Neither
// remaining Count cell carries such an annotation today -- the two that did
// were the `simd/`-inclusive Tier-B row and the `detail/` row, both retired by
// the api-restructure -- but the rule stays deliberately: a tier whose count
// needs a caveat must be able to write one in the table without turning this
// parse into a green no-op.
bool leading_uint(std::string_view cell, std::size_t& out) {
  const std::size_t p = cell.find_first_of("0123456789");
  if (p == std::string_view::npos) return false;
  std::size_t v = 0;
  for (std::size_t i = p; i < cell.size() && cell[i] >= '0' && cell[i] <= '9'; ++i) {
    v = v * 10 + static_cast<std::size_t>(cell[i] - '0');
  }
  out = v;
  return true;
}

// The Count cell of the tier-table row whose FIRST cell names `label`.
ReadmeCount readme_tier_count(std::string_view label) {
  ReadmeCount out;
  const fs::path path = readme_path();
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    out.error = "cannot open " + path.string() + " to read the tier table";
    return out;
  }

  std::string line;
  std::vector<std::string> counts;  // one per matching row, to detect ambiguity
  std::size_t table_rows = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line.front() != '|') continue;
    ++table_rows;
    const std::vector<std::string> cells = markdown_cells(line);
    if (cells.size() < 3) continue;
    if (cells[0].find(label) == std::string::npos) continue;
    counts.push_back(cells[2]);
  }

  const std::string what{label};
  if (counts.empty()) {
    out.error = "no table row in " + path.string() + " has a first cell naming `" + what +
                "` (" + std::to_string(table_rows) +
                " table rows scanned) -- the tier table moved, lost that row, or "
                "renamed it; this test reads the count FROM that row, so it cannot "
                "fall back to a literal";
    return out;
  }
  if (counts.size() > 1) {
    out.error = std::to_string(counts.size()) + " table rows in " + path.string() +
                " have a first cell naming `" + what +
                "`, so which one states the count is ambiguous -- make the tier-table "
                "row the only one";
    return out;
  }
  if (!leading_uint(counts.front(), out.value)) {
    out.error = "the Count cell for `" + what + "` in " + path.string() + " is \"" +
                counts.front() + "\", which has no leading integer";
    return out;
  }
  out.ok = true;
  return out;
}

// AND RUN `-R VolUmbrella`, WHATEVER YOUR TASK'S OWN FILTER IS. These counts are
// a GLOBAL invariant that fires on any header addition, so no topical filter
// covers them and every locally-correct verification misses them. That is how
// this pin broke twice in one day: 3c28ffd took Tier-B 31 -> 32 behind
// `-R CboeStrip`, b1558f7 took `detail/` 30 -> 31 behind
// `-R "^(Arb|SplineVol|...)"`, each lane running precisely the filter it was
// given. The instruction lives here rather than in a brief because a brief that
// has to remember is a brief that will eventually forget.
//
// There is no longer an UPDATE PROCEDURE to follow. Adding or promoting a header
// means editing the README tier table, and that is the whole of it -- this test
// has no count literal left to keep in step with it.
//
// The api/ module directories are NAMED rather than counted, for the same
// reason. `module_dirs == 8` is a restated count that goes stale the way the
// tier triple did, and its failure text cannot say WHICH module appeared or
// vanished; a set compare against this manifest can, and a module rename shows
// up as one added plus one removed instead of a silent pass.
constexpr std::string_view kApiModules[] = {
    "analytics", "backtest", "core", "fitting",
    "marketdata", "pricing", "simd", "storage",
};

TEST(VolUmbrella, TierCountsMatchTheReadmeTable) {
  const ReadmeCount tier_a_row = readme_tier_count("Tier-A");
  const ReadmeCount tier_b_row = readme_tier_count("Tier-B");

  // ASSERT rather than EXPECT: with no table there is nothing to compare
  // against, and the EXPECTs below would each report the same one fault.
  ASSERT_TRUE(tier_a_row.ok) << tier_a_row.error;
  ASSERT_TRUE(tier_b_row.ok) << tier_b_row.error;

  EXPECT_EQ(std::size(kTierA), tier_a_row.value)
      << "the README tier table says Tier-A is " << tier_a_row.value
      << " but the umbrella manifest `kTierA` holds " << std::size(kTierA)
      << " -- they are the same promise, so fix whichever is wrong";

  // Recursive: the public surface is an 8-module tree under api/, not a flat
  // directory -- a non-recursive scan would silently undercount to just the
  // umbrella header itself.
  std::size_t api_headers = 0;
  std::set<std::string> module_dirs;
  const fs::path api_root = include_root() / "atx" / "vol" / "api";
  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(api_root)) {
    if (entry.is_directory()) {
      if (entry.path().parent_path() == api_root) {
        module_dirs.insert(entry.path().filename().string());
      }
      continue;
    }
    if (entry.is_regular_file() && entry.path().extension() == ".hpp") ++api_headers;
  }

  std::set<std::string> want_modules;
  for (const std::string_view name : kApiModules) want_modules.emplace(name);
  EXPECT_EQ(module_dirs, want_modules)
      << "the api/ module tree changed shape -- a module was added, removed or "
         "renamed, so update `kApiModules` and the README's tier table together";

  // Tier-B = every public header minus the Tier-A ones minus vol.hpp itself.
  // This is also what pins the api/ total: a header appearing anywhere under
  // api/ moves `tier_b` and mismatches the table, so no separate total is
  // written down here.
  ASSERT_GT(api_headers, std::size(kTierA) + 1u);
  const std::size_t tier_b = api_headers - std::size(kTierA) - 1u;
  EXPECT_EQ(tier_b, tier_b_row.value)
      << "the README tier table says Tier-B is " << tier_b_row.value
      << " but include/atx/vol/api/ holds " << api_headers << " .hpp, minus "
      << std::size(kTierA) << " Tier-A minus vol.hpp = " << tier_b;
}

// ── The demoted legacy surface containers (S4-T21 / plan 4.4) ───────────────
//
// atx-vol grew four hand-duplicated "stack of fitted slices + linear-in-total-
// variance time interpolation" containers before `CurveSurface` unified them
// (see the vol_curve.hpp file header). Plan 4.4 keeps ONE canonical pipeline —
// CurveSurface (fit) -> PricedSurface / PricedSurfaceView (serve) -> SurfaceSet
// (portfolio) — and demotes the per-family leftovers to `detail/`, where they
// carry no stability promise.
//
// The demotion is at the TYPE level, and this is where it is enforced: a public
// header under include/atx/vol/api/ may not NAME them at all, in code or in
// prose. Internal code (src/, tests) may keep using them; that is what the
// demotion means. Naming one in a public header again — even in a comment —
// re-exposes it to a caller reading the shipped API and fails here.
constexpr std::string_view kDemotedSurfaceTypes[] = {
    "Surface<",       // Surface<Slice> + its SviSurface / EssviSurface aliases
    "SviSurface",     //   (the aliases are also caught on their own names, so a
    "EssviSurface",   //    using-declaration cannot smuggle one back in)
    "C8Surface", "CStarSurface",
};

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << "cannot open " << path.string();
  return std::string{std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>()};
}

TEST(VolUmbrella, DemotedSurfaceContainersAreNotNamedInPublicHeaders) {
  const fs::path public_dir = include_root() / "atx" / "vol" / "api";
  ASSERT_TRUE(fs::is_directory(public_dir)) << public_dir.string();

  // Recursive: the public surface is an 8-module tree under api/ (analytics,
  // backtest, core, fitting, marketdata, pricing, simd, storage), not a flat
  // directory -- a non-recursive scan here finds 0 files and silently
  // unenforces the ban entirely. Private implementation lives under
  // src/<module>/, off this tree, which is exactly where the demoted
  // containers are allowed to live now (the old detail/ tier's role).
  std::size_t headers_scanned = 0;
  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(public_dir)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".hpp") continue;
    ++headers_scanned;
    const std::string text = read_file(entry.path());
    for (const std::string_view type : kDemotedSurfaceTypes) {
      EXPECT_EQ(text.find(type), std::string::npos)
          << entry.path().filename().string() << " names the demoted container `"
          << type << "` — plan 4.4 moved it to src/<module>/ (internal). Use the "
             "canonical pipeline (CurveSurface -> PricedSurface / "
             "PricedSurfaceView -> SurfaceSet) in the public API, and reach for "
             "the demoted type from internal code only.";
    }
  }
  // Derived, not restated. A literal here is a THIRD copy of the api/ total,
  // sitting outside every conflict region and outside any topical test filter --
  // exactly the copy an api/ change forgets to update, and it was already stale
  // (75) when three headers moved into api/. The README tier table owns the
  // number; this reads it back the same way TierCountsMatchTheReadmeTable does,
  // so a non-recursive walk (which finds only vol.hpp and would silently
  // unenforce the ban entirely) still fails loudly.
  const ReadmeCount tier_b_row = readme_tier_count("Tier-B");
  ASSERT_TRUE(tier_b_row.ok) << tier_b_row.error;
  EXPECT_EQ(headers_scanned, std::size(kTierA) + tier_b_row.value + 1u)
      << "public header scan did not walk the full api/ tree: " << headers_scanned
      << " headers seen, but the README tier table accounts for "
      << std::size(kTierA) << " Tier-A + " << tier_b_row.value << " Tier-B + vol.hpp";
}

// ── No second way to resolve a fixture path ─────────────────────────────────
//
// FRI-072 retired six hand-pasted relative "ladders" onto testkit::test_paths.
// TestPathing pins that those resolvers are CWD-independent -- but NOT that this
// tree resolves through them, so pasting a fresh ladder into any *_test.cpp
// leaves TestPathing green. The property that was actually defective is "a
// second mechanism exists", and that is what this checks.
//
// Same mechanism as TierCountsMatchTheReadmeTable above: read the real files
// back off disk at run time rather than trusting a list someone maintains by
// hand. A ladder rung is a string literal opening with "../", and the other
// retired shape derived a path from __FILE__, which is not reliably absolute
// under ninja/clang-cl and was wrong in every copy that used it.
//
// Two things are deliberately NOT failures:
//   * `#include "../src/foo.hpp"` -- a compile-time include path, resolved by
//     the preprocessor against the source directory, never against the CWD.
//   * kPathLiteralExempt -- files that pass "../" to code as DATA rather than
//     using it as a path. Adding to this list is the loud, deliberate step: it
//     is where a reviewer gets asked whether a new ladder is really data.
//
// WHAT THIS GUARD CANNOT SEE -- stated because the first version of it could not
// see EITHER defect that motivated it, and a guard documenting only its
// deliberate non-failures is claiming completeness by omission:
//
//   * A BARE-RELATIVE root: `fs::path{"artifact-cache"}`, `"data/spy_ytd/opra"`.
//     This is the exact shape that forked the artifact cache five ways, and a
//     text scan cannot separate it from any other short string. Nothing here
//     will catch it; only TestPathing's CWD flip and a hostile-directory run
//     will.
//   * A path assembled at run time from fragments, or read from a variable.
//   * A path inside a RAW STRING literal, R"(...)". The extractor is raw-string
//     AWARE, which is not the same as raw-string COVERED: it recognises them so
//     an embedded quote cannot desync the scan, then skips the contents
//     entirely. Skipping is right -- raw strings here hold doc-strings and
//     embedded scripts, not path lookups -- but an absolute path written inside
//     one is invisible to every rule below. Awareness read as coverage is
//     exactly the gap R3-I2 was, so it is listed rather than assumed.
//   * `std::filesystem::current_path()` OUTSIDE atx-vol/tests (checked within).
//   * Non-.cpp/.hpp/.h/.cc files -- CMake, Python, scripts.
//   * The ladder/macro needles are scoped to atx-vol/tests only, deliberately:
//     a relative CLI default in examples/ is sanctioned behaviour, not a defect,
//     so a repo-wide rung rule would be wrong. The ABSOLUTE-literal needle is
//     repo-wide, because a path into another checkout is never acceptable
//     anywhere -- that asymmetry is the point, and it is what lets this catch
//     the third-checkout absolute that lived in examples/.
constexpr std::string_view kPathLiteralExempt[] = {
    // Feeds "../escape" to a partition-name validator to prove it is REJECTED.
    // The literal is the adversary, not a lookup.
    "surface_db_test.cpp",
};

// Files permitted to call current_path(). Only the test that exists to flip the
// working directory and flip it back.
constexpr std::string_view kCurrentPathExempt[] = {
    "test_paths_test.cpp",
};

// THE EXEMPTIONS ARE ASYMMETRIC, AND THAT IS THE GUARD'S STRONGEST PROPERTY.
// Only the rung and current_path() needles have an exemption path at all. There
// is NO list that can silence the compiler-file-macro needle or the
// absolute-literal needle -- not for any file, including this one. So the two
// rules with no legitimate exception in this repository cannot be opted out of
// by adding a name to an array, which is the cheapest way a guard normally dies.

// Needles ASSEMBLED rather than written, so this file does not match its own
// patterns. Self-non-matching rests on TWO mechanisms and both are load-bearing:
// this assembly, and the comment-skip in scan_source_line -- the prose above
// carries every one of these patterns verbatim. Remove the comment-skip and this
// file starts flagging its own explanation.
//
// Exempting this file by name instead would be worse: it is a *_test.cpp like
// any other and must be policed like one.
struct PathNeedles {
  std::string file_macro;    // a path derived from the compiler's file macro
  std::string cwd_call;      // an explicit working-directory read/change
  std::string accepted_root; // the ONE blessed absolute root (see below)

  static PathNeedles make() {
    PathNeedles n;
    n.file_macro = std::string("__FI") + "LE__";
    n.cwd_call = std::string("current_") + "path(";
    // Every absolute literal in atx-vol today resolves inside this external
    // vendor hive, which sits OUTSIDE any checkout. Stated as a positive
    // allowance rather than a per-file allowlist so a path into a sibling
    // worktree fails wherever it is written -- the earlier sweeps searched for
    // "C:/atx/" WITH the trailing slash, and that slash is precisely what
    // excluded C:/atx-wt/... . A pattern shaped by the last defect inherits
    // that defect's blind spot.
    //
    // It is a ROOT, not a prefix: membership is decided by normalising the
    // literal and comparing components (testkit::path_is_under), so a traversal
    // out of it cannot be spelled back in.
    n.accepted_root = std::string("C:") + "/atx-data";
    return n;
  }
};

// One string literal as the COMPILER sees it: escapes decoded, adjacent
// literals merged. Merging matters -- `"C:" "/atx-wt/..."` splits the drive
// letter so no single fragment looks absolute, and the deleted defect was itself
// split across two lines, caught only because the split fell after the drive.
struct SourceLiteral {
  std::string text;
  std::size_t line = 0;
  bool preprocessor = false;  // part of an #include etc.
};

// Extract string literals from C++ source.
//
// Comment-, char-literal- and raw-string-aware, because this tree contains all
// three, and a scanner that desyncs on one silently stops reporting -- the
// failure mode that has bitten six instruments on this task. Every hazard has a
// positive control in PathOriginDetectorCatchesWhatItClaimsTo, and those
// controls, not this comment, are the evidence the handling works.
//
// This comment used to carry a census ("21 files with raw strings"). It was
// wrong: that came from a loose `R"` substring, which matches the tail of
// SYNTHATTR, CZR and AFTER. Requiring a non-identifier character before it
// still counted this file's own probe -- a string ENDING in R. Only a pattern
// requiring the `R"delim(` form is right, and the true answer is one file.
// Three patterns, two wrong, in the lane that spent five rounds on precisely
// this. So the counts are gone rather than corrected: a hand-kept census beside
// the code it describes is the same defect as a hand-kept tier count, and the
// remedy 481377e established is to machine-check a number or not state it.
[[nodiscard]] std::vector<SourceLiteral> extract_string_literals(const std::string& src) {
  std::vector<SourceLiteral> out;
  std::size_t line = 1;
  bool at_line_start = true;
  bool pp_line = false;
  bool adjacent = false;  // previous token was a literal: the next one merges

  for (std::size_t i = 0; i < src.size();) {
    const char c = src[i];
    if (c == '\n') {
      ++line;
      at_line_start = true;
      pp_line = false;
      ++i;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      while (i < src.size() && src[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
        if (src[i] == '\n') ++line;
        ++i;
      }
      i = i + 2 < src.size() ? i + 2 : src.size();
      continue;
    }
    if (at_line_start && c == '#') pp_line = true;

    if (c == '\'') {
      // A C++ DIGIT SEPARATOR (1'000'000), not a char literal. They are common
      // throughout this tree, and treating one as a quote makes the scanner
      // swallow everything to the next apostrophe: it under-counted one file's
      // lines by 192 and still passed every threshold, because a desynced
      // scanner reports fewer findings rather than an error. No prefixed char
      // literals (L'x', u8'x') exist here, so a preceding alphanumeric always
      // means separator.
      //
      // This said "67 files" when written and the true figure was 68 before the
      // task finished -- a hand-kept census rotting inside the task that wrote
      // it. The digit-separator control in PathOriginDetectorCatchesWhatItClaimsTo
      // is what actually holds; a count in a comment never did.
      if (i > 0 && std::isalnum(static_cast<unsigned char>(src[i - 1])) != 0) {
        adjacent = false;
        at_line_start = false;
        ++i;
        continue;
      }
      // A char literal is at most a few characters. Bounding the scan makes any
      // future desync self-limiting instead of silently eating the rest of a
      // file, and newlines are counted so line attribution survives one.
      const std::size_t limit = i + 8 < src.size() ? i + 8 : src.size();
      std::size_t j = i + 1;
      while (j < limit && src[j] != '\'') j += (src[j] == '\\') ? 2 : 1;
      if (j < limit && src[j] == '\'') {
        for (std::size_t k = i; k <= j; ++k) {
          if (src[k] == '\n') ++line;
        }
        i = j + 1;
      } else {
        ++i;  // not a char literal after all: ordinary punctuation
      }
      adjacent = false;
      at_line_start = false;
      continue;
    }
    if (c == 'R' && i + 1 < src.size() && src[i + 1] == '"') {  // raw string
      const std::size_t open = src.find('(', i + 2);
      if (open == std::string::npos) break;
      const std::string close = ")" + src.substr(i + 2, open - (i + 2)) + "\"";
      const std::size_t end = src.find(close, open + 1);
      const std::size_t stop = end == std::string::npos ? src.size() : end + close.size();
      for (std::size_t k = i; k < stop; ++k) {
        if (src[k] == '\n') ++line;
      }
      i = stop;
      adjacent = false;
      at_line_start = false;
      continue;
    }
    if (c == '"') {
      std::string text;
      ++i;
      while (i < src.size() && src[i] != '"') {
        if (src[i] == '\\' && i + 1 < src.size()) {
          text.push_back(src[i + 1]);  // \\ -> \ , \" -> " ; enough for paths
          i += 2;
        } else {
          if (src[i] == '\n') ++line;
          text.push_back(src[i]);
          ++i;
        }
      }
      ++i;
      if (adjacent && !out.empty()) {
        out.back().text += text;
      } else {
        out.push_back(SourceLiteral{text, line, pp_line});
      }
      adjacent = true;
      at_line_start = false;
      continue;
    }
    adjacent = false;
    at_line_start = false;
    ++i;
  }
  return out;
}

// True when a literal's CONTENT is an absolute path: a drive-letter root
// (`C:/`, `D:\`) or a rooted/UNC backslash. Names no drive and no checkout.
[[nodiscard]] bool literal_is_absolute(const std::string& t) {
  if (t.size() >= 3 && std::isalpha(static_cast<unsigned char>(t[0])) != 0 && t[1] == ':' &&
      (t[2] == '/' || t[2] == '\\')) {
    return true;
  }
  return t.size() >= 2 && t[0] == '\\' && t[1] == '\\';
}

[[nodiscard]] bool literal_is_rung(const std::string& t) {
  // Assembled like every other pattern here: written out, these two would be
  // real rung literals in this file, and the whole-tree walk would flag the very
  // rule that defines them.
  static const std::string up_fwd = std::string("..") + "/";
  static const std::string up_bck = std::string("..") + "\\";
  return t.rfind(up_fwd, 0) == 0 || t.rfind(up_bck, 0) == 0;
}

struct LineScan {
  std::vector<std::string> violations;  // empty => clean
};

// Token-level rules. These are identifiers, not literals, so they stay
// line-based; the literal-level rules live in literal_violations below.
[[nodiscard]] LineScan scan_source_line(const std::string& line, const PathNeedles& n,
                                        bool check_macro, bool check_cwd) {
  LineScan out;
  const std::size_t first = line.find_first_not_of(" \t");
  if (first == std::string::npos) return out;
  if (line.compare(first, 2, "//") == 0) return out;  // prose, not code
  if (check_macro && line.find(n.file_macro) != std::string::npos) {
    out.violations.emplace_back("path derived from the compiler file macro");
  }
  if (check_cwd && line.find(n.cwd_call) != std::string::npos) {
    out.violations.emplace_back("working-directory call");
  }
  return out;
}

// Literal-level rules, asking WHERE A PATH RESOLVES rather than how it is
// SPELLED -- and `..` is exactly where those two stop agreeing.
//
// The accepted external data root is compared after normalisation, so
// "<root>/../../atx/x" -- spelled as if it were under the root, resolving
// outside it -- is rejected. The previous text-prefix compare accepted exactly
// that and reconstructed the original FRI-072 path straight through the guard
// built to stop it: twelve characters of prefix authorising an unbounded suffix.
// A per-file allowlist at least enumerates its escapes; that one was silent.
[[nodiscard]] LineScan literal_violations(const SourceLiteral& lit, const fs::path& accepted_root,
                                          bool check_ladder) {
  LineScan out;
  if (lit.preprocessor) return out;  // an #include is a compile-time path
  if (check_ladder && literal_is_rung(lit.text)) {
    out.violations.emplace_back("parent-directory rung");
  }
  if (literal_is_absolute(lit.text) &&
      !testkit::path_is_under(fs::path{lit.text}, accepted_root)) {
    out.violations.emplace_back("absolute path literal resolving outside the accepted data root");
  }
  return out;
}

// The detector's own positive controls. Every needle is fired here on a
// constructed line, so none of them can rot into a pattern that matches nothing
// -- the failure mode a scan cannot report about itself. The `__FILE__` needle
// especially: unlike the rung, it has NO in-tree instance left, so without this
// it would have no evidence at all that it still works.
//
// The probe lines are assembled from the needles for the same reason the needles
// are assembled: writing them out would make this file match itself.
TEST(VolUmbrella, PathOriginDetectorCatchesWhatItClaimsTo) {
  const PathNeedles n = PathNeedles::make();
  const fs::path root{n.accepted_root};
  // Probe fragments that are NOT violations on their own. Every probe below is
  // built from these, because a probe written out whole would be a real literal
  // in this file and the whole-tree walk would flag this test's own evidence.
  // `..` is not a rung until a separator follows; `C:` is not absolute until a
  // separator follows; the backslashes come from a char, not from source text.
  const std::string q = "\"";
  const std::string dotdot = "..";
  const std::string drive = "C:";
  const std::string bs(2, '\\');  // source "\\" -> one backslash of content

  // Literal-level rules. `src` is C++ source text; the verdict is on the merged,
  // escape-decoded literals the compiler would see.
  const auto lits = [&root](const std::string& src, bool ladder = true) {
    std::size_t n_viol = 0;
    for (const SourceLiteral& l : extract_string_literals(src)) {
      n_viol += literal_violations(l, root, ladder).violations.size();
    }
    return n_viol;
  };
  const auto tokens = [&n](const std::string& line) {
    return scan_source_line(line, n, /*check_macro=*/true, /*check_cwd=*/true).violations.size();
  };

  // ── Positives ──────────────────────────────────────────────────────────────
  EXPECT_EQ(lits("  const fs::path p{" + q + dotdot + "/data/x.tsv" + q + "};"), 1u)
      << "the parent-directory rung rule no longer fires";
  EXPECT_EQ(tokens("  return fs::path(" + n.file_macro + ").parent_path();"), 1u)
      << "the compiler-file-macro rule no longer fires";
  EXPECT_EQ(tokens("  fs::" + n.cwd_call + "dir);"), 1u)
      << "the working-directory rule no longer fires";
  EXPECT_EQ(lits("  const char* p = " + q + drive + "/atx-wt/other/x.parquet" + q + ";"), 1u)
      << "a cross-checkout absolute is not caught";
  // Four source backslashes decode to the two that make a UNC root.
  EXPECT_EQ(lits("  const char* p = " + q + bs + bs + "srv" + bs + "share" + q + ";"), 1u)
      << "a UNC/rooted-backslash absolute is not caught";

  // R3-I1: spelled as if under the accepted root, RESOLVES outside it. The
  // second reconstructs the original FRI-072 path. A prefix compare passed both.
  EXPECT_EQ(lits("  const char* p = " + q + n.accepted_root + "/../atx-wt/pool-3/x.tsv" + q + ";"),
            1u)
      << "traversal escape: one level out of the accepted root is not caught";
  EXPECT_EQ(lits("  const char* p = " + q + n.accepted_root +
                 "/../../atx/atx-vol/tests/support/oracle_pde_golden.tsv" + q + ";"),
            1u)
      << "traversal escape: the ORIGINAL FRI-072 path walks through the guard";

  // R3-I2: the drive letter split across adjacent literals, which the compiler
  // concatenates. Neither fragment looks absolute on its own.
  EXPECT_EQ(lits("  const char* p = " + q + drive + q + " " + q + "/atx-wt/other/x" + q + ";"), 1u)
      << "adjacent-literal concatenation hides the drive letter";
  EXPECT_EQ(lits("  const char* p = " + q + "C" + q + " " + q + ":/atx-wt/other/x" + q + ";"), 1u)
      << "adjacent-literal concatenation splitting after the drive letter alone";

  // ── Negatives, each for a stated reason ────────────────────────────────────
  EXPECT_EQ(lits("#include " + q + dotdot + "/src/foo.hpp" + q), 0u)
      << "an #include is a compile-time path, never resolved against the CWD";
  EXPECT_EQ(lits("  // prose naming ../ and " + n.accepted_root + "/../x"), 0u)
      << "comments are prose; this file's own explanation names every pattern";
  EXPECT_EQ(lits("  /* block prose naming ../ and a " + q + " quote */"), 0u)
      << "block comments must not desync the extractor";
  EXPECT_EQ(lits("  const char* p = " + q + n.accepted_root + "/spy-dispersion/opra" + q + ";"), 0u)
      << "the external vendor hive is the one accepted absolute root";
  EXPECT_EQ(lits("  const char* p = " + q + n.accepted_root + "/a/../b/x" + q + ";"), 0u)
      << "traversal that stays inside the accepted root is fine";
  EXPECT_EQ(lits("  std::printf(" + q + "two readings:" + bs.substr(0, 1) + "n" + q + ");"), 0u)
      << "a printf escape is not a drive letter";
  EXPECT_EQ(lits("  const char* u = " + q + "https://example.invalid/x" + q + ";"), 0u)
      << "a URL scheme is not a drive letter";
  EXPECT_EQ(
      lits("  const char c = '" + q + "'; const char* p = " + q + dotdot + "/x" + q + ";", false),
      0u)
      << "a quote inside a CHAR literal must not desync the extractor";
  EXPECT_EQ(lits("  auto s = R" + q + "(raw with " + q + " and ../ inside)" + q + ";"), 0u)
      << "a raw string must not desync the extractor";

  // The extractor itself: merging and line attribution are what the rules ride
  // on, so they are asserted directly rather than inferred from the verdicts.
  const auto merged = extract_string_literals(q + drive + q + " " + q + "/x" + q + ";");
  ASSERT_EQ(merged.size(), 1u) << "adjacent literals were not merged into one";
  EXPECT_EQ(merged[0].text, drive + "/x");
  const auto two_lines = extract_string_literals("int a;\nconst char* p = " + q + "x" + q + ";");
  ASSERT_EQ(two_lines.size(), 1u);
  EXPECT_EQ(two_lines[0].line, 2u) << "line attribution is off; failures would misreport";

  // A C++ digit separator must not be read as a char literal. Mis-reading it
  // swallowed everything to the next apostrophe and cost 192 lines of line
  // attribution in a real file, while every count threshold still passed.
  const auto sep = extract_string_literals("int x = 1'000'000;\nconst char* p = " + q + "y" + q +
                                           ";");
  ASSERT_EQ(sep.size(), 1u) << "a digit separator desynced the extractor and hid a literal";
  EXPECT_EQ(sep[0].text, "y");
  EXPECT_EQ(sep[0].line, 2u) << "a digit separator swallowed a newline";

  // An unmatched apostrophe must not eat the rest of the file.
  const auto stray = extract_string_literals("int a; ' \nconst char* p = " + q + "z" + q + ";");
  ASSERT_EQ(stray.size(), 1u) << "a stray apostrophe swallowed the following literal";
  EXPECT_EQ(stray[0].line, 2u);
}

TEST(VolUmbrella, NoFixturePathResolvedOutsideTheSharedResolver) {
  const fs::path tests = testkit::tests_root();
  const fs::path scope = testkit::repo_file("atx-vol");
  ASSERT_TRUE(fs::is_directory(tests)) << "tests root does not resolve: " << tests.string();
  ASSERT_TRUE(fs::is_directory(scope)) << "atx-vol root does not resolve: " << scope.string();

  const PathNeedles n = PathNeedles::make();
  const fs::path accepted_root{n.accepted_root};
  std::size_t files_scanned = 0;
  std::size_t tests_scanned = 0;
  std::size_t literals_seen = 0;
  std::size_t include_rungs_skipped = 0;

  const auto report = [](const std::string& name, std::size_t ln, const std::string& v) {
    ADD_FAILURE() << name << ":" << ln << " -- " << v
                  << ". A path that resolves against the process working directory, or into "
                     "another checkout, names a different file depending on where the binary "
                     "was launched from. Resolve it through tests/support/test_paths.hpp "
                     "(test_fixture / test_data / market_data / repo_file / "
                     "artifact_cache_root). If the literal is DATA rather than a path, add "
                     "the file to kPathLiteralExempt and say why.";
  };

  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(scope)) {
    if (!entry.is_regular_file()) continue;
    const fs::path& p = entry.path();
    const std::string ext = p.extension().string();
    if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc") continue;
    // Vendored third-party: not ours to restyle, and it resolves nothing here.
    if (p.generic_string().find("/bench/thirdparty/") != std::string::npos) continue;

    const std::string name = p.filename().string();
    const bool in_tests = p.generic_string().rfind(tests.generic_string(), 0) == 0;

    bool ladder_exempt = false;
    for (const std::string_view e : kPathLiteralExempt) {
      if (name == e) ladder_exempt = true;
    }
    bool cwd_exempt = false;
    for (const std::string_view e : kCurrentPathExempt) {
      if (name == e) cwd_exempt = true;
    }

    ++files_scanned;
    if (in_tests) ++tests_scanned;

    std::ifstream in(p);
    const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    for (const SourceLiteral& lit : extract_string_literals(src)) {
      ++literals_seen;
      if (lit.preprocessor && literal_is_rung(lit.text)) ++include_rungs_skipped;
      for (const std::string& v :
           literal_violations(lit, accepted_root, in_tests && !ladder_exempt).violations) {
        report(name, lit.line, v);
      }
    }

    std::istringstream lines(src);
    std::string line;
    for (std::size_t ln = 1; std::getline(lines, line); ++ln) {
      for (const std::string& v :
           scan_source_line(line, n, in_tests, in_tests && !cwd_exempt).violations) {
        report(name, ln, v);
      }
    }
  }

  // Non-vacuity in four directions. A scanner that silently matched nothing --
  // because the walk broke, or the extractor desynced and returned no literals --
  // would otherwise report this contract satisfied, which is the exact shape the
  // tier-table comment above exists to prevent, and the shape five instruments
  // failed in on this task.
  EXPECT_GT(files_scanned, 200u) << "atx-vol scan found almost nothing; the walk is broken";
  EXPECT_GT(tests_scanned, 100u) << "the tests subtree was not reached by the walk";
  EXPECT_GT(literals_seen, 5000u) << "the literal extractor returned almost nothing; it desynced";
  EXPECT_GT(include_rungs_skipped, 0u)
      << "no #include carrying a parent-directory rung was seen, so the exclusion is untested "
         "and this contract may be passing because the scan matched nothing";
}

}  // namespace
