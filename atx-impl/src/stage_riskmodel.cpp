#include "stage_riskmodel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/combine/store.hpp"     // combine::AlphaId
#include "atx/engine/library/library.hpp"   // library::Library (S1-4, dead-alpha holdings source)
#include "atx/engine/loop/panel_types.hpp"  // PanelView, PanelField, kPanelFieldCount
#include "atx/engine/loop/types.hpp"        // InstrumentId
#include "atx/engine/risk/dead_factor.hpp"  // extract_dead_factors, augment_factor_model (S1-4)
#include "atx/engine/risk/exposures.hpp"    // FactorModelConfig, StyleFactor

#include "diag_risk.hpp"

namespace atx::impl {

namespace risk = atx::engine::risk;
namespace data = atx::engine::data;
namespace alpha = atx::engine::alpha;
namespace library = atx::engine::library;
namespace combine = atx::engine::combine;

namespace {

// ---------------------------------------------------------------------------
// PanelWindowView — owns a small (fields_, mask_) buffer covering
// [fit_end - total_rows, fit_end) of a research alpha::Panel, and exposes it
// as an atx::engine::PanelView (newest-first, row 0 == fit_end-1) for
// risk::FactorModelBuilder::build_components. See stage_riskmodel.hpp's
// header doc for why this bridge is necessary (no existing Panel->PanelView
// adapter; PanelView's owner, RollingPanel, is a live-loop construct unrelated
// to this batch pipeline). Mirrors risk_factor_builder_test.cpp's PanelFixture
// pattern (same cap_/mask_words_/physical-row addressing), which is the
// codebase's only precedent for constructing a PanelView from raw data.
//
// `total_rows` is DELIBERATELY LARGER than the estimation window S1 passes to
// build_components: build_components regresses one cross-section per date
// s in [0, window), and EACH date's build_exposures(row=s) needs its own
// trailing style lookback (Beta needs row+253 rows, Momentum row+252, Vol
// row+60) -- so the view must carry `window + kMaxStyleLookback` rows total,
// or the OLDEST estimation dates silently degrade to zero usable style
// columns (see risk_factor_builder_test.cpp's own "sectors only -> no
// per-instrument lookback needed" comment, the precedent for this
// constraint). build_risk_model computes total_rows accordingly (see call
// site) and this class is a dumb buffer -- it does not enforce that math.
//
// PIT: only rows in [fit_end - total_rows, fit_end) of `research` are ever
// read -- rows >= fit_end never enter the buffer, so no future bar can leak
// into the estimator (the S1-1 PIT guard test perturbs rows >= fit_end and
// expects no change).
// ---------------------------------------------------------------------------
class PanelWindowView {
public:
  PanelWindowView(const alpha::Panel& research, atx::usize fit_end, atx::usize total_rows) {
    const atx::usize close_fid = *research.field_id("close");
    const bool have_volume = research.field_id("volume").has_value();
    const atx::usize volume_fid = have_volume ? *research.field_id("volume") : 0;

    n_inst_ = research.instruments();
    const atx::usize win_begin = (fit_end > total_rows) ? (fit_end - total_rows) : 0U;
    n_rows_ = fit_end - win_begin;
    cap_ = pow2_ceil(n_rows_);
    mask_words_ = (n_inst_ + 63U) / 64U;

    universe_.reserve(n_inst_);
    for (atx::usize i = 0; i < n_inst_; ++i) {
      universe_.push_back(atx::engine::InstrumentId{static_cast<atx::u32>(i + 1U)});
    }

    constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
    fields_.assign(atx::engine::kPanelFieldCount * cap_ * n_inst_, kNaN);
    mask_.assign(cap_ * mask_words_, 0ULL);

    const auto close_col = research.field_all(static_cast<alpha::FieldId>(close_fid));
    for (atx::usize r = 0; r < n_rows_; ++r) {
      // Reverse date-major (win_begin..fit_end, ascending date) into
      // newest-first (row 0 == fit_end-1, the most recent date in the window).
      // PHYSICAL ADDRESSING (must match PanelView::physical_row exactly, NOT
      // phys=r): with head_() == n_rows_-1, PanelView maps row_from_newest=0
      // to phys=(head_+cap_-0)&(cap_-1)=head_=n_rows_-1, and row_from_newest=k
      // to phys=n_rows_-1-k -- i.e. physical rows run OLDEST-first ascending,
      // newest at the highest physical index. Mirrors
      // risk_factor_builder_test.cpp's PanelFixture exactly (its `phys =
      // (n_rows_-1) - r` for newest-first r). Using phys=r here (this bridge's
      // original bug) silently wrapped every row but the first to the FAR END
      // of the ring, reading uninitialized NaN cells and starving every
      // cross-section but the very first.
      const atx::usize date = fit_end - 1U - r;
      const atx::usize phys = (n_rows_ - 1U) - r;
      for (atx::usize i = 0; i < n_inst_; ++i) {
        const bool in_universe = research.in_universe(date, i);
        const atx::f64 c = in_universe ? close_col[date * n_inst_ + i] : kNaN;
        const atx::f64 v = have_volume && in_universe
                               ? research.field_all(static_cast<alpha::FieldId>(volume_fid))
                                     [date * n_inst_ + i]
                               : kNaN;
        set(atx::engine::PanelField::Open, phys, i, c);
        set(atx::engine::PanelField::High, phys, i, c);
        set(atx::engine::PanelField::Low, phys, i, c);
        set(atx::engine::PanelField::Close, phys, i, c);
        set(atx::engine::PanelField::Volume, phys, i, v);
        if (in_universe && !std::isnan(c)) {
          mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
        }
      }
    }
  }

  [[nodiscard]] atx::engine::PanelView view() const noexcept {
    return atx::engine::PanelView{fields_.data(),
                                  mask_.data(),
                                  std::span<const atx::engine::InstrumentId>{universe_},
                                  cap_,
                                  head_(),
                                  n_rows_,
                                  mask_words_};
  }

private:
  // The newest row's physical index (matches PanelFixture's head_()): with
  // physical rows filled OLDEST-first ascending (phys = n_rows_-1-r above),
  // the newest row (r=0) lands at phys = n_rows_-1.
  [[nodiscard]] atx::usize head_() const noexcept { return (n_rows_ == 0U) ? 0U : n_rows_ - 1U; }

  static atx::usize pow2_ceil(atx::usize n) noexcept {
    atx::usize p = 1U;
    while (p < n) p <<= 1U;
    return p == 0U ? 1U : p;
  }

  void set(atx::engine::PanelField f, atx::usize phys, atx::usize inst, atx::f64 v) noexcept {
    const atx::usize block = static_cast<atx::usize>(f) * cap_ * n_inst_;
    fields_[block + phys * n_inst_ + inst] = v;
  }

  atx::usize n_rows_ = 0;
  atx::usize n_inst_ = 0;
  atx::usize cap_ = 1;
  atx::usize mask_words_ = 0;
  std::vector<atx::engine::InstrumentId> universe_;
  std::vector<atx::f64> fields_;
  std::vector<atx::u64> mask_;
};

// Translate RiskModelConfig's style toggles into a FactorModelConfig style_mask.
// style_size maps to StyleFactor::Liquidity (log dollar-ADV -- see
// stage_riskmodel.hpp's field-mapping note: the panel carries no market_cap,
// so true Size is never emitted regardless of the mask; Liquidity IS the
// panel-derivable size proxy the sprint brief names).
[[nodiscard]] risk::FactorModelConfig make_factor_model_config(const risk::RiskModelConfig& cfg) {
  risk::FactorModelConfig fmc;
  atx::u8 mask = 0;
  if (cfg.style_mom) mask |= static_cast<atx::u8>(1U << static_cast<atx::u8>(risk::StyleFactor::Momentum));
  if (cfg.style_vol) mask |= static_cast<atx::u8>(1U << static_cast<atx::u8>(risk::StyleFactor::Volatility));
  if (cfg.style_beta) mask |= static_cast<atx::u8>(1U << static_cast<atx::u8>(risk::StyleFactor::Beta));
  if (cfg.style_size) mask |= static_cast<atx::u8>(1U << static_cast<atx::u8>(risk::StyleFactor::Liquidity));
  fmc.style_mask = mask;
  fmc.sector_factors = cfg.industry;
  return fmc;
}

} // namespace

namespace {

// Build the BASE (pre-dead-factor-augmentation) FactorComponents per
// cfg.kind. Diagonal delegates to diagonal_risk_model unchanged (its
// (X, F, D, fit_end) reassembled as FactorComponents so the S1-4 augmentation
// step composes with EITHER kind uniformly); Factor runs the real estimator.
[[nodiscard]] atx::core::Result<risk::FactorComponents>
build_base_components(const alpha::Panel& research, const risk::RiskModelConfig& cfg,
                      std::span<const atx::u32> group_id) {
  if (cfg.kind == risk::RiskModelKind::Diagonal) {
    ATX_TRY(auto model, diagonal_risk_model(research));
    return atx::core::Ok(risk::FactorComponents{
        model.exposures(), model.factor_cov(), model.specific_var(), model.fit_end()});
  }

  // Factor path.
  if (!research.field_id("close").has_value()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "build_risk_model: research panel has no 'close' field");
  }
  const atx::usize n_inst = research.instruments();
  if (!group_id.empty() && group_id.size() != n_inst) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "build_risk_model: group_id span length must equal research.instruments()");
  }

  const atx::usize fit_end = research.dates();
  if (fit_end < 2U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "build_risk_model: research panel needs >= 2 dates for the Factor path");
  }

  // Deepest per-date style lookback among the columns THIS cfg actually
  // emits (see risk/exposures.hpp's detail:: constants: kMomLong=252,
  // kBetaWindow=252 [+1 row], kVolWindow=60, kAdvWindow=20; sector dummies
  // need none). The PanelView buffer must carry
  // `estimation_window + deepest_lookback` rows so build_components's OLDEST
  // estimation date (row = estimation_window - 1) still has a full trailing
  // lookback available for every emitted column; otherwise every date beyond
  // the first few silently drops to an under-determined cross-section
  // (M_s < K) and the WLS pass starves ("too few usable dates"). Computed
  // from cfg (not a fixed worst-case) so a Vol-only / sectors-only config
  // does not force a multi-thousand-row buffer just because Beta COULD have
  // been enabled.
  atx::usize deepest_lookback = 0U; // sectors-only / all-style-off -> 0 extra rows needed
  if (cfg.style_mom) deepest_lookback = std::max(deepest_lookback, atx::usize{252});
  if (cfg.style_beta) deepest_lookback = std::max(deepest_lookback, atx::usize{253});
  if (cfg.style_vol) deepest_lookback = std::max(deepest_lookback, atx::usize{60});
  if (cfg.style_size) deepest_lookback = std::max(deepest_lookback, atx::usize{20}); // Liquidity proxy

  const atx::usize estimation_window =
      std::min(static_cast<atx::usize>(cfg.fit_lookback_days), fit_end - 1U);
  const atx::usize total_rows = estimation_window + deepest_lookback;

  const PanelWindowView window(research, fit_end, total_rows);
  const atx::engine::PanelView view = window.view();

  risk::FactorModelBuilder builder;
  builder.cfg = make_factor_model_config(cfg);

  const std::span<const atx::f64> no_market_cap; // empty: Size never fabricated (see header note)
  const std::span<const atx::u32> group_span = cfg.industry ? group_id : std::span<const atx::u32>{};

  return builder.build_components(view, estimation_window, no_market_cap, group_span);
}

} // namespace

atx::core::Result<data::FactorModelArtifact>
build_risk_model(const alpha::Panel& research, const risk::RiskModelConfig& cfg,
                  std::span<const atx::u32> group_id, const library::Library* dead_lib,
                  std::span<const combine::AlphaId> dead_ids, atx::usize dead_as_of) {
  ATX_TRY(risk::FactorComponents comp, build_base_components(research, cfg, group_id));

  // S1-4: dead-alpha crowding-factor augmentation (opt-in, composes with
  // EITHER kind). FAIL-OPEN: no library / no dead ids -> the base model
  // passes through unaugmented (documented no-op, not an Err -- see the
  // header doc's guardrail note).
  if (cfg.dead_alpha_factors && dead_lib != nullptr && !dead_ids.empty()) {
    const atx::usize universe_size = static_cast<atx::usize>(comp.X.rows());
    ATX_TRY(risk::DeadAlphaFactors dead,
            risk::extract_dead_factors(*dead_lib, dead_ids, dead_as_of, universe_size));
    if (dead.k_dead > 0U) {
      ATX_TRY(risk::FactorModel augmented, risk::augment_factor_model(comp, dead));
      data::FactorModelArtifact art;
      art.X = augmented.exposures();
      art.F = augmented.factor_cov();
      art.D = augmented.specific_var();
      art.fit_begin = augmented.fit_begin();
      art.fit_end = augmented.fit_end();
      return atx::core::Ok(std::move(art));
    }
    // k_dead == 0 (e.g. an empty-overlap dead pool): fall through to the
    // unaugmented artifact below -- augment_factor_model's own passthrough
    // convention, mirrored here rather than calling it (avoids a redundant
    // FactorModel::create round-trip when there is nothing to add).
  }

  data::FactorModelArtifact art;
  art.X = std::move(comp.X);
  art.F = std::move(comp.F);
  art.D = std::move(comp.D);
  art.fit_begin = 0U; // PanelView-local window bound (matches FactorModel::create's own convention)
  art.fit_end = comp.fit_end;
  return atx::core::Ok(std::move(art));
}

} // namespace atx::impl
