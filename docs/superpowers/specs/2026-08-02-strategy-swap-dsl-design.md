# Declarative swap-lane strategies — design

**Goal.** Retire the bespoke `StrangleVsVarswapStrategy` (strangle_varswap.{hpp,cpp}, 880 lines + 600-line driver) by extending the existing declarative strategy DSL (`StrategySpec` → `DeclarativeStrategy`) so the XOM strangle-vs-varswap comparison — and any strategy of its shape — is expressible as data. The comparison backtest and its cumulative-P&L plot must be reproducible from a ~50-line example.

**Approved decisions** (user, 2026-08-02): extend the DSL (no parallel builder layer); track-parity gate on the XOM 2026 run before deleting the old files; deliverable is a compact example binary.

## What already exists (unchanged)

- `PortfolioState` carries both lanes (`lots`, `swap_lots`); the engine marks, accrues (`SwapAccrual`), and settles swaps itself; swap lots are append-only and held to expiry. No portfolio or pricer changes are needed.
- `StrategySpec` grammar covers option legs (tenor/strike/structure/size), cross-leg constraints, lifecycles HoldToExpiry / RollAtHorizon / CloseAtHorizon, hedge overlay, `session_ts`.
- `run_timed` → `TearSheet` + `write_backtest_tsv` is the run spine the example uses.

## Gap being closed

1. No **fixed-expiry, daily-restrike** lifecycle (the comparison's options leg).
2. No **swap leg** in the grammar.
3. No **size-swap-to-group-entry-vega** sizing.
4. Swap-greek **signals** (engine-accrual mirror) are bespoke to the old strategy.

## 1. Grammar additions (strategy.hpp)

### `LifecycleSpec::Holding::FixedExpiryRestrike`

A CYCLE fixes ONE expiry and holds it; steps inside the cycle close the option
lots and reopen them at freshly resolved strikes at the SAME expiry; the engine
settles at expiry and the next cycle is fixed on that same step.

- **Cycle expiry selection** (ceil-snap, verbatim from the old strategy): the
  first session in `spec.session_ts` at or after `base_ts + tenor`, or the LAST
  session when the grid ends before the anchor (a short final cycle the run
  still observes settling). No session strictly after `base_ts` ⇒ no cycle:
  the step is a no-op and the run winds down.
- **Cycle tenor comes from the option legs**: all option legs must carry the
  IDENTICAL `tenor.target_T` (finite, > 0) with `snap_to_listed == false` and
  `snap_to_sessions == false` — the lifecycle owns expiry snapping in this
  mode, and a leg requesting its own snap is `InvalidArgument`, never silently
  ignored. That common `target_T` is the cycle tenor. No new lifecycle field.
- **Restrike cadence** honors `LifecycleSpec::Entry`: `EveryStep` (the old
  behavior, and the example's) restrikes every session; `EveryNDays` restrikes
  only on entry ticks and holds the book otherwise. Cycle open/settle timing is
  unaffected by cadence — expiry comes off the calendar.
- **Keep-strikes policy** (verbatim): a step whose strike resolution fails
  soft (symbol/surface missing, delta unreachable, unpriceable wing) KEEPS the
  live lots and counts `skipped_restrikes`; the same failure with an empty book
  counts `unopened_strangle_steps` (nothing was held). Never a fabricated
  strike, never a 0.0 mark. Configuration errors (`InvalidArgument`) stay
  fatal.
- `Lot::cohort` counts CYCLES, not clips. `spec.session_ts` must be non-empty
  and sorted (validated up front, first `on_step`).

### `SwapLegSpec` + `SwapSizeSpec`

```cpp
struct SwapSizeSpec {
  enum class Kind : std::uint8_t {
    FixedQty = 0,       // qty = value (signed)
    TargetVega = 1,     // qty = sign * value / |swap entry vega|
    MatchGroupVega = 2, // qty = (group options entry dollar vega) / (swap entry vega)
  };
  Kind kind{Kind::MatchGroupVega};
  double value{0.0};
  double sign{+1.0};  // TargetVega only
  std::string group;  // MatchGroupVega: option-leg group to match; empty = ALL option legs
};

struct SwapLegSpec {
  std::string symbol;          // resolved against the snapshot, like LegSpec
  std::uint32_t uid{0};        // preferred if nonzero
  DerivKind kind{DerivKind::VarSwap};
  double cap_dec{0.0};         // > 0 required on capped kinds, must be 0 otherwise
  double notional{1.0};        // sizing rides on qty; notional stays a readable constant
  double annualization{252.0};
  SwapSizeSpec size{};
  DerivConfig deriv_cfg{};     // ENTRY SOLVE ONLY (fair strike + vega); the engine
                               // marks under its own default config — the old
                               // strangle_varswap.hpp caveat carries over verbatim
  std::string group;           // diagnostic tag
};
```

`StrategySpec` gains `std::vector<SwapLegSpec> swap_legs;` (default empty ⇒
behavior and cost bit-identical to today — the additive-lane rule).

**Semantics** (verbatim from `open_cycle_swap`):

- Swap legs open ONCE per cycle, on the cycle-open step, struck at their own
  fair strike (via `deriv_price_on_ref`, the same bridge the engine's mark lane
  prices through — the only construction that opens at genuine zero PV) and
  never restruck (the lane is append-only, held to expiry).
- `n_obs_total` = count of sessions in `(open, expiry]` minus 1 (the first
  session after open seeds). A cycle observing < 1 return runs options-only.
- `MatchGroupVega`: the option group's entry DOLLAR vega — per-share American
  vega × qty × multiplier, summed over the group's freshly opened lots; the
  ratio to the swap's own entry vega is the qty, sign carried (long-vega
  options ⇒ long swap).
- **Fail-soft**: every solve failure (no board, non-finite/zero vega, short
  cycle, failed fair strike, non-finite qty) leaves the book and the id
  watermark untouched and counts one `skipped_swaps` for the cycle. A cycle
  whose OPTIONS failed to resolve also counts its swap legs skipped (no entry
  vega to size against; a per-cycle instrument is never retro-opened).
- **v1 scope**: `swap_legs` non-empty requires `Holding::FixedExpiryRestrike`
  (`NotImplemented` otherwise — a swap cannot be erased, so RollAtHorizon
  cannot carry one, and HoldToExpiry cohort swaps are YAGNI until needed).

## 2. `swap_leg.hpp` / `swap_leg.cpp` (new module)

The reusable swap-leg toolkit, everything moved (not rewritten) out of
strangle_varswap.cpp:

- `swap_contract_for_lot(lot, base_ts, rv) -> DerivContract` — the engine's
  `SwapLot` → `DerivContract` transcription (residual tenor + staged rv), ONE
  home shared by the entry solve and the signal probe.
- `solve_cycle_swap(surface, spec-leg fields, expiry, session span, options_vega)
  -> Result<SwapLot>` — fixing-count, fair-strike, entry-greeks, qty solve;
  every skip cause returns `Err(Unavailable, <cause>)` so callers count and
  report rather than guess.
- `class SwapSignalProbe` — the engine-accrual mirror, semantics verbatim
  (seed on first sight one step late; desync on checkpoint-restored lots and
  on untakeable fixings; fixing per step incl. expiry; frozen when fully
  observed):
  - `capture_pre_step(book)` — swap-lot ids before the strategy mutates;
  - `refresh(base, book)` — after a successful step: drop settled mirrors,
    adopt new lots, take fixings, stamp the as-of snapshot;
  - `swap_greek_signals(base)` — `swap_delta/gamma/vega/theta/rho`, qty-scaled
    sums recomputed on the given snapshot under the engine's mark config;
    NaN (never 0.0) when no swap is live, any lot unpriceable, any mirror
    desynced, or `base` is not the as-of snapshot. Computed only when called —
    the engine calls `signals` on recorded rows only, so `record_every_n > 1`
    pays for finite differences only on kept rows.

Any `IStrategy` can own a probe; `DeclarativeStrategy` wires one automatically
when `swap_legs` is non-empty.

## 3. Interpreter changes (strategy.cpp)

`DeclarativeStrategy` implements the new mode; existing modes' code paths are
untouched (dispersion golden and existing example outputs bit-identical).

Per-step flow for `FixedExpiryRestrike`:

1. Validate once (first `on_step`, `Status` channel — sorted non-empty
   `session_ts`, common leg tenor, swap-leg field rules, mode restrictions).
2. Cycle roll: if `cycle_expiry <= base_ts`, select the next expiry (no
   session left ⇒ `Ok`, run winds down); `++cohort`; mark cycle-open.
3. Resolve option legs pinned to the cycle expiry: exact `expiry_ts_ns`
   anchor, `T` re-derived from it, strikes via the existing
   `resolve_strike`/`expand_leg` machinery, sizing via existing `SizeSpec`.
   Soft failure ⇒ keep-strikes / unopened counters (+ swap skips if
   cycle-open); return `Ok`.
4. Restrike: clear the option lots, append the fresh pair (cohort = cycle,
   expiry = cycle expiry, entry at model mark, seeds handed to the engine).
5. Cycle-open only: `solve_cycle_swap` per swap leg; append or count skip.
6. `on_step` wrapper: probe `capture_pre_step` before, `refresh` after a
   successful step (error ⇒ no refresh; the run aborts).

`DeclarativeStrategy::signals(base)` (new override, emitted ONLY when
`swap_legs` non-empty — existing specs keep their empty default): the probe's 5
greek columns + `options_vega` (the option book's entry dollar vega as of this
row's restrike, NaN on unresolved/no-cycle steps — the old `strangle_vega`
under its lane-agnostic name) + cumulative `skipped_restrikes`, `skipped_swaps`.
Counter accessors (`skipped_restrikes()`, `unopened_strangle_steps()`,
`skipped_swap_cycles()`) exposed on the strategy for drivers and tests.

## 4. Parity gate (before any deletion)

Fixture-gated gtest (skips when `C:/atx-data/surface-db/scratch-fitfix-2026`
is absent, the existing local-fixture pattern): run `StrangleVsVarswapStrategy`
and the equivalent `StrategySpec` on the XOM 2026 db, assert

- track columns (`nav`, `pnl_total`, `swap_pnl`, `swap_pv`) equal within 1e-9
  relative per row;
- identical skip counters and identical lot-id sequences.

The equivalent spec: one 0.25-tenor strangle leg, Delta 0.40/0.40,
FixedContracts 100 long, EveryStep + FixedExpiryRestrike, DeltaToZero daily
hedge, one `VarSwap` swap leg sized `MatchGroupVega`, `session_ts` from the
run's clock.

## 5. Example (deliverable)

`examples/varswap_compare_example.cpp` (~50–60 lines): constants/argv for db
root, symbol, delta, tenor, contracts → build the spec → `run_timed` →
`write_backtest_tsv`. The existing plot script
(`plot_fitfix.py`) renders the cumulative-P&L png from that TSV unchanged.
Replaces `strangle_varswap_driver.cpp`.

## 6. Deletions (after parity green)

- `include/atx/vol/strangle_varswap.hpp`, `src/strangle_varswap.cpp`,
  `examples/strangle_varswap_driver.cpp` (CMake entries with them).
- Old strategy's tests: behavioral coverage ported to declarative-spec
  equivalents (cycle selection, keep-strikes counters, swap sizing/skip
  causes, mirror desync, signals NaN discipline); the parity test itself
  outlives the old class only as history — it is deleted WITH the class in the
  same commit, its assertions having been promoted into the ported tests.

## Error handling

Existing codebase policy throughout: configuration errors are
`InvalidArgument` and fatal on first step; data-driven failures are soft,
counted, and never silent; skips are reported in signals and accessors; NaN
means "not measured", 0.0 means "measured zero".

## Performance constraints

- Empty `swap_legs` + existing lifecycles: zero added work (early-outs), no
  allocation changes on the hot path.
- Restrike mode allocates per step only what the old strategy did (lot vector
  rebuild, seed vector move); probe buffers are members reused across steps.
- Swap greeks are computed only on recorded rows (unchanged engine contract).

## Testing

TDD throughout (failing test first, per component). Unit tests on the
synthetic eSSVI corpus pattern (`strategy_examples.cpp` / existing backtest
tests); parity gate per §4; targeted gtest filters only (no full-suite runs,
standing instruction).

## Non-goals

- Frictions/cash ledger on the swap lane, early swap unwind, capped-kind
  examples, HoldToExpiry cohort swaps, listed-contract restrikes,
  `referenced_uids` for declarative specs, plot-script changes.
