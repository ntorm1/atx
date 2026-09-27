# Next parent goal prompt — resume Tier-1 v2 after session 6

Copy the prompt below into the next parent agent's goal. The previous parent paused at the user's explicit STOP; using this prompt as a new instruction authorizes resumption. Reading the handoff alone does not resume the goal.

---

Resume building atx-db toward production-quality, large cross-sectional, deep-history characteristics with forward 1–12 month evidence for a downstream equity long/short book. Build the required tools, infrastructure and pipelines. Use parallel subagents with independent reviews, within the available runtime capacity. No TDD; pursue depth-first production outputs and measured acceptance. Continue until the spec's §2 exit criteria and §5.7 candidate release are satisfied, or all remaining work is blocked only by an unexecuted user gate/permission denial. Preserve unmet criteria honestly.

Read before acting:

1. C:/Users/natha/.codex/attachments/dafee98b-11ae-4f69-a4d6-4d69f2a02d3b/goal-objective.md.
2. docs/superpowers/handoffs/2026-09-27-tier1-v2-session-6-handoff.md in full. It supersedes older registration and WIP state.
3. .superpowers/sdd/tier1-v2/CONTEXT.md, REVIEW-CONTEXT.md, CARRY.md, and the recent progress.md ledger, plus git log.
4. docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md §§2,3,6,9 and relevant adjacent S0–S5/SX tasks. Use session-5-handoff.md for unchanged accepted evidence; do not restart completed work.

The goal-level intermediate minimum remains: 1.13 w1_price graded, F.2 or 2.10 fundamentals graded, 4.3 QUALIFIED_SIGNALS, and 4.6 alpha export. None is complete yet. The original full DAG/release objective remains in force.

## Critical state at the stop

- Code HEAD was fa2d500bda4fed6399ad2e22ac36901aa07fd35c; later commits may contain handoff documentation only. No session-6 source commit was made. There are 30 preserved atx-db WIP paths, fully mapped in the handoff. Keep every path and other sessions' changes.
- **w1_price already has exactly 432 registered cells. DO NOT REGISTER AGAIN.** Registration at 2026-09-27T12:18:48 has record ID 3afdfc36b2d257244375fa784d7dfe062ab6a00d31280f85cb26dfa546b3920c, sequence0. Actual registry path is atx-db/data/research/registry/trial_registry.jsonl, including the registry subdirectory.
- The working seed anchor is137 bytes/one record and UNCOMMITTED; its HEAD blob is still empty. The runner scripts/research_run_wave.py remains untracked. No real label-value read, prepared real EvaluationSpec, real evaluation cache or grade followed registration. Holdout is sealed.
- All three workers stopped; no owned pipeline/fetch/guard remained. X.5 was not relaunched. Its old operator STOP remains. IDE and other-session processes were untouched.
- 1.12 inputs are approved at47e168cd, but every required4,500-line target is NOT MET (maximum4,147). EVAL code/stub phase is approved and frozen at41168d6f; real Step2 remains pending. These are not full quantitative/release passes.
- F.1 design is approved; source verifier plus unexecuted mapping/candidate scaffolding exist. C105 unit authority is still pending. 3.3 has12 WIP source paths including new0331; small PIT/checksum proof passed, corrected real rehearsal and actual migration unrun.

## First actions, in order

1. Reconcile git log/status and exact WIP map, actual processes, disk and protected DB metadata. The source tree is shared; never bulk-stage/discard. Read existing registration and anchor without label values. Session6 handoff has exact record/file hashes. Preserve that registration; reconcile it through existing guarded API before use.
2. Confirm protected warehouse12,873,379,840 bytes/mtime2026-09-25T11:59:36.2668647Z and backup12,883,341,312/mtime2026-09-24T22:50:23.5699261Z. Last free space101.27GiB; enforce>=35GiB. No old1.3 full-copy cleanup remains. Retained identity scratch is derived data.
3. Inspect memory slots; reclaim only dead slots under a0.2GiB guard if needed. The prior session6 reclaim succeeded (peak0.0283GiB) and removed the dead fetch slot. Do not mistake stale receipt PIDs for live processes.
4. Append a timestamped reconcile/resume entry immediately before progress.md's final historical11:50Z HOLD sentinel. Read the goal status, resume according to the user's new instruction/runtime, and avoid creating a duplicate goal. Set the next report session number consistently; current CONTEXT says6. Every worker must update Progress(session N) after each milestone (C94).
5. Dispatch the following three disjoint priority lanes. Use at most three workers plus controller in the current four-thread runtime. Controller coordinates/reviews/rules/OPS and does not implement source. Reuse freed workers for independent cross-review; never accept an author's review of its own work.

### Lane A — 1.13, commit existing anchor then grade

Read task-1.13-report.md Progress(session6), accepted task-1.12 Relay/re-review, task-4.1 Relay/re-review, brief and CARRY. Own only runner, anchor and eventual WAVE1_PRICE_PROVISIONAL.md. Inspect the entire adopted runner and anchor diff.

The synthetic PIT/adapter probe already passed14 checks (202 rows,8 wrong endpoints withheld, sealed/source/pin/registration checks), peak0.1288GiB. Plan peak0.0965; registration peak0.1324; no cap hits. Do not repeat these unchanged just for reassurance. Runner's complete real path is still unmeasured.

Reconcile existing registration, then **commit reviewed runner plus existing anchor with explicit paths before any real return read**. No new catalog commit is needed;47e168cd already contains it. Do not use a scratch registry or alter trial counts. Exactly36 features × rank_normal/signed_raw/zscore ×1/3/6/12 =432; three reported-only IDs remain counted, two unsupported zero-trade IDs excluded under C82, beta-return opt-in remains off under C103.

Pin these accepted inputs explicitly:

- Catalog36bb1bbe472b1bed0371a1f4bb1cb5ebcdc17132669d0bf341e5f5ffaad82654.
- Manifest58aeacf3967548de9e7e8cc7c30ce830bca5b7df8a1a3f8eb1a12e537edab4de; serialized file SHA8c234060422e1b209bd1d3e6a7e32b1b7b5692d211ec578d2dce87ec8f62cb05.
- Lake price-wave-0ed96b2696f1-bcdf5d0f3437, digest9eeb25fe81a6543cd46be11267a823a087028a2d7e3c726f7e7eb231a818731e.
- Label eaf6c8b9450a7fa3f1f282717a44f9ae6f502a2c424d6185199cab317f08c757.
- Benchmark bench-20260926, digestf2ff84035aa968dcbf7da09e12ceb907caa4a4dd3cc74c694a95cb4ebe429648.
- Policy776db445df6323c5d0dfd7db8e080631d94665c6a84e548f4b1d01b148546d9a; universe pit_vendor_earnings_evidence_name_pattern_exclusion.

Use one frozen EvaluationSpec, selection formations2013-01..2023-12, all endpoints strictly before2024-01-01, holdout sealed. Provisional labels are next-session calendar-month entry to entry h months later. Start one feature per0.6GiB worker under0.2GiB nested orchestrator. Measure complete store/labels/accounting/cache-key/evaluation memory, then relay first pinned real cache for independent old-gate byte parity and4.1 Step2. EVAL remains frozen. Preserve complete-book NULLs and held/priced counts; no survivor renormalization. Calendar-month factors remain window proxies unless endpoints are proved equal. Finish432 cells, report statuses<=selection_pass, exclusions, limitations and the coverage deficit; independent review follows.

### Lane B — F.1 shared item engine, then F.2

Read task-F.1-report.md Stopped(session6), accepted design672e9920, F.1/F.2 briefs and C104. Adopt only untracked fundamental_sources.py and items_map.py. Other F.1 engine/CLI/identity/test/doc paths are absent.

Source audit361 files/0.0944GiB is accepted and unchanged. New candidate/index/Arrow scaffolding and seed mapping are unexecuted and unvalidated. Do not confuse them with a working item engine. Unit override proposal for RECT1106/AP1203/OANCF1301 exists in draft code but **C105 HAS NOT BEEN ISSUED**. Controller must adjudicate narrow authority or make it fail closed before any map output; no STD seed changes have been authorized or applied. Do not run that proposal as if approved.

Complete canonical dependency mapping, explicit pinned plan/build/audit CLI, bounded real-source normalization, exact corroboration and fiscal/vintage engine, immutable publication/reader, then accepted identity projection and audits. FSDS uses accepted_utc; CF-only max(raw_available_at,filed+46h); borrow acceptance only for exact same fact/context/value. Preserve newest-NULL, YTD/Q4/TTM/nonadditive-share and semantic-conflict rules, full-class ME and all original thresholds including>=3,000 owners. No warehouse or label reads in F.1.

Use verified FSDS v2 sibling and CF-R pins from handoff. C104 defines w2_fund_fast:23 canonical hypotheses/default276 cells, exact formulas/input/dedup pins and committed anchor before returns. Later2.10/w0 exclude the same hypotheses; no duplicate qualification vote. Low coverage stays counted; unsupported deferrals need evidence and pre-return ruling. One final holdout opening.

### Lane C — 3.3 identity / MIG

Read task-3.3-report.md Progress/Stopped(session6), brief/CARRY and F.1 identity contract. Adopt the12 owned paths, including new bodies_0331.py, registry.py and test_migrations.py. Preserve other lanes. Existing0329 checksum proof passed; do not repin it.

Small PIT/checksum probe passed at0.1116GiB: complete-class denominator, high-only/low/unknown/missing prices, cross-CIK quarantine and future-name behavior. Real symbol-island scan, read-only attached builder/audit and actual0331 remain unrun. Source-helper imports outside the migration checksum and reserved0330 ordering need review. Existing test conftest/direct connects may set1GB: arrange narrowly owned bounded fixture/runner adjustment before tests.

Reuse data/research/identity_inputs/session5 and identity_rehearsal/session5/preparation; do not rebuild accepted preparation. Never use session5-draft1(scope_complete=false) as accepted F.1 identity. Run corrected rehearsal under a NEW directory, then real migration/schema/API proof and full delisted/multiclass/monthly/no-reuse/clock/evidence/repeatability gates. Unknown/potential classes stay in denominator; no current descriptors/future evidence backfill. Ledger stage/kill-resume waits for accepted1.3; standalone rehearsal stays independent. Publish accepted F.1 evidence only after its gates pass.

## Controller operations and following DAG

X.5 is authorized files-only but stopped. Exportfa2d500b exists but was not launched; use fresh current-HEAD export. After disk/process/limiter verification, remove only documented operator STOP2026-09-27T01:35:41.3448686Z; never clear a SEC trip implicitly. Existing hidden loop uses explicit exported Worker,0.5GiB cap,35GiB disk floor and bounded retries. C85 implementation/relaunch review and0.3922GiB end-state memory measurement are accepted. Last progress17,000/426,151 is a lower bound; inspect durable ledger. Verify resumed rate/progress. --recheck-done and selector/gzip-aware parse/load remain pending; do not repeat accepted probe. Status script's25GB warning is obsolete; live loop enforces35GB.

As capacity frees, finish1.3's preserved five-file WIP, allowed kill/resume proof and one OPS fresh-copy drill/review. Then1.5/1.6/1.8 and the warehouse ladder. Finish2.2 scoped fix/review and X.1/X.2 source fetch contracts. Follow all DAG dependencies through F.2, final labels/external validation,4.3 QUALIFIED_SIGNALS,4.4 composites,4.5 ownership,4.6 export and S5 release. C93 uniqueness still blocks3.6 until relevant publishers are fixed. Do not stop at the first grade or substitute scope acceptance for the full goal.

Completed nodes: M0,0.1–0.15,1.1,1.2,1.7,1.9,1.10,1.11,X.3. Session5 also accepted X.4 corrected staging(C76),1.4 retained extraction,1.12 corrected inputs(C78),4.1 code/stub scope,F.1 design,X.5 C85 scope. Reuse accepted evidence; preserve later gates and coverage deficits.

## Standing rules

- Every Python/pytest/DuckDB process runs under .superpowers/sdd/tier1-parity/run_memory_guarded.py with C:/atx/atx-db/.venv/Scripts/python.exe and OPENBLAS_NUM_THREADS=1. Probes/tests/research0.6GiB/DuckDB256MB/threads1; writer0.8; set-based1.0max; orchestrator0.2 with --allow-nested-guards. Sum caps<=2GiB. pytest -n0. No real-run guard overrides;137/78 are resumable. PowerShell for receipt reads.
- OPS alone holds HEAVY, one production slice at a time from fresh committed export. Keep>=35GiB free and at most one full DB copy. Preserve prod and protected backup. No TDD; tests only for PIT edges, real-shaped migrations, kill/resume and one pipeline e2e. At most two fix rounds then controller ruling.
- Follow R1–R14/C1–C104 and scoped ownership. Add real rulings only when decided; pending C105 is not authority. Timestamp ledger entries using actual date -u +%H:%MZ, immediately before final historical HOLD sentinel. Author edits with apply_patch. Refresh report after every milestone.
- User requested subagents; use available inherited models and runtime slots under C96/C98. Required trailer is project metadata. Controller does not implement source; reviewers must be independent. Graph tools preferred when available, targeted rg/read fallback when unavailable.
- SEC UA exactly atx-db/0.1 atx-research@example.com, shared<=5req/s. U2/U3/U5/U6/U8/U9 granted files-only; U1/U4/U7/U10 not executed. Snapshot2026-09-20, cutoff22UTC. Never relabel reconstructed/inferred as verified vintage, weaken thresholds, or silently narrow denominators.
- Never push/merge/stash/reset/restore/clean/discard, or touch atx-engine/,atx-vol/,root python/,C:/atx-wt/,protected alpha-engine plans,other sessions' processes. Commits use explicit owned paths, [v2 NODE], and Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>.
- Do not bypass the historical automatic cleanup denial for data/tmp/session5-cfr-determinism. Its retained small scratch is documented and does not block research.

Keep the user informed of findings and completion boundaries. Persist toward the full goal once resumed. If the user explicitly stops again, stop owned jobs, pause the goal and write an updated handoff without silently continuing implementation.
