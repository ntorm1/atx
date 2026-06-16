// atx::engine::data — real-data end-to-end Panel assembly (p3 S1-5).
//
// See real_panel.hpp for the full contract, the fixed assembly order, the digest
// pin, and the named error cases. This file composes the shipped seams; it owns
// no new data-plane primitive.
//
// AS-BUILT DIVERGENCE FROM THE FROZEN PLAN (recorded in the S1-5 ledger):
//   The plan's step-1/2 named atx::tsdb::build_dated_segments + attach_segment_panel
//   over the raw databento hive. As-built those two seams cannot consume the real
//   on-disk databento parquet: build_dated_segments hard-codes `data.parquet` per
//   partition (the real hive holds `part-00000.parquet`) AND load_parquet_scaled
//   reads each field as i64 fixed-point × scale (the real OHLCV columns are f64
//   dollars, so the i64 read fails). Per the brief ("trust the code over the
//   plan; follow as-built"), we read the actual parquet directly through the
//   shipped atx::core::io reader and build the Role::Price Dataset ourselves, then
//   land it through the SAME adapt_panel / align / universe / catalog layer the
//   plan specifies. Price still defines the canonical axis; the seg_cache_dir
//   config field is retained for API stability (and a future .seg cache) but is
//   not written here — there is no seg round-trip to cache.

#include "atx/engine/data/real_panel.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/error.hpp"
#include "atx/core/io/parquet.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/parallel/digest.hpp" // signal_set_digest

#include "atx/engine/data/adapt_panel.hpp"
#include "atx/engine/data/adjust.hpp"
#include "atx/engine/data/align.hpp"
#include "atx/engine/data/catalog.hpp"
#include "atx/engine/data/corporate_actions.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/data/universe.hpp"

namespace atx::engine::data {

namespace {

namespace fs = std::filesystem;
namespace io = atx::core::io;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
constexpr atx::i64 kNsPerDay = 86'400LL * 1'000'000'000LL;

// Databento parquet column names (§0.5). The DateKey comes from the partition
// directory name (`date=YYYY-MM-DD`); the raw OHLCV are f64 dollars; `volume` is u64.
constexpr std::string_view kDbSymbol = "symbol";
constexpr std::string_view kDbOpen = "open";
constexpr std::string_view kDbHigh = "high";
constexpr std::string_view kDbLow = "low";
constexpr std::string_view kDbClose = "close";
constexpr std::string_view kDbVolume = "volume";

// One canonical instrument: a symbol and the InstKey it is interned to (the same
// id the corp-action loader assigned by first-seen master-file order).
struct CanonInst {
  std::string symbol;
  InstKey id{};
};

// =========================================================================
//  Step 1 — the canonical symbol -> InstKey map (mirrors the corp loader).
// =========================================================================

// Read the master parquet's `symbol` column and intern FIRST-SEEN, exactly as
// load_security_master does internally, so the InstKey we assign each symbol
// equals the corp Dataset's InstKey for it. align_onto then joins by matching
// InstKey. Deterministic: the master file's row order is fixed.
[[nodiscard]] Result<std::vector<CanonInst>> canonical_instruments(const std::string &master_path) {
  ATX_TRY(auto table, io::read_parquet(master_path));
  ATX_TRY(auto syms, table.strings(kDbSymbol));
  std::vector<CanonInst> out;
  std::unordered_map<std::string_view, atx::usize> seen;
  seen.reserve(syms.size());
  for (const std::string_view s : syms) {
    if (seen.find(s) == seen.end()) {
      seen.emplace(s, out.size());
      out.push_back(CanonInst{std::string{s}, static_cast<InstKey>(out.size())});
    }
  }
  if (out.empty()) {
    return Err(ErrorCode::InvalidArgument, "build_real_panel: master carries no symbols");
  }
  return Ok(std::move(out));
}

// =========================================================================
//  Step 2 — read the databento price parquet(s) over the window.
// =========================================================================

// A long-form price row landed onto the canonical axis: (date epoch-day, InstKey,
// OHLCV). Accumulated across date partitions, then scattered date-major.
struct PriceLong {
  std::vector<DateKey> dates;       // per-partition epoch-day (one per partition kept)
  std::unordered_map<std::string, InstKey> sym_to_id;
  // Parallel cells, indexed [date_idx * ninst + inst_idx]; built after the axis is known.
  std::vector<atx::f64> open, high, low, close, volume;
};

// Enumerate `<hive_root>/date=YYYY-MM-DD/part-*.parquet`, ascending by date. Each
// partition is one trading date. A date whose start-of-day nanos fall outside the
// window is skipped (no-look-ahead: end is exclusive).
[[nodiscard]] Result<std::vector<std::pair<DateKey, std::string>>>
window_partitions(const std::string &hive_root, const alpha::TimeWindow &window) {
  std::error_code ec;
  if (!fs::exists(fs::path{hive_root}, ec)) {
    return Err(ErrorCode::IoError, "build_real_panel: databento_hive_root does not exist");
  }
  std::vector<std::pair<DateKey, std::string>> parts;
  static constexpr std::string_view kPrefix = "date=";
  for (const auto &entry : fs::directory_iterator{fs::path{hive_root}, ec}) {
    if (!entry.is_directory(ec)) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.size() <= kPrefix.size() || name.compare(0, kPrefix.size(), kPrefix) != 0) {
      continue;
    }
    const atx::core::time::Date d{std::stoi(name.substr(5, 4)),
                                  static_cast<atx::u32>(std::stoi(name.substr(10, 2))),
                                  static_cast<atx::u32>(std::stoi(name.substr(13, 2)))};
    const DateKey day = d.to_days();
    const atx::i64 day_nanos = day * kNsPerDay;
    if (day_nanos < window.start_nanos || day_nanos >= window.end_nanos) {
      continue; // outside [start, end)
    }
    // The real hive writes one `part-*.parquet` per date partition.
    std::string parquet;
    for (const auto &f : fs::directory_iterator{entry.path(), ec}) {
      if (f.path().extension() == ".parquet") {
        parquet = f.path().string();
        break;
      }
    }
    if (!parquet.empty()) {
      parts.emplace_back(day, std::move(parquet));
    }
  }
  std::sort(parts.begin(), parts.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  return Ok(std::move(parts));
}

// Read one date partition's OHLCV for the requested symbols into `dst` at row
// `date_idx`. Symbols not in `id_of` are skipped (price is restricted to the
// canonical set). A symbol absent from this date simply stays NaN (price defines
// the axis; absence is honest missing-ness, never fabricated).
[[nodiscard]] atx::core::Status read_partition(const std::string &parquet, atx::usize date_idx,
                                               const std::unordered_map<std::string, InstKey> &id_of,
                                               atx::usize ninst, PriceLong &dst) {
  ATX_TRY(auto table, io::read_parquet(parquet));
  ATX_TRY(auto syms, table.strings(kDbSymbol));
  ATX_TRY(auto open, table.column_view<atx::f64>(kDbOpen));
  ATX_TRY(auto high, table.column_view<atx::f64>(kDbHigh));
  ATX_TRY(auto low, table.column_view<atx::f64>(kDbLow));
  ATX_TRY(auto close, table.column_view<atx::f64>(kDbClose));
  ATX_TRY(auto volume, table.column_view<atx::u64>(kDbVolume));
  const atx::usize base = date_idx * ninst;
  for (atx::usize r = 0; r < syms.size(); ++r) {
    const auto it = id_of.find(std::string{syms[r]});
    if (it == id_of.end()) {
      continue; // symbol outside the canonical set
    }
    const atx::usize cell = base + static_cast<atx::usize>(it->second);
    dst.open[cell] = open[r];
    dst.high[cell] = high[r];
    dst.low[cell] = low[r];
    dst.close[cell] = close[r];
    dst.volume[cell] = static_cast<atx::f64>(volume[r]);
  }
  return Ok();
}

// =========================================================================
//  Step 2/3 — build the Role::Price Dataset (the canonical axis).
// =========================================================================

// Build a price Dataset whose instruments are the canonical InstKeys and whose
// dates are the ascending window partition dates. Columns are open/high/low/close/
// volume, date-major, NaN where a symbol did not trade that date. This Dataset
// DEFINES the canonical axis everything else aligns onto.
[[nodiscard]] Result<Dataset> build_price_dataset(const std::string &hive_root,
                                                  const alpha::TimeWindow &window,
                                                  const std::vector<CanonInst> &canon) {
  ATX_TRY(auto parts, window_partitions(hive_root, window));
  if (parts.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "build_real_panel: window selects no databento trading dates");
  }
  const atx::usize ndates = parts.size();
  const atx::usize ninst = canon.size();
  const atx::usize cells = ndates * ninst;

  std::unordered_map<std::string, InstKey> id_of;
  id_of.reserve(ninst);
  std::vector<InstKey> instruments(ninst);
  for (atx::usize i = 0; i < ninst; ++i) {
    id_of.emplace(canon[i].symbol, canon[i].id);
    instruments[i] = canon[i].id;
  }

  PriceLong pl;
  pl.open.assign(cells, kNaN);
  pl.high.assign(cells, kNaN);
  pl.low.assign(cells, kNaN);
  pl.close.assign(cells, kNaN);
  pl.volume.assign(cells, kNaN);
  std::vector<DateKey> dates(ndates);
  for (atx::usize d = 0; d < ndates; ++d) {
    dates[d] = parts[d].first;
    ATX_TRY_VOID(read_partition(parts[d].second, d, id_of, ninst, pl));
  }

  // Price schema: the 5 OHLCV columns, F64. Role::Price so adapt_panel accepts it.
  DatasetSchema schema;
  schema.columns = {std::string{kDbOpen}, std::string{kDbHigh}, std::string{kDbLow},
                    std::string{kDbClose}, std::string{kDbVolume}};
  schema.dtypes.assign(schema.columns.size(), ColumnDType::F64);
  schema.role = Role::Price;
  schema.region = "US";

  std::vector<std::vector<atx::f64>> cols = {std::move(pl.open), std::move(pl.high),
                                             std::move(pl.low), std::move(pl.close),
                                             std::move(pl.volume)};
  DatasetProvenance prov{"external:databento", "p3 S1-5 real-data price axis"};
  return Dataset::create(std::move(schema), std::move(dates), std::move(instruments),
                         std::move(cols), /*mask=*/{}, std::move(prov));
}

// =========================================================================
//  Step 4 — total-return adjustment, per symbol, on the price axis.
// =========================================================================

// Produce the total_return_index (canonical `close`) and the retained raw_close,
// each date-major over the price axis. For each instrument column, slice its raw
// close (from the price Dataset) and its aligned cum_adj_factor / cash_dividend
// (from align_onto), run S1-3 adjust_total_return, and scatter the result back.
// A symbol absent in corp-actions has NaN factor/dividend -> adjust treats the
// cell as a gap (raw_close retained, no fabricated return) per the S1-3 NaN policy.
struct AdjustedFields {
  std::vector<atx::f64> total_return_index; // the canonical close
  std::vector<atx::f64> raw_close;          // retained unadjusted close
};

[[nodiscard]] AdjustedFields adjust_per_symbol(const Dataset &price, const AlignedView &aligned,
                                               atx::usize caf_col, atx::usize div_col) {
  const atx::usize nd = price.num_dates();
  const atx::usize ni = price.num_instruments();
  const std::span<const atx::f64> raw = price.column(3); // close is column 3 (OHLCV order)

  AdjustedFields out;
  out.total_return_index.assign(nd * ni, kNaN);
  out.raw_close.assign(nd * ni, kNaN);

  std::vector<atx::f64> sym_close(nd), sym_caf(nd), sym_div(nd);
  for (atx::usize i = 0; i < ni; ++i) {
    for (atx::usize d = 0; d < nd; ++d) {
      const atx::usize flat = d * ni + i;
      sym_close[d] = raw[flat];
      sym_caf[d] = aligned.aligned_columns[caf_col][flat];
      sym_div[d] = aligned.aligned_columns[div_col][flat];
    }
    const AdjustedSeries adj = adjust_total_return(sym_close, sym_caf, sym_div);
    for (atx::usize d = 0; d < nd; ++d) {
      const atx::usize flat = d * ni + i;
      out.total_return_index[flat] = adj.total_return_index[d];
      out.raw_close[flat] = sym_close[d]; // raw close retained verbatim
    }
  }
  return out;
}

// =========================================================================
//  Step 5 helper — an axis-matched corp Dataset for build_universe.
// =========================================================================

// build_universe requires a corp Dataset on the EXACT price axis. align_onto gives
// us the corp columns re-expressed on that axis (an AlignedView); wrap them back
// into a Reference Dataset with the canonical 6-column schema so build_universe
// reads shares/sector positionally.
[[nodiscard]] Result<Dataset> corp_on_price_axis(const Dataset &price, const AlignedView &aligned) {
  std::vector<std::vector<atx::f64>> cols = aligned.aligned_columns; // copy (cold path)
  DatasetSchema schema = corp_action_schema();
  std::vector<DateKey> dates(price.dates().begin(), price.dates().end());
  std::vector<InstKey> instruments(price.instruments().begin(), price.instruments().end());
  DatasetProvenance prov{"derived:corp_on_price_axis", "p3 S1-5 aligned corp-actions"};
  return Dataset::create(std::move(schema), std::move(dates), std::move(instruments),
                         std::move(cols), /*mask=*/{}, std::move(prov));
}

// =========================================================================
//  Step 6 — final Panel assembly + digest.
// =========================================================================

// Append a named owned column to the parallel (names, data) builders unless a
// column of that name already exists (idempotent — the caller's column wins).
void put_field(std::vector<std::string> &names, std::vector<std::vector<atx::f64>> &data,
               std::string_view name, std::vector<atx::f64> col) {
  for (const std::string &n : names) {
    if (n == name) {
      return;
    }
  }
  names.emplace_back(name);
  data.push_back(std::move(col));
}

// Digest the Panel's fields in canonical order: one SignalSet alpha per field
// (name = field name, values = the field's date-major column), in the Panel's
// field-dictionary order. This is signal_set_digest over the fields in the order
// build_real_panel landed them (the pin).
[[nodiscard]] atx::u64 digest_panel(const alpha::Panel &panel) {
  alpha::SignalSet ss;
  ss.dates = panel.dates();
  ss.instruments = panel.instruments();
  ss.alphas.reserve(panel.num_fields());
  for (atx::usize f = 0; f < panel.num_fields(); ++f) {
    const std::span<const atx::f64> col = panel.field_all(static_cast<alpha::FieldId>(f));
    ss.alphas.push_back(alpha::SignalSet::Alpha{std::string{panel.field_name(f)},
                                                std::vector<atx::f64>(col.begin(), col.end())});
  }
  return parallel::signal_set_digest(ss);
}

} // namespace

// ---------------------------------------------------------------------------
//  build_real_panel — the one documented entry point.
// ---------------------------------------------------------------------------
Result<RealPanel> build_real_panel(const RealDataConfig &cfg) {
  if (cfg.databento_hive_root.empty() || cfg.security_master_path.empty()) {
    return Err(ErrorCode::InvalidArgument, "build_real_panel: empty required input path");
  }

  // (1) corp-action Dataset + the canonical symbol -> InstKey map.
  ATX_TRY(auto corp, load_security_master(cfg.security_master_path, corp_action_schema()));
  ATX_TRY(auto canon, canonical_instruments(cfg.security_master_path));

  // (2,3) price Dataset (canonical axis) -> augmented Panel (dollar_volume/vwap/adv).
  ATX_TRY(auto price, build_price_dataset(cfg.databento_hive_root, cfg.window, canon));
  const atx::u16 adv_w = static_cast<atx::u16>(cfg.universe.adv_window);
  const std::vector<atx::u16> adv_windows = {adv_w};
  ATX_TRY(auto base_panel, price_to_panel(price, adv_windows));

  // (4) align corp-actions onto the price axis; per-symbol total-return adjust.
  ATX_TRY(auto aligned, align_onto(price, corp));
  // Canonical corp column order (corporate_actions.hpp): 0 cum_adj_factor,
  // 1 cash_dividend, 2 shares, 3 filed_date, 4 gics, 5 sic.
  const AdjustedFields adj = adjust_per_symbol(price, aligned, /*caf*/ 0, /*div*/ 1);

  // (5) universe screen over the augmented Panel + the axis-matched corp Dataset.
  ATX_TRY(auto corp_axis, corp_on_price_axis(price, aligned));
  ATX_TRY(auto uni, build_universe(base_panel, corp_axis, cfg.universe));

  // (6) assemble the final Panel: base fields + close(=TRI) + raw_close + market_cap
  //     + sector, with the in_universe mask applied; then register + digest.
  const atx::usize nd = base_panel.dates();
  const atx::usize ni = base_panel.instruments();
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> data;
  // Canonical close = total-return index (overrides the raw close base field).
  put_field(names, data, kFieldClose, adj.total_return_index);
  put_field(names, data, kFieldRawClose, adj.raw_close);
  // Carry every base/derived field EXCEPT the raw `close` (replaced by the TRI).
  for (atx::usize f = 0; f < base_panel.num_fields(); ++f) {
    const std::string_view fn = base_panel.field_name(static_cast<alpha::FieldId>(f));
    if (fn == kFieldClose) {
      continue; // raw close already retained as raw_close
    }
    const std::span<const atx::f64> col = base_panel.field_all(static_cast<alpha::FieldId>(f));
    put_field(names, data, fn, std::vector<atx::f64>(col.begin(), col.end()));
  }
  // Universe fields: market_cap + sector (widened to f64). adv/dollar_volume already
  // present from price_to_panel; in_universe becomes the mask, not a field.
  std::vector<atx::f64> sector_f64(nd * ni, kNaN);
  for (atx::usize k = 0; k < sector_f64.size(); ++k) {
    sector_f64[k] = static_cast<atx::f64>(uni.sector_code[k]);
  }
  put_field(names, data, kFieldMarketCap, uni.market_cap);
  put_field(names, data, kFieldSector, std::move(sector_f64));

  ATX_TRY(auto panel,
          alpha::Panel::create(nd, ni, std::move(names), std::move(data),
                               std::vector<std::uint8_t>(uni.in_universe.begin(),
                                                         uni.in_universe.end())));

  // Catalog: register the three composing datasets + the derivation lineage.
  DatasetCatalog catalog;
  ATX_TRY_VOID(catalog.register_dataset(std::string{kDatasetPrice}, std::move(price)));
  ATX_TRY_VOID(catalog.register_dataset(std::string{kDatasetCorpActions}, std::move(corp)));
  ATX_TRY_VOID(catalog.register_dataset(std::string{kDatasetUniverse}, std::move(corp_axis)));
  ATX_TRY_VOID(catalog.derive(std::string{kDatasetUniverse},
                              {std::string{kDatasetPrice}, std::string{kDatasetCorpActions}}));

  // Aggregate-init: alpha::Panel has no public default ctor, so build the result
  // in place (digest computed BEFORE the Panel is moved out).
  const atx::u64 digest = digest_panel(panel);
  RealPanel result{std::move(panel), digest, catalog.names()}; // names ascending
  return Ok(std::move(result));
}

} // namespace atx::engine::data
