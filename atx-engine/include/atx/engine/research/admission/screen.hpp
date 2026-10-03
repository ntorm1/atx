#pragma once

// atx::engine::research::admission -- the prior-signed admission screen of the composition fit
// (platform P9 B1, DEC-7; migration plan section 1 `admission/`), moved from
// fit_composition_weights.py screen_v4 (fit:1613-1671) with its statistics (_stats :1491,
// pair_correlation :1502, newey_west_t :1572-1583).
//
// Per candidate k (factor series f_k over decisions, NaN = flat; the K-P9-4 factor.f64), over its
// live TRAIN decisions (finite f_k and train_mask):
//   s_k          the prior sign (+1, or 0: no prior); the series is the DSL's own orientation.
//   checks       in this order, every failure listed, the status is the first (first failure wins):
//                  no_prior       prior sign 0
//                  insufficient   fewer than min_train_days live TRAIN decisions
//                  turnover       tau_k > tau_limit
//                  turnover_cost  tau_k > cost_tau_limit (v4-prior-v2 only)
//                  veto           a defined HAC t of the mean below veto_t
//   redundancy   the survivors in (tier rank, roster index) order -- never a TRAIN statistic -- are
//                admitted unless |pearson rho| over TRAIN decisions where both are live exceeds
//                rho_limit (strict >) against an already admitted candidate; the largest such
//                |rho| (strictly larger wins, so the first admitted on a tie) names it. A pair with
//                fewer than min_common_days common decisions is uncorrelated and noted
//                (low_overlap_with), so is an undefined rho (undefined_rho_with).
//   max_abs_rho  every row: the largest defined |rho| (>= min_common_days common decisions)
//                against any admitted candidate other than itself, strictly larger wins.
//
// Arithmetic: ascending sequential sums. numpy reduces pairwise and through BLAS, so a float
// column ties the fitter's to rounding (1e-12, Ruling P12); every decision, status, failed check,
// order and named candidate is the fitter's exactly (the fixture gtest and the comparator pytest
// pin both). Pure functions; no I/O.

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::admission {

// How the veto's HAC t is computed. Frozen integer values (recorded beside every t).
enum class HacMethod : u8 {
  Unknown = 0,
  // fit_composition_weights.newey_west_t: Bartlett weights 1 - l / (L + 1) at the declared lag L
  // for l = 1..min(L, n - 1), autocovariances of the demeaned series divided by n, no small-sample
  // correction, t = mean / sqrt(LRV / n); undefined for n < 2, a constant series (every value
  // equal to the first) or LRV <= 0. For n > L this is eval::hac::mean_inference(x, BartlettV1, L,
  // false) term for term; for n <= L the fitter keeps the declared L in the weights, which
  // mean_inference's clamped lag would not, so that branch sums the same lag products itself.
  FitterNeweyWestV1 = 1,
};

// "fitter-newey-west-v1"; "" for Unknown.
[[nodiscard]] std::string_view hac_method_name(HacMethod method) noexcept;

// The HAC t of the mean of `x` (finite values) under `method` at `lag`; nullopt when undefined
// (or for HacMethod::Unknown).
[[nodiscard]] std::optional<f64> hac_t(std::span<const f64> x, HacMethod method,
                                       usize lag) noexcept;

// The screens this library implements.
enum class ScreenId : u8 {
  Unknown = 0,
  V4PriorV1 = 1, // v4 pre-registration R3
  V4PriorV2 = 2, // v4.2 pre-registration R3': v4-prior-v1 plus the turnover_cost check
};

// "v4-prior-v1" / "v4-prior-v2"; "" for Unknown.
[[nodiscard]] std::string_view screen_name(ScreenId id) noexcept;
// The id of a screen name; nullopt when unknown.
[[nodiscard]] std::optional<ScreenId> screen_from_name(std::string_view name) noexcept;

// The declared constants of a screen (fit:279, :295, :346).
struct ScreenRules {
  usize min_train_days{250};
  f64 tau_limit{0.70};
  std::optional<f64> cost_tau_limit{}; // v4-prior-v2: 0.08
  f64 veto_t{-2.0};
  usize hac_lag{5};
  HacMethod hac_method{HacMethod::FitterNeweyWestV1};
  f64 rho_limit{0.90};
  usize min_common_days{250};
  f64 sharpe_annualization{252.0};
};

// The rules of `id` (Unknown: v4-prior-v1's).
[[nodiscard]] ScreenRules screen_rules(ScreenId id) noexcept;

// The checks, in their declared order (the order failed_checks lists them).
enum class Check : u8 { NoPrior = 0, Insufficient = 1, Turnover = 2, TurnoverCost = 3, Veto = 4 };
// "no_prior", "insufficient", "turnover", "turnover_cost", "veto".
[[nodiscard]] std::string_view check_name(Check check) noexcept;

enum class Status : u8 {
  Admitted = 0,
  RejectNoPrior = 1,
  RejectInsufficient = 2,
  RejectTurnover = 3,
  RejectTurnoverCost = 4,
  RejectVeto = 5,
  RejectRedundant = 6,
};
// "admitted", "reject_no_prior", ..., "reject_redundant".
[[nodiscard]] std::string_view status_name(Status status) noexcept;

// The inputs of one screen. Spans are borrowed for the call.
struct ScreenInput {
  usize candidates{};
  usize decisions{};
  std::span<const f64> factors;     // decisions x candidates, decision-major (K-P9-4 factor.f64)
  std::span<const f64> taus;        // candidates (K-P9-4 tau.f64)
  std::span<const u8> train_mask;   // decisions, 0 / 1: the decision session lies in TRAIN
  std::span<const usize> tier_rank; // candidates: the declared tier order (lower first)
  std::span<const i32> prior_signs; // candidates: 1, or 0 (no prior)
};

// One candidate's row of the decision table (the fitter's screen_v4 row; candidate references
// are roster indices).
struct ScreenRow {
  i32 s_k{};
  f64 tau{};
  usize train_days{};
  std::optional<f64> train_mean;
  std::optional<f64> train_sharpe;
  std::optional<f64> hac_t;
  std::vector<Check> failed_checks;
  Status status{Status::Admitted};
  std::optional<usize> redundant_with;
  std::optional<f64> redundant_rho;
  std::optional<usize> admission_rank; // 1-based, admitted only
  std::vector<usize> low_overlap_with;
  std::vector<usize> undefined_rho_with;
  std::optional<f64> max_abs_rho;
  std::optional<usize> max_abs_rho_with;
};

// (mean, annualised Sharpe) of live values: mean nullopt for none, Sharpe nullopt below two values
// or at a zero sample SD (fit:1491-1499).
struct MeanSharpe {
  std::optional<f64> mean;
  std::optional<f64> sharpe;
};
[[nodiscard]] MeanSharpe mean_sharpe(std::span<const f64> x, f64 annualization) noexcept;

// Pearson rho of candidates a and b over the decisions where `mask` is set and both are finite,
// and that decision count; rho nullopt below two decisions or at a zero denominator
// (fit:1502-1510). Preconditions (checked by screen_v4): a, b < input.candidates, consistent
// geometry.
struct PairRho {
  std::optional<f64> rho;
  usize days{};
};
[[nodiscard]] PairRho pair_correlation(const ScreenInput &input, usize a, usize b) noexcept;

// The screen. Err(InvalidArgument) on inconsistent geometry, no candidate, a prior sign other than
// 0 or 1 (v4 embeds the prior sign in the DSL; fit:577 refuses -1), a mask byte above 1 or rules
// with an unknown HAC method; Err(OutOfRange) when allocation fails.
[[nodiscard]] core::Result<std::vector<ScreenRow>> screen_v4(const ScreenInput &input,
                                                             const ScreenRules &rules);

// The admitted candidates in admission order (by admission_rank).
[[nodiscard]] std::vector<usize> admitted_order(std::span<const ScreenRow> rows);

} // namespace atx::engine::research::admission
