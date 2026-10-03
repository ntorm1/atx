# tier1-v4 Task 1 report: market infrastructure runs (liquidity, market index, index proxies, identity codes)

Status: DONE. Branch `feat/tier1-v3-warehouse`. Every data job ran under the memory guard from `C:/atx/atx-db` with
`PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1`; below, `G(x)` =
`.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb x --wait-minutes 600 --
.venv/Scripts/python.exe -m`.

## 1. What was built

| stage | output (under `data/alpha_panel/v1/`) | rows | bytes | span | command | guard peak / wall |
|---|---|---|---|---|---|---|
| market_liquidity | `market/liquidity/year=YYYY/liquidity.parquet`, `market/market_liquidity_manifest.json` (gate_status), `validation/liquidity_coverage.json` | 22,927,098 | 1,579,964,789 | 2018-01-02 .. 2026-09-18 | `G(0.5) atx_db.alpha_panel.liquidity all` | 0.366 GiB / 233 s |
| market_index | `market/index_returns.parquet`, `market/market_index_manifest.json` (gate_status), `validation/factors_market.json` | 9,022 | 137,142 | D 2018-01-12 .. 2026-09-18; M 2018-02 .. 2026-08 | `G(0.6) atx_db.alpha_panel.market_index build` | 0.551 GiB / 25 s |
| indexes | `indexes/constituents.parquet` 73,177; `indexes/membership/year=YYYY/membership.parquet` 6,218,671; `indexes/recon_summary.parquet` 61; `indexes/manifest.json` (gate_status) | see left | 1,016,128 + 2,085,448 + 6,964 | 2018-03-19 / 2018-06-25 .. 2026-09-18 | `G(0.5) atx_db.alpha_panel.indexes build` | 0.358 GiB / 38 s |
| (validation) S3.2 | `validation/returns.json` | 500 line-months, 10,502 daily returns | - | 2018-02 .. 2022-12 | `G(0.5) atx_db.alpha_panel.returns_validation` | 0.415 GiB / 3 s |
| share_exchange_history | `security_master/share_exchange_history.parquet` + `_manifest.json` | 397,050 runs, 22,704 lines | 2,291,538 | 2018-01-02 .. 2026-09-18 | `G(0.45) atx_db.alpha_panel.identity_v3 codes` | 0.300 GiB / 25 s |

`lake verify --stage market_liquidity --stage market_index --stage indexes --stage market_shares`: OK 4 (bindings via
`input_manifests_sha256`; every manifest's code SHA matches the committed module). Liquidity rows per year: 2018
2,139,183; 2019 2,198,681; 2020 2,266,378; 2021 2,699,451; 2022 2,996,980; 2023 2,790,668; 2024 2,742,304; 2025
2,860,249; 2026 2,233,204. `_tmp/mkt` had been deleted since the v3 stop, so liquidity rebuilt all 64 units (about
1.5 s each, not 20 s); it ran a second time after the `measure()` edit so the manifest binds the committed code
(identical row counts), followed by an `indexes` rebuild against the new liquidity manifest.

## 2. Gates (GC9)

| criterion | result | measured | command |
|---|---|---|---|
| S3.7 cost-model inputs finite on >= 99% of panel member_equity cells 2019-2026 | PASS | min column 99.34% (turnover_63); spread_cs_21, spread_ar_21, amihud_21/63/252, adv_21/63, zero_vol_21/63, halt_days_21 100.00%; turnover 99.64%, turnover_21 99.55% (4,443,832 cells; lowest year turnover_63 2021 98.51%) | `G(0.5) atx_db.alpha_panel.liquidity all` -> `validation/liquidity_coverage.json` |
| S3 exit: monthly rho(member_common VW - DTB3, French Mkt-RF) >= 0.99, windows <= 2022-12 | PASS | 0.99968 (56 months 2018-05..2022-12; daily 0.99977 over 1,194 days; all_common 0.99958 / 59 months) | `G(0.6) atx_db.alpha_panel.market_index build` -> `validation/factors_market.json` |
| S3 exit: own 2x3 SMB vs French SMB rho >= 0.90 | PASS | 0.98019 (54 months 2018-07..2022-12); HML recorded 0.98502 | same |
| GC2: all_common VW vs French Mkt (Mkt-RF + RF) monthly rho >= 0.99, 2013-01..2022-12 | PASS | 0.99835 (120 months, 0 missing): history 2013-01..2018-01 0.99431 (61 months, mean 3,362 stocks), stage 2018-02..2022-12 0.99956 (59) | same, `factors_market.json` key `mkt_all_common_2013` |
| S3.6 daily membership 2019-2026 | PASS | every session 2019-01-02..2026-09-18 has R1000P/R2000P/R3000P/SP500P flags; 0 duplicate (session, line) | `G(0.5) atx_db.alpha_panel.indexes build` |
| S3.6 R2000 proxy in [1900, 2100] per reconstitution | PASS | companies 1,989..2,016 at all 9 recons (lines 2,018..2,078) | same -> `indexes/manifest.json` gate_status |
| S3.6 turnover per reconstitution | PASS (recorded) | R2000P one-way 12.2-21.5%, cap-weighted 5.5-16.8%; R3000P 8.6-15.0%; SP500P 0.9-4.5% per quarter after the first | same -> `recon_summary.parquet` |
| S3.6 proxy overlap with official lists >= 95% | GAP (not measurable) | FTSE Russell / S&P DJI constituent lists are licensed, pages terms-gated (carried from v3) | - |
| S3.2 >= 99% of sampled daily returns within 1 bp (total-return identity) | PASS | 99.695% of 10,502 (500 line-months, 30 strata, seed 20260929); backward chain 100%; monthly 100% (500) | `G(0.5) atx_db.alpha_panel.returns_validation` -> `validation/returns.json` |
| S3.2 every exception explained | PASS | 32 exceptions, all `cash_distribution_convention` (vendor multiplicative vs CRSP additive dividend convention; max 16 bp on a 12% day) | same |
| S3.2 independent second source | GAP (documented) | free price APIs forbid bulk use or need unapproved keys (carried from v3) | - |
| S2.5 shrcd / exchcd dated histories built (no numeric gate in v3) | PASS | 397,050 runs, 0 overlaps, 0 duplicate keys; member_equity cells 2019-2026: shrcd 100%, exchcd in 1-6 95.1-98.4% | `G(0.45) atx_db.alpha_panel.identity_v3 codes` + check query |

Summary: 10 PASS, 0 FAIL, 2 documented GAPs (licensed index lists, second price source). Manifests carry
`gate_status` (market_liquidity, market_index, indexes); `validation/returns.json` carries its `gate`.

## 3. Tests

- `.venv/Scripts/python.exe -m pytest -n 0 -q tests/test_alpha_panel_liquidity.py tests/test_alpha_panel_market.py
  tests/test_alpha_panel_market_returns.py tests/test_alpha_panel_indexes.py tests/test_alpha_panel_identity_v3.py`
  -> 41 tests, 0 failures, 4.6 s (junit). Adding `test_alpha_panel_{shares,reference,classification}.py`: all pass.
- New tests (5): `test_measure_writes_gate_status_into_manifest`; `test_build_index_writes_daily_and_monthly_rows`
  (the UNION parser defect; fails on the old code); `test_history_months_weight_prior_month_end_vendor_me`;
  `test_market_history_check_splices_stage_and_adds_rf_back`; `test_russell_gate_counts_companies_per_recon`.
- `tests/test_lake_registry.py`: 7 pass, 1 fails (`test_live_lake_every_manifest...`): pre-existing gaps of other
  lanes (classification_tnic, events, fundamentals_v10, identity/link_table_v3, security_master/* manifests incl.
  `share_exchange_history_manifest.json`, nport/, panel_goldready/, ...); none concerns market_liquidity,
  market_index or indexes.

## 4. Files changed

- `atx-db/src/atx_db/alpha_panel/market_index.py`: defect fix (`daily_sql() UNION ALL monthly_sql()` joined two
  `WITH` queries; DuckDB parser error, the build never ran) -> wrapped as subqueries; GC2 check
  (`history_bars_sql`, `history_monthly_sql` rule `crsp-index-history-v1`, `market_history_check`, French RF read);
  manifest now published after the factor checks with `gate_status`; `prices_history` added to inputs / LAKE_STAGES.
- `atx-db/src/atx_db/alpha_panel/liquidity.py`: `measure()` stamps `gate_status` into the manifest.
- `atx-db/src/atx_db/alpha_panel/indexes.py`: `russell_gate()` -> manifest `gate_status`.
- Tests: `atx-db/tests/test_alpha_panel_{market,liquidity,indexes}.py`.
- Docs (GC8/GC5): `atx-db/docs/datasets/{market_liquidity,market_index,indexes,share_exchange_history}.md` and
  `.registry.json` stubs.
- Not changed: `returns_validation.py`, `identity_v3.py` (ran as committed).

Commits: ee353557 (code + tests), 0c35357e (docs + stubs), this report.

## 5. Sources, disk

No new source landed (all inputs already in the lake). Disk: `market/liquidity` 1.58 GB, other outputs < 6 MB;
`_tmp/mkt` 2.1 GB scratch (price projection + liquidity units; deletable, rebuilt on demand); C: 78 GB free.

## 6. Handoff to Task 2 (`universe/`)

| need | path (under `data/alpha_panel/v1/`) | keys | columns to use | clock |
|---|---|---|---|---|
| ADV, spreads, illiquidity | `market/liquidity/year=YYYY/liquidity.parquet` (view `market_liquidity`) | `(security_id, session_date)` | `adv_21`, `adv_63` (USD, untraded = 0), `spread_cs_21`, `spread_ar_21` (fraction of price), `amihud_21/63/252`, `turnover_21/63`, `zero_vol_21/63`, `halt_proxy`, `has_bar`, `shrout_used` | `available_at` = session 22:00 UTC |
| market returns | `market/index_returns.parquet` (view `market_index_returns`) | `(freq, period_end, universe, weighting)` | `freq` 'D'/'M', `universe` 'all_common' (or 'member_common'), `weighting` 'VW'/'EW' -> `ret`, `n_stocks`, `me_lag_total` | period end 22:00 UTC |
| all_common rule | not a per-line output: reuse `market_index.common_filter('p')` AND `coalesce(p.is_operating, false)` AND NOT `coalesce(p.is_index, false)` AND `p.exchange IN market_index.LISTED` on `panel/` rows; ME = `market/shares_daily.shrout` x `raw_close`; add raw close >= 1 and shrout known per the Task 2 spec | `(session_date, security_id)` | - | panel PIT |
| index proxies | `indexes/membership/year=YYYY/membership.parquet` (view `indexes_membership`); intervals in `indexes/constituents.parquet` | `(session_date, security_id)` | `r1000`, `r2000`, `r3000`, `sp500`, `cik` | `available_at` = rank date 22:00 UTC |
| share / exchange codes | `security_master/share_exchange_history.parquet` | `(security_id, valid_from)`; join `valid_from <= d <= valid_to` | `shrcd` (11 US common, 12 FPI common, 18 REIT, 31 ADR, 14 CEF, 71 LP, 73 ETP, NULL other), `exchcd` (1 NYSE, 2 Amex, 3 Nasdaq, 4 Arca, 5 BZX, 6 IEX, 0 OTC, NULL unknown), `exchange`, `security_type` | `valid_from` - 2 h (see open issue 1) |
| shares | `market/shares_daily/year=YYYY/shares_daily.parquet` (v3, current) | `(security_id, session_date)` | `shrout` | per row |

all_common line-sessions per session (from `market_index` receipt, before the $1 / shrout filters): 2019 3,910;
2020 3,852; 2021 4,102; 2022 4,250; 2023 3,978; 2024 3,729; 2025 3,532; 2026 3,327. With ME known: 2025 3,343.
The Task 2 gate (3,600-4,800 names/day each year 2020-2025) is at risk in 2025 (and below 3,600 for 2026 YTD).
Lines with a NULL panel `exchange` (e.g. 376 common lines on 2018-06-01) drop out of all_common; codes cover every
all_common cell by construction (shrcd 11, exchcd 1-5).

## 7. Deviations, open issues, self-review

1. `share_exchange_history.available_at` = `valid_from` - 2 h (22:00 UTC of the calendar day before the run). Under
   the strict lake rule (`available_at < lake_cutoff(d)`) a run becomes visible one session after `valid_from`
   (conservative, not a leak). Not changed (no gate fails; "do not restructure"); documented in the dataset doc
   with the interval-join recommendation. Runs fragment where panel flags flicker (SPAC 92,209 runs averaging 6
   sessions).
2. GC2 history segment (validation only, not published): survivorship (lines ending before 2018 absent; 1,238
   such lines held about USD 2.5T of vendor ME in June 2015, part ETFs) and vendor shares as weights. Mean monthly
   difference vs French Mkt -7.6 bp over 2013-2022 (stage-only Mkt-RF -2.8 bp); no delisting returns in either.
3. market_index peaks at 0.551 GiB of a 0.6 GiB cap; give it 0.6 or more.
4. Process note: stopping my first market_index retry loop (it was retrying the parser error) did not end the bash
   loop; its 4th try ran after the fix and published market_index (code SHA = committed module, verified). No
   other job was affected; the guard showed no live slot afterwards.
5. `atx-db/docs/ALPHA_PANEL_MARKET.md` still says these stages were "not yet run"; it is outside this task's paths:
   suggest the controller (or Task 17) point it to `docs/datasets/`.
6. `share_exchange_history` is not in the lake registry (no LAKE_STAGES literal); the stub
   `docs/datasets/share_exchange_history.registry.json` covers it for Task 17.
7. SP500P counts every eligible line of a member company (525-536 lines for 500 companies).
8. Self-review: the diff touches only the three modules, three test files and new docs; no data or scratch files
   committed; D6 is enforced (`factor_checks` raises for `end` > 2022-12-31, the history window stops at the
   stage's first month, `returns_validation` ends 2022-12-31); every new rule has a fixture test.

## Fix round 1

Review findings: (1) the share_exchange_history clock and the registered as-of path disagreed (a code change was
invisible on its first session); (2) `liquidity.measure()` rewrote the published manifest, which made downstream
`input_manifests_sha256` bindings stale.

### Changes (commit 5b773a2f, docs + report in the next commit)

1. `identity_v3.codes()` (rule `share-exchange-runs-v2`): `available_at` = 22:00 UTC of the calendar session before
   `valid_from` minus 1 s (first calendar session: `valid_from` - 1 day 21:59:59). Stage path, keys and columns are
   unchanged. The manifest records the input clocks (`CODES_CLOCK_NOTE`). I checked the claim in `panel.py`:
   - SIC and the filer regime (`is_reit`, `is_fpi`, `is_adr_likely`) are as of `issuer_cutoff` = previous session
     22:00 UTC, strict.
   - FINRA names (`security_type`, `exchange`) are as of `dissemination_date < d`, with `available_at` =
     dissemination 22:00 UTC. That clock can equal the cutoff, or fall on a non-session day before d.
   - The CIK link behind `is_fpi` uses `available_at <= d 22:00` (identity convention).
   - The Nasdaq directory snapshot is a non-PIT fallback for `security_type` / `is_etf`.
   So the claim holds as "the panel's own visibility of its session-d row", not as strict GC1 on every input. The
   codes inherit the panel's contract. This is documented in `docs/datasets/share_exchange_history.md`, which now
   gives a single consumption path:
   `share_exchange_history_as_of(lake_cutoff(d)) WHERE d BETWEEN valid_from AND valid_to`. The stub's
   `as_of_usage` is that query, and it adds a `clock_rule`.
2. `liquidity.py`: `build()` (steps `build` / `all`) measures the year files before publishing and writes
   `gate_status` in the single `publish_part` call. `measure()` now writes only `validation/liquidity_coverage.json`
   and never touches the manifest. The pure logic is split into `coverage()` and `gate_status()`.

### Covering tests

- `test_share_exchange_history_as_of_returns_new_code_from_its_first_session` (new): the transition falls on a
  Monday. It runs the stub's `as_of_sql` and `as_of_usage` verbatim, with `lake_cutoff` as in `stagelake.catalog`.
  The new code appears on its first session, the Friday still shows the old code, and the new run is absent as of
  Friday's cutoff.
- `test_share_exchange_history_runs` (updated to the new stamp, plus the first-session fallback).
- `test_build_publishes_gate_once_and_measure_leaves_manifest_bytes` (replaces the old measure test): a full
  `build()` on the fixture writes `gate_status` into the manifest, and a later `measure()` sees the changed coverage
  but leaves the manifest bytes identical.
- `pytest -n 0` on `test_alpha_panel_{liquidity,market,market_returns,indexes,identity_v3,shares,reference,
  classification}.py`: 68 tests, 0 failures, 10.8 s (junit). The 5 core files hold 42 tests.

### Rebuild evidence (guarded, from `C:/atx/atx-db`)

| step | command | result |
|---|---|---|
| codes | `G(0.45) atx_db.alpha_panel.identity_v3 codes` | 397,050 runs, same shrcd counts; peak 0.306 GiB, 36 s; file sha256 c537e0f3..., 2,291,842 bytes |
| clock check (real data) | guarded DuckDB query, stub macros verbatim | 397,050 / 397,050 runs stamped previous-session 22:00 - 1 s. As-of path = interval join on 2019-07-01, 2021-03-08, 2022-12-30, 2024-03-15 and 2026-09-18 (8,389 / 9,833 / 10,717 / 10,333 / 12,462 rows, equal sets; 84-249 runs start on those days) |
| liquidity | `G(0.5) atx_db.alpha_panel.liquidity all` | same row counts per year; gate published in the manifest (measured_min 0.99343, PASS); peak 0.369 GiB, 320 s |
| measure leaves manifest | sha256 of `market/market_liquidity_manifest.json` before and after `G(0.5) atx_db.alpha_panel.liquidity measure` | fc73dbfe... both times; `liquidity_coverage.json` refreshed (`measured`: standalone) |
| indexes | `G(0.5) atx_db.alpha_panel.indexes build` | 73,177 constituents, 6,218,671 membership rows, R2000 companies 1,989..2,016 (PASS); peak 0.360 GiB, 35 s |
| lake verify | `stagelake verify --stage market_liquidity --stage market_index --stage indexes --stage market_shares` | OK 4. Every manifest's code SHA equals the committed module |

Gates are unchanged: 10 PASS / 0 FAIL / 2 documented GAPs. Note for Task 2: the share_exchange_history file was
rewritten at 02:07Z (new clock only), and liquidity and indexes were republished at 02:13Z with the same rows.
