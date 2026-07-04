// risk_model_config_test.cpp — p8 S1-0: RiskModelConfig plumbing +
// FactorModelArtifact round-trip.
//
// S1-0 lands (a) RiskModelConfig — the inert-default config every S1 unit reads
// (kind==Diagonal, dead_alpha_factors=false, group_neutralize=false — all no-ops
// until later units wire a reader) and (b) a serialize/deserialize round-trip +
// content digest on FactorModelArtifact (the on-disk seam between the new
// stage_riskmodel producer and the stage_optimize/combine consumers).
//
// Suite: RiskModelConfigSpine
//
// Tests:
//   * DefaultsAreInert               — RiskModelConfig{} matches the documented
//       inert defaults exactly (kind=Diagonal, both bools false, lookback=252,
//       all style toggles true).
//   * DiagonalIsFrozenAtZero          — static_assert + runtime check pinning
//       RiskModelKind::Diagonal == 0 (the frozen enum index the determinism
//       contract depends on: Diagonal must always be the default-constructed
//       zero value, even if Factor/appended kinds are added later).
//   * ArtifactRoundTripByteIdentical  — a hand-built FactorModelArtifact (M=4,
//       K=2) serializes to bytes and deserializes back to X/F/D/fit_begin/
//       fit_end bit-identical (every f64 cell memcmp-equal).
//   * ArtifactDigestStableTwiceRun    — hashing the same artifact's serialized
//       bytes twice yields the same u64 digest (determinism: no clock/RNG/
//       map-iteration in the digest path).
//   * ArtifactDigestChangesOnMutation — perturbing one X cell changes the digest
//       (the digest is actually sensitive to content, not a constant stub).

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/data/adapt_factor.hpp"
#include "atx/engine/data/factor_model_artifact.hpp"
#include "atx/engine/risk/factor_model.hpp"

namespace atxtest_risk_model_config_test {

using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::data::FactorModelArtifact;
using atx::engine::risk::RiskModelConfig;
using atx::engine::risk::RiskModelKind;

// ===========================================================================
//  RiskModelConfig — inert defaults.
// ===========================================================================

TEST(RiskModelConfigSpine, DefaultsAreInert) {
  const RiskModelConfig cfg;
  EXPECT_EQ(cfg.kind, RiskModelKind::Diagonal);
  EXPECT_FALSE(cfg.dead_alpha_factors);
  EXPECT_FALSE(cfg.group_neutralize);
  EXPECT_EQ(cfg.fit_lookback_days, 252U);
  EXPECT_TRUE(cfg.style_size);
  EXPECT_TRUE(cfg.style_vol);
  EXPECT_TRUE(cfg.style_mom);
  EXPECT_TRUE(cfg.style_beta);
  EXPECT_TRUE(cfg.industry);
}

TEST(RiskModelConfigSpine, DiagonalIsFrozenAtZero) {
  static_assert(static_cast<atx::u8>(RiskModelKind::Diagonal) == 0U,
                "RiskModelKind::Diagonal must stay the frozen zero index — the "
                "determinism contract routes the default-constructed kind to the "
                "byte-identical diagonal path");
  EXPECT_EQ(static_cast<atx::u8>(RiskModelKind::Diagonal), 0U);
  // A default-constructed RiskModelConfig's kind must equal the zero enumerator
  // (guards against a future reorder of the enum breaking the aggregate default).
  EXPECT_EQ(static_cast<atx::u8>(RiskModelConfig{}.kind), 0U);
}

// ===========================================================================
//  FactorModelArtifact — round-trip + digest.
// ===========================================================================

namespace {

[[nodiscard]] FactorModelArtifact make_artifact() {
  MatX x(4, 2);
  // clang-format off
  x << 1.0,  0.3,
       0.2,  0.8,
      -0.5,  0.1,
       0.7, -0.4;
  // clang-format on
  MatX f(2, 2);
  f << 0.04, 0.01,
       0.01, 0.09;
  VecX d(4);
  d << 0.02, 0.03, 0.015, 0.025;
  return FactorModelArtifact{std::move(x), std::move(f), std::move(d),
                             /*fit_begin=*/0U, /*fit_end=*/60U};
}

} // namespace

TEST(RiskModelConfigSpine, ArtifactRoundTripByteIdentical) {
  const FactorModelArtifact original = make_artifact();

  const std::vector<std::uint8_t> bytes = atx::engine::data::serialize_artifact(original);
  const auto restored_r = atx::engine::data::deserialize_artifact(bytes);
  ASSERT_TRUE(restored_r.has_value()) << restored_r.error().message();
  const FactorModelArtifact &restored = *restored_r;

  ASSERT_EQ(restored.X.rows(), original.X.rows());
  ASSERT_EQ(restored.X.cols(), original.X.cols());
  for (Eigen::Index r = 0; r < original.X.rows(); ++r) {
    for (Eigen::Index c = 0; c < original.X.cols(); ++c) {
      EXPECT_EQ(restored.X(r, c), original.X(r, c)) << "X(" << r << "," << c << ") mismatch";
    }
  }
  ASSERT_EQ(restored.F.rows(), original.F.rows());
  ASSERT_EQ(restored.F.cols(), original.F.cols());
  for (Eigen::Index r = 0; r < original.F.rows(); ++r) {
    for (Eigen::Index c = 0; c < original.F.cols(); ++c) {
      EXPECT_EQ(restored.F(r, c), original.F(r, c)) << "F(" << r << "," << c << ") mismatch";
    }
  }
  ASSERT_EQ(restored.D.size(), original.D.size());
  for (Eigen::Index i = 0; i < original.D.size(); ++i) {
    EXPECT_EQ(restored.D[i], original.D[i]) << "D(" << i << ") mismatch";
  }
  EXPECT_EQ(restored.fit_begin, original.fit_begin);
  EXPECT_EQ(restored.fit_end, original.fit_end);

  // The round-tripped artifact must still lower cleanly through the existing
  // S6.6 seam (byte-identity guarantee extends through artifact_to_factor_model).
  const auto model_r = atx::engine::data::artifact_to_factor_model(restored);
  ASSERT_TRUE(model_r.has_value()) << model_r.error().message();
}

TEST(RiskModelConfigSpine, ArtifactDigestStableTwiceRun) {
  const FactorModelArtifact a = make_artifact();
  const auto bytes1 = atx::engine::data::serialize_artifact(a);
  const auto bytes2 = atx::engine::data::serialize_artifact(a);
  const atx::u64 d1 = atx::engine::data::digest_artifact(a);
  const atx::u64 d2 = atx::engine::data::digest_artifact(a);
  EXPECT_EQ(bytes1, bytes2);
  EXPECT_EQ(d1, d2);
  EXPECT_NE(d1, 0U);
}

TEST(RiskModelConfigSpine, ArtifactDigestChangesOnMutation) {
  FactorModelArtifact a = make_artifact();
  const atx::u64 d_before = atx::engine::data::digest_artifact(a);
  a.X(0, 0) += 1e-6; // tiny perturbation
  const atx::u64 d_after = atx::engine::data::digest_artifact(a);
  EXPECT_NE(d_before, d_after);
}

} // namespace atxtest_risk_model_config_test
