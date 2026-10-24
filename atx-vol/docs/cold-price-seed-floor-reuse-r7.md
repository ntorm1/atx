# Exact cold seed proof with shared reporting-floor boundaries

The monetary fitting policy now checks an eligible family lane with the existing
full cold price-seed proof after its bounded economic proposal is refused. A
successful proof is prepared and consumed before strict native/K3 confirmation.
Every refusal retains the original route. Strict fitting without an explicit
positive monetary budget is unchanged.

## Proof and lifetime

The accepted sigma is the original solved family lane. The family root and its
residual supply no acceptance evidence: `american_implied_vol_price_seed` still
checks the cold target price and exercise classification, the deterministic target
range, the exact cold price at sigma 0.005, and the positive cold chord when the
target lies in the existing nonmonotone band. The budget comes from the immutable
original quote spread and cap.

`IvPriceSeedFloorCache` owns one geometry and one boundary handle within a single
preparation call. It allocates only after a candidate passes its cold-price check.
Its key includes exact binary64 expiry, rates, yield, internal strike, side and
complete optional AL options, including profile and optional presence. A model
change rebuilds the owned state. Invalid preparation or a failed handle price
falls back to the original cold floor call. There is no global cache, borrowed
external geometry, homogeneity rescaling, or retained fit/archive state.

For calls, the internal strike is the original spot. Multiple strikes can therefore
share one exact floor boundary. For puts it is the original strike; a changed
strike rebuilds the boundary. `AlBoundaryHandle::price_from_boundary` performs
the original premium/European/clamp calculation and checks its anchor and geometry
generation. Its result is the same cold price, rather than a new floor estimate.

`IvPreparedColdSeed` is separate from the family interval proof and strict K3 root
acceptance. Consumption validates the complete request, monetary budget, options,
family/envelope identity and preparation stamp, lane state, sigma and domain.
Successful preparation alone cannot bypass those checks. No raw quotes, projected
prices, classification floors, or reference budgets change.

## Evidence

The diagnostic source remains separate from production:

- `9079f34fe4b87c8c53ec228717acbe463e8706f6`: shadow the full existing seed proof.
- `7fbde0f02eaada16cb4222007908aa8ac48bf27d`: shadow exact floor boundary reuse.
- Evidence root: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/`.

All 496 previously refused economic intervals passed the cold seed proof. The
largest measured Reference price residual was $0.0000647291, or 6.473% of the
applicable budget. None of these targets is in the nonmonotone band, so they require
no chord price. The unshared helper performs 496 seed prices and 496 floor prices;
the ordering change by itself did not demonstrate a saving.

The floor replay found 11 exact model keys. All 496 reused floor prices matched the
original cold values bit for bit, with 11 boundary solves. In the same instrumented
development run, the original floor calls consumed 64.4273 ms; the replay consumed
1.8674 ms for preparation and 2.1775 ms for pricing. These are attribution clocks,
not qualified latency measurements. Complete-fit qualification remains separate.

The production development replay prepared and consumed 496 cold proofs, with
11 floor preparations and 485 hits. Fast native confirmations changed from 1,290
to zero and later Reference price checks from 466 to zero. Across all 13,168 output
rows, only the 496 predicted volatility fields changed; all prices, statuses,
PriceOnly reasons and intervals were identical. The 33 valid slices and their
12,866 rows retained their existing classification and raw-box coverage. This
output comparison does not replace the independent complete API audit.

Focused red/green regressions cover exact call/put floor reuse, options/model/domain
changes, negative-rate and unsupported regimes, lazy preparation, target-floor
refusals, full cold price accuracy, stale family state, changed request/budget, and
strict-default output parity. Integration tests verify actual publication and cached
query metadata. Receipts are `cold-seed-cache-{red,green-v2}.xml`,
`cold-seed-integration-{red,green-v2}.xml`, and `cold-seed-candidate-v1/`.

## Counters

The six `reimply_cold_seed_*` counters distinguish prepared attempts and roots from
consumed `hits`, floor `preparations` and `hits`, and preparation nanoseconds. They
do not increment strict native/K3 root counters or the later ordinary seed fallback
counters. Clocks run only when fitting timing is armed.
