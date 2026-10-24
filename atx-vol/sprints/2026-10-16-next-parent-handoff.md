# ATX-VOL next-parent handoff - user-requested stop

**Historical checkpoint.** The task was subsequently resumed, advanced and
stopped again. Read
[the newest handoff](2026-10-16-common-model-next-parent-handoff.md) first;
its branch, integration state, numerical findings and shutdown receipts
supersede this document's current-state instructions.

Prepared 2026-10-16, approximately 02:45 America/New_York / 06:45 UTC.

## Start here: task is stopped, not complete

The latest user instruction is: **"stop here and write detailed handoff file for next parent agent"**.
Development stopped on that instruction. Do not resume implementation until the
user resumes the task. The goal must remain paused, not complete. Shutdown
receipts are appended below after preservation and lease release.

**The implementation has NOT been merged into `C:/atx/main`.** Main is still
`e696e208ef622aeb8a9ffbcc4bd3e4ea16a74548`. The visible handoff files in main are
local, untracked documentation. No push, PR, deployment or trading action occurred.

The authoritative integration branch is now **`feat/vol-resume-20261016`**,
code checkpoint **`168365044e0f0f1bfef596897085cef9711040a8`**. A later commit on
this branch contains this handoff only. Resume from that branch after acquiring
a new lease; the physical pool checkout is reset on release.

This document supersedes the current-state claims in
[the 2026-10-15 handoff](2026-10-15-next-parent-handoff.md), which remains the
historical record of architecture, earlier commits, benchmark results and research.
Read both. In particular, the old handoff's "batch tests not run", "PWL surface
not integrated", "local vol unimplemented" and "native policy unvalidated WIP"
statements have been superseded as described here.

Critical outstanding state:

1. Native policy routing is implemented and tested on a separate branch, **not
   integrated**. Python defaults to `--policy auto`, while the root native
   runner still accepts only fixed `--curve`. Do not mistake that mismatch for
   a working default end-to-end benchmark.
2. Interval local volatility is implemented and tested on a separate branch,
   **not integrated or independently reviewed by the parent**.
3. A synthetic pricing audit found **default-grid PWL numerical refusals and
   material discretization errors**. Focused tests passing does not establish
   usable production accuracy. Preserve mandatory no-arbitrage gates while
   fixing those issues.
4. No new SpiderRock dev200 scoring or heldout-label inspection occurred during
   this resumed turn. Earlier numerical benchmark results are historical.

## User requirements and scope

- Build `atx-vol` into a modular, reusable, high-performance American equity
  option fitting, pricing and analytics backbone for HFT market making and
  relative value. The goal is much larger than the current checkpoint.
- Use the Downloads option-chain parquet as a benchmark. `srPrc` and `srVol`
  are evaluation labels only, never model inputs or runtime configuration.
- Permitted inputs include contract identity, raw bid/ask, underlying bid/ask,
  rates, dividends, clocks, and supplied contract carry. Do not introduce vendor
  fair values, bid/ask implied vols, ATM vol, Greeks, `synSpot`, or model flags.
- Explicitly use sub-agent-driven development. The user explicitly rejects
  TDD: implement, then perform meaningful numerical and boundary validation.
- No backward-compatibility obligation; substantial redesign and removal of
  obsolete code are authorized when useful.
- **Every valid fit must enforce no-arbitrage.** Better label error cannot
  justify bypassing admission, dropping a violated constraint, or silently
  repairing a price after certification.
- **Different symbols require different configs and parameterizations.**
  A sparse small cap with wide quotes must not use the same flexibility and
  confidence as a dense, liquid ETF. Use raw-liquidity profiles and explicit
  symbol overrides; keep the mandatory constraints common to all profiles.
- During resumed work, iterate autonomously and do not stop for questions.
  The latest stop request overrides that persistence instruction for now.
- Research current theory and performance using primary sources when relevant.
  No evidence currently supports a "world's fastest" or production-ready claim.

## Durable branch and commit inventory

| Lane | Branch | Code to preserve / integrate | Integration state |
| --- | --- | --- | --- |
| Root | `feat/vol-resume-20261016` | `168365044e0f0f1bfef596897085cef9711040a8` | Current integration code checkpoint |
| Native policy | `feat/vol-native-policy-20261016` | `cacb48ebad47da7021eb2707be32865c7b18a4f5`, then `6b7e5a0b4fc557179959387f3da8168fe47b57fd` | Not integrated; 37 CLI tests passed |
| Local volatility | `feat/vol-localvol-20261016` | `61b93c565239287dd4dbf3ea88c4c6535be13bc8` | Not integrated; 27 relevant agent tests passed |
| Numerical audit | `feat/vol-pricing-audit-20261016` | `bd6e7b5f8ec5a0416fdaa7911514672c6c0653f8` | Diagnostic source only; no pricing fix; not integrated |
| Previous root | `feat/vol-benchmark-20261015` | `b1a12bf97194f5ed539c5028ce2205c591f987c1` | Ancestor of current root; includes prior handoff |

The resumed root commits after `b1a12bf9`, oldest first:

- `430e60a0`: cherry-pick of `9e0e7451fc227621a0c7264131399bce7ebe3c12`, shared
  American model compiled into exact represented PWL strike functions.
- `1bde9704a0b46efac7af6ca9fdc2b9b2c8635106`: permanent CMake source and test
  registration for the shared strike surface.
- `168365044e0f0f1bfef596897085cef9711040a8`: explicit funding-discounted dividend
  PV approximation, validation and two post-implementation tests.

Do not cherry-pick `9e0e7451` again; its equivalent is already integrated.
The native lane's `cacb48eb` is the rebased equivalent of old WIP `8d77c355`;
apply the current pair, not both generations. Catalog dependency already exists
in root as `1a9a4b24`; do not duplicate original `5788d6a2` or `12c43053`.

## Repository workflow and environment

- Windows PowerShell 5.1, clang-cl 18, C++20, CMake/Ninja/LLD, ccache, shared
  vcpkg at `C:/atx-cache/vcpkg_installed`. No fast-math. No clang-format/tidy.
- Read `CLAUDE.md`, `.agents/cpp/agent.md`, `atx-vol/CLAUDE.md`, and
  `.claude/skills/vol-dag/SKILL.md` before implementation. User no-TDD overrides
  the local TDD rule. The broker workflow was unavailable, so explicitly
  authorized native subagents used isolated leased pool worktrees.
- User AGENTS asks for codebase-memory graph discovery first. No graph MCP
  tools were exposed in this turn; `rg` was the justified fallback. Check again
  in the next session rather than assuming continued unavailability.
- Always use the **absolute path to the leased worktree's** `scripts/atx-build.ps1`
  with explicit preset, jobs and target. Never raw Ninja or bare all-target
  builds. `vol-dev`, `vol-rel-avx2`, and `vol-hygiene` isolate this restored module
  and dependencies; the normal dev configure hits missing unrelated modules.
- Lease acquisition's automatic dev configure can fail after the lease was
  successfully acquired. Inspect `.atx-lease` and workflow status; do not acquire
  a duplicate lease. Configure the appropriate vol preset through the wrapper.
- `vol-rel-avx2`: Release with IPO, AVX2, no fast-math; directory
  `build-vol-rel-avx2`. `vol-hygiene`: PCH disabled, `build-vol-hygiene`.
  Per-worktree/per-preset FetchContent directories avoid Debug/Release races.
- Worktree pool only; no raw `git worktree add`. Use frozen SHA, unique branch,
  RunId and durable HeartbeatId. Release with the same RunId, after committing
  owned changes. Release resets physical checkout to frozen base; branch remains.
- Do not reset, delete or steal unrelated worktrees/branches. Main has not been
  modified except placing handoff documents where the next parent can find them.
- Do not invoke the old licensed oracle workflow or expose its holdout. The new
  quote-only benchmark is separate. Do not treat broad release qualification as
  already completed from the focused gates below.

## What is integrated: architecture and current limits

### Quote fitting and static price admission

Relevant public APIs: `api/fitting/quote_slice.hpp`, `quote_fit_policy.hpp`,
`api/pricing/american_price_slice.hpp`; implementation under matching `src/`.

The quote fitter takes raw quotes, separate variance/calendar clocks and
contract-level supplied carry. It selects qualified observations, inverts
American prices, fits a configurable volatility curve, reprices American
options, and applies final immutable price-slice admission. Off-node queries
use admitted PWL prices and implied-vol inversion; no extrapolation.

Final gates cover intrinsic and prepaid-stock bounds, monotone/vertical-spread
constraints, global convexity, call/put inequalities, and cross-expiry checks
at the union of exact strike knots. Specialized nondividend American relations
are included. Fixed absolute/relative tolerances are scale bounded rather than
accumulating locally. Generic curve-gate bypasses were repaired previously.

These are necessary static price constraints, **not** a proof that arbitrary
separately fitted American slices come from one joint underlying process.
The constructive common-process model below is the route to that stronger scope.
Per-strike supplied carry is preserved in this quote-fitting path; a future
common-process fitter cannot silently use a different stock drift per strike.

### Funding-discounted versus escrowed dividend PV

New root commit `16836504` changes only:

- `atx-vol/include/atx/vol/api/fitting/quote_slice.hpp`
- `atx-vol/src/fitting/quote_slice.cpp`
- `atx-vol/src/fitting/quote_fit_policy.cpp`
- `atx-vol/tests/quote_slice_test.cpp`

`AggregatePresentValue` remains the existing escrowed approximation:

```text
forward = (spot - PV) * exp((r - q) * calendar_years)
API string = aggregate_pv_approximation
```

New `AggregateFundingPresentValue` uses funding-discounted cash PV:

```text
prepaid_stock = spot * exp(-q * calendar_years) - PV
forward = prepaid_stock * exp(r * calendar_years)
API string = aggregate_funding_pv_approximation
```

Both map the prepaid-stock identity to an effective continuous yield on the
variance clock. Both are explicitly approximations: without ex-dividend dates
they cannot reproduce American early-exercise decisions around cash events.
The new mode rejects nonpositive prepaid stock and combining aggregate PV with
dated cash. It can accept PV above spot when sufficiently negative carry still
makes prepaid stock positive; escrowed mode still requires PV below spot.
Log-domain retention avoids separately overflowing `exp(q*T)`.

Tests independently verify the prepaid identity for positive/zero/negative
carry, invalid states/double counting, synthetic IV recovery, per-contract
carry, and an off-node query. **Native CLI selection has not been extended**;
its old `--dividends aggregate-pv|none` still selects escrowed PV. Neither the
root nor pending policy lane silently changes the default convention yet.

Planned but not implemented: expose explicit `funding-pv|escrowed-pv|none` modes
and record them in reports; decide/document a data convention rather than tune
it secretly on fair-value labels. The user permits a breaking CLI cleanup.

### Positive shared American model and batch queries

Relevant files: `api/pricing/american_markov.hpp`, `american_markov_batch.hpp`,
`american_markov_pwl.hpp`, `pwl_strike.hpp`, and matching sources/tests.

The immutable model evolves a common discounted/drift-adjusted stock coordinate
with nonnegative three-point martingale transitions. It owns separate clocks,
rate/carry intervals, explicit cash-event layers and bounded storage. Cash uses
limited liability `min(D,S)`. Zero/upper boundaries are absorbing in the model
coordinate, with diagnostics. Every expiry uses a prefix of the same process.
Backward induction therefore constructs a consistent finite **Bermudan** model;
this is not a certificate of continuous-exercise or infinite-domain accuracy.

Defaults include 65 states, upper boundary 4 times spot, max clock step 1/52,
CFL 0.9; soft model caps 1025 states, 8192 transitions, 1 million stored rows,
with larger explicit hard bounds. Check actual header when configuring.

Scalar rollback uses prepared reusable workspace. Batch rollback uses
state-major contiguous strike buffers and common transition operations,
specialized by side/exercise style; output is published only on success, size
arithmetic is checked, and overlapping input/output is supported. Prepared
queries avoid allocation. No controlled batch speed claim has been established.

The PWL algebra represents convex strike functions, positive weighted sums and
max-with-payoff, retaining exercise crossings and represented knots without
resampling/compression. Shared surface compilation owns the common model and
call/put functions for every prefix, with live/output/work budgets and no
allocation for const queries. Resource refusal is explicit. Its numerical
crossing representation needs work; see audit findings below.

### Raw-liquidity catalog already integrated

The owned catalog deep-copies configuration and spline grids; resolutions keep
profile lifetime through shared ownership. Raw near-money quote spreads,
bidless fraction and deduplicated strike counts drive classification, with
exact symbol overrides. No hardcoded ticker list is used.

Initial, uncalibrated defaults: dense `ConvexDense` (minimum 12 observations),
ordinary residual-free eSSVI (minimum 6), sparse eSSVI (minimum 4). Dense
classification requires at least 24 strikes, relative spread <= 0.08 and
bidless fraction <= 0.10; sparse includes fewer than 6 strikes, relative spread
>= 0.60 or bidless fraction >= 0.50. Check source for full resolution logic.
These are starting configurations, not measured optimal settings. Profiles
cannot turn off mandatory strike/calendar admission.

## Pending native policy lane: ready for review/integration

Branch and exact commits are in the inventory above. Only three files changed:
`tools/quote_bench_main.cpp`, new private `tools/quote_bench_policy.hpp`, and
`python/tests/test_quote_bench_cli.py`. No CMake changes are needed for the
private header; JSON linkage and catalog registration already exist in root.

Behavior:

- Default `--policy auto`; bounded closed-schema custom JSON profiles and exact
  symbol overrides. `--curve` selects an explicit fixed experiment and is
  mutually exclusive with policy. Global quality/min-quotes overrides require
  explicit fixed-curve mode.
- Strict finite numeric bounds, file/JSON depth and profile/override limits,
  unknown keys, duplicate JSON/CLI keys, and input/output/report/policy alias
  checks including hardlinks. `--help` handling was corrected.
- Resolve once per underlying/root/exact snapshot; ambiguous duplicate expiry
  identities are excluded before classifying liquidity. Existing admission
  remains mandatory for auto, fixed and custom override configurations.
- Prediction CSV adds `resolved_profile`, `policy_source`, `configured_curve`,
  `actual_curve`. Actual curve is empty when no curve was produced. A populated
  actual curve is an intermediate fitting result, **not proof of admission**;
  row status remains authoritative after strike/calendar rejection.
- Fully expanded policy provenance and explicit zero/failure counters.
  `policy_definition_format=expanded-runtime-configuration-v1` means the
  expanded report is **not an input-policy round trip**.

The lane still supports old aggregate-pv/none dividend flags and expects
`aggregate_pv_approximation` in tests. Extending CLI modes after cherry-pick
needs new validation against root's funding mode. Python scoring has not yet
been enhanced to preserve/report policy columns, profile transitions or
per-profile paired results; that was a proposed next delegation, never started.

## Pending local-volatility lane: implemented, review still required

Commit `61b93c565239287dd4dbf3ea88c4c6535be13bc8` changes exactly
`american_markov.hpp`, `american_markov.cpp`, `american_markov_test.cpp`.
No CMake changes. Parent has not independently reviewed or integrated it.

API and design:

- `AmericanMarkovLocalVolNode { log_state_over_spot, volatility_multiplier }`.
- Append `std::span<const ...> local_volatility{}` to each interval. Empty
  profile gives multiplier 1. Otherwise nonnegative PWL multipliers in
  `log(X)-log(initial_spot)`, with flat tails, scale baseline volatility per
  state. Keep common carry/drift; recompute sampling after cash-grid changes.
- Owned `interval_data()` includes first layer, baseline volatility, owned
  knots and actual per-state sigmas. Transitions gain `interval_index`, while
  `transition.volatility` retains baseline-volatility meaning.
- Hard profile cap 257 nodes and named coordinate bound `abs(log coordinate)
  <= 64`. `max_stored_volatility_values` bounds total profile-node plus
  state-sigma count over all intervals (default 1 million, hard 8 million).
- Precompute state sigmas and exit rates once per interval. Empty/constant-one
  profiles preserve exact equivalence with the prior constant-vol model.

Seven post-implementation tests cover positivity/moments, zero sigma,
post-cash behavior, deep ownership, skew with strike/calendar relations,
extreme scales, and invalid/resource limits. This enables local-vol models;
it is **not yet a calibrated quote-fitting implementation**.

## Numerical audit: unresolved defects and evidence

External artifacts (not in git):

- `C:/atx-cache/quote-benchmark/markov-audit-20261016-default.json`
- `C:/atx-cache/quote-benchmark/markov-audit-20261016-full.json`

The diagnostic uses synthetic market data, independent lognormal European
references without cash, independent continuous deterministic stopping with
pre/post cash decisions, and scalar/batch/PWL consistency. It records explicit
refusals and boundary diagnostics. Timings are diagnostic only, not a quiet,
pinned performance benchmark. It was built against root `1bde9704`, without
the pending local-vol extension. Source commit/commands are in final receipt.

Concrete default-grid reproduction: spot 100, variance and calendar expiry
0.25, r=q=0, sigma=0.20, strike domain [40,180], default 65 states. Scalar and
batch price successfully, but shared call PWL compilation refuses expiry 0:

```text
OutOfRange: American Markov PWL: call expiry 0:
PWL strike: exercise crossing below strike resolution
```

This is a practical numerical-usability defect, not a reason to weaken the
convexity/no-arbitrage contract. A roundoff-aware crossing representation with
a stated error budget needs review and implementation. No such fix was made.
The panel also hits explicit PWL node budgets for high vol, negative rates,
negative rate/yield, and a later dated-cash expiry. Exact-knot growth may be
incompatible with a fast general surface unless its representation is improved.

Default-grid cash-free European maximum absolute price errors in examples
reach about 0.19955 (funded) and 0.16984 (separate clocks); the simple martingale
case reaches 0.078292. These are material compared with sub-tick ambitions.
Across the default panel, scalar/batch disagreement <= 1.78e-13 and deterministic
cash stopping disagreement <= 1.43e-13. Agreement between two implementations
on the same grid does not remove discretization bias. Inspect the full panel
for grid/domain/time refinement, and quantify convergence before fitting or
claiming numerical accuracy. No independent stochastic continuous-American
reference panel has yet established the desired accuracy.

Final full-panel receipt: 88 combinations (11 fixtures x 8 grids), 84 models
compile and 4 refuse on transition caps. Of the 84 compiled models, 36 PWL
surfaces succeed and 48 explicitly refuse. Maximum scalar/batch difference
is 2.1316282072803006e-13; maximum successful PWL/batch difference is
1.9895196601282805e-13. Martingale crossing refusal occurs on all eight grids.
Separate-clock European errors decrease from 0.16983548 at default65 to
0.03249548 at uniform129 and 0.00665626 at uniform257. High-volatility sigma .8,
one-year domain2 has error 8.04812205; domain4/default65 has 0.09164862. Domain
and grid refinement must be distinguished; finer spacing cannot fix tail loss.

The preserved diagnostic samples 33 strikes from .4*S through 1.8*S and **does
not include exactly ATM**. Add ATM and close neighbors on resume. Its proposed
unrun ATM edit was reverted at shutdown to preserve the executed behavior.
No independent stochastic cash-dividend American oracle is implemented.

Audit commit `bd6e7b5f` contains only `tools/american_markov_audit_main.cpp` and
`sprints/2026-10-16-markov-audit-handoff.md`. Read the latter with `git show`
from that commit (or the visible copy in main) before integrating. Temporary
CMake wiring was restored. Suggested registration under `if(ATX_BUILD_TOOLS)`:

```cmake
add_executable(atx-vol-american-markov-audit tools/american_markov_audit_main.cpp)
target_link_libraries(atx-vol-american-markov-audit PRIVATE
    atx::vol atx::core atx_warnings nlohmann_json::nlohmann_json)
```

CLI: `atx-vol-american-markov-audit [--full] [--case NAME] [--grid NAME]`.
Build through the vol-rel-avx2 wrapper, set Release vcpkg DLL PATH, and redirect
stdout JSON to a new external artifact path. Release build and both runs
finished exit 0. PCH-off configure completed, but **the audit source PCH-off
object check was not run**. Last source changes were formatting-only wraps.

Evidence SHA256:

- Default JSON: `9be912b6e5c6d643c308d6a40242a9b48422ef09d72d799a85e539065ae0bba4`.
- Full JSON: `99005ea4eaba9ac42e27ce40cacb37bba91920dc86e9da38f5095a40ad6c5a50`.
- Built audit executable: `3d62bbcc674e480b5aaaf3a52487672a5e3ea7c9f65dd26bdbbe1eef835fd4a3`.

The executable currently remains in the released pool-36 build cache; the
source branch and external JSON files are the durable evidence. No copied
executable outside that pool was promised.

## Validation receipts and limitations

Root resumed checks:

- Release focused American Markov target: **25 tests passed** (9 base Markov,
  3 batch, 5 shared surface, 8 PWL primitive). These validate the integrated
  shared surface at `1bde9704` and supersede the previous unrun-batch status.
- Release quote/price/policy target: **47 passed**, then **49 passed** after
  adding the funding-PV tests. No skips in these direct focused runs.
- Anchored CTest for the two targets passed before the two funding tests were
  added; the later 49-case direct run is separate evidence. Do not mislabel
  the older LastTest.log as containing the new tests.
- PCH-off targeted check completed exit 0 at shutdown (shell session 52160
  finished): `american_markov_batch.cpp`, `american_markov_pwl.cpp`,
  `american_markov_batch_test.cpp`, `quote_slice.cpp`, `quote_fit_policy.cpp`,
  `quote_slice_test.cpp`, including focused/monolithic test objects selected by
  the wrapper. `git diff --check` clean before root code commit.
- Root did not rerun the 38 standalone Python adapter tests this resumed turn;
  their prior pass is documented in the older handoff.

Native lane at `6b7e5a0b`: **37 CLI tests passed in 1.18 seconds**, Debug native
build, PCH-off native TU (including private JSON parser), Ruff and diff checks
passed. No source-data labels or performance measurements were used.

Local-vol lane: **16 AmericanMarkov tests plus 11 batch/PWL primitive tests**
passed in Debug. The local branch is based on `b1a12bf9`, so it lacks the five
new shared-surface tests; run those after integration. PCH-off source, test
(focused and monolith objects), and batch consumer compiled. Last post-test
edit was lambda formatting only; exact committed source was PCH-off compiled.
No durable standalone test report was created by the agent; tool outputs are
the evidence. Released pool build artifacts may remain but source is reset.

No full fast-suite, public umbrella release gate, all module gates, final
combined native/local-vol build, or production release qualification is claimed.

### Commands for the next resumed lease

Substitute the newly leased pool path and work from its repository root:

```powershell
powershell -NoProfile -File C:/atx-wt/<pool>/scripts/atx-build.ps1 `
  -Preset vol-rel-avx2 -Jobs 2 configure
powershell -NoProfile -File C:/atx-wt/<pool>/scripts/atx-build.ps1 `
  -Preset vol-rel-avx2 -Jobs 2 build atx-vol-american-markov-tests atx-vol-quote-slice-tests atx-vol-quote-bench
ctest --test-dir C:/atx-wt/<pool>/build-vol-rel-avx2 `
  -R '^atx-vol-(american-markov|quote-slice)$' --output-on-failure

powershell -NoProfile -File C:/atx-wt/<pool>/scripts/atx-build.ps1 `
  -Preset vol-hygiene -Jobs 2 configure
powershell -NoProfile -File C:/atx-wt/<pool>/scripts/atx-build.ps1 `
  -Preset vol-hygiene -Jobs 2 check atx-vol/src/pricing/american_markov.cpp atx-vol/src/pricing/american_markov_batch.cpp atx-vol/src/pricing/american_markov_pwl.cpp

$env:ATX_QUOTE_BENCH_BINARY='C:/atx-wt/<pool>/build-vol-rel-avx2/bin/atx-vol-quote-bench.exe'
$env:PATH='C:/atx-wt/<pool>/build-vol-rel-avx2/bin;C:/atx-cache/vcpkg_installed/x64-windows/bin;'+$env:PATH
python -m pytest --noconftest atx-vol/python/tests/test_quote_bench_cli.py -q
python -m pytest --noconftest atx-vol/python/tests/test_quote_benchmark.py -q
python -m ruff check atx-vol/tools/quote_benchmark.py atx-vol/python/tests/test_quote_benchmark.py atx-vol/python/tests/test_quote_bench_cli.py
```

Native lane's actual test command used `vol-dev`, binary in `build-vol/bin`,
and `C:/atx-cache/vcpkg_installed/x64-windows/debug/bin` on PATH. Match DLLs to
the build mode. Native CLI tests skip if the binary environment variable is
absent; **skipped is not passed**.

`--noconftest` is intentional for these standalone script tests, which load
the tool directly. Ordinary extension tests must use the registered driver
and its contamination guard; do not bypass that guard for compiled atxvol.
PCH-off check can report missing object targets until the preset is configured;
root hit this once and corrected it by configuring vol-hygiene, not by dropping
the check. Do not run noisy throughput tests while other agents compile.

## Benchmark data, leakage controls and previous evidence

Source: `C:/Users/natha/Downloads/OptionIntradayHist_30min_2026-10-14.parquet`,
1,916,931,890 bytes, 34,011,291 rows, 280 row groups; 31,889,698 American and
2,121,593 European rows. This is one day, not a multi-day/live validation set.

Python `tools/quote_benchmark.py` streams projected Arrow columns, physically
separates quote inputs from labels, hashes whole underliers for an 80/20 split,
selects exact snapshots, and uses raw underlying midpoint. Missing adjusted
deliverables (root suffix 1-9) are explicitly refused. Provenance includes
source/input/binary/output hashes. Run receipts include policy hash/content
rechecks and native report. Scoring left-joins the complete cohort and reports
coverage alongside independent price/vol-label masks and paired comparisons.

All exploratory comparisons so far used hash-selected development underliers
at 10:30 NY. **No heldout validation labels were inspected.** Keep that boundary.

Artifacts under `C:/atx-cache/quote-benchmark/`:

| Directory/run | Historical evidence |
| --- | --- |
| `20261014-dev` | 20 development underliers, 10,948 selected / 7,308 eligible |
| `20261014-dev200` | 200 development underliers, 72,348 selected / 63,942 eligible |
| dev200 `essvi-release-carry` | 62,412 served (97.607%), 1,038/1,138 fits; price MAE 0.277666 vs midpoint 0.350695 on 61,758 rows; vol MAE 1344.54 bp |
| liquid-core subset of that baseline | Price 0.3546 vs midpoint 0.3341 (model worse); vol 293.99 bp |
| `essvi-release-quality` | 25,276 served (39.5296%), 203 fits, 935 insufficient |
| quality exact paired population | Price MAE 0.283341 -> 0.266394, but vol 432.93 -> 524.98 bp (worse) |
| `essvi-release-arb-v1` | 10,300 served, 83 fits, 4 strike failures, 116 calendar-rejected chains, 935 insufficient |
| `essvi-release-arb-wide-v1` | 19,722 served, 298 fits, 9 strike failures, 315 calendar rejections, 516 insufficient; paired score still pending |

Carry/quality runs predate final mandatory admission. The arb-v1 runs predate
later global/nondividend gate repairs. Do not present any of them as current
checkpoint performance. No automatic policy run has been measured on dev200.
Preserve all immutable CSVs, run receipts, scores and saved binaries; write new
paths for new experiments. Report selection/coverage effects, not just reduced
MAE on the easiest surviving contracts.

Prior dense-IV optimization had one retained paired CPU comparison: **1.550115x
CPU speedup / 35.49% less CPU** for a 4097-query strip (F=100, T=.25, df=.98,
flat vol .22), Release AVX2, one worker, CPU affinity 0. Four ABBA runs with
five reps each, CPU-CV cutoff 5%, retained only A3 and B1. No clean paired wall
claim. Saved `dense-pinned-comparison.json`, `measure-dense-abba.ps1`, all raw
JSON/stdout/stderr and old/new binaries. Numeric panel had 807 finite of 840
cases, 33 expected refusals, max IV difference 4.55e-12 vs 80-step bisection,
price difference 4.47e-9. This is a specific dense-IV fixture, not an American
model, full-library or HFT throughput claim. Details/hashes in the old handoff.

## Research retained for continuation

Official SpiderRock pages consulted this resumed turn:

- [Live volatility surfaces](https://docs.spiderrockconnect.com/docs/Documentation/PlatformFeatures/Analytics/LiveVolSurfaces/)
  documents a funding-PV forward convention and a strike-dependent carry
  spline; surface proxy-forward conventions can differ from pricing forward.
- [Option pricing](https://docs.spiderrockconnect.com/docs/Documentation/PlatformFeatures/Analytics/OptionPricing/)
  describes the generalized inputs, dated cash dividends, modified binary-tree
  approach with dividend splicing and a numerical target around one tenth of
  the minimum tick. It is a vendor description, not evidence that our model
  meets that target or that the parquet contains every required input.

Earlier references and detailed implications are in the 2026-10-15 handoff:
Cox/Hoeggerl arXiv 1301.5467 (nondividend American consistency), Healy 2109.15157
(negative rates), Itkin 2510.18159 (cash dividends), SSRN 2870285 (local vol and
American calibration). SANOS 2601.11209 is a European construction; do not cite
it as a completed American result. Verify references/current claims when used.

## Recommended continuation after explicit user resume

1. Inspect current main/branch/lease state, acquire a fresh root lease from
   `feat/vol-resume-20261016`, and preserve these handoffs. Do not work from the
   released physical pool source or main's old implementation.
2. Review the native policy commit pair and local-vol commit, then integrate
   them in a controlled root branch and run their combined focused closure.
   Review exact SHAs; lane tests alone are not integrated validation.
3. Integrate the audit tool and register its target. Reproduce default PWL
   crossing refusal, address representational roundoff with a justified bound,
   and examine exact-knot resource growth. Keep mandatory gates and explicit
   refusals. Validate supported domains, scales and common-model consistency.
4. Establish grid/time/domain convergence and independent continuous-American
   accuracy before optimizing the common model. Default65 is not sufficiently
   accurate merely because positive rows and scalar/batch equality hold.
5. Expose explicit funding/escrowed dividend conventions in CLI and provenance;
   validate both. Missing ex-dates remain a material modeling limitation.
6. Extend Python scoring/comparison to retain policy/profile/model columns and
   report paired errors and coverage per profile, plus profile transitions.
   This was suggested for delegation but no agent started it.
7. Run fresh fixed and auto-policy dev20/dev200 comparisons under identical
   latest gates, with new artifact paths. Do not inspect validation labels to
   choose parameters. Use profile-dependent flexibility, not ticker hardcodes.
8. Build reusable **bid/ask interval calibration** on the common local-vol
   process. Current point filtering causes a severe coverage collapse. Bound
   positive parameters/evaluations, use appropriate regularization/model
   complexity by liquidity profile, and share one carry process across strikes
   and expiry prefixes. This calibrator is not implemented yet.
9. Measure fit/update/query and batch throughput under controlled quiet Release
   conditions only after correctness. Add model-consistent Greeks/analytics,
   uncertainty and operational readiness incrementally. No shortcuts around
   continuous-American accuracy, dividends, delivery terms or data quality.
10. Freeze choices before untouched validation. Eventually perform release
    qualification and deliver a validated checkpoint to main after inspecting
    intervening user changes. Nothing in this handoff declares the goal done.

## Final shutdown receipts

All implementation stopped. No new feature or defect fix was started after
the user requested the stop. Root preserved its already-tested funding-PV
change as `16836504`; the remaining root change is this handoff only.

Completed lane release receipts:

```text
RELEASED pool=pool-34 run_id=vol-20261016-policy
RELEASED pool=pool-35 run_id=vol-20261016-localvol
RELEASED pool=pool-36 run_id=vol-20261016-audit
```

Their branches and exact commits remain available. Physical pool-34 and
pool-35 reset to b1a12bf9; pool-36 reset to 1bde9704. No lane process is running.
The root's last PCH-off session 52160 completed exit 0; no root build remains.

Root lease before release: pool-33, RunId/HeartbeatId `vol-20261016-root`,
branch `feat/vol-resume-20261016`, frozen base b1a12bf9. All owned code is
committed. This document is committed on that branch before release and copied
into `C:/atx/atx-vol/sprints/` for immediate discovery. The main visible copy
will append the exact root documentation SHA and release receipt; the branch
copy necessarily precedes its own release. Unrelated existing leases were
inspected only and left untouched.
