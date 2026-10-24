# Exact carry reuse in bounded family batches

`family_prices_bounded` previously recomputed the same two discount exponentials
at every premium quadrature node for every admitted request. A family fixes its
expiry, rate, side and quadrature. Only the request's effective yield changes the
internal carry. The batch now retains these discounts, plus the European forward
growth and discount, until the exact yield bits change.

The scratch belongs to one batch call and has no external lifetime or ownership.
It is populated only after the existing family/envelope identity and request
admission checks. Invalid requests do not populate it. A changed yield, including
a signed-zero change, rebuilds the carry; a new batch starts empty. Stale envelopes,
unsupported profiles and scalar fallback retain their previous behavior. No
allocation, shared state, changed numerical order, approximation or enlarged
price tolerance is introduced.

All remaining price and bound expressions keep their previous operation order.
The underflowed-boundary fallback still calls the original scalar bound. Work
counters distinguish carry preparations from fused/scalar price evaluations;
premium-discount exponential counters now record those preparations rather than
charging the reused exponentials again for each price.

The runtime-red test observes six preparations where three are needed in each
of two repeated batches. After implementation, repeated and changing yields,
interspersed invalid requests, both option sides, negative carry, signed zero and
stale envelopes pass. Batched price/bound bits equal isolated one-request batches.
The existing independent cold-price and scalar-bound comparisons also pass.
Across the focused family-envelope suite and integrated geometry-invalidation
test: 16 pass, one explicit timing probe is skipped. The owning production and
test translation units compile with PCH disabled. The added negative-carry test
initially used an inadmissibly wide q box; the final test uses the existing
supported negative box and a narrow box across zero.

Evidence root:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`:

- `bounded-carry-red.xml` and `.log`;
- `bounded-carry-final-v2.xml` and `.log`;
- `bounded-carry-hygiene.log`.

Whole-SPY output identity, the complete cold-reference audit and quiet release
measurements remain integration gates. This source change has no isolated or
whole-fit latency claim yet. It does not alter the empirical class of the family
envelope or supply a new mathematical error guarantee.
