# p8 Sprint 4 — Cost, Capacity & Execution Correctness — Progress Ledger

Base: feat/p8 @ a7838c6 (S1 complete: risk-model covariance spine).

Branch: feat/p8  Worktree: C:\atx-wt\p8

## Kickoff — bug-verification pass re-confirmed against the CURRENT tree (2026-07-03)

Every LIVE candidate below was re-read against the current file:line before scoping a unit (per
the spec's binding instruction: "S4 ships work for the LIVE rows only"). File:line evidence
re-confirmed at kickoff (line numbers may drift a few lines from the spec's own citation where the
file has since shifted; the CURRENT line is recorded here as the unit's authority):

| # | Candidate | Verdict | Confirmed file:line (kickoff re-read) |
|---|---|---|---|
| B1 | Capacity participation unit bug (shares÷dollar-ADV) | **LIVE — 3 sites** | `risk/capacity.hpp:238-240` (`part = shares/adv`, `adv=dollar_adv`=dollars, `capacity.hpp:165-177`); `factory/fitness.cpp:523` (`part=(target_aum·|w|/price)/adv`); SEAM `atx-impl/src/stage_combine.cpp:315` (`part_per_aum = abs_w/(price*adv)` — S3-owned, confirmed at kickoff to be the current line, not `:289,301` as the spec's earlier draft cited; the bug SHAPE is identical). |
| B2 | Cap-clip-renorm breaks dollar-neutrality | **LIVE — 2 sites** | `risk/optimizer.hpp:389-398` (`project`: demean→gross_normalize→cap_clip_renorm, no re-center after); `:422-458` (`cap_clip_renorm` ends on the deficit-renorm, no demean). `loop/weight_policy.hpp:311-327` (demean→gross_normalize→truncate_renorm, no re-center after); `:480-529` (`truncate_renorm`/`finalize_truncation`, no demean). |
| B3 | Limit orders fill THROUGH their limit after costs | **LIVE** | `exec/execution_sim.hpp` `emit_fill`: `fill_px = ref·(1+dir·slip)·(1+dir·temp)`, no re-clamp to the order's limit for `OrderType::Limit`. |
| B4 | Absent instruments retain stale executable volume | **LIVE** | `loop/market.hpp` `update_prices`: touches only slice-present rows; an absent id's prior `volumes_[idx]` survives, feeding `bar_volume()` → `volume_capped_qty` phantom fills. |
| B5 | Borrow accrual not in the main loop | **LIVE** | `cost/borrow.hpp` `accrue_borrow` built+tested, zero call sites in `src/`; `loop/backtest_loop.hpp` settle sequence never charges short financing. |
| B6 | Permanent impact not persisted across bars | **LIVE** | `exec/execution_sim.hpp` `apply_permanent_impact` → `market.shift_mark` shifts `marks_[idx]` in place; the next `update_prices` (`loop/market.hpp`) overwrites `marks_[idx] = r.bar.close` for any name present next bar, clobbering the shift. |
| B7 | Impact charged only as telemetry, not in the SELECTION objective | **PARTIALLY LIVE** | `factory/fitness.cpp` `book_cost_bps` feeds `objectives[4]` (NSGA) only, gated by `target_aum>0`; ScalarRaw ranks on `raw = wq·diversify·robust` (`fitness.cpp:416`), no cost term; `search_driver.cpp` ScalarRaw path never reads `objectives`. |
| B8 | Garleanu-Pedersen partial-trade | **ALREADY WIRED (linear); make turnover-native** | `risk/garleanu_pedersen.hpp` ships the scalar-Λ aim `gp_aim_and_value`; `atx-impl/src/stage_optimize.cpp:159-172` does a LINEAR blend `w := prev + rate·(w-prev)` toward the freshly-shaped TARGET, not the GP AIM. |
| B9 | First-class capacity curve emitted by stage_report? | **NOT EMITTED (scalar footprint only)** | `atx-impl/src/stage_report.cpp` emits per-name %ADV scalars at one `report_aum`; no `aum_grid`/zero-crossing vector. The build blocks (`risk::capacity_curve`, `cost::capacity_point`, `cost::capacity_for_book`, `cost::emit_capacity_scorecard`) already exist unused by the report.

**Verified-already-fixed / out-of-scope (no unit spent on these):** the p6-S4 admission-gate cost
(`combine/gate.hpp`, `cost_adjusted_fitness`) is shipped/merged — a different decision point
(threshold on an already-picked candidate) from S4-4's search-selection scalar. The √-law cost
MODEL (`cost/cost_aware.hpp` `round_trip_cost_bps`) is the ONE cost model (C6) — S4 does not add a
second impact formula; it fixes the model's INPUTS (participation, B1) and REACH (ScalarRaw +
default-on profile, B7).

## CostSelectionConfig (S4-0 plumbing)

New header `atx-engine/include/atx/engine/cost/cost_selection_config.hpp`:
```cpp
struct CostSelectionConfig {
  bool     impact_in_selection = false;  // inert => raw unchanged (gross selection, today)
  atx::f64 selection_aum       = 0.0;    // AUM the selection cost is priced at; 0 => off
  atx::f64 cost_weight         = 1.0;    // multiplier on the net-of-cost penalty when on
};
```
Pure addition, no existing call site reads it. `factory::FitnessCfg` (fitness.hpp, S5-owned) does
NOT gain a field yet — S4 must not edit fitness.hpp.

**SEAM (binding, for Sprint 5):** Sprint 5 adds `cost::CostSelectionConfig cost_selection{};` to
`FitnessCfg` (fitness.hpp, alongside the existing `target_aum`/`cost` fields, ~line 357) and one
call site in `detail::finish_report` (fitness.cpp) that forwards `cfg.cost_selection` + the
already-computed `core.cost_bps` (re-priced at `cfg.cost_selection.selection_aum` via
`book_cost_bps`, see S4-4) into `factory::apply_selection_cost` (the S4-4-shipped function,
`factory/fitness_cost_selection.hpp`). Until S5 lands that field+call, S4's own on-path tests
construct `CostSelectionConfig` directly and call `apply_selection_cost` — this is the documented,
intentional gap (S4-4 section below).

## Unit checklist
- [x] S4-0  Ledger + bug-verification table + `CostSelectionConfig`
- [ ] S4-1  Capacity participation unit fix (B1)
- [ ] S4-2  Cap-clip-renorm re-center (B2)
- [ ] S4-3a Limit fill clamp (B3)
- [ ] S4-3b Absent-instrument volume zeroing (B4)
- [ ] S4-3c Borrow accrual in the loop (B5)
- [ ] S4-3d Permanent impact persistence (B6)
- [ ] S4-4  Impact in ScalarRaw selection (B7)
- [ ] S4-5a GP turnover-native step (B8)
- [ ] S4-5b stage_report capacity curve (B9)

## Determinism contracts (both apply this sprint)
- **(A) Opt-in** (S4-4, S4-5): inert default ⇒ byte-identical. `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
  `FactoryOos.MineIntoOffPathDigestUnchanged`, optimize/report goldens unchanged on the no-flag path.
- **(B) Correctness fix — documented exception** (S4-1/2/3): the fix changes the number because the
  OLD number was wrong. Any golden encoding the bug is re-baselined WITH the fix commit's SHA as the
  authority; recorded below (name, old digest, new digest, fixture). NO silent drift.

## Log
S4-0: complete. Ledger + bug-verification table written; `CostSelectionConfig` added
(`cost/cost_selection_config.hpp`, new file); `cost_selection_config_test.cpp` (`tests/core/`) pins
the inert defaults `{false, 0.0, 1.0}`. Pure addition — no existing call site changed, no golden
touched.
