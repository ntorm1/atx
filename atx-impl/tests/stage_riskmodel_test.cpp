// stage_riskmodel_test.cpp — p8 S1-1: build_risk_model producer.
//
// Suite: AtxImplRiskModel
//
// Tests (per sprint-1-risk-model-covariance.md S1-1 Accept):
//   * DiagonalEquivalentToDiagonalRiskModel — cfg.kind==Diagonal -> the
//       artifact's (X, F, D) equals diagonal_risk_model's output exactly
//       (drop-in; byte-identical apply()).
//   * FactorNondegenerateDelevers            — a fixture with two correlated
//       instrument groups: the built F is non-diagonal (K>1) and risk(w) for a
//       long-one-group/short-other portfolio is strictly LOWER under the
//       factor model than under the diagonal model (the factor model sees the
//       hedge; the diagonal model does not).
//   * PitGuardIgnoresFutureRows              — perturbing panel rows at/after
//       fit_end does not change the built artifact (no look-ahead).
//   * TwiceRunByteIdenticalArtifact           — same panel+cfg -> identical
//       serialized artifact bytes across two independent calls.

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/adapt_factor.hpp"
#include "atx/engine/data/factor_model_artifact.hpp"
#include "atx/engine/risk/factor_model.hpp"

#include "diag_risk.hpp"
#include "stage_riskmodel.hpp"

namespace atxtest_stage_riskmodel {

namespace alpha = atx::engine::alpha;
namespace data = atx::engine::data;
namespace risk = atx::engine::risk;

using atx::f64;
using atx::usize;

// ---------------------------------------------------------------------------
// Fixture builders.
// ---------------------------------------------------------------------------

// A gently-trending panel (M instruments, D dates), same shape family as
// optimize_test.cpp's make_research_panel — every instrument gets a distinct
// drift so per-name variances differ (keeps the Diagonal-equivalence check
// non-degenerate).
static atx::core::Result<alpha::Panel> make_trend_panel(usize M, usize D) {
  std::vector<f64> close;
  close.reserve(D * M);
  std::vector<f64> volume;
  volume.reserve(D * M);
  for (usize t = 0; t < D; ++t) {
    for (usize i = 0; i < M; ++i) {
      const f64 drift = 0.0002 * (1.0 + static_cast<f64>(i) * 0.1);
      close.push_back(100.0 * std::exp(drift * static_cast<f64>(t)));
      volume.push_back(1'000'000.0 + 1000.0 * static_cast<f64>(i));
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  return alpha::Panel::create(D, M, {"close", "volume"}, {close, volume}, uni);
}

// A panel where EVERY instrument (both group A = [0, M/2) and group B =
// [M/2, M)) loads on the SAME common shock (a shared "market" factor) with
// the SAME sign, plus a small per-name idiosyncratic wobble. A long-A /
// short-B portfolio therefore CANCELS the common shock exactly (both legs
// carry +1 loading on it) and is left with only the idiosyncratic residual --
// the classic Barra "hedged pair" case. The DIAGONAL model is structurally
// blind to this: X=0 means it just sums each name's OWN return variance
// (which includes the common-shock contribution) regardless of any
// offsetting position, so it prices the hedge as if it were unhedged. A
// factor model that recovers a shared exposure column (industry dummies +
// Volatility here) prices the same book with the common-factor variance
// correctly cancelled, leaving only the (much smaller) idiosyncratic sum --
// strictly LOWER risk. Deterministic (no RNG): the "shock" is a fixed
// sinusoid, not a real RNG draw.
static atx::core::Result<alpha::Panel> make_correlated_group_panel(usize M, usize D) {
  std::vector<f64> common_shock(D);
  for (usize t = 0; t < D; ++t) {
    // A dominant common shock every name shares (same sign, same magnitude).
    common_shock[t] = 0.01 * std::sin(0.31 * static_cast<f64>(t));
  }
  std::vector<f64> close(D * M, 100.0);
  std::vector<f64> volume(D * M, 2'000'000.0);
  for (usize i = 0; i < M; ++i) {
    // Tiny idiosyncratic per-name wobble, an order of magnitude smaller than
    // the common shock, breaks exact collinearity across names (deterministic
    // function of i, not RNG) -- this is the residual a hedge CANNOT cancel.
    const f64 idio_amp = 0.0005 * (1.0 + static_cast<f64>(i % 7));
    f64 level = 100.0;
    for (usize t = 0; t < D; ++t) {
      const f64 shock = common_shock[t]; // SAME for every instrument, both groups
      const f64 idio = idio_amp * std::sin(0.9 * static_cast<f64>(t) + static_cast<f64>(i));
      const f64 ret = shock + idio;
      if (t > 0) {
        level *= (1.0 + ret);
      }
      close[t * M + i] = level;
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  return alpha::Panel::create(D, M, {"close", "volume"}, {close, volume}, uni);
}

// Volatility-only style block (lookback 60, the SHALLOWEST style factor) so
// small deterministic fixtures stay tractable: Momentum/Beta both need a
// 252+-row trailing lookback PER ESTIMATION DATE (see risk/exposures.hpp's
// kMomLong/kBetaWindow), which would force multi-thousand-row panels here.
// Volatility alone is enough to prove a non-diagonal, multi-factor F -- the
// S1-1 accept criterion never requires the FULL style block, only that the
// factor estimator (not the diagonal stub) is genuinely exercised.
[[nodiscard]] risk::RiskModelConfig factor_cfg(atx::u32 lookback) {
  risk::RiskModelConfig cfg;
  cfg.kind = risk::RiskModelKind::Factor;
  cfg.fit_lookback_days = lookback;
  cfg.style_mom = false;
  cfg.style_beta = false;
  cfg.style_size = false; // Liquidity proxy: keep the fixture minimal too
  cfg.industry = false;   // no group_map supplied in these fixtures
  return cfg;
}

// ---------------------------------------------------------------------------
// Test 1: Diagonal delegate equivalence.
// ---------------------------------------------------------------------------
TEST(AtxImplRiskModel, DiagonalEquivalentToDiagonalRiskModel) {
  constexpr usize M = 6, D = 40;
  auto panel_r = make_trend_panel(M, D);
  ASSERT_TRUE(panel_r.has_value()) << panel_r.error().message();
  const alpha::Panel& panel = *panel_r;

  risk::RiskModelConfig cfg; // default -> Diagonal
  auto artifact_r = atx::impl::build_risk_model(panel, cfg);
  ASSERT_TRUE(artifact_r.has_value()) << artifact_r.error().message();
  const data::FactorModelArtifact& art = *artifact_r;

  auto expect_model_r = atx::impl::diagonal_risk_model(panel);
  ASSERT_TRUE(expect_model_r.has_value()) << expect_model_r.error().message();

  auto got_model_r = data::artifact_to_factor_model(art);
  ASSERT_TRUE(got_model_r.has_value()) << got_model_r.error().message();

  // Byte-identical apply(): same risk() for an arbitrary weight vector.
  std::vector<f64> w(M);
  for (usize i = 0; i < M; ++i) w[i] = (i % 2 == 0) ? 0.1 : -0.1;
  const f64 expect_risk = expect_model_r->risk(w);
  const f64 got_risk = got_model_r->risk(w);
  EXPECT_DOUBLE_EQ(expect_risk, got_risk);

  // Shapes must also match exactly (X=Mx1 zeros, F=[[1]]).
  EXPECT_EQ(art.X.rows(), static_cast<Eigen::Index>(M));
  EXPECT_EQ(art.X.cols(), 1);
  EXPECT_EQ(art.F.rows(), 1);
  EXPECT_EQ(art.F.cols(), 1);
  EXPECT_DOUBLE_EQ(art.F(0, 0), 1.0);
  for (Eigen::Index r = 0; r < art.X.rows(); ++r) {
    EXPECT_DOUBLE_EQ(art.X(r, 0), 0.0);
  }
}

// ---------------------------------------------------------------------------
// Test 2: Factor model de-levers a correlated pair vs the diagonal model.
// ---------------------------------------------------------------------------
TEST(AtxImplRiskModel, FactorNondegenerateDelevers) {
  constexpr usize M = 10, D = 200;
  auto panel_r = make_correlated_group_panel(M, D);
  ASSERT_TRUE(panel_r.has_value()) << panel_r.error().message();
  const alpha::Panel& panel = *panel_r;

  // Industry dummies (group A = first half, group B = second half) + the
  // Volatility style column together give K=3 (2 sector cols + 1 style col)
  // -- the smallest fixture that genuinely exercises a MULTI-factor F
  // (K>1), not just a single-column degenerate case.
  risk::RiskModelConfig cfg = factor_cfg(/*lookback=*/100U);
  cfg.industry = true;
  std::vector<atx::u32> group_map(M);
  for (usize i = 0; i < M; ++i) group_map[i] = (i < M / 2) ? 0U : 1U;

  auto factor_artifact_r = atx::impl::build_risk_model(panel, cfg, group_map);
  ASSERT_TRUE(factor_artifact_r.has_value()) << factor_artifact_r.error().message();
  const data::FactorModelArtifact& factor_art = *factor_artifact_r;

  // K must exceed 1 (non-diagonal factor structure was actually estimated).
  ASSERT_GT(factor_art.F.rows(), 1) << "expected a multi-factor F (style block), got K="
                                    << factor_art.F.rows();

  auto factor_model_r = data::artifact_to_factor_model(factor_art);
  ASSERT_TRUE(factor_model_r.has_value()) << factor_model_r.error().message();

  risk::RiskModelConfig diag_cfg; // Diagonal
  auto diag_artifact_r = atx::impl::build_risk_model(panel, diag_cfg);
  ASSERT_TRUE(diag_artifact_r.has_value()) << diag_artifact_r.error().message();
  auto diag_model_r = data::artifact_to_factor_model(*diag_artifact_r);
  ASSERT_TRUE(diag_model_r.has_value()) << diag_model_r.error().message();

  // Long the first half (group A), short the second half (group B), equal
  // weight, dollar-neutral -- the classic "hedged pair" the factor model
  // should recognize as LOW risk (both groups' shared beta cancels) while the
  // diagonal model (no cross-sectional structure) prices it as the naive sum
  // of per-name variances.
  const usize half = M / 2;
  std::vector<f64> w(M);
  for (usize i = 0; i < M; ++i) {
    w[i] = (i < half) ? (1.0 / static_cast<f64>(half)) : (-1.0 / static_cast<f64>(M - half));
  }

  const f64 factor_risk = factor_model_r->risk(w);
  const f64 diag_risk = diag_model_r->risk(w);
  EXPECT_LT(factor_risk, diag_risk)
      << "factor risk=" << factor_risk << " diagonal risk=" << diag_risk
      << " -- expected the factor model to see the shared-group hedge and price "
         "strictly lower risk than the structure-blind diagonal model";
}

// ---------------------------------------------------------------------------
// Test 3: PIT guard — perturbing rows >= fit_end must not change the artifact.
//
// S1 fix-loop rework: build_risk_model now takes an EXPLICIT fit_end
// parameter, so this no longer needs the in-test truncation crutch (the
// original version handed build_risk_model a panel PRE-TRUNCATED to
// [0, fit_end) -- exercising build_risk_model's implicit "fit_end ==
// research.dates()" contract, not the real production call pattern, where
// stage_optimize.cpp hands build_risk_model the FULL untruncated research
// panel plus an explicit fit_end). This version calls build_risk_model on the
// FULL D-row panel with fit_end passed explicitly and proves the real
// contract directly: two panels identical on [0, fit_end) but perturbed
// DIFFERENTLY on [fit_end, D) must yield byte-identical artifacts.
// ---------------------------------------------------------------------------
TEST(AtxImplRiskModel, PitGuardIgnoresFutureRows) {
  constexpr usize M = 8, D = 200;
  auto panel_r = make_correlated_group_panel(M, D);
  ASSERT_TRUE(panel_r.has_value()) << panel_r.error().message();

  // Fit over a window that ends BEFORE the panel's last date, so there are
  // "future" rows [fit_end, D) to perturb.
  const atx::u32 lookback = 100U;
  const usize fit_end = D - 10; // 10 held-out future rows

  risk::RiskModelConfig cfg = factor_cfg(lookback);

  // Panel A: the shared [0, fit_end) prefix, rows >= fit_end perturbed one way.
  std::vector<f64> close_a(panel_r->field_all(0).begin(), panel_r->field_all(0).end());
  std::vector<f64> volume_a(panel_r->field_all(1).begin(), panel_r->field_all(1).end());
  for (usize t = fit_end; t < D; ++t) {
    for (usize i = 0; i < M; ++i) {
      close_a[t * M + i] = 111.0 + static_cast<f64>(i);
      volume_a[t * M + i] = 1.0;
    }
  }
  auto panel_a_r = alpha::Panel::create(D, M, {"close", "volume"}, {close_a, volume_a},
                                        std::vector<std::uint8_t>(D * M, 1u));
  ASSERT_TRUE(panel_a_r.has_value()) << panel_a_r.error().message();

  // Panel B: BYTE-IDENTICAL to A on [0, fit_end); rows >= fit_end perturbed a
  // DIFFERENT way (a different, much wilder value).
  std::vector<f64> close_b = close_a;
  std::vector<f64> volume_b = volume_a;
  for (usize t = fit_end; t < D; ++t) {
    for (usize i = 0; i < M; ++i) {
      close_b[t * M + i] = 999999.0 * (1.0 + 0.001 * static_cast<f64>(i));
      volume_b[t * M + i] = 42.0;
    }
  }
  auto panel_b_r = alpha::Panel::create(D, M, {"close", "volume"}, {close_b, volume_b},
                                        std::vector<std::uint8_t>(D * M, 1u));
  ASSERT_TRUE(panel_b_r.has_value()) << panel_b_r.error().message();

  // Real production call pattern: the FULL untruncated D-row panel + an
  // EXPLICIT fit_end (never a pre-truncated sub-panel).
  auto artifact_a_r = atx::impl::build_risk_model(*panel_a_r, cfg, {}, nullptr, {}, 0, fit_end);
  ASSERT_TRUE(artifact_a_r.has_value()) << artifact_a_r.error().message();
  auto artifact_b_r = atx::impl::build_risk_model(*panel_b_r, cfg, {}, nullptr, {}, 0, fit_end);
  ASSERT_TRUE(artifact_b_r.has_value()) << artifact_b_r.error().message();

  const auto bytes_a = data::serialize_artifact(*artifact_a_r);
  const auto bytes_b = data::serialize_artifact(*artifact_b_r);
  EXPECT_EQ(bytes_a, bytes_b)
      << "artifact changed when only rows >= fit_end were perturbed differently -- look-ahead leak";
}

// ---------------------------------------------------------------------------
// Test 4: Twice-run determinism.
// ---------------------------------------------------------------------------
TEST(AtxImplRiskModel, TwiceRunByteIdenticalArtifact) {
  constexpr usize M = 8, D = 200;
  auto panel_r = make_correlated_group_panel(M, D);
  ASSERT_TRUE(panel_r.has_value()) << panel_r.error().message();

  const risk::RiskModelConfig cfg = factor_cfg(100U);
  auto a1 = atx::impl::build_risk_model(*panel_r, cfg);
  auto a2 = atx::impl::build_risk_model(*panel_r, cfg);
  ASSERT_TRUE(a1.has_value()) << a1.error().message();
  ASSERT_TRUE(a2.has_value()) << a2.error().message();

  EXPECT_EQ(data::serialize_artifact(*a1), data::serialize_artifact(*a2));
  EXPECT_EQ(data::digest_artifact(*a1), data::digest_artifact(*a2));
}

} // namespace atxtest_stage_riskmodel
