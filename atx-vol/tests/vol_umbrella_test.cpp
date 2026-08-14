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

#include "support/test_paths.hpp" // testkit::repo_file, tests_root

namespace {

namespace testkit = atx::vol::testkit;

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
    "atx/vol/deriv_book.hpp", // vol-derivatives sprint: swap-book pricing joined the umbrella
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
    "atx/vol/swap_leg.hpp",
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

// ── The README tier table, parsed back off disk ─────────────────────────────
//
// `TierCountsMatchTheReadmeTable` below reads the three Count cells out of the
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
// These helpers exist to make every way the parse can go wrong a LOUD, SPECIFIC
// failure, which matters more than it looks. The three literals had exactly one
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

// LEADING integer only: "34 + 9" -> 34, "31 (+1 generated)" -> 31. Two of the
// three cells deliberately carry a second number that is NOT in the source tree
// this test walks -- `simd/` for Tier-B, and the configure_file'd
// version_generated.hpp that exists only in an install prefix -- so consuming
// the whole cell would be wrong, not merely fragile.
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
// has no literal left to keep in step with it.
TEST(VolUmbrella, TierCountsMatchTheReadmeTable) {
  const ReadmeCount tier_a_row = readme_tier_count("Tier-A");
  const ReadmeCount tier_b_row = readme_tier_count("Tier-B");
  const ReadmeCount detail_row = readme_tier_count("detail/");

  // ASSERT rather than EXPECT: with no table there is nothing to compare
  // against, and the three EXPECTs below would each report the same one fault.
  ASSERT_TRUE(tier_a_row.ok) << tier_a_row.error;
  ASSERT_TRUE(tier_b_row.ok) << tier_b_row.error;
  ASSERT_TRUE(detail_row.ok) << detail_row.error;

  EXPECT_EQ(std::size(kTierA), tier_a_row.value)
      << "the README tier table says Tier-A is " << tier_a_row.value
      << " but the umbrella manifest `kTierA` holds " << std::size(kTierA)
      << " -- they are the same promise, so fix whichever is wrong";

  std::size_t top_level_headers = 0;
  for (const fs::directory_entry& entry :
       fs::directory_iterator(include_root() / "atx" / "vol")) {
    if (entry.is_regular_file() && entry.path().extension() == ".hpp") ++top_level_headers;
  }
  // Tier-B = every top-level header minus the Tier-A ones minus vol.hpp itself.
  ASSERT_GT(top_level_headers, std::size(kTierA) + 1u);
  const std::size_t tier_b = top_level_headers - std::size(kTierA) - 1u;
  EXPECT_EQ(tier_b, tier_b_row.value)
      << "the README tier table says Tier-B is " << tier_b_row.value << " but include/atx/vol/ "
      << "holds " << top_level_headers << " top-level .hpp, minus " << std::size(kTierA)
      << " Tier-A minus vol.hpp = " << tier_b;

  std::size_t detail_headers = 0;
  for (const fs::directory_entry& entry :
       fs::directory_iterator(include_root() / "atx" / "vol" / "detail")) {
    if (entry.is_regular_file() && entry.path().extension() == ".hpp") ++detail_headers;
  }
  EXPECT_EQ(detail_headers, detail_row.value)
      << "the README tier table says detail/ is " << detail_row.value
      << " but include/atx/vol/detail/ holds " << detail_headers
      << " .hpp (the table's '+1 generated' header is install-tree only and is "
         "deliberately not counted here)";
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
constexpr std::string_view kPathLiteralExempt[] = {
    // Feeds "../escape" to a partition-name validator to prove it is REJECTED.
    // The literal is the adversary, not a lookup.
    "surface_db_test.cpp",
};

TEST(VolUmbrella, NoFixturePathResolvedOutsideTheSharedResolver) {
  const fs::path root = testkit::tests_root();
  ASSERT_TRUE(fs::is_directory(root)) << "tests root does not resolve: " << root.string();

  // Both needles are ASSEMBLED rather than written, so this file does not match
  // its own patterns. Exempting it by name instead would stop it policing
  // itself, and it is a *_test.cpp like any other.
  const std::string ladder = std::string("\"..") + "/";
  const std::string file_macro = std::string("__FI") + "LE__";

  std::size_t files_scanned = 0;
  std::size_t include_lines_skipped = 0;

  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    const fs::path& p = entry.path();
    const std::string ext = p.extension().string();
    if (ext != ".cpp" && ext != ".hpp") continue;

    const std::string name = p.filename().string();
    bool exempt = false;
    for (const std::string_view e : kPathLiteralExempt) {
      if (name == e) exempt = true;
    }

    ++files_scanned;
    std::ifstream in(p);
    std::string line;
    for (std::size_t n = 1; std::getline(in, line); ++n) {
      const std::size_t first = line.find_first_not_of(" \t");
      if (first == std::string::npos) continue;
      if (line.compare(first, 2, "//") == 0) continue;  // prose, not code
      if (line[first] == '#') {
        if (line.find("include", first) != std::string::npos &&
            line.find(ladder, first) != std::string::npos) {
          ++include_lines_skipped;
        }
        continue;
      }
      if (!exempt) {
        EXPECT_EQ(line.find(ladder), std::string::npos)
            << name << ":" << n << " opens a path literal with a parent-directory rung ("
            << ladder << "), which resolves against the process working directory -- so it "
            << "names a different file depending on where the binary was launched from. "
            << "Resolve it through tests/support/test_paths.hpp instead (test_fixture / "
            << "test_data / market_data / repo_file / artifact_cache_root). If the literal "
            << "is DATA rather than a path, add this file to kPathLiteralExempt and say why.";
      }
      EXPECT_EQ(line.find(file_macro), std::string::npos)
          << name << ":" << n << " derives a path from the " << file_macro << " macro, which "
          << "ninja/clang-cl does not reliably make absolute. Use tests/support/test_paths.hpp.";
    }
  }

  // Non-vacuity, both directions: the walk must have reached real content, and
  // the #include exclusion must actually have fired -- otherwise a scanner that
  // silently matched nothing would report this contract as satisfied, which is
  // the exact failure shape the tier-table comment above exists to prevent.
  EXPECT_GT(files_scanned, 100u) << "test-tree scan found almost nothing; the walk is broken";
  EXPECT_GT(include_lines_skipped, 0u)
      << "no #include line carrying a parent-directory rung was seen, so the exclusion is "
         "untested and this contract may be passing because the scan matched nothing";
}

}  // namespace
