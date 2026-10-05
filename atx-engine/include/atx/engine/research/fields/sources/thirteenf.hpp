#pragma once

// atx::engine::research::fields -- the 13F stage reader (P9 lane AL-SIG, ruling P18; lane A4 reuses
// it): the atx-db alpha-panel thirteenf/ stage read into per-quarter manager positions with the
// house's 13F rules, ported from research_fields_holdings.py (F13_EFFECTIVE_RULE, F13_ROWS_RULE,
// F13_CLOCK, source_part_is_sealed) and research_fields_mgr13f.py (quarter_positions, BASIS_RULE).
//
//   * Quarters: thirteenf_quarters() lists the calendar quarter ends a build reads.
//   * Visibility V(P) (13f-filed-plus-46h-v1): the maximum available_at over every 13F filing of P
//     filed by its deadline (any submission type, notices included); a quarter with no such filing,
//     or with V(P) on or after the research seal, is never used (kNeverNs; sealed ones counted).
//   * Effective filings (13f-asof45-effective-v1): per (filer CIK, period) among 13F-HR / 13F-HR/A
//     filed by the deadline, the latest (filing_date, accession) 13F-HR or RESTATEMENT plus every
//     NEW HOLDINGS amendment after it.
//   * Reader-side seal: a holdings part parts/source=YYYYqN whose quarter begins on or after the
//     seal is never opened (counted).
//   * Position rows: holdings rows of the effective filings with sshprnamt_type SH, no put/call,
//     shares > 0 and a finite value_usd >= 0; value x filing_checks.unit_factor (1 when unlisted,
//     not finite or not positive); CUSIP -> security_id by cusip_map_pit at the same period
//     (-1 when unmapped; the first map row of a CUSIP wins).
//   * Quarter positions: mapped rows; a priced row (value > 0) whose implied price value / shares
//     lies outside [1/10, 10] x its security's median implied price is dropped; (filer, security)
//     pairs summed; each security's median implied price; each filer's total kept value.
//   * Share-basis factors (13f-basis-change-v1): per security, s = the median of N_q / N_(q-1) over
//     the filers holding it in both quarters; a = s when at least 5 such filers, s outside
//     [1/1.2, 1.2] and s x p_q / p_(q-1) inside [1/1.5, 1.5], else 1.
//
// Every stage file is read through a PinnedStage (stage.hpp). Cold path; not thread-safe.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/sources/stage.hpp"

namespace atx::engine::research::fields {

inline constexpr StageContract kThirteenfStage{"thirteenf", "atx.alpha-panel.thirteenf/v1",
                                               "13f-filed-plus-46h-v1", 150, ""};
inline constexpr i64 kThirteenfStaleDays = 150;
inline constexpr f64 kThirteenfPriceOutlier = 10.0;
inline constexpr f64 kBasisShareTolerance = 1.2;
inline constexpr f64 kBasisValueTolerance = 1.5;
inline constexpr usize kBasisMinHolders = 5;

// One filings.parquet row (nulls as parquet_columns.hpp maps them).
struct ThirteenfFiling {
  std::string accession;
  std::string submission_type;
  std::string amendment_type;
  std::string source_period;
  i64 filer{-1};
  i64 period_day{};
  i64 filing_day{};
  i64 deadline_day{};
  i64 available_ns{kNeverNs};
};

struct UnitFactorRow {
  std::string accession;
  f64 factor{};
};

struct CusipMapRow {
  i64 period_day{};
  std::string cusip;
  i64 security_id{-1};
};

struct ThirteenfQuarter {
  i64 period_day{};
  bool has_filing{false};   // a 13F filing of the quarter filed by its deadline
  bool sealed{false};       // has_filing and V(P) on or after the seal (or unknown): never used
  i64 visible_ns{kNeverNs}; // V(P); kNeverNs without a filing or when sealed
  std::vector<usize> effective;                       // ThirteenfIndex::filings indices
  std::vector<std::pair<std::string, i64>> cusip_sid; // ascending by CUSIP
};

struct ThirteenfCounts {
  u64 filings_after_deadline_ignored{};
  u64 effective_filings{};
  u64 quarters_sealed{};
  u64 holdings_parts_not_read_sealed{};
  u64 holdings_rows_kept{};
};

// What a build reads once: the quarters' filings and visibility, the unit factors and the CUSIP
// maps.
struct ThirteenfIndex {
  std::vector<ThirteenfFiling> filings;    // the filings whose period is one of the quarters
  std::vector<ThirteenfQuarter> quarters;  // one per quarter end, ascending
  std::vector<std::pair<std::string, f64>> unit_factor; // ascending by accession (first row wins)
  ThirteenfCounts counts;

  // The usable unit factor of an accession: its first row's factor when finite and positive,
  // else 1.
  [[nodiscard]] f64 unit_factor_of(std::string_view accession) const noexcept;
};

// Calendar quarter ends (calendar days since 1970-01-01).
[[nodiscard]] i64 quarter_end_on_or_after(i64 day) noexcept;
[[nodiscard]] i64 previous_quarter_end(i64 quarter_end) noexcept;
[[nodiscard]] i64 next_quarter_end(i64 quarter_end) noexcept;

// The quarter ends a build on role sessions [first_day, last_day] reads: q0 = the quarter end on or
// after first_day - 150, the `history` quarter ends before q0, then q0 and every later quarter end
// before last_day (research_fields_holdings.py needed_quarters with its history prefix).
[[nodiscard]] std::vector<i64> thirteenf_quarters(i64 first_day, i64 last_day, usize history);

// True for a part name YYYYqN whose calendar quarter begins on or after the research seal; a part
// name of another shape is not dated and not refused here.
[[nodiscard]] bool thirteenf_part_is_sealed(std::string_view part) noexcept;

// F13_EFFECTIVE_RULE over rows already restricted to 13F-HR / 13F-HR/A filed by the deadline: one
// flag (1 = effective) per row.
[[nodiscard]] std::vector<u8> effective_filings(std::span<const ThirteenfFiling> rows);

// The index of `quarter_days` (ascending quarter ends) from decoded rows (pure).
[[nodiscard]] ThirteenfIndex index_thirteenf(std::span<const i64> quarter_days,
                                             std::vector<ThirteenfFiling> filings,
                                             std::span<const UnitFactorRow> factors,
                                             std::span<const CusipMapRow> cusips);

// Decodes filings.parquet, filing_checks.parquet and cusip_map_pit.parquet of the stage and
// indexes them (rows of other periods are not kept).
[[nodiscard]] core::Result<ThirteenfIndex> read_thirteenf_index(PinnedStage &stage,
                                                                std::span<const i64> quarter_days);

// One screened holdings row: filer CIK, security_id (-1 unmapped), shares and value in USD.
struct PositionRow {
  i64 filer{-1};
  i64 sid{-1};
  f64 shares{};
  f64 value{};
};

// One decoded holdings row before the screen.
struct HoldingRow {
  std::string_view accession;
  std::string_view cusip;
  std::string_view sshprnamt_type;
  std::string_view put_call; // "" for null
  f64 shares{};              // NaN for null
  f64 value{};               // NaN for null
};

// The effective filings of one quarter by accession (ascending): filer CIK and usable unit factor.
struct QuarterFilers {
  std::vector<std::string> accession;
  std::vector<i64> filer;
  std::vector<f64> factor;
};

[[nodiscard]] QuarterFilers quarter_filers(const ThirteenfIndex &index, usize q);

// The position row of a holdings row of `quarter`, nullopt when F13_ROWS_RULE drops it (not one of
// the quarter's effective filings, not SH, a put / call, shares <= 0 or not finite, a value not
// finite or negative).
[[nodiscard]] std::optional<PositionRow> screen_holding(const QuarterFilers &filers,
                                                        const ThirteenfQuarter &quarter,
                                                        const HoldingRow &row);

// The screened rows of quarter `q` read from the holdings parts of its effective filings (sealed
// parts never opened, counted in `counts`); empty for a quarter that is not used.
[[nodiscard]] core::Result<std::vector<PositionRow>> read_quarter_rows(PinnedStage &stage,
                                                                     const ThirteenfIndex &index,
                                                                     usize q,
                                                                     ThirteenfCounts &counts);

struct QuarterPositions {
  std::vector<i64> filers;      // ascending
  std::vector<f64> filer_value; // per filer: the sum of its kept pair values
  std::vector<i64> pair_filer;  // pairs ascending by (filer, security)
  std::vector<i64> pair_sid;
  std::vector<f64> pair_shares;
  std::vector<f64> pair_value;
  std::vector<i64> price_sid; // ascending
  std::vector<f64> price;     // the median implied price of the kept priced rows
  u64 rows_mapped{};
  u64 rows_price_outlier{};
};

[[nodiscard]] QuarterPositions quarter_positions(std::vector<PositionRow> rows,
                                                 f64 outlier = kThirteenfPriceOutlier);

// The quarter's price of `sid`, NaN when it has none.
[[nodiscard]] f64 price_of(const QuarterPositions &positions, i64 sid) noexcept;

// The share-basis factor a of every security of `sids` (any order) for consecutive quarters prev ->
// cur.
[[nodiscard]] std::vector<f64> basis_factors(const QuarterPositions &prev,
                                             const QuarterPositions &cur,
                                             std::span<const i64> sids);

// The source_checks block of the index: counts, rules and each quarter's visibility.
[[nodiscard]] nlohmann::json thirteenf_checks(const ThirteenfIndex &index);

} // namespace atx::engine::research::fields
