# Fundamental desk screen acceptance

The desk question is which US listed common equities, visible at the 2026-09-20
snapshot and 22:00 UTC information cutoff, have positive reported quarterly
diluted EPS year-over-year growth and positive year-over-year change in trailing
operating margin. For each qualified name, the readout also exposes earnings
yield, operating cash-flow to enterprise-value yield, total accruals, and net
debt to trailing EBITDA. The [read-only SQL](../sql/research/fundamental-desk-screen-acceptance.sql)
is prepared for execution after full-universe materialization. It has **not
been executed** as part of this artifact; no current coverage, passing names,
missing counts, or forward-return results are asserted here.

The query pins the `us_listed_v1` membership source, daily market-panel source,
declarative metric source, all four formula expressions and ordered input
lists, metric-definition version `1`, snapshot date, and
cutoff. It reports the maximum observed eligible `trade_date` separately from
the calendar snapshot date; no 2026-09-20 trading session is assumed. The
four accounting codes and windows are pinned to their registry entries:
`eps_diluted_q_growth_yoy/q`, `operating_margin_change_yoy/ttm`,
`total_accruals/ttm`, and `net_debt_ebitda/ttm`. The market fields are the
materialized `earnings_yield`, `cfo_to_ev`, and `market_cap` from the same
observed session. The metric state includes its `definition_hash`,
`inputs_hash`, availability, origin, fiscal span, run ID, and state ID.
The SELECT computes the publisher's expected SHA-256 definition fingerprint
from frozen seed literals and checks each *selected* state against it after
ranking. A mismatched current state cannot expose an older matching state.
The registry rows must also match the frozen expressions and inputs. These
fingerprints are calculated by the SQL when it runs; their current values have
not been measured or verified in the warehouse here.

One aggregate row is always emitted. Its `status` names an empty universe,
overlapping membership, a metric definition failure, a missing market session,
an EPS owner mismatch, or `measured`. The JSON gives visible listed and
common member counts, usable field counts, complete rows, invalid current
states, stale operand periods, and failures of the dated CIK join. It also
separates missing accounting states and counts each invalid or absent market
field independently, even when another field on the same row fails first.
`measured` means the diagnostic
ran, not that production acceptance passed. A missing eligible universe is a
named failure even though the aggregate row remains present.

The name preview is capped at 300 rows each for passing names, complete names
excluded by the descriptive improvement test, and incomplete names. Up to 99
additional rows show overlapping membership quarantines (999 name rows plus
the mandatory aggregate row, 1000 total). It is
ordered deterministically by disposition, descending market cap, and security
ID. A security with two visible membership intervals is excluded before
fundamental or market joins, even if one interval says `common`. The aggregate
counts all overlapping securities and common candidates separately. Preview
rows contain individual field statuses and full source
state structs. The display ticker comes only from a dated, visible
`exchange_listings` row for the same security. Membership's stored CIK and
symbol are retained as lineage but never used as an inferred current issuer
join. Issuer qualification requires exactly one dated `CIK` identifier
history value visible by the cutoff; absent or ambiguous links remain in the
diagnostic and cannot form a complete answer. The query does not backdate a
current `sec_company_tickers` association. The EPS owner check ranks *all*
visible quarterly or instant standardized EPS input rows in the selected
derived bucket using the publisher's period, availability, source, rule,
basis, and state-ID order. Only then does it compare the winning row's CIK,
security, period, and basis with the dated link and derived fiscal period.
Mismatches and missing input states have separate counts. The winning row's
accession and filing date are reported. The derived `inputs_hash` is opaque;
the SQL carries it as lineage and does not claim to invert it to a source ID.

For each metric, the latest eligible *whole state* is selected by fiscal
period end, availability, and stable state ID before checking its value.
`NULL`, conflict, and other invalidation states therefore cannot reveal an
older valid value. `history_status='event_reconstructed'` is required because
legacy latest-only rows do not certify historical state. The EPS growth row
must have quarterly origin. Freshness uses each selected metric's
`fiscal_period_end`, including an annual fallback's actual operand end; a
missing operand end is unavailable. Name rows report both target-quarter and
operand ages. Operand periods older than 200 calendar days are reported as
stale; this is an operational freshness gate, not an alpha
threshold. Values are not filled with zero. Positive EPS growth and positive
margin change are only a descriptive screen. No return prediction, optimized
threshold, or backtest claim follows from this acceptance readout.

Execution should capture the SQL file hash, aggregate JSON row, and bounded
preview as evidence after the guarded writer releases the database. If
`missing_security_joins` dominates, repair dated identity evidence first;
if metric states are absent or uncertified, repair the generic accounting
source or derived publication path and rerun the same frozen query. The
existing eight price/liquidity hypotheses and their planned 21-session
decile evaluation remain the first statistical evaluation.
