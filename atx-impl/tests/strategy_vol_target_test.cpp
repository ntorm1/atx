// vol-target-v1 (platform v8 Y, lane YCOMB) through the NAV hook: the risk target's scaler under
// Law::vol_target_v1 (strategy_risk_target.hpp, strategy_vol_target.hpp, engine
// book/vol_target.hpp). A replay of the fixture role on its atx-risk-v1 store: no estimate on the
// flat first decision, the first estimate on the first decision with a book gives L bit for bit,
// then every 21 sessions; at every estimate sigma_ref is the running mean of the recorded
// sigma_hat (sum / count in order) and L_t = clip(L x sigma_ref / sigma_hat, 1, L), held between
// estimates. aim-partial-v5 plans toward L_t x desired bit for bit; the capacity x1 book is the
// main book bit for bit; the published blocks are keyed "vol_target" (series vol_target.csv, rule
// id +vol-target-v1) and risk-target-v1's spellings are unchanged; the CLI routes the flag and
// refuses the rest.
//
// Suite: VolTarget

#include <bit>
#include <cmath>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/engine/book/risk_target.hpp"
#include "atx/engine/book/vol_target.hpp"
#include "../src/strategy_cost_v2.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_risk_target.hpp"
#include "../src/strategy_spo.hpp"
#include "../src/strategy_target_replay.hpp"
#include "../src/strategy_target_replay_detail.hpp"
#include "../src/strategy_vol_target.hpp"
#include "strategy_spo_fixture.hpp"

namespace {
using namespace atx;
using namespace atx::impl::strategy::spo::fixture;
namespace st = atx::impl::strategy;
namespace sp = atx::impl::strategy::spo;
namespace rt = atx::impl::strategy::risk_target;
namespace v7 = atx::impl::strategy::v7;
namespace eb = atx::engine::book;
using Json = nlohmann::json;

u64 bit_pattern(f64 x) { return std::bit_cast<u64>(x); }

std::shared_ptr<const sp::RiskStore> clean_store(const Directory& dir, const Role& role,
                                                 u64 seed) {
  const std::vector<u8> forecast(role.d, u8{1});
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", seed);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  EXPECT_TRUE(store) << store.error().to_string();
  return store ? std::make_shared<const sp::RiskStore>(std::move(*store)) : nullptr;
}
rt::Options vol_options() {
  rt::Options o;
  o.on = true;
  o.law = rt::Law::vol_target_v1;
  return o;
}
// The members' signal at d, demeaned over the members and scaled to gross 1 (nonmembers 0).
std::vector<f64> desired_at(const Role& role, usize d) {
  const usize n = role.n;
  std::vector<f64> desired(n, 0.0);
  f64 sum = 0;
  usize members = 0;
  for (usize i = 0; i < n; ++i) {
    if (!role.member[d * n + i]) continue;
    desired[i] = role.signal[d * n + i];
    sum += desired[i];
    ++members;
  }
  f64 gross = 0;
  for (usize i = 0; i < n; ++i) {
    if (!role.member[d * n + i]) continue;
    desired[i] -= sum / static_cast<f64>(members);
    gross += std::abs(desired[i]);
  }
  for (f64& v : desired) v /= gross;
  return desired;
}
atx::core::Result<v7::NavV7Command> parse(std::vector<std::string> args) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  return v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data());
}
bool claims(std::vector<std::string> args) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  return v7::claims_nav_args(static_cast<int>(argv.size()), argv.data());
}

// A replay of the fixture under the hook (aim-partial-v5, cadence 1, L 1.2, S2): the law, the
// running mean recomputed from the records, the cadence, the clip [1, L] and the blocks.
TEST(VolTarget, ReplayManagesTheCapWithTheRunningMeanEvery21Sessions) {
  const Directory dir;
  const Role role(70, 12, 53);
  const auto risk = clean_store(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const auto cfg = nav_config();
  const f64 cap = cfg.target.aim_leverage;
  v7::NavV7Options o;
  o.risk_target = vol_options();
  o.spo_risk = risk;
  const v7::ScopedNavExtension extension(o);
  const auto result = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(result) << result.error().to_string();
  const auto* scaler = extension.risk_target_scaler();
  ASSERT_NE(scaler, nullptr);
  const auto records = scaler->records();
  ASSERT_GT(records.size(), 60U);
  EXPECT_FALSE(records.front().updated); // the flat first decision: L in force
  EXPECT_EQ(records.front().leverage, cap);
  EXPECT_TRUE(std::isnan(records.front().sigma_ref));
  usize first = records.size(), estimates = 0;
  f64 sum = 0, in_force = cap;
  for (usize k = 0; k < records.size(); ++k) {
    const auto& r = records[k];
    EXPECT_EQ(r.session, role.sessions[k]) << k;
    if (r.updated) {
      if (first == records.size()) first = k;
      EXPECT_EQ((k - first) % eb::vol_target_cadence, 0U) << k;
      ++estimates;
      sum += r.sigma_hat;
      const f64 ref = sum / static_cast<f64>(estimates);
      EXPECT_EQ(bit_pattern(r.sigma_ref), bit_pattern(ref)) << k;
      EXPECT_EQ(bit_pattern(r.raw), bit_pattern(cap * (ref / r.sigma_hat))) << k;
      const f64 expect = r.raw < 1.0 ? 1.0 : r.raw > cap ? cap : r.raw;
      EXPECT_EQ(bit_pattern(r.leverage), bit_pattern(expect)) << k;
      if (estimates == 1) {
        EXPECT_EQ(r.leverage, cap) << k; // ratio 1: the cap bit for bit
      }
      in_force = r.leverage;
    } else if (first < records.size()) {
      EXPECT_NE((k - first) % eb::vol_target_cadence, 0U) << k;
    }
    EXPECT_EQ(bit_pattern(r.leverage), bit_pattern(in_force)) << k;
    EXPECT_GE(r.leverage, eb::vol_target_floor) << k;
    EXPECT_LE(r.leverage, cap) << k;
  }
  ASSERT_GE(first, 1U);
  ASSERT_LE(first, 3U);
  EXPECT_EQ(estimates, 1 + (records.size() - 1 - first) / eb::vol_target_cadence);
  // The blocks: keyed vol_target, the rule id, the series with sigma_ref.
  Json recipe{{"rule", "aim-partial-v5"}}, summary{{"rule", "aim-partial-v5"}};
  v7::extend_recipe(recipe);
  v7::extend_summary(summary);
  EXPECT_EQ(recipe.at("rule"), "aim-partial-v5+vol-target-v1");
  EXPECT_EQ(summary.at("rule"), "aim-partial-v5+vol-target-v1");
  EXPECT_FALSE(recipe.contains("risk_target"));
  ASSERT_TRUE(recipe.contains("vol_target"));
  EXPECT_EQ(recipe.at("vol_target").at("rule"), "vol-target-v1");
  EXPECT_EQ(recipe.at("vol_target").at("floor"), 1.0);
  EXPECT_EQ(recipe.at("vol_target").at("cadence_sessions"), 21U);
  EXPECT_FALSE(recipe.at("vol_target").contains("books"));
  const auto& books = summary.at("vol_target").at("books");
  ASSERT_EQ(books.size(), 1U);
  const auto& book = books.begin().value();
  EXPECT_EQ(book.at("decisions"), records.size());
  EXPECT_EQ(book.at("estimates"), estimates);
  EXPECT_EQ(book.at("sigma_ref").at("n"), estimates);
  const std::string csv = rt::records_csv(records, rt::Law::vol_target_v1);
  EXPECT_EQ(csv.rfind("session,book,rebalance,updated,gross,priced_share,sigma_hat,sigma_ref,raw,"
                      "L_t,multiplier,clip\n", 0), 0U);
  usize lines = 0;
  for (const char c : csv) lines += c == '\n' ? 1U : 0U;
  EXPECT_EQ(lines, records.size() + 1);
  EXPECT_STREQ(rt::block_key(o.risk_target), "vol_target");
  EXPECT_STREQ(rt::series_file(o.risk_target), "vol_target.csv");
  EXPECT_EQ(rt::rule_suffix(o.risk_target), "+vol-target-v1");
}

// aim-partial-v5 under the hook moves toward L_t x desired: every plan equals update_weights
// with --aim-leverage replaced by the L_t the scaler recorded for that decision, bit for bit.
TEST(VolTarget, AimPartialV5PlansTowardTheManagedAimBitForBit) {
  const Directory dir;
  const Role role(50, 12, 41);
  const auto risk = clean_store(dir, role, 5);
  ASSERT_NE(risk, nullptr);
  const auto cfg = nav_config();
  v7::NavV7Options o;
  o.risk_target = vol_options();
  o.spo_risk = risk;
  const v7::ScopedNavExtension extension(o);
  const auto x = role.target();
  std::vector<f64> current(role.n, 0.0);
  usize estimates = 0;
  for (usize d = 0; d < 45; ++d) {
    const auto desired = desired_at(role, d);
    std::vector<f64> hooked = current, plain = current;
    st::TargetReplayDay a, b;
    ASSERT_TRUE(v7::plan(x, cfg, d, true, 0.0, 1e8, desired, hooked, a, {}));
    const auto& record = extension.risk_target_scaler()->records().back();
    ASSERT_EQ(record.session, role.sessions[d]);
    EXPECT_GE(record.leverage, 1.0);
    EXPECT_LE(record.leverage, cfg.target.aim_leverage);
    auto t = cfg.target;
    t.aim_leverage = record.leverage;
    ASSERT_TRUE(st::detail::update_weights(x, t, d, true, 0.0, desired, plain, b));
    for (usize i = 0; i < role.n; ++i)
      EXPECT_EQ(bit_pattern(hooked[i]), bit_pattern(plain[i])) << d << ' ' << i;
    EXPECT_EQ(bit_pattern(a.gross), bit_pattern(b.gross)) << d;
    estimates += record.updated ? 1U : 0U;
    current = hooked;
  }
  EXPECT_EQ(estimates, 3U); // d = 1 (the first book), 22, 43
}

// Each capacity book runs its own state from a clean one: the x1 book is the main S2 book bit for
// bit, and the capacity pass records nothing.
TEST(VolTarget, CapacityX1IsTheMainBookBitForBit) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_store(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const auto base = nav_config();
  const auto books = st::cost_v2::capacity_scenarios(base.scenario);
  ASSERT_EQ(books[1].id, "capacity-x1-v1");
  v7::NavV7Options o;
  o.capacity = true;
  o.risk_target = vol_options();
  o.spo_risk = risk;
  v7::ScopedNavExtension extension(o);
  extension.begin_run(v7::NavV7Pass::Main);
  const auto primary = st::replay_nav(role.nav(), base);
  ASSERT_TRUE(primary) << primary.error().to_string();
  const usize main_records = extension.risk_target_scaler()->records().size();
  extension.begin_run(v7::NavV7Pass::Capacity);
  auto unit_cfg = base;
  unit_cfg.scenario = books[1];
  const auto unit = st::replay_nav(role.nav(), unit_cfg);
  ASSERT_TRUE(unit) << unit.error().to_string();
  ASSERT_EQ(unit->days.size(), primary->days.size());
  for (usize t = 0; t < primary->days.size(); ++t) {
    EXPECT_EQ(bit_pattern(unit->days[t].net_return), bit_pattern(primary->days[t].net_return))
        << t;
    EXPECT_EQ(bit_pattern(unit->days[t].planned_gross),
              bit_pattern(primary->days[t].planned_gross))
        << t;
  }
  EXPECT_EQ(extension.risk_target_scaler()->records().size(), main_records);
}

// Flag absent (and --risk-target): risk-target-v1's law and spellings are unchanged.
TEST(VolTarget, RiskTargetSpellingsAreUnchangedWithoutTheFlag) {
  rt::Options r8;
  r8.on = true;
  r8.params.sigma_star = 0.05;
  EXPECT_EQ(r8.law, rt::Law::risk_target_v1);
  EXPECT_STREQ(rt::block_key(r8), "risk_target");
  EXPECT_STREQ(rt::series_file(r8), "risk_target.csv");
  EXPECT_EQ(rt::rule_suffix(r8), "+risk-target-0.05");
  EXPECT_EQ(rt::parameters_json(r8).at("rule"), "risk-target-v1");
  const std::vector<rt::Record> none;
  EXPECT_EQ(rt::records_csv(none),
            "session,book,rebalance,updated,gross,priced_share,sigma_hat,raw,L_t,multiplier,"
            "clip\n");
  const std::vector<std::string> store{"--risk-model", "risk", "--risk-model-sha256", "abc"};
  std::vector<std::string> args{"nav", "--rule", "aim-partial-v5", "--output", "x",
                                "--risk-target", ".05"};
  args.insert(args.end(), store.begin(), store.end());
  const auto parsed = parse(args);
  ASSERT_TRUE(parsed) << parsed.error().to_string();
  EXPECT_EQ(parsed->options.risk_target.law, rt::Law::risk_target_v1);
  const auto bare = parse({"nav", "--rule", "aim-partial-v5", "--output", "x"});
  ASSERT_TRUE(bare);
  EXPECT_FALSE(bare->options.risk_target.on);
  EXPECT_FALSE(claims({"nav", "--rule", "aim-partial-v5", "--output", "x"}));
}

TEST(VolTarget, ParseRoutesTheFlagAndRefusesTheRest) {
  const std::vector<std::string> store{"--risk-model", "risk", "--risk-model-sha256", "abc"};
  const auto with = [](std::vector<std::string> head, const std::vector<std::string>& tail) {
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
  };
  const std::vector<std::string> v5{"nav", "--rule", "aim-partial-v5", "--aim-leverage", "2",
                                    "--output", "x"};
  const auto ok = parse(with(with(v5, {"--vol-target", "vol-target-v1"}), store));
  ASSERT_TRUE(ok) << ok.error().to_string();
  EXPECT_TRUE(ok->options.risk_target.on);
  EXPECT_EQ(ok->options.risk_target.law, rt::Law::vol_target_v1);
  EXPECT_EQ(ok->risk_model, "risk");
  for (const auto& token : ok->args) {
    EXPECT_NE(token, "--vol-target") << token; // consumed, not replayed
    EXPECT_EQ(token.rfind("--risk", 0), std::string::npos) << token;
  }
  EXPECT_TRUE(claims(with(v5, {"--vol-target", "vol-target-v1"})));
  const std::vector<std::vector<std::string>> refused{
      with(v5, {"--vol-target", "vol-target-v1"}),                               // no store
      with(with(v5, {"--vol-target", "vol-target-v2"}), store),                  // unknown rule
      with(with(v5, {"--vol-target", "vol-target-v1", "--vol-target", "vol-target-v1"}), store),
      with(with(v5, {"--vol-target", "vol-target-v1", "--risk-target", ".05"}), store),
      with(with(v5, {"--vol-target", "vol-target-v1", "--risk-target-bias", "1.2"}), store),
      with(with({"nav", "--rule", "spo-v3", "--spo-alpha", "implied-aim", "--output", "x",
                 "--vol-target", "vol-target-v1"}, store), {}),
      with(with({"nav", "--rule", "aim-partial-v6", "--output", "x", "--vol-target",
                 "vol-target-v1"}, store), {}),
      with(with({"nav", "--output", "x", "--vol-target", "vol-target-v1"}, store), {}),
      with({"nav", "--rule", "aim-partial-v5", "--output", "x", "--vol-target"}, {}),
  };
  for (const auto& args : refused) {
    const auto r = parse(args);
    std::string line;
    for (const auto& a : args) line += a + ' ';
    EXPECT_FALSE(r) << line;
  }
  std::ostringstream help;
  v7::append_help(help);
  EXPECT_NE(help.str().find("--vol-target vol-target-v1"), std::string::npos);
}
} // namespace
