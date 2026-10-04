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
