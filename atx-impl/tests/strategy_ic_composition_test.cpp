#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "../src/strategy_ic_composition.hpp"

namespace {
using namespace atx;
namespace al = atx::engine::alpha;
namespace st = atx::impl::strategy;
constexpr usize dates = 420, names = 64, future = 360;
atx::core::Result<al::Panel> panel(bool mutate) {
  std::vector<std::vector<f64>> fields(3, std::vector<f64>(dates * names));
  std::vector<u8> present(dates * names, 1);
  for (usize d = 0; d < dates; ++d) for (usize i = 0; i < names; ++i) {
    const auto t = static_cast<f64>(d), n = static_cast<f64>(i); const auto at = d * names + i;
    const auto raw = 40 + .25 * n + .02 * t + 3 * std::sin(.03 * t + .17 * n) +
                     .7 * std::cos(.09 * t + .11 * n);
    fields[0][at] = raw * (1 + .0001 * t); fields[1][at] = raw;
    fields[2][at] = 500000 + 2500 * n + 100000 * std::sin(.07 * t + .21 * n) +
                    50000 * std::cos(.11 * t + .15 * n);
    if (mutate && (d >= future || i >= 60)) {
      fields[0][at] *= 3 + .01 * n; fields[1][at] *= 1e8; fields[2][at] *= 1e9;
    }
  }
  present[25 * names + 2] = 0;
  return al::Panel::create(dates, names, {"close", "raw_close", "volume"},
      std::move(fields), std::move(present));
}
std::vector<u8> members() {
  std::vector<u8> m(dates * names, 1);
  for (usize d = 0; d < dates; ++d) for (usize i = 0; i < names; ++i)
    if (i >= 60 || (i < 8 && d < 80)) m[d * names + i] = 0;
  return m;
}
} // namespace

TEST(StrategyIcComposition, Actual48DslArtifactRunsWithBoundedLookbackAndCausalMask) {
  const auto directory = std::filesystem::path{ATX_IMPL_TESTS_DIR}.parent_path() / "strategies";
  std::ifstream f(directory / "slow_price_volume_ic48_v1.json", std::ios::binary); ASSERT_TRUE(f);
  const std::string bytes{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
  const auto library = nlohmann::json::parse(bytes);
  std::ifstream rf(directory / "slow_price_volume_ic48_v1.recipe.json", std::ios::binary); ASSERT_TRUE(rf);
  const auto recipe = nlohmann::json::parse(rf);
  auto sha = atx::core::sha256_hex(bytes); ASSERT_TRUE(sha);
  EXPECT_EQ(*sha, recipe.at("library").at("sha256").get<std::string>());
  ASSERT_EQ(library.at("schema"), "atx.dsl-ic-library/v1");
  ASSERT_EQ(library.at("candidates").size(), 48U); ASSERT_EQ(library.at("families").size(), 8U);
  auto p = panel(false), changed = panel(true); ASSERT_TRUE(p); ASSERT_TRUE(changed);
  al::Engine engine{*p}, other{*changed};
  engine.set_eval_mode(al::EvalMode::ResearchFast); other.set_eval_mode(al::EvalMode::ResearchFast);
  const auto member = members();
  ASSERT_TRUE(engine.set_cross_section_mask(member)); ASSERT_TRUE(other.set_cross_section_mask(member));
  al::Library ops; std::set<std::string> ids, expressions; std::map<std::string, usize> families;
  usize largest = 0, index = 0;
  for (const auto& candidate : library.at("candidates")) {
    const auto id = candidate.at("id").get<std::string>(); SCOPED_TRACE(id);
    ASSERT_TRUE(ids.insert(id).second); ++families[candidate.at("family").get<std::string>()];
    const auto dsl = candidate.at("dsl").get<std::string>(); ASSERT_TRUE(expressions.insert(dsl).second);
    EXPECT_EQ(candidate.at("sign_policy"), "train-rank-ic21");
    EXPECT_EQ(candidate.at("horizons"), nlohmann::json::array({5, 21, 63}));
    auto ast = al::parse_expr(dsl, ops); ASSERT_TRUE(ast) << ast.error().message();
    auto analysis = al::analyze(*ast); ASSERT_TRUE(analysis) << analysis.error().message();
    largest = std::max(largest, static_cast<usize>(analysis->required_lookback()));
    EXPECT_LE(analysis->required_lookback(), 314U);
    EXPECT_EQ(analysis->required_lookback(), recipe.at("lineage").at(index).at("prior_bars").get<usize>());
    auto program = al::compile(*ast, *analysis); ASSERT_TRUE(program) << program.error().message();
    EXPECT_LE(program->num_slots, 64U);
    auto actual = engine.evaluate(*program), mutated = other.evaluate(*program);
    ASSERT_TRUE(actual) << actual.error().message(); ASSERT_TRUE(mutated) << mutated.error().message();
    ASSERT_EQ(actual->alphas.size(), 1U); ASSERT_EQ(mutated->alphas.size(), 1U);
    const auto& v = actual->alphas.front().values; const auto& x = mutated->alphas.front().values;
    ASSERT_EQ(v.size(), dates * names); ASSERT_EQ(x.size(), v.size());
    usize ready = 0;
    for (usize i = 0; i < 60; ++i) ready += std::isfinite(v[345 * names + i]);
    EXPECT_GE(ready, 48U);
    usize mismatches = 0;
    for (usize d = 0; d < future; ++d) for (usize i = 0; i < 60; ++i)
      mismatches += std::bit_cast<u64>(v[d * names + i]) != std::bit_cast<u64>(x[d * names + i]);
    EXPECT_EQ(mismatches, 0U);
    for (usize i = 60; i < names; ++i) EXPECT_FALSE(std::isfinite(v[345 * names + i]));
    ++index;
  }
  EXPECT_EQ(largest, 314U); EXPECT_EQ(families.size(), 8U);
  for (const auto& [family, count] : families) { SCOPED_TRACE(family); EXPECT_EQ(count, 6U); }
  // Syntax/readiness/causality only; no empirical profitability qualification.
}

TEST(StrategyIcComposition, FixedFamilyBudgetsNeutralMissingAndPlannedExitAccounting) {
  const std::vector<st::IcCompositionCandidate> candidates{{"a1", "a"}, {"a2", "a"}, {"b1", "b"}};
  st::IcCompositionConfig cfg; cfg.dates = 6; cfg.instruments = 4; cfg.decision_end = 6;
  std::vector<u8> member(24, 1);
  for (usize d = 1; d < 6; ++d) member[d * 4 + 3] = 0;
  std::vector<f64> increasing(24), decreasing(24);
  for (usize d = 0; d < 6; ++d) for (usize i = 0; i < 4; ++i) {
    increasing[d * 4 + i] = static_cast<f64>(i + 1); decreasing[d * 4 + i] = static_cast<f64>(4 - i);
  }
  auto c = st::IcComposition::create(cfg, candidates, member); ASSERT_TRUE(c);
  EXPECT_FALSE(c->add(1, increasing, 1)); // refusal must leave next-index unchanged
  ASSERT_TRUE(c->add(0, increasing, 1)); ASSERT_TRUE(c->add(1, {}, 0)); ASSERT_TRUE(c->add(2, decreasing, -1));
  EXPECT_FALSE(c->add(2, decreasing, -1));
  auto out = c->finish(); ASSERT_TRUE(out);
  EXPECT_DOUBLE_EQ(out->signal[0], -.375); EXPECT_NEAR(out->signal[1], -.125, 1e-16);
  EXPECT_NEAR(out->signal[2], .125, 1e-16); EXPECT_DOUBLE_EQ(out->signal[3], .375);
  EXPECT_DOUBLE_EQ(out->contribution_fraction[0], .75); // missing a2 budget remains neutral
  EXPECT_TRUE(std::isnan(out->signal[7])); EXPECT_EQ(out->eligible_names[1], 3U);
  EXPECT_NEAR(out->planned_turnover[0], .25, 1e-15); EXPECT_NEAR(out->deployment_turnover, .25, 1e-15);
  EXPECT_EQ(out->deployment_date, 0U);
  EXPECT_NEAR(out->planned_turnover[1], .09375, 1e-15); // forced off-cadence exit, no survivor renorm
  EXPECT_NEAR(out->planned_gross[1], .15625, 1e-15); EXPECT_NEAR(out->planned_net[1], -.09375, 1e-15);
  EXPECT_DOUBLE_EQ(out->planned_turnover[2], 0); EXPECT_DOUBLE_EQ(out->planned_turnover[4], 0);
  EXPECT_NEAR(out->planned_net[5], -.0703125, 1e-16);
  f64 total = 0; for (auto v : out->planned_turnover) total += v;
  EXPECT_DOUBLE_EQ(out->total_planned_turnover, total); EXPECT_FALSE(c->finish());
}

TEST(StrategyIcComposition, UnorientedAndConstantSignalsCannotInventPositions) {
  const std::vector<st::IcCompositionCandidate> candidates{{"a", "one"}, {"b", "two"}};
  st::IcCompositionConfig cfg; cfg.dates = 4; cfg.instruments = 3; cfg.decision_begin = 1; cfg.decision_end = 4;
  std::vector<u8> member(12, 1); std::vector<f64> constant(12, 7);
  auto c = st::IcComposition::create(cfg, candidates, member); ASSERT_TRUE(c);
  ASSERT_TRUE(c->add(0, {}, 0)); ASSERT_TRUE(c->add(1, constant, 1)); auto out = c->finish(); ASSERT_TRUE(out);
  for (auto v : out->signal) EXPECT_DOUBLE_EQ(v, 0);
  EXPECT_TRUE(std::isnan(out->planned_turnover[0])); EXPECT_EQ(out->deployment_date, cfg.dates);
  EXPECT_DOUBLE_EQ(out->total_planned_turnover, 0);
  for (usize d = 1; d < 4; ++d) { EXPECT_DOUBLE_EQ(out->planned_gross[d], 0); EXPECT_DOUBLE_EQ(out->planned_net[d], 0); }
  EXPECT_DOUBLE_EQ(out->contribution_fraction[1], .5); // observed constant is covered, but uninvested
}

TEST(StrategyIcComposition, FuturePayloadAndMembershipDoNotChangePrefix) {
  const std::vector<st::IcCompositionCandidate> candidates{{"a", "one"}};
  st::IcCompositionConfig cfg; cfg.dates = 8; cfg.instruments = 4; cfg.decision_end = 8;
  std::vector<u8> member(32, 1), changed_member = member;
  std::vector<f64> signal(32), changed_signal(32);
  for (usize d = 0; d < 8; ++d) for (usize i = 0; i < 4; ++i) {
    signal[d * 4 + i] = static_cast<f64>((i + d) % 4);
    changed_signal[d * 4 + i] = d < 4 ? signal[d * 4 + i] : 1000 - signal[d * 4 + i];
    if (d >= 4 && i == 3) changed_member[d * 4 + i] = 0;
  }
  auto a = st::IcComposition::create(cfg, candidates, member), b = st::IcComposition::create(cfg, candidates, changed_member);
  ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(a->add(0, signal, 1)); ASSERT_TRUE(b->add(0, changed_signal, 1));
  auto x = a->finish(), y = b->finish(); ASSERT_TRUE(x); ASSERT_TRUE(y);
  for (usize i = 0; i < 16; ++i) EXPECT_EQ(std::bit_cast<u64>(x->signal[i]), std::bit_cast<u64>(y->signal[i]));
  for (usize d = 0; d < 4; ++d) {
    EXPECT_DOUBLE_EQ(x->planned_turnover[d], y->planned_turnover[d]);
    EXPECT_DOUBLE_EQ(x->planned_gross[d], y->planned_gross[d]);
    EXPECT_DOUBLE_EQ(x->planned_net[d], y->planned_net[d]);
  }
}

TEST(StrategyIcComposition, AdmissionPrecedesAllocationAndIncompleteInputRefuses) {
  const std::vector<st::IcCompositionCandidate> candidates{{"a", "one"}};
  st::IcCompositionConfig cfg; cfg.dates = 1000000; cfg.instruments = 1000000; cfg.decision_end = 2; cfg.max_working_bytes = 1024;
  auto refused = st::IcComposition::create(cfg, candidates, {}); ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), atx::core::ErrorCode::OutOfRange);
  EXPECT_FALSE(st::ic_composition_working_bytes(std::numeric_limits<usize>::max(), 2, 1));
  cfg.dates = 2; cfg.instruments = 2; cfg.max_working_bytes = 1ULL << 20;
  std::vector<u8> member(4, 1); auto c = st::IcComposition::create(cfg, candidates, member); ASSERT_TRUE(c);
  EXPECT_FALSE(c->finish()); EXPECT_FALSE(c->add(0, {}, 2)); EXPECT_FALSE(c->add(0, {}, 1));
  ASSERT_TRUE(c->add(0, {}, 0)); EXPECT_TRUE(c->finish());
  member[0] = 2; EXPECT_FALSE(st::IcComposition::create(cfg, candidates, member));
  const std::vector<st::IcCompositionCandidate> duplicate{{"a", "one"}, {"a", "two"}};
  member[0] = 1; EXPECT_FALSE(st::IcComposition::create(cfg, duplicate, member));
}

// T15: a pooled add splits dates into bands with one ranked row per worker; the
// blend, coverage and planned-target proxy equal the serial path bit for bit for
// every worker count, with ties, NaN/inf, signed zeros, nonmembers, a neutral sign
// and a pinned zero weight.
TEST(StrategyIcComposition, PooledAddMatchesSerialBitsForEveryWorkerCount) {
  const std::vector<st::IcCompositionCandidate> candidates{
      {"a1", "a"}, {"a2", "a"}, {"b1", "b"}, {"b2", "b"}};
  constexpr usize days = 37, width = 11;
  st::IcCompositionConfig cfg; cfg.dates = days; cfg.instruments = width;
  cfg.decision_begin = 5; cfg.decision_end = days;
  std::vector<u8> member(days * width, 1);
  std::vector<std::vector<f64>> signals(candidates.size(), std::vector<f64>(days * width));
  for (usize d = 0; d < days; ++d) for (usize i = 0; i < width; ++i) {
    const auto at = d * width + i; const auto t = static_cast<f64>(d), n = static_cast<f64>(i);
    if ((d + 2 * i) % 7 == 0) member[at] = 0;
    signals[0][at] = std::sin(.3 * t + 1.7 * n);                        // distinct values
    signals[1][at] = static_cast<f64>((d * 3 + i) % 4);                 // heavy ties
    signals[2][at] = (d + i) % 5 == 0 ? std::numeric_limits<f64>::quiet_NaN()
        : i % 3 == 0 ? (i % 2 == 1 ? -0.0 : 0.0) : static_cast<f64>(i % 3); // NaN, +/-0 ties
    signals[3][at] = d < 3 ? std::numeric_limits<f64>::infinity() : -n * t; // non-finite prefix
  }
  const auto compose = [&](const std::vector<int>& signs, std::span<const f64> weights,
                           atx::engine::parallel::DetPool* pool) -> std::optional<st::IcCompositionResult> {
    auto c = st::IcComposition::create(cfg, candidates, member, weights);
    if (!c) return std::nullopt;
    for (usize k = 0; k < candidates.size(); ++k)
      if (!c->add(k, signals[k], signs[k], pool)) return std::nullopt;
    auto out = c->finish();
    if (!out) return std::nullopt;
    return std::move(*out);
  };
  const auto same_bits = [](const std::vector<f64>& a, const std::vector<f64>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](f64 x, f64 y) {
      return std::bit_cast<u64>(x) == std::bit_cast<u64>(y);
    });
  };
  const std::vector<f64> pinned{.5, 0.0, .25, .25};
  for (const auto& [signs, weights] : {std::pair{std::vector<int>{1, -1, 1, -1}, std::vector<f64>{}},
                                       std::pair{std::vector<int>{0, 1, -1, 1}, pinned}}) {
    const auto serial = compose(signs, weights, nullptr); ASSERT_TRUE(serial);
    for (const usize workers : {usize{2}, usize{3}, usize{4}}) {
      SCOPED_TRACE(workers);
      atx::engine::parallel::DetPool pool(workers);
      const auto pooled = compose(signs, weights, &pool); ASSERT_TRUE(pooled);
      EXPECT_TRUE(same_bits(pooled->signal, serial->signal));
      EXPECT_TRUE(same_bits(pooled->contribution_fraction, serial->contribution_fraction));
      EXPECT_TRUE(same_bits(pooled->planned_turnover, serial->planned_turnover));
      EXPECT_TRUE(same_bits(pooled->planned_gross, serial->planned_gross));
      EXPECT_TRUE(same_bits(pooled->planned_net, serial->planned_net));
      EXPECT_EQ(pooled->eligible_names, serial->eligible_names);
      EXPECT_EQ(std::bit_cast<u64>(pooled->total_planned_turnover),
                std::bit_cast<u64>(serial->total_planned_turnover));
      EXPECT_EQ(pooled->deployment_date, serial->deployment_date);
    }
  }
}

// V6-W (fitter ew-theme-v6, rule within-theme-v1): per name and date a member missing
// for that name keeps its mass inside its theme, a theme with no present member adds
// nothing, a fully present date equals the fixed-denominator blend, nonmembers stay
// NaN, the pooled path equals the serial bits and bad theme inputs refuse. The same
// hand numbers are the fitter test's ref_within_theme_blend case.
TEST(StrategyIcComposition, WithinThemeRedistributionKeepsMissingMassInTheme) {
  const std::vector<st::IcCompositionCandidate> candidates{{"a1", "a"}, {"a2", "a"}, {"b1", "b"}};
  st::IcCompositionConfig cfg; cfg.dates = 2; cfg.instruments = 4; cfg.decision_end = 2;
  std::vector<u8> member(8, 1); member[7] = 0;
  constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();
  // date 0: a2 finite on names 2, 3 only, b1 missing on name 0; date 1: all present (name 3 not a member)
  const std::vector<f64> a1{1, 2, 3, 4, 1, 2, 3, 4}, a2{missing, missing, 2, 1, 4, 1, 3, 2},
      b1{missing, 3, 2, 1, 2, 4, 1, 3};
  const std::vector<f64> pinned{.25, .25, .5};
  const std::vector<usize> themes{0, 0, 1};
  const auto compose = [&](std::span<const usize> t, atx::engine::parallel::DetPool* pool)
      -> std::optional<st::IcCompositionResult> {
    auto c = st::IcComposition::create(cfg, candidates, member, pinned, t);
    if (!c || !c->add(0, a1, 1, pool) || !c->add(1, a2, 1, pool) || !c->add(2, b1, 1, pool)) return std::nullopt;
    auto out = c->finish();
    if (!out) return std::nullopt;
    return std::move(*out);
  };
  const auto themed = compose(themes, nullptr), fixed = compose({}, nullptr);
  ASSERT_TRUE(themed); ASSERT_TRUE(fixed);
  // date 0: a1 ranks -.5,-1/6,1/6,.5; a2 (names 2,3) .5,-.5; b1 (names 1..3) .5,0,-.5; W_a = W_b = .5
  EXPECT_NEAR(themed->signal[0], -.25, 1e-15);      // theme a = a1 alone; theme b absent adds nothing
  EXPECT_NEAR(themed->signal[1], 1.0 / 6, 1e-15);
  EXPECT_NEAR(themed->signal[2], 1.0 / 6, 1e-15);
  EXPECT_NEAR(themed->signal[3], -.25, 1e-15);
  EXPECT_NEAR(fixed->signal[0], -.125, 1e-15);      // fixed denominator: missing members add zero
  EXPECT_NEAR(fixed->signal[1], 5.0 / 24, 1e-15);
  for (usize i = 4; i < 7; ++i) EXPECT_NEAR(themed->signal[i], fixed->signal[i], 1e-15);
  EXPECT_TRUE(std::isnan(themed->signal[7])); EXPECT_TRUE(std::isnan(fixed->signal[7]));
  EXPECT_DOUBLE_EQ(themed->contribution_fraction[0], fixed->contribution_fraction[0]); // report-only coverage
  for (const usize workers : {usize{2}, usize{3}}) {
    SCOPED_TRACE(workers);
    atx::engine::parallel::DetPool pool(workers);
    const auto pooled = compose(themes, &pool); ASSERT_TRUE(pooled);
    for (usize i = 0; i < 8; ++i)
      EXPECT_EQ(std::bit_cast<u64>(pooled->signal[i]), std::bit_cast<u64>(themed->signal[i]));
    EXPECT_EQ(std::bit_cast<u64>(pooled->total_planned_turnover), std::bit_cast<u64>(themed->total_planned_turnover));
  }
  // refusals: themes without pinned weights, a size mismatch, a weighted index >= 32, no weighted candidate
  const std::vector<f64> zeros{0, 0, 0}, sparse{.5, 0, .5};
  const std::vector<usize> short_themes{0, 0}, far{0, 32, 1}, ignored{0, 99, 1};
  EXPECT_FALSE(st::IcComposition::create(cfg, candidates, member, {}, themes));
  EXPECT_FALSE(st::IcComposition::create(cfg, candidates, member, pinned, short_themes));
  EXPECT_FALSE(st::IcComposition::create(cfg, candidates, member, pinned, far));
  EXPECT_FALSE(st::IcComposition::create(cfg, candidates, member, zeros, themes));
  EXPECT_TRUE(st::IcComposition::create(cfg, candidates, member, sparse, ignored)); // zero weight: index ignored
  // working-bytes envelope: 16 B per cell per theme, none for the default path
  const auto base = st::ic_composition_working_bytes(2, 4, 3), two = st::ic_composition_working_bytes(2, 4, 3, 2);
  ASSERT_TRUE(base); ASSERT_TRUE(two); EXPECT_EQ(*two - *base, 2U * 8U * 16U);
  EXPECT_FALSE(st::ic_composition_working_bytes(2, 4, 3, 33));
}

// ---- platform v8 R-1: composition ew-theme-std-v1 (IcThemeRule::standardise) ----
namespace {
constexpr f64 v8_nan = std::numeric_limits<f64>::quiet_NaN();
// Literal reference, independent of the kernels: the centred tied rank of values[i] among
// the entries with keep[j]: (less + (less + equal - 1)) / (2 (n - 1)) - 0.5.
std::vector<f64> v8_ref_ranks(const std::vector<f64>& values, const std::vector<bool>& keep) {
  std::vector<f64> out(values.size(), v8_nan);
  usize n = 0;
  for (usize i = 0; i < values.size(); ++i) n += keep[i] ? 1U : 0U;
  if (n < 2) return out;
  for (usize i = 0; i < values.size(); ++i) {
    if (!keep[i]) continue;
    usize less = 0, equal = 0;
    for (usize j = 0; j < values.size(); ++j) {
      if (!keep[j]) continue;
      less += values[j] < values[i] ? 1U : 0U;
      equal += values[j] == values[i] ? 1U : 0U;
    }
    out[i] = (static_cast<f64>(less) + static_cast<f64>(less + equal - 1)) / (2.0 * static_cast<f64>(n - 1)) - 0.5;
  }
  return out;
}
// Reference ew-theme-std-v1 blend: per date and theme the sum of present members'
// w * s * rank, re-ranked over names with a present member, times W_theme = sum of w.
std::vector<f64> v8_ref_blend(usize days, usize width, const std::vector<u8>& member,
                              const std::vector<std::vector<f64>>& signals, const std::vector<f64>& w,
                              const std::vector<int>& s, const std::vector<usize>& theme, usize themes) {
  std::vector<f64> out(days * width, 0.0);
  for (usize d = 0; d < days; ++d) {
    std::vector<std::vector<f64>> sum(themes, std::vector<f64>(width, 0.0));
    std::vector<std::vector<bool>> present(themes, std::vector<bool>(width, false));
    std::vector<f64> mass(themes, 0.0);
    for (usize k = 0; k < signals.size(); ++k) {
      if (!(w[k] > 0) || s[k] == 0) continue;
      mass[theme[k]] += w[k];
      std::vector<f64> row(width); std::vector<bool> keep(width);
      for (usize i = 0; i < width; ++i) {
        row[i] = signals[k][d * width + i];
        keep[i] = member[d * width + i] != 0 && std::isfinite(row[i]);
      }
      const auto r = v8_ref_ranks(row, keep);
      for (usize i = 0; i < width; ++i) if (!std::isnan(r[i])) {
        sum[theme[k]][i] += static_cast<f64>(s[k]) * w[k] * r[i]; present[theme[k]][i] = true;
      }
    }
    for (usize t = 0; t < themes; ++t) {
      const auto rr = v8_ref_ranks(sum[t], present[t]);
      for (usize i = 0; i < width; ++i) if (!std::isnan(rr[i])) out[d * width + i] += mass[t] * rr[i];
    }
    for (usize i = 0; i < width; ++i) if (!member[d * width + i]) out[d * width + i] = v8_nan;
  }
  return out;
}
f64 v8_sd(const std::vector<f64>& v, usize begin, usize end) {
  f64 mean = 0;
  for (usize k = begin; k < end; ++k) mean += v[k];
  mean /= static_cast<f64>(end - begin);
  f64 ss = 0;
  for (usize k = begin; k < end; ++k) ss += (v[k] - mean) * (v[k] - mean);
  return std::sqrt(ss / static_cast<f64>(end - begin));
}
struct V8Fixture {
  static constexpr usize days = 3, width = 9;
  // Theme a: one member; theme b: three members whose ranks disagree (i, (i + 8) % 9 and
  // (8 i) % 9, rotated per date), so b's plain mean rank has a third of a's dispersion.
  std::vector<st::IcCompositionCandidate> candidates{{"a1", "a"}, {"b1", "b"}, {"b2", "b"}, {"b3", "b"}};
  std::vector<usize> themes{0, 1, 1, 1};
  std::vector<int> signs{1, 1, 1, 1};
  std::vector<u8> member = std::vector<u8>(days * width, 1);
  std::vector<std::vector<f64>> signals = std::vector<std::vector<f64>>(4, std::vector<f64>(days * width));
  st::IcCompositionConfig cfg;
  V8Fixture() {
    cfg.dates = days; cfg.instruments = width; cfg.decision_end = days;
    for (usize d = 0; d < days; ++d) for (usize i = 0; i < width; ++i) {
      const auto at = d * width + i, j = (i + d) % width;
      signals[0][at] = static_cast<f64>(j);
      signals[1][at] = static_cast<f64>(j);
      signals[2][at] = static_cast<f64>((j + 8) % width);
      signals[3][at] = static_cast<f64>((8 * j) % width);
    }
  }
  std::optional<st::IcCompositionResult> compose(const std::vector<f64>& weights, std::span<const usize> t,
                                                 st::IcThemeRule rule,
                                                 atx::engine::parallel::DetPool* pool = nullptr) const {
    auto c = st::IcComposition::create(cfg, candidates, member, weights, t, rule);
    if (!c) return std::nullopt;
    for (usize k = 0; k < candidates.size(); ++k)
      if (!c->add(k, signals[k], signs[k], pool)) return std::nullopt;
    auto out = c->finish();
    if (!out) return std::nullopt;
    return std::move(*out);
  }
};
} // namespace

// S-1: under ew-theme-v1 the one-member theme carries about three times the dispersion of
// a theme whose members disagree; after the re-rank both enter with W_theme times the same
// grid of centred ranks, so equal theme weights are equal effective weights.
TEST(CompositionV8, OneMemberThemeHasSameDispersionAsOthers) {
  const V8Fixture f;
  const auto std_rule = st::IcThemeRule::standardise;
  const std::vector<f64> a_only{.5, 0, 0, 0}, b_only{0, 1.0 / 6, 1.0 / 6, 1.0 / 6}, both{.5, 1.0 / 6, 1.0 / 6, 1.0 / 6};
  const f64 w_a = .5, w_b = 0.0 + 1.0 / 6 + 1.0 / 6 + 1.0 / 6; // W_theme as the composition sums it
  const auto a = f.compose(a_only, f.themes, std_rule), b = f.compose(b_only, f.themes, std_rule);
  const auto plain_b = f.compose(b_only, {}, std_rule); // no themes: the ew-theme-v1 pinned path
  const auto full = f.compose(both, f.themes, std_rule);
  ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(plain_b); ASSERT_TRUE(full);
  for (usize d = 0; d < V8Fixture::days; ++d) {
    SCOPED_TRACE(d);
    const usize begin = d * V8Fixture::width, end = begin + V8Fixture::width;
    const f64 grid = v8_sd(a->signal, begin, end) / w_a;
    EXPECT_GT(grid, .3);
    EXPECT_NEAR(v8_sd(b->signal, begin, end) / w_b, grid, 1e-12);
    EXPECT_NEAR(v8_sd(plain_b->signal, begin, end) / w_b, grid / 3, 1e-12); // before: a third
  }
  // Themes fold in index order onto a zero blend: the two-theme blend is the sum of the parts.
  for (usize k = 0; k < full->signal.size(); ++k)
    EXPECT_EQ(std::bit_cast<u64>(full->signal[k]), std::bit_cast<u64>(a->signal[k] + b->signal[k])) << k;
  const auto ref = v8_ref_blend(V8Fixture::days, V8Fixture::width, f.member, f.signals, both, f.signs, f.themes, 2);
  for (usize k = 0; k < ref.size(); ++k) EXPECT_NEAR(full->signal[k], ref[k], 1e-15) << k;
}

// Rule 4 (member cap 1/(2T), excess pro rata to the other themes) is fitted by
// composition_rules.py; the runner-side contract is that a theme enters with W_theme =
// the sum of its pinned (capped) member weights. T = 2: the one-member theme a is capped
// from 1/2 to 1/4 and theme b's members rise from 1/6 to 1/4 each. Missing members stay
// neutral (no redistribution), a theme with no present member adds nothing, nonmembers
// stay NaN, and the pooled path equals the serial bits.
TEST(CompositionV8, MemberCapRedistributes) {
  V8Fixture f;
  f.signals[2][0] = v8_nan;                         // b2 missing for name 0 on date 0: b = b1 + b3 there
  f.signals[0][V8Fixture::width + 1] = v8_nan;      // a1 missing for name 1 on date 1: theme a absent there
  f.member[2 * V8Fixture::width + 8] = 0;           // name 8 leaves on date 2
  const auto std_rule = st::IcThemeRule::standardise;
  const std::vector<f64> uncapped{.5, 1.0 / 6, 1.0 / 6, 1.0 / 6}, capped{.25, .25, .25, .25};
  const auto before = f.compose(uncapped, f.themes, std_rule), after = f.compose(capped, f.themes, std_rule);
  ASSERT_TRUE(before); ASSERT_TRUE(after);
  for (const auto& [weights, out] : {std::pair{uncapped, before}, std::pair{capped, after}}) {
    const auto ref = v8_ref_blend(V8Fixture::days, V8Fixture::width, f.member, f.signals, weights, f.signs,
                                  f.themes, 2);
    for (usize k = 0; k < ref.size(); ++k) {
      if (std::isnan(ref[k])) { EXPECT_TRUE(std::isnan(out->signal[k])) << k; continue; }
      EXPECT_NEAR(out->signal[k], ref[k], 1e-15) << k;
    }
  }
  // Name 1 on date 1 has no present member of theme a: only W_b * rank_b reaches it, and
  // the cap moved mass .25 from a to b.
  const auto at = V8Fixture::width + 1;
  EXPECT_NEAR(after->signal[at] / .75, before->signal[at] / .5, 1e-15);
  EXPECT_TRUE(std::isnan(after->signal[2 * V8Fixture::width + 8]));
  for (const usize workers : {usize{2}, usize{3}}) {
    SCOPED_TRACE(workers);
    atx::engine::parallel::DetPool pool(workers);
    const auto pooled = f.compose(capped, f.themes, std_rule, &pool); ASSERT_TRUE(pooled);
    for (usize k = 0; k < pooled->signal.size(); ++k)
      EXPECT_EQ(std::bit_cast<u64>(pooled->signal[k]), std::bit_cast<u64>(after->signal[k])) << k;
    for (usize d = 0; d < V8Fixture::days; ++d)
      EXPECT_EQ(std::bit_cast<u64>(pooled->contribution_fraction[d]),
                std::bit_cast<u64>(after->contribution_fraction[d]));
  }
  // Envelope: one f64 plane per theme (8 B per cell per theme), half of redistribute's.
  const auto base = st::ic_composition_working_bytes(3, 9, 4);
  const auto two = st::ic_composition_working_bytes(3, 9, 4, 2, std_rule);
  ASSERT_TRUE(base); ASSERT_TRUE(two); EXPECT_EQ(*two - *base, 2U * 27U * 8U);
}

// Without themes the rule changes nothing (the ew-theme-v1 pinned path, bit for bit: the
// runner's rerank-off identity relies on it, see CompositionV8.IdentityWithReRankAndCapOffIsEwThemeV1
// in strategy_ic_runner_test.cpp); with themes it needs pinned weights and a weighted member.
TEST(CompositionV8, StandardiseNeedsThemesAndPinnedWeights) {
  const V8Fixture f;
  const auto std_rule = st::IcThemeRule::standardise;
  const std::vector<f64> w{.5, 1.0 / 6, 1.0 / 6, 1.0 / 6};
  // No themes: the rule is ignored and the blend is the pinned path's, bit for bit.
  const auto plain = f.compose(w, {}, st::IcThemeRule::redistribute), ignored = f.compose(w, {}, std_rule);
  ASSERT_TRUE(plain); ASSERT_TRUE(ignored);
  for (usize k = 0; k < plain->signal.size(); ++k)
    EXPECT_EQ(std::bit_cast<u64>(plain->signal[k]), std::bit_cast<u64>(ignored->signal[k]));
  EXPECT_EQ(std::bit_cast<u64>(plain->total_planned_turnover), std::bit_cast<u64>(ignored->total_planned_turnover));
  // Themes need pinned weights and a weighted member; indices < 32.
  const std::vector<f64> zeros(4, 0.0);
  const std::vector<usize> far{0, 1, 1, 32};
  EXPECT_FALSE(st::IcComposition::create(f.cfg, f.candidates, f.member, {}, f.themes, std_rule));
  EXPECT_FALSE(st::IcComposition::create(f.cfg, f.candidates, f.member, zeros, f.themes, std_rule));
  EXPECT_FALSE(st::IcComposition::create(f.cfg, f.candidates, f.member, w, far, std_rule));
}

// ---- review T-1: rule 1 of ew-theme-std-v1 against the registration written out ----
namespace {
// The five registered rules of ew-theme-std-v1 (docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md, Task R-1), written
// out from the text, independently of the fitter (composition_rules.py) and of the runner's kernels:
//   1. per date and theme, the weighted mean of the theme's signed member ranks; within-theme weights are
//      proportional to the tier score;
//   2. re-rank that composite across names with at least one present member (centred tied rank in [-.5, .5]);
//   3. blend = sum over themes of (1/T) x re-ranked composite; missing stays neutral, nothing is redistributed
//      inside a theme;
//   4. member cap: no member's weight above 1/(2T); the excess goes pro rata to the other themes;
//   5. tier re-grades: res_mom_12_1 B+ to B-, ear B+ to C+, sue C+ stays, ins_opp B- to C+.
// Rules 5, 1 (the within-theme shares), 3 (1/T) and 4 fix one weight per member (rule_weights); rules 1-3 then apply
// per date (rule_blend). After the cap a theme enters with W_theme = the sum of its members' weights, not 1/T.
struct RuleMember {
  const char* id;
  usize theme;
  int sign;
  const char* source_tier;
};
// Rule 5, verbatim (a re-graded member must carry the declared from-grade).
std::string rule_regraded_tier(const std::string& id, const std::string& tier) {
  struct Regrade {
    const char* name;
    const char* from;
    const char* to;
  };
  constexpr std::array<Regrade, 4> regrades{{{"res_mom_12_1", "B+", "B-"}, {"ear", "B+", "C+"}, {"sue", "C+", "C+"},
                                             {"ins_opp", "B-", "C+"}}};
  for (const auto& r : regrades) {
    if (id != r.name) continue;
    EXPECT_EQ(tier, r.from) << id;
    return r.to;
  }
  return tier;
}
// The declared tier scores (registry schema, task A-1).
f64 rule_tier_score(const std::string& tier) {
  if (tier == "A") return 1.0;
  if (tier == "A-") return .9;
  if (tier == "B+") return .8;
  if (tier == "B") return .7;
  if (tier == "B-") return .55;
  if (tier == "C+") return .4;
  ADD_FAILURE() << "tier without a declared score: " << tier;
  return 0.0;
}
// Rules 5, 1 and 3: w_k = (1/T) score_k / (the sum of its theme's scores). Rule 4: repeat until no member is above
// the cap: every member above it is set to the cap (and keeps it), and each theme's excess goes to the members of the
// other themes not at the cap, pro rata to their weights before the pass's additions.
std::vector<f64> rule_weights(const std::vector<RuleMember>& members, usize themes) {
  const usize n = members.size();
  const f64 t_count = static_cast<f64>(themes);
  std::vector<f64> score(n), theme_score(themes, 0.0), w(n);
  for (usize k = 0; k < n; ++k) {
    score[k] = rule_tier_score(rule_regraded_tier(members[k].id, members[k].source_tier));
    theme_score[members[k].theme] += score[k];
  }
  for (usize k = 0; k < n; ++k) w[k] = score[k] / (t_count * theme_score[members[k].theme]);
  const f64 cap = 1.0 / (2.0 * t_count);
  std::vector<bool> capped(n, false);
  for (usize pass = 0; pass <= n; ++pass) {
    std::vector<f64> excess(themes, 0.0);
    bool over = false;
    for (usize k = 0; k < n; ++k) {
      if (capped[k] || !(w[k] > cap * (1.0 + 1e-12))) continue;
      excess[members[k].theme] += w[k] - cap;
      w[k] = cap;
      capped[k] = true;
      over = true;
    }
    if (!over) break;
    const std::vector<f64> before = w;
    for (usize t = 0; t < themes; ++t) {
      if (!(excess[t] > 0)) continue;
      f64 mass = 0;
      for (usize j = 0; j < n; ++j)
        if (!capped[j] && members[j].theme != t) mass += before[j];
      for (usize j = 0; j < n; ++j)
        if (!capped[j] && members[j].theme != t) w[j] += excess[t] * before[j] / mass;
    }
  }
  return w;
}
// Rule 1 as registered, and the wrong readings of it a test must be able to reject.
enum class RuleOne { registered, unsigned_ranks, equal_weights, first_member_only, last_member_wins };
// Rules 1-3 per date: a theme's composite is sum_{k present} w_k s_k r_k / W_theme (the weighted mean; a missing
// member adds nothing), re-ranked over the names with a present member, times W_theme; nonmembers NaN. `min_gap`
// (optional) receives the smallest gap between two present names' composites over every date and theme.
std::vector<f64> rule_blend(usize days, usize width, const std::vector<u8>& member,
                            const std::vector<std::vector<f64>>& signals, const std::vector<RuleMember>& members,
                            const std::vector<f64>& w, usize themes, RuleOne variant, f64* min_gap = nullptr) {
  const usize n = members.size();
  std::vector<f64> mass(themes, 0.0), count(themes, 0.0), weight = w;
  for (usize k = 0; k < n; ++k) {
    mass[members[k].theme] += w[k];
    count[members[k].theme] += 1.0;
  }
  if (variant == RuleOne::equal_weights)
    for (usize k = 0; k < n; ++k) weight[k] = mass[members[k].theme] / count[members[k].theme];
  std::vector<f64> out(days * width, 0.0);
  for (usize d = 0; d < days; ++d) {
    std::vector<std::vector<f64>> sum(themes, std::vector<f64>(width, 0.0));
    std::vector<std::vector<bool>> present(themes, std::vector<bool>(width, false));
    std::vector<bool> seen(themes, false);
    for (usize k = 0; k < n; ++k) {
      const usize t = members[k].theme;
      if (variant == RuleOne::first_member_only && seen[t]) continue;
      seen[t] = true;
      std::vector<f64> row(width);
      std::vector<bool> keep(width);
      for (usize i = 0; i < width; ++i) {
        row[i] = signals[k][d * width + i];
        keep[i] = member[d * width + i] != 0 && std::isfinite(row[i]);
      }
      const auto r = v8_ref_ranks(row, keep);
      const f64 s = variant == RuleOne::unsigned_ranks ? 1.0 : static_cast<f64>(members[k].sign);
      for (usize i = 0; i < width; ++i) {
        if (std::isnan(r[i])) continue;
        const f64 term = s * weight[k] * r[i];
        sum[t][i] = variant == RuleOne::last_member_wins ? term : sum[t][i] + term;
        present[t][i] = true;
      }
    }
    for (usize t = 0; t < themes; ++t) {
      std::vector<f64> mean(width, 0.0), listed;
      for (usize i = 0; i < width; ++i) {
        mean[i] = sum[t][i] / mass[t];
        if (present[t][i]) listed.push_back(mean[i]);
      }
      std::sort(listed.begin(), listed.end());
      for (usize j = 1; min_gap != nullptr && j < listed.size(); ++j)
        *min_gap = std::min(*min_gap, listed[j] - listed[j - 1]);
      const auto rr = v8_ref_ranks(mean, present[t]);
      for (usize i = 0; i < width; ++i)
        if (!std::isnan(rr[i])) out[d * width + i] += mass[t] * rr[i];
    }
    for (usize i = 0; i < width; ++i)
      if (!member[d * width + i]) out[d * width + i] = v8_nan;
  }
  return out;
}
} // namespace

// Review T-1: rule 1 -- the weighted mean of a theme's SIGNED member ranks, each member with its OWN weight -- can
// fail here (the V8Fixture above cannot: every sign is +1, the weights inside a theme are equal and b's three ranks
// sum to b1's). Members disagree in order, signs are mixed inside themes b and c, the weights inside a theme are
// unequal tier shares after the rule 5 re-grades and the rule 4 cap (which binds in two passes), a member is missing
// for one name, a theme is absent for another, and two cells are nonmembers. The expected blend is rule_blend; each
// wrong rule 1 (signs dropped, equal weights inside a theme, first member only, last member wins) moves it by more
// than .2 on this fixture.
TEST(CompositionV8, RuleOneIsTheWeightedMeanOfSignedMemberRanks) {
  constexpr usize days = 4, width = 10, themes = 3;
  const std::vector<RuleMember> members{{"a1", 0, 1, "A"},           {"b_lead", 1, 1, "B+"}, {"c_one", 2, -1, "B"},
                                        {"res_mom_12_1", 1, -1, "B+"}, {"c_two", 2, 1, "A-"},  {"ins_opp", 1, 1, "B-"},
                                        {"c_three", 2, -1, "C+"}};
  const std::array<const char*, themes> family{"a", "b", "c"};
  std::vector<st::IcCompositionCandidate> candidates;
  std::vector<usize> theme_of;
  std::vector<int> signs;
  for (const auto& m : members) {
    candidates.push_back({m.id, family[m.theme]});
    theme_of.push_back(m.theme);
    signs.push_back(m.sign);
  }
  // Rules 5, 1, 3 and 4. The cap (1/6) binds: a1 (1/3) is capped and pass 1 scales every other member by 1.25; then
  // b_lead and c_two are capped, b_lead's excess 1/42 goes to c_one and c_three, c_two's 1/48 to res_mom_12_1 (B-
  // after rule 5) and ins_opp (C+ after rule 5). Hand values:
  const auto w = rule_weights(members, themes);
  const std::array<f64, 7> hand{1.0 / 6, 1.0 / 6, 85.0 / 528, 11.0 / 84 + 11.0 / 912, 1.0 / 6, 2.0 / 21 + 1.0 / 114,
                                85.0 / 924};
  ASSERT_EQ(w.size(), hand.size());
  f64 total = 0;
  for (usize k = 0; k < w.size(); ++k) {
    EXPECT_NEAR(w[k], hand[k], 1e-15) << members[k].id;
    total += w[k];
  }
  EXPECT_NEAR(total, 1.0, 1e-15);
  std::vector<u8> member(days * width, 1);
  member[0] = 0;                 // name 0 is not a member on date 0
  member[3 * width + 9] = 0;     // name 9 leaves on date 3
  std::vector<std::vector<f64>> signals(members.size(), std::vector<f64>(days * width));
  for (usize k = 0; k < members.size(); ++k)
    for (usize d = 0; d < days; ++d)
      for (usize i = 0; i < width; ++i) {
        const auto kk = static_cast<f64>(k), dd = static_cast<f64>(d), ii = static_cast<f64>(i);
        signals[k][d * width + i] = std::sin(.9 + 1.7 * kk + .61 * dd + (.37 + .23 * kk) * ii * ii);
      }
  signals[3][1 * width + 2] = v8_nan;                               // res_mom_12_1 missing for name 2 on date 1
  signals[0][2 * width + 5] = v8_nan;                               // theme a absent for name 5 on date 2
  for (const usize k : {usize{1}, usize{3}, usize{5}}) signals[k][4] = v8_nan; // theme b absent for name 4 on date 0
  st::IcCompositionConfig cfg;
  cfg.dates = days;
  cfg.instruments = width;
  cfg.decision_end = days;
  const auto compose = [&](atx::engine::parallel::DetPool* pool) -> std::optional<st::IcCompositionResult> {
    auto c = st::IcComposition::create(cfg, candidates, member, w, theme_of, st::IcThemeRule::standardise);
    if (!c) return std::nullopt;
    for (usize k = 0; k < candidates.size(); ++k)
      if (!c->add(k, signals[k], signs[k], pool)) return std::nullopt;
    auto out = c->finish();
    if (!out) return std::nullopt;
    return std::move(*out);
  };
  const auto got = compose(nullptr);
  ASSERT_TRUE(got);
  f64 gap = std::numeric_limits<f64>::infinity();
  const auto expected = rule_blend(days, width, member, signals, members, w, themes, RuleOne::registered, &gap);
  EXPECT_GT(gap, 1e-4); // no near tie in any composite: the weighted mean's divisor W_theme cannot change a rank
  EXPECT_TRUE(std::isnan(expected[0]) && std::isnan(expected[3 * width + 9]));
  const auto ref = v8_ref_blend(days, width, member, signals, w, signs, theme_of, themes); // the earlier reference
  for (usize k = 0; k < expected.size(); ++k) {
    if (std::isnan(expected[k])) {
      EXPECT_TRUE(std::isnan(got->signal[k])) << k;
      EXPECT_TRUE(std::isnan(ref[k])) << k;
      continue;
    }
    EXPECT_NEAR(got->signal[k], expected[k], 1e-15) << k;
    EXPECT_NEAR(ref[k], expected[k], 1e-15) << k;
  }
  // Every wrong rule 1 is rejected by this fixture.
  for (const auto variant : {RuleOne::unsigned_ranks, RuleOne::equal_weights, RuleOne::first_member_only,
                             RuleOne::last_member_wins}) {
    const auto wrong = rule_blend(days, width, member, signals, members, w, themes, variant);
    f64 moved = 0;
    for (usize k = 0; k < wrong.size(); ++k)
      if (!std::isnan(expected[k])) moved = std::max(moved, std::abs(wrong[k] - expected[k]));
    EXPECT_GT(moved, .2) << "rule 1 variant " << static_cast<int>(variant);
  }
  // Not the degenerate fixture: on no date does the composite of theme b or c order its names as one of its members'
  // signed ranks does.
  for (usize d = 0; d < days; ++d) {
    for (usize t = 1; t < themes; ++t) {
      std::vector<f64> sum(width, 0.0);
      std::vector<bool> present(width, false);
      std::vector<std::vector<f64>> own;
      for (usize k = 0; k < members.size(); ++k) {
        if (members[k].theme != t) continue;
        std::vector<f64> row(width), signed_rank(width, 0.0);
        std::vector<bool> keep(width);
        for (usize i = 0; i < width; ++i) {
          row[i] = signals[k][d * width + i];
          keep[i] = member[d * width + i] != 0 && std::isfinite(row[i]);
        }
        const auto r = v8_ref_ranks(row, keep);
        for (usize i = 0; i < width; ++i) {
          if (std::isnan(r[i])) continue;
          signed_rank[i] = static_cast<f64>(members[k].sign) * r[i];
          sum[i] += static_cast<f64>(members[k].sign) * w[k] * r[i];
          present[i] = true;
        }
        own.push_back(v8_ref_ranks(signed_rank, keep));
      }
      const auto composite = v8_ref_ranks(sum, present);
      for (const auto& r : own) {
        bool same = true;
        for (usize i = 0; i < width; ++i)
          same = same && (std::isnan(r[i]) ? std::isnan(composite[i]) : r[i] == composite[i]);
        EXPECT_FALSE(same) << "date " << d << " theme " << t;
      }
    }
  }
  // The pooled path equals the serial bits.
  for (const usize workers : {usize{2}, usize{3}}) {
    SCOPED_TRACE(workers);
    atx::engine::parallel::DetPool pool(workers);
    const auto pooled = compose(&pool);
    ASSERT_TRUE(pooled);
    for (usize k = 0; k < pooled->signal.size(); ++k)
      EXPECT_EQ(std::bit_cast<u64>(pooled->signal[k]), std::bit_cast<u64>(got->signal[k])) << k;
  }
}
