// atx::engine::alpha — liquidity / Amihud-style derived field (p7 S2-3).
//
// Suite: LiquidityFields
//
// Exercises with_liquidity_fields(base), which appends one column:
//
//   illiq = group_neutralize(zscore(-1 * adv20), sector)
//
// i.e. the cross-sectional sample z-score (ddof=1) of negated 20-day ADV, then
// demeaned within each sector (a sector-relative illiquidity rank: low ADV ->
// high illiquidity -> positive signal). `adv20` must already be present (the
// caller passes adv_windows containing 20 through with_alpha101_fields /
// with_datafields). If `sector` is absent, group_neutralize degenerates to a
// global demean (the whole universe is one group). Synthetic panels; 1e-12 tol.

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/augment.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace atxtest_liquidity_fields {

using atx::engine::alpha::FieldId;
using atx::engine::alpha::Panel;
using atx::engine::alpha::with_liquidity_fields;

namespace {

constexpr atx::f64 kTol = 1.0e-12;

[[nodiscard]] atx::usize idx(const Panel &p, std::string_view name) noexcept {
  for (atx::usize f = 0; f < p.num_fields(); ++f) {
    if (p.field_name(f) == name) {
      return f;
    }
  }
  return static_cast<atx::usize>(-1);
}

[[nodiscard]] bool unique_field(const Panel &p, std::string_view name) noexcept {
  atx::usize c = 0;
  for (atx::usize f = 0; f < p.num_fields(); ++f) {
    if (p.field_name(f) == name) {
      ++c;
    }
  }
  return c == 1;
}

[[nodiscard]] atx::f64 cell(const Panel &p, std::string_view name, atx::usize d,
                            atx::usize n) noexcept {
  const atx::usize fi = idx(p, name);
  return p.field_all(static_cast<FieldId>(fi))[d * p.instruments() + n];
}

// Reference illiq for one date: zscore(-adv20) (cross-sectional, ddof=1), then
// demean within each sector group (or globally when `sectors` is empty).
[[nodiscard]] std::vector<atx::f64> ref_illiq_row(const std::vector<atx::f64> &adv20,
                                                  const std::vector<atx::f64> &sectors,
                                                  bool has_sector) {
  const atx::usize N = adv20.size();
  std::vector<atx::f64> neg(N);
  for (atx::usize i = 0; i < N; ++i) {
    neg[i] = -1.0 * adv20[i];
  }
  // cross-sectional sample z-score over all N (all in-universe, no NaN).
  atx::f64 sum = 0.0;
  for (const atx::f64 v : neg) {
    sum += v;
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(N);
  atx::f64 ss = 0.0;
  for (const atx::f64 v : neg) {
    ss += (v - mean) * (v - mean);
  }
  const atx::f64 sd = std::sqrt(ss / static_cast<atx::f64>(N - 1));
  std::vector<atx::f64> z(N);
  for (atx::usize i = 0; i < N; ++i) {
    z[i] = (neg[i] - mean) / sd;
  }
  // group demean.
  std::vector<atx::f64> out(N);
  for (atx::usize i = 0; i < N; ++i) {
    const atx::f64 g = has_sector ? sectors[i] : 0.0;
    atx::f64 gsum = 0.0;
    atx::usize cnt = 0;
    for (atx::usize j = 0; j < N; ++j) {
      const atx::f64 gj = has_sector ? sectors[j] : 0.0;
      if (gj == g) {
        gsum += z[j];
        ++cnt;
      }
    }
    out[i] = z[i] - gsum / static_cast<atx::f64>(cnt);
  }
  return out;
}

// D x N panel carrying adv20 (and optionally sector). adv20 varies by (d,n) so
// each cross-section has non-zero variance.
[[nodiscard]] Panel make_liq_panel(atx::usize D, atx::usize N, bool with_sector,
                                   bool with_adv20 = true) {
  const atx::usize cells = D * N;
  std::vector<atx::f64> adv20(cells);
  std::vector<atx::f64> sector(cells);
  for (atx::usize d = 0; d < D; ++d) {
    for (atx::usize n = 0; n < N; ++n) {
      const atx::usize i = d * N + n;
      adv20[i] = 1.0e6 * (1.0 + static_cast<atx::f64>(n)) + 1.0e3 * static_cast<atx::f64>(d);
      sector[i] = (n < N / 2) ? 0.0 : 1.0; // 2 sectors
    }
  }
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> data;
  // a benign anchor column so the panel always has >= 1 field even without adv20.
  names.emplace_back("close");
  data.emplace_back(cells, 100.0);
  if (with_adv20) {
    names.emplace_back("adv20");
    data.push_back(adv20);
  }
  if (with_sector) {
    names.emplace_back("sector");
    data.push_back(sector);
  }
  auto r = Panel::create(D, N, std::move(names), std::move(data), {});
  return std::move(r).value();
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Appends exactly `illiq` (no duplicate); other fields preserved.
// ---------------------------------------------------------------------------
TEST(LiquidityFields, AddsIlliqColumn) {
  const Panel base = make_liq_panel(8, 4, /*with_sector=*/true);
  auto r = with_liquidity_fields(base);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  const Panel &aug = *r;

  EXPECT_EQ(aug.num_fields(), base.num_fields() + 1);
  EXPECT_NE(idx(aug, "illiq"), static_cast<atx::usize>(-1));
  EXPECT_TRUE(unique_field(aug, "illiq"));
  for (atx::usize f = 0; f < base.num_fields(); ++f) {
    EXPECT_EQ(aug.field_name(f), base.field_name(f)) << "field reorder at " << f;
  }
}

// ---------------------------------------------------------------------------
// 2. Idempotent: second call adds nothing, changes nothing.
// ---------------------------------------------------------------------------
TEST(LiquidityFields, IdempotentOnRecall) {
  const Panel base = make_liq_panel(8, 4, /*with_sector=*/true);
  auto r1 = with_liquidity_fields(base);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  auto r2 = with_liquidity_fields(*r1);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();
  const Panel &p1 = *r1;
  const Panel &p2 = *r2;

  ASSERT_EQ(p1.num_fields(), p2.num_fields());
  for (atx::usize f = 0; f < p1.num_fields(); ++f) {
    EXPECT_EQ(p1.field_name(f), p2.field_name(f));
    const std::span<const atx::f64> a = p1.field_all(static_cast<FieldId>(f));
    const std::span<const atx::f64> b = p2.field_all(static_cast<FieldId>(f));
    for (atx::usize c = 0; c < a.size(); ++c) {
      if (std::isnan(a[c])) {
        EXPECT_TRUE(std::isnan(b[c]));
      } else {
        EXPECT_EQ(a[c], b[c]);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 3. illiq == sector-relative z-score of -adv20.
// ---------------------------------------------------------------------------
TEST(LiquidityFields, IlliqCorrectValues) {
  const atx::usize D = 3;
  const atx::usize N = 4; // 2 sectors x 2 instruments
  const Panel base = make_liq_panel(D, N, /*with_sector=*/true);
  auto r = with_liquidity_fields(base);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  const Panel &aug = *r;

  for (atx::usize d = 0; d < D; ++d) {
    std::vector<atx::f64> adv(N);
    std::vector<atx::f64> sec(N);
    for (atx::usize n = 0; n < N; ++n) {
      adv[n] = cell(base, "adv20", d, n);
      sec[n] = cell(base, "sector", d, n);
    }
    const std::vector<atx::f64> expected = ref_illiq_row(adv, sec, /*has_sector=*/true);
    for (atx::usize n = 0; n < N; ++n) {
      EXPECT_NEAR(cell(aug, "illiq", d, n), expected[n], kTol) << "illiq[" << d << "," << n << "]";
    }
  }
}

// ---------------------------------------------------------------------------
// 4. Missing adv20 -> Err(NotFound); no crash.
// ---------------------------------------------------------------------------
TEST(LiquidityFields, MissingAdv20ReturnsError) {
  const Panel base = make_liq_panel(5, 4, /*with_sector=*/true, /*with_adv20=*/false);
  ASSERT_EQ(idx(base, "adv20"), static_cast<atx::usize>(-1));
  auto r = with_liquidity_fields(base);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// 5. No sector -> global z-score (one group); no error.
// ---------------------------------------------------------------------------
TEST(LiquidityFields, NoSectorFallsBackToGlobal) {
  const atx::usize D = 3;
  const atx::usize N = 5;
  const Panel base = make_liq_panel(D, N, /*with_sector=*/false);
  ASSERT_EQ(idx(base, "sector"), static_cast<atx::usize>(-1));
  auto r = with_liquidity_fields(base);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  const Panel &aug = *r;

  for (atx::usize d = 0; d < D; ++d) {
    std::vector<atx::f64> adv(N);
    for (atx::usize n = 0; n < N; ++n) {
      adv[n] = cell(base, "adv20", d, n);
    }
    // With one global group, demean(z) == z - mean(z) == z (z already mean-0).
    const std::vector<atx::f64> expected = ref_illiq_row(adv, {}, /*has_sector=*/false);
    for (atx::usize n = 0; n < N; ++n) {
      EXPECT_NEAR(cell(aug, "illiq", d, n), expected[n], kTol) << "illiq[" << d << "," << n << "]";
    }
  }
}

} // namespace atxtest_liquidity_fields
