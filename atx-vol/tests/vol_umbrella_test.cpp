#include <gtest/gtest.h>

#include <cctype>
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
  std::string ladder;       // a parent-directory rung opening a literal
  std::string file_macro;   // a path derived from the compiler's file macro
  std::string cwd_call;     // an explicit working-directory read/change
  std::string accepted_abs; // the ONE blessed absolute root (see below)

  static PathNeedles make() {
    PathNeedles n;
    n.ladder = std::string("\"..") + "/";
    n.file_macro = std::string("__FI") + "LE__";
    n.cwd_call = std::string("current_") + "path(";
    // Every absolute literal in atx-vol today names this external vendor hive,
    // which sits OUTSIDE any checkout. Stated as a positive allowance rather
    // than a per-file allowlist so that a path into a *sibling worktree* fails
    // wherever it is written. The previous sweeps searched for "C:/atx/" WITH
    // the trailing slash, which is precisely what excluded C:/atx-wt/... --
    // a pattern shaped by the last defect inherits that defect's blind spot.
    n.accepted_abs = std::string("\"C:") + "/atx-data/";
    return n;
  }
};

// True when a string literal beginning at `q` (the opening quote) is an
// absolute path: a drive-letter root (`"C:/`, `"D:\`) or a rooted/UNC backslash
// (`"\\` in source, i.e. an escaped backslash). Names no particular drive or
// checkout, so it cannot inherit one's blind spot.
[[nodiscard]] bool literal_is_absolute(const std::string& line, std::size_t q) {
  if (q + 3 < line.size() && std::isalpha(static_cast<unsigned char>(line[q + 1])) != 0 &&
      line[q + 2] == ':' && (line[q + 3] == '/' || line[q + 3] == '\\')) {
    return true;
  }
  return q + 2 < line.size() && line[q + 1] == '\\' && line[q + 2] == '\\';
}

struct LineScan {
  std::vector<std::string> violations;      // empty => clean
  bool include_with_ladder = false;         // for the non-vacuity check
};

// Which path-origin rules one line of C++ source breaks. Shared by the file walk
// and by the self-test below, so the detector proven in the self-test is the
// same code that polices the tree.
[[nodiscard]] LineScan scan_source_line(const std::string& line, const PathNeedles& n,
                                        bool check_ladder, bool check_macro,
                                        bool check_cwd) {
  LineScan out;
  const std::size_t first = line.find_first_not_of(" \t");
  if (first == std::string::npos) return out;
  if (line.compare(first, 2, "//") == 0) return out;  // prose, not code
  if (line[first] == '#') {
    out.include_with_ladder = line.find("include", first) != std::string::npos &&
                              line.find(n.ladder, first) != std::string::npos;
    return out;
  }

  if (check_ladder && line.find(n.ladder) != std::string::npos) {
    out.violations.emplace_back("parent-directory rung");
  }
  if (check_macro && line.find(n.file_macro) != std::string::npos) {
    out.violations.emplace_back("path derived from the compiler file macro");
  }
  if (check_cwd && line.find(n.cwd_call) != std::string::npos) {
    out.violations.emplace_back("working-directory call");
  }
  for (std::size_t i = 0; i + 1 < line.size(); ++i) {
    if (line[i] != '"' || !literal_is_absolute(line, i)) continue;
    if (line.compare(i, n.accepted_abs.size(), n.accepted_abs) == 0) continue;
    out.violations.emplace_back("absolute path literal outside the accepted data root");
    break;
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
  const auto scan = [&n](const std::string& line) {
    return scan_source_line(line, n, /*check_ladder=*/true, /*check_macro=*/true,
                            /*check_cwd=*/true);
  };

  // Positives -- each must be caught.
  EXPECT_EQ(scan("  const fs::path p{" + n.ladder + "data/x.tsv\"};").violations.size(), 1u)
      << "the parent-directory rung needle no longer fires";
  EXPECT_EQ(scan("  return fs::path(" + n.file_macro + ").parent_path();").violations.size(), 1u)
      << "the compiler-file-macro needle no longer fires";
  EXPECT_EQ(scan("  fs::" + n.cwd_call + "dir);").violations.size(), 1u)
      << "the working-directory needle no longer fires";
  EXPECT_EQ(scan(std::string("  const char* p = \"") + "C:" + "/atx-wt/other/x.parquet\";")
                .violations.size(),
            1u)
      << "a cross-checkout absolute is not caught -- this is the I2 defect";
  // The two backslashes come from a char, not from source text: any literal way
  // of writing them right after a quote would make this line match itself.
  const std::string bs(2, '\\');
  EXPECT_EQ(scan(std::string("  const char* p = \"") + bs + "srv" + bs + "share\";")
                .violations.size(),
            1u)
      << "a UNC/rooted-backslash absolute is not caught";

  // Negatives -- each must be allowed, for a stated reason.
  EXPECT_TRUE(scan("#include " + n.ladder + "src/foo.hpp\"").violations.empty())
      << "an #include is a compile-time path, never resolved against the CWD";
  EXPECT_TRUE(scan("#include " + n.ladder + "src/foo.hpp\"").include_with_ladder)
      << "the #include exclusion did not register, so its own non-vacuity check is blind";
  EXPECT_TRUE(scan("  // prose mentioning " + n.ladder + " and " + n.file_macro).violations.empty())
      << "comments are prose; this file's own explanation names every needle";
  EXPECT_TRUE(scan("  const char* p = " + n.accepted_abs + "spy-dispersion/opra\";")
                  .violations.empty())
      << "the external vendor hive is the one accepted absolute root";
  EXPECT_TRUE(scan("  std::printf(\"two readings:\\n\");").violations.empty())
      << "a printf escape is not a drive letter -- the structural sweep's false-positive class";
  EXPECT_TRUE(scan("  const char* u = \"https://example.invalid/x\";").violations.empty())
      << "a URL scheme is not a drive letter -- the other false-positive class";
}

TEST(VolUmbrella, NoFixturePathResolvedOutsideTheSharedResolver) {
  const fs::path tests = testkit::tests_root();
  const fs::path scope = testkit::repo_file("atx-vol");
  ASSERT_TRUE(fs::is_directory(tests)) << "tests root does not resolve: " << tests.string();
  ASSERT_TRUE(fs::is_directory(scope)) << "atx-vol root does not resolve: " << scope.string();

  const PathNeedles n = PathNeedles::make();
  std::size_t files_scanned = 0;
  std::size_t tests_scanned = 0;
  std::size_t include_lines_skipped = 0;

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
    std::string line;
    for (std::size_t ln = 1; std::getline(in, line); ++ln) {
      const LineScan s = scan_source_line(line, n, in_tests && !ladder_exempt, in_tests,
                                          in_tests && !cwd_exempt);
      if (s.include_with_ladder) ++include_lines_skipped;
      for (const std::string& v : s.violations) {
        ADD_FAILURE() << name << ":" << ln << " -- " << v
                      << ". A path that resolves against the process working directory, or "
                         "into another checkout, names a different file depending on where "
                         "the binary was launched from. Resolve it through "
                         "tests/support/test_paths.hpp (test_fixture / test_data / "
                         "market_data / repo_file / artifact_cache_root). If the literal is "
                         "DATA rather than a path, add the file to kPathLiteralExempt and "
                         "say why.";
      }
    }
  }

  // Non-vacuity in three directions: the walk must have reached the wider scope
  // AND the tests subtree AND actually exercised the #include exclusion.
  // A scanner that silently matched nothing would otherwise report this contract
  // satisfied -- the exact shape the tier-table comment above exists to prevent.
  EXPECT_GT(files_scanned, 200u) << "atx-vol scan found almost nothing; the walk is broken";
  EXPECT_GT(tests_scanned, 100u) << "the tests subtree was not reached by the walk";
  EXPECT_GT(include_lines_skipped, 0u)
      << "no #include line carrying a parent-directory rung was seen, so the exclusion is "
         "untested and this contract may be passing because the scan matched nothing";
}

}  // namespace
