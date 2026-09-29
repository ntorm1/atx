// spo-v1 bit-for-bit guard (platform v7 W1b; R2 review M-4): two FNV-1a digests of spo-v1 as
// pre-registered (SpoParams{}: alpha horizon 1, no specific ceiling, gamma = max(gamma_vol,
// gamma_bind), gross budget = --aim-leverage) on a fixed synthetic role and risk model:
//   * weights: every planned weight (bit pattern) of two books (S1, S2) planned in lockstep
//     through the v7 seam (v7::plan: rebalance and hold decisions, carried without drift),
//     then the decision fields of each plan;
//   * replay: the NAV replay of the S2 book (planned gross/net/turnover, net return, traded
//     and cost dollars, long/short dollars, post-trade NAV per day) and columns 1-38 of
//     spo_diagnostics.csv (the columns the real-data identity run (b) compares).
// A change to spo-v2 or to the flag machinery must leave both digests unchanged.
//
// CAPTURE PROTOCOL (no binary may run in the implementing lane): this file and
// strategy_spo_fixture.hpp use ONLY the API of the pre-W1b commit c8503bb3, so the commit that
// introduces them compiles on that base unchanged. Root builds atx-impl-strategy-target-tests
// at that commit, runs --gtest_filter=SpoPin.*, reads the printed `[spo-pin]` digests and pins
// them below; the W1b head must print the SAME two values. Pinned (see PIN below); zero
// constants would mean unpinned (the test then fails and prints the digests).
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <gtest/gtest.h>
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_spo.hpp"
#include "../src/strategy_target_replay.hpp"
#include "strategy_spo_fixture.hpp"

namespace {
using namespace atx;
using namespace atx::impl::strategy::spo::fixture;
namespace st = atx::impl::strategy;
namespace sp = atx::impl::strategy::spo;
namespace v7 = atx::impl::strategy::v7;

// PIN: captured by root on the pre-W1b base (pool-2 decdf947 = the c8503bb3 line + cd01f74e
// only, build v7-4: weights over 54 diagnostics rows, replay over 40 days) and reproduced
// identically on the W1b head (pool-2 66e0774d = 4c4ce75f + F3, build v7-5): spo-v1 is bit
// for bit. Any change here means a spo-v1 planned weight moved.
constexpr u64 pinned_weights = 0xda6b6871e7e267c5ULL;
constexpr u64 pinned_replay = 0xaabdbb72f99a6e13ULL;

constexpr u64 fnv_basis = 0xcbf29ce484222325ULL;
constexpr u64 fnv_prime = 0x100000001b3ULL;
u64 fnv_byte(u64 h, u64 byte) { return (h ^ (byte & 0xffU)) * fnv_prime; }
u64 fnv_word(u64 h, u64 word) {
  for (u32 b = 0; b < 8; ++b) h = fnv_byte(h, word >> (8U * b));
  return h;
}
// NaN canonicalized (its payload is not part of the rule).
u64 fold(u64 h, f64 x) { return fnv_word(h, std::isnan(x) ? 0x7ff8000000000000ULL : bits(x)); }
u64 fold_text(u64 h, std::string_view text) {
  for (const char c : text) h = fnv_byte(h, static_cast<u64>(static_cast<unsigned char>(c)));
  return h;
}
// The first `columns` comma-separated fields of every line of a CSV.
std::string leading_columns(const std::string& csv, usize columns) {
  std::string out;
  usize field = 0;
  bool keep = true;
  for (const char c : csv) {
    if (c == '\n') { out += '\n'; field = 0; keep = true; continue; }
    if (c == ',' && ++field >= columns) keep = false;
    if (keep) out += c;
  }
  return out;
}
struct Model {
  Directory dir;
  std::shared_ptr<const sp::RiskStore> risk;
};
// Role(40, 12, 53) with a clean risk model (seed 3): the ReplayPlansNeutral... fixture.
std::unique_ptr<Model> model_of(const Role& role) {
  auto m = std::make_unique<Model>();
  const std::vector<u8> forecast(role.d, u8{1});
  const auto sha = write_risk_model(m->dir.path, role.sessions, role.n, forecast, "role-sha", 3);
  auto store = sp::RiskStore::open(m->dir.path.string(), sha, "role-sha");
  if (store) m->risk = std::make_shared<const sp::RiskStore>(std::move(*store));
  return m;
}
v7::NavV7Options spo_v1(std::shared_ptr<const sp::RiskStore> risk) {
  v7::NavV7Options o;
  o.spo_v1 = true; // SpoParams{}: spo-v1 as pre-registered
  o.spo_risk = std::move(risk);
  return o;
}

TEST(SpoPin, SpoV1PlannedWeightsOfTwoLockstepBooks_MatchThePinnedDigest) {
  const Role role(40, 12, 53);
  const auto m = model_of(role);
  ASSERT_NE(m->risk, nullptr);
  const v7::ScopedNavExtension extension(spo_v1(m->risk));
  const auto s2 = nav_config();
  auto s1 = s2;
  s1.scenario = st::fixed_nav_scenarios()[0];
  const auto x = role.target();
  const usize n = role.n;
  std::vector<f64> w1(n, 0.0), w2(n, 0.0), desired(n);
  u64 h = fnv_basis;
  for (usize d = 0; d < role.d; ++d) {
    // Desired: the members' signal, demeaned over the members (nonmembers 0).
    f64 sum = 0;
    usize members = 0;
    for (usize i = 0; i < n; ++i) {
      desired[i] = role.member[d * n + i] ? role.signal[d * n + i] - 0.5 : 0.0;
      if (role.member[d * n + i]) { sum += desired[i]; ++members; }
    }
    for (usize i = 0; i < n; ++i)
      if (role.member[d * n + i]) desired[i] -= sum / static_cast<f64>(members);
    const bool rebalance = d % 3 != 2; // every third decision holds (exits only)
    for (auto* book : {&w1, &w2}) {
      const auto& cfg = book == &w1 ? s1 : s2;
      st::TargetReplayDay day;
      const auto status = v7::plan(x, cfg, d, rebalance, 0.0, 1e8, desired, *book, day, {});
      ASSERT_TRUE(status) << d << ": " << status.error().to_string();
      for (const f64 w : *book) h = fold(h, w);
      for (const f64 v : {day.turnover, day.gross, day.net, day.long_weight, day.short_weight})
        h = fold(h, v);
      h = fnv_word(h, day.held_names);
    }
  }
  const auto* engine = extension.spo_engine();
  ASSERT_NE(engine, nullptr);
  ASSERT_FALSE(engine->rows().empty());
  h = fold(h, engine->calibration().gamma);
  std::printf("[spo-pin] weights=0x%016llx (%zu diagnostics rows)\n",
              static_cast<unsigned long long>(h), engine->rows().size());
  if (pinned_weights == 0 && pinned_replay == 0) {
    ADD_FAILURE() << "spo-v1 digests unpinned: capture on c8503bb3 + this file, then pin";
    return;
  }
  EXPECT_EQ(h, pinned_weights);
}

TEST(SpoPin, SpoV1ReplayAndDiagnosticsColumns1To38_MatchThePinnedDigest) {
  const Role role(40, 12, 53);
  const auto m = model_of(role);
  ASSERT_NE(m->risk, nullptr);
  const v7::ScopedNavExtension extension(spo_v1(m->risk));
  const auto result = st::replay_nav(role.nav(), nav_config());
  ASSERT_TRUE(result) << result.error().to_string();
  u64 h = fnv_basis;
  for (const auto& day : result->days) {
    h = fnv_word(h, static_cast<u64>(day.session));
    for (const f64 v : {day.planned_gross, day.planned_net, day.planned_turnover, day.net_return,
                        day.traded_dollars, day.trade_cost_dollars, day.long_dollars,
                        day.short_dollars, day.posttrade_nav})
      h = fold(h, v);
  }
  const auto* engine = extension.spo_engine();
  ASSERT_NE(engine, nullptr);
  const auto csv = sp::diagnostics_csv(engine->rows());
  h = fold_text(h, leading_columns(csv, 38));
  std::printf("[spo-pin] replay=0x%016llx (%zu days)\n", static_cast<unsigned long long>(h),
              result->days.size());
  if (pinned_weights == 0 && pinned_replay == 0) {
    ADD_FAILURE() << "spo-v1 digests unpinned: capture on c8503bb3 + this file, then pin";
    return;
  }
  EXPECT_EQ(h, pinned_replay);
}
} // namespace
