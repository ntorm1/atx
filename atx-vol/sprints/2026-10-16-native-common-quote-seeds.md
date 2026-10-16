# Opt-in native common-model quote initialization

The common benchmark can explicitly initialize its existing bounded fitter from
the reusable quote-only estimator. The default remains configured initialization.
No production profile, regularization weight, fitter/model/grid capacity, quote
selection, or reporting-IV convention changes. No market-data or label experiment
was run in this lane.

Enable in a common policy file:

```json
{
  "schema_version": 1,
  "model": "common-markov",
  "initialization": {
    "mode": "quote-seed",
    "seed": {"max_inversions": 384, "max_root_iteration_slots": 12288}
  }
}
```

Omission, or `"initialization":{"mode":"configured"}`, uses the original fit
configuration unchanged. Seed settings with configured mode are refused, as are
unknown keys/modes, nonfinite or incorrectly typed numbers, unsupported ALO
quadrature orders, and budgets unable to reserve a bid/mid/ask triplet. Every seed
setting is expanded in the policy definition and bound by the existing native
configuration/policy/executable and wrapper source receipts. The top-level setting
is common to the run; layout, bounds and parameter counts come from each resolved
profile. No new CLI passthrough or wrapper option was introduced.

The private helper owns the effective initial/prior vectors. It materializes the
configured prior before attempting quote estimation: explicit prior values stay
explicit, and an originally empty prior becomes the original configured initial
vector (or repeated `initial_sigma`), never the seed. Only a complete, finite,
in-bounds seed vector of the resolved parameter count can replace initialization.
All state knots retain the estimator's flat seed per time group. An estimator
error or incomplete result retains the entire configured initial vector and
reports the reason. A subsequent fitter failure remains a whole-snapshot failure;
there is no retry under configured initialization and no second search budget.

The estimator sees the prepared common schedule and **all** prepared fit quotes.
The fitter receives that same full quote span regardless of estimator exclusions,
candidate caps, inversion failures or incomplete endpoints. Existing market,
policy, grid and fitter quote-cap refusals precede estimation. The new `seed`
stage is attempted only when estimation actually runs. The earlier `fit` stage
may already be marked attempted because it includes the existing quote-cap check;
the flags describe executed calls, not a presumed chronological stage prefix.

Every snapshot reports configured/applied/fallback/not-attempted state and reason,
effective initial/prior vectors when resolved, original vector sources, numeric
configuration/layout, all quote and parameter status counts, inversion calls and
reserved root-loop slots, plus expiry/endpoint support bands, MAD and spread.
Estimator errors with no returned ledger have null work/selection counts.
Individual inversion error text is sampled at most 32 times, at most 1024 bytes
per sample, with explicit text truncation and unsampled-error counts. This sample
limit affects diagnostics only; every quote refusal remains counted. Other arrays
are bounded by the existing 128-parameter and 1024-expiry hard limits. Progress
snapshots retain compact state/counts/vectors; detailed expiry/error arrays remain
in the final report. Run state counts include all early refusals; per-profile
counts include snapshots whose profile was resolved before any later refusal.

The seed convention remains Andersen-Lake equivalent flat-to-expiry American IV
using variance time and cumulative calendar funding/yield divided by that time.
Aggregate dividend PV already converted by the market adapter is an approximation;
dated cash remains unsupported by the estimator. Support/MAD are descriptive, not
statistical confidence or arbitrage bounds. Root slots reserve configured inversion
iteration capacity, not total ALO pricing work. See the reusable estimator's
[design and numerical limitations](2026-10-16-common-quote-seeds.md).

Implementation preceded tests. The anchored Release/AVX2 native CTest passed all
99 Python checks, zero skips, including 24 new seed cases: unchanged configured
output; complete flat seeds; empty/explicit old priors verified through the actual
regularization loss; state-knot broadcasting; full-quote retention; quote-cap,
selection, inversion-budget, IV-floor and sigma-bound fallbacks; partial term-vector
refusal; bounded inversion error samples; closed parser limits; early stage flags;
failed seeded fits; and compact durable progress binding. No qualified performance
or accuracy improvement is claimed.

Validation commands (lease-local absolute wrapper):

```powershell
& C:/atx-wt/pool-34/scripts/atx-build.ps1 -Preset vol-rel-avx2 -Jobs 4 check atx-vol/tools/quote_bench_common_seed.cpp atx-vol/tools/quote_bench_common_policy.cpp atx-vol/tools/quote_bench_common.cpp
& C:/atx-wt/pool-34/scripts/atx-build.ps1 -Preset vol-rel-avx2 -Jobs 4 build atx-vol-quote-bench
$env:PYTEST_ADDOPTS='--junitxml=C:/atx-cache/quote-benchmark/native-seeds-20261016-tests.xml'
& C:/atx-wt/pool-34/scripts/atx-build.ps1 -Preset vol-rel-avx2 -Ctest -R '^atx-vol-quote-benchmark-native$' --output-on-failure
& C:/atx-wt/pool-34/scripts/atx-build.ps1 -Preset vol-hygiene -Jobs 4 check atx-vol/tools/quote_bench_common_seed.cpp atx-vol/tools/quote_bench_common_policy.cpp atx-vol/tools/quote_bench_common.cpp atx-vol/tools/quote_bench_main.cpp
python -m ruff check atx-vol/python/tests/test_common_markov_bench_cli.py
git diff --check
```

The PCH-off owning translation-unit closure and Ruff pass. The native CTest wall
time was 6.35 seconds on a shared host, diagnostic only. The XML receipt is
`C:/atx-cache/quote-benchmark/native-seeds-20261016-tests.xml`.
SHA256: `3ef8f515feaf47492b619005aaa8e3346440e2a4e1bb13fe4d60c964bfceea75`.

Lane: `feat/vol-native-seeds-20261016`, frozen base
`5a2ac006a8606830c31b226ff4677c9c90cdf710`, pool-34,
RunId `vol-20261016-nativeseeds`, independent heartbeat keeper. Acquisition's
unrelated default-dev configure hit the documented missing-module trap after
successfully publishing the lease; the isolated vol presets configured normally.
Temporary CMake registration was restored before commit. Parent integration must
add `tools/quote_bench_common_seed.cpp` to `atx-vol-quote-bench`; the existing native
CTest already includes the changed Python test file. Parent owns shared CMake,
README and ledger changes. Fresh exact-SHA parent review remains required.
