# Reusing a native root's monetary enclosure

The qualified native inversion already encloses the cold Reference root between
two volatility endpoints. When both endpoints are accepted from the retained
family's validated error envelope, the same two prices also bound the monetary
residual of the returned IV, which is clamped inside that bracket.

For target `P`, family endpoint prices `pa`, `pb` and their error bounds `ea`,
`eb`, the existing monotone-domain qualification gives the residual bound
`max(P - (pa - ea), (pb + eb) - P)`. Endpoint price bounds and the final residual
are rounded outward with `nextafter`. No point-vega approximation is used.
This reuses the existing refinement-validated envelope; it does not strengthen
that envelope's error class or independently establish global monotonicity.

`FamilyEnclosure::bound_price` and `RootAcceptance::bound_price` default to
infinity. Only a closed paired-family enclosure supplies a finite bound.
Confirmed roots, legacy spread-gate acceptance and open enclosures keep infinity.
The scalar and K6 batched paths preserve their previous root/bracket bits and
decisions; a batch bound can be more conservative than its scalar counterpart.

Repaired-price identification consumes the bound only for an accepted native
`Skipped/FamilyEnclosure` root inside its recorded bracket, and only when it
meets the immutable per-contract raw-spread budget. A missing or insufficient
bound retains the existing family-center evaluation, cold Reference check and
typed fallback. Zero-spread policy and default API behavior are unchanged.
`reimply_price_enclosure_hits` counts skipped center evaluations; it is separate
from actual family-center and cold-check counts.

Validation covers scalar/batch brackets against independent cold prices, open
and confirmed bounds remaining absent, native/default publication bit parity,
cached query behavior, and a tight budget requiring cold checks and rejected-root
fallback. The focused run passed 23 pricing and 16 repair/cache tests, plus owning
PCH-off compilation. Integrated SPY accuracy and performance remain separate
qualification gates; this change makes no latency claim.
