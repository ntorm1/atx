# ATX-VOL next-parent handoff: common-model and policy continuation

Prepared 2026-10-16, starting 03:15 America/New_York / 07:15 UTC.

## Start here: the user stopped the task

Latest user instruction: **"stop here and write detailed handoff file for next
parent agent"**. Development stopped. Only preservation, documentation and
lease/process shutdown followed. Do not resume implementation until the user
resumes. The long-running goal is paused, not achieved; final shutdown receipts
appear at the end of the main-workspace copy of this file.

**Nothing has been merged into main.** `C:/atx` main remains
`e696e208ef622aeb8a9ffbcc4bd3e4ea16a74548`. Visible handoff files there are local,
untracked documentation. No push, PR, deployment or trading action occurred.

The authoritative integration branch is **`feat/vol-continue-20261016`**.
Its final code checkpoint is **`911830fec31ae96729c64e6af5dd86027fa190e5`**.
A later documentation-only commit preserves this handoff. Resume from the
branch tip under a fresh lease, not from a released pool directory. Release
resets physical source to its frozen base while preserving the feature branch.

This is the newest handoff. It supersedes current-state claims in
[the earlier October 16 handoff](2026-10-16-next-parent-handoff.md) and
[the October 15 handoff](2026-10-15-next-parent-handoff.md). Those documents
remain useful for historical architecture, earlier benchmarks, research and
build traps. In particular, the earlier claims that native policy, local vol,
scoring metadata, explicit dividend CLI and PWL crossing repair are pending
are now obsolete: all are integrated in this branch.

The next parent should immediately understand these five facts:

1. The current quote-fitting benchmark works end to end with automatic raw
   liquidity policies, mandatory static no-arbitrage gates and separate labels.
   Coverage is poor: **10,694 / 63,942 eligible development contracts, 16.7245%**.
   Sparse profiles serve only 308 / 10,792. Do not relax admission to hide this.
2. The common Markov model now supports interval local volatility. Its finite
   positive-process construction is stronger than independently admitted slices,
   but **continuous-American accuracy is not established**. Default grids have
   substantial numerical bias, including a near-expiry ATM put around 0.00126
   against a separate FD estimate around 0.03223.
3. A reusable common-model bid/ask interval calibrator is implemented, corrected,
   independently approved and committed on a separate branch. **It is not
   integrated or registered permanently in CMake.** Both commits are required.
4. An independent QuantLib finite-difference reference tool is being preserved
   on a separate branch. Its lane tests and numerical results are distinct from
   parent review/integration. Read the final reference-lane receipt below.
5. Concentrated grid generation is a proposal only. No implementation exists.
   Native common-model benchmarking, profile-specific common-model configs,
   continuous accuracy, fast updates, Greeks and HFT qualification remain open.

## User requirements that persist after resume

- Build modular, reusable, high-performance American equity option fitting,
  pricing and analytics for an HFT options market-making/relative-value backbone.
  The ambition is much larger than this checkpoint. Do not claim world's fastest,
  globally most accurate, production ready or complete without evidence.
- Use the Downloads option history for benchmarking. **`srPrc` and `srVol` are
  labels only**, physically separated from model input. Never use fair values,
  vendor fair/ATM/quote implied vols, Greeks, `synSpot` or model flags as fitting
  features. Raw contracts, bid/ask, underlying quotes, rates, clocks, dividends
  and supplied carry are allowed. Do not hardcode this parquet or ticker list.
- Explicitly use subagent-driven development. The user explicitly rejects TDD:
  implement first, then meaningful independent numerical and boundary checks.
- Breaking redesigns/removing outdated code are authorized. Backward
  compatibility is not a goal.
- **Every valid fit must enforce no-arbitrage.** Better benchmark error or
  coverage cannot justify bypasses, weakening required constraints or silent
  post-certification repair. Distinguish finite-model consistency, necessary
  static tests, numerical error and quote-fit quality.
- **Different symbols need different configurations and parameterizations.**
  Use raw liquidity evidence and explicit symbol overrides. A sparse wide small
  cap should have stronger regularization/lower flexibility than a liquid ETF.
  All profiles retain mandatory constraints.
- During active work, continue autonomously without questions. The latest stop
  request overrides that persistence instruction until the user resumes.
- Research current theory/performance with primary sources where relevant.
  Do not mistake vendor descriptions, same-grid parity or unit tests for a
  continuous-American accuracy or speed certificate.

## Durable branches and exact commits

Main is e696e208 above. Root started this continuation from frozen base
`4fa9afddcf84afb4cfc78d4d0fac26a0ff72b744` on leased pool-35, RunId and
HeartbeatId `vol-20261016-root2`. That base already contains the earlier
shared PWL surface and library funding-PV work.

Root commits in chronological order:

| Commit | Change |
| --- | --- |
| `35829a692274cab080e82f54991095d4dfd10186` | Synthetic Markov audit source and original audit handoff, equivalent to bd6e7b5f |
| `afbdd2fe8a86ad1382c504ee262e6c534e860afa` | Permanent `atx-vol-american-markov-audit` target |
| `46564499451fe1aa2675cbe8ec991c4a16aa23a1` | Native bounded policy parser/routing, equivalent to cacb48eb |
| `28bcba49ca83e8d1df97f01ae18b4a373a3d29bb` | Native policy validation/correct configured vs actual curve metadata, equivalent to 6b7e5a0b |
| `dd539657edcfe4d08847d79951be6030b3eeca70` | Owned interval local volatility, equivalent to 61b93c56 |
| `ef561e6276d6730e368b3e14b0bdaf67dcf9c304` | Explicit funding/escrowed dividend CLI and validation |
| `0d8e225d82004e2e13d6d0563bfe7da45d8fc0c5` | Policy coverage/paired transition scoring, equivalent to f8bf08e1 |
| `8d7e7d1e6081df53986ec29e4a4962930655c2f0` | Bounded nonrepresentable exercise-crossing repair, equivalent to ac07729f |
| `911830fec31ae96729c64e6af5dd86027fa190e5` | ATM/nearby audit sampling, schema 2, working sprint and ledger facts |

Do not cherry-pick the original equivalents again. The native first commit's
historical `wip` title does not mean the integrated pair is unvalidated: fresh
review and focused integrated checks passed. Do not infer full qualification.

Pending ready calibrator branch: **`feat/vol-markov-fit-20261016-weightfix`**.
Apply these in order after acquiring a new integration lease:

1. `fa85224d4717a006d6a7ee0a4c70b2a26c90e263`
2. `7f85cd1f5494cf2003a003534815c9f5cb6b4469`

The original implementation branch `feat/vol-markov-fit-20261016-review`
contains the first. The weightfix branch contains both. Independent reviewer
`pricing_audit` approved the exact combined SHAs, verified clean HEAD and
14-test XML. Root did not cherry-pick them after the stop request.

Pending independent reference branch: **`feat/vol-reference-audit-20261016`**,
commit **`8f73464d4647b5574915508492b675ddd2b66005`**, based on dd539657,
lane pool-34, RunId `vol-20261016-reference`. It is committed but has not received
independent parent review or integration. Final tests, artifacts and release
details are in the shutdown addendum below.

Other preserved lane branches are historical/integrated:
`feat/vol-policy-score-20261016`, `feat/vol-pwl-crossing-20261016`,
`feat/vol-localvol-20261016`, `feat/vol-native-policy-20261016`,
`feat/vol-pricing-audit-20261016`. Prior roots are
`feat/vol-resume-20261016` and `feat/vol-benchmark-20261015`.

## Repository workflow and environment

- Windows PowerShell 5.1, clang-cl 18, C++20, CMake/Ninja/LLD, ccache, shared
  `C:/atx-cache/vcpkg_installed`. Release AVX2/IPO, no fast-math. Do not invoke
  clang-format or clang-tidy; repository explicitly disables them.
- Read `CLAUDE.md`, `.agents/cpp/agent.md`, `atx-vol/CLAUDE.md`, and
  `.claude/skills/vol-dag/SKILL.md`. User no-TDD overrides local TDD. The broker
  workflow was unavailable; user-authorized native agents used separate leases.
- User requests codebase-memory graph discovery first. No graph MCP functions
  were exposed, including during shutdown tool discovery; `rg` fallback was
  justified. Check tool availability again in a future session.
- Use **leased pool worktrees only**, frozen SHA, unique branch/RunId and durable
  HeartbeatId. No raw `git worktree add`. Do not steal/reset unrelated leases.
- Acquisition automatically attempts ordinary `dev` configure, which fails on
  unrelated missing atx-kb/agent-db/options-engine in this restored checkout.
  The lease may already be successfully acquired. Inspect `.atx-lease`; do not
  acquire duplicates. Configure the vol preset through the build wrapper.
- Invoke the **absolute leased-tree** `scripts/atx-build.ps1`, stand in that
  same tree, specify preset/jobs/targets. No raw Ninja or all-target build habit.
- `vol-dev`: Debug `build-vol`; `vol-rel-avx2`: Release/IPO/AVX2
  `build-vol-rel-avx2`; `vol-hygiene`: PCH off `build-vol-hygiene`. Separate
  FetchContent worktree/preset directories prevent Debug/Release ABI races.
- Release using the acquiring RunId after owned files are committed. Release
  resets physical checkout to frozen base; the branch is the durable source.
- No licensed historical oracle or holdout workflow was invoked. New quote-only
  benchmark is separate. Do not open untouched validation labels during tuning.
- All source edits/integration/gates belong in leases, not `C:/atx`. Main receives
  discovery copies of handoffs only. Docs/LEDGER.md is append-only.

Typical acquisition after explicit resume (choose fresh identifiers):

```powershell
$volBase = (git rev-parse feat/vol-continue-20261016).Trim()
powershell -NoProfile -File C:/atx/scripts/lease-worktree.ps1 `
  -Branch feat/vol-next-<unique> -Base $volBase -Agent vol-parent `
  -RunId vol-next-<unique> -HeartbeatId vol-next-<unique> -MaxPool 40
```

Check lease status before interpreting configure failure. Substitute the
actually acquired pool path in all commands below.

## Integrated architecture and invariants

### Existing quote fitting and static admission

Public APIs: `api/fitting/quote_slice.hpp`, `quote_fit_policy.hpp`,
`api/pricing/american_price_slice.hpp`. The raw quote path prepares separate
calendar/variance clocks and contract carry, inverts American prices, fits its
configured volatility curve and reprices. Final immutable price-slice admission
checks intrinsic/prepaid bounds, monotonicity/vertical slopes, global convexity,
call/put inequalities and cross-expiry conditions at union strike knots.
Specialized nondividend relations are also enforced. Global fixed tolerances
are scale-bounded. Off-node queries use admitted PWL prices plus IV inversion;
no extrapolation. No price post-repair silently escapes these gates.

These are necessary static conditions, not a general proof that independently
fitted American slices share one stock process. This distinction matters even
when every individual slice passes. Per-strike vendor-supplied carry remains
in this existing quote path. A future common-process adapter cannot silently
assign a different drift to the same stock for every strike.

### Native policies and explicit dividend conventions

`tools/quote_bench_main.cpp` and private `quote_bench_policy.hpp` implement
default `--policy auto`, bounded closed-schema JSON profiles and exact symbol
overrides. `--curve` selects a fixed experiment and is mutually exclusive with
policy; quality/min-quotes overrides require explicit fixed mode.

Validation covers finite ranges, unknown/duplicate JSON or CLI fields, 1 MiB
configuration/depth bounds, 64 profiles/4096 overrides and input/output/report/
policy aliases including hardlinks. Resolve once per underlying/root/exact
snapshot. Ambiguous duplicate expiry identities are excluded before classifying
liquidity. Auto/fixed/override paths retain mandatory strike/calendar gates.

Catalog ownership preserves resolved config lifetimes. Raw near-money spreads,
bidless fraction and distinct strike count select profiles, with no ticker list.
Initial uncalibrated defaults: dense ConvexDense/min12, ordinary eSSVI/min6,
sparse eSSVI/min4. Dense classification requires >=24 strikes, relative spread
<=.08 and bidless fraction <=.10; sparse includes <6 strikes, relative spread
>=.60 or bidless fraction >=.50. Read actual catalog for exact routing.

CSV metadata: `resolved_profile`, `policy_source`, `configured_curve`,
`actual_curve`. An actual curve can exist before later admission rejection;
**status determines serving**, not a nonempty curve name. Expanded runtime
policy provenance is intentionally not the input JSON schema roundtrip.

Native CLI now accepts **`--dividends funding-pv|escrowed-pv|none`**, default
funding-pv. Old `aggregate-pv` spelling is rejected, an intentional breaking
cleanup. API conventions:

```text
funding-pv: AggregateFundingPresentValue / aggregate_funding_pv_approximation
  prepaid = S * exp(-q * calendar_years) - PV
  forward = prepaid * exp(r * calendar_years)
escrowed-pv: AggregatePresentValue / aggregate_pv_approximation
  forward = (S - PV) * exp((r-q) * calendar_years)
none: continuous yield, nonzero aggregate PV refused
```

Both PV modes approximate missing dated dividends with continuous yield. They
cannot reconstruct American exercise around unknown ex-dates. Funding default
follows the documented forward identity, not label-based convention tuning.
Native reports include the CLI spelling and explicit assumption; nested profile
configs use API strings. Python run receipts record both meaning and flag.
Passthrough cannot override the wrapper-bound dividend convention.

The old unconditional PV<spot native guard was removed. Library-specific
validation allows a positive funding-prepaid stock even if PV>=spot under
sufficiently negative carry. Independent no-early-exercise call tests cover
S100, q=-2, T=.5, PV150, and incompatible escrowed/none refusals.

### Common finite-state American process and interval local vol

Public APIs: `api/pricing/american_markov.hpp`,
`american_markov_batch.hpp`, `american_markov_pwl.hpp`, `pwl_strike.hpp`.
Matching implementations/tests are in `src/pricing/` and `tests/`.

The immutable model evolves transformed stock X=S/G on a common nonuniform
grid. Three-point nonnegative martingale transitions use separate variance/
calendar clocks, common funding/carry, and bounded storage. Cash layers use
limited liability min(D,S), shifting grids consistently. Zero/upper boundaries
are absorbing in model coordinates. Every expiry uses a prefix of one process.
Snell rollback constructs a consistent **finite Bermudan model**. This is not
a continuous-exercise, infinite-domain or market-fit accuracy certificate.

Defaults: uniform65 states, upper4S, CFL .9, max clock step1/52; soft caps
1025 states/8192 transitions/1m stored rows, hard4097/65536/8m. Read the header
for all knobs. Do not increase caps silently to conceal work/memory failures.

Scalar and batch queries use prepared reusable workspaces. Batch is state-major
with contiguous strike buffers and shared transitions, side/exercise specialized.
Size arithmetic and overlap are checked; outputs publish only on success.
Prepared price queries do not allocate. Compilation/calibration do allocate.

New `AmericanMarkovLocalVolNode` stores log(X/initial spot) and nonnegative
volatility multiplier. Empty profile means one; otherwise PWL log-coordinate
interpolation with flat tails scales interval baseline sigma. The model owns
interval knots and sampled state sigmas in `interval_data()`. Transition
`interval_index` identifies the interval; `transition.volatility` stays baseline
sigma. State sampling is recomputed after cash events. Constant-one profiles
preserve the old model exactly. Profile cap257, abs(log coordinate)<=64;
stored volatility values cap1m soft/8m hard.

### PWL surface and resolved crossing failure

Convex strike algebra retains represented payoff/exercise knots without
arbitrary resampling/compression. Positive weighted sums and exercise maxima
compile call/put strike functions for each expiry of the same model. Immutable
const queries do not allocate. Work/live/output knot budgets refuse explicitly.

The actual old failure was opposite gaps +/-7.1054273576e-15 between adjacent
doubles K=175 and 175.00000000000003. The true exercise crossing has no interior
representable strike. Commit8d7e7d1e adds
`crossing_resolution_error_bound()` and surface diagnostic
`max_output_crossing_resolution_error`.

Only a nonrepresentable crossing may use the endpoint-max chord. Its excess
is bounded conservatively by min(abs(endpoint gaps)), rounded outward.
Positive sums propagate weighted bounds using outward-rounded multiply/add;
exercise max is 1-Lipschitz and adds newly incurred crossing error. Every
function stays within the unchanged fixed64eps*max(1,maxnodevalue) allowance
and unchanged global convex-hull gate. This is **not a bound on all IEEE
roundoff, finite-grid pricing error or continuous exercise**. All representable
crossings stay represented; no arbitrary knot deletion or cap increase.

Read `sprints/2026-10-16-pwl-crossing-resolution.md` for the argument. Full
33-strike historical panel PWL success improved36->39 of84 compiled models;
45 explicit resource refusals remain. Four of88 models fail transition caps.
Exact PWL knot growth is still a practical scalability limitation. Batch/scalar
queries remain useful without compiling a complete PWL surface.

## Pending common-model interval fitter: approved, not integrated

New files in fa85224d plus correction7f85cd1f:

- `include/atx/vol/api/fitting/american_markov_fit.hpp`
- `src/fitting/american_markov_fit.cpp`
- `tests/american_markov_fit_test.cpp`
- `sprints/2026-10-16-markov-interval-fit.md`

API `fit_american_markov_intervals` accepts common spot, an interval schedule
(two clocks/r/q/cash), state grid/model config and raw quote contracts
(expiry index, strike, side, bid, ask, weight). Duplicate contracts are refused.
Bidless quotes impose upper bounds only; locked positive quotes are valid.
No labels, ticker selection or strike-dependent stock drifts are inputs.

Parameters are positive **absolute sigmas**, baseline fixed1 to avoid a
baseline-times-multiplier degeneracy. Empty log-state knots select one flat
node. Shared or PerVarianceInterval layout controls time flexibility; an
interval with stopped variance has no additional parameters (maps to group0,
whose sigma does not diffuse there). Caller chooses initial/prior parameters,
regularization and model complexity. Result deep-owns best model, parameters,
knots, group mapping, diagnostics and prices in original quote order.

Loss is normalized weighted squared distance outside [bid,ask], scaled by
max(half-spread, price_scale_floor). Optional midpoint tie-break applies only
to positive bids. Fixed log-sigma prior and adjacent strike/time index-difference
roughness are explicit. Empty prior means initial parameters, not moving best.
Zero penalty is allowed but does not establish sparse-data identification.

Bounded deterministic log-coordinate search evaluates positive/negative steps,
keeps strict improvements and halves step after an unimproved sweep. Every
trial compiles one process and grouped batch-prices all queries. Termination
distinguishes step tolerance, evaluation limit and sweep limit. Initial invalid
input/compile/objective returns failure; later refused trials retain best finite
iterate and count failure. A returned finite model can still miss every quote
interval. No global optimum, zero interval error or continuous accuracy claim.

Defaults/hard limits recorded by lane (verify header when using): parameters
32/128; quotes4096/16384; evaluations256/8192; sweeps32/1024; row-option rollback
work50m/1b; batch options128; max buffer1m. Default sigma bounds.01..3, hard
minimum1e-6 and maximum100; positive weights<=1e6; price floor>=1e-12;
penalties<=1e6. Actual prefix row-option work is charged before each price.
Peak memory includes incumbent and one candidate model plus scratch/buffers;
allocation failures propagate as documented. This is an offline fit initially.

Independent review found a material numerical defect before integration:
normalizing denorm_min weight by total first could underflow to0 and erase
a finite ~4.940656458e-10 loss for a large locked-price residual. Correction
uses sqrt(weight)/sqrt(total), multiplies distance before spread division and
applies sqrt(midpoint coefficient) before squaring. A second tiny-spot case
guards midpoint underflow. **Apply both commits.** Exact combined review
approved resource limits, ownership, bidless semantics and common process;
it does not certify continuous prices or optimizer quality.

Lane validation: **14 Debug tests passed, zero failures/errors/skips,2.107s**,
plus final source/test PCH-off objects. Tests cover flat/skew/time recovery,
stopped variance/cash, ownership, consistency, independent Black approximate
recovery with nonzero grid residual, bidless observations, priors, budget
termination, retaining best after refused candidate, invalids and arithmetic.
Durable copied XML is listed in preservation receipts below.

Root must add `src/fitting/american_markov_fit.cpp` to the library and the test
to the monolithic source list, add focused executable
`atx-vol-american-markov-fit-tests` linked to vol/core/warnings/GTest main,
CTest `atx-vol-american-markov-fit`, and run integrated Release/PCH-off closure.
Lane temporary CMake edits were restored before commits. The focused test name
is a proposal, not a currently registered root target.

Do not blindly add the new header to `api/vol.hpp`: that umbrella is exactly
the frozen Tier-A manifest. New public api headers are Tier-B unless deliberately
promoted. `vol_umbrella_test.cpp` checks README Tier-B counts and installed
headers; those counts may already be stale from new APIs. Reconcile the actual
inventory and run the umbrella gate before release. No such fix/test was done
at shutdown. The lane note's generic "umbrella registration" is not authority
to promote every new API into Tier-A.

## Current synthetic pricing audit and accuracy problem

Permanent target `atx-vol-american-markov-audit`; source
`tools/american_markov_audit_main.cpp`. CLI supports `--full`, `--case NAME`,
`--grid NAME`. Eleven fixtures cross8 grids: default65, uniform129/257,
domain2/domain8 and log65/129/257. It reports explicit refusals, transitions/
rows, boundary diagnostics, independent European/deterministic references,
scalar/batch/PWL differences and diagnostic-only timings.

Newest artifact **`C:/atx-cache/quote-benchmark/markov-audit-20261016-atm-v2.json`**,
schema2, SHA256
`bfd5dba7ee1909e0d2acba83c22bc8ca1e4d170b74dddb2fb4040e94d96725f6`.
Adds exact S and S*(1+/-1e-6) to the old33-strike panel. Do not compare maximum
errors with schema1 without accounting for the changed sample. 88 cases,
84 compiled models,39 PWL successes; max reported crossing bound
3.925776314946786e-16. Resource failures are not hidden.

Default-grid maximum sampled European absolute price errors, dollars/share:

| Fixture | Maximum error |
| --- | ---: |
| martingale | 0.200590716585 |
| funded | 0.19954653065 |
| separate clocks | 0.16983547978 |
| short low vol | 0.02540823182 |
| high vol | 0.09164861996 |
| negative rate | 0.07540709160 |
| negative rate and yield | 0.03897857003 |
| clock pause | 0.53128079835 |

Deterministic reference disagreement is about1.421e-13. Close scalar/batch/PWL
agreement validates one discrete model, not its distance to continuous pricing.
The old panel missed exact ATM and understated martingale default error (.07829).
Historical high-vol domain2 error was8.04812 vs domain4/default65 .09165:
resolution and tail-domain truncation need separate study.

Independent FD lane's short-low-vol preliminary artifact:
`C:/atx-cache/quote-benchmark/american-reference-20261016-short-v1.json`, SHA256
`03c7b237c43be5f1afd0aec28e8586ee836eb58031a9559c559493aed77a3189`.
At exact ATM: call reference .08414055318 vs default Markov .05876458293;
put reference .03223379083 vs default Markov .001257913867. Max joint/time/
space/expanded-domain refinement changes were about3.78e-7 call and8.03e-6 put.
Even log257 put was only .008665971. These gaps dominate observed reference
refinement changes. Reference results still need parent source review and
convention verification; "observed stable" is not a rigorous accuracy bound.

The independent tool uses optional external QuantLib1.44 in
`C:/atx-cache/american-reference-venv`. It reads synthetic audit schema1/2,
uses an independently assembled FD operator/solver and emits dependency,
clock, domain, refinement and input receipts. It does not reuse the C++ Markov
transition implementation. Exact rational time maps are used where supported;
unsupported cases are explicit rather than silently date-rounded. Cash
finite-floor conventions and variance-vol interface roundoff need documentation.
Final details/commit/tests are appended below from the lane's stop receipt.

## Concentrated-grid proposal: design only

Reviewer pricing_audit proposed a new reusable leaf factory such as
`make_concentrated_state_grid(spec)` with **no implementation yet**. Inputs:
spot>0, caller-selected characteristic standard deviation a=sigma*sqrt(variance
horizon)>0, explicit upper spot multiple U>1, exact node budget3..4097.
No strikes, prices, labels or hidden default change. Output would own grid,
exact spot index, adjacent spot gaps, minimum positive spacing and maximum
unit-vol diffusion exit rate.

```text
x/S = 1 + a*sinh(u)
u_left = -asinh(1/a)
u_right = asinh((U-1)/a)
```

Allocate N-1 intervals proportionally to transformed left/right span, at least
one each; uniform u separately on each side. Force exact0/S/U*S endpoints.
Refuse nonfinite, collapsed or non-increasing representation. Do not silently
drop nodes, floor concentration width, increase model budgets or replace the
existing default grid. An explicit absorbing upper domain is a tail policy,
not a certified tail-error bound. One width across disparate expiry horizons
is a caller tradeoff requiring evidence.

Diagnostic R=max_i(x_i/h_left)*(x_i/h_right) estimates diffusion exit rate;
ceil(DeltaTv*sigma_max^2*R/CFL) estimates needed steps, plus time/cash effects.
Final positive-transition/storage gates remain with compile_american_markov.
Arithmetic-only example U4,sigma.2,Tv.25,sinh65 ~92 steps/5980 rows;
sinh257 ~1466/376762; sigma.8,Tv1,sinh65 ~778/50570. **These are estimates,
not measured prices or timings.**

After implementation, validate exact endpoints/count/scale covariance/extreme
representability, then all11 independent FD/European/deterministic fixtures
at65/129/257 with exact ATM and tight central strikes. Include cash, negative
rates, separate/paused clocks and high vol; report refusals, errors, row work
and boundary mass. Do not optimize only the motivating near-expiry case.

Potential later design: positive implicit/M-matrix tridiagonal rollback could
avoid explicit-CFL storage blowup. Current layers/rows duplicate state across
many steps. This is an unassigned research direction, not implemented or proven.
Resolve accuracy first, then optimize memory/work without weakening consistency.

## Benchmark data, leakage controls and fresh results

Source unchanged:
`C:/Users/natha/Downloads/OptionIntradayHist_30min_2026-10-14.parquet`,
1,916,931,890 bytes,34,011,291 rows,280 row groups;31,889,698 American rows.
One day is not multi-day/live validation.

`tools/quote_benchmark.py` projects Arrow columns, physically separates quotes
and labels, hash-splits whole underliers80/20, selects exact snapshots and uses
raw underlying midpoint. Adjusted roots with missing deliverable details are
explicitly refused. Receipts bind source/input/policy/binary/output hashes and
full native reports. Scoring left-joins the entire cohort and separately masks
price/vol labels, preserving missing predictions and failures in coverage.
**No heldout validation labels were inspected.**

Current dataset: **`C:/atx-cache/quote-benchmark/20261014-dev200`**,200 hash
development underliers,10:30 New York,72,348 selected/63,942 raw-input eligible.
New immutable run prefixes with CSV, `.csv.run.json`, `-native.json`, `-score.json`:

| Prefix | Served | Eligible coverage | Price-scored rows | Own-population price MAE | Own-population vol MAE |
| --- | ---: | ---: | ---: | ---: | ---: |
| `20261016-essvi-funding-v2` | 10222 | 15.9864% | 10198 | .3567528845 | 389.938946 bp |
| `20261016-auto-funding-v2` | 10694 | 16.7245% | 10654 | .150988326 | 475.242406 bp |
| `20261016-auto-escrowed-v2` | 10772 | 16.8465% | 10732 | .155959119 | 472.209300 bp |

All used the same current Release binary and mandatory static admission.
Own-population MAEs are **not comparable improvements**: served populations
change. Exact intersections in `20261016-compare-essvi-auto-v2.json`:

- Price4045 rows: fixed .318183628 -> auto .201308293 dollars/share.
- Vol4046 rows: fixed190.723791 -> auto118.380774 absolute vol bp.
- Fixed-only price6153; auto-only6609. Substantial selection differences remain.
- Auto-funding matched midpoint price MAE .280587634 on its10654 price rows.

`20261016-compare-dividend-conventions-v2.json`, A=escrowed, B=funding:

- Price10654 rows: .156582615 -> .150988326.
- Vol10694 rows:475.329634 ->475.242406 bp.
- Funding serves78 fewer and adds none. This small convention effect does not
  solve missing cash-event dates or certify correct vendor input conventions.

Automatic-funding coverage by resolved profile:

| Profile | Eligible | Served | Coverage | Strike/calendar/insufficient failures (rows) |
| --- | ---: | ---: | ---: | --- |
| dense | 17734 | 3336 | 18.8113% | 8192 / 3060 / 3146 |
| ordinary | 34700 | 7050 | 20.3170% | 632 / 8806 / 18212 |
| sparse | 10792 | 308 | 2.8540% | 0 / 618 / 9866 |

716 eligible rows have unresolved metadata and remain counted.132/1138 chains
fit/serve;811 have too few admitted distinct strikes,151 calendar rejection;
other failures are strike admission. Dense80 attempted/16 fit,ordinary607/105,
sparse451/11. This supports interval-based fitting and appropriate complexity,
not disabling mandatory checks.

New scoring metadata accepts bounded optional typed strings; literal profile
names `nan`/`NaN` remain values. Separate missing-state categories distinguish
reported/value_absent/column_absent/prediction_absent without sentinel collision.
`policy_strata` reports complete selected/eligible/served/failure populations
by metadata field and joint profile/family. `policy_transitions_paired` tracks
A/B names plus missing state on exact price/vol intersections. Equal profile
names do not prove equal parameters; consult expanded policies in receipts.

Timings ~12-20s were diagnostic, with concurrent compilation and some overlapping
accuracy runs. **No controlled speed claim.** Older97.6% coverage carry baseline
predates mandatory gates and is not current performance. Other historical
benchmarks and a narrow retained dense-IV CPU comparison are in earlier handoffs.

Current comparison SHA256s:

- essvi/auto: `84cec4fc2e71234deb8c33f32a565de66f7c316ef484ee4f702538e6374a65d3`
- dividend: `3bab0829572326ed1c2b34ecdcd8ba2c181c295387586f282715140aa6d7b212`

## Validation completed and what remains unqualified

Root integrated checks this continuation:

- Initially32 Release Markov/PWL tests with local vol; after crossing fix,
  **36 passed**, zero skips, about76ms.
- **49 Release quote/price/policy tests passed**.
- **42 native CLI Python tests passed** in1.14s. One initial help-banner test
  expected the old singular text; expected banner was updated and all42 rerun.
- **54 Python adapter/scoring tests passed**,14.47s; Ruff clean.
- PCH-off native quote-bench TU (including JSON header) and synthetic audit
  source compiled. Earlier base Markov/local-vol owning checks are documented
  in earlier handoff and agent receipts. Do not claim an entire new PCH-off
  build from the targeted checks.
- Expanded schema2 audit Release target built, full JSON run completed,
  and final audit source PCH-off compiled. Git diff checks clean.
- New fitter:14 Debug lane tests/PCH-off and independent review only;
  **no root integration test yet**.
- Independent reference: lane tests/receipts appended below; parent review
  and integration outstanding.

No full atx_vol_fast, public umbrella suite, module gate matrix, sanitizer,
continuous-American certificate or production release qualification is claimed.
Historical restored baseline had Windows fixture-path and current core error
mapping failures; an AL slow test passed only with a longer460s run. Read
the2026-10-08 LEDGER fact rather than rediscovering or hiding these issues.
New public headers may also require README inventory reconciliation.

Typical focused commands from the actual leased tree:

```powershell
powershell -NoProfile -File C:/atx-wt/<pool>/scripts/atx-build.ps1 `
  -Preset vol-rel-avx2 -Jobs 2 configure
powershell -NoProfile -File C:/atx-wt/<pool>/scripts/atx-build.ps1 `
  -Preset vol-rel-avx2 -Jobs 2 build atx-vol-american-markov-tests atx-vol-quote-slice-tests atx-vol-quote-bench atx-vol-american-markov-audit
ctest --test-dir C:/atx-wt/<pool>/build-vol-rel-avx2 `
  -R '^atx-vol-(american-markov|quote-slice)$' --output-on-failure
powershell -NoProfile -File C:/atx-wt/<pool>/scripts/atx-build.ps1 `
  -Preset vol-hygiene -Jobs 2 configure
powershell -NoProfile -File C:/atx-wt/<pool>/scripts/atx-build.ps1 `
  -Preset vol-hygiene -Jobs 2 check atx-vol/tools/quote_bench_main.cpp atx-vol/tools/american_markov_audit_main.cpp
$env:ATX_QUOTE_BENCH_BINARY='C:/atx-wt/<pool>/build-vol-rel-avx2/bin/atx-vol-quote-bench.exe'
$env:PATH='C:/atx-wt/<pool>/build-vol-rel-avx2/bin;C:/atx-cache/vcpkg_installed/x64-windows/bin;'+$env:PATH
python -m pytest --noconftest atx-vol/python/tests/test_quote_bench_cli.py -q
python -m pytest --noconftest atx-vol/python/tests/test_quote_benchmark.py -q
python -m ruff check atx-vol/tools/quote_benchmark.py atx-vol/python/tests/test_quote_benchmark.py atx-vol/python/tests/test_quote_bench_cli.py
```

`--noconftest` is appropriate for these standalone script tests, not a way to
bypass the compiled atxvol extension contamination guard. Native tests skip
without the binary env var; skipped is not passed. Match Debug/Release vcpkg
DLL directories. CMake must be configured before targeted PCH-off object checks.

Example fresh benchmark after future changes (use new unique output paths):

```powershell
python atx-vol/tools/quote_benchmark.py run `
  --dataset C:/atx-cache/quote-benchmark/20261014-dev200 `
  --binary C:/atx-wt/<pool>/build-vol-rel-avx2/bin/atx-vol-quote-bench.exe `
  --output C:/atx-cache/quote-benchmark/20261014-dev200/<new>.csv `
  --policy auto --dividends funding-pv -- `
  --report C:/atx-cache/quote-benchmark/20261014-dev200/<new>-native.json
python atx-vol/tools/quote_benchmark.py score `
  --dataset C:/atx-cache/quote-benchmark/20261014-dev200 `
  --run-receipt C:/atx-cache/quote-benchmark/20261014-dev200/<new>.csv.run.json `
  --output C:/atx-cache/quote-benchmark/20261014-dev200/<new>-score.json
python atx-vol/tools/quote_benchmark.py compare `
  --dataset C:/atx-cache/quote-benchmark/20261014-dev200 `
  --run-a C:/atx-cache/quote-benchmark/20261014-dev200/<baseline>.csv.run.json `
  --run-b C:/atx-cache/quote-benchmark/20261014-dev200/<candidate>.csv.run.json `
  --output C:/atx-cache/quote-benchmark/20261014-dev200/<new>-compare.json
```

Use `--curve essvi` instead of `--policy auto` for the fixed experiment.
Do not rebuild/replace the binary while a run checks its hashes. Do not overwrite
immutable outputs. Exact historical commands and policy definitions are already
inside receipts; do not reconstruct defaults from memory.

## Research and interpretation

Primary sources consulted during this continuation:

- [SpiderRock live volatility surfaces](https://docs.spiderrockconnect.com/docs/Documentation/PlatformFeatures/Analytics/LiveVolSurfaces/):
  funding-PV forward convention, bid/ask-channel fitting and varying treatment
  of liquidity. Proxy moneyness forward and pricing forward can differ.
- [SpiderRock option pricing](https://docs.spiderrockconnect.com/docs/Documentation/PlatformFeatures/Analytics/OptionPricing/):
  generalized inputs, dated cash, modified binary-tree/dividend splicing and
  a stated numerical ambition around a tenth of minimum tick. This is a vendor
  description, not measured accuracy of this library.
- Reference lane used official QuantLib1.44 source, FD engine/operator/dividend
  handling; exact URLs/dependency information are in its tool/report/note.

Earlier handoff retains American no-arbitrage, negative-rate, local-vol/cash
research links. Reverify facts when using them. European surface theorems do
not automatically prove American joint consistency. One trading day's label
agreement does not identify a unique model or establish production readiness.

## Recommended next sequence, only after user resume

1. Inspect branch/main/leases, read this handoff and final addendum, acquire
   a fresh root lease from feat/vol-continue-20261016. Do not use main's old code
   or assume released pool contents still match the feature branch.
2. Integrate both independently approved fitter commits; root owns permanent
   CMake registration. Run focused Release and owning PCH-off checks. Keep the
   finite-model/fit-quality/continuous-accuracy distinctions in API and reports.
3. Independently review the preserved reference tool's exact final SHA, clock
   mappings, cash convention and stability classification, then integrate.
   Reuse saved diagnostics; do not rerun expensive panels without a reason.
4. Delegate the concentrated-grid leaf implementation and post-implementation
   validation. Root wires controlled audit comparisons. Address near-expiry
   bias without worsening tails/dividends/negative-rate regimes or hiding caps.
5. Build a reusable common-market adapter and native benchmark path using the
   new fitter. Need one shared spot/funding/carry/dividend schedule across
   strikes and expiry prefixes. Existing per-strike supplied q cannot become
   a distinct stock drift per option. Choose and report common carry estimation
   from allowed raw inputs, retain dispersion/quality diagnostics, and make
   missing ex-date PV approximation explicit. No such adapter exists yet.
6. Extend liquidity configs to common-model flexibility, time groups, knots,
   priors, regularization and work budgets. Current catalog wraps QuoteSliceConfig
   only. Sparse/wide and dense/tight data need different identification controls.
   Defaults are not optimized; do not use vendor labels as runtime features.
7. Compare fresh development results on exact intersections with whole-cohort
   coverage/failures and per-profile reporting. Freeze choices before untouched
   validation. Broaden beyond one snapshot/day when suitable raw data exists.
8. Tackle PWL knot growth/storage and fit/update/query speed after correctness.
   Measure quiet pinned Release latency, tails, throughput and allocations;
   concurrent diagnostic timers cannot support HFT claims. Add model-consistent
   Greeks/analytics, uncertainty and operational publication/versioning next.
9. Reconcile public API inventory and complete real release gates before main
   delivery. Inspect intervening user changes; don't blindly merge/reset.
   The goal is not complete because these focused pieces or tests passed.

## Preservation and shutdown receipts

At stop, root pool-35 was clean at911830fe; no root build/benchmark session
remained. Root made no further code changes or pending-lane integrations.
Handoff only is committed after that code checkpoint.

Fitter original pool-36 lease was released; correction pool-33 release confirmed:

```text
RELEASED pool=pool-33 run_id=vol-20261016-markovfit-fix
```

Both fitter commits remain on feat/vol-markov-fit-20261016-weightfix. Independent
reviewer pricing_audit stopped with no lease, owned changes or running process.
Its concentrated-grid work is design text only. Earlier scoring pool-34 and PWL
pool-33 task leases were released after delivery. Reference lane final receipt
is appended separately; do not confuse its reused pool-34 with the old score lane.

External preservation directory created at shutdown:
**`C:/atx-cache/quote-benchmark/handoff-20261016-0315`**.

| Saved artifact | SHA256 |
| --- | --- |
| `atx-vol-quote-bench.exe` | `e9c21ea937d16eb3c63d459a16057aaab2d6e669efcefc04e59c5e241e0d58a2` |
| `atx-vol-american-markov-audit.exe` | `1e2649346c8ef4d54fdac9563567f289be2c7e64d10faee7796f4ddf9eaca6f2` |
| `markov-fit-tests.xml` | `b93a24a000e8f85fdc22a49de691df852951fbc04dc6db55320e74bba822d814` |

Quote binary hash matches the three current benchmark receipts. Saved binaries
still require the Release vcpkg DLL environment; source branches and JSON
receipts are authoritative. Fitter XML was copied from the released correction
pool cache before reuse. No source data or labels were copied into git.

The branch copy necessarily precedes its own release; the main discovery copy
includes the final root documentation SHA, lease release and goal status.

## Final independent-reference checkpoint

Agent native_policy stopped and committed three new files as
**`8f73464d4647b5574915508492b675ddd2b66005`**:

- `atx-vol/tools/american_reference_audit.py` (487 lines)
- `atx-vol/python/tests/test_american_reference_audit.py` (222 lines)
- `atx-vol/sprints/2026-10-16-american-reference-audit-handoff.md` (157 lines)

The detailed lane note is accessible using `git show <sha>:<path>` and is also
copied into main's handoff directory for discovery. The implementation is not
integrated. It introduces no production dependency: QuantLib is isolated in
the external reference virtual environment. Lane validation: **19 tests passed
in 0.87 seconds**, Ruff passed, staged diff check clean. Parent has read the
handoff and verified the commit, not conducted the fresh source review.

The tool assembles QuantLib FdmBlackScholesOp/FdmBlackScholesMesher/Fdm1DimSolver
with exercise/event conditions, Douglas stepping and implicit damping. Four
tests match the stock FdBlackScholesVanillaEngine to approximately 1e-12,
including negative rates/carry. Other checks cover independent Black European
values, exact rational dual-clock maps, terminal cash payoff identities,
pre-dividend American exercise, paused variance, schema/cache/bounds/refusals,
global evaluation-date restoration and output collision protection.

Piecewise integrated funding/yield/variance and cash events map to synthetic
Actual365Fixed dates with exact rational decimal endpoint ratios. Zero-cash
entries force coefficient-boundary stops. A bounded representable-variance
adapter keeps paused variance exactly flat through QuantLib's vol interface.
Local vol and zero-total-variance inputs are explicitly unsupported; use the
existing exact deterministic reference for the latter. Cash uses QuantLib's
finite positive stock floor and jump interpolation. Nonpositive deterministic
cash forwards are refused because the log mesher cannot safely represent them.
Wider-domain probes assess but do not rigorously bound this cash approximation.

**Completed refined ATM evidence:**
`C:/atx-cache/quote-benchmark/american-reference-20261016-atm-refined-v1.json`,
SHA256 `074d33d1b7d217efcc03ab043b85bc3f917fdde8b9e5ab7f818901c7dc7db5e2`.
24 rows, **23 observed stable / 1 unresolved**. This run completed before the
stop handling and used final code without overlapping edits. Source-only ATM
input was derived from the full schema2 artifact and embeds its parent hash:
`markov-audit-20261016-atm-only-reference-input.json`, SHA256
`22ef7dd12d72e3352fccd7da2a212da7a62b8cdeb1c1a8ed0a1234f88e81465a`.

| Exact-ATM contract, default65 source | Markov price | FD reference | Largest observed refinement change | Status |
| --- | ---: | ---: | ---: | --- |
| martingale call | 3.787170451090 | 3.987761532638 | .000001095987 | observed stable |
| martingale put | 3.787170451090 | 3.987761419118 | .000000753696 | observed stable |
| short_low_vol call | .058764582935 | .084140435244 | .000000023548 | observed stable |
| short_low_vol put | .001257913867 | .032239718069 | .000001965890 | observed stable |
| negative_rate call | 7.300089614446 | 7.314862440662 | .000127985205 | **unresolved** |
| negative_rate put | 10.111471790358 | 10.107548143010 | .000001629041 | observed stable |
| negative_rate_and_yield call | 9.951483393576 | 9.950065849861 | .000003737658 | observed stable |
| negative_rate_and_yield put | 7.703925259926 | 7.720542585116 | .000087561713 | observed stable |
| clock_pause, expiry 1 put | .660011900818 | 1.191252553521 | .000001610264 | observed stable |

Negative-rate call last joint change .000127985205 exceeds its .000107314862
tolerance; extra time-probe change is .000061458810. Do not report convergence
or loosen tolerance to force a pass. Dated/near-cash rows also completed with
observed-stable status; consult their numerical values and caveats in the JSON.
The 24-row overall source/reference MAE is .16327028415, max gap .53124065270.
These are synthetic accuracy diagnostics, not vendor-label benchmark metrics.

The completed command (do not overwrite the existing output) was:

```powershell
C:/atx-cache/american-reference-venv/Scripts/python.exe atx-vol/tools/american_reference_audit.py --input C:/atx-cache/quote-benchmark/markov-audit-20261016-atm-only-reference-input.json --output C:/atx-cache/quote-benchmark/american-reference-20261016-atm-refined-v1.json --time-steps 800 --space-nodes 1600 --levels 3 --absolute-tolerance 0.0001
```

This reaches 3200 time steps / 6400 space nodes jointly, then separate
6400-time / 12800-space probes. Reported refinement estimates are not rigorous
uncertainty bounds. Tested environment: Python 3.12, QuantLib 1.44, pytest 9.1.1,
Ruff 0.17.1. Official source citations are pinned to QuantLib v1.44 in the note.

**Stopped full panel:** exec session 11776 terminated, exit 1, last progress
1000 / 1296 unique references. The expected
`american-reference-20261016-full-v1.json` **does not exist**. The current tool
serializes only at completion and lost in-memory intermediate numerical results.
Do not claim a full-panel pass or a saved partial numerical checkpoint. Refined
ATM session 52131 completed exit 0. Agent verified no reference Python process
remains; checked stopped PIDs 22464/13696 and completed PIDs 20360/22340.

Known follow-ups before another long reference run:

1. Capture executing tool/source hash at startup/import. Currently it hashes
   the file at the end; do not edit source during execution. The refined
   completed output tool hash is
   `43446dd13e3c0149099f0d454dccbfd1515873ae7eae14cfb6ce67d5cacdc796`.
2. Pin or qualify QuantLib versions explicitly. Actual version is recorded but
   versions other than the validated 1.44 are not rejected.
3. Add durable per-reference checkpointing before rerunning the full panel.
4. Refine the unresolved negative-rate call; preserve strict status semantics.
5. Independently review exact source/conventions before using this as a gate.

Reference clean release confirmed:

```text
RELEASED pool=pool-34 run_id=vol-20261016-reference
```

Pool-34 reset to dd539657. All three agents are stopped; no agent-owned build,
fit or benchmark remains running. Root next preserves documentation and releases
pool-35. No new implementation was started after the user's stop request.
