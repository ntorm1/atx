# Latest controller state: full source audit complete, production and CF1

This section supersedes every older status below. Updated 2026-09-21 UTC
(2026-09-20 local). The production snapshot remains explicitly as-of Sep20.

## Current controller override - 2026-09-21 01:09 UTC

Update01:26:57UTC: SAME archive5/session35531 is healthy and now BEYOND the
archive4 interruption. processed6875/20390, loaded5821 (includes5792verified
reuse), empty1019/unavailable35/failed0, newattemptfactrows90,957. Counts include
replacements, not net growth. Latest guard physical5.537GiB/commit5.703GiB.
No active test/agent work, no extra DB access. Continue this exact run; do not
restart it or run another workload. Root's Downloads filename check around
01:19UTC still found only old priceZIP+updatedParquet, no TickerDefinitionHist.

Update01:15:50UTC: archive5 VERIFIED5792targets /28,205,479retainedrows at
01:14:35; both fingerprint scans10,174groups. Actual INFO output lives in
activation-companyfacts-archive5.err (EDT timestamps), stdout.log is currently
empty. Traversal725/20390, loaded638reused, empty83/unavailable4/failed0,
newattemptrows0. No error/stop. Backup20260921-010817.bak12,125,220,864B;
disk free84,994,764,800B. See companyfacts-archive5-resume-evidence.md.

- Archive5 is LIVE, started01:08:14UTC from sourcecd841f26, sole runtime.
  SAME exec session35531; guard child/venv redirector13004, worker16900,
  conhost10572. Both Python creation01:08:14UTC. Guard3GiB, DuckDB1GB/1thread,
  backup-keep100, asof2026-09-20, dummy SEC UA only. Resume source is terminal
  archive4datasetUUID c7c21cd1-c7c4-4de8-af64-4f2877ef6f59. Actual NEW dataset
  UUID not yet observed; do not assume the prior UUID belongs to this attempt.
  Receipts/logs activation-companyfacts-archive5-memory.json/.log/.err.
  Latest01:08:49 guard physical5.668GiB/commit6.058GiB; no stop/error. No
  concurrent tests/imports/lint/type/DB/probes. Await actual terminal receipt.
- FC1 COMMITTED cd841f26:37focused passes first run; one fresh review0/0/0;
  module/schema contracts passed (one existing slow skip). Isolated committed
  HEAD import+all7module tests passed, peak0.6149GiB. Helper+0319strictmypy
  clean;16legacy errors in5wider modules exactly match isolated318baseline.
  One whitespace Ruff issue fixed; zero new scoped lint. Dictionary regenerated
  and --checkpassed, policy linked README/runbook. Full receipts in
  fundamental-clock-root-verification.md. No full-suite gate yet.
- Source0317/318/319 is complete with owned bodies+registry/facade lines and
  exact module pins. Archive5 governed startup applies pending migrations with
  backup; confirm startup/actual applied versions later, never infer success.
  Raw facts/points/receipts unchanged by FC1. LegacyXBRL empty prerequisite
  measured00:50:22 remains valid; no intervening data writes before archive5.
- All implementation/review agents idle, no pending task except production.
  Once archive5 finishes, inspect ACTUALledgers/receipts and any source gaps;
  resume submissionsUUID04cf947d-53bb-49b7-a276-b3c74a2a52c8, then full
  activation-run5 --start-stage statement_points --force --shards16. Full core
  rebuild includes corrected shares. Fix actual stage capacity failures as
  tasks, no speculative scope growth or budget increases. Then CF1evaluate,
  live coverage/quality docs/thresholds/release, full non-slow gate once and
  Codex whole-branch review. Ask before main; stash@{0} untouched.
- Prelaunch disk free97,515,393,024B; warehouse12,125,220,864B. Existing8
  pre-migrate backups preserved (~55GB); no deletion authorized/performed.
  Monitor remaining disk as stages grow. Companion historical listing source
  question still pending. GoalACTIVE; no parity/significant-alpha claim.

## Historical controller override - 2026-09-21 00:57 UTC

- All four capacity tasks integrated, focused checks accepted and pathspec
  committed: FP1/0317 cc6da619 (36passes); AP1/0318 fca77aef (47passes then
  one corrected calendar-oracle case passed); DP1 197195a3 (20passes then
  two corrected NOTNULL/fiscal-span fixtures passed); MP1 1a3cb68b (37passes).
  PR1 remains5dc0aec7 (5passes). All one-review policies satisfied; no
  production arithmetic changed by fixture corrections. Reports+receipts in
  corresponding *-root-verification.md. Final touched Ruff/type passed;
  known baseline-only Ruff findings documented, not claimed clean globally.
- Current HEAD1a3cb68b. Root isolated git-archive import+module/schema check
  COMPLETED session58500, capacity-committed-head-{memory.json,log,err}; import
  from export and all module/schema cases passed (one existing slow skip),
  exit0/peak0.9511GiB. No full-suite gate yet; no production writer.
- FC1 draft COMPLETE, one fresh review active /root/review_fundamental_clock_policy.
  Read fundamental-clock-policy-report.md and fundamental-clock-draft/
  root-integration.md. Root APPLIED patch after capacity HEAD check, while the
  reviewer reads frozen draft;9existing/4new paths plus three root-owned0319
  registry/facade lines and exact private module pin. Focused tests running
  session8107, fundamental-clock-tests.{log,err}/-memory.json. Review pending.
  0319 is existing overlap view/catalog only. Future raw
  SEC readers use shared max(stored,filed+46h); raw tables remain unchanged.
  Corrected shares refresh added inside existing statement_points stage.
- Root read-only inventory00:50:22UTC proved fundamental_xbrl_metric0,
  shares_outstanding_history0, est_actual0, revisions0, statement_points0,
  standardized0, market_daily_metrics0. Thus legacy-XBRL empty prerequisite
  satisfied before run5. Evidence fundamental-clock-legacy-input-inventory.json.
  Do not confuse accidental nonexistent derived_metrics name with count of
  derived_metric_values. No new legacy policy column/gate needed. Shares
  SQL-wide indexed transaction peak remains unmeasured; fix only actual failure.
- Archive4 terminal/recovery details below remain authoritative. Actual resume
  UUID c7c21cd1-c7c4-4de8-af64-4f2877ef6f59. Finish FC1 review/integration/
  focused checks/commit, then archive5 SAME3GiB/1GB/1thread limits and fresh
  receipt. Submissions verified resume follows, then run5 --start-stage
  statement_points --force --shards16; no bulk redownload. Source rows intact.
- Main/release/fullgate still pending. No conditions flipped. Companion input
  question pending; stash@{0} untouched. GoalACTIVE. Actual source/test tree
  only has four preexisting content-empty EOL phantom modifications.

## Historical controller override - 2026-09-21 00:41 UTC

- Archive4 TERMINAL: stopped_low_headroom (commit2.983GiB; physical2.474GiB).
  Receipt mtime00:25:33UTC, terminal collected00:29:18. Session25640 completed;
  worker21904/parent9712 confirmed absent. Do not wait/restart that old session.
- Read-only inspection00:35:56: facts=points42,052,707; prices31,959,271;
  custom31,934,514; schema0316. Actual dataset UUID
  c7c21cd1-c7c4-4de8-af64-4f2877ef6f59. Newly processed2042loaded issuers,
  7,999,850 attemptrows;1010empty/34unavailable/0error. Prior3750loaded
  receipts reused separately; replacement writes are not net growth.
- Both archive4 ledgers now failed at operator recovery00:36:56UTC. Initial
  recovery COMMIT succeeded but CHECKPOINT failed at512MB; root reopened at
  normal production1GB/1thread, verified both ledgers without secondUPDATE,
  checkpoint PASSED00:37:38, peak0.856GiB. Evidence in archive4 inspection,
  recovery scripts/receipts and ledger-recovery-verified.json. Source untouched.
- Host recovered above6GiB physical/commit. PR1 five focused tests+Ruff passed,
  committed5dc0aec7. FP1 patch0317 applied with exact module pin; focused tests
  running session37273 at00:41UTC. Other capacity drafts not yet applied.
- AP1/MP1 one fresh review each CLEAN0/0/0; DP1 Important+Minor fixed/report
  accepted; all pending root runtime. MP1 patch check passed. Integrate0317
  then0318, using owned registry/facade hunks only. DP1 raw patch requires
  core.autocrlf=false; MP1 default true. Never overwrite full shared copies.
- FC1 effective-clock implementation dispatched STATIC DRAFT ONLY, brief
  fundamental-clock-policy-brief.md. Source-proven winter same-day early-use
  counterexample, live prevalence unmeasured. Next-calendar-day22UTC default,
  stored timestamp floor, before revision ordering; close raw PIT bypasses,
  preserve raw tables/CF5 receipts. Reserve0319 if needed. Not exact acceptance
  or historical arrival certification. Fresh review then root tests/integration.
- Finish focused component validation/commits serially; then archive5 resumes
  verified actual archive4 UUID above under same3GiB/1GB/1thread budgets.
  Submissions resume04cf947d-53bb-49b7-a276-b3c74a2a52c8; then full run5 from
  statement_points --force --shards16. No bulk redownload. CF1 evaluation,
  measured coverage/quality, publication, full suite and branch review remain.
  GoalACTIVE; no main merge; stash@{0} untouched; companion question pending.

## Historical controller override - 2026-09-21 00:14 UTC

- Archive4 remains LIVE, confirmed through SAME session25640 and CIM worker21904,
  parent9712, creation1789945403452. Latest log00:13:55UTC: processed6500/20390,
  loaded5528 (includes3750 verified reuse), empty943, unavailable29, failed0,
  attempt fact rows7061230. Attempt writes include replacements, not net growth.
  Latest guard free physical5.085GiB/commit6.071GiB. Headroom briefly fell to
  physical3.643/commit5.016 around23:59UTC but no guard stop occurred. Disk free
  around00:07UTC102866690048B. No source failure, restart, or competing runtime.
- Root committed the audit/three original capacity briefs/ruling as1c6535e8.
  SOURCE IS STILL UNCHANGED by capacity tasks; all are isolated drafts. PR1
  actual CLI/test/runbook edits remain uncommitted and runtime-unverified.
- FP1 DRAFT COMPLETE and ONE FRESH STATIC REVIEW CLEAN (0/0/0):
  forward-publication-capacity-report.md and -review.md. Six-path patch at
  forward-publication-draft/forward-publication.patch. Root git apply --check
  PASSED against current worktree, without applying it. No runtime validation.
  Helper _forward_return_publication plus0317 belongs to this task. Use patch
  hunks for registry/facade; never overwrite them with full review copies.
  Root module pin and focused tests still pending after writer terminal.
- DP1 ONE REVIEW COMPLETE:0Critical/1Important/1Minor. Important repeated
  fact-level ID UNION per default one-ID page; Minor settings expectation
  captured only after baseline reopen. Both FIXED in draft. Root read fix1
  report and verified source/test hashes, accepting fixes for integration on
  implementer's report; NO extra review needed. Runtime acceptance pending.
  Files: derived-connection-capacity-{report,review,fix1-report}.md.
  Fixed source hash EDCF0286D42F8A09DBAD999F02517E14E99D21EE860532124D02029D34560B10;
  test hash C13E81E1C5176E2ADE0DC576CBDF3C01D8EDD49CB90BF3592DBDE1D479270CC3.
  Live basis remains F953220EA651DD4F60BB1E205046CEE2966F89F1AAD9582412BD805E20978729.
  One UUID-owned ID-only snapshot now feeds bounded keyset pages; explicit
  contextlib.closing ensures cleanup on refresh failure. New tests13cases.
  Agent is packaging a two-path patch only; accepted Python artifacts must stay
  unchanged. Check the fix1 report for final patch path before integration.
- AP1 agent fundamentals_publication_capacity_implementation still assembling
  draft/report/tests for8early output tables. Uses one sorted unindexed output
  stage, then bounded ordered prefix/keyset shadow batches; removes only27
  optional secondary indexes in reserved0318. Coupled standardized/exception/
  completed-build-ledger swap stays atomic. Existing single-table helper body
  remains text-identical; no FP1/DP1 files changed. Review not started yet.
- MP1 is a FOURTH, disjoint capacity task, fresh agent
  market_connection_capacity_implementation. Read market-connection-capacity-
  brief.md. Draft market-connection-draft/; report same prefix. Owns ONLY
  market_daily.py and one focused test file, no migration. Root found existing
  SQL/security batches but no recycle across the whole daily table publication.
  Preserve every calculation/date/source/atomic batch contract; consume scalar
  count then checkpoint/reopen after each successful batch. No runtime or
  production failure is claimed. Draft and its one review still pending.
- ALL capacity tasks now share CF5 eligibility: recycle only persistent files
  with recorded analytical memory/thread settings. Unconfigured/in-memory
  callers keep their session; no implicit adoption/snapshot of a few settings.
  Refuse incompatible caller temp state before mutation where recycling applies.
  All agents static-only; root alone runs imports/tests/type/lint/DB/probes.
- Continue healthy source pass, then handle actual ledgers, submissions resume,
  reviewed capacity integration/focused checks before run5, full downstream
  ladder, CF1 evaluation, measured coverage/quality/release and sprint gates.
  Order0317 before0318; root owns exact module pins. No re-review for accepted
  Important fixes. No memory increases or output reductions. Goal ACTIVE.
  Historical companion question remains pending; stash@{0} remains untouched.

## Current controller override - 2026-09-20 23:56 UTC

- Prior goal turn was PROGRESS plus VERIFIED WAIT: PR1 implementation/review
  artifacts committed2bacc787 and session25640 confirmed live. This turn also
  verified the SAME session, child9712, worker21904/creation1789945403452.
  Latest23:55:12UTC log: processed5925/20390, loaded5033 (includes3750 reuse),
  empty864, unavailable28, failed0, attempt fact rows5200080. These are attempt
  writes including replacements, not net warehouse growth. Latest guard physical
  free5.703GiB/commit7.024GiB. Disk free at23:53UTC102973145088B. No failure.
- Healthy archive4 remains the SOLE runtime workload at1GB/one thread/3GiB job
  cap. Do not run tests, imports, probes, type/lint checks or any DB connection
  until it ends. Poll the actual existing handle/process; silence is not failure.
- A fresh Codex STATIC audit of the imminent fundamentals stages completed:
  fundamentals-activation-memory-audit.md,0Critical/2Important capacity findings.
  Early full-universe indexed transactions and derived connection lifetime remain
  unbounded even though calculations use SQL/per-security bounds. This is source
  evidence plus prior actual price/CF5 COMMIT failures, NOT a claimed run5 failure.
  The16 reconciliation partitions do not bound earlier publication transactions.
- Root also confirmed forward labels use one transaction for five full-universe
  horizon inserts into a PK plus2secondary-index table. With31.96M price rows,
  output may approach160M labels. No forward-stage scale failure has occurred.
  Prepare bounded physical publication while the source pass runs; preserve all
  outputs, scopes, required keys and atomicity, without increasing memory.
- THREE FRESH CODEX IMPLEMENTERS are active, all DRAFT-ONLY/static:
  1. forward_publication_capacity_implementation: FP1 brief/report prefix
     forward-publication-capacity; draft forward-publication-draft/. Owns forward
     label publication and reserved0317 removal of ONLY its2optional indexes.
     Must preserve _bulk_publication's existing single-table API.
  2. derived_connection_capacity_implementation: DP1 brief/report prefix
     derived-connection-capacity; draft derived-connection-draft/. Owns only
     derived_metrics.py plus focused tests; no migration. Keyset traversal and
    10-committed-security recycle cadence, after owned PIT cleanup. Root agreed
     with CF5 eligibility: persistent stores with recorded analytical settings
     recycle; unconfigured callers retain their existing settings/behavior.
  3. fundamentals_publication_capacity_implementation: AP1 brief/report prefix
     fundamentals-publication-capacity; draft fundamentals-publication-draft/.
     Owns8early fundamental output publications and reserved0318 optional-index
     policy/bootstrap changes. May add narrow coupled-swap helper while keeping
     existing single-table API unchanged. Preserve standardized+exceptions+
     completed-build-ledger atomicity. Existing routing/seeding-before and
     calendar-coverage-after boundaries remain unchanged.
- Agents may NOT edit live source, run any runtime, commit, or spawn subagents.
  Drafts are not applied or reviewed/tested yet. Root must serialize0317 then
 0318 registry integration with their own bodies/facade lines, inspect actual
  patches and update exact module pins. One fresh independent review per task;
  Important fixes on implementer report, re-review only Critical. All root runtime
  verification waits for writer terminal. No arithmetic/PIT redesign or global
  atomicity weakening is authorized by these physical capacity tasks.
- PR1 remains stable/reviewed but UNCOMMITTED and runtime checks pending in
  actual cli.py, tests/test_publication_resources.py and root runbook edits.
  Its brief/report/review are committed2bacc787. Preserve unrelated user files,
 4EOL-only phantom modifications and stash@{0}. Historical companion-data
  async question still has no answer; do not repeat it or infer a source.
- Remaining production order: handle archive4 terminal/actual ledgers, complete
  reviewed capacity fixes and focused checks as needed before downstream build,
  submissions verified resume, full activation-run5 from statement_points with
  --force/16sequential reconciliation shards, CF1 evaluation, actual quality/
  coverage/release, full non-slow once, fresh whole-branch review and ask before
  main. Submissions can proceed before draft integration if that keeps ingestion
  moving; do not overlap it with tests. Goal ACTIVE. No production parity claim.

## Current controller override - 2026-09-20 23:39 UTC

- Archive4 is still the sole live runtime workload. Session25640 returned the
  same running child9712; CIM confirms worker21904, parent9712, creation
  1789945403452. Latest log at23:38:45UTC: processed5300/20390, loaded4459,
  empty813, unavailable28, failed0, attempt fact rows3070959. Loaded includes
  3750 verified reused issuers; attempt rows include replacements, not net growth.
  Last guard headroom: physical5.528GiB, commit6.908GiB. Worker at23:39UTC:
  working1308864512B/private1868636160B. No guard or source failure occurred.
  Follow-up at23:40:28UTC: processed5375, loaded4526, empty821, unavailable28,
  failed0, attempt rows3301987. Same live session; guard physical6.217GiB and
  commit6.861GiB. These newer counters supersede5300 above.
- PR1 source is stable but UNCOMMITTED and runtime verification is PENDING.
  Fresh Codex implementer owns cli.py and new test_publication_resources.py;
  publication-resource-report.md records the implementation. One fresh Codex
  static review is COMPLETE: publication-resource-review.md, 0 Critical,
  0 Important, 0 Minor. No further review is required unless Critical fixes arise.
  --memory-limit defaults1GB and --threads defaults1, applied at publication
  boundary using the existing helper. The new test checks actual CLI defaults,
  explicit512MB/two-thread override and configuration replay after reopen.
- Root owns the UNCOMMITTED PRODUCTION_RUNBOOK.md edits: correct the already
  implemented terminal/calendar/survivorship ladder description and show explicit
  publication1GB/one-thread options plus the outer process-guard requirement.
  Scoped git diff --check passed. This is static validation only, not pytest,
  Ruff, mypy or any import/DB probe. No runtime work competed with archive4.
- When archive4 releases the runtime slot, collect its terminal receipt and
  inspect actual ledgers/outcome. Then run PR1's five focused cases from its
  report (two resource cases, pending-migration refusal, missing/legacy variants)
  under the root guard, Ruff the touched Python paths, and commit the task with
  exact pathspecs including its reports and root runbook. Keep unrelated EOL
  phantom modifications and user files out. Do not commit PR1 as tested yet.
- The historical companion-data async question remains pending. No answer or
  new file is assumed, no question was repeated, and ingestion continues.
  Remaining production order is submissions verified resume, activation-run5
  from statement_points with --force and16 sequential shards, CF1 evaluation,
  live coverage/quality and first release, then full non-slow suite once and
  fresh Codex whole-branch review. Ask before merging main; preserve stash@{0}.
  Goal remains ACTIVE, with healthy production progress and no readiness claim.

## Current controller override - 2026-09-20 23:26 UTC

- Archive4 confirmed LIVE via session25640 around23:24UTC, same worker21904 /
  parent9712 / creation1789945403452. At23:24:19UTC processed4750,loaded3984,
  empty739,unavailable27,failed0,newattemptfactrows1131235. These include issuer
  replacements and are not net warehousegrowth. Headroom physical~6.22GiB,
  commit~8.00GiB. No source/guardfailure. SAME one-runtime policy/limits apply.
- Fresh Codex static research COMPLETE: historical-vendor-source-check.md.
  Official TickerDefinitionHist has datedID/MIC/symbolType; v7backfill lacksCIK,
  primaryExch,issueClass; Equity is distinctfrompreferred/ADR/ETF but not
  expresslydefined common-only. Root independentlyopenedprimarydictionary/enum.
  Historicalidentity/PITcoverage stillrequiresrealdata; noadapter/sourcechange,
  download, account, contact, purchase orDBwork was done. This is distinctfrom
  earlierSEC FSNresearch; it identifiesamoredirectvendorcompanion, not AR5closure.
- Root refreshed filteredDownloads metadata: stillonlyoldtickerZIPandupdated
  TickerHistory3.parquet; norelevantcompanionfilename. Asyncuserquestion SENT
  asking whetheranexistingUS TickerDefinitionHist historicalexportisavailable
  anditslocalpath/sample. Noanswerreceivedyet; donotassumepermission/presence,
  askagain, orpauseindependentsource/metricsworkwhilewaiting. No credentials
  requested. Do not manufactureanallhistoryfilename/S3keyorbackdate22CTdelivery.
- Remainingpipelineunchanged: lethealthyarchive4finish, verifyledgers/outcome
  insoleguard, submissionsverifiedresume, fullrun5, CF1eval, measuredcoverage/
  quality/release, fullnon-slowonce, Codexwholebranchreview, askbeforemain.
  Ifcompanionarrives, inspectactualformat/size/columns/coverageinassignedroot
  runtimeslotaftercurrentwriter;onlythen sizeadapter/identitytasks. GoalACTIVE.

## Current controller override - 2026-09-20 23:17 UTC

- Archive4 CONFIRMED LIVE this turn via session25640 and CIM worker21904,
  parent9712, creation1789945403452. Session poll returned the SAME live handle
  around23:16:44UTC. No restart, failure or headroom stop occurred.
- At23:16:41UTC the loader passed the old archive3 failure point: processed4500
  of20390, loaded3758 (includes3750 verified reuse), empty716, unavailable26,
  failed0, newly processed factrows34438. These are log counters/attempt writes,
  not net warehouse growth or completed source/quality coverage. The missing
  payload-CIK errors have not reappeared through the old processed prefix.
- Latestguard physicalfree~7.33GiB/commit~9.07GiB. Keep1GB/one thread/3GiBcap.
  Continue this healthy source pass; no competing runtime, database connection,
  test or probe. Check actualsession/log/process before resuming any future turn.
- Previous goal turn classified PROGRESS (reviewed code+commits and live proof).
  This turn also gained production evidence: safe proof/reuse proceeded beyond
  the old COMMIT failure into new issuer writes. Goal remains ACTIVE. Remaining
  order and all source UUIDs are in23:11/23:05 overrides and production-resume-
  after-cf5.md. No full-universe release or significant-signal claim yet.

## Current controller override - 2026-09-20 23:11 UTC

- Archive4 remains sole ROOT runtime workload, session25640, guard child9712,
  actual worker21904, creation23:03:23UTC. SAME source/limits/command as23:05
  override below. No tests/DB/probes/additional runtime while active.
- PROOF PASSED at23:09:00UTC: targets3750 / retainedrows20205629. Fact and point
  fingerprint groups both8759. Startedproof23:04:17UTC; factscanended23:06:43,
  pointscanended23:08:58, candidate recoveryverifiedby23:09:00. About283sec total.
  Logs activation-companyfacts-archive4.err contain exact times (localEDT,
  add4hforUTC); stdoutfinalstageJSON not yet present. Archive targets20390.
- At23:09:52UTC processed550,loaded490,empty57,unavailable3,failed0,newrows0;
  this is the verified-skip prefix, not a newly loaded20.2M row claim. Continue
  watching actual laterlog tail. Previousarchive3 failed around4489receipts;
  archive4 will replayempty/unavailable, retry5missingCIKerrors withnewhandler,
  and loadremainingtail. Do not stop a healthy pass to run competing probes.
- Process memory after proof recyle at23:10UTC: working684240896B/private
  1159008256B; host physicalfree~7.6GiB/commit~9.2GiB. Nativepeakfinalpending.
  FreeC103347466240B. Preservednewbackup warehouse.duckdb.pre-migrate.20260920-
  230330.bak is11529629696B; hashfromgovernedregistrytoinspectafterterminal,
  no costlyredundant hash duringactivewriter. Live schema expected316from
  governed startup but directpost-runmeasurement remainspending.
- HEAD a54841bf beforethisdocscheckpoint. Sourceunchangedfromc33b5ccd;
  annual-cf-committed-head-verification.md recordsimport/module/schemapasses.
  BothCF5/AF1 reviewed/fixed/focused-tested/committed; dictionary201/API2.1.
- Next afterterminal: verifyledgers/outcome insoleguard, handleactualfailure,
  submissionsverifiedresume, fullrun5, CF1eval, livecoverage/quality/release,
  fullnon-slowsuiteonce, Codexwholebranchreview, askbeforemain. GoalACTIVE,
  significant-signal/full-universe-readiness claims stillunmade. Preserve
  stash@{0}, unrelateduserfiles and4content-emptyEOLmodifications.

## Current controller override - 2026-09-20 23:05 UTC

- SOLE live workload: activation-companyfacts-archive4, tool session25640,
  guard child9712 / actual worker21904, started23:03:23UTC. Project Python,
  DuckDB1 GB/one thread within3 GiB guard; dummy SEC UA. Do not launch any other
  tests, typechecks, DB connections, probes or writers while it runs.
- At23:04:17UTC it reached verified resume after governed startup; 3750 loaded
  receipts, one failed-run ancestor, count inventory done. At23:04:19UTC it
  began retained fact SHA256 aggregation. Proof has NOT completed; no skipped
  issuer or full-source completion claimed. Logs activation-companyfacts-archive4
  .err/.log/-memory.json. Check actual latest output before deciding next action.
- Source HEAD c33b5ccd (docs/snapshot), following AF1 244af575 and CF5 b9572b27.
  Isolated244af import/schema tests passed(one existing slow skip); sole module
  failure was two names before root snapshot commit. Isolatedc33b import and
  all7modulechecks passed, peak0.614365GiB. Source/schema test diff between them
  is empty. Report annual-cf-committed-head-verification.md explains exact order.
- DATA_DICTIONARY regenerated201definitions/API2.1 and corrected event-vs-
  arithmetic clock prose; source/test changes are committed. Four unrelated
  content-empty EOL changes remain, plus user's other-session files/stash@{0}.
- After archive4 terminal, inspect both ledgers and measured outcomes using
  the sole guarded slot; handle actual failures as tasks. Then submissions
  verifiedresume04cf947d-53bb-49b7-a276-b3c74a2a52c8, full activation-run5,
  CF1labels/evaluation, live coverage/quality, eligible release, full non-slow
  once, Codex whole-branch review and ask before main. Goal ACTIVE. No claims
  of live metric coverage or statistically significant signals yet.

## Current controller override - 2026-09-20 22:59 UTC

- No production writer or runtime check currently active. CF5 COMMITTED
  b9572b27; AF1/0316 COMMITTED 244af575 in separate pathspec-only task commits.
  Both independent reviews had 0 Critical / 2 Important, all four findings
  fixed and accepted on implementer reports plus root focused execution.
- AF1 initial 79 cases covered after three fixture/seam fixes; fix3 six targeted
  cases passed, peak 0.789284 GiB. Final strict mypy passed four AF1 files after
  mechanical COUNT-result narrowing; touched Ruff passed. CF5 53 post-review
  cases passed, peak 0.703762 GiB. No second independent reviews performed.
- Root added exact _companyfacts_resume / _derived_annual snapshot names and
  relocated existing _derived_pit next to them. Regenerated DATA_DICTIONARY.md
  from current 201 definitions/API 2.1; pure generation passed under 1.5 GiB
  guard, peak 0.570744 GiB. Root docs truth updates pending their own commit.
- Next: commit root snapshot/dictionary/docs, then isolated committed-HEAD
  import + module boundaries + schema contract via verify_committed_head.py
  under 2.5 GiB guard. Check actual completion before starting another workload.
  Then launch exact companyfacts archive4 verified resume prepared in
  production-resume-after-cf5.md (UUID758f7d5c-73ae-4b66-a8c9-f1996afa163a).
- Live still schema0314 and archive3 retained 38,500,008 facts=points. The CLI
  will govern0315/0316 with a backup and 1 GB/one-thread migration settings.
  Keep production3 GiB guard, one writer/workload, all sources/backups, no RAM
  escalation. Then submissions resume, full run5, CF1 evaluation, measured
  coverage/quality/release, full non-slow once, Codex whole-branch review, ask
  before main. Preserve stash@{0} and unrelated/phantom-EOL work. Goal ACTIVE.

## Current controller override - 2026-09-20 22:51 UTC

- No production writer or runtime check currently active. Archive3's terminal
  failure and 38,500,008 retained facts/points remain the latest live evidence.
- CF5 COMMITTED b9572b27 in its own 13-path task commit. One independent review
  found 0 Critical / 2 Important; both fixed and accepted on the implementer's
  fix1 report and root execution. All 53 post-review focused cases passed,
  peak job memory 0.703762 GiB; touched Ruff and new-module strict mypy passed.
  Reports: companyfacts-archive3-repair-review.md, -fix1-report.md,
  companyfacts-archive3-root-verification.md. No further review required.
- AF1 remains applied/uncommitted in its 11 paths. Strict mypy passed after
  fix1. Initial six-file run 76/79 passed; fix2 corrected forbidden NULL source
  fixtures and the four-argument frame instrumentation wrapper. All three
  affected selectors now passed, peak 0.790688 GiB. Production schema unchanged.
- Fresh AF1 review /root/review_annual_fallback is finishing its single report:
  two concrete Important issues found (unrelated annual span vetoes valid
  quarterly metric; annual weighted-share branch retains quarterly coherence),
  no Critical so far. Await final report, dispatch original AF1 implementer,
  focused root execution, accept fixed Important issues on report. No speculative
  rereview or full suite before the actual gate.
- Root-only runtime rule continues. Production-resume-after-cf5.md prepares the
  exact source UUIDs and order; those commands have NOT run. Finish AF1, commit
  its migration with its source, pin exact new helper snapshot names, regenerate
  dictionary, check committed HEAD, then companyfacts archive4 verified resume,
  submissions verified resume, full activation-run5, CF1 evaluation, measured
  live coverage/quality and first eligible release. Keep 1 GB/one thread within
  the 3 GiB production guard; no global memory changes or other-app termination.
- Goal ACTIVE; user scope includes a production fundamentals warehouse and
  statistically tested custom signals, not just source ingestion. Preserve
  stash@{0}, unrelated files and four content-empty EOL changes. Ask before main.

## Current controller override - 2026-09-20 22:29 UTC

- No production writer. Archive3 terminal failure/recovery in22:07 override below
  remains authoritative: live38500008facts=points,prices/CF1retained,schema0314;
  failedUUID758f7d5c-73ae-4b66-a8c9-f1996afa163a. Do not restart untilCF5 and
  all pending applied production code are tested/reviewed/committed.
- P1/0315 COMMITTED5519d1ac; Core201 COMMITTED0c52fdbd. Root five-file focused
  run65pass/1Corefixturefailure, guardpeak0.922688GiB. Core corrected wrong
  expected TTMwindow0..3 at targetindex4 to1..4; singleaffectedcasePASSED,
  peak0.751171GiB. P1 allcases passed incl realpopulated314->315/invalidations.
  P1 Ruff+newmodule strictmypyclean; rootverificationreportrecordsacceptance.
- IsolatedHEAD0c52 import and schema tests passed(oneoldslowskip); solemodule
  failure was exactmissing `_derived_pit` snapshotname. Rootpinnedonlythatname
  in3ce0db14, then isolatedHEADmodulefile7casesPASSED/importresolvedexport,
  peak0.614235GiB. No schema/sourcecode changedbetweenchecks. CurrentHEAD3ce0db14
  before thischeckpoint. Reports/logs pit-core-head-0c52fdbd and
  pit-module-head-fix1; defaultfull headhelper now accepts --module-only.
- AF1 draftrefreshedagainst0c52,12baseline/evidencehashes+patchSHAverified byroot;
  APPLIED toactualtree. PatchSHAf3711931e29b5d95c50c8cc85d24f8ab34a803b77c0c826d32d8418dde97319d.
  Own11paths in annual-fallback-implementation-report.md, including0316registry,
  new _derived_annual, annualtest and P1/paneltestexpectations. NoCFoverlap.
  Rootfixed3mechanicallintissues; allAF/CFtouchedRuffotherwiseclean.
  Combinedstrictmypyfailed12errorsONLYannualhelper; AFimplementerfixedactual
  Spanarguments/narrowedNumber AST, reports no valid-formula behaviorchange.
  annual-fallback-fix1-report.md; actualhelperSHA4c81e0c469442716d0b7cc70bad51a3f511750d58475c3346198ca48dfb27dae.
  Rootmypyretry+AFfocusedtests remainQUEUED. NoAFindependentreviewyet.
  Draftpatch is historicalinitialdelivery; actualtree nowauthoritative withfixes.
- CF5 implementationCOMPLETEactualtree: fundamentals.py,_companyfacts_resume.py,
  activation.py,newtest_companyfacts_archive_repair.py/newtest_companyfacts_activation_resume.py.
  Agent applied itsactivationpatch afterP1release; DO NOT REAPPLYpatchartifact.
  Tencommittedissueroperations/reopen,proof-phasereopen,atomicfacts/points/
  candidate/receipt,verifiedfailed-run ancestryresumewithSHA256multisets,
  candidate recovery,missingpayloadCIK anddummyUAplumbing. Report
  companyfacts-archive3-repair-report.md. CF5Ruff/strictmypyclean.
- SOLE ROOT workload is CF5nine-file focusedpytest -n0/guard2.5GiB withdummyUA,
  toolsession48505,guardchild11292,actualworker18920(creation1789943239473).
  At22:29:33UTC actualCIMcommandmatched and19caseshadpassed.
  Logs companyfacts-archive3-repair-focused1.*.
  Check actual result; not yet passed. No concurrentmypy/AFtests/DB/probes.
  Fresh staticCF5 review /root/review_companyfacts_archive3_repair underway in
  parallel; CF5source frozen. Runtime/reviewfixes follow reported evidence.
- AfterCFtestfinish: rootmypyAFretry, AFfocusedfivefiles; meaningfulactualfixes
  only, thenonefreshAFreview. CFreviewImportantfixesacceptedonreport;Critical
  fixesrereviewonly. CommitCF5pathsseparately, AF1paths+0316 together; neverbleed
  other'sregistry/source. Rootmustaddexactnewprivatehelper snapshotnames after
  respectivecodecommits, regenerateDATA_DICTIONARY/publicschemaartifacts for316,
  andperformrelevantfinalHEADchecks beforeproductionresume.
- ThenCF5verifiedresume from758f7d5c-73ae-4b66-a8c9-f1996afa163a at1GB/1thread/
  guard3GiB (measureproof/load and fixrealcapacityissues, noRAMescalation),
  submissionsverifiedresume04cf947d-53bb-49b7-a276-b3c74a2a52c8,fullrun5,
  CF1labels/eval/livequalitycoverage/release/fullnon-slowsuiteonce/Codexwholebranch
  review/askbeforemainmerge. Preserve stash@{0}, unrelatedphantomEOLM andotherwork.
- Turnclassification PROGRESS: sourcefailuremeasured, P1/Corevalidatedandcommitted,
  committedHEADchecked, AF1appliedandCF5runtime/reviewbegun. GoalremainsACTIVE.

## Current controller override - 2026-09-20 22:07 UTC

- Archive3 is TERMINAL FAILED at22:02:44UTC; worker9412 gone and session6840
  completedexit1. Fatal COMMIT hit953.6MiB/953.6MiB DuckDB budget, guardpeak
  1.884941GiB, no host-memory guard stop. Do not continue old wait/poll/restart.
  Both DB ledgers already failed with proper clocks; no manual ledger repair.
- Root guarded read-only inspection22:04:46UTC found38500008facts=points,
  31959271prices,31934514CF1rows retained. Attempt20205629facts/3750loadedCIKs;
  receipts3750loaded+708empty+26unavailable+5errors=4489, lastloaded0001033905.
  DatasetUUID758f7d5c-73ae-4b66-a8c9-f1996afa163a, schema0314.
  companyfacts-archive3-failure-report.md/inspection.json and failed-members.json
  are authoritative evidence. Personal configured contact in params was redacted
  from generated evidence; no archive member HTTP occurred.
- Five errors are missing payloadCIK despite valid exact archive names/facts
  objects. CF5 /root/companyfacts_archive3_repair owns fundamentals.py+private
  helper/focusedCFtests per companyfacts-archive3-repair-brief.md. It will repair
  bounded connection lifetime, verified resume including unfinished candidates,
  missingCIK source handling and explicit dummyUA forwarding. Activation changes
  must be a separate patch until P1commit; no actual activation edits by CF5 yet.
- Sole ROOT runtime workload now: focused P1+Core five-file pytest -n0 under
  project .venv guard2.5GiB, toolsession27496, guardchild11480. Exact logfile
  prefix is in derived-pit-active-check.txt. No competing probes/tests/writers.
  Review fixes I1/M1 included. Inspect actual result; do not claim passed yet.
- AF1 /root/annual_fallback_implementation continues isolated draft source/tests
  and0316/API2.1 proposal. No actualP1/Core edits. Root validates/commits P1/0315
  FIRST, Core second, then reconciles draft baseline/patch and CF5activationpatch.
  One independent review per newtask; Criticalfixes only rereview.
- HEAD1fba7641 before this checkpoint. Goal ACTIVE with production repair in
  progress. ResumeCF5 only after tests/review, submissions verifiedresume still
  queued, fullrun5/CF1eval/livequalitycoverage/release/gates/mergeapproval remain.
  Preserve other-session work and stash@{0}. Turn classification PROGRESS:
  authoritative terminal diagnosis, retained-data measurement, CF5implementation
  dispatched and queued P1/Core tests running.

## Current controller override - 2026-09-20 21:53 UTC

- Actual archive3 worker9412 remains LIVE with the same creation time and advancing
  CPU. At21:52:37UTC:4100/20390 processed,3401loaded,668empty,26unavailable,
  5failed,18624608 written attempt rows (not net additions or coverage). Fifth
  failure arose3700..3724. Inspect ALL source error receipts after writer release;
  individual errors are not emitted to stderr. No competing DB/tests/probes.
  Same guard3GiB, DuckDB1GB/1thread; host physicalfree6.44GiB/commitfree8.61GiB;
  private bytes1.965GB, diskfree107.89GiB. Do not restart healthy ingestion.
- Fresh static path audit COMPLETE: annual-filer-metric-path-report.md confirms
  standardized annual flows cannot reach core TTM metrics without complete
  quarterly evidence. Annual-only instants can work, but margins/flow per-share/
  market denominators cannot. This violates the design's fiscal-year fallback;
  no live affected-issuer count measured. Root read and accepted the path finding.
- AF1 fresh Codex implementer /root/annual_fallback_implementation is preparing
  a concrete ISOLATED draft per annual-fallback-implementation-brief.md, including
  baseline copies/hashes, exact integration.patch and focused fixtures. All draft
  files stay under annual-fallback-draft/; no actual atx-db/ edits or tests/DB.
  Reserve0316 in draft only if needed. This keeps P1/Core frozen until validated.
- P1 and Core201 remain uncommitted, with one static review each. I1/M1 tests are
  prepared, not run. Once source releases: inspect/fix real source errors, root
  guarded focused P1/Core/schema/boundary/import/bootstrap checks; P1/0315 commit
  FIRST, Core second. Then apply/rebase AF1, root focused checks, one fresh review,
  commit. Do not silently mix AF1 into previously reviewed P1 source. Regenerate
  dictionary/snapshots for final stable schema before production activation.
- Verified submissions resume, full run5, CF1 labels/evaluation, actual quality/
  coverage, first release and sprint gates remain. Goal ACTIVE, no merge approval;
  preserve stash@{0} and all unrelated files. Current HEADd25a9c7c before checkpoint.
- Turn classification: PROGRESS (confirmed core gap and isolated implementation
  dispatched) plus verified wait on active source. Slow ingestion is not blocked.

## Current controller override — 2026-09-20 21:20 UTC

- Verified LIVE worker9412 by CIM command/runID/creation time and advancing CPU,
  not just a status file. Parent2332/session6840 still archive3. Base interpreter
  process path is the Windows venv redirector's Python312 child; the guard
  receipt/parent command remains the locked project .venv Python. At21:19:08UTC:
  2750/20390 processed,2285loaded,436empty,25unavailable,FOURFAILED,
 13046177 written attempt rows. Third failure arose2625..2649, fourth2675..2699;
  exact details await source receipts after writer release. Memory safe (~1.94GB
  private,7.16GiB physicalfree,9.16GiBcommitfree). Same3GiBguard/DB1GB/1thread.
  Let full source finish; no concurrent DB/tests/probes or speculative restart.
- P1 noncritical review fix preparation COMPLETE: derived-pit-revision-fix1-report.md
  records real populated0314->0315 two-row upgrade/PK/nullability/legacy/API/
  reconstructed-coverage/pin/re-entry assertions and direct invalid daily/factor,
  API first_reported/stub-date cases. Only focusedtest/mainreport/fixreport changed;
  no implementation defect, no runtime checks, no new review needed. Prepared
  coverage gaps addressed on implementer report; final acceptance still requires
  ROOT'S EXECUTION. All implementation/review agents now idle/completed.
- Core201/P1 source remains uncommitted. Root queued focused guard2.5GiB test
  command is in P1 report and includes all added I1/M1 cases; add core breadth
  focused file once source stable. Required schema/bootstrap/module/import and
  relevant dictionary/snapshot integration checks remain. Then pathspec P1/0315
  commit FIRST, core second; preserve clean HEAD against original173 at P1commit.
- Last turn classification: PROGRESS (PITreview gaps now prepared) plus verified
  wait on the actual live source process. Active source is slow, not blocked.
  Current HEADc8211964 before this checkpoint; goal remains ACTIVE. Preserve
  unrelated four EOLphantomM files and stash@{0}; no mergerpermission.

## Current controller override — 2026-09-20 21:11 UTC

- Source archive3 still active: session6840,launcher2332,worker9412; same
  guard3GiB/DB1GB/1thread. At21:09:59UTC:2325/20390 processed,1947loaded,
  353empty,23unavailable,2failed,11462790 written attempt rows. Host physical
  free6.47GiB/commitfree8.58GiB at21:10. Keep full pass running. The two
  member failures are not logged individually; inspect all error receipts
  after writer release, then repair actual failures. No competing DB/tests.
- P1 implementation COMPLETE FOR REVIEW, source uncommitted and frozen.
  Exact18-path inventory, work bounds, consumer contracts and root command in
  derived-pit-revision-report.md. ONE static review COMPLETE at21:13UTC:
  derived-pit-revision-review.md closes original Critical at source level only,
  finds no newCritical,1ImportantI1(populated0314->0315 upgrade case) and
  MinorM1(invalid daily/factor plus API first_reported/stub-range assertions).
  /root/derived_pit_revision_repair is now preparing those tests and
  derived-pit-revision-fix1-report.md. Accept on report/root execution, no
  additional independent review for these noncritical test-preparation fixes.
  No runtime verification or production reconstruction has occurred.
  It has no dependency on core201 CSV/test helpers; commit P1/0315 before
  core breadth after root verification so committed HEAD stays coherent.
- P1 evolves canonical nullable event states under0315, updates daily/raw/DEI,
  API/export/factor/publication/coverage semantics. Atomic scope is one source/
  security plus resolved prerequisite/descendant closure. Defaults:250k input
  rows/security,250k metric candidate bound,1024keys/chunk,8192frame rows,
 100knew/old publication rows,<=500identifier metadata block. These are row
  bounds, not measured capacity. Large issuers must fail honestly, not skip.
  Review must check original Critical closure and this bounded design.
- Core breadth28additions+4reclaimedcodes+newtest remains UNCOMMITTED, clean
  static review; tests queued behind source AND stableP1. No runtime checks
  this continuation. P1 report now pins .venv Python both guard/child,2.5GiB,
  freshGUIDreceipt/logs,-n0. Root must also run required bootstrap/schema/import
  checks and regenerate dictionary/snapshots when relevant source stabilizes.
- HEADa3ac9379 before this checkpoint. Preserve unrelated four bootstrap EOL
  phantomM files and other-session work. No goal completion/release/fullsuite/
  wholebranchreview/mergepermission. Verified submissions resume and run5,
  CF1labels/eval, actualquality/coverage/publication and approval gate remain.

## Current controller override — 2026-09-20 20:58 UTC

- Sole live writer archive3/session6840/launcher2332/worker9412 continues.
  At20:57:21UTC:1825/20390 processed,1552loaded,254empty,17unavailable,
  TWOFAILED,9444918 written attempt rows. Failure1 appeared in1650..1674,
  failure2 in1775..1799. Exact errors await final result/source receipts after
  writer release; per-target errors are not emitted to stderr. No fatal DB
  failure/guard stop, CPUadvancing, workerprivate~1.93GB, physicalfree~6.35GiB.
  Guard3GiB/DB1GB/1thread unchanged. Do not interrupt full source pass for
  diagnostics, run competing tests/probes, or increase RAM. Inspect/fix actual
  failures after it finishes; completion cannot be claimed with failed targets.
- HEADfe0c61c5 committed program ruling, core report/review and prior queue.
  Core breadth SOURCE STILL UNCOMMITTED awaiting root focused runtime checks.
  One static reviewCLEAN. Implementer subsequently aligned focused assertions
  with0315: latest state ORDER BY available_at,derived_value_id; explicit NULL
  missing_input_or_domain/events, no invalid numeric result. Accept those seam
  assertions on implementer report; no new Critical/review loop. Own paths:
  CSVderived_metric_definitions,4entriesderived_registry,test_core_metric_breadth.
- P1 implementer /root/derived_pit_revision_repair remains ACTIVE, source edits
  under exclusive scope in brief; includes0315/registry and minimal activation
  derived-coverage count/provider_coverage closures. No P1 runtime/review yet.
  It must finish the bounded event-state/consumer implementation, then one
  fresh independent review assessing Critical audit closure. Tests remain root
  exclusive after writer and stable source. Source loader untouched by P1.
- No goal completion, release, fullsuite, wholebranchreview or mergerpermission.
  Keep current jobs working; finish source failure repair, verified submissions
  resume UUID04cf947d-53bb-49b7-a276-b3c74a2a52c8 (onlysubmissions/no download),
  testedreviewedcodecommits, run5fromstatement_points --force, CF1labels/eval,
  actual coverage/quality/publication and gates. Remind stash@{0} before merge.

## Current controller override — 2026-09-20 20:54 UTC

- Companyfacts archive3 STILL ACTIVE under the same session6840/worker9412,
  launcher2332, guard3GiB/DB1GB/1thread. At20:53:19UTC:1675 processed,
  1432loaded,229empty,13unavailable,ONEFAILED,8875863 written attempt rows.
  First failure arose between progress1650 and1675. Loader records source/
  normalization errors in DB/source receipts and final result, not per-member
  stderr, so exact target/error cannot yet be inspected without the writer's
  connection. Do not interrupt the healthy full run or open competing DB/probes.
  After completion, inspect ALL source failures (final summary truncates50),
  repair actual failures as tasks and reconcile successful coverage; do not
  rerun the entire healthy archive solely to obtain this one error sooner.
- Core breadth implementation READY and independent static review CLEAN,
  core-metric-breadth-report.md / -review.md. Adds28 CSV rows plus4dead-code
  reclamation entries in derived_registry.py; source total201, original173
  preserved. New test_core_metric_breadth.py is queued after source writer
  and stable0315. Implementer then aligned assertions to explicit NULL PIT
  states on report; no extra review requested for that seam. NO TESTS run,
  NO SOURCE COMMIT yet. Root uses locked .venv Python both guard and child,
 2.5GiB/-n0; no bare systemPython. Registry set entries owned by core only.
- P1 /root/derived_pit_revision_repair is implementing critical historical
  revision preservation under brief/audit. Owns0315 registry/migration and
  derived/market/PIT consumer sources. Root additionally authorized exact
  derived-coverage count predicate in activation.py and provider_coverage:
  explicit invalid states must not fabricate finite valid metric coverage.
  No concurrent registry/jobs/activation editor. No P1 runtime/review yet.
- Root docs checkpointdfa7fbc9 records Critical evidence/brief; subsequent
  program ruling is edited but not committed. Goal ACTIVE. Keep source running;
  finish verified submissions resume, guarded focused/PIT verification and
  commits, then run5 fromstatement_points --force, CF1labels/eval, measurements,
  release and gate/merge approval. Stash@{0} still must be mentioned to user.

## Current controller override — 2026-09-20 20:42 UTC

- Sole active DB writer remains activation-companyfacts-archive3, session6840,
  launcher2332/worker9412, same guard3GiB/DuckDB1GB/1thread. At20:41:19UTC:
  1250/20390 members processed,1082 loaded,158 empty,10 source unavailable,
  zero failed,7045704 attempt rows. CPU/logs advance, physical free ~6.4GiB;
  disk111.28GiB free at20:38. LET THIS HEALTHY FULL ARCHIVE RUN FINISH.
  Root alone runs DB/tests/probes, none concurrently. No RAM escalation.
- Static production-metric-surface-report.md confirms173 catalog definitions
  (143quarterly-grid/30daily) and material ordinary quarterly growth/efficiency
  gaps. Fresh Codex /root/core_metric_breadth owns CSV/newtest/report under
  core-metric-breadth-brief.md:18quarterly YoY/QoQ outputs,5CAGRs,basicEPS,
  DSO/DIO/DPO/CCC. No migration needed: activation reseeds catalog. No runtime
  validation yet; root tests after active writer, one independent review.
- Concrete PIT bug found statically: derived_metrics selects latest-only
  standardized revisions and replaces historical metric rows; market_daily
  independently selects latest-only standardized/DEI revisions. Max-input
  clocks cannot restore lost earlier states. Audit COMPLETE with a Critical
  numeric amendment counterexample and bounded event/state design in
  derived-pit-revision-audit.md. Fresh Codex /root/derived_pit_revision_repair
  implements derived-pit-revision-brief.md: required writer/daily/export/PIT
  consumer closure, nullable invalidation states, stable IDs, event-local frames.
  Owns derived/market/consumer source and migration0315/registry lock; source
  loaders and concurrent breadth CSV off limits. No runtime checks yet. Fresh
  review must assess original Critical closure; rereview only new Critical fixes.
  This repair is REQUIRED before full derived/market activation.
- Completed prior work unchanged: corrected price publication and CF1 build.
  No labels/significance yet. After companyfacts, verified submissions resume
  uses priorUUID04cf947d-53bb-49b7-a276-b3c74a2a52c8, --only submissions_load,
  no archive download; then tested/reviewed PIT+core changes and run5 from
  statement_points --force. Measurements, release, gate suite, whole-branch
  review and USER APPROVAL before merge still required; stash@{0} reminder.

## Current controller override — 2026-09-20 20:06 UTC

- ACTIVE soleDBwriter activation-companyfacts-archive3, tool session6840,
  launcher2332/worker9412. Started20:03:30UTC, exactsamefull20390archive scope,
  --only companyfacts_load --companyfacts-symbol-source archive_members
  --companyfacts-replace-existing --memory-limit1GB --threads1 --backup-keep100
  --force --run-id activation-companyfacts-archive3, dummyUA, guard3GiB.
  Logs/err/memory named activation-companyfacts-archive3*. At20:06:05 had75
  processed/64loaded/10empty/1unavailable/0failed,429320committedattemptrows.
  Workerprivate1.35GB at20:06:22, CPUadvancing, disk119.75GBfree. Inventory6601
  issuers/spellings built0.771sec. No error. LETHEALTHYRUNFINISH; noDB/tests/probes
  concurrently, no further RAM increase or speculative source redesign.
- Exactarchiveplaceholder task COMMITTED1ce0a850,56rootfocusedcasespassed
  (newempty_members,zip,resilience,cik_spellings) guardpeak0.687GiB, independent
  reviewCLEAN, Ruffpass. Sourceunavailable is separatefromloaded/completed/failed,
  preservesoldfinancialdata/candidates, exactarchiveb'{}'only; malformedandHTTP
  emptyobject stillfail. Warning/outcomes explicit, no coveragegate change.
- Priorrootrecovery/placeholder/predicate evidence +docs committedabd3efc3.
  No activeagentimplementation. Allnewsourcechangescommitted; onlyknownunrelated
  migrationEOLphantomMfiles andother-sessionuntrackedworkremainuntouched.
- Aftercompanyfacts finishes: inspectstage/dataset/sourcecounts underguard;
  performverifiedsubmissions resume b14f4c1e withpriorUUID04cf947d-53bb-49b7-a276-b3c74a2a52c8
  (--onlysubmissions_load, no archive download stage), thenactivation-run5
  fromstatement_points --force overfullsources. CF1buildalreadycomplete,
  forwardlabels/eval stillpending; fullmeasurements/release/gates/mergeapproval
  andstash@{0}reminder remain. GoalACTIVE, notcomplete.

## Current controller override — 2026-09-20 19:57 UTC

- NO active DB writer. Root stopped owned archive2 at19:47:59 (checkedPID7628
  command/runID beforeStop-Process); supervisor closed session37220/wholejob.
  Not memoryfailure: nativepeak1.737GiB. Read-onlyrecovery19:48:43 found987977
  committedattemptrows/146CIKs,19empty,1error. Totalfacts=points31837696;
  prices31959271/customfeatures31934514 unchanged. Bothstaleledgersclosedfailed
  at19:53:40UTC, explicitrecoverytime. DatasetUUIDc22823af-2c18-48c9-9fc3-3bba1ae58d8f.
- FailedmemberCIK0000003521.json is exact2-byteb'{}'. Rootdirectory/payload
  inventory19:53:46 found62exactemptyobjects of20390,zeroother<=2byte members,
  zero retainedfacts for all62CIKs. Sourceabsencehandling is independenttask,
  notfinancialcoverage or thresholdrelaxation.
- Raw CIK throughput optimization COMMITTED4ff0e9c2,54focusedcasespassed,
  guardpeak0.697GiB, independentreviewCLEAN. Keepsone distinctrawspellinginventory
  perload, exactboundIN twice, Nonefallback androllback/cachefreshness. No point
  filterchange. Rootoneissuerread-onlypredicateprobe matched11550keys/checksum:
  numeric1.084sec/exact0.006sec, sequentialwarmcacheonly, nofullspeedupclaim.
- /root/companyfacts_empty_archive_members ownsfundamentals.py,new
  tests/test_companyfacts_empty_members.py pluspreciseexistingziptest updates.
  Acceptedbrief: archive-onlyexactrawb'{}' unavailable outcome/counter/source
  metadata+coveragewarning, preservepriorfacts/points/candidates, doNOTcountloaded
  orclearolddata, network/nonemptyinvalidpayloadsremainfailed. Noactivationchange
  needed. Implementation/testprep underway, no runtimeyet. Freshreviewthenroot
  focusedtests thenexactpathcommit andactivation-companyfacts-archive3retry at
  same1GB1thread/3GiBguard. Don'trerun expensiveearliertestswithoutchangedconcern.
- CF1build alreadycomplete; labels/evaluationstillpending. Verifiedsubmissions
  resume b14f4c1e notrun yet; aftercompanyfacts useonlysubmissions_load withprior
  UUID04cf947d-53bb-49b7-a276-b3c74a2a52c8, nocachedownloadstage. Thenrun5from
  statement_points --force. Fullmeasurements/release/gates/mergeapproval remain.

## Current controller override — 2026-09-20 19:40 UTC

- ACTIVE sole writer activation-companyfacts-archive2, tool session37220,
  launcher7628/worker11236, started19:39:07UTC with20390archive targets,
  --only companyfacts_load --companyfacts-symbol-source archive_members
  --companyfacts-replace-existing --memory-limit1GB --threads1 --backup-keep100
  --force --run-id activation-companyfacts-archive2, guard3GiB. Samecorrectvenv
  and dummyUA. Log/err/memory filenames activation-companyfacts-archive2*.
  No completed25-target progress yet at19:39:51, noerror,CPU/WAL advancing.
  Do not openDB/run tests whilethiswriter active; don'tinterrupthealthywork.
- Source query repair COMMITTEDa5004bf5 withreviewCLEAN,38focusedtests passed,
  guard2.5GiBpeak0.689GiB. Preciseissuer tempkeys/directDELETE,transactional.
- Verified submissions resume COMMITTEDb14f4c1e,54focusedcasespassedincluding
  eightImportantfixcases. BothImportantaccepted, noCritical. Productionresume
  not yetrun. Aftercompanyfactsfinished use freshguard/runid --onlysubmissions_load
  --submissions-resume-from-run-id04cf947d-53bb-49b7-a276-b3c74a2a52c8 --force,
  fullforms/history/defaultbatch50, samearchive; do NOTrerundownloadstage.
- CF1 build SUCCESS (31,934,514rows/34,224warehouseIDs,16,338,033cohortrows),
  all8features measuredfinite/no nonnullnonfinite,zero measuredtimeorder/clockerrors.
  Fullguardpeak1.767GiB. Read-onlyinspectionpeak0.552GiB. Labels/evaluationsstill0.
  Evidence/docs committedff46cff7. No need rebuildfeatures absentnewsource/code.
- No activeagent codework. Bothimplementers/reviewercompleted. No new migration.
  After sourcesfinish activation-run5 fromstatement_points --force stillrequired;
  CF1 evaluation afterforwardlabels, livecoverage/provider/quality,release,gates,
  mergerpermission andstashreminder remain. FullsuiteNOTyetexecuted.

## Current controller override — 2026-09-20 19:32 UTC

- Sole active DB writer is custom-features-build1, tool session17955, guard
  launcher19648 /worker2448, 1GB1thread16sequentialpartitions/3GiBguard. Uses
  corrected full price source SHA0ed96b...; no forward labels/evaluation yet.
  Started19:27; no error, CPU advancing, ~1.87GB worker private at19:32,
  spill~1.65GB, host physical~5.37GiB. No other DB/tests/probes during it.
  Guard/log/err names custom-features-build1*. Do not interrupt healthy work.
- After it exits: inspect success/failure, then root run exactly8 new Important
  resume fix cases `test_sec_submissions_resume.py -k 'scope_complete or resume_plan'`.
  Earlier35resume+bulk +11activationpassed. Resume independent review had2Important
  noCritical: forms/history completeness and forceddownload destroyingreceipt.
  Both fixed (preflight rejects resume plan with sec_bulk_download before sideeffects),
  Ruff passed, accept on implementer report after8cases, exactpathcommit.
- Companyfacts query repair source/newtests ready; fresh independent reviewer
  review_companyfacts_issuer_cleanup active. Root focused command AFTERfeature:
  test_companyfacts_issuer_cleanup.py test_companyfacts_zip.py test_fundamentals_spine_link.py,
  -n0 guarded2.5GiB. Reduced-schema250k unrelatedkeys under64MB meaningful bounded
  cleanup case, plus real-schema existingtests. No tests run yet. FixImportant
  acceptedreport; Criticalrereview. Then exactpathcommit +productionretry1GB1thread.
- Measurement task COMMITTEDb420f174, docs/failureauditCOMMITTEDbfff3325.
  Source resumer/cleanup still uncommitted. No schema/registry changes for either.

## Current controller override — 2026-09-20 19:26 UTC

- No active writer. Full companyfacts archive attempt activation-companyfacts-archive1
  FAILED at first issuer `_replace_facts` correlated DELETE, 68.375sec, DuckDB
  953.5/953.6MiB OOM; guard peak1.838GiB, no host exhaustion. Both ledgers correctly
  failed. Root recovery19:24:24 confirms facts and points31,590,760 unchanged,
  prices31,959,271 intact. Fresh query repair needed, no RAM escalation.
  Auditor fundamentals_runtime_audit is writing source-path brief; earlier
  downstream audit did not cover this. Launch2 receipts are actual stage;
  first launch was an argument quoting error before DB access.
- Measurement fix succeeded18.94sec/peak1.530GiB; warehouse metadata unchanged.
  All downstream standardized/derived/market/risk/forward/CF1 surfaces empty.
  Current script + full report receipts owned by measurement_runtime_fix,
  commit pending. Fixed temp_directory/externalaccess order and explicitUTC.
- Submissions resume implemented, uncommitted: sec_submissions.py, activation.py,
  new tests/test_sec_submissions_resume.py. Root passed35resume+bulk cases and
  11activation cases, guarded peaks0.675/0.678GiB. Fresh independent static
  review pending before exactpathcommit/live use. Explicit datasetUUIDresume,
  archivehash/scope/lineage plus source-to-stored prefix keys proof; maxCIK alone
  is not trusted. Resumeowner stable source while review pending.

## Current controller override — 2026-09-20 19:19 UTC

- Source-prepass2 STOPPED by low-host-headroom guard: physical1.096GiB,
  commit2.921GiB. No live writer remains. Completed read-only recovery found
  8,504,213 committed attempt rows /37,700CIKs, maxCIK0000933249;
  total10,297,910rows /72,810CIKs /668forms. Not net additions or completeness.
  Exact archive SHA still matches receipt. Companyfacts never started.
- Exactly stage and dataset UUID04cf947d-53bb-49b7-a276-b3c74a2a52c8 stale
  ledgers closed failed at operator recovery19:16:54UTC. Initial host-timezone
  cast was corrected explicitly with its own preserved receipt; timestamps
  now naiveUTC. No source data removed. All guard/inspection/recovery receipts
  named source-prepass2-*; scripts preserve predicates and assertions.
- Root lifecycle tests completed11passed, peak0.670GiB; implementer now finalizes
  report/commit. Fresh submissions_verified_resume agent DESIGNING proof-bound
  resume: maxCIK alone is insufficient. Wait for previous owner's edit lock.
- Root read-only measurement FAILED before query execution: DuckDB1.5.5 refuses
  temp_directory assignment together with enable_external_access=false in
  connection config. Fix setting order (temp config first, disable access before
  queries) and explicitUTC; rerun fresh receipt before commit. No measurement
  outputs/certification were produced. Production-measurement-prepass2.err.
- Core source companyfacts remains pending; prioritize that full archive build
  independently while submissions resume is made reviewable. One heavy job only.

## Current controller override — 2026-09-20 19:06 UTC

- Source-prepass2 remains active in submissions; no completed-stage count or
  error. Session 44646, launcher 14440, worker 20680. At 19:05:48 UTC worker
  private bytes were 2,583,109,632, physical headroom about 4.77 GiB. Memory has
  fluctuated down and up; no leak or inevitable OOM was established. It retains
  the original 1 GB / one-thread configuration and 3 GiB process guard.
- Root performed ONE additional lightweight NON-DB inspection at 19:04:53 UTC:
  ZIP directory only, no payloads or source transformations. Separate 1 GiB
  guard succeeded in ~7 seconds, native peak 0.770 GiB, initial physical 4.89 GiB.
  submissions.zip has 985,667 main CIK members, 5,374 history members, one other;
  compressed 1,564,656,199 bytes, declared expansion 5,741,005,528 bytes. This is
  archive scope, not progress, unique issuers, or US-equity coverage. Receipts
  submissions-archive-inventory.json/-memory.json; helper inventory_submissions_archive.py.
- The submissions lifecycle/progress change is READY and statically reviewed:
  submissions-bounded-connection-review.md has no Critical/Important findings.
  UNCOMMITTED source/tests/report await root's guarded test file run AFTER the
  active writer exits; then followup_task its implementer to commit exact paths.
  Active process cannot pick up the edited module. No imports/tests/DB activity
  were performed by implementer/reviewer.
- The measurement script/report/review also remain uncommitted, statically clean,
  awaiting root live verification. After source completion/failure inspection:
  run guarded production-measurement-prepass2 outputs with the new script, and
  the focused submissions tests serially; commit those tasks, then run5 or fix
  an actual source failure. Keep all other standing resource/merge/stash rules.

## Current controller override — 2026-09-20 18:50 UTC

- ACTIVE source-prepass2 remains in submissions, with no completed stage or
  reported error yet. Session 44646; guard launcher 14440, worker 20680. At
  18:49 UTC worker private memory was 2,442,285,056 bytes; physical headroom
  about 5 GiB. CPU and committed WAL activity advance. Do not interrupt healthy
  work or open the live DB/tests concurrently. Same 1 GB / one thread, guard 3 GiB.
- A fresh Codex implementer /root/submissions_bounded_connection is preparing
  a small PREVENTIVE connection-lifecycle/progress change in sec_submissions.py
  and its narrow bulk tests. It is not a measured stage-failure fix and cannot
  affect the already-imported active process. Recycle after committed batches
  only for real file-backed stores with recorded memory/thread caps and no
  noninternal temporary/registered relations; use existing reopen behavior.
  It needs one static review and root tests after the writer exits, then an
  exact-path commit. No registry/jobs/activation or schema edits are authorized.
- Measurement script and report remain uncommitted pending root live validation.
  Static review is clean; its one redaction wording caveat was fixed. Root should
  use fresh production-measurement-prepass2 outputs after the live source pass.
- SEC historical listing research is COMPLETE and committed with precise ID
  wording in d61bd7d9. Actual FSN TXT example includes TradingSymbol/exchange/class
  title. Official 2012-onward archives total 24,698.78 MB compressed, but pre-2019
  tuple coverage, continuous listings and vendor instrument matching remain
  unproved. No archive downloaded. Preserve the bounded future-ingestion brief;
  prioritize core warehouse measurements before dispatching an adapter.
- Root checked for an already-installed py-spy executable; none was found and
  nothing was installed or profiled. No new tests, DB probes or source downloads
  have run concurrently with the production writer.

## Current controller override — 2026-09-20 18:23 UTC

- Source prepass2 is still healthy in tool session 44646. Launcher 14440 owns
  worker 20680. Submissions have not emitted their completed-stage count yet;
  do not query the live DB or start tests while this writer is active. At 18:22
  UTC the worker private memory was 2,091,466,752 bytes, physical headroom about
  4 GiB. Guard remains 3 GiB, DuckDB 1 GB / one thread. Disk about 123 GB free.
- Daily-risk code is committed in 666d13e3; final report 826670f4. All six focused
  cases and shared API snapshot passed. Price success evidence/runbook committed
  in 09d1fe03; static fundamentals audit committed in d3cd9593.
- The audit found no remaining whole-fact Python materialization in the main
  fundamentals stages. Revisions and standardization retain large indexed
  transactions; repair only from actual guarded failure evidence. Derived
  metrics already use 500-security transactions (older suspicion was incorrect).
- New scripts/measure_tier1_readiness.py and production-measurement-report.md are
  UNCOMMITTED pending root live verification. Static independent review found
  no Critical/Important issues; production-measurement-review.md records it.
  Nonblocking redaction wording is fixed. No agent has executed DB/tests.
- After prepass: root can run the new read-only measurement under guard using
  fresh production-measurement-prepass2 JSON/Markdown/receipt filenames; runtime
  has not yet been verified. Then commit exact script/report/review paths.
  Run activation-run5 from statement_points --force after any real source repair.
- Correct price counts are warehouse security IDs, not certified distinct
  issuers/common stocks. Current Downloads have no named TickerDefinitionHist,
  ReturnFactorsHist, OptionCorpActionRecordHist or DividendCalendarHist companion.
  Fresh Codex researcher is checking one official SEC bulk text-data alternative
  for historical symbol/exchange evidence; no downloads or DB work authorized.

## Current live override — 2026-09-20 18:04 UTC

- Price publication SUCCEEDED at 18:00:09 UTC (session 72916 closed, exit 0).
  Root read-only receipt at 18:02:30 confirms 31,959,271 rows / 34,251 vendor IDs,
  max date September 18, latest breadth 12,386, zero invalid adjusted closes,
  zero nonnull split-only factors, no retained staging, migrations through 0314.
  Native peak 1.779 GiB with DuckDB 1 GB / one thread and 3 GiB guard. Receipts:
  activation-prices-updated-bounded-memory.json and updated-price-live-success*.
- Daily-risk final single parity test PASSED, native peak 0.693 GiB; receipts
  daily-risk-parity-squared*. Owner is updating the report and committing its
  exact paths. Six daily cases and shared API snapshot now passed. Review closed.
- ACTIVE ROOT JOB: activation-source-prepass2, tool session 44646, child 14440.
  All-form submissions in batches of 50, archive_members full companyfacts
  replacement, dummy SEC contact, 1 GB / one thread, 3 GiB guard, backups kept.
  No concurrent DB/tests. Logs and receipt use activation-source-prepass2 prefix.
- Next: inspect source results, then activation-run5 from statement_points with
  --force, followed by custom-feature live build/evaluation and measured gates.
  Disk free 123,158,454,272 bytes at 18:02 UTC. Do not delete preserved backups.
- Root owns all DB/probe/test execution using absolute project .venv Python
  (DuckDB 1.5.5). Agents may only code/review/static-check and exact-path commit.

## Current live override — 2026-09-20 17:57 UTC

- Price repair COMMITTED80966fa7. Independent review closed architecture blocker;
  root guarded checks67unique passed/1defaultslow skip. New private helper is
  explicitly included in API snapshot. Dictionary regenerated/--check passed,
  no content change. No full suite has run.
- Root removed ONLY the inspected failed staging at17:52:44UTC after verifying
  old live31,178,192 rows/maxJune15 and preserved raw source. Receipts
  price-staging-cleanup-after2g.json/-memory.json, peak0.116GiB; backups untouched.
- LIVE ROOT JOB: activation-prices-updated-bounded, session72916, launcher15488,
  guard3GiB, DuckDB1GB/one thread, backup-keep100, explicit2026-09-20 and updated
  Parquet. Governed314 applies before source. Logs/receipt share that run name.
  Preflight physical5.799GiB/commit9.393GiB. Do not open live DB concurrently.
- Daily-risk uncommitted:5selected cases passed plus the shared snapshot case.
  Its one parity assertion first needed semantic DATE normalization, then exposed
  ~1.46e-9 near-zero idiosyncratic-vol cancellation noise. Agent changed ONLY that
  assertion to squared-vol comparison rtol1e-10/atol3e-18; other tolerances/clocks
  unchanged. Ready for root's ONE failing-case rerun after live price closes.
  Files/receipts daily-risk-focused-final* and daily-risk-parity-final* retain
  failures. No DB execution by agents; original Critical rereview is closed.
- After price success: bounded live receipt via measure_price_publication.py
  --output updated-price-live-success.json; one daily parity case and task commit;
  then all-forms submissions + archive_members full companyfacts replacement,
  followed by activation-run5 --start-stage statement_points --force and CF1 live
  build/evaluation. Root retains shared registry/jobs/activation ownership now.
- Current disk free111,230,443,520B at17:56UTC. Preserve all backups; monitor disk
  as source/fact/feature tables grow. Stash@{0} and other projects untouched.

## Current controller override — 2026-09-20 17:46 UTC

Root now executes **all database work and tests**. Agents write/review code only.
Always use C:/atx/atx-db/.venv/Scripts/python.exe for guard and child; this is
locked DuckDB1.5.5. Bare system Python is1.5.1 and invalid for these receipts.

- Root's guarded isolated git-archive HEAD cde8c6ae bootstrap PASSED through313,
  including verify_schema, peak0.822GiB. The bootstrap agent withdrew every own
  0280/_runner/test hunk; no historical migration or checksum change remains.
  The earlier system-runtime failures and unguarded probes are documentary only.
- Independent atomic-publication-review.md closes the daily-risk Critical at
  code-design level and finds no price/helper/0314 blockers. Three Important
  daily fixes are in progress: exact physical column order, unique propagated
  clock alias, and valid collision-test setup. Accept fixes on report; no more
  review required unless a new Critical issue arises.
- Root currently owns the only heavy workload: guarded focused price/helper/
  recovery/module-boundary/schema-v2 tests, tool session11204, launcher12004,
  cap2.5GiB. Files atomic-publication-focused1-memory.json/.log/.err. No live DB.
- Price agent owns0314/schema two-index removal/registry/helper/bulk publisher;
  no DB launches. Daily agent owns metrics/activation/narrow tests. Both await
  root test results before separate pathspec-only commits.
- Live remains313 and ORIGINAL31,178,192 prices throughJune15; failed new staging
 31,959,271 remains. Both real price attempts failed COMMIT at1GB then2GB safely.
  After code/tests: inspect and remove only failed regenerable staging (preserve
  all backups/source), governed314 with backup-keep100, price retry at1GB under
  guard, all-form/archive source prepass, activation-run5, CF1 live work, gates.

LATEST17:38UTC IMPORTANTENV CORRECTION: root measuredproject
C:/atx/atx-db/.venv/Scripts/python.exe DuckDB1.5.5 (uv.lock), barepython SYSTEM1.5.1.
Daily-riskfailedfixture andpricefirsttinyprobes usedWRONGsystempython; discard
theirclaimsaboutproductionbootstrap/DDL untilverifiedinlockedprojectruntime.
BootstrapagentALSOomittedguardonits90srun (nowexited, noactiveprocess); rootcaught
it, no furtherunguardedDBwork authorized. Itreached0290thenfailedin1.5.1,
NOTevidencefor1.5.5defect. Its0280/_runner/testhunksUNCOMMITTED andONHOLD;
no0290edits. Nextguardedisolatedgit-archiveHEADfreshbootstrapusingproject1.5.5;
ifpasses, removeONLYagentownunneededpatches viaapply_patch, norestore/reset.
PriceagentNOWownssoleDBslot foroneproject1.5.5tinyprobe: secondaryindexrename,
PRIMARY-onlyswap, dependentviewcommit/rollback. Thenbootstrapbaseline next.
0314/schemaedits stillheld; designconditionaloncorrectruntimeprobe. Allfuture
guardANDchildcommandsMUST useABSOLUTEproject.venvpython, actual1GB/1threadconfig,
guardmax3GiB,-n0. No installedpackagesorenvchangesneeded. Rootdisclosedmistake.
Daily-riskpersistentstage/PKshadowintegrationcoded,256orderedSHAprefixbatches,
checkpoint/reopenevery16batches; needsfreshCriticalrereview andprojecttests.
Correctproject1.5.5probeNOWPASSED: secondaryARTblocksrename; PRIMARY-onlyswap
works; dependentviewseesnewaftercommit/oldafterrollback. BootstrapNOWownsDBslot
forisolatedHEAD1.5.5freshinit;0314/schemaeditsholdRELEASEDbecauseexportisolated.
Knownunmeasurednextstageindexrisks(controllerstaticread): forward-returnwriter
holdsall5horizonsinoneDELETE/INSERTtransaction, targetPRIMARY+2secondaryindexes;
market_daily_metricsPRIMARY+lookup at~32Mrows; derived_metric_valuesPRIMARY+
lookup. Do not assumeSQLaloneprovesfull-scalememory. Keepguardandhandleactual
run5failureasboundedtask; sharedshadowstrategycanbereusedafterpriceproof.

LATEST17:32UTC DESIGN RULING: priceagent tinyprobe DuckDB1.5.1 confirmssecondary
ARTindexespreventtableRENAME, butPRIMARYKEY-onlyshadowatomicrename/dropWORKS.
Rootapproved0314RESERVED/exclusive /root/price_publication_bounded: removeONLY
2bar+3riskNONUNIQUEsecondaryindexes, stop2barindexrecreationinbase schema.py,
preservePK/NOTNULL/defaults/publicphysicaltables. No table->viewcontractrewrite.
Agentowns _bulk_publication.py publish_validated_shadow(store,live_table,
shadow_table,before_swap callback) forshortatomicmetadata+tableswap; caller
buildscompletepersistentvalidatedshadow includingother-source rows. Dailyagent
integratesrequiredmetricPKviaorderedkey-prefixchunks/checkpoint/reopenbefore
swap, preservingfullpeerSQLformulas. Rootrequestedexacttablecontractvalidation
andpublic-viewreadbehaviorprobe; nextDBslotpriceprobeafterbootstraptests.
CurrentsoleDBslot /root/fresh_schema_bootstrap_fix. Itrepairs0280dependency
drop/restoreindexes+view and EXACThistoricalchecksumcompatibility12896754736509aad8be7b2bbab4777cf1bf59ade4b286e8a52e3a8bfb8e03aa
in _runner existingallowlist; testsnewfreshbootstrap andrejectarbitrarydrift.
0314/schema/registryedits HELDwhilefreshbootstraptest runs; helpercodefree.
Rootindependentbootstrapreviewinprogress, Importantchecksumfindingfixedonreport.

LATEST17:25UTC: daily-risk review has1Critical (samefull-tableindexedpublication
memoryrisk) and2Important (252dhighwindow, ineffectiveavailabilitycutofftest).
Scoped-peerfindingwithdrawn: existingfilteredcalculation-universecontractpreserved.
Importantsemanticfixescoded; Criticalawaits sharedpublicationdesignbypriceagent.
Daily-riskfirst4focusedtests failedBEFOREtestbodies innewcachedschema bootstrap:
migration0280ALTERdependentfundamental_extension_concept_map; nativepeak.919GiB.
Fresh /root/fresh_schema_bootstrap_fix owns0280body+narrowtests, codeonly.
/root/price_publication_bounded NOWownssoleDBslot fortinycatalog/indexprobe;
daily-risk/bootstrap noDBoperations. No livewriter currently. Rootwill sequence
necessaryfocusedrepairsbeforelongsourceprepass; nofullsuiteorrepeatedbroadtests.

LATEST17:18UTC: price2GB retry FAILED at COMMIT17:16:23UTC after389.734s.
Nativepeak2.707733GiB, host safe, tool83469 CLOSED. Recoverycode recordedfailed
activationledger automatically. Root read-only receiptupdated-price-live-measurement.json
at17:16:50 confirms ORIGINAL31,178,192bars/34,803IDs throughJune15, replacement
staging31,959,271retained, migrationsTHROUGH0313 nowLIVE. Newpreserved backup
warehouse.duckdb.pre-migrate.20260920-170932.bak8,054,386,688B SHA846b4d7e...79.
No further memory escalation. Fresh /root/price_publication_bounded owns
ticker_history_bulk.py +ownfocusedtests, implementing boundedatomicreplacement
fromprice-publication-bounded-brief.md; noDBslot/registry/activationownership.
/root/daily_risk_bounded NOWowns soleDBtestslot forfocusedchecks; reviewer
/root/review_daily_risk_bounded independent codeonly. Rootwill launch allforms
submissions prepass afterthat shorttestslot, whilepublisherrepair codes.
Sourceprepass/archivecompanyfacts/run5/CF1live/release/fullsuite remainpending.

LATEST17:10UTC: root launched activation-prices-updated-2g under unchanged
3GiB process guard, 2GB DuckDB/one thread, source updated Parquet, backup-keep100.
Tool session83469, launcher20004. Fresh receipt
activation-prices-updated-2g-attempt3-memory.json; .log/.err share run ID.
Preflight physical7.353GiB/commit11.256GiB. ONE heavy slot root-owned.
Two earlier2GB attempts safely refused: -2g-memory.json and -2g-retry-memory.json.
Independent source prepass activation-source-prepass FAILED before ingestion:
migration313 contract rejected leftover uncatalogued equity_daily_bars_bulk_next;
governed migration restored backup automatically. No source stages ran, still312.
Preserve extra10,229,919,744B backup warehouse.duckdb.pre-migrate.20260920-170328.bak.
Root inspected then removed ONLY failed regenerable staging at17:05:55UTC;
receipt price-staging-cleanup.json/-memory.json. Original prices/rawsource/backups
preserved. Current retry can apply313 without that catalog drift.
Fresh /root/daily_risk_bounded owns equity_price_metrics.py, own tests and narrow
activation.py stage integration (brief daily-risk-bounded-brief.md); no tests
until root grants heavy slot. No registry ownership; jobs edits serialized.
This closes known full-pandas32Mbar writer and missing daily-risk activation.
Next successful prices -> all-forms/archive_members source-prepass2 -> run5 from
statement_points --force -> CF1 live build/eval -> measured release and gates.

LATEST17:01UTC: CF1COMMITTED6b7d2f65/0313registered,15focused/module/schema
checks passed, independentab65da79CLEAN. Adjustedreaders18c00da1/42passed,
Importantclockfixaccepted. Failure-recovery59dc04e7/4passed(.595GiB), controller
reviewCLEANa9efe279. ALLagentlocks/DBslotsreleased; rootownsproduction.
Root recorded originalfailedstage+dataset at16:59:42UTC (receipts committed
ina9efe279). DATA_DICTIONARY regenerated and --check passed, no contentdiff.
The first2GBquery/3GiBguard RETRY WAS REFUSED beforechildlaunch: physicalfree
4.897GiB < required5GiB. Receiptactivation-prices-updated-2g-memory.json.
No logs/child/migration313 fromthisrefusal. Wait/recheckheadroom, preserve2GiB
reserve. Do not silentlyweaken guard. Nextreceipt mustbefresh; .log/.err still
unused. Intendedrunidactivation-prices-updated-2g, sameargumentsasfailed1GB
but--memory-limit2GB; --backup-keep100. Stillonly0312LIVE/oldpricesretained.

LATEST at16:52UTC: price attempt TERMINATED exit1, safe native peak1.773GiB.
DuckDB1GB budget exhausted at publication COMMIT; rollback invalidated its
connection and cleanup/failed-ledger writes also failed. Not a host crash.
Root read-only recovery receipt price-recovery-readonly.json proves migrations
0303..0312 applied, ORIGINAL31,178,192bars/34,803IDs throughJune15 retained,
new persistent staging31,959,271rows retained. New source validation passed:
34,251IDs/latest12,386 onSep18. Source not published. Tool5584 is CLOSED.
Stage/dataset ledger remain stale running until root runs prepared
record_price_failure.py (NOT YET RUN), targeted to this exact failed run.
Latest backup lookup in recovery receipt alone used a wrong table name;
correct table is migration_backup_registry. Four backups preserved, new6.3GB.

NEXT OPERATOR DECISION: retry price stage at2GB DuckDB/1thread with SAME3GiB
process cap, only if guard preflight passes. Measured1GB-query nativepeak1.773
and post-exit free5.8GiB physical/10.6GiBcommit support this bounded increase;
do not raise guard/global settings. If still insufficient, implement bounded
replacement. No table-swap rewrite or weaker gate yet. Fresh filenames/runid.

ACTIVE: adjusted_return_consumers42focusedtests PASSED (.933GiBnativepeak),
Importantcohortclockfix acceptedonreport; commit imminent. CF1 nowowns DBslot
for6cases +necessaryschema/module checks and own0313registry lock; independent
reviewab65da79CLEAN. Fresh price_failure_recovery owns ticker_history_bulk.py,
activation.py and narrowconnection.py invalidated-connection recovery, plus
focusedtests/report; NO schema/registry/jobs. It will fix original-error/failed
ledger preservation and bad LOGGER '%,d'. CodeonlyuntilrootgrantsDBslot.
Then root targetedledgerclosure +price retry +allform/archiveSECprepass +run5.
No sourceprepass/run5/release/fullsuite/merge yet. Main goal remains active.

LIVE OVERRIDE at16:43UTC: integratione5a9941b committed,125focused checks
passed/one default slow skip; independent controller review CLEAN ea657622.
AR3 fix8d2e07d9 and AR1b fixa685b307 accepted on their focused reports.
Root launched FIRST new production write:
activation-prices-updated, tool session5584, guard child11460,3GiB tree cap,
DuckDB1GB/1thread, explicit2026-09-20, native updated Parquet, --backup-keep100.
Governed migration303..312 occurs before price publication. Logs/receipt:
activation-prices-updated.log/.err/-memory.json. ONE heavy slot is root-owned;
agents may code/review but no DB tests/scans. Root holds registry during launch.
CF1 custom_features_production and adjusted_return_consumers ACTIVE on disjoint
files; neither has run tests. CF1 migration0313 remains unregistered until lock
transfer. Main source prepass and activation-run5 still to follow after price
success. Do not treat this launch as completed migration/publication evidence.

CF1 pre-outcome horizon correction: PRIMARY21, SECONDARY5/63, embargo63,
matching existing production labels(1,5,10,21,63). Initial20/60 draft superseded;
do not create another label panel or change global IC constants. Agent informed.

- Latest user priority: complete production US fundamentals, all core metric,
  ratio/growth/per-share families; add custom TickerHistory feature tables and
  measured forward-return decile spreads. Avoid niche cleanup/test loops.
  CF1 brief is custom-features-production-brief.md, migration0313 reserved.
  Eight predeclared hypotheses,20-session primary,5/60 sensitivity; no guaranteed
  significance. Fresh implementer not yet dispatched (waiting for repair slot).
- Native Parquet reader981f1ec7 plus Important schema repaircf003928 accepted.
  Root FULL audit of staged updated file completed:32,323,644rows,
  2012-03-26..2026-09-18, latest12,489rows/12,462positivevendorIDs;
  1,345repeatedpositivekeys/2,690quarantinedrows;332,607invalidOHLCV;
  272,181ID0;892invalidkeys;0invalidadjustedproducts.
  43,082adjusted-return residuals>1e-3 among31,461,105comparables;
  0prior-close/cumulative-recurrence residuals>1e-8. Internal consistency only.
  Source SHA matched sidecar and unchanged size/mtime.174.889s;1GB/1thread,
  native job peak1.627GiB under2GiBcap.3GiB first launch safely refused for
  headroom. Receipts updated-price-source-audit.json,
  updated-price-audit-memory.json and updated-price-audit-memory-2g.json.
- AR3 cohort93896509 review3d9f7425: two Important repairs coded by original
  implementer (inclusive valid_to, deterministic inserted timestamp). Currently
  owns DB test slot for TWO focused cases, then AR1b seven cases next.
- AR1b SQL panel86aa0e7b review9e602bc5: one Important repair coded by original
  implementer (invalid terminal corrections cannot resurrect older revisions or
  erase terminal boundary; explicit quality failure). Waiting DB slot afterAR3.
- Calendar memoryad6ab754 independently CLEAN in3d9f7425. Accepted.
- Fresh activation_production_integration ACTIVE: exclusive activation/jobs,
  registry/migrations0312; bounded actual legacy universe, all-forms50CIKbatch,
  native source path,1GB/1thread/16sequentialshards, parents/projections, terminal
  and forward panels, actual annual coverage/provider/finalquality stage.
  Narrow optional explicit checked_at in quality/_runner.py approved.
  New production_panels.py is this agent's work. No tests/live writes yet.
- No live writes since run4. Still schema0302, old prices throughJune15,
  incomplete archive coverage and no dated historical exchange evidence.
  Root production reload follows reviewed integration/migrations; do not wait
  for CF1 to complete before source activation. One heavy workload only.
- Remaining adjusted-return reader task is ready in
  adjusted-return-consumers-brief.md (not dispatched). Preserve frozen legacy
  parity; unknown split-only evidence cannot certify split/share-based outputs.
- Final gates unchanged: actual full-universe activation-run5 from
  statement_points --force, measured item/provider/allquality, publish actual
  numbers and first release, dictionary regen, full non-slow suite ONCE,
  Codex whole-branch review, ASK before main merge. Remind stash@{0}; never
  apply/drop it. Other-session atx-engine/atx-impl untouched.

---

# Earlier controller override: updated price source and closed repairs

Updated after 2026-09-20 12:00 local. This override supersedes older notes below.

- AR4 fixes29908a7c plus catalog facadec401fb31 are committed. Critical rereview and first catalog review both CLEAN in6e2785ef.55 source tests+1facade seam passed, bounded helper16tests previouslypassed; no live writes. AR4/fundamentals lock free.
- S3T8 two Important repairs accepted on report: c1f0d95c restores same-day EV baseline precedence and eligible parent ranking. New independent frozen90row/24name edge fixture,3focusedchecks and strengthened1e-12 numerical testcase passed. No46k/fullsuite rerun. S3T8 closure complete.
- AR6 source loader repairs92a40d97 and T11 docs repairs012aaf93 accepted on report.32focusedcases passed under3GiBguard (4GiB initially REFUSED before launch for low headroom). Native peak~0.706GiB, notRSS. Dictionary --check and docs syntax passed. All prior retirement/docs reviews closed.
- User asked root to web-search newer bulk sources. Investigation completed and committed5f3eb5ea, atx-db/docs/BULK_PRICE_SOURCE_OPTIONS.md: official SpiderRock S3, Massive daily files/grouped API/PIT references, EODHD bulk, Nasdaq table exports, Norgate historical content. No account/purchase/messages or user email external. No matching provider environment/.env keys; no AWS config/credentials files. Stooq direct documentation blocked by browser verification, no approved replacement. No further newer-price question needed: user then supplied current source.
- USER supplied completed Downloads/TickerHistory3.parquet (notZIP). Footers:32,323,644rows,262rowgroups,2012-03-26..2026-09-18,3,617,973,507bytes. Copied with1MiBbuffer under2GiBguard to atx-db/data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet. Both originals/oldsources preserved. SHA2560ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae. Source size/mtime unchanged acrosscopy. Receipts updated-price-footer.json, updated-price-staging.json, updated-price-staging-memory.json. Footer dates alone are not fullaudit. Fullnewsourceaudit/nativepublisher still pending; don't use oldTSVasfinalsource.

## Active fresh tasks

1. /root/forward_panel_bounded: AR1b delisting forwardwriter section + checks_survivorship.py +focusedtests. SQL perhorizon staging, actual calendar enddates, partial preterminal leg, maxinput clocks/cutoff, corrected adjusted price default, explicit rawcompatibility, input-derivedemptyoutputcheck. No activation/jobs/registry.
2. /root/annual_coverage_bounded: AR3 item_coverage/cohort, measurementCLI, provider/identityDQC alignment, tests/docs. EXCLUSIVE registry/migrations __init__ ownership for reserved0311body and ownimports. Proposed APIs refresh_item_coverage_cohort(store,AnnualCoverageCohortOptions(as_of_date=...)), measure_item_coverage(store,ItemCoverageOptions(as_of_date=...)). Exact3000positivecap/PITcommon/year evidence and allcompletedFY2015+; currentyear separate, noempty/undersizedpass. No universe_us_listed/activation/jobs.
3. /root/parquet_price_input: native boundedread_parquet in shared ticker_history_quality and bulkpublisher/auditCLI, source_path+format provenance withTSVcompatibility. Newsource FULLaudit and productionwrites remain ROOT only afterindependentreview. No activation/jobs/registry.

No heavy task currently running; every implementer must request root DBtestslot beforetests. Defaults1GB/1thread, -n0 focusedonce, processguard2?3GiB when headroom permits. Lastcopy preflightphysical4.49GiB/commit7.82GiB, so4GiBguardwouldrefuse as intended. No tests overlap production/audits. Old backups preserved (threeexisting); disk155GiBfree before3.62GBcopy.

## Next tasks and production ordering

- Fresh calendarization_bounded implementer NOT spawned (slotlimit): catalog-memory-report.md finalbrief; preserve purecalendaroracle andcompleteissuerinference, SQL or boundedissuerbatches, atomic scopedreplacement, no fullperiodPandas. No unrelatedcalendarTTMrewrite.
- AR1/AR2 integration NOT spawned: activation-integration-addendum.md controls. Jobs/activation lock currentlyfree. Allforms with50CIKbatch; boundeddefaults;16SEQUENTIALreconciliationpartitions(onechild), planningquerycap; actuallegacycohort->parents->23projections; evidence/terminal/reconciliation/calendar/AR1b beforequality; AR3cohort/itemcoverage beforeprovider. Emptycohort explicitdegraded, no expensive emptyscopealltablefallback.
- AR5 listingdiagnostic helper NOT started; no datedexchangeevidence currently. Newpricefile has noexchange/typehistory, so this prerequisite remains. Historicalsource question earlierpending, no answer. No backdating.
- Downstream corrected-adjusted-return and split-evidence readers from AR6report still queued. Finish or exclude affected outputs before certification. Bulkcurrent_date/source_loaded_at identitymapping remains explicitlyunverified inT11docs; no blanketdeterminismclaim.
- After newcode/tests/reviews/migration0311stable: governed migrate --backup-keep100(spacedflag). FullParquetsourceaudit1GB/1threadguard; correctednativepublish underguard. Allforms and archivecompanyfacts fullreplacement sequential; choose guardcap fromheadroom, no6GB/8thread defaults. Then activation-run5 fromstatement_points --force withasof2026-09-20,1GB/1thread/16sequentialshards; watchledger/logandfixfailures.
- Measureitem/provider/allquality, publishnumbers, thresholdsneverweakened, schemafliponlymeasured. Firstsixdatasetreleasewithhonestquality and hashverification. FullnonslowsuiteONCE atgate, freshCodexwholebranchreview, ASKbeforemerge. Nothinglivewritten/fullsuited/released/merged yet. Finalreminderstash@{0}other-session96trackededits, neverapply/drop.

---

# Tier-1 continuation queue — current controller state

Updated 2026-09-20 after run4 termination. Branch feat/tier1-parity. Codex-only agents; no Claude work or LLM API spend. The original literal commit trailer remains required. Earlier implementation notes below are historical; this override and program.md control.

## Current override: run4 terminal, memory safety, review queue

- Run4 companyfacts completed at14:59:21UTC:31,590,760facts,8031targets,7035loaded-target outcomes,996failed targets,6533unresolved candidates. Its old stage incorrectly reports completed despite target failures. Statement_points FAILED15:00:49UTC after88.75s: pandas full-fact allocation in refresh_xbrl_concept_catalog. Parent2264/writer22840 exited without our intervention. No active warehouse writer or new production writes since.
- Read-only live inventory post-run4: migrations through0302; prices31,178,192rows/34,803securities through2026-06-15; directory only2026-09-20; no dated exchange evidence; legacy universe empty. Companyfacts6533uniqueCIKs of20390archive members;13857archiveCIKs missing. Only10K/10Q/8K submissions loaded. See post-run4-inventory.json; the candidate-status query alone needs correction. Old run3 running ledger entry is stale history, not active work.
- S3T8 committed72d38a93 plus fixture a4fcc055; review_s3_t8 active. S4T10 dae4220e independently clean. AR4 committed0b797db8; independent review reports Critical unresolved identity isolation and Important replacement/payload issues; final report pending, original implementer repair next. AR6 committeddc130c53; dictionary current and3deferred checks passed. review_price_docs_guard independently reviews AR6 and T11 plus controller guard.
- Catalog memory helper committed8a95a3ec:16minimaltests+snapshot/ruff/mypy pass. Exact live READ-ONLY31.59Mfact aggregate returned242concepts in26.031s at1GB/1thread; process peak ~1.05GiB. Fundamentals facade still pending behind AR4 fix ownership. Helper review also pending. Later calendar mapping has a full-period pandas risk; bounded brief in catalog-memory-report.md.
- USER MEMORY RULING: host has~15.7GiBphysical/~23.7GiBcommit. One heavy workload only; no production concurrent with tests/audits. Start DuckDB1GB/1thread and reconciliation16sequentialshards(onechildatatime); inspect all worker pools. Never reuse old6GB/8thread/8shard command. Controller run_memory_guarded.py caps own process tree with Windows JobObject (maximum4GiB), checks physical/commit headroom, refuses insufficient memory before launch and stops only own job on low headroom. Incremental128MiB probe passed at104MiBallocated; receipts memory-guard-probe-1/2.json. Native peak metric may include failed allocation attempt; it is not resident memory evidence. Guard review pending. No global/pagefile settings changed.
- Remaining implementers not yet dispatched: AR1b forward SQL/panel; AR3 coverage/cohort SQL; AR1/AR2 activation; AR5 historical-input diagnostics; AR6 downstream return/share paths; calendarization memory follow-up. All migration reservations beyond0310 must be explicit. No full suite, live release, or merge done.

## Complete

- Handoff reconciliation: S3 T7 850ec527 +5b11a272; S4 T7 cbd5488e +43100735; S4 T3 fix1 28dee5a7; S4 T4 fix1 a17ab540; S4 T8 9a5c75bd +62ba125a. Focused tests/reviews accepted. Publication CLI guards pending migrations and pins output ordering.
- Committed HEAD5b11a272 independently exported via git archive: import atx_db and module-boundary/schema-v2 tests passed (one default slow skip). Log reconcile-head-check.log. Full suite has NOT run.
- S4 T10 dae4220e: two leaf modules retired, three legacy valuation schedules disabled by dataset ID, historical data preserved.39 focused checks, independent review clean.
- S3 T8 baseline companion a4fcc055:46,338 genuine legacy rows,23 factors,28 securities,89 dates,min24 per emitted cohort; source/input/output hashes frozen. Report in S3 task-8-fixtures-report.md.
- S4 T11 implementation de32ea6e: docs/CI/generator,18 focused checks, ruff/mypy/YAML passed. Independent review outstanding; dictionary intentionally stale until source settles, then regenerate and run3 deferred current-file checks. CI check remains unconditional.

## Active ownership and serialization

1. /root/s3_t8_retirement owns compatibility/projection/seed/parity,0309 body+registry/__init__, public snapshot,18 legacy modules/14 wrappers/18 old tests, and minimal Piotroski consumer closure. It owns the DB pytest slot through final deletion gate. Full46,338 keys/all23 raw+zscore parity PASSED after net-payout winsor fix2.5%. Consumer tests passed: canonical-source-only supersession of exact legacy factor/source/date slices preserves physical history/unrelated rows; empty refresh clears stale latest flags; Piotroski selects newest eligible source after cutoff. Four consumer cases passed. Deletion/commit underway; independent review next. Registry lock retained until commit.
2. /root/archive_companyfacts_targets implements AR4, owns fundamentals.py, build_companyfacts_bulk.py, related tests, EXCLUSIVE jobs.py/activation.py edits and warehouse_activate CLI logging. Ready for DB tests (about35cases); NEXT slot after S3. Pure5cases and strict mypy fundamentals/activation pass. Explicit archive_members mode; default replacement, append-missing explicitly limited; no receipt migration, but archive/allowlist hashes and failure/empty/unresolved accounting. No current-ticker fallback for archive historical identities. Failure details retained in logs/ledger error. Commit/review pending. Leave four known jobs.py lint findings unchanged.
3. /root/price_source_correction implements AR6, owns ticker_history.py/bulk.py/tests/new audit helper plus price-specific README/runbook/generator wording. No jobs/activation/registry edits. Same-row rawclose*cumulative factor, NULL split-only factor, historical display ticker, quarantine duplicate positive vendor keys, honest provenance. Offline full-source audit running, no warehouse access. DB pytest slot AFTER AR4. Exact downstream readers needing coordinated correction will be reported separately. Commit/review pending.

All shared edits serialized. Never git stash/checkout --/reset/restore/clean; pathspec-only commits. Never commit another owner's registry imports. Snapshot-line bleed only allowed exception. No new unreserved migrations;0309=S3T8,0310=S4T10. Governed migration handles lower-version gaps; ordinary initialize max-version shortcut alone does not.

## Live run4

Run4 TERMINAL as recorded in current override above. Submissions completed3,045,440 rows/47,869CIKs/5,374 history members in5702s. Live warehouse about6.3GB. Preserve old run3 stale-running history.

Prices previously31,178,192 rows/34,803 securities through2026-06-15. Only directory snapshot2026-09-20. Archive inventory20,390CIK members is notUS-listed coverage. No live quality/coverage/release completed yet.

## Mandatory remaining readiness tasks

Read activation-readiness-audit.md for AR1–AR5 bounded briefs, activation-forward-panel-brief.md for AR1b, price-source-correction-brief.md for AR6.

- AR1: all SEC submission forms (current ladder defaults10K/10Q/8K and misses25/15/merger evidence), terminal-return refresh and code reconciliation activation. All-forms offline supplementation after run4. Preserve uncovered reasons and Shumway default per PIT exchange; no blanket filling.
- AR1b: bounded SQL forward-return writer (current all-bar pandas expansion too large), consistent calendar endpoints, nonzero preterminal partial return, all input clocks, populated cohort-only endpoints, non-vacuous input-derived drop check. Calendar + panel must run before quality. AR6 dependency handoff may require corrected adjusted input handling; do not conflate raw price and verified total returns.
- AR2: build actual legacy liquid cohort, refresh selected retained parents via derived_compatibility_parents.refresh_compatibility_parents, then23 canonical projections; jobs+activation integration, diagnostic zero-cohort/factor counts. Retained asset_growth mapping remains explicit opt-in and original publisher stays. Parent helper handles QOP600day history. Compatibility outputs publish conservatively at22:00 with raw input lineage; generic quarter/daily retains cohort-max availability.
- AR3: real separately named top3000 market-cap annual common-equity cohort, complete FY2015+ range, same-year membership join/is_member filter, registry item cross product and zero rows, SQL aggregate instead of full pandas, scoped empty-refresh clears stale coverage. Align provider/DQC defaults; retain110items/90% annual threshold. Missing/undersized historical cohort must fail, not silently pass. Migration number must be reserved before implementation.
- AR5: dated listing evidence diagnostics. No current directory backdating, no NULL-exchange placeholder used as historical proof. A bar-observed candidate surface can be reported separately, not as certified US common equity. User source question is pending.
- AR6 downstream follow-up: return readers using split_factor need corrected adjusted-price semantics; issuance/share readers require genuine split evidence. Follow implementer handoff, don't relabel dividend factor as split-only.
- Finish S3T8 review; review T11 and AR6 (may batch one independent reviewer with distinct reports); review AR4. Final dictionary regeneration. Whole-branch review uses Codex, superseding original Opus request.

## Price source evidence

Official SpiderRock dictionary confirms closePr is adjusted PRIOR close, not current adjusted price:
https://docs.spiderrockconnect.com/docs/next/HistoricalData/Data%20Dictionaries/TickerHistory3/

Loaded original staged file: atx-db/data/staging/broad-bars/tbltickerhistory3_10y.txt,11,084,562,320bytes, recorded SHA25696cb7fbde52e03c7f6559bc1ccdf2dd97e8cec225d93128200afeb8d60ab0629 plus sidecar. Use same file for correction; other-session archive counts differ. Read-only engine source-audit reports support cumulative direction but also record economic adjustment exceptions. Do not modify other-session files or borrow their counts as measurements of this archive. Historical-vintage availability remains unverified; date22 is a modeled backfill convention, vendor current delivery description is05:00CT T+1.

## Operator sequence after code and run4

1. Inspect live ledger/source inventories using audit commands. Governed migrate only after0309+all final migrations exist; use --backup-keep100 actual syntax --backup-keep 100 to preserve existing backups (default3prunes). No backup deletion without user OK.
2. Supplement all submission forms from local archive; archive-wide companyfacts replacement via AR4 command; corrected source-price republish via AR6 command. No single-CIK HTTP loop. Dummy SEC UA atx-db/0.1 atx-research@example.com only.
3. Run activation-run5 from statement_points --force, starting --memory-limit 1GB --threads 1 --shards 16 --backup-keep 100 with explicit as-of date, under verified process-tree memory guard. Watch all stages/ledger/logs; failures become tasks. Escalate memory only after measured headroom; never overlap heavy jobs.
4. Measure actual item/provider/quality coverage, unresolved identity/history/price-source limits, publish numbers in docs. Schema conditions flip only on thresholds measured. First publish_release plus manifest/hash verification only after assessing actual gates; do not call a degraded/empty export full Tier-1 parity.
5. Full non-slow suite ONCE at gate, fresh Codex whole-branch review, then ask user before merge to main. No merge authorization yet.

## Pending user/source and stash

An async question already asks for a historical listing/security-master local archive path or licensed source. No answer as of this snapshot. Source check recorded in historical-listing-source-check.md; no purchase/subscription or user contact info sent.

Remind user at final handoff: stash@{0} holds their other session's96 tracked edits. Do not apply/drop it ourselves; leave other atx-engine/atx-impl work untouched.
Latest 10:57 updates: S3T8 committed72d38a9 and independent /root/review_s3_t8 active; registry lock free. AR4 committed0b797db8 with42unique focused checks, independent /root/review_archive_companyfacts active; activation/jobs lock free. AR6 completed26focused cases (one floating parser assertion corrected on failed-only rerun), strictmypy/ruff passed; dictionary regeneration now AUTHORIZED in its commit, three exact deferred current-file tests plus --check. DBslot free. AR6 source audit on exact stagedhash:31,598,499rows (prior31,464,423 inaccurate),978rows over489repeated positive keys,351,651 ID0,419,228 invalidOHLCV,0bad cumulative products;79,365 adjusted-return residuals>1e-3 among30,679,047 comparable adjacent dn pairs. Internal consistency is not economic verification. Receipt ar6-source-audit.json. After AR6 commit, next fresh implementer AR1b forward writer/quality (brief has later adjusted-input addendum); AR6+T11 independent review queued. Source counts must be published accurately in final docs. No live read/write yet.

Memory audit correction: refresh_reconciliation_sharded.py runs subprocess.run sequentially, so shard count is partitioning, not concurrency. Use16 (or more measured smaller partitions), one child at a time;1shard would increase memory. Input symbol planning also needs an explicit low session cap in activation integration. Corrected candidate inventory:6533proposed unresolved fact candidates.

## Latest repair ownership and next dispatch

- AR4 original implementer repairs1Critical identity isolation+3Important replacement/payload/normalizedCIK findings; owns fundamentals.py and sourceCLI/tests. Also adds explicit1GB/1thread bulkCLI controls, then separate catalog-facade delegation commit. DBpytest slot currentlyAR4. Critical rereview+first cataloghelper review next, independent reviewer can combine bounded reviews. No jobs/activation edits expected.
- S3T8 original implementer repairs2Important same-day EV selection and parent eligibility-before-ranking. Frozen compact genuine-baseline edge fixture (24names/4EVvariants) approved; next DBslot afterAR4, no46k rerun. Accept fix report, no rereview.
- AR6 original implementer repairs2Important stale quarantined keys and nondeterministic identity, then T11truth/memory docs2Important corrections. DBslot afterS3. No Critical; accept report. Guard reviewed no blocker and nowversioned0e6737fa, incrementalprobe passed. Root interim measuredstatus doc committed844274ae.
- Next fresh tasks when slots release: AR1b production SQL forward panel; AR3 actualtop3000/SQLcoverage (0311reserved); catalog calendarization memory follow-up; AR1/AR2 integration (read activation-integration-addendum.md); AR5listing diagnostics; downstream adjusted-return/share-evidence reader repairs. Critical source rereview must close before warehouse changes.
- Reconciliation partition correction confirmed:16sequential shards/onechild, not1giantshard. Allforms sourcebatch should be50CIKs initially, not2000. Update scalar planning DuckDB session too. Tests serial -n0, fullsuite stillnotrun. Never overlapheavyproduction/tests/audits.
