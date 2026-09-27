# Tier-1 v2 — session 6 stop handoff

## Read this first

The latest user instruction is **“stop here and write a detailed handoff markdown file for the next parent agent + a goal prompt.”** The goal is **paused**, not complete. All three workers acknowledged STOP, preserved their edits, wrote stopped checkpoints, and ended their turns. Do not restart implementation or fetching until the user resumes. Submitting the companion next-goal prompt as a new goal is an explicit resume instruction.

**Code HEAD at stop: fa2d500bda4fed6399ad2e22ac36901aa07fd35c.** This is the session-5 handoff commit; no session-6 source commit was made. The commit containing these new handoff documents may be later. There are **30 preserved atx-db WIP paths**. Do not discard or bulk-commit them.

**Critical change since the session-5 handoff: w1_price IS registered.** Registration completed immediately before the price worker received STOP. Exactly 432 cells were registered at 2026-09-27T12:18:48. The anchor is modified but **not committed**. No real label-value read, EvaluationSpec preparation, evaluation job, real cache, or grade followed. Holdout remains sealed. The next parent must preserve and reconcile this registration, then commit the anchor and reviewed runner before any real return read. **Do not register the wave again.**

The actual registry is **atx-db/data/research/registry/trial_registry.jsonl**. Earlier documents checked the shorter data/research/trial_registry.jsonl path; that is not the actual default. This handoff and current task-1.13 report supersede the older “nothing registered” statements.

Companion resume prompt: [2026-09-27-tier1-v2-next-goal-prompt-5.md](2026-09-27-tier1-v2-next-goal-prompt-5.md). The [session-5 handoff](2026-09-27-tier1-v2-session-5-handoff.md) retains detailed accepted-source measurements, commits, rulings and provenance. Read this document first, then that one for unchanged evidence. Never let an older stopped/progress section override a newer checkpoint.

## Objective and authoritative files

Build production-quality, large cross-sectional, deep-history characteristics with forward 1–12 month evidence for an equity long/short book, including the tools, infrastructure and pipelines. Completion requires the DAG index §2 exit criteria and §5.7 candidate release. The intermediate minimum remains 1.13 price grading, a fundamentals wave through F.2 or 2.10, 4.3 QUALIFIED_SIGNALS, and 4.6 alpha export. **None of those four deliverables is complete.** Registration is not grading, and source acceptance is not release acceptance.

Read these on resume:

1. Original objective: C:/Users/natha/.codex/attachments/dafee98b-11ae-4f69-a4d6-4d69f2a02d3b/goal-objective.md.
2. docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md, especially §§2, 3, 6 and 9, and adjacent S0–S5/SX task files.
3. .superpowers/sdd/tier1-v2/CONTEXT.md, REVIEW-CONTEXT.md, CARRY.md, and progress.md. Ledger plus git log is the source of truth; current controller session is 6.
4. Current stopped sections of task-1.13-report.md, task-F.1-report.md and task-3.3-report.md. Reports, probes and receipts under .superpowers are local workspace state; these committed handoffs preserve their essential facts.
5. Accepted task-1.12-report.md Relay to 1.13 and task-4.1-report.md Relay, plus their session-5 re-reviews. Do not repeat unchanged accepted reviews.

## What happened in session 6

The ledger records a brief resume after fa2d500b. The controller reconciled the then-25-path WIP, verified protected files and stopped processes, changed CONTEXT's session number to 6, and dispatched three disjoint workers. No goal completion was claimed. The latest STOP now supersedes that resume.

| Lane | Progress before STOP | Current boundary |
|---|---|---|
| 1.13 / price_wave | Added prepared-code/read-boundary checks; synthetic accounting/PIT probe passed; printed plan and registered 432 cells. | Anchor uncommitted; no real evaluation. |
| F.1 / fundamentals | Drafted shared seed mapping and bounded candidate-input scaffolding. | Unexecuted; unit authority unresolved; no item build. |
| 3.3 / identity | Added observed-symbol islands, read-only preparation attachment, audit draft and migration0331 integration; small PIT/checksum probe passed. | Corrected real rehearsal and actual migration unrun; no accepted identity artifact. |
| Controller / OPS | Reclaimed dead guard state and prepared an X.5 current-HEAD export. | Fetch was never relaunched; old operator STOP remains. |

Session-6 reclaim receipt: .superpowers/sdd/tier1-v2/receipts/session6-reclaim.json, exit 0, cap 0.2 GiB, native peak 0.0283 GiB, cap_hit=false. Its admission reclaimed the dead session-5 fetch slot for PID16196; the inner reclaim found no additional stale slot. The receipt is authoritative if an earlier summary says nothing was reclaimed.

## 1.13 registration and exact resume boundary

Report: .superpowers/sdd/tier1-v2/task-1.13-report.md, Progress (session 6), STOPPED subsection.

| Item | Exact state |
|---|---|
| Registry path | atx-db/data/research/registry/trial_registry.jsonl |
| Registry version / event | trial-registry-v2 / register_wave |
| Sequence / timestamp | 0 / 2026-09-27T12:18:48 |
| Record SHA / registration ID | 3afdfc36b2d257244375fa784d7dfe062ab6a00d31280f85cb26dfa546b3920c |
| Preregistration SHA256 | 19f42d0a4629c386170c812f4991caa01815fc077e75f4cf3ebfcd6cf8879310 |
| Registry file bytes / SHA256 | 21,662 / c42026c16f3c5087126a24c1178a3e4bd74f64b3b51f37c85168e6d501ddda63 |
| Anchor path | atx-db/src/atx_db/seeds/research_trial_registry_anchor.jsonl |
| Working anchor bytes / SHA256 | 137 / 26db1b7d59ab184c762795d4c5caae271b833123dae6a674d2651580aadd7cc8 |
| Committed anchor at fa2d500b | Zero bytes |
| Runner | atx-db/scripts/research_run_wave.py, untracked, 1,464 lines |
| Runner working-file SHA256 | a95d8039fae846a993d5d659f32d6dd6acde3264ccc4215093e1a2c9792bb0f1 |

The working anchor contains one register_wave record with the exact registration ID, sequence0 and wave w1_price above. Reconcile it through the registry API under the guard after resume; do not manufacture, reset or substitute a scratch registration. The file SHA and record SHA are different concepts.

The grid is 36 features × three served transforms (rank_normal, signed_raw, zscore) × horizons 1/3/6/12 = **432 cells**, 144 per transform. There are 33 gating IDs and three reported-only IDs: beta_ew_252d, ivol_ew_21d and ivol_ew_252d. zero_trade_21d and zero_trade_252d remain source-unsupported and unregistered under C-82. Beta-return opt-in is absent under C-103. No policy or trial denominator was relaxed.

New checks and receipts, all under .superpowers/sdd/tier1-v2/:

- probes/1.13-s6-adapter-pit.py; receipts/1.13-s6-adapter-pit-r2.json|out|err. Fourteen checks passed: 202 exact accounting rows, eight deliberately wrong endpoints excluded across four horizons, sealed endpoints excluded, invalid status retained, source/pin drift and unregistered real reader refused. Exit0, native peak0.1288 GiB, no cap hit. The first attempt failed before work because a scratch parent was absent (peak0.0932); the retry used existing data/tmp and cleaned its own TemporaryDirectory.
- receipts/1.13-s6-plan.json|out|err: exact 432-cell plan, exit0, peak0.0965 GiB.
- receipts/1.13-s6-register.json|out|err: production registration completed, exit0, peak0.1324 GiB. Guard22520/child15408 completed; no live process remains.
- Prepared runner/EVAL/feature-adapter drift checks and explicit closed-selection metadata were added. Source bytes are reverified before separate endpoint projection. These are preparation evidence, not an independent full runner review or a real-memory result.

After explicit resume:

1. Inspect/adopt the entire untracked runner and anchor diff; reconcile the existing registry against these pins and accepted catalog. Catalog already landed at 47e168cd. Do not redo registration.
2. Commit only the reviewed runner and existing anchor with an explicit pathspec, [v2 1.13] and the required trailer. The anchor commit must precede every real label-value read/join. Do not commit unrelated WIP.
3. Prepare one frozen EvaluationSpec. Grade formations 2013-01..2023-12 with every mature endpoint strictly before 2024-01-01; holdout sealed. Provisional labels use next-session calendar-month entry to entry h calendar months later, not fixed 21/63/126/252-session windows.
4. Start one feature per 0.6 GiB worker with a 0.2 GiB nested-guard orchestrator. Measure the full labels/store/accounting/cache-key/evaluator path. Group size1 is the current default; do not infer real memory from synthetic probes.
5. Relay the first pinned real feature cache for independent old-gate byte parity and 4.1 Step2 evidence. EVAL stays frozen at41168d6f throughout the real run.
6. Finish all 432 selection cells and atx-db/docs/research/WAVE1_PRICE_PROVISIONAL.md, with full statuses, exclusions, trial counts, coverage and uncertainty. Maximum status is selection_pass. Then independent task review.

Do not survivor-renormalize complete held-book economics. Missing/terminal-pending returns may make gross/EA/net values NULL; preserve held/priced counts. Calendar-month external factors remain labeled window proxies until exact endpoint alignment is proved. Preserve policy, source/code/seal pins and all limitations.

## Accepted price pins and unchanged limitations

Artifact root is C:/atx/atx-db/data/research. These inputs were independently approved in session5 and were not rebuilt in session6.

| Pin | Value |
|---|---|
| Catalog | 36bb1bbe472b1bed0371a1f4bb1cb5ebcdc17132669d0bf341e5f5ffaad82654 |
| Feature manifest ID | 58aeacf3967548de9e7e8cc7c30ce830bca5b7df8a1a3f8eb1a12e537edab4de |
| Manifest relative path | manifests/w1_price/58aeacf3967548de9e7e8cc7c30ce830bca5b7df8a1a3f8eb1a12e537edab4de.json |
| Manifest file SHA256 | 8c234060422e1b209bd1d3e6a7e32b1b7b5692d211ec578d2dce87ec8f62cb05 |
| Lake | price-wave-0ed96b2696f1-bcdf5d0f3437 |
| Lake digest | 9eeb25fe81a6543cd46be11267a823a087028a2d7e3c726f7e7eb231a818731e |
| Labels | eaf6c8b9450a7fa3f1f282717a44f9ae6f502a2c424d6185199cab317f08c757 |
| Benchmark | bench-20260926 |
| Benchmark digest | f2ff84035aa968dcbf7da09e12ceb907caa4a4dd3cc74c694a95cb4ebe429648 |
| Policy semantic SHA | 776db445df6323c5d0dfd7db8e080631d94665c6a84e548f4b1d01b148546d9a |
| Universe basis | pit_vendor_earnings_evidence_name_pattern_exclusion |
| Working build | work/price_wave/th3-20260920 |

1.12 acceptance had zero correctness violations and matched all38 feature SHAs. Bars32,050,111; spine631,442; 161,332 excluded rows split88,305 absent earnings evidence and73,027 future evidence. All2,042,924 label rows remain sealed; valid1,869,328, invalid909, terminal_pending64,582, not_matured105,065, missing entry1,211 and missing exit1,829. The session5 handoff and Relay preserve all artifact/clock counts and memory evidence.

**Every required 4,500-line coverage target is NOT MET; maximum universe is4,147.** No waiver exists. Suspect-price exclusions are not verified repairs; reconstructed classifications/vendor shares/provisional terminal handling remain limitations. The small month-description Minor belongs to the next input refresh, not an unnecessary rebuild.

4.1 code/stub scope is independently accepted at41168d6f. Its corrected accounting requires separately pinned old-entry-to-current-entry returns, all held names priced for finite fixed-book economics, and separate hysteresis state for each horizon sleeve. Original143 columns/gates matched baseline synthetically. Real Step2 remains missing; neither 1.13 preparation nor registration completes it.

## F.1/F.2 fundamentals checkpoint

Read task-F.1-report.md, Stopped (session6), accepted design docs/superpowers/plans/2026-09-27-tier1-v2-fundamentals-fast-path.md at672e9920, task-F.1-brief.md and task-F.2-brief.md. Design review passed; implementation is incomplete.

Exactly two untracked source files are owned:

- src/atx_db/research/fundamental_sources.py: accepted source-verifier functions plus new, **unexecuted** candidate scaffolding. New functions include atomic_json, parquet_relation, check_file, prepare_filing_index, _candidate_query, iter_candidates, iter_fsds_candidates and iter_cf_candidates. Draft SQL normalizes FSDS NUM/SUB/TAG and CF-R contexts, clocks, exact/raw values, source status and hashes, with deterministic candidate IDs and CIK-mod-128 buckets. No global filing index or normalized output was built.
- src/atx_db/research/items_map.py: new, **unexecuted** ItemChain/COMPUSTAT_ANALOG/load_mapping/live_alias draft using seeded canonical items, dependency closure, source multipliers, alias dates and seed hashes. TXDITC/BE/DLCCH/TXPD are marked without seeded canonical definitions; OIADP discloses a withheld unseeded ebit_best fallback. These gaps need reconciliation against the full shared2.9 contract. Metadata fields do not yet implement fiscal/value rules.

**Pending controller decision: C-105 has NOT been issued.** RECT/item1106 and AP/item1203 are quantity in the item seed; OANCF/item1301 is ratio. Statement-map monetary contexts and approved definitions suggest monetary. The draft UNIT_DISCREPANCIES proposes those three overrides and would apply them if called. No seed mutation, generated map digest or data normalization occurred. The next parent must explicitly adjudicate and pin authority or require the code to fail closed before measuring/publishing it. Do not treat draft code as authorization, broaden to other unit discrepancies, or force non-USD/mismatched source units into USD. Route seed correction separately to its STD owner if chosen.

No F.1 Python/DuckDB/test/build process ran in session6. No item_vintages.py, fundamental_identity.py, research_fundamentals.py CLI, FUNDAMENTALS_FAST_PATH.md implementation doc or F.1 tests exist. No forward-return reader or warehouse was opened. All new functions need syntax/schema/semantics/memory measurements; the old source audit does not validate them.

Next implementation order: resolve unit authority; complete canonical map/dependency metadata; add explicit pinned plan/build/audit CLI; measure a bounded real-source slice; then build exact CF/FSDS corroboration, conflict/disposition handling, fiscal slots, duration/YTD/Q4/TTM, restatement propagation, latest-NULL semantics, immutable publication/resume, reader and accepted identity projection. Check staged SQL field names, taxonomy/version formats, null TAG metadata, allowed forms, conflicting SUB contexts, post-snapshot clocks, exact numeric canonicalization and bounded sorting. Keep slices <=2M output and approximately60 seconds where required.

Required clocks and gates remain: FSDS accepted_utc; CF-only max(raw_available_at,filed+46h); acceptance borrowing only for exact same fact/context/value, not accession alone; nonadditive weighted shares; complete TTM; semantic conflict becomes NULL before provenance tie-break (C104); no current/future identity backfill; complete all-class company ME. Preserve the numeric mapping/tie-out, fiscal/PIT,50-owner oracle, real resume and >=3,000-owner coverage criteria. Item work can precede accepted identity; projection cannot.

C104 reserves w2_fund_fast for23 canonical hypotheses, default276 cells. F.2 must pin formulas/dedup/inputs and commit its own registration anchor before returns. Later2.10/w2_fund_a and w0 exclude these hypotheses; changed constructs/clocks/identity are counted configurations. Low-coverage buildable rows remain counted; source-unsupported deferral needs pre-return evidence and controller ruling. Only one final holdout opening.

## Accounting sources already accepted

Use explicit FSDS v2 sibling at atx-db/data/staging/fsds-v2/fsds-staging-manifest.json, SHA2cad6134efd312d7ad9ca84bbc274fdf360e10e581b38aa88a29f1040bebc628. Canonical data/staging/fsds remains v1. All69 quarters/276 files verified: SUB433,717, NUM184,959,880, TAG4,876,740, PRE45,780,878. Corrected UTC clocks all passed;52,567 entry sessions move later versus v1. Bounded orphan audit fixb095ef20 is accepted; four accessions/1,953 orphan facts explicitly remain. X.4 benchmark Step3 belongs to2.8 under C76.

Use CF-R root atx-db/data/staging/companyfacts/ee099c7394a357f1/. Manifest SHA is 50e018e1c26046f3eb3ec27d2f246c8492e60baddbc911ebfda50320f24ac186; members SHA is 07943ae309b8a5d10c2fd3caf6fb7110e0830d6e90676d988515cf4465995c0c. There are 85 batches/20,390 members, 16,995 loaded/3,333 empty/62 unavailable/0 errors and 53,751,273 facts. The 10,832-member prefix reproduces 9,462/1,327/43 under C102. Exact literal 31354367947000000000 survives DOUBLE rounding. Samples, 67-batch regression and four deterministic re-extractions passed; live warehouse parity still belongs to 1.5/1.8.

F.1's unchanged source audit verified all361 consumed files, peak0.0944 GiB. Source pins/receipts are receipts/F.1-source-{pins,verify}*. Do not repeat successful unchanged audits or re-stage accepted sources solely because a new parent took over. The session5 handoff contains full archived-source provenance.

## 3.3 identity / MIG checkpoint

Read task-3.3-report.md Progress/Stopped(session6). It owns12 source WIP paths, mapped below. No source commit or production access occurred in session6. There is no accepted F.1 identity artifact.

New session6 code:

- _dated_vendor_symbols in identity_links.py scans dated ticker_tk, rejects ambiguous days, and creates symbol/observed-XNYS-session islands in64 slices. This replaces unsafe min/max coverage, but the real scan is unrun.
- build_rehearsal now creates an output identity.duckdb and attaches retained.duckdb READ_ONLY via temporary views. This protects retained preparation; the new real adapter is unrun.
- Public snapshots quarantine cross-CIK membership before tier filtering. Current SEC ticker lines missing from represented classes add observation-clock unresolved membership. audit_identity drafts key/allocation/evidence checks and132 monthly coverage rows. Real audit/build remains unrun.
- New migrations/bodies_0331.py creates identity tables/public view, table/field metadata and schema-contract pin swap. Registry imports version331, permanent_identity_history; comment reserves0330 for2.6. tests/test_migrations.py expects331 and upgrade327/328/329/331. No actual migration executed. Runtime helper imports from identity_links are outside the migration-body checksum; assess immutability/schema-contract requirements before acceptance.

One guarded probe passed before STOP: probes/3.3-session6-checks.py, receipts/3.3/s6-pit-checks.json|out|err; exit0/3.0seconds/native peak0.1116 GiB/cap_hitfalse, DuckDB256MB/threads1. It proved common-class ME100+200=300, high-only denominator2/eligible1→NULL, duplicate prices→NULL, low/unknown sibling→NULL, future-name exclusion, cross-CIK quarantine in public and link APIs, and ambiguous ME→NULL. View creation parsed. Old0327/0328/0329 callable checksums stayed unchanged;0329 remains0dfc9b494d4b99942b71c6ac59e9a8a4aec664c588718e7a323af730d836be84. This is a small PIT/checksum proof, not real identity or migration acceptance. Guard16348/child17484 completed.

Reusable data is unchanged:

- data/research/identity_inputs/session5/manifest.json: five OPS Parquets (~1.84MB), identifiers8,031, securities52,133, listings45,820, SECtickers10,438, Nasdaq13,258. Production evidence table was absent.
- data/research/identity_rehearsal/session5/preparation/preparation.json, retained.duckdb and104 evidence parts:25,760 lines,1,000,917 share facts,9,413 CIK candidates,28,799 lifecycle filings,12,650 evidence rows. Preparation code7eaafe75; peak0.1840/0.2016/0.2951 GiB. Preserve and reuse.
- session5-draft1/manifest.json remains exploratory, scope_complete=false, with known pre-correction defects. Its88.1596% operating-proxy coverage is not acceptance and must not qualify F.1 input. No session6 real rehearsal output exists.

After resume: static review of islands/read-only attachment/audit UNNEST/0331 metadata; new retained rehearsal in a fresh output directory; real-shaped0331 migration and schema/API proof; all50 delisted cases,331 multi-line baseline,132 monthly formations,25,760 permanent IDs/no-reuse/uniqueness, complete-class ME and evidence/clock/repeatability gates. Keep unknown/lower-tier/unresolved siblings in denominators. No future class upgrade or current descriptor backfill. Suggested rehearsal tail is in the task report; always write a new directory, never overwrite draft1/preparation.

Existing tests/conftest.py has a1GB DuckDB test setting and some direct connects. Obtain narrow ownership for a bounded fixture/runner adjustment before running touched migration/historical tests; do not silently run at1GB. Use0.6 guard/256MB/threads1 and pytest -n0. Ledger stage/atomic publication/kill-resume remain dependent on accepted1.3; no batch_runner import/adapter was added here. Standalone rehearsal must remain independent. Clarify0330 ordering with controller before migration acceptance.

## X.5 operations — still stopped

No SEC fetch was started in session6. The controller created a fresh archive/extracted export at .superpowers/sdd/tier1-parity/exports/fa2d500b/ containing atx-db/src, scripts and pyproject.toml; source.tar (12,738,560 bytes) is also retained. Fetch entrypoint existence was checked. Full relevant-source comparison and initial operational measurement were not performed because launch never happened. Use a fresh current-HEAD export on eventual resume.

The operator STOP remains .superpowers/sdd/tier1-v2/receipts/x5/STOP with exact contents2026-09-27T01:35:41.3448686Z. Canonical data/cache/.sec_rate.state was read as blocks0/pause_until0; .sec_rate.blocked was absent. Nothing was cleared or rewritten.

The last actual run was session5 segment007: progress17,000/426,151 candidates,3,852 newly fetched indexes,3,618 documents,0 failures. This is a lower bound on durable completion; inspect the ledger/sidecar later. A stale guard receipt may still say running despite no live handle.

C85 implementation20734bce and session5 scoped review are accepted. Full-size synthetic end-state peak0.3922 GiB supports0.5 GiB cap with27.49% headroom. The reviewed loop enforces35 GiB disk floor, three fetch threads, <=3 exit1 relaunches/hour with120-second delay, exit3 stop/no trip clear;137/78 resumable. No need repeat unchanged memory probe. x5-status.ps1 still has an obsolete25GB warning; actual loop floor is35GB. Treat that text as a documentation follow-up, not live policy.

On explicit resume, verify processes/disk/limiter, fresh-export source, remove only the documented operator STOP, and launch the existing loop hidden with explicit exported Worker and JobGb0.5. Exact launch pattern is in task-X.5-ops-session5.md. Never clear a SEC trip implicitly. Verify new rate/progress receipt under shared <=5req/s limiter. Full fetch, explicit --recheck-done integrity pass, selector/gzip-aware parsing and ledgered load remain outstanding. No load is authorized as completed merely because fetching resumes.

## Preserved WIP ownership — 30 paths relative to atx-db

| Lane | Exact paths / ownership |
|---|---|
| 1.13 (2) | Untracked scripts/research_run_wave.py; modified src/atx_db/seeds/research_trial_registry_anchor.jsonl. |
| F.1 (2) | Untracked src/atx_db/research/fundamental_sources.py and items_map.py. |
| 3.3/MIG (12) | Modified scripts/ticker_history_unit_inventory.py; src/atx_db/identity_links.py, market_daily.py, market_owner_bridge.py, publication.py, schema.py; src/atx_db/api/catalog.py; src/atx_db/migrations/bodies_0329.py and registry.py; tests/test_historical_identity.py and test_migrations.py. New src/atx_db/migrations/bodies_0331.py. |
| 1.3 (5) | Untracked src/atx_db/batch_runner.py and bars_unit_correction.py; scripts/run_slices.py and recover_stale_run.py; tests/test_batch_runner_kill_resume.py. |
| 2.2 (4) | Modified src/atx_db/signal_eval.py; tests/test_asof_no_latest_revision.py, test_signal_eval.py and test_signal_eval_fast_panel.py. |
| X.1/X.2 (5) | Modified scripts/download_13f.py; src/atx_db/finra.py, short_volume.py and thirteenf_archive.py; untracked scripts/fetch_finra_short_data.py. |

The five-path increase from session5 is the anchor, items_map.py, bodies_0331.py, registry.py and test_migrations.py. Both fundamental_sources.py and identity_links.py also gained new uncommitted content. Other-session atx-engine/src/book/replay.cpp and protected docs/plans/alpha-engine files remain outside our ownership. The repository also contains pre-existing untracked plans/handoffs; preserve them and never add a whole directory blindly.

1.3 has no accepted interface or report yet; adopt WIP, finish kill/resume proof, commit, run one OPS fresh-copy drill, then independent review. The old dead full-copy scratch was deleted in session5 reconciliation. This lane unblocks1.5/1.6/1.8 and the warehouse ladder. 2.2 needs its scoped fix/review. X.1/X.2 fetches have not started. Prior session5 research recorded SR-FINRA-2026-012 as withdrawn on2026-08-05 at the official SEC rulemaking index; treat this as historical evidence and verify the actual public source contract before fetching, rather than implementing the withdrawn proposal.

## Accepted scopes, commits and remaining DAG

Already complete before session5: M0,0.1–0.15,1.1,1.2,1.7,1.9,1.10,1.11,X.3. Do not redispatch. Session5 accepted X.4 corrected staging under C76,1.4 retained extraction,1.12 corrected input implementation under C78,4.1 code/stub phase, F.1 design, and X.5 C85 operational scope. None erases later quantitative/real-data gates.

Session5 code commits:672e9920 F.1 design;50caac93 FEAT beta helper;dd83baec identity OPS exporter;b095ef20 bounded X.4 audit;7eaafe75 retained identity prep/XNYS;0f8f752e reported evaluation/seal API;c03e687e CF verifier;41168d6f accounting corrections;47e168cd corrected price inputs/catalog. fa2d500b is the prior handoff documentation. Reconcile git log rather than assuming an old expected HEAD. Session6 made no code commit.

After the priority1.13/F.1/3.3 lanes, dispatch1.3 as capacity frees, then1.5/1.6/1.8, fundamentals2.1→2.3–2.7/2.9–2.10, market/identity3.1–3.8 and events3.9, external validation4.2, full-family4.3, preregistered composites4.4, ownership4.5, alpha export4.6 and S5 release. Use DAG dependencies and CARRY. In particular, C93 publish-time uniqueness blocks3.6 until3.1/3.2/3.5 address it. Full goal remains open.

## Binding operating rules and final state

- User explicitly authorized parallel subagents in the objective. This runtime has four total concurrent threads including root: use three disjoint workers, then reuse for independent reviews. No self-review. Inherited available model replaces unavailable Opus under C96; required coauthor trailer is requested metadata, not a claim about the current model. Controller directs/rules/reviews/OPS and does not implement source.
- R1–R14 and C1–C104 remain binding. C94 requires Progress(session N) report refresh after each commit/run/stage. **No C105 unit ruling exists.** Use actual UTC date immediately before ledger edits; insert before the historical final11:50Z HOLD sentinel. Author file changes with apply_patch (C96).
- Every Python/pytest/DuckDB process, including probes, goes through .superpowers/sdd/tier1-parity/run_memory_guarded.py using C:/atx/atx-db/.venv/Scripts/python.exe and OPENBLAS_NUM_THREADS=1. Research/tests0.6 GiB; DuckDB256MB/threads1 at connect. Writers0.8, set-based1.0 max, orchestrator0.2 with --allow-nested-guards. Sum caps<=2GiB. No real-run guard overrides; pytest -n0. Read receipts with PowerShell.
- Only OPS holds HEAVY; fresh committed export, one production slice at a time. Preserve >=35GiB free, at most one full DB copy. No unguarded warehouse opens. No TDD; permitted tests are PIT edges, real-shaped migrations, kill/resume and one pipeline e2e. Reuse unchanged passing evidence.
- Source grants U2/U3/U5/U6/U8/U9 are files-only. U1/U4/U7/U10 remain unexecuted. SEC UA exactly atx-db/0.1 atx-research@example.com; shared <=5req/s limiter. Snapshot2026-09-20, decision cutoff22:00UTC. Never upgrade reconstruction to verified vintage, weaken thresholds, or silently narrow populations.
- Prefer graph MCP for code discovery; none was available in this runtime, documented fallback targeted rg/read. Recheck availability once in a new runtime rather than assuming tools exist.
- No push/merge/stash/reset/restore/clean/discard. Never touch atx-engine/, atx-vol/, root python/, C:/atx-wt/, protected alpha-engine plans or other sessions' processes. Commit explicit owned paths, subject [v2 NODE], trailer Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>.

At final stop inspection, the active guard slots directory contained only .lock/.seq, and all owned worker receipts were completed. Broad Python matches included VS Code mypy/black language services, which were identified and left untouched. No owned pipeline, fetch or guard is running; HEAVY is free. Recheck process ownership on resume; old PIDs are evidence only and may be reused.

Protected production remains12,873,379,840 bytes, mtime2026-09-25T11:59:36.2668647Z. Backup warehouse.duckdb.pre-migrate.20260924-225023.bak remains12,883,341,312 bytes, mtime2026-09-24T22:50:23.5699261Z. Last free disk101.27GiB; recheck before writes. No session6 warehouse read/write occurred.

Root-owned data/tmp/session5-cfr-determinism remains15 files/14,868,836 bytes. Earlier automatic approval review rejected its cleanup with “blocked by policy”; no bypass or retry occurred here. Keep that historical limitation recorded. The fa2d500b export/archive also remains, and retained identity scratch is derived data, not a full production clone.

The handoff task is complete when these documents are saved and linked. The research goal must remain paused until the user resumes; do not mark it complete.
