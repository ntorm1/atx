# tbltickerhistory schema + validation cohort (data recon)

**Date:** 2026-07-18. Confirms units/semantics and locks the reproduction cohort.

## Schema / units (CONFIRMED on real rows)
- `tbltickerhistory3_10y.txt`: TAB-delim, CRLF, **71 cols** (nothing past `qtrD1`), **31,598,499
  rows**, dates **2012-03-26 .. 2026-06-15**, ~11.8k symbols/date (superset of earnings file).
- `atmCenI_*`, `atmCenH_*` = **decimal annualized vol** (NVDA atmCenI_21d=0.395=39.5%). NOT percent.
- `iEMove`, `hEMove` = **decimal move fraction** (NVDA iEMove=0.0665 = 6.65% event move).
- `nEarnCnt` = 8 (all forward slots). **`nEarnCnt_Nd` = integer earnings count within an
  N-TRADING-DAY horizon** — the censoring signal. TENORS ARE TRADING DAYS (21/mo, 63/qtr, 252/yr).
- `ccVar` close-close daily var; `hlVar` Parkinson HL daily var; `rvVar` = **N/A, do not use**.
- `GICS` (col 62) = proprietary small-int bucket, NOT standard GICS. Non-blocking.

## Earnings file (`tblstockearnforecasthist_v2.00_2026-02-10`)
- TAB, CRLF, **59 cols**, 6507 rows, single as-of 2026-02-10. Layout: ticker_at, ticker_ts,
  **ticker_tk**, tradingDate, nearEarnDate(+_us/Type/Time), nextEarnDate1Adj/2Adj(+_us),
  **nextEarnDate1..8(+_us/Type/Time)**, timestamp(+_us), *_cst mirrors, securityID.
- Main datetimes = **UTC** `YYYY-MM-DD HH:MM:SS.ffffff`. `_cst` = US Central DST-aware mirror.
  `_us` = 0/1 flag (NOT a timestamp). `nextEarnTypeN` = status {Announced|Estimate};
  `nextEarnTimeN` = session {AMC|BMO}. AMC=17:00 ET, BMO=08:30 ET (verified).

## CORRECTIONS to design/plan (folded in)
1. **Tenor grid = TRADING days.** Convert tenor N → T by advancing N NYSE trading days from
   `now` (vol_time.hpp VolTimeCalendar), then `time_to_expiry_years(now, target, convention)`.
   NOT N/365.25.
2. **`atmCenI_Nd` = RAW censored-space interpolation at the tenor, NOT the parametric read.**
   (NVDA model lt+(st−lt)e^(−decay·T) at 21d ≈ .372 vs actual atmCenI_21d=.395.) So `_Nd` is the
   primary target produced by S2 (censored-space interp) + S3 (SR trading-day tenor grid).
   `st/lt/decay` is a SEPARATE smooth summary fit (Task 4). Reproduce BOTH; `_Nd` is the tight bar.
3. **nEarnCnt counting is trading-day-horizon** — count events within N trading days of `now`.

## Validation cohort — 10 names × 2 dates (2026-02-10 + 2026-02-17)
All OPRA-liquid, all report >= 2026-02-18 (both dates clean), span short/mid/long censoring,
iEMove 2%–10%. Truth values as-of 2026-02-10 (vol decimal; iE/hE decimal move):

| ticker | secID | earn (UTC) | sess | status | close | iEMove | hEMove | nEC 21/42/63 | aI 21/42/63/252 | st/lt/decay |
|--|--|--|--|--|--|--|--|--|--|--|
| NVDA | 40678 | 2026-02-25 | AMC | Announced | 188.54 | 0.0665 | 0.0572 | 1/1/1 | .395/.415/.421/.442 | .371/.444/.225 |
| CRM | 123898 | 2026-02-25 | AMC | Announced | 193.45 | 0.0970 | 0.0614 | 1/1/1 | .413/.399/.398/.373 | .454/.377/.208 |
| WMT | 44366 | 2026-02-19 | BMO | Announced | 126.70 | 0.0566 | 0.0549 | 1/1/1 | .253/.256/.251/.255 | .253/.255/.099 |
| HD | 37945 | 2026-02-24 | BMO | Announced | 389.68 | 0.0448 | 0.0291 | 1/1/1 | .245/.236/.234/.230 | .254/.228/.226 |
| AVGO | 1510642 | 2026-03-04 | AMC | Announced | 340.44 | 0.0965 | 0.0948 | 1/1/1 | .489/.487/.485/.467 | .478/.467/.150 |
| MU | 39821 | 2026-03-18 | AMC | Estimate | 373.25 | 0.0843 | 0.0971 | 0/1/1 | .681/.693/.688/.674 | .717/.664/.519 |
| ADBE | 32839 | 2026-03-12 | AMC | Announced | 264.67 | 0.0923 | 0.1050 | 0/1/1 | .399/.398/.400/.378 | .406/.378/.187 |
| AAPL | 33449 | 2026-04-30 | AMC | Estimate | 273.68 | 0.0208 | 0.0207 | 0/0/1 | .227/.229/.243/.259 | .217/.265/.306 |
| TSLA | 1914754 | 2026-04-21 | AMC | Estimate | 425.21 | 0.0912 | 0.0919 | 0/0/1 | .412/.433/.433/.480 | .390/.497/.436 |
| AMD | (tbd) | 2026-05-05 | AMC | Estimate | — | — | — | 0/0/1 | — | — |

CSCO/ABNB/COIN/SHOP available as extras (ABNB has nEC63=2 — two prints in 63 td).
Slices saved: scratchpad `th_2026-02-10.tsv` (11844 rows), `th_2026-02-11.tsv`, `th_2026-02-17.tsv`.

## Databento status
- **NO DATABENTO_API_KEY configured** anywhere in the environment/repo. Python `databento`
  v0.79.0 installed; vendored `databento-cpp` v0.59.0 in submodule.
- **Not needed**: all 10 cohort names free on-disk (cbbo-1m) for 2026-02-10 AND 2026-02-17.
  On-disk snapshot ts = 14:55 ET (19:55 UTC). SpiderRock EOD ~16:00 ET → ~65-min timing gap.
- IF the timing residual dominates under the tight bar: OPRA.PILLAR `cbbo-1s` 15:45–16:00 ET
  (20:45–21:00Z) parent-symbol window ≈ **$2–$12 total** for 20 slices; $125 signup credit
  covers it. Requires the user to create an account + set DATABENTO_API_KEY (cannot self-serve).
  Always run the FREE `metadata.get_cost` first.
