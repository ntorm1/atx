// risk-target-v1 (platform v8 R-8) through the NAV hook (strategy_risk_target.hpp): the
// flag-absent identity on the pinned bench (SpoPin's spo-v1 digests; spo-v3's engine given
// base_leverage = L plans as without it), the scaler's sigma_hat and L_t on the fixture's
// atx-risk-v1 store against a dense recomputation (gross 1 over the whole book, the names with a
// risk row, no estimate without a forecast), the cadence in a replay (the first estimate on the
// first decision with a book, then every 21 sessions, L_t held between and inside [.8 L,
// 1.25 L]), aim-partial-v5 planning toward L_t x desired bit for bit, spo-v3 tracking L_t x
// desired with the parent's gamma and gross bound, the capacity x1 book equal to the main book,
// the published blocks and the CLI.
//
// Suite: RiskTarget

#include <bit>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/engine/book/risk_target.hpp"
#include "../src/strategy_cost_v2.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_risk_target.hpp"
#include "../src/strategy_spo.hpp"
#include "../src/strategy_spo_v3.hpp"
#include "../src/strategy_target_replay.hpp"
#include "../src/strategy_target_replay_detail.hpp"
#include "strategy_spo_digest.hpp"
#include "strategy_spo_fixture.hpp"

namespace {
using namespace atx;
using namespace atx::impl::strategy::spo::fixture;
namespace st = atx::impl::strategy;
namespace sp = atx::impl::strategy::spo;
namespace rt = atx::impl::strategy::risk_target;
namespace v7 = atx::impl::strategy::v7;
namespace eb = atx::engine::book;
namespace dg = atx::impl::strategy::spo::digest;
using Json = nlohmann::json;

u64 bit_pattern(f64 x) { return std::bit_cast<u64>(x); }

using Overrides = std::span<const std::pair<usize, f64>>; // (date x names + i, specific)
std::shared_ptr<const sp::RiskStore> store_of(const Directory& dir, const Role& role,
                                              std::span<const u8> forecast, u64 seed,
                                              Overrides overrides = {}) {
  const auto sha =
      write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", seed, overrides);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  EXPECT_TRUE(store) << store.error().to_string();
  return store ? std::make_shared<const sp::RiskStore>(std::move(*store)) : nullptr;
}
std::shared_ptr<const sp::RiskStore> clean_store(const Directory& dir, const Role& role,
                                                 u64 seed) {
  const std::vector<u8> forecast(role.d, u8{1});
  return store_of(dir, role, forecast, seed);
}
rt::Options target(f64 sigma_star) {
  rt::Options o;
  o.on = true;
  o.params.sigma_star = sigma_star;
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
// sigma_hat recomputed densely from the store's row d: every name with a risk row (slot < 50,
// finite positive specific variance) at w_i / gross, gross over the whole book.
f64 dense_sigma_hat(const sp::RiskStore& store, usize d, std::span<const f64> w) {
  sp::RiskSlice slice;
  const auto read = store.read(d, slice);
  EXPECT_TRUE(read) << read.error().to_string();
  constexpr usize k = sp::risk_factors;
  constexpr usize style0 = 1 + sp::risk_industry_slots;
  f64 gross = 0;
  for (const f64 v : w) gross += std::abs(v);
  std::vector<f64> e(k, 0.0);
  f64 specific = 0;
  for (usize i = 0; i < w.size(); ++i) {
    const f64 s = slice.specific[i];
    if (w[i] == 0 || slice.slot[i] >= sp::risk_industry_slots || !std::isfinite(s) || !(s > 0))
      continue;
    const f64 u = w[i] / gross;
    e[0] += u;
    e[1U + static_cast<usize>(slice.slot[i])] += u;
    for (usize c = 0; c < sp::risk_styles; ++c)
      e[style0 + c] += slice.styles[i * sp::risk_styles + c] * u;
    specific += s * u * u;
  }
  f64 factor = 0;
  for (usize a = 0; a < k; ++a)
    for (usize b = 0; b < k; ++b) factor += e[a] * slice.covariance[a * k + b] * e[b];
  return std::sqrt(252.0 * (factor + specific));
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

// Flag absent: the pinned bench (SpoPin's spo-v1 digests through the v7 seam, the procedures of
// strategy_spo_digest.hpp on its role and model) is unchanged, and an installed extension without
// --risk-target writes no risk_target key and keeps the rule id.
TEST(RiskTarget, FlagAbsentKeepsThePinnedBenchDigests) {
  const Role role(40, 12, 53);
  const auto model = dg::model_of(role, 3);
  ASSERT_NE(model->risk, nullptr);
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.spo_risk = model->risk;
  ASSERT_FALSE(o.risk_target.on);
  const auto weights = dg::planned_weights(o, role);
  const auto replay = dg::replay(o, role, 38);
  ASSERT_TRUE(weights.error.empty()) << weights.error;
  ASSERT_TRUE(replay.error.empty()) << replay.error;
  EXPECT_EQ(weights.value, 0xda6b6871e7e267c5ULL); // strategy_spo_pin_test.cpp
  EXPECT_EQ(replay.value, 0xaabdbb72f99a6e13ULL);
  const v7::ScopedNavExtension extension(v7::NavV7Options{});
  EXPECT_EQ(extension.risk_target_scaler(), nullptr);
  Json recipe{{"rule", "aim-partial-v5"}}, summary{{"rule", "aim-partial-v5"}},
      holdings{{"rule", "aim-partial-v5"}};
  v7::extend_recipe(recipe);
  v7::extend_summary(summary);
  v7::extend_holdings(holdings);
  for (const Json* doc : {&recipe, &summary, &holdings}) {
    EXPECT_FALSE(doc->contains("risk_target"));
    EXPECT_EQ(doc->at("rule"), "aim-partial-v5");
  }
  const auto parsed = parse({"nav", "--rule", "aim-partial-v5", "--output", "x"});
  ASSERT_TRUE(parsed);
  EXPECT_FALSE(parsed->options.risk_target.on);
  EXPECT_FALSE(claims({"nav", "--rule", "aim-partial-v5", "--output", "x"}));
}

// The spo edit is value-preserving: an engine given base_leverage = L (what the hook passes
// under the risk target before L_t moves) plans, calibrates and reports exactly as one without
// it. Under L_t = 1.25 L it tracks L_t x desired, with the parent's gamma (calibrated on
// L x desired) and the gross bound 2 x L.
TEST(RiskTarget, SpoV3TracksTheScaledAimWithTheParentsGammaAndBound) {
  const Directory dir;
  const Role role(20, 12, 83);
  const auto risk = clean_store(dir, role, 13);
  ASSERT_NE(risk, nullptr);
  const auto params = sp::v3_params();
  const auto cfg = nav_config();
  const f64 big = 1.25 * cfg.target.aim_leverage;
  auto scaled = cfg;
  scaled.target.aim_leverage = big;
  const auto x = role.target();
  const usize n = role.n, d = 2;
  const auto desired = desired_at(role, d);
  const st::cost_v2::DecisionLiquidity liquidity{std::vector<f64>(n, 1e9),
                                                 std::vector<f64>(n, 0.02)};
  std::vector<f64> start(n);
  for (usize i = 0; i < n; ++i) start[i] = 0.5 * cfg.target.aim_leverage * desired[i];
  sp::Engine plain(params, risk), based(params, risk), moved(params, risk);
  const f64 base = cfg.target.aim_leverage;
  const sp::BookDecision in_plain{x, cfg, cfg.scenario, d, 1e8, desired, {}, {}, liquidity,
                                  "S2"};
  const sp::BookDecision in_based{x, cfg, cfg.scenario, d, 1e8, desired, {}, {}, liquidity,
                                  "S2", base};
  const sp::BookDecision in_moved{x, scaled, cfg.scenario, d, 1e8, desired, {}, {}, liquidity,
                                  "S2", base};
  std::vector<f64> w_plain = start, w_based = start, w_moved = start;
  st::TargetReplayDay day_plain, day_based, day_moved;
  ASSERT_TRUE(plain.plan(in_plain, w_plain, day_plain));
  ASSERT_TRUE(based.plan(in_based, w_based, day_based));
  ASSERT_TRUE(moved.plan(in_moved, w_moved, day_moved));
  for (usize i = 0; i < n; ++i) EXPECT_EQ(bit_pattern(w_based[i]), bit_pattern(w_plain[i])) << i;
  EXPECT_EQ(based.rows_csv(), plain.rows_csv());
  EXPECT_EQ(based.rule_calibration_json().dump(), plain.rule_calibration_json().dump());
  EXPECT_EQ(based.rule_parameters_json().dump(), plain.rule_parameters_json().dump());
  // L_t: the aim is L_t x desired; gamma and the bound are the parent's.
  const auto aim = moved.last_aim();
  ASSERT_EQ(aim.size(), n);
  for (usize i = 0; i < n; ++i) EXPECT_EQ(bit_pattern(aim[i]), bit_pattern(big * desired[i])) << i;
  EXPECT_EQ(bit_pattern(moved.calibration().gamma), bit_pattern(plain.calibration().gamma));
  EXPECT_EQ(bit_pattern(moved.calibration().aim_vol), bit_pattern(plain.calibration().aim_vol));
  EXPECT_EQ(moved.gross_budget(), sp::v3_gross_bound_multiple * cfg.target.aim_leverage);
  ASSERT_EQ(moved.tracking_rows().size(), 1U);
  ASSERT_EQ(plain.tracking_rows().size(), 1U);
  EXPECT_NEAR(moved.tracking_rows().front().aim_gross,
              1.25 * plain.tracking_rows().front().aim_gross, 1e-12);
  EXPECT_NE(bit_pattern(w_moved[0]), bit_pattern(w_plain[0])); // the larger aim reached the plan
}

// The scaler on the fixture's store: no estimate without a forecast (L in force), then sigma_hat
// of the gross-1 book over the names with a risk row (name 3's specific variance is missing at
// d: it scales the book through the gross and adds no variance) equal to a dense recomputation,
// and L_t = S / (b sigma_hat) inside the clip; held on the next decision (not due).
TEST(RiskTarget, ScalerSigmaHatMatchesADenseRecomputationOnTheStore) {
  const Role role(30, 12, 71);
  constexpr usize d = 6;
  std::vector<u8> forecast(role.d, u8{1});
  for (usize t = 0; t < 5; ++t) forecast[t] = 0;
  const std::vector<std::pair<usize, f64>> overrides{{d * role.n + 3, missing}};
  const Directory dir;
  const auto risk = store_of(dir, role, forecast, 9, overrides);
  ASSERT_NE(risk, nullptr);
  const auto x = role.target();
  std::vector<f64> w(role.n);
  Lcg rng{17};
  for (usize i = 0; i < role.n; ++i) w[i] = i == 7 ? 0.0 : 0.2 * (rng.next() - 0.5);
  const f64 sigma = dense_sigma_hat(*risk, d, w);
  ASSERT_TRUE(std::isfinite(sigma));
  ASSERT_GT(sigma, 0.0);
  const f64 base = 1.2;
  auto options = target(1.15 * sigma * base); // raw = L, inside [.8 L, 1.25 L]
  ASSERT_TRUE(eb::validate_risk_target(options.params));
  rt::Scaler scaler(options, risk);
  // d = 2: no forecast, so no estimate; L in force.
  const auto early = scaler.leverage(x, 2, true, "S2", base, w, true);
  ASSERT_TRUE(early) << early.error().to_string();
  EXPECT_EQ(*early, base);
  const auto at = scaler.leverage(x, d, true, "S2", base, w, true);
  ASSERT_TRUE(at) << at.error().to_string();
  const auto next = scaler.leverage(x, d + 1, true, "S2", base, std::vector<f64>(role.n, 0.01),
                                    true);
  ASSERT_TRUE(next);
  const auto records = scaler.records();
  ASSERT_EQ(records.size(), 3U);
  EXPECT_FALSE(records[0].updated);
  EXPECT_TRUE(std::isnan(records[0].sigma_hat));
  EXPECT_EQ(records[0].leverage, base);
  const auto& r = records[1];
  EXPECT_TRUE(r.updated);
  EXPECT_EQ(r.session, role.sessions[d]);
  EXPECT_NEAR(r.sigma_hat, sigma, 1e-12 * sigma);
  EXPECT_NEAR(r.raw, base, 1e-12);
  EXPECT_EQ(r.clip, eb::RiskTargetClip::None);
  EXPECT_EQ(*at, r.leverage);
  f64 gross = 0;
  for (const f64 v : w) gross += std::abs(v);
  EXPECT_DOUBLE_EQ(r.gross, gross);
  EXPECT_NEAR(r.priced_share, (gross - std::abs(w[3])) / gross, 1e-14);
  EXPECT_FALSE(records[2].updated); // 1 session after the estimate: held
  EXPECT_EQ(bit_pattern(*next), bit_pattern(*at));
  EXPECT_EQ(bit_pattern(records[2].sigma_hat), bit_pattern(r.sigma_hat));
  // Without a store, or with a leverage that is not positive: refused.
  rt::Scaler none(options, nullptr);
  EXPECT_FALSE(none.leverage(x, d, true, "S2", base, w, true));
  EXPECT_FALSE(scaler.leverage(x, d, true, "S2", 0.0, w, true));
}

// A replay of the fixture under the hook (aim-partial-v5, cadence 1, L 1.2, S2): no estimate on
// the first (flat) decision, the first on the first decision with a book, then every 21 sessions;
// L_t inside [.8 L, 1.25 L] and held between estimates. The published blocks carry it.
TEST(RiskTarget, ReplayEstimatesOnTheFirstBookThenEvery21Sessions) {
  const Directory dir;
  const Role role(70, 12, 53);
  const auto risk = clean_store(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const auto cfg = nav_config();
  const f64 base = cfg.target.aim_leverage;
  v7::NavV7Options o;
  o.risk_target = target(0.05);
  o.spo_risk = risk;
  const v7::ScopedNavExtension extension(o);
  const auto result = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(result) << result.error().to_string();
  const auto* scaler = extension.risk_target_scaler();
  ASSERT_NE(scaler, nullptr);
  const auto records = scaler->records();
  ASSERT_GT(records.size(), 60U);
  EXPECT_FALSE(records.front().updated); // the flat first decision
  EXPECT_EQ(records.front().leverage, base);
  usize first = records.size(), estimates = 0;
  f64 in_force = base;
  for (usize k = 0; k < records.size(); ++k) {
    const auto& r = records[k];
    EXPECT_EQ(r.session, role.sessions[k]) << k; // one record per decision, in order
    if (r.updated) {
      if (first == records.size()) first = k;
      EXPECT_EQ((k - first) % 21, 0U) << k;
      EXPECT_TRUE(std::isfinite(r.sigma_hat)) << k;
      in_force = r.leverage;
      ++estimates;
    } else if (first < records.size()) {
      EXPECT_NE((k - first) % 21, 0U) << k;
    }
    EXPECT_EQ(bit_pattern(r.leverage), bit_pattern(in_force)) << k;
    EXPECT_GE(r.leverage, eb::risk_target_clip_lo * base) << k;
    EXPECT_LE(r.leverage, eb::risk_target_clip_hi * base) << k;
  }
  ASSERT_GE(first, 1U);
  ASSERT_LE(first, 3U);
  EXPECT_EQ(estimates, 1 + (records.size() - 1 - first) / 21);
  // The blocks: recipe (parameters), summary (per book), rule id, the series.
  Json recipe{{"rule", "aim-partial-v5"}}, summary{{"rule", "aim-partial-v5"}};
  v7::extend_recipe(recipe);
  v7::extend_summary(summary);
  EXPECT_EQ(recipe.at("rule"), "aim-partial-v5+risk-target-0.05");
  EXPECT_EQ(summary.at("rule"), "aim-partial-v5+risk-target-0.05");
  ASSERT_TRUE(recipe.contains("risk_target"));
  EXPECT_FALSE(recipe.at("risk_target").contains("books"));
  EXPECT_EQ(recipe.at("risk_target").at("sigma_star"), 0.05);
  EXPECT_EQ(recipe.at("risk_target").at("bias"), eb::risk_target_default_bias);
  EXPECT_EQ(recipe.at("risk_target").at("cadence_sessions"), 21U);
  const auto& books = summary.at("risk_target").at("books");
  ASSERT_EQ(books.size(), 1U);
  const auto& book = books.begin().value();
  EXPECT_EQ(book.at("decisions"), records.size());
  EXPECT_EQ(book.at("estimates"), estimates);
  EXPECT_EQ(book.at("decisions_before_first_estimate"), first);
  const std::string csv = rt::records_csv(records);
  EXPECT_EQ(csv.rfind("session,book,rebalance,updated,gross,priced_share,sigma_hat,raw,L_t,"
                      "multiplier,clip\n", 0), 0U);
  usize lines = 0;
  for (const char c : csv) lines += c == '\n' ? 1U : 0U;
  EXPECT_EQ(lines, records.size() + 1);
  auto tuned = target(0.05);
  tuned.params.bias = 1.2;
  tuned.params.cadence = 10;
  EXPECT_EQ(rt::rule_suffix(tuned), "+risk-target-0.05-bias-1.2-cadence-10");
}

// aim-partial-v5 under the hook moves toward L_t x desired: every plan equals update_weights with
// --aim-leverage replaced by the L_t the scaler recorded for that decision, bit for bit, and
// L_t leaves L (the fixture's book is above the target vol, so the clip's lower end binds).
TEST(RiskTarget, AimPartialV5PlansTowardTheScaledAimBitForBit) {
  const Directory dir;
  const Role role(30, 12, 41);
  const auto risk = clean_store(dir, role, 5);
  ASSERT_NE(risk, nullptr);
  const auto cfg = nav_config();
  v7::NavV7Options o;
  o.risk_target = target(0.05);
  o.risk_target.params.cadence = 3;
  o.spo_risk = risk;
  const v7::ScopedNavExtension extension(o);
  const auto x = role.target();
  std::vector<f64> current(role.n, 0.0);
  usize moved = 0;
  for (usize d = 0; d < 20; ++d) {
    const auto desired = desired_at(role, d);
    std::vector<f64> hooked = current, plain = current;
    st::TargetReplayDay a, b;
    ASSERT_TRUE(v7::plan(x, cfg, d, true, 0.0, 1e8, desired, hooked, a, {}));
    const auto& record = extension.risk_target_scaler()->records().back();
    ASSERT_EQ(record.session, role.sessions[d]);
    auto t = cfg.target;
    t.aim_leverage = record.leverage;
    ASSERT_TRUE(st::detail::update_weights(x, t, d, true, 0.0, desired, plain, b));
    for (usize i = 0; i < role.n; ++i)
      EXPECT_EQ(bit_pattern(hooked[i]), bit_pattern(plain[i])) << d << ' ' << i;
    EXPECT_EQ(bit_pattern(a.gross), bit_pattern(b.gross)) << d;
    moved += record.leverage != cfg.target.aim_leverage ? 1U : 0U;
    current = hooked;
  }
  EXPECT_GT(moved, 0U);
}

// aim-partial-v5 with the capacity pass: each capacity book runs its own state from a clean one,
// so the x1 book is the main pass's S2 book bit for bit, and the capacity pass records nothing.
TEST(RiskTarget, CapacityX1IsTheMainBookBitForBit) {
  const Directory dir;
  const Role role(40, 12, 53);
  const auto risk = clean_store(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  const auto base = nav_config();
  const auto books = st::cost_v2::capacity_scenarios(base.scenario);
  ASSERT_EQ(books[1].id, "capacity-x1-v1");
  v7::NavV7Options o;
  o.capacity = true;
  o.risk_target = target(0.05);
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

TEST(RiskTarget, ParseRoutesTheFlagsAndRefusesTheRest) {
  const std::vector<std::string> store{"--risk-model", "risk", "--risk-model-sha256", "abc"};
  const auto with = [&](std::vector<std::string> head, std::vector<std::string> tail = {}) {
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
  };
  const std::vector<std::string> v5{"nav", "--rule", "aim-partial-v5", "--output", "x"};
  // aim-partial-v5: the registered defaults; the store flags serve the risk target.
  const auto plain = parse(with(with(v5, {"--risk-target", ".05"}), store));
  ASSERT_TRUE(plain) << plain.error().to_string();
  const auto& p = plain->options.risk_target;
  EXPECT_TRUE(p.on);
  EXPECT_EQ(p.params.sigma_star, 0.05);
  EXPECT_EQ(p.params.bias, 1.15);
  EXPECT_EQ(p.params.cadence, 21U);
  EXPECT_FALSE(plain->options.spo_v1);
  EXPECT_EQ(plain->risk_model, "risk");
  EXPECT_EQ(plain->risk_model_sha256, "abc");
  for (const auto& token : plain->args)
    EXPECT_EQ(token.rfind("--risk", 0), std::string::npos) << token; // consumed, not replayed
  EXPECT_TRUE(claims(with(v5, {"--risk-target", ".05"})));
  EXPECT_TRUE(claims(with(v5, {"--risk-target-cadence", "21"})));
  const auto tuned = parse(with(with(v5, {"--risk-target", ".05", "--risk-target-bias", "1.3",
                                          "--risk-target-cadence", "10"}),
                                store));
  ASSERT_TRUE(tuned) << tuned.error().to_string();
  EXPECT_EQ(tuned->options.risk_target.params.bias, 1.3);
  EXPECT_EQ(tuned->options.risk_target.params.cadence, 10U);
  // spo-v3: one store for both.
  const auto v3 = parse(with({"nav", "--rule", "spo-v3", "--spo-alpha", "implied-aim", "--output",
                              "x", "--risk-target", ".05"},
                             store));
  ASSERT_TRUE(v3) << v3.error().to_string();
  EXPECT_TRUE(v3->options.spo_v1);
  EXPECT_TRUE(v3->options.risk_target.on);
  // Refusals.
  const std::vector<std::vector<std::string>> refused{
      with(with(v5, {"--risk-target-bias", "1.15"}), store),        // tuning without the target
      with(with(v5, {"--risk-target-cadence", "21"}), store),
      with(v5, {"--risk-target", ".05"}),                           // no store
      with(v5, {"--risk-model", "risk", "--risk-model-sha256", "abc"}), // a store without a rule
      with(with({"nav", "--rule", "aim-partial-v6", "--output", "x", "--risk-target", ".05"}),
           store),
      with(with({"nav", "--rule", "spo-v1", "--output", "x", "--risk-target", ".05"}), store),
      with(with({"nav", "--rule", "spo-v2", "--output", "x", "--risk-target", ".05"}), store),
      with(with({"nav", "--output", "x", "--risk-target", ".05"}), store), // no rule: baseline
      with(with({"nav", "--rule", "baseline-v1", "--output", "x", "--risk-target", ".05"}),
           store),
      with(with(v5, {"--risk-target", "0"}), store),
      with(with(v5, {"--risk-target", "2"}), store),
      with(with(v5, {"--risk-target", ".05", "--risk-target-bias", "0"}), store),
      with(with(v5, {"--risk-target", ".05", "--risk-target-cadence", "0"}), store),
      with(with(v5, {"--risk-target", ".05", "--risk-target", ".06"}), store),
      with(with(v5, {"--risk-target", "five"}), store),
  };
  for (const auto& args : refused) {
    const auto r = parse(args);
    std::string line;
    for (const auto& a : args) line += a + ' ';
    EXPECT_FALSE(r) << line;
  }
  std::ostringstream help;
  v7::append_help(help);
  for (const char* flag : {"--risk-target", "--risk-target-bias", "--risk-target-cadence"})
    EXPECT_NE(help.str().find(flag), std::string::npos) << flag;
}
} // namespace
