# Private raw-price projection draft

This is phase 1 of a proposed opt-in cold fitting policy. No public fitting mode
uses it yet. It produces a privately certified price draft, not a completed fit.
Final economic IV identification, genuine curve construction, immutable query
records, and atomic public surface publication remain phase 2 work.

`make_raw_price_draft` calls the same `prepare_fit` market, clock, carry and model
validation used by existing fits. It preserves every original quote and its
mapping to a distinct strike. Derived terminal prepaid spots and discount are
computed on the actual declared model; horizon bounds include the whole exercise
interval, including signed rate/carry cases. There are no American model prices
or inversions in draft construction or projection.

Raw bid/ask boxes are exact; the existing repair half-tick setting does not widen
this policy's boxes. Duplicate quotes intersect their boxes, and their target is
the midpoint of that original intersection. For a single quote this is its raw
midpoint. Original quotes and targets remain unchanged by terminal-floor ask
recovery. Recovery is opt-in and raises only an individual original ask below
its own proven intrinsic/terminal European floor by the existing fixed
certificate allowance. Recovery happens before duplicate intersections, so an
unchanged duplicate ask above the floor is never silently widened. Selected C3
call floors tighten lower bounds and cannot widen asks.

The draft retains `raw_targets` separately from its evolving node values, so
final publication can measure movement from the original input after projection.

Missing paired sides, invalid quote rows, unresolved carry policies and DatedCash
return explicit refusal kinds. Unsupported exercise regimes and shared carry
conflicts also refuse. Nothing is silently omitted. A caller may choose the
existing fitting policy after an unsupported draft; no such public dispatch has
been implemented. This first draft stage requires at least two distinct strikes.

Input quality/admission decisions are not relabeled: draft metadata records only
validated market/carry and `quotes_seen`. It has no success rows, fitted curve or
public serving prices, and its fit status remains non-Success even after a
successful private projection. Original side policy, quote quality, original
spreads, per-row provenance and final identifiability must be applied and recorded
by the later final-classification/curve stage. No draft can seed permanent
PriceOnly flags. Its original quote array is the source for economic budgets;
recovered boxes must never be used to enlarge those budgets.

`project_raw_price_drafts` orders the solve by calendar maturity without changing
caller slots. It uses the reviewed coupled call/put, strike-shape, terminal-floor
and all-pair calendar solver. Node changes and private certificates commit only
after every expiry succeeds; failures preserve all drafts. The solver promises a
certified feasible point, not exact quadratic optimality. The prepared public-fit
metadata remains unqueryable throughout.

Calendar maturities must be distinct in this initial private policy; a duplicate
maturity returns InvalidInput without changing any draft.

Tests cover exact feasible prices, duplicates, unchanged duplicate asks during
recovery, missing pairs, invalid/carry/model refusals, signed funding carry,
nonconvex targets, exhaustion rollback, and partially overlapping domains in
reversed maturity order. An independently exported fixture contains all 33 SPY
positive-time slices and 12,866 original quotes, normalized market/carry only:
SHA-256 `364abcecd0fb7af1e05915ed2d1e2e7aeac58c040d26bbf3ff417d800d3d9133`.
It projects with exactly 15 original-box departures, all from the proved floor
conflicts, while retaining every row, in 40 of the default 100 iterations. The
test pins zero AL boundary solves and
zero American IV iterations. This is correctness evidence only; it is neither a
complete-fit accuracy/latency result nor a claim about analytics quality.

The nine draft tests pass, alongside 19 existing solver/call-floor tests (one
optional old external-box fixture was not selected). Owning production and test
objects compile with PCH off. This exposed and fixed a missing private
`nlohmann_json` header dependency on the umbrella test target for the raw and
existing joint fixture readers; no umbrella executable was built.
