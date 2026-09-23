// combine_orthogonalize_test.cpp — Lane 5: residualization, Löwdin, marginal IC.
//
// Suite: CombineOrthogonalize

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/combine/orthogonalize.hpp"

namespace atx_test_l5_combine_orthogonalize {

using atx::f64;
using atx::usize;
namespace cb = atx::engine::combine;

struct Rng {
  std::uint64_t s;
  f64 uni() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<f64>(s >> 11U) + 0.5) / 9007199254740992.0;
  }
  f64 gauss() {
    const f64 u1 = uni();
    const f64 u2 = uni();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

std::vector<f64> noise(Rng &g, usize n) {
  std::vector<f64> v(n);
  for (f64 &x : v) {
    x = g.gauss();
  }
  return v;
}

cb::PanelView view(const std::vector<f64> &v, usize t, usize n) { return {v, t, n}; }

constexpr usize kT = 6;
constexpr usize kN = 40;

TEST(CombineOrthogonalize, ResidualIsWeightedOrthogonalToExposuresAndPool) {
  Rng g{1U};
  constexpr usize kK = 3;
  const std::vector<f64> b = noise(g, kT * kN * kK); // time-varying exposures
  const std::vector<f64> p1 = noise(g, kT * kN);
  std::vector<f64> s = noise(g, kT * kN);
  for (usize c = 0U; c < s.size(); ++c) {
    s[c] += 0.7 * p1[c] + 0.3 * b[c * kK];
  }
  std::vector<f64> spec(kN);
  for (usize i = 0U; i < kN; ++i) {
    spec[i] = 0.5 + 0.05 * static_cast<f64>(i);
  }
  s[7] = std::nan(""); // one missing cell
  const std::vector<cb::PanelView> pool{view(p1, kT, kN)};
  const auto r = cb::residualize_signal(view(s, kT, kN), {b, kT, kN, kK}, spec, pool);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_TRUE(std::isnan((*r)[7]));
  for (usize t = 0U; t < kT; ++t) {
    for (usize col = 0U; col <= kK; ++col) {
      f64 dot = 0.0;
      for (usize i = 0U; i < kN; ++i) {
        const f64 e = (*r)[t * kN + i];
        if (!std::isfinite(e)) {
          continue;
        }
        const f64 x = (col < kK) ? b[(t * kN + i) * kK + col] : p1[t * kN + i];
        dot += e * x / spec[i];
      }
      EXPECT_NEAR(dot, 0.0, 1e-10) << "t=" << t << " col=" << col;
    }
  }
}

TEST(CombineOrthogonalize, RankDeficientIndustryDummiesStillProject) {
  // Static exposures: market column + a full 4-industry dummy set (collinear).
  constexpr usize kK = 5;
  std::vector<f64> b(kN * kK, 0.0);
  for (usize i = 0U; i < kN; ++i) {
    b[i * kK + 0] = 1.0;
    b[i * kK + 1 + (i % 4U)] = 1.0;
  }
  Rng g{2U};
  const std::vector<f64> s = noise(g, kT * kN);
  const auto r = cb::residualize_signal(view(s, kT, kN), {b, 1U, kN, kK}, {}, {});
  ASSERT_TRUE(r.has_value());
  for (usize t = 0U; t < kT; ++t) {
    for (usize ind = 0U; ind < 4U; ++ind) {
      f64 sum = 0.0;
      for (usize i = ind; i < kN; i += 4U) {
        sum += (*r)[t * kN + i];
      }
      EXPECT_NEAR(sum, 0.0, 1e-10); // industry-demeaned
    }
  }
}

TEST(CombineOrthogonalize, LowdinIsOrthonormalAndOrderIndependent) {
  Rng g{3U};
  const std::vector<f64> a = noise(g, kT * kN);
  std::vector<f64> bb = noise(g, kT * kN);
  std::vector<f64> c = noise(g, kT * kN);
  for (usize i = 0U; i < a.size(); ++i) {
    bb[i] += 0.6 * a[i];
    c[i] += 0.3 * a[i] - 0.4 * bb[i];
  }
  c[11] = std::nan("");
  const std::vector<cb::PanelView> fwd_order{view(a, kT, kN), view(bb, kT, kN), view(c, kT, kN)};
  const std::vector<cb::PanelView> rev_order{view(c, kT, kN), view(a, kT, kN), view(bb, kT, kN)};
  const auto o1 = cb::lowdin_orthogonalize(fwd_order);
  const auto o2 = cb::lowdin_orthogonalize(rev_order);
  ASSERT_TRUE(o1.has_value());
  ASSERT_TRUE(o2.has_value());
  // Order independence: output for `a` is the same whichever position it held.
  for (usize cell = 0U; cell < kT * kN; ++cell) {
    if (cell == 11U) {
      EXPECT_TRUE(std::isnan((*o1)[0][cell]));
      continue;
    }
    EXPECT_NEAR((*o1)[0][cell], (*o2)[1][cell], 1e-12);
    EXPECT_NEAR((*o1)[1][cell], (*o2)[2][cell], 1e-12);
    EXPECT_NEAR((*o1)[2][cell], (*o2)[0][cell], 1e-12);
  }
  // Pooled Gram of outputs = I.
  for (usize x = 0U; x < 3U; ++x) {
    for (usize y = 0U; y < 3U; ++y) {
      f64 dot = 0.0;
      usize n = 0U;
      for (usize cell = 0U; cell < kT * kN; ++cell) {
        if (std::isfinite((*o1)[x][cell])) {
          dot += (*o1)[x][cell] * (*o1)[y][cell];
          ++n;
        }
      }
      EXPECT_NEAR(dot / static_cast<f64>(n), (x == y) ? 1.0 : 0.0, 1e-10);
    }
  }
}

TEST(CombineOrthogonalize, LowdinRejectsCollinear) {
  Rng g{4U};
  const std::vector<f64> a = noise(g, kT * kN);
  std::vector<f64> twice(a);
  for (f64 &x : twice) {
    x *= 2.0;
  }
  const std::vector<cb::PanelView> ps{view(a, kT, kN), view(twice, kT, kN)};
  EXPECT_FALSE(cb::lowdin_orthogonalize(ps).has_value());
}

TEST(CombineOrthogonalize, MarginalIcZeroForCopyPositiveForNewEdge) {
  constexpr usize kTT = 60;
  constexpr usize kNN = 100;
  Rng g{5U};
  const std::vector<f64> e1 = noise(g, kTT * kNN);
  const std::vector<f64> e2 = noise(g, kTT * kNN);
  std::vector<f64> fwd(kTT * kNN);
  std::vector<f64> copy(e1);
  for (usize c = 0U; c < fwd.size(); ++c) {
    fwd[c] = 0.2 * e1[c] + 0.2 * e2[c] + g.gauss();
    copy[c] = 3.0 * e1[c] + 1.0; // affine copy: fully spanned by [1, pool]
  }
  const std::vector<cb::PanelView> pool{view(e1, kTT, kNN)};
  const auto dup = cb::marginal_ic(view(copy, kTT, kNN), pool, view(fwd, kTT, kNN));
  const auto fresh = cb::marginal_ic(view(e2, kTT, kNN), pool, view(fwd, kTT, kNN));
  ASSERT_TRUE(dup.has_value());
  ASSERT_TRUE(fresh.has_value());
  EXPECT_EQ(dup->n_dates, 0U); // copy residual is identically 0 → IC undefined every date
  EXPECT_NEAR(dup->mean_ic, 0.0, 1e-12);
  EXPECT_EQ(fresh->n_dates, kTT);
  EXPECT_GT(fresh->mean_ic, 0.1);
  EXPECT_GT(fresh->tstat, 5.0);
  // Empty pool → plain IC, still positive for e1.
  const auto plain = cb::marginal_ic(view(e1, kTT, kNN), {}, view(fwd, kTT, kNN), 10U, 20U);
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->n_dates, 10U);
  EXPECT_GT(plain->mean_ic, 0.0);
}

TEST(CombineOrthogonalize, RejectsShapeMismatch) {
  const std::vector<f64> s(kT * kN, 1.0);
  const std::vector<f64> small(kN, 1.0);
  EXPECT_FALSE(cb::residualize_signal(view(s, kT, kN), {small, 2U, kN, 1U}, {}, {}).has_value());
  EXPECT_FALSE(cb::residualize_signal(view(s, kT, kN), {}, std::vector<f64>(3, 1.0), {}).has_value());
  const std::vector<cb::PanelView> bad{view(small, 1U, kN)};
  EXPECT_FALSE(cb::lowdin_orthogonalize(std::vector<cb::PanelView>{view(s, kT, kN), bad[0]})
                   .has_value());
  EXPECT_FALSE(cb::marginal_ic(view(s, kT, kN), bad, view(s, kT, kN)).has_value());
}

} // namespace atx_test_l5_combine_orthogonalize
