# Independent date-CPCV core source review

Reviewed `f7b517986207396c202a3bcfeb5d39061d2cc816`: the separate public
`cpcv_date.hpp` API, `cpcv_date.cpp` implementation, and its one source registration.
Verdict: **approve source for focused qualification; no blocker found**.
No build, runtime rerun, benchmark, data read or caller migration was performed.

- `t0` ordering and nonempty half-open labels are validated. All equal-`t0` rows
  belong to one date. Distinct-date offsets are partitioned by `floor(g*D/K)`;
  the quotient/remainder form avoids multiplying the full date count.
- Embargo extends each test label's exclusive endpoint in explicit ordinal units.
  Overflow is checked before addition. Missing dates do not shorten that period.
  The merge retains disconnected test windows. With train rows ordered by `t0`,
  the advancing-window sweep rejects exactly overlapping information intervals;
  it does not assume monotone label end times or use a global convex hull.
- Lexicographic combinations supply `C(K,k)` folds. Each group appears in exactly
  `C(K-1,k-1)` combinations, matching path allocation and occurrence indexing.
  Every path receives one fold per group; each held-out fold/group occurrence
  is assigned once. Folds are not misreported as independent backtest paths.
- `K<=20` bounds binomial intermediates. Budget arithmetic uses remaining-capacity
  division before multiplication. The preflight covers fold index capacity,
  output/vector headers, date/window/test-mask scratch and path storage before
  combinatorial allocations. Default working budget is 64 MiB; excessive plans
  fail explicitly rather than allocating an unbounded fold-by-row matrix.
- The new API and source preserve the legacy entry point and recipe. The code
  makes no claim of application configuration/resume binding or actual fit-stage
  adoption. Those are separate integration work, as are owning runtime fixtures.

Postimplementation fixture review/execution was not included in this source
freeze. No CPCV speedup, statistical independence, or complete E6 gate is claimed.
