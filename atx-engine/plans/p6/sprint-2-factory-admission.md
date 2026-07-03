# Sprint 2 — Factory Admission: Perf, Early-Abort, De-duplicated Ladder

**Goal:** Make the per-candidate evaluate→confirm-on-holdout→admit loop faster and safer:
reuse the holdout Engine across loop iterations, skip doomed holdout evals via a conservative
train-DSR pre-gate, replace K-pass sub-window metric recomputes with prefix-sum slicing, and
collapse the 4× copy-pasted admission ladder into one shared helper — all byte-identical on
the default path.

**Owns (exclusive):** `atx-engine/src/factory/factory.cpp`,
`atx-engine/include/atx/engine/factory/factory.hpp`,
`atx-engine/tests/factory/factory_oos_test.cpp`.
**Must NOT touch:** alpha eval headers (Sprint 1); fitness/search/mutation/genome (Sprint 3);
combine/gate/metrics/cost (Sprint 4); config/stage_* (Sprint 7); `oracle.hpp`.

---

## Determinism contract (S2)

S2 uses the **(B) two-tier EvalMode** contract inherited from p5 Sprint 1 and ratified in the p6
ROADMAP (`atx-engine/plans/p6/ROADMAP.md §Shared determinism contract`):

- **`AuditExact`** — today's bit-identical, chronological-order, single-thread-equivalent path;
  the system of record. Unchanged.
- **`ResearchFast`** — unlocks SIMD horizontal reductions, online variance, parallel reduction,
  fast-math; for search and ranking only. Every admitted alpha is re-evaluated once in
  `AuditExact` before it is written to the library.

S2's tasks are **all allocation/refactor/pre-gate changes on the admission path**, which sits
firmly in `AuditExact` territory. The admission digest MUST stay byte-identical:

> **Non-negotiable:** `FactoryOos.MineIntoOffPathDigestUnchanged` and all OOS goldens stay
> green. The cascade gate (S2-1) is perf-only and MUST NOT drop any currently-admitted
> candidate. The ladder refactor (S2-3) is a pure structural change; `seq==parallel` and
> `serial==seed` invariants must hold by construction after it lands.

---

## Where the cost goes (audited, `factory.cpp`)

| Sink | Location | Cost | Class |
|---|---|---|---|
| **Holdout `Engine` constructed fresh per candidate** in serial OOS loop | `factory.cpp:1351` (`alpha::Engine engine{holdout}`) | Full Engine init × every ranked candidate; train side already reuses one `train_engine` (`factory.cpp:1082`) | Alloc/init overhead |
| **Holdout `Engine` constructed fresh per candidate** in serial seed pre-pass | `factory.cpp:1200`, `factory.cpp:1792` | Same issue; two more seed-pre-pass sites | Alloc/init overhead |
| **`mean_cse_pct` recompiles every scored genome** at report time | `factory.cpp:2136–2147` (`mean_cse_pct` body: `alpha::compile` called per genome in `res.all_scored`) | Full re-compile of every scored genome purely to read `cache_hit_pct()` telemetry | Report-only recompile |
| **`cse_pct` denominator skips compile failures** | `factory.cpp:2139–2147` (loop iterates `all_scored`, skips `!prog.has_value()` — compile failures excluded from `n`) | Denominator is `n_successfully_compiled`, not `n_evaluated`; mean is wrong when compile failure rate is non-zero | Correctness bug (telemetry) |
| **K-pass sub-window `compute_metrics` recompute** — serial OOS loop | `factory.cpp:1434–1435` (`compute_metrics` per sub-window in the `dsr_subwindows` loop) | K full metric passes per candidate when `dsr_subwindows >= 2`; each slices `hold_pnl/hold_pos` and runs O(T/K) statistics | O(K) recompute |
| **K-pass sub-window `compute_metrics` recompute** — parallel OOS gather loop | `factory.cpp:2010–2011` | Same K-pass pattern in the parallel admit loop | O(K) recompute |
| **K-pass sub-window `compute_metrics` recompute** — serial seed pre-pass | `factory.cpp:1272`, `factory.cpp:1864` | Two seed pre-pass sites with the same K-pass sub-window block | O(K) recompute |
| **Admission ladder copy-pasted 4×** | `factory.cpp:1288–1330` (serial OOS — "3d ADMISSION on the HOLDOUT"); `factory.cpp:1454–1510` (parallel OOS gather); `factory.cpp:1880–1926` (serial seed pre-pass A); `factory.cpp:2030–2070` (parallel seed pre-pass B) | price_scale → sub-windows → `hold_dsr>=min_dsr && split_ok` → `try_admit` → histogram → digest fold; four independent copies; `seq==parallel` and `serial==seed` invariants are comment-enforced, not structural | Drift hazard + correctness risk |

---

## Tasks

### S2-0 — Reuse one holdout Engine; kill `mean_cse_pct` recompile; fix `cse_pct` denominator

**Priority:** P1 (prerequisite for S2-1; pure perf/telemetry, byte-identical).

**Root cause:**
- `factory.cpp:1351`: `alpha::Engine engine{holdout}` is constructed fresh inside the serial
  OOS loop body. The train loop correctly hoists `alpha::Engine train_engine{train}` outside
  the loop at `factory.cpp:1082`. The holdout never got the same treatment.
- `factory.cpp:1200`, `1792`: The two serial seed pre-passes have the same fresh-Engine
  construction inside their holdout evaluation blocks.
- `factory.cpp:2136–2147`: `mean_cse_pct` iterates `res.all_scored` and calls
  `alpha::compile(g.ast, g.analysis)` on every genome purely to read `prog->cache_hit_pct()`.
  This is a report-only recompile of the entire scored population.
- `factory.cpp:2139–2147` (`cse_pct` denominator): the loop skips `!prog.has_value()` entries
  and divides by `n` (successfully compiled). If some genomes fail to compile, the denominator
  is smaller than `evaluated`, giving an inflated mean. The correct denominator is the same
  `evaluated` set used for other telemetry.

**Fix:**
1. Hoist a single `alpha::Engine holdout_engine{holdout}` before the serial OOS loop (mirror
   the `train_engine` pattern at line 1082). Replace the three fresh-Engine construction sites
   (`factory.cpp:1351`, `1200`, `1792`) with reuse of this hoisted engine.
   - Dependency note: if Sprint 1 has landed a reusable-Engine API (reset/reinit), prefer it;
     otherwise hoist the `Engine` directly and note the S1 dependency in the ledger.
2. Replace `mean_cse_pct` with a pass over the already-available cache-hit counters from the
   compile results cached during the ranking phase — or, if the ranking phase does not cache
   them, simply read the field from `res.all_scored` metadata without recompiling. If no clean
   path exists without the recompile, drop `rep.cse_pct` from the report entirely (it is
   telemetry-only; its absence does not affect any admission decision or digest).
3. Fix the denominator: compute `cse_pct` over the same denominator as `rep.evaluated`
   (or `n_cands` at the ranking site), not the subset that passed compile.

**Determinism:** All three fixes are allocation changes or telemetry fixes; no change to any
admission decision, histogram entry, or digest fold. Byte-identical.

**Accept:**
- `FactoryOos.*` stays green (including digest and golden slice).
- `rep.cse_pct` now reports over the correct denominator — verify with a config where some
  genomes fail to compile (`cse_pct` before < `cse_pct` after when failures exist).
- Bench: holdout-Engine alloc count per run drops to 1 per seed-pass invocation (vs.
  `n_ranked` constructions before).

---

### S2-1 — Train→holdout cascade gate (conservative true upper-bound pre-gate)

**Priority:** P4 (perf-only; depends on S2-0 having hoisted the holdout Engine so we know
the cost we are skipping).

**Root cause (`factory.cpp:1351`):** Every ranked candidate in the serial OOS loop reaches the
holdout eval — `alpha::Engine engine{holdout}.evaluate(prog)` + `extract_streams` +
`compute_metrics` + the DSR calculation — regardless of whether the train metrics make holdout
success plausible. Many candidates are deeply hopeless (train Sharpe << min_dsr threshold by a
large margin). The train metrics (Sharpe, DSR, vol) and the candidate's `trial_count` are
already known from the ranking phase (`factory.cpp:~1082–1133`). A conservative upper bound on
holdout DSR can be derived from train DSR (train DSR is an UNdeflated Sharpe upper bound in
favorable conditions; the holdout is strictly harder). If the upper bound is below `min_dsr`,
the holdout eval can be skipped.

**Fix:**
Add a coarse, conservative pre-gate function:

```cpp
// Returns true if the candidate CAN POSSIBLY clear min_dsr on holdout.
// This is a TRUE UPPER BOUND — if it returns false, the candidate cannot admit.
// Must never return false for a candidate that would have admitted.
bool cascade_gate_passes(const TrainResult& tr, const FactoryConfig& cfg,
                         atx::usize trial_count) noexcept;
```

The bound must be calibrated to be **strictly conservative**:
- It may use `train_metrics.sharpe` (an un-deflated raw upper bound).
- It MUST NOT use train DSR as a direct proxy for holdout DSR (train DSR is deflation-shrunk
  and the holdout may have different statistics).
- A safe bound: `train_metrics.sharpe / sqrt(kAnnualizationDays) >= min_dsr * safety_factor`
  where `safety_factor >= 2.0` (the factor must be tuned so no admitted alpha is dropped on any
  golden config; start at 3.0 and tighten only with evidence).
- The gate is OFF by default (`cascade_gate_factor = 0.0` in `FactoryConfig` → no pre-gate
  applied). It is opt-in, so the off-path is byte-identical with zero behavior change.

Insert the pre-gate check in the serial OOS loop BEFORE the holdout eval block
(`factory.cpp:~1346`), and symmetrically at the parallel-OOS gather sites and both seed
pre-passes. When the gate fires, record the reject as `AdmitKind::RejectFitness` (the same
kind a low-train-DSR candidate would have gotten had it reached holdout) and fold into
histogram + digest — PRESERVING the fold order.

> **Critical invariant:** the pre-gate folds into the digest the SAME `kind` the candidate
> would have gotten on the holdout path. Any candidate that would have admitted MUST NOT be
> gated. Prove this by running the goldens with and without the gate at the tuned factor.

**Determinism:**
- Off-path (default `cascade_gate_factor = 0.0`): byte-identical; gate is never consulted.
- On-path: admitted set must be identical to off-path. Prove via `AdmittedSetUnchanged_AfterCascadeGate`
  test (see §Tests).

**Accept:**
- `FactoryOos.MineIntoOffPathDigestUnchanged` stays green (off-path byte-identical).
- New test `AdmittedSetUnchanged_AfterCascadeGate`: a config with many hopeless candidates,
  gate on; assert admitted set + digest + `reject_histogram` identical to baseline.
- Bench: holdout-eval count per run drops measurably when `cascade_gate_factor` is set; report
  `n_cascade_skipped` in telemetry.

---

### S2-2 — Single-pass sub-window metrics via prefix sums

**Priority:** P5 (perf; active only when `dsr_subwindows >= 2`).

**Root cause:**
The K sub-window DSR loop calls `combine::compute_metrics` once per sub-window per candidate
at four sites:
- Serial OOS loop: `factory.cpp:1434–1435`
- Parallel OOS gather loop: `factory.cpp:2010–2011`
- Serial seed pre-pass A: `factory.cpp:1272`
- Serial seed pre-pass B: `factory.cpp:1864`

Each `compute_metrics` call iterates its sub-span of `hold_pnl` and `hold_pos_flat` to compute
mean, variance, skew, kurtosis (and Sharpe). With K sub-windows the work is O(K · T/K) = O(T)
per candidate in total, but with K separate calls and K separate temporary allocations — plus
repeated floating-point accumulations over the same positions array.

**Fix:**
Replace the K-call pattern with a single prefix-sum pass over the full `hold_pnl`
(post-drop region):

```
For each sub-window [lo, hi):
  mean   = (prefix_sum[hi] - prefix_sum[lo]) / (hi - lo)
  E[X²]  = (prefix_sum_sq[hi] - prefix_sum_sq[lo]) / (hi - lo)
  var    = E[X²] - mean²
  sharpe = mean / sqrt(var)   (annualized by factor)
```

Positions are needed only to pass `n_inst` and `book_size` into `compute_metrics` for the
non-sub-window fields (turnover, capacity). Since the sub-window gate uses only
`per_period_sharpe` (see `factory.cpp:1443`), only the Sharpe / vol moments from `hold_pnl`
are required — the positions-derived fields (`turnover`, capacity) can be computed once on the
full span and shared across sub-windows.

> **Float-order caveat:** prefix-sum subtraction changes the reduction order vs. the current
> per-slice sequential accumulation. If this changes the last ULP and breaks a golden, keep
> the per-slice path; document the tradeoff clearly and leave a `// TODO(S2-2): prefix-sum
> path blocked by ULP order-sensitivity` comment. Do NOT silently diverge from the golden.

**Determinism:**
- When `dsr_subwindows < 2` (the default): code path untouched, byte-identical.
- When `dsr_subwindows >= 2`: result must be bit-identical to the K-call path (verify via
  `SubwindowMetrics_SinglePass_BitIdentical` test). If ULP order changes break the golden, the
  per-slice path is kept and this task is marked partially complete.

**Accept:**
- `SubwindowMetrics_SinglePass_BitIdentical`: a config with `dsr_subwindows = 4`; assert digest
  identical before and after the refactor.
- Bench: sub-window metric cost per candidate drops by the reduction in `compute_metrics` call
  overhead (function call + internal vector setup × K); report before/after ns/candidate.

---

### S2-3 — Collapse the 4× admission ladder into one `admit_on_holdout(...)` helper

**Priority:** C6 (correctness/maintainability — highest structural value; depends on S2-0 for
the hoisted Engine so the helper's signature is stable).

**Root cause:**
The admission ladder (price-scale check → sub-windows → `hold_dsr >= min_dsr && split_ok` →
`try_admit` → histogram increment → digest fold) is copy-pasted at four sites:

| Site | Location in `factory.cpp` |
|---|---|
| Serial OOS loop | ~line 1288 ("3d ADMISSION on the HOLDOUT — IDENTICAL ladder to the ranked loop") |
| Parallel OOS gather loop | ~line 1454 ("3d ADMISSION on the HOLDOUT: clear the factory deflation bar") |
| Serial seed pre-pass A | ~line 1880 ("3d ADMISSION on the HOLDOUT — IDENTICAL ladder to the ranked loop") |
| Parallel seed pre-pass B | ~line 2030 ("3d ADMISSION on the HOLDOUT — IDENTICAL to serial mine_into_oos:813-830") |

The comments at each site say "IDENTICAL" — but nothing prevents them from drifting. The
`seq==parallel` digest invariant and the `serial==seed` invariant are comment-enforced, not
structural. This is the single highest-value correctness change in S2.

**Fix:**
Extract ONE private static helper (or a free function in an anonymous namespace):

```cpp
// admit_on_holdout — the single admission ladder for all four call sites.
//
// Preconditions:
//   - hold_pnl and hold_pos_flat are populated from a completed holdout eval.
//   - hold_metrics is the full-span compute_metrics result.
//   - price_scale_ok and subwindows_ok are already computed (callers own sub-window logic
//     since S2-2 may change it per-site — or pass raw spans and let this helper run it).
//   - hold_dsr and split_ok are the deflated-Sharpe result and the CPCV split flag.
//
// Effects: increments rep.reject_histogram[kind], folds kind into rep.digest,
//          and on Accept calls lib_lib.try_admit and appends to admitted_pnls.
//
// Returns an error if try_admit encounters a geometry mismatch (propagated as ATX_TRY).
[[nodiscard]] static atx::Result<void> admit_on_holdout(
    const atx::f64 hold_dsr,
    bool price_scale_ok,
    bool subwindows_ok,
    bool split_ok,
    atx::core::HashWord canon_hash,
    std::span<const atx::f64> hold_pnl,
    std::span<const atx::f64> hold_pos_flat,
    const combine::AlphaMetrics& hold_metrics,
    library::AlphaProvenance prov,
    FactoryReport& rep,
    library::Library& lib_lib,
    const library::GateConfig& gate,
    std::vector<std::vector<atx::f64>>& admitted_pnls) noexcept;
```

Call this helper from all four sites, replacing each copy-pasted ladder block.

The `seq==parallel` and `serial==seed` invariants then hold by construction: there is only one
ladder, so they cannot drift.

**Determinism:**
Pure structural refactor — the helper executes the identical operations in the identical order.
Output is byte-identical by construction.

**Accept:**
- `AdmitLadder_SharedHelper_SeqEqualsParallel`: after the refactor, the existing `seq==parallel`
  and P-seed/B3 batteries all stay green; run them explicitly.
- `FactoryOos.MineIntoOffPathDigestUnchanged` stays green.
- All four call sites verified to call `admit_on_holdout`; no remnant copy of the ladder body
  remains outside the helper (grep check).

---

## Tests (extend `factory_oos_test.cpp`)

| Test | What it asserts |
|---|---|
| `AdmittedSetUnchanged_AfterCascadeGate` | A config with many low-train-DSR candidates, cascade gate on; admitted set + digest + `reject_histogram` identical to baseline (gate on vs. off). |
| `SubwindowMetrics_SinglePass_BitIdentical` | `dsr_subwindows = 4`; digest identical before and after the prefix-sum refactor. |
| `AdmitLadder_SharedHelper_SeqEqualsParallel` | Post-refactor: re-run all existing `seq==parallel` and P-seed/B3 test cases; assert they pass unchanged. |
| `HoldoutEngineReuse_DigestUnchanged` | Engine-reuse (S2-0): digest unchanged vs. fresh-Engine baseline. |
| `CsePctDenominator_CorrectOverEvaluated` | Inject a config where some genomes fail compile; assert `rep.cse_pct` denominator equals `rep.evaluated`, not the smaller compile-success count. |
| Full `FactoryOos.*` + golden/digest slice | All existing tests stay GREEN as the baseline gate. |

---

## Sequencing

1. **S2-0 first** — hoists the holdout Engine and fixes telemetry. Unblocks S2-1 (the pre-gate
   needs to know the holdout-eval cost it is saving) and S2-3 (the helper's signature is cleaner
   once the hoisted Engine is in place).
2. **S2-2 in parallel with S2-1** — sub-window prefix sums are independent of the cascade gate.
   Both can land after S2-0.
3. **S2-3 last** — the ladder refactor subsumes the admission blocks that S2-1 and S2-2 modify.
   Landing S2-3 last means the helper absorbs the final form of those blocks, not an intermediate
   state. If S2-3 lands before S2-2, the helper will call `compute_metrics` K times; that is
   acceptable — the prefix-sum optimization is retrofittable inside the helper body.

---

## Risks / guardrails

| Risk | Mitigation |
|---|---|
| **Cascade gate drops an admitted candidate.** The safety_factor is set too aggressively. | Start at `safety_factor = 3.0`; require `AdmittedSetUnchanged_AfterCascadeGate` to pass on ALL golden configs before reducing. Never reduce below 1.5. |
| **Prefix-sum ULP drift breaks a golden.** Float subtraction changes reduction order. | If `SubwindowMetrics_SinglePass_BitIdentical` fails on any golden, keep the per-slice path and document; do NOT re-baseline goldens. |
| **Helper refactor subtly reorders the digest fold.** The four sites fold histogram + digest in slightly different order. | Extract the helper with the serial-OOS-loop site as the reference order; verify the other three sites produce the same fold order before and after. Use `AdmitLadder_SharedHelper_SeqEqualsParallel`. |
| **S2-3 interacts with S2-2 landing order.** The helper calls `compute_metrics` K times until S2-2 is in. | Acceptable; S2-2 is an optimization inside the helper body that can land after S2-3. Document in ledger. |
| **Engine-reuse changes behavior if Engine carries state between evals.** | Verify `Engine::evaluate` leaves no mutable state that affects a subsequent call on the same or a different genome. If state exists, add a reset call; confirm byte-identical via `HoldoutEngineReuse_DigestUnchanged`. |
| **`mean_cse_pct` removal breaks a golden that reads `rep.cse_pct`.** | `cse_pct` is telemetry-only and not folded into the digest (`factory.cpp:82–83` confirms). Any golden reading it will need updating, but the ADMISSION digest and admitted set are unaffected. |

---

## Bench / acceptance

- **Holdout-eval count** (S2-0 + S2-1): report `n_holdout_evals_skipped` (cascade gate) and
  confirm Engine-init count drops from `n_ranked` to 1 per invocation. Record before/after
  on a standard deep-run config in the per-unit commit body.
- **Sub-window cost** (S2-2): report before/after ns/candidate on a config with
  `dsr_subwindows = 4`. Use the `bench/` microbench framework.
- **Ladder site count** (S2-3): grep for the ladder pattern; confirm exactly 1 definition
  (`admit_on_holdout`) and 4 call sites.
- **Regression gate (every task):** `FactoryOos.*` green, admission digest unchanged on the
  golden configs, `MineIntoOffPathDigestUnchanged` green.

Every performance claim must be backed by a recorded before/after bench line in the commit body
per `atx-engine/plans/docs/implementation-quality.md`.

---

## Out of scope

Eval-layer kernels (Sprint 1); selection/fitness/search math (Sprint 3); cost-aware gates
(Sprint 4); panel augmentation (Sprint 5); downstream portfolio (Sprint 6); CLI wiring (Sprint 7).
