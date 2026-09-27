# Task T6 report: PIT research-field producer aligned to role axes

2026-09-26. Worktree `C:/atx-wt/pool-8`, branch `feat/mega-alpha-fields-20260926`, base `bb5bc25b`.
**Commit `cf36d83c`** (not pushed). The only changes are two new files:

- `atx-engine/tools/prepare_research_fields.py`, sha256 `b506bd81243ca8fdf628985fcc6e1154d3e1b311afaafa3e0428560678c1075f` (LF working copy)
- `atx-engine/tools/test_prepare_research_fields.py`, sha256 `69fe5029d78ce080e90cfcb8db4d33253a31758f1028372710ca89dce831b415`

No existing file was edited. I did not run the producer on real sources. Real-source reads were bounded (listed below). Root's `mkt_ret` addendum is included.

## What it produces

`--output DIR` must be a new directory. The tool refuses an existing one, even if empty.

For each selected field it writes `<name>.f64`: little-endian f64, date-major, shape = role dates x role ids. It uses the role's own `sessions.i64` / `ids.u64` exactly and does no re-projection. A cell is NaN wherever the value is not visible at that session's decision, or the source row is absent.

`manifest.json` is written last and exclusively (pending file + `os.link`). Its schema is `atx.research-role-fields/v1` and it records:

- **Role pins:** role `manifest_sha256`, `sessions_sha256`, `ids_sha256`, `member_sha256`. All are checked against the role manifest, and the role is re-pinned at the end of the run.
- **Per-field details:** file, dtype, layout, shape, units, clock, staleness rule, source columns, and sources (resolved path + bytes + sha256). Also caveats, and `definition` / `stats` where they apply.
- **Per-field coverage over member cells:**
  - finite member cells / member cells, per year and for the score window
  - finite cells overall
  - min/max/mean of the finite member values
- **Per-file bytes and sha256.**
- **Other entries:** `excluded_source_columns` (the dropped columns with evidence), `source_checks` (row counts, 2025-dropped rows, unknown ids, duplicates), `code_sha256`, and the seal.

The manifest contains no wall-clock values, so identical inputs give byte-identical output. The fixture test checks this, manifest included.

CLI: `--role DIR --role-sha256 SHA --output DIR [--fields a,b,...] [--max-rss-mib N]`.

Optional arguments and their defaults:

| Argument | Default |
|---|---|
| `--max-seconds` | 1800 |
| `--finra` | `C:/atx/data/finra_short_interest` |
| `--tickerhistory` | `C:/Users/natha/Downloads/TickerHistory3.parquet` |
| `--lake` | `C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23` |

## Fields: exact source columns, clocks, staleness

| Field | Source (exact) | Clock | Staleness / NaN rule | Units |
|---|---|---|---|---|
| `si_shares` | `finra_short_interest/asof/si_shares.csv` `value` | Latest row with `available_at < date(session)` **strictly**. Replicates `atx-impl/src/asof_field.cpp` `build_asof_column`, including the parse contract, the duplicate refusal, and "visible NaN stays NaN" | age = date(s) − available_at > **45** calendar days → NaN; NaN before the first visible row | Shares short |
| `si_dtc` | `asof/si_dtc.csv` `value` | Same as `si_shares` | Same as `si_shares` | Days to cover (FINRA floors at 1.00) |
| `iv_atm_21d` / `_63d` / `_126d` | TickerHistory3 `atmCenI_21d` / `atmCenI_63d` / `atmCenI_126d` (f32 widened) | The vendor row for date d is the end-of-day mark known at d 22:00 UTC (the role's close clock) | Same date only, no fill; null, non-finite or ≤ 0 → NaN | Annualized decimal IV |
| `earn_recent` | TickerHistory3 `earnFlag` | Same date | `0`, `1` → 1; `N`, `-1` → 0; null or unexpected → NaN | Indicator |
| `shares_out` | TickerHistory3 `shares` (thousands ×1000), `cumulReturnFactor` | House A8 rule: vendor shares of the line's last row dated ≤ date(s) − 90 calendar days (0 < shares ≤ 1e8, the A9 ceiling), restated by crf(session row)/crf(lag row) | Lag row older than 90+400 days → NaN; no clean same-date factor → NaN; any row above the A9 ceiling in the scanned window withholds the whole line (C-81) | Shares |
| `mktcap_lagged` | lake `spine_monthly` `me_line` | Row of the latest monthly `formation_date` **strictly before** date(s) | One formation only: a line absent at that formation, or with NULL `me_line`, → NaN | USD |
| `size_grp` | `spine_monthly` `size_grp` | Same as `mktcap_lagged` | NULL → NaN | micro=0, small=1, large=2, mega=3 |
| `is_common` | lake `line_types` `security_type` | Static, **not PIT** | NaN only if the line has no `line_types` row | 1 = `common` / `common_unverified` (the research universe's eligible types), 0 = any other type |
| `mkt_ret` | Role `close.f64`, `raw_close.f64`, `present.u8`, `member.u8` | Row d uses the closes at d−1 and d plus decision membership at d−1 | Row 0 NaN; NaN where `present[d,i]==0`; NaN when no name contributes | Simple return (decimal) |

`mkt_ret` definition, per root's addendum:

- Row d ≥ 1 is the equal-weight mean (`math.fsum` / k) of `close[d]/close[d-1]-1`.
- The mean covers names with `member[d-1]==1`, both endpoints present, finite positive close and raw_close at both endpoints, and not guarded.
- The guard is copied exactly from `strategy_target_replay.cpp` `rough_return` (~:170): a non-finite r, or |log adj| > 1.5, or |log adj| > |log raw| + 0.10.
- The scalar is written into every present cell of row d.
- The manifest records contributors per session (min, median, max, zero-contributor sessions, score-window min and median), the count of guarded member intervals, and the file sha.
- The role's close, raw_close and present are streamed row by row. The exact bytes used are hashed and must match the role manifest, or the run fails unpublished.

Everything available on or after 2025-01-01 is excluded:

- Role sessions are asserted to be before 2025-01-01, and a role reaching 2025 is refused before the output directory is created.
- FINRA rows with `available_at` ≥ 2025 are dropped and counted.
- TickerHistory rows are filtered to [first session − 490 days, last session].
- Spine years ≥ 2025 are never read.

## Semantics evidence

All of this comes from bounded samples; the numbers are the ones I measured.

- **FINRA**
  - Header and format: LF endings and a single trailing newline, checked on the 80-byte tail.
  - A 2 MB prefix of each CSV parses under the strict contract: 85,861 / 96,852 rows, sorted by (id, date).
  - Every `available_at` in those prefixes is an official `dissemination_date` from `dissemination_schedule.csv`. The tool enforces this on every row, and aborts otherwise.
  - Each CSV is pinned to the producer receipt `asof/manifest.json` (si_shares `dfc99b06…`, si_dtc `fce20b8c…`). A mismatch aborts.
  - Vintage cutoff: rows disseminated on or before **2021-06-09** (settlement before 2021-06-01) are the later FINRA republication. The manifest counts the finite member cells sourced from them (`coverage.vintage_risk`).
  - The producer's `mapping_report.json` shows `dtc_exactly_1` = 779,211 of about 1.97M rows. This is FINRA's floor and is recorded as a caveat.
- **TickerHistory3 footer:** 32,323,644 rows, 262 row groups, and every group spans 2012-03-26..2026-09-18. Column types are asserted.
  - The file sha must equal the role's `source_sha256` (`0ed96b2696f1…`), so the calendar and ids are the ones the role was built from. The Downloads copy has the same size (3,617,973,507 bytes) as the retained staging copy.
- **`earnFlag`:** the domain is {N, −1, 0, 1}.
  - Row groups 3 and 7 contain 896 and 838 clean −1/0/1 triplets.
  - `0` is the price-reaction session: median |adjusted return| 3.9% / 4.2% at offset 0, against about 1.2–1.4% on neighbouring days. So the event is public by that close.
  - `-1` presumes a known future date, so it is mapped to 0 and cannot be told apart from N.
- **`nEarnCnt_Nd` is forward-looking.** Examples CERS 2021-02 / 2021-05 and CTBI 2021-01 / 2021-04: `nEarnCnt_5d` is 1 on the 5 sessions up to and including earnFlag −1, then 0 from the reaction session on. It is excluded. `nEarnCnt` (no suffix) is typically 8 and 7 on some event days; its semantics are unverified, so it is excluded.
- **`atmCenH_*` is not realized historical vol, so `hv_*` is dropped.**
  - H/I has p10 = p50 = p90 = 1.000 exactly whenever `nEarnCnt_<tenor>==0` (n = 60,493 / 48,408 / 18,213 for 5d / 21d / 63d). It differs only when an earnings event falls inside the tenor.
  - The log correlation of `atmCenH_21d` with `atmCenI_21d` is 0.948.
  - It is an alternate earnings adjustment of the same IV, and depends on the forward calendar and the `*EMove` family, which the ORATS loader design excludes as look-ahead.
  - Realized vol can be computed from close in the DSL.
- **`atmCenI_*`:** the implied-vol crush falls on the earnFlag-0 reaction session, which is consistent with same-day option data and no forward shift.
  - Median 5d IV rises only about 3% into events, which suggests the vendor has cleaned out the earnings effect using its calendar. That calendar's vintage is unproven, and this is recorded as a caveat.
  - Nulls are about 47–56% of all rows, but about 6.5% of rows with volume > 1e6 (2021+ sample).
- **`shares` units are thousands:** DLO 294,931 ≈ 295M shares, CTBI 17,810 ≈ 17.8M, and the spine's `shares_lagged` is vendor ×1000.
  - The A8 lag (90 days) and A9 ceiling (1e8) are copied from `atx_db.research.spine` and `migrations.bodies_0327`. The lag is used because vendor share runs start at the filing cover date, about 2 weeks before the filing is public.
  - Restatement uses q = shares/crf, then q × crf(session) × 1000. This is the spine's factor ratio.
- **Duplicate keys:** in a sample row group all duplicate (date, securityID) keys were id 0, which is never on the axis. Any on-axis duplicate is quarantined to NaN for every vendor field, as in the role producer.
- **Lake**
  - The actual path is `C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23`. The brief's `C:/atx/data/research/lake/...` does not exist.
  - Every file read is hashed from the exact bytes parsed and compared with the lake `_manifest.json`. The manifest is re-hashed at the end; any change aborts.
  - Formations must be month-contiguous. Formation = the month's last session, known at 22:00 per the spine docstring; the tool applies a strict `<`, as the brief asks.
  - Real 2023 spine and line_types pins verified.
  - `line_types` covers 5627/5627 train ids and 5048/5048 validation ids.
  - Train type mix: common 2556, common_unverified 1479, ETF 956, unknown 348, ADR 178, others small.

## Deviations from the brief

1. **`hv_<tenor>` dropped.** See the `atmCenH_*` evidence above.
2. **`shares_out` uses the house A8 90-day lag and restatement, not the same-day vendor value.** The same-day value would leak about 2 weeks of filing information.
3. **`size_grp` added** as an ordinal field from the same spine rows. It is cheap, and the NYSE breakpoints cannot be derived from the panel.
4. **`is_common` is a static classification.** It is flagged non-PIT in the manifest.
5. **Lake path corrected** to the real location.
6. **`mkt_ret` added** per root's addendum.

## Tests

`& 'C:/Program Files/Python312/python.exe' -m unittest discover -s atx-engine/tools -p test_prepare_research_fields.py -v`

- **Result:** 9/9 pass in 1.05 s, exit 0. Log sha256 `0d8a1e920be57bba9216d9cf5233b990e7bd0df102d07542303acf39a8100dfe` (scratchpad `t6-tests.log`).
- **Fixture:** a tiny synthetic role (66 sessions x 5 ids), a synthetic vendor parquet with dates mixed across row groups plus 2025 rows, synthetic FINRA CSVs with a receipt and schedule, and a synthetic lake.
- **What the tests cover:**
  - FINRA: strict `available_at < session` (not visible on the publication day, visible the next session); 45-day staleness (age 45 visible, age 46 NaN); visible NaN does not skip back; unknown ids ignored.
  - 2025 exclusion: the role assert, plus dropped and skipped counts.
  - Axis alignment of every field.
  - Vendor fields: duplicate-key quarantine; off-calendar rows; earnFlag mapping; A8 lag, split restatement, the 490-day age limit and the C-81 withheld line.
  - Spine: strict formation (the formation session itself still sees the previous formation); `is_common`.
  - Exclusive output, including an empty existing directory.
  - Refusals: wrong `--role-sha256`, a role/TickerHistory sha mismatch, a lake tamper, a FINRA receipt mismatch, a role `close.f64` tamper; each leaves no manifest.
  - Strict CSV contract cases.
  - Manifest hashes, and byte-identical rerun determinism.
  - CLI subset.
  - `mkt_ret`: exact equality with an independent loop implementation, one scalar per session, both guard paths, the member-at-d−1 rule, present-only broadcast, and the stats.
- **Mutation checks:** 15 mutations, all killed (the first version of the exclusive test missed one; strengthened). They covered:
  - strict `<` turned into `<=`
  - the staleness limit moved to 46
  - duplicate quarantine removed
  - earnFlag −1 mapped to 1
  - the shares lag, the restatement and the C-81 rule removed
  - the spine strict `<`
  - the seal assert
  - exclusive mkdir
  - the `mkt_ret` 1.5 guard, raw guard, member row and present-only broadcast
  - the role hash check
- **Full-scale synthetic measurement** (scratchpad, all 11 fields): train shape 1155 x 5627, 8.47M vendor rows (240 MB parquet), 2 x 607,500 FINRA rows, 6 spine years. **13.0 s wall, peak working set 405 MiB.** The admission estimate fired correctly, and the matrices dominate the peak.

## Bounded real-source reads performed (no producer run)

- TickerHistory3: the footer; row groups 0, 3, 5, 7, 10 and 11, a handful of columns each (a few MB).
- FINRA: 2 MB prefixes and 80-byte tails of the two CSVs; `dissemination_schedule.csv`, `asof/manifest.json`, `mapping_report.json` and `README.md`.
- Lake: `_manifest.json`, `spine_monthly/year=2020` (1.6 MB) and `year=2023`, and `line_types` (0.56 MB).
- Role: the manifests, ids and sessions of both roles.
- I wrote nothing outside pool-8, this report and the scratchpad.

## Expected runtime and RAM on real data

- The TickerHistory phase hashes 3.62 GB and scans 32.3M rows (262 groups x 8 columns, about 1 MB compressed per group). Decode of 2 groups took 0.03 s warm.
- Estimate: **about 1–3 min per role, mostly hashing and I/O.**
- Peak RSS estimate: about 420–470 MiB for TRAIN, less for validation. The dense matrices for TRAIN are:
  - counts: 1645x5627 u16
  - 3 IV fields: 1155x5627 f32
  - earn: i8
  - q: 1645x5627 f64
  - crf: 1155x5627 f64
  - total about 230 MB, on top of a baseline of about 175 MB
- The FINRA phase peaks at about 190 MiB and runs first. `mkt_ret` streams one row at a time.
- `--max-rss-mib 700` is enforced by admission and by polling. It is cooperative, not a kernel limit, so root keeps its own monitor.

## Commands for root (not executed)

Each output directory must not already exist. Run them one at a time.

```powershell
& 'C:/Program Files/Python312/python.exe' C:/atx-wt/pool-8/atx-engine/tools/prepare_research_fields.py --role C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v1 --role-sha256 3f53ee9aa1b674d3f5022cbb22d40e5c043e7add8c9422cd2456299ce3662493 --output C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v1-fields-v1 --finra C:/atx/data/finra_short_interest --tickerhistory C:/Users/natha/Downloads/TickerHistory3.parquet --lake C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23 --max-rss-mib 700 --max-seconds 1800

& 'C:/Program Files/Python312/python.exe' C:/atx-wt/pool-8/atx-engine/tools/prepare_research_fields.py --role C:/atx-wt/pool-2/build-equity/recent-fast-validation-2023-2024-v1 --role-sha256 0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7 --output C:/atx-wt/pool-2/build-equity/recent-fast-validation-2023-2024-v1-fields-v1 --finra C:/atx/data/finra_short_interest --tickerhistory C:/Users/natha/Downloads/TickerHistory3.parquet --lake C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23 --max-rss-mib 700 --max-seconds 1800
```

- The role sha values are the sha256 of each role's `manifest.json` as of today.
- To run only `mkt_ret`, which needs no external source and takes seconds, add `--fields mkt_ret`.
- A failed run leaves a partial directory with no `manifest.json`. Consumers must require the manifest. Use a fresh directory to retry.

**For the T7 runner:**

- Require `status == complete`.
- Require `role.manifest_sha256` / `sessions_sha256` / `ids_sha256` to equal the loaded role's values.
- Load `fields[].file` with shape `[dates, instruments]` `<f8`, and check `files[file].sha256`.

## Concerns

1. **`is_common` is not point-in-time.** It uses a 2026-09-18 directory snapshot plus whole-history vendor earnings evidence, so it carries post-2025 information. Use it only as a coarse ETF/fund filter, or leave it out with `--fields`.
2. **The spine's universe gate uses a forward-looking count.** It relies on `first_earn_date` from `nEarnCnt_504d > 0`. That can leak mild survival information into whether `mktcap_lagged` / `size_grp` are present, though not into their values.
3. **Vendor vintage is unproven for IV and earnings.** `iv_atm_*` looks cleaned using the vendor earnings calendar, and `earn_recent` depends on the same calendar.
4. **FINRA data before June 2021 is a later republication, and `si_dtc` is floored at 1.**
5. **`shares_out` restates through the raw `cumulReturnFactor`,** not the VA1-repaired factor. Rare vendor factor artifacts are not detected.
6. **The full FINRA CSVs were not verified by me** (sha and dissemination-date checks cover only the 2 MB prefix), because of the read limits. Both are enforced at run time and the FINRA phase runs first, so a mismatch fails within seconds.
7. **`code_sha256` in the manifest hashes the executed file's bytes.** A CRLF checkout changes it.
8. **`mkt_ret` includes ETFs,** because it uses the role's ADV top-N membership.
