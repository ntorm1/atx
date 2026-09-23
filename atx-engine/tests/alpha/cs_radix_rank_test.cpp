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

// ---- row kernels: bit-identical to the stable_sort reference -------------

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
    cs_rank_row(x, valid, got, scratch);
    for (atx::usize i = 0; i < n; ++i) {
      ASSERT_TRUE(same_bits(got[i], want[i])) << "n=" << n << " i=" << i;
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
  cs_quantile_row(x, valid, static_cast<atx::f64>(nb), got, scratch);
  for (atx::usize i = 0; i < n; ++i) {
    ASSERT_TRUE(same_bits(got[i], want[i])) << "i=" << i;
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
  for (int k = 0; k <= 6; ++k) {
    std::vector<atx::usize> members;
    for (const atx::usize i : valid) {
      if (g[i] == static_cast<atx::f64>(k)) {
        members.push_back(i);
      }
    }
    ref_rank(x, members, want);
  }
  std::vector<atx::f64> got(n, kNaN);
  cs_group_row(x, g, valid, got, /*zscore=*/false, scratch);
  for (atx::usize i = 0; i < n; ++i) {
    ASSERT_TRUE(same_bits(got[i], want[i])) << "i=" << i;
  }
}

} // namespace atx_test_l1_kernels_cs_radix
