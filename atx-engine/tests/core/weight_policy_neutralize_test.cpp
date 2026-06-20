// atx::engine::loop — WeightPolicy neutralization-stage tests (P4-8).
//
// Covers the two stages P4-8 wires into WeightPolicy (the orchestrator-scoped
// subset of plan §8 P4-8 / §5.5):
//   * GROUP-neutralize — the now-LIVE `industry_neutral` flag demeans the dense
//     buffer WITHIN each group (CsDemeanG / §0-H semantics), so each group sums
//     to ~0 before the global dollar-neutralize.
//   * TRUNCATE — a fixed-iteration clip-renorm caps |w_i| at `truncation` (in
//     gross-normalized units) while holding Σ|w| ≈ gross_leverage for a FEASIBLE
//     cap; an infeasibly-small cap pins every name (documented degenerate).
//
// THE CRITICAL TEST is the BIT-IDENTICAL REGRESSION GUARD: with the new stages
// OFF (industry_neutral=false, truncation=0, no group_map) the output must be
// byte-identical to an independent recomputation of the Phase-2 pipeline
// (winsorize → transform → demean → gross). The pre-existing weight_policy_test
// suite is the other half of that guard and MUST stay green UNCHANGED.
//
// Naming: Subject_Condition_ExpectedResult.

#include <gtest/gtest.h>

#include <array>
#include <cmath>  // std::fabs, std::isnan
#include <limits> // quiet_NaN
#include <span>
#include <vector>

#include "atx/core/stats/cross_section.hpp" // rank, zscore, demean, winsorize (reference path)
#include "atx/core/types.hpp"               // f64, u32, usize

#include "atx/engine/loop/signal_source.hpp" // SignalView
#include "atx/engine/loop/types.hpp"         // InstrumentId, Universe
#include "atx/engine/loop/weight_policy.hpp" // WeightPolicy, Transform

namespace atxtest_weight_policy_neutralize_test {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::engine::InstrumentId;
using atx::engine::SignalView;
using atx::engine::Transform;
using atx::engine::Universe;
using atx::engine::WeightPolicy;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 kTol = 1e-9;

[[nodiscard]] InstrumentId inst(u32 id) noexcept { return InstrumentId{id}; }

[[nodiscard]] f64 gross(std::span<const f64> w) noexcept {
  f64 s = 0.0;
  for (const f64 x : w) {
    s += std::fabs(x);
  }
  return s;
}

[[nodiscard]] f64 max_abs(std::span<const f64> w) noexcept {
  f64 m = 0.0;
  for (const f64 x : w) {
    m = std::fabs(x) > m ? std::fabs(x) : m;
  }
  return m;
}

// An INDEPENDENT recomputation of the Phase-2 pipeline (winsorize → transform →
// demean(if dollar_neutral) → gross-normalize), over the dense live buffer, with
// NONE of the P4-8 stages. The bit-identical guard asserts the policy's
// stages-off output equals this verbatim, so the new defaulted param / dormant
// branches change nothing. Mirrors WeightPolicy's own private path deliberately —
// any divergence is a regression in the shipped code, not in this oracle.
[[nodiscard]] std::vector<f64> reference_phase2(std::span<const f64> sig, Transform transform,
                                                bool dollar_neutral, f64 gross_leverage,
                                                f64 winsorize_limit) {
  const usize n = sig.size();
  std::vector<f64> weights(n, 0.0);
  std::vector<usize> live_idx;
  std::vector<f64> dense;
  for (usize i = 0; i < n; ++i) {
    if (!std::isnan(sig[i])) {
      live_idx.push_back(i);
      dense.push_back(sig[i]);
    }
  }
  if (dense.empty()) {
    return weights;
  }
  atx::core::stats::winsorize(std::span<f64>{dense}, winsorize_limit, 1.0 - winsorize_limit);
  // Raw is the identity passthrough: leave the winsorized scores untouched (mirrors
  // WeightPolicy::apply_transform, which returns before allocating the scratch).
  if (transform != Transform::Raw) {
    std::vector<f64> out(dense.size());
    switch (transform) {
    case Transform::Rank:
      atx::core::stats::rank(std::span<const f64>{dense}, std::span<f64>{out});
      break;
    case Transform::ZScore:
      atx::core::stats::zscore(std::span<const f64>{dense}, std::span<f64>{out});
      break;
    case Transform::Raw:
      break; // unreachable (handled above); keeps the switch exhaustive under /W4
    }
    dense.swap(out);
  }
  if (dollar_neutral) {
    atx::core::stats::demean(std::span<f64>{dense});
  }
  f64 l1 = 0.0;
  for (const f64 x : dense) {
    l1 += std::fabs(x);
  }
  if (l1 != 0.0) {
    const f64 scale = gross_leverage / l1;
    for (f64 &x : dense) {
      x *= scale;
    }
  }
  for (usize k = 0; k < live_idx.size(); ++k) {
    weights[live_idx[k]] = dense[k];
  }
  return weights;
}

// Sum the weights belonging to `group`.
[[nodiscard]] f64 group_sum(std::span<const f64> w, std::span<const u32> groups,
                            u32 group) noexcept {
  f64 s = 0.0;
  for (usize i = 0; i < w.size(); ++i) {
    if (groups[i] == group) {
      s += w[i];
    }
  }
  return s;
}

// ===========================================================================
//  GROUP-neutralize — industry_neutral now LIVE (CsDemeanG semantics)
// ===========================================================================

TEST(WeightPolicyNeutralize, GroupDemean_EachGroupSumsToZero) {
  // 6 names in 2 groups (0,0,0 / 1,1,1). After group-neutralize each group's
  // weights must sum to ~0 (per-group demean), independent of the cross-section.
  const std::array<InstrumentId, 6> u{inst(1), inst(2), inst(3), inst(4), inst(5), inst(6)};
  const std::array<f64, 6> sig{1.0, 5.0, 2.0, 9.0, 4.0, 7.0};
  const std::array<u32, 6> groups{0, 0, 0, 1, 1, 1};

  WeightPolicy policy{};
  policy.industry_neutral = true;
  const auto w =
      policy.to_target_weights(SignalView{sig}, Universe{u}, std::span<const u32>{groups});

  EXPECT_NEAR(group_sum(w, std::span<const u32>{groups}, 0U), 0.0, kTol);
  EXPECT_NEAR(group_sum(w, std::span<const u32>{groups}, 1U), 0.0, kTol);
}

TEST(WeightPolicyNeutralize, GroupDemean_UnevenGroupsEachSumZero) {
  // Uneven groups (4 + 2) to prove the per-group mean, not a global one, is
  // subtracted: a global demean would NOT zero each group individually.
  const std::array<InstrumentId, 6> u{inst(1), inst(2), inst(3), inst(4), inst(5), inst(6)};
  const std::array<f64, 6> sig{1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  const std::array<u32, 6> groups{7, 7, 7, 7, 3, 3};

  WeightPolicy policy{};
  policy.industry_neutral = true;
  const auto w =
      policy.to_target_weights(SignalView{sig}, Universe{u}, std::span<const u32>{groups});

  EXPECT_NEAR(group_sum(w, std::span<const u32>{groups}, 7U), 0.0, kTol);
  EXPECT_NEAR(group_sum(w, std::span<const u32>{groups}, 3U), 0.0, kTol);
  EXPECT_NEAR(gross(w), 1.0, kTol); // still gross-normalized to leverage
}

TEST(WeightPolicyNeutralize, GroupDemean_SingleGroupEqualsGlobalDemean) {
  // Boundary: one group over all names ⇒ per-group demean == the global demean,
  // so the result equals the ordinary dollar-neutral Phase-2 path.
  const std::array<InstrumentId, 4> u{inst(1), inst(2), inst(3), inst(4)};
  const std::array<f64, 4> sig{2.0, 8.0, 4.0, 6.0};
  const std::array<u32, 4> groups{5, 5, 5, 5};

  WeightPolicy grouped{};
  grouped.industry_neutral = true;
  const auto wg =
      grouped.to_target_weights(SignalView{sig}, Universe{u}, std::span<const u32>{groups});

  const auto wref = reference_phase2(std::span<const f64>{sig}, Transform::Rank, /*dollar=*/true,
                                     /*gross=*/1.0, /*wins=*/0.025);
  ASSERT_EQ(wg.size(), wref.size());
  for (usize i = 0; i < wg.size(); ++i) {
    EXPECT_NEAR(wg[i], wref[i], kTol)
        << "group-demean over a single group must match global demean";
  }
}

TEST(WeightPolicyNeutralize, GroupDemean_ExcludesNaNFromGroupMean) {
  // A NaN name keeps zero weight and contributes to no group mean; the two live
  // names in its group still demean within the group.
  const std::array<InstrumentId, 4> u{inst(1), inst(2), inst(3), inst(4)};
  const std::array<f64, 4> sig{1.0, kNaN, 3.0, 9.0};
  const std::array<u32, 4> groups{0, 0, 0, 1};

  WeightPolicy policy{};
  policy.industry_neutral = true;
  policy.dollar_neutral = false; // isolate group-demean from a second global demean
  const auto w =
      policy.to_target_weights(SignalView{sig}, Universe{u}, std::span<const u32>{groups});

  EXPECT_EQ(w[1], 0.0); // NaN -> exactly zero, excluded from its group mean
  // Group 0 has two live names (idx 0, 2): they demean to +/- about a center.
  EXPECT_NEAR(w[0] + w[2], 0.0, kTol);
  // Group 1 is a single live name -> its per-group demean drives it to 0.
  EXPECT_NEAR(w[3], 0.0, kTol);
}

// ===========================================================================
//  TRUNCATE — fixed-iteration clip-renorm cap on |w_i|
// ===========================================================================

TEST(WeightPolicyNeutralize, Truncate_NoWeightExceedsCapAndGrossHeld) {
  // One name would otherwise hog the gross under ZScore + an outlier; a FEASIBLE
  // cap bounds every |w_i| while Σ|w| stays at gross_leverage.
  const std::array<InstrumentId, 5> u{inst(1), inst(2), inst(3), inst(4), inst(5)};
  const std::array<f64, 5> sig{-50.0, 1.0, 2.0, 3.0, 4.0};

  WeightPolicy policy{};
  policy.transform = Transform::ZScore;
  policy.winsorize_limit = 0.0; // let the outlier through so truncation has work
  policy.truncation = 0.30;     // feasible: 0.30 * 5 = 1.5 >= gross 1.0
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  EXPECT_LE(max_abs(w), policy.truncation + kTol);
  EXPECT_NEAR(gross(w), 1.0, kTol);
}

TEST(WeightPolicyNeutralize, Truncate_OutlierPinnedAtCap) {
  // With a tight-but-feasible cap the outlier is pinned exactly at the cap.
  const std::array<InstrumentId, 5> u{inst(1), inst(2), inst(3), inst(4), inst(5)};
  const std::array<f64, 5> sig{-50.0, 1.0, 2.0, 3.0, 4.0};

  WeightPolicy policy{};
  policy.transform = Transform::ZScore;
  policy.winsorize_limit = 0.0;
  policy.truncation = 0.25; // feasible (0.25*5=1.25>=1) and below the outlier's raw weight
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  EXPECT_NEAR(std::fabs(w[0]), policy.truncation, kTol); // outlier pinned at the cap
  EXPECT_LE(max_abs(w), policy.truncation + kTol);
  EXPECT_NEAR(gross(w), 1.0, kTol);
}

TEST(WeightPolicyNeutralize, Truncate_InfeasibleCapPinsEveryNameBelowGross) {
  // Documented degenerate: an infeasibly-small cap (truncation * n_active < gross)
  // pins EVERY name at the cap, so Σ|w| < gross_leverage (the cap wins — both
  // constraints cannot hold). 5 names, cap 0.10 -> max Σ|w| = 0.5 < 1.0.
  const std::array<InstrumentId, 5> u{inst(1), inst(2), inst(3), inst(4), inst(5)};
  const std::array<f64, 5> sig{1.0, 2.0, 3.0, 4.0, 5.0};

  WeightPolicy policy{};
  policy.truncation = 0.10; // infeasible: 0.10 * 5 = 0.5 < gross 1.0
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});

  EXPECT_LE(max_abs(w), policy.truncation + kTol);
  EXPECT_LT(gross(w), 1.0); // cannot reach leverage; the cap dominates
}

TEST(WeightPolicyNeutralize, Truncate_DisabledByDefault) {
  // truncation == 0 must leave the result untouched (== Phase-2 path).
  const std::array<InstrumentId, 5> u{inst(1), inst(2), inst(3), inst(4), inst(5)};
  const std::array<f64, 5> sig{-50.0, 1.0, 2.0, 3.0, 4.0};

  WeightPolicy policy{};
  policy.transform = Transform::ZScore;
  policy.winsorize_limit = 0.0;
  // truncation left at its 0.0 default.
  const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});
  const auto wref = reference_phase2(std::span<const f64>{sig}, Transform::ZScore, /*dollar=*/true,
                                     /*gross=*/1.0, /*wins=*/0.0);
  ASSERT_EQ(w.size(), wref.size());
  for (usize i = 0; i < w.size(); ++i) {
    EXPECT_EQ(w[i], wref[i]); // byte-identical: the truncate branch never runs
  }
}

// ===========================================================================
//  BIT-IDENTICAL REGRESSION GUARD (the critical one)
// ===========================================================================

TEST(WeightPolicyNeutralize, StagesOff_ByteIdenticalToPhase2Pipeline) {
  // For several signals + configs (Rank/ZScore, dollar_neutral on/off, various
  // gross_leverage), to_target_weights with the new stages OFF (no group_map,
  // industry_neutral=false, truncation=0) must produce output BYTE-IDENTICAL to
  // an independent recomputation of the Phase-2 winsorize→transform→demean→gross
  // pipeline. This is the contract: the P4-8 additions are inert by construction.
  const std::array<InstrumentId, 6> u{inst(1), inst(2), inst(3), inst(4), inst(5), inst(6)};
  const std::array<std::array<f64, 6>, 3> signals{{
      {1.0, 2.0, 3.0, 4.0, 5.0, 6.0},
      {-3.0, 10.0, 0.5, -7.0, 2.0, 8.0},
      {5.0, kNaN, 2.0, 9.0, kNaN, 1.0}, // with NaN holes
  }};
  const std::array<Transform, 2> transforms{Transform::Rank, Transform::ZScore};
  const std::array<bool, 2> dollar_flags{true, false};
  const std::array<f64, 3> leverages{1.0, 2.0, 0.5};

  for (const auto &sig : signals) {
    for (const Transform t : transforms) {
      for (const bool dn : dollar_flags) {
        for (const f64 lev : leverages) {
          WeightPolicy policy{};
          policy.transform = t;
          policy.dollar_neutral = dn;
          policy.gross_leverage = lev;
          // industry_neutral=false, truncation=0.0 (defaults): stages off.
          const auto w = policy.to_target_weights(SignalView{sig}, Universe{u});
          const auto wref = reference_phase2(std::span<const f64>{sig}, t, dn, lev, 0.025);
          ASSERT_EQ(w.size(), wref.size());
          for (usize i = 0; i < w.size(); ++i) {
            EXPECT_EQ(w[i], wref[i]) << "stages-off must be byte-identical to Phase-2";
          }
        }
      }
    }
  }
}


}  // namespace atxtest_weight_policy_neutralize_test
