#pragma once

// ── Internal seam: the Vellekoop-Nieuwenhuis lattice kernel ───────────────────
//
// A NARROW, library-private declaration surface exposing the file-local lattice
// primitives that american_discrete_div.cpp defines, so a SECOND translation unit
// (american_discrete_div_kernel.cpp: the caller-owned workspace, the Screen tier
// and the seeded inversion, lane L3f 2026-10-20) can run the SAME rollbacks
// without re-deriving them. The DEFINITIONS stay in american_discrete_div.cpp;
// only the two value types and the declarations of the entry points move here.
// Signatures and numerics are unchanged: every public price the default route
// serves is bit-identical through this seam (proof: atx-vol-discrete-div-tests
// 88/88, atx-vol-discrete-div-smoothing-tests 28/28 and the opt-in lattice
// parity hashes abc75955 / 747d3e6c, lane report).
//
// Everything lives in `atx::vol::ddiv_detail`. american_discrete_div.cpp brings
// the names into scope with using-declarations so its unqualified call sites are
// undisturbed; the kernel TU qualifies with `ddiv_detail::`.
//
// NOT a public API. Not installed. No stability guarantees (the
// american_boundary.hpp pattern).

#include <cstddef>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"       // Result, Status, Side, ExerciseStyle
#include "atx/vol/api/pricing/american.hpp" // CashDividend, DiscreteDivScheme
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp" // routes, workspace (D1)

namespace atx::vol::ddiv_detail {

// Price + every greek ONE rollback can carry. `delta`/`gamma`/`theta` need
// `steps >= 2` and `charm` needs `steps >= 3`; below those counts the field
// stays 0 and the public entry that would expose it refuses first, so a
// fabricated 0 never reaches a caller. Under the BBS final step only `price` is
// meaningful (no public entry exposes smoothed greeks).
struct LatticeOutcome {
  double price = 0.0;
  double delta = 0.0;
  double gamma = 0.0;
  double theta = 0.0; // dP/dt, calendar convention, per year
  double charm = 0.0; // d(delta)/dt, calendar convention
};

// The per-call lattice workspace: the dividend step map, the node levels, the
// node values and the splice scratch. A public entry owns ONE of these and every
// rollback it runs reuses it — the greek bundle's eight rollbacks and BBSR's
// coarse rollback included — so a call allocates these buffers once, sized by
// its first (largest) rollback. `scratch` is grown only when a dividend lands
// inside the window, exactly as before the workspace existed. The kernel TU's
// DiscreteDivWorkspace owns one of these with every row RESERVED for its
// maximum step count, so the same `assign` / `resize` calls below never
// allocate there.
struct LatticeBuffers {
  std::vector<double> step_amount;
  std::vector<double> level;
  std::vector<double> value;
  std::vector<double> scratch;
  // Lane D1, production rollback only: the ex-date offset tau - t_k of the cash
  // on each step (amount-weighted when two events share a step). The historical
  // kernels never touch it.
  std::vector<double> step_shift;
};

// Boundary validation (agent profile §4): every external input is checked ONCE,
// here, and the kernels assume the validated invariants. The error texts are the
// public entries' documented ones.
[[nodiscard]] Status validate(double S, double K, double T, double sigma, double r, double q,
                              std::span<const CashDividend> dividends, int steps);

// The spliced CRR rollback; `bbs` selects the Broadie-Detemple final step.
[[nodiscard]] Result<LatticeOutcome> run_lattice(double S, double K, double T, double sigma,
                                                 double r, double q, Side side,
                                                 std::span<const CashDividend> dividends, int steps,
                                                 ExerciseStyle exercise, bool bbs,
                                                 LatticeBuffers &buffers);

// ── The D1 production rollback (lane D1, 2026-10-22) ─────────────────────────
// The spliced CRR with the four corrections the kernel header's "D1 production
// path" block documents and measures: the cubic splice, forward-preserving
// re-dating of each ex-date onto its step, exercise rights re-timed to the true
// ex-date, and PRE-STEPS (the tree starts `pre_steps` steps before valuation, so
// the t = 0 layer holds pre_steps + 1 nodes centred on S: an early ex-step has
// the nodes a splice needs, and delta / gamma are read AT t = 0). A separate
// function, not a flag on `run_lattice`: the historical rollback stays the
// historical source line for line, so no default route can move.

// How the splice re-reads the continuation at the post-dividend levels.
enum class SpliceInterp : unsigned char {
  Linear = 0, // the historical chord of the two bracketing nodes
  Cubic = 1,  // four-point Lagrange on the same geometric nodes
};

struct StepMap; // below

struct ProductionSpec {
  SpliceInterp splice = SpliceInterp::Cubic;
  // Even, in [0, kDiscreteDivMaxPreSteps]. With pre_steps >= 4 an ex-date nearer
  // t = 0 than half a step is spliced ON the t = 0 layer (it has the nodes);
  // below that it is clamped onto step 1, as in the historical kernel.
  int pre_steps = 0;
  bool align_forward = true;  // cash scaled by exp(-(r - q)(tau - t_k))
  bool align_exercise = true; // cum / post exercise valued at the true ex-date
  bool bbs = false;           // Broadie-Detemple final step
  // Sensitivity hooks (no schedule is ever copied): the amount of
  // dividends[bump_index] is read as `bump_amount` (bump_index == kNoBump: none),
  // and every ex-date is read as tau - tau_shift (the calendar roll; an event at
  // or before the shifted origin is dropped, exactly as a past event is).
  std::size_t bump_index = kNoBump;
  double bump_amount = 0.0;
  double tau_shift = 0.0;
  // A step map built once per (schedule, T, r, q, steps, pre_steps) group by
  // `build_production_step_map` (lane D1 batching): read instead of rebuilt.
  // Null: the rollback builds its own. Must not be combined with the hooks above.
  const StepMap *prebuilt = nullptr;

  static constexpr std::size_t kNoBump = static_cast<std::size_t>(-1);
};

// The production rollback's per-step cash and ex-date offset (`step_shift`), for
// steps 0..N: the event placement, the forward-preserving scale and the offsets
// the re-timed exercise reads — everything about the schedule a rollback needs,
// and nothing that depends on sigma or the strike.
struct StepMap {
  std::span<const double> amount; // steps + 1 entries
  std::span<const double> shift;  // steps + 1 entries
  bool any = false;               // some cash landed on a step
};

// Fills `amount` / `shift` (each `steps + 1` long) for a production rollback of
// `steps` steps and `pre_steps` pre-steps under `spec`'s alignment and hooks;
// returns whether any cash landed. Validated inputs; the spans are the caller's.
[[nodiscard]] bool build_production_step_map(double T, double r, double q,
                                             std::span<const CashDividend> dividends, int steps,
                                             const ProductionSpec &spec, std::span<double> amount,
                                             std::span<double> shift) noexcept;

// The prebuilt maps of one route: the fine lattice's and, under
// CubicBbsRichardson, the coarse lattice's (empty otherwise).
struct RouteMaps {
  StepMap fine{};
  StepMap coarse{};
};

// Kernel TU (lane D1 batching): the price and the seeded inversion of section 4 on
// a route whose step maps a batch group built once. `maps` / `screen_maps` may be
// null (the rollback then builds its own). Validation and errors as
// `american_discrete_div_route_price` / `american_discrete_div_production_iv`.
[[nodiscard]] Result<double> route_price_prebuilt(const DiscreteDivLatticeRoute &route,
                                                  const RouteMaps *maps, double S, double K,
                                                  double T, double sigma, double r, double q,
                                                  Side side,
                                                  std::span<const CashDividend> dividends,
                                                  ExerciseStyle exercise,
                                                  DiscreteDivWorkspace &workspace);
[[nodiscard]] Result<DiscreteDivIvResult>
route_iv_prebuilt(double price, const DiscreteDivLatticeRoute &route, const RouteMaps *maps,
                  const DiscreteDivLatticeRoute *screen, const RouteMaps *screen_maps, double S,
                  double K, double T, double r, double q, Side side,
                  std::span<const CashDividend> dividends, const DiscreteDivIvOpts &opts,
                  DiscreteDivWorkspace &workspace);
// The Greek bundle of section 4 on a given route: the base and vega legs read
// `maps` (they share the group's r and q); the rate / yield / dividend legs and the
// roll rebuild theirs, since a step map's forward scale depends on r - q.
[[nodiscard]] Result<DiscreteDivProductionGreeks>
route_greeks_prebuilt(const DiscreteDivLatticeRoute &route, const RouteMaps *maps, double S,
                      double K, double T, double sigma, double r, double q, Side side,
                      std::span<const CashDividend> dividends, ExerciseStyle exercise,
                      std::span<double> dP_dDiv_out, double roll_horizon,
                      const DiscreteDivProductionOpts &opts, DiscreteDivWorkspace &workspace);

// The Screen route of `american_discrete_div_production_iv`: the Standard rule at
// base `screen_steps`, no per-event raise (a seed lattice keeps its count).
[[nodiscard]] Result<DiscreteDivLatticeRoute>
screen_route(double S, double T, std::span<const CashDividend> dividends, int screen_steps);

// The doubles the step maps of `route` occupy (fine + Richardson partner).
[[nodiscard]] std::size_t route_map_doubles(const DiscreteDivLatticeRoute &route) noexcept;

// Builds the step maps of `route` for (T, r, q, dividends) into `arena`
// (>= route_map_doubles(route) long; the maps are views into it). A batch group
// calls this once, in its setup stage. Validated inputs.
[[nodiscard]] RouteMaps build_route_maps(const DiscreteDivLatticeRoute &route, double T, double r,
                                         double q, std::span<const CashDividend> dividends,
                                         std::span<double> arena) noexcept;

// The lowest volatility the inversion on `route` can price (the CRR window of its
// coarsest lattice): a quote at or below the lattice price there is price-only.
[[nodiscard]] double route_sigma_floor(const DiscreteDivLatticeRoute &route, double T, double r,
                                       double q) noexcept;

// Validated inputs (`validate`), steps >= 3, `spec.pre_steps` even and in range.
// With `ProductionSpec{.splice = Linear, .pre_steps = 0, .align_forward = false,
// .align_exercise = false, .bbs = b}` the result is `run_lattice(..., b, ...)` bit
// for bit (tested). Greeks: delta / gamma at t = 0 (pre_steps >= 2) or the
// historical step-1 / step-2 stencils (pre_steps == 0); theta / charm two steps
// forward, as `run_lattice` states.
[[nodiscard]] Result<LatticeOutcome>
run_production_lattice(double S, double K, double T, double sigma, double r, double q, Side side,
                       std::span<const CashDividend> dividends, int steps, ExerciseStyle exercise,
                       const ProductionSpec &spec, LatticeBuffers &buffers);

// Scheme SplicedCubic (american.hpp) on `buffers`: the D1 Standard route at base
// `steps` (kernel TU, section 4), then its production rollback. Validated inputs.
[[nodiscard]] Result<double> spliced_cubic_price(double S, double K, double T, double sigma,
                                                 double r, double q, Side side,
                                                 std::span<const CashDividend> dividends,
                                                 int steps, ExerciseStyle exercise,
                                                 LatticeBuffers &buffers);

// The escrowed lattices; `leisen_reimer` selects the strike-centred tree.
[[nodiscard]] Result<LatticeOutcome>
run_escrowed_lattice(double S, double K, double T, double sigma, double r, double q, Side side,
                     std::span<const CashDividend> dividends, int steps, ExerciseStyle exercise,
                     bool leisen_reimer, LatticeBuffers &buffers);

// The schemes that are ONE rollback (every scheme but SplicedBbsr).
[[nodiscard]] Result<LatticeOutcome>
run_single_rollback(DiscreteDivScheme scheme, double S, double K, double T, double sigma, double r,
                    double q, Side side, std::span<const CashDividend> dividends, int steps,
                    ExerciseStyle exercise, LatticeBuffers &buffers);

// SplicedBbsr: two-point Richardson on the BBS lattice, floored at intrinsic for
// an American; the coarse rollback runs second and reuses `buffers`.
[[nodiscard]] Result<double> richardson_price(double S, double K, double T, double sigma,
                                              double r, double q, Side side,
                                              std::span<const CashDividend> dividends, int steps,
                                              ExerciseStyle exercise, LatticeBuffers &buffers);

} // namespace atx::vol::ddiv_detail
