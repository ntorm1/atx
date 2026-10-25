# Prospective observation of four saved LCP stages

Base: reviewed local-main checkpoint `a037b1e49f4f4e4ff34be46706fb267e5692f9f0`.
Its LCP primitive, harness and failed-result reports match the frozen predecessor.
This separate branch observes the unchanged one-correction map. It changes no
equation, precision, policy comparison, tolerance, PDE grid or historical result.
The four-stage replay has **not run**. Independent exact-source review is required
before execution; passing small harness tests does not satisfy that review gate.

## Fixed scope and inputs

Use only the four NPZ files in
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/zero-rate-lcp-control-v1/`:

| Scheme / grid | Stage | NPZ SHA256 |
|---|---:|---|
| dirk_l / space-fine | 221 | `76a0f697d0756e5b2c2ed4b3558e111dd8c9f334e833a90155000f06dc683056` |
| dirk_l / domain-wide | 423 | `99db1ddfa479aa26060d55e92c3192118aa4c076cc1983609849105059c2408a` |
| dirk_third / space-fine | 190 | `fa679a732cfb1e0440b17c716cfbddf0c428b8983fc9add26b70f962a45add74` |
| dirk_third / domain-wide | 424 | `c79dfccffce87347cc8138626cd9dd45a5009ab56fe6bdfb6416323a993067c2` |

Each has 16,383 interior rows and original tolerance `1e-10`. Start from its
saved `initial`, not the failed final vector. Keep the latter as an immutable
comparison point. Preserve the run manifest SHA256
`79350467d0298be8778c3ab644250476ad5e0d566451998d8eb00a1264c56a53`
and outcomes SHA256
`162feebb7f22d3262fec34310b7de5d78e566838da83c1f733eff4f3e7a63e69`.
Reject missing, duplicate, extra, malformed or hash-mismatched systems. No PDE
entry point is called, and neither the twelve-request nor full144 runner is used.

## Observation without a new arithmetic implementation

Load the frozen `lcp_correction.py` source into a private, unregistered module.
Require canonical text equality with approved primitive `17ab5fbf`; require the
imported original `_multiply` source to match its frozen base. Never patch a
process-global module, copy the policy algorithm into a new implementation, or
insert a different linear solver.

The private module may wrap `_multiply` and `solve_banded` solely to observe
inputs/outputs and actual call counts. Each wrapper calls the original function
with unchanged arguments. Identity checks distinguish the original matrix
multiply in `inspect` from the selected-policy correction multiply. Copy the
checked vector and reconstruct its continuation/gap with the same operation
order, without modifying returned arrays. Private solver arguments retain the
original no-overwrite flags. All input arrays are copied into owned binary64
storage and checked unchanged afterward. Global functions remain untouched.

Call only explicit `mode="one_correction"`. Use its already-approved resource
control to limit observation to `min(n+2,128)` completed updates, at most twice
that many linear calls. The original `n+2` algorithmic bound is unchanged.
Exhausting this smaller observation allowance means **inconclusive observation
cap**, not a new solver failure or numerical result. Do not stop on a suspected
repeat or increase the allowance after seeing output. Early convergence or a
typed original refusal remains explicit and retains actual attempted work.

## Exact repeated-state and policy checks

Retain the initial checked vector and each completed checked update, at most129
states per system. Retain each original binary64 policy (`gap < continuation`),
continuation maximum and complementarity residual. Hashes are indexing aids only:
every claimed equality must compare all binary64 value bits, every policy bit
and residual bits. Signed zero differences remain differences. Nonfinite states
cannot establish a repeat and must retain their original typed refusal.

Report policy recurrence separately from complete-state recurrence. Equal policy
bits with different checked vectors do not prove a state cycle. A confirmed
period p requires two complete consecutive observed laps: all states from i
through i+p match those from i+p through i+2p, including endpoint closure, by
the full comparisons above. Choose the earliest confirmation, then shortest
period, then earliest start deterministically. Period1 is reported as stagnation;
period>1 as a confirmed observed cycle. A lone repeated state without the second
lap is unconfirmed. Absence of a repeat inside the cap is inconclusive.

The reader independently reconstructs each policy and residual from every saved
state and the original arrays; it must reject an invented policy, reordered or
missing state, false hash match, inconsistent counters and a changed tolerance.
It also reconstructs each resulting vector's selected-system residual using the
preceding policy. This separates a violated selected equation from a residual
in an unselected complementarity equation without claiming either is the sole
cause of cycling.

For a later reviewed complete runner, exact rational and Decimal100 row checks
are bounded to at most two deterministically selected states per system: the
last observed state and the closing state of the earliest confirmed repeat,
deduplicated. Check all original complementarity rows and selected equations
for these states. This is at most131,064 row checks and no additional solve.
The existing all-row exact audit and all four numerical refusals stay unchanged.

## Fixed work and storage budgets

- Four saved systems, processed sequentially; at most128 policy updates and256
  linear calls per system, hence512 updates and1024 calls in total.
- At most129 states per system. Preallocated values and policy arrays use at
  most `129*16383*(8+1) = 19,020,663` bytes per system, with fixed small scalar
  metadata. Input copies and the unchanged solver's own temporary allocations
  are reported separately; this is not a bound on Python/SciPy process memory.
- Full uncompressed state evidence, original input copies and scalar arrays
  must fit a24MiB archive cap per system; complete new output is capped at128MiB.
  A storage refusal is explicit and cannot silently truncate a trajectory.
- Create a fresh output directory. Never overwrite previous results. Pin source,
  versions, input bytes, counters and files before/after; preserve incomplete
  outputs as incomplete. No wall-time admission or performance claim.

## Tests and review gates

First freeze this plan. Then use runtime-failing tests for the private seam and
independent reader on tiny synthetic systems only. Verify original-direction
bit parity with the unobserved primitive, unchanged inputs/global modules,
actual two-call accounting, independent state/policy reconstruction, a scripted
two-cycle and stagnation, forced digest collisions, signed-zero distinction,
policy-only repetition, nonfinite/typed failures and observation/storage caps.
Malformed counters and truncated/reordered evidence must fail closed.

The first component supplies the private seam and small controls, not an
automatic four-system launcher. Review that exact SHA before building/finalizing
any complete runner. Review the final runner and fixed output/provenance rules
again before the four saved-stage replay. No implementation or test may launch
the declared four systems as an incidental fixture. No production fitting,
Greek setting, tolerance or historical decision changes belong to this work.
