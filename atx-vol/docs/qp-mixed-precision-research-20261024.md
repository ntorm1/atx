# SPY Newton-system mixed-precision replay

Research branch only, based on integration `0662116e`. **Not promoted.**
Neither the capture hook, replay target, FP32 solver nor floating-point mode
change is part of the integration branch. The shipping solver remains unchanged.

## Hypothesis and controls

The calendar QP spends roughly 200 ms in numeric factorization on the earlier
qualified one-core profiles. Its strict lower factor has 240,475 entries, about
1.92 MB in double or 0.96 MB in float before indices. A smaller factor might
reduce cache traffic; this does not imply a twofold scalar-arithmetic speedup.

[Cornelis and Vanroose](https://arxiv.org/abs/2105.01333) use single-precision
Cholesky as a CG preconditioner in a regularized inexact LP interior-point method.
That motivates a measured experiment, not a proof for this QP or a reason to
relax publication certificates. This first replay uses ordinary iterative
refinement, not their CG algorithm. [PIQP](https://arxiv.org/abs/2304.00290)
also motivates examining numerical factorization and allocation patterns.

`ATX_VOL_QP_MATRIX_CAPTURE` exports the unmodified lower double matrix and each
original RHS/answer. The SPY capture contains 35 factors, 70 directions, 12,866
variables and 56,010 lower-matrix nonzeros. File size is 30,368,366 bytes. The
capture run preserves the complete prediction hash
`e086fd069a81c2c9eb84bb99e25e8a59e5d78fca8b02b0d51556668eff354d5f`
and passes all 13 actual-API gates. Instrumented timings are ineligible.

The offline parser requires a fixed endian marker, bounded dimensions, sorted
lower storage with every diagonal, finite numbers, exactly two directions per
factor, an explicit terminator and no trailing bytes. Double Eigen replay matches
**all 70 captured directions bit for bit**, including each repeated run.

The candidate symmetrically scales by `1/sqrt(diagonal)`, factors in float, then
computes residuals and updates in double against the original matrix. It allows
at most six corrections, stops at a componentwise backward residual of `4e-15`,
and falls back to the original double factor on nonpositive/invalid float pivots,
stagnation or exhausted refinement. All casts, residuals, correction solves,
symbolic work and fallback factors are included in candidate solver clocks.
Input parsing, report construction and comparison against the captured answer
are outside those clocks. These are component experiments under concurrent host
work, without one-core/quiet-sibling or ThinLTO qualification.

## Findings

Every full replay has **191 corrections and 26 of 70 directions falling back**.
The first 22 matrix pairs refine successfully; later pairs either exhaust/stagnate
or have inadmissible float factors. The largest absolute difference against the
original direction is `2.276003691070727e-11`. This is not a full-QP rerun,
condition-number estimate, forward-error bound, or price acceptance certificate.
The double control's maximum componentwise residual is `5.975373421374407e-15`;
fallback preserves that control instead of claiming all answers meet `4e-15`.

The initial three alternating-order pairs measured original solver totals of
490.66, 633.73 and 656.18 ms versus candidate 1,692.28, 3,091.43 and 4,021.02 ms.
Host contention is visible; these values establish no shipping latency ratio.
There is no positive speed evidence supporting integration.

Successful float factors contain 845--4,222 subnormal stored entries. A follow-up
confines FTZ/DAZ to float factor/triangular operations and restores caller MXCSR.
The verified `qp-replay-ftz-v4.json` has zero such entries but still 191 corrections
and 26 fallbacks. Its original/candidate solver clocks are 773.60/1,357.66 ms,
again unqualified. Thus eliminating subnormals alone did not establish a gain.

**Receipt correction:** the file named `qp-replay-ftz-v3.json` reports
`float_ftz_daz: false`; its name is misleading and it is not FTZ evidence. It is
retained. V4 launches through a structured subprocess argument list, records the
exact command and binary hash, and asserts the exported flag is true.

Two research test methods pass, including ten malformed-capture mutants, exact
diagonal replay, a benign float-refinement case, rank-loss double fallback,
both FTZ selections and MXCSR control restoration. These validate the experiment,
not production numerical accuracy.

## Decision and next experiment

Keep this work isolated. Do not add float arithmetic, FTZ, looser stopping rules,
or a new dependency to production on this evidence. Before further mixed-precision
work, require a reason the extra triangular solves/fallbacks would be cheaper.
The next structural study should measure supernode widths and fill in the actual
factor, then compare an appropriate block/supernodal double factorization on the
same captured systems. Even a positive replay must pass a full QP rerun, full
actual-API acceptance and fresh-process clocks before promotion.

All raw evidence is under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`:
`qp-systems-v1.bin`, `qp-capture-v1-audit/`, `qp-replay-v1.json`,
`qp-replay-denormals-v3.json`, `qp-replay-ftz-v3.json`,
`qp-replay-ftz-v4.json`, and `qp-replay-ftz-v4-receipt.json`.
The replay source is `bench/qp_replay_research.cpp`; research controls are
`bench/test_qp_replay_research.py`.
