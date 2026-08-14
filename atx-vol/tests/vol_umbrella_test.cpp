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
#include "atx/vol/api/vol.hpp"

namespace {

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

// The README's tier table (`## API stability policy (1.x)`, the `| Tier | ... |`
// table) states the Tier-A/Tier-B header counts as prose.
// `UmbrellaIsExactlyTierA` above pins the Tier-A *set* against `kTierA`, but
// nothing pins `kTierA`'s own SIZE, nor Tier-B's -- so a silent drift would go
// uncaught until the next manual audit, exactly the failure mode a string of
// README re-derivations (2026-07 through 2026-08-09, narrated in the README
// against the flat pre-restructure layout) kept finding by hand.
//
// api-restructure (2026-08-14): the flat include/atx/vol/*.hpp + detail/ +
// simd/ layout this test used to walk is GONE. The public surface is now
// include/atx/vol/api/, an 8-module tree (analytics, backtest, core, fitting,
// marketdata, pricing, simd, storage) plus the umbrella api/vol.hpp itself;
// the private implementation moved to src/<module>/, off the include/ tree
// entirely -- walking it here would be meaningless, since it carries no path
// under include/atx/vol/ at all. The model therefore collapses to two rows
// counted off the SAME tree: Tier-A (`kTierA`, machine-checked above) and
// Tier-B (every other public header under api/). `s3.hpp` moved
// storage -> fitting in this same commit (Klassen S3/SSVI is a fitting-family
// curve, not AWS storage); a module reassignment changes no header's tier or
// any count below.
//
// UPDATE PROCEDURE when a header is deliberately added/removed/promoted/
// demoted, or moved between modules: update the affected literal(s) below AND
// the README table (`## API stability policy (1.x)`) in the same commit, and
// confirm `UmbrellaIsExactlyTierA` / `TierAIsClosedUnderInclusion` still pass.
TEST(VolUmbrella, TierCountsMatchTheReadmeTable) {
  EXPECT_EQ(std::size(kTierA), std::size_t{58})
      << "Tier-A count drifted -- update the README table (## API stability "
         "policy) alongside this literal";

  // Recursive: the public surface is an 8-module tree under api/, not a flat
  // directory -- a non-recursive scan would silently undercount to just the
  // umbrella header itself.
  std::size_t api_headers = 0;
  std::size_t module_dirs = 0;
  const fs::path api_root = include_root() / "atx" / "vol" / "api";
  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(api_root)) {
    if (entry.is_directory()) {
      if (entry.path().parent_path() == api_root) ++module_dirs;
      continue;
    }
    if (entry.is_regular_file() && entry.path().extension() == ".hpp") ++api_headers;
  }
  EXPECT_EQ(module_dirs, std::size_t{8})
      << "api/ module-directory count drifted -- update the README table "
         "alongside this literal";
  EXPECT_EQ(api_headers, std::size_t{75})
      << "api/ public header count drifted -- update the README table (## API "
         "stability policy) alongside this literal";

  // Tier-B = every public header minus the Tier-A ones minus vol.hpp itself.
  ASSERT_GT(api_headers, std::size(kTierA) + 1u);
  const std::size_t tier_b = api_headers - std::size(kTierA) - 1u;
  EXPECT_EQ(tier_b, std::size_t{16})
      << "Tier-B count drifted -- update the README table (## API stability "
         "policy) alongside this literal";
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
  EXPECT_EQ(headers_scanned, 75u)
      << "public header scan did not walk the full api/ tree (expected all 75 "
         "public headers)";
}

}  // namespace
