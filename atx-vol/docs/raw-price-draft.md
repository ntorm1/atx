# Private raw-price projection draft

This is private staging and finalization for a proposed opt-in cold fitting policy.
The draft itself is never a completed fit. A separate finalizer can classify its
final prices and publish a complete slice; atomic public surface orchestration is
a separate integration step. The raw-midpoint candidate fails the frozen SPY
identification-preservation gate described below and is not promoted.

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
successful private projection. Original spreads and per-row provenance remain
available to the final-classification/curve stage. This proposed policy retains
all valid raw boxes instead of applying the historical midpoint-admission quality
filters. No draft can seed permanent
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

## Final-price slice publication

`finalize_raw_price_draft` consumes a successful projected draft. It validates
the quote/node mapping, prepared market/carry/policy identity, terminal prepaid
values and both retained certificate-bound records before inverse work. It
prepares any requested sigma/q families from raw Black-76 seeds only: no initial
American midpoint or bid/ask inversions are needed.

The finalizer batches explicit final node/side prices and calls the existing
typed repaired-price classifier once per unique node side. Native brackets,
explicit monetary candidate acceptance and historical typed Search fallback
retain their existing contracts. Original raw spreads supply monetary budgets;
recovered boxes never enlarge them. A real ConvexDense curve is then fitted to
unique identified final OTM observations, with raw-spread sensitivity weights
and the configured finite weight cap. The configured `min_quotes` remains the
minimum number of final observations; insufficient evidence returns an error.

Every original row is `ProjectedPrice` with `admitted=false` and `market_iv=NaN`.
`quotes_admitted` and `iv_attempted` remain zero. The separate
`final_price_observations` count reports actual curve inputs, and
`final_price_identified_nodes` reports unique node sides with an identified IV.
The rows contain the actual final Success or PriceOnly result. All node sides
are marked off-curve, and immutable final identifications are retained even
without a native family or monetary acceleration policy. Repeated node queries
reuse exactly those published answers. `fit_status=Success` is set last, after
the genuine curve, certified price representation, rows and cache exist. Any
failure returns no fit, permitting whole-surface fallback. Archive serialization
requires an explicit unsupported-metadata guard until these fields are persisted.

Six focused finalizer regressions cover duplicate reuse, no-family cache lifetime,
scalar batch bit parity, AVX2 cold repricing, locked/bidless PriceOnly metadata,
real projection moves, insufficient final observations, and malformed or
reconfigured draft refusal. The independent full-SPY smoke deliberately retains
the frozen minimum of 12,183 identified rows and currently fails it: all 33 slices
and 12,866 prices are served, with 15 original-box departures, but only 11,767 IVs.
Against the frozen wave1 predictions, 840 previously identified rows become
FloorVol and 424 prior PriceOnly rows become identified. All 840 demotions lie
within $0.000000565 above their intrinsic/terminal floor; 500 original midpoint
targets were already at or below that floor. Same-budget Fast Search reprobes
accept none of those 840 floor targets. This is a projection-target limitation,
not permission to invent an IV or relax identification.

The failed promotion evidence is retained under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/` as
`final-price-spy-diagnostic.xml`, `raw-final-price-diagnostic-v1.json` (all rows),
and `raw-final-price-demotions-v1.json` (exact IDs joined to immutable baseline
SHA-256 `681c3b44e3edbd799b90b03ccfaad2564d98a580085ae4131093cf749e08a8cf`).
No complete-fit accuracy, latency or analytics promotion follows from these
component tests. The raw midpoint objective needs correction before qualification.
