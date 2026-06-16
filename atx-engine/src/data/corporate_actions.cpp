// atx::engine::data — Security-master ingestion implementation (p3 S1-2).
//
// See corporate_actions.hpp for the full contract. The flow is:
//   parquet -> per-symbol raw rows -> PIT-correct fundamentals -> date-major
//   Dataset. Every step is COLD-PATH (once per backtest window); allocation is
//   intentional. Helpers keep each function short and the PIT invariant legible.

#include "atx/engine/data/corporate_actions.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/io/parquet.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::data {

namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
constexpr atx::i32 kI32Null = (std::numeric_limits<atx::i32>::min)(); // date32_days null sentinel

// A single parsed security-master row for one symbol, dates as epoch-days.
struct RawRow {
  atx::i64 date{};            // trading date (epoch-day)
  atx::f64 cum_adj_factor{};  // NaN if absent
  atx::f64 cash_dividend{};   // 0.0 if absent
  atx::f64 shares{};          // NaN if absent
  atx::i64 filed_date{};      // knowledge-date (epoch-day); kNoDate if absent
  atx::i32 gics{};            // kNoSectorCode if absent
  atx::i32 sic{};             // kNoSectorCode if absent
};

// All columns materialized out of the parquet, parallel by row. Owned f64/i32
// vectors + per-cell null masks for the numeric columns the bridge can't tag.
struct MasterColumns {
  std::vector<std::string_view> symbol;
  std::vector<atx::i32> date;          // epoch-day
  std::vector<atx::f64> cum_adj_factor;
  std::vector<atx::f64> cash_dividend;
  std::vector<atx::f64> shares;
  std::vector<atx::i32> filed_date;    // epoch-day, kI32Null if null
  std::vector<std::string_view> sec_sic;
  std::vector<std::string_view> gics;
  std::vector<std::string_view> currency;
  std::vector<atx::u8> shares_null;
  atx::usize rows{};
};

// Parse a non-empty numeric string to i32 (sector/SIC codes). Returns sentinel
// on empty / non-numeric input — sector codes are emitted, never coerced to 0.
[[nodiscard]] atx::i32 parse_sector_code(std::string_view s) noexcept {
  if (s.empty()) {
    return kNoSectorCode;
  }
  atx::i32 value = 0;
  bool any = false;
  for (const char c : s) {
    if (c < '0' || c > '9') {
      return kNoSectorCode; // non-numeric (e.g. blank/NA) -> sentinel
    }
    value = value * 10 + (c - '0');
    any = true;
  }
  return any ? value : kNoSectorCode;
}

// Reject a non-USD dividend currency. Empty/absent currency is permitted (no
// dividend row carries no currency). Non-empty non-"USD" fails the whole load.
[[nodiscard]] Result<void> require_usd(const std::vector<std::string_view> &currency) {
  for (const std::string_view c : currency) {
    if (!c.empty() && c != "USD") {
      return Err(ErrorCode::InvalidArgument,
                 std::string{"load_security_master: non-USD dividend_currency '"} +
                     std::string{c} + "' (multi-currency total return is out of scope)");
    }
  }
  return Ok();
}

// Read an epoch-day date column. The real master stores dates as DATE32; the
// atx-core parquet writer (used by tests) emits i64. Accept either: try date32
// first, then fall back to an i64 epoch-day column. Returns nullopt on absence.
[[nodiscard]] std::optional<std::vector<atx::i32>>
read_date_days(const atx::core::io::ParquetTable &table, std::string_view name) {
  if (auto d32 = table.date32_days(name); d32.has_value()) {
    return std::move(d32).value();
  }
  auto i64v = table.column_view<atx::i64>(name);
  if (!i64v.has_value()) {
    return std::nullopt;
  }
  const std::span<const atx::i64> src = i64v.value();
  std::vector<atx::i32> out(src.size());
  for (atx::usize i = 0; i < src.size(); ++i) {
    out[i] = static_cast<atx::i32>(src[i]);
  }
  return out;
}

// Pull a string column, tolerating absence with an all-empty vector of length
// `rows` (gics/sic/currency are optional across master variants).
[[nodiscard]] std::vector<std::string_view>
strings_or_empty(const atx::core::io::ParquetTable &table, std::string_view name, atx::usize rows) {
  auto col = table.strings(name);
  if (col.has_value()) {
    return std::move(col).value();
  }
  return std::vector<std::string_view>(rows, std::string_view{});
}

// Read the f64 column `name` and overwrite null cells with `null_fill` (NaN for
// unknown facts, 0.0 for "no dividend"). The numeric bridge drops validity, so
// we restore it from null_mask. Err if the column is missing.
[[nodiscard]] Result<std::vector<atx::f64>>
read_f64_pit(const atx::core::io::ParquetTable &table, std::string_view name, atx::f64 null_fill) {
  auto view = table.column_view<atx::f64>(name);
  if (!view.has_value()) {
    return Err(ErrorCode::InvalidArgument,
               std::string{"load_security_master: missing f64 column '"} + std::string{name} + "'");
  }
  auto mask = table.null_mask(name);
  if (!mask.has_value()) {
    return Err(mask.error());
  }
  const std::span<const atx::f64> src = view.value();
  const std::vector<atx::u8> &nulls = mask.value();
  std::vector<atx::f64> out(src.begin(), src.end());
  for (atx::usize i = 0; i < out.size() && i < nulls.size(); ++i) {
    if (nulls[i] != 0) {
      out[i] = null_fill;
    }
  }
  return Ok(std::move(out));
}

// Materialize every needed column out of the master ParquetTable. shares is read
// as i64 then widened to f64 with nulls -> NaN (no fabricated share count).
[[nodiscard]] Result<MasterColumns> read_master_columns(const atx::core::io::ParquetTable &table) {
  MasterColumns m;
  m.rows = static_cast<atx::usize>(table.num_rows());

  auto symbol = table.strings("symbol");
  if (!symbol.has_value()) {
    return Err(ErrorCode::InvalidArgument, "load_security_master: missing 'symbol' column");
  }
  m.symbol = std::move(symbol).value();

  auto date = read_date_days(table, "date");
  if (!date.has_value()) {
    return Err(ErrorCode::InvalidArgument, "load_security_master: missing/invalid 'date' column");
  }
  m.date = std::move(date).value();

  auto filed = read_date_days(table, "shares_filed_date");
  m.filed_date = filed.has_value() ? std::move(filed).value()
                                   : std::vector<atx::i32>(m.rows, kI32Null);

  auto caf = read_f64_pit(table, "cumulative_adjustment_factor", kNaN);
  if (!caf.has_value()) {
    return Err(caf.error());
  }
  m.cum_adj_factor = std::move(caf).value();

  auto div = read_f64_pit(table, "cash_dividend", 0.0);
  if (!div.has_value()) {
    return Err(div.error());
  }
  m.cash_dividend = std::move(div).value();

  // shares_outstanding is i64 in the master; widen to f64, nulls -> NaN.
  auto shares_view = table.column_view<atx::i64>("shares_outstanding");
  if (!shares_view.has_value()) {
    return Err(ErrorCode::InvalidArgument, "load_security_master: missing 'shares_outstanding'");
  }
  auto shares_null = table.null_mask("shares_outstanding");
  if (!shares_null.has_value()) {
    return Err(shares_null.error());
  }
  m.shares_null = std::move(shares_null).value();
  const std::span<const atx::i64> sv = shares_view.value();
  m.shares.resize(sv.size());
  for (atx::usize i = 0; i < sv.size(); ++i) {
    const bool is_null = i < m.shares_null.size() && m.shares_null[i] != 0;
    m.shares[i] = is_null ? kNaN : static_cast<atx::f64>(sv[i]);
  }

  m.sec_sic = strings_or_empty(table, "sec_sic", m.rows);
  m.gics = strings_or_empty(table, "gics_sector_code", m.rows);
  m.currency = strings_or_empty(table, "dividend_currency", m.rows);
  return Ok(std::move(m));
}

// Group raw rows by symbol in FIRST-SEEN order (deterministic intern order), each
// group sorted ascending by date. Returns the ordered symbol list + per-symbol
// rows. Filing date kI32Null widens to kNoDate.
struct GroupedRows {
  std::vector<std::string> symbols;            // intern order
  std::vector<std::vector<RawRow>> per_symbol; // parallel to symbols
};

[[nodiscard]] GroupedRows group_by_symbol(const MasterColumns &m) {
  GroupedRows g;
  std::unordered_map<std::string_view, atx::usize> index;
  index.reserve(m.rows);
  for (atx::usize i = 0; i < m.rows; ++i) {
    const std::string_view sym = m.symbol[i];
    auto it = index.find(sym);
    if (it == index.end()) {
      it = index.emplace(sym, g.symbols.size()).first;
      g.symbols.emplace_back(sym);
      g.per_symbol.emplace_back();
    }
    RawRow row;
    row.date = static_cast<atx::i64>(m.date[i]);
    row.cum_adj_factor = m.cum_adj_factor[i];
    row.cash_dividend = m.cash_dividend[i];
    row.shares = m.shares[i];
    row.filed_date = (m.filed_date[i] == kI32Null) ? kNoDate : static_cast<atx::i64>(m.filed_date[i]);
    row.gics = parse_sector_code(m.gics[i]);
    row.sic = parse_sector_code(m.sec_sic[i]);
    g.per_symbol[it->second].push_back(row);
  }
  for (auto &rows : g.per_symbol) {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const RawRow &a, const RawRow &b) { return a.date < b.date; });
  }
  return g;
}

// A single share-filing event: the value became KNOWLEDGE on `filed_date`.
struct Filing {
  atx::i64 filed_date{};
  atx::f64 shares{};
};

// PIT-correct one symbol's fundamentals. For each ascending date d:
//   * shares visible at d = shares of the filing with greatest filed_date <= d
//     (the knowledge-date leak guard); NaN before the first filing.
//   * sector (gics/sic) is reference metadata joined on the row's own date
//     (forward-filled) — not a forecast, no separate knowledge date exists.
// Writes into the CorpActionColumns arrays already sized to this symbol's dates.
void pit_fill_symbol(const std::vector<RawRow> &rows, CorpActionColumns &out) {
  // Collect filings (distinct knowledge events), ascending by filed_date.
  std::vector<Filing> filings;
  for (const RawRow &r : rows) {
    if (r.filed_date != kNoDate && !std::isnan(r.shares)) {
      filings.push_back({r.filed_date, r.shares});
    }
  }
  std::stable_sort(filings.begin(), filings.end(),
                   [](const Filing &a, const Filing &b) { return a.filed_date < b.filed_date; });

  atx::i32 last_gics = kNoSectorCode;
  atx::i32 last_sic = kNoSectorCode;
  for (atx::usize k = 0; k < rows.size(); ++k) {
    const RawRow &r = rows[k];
    out.dates[k] = r.date;
    out.cum_adj_factor[k] = r.cum_adj_factor;
    out.cash_dividend[k] = r.cash_dividend;

    // As-of: greatest filed_date <= r.date (upper_bound then step back). The row's
    // OWN raw filed_date may be a FUTURE filing (the leak window) — we ignore it
    // and resolve from the filing actually knowable on/before this date. Both
    // shares_outstanding and shares_filed_date reflect that resolving filing, so
    // shares_filed_date[k] <= date[k] holds whenever shares is non-NaN.
    const auto it = std::upper_bound(filings.begin(), filings.end(), r.date,
                                     [](atx::i64 d, const Filing &f) { return d < f.filed_date; });
    if (it == filings.begin()) {
      out.shares_outstanding[k] = kNaN;
      out.shares_filed_date[k] = kNoDate;
    } else {
      out.shares_outstanding[k] = (it - 1)->shares;
      out.shares_filed_date[k] = (it - 1)->filed_date;
    }

    // Sector forward-fill on the row date (metadata, not a forecast).
    if (r.gics != kNoSectorCode) {
      last_gics = r.gics;
    }
    if (r.sic != kNoSectorCode) {
      last_sic = r.sic;
    }
    out.gics_sector_code[k] = last_gics;
    out.sic_code[k] = last_sic;
  }
}

void resize_corp_columns(CorpActionColumns &c, atx::usize n) {
  c.dates.resize(n);
  c.cum_adj_factor.resize(n);
  c.cash_dividend.resize(n);
  c.shares_outstanding.resize(n);
  c.shares_filed_date.resize(n);
  c.gics_sector_code.resize(n);
  c.sic_code.resize(n);
}

// Build the union ascending date axis across all symbols (sorted distinct).
[[nodiscard]] std::vector<DateKey> union_dates(const GroupedRows &g) {
  std::set<atx::i64> seen; // ordered, distinct
  for (const auto &rows : g.per_symbol) {
    for (const RawRow &r : rows) {
      seen.insert(r.date);
    }
  }
  std::vector<DateKey> dates;
  dates.reserve(seen.size());
  for (const atx::i64 d : seen) {
    dates.push_back(static_cast<DateKey>(d));
  }
  return dates;
}

// Assemble the six date-major Dataset columns from the per-symbol PIT-filled
// CorpActionColumns, placing each symbol's value on the union date row it occupies
// (a symbol absent on a union date keeps the column default already written).
void scatter_columns(const std::vector<DateKey> &dates,
                     const std::vector<CorpActionColumns> &per_symbol,
                     std::vector<std::vector<atx::f64>> &cols) {
  const atx::usize nd = dates.size();
  const atx::usize ni = per_symbol.size();
  std::unordered_map<atx::i64, atx::usize> date_index;
  date_index.reserve(nd);
  for (atx::usize d = 0; d < nd; ++d) {
    date_index.emplace(static_cast<atx::i64>(dates[d]), d);
  }
  for (atx::usize i = 0; i < ni; ++i) {
    const CorpActionColumns &c = per_symbol[i];
    for (atx::usize k = 0; k < c.dates.size(); ++k) {
      const auto found = date_index.find(c.dates[k]);
      if (found == date_index.end()) {
        continue;
      }
      const atx::usize flat = found->second * ni + i;
      cols[0][flat] = c.cum_adj_factor[k];
      cols[1][flat] = c.cash_dividend[k];
      cols[2][flat] = c.shares_outstanding[k];
      cols[3][flat] = static_cast<atx::f64>(c.shares_filed_date[k]);
      cols[4][flat] = (c.gics_sector_code[k] == kNoSectorCode)
                          ? kNoSector
                          : static_cast<atx::f64>(c.gics_sector_code[k]);
      cols[5][flat] = (c.sic_code[k] == kNoSectorCode) ? kNoSector
                                                       : static_cast<atx::f64>(c.sic_code[k]);
    }
  }
}

// Default cells before scatter: dividend 0.0, sector sentinel, factor/shares NaN,
// filed-date kNoDate (widened). A (symbol,date) the master never lists reads
// these — never a fabricated dividend or sector.
[[nodiscard]] std::vector<std::vector<atx::f64>> default_columns(atx::usize cells) {
  std::vector<std::vector<atx::f64>> cols(kCorpActionColumnCount);
  cols[0].assign(cells, kNaN);                                  // cum_adj_factor
  cols[1].assign(cells, 0.0);                                   // cash_dividend
  cols[2].assign(cells, kNaN);                                  // shares_outstanding
  cols[3].assign(cells, static_cast<atx::f64>(kNoDate));        // shares_filed_date
  cols[4].assign(cells, kNoSector);                             // gics_sector_code
  cols[5].assign(cells, kNoSector);                             // sic_code
  return cols;
}

// Validate that `schema` is the canonical corporate-action schema (coherent +
// exactly the six kCol* names in canonical order).
[[nodiscard]] Result<void> validate_schema(const DatasetSchema &schema) {
  if (!schema_is_coherent(schema)) {
    return Err(ErrorCode::InvalidArgument, "load_security_master: schema is not coherent");
  }
  const std::array<std::string_view, kCorpActionColumnCount> expected = {
      kColCumAdjFactor, kColCashDividend, kColSharesOutstanding,
      kColSharesFiledDate, kColGicsSectorCode, kColSicCode};
  if (schema.columns.size() != kCorpActionColumnCount) {
    return Err(ErrorCode::InvalidArgument,
               "load_security_master: schema must carry exactly 6 corp-action columns");
  }
  for (atx::usize i = 0; i < kCorpActionColumnCount; ++i) {
    if (schema.columns[i] != expected[i]) {
      return Err(ErrorCode::InvalidArgument,
                 std::string{"load_security_master: schema column ["} + std::to_string(i) +
                     "] must be '" + std::string{expected[i]} + "'");
    }
  }
  return Ok();
}

// Build the Dataset from grouped + currency-checked master columns. Shared by the
// single-file and partitioned loaders.
[[nodiscard]] Result<Dataset> assemble_dataset(GroupedRows &&g, const DatasetSchema &schema) {
  const atx::usize ni = g.symbols.size();
  std::vector<CorpActionColumns> per_symbol(ni);
  for (atx::usize i = 0; i < ni; ++i) {
    resize_corp_columns(per_symbol[i], g.per_symbol[i].size());
    pit_fill_symbol(g.per_symbol[i], per_symbol[i]);
  }

  const std::vector<DateKey> dates = union_dates(g);
  const atx::usize cells = dates.size() * ni;
  std::vector<std::vector<atx::f64>> cols = default_columns(cells);
  scatter_columns(dates, per_symbol, cols);

  // Instrument ids = first-seen intern order [0, ni).
  std::vector<InstKey> instruments(ni);
  for (atx::usize i = 0; i < ni; ++i) {
    instruments[i] = static_cast<InstKey>(i);
  }

  DatasetProvenance prov{"external:security_master", "p3 S1-2 corporate-action ingestion"};
  return Dataset::create(schema, std::vector<DateKey>(dates), std::move(instruments),
                         std::move(cols), /*mask=*/{}, std::move(prov));
}

} // namespace

DatasetSchema corp_action_schema(std::string region, std::string universe_tag) {
  DatasetSchema s;
  s.columns = {std::string{kColCumAdjFactor},      std::string{kColCashDividend},
               std::string{kColSharesOutstanding}, std::string{kColSharesFiledDate},
               std::string{kColGicsSectorCode},    std::string{kColSicCode}};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64, ColumnDType::F64,
              ColumnDType::I64, ColumnDType::Category, ColumnDType::Category};
  s.role = Role::Reference;
  s.pit_delay = 0;
  s.region = std::move(region);
  s.universe_tag = std::move(universe_tag);
  return s;
}

atx::core::Result<Dataset> load_security_master(std::string_view master_parquet_path,
                                                const DatasetSchema &schema) {
  if (auto v = validate_schema(schema); !v.has_value()) {
    return Err(v.error());
  }
  auto table_res = atx::core::io::read_parquet(master_parquet_path);
  if (!table_res.has_value()) {
    return Err(table_res.error());
  }
  const atx::core::io::ParquetTable table = std::move(table_res).value();

  auto cols = read_master_columns(table);
  if (!cols.has_value()) {
    return Err(cols.error());
  }
  if (auto usd = require_usd(cols.value().currency); !usd.has_value()) {
    return Err(usd.error());
  }
  GroupedRows g = group_by_symbol(cols.value());
  return assemble_dataset(std::move(g), schema);
}

atx::core::Result<Dataset>
load_security_master_partitioned(std::string_view root_dir, std::span<const std::string> symbols,
                                 const DatasetSchema &schema) {
  if (auto v = validate_schema(schema); !v.has_value()) {
    return Err(v.error());
  }
  // Compose the single-file master by reading each requested symbol's master row
  // group from the security_master.parquet within root_dir. The canonical layout
  // keeps the unified master alongside the partitions, so we reuse it and filter
  // to the requested symbols (preserving their order as the intern order).
  std::string master_path{root_dir};
  master_path += "/security_master/security_master.parquet";
  auto table_res = atx::core::io::read_parquet(master_path);
  if (!table_res.has_value()) {
    return Err(table_res.error());
  }
  const atx::core::io::ParquetTable table = std::move(table_res).value();
  auto cols = read_master_columns(table);
  if (!cols.has_value()) {
    return Err(cols.error());
  }
  if (auto usd = require_usd(cols.value().currency); !usd.has_value()) {
    return Err(usd.error());
  }

  // Filter to requested symbols, ordering groups by the caller's symbol list.
  std::unordered_map<std::string_view, atx::usize> want;
  want.reserve(symbols.size());
  for (atx::usize i = 0; i < symbols.size(); ++i) {
    want.emplace(symbols[i], i);
  }
  GroupedRows all = group_by_symbol(cols.value());
  GroupedRows g;
  g.symbols.assign(symbols.begin(), symbols.end());
  g.per_symbol.assign(symbols.size(), {});
  for (atx::usize s = 0; s < all.symbols.size(); ++s) {
    const auto it = want.find(all.symbols[s]);
    if (it != want.end()) {
      g.per_symbol[it->second] = std::move(all.per_symbol[s]);
    }
  }
  return assemble_dataset(std::move(g), schema);
}

atx::core::Result<CorpActionColumns> extract_symbol(const Dataset &corp_actions, InstKey inst) {
  if (corp_actions.schema().columns.size() != kCorpActionColumnCount) {
    return Err(ErrorCode::InvalidArgument, "extract_symbol: not a corporate-action Dataset");
  }
  const std::span<const InstKey> insts = corp_actions.instruments();
  atx::usize col_idx = insts.size();
  for (atx::usize i = 0; i < insts.size(); ++i) {
    if (insts[i] == inst) {
      col_idx = i;
      break;
    }
  }
  if (col_idx == insts.size()) {
    return Err(ErrorCode::NotFound, "extract_symbol: instrument not in Dataset");
  }

  const atx::usize nd = corp_actions.num_dates();
  const atx::usize ni = corp_actions.num_instruments();
  const std::span<const DateKey> dates = corp_actions.dates();
  const std::span<const atx::f64> caf = corp_actions.column(0);
  const std::span<const atx::f64> div = corp_actions.column(1);
  const std::span<const atx::f64> shr = corp_actions.column(2);
  const std::span<const atx::f64> fld = corp_actions.column(3);
  const std::span<const atx::f64> gic = corp_actions.column(4);
  const std::span<const atx::f64> sic = corp_actions.column(5);

  CorpActionColumns out;
  resize_corp_columns(out, nd);
  for (atx::usize d = 0; d < nd; ++d) {
    const atx::usize flat = d * ni + col_idx;
    out.dates[d] = static_cast<atx::i64>(dates[d]);
    out.cum_adj_factor[d] = caf[flat];
    out.cash_dividend[d] = div[flat];
    out.shares_outstanding[d] = shr[flat];
    out.shares_filed_date[d] = static_cast<atx::i64>(fld[flat]);
    out.gics_sector_code[d] =
        (gic[flat] == kNoSector) ? kNoSectorCode : static_cast<atx::i32>(gic[flat]);
    out.sic_code[d] = (sic[flat] == kNoSector) ? kNoSectorCode : static_cast<atx::i32>(sic[flat]);
  }
  return Ok(std::move(out));
}

} // namespace atx::engine::data
