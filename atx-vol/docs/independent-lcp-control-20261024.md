# Independent LCP correction control: eight computed, four refused

The prospectively declared twelve-request offline reference control ran once at
reviewed source `8f4f81243e19124fd203ede551c41dba8085fb07`. It computes eight
requests and refuses four. Both previously refused coarse-time requests recover;
all six previous successes remain computed. **The numerical control fails**,
and neither Delta nor Gamma is promoted. This is independent reference research,
not a production fit or an HFT latency result.

Both reviewers approved the exact primitive and harness before execution. The
run preserves the original zero-rate put, both DIRK schemes, six v2 grids, payoff,
stage equations, `1e-10` unscaled complementarity tolerance and policy-update
bound. Exactly one binary64 correction follows each selected-policy solve.
There was no second twelve-request run, changed tolerance or full144 experiment.

| Grid | dirk_l | dirk_third |
|---|---|---|
| space-coarse | computed; previously computed | computed; previously computed |
| center | computed; previously computed | computed; previously computed |
| space-fine | refused; previously refused | refused; previously refused |
| time-coarse | computed; previously refused | computed; previously refused |
| time-fine | computed; previously computed | computed; previously computed |
| domain-wide | refused; previously refused | refused; previously refused |

Every remaining failure reaches the original `n+2=16,385` policy-update limit
for its 16,383-row system, after 32,770 linear solves. No partial PDE result
replaces the refusal. Each actual failed system and its last finite checked
vector are saved together; the independent reader recomputes the reported
original residual from those binary64 inputs.

| Scheme / grid | Failed stage | Last checked residual | Required tolerance |
|---|---:|---:|---:|
| dirk_l / space-fine | 221 | 1.1284839729341911e-10 | 1e-10 |
| dirk_l / domain-wide | 423 | 1.3842793578078272e-10 | 1e-10 |
| dirk_third / space-fine | 190 | 1.2485656952776480e-10 | 1e-10 |
| dirk_third / domain-wide | 424 | 1.0894041224673856e-10 | 1e-10 |

Across the eight computed requests only, the largest absolute discrepancies
from the independent closed form are price `1.480902660855321e-5`, Delta
`1.8833502041237793e-7`, and Gamma `5.387527645187018e-8`. These partial-set
diagnostics cannot replace the four refused grids or establish the complete
space/time/domain uncertainty rule. The summary explicitly retains
`incomplete_or_refused`, `unresolved` prospective outcomes and `not_promoted`
statuses. No historical pilot decision is rewritten.

The observed work includes every attempted failed update: 10,458 obstacle stages,
78,807 policy solves and 78,807 correction solves, for 157,614 linear solves.
All work is from one numerical process with its thread environment set to one.
Its clocks record offline evidence-generation provenance only.

## Frozen evidence

Run directory:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/zero-rate-lcp-control-v1/`.
It contains the streamed outcomes, complete summary, source snapshots, unchanged
historical144 receipt and four failed-stage NPZ archives. Source and historical
hashes match before and after execution. All714 other corpus contracts and six
other cold Greek fields remain uncomputed in this control.

- Run source: `8f4f81243e19124fd203ede551c41dba8085fb07`.
- Outcomes SHA256: `162feebb7f22d3262fec34310b7de5d78e566838da83c1f733eff4f3e7a63e69`.
- Summary SHA256: `38fbfbb1e71143ad537ff65238b1bb7697892ca3d4e1e15e184a747ae59f9a1a`.
- Joined receipt: `repair-iv/lcp-zero-rate-control-outcome-receipt.json` under
  the same evidence root; it pins all files and the independent verification log.

The fresh-process verification exits successfully because the receipt is complete
and internally consistent. Its numerical field remains
`solver_controls_passed: false`; process exit status is not a numerical pass.
No production source or default numerical policy changes in this report.

Next, independently recompute every row of all four saved failed systems at
100 decimal digits. That evidence must distinguish a residual-evaluation limit
from error in the returned binary64 vector. A policy cycle cannot be inferred
merely from exhaustion of the iteration cap. Any new numerical method or
reference-accuracy budget requires a separate prospective decision; another
iteration-count increase is not justified by this result.
