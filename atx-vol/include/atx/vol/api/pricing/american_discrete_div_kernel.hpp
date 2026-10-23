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
class DiscreteDivWorkspace {
public:
  // `max_steps` is clamped into [1, kDiscreteDivMaxSteps]; the one allocation this
  // class ever makes happens here.
  explicit DiscreteDivWorkspace(int max_steps = kDiscreteDivDefaultSteps);
  ~DiscreteDivWorkspace();
  DiscreteDivWorkspace(DiscreteDivWorkspace &&) noexcept;
  DiscreteDivWorkspace &operator=(DiscreteDivWorkspace &&) noexcept;
  DiscreteDivWorkspace(const DiscreteDivWorkspace &) = delete;
  DiscreteDivWorkspace &operator=(const DiscreteDivWorkspace &) = delete;

  [[nodiscard]] int max_steps() const noexcept { return max_steps_; }
  // True when a lattice of `steps` steps (any scheme) runs allocation-free here.
  [[nodiscard]] bool admits(int steps) const noexcept {
    return buffers_ != nullptr && steps >= 1 && steps <= max_steps_;
  }
  // Heap bytes the rows reserve (capacity, not size).
  [[nodiscard]] std::size_t reserved_bytes() const noexcept;

  // Library-internal access for the kernel TU; the type is opaque to callers.
  [[nodiscard]] ddiv_detail::LatticeBuffers &buffers() noexcept { return *buffers_; }

private:
  std::unique_ptr<ddiv_detail::LatticeBuffers> buffers_;
  int max_steps_ = 0;
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

} // namespace atx::vol
