# Task F3: risk model robustness (R2 finding I-2) -- atx-risk-v1.1

**Pool:** C:/atx-wt/pool-7. `git status` clean, then `git checkout -B feat/platform-v7-f3-riskrobust-20260929 c8503bb3`.
**Rules (binding):** never build C++ (root builds; write code that compiles under clang-cl 18 /W4 /WX, C++20); never run
real data; never spawn subagents; never touch C:/atx; never read validation / 2023+ statistics; no pushes. Read
.agents/cpp/agent.md before writing C++ (house style, safety rules). Trailer
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Write files with the Write tool (heredoc apostrophes break).
**Report location (changed from earlier lanes):** write task-F3-report.md into YOUR pool at
C:/atx-wt/pool-7/.superpowers/sdd/platform-20260928/ and commit it with `git add -f`. Do NOT write into C:/atx-wt/pool-2
(a real-data run is in progress there and any write voids it).

**Read first:** C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/task-R2-review.md (findings I-2, M-1, M-4 and the
chain with line numbers), task-L4-report.md, task-F1-report.md, task-F2-report.md; code
atx-impl/src/strategy_risk_model.{hpp,cpp}, atx-impl/tests/strategy_risk_model_test.cpp.

**Defect (confirmed on the pinned TRAIN risk model, no returns involved):** one asset_growth outlier (one instrument, from
2020-05-12) sits inside the SD used by `standardize`, so it clips to z 3 while every other name's z collapses (SD .63 ->
.047); the near-degenerate style column is kept by the cross-section regression; the structural ln-sigma fit extrapolates
exp(3 f) to a name with < 252 sessions of history (specific variance 1.1e-3 -> 3.9e12 per day); the cap-weighted
size-decile shrinkage target spreads it to 176 names. The style is a one-name dummy on 259 dates; profitability on 19.

**Work -- every parameter from robust-statistics convention or the cited risk-model literature, none from returns:**
1. `standardize`: robust location and scale (e.g. winsorise on median +- k x 1.4826 MAD first, then cap-weighted mean and
   equal-weighted SD of the winsorised values, as in the Barra USE4 methodology notes); an outlier must not change the
   other names' z by more than a stated bound (test). State k and the source.
2. Style validity per date: a style column whose dispersion is degenerate (define it: e.g. effective number of names
   with |z| > .1 below a floor, or post-winsorisation SD below a floor) is NOT regressed that date; its factor return is
   missing for the date and the existing F2 structural-forecast path covers the gap. Counted in the manifest.
3. Structural ln-sigma (specific vol for short-history names): no extrapolation outside the fit range -- clamp each
   exposure to the fitted cross-section's range and bound the predicted sigma to the [p1, p99] of the fitted names'
   sigma that date. Bayesian shrinkage target per size decile: robust (cap-weighted median or winsorised mean).
4. Hard invariant: daily specific variance in (0, 1.0) for every name and date or the verb REFUSES with the offending
   instrument / date (no silent clamp). Diagnostics in the manifest and bias_summary.json: max daily D, min style
   dispersion, count of style-dates dropped, count of names at the structural bounds.
5. Model id `atx-risk-v1.1` in the manifest (the v1 pins 897ffdf2 / 17f9328f stay valid as data directories; no legacy
   switch is needed). Bias harness unchanged in meaning.
6. Tests (gtest, synthetic): one extreme outlier in a style (others' z unchanged within the bound); a degenerate column is
   dropped and counted; structural prediction bounded; decile target robust to one corrupt name; the invariant refuses;
   the existing Risk* tests stay green or are updated with the reason stated.

**Root acceptance:** build, Risk* tests, then the risk verb on lo1 TRAIN with `--emit-exposures all`: max daily D < 1.0
(expect ~1e-2 at most), 0 refusals, bias factor family b in [.9, 1.1], random family ok, wall <= 40 s, RSS <= 600 MiB.

**Report** (<= 40 lines): the rules with parameters and sources, files, tests, exact root command lines (build targets,
test filter, risk verb), what changes in the F2 numbers and why, anything untested. Reply to the parent in < 15 lines
with the commit sha.
