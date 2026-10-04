#pragma once

// atx::engine::research::fields -- field gia_13f (P9 lane AL-SIG, lit F3; after Wermers, Yao and
// Zhao 2012, RFS 25(12)): the skill of a line's 13F holders, each manager's skill measured from the
// returns of its own past 13F holdings. An engine-only field (DEC-5: a registry row of kind engine
// naming this builder kind, no Python twin), on the 13F stage reader (sources/thirteenf.hpp).
//
// Per calendar quarter q, from the quarter positions of q-1 and q (thirteenf.hpp):
//   r_q(i)  = a_q(i) p_q(i) / p_(q-1)(i) - 1 (a the share-basis factor); a ratio outside
//             [1/10, 10] or a missing price: no return for i;
//   R_k(q)  = the value-weighted mean of r_q over filer k's pairs of q-1 with value > 0 that have
//             one, NaN when they hold less than half of k's total of q-1; R_agg(q) likewise over
//             every pair (the aggregate 13F book); e_k(q) = R_k(q) - R_agg(q);
//   s_k(P)  = the mean of e_k(q) over q = P-7..P, NaN with fewer than 6;
//   u_k(P)  = the centered average rank of s_k(P) among P's filers with a skill:
//             (rank - (m + 1) / 2) / m, every u undefined when m < 3;
//   v_i(P)  = sum_k w_ki u_k / sum_k w_ki over P's filers k with a skill holding i, w_ki = k's
//             value of i / k's total; NaN with fewer than 5 such holders (w_ki > 0).
// Clock 13f-quarter-skill-asof-v1: V(q) is the stage visibility (thirteenf.hpp); the value of P
// reads the positions of P-8..P, so it is visible at V*(P) = the maximum V(q) over the quarters
// q <= P with positions. Session t reads the latest P with V*(P) < date(t-1) 22:00 UTC (stage.hpp),
// P <= date(t) and date(t) - P <= 150 days in which the line has a mapped position (forward fill
// across a quarter without one); a NaN there stays NaN; session 0 -> NaN.
//
// The constants (GiaParams' defaults) were fixed before any value was read (lane AL-SIG, blind):
// 8 skill quarters (two years of quarterly holdings returns), at least 6 of them, at least 5
// skilled holders, at least 3 ranked managers (the house yz-short-term rule's m >= 3), half of a
// manager's value covered, the [1/10, 10] price-ratio screen of the 13F price outlier rule. No
// return statistic is recorded in the manifest: its source_checks carry counts only.

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/field_writer.hpp"
#include "atx/engine/research/fields/registry.hpp"
#include "atx/engine/research/fields/role_axes.hpp"
#include "atx/engine/research/fields/sources/thirteenf.hpp"

namespace atx::engine::research::fields {

struct GiaParams {
  usize skill_quarters{8};
  usize skill_min_quarters{6};
  usize min_holders{5};
  usize min_managers{3};
  f64 min_covered{0.5};
  f64 max_return_ratio{10.0};
};

// The declared spec (its text is the registry row's spec_text; the fingerprint pins both).
[[nodiscard]] const FieldSpec &gia_13f_spec();

// Holdings returns over quarter q of the positions of q-1 (prices of q-1 and q): one per filer of
// prev (aligned with prev.filers, NaN without one) and the aggregate book's.
struct HoldingsReturns {
  std::vector<f64> filer;
  f64 aggregate{};
};

[[nodiscard]] HoldingsReturns holdings_returns(const QuarterPositions &prev,
                                               const QuarterPositions &cur,
                                               const GiaParams &params);

// (rank - (m + 1) / 2) / m with ascending average ranks 1..m (ties share the mean rank).
[[nodiscard]] std::vector<f64> centered_ranks(std::span<const f64> values);

// v_i per security of `positions` (ascending sids) from the filer scores (aligned with
// positions.filers; NaN = no skill).
struct SecurityScores {
  std::vector<i64> sid;
  std::vector<f64> value;
};

[[nodiscard]] SecurityScores holder_scores(const QuarterPositions &positions,
                                           std::span<const f64> filer_score,
                                           const GiaParams &params);

// Per quarter counts (no return statistic).
struct GiaQuarterCounts {
  i64 period_day{};
  bool positions{false};
  u64 filers{};
  u64 pairs{};
  u64 rows_mapped{};
  u64 rows_price_outlier{};
  u64 filers_with_return{};
  u64 managers_ranked{};
  u64 securities_scored{};
};

// Quarters x role columns: the value v (NaN default), the mapped-position flag and V*.
struct GiaPanel {
  std::vector<i64> period_day;
  std::vector<i64> visible_ns; // V*(q); kNeverNs for a quarter without positions
  std::vector<f64> value;      // date-major: quarter q, column j at q * columns + j
  std::vector<u8> has;
  usize columns{};
  std::vector<GiaQuarterCounts> counts;
};

// Builds the panel one calendar quarter at a time (ascending and consecutive), holding only the
// previous quarter's positions and the managers' excess-return history.
class GiaAccumulator {
public:
  GiaAccumulator(std::span<const i64> ids, const GiaParams &params);

  // Adds the next quarter: its period, its stage visibility V(q) and its positions (nullopt when
  // the quarter is not used: no filing, sealed).
  void add(i64 period_day, i64 visible_ns, std::optional<QuarterPositions> positions);

  [[nodiscard]] const GiaPanel &panel() const noexcept { return panel_; }

private:
  [[nodiscard]] std::vector<f64> filer_scores(const QuarterPositions &positions, usize q,
                                              GiaQuarterCounts &counts) const;

  std::vector<i64> ids_;
  GiaParams params_;
  GiaPanel panel_;
  std::optional<QuarterPositions> prev_;
  // filer -> (quarter index, excess return) of its last quarters
  std::unordered_map<i64, std::vector<std::pair<usize, f64>>> excess_;
  i64 visible_so_far_{kBeforeAllNs};
};

// Row of a session dated `day` with visibility mark `mark` (stage.hpp previous_session_marks):
// `out` (panel.columns values). `fresh` (optional) gets 1 where a fresh visible quarter with a
// mapped position anchors the column.
void gia_row(const GiaPanel &panel, i64 day, i64 mark, std::span<f64> out,
             std::span<u8> fresh = {});

struct Gia13fField {
  WrittenField field;
  nlohmann::json checks; // source_checks block
};

// Reads the 13F stage for the role's span and writes output_dir/gia_13f.f64 (output_dir exists).
[[nodiscard]] core::Result<Gia13fField> build_gia_13f(PinnedStage &stage, const RoleAxes &role,
                                                      const std::filesystem::path &output_dir);

// The builder kind (registry.hpp): the field needs the spec's "thirteenf" stage; its entry's
// source is the stage manifest (which pins every stage file read).
[[nodiscard]] core::Result<FieldPlan> parse_gia_13f_kind(std::string_view field,
                                                         const nlohmann::json &options,
                                                         const BuildSpec &spec);
[[nodiscard]] core::Result<KindOutput> build_gia_13f_kind(const FieldPlan &plan,
                                                          BuildContext &ctx);

} // namespace atx::engine::research::fields
