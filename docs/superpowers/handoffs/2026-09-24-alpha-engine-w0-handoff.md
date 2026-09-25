# Alpha-engine swarm — W0 hand-off (paused mid-W0a, 2026-09-25T01:08Z)

The owner asked to stop here. This file plus `.superpowers/sdd/w0/progress.md` is the resume
point. The goal prompt is at `docs/plans/2026-09-24-alpha-engine-swarm-goal-prompt.md`, the plan
at `docs/plans/2026-09-24-alpha-engine-production-swarm.md`, and the defect register at
`docs/plans/2026-09-24-alpha-engine-review-findings.md`. All three are committed on
`feat/w0-integration`.

## 1. Summary

- **Setup is complete.**
  - All 11 stale qps leases were released after confirming every keeper was dead.
  - The qps notes are archived.
  - The W0 base `458d0bef` was committed.
  - All 10 W0 lane trees were leased.
- **The W0a workflow ran six lanes and was stopped by the owner.**
  - **O1 is merged** into `feat/w0-integration`.
  - **L0, D0, E0a and E0b are approved.** Each passed an adversarial review, one fix pass and a fix-only re-review. Only the final sync and merge remain.
  - **A0 is mid-fix.** Its review returned BLOCK with 1 major finding, and fix pass 1 was interrupted.
- **Not started:** W0b (R0, B0, I0a, I0b; leased and idle), G0, and the W0 gate.
- **`main` is unchanged** at `2e0d738f`. Nothing was pushed. `C:\atx` was never touched: only the plan docs were read, plus `git rev-parse` of HEAD.

## 2. Where the code is

| Item | Value |
|---|---|
| Integration | `C:\atx-wt\pool-1`, `feat/w0-integration` @ `2cdf1277` (W0 base `458d0bef` → O1 merge `14ce9172` → progress commit), plus this hand-off commit |
| W0 base | `458d0bef480a624e258070c9d45174a9984466bf` (plan docs, goal prompt, briefs, RULES, progress, qps archive) |
| Pre-W0 `main` | `2e0d738f2a866b1f253e400ee386ef3e0674cd15` (untouched; fast-forward happens only at the W0 gate) |
| Lane briefs and rules | `.superpowers/sdd/w0/lane-<id>-brief.md`, `RULES.md` (binding for every lane agent) |
| W0a workflow script | `.superpowers/sdd/w0/aes-w0a-workflow.js` (parameterised by `args.lanes`; reusable for W0b) |
| G0 run-book | `.superpowers/sdd/w0/g0-runbook.md` (prepared read-only by a subagent; flag names inferred, see §6) |
| Brief generator | `.superpowers/sdd/w0/gen_briefs.py` (orchestrator tool; reads the plan and findings) |

## 3. Lane status (W0a)

| Lane | Pool | Lane head | State | Review history | Acceptance | Defect IDs |
|---|---|---|---|---|---|---|
| O1 | pool-7 (lease **released**) | `3ccf012c` | **MERGED** @ `14ce9172` | APPROVE (2 minor) → fix 1 → re-review APPROVE | 6 MET; quiet-host bench baselines DEFERRED to gate (by design) | R-14: code fix deferred to W2-R3; ledger wording supplied |
| L0 | pool-3 | `86bb8bdb` | approved; final sync + merge pending | BLOCK (1 major: `nn::train` RowGroups stale-val abort) → fix 1 → re-review APPROVE | 7/7 MET; learn 187/187 | L-01, L-02, L-03, L-07, L-08 (count, IcLoss, blend) CLOSED; L-08 autoencoder part DEFERRED to W3-L4 |
| D0 | pool-4 | `531f73c5` | approved; final sync + merge pending | APPROVE (4 minor) → fix 1 → re-review APPROVE (2 minor) | 4/4 MET; data 235/235 (11 real-data tests filtered, see §5) | D-01, D-02 (engine), D-03, D-04, D-05, D-06 (rebase), D-08, D-09 CLOSED |
| E0a | pool-5 | `e365e0e0` | approved; final sync + merge pending | APPROVE (5 minor) → fix 1 → re-review APPROVE | 5/5 MET (one needs an owner reading, §5); eval 222/222, combine 183/183 | E-02, E-03, E-08, E-09 (engine), E-15 CLOSED |
| E0b | pool-6 | `35198851` (already contains integration `2cdf1277`) | approved; **sync was interrupted after its merge commit**; merge pending | APPROVE (3 minor) → fix 1 → re-review APPROVE | 3/3 MET (one needs an owner reading, §5); eval 221/221 before sync | E-01, E-16 (registry), E-17 CLOSED; L-08 registry side CLOSED |
| A0 | pool-2 | `b74e27d3` + **uncommitted** `lane-a0-report.md` edit | **fix pass 1 interrupted** | BLOCK (1 major + 5 minor) | 8/8 MET at implement; alpha 703/703, factory 299/299 | A-01, A-02, A-03, A-09, A-13 CLOSED; A-18 CLOSED in the engine but the search driver is still hash-only (integration note) |

A0 golden digests (old → new, each reproduced bit-for-bit under its V1 enum): rank A-01
`0xa50ec3743580856b → 0xdd31545d3a5ad696`; hump A-02/A-13
`0x0a9ce0c6fb27f23e → 0x3159e020352402f8`; the full table is in `lane-a0-report.md`.

### Tree residue left by the interrupted agents (fix these first on resume)

- **pool-6 (E0b):** the merge commit `35198851` (parents `3356721a` + `2cdf1277`) is complete, but a stale `MERGE_HEAD` remains. `git status` reports "merge in progress, no conflicts, nothing to commit".
  - Clear it with `git -C C:\atx-wt\pool-6 merge --quit`. This is non-destructive.
  - Then re-run the E0b suites on the merged tree. Post-merge results were never recorded.
- **pool-2 (A0):**
  - The fix commit `b74e27d3` ("CanonSet hash-only key, quantile int range") landed.
  - The report's fix-pass section is still uncommitted (+67/−6 lines in `lane-a0-report.md`).
  - Next: finish fix pass 1 (verify the fix, run the suites, commit the report), then run the fix-only re-review.
- No build or test processes are left running (checked). There are no other dirty trees.

## 4. Leases (all heartbeat keepers alive at the stop)

| Pool | Branch | Run id | Note |
|---|---|---|---|
| pool-1 | feat/w0-integration | aes-w0-integ | integration tree |
| pool-2 | feat/w0-a0 | aes-w0-a0 | dirty (see above), so release is refused until the report is committed |
| pool-3 | feat/w0-l0 | aes-w0-l0 | |
| pool-4 | feat/w0-d0 | aes-w0-d0 | |
| pool-5 | feat/w0-e0a | aes-w0-e0a | |
| pool-6 | feat/w0-e0b | aes-w0-e0b | stale MERGE_HEAD |
| pool-7 | (detached @ base) | — | **free** (O1 released) |
| pool-8 | feat/w0-r0 | aes-w0-r0 | W0b, idle at base |
| pool-9 | feat/w0-b0 | aes-w0-b0 | W0b, idle at base |
| pool-10 | feat/w0-i0a | aes-w0-i0a | W0b, idle at base |
| pool-11 | feat/w0-i0b | aes-w0-i0b | W0b, idle at base |

- **Keepers.** The keepers are independent processes. They keep each lease "alive" until it is released with the same run id: `scripts\lease-worktree.ps1 -Release pool-N -RunId <run id>`, run from pool-1.
- **Resuming vs releasing.** A resuming session can keep using the held leases. It should not re-lease them.
- **Lowest free slot.** `lease-worktree.ps1` always takes the **lowest free slot**, so pool placement depends on lease order.

## 5. Owner decisions needed

**Waivers and readings raised by the reviewers:**

1. **E0a, MA(20) coverage.** The acceptance item "95% CI coverage in [93%, 97%]" is met by the new
   HAC interval `ic_mean_hac`, which measures 94.5% over 2000 repetitions. The default bootstrap
   interval `ic_mean_ci` measures about 92.5–93%. Please confirm that the HAC interval satisfies
   the item.
2. **E0a, combiner-level V1 reproducibility.** Getting per-combiner `IidV1`/`RawV1` and a
   horizon-aware lag floor at the combine sites needs `combine/signal_combiner.hpp` and
   `orthogonalize.hpp`, which E0a does not own. This is recorded as an integration note, and the
   natural owner is W2-E3. E-15 also changes GK/ICIR-EWMA weights, not only t-stats.
3. **E0b, DSR false-positive rate.** Acceptance 1 (FPR 5% ± 1% on an equicorrelated null) is met by
   the opt-in `AccountingDsrRule::MonteCarloMaxV2`. The default `ClusterMcFloorV2` is conservative
   by construction (about 1%). Please rule which rule the item refers to.
4. **D0, vwap deviation.** vwap stays the adjusted-basis typical price, tagged `adjusted_level`,
   instead of `raw_close × volume` as the brief asked. The lane gives its reasoning in its report.
   This needs a waiver or a rework.
5. **D0, real-data items handed to G0.** 14 existing real-data engine tests were excluded in the lane:
   `DataRealPanel.*`, 5 `DataCorporateActions` smoke tests, the `DataAdjust` AAPL split test,
   2 `OratsE2ESmoke`, `DataUniverse` survivorship, and `SharesOutstandingPitForwardFill`.
   - The real-panel smoke golden digest `0x2a22a873483d9157` (`data_real_panel_e2e_test.cpp:169`) will move because of D-03, D-04, D-05 and D-09.
   - Some of these read **2024** data, which W0 data discipline forbids.
   - Please decide whether G0 may run them (≥ 2020 data) or whether they stay excluded or waived.
6. **A0, `streaming_engine.hpp` edit.** A0 edited `alpha/streaming_engine.hpp`, which no W0 lane
   owns. The edit is needed to keep the stream==batch contract green in the alpha target. It
   needs a retroactive ownership grant.
7. **R0, sanitizer requirement.** The acceptance item says to run the missing-group test under
   UBSan/ASan, but no sanitizer preset exists (`agent.md` §8). R0's brief uses Debug checked
   iterators as a substitute and marks the item for waiver.

**Orchestrator decisions to confirm:**

8. **Ledger path.** `atx-vol/docs/LEDGER.md` was deleted from `main` in `e4bdcf54`
   (2026-09-19, "clean up"). Ledger lines are planned for a new `atx-engine/docs/LEDGER.md`. None
   have been written yet.
9. **qps notes.** Deleting the originals was refused by the permission classifier. They were
   **moved** to the session scratch dir `...\scratchpad\qps-archive-moved\pool-N\`, which may not
   survive the session. The committed archive copies are hash-verified.
10. **G0 and 2019 data.** R-1 retires the 2019 holdout, so G0 plans to reuse the old inputs
    including 2019 (for example the L9 `--holdout publish --holdout-prior-reads 4` path). No
    ≥ 2020 data is involved except the D0 item in point 5. G0 outputs go outside `C:\atx`, under
    `C:\atx-wt\g0-data\<name>_20260924\`, with the manifest written last. Confirm the location.
11. **G0 cp21 re-run.** `main` compiles 29 families, but cp21 ran 26. Three are cp22 `si_shares`
    families that the cp16 contexts cannot serve. The plan is a G0-only measurement branch, never
    merged, that pins the list back to cp21's 26 byte-identical entries.

## 6. Integration notes the W0b lanes must pick up

These come from the lane reports; the details are in each `lane-<id>-report.md`.

- **I0b (`stage_equity_ic.cpp`, from E0a):**
  - New defaults: execution_delay 1, `TwoHorizonV2`, `HansenHodrickV1`. The old behaviour is `HalfHorizonV1` with delay 0.
  - `common_sample_dates` must subtract `eval::label_embargo(maxH, delay)`.
  - Update the `kAlignment` (:112) and `block_len_rule` (:334) strings, and `trial_ledger.hpp:89-90`.
- **I0b (`stage_equity_mine.cpp`, from E0b):**
  - Set `pnl_len` to the train+validation calendar (:1739).
  - Record trials with `TrialMeta{window, fidelity, InSample, family, theme}` (:742).
  - Take cluster-N DSR through the `TrialAccounting` overload.
  - Export `chain_head()` to the report and manifest.
- **I0b (config, from D0):** `si_publication_lag = 7` NYSE sessions, with `FinraLagRule::NyseSessionsV2` and `after_close=true`. The int overload still means calendar days.
- **Registry (from L0):** a learned model's `trial_count` is now 1 per configuration (`TrialCountRule::PerConfigurationV2`).
- **Search driver (from A0, owned by W2-A4):** `search_driver.cpp` must call `CanonSet::insert/contains(h, form)` and key `fitness_cache` by (hash, form). Until then A-18 stays collision-blind in production.
- **CRLF hazard (from O1):** with `core.autocrlf=true`, `atx-engine/reviews/trial-ledger.jsonl` checks out as CRLF, so `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` fails when run from the repo root. Suggested fix: `*.jsonl text eol=lf` in `.gitattributes` plus a renormalize. Nobody in W0 owns that file. `CMakePresets.json` must keep its CRLF working-tree line endings.
- **R0:** starts by merging integration, which contains lane 6.

## 7. Resume procedure (in order)

1. **Check the environment.**
   - `powershell scripts\lease-worktree.ps1 -Status` from pool-1: expect the §4 table.
   - Check that `git -C C:\atx rev-parse --abbrev-ref HEAD` is still `feat/tier1-parity`. Never write in `C:\atx`.
   - Check free RAM. It was 4–5 GB during W0a, with the other session running migrations.
2. **Clean the residue (§3).**
   - pool-6: `merge --quit`.
   - pool-2: let the A0 fixer finish.
3. **Finish W0a.** Reuse `aes-w0a-workflow.js`, adapted so that each lane resumes at the right stage:
   - **L0, D0, E0a:** final sync (merge integration, which now contains O1), re-run the owning targets and suites, then the serialized merge.
   - **E0b:** re-run suites on the already-merged tree (the sync agent step), then merge.
   - **A0:** fix pass 1 (finish), fix-only re-review 1, a second round if needed, final sync, merge.
   - A blocker that survives two fix passes means **stop and ask the owner**.
   - The merge agent updates `progress.md` and releases the lease (the §6 protocol).
4. **Run W0b.** Run the same workflow with `args.lanes` set to R0 (pool 8), B0 (9), I0a (10) and I0b (11), branches `feat/w0-<id>` and titles from the briefs. Every implementer first merges `feat/w0-integration`.
5. **Run the W0 gate** in pool-1:
   - Reconfigure `build-equity` with every touched group (`alpha;factory;learn;data;eval;combine;risk;book`) plus atx-impl-tests, and build target-scoped.
   - Run each touched executable whole.
   - Record the O1 quiet-host bench baselines (the `equity-bench` preset, with ≤ 1 other lane running) and run `scripts\bench-gate.ps1` (> 20% regression fails).
6. **Run G0** (heavy; hold `C:\atx\data\.heavy-run.lock`) per `g0-runbook.md`.
   - First re-check its inferred flag names against the merged I0b and B0 code: as-of membership, execution delay, min names, `--allow-same-close`, delisting policy, audit path.
   - Publish an old-vs-new table with defect attribution.
   - Append an I-24 errata section to `atx-engine/docs/QUANT_PLATFORM_SWARM_STATUS.md`. Do not edit earlier lines. True values: validation −1.219 (p 0.95), holdout 1.730, family mean −0.479, 2019 "reused" with 3 prior reads.
7. **STOP for owner review** (the mandatory stop): the W0 summary, the G0 table, and the waiver list in §5.

## 8. Operating facts learned this session

- **The `dev` preset is broken** on this base, because atx-vol was removed and the root CMake still references it. Lanes use `equity-dev` (`build-equity\`), and Release uses `equity-rel`.
- **Lease exit code 1 is misleading.** `lease-worktree.ps1` exits 1 after a valid lease because its cold-tree step configures `dev`. Verify with `-Status`.
- **Build parallelism.** `CMAKE_BUILD_PARALLEL_LEVEL=2` per lane build kept six concurrent lanes at 4–5 GB free, with no OOMs.
- **cwd resets.** The Bash and PowerShell tools reset cwd to `C:\atx` after every call. Every agent prompt pins `git -C <pool>` and `Set-Location <pool>;` for that reason, and no lane touched `C:\atx`.
- **Runtimes.** W0a took about 1 h 50 min wall clock for implement → review → fix → re-review on five lanes. A0 was the slowest implementer.
