#pragma once

// atx::engine::research::fields -- the short-side holdings fields of research_fields_holdings.py
// in C++ (P9 lane A4, migration slice 5): ftd_shares_ratio21 (kind ftd, the ftd/ stage and this
// run's shares_out) and regsho_threshold_days63 (kind regsho, the regsho_threshold/ and
// security_master/ stages). The spec text of each is the registry's (field_registry.json
// spec_text; the fingerprint test pins every copy), the caveats the Python HOLD_FIELDS', the
// arithmetic the Python's operation for operation (FTD: np.add.at in row order into a zero
// matrix, numpy's sequential axis-0 cumsum, a window sum as the difference of two prefix sums, one
// division), and the payload, its coverage, its NaN reasons and its source_checks block (the
// Python source_checks.holdings.<stage key>) must equal the Python builder's (gtest
// ResearchFieldsHoldings*).
//
// Visibility (stage.hpp kStageVisibilityRule): a source row is usable at role session t iff its
// available_at < date(t-1) 22:00 UTC (previous_session_marks), so session 0 is NaN. The kernels
// below take that mark as an argument: the builders pass the role's previous-session marks, and
// the planted-leak probes pass the session's own mark (one session late) to show the probe fails.
//
// Entry: sources are the stage manifests (each pins every stage file) and, for ftd, the
// shares_out payload, so the reuse rule's re-hash covers every input; the files read are named in
// source_checks.<field>.stage_files (the Python entry lists them in sources). Extras, as the
// Python entry: visibility_rule, stage_manifest_sha256 {stage key: sha}, seal_date, depends_on
// (ftd: ["shares_out"]) and nan_reasons_member_cells.

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/field_writer.hpp"
#include "atx/engine/research/fields/registry.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/run_payload.hpp"
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
// and the role lines' fail quantities as (dates + 1) x columns inclusive prefix sums (row 0 zero).
struct FtdPanel {
  std::vector<i64> calendar;
  std::vector<i64> prefix_visible_ns;
  std::vector<f64> cumulative; // row-major: settlement row k + 1 at (k + 1) * columns
  usize columns{};
  nlohmann::json checks; // the Python source_checks.holdings.ftd block
};

// Builds the panel from the decoded year files, one at a time in file order (bounded memory: only
// the role lines' rows are kept): sealed rows are dropped first (they reach no clock), then
// collision rows, unmapped ids (<= 0), ids off the role and non-finite quantities. `role` must
// outlive the indexer.
class FtdIndexer {
public:
  explicit FtdIndexer(const RoleAxes &role);
  void add(const FtdRows &rows, std::string_view name);
  // Err(InvalidArgument) when no settlement date remains.
  [[nodiscard]] core::Result<FtdPanel> finish();

private:
  struct Kept {
    i64 day{};
    usize column{};
    f64 quantity{};
  };
  const RoleAxes *role_; // non-owning, non-null
  std::map<i64, i64> visible_; // settlement day -> the latest available_at of its unsealed rows
  std::vector<Kept> kept_;
  std::vector<std::string> names_;
  u64 rows_{};
  u64 rows_sealed_{};
  u64 rows_on_role_{};
  u64 rows_collision_{};
  u64 rows_unmapped_{};
};

struct FtdReasons {
  u64 no_visible_window{};
  u64 stale{};
  u64 shares_out_nan{};
  u64 short_history{};
};

// Role row t at visibility mark `mark`: the 21-date window ending at the latest settlement date
// whose prefix is visible before the mark, over this run's shares_out at the last role session on
// or before that date. `out` and `member` hold role.instruments() values.
void ftd_row(const FtdPanel &panel, const RoleAxes &role, const RunPayload &shares_out, usize t,
             i64 mark, std::span<const u8> member, FtdReasons &reasons, std::span<f64> out);

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

// Builds the panel from the lists (at construction: the calendar and the list visibility), then the
// threshold year files one at a time in file order. A row later than its list raises the list's
// visibility; the rows of one file are compared with the visibility before that file's raises.
// `role` must outlive the indexer.
class RegshoIndexer {
public:
  RegshoIndexer(const RegshoListRows &lists, const RoleAxes &role);
  [[nodiscard]] std::span<const i64> calendar() const noexcept { return panel_.calendar; }
  void add(const ThresholdRows &rows, std::string_view name);
  [[nodiscard]] RegshoPanel finish();

private:
  const RoleAxes *role_; // non-owning, non-null
  RegshoPanel panel_;
  std::vector<std::string> names_;
  u64 rows_used_{};
  u64 late_rows_{};
};

// The FINRA name rows of the role's lines in visibility order (available_at ascending, stable),
// each row's line column, dissemination day and listing market (kRegshoMarkets index, -1 for a
// class without a list source); rows of other lines, unmapped ids and rows available on or after
// the seal are dropped.
struct ListingRows {
  std::vector<i64> available_ns;
  std::vector<i64> dissemination_day;
  std::vector<usize> column;
  std::vector<i64> market;
};
[[nodiscard]] ListingRows index_listing_rows(const FinraNameRows &names, const RoleAxes &role);

struct RegshoReasons {
  u64 market_unknown{};
  u64 market_lists_incomplete{};
  u64 short_calendar{};
};

// Role row t at visibility mark `mark`, given each line's latest visible name row (`latest`, -1
// none, as LatestRows over `listing` gives it at that mark). `counts_by_market` (4 values) gets the
// finite member cells per listing market.
void regsho_row(const RegshoPanel &panel, const ListingRows &listing, std::span<const i64> latest,
                const RoleAxes &role, usize t, i64 mark, std::span<const u8> member,
                RegshoReasons &reasons, std::span<u64> counts_by_market, std::span<f64> out);

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
