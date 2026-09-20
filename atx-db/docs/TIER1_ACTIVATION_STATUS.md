# Tier-1 activation measurements

This is an interim measurement of the local production warehouse on 2026-09-20,
after activation-run4 stopped. It is not a completed parity gate or a release.
All counts below came from read-only SQL or the exact staged source file.

## Activation result

Run4 loaded 3,045,440 submissions and 31,590,760 raw companyfacts rows.
The companyfacts stage reported 8,031 targets, 7,035 loaded-target outcomes and
996 failed targets; its old completion status did not enforce target success.
Loaded-target outcomes are not unique issuer counts. The next statement_points
stage failed at 15:00:49 UTC because its concept catalog materialized all facts
in pandas and exhausted memory. No later run4 stage completed.

The warehouse had migrations through 0302 at measurement time. Subsequent code
and migrations have not yet been applied to this warehouse. A stale running
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
1,128,935,424 bytes. This validates that query only. Facade integration, review,
and later-stage memory work remain necessary before restarting the ladder.

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
