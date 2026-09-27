# Goal prompt for the next parent agent

Resume building atx-db toward production-quality, large cross-sectional, deep-history characteristics with statistically evaluated forward 1–12 month evidence for a downstream equity long/short book. Build the required tools, data and pipelines. The objective remains the full Tier-1 v2 index §2 exit criteria and §5.7 candidate release, not just the next code task. Use independent subagent implementation/review lanes within the actual runtime capacity. No TDD; prefer measured real-data acceptance.

Read **`docs/superpowers/handoffs/2026-09-27-tier1-v2-session-5-handoff.md` first**, then the spec `docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md` §§2–6 and9, and `.superpowers/sdd/tier1-v2/{CONTEXT,REVIEW-CONTEXT,CARRY}.md`. Ledger `.superpowers/sdd/tier1-v2/progress.md` plus git log are authoritative. Read each task's latest Progress/Stopped(session5) section before its historical text.

Resume, do not restart. Session5 stopped explicitly at the user's request; all worker/fetch jobs stopped. Code HEAD at stop was `47e168cd`; a later handoff-doc commit is expected. The SEC fetch STOP file is retained. The goal was paused, not achieved. No real wave was registered or graded; the committed trial anchor is zero bytes and the default registry absent. Both1.12 and4.1 prerequisite reviews passed immediately before the stop, so the first real price-wave grade is now the top priority after runner preparation.

## First actions

1. Reconcile `git log --oneline 8aba655c..HEAD` and `git status --short -- atx-db` with the handoff's commits and25-path WIP table. Preserve every file. Nine3.3 paths, oneF.1 source verifier and the1.13 runner are current-session partial work;1.3/2.2/X.1/X.2 are preserved earlier WIP.
2. Check actual processes, disk and protected DB metadata. No pipeline job was alive at handoff; VS Code services are not ours. HEAVY is free. Reclaim dead guard slots only through the guard. No full production clone remains; do not mistake derived identity preparation for one.
3. Record session6 in task reports and keep C94's after-every-milestone updates. Preserve all earlier rulings; next new ruling is C105 unless git/ledger shows a later one.
4. Dispatch the priority lanes below, with explicit ownership and independent review. Runtime session5 had only four total threads including root and no Opus override; C96/C98 record the adaptation. Prefer fewer deep workers to shallow oversubscription; never let an author acceptance-review its own task.

## Priority A — 1.13 real price-wave evaluation

Read task-1.13 brief/report/CARRY, task-1.12 report's exact Relay, task-1.12-rereview-session5.md, and task-4.1 report/Relay/re-review. Adopt untracked `atx-db/scripts/research_run_wave.py` first; no result doc exists. Its new compact accounting adapter is **unmeasured WIP**, not approved by the underlying evaluator review. Finish justified stub/PIT validation, registry-before-read, exact endpoint/seal checks and source/code pinning.

Accepted price catalog is already committed in `47e168cd`; no catalog rebuild is needed. Pins:

- Catalog digest `36bb1bbe472b1bed0371a1f4bb1cb5ebcdc17132669d0bf341e5f5ffaad82654`.
- Feature manifest ID `58aeacf3967548de9e7e8cc7c30ce830bca5b7df8a1a3f8eb1a12e537edab4de`.
- Lake `price-wave-0ed96b2696f1-bcdf5d0f3437`.
- Labels `eaf6c8b9450a7fa3f1f282717a44f9ae6f502a2c424d6185199cab317f08c757`.
- Benchmarks `bench-20260926`; policy SHA `776db445df6323c5d0dfd7db8e080631d94665c6a84e548f4b1d01b148546d9a`.
- EVAL frozen at `41168d6f`; FEAT for accepted store is `50caac93`.

Print/check then register **exactly36×3×4=432 cells** for w1_price; commit `src/atx_db/seeds/research_trial_registry_anchor.jsonl` before any real label-value read/join. Three served variants: rank_normal/signed_raw/zscore. Exclude the two unsupported zero_trade rows underC82; retain designated reported-only EW beta/ivol rows within the counted grid. Beta-return registration remains None. Catalog first formations use min_history_sessions, not return outcomes.

Selection formations2013-01..2023-12, all mature endpoints strictly before2024-01-01. Holdout stays sealed. Labels actually span next-session calendar-month entry to entry h calendar months later, not fixed-session windows. The accounting adapter may reuse already-authorized returns only when both entry endpoints independently match; pin it and withhold unmatched intervals. Complete-book returns must not be survivor-renormalized; sparse economics stay NULL with counts.

Start **one feature per worker** under0.6GiB, nested orchestrator0.2; measure the entire read/cache-key/evaluator/accounting path. One frozen EvaluationSpec per run/resume. Hand first pinned real cache to4.1 for required byte-identical old-gate/memory evidence; EVAL never changes during the real run. Write `atx-db/docs/research/WAVE1_PRICE_PROVISIONAL.md` and independent review. Status ceiling selection_pass, not qualified. All4,500-line coverage claims remain **NOT MET**; maximum universe4,147. Do not weaken the target.

## Priority B — F.1 item engine → F.2 fundamentals wave

Read approved `docs/superpowers/plans/2026-09-27-tier1-v2-fundamentals-fast-path.md`, F.1/F.2 briefs, design review, implementation stopped report andC104. Adopt only untracked `research/fundamental_sources.py`; source integrity audit already passed361 files at0.0944GiB. No map/vintage/identity engine or build CLI exists yet. Continue implementation; do not repeat unchanged source verification as a substitute for progress.

Both sources are accepted: explicit FSDS `data/staging/fsds-v2` with69quarters (canonical fsds is stillv1), and CF-R `data/staging/companyfacts/ee099c7394a357f1` with85batches/20,390members/53,751,273facts. Exact hashes are in the handoff and F.1 source-pins receipt.

Implement the shared2.9 Parquet item engine with bounded slices, latest-NULL semantics, semantic conflict detection before row-ID tie-breaking, fiscal slots, proven durations, YTD/Q4/TTM rules and restatement propagation. FSDS accepted_utc; CF-only max(raw_clock,filed+46h); borrowing acceptance requires same-fact exact context/value corroboration, not accession alone. Preserve source/lineage and all real numeric/PIT/fiscal/oracle/resume gates.

Resolve documented RECT/AP/OANCF seed unit metadata inconsistencies explicitly; no STD seed edit or unit-authority choice was applied before stop. Owner items can build before identity; final projection waits for accepted3.3 and complete common-class company ME. >=3,000 owners/month remains unchanged.

C104 ratifies **w2_fund_fast**,23 requested canonical hypotheses/default276 cells. Dedup/formula/input pins and committed registry anchor before returns. Later2.10/w2_fund_a/w0 exclude these same hypotheses; changed constructs/clock/identity configurations count as new trials. Low-coverage buildable rows remain counted. Source-unsupported deferrals require pre-return evidence/controller decision. One final holdout opening.

## Priority C — 3.3 identity completion

Read task-3.3 stopped report and its exact nine-file WIP list. Preserve committed exporter/preparation (`dd83baec`,`7eaafe75`), OPS inputs and completed retained preparation.25,760 lines/1,000,917 share facts/12,650 evidence rows are measured; do not rebuild unchanged preparation.

`data/research/identity_rehearsal/session5-draft1` is **not accepted**, scope_complete=false, and predates fixes. Current class/uncertainty/PIT/publication/schema WIP was not rerun. Prove full company membership before tier filtering; high+medium cannot become complete high-only ME by dropping a class. Retain unknown/low/unresolved/cross-CIK ambiguity, and prove class evidence at each event uses no later facts or symbols. Fix/reject non-contiguous vendor symbol intervals; do not backfill current descriptors into history.

Finish corrected rehearsal,0331 part1/registry/API/schema integration, checksum proof and justified touched migration/PIT tests. `tests/test_migrations.py` ownership is granted but no edits exist yet. No0331 file or ledger adapter exists.1.3's batch_runner is uncommitted/unaccepted; fresh archives cannot depend on it. Keep standalone rehearsal independent and label ledger-stage acceptance pending1.3. Complete real delisted/multi-class/monthly coverage/no-reuse/uniqueness/repeatability acceptance before publishing F.1 projection inputs.

## Fetch and remaining DAG

- **X.5:** C85 fix/relaunch review approved. Fetch stopped on user request at01:35:41Z, roughly17,000/426,151 candidates reported. On this resumed goal, fresh current-HEAD archive, process/limiter/disk checks, remove only documented operator STOP and launch hidden with explicit exported Worker,0.5GiB cap/35GiBfloor. Never clear SEC trip implicitly. End-state peak0.3922GiB already measured; no repeat needed. Full fetch/repair/ledgered parse-load incomplete.
- **1.3:** next available implementation lane. Adopt five untracked files; finish kill/resume, commit, then OPS fresh full-copy drill (one copy, cleanup after use) and review. Unblocks warehouse ladder1.5/1.6/1.8 and dependent loads.
- **2.2:** preserved four-file PIT fix WIP still needs scoped finish/re-review.
- **X.1/X.2:** preserved five-file fetch WIP, no fetch launched. SEC confirmed SR-FINRA-2026-012 withdrawn2026-08-05; verify actual source/publication contract, not proposed daily SI rules.
- Then follow full DAG: fundamentals/market/final labels, external validation,4.3 QUALIFIED_SIGNALS, composites/ownership,4.6alpha export, release/operations. Completed tasks must not be redispatched.

## Standing constraints

All R1–R14/C1–C104 binding. Controller dispatches/records/packages/rules, not source implementation. One owner per serialized token; at most two fix rounds then adjudicate. Use dedicated editing tools (apply_patch in this runtime), never shell/Python rewrites. Prefer graph tools when available; unavailable graph fallback is recorded. Commit explicit owned paths with `[v2 <node>]` and trailer `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.

Every Python/pytest/DuckDB invocation under `run_memory_guarded.py`, OPENBLAS_NUM_THREADS=1. Research/probes/tests0.6GiB/DuckDB256MB/threads1; writers0.8; set-based1.0 max; orchestrators0.2 with allow-nested-guards; total caps<=2GiB. Pytest -n0. No real-run guard overrides, no unbounded full-table pandas, no bulk ART, no whole-stage transactions. Only OPS/HEAVY may open production, one fresh-export slice at a time. Resume137/78 or re-engineer; never raise limits to evade pressure.

>=35GiB free; one full DB copy max. Keep production and protected `.pre-migrate.20260924-225023.bak`. Do not touch atx-engine, atx-vol, root python/, C:/atx-wt, protected alpha-engine plans or other sessions' processes. Never push/merge/stash/reset/restore/clean/discard. The14.9MB controller determinism scratch remains because automatic approval review rejected deletion; do not bypass that rejection.

Snapshot2026-09-20/cutoff22:00UTC. SEC exact UA `atx-db/0.1 atx-research@example.com`, shared<=5req/s. U2/U3/U5/U6/U8/U9 granted files-only; U1/U4/U7/U10 not executed. No fabricated missing data, inferred history presented as verified vintage, narrowed denominators or weakened thresholds.

After every commit/measured run/stage, update the task's current-session Progress with commands, artifacts, peaks, remaining gaps and next step. Stamp ledger edits with actual `date -u +%H:%MZ`, immediately before the historical final11:50Z HOLD sentinel. Keep the original objective intact and do not mark complete until every exit criterion and candidate release is supported by current evidence.
