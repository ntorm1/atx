// P9 lane COV (cov-design 3.1-3.3, brief COV items 2, 3, 6): the no-look-ahead invariant of the
// risk model's per-session blocks under both recipes (CovTruncation: truncate / perturb / widen
// the panel after a cut, every block through the cut byte-identical; a planted leak is caught),
// the atx-cov-v1 S2b eigenfactor adjustment (CovEigen) and the extended bias families (CovBias).
// Synthetic panels only (strategy_cov_fixture.hpp).

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <Eigen/Dense>
#include <gtest/gtest.h>
#include "strategy_cov_fixture.hpp"

namespace {
using namespace atx_cov_test;

constexpr std::array<rk::RiskRecipe, 2> recipes{rk::RiskRecipe::RiskV11, rk::RiskRecipe::CovV1};
constexpr usize planted_dates = 150, planted_names = 160;

const CovPanel& base_panel() {
  static const CovPanel p(planted_dates, planted_names, 17);
  return p;
}
const BlockLog& full_log(rk::RiskRecipe r) {
  static const BlockLog v11 = run_blocks(base_panel(), small_config(rk::RiskRecipe::RiskV11));
  static const BlockLog cov = run_blocks(base_panel(), small_config(rk::RiskRecipe::CovV1));
  return r == rk::RiskRecipe::CovV1 ? cov : v11;
}
std::string violation_text(const std::optional<usize>& v) {
  return v ? "first violating session index " + std::to_string(*v) : "none";
}

// ---- CovTruncation (cov-design 3.3; ruling COV-4) ---------------------------------------------
TEST(CovTruncation, BlocksBeforeTheCutAreByteIdentical) {
  for (const auto recipe : recipes) {
    const auto& full = full_log(recipe);
    ASSERT_EQ(full.blocks.size(), planted_dates);
    usize adjusted = 0, forecast = 0;
    for (const u32 f : full.flags) {
      adjusted += (f & er::cov::date_eigen_adjusted) ? 1U : 0U;
      forecast += (f & er::cov::date_forecast) ? 1U : 0U;
    }
    EXPECT_GT(forecast, 80U) << rk::recipe_id(recipe);
    if (recipe == rk::RiskRecipe::CovV1) {
      EXPECT_GT(adjusted, 100U);
    } else {
      EXPECT_EQ(adjusted, 0U);
    }
    for (const usize cut : {usize{40}, usize{90}, usize{130}}) {
      const auto truncated = run_blocks(base_panel().truncate_after(cut), small_config(recipe));
      ASSERT_EQ(truncated.blocks.size(), cut + 1);
      const auto v = first_violation(full, truncated, cut);
      EXPECT_FALSE(v.has_value()) << rk::recipe_id(recipe) << " cut " << cut << ": "
                                  << violation_text(v);
      // Teeth: the truncated run is another series (no blocks after the cut).
      EXPECT_LT(truncated.blocks.size(), full.blocks.size());
    }
  }
}

TEST(CovTruncation, PerturbedFutureLeavesThePastUnchanged) {
  constexpr usize cut = 90;
  const CovPanel perturbed = base_panel().perturb_after(cut, 5);
  for (const auto recipe : recipes) {
    const auto& full = full_log(recipe);
    const auto log = run_blocks(perturbed, small_config(recipe));
    ASSERT_EQ(log.blocks.size(), planted_dates);
    const auto v = first_violation(full, log, cut);
    EXPECT_FALSE(v.has_value()) << rk::recipe_id(recipe) << ": " << violation_text(v);
    usize differing = 0;
    for (usize t = cut + 1; t < planted_dates; ++t) differing += full.blocks[t] != log.blocks[t] ? 1U : 0U;
    EXPECT_GT(differing, 0U) << rk::recipe_id(recipe) << ": the perturbation must reach the future";
    EXPECT_NE(full.blocks[cut + 1], log.blocks[cut + 1]) << rk::recipe_id(recipe);
  }
}

TEST(CovTruncation, FutureInstrumentsDoNotMoveThePast) {
  constexpr usize cut = 90;
  const CovPanel wider = base_panel().add_future_instruments(cut, 6);
  ASSERT_EQ(wider.n, planted_names + 6);
  for (const auto recipe : recipes) {
    const auto& full = full_log(recipe);
    const auto log = run_blocks(wider, small_config(recipe));
    for (usize t = 0; t <= cut; ++t) {
      EXPECT_TRUE(same_by_id(full.blocks[t], log.blocks[t])) << rk::recipe_id(recipe) << ' ' << t;
      EXPECT_EQ(full.history_first[t], log.history_first[t]) << t;
      EXPECT_EQ(full.flags[t], log.flags[t]) << t;
    }
    // Teeth: the role indexes shifted, so the raw bytes differ while the projection agrees.
    EXPECT_NE(full.blocks[cut], log.blocks[cut]) << rk::recipe_id(recipe);
    // After the cut the new names have rows.
    EXPECT_GT(parse_block(log.blocks.back()).rows, parse_block(full.blocks.back()).rows);
  }
}

TEST(CovTruncation, DetectsAPlantedLeak) {
  constexpr usize cut = 90;
  const CovPanel truncated = base_panel().truncate_after(cut);
  ASSERT_NE(truncated.full_sample_value_mean(), base_panel().full_sample_value_mean());
  for (const auto recipe : recipes) {
    const auto cfg = small_config(recipe);
    const auto leaky_full = run_blocks(base_panel(), cfg, 1.0);
    const auto leaky_cut = run_blocks(truncated, cfg, 1.0);
    const auto v = first_violation(leaky_full, leaky_cut, cut);
    ASSERT_TRUE(v.has_value()) << rk::recipe_id(recipe) << ": the harness missed the planted leak";
    EXPECT_LE(*v, cut);
    // The first violating session is the first whose value style is nonzero for some row.
    const auto view = parse_block(leaky_full.blocks[*v]);
    bool exposed = false;
    for (usize r = 0; r < view.rows; ++r)
      exposed = exposed || view.exposures_f32[r * rk::style_count + value_style] != 0.0F;
    EXPECT_TRUE(exposed);
  }
}

// ---- CovEigen (S2b) ------------------------------------------------------------------------------
// F = Q diag(lambda) Q' with lambda geometric in [1e-6, 1e-4] (ascending) and Q a seeded rotation.
std::vector<f64> planted_block(usize m, u64 seed, std::vector<f64>* truth_values = nullptr) {
  Normal rng{seed};
  Eigen::MatrixXd g(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(m));
  for (Eigen::Index a = 0; a < g.rows(); ++a)
    for (Eigen::Index b = 0; b < g.cols(); ++b) g(a, b) = rng();
  const Eigen::HouseholderQR<Eigen::MatrixXd> qr(g);
  const Eigen::MatrixXd q = qr.householderQ();
  Eigen::VectorXd lambda(static_cast<Eigen::Index>(m));
  for (usize k = 0; k < m; ++k)
    lambda[static_cast<Eigen::Index>(k)] =
        1e-6 * std::pow(100.0, static_cast<f64>(k) / static_cast<f64>(m - 1));
  if (truth_values) truth_values->assign(lambda.data(), lambda.data() + m);
  const Eigen::MatrixXd f = q * lambda.asDiagonal() * q.transpose();
  std::vector<f64> out(m * m);
  for (usize a = 0; a < m; ++a)
    for (usize b = 0; b < m; ++b)
      out[a * m + b] = 0.5 * (f(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) +
                              f(static_cast<Eigen::Index>(b), static_cast<Eigen::Index>(a)));
  return out;
}
Eigen::MatrixXd as_matrix(std::span<const f64> v, usize m) {
  Eigen::MatrixXd f(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(m));
  for (usize a = 0; a < m; ++a)
    for (usize b = 0; b < m; ++b)
      f(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) = v[a * m + b];
  return f;
}
const i64 probe_session = weekday_sessions(1).front();

TEST(CovEigen, RecipeV11IsBitIdenticalToToday) {
  EXPECT_EQ(rk::RiskModelConfig{}.recipe, rk::RiskRecipe::RiskV11);
  // RiskV11 never reads the S2b constants: changed constants give the same bytes, no eigen flag,
  // eigen outcome 0 (not run) and NaN gammas in every block.
  auto inert = small_config(rk::RiskRecipe::RiskV11);
  inert.eigen_simulations = 7; inert.eigen_amplification = 2.5; inert.eigen_seed = 99;
  inert.eigen_max_bytes = 1U << 20U;
  const auto log = run_blocks(base_panel(), inert);
  const auto& reference = full_log(rk::RiskRecipe::RiskV11);
  const auto v = first_violation(reference, log, planted_dates - 1);
  EXPECT_FALSE(v.has_value()) << violation_text(v);
  constexpr usize eigen_outcome_slot = 21;
  ASSERT_STREQ(rk::cov_diag_names()[eigen_outcome_slot], "eigen_outcome");
  for (usize t = 0; t < log.blocks.size(); ++t) {
    EXPECT_EQ(log.flags[t] & er::cov::date_eigen_adjusted, 0U) << t;
    const auto view = parse_block(log.blocks[t]);
    EXPECT_EQ(view.diag[eigen_outcome_slot], 0.0) << t;
    for (const f64 g : view.eigen_gamma) EXPECT_TRUE(std::isnan(g)) << t;
  }
}

TEST(CovEigen, ZeroSimulationsEqualsV11) {
  auto zero = small_config(rk::RiskRecipe::CovV1);
  zero.eigen_simulations = 0;
  const auto log = run_blocks(base_panel(), zero);
  const auto& v11 = full_log(rk::RiskRecipe::RiskV11);
  usize adjusted = 0;
  for (usize t = 0; t < planted_dates; ++t) {
    adjusted += (log.flags[t] & er::cov::date_eigen_adjusted) ? 1U : 0U;
    const auto a = parse_block(log.blocks[t]), b = parse_block(v11.blocks[t]);
    EXPECT_TRUE(same_bits(a.factor_cov, b.factor_cov)) << t;
    EXPECT_TRUE(same_bits(a.specific, b.specific)) << t;
    EXPECT_TRUE(same_bits(a.exposures_f32, b.exposures_f32)) << t;
    EXPECT_TRUE(same_bits(a.group, b.group)) << t;
    EXPECT_TRUE(same_bits(a.ids, b.ids)) << t;
    EXPECT_TRUE(same_bits(a.factor_return, b.factor_return)) << t;
    EXPECT_TRUE(same_bits(a.factor_flags, b.factor_flags)) << t;
  }
  EXPECT_GT(adjusted, 100U); // the kernel ran (0 simulations: the block unchanged)
}

TEST(CovEigen, SmallEigenfactorsInflatedLargeNearOne) {
  constexpr usize m = 10;
  const auto cfg = small_config(rk::RiskRecipe::CovV1);
  const auto block = planted_block(m, 3);
  std::vector<f64> mean_gamma(m, 0.0);
  constexpr usize sessions = 8;
  for (usize s = 0; s < sessions; ++s) {
    const auto e = rk::eigen_adjust_block(block, m, 60.0, probe_session + static_cast<i64>(s) * day_ns, cfg);
    ASSERT_EQ(e.outcome, rk::EigenOutcome::Adjusted);
    ASSERT_EQ(e.gamma.size(), m);
    for (usize k = 0; k < m; ++k) mean_gamma[k] += e.gamma[k] / static_cast<f64>(sessions);
    // An eigen-rotation adjustment: F_adj u_k = gamma_k^2 d_k u_k on the input's eigenbasis.
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(as_matrix(block, m));
    const Eigen::MatrixXd adjusted = as_matrix(e.covariance, m);
    for (usize k = 0; k < m; ++k) {
      const Eigen::VectorXd u = eig.eigenvectors().col(static_cast<Eigen::Index>(k));
      const f64 d = eig.eigenvalues()[static_cast<Eigen::Index>(k)];
      const Eigen::VectorXd residual = adjusted * u - e.gamma[k] * e.gamma[k] * d * u;
      EXPECT_LT(residual.norm(), 1e-9 * d * e.gamma[k] * e.gamma[k]) << k;
    }
  }
  EXPECT_GT(mean_gamma.front(), 1.02);                 // the smallest eigenfactor inflated
  EXPECT_GT(mean_gamma.front(), mean_gamma.back());
  EXPECT_NEAR(mean_gamma.back(), 1.0, 0.1);           // the largest near one
  // Short history: fewer than K + 1 effective observations keeps the block.
  const auto short_history = rk::eigen_adjust_block(block, m, static_cast<f64>(m), probe_session, cfg);
  EXPECT_EQ(short_history.outcome, rk::EigenOutcome::ShortHistory);
  EXPECT_TRUE(same_bits(std::span<const f64>(short_history.covariance), std::span<const f64>(block)));
  // A non-positive eigenvalue is refused by the kernel, the block kept.
  // Off-diagonal 2e-5, diagonal 1e-5: eigenvalue -1e-5 (m - 1 times), not positive definite.
  std::vector<f64> singular(m * m, 2e-5);
  for (usize a = 0; a < m; ++a) singular[a * m + a] = 1e-5;
  const auto refused = rk::eigen_adjust_block(singular, m, 60.0, probe_session, cfg);
  EXPECT_EQ(refused.outcome, rk::EigenOutcome::Refused);
  EXPECT_TRUE(same_bits(std::span<const f64>(refused.covariance), std::span<const f64>(singular)));
}

TEST(CovEigen, SeededPerSessionAndWorkerInvariant) {
  constexpr usize m = 8;
  const auto cfg = small_config(rk::RiskRecipe::CovV1);
  const auto block = planted_block(m, 11);
  const auto reference = rk::eigen_adjust_block(block, m, 45.0, probe_session, cfg);
  ASSERT_EQ(reference.outcome, rk::EigenOutcome::Adjusted);
  // Other sessions' draws in between do not move session t's adjustment.
  const auto other = rk::eigen_adjust_block(block, m, 45.0, probe_session + day_ns, cfg);
  const auto again = rk::eigen_adjust_block(block, m, 45.0, probe_session, cfg);
  EXPECT_TRUE(same_bits(std::span<const f64>(again.covariance), std::span<const f64>(reference.covariance)));
  EXPECT_TRUE(same_bits(std::span<const f64>(again.gamma), std::span<const f64>(reference.gamma)));
  // The session's own stream: another session draws differently.
  EXPECT_FALSE(same_bits(std::span<const f64>(other.covariance), std::span<const f64>(reference.covariance)));
  EXPECT_NE(rk::eigen_session_seed(7, probe_session), rk::eigen_session_seed(7, probe_session + day_ns));
  // 1 and 4 concurrent workers give the same bytes.
  std::vector<rk::EigenBlock> results(4);
  {
    std::vector<std::thread> workers;
    for (usize w = 0; w < results.size(); ++w)
      workers.emplace_back([&, w] { results[w] = rk::eigen_adjust_block(block, m, 45.0, probe_session, cfg); });
    for (auto& w : workers) w.join();
  }
  for (const auto& r : results) {
    EXPECT_TRUE(same_bits(std::span<const f64>(r.covariance), std::span<const f64>(reference.covariance)));
    EXPECT_TRUE(same_bits(std::span<const f64>(r.gamma), std::span<const f64>(reference.gamma)));
  }
}

// Per session: F_t, the structural flags, the factor returns and the VRA audit.
struct DayLog final : rk::RiskSink {
  std::vector<std::vector<f64>> cov, factor_return;
  std::vector<std::vector<u8>> structural;
  std::vector<f64> lambda2, bias;
  std::vector<rk::EigenOutcome> eigen;
  std::vector<usize> structural_factors;
  std::vector<u8> forecast;
  co::Status on_day(const rk::RiskDay& d) override {
    cov.emplace_back(d.covariance.begin(), d.covariance.end());
    factor_return.emplace_back(d.factor_return.begin(), d.factor_return.end());
    structural.emplace_back(d.factor_structural.begin(), d.factor_structural.end());
    lambda2.push_back(d.lambda2_factor);
    bias.push_back(d.bias_factor);
    eigen.push_back(d.eigen);
    structural_factors.push_back(d.structural_factors);
    forecast.push_back(d.forecast ? u8{1} : u8{0});
    return co::Ok();
  }
};
DayLog run_days(const CovPanel& p, const rk::RiskModelConfig& cfg) {
  DayLog log;
  std::array<rk::RiskSink*, 1> extra{&log};
  (void)run_blocks(p, cfg, 0.0, extra);
  return log;
}
// The forecast factors' block (finite diagonal) is positive definite.
bool forecast_block_pd(const std::vector<f64>& cov) {
  constexpr usize kf = rk::factor_count;
  std::vector<usize> keep;
  for (usize k = 0; k < kf; ++k)
    if (std::isfinite(cov[k * kf + k])) keep.push_back(k);
  if (keep.empty()) return true;
  Eigen::MatrixXd f(static_cast<Eigen::Index>(keep.size()), static_cast<Eigen::Index>(keep.size()));
  for (usize a = 0; a < keep.size(); ++a)
    for (usize b = 0; b < keep.size(); ++b) {
      const f64 c = cov[keep[a] * kf + keep[b]];
      if (!std::isfinite(c)) return false;
      f(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) = c;
    }
  const Eigen::LLT<Eigen::MatrixXd> llt(f);
  return llt.info() == Eigen::Success;
}

TEST(CovEigen, StructuralBorderAndPdScalingApplied) {
  // FF49 industry 6 is born 12 sessions before the end (16 names): fewer than min_factor_history
  // returns, so it is bordered structurally onto the adjusted block.
  CovPanel born = base_panel();
  const usize from = planted_dates - 12;
  for (usize t = from; t < born.d; ++t)
    for (usize i = 3; i < born.n; i += 10)
      if (born.present[t * born.n + i]) born.industry[t * born.n + i] = 6;
  const usize k = rk::industry_factor(5);
  const auto v11 = run_days(born, small_config(rk::RiskRecipe::RiskV11));
  const auto cov = run_days(born, small_config(rk::RiskRecipe::CovV1));
  const usize last = planted_dates - 1;
  ASSERT_EQ(cov.eigen[last], rk::EigenOutcome::Adjusted);
  ASSERT_GT(cov.structural_factors[last], 0U);
  ASSERT_EQ(cov.structural[last][k], 1U);
  const auto& f = cov.cov[last];
  EXPECT_GT(f[k * rk::factor_count + k], 0.0);
  EXPECT_TRUE(forecast_block_pd(f));
  // The border reads the adjusted block: the structural variance (class prior) and the fully
  // observed block differ from atx-risk-v1.1's on the same panel.
  EXPECT_NE(f[k * rk::factor_count + k], v11.cov[last][k * rk::factor_count + k]);
  EXPECT_NE(f[0], v11.cov[last][0]);
  // Every session with a structural factor stays positive definite after the PD scaling.
  for (usize t = 0; t < planted_dates; ++t)
    if (cov.structural_factors[t]) EXPECT_TRUE(forecast_block_pd(cov.cov[t])) << t;
}

TEST(CovEigen, VraReadsTheAdjustedPrior) {
  constexpr usize kf = rk::factor_count;
  const auto v11 = run_days(base_panel(), small_config(rk::RiskRecipe::RiskV11));
  const auto cov = run_days(base_panel(), small_config(rk::RiskRecipe::CovV1));
  usize checked = 0, differing = 0;
  for (const DayLog* log : {&v11, &cov})
    for (usize t = 1; t < planted_dates; ++t) {
      // B_t^2 = mean_k f_kt^2 / prior_k over the fully observed factors of t-1 (pre-VRA prior =
      // F_{t-1} / lambda^2_{t-1}, the adjusted block under atx-cov-v1).
      f64 sum = 0;
      usize count = 0;
      for (usize k = 0; k < kf; ++k) {
        const f64 c = log->cov[t - 1][k * kf + k], f = log->factor_return[t][k];
        if (!std::isfinite(c) || log->structural[t - 1][k] || !std::isfinite(f)) continue;
        const f64 prior = c / log->lambda2[t - 1];
        if (!(prior > 0)) continue;
        sum += f * f / prior;
        ++count;
      }
      if (!count) continue;
      const f64 expect = std::sqrt(sum / static_cast<f64>(count));
      EXPECT_NEAR(log->bias[t], expect, 1e-10 * expect) << t;
      ++checked;
    }
  for (usize t = 1; t < planted_dates; ++t)
    if (std::isfinite(cov.bias[t]) && cov.bias[t] != v11.bias[t]) ++differing;
  EXPECT_GT(checked, 200U);
  EXPECT_GT(differing, 50U); // the factor returns are equal; only the (adjusted) prior differs
}

TEST(CovEigen, ResultIsPositiveDefinite) {
  const auto cov = run_days(base_panel(), small_config(rk::RiskRecipe::CovV1));
  usize adjusted = 0;
  for (usize t = 0; t < planted_dates; ++t) {
    EXPECT_TRUE(forecast_block_pd(cov.cov[t])) << t;
    adjusted += cov.eigen[t] == rk::EigenOutcome::Adjusted ? 1U : 0U;
  }
  EXPECT_GT(adjusted, 100U);
  for (u64 seed = 1; seed <= 6; ++seed) {
    constexpr usize m = 12;
    const auto e = rk::eigen_adjust_block(planted_block(m, seed), m, 40.0, probe_session, small_config(rk::RiskRecipe::CovV1));
    ASSERT_EQ(e.outcome, rk::EigenOutcome::Adjusted);
    const Eigen::LLT<Eigen::MatrixXd> llt(as_matrix(e.covariance, m));
    EXPECT_EQ(llt.info(), Eigen::Success) << seed;
  }
}

// Planted truth Sigma (K 15), T = 40 draws; the minimum-variance book of the sample covariance and
// of its S2b adjustment, true risk w' Sigma w averaged over 24 fixed seeds.
TEST(CovEigen, LowersTrueRiskOfTheEstimatedMinVariancePortfolio) {
  constexpr usize m = 15, draws = 40, trials = 24;
  const auto cfg = small_config(rk::RiskRecipe::CovV1);
  const auto truth_values = planted_block(m, 101);
  const Eigen::MatrixXd sigma = as_matrix(truth_values, m);
  const Eigen::LLT<Eigen::MatrixXd> root(sigma);
  ASSERT_EQ(root.info(), Eigen::Success);
  const Eigen::MatrixXd l = root.matrixL();
  const auto true_risk = [&](const Eigen::MatrixXd& f) {
    const Eigen::VectorXd a = f.llt().solve(Eigen::VectorXd::Ones(static_cast<Eigen::Index>(m)));
    const Eigen::VectorXd w = a / a.sum();
    return w.dot(sigma * w);
  };
  f64 raw = 0, adjusted = 0;
  for (usize trial = 0; trial < trials; ++trial) {
    Normal rng{1000 + trial};
    Eigen::MatrixXd sample = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(m));
    for (usize s = 0; s < draws; ++s) {
      Eigen::VectorXd z(static_cast<Eigen::Index>(m));
      for (Eigen::Index a = 0; a < z.size(); ++a) z[a] = rng();
      const Eigen::VectorXd x = l * z;
      sample += x * x.transpose() / static_cast<f64>(draws);
    }
    sample = 0.5 * (sample + sample.transpose()).eval();
    std::vector<f64> block(m * m);
    for (usize a = 0; a < m; ++a)
      for (usize b = 0; b < m; ++b)
        block[a * m + b] = sample(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b));
    const auto e = rk::eigen_adjust_block(block, m, static_cast<f64>(draws),
                                          probe_session + static_cast<i64>(trial) * day_ns, cfg);
    ASSERT_EQ(e.outcome, rk::EigenOutcome::Adjusted) << trial;
    raw += true_risk(sample) / static_cast<f64>(trials);
    adjusted += true_risk(as_matrix(e.covariance, m)) / static_cast<f64>(trials);
  }
  EXPECT_LT(adjusted, raw);
}

// ---- CovBias (extended families) ---------------------------------------------------------------
constexpr usize random_books = 4;
// Checks every extended book formed at each session (after the harness's on_day).
struct Formed final : rk::RiskSink {
  const rk::BiasHarness* harness{};
  usize sessions_with_minvar{}, complete_violations{}, sum_violations{}, excluded_incomplete{};
  std::vector<std::vector<std::pair<usize, f64>>> minvar; // per session
  co::Status on_day(const rk::RiskDay& d) override {
    constexpr usize kf = rk::factor_count;
    const auto complete = [&](usize i) {
      const u8 slot = d.industry_slot[i];
      if (!d.eligible[i] || slot >= rk::industry_slots || !(d.specific_variance[i] > 0) ||
          !std::isfinite(d.specific_variance[i]))
        return false;
      if (!std::isfinite(d.covariance[0]) ||
          !std::isfinite(d.covariance[rk::industry_factor(slot) * kf + rk::industry_factor(slot)]))
        return false;
      for (usize s = 0; s < rk::style_count; ++s)
        if (d.styles[i * rk::style_count + s] != 0.0 &&
            !std::isfinite(d.covariance[rk::style_factor(s) * kf + rk::style_factor(s)]))
          return false;
      return true;
    };
    const auto mv = harness->formed(random_books);
    minvar.emplace_back(mv.begin(), mv.end());
    std::vector<u8> held(d.eligible.size(), 0);
    for (const auto& entry : mv) held[entry.first] = 1;
    if (d.forecast)
      for (usize i = 0; i < d.eligible.size(); ++i)
        if (d.eligible[i] && d.industry_slot[i] != rk::no_exposure && !complete(i) && !held[i])
          ++excluded_incomplete;
    if (mv.empty()) return co::Ok();
    ++sessions_with_minvar;
    f64 sum = 0;
    for (const auto& [i, w] : mv) { complete_violations += complete(i) ? 0U : 1U; sum += w; }
    sum_violations += std::abs(sum - 1.0) < 1e-9 ? 0U : 1U;
    for (usize q = 0; q < rk::optimized_portfolios; ++q) {
      const auto w = harness->formed(random_books + 1 + q);
      f64 net = 0, gross = 0;
      for (const auto& [i, x] : w) { complete_violations += complete(i) ? 0U : 1U; net += x; gross += std::abs(x); }
      if (!w.empty()) sum_violations += (std::abs(net) < 1e-9 && std::abs(gross - 1.0) < 1e-9) ? 0U : 1U;
    }
    return co::Ok();
  }
};
const rk::BiasSeries* find_series(const rk::BiasHarness& h, const std::string& name) {
  for (const auto& s : h.series())
    if (s.name == name) return &s;
  return nullptr;
}

// The eigen family on the planted panel under both recipes: z = realised / forecast vol on every
// observation, the pooled b finite and bounded, and S2b (atx-cov-v1) moves it toward 1 -- the
// eigenportfolios of a short-history sample covariance under-forecast (b > 1 under atx-risk-v1.1),
// which is what the simulated adjustment corrects. (The planted industries obey the cap
// constraint, so the block has near-singular directions: the band is loose, not USE4's.)
f64 pooled_eigen_b(const rk::BiasHarness& h, std::vector<f64>& pooled, usize& series) {
  for (const auto& s : h.series()) {
    if (s.family != "eigen") continue;
    ++series;
    pooled.insert(pooled.end(), s.z.begin(), s.z.end());
    for (usize j = 0; j < s.z.size(); ++j)
      EXPECT_EQ(std::bit_cast<u64>(s.z[j]), std::bit_cast<u64>(s.realized[j] / s.forecast_vol[j]));
  }
  return rk::bias_statistic(pooled);
}
TEST(CovBias, EigenFamilyInBandOnAPlantedModel) {
  const auto panel = base_panel().panel();
  rk::BiasHarness harness(panel, random_books, 7, {}, rk::BiasFamilies::Extended);
  rk::BiasHarness unadjusted(panel, random_books, 7, {}, rk::BiasFamilies::Extended);
  std::array<rk::RiskSink*, 1> extra{&harness}, extra_v11{&unadjusted};
  (void)run_blocks(base_panel(), small_config(rk::RiskRecipe::CovV1), 0.0, extra);
  (void)run_blocks(base_panel(), small_config(rk::RiskRecipe::RiskV11), 0.0, extra_v11);
  std::vector<f64> pooled, pooled_v11;
  usize series = 0, series_v11 = 0;
  const f64 b = pooled_eigen_b(harness, pooled, series);
  const f64 b_v11 = pooled_eigen_b(unadjusted, pooled_v11, series_v11);
  EXPECT_EQ(series, rk::factor_count);
  ASSERT_GT(pooled.size(), 500U);
  EXPECT_GT(b, 0.5);
  EXPECT_LT(b, 3.0);
  EXPECT_GT(b_v11, 1.0);
  EXPECT_LT(b, b_v11) << "S2b should move the eigen family's b toward 1";
  EXPECT_TRUE(std::isfinite(rk::qlike(pooled)));
  EXPECT_TRUE(std::isfinite(rk::mrad(pooled, 63)));
  // QLIKE and MRAD as declared.
  const std::vector<f64> z{1.0, -2.0, 0.0, missing, 0.5};
  const f64 q = ((1.0 - 0.0) + (4.0 - std::log(4.0)) + (0.25 - std::log(0.25))) / 3.0;
  EXPECT_DOUBLE_EQ(rk::qlike(z), q);
  EXPECT_TRUE(std::isnan(rk::qlike(std::vector<f64>{0.0, missing})));
  f64 dev = 0;
  usize windows = 0;
  for (const f64 x : rk::rolling_bias(pooled, 63))
    if (std::isfinite(x)) { dev += std::abs(x - 1.0); ++windows; }
  EXPECT_DOUBLE_EQ(rk::mrad(pooled, 63), dev / static_cast<f64>(windows));
}

// Without structural factor forecasts a style short of history is unforecast while names load on
// it (beta, residual_vol, momentum when they appear): those names are left out of the books.
TEST(CovBias, MinVarAndOptimizedUseCompleteForecastsOnly) {
  const auto panel = base_panel().panel();
  rk::BiasHarness harness(panel, random_books, 7, {}, rk::BiasFamilies::Extended);
  Formed formed;
  formed.harness = &harness;
  std::array<rk::RiskSink*, 2> extra{&harness, &formed};
  auto cfg = small_config(rk::RiskRecipe::CovV1);
  cfg.structural_factor_forecast = false;
  (void)run_blocks(base_panel(), cfg, 0.0, extra);
  EXPECT_GT(formed.sessions_with_minvar, 60U);
  EXPECT_EQ(formed.complete_violations, 0U);
  EXPECT_EQ(formed.sum_violations, 0U);
  EXPECT_GT(formed.excluded_incomplete, 0U); // names without a complete forecast were left out
  const auto* minvar = find_series(harness, "minvar");
  ASSERT_NE(minvar, nullptr);
  EXPECT_GT(minvar->z.size(), 80U);
  EXPECT_EQ(minvar->dropped_factor_exposures + minvar->uncovered_name_returns, 0U);
  usize optimized = 0;
  for (const auto& s : harness.series())
    if (s.family == "optimized") {
      ++optimized;
      EXPECT_EQ(s.dropped_factor_exposures + s.uncovered_name_returns, 0U) << s.name;
    }
  EXPECT_EQ(optimized, rk::optimized_portfolios);
}

TEST(CovBias, MissingReturnCountsZeroNeverRenormalised) {
  // Name 10 is absent for one session t0: the minvar book formed at t0-1 holds it, its return
  // over (t0-1, t0] is missing and counts 0; the realised return is not rescaled.
  CovPanel gap = base_panel();
  constexpr usize t0 = 120, name = 10;
  const usize k = t0 * gap.n + name;
  gap.present[k] = 0; gap.close[k] = missing; gap.raw[k] = missing; gap.volume[k] = missing;
  gap.cap[k] = missing;
  const auto panel = gap.panel();
  rk::BiasHarness harness(panel, random_books, 7, {}, rk::BiasFamilies::Extended);
  Formed formed;
  formed.harness = &harness;
  std::array<rk::RiskSink*, 2> extra{&harness, &formed};
  (void)run_blocks(gap, small_config(rk::RiskRecipe::CovV1), 0.0, extra);
  const auto& held = formed.minvar[t0 - 1];
  ASSERT_FALSE(held.empty());
  f64 expect = 0, held_weight = 0;
  for (const auto& [i, w] : held) {
    const f64 x = rk::interval_return(panel, t0, i);
    if (std::isfinite(x)) expect += w * x;
    if (i == name) held_weight = w;
  }
  ASSERT_NE(held_weight, 0.0);
  const auto* minvar = find_series(harness, "minvar");
  ASSERT_NE(minvar, nullptr);
  const auto at = std::find(minvar->sessions.begin(), minvar->sessions.end(), gap.sessions[t0]);
  ASSERT_NE(at, minvar->sessions.end());
  const auto j = static_cast<usize>(at - minvar->sessions.begin());
  EXPECT_EQ(std::bit_cast<u64>(minvar->realized[j]), std::bit_cast<u64>(expect));
  EXPECT_EQ(std::bit_cast<u64>(minvar->z[j]), std::bit_cast<u64>(expect / minvar->forecast_vol[j]));
  EXPECT_GE(minvar->missing_returns, 1U);
}

TEST(CovBias, V1FamiliesBytesUnchanged) {
  const auto panel = base_panel().panel();
  rk::BiasHarness v1(panel, random_books, 7, {});
  rk::BiasHarness extended(panel, random_books, 7, {}, rk::BiasFamilies::Extended);
  std::array<rk::RiskSink*, 2> extra{&v1, &extended};
  (void)run_blocks(base_panel(), small_config(rk::RiskRecipe::CovV1), 0.0, extra);
  ASSERT_EQ(v1.series().size(), random_books + rk::factor_count);
  ASSERT_EQ(extended.series().size(), random_books + 2 * rk::factor_count + 1 + rk::optimized_portfolios);
  for (usize j = 0; j < v1.series().size(); ++j) {
    const auto& a = v1.series()[j];
    const auto& b = extended.series()[j];
    EXPECT_EQ(a.family, b.family);
    EXPECT_EQ(a.name, b.name);
    EXPECT_EQ(a.sessions, b.sessions) << a.name;
    EXPECT_TRUE(same_bits(std::span<const f64>(a.forecast_vol), std::span<const f64>(b.forecast_vol))) << a.name;
    EXPECT_TRUE(same_bits(std::span<const f64>(a.realized), std::span<const f64>(b.realized))) << a.name;
    EXPECT_TRUE(same_bits(std::span<const f64>(a.z), std::span<const f64>(b.z))) << a.name;
    EXPECT_EQ(a.missing_returns, b.missing_returns);
    EXPECT_EQ(a.warmup_excluded, b.warmup_excluded);
    EXPECT_EQ(a.dropped_factor_exposures, b.dropped_factor_exposures);
    EXPECT_EQ(a.uncovered_name_returns, b.uncovered_name_returns);
  }
}
} // namespace
