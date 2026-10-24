# Preserve a cap bound when the floor requires pricing

The native de-Am band path previously discarded all bounded comparisons if any
floor comparison remained uncertain. It then priced both `kIvMin` and `kIvMax`.
It now retains the cap proof when the bid, ask and their midpoint are each strictly
below the existing European cap lower bound. Only the floor endpoint is priced.

This is an endpoint comparison optimization. It changes no family domain, native
inversion gate, price allowance or quote admission rule. Equality with the lower
bound, a lower bound below the ask, or a NaN lower bound still prices the cap.
The floor follows the same configured kernel, ambiguity margin and accurate
reprice sequence as before. Required endpoint failures still leave the band
unusable. A bound-decided cap keeps its cache price NaN while the floor can be
finite; native consumers already use the explicit comparison results.

`model_prices` counts the actual initial endpoint calls: one for a retained cap
proof, two otherwise. Accurate ambiguity reprices remain separately charged as
before. `edges_priced` continues to count bands, not individual endpoint prices.
The existing endpoint clock and AL reason counters measure the integrated cost.

Validation: four focused regressions cover strict versus equal/unknown bounds,
calls and puts, unchanged non-native behavior, cached repeat calls, floor
ambiguity resolution, and floor/cap errors. The existing cold American endpoint
oracle now checks mixed priced-floor/bounded-cap entries as well as fully bounded
entries. All 38 native-route/de-Am cache tests pass, including scalar/AVX2
decisions and lazy band preparation. PCH-off owning source/test checks pass.

Red and green receipts are under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/k6/`:
`red-partial-cap.xml`, `green-partial-cap-unit.xml`, and `green-partial-cap.xml`.
No whole-fit timing improvement is claimed without the integrated measurement.
