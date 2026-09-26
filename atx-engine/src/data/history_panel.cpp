// atx::engine::data — historical Panel assembly (legacy ORATS API names).
//
// Implements:
//   * orats_total_return_close — pointwise cumulative-factor price adjustment.
//   * build_history_panel (S3-5) — the orchestrator that assembles a
//     deterministic, digest-pinned alpha::Panel from the on-disk ORATS per-date
//     partition. Mirrors the assembly order of real_panel.cpp (S1-5) but sources
//     data from attach_multi_segment_panel instead of a databento parquet hive.

#include "atx/engine/data/history_panel.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/augment.hpp"    // DollarVolumeBasis
#include "atx/engine/alpha/datafields.hpp" // parse_adv_field
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/segment_panel.hpp"

#include "atx/engine/data/catalog.hpp"
#include "atx/engine/data/corporate_actions.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/data/orats_history.hpp" // kOratsFields (canonical segment names)
#include "atx/engine/data/panel_digest.hpp"
#include "atx/engine/data/universe.hpp"

namespace atx::engine::data {

namespace {

core::Result<std::vector<std::string>> bounded_history_paths(const std::string& directory) {
  std::vector<std::string> paths; std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(directory, ec)) {
    if (e.path().extension() != ".seg") continue;
    if (paths.size() == 10000 || e.path().string().size() > 4096)
      return core::Err(core::ErrorCode::InvalidArgument, "history: source file/path bound");
    paths.push_back(e.path().string());
  }
  if (ec || paths.empty()) return core::Err(core::ErrorCode::IoError, "history: no readable segment paths");
  std::sort(paths.begin(), paths.end()); return core::Ok(std::move(paths));
}

core::Result<alpha::IndexedPanel> fixed_history_panel(const HistoryDataConfig& cfg) {
  const auto& ids = cfg.fixed_axis_ids;
  if (ids.empty() || ids.size() > 100000 || cfg.compact_to_universe || !cfg.allow_ids.empty())
    return core::Err(core::ErrorCode::InvalidArgument, "history: fixed union forbids compaction/allow-list");
  for (atx::usize i = 0; i < ids.size(); ++i)
    if (ids[i] <= 0 || (i && ids[i - 1] >= ids[i]))
      return core::Err(core::ErrorCode::InvalidArgument, "history: fixed IDs must be sorted unique positive");
  ATX_TRY(auto dates, history_session_keys(cfg.seg_dir, cfg.window));
  ATX_TRY(auto paths, bounded_history_paths(cfg.seg_dir));
  atx::u64 largest_mapping = 0;
  for (const auto& path : paths) {
    std::error_code ec; const auto bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes > 256ULL * 1024 * 1024)
      return core::Err(core::ErrorCode::InvalidArgument, "history: source mapping budget exceeded");
    largest_mapping = std::max(largest_mapping, static_cast<atx::u64>(bytes));
  }
  const auto n = ids.size(); const auto cells64 = atx::u64{dates.size()} * n;
  // Conservative concurrent raw/corp/derived/panel copies plus cold metadata.
  // This is an admission estimate, not a measured process RSS guarantee.
  const atx::u64 fixed_bytes = 64ULL * 1024 * 1024 + largest_mapping;
  if (cells64 > (std::numeric_limits<atx::usize>::max)() || cfg.max_working_bytes < fixed_bytes ||
      cells64 > (cfg.max_working_bytes - fixed_bytes) / 512)
    return core::Err(core::ErrorCode::InvalidArgument, "history: fixed union working-memory budget exceeded");
  const auto cells = static_cast<atx::usize>(cells64);
  std::vector<std::string> fields(kOratsFields.begin(), kOratsFields.end()), names;
  for (auto id : ids) names.push_back(std::to_string(id));
  std::vector<std::vector<atx::f64>> values(fields.size());
  for (auto& column : values) column.assign(cells, std::numeric_limits<atx::f64>::quiet_NaN());
  std::vector<atx::u8> present(cells, 0);
  std::vector<std::string> selected;
  for (const auto& path : paths) {
    std::error_code ec; const auto bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes > cfg.max_working_bytes / 2)
      return core::Err(core::ErrorCode::InvalidArgument, "history: source mapping budget exceeded");
    ATX_TRY(auto reader, atx::tsdb::SegmentReader::attach(path, largest_mapping));
    const auto times = reader.times();
    auto lo = std::lower_bound(times.begin(), times.end(), cfg.window.start_nanos);
    auto hi = std::lower_bound(times.begin(), times.end(), cfg.window.end_nanos);
    if (lo == hi) continue;
    if (reader.instrument_count() > 100000)
      return core::Err(core::ErrorCode::InvalidArgument, "history: source instrument metadata bound");
    selected.push_back(path);
    std::vector<atx::u32> field_ids; field_ids.reserve(fields.size());
    for (const auto& f : fields) {
      const auto found = reader.field_index(f);
      if (!found) return core::Err(core::ErrorCode::NotFound, "history: source field absent: " + f);
      field_ids.push_back(*found);
    }
    std::vector<atx::i64> source_ids; source_ids.reserve(reader.instrument_count());
    for (atx::u32 j = 0; j < reader.instrument_count(); ++j) {
      const auto name = reader.symbol_name(j); atx::i64 id{};
      const auto parsed = std::from_chars(name.data(), name.data() + name.size(), id);
      if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || id <= 0 || std::to_string(id) != name)
        return core::Err(core::ErrorCode::InvalidArgument, "history: noncanonical source ID");
      source_ids.push_back(id);
      const auto member = std::lower_bound(ids.begin(), ids.end(), id);
      if (member == ids.end() || *member != id) continue;
      const auto col = static_cast<atx::usize>(member - ids.begin());
      for (auto it = lo; it != hi; ++it) {
        const auto local = static_cast<atx::usize>(it - times.begin());
        if (!reader.present(local, j)) continue;
        const auto date = static_cast<atx::usize>(std::lower_bound(dates.begin(), dates.end(), *it) - dates.begin());
        const auto cell = date * n + col;
        if (present[cell]) return core::Err(core::ErrorCode::InvalidArgument, "history: duplicate fixed-axis cell");
        present[cell] = 1;
        for (atx::usize f = 0; f < fields.size(); ++f) values[f][cell] = reader.value(field_ids[f], local, j);
      }
    }
    std::sort(source_ids.begin(), source_ids.end());
    if (std::adjacent_find(source_ids.begin(), source_ids.end()) != source_ids.end())
      return core::Err(core::ErrorCode::InvalidArgument, "history: duplicate source instrument identity");
  }
  ATX_TRY(auto panel, alpha::Panel::create(dates.size(), n, std::move(fields), std::move(values), std::move(present)));
  return core::Ok(alpha::IndexedPanel{std::move(panel), std::move(dates), std::move(names), std::move(selected)});
}

// The source cumulative factor already includes splits and cash distributions.
// Apply it once to every research price; chaining ratios adds avoidable rounding
// and can put close on a slightly different scale from the same row's O/H/L.
[[nodiscard]] std::vector<atx::f64>
adjusted_history_prices(std::span<const atx::f64> prices,
                        std::span<const atx::f64> factors) {
  if (prices.size() != factors.size()) {
    return {};
  }
  std::vector<atx::f64> adjusted(prices.size(), std::numeric_limits<atx::f64>::quiet_NaN());
  for (atx::usize i = 0; i < prices.size(); ++i) {
    if (!std::isfinite(prices[i]) || prices[i] <= 0.0 ||
        !std::isfinite(factors[i]) || factors[i] <= 0.0) {
      continue;
    }
    const atx::f64 value = prices[i] * factors[i];
    if (std::isfinite(value) && value > 0.0) {
      adjusted[i] = value;
    }
  }
  return adjusted;
}

} // namespace

core::Result<std::vector<atx::i64>> history_session_keys(const std::string& seg_dir, alpha::TimeWindow window) {
  if (window.start_nanos >= window.end_nanos || window.start_nanos <= 0 ||
      window.end_nanos > 1'577'836'800'000'000'000LL)
    return core::Err(core::ErrorCode::InvalidArgument, "history: bounded path requires explicit pre-2020 window");
  ATX_TRY(auto paths, bounded_history_paths(seg_dir));
  std::vector<atx::i64> dates;
  for (const auto& path : paths) {
    std::error_code ec; const auto extent = std::filesystem::file_size(path, ec);
    if (ec || extent > 256ULL * 1024 * 1024)
      return core::Err(core::ErrorCode::InvalidArgument, "history: source metadata mapping limit");
    ATX_TRY(auto reader, atx::tsdb::SegmentReader::attach(path, 256ULL * 1024 * 1024));
    const auto times = reader.times();
    for (atx::usize t = 0; t < times.size(); ++t) {
      if (times[t] <= 0 || times[t] >= 1'577'836'800'000'000'000LL || (t && times[t - 1] >= times[t]))
        return core::Err(core::ErrorCode::InvalidArgument, "history: unsealed/nonascending source session axis");
      if (times[t] < window.start_nanos || times[t] >= window.end_nanos) continue;
      if (dates.size() == 100000) return core::Err(core::ErrorCode::InvalidArgument, "history: session metadata bound");
      dates.push_back(times[t]);
    }
  }
  std::sort(dates.begin(), dates.end()); dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
  if (dates.empty()) return core::Err(core::ErrorCode::InvalidArgument, "history: empty selected session axis");
  return core::Ok(std::move(dates));
}

// ---------------------------------------------------------------------------
//  history_field_level_basis (W0-D0, D-01)
// ---------------------------------------------------------------------------

std::optional<LevelBasis> history_field_level_basis(std::string_view name) noexcept {
  // Research prices on the snapshot-factor basis contain future corporate actions.
  // Legacy VWAP is handled by the explicit-rule overload below.
  static constexpr std::array<std::string_view, 4> kAdjusted{
      kHistFieldClose, kHistFieldHigh, kHistFieldLow, kHistFieldOpen};
  // As-traded / as-published levels, counts and category codes.
  static constexpr std::array<std::string_view, 13> kRaw{
      kHistFieldRawClose,   kHistFieldVolume,   kHistFieldMarketCap, kHistFieldSector,
      kHistFieldEarnFlag,   kHistFieldEarnCnt5, "cap",               "IndClass.sector",
      "IndClass.industry",  "IndClass.subindustry", "dollar_volume", "shares", "vwap"};
  // Dimensionless quantities.
  static constexpr std::array<std::string_view, 10> kRatio{
      kHistFieldAtmIv21, kHistFieldAtmIv126, "returns", "iv_term", "iv_vrp",
      "iv_lo",           "illiq",            "si_dtc",  "si_util", "si_chg"};
  for (const std::string_view n : kAdjusted) {
    if (n == name) {
      return LevelBasis::AdjustedLevel;
    }
  }
  for (const std::string_view n : kRaw) {
    if (n == name) {
      return LevelBasis::Raw;
    }
  }
  for (const std::string_view n : kRatio) {
    if (n == name) {
      return LevelBasis::Ratio;
    }
  }
  atx::u16 window = 0;
  if (alpha::datafields::parse_adv_field(name, window)) {
    return LevelBasis::Raw; // ts_mean of the raw dollar_volume
  }
  constexpr std::string_view kRegimePrefix = "regime_";
  if (name.size() > kRegimePrefix.size() && name.substr(0, kRegimePrefix.size()) == kRegimePrefix) {
    return LevelBasis::Raw; // macro series broadcast as published
  }
  return std::nullopt;
}

std::optional<LevelBasis> history_field_level_basis(std::string_view name,
                                                  alpha::DollarVolumeBasis dv_basis,
                                                  alpha::VwapRule vwap_rule) noexcept {
  if (name == "vwap") {
    if (vwap_rule == alpha::VwapRule::RawDailyCloseV2) return LevelBasis::Raw;
    if (vwap_rule == alpha::VwapRule::AdjustedTypicalV1) return LevelBasis::AdjustedLevel;
    return std::nullopt;
  }
  const std::optional<LevelBasis> by_name = history_field_level_basis(name);
  if (dv_basis == alpha::DollarVolumeBasis::RawCloseV2 || !by_name.has_value()) {
    return by_name;
  }
  // CloseV1 (or an unknown enum value — fail closed): liquidity is close x volume,
  // and close on a history panel carries the snapshot factor.
  atx::u16 window = 0;
  if (name == alpha::datafields::kDollarVolume ||
      alpha::datafields::parse_adv_field(name, window)) {
    return LevelBasis::AdjustedLevel;
  }
  return by_name;
}

// ---------------------------------------------------------------------------
//  orats_total_return_close (S3-3)
// ---------------------------------------------------------------------------

std::vector<atx::f64> orats_total_return_close(std::span<const atx::f64> close,
                                               std::span<const atx::f64> cum_return_factor) {
  return adjusted_history_prices(close, cum_return_factor);
}

// ---------------------------------------------------------------------------
//  build_history_panel (S3-5)
// ---------------------------------------------------------------------------

namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

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
  // Request the curated 16 fields explicitly (the on-disk SEGMENT names) so the
  // attach fails fast if a required field is missing and never silently admits
  // an unexpected extra segment column.
  // -------------------------------------------------------------------------
  const std::vector<std::string> want_fields(kOratsFields.begin(), kOratsFields.end());
  ATX_TRY(auto indexed,
          cfg.fixed_axis_ids.empty()
              ? alpha::attach_indexed_multi_segment_panel(cfg.seg_dir, cfg.window, want_fields)
              : fixed_history_panel(cfg));
  auto raw = std::move(indexed.panel);
  const atx::usize D = raw.dates();
  const atx::usize N = raw.instruments();

  if (D == 0 || N == 0) {
    return Err(ErrorCode::InvalidArgument,
               "build_history_panel: empty window or no instruments in partition");
  }
  // Canonical source identity is independent of local dense engine indices.
  // Reject aliases such as "0012", rather than letting them join as a second ID.
  for (const auto &id : indexed.instrument_ids) {
    atx::i64 parsed{};
    const auto result = std::from_chars(id.data(), id.data() + id.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != id.data() + id.size() ||
        parsed <= 0 || std::to_string(parsed) != id) {
      return Err(ErrorCode::InvalidArgument,
                 "build_history_panel: noncanonical positive i64 securityID '" + id + "'");
    }
  }
  if (N - 1 > std::numeric_limits<InstKey>::max()) {
    return Err(ErrorCode::InvalidArgument,
               "build_history_panel: too many instruments for local dense indices");
  }

  // Resolve field IDs we need from the raw panel.
  ATX_TRY(auto close_fid,  raw.field_id("close"));
  ATX_TRY(auto cumret_fid, raw.field_id(kFldCumReturnFactor));
  ATX_TRY(auto volume_fid, raw.field_id("volume"));
  ATX_TRY(auto high_fid,   raw.field_id("high"));
  ATX_TRY(auto low_fid,    raw.field_id("low"));
  ATX_TRY(auto open_fid,   raw.field_id("open"));
  ATX_TRY(auto shares_fid,   raw.field_id("shares"));
  ATX_TRY(auto gics_fid,     raw.field_id("gics"));
  ATX_TRY(auto earnflag_fid, raw.field_id("earnFlag"));
  ATX_TRY(auto atmiv21_fid,  raw.field_id("atmCenI_21d"));
  ATX_TRY(auto atmiv126_fid, raw.field_id("atmCenI_126d"));
  ATX_TRY(auto earncnt5_fid, raw.field_id("nEarnCnt_5d"));

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

  // cash_dividend: 0 (the source cumulative factor already includes dividends).
  std::vector<atx::f64> col_div(cells, 0.0);

  // shares_outstanding: panel's shares column.
  const std::span<const atx::f64> shares_span = raw.field_all(shares_fid);
  std::vector<atx::f64> col_shares(shares_span.begin(), shares_span.end());

  // The archive has no per-observation filing, publication, or revision time.
  // kNoDate means unknown provenance, not proof that these shares were available
  // on the trading date. This panel is an archive replay, not a verified PIT view.
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
  // Step 4: Research close on the source's cumulative-return basis, pointwise.
  // Raw prices, shares, and volume above determine the economic universe screens.
  // -------------------------------------------------------------------------
  std::vector<atx::f64> close_tri = orats_total_return_close(rc, cr);

  // -------------------------------------------------------------------------
  // Step 5: Assemble final Panel in kHistField* order.
  // close/high/low/open share one adjusted basis; raw_close and volume stay raw.
  // market_cap, sector (widened to f64). Mask = in_universe.
  // -------------------------------------------------------------------------
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> data;
  names.reserve(12);
  data.reserve(12);

  // close = TRI
  names.emplace_back(kHistFieldClose);
  data.push_back(std::move(close_tri));

  // raw_close = raw as-traded close
  names.emplace_back(kHistFieldRawClose);
  data.push_back(std::vector<atx::f64>(rc.begin(), rc.end()));

  // Raw traded shares: the total-return factor is not a pure split factor.
  {
    const std::span<const atx::f64> s = raw.field_all(volume_fid);
    names.emplace_back(kHistFieldVolume);
    data.push_back(std::vector<atx::f64>(s.begin(), s.end()));
  }

  // Research O/H/L use the same cumulative factor and validity policy as close.
  {
    const std::span<const atx::f64> s = raw.field_all(high_fid);
    names.emplace_back(kHistFieldHigh);
    data.push_back(adjusted_history_prices(s, cr));
  }

  // low
  {
    const std::span<const atx::f64> s = raw.field_all(low_fid);
    names.emplace_back(kHistFieldLow);
    data.push_back(adjusted_history_prices(s, cr));
  }

  // open
  {
    const std::span<const atx::f64> s = raw.field_all(open_fid);
    names.emplace_back(kHistFieldOpen);
    data.push_back(adjusted_history_prices(s, cr));
  }

  // market_cap
  names.emplace_back(kHistFieldMarketCap);
  data.push_back(uni.market_cap);

  // sector — widen i32 sector_code to f64; map the kNoSectorCode (-1) "missing
  // sector" sentinel to NaN so DSL group ops (group_rank/group_neutralize/...)
  // treat unclassified names as out-of-set (dropped), not as a spurious "-1" group.
  {
    std::vector<atx::f64> sector_f64;
    sector_f64.reserve(cells);
    for (atx::usize k = 0; k < cells; ++k) {
      const atx::i32 sc = uni.sector_code[k];
      sector_f64.push_back(sc == kNoSectorCode
                               ? std::numeric_limits<atx::f64>::quiet_NaN()
                               : static_cast<atx::f64>(sc));
    }
    names.emplace_back(kHistFieldSector);
    data.push_back(std::move(sector_f64));
  }

  // earnFlag — earnings-day flag (raw passthrough)
  {
    const std::span<const atx::f64> s = raw.field_all(earnflag_fid);
    names.emplace_back(kHistFieldEarnFlag);
    data.push_back(std::vector<atx::f64>(s.begin(), s.end()));
  }
  // atmCenI_21d — ATM implied move 21d (raw passthrough)
  { const std::span<const atx::f64> s = raw.field_all(atmiv21_fid);
    names.emplace_back(kHistFieldAtmIv21);
    data.push_back(std::vector<atx::f64>(s.begin(), s.end())); }
  // atmCenI_126d — ATM implied move 126d (raw passthrough)
  { const std::span<const atx::f64> s = raw.field_all(atmiv126_fid);
    names.emplace_back(kHistFieldAtmIv126);
    data.push_back(std::vector<atx::f64>(s.begin(), s.end())); }
  // nEarnCnt_5d — earnings count 5d (raw passthrough)
  { const std::span<const atx::f64> s = raw.field_all(earncnt5_fid);
    names.emplace_back(kHistFieldEarnCnt5);
    data.push_back(std::vector<atx::f64>(s.begin(), s.end())); }

  // -------------------------------------------------------------------------
  // Step 5a-restrict (optional): point-in-time membership allow-list. A column
  // whose securityID is absent from cfg.allow_ids is forced OUT of the universe on
  // every date, before compaction, so the restriction composes with the
  // never-in-universe drop below instead of racing it. This only ever removes
  // membership; it cannot admit a name the screen rejected, and it does not claim
  // per-session point-in-time membership — the caller supplies the set and owns
  // that claim. The set is sorted once and binary-searched per column, never per cell.
  // -------------------------------------------------------------------------
  atx::usize allow_list_excluded_columns = 0;
  if (!cfg.allow_ids.empty()) {
    std::vector<atx::i64> allowed(cfg.allow_ids);
    std::sort(allowed.begin(), allowed.end());
    allowed.erase(std::unique(allowed.begin(), allowed.end()), allowed.end());
    for (atx::usize i = 0; i < N; ++i) {
      const std::string &id = indexed.instrument_ids[i];
      atx::i64 security_id{};
      // Every id was proved canonical above, so this parse cannot fail here.
      (void)std::from_chars(id.data(), id.data() + id.size(), security_id);
      if (std::binary_search(allowed.begin(), allowed.end(), security_id)) {
        continue;
      }
      ++allow_list_excluded_columns;
      for (atx::usize t = 0; t < D; ++t) {
        uni.in_universe[t * N + i] = atx::u8{0};
      }
    }
    if (allow_list_excluded_columns == N) {
      return Err(ErrorCode::InvalidArgument,
                 "build_history_panel: the membership allow-list excludes every instrument "
                 "column in this window");
    }
  }

  // -------------------------------------------------------------------------
  // Step 5b (optional): compact out instrument columns NEVER in-universe over the
  // whole window. Lossless — those columns are all-NaN-masked at eval — and it
  // shrinks the panel from "every symbol that ever traded" to "symbols that pass
  // the screen on >=1 date", cutting memory + per-cell eval cost. The kept order
  // preserves the canonical column order (ascending original index).
  // -------------------------------------------------------------------------
  atx::usize N_out = N;
  std::vector<atx::usize> original_instrument_indices(N);
  std::iota(original_instrument_indices.begin(), original_instrument_indices.end(), atx::usize{0});
  std::vector<std::uint8_t> mask_out(uni.in_universe.begin(), uni.in_universe.end());
  if (!cfg.fixed_axis_ids.empty()) {
    for (atx::usize t = 0; t < D; ++t) for (atx::usize i = 0; i < N; ++i)
      mask_out[t * N + i] = raw.in_universe(t, i) ? 1 : 0;
  }
  if (cfg.compact_to_universe) {
    std::vector<atx::usize> keep;
    keep.reserve(N);
    for (atx::usize i = 0; i < N; ++i) {
      bool ever = false;
      for (atx::usize t = 0; t < D; ++t) {
        if (uni.in_universe[t * N + i] != 0) {
          ever = true;
          break;
        }
      }
      if (ever) {
        keep.push_back(i);
      }
    }
    N_out = keep.size();
    if (N_out == 0) {
      return Err(ErrorCode::InvalidArgument,
                 "build_history_panel: no instrument is ever in-universe under the screen "
                 "(min_adv_usd / min_price / require_sector too strict for this window)");
    }
    if (N_out != N) {
      for (std::vector<atx::f64> &col : data) {
        std::vector<atx::f64> nc(D * N_out);
        for (atx::usize t = 0; t < D; ++t) {
          for (atx::usize k = 0; k < N_out; ++k) {
            nc[t * N_out + k] = col[t * N + keep[k]];
          }
        }
        col = std::move(nc);
      }
      std::vector<std::uint8_t> nm(D * N_out);
      for (atx::usize t = 0; t < D; ++t) {
        for (atx::usize k = 0; k < N_out; ++k) {
          nm[t * N_out + k] = uni.in_universe[t * N + keep[k]];
        }
      }
      mask_out = std::move(nm);
      std::vector<std::string> kept_ids;
      kept_ids.reserve(N_out);
      for (const auto i : keep) kept_ids.push_back(std::move(indexed.instrument_ids[i]));
      indexed.instrument_ids = std::move(kept_ids);
    }
    original_instrument_indices = std::move(keep);
  }

  // Level-basis tags, parallel to the field order (D-01 metadata for the A3 lint).
  // Every assembled name is a kHistField* constant, so an untagged name is a bug.
  std::vector<LevelBasis> field_basis;
  field_basis.reserve(names.size());
  for (const std::string &name : names) {
    const std::optional<LevelBasis> basis = history_field_level_basis(name);
    if (!basis.has_value()) {
      return Err(ErrorCode::Internal,
                 "build_history_panel: field '" + name + "' has no level-basis tag");
    }
    field_basis.push_back(*basis);
  }

  ATX_TRY(auto final_panel,
          alpha::Panel::create(D, N_out, std::move(names), std::move(data), std::move(mask_out)));

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
  HistoryPanel result{std::move(final_panel), digest, catalog.names(),
                      std::move(indexed.session_keys), std::move(indexed.instrument_ids),
                      std::move(original_instrument_indices),
                      std::move(indexed.source_segment_paths), allow_list_excluded_columns,
                      std::move(field_basis)};
  return Ok(std::move(result));
}

} // namespace atx::engine::data
