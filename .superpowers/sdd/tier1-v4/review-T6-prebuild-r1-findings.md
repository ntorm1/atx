# Independent Task6 scoped review: fix round 1/5

Reviewer: /root/t5_prebuild_review. Package: ac5b86d5, supplied per-commit diff.
No implementation edits, git reruns, real sources, HTTP or live jobs.

## Spec compliance: Needs fixes

I2-I6 and M1 are addressed. I1 remains incomplete for issuer weight denominators.
Actual source, coverage and return acceptance remains NOT_MEASURED. R15's
release binding remains a dependency; this review does not authorize legacy data.

## Strengths and checks

Absent bars are retained; duplicate price keys rejected; borrow clocks enforced;
selected-source semantics enter resume identity; dependent factor lineage is
checked; audit vintages consolidated; coverage uses independent grids.
Seven named offline regressions passed in 3.94s without warnings, DuckDB at most
128MB, one thread. Config semantic and LF physical hashes match the report.

## Important: remaining I1

At _factor_native.py:233, :263, the BAB filter, and _factor_sources.py:113,
me>0 removes constituent lines before aggregate qualification. Tiny reproduction:
one issuer has two eligible classes, ME=100 and ME=NULL; the second class has a
failed audit and no return. formation_issuers emits me=100, certified=True;
issuer_period emits ret=.10000000000000009, certified=True. The incomplete
issuer is certified from its surviving class.

Preserve every eligible constituent through qualification. Missing, nonpositive
or invalid ME must invalidate the issuer cap basis rather than shrink its
denominator. Apply null-safe predicates to EG/risk bool_and(... AND me>0), too.
Add multiclass NULL/zero/negative-ME regressions for formation, returns, signals,
EG and BAB. Scoped spec and quality remain Needs fixes until corrected.
