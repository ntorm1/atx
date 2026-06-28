// atx::engine::alpha — IV-surface derived fields (p7 S2-2).
//
// Suite: IvFields
//
// Exercises with_iv_fields(base), which appends three IV-surface columns derived
// from panel fields ORATS already loads but never used as signal inputs:
//
//   iv_term = zscore(atmCenI_21d / atmCenI_126d)   (cross-sectional, per date)
//   iv_vrp  = atmCenI_21d - ts_std(returns, 21)    (IV minus 21d realized vol)
//   iv_lo   = atmCenI_21d / (nEarnCnt_5d + 1.0)     (IV conditioned on earnings)
//
// `zscore` is the cross-sectional sample z-score (ddof=1, mean-0/std-1 across the
// in-universe non-NaN instruments on each date, NaN where that count < 2) —
// matching the engine's cs_zscore_row. `ts_std` is the causal trailing sample
// std over a 21-date window (full-window/any-NaN -> NaN). All assertions use
// deterministic synthetic panels and a 1e-12 tolerance.

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/augment.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace atxtest_iv_fields {

using atx::engine::alpha::FieldId;
using atx::engine::alpha::Panel;
using atx::engine::alpha::with_iv_fields;

namespace {

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
constexpr atx::f64 kTol = 1.0e-12;

// Index of a field by name, or (usize)-1 if absent.
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

// Cross-sectional sample (ddof=1) z-score of one date's values, ascending
// instrument order, NaN where the valid count < 2. Mirrors cs_zscore_row.
[[nodiscard]] atx::f64 cs_zscore_cell(const std::vector<atx::f64> &row, atx::usize target) {
  atx::f64 sum = 0.0;
  atx::usize n = 0;
  for (const atx::f64 v : row) {
    if (!std::isnan(v)) {
      sum += v;
      ++n;
    }
  }
  if (n < 2) {
    return kNaN;
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(n);
  atx::f64 ss = 0.0;
  for (const atx::f64 v : row) {
    if (!std::isnan(v)) {
      const atx::f64 dd = v - mean;
      ss += dd * dd;
    }
  }
  const atx::f64 sd = std::sqrt(ss / static_cast<atx::f64>(n - 1));
  return (row[target] - mean) / sd;
}

// A deterministic D x N panel carrying atmCenI_21d, atmCenI_126d, returns,
// nEarnCnt_5d, sector. All cells in-universe. Values vary by (date, inst) so the
// cross-section is non-degenerate.
[[nodiscard]] Panel make_iv_panel(atx::usize D, atx::usize N, bool with_nearn = true) {
  const atx::usize cells = D * N;
  std::vector<atx::f64> iv21(cells);
  std::vector<atx::f64> iv126(cells);
  std::vector<atx::f64> returns(cells);
  std::vector<atx::f64> nearn(cells);
  std::vector<atx::f64> sector(cells);
  for (atx::usize d = 0; d < D; ++d) {
    for (atx::usize n = 0; n < N; ++n) {
      const atx::usize i = d * N + n;
      // Non-trivial but smooth fields; distinct per (d,n) so no zero-variance row.
      iv21[i] = 0.05 + 0.01 * static_cast<atx::f64>(n) + 0.001 * static_cast<atx::f64>(d);
      iv126[i] = 0.07 + 0.005 * static_cast<atx::f64>(n) + 0.0005 * static_cast<atx::f64>(d);
      returns[i] = 0.001 * static_cast<atx::f64>((d + n) % 5) - 0.002;
      nearn[i] = static_cast<atx::f64>((i % 3)); // 0,1,2 repeating
      sector[i] = (n < N / 2) ? 0.0 : 1.0;
    }
  }
  std::vector<std::string> names = {"atmCenI_21d", "atmCenI_126d", "returns", "sector"};
  std::vector<std::vector<atx::f64>> data = {iv21, iv126, returns, sector};
  if (with_nearn) {
    names.emplace_back("nEarnCnt_5d");
    data.push_back(nearn);
  }
  auto r = Panel::create(D, N, std::move(names), std::move(data), {});
  return std::move(r).value();
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Appends exactly iv_term, iv_vrp, iv_lo (no duplicates).
// ---------------------------------------------------------------------------
TEST(IvFields, AddsThreeColumns) {
  const Panel base = make_iv_panel(10, 5);
  auto r = with_iv_fields(base);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  const Panel &aug = *r;

  EXPECT_EQ(aug.num_fields(), base.num_fields() + 3);
  for (std::string_view nm : {"iv_term", "iv_vrp", "iv_lo"}) {
    EXPECT_NE(idx(aug, nm), static_cast<atx::usize>(-1)) << "missing " << nm;
    EXPECT_TRUE(unique_field(aug, nm)) << "duplicate " << nm;
  }
  // Pre-existing fields preserved in order and by name.
  for (atx::usize f = 0; f < base.num_fields(); ++f) {
    EXPECT_EQ(aug.field_name(f), base.field_name(f)) << "field reorder at " << f;
  }
}

// ---------------------------------------------------------------------------
// 2. Idempotent: a second call adds nothing and changes no values.
// ---------------------------------------------------------------------------
TEST(IvFields, IdempotentOnRecall) {
  const Panel base = make_iv_panel(10, 5);
  auto r1 = with_iv_fields(base);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  auto r2 = with_iv_fields(*r1);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();
  const Panel &p1 = *r1;
  const Panel &p2 = *r2;

  ASSERT_EQ(p1.num_fields(), p2.num_fields()) << "second call changed field count";
  for (atx::usize f = 0; f < p1.num_fields(); ++f) {
    EXPECT_EQ(p1.field_name(f), p2.field_name(f));
    const std::span<const atx::f64> a = p1.field_all(static_cast<FieldId>(f));
    const std::span<const atx::f64> b = p2.field_all(static_cast<FieldId>(f));
    ASSERT_EQ(a.size(), b.size());
    for (atx::usize c = 0; c < a.size(); ++c) {
      if (std::isnan(a[c])) {
        EXPECT_TRUE(std::isnan(b[c])) << "field " << f << " cell " << c;
      } else {
        EXPECT_EQ(a[c], b[c]) << "field " << f << " cell " << c;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 3. iv_term == cross-sectional z-score of atmCenI_21d/atmCenI_126d.
// ---------------------------------------------------------------------------
TEST(IvFields, IvTermCorrectValues) {
  const atx::usize D = 4;
  const atx::usize N = 5;
  const Panel base = make_iv_panel(D, N);
  auto r = with_iv_fields(base);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  const Panel &aug = *r;

  // For every (date, inst) compare iv_term to the hand-computed z-score of the
  // ratio row.
  for (atx::usize d = 0; d < D; ++d) {
    std::vector<atx::f64> ratio(N);
    for (atx::usize n = 0; n < N; ++n) {
      ratio[n] = cell(base, "atmCenI_21d", d, n) / cell(base, "atmCenI_126d", d, n);
    }
    for (atx::usize n = 0; n < N; ++n) {
      const atx::f64 expected = cs_zscore_cell(ratio, n);
      const atx::f64 actual = cell(aug, "iv_term", d, n);
      ASSERT_FALSE(std::isnan(expected)) << "fixture should be non-degenerate";
      EXPECT_NEAR(actual, expected, kTol) << "iv_term[" << d << "," << n << "]";
    }
  }
}

// ---------------------------------------------------------------------------
// 4. iv_vrp == atmCenI_21d - ts_std(returns, 21) (causal trailing sample std).
// ---------------------------------------------------------------------------
TEST(IvFields, IvVrpCorrectValues) {
  // 25 dates so the 21-window has both incomplete-window NaNs and valid cells.
  const atx::usize D = 25;
  const atx::usize N = 3;
  const atx::usize W = 21;
  const Panel base = make_iv_panel(D, N);
  auto r = with_iv_fields(base);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  const Panel &aug = *r;

  for (atx::usize n = 0; n < N; ++n) {
    for (atx::usize d = 0; d < D; ++d) {
      atx::f64 expected;
      if (d + 1 < W) {
        expected = kNaN; // incomplete trailing window
      } else {
        // sample std (ddof=1) of returns over [d-W+1, d].
        atx::f64 sum = 0.0;
        for (atx::usize k = d + 1 - W; k <= d; ++k) {
          sum += cell(base, "returns", k, n);
        }
        const atx::f64 mean = sum / static_cast<atx::f64>(W);
        atx::f64 ss = 0.0;
        for (atx::usize k = d + 1 - W; k <= d; ++k) {
          const atx::f64 dd = cell(base, "returns", k, n) - mean;
          ss += dd * dd;
        }
        const atx::f64 sd = std::sqrt(ss / static_cast<atx::f64>(W - 1));
        expected = cell(base, "atmCenI_21d", d, n) - sd;
      }
      const atx::f64 actual = cell(aug, "iv_vrp", d, n);
      if (std::isnan(expected)) {
        EXPECT_TRUE(std::isnan(actual)) << "iv_vrp[" << d << "," << n << "] should be NaN";
      } else {
        EXPECT_NEAR(actual, expected, kTol) << "iv_vrp[" << d << "," << n << "]";
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 5. iv_lo denominator defaults to 1.0 when nEarnCnt_5d is absent (no error).
// ---------------------------------------------------------------------------
TEST(IvFields, IvLoFallbackNEarnMissing) {
  const atx::usize D = 6;
  const atx::usize N = 4;
  const Panel base = make_iv_panel(D, N, /*with_nearn=*/false);
  ASSERT_EQ(idx(base, "nEarnCnt_5d"), static_cast<atx::usize>(-1));

  auto r = with_iv_fields(base);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  const Panel &aug = *r;

  // With no nEarnCnt_5d the denominator is 1.0 => iv_lo == atmCenI_21d everywhere.
  for (atx::usize d = 0; d < D; ++d) {
    for (atx::usize n = 0; n < N; ++n) {
      EXPECT_NEAR(cell(aug, "iv_lo", d, n), cell(base, "atmCenI_21d", d, n), kTol)
          << "iv_lo[" << d << "," << n << "] should equal atmCenI_21d (denom 1.0)";
    }
  }
}

} // namespace atxtest_iv_fields
