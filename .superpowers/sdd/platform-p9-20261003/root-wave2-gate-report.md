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

## Item 4. COV: S9, COV-7, D-COV, COV report steps 4 and 5

### 4 (a) S9 (COV-9)

**Pre-run list.** Reads the role manifest `build-equity/train-2020-2023-lo3/manifest.json` (pin `e1c67101...95f4`):
`universe.delisting.returns_applied` and the per-event `returns_applied` flags. Prints the boolean, the key list and the
count of events with `returns_applied` true. No value of any event is printed. Before every COV run below.

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

### 4 (c) D-COV (cov-design section 5.3, COV report "What the churn-panel small-rank bias means", COV-5, COV-11)

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

## Item 6. S2R-DIAG (S2R-2)

Pre-run list or skip reason: written below before any run.
