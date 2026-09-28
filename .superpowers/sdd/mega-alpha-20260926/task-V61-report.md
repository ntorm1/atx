# Task V6.1 report: FINRA long-horizon shorting flow (sv_ratio126 / sv_flow)

**Status: DONE_WITH_CONCERNS.** The concerns are listed in section 8. None of them blocks the run.

**Branch:** `feat/mega-alpha-v61-svflow-20260928` in `C:/atx-wt/pool-10`, based on 17a12949. It has three commits:

| SHA | What |
|---|---|
| c026d6a7 | `feat(tools)`: the `sv_ratio126` field in `prepare_research_fields.py`, plus tests |
| 97a34a40 | `feat(strategies)`: library v6.1 (generator, JSON, recipe), `check_fund_ic_v6.py` v6.1 mode, tests |
| 0855bdad | `feat(studies)`: `v61_train.sh` (committed with `git add -f`) |

**Library v6.1 hashes:**
- `atx-impl/strategies/fund_industry_ic_v61.json`: sha256 **db35c2769f6c13a8d9d5b2897a9e6968855bffbe6214ff96a12f6869cb59f9b5** (30,862 bytes).
- `fund_industry_ic_v61.recipe.json`: sha256 9bf278a6f3dfa78edd506ab3e499b2e8dd1ce34899752fe81df573e65550f547 (117,690 bytes).
- Parent: v6 at 5ee66d13 (38 entries, byte-identical, same order).

**Tests** (run with `"C:/Program Files/Python312/python.exe" -m pytest`): 67 passed.
- 16 are new: 11 in `test_prepare_research_fields_sv.py` and 5 in `test_generate_fund_ic_v61.py`.
- 51 are existing regressions: 36 in `test_prepare_research_fields.py` and 15 in `test_generate_fund_ic_v6.py`.

**Owner instruction received mid-task:** memory is no longer a constraint, and tests should stay minimal.
- The streaming implementation was already finished, tested and fast (41 s on lo1), so I kept it rather than rewrite it.
- The byte-identity test was already written and passing, so it stays.
- `v61_train.sh` has no DRY mode.
- The fields phase uses `--max-rss-mib 1536` instead of 700 (see section 5).

## 1. Changes (file:line)

### Builder: `atx-engine/tools/prepare_research_fields.py`

**Registry and constants** (:410-479):
- `SV_FORMULA_ID = "finra-cnms-ratio126-lag1-v1"` is at :419.
- Window 126, minimum 63 sessions, lag 1.
- `SV_MAP_RULE` is the short-interest producer's symbol map.
- The CNMS suffix markers are defined here.
- `SV_FIELDS` (:452) is its own opt-in registry, group `finra_sv`.
- `ALL_FIELDS = {**FIELDS, **ISSUER_FIELDS, **SV_FIELDS}` (:479). As a result `FIELDS`, `DEFAULT_FIELDS` and the manifest order of every other field are unchanged.
- The module docstring gains one bullet.

**Implementation** (:1885-2276):

| Function | Line | Role |
|---|---|---|
| `si_canon` | :1896 | Canonicaliser, verbatim from `iteration21_finra_si_asof.canon` |
| `si_raw` | — | Exact-match spelling |
| `cnms_to_si` | :1906 | Rewrites CNMS suffix markers `p`/`r`/`w` as `PR`/`RT`/`WI` |
| `sv_listing` | — | Lists the CNMS files |
| `read_sv_receipt` | :1927 | Reads `manifest.csv` (sha256 / bytes / rows of each decompressed file) |
| `parse_cnms` | :1947 | Strict file contract, detailed below |
| `SvTickerMap` | :1993 | Point-in-time ticker map over the role's TickerHistory3 `ticker_tk` |
| `ticker_date` | :2113 | Last vendor date on or before the file date, within 7 days |
| `map_symbols` | :2120 | Maps a file's symbols to role columns |
| `sv_resolve_collisions` | :2144 | The producer's exact-match collision rule |
| `sv_field` | :2156 | The 126-slot ring over the extended session calendar |

`parse_cnms` checks:
- the header is exact;
- the last line is a row count that matches the rows;
- every Date equals the file date;
- symbols match `[A-Za-z0-9./-]+`;
- volumes are non-negative decimals, with ShortVolume ≤ TotalVolume.

`SvTickerMap` works in two passes:
- Pass 1 collects the trading dates and the role rows.
- Pass 2 collects non-role rows that share a (day, canonical) key with a role row. This is needed for the producer's ambiguity rule, which drops a canonical ticker held by more than one securityID that day.

**`run()` and the CLI:**
- New `finra_short_volume` argument.
- It refuses `sv_ratio126` without `--finra-short-volume` (:2294).
- The `finra_sv` group runs after the issuer group (:2342).
- The TickerHistory digest is reused when the th group already hashed the same file identity (:2321).
- `--finra-short-volume` is added to the CLI (:2426).

**What the manifest entry records** (under `short_volume`):
- **Source:** `formula_id` and `window_sessions` / `min_sessions` / `lag_sessions`.
- **Files:** the directory path; `files_read`; `files_sha256`, the sha256 of the canonical JSON of the sorted [name, gz bytes, gz sha256] list (the rule is recorded as `files_list_rule`); the first and last file dates; the receipt's path, bytes and sha256; the `downloaded_at` range; how many files are listed.
- **Calendar:** prefix sessions, role sessions without a file, files off the role calendar, files not read.
- **Mapping:** the rule, markers, canonicaliser and lookback; per-year and total counts of rows, kept, ambiguous, no role key, collision-dropped, kept-canonical-only, duplicate symbols and unknown lowercase markers; the TickerHistory stats.

Per-year coverage of finite member cells is in the standard `coverage.per_year` block. The `sources` list holds the directory summary, the receipt and the TickerHistory pin.

### Tests: `atx-engine/tools/test_prepare_research_fields_sv.py`

- The reference implementation (:138) uses the producer's own `canon` and `build_maps`, loaded from `build-equity/audits/iteration21_finra_si_asof.py`. It transcribes the producer's collision loop and computes the window by brute force.
- `ShortVolumeField` (:192) tests:
  - full equality with the reference;
  - the ratio for one cell, by hand;
  - ambiguity, collision, duplicates and the marker rewrite;
  - that a ticker change is picked up on the file's own date;
  - the minimum-count and zero-total rules;
  - the manifest records;
  - lag 1: changing the file dated `SESSIONS[30]` leaves rows 0-30 unchanged and changes row 31;
  - refusals: a tampered file, the missing argument, and seven contract violations.
- `SymbolMapReuse` (:332): the canonicaliser pattern and 7-day lookback equal the producer's, and the collision-rule unit test.
- `ByteIdentity` (:357): runs the full 43-field recipe (legacy and issuer) with and without `sv_ratio126`. Every other field's bytes, `files` entry, manifest entry and source_checks are identical, and the digest-reuse path gives the same `sv_ratio126` bytes as a standalone run.

Mutation check in a scratch copy: breaking the lag, the marker rewrite, the collision rule, or the minimum count (62 instead of 63) makes 6, 4, 4 and 2 tests fail respectively.

Extra check, not committed: I ran the base builder (17a12949, via `git show`) and the new builder on the synthetic fixture.
- The full 43-field recipe: 0 differences in field bytes, `files` entries or manifest entries. Only the code-identity keys and the added entry differ.
- The default 8 fields: only the code-identity keys differ.
- The fields-v6b manifest was produced by exactly the base builder blob (08fa5ae0).

### Library: `atx-impl/strategies/generate_fund_ic_v61.py`

- `pinned_v6()` (:101) re-derives v6 from its own generator and checks the sha pins and the committed files.
- `field_table()` (:118) and `validate()` (:128) add `sv_ratio126` to the v4 field table without modifying the pinned validator module.
- `sv_flow_parts()` (:141) builds the member.
- `documents()` (:149) checks that:
  - the tree is `rank(bin(-1 * CsNeutG(sv_ratio126, grp_ff12)))`;
  - there is no Ts op anywhere and `prior_bars` is 0;
  - the 38 v6 entries are a byte-identical prefix of the candidate list text;
  - the candidate, lineage and template key orders match v6.

The member is:
- **DSL:** `rank((-1 * group_neutralize(sv_ratio126, grp_ff12)))`
- **Labels:** id `sv_flow`, family and theme `short_interest`, tier B- (tier_rank 5), `prior_sign` 1, `raw_prior_direction` -1.
- **Citation:** Wang, Yan and Zheng (2020, JFE).

The recipe adds:
- the `sv_ratio126` field declaration, its clock and origin (fields_v7);
- `v61_changes`, `promotion_tests` (P1-P3), `industry_demeaned_members`;
- trial accounting (admission +1, composition +1, construction +1, DSR N 29);
- the FINRA row of `needs_new_field` marked resolved;
- static-validation maxima recomputed;
- the short_interest theme description updated. This is also in the library `families`; the candidate entries are untouched.

### Checker: `atx-impl/strategies/check_fund_ic_v6.py`

- `prefix_errors()` (:111) plus the `--require-baseline-prefix` and `--expect-added` options (:136). The baseline's candidate entries must be an identical prefix, in the same order, and the appended ids must be exactly those expected.
- With the defaults, the v6 behaviour and its tests are unchanged.

### Library tests: `atx-impl/strategies/test_generate_fund_ic_v61.py`

- Documents are deterministic, committed and pinned, and `--check` mode passes.
- The v6 entries are unchanged (objects and bytes), and `sv_flow` is appended with the exact DSL and labels.
- The fitter's `load_priors` reads 39 × prior_sign +1 and B- for `sv_flow`.
- A numpy mirror of `sv_flow` (the v4 test evaluator) gives the negated FF12 demean, ranked. A NaN label stays NaN, and more shorting flow within an industry ranks lower.
- The checker in v6.1 mode passes with `sv_ratio126` in the manifest, fails without it, and fails on a tampered v6 entry.

### Run script: `.superpowers/sdd/mega-alpha-20260926/studies/v61_train.sh`

- **Phases:** `fields` (:50), `u` (:79), `fit` (:94), `p1` (:110), `w` (:135), `nav` (:150), `summ` (:160), `all` (:165).
- **Pins** checked at start: library and recipe v6.1, library v6, role lo1 3e79978a, fields-v6b c69b9c0f, bridge, events.
- **`fields`:** builds `recent-fast-train-2020-2022-v2-lo1-fields-v7` from the 40 fields-v6b names plus `sv_ratio126`. It then checks that:
  - all 40 fields-v6b files are byte-identical in v7;
  - the only field added is `sv_ratio126`;
  - the CNMS file-list sha equals the pinned `6a968e5a…` (1,112 files, 2018-08-01..2022-12-29).

  It also prints the per-year coverage.
- **`u`:** runs the checker in v6.1 mode first, then the IC runner with its own cache `mega-candidate-cache-v61`.
- **`fit`:** ew-theme-v1, screen v4-prior-v1, output `mega-weights-v61-ew`.
- **`p1`:** prints `sv_flow`'s status, failed checks, runner sign vs the prior (and whether they agree), HAC t, tau against the limit, max |rho| and with whom, and `redundant_with`. It also prints si_ratio / dtc / si_change, and any status changes of the 38 v6 members against `mega-weights-v6u-ew/admission.json`.
  - PASS requires `status == admitted` and `sign_agrees`.
  - Otherwise it exits 10. `all` stops there, and `w` refuses on its own if P1 has not passed.
- **`nav`:** `mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` with `--aim-leverage 1.247` fixed, aim-partial-v5, theta .05, dust .1, price-risk-v1, and `--order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache`.
- **`summ`:** `nav_summ.py` paired against `mega-nav-v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`, with `--dsr-n 29`.
- Outputs are never overwritten.
- Tested: `bash -n` passes, and also with CRLF line endings. Git Bash handles the CRLF heredocs, and the two embedded Python snippets ran on existing artifacts (section 8.6).

## 2. How point-in-time and the lag are guaranteed

**The ratio at role session d** (`sv_field`, loop at :2187-2194):
- Row d is computed from the ring before session d's file is loaded.
- The ring then holds exactly the files dated on the 126 sessions of the extended calendar before d.
- Next, the file dated d is written into slot `e % 126`, overwriting session d-126, which is no longer in the window for d+1.
- So the file dated d is first used at d+1 (lag 1). The last role session's file is never read.
- The lag test proves this: rows up to and including the changed file's own session are identical; the next row changes.

**The extended calendar:**
- It is the role's sessions, preceded by the last 126 or fewer FINRA file dates before the role's first session.
- For lo1 the prefix is empty, because the role starts 2018-06-01 and the first file is 2018-08-01.

**The ticker map:**
- For a file dated f, it uses TickerHistory3 rows dated at or before f: the last vendor trading date within 7 calendar days.
- A row dated s is known at the s 22:00 UTC mark, and f < d, so every input to row d is known before d's decision.
- A ticker change is picked up on the file's own date (tested with OLD/NEW around 2024-09-16).

**The seal:** only files dated before the role's last session (under 2025-01-01) are read.

**Inputs pinned:**
- Each file's decompressed bytes and sha256, and its row count, are checked against `manifest.csv`.
- TickerHistory3 must match the role's `source_sha256`.
- The list of files read is hashed into the manifest.

## 3. Symbol mapping reuse

`si_shares` / `si_dtc` are mapped by their producer, `build-equity/audits/iteration21_finra_si_asof.py` (see `asof/mapping_report.json`), not by the builder. I ported that rule into the builder:
- the canonicaliser (upper-case, then strip `.`, whitespace, `/`, `-`);
- ORATS `ticker_tk` on the last trading day at or before the file date, within 7 days;
- a canonical ticker held by more than one securityID that day is dropped;
- when several symbols map to one securityID, the one that exactly matches the ticker spelling is kept; otherwise all are dropped;
- within a file, the last row of a symbol wins.

The test compares the builder's port against the producer's own functions.

**One addition, needed for "class-share suffixes handled the same way":** CNMS files spell suffixes the CQS way. The short-interest `symbolCode` and ORATS tickers do not:

| Line | CNMS | Short interest | ORATS |
|---|---|---|---|
| Class B | `BF/B` | `BFB` | `BF.B` |
| Preferred K | `CpK` | `CPRK` | `C.PRK` |
| Rights | `GCVr` | `GCVRT` | `ACP.RT` |
| When-issued | `GTXw` | — | `AAN.WI` |

- `/` is already stripped by the canonicaliser.
- The lowercase markers `p`/`r`/`w` are rewritten as `PR`/`RT`/`WI` before the unchanged canonicaliser.
- **Verified on real files (format only):**
  - On 2020-01-15, 421 of 427 CNMS preferreds and 2 of 2 rights match a short-interest symbol after the rewrite; only 3 preferreds match without it.
  - Class shares: 39 of 40 match.
  - ORATS spells these lines with the same `.PR` / `.RT` / `.WI` suffixes.
- **Why it matters:** without the rewrite, `CpK` canonicalises to `CPK` and collides with Chesapeake Utilities. The exact-match rule then drops both, every day. There are 1,070 such collisions in a sample of 323 files (CPK, CPL, CPS vs Citigroup preferreds).

## 4. Real-data measurement

`sv_ratio126` alone, on role lo1, output to my scratchpad only. No returns were touched.

- **Time and memory:**
  - 40.6 s wall.
  - The peak working set is 525 MiB (psutil `peak_wset`, including the interpreter and pyarrow). The builder's own sampled peak is 404 MiB.
  - Two runs gave identical bytes.
- **Files:** 1,112 read, from 2018-08-01 to 2022-12-29 (sha 6a968e5a…).
  - Role sessions without a file: 0.
  - Files off the role calendar: 0.
  - The 60 missing weekdays from 2018-08 to 2024 are all exchange holidays.
- **Rows:** 9,871,862 in total.
  - 5,112,357 mapped to lo1 lines.
  - 64,345 kept on the canonical form only (1.3%).
  - 4,759,505 have no role key (instruments outside lo1, or unmapped).
  - 0 ambiguous, 0 collision drops, 0 duplicate symbols, 0 unknown markers.
- **TickerHistory:** 17,156 canonical tickers, 21 role line-days carrying two tickers.
- **Coverage of finite member cells:**
  - 2018: 0.498 (warm-up; the first file is 2018-08-01, and 63 sessions are needed).
  - 2019-2022: 1.000.
  - Score window: 1.000.

## 5. Expected RSS and time for the fields phase

This is for lo1, 5,627 instruments × 1,155 dates, with 41 fields.

- **Time:** the 40-field part is unchanged (same code paths and bytes). `sv_ratio126` adds about 30-45 s.
  - TickerHistory is already hashed by the th group, and its digest is reused.
  - The two map passes take about 5-10 s.
  - Reading 1,112 files takes about 28 s.
  - The 1,800 s cap is not at risk.
- **Memory:** the sv step's transient peak is about 450 MiB above the baseline, during the TickerHistory key build (5.16 M role rows). The ring is only 12 MB.
  - Groups run in sequence and free their matrices, so I expect under 700 MiB.
  - I did not measure the full 41-field run, so the script passes `--max-rss-mib 1536`, as the owner allowed.

## 6. Root command lines

Integrate the branch into pool-2 first (commits c026d6a7, 97a34a40, 0855bdad). The script `cd`s to pool-2.

```
bash .superpowers/sdd/mega-alpha-20260926/studies/v61_train.sh fields
bash .superpowers/sdd/mega-alpha-20260926/studies/v61_train.sh u
bash .superpowers/sdd/mega-alpha-20260926/studies/v61_train.sh fit
bash .superpowers/sdd/mega-alpha-20260926/studies/v61_train.sh p1      # exit 10 = P1 failed: stop
bash .superpowers/sdd/mega-alpha-20260926/studies/v61_train.sh w
bash .superpowers/sdd/mega-alpha-20260926/studies/v61_train.sh nav
bash .superpowers/sdd/mega-alpha-20260926/studies/v61_train.sh summ
# or all phases, stopping at the first failure:  bash .../v61_train.sh all
```

The builder command the `fields` phase runs:

```
"C:/Program Files/Python312/python.exe" atx-engine/tools/prepare_research_fields.py \
  --role build-equity/recent-fast-train-2020-2022-v2-lo1 --role-sha256 3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809 \
  --output build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7 \
  --fields si_shares,si_dtc,iv_atm_21d,iv_atm_63d,iv_atm_126d,earn_recent,shares_out,mkt_ret,be,at,at_lag4,lt,che,debt,sale_ttm,gp_ttm,oi_ttm,ni_ttm,ni_q,ni_q_lag4,be_lag1q,be_lag1q_lag4,cfo_ttm,capx_ttm,xrd_ttm,dvc_ttm,prstkc_ttm,sstk_ttm,txt_q,txt_q_lag4,shrs_q,shrs_q_lag4,noa,noa_lag4,sue,fscore,me_company,grp_sic2,grp_ff12,grp_ff49,sv_ratio126 \
  --finra C:/atx/data/finra_short_interest --tickerhistory C:/Users/natha/Downloads/TickerHistory3.parquet \
  --finra-short-volume C:/atx/atx-db/data/raw/finra_short_volume \
  --identity-bridge build-equity/identity-bridge-r4-v1 --identity-bridge-sha256 ddf9716459a1116b85f713ca9cb788c3db753a6e1fea8eba335ed34320baebaa \
  --fund-events build-equity/fundamental-events-v2 --fund-events-sha256 74ed9a50ea686e0b0842ff9b09e78d6653ddeedd0d42f37893873ce269e3dd71 \
  --fund-lag-sessions 1 --max-rss-mib 1536 --max-seconds 1800
```

The static check the `u` phase runs:

```
check_fund_ic_v6.py --manifest <fields-v7>/manifest.json --library atx-impl/strategies/fund_industry_ic_v61.json \
  --baseline atx-impl/strategies/fund_industry_ic_v6.json --require-baseline-prefix --expect-added sv_flow
```

## 7. Decisions made while implementing the pre-registration literally

1. **Rank wrapper.** The DSL is `rank((-1 * group_neutralize(sv_ratio126, grp_ff12)))`.
   - The pre-registration says "the field demeaned within FF12 (the DSL's existing group-demean op)".
   - I used the same op and grp idiom as `ind_adj_rev_5`, which is `rank((-1 * group_neutralize(x, grp_ff49)))`, with FF12 as the pre-registration requires.
   - The house cross-section rank is the only op on top; there is no time-series op. Every one of the 38 members carries a rank wrapper, and without it the composite would mix a raw ratio (scale about 0.05) with ranks.
   - **The reviewer should confirm this.**
2. **Prior sign.** The negative prior sign is embedded in the DSL (`-1 *`), and the `prior_sign` label is +1 (`raw_prior_direction` -1 in the lineage). This is the v4+ convention, and the fitter refuses `prior_sign` -1.
3. **Calendar before the role's first session.** Sessions before the role starts are the FINRA file dates (one file per US equity session). This has no effect on lo1 (prefix 0).
4. **A session without a file** is part of the window and simply contributes no row.

## 8. Concerns

1. **Market-maker contamination.** FINRA's consolidated short volume counts market makers' hedging shorts. The daily ratio is mostly liquidity provision (about half of off-exchange volume), and only the 126-session aggregate is used. The literature grade is B-: the evidence covers 2010-15 only.
2. **Vintage.** The files were downloaded on 2026-09-27, long after their trade dates. Whether FINRA re-published any file is unverified. This is recorded as a caveat in the field entry.
3. **Symbology gaps.**
   - Lines whose ORATS ticker differs from FINRA's beyond the canonicaliser are unmapped: renames between vendor and FINRA, units (`/U`), when-issued lines.
   - 1.3% of kept rows match only on the canonical form (e.g. `BF/B` vs `BF.B`).
   - The marker rewrite (section 3) goes beyond a verbatim copy of the producer's rule; it is justified above.
   - Member coverage is still 100% in 2019-2022, so gaps hit non-members or fall inside the ≥63-session tolerance.
4. **Days missing from FINRA:** none in the lo1 window. The run reports them if any appear.
5. **Redundancy pass order.** `sv_flow` is tier B-, at roster position 39. The admission pass orders by (tier, roster order), so `sv_flow` is screened after every A..B- member and before the six C+ members: accruals, fscore, asset_growth, sue, within_ind_mom, seasonality_same_month.
   - If `sv_flow` is admitted, it could displace a C+ member when their correlation exceeds 0.90 (unlikely).
   - `p1` prints any status change of the v6 members against the v6u admission.
6. **Data I read** (hygiene disclosure):
   - `sv_ratio126` coverage and mapping statistics from the scratch build (section 4). These are not return statistics.
   - Format and symbology checks on raw FINRA files.
   - To exercise the `p1` snippet, I ran it on the existing v6u admission with `si_change` standing in for `sv_flow`. That printed v6 statistics that were already on record (si_change reject_veto, HAC t -2.90).
   - No `sv_flow` statistic exists yet, and nothing was related to returns.
7. **Memory.** Section 5: the full-run peak was not measured, so the script uses `--max-rss-mib 1536`.
8. **Integration.** The run script and the pinned library/recipe hashes assume pool-2 contains the three commits.
