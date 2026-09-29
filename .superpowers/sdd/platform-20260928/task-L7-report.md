# Task L7 / L7b report: library v7.0 (v6.1 + wave 1) and spec v70, no runs
**Commits:** pool-10, branch feat/platform-v7-l7-libv70-20260928: `e1915059` (the L7 work) + `36f92885` (merge of a5e4a10d, no conflicts, since the merged side touches no L7 file). The tree is clean. No C++ was built. No IC, NAV or fit was run. No validation or 2023+ statistic was read.
**Review of the stopped lane:** the 11 files were complete and matched the brief and the corrections (no fields v8; sign and max via POLICY_OPS; q5_eg reads 6 fields; qmj_safety has 8 slots; RoF slopes). I added three things. (1) Provenance now cites "Ruling 7.0-b" and "Correction", which moves the recipe sha from c0b52fc6 to 60b82300. The library bytes are unchanged at e7bae75c. (2) The summ grid (see below). (3) Tests for both.
**DSL as implemented** (draft f43e54d9 verbatim; the only respelling is the q5 slopes; W2 spellings checked against registry.cpp by the generator):
- qmj_safety (low_risk, B): `(((rank(decay_linear((-1 * ts_beta_on(R, mkt_ret, 252)), 21)) + rank(decay_linear(((-1 * (debt / at)) + (0 * log(at))), 21))) + rank(decay_linear((-1 * stddev(((ni_q / be_lag1q) + (0 * log(be_lag1q))), 252)), 21))) / 3)`
- nincr (earnings_momentum, C+): `rank(decay_linear((I * (1 + (delay(I, 63) * (1 + (delay(I, 126) * (1 + (delay(I, 189) * (1 + delay(I, 252))))))))), 21))`, I = `max(sign((ni_q - ni_q_lag4)), 0)`
- q5_eg (investment_issuance, B): `group_rank(decay_linear(((((-0.029 * log((me_company / at))) + (0.516 * (cfo_ttm / at))) + (0.771 * ((ni_q / be_lag1q) - delay((ni_q / be_lag1q), 252)))) + (0 * log(be_lag1q))), 21), grp_ff12)`
- smax5 (low_risk, B+): `rank(decay_linear((-1 * ((ts_topk_mean(R, 21, 5) / stddev(R, 21)) + (0 * log(stddev(R, 21))))), 21))`
- res_mom_ind (price_momentum, B): `rank(decay_linear(delay((ts_mean_mp((R - group_mean(R, grp_ff49)), 231, 116) / ts_std_mp((R - group_mean(R, grp_ff49)), 231, 116)), 21), 21))`
- R is `((close / delay(close, 1)) - 1)`, written out in full in the library.
- Static figures: prior bars 272/272/272/41/272; peak slots 8/7/6/6/5; extra fields 5/2/6/0/1.
- **Slopes:** HMXZ RoF 2021 Table I Panel D, tau 1, 1963-2018: -0.029 (t -5.63), 0.516 (t 12.75), 0.771 (t 7.62). These are the values used. The draft's source, NBER w24709 Table 1 Panel D, gives -0.031 / 0.530 / 0.802. Both sets are recorded in `recipe.literature.q5_eg_slopes`.
**Files:**
- atx-impl/strategies: generate_fund_ic_v70.py, fund_industry_ic_v70.json (e7bae75c…) and .recipe.json (60b82300…), test_generate_fund_ic_v70.py, check_fund_ic_v6.py and its test_check_fund_ic_v6_ops.py.
- atx-impl/tools: fit_composition_weights.py (adds ownership_flow) and its test.
- scripts: research_cycle.py (gate.require any; summ.cells; summ.ledger), tests/test_research_cycle.py, specs/v70.json.
**Spec v70:**
- fields: lo1-fields-v7 as-is. The phase shows done; the check expects 41/41 identical fields and none added. Static check uses roster 56.
- u pass: runs on mega-candidate-cache-v70 with `--cache-legacy-fields` fields-v7. That flag is redundant because fields-v7 is the pinned manifest, but it is harmless.
- fit ew-theme-v1 / v4-prior-v1, then card.
- gate p1-v70: prints the 5 wave-1 rows and 7 report rows, and stops only if none is admitted with its prior sign.
- w, then nav with aim-partial-v5 at L 1.247 into the declared output name. monitor is as in v61-ops.
- **summ deviation:** a single-dir `--pbo` cannot run (nav_summ needs at least 4 cells, and `--effective-n dirs` needs the grid). So `summ.cells` holds the 33 dirs of the n33 scoring (mega-nav-v7-summ-n33.json argv). The v7.0 cell is appended as a 34th, all in one positional block. Flags: `--dsr-n 34 --effective-n dirs --psr --pbo --ledger build-equity/trials.jsonl --ledger-kind construction`. Bare `--pbo` covers all 34 cells, as in n29. nav_summ's own argparse was checked on this argv: 34 dirs, pbo [], ledger set.
**Tests:**
- Pre- and post-merge `pytest scripts/tests atx-impl/tools atx-impl/strategies`: 327 passed, 2 skipped (the live-root tests).
- With RESEARCH_CYCLE_LIVE_ROOT set to the overlay below: 169/169 in the touched files (research_cycle 34, generator 22, checker ops 18, fitter 95).
**Plan dry-run (no writes to pool-2):** pool-2 lacks the v70 library until the merge, so `--root C:/atx-wt/pool-2` stops on a pin. Instead I used an overlay root of junctions in my scratchpad (build-equity from pool-2; atx-impl, atx-engine, scripts and .superpowers from pool-10). From pool-10: `"$PY" scripts/research_cycle.py plan scripts/specs/v70.json --root <overlay>` exits 0. All 9 pins show "locked, verified", fields is done, and u's `--train-fields-sha256` resolves to 1d1fa87a. The only placeholders are this cycle's 5 own outputs (orientations, summary, admission, weights, combined). None of the 33 cells is missing in pool-2, and no v70 output exists there yet.
**Root commands** (pool-2 root, PY="C:/Program Files/Python312/python.exe"):
1. `git merge feat/platform-v7-l7-libv70-20260928` (at 36f92885).
2. `cp -al build-equity/mega-candidate-cache-v7w2 build-equity/mega-candidate-cache-v70`. This is a hard-link seed. I checked v7w2 metadata only: 39 sidecars whose dsl_sha256 equals the 39 v6.1 DSLs, fields manifest 1d1fa87a, library db35c276, vm dslvm1_clang18.1.
3. Optional check: `build-equity/bin/atx-equity-strategy-ic.exe --library atx-impl/strategies/fund_industry_ic_v70.json --library-sha256 e7bae75c9dc3c4ac6b826289f39a50e26dc9c41ca80df4b272395a3e7362c162 --train build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --train-sha256 3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809 --train-fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7 --train-fields-sha256 1d1fa87a00d519bcf08fbec83f3fd17029e23650a98a9af26e99ddb1f1a73ee1 --max-memory-mib 1536 --candidate-cache build-equity/mega-candidate-cache-v70 --cache-report`. Expect 39 hits and 5 misses.
4. `"$PY" scripts/research_cycle.py plan specs/v70.json`, then `"$PY" scripts/research_cycle.py run specs/v70.json`. The ledger path comes from the spec. The tree must be clean.
5. For P5 JSON: rerun the plan's summ line with `--json build-equity/mega-nav-v70-summ-n34.json --pbo-json build-equity/mega-nav-v70-pbo-n34.json --ledger-n build-equity/trials.jsonl`. The ledger is keyed on (kind, series sha), so this adds 0 lines. These flags are not in the spec because a `--suffix` rerun would overwrite the files.
**Expected u-pass memory:** fields-v7 is unchanged, so resident field bytes equal the v6.1 pass. The 5 evaluations peak at qmj_safety's 8 slots, about +52 MB for the worker running it. Estimate: about 0.9 GiB (the cached L1 pass was 856 MiB with 38 hits), and at most about 1.11 GiB (1,061 MiB uncached v6.1 + 52). The cap is 1,536 MiB.
**Untested:**
- Native parse and VM of the 5 DSLs on the W2 exe (recipe qualification is "pending").
- Real cache hit counts.
- The u-pass RSS.
- nav_summ over the 34 real dirs (only the argv was parsed).
- The root may prefer an explicit PBO grid, as in n33's {v6.1, C1-C3, spo-v1} plus v7.0, over the 34-cell grid.
