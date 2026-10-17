#pragma once

// atx::engine::research::eval -- numpy's default_rng(seed) for the house statistics' draws
// (platform P9 B2). The paired dSR's circular block bootstrap (nav_summ.cbb_indices) and the
// house ONC's k-means++ seeding (backtest_integrity.kmeans) draw from
// numpy.random.default_rng(seed); the engine reproduces that stream so the statistics of
// record print the values they printed under Python (root's identity run) without a draw file.
//
//   seeding     SeedSequence(seed) (pool of 4 uint32 words, hashmix / mix with the constants
//               INIT_A 0x43b0d7e5, MULT_A 0x931e8875, MIX_MULT_L 0xca01f9dd, MIX_MULT_R
//               0x4973f715), generate_state(4, uint64) (INIT_B 0x8b51f9dd, MULT_B 0x58f38ded),
//               PCG64 set_seed: state = words[0..1], increment sequence = words[2..3] (high
//               word first), srandom_r (state 0, inc = seq << 1 | 1, step, += initstate, step)
//   next_u64    PCG64 XSL-RR 128/64: step state = state * 0x2360ed051fc65da4_4385df649fccf645 +
//               inc (mod 2^128), output rotr64(hi ^ lo, state >> 122)
//   next_u32    the low half of a fresh 64-bit output, the high half buffered for the next call
//               (pcg64_next32's has_uint32)
//   integers    Generator.integers(0, n) as int64: range n - 1 <= 0xfffffffe takes Lemire's
//               method on next_u32 (buffered_bounded_lemire_uint32); n == 1 draws nothing
//   random      Generator.random(): (next_u64 >> 11) * 2^-53
//   choice      Generator.choice(len(p), p=p) (replace=True, one draw): cdf = cumsum(p) in order,
//               cdf /= cdf[-1], index = searchsorted(cdf, random(), "right")
//
// Checked against numpy 1.26.4 (lane B2: seeds 0, 1, 20260927, 20260929, 2^40 + 5 and
// 12345678901234567890; ranges 3, 7, 12, 500, 4999, 2^31, 2^32 - 1; interleaved integers /
// choice / random). The eval_tie fixture's cbb_starts.u16 and ONC pick trace are this
// stream's output (gtest ResearchEvalRng.*).

#include <span>

#include "atx/core/types.hpp"

namespace atx::engine::research::eval {

class NumpyGenerator {
public:
  // numpy.random.default_rng(seed) for a non-negative seed below 2^64.
  explicit NumpyGenerator(u64 seed) noexcept;

  [[nodiscard]] u64 next_u64() noexcept;
  [[nodiscard]] u32 next_u32() noexcept;
  // Generator.random().
  [[nodiscard]] f64 random() noexcept;
  // Generator.integers(0, n): requires 1 <= n <= 0xffffffff.
  [[nodiscard]] u64 integers(u64 n) noexcept;
  // Generator.choice(p.size(), p=p): p non-empty, non-negative, positive sum.
  [[nodiscard]] usize choice(std::span<const f64> p);

private:
  void step() noexcept;

  u64 state_hi_{};
  u64 state_lo_{};
  u64 inc_hi_{};
  u64 inc_lo_{};
  bool has_u32_{false};
  u32 buffered_u32_{};
};

} // namespace atx::engine::research::eval
