# Sprint 1 — Deflation Gates & Honest Selection

**Goal:** wire the deflation machinery that already exists but is either
un-gated or incorrectly deflated into the admission path, so that (a) the
`AlphaGate` screens on DSR floor / PBO ceiling / split-half stability, and
(b) the cascade pre-gate's `trial_count` parameter is no longer silently
voided — all opt-in, default byte-identical.

**Owns (exclusive):**
`atx-engine/include/atx/engine/combine/gate.hpp`,
`atx-engine/include/atx/engine/combine/metrics.hpp`,
`atx-engine/include/atx/engine/eval/deflated_sharpe.hpp` (wiring/threading
only — no new DSR math),
`atx-engine/src/factory/factory.cpp` (cascade `trial_count` path only);
tests under `atx-engine/tests/combine/` and `atx-engine/tests/factory/`.

**Must NOT touch:** `oracle.hpp` (untouchable every sprint); `factory/fitness.hpp`,
`factory/factory.hpp`, `factory/fitness.cpp`, `factory/search_driver.hpp`,
`factory/generate.hpp`, `factory/genome.hpp`, `factory/behavior.hpp`,
`factory/mutation.hpp`, `factory/pool_view.hpp`; `atx-impl/src/*`
(Sprint 7); any p6-frozen files not listed above.

---

## Implementation-quality handoff block (paste verbatim into every subagent brief)

```text
Implementation quality standard:
Use ats-core/include/ats_orderbook.h as the style reference. Prefer clear module-level intent,
grouped constants/types/APIs, explicit ownership and lifecycle rules, named error contracts, and
concise comments that explain invariants, non-obvious control flow, or domain semantics. Do not
follow weaker patterns that expose constants/structs/prototypes without enough API contract.

Prioritize full end-to-end implementation over partial stubs. A unit is not done until the public
API, implementation, tests, docs/ledger row, and build/test gate are complete. Do not leave TODO
placeholders, fake success paths, unused APIs, or untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership, ordering,
crash/recovery semantics, and tricky domain rules. Do not comment obvious assignments or wrap
every field in noise.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and lifecycle.
- Names are domain-accurate and consistent with nearby ATS code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new abstractions.
```

---

## The deflation gap (verified file:line)

The engine today computes DSR, PBO, and split-half stability but gates on
none of them inside `AlphaGate`. The `FactoryConfig` carries `min_dsr` and
`max_pbo` fields and they ARE applied in the factory's per-candidate
admission loop, but the `AlphaGate` struct — the stateless screen consumed
by both `mine()` and library verdict paths — has no corresponding fields.
Additionally, the cascade pre-gate's `trial_count` parameter is explicitly
voided rather than used to tighten the bound.

| Gap | File:line | Evidence |
|---|---|---|
| `GateConfig` has no `min_dsr`, `max_pbo`, or `require_split_stable` | `gate.hpp:69–77` | Fields end at `min_holding_days = 0.0`; no deflation fields exist |
| `GateVerdict` has no `RejectDsr`, `RejectPbo`, `RejectSplitUnstable` enumerators | `gate.hpp:90–96` | Enum ends at `RejectCorrelated`; any DSR/PBO verdict would need new buckets |
| `AlphaGate::admit` does not consult DSR, PBO, or split-half | `gate.hpp:109–148` | Admission checks fitness → sharpe → turnover → holding → corr; DSR comment at line 24 says "statistical-significance gate is DSR" but the gate body never reads it |
| `cascade_gate_passes` voids `trial_count` | `factory.cpp:983` | `static_cast<void>(trial_count); // reserved for a future (looser-only) tightening` — the bound uses only `train_metrics.sharpe`, ignoring how many trials the sweep actually ran |
| `min_dsr`/`max_pbo` live in `FactoryConfig`, not `GateConfig` | `factory.hpp:126,151` | `FactoryConfig::min_dsr = 0.5` and `FactoryConfig::max_pbo = 1.0`; `AlphaGate` reads only `GateConfig` fields; the two structs are independent |

---

## Architecture note — two admission tiers

**Tier 1 (factory-side, already exists):** `FactoryConfig::min_dsr` gates
individual candidates in the factory's admit loop via `holdout_dsr()`
(`factory.cpp:939`). `FactoryConfig::max_pbo` gates the whole admitted SET
post-hoc via `finalize_run_pbo` (`factory.hpp:328`). `split_ok` is evaluated
inline in the admit loop using `split_floor_ok` (`factory.cpp:262`). These
are factory concerns and are NOT in scope for S1 — the factory tier is
correct already (except the cascade bound).

**Tier 2 (gate-side, the S1 gap):** `AlphaGate::admit` is the stateless
screen consumed by the LIBRARY path (`library::verdict_for`) and any future
caller that holds only a `GateConfig` / `AlphaGate`. Today the gate says
"statistical-significance gate is DSR" in its §5.2 comment but never acts
on it. S1 adds the fields and wires them so any caller of `AlphaGate` gets
the same honesty the factory already has.

**Cascade bound (S1-4):** `cascade_gate_passes` (`factory.cpp:980–998`) is a
conservative upper-bound screen that skips candidates whose train Sharpe is
too low to plausibly clear `min_dsr` on the holdout. The `trial_count`
parameter exists for a tighter (still safe) bound but is voided at line 983.
Wiring it raises SR*_N monotonically with N, making the pre-gate STRICTER at
larger sweep sizes — exactly the anti-snooping intent.

---

## Determinism contract (Sprint 1)

Sprint 1 follows the **p7 Opt-in / default-byte-identical** contract (ROADMAP
§Determinism contract, case A). Every new capability lives behind a new
`GateConfig` field with an inert default:

- `min_dsr = 0.0` — inert (the gate checks `> 0.0` before applying);
  at 0.0 no candidate can fail the DSR floor (DSR ∈ [0,1] ≥ 0).
- `max_pbo = 1.0` — inert (the gate checks `< 1.0` before applying;
  PBO ∈ [0,1] never exceeds 1.0 strictly, so the `> max_pbo` test never
  fires at the default).
- `require_split_stable = false` — inert; the guard is a boolean flag.

At the inert defaults, `AlphaGate::admit` is **byte-identical** to today.
New `GateVerdict` enumerators MUST be appended at the END (the enumerator
order is FROZEN as a stable reject-histogram index; `gate.hpp:90–96`).
Sprint 7 exposes the new fields via CLI; this sprint does NOT touch
`atx-impl/src/`.

**Four test classes per opt-in field (mandatory):**
(a) off-path byte-identity — inert defaults, verdict/digest unchanged;
(b) on-path RED→GREEN — non-inert value, a qualifying candidate flips;
(c) twice-run — same inputs, same verdict, no hidden state;
(d) seq==parallel — where `AlphaGate` is touched, parallel path yields the
    same verdict as sequential (the gate is pure/const so this is trivially
    structural, but the test must confirm).

---

## Dependency / wiring map

```
gate.hpp:GateConfig          ← S1-0 adds min_dsr, max_pbo, require_split_stable
gate.hpp:GateVerdict         ← S1-0 appends RejectDsr, RejectPbo, RejectSplitUnstable
gate.hpp:AlphaGate::admit    ← S1-1/S1-2/S1-3 add checks (after check 3b, before corr)
  └─ needs: metrics.dsr      ← AlphaMetrics must carry dsr (S1-0 adds field)
  └─ needs: metrics.pbo      ← AlphaMetrics must carry pbo field (S1-0 adds field)
  └─ needs: metrics.split_stable ← already in FitnessReport; propagate to AlphaMetrics
metrics.hpp:AlphaMetrics     ← S1-0 adds dsr, pbo, split_stable (inert sentinels: 1.0/0.0/false)
factory.cpp:cascade_gate_passes:983  ← S1-4 removes void, threads N into SR*_N bound
  └─ reads: trial_count (already passed; param was discarded)
  └─ reads: cfg.min_dsr (already read at line 987)
  └─ calls: eval::expected_max_sharpe(N, V) to raise the bound
tests/combine/gate_dsr_pbo_tests.cpp    ← S1-1/S1-2/S1-3 new test file (auto-globbed)
tests/factory/cascade_trial_count_tests.cpp ← S1-4 new test file (auto-globbed)
```

---

## Tasks

### S1-0 — Open ledger + field plumbing (do first; all other units depend on this)

**Goal:** create the sprint ledger (marker commit), add `min_dsr`, `max_pbo`,
`require_split_stable` to `GateConfig`, add `RejectDsr`, `RejectPbo`,
`RejectSplitUnstable` to `GateVerdict` (appended at END, order frozen), and
add `dsr`, `pbo`, `split_stable` carrier fields to `AlphaMetrics` with inert
sentinel defaults. No logic changes — the fields exist but nothing reads them
yet.

**Wiring (file:line):**

- `gate.hpp:77` — append after `min_holding_days`:
  ```cpp
  // Deflation / selection-bias fields (S1 plumbing; inert at the stated defaults).
  atx::f64 min_dsr           = 0.0;   // DSR floor; 0.0 => inert (DSR ∈ [0,1] always ≥ 0)
  atx::f64 max_pbo           = 1.0;   // PBO ceiling; 1.0 => inert (PBO ∈ [0,1] never > 1)
  bool     require_split_stable = false; // require split-half sign agreement; false => inert
  ```
- `gate.hpp:96` — append at END of `GateVerdict` (FROZEN order; append only):
  ```cpp
  RejectDsr,           // S1: holdout DSR below min_dsr floor
  RejectPbo,           // S1: run-level PBO above max_pbo ceiling
  RejectSplitUnstable, // S1: split-half halves disagree in sign
  ```
- `metrics.hpp` — append to `AlphaMetrics` struct (after `holding_days` or
  at end of field list; exact line TBD by implementer from current struct):
  ```cpp
  // Deflation / selection-bias scalars (S1 carriers; inert sentinels below).
  atx::f64 dsr          = 1.0;  // Deflated Sharpe Ratio ∈ [0,1]; 1.0 sentinel => always clears min_dsr=0.0
  atx::f64 pbo          = 0.0;  // Probability of Backtest Overfitting ∈ [0,1]; 0.0 => always clears max_pbo=1.0
  bool     split_stable = false; // both holdout halves share full-sample Sharpe sign
  ```
  Note: `dsr` sentinel 1.0 guarantees any `min_dsr ∈ [0,1]` check passes
  when the field is unset. `pbo` sentinel 0.0 guarantees `pbo > max_pbo`
  with `max_pbo=1.0` never fires.

**Determinism (inert default):** pure addition — no logic changes. Existing
goldens unchanged. `sizeof(GateConfig)` and `sizeof(AlphaMetrics)` grow, but
neither struct is serialized (both are in-memory-only config/metrics
objects). No aggregate-initializer breakage because all fields are appended
at the end. Confirm no designated-initializer list in tests omits the new
fields (C++ designated initializers are positional-agnostic for aggregates
when named, but any brace-list that relies on position will need the new
trailing fields or a default).

**Accept:**
- Project compiles (all existing binaries, debug + release).
- Existing `gate_*` and `library_*` test suites green (no new failures —
  the new fields are inert).
- `sizeof(GateVerdict)` now covers 8 enumerators (was 5); assert in a new
  test file `tests/combine/gate_dsr_pbo_tests.cpp` that the enum has the
  expected count (histogram-layout pin).
- No aggregate-initializer breakage: grep for `GateConfig{` and `AlphaMetrics{`
  brace-list usages and confirm all compile.

---

### S1-1 — DSR floor in `AlphaGate::admit`

**Goal:** when `cfg.min_dsr > 0.0`, reject candidates whose `metrics.dsr`
is below the floor, producing `GateVerdict::RejectDsr`. At the inert default
`min_dsr = 0.0` the guard never fires and the verdict is byte-identical to
today.

**Root cause:** `AlphaGate::admit` (`gate.hpp:109–148`) checks fitness →
sharpe → turnover → holding → corr. The §5.2 header comment at line 24 says
"statistical-significance gate is DSR" but no DSR check exists. The
`AlphaMetrics` struct now carries `dsr` after S1-0.

**Wiring (file:line):**

Insert in `AlphaGate::admit` (`gate.hpp`) after the holding-days check
(line 137–139, the `min_holding_days` guard) and BEFORE the lazy correlation
check (line 143). Position rationale: DSR is a cheap scalar comparison (no
per-pool cost) so it belongs before the O(|pool|·T) corr sweep; it is also
a quality floor like fitness/sharpe, so it slots naturally with the other
floor checks.

```cpp
// S1-1: DSR (Deflated Sharpe Ratio) floor — the statistical-significance gate.
// DSR ∈ [0,1]: measures P(true SR > SR*_N | data) under N-trial selection bias.
// The guard is inert at the default min_dsr=0.0 (DSR is always ≥ 0, so the
// test never fires); no extra cost on the off-path. Fires only when the caller
// explicitly sets a positive bar (e.g. min_dsr=0.5 in FactoryConfig-aligned use).
if (cfg.min_dsr > 0.0 && metrics.dsr < cfg.min_dsr) {
    return GateVerdict::RejectDsr;
}
```

Callers of `AlphaGate::admit` that switch on `GateVerdict` must be updated
to handle `RejectDsr` (the enum is exhaustive-switch enforced — no `default`
allowed). Grep for exhaustive switch sites and add the new arm (typically a
fall-through to the reject-histogram increment, same as `RejectSharpe`).

**Determinism (inert default):** `cfg.min_dsr = 0.0` ⇒ the `> 0.0`
guard is false ⇒ the branch never taken ⇒ byte-identical to today.

**Accept:**
- `gate_dsr_offpath` (in `tests/combine/gate_dsr_pbo_tests.cpp`):
  `GateConfig` with `min_dsr=0.0`, pool of 3 synthetic alphas, each with
  `metrics.dsr = {0.1, 0.4, 0.9}` → same verdicts as today (no
  `RejectDsr` fired). Digest byte-identical.
- `gate_dsr_onpath_reject`: `min_dsr=0.5`, alpha with `metrics.dsr=0.3`
  and otherwise gate-clearing metrics (fitness > floor, sharpe > floor,
  turnover < ceiling) → verdict `RejectDsr`. Same alpha with `min_dsr=0.0`
  → `Accept` (assuming pool empty / corr clears).
- `gate_dsr_onpath_pass`: `min_dsr=0.5`, alpha with `metrics.dsr=0.7` →
  DSR check passes; verdict determined by subsequent checks (corr gate,
  etc.), NOT `RejectDsr`.
- Twice-run: same inputs, same verdict on second call (no state).
- Existing gate test suites still green (off-path byte-identity).

---

### S1-2 — PBO ceiling in `AlphaGate::admit`

**Goal:** when `cfg.max_pbo < 1.0`, reject candidates whose `metrics.pbo`
exceeds the ceiling, producing `GateVerdict::RejectPbo`. At the inert
default `max_pbo = 1.0` the guard never fires (PBO ∈ [0,1] is never
strictly `> 1.0`).

**Root cause:** `pbo.hpp` defines `pbo_cscv_checked` / `pbo_cscv` →
`PboResult{pbo, split_logits, mean_logit}`. PBO is computed in the fitness
pipeline but never consulted in `AlphaGate::admit`. The `AlphaMetrics`
struct now carries `pbo` after S1-0.

**Wiring (file:line):**

Insert in `AlphaGate::admit` after the DSR check added by S1-1, before the
lazy corr sweep:

```cpp
// S1-2: PBO (Probability of Backtest Overfitting) ceiling — run-level overfit screen.
// PBO ∈ [0,1]: → 0 a persistent edge; → 0.5 the IS winner is OOS noise.
// Inert at the default max_pbo=1.0 (PBO never strictly exceeds 1.0, so the
// test never fires). Active only when the caller sets max_pbo < 1.0.
if (cfg.max_pbo < 1.0 && metrics.pbo > cfg.max_pbo) {
    return GateVerdict::RejectPbo;
}
```

As with S1-1, all exhaustive-switch sites on `GateVerdict` must handle
`RejectPbo`.

**Determinism (inert default):** `cfg.max_pbo = 1.0` ⇒ the `< 1.0`
guard is false ⇒ byte-identical to today.

**Accept:**
- `gate_pbo_offpath`: `max_pbo=1.0`, alpha with `metrics.pbo=0.45` →
  no `RejectPbo`; same verdict as today.
- `gate_pbo_onpath_reject`: `max_pbo=0.40`, alpha with `metrics.pbo=0.45`
  and otherwise gate-clearing metrics → `RejectPbo`.
- `gate_pbo_onpath_pass`: `max_pbo=0.40`, alpha with `metrics.pbo=0.30` →
  PBO check passes; verdict by subsequent checks.
- Twice-run: same inputs, same verdict.
- PBO sentinel: alpha with `metrics.pbo = 0.0` (S1-0 default) and
  `max_pbo=1.0` → never rejected by the PBO check regardless of the
  sentinel value.

---

### S1-3 — `require_split_stable` gate in `AlphaGate::admit`

**Goal:** when `cfg.require_split_stable = true`, reject candidates whose
`metrics.split_stable` is false, producing
`GateVerdict::RejectSplitUnstable`. At the inert default
`require_split_stable = false` the guard never fires.

**Root cause:** `FitnessReport::split_stable` is computed in
`factory/fitness.cpp` (step 5b: `sharpe_h1` / `sharpe_h2` / `split_stable`
— both halves share the full-sample Sharpe sign). `split_ok` in the factory
admission loops uses `split_floor_ok` which also checks sign agreement
(`factory.cpp:262,413,686`). However `AlphaGate::admit` — the pure gate
struct — has no such check. The `AlphaMetrics` struct now carries
`split_stable` after S1-0.

**Wiring (file:line):**

Insert in `AlphaGate::admit` after the PBO check (S1-2), before the corr
sweep:

```cpp
// S1-3: split-half stability — reject a single-regime artifact.
// split_stable == both halves of the holdout PnL share the full-sample Sharpe sign.
// Inert at the default require_split_stable=false (boolean guard; never fires).
// When active, rejects candidates where one half contradicts the other
// (strong H1, dead/negative H2 is the canonical failure mode).
if (cfg.require_split_stable && !metrics.split_stable) {
    return GateVerdict::RejectSplitUnstable;
}
```

Exhaustive-switch sites must handle `RejectSplitUnstable`.

**Determinism (inert default):** `require_split_stable = false` ⇒ the
boolean guard is false ⇒ byte-identical to today.

**Accept:**
- `gate_split_offpath`: `require_split_stable=false`, alpha with
  `metrics.split_stable=false` → no `RejectSplitUnstable`; same verdict
  as today.
- `gate_split_onpath_reject`: `require_split_stable=true`, alpha with
  `metrics.split_stable=false` and otherwise gate-clearing metrics →
  `RejectSplitUnstable`.
- `gate_split_onpath_pass`: `require_split_stable=true`, alpha with
  `metrics.split_stable=true` → check passes; verdict by subsequent gates.
- Twice-run: same inputs, same verdict.
- S1-0 sentinel: alpha with `metrics.split_stable = false` (S1-0 default)
  and `require_split_stable=false` → never rejected by this check.

---

### S1-4 — Thread cumulative trial-count into `cascade_gate_passes` bound

**Goal:** remove the `static_cast<void>(trial_count)` no-op in
`cascade_gate_passes` (`factory.cpp:983`) and use the real cumulative N to
compute the expected-maximum SR benchmark SR*_N, making the skip-threshold
strictly tighter at larger sweep sizes (more trials ⇒ higher SR*_N ⇒
stricter bound ⇒ fewer false pre-passes — the safe direction).

**Root cause:** `cascade_gate_passes` (`factory.cpp:980–998`) is a
conservative UPPER BOUND pre-filter: it skips candidates that provably cannot
clear `min_dsr` on the holdout based only on their train Sharpe. The bound
formula (`sr_tr * cascade_gate_factor >= min_dsr`) uses only the Sharpe
proxy and a fixed safety factor. The `trial_count` N was reserved for a
"future tightening" but is voided at line 983.

The tighter bound: at N trials with variance V, SR*_N = E[max Sharpe] =
`expected_max_sharpe(N, V)` (`deflated_sharpe.hpp:115`). A candidate clears
the DSR bar (`min_dsr`) only if its Sharpe exceeds SR*_N by enough for PSR
to reach `min_dsr`. The current bound replaces SR*_N = 0 (the N=1 case);
threading real N raises SR*_N monotonically, so the bound gets strictly
tighter (or equal) — NEVER looser. No admitted set can shrink (the bound is
conservative by construction: a bound failure means "skip"; the actual
holdout still decides).

**Wiring (file:line):**

`factory.cpp:983` — replace `static_cast<void>(trial_count)` with:

```cpp
// S1-4: thread the realized trial-count into the expected-maximum SR benchmark.
// At N=1 (the previous implicit behavior after voiding trial_count), expected_max_sharpe
// returns 0 — no selection bias accounted for. At N>1 the benchmark SR*_N rises
// monotonically with N, raising the skip threshold and TIGHTENING the pre-gate
// (a LARGER N makes the pre-gate STRICTER, which is conservative: a candidate
// skipped here would have been rejected anyway if the full holdout DSR were checked).
// The safety factor (cascade_gate_factor, already in use below at line 997) absorbs
// the per-trial variance V; we use the single-stream estimator V = (1/T)*(1 - ...),
// approximated here by the unit-variance case (V=1/T, T ~ 252) to keep the
// bound fast and allocation-free. For the CASCADE GATE we only need a CONSERVATIVE
// UPPER BOUND on DSR achievability, not the exact DSR value — any monotone-in-N
// raise of SR*_N satisfies that contract.
const atx::f64 sr_star_n =
    (trial_count > 1U && cfg.min_dsr > 0.0)
        ? eval::expected_max_sharpe(trial_count,
              1.0 / static_cast<atx::f64>(combine::kAnnualizationDays))
        : 0.0; // N==1 or bar off: SR*_1 == 0 (no selection; byte-identical to old void path)
```

Then in the keep/skip decision at line 997, fold SR*_N into the threshold:
the skip condition becomes:
`sr_tr * cfg.cascade_gate_factor + sr_star_n < cfg.min_dsr` (roughly:
is even the optimistic Sharpe still below the N-deflated bar?). The precise
formulation must be shown equivalent to the existing `>=` idiom at line 997
and proven safe (never skips an admissible candidate) in the Accept tests.

Note: `eval::expected_max_sharpe` is `#include`d via
`atx/engine/eval/deflated_sharpe.hpp` — already transitively available in
`factory.cpp` (confirm the include chain; add explicit include if absent).

**Determinism (inert default):**
- When `trial_count == 1` (the previous implicit value after void): `sr_star_n = 0.0`
  ⇒ the bound collapses to the OLD formula ⇒ byte-identical.
- When `cfg.min_dsr == 0.0` (inert): the whole pre-gate returns `true`
  immediately at line 987–989 before the new code runs ⇒ byte-identical.
- When `trial_count > 1` AND `cfg.min_dsr > 0.0`: the bound is strictly
  tighter — the pre-gate skips MORE candidates. Those candidates would have
  failed the holdout DSR gate anyway (by the conservatism proof), so the
  admitted set is unchanged. The digest is unchanged (digest folds only
  admitted/rejected decisions, not pre-gate skips — pre-gate skips bypass
  the digest fold entirely).

**Accept:**
- `cascade_trial_count_inert_n1`: `trial_count=1`, `min_dsr=0.5`,
  `cascade_gate_factor=3.0`, a train Sharpe of 0.10 ⇒ same result as the
  old void path (skip: `0.10 * 3.0 = 0.30 < 0.50`).
- `cascade_trial_count_n50_tighter`: same Sharpe=0.10 but `trial_count=50`
  ⇒ `sr_star_n > 0` ⇒ the effective threshold rises ⇒ the pre-gate still
  skips (the bound is tighter; this candidate is even more hopeless at N=50).
- `cascade_trial_count_monotone`: for a fixed Sharpe and `min_dsr`,
  assert `skip_at_N=100 implies skip_at_N=10` (monotone: larger N ⇒
  equal or stricter pre-gate). Test with N ∈ {1, 10, 50, 100}.
- `cascade_trial_count_safe`: synthesize a candidate with Sharpe high enough
  to clear `cascade_gate_passes` at N=1 AND at N=100 (it's a keeper; the
  pre-gate must never skip it). Assert `cascade_gate_passes = true` for
  both N values.
- `cascade_trial_count_inert_min_dsr_zero`: `min_dsr=0.0`, any trial_count
  ⇒ always returns true (gate off; byte-identical pre-S1-4 behavior).
- Twice-run: same inputs, same result (no state in this pure function).

---

### S1-5 — Reject-histogram layout pin + telemetry update

**Goal:** add a compile-time / test-time histogram-layout assertion for the
expanded `GateVerdict` enum (now 8 enumerators vs. 5 before S1-0), confirm
no existing histogram index shifts, and update any reject-histogram
initialization code that hard-codes the old count.

**Root cause:** S1-0 appends three new enumerators to `GateVerdict`
(`RejectDsr`, `RejectPbo`, `RejectSplitUnstable`). `FactoryReport` carries
a `reject_histogram` array sized by `GateVerdict` count. Any code that
initializes or displays the histogram with a hard-coded size of 5 will
silently undercount. `AdmitKind` in `library.hpp` is a SEPARATE enum (also
FROZEN) — it is mirrored against `GateVerdict` for the library path and must
also be audited.

**Wiring (file:line):**

- Grep for `reject_histogram` array sizing and any `static_assert` on enum
  count (likely in `factory.hpp` or `factory.cpp`) — update to reflect the
  new count (5 → 8 for `GateVerdict`-driven buckets).
- In `tests/combine/gate_dsr_pbo_tests.cpp` (created S1-0), add:
  ```cpp
  // Histogram-layout pin: new enumerators APPENDED at end; existing indices frozen.
  ATS_TEST(gate_verdict_histogram_layout) {
      // GateVerdict after S1: Accept=0, RejectSharpe=1, RejectFitness=2,
      // RejectTurnover=3, RejectCorrelated=4, RejectDsr=5, RejectPbo=6,
      // RejectSplitUnstable=7. Total = 8.
      static_assert(static_cast<int>(combine::GateVerdict::Accept)              == 0);
      static_assert(static_cast<int>(combine::GateVerdict::RejectCorrelated)    == 4);
      static_assert(static_cast<int>(combine::GateVerdict::RejectDsr)           == 5);
      static_assert(static_cast<int>(combine::GateVerdict::RejectPbo)           == 6);
      static_assert(static_cast<int>(combine::GateVerdict::RejectSplitUnstable) == 7);
  }
  ```
- Audit `AdmitKind` (`library.hpp`) for a parallel enum — if it mirrors
  `GateVerdict` 1:1, the same three new members must be appended there too,
  with the same FROZEN-order constraint. If it diverges from `GateVerdict`,
  document which verdicts map to which `AdmitKind` values (the mapping is
  the seam; it must not be implicit). A mismatch is a correctness bug.

**Determinism (inert default):** pure bookkeeping. No logic change. Existing
goldens unchanged. The histogram grows from 5 to 8 buckets; new buckets
initialize to 0 and remain 0 on the off-path (inert defaults).

**Accept:**
- `gate_verdict_histogram_layout` static_assert test compiles and passes.
- `AdmitKind` audit documented in the ledger row (either "no change needed"
  or "appended N new values at END, indices K..K+N-1").
- All reject-histogram display / reporting code compiles with the new count.
- Existing `factory_*` and `library_*` tests green (histogram arrays are
  zero-initialized; new buckets are zero; no undercount crash).

---

## Sequencing

1. **S1-0 first** (field plumbing + ledger marker commit) — all other units
   read the new `GateConfig` / `GateVerdict` / `AlphaMetrics` fields.
2. **S1-1, S1-2, S1-3 in parallel** — disjoint checks within
   `AlphaGate::admit`; S1-1 touches the DSR branch, S1-2 the PBO branch,
   S1-3 the split-stable branch. All three are in `gate.hpp` but at distinct,
   non-overlapping insertion points and they generate tests in the same
   `gate_dsr_pbo_tests.cpp` file (coordinate to avoid conflicts, or split
   into per-unit test files and merge).
3. **S1-4** — independent of S1-1/S1-2/S1-3 (touches only `factory.cpp`).
   Can run concurrently with S1-1/S1-2/S1-3 after S1-0 lands.
4. **S1-5** — depends on S1-0 (enum count known) and S1-1/S1-2/S1-3
   (exhaustive-switch sites confirmed). Run last, after the three gate units
   land, to write the final histogram-layout pin.

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| `GateVerdict` enumerator order corrupted | Reject-histogram indices silently shift; wrong bucket gets the count | Enumerators MUST append at END. S1-5 histogram-layout static_assert enforces. CI compile check catches any reordering. |
| `AdmitKind` (`library.hpp`) not updated to mirror new `GateVerdict` values | Library path produces a `GateVerdict::RejectDsr` the `AdmitKind` switch cannot handle; compile error or silent fall-through | Audit `AdmitKind` in S1-5; add mirroring values if 1:1; document if divergent. No `default` in exhaustive switches enforces compile error on missing arm. |
| `AlphaMetrics` sentinel values wrong — inert defaults trip the gate | `dsr=0.0` sentinel with any `min_dsr>0` would immediately reject everything | Sentinel is `dsr=1.0` (DSR max; always clears); `pbo=0.0` (PBO min; always clears). The Accept tests for S1-1/S1-2 explicitly exercise sentinel-field alphas to confirm no spurious rejection. |
| `cascade_gate_passes` tightening makes the pre-gate incorrect (skips an admissible candidate) | An alpha that would have been admitted on the holdout is skipped without evaluation; admitted set changes | The SR*_N bound is always ≤ the actual SR*_N (conservative variance estimate). The `cascade_trial_count_safe` test constructs a known-admissible candidate and asserts it is never skipped at any N. The bound proof is documented in the S1-4 commit comment. |
| `expected_max_sharpe` include not transitively available in `factory.cpp` | Compile error | Check include chain before writing S1-4; add `#include "atx/engine/eval/deflated_sharpe.hpp"` directly if absent. |
| S1-1/S1-2/S1-3 parallel edit conflict in `gate.hpp` | Merge conflict or double-insert of checks | Sequential dispatch is safer for `gate.hpp` edits (S1-1 → S1-2 → S1-3 in order). If parallel, split into clearly labeled insertion-point blocks and merge before commit. |
| `metrics.dsr` / `metrics.pbo` never populated by the factory | Gate always sees sentinel values; the gate exists but never fires even when active | Implementer must verify the factory's `admit_on_holdout` / `mine()` paths propagate `dsr`/`pbo` into the `AlphaMetrics` it passes to the gate. If propagation is missing, add it in `factory.cpp` (within S1's owned scope). Document in ledger row. |

---

## Bench / acceptance (sprint close)

- **Default-field byte-identity:** run the existing `gate_*`, `library_*`,
  and `factory_*` golden suites with default `GateConfig`
  (`min_dsr=0.0`, `max_pbo=1.0`, `require_split_stable=false`) → zero
  verdict changes, zero digest changes, zero new test failures.
  `AtxImplDiscover` slice byte-identical (`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`
  and `FactoryOos.MineIntoOffPathDigestUnchanged` must both remain green).
- **Per-task RED→GREEN:** each opt-in field has a test that starts RED before
  the implementation and GREEN after; see individual task Accept sections.
- **Twice-run determinism:** every new test class includes a "call twice,
  compare verdict" assertion.
- **seq==parallel (S1-4):** `cascade_gate_passes` is a pure function; the
  parallel mine path (`mine_into_oos_parallel`, `factory.cpp:~1528`) calls
  it from worker threads. Confirm the function is stateless (it reads only
  its arguments and `cfg` — it is) and the test exercises both
  `mine_into_oos` and `mine_into_oos_parallel` paths with `trial_count > 1`.
- **Histogram-layout pin (S1-5):** `gate_verdict_histogram_layout`
  static_assert test green; `AdmitKind` audit complete and documented.
- **DSR monotone in N (S1-4):** `cascade_trial_count_monotone` test
  demonstrates that the skip threshold is non-decreasing in N at fixed
  Sharpe and `min_dsr` — the fundamental anti-snooping property.

---

## Out of scope

- Wiring `GateConfig::min_dsr`, `max_pbo`, `require_split_stable` to the
  CLI or `atx-impl/src/config.cpp` — Sprint 7.
- Populating `AlphaMetrics::dsr`/`pbo`/`split_stable` from the DSL search
  evaluation loop (the in-search fitness path) — those fields are for the
  gate path only; in-search DSR is already in `FitnessCfg::trial_count`.
- Any `atx-impl` source files.
- `oracle.hpp` — untouchable.
- New CLI flags or `RunConfig` additions — Sprint 7 owns the CLI hub.
- Changing `FactoryConfig::min_dsr` or `FactoryConfig::max_pbo` behavior —
  these already work at the factory tier; S1 adds the GATE tier equivalents.
