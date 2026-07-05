# Sprint 7 progress ledger — Correct the Prod Recipe (V1-Ready)

**Goal:** make `atx-impl/scripts/build-megaalpha-book.ps1 -Profile prod` actually exercise the
now-live S1–S5 levers. The pre-S7 script attached `--risk-model factor --dead-alpha-factors
--group-neutralize` to `New-OptimizeArgv`'s prod branch, but the metabook/optimize mutual-exclusion
filter unconditionally drops `optimize` from `$activeStages` under `-Profile prod` — so those flags
parsed but never reached a stage that runs (a Potemkin book). S7 re-attaches the live levers to the
stage that actually executes (`metabook`/`combine`/`discover`/`report`), fixes the stage-routing bug
that made this discoverable/testable, updates the Pester DryRun argv assertions, and fixes the stale
`$AtxExe` default (was pointing at the p8 worktree's binary). **DryRun-only — no binary invoked, no
engine file touched, zero new `RunConfig` fields.**

- **Worktree:** `C:\atx-wt\p9`
- **Branch:** `feat/p9`
- **Owns (exclusive):** `atx-impl/scripts/build-megaalpha-book.ps1`,
  `atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1`.
- **Test gate:** Pester 3.4.0 (Windows PowerShell 5.1) —
  `Invoke-Pester -Path atx-impl\scripts\tests\build-megaalpha-book.Tests.ps1 -PassThru`.

## S6 deferral (binding, carried from the operator's explicit instruction)

**Sprint 6 (ML-seeded discovery + NCO sleeve method) was DEFERRED by the operator and never landed.**
The p9 binary does **not** parse `--ml-seeds`, `--ml-seed-model-dir`, or `--sleeve-method nco` — none
of S6's engine-side flags exist. Per the ROADMAP's anti-Potemkin guardrail (an absent lever must be a
documented decision, not a silent omission), S7 explicitly:

- Does **not** add `-MlSeeds`/`-MlSeedModelDir` parameters to any `New-*Argv` function.
- Does **not** add `nco` as a supported `-SleeveMethod` script-level option — no top-level script
  switch for sleeve method selection was added at all; the main body's prod/smoke sleeve-method
  selection stays the pre-p9-real, hardcoded `hrp`/`invvol` literals.
- Documents the absence in code comments at both call sites where S6 flags would have gone
  (`New-DiscoverArgv`, near the ml-seed no-op comment; `New-MetabookArgv`'s `$SleeveMethod` param
  doc) — see `build-megaalpha-book.ps1`.
- Pins the absence with Pester assertions (S7-2's S6-guard block, S7-3's smoke-minimality block):
  `--ml-seeds`/`--ml-seed-model-dir` are proven structurally absent (never emitted under any flag
  combination this script can compose), and the script exposes no `-MlSeeds`/`-MlSeedModelDir`/
  `-SleeveMethod` top-level parameter at all.

A future S6 sprint re-enables these behind default-OFF switches once the engine actually parses them.

One line per clean unit (ROADMAP §141). Newest last.

| Unit | Commit | Deliverable | Review |
|------|--------|-------------|--------|
| S7-0 | 1ed1718 | ledger opened; extracted pure `Resolve-ActiveStages` (was inline, untestable, guarded by the main-body invocation check); fixed the routing bug where `-Stage optimize -Profile prod` silently resolved to an empty stage list — an explicit `-Stage` list is no longer second-guessed by the metabook/optimize mutual-exclusion filter, which now applies ONLY to the `all`/`pipeline` shorthands | — |
| S7-1 | fc597ca | corrected prod argv: `--risk-model factor`/`--dead-alpha-factors`/`--dead-alpha-lib-dir`/`--group-neutralize` (S1/S2) now attach to `New-MetabookArgv`+`New-CombineArgv` (the stages `-Profile prod`'s default routing actually runs), not the dead `New-OptimizeArgv` prod branch; `--capacity-objective`/`--turnover-objective`/`--deflate-selection`/`--robustness-battery`+3 sub-checks (S4/S5/p8) now reach `New-DiscoverArgv`; `--book-turnover-gate` (S5-1) reaches `New-MetabookArgv`; `--borrow-bps` (S5) reaches `New-ReportArgv`; **[S7 review CORRECTION: this unit ALSO emitted `--participation-cap` and `--group-neutralize` on the metabook argv — both are optimizer-only (`stage_metabook` has no reader), so they were corrected OUT in the review closeout below to avoid a parse-but-ignore Potemkin on the flagship book]**; `--gp-trading`/`--gp-risk-aversion`/`--gp-trade-cost-scale` (S3) added to `New-OptimizeArgv`, reachable via the explicit `-Stage optimize -Profile prod` companion command S7-0 restores (NOT part of the default `-Stage all` prod pipeline — documented, not hidden); `--capacity-curve` kept as an annotated dead marker; `$AtxExe` default fixed from the stale p8 worktree binary to the p9 one. Hand-verified via `-DryRun -Profile prod` that the emitted argv matches the plan's corrected-argv block exactly | — |
| S7-2 | 1227497 | closed the two coupling gaps S7-1's raw additions left open: `New-DiscoverArgv`'s 3 robustness sub-checks are now structurally unreachable without `--robustness-battery` (nested under the master, not 3 more independently-optional flags); `New-OptimizeArgv`'s `--gp-risk-aversion`/`--gp-trade-cost-scale` now travel only together with `--gp-trading` (never orphaned, never a bare flag missing its own numerics — illustrative defaults bumped 0.0→1.0 so the master switch alone is sufficient). Added an explicit S6-guard block proving `--ml-seeds`/`--ml-seed-model-dir` are structurally absent under every flag combination this script can compose, and that the top-level script exposes no `-MlSeeds`/`-MlSeedModelDir`/`-SleeveMethod` parameter at all | — |
| S7-3 | 0704245 | proof-only unit: pinned that none of S7-1/S7-2's new p9 flags leak into the smoke profile's argv across all five `New-*Argv` functions' default (smoke-shaped) calls. All 5 assertions passed immediately on first run — S7-1/S7-2's inert-by-construction defaults already guaranteed this; no script edit was needed or made (only the test file changed, confirmed via `git diff --stat`). Full-file regression gate: 65/65 green | ↓closeout |
| S7-4 | (this) | **review closeout (BLOCK→fixed):** removed `--group-neutralize` + `--participation-cap` from `New-MetabookArgv` (params + emission + prod call site); both are optimizer-only levers (`stage_optimize.cpp:477`, `:414-438`) with no `stage_metabook` reader, so emitting them on the flagship metabook argv was a parse-but-ignore Potemkin. Fixed the false `New-MetabookArgv` doc-comment ("the optimizer participation cap"), added a Potemkin-guard Pester test asserting the metabook argv never carries either, corrected the stale `ROADMAP.md:151` "S5-1/S5-2 wire both" claim that seeded the error. 66/66 green | SHIP |

**Review (adversarial): BLOCK → fixed → SHIP.** The reviewer's full flag→stage→consumer audit found
discover/combine/report argv all genuinely ACTIVE, frozen scope clean, trailers exact, S6 deferral
honest, Pester 65/65 — but confirmed a **Critical** defect: the "corrected" flagship prod recipe still
emitted `--participation-cap` and `--group-neutralize` on the `metabook` argv, and metabook's HRP-sleeve
construction consumes NEITHER (participation-cap only `stage_optimize.cpp:414-438`; group-neutralize only
`stage_optimize.cpp:477`/`stage_combine.cpp` — `stage_metabook.cpp:419-423,641-645` copy only `.kind`+
`.dead_alpha_factors` into `risk_cfg`). The script's own comment even called it "the optimizer
participation cap" while attaching it to `New-MetabookArgv`. Root cause: a stale `ROADMAP.md:151` claim
("S5-1/S5-2 wire both stage_optimize AND stage_metabook") the S7 plan inherited without checking the
landed S5-2 commit (which touched only `stage_optimize.cpp`). S7-4 fixed it per S7's own mandate ("drop/
annotate the flags that stay no-ops after p9"): the two levers now appear ONLY on `New-OptimizeArgv` (the
MVO path that does consume them, reachable via explicit `-Stage optimize`); a Potemkin-guard test pins
their absence on metabook; `ROADMAP.md:151` corrected. Genuinely wiring an HRP-sleeve participation
bound / metabook group-neutralization is unbuilt engine work (p10/S8), documented at the fixture site,
in `New-MetabookArgv`, and in the ROADMAP — not a silent omission.

**Close note (S7 sprint end):** Full `Invoke-Pester` on `build-megaalpha-book.Tests.ps1`: **66 passed, 0
failed** (31 pre-existing + 35 new across S7-0..S7-4). No engine file touched (diff is confined to the
two owned scripts + this ledger + the `ROADMAP.md:151` doc correction); no binary invoked at any point (DryRun-only, confirmed by hand-running
`-DryRun -Profile prod` and `-DryRun -Profile smoke` — both print argv and exit with "no binary was
invoked", never touching `atx-impl.exe`); zero new `RunConfig`/engine fields (recipe wiring only, per the
SHARED CONFIG-FIELD REGISTRY's S7 row).

The corrected default `-Stage all -Profile prod` argv (hand-verified against a `-DryRun` transcript,
matches the plan's corrected-argv block exactly modulo `WorkDir`/`PanelBin` substitution):
- `discover`: gains `--deflate-selection --capacity-objective --turnover-objective --robustness-battery
  --robustness-sub-universe --robustness-alt-neutralization --robustness-param-perturb`.
- `combine`: gains `--risk-model factor`.
- `metabook`: gains `--risk-model factor --dead-alpha-factors --dead-alpha-lib-dir <libdir>
  --book-turnover-gate 0.2` (post-closeout; `--group-neutralize`/`--participation-cap` intentionally
  NOT here — optimizer-only, see closeout).
- `report`: gains `--borrow-bps 25` (alongside the pre-existing `--capacity-curve`, now annotated as
  a documented dead marker, not a fix target).
- `optimize` is still excluded from the default `-Stage all`/`pipeline` shorthand (metabook is prod's
  book stage) — but is now genuinely reachable via an explicit `-Stage optimize -Profile prod`
  companion command (the S7-0 fix), which emits `--risk-model factor --dead-alpha-factors
  --dead-alpha-lib-dir <libdir> --group-neutralize --gp-trading --gp-risk-aversion 1
  --gp-trade-cost-scale 1` — S3's `--gp-trading` never reaches the default prod pipeline, by design,
  documented twice (rationale table + runbook), not hidden.

`$AtxExe`'s default was confirmed fixed from the stale `C:\atx-wt\p8\build\bin\atx-impl.exe` to
`C:\atx-wt\p9\build\bin\atx-impl.exe` (pinned by a dedicated Pester assertion that dot-sources the
script and reads the bound default directly, rather than string-matching the source).

S6 (ML-seeded discovery + NCO sleeve) stays exactly as deferred at sprint start: zero `-MlSeeds`/
`-MlSeedModelDir`/S6-flavored `-SleeveMethod` wiring exists anywhere in either owned file, pinned by
3 independent Pester assertions (S7-2's S6-guard block) plus S7-3's smoke-minimality sweep.
