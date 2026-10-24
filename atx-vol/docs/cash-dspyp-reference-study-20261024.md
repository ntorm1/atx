# DSPYp cash-reference accuracy study

Status: predeclared correctness experiment, 2026-10-24. No production change or
performance result. The original 50 cases and all 150 three-grid outcomes remain
the qualification denominator; this selected study cannot promote a new policy.

## Question and fixed experiment

The finest previous PDE price for DSPYp is 93.31753028217078 USD/share. Its frozen
reference is 93.318532225080119 with empirical uncertainty
0.000032516860882252968. The difference is 0.001001942909340414, so uncertainty
straddles the 0.001 monetary threshold. This is an uncertain case, not a pass.
The 1600-by-800 frontier remains 43/44 matching spot-liquidator cases passing,
plus six separately classified cash-model cases; none may be dropped.

The predeclared selected cases are DSPYp and its same-calendar call control DSPYc.
Both are taken directly from `tests/support/ddiv_matrix_cases.hpp`, including
the original binary64 event arithmetic. Every original input is exported so the
selection and all omitted computations remain explicit. No baseline price or
reference value enters the native calculation.

Use unchanged `ddiv_ref::pde_price`, base space 1600 and nominal time 800,
width 8, grading 2, two Rannacher steps, default domain sigma. Run exactly:

| Space level | Time level | Effective space | Nominal time | Purpose |
|---:|---:|---:|---:|---|
| 0 | 0 | 1600 | 800 | Exact previous-result control |
| 1 | 0 | 3200 | 800 | Spatial error direction |
| 0 | 1 | 1600 | 1600 | Temporal error direction |
| 1 | 1 | 3200 | 1600 | Interaction check |
| 2 | 0 | 6400 | 800 | Spatial order diagnostic |
| 0 | 2 | 1600 | 3200 | Temporal order diagnostic |

Levels multiply the existing per-event integer step counts, preserving nested
time nodes. Replacing the base with a larger integer would reapply per-segment
rounding and is deliberately not this experiment. Retain all twelve results,
including typed refusals, all original PdeResult fields and actual Thomas work.
No wall clock is recorded. Baseline results must match the frozen three-grid
receipt exactly. Source/executable/input/output hashes accompany the report.

The readout separates price increments along each axis and the mixed increment.
Observed order and extrapolation remain empirical diagnostics, never an error
bound. A successful selected row does not establish full-matrix accuracy for
that grid. No adaptive extra grids or per-case tuning are part of this run.

Frozen input/reference replay SHA256:
`d4b5c2e81c8971cd9d37abd601139217ba8e71161eee38c3ada7c1dba60343cc`.
Historical PDE and trinomial sequences are recorded in
`docs/american-cash-dividend-reference.md` and the immutable D0 matrix receipt.
The forthcoming report will retain those uncertainties and distinguish this
same-engine diagnosis from an independent numerical reference.
