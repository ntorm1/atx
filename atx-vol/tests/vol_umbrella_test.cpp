#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
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
#include "atx/vol/vol.hpp"

namespace {

namespace fs = std::filesystem;

// ── The Tier-A manifest ─────────────────────────────────────────────────────
//
// Tier-A = the shipped, frozen v1 surface, and exactly what `vol.hpp` includes.
// It is CLOSED UNDER INCLUSION (TierAIsClosedUnderInclusion below): if a Tier-A
// header includes another `atx/vol/` header, that header is Tier-A too, or it
// is internal (`detail/`, `simd/`). Several entries here are on the list for
// precisely that reason rather than because a caller reaches for them directly
// — e.g. query_pricing.hpp is pulled in by backtest / portfolio_pricer /
// priced_surface / session, so its declarations are frozen whether or not it is
// named. Pretending otherwise would be the dishonest tiering.
//
// Everything else under include/atx/vol/ is Tier-B: public and includable, but
// deliberately OUTSIDE the frozen umbrella (advanced calibrators, the SoA/SIMD
// batch kernels, the dispersion domain vocabulary, harness-facing
// panels/fixtures).
constexpr std::string_view kTierA[] = {
    "atx/vol/adjusted_greeks.hpp",
    "atx/vol/american.hpp",
    "atx/vol/american_iv.hpp",
    "atx/vol/analytics.hpp",
    "atx/vol/arb.hpp",
    "atx/vol/backtest.hpp",
    "atx/vol/black76.hpp",
    "atx/vol/c8.hpp",
    "atx/vol/calib.hpp",
    "atx/vol/chain.hpp",
    "atx/vol/contract_projection.hpp",
    "atx/vol/corpus.hpp",
    "atx/vol/correction.hpp",
    "atx/vol/curve_fit.hpp",
    "atx/vol/curve_selector.hpp",
    "atx/vol/data.hpp",
    "atx/vol/deamer.hpp",
    "atx/vol/dense_slice.hpp",
    "atx/vol/derivatives.hpp",
    "atx/vol/dispersion.hpp",
    "atx/vol/dispersion_strangle.hpp",
    "atx/vol/dividend.hpp",
    "atx/vol/earnings_term_fit.hpp",
    "atx/vol/event_vol.hpp",
    "atx/vol/fit_metrics.hpp",
    "atx/vol/fit_policy.hpp",
    "atx/vol/greeks.hpp",
    "atx/vol/implied_vol.hpp",
    "atx/vol/market_env.hpp",
    "atx/vol/opra_panel.hpp",
    "atx/vol/parity.hpp",
    "atx/vol/pnl_attribution.hpp",
    "atx/vol/portfolio_pricer.hpp",
    "atx/vol/priced_surface.hpp",
    "atx/vol/priced_surface_view.hpp",
    "atx/vol/pricer_fitter.hpp",
    "atx/vol/profile.hpp",
    "atx/vol/projection.hpp",
    "atx/vol/query_pricing.hpp",
    "atx/vol/rates_curve.hpp",
    "atx/vol/scenario_grid.hpp",
    "atx/vol/session.hpp",
    "atx/vol/spline_curve.hpp",
    "atx/vol/sr_tenor_grid.hpp",
    "atx/vol/strategy.hpp",
    "atx/vol/surface.hpp",
    "atx/vol/surface_archive.hpp",
    "atx/vol/surface_db.hpp",
    "atx/vol/surface_parity.hpp",
    "atx/vol/surface_policy.hpp",
    "atx/vol/types.hpp",
    "atx/vol/universe.hpp",
    "atx/vol/version.hpp",
    "atx/vol/vol_curve.hpp",
    "atx/vol/vol_surface.hpp",
    "atx/vol/vol_time.hpp",
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
    // umbrella's own header comment quotes `#include "atx/vol/vol.hpp"`.
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
// (same pattern as ATX_AMZN_FIXTURE) so the contract holds under any ctest CWD.
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

// Tier-A is closed under inclusion: a frozen header's dependencies are frozen
// too, whether or not a caller names them. The only permitted escape is
// downward into the internal tiers (detail/, simd/), which carry no stability
// promise and are not part of the umbrella.
TEST(VolUmbrella, TierAIsClosedUnderInclusion) {
  const std::set<std::string> tier_a(std::begin(kTierA), std::end(kTierA));

  for (const std::string& header : tier_a) {
    const fs::path path = include_root() / header;
    ASSERT_TRUE(fs::exists(path)) << "Tier-A header does not exist: " << header;
    for (const std::string& dep : direct_atx_vol_includes(path)) {
      if (dep.rfind("atx/vol/detail/", 0) == 0) continue;
      if (dep.rfind("atx/vol/simd/", 0) == 0) continue;
      EXPECT_TRUE(tier_a.count(dep) == 1)
          << header << " is Tier-A but depends on non-Tier-A " << dep
          << " — either promote " << dep << " to Tier-A, demote " << header
          << ", or break the dependency";
    }
  }
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
// header under include/atx/vol/ may not NAME them at all, in code or in prose.
// Internal code (src/, detail/, tests) may keep using them; that is what the
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
  const fs::path public_dir = include_root() / "atx" / "vol";
  ASSERT_TRUE(fs::is_directory(public_dir)) << public_dir.string();

  // Non-recursive on purpose: detail/ and simd/ are the internal tiers and are
  // exactly where the demoted containers are allowed to live.
  std::size_t headers_scanned = 0;
  for (const fs::directory_entry& entry : fs::directory_iterator(public_dir)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".hpp") continue;
    ++headers_scanned;
    const std::string text = read_file(entry.path());
    for (const std::string_view type : kDemotedSurfaceTypes) {
      EXPECT_EQ(text.find(type), std::string::npos)
          << entry.path().filename().string() << " names the demoted container `"
          << type << "` — plan 4.4 moved it to include/atx/vol/detail/. Use the "
             "canonical pipeline (CurveSurface -> PricedSurface / "
             "PricedSurfaceView -> SurfaceSet) in the public API, and reach for "
             "the demoted type from internal code only.";
    }
  }
  EXPECT_GT(headers_scanned, 50u) << "public header scan found almost nothing";
}

}  // namespace
