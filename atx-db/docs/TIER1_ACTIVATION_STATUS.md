# Tier-1 activation measurements

The production snapshot remains 2026-09-20. This page records interim
measurements, most recently observed on 2026-09-23; it is not a completed
parity gate or a release. Completed counts come from SQL or pinned source
artifacts. Dated loader progress below is historical evidence.

## Latest durable warehouse measurement: 2026-09-23

Archive16 stopped under the host memory guard at22:16UTC during point
verification. Its ledger recovery and checkpoint completed22:19:32UTC,
confirming the counts below and zero new attempt rows. Next source predecessor:
`513cfbbc-096a-4186-9666-b6cc5170c4ad`. A smaller recovery-only budget completed
at0.829GiB process peak; source loading keeps its existing1GB DuckDB/2GiB cap.

While backfill awaits host capacity, bounded read-only pipeline status is
committed (`b814f4c1`, eight focused checks passed). Platform work continues on
daily PIT fundamental signal tables and forward-return decile evaluation.
Selected-input lineage is committed (`1fa721c7`, 11 focused checks passed at
0.683 GiB peak); its migration0323 has not been applied to the live warehouse.
The PIT panel (`fbba7bf2`, 12 focused checks) and decile evaluator (`d82f9eca`,
8 focused checks) are also committed. Their measured guarded peaks were
0.657 and0.614 GiB. Migrations0324/0325 remain unapplied; no live research run
or new alpha measurement is claimed. Wider schema checks remain pending after
a host guard stop. The prepared fundamental decile acceptance SQL returns all
five hypotheses across three splits and three horizons, including missing
run/results diagnostics.

The optional fundamental research readiness inventory is also implemented:
five isolated checks passed under the1.5GiB guard at0.570GiB peak, with scoped
Ruff and the two Important review fixes accepted. It reads bounded manifest,
coverage and summary records, reports missing schemas separately, and keeps
certification unmeasured. It does not revalidate panel or label digests. See
[research readiness](FUNDAMENTAL_SIGNAL_READINESS.md).

The dictionary was regenerated and its12 existing checks passed; it now
includes issuer-content APIs and frozen fundamental hypotheses. A clean
export of commita89c2677 imported atx_db successfully. The broader required
schema and affected-regression batch was interrupted by host memory pressure
before completion and remains pending. None of these checks changed live
warehouse rows, migrated0323..0325, or established an alpha result.


The later archive15 process disappeared before point verification finished.
Its missing handle and absent workers were verified at22:01UTC; the stale
run ledgers were closed and CHECKPOINT passed. No termination cause is
asserted from its stale-running guard receipt. Recovery reconfirmed every
retained count below, with zero new attempt rows. The next actual predecessor
is`4932365c-4b61-4437-b634-11b2a7284f6e`. Source loading remains pending a
stable memory window; all existing process and host headroom limits remain.


Archive14 was subsequently stopped by the host headroom guard during source
verification at01:24UTC. Recovery at01:28UTC confirmed the same retained
counts and zero new attempt rows; both run ledgers were closed and CHECKPOINT
passed. The next predecessor is`96f93269-84b5-4854-8ebe-fc5b99ab6612`.
Further source loading is waiting for sustained host headroom. The generic
failure-ledger repair in15235456 passed its focused regression and independent
review; forceful process termination still requires operator recovery.


Archive13's lower-memory trial passed its resume proof but failed during a
new issuer's commit at the512MB DuckDB limit. Read-only inspection at01:09UTC
confirmed the retained counts below and zero newly committed fact rows.
The guard reported1.199GiB peak process-tree memory and healthy host headroom.
The stale dataset run was then marked failed and checkpointed; the original
activation failure was preserved. Its actual dataset ID,
`22d51d47-2992-4043-95ef-54763a9dd45d`, is the next resume predecessor.
The next run returns to the previously working1GB DuckDB limit, one thread,
and the unchanged2GiB process cap and host headroom guard.

The archive12 figures below describe the preceding successful source additions.

Archive12's recovery checkpoint completed after its memory guard stopped the
run at00:26:29UTC. It retained55,234 attempt rows across22 CIKs, including
replacements, for a net increase of34,193 raw facts and points. The actual
dataset receipt for the next resume is
`6beba5d4-e530-45fd-ae04-f872d9c8a896`. Both interrupted run ledgers were closed
as failed at00:29:16UTC; the recovery process exited successfully with a
1.469GiB peak. These latest retained counts supersede the preceding snapshots.

| Latest retained measurement | Result |
| --- | ---: |
| SEC CompanyFacts rows | 47,941,000 |
| Fundamental points | 47,941,000 |
| Daily price rows | 31,959,271 |
| Custom-feature rows | 31,934,514 |
| Applied production migration | 0322 |

Archive12 verified9,440 prior targets covering39,402,481rows before loading
additional issuers. It remained incomplete when host commit headroom dropped
below3GiB. The source log had no reported loader failure. A static throughput
audit did not identify a proven redundant query to justify another rewrite;
a lower DuckDB memory setting is being assessed without changing proof,
scope, the process cap, or the host stop thresholds.

## Preceding checkpoints: archive10 and archive11

Archive10 stopped under the memory guard at22:48:44 UTC. Recovery committed
its two failed run records at22:52:07 UTC; independent verification completed
the checkpoint at23:00:57 UTC. The retained raw facts and points increased by
900,566 compared with the preceding checkpoint. Archive11 then stopped on host
headroom during resume verification at00:05:42 UTC on September23, before any
new source rows. Read-only inspection and a successful recovery checkpoint
confirmed the counts below and the newly applied schema0322. Both archive11
run ledgers were closed as failed at00:08:15 UTC.

| Measurement | Observed result |
| --- | ---: |
| Retained SEC CompanyFacts rows | 47,906,807 |
| Retained fundamental points | 47,906,807 |
| Daily price rows | 31,959,271 |
| Custom-feature rows | 31,934,514 |
| Applied production migration | 0322 |
| Archive10 attempt rows / loaded CIKs | 1,753,504 / 621 |
| Archive11 attempt rows / loaded CIKs | 0 / 0 |

Attempt rows include replacements and are not the net increase. Archive10's
actual dataset run was `ade90629-8e06-4186-9ea7-565cbd05e285`. Archive12 resumed
using archive11's actual dataset receipt,
`694f056e-a268-4927-8500-990b62c9a9be`. Archive10 checkpoint verification peaked at1.539GiB under
the current2GiB process guard. Production DuckDB remains limited to1GB and
one thread, with one heavy runtime at a time. The guard stopped only its own
work when host headroom fell; other workloads were left untouched.

Code progress exceeds materialization progress. The CIK-owned issuer query
surface is committed in92cf42c4 with5focused and14existing API checks passed.
The SEC reported-quarter EPS source is committed in1a0e4f73 with22focused
source checks passed; actual Chevron filing-index discovery and Q42025
extraction also pass. The core bridge and public standardized record contract
3.0.0 are committed in ddcd49d8, with 22 integrated checks and a separate
upgrade/replay check passed. Issuer catalog migration0322 is committed in
42883bff; its upgrade check passed. The required package import, boundary and
schema checks now pass (57 passed, one expected slow skip, peak0.954GiB).
The two integration repairs are committed in938ff7f1. Archive11's interruption
and recovery are recorded above; its source resume remains incomplete.
These tests do not establish live metric coverage.
The full statement/ratio/growth/daily-market rebuild,
coverage and quality measurements, signal evaluation, and release remain
pending. Earlier migration versions and guard limits below describe their
dated runs rather than the current production state.

The retained submissions contain **426,151 distinct 8-K Item 2.02 candidates**
across 2004–2026, measured read-only at23:53:57UTC. This source queue is still
incomplete; candidates are not extracted EPS facts. The first earnings-source
wave will explicitly cover CVX for end-to-end acceptance, followed by the
full-universe fundamentals build and measured item-gap repair. Exhaustive
historical earnings-release coverage remains open. No provider SLO is waived
or certified by that scoped acceptance wave.

Latest successful price publication: **corrected prices were published** by
`activation-prices-updated-bounded` at 18:00:09 UTC. A separate read-only check at
18:02:30 UTC measured the following live state:

| Measurement | Observed result |
| --- | ---: |
| Daily price rows / distinct warehouse security IDs | 31,959,271 / 34,251 |
| Price date range | 2012-03-26 through 2026-09-18 |
| Latest-date rows / distinct warehouse security IDs | 12,386 / 12,386 |
| Invalid or missing adjusted closes | 0 |
| Nonnull split-only factors | 0 (unknown, intentionally) |
| Remaining price staging tables | 0 |
| Applied migration version | 0314 |

The stage took 325.562 seconds with DuckDB limited to 1 GB and one thread.
Native process-tree memory peaked at 1.779 GiB under a 3 GiB guard. The new
publication method validates a complete shadow table and swaps it atomically;
migration 0314 removes five nonunique secondary indexes that prevented bounded
publication. Public tables and their primary-key contracts remain in place.
The latest preserved pre-migration backup is 10,461,917,184 bytes, SHA-256
`2cb19981ad360add3f95f7da13a53bdeebd9521745c5de991cf34ba9de0d6c20`.

Earlier indexed publication attempts failed at COMMIT under both 1 GB and 2 GB
query limits. Their recovery checks confirmed the original live prices survived;
the replacement now supersedes that original snapshot. Only inspected,
regenerable failed staging was removed. All source files and backups remain.
Those failures and memory-preflight refusals are retained as operator evidence;
no source or coverage threshold was relaxed to obtain this successful result.

Current receipts under `.superpowers/sdd/tier1-parity/` are
`activation-prices-updated-bounded-memory.json`, its `.log` and `.err`, and
`updated-price-live-success.json` / `updated-price-live-success-memory.json`.
Earlier failure receipts keep their original names, including
`updated-price-live-measurement.json`; they describe the superseded snapshot.
All live work and tests run sequentially with the locked project Python runtime.

## Metrics build prerequisite

A focused static audit found a historical-revision defect before the first full
derived build. The earlier engine computed from only the latest standardized
revisions and replaced earlier derived states. The daily market join also
filtered standardized facts and DEI shares to latest-only revisions. Consequently,
a later amendment could remove a previously knowable historical value or make a
daily valuation fall back to an older quarter. Maximum-input availability
timestamps could not recover discarded states.

The P1 repair now preserves original/amended states and invalidation events in
bounded event frames, with daily/export/API consumers updated together. One
independent static review closed the original defect at source level. Focused
runtime verification ran after the companyfacts writer stopped. All P1 focused
cases passed, including
the populated upgrade; the repair is committed as `5519d1ac`, with no repaired
full-universe result yet measured. Evidence,
implementation and review reports use the `derived-pit-revision-` prefix under
`.superpowers/sdd/tier1-parity/`.

The catalog audit counted 173 definitions (143 on the quarterly grid and 30
daily). A separate implemented, statically reviewed wave adds 28 definitions
for ordinary quarterly YoY/QoQ growth, three-year CAGR, basic EPS and cash-cycle
metrics. These 201 definitions are committed in `0c52fdbd`. The combined focused
run had 65 passes and one Core test-expectation error; the corrected case then
passed alone. This is fixture validation, not measured live metric coverage.

A further source-path check found that annual statements are standardized but
cannot feed TTM flow metrics unless a complete quarterly history is available.
This prevents annual-only filers from supplying core margins, flow per-share
metrics and daily valuation denominators. The AF1 implementation adds direct
fiscal-year fallback with historical source precedence, actual fiscal spans,
canonical origin metadata and preserved quarterly gaps. Its first 79 focused
cases passed after three fixture/seam corrections. One independent review found
two Important selection issues; both are fixed, and six targeted preservation
and counterexample checks passed with peak process-tree memory 0.789 GiB.
AF1 is committed as `244af575`; touched Ruff and strict type checks pass.
Committed-source import, module-boundary and schema-contract checks passed
(one existing slow schema case skipped). The production rebuild remains pending. This is
source/fixture evidence, not a measured count of covered issuers; see
`annual-filer-metric-path-report.md` and `annual-fallback-implementation-brief.md`.

## Activation result

The issuer cleanup repair passed 38 focused checks and independent review, then
`activation-companyfacts-archive2` progressed beyond the earlier memory failure.
The operator stopped it at 19:47:59 UTC after observing one source-handling
failure and sustained repeated raw-fact scans. Peak process-tree memory was
1.737 GiB; this was an operator stop, not a memory-guard stop. Recovery measured
987,977 committed attempt rows across 146 CIKs, 19 empty outcomes and one source
error. Total raw facts and fundamental points were each 31,837,696; prices and
custom features were unchanged. The two interrupted ledgers were closed as
failed with the separately recorded operator recovery timestamp.

The source error is an exact two-byte `{}` archive member. A subsequent bounded
inventory found 62 such members among 20,390 CIK members, with no stored fact
rows currently belonging to those 62. Explicit unavailable-source handling is
implemented in `1ce0a850` (56 focused checks and clean independent review);
missing source data is not financial coverage. Prior issuer data
must be preserved when a placeholder is encountered.

The next committed throughput repair inventories distinct stored CIK spellings
once per load and uses exact bound predicates for issuer selection/deletion.
It passed 54 focused checks and independent review. A read-only comparison for
one already-processed issuer returned the same 11,550 distinct keys/checksum:
1.084 seconds for the numeric normalization predicate, 0.006 seconds for the
exact spelling predicate. This sequential, warm-cache comparison is not an
end-to-end throughput result. The full optimized source retry,
`activation-companyfacts-archive3`, failed at 22:02:44 UTC after 7,155.515 seconds.
A transaction COMMIT exhausted the 1 GB DuckDB allocation; the process-tree
guard was not triggered and measured a peak of 1.885 GiB. Both run ledgers
recorded failure without operator correction.

A separate read-only check at 22:04:46 UTC found 38,500,008 retained facts and
the same number of fundamental points. Price and custom-feature row counts were
unchanged. The attempt retained 20,205,629 fact rows across 3,750 loaded CIKs;
member receipts also record 708 empty, 26 unavailable and five errors. The last
loaded CIK was 0001033905. This is partial source ingestion, not full-universe
coverage. CF5 is committed as `b9572b27`: it bounds connection lifetime and
verifies retained source evidence before resuming, including unfinished
identity-candidate output. Its independent review found two Important issues;
both were fixed and all 53 post-review focused checks passed. Peak process-tree
memory was 0.704 GiB. Full archive ingestion at 1 GB still requires completion.

The verified resume, `activation-companyfacts-archive4`, started at 23:03:23 UTC
with DuckDB at 1 GB/one thread inside a 3 GiB process-tree guard. After governed
startup, its proof passed at 23:09:00 UTC: 3,750 completed issuers and 20,205,629
retained fact rows were verified against source receipts and matching fact/point
fingerprints. Both scans grouped 8,759 security identities; the proof took about
283 seconds. These are loader-log measurements, not a new completed warehouse
snapshot. The loader is now continuing through all 20,390 archive members;
full source completion and final memory peak remain pending. The preserved new
pre-migration backup is 11,529,629,696 bytes. Logs and the live guard receipt use
the `activation-companyfacts-archive4` prefix.
At 23:16:41 UTC its log reported 4,500 processed members, 3,758 loaded outcomes
(including verified reuse), 716 empty, 26 unavailable, zero failures and 34,438
newly processed fact rows. It has passed archive3's prior COMMIT failure point.
These attempt-write counters do not establish net warehouse growth or completion.

All five member errors were missing top-level payload CIK fields in otherwise
valid archive records. Exact archive filenames still identify those members.
The repair distinguishes absent identity fields from conflicting fields
and retain explicit source provenance. It also forwards the operator's dummy
SEC contact into loader options; the completed attempt used local archive
members without HTTP requests.
Latest evidence: `companyfacts-archive3-failure-inspection.json`,
`companyfacts-archive3-failed-members.json`, their guard receipts, and
`companyfacts-archive3-failure-report.md` under `.superpowers/sdd/tier1-parity/`.
Evidence: `companyfacts-archive2-stop-inspection.json`,
`companyfacts-archive2-ledger-recovery.json`, `companyfacts-placeholder-inventory.json`
and `companyfacts-cik-predicate-measurement.json`, with their guard receipts.

The first custom-feature build subsequently completed over the corrected prices:
31,934,514 daily rows /34,224 warehouse security IDs; 16,338,033 research-cohort
rows. Eight feature columns have finite observations, with zero nonnull
nonfinite values and zero measured date-order or modeled decision-clock errors.
Peak process-tree memory was 1.767 GiB under the 3 GiB guard. Forward labels and
decile evaluations remain pending. See [custom-feature measurements](CUSTOM_FEATURE_RESEARCH.md).
This supersedes the earlier empty CF1 inventory below, not the still-empty
fundamentals and market-metric surfaces.

The next full-archive companyfacts attempt, `activation-companyfacts-archive1`,
failed after 68.375 seconds on its first issuer. The correlated cleanup DELETE
in `_replace_facts` exhausted the 1 GB DuckDB budget; native process-tree peak
was 1.838 GiB under the 3 GiB guard. This was a query failure, not a host crash.
The stage and dataset both recorded failure. A separate read-only check at
19:24:24 UTC confirmed the existing 31,590,760 raw fact rows and the same number
of fundamental point rows remained; corrected prices also remained intact.
The repair must narrow the cleanup to the issuer's old keys before retrying at
the existing memory limit. Evidence: `activation-companyfacts-archive1-launch2.*`,
its memory receipt and `companyfacts-archive1-failure-inspection.json`.

A read-only report at 19:20:47 UTC found zero rows in standardized statements,
derived metrics, daily market/risk metrics, survivorship forward returns and CF1
outputs. Provider SLOs were not run, and the annual coverage gate had no eligible
measured item. These empty surfaces are outstanding production work, not passes.
The report took 18.94 seconds and peaked at 1.530 GiB under a 3 GiB guard;
warehouse file metadata was unchanged. Full results are retained in
`production-measurement-prepass2-fix1.json` and its Markdown companion.

The all-form source prepass was interrupted by its memory guard when host
headroom fell to 1.096 GiB physical and 2.921 GiB commit space. A bounded
read-only recovery check at 19:10:48 UTC found 8,504,213 committed rows from that
attempt across 37,700 CIKs. Total submissions were 10,297,910 rows across 72,810
CIKs and 668 forms. Attempt rows include replacements, not only net additions.
The archive SHA-256 still matches its download receipt. These partial results
do not establish full archive or US-equity coverage; companyfacts did not start.
Exactly the two interrupted stage/dataset ledgers were closed as failed, with
`finished_at` recording the operator recovery time, 19:16:54 UTC. The separate
UTC correction receipt preserves an initial controller timestamp-cast error.
Source files, committed rows and backups were preserved.

Recovery evidence: `source-prepass2-stop-inspection.json`,
`source-prepass2-ledger-recovery.json`, `source-prepass2-ledger-utc-correction.json`
and their memory receipts. A loader lifecycle/progress change passed all eleven
focused submissions tests under a 2.5 GiB guard (0.670 GiB measured peak); it
was not present in the interrupted process. It does not guarantee protection
from changes in other applications' memory consumption.

A directory-only inspection at
19:04:53 UTC found 991,042 members in the local 1,564,656,199-byte submissions
archive: 985,667 main CIK members, 5,374 history members and one other member.
Declared expanded size is 5,741,005,528 bytes. These are archive member counts,
not filing rows, completed ingestion counts, unique issuers or US-equity coverage.
No payload or warehouse data was read by this inspection. Its separate 1 GiB
guard recorded a 0.770 GiB peak and successful exit; the production writer kept
its existing 3 GiB cap. Receipts: `submissions-archive-inventory.json` and
`submissions-archive-inventory-memory.json`.

Run4 loaded 3,045,440 submissions and 31,590,760 raw companyfacts rows.
The companyfacts stage reported 8,031 targets, 7,035 loaded-target outcomes and
996 failed targets; its old completion status did not enforce target success.
Loaded-target outcomes are not unique issuer counts. The next statement_points
stage failed at 15:00:49 UTC because its concept catalog materialized all facts
in pandas and exhausted memory. No later run4 stage completed.

The warehouse had migrations through 0302 at the run4 measurement below; the
successful price publication above applied through 0314. A stale running
entry from run3 is historical bookkeeping, not an active writer.

## Source and identity coverage measured after run4

This historical table predates the successful price replacement above and the
all-form submissions / archive-wide companyfacts prepass. It is retained for
comparison; it is not a measurement of the rebuilt fundamentals warehouse.

| Measurement | Observed result |
| --- | ---: |
| CIK members in local companyfacts archive | 20,390 |
| Distinct CIKs with loaded facts | 6,533 |
| Archive CIKs without loaded facts | 13,857 |
| Current ticker-directory CIKs | 8,031 |
| Archive CIKs outside current ticker directory | 13,355 |
| Loaded CIKs outside current ticker directory | 0 |
| Proposed unresolved fact identity candidates | 6,533 |
| Raw companyfacts rows | 31,590,760 |
| Loaded 10-K / 10-Q / 8-K rows | 238,934 / 718,086 / 2,088,420 |
| Other loaded submission forms | 0 |
| Daily price rows / distinct security IDs | 31,178,192 / 34,803 |
| Daily price date range | 2012-03-26 through 2026-06-15 |
| Symbol-directory rows | 13,258 |
| Symbol-directory snapshot date | 2026-09-20 only |
| Dated listings with nonblank exchange or MIC evidence | 0 |
| Legacy universe membership rows | 0 |

Archive membership does not establish US common-equity eligibility. Current
tickers cannot establish a survivorship-safe historical universe. Historical
listing evidence and identity resolution remain prerequisites for that claim.

## Price source audit

The updated user-supplied Parquet file has been audited and published as described
above. Its 32,323,644 source rows span
2012-03-26 through2026-09-18; the latest source date has12,462 positive vendor IDs.
The native bounded loader and duplicate quarantine are implemented. See the
[updated source receipt](BULK_PRICE_SOURCE_OPTIONS.md) for the exact hash,
quality counts and measured memory. The following TSV audit remains historical
evidence for the old input, not the new file's measurements.

The original staged `tbltickerhistory3_10y.txt` is 11,084,562,320 bytes with SHA-256
`96cb7fbde52e03c7f6559bc1ccdf2dd97e8cec225d93128200afeb8d60ab0629`.
The complete audit counted **31,598,499 source rows**. The earlier operator
entry of 31,464,423 lines was inaccurate for this same file.

The audit found 489 repeated positive vendor-ID/date keys involving 978 rows,
351,651 rows with vendor ID zero, 892 missing display keys, and 419,228 invalid
OHLCV rows. These categories can overlap and should not be added together.
There were no invalid cumulative-factor products. Among 30,679,047 comparable
adjacent-day pairs, 79,365 adjusted-return residuals exceeded 0.001. Internal
factor consistency does not establish economically correct total returns.

The source dictionary defines `closePr` as prior adjusted close. Corrected code
uses same-row raw close multiplied by the cumulative return factor for current
adjusted close and leaves split-only factors unknown. The successful live
publication now uses this corrected mapping. See the
[vendor dictionary](https://docs.spiderrockconnect.com/docs/next/HistoricalData/Data%20Dictionaries/TickerHistory3/).
Historical delivery vintages remain unverified; the date-at-22:00 availability
rule is a modeled backfill convention, not historical delivery evidence.

## Memory and pending gates

The replacement concept-catalog query aggregated all 31,590,760 live facts into
242 concepts in 26.031 seconds, read-only, with DuckDB limited to 1 GB and one
thread. Peak process working set was 1,119,350,784 bytes; peak process commit was
1,128,935,424 bytes. This validates that query only. Facade integration and its
review are now complete; bounded calendar mapping is also independently reviewed.
Production-stage integration is committed and independently reviewed, with125
focused checks passing and one default slow skip. Full-scale activation remains
pending the successful source prepasses.

Future production work is serialized, uses bounded DuckDB queries, and runs
under a separate process-tree memory guard with physical and commit headroom
checks. Sequential reconciliation partitions keep one child active at a time.

Item coverage, provider SLOs, complete quality results and schema-condition
changes have **not yet been measured on the rebuilt warehouse**. The first
release, full non-slow test gate and whole-branch review remain pending. A
release manifest alone cannot establish Tier-1 parity.

Operator evidence is retained in `.superpowers/sdd/tier1-parity/`:
`post-run4-inventory.json`, `ar6-source-audit.json`, `catalog-memory-report.md`
and the activation-run4 log. Later measurements must record their own run,
warehouse state and source hashes rather than overwrite these observations.
