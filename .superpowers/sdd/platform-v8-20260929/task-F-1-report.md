# Task F-1 report: price and long-lookback fields

Lane F, pool-7, branch `feat/platform-v8-f-20260929`. Python only; tests on synthetic data only.

## OD-6: is the open in the price export?

- **The research projection has no open.** `prepare_recent_research.COLUMNS` (line 71) is `tradingDate, securityID, close, volume, cumulReturnFactor`, so no role carries an open payload.
- **The vendor file the role is projected from likely does.** That file is `TickerHistory3.parquet`, SHA-256 `0ed96b26...` = the role's `source_sha256`. atx-db's `BULK_PRICE_SOURCE_OPTIONS.md` audits "invalid OHLCV rows" in it. The SpiderRock TickerHistory3 dictionary lists `open, high, low, close` (`atx-engine/reviews/2026-09-19-tbltickerhistory-input-audit.md`). I could not confirm this from the file: lane rule 3 forbids opening it.
- **What I built.** `ret_overnight` / `ret_intraday` read `open` straight from the role's own source file, pinned by the role's `source_sha256`. A capability check reads only the parquet footer, before any output. If the file has no float `open` column, the build refuses with the named error `FieldNeedsOpen: ...: field needs open; absent in export (...)`.
  - If `open` is present, both fields build with no role rebuild and no change to `prepare_recent_research.py`.
  - If it is absent, root drops the two fields from `--fields`; the other four do not need the open.
- Both fields are tested on synthetic data, including the refusal.

## What was built

`atx-engine/tools/research_fields_price.py` (new) is an opt-in `FIELD_MODULES` module, built exactly like `research_fields_sec.py`: `bind`, `FIELDS`, `OPTIONS`, `add_arguments`, `check`, `compute`.

- It exports `PRODUCERS = {group: (entry functions,)}`, the C-3 shape. Each field family has its own spec group: `price_open`, `price_ceq`, `price_coskew`, `price_volume`, `price_rd`.
- The entries are module-level functions: `source_panel`, `open_return_rows`, `ceq_iss_rows`, `vendor_market`, `coskew_rows`, `volume_mean_rows`, `zero_filled_rows`.
- There is one new option, `--price-source PATH`: the vendor TickerHistory3 parquet. Its SHA-256 must equal the role's `source_sha256`.

Clock: all fields are point in time with the `t-1` close clock. Row t reads nothing dated after session t-1 (`PRICE_CLOCK`, `ROLE_CLOCK`). `xrd0_ttm` reads row t of the already-lagged fundamentals.

| field | as coded |
|---|---|
| `ret_overnight` | a = t-1, b = t-2: `O_a F_a / (C_b F_b) - 1`. F = cumulReturnFactor chained by factor-break-v1. House guard: `|log| <= 1.5` and `<= |log raw| + 0.10`. |
| `ret_intraday` | `C_a / O_a - 1`, with `|log| <= 1.5` |
| `ceq_iss_5y` | Daniel-Titman `ln(ME_a/ME_b) - ln(P_a/P_b)` with b = a - 1,260 sessions. ME is the A8 house market equity: shares from the last observation 90 to 490 days old, restated through F. The price terms cancel, so the value equals `ln(q(o_a)/q(o_b))` with `q = shares / F`. Other conditions: A9 ceiling rows invalid; C-81 withholding; domain `[-ln 100, ln 100]`, with out-of-domain cells counted. |
| `coskew_60m` | Harvey-Siddique standardised coskewness over 60 monthly (21-session) returns ending at t-1: `mean(e_i e_m^2) / (sqrt(mean(e_i^2)) mean(e_m^2))`. The market is the vendor equal-weight market (below). Needs at least 48 of 60 months, else NaN. |
| `vol_126` | Mean of the role's `volume.f64` over present sessions t-126..t-1. Needs at least 63 such sessions and t >= 126. |
| `xrd0_ttm` | This run's `xrd_ttm`, or 0 where it is NaN and this run's `sale_ttm` is finite. `requires: [xrd_ttm, sale_ttm]`. |

Long history comes from the source the role builder uses.

- The TH parquet is read for the role's lines on an extended axis: NYSE rule sessions before the role (`research_fields_sec.nyse_sessions`), then the role's own sessions. The axis starts 1,261 sessions before the role start (plus the 490-day share lookback for `ceq_iss_5y`).
- Observations follow the role's present contract. Duplicate keys are quarantined.
- The vendor factor is chained with the builder's own `factor_breaks`, as `shares_out` does: every repaired step is divided out, and any span across a kept_gap step is NaN.

The market for `coskew_60m` is the vendor equal-weight market, rebalanced daily (CRSP-style). It averages every vendor line, not only role lines, so the market is point in time too. A role-axis market would have picked its constituents with later liquidity.

- It is computed by one DuckDB query on one thread, for determinism.
- Cells are excluded if they fail the house guard or are factor-break-v1 jump cells.
- The DuckDB spill directory sits inside the output directory and is removed.

Manifest additions per field:

- entry keys: `formula_id`, `formula_sha256`, `lag_sessions`, `min_history`, and `producer_code` (the module's code identity, since the builder manifest pins only the builder file);
- per-field counts: `guarded_member_cells`, `outside_domain_member_cells`, `short_history_member_cells`, `zero_filled_member_cells`;
- `source_checks.price`: source scan counts, the factor-break mass sessions and steps, and the market's contributor stats.

**From which date each field is valid** (TickerHistory3.parquet starts 2012-03-26 per atx-db's audit; dates on the NYSE rule calendar):

- `ceq_iss_5y`: finite from about **2017-06-29**. The first b with a share observation at least 90 days old is 2012-06-25, so a = 2017-06-28. That is before the role start 2018-06-01, so every role session can be finite for a line with history (row 0 reads b = 2013-05-30).
- `coskew_60m`: 48 months are reachable from 2016-03-31 and 60 from 2017-03-30. Every role session has its full 60-month window.
- `ret_*`: from role row 0, if the open exists. `vol_126`: from role row 126 (2018-11-29 on the 2018-06-01 role). `xrd0_ttm`: as `xrd_ttm` / `sale_ttm`.
- Hence all six are defined throughout TRAIN 2020-2023.

## How root verifies

1. Tests:
   ```
   cd atx-engine/tools
   "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_research_fields_price.py test_prepare_research_fields.py test_prepare_research_fields_sec.py test_research_fields_holdings.py test_prepare_research_fields_sic.py test_prepare_research_fields_sv.py
   ```
   Here: 89 passed (7 new).
   - `test_field_at_t_unchanged_when_rows_after_t_mutate`: every field. Vendor rows dated on or after session t (off-axis lines included) and role rows are mutated; rows <= t stay bit-identical, and later rows do move.
   - `test_ceq_iss_split_invariant`: a twin line with 2:1, 3:1 and 1:2 splits gives equal values on every row, and they match the definition.
   - `test_coskew_matches_numpy_reference`: an independent Python/numpy market and lstsq regression.
   - Also: `test_factor_break_step_is_chained`, a 52-line mass re-anchoring that is repaired so the result equals the world without it; `test_values_match_definitions`; `test_open_capability_and_refusals`, which includes `FieldNeedsOpen`, the SHA mismatch and the CLI; `test_producers_cover_every_field`.
2. Identity:
   - **v9 fields stay reusable.** Every built-in field group's `producer_fingerprints` is unchanged by the hook: I compared HEAD and the new builder source, and all six groups are equal. With `--reuse`, v9 payloads are therefore copied byte for byte.
   - **Without `--reuse`, v9 payloads are still unchanged.** The module runs after every built-in group and writes only its own files.
   - **Check:** every v9 field's `sha256` in the v10 manifest equals the v9 manifest's.
   - The new fields are opt-in: they are not in `DEFAULT_FIELDS`.
3. Build fields v10: the v9 argv plus
   ```
   --fields <v9 list>,ret_overnight,ret_intraday,ceq_iss_5y,coskew_60m,vol_126,xrd0_ttm
   --reuse <fields-v9 dir> --reuse-sha256 <its manifest sha> --reuse-hardlink
   --price-source C:/Users/natha/Downloads/TickerHistory3.parquet
   --max-rss-mib 1200
   ```
   - `xrd0_ttm` needs `xrd_ttm` and `sale_ttm` in the list (they are in v9).
   - If the build stops with `FieldNeedsOpen`, drop `ret_overnight,ret_intraday`: that is the OD-6 withdrawal.

## Deviations, with reasons

1. **`prepare_recent_research.py:71` is not changed.** Adding `open` to the projection would rewrite every role, a new projection and new role SHAs, and it fails on any source without `open`. Reading the open from the pinned source keeps every role byte-identical.
2. **The t-1 clock is applied literally.** Each formula's "t" is the session t-1 of the field row: `ret_overnight[t]` is session t-1's overnight return. This is the house `t-1` rule of the SEC and holdings modules.
3. **The `coskew_60m` market is the vendor-wide equal-weight daily market** (reason above). Monthly returns use only the raw-versus-adjusted divergence guard, not the 1.5 magnitude bound, which is meant for daily data errors. Raw returns are used with no risk-free rate.
4. **Declared constants, not searched:** `COSKEW_MIN_MONTHS = 48` (80%), `VOL_MIN_SESSIONS = 63`, and the `ceq_iss_5y` domain `[-ln 100, ln 100]`. The domain catches a vendor share-units defect (1000x = 6.9 in log) and keeps genuine 100x issuers.
5. **`vol_126` is raw share volume**, like the DSL's `volume`, without split restatement. It stands in for `ts_mean(volume, 126)` lagged one session.
6. **Registration is at the end of `prepare_research_fields.py`**, after the holdings hook, rather than at lines 501-505, which C-3 is editing. `FIELD_MODULES` and `ALL_FIELDS` are read at call time, so the position is equivalent.

## Cross-lane edits

- `atx-engine/tools/prepare_research_fields.py` (lane C / C-3 file): 5 lines at the end of the file (`import research_fields_price`, `FIELD_MODULES.append(...)`, `ALL_FIELDS.update(...)`). I made no other change there and did not touch the seal lines (W0-1).
- `PRODUCERS` is exported for C-3's fingerprinting. Until C-3 lands, module fields are never reused, as today; that is safe.

## Open risks

- **Open availability is unconfirmed** until root's first run. The check runs before any output, so the failure mode is a clean refusal.
- **Memory.** The panel is about 3,003 x 5,627 x 22 B ≈ 370 MB for the 4-year role (admitted by `budget.admit`), plus a 62 MB share index. The DuckDB market query is capped at half the remaining RSS and runs before the panel is built, but allocator memory may linger after it. Hence `--max-rss-mib 1200`.
- **Runtime [est].** The source is hashed once (3.6 GB, about 20 s). The panel scan takes about the same as the `th` group's scan (about 60 s). The DuckDB market (about 24M rows, one thread) is about 60-120 s. Coskew and ceq rows take about 10 s.
- **The seal comes from the builder's `SEAL` / `SEAL_NS` / `day_of(SEAL)`** through the host namespace; nothing is hard-coded. When W0-1 replaces those constants with `research_window`, this module follows, as long as the builder keeps the names `SEAL` and `SEAL_NS`.
- **The market includes ETFs, ADRs and preferred lines** (every vendor securityID). This is documented in the caveats.
