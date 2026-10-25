# Four saved LCP stages: capped observation result

The reviewed runner `3227632ba5c3e20d50c8dfa2f10e4a73eee1a948` observed the
declared four saved systems once. All four reached the unchanged observation
cap of 128 updates and 256 linear calls. None converged to the original `1e-10`
complementarity tolerance. **No policy or full-state repetition occurred within
the 129 saved states per system**, so neither stagnation nor cycling is established
by this experiment. The observation remains inconclusive about later behavior.

This does not change the earlier fixed 12 result: 8 computed and 4 refused at the
original 16385-update bound. It does not run a PDE, enlarge a work allowance,
change tolerance or update a historical Greek/reference decision.

## Exact residual evidence

The producer and independent reader each checked all 16,383 rows of the final
state. No repeat closing state was selected because no repeat was confirmed.
The exact calculations use the original binary64 operands as rationals;
Decimal100 independently agrees at the displayed precision.

| Saved stage | Final binary64 residual | Final exact residual, displayed | Exact violating rows | Largest exact selected-equation residual |
|---|---:|---:|---|---:|
| dirk_l / space-fine, stage221 | 1.910791525006e-10 | 1.903120420050e-10 | 1548,4442,5883,5925 | 5.228882737611e-12 |
| dirk_l / domain-wide, stage423 | 1.319762077401e-10 | 1.315879810383e-10 | 2152 | 2.948547490125e-12 |
| dirk_third / space-fine, stage190 | 1.220712420036e-10 | 1.221795900971e-10 | 6222 | 5.700703685717e-12 |
| dirk_third / domain-wide, stage424 | 1.016076112137e-10 | 1.018419205487e-10 | 1459 | 2.953982479857e-12 |

Rows are zero-based interior indices. Full exact numerator/denominator strings
and Decimal100 values are retained in each stage JSON and the outcome summary.
Every selected-policy equation at the final state is below tolerance, including
the seven rows whose complementarity equations fail. The selected equation is
chosen by the preceding state's actual policy. Therefore these failures cannot
be explained merely by the final selected-system residual exceeding `1e-10`.
They require inspection of policy choices and the other complementarity equation;
the observation alone does not establish their cause or a safe correction.

Each capped final vector differs in binary64 bits from its earlier historical
16385-update final vector. No capped result substitutes for that old vector.
The full-policy and full-state equality checks retain every row and signed-zero
bit; a hash match alone cannot establish recurrence.

## Work, storage and admission

- Four systems, 512 completed updates, 1024 attempted linear calls, 516 saved states.
- 65,532 distinct final-state rows; 131,064 exact/Decimal row evaluations across
  exactly two automatic passes: producer and independent reader. No extra exact
  reader pass was invoked after the run.
- Per stage: 19,020,663 reserved and retained checked-state bytes, plus 917,432
  original input-array bytes. Solver/Python temporaries remain unbounded by this
  accounting, as declared.
- Complete archive: 81,646,249 bytes, within the 128 MiB cap; all stage caps pass.
- Fresh output, source/input before-and-after joins and independent semantic
  reader pass. The complete manifest was published only after validation.
- One process launch; exit 0. No retry, tuning, PDE request or timing qualification.

The parent's exact-source approval and independently passing 17 runner tests were
recorded before execution. The run occurred 2026-10-24 22:54:21–22:54:33 UTC.
Its roughly 12-second process wall time is diagnostic bookkeeping only.

## Frozen evidence and next question

Numerical archive:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/lcp-four-stage-observation-v1/`.
Manifest SHA256:
`32ae20c59655746b27daf945b0d8ba1cd57b4b6b1f50964e43c6510005a80d3f`.

Approval, launch, outcome summary and final source/artifact receipt:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/lcp-four-stage-outcome-v1/`.
The original four input archives and historical outcomes remain byte-identical.

The next authorized diagnostic uses only these saved trajectories: inspect the
seven final violating rows, preceding/current selected equations, policy margins
and sign changes. Existing exact summaries plus bounded binary64 diagnostics
are sufficient for that next step. No new solve, larger observation window or
new exact all-row audit is implied. A proposed feasibility or policy correction
must preserve the original residual contract and obtain prospective review.
