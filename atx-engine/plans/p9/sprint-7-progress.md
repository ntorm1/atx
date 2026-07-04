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
| S7-1 | fc597ca | corrected prod argv: `--risk-model factor`/`--dead-alpha-factors`/`--dead-alpha-lib-dir`/`--group-neutralize` (S1/S2) now attach to `New-MetabookArgv`+`New-CombineArgv` (the stages `-Profile prod`'s default routing actually runs), not the dead `New-OptimizeArgv` prod branch; `--capacity-objective`/`--turnover-objective`/`--deflate-selection`/`--robustness-battery`+3 sub-checks (S4/S5/p8) now reach `New-DiscoverArgv`; `--book-turnover-gate`/`--participation-cap` (S5) reach `New-MetabookArgv`; `--borrow-bps` (S5) reaches `New-ReportArgv`; `--gp-trading`/`--gp-risk-aversion`/`--gp-trade-cost-scale` (S3) added to `New-OptimizeArgv`, reachable via the explicit `-Stage optimize -Profile prod` companion command S7-0 restores (NOT part of the default `-Stage all` prod pipeline — documented, not hidden); `--capacity-curve` kept as an annotated dead marker; `$AtxExe` default fixed from the stale p8 worktree binary to the p9 one. Hand-verified via `-DryRun -Profile prod` that the emitted argv matches the plan's corrected-argv block exactly | — |
| S7-2 | (pending) | closed the two coupling gaps S7-1's raw additions left open: `New-DiscoverArgv`'s 3 robustness sub-checks are now structurally unreachable without `--robustness-battery` (nested under the master, not 3 more independently-optional flags); `New-OptimizeArgv`'s `--gp-risk-aversion`/`--gp-trade-cost-scale` now travel only together with `--gp-trading` (never orphaned, never a bare flag missing its own numerics — illustrative defaults bumped 0.0→1.0 so the master switch alone is sufficient). Added an explicit S6-guard block proving `--ml-seeds`/`--ml-seed-model-dir` are structurally absent under every flag combination this script can compose, and that the top-level script exposes no `-MlSeeds`/`-MlSeedModelDir`/`-SleeveMethod` parameter at all | — |
