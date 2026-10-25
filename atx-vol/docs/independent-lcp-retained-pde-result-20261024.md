# Retained-policy result on the twelve fixed PDE controls

The retained-policy candidate completed ten of the twelve declared controls.
Both wider-domain requests recovered, while both finest-space requests still
refused. The full control fails; Delta and Gamma remain unresolved and
`not_promoted`. This is progress beyond the four saved-system result, not an
independent reference qualification.

The invocation used clean, reviewed source
`9a2f480366346cd1f683ae591977d5c3dccecb99` and the prospective scope in
`680db3de4fb1c400d85c9a6a406cd4f6c4e9c36b`. Calendar and acceptance approved the
exact source before the parent authorized one run. Acceptance independently
ran all eleven new small controls. The declared PDE requests ran once; there
was no retry, parameter adjustment or historical-control PDE rerun.

## Complete denominator

All twelve requests use the same original put model: S=K=100, T=1, sigma=0.2,
r=q=0. Both DIRK schemes keep their six original grids, graded time mesh,
boundary/payoff equations, 2^27 stage-node guard and unscaled 1e-10 stage
complementarity test. Each stage owns a fresh selector. The original n+2
algorithm bound remains; the separate effective resource cap is
`min(2*nx,256)` linear calls, at most 128 paired updates.

| Grid | nx / nt / domain | Original correction, both schemes | Retained `dirk_l` | Retained `dirk_third` |
|---|---|---|---|---|
| space-coarse | 4097 / 512 / 1 | computed | computed | computed |
| center | 8193 / 512 / 1 | computed | computed | computed |
| space-fine | 16385 / 512 / 1 | refused | refused, stage 453 | refused, stage 391 |
| time-coarse | 8193 / 256 / 1 | computed | computed | computed |
| time-fine | 8193 / 1024 / 1 | computed | computed | computed |
| domain-wide | 16385 / 512 / 2 | refused | computed | computed |

The original correction archive retains eight computed and four refused
requests. The older default study retains six computed and six refused for
these twelve requests. Neither archive nor its decisions changed. All 714
other contracts and the six other cold Greek fields remain uncomputed in this
control.

The two new refusals each exhausted the approved 128 updates / 256 linear-call
cap. Their last original binary64 complementarity residuals are:

| Scheme / grid | Failed stage | Last residual | Original tolerance |
|---|---:|---:|---:|
| `dirk_l` / space-fine | 453 | 1.2820322581319488e-10 | 1e-10 |
| `dirk_third` / space-fine | 391 | 1.4995293895481154e-10 | 1e-10 |

Both are explicit `linear_budget` refusals. They are not relabeled as original
algorithm-bound failures or accepted approximate solutions. The original
correction requests first refused at stages 221 and 190 respectively. The
candidate has different preceding numerical trajectories, so those stage
numbers do not compare identical linear systems.

## Observed work and numerical limits

| Complete recorded work | Original correction archive | Retained candidate |
|---|---:|---:|
| Computed / refused requests | 8 / 4 | 10 / 2 |
| Observed stages | 10,458 | 12,088 |
| Completed stages | 10,454 | 12,086 |
| Corrected policy updates | 78,807 | 33,251 |
| Attempted linear calls | 157,614 | 66,502 |
| Original residual checks | 89,265 | 45,339 |
| Largest observed updates in one stage | 16,385 | 128 |

These are actual counts over the complete observed denominators. The archives
use different effective resource caps and stop at different stages; these
counts do not establish a speedup. The retained run records 38,573,697 retained
override events across stage rows and updates, not distinct rows. Its largest
declared selector reservation is 524,256 bytes; that is not a process peak-memory
measurement.

Among the eight requests computed in both archives, six results changed bits
for each of price, Delta and Gamma. Maximum absolute differences are
2.744471316873387e-13, 1.1224354778960333e-13 and
1.7032694699103956e-12 respectively. Small differences on the common completed
subset do not qualify the missing finest-space results or imply exact parity.

The unchanged refinement rule requires the complete set. Its Delta and Gamma
outcomes therefore remain `unresolved`, with reason `incomplete_or_refused`.
`solver_controls_passed`, `accuracy_established` and `full_bundle_qualified`
are all false. Successful archive validation and process exit zero indicate
complete, consistent evidence, not numerical control success.

## Evidence and next boundary

Archive:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/zero-rate-retained-lcp-control-v1/`.
Its manifest SHA256 is
`a506b01f749af75425b39845fdefa2d910d46132eaa7d102c2e46bcd0d110a9a`.
It contains 37 files totaling 9,573,035 bytes, including both owned failed-stage
NPZ files, all twelve streamed outcomes, per-stage work, source snapshots and
the unchanged historical input archive.

The runner's independent reader completed before publication. It reconstructs
the saved binary64 failed-stage residuals and checks the source, model, grid,
work, refusal phase and summary joins. It does not independently reconstruct
every stage's selector array and does not claim a Fraction/Decimal audit of
these two new final vectors. No additional exact-row audit was performed.

The invocation ran from 23:56:01 to 23:58:42 UTC on 2026-10-24. Host activity
was not qualified for timing; this is an untimed research result. Source was
clean at launch and unchanged at completion. The original correction, Greek
reference and PDE source bytes match the preceding correction archive, as do
all copied historical-v2 bytes.

Approval, launch, comparison script, logs and file receipts are retained under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/lcp-retained-pde-outcome-v1/`.
The comparison reads saved JSON only and performs zero PDE requests, linear
solves or exact-row evaluations.

The saved-stage improvement survives into complete wider-domain controls,
but retention does not eliminate every finest-grid refusal. The two new saved
systems provide the next diagnostic boundary. Any further experiment must
first distinguish remaining selection behavior from correction/residual
arithmetic; this result does not justify increasing the cap or loosening the
original acceptance test. No full144 run, historical decision rewrite or
production change follows from it.
