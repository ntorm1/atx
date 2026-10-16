# ATX-VOL next-parent handoff — user-requested pause

Prepared 2026-10-15, approximately 16:25 America/New_York / 20:25 UTC.

**Historical checkpoint.** Read
[the newest next-parent handoff](2026-10-16-common-model-next-parent-handoff.md)
first. Its branch, integration state, benchmarks and shutdown receipts supersede
the current-state instructions below.

## Read this first

The user explicitly said: **“stop here and write detailed handoff file for next parent agent.”**
Development has stopped. Do not interpret this handoff as permission to resume
before the user resumes the task. The long-running goal is paused, not complete.
All implementation was preserved on branches. No push, PR, deployment, or live
trading action occurred.

**The implementation is NOT merged into `C:/atx/main`.** Main remains at
`e696e208ef622aeb8a9ffbcc4bd3e4ea16a74548`; this handoff is placed in that
workspace so the next parent can find it. The main integration branch is
`feat/vol-benchmark-20261015`, code checkpoint **`ae50b8f0`**. Read that branch,
not the old main checkout, to assess the current implementation.

Two important pieces are separate from the integration branch:

1. **Completed, not integrated:** `9e0e7451fc227621a0c7264131399bce7ebe3c12`,
   exact PWL American rollback on the shared Markov model, from
   `feat/vol-quote-slice-20261015`.
2. **Native policy routing WIP:** the `benchmark_design` agent was implementing
   the JSON policy loader and native benchmark routing. See the final shutdown
   receipt at the end of this document for its preserved commit and status.

**Known temporary integration mismatch:** Python `quote_benchmark.py run` now
defaults to `--policy auto`, but the native policy-routing change has not yet
been integrated. The old native binary only accepts `--curve`. Until the native
change is integrated and validated, use an explicit `--curve essvi` for any
resumed controlled experiment. Do not mistake this intermediate mismatch for a
completed end-to-end policy implementation.

The final root build completed successfully at the pause, but the newly added
batch-pricer tests and integrated policy tests were **not run** after that build.
Do not claim a fresh all-green gate for checkpoint `ae50b8f0`.

## User requirements and authorization

- Build `atx-vol` into a modular, reusable, high-performance American equity
  options fitting, pricing and analytics library suitable as the eventual
  backbone of HFT options market making / relative value trading.
- Optimize against SpiderRock `srPrc` and `srVol` using the Downloads history
  parquet as a benchmark. Those values are **evaluation labels only**.
- Runtime inputs may include contract data, raw bid/ask, underlying bid/ask,
  dividends, clocks, rates and supplied carry. No vendor fair values, vendor
  bid/ask IVs, ATM vol, Greeks, `synSpot`, or vendor model flags as predictors.
- Explicitly use sub-agent-driven development. User explicitly rejects TDD:
  implement first, then meaningful numerical and boundary validation.
- Long-running autonomous iteration; do not stop for questions during resumed
  work. The present stop request supersedes that earlier persistence request.
- No backward compatibility obligation. Large breaking changes and deletion of
  outdated code are authorized when they improve the design.
- **All valid fits must enforce no-arbitrage requirements.** A low-error fit
  that violates mandatory admission is a failure, never a publishable fit.
- **Different symbols need different configurations and parameterizations.**
  A wide, sparse small cap cannot be fitted with the flexibility and observation
  certainty used for SPY. Explicit profiles and symbol overrides are required.
- Use relevant primary web research; never claim “world’s fastest” from the
  limited current evidence. The objective remains unfinished.

## Repository workflow, tools and environment

- Windows PowerShell 5.1, repository `C:/atx`, clang-cl 18, C++20, strict `/WX`,
  CMake/Ninja/LLD, ccache, shared vcpkg under `C:/atx-cache/vcpkg_installed`.
- Read `CLAUDE.md`, `.agents/cpp/agent.md`, `atx-vol/CLAUDE.md`, and the vol-DAG
  workflow before development. These were read earlier in this session. The
  user's explicit no-TDD instruction overrides the local TDD instruction.
- User AGENTS requests codebase-memory graph discovery first. No graph MCP
  tools were exposed in this session; `rg` was the justified fallback. Check
  tool availability again rather than assuming the next session lacks them.
- Native subagents were used because the older repository broker was not
  exposed. All used isolated **leased pool worktrees**. Never raw `git worktree
  add`, do not steal another lease, and do not reset a branch containing work.
- Always build through the **absolute** path to that worktree's
  `scripts/atx-build.ps1`, with explicit preset, jobs and target. No raw Ninja
  or raw `cmake --build`, no habitual full-graph build.
- Targeted CTest/direct focused GTest runs were used, not the legacy oracle
  workflow. No old oracle holdout was inspected. No clang-format/clang-tidy
  workflow was run; preserve repository style by editing directly.
- A lease may succeed and then its default `dev` configure fail because of
  unrelated optional project directories. The lease remains held. Configure
  the isolated vol preset rather than assuming acquisition failed.
- Lease release resets the physical pool checkout to its frozen base, while
  preserving the feature branch. **Released pool paths are not authoritative
  source snapshots. Use branch commits.** Build artifacts may remain there.

### Worktree/branch inventory at shutdown

| Lane | Durable branch / important commit | Former worktree | Lease |
| --- | --- | --- | --- |
| Root integration | `feat/vol-benchmark-20261015`, code `ae50b8f0` | `C:/atx-wt/pool-33` | `vol-20261015-root`; final release receipt below |
| Quote profile catalog | `feat/vol-quote-policy-20261015`, tip `6acff175`; catalog `5788d6a2` | pool-34 | `vol-20261015-policy`, released |
| Shared Markov baseline | `feat/vol-american-markov-20261015`, `b27a4d80` | pool-35 | `vol-20261015-markov`, released earlier |
| Native policy WIP | see final receipt below | pool-35 | new native-policy lease; final receipt below |
| PWL algebra/surface | `feat/vol-quote-slice-20261015`, `9e0e7451` | pool-36 | `vol-20261015-slice`, released |

The catalog branch also cherry-picked the Markov baseline as `6acff175` before
the stop. That is not a new local-vol implementation. **No local-vol extension
code was started.** Pool-34 reset to `69915708` upon release; pool-36 reset to
`e696e208`. No builds remain in those lanes.

On a resumed task, acquire a new uniquely named branch from the integration
branch tip through `scripts/lease-worktree.ps1`, with a new durable heartbeat.
Do not simply edit a released pool directory. Example shape (choose unique
names and inspect actual current branch/lease state first):

```powershell
powershell -NoProfile -File C:/atx/scripts/lease-worktree.ps1 `
  -Branch feat/vol-resume-<unique> -Base feat/vol-benchmark-20261015 `
  -Agent parent -RunId <unique> -HeartbeatId <unique> -MaxPool 40
```

## Integration commit map

The following are already on `feat/vol-benchmark-20261015`, oldest first:

| Commit | Change |
| --- | --- |
| `cd1b3979` | Vendor-independent American quote-slice API/implementation |
| `1db40645` | Quote clocks and independent terminal-cash validation |
| `399e70d4` | Native quote-only benchmark, isolated presets, initial sprint |
| `11cbd8a3` | Streaming data adapter and label-isolated scorecard |
| `9a35d81c` | Benchmark provenance/output guards |
| `1c636aeb` | Contract identity, adjusted deliverable refusal, file alias guards |
| `87029a0e` | Preserve per-strike supplied carry in fit/evaluation |
| `e4c90a0e` | Paired comparison and raw input strata |
| `9a10c8c2` | Native runner forwards per-contract carry |
| `913d741e` | Quote quality, uncertainty weights and caps |
| `ac2eecde` | Certified fast dense-IV root bracket |
| `10a830da` | Return certified Newton root directly |
| `370e74fa` | Immutable bounded American price slice |
| `524bf6fd` | Close generic curve no-arbitrage admission bypasses |
| `69915708` | Specialized nondividend American price relations |
| `65fc3b28` | Global convexity/vertical-spread tolerance checks |
| `58e71e47` | Final strike and cross-expiry price admission in fitter/runner |
| `d7c7e109` | Shared positive Markov model (agent original `b27a4d80`) |
| `21ca9177` | Convex PWL strike algebra (agent original `8fbe3396`) |
| `7e303f60` | Python policy provenance + Markov/PWL build registration |
| `1a9a4b24` | Owned raw-liquidity policy catalog (agent original `5788d6a2`) |
| `ae50b8f0` | WIP batch Markov pricer, tests, policy/build registration at pause |

Do not cherry-pick the original agent commits again when an integration
equivalent is already listed. Unintegrated `9e0e7451` adds only three new files;
the root must add its source/test to CMake after cherry-picking.

## Implemented library components

### Quote-slice fitting

Files: `include/atx/vol/api/fitting/quote_slice.hpp`,
`src/fitting/quote_slice.cpp`, `tests/quote_slice_test.cpp`.

- The API knows nothing about Parquet or SpiderRock column names.
- `QuoteSliceMarket`: spot, variance years, calendar years, rate, continuous
  yield, aggregate dividend PV. Funding/carry use calendar time; variance uses
  variance time. The constant clock ratio does not model intraday sessions.
- `QuoteSliceQuote`: strike, side, bid, ask, optional contract continuous yield.
  Carry curve owns sorted log-strike knots, interpolates carry, has flat edges,
  and rejects conflicting carries at one strike. Matched call/put carries agree
  in the inspected data. Common reference forward is used for smile coordinates.
- ContinuousYield refuses separately supplied cash PV. AggregatePresentValue
  is an explicit **escrowed-PV equivalent-yield approximation**, preserving
  `(S-PV)*exp((r-q)*calendar_T)`; it cannot recover exercise decisions at unknown
  cash dates. DatedCash uses the existing explicit-cash lattice for inversion
  and repricing, with supplied dates and the constant clock-ratio mapping.
- Point observations require `0 < bid < ask`. Bidless quotes are not forced
  toward half the ask; currently they are not interval constraints either.
  Contracts rejected as fit targets remain eligible for predictions/coverage.
- Current quality defaults: max spread/mid .60, max spread/Black-vega .05,
  min vega weight 1e-6, max inverse-uncertainty weight 1000. These are tunable
  policy choices, not no-arbitrage tolerances.
- American IV extraction -> normalized European fitting observations -> selected
  curve family -> American reprice. The intermediate curve is **not** a final
  American no-arbitrage serving certificate.
- Final admission evaluates both calls and puts at every distinct valid input
  strike, including rejected quote targets, then constructs AmericanPriceSlice.
  Failure produces `ArbitrageFailure` and no valid prices. Special q=0/no-cash,
  r>=0 checks are applied where their preconditions hold.
- `fit.prices->price(K, side)` is the bounded allocation-free price-only path.
  `evaluate_quote_slice` refuses outside the certified domain. At input knots
  it uses stored prices; between knots it uses canonical PWL prices and then
  re-inverts American IV. Off-node IV lookup therefore costs a root solve.
- No claim of smooth strike gamma from piecewise-linear price interpolation.

### Final American price admission

Files: `api/fitting/american_price_slice.hpp`, corresponding source/test.

- Immutable owned call/put nodes, finite positive strictly increasing strikes,
  at least two nodes. Fixed tolerance `1e-8 + 1e-9*max(spot,strike)`; callers
  cannot disable or loosen it.
- Checks intrinsic/upper bounds, conservative affine cross-side inequalities,
  global monotonicity, vertical spreads and global convex-hull deviation.
- Global checks prevent material concavity from accumulating through many tiny
  locally tolerated violations. The validator refuses; it does not project.
- `check_american_calendar` checks both sides at the union of PWL knots over
  the overlapping domains. Every pair of expiries is needed when strike domains
  differ. Disjoint domains are vacuous; caller establishes identity/time order.
- `check_nondividend_american` adds nondividend r>=0 conditions: European call
  lower bound and slope limit, A>=E, Legendre-type condition, and a scaled-strike
  upper bound over the available domain. Empty relevant overlap is Unavailable.
- **Necessary finite-domain price constraints are not a general proof that an
  arbitrary cash/carry American surface has a joint optimal-stopping model.**
  This gap motivated the constructive Markov/PWL components below. Keep that
  distinction explicit; do not relabel sampled/generic curve checks a full proof.

### Generic curve gate repairs

Files primarily `src/fitting/vol_curve.cpp` and its public header.

- LinearVariance now checks derivative jumps; nonflat flat-tail linear variance
  often refuses correctly. ConvexDense's unvalidated variance-wing fallback
  and cache were removed; failed IV inversion returns invalid instead.
- Dense fitting enforces the lower slope bound. Spline checks its actual final
  post-calendar curve, cubic extrema/positivity, tail jumps and hard cap.
- eSSVI residual hinge/clamp derivative jumps and full quoted ranges are checked.
  Warm SVI refit delegates to the full cold gate; warm paths cannot bypass it.
- Smooth density checks remain sampled. Final American price admission is the
  serving gate for the current quote fitter. 90 targeted tests passed in the
  implementing lane; these changes are already integrated.

### Dense-IV optimization

The direct Newton candidate is accepted only after two independent reprices
certify a narrow near-machine-IV root bracket with sign margins. Rounded or
low-time-value wings fall back to the original production bisection. Returning
the certified root directly avoids replaying obsolete 43-step arithmetic.
The old environment switch forcing 64 iterations was not used as the baseline.

Numerical evidence: 807 finite results from an 840-case grid, 33 expected domain
refusals. Against an independent 80-step bisection: max IV difference 4.55e-12,
max Black price difference 4.47e-9, max scaled analytic Greek difference 2.13e-9.
For 24 American comparisons: max price difference 2.85e-14, scaled Greek
difference 4.56e-8. 66 targeted tests and changed-TU PCH-off compilation passed.

### Symbol/liquidity policy catalog

Files: `api/fitting/quote_fit_policy.hpp`, source and 10-case test.

- `QuoteFitExpirySnapshot` borrows one market and quote span per distinct expiry
  of one symbol at one exact snapshot. `quote_fit_liquidity_features` derives
  row/contract counts, near-money usable strike counts, bidless fractions and
  median relative spread from raw quotes only.
- Duplicate valid contracts conservatively combine min bid/max ask; crossed,
  locked, invalid quotes do not inflate usable counts. Calls and puts at one
  strike count once for identifiability. Duplicate calendar maturities refuse.
- The catalog owns complete immutable configs, including a deep copy of any
  borrowed spline grid. Resolved shared_ptr profiles survive original inputs
  and catalog destruction.
- Exact case-sensitive symbol override maps to a named profile. There are no
  built-in ticker mappings or SPY special cases. Overrides cannot bypass quote,
  point count, strike or calendar admission.
- Built-in **uncalibrated** profiles: dense uses ConvexDense/min12, ordinary
  uses residual-free three-parameter eSSVI/min6, sparse uses eSSVI/min4 with
  different uncertainty and robust-loss settings. More flexible families demand
  more observations. LinearVariance is refused as a profile risk-fit family.
- Default raw routing near-money band .4 log-moneyness; dense median strike
  count >=24, median relative spread <=.08, near-money bidless fraction <=.10.
  Sparse triggers include median count <6, relative spread >=.60 or bidless
  fraction >=.50. Read the implementation for exact precedence and defaults.
- Source/test passed 10 focused Debug cases and PCH-off compilation in pool-34.
  Root compiled their integrated target at pause; root has not run that new
  integrated target since adding the catalog.

### Constructive shared American model

Files: `api/pricing/american_markov.hpp`, source and 9-case test.

- `AmericanMarkovInterval` currently has cumulative calendar/variance endpoints,
  rate, continuous yield, volatility, cash dividend at end. Endpoints are expiry
  prefixes of one common model. Volatility is currently constant by interval.
- State coordinate `X=S/G`, `G=exp(integral(r-q) d calendar_time)`. On nonuniform
  adjacent spacings h-/h+, local variance increment v=sigma² X² dVariance gives
  p-=v/[h-(h-+h+)], p+=v/[h+(h-+h+)], p0=1-p--p+. Time subdivision guarantees
  nonnegative probabilities. Discount is positive even with negative rates.
- Zero/upper X boundaries are absorbing for diffusion; upper actual S still
  follows G and cash jumps. Initial spot must be an exact grid state.
- Cash maps states to max(X-D/G,0), coalesces zeros, and is deterministic on the
  shifted grid. Limited liability means paid_cash=min(D,S_before). Exercise is
  available pre/post cash; expiry endpoint payoff is post cash. These explicit
  model conventions must not be disguised as an unbounded continuous model.
- Model owns immutable layers, positive sparse transition rows and expiry
  indices. Scalar `price_american_markov` uses caller-prepared two-buffer
  workspace; successful pricing allocates nothing.
- Config defaults: 65 generated nodes, upper multiple4, max calendar/variance
  step1/52, CFL.9, max1025states, 8192transitions, 1m stored rows. Hard caps:
  4097states, 65536transitions, 8m rows. Boundary mass diagnostics are exposure
  diagnostics, not rigorous price error bounds.
- Nine tests pass Debug and root Release: positive rows/moments, clocks,
  independent European limit, negative-rate deterministic cases, terminal cash,
  limited liability, scaling, strike shape and shared-prefix calendar order.
- **Finite Bermudan model**, not yet a demonstrated accurate approximation for
  all continuous American contracts. Grid/time/domain convergence is required.

### PWL strike algebra and shared-model surface

Integrated primitive: `api/pricing/pwl_strike.hpp`, source, 8-case test.
Unintegrated surface: `api/pricing/american_markov_pwl.hpp`, source/test in
commit `9e0e7451`.

- ConvexPwlStrikeFunction owns finite-domain positive-strike knots. Supports
  payoff, nonnegative weighted sums, and maximum with intrinsic by inserting
  every representable affine crossing. Preserves knots, has no extrapolation,
  resampling, silent compression or configurable convexity tolerance.
- Global hull deviation must be within 64*epsilon*max(1,max_node_value).
  Unrepresentable near-endpoint crossing returns explicit failure.
- Resource caps bound terms, input/output nodes and scratch. Queries are
  allocation-free binary search plus affine interpolation.
- Shared rollback surface owns the exact source model by value and call/put
  PWL functions for each expiry prefix. Positive destinations only contribute;
  zero-probability terms are omitted exactly. Source model remains exposed
  through const access for scalar comparison.
- Surface limits also cover peak live knots, total generated knots, total
  input-knot work, operator count and expiry count. No silent budget fallback.
- Agent reported 13 passing tests: 8 primitive +5 integration. Integration
  covers 801 off-grid strikes against independent finite distribution, dense
  off-node scalar agreement for three expiries with cash/negative/changing
  rates, convexity/intrinsic/monotonicity and every calendar pair, cash wipeout,
  resource limits and moved snapshots. Root has not reviewed/integrated this
  last commit; register its source and test after cherry-picking.

### Root batch-pricer WIP (checkpoint ae50b8f0)

Files: `api/pricing/american_markov_batch.hpp`, source and 3-case test.

- One side and expiry with arbitrary ordered strikes. State-major buffers with
  strikes contiguous permit vectorization through the same three-transition
  operator. Compile-time side/exercise specializations remove inner branches.
- Noncopyable movable caller workspace; prepare allocates two bounded buffers.
  Max65536options, max16,777,216 double values per buffer. Successful call has
  no allocation; output copied only after all rollback values are finite.
- Input and output strike/value spans may overlap. Invalid/moved-from or
  undersized workspace fails; empty batches are allowed with a valid model.
- Tests written after implementation cover scalar agreement at every side,
  style and expiry, odd tails, unsorted/duplicate/zero strikes, overlapping
  input/output, invalid data and moved workspace/model handling.
- Initial compile caught a missing aggregate initializer in the test; fixed.
  Source and tests now compile Release `/WX`; **tests not executed yet** because
  the user stopped the task. No throughput measurement exists. Independent
  review, focused run, PCH-off compilation and performance comparison remain.

## Native benchmark and adapter

### Current integrated native runner

`atx-vol/tools/quote_bench_main.cpp`, target `atx-vol-quote-bench`.

- Closed raw CSV allowlist; any label/unknown field rejects. Canonical numeric
  expiry/snapshot identity prevents alternate spellings splitting one surface.
- Refuses mixed identities/clocks within a chain, duplicate strike/side, missing
  eligible identity. Adjusted OCC roots ending1-9 are `unsupported_deliverable`
  because the file lacks share/cash basket details; never assume standard100.
- Median spot/rate/reference market within a chain; per-row sdiv forwarded.
  Reports underlying spot dispersion. Exact snapshots never coalesce.
- Fits and reprices, then groups underlying/root/exact snapshot and checks
  **every expiry pair** on overlapping canonical price domains. Both offending
  chains are refused before output, all prices/vols blank. Duplicate expiry or
  equal calendar maturity is `ambiguous_expiry`, with explicit counts.
- Protects input/output/report aliases including hardlinks and Windows
  case-folded canonical paths. `--max-rows` errors rather than truncating.
- Outputs row_id, predicted_price, predicted_vol, status, chain_id, quote_status,
  admitted; policy WIP adds resolved_profile, policy_source, actual_curve.
- Fixed-curve CLI exists. Policy defaults/loader/routing are separate WIP below.

### Streaming Python benchmark

`atx-vol/tools/quote_benchmark.py`; standalone tests under `atx-vol/python/tests/`.

- Arrow projection/batched preparation, physically separate `quotes.parquet`,
  `quotes.csv`, `labels.parquet`. Source date is exact snapshot; selection time
  buckets only filter. Raw spot=(uBid+uAsk)/2; separate source years/yearsC.
- Whole-underlier seeded BLAKE2 split80/20 development/validation. All roots,
  snapshots and expiries of an underlier remain in its split.
- Hashes source, adapter, quote inputs, label files, native binary, predictions;
  new output paths only. Receipts record exact command. Native sees only CSV.
- Scorer left joins from all eligible rows, reports coverage/failures and label
  availability alongside price/vol errors. Reference outside NBBO is diagnostic,
  never a selection filter. Unidentifiable vols remain explicitly accounted for.
- Compare uses exact independent price and vol intersections, arm coverage and
  paired improvement/worsening fractions. Reduced coverage is not accuracy gain.
- Raw strata include adjusted root, moneyness, cash-PV and liquid-core cuts.
- New wrapper supports mutually exclusive `--curve` or `--policy auto|PATH`.
  Extra args cannot replace either selection. Custom JSON size <=1MiB, complete
  content+SHA recorded and rehashed after run. Native stdout is now parsed as one
  JSON object and embedded in receipt. Binary/input unchanged checks remain.
- 38 standalone Python adapter/provenance tests pass after these changes; Ruff
  passes. Earlier native+adapter combined gate was42 tests (33+9), before the
  policy wrapper changes. Do not conflate these counts.

### Native policy WIP schema agreed with agent

Closed JSON v1:

```json
{
  "schema_version": 1,
  "profiles": [
    {"name": "dense", "base": "dense"},
    {"name": "ordinary", "base": "ordinary"},
    {"name": "sparse", "base": "sparse"}
  ],
  "symbol_overrides": [{"symbol": "SPY", "profile": "dense"}],
  "regimes": {}
}
```

This is a syntax example, not an empirically selected SPY setting. Missing
profiles means built-ins; supplied list replaces the catalog. Each profile base
must dense/ordinary/sparse. Optional fields: curve,min_quotes,quality,parametric,
convex,spline,side_policy(all|otm),atm_log_band,spread_floor,min_time_value,
iv_tolerance,max_iv_iterations. Regimes uses the catalog's field names.
No no-arbitrage-disable knobs. Unknown/duplicate keys, nonfinite/range-invalid
values reject; bounds proposed1MiB,64profiles,4096symbol overrides.

`--policy auto` default; explicit `--curve` fixed experiment mutually exclusive.
Global quote quality/minimum knobs require explicit `--curve`; profiles own
their settings in policy mode. Global dividend convention remains explicit.
Resolve once per underlying/root/exact snapshot across consistent expiry chains.
Report schema2 with expanded policy definitions, per-snapshot raw decisions,
actual families and per-profile counts. Stdout remains exactly one JSON object.
Root added `nlohmann_json::nlohmann_json` linkage already. Agent owns native
main, new private `tools/quote_bench_policy.hpp`, and native Python CLI tests.
Final WIP receipt below determines what was actually completed/validated.

## Data and measured benchmark results

Source: `C:/Users/natha/Downloads/OptionIntradayHist_30min_2026-10-14.parquet`.
1,916,931,890bytes,34,011,291rows,280rowgroups. American31,889,698;
European2,121,593. One day does not establish multi-day/live readiness.

No heldout validation labels were inspected. All exploratory numerical fit
comparisons below use hash-selected **development** underliers at10:30NY.

External artifact root: `C:/atx-cache/quote-benchmark/`.

| Cohort/run | Evidence |
| --- | --- |
| `20261014-dev` |20development underliers;10,948selected /7,308eligible |
| `20261014-dev200` |200development underliers;72,348selected /63,942eligible |
| dev20 pre-carry |6,896served,111fit;6,799price-label rows; MAE .282430 vs midpoint .333089; vol MAE993.77bp |
| dev20 per-strike carry |same-population MAE .282881, vol994.21bp; slightly worse error, correct preservation of inputs |
| dev200 `essvi-release-carry` |62,412served97.607%,1038/1138fit;61,758price rows; MAE .277666 vs midpoint .350695; vol1344.54bp |
| dev200 liquid-core baseline |vol293.99bp; price .3546 versus midpoint .3341, so model not winning on this subset |
| dev200 `essvi-release-quality` |25,276served39.5296%,203fit,935insufficient,7,368admitted |
| quality exact paired price population |25,165rows: .283341 baseline -> .266394 quality |
| quality exact paired vol population |25,276rows:432.93 ->524.98bp, WORSE; not an overall improvement |
| `essvi-release-arb-v1` |10,300served83fit;4strike failures,116calendar-rejected chains,935insufficient;1244expiry pairs |
| `essvi-release-arb-wide-v1` |19,722served298fit;9strike failures,315calendar rejections,516insufficient;18,158admitted,2806pairs |

The carry/quality baselines predate mandatory final price admission and are
development comparisons, not production-accepted fits. Arb-v1 runs predate the
later global-tolerance/nondividend improvements. Do not attribute their results
to the latest binary. The wide run used max-spread-to-mid0,max-spread-vol0,
max-weight1e6. Its paired score was not yet computed. The auto policy has not
yet been measured on this cohort.

Dev200 directory contains immutable prediction CSVs, `.csv.run.json` receipts,
timing JSONs, `essvi-release-carry-score.json`, `compare-quality-release.json`,
quotes/labels/manifests. Preserve all evidence, use new paths for future runs.

Adjusted roots caused59.92% of absolute price error on3.9% of the old dev20 rows.
Refusing missing deliverables is scope correctness, not a fitting accuracy win.
41/125standard chains have strike-dependent sdiv (max spread .005); matched
calls/puts share carry. IEF reference prices sometimes lie outside NBBO; leave
them in scoring and report the diagnostic rather than filtering them away.

### Dense-IV speed measurement

External saved binaries:

- `atx-vol-fitting-bench-bisection.exe`
- `atx-vol-fitting-bench-newton.exe`

Script `measure-dense-abba.ps1`; summary `dense-pinned-comparison.json`.
Four original runs total: bisection-pinned-0, newton-pinned-1,
newton-pinned-2, bisection-pinned-3, each JSON/stdout/stderr, five repetitions.
Do not claim four runs per arm; there are two per arm.

Quiet coordinated agent pause; Release AVX2, no fast-math, one fitting worker,
process affinitymask1 (logical CPU0). Fixture4097 dense IV queries,
F100,T.25,df.98,flat vol.22. CPU CV cutoff5%:

| Run | CPU mean us | CPU CV | Retained? |
| --- | ---: | ---: | --- |
| A0 bisection |10100.446 |6.299% |no |
| B1 Newton |6520.433 |3.769% |yes |
| B2 Newton |6458.333 |5.989% |no |
| A3 bisection |10107.422 |3.351% |yes |

Retained pair gives **1.550115x CPU speedup /35.49% less CPU** on this fixture.
Wall CVs7.61%,4.74%,6.61%,11.24%; no clean paired wall comparison. No tail,
whole-library, live/HFT or world-record claim is supported. Saved executables
need DLL directories on PATH (see script); an initial missing-DLL run generated
no measurements and was rerun with correct paths.

Bisection SHA256
`d75b73ff25d75b4f308ae91d8758f6de39035fc87ecfa9242737ce24d15be324`;
Newton SHA256
`278b6cc08d55033e8124127b057e827bf4e2a12c41c28155991141c17e396dd1`.
Additional preserved quote binaries: precarry,carry,quality,arb-v1. Their old
receipts remain intact even though the current root executable has advanced.

## Research and material unresolved model issues

1. **General American consistency:** an arbitrage-free European smile repriced
   through independent strike-specific American models need not be arbitrage
   free. Reproduced counterexample S100,r0,T1,sigma.2: q(100)=0 yields call
   7.965567, q(100.1)=-.005 yields8.191733. Higher strike costs more. Final-price
   regression now refuses this. The shared model is intended to eliminate the
   underlying inconsistency rather than merely reject many chains.
2. **Dividend PV convention:** SpiderRock docs use a forward approximately
   `S*exp((r-q)T)-PV*exp(rT)`; current aggregate quote approximation is
   `(S-PV)*exp((r-q)T)`. These differ when q!=0. Inspection of27,290positive-div
   dev200 rows gave implied discount times `-log(PV/D)/r` within calendar expiry,
   median fraction about.508, suggesting funding-discounted PV. This is evidence,
   not proof of exact semantics. Add an explicit convention enum/adapter after
   verifying; do not silently treat funding PV as escrowed stock PV.
3. **Cash solver:** existing discrete-dividend CRR rounds dates, has a low-vol
   transition-probability domain, and its quadratic cash extrapolation can have
   negative weights [8/3,-2,1/3]. Existing test PDE central drift/CN is also not
   automatically a positive operator. They are not a shared positivity proof.
4. **Uncertainty/sparse quotes:** hard filters greatly reduce coverage; wide
   and bidless quotes should become meaningful interval constraints with robust
   loss/regularization, not fabricated midpoint targets. Dense/sparse profiles
   are starting points; no heldout superiority has been demonstrated.
5. **Common-model carry:** use one underlying carry process in a constructive
   model. Contract-dependent vendor sdiv cannot become a different stock drift
   per strike if the objective is joint model existence. Reconciliation and
   reporting of this input convention still need design and empirical work.
6. **Accuracy/refinement:** explicit positive Markov time subdivision has CFL
   cost. Coarse uniform grids may have unacceptable pricing bias. Nonuniform
   grids/time/domain refinement need independent study before calibration can
   hide discretization error in local-vol parameters.
7. **Analytics/HFT:** no new complete surface Greeks/risk/update scheduler or
   live production qualification has been implemented in this work. Existing
   library analytics exist, but the new constructive path still needs compatible
   derivatives and clearly defined sticky/parameter dynamics.

Primary/reference sources already consulted (avoid re-deriving known scope):

- Andersen/Lake/Offengenden American pricing,
  DOI `10.21314/JCF.2016.312`; historical100kprices/s is not a current world record.
- QuantLib current `qdfpamericanengine.cpp`, numerical fast/accurate settings
  and existing repository AL implementation were reviewed earlier.
- Healy negative-rate American exercise boundaries:
  <https://arxiv.org/abs/2109.15157>.
- Cox/Hoeggerl American price consistency and model existence:
  <https://arxiv.org/pdf/1301.5467>, especially Theorems2.3/3.9 under their
  nondividend/interest assumptions.
- Itkin recent cash-dividend research:
  <https://arxiv.org/html/2510.18159v1> (page revision discussed Aug2026).
- De Marco/Henry-Labordere, local volatility from American options:
  <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2870285>.
- SANOS Jan2026:
  <https://arxiv.org/html/2601.11209v1>. Positive discrete local vol / smooth
  European marginal LP ideas are relevant, but lines97-99 explicitly leave
  American options for future research. Do not transfer its European guarantee
  to American repricing. The paper's “instantaneous” wording is not our measured
  performance standard.
- Andreasen-Huge implementation discussion by QuantLib implementer:
  <https://hpcquantlib.wordpress.com/2018/01/05/andreasen-huge-volatility-interpolation/>.
  Again, European forward interpolation differs from American Snell recursion.

### Agreed local-vol extension, NOT implemented

The fitting_audit agent reviewed this mathematically and was about to implement
when stopped. No source changes exist, so resume from design rather than hunting
for missing WIP.

- Append optional interval-local knot span with finite increasing
  `log_state_over_spot` and finite nonnegative `volatility_multiplier`.
- State-local sigma = interval baseline sigma * piecewise-linear multiplier
  in log(X/initial_spot), flat endpoint continuation, empty knots means1.
- Keep common carry G. Zero/upper absorbing boundaries need no log(0).
- Recompute actual local sigmas on each post-cash interval state grid. The
  diffusion grid is fixed within an interval, so precompute once there.
- Replace constant exit rates with sigma_i² X_i²/(h-h+), retain positivity
  subdivision, exact first moments and local second moments.
- Expose immutable per-interval per-state sigma metadata, referenced by
  transition interval index, rather than copying an entire sigma vector every
  step. No final public metadata interface was committed.
- Bound node/resource counts. Empty/constant-one profile must reproduce old
  operators exactly. Test positivity, moments, flat equivalence, cash-grid
  reevaluation, skew effects, strike/calendar shape and resource failures.
- This supplies smile flexibility while retaining one model. A bid/ask interval
  calibrator and symbol-specific local-vol parameter-count/regularization policy
  are subsequent work, not implemented or empirically selected yet.

## Validation, build commands and known pitfalls

Root presets: `vol-dev`, new `vol-rel-avx2` (Release IPO AVX2, no fast-math),
new `vol-hygiene` (PCH off), with isolated FetchContent directories. Only use
these through the worktree wrapper. Existing root build directories were
`C:/atx-wt/pool-33/build-vol-rel-avx2` and `build-vol-hygiene`.

Before pause:

- 37 root focused quote/price-slice Release tests passed at58e71e47.
- Changed `quote_slice.cpp` and native runner PCH-off object checks passed.
- Nine Markov tests passed root Release after d7c7e109.
- PWL primitive8 and shared surface combined13 passed agent Debug.
- Policy catalog10 passed agent Debug and source/test PCH-off.
- New Python adapter/provenance38 passed; Ruff clean.
- Final build of `atx-vol-american-markov-tests` and `atx-vol-quote-slice-tests`
  completed at pause (shell session17679 exited0). It includes batch3, Markov9,
  PWL8 in first target and quote17+price20+policy10 in second. **Do not assume
  their newly linked tests were executed.** No root shell build remains running.

Example targeted commands after resume, with actual leased path substituted:

```powershell
powershell -NoProfile -File C:/atx-wt/<pool>/scripts/atx-build.ps1 `
  -Preset vol-rel-avx2 -Jobs 2 build atx-vol-american-markov-tests atx-vol-quote-slice-tests

powershell -NoProfile -File C:/atx-wt/<pool>/scripts/atx-build.ps1 `
  -Preset vol-hygiene -Jobs 2 check atx-vol/src/pricing/american_markov_batch.cpp

ctest --test-dir C:/atx-wt/<pool>/build-vol-rel-avx2 `
  -R '^atx-vol-(american-markov|quote-slice)$' --output-on-failure

python -m pytest --noconftest atx-vol/python/tests/test_quote_benchmark.py -q
python -m ruff check atx-vol/tools/quote_benchmark.py atx-vol/python/tests/test_quote_benchmark.py
```

Standalone adapter tests load the Python script directly and do not use the
compiled atxvol extension. Plain pytest discovers an unrelated conftest and
correctly refuses an installed editable extension from `C:/atx` when working in
a pool. `--noconftest` is intentional only for these standalone script tests.
For actual extension tests, use the repository's `_ctest_pytest_driver.py` /
registered lane; do not bypass its contamination guard.

Native CLI tests require `ATX_QUOTE_BENCH_BINARY` set to the newly built binary
and suitable DLL paths. They skip if the env var is absent; **skipped is not
passed**. They are in `atx-vol/python/tests/test_quote_bench_cli.py`.

Do not run raw all-target builds. Do not run noisy performance comparisons
while agents compile. Independent calls can run concurrently, but build/edit
dependencies must remain sequential. Use proper PowerShell literals; do not
interpolate untrusted strings into shell source. All data/output stay external.

## Suggested continuation sequence once the user resumes

1. Read this handoff and inspect branch/lease state. Acquire a fresh lease from
   the integration branch. Preserve the visible handoff in main.
2. Run/review the just-built batch/pricing/policy tests, then PCH-off the new
   batch source/test. No batch performance claims until measured.
3. Cherry-pick/review `9e0e7451`; add `american_markov_pwl.cpp` and its test to
   CMake, run focused scalar/PWL consistency gates.
4. Integrate or finish the preserved native policy WIP from the final receipt.
   Validate strict JSON/alias/snapshot handling, fixed-vs-auto behavior,
   symbol overrides, profile provenance and the Python/native end-to-end path.
5. Run fresh dev20/dev200 fixed and automatic policies under identical latest
   mandatory gates. Report paired errors **and coverage** plus per-profile
   results. Do not reuse old-baseline timings as current-version measurements.
6. Continue common-model local-vol extension and reusable quote interval
   calibration, with one carry process and shared model across expiries.
   Profile-dependent model complexity/regularization belongs here as well.
7. Establish numerical refinement and independent continuous-American price
   evidence before attributing fit improvement to model rather than bias.
8. Resolve dividend-PV convention explicitly. Extend data only with authorized
   raw market inputs. Never introduce srVol/srPrc into runtime fitting.
9. Measure hot query/batch fit/update speed only after correctness; add useful
   analytics consistent with the chosen model. Freeze settings before untouched
   validation, then assess without selecting parameters on validation errors.
10. Eventually deliver a validated checkpoint to `C:/atx/main` after checking
    for intervening user changes. Nothing has been merged there yet. Continue
    the goal rather than declaring a complete HFT library from these modules.

## Final shutdown receipts

The code checkpoint remains `ae50b8f0`; any subsequent root commit is
documentation only.

### Native policy lane

- Preserved **unvalidated WIP**:
  `8d77c35599cd7b615f1b2da8d75377c422f1c55a` on
  `feat/vol-native-policy-20261015`.
- Only three files changed: `tools/quote_bench_main.cpp`, new
  `tools/quote_bench_policy.hpp`, `python/tests/test_quote_bench_cli.py`.
- The lane cherry-picked the catalog as `12c43053` (same as original5788d6a2,
  already integrated in root as1a9a4b24). **Cherry-pick only8d77c355 into root**,
  not its duplicated dependency.
- Configure completed exit0, shell session58763 closed. **No native compile or
  test was started.** Post-implementation CLI tests exist but have not run.
- Temporary CMake edits restored before commit. Root already has permanent
  catalog source registration and nlohmann JSON linkage; verify instead of
  adding duplicates. Agent reports `git diff --check` clean, but C++ style and
  independent review remain.
- Lease `pool-35`, RunId/Heartbeat `vol-native-policy-20261015`, released clean.
  Release reset the physical checkout to58e71e47. No ongoing lane process.
- Implemented guards include duplicate CLI flags, policy/output/report aliases
  including hardlinks, JSON depth16, file1MiB,64profiles,4096overrides, bounded
  iterations/nodes/grids and unknown/duplicate keys.
- Duplicate expiry day/calendar maturity is excluded **before** classification.
  Resolve once per underlying/root/exact snapshot across remaining consistent
  chains, then preserve strike/calendar gates.
- Known review points: `actual_curve` initially means selected family even on
  failure, then produced family when a curve exists; some zero per-profile
  counters are omitted; expanded `policy_definition` is provenance and is not
  accepted as an input-schema round trip. Existing calendar synthetic CLI test
  still runs default auto and may need explicit fixed curve to isolate intent.
- Added CLI checks cover snapshot routing, exact override/provenance, duplicate
  expiry exclusion, invalid schemas/duplicate keys/unsafe knobs/bounds, aliases
  and mutually exclusive flags. No development or heldout data read in this lane.

### Other lanes and root

- pricing_research: clean pool36 released;9e0e7451 preserves the completed PWL
  surface.13/13 combined focused tests passed. No running process or WIP.
- fitting_audit: clean pool34 released; catalog5788d6a2 preserved;10focused
  tests and PCH-off checks passed. Local-vol design remains unimplemented.
- Root: all code committed throughae50b8f0. Last build completed successfully;
  no build/test process remains. This handoff is copied into the integration
  branch as a documentation-only commit before clean root lease release.
- Main remains at the original base with this handoff as a visible local file;
  no feature branch has been merged into main. The next parent must resume
  from the preserved branch, not assume main contains the implementation.
