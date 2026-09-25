// atx::engine::alpha — LSD radix argsort for the cross-sectional rank family
// (Lane 1 / cs_radix.hpp).
//
// The contract under test: `cs_stable_argsort` is a drop-in replacement for
//   std::stable_sort(ids, [&](a, b) { return x[a] < x[b]; })
// on a NaN-free id set — the SAME permutation, including the tie-break by input
// order and the -0.0 == +0.0 tie. The rank-family kernels (cs_rank_row,
// cs_quantile_row, cs_group_row) switch to it above a size threshold, so the
// row outputs must stay bit-identical to the stable_sort reference.
//
// Naming: Subject_Condition_ExpectedResult.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/cs_ops.hpp"
#include "atx/engine/alpha/cs_radix.hpp"

namespace atx_test_l1_kernels_cs_radix {

using atx::engine::alpha::detail::cs_group_row;
using atx::engine::alpha::detail::cs_quantile_row;
using atx::engine::alpha::detail::cs_radix_argsort;
using atx::engine::alpha::detail::cs_radix_key;
using atx::engine::alpha::detail::cs_rank_row;
using atx::engine::alpha::detail::cs_stable_argsort;
using atx::engine::alpha::detail::CsRadixScratch;
using atx::engine::alpha::detail::CsScratch;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
constexpr atx::f64 kInf = std::numeric_limits<atx::f64>::infinity();

[[nodiscard]] bool same_bits(atx::f64 a, atx::f64 b) noexcept {
  if (std::isnan(a) && std::isnan(b)) {
    return true;
  }
  atx::u64 ua = 0;
  atx::u64 ub = 0;
  std::memcpy(&ua, &a, sizeof(a));
  std::memcpy(&ub, &b, sizeof(b));
  return ua == ub;
}

// A row with many exact ties, both signed zeros, both infinities and a spread of
// magnitudes (sub-normal through 1e300) — every radix byte position varies.
[[nodiscard]] std::vector<atx::f64> nasty_row(atx::usize n, atx::u32 seed) {
  std::mt19937_64 rng{seed};
  std::uniform_int_distribution<int> pick{0, 11};
  std::normal_distribution<atx::f64> nd{0.0, 1.0};
  std::vector<atx::f64> x(n);
  for (atx::usize i = 0; i < n; ++i) {
    switch (pick(rng)) {
    case 0:
      x[i] = 0.0;
      break;
    case 1:
      x[i] = -0.0;
      break;
    case 2:
      x[i] = kInf;
      break;
    case 3:
      x[i] = -kInf;
      break;
    case 4:
      x[i] = static_cast<atx::f64>(static_cast<int>(nd(rng) * 3.0)); // integer ties
      break;
    case 5:
      x[i] = nd(rng) * 1e300;
      break;
    case 6:
      x[i] = nd(rng) * 1e-310; // sub-normal
      break;
    default:
      x[i] = nd(rng);
      break;
    }
  }
  return x;
}

[[nodiscard]] std::vector<atx::usize> reference_order(const std::vector<atx::f64> &x,
                                                      std::vector<atx::usize> ids) {
  std::stable_sort(ids.begin(), ids.end(), [&](atx::usize a, atx::usize b) { return x[a] < x[b]; });
  return ids;
}

TEST(CsRadixRank_Key, SignedZerosShareOneKey) {
  EXPECT_EQ(cs_radix_key(0.0), cs_radix_key(-0.0));
  EXPECT_LT(cs_radix_key(-kInf), cs_radix_key(-1.0));
  EXPECT_LT(cs_radix_key(-1.0), cs_radix_key(-1e-310));
  EXPECT_LT(cs_radix_key(-1e-310), cs_radix_key(0.0));
  EXPECT_LT(cs_radix_key(0.0), cs_radix_key(1e-310));
  EXPECT_LT(cs_radix_key(1e-310), cs_radix_key(1.0));
  EXPECT_LT(cs_radix_key(1.0), cs_radix_key(kInf));
  EXPECT_LT(cs_radix_key(kInf), cs_radix_key(kNaN)); // NaN sorts last
}

TEST(CsRadixRank_Argsort, MatchesStableSortOnNastyRows) {
  CsRadixScratch rs;
  for (const atx::usize n : {1U, 2U, 3U, 17U, 64U, 65U, 300U, 3000U, 5001U}) {
    const std::vector<atx::f64> x = nasty_row(n, static_cast<atx::u32>(n) * 7U + 1U);
    std::vector<atx::usize> ids(n);
    for (atx::usize i = 0; i < n; ++i) {
      ids[i] = i;
    }
    const std::vector<atx::usize> want = reference_order(x, ids);
    std::vector<atx::usize> got = ids;
    cs_stable_argsort(x, std::span<atx::usize>{got}, rs);
    ASSERT_EQ(got, want) << "n=" << n;
  }
}

TEST(CsRadixRank_Argsort, PreservesNonAscendingInputOrderOnTies) {
  // The tie-break is the INPUT order of ids (stable), not the index value.
  const std::vector<atx::f64> x{1.0, 1.0, -0.0, 0.0, 2.0, 1.0};
  std::vector<atx::usize> ids{5, 3, 1, 2, 0, 4};
  const std::vector<atx::usize> want = reference_order(x, ids);
  CsRadixScratch rs;
  cs_radix_argsort(x, std::span<atx::usize>{ids}, rs); // force the radix path
  EXPECT_EQ(ids, want);
}

TEST(CsRadixRank_Argsort, RadixPathMatchesStableSortForcedAtEverySize) {
  CsRadixScratch rs;
  for (atx::usize n = 1; n < 200; n += 7) {
    const std::vector<atx::f64> x = nasty_row(n, static_cast<atx::u32>(n) + 99U);
    std::vector<atx::usize> ids(n);
    for (atx::usize i = 0; i < n; ++i) {
      ids[i] = n - 1 - i; // descending input order
    }
    const std::vector<atx::usize> want = reference_order(x, ids);
    cs_radix_argsort(x, std::span<atx::usize>{ids}, rs);
    ASSERT_EQ(ids, want) << "n=" << n;
  }
}

TEST(CsRadixRank_Argsort, NaNsGoLastInInputOrder) {
  const std::vector<atx::f64> x{kNaN, 2.0, kNaN, -1.0, 0.5};
  std::vector<atx::usize> ids{0, 1, 2, 3, 4};
  CsRadixScratch rs;
  cs_radix_argsort(x, std::span<atx::usize>{ids}, rs);
  const std::vector<atx::usize> want{3, 4, 1, 0, 2};
  EXPECT_EQ(ids, want);
}

// Distribution shapes that stress a bucketed (MSD-first) radix: a dense
// realistic price row, one huge outlier squeezing every other key into a single
// bucket, a heavily-tied quantized row, and an all-equal row.
[[nodiscard]] std::vector<atx::f64> shaped_row(atx::usize n, int shape, atx::u32 seed) {
  std::mt19937_64 rng{seed};
  std::normal_distribution<atx::f64> nd{0.0, 1.0};
  std::uniform_int_distribution<int> q{0, 4};
  std::vector<atx::f64> x(n);
  for (atx::usize i = 0; i < n; ++i) {
    switch (shape) {
    case 0: // cent-rounded prices ~100 (many near-ties, some exact ties)
      x[i] = std::round((100.0 + 5.0 * nd(rng)) * 100.0) / 100.0;
      break;
    case 1: // N(0,1) with a single 1e300 outlier
      x[i] = i == n / 2 ? 1e300 : nd(rng);
      break;
    case 2: // quantized {0..4}
      x[i] = static_cast<atx::f64>(q(rng));
      break;
    default: // all equal
      x[i] = 7.25;
      break;
    }
  }
  return x;
}

TEST(CsRadixRank_Argsort, MatchesStableSortOnSkewedAndTiedShapes) {
  CsRadixScratch rs;
  for (int shape = 0; shape < 4; ++shape) {
    for (const atx::usize n : {97U, 300U, 3000U, 20000U}) {
      const std::vector<atx::f64> x = shaped_row(n, shape, static_cast<atx::u32>(n) + 5U);
      std::vector<atx::usize> ids(n);
      for (atx::usize i = 0; i < n; ++i) {
        ids[i] = i; // ascending input order
      }
      const std::vector<atx::usize> want = reference_order(x, ids);
      cs_radix_argsort(x, std::span<atx::usize>{ids}, rs);
      ASSERT_EQ(ids, want) << "shape=" << shape << " n=" << n;
    }
  }
}

TEST(CsRadixRank_Argsort, MatchesStableSortOnShuffledInputOrder) {
  CsRadixScratch rs;
  for (int shape = 0; shape < 4; ++shape) {
    const atx::usize n = 3000;
    const std::vector<atx::f64> x = shaped_row(n, shape, 11U);
    std::vector<atx::usize> ids(n);
    for (atx::usize i = 0; i < n; ++i) {
      ids[i] = i;
    }
    std::mt19937_64 rng{static_cast<std::uint64_t>(shape) + 3U};
    std::shuffle(ids.begin(), ids.end(), rng);
    const std::vector<atx::usize> want = reference_order(x, ids);
    cs_radix_argsort(x, std::span<atx::usize>{ids}, rs);
    ASSERT_EQ(ids, want) << "shape=" << shape;
  }
}

// ---- row kernels: bit-identical to the stable_sort reference -------------
//
// W0-A0 (A-01): the row kernels now default to average-rank ties. The ordinal
// references below are the pre-W0 bodies and are checked against the kernels
// run with the legacy RankTies::OrdinalV1 policy (expectations unchanged); each
// test also checks the default Average policy against an average-rank
// reference built from the SAME stable_sort permutation.

using atx::engine::alpha::detail::RankTies;

// Reference ordinal rank (the pre-radix cs_rank_row body).
void ref_rank(const std::vector<atx::f64> &x, const std::vector<atx::usize> &valid,
              std::vector<atx::f64> &out) {
  const std::vector<atx::usize> order = reference_order(x, valid);
  const atx::usize n = order.size();
  for (atx::usize r = 0; r < n; ++r) {
    out[order[r]] =
        (n == 1) ? 0.5 : static_cast<atx::f64>(r) / static_cast<atx::f64>(n - 1);
  }
}

// Average-rank position of every sorted slot: a run of equal values (-0.0 ties
// +0.0) shares lo + (hi-lo)/2.
[[nodiscard]] std::vector<atx::f64> ref_avg_pos(const std::vector<atx::f64> &x,
                                                const std::vector<atx::usize> &order) {
  const atx::usize n = order.size();
  std::vector<atx::f64> pos(n);
  for (atx::usize lo = 0; lo < n;) {
    atx::usize hi = lo;
    while (hi + 1 < n && x[order[hi + 1]] == x[order[lo]]) {
      ++hi;
    }
    for (atx::usize r = lo; r <= hi; ++r) {
      pos[r] = static_cast<atx::f64>(lo) + static_cast<atx::f64>(hi - lo) / 2.0;
    }
    lo = hi + 1;
  }
  return pos;
}

// Reference average rank.
void ref_rank_avg(const std::vector<atx::f64> &x, const std::vector<atx::usize> &valid,
                  std::vector<atx::f64> &out) {
  const std::vector<atx::usize> order = reference_order(x, valid);
  const std::vector<atx::f64> pos = ref_avg_pos(x, order);
  const atx::usize n = order.size();
  for (atx::usize r = 0; r < n; ++r) {
    out[order[r]] = (n == 1) ? 0.5 : pos[r] / static_cast<atx::f64>(n - 1);
  }
}

[[nodiscard]] std::vector<atx::usize> valid_of(const std::vector<atx::f64> &x) {
  std::vector<atx::usize> v;
  for (atx::usize i = 0; i < x.size(); ++i) {
    if (!std::isnan(x[i])) {
      v.push_back(i);
    }
  }
  return v;
}

TEST(CsRadixRank_Row, RankRowBitIdenticalToStableSortReference) {
  CsScratch scratch;
  for (const atx::usize n : {1U, 5U, 63U, 64U, 129U, 3000U}) {
    std::vector<atx::f64> x = nasty_row(n, static_cast<atx::u32>(n) * 3U + 5U);
    for (atx::usize i = 0; i < n; i += 11) {
      x[i] = kNaN; // NaN holes excluded from the valid set
    }
    const std::vector<atx::usize> valid = valid_of(x);
    std::vector<atx::f64> want(n, kNaN);
    ref_rank(x, valid, want);
    std::vector<atx::f64> got(n, kNaN);
    cs_rank_row(x, valid, got, scratch, RankTies::OrdinalV1);
    for (atx::usize i = 0; i < n; ++i) {
      ASSERT_TRUE(same_bits(got[i], want[i])) << "n=" << n << " i=" << i;
    }
    std::vector<atx::f64> want_avg(n, kNaN);
    ref_rank_avg(x, valid, want_avg);
    std::vector<atx::f64> got_avg(n, kNaN);
    cs_rank_row(x, valid, got_avg, scratch);
    for (atx::usize i = 0; i < n; ++i) {
      ASSERT_TRUE(same_bits(got_avg[i], want_avg[i])) << "avg n=" << n << " i=" << i;
    }
  }
}

TEST(CsRadixRank_Row, QuantileRowBitIdenticalToStableSortReference) {
  CsScratch scratch;
  const atx::usize n = 2500;
  std::vector<atx::f64> x = nasty_row(n, 4242U);
  x[7] = kNaN;
  const std::vector<atx::usize> valid = valid_of(x);
  const std::vector<atx::usize> order = reference_order(x, valid);
  const atx::usize m = order.size();
  const int nb = 5;
  std::vector<atx::f64> want(n, kNaN);
  for (atx::usize r = 0; r < m; ++r) {
    const atx::f64 p = static_cast<atx::f64>(r) / static_cast<atx::f64>(m - 1);
    int b = static_cast<int>(p * static_cast<atx::f64>(nb));
    if (b >= nb) {
      b = nb - 1;
    }
    want[order[r]] = static_cast<atx::f64>(b) / static_cast<atx::f64>(nb - 1);
  }
  std::vector<atx::f64> got(n, kNaN);
  cs_quantile_row(x, valid, static_cast<atx::f64>(nb), got, scratch, RankTies::OrdinalV1);
  for (atx::usize i = 0; i < n; ++i) {
    ASSERT_TRUE(same_bits(got[i], want[i])) << "i=" << i;
  }
  // Default Average policy: the bucket of a tie run comes from its mean position.
  const std::vector<atx::f64> pos = ref_avg_pos(x, order);
  std::vector<atx::f64> want_avg(n, kNaN);
  for (atx::usize r = 0; r < m; ++r) {
    const atx::f64 p = pos[r] / static_cast<atx::f64>(m - 1);
    int b = static_cast<int>(p * static_cast<atx::f64>(nb));
    if (b >= nb) {
      b = nb - 1;
    }
    want_avg[order[r]] = static_cast<atx::f64>(b) / static_cast<atx::f64>(nb - 1);
  }
  std::vector<atx::f64> got_avg(n, kNaN);
  cs_quantile_row(x, valid, static_cast<atx::f64>(nb), got_avg, scratch);
  for (atx::usize i = 0; i < n; ++i) {
    ASSERT_TRUE(same_bits(got_avg[i], want_avg[i])) << "avg i=" << i;
  }
}

TEST(CsRadixRank_Row, GroupRankBitIdenticalToPerGroupReference) {
  CsScratch scratch;
  const atx::usize n = 4000;
  const std::vector<atx::f64> x = nasty_row(n, 777U);
  std::vector<atx::f64> g(n);
  std::mt19937 rng{5U};
  std::uniform_int_distribution<int> grp{0, 6}; // few, large groups -> radix path
  for (atx::usize i = 0; i < n; ++i) {
    g[i] = static_cast<atx::f64>(grp(rng));
  }
  g[3] = kNaN; // NaN label stays out-of-set
  const std::vector<atx::usize> valid = valid_of(x);
  std::vector<atx::f64> want(n, kNaN);
  std::vector<atx::f64> want_avg(n, kNaN);
  for (int k = 0; k <= 6; ++k) {
    std::vector<atx::usize> members;
    for (const atx::usize i : valid) {
      if (g[i] == static_cast<atx::f64>(k)) {
        members.push_back(i);
      }
    }
    ref_rank(x, members, want);
    ref_rank_avg(x, members, want_avg);
  }
  std::vector<atx::f64> got(n, kNaN);
  cs_group_row(x, g, valid, got, /*zscore=*/false, scratch, RankTies::OrdinalV1);
  for (atx::usize i = 0; i < n; ++i) {
    ASSERT_TRUE(same_bits(got[i], want[i])) << "i=" << i;
  }
  std::vector<atx::f64> got_avg(n, kNaN);
  cs_group_row(x, g, valid, got_avg, /*zscore=*/false, scratch);
  for (atx::usize i = 0; i < n; ++i) {
    ASSERT_TRUE(same_bits(got_avg[i], want_avg[i])) << "avg i=" << i;
  }
}

} // namespace atx_test_l1_kernels_cs_radix
