# Cached LDLT experiment: not promoted

This branch preserves an unsuccessful performance experiment against the approved
`6808691e` joint-projection profile. It must not be integrated as a performance
improvement. Constraints, objective, stopping rule and final certificates remain
unchanged. No production speed or subsecond claim follows from these results.

The prototype caches Eigen 3.4.0's permutation, exact input-value mapping,
topological reach order and numeric update boundaries. The first successful
factor uses Eigen's public interfaces. Its adapted numeric implementation retains
the original Eigen/LDL notices. A guarded clang-cl Release AVX2/FMA experiment
packs four independent contiguous workspace updates; other updates use scalar
arithmetic. The diagonal reduction retains the upstream nonfused expression.

The red tests exposed two real defects: permuted upper columns are deliberately
unsorted, and the diagonal versus workspace update expressions have different
contraction boundaries. Preserving upstream traversal and `numext` expressions
restored bit identity. The first timing regression came from repeated pointer
loads after scattered writes; hoisting owned buffer pointers removed that cost.

Every L/D/permutation/solve bit matches Eigen on repeated arbitrary sparse
patterns, extreme scales, negative/near-zero pivots and recovery after numerical
failure. Changed patterns, malformed storage and nonfinite input fail closed.
All 35 focused solver/draft/target/factor tests passed. The optional full-SPY
diagnostic compared 35 factors and 70 directions with zero mismatches or numeric
failures. The complete projected price fingerprint remains
`8251046449056129050`. This rejected branch has not completed include-hygiene
qualification and is not a release candidate.

All three fresh-process trials in each series used logical processor 6 and had
zero measured SMT-sibling activity. They are isolated projection observations,
excluding final IV/curve publication:

| Variant | Projection range, ms | Factor range, ms | Projection CV |
| --- | ---: | ---: | ---: |
| Approved Eigen baseline | 357.85–398.21 | 200.79–209.30 | 5.36% |
| Initial cached implementation, rejected | 442.03–450.81 | 269.46–278.32 | 0.98% |
| Pointer-hoisted, packed experiment | 349.16–384.35 | 199.88–218.66 | 5.04% |

The packed result overlaps baseline and does not demonstrate a factorization
gain. It does not justify shipping the extra implementation. Receipts, binaries'
hashes and every trial remain in `calendar/qp-profile-{baseline-v1,cached-v1,
packed-v2}/receipt.json` under `C:/atx-cache/quote-benchmark/
spy-qualified-r7-20261023/`.

An independent diagnostic then captured every iterate, without changing the
solve, and ran the unchanged complete publication certificate afterward. The
first passing iterate is sweep 35 of 36. Sweep 34 still fails. Thus there is only
one removable iteration; no material optimization tail was found. The existing
stop already depends only on primal residual, not dual/gap convergence.

| Metric | First certified, sweep 35 | Existing final, sweep 36 |
| --- | ---: | ---: |
| True constraint violation | 2.51675e-10 | 3.44294e-11 |
| Primal residual | 6.13823e-10 | 8.47577e-11 |
| Squared feasible-midpoint distance | 651.545024 | 651.544341 |
| Squared original-midpoint distance | 1736.197522 | 1736.198241 |
| Original raw-box departures | 15 | 15 |

The largest early/final price difference is $0.000254881/share. Both iterates
have the same 16 analytic floor-band rows; neither violates the independently
recorded cold sigma-floor price for any of the final 12,850 identified rows.
This is a floor diagnostic, not final-IV acceptance of the earlier iterate.
No alternative stopping policy is proposed.

Reproduction uses the existing normalized raw fixture plus
`ATX_VOL_VERIFY_JOINT_FACTORS=1` and
`ATX_VOL_JOINT_ITERATION_RECEIPT=<output.json>` with
`RawPriceDraft.ExternalFeasibleMidProjectionProfileWithoutAnyInitialIvFit`.
Those optional diagnostic flags duplicate factors/capture iterates and must be
absent from performance measurements. The per-iterate certificate work is
outside the recorded solve timing. Exact receipts:
`calendar/joint-iteration-diagnostic-v1.json`,
`calendar/joint-iteration-analysis-v1.json`,
`calendar/joint-numeric-diagnostics-{red,green}.xml`.
