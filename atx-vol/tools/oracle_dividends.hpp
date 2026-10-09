#pragma once

// Recover a discrete cash-dividend SCHEDULE from the oracle store's `ddiv`
// column, and (opt-in) split the merged jumps a sparse expiry ladder produces.
//
// ## What `ddiv` is, and why differencing recovers a schedule
//
// `OracleRow::ddiv` is the SUM of the cash dividends whose ex-date falls at or
// before THAT option's expiry. Across one underlier's chain it is therefore a
// step FUNCTION of `years`: flat between ex-dates, stepping up by one dividend
// at the first expiry that includes it. Differencing consecutive expiries
// recovers the dividend amounts, and the `years` of the expiry where the step
// appears is the UPPER BRACKET on the ex-date.
//
// The upper bracket is not merely a bound — it is the ex-date. Measured on
// 14,357 SPY rows: placing each recovered dividend at the upper bracket priced
// to 6.96 ticks MAE against the vendor mark, while the bracket MIDPOINT (the
// natural "we only know an interval" choice) priced to 22.28. A schedule is
// only ever recovered to the resolution of the expiry ladder, and where that
// ladder is sparse this module says so rather than guessing (see the merged-jump
// splitter below).
//
// ## Fail closed, and COUNT the refusals
//
// A caller must be able to tell "reconstruction failed for this underlier" from
// "this underlier pays no dividends" — the two produce the same empty schedule
// and mean opposite things. So a group that violates the step-function shape is
// REFUSED whole, is listed by key and reason, and is tallied per reason in
// `DividendReconstruction::refusals`. Nothing is silently dropped and nothing
// partially reconstructed is returned.
//
// The grouping key is `(date, underlier)`: `ddiv` is a per-snapshot property of
// one underlier, so two dates of the same name are two independent schedules.
// Within a group the EXPIRY key is `years` itself — `OracleRow` carries no
// expiry date, and `years` is what the option was actually priced on.
//
// Thread-safety: every entry here is a pure function of its arguments.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/core/types.hpp"       // Result / ErrorCode
#include "atx/vol/api/pricing/american.hpp" // CashDividend — the pricer's own type
#include "oracle_cohort_reader.hpp"         // OracleRow

namespace atx::vol::oracle {

// Two tolerances, because "did `ddiv` change?" and "is the change a dividend?"
// are different questions and collapsing them hides a real defect class.
//
// A |change| at or below `kDdivFlatTol` is FLAT — it is the rounding noise a
// float column carries, sits three orders of magnitude above the ~1e-15
// telescoping residual measured across the SPY population, and must NOT refuse
// an otherwise clean underlier.
//
// A change ABOVE that but below `kMinDividendJump` is neither: too large to be
// noise, far too small to be cash (a sub-nano-dollar per-share dividend does not
// exist). That band is what `NonPositiveJump` refuses, rather than emitting a
// dividend nobody declared.
inline constexpr double kDdivFlatTol = 1.0e-12;
inline constexpr double kMinDividendJump = 1.0e-9;

// Why one group was refused. Listed in the order the scan tests them: the FIRST
// violation a group hits is the one recorded, and the group is abandoned there.
enum class DividendRefusal : std::uint8_t {
  // A row carried a non-finite `years` or `ddiv`. The cohort reader validates
  // finiteness upstream; this is the guard that keeps a bypassed or future
  // caller from differencing NaNs into a schedule.
  NonFiniteInput = 0,
  // Two rows share one `years` but disagree on `ddiv`. `ddiv` is a property of
  // the expiry, so this is not a step function at all and no differencing of it
  // means anything.
  AmbiguousDdivAtExpiry = 1,
  // `ddiv` DECREASED as `years` grew. A cumulative sum cannot shrink; a schedule
  // differenced out of it would carry a negative dividend.
  NonMonotoneDdiv = 2,
  // `ddiv` changed, but not by a usable positive amount: a change in
  // (0, kMinDividendJump) is too small to be cash and too large to be flat, so
  // it is neither emitted nor ignored.
  NonPositiveJump = 3,
};

[[nodiscard]] std::string_view to_string(DividendRefusal reason) noexcept;

// Per-reason refusal tally. `total()` is the number of REFUSED GROUPS, since a
// group is abandoned at its first violation.
struct DividendRefusalCounts {
  std::size_t non_finite_input = 0;
  std::size_t ambiguous_ddiv_at_expiry = 0;
  std::size_t non_monotone_ddiv = 0;
  std::size_t non_positive_jump = 0;

  [[nodiscard]] std::size_t total() const noexcept {
    return non_finite_input + ambiguous_ddiv_at_expiry + non_monotone_ddiv + non_positive_jump;
  }
};

// One reconstruction group: a snapshot date and an underlier.
struct DividendGroupKey {
  std::string date;
  std::string underlier;

  [[nodiscard]] friend bool operator==(const DividendGroupKey &,
                                       const DividendGroupKey &) = default;
};

// One accepted group's recovered schedule.
struct DividendSchedule {
  DividendGroupKey key;
  // Ascending in `tau`, every amount strictly positive. `tau` is the `years` of
  // the first expiry that included the dividend (the upper bracket).
  std::vector<CashDividend> dividends;
  std::size_t rows = 0;     // rows that fed this group
  std::size_t expiries = 0; // distinct `years` observed in it
};

// One refused group and what stopped it.
struct RefusedDividendGroup {
  DividendGroupKey key;
  DividendRefusal reason = DividendRefusal::NonFiniteInput;
  double years = 0.0; // the expiry the violation was detected at
};

struct DividendReconstruction {
  std::vector<DividendSchedule> schedules; // accepted groups, key-sorted
  std::vector<RefusedDividendGroup> refused;
  DividendRefusalCounts refusals;
  std::size_t groups_seen = 0;
  std::size_t rows_seen = 0;
};

// Reconstruct one schedule per `(date, underlier)` group.
//
// The differencing baseline is 0, so a group whose EARLIEST expiry already
// carries `ddiv > 0` emits that dividend at that expiry — the dividend is real
// and its upper bracket is the first expiry, exactly as for any later one.
// (A group whose earliest `ddiv` is negative is refused as NonMonotoneDdiv
// against that same baseline.)
//
// @param rows any order; grouping and per-group ordering are done here
// @return accepted schedules plus the refusal ledger. NEVER partial: a refused
//         group contributes no schedule at all.
[[nodiscard]] DividendReconstruction reconstruct_dividends(std::span<const OracleRow> rows);

// The invariant reconstruction is FOR: the sum of the scheduled dividends with
// ex-date at or before `years` is what an `OracleRow` at that expiry reports as
// `ddiv`. Measured to 1.8e-15 across all 14,357 SPY rows.
//
// Summation is in schedule order, which is the order the amounts were
// differenced out — the telescoping is what makes the residual ~1e-15 rather
// than ~1e-13.
[[nodiscard]] double accrued_dividend(std::span<const CashDividend> dividends,
                                      double years) noexcept;

// ── Merged jumps, and the OPT-IN cadence split ──────────────────────────────
//
// A reconstructed jump is MERGED when no listed expiry separated two ex-dates:
// the recovered amount is their SUM and the recovered tau is the LAST of them.
// SPY's far-dated ladder does this — its 2028 expiries recover +4.00 and +4.50,
// which are two quarters each. Left merged, those two rows priced the 2028
// expiries to 33.95 ticks MAE; split onto the true ex-dates, 4.90.
//
// Splitting needs knowledge the `ddiv` column does not contain — the issuer's
// cadence — so it is a SEPARATE step a caller opts into, never something
// `reconstruct_dividends` does on its own. Every reconstructed SPY ex-date fell
// on the 3rd Friday of a quarter-end month; rather than teach this module that
// calendar (which is an assumption about ONE underlier), the split is expressed
// as a nominal PERIOD, and the part count is read off the gap to the previous
// ex-date. On the SPY schedule that places the split ex-dates within 2.2e-4
// years of the true 3rd Fridays — four orders of magnitude below the tau
// placement error the measurement above is sensitive to.
struct DividendCadence {
  // Nominal spacing between consecutive ex-dates, in the same year-fraction
  // units as `tau`. 0.25 is quarterly, the US large-cap norm.
  double period_years = 0.25;
  // Bounded-loop guard (JPL rule 2) AND a refusal: a gap implying more parts
  // than this is not a cadence this rule can resolve, so it fails closed.
  int max_parts = 4;
};

// ── The snapshot-keyed schedule index (the sweep / bench pre-pass) ──────────
//
// The convention layer prices one row at a time, but a schedule is a property
// of a whole CHAIN. This index is the pre-pass that closes the gap: built ONCE
// per cohort scan, before any candidate is evaluated, because the schedules are
// candidate-independent and rebuilding them per grid point is pure waste.
//
// KEYED BY (date, bucket_et, underlier), not by (date, underlier): a
// `CashDividend::tau` is "a year-fraction measured from the SAME valuation
// instant as the option's `T`" (american.hpp), and the valuation instant of a
// stored row is its BUCKET. Two buckets of one date are two clocks hours
// apart, so pooling them would (a) smear every recovered tau by the
// inter-bucket year-fraction and (b) let an intraday `ddiv` update refuse the
// whole day as AmbiguousDdivAtExpiry when each snapshot alone is clean. Each
// bucket sees the full chain, so nothing is lost by the finer key.
//
// FAIL CLOSED, per row: `for_row` reports `refused = true` both for a group
// reconstruction actually refused AND for a row whose snapshot the build never
// saw — a caller holding a row this index cannot vouch for must not price it as
// if it could. Refusals are surfaced as RUN-LEVEL AGGREGATE COUNTS ONLY: the
// group keys are cohort membership, and membership must never reach an
// aggregate receipt.
class DividendScheduleIndex {
public:
  // What one row is entitled to. `schedule` is the row's whole-chain snapshot
  // schedule (possibly empty: "pays no dividends" — distinguishable from
  // `refused`, which is "reconstruction cannot vouch for this snapshot").
  struct RowSchedule {
    std::span<const CashDividend> schedule{};
    bool refused = false;
  };

  // Run-level aggregates, the ONLY reconstruction facts a receipt may carry.
  struct Aggregates {
    std::int64_t rows_seen = 0;
    std::int64_t groups_seen = 0;
    std::int64_t groups_refused = 0;
    std::int64_t rows_in_refused_groups = 0;
    DividendRefusalCounts refusals;
  };

  DividendScheduleIndex() = default;

  // One reconstruction per (date, bucket_et, underlier) snapshot over the
  // concatenation of the two spans (two, because the sweep holds smoke and tune
  // as separate scans; a snapshot present in both reconstructs once from the
  // union, and duplicated rows collapse in the reconstructor). Row order is
  // irrelevant.
  [[nodiscard]] static DividendScheduleIndex build(std::span<const OracleRow> first,
                                                   std::span<const OracleRow> second = {});

  // The returned span stays valid for the life of this index.
  [[nodiscard]] RowSchedule for_row(const OracleRow &row) const noexcept;

  [[nodiscard]] const Aggregates &aggregates() const noexcept { return aggregates_; }

private:
  struct Entry {
    std::string date;
    std::string bucket_et;
    std::string underlier;
    std::vector<CashDividend> schedule;
    bool refused = false;
  };

  // Sorted by (date, bucket_et, underlier); for_row binary-searches it.
  std::vector<Entry> entries_;
  Aggregates aggregates_;
};

// Split every merged jump into `round(gap / period)` equal instalments placed
// backwards from the recovered tau on `cadence.period_years`.
//
// The LAST instalment stays exactly on the recovered tau — that one is not an
// inference, it is the upper bracket the ladder actually gave. Only the earlier
// ones are placed by the cadence rule.
//
// The FIRST dividend of a schedule is never split: with no previous ex-date
// there is no gap to read a part count from, so a merge there is undetectable
// and this returns it untouched rather than guessing.
//
// @param dividends ascending in `tau`, all amounts > 0 (i.e. the shape
//        `reconstruct_dividends` emits)
// @param cadence   period and part cap
// @return the expanded schedule, or an Error:
//   InvalidArgument — non-finite / non-positive `period_years`, `max_parts` < 1,
//                     or an input schedule that is not strictly ascending in
//                     `tau` with finite positive amounts
//   OutOfRange      — a gap implies more than `max_parts` instalments, or a
//                     placed instalment would land at or before tau 0
[[nodiscard]] Result<std::vector<CashDividend>>
split_merged_dividends(std::span<const CashDividend> dividends, const DividendCadence &cadence);

} // namespace atx::vol::oracle
