# Independent American reference audit: stop handoff

User requested stopping development and preserving the next-parent handoff on
2026-10-16. This checkpoint implements an independent numerical reference tool;
it does not change production pricing, fitting, configuration, or market labels.

## Git and lease

- Branch: `feat/vol-reference-audit-20261016`.
- Worktree while developing: `C:/atx-wt/pool-34`.
- Frozen base: `dd539657edcfe4d08847d79951be6030b3eeca70`.
- Agent: `native_policy`.
- RunId and HeartbeatId: `vol-20261016-reference`.
- Assigned files: `atx-vol/tools/american_reference_audit.py`,
  `atx-vol/python/tests/test_american_reference_audit.py`, and this note.
- Default dev configure failed after successful acquisition because unrelated
  repository components were unavailable; no C++ build was needed for this lane.
- The final collaboration receipt supplies the checkpoint SHA and clean release
  outcome. Reacquire a new lease before further work; do not assume pool-34 stays
  on this branch after release.

## Implementation

`american_reference_audit.py` accepts generic schema 1 or 2 Markov audit JSON.
It builds independent continuous-American targets using QuantLib finite
differences, not the atx Markov kernel. It reports each source contract, reference,
signed source-minus-reference error, convergence diagnostics, unsupported cases,
dependency versions, native module hashes, input hash, and tool hash. Source
prices never enter reference construction. Matching contracts across source grids
share one cached reference. American contracts are selected by default; excluded
European rows remain counted. No SpiderRock/vendor data was used.

The solver uses QuantLib's exposed `FdmBlackScholesOp`, `FdmBlackScholesMesher`,
`Fdm1DimSolver`, and vanilla step conditions, assembled like the stock
`FdBlackScholesVanillaEngine`. Douglas stepping with implicit damping provides
three jointly doubled grids plus separate time, space, and widened-domain probes.
The reported reference is the last joint grid. `observed_stable` means observed
changes, payoff checks, and any analytic anchor passed the specified tolerance.
It is **not** an exact value, rigorous error bound, or no-arbitrage certificate.

Piecewise funding, yield, variance-clock increments, and dated cash events are
mapped to a synthetic Actual365Fixed date axis while preserving integrated
coefficients and exact rational decimal endpoint ratios. Unrepresentable dates
are explicitly unsupported rather than rounded. Zero cash entries force rollback
stops at coefficient breakpoints. A bounded representable-variance adapter keeps
paused variance clocks exactly flat despite floating point reconstruction in
QuantLib's variance curve. Negative funding/carry use the same PDE operator.

Cash references retain QuantLib's finite positive stock floor and interpolation
at cash jumps. A wider domain probes this approximation; no rigorous boundary
error bound is claimed. Cash schedules whose deterministic forward becomes
nonpositive are refused because the stock QuantLib log mesher cannot represent
them safely. Local volatility and zero-total-variance cases are also explicitly
unsupported. The latter should use an exact deterministic reference.

## Environment, research, and validation

Isolated external environment: `C:/atx-cache/american-reference-venv`.
Installed QuantLib 1.44, Python 3.12, pytest 9.1.1, and Ruff 0.17.1. QuantLib was
not installed globally. The tested upstream source links are pinned to v1.44:

- [Vanilla engine assembly](https://github.com/lballabio/QuantLib/blob/v1.44/ql/pricingengines/vanilla/fdblackscholesvanillaengine.cpp)
- [PDE coefficients](https://github.com/lballabio/QuantLib/blob/v1.44/ql/methods/finitedifferences/operators/fdmblackscholesop.cpp)
- [Stock mesher](https://github.com/lballabio/QuantLib/blob/v1.44/ql/methods/finitedifferences/meshers/fdmblackscholesmesher.cpp)
- [Exercise/event ordering](https://github.com/lballabio/QuantLib/blob/v1.44/ql/methods/finitedifferences/stepconditions/fdmstepconditioncomposite.cpp)
- [Cash jump interpolation](https://github.com/lballabio/QuantLib/blob/v1.44/ql/methods/finitedifferences/utilities/fdmdividendhandler.cpp)

Implementation preceded tests. Final focused validation: **19 passed in 0.87s**;
Ruff passed. Exact commands from the leased worktree:

```powershell
C:/atx-cache/american-reference-venv/Scripts/python.exe -m pytest --noconftest atx-vol/python/tests/test_american_reference_audit.py -q
C:/atx-cache/american-reference-venv/Scripts/python.exe -m ruff check atx-vol/tools/american_reference_audit.py atx-vol/python/tests/test_american_reference_audit.py
```

Tests cover exact dual-clock/date maps, paused variance, four comparisons with the
stock QuantLib engine including negative rates/carry, independent European Black
values, terminal-cash payoff identities, pre-dividend American exercise,
short-low-volatility early exercise premium, unresolved tolerances, schema/cache
behavior, evaluation-date restoration, explicit unsupported inputs, limits,
duplicate JSON keys, and protection against overwriting an output/input file.

## Durable evidence and process status

All evidence files are outside Git under `C:/atx-cache/quote-benchmark/`.

| File | SHA-256 | Status |
| --- | --- | --- |
| `markov-audit-20261016-atm-v2.json` | `bfd5dba7ee1909e0d2acba83c22bc8ca1e4d170b74dddb2fb4040e94d96725f6` | Root's complete schema-2 source, 88 cases and 20,160 source rows |
| `american-reference-20261016-short-v1.json` | `03c7b237c43be5f1afd0aec28e8586ee836eb58031a9559c559493aed77a3189` | Complete: 72 unique contracts reused across 576 source rows; all observed stable at default tolerance |
| `markov-audit-20261016-atm-only-reference-input.json` | `22ef7dd12d72e3352fccd7da2a212da7a62b8cdeb1c1a8ed0a1234f88e81465a` | Derived exact-ATM default65 panel; contains parent source hash |
| `american-reference-20261016-atm-refined-v1.json` | `074d33d1b7d217efcc03ab043b85bc3f917fdde8b9e5ab7f818901c7dc7db5e2` | Complete: 24 rows, 23 observed stable, 1 unresolved |
| `american-reference-20261016-full-v1.json` | none | **Does not exist**: full panel stopped before final serialization |

The short panel was produced before the later paused-variance representation and
non-numerical bounds/status changes. Its single-interval constant-variance results
remain applicable; the finer ATM output uses the final code, tool SHA
`43446dd13e3c0149099f0d454dccbfd1515873ae7eae14cfb6ce67d5cacdc796`.

Full-panel exec session **11776** was terminated after last logged progress
**1000/1296**; exit code 1. Verified Python PIDs 22464/13696 were stopped. No partial
numerical JSON exists because the current tool writes only on completion. Its
partial progress is recorded here; numerical intermediate results were in memory.
Refined ATM session **52131** had already completed with exit code 0 before stop
handling; its launcher/child PIDs 20360/22340 no longer existed. No reference
benchmark remains running from this lane.

The complete finer ATM command was:

```powershell
C:/atx-cache/american-reference-venv/Scripts/python.exe atx-vol/tools/american_reference_audit.py --input C:/atx-cache/quote-benchmark/markov-audit-20261016-atm-only-reference-input.json --output C:/atx-cache/quote-benchmark/american-reference-20261016-atm-refined-v1.json --time-steps 800 --space-nodes 1600 --levels 3 --absolute-tolerance 0.0001
```

It uses joint grids through 3200 time steps / 6400 space nodes, then separate
6400-time and 12800-space probes. Its overall source-reference MAE is
0.16327028414990366; maximum absolute gap is 0.5312406527036704. Selected exact-ATM
results below are all default65 source prices; changes are observed refinement
estimates, not proven uncertainty bounds:

| Fixture/contract | Source | Independent reference | Largest refinement change | Status |
| --- | ---: | ---: | ---: | --- |
| martingale call | 3.787170451090 | 3.987761532638 | 0.000001095987 | observed_stable |
| martingale put | 3.787170451090 | 3.987761419118 | 0.000000753696 | observed_stable |
| short_low_vol call | 0.058764582935 | 0.084140435244 | 0.000000023548 | observed_stable |
| short_low_vol put | 0.001257913867 | 0.032239718069 | 0.000001965890 | observed_stable |
| negative_rate call | 7.300089614446 | 7.314862440662 | 0.000127985205 | **unresolved** |
| negative_rate put | 10.111471790358 | 10.107548143010 | 0.000001629041 | observed_stable |
| negative_rate_and_yield call | 9.951483393576 | 9.950065849861 | 0.000003737658 | observed_stable |
| negative_rate_and_yield put | 7.703925259926 | 7.720542585116 | 0.000087561713 | observed_stable |
| clock_pause expiry 1 put | 0.660011900818 | 1.191252553521 | 0.000001610264 | observed_stable |

The negative-rate call misses its 0.000107314862 tolerance because the last joint
change is 0.000127985205; its additional time-probe change is 0.000061458810. Do not
treat it as converged. Dated-cash and near-cash panel rows also completed with
observed-stable statuses; consult the JSON for all event expiries and cash-floor
scope. These results provide strong evidence that the coarse Markov state grid
under-resolves near-ATM option value; they do not certify a replacement solver.

## Next authorized continuation

1. Review/cherry-pick this checkpoint and preserve the independent audit role.
2. Capture the executing source hash at startup/import rather than at the end of
   CLI execution. Current `tool_sha256` hashes the file after the solve; do not edit
   the source during a run. The completed refined run did not overlap edits.
3. Pin or explicitly qualify QuantLib versions. The tool records actual versions
   but currently does not reject versions other than validated 1.44.
4. Add durable per-reference checkpointing before another long full panel; the
   interrupted full panel currently must start over. A fresh output path is
   mandatory. The stopped command used the full source above and default settings.
5. Refine the unresolved negative-rate call in time; do not weaken tolerance to
   manufacture a pass. Use the completed ATM panel to guide concentrated state
   grids or implicit positive operators in the separate native lane.
6. Coordinate numerical/performance runs with the parent: run durations above were
   collected alongside other work and are diagnostic, not throughput benchmarks.

No production changes, native build, market fit, or held-out label evaluation was
performed in this lane. User stop takes precedence over these continuation items.
