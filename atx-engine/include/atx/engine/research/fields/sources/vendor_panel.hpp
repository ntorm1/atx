#pragma once

// atx::engine::research::fields -- the shared vendor panel (contract K-P9-2, source half; P9 lane
// A3, migration slice 4): the role's vendor TickerHistory3 parquet read once per build for every
// builder kind that needs it (BuildContext::sources).
//
// load():
//   1. hashes the file once (SHA-256): it must be the file the role was projected from (the role
//      manifest's source_sha256), and its (size, write time) must not move while it is hashed or
//      read;
//   2. checks the columns the request needs (tradingDate date32, securityID int64, close float32,
//      volume float64, cumulReturnFactor float64, shares int64, open / high / low float32 or
//      float64; research_fields_price.py TH_TYPES / OPEN_TYPES, research_fields_ohlc.py BAR_TYPES);
//   3. scans the row groups once over the union of requested columns, the seal pushed down: a row
//      group whose tradingDate statistics start on or after the research seal is never read (its
//      pages are never decompressed or decoded), nor is one wholly outside the axis window and
//      before the seal; in a read row group the key columns (tradingDate, securityID) are decoded
//      first, sealed rows and rows off the window, the role's lines or the axis calendar are
//      dropped (and counted) by key, and the value columns are decoded only when a row survives.
//      A parquet column chunk is the unit of decode, so a group that straddles the seal and keeps
//      a surviving pre-seal row has its value chunks decoded whole, its sealed rows' values
//      included: those values are dropped at decode, never read (only the surviving rows' indexes
//      are), so none reaches an observation, a matrix, a statistic or a message;
//      rows_sealed_value_decoded counts them;
//   4. applies the observation contract once (finite positive cumulReturnFactor, finite positive
//      close, finite volume >= 0, a unique (tradingDate, securityID) key: duplicate keys are
//      quarantined to NaN in every matrix), runs factor-break-v1 once (factor_break.hpp) and
//      divides every repaired step out of the factor.
//
// Matrices (axis rows x role lines, date-major), each present only when the request asks for it:
//   factor (f64) and close (f32): the price module's observation contract (NaN otherwise), the
//     factor chained (repaired steps divided out); requested by `price`;
//   shares (f32): vendor shares (thousands) of an observation with 0 < shares <= the A9 ceiling
//     (1e8), else NaN; first_above: the axis day of each line's first row above the ceiling
//     (C-81); requested by `shares`;
//   price_open (f32): the open of an observation with a finite positive open, else NaN
//     (`price_open`);
//   bar_open / bar_high / bar_low (f32): the vendor values as stored (the ohlc module reads them
//     without the observation contract; duplicates quarantined), requested by `bars`.
// The panel is immutable after load and safe to read from several threads.

#include <filesystem>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/sources/factor_break.hpp"
#include "atx/engine/research/fields/sources/nyse_calendar.hpp"

namespace atx::engine::research::fields {

// The vendor panel's A9 row ceiling of `shares` (thousands; prepare_research_fields.py
// THOUSANDS_ROW_CEILING).
inline constexpr f64 kSharesThousandsRowCeiling = 100'000'000.0;
inline constexpr i64 kNeverDay = std::numeric_limits<i64>::max();

// What one build's vendor kinds need; merge() is their union (the longest history wins).
struct VendorPanelRequest {
  usize pre_sessions{}; // NYSE rule sessions before the role (extended_axis)
  i64 lookback_days{};  // further calendar days of rule sessions before the first of them
  bool price{};         // factor and close, factor-break-v1
  bool shares{};        // shares and first_above (needs price)
  bool price_open{};    // the observation-contract open (needs price)
  bool bars{};          // the stored open, high and low

  void merge(const VendorPanelRequest &other) noexcept;
  [[nodiscard]] bool any() const noexcept { return price || shares || price_open || bars; }
};

// One decoded vendor row on the panel axis (after the window, line and calendar selection), its
// values at the column precision widened to f64; a null float is NaN, a null shares is 0.
struct VendorObservation {
  usize row{};
  usize line{};
  f64 factor{std::numeric_limits<f64>::quiet_NaN()};
  f32 close{std::numeric_limits<f32>::quiet_NaN()};
  f64 volume{std::numeric_limits<f64>::quiet_NaN()};
  i64 shares{};
  f64 open{std::numeric_limits<f64>::quiet_NaN()};
  f64 high{std::numeric_limits<f64>::quiet_NaN()};
  f64 low{std::numeric_limits<f64>::quiet_NaN()};
};

struct VendorScanStats {
  u64 rows_in_file{};
  u64 row_groups{};
  u64 row_groups_pruned_sealed{}; // tradingDate statistics start on or after the seal: never read
  u64 rows_in_row_groups_pruned_sealed{};
  u64 row_groups_pruned_outside_window{};
  u64 rows_in_row_groups_pruned_outside_window{};
  u64 row_groups_keys_decoded{};
  u64 row_groups_values_decoded{};
  u64 rows_keys_decoded{};
  // Every row dated on or after the seal (Python rows_on_or_after_seal_skipped): the rows of the
  // pruned sealed groups plus the sealed rows of a read group, dropped by key (values never used).
  u64 rows_sealed_dropped{};
  // Of those, the sealed rows of a read group whose value chunks were decoded (it straddles the
  // seal and kept a pre-seal row): decoded with their chunk, dropped unread. 0 when no sealed
  // value was decoded at all.
  u64 rows_sealed_value_decoded{};
  u64 rows_selected{};
  u64 rows_off_calendar{};
  u64 duplicate_keys_quarantined{};
  u64 shares_rows_above_a9_ceiling{};
  u64 shares_lines_withheld_c81{};
};

class VendorPanel {
public:
  // The panel of `role` from `source` (see above). Err(InvalidArgument) for a source that is not
  // the role's (SHA-256), a missing or mistyped column, or a request the role's calendar cannot
  // serve; Err(IoError) for a file that cannot be read or changes while it is read.
  [[nodiscard]] static core::Result<VendorPanel> load(const std::filesystem::path &source,
                                                      const RoleAxes &role,
                                                      const VendorPanelRequest &request);

  // The scan's back end: observations in, the panel out (tests assemble panels from rows).
  class Assembler {
  public:
    // Err(InvalidArgument) for an empty axis or no lines, or shares / price_open without price.
    [[nodiscard]] static core::Result<Assembler> create(ExtendedAxis axis, usize lines,
                                                        const VendorPanelRequest &request);
    // Precondition (asserted): row < axis rows, line < lines.
    void add(const VendorObservation &observation);
    // Quarantines duplicate keys, runs factor-break-v1 and chains the factor.
    [[nodiscard]] VendorPanel finish(SourceRecord source, VendorScanStats stats) &&;

  private:
    Assembler() = default;
    [[nodiscard]] usize cell(usize row, usize line) const noexcept { return row * lines_ + line; }

    ExtendedAxis axis_;
    usize lines_{};
    VendorPanelRequest request_;
    std::vector<u16> counts_;
    std::vector<f64> factor_;
    std::vector<f32> close_;
    std::vector<f32> shares_;
    std::vector<i64> first_above_;
    std::vector<f32> price_open_;
    std::vector<f32> bar_open_;
    std::vector<f32> bar_high_;
    std::vector<f32> bar_low_;
    u64 shares_rows_above_{};
  };

  VendorPanel(VendorPanel &&) noexcept = default;
  VendorPanel &operator=(VendorPanel &&) noexcept = default;
  VendorPanel(const VendorPanel &) = delete;
  VendorPanel &operator=(const VendorPanel &) = delete;
  ~VendorPanel() = default;

  [[nodiscard]] usize rows() const noexcept { return days_.size(); }
  [[nodiscard]] usize lines() const noexcept { return lines_; }
  [[nodiscard]] usize prefix() const noexcept { return prefix_; } // axis row of role row 0
  [[nodiscard]] std::span<const i64> days() const noexcept { return days_; }
  [[nodiscard]] const VendorPanelRequest &request() const noexcept { return request_; }

  // Cell accessors; preconditions: the matrix was requested, row < rows(), line < lines().
  [[nodiscard]] f64 factor(usize row, usize line) const noexcept { return factor_[at(row, line)]; }
  [[nodiscard]] f32 close(usize row, usize line) const noexcept { return close_[at(row, line)]; }
  [[nodiscard]] f32 shares(usize row, usize line) const noexcept { return shares_[at(row, line)]; }
  [[nodiscard]] f32 price_open(usize row, usize line) const noexcept {
    return price_open_[at(row, line)];
  }
  [[nodiscard]] f32 bar_open(usize row, usize line) const noexcept {
    return bar_open_[at(row, line)];
  }
  [[nodiscard]] f32 bar_high(usize row, usize line) const noexcept {
    return bar_high_[at(row, line)];
  }
  [[nodiscard]] f32 bar_low(usize row, usize line) const noexcept {
    return bar_low_[at(row, line)];
  }
  // The axis day of the line's first row above the shares ceiling, kNeverDay when none.
  [[nodiscard]] i64 first_above(usize line) const noexcept { return first_above_[line]; }

  [[nodiscard]] const FactorBreaks &factor_breaks() const noexcept { return breaks_; }
  [[nodiscard]] bool gap_crosses(usize line, usize lo, usize hi) const noexcept {
    return gaps_.crosses(line, lo, hi);
  }
  [[nodiscard]] usize kept_gap_steps() const noexcept { return gaps_.count(); }
  [[nodiscard]] usize repaired_steps() const noexcept;

  [[nodiscard]] const SourceRecord &source() const noexcept { return source_; }
  [[nodiscard]] const VendorScanStats &stats() const noexcept { return stats_; }

private:
  VendorPanel() = default;
  [[nodiscard]] usize at(usize row, usize line) const noexcept { return row * lines_ + line; }

  std::vector<i64> days_;
  usize prefix_{};
  usize lines_{};
  VendorPanelRequest request_;
  std::vector<f64> factor_;
  std::vector<f32> close_;
  std::vector<f32> shares_;
  std::vector<i64> first_above_;
  std::vector<f32> price_open_;
  std::vector<f32> bar_open_;
  std::vector<f32> bar_high_;
  std::vector<f32> bar_low_;
  FactorBreaks breaks_;
  KeptGaps gaps_;
  SourceRecord source_;
  VendorScanStats stats_;
};

// IEEE round-to-nearest of an f64 to f32 (numpy astype(float32)), with no out-of-range conversion:
// a finite value beyond the f32 range rounds to the largest finite f32 or to infinity as IEEE does.
[[nodiscard]] f32 to_f32(f64 value) noexcept;

} // namespace atx::engine::research::fields
