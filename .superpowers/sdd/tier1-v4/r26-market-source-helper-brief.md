# Shared market source helper under existing Task17 R26 repair2/5

This supplies the missing original market admission needed by both Task17 and
priority Task1 P9 repair2/5. It is not a new producer, allowance or task counter.
Read R26, R27, task17-integration-source-map.md and P9 Part A. Coordinate the
fixed stage/relation contract with both owners before writing it. Task1 retains
all index-specific ranking/eligibility/release code; Task17 retains factor,
daily/reference, ownership, golden/catalog/registry integration.

After explicit dispatch, own only new:
- atx-db/src/atx_db/alpha_panel/private_tier1_market_sources.py
- atx-db/tests/test_private_tier1_market_sources.py
- .superpowers/sdd/tier1-v4/task17-market-sources-helper-report.md

No subagents. No edits to prices.py, shares_daily.py, market_common.py,
liquidity.py, borrow_proxy.py, security_master.py, corporate_actions.py, identity,
FINRA, fundamentals, R23, common, foreign panel or consumer files. All frozen
producer and review caps remain. Existing private replay helpers are read-only
dependencies unless their owner separately authorizes a change.

Use VERSION tier1-market-sources-v1. Provide bounded materialize/admit(name,spec,con), iter_rows and consuming-style
interfaces consistent with the reviewed source-helper family. Bind a complete,
independently expected fixed original source plan, actual original bytes,
native/reviewer/code inventories, complete population and source-clock/economic
proof, native products and immutable mapped products. Before/after materialize
and public reads must authenticate every consumed dependency. A native Parquet,
manifest hash, PASS Boolean, caller-selected proof subset or populated value
alone is insufficient. No caller ability to redirect fixed production readers
into arbitrary Parquet or waive missing original evidence.

Keep fixed stage contracts acyclic: prices/vendor-calendar first, then shares,
then liquidity. These core stages must be independently admissible without
borrow_proxy, which later consumes admitted market, short, ownership, FTD and
threshold inputs. Ownership itself needs shares as a denominator; requiring the
entire borrow bundle to admit shares would create a dependency cycle. A stage
selector chooses a fixed complete contract, never a caller-selected proof subset
within that stage. Keep planned exchange-calendar evidence separately bound.
Exact native products include calendar.parquet, prices/year=*/prices.parquet,
market/shares_daily/year=*/shares_daily.parquet,
market/liquidity/year=*/liquidity.parquet and borrow_proxy/year=*/*.parquet.
Confirm this small interface with both consumers before implementation.

Required phases and exact reusable native boundaries:
- prices.py build_calendar63, project84, repair_sid0120, _steps_sql200,
  jump_counts234, finalize_bucket263 and write_years336: original ORATS
  TickerHistory3 types/units/complete axes, sid0 original-wins/brackets, mass
  repair populations, action decisions, clean/guarded returns and full outputs.
- market_common.py calendar_sql48, project_prices59, split_events_sql93;
  original corporate_actions classification and source/action identities.
- shares_daily.py known_link_sql75, prepare85 and bucket_sql117: all actual
  original financial/identity/classification/action dependencies, native share
  values, basis, time/staleness/matching/conflict/line-count populations.
- liquidity.py bucket_sql76: all original full-window ADV/turnover/spread/Amihud
  semantics. Reuse Task17's connection-taking replay helper where complete.
- borrow_proxy.py build53 inline relational phases: complete original SI,
  institutional, FTD and threshold-list/source-file populations, visible latest
  order, staleness, original clocks and strict prior-session22UTC. It remains
  a public-input proxy, never lending fee/utilization/lendable supply.

Line references identify the currently inspected code, not a substitute for
actual dependency pins. Native entrypoints that create fixed scratch, mutate
global roots or set resources must not run through globals overrides. The
smallest explicit-relation/connection-taking projection of their original SQL
is authorized, following the notes/short precedent. Preserve formulas, types,
rows, duplicate semantics, exclusions and clocks, with independent full typed
duplicate-preserving native-SQL oracle comparisons. Do not clone a whole
producer, monkeypatch globals or change frozen byte identities.

Time-relative qualification is distinct from reproducing native output:
- SID0 mappings require their actual bracketing evidence; future matches cannot
  be backdated to the original bar. Preserve excluded rows and their reasons.
- Native adj_close anchors at the final raw close; distinguish supported return
  ratios from absolute levels. Do not confer early availability on future anchors.
- shares_daily.prepare uses full-history mode(finra_type), and native share
  basis depends on ADR/non-equity and issuer-line classification. Exact native
  replay does not prove those decisions were available at a historical cell.
  Authenticate independently dated classification/link/share evidence at that
  cell, retain each unsupported dependency/reason, and implement reachable
  qualified paths. Future/directory classifications cannot influence qualified
  shrout or its selection even indirectly. Preserve full exclusion denominators.
- A native vendor-row-threshold calendar does not establish an independently
  scheduled exchange calendar. Task1 needs separately authenticated planned
  sessions known by each rank22UTC mark (including future recon next session),
  and realized sessions known by later daily/exit clocks. A later calendar
  revision must not rewrite a rank vintage. Keep the roles and evidence separate.
- Missing data must remain missing; zeros for absent FTD/threshold events require
  authenticated complete corresponding public file/list populations. SI vintage
  risk, manager/13F/source-body gaps, stale observations and unknown publication
  clocks remain explicit. Do not borrow other stages' qualification flags.

Task1 needs complete line-session price/shares/liquidity inputs, raw close,
ticker/namespace and available_at, qualified shrout basis/source dates/clocks,
adv_63 with full window lineage, complete line population/last observations,
explicit delisting evidence and vendor endpoint, plus dated identity mapping.
It owns indexes.snapshot original price-times-shrout/liq projection and eligible
company summation; all decisive classification there remains independently dated.
Task17 consumes the same admitted market inputs for its original field families.

Historical source-return predictors may extend beyond2022 under R21; all return
evaluation and complete forward-label windows remain strictly pre2023. No new
feature search, source-rights waiver, later consumer seal or actual job follows.
Missing real sources remain actual prerequisites; an always-empty helper is not
completed reachable engineering. Positive original-source-shaped routes and
adversarial missing/mutated/future/partial-source cases are required.

Only controller-serialized focused offline checks are authorized: exact node IDs,
explicit single pytest worker, file-backed128MB/one thread, original guard<=1GiB,
each invocation<60s. Root owns real source/hash/data/build/HTTP/consumer jobs.
Keep completed test evidence, resource deviations and changed-code distinctions.
Preserve unrelated outputs and clean only exact owned disposable test scratch.

Freeze the exact three owned paths after index coordination, with required
repository trailer. Report complete source/API/formula/clock contracts, proof
dependencies, native equivalence, exclusions and remaining actual gates.
Independent helper review must exclude its author. Later combined Task17 review
must separately cover Task17-owned wiring and cite independent helper coverage.

## Scoped transaction efficiencies (same R26 repair2/5)

Complete large-source/code/product inventories must be authenticated before and
after one consumption transaction, including errors/early close, without a
whole-source rehash for every small row batch or point lookup. Keep copied fixed
bindings and no successful consumption proof on incomplete/error exit. Original
typed replay/admission remains mandatory; this changes orchestration only.

The shared helper may expose an explicit-connection vendor price scope/point API
for Task17 NativeWindows. Project exactly mna_price_native_v2.read_vendor_price
112-166 over a fully authenticated original vendor relation, reuse unchanged
read_economic, and preserve full original schema, exact SID/date and ticker/date
duplicate/NULL ambiguity, finite positive Decimal close, rawUSD economics and
all source/dated clock maxima. Bind original bytes before/finally the whole
transaction. No persistent diagnostic cache, callerParquet substitution, frozen
function/global override or final native M&A replay shortcut. Prove full typed
source-shaped native equivalence and mutation/early-close refusal.

Original NYSE calendar source metadata for fixture design is now in
nyse-calendar-source-leads-v1.md and nyse-calendar-structure-preflight-v1.json.
The five root-captured original HTML pages are unqualified: date-only clocks,
weekday authority and complete amendments remain open. Implement a reachable
original HTML table/covered-year/dated amendment route; generated self-declared
CSV/JSON proof is not original authority. No author source job follows.
