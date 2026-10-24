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
