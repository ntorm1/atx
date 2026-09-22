# TickerHistory input audit — 2026-09-19

## Input identity and measured coverage

The authorized input is `C:/Users/natha/Downloads/tbltickerhistory3_10y.zip`.
It is 3,542,506,361 bytes with SHA256
`7d2b7a615a08ef3b8686608eda384c4e30e2ce23bdee294ea141b3c9334acd8f`.
Its sole member, `tbltickerhistory3_10y.txt`, is 11,084,562,320 bytes uncompressed
(3,542,506,173 compressed), deflated, CRC32 `15b6e051`.

A complete streaming read measured **31,598,499 data rows**, **71 columns**, and
**3,576 dates from 2012-03-26 through 2026-06-15**. There were no field-count
errors or date-order reversals. There are 24,959 distinct security-ID strings,
including `0`; 351,651 rows have ID `0`, and 892 rows have an empty ticker.
The archive is substantially longer than the filename's `10y` suffix suggests.
Full-member reads completed with ZIP CRC validation; no member was extracted.

The repeatable profiler is
`C:/atx/build-equity/audits/profile_tickerhistory.py`; its detailed JSON is
`C:/atx/build-equity/audits/tbltickerhistory-input-profile.json`.
It hashes the original archive and streams 8 MiB chunks, retaining only one
date's rows for duplicate comparison plus aggregate counters. It never creates
a dense historical panel. The first coverage pass took 123.2 seconds; this is
an input-inspection timing, not an engine throughput benchmark.

## Schema and vendor evidence

The exact schema and start date match SpiderRock's
[TickerHistory3 dictionary](https://docs.spiderrockconnect.com/docs/next/HistoricalData/Data%20Dictionaries/TickerHistory3/).
The repository currently calls its adapter `orats_history`; that API name does
not establish the vendor provenance of this archive.

| Source field | Vendor type | Observed/required interpretation |
| --- | --- | --- |
| `tradingDate` | date | ASCII `YYYY-MM-DD`, a session date |
| `securityID` | bigint | Numeric security identity; reject nonpositive IDs |
| `ticker_tk` | string | Row's historical ticker |
| `todayTicker` | string | Latest ticker; metadata only |
| `dn` | bigint | Vendor NMS trading-day ordinal, not epoch days |
| `open`, `high`, `low`, `close` | float | Contemporaneous prices; decimal/scientific text |
| `volume` | double | Trading volume; do not assume integer storage |
| `shares` | bigint | Shares outstanding; preserve integer source precision |
| `closePr`, `closeUnadjPr` | float | Adjusted/unadjusted **prior-day** close |
| `returnFactor`, `totalReturn` | float | Daily corporate-action factor and return |
| `cumulReturnFactor` | double | Cumulative adjustment; direction tested below |

The current dictionary lists full-history delivery at **05:00 CT T+1**.
This is neither a historical release-time record nor proof that this snapshot's
shares and revisions were known on their stated trading dates. Use an explicit
conservative replay convention; CT requires `America/Chicago` daylight-saving
rules, and the documentation does not resolve calendar-day versus trading-day
meaning of T+1. Do not equate midnight session keys with information availability.

## Adjustment direction: observed, not guessed

In the first 300,000 rows, `cumul[t] / cumul[previous]` agrees with
`1 / returnFactor[t]` within 1.293e-7 across 293,119 comparable pairs.
The direct-factor alternative does not agree. The supported adjusted-price
construction is therefore **same-row raw price multiplied by cumulReturnFactor**.
The following source observations are independent numerical oracles:

| AAPL date | Raw close | Cumulative factor | Daily factor |
| --- | ---: | ---: | ---: |
| 2012-08-08 | 620.1 | 0.0299354214754423 | 1 |
| 2012-08-09 | 620.51 | 0.0300639455623688 | 0.995725 |
| 2014-06-06 | 645.57 | 0.0313192321659319 | 1 |
| 2014-06-09 | 93.7 | 0.219234844040724 | 0.142857 |
| 2020-08-28 | 499.23 | 0.242493598431242 | 1 |
| 2020-08-31 | 129.04 | 0.96997439372497 | 0.25 |

The 2014 factor increases approximately sevenfold when raw prices split.
Apple independently confirms the [June 9 seven-for-one split](https://investor.apple.com/faq/).
The 2012 event adjusts prior close from 620.1 to 617.449, consistent with
Apple's announced [$2.65 dividend](https://www.apple.com/newsroom/2012/07/24Apple-Reports-Third-Quarter-Results/).
Thus the cumulative factor includes cash distributions as well as splits.

Consequences for the adapter:

- Apply one valid, finite, positive same-row factor consistently to canonical
  research OHLC. Preserve raw close for price screens, dollar volume, valuation,
  and execution. Never combine raw open/high/low with adjusted close.
- Preserve raw volume and raw shares. A factor containing dividends is not a
  split-only share/volume adjustment. Do not divide these fields by it.
- Do not add cash dividends again to returns already represented by this factor.
- A future common rescaling cancels in an individual security's historical
  returns. This does not justify arbitrary absolute adjusted-price thresholds,
  cross-security price-level comparisons, or adjusted-price dollar-volume screens.
- Factor positivity and finite arithmetic are necessary, not sufficient, source
  quality checks. These factors are not a corporate-action event ledger and do
  not provide historical revision vintages.

## Observed source quality defects

The first 100,000 rows contain 130 negative low prices, 148 zero-share values,
and two zero volumes. For example, ACFC on 2012-03-26 has open/high/close 2.34
but low -2.4; DISCB has low -49. No evidence supports taking absolute values or
silently manufacturing a replacement low. Zero volume can be legitimate;
zero shares must not create a valid market-cap observation.

The first 300,000 rows contain 933 duplicate `(date, ID=0)` keys and two
conflicting positive-ID duplicates. Both positive collisions use ID 77622:

| Date | First ticker/close | Second ticker/close |
| --- | --- | --- |
| 2012-04-09 | MIL / 7.578 | TTT / 23.3 |
| 2012-05-03 | MIL / 7.22 | TTT / 22.03 |

Many unrelated securities share ID `0`, so treating it as one security conflates
instruments. The source dictionary does not explicitly define the zero sentinel;
its unusability as a unique key is measured directly. Positive conflicting keys
must fail the selected ingestion interval rather than select a row by input order.
The detailed full profile records positive collisions by date and ID so a bounded
acceptance interval can be chosen without discarding arbitrary conflicting rows.
The complete scan found **489 positive duplicate rows across 267 dates and 300
IDs; all 489 differ from the preceding row with that key**. No negative or
nonnumeric IDs were observed; the largest numeric ID is 1,001,001,001,070 and
fits signed 64-bit storage. The tracked summary is
[`2026-09-19-tbltickerhistory-input-profile.json`](2026-09-19-tbltickerhistory-input-profile.json).

Daily-return fields can disagree with the price/factor series even after
excluding ID zero and requiring adjacent `dn` values. In the initial sample,
52 of 286,855 such pairs differ by more than 0.001. Example: PGN, ID 41613,
2012-03-27 has prior observed close 52.87 and current close 53.09 but `closePr`
0.165, `totalReturn` 320.757563896854, and both adjustment factors equal to 1.
The identity `close / closePr - 1` matches that bad return; agreement between
those two fields alone is therefore insufficient validation.

Across all 3,576 observations of each AAPL/MSFT/SPY/TSLA series, adjusted-close
returns generally match `totalReturn`, but each has two differences above 1e-6;
maximum absolute differences are approximately 0.0004214, 0.0006306, 0.00007847,
and 0.0002402 respectively. Do not enforce an unrealistically exact equality
or claim the whole archive passes a return-identity gate.

Shares can lag corporate-action state: AAPL reports 861,381,000 shares on both
2014-06-06 and the seven-for-one split session 2014-06-09. Preserve that fact;
do not assert fully synchronized market capitalization or historical filing
availability from the bare `shares` column.

## Existing adapter and next real-data checkpoint

Reuse `load_orats_history` in `src/data/orats_history.cpp` and
`build_history_panel` in `src/data/history_panel.cpp`. The loader already reads
this ZIP/TSV format and stores `cumulReturnFactor` under segment field
`cumReturnFactor` to meet the segment-name limit. No new parallel loader is needed.

The proposed 2026-05-01 through 2026-06-15 interval **fails** the positive-key
uniqueness prerequisite: May 4 has one conflict; May 18 four; May 26 two;
May 27 two; June 1 one; and June 8 six. The collision-free tail June 9–15 has
only five sessions, insufficient for 21-session liquidity warmup.

The implemented preparation policy quarantines **every row of a duplicate
positive `(date, securityID)` key**, with no first/last winner. A security's
nonconflicting dates remain eligible; the rejected session is a gap. This does
not resolve vendor identity or justify a full-universe performance claim.
The separate `conflict_fixture.zip` contains the original header and all original
rows of the first duplicate key in the chosen window, allowing the native
loader's rejection to be verified without another full-history scan.

The April 1–June 15 numeric profile includes 626,602 rows, 38,518 zero-share
observations, 383 zero volumes, two zero opens, and 4,336 OHLC ordering violations
among finite positive prices. These are source-quality observations; the current
adapter corrections do not constitute a complete OHLC consistency validator.

Strict date/positive-ID parsing, duplicate
rejection, coherent OHLC adjustment, quality counters, and raw economic units
must be reviewed before interpreting any strategy result. Original ZIP bytes
remain untouched; derived data stays in ignored `C:/atx/data`.

This audit performs source inspection and numerical analysis. It does not claim
a successful native ingestion, point-in-time certification, or tradable P&L.

## Reproducible preparation

The stdlib-only tool is [`prepare_tickerhistory.py`](../tools/prepare_tickerhistory.py).
Run from the repository root:

```powershell
python atx-engine/tools/prepare_tickerhistory.py `
  --source C:/Users/natha/Downloads/tbltickerhistory3_10y.zip `
  --out-dir C:/atx/data/tickerhistory_20260501_20260615_20260919 `
  --start-date 2026-05-01 --end-date 2026-06-15
```

The destination must not already exist. Accepted rows retain the original header,
columns, numeric strings, ordering, and line bytes. Rejection rules cover invalid
or nonpositive IDs; duplicate keys; nonfinite/nonpositive OHLC or inconsistent
OHLC ordering; invalid cumulative factors or nonrepresentable adjusted products;
and invalid or negative volume. Zero volume is permitted and counted. Reported
shares are never repaired; zero or malformed shares are separately flagged.
Raw text preserves the reported integer digits; the existing native segment
projection uses `f64` and does not guarantee exact representation of every
possible 64-bit shares value.
Numeric syntax is restricted to match native parsing, including rejection of
underflow-to-zero rather than silently accepting it as zero volume.

The manifest separates exclusive primary reasons from overlapping reason flags,
includes bounded examples and daily counts, pins source/output SHA256 digests,
and identifies the conflict fixture as rejected data. Source bytes are hashed
before and after processing; ZIP member CRC is checked by the completed read.
Output ZIP metadata is deterministic. A completed `manifest.json` is the
publication marker. Failure leaves diagnostic `failure.json` and partial files;
those paths must not be consumed or overwritten by a rerun.

Three small fixture tests passed using
`python -m unittest discover -s atx-engine/tests/tools -p test_prepare_tickerhistory.py -v`.
They check all-row duplicate quarantine, untouched accepted bytes, repeatable
ZIP hashes, source immutability, numeric bounds, and failure/output safeguards.
The quality policy intentionally does **not** validate economic return-field
consistency, historical shares revisions, or original observation availability.

### Completed preparation measurement

The May 1–June 15 preparation completed with **375,746 selected rows**, of which
**372,014 were accepted** and **3,732 rejected**. Exclusive primary reasons were
1,916 invalid/nonpositive IDs, 32 rows from 16 duplicate positive keys, one invalid
OHLC row, and 1,783 OHLC-order violations. Counting overlapping reasons finds
2,488 ordering violations because some rows also fail identity checks.
Accepted rows retain 20,829 zero-share observations and 71 zero volumes, both
explicitly flagged rather than repaired.

`accepted.zip` is 44,080,207 bytes (138,281,873 uncompressed) with SHA256
`907d877b4bbd11db6dc6d86bf5fc6cad92fc891fdadff16adfeb9ba6daa85c07`.
`conflict_fixture.zip` contains the two original rows for ID 6459818 on 2026-05-04;
its SHA256 is `84f836f630f2aedc27e1096db14ed4af356503f24f4065321ae45c9d7d3d7bbb`.
Both output ZIPs passed an independent CRC read after publication.
The original archive's SHA256 remained unchanged across preparation.

The full manifest is
`C:/atx/data/tickerhistory_20260501_20260615_20260919/manifest.json`.
The tracked receipt, including policy, counts, tool/manifest hashes and limitations,
is [`2026-09-19-tbltickerhistory-preparation.json`](2026-09-19-tbltickerhistory-preparation.json).
These measurements establish the preparation result; native loader and panel
measurements are recorded separately by the build owner.

### Identified ingestion and panel checkpoint

The native `load` stage now accepts `--preparation-manifest` and reserves an
absent or empty output directory. It validates the supported complete preparation
report against the supplied ZIP filename, byte size, SHA256, row-count identities,
and date window. It hashes the ZIP before and after loading and checks that the
number of parsed source rows equals the preparation report's accepted count.
Zero outside-window/rejection counters may be absent because the preparation
tool serializes a sparse Python `Counter`.

Successful loading publishes `_ingestion.manifest.json` after hashing every
generated segment. This receipt binds the actual accepted input, preparation
report, declared original archive hash, resolved loader recipe and executable
hash. Failed loads retain a pending marker or partial receipt; they cannot be
silently reused as completed ingestion output. Preparation and ingestion JSON
readers reject duplicate object keys, nesting beyond 64 levels, and files larger
than 16 MiB.

The native `panel` stage captures the selected segment hashes before assembly,
checks the exact engine-selected paths and hashes afterward, and validates their
bindings against any ingestion receipt. A supplied preparation report must match
that receipt; it cannot assert a link to arbitrary segments on its own. Legacy
segments without a receipt can still produce an identified panel, with exact
segment parents and explicitly unknown upstream provenance. Pending ingestion
output is rejected even when no preparation report is supplied.

The required `<panel>.manifest.json` stores actual ordered session labels,
canonical positive `securityID` strings, original column indices, and the final
numeric payload hash. IDs above binary64's exact-integer range remain strings.
Compaction and optional field augmentation preserve those axes. The resolved
recipe records the universe's actual 21-bar ADV default separately from optional
augmentation windows, adjusted-price/raw-volume economics, current-session input
use, and the unknown availability and instrument-type eligibility limits. Paths
and wall-clock `.meta.txt` fields do not enter the stage recipe. Exact upstream
manifest hashes still bind all bytes of those reports, including their metadata.

This is local content provenance, not signed vendor authentication or verified
historical publication. The original archive hash is a declaration in the bound
preparation report; loading the accepted subset does not re-read the original
3.5 GB archive. The preparation policy still does not validate economic return
identities or historical shares revisions. `--incremental-panel` is explicitly
rejected until an API can verify the old axes and write a distinct output.

All ten focused `AtxImplDataProvenance` fixtures passed in checkpoint 4. The native
loader also verified the preparation binding and ingested all 372,014 accepted
rows; independently checked panel builds preserved the source-derived date/ID
mapping and checkpoint 3 numeric bytes. See the
[validation receipt](2026-09-19-panel-identity-validation.json) for exact artifacts
and the broader 93-pass, one-skip gate. The design distinguishes inputs,
processing, and derivations following
the [W3C provenance model](https://www.w3.org/TR/prov-overview/). Recipe identity
uses exact versioned serialization bytes, without claiming compliance with
[RFC 8785 canonical JSON](https://www.rfc-editor.org/rfc/rfc8785).
