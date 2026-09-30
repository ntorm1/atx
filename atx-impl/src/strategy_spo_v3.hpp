#pragma once

// spo-v3 (platform v8 R-6, review S-8, research recipe 1A; spo trial 3): target tracking, a
// construction rule of the NAV replay reached through strategy_nav_v7
// (`--rule spo-v3 --spo-alpha implied-aim`). Per rebalance decision d and book:
//
//   min_w  (gamma/2) (w - w_aim)' Sigma (w - w_aim)
//          + (1/H) sum_i [s_i |w_i - w0_i| + eta_i |w_i - w0_i|^{3/2}]
//          + sum_i b_i max(-w_i, 0)
//   s.t.   1'w = 0                        book net, with the names held outside the problem
//          |beta'w| <= beta_max           book ex-ante beta (spo-v1's beta)
//          |w_i - w0_i| <= p ADV_i / NAV  the trade limit (0 without ADV)
//          w_i >= min(w0_i, 0)            where the book's locate rule guards the name
//
// w_aim = L x desired on the members (L = --aim-leverage): the accepted rule's aim, desired
// being the NAV replay's shared desired target (v8 E-26: with --hold-band B / --adv-hold-q Q
// the hold band on the ranks and the ADV cap shape it exactly as they shape aim-partial-v5's,
// detail::form_desired and its state; the tracker's own limits are unchanged, and gamma is
// calibrated on the shaped aim). No alpha vector is fitted (--spo-alpha implied-aim): the tracker trades for the aim's implied alpha
// gamma Sigma w_aim. Sigma = X F X' + D is atx-risk-v1 at the close of d (spo-v1's pinned
// store; a daily specific variance above the ceiling is clamped and counted). gamma =
// S_prior / sigma_aim with sigma_aim = sqrt(252 w_aim' Sigma w_aim) of the whole aim at the
// first rebalance decision: Sigma is daily, so the aim's implied annual Sharpe is S_prior.
// s_i = half spread + commission and eta_i = impact_y sigma_i sqrt(NAV / ADV_i) of the primary
// S2 law on the decision liquidity window, amortized over H sessions; b_i = the book's short
// financing per session (no long financing term). No holding cap and no gross constraint:
// a planned gross above 2 x L breaches a sanity bound, and with --specific-ceiling-void on
// (the default) a breach or a clamped specific variance at any scored decision voids the run.
//
// Nonmembers follow aim-partial-v5's exit rule and members without a risk row keep their
// weight (spo-v1's fixed positions): they enter the limits, and their factor exposure enters
// the tracking term as the external gap (their aim is 0; unpriced members have no risk row).
// Solver: the engine's solve_tracking (atx/engine/book/target_tracking.hpp), warm-started per
// book from its last dual. Non-rebalance decisions are aim-partial-v5's (exits only). A
// plan-level aim-partial-v5 shadow book (full fills, no drift) is scored beside each book
// against the same aim, risk model and S2 law.
//
// The per-decision assembly is Engine::Impl::plan_tracking (strategy_spo.cpp); this header
// holds the registered constants and the rule's published blocks (strategy_spo_v3.cpp).

#include <span>
#include <string>
#include <nlohmann/json_fwd.hpp>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "strategy_spo.hpp"

namespace atx::impl::strategy::spo {

// ---- registered constants (the parser refuses every spo flag that would move them) ----------
inline constexpr atx::f64 v3_horizon = 20.0; // H, sessions: fixed, not 1 / theta
// S_prior, the aim's implied annual Sharpe (gamma = S_prior / sigma_aim). Ruling E-14 (PM
// session 2, 2026-09-30; a pre-read amendment of the registration): 20, not the 1.0 first
// declared. On the lane's synthetic prototype S_prior 1 traded factor loadings only
// (corr(w, w_aim) .35, gross .37 L) while 20 tracked at .96; no TRAIN statistic informed it.
// The cell's mechanical criterion (mean correlation of the traded book with the aim over
// scored decisions >= v3_aim_correlation_min; review A-4: aim_correlation_traded, not the
// plan's aim_correlation) is read from the tripwire record's report (tracking_tripwire_json,
// aim_correlation_criterion), never enforced here.
inline constexpr atx::f64 v3_sharpe_prior = 20.0;
inline constexpr atx::f64 v3_aim_correlation_min = 0.9; // Ruling E-14's criterion threshold
inline constexpr atx::f64 v3_adv_trade_p = 0.01;         // p of the trade limit p ADV / NAV
inline constexpr atx::f64 v3_beta_max = 0.02;            // |book beta| bound
inline constexpr atx::f64 v3_specific_ceiling = 1.0;     // daily; a clamp is a tripwire
inline constexpr atx::f64 v3_gross_bound_multiple = 2.0; // breach: planned gross > 2 x L

// spo-v3's parameters: SpoParams{} with version 3, H = v3_horizon, S_prior = v3_sharpe_prior,
// p and beta_max as above, the specific ceiling 1 with the void on, and the engine's
// iteration cap and tolerance (tracking_max_iterations, tracking_tolerance). --spo-iters,
// --spo-tol, --spo-books and --specific-ceiling(-void) may override them; every other field
// keeps spo-v1's default and is unused.
[[nodiscard]] SpoParams v3_params();

// ---- published blocks (Engine's rule_* / rows_* members dispatch here under spo-v3) ----------
[[nodiscard]] std::string tracking_declaration(const SpoParams& p);
// horizon: H in effect; gross_bound: 2 x L in effect (NaN before the first plan).
[[nodiscard]] nlohmann::json tracking_parameters_json(const SpoParams& p, atx::f64 horizon,
                                                      atx::f64 gross_bound);
// gamma's calibration: session, S_prior, sigma_aim (c.aim_vol), the aim's gross, gamma, names.
[[nodiscard]] nlohmann::json tracking_calibration_json(const SpoParams& p, const Calibration& c);
// spo_diagnostics.csv under spo-v3: one row per (scored rebalance decision, book). Units:
// tracking_error* ANNUALISED (sqrt(252 x daily variance)); objective, amortized_cost and
// borrow per session; trade_cost* per decision (tracking_units_json).
[[nodiscard]] std::string tracking_csv(std::span<const TrackingRow> rows);
[[nodiscard]] nlohmann::json tracking_units_json();
// Per book: decisions, convergence, tracking error mean / max, share at the trade limit
// mean / max, aim correlation mean / min of the plan and of the traded book, the E-14
// criterion on the traded one, cost, gross, turnover, holding period, gross-bound breaches,
// clamps, and the shadow book with the cost ratio.
[[nodiscard]] nlohmann::json tracking_summary_json(std::span<const TrackingRow> rows);
// The tripwire, read after the replay and before anything is published: with the void on,
// Unavailable when a scored decision clamped a specific variance or a book planned a gross
// above 2 x L (the run is void); Ok otherwise (void off, or neither).
[[nodiscard]] atx::core::Status tracking_tripwire(const SpoParams& p,
                                                  std::span<const TrackingRow> rows);
// Its record: ceiling, void flag, clamp counts, gross-bound breaches, the largest planned
// gross, the status and, report only, per book: tracking error mean / max, share at the trade
// limit mean / max, aim correlation mean / min (planned and traded), the E-14 criterion
// (aim_correlation_criterion: the traded book's mean against .9), unconverged and limits
// unmet solves.
[[nodiscard]] nlohmann::json tracking_tripwire_json(const SpoParams& p,
                                                    std::span<const TrackingRow> rows);
} // namespace atx::impl::strategy::spo
