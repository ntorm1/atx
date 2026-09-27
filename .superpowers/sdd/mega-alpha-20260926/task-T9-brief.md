# Task T9 — TRAIN-only composition weight fitter (Python tool) -> pinned weights JSON

Owner worktree: `C:/atx-wt/pool-3`, new branch `feat/mega-alpha-weights-20260926` from root HEAD
(root gives the SHA). New files only: `atx-impl/tools/fit_composition_weights.py` and a synthetic test
`atx-impl/tools/test_fit_composition_weights.py`.

## Why
The IC runner (`atx-impl/src/strategy_ic_runner.cpp`) now caches each candidate's raw VM signal
(`--candidate-cache DIR` -> `DIR/<role-manifest-sha256>/<candidate-id>.f64` date-major f64 + sidecar
`.json`, schema `atx.dsl-candidate-signal/v1`) and accepts pinned per-candidate weights
(`--composition-weights PATH --composition-weights-sha256 SHA`, schema `atx.dsl-composition-weights/v1`,
`library_sha256`, object id -> finite weight >= 0; signs still come from TRAIN orientation). Read the
exact weights schema/validation in `strategy_ic_runner.cpp` (and T1 report
`C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T1-report.md`). A root TRAIN-only study
(`C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/studies/compose_study.py`, read it) chose the
preregistered rule below by a 2020-21 fit / 2022 holdout comparison inside TRAIN.

## Rule `mv-shrink-0.9-nonneg-v1` (exact)
1. Inputs: library JSON (+sha), TRAIN role dir (+ manifest sha), candidate cache dir, the TRAIN
   orientations artifact produced by the runner (`orientations.json` + sha; use its per-candidate
   `sign`; sign 0 -> weight 0), optional extra fields not needed.
2. Per scored decision d in the role's scored window [score_begin, score_end-2): exposures at d from the
   role payload exactly as the C++ module `atx-impl/src/strategy_price_exposures.cpp` defines them
   (beta 252 vs equal-weight market of valid guarded returns, >=126 pairs; vol 63 sample SD, >= 32
   pairs; log mean dollar ADV 63, absent days 0; z-score over used rows, clip +-5; used rows = member &
   all exposures finite). Mirror that CPP (read it) — same guard (|log adj| > 1.5 or > |log raw| + .10).
3. Candidate factor return f_k(d): q = centered tied rank of sign_k * signal_k[d] over used rows with a
   finite signal (others 0), residualize q on [1, z_beta, z_vol, z_ladv] over used rows, scale to
   sum|q| = 1, f_k(d) = sum_i q_i * r_i(d+2) where r(d+2) = close[d+2]/close[d+1]-1 (both present,
   finite, positive, unguarded; else 0 contribution).
4. mu = mean_d f_k, S = sample covariance over all TRAIN decisions; Sh = 0.1*S + 0.9*diag(S);
   w = solve(Sh, mu); w = max(w, 0); w /= sum(w). If all zero -> error.
5. Output `atx.dsl-composition-weights/v1` JSON with every library candidate id (weight 0 allowed),
   `library_sha256`, plus provenance: rule id, lambda, role manifest sha, orientations sha, cache
   sidecar payload sha per candidate, script sha, window, per-candidate mean/sd/Sharpe of f_k. Canonical
   deterministic bytes. Exclusive output (refuse overwrite). Also write a small CSV of the f_k series?
   No — keep only summary stats (size).
6. Performance: stream one candidate at a time (52 MB each for TRAIN; 96-200 candidates); precompute
   per-date exposure projections once; must run < 150 s for 100 candidates on ~1155x5627. Numpy only.
   Never read validation or 2025+ data (assert role name/dates <= 2022-12-31).

## Constraints
- Not TDD: implement, then a tiny synthetic test (fake role + fake cache + fake orientations) checking
  exact weights vs a hand-computed small case, sign handling, nonneg clip, exclusive output, schema.
  You may run that synthetic test. Do NOT run on real data (root does).
- No subagents. Commit in pool-3 (messages end with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`). No push.

## Report
`C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T9-report.md` incl. the exact root command
line. Return only: status, commit SHA, one-line summary, concerns.

## Addendum 2026-09-27 (owner turnover ruling; handoff §2a)
The shipped book is rebalanced daily. Individual alphas may turn over up to 70% of GMV/day standalone,
on the assumption that combining nets trades; the combined book must stay <= 20%/day mean, <= 30%/day
p95. T9 supplies the per-alpha side of that check (TRAIN only, same data it already streams):
- per candidate k: standalone daily turnover `tau_k = mean_d sum_i |q_k(d)_i - q_k(d-1)_i|` over TRAIN
  scored decisions, with q_k from step 3 (neutralized, sum|q| = 1; ignore price drift). Write it to the
  per-candidate provenance next to mean/sd/Sharpe, and flag `tau_k > 0.70` (flag only; admission is
  root's decision);
- blend level: `weighted_standalone_turnover = sum_k w_k tau_k` with the final normalized weights. Root
  divides the NAV replay's daily-full book turnover by this to get the netting ratio.
Fixture: tau_k hand-checked on the synthetic case.
