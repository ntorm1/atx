# Intrinsic lower bound for native fit band edges

`SliceDeAmModel::edge_bounds` now uses the larger of intrinsic and the existing
European lower bound at `kIvMin`. The accurate American kernel clamps its result
to the same intrinsic subtraction, so this lower bound requires no additional
rounding allowance. The European leg retains its relative plus absolute margin,
and a nonfinite European bound remains nonfinite. The cap and floor upper bounds
are unchanged.

This lets a bid at intrinsic, including a zero bid, prove that it does not exceed
the floor price. An endpoint solve is still required if any other band comparison
is uncertain. In particular, a loose family upper bound can still force a solve
for a narrow spread near exercise. No native price-domain gate is widened.

The regression covers calls and puts with both positive intrinsic and zero bids.
Its midpoint explicitly clears the independent family and nonmonotone-band upper
bounds, isolating the lower-bound decision. Before the change, all four cases
priced both endpoints; afterward, all four avoid both solves and preserve the
expected bid route and midpoint admission. Independent cold-kernel checks cover
both sides, seven signed rate/carry pairs, five strikes and three maturities;
unsupported regimes remain explicit errors.

Validation: 34 native-route and de-Am cache tests pass, including scalar/batch
parity, lazy band preparation and accurate endpoint comparisons. Red and green
receipts are `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/k6/`
`{red,green}-intrinsic-floor.xml`. PCH-off owning source/test checks also pass.
This change has no isolated or whole-fit latency claim.
