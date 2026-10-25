# Retained-policy result on the four saved LCP stages

All four previously failing saved systems now pass the unchanged original
1e-10 complementarity gate. The exact final-row audits also pass. This is a
saved-system result, not a complete PDE, option-price or Greek qualification.

The candidate used clean, independently reviewed source
`c670ebbdc299f81d6053e8d74305f5b91d631464`. Root and calendar approved the exact
archive/launcher before one fixed invocation. The rule and observer had already
received their own reviews. No source, grid, input, tolerance or work-cap change
was made after seeing the candidate result.

## Fixed denominator and result

The four original 16,383-row systems came from the unchanged 12-request
zero-rate control archive. Each retained the original initial vector, equations,
1e-10 tolerance, n+2 algorithm limit and the observation cap of 128 updates /
256 attempted linear solves. Each update still uses the original selected-policy
solve followed by exactly one residual-correction solve.

| Saved stage | Strict updates / calls | Retained updates / calls | Retained exact final residual |
|---|---:|---:|---:|
| dirk_l, space-fine | 128 / 256, capped | 7 / 14 | 9.96893712745160923510268e-11 |
| dirk_l, domain-wide | 128 / 256, capped | 6 / 12 | 7.72494955509489498577011e-11 |
| dirk_third, space-fine | 128 / 256, capped | 9 / 18 | 9.93255397529899040294211e-11 |
| dirk_third, domain-wide | 128 / 256, capped | 9 / 18 | 5.94465479931403034956798e-11 |

The retained candidate completed 31 updates and 62 linear calls, compared with
512 updates and 1,024 calls in the earlier strict-selection observation. All
four strict outcomes were observation-cap refusals. The corresponding strict
exact residuals were 1.903120420050e-10, 1.315879810383e-10,
1.221795900971e-10 and 1.018419205487e-10, with seven violating rows in total.

No retained final row violates the original tolerance. Final selected-system
residuals are at most 5.515e-12. The smallest remaining margin to the global
tolerance is about 3.11e-13, so this result must not be described as a wide
numerical safety margin or a proof for other systems.

The candidate stored 35 checked states. Raw policies repeat locally in each
trajectory, while actual used-policy and complete-state repetitions were absent.
No full-state cycle was confirmed. The 64,864 retained-overrides count is the
sum of row-selection events across updates, not a count of distinct rows.

## What changed, and what stayed fixed

After an update, the candidate retains the just-used policy only where the
original current row test `abs(min(continuation, gap)) <= 1e-10` already passes.
Failing rows are reselected with the original strict comparison. The initial
selection is unchanged. Every update still solves the full coupled system and
must pass the unchanged original global residual test before publication.

This targets the previously recorded local chatter: a valid continuation row
was being switched to the obstacle because of a tiny strict-comparison
difference, making its continuation residual fail on the next update. The
observed recovery supports that diagnosis on these four systems. It does not
establish a general convergence theorem or justify accepting an unchanged
policy when the global residual fails.

The original correction/PDE/strict-observer sources remain byte-identical to
the preceding strict archive. All copied historical input bytes are identical.
The candidate uses separate actual-policy evidence and its own reader; raw
comparisons are not relabeled as the policies that drove the solves.

## Evidence and limits

The independent reader completed before the archive was marked complete. There
were 65,532 distinct final rows and exactly two automatic Fraction/Decimal100
passes, totaling 131,064 row evaluations. The outcome comparison only reads
those saved summaries and performs zero additional exact-row evaluations.

Archive:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/lcp-retained-four-stage-v1/`.
Its manifest SHA256 is
`943f8ce12b6b82c9c284d443747eaa07943c22ca894c95a371fbd06c8d7864d8`.
The complete archive occupies 11,185,832 bytes. The preallocated observation
reservation remains 21,117,687 bytes per stage; smaller retained output is not a
claim of a smaller peak process allocation.

Approval, launch log, comparison and immutable file receipts are under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/lcp-retained-four-stage-outcome-v1/`.
The invocation ran once from 23:35:29 to 23:36:14 UTC on 2026-10-24 and exited
successfully. Its elapsed time is an untimed-research diagnostic only; the host
was not qualified for a performance comparison.

No PDE request, tolerance adjustment, retry, historical-decision rewrite or
production change occurred. The next meaningful gate is a prospectively
reviewed retained-policy integration into the existing fixed 12-request
zero-rate PDE control, preserving its original models, grids and comparisons.
The historical 144-request study remains unchanged.
