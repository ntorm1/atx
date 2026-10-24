# Early residual proposals for economic repaired IVs

The explicit repaired-price budget route can propose an IV from an evaluated
Screen or Fast residual before paying for strict sigma convergence and cold polish.
This private optimization changes `american_implied_vol_price_candidate` only.
Public Search, its sigma tolerance, public `price_tol` polish, default fitting,
and the three-day family-domain guard retain their existing controls.

Screen 7/16/48/5 proposes first. A refusal, numerical failure, or full Screen
convergence that misses the target check falls through the complete Fast chain.
The private `IvPriceProposalKernel::Fast` control retains that approved chain for
comparison; the private helper defaults to `ScreenThenFast`. Neither mode changes
public Search or enables the monetary policy on a default fit. Screen has no
uniform accuracy guarantee and supplies no acceptance proof.

An internal `PriceProposal` is null on every public/default inversion. On the
economic candidate route it requests an early return only at a finite evaluated
residual whose magnitude is at most one quarter of the immutable price budget,
with sigma strictly above the reporting floor. It can stop at the initial seed,
a bracketing probe, or a Newton residual. It never returns an unevaluated Newton
step as an early proposal. The residual supplies a candidate, not acceptance:
warm Fast pricing can depend on its boundary state.

Every proposed IV goes through the unchanged `american_implied_vol_price_seed`
check against the selected cold target map. This independently requires the
exact monetary residual budget, deterministic target range/zero-volatility
screen, actual cold `P(kIvMin) < target`, continuation classification, and the
existing positive cold chord in the near-exercise band. No floor witness or
exercise check is replaced by a monetary residual.

When an early Fast candidate fails that check, the implementation reruns the original
complete Fast Search and its original cold-target check. If Fast has no early stop,
that search already performed the full algorithm and is checked once.
An ultimate refusal delegates to the caller's existing typed fallback. A refusal
does not change a row to PriceOnly directly. Zero/invalid budgets and unsupported
controls refuse before starting any work.

The private optional `IvPriceCandidateStats` reports the Fast chain's `stopped_early`,
`accepted_early`, and `retried_full_search`, separately from native K3 acceptance.
`screen_attempted`, `screen_stopped_early`, and `screen_accepted` report the preceding
Screen proposal. Existing aggregate fit counters named `reimply_fast_attempts` and
`reimply_fast_roots` count calls/results of the monetary candidate chain; after this
addition those historical names do not mean that Fast arithmetic actually ran.
It is reset even on an invalid request. No strict IV accuracy or Greek accuracy
is inferred from economic acceptance.

## Focused evidence

Evidence is retained under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/`.
The initial runtime red test (`price-proposal-red.xml`) had zero early accepts and
22 residual evaluations, equal to full Search. The six one/two-day put cases now
all accept only after the cold checks, use 18 residual evaluations instead of
22, and use 18 boundary solves instead of 24. These are work counts, not a full
SPY speed claim.

Ill-conditioned SPY and near-exercise cases at three budgets exercise six rejected
early proposals; all repeat the original full result and two recover a valid
candidate with exactly the original IV bits. Zero-budget, sub-floor and exercise
plateau regressions remain refused. A tighter budget can produce a different
candidate, so its test independently reprices any accepted answer against the
exact smaller budget instead of assuming that a previous answer must be reused.

The owning five-suite pricing run reports 80 passes, three instrumentation-disabled
skips and one pre-existing hard-bit golden failure. Rebuilding the exact clean
`c47dc76d` baseline reproduces that failure with the same actual bits on both
contracts: `4597094347033763856` and `4597201503396380501`, versus the older test
pins one/two ULP away. `price-proposal-baseline-golden.xml` and
`price-proposal-final.xml` preserve both results; the pins were not changed.
The 16 focused identification/finalizer tests pass (`price-proposal-fit.xml`),
including final cold repricing and cached query metadata. PCH-off compilation of
the changed pricing source/test and the quote-slice consumer passes
(`price-proposal-hygiene.log`).

The separate Screen runtime red (`price-proposal-screen-red.xml`) records zero
Screen attempts/acceptances before implementation. The new Screen test accepts
all twelve one/two-day call/put cases after independent cold checks. Across twelve
ill-conditioned/tight-budget cases it refuses nine proposals; seven refusals have
no early stop, and two recover valid Fast answers. Every refusal's result and Fast
diagnostics exactly match the explicit Fast-only control. An explicit nondefault
target-scheme test checks the caller's cold map, not Screen's own map. All fifteen
non-monotone/candidate tests pass (`price-proposal-screen-cases.xml`). Zero budget,
unknown private kernel selection, sub-floor and exercise plateau requests remain
refused. The sixteen identification/finalizer regressions also pass after the
Screen change (`price-proposal-screen-fit.xml`), and the changed pricing source,
test and quote-slice consumer pass PCH-off compilation
(`price-proposal-screen-hygiene.log`). Screen's full-input correctness and timing
are separate integration work.

Full SPY actual-API price/floor/coverage audit and a quiet complete-fit timing
remain integration gates. This optimization does not turn historical typed
fallback into a general monetary-error guarantee on previously unseen inputs.
