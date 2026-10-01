#pragma once

// spo-v1 / spo-v2 (platform v7 wave 2, W1; literature-v7 R2.1 + R3.4): the cost-aware
// single-period optimiser around the Garleanu-Pedersen aim, a construction rule of the NAV
// replay reached through strategy_nav_v7 (`--rule spo-v1|spo-v2`). Per rebalance decision d
// and book:
//
//   max_w  a'w - (gamma/2) w'(X F X' + D) w - (1/H) sum_i [s_i |dw_i| + eta_i |dw_i|^{3/2}]
//          - sum_i [b_i max(-w_i, 0) + l_i max(w_i, 0)]
//   s.t.   1'w = 0 (book net, with the names held outside the problem)
//          |beta'w| <= beta_max (book ex-ante beta to the equal-weight member market)
//          sum_i |w_i| <= G (book gross budget, a hard cap on planned gross: G = --spo-gross;
//                            spo-v1 default L = --aim-leverage, spo-v2 default 1.0)
//          |w_i| <= min(w_max, q ADV_i / NAV), |dw_i| <= p ADV_i / NAV, dw = w - w0
//          w_i >= min(w0_i, 0) where the book's locate rule guards the name (no locate)
//
// with (all per session) a_i = IC_book sigma_i z_i / sqrt(h) (Grinold-Kahn; sigma_i =
// sqrt(D_i) the atx-risk-v1 daily specific vol at d, z = the shared desired target over its
// cross-sectional SD; IC_book is the IC over the alpha horizon of h sessions, so the forecast
// over h sessions is IC_book (sigma_i sqrt(h)) z_i and its per-session share is that over h),
// X/F/D = atx-risk-v1 exposures, factor covariance and specific variance forecast at the
// close of d, s_i + eta_i |dw|^{1/2} = the primary S2 law's cost per dollar (half spread +
// commission; 0.6 sigma_i (NAV |dw| / ADV_i)^{1/2}) on the decision liquidity window, b_i /
// l_i = the book's own short / long financing per session (tier fee at d), and H = the cost
// amortization horizon (default 1/theta; a trade's cost is paid once and its alpha accrues
// over the holding period). h is the IC's measurement horizon (--alpha-horizon; spo-v2
// default 21, the horizon of the literature's ICs of .02-.04) and is independent of H, so a
// change of theta does not rescale the alpha; one uniform 1/sqrt(h) serves every sleeve (the
// per-sleeve decay of R2.1 is not modeled). The members of d with a risk row are optimized;
// nonmembers follow
// aim-partial-v5's exit rule and members without a risk forecast keep their weight -- both
// enter the book constraints and the risk as fixed positions.
//
// Solver: accelerated proximal gradient (FISTA, Beck-Teboulle 2009) with adaptive restart
// (O'Donoghue-Candes 2015) in the diagonal metric M = sigma gamma D, sigma >= 1 +
// lambda_max(D^{-1/2} X F X' D^{-1/2}) on the dollar-neutral subspace (power iteration, then
// backtracking on the exact quadratic model). The risk gradient is X (F (X' w)) + D w over the
// sparse exposure rows (market, one industry, styles): never an N x N matrix. The prox of the
// separable costs, financing and boxes jointly with the net, gross and beta constraints is
// exact: with dollar-neutral multiplier nu and gross multiplier mu >= 0, a name's positive part
// depends only on alpha+ = mu + nu and its negative part only on alpha- = mu - nu, so the two
// sides are two independent monotone 1-D roots (Newton with bisection safeguard); the beta
// multiplier is an outer monotone root; the 1-D minimizer of the linear + |.|^{3/2} cost is in
// closed form. Every iterate is feasible. Stops when the prox-gradient residual
// ||w - prox(w - M^{-1} grad f(w))||_inf <= tolerance (a KKT certificate), or at the declared
// iteration count. Warm start: the book's current weights and its previous multipliers.
//
// gamma (when --gamma is not given) is calibrated once, on the first rebalance decision,
// on the cost-free aim (no costs, financing, trade limits or gross cap; net, beta and
// |w_i| <= w_max kept), gamma_vol giving the aim the annualised ex-ante vol --target-vol and
// gamma_bind giving it gross G, each by a bracketed monotone root in ln gamma. spo-v1:
// gamma = max(gamma_vol, gamma_bind); spo-v2: gamma = gamma_vol, and gamma_bind is a
// report-only root solved after the aim (a failure is reported as NaN with a note, never an
// abort). Nothing is fitted on returns.
//
// spo-v1 is the rule as pre-registered for the 2026-09-28 TRAIN cell; spo-v2 changes these
// defaults after its root cause (task-W1-report.md fix-up 2, task-R2-review.md):
//   h = 1 -> 21: v1 read IC_book = .02 as a one-session IC while amortizing costs over H = 20
//     sessions, overstating the alpha that pays for a trade by ~sqrt(20) (Grinold-Kahn: IC and
//     vol at the forecast horizon; the fundamental law puts a one-session IC of .02 over
//     ~1,800 names at an ex-ante IR near 13).
//   max(gamma_vol, gamma_bind) -> gamma_vol: gamma >= gamma_bind puts the cost-free aim at
//     gross <= L, and costs, ADV caps and trade limits pull the live book inside it, so
//     the gross budget never bound (0 of 1,508 v1 solves).
//   G = L -> 1.0: the budget binds by design in spo-v2, so planned gross = G; L = 1.247 was
//     aim-partial's 1 / mean gross of a partially trading aim, and R6' tests mean gross in
//     [.90, 1.05]. The aim-partial-v5 shadow book keeps --aim-leverage.
//   specific ceiling inf -> 1.0 as a tripwire: a daily specific variance above 1 (vol 100% per
//     session) is clamped and counted, and with --specific-ceiling-void (spo-v2 default on)
//     one clamped entry voids the run before any NAV or return file exists. The clamp does not
//     repair the input: a_i ~ sqrt(D_i), so a clamped name still carries an inflated alpha
//     (the TRAIN risk model has 1e0..9e12 on 177-179 names for 20 sessions from 2020-05-12).
// Every other default is unchanged. A plan-level aim-partial-v5 shadow book (full fills, no
// drift) runs beside each book: its a'w, gross, turnover, modeled cost and ex-ante vol are
// recorded per decision, so alpha capture is read apart from cost.
//
// spo-v3 (platform v8 R-6, strategy_spo_v3.hpp) is target tracking toward the aim itself: no
// alpha vector, the engine's solve_tracking instead of FISTA, its own rows (TrackingRow).
// It shares with spo-v1/v2 the risk store, the fixed positions, the per-name market terms,
// the plan fields and the shadow book. With spo-v1/v2 nothing of it runs.

#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "strategy_cost_v2.hpp"
#include "strategy_nav_replay.hpp"
#include "strategy_target_replay.hpp"

namespace atx::impl::strategy::spo {

inline constexpr atx::f64 unset = std::numeric_limits<atx::f64>::quiet_NaN();
inline constexpr atx::f64 unbounded = std::numeric_limits<atx::f64>::infinity();

// ---- declared parameters (CLI flags; defaults from literature-v7 R2.1, never from returns) -
enum class GammaRule : atx::u8 {
  VolAndBind, // spo-v1: max(gamma_vol, gamma_bind)
  Vol,        // spo-v2: gamma_vol (the gross budget is a hard cap that binds)
};
struct SpoParams {
  atx::f64 gamma{unset};           // --gamma (NaN: calibrated, see above)
  atx::f64 ic_book{0.02};          // --ic-book
  atx::f64 w_max{0.01};            // --w-max
  atx::f64 adv_cap_q{0.05};        // --adv-cap-q (holding cap q ADV / NAV)
  atx::f64 adv_trade_p{0.01};      // --adv-trade-p (trade cap p ADV / NAV)
  atx::usize max_iterations{500};  // --spo-iters
  atx::f64 tolerance{1e-8};        // --spo-tol (prox-gradient residual, weight units)
  atx::f64 target_vol{0.05};       // --target-vol (annualised, sqrt(252))
  atx::f64 horizon{unset};         // --spo-horizon H (NaN: 1 / --trade-fraction)
  atx::f64 beta_max{0.02};         // |beta'w| bound (R2.1)
  bool all_books{true};            // --spo-books all|primary (primary: S1 and S2 only)
  atx::f64 alpha_horizon{1.0};     // --alpha-horizon h in [1, 1e4]: a_i = IC sigma z / sqrt(h)
  atx::f64 specific_ceiling{unbounded}; // --specific-ceiling: larger daily D_i clamped
  bool void_on_capped{false};      // --specific-ceiling-void on|off: a clamp voids the run
  atx::f64 gross_budget{unset};    // --spo-gross G (NaN: --aim-leverage)
  GammaRule gamma_rule{GammaRule::VolAndBind};
  // 1: spo-v1 as pre-registered; 2: spo-v2 (v2_params); 3: spo-v3 (v3_params,
  // strategy_spo_v3.hpp)
  atx::u32 version{1};
  atx::f64 sharpe_prior{unset}; // spo-v3 only: S_prior, gamma = S_prior / sigma_aim
};
// spo-v2's defaults: SpoParams{} with h = 21, G = 1.0, specific ceiling 1.0 (daily vol 100%)
// as a tripwire (void on) and gamma = gamma_vol.
[[nodiscard]] SpoParams v2_params();
[[nodiscard]] const char* rule_name(const SpoParams& p); // "spo-v1" / "spo-v2" / "spo-v3"
// The key of the rule's recipe / summary / extras blocks: "spo_v1" / "spo_v2" / "spo_v3".
[[nodiscard]] const char* json_key(const SpoParams& p);
// Ranges of every parameter; G (when given) finite > 0; spo-v3's S_prior finite in (0, 1e3].
// The bound against --aim-leverage is validate_gross_budget's (the leverage is a replay flag).
[[nodiscard]] atx::core::Status validate_params(const SpoParams& p);
// G in effect: --spo-gross, else aim_leverage (spo-v1 as pre-registered, bit for bit).
[[nodiscard]] atx::f64 gross_budget_of(const SpoParams& p, atx::f64 aim_leverage) noexcept;
// A sanity bound, not a model parameter: G in (0, 1.5 x --aim-leverage].
inline constexpr atx::f64 gross_budget_sanity = 1.5;
[[nodiscard]] atx::core::Status validate_gross_budget(atx::f64 budget, atx::f64 aim_leverage);

// ---- the alpha (Grinold-Kahn) ------------------------------------------------------------
// Sample SD of desired over the members (0 with fewer than two members or no dispersion).
[[nodiscard]] atx::f64 member_sd(std::span<const atx::f64> desired,
                                 std::span<const atx::u8> member);
// Per-session alpha of a Grinold-Kahn forecast over h sessions: IC (sigma sqrt(h)) z / h with
// sigma = sqrt(specific) the daily specific vol.
[[nodiscard]] inline atx::f64 gk_alpha(atx::f64 ic, atx::f64 specific, atx::f64 z,
                                       atx::f64 horizon) {
  return ic * std::sqrt(specific) * z / std::sqrt(horizon);
}

// ---- the convex problem (generic; the replay fills it from atx-risk-v1) ----------------
// Factor columns: 0 = market (exposure 1 for every problem name), 1..industries = industry
// dummies (a name loads 1 on at most one), then `styles` dense style columns.
struct FactorLayout {
  atx::usize industries{}, styles{};
  [[nodiscard]] atx::usize factors() const noexcept { return 1 + industries + styles; }
};
struct Problem {
  FactorLayout layout;
  atx::usize n{};
  std::vector<atx::u32> industry;   // per name: factor column in [1, industries], 0 = none
  std::vector<atx::f64> styles;     // n x styles, row-major
  std::vector<atx::f64> specific;   // D_i, finite > 0
  std::vector<atx::f64> covariance; // factors x factors, row-major, symmetric PSD
  std::vector<atx::f64> fixed_exposure; // factors: X'w of the positions held outside
  std::vector<atx::f64> alpha, beta;    // a_i, beta_i
  std::vector<atx::f64> w0;             // current weights (costs are on w - w0)
  std::vector<atx::f64> lower, upper;   // lower <= upper (may be +-inf)
  std::vector<atx::f64> linear_cost, impact_cost; // s_i, eta_i >= 0 (already / H)
  std::vector<atx::f64> long_rate, short_rate;    // l_i, b_i >= 0 per period
  atx::f64 gamma{1.0};
  atx::f64 net{};                  // 1'w == net
  atx::f64 gross{unbounded};       // sum |w| <= gross (inf: no budget)
  atx::f64 beta_lo{-unbounded}, beta_hi{unbounded}; // beta'w in [lo, hi]
};
// Sizes, finiteness and signs (InvalidArgument otherwise).
[[nodiscard]] atx::core::Status validate_problem(const Problem& p);

// Warm multipliers of the coupled prox (alpha+ = mu + nu, alpha- = mu - nu, beta rho).
struct Multipliers {
  atx::f64 pos{}, neg{}, rho{};
  bool gross_binding{};
};
struct SolverOptions {
  atx::usize max_iterations{500};
  atx::f64 tolerance{1e-8};
  atx::f64 metric_scale{unset}; // sigma; NaN: estimate_metric_scale(problem)
};
struct Solution {
  std::vector<atx::f64> w;
  atx::usize iterations{}, restarts{}, backtracks{}, prox_passes{};
  bool converged{};    // residual <= tolerance
  bool coupling_met{}; // net, gross and beta met to 1e-10 at the returned point
  atx::f64 residual{}; // ||w - prox(w - M^{-1} grad f(w))||_inf
  atx::f64 metric_scale{};
  Multipliers multipliers;
};
// sigma = 1 + 1.1 lambda, lambda the power-iteration (40 steps, deterministic start)
// estimate of max d'X F X'd / d'D d over 1'd = 0. Backtracking in solve() covers an
// underestimate.
[[nodiscard]] atx::f64 estimate_metric_scale(const Problem& p);
// Minimizes the negated objective from `start` (any point; projected by the first prox).
// Deterministic: same inputs, same bits.
[[nodiscard]] atx::core::Result<Solution> solve(const Problem& p, std::span<const atx::f64> start,
                                               const SolverOptions& options,
                                               const Multipliers& warm = {});
// The prox-gradient residual at w in the metric sigma gamma D (0 iff w is optimal).
[[nodiscard]] atx::core::Result<atx::f64> kkt_residual(const Problem& p,
                                                       std::span<const atx::f64> w,
                                                       atx::f64 metric_scale);
struct Terms {
  atx::f64 alpha{}, variance{}, risk{}, cost{}, financing{}, objective{};
};
// a'w, (X'w + fixed)'F(X'w + fixed) + sum D w^2, gamma/2 variance, the (amortized) trade
// cost, financing and alpha - risk - cost - financing.
[[nodiscard]] Terms objective_terms(const Problem& p, std::span<const atx::f64> w);

// ---- atx-risk-v1 inputs (the `risk` verb run with --emit-exposures all) ---------------
inline constexpr atx::usize risk_industry_slots = 50; // FF49 slots 0..48, residual 49
inline constexpr atx::usize risk_styles = 11;
inline constexpr atx::usize risk_factors = 1 + risk_industry_slots + risk_styles; // 62
inline constexpr atx::u8 risk_no_row = 255;
struct RiskSlice {
  atx::usize date{};
  atx::i64 session{};
  std::vector<atx::u8> slot;        // per instrument: 0..49, risk_no_row = no exposure row
  std::vector<atx::f64> styles;     // instruments x 11
  std::vector<atx::f64> specific;   // per instrument daily variance, NaN none
  std::vector<atx::f64> covariance; // 62 x 62 daily; NaN entries set to 0 (counted)
  atx::usize nan_covariance_entries{};
  atx::usize capped_specific{}; // entries clamped by cap_specific
};
// Clamps every finite specific variance above `ceiling` to it (a plausibility bound on the
// risk model, not a model parameter); returns and records the count. inf: no change.
atx::usize cap_specific(RiskSlice& r, atx::f64 ceiling);
class RiskStore {
public:
  // Opens an atx-risk-v1 output directory: manifest.json whose SHA-256 is manifest_sha256,
  // schema atx.risk-model/v1, status complete, role.manifest_sha256 == role_sha256,
  // geometry 62 factors / 11 styles, the per-date files of --emit-exposures all
  // (factor_covariance.f64, specific_variance.f64, style_exposures.f32, industry_slot.u8)
  // and diagnostics.csv, each with the manifest's SHA-256 and byte size. Refuses otherwise.
  [[nodiscard]] static atx::core::Result<RiskStore> open(const std::string& directory,
                                                         const std::string& manifest_sha256,
                                                         const std::string& role_sha256);
  [[nodiscard]] atx::usize dates() const noexcept { return dates_; }
  [[nodiscard]] atx::usize instruments() const noexcept { return instruments_; }
  [[nodiscard]] std::span<const atx::i64> sessions() const noexcept { return sessions_; }
  [[nodiscard]] std::span<const atx::u8> forecast() const noexcept { return forecast_; }
  [[nodiscard]] const std::string& directory() const noexcept { return directory_; }
  [[nodiscard]] const std::string& manifest_sha256() const noexcept { return manifest_sha256_; }
  // The replay's axes must be the model's (same role): dates, instruments, session keys.
  [[nodiscard]] atx::core::Status check_axes(std::span<const atx::i64> sessions,
                                             atx::usize instruments) const;
  // Row d. Unavailable when d is outside the model or the model has no forecast at d.
  [[nodiscard]] atx::core::Status read(atx::usize d, RiskSlice& out) const;

private:
  std::string directory_, manifest_sha256_;
  atx::usize dates_{}, instruments_{};
  std::vector<atx::i64> sessions_;
  std::vector<atx::u8> forecast_;
};

// ---- one book's rebalance decision in the replay -----------------------------------------
struct BookDecision {
  const TargetReplayInput& x;
  const NavReplayConfig& cfg;      // the book's config (its scenario's financing and locate)
  const NavScenario& s2;           // the primary S2 law (the cost model of every book)
  atx::usize d{};
  atx::f64 nav_post{};
  std::span<const atx::f64> desired; // the shared desired target of d
  std::span<const atx::u8> tier;      // borrow tier per name at d (empty: no tiers)
  std::span<const atx::u8> no_locate; // decide --locates only (empty in the replay)
  const cost_v2::DecisionLiquidity& liquidity; // decision window [d - w, d)
  std::string_view book;
};
// One diagnostics row per (rebalance decision, book).
struct DiagnosticRow {
  atx::i64 session{};
  std::string book;
  atx::usize members{}, optimized{}, unpriced_members{}, fixed_nonmembers{};
  atx::f64 gamma{};
  atx::usize iterations{}, restarts{}, backtracks{}, prox_passes{};
  bool converged{}, coupling_met{};
  atx::f64 residual{};
  // Objective terms of the optimized names (per session, NAV fractions): alpha a'w, risk
  // (gamma/2) w'Sigma w of the whole book, modeled trade cost (unamortized) and its
  // amortized charge, financing, and the objective (alpha - risk - cost/H - financing).
  atx::f64 alpha{}, risk{}, trade_cost{}, amortized_cost{}, financing{}, objective{};
  // ANNUALISED (sqrt(252 x daily variance)), whole book (priced names); not per session.
  atx::f64 exante_vol{}, exante_vol_current{};
  atx::f64 transfer_coefficient{}; // corr(a_i / sigma_i^2, w_i) over the optimized names
  atx::f64 gross{}, net{}, long_weight{}, short_weight{}, abs_beta{}, turnover{};
  atx::usize no_trade{}, at_cap{}, at_trade_limit{}, at_locate_floor{};
  bool gross_binding{}, beta_binding{};
  atx::f64 mu{}, nu{}, rho{};
  // Risk-model entries (every instrument of the slice) clamped at d (--specific-ceiling).
  atx::usize capped_specific{};
  // The plan-level aim-partial-v5 shadow book at d (same desired target, same alpha a and S2
  // law; full fills, no drift): a'w over the optimized names, gross, turnover, modeled
  // (unamortized) trade cost over the optimized names, ex-ante vol (annualised).
  atx::f64 alpha_shadow{unset}, gross_shadow{unset}, turnover_shadow{unset};
  atx::f64 trade_cost_shadow{unset}, exante_vol_shadow{unset};
};
// spo-v3's diagnostics row per (rebalance decision, book) (strategy_spo_v3.hpp).
struct TrackingRow {
  atx::i64 session{};
  std::string book;
  atx::usize members{}, optimized{}, unpriced_members{}, fixed_nonmembers{};
  atx::f64 gamma{};
  atx::usize iterations{};
  bool converged{}, limits_met{};
  atx::f64 primal_residual{}, dual_residual{}, limit_violation{}; // weight units
  atx::usize clipped_eigenvalues{};
  // ANNUALISED (sqrt(252 x daily variance)), whole book (priced names): the plan's and the
  // current book's distance to the aim w_aim.
  atx::f64 tracking_error{}, tracking_error_current{};
  // Pearson correlation of the planned and the aim weights over the optimized names.
  atx::f64 aim_correlation{};
  // Review A-4, the Ruling E-14 criterion's input: Pearson correlation of the traded book (the
  // holdings DECIDE read at d, after the fills, caps, blocks and drift of earlier decisions;
  // nonmember exits and unpriced members included) and the aim, over every name either
  // holds (NaN from a flat book).
  atx::f64 aim_correlation_traded{};
  // The solver's terms (per session, NAV fractions): objective = (gamma/2) tracking variance
  // over the problem + amortized_cost + borrow; trade_cost = amortized_cost x H (unamortized).
  atx::f64 objective{}, trade_cost{}, amortized_cost{}, borrow{};
  atx::f64 gross{}, aim_gross{}, net{}, long_weight{}, short_weight{}, abs_beta{}, turnover{};
  atx::usize no_trade{}, at_trade_limit{};
  atx::f64 trade_limit_share{}; // at_trade_limit / optimized
  atx::usize at_locate_floor{};
  bool gross_bound_breached{}; // planned gross above the sanity bound 2 x --aim-leverage
  atx::f64 nu{}, rho{};        // the net and beta multipliers of the solver's x-update
  // Risk-model entries (every instrument of the slice) clamped at d (--specific-ceiling).
  atx::usize capped_specific{};
  // The plan-level aim-partial-v5 shadow book at d (same aim, S2 law and risk model; full
  // fills, no drift): gross, turnover, modeled (unamortized) trade cost over the optimized
  // names, its tracking error (annualised) and aim correlation.
  atx::f64 gross_shadow{unset}, turnover_shadow{unset}, trade_cost_shadow{unset};
  atx::f64 tracking_error_shadow{unset}, aim_correlation_shadow{unset};
};
// The calibration blocks' "warm_up" entry (Calibration::warm_up, review A-2).
inline constexpr const char* warm_up_calibration_text =
    "v8 D-0 warm start: the unscored warm-up decisions (before the role's decision_begin) move "
    "the book as aim-partial-v5 toward the same desired target and read no risk row; gamma, "
    "session and the aim figures are those of the first scored decision";
struct Calibration {
  bool done{}, from_flag{};
  // v8 D-0 warm start (review A-2): true once a warm-up decision (d before the role's
  // decision_begin) was planned. Such a decision moves the book as aim-partial-v5 and reads no
  // risk row, so gamma (and `session`) belong to the first scored decision. The calibration
  // blocks carry a "warm_up" key only when true (without a warm start: byte-identical).
  bool warm_up{};
  GammaRule rule{GammaRule::VolAndBind};
  atx::i64 session{};
  atx::f64 gamma{}, gamma_vol{}, gamma_bind{};
  bool vol_reached{}, bind_reached{};
  atx::f64 aim_vol{}, aim_gross{};
  atx::usize evaluations{}, names{};
  // gamma_rule Vol: why gamma_bind is NaN (its report-only root failed or left the bracket);
  // empty otherwise.
  std::string bind_note;
};
// gamma of the rule on the cost-free aim of `base` (its alpha, risk and beta; costs,
// financing, trade limits and the gross cap dropped; |w_i| <= w_max, net 0 and |beta'w| <=
// beta_max kept) for the gross budget G = budget. Unavailable when the aim has no alpha, or
// (gamma_rule Vol) when the vol target is out of reach of the aim. VolAndBind (spo-v1): the
// vol root, the bind root, then the aim at max(gamma_vol, gamma_bind); a solver failure in
// either root aborts. Vol (spo-v2): the vol root, the aim at gamma_vol, then the bind root,
// report only: its failure or a bracket end sets gamma_bind NaN and bind_note, never aborts.
[[nodiscard]] atx::core::Status calibrate_gamma(const Problem& base, const SpoParams& params,
                                                atx::f64 budget, atx::f64 metric_scale,
                                                Calibration& out);
struct Timing {
  atx::usize solves{};
  atx::f64 seconds{}, max_seconds{};
  // spo-v3: the solves that stopped at the iteration cap (not converged) and their seconds.
  atx::usize unconverged{};
  atx::f64 unconverged_seconds{};
};

// The per-run state of the rule: declared parameters, the risk model, gamma, the shared
// per-decision data (risk slice, betas, alpha, metric) and each book's warm multipliers.
class Engine {
public:
  Engine(const SpoParams& params, std::shared_ptr<const RiskStore> risk);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) noexcept;
  Engine& operator=(Engine&&) noexcept;
  // Plans the book's rebalance decision: `planned` holds the current weights on entry and the
  // plan on return; `out` accumulates the plan fields exactly as aim-partial-v5's move. A
  // warm-up decision (d < x.decision_begin, v8 D-0) is aim-partial-v5's move itself (review
  // A-2): no risk row, no solve, no calibration and no diagnostics row.
  [[nodiscard]] atx::core::Status plan(const BookDecision& in, std::vector<atx::f64>& planned,
                                       TargetReplayDay& out);
  // A non-rebalance decision of the book: the shadow book's aim-partial-v5 move (exits only).
  [[nodiscard]] atx::core::Status hold(const TargetReplayInput& x, const NavReplayConfig& cfg,
                                       atx::usize d, std::span<const atx::f64> desired,
                                       std::string_view book);
  // A new replay pass: clears the per-book warm state (gamma and the rows are kept).
  void begin_run();
  [[nodiscard]] std::span<const DiagnosticRow> rows() const noexcept; // spo-v1/v2
  [[nodiscard]] std::span<const TrackingRow> tracking_rows() const noexcept; // spo-v3
  // spo-v3: the aim L x desired the tracker received at the latest rebalance decision (every
  // instrument, 0 off the members; empty before the first). An observation in memory only,
  // never published (v8 E-26 test seam).
  [[nodiscard]] std::span<const atx::f64> last_aim() const noexcept;
  // spo-v3: session, gamma, aim_vol = sigma_aim, aim_gross and names of its calibration.
  [[nodiscard]] const Calibration& calibration() const noexcept;
  [[nodiscard]] const SpoParams& params() const noexcept;
  [[nodiscard]] Timing timing() const noexcept;
  [[nodiscard]] atx::f64 horizon() const noexcept; // H in effect (NaN before the first plan)
  // G in effect, spo-v3's gross sanity bound 2 x L (NaN before the first plan).
  [[nodiscard]] atx::f64 gross_budget() const noexcept;
  // spo-v3, Ruling E-31a: the label ("<trading id>+<financing id>") of the run's primary book,
  // whose limits_unmet on a scored decision voids the run (tracking_tripwire). Default: the
  // untiered primary (default_primary_book(), strategy_spo_v3.hpp); the v7 hook sets the run's
  // own from its scenario matrix.
  void set_primary_book(std::string book);
  [[nodiscard]] const std::string& primary_book() const noexcept;
  // The rule's published blocks over its scored rows (strategy_spo_v3.cpp). spo-v1/v2
  // delegate unchanged to declaration(params()), parameters_json(params(), horizon(),
  // gross_budget()), calibration_json(calibration()) and diagnostics_csv,
  // diagnostics_units_json, summary_json, tripwire_json, ceiling_tripwire on rows(); spo-v3
  // to the tracking_* functions of strategy_spo_v3.hpp on tracking_rows().
  [[nodiscard]] std::string rule_declaration() const;
  [[nodiscard]] nlohmann::json rule_parameters_json() const;
  [[nodiscard]] nlohmann::json rule_calibration_json() const;
  [[nodiscard]] std::string rows_csv() const;
  [[nodiscard]] nlohmann::json rows_units_json() const;
  [[nodiscard]] nlohmann::json rows_summary_json() const;
  [[nodiscard]] nlohmann::json rows_tripwire_json() const;
  [[nodiscard]] atx::core::Status rows_tripwire() const;
  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

// The specific-ceiling tripwire, read after the replay and before anything is published:
// with void_on_capped, Unavailable when any decision clamped an entry (the run is void); Ok
// otherwise (void off, or no clamp).
[[nodiscard]] atx::core::Status ceiling_tripwire(const SpoParams& p,
                                                 std::span<const DiagnosticRow> rows);
// Its record: ceiling, void flag, capped_specific_decisions (distinct decisions with a clamp),
// capped_specific_names_max (most entries clamped at one decision) and the status.
[[nodiscard]] nlohmann::json tripwire_json(const SpoParams& p,
                                           std::span<const DiagnosticRow> rows);

// The rule's declaration text (recipe/summary), its CSV and its JSON blocks.
[[nodiscard]] std::string declaration(const SpoParams& p);
// spo_diagnostics.csv, one row per (rebalance decision, book). Units: exante_vol,
// exante_vol_current and exante_vol_shadow are ANNUALISED (sqrt(252 x daily variance));
// alpha, risk, trade_cost, amortized_cost, financing, objective and the shadow's alpha and
// cost are per session in NAV fractions (diagnostics_units_json).
[[nodiscard]] std::string diagnostics_csv(std::span<const DiagnosticRow> rows);
[[nodiscard]] nlohmann::json diagnostics_units_json();
// horizon / gross_budget: H and G in effect (Engine::horizon / Engine::gross_budget).
[[nodiscard]] nlohmann::json parameters_json(const SpoParams& p, atx::f64 horizon,
                                             atx::f64 gross_budget);
[[nodiscard]] nlohmann::json calibration_json(const Calibration& c);
// Per book: decisions, convergence, binding counts, means, capped_specific_decisions and
// capped_specific_names_max, the shadow and alpha capture.
[[nodiscard]] nlohmann::json summary_json(std::span<const DiagnosticRow> rows);
} // namespace atx::impl::strategy::spo
