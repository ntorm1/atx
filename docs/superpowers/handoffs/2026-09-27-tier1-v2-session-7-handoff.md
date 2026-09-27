# Tier-1 v2 — session 7 stop handoff

## Current instruction and state

The user requested: **“stop here and write a detailed handoff markdown file for the next parent agent + a goal prompt.”** The goal has been set to **paused**, not complete. All three workers acknowledged STOP, saved final checkpoints and ended their turns. Implementation, evaluations, tests and SEC fetching have stopped. Resume only when the user explicitly continues; submitting the companion goal prompt is such an instruction.

**Source HEAD at stop: `251995c72495a795ef5b8745d8ab20e1c248e407`.** A later commit may contain these handoff documents. There are **32 preserved atx-db WIP paths**. Do not discard or bulk-commit them. Final process checks found zero owned pipeline/fetch/pytest/guard processes and zero guard slot JSON files. Other-session and IDE processes were untouched. Production database and backup sizes/mtimes remain unchanged.

The main new result is **12 of 36 price features evaluated, 144 of 432 cells, with no grade yet**. Registration and its anchor are now committed. **Do not register again.** The runner has a confirmed cache-hit resume bug: resume only the remaining 24 features with the frozen source/spec, retaining the completed receipts. The independent real-data comparison of the evaluation engine passed; that does not approve the unfinished runner or complete the goal.

Companion: [next goal prompt 6](2026-09-27-tier1-v2-next-goal-prompt-6.md). Older detail: [session 6](2026-09-27-tier1-v2-session-6-handoff.md), [session 5](2026-09-27-tier1-v2-session-5-handoff.md). This document and current stopped reports supersede older “uncommitted anchor,” “no real reads,” “C105 pending,” and “migration never run” descriptions.

## Goal, source of truth and read order

Build production-quality, large cross-sectional, deep-history characteristics with forward 1–12 month evidence for an equity long/short book, including the tools, infrastructure and pipelines. Completion requires the DAG index §2 exit criteria and §5.7 candidate release. Intermediate minimum: 1.13 price grading, F.2 or 2.10 fundamental grading, 4.3 QUALIFIED_SIGNALS, and 4.6 alpha export. **None of those four is complete.** No thresholds were waived; source acceptance, a partial wave and a parity measurement are not release acceptance.

Read on resume:

1. `C:/Users/natha/.codex/attachments/dafee98b-11ae-4f69-a4d6-4d69f2a02d3b/goal-objective.md` (read again by this parent before stopping).
2. `docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md` §§2, 3, 6, 9 and relevant adjacent S0–S5/SX files.
3. `.superpowers/sdd/tier1-v2/{CONTEXT,REVIEW-CONTEXT,CARRY,progress}.md`. Ledger plus git log is authoritative. Current report session is 7; next parent should advance to 8 only on explicit resume.
4. Current stopped sections of `task-1.13-report.md`, `task-F.1-report.md`, `task-3.3-report.md`, `task-X.5-ops-session7.md`; review `task-1.13-controller-review-session7.md` and measurement `task-4.1-real-session7.md`.
5. Accepted input Relays in `task-1.12-report.md`, `task-4.1-report.md`, their session-5 re-reviews, and the accepted F.1 design at commit672e9920.

Local `.superpowers` reports/receipts and generated data are retained workspace state, not necessarily committed. These handoff documents preserve the essential decisions and boundaries for a replacement parent. Read newer sections first; older sections deliberately retain historical facts.

## Session 7 changes at a glance

| Lane | Completed measurements/work | Still open |
|---|---|---|
| 1.13 price wave | Reconciled existing registration; committed runner+anchor251995c7 before returns; prepared frozen spec; 12 features/144 cells cached, max completed worker0.4553GiB. | Remaining24 features, whole-wave grade/report, I1 cache resume fix and independent review. |
| 4.1 evaluation | Independent real ret_12_1 comparison to8aba655c: original143 columns, all old rows and family exact; current output equals pinned cache. | Parent reconcile scope/record acceptance as appropriate; no full-node completion line added here. |
| F.1 fundamentals | Pinned seed map and433,717-row filing index; three normalized source slices; diagnostic plan. Four source files authored. | Item engine entirely unmeasured, source subdivision, publication/audit, identity projection and every final acceptance gate. |
| 3.3 identity/MIG | Frozen0331 definitions and catalog swaps; real migration exposed populated-history binder bug, then code changed; rehearsal memory engineering. | Corrected real rehearsal and full acceptance, remaining test outcomes/review, independent delisted oracle, ledger adapter. No accepted identity manifest. |
| X.5 OPS | Fresh208c3d11 export; segment008 fetched additional files;19,611 durable units-done records at stop. | Full426,151-candidate fetch, integrity pass, parse/load. Loop stopped with operator STOP. |

Only source commit this session: **251995c7**, `feat(research): anchor price-wave registration and runner [v2 1.13]`, exactly `scripts/research_run_wave.py` and `src/atx_db/seeds/research_trial_registry_anchor.jsonl`, with required trailer. No F.1 or 3.3 source commit was made.

## Binding operating rules

- Parent dispatches, reviews, adjudicates and runs authorized OPS; source implementation belongs to disjoint worker lanes. User authorized subagents in the objective. Runtime has four slots total (parent plus three workers), inherited available model under C96; do not claim agents are Opus. Last workers were `/root/price_wave`, `/root/fundamentals`, `/root/identity`. All were told STOP; do not restart them while paused.
- Every Python/pytest/DuckDB process, including inline probes, must run under `.superpowers/sdd/tier1-parity/run_memory_guarded.py`; interpreter `C:/atx/atx-db/.venv/Scripts/python.exe`, `OPENBLAS_NUM_THREADS=1`. Probes/tests/research workers0.6GiB, writer0.8, set-based1.0 maximum; orchestrator0.2 with `--allow-nested-guards`; aggregate caps≤2GiB. DuckDB normally256MB/threads1 at connect. Pytest `-n 0`. No real-run test overrides. Exit137/78 is resumable; do not raise caps or narrow populations to solve it.
- No TDD. Acceptance is real measured evidence. Tests only PIT edges, real-shaped migrations, kill/resume and one pipeline end-to-end check; do not rerun accepted unchanged suites.
- Keep ≥35GiB free and at most one full warehouse copy. Only OPS/HEAVY may access production, one slice at a time from a fresh committed export. No production migration was authorized for the current identity rehearsal.
- Authored changes use `apply_patch` (C96). Explicit commit pathspecs and `[v2 NODE]`; trailer exactly `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>` as project metadata. Never push/merge/stash/reset/restore/clean/discard. Never touch `atx-engine/`, `atx-vol/`, root `python/`, `C:/atx-wt`, protected alpha-engine plans or other-session processes.
- Ledger lines go immediately before its final historical `- 11:50Z HOLD 0.12 fix round until rev-0-4 returns ...` sentinel. Immediately before editing, obtain actual UTC using `& 'C:/Program Files/Git/bin/bash.exe' -lc 'date -u +%H:%MZ'`. Do not estimate timestamps. Every agent refreshes `## Progress (session N)` after each milestone (C94).
- Snapshot2026-09-20; decision cutoff22:00UTC. Latest revision wins even if NULL. Never inferred/reconstructed→verified vintage; no weaker thresholds or hidden denominator narrowing. DuckDB1.5.5 DEFAULT-now WAL risk: create/copy/swap+CHECKPOINT rather than in-place mutation in relevant batch/migration paths.
- U2/U3/U5/U6/U8/U9 granted files-only; U1/U4/U7/U10 unexecuted. SEC UA exactly `atx-db/0.1 atx-research@example.com`, canonical host-wide≤5req/s limiter; no trip auto-clear. Research projection does not expand external-access grants.
- Graph MCP is unavailable in this runtime (discovery returned no graph tools); C96 allows targeted `rg`/file reads. Use `--no-ignore` for ignored artifact discovery when necessary. Do not use app/resource discovery to pretend graph access exists.

## New rulings C105–C107

Full wording is in ledger/CARRY. These are narrow authorities, not acceptance waivers.

- **C105:** Only RECT/item1106, AP/item1203 and OANCF/item1301 may use monetary authority from existing monetary statement-map aliases and approved definitions despite seed labels quantity/quantity/ratio. Preserve original seed hashes/labels and discrepancy/authority records in the mapping digest. Validate actual source UOM; no mismatched/non-USD relabeling. STD seeds untouched;2.5 owns durable correction.
- **C106:** Keep0331 for identity and reserve0330 for2.6. New0331 table/view definitions must be migration-local and checksum-covered; catalog updates preserve DDL, keys and unrelated rows by copy/swap. After0331 is accepted/applied,3.4part2 must add a later migration.2.6 must prove both330/331 orders on real-shaped data. Runner uses applied-version set, so a later-added330 still executes after331. Narrow tests/conftest.py256MB/threads1 adjustment granted. No production migration grant or old-checksum blind repin.
- **C107:** Local content-pinned metadata may implement already-approved2.5 TXPD/cash_taxes_paid (`IncomeTaxesPaidNet` then `IncomeTaxesPaid`), named EBIT-best (operating_income→ebit__1017→pretax_income+interest_expense_total), and approved BE/SEQ formula/missing policy. Absence of durable STD IDs remains explicit. F.2 ebit_to_ev keeps seeded operating income; never silently substitute EBIT-best. TXPD can be FSDS-only because retained CF allowlist lacks it. TXDITC/DLCCH exact chains remain unresolved: duration1327 is not stock TXDITC, total-net-debt1315 is not short-term DLCCH, and structurally unsupported TXDITC cannot be zero-filled into BE. Shared2.9 support is still required.

Earlier R1–R14/C1–C104 remain binding; consult ledger/CARRY and session5/6 handoffs for unchanged rulings, including C78/C82 coverage/source limitations, C93 uniqueness blockers, C103 disabled beta-return variant and C104 fast-path wave deduplication.

## 1.13: registered, committed, partially evaluated

Report: `.superpowers/sdd/tier1-v2/task-1.13-report.md`. Run root: `atx-db/data/research/work/wave_eval/w1_price/selection-s7/`.

| Registration/source pin | Value |
|---|---|
| Actual registry | `atx-db/data/research/registry/trial_registry.jsonl` |
| Sequence / registered time / cells | 0 /2026-09-27T12:18:48 /432 |
| Registration ID | `3afdfc36b2d257244375fa784d7dfe062ab6a00d31280f85cb26dfa546b3920c` |
| Preregistration SHA | `19f42d0a4629c386170c812f4991caa01815fc077e75f4cf3ebfcd6cf8879310` |
| Registry file SHA / bytes | `c42026c16f3c5087126a24c1178a3e4bd74f64b3b51f37c85168e6d501ddda63` /21,662 |
| Committed anchor SHA / bytes | `26db1b7d59ab184c762795d4c5caae271b833123dae6a674d2651580aadd7cc8` /137 |
| Frozen runner SHA | `a95d8039fae846a993d5d659f32d6dd6acde3264ccc4215093e1a2c9792bb0f1` |
| Catalog | `36bb1bbe472b1bed0371a1f4bb1cb5ebcdc17132669d0bf341e5f5ffaad82654` |
| Manifest ID | `58aeacf3967548de9e7e8cc7c30ce830bca5b7df8a1a3f8eb1a12e537edab4de` |
| Manifest file SHA | `8c234060422e1b209bd1d3e6a7e32b1b7b5692d211ec578d2dce87ec8f62cb05` |
| Lake / digest | `price-wave-0ed96b2696f1-bcdf5d0f3437` / `9eeb25fe81a6543cd46be11267a823a087028a2d7e3c726f7e7eb231a818731e` |
| Labels | `eaf6c8b9450a7fa3f1f282717a44f9ae6f502a2c424d6185199cab317f08c757` |
| Benchmark / digest | `bench-20260926` / `f2ff84035aa968dcbf7da09e12ceb907caa4a4dd3cc74c694a95cb4ebe429648` |
| Policy | `776db445df6323c5d0dfd7db8e080631d94665c6a84e548f4b1d01b148546d9a` |
| Universe basis | `pit_vendor_earnings_evidence_name_pattern_exclusion` |

The registry, anchor and runner hashes were reconfirmed after stop. Registration was reconciled through the API (`1.13-s7-reconcile-r2`, exit0/peak0.0881GiB) before commit251995c7. The initial reconcile correctly refused an empty committed anchor. No duplicate registration was written. `prepare.json` was created2026-09-27T12:35:49 after the anchor commit, before evaluations, and pins runner/EVAL/store-adapter code. Preserve it unchanged.

Grid:36 features×3 transforms(rank_normal,signed_raw,zscore)×horizons1/3/6/12=432 cells;33 gating and3 reported-only(beta_ew_252d,ivol_ew_21d,ivol_ew_252d). zero_trade_21d/252d are source-unsupported and excluded under C82. No beta-return opt-in (C103). Formations2013-01..2023-12; every mature endpoint strictly before2024-01-01. Holdout sealed; maximum status selection_pass. Actual labels span next-session calendar-month entry to entry h calendar months later, not fixed21*h sessions.

Completed12 features (144 cells): `ami_126d,ami_252d,beta_bab_1260d,beta_dimson_252d,beta_down_252d,beta_ew_252d,bidask_ar_21d,bidask_cs_21d,chmom,coskew_252d,dolvol_126d,ret_12_1`. All have caches and `jobs/eval-<feature>.json`. Registration remains432 trials. No grade, final grade ledger or result tables were produced. `atx-db/docs/research/WAVE1_PRICE_PROVISIONAL.md` is untracked methods/pins/limitations only.

First ret_12_1 worker:12 cells,61.1s,native peak0.4524GiB,no cap hit.481,991 context rows/6,566 lines,140 artifact exclusions;1,758,721 accounting rows,zero endpoint mismatch. Counts h1/h3/h6/h12=468,345/455,350/436,139/398,887. Accounting pin `3ddadd4f93f11b3ed522cee4890ffbe48da867b2ee6f9cd9bae65c8ca24d94d3`. First cache relative to `data/research/eval/`:

`843723d019af9172434d8b16e51403003f224177b462ed7c95f15058fba7bd1d/eaf6c8b9450a7fa3f1f282717a44f9ae6f502a2c424d6185199cab317f08c757/6e2a40fc79731e25b0d31640383f881296fd4ea08092cb4734e1f82e0b7f5c73.*`

Maximum of completed continuation workers0.4553GiB(coskew_252d); all cap_hitfalse. First native summary retained separately as `receipts/1.13-s7-first-worker-summary.json`. All13 continuation worker guard receipts copied verbatim to `receipts/1.13-s7-workers/job-{0..12}.receipt.json`: jobs0–10 completed,11 frog_in_pan stopped_parent_gone/137 after33.1s/peak0.4524,12 ivol_ew_21d refused_parent_gone while queued/peak0.0132(no payload). Orchestrator payload6628 was stopped after ownership verification; outer guard18204 finished operator failure4294967295/peak0.0875, not a source failure. Parent-job shutdown drained nested guards. The original temp receipt directory `C:/Users/natha/AppData/Local/Temp/atx_research_jobs_g4mh5ufy` is retained.

### Exact next price command, only after explicit resume

First verify current source still matches the prepared251995c7 pins. Do not prepare a replacement spec, rerun registration, or reevaluate the completed12 before fixing I1. From `C:/atx/atx-db`:

```powershell
$env:OPENBLAS_NUM_THREADS='1'
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --quiet --job-gb 0.2 --allow-nested-guards --wait-minutes 30 --receipt ../.superpowers/sdd/tier1-v2/receipts/1.13-next-resume.json --stdout ../.superpowers/sdd/tier1-v2/receipts/1.13-next-resume.out --stderr ../.superpowers/sdd/tier1-v2/receipts/1.13-next-resume.err -- .venv/Scripts/python.exe scripts/research_run_wave.py run --wave w1_price --run selection-s7 --formations 2013-01..2023-12 --workers 2 --group-size 1 --stages eval,grade --features frog_in_pan,ivol_ew_21d,ivol_ew_252d,me_line_log,prc_highprc_252d,prc_log,price_delay_52w,ret_12_7,ret_1_0,ret_36_13,ret_60_13,ret_6_1,ret_9_1,rmax1_21d,rmax5_21d,rskew_252d,rvol_21d,rvol_252d,seas_1_1an,seas_2_5an,std_dvol_126d,std_turn_126d,turnover_126d,turnover_252d --manifest-sha 58aeacf3967548de9e7e8cc7c30ce830bca5b7df8a1a3f8eb1a12e537edab4de --catalog-digest 36bb1bbe472b1bed0371a1f4bb1cb5ebcdc17132669d0bf341e5f5ffaad82654 --lake-snapshot price-wave-0ed96b2696f1-bcdf5d0f3437 --bench-snapshot bench-20260926 --label-sha eaf6c8b9450a7fa3f1f282717a44f9ae6f502a2c424d6185199cab317f08c757 --require-artifact-flag
```

### Independent controller review: Important I1 still open

Package `pkg-1.13-session7-r0.diff`; review `task-1.13-controller-review-session7.md`. The controller inspected the1464-line runner and provenance. No Critical found in reviewed scope; **Needs fixes**, not accepted.

At runner1052/1058 a full cache hit writes `bases=None` over a prior successful job receipt; grade1086 requires a non-null evaluated basis receipt. Rerunning all completed features can therefore erase the provenance needed to grade valid caches. Author confirmed. Existing12 non-null receipts make the remaining-features resume valid with unchanged source. Finish/preserve that frozen run first.

Then implement the smallest cache-sidecar basis recovery, validate exact current-basis equality, preserve prior family/evidence as appropriate, and prove real cache-hit resume under guard. Changing the file hash changes cache identity: preserve original251995c7 artifacts; use a fresh same-configuration bounded feature if needed for the fix proof. **No source-pin bypass, blind metadata repin, changed cells/policy/accounting, or automatic whole-wave rerun.** Re-review the fix and complete wave/report before task completion. Minor M1: separate `_jobs` invocations overwrite summary.json; keep all36 native worker evidence, including the separately preserved first worker.

## 4.1 real-data parity: measured PASS with economic limitations

`task-4.1-real-session7.md`, probe `probes/4.1-real-session7.py`, machine evidence `receipts/4.1-real-session7-measurement.json`, successful guard `4.1-real-session7-r2.{json,out,err}`. price_wave did not author EVAL, so this is independent of the frozen evaluation change; it is not independent of its own runner.

Against baseline8aba655c and current41168d6f on the identical registered ret_12_1 spec, all143 original columns of12 cells matched exactly, plus old slices258,series1,584,quantiles360,decay36,attrition40,feature_inputs1 and family summary. Exact typed comparison and lossless float-hex canonical bytes both pass. Current outputs also equal the pinned cache (including expanded slices282/quantiles828/decay72). Peak0.5229GiB includes two engines and comparison frames; production worker separately0.4524GiB. No cap hit.

First probe failed only cache feature_inputs variant order after numerical/old-output comparisons: probe catalog order differed from runner store order. Retry fed both engines runner order(rank_normal,signed_raw,zscore), without normalizing outputs or changing tolerance. EVAL/FEAT/catalog/policy stayed unchanged. Do not repeat this successful proof merely for a new parent; reconcile it with the task brief/earlier accepted review before recording broader acceptance.

Five diagnostics populate12/12 cells: JKP capped-value deciles, EW terciles, decile1 ADV capacity, approximate IC MDE and OSAP original-paper t. **Complete-book gross/turnover/cost/net/break-even remain NULL12/12.** Each portfolio has1,560 incomplete formed-book rows out of1,584 total; held/priced counts remain present and no survivor renormalization occurs. Fundamentals FM controls, EA, beta-neutral, internal atx factor run/exact external spans are unavailable or unregistered. All ten portfolio/model claimable flags false. The inherited proxy string mentions21-session labels incorrectly; report discloses actual calendar-month entry windows and keeps external factors unclaimable proxies. Exact accounting availability does not imply finite investment economics.

## F.1/F.2: measured sources, unmeasured item engine

Read `task-F.1-report.md` **Stopped(session7)** first. Accepted design: `docs/superpowers/plans/2026-09-27-tier1-v2-fundamentals-fast-path.md`,672e9920/C104; implementation is not accepted. Four untracked owned source files now exist:

- `src/atx_db/research/fundamental_sources.py`: source verifier, global filing index, pinned raw/context projections, Arrow iterators, bucket Parquet normalization and completion receipts. Three source slices measured.
- `src/atx_db/research/items_map.py`: seeded dependency/alias/unit metadata, C105 overrides, C107 local negative IDs/recipe authorities. Current plan map differs from early preparation map. No STD edits.
- `src/atx_db/research/item_vintages.py`: entirely unmeasured Decimal corroboration, fiscal grids/invalidation, direct/YTD/Q4/TTM, semantic-conflict NULLs, composition, lineage, intervals, bucket/year writing and as-of reader. Code intent is not evidence.
- `scripts/research_fundamentals.py`: prepare/normalize/plan measured; normalize-all/build orchestration and one-bucket engine unmeasured. `audit` still raises NotImplementedError. No final publisher/build-index promotion.

`fundamental_identity.py`, implementation document and optional tests do not exist. No item owner/bucket was executed, no item dataset published, no forward returns/warehouse opened, no registration or feature coverage claim.

Measured source work under `atx-db/data/research/work/fundamentals/f1-s7/`:

| Measurement | Result | Guard native peak |
|---|---|---:|
| prepare | 145 rules/329 alias-basis records; SUB433,717 rows,0 conflicting accession contexts,5,330,210bytes |0.2470GiB|
| FSDS2009q2 |3,999 rows:2,894 eligible/446 custom/269 dimensional or co-registrant/390 disallowed form |0.1334GiB|
| CF batch0 |645,820 rows:640,289 eligible/5,531 disallowed form;10.739s worker |0.3752GiB|
| FSDS2015q4 |2,780,517 rows:1,448,429 eligible/105,420 custom/1,103,147 dimensional/1,536 duration/121,951 form/34 post-snapshot |0.3895GiB|
| diagnostic plan |3 sources only,scope_complete=false,diagnostic=true;2.0s worker after204.1s queue |0.0885GiB|

All above successful receipts cap_hitfalse. Initial2009q2 parser error fixed then measured. Large2015q4 worker67.763s exceeds approximate60s slice target; tighten source subdivision before full normalization. Bucket files are bounded, but a whole-quarter aggregate pass is not yet the intended final slice contract.

Receipts prefixes: `F.1-s7-prepare`, `F.1-s7-normalize-2009q2-r1`, `F.1-s7-normalize-cf0`, `F.1-s7-normalize-2015q4`, `F.1-s7-plan-diagnostic`. Source complete.json IDs/hashes and full commands are in the report. Filing index SHA `32c5af646e1c194770b4a766e342515e373545d1a3ece82e517b5efaf421b8e8`. Early `mapping.json` SHA `27f46fe2891e56e437c55c1ad225529bcec666d7371892907a10073659097dbd`; **current diagnostic plan** map SHA `927420ceecc210d7bada6c7c99c4ba76f9e79700f8378a37b1664ef9ae9924cf`.

Diagnostic plan: `work/fundamentals/f1-s7/fund-bf2b84798566cd5b5e2b/plan.json`. The queued plan finished naturally during STOP ownership verification; no new task launched after stop. Guard19992/wrapper14268/child12432 all gone; tool51916 reaped0. It is a partial-source diagnostic only, not an accepted build.

Next diagnostic from atx-db, unchanged code only:

```powershell
$env:OPENBLAS_NUM_THREADS='1'
$env:PYTHONPATH='C:/atx/atx-db/src'
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --quiet --job-gb 0.6 --wait-minutes 30 --receipt ../.superpowers/sdd/tier1-v2/receipts/F.1-s8-build-b000.json --stdout ../.superpowers/sdd/tier1-v2/receipts/F.1-s8-build-b000.out --stderr ../.superpowers/sdd/tier1-v2/receipts/F.1-s8-build-b000.err -- .venv/Scripts/python.exe scripts/research_fundamentals.py build --pins ../.superpowers/sdd/tier1-v2/receipts/F.1-source-pins.json --work data/research/work/fundamentals/f1-s7 --root data/research --plan data/research/work/fundamentals/f1-s7/fund-bf2b84798566cd5b5e2b/plan.json --bucket 0
```

If code is repaired first, create a fresh explicit `plan ... --diagnostic`; do not rewrite the old plan's code pins. Known correctness work from author: full intermediate lineage; terminal dispositions reconcile to raw-candidate denominator rather than per mapping; same-clock/multiple-duration serving conflicts; fiscal-grid transitions and restatement/NULL propagation; exact-FY TTM fallback when quarterly series incomplete; reader must inspect `verify_lake_snapshot(...)["ok"]` rather than merely call it; immutable lake publication/index, audit and accepted identity projection. Strict missing-value handling must follow approved source-supported historical policy without losing denominators.

TXPD and EBIT-best local recipes compile. BE computation remains withheld because stock TXDITC chain unresolved; DLCCH also unsupported pending evidence. Root retained-TAG probe `session7-taxonomy-probe` failed before queries due PowerShell→Python `-c` quote loss (SyntaxError,exit1,peak0.0206); **it supplied no taxonomy evidence or new authority**. Next bounded audit should inspect retained `tag/2009q2.parquet` and `tag/2025q2.parquet` definitions/iord/datatype for investment-tax-credit stock tags, `ProceedsFromRepaymentsOfShortTermDebt`, `PaymentsForProceedsFromShortTermDebt`, `DeferredIncomeTaxLiabilitiesNet`. Use an apply_patch-authored probe rather than repeating broken inline quoting. No internet result was adopted as a chain ruling.

F.1 hard gates unchanged: FSDS accepted_utc; CF-only max(raw_available_at,filed+46h), exact same fact/context/value required for acceptance borrowing; complete fiscal slots/YTD/Q4/TTM, weighted shares/EPS nonadditive, latest NULL, conflicts before provenance-ID tie-break; numeric/oracle/PIT/resume and≥3,000-owner/month coverage. Item work may precede identity acceptance; projection may not. C104 reserves w2_fund_fast23 canonical hypotheses/default276cells; F.2 pins formulas/configuration and commits registration before returns. Later2.10/w0 deduplicate; changed constructs/clocks/identity count as configurations. One final holdout opening only.

## 3.3 identity/MIG checkpoint

Read current `task-3.3-report.md` stopped section for final receipt outcomes. Thirteen owned WIP paths include the prior12 plus narrowly granted tests/conftest.py. No production access, preparation rebuild or source commit. **No accepted F.1 identity artifact exists.**

Current source changes: dated ticker_tk contiguous observed-XNYS islands in64 slices; ambiguous/non-session days rejected; suffix heuristics retain unknown possible common siblings; read-only attachment of retained preparation to a new output identity.duckdb; public cross-CIK quarantine before tier filters; observation-clock unresolved membership for absent current SEC classes; evidence/key/allocation and132-formation audit. Company ME must retain every low/unknown/unresolved class in the denominator and return NULL when full membership/pricing is incomplete. No primary-line proxy or future current-name/class backfill.

C106 froze0331 DDL/keys/history view locally under checksum and used preserved-catalog copy/swap. Existing0327/0328/0329 checksum pins remain unchanged;0329 edit is docstring-only. Migration file measurement `receipts/3.3/s7-migrations.*`:30 passed/1 failed/1 skipped,318.6s,peak0.3169,no cap hit. Actual0331 transaction/commit/CHECKPOINT/schema verification/catalog preservation/keys passed. Populated historical view exposed DuckDB1.5.5 correlated LATERAL/CTE binder failure (`BIGINT != TIMESTAMP`); author rewrote runtime/frozen history with event-keyed set joins and scheduled the changed case only. Do not infer view correctness from CREATE VIEW parsing or blindly rerun the other30 tests. The focused migration retry and historical suite were both explicitly stopped after37.0s,return4294967295,peaks0.3135/0.1208GiB,no cap hits and **no verdict**. Receipts `s7-migration-history-fix.*` and `s7-historical.*`; rerun these interrupted scopes after resume. Historical stdout had only `ss`.

Real rehearsal attempts, unchanged0.6GiB cap:

- `session7-run1`, `s7-rehearsal-run1.*`:exit1/93.1s/peak0.6005,cap_hittrue during250-line raw share matching; no manifest.
- `session7-run2`, `s7-rehearsal-run2.*`:exit1/175.3s/peak0.6000,cap_hittrue inserting initial evidence with retained JSON/autocommit autocheckpoint; no manifest. Commit-durable/autocheckpoint allocation failure occurred only in scratch.
- `session7-run3`, `s7-rehearsal-run3.*`: changed to stream104 prepared evidence parts independently,1000-row transactions,100-line uncertainty slices, but **failed before stop cleanup**,exit1/378.7s/peak0.6004GiB,cap_hittrue/MemoryError, no manifest or accepted Parquet. Scratch identity.duckdb102,772,736bytes plus16,584,469-byte WAL retained. Source changed after process import, so displayed current line667 is not a reliable operation label; loaded-position comparison places failure around final link/name assembly. Latest event-view/code-pin changes were not loaded in that run. Do not retry the unchanged monolith or raise caps.

Next architecture step is fresh-process, independently pinned phases for symbol islands, evidence/link candidates, raw-share uncertainty, names, and audit/export; stream names and release candidate/by-company/span collections. Preserve full population, valid preparation and failed scratch until safe recovery/reuse is understood. Capture loaded code/input pins at launch. New unrun probes `probes/3.3-session7-audit.py` and `probes/3.3-session7-clock-edges.py` exist: the audit projects only base price keys/clocks/size, never returns, and explicitly does not prove the missing oracle/repeatability/331baseline. Validate their assumptions before execution; neither has measured evidence.

Reusable inputs (do not redo):

- `data/research/identity_inputs/session5/manifest.json`:five small OPS Parquets; identifiers8,031,securities52,133,listings45,820,SECtickers10,438,Nasdaq13,258. Production evidence table absent explicitly.
- `data/research/identity_rehearsal/session5/preparation/preparation.json`, retained.duckdb and104 evidence Parquets, committed7eaafe75:25,760 lines,1,000,917 share facts,9,413 candidate CIKs,28,799 lifecycle filings,12,650 evidence rows. Valid preparation remains read-only.
- `session5-draft1/manifest.json` is scope_complete=false and predates class/denominator corrections; forbidden as accepted projection. Its88.1596% retrospective operating-proxy coverage is not a new verified gate.

Rehearsal command shape: guarded0.6/256MB/threads1, `PYTHONPATH=C:/atx/atx-db/src`, `-m atx_db.identity_links rehearsal --work C:/atx/atx-db/data/research/identity_rehearsal/session5/preparation --ops-manifest C:/atx/atx-db/data/research/identity_inputs/session5/manifest.json --out <NEW_OUTPUT>`. Never overwrite prior proof or mutate preparation. Standalone rehearsal stays independent of unaccepted1.3 WIP.

Acceptance gaps: all25,760 permanent IDs/no-reuse/date uniqueness; all clocks/source hashes,132 monthly tier/basis/company-ME coverage,331 multi-line-CIK baseline;≥88% correctly labeled operating-company gate; real complete-class ME; deterministic repetition;≥50 delisted CIK/date cases across2012–2025 independently evidenced. Retained submissions give CIK/name/lifecycle but not necessarily historical ticker→CIK truth. A metadata consistency sample does not equal that independent oracle.

Old `.superpowers/sdd/tier1-parity/claude-ctl/RI1-report.md` cites `C:/Users/natha/AppData/Local/Temp/claude/c--atx/b4bd2aa4-4a8c-4961-a262-297640e82cb3/scratchpad/RI1-measure/precision_sample.csv`; root exact-path read-only check found it absent. Do not assume it exists or is verified dated evidence. Old report's named delisted cases can guide recovery, not substitute for sources. U5 is granted and task-X.6-brief.md describes dated symbol-bearing Form25/15/10-K/Q evidence if required. No new fetch was started for this oracle. Ledger adapter/atomic publication/kill-resume remain pending accepted1.3.

## X.5 stopped operational checkpoint

Report `task-X.5-ops-session7.md`. Root launched hidden loop18688 at2026-09-27T12:36:02 from fresh export208c3d11; four source files matched commit moduloCRLF. Unchanged loop SHA `f82b53a9c8f38c171643db343858c9bb65841df70cf438d822db3785a403ccc2`. Worker path `.superpowers/sdd/tier1-parity/exports/208c3d11/atx-db/scripts/fetch_sec_earnings_releases.py`.0.5GiB cap,35GiB floor,three threads,exactUA/canonical rate lock. Existing accepted end-state probe0.3922GiB was not repeated.

Stopped only verified owned loop/guard/worker tree after writing `.superpowers/sdd/tier1-v2/receipts/x5/STOP` via apply_patch with timestamp **2026-09-27T12:53:44.0779927Z** and operator reason. No SEC state cleared. Segment `seg-008-20260927T123604Z.{json,out,err,rate.jsonl,guard.log}`. Its JSON still says running because guard17376 was killed; process checks prove it stale. Last sampled native peak0.1332GiB/no cap hit; not a terminal peak measurement.

Last stdout:19,600/426,151 candidates;2,581 newly fetched indexes,2,537documents,zero index/document failures. Durable `atx-db/data/raw/sec-earnings-release/units-done.jsonl`:**19,611 parseable lines,0 corrupt**, versus17,018 at startup. Not a full pass/integrity claim. Entire segment rate log:5,142 grants,0 other events,max5 per closed one-second,min gap0.2001442s. No parse/load or `--recheck-done` pass.

Stop-only guarded reclaim at12:55:11, `receipts/session7-stop-reclaim.{json,out,err}`:exit0/peak0.0283/no cap hit; admission reclaimed dead slot932/guard17376(job_existsfalse,pid_matchfalse); inner reclaim no further stale entries. Final slot count0.

On explicit resume, verify no existing fetch, inspect operator STOP versus trip state, make a **fresh current-HEAD** archive, verify relevant sources, remove only documented operator STOP, and launch hidden with explicit exported `-Worker`. Never use historical default worker path, never clear SEC trips automatically. Reviewed retry contract:exit1≤3/hour with120s delay;exit3 stop;137/78 resume60s unless disk/nesting refusal. Later integrity pass must be selector/gzip-aware. Old status helper text25GB is stale; actual floor35GiB remains binding.

## Accepted inputs and unfinished DAG outside active lanes

Already complete in ledger: M0,0.1–0.15,1.1,1.2,1.7,1.9,1.10,1.11,X.3, plus session5 1.4 retained extract and1.12 corrected research inputs. Do not redispatch completed scopes or equate them with unmet quantitative release criteria.

- Price inputs accepted47e168cd:32,050,111 bars,631,442 spine rows,38 matching feature SHAs;161,332 exclusions split88,305 absent earnings evidence/73,027 future evidence.2,042,924 labels:valid1,869,328,invalid909,terminal_pending64,582,not_matured105,065,missingentry1,211,missingexit1,829. **All required4,500-line/month targets NOT MET; maximum4,147.** Suspect exclusions are not verified repairs; provisional terminals/vendor shares/reconstructed classification stay disclosed.
- FSDS v2 explicit sibling `data/staging/fsds-v2/fsds-staging-manifest.json`, SHA `2cad6134efd312d7ad9ca84bbc274fdf360e10e581b38aa88a29f1040bebc628`; canonicalfsds remainsv1.69quarters/276files; SUB433,717,NUM184,959,880,TAG4,876,740,PRE45,780,878. UTC validation passed;52,567 entry sessions move later. Four orphan accessions/1,953facts retained explicitly; bounded audit fixb095ef20 accepted. X.4 benchmark Step3 belongs2.8/C76.
- CF-R root `data/staging/companyfacts/ee099c7394a357f1/`; manifestSHA `50e018e1c26046f3eb3ec27d2f246c8492e60baddbc911ebfda50320f24ac186`,membersSHA `07943ae309b8a5d10c2fd3caf6fb7110e0830d6e90676d988515cf4465995c0c`.85batches,20,390members,53,751,273facts;16,995loaded/3,333empty/62unavailable/0errors. Exact literal/regression/deterministic extraction accepted; live warehouse parity belongs1.5/1.8. F.1's361-file audit passed0.0944GiB; do not repeat unchanged.
- **1.3** preserved batch runner/correction/slice/recovery/kill-resume WIP needs the next available lane. At most one OPS full-copy drill, reviewed stage interface and cleanup. Unblocks1.5/1.6→1.8,2.1→2.3–2.7,3.1/3.2, X.1/X.2 loads and identity ledger integration.
- **2.2** signal_eval retirement-filter/m1 fix and3tests preserved; scoped re-review outstanding. Prior10issuer/396row restatement tie-out is not permission to skip remaining review.
- **X.1/X.2** fetch WIP preserved, no fetch launched. Verify actual source contracts before use. Prior status records SR-FINRA-2026-012 withdrawn2026-08-05; never implement a withdrawn proposal as operative law.
- **3.6** still blocked by C93 publish uniqueness in3.1/3.2/3.5. Then3.8/3.9(withX.5),4.3 full family,4.4 preregistered composites,4.5 ownership,4.6 monthly JKP-layout export; follow DAG through5.7. No release/signal-qualification claim yet.

## Exact WIP ownership at stop

Paths below are relative to `atx-db/`;32 total. Keep all. The frozen runner/anchor are committed and clean; only the draft1.13 document is WIP.

| Lane | Paths |
|---|---|
| 1.13 (1) | `docs/research/WAVE1_PRICE_PROVISIONAL.md` (untracked) |
| F.1 (4) | untracked `scripts/research_fundamentals.py`; `src/atx_db/research/fundamental_sources.py`, `items_map.py`, `item_vintages.py` |
| 3.3/MIG (13) | modified `scripts/ticker_history_unit_inventory.py`; `src/atx_db/api/catalog.py`; `src/atx_db/identity_links.py`, `market_daily.py`, `market_owner_bridge.py`, `publication.py`, `schema.py`; `src/atx_db/migrations/bodies_0329.py`, `registry.py`; `tests/conftest.py`, `test_historical_identity.py`, `test_migrations.py`; untracked `src/atx_db/migrations/bodies_0331.py` |
| 1.3 (5) | untracked `scripts/run_slices.py`, `recover_stale_run.py`; `src/atx_db/batch_runner.py`, `bars_unit_correction.py`; `tests/test_batch_runner_kill_resume.py` |
| 2.2 (4) | modified `src/atx_db/signal_eval.py`; `tests/test_asof_no_latest_revision.py`, `test_signal_eval.py`, `test_signal_eval_fast_panel.py` |
| X.1/X.2 (5) | modified `scripts/download_13f.py`; `src/atx_db/finra.py`, `short_volume.py`, `thirteenf_archive.py`; untracked `scripts/fetch_finra_short_data.py` |

Other untracked historical handoffs/plans and another session's atx-engine/src/book/replay.cpp remain outside this task's ownership. Never add all files. Handoff commit should include exactly this file and the companion prompt, no source WIP.

## Protected files, scratch and restart order

Production `C:/atx/atx-db/data/warehouse.duckdb`:12,873,379,840bytes,mtime2026-09-25T11:59:36.2668647Z. Backup `warehouse.duckdb.pre-migrate.20260924-225023.bak`:12,883,341,312bytes,mtime2026-09-24T22:50:23.5699261Z. Metadata unchanged; neither opened this session. Last stop check free disk100.12GiB. No full warehouse drill copy was created. Small research scratch, partial identity outputs, frozen caches and exports retained for resume.

Prior automatic approval review rejected removal of `atx-db/data/tmp/session5-cfr-determinism` (“blocked by policy”);15files/14,868,836bytes retained. Do not retry by another deletion mechanism. No cleanup bypass attempted. This is not a blocker for work. Failed/interrupted receipt files are kept honestly rather than rewritten as successful.

On explicit resume:

1. Read this handoff/current reports/rules; verify goal active, HEAD/log/status,32-path ownership, zero owned jobs/slots, production metadata and disk. Do not mistake stale X.5 JSON for liveness; PIDs are historical and must never be killed without fresh creation/command checks. Reclaim dead slots only under guard if needed. Advance report session to8 and append actual-time ledger reconciliation.
2. Dispatch disjoint price/F.1/identity workers from their stopped reports. Price runs remaining24 only with frozen spec; F.1 measures item engine and resolves explicit source gaps; identity finishes bounded rehearsal/test/audit without accepted-manifest fiction. Preserve EVAL/FEAT/CAT freeze throughout the original wave.
3. Parent can resume authorized files-only X.5 from fresh export while reviewing/adjudicating. No source implementation by parent. Reconcile the already-passed4.1 real evidence instead of rerunning it.
4. After full price grading, resolve I1 with bounded proof, complete provisional report and independent review. Take the next available lane for1.3 to unlock the broader DAG. Continue F.1→F.2 and full release criteria; do not stop merely because first-wave results exist.

This stop completed the user's handoff request. It did not complete the production-characteristics goal.
