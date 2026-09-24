# Goal: run the atx alpha-engine swarm (plan v2), waves W0 → W4, then stop before W5

You are the **orchestrator** of a multi-agent swarm. I am explicitly asking for multi-agent orchestration: use the Workflow tool (one workflow per wave batch) together with Agent subagents to run lanes in parallel. A wave may use about 30 agents (implementers, reviewers, fixers). That is intended.

## 1. Read first (authoritative, in this order)

1. `C:\atx\docs\plans\2026-09-24-alpha-engine-production-swarm.md`: plan v2. It defines the waves, tracks and lanes, and each lane's owned files, suites and acceptance criteria. §5 holds the swarm rules, §6 the DAG, §9 the batches.
2. `C:\atx\docs\plans\2026-09-24-alpha-engine-review-findings.md`: the defect register (the IDs each lane cites, with file:line) and the research digest.
3. `C:\atx\CLAUDE.md`, `.agents/cpp/agent.md` (house style, safety rules, review checklist) and `.agents/harness/TEMPLATES.md`.
4. `atx-vol/docs/LEDGER.md`. Grep it before re-deriving anything.

Both plan docs are **untracked** in `C:\atx`. Setup step 4 commits copies into the W0 integration branch so every lane worktree has them.

## 2. Owner rulings (in force; plan §3)

- **R-1:** 2013–2019 is development data only (walk-forward + CPCV + PBO). The "2019 holdout" label is retired. The pre-registered out-of-sample split is validation 2020-01..2022-12 and final 2023-01..2025-12, each opened exactly once and only by me.
- **R-2:** open a new trial-registry epoch at W1 that imports all prior trials (cp14–cp22 sidecars, L9 2065, L10 120). DSR uses cluster-N over the cumulative registry.
- **R-3:** no licensed estimates or borrow data. SUE comes from the XBRL seasonal random walk plus 8-K Item 2.02 timing. Borrow uses a tier model built from public predictors.
- **R-4:** the plan §1 book defaults are the pre-registered W5 parameters ($1bn AUM, gross ≤ 2× NAV, dollar-, beta- and industry-neutral, price ≥ $5, ADV ≥ $5m, common stock only).
- **R-5:** R16-8 is a single-family screen only. Combined books are gated on walk-forward net SR with a HAC CI, plus PBO, plus cluster-N DSR, using the bars set by W1-G1.

## 3. Owner overrides of house rules for this series

- **No test-driven development.** The TDD / red-first requirement in `.agents/cpp/agent.md` and the superpowers TDD skill are waived.
  - Lanes implement first, then add tests that prove every acceptance item.
  - Tests are still mandatory. Acceptance is still measured against the plan. There is no red-commit ceremony.
  - Never weaken, skip or delete an acceptance test to make it pass.
- **Reuse the existing pool worktrees.**
  - Do not create new pools. Never use raw `git worktree add`.
  - Move lanes between the warm pools by release plus re-lease (a warm branch flip takes about 27 s). Merge the integration branch or main into lane branches (§6).
- **Fast-forwarding local `main` at each wave gate is authorized.** Never push to origin.

## 4. Environment facts (verified 2026-09-24)

- **Branch state.**
  - `main` = `2e0d738f2a866b1f253e400ee386ef3e0674cd15`. It contains swarm lanes 1–5 and 7–10.
  - Lane 6 is **not merged**: `feat/qps-l6-optim` @ `1cf59cb7cd52ec92f99c4b8566c467f0d7527926`, in pool-7. Its review-fix pass is done.
- **`C:\atx` belongs to another active session.**
  - That session is on branch `feat/tier1-parity`, the tree is dirty, and it is running atx-db migrations.
  - Never switch branches, stash, reset, build, commit or write files in `C:\atx`. The only thing you do there is read the plan docs.
  - All work happens in `C:\atx-wt\pool-N`.
- **Pools.** `pool-1..pool-11` exist and are warm (build dirs plus ccache). All are LEASED to dead owners from the previous qps swarm. Old run ids:

  | Pool | Old run id |
  |---|---|
  | pool-1 | `swarm-20260922` |
  | pool-2 | `qps-l1-kernels` |
  | pool-3 | `qps-l2-vmcache` |
  | pool-4 | `qps-l3-search` |
  | pool-5 | `qps-l4-mtest` |
  | pool-6 | `qps-l5-combine` |
  | pool-7 | `qps-l6-optim` |
  | pool-8 | `qps-l7-riskmodel` |
  | pool-9 | `qps-l8-e2e` |
  | pool-10 | `qps-l9-realmine` |
  | pool-11 | `qps-l10-fundzoo` |

  - Every qps branch except l6 is already in `main`.
  - Each pool has an untracked `SWARM_STATUS.md`. pool-1 also has `SWARM_PLAN.md` and `SWARM_WAVE2.md`.
- **Host.** 16 GB RAM, shared.
  - At most **6 concurrent lanes**, at most **2 RAM:heavy**.
  - Heavy runs serialize through the lock file `C:\atx\data\.heavy-run.lock`. Creating that one file is the only write allowed under `C:\atx`, because it is data coordination, not the checkout.
- **Builds.**
  - Only `powershell scripts\atx-build.ps1 configure | check <file> | build <target> | -Ctest -R <Suite>`, run inside the lane's pool.
  - Never build all targets bare. Never run full test labels inside a lane.
  - Use `powershell` 5.1; `pwsh` may be absent.
- **Data.**
  - Never read anything dated ≥ 2020-01-01.
  - `atx-db/data/warehouse.duckdb` is read-only for this series: `read_only=True` connections, no writes, no migrations, and never kill a process holding it (R20-1).
  - Track-D lanes publish their outputs as new versioned artifacts under `C:\atx\data\<name>_<date>\`, with a hash-bound manifest published last.
  - New atx-db Python goes into **new modules**. Do not edit the atx-db files or migrations that `feat/tier1-parity` touches (`git diff --name-only main...feat/tier1-parity -- atx-db`).

## 5. Setup (once, before W0)

1. **Check status.** Run `powershell scripts\lease-worktree.ps1 -Status` from `C:\atx-wt\pool-1`, and confirm every keeper pid listed above is dead.
2. **Archive the qps notes.** Copy each pool's untracked `SWARM_STATUS.md` (and pool-1's `SWARM_PLAN.md` and `SWARM_WAVE2.md`) into a scratch folder. They are committed in step 4 under `.superpowers/sdd/w0/qps-archive/pool-N/`. Then delete the originals.
3. **Release every stale lease** with its own run id: `-Release pool-N -RunId <old-run-id>`. Add `-RecoverStale` only if the release is refused for owner state **and** you have confirmed the keeper is dead.
4. **Create the integration tree.**
   - Lease pool-1 with: `-Branch feat/w0-integration -Base 2e0d738f2a866b1f253e400ee386ef3e0674cd15 -Agent orchestrator -RunId aes-w0-integ -HeartbeatId aes-w0-integ-<utc-stamp>`.
   - Commit these to it:
     - both plan docs, copied to `docs/plans/`
     - the qps archive
     - `.superpowers/sdd/w0/progress.md` with lane, pool, owned files, cited defect IDs, suites and state
   - This commit is the **W0 base**. W0 needs no `.cpp` stubs.
5. **Lease the lane trees.** For each W0 lane, lease a released pool with `-Branch feat/w0-<lane> -Base <W0 base sha> -Agent <lane> -RunId aes-w0-<lane> -HeartbeatId aes-w0-<lane>-<utc-stamp>`.
   - Keep pools close to their previous build groups to maximize ccache hits:

     | Pools | Lanes |
     |---|---|
     | pool-2, pool-3 | A-track |
     | pool-4 | D0 |
     | pool-5, pool-6 | E-track |
     | pool-7 | O1 (lane 6) |
     | pool-8 | R-track and L0 |
     | pool-9 | B-track |
     | pool-10, pool-11 | I-track and heavy lanes |

   - Reconfigure `ATX_TEST_GROUPS` only when the lane's groups differ from what the tree already has.

## 6. Reuse and merge-main protocol (every wave)

- **Pools persist across waves.** When a lane merges, release its lease. At the next wave start, re-lease the same pool as `feat/w<N>-<lane>` from the new wave base (the integration head the previous gate fast-forwarded `main` to).
- **Pull producers mid-wave.** When a lane needs code from a producer lane that has already merged into `feat/w<N>-integration`, run `git merge --no-ff feat/w<N>-integration` in the lane worktree. Never rebase a shared branch. Then rebuild the target and continue.
- **Pre-merge before review.** Before asking for review, every lane merges the current `feat/w<N>-integration` into its branch, resolves conflicts inside its own files, and re-runs its suites. The orchestrator's merge should then be conflict-free.
- **Lane 6 (W0-O1).**
  1. In pool-7, lease `feat/w0-o1-l6` from the W0 base.
  2. `git merge feat/qps-l6-optim`. It was last synced at `82df1513`, so expect conflicts only in risk-owned files.
  3. Run the O1 acceptance.
  4. O1 merges into `feat/w0-integration` **first**, before any R-track lane, and every other W0 lane then merges integration into its branch.

## 7. Per-lane contract

- **Brief.** The plan's lane section, the cited findings rows, owned files, suites, acceptance criteria, host tag and pool.
  - Briefs, reports and reviews live at `.superpowers/sdd/w<N>/lane-<id>-{brief,report,review}.md` on the integration branch.
- **Cycle:**
  1. Implement.
  2. Add tests that prove each acceptance item.
  3. Anchored `-Ctest -R <Suite>` green, plus the must-stay-green suites the plan names.
  4. Merge integration into the lane branch.
  5. Review by a **fresh adversarial reviewer**. It is read-only and checks:
     - the `.agents/cpp` checklist
     - every acceptance item against real output
     - that every cited defect ID is closed or explicitly deferred
     - file ownership
     - causality-harness registration (from W1 onward)
     - that no acceptance test was weakened
  6. Fix pass.
  7. Fix-only re-review.
  8. The orchestrator merges into `feat/w<N>-integration`.
- **Rules:**
  - Touch only owned files; anything else becomes an integration note.
  - No `CMakeLists.txt` edits after the wave's Lane 0.
  - Test helpers go in `namespace atx_test_w<N>_<lane>_<file>`.
  - Any changed numeric default keeps the old behavior behind a versioned enum.
  - Golden-digest re-baselines record an old→new table in the report.
  - Bench numbers count only on `equity-bench` with at most 1 other lane running.
  - Report honestly: an unmet acceptance item needs my written waiver in `progress.md`.

## 8. Wave gate

The gate requires:
- All touched test targets green.
- The causality harness green (from W1 onward).
- `scripts/bench-gate.ps1` shows no regression over 20%.
- Ledger lines appended **by the orchestrator only**: bench numbers, real-data results, registry N / cluster-N, decisions.

Then:
1. Fast-forward local `main`: `git -C C:\atx-wt\pool-1 fetch . feat/w<N>-integration:main` (fast-forward only; stop and report if it isn't one).
2. Append a wave section to `atx-engine/docs/QUANT_PLATFORM_SWARM_STATUS.md`. It is a log; never rewrite earlier sections.
3. Release lane leases, then lease the next wave's integration tree (pool-1, `feat/w<N+1>-integration`) from the new `main`.
4. Commit that wave's Lane 0 (stubs, dispatch lines, progress.md).

## 9. Run order and mandatory stops

- **W0**: 10 lanes in batches W0a and W0b (plan §9). Then **G0**, the truth-delta report: rerun the old results on the fixed code, change nothing.
  - **STOP** and report to me: the W0 summary plus the G0 old-vs-new table.
  - Resume on my go.
- **W1 → W2 → W3 → W4** run autonomously, following the plan §6 DAG and the §9 batches.
  - Start W1-D1 (the PIT security master, the pacing lane) in the first heavy slot.
  - Check its coverage table mid-wave.
- **STOP** before W5. Present the evidence-readiness checklist (plan §8 preconditions). The canonical run can proceed on my go. The 2020–22 unseal is my decision alone.
- **Also stop and ask** when:
  - a blocker survives two fix passes
  - there is any data-discipline question
  - anything would need write access to `warehouse.duckdb`
  - anything would need a change in `C:\atx`
  - a lane needs files owned by another track in the same wave and an integration note cannot resolve it

## 10. Reporting

- After each batch, give a short table: lane, pool, state, acceptance met/unmet, key numbers, defect IDs closed.
- Keep `.superpowers/sdd/w<N>/progress.md` current. It is the resume point: a fresh session must be able to continue from it alone.
