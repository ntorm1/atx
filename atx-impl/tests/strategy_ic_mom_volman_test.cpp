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
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <string>
#include <utility>
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

// `first` for d < 126, then `rest`: the block at 253 (and later) reads only `rest`.
std::vector<f64> regime_sleeve(usize n, f64 first, f64 rest) {
  std::vector<f64> out(n);
  for (usize d = 0; d < n; ++d) out[d] = (d < 126U ? first : rest) * (d % 2U == 0U ? 1.0 : -1.0);
  return out;
}

// Review S-1 / ruling ALCOMB-4 (no cap): a tiny but positive realised volatility (1e-120 after a
// 1e-2 regime) gives a large scale, finite and accepted, and the parse schedules it; a scale that
// overflows (1e153 then 3e-162: a positive sigma near 3e-162 under a target near 1e152) is refused
// by mom_volman_blocks, naming the rule and the block, not later by the composition's mass check.
TEST(MomVolman, TinyVolatilityGivesAFiniteScaleAndOverflowIsRefused) {
  const auto tiny = st::mom_volman_blocks(regime_sleeve(280, 1e-2, 1e-120));
  ASSERT_TRUE(tiny) << tiny.error().to_string();
  ASSERT_EQ(tiny->size(), 8U); // 127, 148, ..., 274
  for (const auto& b : *tiny) {
    SCOPED_TRACE(b.decision);
    EXPECT_TRUE(std::isfinite(b.scale));
    EXPECT_EQ(bits(b.scale), bits(b.target / b.sigma));
  }
  const auto& quiet = (*tiny)[6]; // j = 253: the window 126 .. 251 holds only 1e-120
  EXPECT_EQ(quiet.decision, 253U);
  EXPECT_NEAR(quiet.sigma / 1e-120, 1.0, 1e-12);
  EXPECT_GT(quiet.scale, 1e117);
  EXPECT_LT(quiet.scale, 1e119);
  // The parse takes it: price_momentum's mass in that block is W x the scale, finite.
  Json block = Weights::volman(280);
  const auto values = regime_sleeve(280, 1e-2, 1e-120);
  for (usize d = 0; d < values.size(); ++d) block["sleeve"][d] = values[d];
  const Weights w;
  const auto pinned = w.parse(Weights::document(block));
  ASSERT_TRUE(pinned) << pinned.error().to_string();
  ASSERT_EQ(pinned->schedule_mass.size(), 8U);
  EXPECT_EQ(bits(pinned->schedule_mass[6][1]), bits((0.0 + .2 + .3) * quiet.scale));
  EXPECT_TRUE(std::isfinite(pinned->schedule_mass[6][1]));
  // Overflow: refused here, with the rule's name and the block's decision.
  const auto overflow = st::mom_volman_blocks(regime_sleeve(260, 1e153, 3e-162));
  ASSERT_FALSE(overflow);
  EXPECT_NE(overflow.error().message().find("mom-volman-v1: the scale target / sigma before "
                                            "decision 253 must be finite"),
            std::string::npos)
      << overflow.error().message();
  // Its blocks before 253 are finite: the same sleeve cut at 253 decisions is accepted.
  EXPECT_TRUE(st::mom_volman_blocks(regime_sleeve(253, 1e153, 3e-162)));
}

// Ruling ALCOMB-5 S-3: the role summary prints, per block, its first session, sigma, target and
// scale (the values the schedule applied, bit for bit); nothing without a mom-volman-v1 block (a
// theme-tsmom-v1 schedule included).
TEST(MomVolman, SummaryTracePrintsTheScalePerBlock) {
  const Weights w;
  const auto pinned = w.parse(Weights::document(Weights::volman(400)));
  ASSERT_TRUE(pinned) << pinned.error().to_string();
  const auto blocks = st::mom_volman_blocks(step_sleeve(400));
  ASSERT_TRUE(blocks);
  Json result{{"role", "train"}};
  ASSERT_TRUE(icd::add_mom_volman_trace(*pinned, result));
  ASSERT_TRUE(result.contains("composition_schedule_trace"));
  const auto& trace = result.at("composition_schedule_trace");
  EXPECT_EQ(trace.at("rule"), "mom-volman-v1");
  EXPECT_EQ(trace.at("theme"), "price_momentum");
  ASSERT_EQ(trace.at("blocks").size(), blocks->size());
  for (usize b = 0; b < blocks->size(); ++b) {
    SCOPED_TRACE(b);
    const auto& row = trace.at("blocks").at(b);
    EXPECT_EQ(row.at("from_session").get<i64>(), static_cast<i64>(1000 + (*blocks)[b].decision));
    EXPECT_EQ(bits(row.at("sigma").get<f64>()), bits((*blocks)[b].sigma));
    EXPECT_EQ(bits(row.at("target").get<f64>()), bits((*blocks)[b].target));
    EXPECT_EQ(bits(row.at("scale").get<f64>()), bits((*blocks)[b].scale));
  }
  // Printed as JSON text and read back, every value keeps its bits.
  const auto reread = Json::parse(result.dump());
  EXPECT_EQ(bits(reread.at("composition_schedule_trace").at("blocks").at(5).at("scale").get<f64>()),
            bits((*blocks)[5].scale));
  // No block, or theme-tsmom-v1's: nothing is added.
  for (const auto& doc : {Weights::document(nullptr),
                          Weights::document(Json::parse(
                              R"({"rule":"theme-tsmom-v1","lookback":252,"lag":3,"step":21,)"
                              R"("themes":["price_momentum","value"],)"
                              R"("blocks":[{"from_session":1200,"trailing":[1.0,-1.0]}]})"))}) {
    const auto other = w.parse(doc);
    ASSERT_TRUE(other) << other.error().to_string();
    Json untouched{{"role", "train"}};
    ASSERT_TRUE(icd::add_mom_volman_trace(*other, untouched));
    EXPECT_EQ(untouched, (Json{{"role", "train"}}));
  }
}

// P9 lane D3W (ruling ALCOMB-6): the sleeve before and after scaling as two series files
// (session_ns,net_return): from the first block's session on, within the role's sessions, raw
// r(d) and scale_b r(d) with b the block in force, every value's bits kept; nothing for a role
// outside the sleeve's sessions or without the rule.
namespace {
std::vector<std::pair<i64, f64>> read_series(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::string line;
  std::getline(file, line);
  EXPECT_EQ(line, "session_ns,net_return");
  std::vector<std::pair<i64, f64>> out;
  while (std::getline(file, line)) {
    const auto comma = line.find(',');
    i64 session{};
    f64 value{};
    std::from_chars(line.data(), line.data() + comma, session);
    std::from_chars(line.data() + comma + 1, line.data() + line.size(), value);
    out.emplace_back(session, value);
  }
  return out;
}
} // namespace

TEST(MomVolman, SleeveFilesBeforeAndAfterScaling) {
  const Weights w;
  const auto pinned = w.parse(Weights::document(Weights::volman(400)));
  ASSERT_TRUE(pinned) << pinned.error().to_string();
  const auto blocks = st::mom_volman_blocks(step_sleeve(400));
  ASSERT_TRUE(blocks);
  const auto sleeve = step_sleeve(400);
  const auto dir = std::filesystem::temp_directory_path() / "atx_mom_volman_sleeve_files";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto keys = [](i64 first, i64 last) {
    std::vector<i64> out;
    for (i64 s = first; s <= last; ++s) out.push_back(s);
    return out;
  };
  ASSERT_TRUE(icd::write_mom_volman_sleeves(*pinned, keys(1000, 1399), dir, "train"));
  const auto raw = read_series(dir / "train_mom_volman_sleeve_raw.csv");
  const auto scaled = read_series(dir / "train_mom_volman_sleeve_scaled.csv");
  const usize first = (*blocks)[0].decision; // 127
  ASSERT_EQ(raw.size(), 400U - first);
  ASSERT_EQ(scaled.size(), raw.size());
  usize b = 0;
  for (usize k = 0; k < raw.size(); ++k) {
    const usize d = first + k;
    while (b + 1U < blocks->size() && (*blocks)[b + 1U].decision <= d) ++b;
    SCOPED_TRACE(d);
    EXPECT_EQ(raw[k].first, static_cast<i64>(1000 + d));
    EXPECT_EQ(scaled[k].first, static_cast<i64>(1000 + d));
    EXPECT_EQ(bits(raw[k].second), bits(sleeve[d]));
    EXPECT_EQ(bits(scaled[k].second), bits(sleeve[d] * (*blocks)[b].scale));
  }
  EXPECT_GT(b, 5U); // several blocks were in force
  // A role over part of the sleeve: only its sessions.
  ASSERT_TRUE(icd::write_mom_volman_sleeves(*pinned, keys(1200, 1250), dir, "part"));
  const auto part = read_series(dir / "part_mom_volman_sleeve_scaled.csv");
  ASSERT_EQ(part.size(), 51U);
  EXPECT_EQ(part.front().first, 1200);
  EXPECT_EQ(bits(part.front().second), bits(scaled[200 - first].second));
  // No overlap, a role before the first block, or no mom-volman-v1 rule: no file.
  ASSERT_TRUE(icd::write_mom_volman_sleeves(*pinned, keys(5000, 5010), dir, "later"));
  ASSERT_TRUE(icd::write_mom_volman_sleeves(*pinned, keys(1000, 1100), dir, "early"));
  const auto plain = w.parse(Weights::document(nullptr));
  ASSERT_TRUE(plain) << plain.error().to_string();
  ASSERT_TRUE(icd::write_mom_volman_sleeves(*plain, keys(1000, 1399), dir, "plain"));
  for (const char* role : {"later", "early", "plain"}) {
    EXPECT_FALSE(std::filesystem::exists(dir / (std::string(role) + "_mom_volman_sleeve_raw.csv")))
        << role;
    EXPECT_FALSE(
        std::filesystem::exists(dir / (std::string(role) + "_mom_volman_sleeve_scaled.csv")))
        << role;
  }
  // A sleeve whose shape does not match the blocks is an internal error.
  auto broken = *pinned;
  broken.volman_sleeve.pop_back();
  EXPECT_FALSE(icd::write_mom_volman_sleeves(broken, keys(1000, 1399), dir, "broken"));
  std::filesystem::remove_all(dir);
}
} // namespace
