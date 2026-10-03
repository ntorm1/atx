// mom-volman-v1 (P9 lane AL-COMB, cell P9-M; strategy_ic_mom_volman.hpp): the scale on a sleeve
// whose variance steps up (sigma, the running-mean target and the scale per block in closed form),
// the theme_schedule block the fitter writes (the price_momentum mass per block, the other themes'
// kept; the same schedule theme-tsmom-v1 uses), the shared parse that still reads theme-tsmom-v1
// blocks unchanged, and the refusals.
//
// Suite: MomVolman

#include <gtest/gtest.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "../src/strategy_ic_mom_volman.hpp"
#include "../src/strategy_ic_rules.hpp"
#include "../src/strategy_ic_runner.hpp"

namespace {
using namespace atx;
using Json = nlohmann::json;
namespace st = atx::impl::strategy;
namespace icd = atx::impl::strategy::ic_detail;

u64 bits(f64 x) { return std::bit_cast<u64>(x); }

// .5 (-1)^d for d < 200, then 2 (-1)^d: the realised volatility steps from .5 to 2.
std::vector<f64> step_sleeve(usize n) {
  std::vector<f64> out(n);
  for (usize d = 0; d < n; ++d) out[d] = (d < 200 ? .5 : 2.0) * (d % 2U == 0U ? 1.0 : -1.0);
  return out;
}

TEST(MomVolman, RegisteredConstants) {
  EXPECT_EQ(st::mom_volman_rule, "mom-volman-v1");
  EXPECT_EQ(st::mom_volman_theme, "price_momentum");
  EXPECT_EQ(st::mom_volman_lookback, 126U);
  EXPECT_EQ(st::mom_volman_lag, 2U);
  EXPECT_EQ(st::mom_volman_step, 21U);
  const auto* row = icd::find_rule("mom-volman-v1");
  ASSERT_TRUE(row != nullptr);
  EXPECT_EQ(row->block_key, "theme_schedule");
  EXPECT_EQ(icd::stage_name(row->stage), "schedule");
  EXPECT_EQ(row->recipe_key, "composition_schedule");
  EXPECT_TRUE(row->parse == icd::find_rule("theme-tsmom-v1")->parse); // one block key, one parse
}

// Blocks start at decision 127 + 21 b. The block at j reads decisions j - 127 .. j - 2, of which m
// = clamp(j - 201, 0, 126) lie after the step: sigma_b = sqrt(((126 - m) .25 + 4 m) / 126) (every
// square and sum exact), target_b the running mean of sigma_0..sigma_b, scale_b = target_b /
// sigma_b. Before the step the scale is 1; once the window holds only the high regime, sigma is 2
// and the scale target / 2 < 1: the momentum mass falls when its volatility rises.
TEST(MomVolman, StepVarianceClosedForm) {
  const auto sleeve = step_sleeve(400);
  const auto blocks = st::mom_volman_blocks(sleeve);
  ASSERT_TRUE(blocks) << blocks.error().to_string();
  ASSERT_EQ(blocks->size(), 13U); // 127, 148, ..., 379
  f64 sum = 0.0;
  for (usize b = 0; b < blocks->size(); ++b) {
    SCOPED_TRACE(b);
    const usize j = 127U + 21U * b;
    const usize m = j < 201U ? 0U : std::min<usize>(j - 201U, 126U);
    const f64 low = static_cast<f64>(126U - m), high = static_cast<f64>(m);
    const f64 sigma = std::sqrt((low * .25 + high * 4.0) / 126.0);
    sum += sigma;
    const f64 target = sum / static_cast<f64>(b + 1U);
    const auto& got = (*blocks)[b];
    EXPECT_EQ(got.decision, j);
    EXPECT_EQ(bits(got.sigma), bits(sigma));
    EXPECT_EQ(bits(got.target), bits(target));
    EXPECT_EQ(bits(got.scale), bits(target / sigma));
    if (j < 201U) {
      EXPECT_EQ(got.sigma, .5);
      EXPECT_EQ(got.scale, 1.0);
    }
    if (j >= 327U) {
      EXPECT_EQ(got.sigma, 2.0);
      EXPECT_LT(got.scale, 1.0);
    }
  }
  // The block at 211 holds 10 high-regime decisions: sigma^2 = (116 x .25 + 10 x 4) / 126 = 69/126.
  EXPECT_EQ(bits((*blocks)[4].sigma), bits(std::sqrt(69.0 / 126.0)));
  // Refusals: too short, too long, a non-finite value, a window without volatility.
  EXPECT_FALSE(st::mom_volman_blocks(step_sleeve(127)));
  EXPECT_TRUE(st::mom_volman_blocks(step_sleeve(128)));
  EXPECT_FALSE(st::mom_volman_blocks(std::vector<f64>(st::mom_volman_max_decisions + 1U, 1.0)));
  auto with_nan = step_sleeve(300);
  with_nan[250] = std::numeric_limits<f64>::quiet_NaN();
  EXPECT_FALSE(st::mom_volman_blocks(with_nan));
  auto flat = step_sleeve(300);
  std::fill(flat.begin(), flat.begin() + 140, 0.0);
  const auto zero = st::mom_volman_blocks(flat);
  ASSERT_FALSE(zero);
  EXPECT_NE(zero.error().message().find("mom-volman-v1"), std::string::npos);
}

// The library of the parse fixtures: a1, a2 (value), b1, b2 (price_momentum).
struct Weights {
  icd::Library lib;
  std::vector<f64> values{.25, .25, .2, .3};
  Weights() {
    for (const char* id : {"a1", "a2", "b1", "b2"})
      lib.candidates.push_back(icd::Candidate{id, "fam", std::string("sha-") + id, {}, {}});
  }
  // A weights document with the given theme_schedule block (none: null), round-tripped as text.
  static Json document(const Json& schedule, bool momentum = true) {
    Json doc{{"schema", "atx.dsl-composition-weights/v2"},
             {"provenance", {{"rule", "ew-theme-std-v1"}}},
             {"theme_standardise",
              {{"rule", "ew-theme-std-v1"},
               {"rerank", true},
               {"themes",
                {{"a1", "value"}, {"a2", "value"},
                 {"b1", momentum ? "price_momentum" : "value"},
                 {"b2", momentum ? "price_momentum" : "value"}}}}}};
    if (!schedule.is_null()) doc["theme_schedule"] = schedule;
    return Json::parse(doc.dump());
  }
  static Json volman(usize n) {
    Json sessions = Json::array(), sleeve = Json::array();
    const auto values = step_sleeve(n);
    for (usize d = 0; d < n; ++d) {
      sessions.push_back(static_cast<i64>(1000 + d));
      sleeve.push_back(values[d]);
    }
    return Json{{"rule", "mom-volman-v1"}, {"theme", "price_momentum"}, {"lookback", 126},
                {"lag", 2}, {"step", 21}, {"sessions", sessions}, {"sleeve", sleeve}};
  }
  core::Result<icd::PinnedWeights> parse(const Json& doc) const {
    const auto themes = icd::builtin_theme_table();
    icd::PinnedWeights pinned;
    pinned.values = values;
    ATX_TRY_VOID(icd::parse_composition_rules(doc, icd::RuleInputs{lib, themes}, pinned));
    return core::Ok(std::move(pinned));
  }
};

// The block's schedule: from the session of each block's decision, value keeps W = .25 + .25 and
// price_momentum gets W = .2 + .3 times the block's scale; the rows, stage list and recipe name the
// rule; composition_stages places each block at its session's index.
TEST(MomVolman, ScheduleFromTheWeightsFile) {
  const Weights w;
  const auto pinned = w.parse(Weights::document(Weights::volman(400)));
  ASSERT_TRUE(pinned) << pinned.error().to_string();
  ASSERT_EQ(pinned->rules.size(), 2U);
  EXPECT_EQ(pinned->rules.back()->id, "mom-volman-v1");
  EXPECT_EQ(pinned->schedule, "mom-volman-v1");
  EXPECT_EQ(pinned->schedule_off, 0U);
  const auto blocks = st::mom_volman_blocks(step_sleeve(400));
  ASSERT_TRUE(blocks);
  ASSERT_EQ(pinned->schedule_from.size(), blocks->size());
  ASSERT_EQ(pinned->schedule_mass.size(), blocks->size());
  // W_theme as IcComposition::create sums them.
  const f64 w_value = 0.0 + .25 + .25, w_momentum = 0.0 + .2 + .3;
  for (usize b = 0; b < blocks->size(); ++b) {
    SCOPED_TRACE(b);
    EXPECT_EQ(pinned->schedule_from[b], static_cast<i64>(1000 + (*blocks)[b].decision));
    ASSERT_EQ(pinned->schedule_mass[b].size(), 2U); // composition index: value 0, price_momentum 1
    EXPECT_EQ(bits(pinned->schedule_mass[b][0]), bits(w_value));
    EXPECT_EQ(bits(pinned->schedule_mass[b][1]), bits(w_momentum * (*blocks)[b].scale));
  }
  using K = st::IcStageKind;
  EXPECT_EQ(icd::running_stage_kinds(*pinned), (std::vector<K>{K::standardise, K::schedule}));
  std::vector<i64> keys(400);
  for (usize d = 0; d < keys.size(); ++d) keys[d] = static_cast<i64>(1000 + d);
  const auto stages = icd::composition_stages(*pinned, keys);
  ASSERT_TRUE(stages);
  ASSERT_EQ(stages->stages.size(), 2U);
  ASSERT_EQ(stages->stages[1].blocks.size(), blocks->size());
  for (usize b = 0; b < blocks->size(); ++b)
    EXPECT_EQ(stages->stages[1].blocks[b].begin, (*blocks)[b].decision);
  st::IcRunnerConfig cfg;
  cfg.library_sha256 = std::string(64, 'a');
  cfg.composition_weights_sha256 = std::string(64, 'c');
  EXPECT_EQ(icd::method_recipe(cfg, true, true, &*pinned).at("composition_schedule"),
            "mom-volman-v1");
}

// The shared parse reads a theme-tsmom-v1 block as before and refuses an unknown rule with the v8
// message; a mom-volman-v1 block off its registration, on an unweighted theme, too short, with
// sessions that do not increase, with a non-number or a still window, or beside theme_residualise
// is refused.
TEST(MomVolman, SharedParseAndRefusals) {
  const Weights w;
  const auto tsmom = w.parse(Weights::document(Json::parse(
      R"({"rule":"theme-tsmom-v1","lookback":252,"lag":3,"step":21,)"
      R"("themes":["price_momentum","value"],)"
      R"("blocks":[{"from_session":1200,"trailing":[1.0,-1.0]}]})")));
  ASSERT_TRUE(tsmom) << tsmom.error().to_string();
  EXPECT_EQ(tsmom->schedule, "theme-tsmom-v1");
  EXPECT_EQ(tsmom->rules.back()->id, "theme-tsmom-v1");
  const auto unknown = w.parse(Weights::document(Json{{"rule", "mom-volman-v2"}}));
  ASSERT_FALSE(unknown);
  const std::string v8_shape = "theme_schedule must be {rule: theme-tsmom-v1";
  EXPECT_NE(unknown.error().message().find(v8_shape), std::string::npos)
      << unknown.error().message();
  const auto refused = [&w](const Json& doc, const std::string& part) {
    const auto r = w.parse(doc);
    ASSERT_FALSE(r) << doc.dump().substr(0, 200);
    EXPECT_NE(r.error().message().find(part), std::string::npos) << r.error().message();
  };
  const std::string shape = "theme_schedule must be {rule: mom-volman-v1";
  for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{
           {"lookback", 252}, {"lag", 3}, {"step", 5}, {"theme", "value"}, {"extra", 1}}) {
    auto block = Weights::volman(300);
    block[key] = value;
    refused(Weights::document(block), shape);
  }
  refused(Weights::document(Weights::volman(127)), shape);
  auto ragged = Weights::volman(300);
  ragged["sleeve"].erase(0);
  refused(Weights::document(ragged), shape);
  refused(Weights::document(Weights::volman(300), false), "price_momentum is not a weighted theme");
  auto unordered = Weights::volman(300);
  unordered["sessions"][10] = 1000;
  refused(Weights::document(unordered), "sessions must be integers that increase strictly");
  auto text = Weights::volman(300);
  text["sleeve"][150] = "x";
  refused(Weights::document(text), "the sleeve's values must be finite");
  auto still = Weights::volman(300);
  for (usize d = 0; d < 140; ++d) still["sleeve"][d] = 0.0;
  refused(Weights::document(still), "realised volatility");
  auto resid = Weights::document(Weights::volman(300));
  resid["theme_residualise"] = {{"rule", "theme-resid-v1"},
                                {"order", Json::array({"value", "price_momentum"})}};
  refused(resid, "no theme_residualise");
}
} // namespace
