// strategy_ic_theme_resid_test.cpp -- composition rule theme-resid-v1 (platform v8 R-11):
// IcThemeRule::residualise in the IC composition (strategy_ic_theme_resid.cpp). The weights
// file's theme_residualise block is covered end to end by ThemeResidRunner.* in
// strategy_ic_runner_test.cpp; the least-squares kernel by GroupResidualise.* in
// atx-engine/tests/combine/combine_group_residualise_test.cpp.
//
// Suite: ThemeResid

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include "atx/engine/parallel/det_pool.hpp"
#include "../src/strategy_ic_composition.hpp"
#include "../src/strategy_ic_theme_resid.hpp"

namespace atx_test_v8_theme_resid {

using atx::f64;
using atx::u8;
using atx::usize;
namespace st = atx::impl::strategy;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// Literal centred tied rank of values[i] among the kept entries, independent of the kernels:
// (less + (less + equal - 1)) / (2 (n - 1)) - 0.5; NaN elsewhere and when fewer than two are kept.
std::vector<f64> ref_ranks(const std::vector<f64>& values, const std::vector<bool>& keep) {
  std::vector<f64> out(values.size(), kNaN);
  usize n = 0;
  for (usize i = 0; i < values.size(); ++i) n += keep[i] ? 1U : 0U;
  if (n < 2) return out;
  for (usize i = 0; i < values.size(); ++i) {
    if (!keep[i]) continue;
    usize less = 0, equal = 0;
    for (usize j = 0; j < values.size(); ++j) {
      if (!keep[j]) continue;
      less += values[j] < values[i] ? 1U : 0U;
      equal += values[j] == values[i] ? 1U : 0U;
    }
    out[i] = (static_cast<f64>(less) + static_cast<f64>(less + equal - 1)) / (2.0 * static_cast<f64>(n - 1)) - 0.5;
  }
  return out;
}

bool same_bits(const std::vector<f64>& a, const std::vector<f64>& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](f64 x, f64 y) {
    return std::bit_cast<std::uint64_t>(x) == std::bit_cast<std::uint64_t>(y);
  });
}

f64 max_abs_difference(const std::vector<f64>& a, const std::vector<f64>& b) {
  f64 out = 0;
  for (usize k = 0; k < a.size(); ++k)
    if (std::isfinite(a[k]) && std::isfinite(b[k])) out = std::max(out, std::abs(a[k] - b[k]));
  return out;
}

// test_composition_resid.py fixture(), built identically: 3 dates x 8 names; theme a = a1, a2
// (sign -1), b = b1, b2, c = c1, registered order a, b, c. Name 7 leaves on date 2; a2 misses
// name 0 on date 0 and every name on date 2; c1 misses name 2 on date 0; theme b misses name 1
// on date 1; on date 2 b1 = b2 = a1, so theme b is theme a there.
struct ResidFixture {
  static constexpr usize days = 3, width = 8;
  std::vector<st::IcCompositionCandidate> candidates{{"a1", "a"}, {"a2", "a"}, {"b1", "b"}, {"b2", "b"}, {"c1", "c"}};
  std::vector<int> signs{1, -1, 1, 1, 1};
  std::vector<f64> weights{.2, .15, .2, .2, .25};
  std::vector<usize> positions{0, 0, 1, 1, 2};
  std::vector<u8> member = std::vector<u8>(days * width, 1);
  std::vector<std::vector<f64>> signals = std::vector<std::vector<f64>>(5, std::vector<f64>(days * width));
  st::IcCompositionConfig cfg;
  ResidFixture() {
    cfg.dates = days; cfg.instruments = width; cfg.decision_end = days;
    member[2 * width + 7] = 0;
    for (usize d = 0; d < days; ++d) for (usize i = 0; i < width; ++i) {
      const auto at = d * width + i;
      const auto fi = static_cast<f64>(i);
      signals[0][at] = static_cast<f64>((3 * i + 2 * d) % 8) + .25 * fi;
      signals[1][at] = static_cast<f64>((5 * i + 3 * d + 1) % 8);
      signals[2][at] = static_cast<f64>((7 * i + d) % 8) - .5 * static_cast<f64>(i % 3);
      signals[3][at] = static_cast<f64>((i * i + 3 * d) % 11);
      signals[4][at] = static_cast<f64>((3 * i * i + 5 * d + 2) % 13) + .125 * fi;
    }
    signals[1][0] = kNaN;
    signals[4][2] = kNaN;
    signals[2][width + 1] = kNaN;
    signals[3][width + 1] = kNaN;
    for (usize i = 0; i < width; ++i) {
      signals[1][2 * width + i] = kNaN;
      signals[2][2 * width + i] = signals[0][2 * width + i];
      signals[3][2 * width + i] = signals[0][2 * width + i];
    }
  }
  std::optional<st::IcCompositionResult> compose(const std::vector<f64>& w, std::span<const usize> themes,
                                                 st::IcThemeRule rule,
                                                 atx::engine::parallel::DetPool* pool = nullptr) const {
    auto c = st::IcComposition::create(cfg, candidates, member, w, themes, rule);
    if (!c) return std::nullopt;
    for (usize k = 0; k < candidates.size(); ++k)
      if (!c->add(k, signals[k], signs[k], pool)) return std::nullopt;
    auto out = c->finish();
    if (!out) return std::nullopt;
    return std::move(*out);
  }
  // Date d's standardised composites (ew-theme-std-v1): per theme the centred tied rank, over
  // the names with a present member, of the sum of present w s rank; a lone present name 0.
  std::vector<std::vector<f64>> composites(const std::vector<f64>& w, usize d, usize themes) const {
    std::vector<std::vector<f64>> sum(themes, std::vector<f64>(width, 0.0));
    std::vector<std::vector<bool>> present(themes, std::vector<bool>(width, false));
    for (usize k = 0; k < signals.size(); ++k) {
      if (!(w[k] > 0) || signs[k] == 0) continue;
      std::vector<f64> row(width);
      std::vector<bool> keep(width);
      for (usize i = 0; i < width; ++i) {
        row[i] = signals[k][d * width + i];
        keep[i] = member[d * width + i] != 0 && std::isfinite(row[i]);
      }
      const auto r = ref_ranks(row, keep);
      for (usize i = 0; i < width; ++i) {
        if (std::isnan(r[i])) continue;
        sum[positions[k]][i] += static_cast<f64>(signs[k]) * w[k] * r[i];
        present[positions[k]][i] = true;
      }
    }
    std::vector<std::vector<f64>> z(themes);
    for (usize t = 0; t < themes; ++t) {
      z[t] = ref_ranks(sum[t], present[t]);
      if (std::count(present[t].begin(), present[t].end(), true) == 1)
        for (usize i = 0; i < width; ++i) if (present[t][i]) z[t][i] = 0.0;
    }
    return z;
  }
};

// Two themes in closed form: theme a adds W_a z_a; theme b adds W_b times the re-rank of
// e = (y - mean y) - beta (x - mean x), beta = Sxy / Sxx, over b's present names, y = z_b,
// x = z_a (0 where a is absent). On date 2 theme b is theme a: e = 0 and b adds nothing.
TEST(ThemeResid, TwoThemesMatchTheClosedForm) {
  const ResidFixture f;
  const std::vector<f64> w{.2, .15, .2, .2, 0.0}; // c1 off: themes a, b
  const f64 w_a = 0.0 + .2 + .15, w_b = 0.0 + .2 + .2;
  const auto got = f.compose(w, f.positions, st::IcThemeRule::residualise);
  const auto plain = f.compose(w, f.positions, st::IcThemeRule::standardise);
  ASSERT_TRUE(got); ASSERT_TRUE(plain);
  constexpr usize width = ResidFixture::width;
  for (usize d = 0; d < ResidFixture::days; ++d) {
    SCOPED_TRACE(d);
    const auto z = f.composites(w, d, 2);
    std::vector<f64> want(width);
    for (usize i = 0; i < width; ++i) want[i] = f.member[d * width + i] ? 0.0 : kNaN;
    for (usize i = 0; i < width; ++i) if (!std::isnan(z[0][i])) want[i] += w_a * z[0][i];
    std::vector<usize> support;
    for (usize i = 0; i < width; ++i) if (!std::isnan(z[1][i])) support.push_back(i);
    ASSERT_GE(support.size(), 2U);
    const auto x = [&](usize i) { return std::isnan(z[0][i]) ? 0.0 : z[0][i]; };
    const auto n = static_cast<f64>(support.size());
    f64 mx = 0, my = 0;
    for (const usize i : support) { mx += x(i); my += z[1][i]; }
    mx /= n; my /= n;
    f64 sxx = 0, sxy = 0, syy = 0;
    for (const usize i : support) {
      sxx += (x(i) - mx) * (x(i) - mx); sxy += (x(i) - mx) * (z[1][i] - my); syy += (z[1][i] - my) * (z[1][i] - my);
    }
    const f64 beta = sxx > 0 ? sxy / sxx : 0.0;
    std::vector<f64> e(width, 0.0);
    std::vector<bool> keep(width, false);
    f64 ee = 0;
    for (const usize i : support) {
      e[i] = (z[1][i] - my) - beta * (x(i) - mx); keep[i] = true; ee += e[i] * e[i];
    }
    EXPECT_EQ(std::sqrt(ee) > 1e-10 * std::sqrt(syy), d != 2U); // spanned exactly on date 2
    if (std::sqrt(ee) > 1e-10 * std::sqrt(syy)) {
      const auto r = ref_ranks(e, keep);
      for (const usize i : support) want[i] += w_b * r[i];
    }
    for (usize i = 0; i < width; ++i) {
      const f64 cell = got->signal[d * width + i];
      if (std::isnan(want[i])) { EXPECT_TRUE(std::isnan(cell)) << i; continue; }
      EXPECT_NEAR(cell, want[i], 1e-14) << i;
    }
  }
  EXPECT_GT(max_abs_difference(got->signal, plain->signal), .05); // the rule changed the blend
}

// Python equals C++: composition_resid.blend (numpy lstsq) on this fixture gives these exact
// values of the rule (test_composition_resid.py EXPECTED_BLEND pins the same fractions). The
// pooled add equals the serial bits, and the registered order is part of the rule.
TEST(ThemeResid, ThreeThemesEqualTheRegisteredRule) {
  const ResidFixture f;
  const std::vector<std::vector<f64>> expected{
      {-337.0 / 840, -27.0 / 280, 9.0 / 56, 1.0 / 10, 17.0 / 70, 1.0 / 60, -13.0 / 168, 23.0 / 420},
      {-53.0 / 140, 9.0 / 70, 1.0 / 140, -1.0 / 140, 11.0 / 60, -97.0 / 420, 17.0 / 210, 13.0 / 60},
      {1.0 / 15, 3.0 / 40, -7.0 / 60, -1.0 / 15, -31.0 / 120, 1.0 / 12, 13.0 / 60, kNaN}};
  const auto rule = st::IcThemeRule::residualise;
  const auto serial = f.compose(f.weights, f.positions, rule);
  ASSERT_TRUE(serial);
  for (usize d = 0; d < ResidFixture::days; ++d) for (usize i = 0; i < ResidFixture::width; ++i) {
    const f64 cell = serial->signal[d * ResidFixture::width + i];
    if (std::isnan(expected[d][i])) { EXPECT_TRUE(std::isnan(cell)) << d << ' ' << i; continue; }
    EXPECT_NEAR(cell, expected[d][i], 1e-12) << d << ' ' << i;
  }
  for (const usize workers : {usize{2}, usize{3}}) {
    SCOPED_TRACE(workers);
    atx::engine::parallel::DetPool pool(workers);
    const auto pooled = f.compose(f.weights, f.positions, rule, &pool);
    ASSERT_TRUE(pooled);
    EXPECT_TRUE(same_bits(pooled->signal, serial->signal));
    EXPECT_TRUE(same_bits(pooled->contribution_fraction, serial->contribution_fraction));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(pooled->total_planned_turnover),
              std::bit_cast<std::uint64_t>(serial->total_planned_turnover));
  }
  const auto plain = f.compose(f.weights, f.positions, st::IcThemeRule::standardise);
  ASSERT_TRUE(plain);
  EXPECT_GT(max_abs_difference(serial->signal, plain->signal), .05);
  EXPECT_TRUE(same_bits(serial->contribution_fraction, plain->contribution_fraction)); // coverage is the planes'
  const std::vector<usize> b_first{1, 1, 0, 0, 2};
  const auto swapped = f.compose(f.weights, b_first, rule);
  ASSERT_TRUE(swapped);
  EXPECT_GT(max_abs_difference(serial->signal, swapped->signal), .05);
}

// The first theme in order adds exactly what ew-theme-std-v1 adds for it: one theme is the
// standardised blend bit for bit. Without themes the rule is ignored (the plain pinned path).
TEST(ThemeResid, FirstThemeIsTheStandardisedCompositeBitForBit) {
  const ResidFixture f;
  const std::vector<f64> a_only{.2, .15, 0.0, 0.0, 0.0};
  const auto resid = f.compose(a_only, f.positions, st::IcThemeRule::residualise);
  const auto plain = f.compose(a_only, f.positions, st::IcThemeRule::standardise);
  ASSERT_TRUE(resid); ASSERT_TRUE(plain);
  EXPECT_TRUE(same_bits(resid->signal, plain->signal));
  EXPECT_TRUE(same_bits(resid->planned_turnover, plain->planned_turnover));
  const auto unthemed = f.compose(f.weights, {}, st::IcThemeRule::residualise);
  const auto pinned = f.compose(f.weights, {}, st::IcThemeRule::redistribute);
  ASSERT_TRUE(unthemed); ASSERT_TRUE(pinned);
  EXPECT_TRUE(same_bits(unthemed->signal, pinned->signal));
}

// Flag absent: standardise and redistribute keep their envelopes; residualise adds only the
// per-name regression scratch, instruments x (8 x themes + 8) B; no themes, nothing.
TEST(ThemeResid, EnvelopeAddsOnlyTheRegressionScratch) {
  const auto base = st::ic_composition_working_bytes(3, 8, 5);
  const auto plain = st::ic_composition_working_bytes(3, 8, 5, 3, st::IcThemeRule::standardise);
  const auto spread = st::ic_composition_working_bytes(3, 8, 5, 3, st::IcThemeRule::redistribute);
  const auto resid = st::ic_composition_working_bytes(3, 8, 5, 3, st::IcThemeRule::residualise);
  const auto none = st::ic_composition_working_bytes(3, 8, 5, 0, st::IcThemeRule::residualise);
  ASSERT_TRUE(base); ASSERT_TRUE(plain); ASSERT_TRUE(spread); ASSERT_TRUE(resid); ASSERT_TRUE(none);
  EXPECT_EQ(*plain - *base, 3U * 24U * 8U);
  EXPECT_EQ(*spread - *base, 3U * 24U * 16U);
  EXPECT_EQ(*resid - *plain, 8U * (8U * 3U + 8U));
  EXPECT_EQ(*none, *base);
}

// The kernel: absent planes add nothing; mismatched shapes and a non-finite theme weight refuse.
TEST(ThemeResid, KernelRefusesMismatchedShapes) {
  std::vector<std::vector<f64>> planes(2, std::vector<f64>(8, kNaN));
  std::vector<f64> mass{.5, .5}, out(8, 0.0), one{.5};
  std::vector<std::pair<f64, usize>> row;
  ASSERT_TRUE(st::add_theme_residualised(planes, mass, 4, out, row));
  for (const f64 v : out) EXPECT_EQ(v, 0.0);
  EXPECT_FALSE(st::add_theme_residualised(planes, one, 4, out, row));  // one weight per plane
  EXPECT_FALSE(st::add_theme_residualised(planes, mass, 0, out, row)); // no names
  EXPECT_FALSE(st::add_theme_residualised(planes, mass, 3, out, row)); // 8 cells are not rows of 3
  mass[1] = kNaN;
  EXPECT_FALSE(st::add_theme_residualised(planes, mass, 4, out, row));
  mass[1] = .5;
  planes[1].resize(4);
  EXPECT_FALSE(st::add_theme_residualised(planes, mass, 4, out, row));
}

} // namespace atx_test_v8_theme_resid
