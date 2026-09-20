# Tier-1 activation measurements

This is an interim measurement of the local production warehouse on 2026-09-20,
after activation-run4 stopped. It is not a completed parity gate or a release.
All counts below came from read-only SQL or the exact staged source file.

Latest production update: the guarded price prepass applied migrations0303-0312
and created a6,303,264,768-byte backup, retaining three older backups. Its
replacement table validated31,959,271 rows across34,251 IDs, including12,386 on
2026-09-18. Publication COMMIT then exhausted DuckDB's1GB limit and invalidated
the connection during rollback. The process exited1 with native job peak1.773GiB
under a3GiB cap; it did not exhaust host memory.

A fresh read-only recovery check confirmed all ten migrations and the original
31,178,192 price rows/34,803 IDs through2026-06-15. The operator closed the failed
dataset and activation ledgers at16:59:42UTC; that is the recovery time, not the
original failure time. Code now preserves the original exception and can reopen
an invalidated connection under its recorded budget to write failure ledgers.

Two attempts to start a2GB query-budget retry were refused before launch because
physical headroom was below the unchanged5GiB requirement. A separate SEC source
prepass then stopped before ingestion: migration0313 rejected the uncatalogued
table left by the failed price staging, and governed migration restored its
backup. The additional10,229,919,744-byte backup is preserved. After inspecting
the exact staging state and preserving its backup and audited raw source, the
operator removed only that regenerable staging table at17:05:55UTC. Original
live prices remain unchanged.

The2GB DuckDB/one-thread retry also failed at COMMIT at17:16:23UTC after389.734s.
Native job peak was2.708GiB under the same3GiB cap; the host retained headroom.
The repaired failure handler automatically recorded the failed stage. A fresh
read-only measurement at17:16:50UTC confirmed the original31,178,192 rows remain
intact and the replacement staging retains31,959,271 rows. Migration0313 did
complete, with a further preserved8,054,386,688-byte backup.

The original prices still contain1,133 invalid adjusted closes and31,178,192
nonnull split-factor values from the old mapping. They are not the corrected
source. The next repair changes the publication method to fit bounded memory;
no further budget increase is planned. Receipts are
`activation-prices-updated-2g-attempt3-memory.json` and
`updated-price-live-measurement.json`. No source/coverage threshold is relaxed,
and heavy tests or scans run sequentially with production.

Operator receipts: `activation-prices-updated-memory.json`,
`activation-prices-updated.err` and `price-recovery-readonly.json` under
`.superpowers/sdd/tier1-parity/`. The recovery receipt's backup lookup alone used
an obsolete table name; migration and price recovery queries succeeded. The
backup registry is `migration_backup_registry`.

## Activation result

Run4 loaded 3,045,440 submissions and 31,590,760 raw companyfacts rows.
The companyfacts stage reported 8,031 targets, 7,035 loaded-target outcomes and
996 failed targets; its old completion status did not enforce target success.
Loaded-target outcomes are not unique issuer counts. The next statement_points
stage failed at 15:00:49 UTC because its concept catalog materialized all facts
in pandas and exhausted memory. No later run4 stage completed.

The warehouse had migrations through0302 at the run4 measurement below; the
later guarded prepass above applied through0312. A stale running
entry from run3 is historical bookkeeping, not an active writer.

## Source and identity coverage

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

An updated user-supplied Parquet file has now been staged and fully audited,
but has not yet replaced the live prices listed above. Its32,323,644 rows span
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
adjusted close and leaves split-only factors unknown. The live prices above
still use the old mapping and require republication. See the
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
