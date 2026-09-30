# W0-2 runbook: role and fields for TRAIN 2020-2023 (seal 2024-01-01)

Read-only investigation, 2026-09-29, pool-2 `feat/platform-v8-20260929` (HEAD ef11f462). Nothing was built, run or
deleted. Sources: `build-equity/*-run/receipt.json` (argv, seconds, sampled peak), manifests (pins, date ranges),
`scripts/specs/*.json`, tool source, platform-20260928 reports (L9, U2, W5a, W5b) and progress ledgers. Where no
receipt exists (fields v8/v9, lo3 role, lo3 fields v7 were built "direct"), the argv below is reconstructed from the
reports, the spec and the output manifest and is marked **reconstructed**.

Hidden-data note: no file or statistic dated 2024-01-01 or later is needed by any step below except the sealed-row
counters that the tools already write (see Q1, item "counters"). Disclosure: while scanning atx-db stage manifests
for date ranges my first filter printed per-year coverage counts (2009-2026) from `earnings_calendar/manifest.json`
and `sec_filings/manifest.json`; they are data-coverage counts, not returns or IC, and nothing here uses them.

---

## 0. Summary

Chain (every arrow is one bounded run; pool-2 artifacts in `build-equity/`):

```
TickerHistory3.parquet ──project──> recent-projection-v2 ──role──> train-2020-2023-base-v1 ──repair──> train-2020-2023-base
r4 export (atx-db) ──prepare_identity_bridge --seal 2024-01-01──> identity-bridge-r4-v2 ──ciks.txt──┐
CF-R + FSDS (atx-db staging) ──build_fundamental_events prepare/events x4/finalize──> fundamental-events-v3 <─┘
base + r4-v2 + fe-v3 ──role linked-operating-v1──> train-2020-2023-lo1 ──fields (63)──> train-2020-2023-lo1-fields-v9
base + v2-pit + stage SIC + delisting ──role linked-operating-v3──> train-2020-2023-lo3 ──fields (63)──> ...-lo3-fields-v9
fields builder also reads (unchanged, atx-db / vendor, reader-side seal): TickerHistory3 (IV, earn, shares), FINRA SI,
FINRA raw short volume, earnings_calendar, insider, sec_filings, thirteenf, ftd, regsho_threshold, security_master,
short_volume_ext (+ fundamentals stage and v2-pit bridge for lo3).
```

Must be rebuilt for the new window: projection (forced: `prepare_recent_research.py:439`), base role (both stages),
identity-bridge r4 (brief step 1), fundamental events (recommended; needs a W0-1 fix first), roles lo1/lo3, fields v9
on both roles (no reuse is possible: `REUSE_RULE` binds a prior fields dir to the exact role). Not rebuilt: every
atx-db stage, the vendor file and the FINRA files (they already cover 2023; seal applied at read time).

Blockers found (details in section 5 and the open-questions file):
1. `regsho_threshold` stage was republished: live manifest `fb073c62...` != the pin in fields-v9 / L9 (`68f431f0...`).
2. `build_fundamental_events.py:73 LAST_SUB_QUARTER = "2024q4"` is a second seal constant, not in W0-1's list.
3. `research_fields_sec.py:574-577` opens `insider/transactions/year=2024/2024q1.parquet` for a role ending 2023-12-29.
4. `backtest_integrity.py:47 TRAIN_END_EXCLUSIVE = 2023-01-01` (nav_summ ledger) refuses any 4-year series: B0 summ
   fails until it reads the window; it is outside W0-1's file list and grep gate (`.superpowers/.../studies/`).
5. The brief's one-line role command cannot run: linked universes restrict an existing `--base-role` and refuse
   `--cache` (`prepare_recent_research.py:957-961`); hence the base-role chain R1-R3.
6. W0-3's `protocol` ledger line has no `cell`; `research_cycle.ledger_cells` raises on it (`research_cycle.py:585`).

IC admission, 4-year role, library v7.1 (48 candidates, max slots 8, resident capacity 6, 4 workers): about
1,952 MiB at 6,100 instruments (1,887 MiB with slots 7); every union the role builder can admit (<= 8,000) fits
2,560 MiB. Details in Q2.

---

## 1. Preconditions (check before R1)

- W0-1 merged into pool-2: `research_window.json` exists and every SEAL constant reads it. Verify:
  `"$PY" -c "import sys; sys.path.insert(0,'atx-engine/tools'); import research_window as w; print(w.SEAL_DATE)"` prints
  `2024-01-01`; `"$PY" -c "import sys; sys.path.insert(0,'atx-engine/tools'); import prepare_recent_research as p, prepare_research_fields as f, research_fields_holdings as h, prepare_identity_bridge as b; print(p.SEAL, f.SEAL, h.SEAL, b.SEAL)"`
  prints `2024-01-01` four times.
- Also required before the step that uses them (not in the W0-1 file list; see blockers 2-4):
  - before R5: `build_fundamental_events.LAST_SUB_QUARTER == "2023q4"` (quarter before the seal) and `SEAL` from the window.
  - before R10/R11: the insider reader skips filing quarters that start on or after the seal (one-line guard at
    `research_fields_sec.py:575`: `or qday >= <seal date>`). Without it the builder opens a 2024-named file.
  - before B0 summ (W0-4): `backtest_integrity.TRAIN_END_EXCLUSIVE` from the window.
- IC and NAV exes rebuilt after W0-1 through the tracked build script (E-1). The current exe accepts a role ending
  2024-01-01 (old C++ seal 2025, `strategy_data.cpp:21,101`), so R12-R13 would also run on it, but build first.
- Tree clean (`git status --porcelain` empty; the bounded runner refuses otherwise, `run_bounded_research.py:91-93`).
- Free disk >= 30 GB (C: had 69.9 GiB free at 2026-09-29); free RAM >= 2.5 GB before R13 (IC RSS ~1.6 GB + 512 MiB floor).
- Re-hash the atx-db stage manifests immediately before R8/R10/R11 (the stages are live; see R0 pins).

## 2. Shell variables (Git Bash, pool-2 root)

```bash
cd C:/atx-wt/pool-2
PY="C:/Program Files/Python312/python.exe"
sha() { sha256sum "$1" | cut -c1-64; }
V1=C:/atx/atx-db/data/alpha_panel/v1
TH=C:/Users/natha/Downloads/TickerHistory3.parquet          # 3,617,973,507 B, mtime_ns 1789920127331396300 (unchanged)
FINRA=C:/atx/data/finra_short_interest                      # asof/manifest.json a2561d75... (unchanged)
SVRAW=C:/atx/atx-db/data/raw/finra_short_volume             # CNMSshvol20180801 .. 20260918, 250 files in 2023
IC=build-equity/bin/atx-equity-strategy-ic.exe
LIB=atx-impl/strategies/fund_industry_ic_v71.json
LIBS=787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259
# atx-db stage manifest pins, hashed 2026-09-29 (re-hash before use: `sha $V1/<stage>/manifest.json`)
V2PIT=09aac28f757fa959b0ed4cd9296b2267940e70af98e0d2c67cc45b1df4f7fa01   # export/identity-bridge-v2-pit (= L9)
EC=9a4a976b03d0ad04672796f01abc129d0d09e3db23d62ddf57aea68ae3c7d769      # earnings_calendar (= L9)
INS=dcd3f1aa4ba6e266c03ef78568ca131c1336f51ade477f03faff2e88a62ba061     # insider (= L9)
SECF=5190fe99e4c2f1f13218d966a67d06995d51b7a31a73151dad2686e829aed693    # sec_filings (= L9)
F13=8974170f64b4a002cc1b131449c2abf0c7daaab23d4a256992afbdfc4105ffb0     # thirteenf (= L9)
FTD=a76d69bed49829d9e14216f1abe2ef76b480c16c576fa49ae63fa2a28050f945     # ftd (= L9)
REGSHO=fb073c6222cb16cb27968c067d45ecd2ffe50cf10958391cae8d6417ad9e6f4f  # regsho_threshold LIVE; L9/fields-v9 pin was 68f431f0... (BLOCKER 1)
SECM=3afe06605adac9aa85494cc5d1e7307194392954ad3b6ba8142b281fa62a414a    # security_master (= L9)
SVX=7007a13c226a1730d0ef778d7201bf7c1d0e97ed61d4c12af32c1c4567444928     # short_volume_ext (= L9)
SIC=9f9b2f85f6bcd5c7f3a55aee097893094a5cb85ab2b4edbfb582297dab06816b     # fundamentals stage (sic_events) (= L9)
DL=1b1166b61e5a77d8dbe007f2de3261392424fb86a59c1118028862abc264c37f      # delisting (= U2)
F63=si_shares,si_dtc,iv_atm_21d,iv_atm_63d,iv_atm_126d,earn_recent,shares_out,mkt_ret,be,at,at_lag4,lt,che,debt,sale_ttm,gp_ttm,oi_ttm,ni_ttm,ni_q,ni_q_lag4,be_lag1q,be_lag1q_lag4,cfo_ttm,capx_ttm,xrd_ttm,dvc_ttm,prstkc_ttm,sstk_ttm,txt_q,txt_q_lag4,shrs_q,shrs_q_lag4,noa,noa_lag4,sue,fscore,me_company,grp_sic2,grp_ff12,grp_ff49,sv_ratio126,ea_days_to_expected,ea_days_since,ea_window_pre5,ea_window_post3,ea_delay_days,ea_time_of_day,ins_net_buy_ratio,ins_n_buyers,ins_n_sellers,ins_opportunistic_net,ins_cluster_buy,k8_count_63,k8_item_material_21,k8_days_since_any,inst_own_share,inst_breadth_chg,inst_own_chg_q,inst_best_ideas,inst_n_holders,ftd_shares_ratio21,regsho_threshold_days63,sv_offexchange_share126
```

`F63` is `scripts/specs/v71.json` `fields.list` in order (63 names; the as-built pin check requires the same order).
Names deliberately avoid the substring `2024` (the report seal regex refuses such paths until E-4 lands).

---

## 3. Steps

Each step: purpose, command (bounded exactly as the old receipt), old cost from the receipt, expected 4-year cost,
output size, and the pin to record. "4-year factor" = cells ratio (1,405 x n) / (1,155 x 5,627) = 1.319 at n = 6,100.

### R1. Source projection (replaces recent-projection-v1)

Old receipt `recent-projection-v1-run` (src e2ef9c27): `prepare_recent_research.py project --source TickerHistory3
--start 2018-06-01 --end 2025-01-01 --memory-mib 768 --disk-mib 12288 --max-seconds 270`, limits 300 s / 1,100 MiB /
512; 39.2 s, peak 759 MiB; output 425 MiB (accepted.parquet 225 MB, projection.duckdb 200 MB). It holds sessions
through 2024-12-31 and, after W0-1, `create_role` refuses it (`:439`, see Q1).

```bash
"$PY" scripts/run_bounded_research.py --seconds 300 --max-rss-mib 1100 --min-free-mib 512 \
  --output build-equity/recent-projection-v2-run --bind atx-engine/tools/prepare_recent_research.py -- \
  "$PY" atx-engine/tools/prepare_recent_research.py project --source $TH \
  --out C:/atx-wt/pool-2/build-equity/recent-projection-v2 --start 2018-06-01 --end 2024-01-01 \
  --memory-mib 768 --disk-mib 12288 --max-seconds 270
PROJ=$(sha build-equity/recent-projection-v2/manifest.json)
```

Expect ~35-40 s, ~760 MiB (the whole vendor file is scanned either way), ~360 MiB on disk. Check: manifest
`end_exclusive` "2024-01-01"; `session_days` = 1,405 entries from 2018-06-01 (the v1 manifest has 1,405 sessions in
[2018-06-01, 2024-01-01), 250 of them in 2023, last 2023-12-29).

### R2. Base role, default universe (replaces recent-fast-train-2020-2022-v1)

Old receipt `recent-fast-train-2020-2022-v1-run` (src 527add1c): limits 180 s / 1,024 MiB / 768; 12.3 s, 320 MiB;
output 161 MiB.

```bash
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1024 --min-free-mib 768 \
  --output build-equity/train-2020-2023-base-v1-run --bind atx-engine/tools/prepare_recent_research.py \
  --bind build-equity/recent-projection-v2/manifest.json -- \
  "$PY" -B atx-engine/tools/prepare_recent_research.py role --cache build-equity/recent-projection-v2 \
  --out build-equity/train-2020-2023-base-v1 --start 2018-06-01 --score-start 2020-01-01 --end 2024-01-01 \
  --top-n 3000 --max-union 8000 --memory-mib 768 --disk-mib 1024 --max-output-mib 512 --max-seconds 120
BASE1=$(sha build-equity/train-2020-2023-base-v1/manifest.json)
"$PY" -c "import json;m=json.load(open('build-equity/train-2020-2023-base-v1/manifest.json'));print(m['dates'],m['instruments'],m['score_begin'],m['score_end_ns'])"
```

Expect `1405 <n> 399 1704067200000000000`; n >= 5,627 (estimate 6,100; record it for Q2). ~15 s, ~380 MiB, ~213 MiB.
Overlap check (cheap, exact): `score_member_counts[:756]` must equal the v1 role's list (membership at t reads only
t-1 and earlier; rows before 2023 are identical in both projections).

### R3. Factor-break repair (replaces recent-fast-train-2020-2022-v2)

Old receipts `mega-role-scan-train-run` (1.7 s, 156 MiB) and `mega-role-repair-train-run` (src 99421a5f; limits
120 s / 1,536 MiB / 512; 2.45 s, 315 MiB; output 162 MiB). Rule v1 (the default) is the 3-year role's rule; do not
pass `--rule v2`. The ledger records the 2023-2024 validation scan as clean (mega-alpha progress.md:1147,1189), so the
expected mass set is still {2021-01-04}; a new union column can push a non-mass session past the 50-cell threshold
(T12 review: margin 44/47), which the scan shows first.

```bash
"$PY" scripts/run_bounded_research.py --seconds 120 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/train-2020-2023-scan-run --bind atx-impl/tools/repair_role_factor_breaks.py \
  --bind build-equity/train-2020-2023-base-v1/manifest.json -- \
  "$PY" -B atx-impl/tools/repair_role_factor_breaks.py --scan-only --role build-equity/train-2020-2023-base-v1 --role-sha256 $BASE1
# proceed only if the v1 verdict (stdout.log of that run) lists exactly one mass session, 2021-01-04
"$PY" scripts/run_bounded_research.py --seconds 120 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/train-2020-2023-base-run --bind atx-impl/tools/repair_role_factor_breaks.py \
  --bind build-equity/train-2020-2023-base-v1/manifest.json -- \
  "$PY" -B atx-impl/tools/repair_role_factor_breaks.py --role build-equity/train-2020-2023-base-v1 --role-sha256 $BASE1 \
  --out build-equity/train-2020-2023-base --expect-sessions 2021-01-04
BASE=$(sha build-equity/train-2020-2023-base/manifest.json)
```

Expect ~2 s / ~200 MiB (scan), ~3.5 s / ~420 MiB (repair), ~213 MiB. If another mass session appears, stop: a repair
at a later session rescales earlier closes of the repaired names and breaks the W0-a overlap.

### R4. Identity bridge r4 with the new seal (brief step 1)

Old receipt `identity-bridge-r4-v1-run` (src 791f1e4b): no bindings; 1.6 s, 143 MiB; output 1.7 MiB. The v1 bridge
has seal 2025-01-01 and `max_end_incl` 2024-12-31. Source r4 manifest `ac9bcda7...` re-hashed today: unchanged.

```bash
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/identity-bridge-r4-v2-run -- \
  "$PY" atx-engine/tools/prepare_identity_bridge.py --output build-equity/identity-bridge-r4-v2 \
  --expect-source-sha256 ac9bcda74b01badf507351a53b8b078c733ce00a7cc01a0dcf33d68935a6bc28 --seal 2024-01-01
R4=$(sha build-equity/identity-bridge-r4-v2/manifest.json)
# optional diagnostic (old: identity-bridge-r4-v1-check-run, 4.0 s, 778 MiB). ALWAYS pass --role: the default roles
# include recent-fast-validation-2023-2024-v1 (2024 sessions, prepare_identity_bridge.py:97-98). It opens the live
# atx-db warehouse read-only; skip it while the atx-db session holds a write lock.
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/identity-bridge-r4-v2-check-run -- \
  "$PY" atx-engine/tools/prepare_identity_bridge.py --check build-equity/identity-bridge-r4-v2 \
  --role build-equity/train-2020-2023-base --static-warehouse C:/atx/atx-db/data/warehouse.duckdb
```

Check: manifest `seal` "2024-01-01", `counts.max_end_incl` <= "2023-12-31". Link rows for every date before 2024 are
the same as v1's (the rule is evaluated per date d < seal), so lo1 membership on 2018-2023 is unchanged; `ciks.txt`
loses the CIKs whose only visible evidence is dated 2024 (this is what scopes R5).

### R5. Fundamental events (replaces fundamental-events-v2; needs precondition "LAST_SUB_QUARTER")

Old receipts `fundamental-events-v2-run-{prep,b0-20,b21-41,b42-62,b63-84,fin}` (src 6154deaf, no bindings, limits
180 s / 1,536 MiB / 512): 6.7 s / 467 MiB, 22.1 / 651, 23.5 / 646, 20.0 / 658, 17.9 / 617, 3.0 / 305 (93 s total);
output 55.9 MiB. Staging pins re-hashed today: companyfacts `50e018e1...` and FSDS `fsds-staging-manifest.json`
`2cad6134...` unchanged.

```bash
FE=build-equity/fundamental-events-v3
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 --output $FE-run-prep -- \
  "$PY" atx-engine/tools/build_fundamental_events.py prepare --out $FE --cik-list build-equity/identity-bridge-r4-v2/ciks.txt \
  --companyfacts-dir C:/atx/atx-db/data/staging/companyfacts/ee099c7394a357f1 \
  --companyfacts-manifest-sha256 50e018e1c26046f3eb3ec27d2f246c8492e60baddbc911ebfda50320f24ac186 \
  --fsds-dir C:/atx/atx-db/data/staging/fsds-v2 --fsds-manifest-sha256 2cad6134efd312d7ad9ca84bbc274fdf360e10e581b38aa88a29f1040bebc628
for B in 0-20 21-41 42-62 63-84; do
  "$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 --output $FE-run-b$B -- \
    "$PY" atx-engine/tools/build_fundamental_events.py events --out $FE --batches $B
done
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 --output $FE-run-fin -- \
  "$PY" atx-engine/tools/build_fundamental_events.py finalize --out $FE
FEV=$(sha $FE/manifest.json)
```

Check: manifest `seal` and `parameters.seal` "2024-01-01"; the last SUB quarter listed in its run/prepare records
is 2023q4. Expect
~90 s total, <= ~660 MiB, ~55 MiB. Fallback if R5 is skipped: keep `fundamental-events-v2` (74ed9a50...) and rely on
the reader filter (Q1); lo1 values are identical either way, lo3 differs only through the CIK scope (Q1, item 4).

### R6. (Optional, mirrors the old lo1 receipt) grp cross-check fields on the base role

The old lo1 role passed `--check-fields recent-fast-train-2020-2022-v2-fields-v6` (40 fields on the base role, 41.9 s,
637 MiB). The cross-check needs grp_ff12 built on the base role; a grp-only build is enough.

```bash
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/train-2020-2023-base-grp-run --bind build-equity/train-2020-2023-base/manifest.json -- \
  "$PY" atx-engine/tools/prepare_research_fields.py --role build-equity/train-2020-2023-base --role-sha256 $BASE \
  --output build-equity/train-2020-2023-base-grp --fields grp_sic2,grp_ff12,grp_ff49 \
  --identity-bridge build-equity/identity-bridge-r4-v2 --identity-bridge-sha256 $R4 \
  --fund-events $FE --fund-events-sha256 $FEV --fund-lag-sessions 1 --max-rss-mib 1536 --max-seconds 175
GRP=$(sha build-equity/train-2020-2023-base-grp/manifest.json)
```

Expect ~10-20 s, ~400 MiB, ~196 MiB.

### R7. Role lo1 = linked-operating-v1

Old receipt `recent-fast-train-2020-2022-v2-lo1-run` (src 8ec2d4c7; limits 180 s / 1,536 MiB / 512; 4.8 s, 211 MiB;
output 161 MiB): `role --universe linked-operating-v1 --base-role ...-v2 --base-role-sha256 210fff96... --identity-bridge
identity-bridge-r4-v1 --identity-bridge-sha256 ddf97164... --sic-events fundamental-events-v2 --sic-events-sha256
74ed9a50... --check-fields ...-v2-fields-v6 --check-fields-sha256 32565c32... --memory-mib 1024 --max-seconds 170`.

```bash
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/train-2020-2023-lo1-run --bind build-equity/train-2020-2023-base/manifest.json \
  --bind build-equity/identity-bridge-r4-v2/manifest.json --bind $FE/manifest.json -- \
  "$PY" atx-engine/tools/prepare_recent_research.py role --universe linked-operating-v1 \
  --base-role build-equity/train-2020-2023-base --base-role-sha256 $BASE \
  --identity-bridge build-equity/identity-bridge-r4-v2 --identity-bridge-sha256 $R4 \
  --sic-events $FE --sic-events-sha256 $FEV \
  --check-fields build-equity/train-2020-2023-base-grp --check-fields-sha256 $GRP \
  --out build-equity/train-2020-2023-lo1 --memory-mib 1024 --max-seconds 170
LO1=$(sha build-equity/train-2020-2023-lo1/manifest.json)
"$PY" -c "import json;o=json.load(open('build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json'))['universe']['kept_member_counts'];n=json.load(open('build-equity/train-2020-2023-lo1/manifest.json'))['universe']['kept_member_counts'];print(len(o),len(n),o==n[:len(o)])"
```

Drop the two `--check-fields` lines if R6 was skipped. Expect ~6 s, ~280 MiB, ~213 MiB; the one-liner must print
`1155 1405 True` (kept members per session identical on the 3-year sessions).

### R8. Role lo3 = linked-operating-v3

No receipt (built direct by root; U2 report measured 2.6 s / 320 MiB in pool-9; output 162 MiB). Argv from the U2
report, the v7u-lo3.json `template.role_command` and the lo3 manifest's universe inputs.

```bash
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/train-2020-2023-lo3-run --bind build-equity/train-2020-2023-base/manifest.json \
  --bind $V1/export/identity-bridge-v2-pit/manifest.json --bind $V1/fundamentals/manifest.json \
  --bind $V1/delisting/manifest.json -- \
  "$PY" atx-engine/tools/prepare_recent_research.py role --universe linked-operating-v3 \
  --out build-equity/train-2020-2023-lo3 --base-role build-equity/train-2020-2023-base --base-role-sha256 $BASE \
  --identity-bridge $V1/export/identity-bridge-v2-pit --identity-bridge-sha256 $V2PIT \
  --sic-events $V1/fundamentals --sic-events-sha256 $SIC \
  --delisting $V1/delisting --delisting-sha256 $DL --memory-mib 1024 --max-seconds 170
LO3=$(sha build-equity/train-2020-2023-lo3/manifest.json)
```

Same one-liner as R7 with `lo3` paths must print `1155 1405 True`. Expect ~3.5 s, ~420 MiB, ~213 MiB; manifest
~560 KB (3-year 448.5 KB; C++ readers cap metadata at 1 MiB, `strategy_data.cpp:22`). `--delisting-returns` stays off
(payloads byte for byte); it is B0c's variant (R15).

### R9. (Optional) admission probe before the fields exist (brief step 3, early)

`--plan-only` binds the fields manifest after `admit()` (`strategy_ic_runner.cpp:2366-2368`), so without fields it
stops at `bind_fields`. A deliberately low cap makes `admit()` itself refuse and print the number
(`:654-656`: "IC runner: required_bytes=N max_compiled_slots=8 exceeds configured memory budget before payload load").
Metadata only; the receipt outcome is `process-error` by design.

```bash
for R in lo1 lo3; do
  "$PY" scripts/run_bounded_research.py --seconds 60 --max-rss-mib 512 --min-free-mib 512 \
    --output build-equity/w0-2-admit-probe-$R-run --bind $IC --bind $LIB --bind build-equity/train-2020-2023-$R/manifest.json -- \
    $IC --library $LIB --library-sha256 $LIBS --train build-equity/train-2020-2023-$R/manifest.json \
    --train-sha256 $(sha build-equity/train-2020-2023-$R/manifest.json) --max-memory-mib 64 --min-names 1000 --workers 4 --plan-only
done
```

Or compute it from n (exact formula, reproduces 1,553,063,994 for the 3-year role):
`"$PY" -c "d,n,b,K,s,c,w=1405,<n>,399,48,8,6,4;x=d*n;sd=d-b;L=sum((sd-h-1)*(n*16+32) for h in (5,21,63));print(32*2**20+x*(72+8*s)+(4096+9*x+40*d+32*n+512*K)+512*(d+n)+x*(8*c+1)+L+w*((8<<20)+(64<<10))+w*n*1104+w*d*64)"`.
Stop (OD-2 / H-1) only if it exceeds 2,684,354,560 B.

### R10. Fields v9 on lo1 (brief step 4) -- reconstructed argv

No receipt: lo1-fields-v9 was built direct (progress.md:234: 63 fields, 121 s, peak 571 MiB, 40 fields hardlinked
from v8, which reused v7). Argv = the W5a root command (v8) + the W5b holdings flags + the v9 manifest's sources and
stage pins. On the new role nothing can be reused (`REUSE_RULE`: the prior manifest must be bound to this very role),
so all 63 fields are computed: 3-year no-reuse references are W5a 55 fields 67.9 s / 645 MiB sampled (832 MiB working
set) and W5b holdings 76 s / 536 MiB. Expect ~150-200 s and ~0.9-1.1 GB at 4 years: above the 180 s default cap,
hence 600 s / 2,560 MiB below (needs a root ruling; OD-2 covers the IC pass only).

```bash
"$PY" scripts/run_bounded_research.py --seconds 600 --max-rss-mib 2560 --min-free-mib 512 \
  --output build-equity/train-2020-2023-lo1-fields-v9-run \
  --bind atx-engine/tools/prepare_research_fields.py --bind atx-engine/tools/research_fields_sec.py \
  --bind atx-engine/tools/research_fields_holdings.py --bind build-equity/train-2020-2023-lo1/manifest.json -- \
  "$PY" atx-engine/tools/prepare_research_fields.py --role build-equity/train-2020-2023-lo1 --role-sha256 $LO1 \
  --output build-equity/train-2020-2023-lo1-fields-v9 --fields $F63 \
  --finra $FINRA --tickerhistory $TH --finra-short-volume $SVRAW \
  --identity-bridge build-equity/identity-bridge-r4-v2 --identity-bridge-sha256 $R4 \
  --fund-events $FE --fund-events-sha256 $FEV --fund-lag-sessions 1 \
  --sec-stages $V1 --sec-identity-bridge $V1/export/identity-bridge-v2-pit --sec-identity-bridge-sha256 $V2PIT \
  --earnings-calendar-sha256 $EC --insider-sha256 $INS --sec-filings-sha256 $SECF \
  --thirteenf $V1/thirteenf --thirteenf-sha256 $F13 --ftd $V1/ftd --ftd-sha256 $FTD \
  --regsho-threshold $V1/regsho_threshold --regsho-threshold-sha256 $REGSHO \
  --security-master $V1/security_master --security-master-sha256 $SECM \
  --short-volume-ext $V1/short_volume_ext --short-volume-ext-sha256 $SVX \
  --max-rss-mib 2048 --max-seconds 580
F1=$(sha build-equity/train-2020-2023-lo1-fields-v9/manifest.json)
```

Output ~4,120 MiB (63 x 1,405 x n x 8 B; 3-year v9: 3,124 MiB). Checks:
- seal and no 2024-named source:
  `"$PY" -c "import json,sys;m=json.load(open(sys.argv[1],encoding='utf-8'));s={x['path'] for f in m['fields'] for x in f.get('sources') or [] if isinstance(x,dict)};print(m['seal']);print([p for p in s if '2024' in p] or 'no 2024-named source')" build-equity/train-2020-2023-lo1-fields-v9/manifest.json`
- names in order: `[f['name'] for f in m['fields']]` == F63.
- acceptance "coverage within .02 of the 3-year build" on 2020-2022 member cells (per_year is in every entry):
  `"$PY" -c "import json,sys;L=lambda p:{f['name']:f['coverage']['per_year'] for f in json.load(open(p,encoding='utf-8'))['fields']};o,n=L(sys.argv[1]),L(sys.argv[2]);print('fields',len(n));print([(k,y,o[k][y]['finite_member_frac'],n[k][y]['finite_member_frac']) for k in o for y in ('2020','2021','2022') if abs(o[k][y]['finite_member_frac']-n[k][y]['finite_member_frac'])>.02] or 'all within .02')" build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9/manifest.json build-equity/train-2020-2023-lo1-fields-v9/manifest.json`
  Expected exception: `regsho_threshold_days63` (stage republished, BLOCKER 1; no v7.1 candidate reads it).

### R11. Fields v9 on lo3 -- reconstructed argv (never built; L9 design)

The latest lo3 fields are `recent-fast-train-2020-2022-v2-lo3-fields-v7` (41 fields; built by the v70-lo3 cycle's
direct fields phase, peak 649 MiB, time not recorded; 2,033 MiB). lo3 uses the atx-db v2-pit bridge for the issuer
fields and the fundamentals-stage SIC for grp_* (U2: role and fields must share one SIC table and one bridge).

```bash
"$PY" scripts/run_bounded_research.py --seconds 600 --max-rss-mib 2560 --min-free-mib 512 \
  --output build-equity/train-2020-2023-lo3-fields-v9-run \
  --bind atx-engine/tools/prepare_research_fields.py --bind atx-engine/tools/research_fields_sec.py \
  --bind atx-engine/tools/research_fields_holdings.py --bind build-equity/train-2020-2023-lo3/manifest.json -- \
  "$PY" atx-engine/tools/prepare_research_fields.py --role build-equity/train-2020-2023-lo3 --role-sha256 $LO3 \
  --output build-equity/train-2020-2023-lo3-fields-v9 --fields $F63 \
  --finra $FINRA --tickerhistory $TH --finra-short-volume $SVRAW \
  --identity-bridge $V1/export/identity-bridge-v2-pit --identity-bridge-sha256 $V2PIT \
  --fund-events $FE --fund-events-sha256 $FEV --fund-lag-sessions 1 \
  --sic-events $V1/fundamentals --sic-events-sha256 $SIC \
  --sec-stages $V1 --sec-identity-bridge $V1/export/identity-bridge-v2-pit --sec-identity-bridge-sha256 $V2PIT \
  --earnings-calendar-sha256 $EC --insider-sha256 $INS --sec-filings-sha256 $SECF \
  --thirteenf $V1/thirteenf --thirteenf-sha256 $F13 --ftd $V1/ftd --ftd-sha256 $FTD \
  --regsho-threshold $V1/regsho_threshold --regsho-threshold-sha256 $REGSHO \
  --security-master $V1/security_master --security-master-sha256 $SECM \
  --short-volume-ext $V1/short_volume_ext --short-volume-ext-sha256 $SVX \
  --max-rss-mib 2048 --max-seconds 580
F3=$(sha build-equity/train-2020-2023-lo3-fields-v9/manifest.json)
```

Same checks as R10. Coverage reference: the 41 fields-v7 names against `...-v2-lo3-fields-v7`; the 22 W5a/W5b names
have no 3-year lo3 build (open question). A lo3 role cross-check (`--check-fields`) would need grp fields on the BASE
role built with v2-pit and the stage SIC (`_check_fields` requires the fields to be bound to `--base-role`); the
3-year lo3 root build did not use it, so R8 omits it.

### R12. IC plan-only, exact (brief step 3)

```bash
for R in lo1 lo3; do
  "$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
    --output build-equity/w0-2-plan-$R-run --bind $IC --bind $LIB --bind build-equity/train-2020-2023-$R/manifest.json \
    --bind build-equity/train-2020-2023-$R-fields-v9/manifest.json -- \
    $IC --library $LIB --library-sha256 $LIBS --train build-equity/train-2020-2023-$R/manifest.json \
    --train-sha256 $(sha build-equity/train-2020-2023-$R/manifest.json) --train-fields build-equity/train-2020-2023-$R-fields-v9 \
    --train-fields-sha256 $(sha build-equity/train-2020-2023-$R-fields-v9/manifest.json) \
    --max-memory-mib 2560 --min-names 1000 --workers 4 --plan-only
done
```

Old reference: `mega-v51-plan-run` (breadth-check-v5.md section 5). Expect `required_bytes` = the R9 formula at the
role's n (lo1 and lo3 share axes, so one number), `max_compiled_slots` 8, `resident_capacity` 6, 48 candidates.

### R13. u pass v7.1 on lo1, cold cache, and the overlap reports (brief step 5)

3-year references: `mega-v71-train-u-run1` (warm: 44 hits + 4 evals) 23.0 s / 767 MiB; cold `mega-v70-lo3-train-u-run1`
82.2 s / 1,216 MiB. Expect cold 4-year ~120 s, ~1.6 GB RSS; cache ~3,140 MiB; u dir ~95 MiB. The cache dir is the
one the B0a spec uses, so B0a's u phase is then all hits.

```bash
"$PY" scripts/run_bounded_research.py --seconds 300 --max-rss-mib 2560 --min-free-mib 512 \
  --output build-equity/w0-2-v71-u-lo1-run1 --bind $IC --bind $LIB --bind build-equity/train-2020-2023-lo1/manifest.json \
  --bind build-equity/train-2020-2023-lo1-fields-v9/manifest.json -- \
  $IC --library $LIB --library-sha256 $LIBS --train build-equity/train-2020-2023-lo1/manifest.json --train-sha256 $LO1 \
  --train-fields build-equity/train-2020-2023-lo1-fields-v9 --train-fields-sha256 $F1 \
  --output build-equity/w0-2-v71-u-lo1-1 --max-memory-mib 2560 --min-names 1000 --workers 4 --save-combined \
  --candidate-cache build-equity/mega-candidate-cache-v8-lo1
# overlap (tool from this task; interface per brief; sessions before 2022-09-30 for signal and daily_ic)
"$PY" atx-impl/tools/compare_window_overlap.py --kind field --old build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9 \
  --new build-equity/train-2020-2023-lo1-fields-v9 --out build-equity/w0-2-overlap-field-lo1.json
"$PY" atx-impl/tools/compare_window_overlap.py --kind signal --old build-equity/mega-candidate-cache-v71 \
  --new build-equity/mega-candidate-cache-v8-lo1 --out build-equity/w0-2-overlap-signal-lo1.json
"$PY" atx-impl/tools/compare_window_overlap.py --kind daily_ic --old build-equity/mega-v71-train-u-1 \
  --new build-equity/w0-2-v71-u-lo1-1 --out build-equity/w0-2-overlap-daily-ic-lo1.json
```

Run the field overlap first (seconds, no IC): two fields depend on the role's line set and may differ on 2020-2022
cells by construction, which decides ruling W0-a before any IC is read:
- `me_company` = sum over the issuer's *role lines* (P and J) of shares_out x raw_close (`prepare_research_fields.py:417-418`).
  A line that first enters the union in 2023 but was present earlier as a J line of an existing issuer changes the
  issuer's 2020-2022 values. Ten v7.1 candidates read me_company (value_composite, bm, ep, cfp, fcfp, ebit_ev,
  net_payout, sp, rd_me, q5_eg).
- `sv_ratio126` maps FINRA symbols through the role's own ticker map with an ambiguity drop (`SV_MAP_RULE`, `:459-467`);
  extra role lines can add or resolve collisions. One candidate (sv_flow).
Floating-point summation over a wider column axis is the third, generic source (Review Focus 2).

### R14. Ledger and specs (brief step 6)

Record R1-R13 pins, seconds and peaks and the W0-a outcome in `progress.md`; fill every `<fill>` in the two spec
drafts (Q4) with `sha` of the named file (or set them to null and run `research_cycle.py lock specs/v8/base-lo1.json
--write`); commit with `git add -f` for the sprint dir.

### R15. Later (W0-4 B0c only): delisting-returns role

Only linked-operating-v2/v3 take `--delisting` / `--delisting-returns` (`prepare_recent_research.py:665-670,962-966`).
If B0b (lo3) wins: R8's command plus `--delisting-returns $V1/delisting`, output `train-2020-2023-lo3-dlret`, then an
R11-style fields build on it (no reuse possible: different role manifest; ~200 s, ~4,120 MiB). If B0a (lo1) wins,
there is no lo1 rule with delisting returns in code (open question).

---

## 4. Artifact table (a-d)

Coverage column: "2023" = the input covers 2023-01-01..2023-12-31; "2024+" = it physically contains rows dated or
available on/after 2024-01-01. Old cost = the old receipt (sampled peak tree RSS). Size = on disk now -> 4-year estimate.

| # | artifact | builder (argv in step) | inputs | 2023 / 2024+ | rebuild? | old s / MiB | size MiB |
|---|---|---|---|---|---|---|---|
| a1 | TickerHistory3.parquet (vendor; also options/IV, earn, shares) | vendor snapshot 2026-09-20 | - | yes / yes (to 2026-09) | no (reader filter) | - | 3,450 |
| a2 | recent-projection-v1 -> -v2 | R1 | a1 | yes / yes (end 2025-01-01) | yes, forced | 39.2 / 759 | 425 -> ~360 |
| a3 | recent-fast-train-2020-2022-v1 -> train-2020-2023-base-v1 | R2 | a2 | no / no (ends 2022-12-30) | yes | 12.3 / 320 | 161 -> ~213 |
| a4 | recent-fast-train-2020-2022-v2 -> train-2020-2023-base | R3 | a3 | no / no | yes | 2.45 / 315 (scan 1.7 / 156) | 162 -> ~213 |
| b1 | identity-bridge-r4-v1 -> -r4-v2 | R4 | r4 export (atx-db, manifest ac9bcda7, unchanged) | yes / yes (seal 2025, max_end_incl 2024-12-31) | yes (brief step 1) | 1.6 / 143 (check 4.0 / 778) | 1.7 -> 1.7 |
| b2 | fundamental-events-v2 -> -v3 | R5 (6 runs) | b1 ciks.txt, CF-R ee099c73 (50e018e1), FSDS v2 (2cad6134) | yes / yes (seal 2025, SUB <= 2024q4) | recommended, after the LAST_SUB_QUARTER fix | 93 total / 658 | 56 -> ~55 |
| b3 | earnings_calendar (atx-db) | atx-db session (not in pool-2 receipts) | SEC 8-K 2.02 | yes / yes (vendor range 2012-03-26..2026-09-18) | no (reader filter) | - | 17 |
| b4 | insider (atx-db) | atx-db | Form 4 | yes / yes (files 2015q1..2026q2) | no; reader guard needed (2024q1 file) | - | 213 |
| b5 | sec_filings (atx-db) | atx-db | EDGAR | yes / yes (filing_date 2009-01-01..2026-09-19) | no | - | 589 |
| b6 | thirteenf (atx-db) | atx-db | 13F data sets | yes / yes (source=2013q2..2023q4 + rolling 2024-2026) | no | - | 2,647 |
| b7 | ftd (atx-db) | atx-db | SEC FTD | yes / yes (year=2013..2026) | no | - | 210 |
| b8 | regsho_threshold (atx-db) | atx-db | exchange lists | yes / yes (window 2018-01-02..2026-09-25) | no; PIN CHANGED (fb073c62 vs 68f431f0) | - | 1.8 |
| b9 | security_master (atx-db) | atx-db | FINRA names | yes / yes | no | - | 1.8 |
| b10 | short_volume_ext (atx-db) | atx-db | FINRA CNMS | yes / yes (year=2018..2026) | no | - | 203 |
| b11 | FINRA raw short volume (sv_ratio126) | downloader receipt manifest.csv | - | yes / yes (files to 2026-09-18) | no; only files d-126..d-1 opened | - | 217 |
| b12 | FINRA short interest asof (si_shares, si_dtc) | C:/atx/data/finra_short_interest (asof manifest a2561d75) | FINRA | yes / yes (available 2018-01-10..2026-06-25) | no (reader filter) | - | 510 |
| b13 | fundamentals stage SIC (lo3 role and grp) | atx-db | Company Facts | yes / yes | no | - | 135 |
| b14 | delisting (lo3 role) | atx-db | vendor + SEC | yes / yes (last sessions 2018-01-02..2026-08-31) | no | - | 0.1 |
| b15 | identity-bridge-v2-pit (lo3 role/fields, SEC fields) | atx-db export | - | yes / yes | no | - | 0.8 |
| c1 | role lo1 -> train-2020-2023-lo1 | R7 (+R6) | a4, b1, b2 | no / no | yes | 4.8 / 211 | 161 -> ~213 |
| c2 | role lo3 -> train-2020-2023-lo3 | R8 | a4, b15, b13, b14 | no / no | yes | 2.6 / 320 (U2, no receipt) | 162 -> ~213 |
| d1 | lo1-fields-v9 -> train-2020-2023-lo1-fields-v9 | R10 (reconstructed) | c1, a1, b1-b12, b15 | no / no | yes, all 63 computed | 121 / 571 (with reuse) | 3,124 -> ~4,120 |
| d2 | lo3-fields-v7 (latest) -> train-2020-2023-lo3-fields-v9 | R11 (reconstructed) | c2, a1, b2-b15 | no / no | yes | - / 649 (v7, 41 fields) | 2,033 -> ~4,120 |

Stage manifests re-hashed 2026-09-29: all equal the L9 pins except regsho_threshold.

---

## 5. Answers

### Q1. Inputs built under the old seal that physically hold 2024 rows; cheapest compliant path

Built by pool-2 tools under SEAL 2025-01-01 (`prepare_recent_research.py:70`, `prepare_identity_bridge.py:101`,
`build_fundamental_events.py:68`):

1. **recent-projection-v1** (end_exclusive 2025-01-01, sessions through 2024-12-31). Path: **rebuild (R1)**. It is
   forced: after W0-1 the role builder refuses it:
   `prepare_recent_research.py:439  if not day(m["start"]) <= begin < start < end <= day(m["end_exclusive"]) <= day(SEAL): raise ValueError("role outside sealed reusable projection")`.
   The reader itself would never materialise 2024 rows into a role (`:396-402`: row groups skipped by min/max,
   `keep = (a[0] >= begin) & (a[0] < end)`), but it decodes the boundary row group.
2. **identity-bridge-r4-v1** (seal 2025, `max_end_incl` 2024-12-31, versions available in 2024). Path: **rebuild with
   `--seal 2024-01-01` (R4, 1.6 s)**. The reader filter drops only rows *available* after the seal and keeps
   intervals that extend into 2024:
   `prepare_research_fields.py:1669-1671  sealed = avail >= SEAL_NS ... keep = known & ~excluded & ~sealed & on`
   (same test in `prepare_recent_research.py:600`). Lookups only use role sessions, so values are identical either way;
   the rebuild matters because `ciks.txt` scopes fundamental events (item 3).
   `prepare_identity_bridge.py:585,593-594  --seal (default SEAL) ... if a.seal > SEAL: raise SystemExit(...)`.
3. **fundamental-events-v2** (seal 2025; SUB quarters to 2024q4; filings through 2024). Path: **rebuild (R5, ~93 s)
   after W0-1 also sets `LAST_SUB_QUARTER`** (`build_fundamental_events.py:73 LAST_SUB_QUARTER = "2024q4"`,
   used in `sub_quarters`, `:961-977`: "the seal: no SUB quarter after 2024q4 is opened"). The builder's own reads are pushed down:
   `:1050  filters = [("taxonomy", "=", "us-gaap"), ("filed_date", "<", SEAL), ...]`. Reader-side alternative (zero
   cost): `prepare_research_fields.py:1731-1734  sealed = clock >= SEAL_NS ... keep = ~sealed & linked`, after
   `read_listed` has read and parsed the whole file (`:1590-1602`). For lo1 both paths give the same values; for lo3
   (v2-pit links) the v3 scope drops CIKs whose only r4 evidence is dated 2024, so v2 would give such CIKs fundamentals
   through a scope that used 2024 information (decision in the open questions).
4. Not inputs, but hold hidden data: `recent-fast-validation-2023-2024-v1` and its fields v1-v6 (owner decision, Q3).

Unsealed upstream (atx-db stages, vendor file, FINRA files; all cover 2023 and hold 2024-2026 rows). Path: **reader-
side filter; no rebuild is possible in this sprint** (Ruling E-3). Decisive lines:

| input | filter line | loads sealed rows? |
|---|---|---|
| TickerHistory3 (projection) | `prepare_recent_research.py:277-279` batch mask `date >= first & date < last` before any QA | decodes, drops per batch |
| TickerHistory3 (fields) | `prepare_research_fields.py:1051-1052` count `d >= day_of(SEAL)`; select `(d >= first - pre) & (d <= last)` | decodes, never selects |
| FINRA SI CSV | `:890-892 sealed = days >= day_of(SEAL); ids, days, values = ids[~sealed], ...` | whole CSV read and hashed |
| FINRA raw SV | `:2338-2341` opens only the file of each needed session d-126..d-1 | no 2024 file opened |
| earnings_calendar | `research_fields_sec.py:484 sealed = avail >= h.SEAL_NS` | whole file |
| insider | `research_fields_sec.py:587 sealed = avail >= h.SEAL_NS`; file choice `:574-577 if (qday - epoch).days > end_day + INS_SKIP_MARGIN_DAYS: ... continue` with `INS_SKIP_MARGIN_DAYS = 31` (`:63`) | **opens `transactions/year=2024/2024q1.parquet`** for a role ending 2023-12-29: add `or qday >= seal` |
| sec_filings (8-K) | `research_fields_sec.py:713 sealed = avail >= h.SEAL_NS` | whole file |
| thirteenf | `research_fields_holdings.py:566-567 sealed = vis >= SEAL_NS; vis[sealed] = NEVER`; quarters `needed_quarters` P < last role session (`:251-259`), parts by source period | whole filings / agg / cusip tables; parts only to source=2023q4 |
| ftd | `research_fields_holdings.py:753-755 sealed = av >= SEAL_NS ... d[~sealed]`; files `:738-740` years to the role's last year | year=2023 file (late-Dec rows sealed) |
| regsho_threshold | `:830 ok_list = ... & (l_av < SEAL_NS)`, `:859 ... & (av < SEAL_NS)`; files `:845-846` role years | whole lists file |
| security_master | `:877 keep = fon & (f_sid > 0) & (f_av < SEAL_NS)` | whole file |
| short_volume_ext | `:958 keep = ... & (av < SEAL_NS)`; files `:940` role years | no 2024 file |
| fundamentals SIC stage | `prepare_research_fields.py:1830 used = linked & (clock < SEAL_NS) & valid` | whole file |
| delisting | `prepare_recent_research.py:847 sealed = inside & (avail >= prf.SEAL_NS)` | whole file |
| v2-pit bridge | `prepare_research_fields.py:1669-1671`, `prepare_recent_research.py:600` | whole file |

"Never loads sealed rows" is not reachable for any whole-file-pinned stage: `read_listed` and `Stage.blob` hash every
byte before parsing (`prepare_research_fields.py:1590-1592`, `research_fields_holdings.py:444-457`). The only strict
alternative is a one-time sealed copy per stage (like the projection), which no brief covers. Counters: every sealed
row is still counted into the manifests under keys literally named `rows_available_on_or_after_2025_dropped`,
`rows_on_or_after_2025_skipped`, `rows_sealed` (e.g. `prepare_research_fields.py:936,1044,1677,1742`); after W0-1 they
count 2024+ rows under a 2025 label (open question).

### Q2. IC memory admission for the 4-year role

`strategy_ic_runner.cpp:631-653` (with `ic_composition_working_bytes`, `strategy_ic_composition.cpp:39-56`):

```
required = 32 MiB
         + cells*(72 + 8*max_slots)                         // role26, guard4, masks2, signal8, VM scratch32, slot payload
         + [4096 + 9*cells + 40*d + 32*n + 512*K (+16*cells*themes)]   // composition
         + 512*d + 512*n
         + cells*(8*capacity + 1)                           // only if the library has extra fields
         + sum_{h in 5,21,63} (score_dates-h-1)*(16*n + 32)  // IC labels/ranks
         + workers*(8 MiB + 64 KiB) + workers*n*1104 + workers*d*64   // workers > 1
cells = d*n, score_dates = d - score_begin
```

Assumptions: d = 1,405 and score_begin = 399 (both measured from the projection manifest: 1,405 sessions in
[2018-06-01, 2024-01-01), 1,006 score sessions; start date kept); K = 48 candidates (v7.1 library); max_slots = 8 and
resident capacity = 6 (L8 report; the formula with these values reproduces the recorded 3-year admission 1,553,063,994 B
exactly; "slots 7" in the request gives 1,501,070,514 B for the 3-year role, which does not match); the "field count"
enters only as the resident capacity (the most extras any one candidate reads), not the 40 declared extras or the 63
manifest fields; workers 4; themes 0 (the 3-year weighted pass with ew-theme-v1 weights was admitted under 1,536 MiB,
which with 10 themes would have needed 2,472.8 MiB, so ew-theme-v1 carries no redistribution block); lo1 and lo3 share
axes, so one number serves both. n is unknown until R2: the 3-year union is 5,627 and 2023 adds the names first
selected in 2023 (estimate +350..+550, central 6,100).

| n | slots 8, cap 6 (v7.1 as recorded) | slots 7, cap 6 (as requested) |
|---|---|---|
| 5,627 (no new names; lower bound) | 1,893,723,494 B = 1,806.0 MiB | 1,830,476,014 B = 1,745.7 MiB |
| 5,900 | 1,982,269,952 B = 1,890.4 MiB | 1,915,953,952 B = 1,827.2 MiB |
| **6,100 (central)** | **2,047,139,152 B = 1,952.3 MiB** | **1,978,575,152 B = 1,886.9 MiB** |
| 6,300 | 2,112,008,352 B = 2,014.2 MiB | 2,041,196,352 B = 1,946.6 MiB |
| 8,000 (role builder `--max-union` cap) | 2,663,396,552 B = 2,540.0 MiB | 2,573,476,552 B = 2,454.3 MiB |

Slope: 324,346 B per instrument at d = 1,405. Every admissible union fits the OD-2 cap (2,560 MiB = 2,684,354,560 B);
none fits 1,536 MiB. Measured RSS was 82% of admission in the cold 3-year lo3 u pass (1,216 of ~1,481 MiB), so expect
~1.6 GB RSS at n = 6,100. Breakdown at n = 6,100 (slots 8): VM/role 1,111.6 MiB, fields 400.5, labels 272.4,
composition 73.8, workers 58.3, fixed 32.0, per-axis 3.7.

### Q3. Disk

Now (hardlink-aware; "excl" = bytes whose every link is inside that directory). C: free 73,309,640 KiB (69.9 GiB);
build-equity holds 76.8 GB unique (87.3 GB apparent).

| artifact | apparent MiB | excl MiB |
|---|---|---|
| role v2 / lo1 / lo3 (3-year) | 161.6 / 161.3 / 161.6 | same |
| lo1-fields-v9 | 3,124.3 | 1,140.9 (40 payloads hardlinked from v8 <- v7) |
| lo1-fields-v8 / -v7 | 2,727.5 / 2,033.2 | 744.1 / 49.8 |
| lo3-fields-v7 | 2,033.2 | 2,033.2 |
| mega-candidate-cache-v71 / -v70 / -v70-lo3 | 2,383.5 / 2,184.9 / 2,184.9 | 198.6 / 0 / 2,184.9 |
| u pass dirs (mega-v71-train-u-1 etc.) | ~70-72 each | same |
| recent-projection-v1 | 425.2 | 425.2 |

4-year estimates (n = 6,100): projection ~360 MiB; base-v1, base, lo1, lo3 ~213 MiB each (declared output =
1,405 x n x 26 B); r4-v2 1.7 MiB; fundamental-events-v3 ~55 MiB; optional base grp fields ~196 MiB; fields v9 ~4,120 MiB
per role (63 x 65.4 MiB, all exclusive, no reuse); candidate cache ~3,140 MiB per role (48 x 65.4 MiB); u/w dirs ~95 MiB
each; fit work ~112 MiB. W0-2 + B0a/B0b: ~16 GiB. B0c adds ~7.4 GiB (dlret role, fields, cache). W0-4 step 4 re-runs
reuse the v8 caches for v7.0 members (subset of v7.1); v6.1 adds up to ~2.5 GiB per role.

Delete candidates (nothing deleted; freed = hardlink-aware for the whole set):

| set | directories | apparent MiB | freed MiB | note |
|---|---|---|---|---|
| A1 caches v1-v6 | mega-candidate-cache, -v6, -v6u | 24,202 | **22,315** | v6u is all hardlinks into v6 |
| A2 caches v6.1 | mega-candidate-cache-v61, -v61-r7 | 3,873 | 3,873 | v61.json points at -v61: a re-run recomputes (~75 s) |
| A3 v7 lane caches | -v7rel, -v7l1, -v7w2 | 5,810 | 1,986 | rest shared with v70/v71 |
| B1 base-role fields v1-v6 | ...-v1-fields-v1, ...-v2-fields-v2..v6 | 5,703 | 5,703 | v2-fields-v6 is the 3-year lo1 role's `--check-fields` provenance |
| B2 lo1 fields v6, v7-r7b, v7-r7c | 3 dirs | 6,001 | 6,001 | L2 reuse identity runs |
| B3 lo1-fields-v8 | 1 dir | 2,728 | 744 | superseded by v9 (v9 records it as reuse source) |
| C fit work (root, v5, v51, v6l) | 4 dirs | 595 | 595 | |
| D hidden-window (2023-2024) role + fields v1-v6 | 7 dirs | 4,113 | 4,113 | owner decision: record of 2 validation trials; holds 2024 data |
| E dev/smoke + v1 role | recent-dev-check-v1, recent-dev-smoke-v1, recent-fast-train-2020-2022-v1 | 260 | 260 | v1 role = repair provenance |
| F role lo2 | recent-fast-train-2020-2022-v2-lo2 | 162 | 162 | no spec uses it |

A1+A2+A3 frees 30,062 MiB; B1+B2+B3 frees 12,448 MiB. Keep: `trials.jsonl`, every `*-run*` receipt dir, accepted NAV
cells (v6.1, v7.0, v7.0-lo3, v7.1) and their u/w/weights/fit dirs, roles v2/lo1/lo3, fields lo1 v6b (v61.json baseline),
v7 (v70/v71 baseline and `--cache-legacy-fields`), v9 (v71 as built), lo3 v7, caches v70/v70-lo3/v71.

### Q4. Spec drafts (text only; not written as files)

Differences from v71.json / v70-lo3.json: new role and as-built fields v9 (built by R10/R11, pinned, never built by the
cycle: works with research_cycle.py as it is, before or after A-3); no `ref`, no `compare`, no `reference_*` identity
inputs, no `static_check`/`baseline_library` (library unchanged; new window); no `--cache-legacy-fields` (no legacy
entries on the new role); IC `--max-memory-mib 2560`; runner 300 s / 2,560 MiB (research_cycle has one runner block, so
the cap applies to every bounded phase; measured non-IC phases stay far below); `dsr_n` "ledger+1" (A-3 contract;
before A-3 set the integer: 38 for B0a, 39 for B0b); monitor without the 3-year holdings/bias inputs. base-lo3 keeps
one paired reference, B0a's cell, because W0-4 step 1 accepts B0b "on paired S2 net dSR > 0 against B0a"; drop
`inputs.reference_cell` if root reads "no reference cell comparison" strictly and pass `--reference` by hand.
`"<fill>"` fails `validate_spec` / `verify_inputs` until replaced (or set to null and `lock --write`).

`scripts/specs/v8/base-lo1.json` (cell B0a):

```json
{
  "schema": "atx.research-cycle-spec/v1",
  "name": "v8-b0a-lo1",
  "description": "Cell B0a (v8-prereg.md, W0-4): the accepted v7.1 book on the new TRAIN window 2020-2023 (research-window-v2, seal 2024-01-01). Library v7.1 + recipe held fixed; admission v4-prior-v1 and ew-theme-v1 refit on 2020-2023; construction aim-partial-v5 theta .05 dust .1 fixed, delta orders, exit .05, locate-in-aim, liquidity cache, price-risk-v1, L 1.247 fixed; cost S2. Role train-2020-2023-lo1 = linked-operating-v1 on the 4-year base (start 2018-06-01, factor-break-v1 repair at 2021-01-04) with identity-bridge-r4-v2 (seal 2024-01-01) and fundamental-events-v3 SIC. Fields v9 AS BUILT (63 fields, scripts/specs/v71.json fields.list; built by the W0-2 runbook R10, never by the cycle). New window: no ref phase, no compare, no reference cell (the 2020-2022 overlap is checked outside the cycle, W0-2 step 5, ruling W0-a). Runner 300 s / 2,560 MiB (OD-2) for every bounded phase. summ over the ledger grid, dsr_n = ledger lines + 1 (N 38 when B0a is first).",
  "python": "C:/Program Files/Python312/python.exe",
  "env_path_prepend": [
    "C:/atx-cache/vcpkg_installed/x64-windows/debug/bin",
    "C:/atx-cache/vcpkg_installed/x64-windows/bin"
  ],
  "runner": {
    "script": "scripts/run_bounded_research.py",
    "seconds": 300,
    "max_rss_mib": 2560,
    "min_free_mib": 512
  },
  "exes": {
    "ic": "build-equity/bin/atx-equity-strategy-ic.exe",
    "nav": "build-equity/bin/atx-equity-strategy-targets.exe"
  },
  "inputs": {
    "library": {
      "path": "atx-impl/strategies/fund_industry_ic_v71.json",
      "sha256": "787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259"
    },
    "recipe": {
      "path": "atx-impl/strategies/fund_industry_ic_v71.recipe.json",
      "sha256": "7f8a264312d47487d2a9f7e0e92ecd76defb22ce2822ec319805f9ad0dffb271"
    },
    "role": {
      "dir": "build-equity/train-2020-2023-lo1",
      "path": "build-equity/train-2020-2023-lo1/manifest.json",
      "universe": "linked-operating-v1",
      "sha256": "<fill>"
    },
    "identity_bridge": {
      "dir": "build-equity/identity-bridge-r4-v2",
      "path": "build-equity/identity-bridge-r4-v2/manifest.json",
      "sha256": "<fill>"
    },
    "fund_events": {
      "dir": "build-equity/fundamental-events-v3",
      "path": "build-equity/fundamental-events-v3/manifest.json",
      "sha256": "<fill>"
    }
  },
  "fields": {
    "output": "build-equity/train-2020-2023-lo1-fields-v9",
    "manifest_sha256": "<fill>",
    "list": [
      "si_shares", "si_dtc", "iv_atm_21d", "iv_atm_63d", "iv_atm_126d", "earn_recent", "shares_out", "mkt_ret",
      "be", "at", "at_lag4", "lt", "che", "debt", "sale_ttm", "gp_ttm", "oi_ttm", "ni_ttm", "ni_q", "ni_q_lag4",
      "be_lag1q", "be_lag1q_lag4", "cfo_ttm", "capx_ttm", "xrd_ttm", "dvc_ttm", "prstkc_ttm", "sstk_ttm", "txt_q",
      "txt_q_lag4", "shrs_q", "shrs_q_lag4", "noa", "noa_lag4", "sue", "fscore", "me_company", "grp_sic2", "grp_ff12",
      "grp_ff49", "sv_ratio126", "ea_days_to_expected", "ea_days_since", "ea_window_pre5", "ea_window_post3",
      "ea_delay_days", "ea_time_of_day", "ins_net_buy_ratio", "ins_n_buyers", "ins_n_sellers", "ins_opportunistic_net",
      "ins_cluster_buy", "k8_count_63", "k8_item_material_21", "k8_days_since_any", "inst_own_share",
      "inst_breadth_chg", "inst_own_chg_q", "inst_best_ideas", "inst_n_holders", "ftd_shares_ratio21",
      "regsho_threshold_days63", "sv_offexchange_share126"
    ]
  },
  "ic": {
    "u_output": "build-equity/mega-v8-b0a-train-u",
    "w_output": "build-equity/mega-v8-b0aw-train-ew",
    "cache": "build-equity/mega-candidate-cache-v8-lo1",
    "flags": ["--max-memory-mib", "2560", "--min-names", "1000", "--workers", "4", "--save-combined"]
  },
  "fit": {
    "script": "atx-impl/tools/fit_composition_weights.py",
    "output": "build-equity/mega-weights-v8-b0a-ew",
    "work_dir": "build-equity/mega-fit-work-v8-b0a",
    "flags": ["--orientation", "prior", "--screen", "v4-prior-v1", "--composition", "ew-theme-v1"],
    "max_seconds": 150,
    "max_passes": 3
  },
  "card": {
    "script": "atx-impl/tools/alpha_report_card.py",
    "output": "build-equity/mega-cards-v8-b0a",
    "flags": ["--workers", "4"]
  },
  "gate": {
    "name": "b0a-readout",
    "admitted": [],
    "report": ["ins_opp", "inst_best_ideas", "ftd_fail", "ea_overdue", "mom_12_1", "si_ratio", "dtc", "sv_flow",
               "iv_rv_spread", "ear", "sue", "droe"]
  },
  "nav": {
    "output": "build-equity/mega-nav-v8-b0a-lo1-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247",
    "rule": "aim-partial-v5",
    "leverage": "1.247",
    "flags": ["--cadence", "1", "--trade-fraction", ".05", "--dust-multiple", ".1", "--aim-leverage", "{leverage}",
              "--daily-turnover-mean-max", ".20", "--daily-turnover-p95-max", ".30", "--neutralize", "price-risk-v1",
              "--max-bytes", "1073741824", "--order-basis", "delta", "--exit-rate", ".05", "--locate-in-aim",
              "--liquidity-cache"]
  },
  "monitor": {
    "script": "atx-impl/tools/book_monitor.py",
    "output": "build-equity/mega-monitor-v8-b0a",
    "flags": []
  },
  "summ": {
    "script": ".superpowers/sdd/mega-alpha-20260926/studies/nav_summ.py",
    "dsr_n": "ledger+1",
    "cells_from_ledger": true,
    "extra": ["--effective-n", "dirs", "--psr", "--pbo"],
    "ledger": "build-equity/trials.jsonl",
    "ledger_kind": "construction"
  }
}
```

`scripts/specs/v8/base-lo3.json` (cell B0b):

```json
{
  "schema": "atx.research-cycle-spec/v1",
  "name": "v8-b0b-lo3",
  "description": "Cell B0b (v8-prereg.md, W0-4): V7-F restated on the new TRAIN window 2020-2023 (seal 2024-01-01): the B0a book (library v7.1, ew-theme-v1 refit, aim-partial-v5, L 1.247 fixed, same flags) on role train-2020-2023-lo3 = linked-operating-v3 on the same 4-year base (atx-db identity-bridge-v2-pit, v2 class test, fundamentals-stage SIC, delisting attributes only, --delisting-returns off) with fields v9 AS BUILT on lo3 (63 fields; issuer fields on v2-pit, grp_* on the stage SIC; W0-2 runbook R11). Accepted on paired S2 net dSR > 0 against B0a (inputs.reference_cell, the only reference; no ref phase, no compare) and mechanics. Runner 300 s / 2,560 MiB (OD-2). dsr_n = ledger lines + 1 (N 39 after B0a).",
  "python": "C:/Program Files/Python312/python.exe",
  "env_path_prepend": [
    "C:/atx-cache/vcpkg_installed/x64-windows/debug/bin",
    "C:/atx-cache/vcpkg_installed/x64-windows/bin"
  ],
  "runner": {
    "script": "scripts/run_bounded_research.py",
    "seconds": 300,
    "max_rss_mib": 2560,
    "min_free_mib": 512
  },
  "exes": {
    "ic": "build-equity/bin/atx-equity-strategy-ic.exe",
    "nav": "build-equity/bin/atx-equity-strategy-targets.exe"
  },
  "inputs": {
    "library": {
      "path": "atx-impl/strategies/fund_industry_ic_v71.json",
      "sha256": "787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259"
    },
    "recipe": {
      "path": "atx-impl/strategies/fund_industry_ic_v71.recipe.json",
      "sha256": "7f8a264312d47487d2a9f7e0e92ecd76defb22ce2822ec319805f9ad0dffb271"
    },
    "role": {
      "dir": "build-equity/train-2020-2023-lo3",
      "path": "build-equity/train-2020-2023-lo3/manifest.json",
      "universe": "linked-operating-v3",
      "sha256": "<fill>"
    },
    "identity_bridge": {
      "dir": "C:/atx/atx-db/data/alpha_panel/v1/export/identity-bridge-v2-pit",
      "path": "C:/atx/atx-db/data/alpha_panel/v1/export/identity-bridge-v2-pit/manifest.json",
      "sha256": "09aac28f757fa959b0ed4cd9296b2267940e70af98e0d2c67cc45b1df4f7fa01"
    },
    "fund_events": {
      "dir": "build-equity/fundamental-events-v3",
      "path": "build-equity/fundamental-events-v3/manifest.json",
      "sha256": "<fill>"
    },
    "sic_events": {
      "dir": "C:/atx/atx-db/data/alpha_panel/v1/fundamentals",
      "path": "C:/atx/atx-db/data/alpha_panel/v1/fundamentals/manifest.json",
      "sha256": "9f9b2f85f6bcd5c7f3a55aee097893094a5cb85ab2b4edbfb582297dab06816b"
    },
    "reference_cell": {
      "dir": "build-equity/mega-nav-v8-b0a-lo1-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247",
      "path": "build-equity/mega-nav-v8-b0a-lo1-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247/summary.json",
      "sha256": "<fill>"
    }
  },
  "fields": {
    "output": "build-equity/train-2020-2023-lo3-fields-v9",
    "manifest_sha256": "<fill>",
    "list": [
      "si_shares", "si_dtc", "iv_atm_21d", "iv_atm_63d", "iv_atm_126d", "earn_recent", "shares_out", "mkt_ret",
      "be", "at", "at_lag4", "lt", "che", "debt", "sale_ttm", "gp_ttm", "oi_ttm", "ni_ttm", "ni_q", "ni_q_lag4",
      "be_lag1q", "be_lag1q_lag4", "cfo_ttm", "capx_ttm", "xrd_ttm", "dvc_ttm", "prstkc_ttm", "sstk_ttm", "txt_q",
      "txt_q_lag4", "shrs_q", "shrs_q_lag4", "noa", "noa_lag4", "sue", "fscore", "me_company", "grp_sic2", "grp_ff12",
      "grp_ff49", "sv_ratio126", "ea_days_to_expected", "ea_days_since", "ea_window_pre5", "ea_window_post3",
      "ea_delay_days", "ea_time_of_day", "ins_net_buy_ratio", "ins_n_buyers", "ins_n_sellers", "ins_opportunistic_net",
      "ins_cluster_buy", "k8_count_63", "k8_item_material_21", "k8_days_since_any", "inst_own_share",
      "inst_breadth_chg", "inst_own_chg_q", "inst_best_ideas", "inst_n_holders", "ftd_shares_ratio21",
      "regsho_threshold_days63", "sv_offexchange_share126"
    ]
  },
  "ic": {
    "u_output": "build-equity/mega-v8-b0b-train-u",
    "w_output": "build-equity/mega-v8-b0bw-train-ew",
    "cache": "build-equity/mega-candidate-cache-v8-lo3",
    "flags": ["--max-memory-mib", "2560", "--min-names", "1000", "--workers", "4", "--save-combined"]
  },
  "fit": {
    "script": "atx-impl/tools/fit_composition_weights.py",
    "output": "build-equity/mega-weights-v8-b0b-ew",
    "work_dir": "build-equity/mega-fit-work-v8-b0b",
    "flags": ["--orientation", "prior", "--screen", "v4-prior-v1", "--composition", "ew-theme-v1"],
    "max_seconds": 150,
    "max_passes": 3
  },
  "card": {
    "script": "atx-impl/tools/alpha_report_card.py",
    "output": "build-equity/mega-cards-v8-b0b",
    "flags": ["--workers", "4"]
  },
  "gate": {
    "name": "b0b-readout",
    "admitted": [],
    "report": ["ins_opp", "inst_best_ideas", "ftd_fail", "ea_overdue", "mom_12_1", "si_ratio", "dtc", "sv_flow",
               "iv_rv_spread", "ear", "sue", "droe"]
  },
  "nav": {
    "output": "build-equity/mega-nav-v8-b0b-lo3-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247",
    "rule": "aim-partial-v5",
    "leverage": "1.247",
    "flags": ["--cadence", "1", "--trade-fraction", ".05", "--dust-multiple", ".1", "--aim-leverage", "{leverage}",
              "--daily-turnover-mean-max", ".20", "--daily-turnover-p95-max", ".30", "--neutralize", "price-risk-v1",
              "--max-bytes", "1073741824", "--order-basis", "delta", "--exit-rate", ".05", "--locate-in-aim",
              "--liquidity-cache"]
  },
  "monitor": {
    "script": "atx-impl/tools/book_monitor.py",
    "output": "build-equity/mega-monitor-v8-b0b",
    "flags": []
  },
  "summ": {
    "script": ".superpowers/sdd/mega-alpha-20260926/studies/nav_summ.py",
    "dsr_n": "ledger+1",
    "cells_from_ledger": true,
    "extra": ["--effective-n", "dirs", "--psr", "--pbo"],
    "ledger": "build-equity/trials.jsonl",
    "ledger_kind": "construction"
  }
}
```

Expected cycle cost per cell (3-year measurements x 1.32): u ~30 s warm after R13 (B0a) or ~120 s / ~1.6 GB cold
(B0b), fit ~29 s / ~420 MiB, card ~26 s / ~1.3 GB, w ~20 s / ~670 MiB, nav ~35 s / ~460 MiB. `nav_summ.py` moves if
V-1 merges first (update `summ.script`).
