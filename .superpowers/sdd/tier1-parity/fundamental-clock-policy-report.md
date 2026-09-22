# FC1 conservative effective filing clock — static draft handoff

2026-09-21 UTC. **Ready for the one requested static review and root verification.**
No live source file was changed by this implementer. No Python/import, test or
collection, Ruff/mypy, DB connection/probe, network, install, commit, process
control, or subagent was used. Graph tools were unavailable; discovery used static
source reads and `rg`. The only execution was shell-based file reading/draft
writing and static Git text comparisons/check-only patch validation.

## Integration artifacts

- `.superpowers/sdd/tier1-parity/fundamental-clock-draft/integration.patch`
- Proposed paths under `fundamental-clock-draft/atx-db/`.
- `fundamental-clock-draft/base/atx-db/`: LF-normalized baseline snapshots.
- `fundamental-clock-draft/baseline-manifest.json`: origin and normalized baseline
  SHA-256 values; all nine existing files matched current live normalized content
  at final assembly. `fundamentals.py` originated from the **reviewed AP1 draft**
  and now matches the AP1-integrated live source.
- `fundamental-clock-draft/root-integration.md`: root-owned 0319 registry/facade
  insertion instructions, activation sequencing and legacy-input prerequisite.

The patch has nine existing paths: `fundamentals.py`, `asof/fundamentals.py`,
`features.py`, `quality/checks_market_reference.py`, `activation.py`,
`fundamental_xbrl_metrics.py`, `expected_growth.py`, `estimates/measure_actuals.py`,
and the estimates facade's clock documentation. Four added paths are
`_fundamental_clock.py`, `migrations/bodies_0319.py`,
`tests/test_fundamental_clock.py`, and `docs/FUNDAMENTAL_CLOCK_POLICY.md`.

The integration patch contains only FC1 hunks. It does not include AP1 physical
publication changes, replace live files with snapshots, edit jobs or migration
registries, or alter FP1/DP1/MP1. Root must insert only the three owned 0319 lines
after 0318 and update combined module/schema/version pins. `git apply --check`
with the repository's normal EOL configuration passed. Static no-index whitespace
comparison had no findings. No patch was applied to live source.

## Clock policy and bounded implementation

`sec_filed_date_plus_46h_v1` projects SEC companyfacts availability to
`greatest(stored available_at, filed_date + INTERVAL 46 HOUR)`; raw points use
their loader-copied filing date in `as_of_date`. This is an inline relation, with
no new fact join, source download, raw UPDATE, pandas universe materialization,
memory-budget change, or scope/metric reduction. The existing revision base reads
the projection before hashing and ordering, so P1/AF1 and all downstream arithmetic
receive corrected input clocks through their existing event machinery. Accession
IDs that never included a clock remain stable by design.

Only exact loader source `SEC companyfacts` receives this policy. Other sources
retain their timestamps and existing NULL behavior. Null, malformed or non-finite
SEC filing dates are unresolved and excluded from effective readers while raw
rows remain intact. A valid filing date with a NULL stored timestamp receives the
date-policy floor, not an invented exact acceptance time. A later stored timestamp
is never pulled earlier. Typed source DATE columns already reject most malformed
values; the fixture exercises malformed strings through the same SQL projection.

No submissions acceptance timestamp or RDQ accelerates eligibility. RDQ selection
and P1/AF1 value arithmetic are unchanged. The policy addresses the demonstrated
ordinary winter cutoff error; it is not measured acceptance, first SEC/API arrival,
historical local delivery, or vintage certification. Filing-date correction,
unbounded arrival delay, original identity resolution, source completeness and
observation-vintage limitations remain explicit.

## Consumer closure and migration choice

The shared projection now covers:

- Revision reconstruction before sequencing and downstream IDs/hashes.
- The advertised raw `fundamentals_asof` reader, including its returned clock.
- All four raw-point reads in SEC fundamental feature construction; feature
  definitions and build-manifest params record the version.
- Both detail/count predicates of the overlap-related quality check.
- The existing `v_price_fundamental_overlap` view through 0319, with exactly the
  prior output columns, grouping, membership and aggregation semantics.
- Both Company Facts candidate branches in the XBRL metric extractor, SEC debt
  inputs in expected growth, and primary/preference reads in estimate actuals.
  These are relation substitutions; their arithmetic and inline-XBRL clocks stay
  unchanged.

0319 is justified by the persisted existing-view definition and inspectable
catalog policy. It appends notes rather than replacing P1/AF1 descriptions and
clarifies raw provenance fields; it adds no physical columns, public view, API,
table, or data backfill. Metadata explicitly says existing materializations need
rebuilding. Raw tables, raw/latest exports and watermarks remain provenance
surfaces, not effective as-of guarantees. CF5 receipts/fingerprints and the raw
loader, identifier-resolution and raw-publication code are untouched.

## Activation, shares, and measured prerequisite

The minimal root-owned `stage_statement_points` hunk refreshes all SEC share
history after corrected statements and records `shares_history_rows` plus the
clock policy. It retains the existing source-wide idempotent replacement. There
is no new ladder stage or scheduler entry. A shares failure fails the statement
stage and allows its normal retry. This writer uses a whole-table indexed
transaction; its production peak is **unmeasured**. Root will measure it under
the current budgets and consider a separate bounded-publication task only on
actual failure; FC1 makes no shares physical-design change.

Share-history consumers inventoried: `asof/fundamentals.py`, `market_daily.py`
(DEI), `derived_compatibility.py` (shares/market inputs for projections),
`valuation_multiples.py` (market cap and valuations), `short_interest_metrics.py`,
`fundamental_signals.py`, `cash_profitability.py`, `earnings_surprise.py`, and
`revenue_surprise.py`. Lake exports and releases also need their normal
republication. This inventory does not claim every legacy consumer preserves
all historical revision states; that separate question is outside FC1.

Run5 must rebuild from `statement_points` through periods, TTM, calendarization,
standardization, derived metrics, market daily and subsequent projections/exports;
old successful stage receipts must not skip the new policy. Existing XBRL metrics
are a special prerequisite because standardization UNIONs them: legacy rows mix
inline/Company Facts origins and discard the original filed date. The existing
XBRL builder collects candidates in pandas, so invoking it blindly over the full
universe would violate this task's capacity limits.

Root explicitly chose an inventory/empty-input prerequisite rather than a new
physical policy column or activation gate. Root's guarded **read-only** receipt
`fundamental-clock-legacy-input-inventory.json`, observed at
**2026-09-21 00:50:22.238294 UTC**, reports zero rows for
`fundamental_xbrl_metric`, `shares_outstanding_history`, `est_actual`, revisions,
statement points, standardized facts and market daily. Thus the legacy XBRL
empty-input prerequisite was satisfied in that warehouse at that time. Root
reported a 0.099 GiB peak; the receipt is `fundamental-clock-inventory-memory.json`.
These measurements belong to root, not this implementer. The JSON's separate
`derived_metrics: "not present"` entry is not a count of `derived_metric_values`,
and no such count is inferred here.

Expected growth and estimate actuals are outside the core activation ladder;
future builds now read effective clocks, while any preexisting dependent outputs
still require their normal refresh. If another writer populates legacy XBRL rows
before run5, root must satisfy the prerequisite again with a concrete bounded
refresh. There is no blanket legacy-materialization closure claim.

## Focused root verification — not executed here

The new module has 13 intended parameterized cases, uncollected. It verifies the
constructed 22:15 winter acceptance case at both cutoff sides, prior TTM420
versus new460 and daily 4600/420 versus10, Friday/Saturday eligibility with the
next Monday decision, a descriptive earlier RDQ, corrected shares activation,
raw public reader agreement, later stored timestamps, summer/null/invalid-date
policy, non-SEC preservation, changed revision ordering, scoped deterministic
revision/statement IDs, unchanged raw rows, feature metadata and clocks, overlap
view/quality agreement, idempotent metadata and future XBRL/estimate reads.

From `atx-db/`, under root's sole guarded runtime slot and `-n 0`, run:

```text
tests/test_fundamental_clock.py
tests/test_derived_pit_revisions.py::test_exact_amendment_dependency_export_daily_and_projection
tests/test_derived_pit_revisions.py::test_downstream_growth_rebuild_includes_changed_lagged_ttm
tests/test_derived_annual.py::test_annual_only_values_growth_api_and_daily
tests/test_derived_annual.py::test_precedence_switches_and_null_never_resurrects
tests/test_derived_annual.py::test_actual_standardizer_fy_minus_nine_month_precedence
tests/test_fundamental_publication.py::test_all_eight_sql_writers_cross_file_reopens_with_unchanged_payload
tests/test_fundamental_concept_dictionary.py::test_scoped_revision_and_statement_refresh_preserves_other_concepts
tests/test_fundamentals_spine_link.py::test_unresolved_archive_fact_cannot_join_current_sec_identity_or_bars
tests/test_fundamental_xbrl_metrics.py::TestCompanyFactsCandidatePath
tests/test_estimates.py::TestActualsMapping
tests/test_expected_growth.py
tests/test_shares_outstanding_history.py::test_shares_outstanding_asof_filters_by_availability_and_type
```

Existing P1/AF1 arithmetic assertions are not edited. Intentional clock expectation
changes apply only when constructing from `source='SEC companyfacts'`: the legacy
filed-day22 timestamp becomes at least next-day22 downstream; raw stamps remain
filed-day22. Existing arithmetic tests that seed already-derived inputs keep their
clocks; XBRL fixture source `sec_companyfacts` and estimates source `test` are
non-SEC test sources and also retain their old timestamps. Do not globally shift
fixture dates or raw-ingestion expectations to make a failure pass.

Then root should run touched-path Ruff, strict mypy for the new helper/migration
and directly changed typed modules, plus the combined module/schema/migration
contracts after its pins/registry edits. One fresh static review follows this
handoff; Important fixes are accepted on report and re-review is Critical only.
Small fixtures cannot certify full-universe memory/disk demand, input timing
prevalence or a completed run5. Those remain production work owned by root.
