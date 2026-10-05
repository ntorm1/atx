# Root wave-2 gate report (diagnostics part 1: step group M2g, Ruling GATE-EARLY)

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`, code head at start `42f42a07`. Zero trials: the trial
ledger `build-equity/trials.jsonl` is checked at start and end (133 lines, sha256 prefix `27e40f9f`); no step passes a
ledger to anything. Every real-data run goes through `scripts/run_bounded_research.py` with `--max-rss-mib 2048`
(RUN-1) unless a section states a measured figure. Runs of record use the Release exes (`build-equity-rel/bin`).
Evidence: `m2g-logs/` (tools in `m2g-logs/tools/`). Each section's "pre-run list" (what the run reads, what it prints,
what it may NOT print) is committed before that run; the result is appended below it afterwards.

Blind rule for every section: no 2020-2023 return / IC / Sharpe / turnover / NAV statistic is printed or read by root;
nothing dated 2024-01-01 or later is opened.

Start state: trial ledger 133 lines, `27e40f9f6314585e...` (checked 2026-10-04 14:52 EDT). Free memory 1.6-4.1 GB.

## Item 1. AL-DATA: files, delta-check nits, spec amendments 5.1 / 5.2 (ALD-12 (c))

- `al-data/*.md` as they stood: already committed byte for byte in `ab189471` (M2f's docs commit; working tree = HEAD
  for all four files, checked by SHA-256). Nothing new to commit before the nits.
- D-n1 and D-n2 (delta check's replacement text, verbatim) applied as doc slips to `al-data/atx-db-request.md`: the S4
  bullet (old `:263-264`), the row-11 tail (old `:362`), the `code` fields sentence (old `:316-317`) and the
  backfill / names / Stage I bullet (old `:322-323`). Committed with this report's docs.
- Spec amendments 5.1 (`scripts/specs/p9/blocked/lazy_prices.json` lines 17, 18, 19, 20, 28) and 5.2
  (`scripts/specs/p9/blocked/hf_crowd.json` line 20), exactly as `task-AL-DATA-report.md` section 5 gives them, in ONE
  spec commit `30b9c9d2`. Tool `m2g-logs/tools/ald_amend.py` read the report's four ```json fences and checked every
  "before" line byte for byte against `git show HEAD:` first: **all six byte-identical** (both files LF, no BOM,
  HEAD blob = working tree), then substituted and parsed: key set and order unchanged; changed keys exactly
  `blocked_on, deviation, domain, formula, notes` (lazy_prices) and `notes` (hf_crowd); `history` untouched
  (`m2g-logs/ald-amend-apply.log`).
- 5.3 (`russell_recon` second aspect) NOT applied: it moves the pinned `formula_sha256` (`787a8d87...a699`); P10 note
  (ALD-11, ALD-12 (c)).
- Registration tests re-run (explicit path): `scripts/tests/test_p9_al_sig_registrations.py` **11 passed** before and
  **11 passed** after (`m2g-logs/ald-pyt-registrations-{before,after}.log`). It is the only reader of the two files in
  the tree (`git grep`; `classes.json:16` pins the class by git only).

## Item 2. ALD-DIAG (OD5-1 (iii), ALD-10)

**Pre-run list (committed before the run).**
- Tool: `m2g-logs/tools/ald_diag.py` (read-only, written for this step). No existing reader prints per-year counts with
  the ALD-10 split: `atx-impl/tools/era_data_audit.py` prints whole-role delisting totals and needs a fields manifest
  and field payloads; the new script follows its no-return contract.
- Reads: the sealed role manifest `build-equity/train-2020-2023-lo3/manifest.json` (pin `e1c67101...95f4`) and its
  payloads `sessions.i64`, `ids.u64`, `member.u8` (each hash-verified against the manifest). Never opens `close.f64`,
  `raw_close.f64`, `volume.f64`. The delisting events' `delist_return` / `delist_return_if_performance` keys and the
  by-cause `with_return` counts are dropped on load. Refuses a role whose session axis reaches 2024-01-01 before any
  payload is opened.
- Prints, per year 2020-2023: names in the role universe (distinct instruments with `member.u8 == 1` on a session of
  that year); universe exits through a delisting (role delisting terminations with `kept_member_at_last_session`) whose
  last session is on or before 2023-12-01, also by cause (`delist_code`) and as a share of the year's names; exits with
  a last session after 2023-12-01 as "unclassified (within 30 days of the seal)". Context: the 2020-2023 totals, the
  warm-up exits (last session before 2020), terminations of non-member lines by year (not universe exits), the stage's
  `counts` block, `delist_code` values outside the five causes, and the count of exits whose line is a member at its
  last session in `member.u8` (consistency).
- May NOT print: any return (delisting return, imputed or not), price, volume, NAV, IC, Sharpe or turnover statistic.
- Run: `run_bounded_research.py --output build-equity/p9-m2g-ald-diag-run --seconds 600 --max-rss-mib 2048 --
  python .../ald_diag.py build-equity/train-2020-2023-lo3/manifest.json e1c67101...95f4`.
- Disclosure (preparation, before this list): to write the script, root printed the event key names and types, the
  distribution of `delist_code` / `cause_basis`, the `last_session` range, the count of `kept_member_at_last_session`,
  the stage's `counts` block and the role's rule texts (metadata only; no value key).

**Result** (run `build-equity/p9-m2g-ald-diag-run`: completed, exit 0, 0.5 s, peak 31 MiB; source `3fd0bf98`, code
pathspec clean; evidence `m2g-logs/ald-diag.txt`, `m2g-logs/receipt-p9-m2g-ald-diag.json`). Role sessions 1405
(2018-06-01 .. 2023-12-29), 5922 instruments; payloads hash-verified; the seal check passed (no session >= 2024-01-01).

| year | names in the role universe | universe exits through a delisting, last session <= 2023-12-01 | share of the year's names | by cause (mna / performance / non_common / unknown / unknown_unlinked) | unclassified (last session > 2023-12-01) |
|---|---|---|---|---|---|
| 2020 | 2,420 | 83 | .034 | 82 / 1 / 0 / 0 / 0 | 0 |
| 2021 | 2,540 | 103 | .041 | 97 / 1 / 0 / 5 / 0 | 0 |
| 2022 | 2,325 | 106 | .046 | 102 / 1 / 0 / 3 / 0 | 0 |
| 2023 | 2,186 | 95 | .043 | 88 / 2 / 0 / 5 / 0 | 2 (within 30 days of the seal) |

- 2020-2023: 3,256 distinct universe names; 387 classified delisting exits + 2 unclassified. Lower bound (ALD-10).
- Context: 152 universe-exit terminations in the warm-up (last session before 2020); terminations of lines that were
  NOT universe members at their last session (not universe exits): 2018 7, 2019 40, 2020 77, 2021 66, 2022 104,
  2023 155; the stage's counts: terminations 990, continued (not terminations) 152, rows on the role in the window
  1,145, rows total 9,727, rows available on or after the stage's cutoff dropped 3; no `delist_code` outside the five
  causes.
- Check: all 541 universe-exit events have `member.u8 == 1` at their last session (0 mismatches).
- Reading (for the prereg line beside the OD5-1 flag; gates nothing): the role keeps its pre-2024 delistings (about
  3.4-4.6% of each year's names exit through a delisting), but almost all are M&A; performance delistings of universe
  members are 1-2 a year (5 in 2020-2023), so the universe holds few failure exits.

## Item 3. rho of the leverage-only pairs (B2-13)

**Pre-run list (committed before the run).**
- Tool: `m2g-logs/tools/b2_13_rho.py`, the M2f `b2_rho.py` method with the engine eval verb as the producer: nav_summ's
  paired-block series (primary scenario, `load_daily` with its seal refusal, `net_series`, `align`) handed to the
  Release verb `build-equity-rel/bin/atx-research-eval.exe` (p9-2j, `a57a2090`) exactly as nav_summ's `--engine-stats`
  adapter writes them; called as `--daily c0=<child> --paired <reference> --json`.
- Pairs: X-10 vs Y-F0 and Y-1 vs X-10 on the P9-B0 references (`p9-b0-x10-nav` vs `p9-b0-yf0-nav`; `p9-b0-y1-nav` vs
  `p9-b0-x10-nav`) and, as in M2f, on the ledgered dirs (`mega-nav-v8x-theme-erc-L2.0-v8ysb` vs
  `mega-nav-v8x-theme-erc-L1.1828-v8ysb`; `mega-nav-v8y-vol-target-L2.0` vs `mega-nav-v8x-theme-erc-L2.0-v8ysb`).
- Reads: each dir's `summary.json` (primary scenario name only) and primary-scenario daily CSV; from the verb's JSON
  only `cells[0].paired.{sessions, rho, one_minus_rho, degenerate}`.
- Prints: dirs, scenario, daily-file SHA-256 prefix, T, the verb's rho and 1 - rho (stable form), its degenerate flag,
  numpy's corrcoef rho and the stable form from the same arrays (cross-check), the B2-R1 tolerance 1e-12 / (1 - rho)
  and the class (degenerate iff 1 - rho < 1e-6; then the B2-13 fallback).
- May NOT print: Sharpe, dSR, SE, t, p, CI, mean, vol, NAV, turnover or any other key of the verb's document.
- Run: `run_bounded_research.py --output build-equity/p9-m2g-b2-13-rho-run --seconds 600 --max-rss-mib 2048 -- python
  .../b2_13_rho.py build-equity-rel/bin/atx-research-eval.exe`.

**Result** (run `build-equity/p9-m2g-b2-13-rho-run`: completed, exit 0, 1.1 s, peak 51 MiB; evidence
`m2g-logs/b2-13-rho.txt`, `m2g-logs/receipt-p9-m2g-b2-13-rho.json`). Verb `a57a2090...`; T = 1005 common sessions in
every pair; scenario `modeled-1bn-stale5-v1+swap-fin-v1`.

| pair | rho (verb) | 1 - rho (verb, stable form) | class |
|---|---|---|---|
| X-10 vs Y-F0 (P9-B0 `p9-b0-x10-nav` vs `p9-b0-yf0-nav`) | 0.9994863034956717 | 5.13696504329025e-4 | paired test defined (1 - rho >= 1e-6) |
| Y-1 vs X-10 (P9-B0 `p9-b0-y1-nav` vs `p9-b0-x10-nav`) | 0.9947113980219977 | 5.288601978002812e-3 | paired test defined |
| X-10 vs Y-F0 (as ledgered) | 0.9994863034956717 | 5.13696504329025e-4 | paired test defined |
| Y-1 vs X-10 (as ledgered) | 0.9947113980219977 | 5.288601978002812e-3 | paired test defined |

- Cross-check: numpy corrcoef differs from the verb by 4.4e-16 / 3.3e-16; the stable forms agree to the last digit.
- B2-R1 tolerances for paired floats: 1.947e-9 (X-10 vs Y-F0), 1.891e-10 (Y-1 vs X-10).
- Neither pair is degenerate: the B2-13 fallback ("na" paired SE / t / p) is not triggered. X-10 vs Y-F0 is the
  closest pair measured so far (1 - rho 5.1e-4, two orders above the 1e-6 bound).

## Item 4. COV: S9, COV-7, D-COV, COV report steps 4 and 5

### 4 (a) S9 (COV-9)

**Pre-run list.** Reads the role manifest `build-equity/train-2020-2023-lo3/manifest.json` (pin `e1c67101...95f4`):
`universe.delisting.returns_applied` and the per-event `returns_applied` flags. Prints the boolean, the key list and the
count of events with `returns_applied` true. No value of any event is printed. Before every COV run below.

**Result: S9 HOLDS.** `universe.delisting.returns_applied = false`; events with `returns_applied` true: 0 of 990
(`m2g-logs/cov-s9.txt`, tool `m2g-logs/tools/s9_check.py`). (No real-data COV run followed: 4 (b) missed.)

### 4 (b) COV-7: Debug = Release container bytes on the lane fixture

**Pre-run list (synthetic data; written before the run).**
- Builds: Debug `atx-equity-strategy-risk` p9-2k (`94b5a5e7`, at `30b9c9d2`); Release `atx-equity-strategy-risk`
  (+ `atx-equity-strategy-targets`) at the next tag.
- Fixture: the lane fixture is `atx-impl/tests/strategy_cov_fixture.hpp` `CovPanel(300, 200, 23)` written by
  `write_role_and_fields` (the verb suites' `verb_panel()`); unchanged since the COV merge `694b4b95`. It lives only in
  a test temp dir, so tool `m2g-logs/tools/cov7.py` runs the Debug `atx-impl-strategy-target-tests.exe
  --gtest_filter=CovRecipe.CovV1RecipeRecorded` with `TMP` / `TEMP` set to a fresh directory, waits until the fixture's
  `in/role` and `in/fields` manifests exist and every payload hash in them matches, copies `in/` to
  `build-equity/p9-m2g-cov7/in` (the test then finishes normally and removes its own dir), and re-verifies the copy.
- Runs (direct; synthetic): for each exe (Debug p9-2k, Release) and each recipe (`atx-risk-v1.1`, `atx-cov-v1`):
  `risk --role .../in/role/manifest.json --role-sha256 R --fields .../in/fields/manifest.json --fields-sha256 F
  --output build-equity/p9-m2g-cov7/<dbg|rel>-<v11|covv1> --random-portfolios 4 --emit-exposures all --emit-container
  --bias-families extended --recipe <recipe>`.
- Identity criterion, per recipe (Debug vs Release):
  1. `cov-diff <dbg>/model.atxcov <rel>/model.atxcov` exits 0 with "equal through the last session" (no differing
     block), run with both exes.
  2. Byte comparison of the two `model.atxcov`: same size; every byte equal outside exactly these producer-derived
     ranges: the META section (`MetaJson`), where the parsed JSON may differ only at `/producer/executable_sha256`
     (and `/producer/engine_git_sha` if the two trees' configure SHAs differ), the header's `root_sha256` field
     (bytes 224-256) and the trailer's `root_sha256`. Every block byte, the header's first 224 bytes, the section
     table, `FactorNames`, `DateIndex` and `BlockHashes` byte-identical.
  - Why the root SHA-256 itself cannot be equal (stated before the run): the root hashes the META section
    (`cov_container.cpp:552-557`), and META carries `producer_json()` = the running exe's SHA-256 and the configure-time
    engine git SHA (`strategy_risk_verb.cpp:959-963`, `:1081`); the COV report records this ("the producer block makes
    the root build-specific", Decisions). So re-review 2's "equal root SHA-256" is replaced by criterion 2, which is
    stronger on every byte the root covers except the producer string. Recorded beside it: each file's root SHA-256.
- Also compared (reported; not the COV-7 criterion): every other output file by SHA-256 and `manifest.json` by JSON
  leaf path (expected to differ only at `/producer/executable_sha256`, `/container/{file_sha256, root_sha256}`,
  `/files/model.atxcov/sha256`).
- A difference outside the ranges of criterion 2, or a non-zero `cov-diff`, is an identity miss: item 4 (c) stops.

**Result: IDENTITY MISS (both recipes). Item 4 (b) stops; 4 (c) is not run (COV-7: "if Debug and Release differ, the
Release ruling is void and the run waits").** Evidence `m2g-logs/cov7.txt`, `m2g-logs/cov7-attribution.txt`, tool
`m2g-logs/tools/cov7.py`; outputs kept in `build-equity/p9-m2g-cov7/` (synthetic, 0.2 GB).
- Builds: Debug risk exe p9-2k `94b5a5e7` (1 TU, 9 s, 0 warnings); Release risk exe p9-2l `a31a0f96` and Release
  targets exe p9-2l `53ca5dd4` (2 TUs, 9 s; first attempt refused by the memory gate, no receipt, retried when lane
  C3's Release build in pool-15 had finished). Fixture extracted by the Debug test exe `ecb229ca` (p9-2d):
  role manifest `c93324e7...648d`, fields manifest `7e0cc52b...68c7`; copy re-verified; the test passed.
- atx-risk-v1.1: `cov-diff` exit 1 with BOTH exes: "first difference at block 2 session 1546560000000000000 (block)".
  Same size (14,786,944 bytes); 55,258 bytes differ: META 63 (only `/producer/executable_sha256`), header root 32,
  trailer root 32, and outside the allowed ranges **Blocks 45,796 and BlockHashes 9,335**. Payloads: factor covariance
  31,552 of 32,556 finite cells differ (from the first forecast session, index 63; 79% of sessions), ULP distance
  median 17, max relative 1.5e-9; specific variance 7,928 of 9,504 (from session 252), median 4 ULP, max rel 2.8e-15;
  no NaN-pattern difference; `diagnostics.csv` and the bias files differ too (`style_exposures.f32`,
  `industry_slot.u8`, `factor_returns.csv`, `factor_structural.u8`, `exposures_last.csv` are identical).
- atx-cov-v1: the same picture: `cov-diff` exit 1 at block 2 with both exes; Blocks 49,934, BlockHashes 9,342 bytes;
  factor covariance 98% of finite cells differ (median 24 ULP, max rel 3.3e-11).
- Each build is deterministic run to run (Debug twice and Release twice: every payload and the container identical),
  so the difference is between builds, not between runs.
- Attribution NOT done (stop rule); what the synthetic outputs show: the WLS factor returns and the exposures agree
  bit for bit (`factor_returns.csv`, `style_exposures.f32`, `industry_slot.u8` identical); `diagnostics.csv` first
  differs at `bias_factor` (row 65), `lambda2_factor` (96), then the specific columns (252+); the container's first
  differing block is index 2 (a block field that `diagnostics.csv` does not carry). So the variance / EWMA / VRA
  arithmetic moves, not the regression. Candidates, by reading only: the CRT transcendental functions (`ucrtbased`
  vs `ucrtbase`, the M2c-RED-2 class) or a different reduction order between the Debug and Release code paths.
- Consequence: no D-COV, no COV report step 4 (Y-1 NAV through the container store), no step 5 (`atx-cov-v1` on
  TRAIN) in this step group; no `atx-cov-v1` store exists for W3-10 to pin. The Debug-built stores of record
  (`v8-risk-lo3-v10`, the kept `p9-m2d-cov-risk-container`) are untouched; a Release rebuild of the atx-risk-v1.1 store
  would NOT be byte-identical to them (same finding, atx-risk-v1.1 recipe). Needs a PM ruling (see the handoff).

### 4 (c) D-COV (cov-design section 5.3, COV report "What the churn-panel small-rank bias means", COV-5, COV-11)

**NOT RUN: 4 (b) missed.** The list below stays as written for the re-run once the PM rules on COV-7.

**Pre-run list (committed before the runs).**
- Gate: runs only if 4 (b) holds. S9 checked before each run.
- Runs (Release `atx-equity-strategy-risk`, bounded, `--max-rss-mib 2048`, `--seconds 1800`): the M2d step-3 argv of
  `p9-m2d-cov-risk-container` (role `train-2020-2023-lo3` pin `e1c67101...95f4`, fields
  `train-2020-2023-lo3-fields-v10` pin `a4a060ae...0809`, `--emit-exposures all --emit-container`) plus
  `--bias-families extended` and
  - `--recipe atx-risk-v1.1` -> `build-equity/p9-m2g-dcov-v11` (run dir `-run`);
  - `--recipe atx-cov-v1` -> `build-equity/p9-m2g-dcov-covv1` (run dir `-run`) = COV report step 5 ("Once,
    `--recipe atx-cov-v1 --emit-container --bias-families extended` on the same role"); its wall and peak are logged.
  No other flag; every atx-cov-v1 parameter is the recipe's (a = 1, 64 simulations, seed 7, simulated length Kish of
  HL 504; COV-5); the manifest's `recipe.eigen_adjustment` block is printed to show it.
- Reads (tool `m2g-logs/tools/dcov_read.py`): each store's `bias_summary.json` (families random, factor, eigen, minvar,
  optimized) and from `bias.csv` only the columns `family`, `name`, `z` of the eigen family; the manifest's `model`,
  `recipe.eigen_adjustment`, `eigen_adjustment.sessions`, `extended_families.unformed_sessions`, `container` block.
- Prints, side by side for atx-risk-v1.1 and atx-cov-v1 on identical observations: per family status, series ok,
  observations, full-sample b mean / median, series in band (and in the kurtosis band), rolling 63 / 252 share in band
  and b mean, QLIKE, MRAD 63 / 252; per eigen rank k: b_k (sample SD of z, ddof 1; `bias_statistic`), T_k, in band
  (|b - 1| <= sqrt(2 / T_k)), and the direction (b < 1 - band: over-forecast / over-corrected; b > 1 + band:
  under-forecast); the per-rank reduction of |b - 1| from v1.1 to atx-cov-v1; pooled eigen b over all ranks and
  without the lowest rank (eigen-00), with N and band sqrt(2 / N); ranks in band (count of ranks). Read in both
  directions (COV-11: b below 1 = over-corrected).
- May NOT print: any book family (none is formed: no `--book-weights`), any `realized_return` or `forecast_vol` value
  of `bias.csv`, any NAV, book return, IC, Sharpe or turnover figure.
- Kept: the atx-cov-v1 store `build-equity/p9-m2g-dcov-covv1` is what P9-MV / P9-V pin (W3-10): never deleted; its
  manifest pins (path and SHA-256 of `manifest.json` and of every `files` payload, the container root) are recorded in
  this section after the run.

### 4 (c) COV report step 4: Y-1 NAV through the container store vs its P9-B0 reference

**NOT RUN: 4 (b) missed** (the brief gates steps 4 and 5 on COV-7). The list stays for the re-run.

**Pre-run list (substitution list written before the run).**
- Run: the P9-B0 Y-1 argv (`build-equity/p9-b0-y1-nav-run/receipt.json`: `nav ... --vol-target vol-target-v1
  --risk-model build-equity/v8-risk-lo3-v10 --risk-model-sha256 862515d9...`) with three tokens changed: `--output
  build-equity/p9-m2g-cov-y1-nav`, `--risk-model build-equity/p9-m2d-cov-risk-container`, `--risk-model-sha256
  9c12852ea0cd578d2cf968fc858ae7cb986c308f4b533824a34d273ca7e017c7` (its `manifest.json`, which names the container:
  `RiskStore::open` takes the container path). Exe: Release `atx-equity-strategy-targets` (this step group's tag),
  bounded, `--seconds 600 --max-rss-mib 2048`. 0 trials (identity; SHA comparison only).
- Expected vs `build-equity/p9-b0-y1-nav` (Debug `407c34ad`): all 28 files present on both sides; every file
  byte-identical except `summary.json`, which may differ only at `/producer/build_type` (debug -> release) and
  `/producer/engine_git_sha` (the producer block is written only into the main pass's `summary.json`,
  `strategy_nav_replay.cpp:2953-2962`, `:3029`; G-P3 NAV half: Release = Debug outside `producer`, shown on X-5 at M1c).
  The risk-model directory and pin appear in no NAV output file (receipt argv only).
- Compared by file SHA-256 and JSON leaf path; CSVs by header names and differing row / cell counts only; no value is
  printed.
- If anything else differs: the same argv is run once with the Debug exe (`build-equity/bin`, p9-2d `90cb34a7`) to
  attribute the difference to the container path (Debug also differs) or to the build (Debug equal); reported, not
  re-run further.

### 4 (d) M2i: item 4 re-run in Release (rulings COV-12, COVR-1, PRE-29, EXE-CUR, RUN-1 / RUN-2)

Step group M2i (root, 2026-10-04), after the merges of D2R (`5afb5447`) and COVR (`c8ddf736`). COV-7's Debug = Release
precondition is replaced by Release = Release (COV-12 (b), cause class (i): the C runtime's `exp2`, COVR report). Evidence
`m2i-logs/` (tools `m2i-logs/tools/`).

**Pre-run amendments (written and committed before any run below).** The lists 4 (a), 4 (c) D-COV and 4 (c) step 4
stand as written, except:

- **Exe of record (EXE-CUR, PRE-29).** Every COV run (both D-COV stores and both repeats) is written by ONE file:
  Release `build-equity-rel/bin/atx-equity-strategy-risk.exe`, build tag **p9-2o** (preset equity-rel, source
  `c8ddf736`, the COVR merge; receipt `build-equity/mega-p9-2o-receipt.json`), SHA-256 `ccc73cd10ddc0c4454de50a51f2624e8f38f440e1ea5b47b984cbab06444d811` (63 TUs, 0 warnings). Its SHA-256
  is re-read before the first and after the last run, and each runner receipt records `executable_sha256`.
- **UCRT record** (M2c-RED-2 (d), COVR 8.1) before the runs: `ucrtbase.dll` 10.0.26100.7623 `2C08E77F...65EF`;
  `ucrtbased.dll` 10.0.22000.832 `19E79E91...F48D` (`m2i-logs/ucrt-before.txt`). The atx-cov-v1 store's bits depend on
  `ucrtbase!exp2` (COVR); a store from another `ucrtbase` build is not expected to be byte-identical.
- **Runner (COVR-1, RUN-1, RUN-2).** Every run: `"C:/Program Files/Python312/python.exe" scripts/run_bounded_research.py
  --output <out>-run --seconds 600 --max-rss-mib 2048 --build-type Release --admission-wait-seconds 1800 -- <exe> risk
  ...` (`--seconds 1800` of the D-COV list becomes 600). `covr_cmp.py` starts nothing (its compare logic is re-written
  as `m2i-logs/tools/tree_cmp.py`, which reads outputs the runner wrote). A run that reaches the 600 s cap stops its
  item: reported, not split.
- **Output dirs** `build-equity/p9-m2i-*` (instead of `p9-m2g-*`).
- **C1 = 4 (a)** unchanged (`m2g-logs/tools/s9_check.py` on `build-equity/train-2020-2023-lo3/manifest.json`, pin
  `e1c67101...95f4`), run before each COV run.
- **C2 = 4 (c) D-COV**, argv (the M2d step-3 argv of `p9-m2d-cov-risk-container` + `--bias-families extended` +
  `--recipe`; no other flag):
  `<exe> risk --role build-equity/train-2020-2023-lo3/manifest.json --role-sha256
  e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4 --fields
  build-equity/train-2020-2023-lo3-fields-v10/manifest.json --fields-sha256
  a4a060ae29c10080ca89b4b29335709558c2ae4842bd29db0398b23a43070809 --output build-equity/p9-m2i-dcov-<v11|covv1>
  --emit-exposures all --emit-container --bias-families extended --recipe <atx-risk-v1.1|atx-cov-v1>`; first
  `atx-risk-v1.1` -> `p9-m2i-dcov-v11`, then `atx-cov-v1` -> `p9-m2i-dcov-covv1` (= COV report step 5). Wall and peak
  from each receipt.
- **C3 = Release = Release on TRAIN** (replaces Debug = Release, COV-12 (b)): each recipe once more, same exe, same
  argv except `--output build-equity/p9-m2i-dcov-<v11|covv1>-r2` (run dir `-r2-run`), started as
  `cmd /c start "" /b /wait /affinity 1 <python> scripts/run_bounded_research.py ...` (`m2i-logs/tools/aff1.cmd`; every
  C2 / C3 run is driven by `m2i-logs/tools/dcov_run.sh`, which runs S9 first and copies the receipt) so the runner and
  the exe run on one CPU (the mask is inherited; probed on this host: parent and child report `[0]`). `m2i-logs/tools/aff_probe.py`
  records the exe's affinity while it runs. The estimator, S2b and the writer start no thread and have no worker knob
  (COVR report 4), so mask 1 is the "1 CPU / 1 worker" leg; the first run (C2) is the "all CPUs" leg. Criterion
  (`tree_cmp.py`, full SHA-256 of every file, printed): same file set and every output file byte-identical, incl. the
  whole `model.atxcov`, its root and `manifest.json` with META (same exe). Any difference = identity miss: stop and
  report; D-COV is then not read. The output path enters no output file (COVR: equal bytes across output dirs).
- **C4 = the 4 (c) reads**, tool `m2i-logs/tools/dcov_read.py` (new: the `m2g-logs/tools/dcov_read.py` the list names
  was never written; tried only on the synthetic COV-7 outputs before this list). Two amendments: (i) the manifest's
  `extended_families` is null in the COV-7 outputs; `extended_families.unformed_sessions` is then read from
  `bias_summary.json`; (ii) `bias.csv` is read in the columns `family`, `name`, `z` of the eigen family plus `session`,
  a timestamp used only to check that both stores score the same observations per rank (never printed). Prints and
  may-not-prints exactly as the list (no book family, no `realized_return` / `forecast_vol`, no NAV / return / IC /
  Sharpe / turnover figure). Output `m2i-logs/dcov-read.txt`, copied below. COV-11: per-rank b is read in both
  directions (b below 1 = over-forecast / over-corrected).
- **C5 = step 4.** Run: the P9-B0 Y-1 argv with three tokens changed: `--output build-equity/p9-m2i-cov-y1-nav`,
  `--risk-model build-equity/p9-m2d-cov-risk-container`, `--risk-model-sha256
  9c12852ea0cd578d2cf968fc858ae7cb986c308f4b533824a34d273ca7e017c7`; runner `--output
  build-equity/p9-m2i-cov-y1-nav-run --seconds 600 --max-rss-mib 2048 --build-type Release --admission-wait-seconds
  1800`. Exe: Release `build-equity-rel/bin/atx-equity-strategy-targets.exe` p9-2l `53ca5dd4...` (source `3fd0bf98`).
  EXE-CUR: `git diff 3fd0bf98..HEAD` over C++ / CMake lists only `atx-impl/tests/CMakeLists.txt` and
  `atx-impl/tests/strategy_cov_stage_diag.cpp` (COVR), which belong only to the EXCLUDE_FROM_ALL test exe
  `atx-impl-strategy-target-tests` (no flag, no other target; COVR review (b)); the diff over the files of the NAV
  target is empty: current. Expected vs `build-equity/p9-b0-y1-nav` as the step-4 list (every file byte-identical except
  `summary.json` at `/producer/build_type` and `/producer/engine_git_sha`); compared by `m2g-logs/tools/nav_cmp.py
  build-equity/p9-b0-y1-nav build-equity/p9-m2i-cov-y1-nav --allow summary.json:/producer/build_type --allow
  summary.json:/producer/engine_git_sha` (file SHA-256, JSON leaf paths, CSV shape and differing-cell counts; no value
  printed). Checked by reading that the vol-target block names no risk-model path: `strategy_vol_target.cpp:29-41`
  `parameters_json` has none (the `risk_model` directory block at `strategy_nav_v7.cpp:305-307` is the spo engine's
  only). If anything else differs: the one Debug attribution run, same argv with `--output
  build-equity/p9-m2i-cov-y1-nav-dbg`, runner `--build-type Debug`, Debug exe `build-equity/bin/
  atx-equity-strategy-targets.exe` p9-2m `c02717ef...` (source `5afb5447`; built this step group, replacing the stale
  p9-2d; no C++ / CMake change of its target since), compared the same way; then STOP and report (COV-12: the PM rules).
- **C5, COVR-1 reading (written before step 4 runs): `exp2` / `pow` / `exp` on a non-exact input on the NAV exe's read
  path of `--risk-model` (Y-1: `--vol-target vol-target-v1`, `--rule aim-partial-v5`, no `--two-speed`, no spo
  rule).** Path read: `strategy_nav_v7.cpp:1333-1340` -> `spo::RiskStore::open` (`strategy_spo.cpp:657-772`: SHA-256
  of the manifest, JSON parse, `diagnostics.csv` `stoll`, container `CovModelFile::open(..., VerifyMode::Full)` =
  hash verification, index / flag checks) -> per decision `RiskStore::read` (`strategy_spo.cpp:782-828`:
  `CovModelFile::view`, `scatter_cov_block` `strategy_risk_model.cpp:1623+` = copies and an f32 store, `isfinite`,
  f32 -> f64 widening, all exact) -> the scaler `strategy_risk_target.cpp:111-162` -> `engine::book::vol_target_update`
  (`vol_target.hpp:93-120`: sum, count, one division, `vol_target_leverage` clip) -> `gross_one_vol`
  (`atx-engine/src/book/risk_target.cpp:120-131`: w / gross, `factor_variance` = products and sums,
  `std::sqrt(periods_per_year * variance)`). **Found: none.** The only libm call on the path is `std::sqrt`, which
  IEEE 754 requires correctly rounded in every CRT. Present elsewhere and NOT reached by Y-1: `std::exp2` in
  `atx-engine/include/atx/engine/book/two_speed.hpp:49` (`two_speed_fast_theta`; `strategy_target_replay.cpp:82-83`,
  `:650-651` and `strategy_risk_target.cpp:184-201` call it / `two_speed_carry` only when `--two-speed` is on);
  `std::exp` / `std::log` in `strategy_spo.cpp:879-931` (`calibrate_gamma`, spo rules only); `std::log` of prices in
  `strategy_target_replay.cpp:670-671` (every NAV run, not the risk-model path; Release = Debug on it is shown by the
  M2g Q5 Y-F0 re-run, 26 / 27 files identical, `summary.json` producer only). The engine `exp2` sites the COVR review
  names (`atx-engine/src/risk/cov_ewma.cpp:20`, `src/risk/vol_regime.cpp:65`, `include/.../risk/horizon.hpp:72`) are
  included by no `atx-impl/src` file (of the four, only `book/two_speed.hpp` is). So a step-4 difference outside the
  list would not be explained by a CRT transcendental on the container read path.
- **C6 = step 5** is the atx-cov-v1 run of C2: wall, peak and the manifest's `recipe.eigen_adjustment` block printed.
- **C7 = store pins** (W3-10 extended by PRE-29), `m2i-logs/tools/store_pins.py` on both D-COV stores: path, SHA-256 of
  `manifest.json` and of every `files` payload (recomputed and checked against the listed value), the container block.
  Both stores are kept and never deleted. After C3 is recorded, `build-equity/p9-m2i-dcov-v11-r2`,
  `build-equity/p9-m2i-dcov-covv1-r2` (their `-r2-run` receipt dirs are kept; receipts copied to `m2i-logs/`) and
  `build-equity/p9-m2g-cov7/` (COV-12 ruled; synthetic, regenerable) are deleted by name.
- **Seal:** if a run stops on the run date's token, SEAL-ALLOW-3 applies after the per-hit check (sprint dir `tools/sealsrc.py`).

**Results (pre-run list committed in `f1d2a102` before the first run; code head `c8ddf736`).** Every run: bounded
runner, outcome `completed`, exit 0, no seal stop, no admission wait beyond the first check. Exe re-read after the last
COV run: `ccc73cd1...d811` (unchanged; every receipt's `executable_sha256` = it). Logs: `m2i-logs/dcov-runs.txt`,
`receipt-p9-m2i-*.json`, `cov-s9.txt`, `aff-probe-*.txt`, `c3-rel-eq-*.txt`, `dcov-read.txt`, `step4-*.txt`,
`store-pins.txt`.

- **C1 S9: HOLDS before each of the four COV runs** (`returns_applied = false`; 0 of 990 events true).
- **C2 / C6 runs (all CPUs):**

| run | recipe | wall (receipt) | peak tree RSS | argv sha256 |
|---|---|---|---|---|
| `p9-m2i-dcov-v11` | atx-risk-v1.1 | 47.1 s | 633 MiB | `c9eaedb6...` |
| `p9-m2i-dcov-covv1` (= COV step 5) | atx-cov-v1 | **211.2 s** | **601 MiB** | `91283767...` |

  Step 5's `recipe.eigen_adjustment` (manifest): `{"amplification": 1.0, "method": "menchero-wang-orr simulated (USE4
  B7)", "seed": 7, "seed_rule": "per session", "simulated_length": "kish effective count of the HL-504 weights",
  "simulations": 64}` (COV-5: every parameter the recipe's); `eigen_adjustment.sessions` `{"adjusted": 1279, "not_run":
  126, "refused": 0, "refused_budget": 0, "short_history": 0}`. Neither run approached the 600 s cap.
- **C3 Release = Release on TRAIN: HOLDS, both recipes.** Repeats under affinity mask 1 (probe: the exe and its parent
  runner report `[0]` on a 16-CPU host): `p9-m2i-dcov-v11-r2` 46.6 s / 601 MiB, `p9-m2i-dcov-covv1-r2` 181.3 s / 601
  MiB. `tree_cmp.py`: 12 of 12 files byte-identical in each pair, whole `model.atxcov` (510,677,824 / 510,678,016
  bytes), `manifest.json` incl. META and the container root equal (v1.1 root `b39a1b77...2e1d`, atx-cov-v1 root
  `20919f7b...9f8e`). Full per-file SHA-256: `m2i-logs/c3-rel-eq-v11.txt`, `c3-rel-eq-covv1.txt`. Not an identity
  miss; D-COV is read.
- **C4 D-COV (risk-forecast accuracy only; no book family exists, no NAV / return / IC / Sharpe / turnover figure).**
  Same observations in both stores: every family's observation count equal; per eigen rank the same sessions and
  finite-z pattern (51 ranks). Manifests: `extended_families` null in both; `bias_summary.json`
  `extended_families.unformed_sessions` 0 in both. Families (`bias_summary.json`), v1.1 | atx-cov-v1:

| family | status | series ok | obs | full b mean | full b median | in band | in kurt. band | roll63 share / b mean | roll252 share / b mean | QLIKE | MRAD 63 / 252 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| random | ok / ok | 64 / 64 | 69,696 | 0.9603 / 0.9611 | 0.9618 / 0.9624 | 34 / 34 | 44 / 44 | .7232 / .9431 \| .7234 / .9438 | .6072 / .9369 \| .6105 / .9374 | 2.4080 / 2.4078 | .1321 / .0824 \| .1320 / .0820 |
| factor | ok / ok | 51 / 51 | 51,193 | 1.0016 / 1.0041 | 1.0050 / 1.0181 | 27 / 25 | 36 / 35 | .6361 / .9745 \| .6364 / .9760 | .5764 / .9803 \| .5809 / .9812 | 2.5149 / 2.5144 | .1693 / .0944 \| .1692 / .0949 |
| eigen | ok / ok | 51 / 51 | 54,770 | 434.61 / 379.78 | 1.1629 / 1.1217 | 10 / 9 | 48 / 48 | .4346 / 367.78 \| .4824 / 331.78 | .2568 / 430.53 \| .2849 / 379.74 | 5,621,299.8 / 4,149,159.8 | 361.50 / 421.87 \| 326.03 / 371.99 |
| minvar | ok / ok | 1 / 1 | 1,089 | 1.4995 / 1.4747 | 1.4995 / 1.4747 | 0 / 0 | 0 / 0 | .5063 / 1.3364 \| .5628 / 1.3110 | .1969 / 1.3145 \| .2792 / 1.2908 | 3.1942 / 3.1430 | .3725 / .3178 \| .3577 / .2974 |
| optimized | ok / ok | 32 / 32 | 34,848 | 1.0081 / 1.0078 | 1.0045 / 1.0044 | 23 / 23 | 26 / 27 | .7409 / .9921 \| .7408 / .9918 | .7692 / .9900 \| .7694 / .9898 | 2.4311 / 2.4271 | .1341 / .0602 \| .1341 / .0601 |

  Eigen family per rank (b = sample SD of z, ddof 1; T = 1,089 for ranks 00-47, 948 / 817 / 733 for ranks 48 / 49 / 50;
  band sqrt(2 / T) = .0429 (.0459 / .0495 / .0522); "above" = under-forecast, "below" = over-forecast / over-corrected,
  COV-11; reduction = |b_v1.1 - 1| - |b_cov - 1|, positive = atx-cov-v1 nearer 1):

| rank | b v1.1 | b atx-cov-v1 | v1.1 | atx-cov-v1 | reduction |
|---|---|---|---|---|---|
| 00 | 16074.6219 | 13667.7331 | above | above | +2406.8888 |
| 01 | 4802.5569 | 4565.5452 | above | above | +237.0117 |
| 02 | 1230.4744 | 1079.8917 | above | above | +150.5827 |
| 03 | 1.8387 | 1.6799 | above | above | +0.1588 |
| 04 | 1.6923 | 1.5373 | above | above | +0.1550 |
| 05 | 1.7420 | 1.6582 | above | above | +0.0838 |
| 06 | 1.6976 | 1.5493 | above | above | +0.1483 |
| 07 | 1.6226 | 1.5702 | above | above | +0.0525 |
| 08 | 1.5256 | 1.4314 | above | above | +0.0942 |
| 09 | 1.6104 | 1.4994 | above | above | +0.1110 |
| 10 | 1.5563 | 1.4260 | above | above | +0.1303 |
| 11 | 1.5363 | 1.3884 | above | above | +0.1480 |
| 12 | 1.3838 | 1.3174 | above | above | +0.0664 |
| 13 | 1.3362 | 1.2963 | above | above | +0.0399 |
| 14 | 1.3258 | 1.2274 | above | above | +0.0984 |
| 15 | 1.3030 | 1.2265 | above | above | +0.0765 |
| 16 | 1.3352 | 1.2610 | above | above | +0.0742 |
| 17 | 1.3218 | 1.2462 | above | above | +0.0756 |
| 18 | 1.3773 | 1.2865 | above | above | +0.0908 |
| 19 | 1.3108 | 1.2384 | above | above | +0.0724 |
| 20 | 1.3225 | 1.2681 | above | above | +0.0544 |
| 21 | 1.2136 | 1.1690 | above | above | +0.0446 |
| 22 | 1.2481 | 1.2251 | above | above | +0.0230 |
| 23 | 1.2400 | 1.1790 | above | above | +0.0610 |
| 24 | 1.1761 | 1.1868 | above | above | -0.0107 |
| 25 | 1.1629 | 1.1150 | above | above | +0.0479 |
| 26 | 1.1523 | 1.1217 | above | above | +0.0306 |
| 27 | 1.1432 | 1.1135 | above | above | +0.0297 |
| 28 | 1.1202 | 1.0983 | above | above | +0.0219 |
| 29 | 1.1196 | 1.1035 | above | above | +0.0161 |
| 30 | 1.0858 | 1.0993 | above | above | -0.0136 |
| 31 | 1.0962 | 1.0598 | above | above | +0.0364 |
| 32 | 0.9869 | 0.9337 | in band | below | -0.0532 |
| 33 | 0.8009 | 0.8837 | below | below | +0.0828 |
| 34 | 1.0167 | 1.0515 | in band | above | -0.0349 |
| 35 | 1.0781 | 1.0883 | above | above | -0.0102 |
| 36 | 1.0251 | 1.0255 | in band | in band | -0.0004 |
| 37 | 1.0280 | 1.0220 | in band | in band | +0.0060 |
| 38 | 1.0393 | 1.0761 | in band | above | -0.0368 |
| 39 | 0.9842 | 0.9783 | in band | in band | -0.0060 |
| 40 | 1.0003 | 1.0222 | in band | in band | -0.0220 |
| 41 | 0.9882 | 1.0003 | in band | in band | +0.0115 |
| 42 | 1.0015 | 1.0261 | in band | in band | -0.0245 |
| 43 | 0.9165 | 0.9630 | below | in band | +0.0465 |
| 44 | 0.9863 | 0.9694 | in band | in band | -0.0169 |
| 45 | 0.9437 | 1.0023 | below | in band | +0.0540 |
| 46 | 0.9113 | 0.9402 | below | below | +0.0289 |
| 47 | 0.7836 | 0.8009 | below | below | +0.0172 |
| 48 | 0.7365 | 0.7536 | below | below | +0.0171 |
| 49 | 0.7118 | 0.7521 | below | below | +0.0403 |
| 50 | 0.8135 | 0.8004 | below | below | -0.0131 |

  Ranks in band: v1.1 10 of 51, atx-cov-v1 9 of 51 (equal to each store's own eigen `series_in_band`). Ranks reduced
  (positive): 39 of 51 (12 negative). Pooled eigen b, N 54,770, band .0060: v1.1 2370.9495, atx-cov-v1 2036.9591 (both above);
  without eigen-00, N 53,681, band .0061: v1.1 705.8525, atx-cov-v1 667.9782 (both above). Read in both directions
  (COV-11): below band = over-forecast / over-corrected in v1.1 at ranks 33, 43, 45-50 (8 ranks) and in atx-cov-v1 at
  ranks 32, 33, 46-50 (7 ranks). Fact for the reading, not an interpretation: ranks 00-02 have b above 1,000 in both
  recipes, so every pooled and mean eigen figure above (pooled b, `full b mean`, rolling b mean, QLIKE, MRAD) is
  dominated by them; the COV report's note 1 expected one such near-null rank on the churn panel (eigen-00); TRAIN has
  three. No eigenvector diagnostic exists to name the directions.
- **C5 step 4: HOLDS.** COVR-1 reading above (no `exp2` / `pow` / `exp` on the read path). Release NAV exe p9-2l
  `53ca5dd4...` reading the Debug-built container `p9-m2d-cov-risk-container`: run 30.5 s, peak 1,079 MiB; argv vs the
  P9-B0 receipt: 52 tokens each, exactly the three listed tokens differ (indices 14, 45, 47). `nav_cmp.py`: 28 files on
  both sides, **27 byte-identical**, `summary.json` differs at exactly `/producer/build_type` and
  `/producer/engine_git_sha`: inside the list. No Debug attribution run was needed (none made).
- **C7 store pins (W3-10, PRE-29).** Both stores written by Release `atx-equity-strategy-risk` p9-2o `ccc73cd1...d811`
  (source `c8ddf736`), on role `train-2020-2023-lo3` (`e1c67101...95f4`) and fields `-v10` (`a4a060ae...0809`). Kept,
  never deleted. Every listed payload re-hashed = its `files` pin; no unlisted file.
  - **`build-equity/p9-m2i-dcov-v11`** (atx-risk-v1.1; P9-X in the Y-1 form and the wave-3 P9-B0's re-based Y-1 read
    it, PRE-29): `manifest.json` `397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7`; container
    `model.atxcov` root `b39a1b776639e7d0b88c70e8a43bb9f6c5e6dacb92a336a327234dfc5d312e1d`, file
    `cd48d5749dd7484ca69ae6b0b9df142b73e2beb64c814d3c143982ec59bb2ce3` (510,677,824 bytes).
  - **`build-equity/p9-m2i-dcov-covv1`** (atx-cov-v1; P9-MV and P9-V pin it, W3-10 / PRE-29): `manifest.json`
    `ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3`; container root
    `20919f7b338dfbd786994cbbcb506bdaabd9c3af3266d4394d4d2e07d90c9f8e`, file
    `0f90bf7c78203ca8c93d6003d58c8b2894747045485137756fbc13a32e3b642a` (510,678,016 bytes).
  - Per-payload SHA-256 of both stores (11 payloads each): `m2i-logs/store-pins.txt` (equal across the two stores for
    the recipe-independent payloads: exposures, slots, factor returns, structural flags, specific variance).
  - Not byte-comparable with the Debug-built stores of record (`v8-risk-lo3-v10`, `p9-m2d-cov-risk-container`): COV-7 /
    COVR, expected, not a red (COVR review item 6).
- **Deleted by name after C3 was recorded:** `build-equity/p9-m2i-dcov-v11-r2`, `build-equity/p9-m2i-dcov-covv1-r2`
  (980 MB each) and `build-equity/p9-m2g-cov7/` (163 MB); the `-r2-run` receipt dirs are kept.

### 4 (e) M2j: D-COV-EIG (Ruling COV-13 (b); zero trials)

Step group M2j (root, 2026-10-05). The reading rule is COV-13 (b)'s, fixed by the PM before this diagnostic existed.

**Pre-run list (written and committed before the tool is run on either store).**
- **Stores** (never deleted, never rewritten): `build-equity/p9-m2i-dcov-v11` (atx-risk-v1.1, `manifest.json`
  `39736647...23c7`) and `build-equity/p9-m2i-dcov-covv1` (atx-cov-v1, `ea58691f...f0f3`), the M2i pins
  (`handoff-root-m2i.md`). The tool refuses a store whose `manifest.json` SHA-256 is not its pin, or whose two
  payloads read do not equal their `files` pins (SHA-256 and size).
- **Tool** `m2j-logs/tools/eig_read.py` (read-only; checked before the run only on a synthetic store in the session
  scratchpad). Per store it opens exactly `manifest.json` (factor names `recipe.factors`, `geometry`, `layouts`,
  `model`, `files` pins), `factor_covariance.f64` (dates x 62 x 62 forecast factor blocks; the `RiskDay::covariance`
  every sink receives, `strategy_risk_verb.cpp` `FileSink::on_day`) and `factor_structural.u8` (dates x 62 structural
  flags). It never opens `bias.csv`, `bias_summary.json`, `factor_returns.csv`, `diagnostics.csv` or any other file:
  no return, no realised value, no bias statistic.
- **Eigenportfolios, as the eigen family forms them** (`strategy_risk_model.cpp` `BiasHarness::form_extended`): at
  session t the block is the factors whose forecast variance is finite; it is formed only when every block entry is
  finite and the smallest eigenvalue is > 0; ranks ascend by eigenvalue (eigen-00 = smallest; numpy `eigh`, the same
  symmetric eigenproblem as Eigen's `SelfAdjointEigenSolver`). A block formed at t is scored at t + 1, so the sessions
  read are the formed t with a next session; rank k exists at t when the block has more than k factors. The tool prints
  the per-rank session count; it must equal T_k of 4 (d) (1,089 for ranks 00-47, 948 / 817 / 733 for 48 / 49 / 50),
  else the session set is not the harness's and Part B stops for the PM.
- **Per session, ranks 00, 01, 02:** r = eigenvalue / the median of that session's block eigenvalues; the unit
  eigenvector's squared loadings (sum 1) split over a partition of the block: (i) factors flagged structural at t
  (`factor_structural.u8` = 1); (ii) the industry block (`ind_ff1` .. `ind_ff49`, `ind_residual`) plus the market
  factor (`market`, the model's country factor), not flagged at t; (iii) the style factors (`style_*`), not flagged at
  t. (i) + (ii) is the share outside the unflagged styles, so no structural loading is counted twice.
- **Prints per recipe and rank:** session count; median over sessions of r; median over sessions of share (i), (ii),
  (iii) and of the per-session sum (i) + (ii); for information the medians of the unpartitioned industry + market share
  and style share (structural members included); the five factor names with the largest median-over-sessions squared
  loading (0 in a session where the factor is outside the block). Nothing else.
- **Reading rule (COV-13 (b), applied as written):** a rank is "structurally null" iff its median over sessions of
  share (i) + (ii) is at least .99. A structurally null rank is excluded from one more pooled eigen row, "without
  structurally null ranks", printed beside the two rows of 4 (d) (all ranks; without eigen-00). A rank that fails the
  rule stays in every pooled row and is printed as a finding. Ranks 03-50 are not read and stay in every row.
- **The third pooled row** is computed from the per-rank values already printed in 4 (d) (`m2i-logs/dcov-read.txt`:
  b_k, T_k): N = sum of T_k over the included ranks, band sqrt(2 / N), and the pooled b from the per-rank values as
  sqrt(sum (T_k - 1) b_k^2 / (N - 1)). The 4 (d) rows pooled the eigen z series themselves (`dcov_read.py`: the sample
  SD of the concatenated z); the two agree exactly only when every rank's z mean equals the pooled mean, and the
  per-rank means are not printed (they live in `bias.csv`, which this step does not open). So the same formula is also
  applied to the two existing rows' rank sets and its residual against their printed values is printed beside the new
  row, to show the size of the between-rank term it omits.
- **Stop:** if a store lacks what the rule needs (no structural flags, no factor names, eigenvectors not recoverable
  from the blocks) or the session counts do not equal T_k, the tool's output says exactly what is missing and Part B
  stops for the PM.
- Output: `m2j-logs/dcov-eig.txt`, copied below with the reading.

## Item 5. AL-COMB Q5 zero-trial diagnostic (`scripts/specs/p9/diagnostics/al-comb-q5-style-variance.json`)

**Pre-run list (committed before the runs; root names the cell here, as the spec requires).**
- Cell: the last accepted unlevered cell, P9-H's parent: Y-F0 (`scripts/specs/v8/lib-v8ysb-gm.json`), resolved outputs
  at P9-B0: `$N` = `build-equity/p9-b0-yf0-nav`, `$ROLE` = `build-equity/train-2020-2023-lo3` (pin `e1c67101...95f4`),
  `$FIELDS` = `build-equity/train-2020-2023-lo3-fields-v15` (pin `26fee5ce...3b09`). The spec's nominal X-5 is
  superseded by "the last accepted cell when root runs this"; every P9 cell's parent is Y-F0 (plan section 5.2,
  integration-log M1d P9-B0).
- Step 0 (holdings): `$N` ran without `--emit-holdings`, so its exact argv (`p9-b0-yf0-nav-run/receipt.json`) is
  re-run with `--emit-holdings build-equity/p9-b0-yf0-nav-h --output build-equity/p9-b0-yf0-nav-hrun` (Release
  targets exe, bounded `--seconds 600 --max-rss-mib 2048`; not ledgered). Expected vs `$N`: every daily / events CSV,
  `recipe.json`, `v7_extras.json`, `capacity/*`, `capacity_curve.csv`, `v7_transfer_coefficient.csv` byte-identical;
  `summary.json` only `/producer/build_type`, `/producer/engine_git_sha` (as step 4). Anything else stops item 5.
- Step 1 `book-csv`, step 2 `risk --book-weights ... --emit-exposures all` (atx-risk-v1.1, Release risk exe, output
  `build-equity/p9-q5-risk`), step 3 `book_diagnostics.py run --only G-2a` (output
  `build-equity/p9-q5-diagnostics/diagnostics-v8-g2a.json`), each through the bounded runner (`--max-rss-mib 2048`;
  the spec's 2560 cap for G-2a is replaced by RUN-1's 2048 unless admission refuses), argv as the spec gives it.
- Added after COV-7's result, before any Q5 run: the Release risk verb is not bit-identical to the Debug one (4 (b)),
  so steps 2 and 3 are repeated once with the Debug risk exe p9-2k (`build-equity/p9-q5-risk-dbg`,
  `build-equity/p9-q5-diagnostics-dbg/diagnostics-v8-g2a.json`) as a cross-check; only q, q_lit, the five column
  shares and the session counts are compared (absolute differences printed). The Release figures are the ones of
  record (M2c-RED-2 (e)); this is atx-risk-v1.1, which COV-7 does not gate.
- Reads (tool `m2g-logs/tools/q5_read.py`): from the G-2a JSON only `complete_sessions`, `incomplete_sessions` and
  `style_share_of_summed_variance` for the columns size, residual_vol, liquidity (q) and beta, leverage (q_lit).
- Prints: q (sum over size, residual_vol, liquidity) with each column's share beside it; q_lit (adds beta and
  leverage); complete / incomplete sessions; the spec's arithmetic bound 1 / sqrt(1 - q) and 1 / sqrt(1 - q_lit).
  ALCOMB-9 (b): the ex-ante vol RATIO needs G-2a on two books (cell and parent); Q5 reads one book, so it does not
  produce the ratio: printed as "not produced".
- May NOT print: `mean_ex_ante_vol_annual`, `mean_share`, `share_of_summed_variance`, `by_year`, `mean_gross`,
  `mean_uncovered_gross`, any bias statistic of the risk step's `book` family (its `bias.csv` / `bias_summary.json`
  hold realised book returns: not opened), any NAV / return / IC / Sharpe / turnover figure. The runner's stdout logs
  of steps 2 and 3 are not opened (only receipts: outcome, exit code, wall, peak).

**Result** (evidence `m2g-logs/q5.txt`, `m2g-logs/q5-holdings-identity.txt`, `m2g-logs/q5-risk-dbg-vs-rel-sha.txt`,
receipts `m2g-logs/receipt-p9-*q5*.json`, `receipt-p9-b0-yf0-nav-hrun.json`; every run completed, exit 0, within
2,048 MiB).
- Step 0: `p9-b0-yf0-nav-hrun` (Release targets p9-2l `53ca5dd4`, 27.4 s, peak 583 MiB): **inside the list**: 26 of 27
  files byte-identical to `p9-b0-yf0-nav`; `summary.json` differs only at `/producer/build_type` and
  `/producer/engine_git_sha`. Holdings `build-equity/p9-b0-yf0-nav-h` (holdings.f64 162 MB). (Also the G-P3 NAV-half
  evidence for Y-F0: Release NAV = Debug outside `producer`.)
- Step 1 `book-csv` (4.0 s, 396 MiB): `build-equity/p9-q5-book.csv` sha256 `8f756fa2...c6c0`, 1,927,595 rows, 1,005
  sessions. Step 2 `risk --book-weights` Release (`a31a0f96`, 20.7 s, 615 MiB) -> `build-equity/p9-q5-risk`; Debug
  cross-check (`94b5a5e7`, 49.2 s, 642 MiB) -> `build-equity/p9-q5-risk-dbg`. Step 3 G-2a (6.1 s, 356 / 364 MiB).
- **q = +0.0502** (size +0.0334, residual_vol +0.0275, liquidity -0.0107); **q_lit = +0.0671** (adds beta +0.0091,
  leverage +0.0078). Sessions complete 1,005, incomplete 0. Spec arithmetic bound on the SR factor: 1 / sqrt(1 - q) =
  1.0261; 1 / sqrt(1 - q_lit) = 1.0353. Against the spec's reading line ("q well below .05 argues against
  registering P9-H"; the lit's +.05 to +.15 needs q of about .05 to .15 removed): q is at the .05 edge, q_lit below
  .10. Decides nothing; the PM rules P9-H with these printed.
- Ex-ante vol ratio (ALCOMB-9 (b)): **not produced** (Q5 reads one book): PRE cuts it unless G-2a is registered on
  both books.
- Debug cross-check: every printed share identical (G-2a rounds to 10 significant digits). Note for COV-7: the two
  TRAIN atx-risk-v1.1 stores themselves differ (factor covariance 99% of finite cells, median 52 ULP, max rel 1.1e-8;
  specific variance 98%, median 13 ULP, max rel 3.6e-13; NaN patterns equal; exposures, slots, factor returns
  identical); the Debug store's `factor_covariance.f64` / `specific_variance.f64` equal the store of record
  `v8-risk-lo3-v10`'s (`9d9c62a2...`, `a7792a53...`, the M2d comparison), the Release store's do not. So COV-7's miss
  is the same on real data, at a size that does not reach G-2a's printed digits.

## Item 6. S2R-DIAG (S2R-2)

**Pre-run list (committed before the run).**
- Cost check: AuditExact ran at 1.18x ResearchFast in v8 H-2 (platform-v8 progress, integration 8); the cold Release
  ResearchFast u is 94-164 s (M2c), so a cold Release AuditExact u is expected at about 2-4 min (< 15 min).
- Path check (PATH-1 / S2-4): a fresh cache dir `build-equity/s2rd` (`C:\atx-wt\pool-2\build-equity\s2rd`, 34
  chars, <= 51); root + Release audit identity dir `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0_audit\` = 81 chars; the
  longest relative entry path in the lo3 cache is 147 chars; longest expected path 228 < 250. No existing cache is
  renamed or written.
- Run (Release IC exe `build-equity-rel/bin/atx-equity-strategy-ic.exe`, p9-2j `fccaa07e`; bounded `--seconds 1200
  --max-rss-mib 2048 --min-free-mib 512 --build-type Release --admission-wait-seconds 3600`): the P9-B0 u argv
  (`build-equity/p9-b0-yf0-u-run/receipt.json`) with `--output build-equity/p9-m2g-s2rd-u`, `--candidate-cache
  build-equity/s2rd` and `--eval-mode audit-exact` appended. Everything else verbatim (library `41010b0b...`, role
  `e1c67101...`, fields v15 `26fee5ce...`, `--no-composition`, `--workers 4`, `--max-memory-mib 2560`).
- Reads (tool `m2g-logs/tools/s2r_diag.py`): only the `smax` and `smax5` signal payloads and sidecars of the two
  caches: ResearchFast = the P9-B0 u's entries in `build-equity/mega-candidate-cache-v8-lo3` (Debug VM identity, the
  entries `p9-b0-yf0-u` hit); AuditExact = the new `_audit` entries in `build-equity/s2rd`; each payload checked
  against its sidecar SHA-256; both sidecars must agree on role, geometry, DSL SHA-256 and id.
- Prints per candidate: the count of (name, day) cells NaN under AuditExact and finite under ResearchFast, over all
  role sessions and over the score window; context: finite AE & NaN RF, both NaN, both finite (counts only).
- May NOT print: any signal value, and nothing of the u pass's own outputs (`orientations.json`,
  `train_daily_ic.csv`, `summary.json`, `train_candidates.jsonl` hold IC statistics: not opened); no return, IC,
  Sharpe or NAV statistic. The runner's stdout log is not opened (receipt only).

**Result** (evidence `m2g-logs/s2r-diag.txt`, `m2g-logs/receipt-p9-m2g-s2rd-u.json`). One deviation from the list, on
a limit only: the runner caps `--seconds` at 600 (its refusal: "require a command, <=600 seconds"), so the run used
`--seconds 600` instead of 1200 (nothing ran under the refused argv). Run `p9-m2g-s2rd-u`: completed, exit 0,
**94.6 s**, peak 1,617 MiB (inside 2,048), admission wait 0 s; exe `fccaa07e`. Longest path written 210 chars.

| candidate | cells (all sessions / score window) | NaN AuditExact & finite ResearchFast | finite AE & NaN RF | both NaN | both finite |
|---|---|---|---|---|---|
| `smax` | 8,320,410 / 5,957,532 | **0 / 0** | 0 / 0 | 6,328,780 / 4,188,740 | 1,991,630 / 1,768,792 |
| `smax5` | 8,320,410 / 5,957,532 | **0 / 0** | 0 / 0 | 5,830,001 / 4,100,337 | 2,490,409 / 1,857,195 |

- On the P9-B0 u the two candidates' NaN patterns are identical under AuditExact and ResearchFast: the S2R-2
  flat-window case (AuditExact NaN, ResearchFast 0) does not occur on TRAIN for `smax` / `smax5`. The prereg line
  stays as ruled (a property of the mode); its measured size on the P9-B0 u is 0 cells. Gates nothing.
- The `_audit` cache (4.2 GB) and the u output dir were deleted by name after this commit (regenerable in 95 s);
  the run receipt is kept.

## Notes for the re-run of item 4 (c)

- `run_bounded_research.py` refuses `--seconds` above 600, so the D-COV list's `--seconds 1800` becomes 600 (the COV
  report's step 5 already treats a wall above 600 s as a stop for its own ruling). Lists otherwise unchanged.
- The Release risk exe p9-2l (`a31a0f96`) and Debug p9-2k (`94b5a5e7`) are on disk; the COV-7 fixture copy and the
  four synthetic outputs are kept in `build-equity/p9-m2g-cov7/` for the ruling.

## End state

Trial ledger at the end: 133 lines, `27e40f9f6314585e...` (unchanged; no step passed a ledger). UCRT
`ucrtbase.dll` 10.0.26100.7623, SHA-256 `2c08e77f...65ef` (unchanged since M2f).

M2i (item 4 (d)) end state: trial ledger 133 lines, `27e40f9f6314585e...` at start and end (no run passed a ledger;
zero trials). UCRT `ucrtbase.dll` 10.0.26100.7623, `2c08e77f...65ef`, re-read after the runs: unchanged. Release risk
exe p9-2o `ccc73cd1...d811` unchanged across the four COV runs.
