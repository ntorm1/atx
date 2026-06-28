// weight_policy_decay_test.cpp — p7-S4-1: the opt-in stateful EmaDecayPolicy.
//
// EmaDecayPolicy wraps the stateless WeightPolicy with a per-name EMA over the
// PREVIOUS call's smoothed output:
//
//   smoothed[i] = alpha * raw_target[i] + (1 - alpha) * prev_smoothed[i]
//
// then (when dollar_neutral) a final demean and a gross-renormalize so Σ|w| holds
// at gross_leverage. alpha == 1.0 is the INERT pass-through identity (byte-
// identical to base.to_target_weights). The first call after construction / reset()
// is the cold start (prev_smoothed == raw_target), also identity on that call.
//
// Suite: WeightPolicyDecay
//   (a) AlphaOneByteIdenticalToStateless — inert default over 10 rounds.
//   (b) DecayReducesTurnover            — alpha=0.3 cuts measured churn > 5 pp.
//   (c) TwiceRunStable                  — same series twice -> identical output.
//   (d) ResetRestoresColdStart          — reset() -> period-0 == original cold start.

#include <cmath>   // std::fabs
#include <cstring> // std::memcmp (bitwise identity)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/loop/signal_source.hpp" // SignalView
#include "atx/engine/loop/types.hpp"         // InstrumentId (Symbol), Universe
#include "atx/engine/loop/weight_policy.hpp" // WeightPolicy, EmaDecayPolicy

namespace atxtest_weight_policy_decay_test {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::domain::Symbol;
using atx::engine::EmaDecayPolicy;
using atx::engine::InstrumentId;
using atx::engine::SignalView;
using atx::engine::Universe;
using atx::engine::WeightPolicy;

// A fixed n-name universe (synthetic contiguous ids; to_target_weights uses only
// the size for cross-section alignment and never dereferences an id).
[[nodiscard]] std::vector<InstrumentId> make_universe(usize n) {
  std::vector<InstrumentId> ids(n);
  for (usize i = 0; i < n; ++i) {
    ids[i] = Symbol{static_cast<u32>(i + 1U)};
  }
  return ids;
}

// Mean over periods of 0.5 * Σ_i |w_t[i] - w_{t-1}[i]| (the L1 half-turnover the
// plan's class-(b) criterion measures). The first period has no prior, so it does
// not contribute; the divisor is the number of transition periods.
[[nodiscard]] f64 mean_turnover(const std::vector<std::vector<f64>> &series) {
  if (series.size() < 2U) {
    return 0.0;
  }
  f64 total = 0.0;
  for (usize t = 1U; t < series.size(); ++t) {
    f64 l1 = 0.0;
    for (usize i = 0; i < series[t].size(); ++i) {
      l1 += std::fabs(series[t][i] - series[t - 1U][i]);
    }
    total += 0.5 * l1;
  }
  return total / static_cast<f64>(series.size() - 1U);
}

// ===========================================================================
//  (a) alpha == 1.0 is the inert pass-through: byte-identical to the stateless
//      base over many rounds of a hand-built 8-name signal series. Bitwise.
// ===========================================================================
TEST(WeightPolicyDecay, AlphaOneByteIdenticalToStateless) {
  const usize n = 8U;
  const std::vector<InstrumentId> ids = make_universe(n);
  const Universe universe{ids};

  WeightPolicy base{}; // canonical defaults (Rank, dollar_neutral, gross 1.0)
  EmaDecayPolicy decay{base, /*ema_alpha=*/1.0};

  // 10 rounds; each round is a distinct 8-name signal (rotate magnitudes so the
  // cross-section changes period to period).
  for (usize round = 0; round < 10U; ++round) {
    std::vector<f64> sig(n);
    for (usize i = 0; i < n; ++i) {
      sig[i] = static_cast<f64>((i + round) % n) - 3.5; // mixed sign, rotating ranks
    }
    const SignalView row{std::span<const f64>{sig}};
    const std::vector<f64> w_base = base.to_target_weights(row, universe);
    const std::vector<f64> w_decay = decay.to_target_weights(row, universe);

    ASSERT_EQ(w_base.size(), w_decay.size());
    // Bitwise identity (determinism §3) — not just EXPECT_DOUBLE_EQ.
    EXPECT_EQ(std::memcmp(w_base.data(), w_decay.data(), w_base.size() * sizeof(f64)), 0)
        << "alpha=1.0 must be byte-identical to the stateless path at round " << round;
  }
}

// ===========================================================================
//  (b) decay reduces measured turnover. A 20-period, 6-name series where ranks
//      swap every other period (maximum raw turnover). alpha=0.3 must cut the
//      L1 half-turnover by > 5 percentage points vs alpha=1.0.
// ===========================================================================
TEST(WeightPolicyDecay, DecayReducesTurnover) {
  const usize n = 6U;
  const usize periods = 20U;
  const std::vector<InstrumentId> ids = make_universe(n);
  const Universe universe{ids};

  WeightPolicy base{};
  EmaDecayPolicy no_decay{base, /*ema_alpha=*/1.0};
  EmaDecayPolicy with_decay{base, /*ema_alpha=*/0.3};

  std::vector<std::vector<f64>> w_no_decay;
  std::vector<std::vector<f64>> w_with_decay;
  w_no_decay.reserve(periods);
  w_with_decay.reserve(periods);

  for (usize t = 0; t < periods; ++t) {
    std::vector<f64> sig(n);
    // Ranks flip every other period: even periods ascending, odd periods
    // descending -> the raw target swaps the long and short legs each step.
    for (usize i = 0; i < n; ++i) {
      const f64 asc = static_cast<f64>(i);
      sig[i] = (t % 2U == 0U) ? asc : (static_cast<f64>(n - 1U) - asc);
    }
    const SignalView row{std::span<const f64>{sig}};
    w_no_decay.push_back(no_decay.to_target_weights(row, universe));
    w_with_decay.push_back(with_decay.to_target_weights(row, universe));
  }

  const f64 to_no_decay = mean_turnover(w_no_decay);
  const f64 to_with_decay = mean_turnover(w_with_decay);

  EXPECT_GT(to_no_decay, 0.0) << "the fixture must produce real churn without decay";
  EXPECT_LT(to_with_decay, to_no_decay - 0.05)
      << "decay (alpha=0.3) must cut turnover by > 5 pp: no_decay=" << to_no_decay
      << " with_decay=" << to_with_decay;
}

// ===========================================================================
//  (c) twice-run: feed the same 20-period series to two fresh policies; assert
//      bit-identical output vectors on every period (no hidden state leak).
// ===========================================================================
TEST(WeightPolicyDecay, TwiceRunStable) {
  const usize n = 6U;
  const usize periods = 20U;
  const std::vector<InstrumentId> ids = make_universe(n);
  const Universe universe{ids};

  WeightPolicy base{};
  EmaDecayPolicy run_a{base, /*ema_alpha=*/0.4};
  EmaDecayPolicy run_b{base, /*ema_alpha=*/0.4};

  for (usize t = 0; t < periods; ++t) {
    std::vector<f64> sig(n);
    for (usize i = 0; i < n; ++i) {
      sig[i] = static_cast<f64>((i * 7U + t * 3U) % n) - 2.5; // pseudo-rotating
    }
    const SignalView row{std::span<const f64>{sig}};
    const std::vector<f64> a = run_a.to_target_weights(row, universe);
    const std::vector<f64> b = run_b.to_target_weights(row, universe);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size() * sizeof(f64)), 0)
        << "twice-run divergence at period " << t;
  }
}

// ===========================================================================
//  (d) reset() restores the cold start. After running 10 periods, reset() then
//      re-run period 0; the output must equal the first-call cold-start output.
// ===========================================================================
TEST(WeightPolicyDecay, ResetRestoresColdStart) {
  const usize n = 6U;
  const std::vector<InstrumentId> ids = make_universe(n);
  const Universe universe{ids};

  WeightPolicy base{};
  EmaDecayPolicy decay{base, /*ema_alpha=*/0.5};

  // Period 0 signal — capture its cold-start output BEFORE any state accrues.
  std::vector<f64> sig0(n);
  for (usize i = 0; i < n; ++i) {
    sig0[i] = static_cast<f64>(i) - 2.5;
  }
  const SignalView row0{std::span<const f64>{sig0}};
  const std::vector<f64> cold_start = decay.to_target_weights(row0, universe);

  // Run 10 more periods to accrue decay state.
  for (usize t = 1; t < 11U; ++t) {
    std::vector<f64> sig(n);
    for (usize i = 0; i < n; ++i) {
      sig[i] = static_cast<f64>((i + t) % n) - 2.5;
    }
    const SignalView row{std::span<const f64>{sig}};
    (void)decay.to_target_weights(row, universe);
  }

  // reset() -> the next call must reproduce the original cold-start output.
  decay.reset();
  const std::vector<f64> after_reset = decay.to_target_weights(row0, universe);

  ASSERT_EQ(cold_start.size(), after_reset.size());
  EXPECT_EQ(std::memcmp(cold_start.data(), after_reset.data(), cold_start.size() * sizeof(f64)), 0)
      << "reset() must restore the cold-start (prev_smoothed == raw) path";
}

} // namespace atxtest_weight_policy_decay_test
