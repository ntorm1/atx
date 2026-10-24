# Independent American Delta/Gamma: reference failures and confirmed FD error

The fixed independent pilot does not qualify a full Greek bundle. Of216 requested
PDE results,215 computed and one explicitly refused. The frozen all-three-scheme
decision resolves Delta for7/12 contracts and Gamma for1/12. All703 other corpus
contracts and the six other cold Greek fields remain uncomputed by this study.

Comparison against every48-entry native FD evaluation retains2,304 field/shock
outcomes: **618 pass,54 fail,312 candidate refusals,1,320 unresolved references**.
These use the proposed hedge-risk contract, not the user-approved SPY price
budget. No production numerical default or reference price was changed.

## What was calculated independently

Source `23c7207e30d2cb692eaa88ff8af72fbef8b34e60` uses an offline SciPy
tridiagonal PDE, exact discrete obstacle solves, and a spot-centered log grid.
It calls no ATX/QuantLib price, boundary or Greek implementation. Three-point
Delta/Gamma comes from the solved grid, with actual final LCP residuals defining
the stencil regime. Mixed or ambiguous stencils retain a price and refuse Greeks.

The three temporal schemes are CN and two DIRK variants, all on a quadratic
clock with two initial backward-Euler intervals. The stage-LCP adaptation is
experimental. [In't Hout's primary study](https://arxiv.org/html/2401.13361v1)
motivates the damping comparison; its penalty-solver convergence results are not
automatically inherited by this implementation.

The predeclared six grids independently vary space, time and domain. All18 results
per contract must supply the same regime; adjacent space/time drifts must meet
the fixed ratio test. The reference uncertainty adds scheme disagreement,
space/time/domain drift and a cancellation indicator. Both0.1% and1% spot shocks
must leave uncertainty within25% of the proposed0.0001 USD/share component cap.
This is an empirical decision, not a rigorous numerical error bound.

## Full reference denominator

| Contract | Delta reference | Gamma reference |
|---|---|---|
| g000-put-03 | Resolved | Uncertainty reserve failed |
| g000-call-03 | Resolved | Uncertainty reserve failed |
| g001-call-03 | Resolved | Uncertainty reserve failed |
| g008-put-02 | Refinement ratio failed | Refinement ratio failed |
| g010-put-03 | Resolved | Refinement ratio failed |
| g011-put-02 | Refinement ratio failed | Refinement ratio failed |
| g017-put-03 | Resolved | Refinement ratio failed |
| g042-call-06 | Resolved, exercised | Resolved, exercised |
| zero_rate_closed_form-put-03 | One solve refused | One solve refused |
| negative_rate_european-put-03 | Refinement ratio failed | Refinement ratio failed |
| double_boundary_diagnostic-put-03 | Resolved | Refinement ratio failed |
| sigma-family-near-edge-put-00 | Inconsistent stencil regimes | Inconsistent stencil regimes |

The sole numerical refusal is CN/space-fine for the zero-rate put: the final
obstacle iteration residual1.647094904910773e-9 exceeds its fixed1e-10 tolerance.
No larger tolerance or substituted European value was used to rescue this row.

## A temporal-method defect in the reference candidate

For g017-put-03 Gamma, CN's space-fine drift is0.00236836334 and its coarse-time
drift0.00254239979. The L-stable DIRK scheme's spatial drifts instead decrease
from2.40375869e-7 to6.00905780e-8, and temporal drifts from5.40145771e-8 to
1.37724636e-8. The second DIRK scheme also decreases steadily. The negative-rate
European control exhibits the same CN-specific pattern.

This supports a method-level diagnosis: increasing spatial resolution without
adequate temporal damping can make Gamma worse while prices look plausible.
It does not justify deleting CN from this completed pilot's decision. Those
Gamma references remain unresolved under the original rule. The follow-up
declaration below changes the candidate reference method prospectively and
retains CN as an explicit failed diagnostic.

## Confirmed short-expiry Delta error

All54 failed comparisons concern the same three one-day,5%-volatility contracts.
They occur with the legacy spot step, including designs that change only the
time step, across all three native pricing profiles. On a1% spot shock:

| Contract | Legacy discrepancy + reference uncertainty, USD/share | Existing diffusion-scaled spot-step experiment |
|---|---:|---:|
| g000-put-03 | 0.0004379410664 | 0.0000035789773 |
| g000-call-03 | 0.0003358497620 | 0.0000019042496 |
| g001-call-03 | 0.0002361590291 | 0.0000012595705 |

The default legacy step is0.001*S. The existing experiment uses
`min(0.001*S,0.02*S*sigma*sqrt(T))` at scale1, leaving the model and price profile
unchanged. This is an independently supported improvement for these Delta
contracts. Gamma is still unresolved here; better-looking Gamma numbers cannot
promote it. Full-corpus/other-field evidence remains necessary before changing
the public FD convention.

The double-boundary and near-exercise native refusals remain in the comparison.
No missing Greek is converted to zero, and a refused candidate cannot receive a
pass merely because its reference was computed.

## Evidence and review controls

Evidence root: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`.

- `independent-greek-pilot-v1/manifest.json`: clean source before/after,
  immutable snapshots and complete215/1 census.
- `independent-greek-pilot-v1/outcomes.jsonl`: SHA256
  `2ee9f1e8ebbbdaa213c9c77a4046947491bdd6d074be2d901d4de8c562686329`.
- `independent-greek-comparison-v2.json`: SHA256
  `6601451d54b78afc3ebd622eafb8af6d52d75ac8fb2756be8233b3dfe3d784f7`.
- Native48-evaluation FD receipt: SHA256
  `71d4bc1285ec7e520e8d78e464072db17600885909dd22e02cb37dc0901a62d3`.

The external reader joins archived source bytes to their exact Git commit,
models and both inner/outer grid controls, including width/spacing/domain,
solver tolerance, stage/work counts, intrinsic and actual stencil residuals.
It reconstructs derivatives and reproduces every original reference decision.
The existing native reader independently revalidates its options, availability
masks and price stencils before comparison.

Review found and closed an omitted grid/tolerance join; its malformed receipt
test is retained. A Windows Git object-path separator error and the native
exporter's sorted ID order each produced retained failed verification runs.
Neither changed the numerical pilot or its selected set. Reviewed verifier
source is `3df29a9df80f5c1f1ec867a29381a230c95d32fd` atop `a3100e63`.

The first prerequisite incorrectly required monotone tiny Gamma errors for an
unaligned sampled payoff kink. The error was around1e-7, already inside the
absolute control. The retained correction checks convergence on an aligned
kink and absolute accuracy on the unaligned case; the pilot still charges all
grid-phase drift. Eleven PDE/prerequisite tests, three decision tests and three
external-reader tests pass. No latency was qualified.

## Prospective next reference experiment

Before any new American outcome, freeze the next plan in
`../bench/independent-reference/greek-plan-v2.md`. It uses the two damped DIRK
schemes and finer nested grids, keeps all12 contracts and the original216
outcomes, and preserves the same monetary reserve and refusal rules. The
previously failed CN scheme remains a diagnostic, rather than an accuracy
authority. This selects a numerical method using reference convergence, not
whichever method agrees most closely with the production candidate.
