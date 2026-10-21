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

#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"       // Result, Status, Side, ExerciseStyle
#include "atx/vol/api/pricing/american.hpp" // CashDividend, DiscreteDivScheme

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
