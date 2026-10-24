# Economic admission of different numerical solutions

The user's 2026-10-24 instruction removes exact reproducibility and numerical
identity as requirements for this performance work. Different algorithms,
optimization paths, fitted targets and reported IVs may qualify independently.
This supersedes earlier forward-looking requirements to reproduce prediction
hashes or the two historical floor targets. Historical results remain unchanged.

## Financial requirements

The price error at a reported IV remains at most
`min($0.001/share, 1% of the original raw bid–ask spread)`, with the separately
documented floating-point roundoff allowance. Duplicate observations use every
original box, including the strictest spread. A locked quote has zero economic
budget. Strict IV error is diagnostic unless explicitly requested.

Every eligible row and valid expiry must still be served. Original-box
containment must be at least 99.5%. Actual API prices must satisfy the existing
American bounds, vertical, convexity, parity and calendar checks. Model/carry
inputs and original observations remain fixed. A reported IV must have valid
floor semantics; PriceOnly must not silently replace previously identified rows.
These checks do not establish cash-dividend model accuracy, Greek accuracy,
extrapolation safety or live-update latency.

The new `economic-floor-identification-v2` contract admits the same two reviewed
floor exceptions (row IDs 27418697 and 27419115) against each candidate's actual
target. It removes the old target-price, implementation-name and representation
pins. It retains the independently reconstructed zero-volatility band, cold
sigma-floor proof, original financial context, valid interval/classification,
node/query consistency and the economic price budget. It does not widen the
floor band to the price budget or authorize additional demotions. The immutable
review fixture is unchanged. Earlier contracts remain explicitly selectable.

The CLI now defaults to `raw-spread-price-v1` and
`economic-floor-identification-v2`. Low-level historical API defaults remain
unchanged for callers replaying old evidence; new callers should name both
contracts. Every verification result records its selected versions.

## Timing proof for different solutions

`run_cold_trials.ps1` records each prediction hash and reports equality as a
diagnostic. Different hashes no longer cause launcher exit 3. Launcher success
means the trials were collected; it does not establish financial correctness.

`spy_acceptance.py` checks each CSV against its own declared hash. A historical
ordinary trial may reuse a previously qualified identical witness under the
same executable, input and policy. This is optional proof reuse, not a required
numerical identity. A different result needs its own complete actual-API proof.

The new native producer is being implemented separately. It will retain the
actual fitted surface, end the cold clock and snapshot fit counters and CPU
usage, write the prediction CSV, and only then probe that same owned surface.
No second fit may stand in for the timed result. Its marker is
`bench.post_fit_acceptance_audit`, schema `atx-vol-post-fit-api-proof-v1`, with
phase `after_cold_publication_and_fit_snapshots`. The nested actual-API payload
has phase `post_fit`.

The Python verifier joins snapshot/member counts and identities, all four
artifact hashes, fit-invocation counts, ownership lifecycle and nonnegative
diagnostic accounting. It then runs all thirteen financial checks against the
trial's own report and CSV. Marked trials always receive their own full proof,
even when their prediction bytes happen to equal the earlier witness. A marker
asserting completion alone cannot qualify a result.

Retaining the surface changes the old benchmark's destruction boundary. The
new clock contract is `retained_surface_publication_v1`: latency ends with a
published, queryable owned surface. Owner destruction is measured separately.
No diagnostic duration is subtracted from the cold clock. One campaign cannot
mix this boundary with the historical destroy-before-clock-end boundary.
Control/candidate comparisons must use the same contract. The existing fresh
process, single-core, affinity, worker, SMT, ThinLTO and hundred-trial checks
remain in force.

## Verification at this checkpoint

The focused Python suite passes 68 tests, including alternative valid targets,
floor/status/model mutations, unexpected demotions, altered artifact hashes,
phase/refit/ownership mutations, different-solution dispatch, and rejection of
mixed lifetime boundaries. PowerShell launcher syntax also passes. The new
native post-fit producer is not yet complete or timing-qualified.

Read-only verification of two saved full-SPY audits under the new contract:

| Saved audit | Old reviewed contract | New economic contract |
|---|---|---|
| Ordinary r8 | 13/13 | 13/13 |
| Whole-QP centrality experiment | 12/13; old floor target pins | 13/13 |

The new centrality admission changes its correctness classification only.
Its measured component work remains unfavorable: factors fell from 35 to 29,
but direction solves rose from 70 to 124. Centrality stays default OFF; no new
speed claim follows from removing the historical target pins.

Evidence directory:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023`.
New results are `economic-admission-v2-{ordinary,centrality}-verification.json`;
old audit reports and receipts were not rewritten. Test evidence includes
`economic-admission-v2-full-green.log` and the separate RED/green records for
floor admission, per-solution verification, post-fit marker checks and timing
dispatch. No fit was rerun to produce these two re-verifications.

## Consequence for implementation work

The constraint census and subsequent bounded row-generation experiment should
compare feasibility, economic error, complete coverage and total work. Exact
output-bit parity is diagnostic. Copied input/constraint bytes and file hashes
remain integrity checks: they establish what was observed, not which solution
must be produced. Reduced bases, different active sets, better stopping rules,
SIMD and parallel scheduling may exploit the full economic error budget while
meeting the same financial obligations. No qualified subsecond result or
worldwide performance lead has yet been established.
