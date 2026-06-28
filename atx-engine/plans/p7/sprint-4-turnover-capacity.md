# Sprint 4 — Turnover & Capacity Realism

**Goal:** turn a positive gross edge into a tradeable net one by (1) wiring
the existing turnover-penalty mechanism into a sensible tradeable-profile
default, (2) building an opt-in stateful EMA-decay `WeightPolicy` so deployed
rebalance churn drops, (3) replacing the constant-1.0 per-name capacity vector
in `decorrelate_weights` with a real per-alpha capacity AUM from
`cost/capacity.hpp`, and (4) promoting the AUM→net-edge curve to a first-class
report output.

**Owns (exclusive):** `atx-engine/include/atx/engine/loop/weight_policy.hpp`,
`atx-engine/include/atx/engine/combine/combiner.hpp`,
`atx-engine/include/atx/engine/cost/capacity.hpp`,
`atx-engine/include/atx/engine/risk/capacity.hpp`,
plus the test files that exercise them. Do NOT touch
`atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}` (S7
owns the CLI hub), `atx-engine/include/atx/engine/factory/fitness.hpp` /
`src/factory/fitness.cpp` (S1 owns the gate/fitness deflation path), or
`atx-engine/include/atx/engine/combine/conviction.hpp` /
`risk/kelly_sizing.hpp` (S5 owns conviction + sizing). `oracle.hpp` is
untouchable by every sprint.

---

## Implementation-quality handoff block

Paste into every coding sub-agent brief verbatim:

```text
Implementation quality standard:
Use ats-core/include/ats_orderbook.h as the style reference. Prefer clear
module-level intent, grouped constants/types/APIs, explicit ownership and
lifecycle rules, named error contracts, and concise comments that explain
invariants, non-obvious control flow, or domain semantics. Do not follow
weaker patterns that expose constants/structs/prototypes without enough API
contract.

Prioritize full end-to-end implementation over partial stubs. A unit is not
done until the public API, implementation, tests, docs/ledger row, and
build/test gate are complete. Do not leave TODO placeholders, fake success
paths, unused APIs, or untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership,
ordering, crash/recovery semantics, and tricky domain rules. Do not comment
obvious assignments or wrap every field in noise.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and
  lifecycle.
- Names are domain-accurate and consistent with nearby ATS code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new
  abstractions.
```

---

## Determinism contract (inherited from p6, mandatory for every unit)

Every output-changing capability sits behind an engine-config field (or a new
`WeightPolicy` / `CombinerConfig` field) defaulting to today's value. The
no-flag path is byte-identical (`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
`FactoryOos.MineIntoOffPathDigestUnchanged`, OOS goldens; `oracle.hpp`
untouched). Each opt-in ships all four test classes:

- **(a) Off-path byte-identity** — default field values → book/digest
  byte-identical to pre-S4.
- **(b) On-path RED→GREEN** — the new behavior is absent before the commit,
  present and tested after.
- **(c) Twice-run stability** — same output on two consecutive runs.
- **(d) Seq==parallel** — wherever an admission or combine path is touched,
  the parallel run matches the sequential run byte-for-byte.

No hour-long production run is a gate. No golden re-baseline.

---

## Context

### What p6-S3 shipped (the turnover-penalty mechanism)

`FitnessCfg` (`atx-engine/include/atx/engine/factory/fitness.hpp:352-361`) has
two inert-by-default fields:

```cpp
atx::f64 turnover_penalty_slope = 0.0;        // line 358
atx::f64 max_turnover_target =                // line 359-360
    std::numeric_limits<atx::f64>::infinity();
```

The penalty formula in `src/factory/fitness.cpp:391-434`:
```
excess = max(0, turnover - max_turnover_target)
slack  = max(max_turnover_target * slope, kPenaltyEps)
mult   = clamp(1 - excess/slack, kFloor, 1.0)
raw   *= mult
```
enters only when `turnover_penalty_slope > 0.0` (`fitness.cpp:420`); with the
default `slope=0.0` the branch is never entered and the result is
byte-identical to pre-S3. The CLI knobs `--turnover-penalty-slope` and
`--max-turnover-target` are threaded through
`atx-impl/src/config.hpp:136-137` and wired in
`atx-impl/src/stage_discover.cpp:873-874`. The mechanism works; it just has
no sensible default that pushes search toward low-turnover alphas, and the
gate's `max_turnover` is not fed back to the fitness target.

### What the WeightPolicy is today

`loop/weight_policy.hpp` (`atx-engine/include/atx/engine/loop/weight_policy.hpp`)
is a **pure stateless struct**. The `to_target_weights` pipeline is:
winsorize → transform → [group-demean] → dollar-neutralize → [truncate-renorm]
→ gross-normalize. DECAY is a documented deferred residual (header comment
lines 95-100):

> DEFERRED P4-8 stages (documented residuals, NOT implemented here — see the
> 4b ledger): DECAY (a stateless const WeightPolicy holds no signal history,
> so a d-window temporal decay needs a signal-history input / a stateful policy
> — an architectural change) ...

`kTruncateIters = 8` (line 534) is load-bearing for determinism. The
`to_target_weights` method does NOT call `FactorModel::neutralize`
(documented in the header residuals, lines 95-100 — the wiring gap is not
in scope for S4; S5 owns the risk include chain).

### What the combiner's capacity vector is today

In `atx-impl/src/stage_combine.cpp`, the `decorrelate_weights` call site
(lines 562-629) already supports a per-name capacity vector. With
`cfg.capacity_floor <= 0.0` **or** `cfg.target_aum <= 0.0` the vector is
filled with the constant-1.0 stub (`stage_combine.cpp:589`):

```cpp
std::vector<atx::f64> capacity(pool.size(), 1.0);   // line 589 — the stub
if (cfg.capacity_floor > 0.0 && cfg.target_aum > 0.0) {
    // ... per-alpha capacity_aum computation ... (T6)
}
```

When `--capacity-floor` is absent (the default), `cfg.capacity_floor == 0.0`
and the outer guard `cfg.corr_penalty > 0.0 || cfg.capacity_floor > 0.0`
(line 562) is false if `corr_penalty` is also zero, so `decorrelate_weights`
is not called at all — the constant-1.0 is never even consumed. The ROADMAP
gap (`--capacity-floor is a 1.0 no-op stub`) means that even when a user
passes `--capacity-floor <F>` without `--target-aum`, the per-alpha AUM
computation is skipped and every alpha gets `cap_scale = clamp(1.0 / F, 0, 1)`,
which is 1 for any `F <= 1.0` — the stub is confirmed.

S4 closes this gap at the **engine header layer** (`cost/capacity.hpp`,
`risk/capacity.hpp`) so the real per-alpha capacity vector is computable from
a `(weights, panel, sim, aum_grid)` tuple. The driver-layer wiring (feeding
the vector into `stage_combine`) is S7's threading job; S4 proves the capacity
math is correct on tiny fixtures.

### What the capacity curve infrastructure is today

`risk/capacity.hpp` (`atx-engine/include/atx/engine/risk/capacity.hpp`)
implements `capacity_curve` (line 264), which sweeps an AUM grid and returns
`vector<CapacityPoint>{aum, net_edge_bps}`. `cost/capacity.hpp`
(`atx-engine/include/atx/engine/cost/capacity.hpp`) wraps it with
`capacity_for_book` (line 30) and `capacity_for_alpha` (line 41).
`capacity_point` (line 63) finds the zero-crossing. These are correct but
orphaned — no report output emits the curve.

---

## Wiring map

```
loop/weight_policy.hpp          <-- S4: add EmaDecayPolicy (opt-in stateful sibling)
    |
    v
combine/combiner.hpp            <-- S4: add capacity_scale_weights helper
    |                                   (per-name scalar pre-combiner blend)
    v
cost/capacity.hpp               <-- S4: expose compute_capacity_vector
    risk/capacity.hpp               (per-alpha capacity AUM → vector<f64>)
    |
    v
report output (capacity curve)  <-- S4: capacity_scorecard struct + emit helper
                                    (first-class scorecard; driver wiring = S7)
```

S7 owns `stage_discover.cpp` and `config.{hpp,cpp}` — threading the new
knobs through the CLI and validating on the dev panel are S7 tasks. S4 proves
every new knob correct on tiny deterministic fixtures and leaves S7 a clean,
self-contained API surface.

---

## Tasks

### S4-0 — Open ledger (marker commit)

**Goal:** create the sprint ledger
`atx-engine/plans/p7/phase-s4-progress.md`, record the base SHA, list the
4 units (S4-1..S4-4) with their one-line themes, and commit as:

```
docs(p7-s4-0): open sprint-4 turnover-capacity ledger
```

No source changes. No test changes. Ledger structure per
`docs/sprint.md:65-116`.

**Wiring (file:line):** `atx-engine/plans/p7/phase-s4-progress.md` (new file
only).

**Determinism:** no source touched; all digests unchanged.

**Accept:** ledger file exists, marker commit landed, base SHA recorded,
4 unit rows in the per-unit ledger table with status `pending`.

---

### S4-1 — Stateful EMA-decay WeightPolicy

**Goal:** add an opt-in `EmaDecayPolicy` struct alongside the existing
stateless `WeightPolicy` in `loop/weight_policy.hpp` that smooths raw
target weights toward the previous-period weights via an EMA, reducing
rebalance churn without touching the stateless default path.

#### Mechanism

The decay rule (per-step, per-name):

```
smoothed[i] = alpha * raw_target[i] + (1 - alpha) * prev_smoothed[i]
```

where `alpha ∈ (0, 1]` is the `ema_alpha` knob. `alpha=1.0` is the
pass-through identity (stateless WeightPolicy behaviour). The smoothed weights
are then gross-renormalized so `Σ|w|` still equals `gross_leverage` (the same
`gross_normalize` helper already in `WeightPolicy`). Dollar-neutrality may
drift slightly after the EMA blend; apply a final demean before renorm when
`dollar_neutral=true`.

Iteration order: ascending universe index (determinism). Smoothing uses the
PREVIOUS call's smoothed output, stored as `std::vector<f64> prev_weights_`
(mutable state, universe-sized). On the FIRST call (cold start, or after
`reset()`), `prev_smoothed[i] = raw_target[i]` — no prior information, so the
first call is byte-identical to the stateless path.

#### Design

`EmaDecayPolicy` is a **separate struct** (not a modification of
`WeightPolicy`) because `WeightPolicy` is documented "pure configuration, holds
no mutable state" and the caller-provided-scratch overload contract assumes
the struct is `const`. Introducing mutable state into `WeightPolicy` would
violate the header's documented invariant and break every caller that holds it
as `const`. The new struct holds:

```cpp
struct EmaDecayPolicy {
    WeightPolicy base;    // the stateless inner pipeline (delegated to)
    atx::f64 ema_alpha = 1.0;  // 1.0 = identity / inert (pass-through)
    void reset();              // clear prev_weights_ (cold start)
    // main entry — same signature as WeightPolicy::to_target_weights
    std::vector<atx::f64>
    to_target_weights(SignalView signal, const Universe& universe,
                      std::span<const atx::u32> group_map = {});
private:
    std::vector<atx::f64> prev_weights_; // mutable decay state
};
```

`EmaDecayPolicy` with `ema_alpha=1.0` is BYTE-IDENTICAL to calling
`base.to_target_weights` directly (the off-path identity): when alpha=1,
`smoothed[i] = raw[i]`, renorm reproduces the stateless path, and
`prev_weights_` is never read by the caller — full off-path byte-identity.

#### Wiring (file:line)

- `loop/weight_policy.hpp` — append `EmaDecayPolicy` struct after line 535
  (after the closing `}` of `WeightPolicy`). The header already includes
  everything needed (`<vector>`, `<span>`, the cross-section helpers).
- `atx-engine/tests/loop/weight_policy_decay_test.cpp` — new test file
  (auto-globbed by CMake).

#### Determinism (inert default)

`ema_alpha=1.0` → first call smoothed=raw, renorm=gross_normalize(raw),
result byte-identical to `WeightPolicy::to_target_weights`. `kTruncateIters=8`
in the base policy is preserved (load-bearing, not touched). The `WeightPolicy`
struct itself is NOT modified.

#### Accept

All four test classes must pass:

- **(a) Off-path byte-identity:** `EmaDecayPolicy{base, /*ema_alpha=*/1.0}`
  produces the same weights as `base.to_target_weights` on the same signal.
  Assert byte-exact equality over 10 rounds of a hand-built 8-name signal
  series.
- **(b) On-path RED→GREEN — decay reduces measured turnover:**
  Build a 20-period, 6-name signal series where names swap rank every other
  period (maximum raw turnover). Compute turnover without decay (alpha=1.0)
  and with decay (alpha=0.3). Assert `turnover_with_decay < turnover_no_decay`
  by a non-trivial margin (> 5 percentage points). Turnover = mean over
  periods of `0.5 * Σ|w_t[i] - w_{t-1}[i]|`.
- **(c) Twice-run:** feed the same 20-period series twice; assert identical
  output vectors on both runs.
- **(d) reset() restores cold-start:** after 10 periods, call `reset()`,
  re-run period 0 — output equals the first-call output from the cold start.

Commit: `feat(p7-s4-1): add EmaDecayPolicy with ema_alpha=1.0 inert default`

---

### S4-2 — Real per-name capacity vector (kill the 1.0 stub)

**Goal:** expose a `compute_capacity_vector` helper in `cost/capacity.hpp`
that computes a per-alpha capacity AUM vector from an `AlphaStreams` object,
a `PanelView`, and an `ExecutionSimulator`, using the existing
`capacity_for_alpha` / `risk::capacity_curve` infrastructure. This makes the
real capacity vector available so `decorrelate_weights` can use it instead of
the constant-1.0 stub.

#### The confirmed stub

`atx-impl/src/stage_combine.cpp:589`:
```cpp
std::vector<atx::f64> capacity(pool.size(), 1.0);   // constant-1.0 stub
if (cfg.capacity_floor > 0.0 && cfg.target_aum > 0.0) {
    // ... per-alpha computation ... (T6, only entered when BOTH flags set)
}
```
The gap: even when `--capacity-floor` is set but `--target-aum` is absent
(or vice versa), the stub is consumed unchanged. S4 fixes the engine-layer
helper so the vector is correctly computable; driver-layer wiring is S7.

#### Mechanism

```cpp
// cost/capacity.hpp — new helper
[[nodiscard]] inline std::vector<atx::f64>
compute_capacity_vector(
    const alpha::AlphaStreams& streams,
    const PanelView& panel,
    const exec::ExecutionSimulator& sim,
    atx::f64 target_aum);
```

For each alpha `a` in `[0, streams.n_alphas())`, ascending:
1. Compute `capacity_for_alpha(streams, a, panel, sim, aum_grid)` where
   `aum_grid` is a log-spaced grid of 20 points from `0.01 * target_aum` to
   `10.0 * target_aum` (wide enough to bracket the zero-crossing in typical
   cases).
2. Call `capacity_point(curve)` to get the capacity AUM (the zero-crossing).
3. Store the capacity AUM in `out[a]`.

A capacity AUM of `+inf` (no crossing on the grid) is stored as-is; the
caller's `cap_scale = clamp(capacity_aum / capacity_floor, 0, 1)` naturally
clamps infinity to 1.0 (no penalty), preserving the existing contract.
A capacity AUM of 0 (gross edge ≤ 0 for the last-period book, per
`cost::capacity_point`) means the name's book has no positive frictionless
edge and `cap_scale` → 0 — correctly fading it out.

The AUM grid is a named constant array (`kCapacityAumGridPoints = 20`,
computed at call time from `target_aum`) so the log-spacing is deterministic
and the grid is reproducible from the same `target_aum`.

#### Why `capacity_for_alpha` uses the last-period weights

`cost::capacity_for_alpha` (`cost/capacity.hpp:41-48`) takes
`streams.positions(alpha_idx, streams.n_periods() - 1U)` — the LAST-period
target weights — as the book proxy. This matches the existing driver-layer
T6 path (`stage_combine.cpp:573`). S4 does not change this; the p6-S6-1
note (realized-edge vs frozen-snapshot edge) is a separate concern owned by
the driver. The engine-layer helper is correct for its documented input.

#### Wiring (file:line)

- `cost/capacity.hpp` — add `compute_capacity_vector` after `capacity_point`
  (after line 91). The helper is header-only inline (cold path, research
  cadence; the existing helpers in this file are also inline).
- `atx-engine/tests/cost/capacity_vector_test.cpp` — new test file.

#### Determinism (inert default)

`compute_capacity_vector` is a NEW helper; no existing call site is modified.
The constant-1.0 stub in `stage_combine.cpp` is unchanged (S7 replaces it
with a call to this helper). All existing digests are byte-identical. The new
helper is deterministic (ascending alpha order, fixed 20-point AUM grid,
`risk::capacity_curve` is already documented NO RNG / bit-deterministic at
`risk/capacity.hpp:85-88`).

#### Accept

All four test classes:

- **(a) Off-path byte-identity:** `stage_combine.cpp` is NOT touched in this
  unit. The existing golden digests (`FactoryOos.MineIntoOffPathDigestUnchanged`,
  `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`) remain byte-identical
  (reviewer gate: diff of `stage_combine.cpp` is empty).
- **(b) On-path RED→GREEN — capacity vector ≠ 1.0 and bounds a high-participation
  name:**
  Build a synthetic `AlphaStreams` with 4 alphas. Alpha 0 has uniform weights
  over 50 names (modest participation). Alpha 1 has 90% of its weight
  concentrated on one name with tiny ADV (very high participation, low capacity
  AUM). Call `compute_capacity_vector` with `target_aum = $100M`.
  Assert: `capacity_vec[0] > capacity_vec[1]` (alpha 1 is more capacity-
  constrained). Assert: `capacity_vec[1] < target_aum` (the high-participation
  alpha's capacity crosses zero below the target AUM — it IS capacity-
  constrained at $100M).
- **(c) Twice-run:** same inputs → bit-identical output.
- **(d) Seq==parallel:** `compute_capacity_vector` is a pure function of its
  inputs with no shared state; the unit test verifies this by running it from
  two threads with the same input and asserting identical outputs.

Commit: `feat(p7-s4-2): compute_capacity_vector — per-alpha capacity AUM from last-period book`

---

### S4-3 — Capacity-curve scorecard output

**Goal:** promote the AUM→net-edge curve to a first-class output by adding
a `CapacityScorecard` struct in `cost/capacity.hpp` that packages the curve
(the `vector<CapacityPoint>`) together with the capacity-point AUM and a few
derived summary stats, and an `emit_capacity_scorecard` function that builds it
from a combined-book weight vector.

This makes the scorecard a self-contained, testable artefact that the driver
(S7) can wire to the report KV block. S4 does NOT modify any report KV
emission (driver territory).

#### Struct design

```cpp
// cost/capacity.hpp — after compute_capacity_vector

/// Capacity scorecard for a fitted book at a given target AUM.
///
/// capacity_point_aum : the AUM where net edge → 0 (linear interpolation
///                      over the aum_grid), or +inf if the grid does not
///                      bracket the crossing. See cost::capacity_point.
/// gross_edge_bps     : the AUM-independent frictionless edge (gross) in bps.
/// net_edge_at_target : net_edge_bps at target_aum (the operational AUM).
/// curve              : the full (aum, net_edge_bps) series in grid order.
///                      Monotone non-increasing (the capacity model
///                      CONTRACT; verified by capacity_point's C4 guard).
struct CapacityScorecard {
    atx::f64 capacity_point_aum;   // AUM at which net edge = 0
    atx::f64 gross_edge_bps;       // frictionless edge (AUM-independent)
    atx::f64 net_edge_at_target;   // net edge at the target_aum grid point
    std::vector<risk::CapacityPoint> curve; // the full sweep
};

/// Build a CapacityScorecard for a combined book weight vector.
///
/// aum_grid must be non-empty and ascending; capacity_point uses it for
/// the zero-crossing interpolation.  A short panel degenerates gracefully
/// (windows clamp; no OOB/div-by-zero — see risk::capacity_curve header).
/// Pure: same (weights, panel, sim, aum_grid) -> bit-identical scorecard.
[[nodiscard]] inline CapacityScorecard
emit_capacity_scorecard(std::span<const atx::f64> weights,
                        const PanelView& panel,
                        const exec::ExecutionSimulator& sim,
                        std::span<const atx::f64> aum_grid,
                        atx::f64 target_aum);
```

The `net_edge_at_target` field is the `CapacityPoint.net_edge_bps` for the
grid entry whose AUM is closest to `target_aum` (or the last point if
`target_aum` exceeds the grid). The gross edge is extracted from the first
call to `risk::detail::gross_edge_bps` (AUM-independent; computed once inside
`capacity_for_book` → `risk::capacity_curve`). To avoid re-implementing the
gross edge computation, `emit_capacity_scorecard` delegates entirely to
`capacity_for_book` and derives `gross_edge_bps` from the curve:
`gross_edge_bps = curve[0].net_edge_bps + (cost at aum_grid[0])`. However,
re-extracting the cost reverses the architecture — instead, expose the gross
edge directly as a detail helper read-out. The cleaner approach: call
`risk::detail::gross_edge_bps(weights, panel, w_edge)` directly (it is
already a `[[nodiscard]] inline` in the `detail` namespace of
`risk/capacity.hpp:183`). Since `cost/capacity.hpp` already includes
`risk/capacity.hpp` (line 21), the detail function is reachable.

#### Monotonicity and the scorecard contract

The scorecard `curve` is the output of `risk::capacity_curve`, which the
`capacity_point` C4 guard asserts is non-increasing (verified at
`cost/capacity.hpp:70`). `emit_capacity_scorecard` documents this invariant so
callers can safely plot or interpolate without a re-check.

#### Wiring (file:line)

- `cost/capacity.hpp` — add `CapacityScorecard` struct and
  `emit_capacity_scorecard` function after `compute_capacity_vector` (after
  the new line added in S4-2, approximately line 100+). Header-only inline.
- `atx-engine/tests/cost/capacity_scorecard_test.cpp` — new test file.

#### Determinism (inert default)

No existing call site is modified. `emit_capacity_scorecard` is a new
function; no driver emits it yet (S7 wires it). Existing digests unchanged.

#### Accept

All four test classes:

- **(a) Off-path byte-identity:** no existing source file modified. Golden
  digests byte-identical (reviewer gate: diff of `stage_combine.cpp`,
  `stage_report.cpp`, `stage_discover.cpp` is empty for this unit).
- **(b) On-path RED→GREEN — capacity curve is monotone decreasing in AUM and
  crosses zero where constructed:**
  Build a hand-crafted 4-name, 60-period panel. Assign weights uniformly
  (0.25 each). Set `ExecutionSimulator` ImpactCfg with known `Y` and `delta`.
  Compute the AUM at which cost equals the known gross edge analytically
  (the capacity point formula from `risk/capacity.hpp:63`). Call
  `emit_capacity_scorecard` with an AUM grid that brackets that analytic value.
  Assert:
  - `curve[i].net_edge_bps >= curve[i+1].net_edge_bps` for all consecutive
    pairs (monotone non-increasing).
  - `|scorecard.capacity_point_aum - analytic_capacity_aum| < 5%` relative
    tolerance (the grid interpolation is approximate; the analytic formula is
    the ground truth).
  - `scorecard.gross_edge_bps > 0` (our fixture has a positive edge).
  - `scorecard.net_edge_at_target < scorecard.gross_edge_bps` (cost erodes
    the edge at any positive AUM with the impact-bearing sim).
- **(c) Twice-run:** same inputs → bit-identical scorecard.
- **(d) Pure function:** no shared mutable state; parallel calls with the same
  input produce identical output (one thread each, verified in the test).

Commit: `feat(p7-s4-3): CapacityScorecard struct + emit_capacity_scorecard helper`

---

### S4-4 — Turnover-penalty default profile and gate→fitness target wiring

**Goal:** make the turnover-penalty mechanism actively useful for the tradeable
profile by (1) adding a `tradeable_fitness_cfg()` factory function in
`atx-engine/include/atx/engine/factory/fitness.hpp` that returns a
`FitnessCfg` with a recommended tradeable-profile default
(`turnover_penalty_slope=2.0`, `max_turnover_target=0.20`), and (2) adding a
`turnover_target_from_gate` helper that computes a `max_turnover_target` from
a gate's `cost_max_turnover` threshold so the search penalty is coherent with
the admission gate — when the gate bars alphas with turnover > T, the search
should be penalised for exceeding T rather than discovering them and failing
admission. Both are **opt-in** (the default `FitnessCfg{}` is unchanged, so
the no-flag path stays byte-identical).

#### Why this unit belongs in S4

- S4 owns `combine/combiner.hpp` and `cost/capacity.hpp` (the capacity side
  of "tradeable"), and the turnover side of the same gap
  (`turnover_penalty_slope=0.0` is listed in the p7 ROADMAP S4 row). The
  factory fitness header is a read-only dependency for S1 (S1 wires the gate
  logic, not the turnover knob). Adding a read-only factory function to
  `fitness.hpp` does not conflict with S1's gate wiring (S1 touches
  `combine/gate.hpp`, `combine/metrics.hpp`, `eval/deflated_sharpe.hpp`
  wiring, and the `trial-count` void-cast in `factory.cpp` — not the
  `FitnessCfg` struct fields or helpers).
- S7 will call `tradeable_fitness_cfg()` from `stage_discover.cpp` when the
  user passes `--tradeable-profile`. S4 ships the helper; S7 wires the flag.

#### Mechanism

```cpp
// fitness.hpp — new helpers (after the FitnessCfg struct, ~line 362)

/// Recommended FitnessCfg for tradeable-profile search.
/// turnover_penalty_slope = 2.0: a 100% excess above the target halves `raw`.
/// max_turnover_target = 0.20: typical intraday mean-rev turnover budget
///   (per-period; matches the rt_cost_bps + min_holding_days gate budget).
/// All other fields: the same defaults as FitnessCfg{} (inert).
/// The caller may override any field after the call.
/// This is OPT-IN: FitnessCfg{} (the default) is unchanged.
[[nodiscard]] inline FitnessCfg tradeable_fitness_cfg() noexcept;

/// Derive a max_turnover_target from a gate cost_max_turnover threshold
/// so the search penalty is coherent with admission.
/// Returns +inf when gate_turnover_limit <= 0 (no gate -> no target).
/// The caller sets this into FitnessCfg::max_turnover_target.
[[nodiscard]] inline atx::f64
turnover_target_from_gate(atx::f64 gate_turnover_limit) noexcept;
```

The constants `kTradeableTurnoverSlope = 2.0` and
`kTradeableMaxTurnover = 0.20` are named, not magic numbers. They live as
`inline constexpr atx::f64` constants in the `detail` namespace of
`fitness.hpp` and are documented with their derivation rationale:

- `kTradeableMaxTurnover = 0.20`: at 10 bps round-trip and 0.20/period turnover,
  the transaction-cost drag is 2 bps/period — still below the minimum viable
  gross edge targeted in the tradeable profile. At 0.30+/period (the
  mean-reversion regime's natural maximum) costs exceed this floor.
- `kTradeableTurnoverSlope = 2.0`: excess of 0.10 above the 0.20 target (i.e.
  turnover=0.30) gives `mult = clamp(1 - 0.10/(0.20*2), 0, 1) = 0.75` — a
  25% raw-fitness haircut. This is a meaningful but not catastrophic penalty,
  leaving the search some latitude while discouraging chronically high turnover.

#### Wiring (file:line)

- `atx-engine/include/atx/engine/factory/fitness.hpp` — add
  `tradeable_fitness_cfg()` and `turnover_target_from_gate()` after the
  `FitnessCfg` struct (after line ~362). Header-only inline; no `fitness.cpp`
  change.
- `atx-engine/tests/factory/fitness_turnover_test.cpp` — new test file. (The
  existing `fitness_test.cpp` fixtures test the penalty formula; this new file
  tests the helper functions and the coherence of the default profile.)

#### Determinism (inert default)

The `FitnessCfg{}` default constructor is NOT modified. `tradeable_fitness_cfg()`
is a NEW free function that returns a non-default-constructed struct. No
existing call site changes. All existing digests byte-identical.

The penalty formula (`finish_report`, `fitness.cpp:405-433`) is not touched —
its inert-default-path byte-identity (verified by the existing golden tests)
is preserved.

#### Accept

All four test classes:

- **(a) Off-path byte-identity:** `FitnessCfg{}` default values are unchanged
  (`fitness.hpp:358-360`). The existing golden-digest tests
  (`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
  `FactoryOos.MineIntoOffPathDigestUnchanged`) remain green. Reviewer gate:
  diff of `src/factory/fitness.cpp` is empty.
- **(b) On-path RED→GREEN — turnover penalty multiplier matches the formula at
  chosen points:**
  Unit test: call `tradeable_fitness_cfg()`, verify
  `slope == kTradeableTurnoverSlope` and `target == kTradeableMaxTurnover`.
  Then manually compute `mult` at three turnover levels using the formula from
  `fitness.cpp:394-432` and assert the result from `finish_report` with a
  synthetic `FitnessCore{..., turnover=X}` matches to within `1e-12`:
  - `X = 0.10` (below target): `mult = 1.0` (no penalty).
  - `X = 0.30` (target + 0.10): `mult = 0.75` (25% haircut).
  - `X = 0.60` (target + 0.40): `mult = clamp(1 - 0.40/0.40, kFloor, 1) =
    kFloor` (maximum penalty).
  Call `turnover_target_from_gate(0.25)` and assert it returns `0.25`. Call
  `turnover_target_from_gate(0.0)` and assert it returns `+inf`.
- **(c) Twice-run:** `tradeable_fitness_cfg()` is a pure function; called twice
  it returns bit-identical structs.
- **(d) Seq==parallel:** `tradeable_fitness_cfg()` has no shared mutable state;
  safe to call from any thread.

Commit: `feat(p7-s4-4): tradeable_fitness_cfg + turnover_target_from_gate helpers`

---

## Sequencing

```
S4-0 (marker)
  │
  ├── S4-1 (EmaDecayPolicy)          independent of S4-2, S4-3, S4-4
  │
  ├── S4-2 (capacity vector)         independent of S4-1, S4-4
  │
  ├── S4-3 (scorecard)               depends on S4-2 (uses cost/capacity.hpp
  │                                  helpers added in S4-2; commit S4-2 first)
  │
  └── S4-4 (turnover helper)         independent of S4-1, S4-2, S4-3
```

S4-0 is the marker (no code). S4-1 and S4-4 are fully independent and can be
dispatched to parallel sub-agents after S4-0 lands. S4-2 must commit before
S4-3 is dispatched (S4-3 extends the same header). S4-1 and S4-2 can also run
in parallel (disjoint files: `weight_policy.hpp` vs `cost/capacity.hpp`).

Commit order: S4-0 → {S4-1 ∥ S4-2 ∥ S4-4} → S4-3 → sprint close.

---

## Risks / guardrails

- **`WeightPolicy` is const — do not mutate it.** The header documents
  `WeightPolicy` as "pure configuration, holds no mutable state" and
  `to_target_weights` is `const`. `EmaDecayPolicy` MUST be a separate struct.
  Any attempt to add mutable state to `WeightPolicy` breaks the caller-
  provided-scratch overload contract (callers hold `const WeightPolicy&`).
- **`kTruncateIters = 8` is load-bearing (determinism §3.2).** Do not change
  this constant in `weight_policy.hpp:534`. The fixed truncate iteration count
  is the determinism pin; a convergence-dependent early exit is forbidden.
- **`fitness.cpp` is S1 territory for gate changes; S4 only adds read-only
  helpers to `fitness.hpp`.** If `fitness.cpp` needs a change, pause and
  coordinate with S1.
- **`capacity_for_alpha` uses last-period weights** (a documented limitation
  inherited from the existing driver T6 path). Do not replace the last-period
  snapshot with a realized PnL estimator inside S4 — the p6-S6-1 realized-
  edge approach is a driver-layer concern (outside the engine header layer S4
  owns). Document the limitation in the `compute_capacity_vector` header
  comment.
- **No hour-long production run as a gate.** Every acceptance criterion is
  unit tests on tiny deterministic fixtures (≤ 50 names, ≤ 100 periods,
  hand-known answers). The dev-panel smoke (`≤ 5 min`) is S7's integration
  gate, not S4's.
- **No golden re-baseline.** If a new field default changes any existing golden
  digest, the unit is rejected — diagnose the regression, don't update the
  golden.
- **`oracle.hpp` is untouchable.** No sprint touches it.
- **`stage_combine.cpp` stub (line 589) is S7's to replace**, not S4's. S4
  proves `compute_capacity_vector` works; S7 calls it from the driver.

---

## Bench / acceptance

| Criterion | Target | Evidence |
|-----------|--------|----------|
| Decay reduces turnover | `turnover_with_decay < turnover_no_decay` by > 5 pp on a rank-swapping fixture | S4-1 class-(b) test |
| Decay with alpha=1.0 byte-identical to stateless | bit-exact equality over 10 rounds on 8-name series | S4-1 class-(a) test |
| reset() restores cold start | period-0 output after reset equals original cold-start output | S4-1 class-(d) test |
| Capacity vector ≠ 1.0 for high-participation alpha | `capacity_vec[high_part] < target_aum` on synthetic panel | S4-2 class-(b) test |
| Capacity vector ordering correct | `capacity[uniform] > capacity[concentrated]` | S4-2 class-(b) test |
| Capacity curve monotone non-increasing | `curve[i].net_edge_bps >= curve[i+1].net_edge_bps` for all i | S4-3 class-(b) test |
| Capacity curve crosses zero near analytic value | `|scorecard.capacity_point_aum - analytic| < 5%` | S4-3 class-(b) test |
| Turnover penalty mult matches formula at 3 levels | <1e-12 error at X=0.10, 0.30, 0.60 vs kTradeableMax=0.20 | S4-4 class-(b) test |
| Gate→target coherence | `turnover_target_from_gate(0.25)==0.25`; `(0.0)==+inf` | S4-4 class-(b) test |
| Off-path byte-identity (all units) | existing goldens (`ScalarRaw_ReproducesGoldenDigest`, `MineIntoOffPathDigestUnchanged`) green; `stage_combine.cpp` diff empty | S4-1/2/3/4 class-(a) tests + reviewer gate |
| Twice-run stability (all units) | identical output on two consecutive runs | each unit class-(c) test |
| `WeightPolicy` unchanged | diff of `WeightPolicy` struct body is empty | reviewer gate |
| `kTruncateIters=8` preserved | grep of `weight_policy.hpp` confirms value unchanged | reviewer gate |

All class-(a), (b), (c), (d) tests across all four units pass before S4 is
marked complete. `oracle.hpp` is untouched. `stage_combine.cpp`,
`stage_discover.cpp`, `config.{hpp,cpp}` diffs are empty (S7 territory).

---

## Out of scope

- CLI threading of `--ema-alpha`, `--capacity-floor` (real vector), or
  `--tradeable-profile` → Sprint 7 (`stage_discover.cpp`, `config.{hpp,cpp}`).
- Replacing the last-period snapshot in `capacity_for_alpha` with a
  realized-PnL-based edge estimator → p6-S6-1 style driver fix, out of S4.
- `FactorModel::neutralize` wiring into `to_target_weights` → S5 (owns the
  risk include chain, `conviction.hpp`, `kelly_sizing.hpp`).
- Dev-panel smoke test → S7's integration gate after CLI threading lands.
- Conviction-weighted sizing in the combiner blend → S5 owns
  `conviction.hpp` and `kelly_sizing.hpp`; S4 must not modify those files.
- SIMD intrinsics for EMA computation → future-work backlog (premature before
  S3 bench baseline shows the ceiling).
