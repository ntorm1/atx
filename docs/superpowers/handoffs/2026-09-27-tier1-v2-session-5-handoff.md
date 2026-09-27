# Tier-1 v2 — session 5 handoff

## Read this first

The user explicitly requested: **“stop here and write a detailed handoff markdown file for the next parent agent + a goal prompt.”** Work is paused, not complete. The three workers stopped; the SEC fetch was stopped at `2026-09-27T01:35:41.3448686Z`, with its `STOP` file retained. A later interrupted-turn continuation was used only to finish this handoff and recheck state. Do not resume implementation or fetching until the user resumes the goal.

**Code HEAD at stop: `47e168cd`.** The documentation commit containing this handoff may be later. There are **25 preserved atx-db WIP paths**, mapped below. No real wave was registered or graded: the committed trial-registry anchor is still zero bytes, and `atx-db/data/research/trial_registry.jsonl` does not exist. No real forward-return values were read for selection in this session. The holdout remains sealed.

**Immediate next work after resume:** finish the 1.13 runner's unmeasured accounting-adapter preparation, register exactly 432 price-wave cells, commit the anchor, then grade the selection sample. Both prerequisite reviews have now passed. In parallel, finish F.1's item engine and 3.3's identity rehearsal. Keep the 4,500-line coverage deficit explicit.

Authoritative sources:

- Spec: `docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md`, especially §§2–6 and 9; adjacent S0–S5/SX files contain the task details.
- Ledger: `.superpowers/sdd/tier1-v2/progress.md` plus `git log`.
- Binding working context: `.superpowers/sdd/tier1-v2/{CONTEXT,REVIEW-CONTEXT,CARRY}.md`.
- Every active task's report has session-5 progress; F.1 and 3.3 also have explicit stopped checkpoints. Read current sections before historical stopped-session text.
- Original objective: `C:/Users/natha/.codex/attachments/dafee98b-11ae-4f69-a4d6-4d69f2a02d3b/goal-objective.md`.
- Earlier handoff: `docs/superpowers/handoffs/2026-09-27-tier1-v2-next-goal-prompt-3.md`; older rulings remain binding.

## Goal and acceptance boundaries

Build a production-quality, large cross-sectional, deep-history characteristic library with 1–12 month evidence for a downstream equity long/short book. The full goal remains the index §2 exit criteria and §5.7 candidate release. The intermediate minimum is w1_price grading, a fundamentals wave, 4.3 QUALIFIED_SIGNALS, and 4.6 alpha export. None of those four grading/export milestones is complete yet.

Completed before session 5: M0, 0.1–0.15, 1.1, 1.2, 1.7, 1.9, 1.10, 1.11, X.3. Do not redispatch them.

New accepted scopes:

| Work | State at stop |
|---|---|
| X.4 | Corrected FSDS v2 staging and scoped independent review complete. Benchmark Step 3 remains with 2.8 under C-76. |
| 1.4 | Retained CF-R extraction, exact-value fix, regression and independent review complete. Later 1.5/1.8 live warehouse digest proof remains. |
| 1.12 | Corrected research-input implementation/artifacts independently approved and recorded complete under C-78. **Quantitative coverage exit criterion remains NOT MET.** |
| 4.1 | Code/stub phase independently approved after one fix round. EVAL frozen at `41168d6f`; real Step 2 remains incomplete. |
| F.1 design | Independently approved; C-104 ratifies wave/dedup scope. Implementation is only a source verifier so far. |
| X.5 | C-85 code and operational relaunch independently approved. Full fetch/parse/load incomplete; fetch stopped on user request. |
| 3.3 | Retained preparation measured; identity/publication/migration WIP and draft rehearsal **not accepted**. |

An approved narrow scope is not proof of the release criteria. In particular, no required month reaches the 4,500 price-line target; the maximum corrected monthly universe is 4,147. Do not weaken the threshold or imply it passed.

## Commits landed in session 5

Reconcile with `git log --oneline 8aba655c..HEAD`:

| Commit | Node | Content |
|---|---|---|
| `672e9920` | F.1 | Fundamentals fast-path design. |
| `50caac93` | 4.1 | Reported beta-neutral feature transform; FEAT used by accepted price-store hashes. |
| `dd83baec` | 3.3 | Bounded read-only identity input exporter for OPS. |
| `b095ef20` | X.4 | Bound global orphan audit by source quarter, retaining cross-quarter semantics. |
| `7eaafe75` | 3.3 | Retained identity preparation and canonical XNYS next-session clocks. |
| `0f8f752e` | 4.1 | Reported evaluation economics and public label seal metadata API. |
| `c03e687e` | 1.4 | Forced-member/exact-literal verification script, same bytes as measured WIP. |
| `41168d6f` | 4.1 | Correct execution timing, complete traded books and horizon-specific hysteresis. |
| `47e168cd` | 1.12 | Formation-time earnings evidence and past-only artifact exclusions; committed catalog. |

The session started at `8aba655c`; earlier unreviewed fixes `20734bce` (X.5) and `1e438796` (X.4) were accepted during this session. No push/merge/reset/stash/restore/clean occurred.

## 1.12 accepted price inputs — exact handoff

Report: `.superpowers/sdd/tier1-v2/task-1.12-report.md`, sections **Session 5 final measurement and changes** and **Relay to 1.13**. Independent review: `task-1.12-rereview-session5.md`; package `pkg-1.12-session5-r1.diff`. Both original Important findings are closed; one documentation Minor remains.

Root for these artifacts: `C:/atx/atx-db/data/research`.

| Pin | Value |
|---|---|
| Catalog digest | `36bb1bbe472b1bed0371a1f4bb1cb5ebcdc17132669d0bf341e5f5ffaad82654` |
| Feature manifest ID | `58aeacf3967548de9e7e8cc7c30ce830bca5b7df8a1a3f8eb1a12e537edab4de` |
| Manifest path | `manifests/w1_price/58aeacf3967548de9e7e8cc7c30ce830bca5b7df8a1a3f8eb1a12e537edab4de.json` |
| Manifest file SHA256 | `8c234060422e1b209bd1d3e6a7e32b1b7b5692d211ec578d2dce87ec8f62cb05` |
| Lake ID | `price-wave-0ed96b2696f1-bcdf5d0f3437` |
| Lake snapshot digest | `9eeb25fe81a6543cd46be11267a823a087028a2d7e3c726f7e7eb231a818731e` |
| Label SHA | `eaf6c8b9450a7fa3f1f282717a44f9ae6f502a2c424d6185199cab317f08c757` |
| Benchmark snapshot | `bench-20260926` |
| Benchmark digest | `f2ff84035aa968dcbf7da09e12ceb907caa4a4dd3cc74c694a95cb4ebe429648` |
| Policy semantic SHA | `776db445df6323c5d0dfd7db8e080631d94665c6a84e548f4b1d01b148546d9a` |
| Universe basis | `pit_vendor_earnings_evidence_name_pattern_exclusion` |
| Work directory | `work/price_wave/th3-20260920` |

Do not confuse canonical manifest ID with its serialized file SHA. Previous manifests/lakes/labels are retained for provenance and must not be selected as “latest.” Full source/code/seal hashes and exact commands are in the Relay.

Measured acceptance:

- All 16 bar digests, 15 native-year digests, label code and all **38 feature SHAs** match; zero correctness violations. Acceptance job 16 seconds, peak 0.2435 GiB.
- 32,050,111 bars. Corrected spine 631,442 rows; 161,332 removals split exactly into 88,305 without earnings evidence and 73,027 with evidence after formation. Zero admitted absent/future evidence rows.
- 136 suspect bars across 120 lines: 15 mandatory factor flags, 100 flat-close factor flags, 21 sentinels; 148 return-break bars. The corrected sentinel rule removed one future-dependent flag. Six real histories/738 prefix bars show zero prefix-extension changes.
- 21,798 high-close bars on 25 lines have no prior sub-floor anchor and are counted as ambiguous, not classified from a future price.
- Labels: 48 partitions; 510,731 rows per horizon, 2,042,924 total. Valid 1,869,328; invalid 909; terminal_pending 64,582; not_matured 105,065; missing entry 1,211; missing exit 1,829. All 909 artifact-spanning pairs invalid. Zero spine-key or entry/holdout-clock violations. Only keys/clocks/reasons were projected for this audit.
- Maximum native worker 0.4914 GiB; label worker 0.1637; store 0.3159; lake 0.3498. No research cap increase.
- **All 38 coverage requirements NOT MET; zero required months reach 4,500.** Maximum universe 4,147. Detailed 711-line tables: `receipts/1.12-s5-coverage-tables.md` and work `coverage.json`.
- 2020-06 has 3,187 formation bars among 3,454 spine rows. Only April/May 2012 are wholly size-missing; the module's April–June wording is a Minor to fix during a future input-code refresh, not by invalidating this snapshot just for prose.
- Unsupported zero-trade rows remain materialized but excluded from registration under C-82. All-zero in 101/132 and 76/132 selection months for 21d/252d respectively.

C-101 exclusions are suspect-data exclusions, not verified corrections; real corporate actions with offsetting price moves can also be withheld. Reconstructed security types, vendor shares, provisional terminal treatment and retained OHLC issues remain disclosed limits.

## 1.13 — next priority, not started on real returns

Preserved untracked file: `atx-db/scripts/research_run_wave.py`. Report: `task-1.13-report.md`. No provisional result document exists. No registry write or anchor change occurred before the stop. Both prerequisite reviews passed immediately before the stop; the earlier WAITING_1_12 text in the report is historical.

Session-5 preparation already added:

- Correct C-78 universe metadata; exact 36 features × 3 served variants × 4 horizons = **432 cells**.
- Pinned p80 alongside p20/p50, reported auxiliary inventory, pinned benchmark span/original-paper adapters, whole-wave EB reduction, explicit `beta_neutral_registration=None`, default group size 1.
- A new compact `_rebalance_inputs` adapter is present in WIP. It reuses authorized return/status arrays and separately reads endpoint columns to match old/current entry clocks. Its pin binds label/files/windows and runner method/code; prepared-run code-drift checks were added. **This latest adapter was not measured or reviewed before the stop.** Do not call it accepted merely because the underlying 4.1 API passed review.

On resume:

1. Inspect the entire adopted runner WIP, especially the new adapter, argument plumbing, code pins, registry-before-read boundary and sealed endpoint filters. Complete only justified stub/PIT verification first.
2. Use the accepted catalog already committed at `47e168cd`; no new catalog commit is required unless an intentional reviewed change is made.
3. Print/check the registration plan; register exactly 432 cells; commit `atx-db/src/atx_db/seeds/research_trial_registry_anchor.jsonl` **before any real label-value join/read**. Do not use a scratch registry to stand in for this.
4. Grade formations 2013-01..2023-12, with mature endpoints strictly before 2024-01-01. Holdout stays sealed. Actual provisional labels are **next-session calendar-month entry to entry h calendar months later**, not fixed 21/63/126/252-session intervals.
5. Start one feature per 0.6 GiB worker, 0.2 GiB nested-guard orchestrator. Measure the whole label/store/cache-key/evaluation/accounting path before increasing grouping. Keep one frozen EvaluationSpec across workers/resumes.
6. Hand the first registered pinned feature cache to the 4.1 owner for required real old-gate byte-parity and memory evidence. EVAL must not change during the real run.
7. Produce `atx-db/docs/research/WAVE1_PRICE_PROVISIONAL.md`, full statuses, unsupported rows, trial counts, coverage/terminal exclusions and uncertainty. Status ceiling is `selection_pass`, not qualified. Then independent task review.

Complete-book economics may be sparse/NULL because terminal-pending and missing returns cannot be survivor-renormalized away. Preserve counts and the later final-label dependency. External calendar-month factors remain a labeled window proxy unless exact endpoint alignment is separately proved.

## 4.1 — frozen, reviewed code; real Step 2 pending

Commits `50caac93`, `0f8f752e`, `41168d6f`. Report/Relay: `task-4.1-report.md`. Review: `task-4.1-review-session5.md`; approved re-review: `task-4.1-rereview-session5.md`. No repeated unchanged review is needed.

Original gates/policy remain unchanged. Added reported portfolios, FM/HXZ controls, decay, costs/capacity, spanning, publication/original-paper evidence, MDE/EB, public label seal metadata and feature-side beta residualization. Three Important findings were fixed:

1. Drift uses separately pinned exact old-entry-to-current-entry accounting, not a formation-end or arbitrary horizon label fallback. Missing adapter means NULL economics with counts.
2. Formation weights stay fixed. All held names need valid current returns for finite gross/EA/net; no renormalization onto surviving/priced names. Held/priced coverage is explicit.
3. Buy/hold state is independent for each formation offset modulo horizon.

`BasisInputs.rebalance_returns` / `rebalance_returns_sha256` is the accepted interface. Source/content pins, keys/endpoints/statuses and seal boundaries were reviewed. Costs using AR/CS spreads remain formation-spread estimates. Beta-return configurations remain OFF without their own explicit preregistration (C-103); feature residualization alone does not authorize return tests.

Synthetic evidence: all old 143 cell columns/old rows/family output exactly equal the baseline; 144×2,000 with accounting peak 0.3244 GiB; 160×5,000 default inputs peak 0.4886; fixed-accounting edges 0.0891; seal/edge suite 0.1800. These do not prove real worker memory with the new runner adapter. Real Step 2 remains required.

## F.1/F.2 — design accepted, implementation just begun

Design: `docs/superpowers/plans/2026-09-27-tier1-v2-fundamentals-fast-path.md` (`672e9920`). Briefs: `task-F.1-brief.md`, `task-F.2-brief.md`. Approved design review: `task-F.1-design-review.md`. Implementation checkpoint: `task-F.1-report.md`, **Stopped (session 5)**.

Only `research/fundamental_sources.py` exists as new uncommitted implementation. It validates explicit manifest versions/scope/path/file hashes. Source audit passed all 361 consumed table/fact files (276 FSDS + 85 CF), plus plan/member/batch pins: peak 0.0944 GiB, 8 seconds. Evidence: `receipts/F.1-source-{verify,pins}*`, probe `probes/F.1-verify-sources.py`.

No `items_map.py`, `item_vintages.py`, `fundamental_identity.py`, build CLI or accepted item output exists yet. Resume mapping/candidates/clock and fiscal engine, not the already completed unchanged source audit.

Required design contracts:

- Shared Parquet item engine for later 2.9; no competing warehouse or manifest system. Use bounded source/bucket/year slices and existing lake publication.
- FSDS uses corrected `accepted_utc`. CF-only effective clock is `max(raw_available_at, filed_date+46h)`, not the staging +22h stamp. Same accession alone cannot borrow FSDS acceptance; exact same-fact context/value corroboration is required.
- Fiscal-slot lags, proven duration starts, explicit YTD→quarter/Q4 rules, nonadditive weighted shares, complete TTM intervals, event restatement propagation, latest-NULL wins. Conflicts at equal semantic precedence become NULL **before** row-ID representative selection.
- Exact point-in-time CIK→line links and all-class company ME. No current snapshot backfill or future tier/class upgrades. Source/item work can precede identity, but projection acceptance cannot.
- Original numeric/PIT/fiscal/50-owner oracle/resume gates and >=3,000-owner target remain. No silent population narrowing.

C-104 ratifies `w2_fund_fast`: 23 requested canonical hypotheses, default 276 cells at 3 transforms × 4 horizons. Commit formula/input/dedup pins and anchor before returns; later 2.10/w2_fund_a and w0 exclude the same canonical hypotheses. Changed constructs/clock/identity bases count as new configurations. Source-unsupported deferrals need pre-return evidence/controller decision; buildable low-coverage rows remain counted. One final holdout opening, never an extra qualification vote from rematerialization.

New unresolved mapping observation: existing fundamental-item seed metadata calls RECT/AP “quantity” and OANCF “ratio”, whereas approved definitions/statement-map USD contexts are monetary. No seed or mapping decision was applied. Resolve/pin unit authority and report discrepancies; do not silently edit STD-owned seeds.

## Accepted accounting source artifacts

### X.4 FSDS

Use `atx-db/data/staging/fsds-v2/fsds-staging-manifest.json`, SHA `2cad6134efd312d7ad9ca84bbc274fdf360e10e581b38aa88a29f1040bebc628`. **Canonical `data/staging/fsds` remains v1.** No promotion is necessary to consume the explicit verified v2 sibling.

All 69 quarters/276 file hashes verified, zero problems. SUB 433,717; NUM 184,959,880; TAG 4,876,740; PRE 45,780,878. All filing clocks independently checked: 291,107 EDT and 142,610 EST; zero null/ambiguous/nonexistent clocks. Correct UTC conversion moves 52,567 entry sessions later; 381,150 unchanged. Eight DST boundaries pass; 12,519 retained source SUB clocks and 184 exact decimal samples match. Four global orphan accessions/1,953 facts remain explicitly unrecoverable.

The initial all-history orphan audit OOMed at DuckDB 256MB after all quarters staged. `b095ef20` preserves global SUB/PRE semantics while materializing one NUM quarter's accession counts at a time. Resume reused all quarters: peak 0.1426; full verification 0.1715; independent clock/source probe 0.1278. Reports `task-X.4-{report,ops-session5,rereview-session5}.md`; receipts `session5-x4-*`. Benchmark/documentation follow-up remains in CARRY 2.8.

### 1.4 CF-R

Accepted root `atx-db/data/staging/companyfacts/ee099c7394a357f1/`:

- Manifest SHA `50e018e1c26046f3eb3ec27d2f246c8492e60baddbc911ebfda50320f24ac186`.
- Members SHA `07943ae309b8a5d10c2fd3caf6fb7110e0830d6e90676d988515cf4465995c0c`.
- Archive SHA `ee099c7394a357f1996c728b7362f25158613b34f2ab1ad270cffc1befb917b6`.
- 85 batches, 20,390 members; 16,995 loaded / 3,333 empty / 62 unavailable / 0 errors; 53,751,273 facts, 296,626,480 Parquet bytes; one exact-literal row.
- Original 10,832-member prefix reproduces 9,462 loaded / 1,327 empty / 43 unavailable exactly. C-102 distinguishes this from whole-archive counts.
- 20 sampled members + forced CIK1065088 + 8 other dispositions passed. Literal `31354367947000000000` is preserved despite DOUBLE differing by 512.
- Regression across 67 retained v1 batches/42,876,876 comparable rows passed, only expected CIK1065088 correction. C-87 255-concept projection exactly 39,457,715 rows / 9,462 CIKs. Thirteen added concepts contribute 239,175 prefix / 342,372 whole-archive rows; do not erase them to force old totals.
- Re-extracted batches 0/19/35/84 have byte-identical Parquet hashes. Extraction max 0.4182 GiB, sample 0.3611, regression 0.2865, determinism worker 0.3981.

Reports `task-1.4-{report,ops-session5,review-session5}.md`; receipts `session5-cfr-*`. The old v1 sibling is retained. Live warehouse parity is still a future 1.5/1.8 gate.

## 3.3 identity — preserve WIP, draft is not accepted

Read `task-3.3-report.md`, especially **Stopped (session 5)** and its six next steps. Committed exporter `dd83baec` and preparation `7eaafe75` are measured. Nine source paths remain uncommitted, including partial schema/publication/API hooks. Migration0331, registry changes, tests/test_migrations edits and ledger adapter do **not** exist yet.

OPS exported five small Parquets under `data/research/identity_inputs/session5/`: identifiers 8,031 allowed CIK/LEI/FIGI rows, securities 52,133, listings 45,820, SEC tickers 10,438, Nasdaq directory 13,258. The raw production evidence table is missing. Exporter opens were read-only, one guarded HEAVY slice at a time; max 0.3556 GiB; production/backup metadata unchanged.

Retained preparation at `data/research/identity_rehearsal/session5/preparation/` is complete: 25,760 vendor lines; 1,000,917 share facts; 9,413 candidate CIKs; 28,799 lifecycle filings; 12,650 evidence rows in 104 hashed parts. Method `ri1_share_fingerprint_v3_xnys`. Receipts `receipts/3.3/prep-{vendor,facts,reconstruct}.*`; peaks 0.1840/0.2016/0.2951 GiB. Preserve valid preparation; do not repeat unchanged reconstruction.

`session5-draft1/manifest.json` is exploratory and explicitly `scope_complete=false`: 25,760 permanent lines, 9,382 companies, 30,952 link versions, 39,261 names, 24,666 evidence events; operating proxy 7,535/8,547 = 88.1596%; peak 0.3584 GiB. It predates known class/clock fixes and **must not feed F.1 as accepted identity**.

Unrun fixes now retain class membership/unknowns before tier filtering, cross-company ambiguity and raw-share uncertainty, and event-local classification evidence. Remaining checks are substantial:

- High-only filtering must not turn a high+medium two-class company into a falsely complete one-class ME. Missing/low/unknown/unresolved possible siblings must stay in the denominator and withhold values where unproven.
- Earlier tier events cannot reuse the final classification. Current snapshots and names/LEI/FIGI must not backfill history. Vendor symbol min/max aggregation must resolve or reject non-contiguous symbol islands.
- Re-run corrected rehearsal under a new output name; check unexecuted SQL/tuple shapes and historical public view.
- Implement0331 part1, integration and real-shaped migration proof. `tests/test_migrations.py` was granted but remains unchanged. Confirm the0329 module-docstring edit does not alter the callable checksum; never silently repin.
- Implement ledger stage only against accepted1.3; its WIP is absent from fresh exports. Standalone rehearsal must remain independent.
- Complete delisted-case, multi-class, monthly tier/basis, identity no-reuse, uniqueness, clock, repeatability and kill/resume acceptance from the brief.

No production identity materialization occurred.

## X.5 fetch — stopped, resumable

Code `20734bce` and C-85 changes independently approved in `task-X.5-rereview-session5.md`: rate-log failures are nonfatal; authoritative limiter state remains fail-closed; sharing I/O retried; one-member streaming parse; selector-aware done units; ASCII Seq; explicit repair pass; bounded exit-1 retry policy.

End-state probe: 826,000 ledger entries + 426,151 done units and four large CIKs, peak **0.3922 GiB**. A 0.5 GiB cap has 27.49% headroom. No need repeat unchanged probe. Loop now enforces **35 GiB** disk floor, <=3 exit-1 relaunches/hour with120s delay, exit3 stops, no trip auto-clear.

Session5 launched hidden loop16668 from fresh export0f8f752e at01:09:55Z. First grant19.267s; initial307 grants had max5/closed1s and minimum gap0.200273s; zero limiter events/failures. Segment: `receipts/x5/seg-007-20260927T010956Z.*`.

The owner inspected and stopped its exact loop/guard/worker tree on user request. `receipts/x5/STOP` contains `2026-09-27T01:35:41.3448686Z`. Final reported progress was **17,000 / 426,151 candidates**, 3,852 new indexes and 3,618 documents in this segment, zero fetch failures. That progress line is a lower bound on durable completed units; inspect the sidecar on resume. The guard receipt may retain a running status after forced tree termination: authoritative process checks show the handles gone. Do not mistake the stale receipt for a live worker.

On authorized resume, check processes/limiter/disk; make a fresh current-HEAD archive; remove only this documented operator STOP; launch hidden with explicit exported `-Worker` and `-JobGb 0.5`. Never clear a SEC trip implicitly. Loop/status/stop scripts are under `receipts/x5/`. Full fetch, explicit integrity recheck and ledgered parse/load remain pending. Old estimate was roughly57h from13k; it is not a completion prediction.

## Preserved WIP ownership — all paths relative to atx-db

| Lane | Paths |
|---|---|
| 1.13 | Untracked `scripts/research_run_wave.py` |
| F.1 | Untracked `src/atx_db/research/fundamental_sources.py` |
| 3.3 / MIG | Modified `scripts/ticker_history_unit_inventory.py`; `src/atx_db/{identity_links,market_daily,market_owner_bridge,publication,schema}.py`; `src/atx_db/api/catalog.py`; `src/atx_db/migrations/bodies_0329.py`; `tests/test_historical_identity.py` |
| 1.3 | Untracked `src/atx_db/{batch_runner,bars_unit_correction}.py`; `scripts/{run_slices,recover_stale_run}.py`; `tests/test_batch_runner_kill_resume.py` |
| 2.2 | Modified `src/atx_db/signal_eval.py`; `tests/{test_asof_no_latest_revision,test_signal_eval,test_signal_eval_fast_panel}.py` |
| X.1/X.2 | Modified `scripts/download_13f.py`; `src/atx_db/{finra,short_volume,thirteenf_archive}.py`; untracked `scripts/fetch_finra_short_data.py` |

The last three lanes were preserved from session4 and not implemented here. 1.3 has no report/accepted interface yet: adopt its WIP, finish the allowed kill/resume test, commit, then OPS fresh-copy drill and review. The dead old1.3 full DB copy was deleted during reconciliation. 2.2 still needs its scoped fix/review. X.1/X.2 fetching has not started.

X.2 prerequisite: official SEC FINRA rulemaking index reported **SR-FINRA-2026-012 withdrawn on2026-08-05**, not an effective daily/granular SI regime. Source: https://www.sec.gov/rules-regulations/self-regulatory-organization-rulemaking/finra?page=1 . Verify the actual public source contract and publication clocks before fetching; do not implement the withdrawn proposal as law.

## Runtime, safety and review rules

User asks for subagent swarms and independent reviews. This runtime allowed only four total agent threads including root, even after a worker completed; we reused three workers for disjoint tasks and cross-reviews. Never let an author acceptance-review its own task. Original Opus runtime was unavailable; C-96 records inherited available models and dedicated `apply_patch` editing. Preserve the required coauthor trailer as requested metadata; do not misrepresent the current model. Graph MCP tools were unavailable, so targeted rg/file reads were the documented fallback.

Follow R1–R14 and C1–C104. New rulings in this session:

| Ruling | Meaning |
|---|---|
| C94 | Refresh task report after every commit/run/stage; session checkpoints must survive abrupt stops. |
| C95 | Adopt exact session4 WIP by lane; never discard it. |
| C96 | Runtime adaptation: inherited model, four threads, apply_patch, unavailable graph fallback. |
| C97 | 4.1 public seal metadata API and one registry caller/docs scope. |
| C98 | Reuse worker threads for disjoint tasks and independent cross-review. |
| C99 | Controller may act OPS for short read-only fresh-export identity slices. |
| C100 | Narrow3.3 market_daily tier/source plumbing and CARRY0329/inventory/abort-proof scope. |
| C101 | Past-only artifact rules; no future sentinel anchor; ambiguity and false positives disclosed. |
| C102 | Original empty/unavailable counts apply to verified prefix, whole archive separately reported. |
| C103 | Beta-return configurations require preregistration; off for432-cell w1. |
| C104 | F.1 common engine/F.2 wave split and dedup; semantic conflicts before provenance tie-break. |

Every Python/pytest/DuckDB process, including one-line probes, must run through `.superpowers/sdd/tier1-parity/run_memory_guarded.py` with `OPENBLAS_NUM_THREADS=1`. Interpreter `C:/atx/atx-db/.venv/Scripts/python.exe`. Research/probes/tests0.6GiB with DuckDB256MB/threads1 at connect; writers0.8; set-based1.0 maximum; orchestrators0.2 with `--allow-nested-guards`. Total running caps<=2GiB. No guard overrides for real jobs; pytest `-n 0`. Exit137/78 is resumable, not grounds to raise caps. Only OPS holds HEAVY, one production slice from fresh committed export at a time.

No TDD; acceptance is measured real data. Tests only for PIT edges, real-shaped migrations, ledger kill/resume or one pipeline e2e. Do not repeat successful unchanged suites merely for reassurance. Review packages: `bash .superpowers/sdd/tier1-v2/pkg.sh OUT SHA...` with explicit commits. At most two fix rounds, then adjudicate.

Keep >=35GiB free and at most one full DB copy. Never delete production or its protected backup, and never touch other sessions' processes or `atx-engine/`, `atx-vol/`, root `python/`, `C:/atx-wt/`, or protected alpha-engine plans. Never push/merge/stash/reset/restore/clean/discard. Commit explicit owned pathspecs, `[v2 <node>]`, trailer `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.

Source gates U2/U3/U5/U6/U8/U9 are granted files-only; U1/U4/U7/U10 are not executed. SEC UA exactly `atx-db/0.1 atx-research@example.com`, shared <=5req/s limiter. Snapshot stays2026-09-20, cutoff22:00UTC. Never present reconstruction as verified vintage.

Ledger edits use the dedicated editor, inserted immediately before the historical final11:50Z HOLD sentinel. Obtain each stamp immediately beforehand with `date -u +%H:%MZ`; never estimate it.

## Final process/disk state and next-session order

At handoff verification: no owned pipeline/fetch/guard Python processes; only VS Code Python services remain and were untouched. Workers are no longer live in the team listing. HEAVY is free. Production remains12,873,379,840 bytes, mtime2026-09-25T11:59:36Z; protected backup12,883,341,312 bytes, mtime2026-09-24T22:50:23Z. Latest free disk101.75GiB (was99.76GiB at the original stop). Recheck on resume.

The controller's `atx-db/data/tmp/session5-cfr-determinism` remains:15files/14,868,836bytes. Automatic approval review rejected its native PowerShell cleanup with **“blocked by policy”**; no bypass was attempted. All receipts remain. This small scratch does not block research. The synthetic X.5 end-memory scratch was successfully removed earlier. Retained identity preparation is resumable derived data, not a production clone.

After the user resumes:

1. Reconcile log/status/WIP against this table; verify actual processes, disk and protected DB metadata. Reclaim only dead guard slots through a guarded0.2GiB reclaim if needed. Do not kill IDE/other-session processes.
2. Dispatch 1.13 preparation→registration→real grade, F.1 implementation, and3.3 completion as the priority disjoint lanes. Reuse or replace workers according to actual runtime capacity; use independent reviews and keep reports current.
3. Relaunch X.5 under the already authorized source scope after fresh-export/STOP/limiter checks. Keep it separate from real grading memory admission.
4. Start1.3 as the next available implementation lane; OPS owns its single fresh full-copy drill. It unblocks1.5/1.6/1.8 and the warehouse ladder. Finish2.2 andX.1/X.2 in disjoint available lanes.
5. Finish F.1 audits/review, F.2 preregistration and grade, then the full DAG through final labels, external validation, QUALIFIED_SIGNALS, composites, ownership, alpha export and release. Preserve every unmet criterion; no full-goal completion claim is justified yet.
