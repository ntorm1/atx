# Retained-policy observation seam

This component follows the prospective boundary `25a32861`. It supplies an
in-memory observer and independent reader, not a four-stage launcher. The
candidate has not run any saved system or PDE. Its archive/launcher and final
source review remain separate prerequisites.

`lcp_retained_observe.py` loads the approved retention source into a private
module, instruments only that instance and records each actual completed
selection before the original coupled solve. Original numerical calls delegate
unchanged. Raw comparisons and used policies are distinct arrays. The reader
independently reconstructs both the retention rule and selected-equation
residuals; it never calls the old strict-selector reader or any linear solver.

Full-state recurrence includes the preceding used policy, so identical prices
with different retained memory cannot prove a repeated transition state. The
initial state has no preceding policy and remains distinct. Every equality uses
all value/policy/residual bits; forced digest collisions and signed zeros are
covered. Two complete laps remain necessary for a confirmed observed period.

The prefix and phase checks preserve the previous zero-solve reason-laundering
and erased-finite-initial-state protections. Failed policy/correction attempts
retain actual selected masks without inventing a following checked state.
Primitive allocation/workspace failures without original work diagnostics abort
the observation as incomplete; they do not become fabricated numerical failures.

Ten refusal-stub tests reproduced runtime RED. The final13 controls pass,
including actual primitive bit/count parity and global isolation, an explicitly
scripted trace where retained and raw masks differ, actual-mask substitution,
forged retention counts, selected-equation corruption, zero-solve phase mutants,
nonfinite attempts/initial inspection, allocation/source/storage guards, and
the complete-state memory distinction. Scripted solver results test evidence
handling and are not represented as genuine numerical solutions.

All57 combined controls pass and all42 optimized controls pass:

```text
python -m unittest -v test_lcp_retained_observe test_lcp_policy_retention test_lcp_observe test_lcp_correction test_pde_reference.ObstacleTests
python -O -m unittest -v test_lcp_retained_observe test_lcp_policy_retention test_lcp_observe
```

Evidence:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/lcp-retained-observer-v1/receipt.json`.
Original correction, PDE, strict observer and approved retention primitive stay
unchanged. The maximum checked-state/raw/used-mask reservation is21,117,687
bytes per stage. No convergence, cycle, reference-accuracy or timing claim is
made for the candidate by these tiny controls.
