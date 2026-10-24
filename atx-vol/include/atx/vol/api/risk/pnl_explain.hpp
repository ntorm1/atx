#pragma once

// pnl_explain — Greek-level P&L explain between two RiskFrames of one book, with
// the vol move split into level / skew / curvature, share legs included, and the
// streaming driver (RiskStream) that computes each state's frame exactly once.
//
// FROZEN API CONTRACT (sprint 2026-10-24; see risk_types.hpp banner).
//
// ## The decomposition (per unique option; base frame b, shifted frame t)
//
//   dS  = S_t - S_b                         (per uid)
//   dr  = r_t - r_b                         (the contract's rate column)
//   dt  = (t.ts - b.ts) in years            (the base state's TimeSpec)
//   dsig = sigma_t - sigma_b                (the SAME contract's vol; see VolMoveConvention)
//
//   pnl_delta = delta_b dS          pnl_gamma = 1/2 gamma_b dS^2
//   pnl_vega  = vega_b  dsig        pnl_volga = 1/2 volga_b dsig^2
//   pnl_vanna = vanna_b dS dsig     pnl_theta = theta_b dt
//   (with ExplainOptions::trapezoid_second_order: gamma_b, volga_b, vanna_b are
//    replaced by the base/shifted averages; the three other base Greeks are kept)
//   pnl_rho   = rho_b dr            pnl_charm = charm_b dS dt
//   pnl_carry = delta_b S_b J       (the carry axis, below; 0 when the carry did not move)
//   explained = sum of the eight (+ pnl_carry)       pnl_total = P_t - P_b
//   pnl_unexplained = pnl_total - explained          (exact remainder)
//
// `dt` is per expiry when either state carries a finite ExpiryCarry::T_override for the
// contract's (uid, expiry) (design D22): the contract's own roll T_base - T_shifted, read
// off the two frames. Otherwise it is the instants' step above.
//
// ## Carry axis (design D23a, lane E)
//
// A re-mark of an expiry's carry INPUTS between the two states moves its forward at a
// fixed spot. Per (uid, expiry), with both states under CarryFrom::State:
//
//   J = ln((S_b - pv_t) / (S_b - pv_b)) - (q_t - q_b) T_b
//
// the ln-forward move from (pv_divs, q) alone, at the base spot, rate and T. pv / q are the
// exact-expiry ExpiryCarry row's when BOTH states have one (else flat_q, pv term 0: a cash
// dividend schedule is taken as unchanged); a CarryFrom::Surface state, or a row in one
// state only, is not decomposed (J = 0: the move stays in unexplained). The rate is NOT
// carry (rho's axis); under an implied-carry row (q = r - ln(F/S)/T) a rate re-mark also moves
// q by dr, so pnl_carry books -w delta S T dr that only the rho axis offsets — with rho off
// (RiskNeeds without rho) that lands in unexplained, as `rate_move_without_rho` flags.
// Per leg pnl_carry = w delta_b S_b J (an Exercised base row: 0) — the
// kernel prices on (S, r, q_eff, T), and for the forward-driven (European) part of the
// price dP/dq_eff = -T S delta exactly; the American early-exercise sensitivity to carry
// beyond that is not modelled and stays in unexplained. No solve: no RiskNeeds flag. An
// unmoved carry gives J == 0, pnl_carry == +0.0 and bit-identical other columns (the
// remainder is then taken over the eight terms exactly as before the axis existed).
//
// all per share, then scaled by the leg weight qty * multiplier and scattered to
// legs in input order (simd/pnl_batch: the eight terms are bit-identical scalar vs
// AVX2). A share leg: pnl_total = pnl_delta = qty dS, every other axis 0.
//
// `rho_b` is read only when the two frames' rates differ for that row; a frame
// evaluated with RiskNeeds::rho == false has rho == 0 and an explain over a rate
// move then reports the move in `pnl_unexplained` AND sets
// ExplainResult::rate_move_without_rho so the caller knows which frame to redo.
//
// ## Which "sigma_t" (VolMoveConvention)
//
//   ContractOwn (default)  sigma_t is the shifted FRAME's sigma: the contract's own vol
//                          at its own residual maturity T_t (a mark, or surface.iv(K, T_t)).
//                          The term-structure roll-down is part of the vol move
//                          (SpiderRock's fixed-strike iVolMove; the only definition a
//                          marks-only state admits).
//   CommonBaseMaturity     sigma_t = shifted_state.sigma_at(contract, T_b): the shifted
//                          surface read at the BASE maturity, the PortfolioPricer::
//                          pnl_explain convention (roll-down lands in unexplained).
//                          Identical to ContractOwn for a mark.
//
// ## Identities (how the tests pin them)
//
// explained is the eight columns summed left to right (plus pnl_carry last when it is
// nonzero) and pnl_unexplained = pnl_total - explained, so explained + pnl_unexplained reproduces
// pnl_total to within 1 ulp of pnl_total (fl(e + fl(t - e)) is not always t); the gate
// is <= 2 ulp, never "bit-exact". The vega split is a fraction partition with the
// remainder in vol_resid: level + skew + curv + resid == pnl_vega to <= 2 ulp.
//
// ## Vega split (level / skew / curvature / residual): a partition to 2 ulp
//
// Per (uid, expiry) group with a surface in the base state: sample both states' vol
// at K_z = F_b exp(z sigma_atm sqrt T_b), z in {-z_ref, 0, +z_ref}; fit
// dsig(z) = a0 + a1 z + a2 z^2 exactly through the three points; split each
// contract's pnl_vega by the fractions a0/dsig_i, a1 z_i/dsig_i, a2 z_i^2/dsig_i and
// put the remainder in vol_resid, so level + skew + curv + resid == pnl_vega to the
// bit. With marks only, the three pivots are the group's marked contracts nearest
// to z (flag PivotFromMarks); fewer than three usable pivots => everything in
// vol_resid. A pivot that resolves NaN => a0 = a1 = a2 = 0 for the group (counted).
//
// ## Exclusions
//
// A leg whose base or shifted row is not Ok (Rolled, Refused, NumericError,
// ModelUnavailable, InvalidContract) contributes to NO total; it is counted per
// reason with its base premium |w P_b| (0 when P_b itself is unavailable). Nothing
// here ever adds a NaN.
//
// ## Determinism
//
// The per-leg arithmetic is embarrassingly parallel over disjoint slots; every
// total is a Neumaier-compensated sum in LEG INPUT ORDER on the calling thread.
// Bit-identical for any thread count.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/risk/market_state.hpp"
#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_frame.hpp"
#include "atx/vol/api/risk/risk_kernel.hpp"
#include "atx/vol/api/risk/risk_types.hpp"

namespace atx::vol {

enum class VolMoveConvention : std::uint8_t { ContractOwn = 0, CommonBaseMaturity = 1 };

struct ExplainOptions {
  bool vega_split{true};
  double z_ref{1.0};             // pivot half-width in standardized moneyness
  bool include_shares{true};
  VolMoveConvention vol_move{VolMoveConvention::ContractOwn};
  // Second-order terms on the TRAPEZOID rule: gamma, volga and vanna are the average
  // of the base and shifted frames' values (SpiderRock's production risk cube
  // convention; the shifted Greeks are free in a stream because they are the next
  // step's base). Off by default so the eight terms are bit-comparable to the
  // PortfolioPricer::pnl_explain oracle (base Greeks only). The identity
  // explained + unexplained == pnl_total holds either way (unexplained is the
  // remainder); `d_vol`/`d_spot` columns are unchanged.
  bool trapezoid_second_order{false};
  unsigned n_threads{0};         // per-leg fan-out degree; totals are serial regardless
  [[nodiscard]] bool operator==(const ExplainOptions &) const = default;
};

enum class ExplainLegKind : std::uint8_t { Option = 0, Share = 1 };

// Column sums over the Ok legs (position-scaled currency). Shared by totals and
// buckets.
struct ExplainTotals {
  double pv_base{0.0};
  double pv_target{0.0};
  double pnl_total{0.0};
  double pnl_delta{0.0};
  double pnl_gamma{0.0};
  double pnl_vega{0.0};
  double pnl_volga{0.0};
  double pnl_vanna{0.0};
  double pnl_theta{0.0};
  double pnl_rho{0.0};
  double pnl_charm{0.0};
  double pnl_unexplained{0.0};
  double vol_level{0.0};
  double vol_skew{0.0};
  double vol_curv{0.0};
  double vol_resid{0.0};
  double shares_pnl{0.0}; // the share legs' part of pnl_total (also inside pnl_delta)
  std::uint32_t n_ok{0};
  std::uint32_t n_excluded{0};
  double excluded_premium{0.0};
  // ── Lane E addition (appended; design D23a) ──
  double pnl_carry{0.0}; // the carry axis (see "Carry axis" above)
};

struct ExplainBucket {
  std::uint64_t key{0}; // by uid: the uid; by expiry: expiry_ns
  std::int64_t expiry_ns{0};
  ExplainTotals totals{};
};

// SoA, one row per leg in INPUT order: option legs first (book order), then share
// legs (book order). `kind` says which.
struct ExplainResult {
  std::int64_t base_ts_ns{0};
  std::int64_t shifted_ts_ns{0};
  double dt_years{0.0};

  std::vector<std::uint64_t> id;
  std::vector<std::uint32_t> uid;
  std::vector<ExplainLegKind> kind;
  std::vector<double> pv_base;
  std::vector<double> pv_target;
  std::vector<double> pnl_total;
  std::vector<double> pnl_delta;
  std::vector<double> pnl_gamma;
  std::vector<double> pnl_vega;
  std::vector<double> pnl_volga;
  std::vector<double> pnl_vanna;
  std::vector<double> pnl_theta;
  std::vector<double> pnl_rho;
  std::vector<double> pnl_charm;
  std::vector<double> pnl_unexplained;
  std::vector<double> vol_level;
  std::vector<double> vol_skew;
  std::vector<double> vol_curv;
  std::vector<double> vol_resid;
  std::vector<double> d_spot;  // per-share state moves this row was decomposed over
  std::vector<double> d_vol;
  std::vector<double> d_time;
  std::vector<double> d_rate;
  std::vector<RiskRoute> route_base;
  std::vector<RiskRoute> route_shifted;
  std::vector<RiskFlags> flags;    // base | shifted flags
  std::vector<RiskLaneStatus> status; // Ok, or the exclusion reason
  // ── Lane E addition (appended; design D23a): the 21st per-leg double column ──
  std::vector<double> pnl_carry; // the carry axis per leg (position-scaled; 0 for shares)

  ExplainTotals total{};
  std::vector<ExplainBucket> by_uid;    // ascending uid
  std::vector<ExplainBucket> by_expiry; // ascending expiry_ns
  std::array<std::uint32_t, kRiskLaneStatusCount> excluded_count{};
  std::array<double, kRiskLaneStatusCount> excluded_premium{};
  std::uint32_t n_pivot_groups{0};
  std::uint32_t n_pivot_edge_fallback{0}; // groups whose pivots resolved NaN
  bool rate_move_without_rho{false};
  double explain_seconds{0.0};

  [[nodiscard]] std::size_t size() const noexcept { return id.size(); }
  void reserve(std::size_t n_option_legs, std::size_t n_share_legs, std::size_t n_uids,
               std::size_t n_expiries);
};

// Explain `base -> shifted` for `book`. Both frames must have been evaluated for
// this exact book (logical id) — InvalidArgument otherwise. `base_state` is needed
// for the vega-split pivots (its surfaces / marks) and the TimeSpec; `shifted_state`
// for the pivots' shifted vol. Pass the same state twice for a frame-only explain
// (then vega_split is forced off).
[[nodiscard]] Result<ExplainResult> explain(const PreparedRiskBook &book,
                                            const MarketState &base_state,
                                            const RiskFrame &base,
                                            const MarketState &shifted_state,
                                            const RiskFrame &shifted,
                                            const ExplainOptions &opts = {});

// In-place form: `out` is reused (reserve is grow-only); bit-identical to `explain`.
[[nodiscard]] Status explain_into(const PreparedRiskBook &book, const MarketState &base_state,
                                  const RiskFrame &base, const MarketState &shifted_state,
                                  const RiskFrame &shifted, const ExplainOptions &opts,
                                  ExplainResult &out);

// Streaming driver: a fixed book, a moving market. `start(s0)` evaluates frame 0;
// every `step(s_k)` evaluates frame k ONCE and explains k-1 -> k, then frame k
// becomes the base. N states cost N evaluations and N-1 explains (pinned by
// RiskStream.FramesComputedOncePerState). The stream borrows `book` and owns the
// kernel. LIFETIME: `step(s_k)` reads the PREVIOUS state (its marks / surfaces feed
// the vega-split pivots), so the caller keeps s_{k-1} alive until `step(s_k)` returns;
// after that only s_k must stay alive (until the next step).
class RiskStream {
public:
  RiskStream(const RiskBook &book, RiskKernel kernel, RiskNeeds needs = {},
             ExplainOptions opts = {});
  ~RiskStream();
  RiskStream(RiskStream &&) noexcept;
  RiskStream &operator=(RiskStream &&) noexcept;
  RiskStream(const RiskStream &) = delete;
  RiskStream &operator=(const RiskStream &) = delete;

  [[nodiscard]] Status start(const MarketState &s0);
  // Evaluates `s1`, explains current -> s1, rolls. The returned reference is valid
  // until the next `step` / `start`.
  [[nodiscard]] Result<const ExplainResult *> step(const MarketState &s1);

  [[nodiscard]] const PreparedRiskBook &prepared() const noexcept;
  [[nodiscard]] const RiskFrame &current() const noexcept;  // last evaluated frame
  [[nodiscard]] const RiskFrame &previous() const noexcept; // the base of the last step
  [[nodiscard]] std::uint64_t n_evaluations() const noexcept;
  [[nodiscard]] std::uint64_t n_explains() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::vol
