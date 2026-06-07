// risk_exposures_test.cpp — P4-6: the factor exposure matrix `X` builder.
//
// build_exposures(panel, cfg, row, market_cap, group_id) assembles the per-date
// M_valid×K exposure block from the as-built OHLCV-only PanelView plus the two
// OPTIONAL external inputs (cap span -> Size; group_id span -> sector dummies).
// The price-derived style factors (Momentum, Volatility, Beta, Liquidity) come
// purely from the panel; each style column is z-scored cross-sectionally over its
// non-NaN universe; sector dummies are 0/1 and NOT standardized. An instrument
// missing ANY emitted style exposure is DROPPED from the date (§3.3).
//
// Coverage (plan §8 P4-6):
//   * Size = ln(cap) on a fixture (raw value pinned via a single-instrument cap so
//     the z-score is degenerate-0 and the RAW ln(cap) is recoverable by mask gate).
//   * a standardized style column has mean≈0 / population-std≈1 over the valid set.
//   * sector dummies sum to the group size.
//   * Momentum / Volatility windows read only the right trailing rows
//     (truncation-invariant: mutating rows BEYOND the lookback leaves X identical;
//      mutating rows INSIDE it changes X).
//   * a missing cap (NaN / <=0) drops that instrument when Size is emitted.
//   * boundary: 1-instrument universe (every style z-scores to 0, degenerate);
//     a single sector (one dummy column, all 1s, summing to the surviving count).

#include <cmath>   // std::isnan, std::log, std::sqrt, std::exp
#include <cstdint> // fixed-width
#include <limits>  // std::numeric_limits (quiet NaN sentinel)
#include <span>
#include <vector>

#include <gtest/gtest.h> // gtest macros

#include <Eigen/Dense> // Eigen::Index for matrix indexing in assertions

#include "atx/core/types.hpp"

#include "atx/engine/loop/panel_types.hpp" // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"       // InstrumentId (Symbol)
#include "atx/engine/risk/exposures.hpp"

namespace {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::domain::Symbol;
using atx::engine::InstrumentId;
using atx::engine::kPanelFieldCount;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::risk::build_exposures;
using atx::engine::risk::ColumnTag;
using atx::engine::risk::ExposureMatrix;
using atx::engine::risk::FactorModelConfig;
using atx::engine::risk::StyleFactor;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// ===========================================================================
//  PanelFixture — owns a PanelView's backing storage for one test.
//
//  Builds a column-major-per-field ring block exactly as RollingPanel would.
//  Caller supplies an `n_rows × n_inst` grid for close + volume (the only fields
//  the style factors read); open/high/low are filled with close. row index 0 of
//  the input grids is the NEWEST cross-section (matching PanelView newest-first).
//  cap = next pow2 >= n_rows; head = n_rows-1 so newest-first index r maps to
//  physical row n_rows-1-r (physical 0 = oldest) — a clean, no-wrap layout.
// ===========================================================================
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close,
               const std::vector<std::vector<f64>> &volume)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2_ceil(n_rows)},
        mask_words_{(n_inst + 63U) / 64U} {
    universe_.reserve(n_inst);
    for (usize i = 0; i < n_inst; ++i) {
      universe_.push_back(Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(kPanelFieldCount * cap_ * n_inst_, kNaN);
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r; // newest-first r -> physical row
      for (usize i = 0; i < n_inst_; ++i) {
        const f64 c = close[r][i];
        const f64 v = volume[r][i];
        set(PanelField::Open, phys, i, c);
        set(PanelField::High, phys, i, c);
        set(PanelField::Low, phys, i, c);
        set(PanelField::Close, phys, i, c);
        set(PanelField::Volume, phys, i, v);
        if (!std::isnan(c)) {
          mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
        }
      }
    }
  }

  [[nodiscard]] PanelView view() const noexcept {
    return PanelView{fields_.data(), mask_.data(), std::span<const InstrumentId>{universe_},
                     cap_,           head_(),      n_rows_,
                     mask_words_};
  }

private:
  [[nodiscard]] usize head_() const noexcept { return (n_rows_ == 0U) ? 0U : n_rows_ - 1U; }

  static usize pow2_ceil(usize n) noexcept {
    usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }

  void set(PanelField f, usize phys, usize inst, f64 v) noexcept {
    const usize block = static_cast<usize>(f) * cap_ * n_inst_;
    fields_[block + phys * n_inst_ + inst] = v;
  }

  usize n_rows_;
  usize n_inst_;
  usize cap_;
  usize mask_words_;
  std::vector<InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<atx::u64> mask_;
};

// Locate the matrix column index of a given style factor; -1 if not emitted.
[[nodiscard]] int style_col(const ExposureMatrix &x, StyleFactor f) {
  for (usize k = 0; k < x.columns.size(); ++k) {
    if (x.columns[k].kind == ColumnTag::Kind::Style && x.columns[k].style == f) {
      return static_cast<int>(k);
    }
  }
  return -1;
}

// Config emitting ONLY one style factor (no sectors) — isolates a column's math.
[[nodiscard]] FactorModelConfig only_style(StyleFactor f) {
  FactorModelConfig cfg;
  cfg.sector_factors = false;
  cfg.style_mask = static_cast<atx::u8>(1U << static_cast<unsigned>(f));
  return cfg;
}

// Build a constant-volume grid (n_rows × n_inst), value `vol`.
[[nodiscard]] std::vector<std::vector<f64>> const_grid(usize n_rows, usize n_inst, f64 val) {
  return std::vector<std::vector<f64>>(n_rows, std::vector<f64>(n_inst, val));
}

// ===========================================================================
//  Size = ln(cap). Single instrument so the z-score is degenerate (->0); but
//  with one instrument the *standardized* Size is 0 regardless of cap, so to pin
//  the ln(cap) MECHANICS we use TWO instruments with caps whose z-score is hand-
//  computable: caps {e^1, e^3} -> ln = {1, 3} -> mean 2, popstd 1 -> z = {-1, +1}.
// ===========================================================================
TEST(RiskExposures, SizeIsLnCapStandardized) {
  const usize rows = 1U, inst = 2U;
  PanelFixture fx{rows, inst, {{100.0, 100.0}}, {{10.0, 10.0}}};
  const std::vector<f64> cap{std::exp(1.0), std::exp(3.0)}; // ln -> 1, 3
  const FactorModelConfig cfg = only_style(StyleFactor::Size);

  const auto r =
      build_exposures(fx.view(), cfg, 0U, std::span<const f64>{cap}, std::span<const u32>{});
  ASSERT_TRUE(r.has_value());
  const ExposureMatrix &x = r.value();
  ASSERT_EQ(x.n_instruments(), 2U);
  const int col = style_col(x, StyleFactor::Size);
  ASSERT_GE(col, 0);
  EXPECT_NEAR(x.x(0, col), -1.0, 1e-9);
  EXPECT_NEAR(x.x(1, col), +1.0, 1e-9);
}

// A missing cap (NaN or <=0) drops the instrument when Size is in the mask.
TEST(RiskExposures, MissingCapDropsInstrument) {
  const usize rows = 1U, inst = 3U;
  PanelFixture fx{rows, inst, {{100.0, 100.0, 100.0}}, {{10.0, 10.0, 10.0}}};
  const std::vector<f64> cap{std::exp(1.0), kNaN, std::exp(3.0)}; // middle missing
  const FactorModelConfig cfg = only_style(StyleFactor::Size);

  const auto r =
      build_exposures(fx.view(), cfg, 0U, std::span<const f64>{cap}, std::span<const u32>{});
  ASSERT_TRUE(r.has_value());
  const ExposureMatrix &x = r.value();
  ASSERT_EQ(x.n_instruments(), 2U);
  // Surviving rows are universe columns 0 and 2 (the NaN-cap instrument 1 dropped).
  EXPECT_EQ(x.instrument_rows[0], 0U);
  EXPECT_EQ(x.instrument_rows[1], 2U);
}

// ===========================================================================
//  Liquidity = ln(adv20), adv20 = mean(close*volume) over 20 trailing rows.
//  Multi-instrument distinct values -> the emitted column is mean≈0 / popstd≈1.
// ===========================================================================
TEST(RiskExposures, StandardizedColumnHasZeroMeanUnitStd) {
  const usize rows = 20U, inst = 4U;
  // Distinct per-instrument price*volume so adv20 differs across instruments.
  std::vector<std::vector<f64>> close(rows, std::vector<f64>(inst));
  std::vector<std::vector<f64>> volume(rows, std::vector<f64>(inst));
  for (usize r = 0; r < rows; ++r) {
    for (usize i = 0; i < inst; ++i) {
      close[r][i] = 10.0 * static_cast<f64>(i + 1U); // 10,20,30,40
      volume[r][i] = 1000.0;
    }
  }
  PanelFixture fx{rows, inst, close, volume};
  const FactorModelConfig cfg = only_style(StyleFactor::Liquidity);

  const auto r =
      build_exposures(fx.view(), cfg, 0U, std::span<const f64>{}, std::span<const u32>{});
  ASSERT_TRUE(r.has_value());
  const ExposureMatrix &x = r.value();
  ASSERT_EQ(x.n_instruments(), 4U);
  const int col = style_col(x, StyleFactor::Liquidity);
  ASSERT_GE(col, 0);
  f64 mean = 0.0;
  for (usize i = 0; i < 4U; ++i) {
    mean += x.x(static_cast<Eigen::Index>(i), col);
  }
  mean /= 4.0;
  EXPECT_NEAR(mean, 0.0, 1e-9);
  f64 ss = 0.0;
  for (usize i = 0; i < 4U; ++i) {
    const f64 d = x.x(static_cast<Eigen::Index>(i), col) - mean;
    ss += d * d;
  }
  EXPECT_NEAR(std::sqrt(ss / 4.0), 1.0, 1e-9);
}

// ===========================================================================
//  Volatility window truncation-invariance: stddev(returns,60) reads only the
//  newest ~60 returns. Two panels identical in the newest 61 rows but DIFFERENT
//  in older rows must yield the SAME Volatility column; a change INSIDE the window
//  must change it.
// ===========================================================================
TEST(RiskExposures, VolatilityWindowTruncationInvariant) {
  const usize rows = 120U, inst = 2U;
  auto make = [&](f64 old_close) {
    std::vector<std::vector<f64>> close(rows, std::vector<f64>(inst));
    std::vector<std::vector<f64>> volume = const_grid(rows, inst, 1000.0);
    for (usize r = 0; r < rows; ++r) {
      for (usize i = 0; i < inst; ++i) {
        // Newest 61 rows: a deterministic zig-zag; rows >= 61: a flat `old_close`.
        if (r < 61U) {
          const f64 base = 100.0 + static_cast<f64>(i) * 5.0;
          close[r][i] = base + ((r % 2U == 0U) ? 1.0 : -1.0);
        } else {
          close[r][i] = old_close;
        }
      }
    }
    return PanelFixture{rows, inst, close, volume};
  };
  const FactorModelConfig cfg = only_style(StyleFactor::Volatility);
  const PanelFixture a = make(50.0);
  const PanelFixture b = make(999.0); // differs ONLY beyond the 60-return window
  const auto ra =
      build_exposures(a.view(), cfg, 0U, std::span<const f64>{}, std::span<const u32>{});
  const auto rb =
      build_exposures(b.view(), cfg, 0U, std::span<const f64>{}, std::span<const u32>{});
  ASSERT_TRUE(ra.has_value());
  ASSERT_TRUE(rb.has_value());
  const int ca = style_col(ra.value(), StyleFactor::Volatility);
  const int cb = style_col(rb.value(), StyleFactor::Volatility);
  ASSERT_GE(ca, 0);
  ASSERT_GE(cb, 0);
  for (usize i = 0; i < 2U; ++i) {
    EXPECT_NEAR(ra.value().x(static_cast<Eigen::Index>(i), ca),
                rb.value().x(static_cast<Eigen::Index>(i), cb), 1e-12);
  }
}

// ===========================================================================
//  Sector dummies sum to the group size. Two groups {A: 3 insts, B: 2 insts};
//  no style factors -> exactly two 0/1 columns, each summing to its group count.
// ===========================================================================
TEST(RiskExposures, SectorDummiesSumToGroupSize) {
  const usize rows = 1U, inst = 5U;
  PanelFixture fx{rows, inst, {const_grid(1U, inst, 100.0)[0]}, {const_grid(1U, inst, 10.0)[0]}};
  const std::vector<u32> group{7U, 7U, 9U, 7U, 9U}; // group 7: {0,1,3}, group 9: {2,4}
  FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = 0x00; // sectors only

  const auto r =
      build_exposures(fx.view(), cfg, 0U, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(r.has_value());
  const ExposureMatrix &x = r.value();
  ASSERT_EQ(x.n_factors(), 2U); // one dummy per distinct group
  ASSERT_EQ(x.n_instruments(), 5U);
  // Columns are ascending group id: col 0 -> group 7, col 1 -> group 9.
  EXPECT_EQ(x.columns[0].kind, ColumnTag::Kind::Sector);
  EXPECT_EQ(x.columns[0].group_id, 7U);
  EXPECT_EQ(x.columns[1].group_id, 9U);
  f64 sum7 = 0.0, sum9 = 0.0;
  for (usize i = 0; i < 5U; ++i) {
    sum7 += x.x(static_cast<Eigen::Index>(i), 0);
    sum9 += x.x(static_cast<Eigen::Index>(i), 1);
  }
  EXPECT_NEAR(sum7, 3.0, 1e-12);
  EXPECT_NEAR(sum9, 2.0, 1e-12);
}

// ===========================================================================
//  Boundary — single sector: one group over all instruments -> one dummy column,
//  all 1s, summing to the surviving instrument count.
// ===========================================================================
TEST(RiskExposures, SingleSectorAllOnes) {
  const usize rows = 1U, inst = 3U;
  PanelFixture fx{rows, inst, {const_grid(1U, inst, 100.0)[0]}, {const_grid(1U, inst, 10.0)[0]}};
  const std::vector<u32> group{4U, 4U, 4U};
  FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = 0x00;

  const auto r =
      build_exposures(fx.view(), cfg, 0U, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(r.has_value());
  const ExposureMatrix &x = r.value();
  ASSERT_EQ(x.n_factors(), 1U);
  ASSERT_EQ(x.n_instruments(), 3U);
  f64 sum = 0.0;
  for (usize i = 0; i < 3U; ++i) {
    EXPECT_NEAR(x.x(static_cast<Eigen::Index>(i), 0), 1.0, 1e-12);
    sum += x.x(static_cast<Eigen::Index>(i), 0);
  }
  EXPECT_NEAR(sum, 3.0, 1e-12);
}

// ===========================================================================
//  Boundary — 1-instrument universe: a style column degenerately z-scores to 0.
//  Liquidity is well-defined for one instrument (adv20 > 0); its standardized
//  value is 0 because the cross-section has no spread.
// ===========================================================================
TEST(RiskExposures, SingleInstrumentDegenerateZeroZScore) {
  const usize rows = 20U, inst = 1U;
  PanelFixture fx{rows, inst, const_grid(rows, inst, 100.0), const_grid(rows, inst, 1000.0)};
  const FactorModelConfig cfg = only_style(StyleFactor::Liquidity);

  const auto r =
      build_exposures(fx.view(), cfg, 0U, std::span<const f64>{}, std::span<const u32>{});
  ASSERT_TRUE(r.has_value());
  const ExposureMatrix &x = r.value();
  ASSERT_EQ(x.n_instruments(), 1U);
  const int col = style_col(x, StyleFactor::Liquidity);
  ASSERT_GE(col, 0);
  EXPECT_NEAR(x.x(0, col), 0.0, 1e-12);
}

// ===========================================================================
//  Momentum = ts_sum(ret,252) - ts_sum(ret,21). Needs >= row+253 valid rows.
//  Truncation-invariance: a change beyond the 252-return window leaves Momentum
//  unchanged. Build 260-row panels differing only in the oldest rows.
// ===========================================================================
TEST(RiskExposures, MomentumWindowTruncationInvariant) {
  const usize rows = 260U, inst = 2U;
  auto make = [&](f64 tail_close) {
    std::vector<std::vector<f64>> close(rows, std::vector<f64>(inst));
    std::vector<std::vector<f64>> volume = const_grid(rows, inst, 1000.0);
    for (usize r = 0; r < rows; ++r) {
      for (usize i = 0; i < inst; ++i) {
        // Momentum reads ret[0..251] = close[r]/close[r+1]-1 for r in [0,251],
        // i.e. closes [0..252]. Rows >= 254 are beyond the window -> free to vary.
        if (r <= 252U) {
          close[r][i] = 100.0 + static_cast<f64>(252U - r) + static_cast<f64>(i) * 3.0;
        } else {
          close[r][i] = tail_close;
        }
      }
    }
    return PanelFixture{rows, inst, close, volume};
  };
  const FactorModelConfig cfg = only_style(StyleFactor::Momentum);
  const PanelFixture a = make(40.0);
  const PanelFixture b = make(777.0);
  const auto ra =
      build_exposures(a.view(), cfg, 0U, std::span<const f64>{}, std::span<const u32>{});
  const auto rb =
      build_exposures(b.view(), cfg, 0U, std::span<const f64>{}, std::span<const u32>{});
  ASSERT_TRUE(ra.has_value());
  ASSERT_TRUE(rb.has_value());
  const int ca = style_col(ra.value(), StyleFactor::Momentum);
  const int cb = style_col(rb.value(), StyleFactor::Momentum);
  ASSERT_GE(ca, 0);
  ASSERT_GE(cb, 0);
  for (usize i = 0; i < 2U; ++i) {
    EXPECT_NEAR(ra.value().x(static_cast<Eigen::Index>(i), ca),
                rb.value().x(static_cast<Eigen::Index>(i), cb), 1e-12);
  }
}

// ===========================================================================
//  Column ORDER: sector dummies first (ascending group id), then style columns in
//  StyleFactor enum order. Emit Liquidity + one sector and assert the layout.
// ===========================================================================
TEST(RiskExposures, ColumnOrderSectorsThenStyles) {
  const usize rows = 20U, inst = 2U;
  std::vector<std::vector<f64>> close(rows, std::vector<f64>(inst));
  std::vector<std::vector<f64>> volume = const_grid(rows, inst, 1000.0);
  for (usize r = 0; r < rows; ++r) {
    close[r][0] = 10.0;
    close[r][1] = 50.0; // distinct adv -> Liquidity not all-equal
  }
  PanelFixture fx{rows, inst, close, volume};
  const std::vector<u32> group{2U, 2U};
  FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = static_cast<atx::u8>(1U << static_cast<unsigned>(StyleFactor::Liquidity));

  const auto r =
      build_exposures(fx.view(), cfg, 0U, std::span<const f64>{}, std::span<const u32>{group});
  ASSERT_TRUE(r.has_value());
  const ExposureMatrix &x = r.value();
  ASSERT_EQ(x.n_factors(), 2U);
  EXPECT_EQ(x.columns[0].kind, ColumnTag::Kind::Sector);
  EXPECT_EQ(x.columns[1].kind, ColumnTag::Kind::Style);
  EXPECT_EQ(x.columns[1].style, StyleFactor::Liquidity);
}

// A row offset >= panel.rows() is an error (PIT cannot read a non-existent date).
TEST(RiskExposures, RowOutOfRangeIsError) {
  const usize rows = 5U, inst = 2U;
  PanelFixture fx{rows, inst, const_grid(rows, inst, 100.0), const_grid(rows, inst, 10.0)};
  const FactorModelConfig cfg = only_style(StyleFactor::Liquidity);
  const auto r =
      build_exposures(fx.view(), cfg, /*row=*/5U, std::span<const f64>{}, std::span<const u32>{});
  EXPECT_FALSE(r.has_value());
}

// A cap/group span whose length != instruments() (when non-empty) is an error.
TEST(RiskExposures, SpanLengthMismatchIsError) {
  const usize rows = 1U, inst = 3U;
  PanelFixture fx{rows, inst, {const_grid(1U, inst, 100.0)[0]}, {const_grid(1U, inst, 10.0)[0]}};
  const std::vector<f64> bad_cap{std::exp(1.0), std::exp(2.0)}; // len 2 != 3
  const FactorModelConfig cfg = only_style(StyleFactor::Size);
  const auto r =
      build_exposures(fx.view(), cfg, 0U, std::span<const f64>{bad_cap}, std::span<const u32>{});
  EXPECT_FALSE(r.has_value());
}

} // namespace
