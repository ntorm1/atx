#pragma once

// Breakeven replay (THEO-2): fixed-sigma delta-hedged single-option P&L as a
// backtest-engine EXTENSION, not a parallel engine.
//
// `bev_replay_pnl` is the public entry point; its implementation is appended
// to the END of `src/backtest.cpp` (not a new .cpp) so it can reuse that TU's
// file-local `HedgeLedger` share ledger and mirror the engine's
// financing/settlement accounting verbatim, without touching a single byte of
// the existing engine code (`run_backtest`, `RunConfig`, `HedgeLedger`'s
// class definition, or the step loop). Later tasks (root-find, batch, loader)
// layer on top of this one function.
//
// Tier-B: reachable by explicit include, deliberately outside the
// `atx/vol/vol.hpp` umbrella.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"                 // Result (used in bev_replay_pnl's return type)
#include "atx/vol/american.hpp"               // AlOpts, al_fast_opts, Side, Result
#include "atx/vol/backtest.hpp"               // Clock, MarketSnapshot (Task 5: path loader)
#include "atx/vol/detail/aggregate_arity.hpp" // BevReplayConfig field-count drift pin
#include "atx/vol/rates_curve.hpp"            // DividendEvent

namespace atx::vol {

// One session close on the replay path: the day's served surface (or a
// synthetic path in tests) collapses to spot + the two rates the American
// pricer needs at that instant. `r`/`q_eff` are the CONTINUOUSLY-COMPOUNDED
// rate and effective carry (borrow + dividend-yield proxy) to expiry as of
// this close, not overnight/short rates.
struct BevDayState {
  std::int64_t ts_ns{0}; // session close timestamp (epoch ns)
  double s{0.0};         // underlying close
  double r{0.0};         // rate to expiry (cont. comp.)
  double q_eff{0.0};     // effective carry to expiry (borrow + div yield proxy)
};

// The single option being replayed: one unit contract, long, of `side` at
// `strike`, expiring at `expiry_ns`.
struct BevSpec {
  double strike{0.0};
  std::int64_t expiry_ns{0};
  Side side{Side::Call};
};

// Replay knobs. DESIGNATED INITIALIZERS ONLY (mirrors `AlOpts`'s construction
// contract, american.hpp ~:51-66): construct as
// `BevReplayConfig{.hedge_band = 0.01, .finance_cash = false}`, never
// positionally — a positional initializer silently rebinds the moment a field
// is inserted rather than appended. `aggregate_arity_is_v` below pins the
// field count at FIVE so an insertion (not an append) turns red at compile
// time instead of quietly rebinding an existing call site.
struct BevReplayConfig {
  std::optional<AlOpts> al_opts{}; // nullopt -> al_fast_opts()
  double hedge_band{0.0};          // |net delta| below which no rebalance
  bool finance_cash{true};         // accrue cash at r*dt (label default ON)
  bool apply_early_exercise{true}; // long-side optimal exercise (B3 semantics)
  double hedge_slippage_bps{0.0};
};

// Drift pin: BevReplayConfig has exactly FIVE fields. Adding, removing, or
// splitting one breaks this line — the point is to force whoever changes the
// struct to read the construction contract above instead of appending a knob
// "for compatibility" with positional initializers that were never a
// supported form here.
static_assert(detail::aggregate_arity_is_v<BevReplayConfig, 5>,
              "BevReplayConfig field count changed: update this pin, and confirm "
              "every construction site still uses designated initializers.");

// Replay outcome for one (path, spec, sigma, dividends, cfg) trial.
struct BevReplayResult {
  double pnl{0.0};         // terminal cash, unit contract, mult=1
  double premium{0.0};     // entry premium at trial sigma
  double vega_entry{0.0};  // entry-day American vega at trial sigma
  std::uint16_t n_days{0}; // sessions actually replayed (early-exit shortens this)
  bool exercised_early{false};
  std::int64_t exercise_ts_ns{0}; // valid iff exercised_early
};

// Fixed-sigma delta-hedged single-option replay: the "breakeven" building
// block a root-find over sigma later turns into an implied breakeven vol.
//
// Semantics: long one unit contract bought at the American price under the
// TRIAL `sigma` on `path[0]`, delta-hedged to zero at every subsequent close
// with the American DELTA AT THE SAME TRIAL sigma (self-consistent hedge —
// convention (a) of the research doc §7.4: the replay never looks at any
// other vol than the one being tested). Cash accrues at `r * dt` between
// closes when `cfg.finance_cash` (default ON here — this label is meant to be
// carry-faithful, even though the engine's own default for the analogous
// knob is OFF). Hedge shares receive/pay each `DividendEvent::amount` on
// ex-dates that fall in `(prev.ts_ns, cur.ts_ns]`. `BevDayState::q_eff` MUST
// EXCLUDE any cash amount already carried in `dividends` — the pricer sees
// `q_eff` as the option's carry input, while `dividends` are discrete cash
// events applied only to the hedge share and the early-exercise threshold;
// double-counting a dividend in both would misprice the option AND
// misattribute its hedge cash flow. Expiry settles the option
// at intrinsic and liquidates the hedge at the final close's spot, exactly as
// the engine's own step loop does for a held-to-expiry lot. `apply_early_exercise`
// (default ON) additionally allows the long side to take the engine's B3-style
// optimal early exercise instead of holding to expiry — a replay-path-only
// extension of the WS-F F3 "expiry is the only exercise event" model (see
// backtest.hpp's EXERCISE MODEL banner); disable it to reproduce the pure
// hold-to-expiry engine convention.
//
// Year fractions use ACT/365.25: `T = (expiry_ns - ts_ns) / (365.25 * 86400e9)`.
//
// Fail-closed (mirrors backtest.hpp:12-15's "crossing expiry without an exact
// observation is not a valid settlement"): `path.back().ts_ns` MUST equal
// `spec.expiry_ns`, or this returns `Err(InvalidArgument, ...)` without
// touching cash. Also `Err(InvalidArgument, ...)` on `path.size() < 2`, a
// `path` longer than the bounded-loop cap (4000 sessions; JPL Rule 2, see the
// implementation), non-positive `sigma`, or non-positive `spec.strike`;
// propagates any American-pricer `Err` (e.g. a degenerate boundary solve)
// unchanged.
//
// @param path       session closes in chronological order; path[0] is entry,
//                    path.back() MUST be the expiry observation.
// @param spec       the option being replayed.
// @param sigma      trial (constant) volatility used for both entry pricing
//                    and every hedge-delta re-solve along the path.
// @param dividends  discrete cash-dividend events on the hedge share, in any
//                    order (scanned linearly per step, mirrors
//                    `DividendSchedule`'s no-particular-order contract).
// @param cfg        replay knobs; default-constructed selects `al_fast_opts()`,
//                    zero hedge band, financing ON, early exercise ON, zero
//                    slippage.
[[nodiscard]] Result<BevReplayResult> bev_replay_pnl(std::span<const BevDayState> path,
                                                     const BevSpec &spec, double sigma,
                                                     std::span<const DividendEvent> dividends,
                                                     const BevReplayConfig &cfg = {});

// THEO-3: breakeven-vol root-find. Layers a bounded bisection over
// `bev_replay_pnl`; the replay itself is untouched by anything below.

// Root-find knobs for `solve_breakeven_vol`. DESIGNATED INITIALIZERS ONLY,
// same construction contract as `BevReplayConfig` above: construct as
// `BevSolveConfig{.sigma_tol = 1e-5}`, never positionally.
// `aggregate_arity_is_v` below pins the field count at FOUR.
struct BevSolveConfig {
  BevReplayConfig replay{}; // forwarded verbatim to every bev_replay_pnl call
  double sigma_lo{0.01};    // lower bracket bound; must be > 0
  double sigma_hi{3.00};    // upper bracket bound; must be > sigma_lo
  double sigma_tol{1e-4};   // bisection stops once the bracket width is below this
};

// Drift pin: BevSolveConfig has exactly FOUR fields (see BevReplayConfig's
// pin above for the rationale).
static_assert(detail::aggregate_arity_is_v<BevSolveConfig, 4>,
              "BevSolveConfig field count changed: update this pin, and confirm "
              "every construction site still uses designated initializers.");

// Outcome flags for `solve_breakeven_vol`. `NoBracket` is DATA, not an error:
// PnL(sigma) can fail to change sign across [sigma_lo, sigma_hi] for a
// legitimately ill-conditioned wing (e.g. a far-OTM strike whose premium
// never crosses the realized payoff inside the bracket), and the batch layer
// that consumes labels filters on this flag rather than treating it as a
// solver defect.
enum class BevFlag : std::uint8_t {
  Ok = 0,             // bisection converged within sigma_tol
  NoBracket = 1,      // pnl(sigma_lo)/pnl(sigma_hi) did not bracket a root
  ExercisedEarly = 2, // converged, and the converged replay exercised early
  MaxIter = 3,        // hit kBevMaxSolveIter without meeting sigma_tol
};

// One breakeven-vol solve outcome.
struct BevLabel {
  double sigma_be{0.0};      // converged (or best-estimate) breakeven vol
  double premium_at_be{0.0}; // entry premium of the converged/best-estimate replay
  double vega_at_be{0.0};    // entry vega of the converged/best-estimate replay
  double pnl_residual{0.0};  // replay PnL at sigma_be
  std::uint16_t n_days{0};   // sessions replayed at sigma_be
  std::uint8_t iters{0};     // bev_replay_pnl evaluations this solve spent
  BevFlag flag{BevFlag::Ok};
};

// Bisection root-find for the breakeven vol: the constant sigma at which
// `bev_replay_pnl`'s hedged P&L is zero. Relies on Task 2's proven
// monotone-decreasing-in-sigma property (`PnlIsMonotoneDecreasingInEntrySigma`)
// rather than re-deriving it.
//
// Algorithm: evaluate PnL at `cfg.sigma_lo` and `cfg.sigma_hi`. Given the
// monotone-decreasing assumption, a genuine bracket needs a STRICT sign
// change, `pnl(sigma_lo) > 0 > pnl(sigma_hi)`. A boundary PnL of exactly 0.0
// does not count as an already-resolved root: for a far-enough-OTM wing at
// `sigma_lo`, the American price/delta can underflow to exactly 0.0, so the
// replay never trades and the resulting 0.0 carries no real vega/gradient
// information. Anything failing the strict bracket is wing ill-conditioning,
// not a solver failure — this returns `Ok(label)` with
// `flag = BevFlag::NoBracket` and the closer-to-zero bracket endpoint as the
// best estimate. Otherwise, standard bisection narrows [sigma_lo, sigma_hi]
// until the bracket width drops below `cfg.sigma_tol`, bounded by
// `kBevMaxSolveIter` (JPL Rule 2, see breakeven.cpp) bisection steps. Each
// step is exactly one more `bev_replay_pnl` evaluation, and `BevLabel::iters`
// counts every evaluation the call makes (the two bracket evals plus one per
// bisection step) — a directly-auditable definition of "how much work did
// this solve cost". If the cap is hit before `sigma_tol` is met, this
// returns `Ok(label)` with `flag = BevFlag::MaxIter` and the narrowest
// estimate reached. On convergence, `flag` is `BevFlag::ExercisedEarly` if
// the converged replay exercised early, else `BevFlag::Ok`; `premium_at_be`,
// `vega_at_be`, `pnl_residual`, and `n_days` all come from that same
// converged (or best-estimate, for NoBracket/MaxIter) replay call.
//
// Validates only its own inputs (`sigma_lo < sigma_hi`, both > 0, and
// `sigma_tol > 0`) and returns `Err(InvalidArgument)` on violation;
// `path`/`spec` validation is delegated to `bev_replay_pnl`, whose errors
// propagate unchanged.
//
// @param path       session closes in chronological order; forwarded as-is
//                    to every `bev_replay_pnl` call (see that function's own
//                    contract for the path/expiry requirements).
// @param spec       the option being replayed.
// @param dividends  discrete cash-dividend events, forwarded unchanged.
// @param cfg        solve knobs; default-constructed brackets [0.01, 3.00]
//                    with a 1e-4 sigma tolerance and default replay config.
[[nodiscard]] Result<BevLabel> solve_breakeven_vol(std::span<const BevDayState> path,
                                                   const BevSpec &spec,
                                                   std::span<const DividendEvent> dividends,
                                                   const BevSolveConfig &cfg = {});

// THEO-4: batch label runner — deterministic parallel fan-out over
// solve_breakeven_vol. Layers a disjoint-write parallel fan-out over Task 3's
// solver; the solver itself is untouched by anything below.

// One breakeven-vol label request: a (path, spec, dividends) triple forwarded
// to solve_breakeven_vol with the batch's shared BevSolveConfig. `path` and
// `dividends` are NON-OWNING spans -- the caller must keep the backing
// storage alive for the duration of the solve_breakeven_batch call that
// consumes this job; BevJob stores no lifetime machinery of its own.
struct BevJob {
  std::span<const BevDayState> path;
  BevSpec spec{};
  std::span<const DividendEvent> dividends;
};

// Structure-of-arrays batch result, index-aligned with the `jobs` span passed
// to solve_breakeven_batch: element i of every vector describes jobs[i]'s
// outcome. `flag` stores `static_cast<std::uint8_t>(BevLabel::flag)` and is
// MEANINGFUL ONLY WHEN `status_ok[i] == 1` -- a rejected job (bad path, etc.)
// writes flag=0 alongside status_ok=0 rather than any BevFlag enumerator
// value, so a reader must check status_ok before interpreting flag (or any
// other field): every numeric field is 0.0/0 for a rejected job.
struct BevLabelFrame {
  std::vector<double> sigma_be, premium_at_be, vega_at_be, pnl_residual;
  std::vector<std::uint16_t> n_days;
  std::vector<std::uint8_t> iters, flag;
  std::vector<std::uint8_t> status_ok; // 1 = solver ran, 0 = input rejected
};

// Deterministic parallel fan-out of solve_breakeven_vol over `jobs`, one
// label per job, `cfg` shared and forwarded verbatim to every solve. Uses
// `parallel_for` (atx/vol/detail/parallel_for.hpp)'s contiguous block
// partition: each worker owns a disjoint [lo, hi) range of job indices and
// writes only its own output slots, so the returned frame is BIT-IDENTICAL
// for any `n_threads` (0 = atx_auto_worker_count(), 1 = serial, byte-for-byte
// with a loop of `jobs.size()` direct solve_breakeven_vol calls).
//
// A per-job failure (an invalid path/spec that bev_replay_pnl or
// solve_breakeven_vol itself rejects) writes `status_ok[j] = 0` and neutral
// (0.0/0) values for every other field at index j; it does NOT sink the
// batch -- every other job's result is unaffected. See BevLabelFrame's
// comment for the flag/status_ok discriminator contract.
//
// `cfg` itself is validated once, up front, exactly as solve_breakeven_vol
// validates its own cfg (sigma_lo/sigma_hi/sigma_tol): an invalid cfg is a
// batch-wide configuration error, not a per-job one, so this returns
// `Err(InvalidArgument)` immediately without touching any job or spawning any
// worker. `jobs` may be empty, which returns `Ok` with every vector empty.
//
// @param jobs      one label request per output slot; spans inside each
//                   BevJob must stay alive for the duration of this call.
// @param cfg       solve knobs, shared by every job; default as
//                   solve_breakeven_vol's default.
// @param n_threads worker count forwarded to parallel_for (0 = auto,
//                   1 = serial).
[[nodiscard]] Result<BevLabelFrame> solve_breakeven_batch(std::span<const BevJob> jobs,
                                                          const BevSolveConfig &cfg = {},
                                                          unsigned n_threads = 0);

// Task 5: surface-corpus path loader — builds a per-session `BevDayState`
// path from stored `PricedSurface` corpora (a `Clock` + its archived
// `MarketSnapshot`s), so a replay/solve can run against REAL market history
// instead of only the synthetic paths THEO-2/3/4 test against. Layers on top
// of the Clock/MarketSnapshot loader (backtest.hpp); it does not touch the
// replay or solver, it only produces their `path` input.

// How the requested option `expiry_ns` is reconciled against the clock's
// actual session observations.
enum class BevExpirySnap : std::uint8_t {
  // FAIL CLOSED unless some session in `clock` observes `expiry_ns` EXACTLY.
  // A path built against an interpolated/nearby session would silently mix a
  // wrong settlement date into the label; refusing is the safe default.
  Exact = 0,
  // Snap down to the LAST session at-or-before `expiry_ns` when no exact
  // observation exists. Sets `BevPath::snapped = true` and
  // `BevPath::settle_ts_ns` to that session's timestamp (strictly less than
  // `expiry_ns`) -- the label consumer decides whether a snapped label is
  // admissible; this loader only reports the fact honestly. When a session
  // DOES land exactly on `expiry_ns`, that session is used and `snapped`
  // stays false (identical outcome to `Exact` in that case -- there was
  // nothing to snap).
  LastSessionAtOrBefore = 1,
};

// A loaded, replay-ready path plus its settlement provenance.
struct BevPath {
  std::vector<BevDayState> days; // ascending ts_ns; days.back().ts_ns == settle_ts_ns
  std::int64_t settle_ts_ns{0};  // == the requested expiry_ns unless snapped
  bool snapped{false};           // true iff settle_ts_ns != the requested expiry_ns
};

// Build a per-session `BevDayState` path for `uid` (an underlying SYMBOL,
// resolved against each loaded session's own SurfaceSet via
// `MarketSnapshot::uid_of`) spanning `entry_ts_ns` through the settlement
// session `snap` resolves `expiry_ns` to.
//
// Per session in [entry_ts_ns, settle_ts_ns]: `s = pricing().S`,
// `r = pricing().r`, `q_eff = q_eff_at(max(t_rem, tenor_probe_years))` where
// `t_rem = (expiry_ns - session_ts_ns) / (365.25 * 86400e9)` (ACT/365.25,
// `kNsPerYear`) is the remaining tenor from THAT session to the REQUESTED
// `expiry_ns` (NOT the possibly-snapped `settle_ts_ns`) -- carry is a
// property of the option's real listed expiry, independent of where the data
// happens to run out; recomputing it fresh per session (rather than once at
// entry) is the "carry errors masquerade as skew" guard. `tenor_probe_years`
// floors `t_rem` near/at expiry (a `kTMinEval`-style guard) so `q_eff_at` is
// never probed at a degenerate/zero tenor.
//
// One `MarketSnapshot::load` per session -- no `SnapshotCache` at this layer;
// a caller driving many loads batches/caches at its own level (e.g. by date).
//
// Fail-closed:
//   * `entry_ts_ns >= expiry_ns`, or `tenor_probe_years <= 0`: `InvalidArgument`.
//   * `entry_ts_ns` is not itself an exact clock observation: `InvalidArgument`
//     (the entry day anchors the replay; snapping it would silently relabel
//     the trade's actual entry date -- only `expiry_ns` is snappable).
//   * `Exact` and no session observes `expiry_ns` exactly: `InvalidArgument`.
//   * `LastSessionAtOrBefore` and no session lies in `[entry_ts_ns,
//     expiry_ns]`: `InvalidArgument`.
//   * a session strictly between entry and settlement whose snapshot has no
//     surface for `uid` (via `uid_of` + `find`): `NotFound`, naming the
//     session's `ts_ns` -- a silent hole in the path would corrupt the label
//     with no trace.
//   * fewer than two sessions assemble (`BevPath::days.size() < 2`): the
//     replay needs at least entry + settlement: `InvalidArgument`.
//   * any `MarketSnapshot::load` failure propagates unchanged.
//
// Postconditions on a successful return: `days` is strictly increasing in
// `ts_ns`, every `s > 0`, `days.size() >= 2`, and `days.back().ts_ns ==
// settle_ts_ns` -- the path is ready to hand to `bev_replay_pnl` /
// `solve_breakeven_vol` with `spec.expiry_ns = settle_ts_ns` (NOT the
// originally requested `expiry_ns` when `snapped`).
//
// @param clock             the corpus timeline to walk, ascending.
// @param uid                underlying symbol to resolve per session.
// @param entry_ts_ns        the replay's first session; must be an exact
//                            observation.
// @param expiry_ns          the option's real listed expiry (also the carry
//                            anchor for every session's `q_eff`).
// @param tenor_probe_years  floor applied to each session's remaining tenor
//                            before probing `q_eff_at` (must be > 0).
// @param snap                how to reconcile `expiry_ns` against the
//                            clock's actual sessions; defaults to the
//                            fail-closed `Exact`.
[[nodiscard]] Result<BevPath> load_bev_path(const Clock &clock, std::string_view uid,
                                            std::int64_t entry_ts_ns, std::int64_t expiry_ns,
                                            double tenor_probe_years,
                                            BevExpirySnap snap = BevExpirySnap::Exact);

} // namespace atx::vol
