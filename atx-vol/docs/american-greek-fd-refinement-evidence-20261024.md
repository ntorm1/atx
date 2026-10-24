# Greek FD refinement: frozen twelve-case evidence

Scale-aware spot and maturity steps materially improve the measured short-T
gamma/theta cases without increasing the boundary solves for one bundle.
They do not resolve the near-exercise-boundary quadrature and derivative
stability problem. No production steps or accuracy contract change in this
experiment.

The clean source is `c116bbae8f3d56c7e3e7a51a3463f8325c60c843`, including the
reviewed private observation seam and its short-circuit accounting fix.
Receipts live at
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/greek-fd-refinement-v1`.
The five manifest artifact hashes were independently checked:

| Artifact | SHA256 |
|---|---|
| Archived executable | `152285f3c51902474f77d07d2e49605642b0e931ae5b626b4b30b0a1114f1d05` |
| Native report | `71d4bc1285ec7e520e8d78e464072db17600885909dd22e02cb37dc0901a62d3` |
| Summary | `f8850bf713a2d46ef0a0a480edeb89bae86cf560d6a4d297ab981f2bf75d2e17` |

This is twelve explicitly selected contracts, not the complete 715-case
qualification corpus. Each case has 48 design/profile/scale evaluations and
nine requested risk fields. All **5,184 outcomes** are retained: 3,519 computed
estimates, 384 exact exercised outcomes, 321 stencil-crossing refusals, 384
API-refused fields and 576 absent Phi fields. Supplied values total 3,903.
The unsupported contract remains one refused contract in every design/profile.
Every legacy/default price and all eight represented risk fields match the
frozen full715 receipt: 12 price outcomes and 96 risk outcomes, bit-identical
where finite and with matching nulls. Independent Python reconstruction and
all seven harness tests pass, including 816 separate Black closed-form sample
prices. The owning native TU passes its PCH-off check.

Peer review then found that the verifier trusted an exported boundary-required
flag. `029f13812df7b9974778a9b40e371f29313f0016` independently derives that flag
from each sample model, with a runtime RED mutation and nine passing tests.
`summary-hardened-v2.json` is byte-identical to the original summary;
`verification-hardened-v2.json` binds that revalidation, the verifier hashes and
the twelve exact legacy metadata joins. The original native/manifest files
remain immutable.

The experiment uses legacy steps once and spot-only, time-only and combined
refinement at scales 1, 1/2, 1/4, 1/8 and 1/16. The spot base is
`min(.001*S,.02*S*sigma*sqrt(T))`; the maturity base is `.002*T`.
Volatility/rate steps remain fixed to isolate the two changed axes. Three
quadratures are retained: default accurate Reference, refined24 and refined32.

| Default quadrature, twelve cases | Requests | Supplied | Crossing refusals | Cold boundary solves |
|---|---:|---:|---:|---:|
| Legacy | 108 | 77 | 11 | 57 |
| Spot only, scale 1/4 | 108 | 79 | 9 | 57 |
| Time only, scale 1/4 | 108 | 82 | 6 | 57 |
| Combined, scale 1/4 | 108 | 85 | 3 | 57 |

Each row also includes twelve absent Phi fields, eight API-refused fields and
eight exact exercised fields. The three remaining combined crossing refusals
are preserved. Increased availability means the actual smaller stencil stays
within one sampled exercise regime; it does not prove derivative accuracy.
Each design has the same 137 prepared-boundary price calls, 50 European price
calls and one opaque public fallback call in this selected subset. Timings
include observation and ran under contention; they are not latency evidence.

The two frozen short-T failures isolate different causes:

| Contract/field | Legacy default | Combined scale 1/4 | Prior refined reference |
|---|---:|---:|---:|
| `g001-call-03` Gamma | 1.5056756387 | 1.5239342763 | 1.5239342763 |
| `g001-call-03` Theta | -20.401078063 | -20.063723580 | -20.063723580 |
| `g010-put-03` Theta | -312.570481148 | -307.170815272 | -307.170815272 |

Spot-only changes Gamma while retaining the old Theta; time-only changes Theta
while retaining the old Gamma. Both axes affect Charm. Agreement in the last
two columns is **not independent verification**: scale 1/4 reproduces the
prior reference's spot/time stencil, and these cases have negligible profile
differences. At scale 1/16, the call Gamma is 1.5239374635 and put Theta is
-307.1708063182. Those further changes remain in the report.

The European control independently shows the same truncation-error direction:
Gamma absolute error falls from `3.8548e-8` (legacy) to `2.4419e-9` (combined
1/4) and `4.8930e-10` (1/16). Theta errors are `4.9648e-7`, `1.2412e-7` and
`7.7499e-9`. These are one continuous-carry European control, not an American
domain accuracy claim.

The near-edge SPY fixture prevents promoting a blanket smaller-step rule:

| Scheme and scale | Gamma | Theta | Charm |
|---|---:|---:|---:|
| Default, 1/4 | .0082309527 | -.3376758681 | -1.6004279736 |
| Default, 1/16 | .0081540737 | -.3376773425 | -1.5768943202 |
| Refined32, 1/4 | .0082590911 | -.3373631081 | -1.5927192439 |
| Refined32, 1/16 | .0082880958 | -.3373652104 | -1.6008565291 |

Default Theta stabilizes to a value about `.0003128` away from refined32 at
scale 1/4. Smaller steps alone cannot remove that model-quadrature difference.
Even refined32 Gamma and Charm move materially as the spot stencil shrinks;
the absence of a sampled exercise crossing is insufficient evidence of
derivative stability. The simple floating-point cancellation indicator is
smaller than these movements, so it must not be presented as a total numerical
error bound. The deep-exercise case continues to supply the exact payoff
derivatives under every tested step/profile.

The report also expresses derivative discrepancies as individual Taylor-term
differences for explicit shocks: 0.1% spot, sixty calendar seconds and one basis
point of volatility/rate. For example, the legacy short-T put Theta discrepancy
contributes about `$1.0273e-5/share` over sixty seconds; the call Gamma discrepancy
contributes about `$9.1293e-5/share` at a 0.1% spot move. These are neither full
repricing P&L errors nor approved Greek budgets. A zero discrepancy against a
shared stencil carries the same correlation limitation as the table above.

Next work should require a stable multi-step, multi-profile reference window
and retain uncertainty separately from candidate error. The accompanying
proposed engineering risk contract defines explicit operational/stress shocks
and provisional gates for validation; it does not borrow approval from the
SPY price-fit budget. An adaptive implementation could reuse boundary models
across step levels: for two time scales the union has five fixed
base/vol/rate models plus four time models, rather than two independent
seven-model bundles. That nine-model count is a design opportunity, not a
measured implementation or an accuracy justification.
