#include "atx/engine/research/eval/numpy_rng.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace atx::engine::research::eval {
namespace {

constexpr u64 kLow32 = 0xFFFFFFFFULL;
constexpr u32 kInitA = 0x43b0d7e5U;
constexpr u32 kMultA = 0x931e8875U;
constexpr u32 kInitB = 0x8b51f9ddU;
constexpr u32 kMultB = 0x58f38dedU;
constexpr u32 kMixL = 0xca01f9ddU;
constexpr u32 kMixR = 0x4973f715U;
constexpr u64 kMultHi = 0x2360ED051FC65DA4ULL; // PCG_DEFAULT_MULTIPLIER_128
constexpr u64 kMultLo = 0x4385DF649FCCF645ULL;
constexpr usize kPool = 4U;
// Lemire rejections: each repeat has probability < 1/2, so this bound is never reached.
constexpr u32 kMaxRejections = 1U << 20U;

struct U128 {
  u64 hi{};
  u64 lo{};
};

// Full 64 x 64 -> 128 product from 32-bit limbs (portable; no __int128).
[[nodiscard]] constexpr U128 mul_wide(u64 a, u64 b) noexcept {
  const u64 a_lo = a & kLow32;
  const u64 a_hi = a >> 32U;
  const u64 b_lo = b & kLow32;
  const u64 b_hi = b >> 32U;
  const u64 p0 = a_lo * b_lo;
  const u64 p1 = a_lo * b_hi;
  const u64 p2 = a_hi * b_lo;
  const u64 p3 = a_hi * b_hi;
  const u64 mid = (p0 >> 32U) + (p1 & kLow32) + (p2 & kLow32);
  return U128{p3 + (p1 >> 32U) + (p2 >> 32U) + (mid >> 32U), (p0 & kLow32) | (mid << 32U)};
}

// a * b mod 2^128.
[[nodiscard]] constexpr U128 mul128(U128 a, U128 b) noexcept {
  U128 r = mul_wide(a.lo, b.lo);
  r.hi += a.hi * b.lo + a.lo * b.hi;
  return r;
}

// a + b mod 2^128.
[[nodiscard]] constexpr U128 add128(U128 a, U128 b) noexcept {
  const u64 lo = a.lo + b.lo;
  const u64 carry = lo < a.lo ? 1U : 0U;
  return U128{a.hi + b.hi + carry, lo};
}

[[nodiscard]] constexpr u32 hashmix(u32 value, u32 &hash_const) noexcept {
  value ^= hash_const;
  hash_const *= kMultA;
  value *= hash_const;
  value ^= value >> 16U;
  return value;
}

[[nodiscard]] constexpr u32 mix(u32 x, u32 y) noexcept {
  u32 result = kMixL * x - kMixR * y;
  result ^= result >> 16U;
  return result;
}

// SeedSequence(seed).generate_state(4, np.uint64).
[[nodiscard]] std::array<u64, 4> seed_words(u64 seed) noexcept {
  std::array<u32, 2> entropy{};
  usize n_entropy = 0;
  if (seed == 0U) {
    n_entropy = 1U;
  }
  for (u64 rest = seed; rest > 0U; rest >>= 32U) {
    entropy[n_entropy++] = static_cast<u32>(rest & kLow32);
  }
  std::array<u32, kPool> pool{};
  u32 hash_a = kInitA;
  for (usize i = 0; i < kPool; ++i) {
    pool[i] = hashmix(i < n_entropy ? entropy[i] : 0U, hash_a);
  }
  for (usize src = 0; src < kPool; ++src) {
    for (usize dst = 0; dst < kPool; ++dst) {
      if (src != dst) {
        pool[dst] = mix(pool[dst], hashmix(pool[src], hash_a));
      }
    }
  }
  std::array<u32, 2U * kPool> words{};
  u32 hash_b = kInitB;
  for (usize i = 0; i < words.size(); ++i) {
    u32 v = pool[i % kPool];
    v ^= hash_b;
    hash_b *= kMultB;
    v *= hash_b;
    v ^= v >> 16U;
    words[i] = v;
  }
  std::array<u64, 4> out{};
  for (usize k = 0; k < out.size(); ++k) {
    out[k] = static_cast<u64>(words[2U * k]) | (static_cast<u64>(words[2U * k + 1U]) << 32U);
  }
  return out;
}

} // namespace

NumpyGenerator::NumpyGenerator(u64 seed) noexcept {
  const std::array<u64, 4> words = seed_words(seed);
  const U128 initstate{words[0], words[1]};
  const U128 initseq{words[2], words[3]};
  inc_hi_ = (initseq.hi << 1U) | (initseq.lo >> 63U);
  inc_lo_ = (initseq.lo << 1U) | 1U;
  state_hi_ = 0U;
  state_lo_ = 0U;
  step();
  const U128 seeded = add128(U128{state_hi_, state_lo_}, initstate);
  state_hi_ = seeded.hi;
  state_lo_ = seeded.lo;
  step();
}

void NumpyGenerator::step() noexcept {
  const U128 next = add128(mul128(U128{state_hi_, state_lo_}, U128{kMultHi, kMultLo}),
                           U128{inc_hi_, inc_lo_});
  state_hi_ = next.hi;
  state_lo_ = next.lo;
}

u64 NumpyGenerator::next_u64() noexcept {
  step();
  const u64 x = state_hi_ ^ state_lo_;
  const u64 rot = state_hi_ >> 58U;
  return (x >> rot) | (x << ((64U - rot) & 63U));
}

u32 NumpyGenerator::next_u32() noexcept {
  if (has_u32_) {
    has_u32_ = false;
    return buffered_u32_;
  }
  const u64 next = next_u64();
  has_u32_ = true;
  buffered_u32_ = static_cast<u32>(next >> 32U);
  return static_cast<u32>(next & kLow32);
}

f64 NumpyGenerator::random() noexcept {
  return static_cast<f64>(next_u64() >> 11U) * (1.0 / 9007199254740992.0);
}

u64 NumpyGenerator::integers(u64 n) noexcept {
  const u64 range = n - 1U;
  if (range == 0U) {
    return 0U; // numpy returns the offset without a draw
  }
  const u64 range_excl = range + 1U;
  u64 m = static_cast<u64>(next_u32()) * range_excl;
  u64 leftover = m & kLow32;
  if (leftover < range_excl) {
    const u64 threshold = (kLow32 - range) % range_excl;
    for (u32 tries = 0; leftover < threshold && tries < kMaxRejections; ++tries) {
      m = static_cast<u64>(next_u32()) * range_excl;
      leftover = m & kLow32;
    }
  }
  return m >> 32U;
}

usize NumpyGenerator::choice(std::span<const f64> p) {
  std::vector<f64> cdf(p.size());
  f64 running = 0.0;
  for (usize i = 0; i < p.size(); ++i) {
    running += p[i];
    cdf[i] = running;
  }
  const f64 last = cdf.back();
  for (f64 &value : cdf) {
    value /= last;
  }
  const f64 u = random();
  return static_cast<usize>(std::upper_bound(cdf.begin(), cdf.end(), u) - cdf.begin());
}

} // namespace atx::engine::research::eval
