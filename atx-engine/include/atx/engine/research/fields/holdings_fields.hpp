#pragma once

// atx::engine::research::fields -- the short-side holdings fields of research_fields_holdings.py
// in C++ (P9 lane A4, migration slice 5): ftd_shares_ratio21 (kind ftd, the ftd/ stage and this
// run's shares_out) and regsho_threshold_days63 (kind regsho, the regsho_threshold/ and
// security_master/ stages). The spec text of each is the registry's (field_registry.json
// spec_text; the fingerprint test pins every copy), the caveats the Python HOLD_FIELDS', the
// arithmetic the Python's operation for operation (FTD: np.add.at in row order into a zero row,
// numpy's sequential axis-0 cumsum, a window sum as the difference of two prefix rows, one
// division), and the payload, its coverage, its NaN reasons and its source_checks block (the
// Python source_checks.holdings.<stage key>) must equal the Python builder's (gtest
// ResearchFieldsHoldings*).
//
// Visibility (stage.hpp kStageVisibilityRule): a source row is usable at role session t iff its
// available_at < date(t-1) 22:00 UTC (previous_session_marks), so session 0 is NaN. The kernels
// below take that mark as an argument: the builders pass the role's previous-session marks, and
// the planted-leak probes pass the session's own mark (one session late) to show the probe fails.
//
// Reading (sources/short_side.hpp): every stage file through the one short-side reader, the year
// files one parquet row group at a time into the indexers; memory is the role lines' rows, one
// entry per calendar date and the kernels' scratch, never a whole year file or a (dates x lines)
// matrix of fails.
//
// Entry: sources are the stage manifests (each pins every stage file) and, for ftd, the
// shares_out payload, so the reuse rule's re-hash covers every input; the files read are named in
// source_checks.<field>.stage_files (the Python entry lists them in sources). Extras, as the
// Python entry: visibility_rule, stage_manifest_sha256 {stage key: sha}, seal_date, depends_on
// (ftd: ["shares_out"]) and nan_reasons_member_cells.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/field_writer.hpp"
#include "atx/engine/research/fields/registry.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/run_payload.hpp"
#include "atx/engine/research/fields/session_clock.hpp"
#include "atx/engine/research/fields/sources/short_side.hpp"
#include "atx/engine/research/fields/sources/stage.hpp"

namespace atx::engine::research::fields {

inline constexpr usize kFtdWindow = 21;                 // FTD_WINDOW
inline constexpr i64 kFtdStaleDays = 60;                // FTD_STALE_DAYS
inline constexpr usize kRegshoWindow = 63;              // REGSHO_WINDOW
inline constexpr usize kRegshoMinLists = 60;            // REGSHO_MIN_LISTS
inline constexpr i64 kRegshoNameMaxAgeDays = 45;        // REGSHO_NAME_MAX_AGE_DAYS
inline constexpr usize kRegshoPrefixLists = kRegshoWindow + 2; // list dates before the role

// The declared specs (registry spec_text; caveats from HOLD_FIELDS).
[[nodiscard]] const FieldSpec &ftd_shares_ratio21_spec();
[[nodiscard]] const FieldSpec &regsho_threshold_days63_spec();

// What every holdings builder returns: the payload, the entry's sources, its extra keys and its
// source_checks block.
struct HoldingsField {
  WrittenField field;
  std::vector<SourceRecord> sources;
  nlohmann::json extra;
  nlohmann::json source_checks;
};

// ---- ftd_shares_ratio21 ----------------------------------------------------------------------

// The settlement panel of the stage's year files: the distinct settlement dates (the settlement
// calendar), each date's prefix visibility (the running maximum of the dates' latest available_at)
// and the role lines' fails grouped by settlement date in read order (date k: fail rows
// date_begin[k] .. date_begin[k + 1]).
struct FtdPanel {
  std::vector<i64> calendar;
  std::vector<i64> prefix_visible_ns;
  std::vector<usize> date_begin; // calendar.size() + 1 offsets into the fail rows
  std::vector<u32> fail_column;
  std::vector<f64> fail_quantity;
  usize columns{};
  nlohmann::json checks; // the Python source_checks.holdings.ftd block
};

// Builds the panel from the year files' batches in read order (read_short_side; bounded memory:
// only the role lines' fails and one entry per settlement date are kept): sealed rows are dropped
// first (they reach no clock), then collision rows, unmapped ids (<= 0), ids off the role and
// non-finite quantities. `role` must outlive the indexer.
class FtdIndexer {
public:
  explicit FtdIndexer(const RoleAxes &role);
  // One batch (a parquet row group) of the current year file.
  void add(const FtdRows &rows);
  // The year file whose batches were just added (its stage-relative name, for the checks).
  void end_file(std::string_view name);
  // Err(InvalidArgument) when no settlement date remains.
  [[nodiscard]] core::Result<FtdPanel> finish();

private:
  void see(i64 day, i64 available);

  const RoleAxes *role_; // non-owning, non-null
  // settlement day -> the latest available_at of its unsealed rows, ascending by day
  std::vector<std::pair<i64, i64>> visible_;
  usize last_{}; // the slot of the last day seen (a file's rows come grouped by date)
  std::vector<i64> kept_day_;
  std::vector<u32> kept_column_;
  std::vector<f64> kept_quantity_;
  std::vector<std::string> names_;
  u64 rows_{};
  u64 rows_sealed_{};
  u64 rows_collision_{};
  u64 rows_unmapped_{};
};

struct FtdReasons {
  u64 no_visible_window{};
  u64 stale{};
  u64 shares_out_nan{};
  u64 short_history{};
};

// The ftd_shares_ratio21 row kernel over one panel. A window sum is the difference of two prefix
// rows of numpy's (dates + 1) x lines cumsum of np.add.at (prefix row k + 1 = prefix row k plus
// date k's fails added in read order into a zero row), bit for bit; only the last kFtdWindow + 1
// prefix rows are kept (memory (kFtdWindow + 3) x lines; each row computed once while the anchor
// grows, as it does with the mark; an anchor before an evicted row recomputes from row 0, same
// bits). Not thread-safe. `panel`, `role` and `shares_out` (on the role's shape) must outlive it.
class FtdKernel {
public:
  FtdKernel(const FtdPanel &panel, const RoleAxes &role, const RunPayload &shares_out);
  // Role row t at visibility mark `mark`: the 21-date window ending at the latest settlement date
  // whose prefix is visible before the mark, over this run's shares_out at the last role session
  // on or before that date. `member` and `out` hold role.instruments() values.
  void row(usize t, i64 mark, std::span<const u8> member, FtdReasons &reasons,
           std::span<f64> out);

private:
  // The sums of the kFtdWindow dates ending at settlement row `anchor` (anchor + 1 >= kFtdWindow).
  [[nodiscard]] std::span<const f64> window(usize anchor);
  void advance(); // prefix row next_ from row next_ - 1 and date next_ - 1's fails
  [[nodiscard]] std::span<f64> ring_row(usize prefix_row) noexcept;

  const FtdPanel *panel_;        // non-owning, non-null
  const RoleAxes *role_;         // non-owning, non-null
  const RunPayload *shares_out_; // non-owning, non-null
  std::vector<f64> ring_;        // (kFtdWindow + 1) x columns: prefix row p at slot p % 22
  std::vector<f64> adds_;        // one date's fails
  std::vector<f64> sums_;        // the last window
  usize next_{1};                // prefix rows < next_ computed (row 0: zeros)
  usize sums_anchor_{};
  bool sums_valid_{false};
};

[[nodiscard]] core::Result<HoldingsField>
build_ftd_shares_ratio21(PinnedStage &stage, const RunPayload &shares_out, const RoleAxes &role,
                         const std::filesystem::path &output_dir);

// ---- regsho_threshold_days63 -----------------------------------------------------------------

// The list calendar (the last 65 list dates of a usable list before the role, then the role's
// sessions), each market's list visibility per calendar date (the earliest available_at of its
// usable list, raised by a later on-list row of that date; kNeverNs without one), and per date and
// line the market index + 1 of a usable on-list row (0 none).
struct RegshoPanel {
  std::vector<i64> calendar;
  usize prefix{};
  std::vector<i64> list_visible_ns; // market-major: market m, date k at m * calendar.size() + k
  std::vector<u8> on_market;        // date-major: date k, line j at k * columns + j
  usize columns{};
  nlohmann::json checks; // the Python source_checks.holdings.regsho_threshold block (lists part)
};

// Builds the panel from the lists (at construction: the calendar and the list visibility), then
// the threshold year files' batches in read order. Per file (the Python's unit): every usable
// on-list row of the role's lines is compared with the list visibility before that file's raises;
// at end_file a row later than its list raises the list's visibility, then the rows mark their
// cells in read order (the last row of a cell wins). `role` must outlive the indexer.
class RegshoIndexer {
public:
  RegshoIndexer(const RegshoListRows &lists, const RoleAxes &role);
  [[nodiscard]] std::span<const i64> calendar() const noexcept { return panel_.calendar; }
  void add(const ThresholdRows &rows);
  void end_file(std::string_view name);
  [[nodiscard]] RegshoPanel finish();

private:
  struct Held {
    usize date{};
    usize column{};
    usize market{};
    i64 available{};
    bool late{};
  };

  const RoleAxes *role_; // non-owning, non-null
  RegshoPanel panel_;
  std::vector<Held> held_; // the current file's usable rows (cleared per file, capacity kept)
  std::vector<std::string> names_;
  u64 rows_used_{};
  u64 late_rows_{};
};

// The FINRA name rows of the role's lines in visibility order (available_at ascending, stable in
// read order), each row's line column, dissemination day and listing market (kRegshoMarkets index,
// -1 for a class without a list source); rows of other lines, unmapped ids and rows available on
// or after the seal are dropped.
struct ListingRows {
  std::vector<i64> available_ns;
  std::vector<i64> dissemination_day;
  std::vector<usize> column;
  std::vector<i64> market;
};

// Gathers ListingRows from finra_names batches in read order. `role` must outlive the indexer.
class ListingIndexer {
public:
  explicit ListingIndexer(const RoleAxes &role);
  void add(const FinraNameRows &rows);
  [[nodiscard]] ListingRows finish();

private:
  const RoleAxes *role_; // non-owning, non-null
  ListingRows rows_;     // read order until finish
};

struct RegshoReasons {
  u64 market_unknown{};
  u64 market_lists_incomplete{};
  u64 short_calendar{};
};

// The regsho_threshold_days63 row kernel: each line's listing market from its latest name row
// visible at the mark (LatestRows over the listing; marks non-decreasing across calls, the Python
// pointer's rule), the 63-session counts summed list date by list date over contiguous panel
// rows. Scratch sized once. Not thread-safe. `panel`, `listing` and `role` must outlive it.
class RegshoKernel {
public:
  // Err(InvalidArgument) for listing rows out of visibility order or off the role's columns.
  [[nodiscard]] static core::Result<RegshoKernel>
  create(const RegshoPanel &panel, const ListingRows &listing, const RoleAxes &role);

  // Role row t at visibility mark `mark`; `counts_by_market` (kRegshoMarkets.size() values) gets
  // the finite member cells per listing market. `member` and `out` hold role.instruments() values.
  void row(usize t, i64 mark, std::span<const u8> member, RegshoReasons &reasons,
           std::span<u64> counts_by_market, std::span<f64> out);

private:
  RegshoKernel(const RegshoPanel &panel, const ListingRows &listing, const RoleAxes &role,
               LatestRows latest);

  const RegshoPanel *panel_;   // non-owning, non-null
  const ListingRows *listing_; // non-owning, non-null
  const RoleAxes *role_;       // non-owning, non-null
  LatestRows latest_;
  std::vector<u32> counts_; // per line: visible on-list window sessions
};

[[nodiscard]] core::Result<HoldingsField>
build_regsho_threshold_days63(PinnedStage &lists_stage, PinnedStage &names_stage,
                              const RoleAxes &role, const std::filesystem::path &output_dir);

// ---- the builder kinds (registry.hpp) ---------------------------------------------------------

// ftd_shares_ratio21 needs the spec's ftd stage and shares_out; regsho_threshold_days63 its
// regsho_threshold and security_master stages. Each plan's inputs are its entry's sources.
[[nodiscard]] core::Result<FieldPlan> parse_ftd_kind(std::string_view field,
                                                     const nlohmann::json &options,
                                                     const BuildSpec &spec);
[[nodiscard]] core::Result<KindOutput> build_ftd_kind(const FieldPlan &plan, BuildContext &ctx);
[[nodiscard]] core::Result<FieldPlan> parse_regsho_kind(std::string_view field,
                                                        const nlohmann::json &options,
                                                        const BuildSpec &spec);
[[nodiscard]] core::Result<KindOutput> build_regsho_kind(const FieldPlan &plan,
                                                         BuildContext &ctx);

} // namespace atx::engine::research::fields
