# Joint projection profile and next optimization

The exact feasible-midpoint SPY draft uses 12,866 free variables and 54,104
inequalities: 25,732 unary box rows, 13,669 two-term rows and 14,703 three-term
rows. The stored row capacity occupies 6,882,064 bytes. The normal matrix has
56,010 lower-triangle entries; the strict lower LDLT factor has 240,475 entries.
The existing solve takes 36 sweeps, including 35 numeric factorizations and
70 directions. Its final primal residual is 8.47576864266702e-11.

Optional private instrumentation adds no clock reads when its pointer is null.
It is never consulted by the solver, bounds, objective, stopping rule or final
certificate. The enabled/disabled regression preserves every fitted price bit,
iteration count, residual and success/exhaustion outcome. All 28 focused
solver/draft/target tests and scoped owning PCH-off checks pass.

Three fresh-process ThinLTO diagnostic replays were pinned to logical processor
6. Every replay matches all projected price bits in the integrated native
API artifact, FNV-1a fingerprint `8251046449056129050`. The normalized raw fixture,
binary and reference report hashes are captured in the receipt. There is no
initial IV fit, final IV reconstruction or final curve fit in this diagnostic.

| Stage | Observed range, ms |
| --- | ---: |
| Complete profiled projection | 357.85–398.21 |
| Constraint construction | 9.03–37.97 |
| Symbolic matrix/factor setup | 13.27–17.74 |
| Numeric factorization | 200.79–209.30 |
| Numeric matrix assembly | 12.62–13.80 |
| Direction RHS construction | 19.56–21.31 |
| Triangular solves | 37.92–38.69 |
| Slack/dual directions | 23.12–26.09 |
| Residual computation | 9.66–12.51 |
| Final independent certificates | 13.17–21.25 |

All three measured SMT-sibling activity values were zero. Total projection CV
was 5.36%, including the construction outlier, so there is no qualified aggregate
latency. These are stage-attribution observations, not a complete-fit benchmark
or a subsecond claim. Every trial remains in
`calendar/qp-profile-baseline-v1/receipt.json` under the r7 cache run.

Numeric factorization is the first optimization target. The implementation
already reuses Eigen's symbolic analysis and assembled coefficient positions;
adding that same cache again would not help. Inspection of the pinned Eigen
3.4.0 source (`SimplicialCholesky.h` and `SimplicialCholesky_impl.h`) shows each
numeric factorization still creates a permuted matrix and rediscovers each
factor row's topological reach pattern. The matrix graph remains fixed for
all Newton iterations of this solve.

The proposed next experiment caches the existing permutation, input-value
mapping, factor-row reach order and update boundaries once, then reuses numeric
workspaces. It must retain the same permutation and arithmetic order and compare
every matrix factor, solve result and complete projected price against the
current Eigen path. The original typed failure behavior and complete final
slice/calendar certificates remain mandatory. No gain is assumed until the
isolated replay is repeated. Alternative ordering, stopping rules and different
projection objectives are outside that bounded experiment.

Unary bound specialization and a reusable direction RHS can follow if measured
worthwhile. They affect roughly 100 ms of scans/solves, while the factorization
accounts for about 200 ms; removing only row allocation would miss the dominant
cost. No optimization implementation is included in this observational change.
