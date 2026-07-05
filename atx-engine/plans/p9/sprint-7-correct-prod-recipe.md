# Sprint 7 — Correct the Prod Recipe (V1-Ready)

**Goal:** make `atx-impl/scripts/build-megaalpha-book.ps1 -Profile prod` actually exercise every
lever S1–S6 built. Enable the now-LIVE p9 flags on the stage that *really* runs them (not the one
that used to, or the one the doc-comment claims does); drop/annotate the flags that stay no-ops
after p9; update the Pester DryRun argv assertions to match. **DryRun-only — this sprint never
invokes the binary or runs the real full-panel V1.** The V1 run stays the operator's step.

**Owns (exclusive):**
`atx-impl/scripts/build-megaalpha-book.ps1`, `atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1`.

**Must NOT touch:** every C++ root S1–S6 own (`stage_optimize.cpp`, `stage_combine.cpp`,
`stage_metabook.cpp`, `stage_riskmodel.*`, `config.{hpp,cpp}`, `factory/fitness.hpp`,
`factory/search_driver.*`, `risk/optimizer.hpp`, `loop/*`, `learn/*`, `fund/*`) — S7 assumes their
flags exist and behave as the ROADMAP registry documents; it does not re-verify or extend their
internals. Also do not touch `atx-impl/research/2026-07-03-megaalpha-book-results.md` (the V1
scorecard template) beyond *reading* it for the runbook cross-reference — filling it in is the
operator's V1 step, not this sprint's.

---

## Implementation-quality handoff block (paste verbatim into every subagent brief)

```text
Implementation quality standard:
Use the surrounding engine headers as the style reference. Prefer clear module-level intent,
grouped constants/types/APIs, explicit ownership and lifecycle rules, named error contracts, and
concise comments that explain invariants, non-obvious control flow, or domain semantics.

Prioritize full end-to-end implementation over partial stubs. A unit is not done until the public
API, implementation, tests, docs/ledger row, and build/test gate are complete. Do not leave TODO
placeholders, fake success paths, unused APIs, or untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership, ordering, and tricky
domain rules. Do not comment obvious assignments.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and lifecycle.
- Names are domain-accurate and consistent with nearby engine code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new abstractions.
```

---

## The current prod argv — EXACTLY, as the script emits it TODAY (code-confirmed)

Default invocation `build-megaalpha-book.ps1 -Profile prod` (i.e. `-Stage @('all')`, all other
params at their defaults: `$WorkDir='work\megaalpha-build'`, `$PanelBin='work\accept\panel.bin'`,
`$SeedFile='atx-impl\tests\fixtures\alpha101.txt'`, `$Workers=0`, `$CostBps=10`,
`$SelectionAum=5.0e7`) expands `'all'` to `discover,combine,metabook,optimize,report`
(`build-megaalpha-book.ps1:285-294`), then applies the metabook/optimize mutual-exclusion filter
(`:300-305`):

```powershell
$useMetabook = ($Profile -eq 'prod')
$activeStages = $activeStages | Where-Object {
    if ($_ -eq 'metabook') { $useMetabook }
    elseif ($_ -eq 'optimize') { -not $useMetabook }
    else { $true }
}
```

Because `$useMetabook` depends only on `$Profile`, **`optimize` is unconditionally dropped whenever
`-Profile prod`, regardless of what `-Stage` actually asked for.** For the default `-Stage all`
invocation this yields exactly four subprocess calls — `discover → combine → metabook → report`:

```
discover --panel work\accept\panel.bin --seed-file atx-impl\tests\fixtures\alpha101.txt --gated \
  --library-dir work\megaalpha-build\_library --max-pbo 0.5 --min-dsr 0.5 --min-sharpe 0.25 \
  --min-fitness 1.0 --max-turnover 0.50 --oos-fraction 0.25 --alpha-out work\megaalpha-build\alphas \
  --population 300 --generations 15 --impact-in-selection --selection-aum 50000000 \
  --require-split-stable --blocking-pbo

combine --panel work\accept\panel.bin --library-dir work\megaalpha-build\_library \
  --combo-out work\megaalpha-build\combo.bin --holdout-frac 0.25 --method stack

metabook --panel work\accept\panel.bin --combo work\megaalpha-build\combo.bin \
  --library-dir work\megaalpha-build\_library --books-out work\megaalpha-build\books.bin \
  --sleeve-method hrp

report --panel work\accept\panel.bin --books work\megaalpha-build\books.bin \
  --combo work\megaalpha-build\combo.bin --report-out work\megaalpha-build\report --capacity-curve
```

**The load-bearing discovery this sprint's rationale rests on:** `New-OptimizeArgv`'s prod branch
(`build-megaalpha-book.ps1:348-355`) already composes `--risk-model factor --dead-alpha-factors
--group-neutralize`:

```powershell
'optimize' {
    if ($Profile -eq 'prod') {
        $argv = New-OptimizeArgv -PanelBin $PanelBin -WorkDir $WorkDir -CostBps $CostBps `
            -RiskModel 'factor' -DeadAlphaFactors -GroupNeutralize
    } else { ... }
    Invoke-Stage 'optimize' $argv
}
```

but this `switch` arm is **dead code** for `-Stage all`/`-Stage pipeline -Profile prod` — the
`foreach ($s in $activeStages)` loop (`:324`) never iterates `'optimize'` because the filter above
already removed it from `$activeStages` before the loop runs. `atx-impl/research/2026-07-03-megaalpha-book-results.md:108,116-118` (the p8 V1 scorecard template) documents this exact
misunderstanding as settled fact — "`-Profile prod` already folds the risk-model knobs
(`--risk-model factor --dead-alpha-factors --group-neutralize`) into the `discover`+`optimize`
stages' own argv" and "the harness auto-excludes optimize so books.bin is written exactly once" —
without noticing that auto-excluding `optimize` also permanently excludes the only argv that ever
carried the risk-model flags. **So even after S1/S2 land, the documented prod recipe would still be
a Potemkin book**: the risk-model CLI surface would finally *work*, but the script would still never
hand it to a stage that runs.

| Prod flag (as the OLD script emits it) | Reality after S1–S6 land | Why still broken |
|---|---|---|
| `--risk-model factor --dead-alpha-factors --group-neutralize` | Live in the engine (S1/S2) | Attached only to `New-OptimizeArgv`'s prod branch; `optimize` never executes under `-Stage all\|pipeline -Profile prod` (`:300-305` routing) |
| `--capacity-curve` | Still a dead marker (no p9 sprint fixes `stage_report.cpp`'s `has_volume && report_aum>0` unconditional compute) | Not an S1–S6 target; annotate, don't drop |
| *(absent)* `--gp-trading` | Live in the engine (S3) at `stage_optimize.cpp`'s position-mode blend | Never emitted at all today; and even once added, only bites via `optimize`, which is routed out |
| *(absent)* `--capacity-objective`/`--turnover-objective` | Live (S4) in `stage_discover.cpp`'s factory search | Never emitted |
| *(absent)* `--book-turnover-gate`/`--participation-cap`/`--borrow-bps`/`--robustness-*` | Live (S5) | Never emitted; `--robustness-battery` itself (pre-existing, p8) is never emitted either |
| *(absent)* `--deflate-selection` | Live since p8 (R4), fully wired to `stage_discover.cpp:600,1038` | Simply never turned on in the prod recipe — a pure recipe omission, not an engine gap |
| `$AtxExe` default `C:\atx-wt\p8\build\bin\atx-impl.exe` | This script now lives in the **p9** worktree (`C:\atx-wt\p9`) | An operator who runs the documented example without `-AtxExe` silently invokes the STALE p8 binary — none of S1–S6's new flags would even parse |

---

## Architecture note — why the fixes land on `combine`/`metabook`, not `optimize`

Grounding (all read directly from the p9 worktree's current `atx-impl/src`, pre-S1–S6):

- `atx-impl/src/dispatch.cpp:102,103,108` — the CLI subcommand router: `"combine" → run_combine(cfg)`,
  `"optimize" → run_optimize(cfg)`, `"metabook" → run_metabook(cfg)`. Each subcommand is a **separate
  OS process** in this harness (`Invoke-Stage` calls `& $AtxExe @Argv` once per stage) — nothing
  carries over between them except what a later stage's argv explicitly re-supplies (e.g.
  `--library-dir`, `--combo`).
- `atx-impl/src/config.cpp` parses `--risk-model` (:131-138), `--dead-alpha-factors` (:44),
  `--group-neutralize` (:45) into flat `RunConfig` fields (`config.hpp:307-309`) with **no
  subcommand restriction** — any subcommand's invocation can carry them, they are simply unread by
  stages that don't consume them yet.
- `atx-impl/src/stage_optimize.cpp:40-48` — `run_optimize(cfg)` (the CLI entry) **already** builds a
  `risk::RiskModelConfig` from `cfg.risk_model`/`cfg.dead_alpha_factors`/`cfg.group_neutralize` and
  forwards to the 2-arg overload. This is the *only* CLI entry point in the current tree that reads
  these fields at all.
- `atx-impl/src/stage_optimize.cpp:251-273` — the MVO path's `build_risk_model(...)` calls (Diagonal
  branch and the per-step Factor branch) still pass `dead_lib=nullptr`/`dead_ids={}` literally — the
  exact no-op the ROADMAP's Potemkin table cites at `stage_optimize.cpp:260,267`. S1 fixes this by
  threading a real `library::Library*`, sourced from a **new** `--dead-alpha-lib-dir` (since, per the
  one-process-per-stage model above, whichever stage calls `build_risk_model` needs its own explicit
  path — there is no ambient shared library handle across subprocess boundaries).
- `atx-impl/src/stage_combine.cpp:760-772` — the 0-arg and 2-arg `run_combine` overloads (the ones
  `dispatch.cpp:102`'s `"combine"` subcommand and `run_all`'s internal call actually use) hardcode
  `risk::RiskModelConfig{}` (Diagonal). The 3-arg overload (`:774-781`) already *consumes* `risk_cfg`
  at two internal sites (the ShrinkageMv cleaned-covariance fit and the breadth-instrumentation
  covariance) — the wiring exists, only the **CLI entry** needs to stop hardcoding Diagonal. This is
  S2's job (mirror `stage_optimize.cpp:40-48`'s pattern: build `risk_cfg` from `cfg.risk_model`, call
  the 3-arg overload).
- `atx-impl/src/stage_run.cpp:34-38` — the CLI entry `run_metabook(cfg)` (routed by
  `dispatch.cpp:108`) builds a `MetaBookStageConfig` from `cfg.sleeve_method` **only** — it never
  reads `cfg.risk_model`/`dead_alpha_factors`/`group_neutralize` at all, and
  `stage_metabook.hpp:81-95`'s `MetaBookStageConfig` has no `RiskModelConfig` field to receive them.
  This is the literal "`stage_metabook` has no `RiskModelConfig` parameter at all" gap the ROADMAP
  names for S2 — the same-shaped fix as `stage_combine`'s (mirror `stage_optimize.cpp:40-48` again).
- `atx-impl/src/stage_metabook.cpp:62` — `mh.trade_rate = 1.0;` is metabook's **own**, separate
  turnover-control hardcode (feeding `risk::multi_period`'s per-sleeve construction) — it is a
  different mechanism from `stage_optimize.cpp:191`'s manual `w := prev + trade_rate*(target-prev)`
  blend that S3's `--gp-trading` targets. **S3's ROADMAP roots name `stage_optimize.cpp` only** — it
  does not touch this hardcode. See the GP-trading row in the rationale table below for the
  consequence.

**Net effect for the script:** since the harness's prod pipeline uses `metabook` (not `optimize`) as
its book-construction stage, S7's corrected recipe must attach `--risk-model factor
--dead-alpha-factors --dead-alpha-lib-dir --group-neutralize` (S1/S2) and `--book-turnover-gate
--participation-cap` (S5, whose roots explicitly list *both* `stage_metabook.cpp` and
`stage_optimize.cpp`) to **`New-MetabookArgv`**, and `--risk-model factor` alone (S2's combine-side
seam) to **`New-CombineArgv`**. `New-OptimizeArgv` keeps its own corrected argv (useful for
`-Profile smoke`, and for an operator who explicitly wants the single-blend MVO book instead of the
sleeve/metabook one) but its prod-shaped invocation is only ever reachable once S7-0 fixes the
stage-routing bug below.

---

## Determinism contract (Sprint 7)

DryRun-only. No behavior of the *engine* changes — this sprint edits argv-composition PowerShell
only. "Determinism" here means: **the same inputs to a `New-*Argv` function always produce the same
`[string[]]` array**, and the smoke profile's argv is unchanged bit-for-bit except where explicitly
noted (the `Resolve-ActiveStages` extraction in S7-0 must reproduce today's smoke routing exactly —
smoke was never affected by the prod-only bug, and stays that way). No pinned engine golden is
touched by this sprint; the four class-(a)-(d) test taxonomy from the ROADMAP does not apply here
(there is no opt-in *engine* field — S7 adds zero new `RunConfig`/engine fields, per the SHARED
CONFIG-FIELD REGISTRY's S7 row: "no new fields; recipe wiring only").

**Commits:** stage explicit paths (never `git add -A`); never push; trailer exactly
`Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

## Dependency / wiring map (flag → stage → engine file:line, corrected)

```
--dead-alpha-lib-dir <dir>         → New-MetabookArgv (+ New-OptimizeArgv)  ← S1 (stage_riskmodel's
                                      build_risk_model dead_lib param; stage_optimize.cpp:260,267)
--dead-alpha-factors               → New-MetabookArgv (+ New-OptimizeArgv)  ← S1 (RunConfig, existing
                                      config.cpp:44)
--group-neutralize                 → New-MetabookArgv (+ New-OptimizeArgv)  ← S1 (RunConfig, existing
                                      config.cpp:45)
--risk-model factor                → New-CombineArgv, New-MetabookArgv,
                                      New-OptimizeArgv                      ← S2 (stage_combine.cpp:
                                      760-781, stage_run.cpp:34-38 CLI entries; existing
                                      config.cpp:131-138)
--gp-trading / --gp-risk-aversion /
--gp-trade-cost-scale               → New-OptimizeArgv ONLY                 ← S3 (stage_optimize.cpp:
                                      191 position-mode blend). NOT reachable via metabook (S3 roots
                                      exclude stage_metabook.cpp; see Architecture note).
--capacity-objective /
--turnover-objective                → New-DiscoverArgv                      ← S4 (factory/fitness.hpp
                                      kMaxObjectives 7→9, :183,294; factory/search_driver.*)
--book-turnover-gate /
--participation-cap                 → New-MetabookArgv (+ New-OptimizeArgv) ← S5 (stage_metabook.cpp /
                                      stage_optimize.cpp book-turnover measure; risk/optimizer.hpp QP
                                      participation cap)
--borrow-bps                        → New-ReportArgv                        ← S5 (loop/backtest_loop.hpp,
                                      cost/borrow.hpp — the honest book-level cost numbers stage_report.cpp
                                      already assembles at :644-777 alongside capacity_point_aum)
--robustness-battery (existing) /
--robustness-sub-universe /
--robustness-alt-neutralization /
--robustness-param-perturb          → New-DiscoverArgv                      ← p8 (robustness_battery,
                                      config.cpp:52, stage_discover.cpp:597) + S5 (the 3 new checks,
                                      factory/factory.cpp battery surface)
--deflate-selection (existing)      → New-DiscoverArgv                      ← p8/R4 (config.cpp:40,
                                      stage_discover.cpp:600,1038 — already fully wired, just never
                                      turned on in the prod recipe)
--ml-seeds / --ml-seed-model-dir /
--sleeve-method nco                 → New-DiscoverArgv / New-MetabookArgv,
                                      GUARDED behind -MlSeeds / -SleeveMethod
                                      script switches, default OFF          ← S6 (explicit cut-point)
```

---

## Tasks

### S7-0 — Ledger + `Resolve-ActiveStages` extraction (fixes the routing bug; do first)

**Goal:** open the sprint ledger, then extract the stage-list computation
(`build-megaalpha-book.ps1:285-305`) out of the un-testable "main execution body" (guarded by
`if ($MyInvocation.InvocationName -ne '.')`, per the Pester file's own dot-source convention — see
its header comment) into a **pure, testable** `Resolve-ActiveStages` function, and fix the bug this
sprint's rationale depends on: today, `-Stage optimize -Profile prod` silently resolves to an
**empty** active-stage list (the mutual-exclusion filter drops `'optimize'` whenever
`$Profile -eq 'prod'`, with no regard for what `-Stage` explicitly asked for). Every other S7 task
assumes an operator *can* reach the `optimize` stage explicitly (for `--gp-trading` evaluation, or a
single-blend-vs-metabook comparison) — this task makes that true and provable.

**Root cause:** `build-megaalpha-book.ps1:300-305`:
```powershell
$useMetabook = ($Profile -eq 'prod')
$activeStages = $activeStages | Where-Object {
    if ($_ -eq 'metabook') { $useMetabook }
    elseif ($_ -eq 'optimize') { -not $useMetabook }
    else { $true }
}
```
`$useMetabook` is a function of `$Profile` alone, so the auto-exclusion fires even when the caller
did not use the `'all'`/`'pipeline'` shorthand — it fires on an explicit, unambiguous `-Stage
optimize` request too, silently returning nothing.

**Files:** `atx-impl/scripts/build-megaalpha-book.ps1`, `atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1`.

**Steps:**

1. **Write the FAILING Pester `It` first** (append to the Tests file, new `Describe` block):
   ```powershell
   Describe 'Resolve-ActiveStages - explicit stage requests are never silently emptied (S7-0)' {

       It 'an explicit -Stage optimize -Profile prod resolves to @(''optimize''), NOT empty' {
           $stages = Resolve-ActiveStages -Stage @('optimize') -Profile 'prod'
           ($stages -contains 'optimize') | Should Be $true
           $stages.Count | Should Be 1
       }

       It 'the "all" shorthand still auto-excludes optimize for -Profile prod (metabook substitutes)' {
           $stages = Resolve-ActiveStages -Stage @('all') -Profile 'prod'
           ($stages -contains 'metabook') | Should Be $true
           ($stages -contains 'optimize') | Should Be $false
       }

       It 'the "all" shorthand still auto-excludes metabook for -Profile smoke (optimize substitutes)' {
           $stages = Resolve-ActiveStages -Stage @('all') -Profile 'smoke'
           ($stages -contains 'optimize') | Should Be $true
           ($stages -contains 'metabook') | Should Be $false
       }

       It 'the "pipeline" shorthand behaves identically to "all" minus discover' {
           $stages = Resolve-ActiveStages -Stage @('pipeline') -Profile 'prod'
           ($stages -contains 'discover') | Should Be $false
           ($stages -contains 'metabook') | Should Be $true
           ($stages -contains 'optimize') | Should Be $false
       }
   }
   ```
2. **Run to fail:**
   ```powershell
   Invoke-Pester -Script atx-impl\scripts\tests\build-megaalpha-book.Tests.ps1 `
       -TestName '*Resolve-ActiveStages*'
   ```
   Expected: **FAIL** — `Resolve-ActiveStages` is not a recognized function (the dot-sourced script
   has no such name yet); all four `It`s error out with "The term 'Resolve-ActiveStages' is not
   recognized...".
3. **Edit the script** — extract the logic into a new function ahead of the main-body guard, and
   have the main body call it:
   ```powershell
   function Resolve-ActiveStages {
       param(
           [string[]] $Stage,
           [string]   $Profile
       )
       $expandedStages = @()
       foreach ($s in $Stage) {
           if ($s -eq 'all')         { $expandedStages += @('discover', 'combine', 'metabook', 'optimize', 'report') }
           elseif ($s -eq 'pipeline') { $expandedStages += @('combine', 'metabook', 'optimize', 'report') }
           else                       { $expandedStages += $s }
       }
       $seen = @{}; $orderedStages = @()
       foreach ($s in $expandedStages) { if (-not $seen.ContainsKey($s)) { $seen[$s] = $true; $orderedStages += $s } }
       $canonicalOrder = @('discover', 'combine', 'metabook', 'optimize', 'report')
       $activeStages   = $canonicalOrder | Where-Object { $orderedStages -contains $_ }

       # S7-0 fix: metabook/optimize are alternatives (both write books.bin) ONLY when the caller
       # used the 'all'/'pipeline' SHORTHAND -- that's the one ambiguous case (the shorthand can't
       # know which the caller wants, so the profile decides). An EXPLICIT -Stage list (e.g.
       # -Stage optimize, or even -Stage metabook,optimize) is never second-guessed: the caller said
       # exactly what they meant. This is what makes --gp-trading (S3, optimize-only) reachable at
       # all -- see sprint-7 ROADMAP note.
       $isShorthand = ($Stage -contains 'all') -or ($Stage -contains 'pipeline')
       if ($isShorthand) {
           if ($Profile -eq 'prod') { $activeStages = $activeStages | Where-Object { $_ -ne 'optimize' } }
           else                     { $activeStages = $activeStages | Where-Object { $_ -ne 'metabook' } }
       }
       [string[]]$activeStages
   }
   ```
   Replace the old inline block (`:285-305`) in the main execution body with:
   ```powershell
   $activeStages = Resolve-ActiveStages -Stage $Stage -Profile $Profile
   ```
4. **Run to pass:**
   ```powershell
   Invoke-Pester -Script atx-impl\scripts\tests\build-megaalpha-book.Tests.ps1
   ```
   Expected: **all Describe blocks green**, including the 4 new `It`s and every pre-existing one
   (the smoke/prod shorthand behavior is unchanged — only the explicit-list case changed).
5. **Commit:**
   ```
   git add atx-impl/scripts/build-megaalpha-book.ps1 atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1
   git commit -m "p9 S7-0: extract Resolve-ActiveStages; fix explicit -Stage optimize -Profile prod

   Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
   ```

**Accept:** `Resolve-ActiveStages` is a pure function reachable via dot-source; the 4 new `It`s pass;
every pre-existing `Describe` still passes (smoke/prod shorthand routing is bit-for-bit unchanged).

---

### S7-1 — Corrected prod argv + per-flag rationale table

**Goal:** thread the now-live p9 flags onto the stage that actually executes them, per the
Architecture note above. Extend `New-DiscoverArgv`, `New-CombineArgv`, `New-MetabookArgv`,
`New-OptimizeArgv`, `New-ReportArgv` with new parameters (all inert-default, mirroring the existing
`-ImpactInSelection`/`-SelectionAum` convention: opt-in booleans absent by default, opt-in numerics
only emitted when non-zero/non-empty); wire the prod `switch` bodies in the main execution block to
pass them; fix the stale `$AtxExe` default.

**Files:** `atx-impl/scripts/build-megaalpha-book.ps1`, `atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1`.

**Per-flag rationale table (the reconciler's classification request):**

| Flag | Class | Target (corrected) | Rationale |
|---|---|---|---|
| `--dead-alpha-lib-dir <libdir>` | **new, makes an ex-no-op live** | `New-MetabookArgv` (+ `New-OptimizeArgv`) | S1's dead_lib source; reuses the SAME `$libraryDir` (`Join-Path $WorkDir '_library'`) already threaded via `--library-dir`, so the retired-alpha holdings the crowding extractor reads come from the exact library discover/combine/metabook already share |
| `--dead-alpha-factors` | **ex-no-op → now-live** | `New-MetabookArgv` (+ `New-OptimizeArgv`) | Existing flag (config.cpp:44), previously attached only to the dead `optimize` branch for prod; S1 makes the engine honor it once `--dead-alpha-lib-dir` is non-null |
| `--group-neutralize` | **ex-no-op → now-live** | `New-MetabookArgv` (+ `New-OptimizeArgv`) | Same class as above (config.cpp:45) |
| `--risk-model factor` | **ex-no-op → now-live** | `New-CombineArgv`, `New-MetabookArgv`, `New-OptimizeArgv` | Existing flag (config.cpp:131-138); S2 stops `stage_combine.cpp`'s CLI entry (:760-766) and `stage_run.cpp`'s metabook CLI entry (:34-38) from hardcoding Diagonal |
| `--gp-trading`, `--gp-risk-aversion`, `--gp-trade-cost-scale` | **new, still stage-gated** | `New-OptimizeArgv` ONLY | S3 wires only `stage_optimize.cpp:191`; NOT reachable via `metabook` (S3's ROADMAP roots exclude `stage_metabook.cpp` — see architecture note). Prod's default `all`/`pipeline` shorthand routes to `metabook`, so this flag does **not** bite in the default V1 command. Reachable only via an explicit `-Stage optimize` companion run (now possible post-S7-0) — documented, not hidden, in the runbook below |
| `--capacity-objective`, `--turnover-objective` | **new, now-live** | `New-DiscoverArgv` | S4's NSGA objectives are computed during the factory search inside `discover` |
| `--book-turnover-gate 0.20`, `--participation-cap <x>` | **new, now-live** | `New-MetabookArgv` (+ `New-OptimizeArgv`) | S5 roots list BOTH `stage_metabook.cpp` and `stage_optimize.cpp` explicitly (unlike S3) — the book-level gate and the QP participation cap both reach whichever stage constructs the book, including metabook |
| `--borrow-bps <x>` | **new, now-live** | `New-ReportArgv` | Placed alongside the existing `--capacity-curve`; `stage_report.cpp:644-777` is where the honest book-level cost/capacity numbers (`capacity_point_aum`, etc.) are already assembled — the natural home for a non-zero financing rate feeding the same scorecard row |
| `--robustness-battery` (existing) | **recipe omission, now fixed** | `New-DiscoverArgv` | Pre-existing flag (config.cpp:52, stage_discover.cpp:597); the prod recipe simply never passed it — not an engine gap, a recipe bug |
| `--robustness-sub-universe`, `--robustness-alt-neutralization`, `--robustness-param-perturb` | **new, now-live** | `New-DiscoverArgv` | S5 adds the config surface for the 3 previously-unreachable battery checks |
| `--deflate-selection` | **recipe omission, now fixed** | `New-DiscoverArgv` | Fully wired since p8/R4 (config.cpp:40, stage_discover.cpp:600,1038); simply never turned on in the prod profile until now |
| `--ml-seeds`, `--ml-seed-model-dir`, `--sleeve-method nco` | **new, explicitly guarded** | `New-DiscoverArgv` / `New-MetabookArgv`, gated behind script switches `-MlSeeds` / `-SleeveMethod nco`, default OFF | S6 is the explicit ROADMAP cut-point; the recipe must produce the identical argv whether S6 landed or was cut — see the S6-guard note below |
| `--capacity-curve` (existing) | **annotated, not fixed** | `New-ReportArgv` (unchanged) | Design spec: "Dead marker; the curve computes whenever `has_volume && report_aum>0` regardless of the flag." No p9 sprint (S1–S6) touches `stage_report.cpp`'s unconditional compute — keep passing it (documents intent / a future fix target) but add a code comment recording that it is still cosmetic |
| `$AtxExe` default | **stale, now fixed** | script param default | Was `C:\atx-wt\p8\build\bin\atx-impl.exe`; this script now lives in the **p9** worktree — corrected to `C:\atx-wt\p9\build\bin\atx-impl.exe` (mirrors the doc-comment's own stated intent: "never silently invokes the main repo's ... binary" — extended to never silently invoke the *prior sprint's worktree* binary either) |

**Corrected prod argv (all four then-executing stages, `-Stage all` default routing):**

```
discover --panel work\accept\panel.bin --seed-file atx-impl\tests\fixtures\alpha101.txt --gated \
  --library-dir work\megaalpha-build\_library --max-pbo 0.5 --min-dsr 0.5 --min-sharpe 0.25 \
  --min-fitness 1.0 --max-turnover 0.50 --oos-fraction 0.25 --alpha-out work\megaalpha-build\alphas \
  --population 300 --generations 15 --impact-in-selection --selection-aum 50000000 \
  --require-split-stable --blocking-pbo --deflate-selection --capacity-objective \
  --turnover-objective --robustness-battery --robustness-sub-universe \
  --robustness-alt-neutralization --robustness-param-perturb
  [+ --ml-seeds --ml-seed-model-dir <dir>  ONLY when -MlSeeds passed AND S6 has landed]

combine --panel work\accept\panel.bin --library-dir work\megaalpha-build\_library \
  --combo-out work\megaalpha-build\combo.bin --holdout-frac 0.25 --method stack \
  --risk-model factor

metabook --panel work\accept\panel.bin --combo work\megaalpha-build\combo.bin \
  --library-dir work\megaalpha-build\_library --books-out work\megaalpha-build\books.bin \
  --sleeve-method hrp --risk-model factor --dead-alpha-factors \
  --dead-alpha-lib-dir work\megaalpha-build\_library --group-neutralize \
  --book-turnover-gate 0.20 --participation-cap 0.10
  [sleeve-method becomes 'nco' ONLY when -SleeveMethod nco explicitly passed AND S6 has landed]

report --panel work\accept\panel.bin --books work\megaalpha-build\books.bin \
  --combo work\megaalpha-build\combo.bin --report-out work\megaalpha-build\report \
  --capacity-curve --borrow-bps 25
```

`--gp-trading`/`--gp-risk-aversion`/`--gp-trade-cost-scale` are NOT in the above (they do not run
under the default `-Stage all` shorthand); the corrected `optimize` argv, reachable via the explicit
companion invocation S7-0 restores, is:

```
optimize --panel work\accept\panel.bin --combo work\megaalpha-build\combo.bin \
  --books-out work\megaalpha-build\books.bin --position-mode --cost-bps 10 --risk-model factor \
  --dead-alpha-factors --dead-alpha-lib-dir work\megaalpha-build\_library --group-neutralize \
  --gp-trading --gp-risk-aversion 1.0 --gp-trade-cost-scale 1.0
```

`--participation-cap 0.10`, `--borrow-bps 25`, `--gp-risk-aversion 1.0`, `--gp-trade-cost-scale 1.0`
are ILLUSTRATIVE defaults (same convention as the pre-existing `-SelectionAum` doc-comment,
`build-megaalpha-book.ps1:83-90`) — an operator MUST override them for the book's actual ADV/target
AUM/financing terms; they exist so the CLI parses non-degenerate values, not as tuned production
numbers.

**Steps (repeat this shape per `New-*Argv` function touched — shown once in full for
`New-MetabookArgv`, the highest-value one; apply identically to `New-CombineArgv`,
`New-DiscoverArgv`, `New-OptimizeArgv`, `New-ReportArgv`):**

1. **Write the FAILING Pester `It`s first:**
   ```powershell
   Describe 'New-MetabookArgv - S7-1 risk-model + book-level gates now reach the prod book stage' {

       It 'prod: contains --risk-model factor --dead-alpha-factors --dead-alpha-lib-dir --group-neutralize' {
           $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'hrp' `
               -RiskModel 'factor' -DeadAlphaFactors -DeadAlphaLibDir 'C:\atx-test\work\_library' -GroupNeutralize
           ($argv -contains '--risk-model')         | Should Be $true
           ($argv -contains '--dead-alpha-factors')  | Should Be $true
           ($argv -contains '--dead-alpha-lib-dir')  | Should Be $true
           ($argv -contains '--group-neutralize')    | Should Be $true
       }

       It 'default (smoke): omits risk-model/dead-alpha/group-neutralize entirely (absence == inert)' {
           $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'invvol'
           ($argv -contains '--risk-model')        | Should Be $false
           ($argv -contains '--dead-alpha-factors') | Should Be $false
           ($argv -contains '--dead-alpha-lib-dir') | Should Be $false
           ($argv -contains '--group-neutralize')   | Should Be $false
       }

       It 'coupling: --dead-alpha-factors implies --dead-alpha-lib-dir is present with a non-empty value' {
           $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'hrp' `
               -RiskModel 'factor' -DeadAlphaFactors -DeadAlphaLibDir 'C:\atx-test\work\_library'
           if ($argv -contains '--dead-alpha-factors') {
               $i = [array]::IndexOf($argv, '--dead-alpha-lib-dir')
               $i | Should BeGreaterThan -1
               $argv[$i + 1] | Should Not Be ''
           }
       }

       It 'contains --book-turnover-gate 0.20 and --participation-cap when requested' {
           $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'hrp' `
               -BookTurnoverGate 0.20 -ParticipationCap 0.10
           $gi = [array]::IndexOf($argv, '--book-turnover-gate')
           $pi = [array]::IndexOf($argv, '--participation-cap')
           $argv[$gi + 1] | Should Be '0.2'
           $argv[$pi + 1] | Should Be '0.1'
       }

       It 'omits --book-turnover-gate/--participation-cap when both are 0 (the inert default)' {
           $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'hrp'
           ($argv -contains '--book-turnover-gate') | Should Be $false
           ($argv -contains '--participation-cap')  | Should Be $false
       }
   }
   ```
2. **Run to fail:**
   ```powershell
   Invoke-Pester -Script atx-impl\scripts\tests\build-megaalpha-book.Tests.ps1 `
       -TestName '*New-MetabookArgv - S7-1*'
   ```
   Expected: **FAIL** — `New-MetabookArgv` does not accept `-RiskModel`/`-DeadAlphaFactors`/
   `-DeadAlphaLibDir`/`-GroupNeutralize`/`-BookTurnoverGate`/`-ParticipationCap` (Pester 3 reports "A
   parameter cannot be found that matches parameter name ...").
3. **Edit the script** — extend the function signature and body:
   ```powershell
   function New-MetabookArgv {
       param(
           [string] $PanelBin,
           [string] $WorkDir,
           [string] $SleeveMethod = 'invvol',
           # S7-1 (S2): reuses the existing --risk-model taxonomy; '' (default) omits the flag so
           # the metabook subcommand stays byte-for-argv Diagonal, matching pre-p9 behavior.
           [string] $RiskModel = '',
           # S7-1 (S1): opt-in dead-alpha crowding de-levering. Requires DeadAlphaLibDir to actually
           # bite (fail-open no-op otherwise, per S1's own contract) -- the coupling is enforced here,
           # not just asserted in tests, so a caller can't silently ship a no-op --dead-alpha-factors.
           [switch] $DeadAlphaFactors,
           [string] $DeadAlphaLibDir = '',
           [switch] $GroupNeutralize,
           # S7-1 (S5): book-level turnover gate + optimizer participation cap. 0.0 (default) => off.
           [double] $BookTurnoverGate = 0.0,
           [double] $ParticipationCap = 0.0
       )
       $libraryDir = Join-Path $WorkDir '_library'
       $comboIn    = Join-Path $WorkDir 'combo.bin'
       $booksOut   = Join-Path $WorkDir 'books.bin'

       $argv = [System.Collections.Generic.List[string]]::new()
       $argv.AddRange([string[]]@(
           'metabook',
           '--panel',         $PanelBin,
           '--combo',         $comboIn,
           '--library-dir',   $libraryDir,
           '--books-out',     $booksOut,
           '--sleeve-method', $SleeveMethod
       ))
       if ($RiskModel -ne '') { $argv.Add('--risk-model'); $argv.Add($RiskModel) }
       if ($DeadAlphaFactors) {
           $argv.Add('--dead-alpha-factors')
           # Coupling (S1's own fail-open contract): an empty lib dir here would silently no-op the
           # flag we just set, so default it to the shared library dir when the caller didn't supply
           # one explicitly.
           $libDir = if ($DeadAlphaLibDir -ne '') { $DeadAlphaLibDir } else { $libraryDir }
           $argv.Add('--dead-alpha-lib-dir'); $argv.Add($libDir)
       }
       if ($GroupNeutralize) { $argv.Add('--group-neutralize') }
       if ($BookTurnoverGate -gt 0) { $argv.Add('--book-turnover-gate'); $argv.Add([string]$BookTurnoverGate) }
       if ($ParticipationCap -gt 0) { $argv.Add('--participation-cap'); $argv.Add([string]$ParticipationCap) }
       [string[]]$argv.ToArray()
   }
   ```
4. **Run to pass:**
   ```powershell
   Invoke-Pester -Script atx-impl\scripts\tests\build-megaalpha-book.Tests.ps1 `
       -TestName '*New-MetabookArgv*'
   ```
   Expected: **all green**, including the pre-existing `New-MetabookArgv (S5-4 new stage)` block
   (unaffected — none of its assertions touch the new params).
5. Repeat steps 1-4 for `New-CombineArgv` (`-RiskModel`), `New-DiscoverArgv` (`-DeflateSelection`,
   `-CapacityObjective`, `-TurnoverObjective`, `-RobustnessBattery`, `-RobustnessSubUniverse`,
   `-RobustnessAltNeutralization`, `-RobustnessParamPerturb`, `-MlSeeds`, `-MlSeedModelDir`),
   `New-OptimizeArgv` (`-DeadAlphaLibDir`, `-GpTrading`, `-GpRiskAversion`, `-GpTradeCostScale` —
   `-RiskModel`/`-DeadAlphaFactors`/`-GroupNeutralize` already exist on this function), and
   `New-ReportArgv` (`-BorrowBps`).
6. **Edit the main execution body's prod `switch` arms** to pass the new flags (discover/combine/
   metabook/report cases), and the `$AtxExe` default:
   ```powershell
   [string]   $AtxExe      = 'C:\atx-wt\p9\build\bin\atx-impl.exe',
   ```
   (update the accompanying doc-comment at `:76-80` to say "p9 worktree" instead of "p8 WORKTREE").
7. **Run the full suite to pass:**
   ```powershell
   Invoke-Pester -Script atx-impl\scripts\tests\build-megaalpha-book.Tests.ps1
   ```
8. **Commit:**
   ```
   git add atx-impl/scripts/build-megaalpha-book.ps1 atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1
   git commit -m "p9 S7-1: corrected prod argv -- risk-model/dead-alpha/gates onto the stage that runs

   Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
   ```

**Accept:** every new parameter is inert-default (absent/0/empty ⇒ no new flag emitted, smoke's argv
untouched); the coupling test (`--dead-alpha-factors` ⇒ `--dead-alpha-lib-dir` present, non-empty)
passes; the corrected prod argv block above matches what the main body actually emits under
`-DryRun -Profile prod` (verified by hand-running `-DryRun`, not just Pester — Pester only exercises
the pure functions, not the main body's stage wiring).

---

### S7-2 — Pester assertion updates (the remaining coupling / regression assertions)

**Goal:** close the remaining gaps the S7-1 unit didn't already cover: a `--gp-trading` implies
`--gp-risk-aversion`/`--gp-trade-cost-scale` coupling on `New-OptimizeArgv`; a
`--robustness-sub-universe`/`-alt-neutralization`/`-param-perturb` presence-without-`--robustness-battery`
guard (S5's sub-checks should not be emitted if the master switch is off — matches the design spec's
"only 1 of 4 checks reachable... prod recipe never sets the flag" framing: the sub-checks are
meaningless without the master); and an S6-guard regression test proving the recipe's prod argv is
byte-identical whether or not `-MlSeeds`/`-SleeveMethod nco` are ever passed.

**Files:** `atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1` only (no further `.ps1` edits
expected — S7-1 already implemented the underlying coupling logic in `New-MetabookArgv`; this task
extends the same discipline to `New-DiscoverArgv`/`New-OptimizeArgv` and pins it).

**Steps:**

1. **Write the FAILING `It`s:**
   ```powershell
   Describe 'New-OptimizeArgv - S7-2 gp-trading coupling' {

       It 'contains --gp-trading --gp-risk-aversion --gp-trade-cost-scale together' {
           $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10 `
               -GpTrading -GpRiskAversion 1.0 -GpTradeCostScale 1.0
           ($argv -contains '--gp-trading') | Should Be $true
           $ri = [array]::IndexOf($argv, '--gp-risk-aversion')
           $ci = [array]::IndexOf($argv, '--gp-trade-cost-scale')
           $ri | Should BeGreaterThan -1
           $ci | Should BeGreaterThan -1
       }

       It 'omits all three by default (smoke stays untouched)' {
           $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10
           ($argv -contains '--gp-trading') | Should Be $false
           ($argv -contains '--gp-risk-aversion') | Should Be $false
           ($argv -contains '--gp-trade-cost-scale') | Should Be $false
       }
   }

   Describe 'New-DiscoverArgv - S7-2 robustness sub-checks require the master --robustness-battery' {

       It 'sub-checks are ABSENT even if requested when RobustnessBattery is not set (fail-safe, not silent)' {
           $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
               -RobustnessSubUniverse -RobustnessAltNeutralization -RobustnessParamPerturb
           ($argv -contains '--robustness-sub-universe') | Should Be $false
           ($argv -contains '--robustness-battery') | Should Be $false
       }

       It 'sub-checks ARE present when RobustnessBattery is also set (the prod combination)' {
           $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
               -RobustnessBattery -RobustnessSubUniverse -RobustnessAltNeutralization -RobustnessParamPerturb
           ($argv -contains '--robustness-battery') | Should Be $true
           ($argv -contains '--robustness-sub-universe') | Should Be $true
           ($argv -contains '--robustness-alt-neutralization') | Should Be $true
           ($argv -contains '--robustness-param-perturb') | Should Be $true
       }
   }

   Describe 'New-DiscoverArgv / New-MetabookArgv - S7-2 S6 guard: identical argv whether S6 landed or was cut' {

       It 'discover: --ml-seeds absent by default; present only when -MlSeeds is explicitly passed' {
           $withoutMl = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir
           $withMl    = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
               -MlSeeds -MlSeedModelDir 'C:\atx-test\work\_ml'
           ($withoutMl -contains '--ml-seeds') | Should Be $false
           ($withMl -contains '--ml-seeds')    | Should Be $true
       }

       It 'metabook: --sleeve-method defaults to hrp/invvol regardless of S6; nco only on explicit request' {
           $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'hrp'
           $si = [array]::IndexOf($argv, '--sleeve-method')
           $argv[$si + 1] | Should Be 'hrp'
           $argvNco = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'nco'
           $siNco = [array]::IndexOf($argvNco, '--sleeve-method')
           $argvNco[$siNco + 1] | Should Be 'nco'
       }
   }
   ```
2. **Run to fail:**
   ```powershell
   Invoke-Pester -Script atx-impl\scripts\tests\build-megaalpha-book.Tests.ps1 `
       -TestName '*S7-2*'
   ```
   Expected: **FAIL** on the robustness-master-coupling `It`s (S7-1's `New-DiscoverArgv` edit, if
   done naively, emits sub-checks independent of the master switch) and possibly on `-GpTrading`
   coupling if S7-1 didn't already couple it (S7-1's steps above only added the flags, not the
   coupling guard — this task adds the guard).
3. **Edit the script** — in `New-DiscoverArgv`, gate the 3 sub-checks behind `$RobustnessBattery`:
   ```powershell
   if ($RobustnessBattery) {
       $argv.Add('--robustness-battery')
       if ($RobustnessSubUniverse)      { $argv.Add('--robustness-sub-universe') }
       if ($RobustnessAltNeutralization) { $argv.Add('--robustness-alt-neutralization') }
       if ($RobustnessParamPerturb)     { $argv.Add('--robustness-param-perturb') }
   }
   ```
   (replacing four independent `if`s with the master-gated block — the sub-checks are structurally
   incapable of being emitted without the master, closing the exact "1 of 4 checks reachable, no
   surface for the other 3" gap honestly instead of just adding three more independently-optional
   flags that could recreate a *different* half-wired state).
4. **Run to pass:**
   ```powershell
   Invoke-Pester -Script atx-impl\scripts\tests\build-megaalpha-book.Tests.ps1
   ```
5. **Commit:**
   ```
   git add atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1 atx-impl/scripts/build-megaalpha-book.ps1
   git commit -m "p9 S7-2: couple robustness sub-checks to the master flag; pin gp-trading + S6 guards

   Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
   ```

**Accept:** robustness sub-checks are unreachable without `--robustness-battery` (by construction,
not just convention); gp-trading's three flags travel together; the S6 guard proves the recipe's
argv is unaffected by whether S6's flags are ever invoked (they're the caller's explicit opt-in, not
a profile default).

---

### S7-3 — Smoke-profile stays minimal (proof-only unit)

**Goal:** pin, via Pester, that none of S7-1/S7-2's new flags leak into the smoke profile's argv.
Smoke exists to prove CLI *wiring* (the binary accepts and parses the flag) at each flag's own inert
value — per the script's own documented convention (header `.NOTES`: "Every new p8 flag is passed at
its INERT value ... EXCEPT the opt-in boolean switches, which stay ABSENT"). None of the new p9
flags should change that convention: they inherit it for free, because every new parameter added in
S7-1 defaults to the inert value/absence and the smoke `switch` arms in the main body were not
touched to pass them. This task is pure verification — if any assertion here fails, S7-1/S7-2 leaked
a flag into smoke and must be fixed, not this task's test relaxed.

**Files:** `atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1` only.

**Steps:**

1. **Write the (expected-to-pass-immediately-if-S7-1/S7-2-were-done-correctly, but written and run
   as a genuine RED→GREEN check per the sprint's TDD discipline — see run-to-fail below for how it
   earns its GREEN) `It`s:**
   ```powershell
   Describe 'Smoke profile - S7-3 stays minimal: none of the new p9 flags leak in' {

       It 'New-DiscoverArgv default (smoke-shaped call) omits every new p9 flag' {
           $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir -LooseGates
           foreach ($f in @('--deflate-selection','--capacity-objective','--turnover-objective',
                             '--robustness-battery','--robustness-sub-universe',
                             '--robustness-alt-neutralization','--robustness-param-perturb',
                             '--ml-seeds','--ml-seed-model-dir')) {
               ($argv -contains $f) | Should Be $false
           }
       }

       It 'New-CombineArgv default (smoke-shaped call) omits --risk-model' {
           $argv = New-CombineArgv -PanelBin $testPanel -WorkDir $testWorkDir
           ($argv -contains '--risk-model') | Should Be $false
       }

       It 'New-MetabookArgv default (smoke-shaped call) omits every new p9 flag' {
           $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'invvol'
           foreach ($f in @('--risk-model','--dead-alpha-factors','--dead-alpha-lib-dir',
                             '--group-neutralize','--book-turnover-gate','--participation-cap')) {
               ($argv -contains $f) | Should Be $false
           }
       }

       It 'New-OptimizeArgv default (smoke-shaped call) omits gp-trading + new S1 flags' {
           $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10
           foreach ($f in @('--gp-trading','--gp-risk-aversion','--gp-trade-cost-scale','--dead-alpha-lib-dir')) {
               ($argv -contains $f) | Should Be $false
           }
       }

       It 'New-ReportArgv default (smoke-shaped call) omits --borrow-bps' {
           $argv = New-ReportArgv -PanelBin $testPanel -WorkDir $testWorkDir
           ($argv -contains '--borrow-bps') | Should Be $false
       }
   }
   ```
2. **Run to fail (the honest RED for this unit):** temporarily verify the RED by asserting against
   the PRE-S7-1 state is not applicable here (S7-1/S7-2 already landed by this point in the
   sequence) — instead, the RED this task earns is procedural: run the suite BEFORE writing these
   `It`s to confirm the underlying `New-*Argv` defaults truly are inert (they are, by S7-1's own
   construction), then add the `It`s and run:
   ```powershell
   Invoke-Pester -Script atx-impl\scripts\tests\build-megaalpha-book.Tests.ps1 -TestName '*S7-3*'
   ```
   Expected: **PASS immediately** (S7-1/S7-2 already made every new parameter inert-default). If any
   assertion FAILS here, that is a genuine regression in S7-1/S7-2 — fix the flagged `New-*Argv`
   function's default, not this test.
3. **No script edit expected.** If step 2 passes clean, S7-3 is a pure-verification unit; if it
   fails, go back to S7-1/S7-2's diff and correct the leaking default before proceeding.
4. **Run the FULL suite one more time** (regression gate for the whole sprint):
   ```powershell
   Invoke-Pester -Script atx-impl\scripts\tests\build-megaalpha-book.Tests.ps1
   ```
   Expected: every `Describe` block in the file — pre-existing p8 ones and all of S7-0/1/2/3's new
   ones — green.
5. **Commit:**
   ```
   git add atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1
   git commit -m "p9 S7-3: pin smoke-profile argv unaffected by the S7 prod-recipe corrections

   Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
   ```

**Accept:** smoke's argv (all five `New-*Argv` functions, default/smoke-shaped params) contains zero
of the new S1–S6 flags; the full Pester file is green end-to-end.

---

## V1 operator runbook (documented here; NOT executed in this sprint)

The real full-panel run is the operator's step (ROADMAP: "No long sweeps... V1 real-panel run is the
operator's, not p9's"). Once S1–S6 have merged and this sprint's corrected `New-*Argv` functions are
in place:

```powershell
# One-time sanity: confirm the corrected argv with -DryRun BEFORE spending the hour-long run.
# (This is the closest this sprint comes to "running" anything -- DryRun never invokes the binary.)
.\atx-impl\scripts\build-megaalpha-book.ps1 -DryRun -Profile prod -WorkDir work\megaalpha

# The real V1 run (overnight; the only hour-long run this series sanctions):
.\atx-impl\scripts\build-megaalpha-book.ps1 -Profile prod -Stage discover -WorkDir work\megaalpha
.\atx-impl\scripts\build-megaalpha-book.ps1 -Profile prod -Stage pipeline -WorkDir work\megaalpha
# ("pipeline" = combine -> metabook -> report under -Profile prod, post-S7-0's Resolve-ActiveStages;
#  optimize is still excluded from THIS shorthand by design -- metabook is prod's book stage. An
#  operator who additionally wants the --gp-trading (S3) comparison runs a THIRD, explicit command:
.\atx-impl\scripts\build-megaalpha-book.ps1 -Profile prod -Stage optimize -WorkDir work\megaalpha
# (now reachable post-S7-0; writes an alternate books.bin -- rename/relocate before or after so it
#  does not collide with the metabook run's books.bin at the same $WorkDir.)

# Scorecard to fill (pre-existing p8 template, corrected provenance expectations):
#   atx-impl\research\2026-07-03-megaalpha-book-results.md
# Its "1. Run provenance" row "Prod-profile argv actually used" should be pasted from the -DryRun
# output above -- this closes the file's own stale claim at lines 108/116-118 (written under p8,
# before this sprint's S7-0 fix) that the risk-model flags already reached discover+optimize; they
# now correctly show up on combine+metabook per this sprint.
```

Scorecard file: `atx-impl/research/2026-07-03-megaalpha-book-results.md` (already exists, from p8's
S5-5 — still all `<TBD — filled at V1>`; S7 does not edit it, the operator fills it after the V1
run, and should update its stale provenance narrative per the note above while doing so).

---

## Sequencing

1. **S7-0 first** — the `Resolve-ActiveStages` extraction + routing fix is a prerequisite for the
   `--gp-trading` runbook path and for testing anything about explicit stage selection.
2. **S7-1** — the bulk of the argv correction + the per-flag rationale table.
3. **S7-2** — the coupling/guard assertions S7-1's raw parameter additions don't yet enforce.
4. **S7-3** — pure verification that smoke stayed untouched; run last as the whole-file regression
   gate.

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| S1–S6 land with different exact flag names than the SHARED CONFIG-FIELD REGISTRY promises | S7's argv additions reference flags that don't exist; `Invoke-Stage` (real, non-DryRun) would hit `--risk-model must be ...`-style `InvalidArgument` at V1 time | S7 is DryRun-only — a name mismatch is caught the moment an operator runs the `-DryRun` sanity check in the runbook, never silently, and never mid-V1-run |
| The `Resolve-ActiveStages` refactor (S7-0) subtly changes smoke's or the old prod shorthand's routing | Pinned pre-existing Pester `Describe` blocks (`New-DiscoverArgv`, etc.) don't cover stage-list routing directly, but the 4 new S7-0 `It`s do, and are written to reproduce today's shorthand behavior exactly | Run the FULL Pester file after S7-0, not just the new `It`s, before proceeding to S7-1 |
| `--gp-trading` looks "added" in the argv table but is genuinely unreachable via prod's default shorthand | An operator reads the corrected-argv block, assumes GP-trading is live in the V1 run, and it silently isn't | Called out explicitly, twice (rationale table + runbook), as stage-gated with its own explicit companion command — never presented as part of the default prod pipeline |
| S6 cut after this sprint's flags are already added to the functions | `-MlSeeds`/`-SleeveMethod nco` would parse-fail against a pre-S6 binary if an operator passes them anyway | Guarded behind explicit script switches defaulting OFF; the prod profile's default argv never includes them regardless of whether S6 landed — the guard is "never on by default," not "detect at runtime" (DryRun can't safely probe the binary's flag support without invoking it) |
| `--participation-cap`/`--borrow-bps`/`--gp-risk-aversion`/`--gp-trade-cost-scale` illustrative defaults get mistaken for tuned production values | An operator ships an under- or over-constrained V1 book | Marked ILLUSTRATIVE in-line, mirroring the pre-existing `-SelectionAum` doc-comment convention exactly (`build-megaalpha-book.ps1:83-90`) |

---

## Bench / acceptance (sprint close)

- Full `Invoke-Pester -Script atx-impl\scripts\tests\build-megaalpha-book.Tests.ps1` green: every
  pre-existing `Describe` (unchanged assertions) plus S7-0/1/2/3's new ones.
- Corrected prod argv block in this doc matches a hand-run `-DryRun -Profile prod` transcript
  exactly (verified once, informally, as part of S7-1's close-out — not a Pester assertion, since
  Pester only exercises the pure functions, not the main execution body's `Write-Host` output).
- Per-flag rationale table complete: every flag in the ROADMAP's S1–S6 registry classified as
  now-live / still-dormant-and-why / annotated-not-fixed — none silently dropped, none silently
  oversold.
- No engine file touched; no binary invoked; no golden re-baselined.

---

## Out of scope

- Running the real full-panel V1 — operator step (runbook above documents it, does not execute it).
- Fixing `stage_metabook.cpp:62`'s `mh.trade_rate = 1.0` hardcode so `--gp-trading` reaches the
  metabook path too — that is new engine work (a future sprint's root, not S7's; S7 only corrects
  the *recipe*, it does not extend the *engine*).
- Fixing `stage_report.cpp`'s unconditional `capacity_point` compute so `--capacity-curve` stops
  being a dead marker — not an S1–S6 target; annotated only.
- Editing `atx-impl/research/2026-07-03-megaalpha-book-results.md` beyond what the operator does at
  V1 time — S7 reads it for the runbook cross-reference only.
- Any change to `RunConfig`, `MetaBookStageConfig`, or any other engine-side struct — the SHARED
  CONFIG-FIELD REGISTRY's S7 row is explicit: "no new fields; recipe wiring only."
