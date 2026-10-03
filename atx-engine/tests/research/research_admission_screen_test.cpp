// research/admission (P9 B1): screen_v4's statistics in closed form, the declared check order, the
// (tier, roster) greedy pass, the strict rho limit, the fitter's HAC arithmetic on short series,
// input refusals and the report-only traded-horizon columns.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/engine/eval/hac.hpp"
#include "atx/engine/research/admission/screen.hpp"
#include "atx/engine/research/admission/traded_horizon.hpp"

namespace adm = atx::engine::research::admission;
namespace hac = atx::engine::eval::hac;
using atx::f64;
using atx::i32;
using atx::u64;
using atx::u8;
using atx::usize;

namespace {

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// A screen input over owned buffers: every factor NaN (flat), every decision in TRAIN, tau .05,
// tier rank 0 and prior sign 1 until a test says otherwise.
struct Table {
  usize candidates{};
  usize decisions{};
  std::vector<f64> factors;
  std::vector<f64> taus;
  std::vector<u8> mask;
  std::vector<usize> tiers;
  std::vector<i32> priors;
  Table(usize k, usize d)
      : candidates{k}, decisions{d}, factors(k * d, kNaN), taus(k, 0.05), mask(d, 1U),
        tiers(k, 0U), priors(k, 1) {}
  f64 &at(usize d, usize k) { return factors[d * candidates + k]; }
  [[nodiscard]] adm::ScreenInput input() const {
    return {candidates, decisions, factors, taus, mask, tiers, priors};
  }
};

struct Lcg {
  u64 state{};
  f64 next() { // [0, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) * 0x1.0p-53;
  }
};

std::vector<adm::ScreenRow> screen(const Table &t, const adm::ScreenRules &rules = {}) {
  auto rows = adm::screen_v4(t.input(), rules);
  EXPECT_TRUE(rows.has_value()) << (rows ? std::string{} : rows.error().message());
  return rows ? std::move(*rows) : std::vector<adm::ScreenRow>{};
}

std::vector<adm::Check> checks(std::initializer_list<adm::Check> list) { return list; }

} // namespace

// One alternating series a + b(-1)^t (powers of two: every sum is exact) over 300 TRAIN decisions,
// with 20 masked decisions holding values that would move every statistic: the mean is a, the
// sample SD b sqrt(n / (n - 1)), the HAC t a / sqrt(S / n^2) with S the Bartlett lag-5 sum of the
// exact autocovariances (n - l) b^2 (-1)^l. A constant (2^-13) has no t and no Sharpe; twice the
// first series (an exact double) has rho exactly 1 and is redundant; no prior is refused first.
TEST(ResearchAdmission, ScreenV4ClosedForm) {
  constexpr usize kTrain = 300;
  constexpr usize kMasked = 20;
  const f64 a = std::ldexp(1.0, -10);
  const f64 b = std::ldexp(1.0, -9);
  Table t(4, kTrain + kMasked);
  for (usize d = 0; d < t.decisions; ++d) {
    const bool masked = d < kMasked;
    t.mask[d] = masked ? 0U : 1U;
    const usize s = masked ? 0U : d - kMasked; // the TRAIN index
    const f64 x = masked ? 100.0 : a + ((s % 2U == 0U) ? b : -b);
    t.at(d, 0) = x;
    t.at(d, 1) = masked ? -100.0 : std::ldexp(1.0, -13);
    t.at(d, 2) = 2.0 * x;
    t.at(d, 3) = x;
  }
  t.priors[3] = 0;
  const auto rows = screen(t);
  ASSERT_EQ(rows.size(), 4U);
  const f64 n = static_cast<f64>(kTrain);
  f64 s = n * b * b;
  for (usize l = 1; l <= 5; ++l) {
    const f64 sign = (l % 2U == 0U) ? 1.0 : -1.0;
    s += 2.0 * (1.0 - static_cast<f64>(l) / 6.0) * (n - static_cast<f64>(l)) * b * b * sign;
  }
  const f64 t_closed = a / std::sqrt(s / (n * n));
  const f64 sharpe_closed = a / (b * std::sqrt(n / (n - 1.0))) * std::sqrt(252.0);
  const adm::ScreenRow &r0 = rows[0];
  EXPECT_EQ(r0.train_days, kTrain);
  ASSERT_TRUE(r0.train_mean && r0.train_sharpe && r0.hac_t);
  EXPECT_EQ(*r0.train_mean, a); // exact: every partial sum is a multiple of 2^-10
  EXPECT_NEAR(*r0.train_sharpe, sharpe_closed, 1e-12 * std::fabs(sharpe_closed));
  EXPECT_NEAR(*r0.hac_t, t_closed, 1e-12 * std::fabs(t_closed));
  EXPECT_EQ(r0.status, adm::Status::Admitted);
  EXPECT_EQ(r0.admission_rank, 1U);
  EXPECT_TRUE(r0.failed_checks.empty());
  EXPECT_EQ(r0.s_k, 1);
  EXPECT_EQ(r0.tau, 0.05);
  const adm::ScreenRow &r1 = rows[1];
  EXPECT_EQ(r1.train_mean, std::ldexp(1.0, -13));
  EXPECT_FALSE(r1.train_sharpe.has_value());
  EXPECT_FALSE(r1.hac_t.has_value());
  EXPECT_EQ(r1.status, adm::Status::Admitted);
  EXPECT_EQ(r1.admission_rank, 2U);
  EXPECT_EQ(r1.undefined_rho_with, std::vector<usize>{0U}); // zero variance: rho undefined
  const adm::ScreenRow &r2 = rows[2];
  EXPECT_EQ(r2.status, adm::Status::RejectRedundant);
  EXPECT_EQ(r2.redundant_with, 0U);
  EXPECT_EQ(r2.redundant_rho, 1.0);
  EXPECT_EQ(r2.max_abs_rho, 1.0);
  EXPECT_EQ(r2.max_abs_rho_with, 0U);
  EXPECT_EQ(r2.undefined_rho_with, std::vector<usize>{1U});
  EXPECT_EQ(r2.train_mean, 2.0 * a);
  EXPECT_NEAR(*r2.hac_t, t_closed, 1e-12 * std::fabs(t_closed)); // t is scale free
  const adm::ScreenRow &r3 = rows[3];
  EXPECT_EQ(r3.status, adm::Status::RejectNoPrior);
  EXPECT_EQ(r3.failed_checks, checks({adm::Check::NoPrior}));
  EXPECT_EQ(r3.s_k, 0);
  EXPECT_EQ(adm::admitted_order(rows), (std::vector<usize>{0U, 1U}));
}

// Every failing check is listed in the declared order and the first one names the status:
// no_prior, insufficient, turnover, turnover_cost (v4-prior-v2 only), veto. Limits are strict:
// tau .70 is not turnover, tau .08 is not turnover_cost, 250 days are sufficient.
TEST(ResearchAdmission, FirstFailureWins) {
  constexpr usize kDays = 300;
  Table t(8, kDays);
  Lcg rng{11};
  for (usize d = 0; d < kDays; ++d) {
    const f64 negative = -1e-3 + 1e-4 * (rng.next() - 0.5); // HAC t far below -2
    const f64 positive = 1e-3 + 1e-4 * (rng.next() - 0.5);
    for (usize k = 0; k < 8; ++k) t.at(d, k) = (k < 5) ? negative : positive;
    if (d >= 100) t.at(d, 0) = t.at(d, 1) = kNaN; // 100 live days
    if (d >= 250) t.at(d, 6) = kNaN;              // exactly 250 live days
    if (d >= 249) t.at(d, 7) = kNaN;              // 249
  }
  t.priors[0] = 0;
  t.taus = {0.9, 0.9, 0.9, 0.5, 0.05, 0.70, 0.08, 0.05};
  const auto v1 = screen(t);
  EXPECT_EQ(v1[0].failed_checks, checks({adm::Check::NoPrior, adm::Check::Insufficient,
                                          adm::Check::Turnover, adm::Check::Veto}));
  EXPECT_EQ(v1[0].status, adm::Status::RejectNoPrior);
  EXPECT_EQ(v1[1].failed_checks,
            checks({adm::Check::Insufficient, adm::Check::Turnover, adm::Check::Veto}));
  EXPECT_EQ(v1[1].status, adm::Status::RejectInsufficient);
  EXPECT_EQ(v1[2].failed_checks, checks({adm::Check::Turnover, adm::Check::Veto}));
  EXPECT_EQ(v1[2].status, adm::Status::RejectTurnover);
  EXPECT_EQ(v1[3].failed_checks, checks({adm::Check::Veto})); // tau .5: no cost check in v1
  EXPECT_EQ(v1[3].status, adm::Status::RejectVeto);
  EXPECT_EQ(v1[4].status, adm::Status::RejectVeto);
  EXPECT_TRUE(v1[5].failed_checks.empty()); // tau .70 is not above the limit
  EXPECT_TRUE(v1[6].failed_checks.empty()); // 250 days suffice
  EXPECT_EQ(v1[7].failed_checks, checks({adm::Check::Insufficient}));
  const auto v2 = screen(t, adm::screen_rules(adm::ScreenId::V4PriorV2));
  EXPECT_EQ(v2[2].failed_checks,
            checks({adm::Check::Turnover, adm::Check::TurnoverCost, adm::Check::Veto}));
  EXPECT_EQ(v2[3].failed_checks, checks({adm::Check::TurnoverCost, adm::Check::Veto}));
  EXPECT_EQ(v2[3].status, adm::Status::RejectTurnoverCost);
  EXPECT_EQ(v2[5].status, adm::Status::RejectTurnoverCost); // .70 > .08
  EXPECT_TRUE(v2[6].failed_checks.empty());                 // .08 is not above .08
  EXPECT_FALSE(adm::screen_rules(adm::ScreenId::V4PriorV1).cost_tau_limit.has_value());
  EXPECT_EQ(adm::screen_rules(adm::ScreenId::V4PriorV2).cost_tau_limit, 0.08);
}

// The greedy pass visits survivors by (tier rank, roster index), never by a statistic: the two
// A+ strings late in the roster go first, so the earlier B string correlated with them is the
// redundant one; between two admitted strings above the limit the larger |rho| names the
// redundancy; max |rho| covers every row against the admitted set.
TEST(ResearchAdmission, GreedyOrderTierThenRoster) {
  constexpr usize kDays = 400;
  Table t(5, kDays);
  Lcg rng{2026};
  for (usize d = 0; d < kDays; ++d) {
    const f64 z1 = rng.next() - 0.5;
    const f64 z2 = rng.next() - 0.5;
    const f64 z3 = rng.next() - 0.5;
    t.at(d, 0) = z1 + 0.05 * z3;      // B: a copy of z1
    t.at(d, 1) = z1;                  // A+
    t.at(d, 2) = z1 + 0.1 * z2;       // A+, later roster: redundant with 1
    t.at(d, 3) = 0.8 * z1 + 0.6 * z2; // B-: rho .8 with 1, admitted
    t.at(d, 4) = z1 + 0.9 * (0.8 * z1 + 0.6 * z2) + 0.05 * z3; // D: above .9 with 1 and 3
  }
  t.tiers = {4U, 0U, 0U, 5U, 9U};
  const auto rows = screen(t);
  ASSERT_EQ(rows.size(), 5U);
  EXPECT_EQ(rows[1].admission_rank, 1U);
  EXPECT_EQ(rows[2].status, adm::Status::RejectRedundant);
  EXPECT_EQ(rows[2].redundant_with, 1U);
  EXPECT_EQ(rows[0].status, adm::Status::RejectRedundant); // roster first, tier B: after the A+
  EXPECT_EQ(rows[0].redundant_with, 1U);
  EXPECT_EQ(rows[3].admission_rank, 2U);
  EXPECT_EQ(rows[4].status, adm::Status::RejectRedundant);
  const auto input = t.input();
  const f64 with1 = std::fabs(*adm::pair_correlation(input, 4, 1).rho);
  const f64 with3 = std::fabs(*adm::pair_correlation(input, 4, 3).rho);
  ASSERT_GT(with1, 0.9);
  ASSERT_GT(with3, 0.9);
  EXPECT_EQ(rows[4].redundant_with, with1 > with3 ? 1U : 3U);
  EXPECT_EQ(rows[4].redundant_rho, std::max(with1, with3));
  EXPECT_EQ(rows[0].max_abs_rho_with, 1U);
  EXPECT_EQ(rows[1].max_abs_rho_with, 3U); // itself excluded
  EXPECT_EQ(adm::admitted_order(rows), (std::vector<usize>{1U, 3U}));
}

// |rho| must exceed the limit: an exact double (rho exactly 1: scaling by 2 commutes with every
// rounding) is not redundant at a limit of 1. A pair with fewer than min_common_days common
// decisions is uncorrelated and noted.
TEST(ResearchAdmission, RhoLimitIsStrictAndLowOverlapIsNoted) {
  constexpr usize kDays = 300;
  Table t(3, kDays);
  for (usize d = 0; d < kDays; ++d) {
    const f64 x = std::ldexp(static_cast<f64>((d * 7U) % 13U) - 5.0, -12);
    t.at(d, 0) = x;
    t.at(d, 1) = 2.0 * x;
    t.at(d, 2) = (d < 249U) ? kNaN : x + std::ldexp(1.0, -10); // 51 days, all shared with 0
  }
  adm::ScreenRules rules{};
  rules.rho_limit = 1.0;
  rules.min_train_days = 50;
  const auto at_one = screen(t, rules);
  EXPECT_EQ(at_one[1].status, adm::Status::Admitted);
  EXPECT_EQ(*adm::pair_correlation(t.input(), 1, 0).rho, 1.0);
  rules.rho_limit = 0.999;
  const auto below = screen(t, rules);
  EXPECT_EQ(below[1].status, adm::Status::RejectRedundant);
  EXPECT_EQ(below[1].redundant_rho, 1.0);
  EXPECT_EQ(below[2].low_overlap_with, std::vector<usize>{0U});
  EXPECT_EQ(below[2].status, adm::Status::Admitted);
}

// fit_composition_weights.newey_west_t: for n <= lag the declared lag stays in the weights
// 1 - l / 6 (mean_inference would clamp it to n - 1 and weight 1 - l / n); for n > lag the method
// is mean_inference itself, bit for bit; a constant series, n < 2 and an unknown method have no t.
TEST(ResearchAdmission, HacMethodReproducesTheFitter) {
  const std::vector<f64> x{-1.0, -1.2, -0.9, -1.1};
  f64 m = 0.0;
  for (const f64 v : x) m += v;
  m /= 4.0;
  f64 lrv = 0.0;
  for (const f64 v : x) lrv += (v - m) * (v - m);
  lrv /= 4.0;
  for (usize l = 1; l <= 3; ++l) {
    f64 cross = 0.0;
    for (usize i = l; i < 4; ++i) cross += (x[i] - m) * (x[i - l] - m);
    lrv += 2.0 * (1.0 - static_cast<f64>(l) / 6.0) * cross / 4.0;
  }
  const f64 fitter = m / std::sqrt(lrv / 4.0);
  const auto got = adm::hac_t(x, adm::HacMethod::FitterNeweyWestV1, 5);
  ASSERT_TRUE(got.has_value());
  EXPECT_NEAR(*got, fitter, 1e-14 * std::fabs(fitter));
  const auto clamped = hac::mean_inference(x, hac::Kernel::BartlettV1, 5, false);
  EXPECT_GT(std::fabs(clamped.t - fitter), 1e-6); // the clamped weights are another statistic
  const std::vector<f64> y{0.3, -0.1, 0.25, 0.05, -0.2, 0.4, 0.1};
  const auto engine = hac::mean_inference(y, hac::Kernel::BartlettV1, 5, false);
  EXPECT_EQ(adm::hac_t(y, adm::HacMethod::FitterNeweyWestV1, 5), engine.t);
  EXPECT_FALSE(adm::hac_t(std::vector<f64>(9, 0.1), adm::HacMethod::FitterNeweyWestV1, 5));
  EXPECT_FALSE(adm::hac_t(std::vector<f64>{1.0}, adm::HacMethod::FitterNeweyWestV1, 5));
  EXPECT_FALSE(adm::hac_t(y, adm::HacMethod::Unknown, 5));
  EXPECT_EQ(adm::hac_method_name(adm::HacMethod::FitterNeweyWestV1), "fitter-newey-west-v1");
}

TEST(ResearchAdmission, RefusesMalformedInput) {
  Table t(2, 10);
  t.priors[1] = -1; // v4 embeds the prior sign in the DSL
  EXPECT_FALSE(adm::screen_v4(t.input(), {}).has_value());
  t.priors[1] = 1;
  t.mask[3] = 2U;
  EXPECT_FALSE(adm::screen_v4(t.input(), {}).has_value());
  t.mask[3] = 1U;
  t.taus.pop_back();
  EXPECT_FALSE(adm::screen_v4(t.input(), {}).has_value());
  t.taus.push_back(0.05);
  adm::ScreenRules unknown{};
  unknown.hac_method = adm::HacMethod::Unknown;
  EXPECT_FALSE(adm::screen_v4(t.input(), unknown).has_value());
  EXPECT_TRUE(adm::screen_v4(t.input(), {}).has_value());
  const Table empty(0, 10);
  const auto none = adm::screen_v4(empty.input(), {});
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_EQ(adm::screen_from_name("v4-prior-v2"), adm::ScreenId::V4PriorV2);
  EXPECT_FALSE(adm::screen_from_name("v3-admit-v1").has_value());
}

// Report only (F-3): the mean of s_k h over the live TRAIN decisions and its HAC t at the
// 21-session overlap (eval::hac HorizonAwareV3, label horizon 21), undefined at n <= 21; no prior:
// no value; NaN and masked decisions are skipped.
TEST(ResearchAdmission, TradedHorizonClosedForm) {
  constexpr usize kDays = 80;
  std::vector<f64> h(kDays * 3, kNaN);
  std::vector<u8> mask(kDays, 1U);
  mask[0] = 0U;
  Lcg rng{5};
  std::vector<f64> live;
  for (usize d = 0; d < kDays; ++d) {
    const f64 v = 0.01 + 0.02 * (rng.next() - 0.5);
    h[d * 3 + 0] = v;
    h[d * 3 + 2] = v;
    if (d >= 60) h[d * 3 + 1] = v; // 20 live days
    if (d > 0) live.push_back(v);
  }
  const std::vector<i32> priors{1, 1, 0};
  const auto rows = adm::traded_horizon(3, kDays, h, mask, priors);
  ASSERT_TRUE(rows.has_value());
  const auto expected = hac::mean_tstat(live, hac::TStatRule::HorizonAwareV3, 21);
  ASSERT_EQ(expected.defined, 1U);
  EXPECT_EQ((*rows)[0].days, kDays - 1);
  EXPECT_EQ((*rows)[0].ic, hac::detail::mean_of(live));
  EXPECT_EQ((*rows)[0].hac_t, expected.t);
  EXPECT_EQ((*rows)[1].days, 20U);
  EXPECT_TRUE((*rows)[1].ic.has_value());
  EXPECT_FALSE((*rows)[1].hac_t.has_value()); // n <= 21
  EXPECT_EQ((*rows)[2].days, kDays - 1);
  EXPECT_FALSE((*rows)[2].ic.has_value());
  EXPECT_FALSE((*rows)[2].hac_t.has_value());
  EXPECT_EQ(adm::kTradedHorizonSessions, 21U);
  EXPECT_FALSE(adm::traded_horizon(3, kDays, std::span<const f64>(h).first(10), mask, priors));
}
