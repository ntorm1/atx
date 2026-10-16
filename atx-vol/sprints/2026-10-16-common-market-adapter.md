# Common American market adapter

Implemented on `feat/vol-common-market-20261016`, frozen base
`9717c4769fb93ff8e40e9159bcc6b741aa46bf7e`, pool-36 lease
`vol-common-market-20261016`. User requested implementation before checks;
implementation preceded the new test suite. Graph MCP tools were not exposed,
so repository discovery used the documented `rg` fallback. The unavailable
broker was replaced by the parent-authorized native lease workflow.

Dependencies are the already approved interval fitter pair
`fa85224d4717a006d6a7ee0a4c70b2a26c90e263` and
`7f85cd1f5494cf2003a003534815c9f5cb6b4469`. Local cherry-pick equivalents are
`9df1f4e3` and `f6795afa`. Parent integrates those separately; the adapter commit
contains only its new header, source, test and this note.

## Contract and construction

`prepare_american_markov_market` accepts bounded raw contract records for exactly
one underlying/deliverable/valuation snapshot. The caller enforces that identity;
opaque expiry keys distinguish expiry contracts. The input type contains only
contract/side/bid/ask/weight, raw underlying spot, calendar/variance clocks,
zero-to-expiry rate, supplied continuous yield and aggregate dividend PV.
No vendor fair values, fair/quote/ATM vols, Greeks, synthetic spot or model flags
are fields. Underlying spot provenance remains the caller's responsibility.

The adapter takes an equal-contract-weight median spot over the complete chain
and coordinate medians for each expiry's clocks/rate/yield/PV. Price, spread and
fit weight do not influence these medians. Even samples use the midpoint of
their central values. All contracts remain represented; no outlier trimming or
automatic reweighting hides discrepancies. Duplicate expiry/strike/side records
refuse the snapshot. Different expiry keys at an equal calendar endpoint refuse.

Let R_i=r_i*T_i, and Q_i be the effective integrated continuous yield after the
selected PV convention. Interval rates are `(R_i-R_previous)/(T_i-T_previous)`
and yields are `(Q_i-Q_previous)/(T_i-T_previous)`, starting from zero integrals.
All strikes consume the same interval schedule and every expiry is its prefix.
Cumulative rate/yield and both clocks interpolate linearly between expiries.
Variance clocks can pause, but may not decrease. There is no hidden clock repair.

The successful prepared object owns spot, interval and quote vectors, and expiry
keys; quotes remain in original input order with mapped expiry indices. Its
`fit_market()` view borrows the object and any explicitly supplied state grid.
No volatility prior, profile, state grid or solver complexity is selected here.

## Dividends and diagnostics

The default `None` refuses nonzero aggregate PV. The two explicit approximations
preserve terminal prepaid values:

- `FundingPvContinuousYield`: `S*exp(-q*T)-PV`.
- `EscrowedPvContinuousYield`: `(S-PV)*exp(-q*T)`.

Both output zero cash events and an effective continuous yield. Missing ex-dates
preclude exact American exercise treatment. Dated cash remains available through
the fitter's interval API; this adapter does not invent event dates. Funding PV
may exceed spot under negative q if prepaid stays positive. Each original carry
and the resolved common carry must satisfy positive prepaid and numerical bounds;
a feasible median cannot hide an infeasible row. `log1p` evaluates the PV yield
adjustment without cancellation for small dividend fractions.

Diagnostics retain median/minimum/maximum/MAD/maximum absolute deviation for spot
and each expiry's clocks, funding, supplied yield, PV, prepaid/spot and forward/spot.
Prepaid/forward ratios use the common spot and median expiry clock, separating
carry disagreement from spot/clock disagreement. The resolved cumulative R/Q and
prepaid/forward ratios and interval r/q are reported separately. An invalid row
or expiry is identified when applicable; dispersion refusal retains the failing
expiry summary. Ratios and raw samples are dispersion diagnostics, not uncertainty
bounds. A caller should record the chosen dividend convention and all tolerances.

Defaults require max deviations <=0.5% of spot for spot and PV, <=25bp annualized
funding, <=5% annualized supplied yield, <=0.05 in prepaid/spot and forward/spot,
and <=1e-10 years for each clock. Zero dispersion tolerance means exact agreement.
No threshold is silently disabled. These are explicit initial data policies, not
empirically calibrated acceptance standards. Hard bounds are 16,384 quotes,
1,024 expiries, 100 years, 100 absolute annualized input/interval rates and 64
absolute cumulative logarithmic factors. Defaults use 4,096 quotes, 128 expiries,
10 absolute interval annualized rate/yield and a 1e-10-year minimum interval.
The interval bound catches noisy term inputs amplified by close expiry times.

## Verification and integration

Post-implementation Debug tests: **20 passed, zero skips**, including:

- robust median and quote-weight independence, original-order mapping/ownership;
- term-rate and funding-PV prefix recovery, paused/decreasing clocks;
- distinct dividend forward identities, negative-yield PV above spot;
- funding-PV scale invariance at multipliers 1e-200, 1 and 1e200;
- complete outlier/clock/spot/numeric/resource/duplicate/invalid-input refusals;
- inclusive/zero tolerance, bidless/locked quotes, permutation invariance;
- direct fitter integration and independent European put-call/forward identity
  at every expiry prefix of the prepared common process.

Commands used the absolute pool-36 build wrapper with `-Preset vol-dev -Jobs 2`,
first focused `check`, then `build atx-vol-american-markov-market-tests`, anchored
ctest `^atx-vol-american-markov-market$`, and the binary's XML output. XML artifact:
`C:/atx-cache/quote-benchmark/common-market-20261016-tests.xml`. Both owning source
and test TUs separately compiled with `vol-hygiene` (PCH off). No clang-format or
clang-tidy was run. `git diff --check` and the 100-column manual check passed.

Validation temporarily registered the fitter + adapter sources in the library
and a focused test executable. Those CMake edits are restored before commit.
Parent must permanently register `src/marketdata/american_markov_market.cpp` and
`tests/american_markov_market_test.cpp` after the fitter dependencies are integrated.

No parquet-specific inputs or labels were used. These checks establish the market
adapter's algebra and API behavior, not fit quality, continuous-American numerical
accuracy, HFT latency or release qualification. The finite-model fitter retains
its mandatory consistency constraints. A final-price equivalent IV needs an
explicit pricing convention; a local-vol node sigma is not an implied volatility.
