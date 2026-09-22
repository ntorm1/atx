# CVX quarterly diluted-EPS YoY: static production-route audit

Scope: source-only review for the as-of `2026-09-20T22:00:00Z` query, covering
quarterly GAAP diluted EPS and same-quarter YoY growth. No database, runtime, or
live-row inspection was performed here.

## Production route and identifiers

The answerable derived metric is:

| Layer | Table / code | Required identity |
| --- | --- | --- |
| Reported input | `fundamental_statement_points` | `canonical_metric = 'eps_diluted'`, `concept = 'EarningsPerShareDiluted'`, `unit_type = 'per_share'`, a 3-month duration |
| Standardized input | `fundamental_standardized` | `item_id = 1035`, `canonical_code = 'eps_diluted'`, `basis = 'quarterly'`, `rule_id = 'std_quarterly_1035'` |
| User-facing calculation | `derived_metric_values` | `metric_code = 'eps_diluted_q_growth_yoy'`, `metric_window = 'q'`, source `atx-db declarative derived metrics v1` |

The item map explicitly assigns `us-gaap:EarningsPerShareDiluted` to
`eps_diluted` / item 1035 ([concept_map.csv](C:/atx/atx-db/src/atx_db/seeds/concept_map.csv#L228));
the statement map makes it a `per_share`, duration, canonical diluted-EPS fact
([statement_map.csv](C:/atx/atx-db/src/atx_db/seeds/statement_map.csv#L82)).
Net income and diluted weighted-average shares are distinct inputs: `NetIncomeLoss`
maps to `net_income` / item 1031 ([statement_map.csv](C:/atx/atx-db/src/atx_db/seeds/statement_map.csv#L156)),
while `WeightedAverageNumberOfDilutedSharesOutstanding` maps to
`shares_diluted_avg` / item 1041 ([statement_map.csv](C:/atx/atx-db/src/atx_db/seeds/statement_map.csv#L265)).
The quarterly EPS route therefore selects the reported EPS tag; it is not a
net-income ÷ share-count reconstruction.

The derived seed is exactly `yoy(eps_diluted)` on the quarterly grid
([derived_metric_definitions.csv](C:/atx/atx-db/src/atx_db/seeds/derived_metric_definitions.csv#L104)).
The DSL uses the fourth prior quarter and returns `(current-prior)/abs(prior)`
([derived_dsl.py](C:/atx/atx-db/src/atx_db/derived_dsl.py#L505)). This is the
same-quarter YoY definition requested, unlike `eps_diluted_growth_yoy`, which
is YoY growth of trailing EPS ([derived_metric_definitions.csv](C:/atx/atx-db/src/atx_db/seeds/derived_metric_definitions.csv#L78)).

## PIT selection

Raw statement as-of selection partitions by security, metric, exact start/end
window, and unit, then keeps the latest visible filing vintage
([fundamentals.py](C:/atx/atx-db/src/atx_db/asof/fundamentals.py#L53)). It
filters both `as_of_date` and `available_at` ([fundamentals.py](C:/atx/atx-db/src/atx_db/asof/fundamentals.py#L68)).
The derived writer persists filing-event states, including unavailable states,
with `period_end`, `available_at`, `revision_group_id`, `target_bucket`, and
input lineage ([api/catalog.py](C:/atx/atx-db/src/atx_db/api/catalog.py#L617)).
For a direct SQL read, rank the derived state by `revision_group_id` (or its
stable bucket identity) before filtering outputs; do not filter out NULL values
before the rank. The public API applies the same special derived-state handling
([api/service.py](C:/atx/atx-db/src/atx_db/api/service.py#L461)).

For SEC Company Facts, the governed effective raw clock is
`greatest(available_at, filed_date + 46 hours)`, not a measured acceptance
timestamp ([\_fundamental_clock.py](C:/atx/atx-db/src/atx_db/_fundamental_clock.py#L13)).
The policy documentation says it is a conservative date-based policy and not
proof of acceptance or delivery time ([FUNDAMENTAL_CLOCK_POLICY.md](C:/atx/atx-db/docs/FUNDAMENTAL_CLOCK_POLICY.md#L5)).
The mapped statement/standardized/derived materializations must have been
rebuilt for that corrected clock to propagate ([FUNDAMENTAL_CLOCK_POLICY.md](C:/atx/atx-db/docs/FUNDAMENTAL_CLOCK_POLICY.md#L28)).

## Q4 semantics: no proven per-share subtraction bug

The standardization path first accepts a direct 70–120-day statement EPS fact
as `basis = 'quarterly'` ([\_standardization_set_based.py](C:/atx/atx-db/src/atx_db/_standardization_set_based.py#L177)).
Its only discrete-quarter subtraction staging explicitly requires
`unit_type = 'monetary'` ([\_standardization_set_based.py](C:/atx/atx-db/src/atx_db/_standardization_set_based.py#L344)); that staging labels a derived Q4 only for those monetary values
([\_standardization_set_based.py](C:/atx/atx-db/src/atx_db/_standardization_set_based.py#L449)).
It cannot derive a quarterly per-share fact from FY EPS less nine-month EPS.
The TTM statement builder has the same monetary-only YTD gate
([fundamental_statements.py](C:/atx/atx-db/src/atx_db/fundamental_statements.py#L1618)).

Therefore the known FY-minus-nine-month discrepancy is **not** a statically
proven transformation defect. A valid Q4 answer requires a direct three-month
`EarningsPerShareDiluted` fact. If the post-writer inspection finds no such
visible CVX fact, it establishes a raw/source-coverage gap for this route; it
does not show that this code fabricated the incorrect per-share subtraction.

The distinct limitation is material: the mapped core route starts from SEC
Company Facts (the statement-map source on the EPS mapping above). The separate
press-release loader is explicitly injectable ([press_release.py](C:/atx/atx-db/src/atx_db/press_release.py#L1)) and is scheduled as `press_release_facts`, dependent on
`est_actual`, rather than as an input to the standardized EPS route
([jobs.py](C:/atx/atx-db/src/atx_db/jobs.py#L1526)). Thus this static review
cannot claim Chevron IR-release coverage in the core query path.

## Post-writer read-only inspection

Use the production snapshot cutoff, `TIMESTAMP '2026-09-20 22:00:00'`. First
inspect the raw source (CIK is included to avoid ticker-resolution ambiguity):

```sql
WITH raw AS (
  SELECT *, greatest(available_at, CAST(filed_date AS TIMESTAMP) + INTERVAL 46 HOUR) AS effective_at
  FROM sec_company_facts
  WHERE source = 'SEC companyfacts' AND cik = '0000093410'
    AND taxonomy = 'us-gaap' AND concept = 'EarningsPerShareDiluted'
), visible AS (
  SELECT *, row_number() OVER (
    PARTITION BY period_start, period_end, unit
    ORDER BY filed_date DESC, effective_at DESC, source_loaded_at DESC
  ) AS rn
  FROM raw WHERE effective_at <= TIMESTAMP '2026-09-20 22:00:00'
)
SELECT period_start, period_end, filed_date, fiscal_year, fiscal_period, form,
       accession_number, unit, value, available_at, effective_at
FROM visible WHERE rn = 1
ORDER BY period_end, period_start;
```

Then inspect the mapped/standardized/derived route. This query preserves empty
or NULL results as evidence for the appropriate layer rather than inventing a
Q4 value:

```sql
WITH p AS (SELECT TIMESTAMP '2026-09-20 22:00:00' AS cutoff),
raw_eps AS (
  SELECT s.*, row_number() OVER (
    PARTITION BY s.security_id, s.canonical_metric, s.period_start, s.period_end, s.unit
    ORDER BY s.as_of_date DESC, s.available_at DESC NULLS LAST,
             s.source_loaded_at DESC NULLS LAST, s.statement_point_id DESC
  ) AS rn
  FROM fundamental_statement_points s, p
  WHERE s.symbol = 'CVX' AND s.canonical_metric = 'eps_diluted'
    AND s.concept = 'EarningsPerShareDiluted' AND s.unit_type = 'per_share'
    AND s.period_type = 'duration'
    AND date_diff('day', s.period_start, s.period_end) + 1 BETWEEN 70 AND 115
    AND s.as_of_date <= CAST(p.cutoff AS DATE) AND s.available_at <= p.cutoff
), std_eps AS (
  SELECT s.*, row_number() OVER (
    PARTITION BY s.security_id, s.item_id, s.basis, s.period_end, s.rule_id
    ORDER BY s.available_at DESC, s.revision_sequence DESC, s.standardized_id DESC
  ) AS rn
  FROM fundamental_standardized s, p
  WHERE s.symbol = 'CVX' AND s.item_id = 1035 AND s.canonical_code = 'eps_diluted'
    AND s.basis = 'quarterly' AND s.rule_id = 'std_quarterly_1035'
    AND s.available_at <= p.cutoff
), derived_eps_yoy AS (
  SELECT d.*, row_number() OVER (
    PARTITION BY coalesce(d.revision_group_id, d.derived_value_id)
    ORDER BY d.available_at DESC, d.derived_value_id DESC
  ) AS rn
  FROM derived_metric_values d, p
  WHERE d.source = 'atx-db declarative derived metrics v1'
    AND d.metric_code = 'eps_diluted_q_growth_yoy' AND d.metric_window = 'q'
    AND d.available_at <= p.cutoff
    AND d.security_id IN (SELECT DISTINCT security_id FROM raw_eps)
)
SELECT 'raw_statement' AS layer, period_end, period_start, fiscal_year, fiscal_period,
       value, accession_number AS provenance, available_at, NULL::VARCHAR AS status
FROM raw_eps WHERE rn = 1
UNION ALL
SELECT 'standardized', period_end, period_start, fiscal_year, fiscal_period,
       value, source_accession, available_at, combination_rule
FROM std_eps WHERE rn = 1
UNION ALL
SELECT 'derived_q_yoy', period_end, fiscal_period_start, NULL, NULL,
       value, revision_group_id, available_at, value_status
FROM derived_eps_yoy WHERE rn = 1
ORDER BY period_end, layer;
```

For the three requested periods, verify that the raw and standardized rows have
three-month windows and that the derived rows are `value_status = 'valid'` with
`value_origin = 'quarterly'`. Compare each derived result to the selected raw
current quarter and its matching year-prior raw quarter; do not compare it to
`eps_diluted_growth_yoy` or to FY-minus-nine-month EPS.

## CVX Company Facts identity route (static follow-up)

The observed `SEC-COMPANYFACTS-UNRESOLVED-CIK-0000093410` is an expected
*initial archive-member target*, not enough evidence of a resolver defect.
When `symbol_source = 'archive_members'`, target discovery intentionally creates
`(CIK<digits>, cik, SEC-COMPANYFACTS-UNRESOLVED-CIK-<digits>)`
([fundamentals.py](C:/atx/atx-db/src/atx_db/fundamentals.py#L325)). The prefix is
documented as a source-issuer identity rather than a security-master/bar ID
([fundamentals.py](C:/atx/atx-db/src/atx_db/fundamentals.py#L59)). It is not
inserted into `securities` in the archive fixture contract
([test_companyfacts_zip.py](C:/atx/atx-db/tests/test_companyfacts_zip.py#L195)).

### Conditions for a fact-time link

For **each individual fact**, Company Facts assigns a preliminary availability
of `filed_date + 22h` ([fundamentals.py](C:/atx/atx-db/src/atx_db/fundamentals.py#L680))
and resolves its exact, zero-padded CIK and that fact's own `available_at`
([fundamentals.py](C:/atx/atx-db/src/atx_db/fundamentals.py#L418)). A link to a
market security exists only when all of these hold:

1. `security_identifier_history` has `id_type = 'CIK'` and `id_value` exactly
   equals `0000093410`.
2. Its `[valid_from, valid_to)` interval covers the fact availability date.
3. Its own `available_at` is NULL or no later than the fact availability.
4. Its `security_id` is the intended existing market-security ID.

Those are the resolver's priority-1 join predicates
([fundamentals.py](C:/atx/atx-db/src/atx_db/fundamentals.py#L434)). Multiple
eligible mappings are deterministically ordered by priority, mapping
availability, valid-from, load time, then security ID
([fundamentals.py](C:/atx/atx-db/src/atx_db/fundamentals.py#L461)). This prevents
a mapping learned after a filing from being used for that filing.

For a resolved security, `entity_id` is independently selected from a visible
`ENTITY_ID` history row with the same interval and knowledge-time tests
([fundamentals.py](C:/atx/atx-db/src/atx_db/fundamentals.py#L470)). In
archive mode an absent entity still produces an unresolved-ledger record, but
does not undo a resolved security ID ([fundamentals.py](C:/atx/atx-db/src/atx_db/fundamentals.py#L519)).

The only alternate security candidate is the current `sec_company_tickers`
mapping, and only when *no CIK history exists at all* for that CIK
([fundamentals.py](C:/atx/atx-db/src/atx_db/fundamentals.py#L450)). The
activation archive path explicitly disables this fallback:
`allow_current_fallback = not archive_mode`
([fundamentals.py](C:/atx/atx-db/src/atx_db/fundamentals.py#L1250)). A present
current CVX ticker/CIK row consequently cannot link the archived Q1/Q2 facts.
That is deliberate PIT behavior, not a missing downstream join.

The activation does run `security_master` before `companyfacts_load`
([activation.py](C:/atx/atx-db/src/atx_db/activation.py#L46)), but that loader
creates its CIK/ENTITY_ID history with `valid_from = activation as-of date` and
`available_at = now()` ([security_master.py](C:/atx/atx-db/src/atx_db/security_master.py#L447)).
Such a newly observed mapping cannot prove identity for a prior filing-time
fact. Whether CVX has a separate qualifying historical mapping to the market
security is a live-data question.

### No scheduled automatic relink

`resolve_company_facts_identifiers` has one production caller: the Company
Facts load ([fundamentals.py](C:/atx/atx-db/src/atx_db/fundamentals.py#L1250)).
After that stage, activation only rebuilds statement points, periods, TTM,
calendarization, and standardization ([activation.py](C:/atx/atx-db/src/atx_db/activation.py#L54)); those stages do not rerun identity resolution. Facts that
cannot link retain the archive placeholder and are routed to an auditable,
zero-confidence, `proposed` Company Facts CIK candidate
([fundamentals.py](C:/atx/atx-db/src/atx_db/fundamentals.py#L549)).

The generic decision manager does not currently repair this candidate: its
accepted-identifier application consumes only CUSIP candidates
([identifier_decisions.py](C:/atx/atx-db/src/atx_db/identifier_decisions.py#L238)).
Thus a later scheduled activation stage will not link the placeholder to CVX.
A later **Company Facts reload** can resolve the rows only after the required
fact-time CIK history exists; the normal non-archive loader can use its
current-ticker fallback, but that is expressly not historical identity evidence.

### Bounded post-writer identity inspection

Run these read-only checks with the live security IDs rather than assuming that
`SEC-CIK-0000093410` is the market-security identity:

```sql
SELECT security_id, id_type, id_value, valid_from, valid_to, as_of_date,
       available_at, source, source_loaded_at
FROM security_identifier_history
WHERE id_type = 'CIK' AND id_value = '0000093410'
ORDER BY valid_from, available_at, source_loaded_at, security_id;

SELECT cik, ticker, title, security_id, source_loaded_at
FROM sec_company_tickers
WHERE cik = '0000093410';

SELECT security_id, entity_id, primary_symbol, issuer_id, first_seen_date,
       source, source_loaded_at
FROM securities
WHERE primary_symbol = 'CVX'
   OR security_id IN (
       SELECT security_id
       FROM security_identifier_history
       WHERE id_type = 'CIK' AND id_value = '0000093410'
   );
```

For each CVX EPS fact, compare its `available_at` to the CIK-history rows with
the three predicates above. A CIK history row pointing to the market ID but
too late in `available_at`, or starting after the fact date, correctly leaves
that fact unresolved. A history row satisfying all predicates that still
leaves the fact at the archive prefix would establish a focused resolver/data
inconsistency; this static audit does not assert that outcome.
