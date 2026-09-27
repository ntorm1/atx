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

## Fix round 1

2026-09-27. Owner: t6-fix (fresh implementer). Worktree `C:/atx-wt/pool-8`, branch
`feat/mega-alpha-fields-fix1-20260927` at root `71cbec8f`. **Commit `a0d9deeb`** (not pushed), which
touches the same two files only:

- `atx-engine/tools/prepare_research_fields.py`: sha256 `1b474d73f80c21fa9f3d122ee62ca85ef0aa3bddd3a984c67b744bbae508a03c` (LF), git blob `482da7bb16b8ec7a5e77e5b4c9bc657e1ad291df`
- `atx-engine/tools/test_prepare_research_fields.py`: sha256 `9f982c6f17df5ed33c13e4a64f93a51cfbb30533765d7331f5b0c28226e480cc`

I built nothing and ran nothing on real data. The only real-data reads were the two role `manifest.json`
files, which I hashed. Their pins have not changed.

### Important 1: IV plausibility domain (fixed)

- **The domain is declared in code.** `IV_DOMAIN = (0.02, 5.0)` is annualized decimal with both bounds
  inclusive. It is attached to each `iv_atm_*` spec as `"domain"`, and a comment records the root
  ruling and that it was declared before any IV measurement.
- **The comparison is in float32**, the vendor's own precision, so a stored 0.02 counts as in-domain.
- **Out-of-domain values become NaN; nothing is clamped or rescaled.** The fixture checks that
  in-domain cells equal the vendor float32 value exactly. Previously the "null, non-finite or <= 0 ->
  NaN" rule did this job. Now null or NaN stays NaN, and every other value outside the domain,
  including <= 0 and +-inf, becomes NaN and is counted.
- **The vendor value is kept raw until the write**, and the domain is applied per role cell after
  duplicate-key quarantine. Each IV field entry gets a new `plausibility` object:
  `{min: 0.02, max: 5.0, inclusive: true, units, rule, implausible_to_nan, implausible_to_nan_member,
  below_min, above_max, member_below_min, member_above_max}`. `below_min` includes <= 0 and -inf;
  `above_max` includes +inf.
- **The old key is unchanged.** `source_checks.tickerhistory.iv_nonpositive_or_nonfinite` keeps its
  meaning: a row-level count across all tenors, taken before quarantine.
- **Coverage now shows the tails.** The review asked for this: every field's `coverage` gains
  `member_finite_quantiles` `{p0.1, p1, p50, p99, p99.9}`. They are computed from the published bytes
  in the same read that computes the file sha256, and cross-checked against the writer's
  finite-member count. For IV the min/max/mean are now post-domain, so the mean is usable again.

### Important 2: is_common not point in time (fixed, and extended to the spine fields; decision below)

- **Every field entry has a flag.** Each entry now carries `point_in_time` (bool) and
  `non_pit_aspects` (a list drawn from `values` / `presence`). When the flag is false it also carries
  `point_in_time_reason`.
- **New top-level keys:**
  - `point_in_time_definition`: true only when both the cell values and which cells are NaN use only
    information available by the decision. Revision vintage is reported separately and does not set
    the flag.
  - `non_point_in_time_fields`: the list of false fields.
  - `visibility_mark`: every finite cell is known by the session 22:00 UTC mark, before the 23:00
    decision. This answers the review's `available_at_ns` note for T10.
- **`is_common`** is now `point_in_time: false`, `non_pit_aspects: ["values"]`. Its reason: it is
  classified from the 2026 directory snapshot plus whole-history earnings evidence, so it encodes
  survival.
- **DECISION: `mktcap_lagged` and `size_grp` are also flagged `point_in_time: false`, with
  `non_pit_aspects: ["presence"]`.**
  - Their values are PIT: a strict formation clock and A8-lagged shares.
  - Which lines have a value is not. It is the spine universe (`spine.py` `classify_lines`
    :810-853, `stage_spine` :964-973): an eligible type taken from the 2026-09-18 directory snapshot,
    admitted from `first_earn_date = min(trade_date) FILTER (WHERE nEarnCnt_504d > 0)` (:878).
    `nEarnCnt_*` counts FUTURE events (T6 evidence), so a line enters up to 504 sessions before its
    first actual earnings, and lines that die before ever reporting are never present.
  - NaN versus finite therefore leaks survival. It also nearly reproduces `is_common`: the review
    measured member coverage 0.751 vs 0.753. Flagging only `is_common` would have left the same
    information reachable through `isnan(mktcap_lagged)`.
- **Default field list.** `DEFAULT_FIELDS` is now the PIT fields only: si_shares, si_dtc,
  iv_atm_21d/63d/126d, earn_recent, shares_out, mkt_ret.
  - `is_common`, `mktcap_lagged` and `size_grp` are opt-in: they are produced only when named in
    `--fields`.
  - The rerun commands below name `mktcap_lagged,size_grp` explicitly, because T10 loads
    `mktcap_lagged`. `is_common` is left out.
  - **Root ruling needed:** whether T7 or T10 may consume the two presence-non-PIT fields, and how.
    - T10 falls back to `shares_out*raw_close` when `mktcap_lagged` is missing. Using that for every
      name removes the leak at the cost of the spine's formation-date value.
    - For T8, the brief already prefers `shares_out*raw_close`, which is PIT.
  - If root prefers the old default set, it can list the fields explicitly. The flags stay honest
    either way.
- **`shares_out` is flagged PIT** after the Minor 4 fix below. `mkt_ret`, `si_*`, `iv_*` and
  `earn_recent` are PIT on their clocks; their vintage caveats are unchanged.

### Minors

| # | Status | Change / reason |
|---|---|---|
| M1 FINRA republication not machine-maskable | **Fixed (structured).** Optional masking flag not added | FINRA entries get `vintage_safe_from` (the first `available_at` of original-vintage rows; real schedule gives `2021-06-10`). `coverage.vintage_risk` gains `last_session_with_republished_visible_cell` and `first_session_vintage_safe`, which give a ready selection-window start. Masking flag not added: choosing the SI selection window is controller policy, and the structured keys let the fitter mask by session without another payload |
| M2 NaN meaning of `mktcap_lagged` | **Fixed** | `staleness` for `mktcap_lagged` and `size_grp` now says NaN mostly means outside the spine universe (not "large", not a data gap, about 25-27% of member cells), and names the `shares_out x raw_close` fallback. Also covered by the PIT flag above |
| M3 `shares_out` restatement has no artifact guard (C-79) | **Not fixed** | A plausibility bound on restated shares or on the factor ratio is a new policy threshold. Root should declare it before measurement, as it did for IV, rather than have me pick it after seeing the 20-share / 3.1e10 extremes. The new `member_finite_quantiles` now show those tails in the manifest |
| M4 C-81 withholding uses rows after the session | **Fixed** | A line is withheld from the date of its first above-ceiling vendor row onward (`first_above <= date(session)`; that row is known at its 22:00 mark), not over its whole history. This deviates from the spine's whole-line C-81, which is stated in `staleness`. `shares_lines_withheld_c81` keeps its meaning; `shares_cells_withheld_c81` is new. Real effect: 1 VAL line, 0 TRAIN lines (per the review) |
| M5 no bound on spine formation age | **Fixed** | Refuses before writing when any session's latest formation is more than 35 days old; consecutive month-end sessions are at most about 33-34 days apart. Records `source_checks.lake.spine_max_formation_age_days`, and also refuses a lake with no formation at all |
| M6 `code_sha256` depends on line endings | **Fixed (additive)** | `code_sha256` keeps its raw-bytes meaning. Adds `code_sha256_lf` (LF-normalised) and `code_git_blob_sha1`. The latter equals `git hash-object` and `git rev-parse HEAD:<path>` (`482da7bb...`, checked) |

### Compatibility (`atx.research-role-fields/v1` kept)

- **Nothing removed or renamed.** The schema string, `status`, `role.*` pins, `fields[].{name,file,dtype,layout,shape,sha256,...}` and `files[file].{bytes,sha256}` are unchanged, so the T7 and T10 loaders are unaffected.
- **Additive keys:**
  - `fields[].point_in_time`, `non_pit_aspects`, `point_in_time_reason`
  - `fields[].plausibility` (IV fields)
  - `fields[].vintage_safe_from` (FINRA fields)
  - `coverage.member_finite_quantiles`
  - `coverage.vintage_risk.{last_session_with_republished_visible_cell, first_session_vintage_safe}`
  - `source_checks.tickerhistory.shares_cells_withheld_c81`
  - `source_checks.lake.spine_max_formation_age_days`
  - top level: `point_in_time_definition`, `non_point_in_time_fields`, `visibility_mark`, `code_sha256_lf`, `code_git_blob_sha1`
- **Behaviour changes, deliberate:**
  - The default `--fields` is PIT only.
  - IV cells outside [0.02, 5.0] are NaN.
  - `shares_out` C-81 is applied point in time.
  - A new refusal fires on a stale spine formation.
  - Every field sha changes for the IV fields and for any role whose `shares_out` had a mid-window C-81 line. All shas change anyway because `-fields-v2` is a fresh run.
- **For T7:** refuse `point_in_time == false` fields for DSL panels unless root explicitly allows them. A library that references `is_common` will fail at load against the v2 payload, because the field is absent. That is intended.

### Tests

Command, run from `C:/atx-wt/pool-8`:
`& 'C:/Program Files/Python312/python.exe' -m unittest discover -s atx-engine/tools -p test_prepare_research_fields.py -v`

- **Result:** `Ran 14 tests in 2.437s`, `OK`, exit 0. Nine tests existed before; five are new.
- **Log:** scratchpad `t6fix/t6-fix1-tests.log`, sha256 `f105d62e4f648964d1f75c0f0080381cf9b8722e714071b568e851d4e4484703`.

New or extended coverage:

- **`test_iv_declared_domain_to_nan_and_counted`**
  - Fixture cells in each tenor: 1e16, 0.0199, float32 0.02 (kept), 5.0 (kept), 5.05/5.1, +inf, negative, and a non-member 9.0.
  - Checks exact NaN/keep per cell and that kept cells are unclamped.
  - Checks per-field below/above/member counts (21d 3/3, 63d 1/4, 126d 1/4), post-domain min/max, and ordered quantiles.
- **`test_point_in_time_flags_and_default_fields`**
  - Flags, reasons and aspects for all 11 fields; `non_point_in_time_fields`.
  - The default run publishes exactly the 8 PIT files, byte-identical to the full run.
- **`test_finra_vintage_structured`**
  - `vintage_safe_from` on the real-shaped schedule.
  - A 2024-10-09 cutoff injected into `finra_field`: last republished session 2024-11-15, first safe session 2024-11-18, and a hand-counted `finite_member_cells`.
- **`test_stale_spine_formation_refused`:** a lake ending at the October formation fails at session 2024-12-06 (36 days) and publishes no manifest.
- **`test_code_identity_is_line_ending_independent`:** a CRLF copy gives the same `code_sha256_lf` / git blob and a different raw sha; the git blob id is checked against the formula.
- **Extended tests:**
  - `shares_out`: 303 gets an above-ceiling row on 2024-12-02. It is finite through 11-29 and NaN from 12-02; lines withheld = 2, cells = 66 + 22.
  - Quantiles are checked to equal `np.quantile` of the published finite member cells for every field.
  - `iv_nonpositive_or_nonfinite` = 7.
  - The CLI run with `is_common` flags it false.

**Mutation sweep** (scratchpad `t6fix/mutate.py`): 13 of 13 killed.

- IV domain: compared in f64 instead of f32; clamped instead of NaN; exclusive upper bound; domain removed; member count taken over all cells.
- C-81: whole-line; strict `<`.
- PIT flags: default = all fields; `is_common` flagged PIT.
- Vintage: last-republished tracking removed.
- Spine: age bound 40.
- Quantiles: computed without the member mask.
- Code pin: LF normalisation removed.

### Real-data runtime and RAM (estimate; not run)

- **Runtime:** unchanged except one read-back per field for the quantiles. That is the same read that already computed the sha, plus `np.quantile` on at most 6.5M values, so about 5 s more per role in total.
- **Peak RSS:** unchanged at about 450 MiB. The read-back adds at most about 52 MB for TRAIN, and it runs after the TickerHistory matrices are freed. It is admitted through `--max-rss-mib`.

### Root rerun commands (not executed; output directories must not exist; run one at a time)

```powershell
& 'C:/Program Files/Python312/python.exe' C:/atx-wt/pool-8/atx-engine/tools/prepare_research_fields.py --role C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v1 --role-sha256 3f53ee9aa1b674d3f5022cbb22d40e5c043e7add8c9422cd2456299ce3662493 --output C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v1-fields-v2 --fields si_shares,si_dtc,iv_atm_21d,iv_atm_63d,iv_atm_126d,earn_recent,shares_out,mktcap_lagged,size_grp,mkt_ret --finra C:/atx/data/finra_short_interest --tickerhistory C:/Users/natha/Downloads/TickerHistory3.parquet --lake C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23 --max-rss-mib 700 --max-seconds 1800

& 'C:/Program Files/Python312/python.exe' C:/atx-wt/pool-8/atx-engine/tools/prepare_research_fields.py --role C:/atx-wt/pool-2/build-equity/recent-fast-validation-2023-2024-v1 --role-sha256 0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7 --output C:/atx-wt/pool-2/build-equity/recent-fast-validation-2023-2024-v1-fields-v2 --fields si_shares,si_dtc,iv_atm_21d,iv_atm_63d,iv_atm_126d,earn_recent,shares_out,mktcap_lagged,size_grp,mkt_ret --finra C:/atx/data/finra_short_interest --tickerhistory C:/Users/natha/Downloads/TickerHistory3.parquet --lake C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23 --max-rss-mib 700 --max-seconds 1800
```

- **Role shas.** Both role manifest shas were re-hashed today and are unchanged. The shape matches the v1 runs, plus an explicit `--fields`, which is the ten v1 fields minus `is_common`.
- **Code pin.** If root runs its cherry-picked copy instead of the pool-8 path, `code_git_blob_sha1` must be `482da7bb16b8ec7a5e77e5b4c9bc657e1ad291df` whatever the line endings.
- **Descriptive checks for root after the run:**
  - `fields[iv_*].plausibility.implausible_to_nan_member` per tenor and role. From v1, expect VAL `iv_atm_126d` above_max >= 1 (the 1.19e16 cell) and the 69.3 cells in 21d/63d in both roles.
  - `non_point_in_time_fields == ["mktcap_lagged","size_grp"]`.
  - `source_checks.tickerhistory.shares_cells_withheld_c81`.
  - `source_checks.lake.spine_max_formation_age_days` <= 35.

### Concerns (fix round 1)

1. **Decision for root: `mktcap_lagged` and `size_grp` are flagged non-PIT because their presence is non-PIT.** This goes beyond the literal ruling, which only named `is_common`. The consumers are T10's market-cap predictor and any T8 size family, and each needs an explicit allow-or-refuse ruling.
2. **The IV domain removes only out-of-domain garbage.** In-domain vendor errors, and the unproven earnings-calendar vintage behind the IV cleaning, remain.
3. **M3 is still open:** the `shares_out` restatement has no artifact guard. It needs a declared bound.
4. **Pre-2021-06 FINRA republication is only reported, not masked.** It covers roughly the first 18 months of the TRAIN score window. `first_session_vintage_safe` gives the masking start if root wants SI selected on 2021-06 onward only.

## Fix round 2

2026-09-27. Owner: t6-fix. Worktree `C:/atx-wt/pool-8`, new branch `feat/mega-alpha-fields-fix2-20260927`
cut from root `e885687b` (T12's `f822dd42` branch left untouched). **Commit `03a8f746`** (not pushed), which
touches the same two files only:

- `atx-engine/tools/prepare_research_fields.py`: sha256 `7c77fc46929d4e1e92e00fa8c08ea1d4f3e30bcc10115d44ee5caa24764a0af2` (LF), git blob `a17f83c62c13a0017444ef3b4b74bac79ddc6024`
- `atx-engine/tools/test_prepare_research_fields.py`: sha256 `d849a86ce2774da1cb9672bcf38ee1b9a3c9334673713ed320915c9c241aa841`

Nothing built and no field computed on real data. Real-data reads:

- TickerHistory3 footer schema only.
- The two role manifests.
- Binding checks of the TRAIN v2 and validation v1 roles (manifest, sessions, ids and member sha checks through the producer's `Role`).

### Finding (T12 §6.1) and the fix

`shares_out` restated the 90-day-lagged vendor share count by `cumulReturnFactor(session)/cumulReturnFactor(lag)`.
That factor is not chained across 2021-01-04: bars before that date omit the corporate actions dated on or after
it (atx-db VA1 / C-35).

- **The error.** For every session whose lag row precedes the break, the ratio carried the re-anchoring step:
  - later consolidations: x10..x80
  - later forward splits: x1/4..x1/10
  - dividend payers: 1-10%
- **Where it hit.** About 62 TRAIN sessions, plus any validation cell whose lag row is stale back to before 2021-01-04.

**Chosen fix: apply factor-break-v1 consistently.** It is ported into the producer with T12's constants and step
classification, and recorded in the manifest as `factor_break.parameters` / `statement`.

- **Observations.** The rule runs on the vendor observations of the role's ids over the producer's extended
  calendar, [first session - 490 d, last session]. An observation is the role's present contract:
  - unique (date, id) key
  - finite positive `close` and `cumulReturnFactor`
  - finite `volume` >= 0
- **Mass sessions.** A mass session has >= 50 jump cells. Every crossing step on it is classified exactly as T12
  does: `repaired`, `kept_gap`, `kept_split_follow` or `kept_distribution`.
- **Restatement.** shares_out(s) = shares(L) x 1000 x crf(s)/crf(L) / prod{k of repaired steps (p, t, k) with L < t <= s}.
  - Cells without a repaired step are bit-identical to before (division by 1.0).
  - Genuine splits and distributions stay in the ratio.
- **Point in time.** The correction starts at the step's end t, where k is first known, not at the mass session b.
  - Mass detection at b and classification of (p, t) read rows <= t only.
  - A test shows that a role truncated at 2021-01-05 gives byte-identical cells to the full role on the common
    sessions, even though a gap step ending 2021-01-06 exists in the full run.
- **Gap steps.** A restatement window that spans a `kept_gap` step (a hole of more than 10 days across a mass
  session, where artifact and genuine action cannot be separated) becomes NaN, counted.
- **Lag row and session row must both be observations.** No row can then sit between a step's p and t, which
  keeps the correction exact.
- **Binding gate (fail closed).** Mass sessions strictly inside the role (first < b <= last, i.e. a return across
  b is in the role) must equal the role manifest's `repair.mass_sessions`.
  - TRAIN v2 lists `2021-01-04`; validation v1 lists none.
  - Binding the unrepaired TRAIN v1 role, or a role whose repair lists another date, is refused before
    publication, and the error message prints both lists.

**Alternatives rejected:**

1. **Reading the repaired role's `repair_cells.csv`** (T12's suggestion). It covers only in-role steps. It cannot
   fix pre-role lag rows: validation's stale lags, or TRAIN's first sessions. It would also couple the producer
   to a sidecar format.
2. **A "split-only" chain** (restate only steps where the raw close followed). It ignores the broken chain, but
   it would drop steps the vendor dates off the price (VA1 cites ARCM x5 dated one session later). It would also
   change every restatement, not just the broken window.
3. **`returnFactor`.** Its behaviour across 2021-01-04 cannot be verified without reading real data, which this
   lane forbids.

**M3 (open review minor), in the form root declared for T10:** `shares_out` outside [1e5, 5e10] (float64,
inclusive) becomes NaN and is never clamped.

- It is applied after the C-81 and factor-break rules.
- It is counted in a `plausibility` block with the same keys as IV: `min`, `max`, `inclusive`, `units`, `rule`,
  `implausible_to_nan`, `implausible_to_nan_member`, `below_min`, `above_max`, `member_below_min`,
  `member_above_max`.
- As T12 noted, this range alone would not catch a x10 error. The factor-break fix does.

### TRAIN v2 binding (confirmed)

`Role(recent-fast-train-2020-2022-v2, 210fff96…)` accepts it:

- 1155 x 5627, 2018-06-01..2022-12-30
- `source_sha256` `0ed96b2696f1…`, the same vendor file
- `repair.mass_sessions` = `["2021-01-04"]`

Validation v1 (`0c757c41…`) is accepted too (903 x 5048, 2021-06-01..2024-12-31, no repair block). The role-derived
`mkt_ret` is computed from the bound role's `close.f64`, so on TRAIN v2 it uses the repaired closes.

### Manifest (schema unchanged, additive)

- **`fields[shares_out].factor_break`**:
  - `rule`, `ported_from`, `statement`, `parameters`, `use`
  - `mass_sessions[]` with `{session, inside_role, jump_cells, crossing_steps, repaired, kept_gap, kept_split_follow, kept_distribution}`
  - `role_repair_mass_sessions`
  - `max_non_mass_jump_cells` / `max_non_mass_session` (the margin to 50)
  - `restated_cells` / `restated_member_cells`
  - `gap_ambiguous_to_nan_cells` / `gap_ambiguous_to_nan_member_cells`
- **`fields[shares_out].plausibility`**: as above.
- **Changed values:**
  - `shares_out` `source_columns` now also list `close` and `volume`.
  - The `clock` is `A8-vendor-shares-lag90-restated-v2`.
  - `staleness` and `caveats` are rewritten.
- **Nothing removed or renamed.**

### Tests

Command, run from `C:/atx-wt/pool-8`:
`& 'C:/Program Files/Python312/python.exe' -m unittest discover -s atx-engine/tools -p test_prepare_research_fields.py -v`

- **Result:** `Ran 19 tests in 2.507s`, `OK`, exit 0. Fourteen tests were already there; five new ones form class `FactorBreakShares`.
- **Log:** scratchpad `t6fix/t6-fix2-tests.log`, sha256 `621bc54635196f52c7c387d46c8a386ae5a307d54aed41926364a95a2b19f1af`.

**New fixture** (64 names, weekdays 2019-05..2021-06, unchained factor at 2021-01-04):

| Lines | What they model |
|---|---|
| 52 | dividend payers, factor step -3% |
| 1 | later consolidation, x10 |
| 1 | later 4:1 split: x1/4 at the break, genuine split 2021-03-01 |
| 1 | 2:1 split on the break, shown by the factor and raw followed (`kept_split_follow`) |
| 1 | 14-day hole across the break (`kept_gap`) |
| 1 | 5% same-day distribution (`kept_distribution`) |
| 1 | opposite-sign dividend: repaired but not a jump cell |
| 1 | repaired step ending 2021-01-06 |
| 1 | control with a null-close row |
| 1 | genuine split on a normal day |
| 2 | out-of-domain lines (5e4 and 6e10 shares) |
| 1 | stale lag rows before the break |

New tests:

- **`test_restated_correctly_across_the_unchained_factor`**
  - Every cell equals an independent oracle (lag shares x genuine actions in (lag, s] x 1000; rtol 1e-12).
  - Explicit cells: the x10 and x1/4 lines equal 1e6 on 2021-02-01; the correction applies on the step's own day; the lag row equal to the step end is not corrected.
  - The `factor_break` block: 2021-01-04 with 56 jump cells, 60 steps = 57 repaired + 1 of each kept class; margin 1 on 2021-01-06.
  - Restated and gap counts equal the oracle.
  - The shares plausibility counts, including non-member cells.
- **`test_break_before_the_role_corrects_stale_lag_rows`:** a validation-like role starting 2021-05-03. The break is detected pre-role (`inside_role` false) and the stale-lag line is corrected.
- **`test_break_on_the_first_session_needs_no_role_repair`:** an unrepaired role starting on the break is accepted and corrected.
- **`test_point_in_time_later_rows_change_no_earlier_cell`:** the truncation test described above.
- **`test_role_repair_block_must_match_detected_breaks`:** unrepaired or wrong repair blocks are refused and no manifest is published.
- **Existing tests:** the main fixture gained a `volume` column. Its `shares_out` values are unchanged, and it checks an empty `factor_break` and a zero-count plausibility block.

**Mutation sweeps:** 18 of 18 fix-2 mutations killed, plus 3 disambiguated fix-1/fix-2 mutations killed; the fix-1 sweep still kills its patterns. The fix-2 mutations were:

- correction removed or inverted
- `t < day`, and `lag <= t` for both the repaired and the gap window
- gap not NaN; every class repaired
- split-follow veto removed; distribution keep removed
- mass threshold 57; jump count ignoring the gap limit
- gate removed; gate including the first session
- observation ignoring close
- domain removed; domain upper bound 1e11
- restated count taken before the domain
- C-81 whole-line

**Full-scale synthetic** (scratchpad; TRAIN shape 1196 x 5627; 8.7M vendor rows, 282 MB parquet; 1200-line break; 3 IV + earn + shares_out):

- **7.3 s wall, peak working set 441 MiB.** The prior all-field measurement was 405 MiB.
- The TickerHistory phase now also holds the extended-calendar factor (f64) and raw close (f32): about +60 MB for TRAIN.
- It runs under `--max-rss-mib 700`.

### Root commands (not executed; output directories do not exist yet; run one at a time)

```powershell
& 'C:/Program Files/Python312/python.exe' C:/atx-wt/pool-8/atx-engine/tools/prepare_research_fields.py --role C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v2 --role-sha256 210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de --output C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v2-fields-v2 --fields si_shares,si_dtc,iv_atm_21d,iv_atm_63d,iv_atm_126d,earn_recent,shares_out,mkt_ret --finra C:/atx/data/finra_short_interest --tickerhistory C:/Users/natha/Downloads/TickerHistory3.parquet --lake C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23 --max-rss-mib 700 --max-seconds 1800

& 'C:/Program Files/Python312/python.exe' C:/atx-wt/pool-8/atx-engine/tools/prepare_research_fields.py --role C:/atx-wt/pool-2/build-equity/recent-fast-validation-2023-2024-v1 --role-sha256 0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7 --output C:/atx-wt/pool-2/build-equity/recent-fast-validation-2023-2024-v1-fields-v2 --fields si_shares,si_dtc,iv_atm_21d,iv_atm_63d,iv_atm_126d,earn_recent,shares_out,mkt_ret --finra C:/atx/data/finra_short_interest --tickerhistory C:/Users/natha/Downloads/TickerHistory3.parquet --lake C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23 --max-rss-mib 700 --max-seconds 1800
```

- **Code pin.** If root runs a cherry-picked copy, the manifest `code_git_blob_sha1` must be `a17f83c62c13a0017444ef3b4b74bac79ddc6024`.
- **Output names.** The fix-1 `...-v1-fields-v2` TRAIN name is superseded by `...-v2-fields-v2` (repaired role). Validation keeps `...-v1-fields-v2`.

**What to read after the runs** (descriptive, no outcomes):

- **TRAIN `fields[shares_out].factor_break`:**
  - `mass_sessions` should be exactly `2021-01-04` with `inside_role: true`.
  - Its counts should reproduce T12's role block: jump 1185, crossing 2123, repaired 2114, kept_split_follow 2, kept_distribution 7, kept_gap 0. The populations are defined identically, so a difference means they diverge; worth a look, not a refusal.
  - `restated_cells` should be about 62 sessions x the affected names.
  - `gap_ambiguous_to_nan_cells` should be 0.
  - `max_non_mass_jump_cells` should be below 50; T12 saw 44 on 2022-12-29.
- **Validation:**
  - `mass_sessions` should be `2021-01-04` with `inside_role: false`, detected pre-role from the validation ids' vendor rows.
  - `restated_cells` covers the stale-lag cells only.
  - No mass session inside the role.
- **Both roles:** `fields[shares_out].plausibility` counts.

### Concerns (fix round 2)

1. **Hard refusal on mismatch.** The gate refuses when the producer's mass sessions inside the role differ from the role's repair block. The margin on TRAIN is 44 vs 50 (2022-12-29), and the populations are identical by construction, so a spurious refusal is unlikely. If it happens, the message names both lists, and it is a real data-definition disagreement to resolve before using TRAIN sizes.
2. **Validation correction depends on detecting the break pre-role.** The validation stale-lag correction needs at least 50 jump cells among the validation ids at 2021-01-04. If fewer are present, the break is not detected and those cells stay uncorrected. Read `mass_sessions` in the validation manifest.
3. **Residuals as in T12:**
   - Artifact components inside `kept_split_follow` or `kept_distribution` steps on the break remain.
   - A genuine split that the vendor factor never shows is not restated, as before.
   - The dividend steps inside the ratio (a few tenths of a percent) remain.

## Fix round 3

2026-09-27. Owner: t6-fix. Worktree `C:/atx-wt/pool-8`, new branch `feat/mega-alpha-fields-fix3-20260927`
cut from root `e756d2a9`. At that root the T6 files equal my fix-2 commit. **Commit `e0ae0e18`** (not pushed),
which touches the same two files only:

- `atx-engine/tools/prepare_research_fields.py`: sha256 `f0933e2fb3411490c4df9a1dae1355db70ff9f666ed78924d071b7db3695af24` (LF), git blob `f5e38b97b625f57129bcd93f2f0f2ca350282e7d`
- `atx-engine/tools/test_prepare_research_fields.py`: sha256 `25551ebb860bae05c71fb848ec7d7441104dfc52b804903a3c790f7d698955ff`

Nothing was built and nothing was run on real data. The only real-data reads were the two role manifests, for
`volume_basis` and the file list.

### Finding (re-review 2) and ruling implemented

About 105 lines carry vendor share counts roughly 1000x too small. The [1e5, 5e10] domain catches only part of
them. Root's ruling is implemented as declared, with no thresholds of mine.

After the factor-break correction, the gap-ambiguity rule, C-81 and the domain, in that order, a `shares_out`
cell becomes NaN (never clamped) and is counted per rule when:

- **(a) turnover:** the trailing 21-session median of the name's daily volume, in the session's share basis,
  exceeds 1.0 x `shares_out`.
- **(b) short interest:** `si_shares` visible at the session exceeds 1.5 x `shares_out`.

Only `shares_out` is set to NaN. A test checks that `si_shares` is untouched.

### Decisions (the ruling left these open; please accept or overrule)

1. **Volume basis (confirmed).** Both role manifests declare `volume_basis: "raw-share-volume"`, the vendor
   `volume` column in each day's own share units (`prepare_recent_research.py` projection). `shares_out` is in the
   session's share basis.
   - Each window day is therefore restated with the role's own factor f = close/raw, which is the T12-repaired
     chain on TRAIN v2: u_d = volume_d / f_d.
   - The rule compares median(u) x f_t with `shares_out`, which is the same as median(volume_d x f_t / f_d).
   - The factor also carries dividends. Their drift over 21 sessions is a few percent at most, which does not
     matter at a 1.0x threshold.
   - A test pins this with a genuine 1:10 consolidation at 15% daily turnover. The unconverted raw median would
     read 1.5x and wrongly NaN the cell for about 10 sessions; the mutation sweep kills that variant.
2. **Window definition.** The window is the 21 role sessions ending at t, t included, and uses the name's present
   days only.
   - The rule is evaluated only when at least 11 of those days are present (a majority).
   - Otherwise the cell is kept and counted as `not_evaluable_cells`. This mainly affects each role's first 10
     sessions, which are warm-up.
   - Constant: `SHARES_TURNOVER_MIN_OBS = 11`.
3. **Which `si_shares`.** Rule (b) uses this run's own published `si_shares` field: strict available_at < session,
   45-day staleness.
   - `shares_out` therefore now **requires `si_shares` in the same run**. `--fields shares_out` alone is refused
     before anything is written, and the manifest entry records `depends_on: ["si_shares"]`.
4. **Strict comparisons** in both rules: exactly 1.0x or exactly 1.5x is kept. A test pins SI = 1.5e6 against
   `shares_out` = 1e6.
5. **Counting.**
   - (a) counts the cells it sets to NaN.
   - (b) counts only cells (a) did not already take. The overlap is reported as `turnover.also_above_si_ratio`.
   - `plausibility.implausible_to_nan` (and `_member`) now total **all** rules: domain + (a) + (b). This
     **widens the meaning** of an existing key; the per-rule keys hold the old parts.
6. **Point in time.** Row t uses role rows <= t and the `si_shares` row t, which is visible strictly before the
   session.
   - A role truncated 8 sessions into a volume spike is byte-identical to the full role on the common sessions.
   - The spike name becomes NaN only from the 11th spike session onward.
7. **Binding.** The role's `volume.f64`, `close.f64`, `raw_close.f64` and `present.u8` are streamed row by row.
   - Each is hashed as it is read and must equal the role manifest; tampered bytes are refused with no manifest.
   - They are listed in `plausibility.turnover.inputs`.

### Manifest (schema unchanged; additive, except the widened total in decision 5)

- **`fields[shares_out].plausibility`** gains:
  - `units_rule`: the full statement
  - `turnover`: `{window_sessions 21, min_present_sessions 11, max_median_volume_over_shares_out 1.0, volume_basis, to_nan, to_nan_member, also_above_si_ratio, not_evaluable_cells, inputs}`
  - `si_ratio`: `{max_si_shares_over_shares_out 1.5, si_shares, to_nan, to_nan_member, counting}`
- **`fields[shares_out].depends_on`** = `["si_shares"]`.
- **Changed text:** `shares_out` `staleness`.

### Tests

Command, run from `C:/atx-wt/pool-8`:
`& 'C:/Program Files/Python312/python.exe' -m unittest discover -s atx-engine/tools -p test_prepare_research_fields.py -v`

- **Result:** `Ran 22 tests in 4.745s`, `OK`, exit 0. Nineteen tests were already there; three are new, and several existing ones are extended.
- **Log:** scratchpad `t6fix/t6-fix3-tests.log`, sha256 `d6442add6dec54a07682122219e6a418cb39cf07b6648fca337a47fc178d9206`.

Fixture additions to the unchained-factor fixture (now 68 names, with role volume/close/raw/present payloads and a FINRA set):

| Line | Setup | Expected |
|---|---|---|
| 1065 | 1:10 consolidation, 15%/day turnover in both bases | always kept |
| 1066 | 1.5e5 shares, inside the domain, trading 5e5 a day | NaN from the 11th session; not evaluable before |
| 1067 | SI 1.6e6, then exactly 1.5e6, on 1e6 shares | NaN at 1.6x, kept at 1.5x, kept after the SI goes stale |
| 1068 | trades 5x its shares a day from 2021-02-01 | kept until the 11th spike session |
| 1059 | SI at 1% | untouched |
| 1066 | also SI 6.7x | counted in `also_above_si_ratio`, not in `si_ratio.to_nan` |

- **Oracle.** It now applies both rules independently (as-of SI; a per-cell median with an explicit basis
  restatement). The full `shares_out` matrix and every per-rule count, member counts included, must match it.
  The main fixture's `shares_out` values are unchanged: its role gained a `volume.f64` at about 1%/day.
- **New tests:**
  - `test_units_rules_turnover_and_short_interest`
  - `test_units_rules_point_in_time`
  - `test_shares_out_requires_si_shares`
- **Extended:** the source-pin test now also refuses tampered role bytes through the `shares_out` stream.

**Mutation sweep** (scratchpad `t6fix/mutate4.py`): 13 of 13 killed.

- no basis conversion; minimum present days 1; window 11; turnover bound 10
- window excluding t; SI `>=`
- either rule turned off; SI count including turnover cells
- stream verification skipped; `requires` check removed
- units rules applied before the domain; not-evaluable count over all cells

The fix-1 and fix-2 sweeps were re-run against the final code: all killed, with the one re-indented pattern re-run
at its new indentation.

**Full-scale synthetic** (scratchpad; TRAIN shape 1196 x 5627, 8.7M vendor rows, si_shares + 3 IV + earn + shares_out):

- **11.2 s wall, peak working set 427 MiB.** Fix 2 took 7.3 s; the extra time is the rolling median and the role streams.
- The rules add only a 21 x names window and single-row buffers, so the real TRAIN peak should stay close to the
  re-review's 635 MiB, against `--max-rss-mib 700`.

### Expected real counts

I cannot compute these without real data; root runs it. Read them from `fields[shares_out].plausibility`:

- `turnover.to_nan` / `to_nan_member`
- `turnover.also_above_si_ratio`
- `turnover.not_evaluable_cells`
- `si_ratio.to_nan` / `to_nan_member`
- `below_min` / `above_max`: should be unchanged from fields-v2 (TRAIN below_min 25,411)

Re-review references on TRAIN fields-v2 (in-domain member cells):

- 1,381 cells trade more than 10x their count daily. Rule (a) at 1.0x should take nearly all of them, apart from
  any with fewer than 11 present days.
- 6,851 cells have an SI ratio above 1, and 3,258 above 10. Rule (b) takes the part above 1.5 that (a) did not.

### Root commands (not executed; output directories must not exist; run one at a time)

```powershell
& 'C:/Program Files/Python312/python.exe' C:/atx-wt/pool-8/atx-engine/tools/prepare_research_fields.py --role C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v2 --role-sha256 210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de --output C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v2-fields-v3 --fields si_shares,si_dtc,iv_atm_21d,iv_atm_63d,iv_atm_126d,earn_recent,shares_out,mkt_ret --finra C:/atx/data/finra_short_interest --tickerhistory C:/Users/natha/Downloads/TickerHistory3.parquet --lake C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23 --max-rss-mib 700 --max-seconds 1800

& 'C:/Program Files/Python312/python.exe' C:/atx-wt/pool-8/atx-engine/tools/prepare_research_fields.py --role C:/atx-wt/pool-2/build-equity/recent-fast-validation-2023-2024-v1 --role-sha256 0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7 --output C:/atx-wt/pool-2/build-equity/recent-fast-validation-2023-2024-v1-fields-v3 --fields si_shares,si_dtc,iv_atm_21d,iv_atm_63d,iv_atm_126d,earn_recent,shares_out,mkt_ret --finra C:/atx/data/finra_short_interest --tickerhistory C:/Users/natha/Downloads/TickerHistory3.parquet --lake C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23 --max-rss-mib 700 --max-seconds 1800
```

- The role pins and field list are unchanged from fields-v2; `si_shares` is already in the list.
- If root runs a cherry-picked copy, the manifest `code_git_blob_sha1` must be `f5e38b97b625f57129bcd93f2f0f2ca350282e7d`.
- Every field other than `shares_out` should be byte-identical to fields-v2, so its sha should match the v2 manifest. That is a cheap regression check.

### Concerns (fix round 3)

1. **Genuine exchange-traded-product exceptions fall to both rules.**
   - Leveraged and volatility products can trade more than their shares outstanding in a day.
   - Some ETFs have carried short interest far above 150% of shares outstanding; XRT in early 2021 is the known
     case, reportedly several hundred percent.
   - The role universe includes ETFs, so those cells become NaN. T10 then treats them as a missing predictor
     (warm), which understates borrow cost for exactly the most heavily shorted ETFs.
   - Root may want a declared fallback for units-rule NaN cells in T10, for example tiering those names from
     `si_shares` against a known float instead.
2. **Decisions 2 and 5 are mine and need root's acceptance:** the 11-of-21 present-day minimum, and widening
   `implausible_to_nan` to total all rules.
3. **Re-review-2 minors are still open,** outside this round's ruling: N1 (the ported detector is v1 with 3
   cells of headroom on validation) and N2 (the binding gate checks session names only, not repaired counts).
