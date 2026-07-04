#include "stage_augment.hpp"

// =============================================================================
//  atx::impl — FINRA short-interest augment core (Track B1; p7 S2-1).
//
//  augment_panel_with_finra appends three CAUSALLY-placed FINRA short-interest
//  features (si_dtc, si_util, si_chg) to a research Panel as NEW fields at the
//  END. It is the PURE, axis-parametric core: given a Panel + a FINRA hive root
//  + an in-memory axis (epoch-day dates + ticker->column map), it derives a
//  shares denominator from the panel's own market_cap/raw_close fields, calls
//  data::load_finra_features, and rebuilds the Panel with the 3 columns
//  appended. Existing FieldIds and column bytes are bitwise-unchanged
//  (append-only invariant). Unit-tested in augment_test.cpp (suite Augment).
//
//  CLI DEFERRAL (p7 decision D1): the run_augment CLI stage — which reconstructs
//  the panel axis from the ORATS seg partition + _symbology.parquet and writes
//  research_aug.bin — is deferred to Sprint 7 (it consumes RunConfig fields that
//  do not exist until the S7 CLI hub lands). This translation unit therefore
//  carries ONLY the pure core; it pulls in no stages.hpp / RunConfig dependency.
// =============================================================================

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/finra_short.hpp"
#include "atx/engine/data/history_panel.hpp" // kHistFieldMarketCap, kHistFieldRawClose

namespace atx::impl {

using atx::core::Err;
using atx::core::ErrorCode;
namespace data = atx::engine::data;
namespace alpha = atx::engine::alpha;

namespace {

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Derive a per-cell shares denominator from the panel: shares = market_cap /
// raw_close where both fields exist and raw_close > 0; NaN otherwise. Returns an
// EMPTY vector (ADV fallback everywhere) if either field is absent.
[[nodiscard]] std::vector<atx::f64> derive_shares(const alpha::Panel &panel) {
  auto mc = panel.field_id(data::kHistFieldMarketCap);
  auto rc = panel.field_id(data::kHistFieldRawClose);
  if (!mc.has_value() || !rc.has_value()) {
    return {};
  }
  const std::span<const atx::f64> mcol = panel.field_all(*mc);
  const std::span<const atx::f64> rcol = panel.field_all(*rc);
  const atx::usize cells = panel.cells();
  std::vector<atx::f64> shares(cells, kNaN);
  for (atx::usize k = 0; k < cells; ++k) {
    const atx::f64 r = rcol[k];
    if (!std::isnan(mcol[k]) && !std::isnan(r) && r > 0.0) {
      shares[k] = mcol[k] / r;
    }
  }
  return shares;
}

} // namespace

atx::core::Result<alpha::Panel>
augment_panel_with_finra(const alpha::Panel &panel, const std::string &finra_root,
                         std::span<const data::DateKey> panel_dates,
                         const std::unordered_map<std::string, data::InstKey> &sym_to_inst,
                         int lag_days) {
  const atx::usize D = panel.dates();
  const atx::usize N = panel.instruments();
  if (panel_dates.size() != D) {
    return Err(ErrorCode::InvalidArgument,
               "augment: panel_dates size does not match panel.dates()");
  }

  // shares denominator from the panel's own market_cap/raw_close (or empty).
  const std::vector<atx::f64> shares = derive_shares(panel);

  ATX_TRY(auto feats, data::load_finra_features(finra_root, panel_dates, sym_to_inst, N,
                                                std::span<const atx::f64>(shares), lag_days));

  // Append the 3 columns at the END. Copy existing fields verbatim first so every
  // existing FieldId/column is bitwise-unchanged (append-only invariant), then add
  // si_dtc, si_util, si_chg.
  const atx::usize nf = panel.num_fields();
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> data_cols;
  names.reserve(nf + 3);
  data_cols.reserve(nf + 3);
  for (atx::usize f = 0; f < nf; ++f) {
    const std::span<const atx::f64> col = panel.field_all(static_cast<alpha::FieldId>(f));
    const std::string_view nm = panel.field_name(static_cast<alpha::FieldId>(f));
    if (nm == data::kFinraFieldDtc || nm == data::kFinraFieldUtil || nm == data::kFinraFieldChg) {
      return Err(ErrorCode::InvalidArgument,
                 std::string{"augment: panel already has a field named '"} + std::string{nm} +
                     "' (re-augment not supported)");
    }
    names.emplace_back(nm);
    data_cols.emplace_back(col.begin(), col.end());
  }
  names.emplace_back(data::kFinraFieldDtc);
  data_cols.push_back(std::move(feats.si_dtc));
  names.emplace_back(data::kFinraFieldUtil);
  data_cols.push_back(std::move(feats.si_util));
  names.emplace_back(data::kFinraFieldChg);
  data_cols.push_back(std::move(feats.si_chg));

  // Carry the universe mask over verbatim.
  std::vector<std::uint8_t> universe(D * N, std::uint8_t{0});
  for (atx::usize d = 0; d < D; ++d) {
    for (atx::usize i = 0; i < N; ++i) {
      universe[d * N + i] = panel.in_universe(d, i) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }

  return alpha::Panel::create(D, N, std::move(names), std::move(data_cols), std::move(universe));
}

} // namespace atx::impl
