# Task W5b report: 13F / FTD / Reg SHO / short-volume fields and the linked-operating-v2 role
Branch `feat/platform-v7-w5b-holdfields-20260928` (pool-9) HEAD `b9d05e55`, base `9da38ad8`, 6 commits. Nothing in C++ was built, no IC/NAV was run, no returns were read, and I printed nothing for 2023+.
**Code.** New module `atx-engine/tools/research_fields_holdings.py`: `HOLD_FIELDS` :134, `build_13f` :544, `build_ftd` :736, `build_regsho` :824, `build_svx` :932, `register` :1162. Hook in `prepare_research_fields.py` :2675-2679 (5 lines, including the W5a placeholder; W5a says to delete that placeholder at merge). `register(globals())` wraps run/main; stage kwargs are popped and `**kw` (incl. W5a `module_options`) passes through. Part B is in `prepare_recent_research.py`: rule text :100, `v2_class_ok` :555, `restrict_role` :617, `_delisting` :778.
**Tests.** 16 in `test_research_fields_holdings.py` and 5 in `test_linked_operating_v2.py`; with the existing suites, 86/86 pass. Planted mutations caught 9/9 (strict `<`, price screen, unit factor, 45-day deadline, staleness, forward fill, FTD window, Reg SHO minimum lists, svx visibility). Byte identity: every builder field and entry is unchanged with the holdings fields added. The v1 role matches a golden normalised manifest + member.u8 taken from the base commit.
**PIT rule (all 8 fields).** A value is used at session t only if `available_at < date(t-1) 22:00 UTC`. Each field records its stage manifest SHA and formula id; staleness comes from the stage manifests, which are refused if they drift.
| field | formula id | clock / window | cov 2020 / 2021 / 2022 |
|---|---|---|---|
| inst_own_share | 13f-asof45-io-share-qe-v1 | agg inst_shares / shares_out at the quarter-end session; above 2 is NaN | .985 / .993 / .996 |
| inst_breadth_chg | 13f-asof45-breadth-chs-v1 | CHS, over filers present in both quarters | .996 / .994 / .998 |
| inst_own_chg_q | 13f-asof45-io-chg-q-v1 | IO(P) - IO(P-1) | .971 / .974 / .990 |
| inst_best_ideas | 13f-asof45-best-ideas-cps-v1 | sum over holders of max(0, w - mw); mw = 13F-universe value weight | .998 / .997 / .999 |
| inst_n_holders | 13f-asof45-n-holders-v1 | stage agg_asof45 | .998 / .997 / .999 |
| ftd_shares_ratio21 | sec-ftd-sum21-over-shares-v1 | 21 settlement dates to the latest visible prefix date; 60-day staleness | .935 / .999 / 1.000 |
| regsho_threshold_days63 | regsho-threshold-days63-listing-market-v1 | 63 sessions to t-2; needs 60 of 63 lists of the line's own market | .430 / .444 / .424 |
| sv_offexchange_share126 | finra-offexchange-short-over-consolidated126-v1 | 126 sessions to t-2; FINRA short / vendor volume, at least 63 sessions | 1.000 / 1.000 / 1.000 |
**How the 13F clock works.** A quarter is visible only at V(P) = the latest clock of any filing made by the 45-day deadline (filed + 46 h; the same time for every security). Effective filings are the latest HR or RESTATEMENT plus later NEW HOLDINGS; post-deadline and untyped HR/A filings are dropped (counted). A quarter without a row is bridged from the prior quarter until 150 days. My holdings `n_holders` equals the stage aggregate on 91.2% of role cells: the stage's price-outlier screen differs from my declared 10x screen, which I use only for best-ideas and breadth.
**Part B (`linked-operating-v2`).** Same tests as v1, but with atx-db `identity-bridge-v2-pit` (strict + name tiers). Class test: `common` or a share-class letter passes; U / W / R (units, warrants, rights) fail. Delisting terminations are stored in `universe.delisting.events` (delist_code = cause, delist_return = dlret, null where unknown). `--delisting-returns` is off by default. When on, the termination session gets close x (1 + r), present = 1, volume = 0, member = 0.
Base `recent-fast-train-2020-2022-v2`: kept member cells lo1 1,940,364 -> v2 1,967,838. In the score window (2020-2022) the drop share goes .403 -> .396, and kept cells by year are:
- 2020: 447,841 -> 453,560
- 2021: 453,395 -> 461,168
- 2022: 442,568 -> 445,928
Dropped by reason, score window, lo1 -> v2: unlinked 899,105 -> 820,393; no_visible_sic 34 -> 53,691; secondary 3,068 -> 7,901; non_operating_sic 5,343 -> 8,711; class 0 -> 2.
Delisting: 738 terminations in the role (522 mna, 72 non_common, 58 performance, 86 unknown). With the flag on, 651 returns apply; 86 have no return and 1 falls on the last session.
**Root commands** (from the merged tree; V1 = `C:/atx/atx-db/data/alpha_panel/v1`):
`python atx-engine/tools/prepare_research_fields.py --role build-equity/recent-fast-train-2020-2022-v2-lo1 --role-sha256 3e79978a…eb809 --output build-equity/recent-fast-train-2020-2022-v2-lo1-fields-w5b --fields si_shares,shares_out,inst_own_share,inst_breadth_chg,inst_own_chg_q,inst_best_ideas,inst_n_holders,ftd_shares_ratio21,regsho_threshold_days63,sv_offexchange_share126 --thirteenf $V1/thirteenf --thirteenf-sha256 8974170f… --ftd $V1/ftd --ftd-sha256 a76d69be… --regsho-threshold $V1/regsho_threshold --regsho-threshold-sha256 68f431f0… --security-master $V1/security_master --security-master-sha256 3afe0660… --short-volume-ext $V1/short_volume_ext --short-volume-ext-sha256 7007a13c… --max-rss-mib 700 --max-seconds 1800`
`python atx-engine/tools/prepare_recent_research.py role --universe linked-operating-v2 --out build-equity/recent-fast-train-2020-2022-v2-lo2 --base-role build-equity/recent-fast-train-2020-2022-v2 --base-role-sha256 210fff96… --identity-bridge $V1/export/identity-bridge-v2-pit --identity-bridge-sha256 09aac28f… --sic-events build-equity/fundamental-events-v2 --sic-events-sha256 74ed9a50… --delisting $V1/delisting --delisting-sha256 1b1166b6…` (add `--delisting-returns $V1/delisting` for the return variant)
**Measured cost.**
- Fields, no reuse: 76 s, peak RSS 536 MiB (holdings stage 518 MiB).
- Fields with `--reuse` of lo1-fields-v7: 69 s, 581 MiB.
- Role: about 2 s.
- Outputs are in pool-9 `build-equity/*-w5b-noreuse` and `*-lo2[-dlret]`. The noreuse fields manifest is `3bae0db5…`.
**Concerns.**
1. Reg SHO: the NYSE lists have not landed, so NYSE-listed lines are NaN, never a false 0. A rebuild after the stage lands them fills these cells with the same formula.
2. FTD 2020: the Oct-Nov 2020 files carry a Last-Modified of 2020-12-19, so FTD is stale (NaN) from 2020-11-30 to 2020-12-21.
3. v2 gains little (+1.25%), mostly because of no_visible_sic: the pool-2 SIC events miss the newly linked CIKs. Rerun with the atx-db SIC events (outside my read scope).
4. 13F limits: filer type is not classified, and late confidential amendments are excluded. FINRA total / vendor volume is 0.34, which is consistent with vendor volume being consolidated.
5. Field count: 41 builder + 14 W5a + 8 W5b = 63, against the runner limit of 64.
6. Disclosure: while learning schemas I saw the atx-db stage receipts, including coverage counts for 2023+. These are data coverage only, not returns.
