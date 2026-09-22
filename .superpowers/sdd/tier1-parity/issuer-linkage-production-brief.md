# Production brief: issuer query projection and qualified market association

## Implementation dispatch correction - root, 2026-09-22

The first implementation is the read-only issuer-content query surface.
Discover all actual source owner IDs from visible facts with exact normalized
CIK; do not construct only the unresolved owner ID suggested below. One CIK
may have both resolved and unresolved owners across periods. Handle ambiguous
or duplicate states explicitly and report whether upstream security-partitioned
growth splits across an owner transition. That producer issue is a separate
bounded repair, not permission to rewrite raw ownership or receipt hashes.
Direct CIK selection must work without directory evidence. A changed current
directory must not veto otherwise valid co-visible identifier history or leak
future identity evidence. A separate historical market association dataset is
deferred; first expose honest qualification metadata with issuer content.
The implementer owns an isolated draft while archive8 runs. Root will execute
focused verification after the writer is terminal; the planning-only runtime
restriction below does not prohibit that authorized implementation verification.

## Correction to the initial brief

The prior brief incorrectly said that an unresolved Company Facts CIK cannot
materialize statements, ratios, or derived metrics because it lacks a security
master link. Source inspection does **not** support that claim. The live zero
counts for `fundamental_statement_points`, `fundamental_standardized`, and
`derived_metric_values` are currently explained by the unrun/failed activation
path, not by a producer rejection of the isolated CIK owner.

`refresh_fundamental_fact_revisions` accepts a nonempty `security_id` from
`sec_company_facts` and never joins `securities`. In
`refresh_fundamental_statement_points`, the joins to
`security_industry_templates` and `securities` are left joins; the statement
map permits the `ALL` template when no industry row exists. The standardizer,
shares writer, accounting-ratio writers, periods/TTM builders, and derived PIT
engine group and partition directly on `security_id`; they do not require a
`securities` row. The archive's nonempty
`SEC-COMPANYFACTS-UNRESOLVED-CIK-0000093410` therefore already functions as a
stable, isolated source-issuer owner through those producers.

This correction materially reduces the proposal. Do **not** create a new
issuer namespace, rewrite raw ownership, or change `sec_company_facts` or
fundamental-point schemas during verified resume. Such mutations could
invalidate the retained 46.6M-row fingerprint receipts. Treat raw facts,
their placeholder security IDs, receipts, candidates, and archive-resolution
rules as immutable for this work.

## Actual gap and bounded outcome

There are two different unsolved questions.

1. A caller asking for historical financial content with ticker `CVX` at the
   current snapshot cannot select the existing isolated Company Facts owner:
   the normal API resolves symbols through dated
   `security_identifier_history`, while the current CVX CIK/ticker history
   starts 2026-09-20. It consequently cannot cover the earlier reporting
   range. This is a **current-as-of ticker-to-issuer lookup** problem, not a
   raw-fundamental ownership problem.
2. A CVX market bar is stored under `SEC-CIK-0000093410`, while historical
   filings are under `SEC-COMPANYFACTS-UNRESOLVED-CIK-0000093410`. A current
   directory observation can identify both by CIK for a present query, but it
   does not prove that the market security/share class was linked at a past
   filing or bar date. This is a **qualified historical market/security
   association** problem.

Implement the first now so the full US Company Facts universe can produce and
serve issuer-owned accounting statements, standardized items, TTM, accounting
ratios, shares, and declarative derived metrics. Add an explicit projection
and lineage surface for the second, but permit strict PIT market valuation only
when existing dated identifier evidence independently qualifies it. This does
not add a current fallback to archive loading or backdate ticker history.

## Existing identities and the required clocks

Keep the existing source owner exactly as stored:

```
SEC-COMPANYFACTS-UNRESOLVED-CIK-0000093410
```

It is deterministic for archive CIK `0000093410`, is already nonempty, and is
the partition key used by existing fundamental materializers. Expose it as
`issuer_owner_id` in the new issuer query API. It means only "Company Facts
content whose source CIK is this registrant"; it is not a tradable security,
share class, or current ticker. The source CIK already present in raw facts,
statement points, standardizations, TTM, ratios, and shares remains the
human-readable issuer key. Derived rows lack a CIK column, but their
`security_id` remains the same isolated issuer-owner ID and their lineage
identifies the standardized inputs.

The implementation must retain three clocks and return them separately:

| Purpose | Evidence | Clock | What it may establish |
| --- | --- | --- | --- |
| Financial-content visibility | Filing fact / derived input lineage | existing effective fundamental `available_at` (including the 46-hour policy) | Which filing/revision is visible |
| Current issuer selection | Co-visible `TICKER` and `CIK` `security_identifier_history` rows for one current-directory security (cross-checked to `sec_company_tickers`) | `issuer_lookup_as_of`, identifier interval/as-of date, and mapping availability | The CIK a caller means by a ticker at that current lookup time |
| Historical security attachment | `security_identifier_history` or other dated class-specific source | mapping `valid_from`/`valid_to` and `available_at` | A named security/share class at a past fact or bar time |

The first two are sufficient to answer "show historical fundamentals for the
issuer currently identified as CVX as of 2026-09-20." They do not establish
the third. Existing `resolve_company_facts_identifiers` retains the correct
third-clock rule: exact normalized CIK, interval covering the fact
availability date, and mapping known no later than the fact. Archive mode
must continue to pass `allow_current_fallback=False`.

## Minimal implementation slices

### 1. No-change production activation of issuer-owned accounting content

No raw or fundamental ownership schema change is needed. The activation
sequence can materialize archive placeholder owners as it does any other
nonempty `security_id`:

* `fundamentals.py:refresh_fundamental_fact_revisions`;
* `fundamental_statements.py:refresh_fundamental_statement_points`, periods,
  and TTM;
* `standardization.py:refresh_fundamental_standardized` and
  `_standardization_set_based.py`;
* `shares_outstanding.py:refresh_shares_outstanding_history`;
* `fundamental_ratios.py:refresh_fundamental_ratios`; and
* `derived_metrics.py:refresh_derived_metrics` / `_derived_pit.py`.

These paths must keep their existing `security_id` keys. In this scoped use,
the value is a source issuer-owner key, so documentation and the new query
projection must label it as such instead of claiming it is a market security.
This use is safe because the per-CIK archive placeholder prevents two CIKs
from sharing an owner partition. A future collision or a source that has only
a ticker and no immutable issuer ID would require a different design; neither
is present for SEC Company Facts.

The immediate operator work remains the established activation/materialization
run after archive work is terminal. It must not be folded into verified
resume/Archive8 source work and must not mutate retained raw fact rows or
receipt-bearing tables.

### 2. New current-as-of ticker-to-issuer query projection

Add a read-only issuer-resolution/query surface; it may be an API query
adapter plus a catalog schema and does not need a raw-table migration.

Affected files are:

* `src/atx_db/asof/fundamentals.py`: add issuer-owned statement, TTM,
  standardized, ratio, shares, and derived selection helpers. Filter
  statements/standardizations/TTM/ratios/shares by normalized `cik`; filter
  derived values by the resolved `issuer_owner_id`. Preserve the existing PIT
  revision ranking, including derived NULL/unavailable state ranking.
* `src/atx_db/api/catalog.py`: add separate issuer-content datasets whose
  natural owner is `issuer_owner_id`/CIK, not a historical security.
* `src/atx_db/api/service.py`: add an explicit issuer lookup path alongside,
  rather than inside, `WarehouseReadService._security_ids`. `_security_ids`
  remains the strict historical-security resolver.

The resolver receives `ticker` and `issuer_lookup_as_of`. It joins exact
upper-case `TICKER` and normalized `CIK` rows for the same security in
`security_identifier_history`, requiring both rows to cover the lookup date,
to have `as_of_date <= issuer_lookup_as_of`, and to have been available no
later than the lookup timestamp. `sec_company_tickers` is a current-directory
cross-check, not the historical interval authority. Its result carries:

```
requested_ticker, lookup_cik, issuer_owner_id,
directory_security_id, issuer_lookup_as_of,
directory_valid_from, directory_valid_to, directory_available_at,
directory_as_of_date, directory_source, lookup_method
```

`issuer_owner_id` is constructed as the existing archive form
`SEC-COMPANYFACTS-UNRESOLVED-CIK-` plus the normalized lookup CIK. The adapter
must not claim that `directory_security_id` is the owner or that the ticker was
valid before its observed identifier interval. If a desired lookup date has no
co-visible TICKER/CIK history, the API returns unresolved rather than falling
back to an undated current `sec_company_tickers` row.

The content query receives its separate `content_as_of`. It returns source
CIK, owner ID, raw/statement/standardized/derived lineage, and the selected
availability timestamp in addition to the lookup metadata. It must not reuse
`issuer_lookup_as_of` as filing availability or alter a fact's revision
selection. Current API methods and security-range behavior remain unchanged
for compatibility.

### 3. Qualified market/security association, separated from accounting

Add a small derived/read-only association surface after the issuer query works.
It need not rewrite bars, raw facts, identifier history, or their primary keys.
For a resolved current ticker/CIK, it may report candidate relation:

```
issuer_owner_id, cik, directory_security_id, market_security_id,
association_method, association_scope,
lookup_as_of, mapping_available_at, source_record_ref,
historical_security_qualified, unavailable_reason
```

For CVX, `directory_security_id` and `market_security_id` are currently
`SEC-CIK-0000093410`; `association_scope` is
`query_asof_current_cik_directory`, and
`historical_security_qualified=false`. The result can label/display the
market series for a present issuer query but cannot certify that the 2012--2026
bars or early filings were associated then.

The association may be upgraded to `fact_time_identifier_history` only when
the existing CIK-history predicates cover the relevant fact/bar time and the
mapping was known then. This can be computed from
`security_identifier_history`; no history row may be synthesized from the
current directory. Multiple security candidates, class ambiguity, a late
mapping, or no mapping result in an explicit unavailable/ambiguous status.

Keep `valuation_multiples.py:load_market_cap_inputs` and
`_market_cap_stage_sql` security-keyed for now. They currently equate
`equity_daily_bars.security_id` and `shares_outstanding_history.security_id`.
They must consume only a qualifying historical association before a future
issuer-share-to-market-security join. Do not make the current-as-of candidate
relation eligible for `market_cap`, `valuation_multiples`, factor panels,
backtests, or certified historical output. Accounting ratios and EPS growth
do not require that market association and can be published now.

### 4. Operational ordering

1. Finish VR1 and Archive8 without raw fact/schema/ownership edits, preserving
   verified receipt and fingerprint semantics.
2. Run the ordinary post-archive fundamental materializers. Diagnose any
   real producer failure independently; do not pre-emptively change identity
   keys.
3. Implement and validate the issuer query projection against materialized
   content.
4. Add the qualified market-association surface, then separately schedule
   dated listing/class evidence before expanding strict market valuation.

## Compatibility and migration implications

The issuer-content slice requires no migration and no data rewrite. It reads
existing `cik` columns and existing isolated owner IDs. It introduces only new
read API/catalog behavior and response metadata.

The qualified-market slice can initially be a view/query. Persist a bridge
only if measured API/provenance requirements need it; if so, reserve the next
migration number at implementation time, make it additive, and seed it only
from existing recorded directory/history evidence. It must never alter
`sec_company_facts`, `fundamental_fact_revisions`, statement points, or
identifier-history intervals as part of receipt-preserving archive work.

Existing security APIs continue to mean historical security IDs. New issuer
results are deliberately a different schema so callers cannot mistake a
current ticker lookup for historical share-class proof.

## Validation

No tests or runtime work are authorized by this brief. The later focused
validation must demonstrate:

* An archive placeholder CIK has statement/standardized/TTM/ratio/share/derived
  rows after materialization without a matching `securities` row. Its ID is
  stable across all those paths.
* The CVX current lookup at 2026-09-20 maps `CVX` to CIK `0000093410` and the
  existing source owner, while exposing the actual identifier interval and
  availability. It
  does not claim ticker validity in 2025 or early 2026.
* Content revision selection uses the effective filing clock independently of
  lookup time. A later current directory lookup neither makes an old filing
  visible earlier nor changes its selected revision.
* Security-range APIs still reject a ticker whose dated identifier interval
  does not cover the requested historical range; the issuer endpoint is the
  intentional, labelled alternative.
* CVX Q1/Q2 direct per-share inputs yield the documented quarterly YoY values
  once materialized: -44.5% and +321.3793103448%. Q4 stays unavailable until
  a direct reported-Q4 source is ingested; it must never use FY 6.63 minus
  nine-month 5.27 to emit 1.36.
* CVX's present CIK-directory association to `SEC-CIK-0000093410` is reported
  as unqualified for historical market/security use. A late or non-covering
  CIK history row cannot enable strict market cap/valuation.

## Non-goals

This work does not add reported-Q4 press-release coverage, infer historical
listing/exchange/common-stock status, resolve issuer successors or share
classes, or reclassify historical price bars. Those are required only to grow
strict historical market joins, not to serve CIK-owned accounting content for
a current-as-of issuer lookup.
