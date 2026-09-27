#pragma once

// atx::engine::data — Security-master ingestion into a typed, point-in-time
// corporate-action Dataset (p3 S1-2).
//
// WHAT THIS IS
//   Loads the US security-master parquet (`security_master.parquet`, or the
//   canonical by-symbol partitions) into a Reference-role `data::Dataset` keyed
//   (date x instrument). The Dataset carries the corporate-action /fundamental
//   columns every later real-data unit consumes: split factor, cash dividend,
//   shares outstanding, the knowledge-date stamp, and sector classifications.
//
// THE POINT-IN-TIME (PIT) LEAK GUARD — the load-bearing invariant
//   Fundamentals (`shares_outstanding`, sector codes) are FORECASTS-as-of a
//   filing: a row dated `d` may carry a `shares_outstanding` value that was only
//   *filed* (became public knowledge) on a LATER date. Using that value at `d`
//   leaks the future. So at load time we forward-fill each fundamental using
//   ONLY rows whose knowledge-date `shares_filed_date <= d`. A value is invisible
//   on every date before its filing date.
//   Corporate-action facts (`cum_adj_factor`, `cash_dividend`) are mechanical
//   ex-date facts, NOT forecasts — they are joined on their own event date with
//   no knowledge-date guard.
//
// SENTINELS / MISSING DATA (never silently coerce missing -> 0)
//   * cash_dividend   : absent -> 0.0   (genuinely no dividend on a non-ex date)
//   * shares_outstanding : absent / not-yet-filed -> NaN (no fabricated count)
//   * shares_filed_date  : absent -> NaN
//   * gics_sector_code, sic_code : absent -> kNoSector (-1.0), never 0
//   * cum_adj_factor  : absent -> NaN (a missing split factor is unknown, not 1.0)
//
// CURRENCY
//   US-equity scope only: `dividend_currency` must be "USD". A non-USD row makes
//   the whole load fail closed with Err(InvalidArgument) — multi-currency total
//   return is out of scope for S1.
//
// DETERMINISM
//   Instruments are interned first-seen in row order (matching the price segment
//   interning), so the corp-action Dataset aligns onto the same instrument ids in
//   S1-5. Two loads of the same file produce byte-identical symbol->id order, the
//   same ascending union date axis, and the same NaN placement.
//
// OWNERSHIP / LIFECYCLE
//   `load_*` returns an owned `Dataset` by value (cold path; allocation is
//   intentional). `CorpActionColumns` owns its vectors. Neither borrows the
//   parquet file: the table is read, materialized into owned columns, and closed
//   before return. Spans handed out by the Dataset alias its owned columns — see
//   dataset.hpp for span lifetime rules.
//
// COLUMN ORDER (canonical — stable across loads, consumed positionally by S1-3+)
//   0: cum_adj_factor      3: shares_filed_date
//   1: cash_dividend       4: gics_sector_code
//   2: shares_outstanding  5: sic_code

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cstdint>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/align.hpp" // AlignOptions (corp_action_align_options)
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace atx::engine::data {

// =========================================================================
//  Public constants / vocabulary
// =========================================================================

// Sector sentinel: emitted when neither a GICS code nor a SIC code is available
// for a (symbol, date). NEVER 0 — 0 is a valid-looking sector and would silently
// misclassify. Stored as f64 in the Dataset (the columnar store is f64-only).
inline constexpr atx::f64 kNoSector = -1.0;

// Canonical column names + count for the corporate-action Reference Dataset.
// Order is load-bearing: S1-3/S1-4 read these columns by name, and the schema
// built by corp_action_schema() lists them in exactly this order.
inline constexpr std::string_view kColCumAdjFactor = "cum_adj_factor";
inline constexpr std::string_view kColCashDividend = "cash_dividend";
inline constexpr std::string_view kColSharesOutstanding = "shares_outstanding";
inline constexpr std::string_view kColSharesFiledDate = "shares_filed_date";
inline constexpr std::string_view kColGicsSectorCode = "gics_sector_code";
inline constexpr std::string_view kColSicCode = "sic_code";
inline constexpr atx::usize kCorpActionColumnCount = 6;

// =========================================================================
//  Per-symbol extracted view
// =========================================================================

// One symbol's corporate-action / fundamental history, ascending by date and
// knowledge-date stamped. The arrays are PARALLEL: index k describes date[k].
// This is the per-symbol slice S1-3 (`adjust_total_return`) consumes directly.
//
// `shares_outstanding`, `gics_sector_code`, `sic_code` are ALREADY PIT-corrected
// (forward-filled by `shares_filed_date`) — index k holds the value knowable at
// date[k], or the sentinel/NaN if nothing was filed on/before date[k].
struct CorpActionColumns {
  std::vector<atx::i64> dates;              // epoch-days, strictly ascending
  std::vector<atx::f64> cum_adj_factor;     // cumulative split factor; NaN if absent
  std::vector<atx::f64> cash_dividend;      // per-share on ex-date; 0.0 when none
  std::vector<atx::f64> shares_outstanding; // PIT fwd-filled; NaN before first filing
  std::vector<atx::i64> shares_filed_date;  // knowledge-date (epoch-day); kNoDate if none
  std::vector<atx::i32> gics_sector_code;   // PIT fwd-filled; kNoSectorCode if absent
  std::vector<atx::i32> sic_code;           // PIT fwd-filled; kNoSectorCode if absent
};

// Sentinels for the integer columns of CorpActionColumns (the Dataset stores the
// f64-widened forms; kNoSector above is the f64 sector sentinel).
inline constexpr atx::i64 kNoDate = atx::i64{-1};       // shares_filed_date absent
inline constexpr atx::i32 kNoSectorCode = atx::i32{-1}; // sector code absent

// =========================================================================
//  PIT shares rule (W0-D0, D-06 rebase + tie part)
// =========================================================================
//
// A filing's share count is stated on the share basis of the row that carries it.
// A split between that row and date d changes the count (a 2:1 split doubles it),
// so the value visible at d must be REBASED by the cum_adj_factor ratio:
//
//     shares_d = shares_row · cum_adj_factor(d) / cum_adj_factor(row)
//
// (cum_adj_factor steps UP by the split ratio on the ex-date — the raw price falls
// by the same ratio — so the count scales with the factor.) Several rows can carry
// the SAME filed_date (the vendor repeats one filing across dates, restated on each
// row's basis); only rows dated <= d may resolve date d.
//
//   AsFiledV1        — legacy: greatest filed_date <= d over ALL rows (a tie can
//                      resolve to a row dated after d), no rebase.
//   RebasedRowDatedV2 — greatest filed_date <= d among rows dated <= d, ties to the
//                      latest such row; the count is rebased from that row's basis to
//                      d's basis. When the two factors differ and either is not a
//                      finite positive number the count is NaN (unknown basis, never
//                      a guessed one). Default.
enum class SharesPitRule : std::uint8_t {
  AsFiledV1 = 1,
  RebasedRowDatedV2 = 2,
};

// =========================================================================
//  Aligning the corporate-action Dataset onto a price axis (W0-D0, D-05)
// =========================================================================
//
//   AsOfForwardFillV1  — legacy align_onto: every column forward-filled without
//                        limit (a dividend is counted on every following session
//                        the master has no row for; a factor freezes past the
//                        master's last row).
//   EventOnceCappedV2  — cash_dividend is an EVENT column (joins its own session
//                        only, kAlignEventSession); every other column may be at most
//                        kCorpMaxStaleSessions canonical sessions stale; and a price
//                        date past the master's last date fails (require_coverage).
//                        Default.
enum class CorpAlignRule : std::uint8_t {
  AsOfForwardFillV1 = 1,
  EventOnceCappedV2 = 2,
};

inline constexpr atx::usize kCorpMaxStaleSessions = 5; // one trading week of master holes

// Align options for the canonical 6-column corporate-action Dataset under `rule`.
[[nodiscard]] AlignOptions
corp_action_align_options(CorpAlignRule rule = CorpAlignRule::EventOnceCappedV2);

// =========================================================================
//  Schema + loaders
// =========================================================================

// The fixed Reference-role schema describing the corporate-action Dataset's six
// f64 columns in canonical order. `region`/`universe_tag` are forwarded onto the
// returned Dataset's schema. Callers pass their own schema to the loaders so the
// PIT-delay / region / universe tagging is theirs to set; this helper builds the
// conventional one.
[[nodiscard]] DatasetSchema corp_action_schema(std::string region = "US",
                                               std::string universe_tag = {});

// Load the security master parquet into a Reference-role Dataset keyed
// (date x instrument).
//
// Expected parquet columns (the smoke + full us_split_adjustment_factors master):
//   date(date32), symbol(string), cumulative_adjustment_factor(f64),
//   cash_dividend(f64), dividend_currency(string), shares_outstanding(i64),
//   shares_filed_date(date32), sec_sic(string), gics_sector_code(string).
//
// `schema` must be coherent and carry exactly the six kCol* columns in canonical
// order (corp_action_schema() builds a valid one). It supplies role/region/
// universe metadata; column names are validated against the canonical set.
//
// Returns Err(InvalidArgument) on: a missing/extra schema column, any non-USD
// dividend_currency, or a malformed parquet (missing required column, ragged).
// Returns Err(IoError)/Err(ParseError) on a parquet read failure.
//
// `shares_rule` selects the PIT shares resolution (default RebasedRowDatedV2).
[[nodiscard]] atx::core::Result<Dataset>
load_security_master(std::string_view master_parquet_path, const DatasetSchema &schema,
                     SharesPitRule shares_rule = SharesPitRule::RebasedRowDatedV2);

// Load the canonical by-symbol partitions (factors / dividends / shares /
// sectors under `root_dir`) for the requested symbols, producing the SAME
// Dataset shape as load_security_master. Use when the single master file is
// unavailable. `symbols` fixes the instrument intern order (first-seen).
//
// Partition layout (us_split_adjustment_factors):
//   factors_by_symbol/symbol=SYM/data_0.parquet            (symbol,date,return_factor*)
//   dividends_by_symbol/symbol=SYM/data_0.parquet          (symbol,date,cash_dividend,currency)
//   shares_outstanding_by_symbol/symbol=SYM/data_0.parquet (symbol,date,shares_outstanding,filed_date)
//   sectors_by_symbol/symbol=SYM/data_0.parquet            (symbol,sec_sic,gics_sector_code)
//
// (*) factors store a cumulative_adjustment_factor under the canonical layout;
// the partition reader reads whichever of cumulative_adjustment_factor /
// return_factor is present.
[[nodiscard]] atx::core::Result<Dataset>
load_security_master_partitioned(std::string_view root_dir,
                                 std::span<const std::string> symbols,
                                 const DatasetSchema &schema,
                                 SharesPitRule shares_rule = SharesPitRule::RebasedRowDatedV2);

// Extract one symbol's parallel CorpActionColumns from a corporate-action
// Dataset built by the loaders above. `inst` must be one of the Dataset's
// instruments. Rows where the symbol is absent on a union date are emitted with
// the column's sentinel (0.0 dividend / NaN factor+shares / kNoSectorCode).
// Err(NotFound) if `inst` is not in the Dataset; Err(InvalidArgument) if the
// Dataset is not a corporate-action Dataset (wrong columns).
[[nodiscard]] atx::core::Result<CorpActionColumns> extract_symbol(const Dataset &corp_actions,
                                                                  InstKey inst);

} // namespace atx::engine::data
