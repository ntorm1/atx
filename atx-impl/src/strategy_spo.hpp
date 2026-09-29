#pragma once

// spo-v1 (platform v7 wave 2, W1; literature-v7 R2.1 + R3.4): the cost-aware single-period
// optimiser around the Garleanu-Pedersen aim, a construction rule of the NAV replay reached
// through strategy_nav_v7 (`--rule spo-v1`). Per rebalance decision d and book:
//
//   max_w  a'w - (gamma/2) w'(X F X' + D) w - (1/H) sum_i [s_i |dw_i| + eta_i |dw_i|^{3/2}]
//          - sum_i [b_i max(-w_i, 0) + l_i max(w_i, 0)]
//   s.t.   1'w = 0 (book net, with the names held outside the problem)
//          |beta'w| <= beta_max (book ex-ante beta to the equal-weight member market)
//          sum_i |w_i| <= L (book gross; L = --aim-leverage)
//          |w_i| <= min(w_max, q ADV_i / NAV), |dw_i| <= p ADV_i / NAV, dw = w - w0
//          w_i >= min(w0_i, 0) where the book's locate rule guards the name (no locate)
//
// with (all per session) a_i = IC_book sigma_i z_i (Grinold-Kahn; sigma_i = sqrt(D_i) the
// atx-risk-v1 specific vol at d, z = the shared desired target over its cross-sectional SD),
// X/F/D = atx-risk-v1 exposures, factor covariance and specific variance forecast at the
// close of d, s_i + eta_i |dw|^{1/2} = the primary S2 law's cost per dollar (half spread +
// commission; 0.6 sigma_i (NAV |dw| / ADV_i)^{1/2}) on the decision liquidity window, b_i /
// l_i = the book's own short / long financing per session (tier fee at d), and H = the cost
// amortization horizon (default 1/theta: under Garleanu-Pedersen quadratic costs a
// single-period problem with H = 1/theta trades at the declared GP rate theta). The members
// of d with a risk row are optimized; nonmembers follow aim-partial-v5's exit rule and
// members without a risk forecast keep their weight -- both enter the book constraints and
// the risk as fixed positions.
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
// |w_i| <= w_max kept): gamma = max(gamma_vol, gamma_bind), gamma_vol giving the aim the
// annualised ex-ante vol --target-vol and gamma_bind giving it gross L (so the aim meets both
// the vol target and the gross budget), each by a bracketed monotone root in ln gamma.
// Nothing is fitted on returns.

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
};
[[nodiscard]] atx::core::Status validate_params(const SpoParams& p);

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
};
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
  atx::f64 exante_vol{}, exante_vol_current{}; // annualised, whole book (priced names)
  atx::f64 transfer_coefficient{}; // corr(a_i / sigma_i^2, w_i) over the optimized names
  atx::f64 gross{}, net{}, long_weight{}, short_weight{}, abs_beta{}, turnover{};
  atx::usize no_trade{}, at_cap{}, at_trade_limit{}, at_locate_floor{};
  bool gross_binding{}, beta_binding{};
  atx::f64 mu{}, nu{}, rho{};
};
struct Calibration {
  bool done{}, from_flag{};
  atx::i64 session{};
  atx::f64 gamma{}, gamma_vol{}, gamma_bind{};
  bool vol_reached{}, bind_reached{};
  atx::f64 aim_vol{}, aim_gross{};
  atx::usize evaluations{}, names{};
};
struct Timing {
  atx::usize solves{};
  atx::f64 seconds{}, max_seconds{};
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
  // plan on return; `out` accumulates the plan fields exactly as aim-partial-v5's move.
  [[nodiscard]] atx::core::Status plan(const BookDecision& in, std::vector<atx::f64>& planned,
                                       TargetReplayDay& out);
  // A new replay pass: clears the per-book warm state (gamma and the rows are kept).
  void begin_run();
  [[nodiscard]] std::span<const DiagnosticRow> rows() const noexcept;
  [[nodiscard]] const Calibration& calibration() const noexcept;
  [[nodiscard]] const SpoParams& params() const noexcept;
  [[nodiscard]] Timing timing() const noexcept;
  [[nodiscard]] atx::f64 horizon() const noexcept; // H in effect (NaN before the first plan)
  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

// The rule's declaration text (recipe/summary), its CSV and its JSON blocks.
[[nodiscard]] std::string declaration();
[[nodiscard]] std::string diagnostics_csv(std::span<const DiagnosticRow> rows);
[[nodiscard]] nlohmann::json parameters_json(const SpoParams& p, atx::f64 horizon);
[[nodiscard]] nlohmann::json calibration_json(const Calibration& c);
[[nodiscard]] nlohmann::json summary_json(std::span<const DiagnosticRow> rows);
} // namespace atx::impl::strategy::spo
