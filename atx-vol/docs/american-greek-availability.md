# American Greek availability

`AmericanGreeks` carries eight sensitivities and a price. Each field has an
explicit requested bit, supported bit and reason. Continuous-carry phi remains
in the separate `CarryGreeks` API. The field order is the existing
`GreekFieldMask` order: delta, gamma, vega, theta, rho, vanna, volga, charm, price.
Moving that enum into `american.hpp` preserves its public bit values.

A default bundle supplies nothing. Its legacy numeric members still default to
zero, but both masks are `None` and every reason is `NotRequested`. A caller must
not infer a supplied value from a finite zero. A partial result may supply the
price and delta while refusing theta; refused requested fields are NaN and carry
their reason. Unrequested fields retain their route's numeric convention without
acquiring support.

`ComputedEstimate` and `Exercised` identify supplied finite values. Neither is a
derivative accuracy certificate. The producing model, its exercise rule, and any
independent accuracy evidence remain separate. The other reasons are
`NotRequested`, `UnsupportedAxis`, `StencilCrossing`, and `Nonfinite`.

The private helper `pricing/american_greek_availability.hpp` centralizes four
operations:

- `american_greek_request` converts the existing three solve-need flags to an
  exact field mask, including the base price/delta/gamma/theta group.
- `mark_american_greek_fields` records a producer's supplied or refused fields.
  Nonfinite supplied values become `Nonfinite`; refusal poisons only those fields.
- `narrow_american_greek_request` removes metadata for unrequested fields without
  changing their legacy numeric slots. Expanding a request cannot resurrect an
  old value: newly requested fields become unsupported.
- `american_greeks_support` checks the exact required fields against request,
  support, reasons and finite values. It rejects unknown mask bits and contradictory
  support metadata. A delta-only request does not require an unrequested theta.

This first component establishes the shared schema and helpers. Existing numerical
producers and downstream gates are migrated in follow-up components; their default
empty metadata must not be interpreted as completed propagation. Appending metadata
changes the C++ object layout, so all consumers must be rebuilt. No pricing or Greek
arithmetic changes in this component, and no Greek error budget or latency claim is
introduced.

The new interface tests first failed to compile because the schema/helper was
absent. With the component implemented, six availability tests and nine existing
batch/mask tests pass. PCH-off owning batch and umbrella objects compile. The
umbrella's public-type and inclusion tests pass; its full run remains 7/10 because
of the previously recorded README header-count mismatch and fixture-path checks
in unchanged inputs. Receipts are `repair-iv/greek-availability-*` under the
`spy-qualified-r7-20261023` benchmark evidence directory. No numerical producer
admission or downstream mask enforcement has yet been changed by this component.

The cached correction graph and native adjoint producers now stamp their computed
fields as `ComputedEstimate`, including explicitly computed zeros in the closed-form
European degenerate limits. Nonfinite results are unavailable. The cached graph's
sensitivities still differentiate its fixed-carry correction model; this change does
not claim they differentiate the separate served-price floor or match cold American
sensitivities. In particular, `american_greeks` with a null correction uses the
documented European graph, while `american_price_cached` with a null correction
falls back to the cold American price. Those are distinct existing contracts.

The session record adapter requests all fields and supplies price, delta, gamma,
vega and theta. It explicitly reports rho, vanna, volga and charm as
`UnsupportedAxis`; their existing NaN values remain. The record's supplied fields
are model estimates, not new derivative accuracy certificates. Producer stamping
changes no finite numeric value or pricing decision. Consumer gates and the separate
cold-stencil admission changes remain follow-up work.

Producer regressions first failed at runtime on the missing metadata. The final
focused run passes 29 schema/cached/adjoint tests and all seven session-record
tests, including 104 certified record contracts. Six owning source/test objects
also pass the PCH-off build. Evidence is `repair-iv/greek-producer-*` and
`repair-iv/greek-record-*`. A separate shared-consumer regression remains RED
until the next component: this producer change does not yet enforce metadata in
the old finite-only caller gates.
The first producer component now stamps cold FD, scalar/prepared AL, and both
direct AVX2 kernels as `ComputedEstimate` for their actual computed groups. The
AL-to-FD fallback narrows metadata to the caller's solve needs without changing
the legacy numeric slots. All pricing, stencil and PDE arithmetic is unchanged;
the known near-edge and deep-exercise numerical counterexamples remain open for
the subsequent admission component. This is not completed consumer propagation.

A new runtime test failed on the previously empty metadata, then passed across
both sides, full/base masks, prepared geometry, the European fallback and direct
AVX2 kernels. Together with six schema tests and three existing batch tests,
10 tests pass. PCH-off checks pass for both producer sources and the owning test.
Evidence: `acceptance/cold-greek-admission-red.xml`,
`acceptance/cold-greek-metadata-green.xml`, and
`acceptance/cold-greek-metadata-hygiene.log`. The separate numerical-admission
regressions in the RED receipt are intentionally not claimed fixed here.

The shared surface/portfolio caller gate now requires supplied-field metadata as
well as finite numbers. Its existing solve-need groups are unchanged. Normalizing
an unrequested NaN to its legacy numeric zero also removes availability metadata;
request expansion never turns that zero into a supplied field.

The public American Greek batch uses the exact output mask instead of requiring
every field in an internal solve group. A price-only request can therefore succeed
when gamma is unavailable; a full request retains the price but reports
`Unsupported`. Requested and supported masks and nine reasons are available through
the workspace's three new `lane_*_view` accessors, with the same borrow/invalidation
rules as its status and route views. Unknown mask bits fail before any output or
workspace write. A supplied finite mark remains a prerequisite for a successful
lane, including requests that do not write a price column.

`reserve_lanes` preallocates the Greek metadata for repeated Greek batches. The
price-only batch uses `reserve_price_lanes`, which allocates no Greek metadata and
invalidates any old Greek views while retaining their capacity. No pricing options,
stencils, or numerical acceptance conditions change in this caller component.
Portfolio partial-result aggregation and Python propagation remain separate work.

The shared-gate regressions failed at runtime on untouched/default zeros and
normalization that retained stale availability. Missing batch metadata first
failed to compile; a separate runtime regression caught allocating Greek metadata
on price-only batches. With the wrapper metadata prerequisite applied, 29 focused
pricing/batch/gate tests pass. Another 50 surface/portfolio/seed checks pass with
one counter-dependent skip. All six owning PCH-off objects pass. Receipts are
`repair-iv/greek-{gate-red,batch-interface-red,workspace-red,consumer-*}`. The
finite-price/nonfinite-gamma test uses the existing tiny-spot fixture on unchanged
numerical arithmetic; the subsequent exercise-admission component must replace
that fixture with a genuine stencil refusal when it supplies exact exercised
derivatives. The partial-result contract itself is unchanged by that migration.
