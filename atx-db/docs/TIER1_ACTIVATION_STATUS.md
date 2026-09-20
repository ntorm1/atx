# Tier-1 activation measurements

This is an interim measurement of the local production warehouse on 2026-09-20,
after activation-run4 stopped. It is not a completed parity gate or a release.
All counts below came from read-only SQL or the exact staged source file.

Latest production update: **corrected prices were published successfully** by
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

## Activation result

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
