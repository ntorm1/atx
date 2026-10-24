# Independent Delta/Gamma study, fixed before American results

This offline experiment leaves the existing price reference and all shipping
APIs unchanged. It reuses only the independent SciPy tridiagonal obstacle solver
and deterministic far-boundary envelope. No ATX or QuantLib numerical prices,
exercise boundaries, interpolators or derivatives enter the calculation.

The temporal comparison follows the two-stage equations in
[in't Hout (2024), equations 3.4 and 3.12](https://arxiv.org/html/2401.13361v1):
CN, DIRK with theta=1-sqrt(0.5), and DIRK with theta=1/3. All use a quadratic
time grid and two initial backward-Euler steps. Each implicit American stage
solves its discrete complementarity problem directly instead of using a large
penalty. This adaptation is an experiment, not a reproduction of the paper's
penalty solver or an inherited convergence guarantee. Stage boundary terms use
the appropriate old/new times; the final stage consumes the first-stage result.

The independent log-stock grid is centered on the requested spot, which is an
exact node. Its half-width is
`max(0.1, 8*sigma*sqrt(T)+abs((r-q-sigma^2/2)*T), abs(log(K/S))+0.1)`
times the declared domain multiplier. Initial payoff is sampled without cell
averaging. The generator uses central differences and refuses non-monotone
off-diagonals or an unrepresentable domain. Three-point differences give
`Delta=V_x/S` and `Gamma=(V_xx-V_x)/S^2`. The returned price is the center node;
no price interpolation or bumped production prices are involved.

Each final-stage LCP row retains its actual gap and continuation residual.
Three continuation rows permit numerical derivatives. Three definitely active
rows with positive intrinsic permit exact affine exercise derivatives. Mixed
or numerically ambiguous stencil states return explicit unresolved derivatives,
while preserving the computed price and residuals. This local discrete evidence
is not a proof of the true continuous exercise region.

Before American comparisons, require independent European price/Delta/Gamma
controls on both sides, zero-rate American/European equivalence, exercised affine
limits, mixed/ambiguous stencil refusal, invalid input and work-budget refusals.
Retain failed controls; do not tune the pilot to candidate errors.

Use exactly the twelve IDs already frozen in
`../american-greeks/refinement-v1.json`, from the full715-case corpus. Preserve
the other703 IDs as uncomputed and all eight requested cold Greek fields; this
experiment addresses only Delta/Gamma. For each of three schemes run the six
fixed `(nx,nt,domain_scale)` requests:

| Label | nx | nt | Domain multiplier |
|---|---:|---:|---:|
| space-coarse | 2049 | 512 | 1 |
| center | 4097 | 512 | 1 |
| space-fine | 8193 | 512 | 1 |
| time-coarse | 4097 | 256 | 1 |
| time-fine | 4097 | 1024 | 1 |
| domain-wide | 8193 | 512 | 2 |

There are216 requested results, including every refusal. Domain-wide preserves
center-grid spacing. No adaptive extra grid is hidden in this pilot. Per solve:
`nx<=32769`, `nt<=4096`, and at most2^27 stage-node updates (two stages charged
for DIRK). The direct LCP solver itself has its existing finite iteration limit.

## Frozen empirical reference decision

Apply this rule independently to Delta and Gamma, before inspecting candidate
error. All18 results for an ID (three schemes, six configurations) must succeed,
supply the field and have the same unambiguous stencil regime. Never omit a
refused or mixed/ambiguous result to improve the envelope. Keep sampled-payoff
kink/grid-phase drift; no retrospective mesh alignment or averaging correction.

For each result define `p=32*epsilon*max(1,max(abs(stencil_values)))` and its
roundoff indicator as `p/(S*dx)` for Delta or
`p*(4/dx^2+1/dx)/S^2` for Gamma. This is an engineering cancellation indicator,
not a rounding-error proof. Let R be the maximum indicator over all18 results.
For each scheme compute the coarse-to-center and center-to-fine absolute drifts
on both the space and time axes. Require each fine drift to be at most half
its coarse drift plus `2*R`. No selection of a more favorable three-grid window
is allowed. Exercise derivatives may have zero drift.

The reference center is the midrange of the three schemes' center results.
Its empirical uncertainty is half that center range, plus R, plus the maximum
over schemes of `(max(space drifts)+max(time drifts)+domain drift)`.
Domain drift is the absolute difference between domain-wide and center at equal
spacing. This additive rule intentionally retains each unresolved error source;
agreement between methods does not cancel a grid drift. A failed ratio or
inconsistent regime produces unresolved status even when this number is small.

The proposed contract uses spot shocks0.001*S and0.01*S. For each shock the
coefficient is abs(dS) for Delta or0.5*dS^2 for Gamma. Require uncertainty times
coefficient <=0.000025 USD/share (25% reserve). Compare native candidate error
plus uncertainty times coefficient against0.0001 USD/share, separately for both
shocks. Candidate refusal remains a refusal. These are proposed engineering
gates, not user-approved generic Greek accuracy or a rigorous error bound.
A pair of accurate derivatives does not qualify the other six fields or a full
risk bundle. No timing campaign, reference replacement or production dispatch
change is authorized by this pilot.
