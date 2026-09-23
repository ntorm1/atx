// risk_fundamental_factors_test.cpp — L7: fundamental style factors + market intercept.
//
// Covers build_fundamental_exposures / FundamentalPanel (risk/fundamental_factors.hpp):
//   * USE4 standardization: every style column has cap-weighted mean 0 and
//     equal-weighted std 1 per date (with and without winsorization);
//   * PIT: moving a record's availability date past the query date changes the
//     exposure; the availability lag and staleness limits are honoured;
//   * column layout ([Market][sectors][styles]) and the missing-data policies;
//   * the hoisted Beta market series is bit-identical to the legacy per-instrument path;
//   * a golden determinism hash.

#include <bit>     // std::bit_cast
#include <cmath>   // std::isnan, std::sqrt, std::exp
#include <cstdint> // std::uint64_t
#include <limits>  // std::numeric_limits
#include <span>    // std::span
#include <vector>  // std::vector

#include <gtest/gtest.h>

#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"

#include "atx/engine/loop/panel_types.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/risk/exposures.hpp"
#include "atx/engine/risk/fundamental_factors.hpp"

namespace atx_test_l7_riskmodel_fundamental {

using atx::f64;
using atx::i64;
using atx::u32;
using atx::usize;
using atx::core::domain::Symbol;
using atx::engine::InstrumentId;
using atx::engine::kPanelFieldCount;
using atx::engine::PanelField;
using atx::engine::PanelView;
using namespace atx::engine::risk; // NOLINT(google-build-using-namespace) test-local

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// Owns a no-wrap PanelView backing store (row 0 of the grids = newest).
class L7Panel {
public:
  L7Panel(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close,
          const std::vector<std::vector<f64>> &volume)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2(n_rows)}, words_{(n_inst + 63U) / 64U} {
    for (usize i = 0; i < n_inst; ++i) {
      universe_.push_back(Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(kPanelFieldCount * cap_ * n_inst_, kNaN);
    mask_.assign(cap_ * words_, 0ULL);
    for (usize r = 0; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r;
      for (usize i = 0; i < n_inst_; ++i) {
        for (const PanelField f :
             {PanelField::Open, PanelField::High, PanelField::Low, PanelField::Close}) {
          set(f, phys, i, close[r][i]);
        }
        set(PanelField::Volume, phys, i, volume[r][i]);
        if (!std::isnan(close[r][i])) {
          mask_[phys * words_ + (i >> 6U)] |= (1ULL << (i & 63U));
        }
      }
    }
  }
  [[nodiscard]] PanelView view() const noexcept {
    return PanelView{fields_.data(), mask_.data(), std::span<const InstrumentId>{universe_},
                     cap_,           n_rows_ - 1U, n_rows_,
                     words_};
  }

private:
  static usize pow2(usize n) noexcept {
    usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }
  void set(PanelField f, usize phys, usize inst, f64 v) noexcept {
    fields_[static_cast<usize>(f) * cap_ * n_inst_ + phys * n_inst_ + inst] = v;
  }
  usize n_rows_;
  usize n_inst_;
  usize cap_;
  usize words_;
  std::vector<InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<std::uint64_t> mask_;
};

// Deterministic random-walk panel: n_rows × n_inst closes (newest first) + volumes.
struct World {
  usize rows;
  usize inst;
  std::vector<std::vector<f64>> close;
  std::vector<std::vector<f64>> volume;
  std::vector<f64> cap;
  std::vector<u32> group;
};

World make_world(usize rows, usize inst, std::uint64_t seed) {
  atx::core::Xoshiro256pp rng{seed};
  World w{rows, inst, std::vector<std::vector<f64>>(rows, std::vector<f64>(inst)),
          std::vector<std::vector<f64>>(rows, std::vector<f64>(inst)), {}, {}};
  // Build oldest -> newest so row rows-1 is the oldest.
  std::vector<f64> px(inst);
  for (usize i = 0; i < inst; ++i) {
    px[i] = 20.0 + 5.0 * rng.uniform01();
  }
  for (usize k = 0; k < rows; ++k) {
    const usize r = rows - 1U - k;
    const f64 mkt = 0.01 * rng.normal();
    for (usize i = 0; i < inst; ++i) {
      px[i] *= std::exp(mkt * (0.5 + 0.01 * static_cast<f64>(i)) + 0.02 * rng.normal());
      w.close[r][i] = px[i];
      w.volume[r][i] = 1e5 * (1.0 + rng.uniform01());
    }
  }
  for (usize i = 0; i < inst; ++i) {
    w.cap.push_back(1e8 * std::exp(2.0 * rng.normal()));
    w.group.push_back(static_cast<u32>(i % 4U));
  }
  return w;
}

FundamentalPanel make_fund(usize inst, std::uint64_t seed) {
  atx::core::Xoshiro256pp rng{seed};
  FundamentalPanel fp{inst};
  for (usize i = 0; i < inst; ++i) {
    const f64 scale = 1e7 * (1.0 + 4.0 * rng.uniform01());
    for (const i64 day : {i64{0}, i64{400}}) {
      EXPECT_TRUE(fp.add(i, FundamentalField::BookEquity, day, scale * (0.5 + rng.uniform01())));
      EXPECT_TRUE(fp.add(i, FundamentalField::EarningsTtm, day, scale * 0.1 * rng.normal()));
      EXPECT_TRUE(fp.add(i, FundamentalField::SalesTtm, day, scale * (1.0 + rng.uniform01())));
      EXPECT_TRUE(fp.add(i, FundamentalField::GrossProfit, day, scale * 0.3 * rng.uniform01()));
      EXPECT_TRUE(fp.add(i, FundamentalField::TotalAssets, day, scale * (2.0 + rng.uniform01())));
      EXPECT_TRUE(fp.add(i, FundamentalField::TotalDebt, day, scale * rng.uniform01()));
      EXPECT_TRUE(fp.add(i, FundamentalField::DividendsTtm, day, scale * 0.02 * rng.uniform01()));
      EXPECT_TRUE(fp.add(i, FundamentalField::SharesOutstanding, day, 1e6 * (1.0 + rng.uniform01())));
      EXPECT_TRUE(
          fp.add(i, FundamentalField::ShortInterestShares, day, 1e4 * (1.0 + rng.uniform01())));
    }
  }
  return fp;
}

StyleMask all_l7_mask() {
  StyleMask m;
  for (const StyleFactor f :
       {StyleFactor::Size, StyleFactor::Volatility, StyleFactor::Liquidity,
        StyleFactor::BookToPrice, StyleFactor::EarningsYield, StyleFactor::Growth,
        StyleFactor::Profitability, StyleFactor::Leverage, StyleFactor::DivYield,
        StyleFactor::ResidVol, StyleFactor::ShortInterest, StyleFactor::STReversal,
        StyleFactor::Market}) {
    m |= style_bit(f);
  }
  return m;
}

std::uint64_t fnv(const atx::core::linalg::MatX &x) {
  std::uint64_t h = 1469598103934665603ULL;
  for (Eigen::Index c = 0; c < x.cols(); ++c) {
    for (Eigen::Index r = 0; r < x.rows(); ++r) {
      h ^= std::bit_cast<std::uint64_t>(x(r, c));
      h *= 1099511628211ULL;
    }
  }
  return h;
}

// Column index of a style (or -1).
Eigen::Index style_col(const ExposureMatrix &x, StyleFactor f) {
  for (usize k = 0; k < x.columns.size(); ++k) {
    if (x.columns[k].kind == ColumnTag::Kind::Style && x.columns[k].style == f) {
      return static_cast<Eigen::Index>(k);
    }
  }
  return -1;
}

// Tests live inside the named namespace (unity-build safe: no file-scope using).

TEST(RiskFundamentalFactors, PanelAsOfHonoursAvailabilityLagAndStaleness) {
  FundamentalPanel fp{2};
  ASSERT_TRUE(fp.add(0, FundamentalField::BookEquity, 100, 1.0));
  ASSERT_TRUE(fp.add(0, FundamentalField::BookEquity, 200, 2.0));
  ASSERT_TRUE(fp.add(0, FundamentalField::BookEquity, 200, 3.0)); // same-day restatement wins
  EXPECT_TRUE(std::isnan(fp.as_of(0, FundamentalField::BookEquity, 99)));
  EXPECT_EQ(fp.as_of(0, FundamentalField::BookEquity, 100), 1.0);
  EXPECT_EQ(fp.as_of(0, FundamentalField::BookEquity, 199), 1.0);
  EXPECT_EQ(fp.as_of(0, FundamentalField::BookEquity, 200), 3.0);
  EXPECT_EQ(fp.as_of(0, FundamentalField::BookEquity, 245, /*lag=*/45), 3.0);
  EXPECT_EQ(fp.as_of(0, FundamentalField::BookEquity, 244, /*lag=*/45), 1.0);
  EXPECT_TRUE(std::isnan(fp.as_of(0, FundamentalField::BookEquity, 500, 0, /*stale=*/100)));
  EXPECT_TRUE(std::isnan(fp.as_of(1, FundamentalField::BookEquity, 500)));
  EXPECT_FALSE(fp.add(2, FundamentalField::BookEquity, 0, 1.0));
  EXPECT_FALSE(fp.add(0, FundamentalField::BookEquity, 0, kNaN));
}

TEST(RiskFundamentalFactors, ColumnsAreCapWeightedMeanZeroEqualWeightedSdOne) {
  const World w = make_world(90, 60, 11U);
  const L7Panel p{w.rows, w.inst, w.close, w.volume};
  const FundamentalPanel fp = make_fund(w.inst, 12U);
  for (const f64 winsor : {0.0, 3.0, 1.5}) {
    FundamentalCfg cfg;
    cfg.mask = all_l7_mask();
    cfg.winsor = winsor;
    const auto x = build_fundamental_exposures(p.view(), &fp, cfg, 0U, 450, w.cap, w.group);
    ASSERT_TRUE(x) << x.error().message();
    ASSERT_EQ(x->n_instruments(), w.inst);
    for (usize k = 0; k < x->columns.size(); ++k) {
      const ColumnTag &t = x->columns[k];
      if (t.kind != ColumnTag::Kind::Style || t.style == StyleFactor::Market) {
        continue;
      }
      f64 sw = 0.0;
      f64 swx = 0.0;
      f64 ss = 0.0;
      for (usize r = 0; r < w.inst; ++r) {
        const f64 v = x->x(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(k));
        const f64 c = w.cap[x->instrument_rows[r]];
        sw += c;
        swx += c * v;
        ss += v * v;
      }
      EXPECT_NEAR(swx / sw, 0.0, 1e-12) << "col " << k << " winsor " << winsor;
      EXPECT_NEAR(std::sqrt(ss / static_cast<f64>(w.inst)), 1.0, 1e-12) << "col " << k;
    }
  }
}

TEST(RiskFundamentalFactors, LayoutIsMarketThenSectorsThenStyles) {
  const World w = make_world(70, 12, 3U);
  const L7Panel p{w.rows, w.inst, w.close, w.volume};
  const FundamentalPanel fp = make_fund(w.inst, 4U);
  FundamentalCfg cfg;
  cfg.mask = style_bit(StyleFactor::Market) | style_bit(StyleFactor::BookToPrice) |
             style_bit(StyleFactor::STReversal);
  const auto x = build_fundamental_exposures(p.view(), &fp, cfg, 0U, 450, w.cap, w.group);
  ASSERT_TRUE(x);
  ASSERT_EQ(x->n_factors(), 1U + 4U + 2U);
  EXPECT_EQ(x->columns[0].style, StyleFactor::Market);
  for (Eigen::Index r = 0; r < x->x.rows(); ++r) {
    EXPECT_EQ(x->x(r, 0), 1.0);
  }
  for (usize g = 0; g < 4U; ++g) {
    EXPECT_EQ(x->columns[1U + g].kind, ColumnTag::Kind::Sector);
    EXPECT_EQ(x->columns[1U + g].group_id, static_cast<u32>(g));
  }
  EXPECT_EQ(x->columns[5].style, StyleFactor::BookToPrice);
  EXPECT_EQ(x->columns[6].style, StyleFactor::STReversal);
}

TEST(RiskFundamentalFactors, MovingAvailabilityPastAsOfChangesExposure) {
  const World w = make_world(40, 10, 5U);
  const L7Panel p{w.rows, w.inst, w.close, w.volume};
  FundamentalCfg cfg;
  cfg.mask = style_bit(StyleFactor::BookToPrice);
  cfg.winsor = 0.0;
  auto fill = [&](i64 avail_of_inst3) {
    FundamentalPanel fp{w.inst};
    for (usize i = 0; i < w.inst; ++i) {
      EXPECT_TRUE(fp.add(i, FundamentalField::BookEquity, 0, 1e7 * static_cast<f64>(i + 1U)));
    }
    // Instrument 3 files a big restatement; its availability is the variable under test.
    EXPECT_TRUE(fp.add(3, FundamentalField::BookEquity, avail_of_inst3, 5e9));
    return fp;
  };
  const FundamentalPanel early = fill(100);
  const FundamentalPanel late = fill(101);
  const auto xe = build_fundamental_exposures(p.view(), &early, cfg, 0U, 100, w.cap, {});
  const auto xl = build_fundamental_exposures(p.view(), &late, cfg, 0U, 100, w.cap, {});
  ASSERT_TRUE(xe);
  ASSERT_TRUE(xl);
  EXPECT_NE(xe->x(3, 0), xl->x(3, 0)); // public on day 100 vs not yet public
  // The late panel at day 100 equals a panel that never had the restatement.
  const FundamentalPanel none = fill(1'000'000);
  const auto xn = build_fundamental_exposures(p.view(), &none, cfg, 0U, 100, w.cap, {});
  ASSERT_TRUE(xn);
  EXPECT_EQ(fnv(xl->x), fnv(xn->x));
  // An availability lag of 1 day hides the day-100 filing too.
  cfg.availability_lag = 1;
  const auto xlag = build_fundamental_exposures(p.view(), &early, cfg, 0U, 100, w.cap, {});
  ASSERT_TRUE(xlag);
  EXPECT_EQ(fnv(xlag->x), fnv(xn->x));
}

TEST(RiskFundamentalFactors, MissingPolicyFillZeroVersusDrop) {
  const World w = make_world(40, 8, 6U);
  const L7Panel p{w.rows, w.inst, w.close, w.volume};
  FundamentalPanel fp{w.inst};
  for (usize i = 0; i < w.inst; ++i) {
    if (i != 5U) { // instrument 5 never reports earnings
      ASSERT_TRUE(fp.add(i, FundamentalField::EarningsTtm, 0, 1e6 * static_cast<f64>(i + 1U)));
    }
  }
  FundamentalCfg cfg;
  cfg.mask = style_bit(StyleFactor::EarningsYield);
  const auto fill = build_fundamental_exposures(p.view(), &fp, cfg, 0U, 10, w.cap, {});
  ASSERT_TRUE(fill);
  EXPECT_EQ(fill->n_instruments(), w.inst);
  EXPECT_EQ(fill->x(5, 0), 0.0);
  cfg.missing = MissingPolicy::Drop;
  const auto drop = build_fundamental_exposures(p.view(), &fp, cfg, 0U, 10, w.cap, {});
  ASSERT_TRUE(drop);
  EXPECT_EQ(drop->n_instruments(), w.inst - 1U);
  for (const usize inst : drop->instrument_rows) {
    EXPECT_NE(inst, 5U);
  }
}

TEST(RiskFundamentalFactors, CapFromSharesWhenNoCapSpan) {
  const World w = make_world(30, 6, 8U);
  const L7Panel p{w.rows, w.inst, w.close, w.volume};
  FundamentalPanel fp{w.inst};
  for (usize i = 0; i < w.inst; ++i) {
    ASSERT_TRUE(fp.add(i, FundamentalField::SharesOutstanding, 0, 1e6 * static_cast<f64>(i + 1U)));
  }
  FundamentalCfg cfg;
  cfg.mask = style_bit(StyleFactor::Size);
  cfg.winsor = 0.0;
  const auto x = build_fundamental_exposures(p.view(), &fp, cfg, 0U, 10, {}, {});
  ASSERT_TRUE(x);
  ASSERT_EQ(x->n_instruments(), w.inst);
  // ln(shares·close) ranks: the standardized column is increasing in shares·close.
  const Eigen::Index c = style_col(*x, StyleFactor::Size);
  for (usize a = 0; a < w.inst; ++a) {
    for (usize b = 0; b < w.inst; ++b) {
      const f64 ca = 1e6 * static_cast<f64>(a + 1U) * w.close[0][a];
      const f64 cb = 1e6 * static_cast<f64>(b + 1U) * w.close[0][b];
      if (ca < cb) {
        EXPECT_LT(x->x(static_cast<Eigen::Index>(a), c), x->x(static_cast<Eigen::Index>(b), c));
      }
    }
  }
}

TEST(RiskFundamentalFactors, RejectsFundamentalStyleWithoutPanelAndBadSpans) {
  const World w = make_world(30, 6, 9U);
  const L7Panel p{w.rows, w.inst, w.close, w.volume};
  FundamentalCfg cfg;
  cfg.mask = style_bit(StyleFactor::BookToPrice);
  EXPECT_FALSE(build_fundamental_exposures(p.view(), nullptr, cfg, 0U, 0, w.cap, {}));
  cfg.mask = style_bit(StyleFactor::STReversal);
  EXPECT_TRUE(build_fundamental_exposures(p.view(), nullptr, cfg, 0U, 0, w.cap, {}));
  const std::vector<f64> short_cap(3, 1.0);
  EXPECT_FALSE(build_fundamental_exposures(p.view(), nullptr, cfg, 0U, 0, short_cap, {}));
  EXPECT_FALSE(build_fundamental_exposures(p.view(), nullptr, cfg, 30U, 0, w.cap, {}));
  const FundamentalPanel wrong{5};
  EXPECT_FALSE(build_fundamental_exposures(p.view(), &wrong, cfg, 0U, 0, w.cap, {}));
}

TEST(RiskFundamentalFactors, HoistedBetaIsBitIdenticalToLegacy) {
  const World w = make_world(300, 25, 21U);
  const L7Panel p{w.rows, w.inst, w.close, w.volume};
  const PanelView v = p.view();
  for (const usize row : {usize{0}, usize{10}, usize{47}}) {
    const std::vector<f64> mkt = atx::engine::risk::detail::market_returns(v, row, atx::engine::risk::detail::kBetaWindow);
    for (usize i = 0; i < w.inst; ++i) {
      const f64 a = atx::engine::risk::detail::beta(v, row, i);
      const f64 b = atx::engine::risk::detail::beta_cached(v, row, i, mkt);
      EXPECT_EQ(std::bit_cast<std::uint64_t>(a), std::bit_cast<std::uint64_t>(b));
    }
  }
}

TEST(RiskFundamentalFactors, GoldenDeterminismHash) {
  const World w = make_world(90, 30, 31U);
  const L7Panel p{w.rows, w.inst, w.close, w.volume};
  const FundamentalPanel fp = make_fund(w.inst, 32U);
  FundamentalCfg cfg;
  cfg.mask = all_l7_mask();
  const auto a = build_fundamental_exposures(p.view(), &fp, cfg, 3U, 420, w.cap, w.group);
  const auto b = build_fundamental_exposures(p.view(), &fp, cfg, 3U, 420, w.cap, w.group);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_EQ(fnv(a->x), fnv(b->x));
  EXPECT_EQ(a->instrument_rows, b->instrument_rows);
  EXPECT_EQ(fnv(a->x), 5782365001156645395ULL) << "golden exposure hash drifted";
}

} // namespace atx_test_l7_riskmodel_fundamental
