# Fast IC48 library and streaming composition

User steering stops further corporate-action/detail expansion and starts a separate fast alpha research phase. This packet does not change the original 24 candidates, their recipe, event inputs or previous attempt receipts. Previously researched event notes remain in the conversation; no further research, data acquisition or event configuration was performed for this packet.

Production: `c4c8dc59`. Postimplementation fixtures: `67b28ba0`.

## Fixed research library

`atx-impl/strategies/slow_price_volume_ic48_v1.json` uses new schema `atx.dsl-ic-library/v1`: 48 candidates = eight families x three templates x two smoothers (21 and 63 sessions). Families are momentum, trend quality, medium reversal, defensive risk, liquidity state, flow confirmation, price location and trading-year seasonality. The last family is explicitly a 252-session proxy, not calendar seasonality. Family labels express hypotheses, not statistical independence.

Only `close`, `raw_close`, and `volume` are used. Returns/range measures use the vendor adjusted-close proxy; dollar liquidity uses raw close times raw share volume. No adjusted OHLC candles, industry, cap or unavailable fundamental inputs are invented. Zero-denominator/flat rolling cases remain unavailable according to VM semantics. Expressions are fixed before performance observation. Maximum declared lookback is 314 prior bars (315 complete observations), within the requested 320-session warmup.

Exact library SHA256: `1ec75242f0328a459ac114256f99f534eb0b8ec2e642794d3f4ad846779a09ff` (15,850 bytes).

Sidecar `slow_price_volume_ic48_v1.recipe.json` SHA256: `3b6707f7a46e392391281a9f84585e6c6b1a1b5567cd9ff2656931b72481130b` (16,479 bytes).

`generate_slow_ic_48.py` creates both exact JSON artifacts without market inputs, random search or fitting. `python atx-impl/strategies/generate_slow_ic_48.py --check` passed. Its expression builder records lookback provenance; only the native compiler can qualify the actual DSL lookback and slot count.

## Actual composition implementation

New private implementation `atx-impl/src/strategy_ic_composition.cpp`, lightweight owning header `strategy_ic_composition.hpp`; root registers the CPP. `IcComposition::create` copies the eligible mask and bounded candidate IDs/families. `add(index, signal, frozen_sign)` takes one candidate at a time, in exact library order. TRAIN can calculate that candidate's 21-session rank-IC orientation and immediately accumulate its still-live signal before discarding it. Validation supplies the same frozen sign. No repeated VM pass or population of retained D*N candidate arrays is needed.

Every finite eligible row is converted to centered tied ranks. A candidate receives `1 / family_count / candidates_in_its_family`: exactly 1/48 for this library, equivalent to 1/8 family x 1/3 template x 1/2 variant. An undefined/zero TRAIN orientation uses sign0 and contributes neutral zero. Missing candidate values likewise contribute zero, with fixed denominators. Neither daily missingness nor validation evidence redistributes a family budget. All-zero and all-tied blends produce flat targets.

The result owns one D*N combined signal plus O(D) coverage/turnover metadata. Nonmembers remain NaN. Coverage is fixed-weight finite ranked contribution divided by the original eligible-name count, distinct from invested breadth. The runner owns IC labels, exact recipe/role pins and orientation artifacts; this helper sees no returns or fit evidence.

## Planned turnover diagnostic

`finish` builds desired centered tied-rank, dollar-neutral gross1 targets without winsorization. Every fifth decision session, anchored to the role's declared score beginning, planned weights interpolate 25% toward those targets. It does not apply price drift, trade fills, cash accounting, fees, borrow or liquidity caps. Membership exits force zero immediately, including off cadence; survivors are not renormalized. Daily planned gross and net expose departures from the desired 1/0 properties after partial updates and exits.

Turnover is `sum(abs(new_planned_weight - previous_planned_weight))`. Initial deployment is included in daily and total turnover and also reported separately. This is intended target change only; it cannot establish actual turnover, capacity, net Sharpe or profitability.

## Bounds and validation

Preallocation validates D*N overflow, exact mask shape/values, finite fraction in (0,1], nonempty decision window, bounded unique IDs and at most 256 candidates/32 families. The allocation envelope charges D*N*(8+1), D*(4*8+sizeof(usize)), N*(sizeof(pair<double,usize>)+2*8), 512 bytes/candidate and 4096 bytes fixed overhead. This covers owned blend/membership/result/scratch; caller panel, incoming signal, VM and label cache are separate and must be included in runner admission. Allocation failures return typed errors. No candidate-time allocation is required. Ordered exactly-once input and complete finish are enforced.

Five new source fixtures in `strategy_ic_composition_test.cpp` cover actual 48-artifact parsing/ResearchFast execution on a 420x64 synthetic panel, authoritative lookback/slot admission, readiness, future and forever-ineligible mutation invariance, unequal family counts, neutral missing budgets, planned off-cadence exits/deployment, flat/unoriented behavior, causal composition prefixes, and preallocation/sequence refusals. These fixtures are implementation-following and **have not been compiled or executed in this lane**. `git diff --check` passed. Parent owns one focused native batch and actual IC research.

48 orientation fits and one fixed blend are predeclared; three horizons and related smoothing variants do not imply independent discoveries. All actual candidates/attempts remain counted, including unavailable candidates. No selective pruning, learned blend weights, optimizer grid, realistic book prerequisite or performance claim is introduced.
