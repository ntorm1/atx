# Opt-in raw-quote initial sigma estimates

This standalone initializer accepts an existing common fit market, raw fit
quotes, parameter layout and bounded seed controls. It returns owned estimates
and diagnostics. It never fits, compiles, changes a prior, changes a policy,
edits quotes, or applies its output. No CLI/default integration is included.

API: `estimate_american_markov_quote_seeds` in
`include/atx/vol/api/fitting/american_markov_seed.hpp`, implemented by
`src/fitting/american_markov_seed.cpp`.

## Numerical convention

For expiry j, the helper accumulates `R_j = sum r_i * delta_calendar_i` and
`Q_j = sum q_i * delta_calendar_i`, retaining supplied cumulative variance time
`V_j`. Bid, midpoint and ask are independently inverted through the existing
Andersen-Lake American IV routine at `T=V_j`, `r=R_j/V_j`, `q=Q_j/V_j`.
This is an approximate equivalent flat-to-expiry American IV. It is not local
volatility and does not preserve a time-varying optimal exercise boundary.

Schedules prepared from aggregate dividend PV through the explicit common
continuous-yield approximation can be used: their cash entries are zero and
their effective yield already contains the convention. Any nonzero dated cash
entry is explicitly refused; this helper does not erase an ex-date or reinterpret
cash as continuous yield. Negative-rate pricer failures remain failures, with
the original error text. There is no alternate pricer fallback.

The initializer screens valid positive two-sided quotes by raw forward
moneyness and relative spread. Bidless quotes, invalid/duplicate contracts,
zero variance clocks, intrinsic plateaus, wide/far quotes, inversion failure,
IV-floor results, and configured sigma-bound violations each retain explicit
quote-level status. Every successful inversion value is retained, including
values leading to refusal. The existing inverter reports `kIvMin=0.005` for
plateaus/sub-floor roots; these are refused even if the fitter's requested lower
sigma bound is smaller. No seed is silently clipped into the requested range.

Within each expiry candidates sort deterministically by absolute log-forward
moneyness, relative spread, strike and side. Each expiry has a candidate cap;
inversions are allocated round-robin across chronological expiries. Selection
only limits initialization work. All original rows must remain in the eventual
fitter, including malformed or otherwise seed-refused rows. The initializer
does not replace the fitter's own validation or numerical resource gates.

Per-expiry equal-quote medians of bid/mid/ask total variances `sigma^2 V`
produce a support band. Median absolute deviation and median relative spread
describe the evidence. Fitter quote weights are validated but are not treated
as statistical confidence. These descriptive bands are not confidence intervals
or certified no-arbitrage bounds.

Stopped-variance expiries are aggregated within their distinct positive
variance endpoint first. Shared time shape uses an equal-endpoint median of
`W_j/V_j`; extra stopped-clock expiries cannot give an endpoint additional
weight in that final median. Supported endpoints are explicit, so a shared
estimate based on only part of the schedule does not claim complete quote
coverage.

For per-variance-interval shape the midpoint forward variance is
`(W_mid,j - W_mid,j-1) / delta_V`, with support interval
`[(W_bid,j-W_ask,j-1)/delta_V, (W_ask,j-W_bid,j-1)/delta_V]`. The origin has
exact `W=0`. A missing adjacent endpoint, negative midpoint increment, support
crossing zero, nonfinite arithmetic, or midpoint sigma outside the requested
bounds leaves that parameter unseeded. Raw bands remain visible. There is no
isotonic repair, floor, interpolation, or default fill. Every requested state
knot receives the same seed for its time group; no smile recovery is claimed.
The complete `initial_sigmas` vector is absent if any requested parameter is
unseeded, while partial parameter diagnostics remain available.

The American algorithm and its exercise limitations follow the existing
library method; see the primary
[Andersen/Lake/Offengenden paper](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2547027)
and [Healy's negative-rate analysis](https://arxiv.org/abs/2109.15157).
Total-variance differencing here is only an initialization heuristic.
[Gatheral/Jacquier's European static-arbitrage framework](https://arxiv.org/abs/1204.0646)
does not make inconsistencies in American equivalent-IV estimates an arbitrage
admission failure.

## Work and prior contracts

Defaults scan at most 16,384 input quotes and 128 expiries, keep five candidates
per expiry, allow 384 inversion calls, and reserve 12,288 root-iteration slots.
Each attempted inversion consumes 32 slots, including failures. The scheduler
requires capacity for all three bid/mid/ask calls before starting a candidate;
early failure consumes only the calls actually attempted. Hard caps are 1,024
expiries, 3,072 inversions, 128 iterations per inversion and 393,216 slots.
The common fitter's existing 128-parameter hard cap is retained.

ALO defaults are explicit: 7 collocation nodes, fixed-point quadrature 16,
premium quadrature 16, four boundary sweeps and boundary tolerance 1e-8.
Options are validated, not silently quantized: collocation 6..32, quadrature
from {8,16,24,32,48,64}, sweeps 2..16, and bounded finite tolerances. The IV
tolerance defaults to 1e-6. The underlying IV routine has additional bounded
bracketing/polish work; reserved root slots are not its actual evaluation count,
hardware operations or elapsed-time budget. No HFT latency claim is made.

The fitter interprets an empty `prior_sigmas` as its INITIAL vector. A caller
choosing these seeds while preserving its prior regularization must explicitly
materialize the old prior before changing `initial_sigmas`. No helper silently
makes that decision, and no existing initial/prior span is modified.

## Validation and integration

Implementation preceded tests. The focused source and tests use the leased
pool-36 wrappers from frozen `45756049154585b2be731921c2582409bdf5dc93`.
Tests cover independent flat/term-variance price controls, dual clocks and
funding-PV effective yield, equal endpoint weighting with stopped variance,
missing/negative/uncertain forward estimates, cash, bidless/plateau/floor/bound
refusals, retained inverter errors, exact work accounting, deterministic
selection under permutation, ownership and unchanged caller configuration.

Temporary CMake registration places this source in `atx-vol` and this test TU
in the existing focused American Markov fit target. Parent owns permanent
source/test registration and public API inventory/umbrella integration. This
commit includes only the new header/source/test and this note. No benchmark
dataset, vendor label, validation split, pricing defaults, broad test label or
production policy is used or changed.

Validation passed: 14 new `AmericanMarkovSeeds` and 14 existing
`AmericanMarkovFit` cases, 28 total with zero skips/disabled/failures, via
`atx-build.ps1 -Preset vol-rel-avx2 -Ctest -R '^atx-vol-american-markov-fit$'`.
The GoogleTest receipt records 0.210 seconds total; this is diagnostic test time,
not a performance benchmark. Both new source/test TUs also passed the
`vol-hygiene` PCH-off wrapper check. The XML receipt is
`C:/atx-cache/quote-benchmark/quote-seed-20261016-tests.xml`; its hash and exact
commit are supplied in the delivery receipt. No full Debug runtime or broad
release qualification is claimed.

## Parent integration

Parent reviewed exact `6d8cd7f32b2da84cc0250051765e975391b0a652` and integrated
it as `bc235d86`. Permanent library source, focused/monolithic tests and public
header inventory are registered (Tier-A58/Tier-B32,91 api headers including
umbrella). Parent14 seed plus14 fitter tests pass, as do source and both test
PCH-off objects and the two affected public-header inventory/install checks.
No full-release pass is claimed; the known unrelated fixture-path umbrella
failure remains. The seed helper is still opt-in and has no native CLI application.
