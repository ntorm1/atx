// atx::engine::data — adapt_factor unit tests (P2-S6.6).
//
// Suite: DataAdaptFactor
//
// S6.6 wires two seams:
//   (1) artifact_to_factor_model  — BYO FactorModelArtifact -> risk::FactorModel
//       via a thin forward to risk::FactorModel::create (single validation point).
//   (2) reference_spans           — materialize market_cap / group_id cross-section
//       from a reference Dataset as-of a date, aligned onto the price instrument axis.
//
// Tests:
//   * ArtifactLowersToCreateByteIdentical     — both construction paths (create direct
//       + artifact_to_factor_model) produce bit-identical apply() output.
//   * NonSpdFArtifactErrs                     — a non-SPD F propagates the same error
//       code as FactorModel::create.
//   * ReferenceSpansFeedBuildComponents       — reference_spans produces correct spans
//       that successfully feed FactorModelBuilder::build_components.
//   * ReferenceMissingColumnErrs              — missing 'group_id' -> Err(NotFound).
//   * MissingInstrumentGivesNanDefault        — price instrument absent from reference
//       -> NaN market_cap / default_group group_id.
//   * NoPlugFallsBackToPriceDerived           — build_components with empty spans
//       still builds a valid FactorComponents (price-derived, no reference needed).

#include <array>
#include <bit>     // std::bit_cast (NaN-safe bit comparison)
#include <cmath>   // std::isnan
#include <cstdint> // uint64_t
#include <limits>  // quiet_NaN
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/data/adapt_factor.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/data/factor_model_artifact.hpp"
#include "atx/engine/loop/panel_types.hpp" // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"       // InstrumentId
#include "atx/engine/risk/exposures.hpp"   // FactorModelConfig
#include "atx/engine/risk/factor_model.hpp"

namespace atxtest_data_adapt_factor_test {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::InstrumentId;
using atx::engine::kPanelFieldCount;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::data::ColumnDType;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DatasetSchema;
using atx::engine::data::DateKey;
using atx::engine::data::FactorModelArtifact;
using atx::engine::data::InstKey;
using atx::engine::data::reference_spans;
using atx::engine::data::RefSpans;
using atx::engine::data::Role;
using atx::engine::risk::FactorComponents;
using atx::engine::risk::FactorModel;
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// ===========================================================================
//  Small valid test matrices: M=4 instruments, K=2 factors.
// ===========================================================================

// M×K exposure matrix.
[[nodiscard]] MatX make_x() {
  MatX x(4, 2);
  // clang-format off
  x << 1.0,  0.3,
       0.2,  0.8,
      -0.5,  0.1,
       0.4, -0.6;
  // clang-format on
  return x;
}

// K×K SPD factor covariance (eigenvalues ~ 2.1, 1.4 — clearly positive).
[[nodiscard]] MatX make_f_spd() {
  MatX f(2, 2);
  // clang-format off
  f << 2.0, 0.3,
       0.3, 1.5;
  // clang-format on
  return f;
}

// K×K non-SPD matrix (eigenvalues 3, -1 — not PD).
[[nodiscard]] MatX make_f_nonspd() {
  MatX f(2, 2);
  // clang-format off
  f << 1.0, 2.0,
       2.0, 1.0;
  // clang-format on
  return f;
}

// M positive specific variances.
[[nodiscard]] VecX make_d() {
  VecX d(4);
  d << 0.01, 0.02, 0.015, 0.008;
  return d;
}

// Deterministic probe vector (length M=4).
[[nodiscard]] std::vector<f64> make_probe() { return {0.5, -0.3, 0.2, -0.1}; }

// ===========================================================================
//  PanelFixture — minimal PanelView builder (copied from risk_factor_builder_test
//  pattern). Supports newest-first close/volume grids. n_rows × n_inst.
// ===========================================================================
class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close,
               const std::vector<std::vector<f64>> &volume)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2_ceil(n_rows)},
        mask_words_{(n_inst + 63U) / 64U} {
    universe_.reserve(n_inst);
    for (usize i = 0; i < n_inst; ++i) {
      universe_.push_back(InstrumentId{atx::core::domain::Symbol{static_cast<u32>(i + 1U)}});
    }
    fields_.assign(kPanelFieldCount * cap_ * n_inst_, kNaN);
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r;
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
  std::vector<u64> mask_;
};

// Build a minimal single-sector panel fixture (n_inst instruments, n_rows rows).
// Returns close prices: close[r][i] = 1.0 + 0.01*(r*n_inst + i) (positive,
// distinct per cell so step_returns are non-zero and non-degenerate).
[[nodiscard]] PanelFixture make_panel_fixture(usize n_rows, usize n_inst) {
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, 1000.0));
  // Newest row first (r=0 is newest). Price walks so step_return gives non-zero r.
  for (usize r = 0; r < n_rows; ++r) {
    for (usize i = 0; i < n_inst; ++i) {
      // Generate from oldest (r = n_rows-1) so newest (r=0) has higher prices.
      close[r][i] = 1.0 + 0.01 * static_cast<f64>((n_rows - r) * n_inst + i);
    }
  }
  return PanelFixture{n_rows, n_inst, close, volume};
}

// Sectors-only config: all instruments in one group (K=1) — the simplest valid
// model that doesn't require style-factor lookback (style_mask = 0).
[[nodiscard]] FactorModelConfig single_sector_cfg() {
  FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = 0x00;
  return cfg;
}

// Build a reference Dataset with "market_cap" and "group_id" columns.
// instruments: inst_keys; dates: date_keys; values filled per callback.
[[nodiscard]] Dataset make_reference_dataset(const std::vector<InstKey> &inst_keys,
                                             const std::vector<DateKey> &date_keys,
                                             const std::vector<f64> &market_cap_col,
                                             const std::vector<f64> &group_id_col) {
  DatasetSchema s;
  s.columns = {"market_cap", "group_id"};
  s.dtypes = {ColumnDType::F64, ColumnDType::Category};
  s.role = Role::Reference;
  std::vector<std::vector<f64>> data = {market_cap_col, group_id_col};
  auto res = Dataset::create(std::move(s), date_keys, inst_keys, std::move(data),
                             /*mask=*/{}, DatasetProvenance{"test:reference", ""});
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  return std::move(res).value();
}

// Build a minimal price Dataset (1 column "close") over the given instruments/dates.
[[nodiscard]] Dataset make_price_dataset(const std::vector<InstKey> &inst_keys,
                                         const std::vector<DateKey> &date_keys) {
  const usize cells = date_keys.size() * inst_keys.size();
  DatasetSchema s;
  s.columns = {"close"};
  s.dtypes = {ColumnDType::F64};
  s.role = Role::Price;
  std::vector<f64> col(cells, 100.0);
  std::vector<std::vector<f64>> data = {col};
  auto res = Dataset::create(std::move(s), date_keys, inst_keys, std::move(data),
                             /*mask=*/{}, DatasetProvenance{"test:price", ""});
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  return std::move(res).value();
}

// ===========================================================================
//  Tests
// ===========================================================================

// Both construction paths (FactorModel::create direct + artifact_to_factor_model)
// produce bit-identical apply() output for the same probe vector.
TEST(DataAdaptFactor, ArtifactLowersToCreateByteIdentical) {
  const MatX x = make_x();
  const MatX f = make_f_spd();
  const VecX d = make_d();
  const usize fit_begin = 0U;
  const usize fit_end = 3U;

  // Path A: direct create.
  auto res_a = FactorModel::create(x, f, d, fit_begin, fit_end);
  ASSERT_TRUE(res_a.has_value()) << res_a.error().message();
  const FactorModel &fm_a = res_a.value();

  // Path B: via artifact.
  FactorModelArtifact art{x, f, d, fit_begin, fit_end};
  auto res_b = atx::engine::data::artifact_to_factor_model(art);
  ASSERT_TRUE(res_b.has_value()) << res_b.error().message();
  const FactorModel &fm_b = res_b.value();

  // Verify n_factors() and specific_var() match.
  EXPECT_EQ(fm_a.n_factors(), fm_b.n_factors());
  ASSERT_EQ(fm_a.specific_var().size(), fm_b.specific_var().size());
  for (Eigen::Index i = 0; i < fm_a.specific_var().size(); ++i) {
    EXPECT_DOUBLE_EQ(fm_a.specific_var()[i], fm_b.specific_var()[i]);
  }

  // apply() both to the same probe vector; assert bit-identical output.
  const std::vector<f64> probe = make_probe();
  std::vector<f64> out_a(probe.size(), 0.0);
  std::vector<f64> out_b(probe.size(), 0.0);
  fm_a.apply(probe, out_a);
  fm_b.apply(probe, out_b);

  ASSERT_EQ(out_a.size(), out_b.size());
  for (usize i = 0; i < out_a.size(); ++i) {
    // NaN-safe bit comparison via memcpy reinterpret (std::bit_cast if C++20 available).
    u64 bits_a = 0;
    u64 bits_b = 0;
    static_assert(sizeof(f64) == sizeof(u64));
    __builtin_memcpy(&bits_a, &out_a[i], sizeof(u64));
    __builtin_memcpy(&bits_b, &out_b[i], sizeof(u64));
    EXPECT_EQ(bits_a, bits_b) << "apply() output differs at i=" << i
                               << " out_a=" << out_a[i] << " out_b=" << out_b[i];
  }
}

// A non-SPD F propagates the same error code from create (NotFound / InvalidArgument
// from create's LLT check) via artifact_to_factor_model.
TEST(DataAdaptFactor, NonSpdFArtifactErrs) {
  const MatX x = make_x();
  const MatX f_bad = make_f_nonspd();
  const VecX d = make_d();

  // Direct create: collect the error code.
  auto res_direct = FactorModel::create(x, f_bad, d, 0U, 3U);
  ASSERT_FALSE(res_direct.has_value()) << "expected create to fail on non-SPD F";

  // Via artifact: must also fail with the same error code.
  FactorModelArtifact art{x, f_bad, d, 0U, 3U};
  auto res_art = atx::engine::data::artifact_to_factor_model(art);
  ASSERT_FALSE(res_art.has_value()) << "expected artifact_to_factor_model to fail on non-SPD F";

  EXPECT_EQ(res_art.error().code(), res_direct.error().code())
      << "error codes differ: art=" << static_cast<int>(res_art.error().code())
      << " direct=" << static_cast<int>(res_direct.error().code());
}

// reference_spans produces correct market_cap / group_id per-instrument vectors
// aligned to the price instrument axis. Feed the spans into build_components and
// assert a valid FactorComponents results.
TEST(DataAdaptFactor, ReferenceSpansFeedBuildComponents) {
  // Reference: 2 instruments {10, 20}, 2 dates {100, 200}.
  // market_cap: flat {1e9, 2e9, 1.1e9, 2.1e9} (date-major: d0i0, d0i1, d1i0, d1i1)
  // group_id:   flat {1.0, 2.0, 1.0, 2.0}
  const std::vector<InstKey> ref_insts = {10u, 20u};
  const std::vector<DateKey> ref_dates = {100, 200};
  const std::vector<f64> mc_flat = {1e9, 2e9, 1.1e9, 2.1e9};
  const std::vector<f64> grp_flat = {1.0, 2.0, 1.0, 2.0};
  const Dataset ref = make_reference_dataset(ref_insts, ref_dates, mc_flat, grp_flat);

  // Price: same 2 instruments, 1 date {150}.
  const Dataset price = make_price_dataset({10u, 20u}, {150});

  // as_of_date = 150 -> as_of_index(ref_dates={100,200}, 150) = row 0 (date 100 ≤ 150).
  auto spans_r = reference_spans(ref, price, /*as_of_date=*/150);
  ASSERT_TRUE(spans_r.has_value()) << spans_r.error().message();
  const RefSpans &spans = spans_r.value();

  ASSERT_EQ(spans.market_cap.size(), usize{2});
  ASSERT_EQ(spans.group_id.size(), usize{2});

  // Row 0 of ref (date 100): inst 10 -> mc=1e9, grp=1; inst 20 -> mc=2e9, grp=2.
  EXPECT_DOUBLE_EQ(spans.market_cap[0], 1e9);
  EXPECT_DOUBLE_EQ(spans.market_cap[1], 2e9);
  EXPECT_EQ(spans.group_id[0], u32{1});
  EXPECT_EQ(spans.group_id[1], u32{2});

  // Feed spans into build_components with a valid panel. Use 2 instruments, window
  // rows. The single_sector_cfg emits K=1 sector column; window=4 >= K=1.
  const usize window = 4U;
  const usize n_rows = window + 1U; // one extra row for the oldest step_return
  const PanelFixture fx = make_panel_fixture(n_rows, /*n_inst=*/2);
  const std::vector<u32> group_ids = {spans.group_id[0], spans.group_id[1]};
  const std::vector<f64> mkt_caps = {spans.market_cap[0], spans.market_cap[1]};

  FactorModelBuilder builder;
  builder.cfg = single_sector_cfg();
  auto comp_r = builder.build_components(fx.view(), window,
                                         std::span<const f64>{mkt_caps},
                                         std::span<const u32>{group_ids});
  ASSERT_TRUE(comp_r.has_value()) << comp_r.error().message();
  const FactorComponents &comp = comp_r.value();

  // Valid output: 2 distinct group_ids (1 and 2) -> K=2 sector columns.
  // M=2 (the 2 panel instruments); D length == M.
  EXPECT_EQ(comp.X.rows(), Eigen::Index{2});
  EXPECT_EQ(comp.X.cols(), Eigen::Index{2}); // 2 sector groups
  EXPECT_EQ(comp.F.rows(), Eigen::Index{2});
  EXPECT_EQ(comp.F.cols(), Eigen::Index{2});
  EXPECT_EQ(comp.D.size(), Eigen::Index{2});
  EXPECT_EQ(comp.fit_end, window);

  // Same hand call with the same span values yields byte-identical X/F/D (determinism pin).
  auto comp_hand_r = builder.build_components(fx.view(), window,
                                               std::span<const f64>{mkt_caps},
                                               std::span<const u32>{group_ids});
  ASSERT_TRUE(comp_hand_r.has_value()) << comp_hand_r.error().message();
  const FactorComponents &comp_hand = comp_hand_r.value();

  ASSERT_EQ(comp.X.rows(), comp_hand.X.rows());
  ASSERT_EQ(comp.X.cols(), comp_hand.X.cols());
  for (Eigen::Index r = 0; r < comp.X.rows(); ++r) {
    for (Eigen::Index c = 0; c < comp.X.cols(); ++c) {
      EXPECT_DOUBLE_EQ(comp.X(r, c), comp_hand.X(r, c)) << "X(" << r << "," << c << ")";
    }
  }
  for (Eigen::Index k = 0; k < comp.F.rows(); ++k) {
    for (Eigen::Index l = 0; l < comp.F.cols(); ++l) {
      EXPECT_DOUBLE_EQ(comp.F(k, l), comp_hand.F(k, l)) << "F(" << k << "," << l << ")";
    }
  }
  for (Eigen::Index i = 0; i < comp.D.size(); ++i) {
    EXPECT_DOUBLE_EQ(comp.D[i], comp_hand.D[i]) << "D[" << i << "]";
  }
}

// reference Dataset missing 'group_id' -> reference_spans returns Err(NotFound).
TEST(DataAdaptFactor, ReferenceMissingColumnErrs) {
  // Reference with only "market_cap" (no "group_id").
  DatasetSchema s;
  s.columns = {"market_cap"};
  s.dtypes = {ColumnDType::F64};
  s.role = Role::Reference;
  std::vector<std::vector<f64>> data = {{1e9, 2e9}};
  auto ref_r =
      Dataset::create(std::move(s), {100}, {10u, 20u}, std::move(data), /*mask=*/{},
                      DatasetProvenance{"test:ref_no_grp", ""});
  ASSERT_TRUE(ref_r.has_value()) << ref_r.error().message();
  const Dataset ref = std::move(ref_r).value();
  const Dataset price = make_price_dataset({10u, 20u}, {150});

  auto res = reference_spans(ref, price, /*as_of_date=*/150);
  ASSERT_FALSE(res.has_value()) << "expected NotFound for missing 'group_id' column";
  EXPECT_EQ(res.error().code(), atx::core::ErrorCode::NotFound);
}

// A price instrument absent from the reference -> its market_cap is NaN and
// group_id is default_group.
TEST(DataAdaptFactor, MissingInstrumentGivesNanDefault) {
  // Reference has instrument 10 only; price also has instrument 99 (not in reference).
  const std::vector<InstKey> ref_insts = {10u};
  const std::vector<DateKey> ref_dates = {100};
  const std::vector<f64> mc_flat = {5e8};
  const std::vector<f64> grp_flat = {3.0};
  const Dataset ref = make_reference_dataset(ref_insts, ref_dates, mc_flat, grp_flat);

  // Price has two instruments: 10 (present in ref) and 99 (absent from ref).
  const Dataset price = make_price_dataset({10u, 99u}, {100});

  constexpr u32 kDefaultGroup = 7U;
  auto res = reference_spans(ref, price, /*as_of_date=*/100, kDefaultGroup);
  ASSERT_TRUE(res.has_value()) << res.error().message();
  const RefSpans &spans = res.value();

  ASSERT_EQ(spans.market_cap.size(), usize{2});
  ASSERT_EQ(spans.group_id.size(), usize{2});

  // Instrument 10 (index 0): present -> mc=5e8, grp=3.
  EXPECT_DOUBLE_EQ(spans.market_cap[0], 5e8);
  EXPECT_EQ(spans.group_id[0], u32{3});

  // Instrument 99 (index 1): absent -> NaN mc, default_group grp.
  EXPECT_TRUE(std::isnan(spans.market_cap[1])) << "expected NaN for missing instrument";
  EXPECT_EQ(spans.group_id[1], kDefaultGroup);
}

// build_components with empty market_cap + group_id spans (no reference) still
// produces a valid price-derived FactorComponents — the price-only path is
// unaffected by the S6.6 seams.
//
// Config: Liquidity only (ln(adv20), bit 4 = 0x10), no sector factors, no Size
// (which needs market_cap). Liquidity needs 20 trailing rows for adv20; we supply
// 22 rows (window=21, 1 extra for oldest step_return).
TEST(DataAdaptFactor, NoPlugFallsBackToPriceDerived) {
  // Liquidity = bit 4 in StyleFactor enum (Size=0,Momentum=1,Volatility=2,Beta=3,Liquidity=4).
  constexpr atx::u8 kLiquidityOnly = static_cast<atx::u8>(1U << 4U);
  const usize window = 21U;        // >= 20 rows needed for adv20; also >= K=1
  const usize n_rows = window + 1U; // 1 extra row so the oldest step_return is finite
  const usize n_inst = 3U;
  const PanelFixture fx = make_panel_fixture(n_rows, n_inst);

  FactorModelBuilder builder;
  builder.cfg.sector_factors = false; // no sectors (no group_id needed)
  builder.cfg.style_mask = kLiquidityOnly; // price-derived Liquidity column only

  // Empty market_cap + empty group_id — price-only path.
  auto comp_empty_r = builder.build_components(fx.view(), window,
                                                std::span<const f64>{},
                                                std::span<const u32>{});
  ASSERT_TRUE(comp_empty_r.has_value()) << comp_empty_r.error().message();
  const FactorComponents &comp = comp_empty_r.value();

  // Valid FactorComponents from price-only path: K=1 (Liquidity), M=n_inst.
  EXPECT_GT(comp.X.rows(), Eigen::Index{0});
  EXPECT_EQ(comp.X.cols(), Eigen::Index{1}); // Liquidity -> K=1
  EXPECT_EQ(comp.F.rows(), Eigen::Index{1});
  EXPECT_EQ(comp.F.cols(), Eigen::Index{1});
  EXPECT_EQ(comp.D.size(), comp.X.rows());
  EXPECT_EQ(comp.fit_end, window);

  // build() (the thin wrapper) must also succeed on the price-only path.
  auto model_r = builder.build(fx.view(), window, std::span<const f64>{},
                                std::span<const u32>{});
  ASSERT_TRUE(model_r.has_value()) << model_r.error().message();
  EXPECT_EQ(model_r->n_factors(), static_cast<usize>(comp.X.cols()));
}

} // namespace atxtest_data_adapt_factor_test
