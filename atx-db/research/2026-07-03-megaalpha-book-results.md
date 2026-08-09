# p8 Sprint 5 (Capstone) — V1 Mega-Alpha Book Scorecard (TEMPLATE)

**Status: TEMPLATE ONLY. V1 has NOT been run.** Every measured row below is marked
`<TBD — filled at V1>`. This file is committed by S5-5 as the operator scorecard shape; filling
it in is an out-of-loop, operator-driven milestone (never a sprint gate — see
`atx-engine/plans/p8/sprint-5-wire-deflate-validate.md`, "Validation discipline").

**Branch/worktree:** `feat/p8` @ `C:\atx-wt\p8`. **Harness:**
`atx-impl\scripts\build-megaalpha-book.ps1` (S5-4). **CLI:** `atx-impl.exe` @
`build\bin\atx-impl.exe` (this worktree's own build — never `C:\atx`'s).

---

## 1. Run provenance

| Field | Value |
|---|---|
| Run date | `<TBD — filled at V1>` |
| Panel | `<TBD — filled at V1>` (expected: the full canonical-screened accept panel, e.g. `work\accept\panel.bin`, NOT the dev-panel smoke fixture) |
| Panel universe size / date range | `<TBD — filled at V1>` |
| Seed | `<TBD — filled at V1>` |
| Worker count | `<TBD — filled at V1>` |
| Wall time (discover / combine+metabook+optimize+report) | `<TBD — filled at V1>` |
| Commit SHA (this worktree, at run time) | `<TBD — filled at V1>` |
| `atx-impl` build config (Debug/Release, compiler, flags) | `<TBD — filled at V1>` |
| Prod-profile argv actually used (paste from the run's own echo / `-DryRun` dump) | `<TBD — filled at V1>` |

## 2. Book-level net-of-10bps OOS Sharpe

| Metric | Value |
|---|---|
| `portfolio_oos_sharpe` (from `stage_report.cpp`'s report KV, cost model at 10bps) | `<TBD — filled at V1>` |
| `portfolio_is_sharpe` (in-sample, sign-check only — NOT the acceptance number) | `<TBD — filled at V1>` |
| Admitted alpha count feeding the book | `<TBD — filled at V1>` |
| `avg_names_held` / book footprint | `<TBD — filled at V1>` |

## 3. DSR under cumulative-N deflation

| Metric | Value |
|---|---|
| Cumulative trial count `N` at run end (`library::Library::cumulative_trials()`) | `<TBD — filled at V1>` |
| Mean admitted-candidate DSR at that `N` (S5-2 `deflate_selection` NSGA column) | `<TBD — filled at V1>` |
| DSR sign (must be `> 0` for the north-star bar) | `<TBD — filled at V1>` |

## 4. PBO (CSCV)

| Metric | Value |
|---|---|
| Run-level PBO (`FactoryReport::pbo`, `factory.cpp` `finalize_run_pbo`) | `<TBD — filled at V1>` |
| `--blocking-pbo` verdict (admitted vs. un-admitted by the S5-2 blocking gate) | `<TBD — filled at V1>` |
| PBO bar (`< 0.5` for the north-star) | `<TBD — filled at V1>` |

## 5. CPCV (`eval::cpcv_folds`)

| Metric | Value |
|---|---|
| Number of CPCV folds evaluated | `<TBD — filled at V1>` |
| CPCV out-of-sample Sharpe distribution (mean / stdev) | `<TBD — filled at V1>` |

## 6. Walk-forward OOS Sharpe

| Metric | Value |
|---|---|
| `--walk-forward` telemetry (per-window OOS Sharpe, if enabled for the V1 run) | `<TBD — filled at V1>` |
| Worst single walk-forward window Sharpe | `<TBD — filled at V1>` |

## 7. Capacity curve

| Metric | Value |
|---|---|
| Edge-vs-AUM zero-crossing under √-impact (`--capacity-curve`, `stage_report.cpp` emit) | `<TBD — filled at V1>` |
| Capacity bar (`>= $100M` for the north-star) | `<TBD — filled at V1>` |

## 8. N_eff / IR breadth (`eval/breadth.hpp`)

| Metric | Value |
|---|---|
| Effective number of independent bets (`N_eff`) | `<TBD — filled at V1>` |
| Information ratio breadth decomposition | `<TBD — filled at V1>` |

## 9. Robustness-battery pass/fail matrix (S5-3)

| Check | Pass/Fail | Notes |
|---|---|---|
| `sub_universe` | `<TBD — filled at V1>` | |
| `alt_neutralization` | `<TBD — filled at V1>` | |
| `noise_control` | `<TBD — filled at V1>` | the check that rejects a degenerate `1/price`-style artifact (proven on synthetic fixtures in `robustness_battery_test.cpp`; V1 fills the LIVE book's verdict) |
| `param_perturbation` | `<TBD — filled at V1>` | |
| Overall battery verdict (`BatteryResult::survived`) | `<TBD — filled at V1>` | |

## 10. Reject-histogram / battery-failure dominant bucket

| Metric | Value |
|---|---|
| `FactoryReport::reject_histogram` (11-wide, S5-1 `AdmitKind` order) | `<TBD — filled at V1>` |
| Dominant reject bucket (names the next module's target if the bar is missed) | `<TBD — filled at V1>` |
| Dominant battery-failure check (if the battery verdict fails) | `<TBD — filled at V1>` |

---

## The V1 command (documented, NOT run in-sprint)

Adapted from the sprint spec's literal two-command form. **Deviation, recorded:** the spec's
V1 command uses `-Stage augment,discover` then `-Stage riskmodel,combine,metabook,optimize,report`.
This codebase has no standalone `augment` or `riskmodel` CLI subcommand (see
`atx-engine/plans/p8/sprint-5-progress.md`'s S5-4 section and this script's own header comment for
the full rationale) — `build-megaalpha-book.ps1`'s `-Profile prod` already folds the risk-model
knobs (`--risk-model factor --dead-alpha-factors --group-neutralize`) into the `discover`+`optimize`
stages' own argv, and short-interest augment is a pre-existing deferred gap (`stage_augment.hpp`).
The equivalent, composable two-command V1 invocation for THIS harness is:

```powershell
# After S1–S4 land on main and S5 threads the hub — run ONCE, overnight (the only hour-long run in p8):
.\atx-impl\scripts\build-megaalpha-book.ps1 -Profile prod -Stage discover -WorkDir work\megaalpha
.\atx-impl\scripts\build-megaalpha-book.ps1 -Profile prod -Stage pipeline -WorkDir work\megaalpha
# ("pipeline" = combine -> metabook -> optimize -> report; the prod profile selects metabook and
#  the harness auto-excludes optimize so books.bin is written exactly once, per S5-4's stage-graph
#  dedup logic.)
# Output (this file, filled in): atx-impl\research\2026-07-03-megaalpha-book-results.md
```

Both commands were verified COMPOSABLE via `-DryRun` (real script invocation, not just Pester's
dot-source) — confirmed to emit the correct discover argv (with `--impact-in-selection
--require-split-stable --blocking-pbo --min-dsr 0.5 --max-pbo 0.5`) and the correct
combine/metabook/report argv (with `--method stack`, `--sleeve-method hrp`, `--capacity-curve`),
with `optimize` correctly excluded (metabook substitutes it in the prod profile) and no binary
invoked. See `atx-engine/plans/p8/sprint-5-progress.md`'s S5-5 section for the exact captured
`-DryRun` output.

## North-star acceptance bar (p8, from the ROADMAP)

> Book net-of-10bps OOS Sharpe **> 1.0** with **>= 5** admitted alphas, DSR **> 0** under
> cumulative-N, PBO **< 0.5**, turnover **< 0.20/day** (cross-sleeve netted), capacity
> **>= $100M**, and the book **survives the robustness battery** — **OR** a documented frontier
> naming the binding constraint. An honest null is a valid V1 outcome (the p6-S7 / p7 precedent:
> `atx-impl/research/2026-06-27-tradeable-alpha-results.md`).

## Deferred / not-yet-measured (recorded honestly, per sprint discipline)

- V1 has not been run. Every row above is a `<TBD>` placeholder, not a fabricated number.
- The dev-panel smoke (S5-4) proves WIRING only (no dev-panel.bin exists in this worktree to even
  attempt a fast iteration loop on synthetic data — see the S5-4 ledger section); it is not a
  substitute for the V1 full-panel prod measurement.
- The robustness battery (S5-3) has no LIVE `Reevaluator` wired into `Factory::mine_into`/
  `mine_into_oos` yet (recorded as a deferred integration gap in the S5-3 ledger section) — V1's
  row 9 above can only be filled once that adapter exists, or via an out-of-band battery run
  against the V1 book's admitted candidates.
