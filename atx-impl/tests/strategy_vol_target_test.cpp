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
// Suites: VolTarget; NavBookRule (P9 C1, DEC-10: the leverage rule as a per-book member of the
// NAV config -- construction grids and books on a pool under it, the v7 seam's injection, the
// `nav --list-rules` contract).

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
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
#include "../src/strategy_nav_replay_detail.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_risk_target.hpp"
#include "../src/strategy_spo.hpp"
#include "../src/strategy_target_replay.hpp"
#include "../src/strategy_target_replay_detail.hpp"
#include "../src/strategy_vol_target.hpp"
#include "strategy_spo_cli_fixture.hpp"
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

// ---- P9 C1 (DEC-10): the leverage rule as a per-book member of the NAV config ----------------
// Every daily row's arithmetic and every leverage record of two books, bit for bit.
void expect_same_books(const st::NavReplayResult& a, const st::NavReplayResult& b) {
  ASSERT_EQ(a.days.size(), b.days.size());
  for (usize t = 0; t < a.days.size(); ++t) {
    const auto& x = a.days[t];
    const auto& y = b.days[t];
    EXPECT_EQ(bit_pattern(x.net_return), bit_pattern(y.net_return)) << t;
    EXPECT_EQ(bit_pattern(x.posttrade_nav), bit_pattern(y.posttrade_nav)) << t;
    EXPECT_EQ(bit_pattern(x.planned_gross), bit_pattern(y.planned_gross)) << t;
    EXPECT_EQ(bit_pattern(x.traded_dollars), bit_pattern(y.traded_dollars)) << t;
    EXPECT_EQ(bit_pattern(x.trade_cost_dollars), bit_pattern(y.trade_cost_dollars)) << t;
    EXPECT_EQ(x.fills, y.fills) << t;
  }
  ASSERT_EQ(a.leverage.size(), b.leverage.size());
  for (usize k = 0; k < a.leverage.size(); ++k) {
    const auto& x = a.leverage[k];
    const auto& y = b.leverage[k];
    EXPECT_EQ(x.session, y.session) << k;
    EXPECT_EQ(x.book, y.book) << k;
    EXPECT_EQ(x.rebalance, y.rebalance) << k;
    EXPECT_EQ(x.updated, y.updated) << k;
    EXPECT_EQ(bit_pattern(x.base), bit_pattern(y.base)) << k;
    EXPECT_EQ(bit_pattern(x.gross), bit_pattern(y.gross)) << k;
    EXPECT_EQ(bit_pattern(x.sigma_hat), bit_pattern(y.sigma_hat)) << k;
    EXPECT_EQ(bit_pattern(x.sigma_ref), bit_pattern(y.sigma_ref)) << k;
    EXPECT_EQ(bit_pattern(x.raw), bit_pattern(y.raw)) << k;
    EXPECT_EQ(bit_pattern(x.leverage), bit_pattern(y.leverage)) << k;
    EXPECT_EQ(x.clip, y.clip) << k;
  }
}
// risk-target-v1 far below the fixture's forecast (sigma* 1e-4): every estimate clips at .8 L,
// so L_t leaves L at the first estimate of every book.
rt::Options clipped_risk_target() {
  rt::Options o;
  o.on = true;
  o.params.sigma_star = 1e-4;
  return o;
}

// A construction grid at --aim-leverage 1.0, 1.5 and 2.0 under a leverage rule (risk-target-v1,
// so L_t moves), in one lockstep pass, is the three single runs book for book: every daily row
// and every leverage record bit for bit, on one thread and on a pool of 4 book workers. Before
// P9 C1 the rule lived in the thread-local v7 hook, so grids and pools refused it. Variants that
// differ in the leverage rule are refused (one shared construction, one rule).
TEST(NavBookRule, GridOfLeveragesInOnePassEqualsThreeSingleRuns) {
  const Directory dir;
  const Role role(60, 12, 53);
  const auto risk = clean_store(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  auto base = nav_config();
  base.leverage = rt::leverage_rule(clipped_risk_target(), risk);
  ASSERT_EQ(base.leverage.law, st::NavLeverageLaw::RiskTargetV1);
  const auto scenarios = st::fixed_nav_scenarios();
  std::vector<st::NavReplayConfig> variants;
  for (const f64 leverage : {1.0, 1.5, 2.0}) {
    auto v = base;
    v.target.aim_leverage = leverage;
    variants.push_back(v);
  }
  std::vector<std::vector<st::NavReplayResult>> singles;
  for (const auto& v : variants) {
    auto single = st::replay_nav_scenarios(role.nav(), v, scenarios);
    ASSERT_TRUE(single) << single.error().to_string();
    singles.push_back(std::move(*single));
  }
  for (const usize workers : {usize{1}, usize{4}}) {
    auto run = variants;
    for (auto& v : run) v.book_workers = workers;
    const auto grid = st::replay_nav_grid(role.nav(), run, scenarios);
    ASSERT_TRUE(grid) << workers << ": " << grid.error().to_string();
    ASSERT_EQ(grid->size(), variants.size());
    for (usize v = 0; v < variants.size(); ++v) {
      ASSERT_EQ((*grid)[v].size(), scenarios.size());
      for (usize k = 0; k < scenarios.size(); ++k) {
        SCOPED_TRACE("workers " + std::to_string(workers) + " variant " + std::to_string(v) +
                     " book " + scenarios[k].id);
        expect_same_books((*grid)[v][k], singles[v][k]);
      }
    }
  }
  // The rule moved every variant's L (L_t = .8 L once estimated), and each book recorded its
  // scored decisions under its own label.
  for (usize v = 0; v < variants.size(); ++v) {
    const f64 cap = variants[v].target.aim_leverage;
    usize moved = 0;
    for (const auto& r : singles[v][st::nav_primary_scenario_index].leverage) {
      EXPECT_EQ(r.book, scenarios[st::nav_primary_scenario_index].id + "+" +
                            scenarios[st::nav_primary_scenario_index].financing.id);
      EXPECT_EQ(r.base, cap);
      moved += r.leverage != cap ? 1U : 0U;
    }
    EXPECT_GT(moved, 40U) << v;
  }
  auto mixed = variants;
  mixed[1].leverage = st::NavLeverageRule{};
  EXPECT_FALSE(st::replay_nav_grid(role.nav(), mixed, scenarios));
}

// vol-target-v1 on a pool of 4 book workers is the serial replay bit for bit, and the v7 seam's
// --vol-target is exactly the rule given in the config (v7::configure): before P9 C1 a pool was
// refused while the extension was installed. The seam's records are the books' merged in
// session then book order. An spo rule still refuses the pool (its engines hold every book).
TEST(NavBookRule, VolTargetOnBookWorkersEqualsSerialAndTheSeamInjectsTheRule) {
  const Directory dir;
  const Role role(60, 12, 53);
  const auto risk = clean_store(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const auto scenarios = st::fixed_nav_scenarios();
  auto direct = nav_config();
  direct.leverage = rt::leverage_rule(vol_options(), risk);
  ASSERT_EQ(direct.leverage.law, st::NavLeverageLaw::VolTargetV1);
  const auto serial = st::replay_nav_scenarios(role.nav(), direct, scenarios);
  ASSERT_TRUE(serial) << serial.error().to_string();
  auto pooled_cfg = direct;
  pooled_cfg.book_workers = 4;
  const auto pooled = st::replay_nav_scenarios(role.nav(), pooled_cfg, scenarios);
  ASSERT_TRUE(pooled) << pooled.error().to_string();
  ASSERT_EQ(pooled->size(), scenarios.size());
  for (usize k = 0; k < scenarios.size(); ++k) {
    SCOPED_TRACE(scenarios[k].id);
    EXPECT_GT((*serial)[k].leverage.size(), 50U);
    expect_same_books((*pooled)[k], (*serial)[k]);
  }
  {
    v7::NavV7Options o;
    o.risk_target = vol_options();
    o.spo_risk = risk;
    const v7::ScopedNavExtension extension(o);
    auto cfg = nav_config(); // no rule of its own: the seam injects --vol-target
    cfg.book_workers = 4;
    const auto hooked = st::replay_nav_scenarios(role.nav(), cfg, scenarios);
    ASSERT_TRUE(hooked) << hooked.error().to_string();
    usize total = 0;
    for (usize k = 0; k < scenarios.size(); ++k) {
      SCOPED_TRACE(scenarios[k].id);
      expect_same_books((*hooked)[k], (*serial)[k]);
      total += (*hooked)[k].leverage.size();
    }
    const auto records = extension.risk_target_scaler()->records();
    ASSERT_EQ(records.size(), total);
    for (usize k = 0; k < records.size(); ++k) {
      const usize decision = k / scenarios.size(), book = k % scenarios.size();
      const auto& expected = (*hooked)[book].leverage[decision];
      EXPECT_EQ(records[k].session, expected.session) << k;
      EXPECT_EQ(records[k].book, expected.book) << k;
      EXPECT_EQ(bit_pattern(records[k].leverage), bit_pattern(expected.leverage)) << k;
    }
  }
  {
    v7::NavV7Options o;
    o.spo_v1 = true;
    o.spo_risk = risk;
    const v7::ScopedNavExtension extension(o);
    EXPECT_TRUE(v7::shared_plan_state());
    auto cfg = nav_config();
    cfg.book_workers = 2;
    const auto refused = st::replay_nav_scenarios(role.nav(), cfg, scenarios);
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().message().find("spo rule"), std::string::npos)
        << refused.error().to_string();
  }
  EXPECT_FALSE(v7::shared_plan_state());
}

// A leverage rule is aim-partial-v5's (its aim is aim_leverage x desired) and replay state the
// decide path's positions do not carry: refused elsewhere, before any plan.
TEST(NavBookRule, RefusedOutsideAimPartialV5AndOnTheDecidePath) {
  const Directory dir;
  const Role role(30, 12, 53);
  const auto risk = clean_store(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  auto cfg = nav_config();
  cfg.leverage = rt::leverage_rule(vol_options(), risk);
  auto baseline = cfg;
  baseline.target.rule = st::TargetReplayRule::BaselineTargetV1;
  baseline.target.dust_multiple = 0;
  baseline.target.aim_leverage = 1;
  baseline.target.exit_rate = 1;
  const auto refused = st::replay_nav(role.nav(), baseline);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().message().find("leverage rule"), std::string::npos);
  baseline.leverage = st::NavLeverageRule{};
  EXPECT_TRUE(st::replay_nav(role.nav(), baseline)); // the control: the same book, fixed L
  const auto decided = st::detail::nav_decide(role.nav(), cfg, 10,
                                              std::vector<f64>(role.n, 0.0), 1e8);
  ASSERT_FALSE(decided);
  EXPECT_NE(decided.error().message().find("leverage rule"), std::string::npos);
}

// K-P9-7 (Ruling P7): `nav --list-rules --json` prints the envelope {schema "atx.nav-rules/v1",
// capabilities, rules} and exits 0 before any other flag is read; the leverage rows fixed-v1,
// vol-target-v1 and risk-target-v1 each carry kind "leverage", a JSON Schema object of their
// parameters (each property naming its nav flag) and the rules they exclude.
TEST(NavBookRule, ListRulesJsonCarriesTheLeverageRows) {
  const auto run = [](std::vector<std::string> args, std::ostream& out, std::ostream& err) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    return st::dispatch_nav_replay(static_cast<int>(argv.size()), argv.data(), out, err);
  };
  std::ostringstream out, err;
  ASSERT_EQ(run({"nav", "--list-rules", "--json", "--no-such-flag"}, out, err), 0) << err.str();
  const auto contract = Json::parse(out.str());
  EXPECT_EQ(contract.at("schema"), "atx.nav-rules/v1");
  EXPECT_EQ(contract.at("schema"), st::nav_rules_schema);
  ASSERT_TRUE(contract.at("capabilities").is_array());
  const auto& caps = contract.at("capabilities");
  EXPECT_NE(std::find(caps.begin(), caps.end(), Json("leverage-rule-per-book")), caps.end());
  std::vector<std::string> ids;
  for (const auto& row : contract.at("rules")) {
    ids.push_back(row.at("id").get<std::string>());
    EXPECT_EQ(row.at("kind"), "leverage") << ids.back();
    const auto& schema = row.at("params_schema");
    EXPECT_EQ(schema.at("type"), "object") << ids.back();
    EXPECT_FALSE(schema.at("additionalProperties").get<bool>()) << ids.back();
    for (const auto& name : schema.at("required"))
      EXPECT_TRUE(schema.at("properties").contains(name.get<std::string>())) << ids.back();
    for (const auto& property : schema.at("properties"))
      EXPECT_EQ(property.at("x-flag").get<std::string>().rfind("--", 0), 0U) << ids.back();
    EXPECT_TRUE(row.at("incompatible").is_array()) << ids.back();
  }
  EXPECT_EQ(ids, (std::vector<std::string>{"fixed-v1", "vol-target-v1", "risk-target-v1"}));
  const auto& vol = contract.at("rules").at(1);
  EXPECT_NE(std::find(vol.at("incompatible").begin(), vol.at("incompatible").end(),
                      Json("risk-target-v1")),
            vol.at("incompatible").end());
  const auto& risk = contract.at("rules").at(2).at("params_schema").at("properties");
  EXPECT_EQ(risk.at("sigma_star").at("x-flag"), "--risk-target");
  EXPECT_EQ(risk.at("bias").at("default"), eb::risk_target_default_bias);
  EXPECT_EQ(risk.at("cadence").at("default"), eb::risk_target_default_cadence);
  std::ostringstream text, quiet;
  ASSERT_EQ(run({"nav", "--list-rules"}, text, quiet), 0);
  EXPECT_EQ(text.str(), "leverage fixed-v1\nleverage vol-target-v1\nleverage risk-target-v1\n");
  std::ostringstream help;
  ASSERT_EQ(run({"nav", "--help"}, help, quiet), 0);
  EXPECT_NE(help.str().find("--list-rules"), std::string::npos);
}

// The command line (dispatch_nav_replay -> dispatch_nav_v7 -> the replay): a construction grid
// at --aim-leverage 1.0, 1.5 and 2.0 under --vol-target publishes <output>/<id>/ byte for byte
// the directory of each standalone run (recipe, daily, events, the v7 files, vol_target.csv,
// v7_extras.json and summary.json), and --book-workers 4 publishes the serial run's bytes.
TEST(NavBookRule, CliGridAndBookWorkersEqualTheStandaloneRunsByteForByte) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto inputs = write_run_inputs(dir.path, role);
  const std::vector<u8> forecast(role.d, u8{1});
  ASSERT_TRUE(std::filesystem::create_directory(dir.path / "risk"));
  const auto sha = write_risk_model(dir.path / "risk", role.sessions, role.n, forecast,
                                    inputs.role_sha256, 3);
  const auto nav = [&](const std::filesystem::path& output, std::vector<std::string> extra,
                       std::ostream& out, std::ostream& err) {
    std::vector<std::string> args{
        "nav", "--combined", inputs.combined, "--combined-sha256", inputs.combined_sha256,
        "--role", inputs.role, "--role-sha256", inputs.role_sha256,
        "--output", output.string(), "--rule", "aim-partial-v5", "--cadence", "1",
        "--trade-fraction", ".25", "--dust-multiple", ".1", "--exit-rate", ".05",
        "--vol-target", "vol-target-v1", "--risk-model", (dir.path / "risk").string(),
        "--risk-model-sha256", sha};
    args.insert(args.end(), extra.begin(), extra.end());
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    return st::dispatch_nav_replay(static_cast<int>(argv.size()), argv.data(), out, err);
  };
  const auto bytes_of = [](const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  };
  const auto tree = [](const std::filesystem::path& root) {
    std::vector<std::string> names;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root))
      if (e.is_regular_file())
        names.push_back(std::filesystem::relative(e.path(), root).generic_string());
    std::sort(names.begin(), names.end());
    return names;
  };
  const auto expect_same_tree = [&](const std::filesystem::path& a,
                                    const std::filesystem::path& b) {
    const auto names = tree(a);
    EXPECT_EQ(names, tree(b)) << a.string();
    for (const auto& name : names)
      EXPECT_TRUE(bytes_of(a / name) == bytes_of(b / name)) << a.string() << ' ' << name;
    return names.size();
  };
  const Json grid{{"schema", "atx.nav-construction-grid/v1"},
                  {"variants", Json::array({{{"id", "l10"}, {"flags", {{"--aim-leverage", "1.0"}}}},
                                            {{"id", "l15"}, {"flags", {{"--aim-leverage", "1.5"}}}},
                                            {{"id", "l20"},
                                             {"flags", {{"--aim-leverage", "2.0"}}}}})}};
  write_json_file(dir.path / "grid.json", grid);
  std::ostringstream out, err;
  ASSERT_EQ(nav(dir.path / "grid", {"--construction-grid", (dir.path / "grid.json").string()},
                out, err),
            0)
      << err.str();
  const std::array<std::pair<const char*, const char*>, 3> runs{
      {{"l10", "1.0"}, {"l15", "1.5"}, {"l20", "2.0"}}};
  for (const auto& [id, leverage] : runs) {
    const auto alone = dir.path / (std::string("alone-") + id);
    ASSERT_EQ(nav(alone, {"--aim-leverage", leverage}, out, err), 0) << id << ' ' << err.str();
    EXPECT_GE(expect_same_tree(dir.path / "grid" / id, alone), 9U) << id;
    EXPECT_TRUE(std::filesystem::exists(alone / "vol_target.csv")) << id;
  }
  const auto pooled = dir.path / "pooled";
  ASSERT_EQ(nav(pooled, {"--aim-leverage", "1.5", "--book-workers", "4"}, out, err), 0)
      << err.str();
  EXPECT_GE(expect_same_tree(pooled, dir.path / "alone-l15"), 9U);
  const auto summary = read_json_file(pooled / "summary.json");
  EXPECT_EQ(summary.at("rule"), "aim-partial-v5+vol-target-v1");
  EXPECT_TRUE(summary.at("v7").at("files").contains("v7_extras.json"));
}

// Look-ahead (P9 C1, deliverable 6): the scaler reads the store's row d and the book's own weights
// at its DECIDE, never a later row. A replay whose future -- prices, volume and signals after row
// T and the store's specific variances after row T -- is replaced reproduces every leverage
// record and every daily row up to row T bit for bit, under vol-target-v1 and risk-target-v1;
// the first estimate after T moves (the scaler sees the replaced future there, not before).
TEST(VolTarget, TruncationInvariant) {
  constexpr usize horizon_row = 40;
  const Directory dir;
  const Role role(70, 12, 53);
  const std::vector<u8> forecast(role.d, u8{1});
  ASSERT_TRUE(std::filesystem::create_directory(dir.path / "now"));
  ASSERT_TRUE(std::filesystem::create_directory(dir.path / "later"));
  const auto now_sha =
      write_risk_model(dir.path / "now", role.sessions, role.n, forecast, "role-sha", 3);
  std::vector<std::pair<usize, f64>> future;
  for (usize d = horizon_row + 1; d < role.d; ++d)
    for (usize i = 0; i < role.n; ++i)
      future.emplace_back(d * role.n + i, 4e-4 + 1e-5 * static_cast<f64>(i));
  const auto later_sha = write_risk_model(dir.path / "later", role.sessions, role.n, forecast,
                                          "role-sha", 3, future);
  auto now_store = sp::RiskStore::open((dir.path / "now").string(), now_sha, "role-sha");
  auto later_store = sp::RiskStore::open((dir.path / "later").string(), later_sha, "role-sha");
  ASSERT_TRUE(now_store) << now_store.error().to_string();
  ASSERT_TRUE(later_store) << later_store.error().to_string();
  const auto now_risk = std::make_shared<const sp::RiskStore>(std::move(*now_store));
  const auto later_risk = std::make_shared<const sp::RiskStore>(std::move(*later_store));
  Role changed = role;
  for (usize t = horizon_row + 1; t < role.d; ++t)
    for (usize i = 0; i < role.n; ++i) {
      const usize k = t * role.n + i;
      changed.close[k] *= 1.0 + 0.03 * static_cast<f64>(i % 3);
      changed.raw[k] = changed.close[k];
      changed.volume[k] *= 2.0;
      if (role.member[k]) changed.signal[k] = 1.0 - role.signal[k];
    }
  for (const bool vol : {true, false}) {
    SCOPED_TRACE(vol ? "vol-target-v1" : "risk-target-v1");
    rt::Options options = vol_options();
    if (!vol) {
      options = rt::Options{};
      options.on = true;
      options.params.sigma_star = 0.05;
      options.params.cadence = 10;
    }
    auto cfg = nav_config();
    cfg.leverage = rt::leverage_rule(options, now_risk);
    const auto full = st::replay_nav(role.nav(), cfg);
    ASSERT_TRUE(full) << full.error().to_string();
    cfg.leverage = rt::leverage_rule(options, later_risk);
    const auto cut = st::replay_nav(changed.nav(), cfg);
    ASSERT_TRUE(cut) << cut.error().to_string();
    const i64 horizon = role.sessions[horizon_row];
    ASSERT_EQ(full->leverage.size(), cut->leverage.size());
    usize checked = 0;
    bool moved = false;
    for (usize k = 0; k < full->leverage.size(); ++k) {
      const auto& a = full->leverage[k];
      const auto& b = cut->leverage[k];
      ASSERT_EQ(a.session, b.session) << k;
      if (a.session <= horizon) {
        EXPECT_EQ(a.updated, b.updated) << k;
        EXPECT_EQ(bit_pattern(a.gross), bit_pattern(b.gross)) << k;
        EXPECT_EQ(bit_pattern(a.sigma_hat), bit_pattern(b.sigma_hat)) << k;
        EXPECT_EQ(bit_pattern(a.sigma_ref), bit_pattern(b.sigma_ref)) << k;
        EXPECT_EQ(bit_pattern(a.raw), bit_pattern(b.raw)) << k;
        EXPECT_EQ(bit_pattern(a.leverage), bit_pattern(b.leverage)) << k;
        ++checked;
      } else if (a.updated || b.updated) {
        moved = moved || bit_pattern(a.sigma_hat) != bit_pattern(b.sigma_hat);
      }
    }
    EXPECT_EQ(checked, horizon_row + 1); // one record per decision 0..T
    EXPECT_TRUE(moved);
    usize rows = 0;
    for (usize t = 0; t < full->days.size() && t < cut->days.size(); ++t) {
      const auto& a = full->days[t];
      if (a.session_index > horizon_row) break;
      const auto& b = cut->days[t];
      EXPECT_EQ(bit_pattern(a.net_return), bit_pattern(b.net_return)) << t;
      EXPECT_EQ(bit_pattern(a.posttrade_nav), bit_pattern(b.posttrade_nav)) << t;
      EXPECT_EQ(bit_pattern(a.planned_gross), bit_pattern(b.planned_gross)) << t;
      ++rows;
    }
    EXPECT_EQ(rows, horizon_row + 1);
  }
}
} // namespace
