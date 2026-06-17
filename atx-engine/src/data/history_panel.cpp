// atx::engine::data — ORATS history Panel assembly (p3 S3-5).
//
// Implements:
//   * orats_total_return_close (S3-3) — thin delegator to adjust_total_return.
//   * build_history_panel (S3-5) — the orchestrator that assembles a
//     deterministic, digest-pinned alpha::Panel from the on-disk ORATS per-date
//     partition. Mirrors the assembly order of real_panel.cpp (S1-5) but sources
//     data from attach_multi_segment_panel instead of a databento parquet hive.

#include "atx/engine/data/history_panel.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/segment_panel.hpp"

#include "atx/engine/data/adjust.hpp"
#include "atx/engine/data/catalog.hpp"
#include "atx/engine/data/corporate_actions.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/data/orats_history.hpp" // kOratsFields (canonical segment names)
#include "atx/engine/data/panel_digest.hpp"
#include "atx/engine/data/universe.hpp"

namespace atx::engine::data {

// ---------------------------------------------------------------------------
//  orats_total_return_close (S3-3)
// ---------------------------------------------------------------------------

std::vector<atx::f64> orats_total_return_close(std::span<const atx::f64> close,
                                               std::span<const atx::f64> cum_return_factor) {
  // Dividends are already folded into cum_return_factor, so the dividend input is 0:
  // adjust_total_return then yields total_return_index == close * cum_return_factor,
  // with the proven gap/NaN policy and return-invariance contract.
  const std::vector<atx::f64> zero_div(close.size(), 0.0);
  AdjustedSeries adj = adjust_total_return(close, cum_return_factor, zero_div);
  return std::move(adj.total_return_index);
}

// ---------------------------------------------------------------------------
//  build_history_panel (S3-5)
// ---------------------------------------------------------------------------

namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Catalog registration names (minimal lineage for the history path).
constexpr std::string_view kDatasetOratsHistory = "orats_history";
constexpr std::string_view kDatasetHistUniverse = "universe";

// The cumulative-return-factor SEGMENT field name (kOratsFields[10]). This is the
// 15-char on-disk name ("cumReturnFactor"); we reference the canonical constant so
// a future rename stays in lockstep with the loader and the static_assert guard.
constexpr std::string_view kFldCumReturnFactor = kOratsFields[10];

} // namespace

atx::core::Result<HistoryPanel> build_history_panel(const HistoryDataConfig &cfg) {
  // -------------------------------------------------------------------------
  // Step 1: Raw panel via attach_multi_segment_panel.
  // The panel's `close` field is the raw as-traded close (pre-cumret).
  // D = dates, N = instruments.
  // Use empty fields → auto-discover from the first in-window segment.
  // -------------------------------------------------------------------------
  ATX_TRY(auto raw, alpha::attach_multi_segment_panel(cfg.seg_dir, cfg.window));
  const atx::usize D = raw.dates();
  const atx::usize N = raw.instruments();

  if (D == 0 || N == 0) {
    return Err(ErrorCode::InvalidArgument,
               "build_history_panel: empty window or no instruments in partition");
  }

  // Resolve field IDs we need from the raw panel.
  ATX_TRY(auto close_fid,  raw.field_id("close"));
  ATX_TRY(auto cumret_fid, raw.field_id(kFldCumReturnFactor));
  ATX_TRY(auto volume_fid, raw.field_id("volume"));
  ATX_TRY(auto high_fid,   raw.field_id("high"));
  ATX_TRY(auto low_fid,    raw.field_id("low"));
  ATX_TRY(auto open_fid,   raw.field_id("open"));
  ATX_TRY(auto shares_fid, raw.field_id("shares"));
  ATX_TRY(auto gics_fid,   raw.field_id("gics"));

  const std::span<const atx::f64> rc = raw.field_all(close_fid);
  const std::span<const atx::f64> cr = raw.field_all(cumret_fid);

  // -------------------------------------------------------------------------
  // Step 2: Build an axis-matched 6-column Reference corp Dataset.
  // Positional DateKey/InstKey 0..D-1 / 0..N-1; build_universe matches by
  // count/position, so synthetic ascending keys are correct.
  // Canonical 6-column order: cum_adj_factor, cash_dividend, shares_outstanding,
  // shares_filed_date, gics_sector_code, sic_code.
  // -------------------------------------------------------------------------
  const atx::usize cells = D * N;

  // cum_adj_factor: panel's cumulReturnFactor column.
  std::vector<atx::f64> col_caf(cr.begin(), cr.end());

  // cash_dividend: 0 (dividends already folded into cumulReturnFactor by ORATS).
  std::vector<atx::f64> col_div(cells, 0.0);

  // shares_outstanding: panel's shares column.
  const std::span<const atx::f64> shares_span = raw.field_all(shares_fid);
  std::vector<atx::f64> col_shares(shares_span.begin(), shares_span.end());

  // shares_filed_date: kNoDate sentinel (ORATS row is already PIT-as-published).
  std::vector<atx::f64> col_filed(cells, static_cast<atx::f64>(kNoDate));

  // gics_sector_code: panel's gics column (NaN -> kNoSector = -1).
  const std::span<const atx::f64> gics_span = raw.field_all(gics_fid);
  std::vector<atx::f64> col_gics(cells);
  for (atx::usize k = 0; k < cells; ++k) {
    col_gics[k] = std::isnan(gics_span[k]) ? kNoSector : gics_span[k];
  }

  // sic_code: kNoSector sentinel.
  std::vector<atx::f64> col_sic(cells, kNoSector);

  // Build the Reference-role corp Dataset.
  DatasetSchema corp_schema = corp_action_schema();
  std::vector<DateKey> corp_dates(D);
  for (atx::usize d = 0; d < D; ++d) {
    corp_dates[d] = static_cast<DateKey>(d);
  }
  std::vector<InstKey> corp_insts(N);
  for (atx::usize i = 0; i < N; ++i) {
    corp_insts[i] = static_cast<InstKey>(i);
  }
  std::vector<std::vector<atx::f64>> corp_cols = {
      std::move(col_caf),
      std::move(col_div),
      std::move(col_shares),
      std::move(col_filed),
      std::move(col_gics),
      std::move(col_sic),
  };
  DatasetProvenance corp_prov{"derived:orats_history_corp", "p3 S3-5 axis-matched corp-actions"};
  ATX_TRY(auto corp_ds,
          Dataset::create(std::move(corp_schema), corp_dates, corp_insts,
                          std::move(corp_cols), /*mask=*/{}, std::move(corp_prov)));

  // -------------------------------------------------------------------------
  // Step 3: build_universe on the RAW panel.
  // market_cap = shares * raw close, ADV recomputed causally.
  // -------------------------------------------------------------------------
  ATX_TRY(auto uni, build_universe(raw, corp_ds, cfg.universe));

  // -------------------------------------------------------------------------
  // Step 4: TRI close — per-instrument stride-N gather/scatter.
  // cell = d * N + i  (date-major).
  // -------------------------------------------------------------------------
  std::vector<atx::f64> close_tri(cells);
  std::vector<atx::f64> one_close(D), one_cr(D);
  for (atx::usize i = 0; i < N; ++i) {
    for (atx::usize d = 0; d < D; ++d) {
      one_close[d] = rc[d * N + i];
      one_cr[d]    = cr[d * N + i];
    }
    std::vector<atx::f64> one_tri = orats_total_return_close(one_close, one_cr);
    for (atx::usize d = 0; d < D; ++d) {
      close_tri[d * N + i] = one_tri[d];
    }
  }

  // -------------------------------------------------------------------------
  // Step 5: Assemble final Panel in kHistField* order.
  // close  = TRI, raw_close = raw close, volume, high, low, open,
  // market_cap, sector (widened to f64). Mask = in_universe.
  // -------------------------------------------------------------------------
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> data;
  names.reserve(8);
  data.reserve(8);

  // close = TRI
  names.emplace_back(kHistFieldClose);
  data.push_back(std::move(close_tri));

  // raw_close = raw as-traded close
  names.emplace_back(kHistFieldRawClose);
  data.push_back(std::vector<atx::f64>(rc.begin(), rc.end()));

  // volume
  {
    const std::span<const atx::f64> s = raw.field_all(volume_fid);
    names.emplace_back(kHistFieldVolume);
    data.push_back(std::vector<atx::f64>(s.begin(), s.end()));
  }

  // high
  {
    const std::span<const atx::f64> s = raw.field_all(high_fid);
    names.emplace_back(kHistFieldHigh);
    data.push_back(std::vector<atx::f64>(s.begin(), s.end()));
  }

  // low
  {
    const std::span<const atx::f64> s = raw.field_all(low_fid);
    names.emplace_back(kHistFieldLow);
    data.push_back(std::vector<atx::f64>(s.begin(), s.end()));
  }

  // open
  {
    const std::span<const atx::f64> s = raw.field_all(open_fid);
    names.emplace_back(kHistFieldOpen);
    data.push_back(std::vector<atx::f64>(s.begin(), s.end()));
  }

  // market_cap
  names.emplace_back(kHistFieldMarketCap);
  data.push_back(uni.market_cap);

  // sector — widen i32 sector_code to f64
  {
    std::vector<atx::f64> sector_f64(cells, kNaN);
    for (atx::usize k = 0; k < cells; ++k) {
      sector_f64[k] = static_cast<atx::f64>(uni.sector_code[k]);
    }
    names.emplace_back(kHistFieldSector);
    data.push_back(std::move(sector_f64));
  }

  ATX_TRY(auto final_panel,
          alpha::Panel::create(D, N, std::move(names), std::move(data),
                               std::vector<std::uint8_t>(uni.in_universe.begin(),
                                                         uni.in_universe.end())));

  // -------------------------------------------------------------------------
  // Step 6: Catalog lineage + digest.
  // -------------------------------------------------------------------------
  DatasetCatalog catalog;

  // Build tiny 1-cell Reference datasets as lineage records (we only need names).
  const std::vector<DateKey>              ld   = {DateKey{0}};
  const std::vector<InstKey>              li   = {InstKey{0}};
  const std::vector<std::vector<atx::f64>> ld1 = {{0.0}};

  // Register the history price record.
  DatasetSchema hist_schema;
  hist_schema.columns = {std::string{kDatasetOratsHistory}};
  hist_schema.dtypes  = {ColumnDType::F64};
  hist_schema.role    = Role::Reference;
  ATX_TRY(auto hist_ds,
          Dataset::create(hist_schema, ld, li, ld1, {},
                          DatasetProvenance{"derived:orats_history", "p3 S3-5 lineage"}));
  ATX_TRY_VOID(catalog.register_dataset(std::string{kDatasetOratsHistory}, std::move(hist_ds)));

  // Register a universe record.
  DatasetSchema uni_schema;
  uni_schema.columns = {std::string{kDatasetHistUniverse}};
  uni_schema.dtypes  = {ColumnDType::F64};
  uni_schema.role    = Role::Reference;
  ATX_TRY(auto uni_ds,
          Dataset::create(uni_schema, ld, li, ld1, {},
                          DatasetProvenance{"derived:orats_universe", "p3 S3-5 lineage"}));
  ATX_TRY_VOID(catalog.register_dataset(std::string{kDatasetHistUniverse}, std::move(uni_ds)));

  // Record derivation lineage.
  ATX_TRY_VOID(catalog.derive(std::string{kDatasetHistUniverse},
                               {std::string{kDatasetOratsHistory}}));

  const atx::u64 digest = digest_panel(final_panel);
  HistoryPanel result{std::move(final_panel), digest, catalog.names()};
  return Ok(std::move(result));
}

} // namespace atx::engine::data
