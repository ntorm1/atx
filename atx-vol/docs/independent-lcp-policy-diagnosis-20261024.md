# Saved-row diagnosis: policy changes undo locally admissible states

The seven final violating rows from the frozen four-stage observation were
examined across their 129 saved states: 903 bounded binary64 row observations,
zero new solves, zero PDE requests and zero new exact row evaluations. The
existing exact final-state summaries remain the authority for the seven failures.

Six rows alternate exercise/continuation selections throughout the last16 saved
states. The remaining row follows a repeating three-selection pattern. This is
local policy chatter; the complete policy vectors and complete states had no
repeat within the observation window. It does not establish a global cycle.

For every final violating row, state127 was locally admissible under the original
`abs(min(continuation,gap)) <= 1e-10` test. Its preceding policy selected the
continuation equation. Nevertheless, the newly computed continuation residual
was slightly larger than its positive obstacle gap, so the strict comparison
selected exercise. At state128 that exercise equation clamped the row to the
obstacle; the resulting negative continuation residual exceeded tolerance.

| Stage / row | State127 gap | State127 continuation | State128 continuation | Last16 above-obstacle offsets, ulps | Policy changes across128 updates |
|---|---:|---:|---:|---|---:|
| dirk_l fine /1548 | 4.689582056017e-13 | 1.676880856394e-12 | -1.183764197776e-10 | 33,0 repeated |128 |
| dirk_l fine /4442 | 2.486899575160e-13 | 3.552713678801e-13 | -1.042366193360e-10 | 35,0 repeated |124 |
| dirk_l fine /5883 | 7.531752999057e-13 | 8.242295734817e-13 | -1.910791525006e-10 |106,0 repeated |128 |
| dirk_l fine /5925 | 4.831690603169e-13 | 8.668621376273e-13 | -1.073630073734e-10 |0,38,68 repeated |84 |
| dirk_l wide /2152 | 3.169020601490e-12 | 3.538502824085e-12 | -1.319762077401e-10 |223,0 repeated |126 |
| dirk_third fine /6222 | 9.023892744153e-13 | 1.165290086647e-12 | -1.220712420036e-10 |127,0 repeated |122 |
| dirk_third wide /1459 | 5.684341886081e-14 | 2.557953848736e-13 | -1.016076112137e-10 |4,0 repeated |127 |

State128 gaps are all exactly zero in binary64. The existing exact summaries
show those seven complementarity violations also exist for the actual binary64
operands interpreted exactly. They are not just false failures of the residual
evaluation. Over all128 updates, these seven rows' selected-equation residuals
remain at most3.538503e-12, far below the unchanged1e-10 tolerance.

The diagonal coefficients are122.425–255.850. Small obstacle-gap changes and
coupled neighbor changes can therefore create much larger continuation changes.
For example, row1548 drops33 ulps while both neighbors remain unchanged between
states127 and128. Other rows have simultaneous neighbor movements; a local
clamp or independent row correction cannot be assumed to preserve their
neighbors' complementarity. The archived coefficient-weighted ulp quantity is
only a sensitivity scale, not an acceptance rule or rigorous error bound.

## Primary-method context and bounded next experiment

Huang, Forsyth and Labahn document floating-point policy-iteration oscillations
despite exact-arithmetic M-matrix convergence conditions. Their Section5 also
shows how inexact arithmetic can break the monotone-iterate argument, and later
analysis considers scaling. That establishes relevant precedent, not a proof
of the proposed ATX retention rule. We do not adopt their iterate-difference
stopping criterion or alter the original residual gate.
[Author-hosted paper, Sections5–6](https://cs.uwaterloo.ca/~glabahn/Papers/inexact.pdf).

Reisinger and Witte analyze policy iteration for M-matrix complementarity
problems, including finite exact-arithmetic convergence. Their numerical studies
check both inequality feasibility and complementarity. Those conditions support
retaining full coupled admission after any policy-selection experiment; the
finite-precision modification still needs independent tests.
[Version4 primary preprint, Sections2–3](https://arxiv.org/pdf/1012.4976v4).

The source-derived proposal is to retain the policy actually used for the
just-completed update on rows that already satisfy the unchanged local
complementarity test. Reselect failing rows with the original strict comparison.
The initial policy, coupled linear solves, residual-correction arithmetic,
global admission and work limits remain unchanged. A retained row is never
removed from subsequent solves or checks. Neighbor motion can make it fail
later, in which case it must be reselected. This is a prospective experimental
policy rule, not a claimed convergence theorem or tolerance increase.

The parent authorized that isolated experiment after reviewing this mechanism.
Small synthetic controls must cover local chatter, false feasibility and
adjoining coupled rows before source review and any replay of the four saved
systems. No full PDE study follows automatically.

## Frozen diagnostic evidence

`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/lcp-four-stage-row-diagnosis-v1/`
contains the read-only analysis source, all903 row observations and seven
summaries. Every input file is bound to the frozen observation manifest SHA
`32ae20c59655746b27daf945b0d8ba1cd57b4b6b1f50964e43c6510005a80d3f`.
`receipt.json` binds this report, source and outputs. No original archive bytes
were changed and no additional exact all-row audit was run.
