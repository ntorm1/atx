# AR1b independent review

Reviewed commit `86aa0e7babf5ac13e7b2bc38fc4b56282dd7781e` against
`activation-forward-panel-brief.md` (including its later adjusted-price addendum),
the implementation report and program rulings. This was one Codex source review;
no tests, source-data scans or warehouse operations were run.

Disposition: **one Important finding; no Critical findings.** Accept the bounded
fix on the implementer's issue-by-issue report and focused regression evidence;
another review pass is not required by the process ruling.

## Important F1: invalid terminal corrections are filtered before revision selection

`atx-db/src/atx_db/delisting.py:2227` and
`atx-db/src/atx_db/quality/checks_survivorship.py:72` remove nonfinite terminal
returns and returns below -1 before computing the winning revision. This differs
from the intentionally correct price path, which picks a revision before testing
its value. With an older eligible valid terminal and a newer eligible invalid
correction for the same security/date, both writer and quality check silently
reuse the older value. With only invalid terminal evidence, the terminal vanishes
from the expected grid: an empty panel can pass, or later bars can be treated as
ordinary survivor observations. The separate uncovered-event check only requires
a matching terminal row and does not catch this condition.

This input is representable: the terminal table's DOUBLE NOT NULL column has no
finite/range constraint (`migrations/bodies_0185_0188.py:53`), and the observed
terminal collapse (`delisting.py`, `compute_delisting_terminal_returns`) excludes
missing observations but does not enforce that numeric domain.

Required bounded repair: apply the observation cutoff, choose terminal revisions
and first events deterministically, then evaluate numeric validity. An invalid
selected terminal must neither resurrect an old revision nor become an absent
event that permits post-terminal survivor labels. Make selected invalid evidence
fail the input-derived quality gate (or an equally explicit registered terminal
input check). Add focused cases for a newer invalid correction and invalid-only
evidence, including historical cutoff behavior. Keep the no-fallback behavior for
invalid prices.

## Confirmed scope and limits

- Calendar-session endpoints, exact survivor price matching, strictly preterminal
  partial legs, observed/policy lineage and symbols are internally aligned.
- Production uses adjusted close; raw compatibility is explicit. Formation,
  preterminal/endpoint and selected terminal clocks are maximized. Historical
  cutoffs precede price selection and do not rely on latest-revision flags.
- Replacement is source scoped and transactional. Production constructs no
  all-price or expanded-panel pandas frame, and the supplied tests cover rollback
  and empty replacement. This review did not rerun them.
- Expected stitch windows come from own formation bars, input calendar and pinned
  horizons; unrelated output sources cannot mask absent production stitches.
- Full-universe memory, runtime and primary-key index behavior remain unmeasured.
  The report states this accurately; 1 GB/one thread plus the reviewed process-tree
  guard and a single heavy workload remain required. Calendar population/counts,
  activation integration and migration0312 metadata are explicit pending work.

No additional review finding is inferred from those already documented limits.
