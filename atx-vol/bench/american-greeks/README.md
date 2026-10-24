# Frozen American Greek corpus

`corpus-v1.json` contains 715 continuous-carry contracts: both sides of the
existing 357-contract public pricing corpus, followed by the fixed near-edge
regression. Its source hash, model/units, IDs and full denominator are explicit.
No cash dividends, vendor labels or fitted IVs enter it. The offline runner
produces numerical diagnostics, without claiming derivative qualification.

No derivative tolerances are selected. The approved SPY price budget does not
define a Greek budget. Preserve every refusal and unresolved derivative, and
distinguish derivative-of-served-map consistency from independent model accuracy.

[The review and execution contract](../../docs/american-greek-qualification-review-20261024.md)
records the existing API differences, known near-edge gap, caller trace,
independent-reference requirements and inclusive latency accounting.

## Reproduce

From the repository root on the supported toolchain:

```powershell
./scripts/atx-build.ps1 -Preset vol-rel configure -DATX_VOL_BUILD_GREEK_AUDIT=ON
./scripts/atx-build.ps1 -Preset vol-rel -Jobs 2 build atx-vol-greek-audit
python atx-vol/bench/american-greeks/greek_report.py --binary build-vol-rel/bin/atx-vol-greek-audit.exe --selection atx-vol/bench/american-greeks/pilot-v1.json --output <new-evidence-directory>
```

Omit `--selection` to run all 715 contracts. Each output directory must be new.
The launcher archives the executable, hashes every receipt and runner source,
records Git/Python/platform identity, and checks the frozen corpus hash. The
native report preserves exact input rows, schemes, model convention and raw
price stencils; Python reconstructs derivatives independently of the C++ code.
`--report native.json --output summary.json` reproduces a summary without rerunning
prices. Add `--require-all` to require exactly all 715 frozen IDs, or
`--selection selection.json` to require that exact subset. Native execution
always joins the result to the requested selection. Standalone revalidation
without either flag permits an explicitly declared subset. `test_greek_report.py`
exercises polynomial signs/mixed derivatives and
malformed receipts. Set `ATX_GREEK_NATIVE_REPORT` to a pilot receipt to also check
228 price stencil values against independent European Black closed forms.

Four routes request all nine fields: cold `american_greeks_fd`, cold
`american_greeks_al`, sigma-family with carry bumps, and sigma/q-family with rate
bumps. The cold APIs have no phi field and the sigma/q API has no vanna, volga or
charm; these remain `NotInApi` outcomes in every denominator. Native API success
is not Greek accuracy qualification. Build failures and typed refusals stay in
the report. Each family is built afresh per contract; no latency comparison is
made by this correctness runner.

Family geometry uses default accurate Reference options (explicitly resolved
as 12/24/48/6, tolerance 1e-10). Sigma nodes: 12 LogSigma over
`[0.8*sigma,1.2*sigma]`. Sigma/q adds four nodes over `q +/- 0.0005`, without
silently changing a box to avoid a preparation refusal. The internal-put strike
is the anchor (`K` for a put, `S` for a call). Carry bumps retain their public
1e-4 convention. These are declared diagnostic boxes, not calibrated production
boxes or a statement that every such box should prepare.

Cold references use explicit Andersen-Lake Reference schemes 24/48/64/24 at
1e-13 and 32/64/64/32 at 1e-14. They share the production method and therefore
are correlated references, not independent numerical model verification. Each
scheme exports nineteen price points at scales 1, 1/2 and 1/4, with base steps:

- Spot: `min(0.001*S, 0.02*S*sigma*sqrt(T))`.
- Volatility: `0.001*max(sigma,0.1)`.
- Time: `0.002*T` on the declared model year fraction.
- Rate and continuous yield: `0.0001` each.

Central first/second/mixed differences reconstruct all nine fields. Theta and
charm have the calendar minus sign. Reports expose each estimate, last-step
change, Richardson drift, profile change and intrinsic-band crossing (price
within `1e-9*K` of positive intrinsic). None is a rigorous reference error bound.
The per-level crossings remain in the raw report; the summary's union flag is
conservative and must not be read as saying every finer level crossed.
Python independently checks each native flag against the bumped spot, strike,
side and price. Each field's regime evidence includes the center even when its
formula does not. A missing center is reported as incomplete regime evidence,
without discarding a first difference whose own operands are available.

Separate served-map stencils vary only actual family axes. Sigma-family permits
spot/sigma; sigma/q permits spot/sigma/q. Time and rate axes, and sigma-family q,
are `NoModelAxis`; no fixed-expiry family theta is invented by repricing another
geometry. Per-field totals keep requested, supported, compared, reference-
unavailable and refusal-reason counts. Error maxima are diagnostics in each
field's native units. The schema always leaves `derivative_tolerances: null`
and `qualified: false`.

The existing preset-ladder benchmark also reports `coverage_rows` and
`coverage_iterations`: direct/error rows count all iterations (including failed
family preparation), whereas chain/shock counters describe the final complete
iteration. Shock coverage accumulates every contract before its output columns
are reused. Price-only work requests zero derivatives; the separate chord and
price/vega microbenchmarks do not masquerade as nine-field requests.

The [complete 715-case receipt review](../../docs/american-greek-full-corpus-20261024.md)
retains all 25,740 requested field outcomes and documents the cold PDE exercise
failures, short-expiry step errors, and unresolved carry-boundary references.
