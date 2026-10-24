# Joint-QP ordering study: retain scalar AMD

No ordering in this bounded study reduces symbolic work relative to the existing
Eigen scalar AMD ordering. A reordered numerical fit is therefore unjustified.
Production ordering, constraints, objective and stopping rules are unchanged.

The frozen graph comes directly from the SPY Newton system after fixed-variable
substitution and redundant-constraint elimination: 12,866 scalar variables and
56,010 lower-triangle entries. Its SHA256 is
`b48629bf26c63bb61be720bda181b9117ee730645fdb4511e7b10b700c4f01ee`.
The exported original-coordinate map binds each free variable to expiry, strike
and side; it contains no fitted target, vendor label or baseline identification
input. Export preserves the existing fitted-price fingerprint
`8251046449056129050`.

The graph has 225 connected components, but 12,210 variables lie in one component.
Only 2,435 of 6,433 call/put node pairs have a direct graph edge after redundant
constraints are removed. Forcing pairs together can therefore constrain the
ordering unnecessarily; splitting independent tiny components offers limited
scope for reducing the dominant factor.

## Declared comparison

The original six methods were declared before examining results: scalar AMD;
expiry-major call/put pairs; strike-major pairs; paired-node AMD; strike-first
geometric nested dissection; and best-axis geometric nested dissection. Pair
orders preserve call then put. Nested dissection recursively eliminates the two
interiors before their separator and uses local AMD at leaves and separators.
The leaf limit is 128 graph vertices. Median strike/expiry splits use the smaller
one-sided boundary as a valid vertex separator, with AMD fallback for an empty
or badly unbalanced interior. Best-axis chooses the smaller valid separator.

After all paired methods failed, one explicit follow-up control applied the same
best-axis algorithm and 128-vertex leaf limit directly to scalar variables. No
threshold search or reordered fit followed.

| Ordering | Strict L entries | Sum of squared column degrees | Workspace updates/factor | Relative work |
| --- | ---: | ---: | ---: | ---: |
| Existing scalar AMD | 240,475 | 17,874,577 | 8,817,051 | 1.000 |
| Expiry-major pairs | 7,151,106 | 5,499,647,870 | 2,746,248,382 | 311.470 |
| Strike-major pairs | 983,546 | 97,951,498 | 48,483,976 | 5.499 |
| Paired-node AMD | 326,584 | 26,571,026 | 13,122,221 | 1.488 |
| Strike-first paired nested dissection | 605,545 | 80,572,661 | 39,983,558 | 4.535 |
| Best-axis paired nested dissection | 463,151 | 51,658,707 | 25,597,778 | 2.903 |
| Best-axis scalar nested dissection, follow-up | 385,738 | 38,749,392 | 19,181,827 | 2.176 |

For strict-L column degree d, the existing LDLT workspace update count is
sum(d*(d-1)/2). This counts arithmetic work independently of wall-clock noise.
The baseline count agrees exactly with the actual numeric-factor instrumentation
from the earlier cached-factor study. The baseline AMD permutation also matches
the exported production permutation exactly.

Each candidate receives Eigen symbolic analysis of its permuted graph. A small
counter adapted from pinned Eigen 3.4.0 computes column degrees without using
private Eigen fields or factoring the full SPY system. Tests compare those
counts against public Eigen numeric L patterns on multiple synthetic positive
definite sparse matrices. No new dependency was installed.

The saved development receipts separately record order generation, matrix
permutation, Eigen symbolic analysis and independent counting time. For example,
the v2 scalar-AMD setup took 9.60/1.89/1.97 ms for its first three stages, while the
scalar nested-dissection control took 14.94/1.86/2.58 ms. These are unqualified,
one-pass `vol-rel` observations; they are excluded from latency claims. The
deterministic work counts suffice to reject these candidates.

## Evidence and limits

Research source is frozen at
`926f0b0c6b8d2707e60a46e5487e6faa3f4a7dfa`, retained on
`feat/vol-r7-ordering-study-20261024`. It includes a default-off private graph
capture and opt-in study tests. It is research evidence, without release
include-hygiene qualification or a production ordering change.

Under `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/`:

- `joint-newton-graph-v1.json`: exact graph, mapping and baseline permutation.
- `joint-ordering-study-v1.json`: all six originally declared methods.
- `joint-ordering-study-v2.json`: complete table plus scalar control; every
  candidate permutation is retained for reproduction.
- `joint-ordering-study-v2-green.xml`: symbolic-counter and frozen-graph tests.
- `joint-ordering-study-provenance.json`: source, binary, graph and receipt hashes.

The graph export used `ATX_VOL_JOINT_GRAPH_RECEIPT=<file>` with the external
feasible-midpoint draft test. The study uses `ATX_VOL_ORDERING_GRAPH=<graph>` and
`ATX_VOL_ORDERING_RESULT=<file>` with `JointOrderingStudy.*`.

This result compares the declared methods on one frozen graph. It does not prove
global optimality of AMD or predict other instruments' graph structure. Any
future promising ordering would still require a full numerical fit and cold
price/identification/arbitrage audit because elimination order changes rounding.

AMD is a fill-reducing heuristic; its original implementation documentation
describes the ordering contract and numerical-factorization purpose.
[SuiteSparse AMD guide](https://github.com/DrTimothyAldenDavis/SuiteSparse/blob/dev/AMD/Doc/AMD_UserGuide.tex).
Nested dissection can reduce arithmetic and storage on mesh graphs by changing
elimination order; those theoretical mesh results do not establish a gain for
this irregular option constraint graph.
[George, 1973](https://epubs.siam.org/doi/10.1137/0710032).

The separately rejected cached/packed numeric-loop experiment is documented in
[cached-ldlt-experiment-r7.md](cached-ldlt-experiment-r7.md). It preserved every
factor/direction/output bit but did not demonstrate a reliable factor-time gain.
