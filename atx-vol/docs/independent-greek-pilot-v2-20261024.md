# Second independent Greek pilot: better reference coverage, remaining refusals

The prospectively declared two-DIRK experiment resolves Delta for 10 of 12
contracts and Gamma for 7 of 12 under the unchanged empirical decision rule.
All 144 requested solves are retained: 138 computed and six refused. The first
pilot remains unchanged at 7/12 Delta and 1/12 Gamma; its CN failures were not
removed retroactively.

Every original native field/shock comparison was repeated against the newly
frozen references: **1,386 pass, 54 fail, 312 candidate refusals and 552 unresolved
references**, totaling 2,304. The same three short-expiry Delta contracts account
for every failure. No production numerical default or accuracy requirement was
changed, and this does not qualify the complete Greek bundle.

## Fixed experiment and independent checks

The declaration is `f8182a29a9eaf1ed4e3a30ca44dc24ff26b1ec48`,
`bench/independent-reference/greek-plan-v2.md`. Reviewed implementation
`a5c8fe79c751430b348bad35b99eff7ba9306920` ran with clean, unchanged source.
It uses the same twelve IDs, two DIRK variants and six grids: center 8193/512,
space 4097/16385, time 256/1024, and a doubled domain retaining center spacing.
No result selected a replacement grid or solver tolerance.

The standalone SciPy PDE calls no ATX or QuantLib numerical implementation.
The temporal variants remain stage-LCP adaptations motivated by
[In't Hout's study](https://arxiv.org/html/2401.13361v1), rather than an
implementation inheriting that paper's penalty-solver convergence theorem.
All twelve results for a contract must supply the same unambiguous regime.
The original refinement-ratio, additive uncertainty, cancellation allowance
and two-shock monetary reserve rules remain in force.

The three predeclared no-exercise controls also have independent closed-form
price, Delta and Gamma values. The g000 call and negative-rate European put
derivatives lie inside their PDE intervals. The zero-rate put has refused PDE
rows, so its analytic values do not replace the failed experiment. The g000 call
Gamma remains unresolved despite analytic interval agreement because its
uncertainty exceeds the reserve. Agreement alone does not certify the interval.

## Complete reference denominator

| Contract | Delta | Gamma |
|---|---|---|
| g000-put-03 | Resolved | Reserve failed |
| g000-call-03 | Resolved; analytic interval passed | Reserve failed; analytic interval passed |
| g001-call-03 | Resolved | Reserve failed |
| g008-put-02 | Resolved | Resolved |
| g010-put-03 | Resolved | Resolved |
| g011-put-02 | Resolved | Resolved |
| g017-put-03 | Resolved | Resolved |
| g042-call-06 | Resolved, exercised | Resolved, exercised |
| zero_rate_closed_form-put-03 | Six solves refused | Six solves refused |
| negative_rate_european-put-03 | Resolved; analytic interval passed | Resolved; analytic interval passed |
| double_boundary_diagnostic-put-03 | Resolved | Resolved |
| sigma-family-near-edge-put-00 | Refinement ratio failed | Reserve failed |

The three short-expiry Gamma uncertainties are approximately 5.36e-5 to
5.49e-5. Their stress-shock contributions exceed the unchanged 2.5e-5 USD/share
reference reserve. Near the exercise boundary, Delta uncertainty is 2.19e-5
and Gamma uncertainty is 0.00215328; neither is promoted.

All six solver refusals concern the zero-rate ATM put. Both schemes refuse
space-fine, time-coarse and domain-wide. Residuals range from
1.0375345027568983e-10 to 1.021263074107992e-9 against the unchanged 1e-10
tolerance. These failures occur even where the model has a closed-form price.
Before choosing another discretization or stopping rule, inspect the actual
policy residual, active-set history and arithmetic conditioning. The current
evidence does not distinguish iteration cycling from a rounding floor.

## Production comparison and next numerical work

All 54 failures again concern legacy spot steps on one-day, 5%-volatility
contracts, including designs that change only the time step, across all three
pricing profiles. For default-profile Delta at a 1% spot shock:

| Contract | Legacy discrepancy plus uncertainty, USD/share | Existing diffusion-scaled spot step |
|---|---:|---:|
| g000-put-03 | 0.000436269874 | 0.000001907785 |
| g000-call-03 | 0.000335131018 | 0.000001185505 |
| g001-call-03 | 0.000235713600 | 0.000000814142 |

The existing alternative uses `min(0.001*S, 0.02*S*sigma*sqrt(T))` at scale 1.
The expanded set adds resolved Gamma comparisons without adding a failed
Gamma comparison. It still leaves the three short-expiry Gamma references
unresolved; passing their Delta controls does not settle their Gamma accuracy.
All 312 candidate refusals remain refusals, including cases where the PDE can
compute a derivative that the production API does not supply.

The component cap and reserve are proposed Greek engineering controls, separate
from the user-approved SPY price budget. All 703 unselected contracts and 6,912
other-field/shock comparisons remain uncomputed. Production step changes require
the broader corpus and actual consumer contract; this pilot supplies evidence
for that work without declaring it complete.

## Evidence

Evidence root: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`.

- Pilot manifest `independent-greek-pilot-v2/manifest.json`:
  `cd13a2d26c1cfea6356a87a4a0a6a9d9e7f474a663851367b24f802347f9ec25`.
- All outcomes `independent-greek-pilot-v2/outcomes.jsonl`:
  `f93caa9032237d362bd969bb518e1bb0d988a78bf0e3b4d9d7ed2a510dbfd4b3`.
- Reference summary `independent-greek-pilot-v2/summary.json`:
  `34fd09b562e97d9f18a2c3c58377dcee701ff7e2d9d46155e9dac161e2cecd9f`.
- Full comparison `independent-greek-comparison-pilot-v2.json`:
  `558e17734f9af8e8e937fa51e94a46579321807cae54827804230a4c205bde5e`.

The reader verifies archived sources against their frozen Git commit, the
original declaration, every request and grid, stencil reconstruction, solver
tolerance, decisions and native receipt before comparing. Twenty-three local
controls pass. The updated reader also reproduces every reference decision and
all 2,304 comparisons from the original pilot exactly; that compatibility check
is retained as `independent-greek-v2-v1-compatibility-v1.json`. No timing is
qualified by this offline experiment.
