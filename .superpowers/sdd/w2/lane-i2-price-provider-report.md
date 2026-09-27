# I2 computed price-descriptor provider

2026-09-26. Production `e396adcfa16e01a06c5a5652b917ea2430c50f78`, clock correction `d88e8e2b`, metadata/syntax correction `a2f64049`; five postimplementation fixture definitions `94d6e80c4c7a831c22a55b7a0567e1e2acd04ecf`. Dependency is the independently reviewed I2 panel source `09d5ff956ec28b9454151b851246a72fea7a891d` (local import `5a060e55`). **Source implemented; focused compilation/runtime and original I2 scale/data acceptance pending.** No build, test execution, dataset acquisition or warehouse activity occurred in this lane.

Owned additions are `include/atx/engine/data/price_exposure_provider.hpp`, `src/data/price_exposure_provider.cpp`, and `tests/data/price_exposure_provider_test.cpp`. Root registers the private CPP and test TU. No existing algorithm, legacy risk exposure path, config, stage or CMake file changed.

`build_price_exposures` accepts either a borrowed `alpha::Panel` or validated D6 `PanelStore`, plus sequential typed dated evidence. It computes six actual descriptors, sends them into the existing I2 normalizer, and returns an immutable `ExposurePanel` ready for checked extraction and the residual-IC context. This closes the earlier supplied-six-descriptor gap for this bounded programmatic producer, without claiming the original exposure stage or real artifact complete.

The fixed `PriorKnownCapPriceV1` recipe is explicit:

| Column | Definition and support |
|---|---|
| Log ADV63 | Log arithmetic mean of raw USD close times raw traded shares over the last 63 calendar rows. Observed zero volume contributes zero; absent volume remains missing. |
| Amihud63 | Mean absolute simple adjusted-close return divided by same-day raw dollar volume over 63 complete return rows; zero denominator is unavailable. |
| Beta252 | Regression with intercept against the prior-known-cap market, using 252 complete original-calendar pairs. |
| Residual volatility252 | Population daily residual SD from that same intercept regression; no annualization. |
| Momentum252 skip21 | `log(close[d-21])-log(close[d-252])`, requiring valid interior returns. |
| Short reversal21 | `log(close[d-21])-log(close[d])`, requiring valid interior returns. |

For the market interval ending d, weights and membership are frozen from qualified cap, identity and membership at decision d-1. Every membership state must be known; each member must have positive qualified cap and a valid identity, at least two members must exist, and every weighted return must be observed. There is no zero-return imputation or silent renormalization over unknown support. Unknown-member/cap/identity counts, observed/missing-return counts and reason bits are retained. Cap-weight coverage is null/NaN when its denominator is not known. No relaxed support knobs were introduced.

Price bars require independently qualified source presence and availability at or after their observed mark but strictly before their own decision. Late bars remain unavailable under the named on-time rule; they never retroactively rewrite rolling history. Membership and price presence remain separate. Prior-window inputs precede the current mark; beta/residual-volatility availability additionally includes the latest required current constituent price/presence clock. This clock correction changes metadata attribution, not the decision-time calculation. Optional return guards require a named parent and exact per-row flags.

Adjusted closes are used only in same-security ratios/log differences. Raw close and raw share volume form dollar volume. The D6 adapter checks the pinned manifest, full axes, namespace, membership parent and field-basis metadata before mapping, verifies known membership against the captured D6 row, and reads its dedicated original-f64 close channel. Raw close/volume retain their stored precision, including f32 where declared; widening and all accumulation are f64. The provider does not claim bit-identical dollar volume to an original f64 source that D6 quantized. Plain Panel numeric axes, field basis and source SHA remain caller declarations; this interface authenticates no vendor or filing evidence.

Rolling state is O(253*N): fixed close/return/liquidity rings, invalid-return prefixes, compensated sums and centered add/remove regression moments. Periodic chronological reseeding and cancellation/nonfinite guards prevent a departing extreme from poisoning later moments. Near-perfect regression residual sums use actual centered residuals instead of clamping a cancelling difference to zero. A joint admission envelope reserves the evidence source's declared peak retained/decode memory, provider state, returned date diagnostics, one D6 mapped chunk and metadata, plus the retained I2 builder/output. Oversized copied parent metadata is rejected before allocation. These are bounded ownership/admission contracts, not measured RSS or throughput results; pre-existing caller-owned Panel storage is borrowed separately.

Postimplementation fixtures (all runtime pending):

- `SixComputedDescriptorsMatchIndependentScalarRecipes`: independent full scalar price/market/regression formulas, then the already-qualified I2 normalizer, plus raw warmup and aggregate publication clocks.
- `PriorCapAndStrictEqualityNeverUseTodaysWeightsOrUnknownDenominators`: prior-date weights, exact equality exclusion, and unknown membership/cap denominator reporting.
- `FutureMutationAndTruncationPreserveEarlierRows`: identical extracted values across a truncated prefix and later price/cap/volume mutations.
- `AbsentFiniteMarksLateBarsAndZeroVolumeRemainDistinct`: finite777 behind absence, on-time equality boundary, no gap compression, real zero volume versus unavailable Amihud.
- `D6UsesExactCloseAndChecksPinnedAxesBasesAndJointBudget`: synthetic f32 field storage with exact-f64 close parity, manifest/axis/basis/member mismatch and byte/oversized-metadata refusal.

Source-only `git diff --check` and a lexical delimiter scan passed. Review found two missing closing braces and unbounded parent metadata copying; both were corrected before any compile. No performance, real 2012–2019 t3000 artifact, PIT sector acquisition, price-vintage authentication, cap construction, exposure stage/disk publication, or full original I2 acceptance is claimed. D3/D4 qualified cap/classification acquisition and the actual persisted stage remain dependencies.
