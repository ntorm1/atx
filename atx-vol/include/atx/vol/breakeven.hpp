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

#include "atx/vol/american.hpp"    // AlOpts, al_fast_opts, Side, Result
#include "atx/vol/detail/aggregate_arity.hpp" // BevReplayConfig field-count drift pin
#include "atx/vol/rates_curve.hpp" // DividendEvent

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
  double pnl{0.0};        // terminal cash, unit contract, mult=1
  double premium{0.0};    // entry premium at trial sigma
  double vega_entry{0.0}; // entry-day American vega at trial sigma
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
// ex-dates that fall in `(prev.ts_ns, cur.ts_ns]`. Expiry settles the option
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
[[nodiscard]] Result<BevReplayResult> bev_replay_pnl(
    std::span<const BevDayState> path, const BevSpec &spec, double sigma,
    std::span<const DividendEvent> dividends, const BevReplayConfig &cfg = {});

} // namespace atx::vol
