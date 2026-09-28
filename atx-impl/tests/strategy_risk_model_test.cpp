// atx-risk-v1 (platform v7 lane L4): the constrained WLS, the recursive EWMA/Newey-West
// estimator against atx-engine risk V2, the bias statistic, and the model end to end on a
// planted factor panel. Synthetic inputs only.
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/risk/cov_ewma.hpp"
#include "../src/strategy_risk_model.hpp"

namespace {
using namespace atx;
namespace rk = atx::impl::strategy::risk;
namespace co = atx::core;
using Json = nlohmann::json;
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();

struct Normal {
  u64 state;
  f64 uniform() { // (0, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<f64>(state >> 11U) + 0.5) * 0x1.0p-53;
  }
  f64 operator()() {
    const f64 a = uniform(), b = uniform();
    return std::sqrt(-2.0 * std::log(a)) * std::cos(2.0 * std::numbers::pi * b);
  }
};
f64 correlation(const std::vector<f64>& a, const std::vector<f64>& b) {
  f64 ma = 0, mb = 0; usize n = 0;
  for (usize k = 0; k < a.size(); ++k)
    if (std::isfinite(a[k]) && std::isfinite(b[k])) { ma += a[k]; mb += b[k]; ++n; }
  ma /= static_cast<f64>(n); mb /= static_cast<f64>(n);
  f64 sab = 0, saa = 0, sbb = 0;
  for (usize k = 0; k < a.size(); ++k) {
    if (!std::isfinite(a[k]) || !std::isfinite(b[k])) continue;
    sab += (a[k] - ma) * (b[k] - mb); saa += (a[k] - ma) * (a[k] - ma); sbb += (b[k] - mb) * (b[k] - mb);
  }
  return sab / std::sqrt(saa * sbb);
}

// ---- constrained WLS ----------------------------------------------------------------------
struct Rows {
  std::vector<u8> industry;
  std::vector<f64> styles, weight, cap, value;
  [[nodiscard]] rk::CrossSection view() const { return {industry, styles, weight, cap, value}; }
};
Rows planted_rows(usize rows, std::array<f64, rk::factor_count>& f, bool noise, u64 seed) {
  Normal rng{seed};
  Rows r;
  std::array<f64, 5> cap_by{};
  for (usize k = 0; k < rows; ++k) {
    r.industry.push_back(static_cast<u8>(k % 5));
    r.cap.push_back(1e9 * std::exp(rng()));
    r.weight.push_back(std::sqrt(r.cap.back()));
    cap_by[k % 5] += r.cap.back();
    for (usize s = 0; s < rk::style_count; ++s) r.styles.push_back(rng());
  }
  f.fill(missing);
  f[0] = 0.003;
  f64 constrained = 0;
  for (usize g = 0; g < 4; ++g) {
    f[rk::industry_factor(g)] = 0.01 * rng();
    constrained += cap_by[g] * f[rk::industry_factor(g)];
  }
  f[rk::industry_factor(4)] = -constrained / cap_by[4];
  for (usize s = 0; s < rk::style_count; ++s) f[rk::style_factor(s)] = 0.002 * rng();
  for (usize k = 0; k < rows; ++k) {
    f64 y = f[0] + f[rk::industry_factor(r.industry[k])];
    for (usize s = 0; s < rk::style_count; ++s) y += f[rk::style_factor(s)] * r.styles[k * rk::style_count + s];
    r.value.push_back(y + (noise ? 0.02 * rng() : 0.0));
  }
  return r;
}
TEST(RiskWls, RecoversPlantedFactorReturnsExactly) {
  std::array<f64, rk::factor_count> planted{};
  const auto rows = planted_rows(400, planted, false, 3);
  const auto fit = rk::regress_cross_section(rows.view());
  ASSERT_TRUE(fit) << fit.error().to_string();
  for (usize k = 0; k < rk::factor_count; ++k) {
    if (std::isnan(planted[k])) { EXPECT_TRUE(std::isnan(fit->factor[k])) << k; continue; }
    EXPECT_NEAR(fit->factor[k], planted[k], 1e-12) << rk::factor_name(k);
  }
  for (const f64 u : fit->residual) EXPECT_NEAR(u, 0.0, 1e-12);
  EXPECT_NEAR(fit->r2, 1.0, 1e-12);
  EXPECT_EQ(fit->active_industries, 5U);
}
TEST(RiskWls, CapConstraintHoldsAndResidualsAreOrthogonalToMarketAndStyles) {
  std::array<f64, rk::factor_count> planted{};
  const auto rows = planted_rows(600, planted, true, 5);
  const auto fit = rk::regress_cross_section(rows.view());
  ASSERT_TRUE(fit);
  std::array<f64, 5> cap_by{};
  for (usize k = 0; k < rows.cap.size(); ++k) cap_by[rows.industry[k]] += rows.cap[k];
  f64 constrained = 0, total = 0;
  for (usize g = 0; g < 5; ++g) { constrained += cap_by[g] * fit->factor[rk::industry_factor(g)]; total += cap_by[g]; }
  EXPECT_NEAR(constrained / total, 0.0, 1e-13);
  f64 market = 0;
  std::array<f64, rk::style_count> style{};
  for (usize k = 0; k < rows.value.size(); ++k) {
    market += rows.weight[k] * fit->residual[k];
    for (usize s = 0; s < rk::style_count; ++s)
      style[s] += rows.weight[k] * fit->residual[k] * rows.styles[k * rk::style_count + s];
  }
  f64 scale = 0;
  for (const f64 w : rows.weight) scale += w;
  EXPECT_NEAR(market / scale, 0.0, 1e-12);
  for (usize s = 0; s < rk::style_count; ++s) EXPECT_NEAR(style[s] / scale, 0.0, 1e-12) << s;
  // Recovery within noise: 0.02 / sqrt(600) ~ 8e-4 per factor.
  for (usize s = 0; s < rk::style_count; ++s)
    EXPECT_NEAR(fit->factor[rk::style_factor(s)], planted[rk::style_factor(s)], 5e-3) << s;
}
TEST(RiskWls, CollinearStylesAreRefusedNotSolved) {
  std::array<f64, rk::factor_count> planted{};
  auto rows = planted_rows(200, planted, true, 9);
  for (usize k = 0; k < 200; ++k) rows.styles[k * rk::style_count + 1] = rows.styles[k * rk::style_count];
  const auto fit = rk::regress_cross_section(rows.view());
  ASSERT_FALSE(fit);
  EXPECT_EQ(fit.error().code(), co::ErrorCode::Unavailable);
}

// ---- recursive EWMA / Newey-West against the engine ---------------------------------------
std::vector<f64> ar1(usize n, f64 phi, u64 seed, usize gap) {
  Normal rng{seed};
  std::vector<f64> out(n);
  f64 x = 0;
  for (usize k = 0; k < n; ++k) {
    x = phi * x + 0.01 * rng();
    out[k] = gap && k % gap == 3 ? missing : 0.0005 + x;
  }
  return out;
}
TEST(RiskEwma, VarianceWithNeweyWestEqualsEngineEwmaVarianceV2) {
  const auto series = ar1(700, 0.3, 17, 7); // oldest first, every 7th session missing
  rk::LaggedEwma fast(1, 84, 0, false), nw(1, 252, 5, false);
  for (const f64 v : series) {
    fast.push(std::span<const f64>(&v, 1)); nw.push(std::span<const f64>(&v, 1));
  }
  const std::vector<f64> newest_first(series.rbegin(), series.rend());
  const auto engine = atx::engine::risk::ewma_variance_v2(newest_first, {}, 84, 5, 252);
  ASSERT_TRUE(engine) << engine.error().to_string();
  const f64 mine = rk::serial_adjusted_variance(fast, nw, 0);
  EXPECT_NEAR(mine / engine->variance, 1.0, 1e-10);
  EXPECT_EQ(fast.observations(0), engine->observations);
}
TEST(RiskEwma, CorrelationWithNeweyWestEqualsEngineFactorCovarianceV2) {
  const usize t = 600, k = 3;
  const auto a = ar1(t, 0.2, 1, 11), b = ar1(t, -0.1, 2, 0), c = ar1(t, 0.4, 3, 5);
  std::vector<f64> mixed(t);
  for (usize s = 0; s < t; ++s) mixed[s] = std::isfinite(a[s]) ? 0.6 * a[s] + b[s] : b[s];
  rk::LaggedEwma corr(k, 504, 2, true), fast(k, 84, 0, false), nw(k, 252, 2, false);
  atx::core::linalg::MatX engine_input(static_cast<Eigen::Index>(t), static_cast<Eigen::Index>(k));
  for (usize s = 0; s < t; ++s) {
    const std::array<f64, 3> row{a[s], mixed[s], c[s]};
    corr.push(row); fast.push(row); nw.push(row);
    for (usize j = 0; j < k; ++j)
      engine_input(static_cast<Eigen::Index>(t - 1 - s), static_cast<Eigen::Index>(j)) = row[j];
  }
  const auto engine = atx::engine::risk::ewma_factor_covariance_v2(engine_input, {}, 84, 504, 2, 252);
  ASSERT_TRUE(engine) << engine.error().to_string();
  const auto& f = engine->covariance;
  for (usize i = 0; i < k; ++i) {
    const auto ii = static_cast<Eigen::Index>(i);
    EXPECT_NEAR(rk::serial_adjusted_variance(fast, nw, i) / f(ii, ii), 1.0, 1e-10) << i;
    for (usize j = i + 1; j < k; ++j) {
      const auto jj = static_cast<Eigen::Index>(j);
      const f64 engine_rho = f(ii, jj) / std::sqrt(f(ii, ii) * f(jj, jj));
      const f64 rho = corr.covariance(i, j) / std::sqrt(corr.covariance(i, i) * corr.covariance(j, j));
      EXPECT_NEAR(rho, engine_rho, 1e-10) << i << ',' << j;
    }
  }
}
TEST(RiskEwma, EqualWeightsWithoutLagsIsThePopulationCovariance) {
  rk::LaggedEwma equal(2, 0, 0, true);
  const std::array<std::array<f64, 2>, 4> rows{{{1.0, 2.0}, {2.0, 1.0}, {3.0, 5.0}, {4.0, 4.0}}};
  for (const auto& r : rows) equal.push(r);
  // means 2.5 and 3; population moments: var x 1.25, var y 2.5, cov (1.5+1+1+1.5)/4 = 1.25.
  EXPECT_NEAR(equal.zero_lag(0, 0), 1.25, 1e-15);
  EXPECT_NEAR(equal.zero_lag(1, 1), 2.5, 1e-15);
  EXPECT_NEAR(equal.zero_lag(0, 1), 1.25, 1e-15);
  EXPECT_NEAR(equal.mean(1), 3.0, 1e-15);
}

// ---- bias statistic -----------------------------------------------------------------------
TEST(RiskBias, StatisticIsNearOneOnCorrectlySpecifiedForecasts) {
  Normal rng{2024};
  std::vector<f64> z;
  for (usize k = 0; k < 2000; ++k) {
    const f64 vol = 0.005 + 0.02 * rng.uniform(); // a time-varying but correct forecast
    z.push_back(vol * rng() / vol);
  }
  const f64 b = rk::bias_statistic(z);
  const auto band = rk::bias_band(z.size());
  EXPECT_GT(b, 1.0 - 2.0 * (band.hi - 1.0)); EXPECT_LT(b, 1.0 + 2.0 * (band.hi - 1.0));
  EXPECT_NEAR(rk::sample_kurtosis(z), 3.0, 0.5);
  const auto rolling = rk::rolling_bias(z, 63);
  EXPECT_TRUE(std::isnan(rolling[61])); EXPECT_TRUE(std::isfinite(rolling[62]));
  EXPECT_DOUBLE_EQ(rolling[62], rk::bias_statistic(std::span<const f64>(z).first(63)));
  usize inside = 0, windows = 0;
  const auto b63 = rk::bias_band(63);
  for (usize k = 62; k < rolling.size(); ++k) { ++windows; inside += rolling[k] >= b63.lo && rolling[k] <= b63.hi; }
  EXPECT_GT(static_cast<f64>(inside) / static_cast<f64>(windows), 0.85);
  // Mis-scaled forecasts (vol x 0.8) push b to ~1.25, outside the band.
  std::vector<f64> scaled(z);
  for (auto& v : scaled) v /= 0.8;
  EXPECT_GT(rk::bias_statistic(scaled), band.hi);
}
TEST(RiskBias, BandsFollowUse4AppendixA) {
  const auto normal = rk::bias_band(252);
  EXPECT_NEAR(normal.hi - 1.0, std::sqrt(2.0 / 252.0), 1e-15);
  const auto gaussian = rk::kurtosis_band(252, 3.0);
  EXPECT_NEAR(gaussian.hi - 1.0, 1.96 * std::sqrt(2.0 / (4.0 * 252.0)), 1e-15);
  EXPECT_GT(rk::kurtosis_band(252, 13.5).hi, normal.hi); // fat tails widen the band
}

// ---- the model end to end on a planted panel ----------------------------------------------
// r_it = m_t + g_{ind(i),t} + b_t v_i + sigma_i e_it with cap-weighted industry returns summing
// to 0, v_i the (standard normal) book-to-price descriptor, three FF49 industries.
struct Planted {
  usize d{}, n{};
  std::vector<i64> sessions;
  std::vector<u64> ids;
  std::vector<f64> close, raw, volume, cap, sigma, market, loading;
  std::vector<u8> present, member, industry;
  std::vector<f32> value;
  std::array<std::vector<f64>, 3> industry_return;
  Planted(usize dates, usize names, u64 seed)
      : d(dates), n(names), close(dates * names), raw(dates * names), volume(dates * names, 2e5),
        cap(dates * names), sigma(names), market(dates, missing), loading(dates, missing),
        present(dates * names, 1), member(dates * names, 1), industry(dates * names),
        value(dates * names) {
    Normal rng{seed};
    auto date = std::chrono::sys_days{std::chrono::year{2019} / 1 / 2};
    while (sessions.size() < d) {
      const std::chrono::weekday wd{date};
      if (wd != std::chrono::Saturday && wd != std::chrono::Sunday)
        sessions.push_back(static_cast<i64>(date.time_since_epoch().count()) * day_ns);
      date += std::chrono::days{1};
    }
    std::vector<f64> caps(n), v(n);
    std::array<f64, 3> cap_by{};
    for (usize i = 0; i < n; ++i) {
      ids.push_back(1000 + i);
      caps[i] = 1e9 * std::exp(rng()); v[i] = rng(); sigma[i] = 0.01 + 0.02 * rng.uniform();
      cap_by[i % 3] += caps[i];
    }
    for (auto& g : industry_return) g.assign(d, missing);
    for (usize t = 0; t < d; ++t) {
      if (t > 0) {
        market[t] = 0.01 * rng(); loading[t] = 0.004 * rng();
        industry_return[0][t] = 0.005 * rng(); industry_return[1][t] = 0.005 * rng();
        industry_return[2][t] = -(cap_by[0] * industry_return[0][t] + cap_by[1] * industry_return[1][t]) / cap_by[2];
      }
      for (usize i = 0; i < n; ++i) {
        const usize k = t * n + i;
        cap[k] = caps[i]; value[k] = static_cast<f32>(v[i]); industry[k] = static_cast<u8>(1 + i % 3);
        const f64 r = t == 0 ? 0.0
            : market[t] + industry_return[i % 3][t] + loading[t] * v[i] + sigma[i] * rng();
        close[k] = t == 0 ? 50.0 : close[k - n] * (1.0 + r);
        raw[k] = close[k];
      }
    }
  }
  [[nodiscard]] rk::RiskPanel panel() const {
    rk::RiskPanel p;
    p.dates = d; p.instruments = n; p.sessions = sessions; p.ids = ids;
    p.close = close; p.raw_close = raw; p.volume = volume; p.present = present; p.member = member;
    p.cap = cap; p.industry = industry; p.descriptors[0] = value;
    return p;
  }
};
struct Collect final : rk::RiskSink {
  std::vector<std::array<f64, rk::factor_count>> factor;
  std::vector<u8> fitted, forecast;
  std::vector<f64> last_specific, value_z, caps;
  std::vector<u8> eligible;
  const Planted* planted{};
  co::Status on_day(const rk::RiskDay& day) override {
    std::array<f64, rk::factor_count> f{};
    std::copy(day.factor_return.begin(), day.factor_return.end(), f.begin());
    factor.push_back(f);
    fitted.push_back(static_cast<u8>(day.fitted)); forecast.push_back(static_cast<u8>(day.forecast));
    if (day.date + 1 == planted->d) {
      last_specific.assign(day.specific_variance.begin(), day.specific_variance.end());
      eligible.assign(day.eligible.begin(), day.eligible.end());
      for (usize i = 0; i < planted->n; ++i) {
        value_z.push_back(day.styles[i * rk::style_count + static_cast<usize>(rk::Style::Value)]);
        caps.push_back(planted->cap[day.date * planted->n + i]);
      }
    }
    return co::Ok();
  }
};
TEST(RiskModel, RecoversPlantedFactorsForecastsSpecificRiskAndIsUnbiased) {
  // Specific forecasts start once names have 252 residuals (t ~ 253): 420 sessions leave ~166.
  const Planted planted(420, 600, 41);
  const auto panel = planted.panel();
  Collect collect; collect.planted = &planted;
  rk::BiasHarness harness(panel, 32, 7, {});
  std::array<rk::RiskSink*, 2> sinks{&collect, &harness};
  const auto status = rk::run_risk_model(panel, rk::RiskModelConfig{}, sinks);
  ASSERT_TRUE(status) << status.error().to_string();
  ASSERT_EQ(collect.factor.size(), planted.d);
  usize fitted = 0;
  for (usize t = 1; t < planted.d; ++t) fitted += collect.fitted[t];
  EXPECT_EQ(fitted, planted.d - 1);
  std::vector<f64> f_market, f_value, f_ind0;
  for (const auto& f : collect.factor) {
    f_market.push_back(f[0]);
    f_value.push_back(f[rk::style_factor(static_cast<usize>(rk::Style::Value))]);
    f_ind0.push_back(f[rk::industry_factor(0)]); // FF49 id 1 -> slot 0
  }
  EXPECT_GT(correlation(f_market, planted.market), 0.95);
  EXPECT_GT(correlation(f_value, planted.loading), 0.9);
  EXPECT_GT(correlation(f_ind0, planted.industry_return[0]), 0.9);
  EXPECT_TRUE(collect.forecast.back());
  // Specific variance near the planted sigma^2 (estimation with 62 factors on 600 names
  // deflates residual variance by ~10%).
  f64 ratio = 0; usize names = 0;
  for (usize i = 0; i < planted.n; ++i) {
    if (!std::isfinite(collect.last_specific[i])) continue;
    ratio += collect.last_specific[i] / (planted.sigma[i] * planted.sigma[i]); ++names;
  }
  ASSERT_GT(names, planted.n / 2);
  EXPECT_GT(ratio / static_cast<f64>(names), 0.6); EXPECT_LT(ratio / static_cast<f64>(names), 1.4);
  // Exposure standardization: cap-weighted mean ~0, equal-weighted SD ~1 (clipping at 3).
  f64 cw = 0, cx = 0, sum = 0, sq = 0; usize count = 0;
  for (usize i = 0; i < planted.n; ++i) {
    if (!collect.eligible[i]) continue;
    cw += collect.caps[i]; cx += collect.caps[i] * collect.value_z[i];
    sum += collect.value_z[i]; sq += collect.value_z[i] * collect.value_z[i]; ++count;
  }
  EXPECT_NEAR(cx / cw, 0.0, 0.02);
  const f64 mean = sum / static_cast<f64>(count);
  EXPECT_NEAR(std::sqrt(sq / static_cast<f64>(count) - mean * mean), 1.0, 0.05);
  // Bias harness: random dollar-neutral portfolios and the market factor near 1.
  std::vector<f64> random_z, market_z;
  for (const auto& s : harness.series()) {
    if (s.family == "random") random_z.insert(random_z.end(), s.z.begin(), s.z.end());
    if (s.family == "factor" && s.name == "market") market_z = s.z;
  }
  ASSERT_GT(random_z.size(), 1000U);
  const f64 b_random = rk::bias_statistic(random_z);
  EXPECT_GT(b_random, 0.8); EXPECT_LT(b_random, 1.3);
  ASSERT_GT(market_z.size(), 100U);
  const f64 b_market = rk::bias_statistic(market_z);
  EXPECT_GT(b_market, 0.7); EXPECT_LT(b_market, 1.4);
}
TEST(RiskModel, BookWeightsFormTheBookSeriesAndUnknownRowsAreRefused) {
  const Planted planted(300, 150, 43);
  const auto panel = planted.panel();
  std::vector<rk::BookWeight> book;
  for (usize t = 262; t < 292; ++t)
    for (usize i = 0; i < 10; ++i)
      book.push_back({planted.sessions[t], planted.ids[i], i % 2 ? 0.05 : -0.05});
  rk::RiskModelConfig cfg;
  cfg.min_regression_names = 100;
  rk::BiasHarness harness(panel, 4, 7, book);
  std::array<rk::RiskSink*, 1> sinks{&harness};
  ASSERT_TRUE(rk::run_risk_model(panel, cfg, sinks));
  const auto& series = harness.series();
  ASSERT_EQ(series.back().family, "book");
  EXPECT_FALSE(series.back().z.empty());
  auto bad = book;
  bad.push_back({planted.sessions[270], 999'999, 0.1});
  rk::BiasHarness refused(panel, 4, 7, bad);
  std::array<rk::RiskSink*, 1> one{&refused};
  EXPECT_FALSE(rk::run_risk_model(panel, cfg, one));
}
TEST(RiskModel, NamesAndReturnGuard) {
  EXPECT_EQ(rk::factor_name(0), "market");
  EXPECT_EQ(rk::factor_name(1), "ind_ff1");
  EXPECT_EQ(rk::factor_name(50), "ind_residual");
  EXPECT_EQ(rk::factor_name(51), "style_size");
  EXPECT_EQ(rk::factor_name(61), "style_short_interest");
  Planted planted(5, 2, 1);
  planted.close[3 * 2 + 0] = planted.close[2 * 2 + 0] * 10.0; // adjusted jump, raw unchanged
  const auto p = planted.panel();
  EXPECT_TRUE(std::isnan(rk::interval_return(p, 3, 0)));
  EXPECT_TRUE(std::isfinite(rk::interval_return(p, 3, 1)));
  EXPECT_TRUE(std::isnan(rk::interval_return(p, 0, 1)));
}
// ---- restricted roles: membership independent of presence -----------------------------------
// The -lo roles restrict membership (linked-operating names) and a member may be absent at a
// session. Every 4th name is a present non-member; three members are absent at three sessions
// (prices NaN, as the role contract requires).
constexpr std::array<usize, 3> absent_sessions{150, 151, 200};
constexpr std::array<usize, 3> absent_names{0, 5, 10};
void restrict_membership(Planted& p) {
  for (usize t = 0; t < p.d; ++t)
    for (usize i = 3; i < p.n; i += 4) p.member[t * p.n + i] = 0;
  for (const usize t : absent_sessions)
    for (const usize i : absent_names) {
      const usize k = t * p.n + i;
      p.present[k] = 0; p.close[k] = missing; p.raw[k] = missing; p.volume[k] = missing;
    }
}
struct Rows final : rk::RiskSink {
  std::vector<usize> regression_rows;
  std::vector<u8> fitted, last_eligible;
  std::vector<f64> last_specific;
  usize dates{};
  co::Status on_day(const rk::RiskDay& day) override {
    regression_rows.push_back(day.regression_rows);
    fitted.push_back(static_cast<u8>(day.fitted));
    if (day.date + 1 == dates) {
      last_eligible.assign(day.eligible.begin(), day.eligible.end());
      last_specific.assign(day.specific_variance.begin(), day.specific_variance.end());
    }
    return co::Ok();
  }
};
TEST(RiskModel, AbsentMembersAndNonMembersStayOutOfTheFit) {
  Planted planted(300, 200, 53);
  restrict_membership(planted);
  const auto panel = planted.panel();
  Rows rows; rows.dates = planted.d;
  std::array<rk::RiskSink*, 1> sinks{&rows};
  const auto status = rk::run_risk_model(panel, rk::RiskModelConfig{}, sinks);
  ASSERT_TRUE(status) << status.error().to_string();
  const usize n = planted.n;
  for (usize t = 1; t < planted.d; ++t) {
    usize expected = 0; // members present at t-1 (with cap) and at t: a present return
    for (usize i = 0; i < n; ++i)
      expected += planted.member[(t - 1) * n + i] && planted.present[(t - 1) * n + i] &&
                  planted.present[t * n + i] ? 1U : 0U;
    EXPECT_EQ(rows.regression_rows[t], expected) << t;
    EXPECT_TRUE(rows.fitted[t]) << t;
  }
  EXPECT_EQ(rows.regression_rows[150], 150U - 3U); // absent at t
  EXPECT_EQ(rows.regression_rows[152], 150U - 3U); // absent at t - 1
  EXPECT_EQ(rows.regression_rows[153], 150U);
  for (usize i = 3; i < n; i += 4) {
    EXPECT_FALSE(rows.last_eligible[i]) << i;
    EXPECT_TRUE(std::isfinite(rows.last_specific[i])) << i; // held non-members stay priced
  }
}

// The `risk` verb on a pinned synthetic restricted role + fields set.
struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<u64> counter{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
        ("atx_risk_" + std::to_string(tick) + "_" + std::to_string(counter.fetch_add(1)));
    if (!std::filesystem::create_directory(path)) throw std::runtime_error("fixture directory");
  }
  ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
};
template <class T>
Json write_payload(const std::filesystem::path& file, const std::vector<T>& values) {
  std::ofstream out(file, std::ios::binary);
  const auto bytes = std::as_bytes(std::span(values));
  // SAFETY: char writes the object representation of contiguous arithmetic fixture data.
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) throw std::runtime_error("fixture payload write");
  return Json{{"bytes", bytes.size()}, {"sha256", co::sha256_file(file.string()).value()}};
}
std::string write_json(const std::filesystem::path& path, const Json& value) {
  std::ofstream out(path, std::ios::binary);
  out << value.dump(2) << '\n';
  out.close();
  if (!out) throw std::runtime_error("fixture JSON write");
  return co::sha256_file(path.string()).value();
}
struct PinnedSet {
  std::string role, role_sha, fields, fields_sha;
};
// Role (atx.recent-research-role/v1) and fields (atx.research-role-fields/v1: me_company,
// grp_ff49, be) for `p` under `dir`.
PinnedSet write_role_and_fields(const std::filesystem::path& dir, const Planted& p) {
  const auto role_dir = dir / "role", fields_dir = dir / "fields";
  std::filesystem::create_directories(role_dir);
  std::filesystem::create_directories(fields_dir);
  Json files = Json::object();
  files["sessions.i64"] = write_payload(role_dir / "sessions.i64", p.sessions);
  files["ids.u64"] = write_payload(role_dir / "ids.u64", p.ids);
  files["close.f64"] = write_payload(role_dir / "close.f64", p.close);
  files["raw_close.f64"] = write_payload(role_dir / "raw_close.f64", p.raw);
  files["volume.f64"] = write_payload(role_dir / "volume.f64", p.volume);
  files["present.u8"] = write_payload(role_dir / "present.u8", p.present);
  files["member.u8"] = write_payload(role_dir / "member.u8", p.member);
  const Json role{{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
      {"volume_basis", "raw-share-volume"}, {"dates", p.d}, {"instruments", p.n}, {"files", files}};
  PinnedSet out;
  out.role = (role_dir / "manifest.json").string();
  out.role_sha = write_json(role_dir / "manifest.json", role);
  std::vector<f64> industry(p.industry.begin(), p.industry.end()), be(p.d * p.n);
  for (usize k = 0; k < be.size(); ++k) be[k] = static_cast<f64>(p.value[k]) * p.cap[k];
  Json field_list = Json::array(), field_files = Json::object();
  const auto add = [&](const std::string& name, const std::vector<f64>& values) {
    const auto receipt = write_payload(fields_dir / (name + ".f64"), values);
    field_files[name + ".f64"] = receipt;
    field_list.push_back(Json{{"name", name}, {"file", name + ".f64"}, {"dtype", "<f8"},
        {"layout", "date-major"}, {"shape", Json::array({p.d, p.n})},
        {"sha256", receipt.at("sha256")}, {"point_in_time", true}});
  };
  add("me_company", p.cap);
  add("grp_ff49", industry);
  add("be", be);
  const Json fields{{"schema", "atx.research-role-fields/v1"}, {"status", "complete"},
      {"role", {{"manifest_sha256", out.role_sha},
                {"sessions_sha256", files.at("sessions.i64").at("sha256")},
                {"ids_sha256", files.at("ids.u64").at("sha256")},
                {"member_sha256", files.at("member.u8").at("sha256")}}},
      {"fields", field_list}, {"files", field_files}};
  out.fields = (fields_dir / "manifest.json").string();
  out.fields_sha = write_json(fields_dir / "manifest.json", fields);
  return out;
}
int run_verb(const PinnedSet& s, const std::filesystem::path& output, std::string& error) {
  std::vector<std::string> args{"risk", "--role", s.role, "--role-sha256", s.role_sha,
      "--fields", s.fields, "--fields-sha256", s.fields_sha, "--output", output.string(),
      "--random-portfolios", "4"};
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  std::ostringstream out, err;
  const int code = rk::dispatch_risk_model(static_cast<int>(argv.size()), argv.data(), out, err);
  error = err.str();
  return code;
}
TEST(RiskVerb, RestrictedRoleWithAbsentMembersRunsAndMalformedMasksAreRefused) {
  Directory dir;
  Planted planted(300, 200, 59);
  restrict_membership(planted);
  const auto pinned = write_role_and_fields(dir.path / "ok", planted);
  std::string error;
  ASSERT_EQ(run_verb(pinned, dir.path / "ok" / "out", error), 0) << error;
  std::ifstream in(dir.path / "ok" / "out" / "manifest.json");
  Json manifest;
  in >> manifest;
  EXPECT_EQ(manifest.at("status"), "complete");
  usize members = 0;
  for (const u8 m : planted.member) members += m;
  EXPECT_EQ(manifest.at("role_masks").at("member_cells").get<usize>(), members);
  EXPECT_EQ(manifest.at("role_masks").at("member_absent_cells").get<usize>(), 9U);
  // A non-binary mask is refused.
  Planted bad_mask = planted;
  bad_mask.member[10 * bad_mask.n + 1] = 2;
  const auto refused = write_role_and_fields(dir.path / "mask", bad_mask);
  EXPECT_EQ(run_verb(refused, dir.path / "mask" / "out", error), 1);
  EXPECT_NE(error.find("not binary"), std::string::npos) << error;
  // An absent cell with a price breaks the role contract.
  Planted bad_price = planted;
  bad_price.present[20 * bad_price.n + 2] = 0;
  const auto contract = write_role_and_fields(dir.path / "price", bad_price);
  EXPECT_EQ(run_verb(contract, dir.path / "price" / "out", error), 1);
  EXPECT_NE(error.find("price contract"), std::string::npos) << error;
}
} // namespace
