#pragma once

// ── Independent cash-dividend REFERENCE (lane D0, vol-sota r5, 2026-10-22) ──
//
// WHY THIS EXISTS. Every discrete-cash-dividend price in the tree comes from ONE
// method: the Vellekoop-Nieuwenhuis spliced lattice (american_discrete_div.cpp),
// at the vendor-reproducing 301 steps by default, and its "converged" comparator
// (`american_discrete_div_converged_price`, SplicedBbs 2,401 + Richardson against
// 1,201) is the SAME lattice refined. A lattice cannot certify itself: its ex-dates
// are rounded onto steps, a first-step ex-date never converges (LEDGER L522), and
// cash that is a large fraction of spot pushes the splice into extrapolation
// (review L3f m1). Deep review F10: converge an independent event-aligned method
// in time and space separately, publish its uncertainty, and call a case
// UNRESOLVED when that uncertainty exceeds the tolerance under test.
//
// WHAT THIS IS. Two independent discretisations of the SAME model, each
// refined with a stated empirical convergence uncertainty:
//
//   A. `pde_reference` — event-aligned finite differences. Uniform grid in the
//      log of the state variable (central differences); Crank-Nicolson in time
//      with a Rannacher start (the first `rannacher_steps` CN steps after the
//      payoff and after EVERY ex-date are replaced by two implicit-Euler half
//      steps each, because each of those events hands the solver non-smooth
//      data) on a time grid GRADED quadratically from each segment's end (the
//      square-root time change that restores second order to CN with an American
//      constraint; uniform steps measured order ~1.25). The terminal payoff is
//      cell-averaged over each node's control volume (kinks split exactly), and
//      so is every node whose volume holds a kink of the post-jump curve. The
//      American constraint is the discrete linear complementarity problem
//      min(M V - b, V - g) = 0 solved each step by policy (Howard) iteration
//      on the tridiagonal M-matrix to rounding (64 ulps of each row's term
//      scale; 1024 from the third iteration, for the rounding ties r = q = 0
//      deep-ITM rows produce) — no penalty parameter and no SOR tolerance, so
//      the constraint adds no error of its own beyond the discretisation. The
//      time grid contains every ex-date bit-exactly (segments between events),
//      the cash jump is applied there by cubic
//      Lagrange interpolation of the post-dividend value in log space, and both
//      exercise rights are tested at the ex-date (cum-dividend against the
//      undiluted stock, post-dividend through the post-jump value). Space and
//      time are refined INDEPENDENTLY — a space sequence at the finest time
//      grid and a time sequence at the finest space grid — and each axis gets
//      its own observed order and Richardson correction.
//   B. `trinomial_reference` — an ex-date-aligned spliced trinomial lattice: a
//      fixed log-spot spacing dx = sigma sqrt(3 dt_max) with per-segment time
//      steps (the middle branch absorbs the shorter steps, so the lattice stays
//      recombining while every ex-date sits ON a step), Bermudan exercise at
//      every step plus the cum/post test at the ex-date, the V&N splice at the
//      exact ex-date. It expands from the root with a padding band below it and
//      so needs NO boundary condition. Its single refinement parameter is the
//      step count (dt and dx^2 both scale as 1/N).
//   The two share the model, the event schedule and three small primitives (the
//   cubic interpolation `interp_uniform_cubic`, the payoff cell average and the
//   jump operator built on them); they share no time discretisation, no
//   constraint treatment, no grid and no boundary treatment. The fast suite pins
//   each shared primitive against an exact value (cubics; closed-form payoffs;
//   one-dividend quadratures), because a defect there would move both methods
//   together and the cross-check could not see it.
//
// MODEL (`CashProcess`). Between ex-dates the stock is geometric Brownian
// motion with drift r - q and volatility sigma; `q` is the residual continuous
// yield on top of the cash schedule (the lattice kernel's convention). At an
// ex-date the price falls by the cash. When the cash would EXCEED the stock:
//   SpotLiquidator  S+ = max(S- - D, 0): the holder receives what the firm is
//                   worth and the stock is absorbed at zero. THE KERNEL'S
//                   TREATMENT: `splice_dividend` floors the post level at 0.
//   SpotSurvivor    S+ = S- - D when S- >= D, else S+ = S- (the dividend is
//                   skipped). Offered to MEASURE the treatment's weight; no
//                   kernel implements it and nothing selects it by default.
//   Escrowed        X = S - escrow(t) is the lognormal state, escrow(t) = the
//                   cash after t discounted at r - q (the Escrowed* lattices'
//                   convention, american_discrete_div.cpp); exercise values the
//                   stock as X + escrow(t), cum and post at the ex-date. The
//                   stock never falls below the cash still to come, so the
//                   liquidator/survivor question cannot arise; X0 <= 0 (cash
//                   exhausting the spot) is OutOfRange, as in the kernel.
// Events outside (0, T] are ignored; an ex-date within kTauAtExpiryRelTol * T of
// expiry lands AT expiry and is applied to the payoff analytically (an American
// holder may exercise the instant before it). Same window as the kernel.
//
// UNCERTAINTY. Per axis, from the last three levels v0, v1, v2 (ratio 2):
//   d1 = v1 - v0, d2 = v2 - v1. ASYMPTOTIC when d1 and d2 share a sign with
//   |d1| > |d2| and the observed order p = log2(d1 / d2) lies in
//   [p_theory / 2, p_theory + 1] (an order far above the scheme's is a
//   cancellation between oscillating terms, not convergence):
//   correction = d2 / (2^min(p, p_theory) - 1),
//   uncertainty = |correction|; using the smaller order makes this empirical
//   indicator larger. Three observed levels do not prove an asymptotic expansion
//   or bound its unobserved remainder, so this is not a rigorous error bound.
//   Otherwise NOT asymptotic: no correction, uncertainty = max(|d1|, |d2|).
// A reference's uncertainty is the sum over its axes plus a rounding floor.
// Retain separate time/space/domain refinements and independent exact controls;
// agreement of these methods cannot detect defects in their shared jump operator.
// `reference_price` reports both methods; it states the PDE value with the PDE
// uncertainty when the two agree (|A - B| <= u_A + u_B) and, when they do not,
// widens the uncertainty to |A - B| + u_A + u_B and clears `methods_agree`: the
// case is then UNRESOLVED, never silently passed.
//
// COST. A reference engine, not a serve path: every call allocates its grids
// (std::vector) and a 4-level PDE ladder runs 7 solves. Pure functions of their
// arguments; concurrent calls are safe.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol::ddiv_ref {

enum class CashProcess : std::uint8_t {
  SpotLiquidator = 0,
  SpotSurvivor = 1,
  Escrowed = 2,
};

// Stable names: "spot-liquidator", "spot-survivor", "escrowed".
[[nodiscard]] const char *to_string(CashProcess value) noexcept;

// One contract under one convention. `dividends` is NON-OWNING: the caller's
// schedule must outlive every call this Problem is passed to (it is read, never
// stored past the call).
struct Problem {
  double S = 0.0;
  double K = 0.0;
  double T = 0.0;
  double sigma = 0.0;
  double r = 0.0;
  double q = 0.0;
  Side side = Side::Put;
  ExerciseStyle exercise = ExerciseStyle::American;
  CashProcess process = CashProcess::SpotLiquidator;
  std::span<const CashDividend> dividends;
};

// InvalidArgument — non-positive / non-finite S, K, T, sigma; non-finite r, q; a
//                   non-finite dividend field or a negative amount
// OutOfRange      — Escrowed with the escrow exhausting the spot (X0 <= 0)
[[nodiscard]] Status validate(const Problem &p);

// ── The event-aligned time grid ──────────────────────────────────────────

struct ExDate {
  double t = 0.0;      // strictly inside (0, T)
  double amount = 0.0; // > 0, coincident events summed
};

struct EventSchedule {
  std::vector<ExDate> interior; // ascending, strictly increasing t
  double terminal_amount = 0.0; // cash landing at expiry
};

// The schedule the solvers see: events with amount 0 or tau outside
// (0, T (1 + kTauAtExpiryRelTol)] dropped, tau >= T (1 - kTauAtExpiryRelTol)
// moved to `terminal_amount`, the rest sorted and merged when bit-equal.
// Precondition: validated inputs.
[[nodiscard]] EventSchedule event_schedule(double T, std::span<const CashDividend> dividends);

// Steps per segment [t_j, t_j+1] (j = 0 .. interior.size()): at level 0,
// max(4, ceil(base * length / T)) — every segment starts from non-smooth data, so
// even a sliver of a segment gets a few steps; at `level` every count times
// 2^level, so the
// grids of successive levels are exactly nested. Precondition: base >= 1,
// 0 <= level < kMaxLevels, base * 2^level <= kMaxGridIntervals (the public solvers
// refuse anything larger; here a level is clamped into range and a count saturates
// at kMaxGridIntervals >> level, so no input reaches an int overflow).
[[nodiscard]] std::vector<int> segment_steps(const EventSchedule &events, double T, int base,
                                             int level);

// Every time level of that grid, ascending from 0 to T; each ex-date appears
// bit-exactly (it is a segment end, never a sum of steps). Inside a segment the
// levels sit at t_end - L (k / n)^grading, k = 0 .. n: grading 1 is uniform (the
// trinomial lattice), grading 2 concentrates steps at the segment's END — where
// backward time starts from non-smooth data (the payoff, or a jump) and the
// exercise boundary moves like sqrt(time) — which is the PDE's default. Either way
// level `l + 1` contains every level of `l` (k -> 2k).
[[nodiscard]] std::vector<double> time_grid(const EventSchedule &events, double T, int base,
                                            int level, double grading = 1.0);

// The k-th level (k = 0 .. n) of one segment under that rule; k == n returns
// `t_start` exactly.
[[nodiscard]] double segment_time(double t_start, double t_end, int n, int k,
                                  double grading) noexcept;

// Cubic Lagrange interpolation of nodal `values` (node i at x0 + i h) at `x`,
// through the four nodes around it (one-sided at the ends; x outside the grid
// is clamped to it). Exact on cubics. Precondition: values.size() >= 4, h > 0;
// outside it the result is NaN (never an out-of-range read).
[[nodiscard]] double interp_uniform_cubic(std::span<const double> values, double x0, double h,
                                          double x) noexcept;

// ── A. The event-aligned PDE ─────────────────────────────────────────────

inline constexpr int kMaxLevels = 8;

// The largest grid either method builds, in intervals (PDE space, time steps over
// [0, T]) or lattice half-width: base * 2^level beyond it is InvalidArgument, and a
// lattice whose padding band would exceed it is OutOfRange. It keeps every node
// count far inside int and the memory to a few hundred MB.
inline constexpr int kMaxGridIntervals = 1 << 22;

struct PdeGrid {
  int space_base = 400; // intervals of the log-state grid at space level 0
  int space_level = 0;  // intervals = space_base * 2^space_level (nested)
  int time_base = 200;  // nominal steps over [0, T] at time level 0 (see segment_steps)
  int time_level = 0;
  // Domain: [min(ln centre, ln K, ln of the spot net of all cash) - w,
  //          max(ln centre, ln K) + w], w = width_sd * domain_sigma * sqrt(T);
  // the centre (ln S, or ln X0 under Escrowed) is a grid node at every level.
  double width_sd = 8.0;
  int rannacher_steps = 2;   // CN steps replaced by 2 implicit half steps, per event
  double domain_sigma = 0.0; // > 0: size the domain with this sigma (common-mode bumps)
  // Time levels per segment at t_end - L (k / n)^time_grading (`time_grid`). 2 is the
  // square-root time change that restores second order to Crank-Nicolson with an
  // American constraint (Reisinger-Whitley 2014); 1 is uniform. Measured on the fast
  // suite's plain American puts: uniform steps converge at order ~1.25 in time.
  double time_grading = 2.0;
};

struct PdeResult {
  double price = 0.0;
  double delta = 0.0;           // central difference on the grid at the centre node
  double gamma = 0.0;           // ditto, second order
  int space_intervals = 0;
  int time_steps = 0;           // implicit solves performed (Rannacher half steps count)
  int max_policy_iterations = 0; // the worst step's Howard iterations (0 European)
  double max_policy_time = 0.0;  // the calendar time of that step
  // The worst LCP violation any accepted step left, relative to its row's term
  // scale (<= 64 eps, or <= 1024 eps on a step the rounding-tie rule accepted),
  // and how many steps that rule accepted (0 European).
  double max_lcp_violation = 0.0;
  int lcp_rounding_ties = 0;
};

// Optional work census. Reset on entry, including refusals; owned by the caller
// and borrowed only for this solve. No clocks. A null pointer performs no census
// traversal/allocation. It does not alter prices, grids, policies or stopping.
struct PdeCoefficientStep {
  std::array<std::uint64_t, 3> coefficient_bits{}; // exact lo/di/up
  std::uint64_t thomas_sweeps = 0;
  std::uint64_t continuation_rows = 0;
  std::uint64_t max_run_length = 0;
};

struct PdeDiagnostics {
  std::uint64_t thomas_sweeps = 0;
  std::uint64_t fresh_seed_sweeps = 0;
  std::uint64_t forward_rows = 0; // rows 1..n, including the right Dirichlet row
  std::uint64_t continuation_rows = 0;
  std::uint64_t identity_rows = 0;
  std::uint64_t continuation_runs = 0;
  // Hypothetical lazy coefficient rows required across consecutive sweeps with
  // bit-identical lo/di/up, extending only to the longest free run encountered.
  // This counts an opportunity; no coefficient cache is used by the solver.
  std::uint64_t exact_key_coefficient_rows = 0;
  std::vector<std::uint64_t> continuation_run_lengths; // index = run length
  std::vector<PdeCoefficientStep> coefficient_steps; // backward solve order
};

// Optional research observation after an unchanged scalar Thomas sweep. Spans
// borrow live state only for the callback; the observer must copy anything kept.
// No callback, recording or allocation occurs when the observer is null.
struct PdeThomasObservation {
  double lo = 0., di = 0., up = 0., bottom = 0., top = 0.;
  int implicit_step = 0;
  bool fresh_data = false;
  std::span<const double> rhs, obstacle, cp, dp, work;
  std::span<const unsigned char> active;
};

struct PdeThomasObserver {
  void (*on_sweep)(const PdeThomasObservation &, void *) = nullptr;
  void *context = nullptr; // borrowed; caller keeps it alive throughout pde_price
};

// One PDE solve. Errors: `validate`'s; InvalidArgument for a grid outside
// [space_base >= 8, time_base >= 1, levels in [0, kMaxLevels), width_sd > 0,
// rannacher_steps >= 0, base * 2^level <= kMaxGridIntervals on both axes];
// OutOfRange when the domain degenerates (no positive finite spacing, e.g. a
// sigma that underflows), or the centered operator is nonfinite or has a negative
// off-diagonal generator coefficient, or an implicit step lacks finite nonpositive
// off-diagonals and strict diagonal dominance (a sufficient M-matrix condition).
// This supported-grid restriction can refuse an otherwise valid model; it never
// silently switches schemes. Internal if a policy iteration fails to settle.
[[nodiscard]] Result<PdeResult> pde_price(const Problem &p, const PdeGrid &grid,
                                         PdeDiagnostics *diagnostics = nullptr,
                                         const PdeThomasObserver *observer = nullptr);

// ── Convergence and the uncertainty rule (header block above) ────────────

struct Axis {
  std::array<double, kMaxLevels> values{}; // refinement levels 0 .. n - 1
  int n = 0;
  double observed_order = 0.0; // log2(d1 / d2); 0 when undefined
  bool asymptotic = false;
  double correction = 0.0;  // added to values[n - 1]
  double uncertainty = 0.0; // empirical residual indicator, not a proved error bound
};

// The rule above, on any refinement sequence of ratio 2. n < 3 is never
// asymptotic (n == 2: uncertainty |d|; n < 2: 0). A last difference of exactly
// zero is not evidence of convergence on its own: the axis then takes the previous
// difference |d1| as its uncertainty and is asymptotic only if d1 is zero too.
[[nodiscard]] Axis richardson_axis(std::span<const double> values, double theory_order) noexcept;

struct Estimate {
  double value = 0.0;       // finest + corrections
  double uncertainty = 0.0; // sum of the axes' uncertainties + rounding floor
  double finest = 0.0;      // the finest grid's raw value
  Axis space;               // PDE: space at the finest time grid; trinomial: unused (n == 0)
  Axis time;                // PDE: time at the finest space grid; trinomial: the step axis
  int max_policy_iterations = 0;
  double max_lcp_violation = 0.0; // PDE: over the ladder's solves (PdeResult's meaning)
  int lcp_rounding_ties = 0;      // PDE: summed over the ladder's solves
};

struct PdeLadder {
  PdeGrid base{}; // its levels are ignored
  int levels = 4; // [3, kMaxLevels]
};

// 2 * levels - 1 solves. Errors: `pde_price`'s; InvalidArgument for levels.
[[nodiscard]] Result<Estimate> pde_reference(const Problem &p, const PdeLadder &ladder);

// ── B. The ex-date-aligned spliced trinomial lattice ─────────────────────

struct TrinomialGrid {
  int steps_base = 500; // nominal steps over [0, T] at level 0 (segment_steps rule)
  int level = 0;
  double pad_sd = 7.0;  // nodes below the root band, in sigma sqrt(T) (+ the cash)
};

// One lattice. Errors: `validate`'s; InvalidArgument for the grid (steps_base *
// 2^level <= kMaxGridIntervals); OutOfRange when a branch probability leaves
// [0, 1] (|r - q - sigma^2 / 2| dx > sigma^2), or when the padding band the cash
// needs would exceed kMaxGridIntervals nodes (a sigma far too small for the cash).
[[nodiscard]] Result<double> trinomial_price(const Problem &p, const TrinomialGrid &grid);

struct TrinomialLadder {
  TrinomialGrid base{};
  int levels = 4; // [3, kMaxLevels]
};

[[nodiscard]] Result<Estimate> trinomial_reference(const Problem &p,
                                                   const TrinomialLadder &ladder);

// ── The cross-checked reference ──────────────────────────────────────────

struct ReferenceOpts {
  PdeLadder pde{};
  TrinomialLadder lattice{};
};

struct Reference {
  Estimate pde;
  Estimate lattice;
  double value = 0.0;       // == pde.value
  double uncertainty = 0.0; // pde.uncertainty, or |A - B| + u_A + u_B on disagreement
  bool methods_agree = false;
};

[[nodiscard]] Result<Reference> reference_price(const Problem &p, const ReferenceOpts &opts = {});

} // namespace atx::vol::ddiv_ref
