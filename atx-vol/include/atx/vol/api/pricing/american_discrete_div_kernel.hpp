#pragma once

// ── Discrete-cash-dividend lattice KERNEL objects: workspace, Screen tier, seeded
//    inversion (lane L3f, vol-sota 2026-10-20) ───────────────────────────────────
//
// WHY THIS EXISTS. About one third of all listed (underlier, expiry) chains and
// 56 % of SPY's quote rows carry an ex-date inside the option's life (research
// report, counted on dev200 / dev200b / cohort5). Those chains price on the
// Vellekoop-Nieuwenhuis spliced lattice (american.hpp, `american_discrete_div_price`)
// which, until this header, had ONE shape: one heap-allocating public call per
// price, no cheap tier, and an implied-vol inversion (`dated_cash_iv`,
// src/fitting/quote_slice.cpp) that brackets and secants on full O(N^2) rollbacks
// — about 10-15 lattices per root. This header adds, WITHOUT changing a single
// default path (nothing here is selected by any default; the fingerprint and the
// parity hashes in the lane report are the proof):
//
//   1. `DiscreteDivWorkspace` — a caller-owned, move-only owner of the four lattice
//      rows, reserved ONCE for a maximum step count. The `american_discrete_div_price`
//      overload taking one is BIT-IDENTICAL to the allocating entry for every scheme
//      (tested over a 2,000+ case grid incl. ex-date-on-expiry, first-step ex-date,
//      cash exhausting the low nodes, empty schedules) and allocation-free after
//      construction (alloc-counter test). One workspace per thread: concurrent
//      calls with distinct workspaces are safe (tested bit-identical vs the serial
//      loop). The HFT serve shape.
//   2. `DiscreteDivTier::Screen` — the spliced lattice at a COARSE step count
//      (`kDiscreteDivScreenScheme` at `kDiscreteDivScreenSteps`; the measured choice
//      and the BBS candidates it beat are on the constants) whose error against the
//      CONVERGED lattice is MEASURED and pinned (`DiscreteDivScreenTier.*`,
//      tests/discrete_div_kernel_test.cpp). Same contract shape as
//      `AlProfile::Screen`: a screening tier for inversion iterates that an
//      accurate confirm finishes, never a served price. It REFUSES (typed
//      OutOfRange, the caller maps it to the Accurate tier) when an ex-date would
//      land on the first steps of its coarsest lattice — that case never
//      converges on a spliced lattice (LEDGER 2026-10-17 L522) — or when the CRR
//      probability window closes at the coarse step.
//   3. `american_discrete_div_implied_vol` — a SEEDED inversion: safeguarded secant
//      on the Screen tier from the caller's seed (the funding-pv / escrowed AL IV
//      the fitting layer already has, a neighbour's root, or 0 = a Black-76 seed
//      at the dated-cash forward), then ONE accurate confirm on the configured
//      (steps, scheme) lattice and at most two Newton polish steps that are
//      accepted only when they REDUCE the accurate residual (the L2e Premium
//      acceptance pattern). Every leg keeps `dated_cash_iv`'s safeguards: the CRR
//      probability window sets the lower endpoint, a quote at or below the lower
//      lattice price or above the maximum-volatility price is a typed OutOfRange,
//      the iteration budget is a cap. Where the Screen leg cannot finish (typed
//      `DiscreteDivIvFallback`) the root is found by the accurate bracketed secant
//      (the dated_cash_iv algorithm on the accurate kernel), so the answer is
//      ALWAYS the accurate lattice's root; the fallback only costs lattices. The
//      two counters say what each root cost.
//
// WHAT IS "ACCURATE". PM ruling (vol-sota resume 3, cash-dividend decision 2): the
// accurate root for dividend rows is the CONVERGED `SplicedBbs` lattice —
// `american_discrete_div_converged_price` below: 2,401 steps with two-point
// Richardson against 1,201, floored at intrinsic — and the vendor-reproducing 301
// `SplicedCrr` default is reported beside it. The inversion's "accurate confirm"
// runs on whatever (steps, scheme) the CALLER configures (today's default route:
// 301 SplicedCrr); the Screen tier's bound is measured against the converged
// lattice AND its distance to the 301 default is tabulated (lane note).
//
// MEASURED SCREEN BOUND vs the converged lattice (|P_screen - P_conv|; relative on
// P_conv >= 0.01; IV in bp on vega-bearing contracts, vega >= 1e-2 * max(P, 1)):
// the numbers are in the lane note (sprints/2026-10-20-L3f-cash-dividend-kernel-sprint.md
// §4) and pinned, as 2-figure round-ups, by `DiscreteDivScreenTier.*`; the text on
// `kDiscreteDivScreenSteps` carries the headline.
//
// Thread-safety: every free function is a pure function of its arguments plus the
// ONE workspace it is handed; a workspace may be used by one thread at a time.
//
// Tier-B: public and includable, deliberately outside the frozen umbrella
// (expiry_record.hpp / chain_tick.hpp pattern). No caller in the tree selects
// anything here; the fitting-layer wiring (policy keys `dividend_kernel`,
// `dividend_kernel_screen_steps`) is lane L3f-wire's (lane note §7).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {

namespace ddiv_detail {
struct LatticeBuffers; // src/pricing/american_discrete_div_detail.hpp (library-private)
} // namespace ddiv_detail

// ── 0. The declared convention of every scheme (lane D0, 2026-10-22) ────────
//
// ADDITIVE METADATA, NO ARITHMETIC. Until this block a dividend-path result
// (`american_discrete_div_price`'s double, `DiscreteDivGreeks`, the converged and
// IV results below) did not say which model produced it. Master plan section
// 3.2: every output reports its model and convention. `discrete_div_convention`
// answers five questions for each scheme, as the code in american_discrete_div.cpp
// implements them (not as a policy wish), and the two result structs below carry
// the answer for the scheme that produced them. The independent reference
// (src/pricing/american_discrete_div_reference.hpp) prices the SAME models:
// SpotShift + Liquidator is its `CashProcess::SpotLiquidator`, Escrowed its
// `CashProcess::Escrowed`.
enum class DiscreteDivProcess : std::uint8_t {
  SpotShift = 0, // dividend-free GBM spot; at an ex-date the stock drops by the cash
  Escrowed = 1,  // X = S - cash to come (discounted at r - q) is the lognormal state
};

// What happens when the cash exceeds the stock it is paid from.
enum class DiscreteDivCashExcess : std::uint8_t {
  Liquidator = 0, // post-dividend level floored at 0 (`splice_dividend`)
  Refused = 1,    // escrowed: unreachable after t = 0 (the stock never falls below
                  // the cash to come); X0 <= 0 at t = 0 is OutOfRange
};

// How an ex-date meets the lattice's time grid.
enum class DiscreteDivExDateGrid : std::uint8_t {
  NearestStep = 0, // round-half-to-even onto a step of the UNIFORM grid, clamped
                   // into [1, N]; an ex-date is moved by up to half a step
  // Lane D1 (SplicedCubic / the production routes, section 4): the same rounding
  // (onto step 0 too when the tree carries >= 4 pre-steps), with the cash scaled
  // so the move changes no forward and the cum / post exercise rights valued at
  // the TRUE ex-date.
  NearestStepRetimed = 1,
};

struct DiscreteDivConvention {
  DiscreteDivProcess process = DiscreteDivProcess::SpotShift;
  // American: at an ex-step the exercise test is applied to the CUM- and the
  // POST-dividend stock (and the cum level on an expiry ex-date).
  bool cum_and_post_exercise = true;
  DiscreteDivCashExcess cash_excess = DiscreteDivCashExcess::Liquidator;
  DiscreteDivExDateGrid ex_dates = DiscreteDivExDateGrid::NearestStep;
  // true: sigma is the SPOT's volatility; false: the escrowed stock's.
  bool spot_volatility = true;

  [[nodiscard]] constexpr bool operator==(const DiscreteDivConvention &) const = default;
};

[[nodiscard]] constexpr DiscreteDivConvention
discrete_div_convention(DiscreteDivScheme scheme) noexcept {
  switch (scheme) {
  case DiscreteDivScheme::SplicedCrr:
  case DiscreteDivScheme::SplicedBbs:
  case DiscreteDivScheme::SplicedBbsr:
    return DiscreteDivConvention{};
  case DiscreteDivScheme::EscrowedBbs:
  case DiscreteDivScheme::EscrowedLeisenReimer:
    return DiscreteDivConvention{.process = DiscreteDivProcess::Escrowed,
                                 .cum_and_post_exercise = true,
                                 .cash_excess = DiscreteDivCashExcess::Refused,
                                 .ex_dates = DiscreteDivExDateGrid::NearestStep,
                                 .spot_volatility = false};
  case DiscreteDivScheme::SplicedCubic:
    return DiscreteDivConvention{.process = DiscreteDivProcess::SpotShift,
                                 .cum_and_post_exercise = true,
                                 .cash_excess = DiscreteDivCashExcess::Liquidator,
                                 .ex_dates = DiscreteDivExDateGrid::NearestStepRetimed,
                                 .spot_volatility = true};
  }
  return DiscreteDivConvention{};
}

// ── 1. The caller-owned workspace ───────────────────────────────────────────

// Owns the four lattice rows (dividend step map, node levels, node values, splice
// scratch) with every row RESERVED for `max_steps` at construction: a price call
// through it performs no heap allocation (tests/discrete_div_kernel_alloc_test.cpp)
// for any `steps <= max_steps()`; a larger request is REFUSED (InvalidArgument),
// never grown, so the budget stated at construction is the budget.
//
// Memory: `reserved_bytes()` = 4 rows x (max_steps + 2) doubles (the +2 covers the
// Leisen-Reimer odd-step rounding) — 9.7 KB at the default 301, 76.9 KB at the
// 2,401-step converged reference.
//
// Move-only (Rule of Five, stated): the rows are heap vectors behind one owner;
// copying a workspace would be a silent allocation on the hot path. A moved-from
// workspace is EMPTY (max_steps() == 0) and refuses every price.
//
// Ownership / threading: one workspace per thread. The object holds no state
// between calls that a caller can observe — every rollback re-initialises the rows
// it reads — so reusing it across contracts, schemes and step counts is the
// intended use.
//
// PRODUCTION SIZE (lane D1). The D1 lattice (section 4; scheme SplicedCubic) runs
// `steps + pre_steps + 1` nodes and one more row, the per-step ex-date offset. The
// one-argument constructor reserves exactly what it always did (the four rows
// above), so a workspace built by it REFUSES a production lattice (InvalidArgument)
// rather than allocate; `DiscreteDivWorkspace(max_steps, max_pre_steps)` reserves
// the production rows too: 4 rows x (max_steps + max_pre_steps + 2) doubles plus
// the offset row x (max_steps + 2) — 14.6 KB at (301, 64), 99.3 KB at (2401, 64).
class DiscreteDivWorkspace {
public:
  // `max_steps` is clamped into [1, kDiscreteDivMaxSteps]; the one allocation this
  // class ever makes happens here.
  explicit DiscreteDivWorkspace(int max_steps = kDiscreteDivDefaultSteps);
  // Production-sized (above): `max_pre_steps` is clamped into [0, 64]
  // (kDiscreteDivMaxPreSteps, section 4).
  DiscreteDivWorkspace(int max_steps, int max_pre_steps);
  ~DiscreteDivWorkspace();
  DiscreteDivWorkspace(DiscreteDivWorkspace &&) noexcept;
  DiscreteDivWorkspace &operator=(DiscreteDivWorkspace &&) noexcept;
  DiscreteDivWorkspace(const DiscreteDivWorkspace &) = delete;
  DiscreteDivWorkspace &operator=(const DiscreteDivWorkspace &) = delete;

  [[nodiscard]] int max_steps() const noexcept { return max_steps_; }
  // -1 for a workspace built without the production rows.
  [[nodiscard]] int max_pre_steps() const noexcept { return max_pre_steps_; }
  // True when a lattice of `steps` steps (any historical scheme) runs allocation-free here.
  [[nodiscard]] bool admits(int steps) const noexcept {
    return buffers_ != nullptr && steps >= 1 && steps <= max_steps_;
  }
  // True when a D1 production lattice of (steps, pre_steps) runs allocation-free here.
  [[nodiscard]] bool admits_production(int steps, int pre_steps) const noexcept {
    return admits(steps) && pre_steps >= 0 && pre_steps <= max_pre_steps_;
  }
  // Heap bytes the rows reserve (capacity, not size).
  [[nodiscard]] std::size_t reserved_bytes() const noexcept;

  // Library-internal access for the kernel TU; the type is opaque to callers.
  [[nodiscard]] ddiv_detail::LatticeBuffers &buffers() noexcept { return *buffers_; }

private:
  std::unique_ptr<ddiv_detail::LatticeBuffers> buffers_;
  int max_steps_ = 0;
  int max_pre_steps_ = -1;
};

// `american_discrete_div_price` (american.hpp) on a caller-owned workspace.
//
// Same arguments, same validation, same arithmetic and the SAME error contract as
// the allocating entry, plus: InvalidArgument when `!workspace.admits(steps)`
// (the contract is "allocation-free", so a step count the workspace was not sized
// for is refused rather than grown). Bit-identical to the allocating entry for
// every scheme and every input the grid test covers.
//
// noexcept: the success path allocates nothing and cannot throw. SAFETY: a REFUSAL
// builds its `Error` message (a std::string; the allocating entry's own texts), so
// the one exception this function could see is std::bad_alloc while refusing, and
// under noexcept that terminates — the JPL fail-loud choice for a serve-path
// kernel, stated here rather than hidden behind a non-noexcept signature whose
// only throw is an out-of-memory process.
[[nodiscard]] Result<double>
american_discrete_div_price(double S, double K, double T, double sigma, double r, double q,
                            Side side, std::span<const CashDividend> dividends, int steps,
                            ExerciseStyle exercise, DiscreteDivScheme scheme,
                            DiscreteDivWorkspace &workspace) noexcept;

// The dated-cash forward F = S e^{(r-q)T} - sum_i D_i e^{(r-q)(T - tau_i)} over the
// events in (0, T] (the same `kTauAtExpiryRelTol` window the lattice applies). The
// forward the escrowed schemes' European converges to, and the Black-76 seed's.
[[nodiscard]] double discrete_div_dated_forward(double S, double T, double r, double q,
                                                std::span<const CashDividend> dividends) noexcept;

// ── 2. Tiers: the converged reference and the Screen tier ───────────────────

enum class DiscreteDivTier : std::uint8_t {
  Accurate = 0, // the caller's (steps, scheme) lattice — today's default route is 301 SplicedCrr
  Screen = 1,   // the coarse measured tier below; never a served price
};

// The converged reference (PM ruling, resume 3): SplicedBbs at 2,401 steps with
// two-point Richardson against 1,201, floored at intrinsic. `fine - coarse` is the
// reference's own uncertainty and is reported beside every table that uses it.
inline constexpr int kDiscreteDivConvergedSteps = 2401;
inline constexpr int kDiscreteDivConvergedCoarseSteps = 1201;

struct DiscreteDivConverged {
  double price = 0.0;  // (N P_N - M P_M) / (N - M), floored at intrinsic for an American
  double fine = 0.0;   // P_N, SplicedBbs at kDiscreteDivConvergedSteps
  double coarse = 0.0; // P_M, SplicedBbs at kDiscreteDivConvergedCoarseSteps
  // Appended (lane D0): the model of all three numbers above.
  DiscreteDivConvention convention = discrete_div_convention(DiscreteDivScheme::SplicedBbs);
};

// Needs `workspace.admits(kDiscreteDivConvergedSteps)`; same error contract as the
// price overload. Two rollbacks (fine first, coarse second, one workspace).
//
// REFUSES (OutOfRange) a CASH-DOMINATED contract (lane D1, L3f review m1, D0 row
// E64): one whose spot, after the cash paid up to and including an ex-date tau in
// (0, T], sits at or below S e^{-6 sigma sqrt(tau)} — beyond the 6-sd band of log
// spot at that date (or at or below zero). Both lattices then read the spot's
// post-dividend continuation off the grid-bottom extrapolation, not off the grid,
// and `fine - coarse` stops being an uncertainty: on E64 (cash 64 % of spot 5.9
// days out, 40 sd) the pair was +2.61 USD off the independent reference while
// claiming 1.0. Every other row of D0's 50-row matrix is at most 2.5 sd (B1).
[[nodiscard]] Result<DiscreteDivConverged>
american_discrete_div_converged_price(double S, double K, double T, double sigma, double r,
                                      double q, Side side, std::span<const CashDividend> dividends,
                                      ExerciseStyle exercise, DiscreteDivWorkspace &workspace);

// The Screen tier: `kDiscreteDivScreenScheme` at `kDiscreteDivScreenSteps` steps.
//
// MEASURED CHOICE (lane scan, sprints/2026-10-20-L3f-cash-dividend-kernel-sprint.md
// §3; vol-rel-avx2, loaded host, one run): {101, 151, 201} x {SplicedBbsr, SplicedBbs,
// SplicedCrr} against the converged lattice on the SPY dividend ladder (2,040
// contracts). The design brief expected a SplicedBbs + Richardson tier at ~0.1-0.3 of
// a 301-step rollback; the measurement rules it out on COST: the BBS final step
// evaluates Black-76 at every step-(N-1) node (two per node when cash lands on
// expiry, SPY's normal case), ~15-25 us that does not shrink with N, so a 101-step
// BBS lattice costs 24 us and the 151-step Richardson pair 55 us against 57.6 us for
// the 301 SplicedCrr rollback it is meant to screen for. The plain SplicedCrr
// rollback at 101 steps costs 10.4 us (0.18 of the accurate rollback) and its root,
// although rippled in sigma (LEDGER L247) and a staircase in K (L499), sits within
// the ripple's amplitude of the accurate root — a seed the accurate Newton polish
// finishes in the same two steps the BBS tier needed. So the Screen tier IS the
// accurate scheme at a coarse count: cheap, no Black-76, same splice, same
// refusals. Smoothness in (K, sigma) is NOT a property of this tier; a (K, sigma)
// record (plan D2) needs SplicedBbs / EscrowedLeisenReimer and is not built here.
// Cost: `discrete_div_screen_equivalent` (below) accurate rollbacks per Screen call.
inline constexpr int kDiscreteDivScreenSteps = 101;
inline constexpr DiscreteDivScheme kDiscreteDivScreenScheme = DiscreteDivScheme::SplicedCrr;

// The Screen tier's cost in ACCURATE-rollback equivalents by node count: (N_s / N)^2,
// times 1.25 under a Richardson scheme (the coarse leg is a quarter of the fine one).
// The single source the tests and the bench charge Screen calls with.
[[nodiscard]] constexpr double discrete_div_screen_equivalent(int screen_steps,
                                                              int accurate_steps) noexcept {
  const double ratio = static_cast<double>(screen_steps) / static_cast<double>(accurate_steps);
  const double richardson =
      (kDiscreteDivScreenScheme == DiscreteDivScheme::SplicedBbsr) ? 1.25 : 1.0;
  return richardson * ratio * ratio;
}

// An ex-date must land on at least this step of the tier's COARSEST lattice. On
// the first step two nodes survive and the V&N splice extrapolates linearly over
// the whole cash — the price never converges (LEDGER 2026-10-17 L522); on the
// second, three nodes carry the quadratic grid-bottom rule. The Screen tier is
// coarse by design, so this trap is MORE reachable here than at 301 steps; it is
// a typed refusal, not a worse price.
inline constexpr int kDiscreteDivScreenMinExStep = 2;

// The Screen step count for one contract, or a typed refusal the caller maps to
// the Accurate tier:
//   OutOfRange — an ex-date in (0, T] would land before `kDiscreteDivScreenMinExStep`
//                on the tier's coarsest lattice (`screen_steps / 2` under Richardson),
//                or `screen_steps` is not below `steps_accurate` (nothing to screen
//                with), or the inputs are not a lattice's (T <= 0, steps out of range)
// The CRR probability window (sigma > |r - q| sqrt(T / coarsest)) is NOT checked
// here — it depends on sigma — and surfaces as the price call's own OutOfRange;
// the inversion below folds it into its lower endpoint exactly as dated_cash_iv
// does (`discrete_div_coarsest_steps`).
[[nodiscard]] Result<int> discrete_div_screen_steps(double T,
                                                    std::span<const CashDividend> dividends,
                                                    int steps_accurate,
                                                    int screen_steps = kDiscreteDivScreenSteps);

// One Screen-tier price: `kDiscreteDivScreenScheme` at `screen_steps` on the
// workspace. Validation and errors as the price overload (OutOfRange when the
// coarse CRR window closes for this sigma). The caller is expected to have
// obtained `screen_steps` from `discrete_div_screen_steps` for this contract.
[[nodiscard]] Result<double>
american_discrete_div_screen_price(double S, double K, double T, double sigma, double r, double q,
                                   Side side, std::span<const CashDividend> dividends,
                                   int screen_steps, DiscreteDivWorkspace &workspace) noexcept;

// ── 3. The seeded inversion ─────────────────────────────────────────────────

// Why the Screen leg handed the root to the accurate kernel (or never ran). The
// SIGMA RETURNED IS THE ACCURATE LATTICE'S ROOT IN EVERY CASE; a fallback is a cost
// record, not an accuracy one. Shape follows `IvPremiumFallback` (american_iv.hpp).
enum class DiscreteDivIvFallback : std::uint8_t {
  None = 0,          // Screen secant + accurate confirm (+ <= 2 polish) reached the root
  NotApplicable = 1, // tier Accurate requested, or `discrete_div_screen_steps` refused
  RangeEdge = 2,     // the quote sits within the Screen bound of the lower lattice price
  NotIncreasing = 3, // the Screen chord slope was not positive (exercise plateau)
  VolBound = 4,      // a Screen iterate was clamped at kIvMin / kIvMax
  PricerFailure = 5, // a Screen lattice refused (coarse CRR window closed)
  NoConvergence = 6, // Screen iterations exhausted, or the polish stopped reducing
};

[[nodiscard]] const char *to_string(DiscreteDivIvFallback value) noexcept;

struct DiscreteDivIvOpts {
  // The caller's seed (a funding-pv / escrowed AL IV of the same quote, a
  // neighbour's root). 0 = none: a Black-76 inversion of `price` at the dated-cash
  // forward seeds the Screen leg, and where that has no solution (an American put
  // above its European ceiling) the Screen leg brackets from the lower endpoint as
  // dated_cash_iv does.
  double seed_sigma = 0.0;
  // Accept a root when its Newton distance on the ACCURATE lattice,
  // |P(sigma) - price| / slope, is <= tol (the quote slice's iv_tolerance, 1e-7).
  double tol = 1.0e-7;
  // Total price-call budget across both tiers (the quote slice's max_iv_iterations).
  std::uint16_t max_iter = 64;
  DiscreteDivTier tier = DiscreteDivTier::Screen;
  int screen_steps = kDiscreteDivScreenSteps;
};

struct DiscreteDivIvResult {
  double sigma = 0.0;
  // Price calls on the Screen tier (each `discrete_div_screen_equivalent(N_s, N)` of
  // an accurate rollback: 0.11 at 101 / 301) and on the accurate (steps, scheme)
  // lattice.
  std::uint16_t screen_lattices = 0;
  std::uint16_t accurate_lattices = 0;
  DiscreteDivIvFallback fallback = DiscreteDivIvFallback::None;
  // Appended (lane D0): the model `sigma` is a volatility OF — the accurate
  // (steps, scheme) lattice's, `discrete_div_convention(scheme)`.
  DiscreteDivConvention convention{};
};

// Invert `price` for the volatility of the accurate (steps, scheme) lattice.
//
// @param price     the quote (> 0, finite)
// @param S,K,T,r,q,side,dividends  as `american_discrete_div_price` (American exercise)
// @param steps,scheme  the ACCURATE lattice the root is confirmed on
// @param opts      seed, tolerance, budget, tier
// @param workspace must admit `steps` (and `opts.screen_steps` under tier Screen)
// @return the root with its cost record, or an Error:
//   InvalidArgument — the price overload's refusals; a workspace too small
//   OutOfRange      — no admissible volatility range (lattice window >= kIvMax); the
//                     quote at or below the lower lattice price (dated_cash_iv's
//                     "at or below the identifiable range"); the quote above the
//                     maximum-volatility price; a non-finite lattice price
//   Unavailable     — the iteration budget is exhausted before the accurate Newton
//                     distance reaches `opts.tol`
[[nodiscard]] Result<DiscreteDivIvResult>
american_discrete_div_implied_vol(double price, double S, double K, double T, double r, double q,
                                  Side side, std::span<const CashDividend> dividends, int steps,
                                  DiscreteDivScheme scheme, const DiscreteDivIvOpts &opts,
                                  DiscreteDivWorkspace &workspace);

// ── 4. The D1 production path (lane D1, vol-sota r5, 2026-10-22) ────────────
//
// THE SPLICE DEFECT (D0 measured it; this lane identified and fixed it). Against
// the independent reference (src/pricing/american_discrete_div_reference.hpp,
// docs/american-cash-dividend-reference.md) the 301-step SplicedCrr overprices
// multi-event chains by 2.8 to 4.4 IV bp PER EVENT. The cause is the splice's
// LINEAR interpolation: at an ex-date the continuation is re-read at the
// post-dividend levels off the chord between two nodes, and the chord of a convex
// curve lies above it by (x - x_j)(x_{j+1} - x) V''/2 — a positive bias of order
// h^2 V'' = O(1/N) at every ex-date, which the events add up. Measured on D0's
// 12-event LEAPS truncated to its first k events (doc section 11): the error grows
// +0.0167 USD per event under the linear splice and is flat under a cubic one.
//
// THE PRODUCTION LATTICE (src/pricing/american_discrete_div_detail.hpp,
// `run_production_lattice`): the same tree and model as SplicedCrr with
//   1. the CUBIC splice (four-point Lagrange on the same geometric nodes; O(h^4));
//   2. forward-preserving re-dating: cash rounded onto step t_k is scaled by
//      exp(-(r - q)(tau - t_k)), so the rounding moves no forward;
//   3. re-timed exercise at an ex-step: cum / post exercise valued at the TRUE
//      ex-date (sgn (x e^{-q s} - K e^{-r s}), s = tau - t_k), plus exercise at
//      t_k itself on the side of the ex-date t_k actually sits;
//   4. PRE-STEPS: the tree is rooted `pre_steps` steps before valuation, so the
//      t = 0 layer has pre_steps + 1 nodes centred on S. An ex-date in the first
//      steps then has the nodes a splice needs (the first-step ex-date never
//      converged on the historical lattice, LEDGER L522; D0 rows B1p / B1c,
//      +19.3 / +17.0 bp at 301 steps), and delta / gamma are read AT t = 0.
//
// THE ROUTE (`discrete_div_lattice_route`, the documented rule; every row cited is
// a D0 matrix row, re-scored in doc section 11):
//   * kill switch (`opts.kill_switch`): the historical SplicedCrr at base_steps,
//     bit-identical to `american_discrete_div_price` (method HistoricalSpliced);
//   * Standard accuracy: method CubicCrr. steps = max(base_steps,
//     kDiscreteDivStepsPerEvent x in-life events), capped at
//     kDiscreteDivMaxRouteSteps (flag StepsCapped). The per-event coefficient is
//     the residual first-order term the cubic lattice keeps per event (the
//     exercise-boundary kink next to each ex-date; D12p / C8p truncation study):
//     25 steps per event hold it under 0.5 bp of IV, so every D0 chain (<= 12
//     events) stays at 301 — no global raise.
//   * pre_steps = the smallest even count >= 2 that puts the post-dividend level of
//     every node inside the 6-sd band of every in-life ex-step at or above the
//     grid's second node, evaluated at kDiscreteDivRouteSigma (the route does not
//     depend on the contract's sigma, so a root find and a batch group see ONE
//     discretisation); capped at kDiscreteDivMaxPreSteps, beyond which the route
//     says GridBottomExtrapolated and reports the depth. The count, and so the
//     flag, is a statement for sigma >= kDiscreteDivRouteSigma: below it the
//     cash spans more rungs of the contract's tree than of the route's, so a band
//     node can read the extrapolated grid bottom with the flag clear. The cap is
//     1024: D0 row
//     E64 (cash 64 % of spot, 5.9 days out; price-only) asks for 334 / 688 / 940
//     pre-steps at 301 / 1201 / 2401 steps, and at the checkpoint's cap of 64 its
//     band read below the grid (Refined +1.28 USD on E64p). What still exceeds 1024
//     is cash beyond the band (a post-dividend level <= 0 for the band's lowest
//     node), which no pre-step count reaches.
//   * Refined accuracy: method CubicBbsRichardson — the BBS final step plus the
//     same-parity Richardson pair (N, (N + 1) / 2), N = kDiscreteDivRefinedSteps
//     (or the Standard rule's count if larger): the reference-grade route. EXCEPT
//     when an in-life ex-date falls in the last two COARSE steps, (T - 2 T / M, T)
//     with M = (N + 1) / 2: the coarse lattice's BBS final step then spans the
//     ex-date while the fine lattice splices it, the pair is inconsistent (D0 row
//     B2c: +1.885 bp), and the route runs method CubicCrr at
//     kDiscreteDivNearExpirySteps instead (flag NearExpiryExDate). IV bp by
//     inversion, the convention of the +1.885 above: B2c +0.114, B2p -0.268
//     (doc section 11.4).
// A route is a pure function of (S, T, schedule, opts): no sigma, rate or strike.
//
// Thread-safety: as the rest of this header (pure functions + one workspace).

inline constexpr int kDiscreteDivMaxPreSteps = 1024;
inline constexpr int kDiscreteDivStepsPerEvent = 25;
inline constexpr int kDiscreteDivMaxRouteSteps = 2401;
inline constexpr int kDiscreteDivRefinedSteps = 1201;
inline constexpr int kDiscreteDivNearExpirySteps = 2401;
inline constexpr double kDiscreteDivRouteSigma = 0.10;

enum class DiscreteDivAccuracy : std::uint8_t {
  Standard = 0, // CubicCrr at the routed step count
  Refined = 1,  // CubicBbsRichardson at kDiscreteDivRefinedSteps (CubicCrr at
                // kDiscreteDivNearExpirySteps for an ex-date in the last two coarse steps)
};

enum class DiscreteDivMethod : std::uint8_t {
  HistoricalSpliced = 0,  // the kill switch: SplicedCrr, linear splice, bit-identical
  CubicCrr = 1,           // the production lattice, CRR terminal step
  CubicBbsRichardson = 2, // the production lattice, BBS final step, Richardson pair
};

// Stable names: "historical-spliced", "cubic-crr", "cubic-bbs-richardson".
[[nodiscard]] const char *to_string(DiscreteDivMethod value) noexcept;

// What the route had to do (a bit set in `DiscreteDivLatticeRoute::flags`).
enum class DiscreteDivRouteFlag : std::uint16_t {
  NoInLifeEvent = 1U << 0,          // no cash strictly inside (0, T): no splice runs
  PreStepsForEarlyExDate = 1U << 1, // more than the minimum two pre-steps were needed
  StepsRaisedForEvents = 1U << 2,   // the per-event rule raised the step count
  StepsCapped = 1U << 3,            // ... and hit kDiscreteDivMaxRouteSteps
  GridBottomExtrapolated = 1U << 4, // kDiscreteDivMaxPreSteps left a band node below the grid
                                    // (at kDiscreteDivRouteSigma: a sigma >= 0.10 statement)
  ExDateOnStepZero = 1U << 5,       // an ex-date within half a step of t = 0, spliced at t = 0
  KillSwitch = 1U << 6,             // historical lattice by request
  NearExpiryExDate = 1U << 7,       // Refined: an ex-date in the last two coarse steps
};

[[nodiscard]] constexpr std::uint16_t bit(DiscreteDivRouteFlag flag) noexcept {
  return static_cast<std::uint16_t>(flag);
}

struct DiscreteDivLatticeRoute {
  DiscreteDivMethod method = DiscreteDivMethod::CubicCrr;
  int steps = 0;          // the (fine) lattice
  int coarse_steps = 0;   // the Richardson partner (CubicBbsRichardson), else 0
  int pre_steps = 0;      // even; 0 under HistoricalSpliced
  int events = 0;         // ex-dates strictly inside (0, T) with cash > 0
  int first_ex_step = -1; // the earliest in-life ex-date's step on the fine lattice; -1 none
  double max_cash_ratio = 0.0;    // largest in-life amount / S
  double grid_bottom_depth = 0.0; // node spacings the deepest band node's post level sits
                                  // under the grid after pre-steps; 0 = inside
  std::uint16_t flags = 0;        // DiscreteDivRouteFlag bits

  [[nodiscard]] constexpr bool has(DiscreteDivRouteFlag flag) const noexcept {
    return (flags & bit(flag)) != 0U;
  }
};

struct DiscreteDivProductionOpts {
  DiscreteDivAccuracy accuracy = DiscreteDivAccuracy::Standard;
  int base_steps = kDiscreteDivDefaultSteps; // [4, kDiscreteDivMaxSteps]
  bool kill_switch = false;
};

// The route rule above. InvalidArgument for S, T or a dividend field the price
// entry would refuse, or base_steps outside [4, kDiscreteDivMaxSteps]. The
// per-event raise is capped at max(base_steps, kDiscreteDivMaxRouteSteps).
[[nodiscard]] Result<DiscreteDivLatticeRoute>
discrete_div_lattice_route(double S, double T, std::span<const CashDividend> dividends,
                           const DiscreteDivProductionOpts &opts);

// The price on a route (the route must be `discrete_div_lattice_route`'s for the
// same S, T and schedule). Validation and errors as the price overload, plus
// InvalidArgument when `!workspace.admits_production(route.steps, route.pre_steps)`
// (a HistoricalSpliced route needs only `admits(route.steps)`). noexcept: as the
// price overload (SAFETY note there).
[[nodiscard]] Result<double>
american_discrete_div_route_price(const DiscreteDivLatticeRoute &route, double S, double K,
                                  double T, double sigma, double r, double q, Side side,
                                  std::span<const CashDividend> dividends, ExerciseStyle exercise,
                                  DiscreteDivWorkspace &workspace) noexcept;

struct DiscreteDivProductionPrice {
  double price = 0.0;
  DiscreteDivLatticeRoute route{};
  DiscreteDivConvention convention = discrete_div_convention(DiscreteDivScheme::SplicedCubic);
};

// Route + price in one call.
[[nodiscard]] Result<DiscreteDivProductionPrice>
american_discrete_div_production_price(double S, double K, double T, double sigma, double r,
                                       double q, Side side,
                                       std::span<const CashDividend> dividends,
                                       ExerciseStyle exercise,
                                       const DiscreteDivProductionOpts &opts,
                                       DiscreteDivWorkspace &workspace);

struct DiscreteDivProductionIv {
  DiscreteDivIvResult iv{};          // sigma + cost record (section 3's fields)
  DiscreteDivLatticeRoute route{};   // the accurate route the root is confirmed on
  DiscreteDivLatticeRoute screen{};  // the Screen route (CubicCrr at opts.screen_steps)
};

// The seeded inversion of section 3 on a production route: the Screen leg runs the
// CubicCrr lattice at `iv_opts.screen_steps` (with its own pre-steps — a cubic
// screen, so its root sits within the CRR floor of the accurate one instead of the
// linear splice's per-event bias), the confirm / polish / bracketed fallback run on
// the accurate route. The root is ALWAYS the accurate route's; errors as section 3.
// A Screen tier needs `iv_opts.screen_steps < route.steps`; otherwise (or with tier
// Accurate) the accurate bracketed secant runs alone (fallback NotApplicable).
[[nodiscard]] Result<DiscreteDivProductionIv>
american_discrete_div_production_iv(double price, double S, double K, double T, double r,
                                    double q, Side side, std::span<const CashDividend> dividends,
                                    const DiscreteDivProductionOpts &opts,
                                    const DiscreteDivIvOpts &iv_opts,
                                    DiscreteDivWorkspace &workspace);

// ── The event-aware sensitivities ────────────────────────────────────────────
//
// Every number below is computed on ONE route (fixed node set and pre-steps), so a
// bump moves the model, never the discretisation. Supported greeks are FINITE;
// an unsupported one is NaN with its bit set in `unsupported` — never 0.
//   delta, gamma  the t = 0 central stencils of the pre-stepped tree (S d^2, S, S u^2);
//                 under CubicBbsRichardson the route's own Richardson pair of them
//                 (n d_n - m d_m) / (n - m), as its price: the stencil's error is
//                 first order in 1 / N (D0 row E20p, Refined: +2.9e-4 from the fine
//                 lattice alone, review-D1 M1);
//   vega          the Richardson pair of central sigma bumps at h and h / 2,
//                 h = kDiscreteDivSigmaBumpRel x sigma, (4 v(h / 2) - v(h)) / 3: it
//                 removes the bump's O(h^2) bias (E20p: -0.27 % at h alone, +0.003 %).
//                 UNSUPPORTED (reason ExerciseDominated) on an exercise-dominated
//                 American call: an in-life ex-date tau_i where early exercise just
//                 before it can be optimal, D_i > K (1 - e^{-r (t_next - tau_i)})
//                 (t_next the next in-life ex-date or T), with the cum-dividend dated
//                 forward there at least kDiscreteDivExerciseDominatedSd standard
//                 deviations in the money, ln(F(tau_i-) / K) >= z sigma sqrt(tau_i).
//                 Its vega is the small residual of the paths that keep the option,
//                 read through the exercise boundary at the ex-date; on D0 row F1c
//                 (z = 2.23) the lattice is +1.3 to +1.7 % off the reference at every
//                 N and every bump size (doc section 11.6), so it is not served;
//   rho, phi      central r and q bumps (kDiscreteDivProductionRateBump /
//                 kDiscreteDivProductionYieldBump, below), q being the residual
//                 continuous yield on top of the cash;
//   dP/dD_i       per event, central amount bump (discrete_div_amount_bump; one-sided
//                 at a zero amount, as american_discrete_div_dividend_sensitivities);
//   theta, charm  the tree's two-step stencils (dP/dt at fixed spot, per year,
//                 calendar convention). UNSUPPORTED when an in-life ex-date sits on
//                 steps 0..2 of the route: the stencil would difference across the
//                 ex-date, and a roll across an ex-date is an EVENT, not a derivative;
//   roll          the event-aware time roll over `roll_horizon` (below). Under
//                 CubicBbsRichardson theta and charm read the fine lattice's stencils
//                 (the coarse one's first ex-step can sit inside its two-step stencil).
enum class DiscreteDivGreek : std::uint8_t {
  Delta = 0,
  Gamma = 1,
  Vega = 2,
  Rho = 3,
  Phi = 4,
  Theta = 5,
  Charm = 6,
  DividendAmount = 7,
  Roll = 8,
};

// Why a greek is unsupported (a bit 1 << reason in `DiscreteDivProductionGreeks::
// reasons`; the greek's own bit says WHICH greek). NaN, never 0, in every case.
enum class DiscreteDivUnsupportedReason : std::uint8_t {
  NotRequested = 0,      // dP/dD without slots, the roll with horizon <= 0
  ExDateInStencil = 1,   // theta / charm: an in-life ex-date on steps 0..2
  RollRefused = 2,       // the roll's re-route or rollback refused (its error class)
  ExerciseDominated = 3, // vega: the exercise-dominated American call (above)
  Refused = 4,           // batch: the group or the whole bundle refused
};

// Stable names: "not_requested", "ex_date_in_stencil", "roll_refused",
// "exercise_dominated", "refused".
[[nodiscard]] const char *to_string(DiscreteDivUnsupportedReason value) noexcept;

// The exercise-dominated threshold of the vega rule above, in standard deviations
// of log spot at the ex-date (2: about 98 % of the lognormal mass in the money).
inline constexpr double kDiscreteDivExerciseDominatedSd = 2.0;

// The production bundle's rate / yield bumps (lane D1, r6). NOT the historical
// bundle's kDiscreteDivRateBump / kDiscreteDivYieldBump (1e-4, american.hpp): r and q
// move no node, but they move the exercise decisions node by node, so a lattice price
// is rippled in r and q below ~1e-3 and a 1e-4 central difference reads that ripple.
// Measured on the Refined route against the reference's bump study (D0 rows A2p, B3p,
// C4c, D12p, E20p, F1c, B1p; doc section 11): worst relative error 1.7e-2 / 8.9e-3
// (rho / phi, D12p) at 1e-4, 2.3e-3 (C4c) / 1.0e-3 (E20p) at 1e-3; 2e-3 and a
// Richardson pair were no better.
inline constexpr double kDiscreteDivProductionRateBump = 1.0e-3;
inline constexpr double kDiscreteDivProductionYieldBump = 1.0e-3;

// The calendar roll to t + horizon at a fixed SPOT: an ex-date in (0, horizon] is
// an event operation — its cash comes off the stock (S - cash, floored at 0) and
// off the schedule — and every later ex-date moves `horizon` closer. Not a
// derivative and never divided by the horizon.
struct DiscreteDivTimeRoll {
  double horizon = 0.0;
  int events_crossed = 0;
  double cash_crossed = 0.0;
  double rolled_price = 0.0; // the option at t + horizon (NaN when unsupported)
  double roll = 0.0;         // rolled_price - price
};

struct DiscreteDivProductionGreeks {
  double price = 0.0;
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double rho = 0.0;
  double phi = 0.0;
  double theta = 0.0;
  double charm = 0.0;
  DiscreteDivTimeRoll roll{};
  std::uint16_t unsupported = 0; // bits 1 << DiscreteDivGreek
  std::uint16_t reasons = 0;     // bits 1 << DiscreteDivUnsupportedReason
  DiscreteDivLatticeRoute route{};

  [[nodiscard]] constexpr bool supported(DiscreteDivGreek greek) const noexcept {
    return (unsupported & (1U << static_cast<unsigned>(greek))) == 0U;
  }
  [[nodiscard]] constexpr bool has(DiscreteDivUnsupportedReason reason) const noexcept {
    return (reasons & (1U << static_cast<unsigned>(reason))) != 0U;
  }
};

// The bundle above. `dP_dDiv_out` is empty (no dividend sensitivities computed:
// bit DividendAmount is SET, as not-computed is never reported as a value) or
// holds one slot per entry of
// `dividends` (an event outside (0, T] gets exactly 0). `roll_horizon` <= 0 skips
// the roll (its bit set). Cost: 9 rollbacks (5 when vega is unsupported) + 2 per
// in-life event + 1 for the roll (x 2 under CubicBbsRichardson). Errors: the route's
// and the price's, plus
// InvalidArgument for a wrongly sized `dP_dDiv_out` or a non-finite horizon.
[[nodiscard]] Result<DiscreteDivProductionGreeks> american_discrete_div_production_greeks(
    double S, double K, double T, double sigma, double r, double q, Side side,
    std::span<const CashDividend> dividends, ExerciseStyle exercise,
    std::span<double> dP_dDiv_out, double roll_horizon, const DiscreteDivProductionOpts &opts,
    DiscreteDivWorkspace &workspace);

} // namespace atx::vol
