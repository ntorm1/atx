# Markov fit budget, Levenberg-Marquardt search and sparse interval seed

Lane `feat/vol-markov-fit-budget-20261016`, frozen base `da3a4179`, pool-1,
RunId `vol-20261016-mkfit`. Implementation first, tests after. Everything is
opt-in through explicit policy keys; defaults are byte-identical (eight-case
parity probe and the full dev200 R104 run). No vendor label was read outside
the frozen scorer and the unmodified diagnosis scripts, which ran only after
every run receipt existed.

## Current optimizer (characterisation)

- Algorithm: derivative-free compass search in log sigma, no gradients. A sweep
  tries `log sigma_i -/+ step` per parameter (clamped), one full evaluation per
  trial, accepting any strict improvement at once; the step (0.25) halves only
  after a non-improving sweep, so `StepTolerance` (step < 1e-4) needs >= 12 of them.
- Objective: weighted hinge distance outside `[bid, ask]` in half-spread units,
  optional midpoint tie, log-sigma prior and roughness.
- Limits: `max_evaluations` (R104 ordinary 256, sparse 128, dense 512; cap 8192)
  counts every trial; `max_sweeps` 32/24/48. A sweep costs 2P evaluations: 3
  knots fit 32 sweeps in 192, so R104 never hit its limit (60 step tolerance,
  31 sweep limit); 5 knots exhaust 256 after 25 sweeps (X2: 84/91 at the limit).
- Work: each evaluation compiles the whole model under the compiler caps and
  prices every quote against `max_rollback_work`; nothing is charged across
  evaluations.
- Cost (R104 report, unqualified, shared host): 17.7 ms mean per evaluation
  (328.1 s / 18,542 evaluations); ordinary median 7.7 ms at 240 quotes (32.5 us
  per quote-evaluation). Probe (300 quotes, 129 states, 158 transitions): 0.31
  ms compile vs 20.0 ms pricing, so compilation is 1.5% of an evaluation;
  reusing compiled factors cannot pay, repricing is the cost to avoid.

## Design (all opt-in; defaults unchanged)

**Scaled budget.** `AmericanMarkovFitConfig::evaluations_per_parameter` (0 =
off) and `max_scaled_evaluations` (explicit hard cap). The effective budget is
`clamp(per_parameter * P, max_evaluations, max_scaled_evaluations)`, computed
without overflow (`american_markov_fit_evaluation_budget`). Scaling without a
cap, a cap without scaling, a cap below `max_evaluations` or above 8192 are all
refused by the fitter and by the policy catalog through one shared validator
(`src/fitting/american_markov_fit_controls.hpp`). The fit report carries
`evaluation_budget`, `evaluations`, `termination`, and the native report adds
`fit_termination_counts` by profile.

**Levenberg-Marquardt search** (`search = LevenbergMarquardt`). Damped
Gauss-Newton on the exact objective's residual vector in log sigma: interval
hinge rows are active only outside `[bid, ask]`, midpoint rows only with a bid,
and prior/roughness rows are linear, so `J'J`/`J'r` reuse the objective's own
formulas. The price Jacobian is a forward difference (`jacobian_log_step`
1e-3, mirrored at `max_sigma`): P evaluations per iteration, and an iteration
starts only if the remaining budget covers P + 1. Marquardt scaling by
`diag(J'J)`, Nielsen damping update, Cholesky solve (P <= 128), box projection,
strict-decrease acceptance. Terminations are explicit: `StepTolerance`
(projected step below `min_log_step`), `ObjectiveTolerance`,
`GradientTolerance`, `DampingLimit`, `EvaluationLimit`, `SweepLimit`
(`max_sweeps` = Jacobian iterations). Jacobian probes are fully evaluated
models, so one that beats the stored best replaces it (best-iterate contract
unchanged) without moving the iterate.

**Exact reuse and honest work.** Compilation and rollback of an expiry prefix
read only prefix intervals, so a parameter whose time group first appears in
interval k leaves every earlier expiry's price bit-identical. Jacobian columns
of PerVarianceInterval/VarianceTimeKnots parameters reprice only the
expiry-major suffix (paused rows that read the group count as affected). The
probe confirms the premise: after perturbing only the last interval, 108 of 158
transitions are bit-equal, and a public-API test shows unchanged earlier-expiry
batch bits under explicit and resolvent methods with dated cash between.
Every evaluation still compiles under all compiler caps; only the quotes it
prices are checked against `max_rollback_work` and charged. New diagnostics:
`rollback_work` (all admitted work over the fit), `priced_quotes`,
`reused_quote_prices`, `jacobian_evaluations`, `rejected_steps`,
`final_damping`, `gradient_norm`.

**Sparse interval seed** (`AmericanMarkovSeedEstimator::SigmaInterval`). Every
near-forward quote (bidless and wide included) bounds its flat-to-expiry
equivalent sigma: ask -> upper bound, bid above the intrinsic floor -> lower
bound, inverted with the existing Andersen-Lake convention (1 or 2 calls, never
the midpoint). An IV on the uninformative side (ask above `max_sigma`, bid
below `min_sigma` or on the IV floor) is no bound; one on the conflicting side
refuses the quote. The feasible interval is `[max lower, min upper]`, a missing
side closed by the layout bound; the seed is its geometric midpoint (Shared:
over all used quotes; PerVarianceInterval: endpoint midpoints differenced).
Refusal only when unbounded both ways (`MissingEndpoint`) or a non-positive
forward difference. Crossed intervals are reported (`crossed`, negative
`log_width`), never repaired. Provenance: per-quote bound flags, per-expiry,
per-endpoint and global intervals, `interval_quotes` in the native report.
The native policy spelling `bid-mid-ask-then-sigma-interval` keeps every
bid-mid-ask seed and runs the interval estimator only after an incomplete one,
inside the same configured inversion/slot caps (it receives the remainder);
the state is `applied_interval_fallback`, totals and primary calls are both
reported.

**Native policy keys** (`fit`): `search` (`coordinate-pattern`,
`levenberg-marquardt`), `evaluations_per_parameter`, `max_scaled_evaluations`,
`objective_tolerance`, `gradient_tolerance`, `jacobian_log_step`,
`initial_damping`, `max_damping`; (`initialization.seed`): `estimator`
(`bid-mid-ask`, `sigma-interval`, `bid-mid-ask-then-sigma-interval`). They are
expanded into the policy definition only when any differs from its default,
so legacy configuration hashes keep their bytes.

**Seed as prior centre** (`initialization.prior`: `configured` | `seed`, commit
`fc7114b1`, added after the m1 run; see the addendum below). With `seed`, every
applied quote seed (any estimator, any profile) also becomes the log-sigma
prior centre (`prior_sigmas`), not only the starting vector; refused or
incomplete seeds leave the configured prior. The decision records
`effective_prior_source: quote_seed`, and the definition gains `prior` and a
`prior_application` text only when opted in. It needs `mode: quote-seed`;
any other spelling is refused before execution.

## Gates (pasted)

Final source `fc7114b1` on base `da3a4179`; preset `vol-rel` (Release, PCH on)
unless stated; target-scoped builds of
`atx-vol-american-markov-{fit,seed,policy,market}-tests` and
`atx-vol-quote-bench` only (`atx-vol-tests` was never built).

```
[atx-build] ctest --test-dir C:\atx-wt\pool-1\build-vol-rel --output-on-failure -j 1
  -R ^(atx-vol-american-markov-(fit|seed|policy|market)|atx-vol-quote-benchmark-native)$
1/5 Test #1120: atx-vol-american-markov-fit ......   Passed    5.28 sec
2/5 Test #1121: atx-vol-american-markov-seed .....   Passed    0.05 sec
3/5 Test #1122: atx-vol-american-markov-market ...   Passed    0.03 sec
4/5 Test #1123: atx-vol-american-markov-policy ...   Passed    0.03 sec
5/5 Test #1135: atx-vol-quote-benchmark-native ...   Passed   10.34 sec
100% tests passed, 0 tests failed out of 5
```

| Suite (JUnit receipt) | Executed / passed | New | Failures, skips |
| --- | ---: | ---: | ---: |
| `atx-vol-american-markov-fit` (`AmericanMarkovFit` 24, `TimeBasis` 4, `FitSearch` 7) | 35/35 | 7 | 0 |
| `atx-vol-american-markov-seed` (`Seeds` 15, `SeedInterval` 7) | 22/22 | 7 | 0 |
| `atx-vol-american-markov-policy` | 17/17 | 1 | 0 |
| `atx-vol-american-markov-market` | 20/20 | 0 | 0 |
| `atx-vol-quote-benchmark-native` (pytest, at `fc7114b1`) | 139/139 | 17 | 0 |

The gtest receipts were produced at `9d780ddb`; `fc7114b1` touched only
`tools/quote_bench_common_seed.{hpp,cpp}` and the native pytest file, whose
suite was rerun at `fc7114b1` (`native-tests-v3.xml`, 139; it was 136 at
`9d780ddb`).

- **Default bit parity, library level:** the eight-case probe (Shared and
  PerVarianceInterval, explicit and resolvent, flat and three-node smiles) was
  registered temporarily and run against the changed fitter twice (after
  `0dce60a7` and after `9d780ddb`): 8/8 cases, all 232 binary64
  sigma/price/objective values and all five optimizer counters per case equal
  the baseline (`b3e08ab02373f8a69cb0cb05844937604c9e7450b5c291c0f53fab0dc5f178a9`
  both times). The registration was removed; `atx-vol/CMakeLists.txt` equals
  the base.
- **Default bit parity, dev200:** the unchanged R104 policy reproduced the R104
  prediction bytes `6b129f64...` with all three lane binaries (v1 `0dce60a7`, v2
  `9d780ddb`, v3 `fc7114b1`); hashes in the benchmark section.
- **PCH-off (`vol-hygiene`, Debug):** `check` compiled the owning objects of every
  changed header: `src/fitting/american_markov_{fit,policy,seed}.cpp`,
  `src/marketdata/american_markov_market.cpp`,
  `tools/quote_bench_common{,_policy,_seed}.cpp`, `tools/quote_bench_main.cpp`
  and the fit/seed/policy/market test sources (focused and monolithic objects
  are compiled only, never linked). The seed tool sources were rechecked after
  `fc7114b1`. Zero diagnostics.
- **Probe receipts:** `markov-fit-20261016-eval-cost-probe.{cpp,json}` (the
  compile-vs-price and transition-reuse numbers quoted above).

Receipts in `C:/atx-cache/quote-benchmark/`, prefix `markov-fit-20261016-`:
`ctest.{log,xml}`, `{fit,seed,policy,market}-tests.xml`, `native-tests.xml`
(136), `native-tests-v3.xml` (139), `parity-{candidate,summary}{,-v2}.json`,
`hygiene-check{,-2,-3,-4}.log` (`-4`: final-head recheck of the seed tool
source, exit 0), `build-owning-targets.log`; SHA256 list in
`markov-fit-20261016-receipts.sha256`.

## Benchmark (dev200, development split)

Folder `C:/atx-cache/quote-benchmark/markov-fit-20261016-bench/`. Frozen
runner copy `tools/quote_benchmark.py` (`34ac8b20...`), dataset
`20261014-dev200`, `--model common-markov --dividends funding-pv`. Each run
wrote a fresh `runs/<name>/` folder (exclusive creation). Labels were read only
by `quote_benchmark.py score/compare` (`tools/score_experiment.py`) and by the
unmodified diagnosis scripts run through `analysis/run_diag_mf.py` (text
substitution of the run table and output folder only) and
`analysis/exp_compare_mf.py`. The predeclared plan is `experiment-plan.json`;
`experiment-plan-addendum.json` declares m5/m6 (see the disclosure below).

| Run | Policy delta vs R104 (exact keys in `policies/policy-manifest-v3.json`) | Binary |
| --- | --- | --- |
| m0 | none (parity) | v1, v2, v3 |
| m1 | X1 keys (quote seed, sparse `prior_weight` 0.001) with estimator `bid-mid-ask-then-sigma-interval` | v2 |
| m2 | ordinary: 5 knots `[-0.6,-0.3,0,0.3,0.6]`, LM, 64 evals/param capped 1024, `max_sweeps` 64, objective tol 1e-6 | v2 |
| m3 | m2 + m1 seed keys | v2 |
| m4 | ordinary: 5 knots, coordinate search, 64 evals/param capped 1024 (budget 320) | v2 |
| m5 | m1 + `initialization.prior: seed` | v3 |
| m6 | m3 + `initialization.prior: seed` | v3 |

v1 (`0dce60a7`) aborted m1 on the null seed counters fixed in `9d780ddb`
(receipt kept in `runs/launch-failures/`); m0 and m2 were rerun on v2 with
identical bytes. Binary SHA256: v1 `62d2f1f0...0ced86fa`, v2
`0385042d...5ea3f228`, v3 `92f00366...5d601490a`.

**Parity.** m0 predictions are `6b129f6478c86b10b7a8dc3a5b20beecc5f35218fb21471a919d15fe178ad8e2`
with v1, v2 and v3, equal to R104.

**Exact intersections, price MAE (USD).** Every run serves the same 45,070 rows
and scores 44,468 price rows, so all paired numbers are on that one mask
(`a` = reference, `b` = run). Fixed classes from the cm diagnosis: A sparse
ATM < 0.8x (3,608 rows), B ordinary failed-evals ATM < 0.8x (5,567), C other
ordinary ATM < 0.8x (295), D ATM >= 0.8x (14,768), E not fitted by cm (19,887).

| Run | ALL | A | B | C | D | E | improved / worsened vs R104 | ALL vs X1 (0.3428) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| R104 | 0.3519 | 0.7327 | 0.2881 | 0.5824 | 0.3377 | 0.3131 | - | - |
| X1 | 0.3428 | 0.6877 | 0.2877 | 0.5824 | 0.3200 | 0.3141 | 0.4461 / 0.3529 | - |
| X2 | 0.3559 | 0.7327 | 0.2996 | 0.5812 | 0.3372 | 0.3193 | 0.3285 / 0.3468 | - |
| m0 | 0.3519 | 0.7327 | 0.2881 | 0.5824 | 0.3377 | 0.3131 | 0 / 0 | 0.3519 |
| m1 | 0.3428 | 0.6877 | 0.2877 | 0.5826 | 0.3200 | 0.3141 | 0.4859 / 0.3921 | 0.3428 (0.057 / 0.060) |
| m2 | 0.3532 | 0.7327 | 0.2975 | 0.5812 | 0.3371 | 0.3138 | 0.3379 / 0.3369 | 0.3532 |
| m3 | 0.3436 | 0.6877 | 0.2975 | 0.5812 | 0.3194 | 0.3138 | 0.4812 / 0.3980 | 0.3436 |
| m4 | 0.3547 | 0.7327 | 0.2990 | 0.5812 | 0.3372 | 0.3166 | 0.3250 / 0.3501 | 0.3547 |
| m5 | **0.3391** | **0.6753** | **0.2237** | 0.6203 | 0.3317 | 0.3173 | 0.5300 / 0.3543 | 0.3391 (0.485 / 0.326) |
| m6 | **0.3390** | **0.6753** | **0.2208** | 0.6169 | 0.3315 | 0.3180 | 0.5291 / 0.3558 | 0.3390 (0.515 / 0.360) |

Paired against their own parents: m5 vs m1 ALL 0.3428 -> 0.3391, A 0.6877 ->
0.6753, B 0.2877 -> 0.2237, C 0.5826 -> 0.6203, D 0.3200 -> 0.3317; m6 vs m3
ALL 0.3436 -> 0.3390, B 0.2975 -> 0.2208. Core-vol MAE on the exact
intersection with R104: m5 980 bp vs 1,127 (25,030 rows), m6 976 vs 1,128.
Own-population vol rows scored fall from 38,762 (X1/m1) to 38,405 (m5) and
38,430 (m6): fewer rows are vol-identifiable, so vol numbers are not
mask-equal across runs.

**ATM sigma / ATM srVol by fixed class** (median ratio, snapshots >= 0.8x;
39 A, 21 B, 2 C, 93 D; re-derived from native sigmas by the diagnosis).

| Run | A | B | C | D | Seeds applied A/B/D |
| --- | --- | --- | --- | --- | --- |
| R104, m0, m2, m4 | 0.410 (0) | 0.962-0.985 (19-20) | 0.707-0.709 | 1.008-1.011 (86) | - |
| X1 | 0.655 (9) | 0.962 (19) | 0.709 | 1.028 (91) | 8 / 21 / 67 |
| m1, m3 | 0.655 (9) | 0.962, 0.983 (19, 20) | 0.707-0.708 | 1.028, 1.033 (91) | 32 / 21 / 93 |
| m5 | 0.735 (17) | 1.021 (20) | 0.469 | 1.016 (76) | 32 / 21 / 93 |
| m6 | 0.735 (17) | 1.012 (20) | 0.464 | 1.016 (76) | 32 / 21 / 93 |

**Optimizer work (ordinary profile, 91 fits; label-free, `analysis/optimizer-m0-m6.json`).**
Fit seconds and wall seconds are unqualified: shared host, runs overlapped
with each other and with other lanes.

| Run | Search, knots | Budget | Evaluations sum (median / max) | Terminations | Jacobian evals | Rejected steps | Admitted rollback work | Fit s | Wall s |
| --- | --- | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |
| m0 (v3) | coordinate, 3 | 256 | 15,349 (175 / 193) | step 60, sweep 31 | 0 | 0 | 7.05e10 | 487.8 | 540 |
| m1 | coordinate, 3 | 256 | 15,403 (169 / 193) | step 62, sweep 29 | 0 | 0 | 6.98e10 | 472.2 | 523 |
| m4 | coordinate, 5 | 320 | 27,694 (320 / 320) | **evaluation 67**, step 24 | 0 | 0 | 1.24e11 | 625.3 | 662 |
| m2 | LM, 5 | 320 | 4,618 (52 / 102) | objective 36, step 51, gradient 4 | 3,635 | 220 | 2.32e10 | 191.5 | 251 |
| m3 | LM, 5 + seed | 320 | 3,618 (37 / 96) | objective 33, step 58 | 2,840 | 177 | 1.51e10 | 100.5 | 150 |
| m5 | coordinate, 3 + seed prior | 256 | 13,027 (151 / 193) | step 70, sweep 21 | 0 | 0 | 6.76e10 | 473.4 | 523 |
| m6 | LM, 5 + seed prior | 320 | 2,546 (30 / 74) | step 54, gradient 21, objective 16 | 2,040 | 82 | 1.44e10 | 106.2 | 160 |

X2 (coordinate, 5 knots, 256) ended 84 of 91 ordinary fits at the evaluation
limit. Scaling the coordinate budget to 320 (m4) still leaves 67/91 at the
limit; LM converges all 91 under the same cap with 3.3x-6.0x fewer
evaluations than R104's 3-knot coordinate search. Median cost per evaluation
stays 8.5-14 ms. Sparse fits (93, one Shared parameter, coordinate search in
every run): 3,193 evaluations in R104/m2/m4, 3,595 in m1/m3, 2,835 in m5/m6.
`reused_quote_prices` is 0 in every run: dev200 fits only Shared profiles
(no dense snapshot), so the exact prefix reuse is exercised only by the unit
tests.

**Seeds (m1/m3/m5/m6, identical seeds).** Of 184 seedable snapshots, 120 apply
bid-mid-ask, 53 apply the interval fallback (7 ordinary, 46 sparse; 82/93
sparse seeded vs 36 before), 11 stay unseeded. All 11 are sparse with every
quote `far_moneyness`, so the interval estimator refuses with
`missing_endpoint` (no near-forward quote); configured initial and prior are
kept. Of the 53 intervals, 33 have both sides, 17 only an upper (ask) bound, 1
only a lower bound, and 2 are crossed (reported, not repaired).

**Findings.**

1. Default behaviour is unchanged: library probe bit-equal, and dev200 R104
   bytes reproduced with all three binaries.
2. LM fixes the 5-knot budget failure (0/91 at the limit, vs 84/91 in X2 and
   67/91 with a scaled coordinate budget) at 21-39% of R104's ordinary fit
   time, but converged 5-knot fits do not beat 3 knots on labels (m2 0.3532 vs
   R104 0.3519; class B 0.2881 -> 0.2975). The interval-hinge objective, not
   the optimizer, limits accuracy there.
3. The interval seed starts 82/93 sparse snapshots near the quoted level, but
   alone it does not move class A (m1 = X1 = 0.6877): with a hinge loss that
   is zero inside wide spreads, the fit walks back to the configured 0.25 prior
   centre. Using the applied seed as the prior centre (m5/m6) does move it:
   class A 0.6877 -> 0.6753, ATM ratio 0.655 -> 0.735, and the largest gain is
   in class B (0.2877 -> 0.2237), where ordinary snapshots also sat at the
   prior. m6 (LM + 5 knots + seed prior) matches m5 on accuracy (0.3390 vs
   0.3391) at 22% of its fit time.
4. Costs of the seed prior: class C worsens (0.5826 -> 0.6203, 295 rows, 2
   snapshots), class D slightly vs X1 (0.3200 -> 0.3317, still better than
   R104's 0.3377), D snapshots at >= 0.8x fall from 91 to 76, and fewer rows
   are vol-identifiable. These were not attributed further.

**Disclosure.** The addendum (m5/m6 and the `prior: seed` key) was written
after the m0-m4 scores and diagnosis had been read. Its trigger is a label-free
native-report mechanism (fits returning exactly to 0.25), but the choice to
build it was made with the class-A result in view. m5/m6 are therefore one
post-hoc iteration on the development split, not confirmation; no weight,
tolerance or cap was tuned.

Artifact hashes (full list: `markov-fit-20261016-bench/artifacts.sha256`, 105
lines; receipts: `markov-fit-20261016-receipts.sha256`):

| Artifact | SHA256 |
| --- | --- |
| m0 predictions (v1 = v2 = v3) | `6b129f6478c86b10b7a8dc3a5b20beecc5f35218fb21471a919d15fe178ad8e2` |
| m1 predictions | `531c843f596edf0e87169bce27308878d0ac8897587f0eb3025a4488ef220a9a` |
| m2 predictions (v1 = v2) | `93b39363e4ba92608c41b8227c73bfa5272b2437b1305d85275f3f374aa39579` |
| m3 predictions | `efa6a082131196f0fa8f58955dc0ce25b204a142c752379314cff1f48329205d` |
| m4 predictions | `b33c2e65257f83ce949c8414a48c64971c4ecdb473d6cda54247736285a214a8` |
| m5 predictions | `2ed1cd7445fa0bcd6f90860d90abc05f31bc77659b1dcc9a3527c5134a0dc96a` |
| m6 predictions | `dc435595324ac59171be8fb05d77d2e78c74774859f54a7285e873bb84228076` |
| m1..m6 scores | `8b15e6a1...`, `1e3de7ee...`, `108a60dc...`, `c134bbb6...`, `37ab5641...`, `4d3046a6...` |
| binaries v1 / v2 / v3 | `62d2f1f0e8d4c8985bcf809cd32543c1e58879de53ba728bc2a35bfd0ced86fa` / `0385042dfdda22fbae719e74da88afa3d37807a11451b1898d9a6ae48ea3f228` / `92f003662729fbad027fac80631a011b40913fcd826bddbec07aec75d601490a` |
| experiment plan / addendum | `8e8f4f92...d94f840` / `f04078c8...16fa8b` |
| policy manifest v3 | `cf90045fdfd15da8c66d0d043a550a74d78b5711dc09d88f8a6b3583e2088fb1` |
| optimizer summary | `57a5b7876e7dc4f49dfc8dbd37ca92a73b92397ad05cb641a456250db7eed22f` |
| fixed-class tables (`diag-final-v3/compare/`) | paired `f49edab3...`, ATM `4e69eb30...`, coverage `0fd0f250...` |

## Unfinished

- No default promotion: every key is opt-in and R104 stays the default.
- m5/m6 are a post-hoc development-split result; they need a held-out or
  fresh-day confirmation before any policy adoption.
- The seed prior applies to every profile at once; a sparse-only variant
  (per-profile key) is the obvious split to explain class B vs C/D effects.
- Exact prefix reuse is unit-tested but unused on dev200 (Shared only); no
  per-interval or time-knot LM run on real data.
- No qualified timing: all fit/wall seconds come from a shared, loaded host.
- `DampingLimit` has no unit test that reaches it; the X3 parameter-cap
  coverage issue was not touched.
- 11 sparse snapshots stay unseeded (all quotes far from the forward); the
  far-moneyness gate is unchanged.
