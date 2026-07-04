// weight_policy_recenter_test.cpp — S4-2 [CORRECTNESS, B2]: truncate_renorm
// must not break dollar-neutrality. WeightPolicy::to_target_weights demeans,
// gross-normalizes, then truncate_renorms (the P4-8 per-name cap); the header
// itself documents "No gross_normalize follows" the truncation -- but nothing
// re-centers either. finalize_truncation's deficit pass scales ONLY the
// sub-cap (unbinding) names to hit gross_leverage, holding binding names
// exactly at +/-truncation. When the cap binds an unequal mass of longs vs
// shorts, the binding mass alone is generally != 0, so the returned book has
// Sigma(w) != 0 even though it entered perfectly centered -- a spurious net
// exposure on a book the caller declared dollar-neutral (a fake market-beta
// return).
//
// Same by-construction fixture as risk/optimizer_recenter_test.cpp (the S4-2
// mirror fix): with Transform::Raw + winsorize_limit=0, raw scores pass
// through demean+gross_normalize UNCHANGED when they are already zero-mean
// and unit-L1 -- so feeding [0.40, 0.10, -0.35, -0.15] (sum==0, sum|.|==1.0)
// drives `dense` into truncate_renorm EXACTLY as constructed. cap=0.30 pins
// 0.40 and -0.35; 0.10 and -0.15 stay unbound. The CURRENT (buggy) code
// leaves net != 0 (hand-verified via the risk/ mirror's Python
// transliteration: -0.08 on a single truncate_renorm call). The FIX
// (alternating demean + truncate_renorm, kRecenterIters passes) drives net to
// float noise while max|w|<=truncation and Sigma|w|==gross_leverage hold.

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/loop/signal_source.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxtest_weight_policy_recenter_test {

using atx::f64;
using atx::usize;
using atx::engine::InstrumentId;
using atx::engine::SignalView;
using atx::engine::Transform;
using atx::engine::Universe;
using atx::engine::WeightPolicy;

[[nodiscard]] InstrumentId inst(atx::u32 id) noexcept { return InstrumentId{id}; }

// ===========================================================================
//  weight_policy_neutral_after_truncate: the asymmetric-clip fixture must
//  return Sigma w ~ 0 through to_target_weights (dollar_neutral, a binding
//  truncation), while max|w|<=truncation and Sigma|w|==gross_leverage hold.
//  RED (pre-fix): net is meaningfully nonzero (fails at 1e-9).
// ===========================================================================
TEST(WeightPolicyRecenter, NeutralAfterTruncate) {
  const std::array<InstrumentId, 4> u{inst(1), inst(2), inst(3), inst(4)};
  const std::array<f64, 4> sig{0.40, 0.10, -0.35, -0.15}; // sum==0, sum|.|==1.0 already

  WeightPolicy policy{};
  policy.transform = Transform::Raw;
  policy.winsorize_limit = 0.0; // no-op winsorize -> Raw passthrough (see header)
  policy.dollar_neutral = true;
  policy.gross_leverage = 1.0;
  policy.truncation = 0.30; // binds names 0 (0.40) and 2 (-0.35)

  const std::vector<f64> w = policy.to_target_weights(SignalView{sig}, Universe{u});
  ASSERT_EQ(w.size(), 4U);

  f64 sum = 0.0, gross = 0.0, max_abs = 0.0;
  for (const f64 x : w) {
    sum += x;
    gross += std::fabs(x);
    max_abs = std::max(max_abs, std::fabs(x));
  }
  EXPECT_NEAR(sum, 0.0, 1e-9)
      << "dollar-neutral book must have Sigma w ~ 0 after truncate+re-center; got " << sum;
  EXPECT_LE(max_abs, 0.30 + 1e-9) << "the per-name truncation cap must still hold";
  EXPECT_NEAR(gross, 1.0, 1e-6) << "the gross leverage budget must still hold";
}

// ===========================================================================
//  clip_renorm_non_neutral_unchanged: with dollar_neutral=false the re-center
//  is a documented no-op -- the cap and gross invariants still hold, but
//  neutrality is NOT enforced (unlike the dollar_neutral=true case above).
// ===========================================================================
TEST(WeightPolicyRecenter, ClipRenormNonNeutralUnchanged) {
  const std::array<InstrumentId, 4> u{inst(1), inst(2), inst(3), inst(4)};
  const std::array<f64, 4> sig{0.40, 0.10, -0.35, -0.15};

  WeightPolicy policy{};
  policy.transform = Transform::Raw;
  policy.winsorize_limit = 0.0;
  policy.dollar_neutral = false; // neutrality NOT requested -- recenter must no-op
  policy.gross_leverage = 1.0;
  policy.truncation = 0.30;

  const std::vector<f64> w = policy.to_target_weights(SignalView{sig}, Universe{u});
  ASSERT_EQ(w.size(), 4U);
  f64 max_abs = 0.0;
  for (const f64 x : w) {
    max_abs = std::max(max_abs, std::fabs(x));
  }
  EXPECT_LE(max_abs, 0.30 + 1e-9) << "the per-name truncation cap must still hold";

  // Twice-run byte-identity on the non-neutral (recenter-inert) path.
  const std::vector<f64> w2 = policy.to_target_weights(SignalView{sig}, Universe{u});
  ASSERT_EQ(w2.size(), w.size());
  for (usize i = 0; i < w.size(); ++i) {
    EXPECT_EQ(w[i], w2[i]) << "non-neutral path must be bit-reproducible at i=" << i;
  }
}

} // namespace atxtest_weight_policy_recenter_test
