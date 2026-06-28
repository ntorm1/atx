# Sprint 5 — Conviction-Scaled Sizing

**Goal:** wire the conviction + fractional-Kelly layer into the live atx-impl deploy path and
make the walk-forward OOS estimator honest about the book that actually ships.

Three concrete gaps, each with a confirmed engine root cause:

1. **Kelly wiring absent** — `risk/kelly_sizing.hpp` + `src/risk/kelly_sizing.cpp` ship a
   complete, tested `kelly_size()` (f\* = Σ⁻¹μ, conviction-scaled, gross-clamped) that has
   zero call sites in `atx-impl/`. No `--kelly-fraction` config field exists.
2. **Conviction breakdown not surfaced** — `apply_conviction()` (`stage_combine.cpp:113-172`)
   computes a `ConvictionScore` per alpha (dsr_term, pbo_term, stability_term, score) but
   discards the struct; the KV output has no conviction telemetry. The per-alpha score that
   governs sizing is invisible to the operator.
3. **Walk-forward already conviction-aware; crowding gap is out of scope** — T7 NEW-1
   (`stage_combine.cpp:811-819`) already re-applies `apply_conviction` over each fold's train
   window before scoring, so the WF estimator IS honest about the conviction-weighted book.
   The only WF gap (per the code comment at line 784-787) is crowding-awareness — explicitly
   deferred to S4, which owns `combine/combiner.hpp` + crowding.

**Revised scope (corrected against engine reality):**

| What the brief said | What the audit found | Revised S5 action |
|---|---|---|
| "wire `conviction()` into live path" | Already wired (`cfg.conviction`, merged Jun-22) | Surface per-alpha conviction scores in KV telemetry; emit combo_conviction_scores KV |
| "NEW `risk/kelly_sizing.hpp`" | Already exists in main (`risk/kelly_sizing.hpp` + `.cpp`, ported from s10) | Wire `kelly_size()` into the deploy path via a new `--kelly-fraction` config flag |
| "conviction-aware WF" | Already done (T7 NEW-1, Jun-22) | Add unit test proving the WF fold Sharpe differs from bare-combiner WF on a constructed fixture |

**Owns (exclusive):** `atx-impl/src/config.hpp` (new `kelly_fraction` field),
`atx-impl/src/stage_combine.cpp` (Kelly wiring + conviction-KV emission),
`atx-engine/include/atx/engine/risk/kelly_sizing.hpp` (no structural change; doc-comment only
if needed), `atx-engine/include/atx/engine/eval/regime_slice.hpp` (walk-forward test only),
`atx-engine/tests/risk/kelly_sizing_test.cpp` (NEW — Kelly math tests),
`atx-engine/tests/eval/conviction_wf_test.cpp` (NEW — WF conviction-awareness test),
`atx-impl/tests/conviction_sizing_test.cpp` (NEW — integration test).

**Must NOT touch:** `combine/conviction.hpp` (API frozen; tests green), `risk/kelly_sizing.hpp`
functional body (already correct), `combine/combiner.hpp` (S4 owns), `cost/capacity.hpp` (S4
owns), `eval/regime_slice.hpp` functional body (frozen), `atx-impl/src/stage_run.cpp` +
`stage_report.cpp` + `stage_optimize.cpp` (S7 owns CLI hub; S6 owns downstream stages),
`oracle.hpp` (untouchable by every sprint).

**Coordinate with S4:** S4 owns `capacity.hpp` + `combiner.hpp` + turnover knobs. S5 owns
the conviction + Kelly sizing layer and the WF unit test. The Kelly weights computed here
target a POSITION that the GP optimizer (risk/garleanu_pedersen.hpp) tracks — it is a TARGET
input, not a replacement QP. Do not touch the optimizer.

**Determinism contract (inherited from p6/p7 ROADMAP §Shared determinism contract):**
Every output-changing capability sits behind an engine-config field defaulting to today's
value. The no-flag path stays byte-identical. Each opt-in ships:
(a) off-path byte-identity test,
(b) on-path RED→GREEN test,
(c) twice-run stability.

---

## Implementation-quality handoff block

```text
Implementation quality standard:
Use ats-core/include/ats_orderbook.h as the style reference. Prefer clear module-level
intent, grouped constants/types/APIs, explicit ownership and lifecycle rules, named error
contracts, and concise comments that explain invariants, non-obvious control flow, or domain
semantics. Do not follow weaker patterns that expose constants/structs/prototypes without
enough API contract.

Prioritize full end-to-end implementation over partial stubs. A unit is not done until the
public API, implementation, tests, docs/ledger row, and build/test gate are complete. Do not
leave TODO placeholders, fake success paths, unused APIs, or untested skeletons.

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

## Engine facts (verified, with line citations)

- **`convince()` wired:** `atx-impl/src/stage_combine.cpp:535` — `if (cfg.conviction) { apply_conviction(...) }`. The helper `apply_conviction` at lines `113-172` computes a `ConvictionScore` per alpha but discards all fields except `.score` (used only to scale `weights[a]` at line 168). The breakdown `{dsr_term, pbo_term, stability_term, explain_mult}` is unreported.
- **`cfg.conviction`:** `atx-impl/src/config.hpp:240` — `bool conviction = false; // --conviction`. Parsed at `config.cpp:30`.
- **Walk-forward conviction-aware:** `stage_combine.cpp:811-819` — per-fold `apply_conviction` over `[fit_begin, train_end)` already in place. The comment at line 784-787 explicitly notes the only residual gap is crowding-awareness (S4).
- **`kelly_size()`:** `atx-engine/include/atx/engine/risk/kelly_sizing.hpp:73-76` (declaration); `atx-engine/src/risk/kelly_sizing.cpp:23-79` (body). Algorithm: (1) `f* = V⁻¹μ` via `FactorModel::apply_inverse` (Woodbury, never materializes MxM inverse), (2) `f = kelly_fraction * f*`, (3) `w_i = conviction[i] * f_i`, (4) gross-clamp if `Sum|w| > max_gross > 0`. `KellyConfig{kelly_fraction=0.25, max_gross=1.0}`. Compiled into `atx-engine` static lib. Zero call sites in `atx-impl/`.
- **`KellyConfig` / `KellyWeights`:** `risk/kelly_sizing.hpp:45-57`. `KellyWeights` owns its `VecX` (Rule of Zero). `scale_applied` records the gross-clamp factor (1.0 when slack).
- **`FactorModel::apply_inverse`:** `risk/factor_model.hpp` (declaration); `src/risk/factor_model.cpp` (body). Woodbury apply: O(MK + K³), no MxM materialization. S5 does NOT call this directly — it calls `kelly_size()` which encapsulates it.
- **`walk_forward_sharpe`:** `eval/regime_slice.hpp:304-324` — disjoint rolling-window Sharpes. `RobustnessConfig::n_walk_forward = 4` (line 114). Input is already the firewalled OOS PnL stream; no fitting inside.
- **`walk_forward` config field:** `atx-impl/src/config.hpp:245` — `long walk_forward = 0; // --walk-forward`. Already parsed and exercised in the WF loop at `stage_combine.cpp:793-835`.
- **s10-conviction-regime worktree (unmerged branch `ea59922`):** Shipped S10-0 (scaffold), S10-1 (conviction score), S10-2 (kelly_sizing), S10-3 (regime combiner), S10-4 (crowding/decorrelate), S10-5 (breadth). All six units merged to main. S10-6 (walk-forward adaptation harness) and S10-7 (data-validation gate) were explicitly deferred and lifted to the p2 ROADMAP future-work backlog. The worktree is the source of `kelly_sizing.hpp/.cpp` and `conviction.hpp` that main already carries. No partial Kelly wiring exists anywhere in the worktree's atx-impl layer.

---

## Wiring map

```
atx-impl/src/config.hpp           -- ADD: kelly_fraction (f64, default 0.0 = off)
                                          kelly_max_gross (f64, default 1.0)
atx-impl/src/config.cpp           -- ADD: parse --kelly-fraction, --kelly-max-gross
atx-impl/src/stage_combine.cpp    -- ADD (S5-1): conviction KV emission after apply_conviction
                                  -- ADD (S5-2): kelly_size() call site + kelly_weights KV
atx-engine/include/atx/engine/
  risk/kelly_sizing.hpp            -- NO CHANGE (API frozen and correct)
  eval/regime_slice.hpp            -- NO CHANGE (walk_forward_sharpe frozen and correct)
atx-engine/tests/risk/
  kelly_sizing_test.cpp            -- NEW (S5-0): exact 2×2 and 3×3 hand-solved math tests
atx-engine/tests/eval/
  conviction_wf_test.cpp           -- NEW (S5-3): WF conviction-awareness on constructed fold
atx-impl/tests/
  conviction_sizing_test.cpp       -- NEW (S5-1, S5-2): integration tests for KV + Kelly
```

---

## Tasks

### S5-0 — Open ledger + Kelly math unit tests

**Goal:** Open the sprint ledger (marker commit) and add a self-contained unit test suite
that proves `kelly_size()` computes `f* = Σ⁻¹μ` exactly on hand-solved fixtures. This is
the primary numerical gate for the entire sprint — every subsequent wiring unit depends on
trusting the math.

**Wiring (file:line):**
- NEW `atx-engine/tests/risk/kelly_sizing_test.cpp` — GoogleTest suite `KellySizingMath`.
- `atx-engine/tests/risk/CMakeLists.txt` or equivalent auto-glob — must pick up the new file.
- Read `atx-engine/src/risk/kelly_sizing.cpp:23-79` and `risk/kelly_sizing.hpp:45-76`
  before writing tests; the four-step algorithm (Woodbury → fraction → conviction → clamp)
  is the contract under test.

**Determinism (inert default):** Tests exercise the engine library in isolation — no
`atx-impl` changes, no config changes, no golden-digest impact. Off-path byte-identity is
trivially satisfied (no pipeline code touched).

**Accept:**

(a) **2×2 diagonal covariance, exact hand-solved f\*:**
    ```
    mu = [0.1, 0.2], V = diag(0.01, 0.04) => V^{-1}mu = [10.0, 5.0]
    kelly_fraction=1.0, conviction=[1.0, 1.0], max_gross=0 (disabled)
    => weights = [10.0, 5.0] exactly (within 1e-12)
    ```
    At `kelly_fraction=0.25`: weights = [2.5, 1.25] exactly.

(b) **3×3 diagonal covariance:** extend to three names; verify per-name scaling is
    independent (diagonal V means name i's weight depends only on mu[i] and V[ii]).

(c) **Per-name conviction scaling:** 2×2 case, conviction=[0.5, 1.0] with kelly_fraction=1.0:
    weights = [5.0, 5.0] (name 0 halved, name 1 unchanged). Verify the product
    `conviction[i] * f*[i]` is exact.

(d) **Gross clamp:** 2×2 with kelly_fraction=1.0, conviction=[1,1], max_gross=1.0:
    weights = [10.0, 5.0] * (1.0/15.0) = [0.667, 0.333] (within 1e-12); `scale_applied =
    1.0/15.0`; `gross == 1.0`.

(e) **kelly_fraction=0.0 → zero weights:** all `weights[i] == 0.0` exactly, `gross == 0.0`,
    `scale_applied == 1.0` (clamp not binding). This is the inert-default proof: when the
    caller passes fraction=0, the sizing layer contributes nothing.

(f) **Zero-conviction name → exactly 0 weight:** conviction=[0.0, 1.0]: weights[0] == 0.0
    exactly; weights[1] is unchanged by the conviction scale.

(g) **Twice-run determinism:** call `kelly_size()` twice on the same inputs in the same
    process; results are bit-for-bit identical (no RNG path, no mutable global).

All sub-cases pass; no `atx-impl` code touched; `oracle.hpp` untouched.

---

### S5-1 — Conviction KV telemetry

**Goal:** Surface the per-alpha conviction breakdown in the combine stage's KV output so the
operator can see, for each admitted alpha, what score governed its weight and why (DSR term,
stability term, overall score). Currently `apply_conviction` at `stage_combine.cpp:168`
discards the `ConvictionScore` struct after using only `.score`.

**Wiring (file:line):**
- `atx-impl/src/stage_combine.cpp:113-172` — modify `apply_conviction` to accumulate the
  per-alpha `ConvictionScore` structs into a caller-supplied output vector (e.g.,
  `std::vector<combine::ConvictionScore>& out_scores`).
- `atx-impl/src/stage_combine.cpp:535-542` — at the `if (cfg.conviction)` call site, pass
  a `std::vector<combine::ConvictionScore> conviction_scores` collector; after the call,
  emit the scores into `sr.kvs` as additive KVs.
- New KVs (additive, only present when `cfg.conviction == true`):
  - `conviction_scores` — comma-joined per-alpha final scores in AlphaId order,
    e.g. `"0.712,0.543,0.891"`.
  - `conviction_dsr_terms` — comma-joined clamped DSR terms.
  - `conviction_stability_terms` — comma-joined clamped stability terms.
  These mirror the pattern of `capacity_alpha_aum` at `stage_combine.cpp:880`
  (additive, off by default, byte-identical on the default path).
- `atx-impl/tests/conviction_sizing_test.cpp` — new test file; see Accept below.

**Determinism (inert default):** When `cfg.conviction == false` (the default), the
`apply_conviction` signature change is invisible — the output vector is not passed / is
empty, no new KVs are emitted. Default-path KV set is byte-identical to today. The new KVs
are strictly additive (no removal of existing keys).

**Accept:**

(a) **Off-path byte-identity:** a test constructs a `RunConfig` with `conviction = false`,
    runs `stage_combine` on a small synthetic pool, and asserts that the KV set contains
    none of `conviction_scores`, `conviction_dsr_terms`, `conviction_stability_terms`.

(b) **On-path KV presence:** with `conviction = true`, the three new KVs are present; the
    number of comma-separated entries in `conviction_scores` equals `pool.n_alphas()`.

(c) **Score bounds:** every parsed score in `conviction_scores` is in [0, 1]. Every parsed
    DSR term is in [0, 1]. Every stability term is in [0, 1]. (Guaranteed by the conviction
    math; the test proves no serialization error breaks this.)

(d) **Score matches formula at known inputs:** construct a synthetic alpha with known PnL
    such that DSR is calculable; verify the emitted score matches the hand-computed
    `w_dsr*dsr_term + w_stability*stab_term` (with w_pbo=0 per the existing `apply_conviction`
    convention, which drops PBO as a per-alpha-level input since PBO is a per-run set statistic).

(e) **Twice-run:** identical KV output on two consecutive calls with the same inputs.

---

### S5-2 — Fractional-Kelly wiring into atx-impl

**Goal:** Wire `kelly_size()` into the combine stage as an opt-in post-conviction,
pre-report sizing layer. When `--kelly-fraction > 0`, the combined weights (post-conviction
if `--conviction` is also set) are replaced by the Kelly-sized target weights.

This is the load-bearing unit: it makes the engine produce a covariance-aware, fractional-Kelly,
conviction-scaled position target instead of the naive renormalized combiner weights.

**Wiring (file:line):**
- `atx-impl/src/config.hpp:240` (after `conviction` field) — ADD:
  ```cpp
  // --kelly-fraction (S5-2, opt-in): fractional-Kelly conviction-scaled sizing.
  // f* = V^{-1}mu via FactorModel::apply_inverse; scaled by kelly_fraction then
  // per-name conviction. Default 0.0 = off (byte-identical to today). Requires
  // a fitted FactorModel (--factor-model or the engine default) and --conviction
  // scores as the per-name conviction vector. See risk/kelly_sizing.hpp.
  double      kelly_fraction = 0.0;  // --kelly-fraction (0.0 = off)
  double      kelly_max_gross = 1.0; // --kelly-max-gross
  ```
- `atx-impl/src/config.cpp` — parse `--kelly-fraction <f64>` and `--kelly-max-gross <f64>`.
- `atx-impl/src/stage_combine.cpp` — AFTER the `if (cfg.conviction)` block (line 543) and
  BEFORE the crowding block (line 562), add:
  ```cpp
  // S5-2: opt-in fractional-Kelly sizing. Replaces the combiner's renormalized
  // weights with a covariance-aware, conviction-scaled Kelly target. Off (fraction=0)
  // => byte-identical to the existing post-conviction weights. Requires:
  //   * a FactorModel built from the panel (same panel the combiner sees).
  //   * per-name conviction vector: the S5-1 scores mapped to pool names.
  //   * cfg.kelly_fraction > 0 (the gate; 0.0 is the inert default).
  ```
  The `FactorModel` is constructed from the panel using `FactorModel::fit(panel, fit_begin,
  fit_end)` (the same window the combiner fit on). The per-name conviction vector is derived
  from the per-alpha conviction scores (one score per alpha, broadcast to the names that alpha
  contributes to, or averaged if multiple alphas target the same name).
  `expected_alpha` is the combined mega-alpha return prediction over the test period (the
  per-period mean return of each name's blended weight, computed from `pool.pnl` and
  `combo.weights`).
  After `kelly_size()` returns, replace `combo.weights` with the Kelly weights (ONLY when
  `kelly_fraction > 0`). Add KV telemetry:
  - `kelly_fraction_used` — the `cfg.kelly_fraction` value applied.
  - `kelly_gross` — `KellyWeights::gross` (the realized `Sum|w|`).
  - `kelly_scale_applied` — `KellyWeights::scale_applied` (1.0 when gross clamp not binding).
- New includes in `stage_combine.cpp`:
  ```cpp
  #include "atx/engine/risk/kelly_sizing.hpp"   // risk::kelly_size, KellyConfig
  #include "atx/engine/risk/factor_model.hpp"    // risk::FactorModel
  ```
  (Both are already compiled into `atx-engine`; no CMakeLists change needed.)

**Determinism (inert default):** When `cfg.kelly_fraction == 0.0` (the default), the entire
Kelly block is skipped. `combo.weights` is untouched. `sr.digest` is computed from the
unchanged `combo.bin`. All three new KVs are absent. The output is byte-identical to today.
The Kelly block runs ONLY inside `if (cfg.kelly_fraction > 0.0)`.

**Accept:**

(a) **Off-path byte-identity:** `kelly_fraction = 0.0` (default) → `stage_combine` output
    digest unchanged; `kelly_fraction_used`, `kelly_gross`, `kelly_scale_applied` absent from
    KVs.

(b) **On-path weights change:** with `kelly_fraction = 0.25` and a synthetic pool where the
    hand-solved Kelly weights differ from the renormalized combiner weights (e.g., two
    alphas with different edge/variance ratios), the output `combo.weights` differs from the
    no-Kelly run.

(c) **Gross is bounded:** with `kelly_max_gross = 1.0` (default), `kelly_gross <= 1.0`
    always; `kelly_scale_applied < 1.0` when the unclamped gross exceeds 1.0.

(d) **kelly_fraction × conviction interaction:** with both `--conviction` and
    `--kelly-fraction 0.25` active, a zero-conviction alpha gets exactly 0.0 Kelly weight
    (the `conviction[i] * f*[i]` product propagates the zero exactly).

(e) **Twice-run determinism:** same inputs → byte-identical weights, digest, and KVs.

---

### S5-3 — Walk-forward conviction-awareness unit test

**Goal:** Add a focused unit test that proves the WF fold Sharpe DOES differ from the
bare-combiner WF Sharpe on a constructed fixture where conviction-weighting shifts the
blend. This is a correctness gate for T7 NEW-1, which already exists in code but has no
dedicated test proving the interaction.

The test exercises `eval/regime_slice.hpp:walk_forward_sharpe` and the `apply_conviction`
path in `stage_combine.cpp:811-819` at the unit level: it does NOT run the full pipeline.

**Wiring (file:line):**
- NEW `atx-engine/tests/eval/conviction_wf_test.cpp` — GoogleTest suite `ConvictionWF`.
- Depends only on `eval/regime_slice.hpp:walk_forward_sharpe` (header-only, no link
  dependency beyond `atx-engine-eval-tests`).
- Also exercises `combine/conviction.hpp:conviction()` directly to construct
  conviction-scaled weights and verify the per-fold score moves.

**Constructed fixture:**

Build a pool of two synthetic alphas over T=60 periods with disjoint non-overlapping
return profiles:
- Alpha A: positive PnL in periods 0-29, zero in 30-59. DSR > 0, stability ratio < 1
  (first half good, second half flat). conviction(A) ≈ 0.4.
- Alpha B: zero PnL in 0-29, positive in 30-59. DSR > 0, stability ratio < 1 (reverse).
  conviction(B) ≈ 0.4.

A naive equal-weight combiner assigns w=[0.5, 0.5]. The conviction transform scales both
down similarly, so the WEIGHTS change but the PnL blend is similar. However, a 2-fold WF:
- Fold 1 train=[0,30), test=[30,60): Alpha A trains well, B trains poorly → combiner
  assigns more weight to A. After conviction on train window [0,30): A's stability ratio
  is high (first half good = both halves of [0,30) are good? no — use a more asymmetric
  design). Use:
  - Alpha A: periods 0-44 positive, 45-59 negative. In WF fold 1 train=[0,30) A is all
    positive → high conviction. Fold 1 test=[30,60): A is positive 30-44, negative 45-59.
  - Alpha B: periods 0-44 zero, 45-59 strongly positive. In fold 1 train=[0,30) B is zero
    → combiner assigns low weight. After conviction: B's score is low (no edge in train).
    Fold 1 test=[30,60): B is positive 45-59 → BUT combiner doesn't know this yet.

The key assertion: compute the fold 1 OOS Sharpe with conviction ON vs. OFF. They must
differ (the conviction transform changes the weights, which changes the blended PnL, which
changes the fold Sharpe). The direction is: with conviction ON, the over-weighted A (which
has worse test performance 45-59) gets more weight → slightly worse OOS Sharpe in fold 1.
The test asserts `abs(sharpe_conviction_on - sharpe_conviction_off) > 1e-6` (the two are
NOT equal), which is the core proof that conviction modulates the WF score.

**Determinism (inert default):** This test only exercises engine headers, not `atx-impl`.
No pipeline changes. No digest impact.

**Accept:**

(a) **WF differs:** `sharpe_wf_conviction_on != sharpe_wf_conviction_off` on the
    constructed fixture (differs by more than 1e-6 in absolute value, in the
    constructed direction).

(b) **WF conviction-off matches bare-combiner WF:** with conviction=false, the fold Sharpe
    matches the bare-combiner fold Sharpe exactly (bit-for-bit), proving the conviction
    path is truly opt-in.

(c) **Twice-run:** both versions are byte-identical on two consecutive calls.

(d) **n_windows=1 edge case:** with n_windows=1, walk_forward_sharpe returns a single
    window = the full-sample Sharpe; conviction scaling changes it (or does not) per the
    math — assert it is at least finite and in a reasonable range.

---

### S5-4 — Integration smoke + off-path byte-identity

**Goal:** Add an `atx-impl` integration test that exercises all three new knobs together
(`--conviction`, `--kelly-fraction`, `--walk-forward`) and proves that with all knobs OFF
the output digest is unchanged from the no-knob baseline.

**Wiring (file:line):**
- `atx-impl/tests/conviction_sizing_test.cpp` — extend (from S5-1) with the integration
  cases below.
- Constructs `StageInputs` directly (the same pattern as S6-3's e2e test in p6), NOT via
  `run_all` or `stage_run`.
- Small synthetic pool: ≥3 alphas, ≥40 periods, ≥20 instruments.

**Determinism (inert default):** The off-path test (no flags) is the primary regression
guard for the entire sprint.

**Accept:**

(a) **Off-path byte-identity (the mandatory class-a test):**
    Run `stage_combine` twice with a fixed synthetic pool and `conviction=false`,
    `kelly_fraction=0.0`, `walk_forward=0`. Assert the two output digests are identical
    and that neither `conviction_scores` nor `kelly_gross` nor `walk_forward_oos_sharpe`
    appear in the KV set.

(b) **Conviction-only:** `conviction=true`, `kelly_fraction=0.0` → `conviction_scores` KV
    is present; `kelly_gross` KV is absent; digest differs from the off-path baseline
    (weights changed). Twice-run identical.

(c) **Kelly-only (no prior conviction):** `conviction=false`, `kelly_fraction=0.25` →
    `kelly_gross` KV is present; `conviction_scores` KV is absent; Kelly weights are
    applied (digest differs from baseline). Twice-run identical.

(d) **Combined:** `conviction=true`, `kelly_fraction=0.25` → both KV families present;
    the zero-conviction-alpha has exactly 0.0 Kelly weight; `kelly_gross <= 1.0`;
    twice-run identical.

(e) **WF + conviction:** `conviction=true`, `walk_forward=2` → `walk_forward_oos_sharpe`
    KV present; WF Sharpe is finite; twice-run identical. (This validates the T7 NEW-1
    path end-to-end in atx-impl.)

---

## Sequencing

1. **S5-0** (Kelly math tests) — first; no code changes, establishes the numerical
   contract before any wiring lands.
2. **S5-1** (conviction KV telemetry) — independent of S5-0 in code; can follow
   immediately. Modifies `apply_conviction` signature and KV emission only.
3. **S5-2** (Kelly wiring) — depends on S5-0 (trust the math) and S5-1 (conviction
   scores needed as the per-name conviction vector). Implement after both are green.
4. **S5-3** (WF unit test) — independent of S5-1/S5-2; can run in parallel with S5-1.
   Exercises engine headers only.
5. **S5-4** (integration smoke) — final; composes after S5-1 and S5-2 are in and green.

Each unit follows the marker-commit pattern: code + tests + ledger row in the same commit.
Marker commit lands before S5-0 code; close commit after S5-4 is green.

---

## Risks / guardrails

**FactorModel construction in stage_combine.** `kelly_size()` requires a fitted
`FactorModel`. The combine stage currently has a `Panel` and the pool's PnL streams but
does not fit a factor model. The simplest safe path is a **diagonal FactorModel** fit
directly from per-name return variance over `[fit_begin, fit_end)` — this is O(M) and
requires no new dependency (the cross-sectional return variance is already computable from
the panel's `close` field). A full statistical factor model (BARRA-style) is S4's territory
and is NOT in scope for S5. The plan text notes this explicitly so implementers do not
over-engineer: a diagonal covariance with per-name realized variance is the correct
minimal-scope choice for S5, and `kelly_size()` is agnostic to how rich the `FactorModel`
is (Woodbury works on any factored form, including the degenerate K=0 diagonal case).

**Per-name conviction mapping.** The `conviction_scores` vector from S5-1 is per-ALPHA
(one score per admitted alpha). `kelly_size()` takes a per-NAME (per-instrument) conviction
vector of length M. The mapping: for each instrument j, the per-name conviction is the
conviction-weighted average of the scores of all alphas that have a nonzero weight on name
j. In the common single-alpha case this is just `score[0]`. In the multi-alpha case it is
`Σ_a |combo.weights[a]| * conviction_scores[a]` normalized, or simply the max score across
contributing alphas — implementers must pick one convention, document it in a comment at the
call site, and pin it in a test. The plan recommends the `|combo.weights[a]|`-weighted
average as it is consistent with the combine step.

**Kelly weights replace combine weights only in the deploy book, not in the WF scoring.**
The WF folds in `stage_combine.cpp:793-835` use scratch `wf_combo.weights` that are
discarded after scoring. The Kelly sizing (S5-2) modifies `combo.weights` AFTER the WF
loop. Implementers must verify the sequencing: WF loop first, then conviction (already
sequenced), then Kelly. This mirrors the comment at `stage_combine.cpp:780-787` (WF scores
the conviction-weighted book, not the Kelly-weighted book). Making WF Kelly-aware is
explicitly out of scope for S5 — it would require fitting a FactorModel inside each fold,
which is a non-trivial cost. The KV comment should record this caveat.

**`cfg.conviction` vs. `cfg.kelly_fraction` interaction.** The Kelly layer is independent
of the conviction flag: `kelly_size()` accepts a conviction vector that the caller
constructs. When `--conviction` is OFF and `--kelly-fraction > 0`, the caller must supply a
uniform all-1.0 conviction vector (full-conviction for every name), so the Kelly sizing
operates without any conviction haircut. Document this interaction at the call site.

**Digest impact.** `combo.bin` contains the fitted weights. When `kelly_fraction > 0`, the
weights change, so the digest changes — this is correct and expected (the deploy book is
different). The off-path digest (fraction=0) must remain unchanged: the Kelly block must be
fully inside `if (cfg.kelly_fraction > 0.0)` with no side-channel writes outside the block.

**`oracle.hpp` is untouchable.** No sprint touches it. If a test relies on a golden digest
that oracle.hpp pins, that test must use the no-flag path.

---

## Bench / acceptance

| Criterion | Target | Evidence |
|---|---|---|
| Kelly f\* = Σ⁻¹μ exact (2×2 diagonal) | weights match hand-solved to 1e-12 | S5-0 case (a) |
| Kelly f\* = Σ⁻¹μ exact (3×3 diagonal) | weights match hand-solved to 1e-12 | S5-0 case (b) |
| Per-name conviction scaling exact | `conviction[i] * f*[i]` exact to 1e-12 | S5-0 case (c) |
| Gross clamp correct | `Sum|w| == max_gross` when binding, `scale_applied < 1.0` | S5-0 case (d) |
| kelly_fraction=0 → zero weights (inert) | all `weights[i] == 0.0` exactly | S5-0 case (e) |
| Zero-conviction name → exactly 0 weight | `weights[0] == 0.0` exactly | S5-0 case (f) |
| Conviction KV present when on | `conviction_scores` in KV set, len == n_alphas | S5-1 case (b) |
| Conviction KV absent when off | none of the three conviction KVs present | S5-1 case (a) |
| Kelly KVs present when on | `kelly_fraction_used`, `kelly_gross`, `kelly_scale_applied` in KV | S5-2 case (b) |
| Off-path byte-identity (conviction=false, kelly=0) | digest unchanged from baseline | S5-4 case (a) |
| WF + conviction ON differs from WF bare | `|sharpe_on - sharpe_off| > 1e-6` | S5-3 case (a) |
| WF + conviction OFF = bare-combiner WF | bit-for-bit identical | S5-3 case (b) |
| Combined knobs (conviction + kelly) | both KV families present; zero-conviction name = 0 Kelly weight | S5-4 case (d) |
| Twice-run determinism (all units) | identical output on two consecutive runs | S5-0(g), S5-1(e), S5-2(e), S5-3(c), S5-4 each case |
| `oracle.hpp` untouched | diff is empty | reviewer gate |
| Engine libs untouched (conviction.hpp, kelly_sizing.hpp body) | diff is empty | reviewer gate |

All tests in all five units pass before S5 is marked complete. No production run required:
every claim is proven on small deterministic synthetic fixtures (≤3 alphas, ≤60 periods,
≤20 instruments) constructed inside the test bodies. The dev-panel smoke (`-Profile smoke`)
is the integration gate for the full pipeline and is NOT a sprint-level requirement.

---

## Out of scope

- WF Kelly-awareness (making WF folds use Kelly weights): requires fitting a FactorModel
  inside each fold; deferred. The code comment at `stage_combine.cpp:784-787` already notes
  the WF measures the conviction-weighted but NOT Kelly-weighted book.
- WF crowding-awareness: S4 owns `combiner.hpp`/crowding; explicitly excluded from S5.
- Full statistical factor model (BARRA-style K-factor): S5 uses the diagonal form.
  A rich factor model is S4 territory or a future sprint.
- `report_aum` defaults, CLI hub threading, `stage_run.cpp` changes: S7 owns these.
- `regime_slice.hpp:robustness_verdict` structural changes: header is frozen; S5 adds
  tests, not structural changes.
- Breadth report wiring (deferred from S10-5): the S10-5 ledger entry notes that
  `effective_breadth()` wiring into `stage_report.cpp` was deferred because the stage
  lacks a per-alpha-per-name PnL covariance source. That remains deferred; S5 does not
  manufacture a covariance wiring path for breadth.
