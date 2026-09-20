# Latest controller state: full source audit complete, production and CF1

This section supersedes every older status below. Updated 2026-09-20.

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
