// theme-cov-hedge-v1 (P9 lane AL-COMB, cell P9-H; strategy_ic_theme_hedge.hpp): the rule's row and
// block, its absence (flag absent: the parse, the stage list, the recipe and the composition are
// the D1 runner's, bit for bit, and the role is not read), the composition's hedge stage (before
// the hedge ratio's first estimate the hedged composition is ew-theme-std-v1's bit for bit; after
// it, each theme adds its mass times the kernel's hedged rank), the stage wiring and its refusals,
// and the session returns the runner hands the composition. The kernel itself:
// combine_char_cov_hedge_test.cpp.
//
// Suite: ThemeHedgeRule

#include <gtest/gtest.h>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/char_cov_hedge.hpp"
#include "atx/engine/data/strategy_data.hpp"
#include "../src/strategy_ic_composition.hpp"
#include "../src/strategy_ic_rules.hpp"
#include "../src/strategy_ic_runner.hpp"
#include "../src/strategy_ic_theme_hedge.hpp"
#include "../src/strategy_price_exposures.hpp"

namespace {
using namespace atx;
using Json = nlohmann::json;
namespace st = atx::impl::strategy;
namespace icd = atx::impl::strategy::ic_detail;
namespace cb = atx::engine::combine;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

u64 bits(f64 x) { return std::bit_cast<u64>(x); }
bool same_bits(const std::vector<f64>& a, const std::vector<f64>& b) {
  if (a.size() != b.size()) return false;
  for (usize k = 0; k < a.size(); ++k)
    if (bits(a[k]) != bits(b[k])) return false;
  return true;
}
bool same_result(const st::IcCompositionResult& a, const st::IcCompositionResult& b) {
  return same_bits(a.signal, b.signal) && same_bits(a.planned_turnover, b.planned_turnover) &&
         same_bits(a.planned_gross, b.planned_gross) && same_bits(a.planned_net, b.planned_net) &&
         same_bits(a.contribution_fraction, b.contribution_fraction);
}

// Deterministic uniform values in [-1, 1).
struct Lcg {
  u64 state;
  f64 next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) / static_cast<f64>(1ULL << 52) - 1.0;
  }
};

// Four candidates in two themes (a1, a2: value; b1, b2: price_momentum) over `width` names and
// `days` sessions; random signals with a common return factor, every name a member; a role panel
// whose close and raw_close carry the same random returns (one large jump the guard refuses).
struct Fixture {
  usize days, width;
  std::vector<st::IcCompositionCandidate> candidates{
      {"a1", "a"}, {"a2", "a"}, {"b1", "b"}, {"b2", "b"}};
  std::vector<usize> themes{0, 0, 1, 1};
  std::vector<f64> weights{.25, .25, .2, .3};
  std::vector<u8> member;
  std::vector<std::vector<f64>> signals;
  std::vector<f64> close;
  std::vector<i64> sessions;
  st::IcCompositionConfig cfg;
  icd::Library lib;
  Fixture(usize d, usize n)
      : days{d}, width{n}, member(d * n, 1U), signals(4, std::vector<f64>(d * n)), close(d * n),
        sessions(d) {
    cfg.dates = days;
    cfg.instruments = width;
    cfg.decision_end = days;
    cfg.max_working_bytes = 64ULL << 20;
    Lcg g{17U};
    std::vector<f64> loading(width);
    for (auto& b : loading) b = 1.0 + .8 * g.next();
    for (usize s = 0; s < days; ++s) {
      sessions[s] = static_cast<i64>(1000 + s);
      const f64 f = .01 * g.next();
      for (usize i = 0; i < width; ++i) {
        const usize at = s * width + i;
        const f64 r = loading[i] * f + .006 * g.next();
        close[at] = s == 0 ? 50.0 + 10.0 * g.next() : close[at - width] * (1.0 + r);
        for (auto& signal : signals) signal[at] = g.next() + .4 * loading[i];
      }
    }
    close[(days / 2) * width + 1] *= 9.0; // |log| > 1.5 into and out of this session: guarded
    for (const auto& c : candidates)
      lib.candidates.push_back(icd::Candidate{c.id, c.family, "sha-" + c.id, {}, {}});
  }
  // The role the runner reads (close and raw_close; every cell in the universe).
  engine::data::StrategyRoleData role(bool with_prices = true) const {
    std::vector<std::string> names;
    std::vector<std::vector<f64>> data;
    if (with_prices) {
      names = {"close", "raw_close"};
      data = {close, close};
    }
    auto panel = engine::alpha::Panel::create(days, width, std::move(names), std::move(data), {});
    EXPECT_TRUE(panel);
    return engine::data::StrategyRoleData{std::move(*panel)};
  }
  std::optional<st::IcCompositionResult> finish(st::IcComposition& c) const {
    for (usize k = 0; k < candidates.size(); ++k)
      if (!c.add(k, signals[k], 1)) return std::nullopt;
    auto out = c.finish();
    if (!out) return std::nullopt;
    return std::move(*out);
  }
  // ew-theme-std-v1 (the v8 composition): create, optionally a schedule, then the members.
  std::optional<st::IcCompositionResult> plain(
      std::span<const st::IcThemeBlock> schedule = {}) const {
    auto c = st::IcComposition::create(cfg, candidates, member, weights, themes,
                                       st::IcThemeRule::standardise);
    if (!c || (!schedule.empty() && !c->schedule_theme_masses(schedule))) return std::nullopt;
    return finish(*c);
  }
  // The same with the hedge attached (the role's session returns).
  std::optional<st::IcCompositionResult> hedged(
      std::span<const st::IcThemeBlock> schedule = {}) const {
    auto c = st::IcComposition::create(cfg, candidates, member, weights, themes,
                                       st::IcThemeRule::standardise);
    if (!c || (!schedule.empty() && !c->schedule_theme_masses(schedule))) return std::nullopt;
    auto returns = st::theme_hedge_session_returns(role());
    if (!returns || !c->set_theme_hedge(std::move(*returns))) return std::nullopt;
    return finish(*c);
  }
  // A weights document of this library: theme_standardise ew-theme-std-v1 with rerank true, plus
  // `extra` top-level blocks (parsed from text, as a weights file is).
  static Json document(const std::string& extra = {}) {
    return Json::parse(R"({"schema":"atx.dsl-composition-weights/v2",)"
                       R"("provenance":{"rule":"ew-theme-std-v1"},)"
                       R"("theme_standardise":{"rule":"ew-theme-std-v1","rerank":true,)"
                       R"("themes":{"a1":"value","a2":"value",)"
                       R"("b1":"price_momentum","b2":"price_momentum"}})" +
                       extra + "}");
  }
  core::Result<icd::PinnedWeights> parse(const Json& doc) const {
    const auto themes_table = icd::builtin_theme_table();
    icd::PinnedWeights pinned;
    pinned.values = weights;
    ATX_TRY_VOID(icd::parse_composition_rules(doc, icd::RuleInputs{lib, themes_table}, pinned));
    return core::Ok(std::move(pinned));
  }
};
constexpr const char* hedge_block = R"(,"theme_hedge":{"rule":"theme-cov-hedge-v1",)"
                                    R"("lookback":252,"lag":2,"step":21,"min_pairs":126})";

TEST(ThemeHedgeRule, RegisteredConstantsAndRow) {
  EXPECT_EQ(st::theme_hedge_rule, "theme-cov-hedge-v1");
  EXPECT_EQ(st::theme_hedge_lookback, 252U);
  EXPECT_EQ(st::theme_hedge_lag, 2U);
  EXPECT_EQ(st::theme_hedge_step, 21U);
  EXPECT_EQ(st::theme_hedge_min_pairs, 126U);
  const auto* row = icd::find_rule("theme-cov-hedge-v1");
  ASSERT_TRUE(row != nullptr);
  EXPECT_EQ(row->block_key, "theme_hedge");
  EXPECT_EQ(icd::stage_name(row->stage), "hedge");
  EXPECT_EQ(row->recipe_key, "composition_hedge");
  EXPECT_TRUE(row->verify == nullptr);
  EXPECT_EQ(row->requires_any.size(), 4U);
  EXPECT_TRUE(&icd::composition_rules().back() == row); // parsed last, after the v8 rows
  const auto schema = Json::parse(row->params_schema.begin(), row->params_schema.end());
  for (const auto& [key, value] : std::vector<std::pair<std::string, int>>{
           {"lookback", 252}, {"lag", 2}, {"step", 21}, {"min_pairs", 126}})
    EXPECT_EQ(schema.at("properties").at(key).at("const"), value) << key;
}

// Flag absent (no theme_hedge block): the parse, the running rows, the stage list, its bytes and
// the recipe are the D1 runner's, the hook reads nothing (the role here has no price field at all),
// and the composition is ew-theme-std-v1's, bit for bit.
TEST(ThemeHedgeRule, FlagAbsentIdentity) {
  const Fixture f(30, 7);
  const auto pinned = f.parse(Fixture::document());
  ASSERT_TRUE(pinned) << pinned.error().to_string();
  ASSERT_EQ(pinned->rules.size(), 1U);
  EXPECT_EQ(pinned->rules.front()->id, "ew-theme-std-v1");
  EXPECT_EQ(icd::running_stage_kinds(*pinned),
            std::vector<st::IcStageKind>{st::IcStageKind::standardise});
  st::IcRunnerConfig cfg;
  cfg.library_sha256 = std::string(64, 'a');
  cfg.composition_weights_sha256 = std::string(64, 'c');
  const auto recipe = icd::method_recipe(cfg, true, true, &*pinned);
  EXPECT_FALSE(recipe.contains("composition_hedge"));
  EXPECT_EQ(recipe.at("composition_standardise"), "ew-theme-std-v1");
  const auto kinds = icd::running_stage_kinds(*pinned);
  const auto staged_bytes = st::ic_composition_stage_bytes(f.days, f.width, 4, 2, kinds);
  const auto v8_bytes =
      st::ic_composition_working_bytes(f.days, f.width, 4, 2, st::IcThemeRule::standardise);
  ASSERT_TRUE(staged_bytes);
  ASSERT_TRUE(v8_bytes);
  EXPECT_EQ(*staged_bytes, *v8_bytes);
  const auto stages = icd::composition_stages(*pinned, f.sessions);
  ASSERT_TRUE(stages);
  auto c = st::IcComposition::create_from_stages(f.cfg, f.candidates, f.member, *stages);
  ASSERT_TRUE(c) << c.error().to_string();
  const auto no_prices = f.role(false);
  ASSERT_TRUE(icd::attach_theme_hedge(*pinned, no_prices, *c));
  const auto got = f.finish(*c);
  const auto want = f.plain();
  ASSERT_TRUE(got);
  ASSERT_TRUE(want);
  EXPECT_TRUE(same_result(*got, *want));
}

// The block is exactly the registered constants, beside a rerank-true theme_standardise and without
// theme_residualise or theme_sleeves; accepted, it is the last running row, a hedge stage and the
// recipe's composition_hedge.
TEST(ThemeHedgeRule, BlockParseAndRefusals) {
  const Fixture f(30, 7);
  const auto pinned = f.parse(Fixture::document(hedge_block));
  ASSERT_TRUE(pinned) << pinned.error().to_string();
  ASSERT_EQ(pinned->rules.size(), 2U);
  EXPECT_EQ(pinned->rules.back()->id, "theme-cov-hedge-v1");
  EXPECT_EQ(icd::running_stage_kinds(*pinned),
            (std::vector<st::IcStageKind>{st::IcStageKind::standardise, st::IcStageKind::hedge}));
  st::IcRunnerConfig cfg;
  cfg.library_sha256 = std::string(64, 'a');
  cfg.composition_weights_sha256 = std::string(64, 'c');
  EXPECT_EQ(icd::method_recipe(cfg, true, true, &*pinned).at("composition_hedge"),
            "theme-cov-hedge-v1");
  const std::string v1 = R"(,"theme_hedge":{"rule":"theme-cov-hedge-v1",)";
  const std::vector<std::string> off_registration{
      v1 + R"("lookback":126,"lag":2,"step":21,"min_pairs":126})",
      v1 + R"("lookback":252,"lag":3,"step":21,"min_pairs":126})",
      v1 + R"("lookback":252,"lag":2,"step":63,"min_pairs":126})",
      v1 + R"("lookback":252,"lag":2,"step":21,"min_pairs":63})",
      v1 + R"("lookback":252,"lag":2,"step":21})",
      v1 + R"("lookback":252,"lag":2,"step":21,"min_pairs":126,"h":1})",
      R"(,"theme_hedge":{"rule":"theme-cov-hedge-v2",)"
      R"("lookback":252,"lag":2,"step":21,"min_pairs":126})",
      v1 + R"("lookback":252.5,"lag":2,"step":21,"min_pairs":126})",
      R"(,"theme_hedge":["theme-cov-hedge-v1"])"};
  for (const auto& extra : off_registration) {
    const auto refused = f.parse(Fixture::document(extra));
    ASSERT_FALSE(refused) << extra;
    EXPECT_NE(refused.error().message().find("theme_hedge must be"), std::string::npos) << extra;
  }
  // Beside theme_residualise or theme_sleeves, or without a rerank-true theme_standardise.
  auto resid = Fixture::document(std::string(hedge_block) +
                                 R"(,"theme_residualise":{"rule":"theme-resid-v1",)"
                                 R"("order":["value","price_momentum"]})");
  const auto with_resid = f.parse(resid);
  const std::string exclusive = "no theme_residualise or theme_sleeves";
  ASSERT_FALSE(with_resid);
  EXPECT_NE(with_resid.error().message().find(exclusive), std::string::npos)
      << with_resid.error().message();
  auto sleeved = Json::parse(R"({"schema":"atx.dsl-composition-weights/v2",)"
                             R"("provenance":{"rule":"ew-theme-std-v1"},)"
                             R"("theme_standardise":{"rule":"ew-theme-std-v1","rerank":true,)"
                             R"("themes":{"a1":"value","a2":"value",)"
                             R"("b1":"reversal_seasonality","b2":"reversal_seasonality"}},)"
                             R"("theme_sleeves":{"rule":"two-speed-v1"}})" +
                             std::string(hedge_block) + "}");
  const auto with_sleeves = f.parse(sleeved);
  ASSERT_FALSE(with_sleeves);
  EXPECT_NE(with_sleeves.error().message().find(exclusive), std::string::npos)
      << with_sleeves.error().message();
  auto rerank_off = Fixture::document(hedge_block);
  rerank_off["theme_standardise"]["rerank"] = false;
  const auto off = f.parse(rerank_off);
  ASSERT_FALSE(off);
  EXPECT_NE(off.error().message().find("needs a theme_standardise block"), std::string::npos)
      << off.error().message();
}

// Before the hedge ratio's first estimate (a role shorter than 274 sessions has none) the hedged
// composition is ew-theme-std-v1's bit for bit, with or without a theme schedule riding on it.
TEST(ThemeHedgeRule, BeforeTheFirstEstimateTheHedgeIsTheParent) {
  const Fixture f(60, 9);
  const auto plain = f.plain();
  const auto hedged = f.hedged();
  ASSERT_TRUE(plain);
  ASSERT_TRUE(hedged);
  EXPECT_TRUE(same_result(*hedged, *plain));
  const f64 w_a = .25 + .25; // theme a's W_theme
  const std::vector<st::IcThemeBlock> schedule{{20, {w_a, 0.0}}, {41, {.3, .7}}};
  const auto plain_scheduled = f.plain(schedule);
  const auto hedged_scheduled = f.hedged(schedule);
  ASSERT_TRUE(plain_scheduled);
  ASSERT_TRUE(hedged_scheduled);
  EXPECT_TRUE(same_result(*hedged_scheduled, *plain_scheduled));
  EXPECT_FALSE(same_bits(plain_scheduled->signal, plain->signal)); // the schedule moved the blend
}

// add_theme_hedged is, per theme in index order and per block of the schedule, the mass in force
// times the kernel's hedged rank under the registered constants, added to the blend: on a role long
// enough for the hedge ratio (first estimated at decision 274 here), and the hedge moves the blend
// there.
TEST(ThemeHedgeRule, AddThemeHedgedIsTheMassTimesTheKernel) {
  constexpr usize days = 320, width = 8;
  Lcg g{3U};
  std::vector<std::vector<f64>> planes(2, std::vector<f64>(days * width));
  std::vector<f64> returns(days * width, kNaN);
  std::vector<f64> loading(width);
  for (auto& b : loading) b = 1.0 + .7 * g.next();
  for (usize d = 0; d < days; ++d) {
    const f64 f = .01 * g.next();
    for (usize i = 0; i < width; ++i) {
      const usize at = d * width + i;
      planes[0][at] = g.next() > -.85 ? g.next() + .5 * loading[i] : kNaN;
      planes[1][at] = g.next() > -.85 ? g.next() - .3 * loading[i] : kNaN;
      if (d > 0 && g.next() > -.9) returns[at] = loading[i] * f + .004 * g.next();
    }
  }
  const std::vector<f64> mass{.4, .6};
  const std::vector<st::IcThemeBlock> schedule{{280, {.2, .8}}, {300, {0.0, 1.0}}};
  std::vector<f64> out(days * width, 0.0);
  ASSERT_TRUE(st::add_theme_hedged(planes, mass, schedule, returns, width, out));
  // The kernel under the registered constants, theme by theme.
  const cb::CharCovHedgeConfig registered{252, 2, 21, 126};
  std::vector<f64> want(days * width, 0.0), hedged(days * width), ratio(days);
  cb::CharCovHedgeScratch scratch;
  bool active = false;
  for (usize t = 0; t < 2; ++t) {
    ASSERT_TRUE(cb::char_cov_hedge(planes[t], returns, width, registered, hedged, scratch,
                                   {{}, {}, ratio, {}}));
    for (usize d = 0; d < days; ++d) {
      active = active || (d >= 274 && ratio[d] != 0.0);
      if (d < 274) EXPECT_EQ(bits(ratio[d]), bits(0.0)) << t << ' ' << d;
      const f64 m = d < 280 ? mass[t] : d < 300 ? schedule[0].mass[t] : schedule[1].mass[t];
      if (!(m > 0)) continue;
      for (usize i = 0; i < width; ++i)
        if (!std::isnan(hedged[d * width + i])) want[d * width + i] += m * hedged[d * width + i];
    }
  }
  EXPECT_TRUE(active);
  EXPECT_TRUE(same_bits(out, want));
  // The refusals: shapes and a non-finite mass, before any write.
  std::vector<f64> untouched(days * width, 7.0);
  const std::vector<f64> one_mass{.4}, nan_mass{.4, kNaN};
  const std::vector<st::IcThemeBlock> narrow{{280, {.2}}};
  const auto first_row = std::span<const f64>(returns).first(width);
  EXPECT_FALSE(st::add_theme_hedged(planes, one_mass, schedule, returns, width, untouched));
  EXPECT_FALSE(st::add_theme_hedged(planes, nan_mass, schedule, returns, width, untouched));
  EXPECT_FALSE(st::add_theme_hedged(planes, mass, schedule, first_row, width, untouched));
  EXPECT_FALSE(st::add_theme_hedged(planes, mass, narrow, returns, width, untouched));
  for (const f64 x : untouched) EXPECT_EQ(x, 7.0);
}

// The runner's path: a weights document with the block runs [standardise, hedge];
// attach_theme_hedge hands the composition the role's session returns, and the result is the hedged
// composition set up by hand. A hedge stage without its returns refuses finish; set_theme_hedge
// refuses another rule, sleeves, a wrong shape, an exceeded budget and a finished composition; the
// stage list refuses a hedge with sleeves; the stage bytes add theme_hedge_working_bytes.
TEST(ThemeHedgeRule, StageWiringAndRefusals) {
  const Fixture f(300, 8);
  const auto pinned = f.parse(Fixture::document(hedge_block));
  ASSERT_TRUE(pinned) << pinned.error().to_string();
  const auto stages = icd::composition_stages(*pinned, f.sessions);
  ASSERT_TRUE(stages);
  ASSERT_EQ(stages->stages.size(), 2U);
  EXPECT_TRUE(stages->stages.back().kind == st::IcStageKind::hedge);
  auto runner = st::IcComposition::create_from_stages(f.cfg, f.candidates, f.member, *stages);
  ASSERT_TRUE(runner) << runner.error().to_string();
  ASSERT_TRUE(icd::attach_theme_hedge(*pinned, f.role(), *runner));
  const auto got = f.finish(*runner);
  const auto want = f.hedged();
  const auto parent = f.plain();
  ASSERT_TRUE(got);
  ASSERT_TRUE(want);
  ASSERT_TRUE(parent);
  EXPECT_TRUE(same_result(*got, *want));
  EXPECT_FALSE(same_bits(got->signal, parent->signal)); // 300 sessions: the hedge acts from 274
  for (usize k = 0; k < 274 * f.width; ++k)
    EXPECT_EQ(bits(got->signal[k]), bits(parent->signal[k])) << k;
  // Without the returns, finish refuses.
  auto pending = st::IcComposition::create_from_stages(f.cfg, f.candidates, f.member, *stages);
  ASSERT_TRUE(pending);
  EXPECT_FALSE(f.finish(*pending));
  // set_theme_hedge refusals.
  const auto returns = st::theme_hedge_session_returns(f.role());
  ASSERT_TRUE(returns);
  auto redistribute = st::IcComposition::create(f.cfg, f.candidates, f.member, f.weights, f.themes,
                                                st::IcThemeRule::redistribute);
  ASSERT_TRUE(redistribute);
  EXPECT_FALSE(redistribute->set_theme_hedge(*returns));
  auto sleeved = st::IcComposition::create(f.cfg, f.candidates, f.member, f.weights, f.themes,
                                           st::IcThemeRule::standardise);
  ASSERT_TRUE(sleeved);
  ASSERT_TRUE(sleeved->set_theme_sleeves(std::vector<u8>{1, 0}));
  EXPECT_FALSE(sleeved->set_theme_hedge(*returns));
  auto standard = st::IcComposition::create(f.cfg, f.candidates, f.member, f.weights, f.themes,
                                            st::IcThemeRule::standardise);
  ASSERT_TRUE(standard);
  EXPECT_FALSE(standard->set_theme_hedge(std::vector<f64>(f.width)));
  auto tight_cfg = f.cfg;
  const auto envelope =
      st::ic_composition_working_bytes(f.days, f.width, 4, 2, st::IcThemeRule::standardise);
  ASSERT_TRUE(envelope);
  tight_cfg.max_working_bytes = *envelope;
  auto tight = st::IcComposition::create(tight_cfg, f.candidates, f.member, f.weights, f.themes,
                                         st::IcThemeRule::standardise);
  ASSERT_TRUE(tight);
  const auto over = tight->set_theme_hedge(*returns);
  ASSERT_FALSE(over);
  EXPECT_EQ(over.error().code(), core::ErrorCode::OutOfRange);
  ASSERT_TRUE(f.finish(*standard));
  EXPECT_FALSE(standard->set_theme_hedge(*returns)); // after finish
  // The stage list and its bytes.
  using K = st::IcStageKind;
  const std::vector<K> with_sleeves{K::standardise, K::sleeves, K::hedge};
  const std::vector<K> with_resid{K::standardise, K::residualise, K::hedge};
  const std::vector<K> alone{K::hedge}, scheduled{K::standardise, K::schedule, K::hedge};
  EXPECT_FALSE(st::ic_composition_stage_bytes(f.days, f.width, 4, 2, with_sleeves));
  EXPECT_FALSE(st::ic_composition_stage_bytes(f.days, f.width, 4, 2, with_resid));
  EXPECT_FALSE(st::ic_composition_stage_bytes(f.days, f.width, 4, 2, alone));
  const auto hedge_bytes = st::theme_hedge_working_bytes(f.days, f.width);
  const auto staged = st::ic_composition_stage_bytes(f.days, f.width, 4, 2, scheduled);
  ASSERT_TRUE(hedge_bytes);
  ASSERT_TRUE(staged);
  EXPECT_EQ(*staged, *envelope + *hedge_bytes);
  const auto scratch =
      cb::char_cov_hedge_scratch_bytes(f.days, f.width, cb::CharCovHedgeConfig{252, 2, 21, 126});
  ASSERT_TRUE(scratch);
  EXPECT_EQ(*hedge_bytes, f.days * f.width * (2U * sizeof(f64) + 1U) + *scratch);
}

// The runner's returns are price-risk-v1's guarded session returns (session_interval_returns, row
// by row): row 0 NaN, the guarded jump NaN on both its sessions, every other cell close[s] /
// close[s-1] - 1.
TEST(ThemeHedgeRule, SessionReturnsAreTheGuardedIntervalReturns) {
  const Fixture f(40, 6);
  const auto role = f.role();
  const auto returns = st::theme_hedge_session_returns(role);
  ASSERT_TRUE(returns) << returns.error().to_string();
  ASSERT_EQ(returns->size(), f.days * f.width);
  std::vector<u8> present(f.days * f.width, 1U);
  const st::PriceExposureInput input{f.days, f.width, f.close, f.close, {}, present};
  st::PriceExposureScratch scratch;
  std::vector<f64> row(f.width);
  for (usize i = 0; i < f.width; ++i) EXPECT_TRUE(std::isnan((*returns)[i]));
  for (usize s = 1; s < f.days; ++s) {
    ASSERT_TRUE(st::session_interval_returns(input, s, scratch, row));
    for (usize i = 0; i < f.width; ++i)
      EXPECT_EQ(bits((*returns)[s * f.width + i]), bits(row[i])) << s << ' ' << i;
  }
  const usize jump = (f.days / 2) * f.width + 1;
  EXPECT_TRUE(std::isnan((*returns)[jump]));
  EXPECT_TRUE(std::isnan((*returns)[jump + f.width]));
  EXPECT_NEAR((*returns)[2 * f.width + 3], f.close[2 * f.width + 3] / f.close[f.width + 3] - 1.0,
              1e-15);
  // A role without close or raw_close is refused.
  EXPECT_FALSE(st::theme_hedge_session_returns(f.role(false)));
}
} // namespace
