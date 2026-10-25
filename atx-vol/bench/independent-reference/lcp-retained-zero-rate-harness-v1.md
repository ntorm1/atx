# Retained-policy twelve-request harness checkpoint

This implements the approved scope in `680db3de4fb1c400d85c9a6a406cd4f6c4e9c36b`.
It extends the three existing adapter/control/runner modules. No retained-mode
declared PDE request has run. Exact-source review is required before that run.

The explicit CLI mode `one_correction_retained_admissible` uses the reviewed
retention primitive unchanged. Each actual PDE stage constructs a fresh selector
inside that primitive. The original n+2 algorithm bound and residual equations
remain; the effective call budget is `min(2*nx,256)`. There is no budget, grid,
tolerance or model CLI override. Both schemes, all six grids, the zero-rate
model, 2^27 stage-node guard and original refinement/analytic rules stay fixed.

The new mode has its own control and summary schemas. Default original and
one-correction paths remain available with their existing outputs. The old
immutable twelve-request correction archive still reconstructs as eight
computed/four refused without executing PDE. Its historical decisions remain
unchanged. New source manifests bind the retained primitive, supporting original
observer and both prospective declarations to their frozen Git identities.

Full observed stages retain the actual paired-solve work and selector counters.
The reader checks their conservation, original residual admission, distinct
effective cap, failure phase and PDE aggregate joins. Retention allocation or
workspace failure without original solver counters retains available partial
selector diagnostics and explicitly unknown original work. It cannot be
relabeled as an algorithmic-limit failure or fabricated zero work.

Numerical refusals retain the existing owned input/vector snapshot. The reader
checks the matching saved residual. For a new-mode initial nonfinite refusal
without a checked vector, it now independently reconstructs continuation/gap
from the initial arrays. A checksum-valid finite initial input cannot silently
erase its checked state. This guard is limited to the new mode, preserving the
old receipt contract.

The new mode publishes complete only after its independent reader and a final
source/history check succeed. Reader failure or source changes during that
phase leave the original incomplete marker. Twelve streamed outcomes and all714
uncomputed contracts are retained. Six other Greek fields remain uncomputed;
Delta/Gamma decisions remain `not_promoted` even if the control criteria pass.

## Controls

Six initial adapter/protocol tests failed against the old interface, while the
old-receipt control passed. Two runner tests then failed because the retained
mode was unavailable. The rehashed finite-initial/absent-state adversary
reproduced a separate runtime failure before the independent operand guard.
All three RED receipts remain unchanged.

The final11 new controls cover actual nx=33/nt=4 PDE adapters for both schemes,
fresh stage selectors, namespace/default parity, typed cap and allocation
diagnostics, selector/phase mutants, failed snapshot ownership, source/cap/mode
receipt mutations, incomplete publication, and old-receipt compatibility.
The twelve-outcome orchestration control uses explicitly stubbed solves and
historical protocol rows; it is not a candidate PDE result.

With the immutable historical and original-control archive paths explicitly
supplied, 54 combined tests pass and 26 optimized tests pass, with no skips:

```text
python -m unittest -v test_lcp_retained_pde test_lcp_stage_adapter test_lcp_zero_rate_control test_lcp_zero_rate_run test_lcp_policy_retention test_lcp_correction test_pde_reference.ObstacleTests
python -O -m unittest -v test_lcp_retained_pde test_lcp_stage_adapter test_lcp_zero_rate_control test_lcp_zero_rate_run
```

Evidence:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/lcp-retained-pde-v1/receipt.json`.
Only Python syntax and Git whitespace hygiene apply; no C++ source changed.

After exact-source approval, the intended one-shot command uses a new output:

```text
python lcp_zero_rate_run.py --mode one_correction_retained_admissible --baseline C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/independent-greek-pilot-v2 --output C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/zero-rate-retained-lcp-control-v1
```

That command has not run at this checkpoint. There is no original-control PDE
rerun, historical144 rerun, tuning, retry, full-bundle or latency promotion.
