# Tier-1 parity program ledger (atx-db)
Spec: docs/superpowers/specs/2026-09-19-tier1-parity-design.md
Audits: .superpowers/sdd/tier1-parity/audit-atx-db.md, audit-ticker-zip.md
Baseline: main a79f8371, smoke 6 passed.

## Query-driven production ruling - user, 2026-09-21 local

Implementation reservation0320: reported_quarter_eps_implementation exclusively
owns the additive earnings-release receipt/evidence migration in its draft,
including its own body/import/registration. Activation/jobs draft edits are
serialized to the same owner; root integrates only after the active writer.
Issuer query work remains disjoint, with actual CIK owner discovery and no
raw receipt mutation.

Reported-EPS availability follows the existing daily FC1 policy when exact
source dissemination cannot be qualified: valid SEC filing date plus46h,
max with any later qualified availability, explicitly labeled conservative
date evidence. Unknown naive timestamps are never promoted to exact UTC;
acceptance is not automatically dissemination. This allows useful dated
accounting content without a false intraday-vintage claim. SEC source guidance:
https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data
This supersedes the planning brief's overbroad discard interpretation. Local
bulk filing/cache inputs should be supported; no daily Feed archive download
has been started or historical-vintage qualification inferred.

The user directs work through institutional equity long/short desk questions:
write the SQL expected to answer an alpha-relevant question, execute it on
the warehouse, inspect missing or incorrect behavior, and implement a general
fix. Questions, executable SQL, expected results, observed results, provenance
and availability are the acceptance evidence. Avoid task selection driven
only by catalog breadth, isolated edge cases, or repeated test loops.

CVX quarterly GAAP diluted-EPS YoY is the first measured case:
atx-db/docs/CVX_EPS_ACCEPTANCE.md. Root found Q1/Q2 raw inputs present, direct
Q4 inputs absent, all three core materialized tables empty, and a conservative
CIK/security identity link missing. Existing EPS subtraction protection is
correct. Follow this with investable earnings/profitability changes, valuation,
and forward-return decile spreads after costs, reusing the implemented CF1
evaluation and full-universe metric machinery rather than creating parallel
unverified signal paths. No significance or production-readiness claim without
actual outputs and appropriately scoped evidence.

VR1 is committed fd2738c7 after one reviewed draft, scoped Critical closure,
five initial focused passes plus the affected pass after a fixture-count
correction. Ruff passes; four strict-mypy diagnostics match isolated baseline.
Archive7 stopped on host headroom, retained46,639,358facts/points, and its
actual UUID404f66a2-655b-4611-a7a0-b71d4dc6d4d3 was recovered failed. The next
resume preserves its raw schema, fingerprints, ownership and source scope.
Issuer-query and reported-quarter EPS plans are under source-level review;
neither is an authorization to backdate current ticker links, rewrite raw
receipt evidence, silently limit production to two years, or fabricate EPS.

## Verified resume recycling ruling - 2026-09-21 22:38 UTC

Archive7 remains the sole guarded writer; its6805receipt proof passed. The
static verified-resume-prefix-audit.md explains source-level lifecycle work
after proof: candidate-only verified members share the raw replacement
ten-member recycle counter. The measured archive6 prefix took15minutes but
the contribution of each operation is not measured. Prepare VR1 under
verified-resume-recycling-brief.md as an isolated draft, preserving raw/empty
replacement cadence10 and adding a separate bounded100 verified-candidate
cadence. Either reopen resets both. Preserve all proofs, source scope, row
ownership, candidate transactions, empty cleanup, clocks and final/postproof
reopen. Do not remove candidate recycling entirely. Codex implementer then one
fresh review; root integration/focused checks only after the writer is terminal.
No extra full-universe rewrite solely to benchmark; no performance claim yet.

## Explicit user resume - 2026-09-21 21:55 UTC

The user stopped archive5 and has now explicitly requested continuation. Recover
only its verified interrupted ledgers, preserve all committed data, and resume
from actual datasetUUID6400b3c2-f0d1-4f47-bcf0-95aadd9241de. Snapshot remains
2026-09-20. The latest continuation-queue.md overrides old LIVE statements.
Host free physical4.2-4.7GiB is below the3GiBjob preflight requirement5GiB.
For archive6, reduce the process-tree ceiling to2GiB while retaining DuckDB1GB,
one thread, cap+2GiB preflight and runtime physical1.5GiB/commit3GiB guards.
This lowers maximum memory exposure; it does not establish capacity success.
No escalation, other-app termination or global settings change. Root alone
owns runtime; Codex agents remain static-only during production. An actual
capacity failure is a bounded task. All full production outputs, measurements,
publication and sprint gates remain required; no main merge without user OK.

## Production continuation ruling - 2026-09-21 00:43 UTC

Archive4 stopped on low host commit headroom; guard protection is retained.
42,052,707 fact and point rows survived. Actual interrupted dataset UUID
c7c21cd1-c7c4-4de8-af64-4f2877ef6f59 is terminal failed after explicit ledger
recovery00:36:56UTC, verified00:37:38. Resume from this UUID, preserving source
receipts and normal3GiB process/1GB DuckDB/1thread budgets. All runtime serial.

FC1 effective clock correction is authorized before run5, from the source-level
audit fundamental-availability-clock-audit.md: date-only same-day22UTC can be
earlier than a winter filing acceptance. Project SEC eligibility no earlier than
stored time and filed date +46h, before revision ordering, and close raw-point
PIT reader bypasses. Preserve raw tables/CF5 evidence; disclose that this is a
conservative date policy, not exact acceptance or delivery proof. Reserve0319
only for its governed view/catalog metadata. Scope in fundamental-clock-policy-
brief.md; one fresh Codex implementer/review, root focused checks. AP1/MP1 static
reviews clean; DP1 review fixes accepted pending runtime. No new quality claims.

## Production capacity continuation ruling - 2026-09-20 23:56 UTC

Archive4 remains healthy and owns the only runtime slot. A focused static audit
found full-universe indexed publication transactions in the early fundamentals
stages and a whole-run derived identifier cursor/connection. Root also found the
five-horizon forward panel published in one indexed transaction. These are
source-level capacity findings supported by prior measured price/CF5 failures,
not newly measured run5 failures. Preparing bounded publication now advances the
full production objective without interrupting ingestion or raising memory.

Fresh Codex draft-only tasks: AP1 fundamentals-publication-capacity-brief.md,
DP1 derived-connection-capacity-brief.md, FP1 forward-publication-capacity-brief.md.
FP1 reserves0317; AP1 reserves0318. Only optional secondary-index policy may
change under those governed migrations; preserve all required keys, physical
columns, defaults, nullability, views, atomicity, full scope and calculations.
No historical migration/checksum edits. Root serializes integration with owned
bodies/registry lines after the writer finishes. DP1 requires no migration.
All agents remain static-only; root alone executes guarded focused checks.
One fresh review per task; Important fixes accepted on report, re-review Critical
only. Actual post-integration/production evidence remains mandatory. Latest
continuation-queue.md records exact task ownership and live process state.

MP1 addendum, 2026-09-21 00:14 UTC: the existing daily-market SQL batches also
retain one connection across the complete price universe. Prepare the disjoint
market-connection-capacity-brief.md draft, preserving batch atomicity and all
calculations while recycling after committed batches. No migration is needed.
All four capacity tasks use CF5's configured-persistent eligibility rule;
unconfigured and in-memory callers retain their session. DP1's one review found
repeated full-fact ID enumeration and a test-baseline weakness; both are fixed
and accepted on the implementer report pending root execution. FP1's one static
review is clean and its patch applies in check-only mode. Neither is live yet.

## Annual fallback continuation ruling - 2026-09-20 21:53 UTC

AF1 addresses the design's fiscal-year fallback promise and the confirmed
annual-only filer gap in `annual-filer-metric-path-report.md`. Prepare a bounded
implementation under `annual-fallback-draft/` while archive3 owns the warehouse.
The fresh Codex implementer follows `annual-fallback-implementation-brief.md`:
no live source changes, tests, DB access, or commits. P1/0315 and Core remain
frozen until root runtime verification and separate commits, P1 first. Apply
AF1 only afterward, verify focused cases, then one independent review. Migration
0316 is reserved in the draft only if needed for inspectable source precedence.
No fabricated quarterly flows, no stale FY total at a later fiscal endpoint,
and no coverage threshold changes. This is core production work, not a reason
to interrupt or compete with healthy archive ingestion.

## Production continuation ruling — 2026-09-20 20:52 UTC

The latest `continuation-queue.md` override is authoritative for live process
state and task ownership. Companyfacts archive3 remains the sole guarded writer;
root alone runs DB/tests/probes, sequentially. Do not interrupt healthy ingestion
or increase memory budgets for speculative optimization.

The user's later request for complete professional growth/per-share families
supersedes the earlier S3 QoQ/CAGR breadth cut. The bounded core-metric-breadth
wave adds28 catalog definitions from existing inputs (201 total); implementation
is awaiting review/root tests and is not measured live coverage. Existing173
definitions remain unchanged. The four dead statement-derived codes for DSO,
DIO,DPO and cash conversion cycle are explicitly reclaimed in derived_registry.
Normal activation reseeds the CSV; no migration is needed for that wave.

Static audit `derived-pit-revision-audit.md` confirms a Critical historical
correctness defect despite completed prior S3 code tasks: latest-only input
selection plus scoped replacement discards original derived revisions, and
daily direct-fact/DEI joins repeat the problem. P1 under
`derived-pit-revision-brief.md` is required before full derived/market activation.
It owns migration0315 if needed for explicit invalid and valid revision states,
bounded affected-event frames, deterministic lineage, and all traced consumers.
One fresh implementation review must assess closure; additional review only for
Critical fixes. No historical-vintage certification follows from modeled clocks.
Full measurements/publication/gate suite/whole-branch review and explicit user
permission before merging main remain outstanding; preserve stash@{0}.

Facts:
- No live warehouse.duckdb, no companyfacts.zip/submissions.zip on this machine. Foundation rebuilt from scratch.
- Ticker archive: 31,464,423 rows, 2012-03-26..2026-06-15, 70 TSV cols, single 11 GB member; loader exists (ticker_history.py, ticker_history_bulk.py needs extracted TSV).
- Migrations head 0299; pf4-S5 branch stranded (migration collision). Ruling: do NOT rebase pf4-s5; re-derive only what Tier-1 needs.
- Disk free 192 GB.

Sprints:
- S1 foundation activation: plan docs/superpowers/plans/2026-09-19-tier1-s1-foundation.md
- S2 standardization breadth/depth: plan docs/superpowers/plans/2026-09-19-tier1-s2-standardization.md
- S3 derived engine + market join: plan TBD after S1 lands
- S4 quality/universe/delisting/publication/docs: plan TBD

Rulings:
- Ruling: SEC User-Agent for bulk downloads is parameterized (ATX_SEC_USER_AGENT required); operator run uses a non-email product/contact string first; if SEC 403s, surface to user rather than embed user email — cost if wrong: one blocked download, retried after user input.
- Ruling: branch feat/tier1-parity off main; commit per task; merge to main at sprint gates (user asked commit as we go).

## S2 preflight rulings (planner-raised)
- Ruling: accept Task 5 engine change (coalesce_or_sum / coalesce_or_difference) for derived-if-missing — cost if wrong: ~60 lines in two engines.
- Ruling: keep availability-first ordering (available_at DESC, then alias priority) — PIT correctness dominates alias preference — cost if wrong: alias priority is only a tie-break.
- Ruling: accept fundamental_items.csv alias re-sort to (priority, scheme, code); output-neutral.
- Ruling: accept 3-file coordinated alias edits via scripts/apply_alias_wave.py; Task 1 (statement map → CSV) is prerequisite.
- Ruling: alias uniqueness stays global; bank interest_expense_bank routed to deposit/borrowing-specific tags; no template column this sprint.
- Ruling: single stock-compensation item 1308 (spec double-count collapsed).
- Ruling: FFO ships as FFO-before-gains in S2; n-ary signed composition + gain_on_sale item deferred to S3 derived engine.
- Ruling: ≥110-item gate counts all published registry items, not only spec-catalog codes.
- Ruling: employees item 1052 not_available=True (no numeric XBRL fact).
- Ruling: commit trailer is `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Ruling: 10 tasks accepted; pinned-test updates allowed per task with justification in commit body.
- Ruling: fixture CIK 0000095029 (Sturm Ruger) accepted; operator verifies on the network run.

## S3 preflight rulings (planner-raised)
- S3 plan: docs/superpowers/plans/2026-09-19-tier1-s3-derived-engine.md (10 tasks, 173 metrics, migrations 0302+0303, 18 modules retired / 44 kept).
- Ruling: parity contract = engine metric level as-of-joined to the module's monthly grid, sign-normalized, equals module raw_value at 1e-9 rel; plus cohort-matched z-score check via projection — cost if wrong: parity harness measures the wrong thing; caught by the whole-branch review.
- Ruling: accept derived_factor_projection.py + derived_factor_projections.csv so retired factor_ids keep publishing.
- Ruling: modules with importers (asset_growth, piotroski, quarterly_roe, ...) stay; cascade retirement deferred to S4/S5.
- Ruling: DSL adds indicator_gt/indicator_lt, lag_d/avg_d/tret/rvol.
- Ruling: RECLAIMED_ITEM_CODES: 27 dead `derived` registry codes owned by the engine; total_debt_q / effective_tax_rate_ttm renamed to avoid live-item collisions.
- Ruling: old market_cap/enterprise_value tables (valuation_multiples.py) stay untouched this sprint; daily market_cap/EV live in market_daily_metrics; retirement deferred.
- Ruling: market_daily_metrics stays WIDE with a migration-pinned 30-column set (31M rows × 30 cols beats ~900M long rows); new daily metrics need a migration — accepted.
- Ruling: S3 Task 10 activation half executes only after S1 Tasks 5-7 land (sequencing S1 → S2 → S3 guarantees this).

## S4 preflight rulings (planner-raised)
- S4 plan: docs/superpowers/plans/2026-09-19-tier1-s4-quality-universe-publication.md (11 tasks, migrations 0304-0309).
- Ruling: retirement wave 2 = abnormal_capex + operating_leverage only; script-backed modules stay.
- Ruling: S3 Task 9 dispatch carries an addendum: seed api_schema_coverage_slo rows for derived-metrics and market-daily-1d (else provider_coverage stage fails after S3); S4 Task 7 keeps the idempotent INSERT OR REPLACE as belt-and-braces.
- Ruling: no fourth `fundamentals-core` schema code; release dataset cites existing `standardized` schema.
- Ruling: identity checks use seed canonical codes (gross_profit__1004, cost_of_revenue_cogs, cash_flow_from_operations, ...); S2 adds items/aliases but does not rename.
- Ruling: v_security_master_public (CUSIP-free, PIT columns) backs the public schema; existing view untouched.
- Ruling: Shumway performance-delisting convention is ON by default (−0.30 when terminal return unknown and reason is performance-related; −0.55 for the NASDAQ variant per plan), opt-out via option=None; documented in runbook + data dictionary — cost if wrong: imputed returns in survivorship-adjusted series; loud via the coverage gate either way.
- Ruling: market_cap_decile recorded at valid_from only; include_snapshot_absence stays False.
2026-09-19T22:34:18Z activation-run1 launched PID 6432: python scripts/warehouse_activate.py --db-path data/warehouse.duckdb --memory-limit 6GB --threads 8 --shards 8 --run-id activation-run1; UA='atx-db/0.1 research-bot (+https://github.com/atx)'; log .superpowers/sdd/tier1-parity/activation-run1.log
- 2026-09-19T18:36:41 activation-run1: security_master FAILED 403 from sec.gov with non-email UA (as the ruling anticipated). BLOCKED ON USER: need an ATX_SEC_USER_AGENT with a contact email (SEC fair-access policy). Not embedding the user's email without explicit consent.
- 2026-09-19T18:36:41 activation-run2-prices launched PID 20464: --only ticker_history_extract --only ticker_history_publish (offline). log activation-run2-prices.log
- 2026-09-19T22:40:54Z activation-run2-prices COMPLETED: ticker_history_extract 11,084,562,320 bytes in 81.8 s (sha 96cb7fbd...); ticker_history_publish 31,178,192 rows / 34,803 securities / 12,206 on latest date 2026-06-15, 0 duplicate keys, 0 invalid, 149.5 s. Archive has 31,464,423 lines → ~286K (0.9%) rows collapsed by the publisher's (security_id, trade_date) dedupe (keeps max volume line).

## Process ruling (user, 2026-09-19T23:05:12Z): speed over test ceremony
- Implementers: write code + focused tests once; no RED/GREEN evidence; run only the covering test files once before commit; never the full suite; ruff on touched files only.
- Reviewers: one pass, no test re-runs; re-review only for Critical findings; Important findings fixed by the implementer and accepted on its report.
- Full-suite/CI verification happens once at the sprint gate, not per task.

## Scope ruling (user, 2026-09-19T23:27:13Z): full production DB + core metrics over niche coverage
- S2: keep T3 (seed guards, in flight), T4 (alias mining, in flight, cheap), T5 (engine composition rules), T6+T7 (core IS/BS/CF alias waves — these drive full-universe coverage), T9 (coverage measurement). DEFER T8 (industry templates) and T10 (fixture corpus).
- S3 (derived engine + market daily join + panels): full priority, all 10 tasks.
- S4: keep T1-T2 (universe), T3-T4 (delistings), T6 (quality gates), T7-T8 (publication/release), T9+T11 (data dictionary, docs/CI), T10 (retirement, cheap). DEFER T5 (LEI/FIGI identifier master).
- Critical path for 'entire universe' = SEC bulk download (companyfacts.zip + submissions.zip + company_tickers.json) → blocked on ATX_SEC_USER_AGENT with a contact email.
- 2026-09-19T23:51:40Z S1 GATE CLEAN (feat/tier1-parity through e01ba95d + later S2/S3 commits). Awaiting user OK to merge to main.
- 2026-09-19T20:06:03.3350374-04:00 activation-run3 launched PID 15624 (UA dummy email atx-research@example.com per user); log activation-run3.log
- 2026-09-20T00:08:03Z activation-run3: security_master 10,438 (1.6 s); symbol_directory 13,258; sec_bulk_download companyfacts.zip 1,409,212,553 B sha ee099c73…, submissions.zip 1,564,656,199 B sha 702fbcd8…, 73.5 s. Dummy-email UA accepted by SEC.
- 2026-09-20T00:49:33Z Ruling: run3 process imported seeds before T7/T9 landed (stale allowlist, no 0301). Kill run3 once submissions_load completes; launch run4 --start-stage companyfacts_load (governed migrate applies 0301 first) — cost if wrong: one submissions_load redo (~45 min) if the kill lands mid-stage.
- 2026-09-20T11:15:09Z activation-run4 launched PID 2264: --start-stage submissions_load --run-id activation-run4 (run3 died mid submissions_load leaving a stale 'running' ledger row; sec_submissions loader is DELETE+INSERT so the stage reruns cleanly; governed migrate applies pending 0301 with a pre-migrate backup first). Seeds at aeb9a7e2 (T7 fix) are final for the allowlist. UA dummy email per prior user ruling. log activation-run4.log
- 2026-09-20 Process ruling (shared tree): agents never run git stash/checkout --/reset/restore/clean; commits are pathspec-only; never commit an import or registry line that references another agent's uncommitted module (S3 T9 shipped a bodies_0305 import ahead of S4 T3 → HEAD unimportable on clean checkouts until T3 lands). Snapshot-line bleed remains the only permitted cross-task inconsistency.
- 2026-09-20 ~12:50Z activation-run4: submissions_load COMPLETED 3,045,440 rows / 47,869 CIKs / 985,667 main members in 5,702 s. companyfacts_load now running (seeds at aeb9a7e2 allowlist).
- 2026-09-20 ~13:15Z USER STOP. Handoff: docs/superpowers/handoffs/2026-09-20-tier1-parity-handoff.md + -next-goal.md. In flight at stop: S3 T7, S4 T7, S4 T8, S4 T3 fix1, S4 T4 fix1 (agents may still land pathspec commits). run4 in companyfacts_load.
- 2026-09-20 continuation: user supersedes model choices: Codex models only for all implementation and reviews (including whole-branch review); never invoke Claude for work. No LLM API spend. Existing commit trailer remains as explicitly specified.
- 2026-09-20 continuation baseline HEAD 41ebe820. Reconciliation dispatched to fresh Codex implementers for S3 T7, S4 T7 (exclusive registry lock), S4 T3 fix1. S4 T8 and T4 fix1 queued; controller owns ledgers. activation-run4 PID 22840 remains active in companyfacts_load; DB lock confirmed, file writes advancing. Direct activation ledger inspection waits for writer release.
- 2026-09-20 10:09 local reconciliation gate: committed HEAD5b11a272 independently exported with git archive, import atx_db resolved only from export; tests/test_module_boundaries.py +tests/test_schema_contract_v2.py -n0 -q exited0 (one default slow skip). Log reconcile-head-check.log, verifier verify_committed_head.py. All five handoff owner groups now committed: T7 projection850ec527+5b11a272; T7 schemascbd5488e+43100735; T8 publication9a5c75bd; T3 evidence28dee5a7; T4 delistinga17ab540. Publication review1 Important governance+1 Minor deterministic ordering fix remains underway. No full suite yet.
- Migration reservation for continuation:0309=S3 T8 retirement (next free after308);0310=S4 T10 retirement wave2. Registration edits require serialized ownership and body in same commit. No migration/schema conditions inferred from empty data.
- 2026-09-20 USER MEMORY RULING: protect this16GiB host from exhaustion. Only one heavy workload; no concurrent production/tests/audits. Initial production DuckDB1GB/1thread/reconciliation16sequentialshards(onechildatatime), with an independently reviewed Windows process-tree memory cap and physical/commit headroom checks. Old6GB/8thread/8shard activation settings are superseded. No pagefile/global changes.
- 2026-09-20 run4 TERMINAL: companyfacts_load completed14:59:21UTC (31,590,760facts,8031targets,7035loaded-target outcomes,996failed targets,6533unresolvedCIK candidates); old stage marked complete despite target failures. statement_points failed15:00:49UTC with MemoryError in pandas full-fact concept catalog. Writer22840/parent2264 exited without controller termination; no new production writes. Post-run4 read-only inventory documents missing13857archiveCIKs, absent dated exchange evidence and empty legacy cohort. No coverage conditions may be flipped from these gaps.
- 2026-09-20 catalog memory repair8a95a3ec: SQL aggregation tested over31,590,760livefacts read-only at1GB/1thread,242concepts in26.031s/~1.05GiBprocesspeak. Facade integration and independent review pending; later calendar mapping pandas risk remains scoped in report. Source identity/replacement review fixes must land before offline archive reload.
- Migration reservation0311: AR3 actual annual top3000 cohort/coverage metadata only. Owner must be dispatched by root; registry edits serialized, body must exist in same commit. AR1/AR2 integration addendum records all-forms50CIKbatch/conservative session defaults and sequential reconciliation partition semantics.

- 2026-09-20 user supplied completed updated bulk history in Downloads. Actual formatParquet,32,323,644rows through2026-09-18 byfooter. Staged separate source at data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet; SHA2560ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae. Native reader/fullaudit/republication required; keep oldsource and Downloadsoriginal. Newdata removesobservedJuneenddategap butdoesnotprovehistoricalUSlisting or priceeconomicaccuracy.

- Migration reservation0312: later AR1/AR2 activation integration owner only, for scheduler/catalog metadata needed by new production integration. Include forward-return dataset description correction from0186 (old description says raw close/no imputation; new default is corrected adjusted price plus explicitly named terminal policies). Do not edit historical migration0186 or mix this metadata into AR3's0311. Registry ownership transfers only after AR3commit.

- 2026-09-20 USER production/research priority: finish the complete US-equity fundamentals warehouse (all core metrics, ratios, growth and per-share families) and add custom TickerHistory feature tables with measured forward-return decile spreads. Avoid niche cleanup and repeated test loops. Existing production activation remains the critical path. Codex-only implementation/review, bulk inputs and memory protections continue.
- Custom research CF1 brief: custom-features-production-brief.md. Predeclare eight causal price/liquidity hypotheses, primary horizon20 market sessions, secondary5/60; chronological splits, label purge/embargo, ranks before label attrition, daily-horizon HAC and fixed-family multiple-test correction. Statistical evidence and production eligibility are separate. No guaranteed alpha or significance, and no false certification from unverified listing/adjustment/vintage inputs.
- Migration reservation0313: CF1 feature/research surfaces only. Registry ownership must wait for integration0312 commit. Source data work stays bounded in SQL; Python receives only small aggregate spread series. This task may develop alongside production reloads but must not run heavy tests concurrently.
- CF1 horizon correction BEFORE any forward-outcome evaluation: existing production panel horizons are(1,5,10,21,63), so primary21 and secondary5/63 replace the initial20/5/60 draft; embargo63. Reuse actual calendar labels without changing legacy IC constants or creating another large panel. Missing required horizons must be explicit. This is a source-contract alignment, not performance-driven selection.
- 2026-09-20 production price publication at1GB/1thread failed atCOMMIT;3GiBguard measurednativepeak1.773GiB, hostsafe. Read-only recovery proved original31,178,192bars retained, new31,959,271staging retained, migrations0303..0312applied. Operatorclosedstaleledgers at16:59:42UTC. Recoverycode59dc04e7 independentlyclean. A2GBqueryretry within SAME3GiBprocessguard is authorized by measuredheadroom/overhead; firstpreflightrefused at4.897GiBfree (requires5). Preserve thresholds and usefreshreceipts. If2GBstillfails, make boundedreplacement a task instead of repeatedly increasing budgets.

2026-09-20T17:18Z production ruling: both price publication COMMIT attempts exhausted DuckDB budgets (1GB then2GB) while the process guard protected host memory. No further budget escalation; a fresh Codex bounded-publication implementer is authorized. Live313 applied, original31178192bars retained, failureledger recovered automatically. Root owns production; daily-risk focused tests currently sole heavy workload, then all-forms submissions may proceed independently of the price repair. Preserve all backups/staging evidence; no schema-contract relaxation without reviewed design.

2026-09-20T17:32Z physical-design ruling: reserve0314 to remove only the two nonunique equity_daily_bars and three nonunique equity_price_metrics secondary ART indexes. Preserve primary-key/NOTNULL/default contracts and physical public tables; validate shadows and atomically swap them with relatedmetadata. This changes optional lookup acceleration, notlogicalkeys/qualitythresholds. Rootauthorizes base-schema stoprecreatingtwo barindexes. Price implementer owns0314registry lines; holdschema editsduringactivefreshbootstraptests. Requiredprimaryindexesbuilt inboundedorderedprefixbatches withcheckpoint/reopen, productioncapacitystillmustbemeasured. No extra memory escalation.
