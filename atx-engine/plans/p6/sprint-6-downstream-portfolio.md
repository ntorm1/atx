# Sprint 6 — Downstream Portfolio Correctness

**Goal:** make combine→optimize→report deploy a sign-correct, non-empty, sanely-sized book
for an admitted alpha; the current pipeline inverts the sign (MVO −1.86 vs raw +1.93), zeros
the book under capacity, and reports 8,464,812% participation on alpha101 a42.

**Owns (exclusive):** `atx-impl/src/{stage_combine,stage_optimize,stage_report}.cpp`,
`atx-impl/src/{book_shape,diag_risk}.hpp`,
`atx-impl/tests/{combine,cost_bps,e2e_pipeline}_test.cpp`.

**Must NOT touch:** `atx-impl/src/stage_run.cpp`, `atx-impl/src/config.{hpp,cpp}`
(default-flip reserved for Sprint 7); `atx-engine/include/atx/engine/risk/optimizer.hpp`,
`atx-engine/include/atx/engine/combine/combiner.hpp`,
`atx-engine/src/combine/crowding.cpp` (all three engine libs are CORRECT — every fix lives
in the atx-impl driver layer); `atx-engine/tests/factory/oracle.hpp`.

---

## Context

These are **bug fixes** (current downstream numbers are wrong), so output WILL change.
Each fix is guarded by a regression test pinning the corrected behavior.
The DEFAULT selection that triggers the sign-correct path (position_mode vs λ=0) is set by
Sprint 7 in `stage_run`/`config`; here, implement the sign-correct deploy path and make it
selectable via an existing owned mechanism, proven with a test that constructs optimizer
inputs directly (not via `run_all`).

The determinism contract for S6 is **(A) opt-in / default-byte-identical** (from ROADMAP §Shared
determinism contract): any output-changing capability is gated behind a new engine-config field
defaulting to today's value. The three test classes required are (a) off-path byte-identity,
(b) on-path RED→GREEN, (c) twice-run stability.

---

## The three confirmed defects

| # | Label | Root-cause location | Symptom |
|---|-------|--------------------|---------| 
| 1 | SIGN-FLIP | `stage_optimize.cpp:193-223` → `optimizer.hpp:317-349` | report Sharpe −1.86 for a +1.93 alpha |
| 2 | CAPACITY-ZEROING | `stage_combine.cpp:259-282`; applied `crowding.cpp:73-79` | cap_scale=0, empty book |
| 3 | PARTICIPATION OVERFLOW | `stage_report.cpp:394-400` | 8,464,812% participation |

### Defect detail

**SIGN-FLIP.** The default MVO path (`position_mode=false`, `risk_aversion=1.0`,
`stage_optimize.cpp:193-223`) feeds the combined TARGET-WEIGHT vector to
`PortfolioOptimizer::solve` as an expected-return proxy. Inside the optimizer,
`t = (1/2λ)·P V⁻¹ P α` with diagonal `V = diag(return-variance)` re-weights by `1/dvar`
and re-centers (`optimizer.hpp:317-349`), inverting the realized book. The `λ=0` branch
(`t = demean(α)`, `optimizer.hpp:317,329-334`) is the only sign-preserving path and is NOT
the default. Combine is exonerated for a single alpha: `combiner.hpp:486-488` returns
`w = [1.0]` short-circuit for `n==1`; `breadth_realized_ir = +0.887`.

**CAPACITY-ZEROING.** `alpha_capacity_aum` (`stage_combine.cpp:259-282`) estimates edge
from the alpha's **last-period target weights** held over ALL history: it takes the final
`w = streams.positions(a, n_periods-1)` and computes `gross_edge_bps` as the mean of
`Σ_i w_i·ret_i(t)` over every historical period. A mean-reversion alpha's frozen last-day
book is easily ≤0 over all history even when its realized OOS edge is +1.93 — triggering
the `gross_edge_bps <= 0.0 → return 0.0` hard-zero at line 280-281. That zero propagates
through `crowding.cpp:73-74` as `cap_scale = clamp(0 / floor, 0, 1) = 0`, zeroing
`out[i]` for every name.

**PARTICIPATION OVERFLOW.** `stage_report.cpp:394-400` computes `dvol = rc * vol` using
a single raw day's close×volume. With `report_aum=$1B` default and any thin-volume name,
`part = notional / dvol` explodes. `alpha_max_participation` in `stage_combine.cpp:319-330`
correctly uses a trailing 20-day average `dollar_adv`; the report stage does not mirror
this averaging.

---

## Tasks

### S6-0 — Sign-correct deploy of the combined book

**Root cause:** `stage_optimize.cpp:193-223`: default `position_mode=false`,
`risk_aversion=1.0`; combined target-weight panel fed as expected-return proxy to MVO;
`optimizer.hpp:317-349` applies `t=(1/2λ)·P V⁻¹ P α` with diagonal variance, inverting
the book. The λ=0 path (`optimizer.hpp:329-334`, `t=demean(α)`) is sign-preserving
but not default.

**Fix (driver layer only — do NOT edit `optimizer.hpp`):** In `stage_optimize.cpp`, when the
input is a combined target-weight panel, route through the **position-mode** path already
implemented in `book_shape.hpp` (`shape_book`: dollar-neutralize → gross-normalize →
name-cap), OR force `risk_aversion=0.0` to select the `t=demean(α)` MVO branch. Gate the
routing behind an existing config field (e.g. `position_mode`) so the off-path byte-identical
digest is unchanged. Add a code comment at the decision point citing:

```
// ROOT CAUSE S6-0: default MVO (λ=1) feeds combined target-weights as expected-returns;
// t=(1/2λ)·P V⁻¹ P α with diagonal V re-weights by 1/dvar and inverts the book
// (optimizer.hpp:318). The λ=0 branch t=demean(α) is sign-preserving (optimizer.hpp:317).
// When the input is a combined target-weight panel, use position-mode (shape_book) or
// force risk_aversion=0. The MVO math in optimizer.hpp is correct for a TRUE expected-return
// input — do not edit it.
```

**Determinism:** off-path byte-identity test: with `position_mode=false` (default) the output
digest is unchanged. On-path RED→GREEN test: with the sign-correct routing active, a known
positive alpha produces a positive deployed Sharpe.

**Accept:** test passes; a known-positive synthetic alpha fed as a combined target-weight
panel exits with the same sign as the raw blend IR; `optimizer.hpp` untouched.

---

### S6-1 — Realized-edge capacity estimation

**Root cause:** `stage_combine.cpp:259-282`: `alpha_capacity_aum` takes the alpha's
LAST-period target weights (`streams.positions(a, n_periods-1)`, line 261) and computes
mean book return over ALL history using those frozen weights. For a mean-reversion alpha
the frozen last-day book is random-sign over history → `gross_edge_bps ≤ 0` → hard-zero
`return 0.0` (line 280-281) → `cap_scale=0` in `crowding.cpp:73-74` → empty book.

**Fix (driver layer only — do NOT edit `crowding.cpp`):** In `stage_combine.cpp`,
replace the frozen-last-period edge in `alpha_capacity_aum` with the alpha's **realized
per-period blend PnL mean** (the existing `pool.pnl(a)` stream mean is available and
reflects the actual OOS edge). Guard the `≤0 → 0` rail: only hard-zero when the realized
OOS edge is genuinely ≤0; otherwise floor capacity at the gross-frictionless capacity.
Add a code comment:

```
// BUG S6-1: original used last-period frozen weights over all history to estimate edge.
// A mean-reversion alpha's terminal book is random-sign over history → gross_edge_bps≤0
// → hard-zeros capacity (stage_combine.cpp:280-281) even when realized OOS edge is +1.93.
// Fix: use pool.pnl(a) stream mean (actual realized per-period edge) as the edge estimate.
```

**Determinism:** off-path byte-identity: when `--capacity-floor` is absent (disabled), the
crowding pass-through is unchanged. On-path RED→GREEN: a synthetic alpha with positive
realized PnL but a negative frozen-snapshot edge gets non-zero capacity.

**Accept:** test passes; synthetic alpha with positive realized PnL but negative frozen-day
book gets non-zero capacity and a non-empty book; `crowding.cpp` untouched.

---

### S6-2 — Sane participation footprint

**Root cause:** `stage_report.cpp:394-400`: `dvol = rc * vol` is a SINGLE day's
raw close×volume. With `report_aum=$1B` and any thin-volume name, `part = notional / dvol`
blows up to millions of percent. `alpha_max_participation` at `stage_combine.cpp:319-330`
correctly uses a trailing 20-day `dollar_adv` average (window `kAdvWindow=20`); the report
stage does not mirror this.

**Fix (driver layer only):** In `stage_report.cpp`, compute `dvol` as a **trailing 20-day
average dollar-volume** mirroring the `dollar_adv` calculation in `stage_combine.cpp:321-330`.
Clamp/winsorize per-name participation at a sane ceiling (e.g. 1.0 = 100% ADV). Report
**p95 and p99** participation plus the raw max, rather than only the raw max that one thin
name dominates. (The `report_aum` default itself is a config default → Sprint 7; make the
metric robust to a large AUM without changing the default.) Add a code comment:

```
// BUG S6-2: original dvol = rc * vol was a single-day snapshot; one thin-volume name
// inflates participation to millions of percent. Fix: 20-day trailing avg dollar-volume
// mirrors stage_combine.cpp:321-330 (alpha_max_participation, kAdvWindow=20).
```

**Determinism:** the KV output schema adds new keys (`p99_participation_pct`,
`p95_participation_pct`); the existing `max_participation_pct` key is retained for
backward compatibility. Off-path byte-identity: schema extension only (no removal).

**Accept:** test passes; on a synthetic panel with one thin-volume name, p95 participation
is finite/sane and the raw max is no longer inflated; `max_participation_pct` key still
present.

---

### S6-3 — End-to-end downstream regression test

**Root cause / motivation:** the combine→optimize→report chain has never been validated
end-to-end with a real admitted alpha. All three defects above were invisible without
an e2e test on a known-positive alpha.

**Fix:** Add `atx-impl/tests/e2e_pipeline_test.cpp` that:

1. Builds a **small augmented synthetic panel** (≥50 instruments, ≥252 periods) with a
   single known-positive alpha (positive mean cross-sectional book return, IR > 0).
2. Constructs `StageInputs` directly and runs `stage_combine` → `stage_optimize` (via the
   sign-correct routing from S6-0) → `stage_report` through the FIXED stage functions.
3. Asserts all of:
   - Non-empty book: `avg_names_held > 0`.
   - Sign-correct portfolio Sharpe: `portfolio_oos_sharpe > 0` for a positive alpha.
   - Sane participation: `p95_participation_pct < 100.0` (i.e. < 100% ADV at p95).
   - Net-of-cost identity: `net_pnl ≈ gross_pnl − cost_pnl` within floating-point
     tolerance (verifies the PnL accounting identity, not a magnitude target).
4. Runs twice and produces identical output (twice-run stability).

Do NOT use `run_all` or `stage_run` — construct optimizer inputs directly so S7's default
wiring is not a prerequisite.

**Accept:** all four assertions pass; engine libs untouched; test is deterministic.

---

## Sequencing

1. **S6-0** (sign-flip) — implement and test first; gates S6-3's sign-correctness assertion.
2. **S6-1** (capacity) — independent of S6-0; implement in parallel or immediately after.
3. **S6-2** (participation) — independent; implement in parallel or immediately after S6-1.
4. **S6-3** (e2e regression) — compose after S6-0, S6-1, S6-2 are green; the e2e test
   exercises all three fixes together.

Each task commits independently (marker → S6-0 → S6-1 → S6-2 → S6-3 → close) following
the marker-commit pattern: code + tests + ledger row in the same commit per unit.

---

## Risks / guardrails

- **Engine libs are correct — do not edit them.** `optimizer.hpp`, `combiner.hpp`,
  `crowding.cpp` all have the right math for their intended inputs. Every fix is a
  misuse-of-API correction in the driver layer. If a fix seems to require touching an
  engine lib, re-examine the driver call site first.
- **Default flip is Sprint 7.** Do not change `stage_run.cpp` or `config.{hpp,cpp}`.
  The sign-correct path must be reachable via an existing/owned config field and proven
  with a direct-construction test. `run_all` with stock defaults must still produce the
  same legacy digest.
- **Off-path byte-identity is mandatory (contract A).** For every behavioral change,
  the test class (a) — default path, golden digest unchanged — must be green before the
  per-unit commit lands. If it is red, the commit does not land.
- **`oracle.hpp` is untouchable** by every sprint.

---

## Bench / acceptance

| Criterion | Target | Evidence |
|-----------|--------|----------|
| SIGN-FLIP fixed | `portfolio_oos_sharpe > 0` on known-positive synthetic alpha | S6-0 test |
| CAPACITY fixed | non-empty book (`avg_names_held > 0`) on positive-PnL alpha with negative frozen-day snapshot | S6-1 test |
| PARTICIPATION fixed | `p95_participation_pct < 100.0` on thin-name synthetic panel | S6-2 test |
| Net=Gross−Cost identity | `|net − (gross − cost)| < 1e-9` per period in e2e | S6-3 test |
| Sign-correct e2e | `avg_names_held > 0`, `portfolio_oos_sharpe > 0`, `p95_participation_pct < 100.0` on synthetic augmented panel | S6-3 test |
| Off-path byte-identical | default-path digest unchanged for all three fixes | S6-0/1/2 class-(a) tests |
| Twice-run stable | identical output on two consecutive runs | S6-3 class-(c) test |
| Engine libs untouched | `optimizer.hpp`, `combiner.hpp`, `crowding.cpp` diff is empty | reviewer gate |

All seven regression tests + the e2e test pass before S6 is marked complete. On a
known-positive synthetic alpha the downstream yields a sign-correct non-empty book with
sane participation and correct net PnL accounting.

---

## Out of scope

- The DEFAULT flip (`run_all` using the sign-correct path, `position_mode`/`risk_aversion`
  config defaults) → Sprint 7.
- `report_aum` default → Sprint 7 (`config.cpp`).
- Engine optimizer/combiner math — correct by design.
- Any change to `stage_discover.cpp` or `stage_sweep.cpp`.
