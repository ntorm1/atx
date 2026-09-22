# AP1 fundamentals publication capacity draft

2026-09-20. **Draft only; no runtime verification or full-universe capacity claim.**
Archive4 retained the sole runtime slot. No live `atx-db` file was edited, and no
Python/import, test/collection, Ruff/mypy, database/probe, network, install,
production command, git mutation, or subagent was used. Work was static source
reading, draft editing, and path-specific text comparison.

## Integration artifacts and ownership

- Complete root-relative patch:
  `.superpowers/sdd/tier1-parity/fundamentals-publication-draft/integration.patch`.
- Proposed files: `fundamentals-publication-draft/atx-db/`, relative to this report.
- Original content snapshots: `fundamentals-publication-draft/base/`, normalized
  to LF solely to avoid EOL-only patch noise.
- `fundamentals-publication-draft/baseline-manifest.json` records original live
  SHA-256 values and normalized live/baseline comparisons. All eight copied
  existing paths still matched the live content during final assembly.

The patch changes exactly these existing paths:

```text
atx-db/src/atx_db/_bulk_publication.py
atx-db/src/atx_db/_standardization_set_based.py
atx-db/src/atx_db/calendarization.py
atx-db/src/atx_db/fundamental_statements.py
atx-db/src/atx_db/fundamentals.py
atx-db/src/atx_db/migrations/__init__.py
atx-db/src/atx_db/migrations/registry.py
atx-db/src/atx_db/schema.py
```

New integration paths:

```text
atx-db/src/atx_db/_fundamental_publication.py
atx-db/src/atx_db/migrations/bodies_0318.py
atx-db/tests/test_fundamental_publication.py
```

0318 is reserved only in this draft. Apply after FP1/0317. The registry/facade
hunks add only AP1's 0318 import, MIGRATIONS expansion, and facade cleanup line;
place these after 0317 during integration. Do not overwrite FP1's registry/facade
copies with this draft's full files. The patch intentionally contains no 0317,
forward/delisting, derived-metrics, activation, job, or connection changes.
Root owns exact module-boundary and schema/version pins after combined integration
(`_fundamental_publication` and `migrations.bodies_0318` are new modules).

## Physical build and publication

All eight outputs retain their existing calculation SQL, scopes, input revisions,
PIT/lineage/clocks, options, return values, and materialized-result policy:

| Output | Adapter change |
| --- | --- |
| `fundamental_fact_revisions` | Existing revision SQL inserts into a persistent unindexed stage; concept scope preserves other rows. |
| `fundamental_statement_points` | Existing mapped SQL and full-refresh REIT additions both read/write the stage. Selected concepts retain the previous REIT behavior. |
| `fundamental_periods` | Existing global period projection writes its stage. |
| `fundamental_ttm_points` | Existing PIT/quarter reconstruction writes its stage; canonical-metric scope is unchanged. |
| `fundamental_calendar_map` | Existing SQL FYR snapshot and 2,048-row Python batches remain; the completed projection enters its persistent stage. |
| `fundamental_calendar_ttm` | Existing calendar TTM SQL writes its stage; other sources are retained. |
| `fundamental_standardized` | Existing candidate/output calculation remains; completed rows enter its persistent stage. |
| `fundamental_standardization_exception` | Existing exception calculation remains; its stage publishes together with standardized rows. |

The private helper accepts only these eight target names. It copies retained rows
using `(original replacement predicate) IS NOT TRUE`, preserving NULL predicate
semantics and every original out-of-scope physical value, including storage clocks.
Stages retain column types, defaults, and NOT NULL, with no ART keys. Calculation
and retained-row assembly remain one unindexed stage transaction, so multi-insert
default clocks retain transaction semantics. Public tables are untouched.

Before shadow insertion, the helper checks nonnull unique required IDs, counts the
complete stage, and performs **one spillable `CREATE TABLE AS ... ORDER BY key`**.
The sorted table replaces the unsorted private stage. The existing 256 contiguous
prefix intervals bound each keyset boundary query; first and last intervals are
open-ended so non-hash IDs cannot disappear. Within each interval, at most 50,000
unique keys enter one independently committed, ordered insert. Consequently a
skewed prefix cannot enlarge a shadow transaction beyond the row bound. Prefix
predicates permit ordered row-group min/max pruning rather than repeatedly
sorting/scanning an unordered complete universe. Actual warehouse scan cost and
spill behavior still require the root runtime slot.

The shadow DDL comes from the live table's DuckDB catalog DDL. This preserves
physical column order, migrated appended columns, defaults, NOT NULL, PRIMARY KEY,
UNIQUE, and CHECK constraints; it does not rely on CTAS to create public tables.
The existing `_validate_shadow_contract` compares physical columns and required
logical keys before insertion/publication, and constraints enforce shadow rows.
Final shadow, staged, and copied counts must agree before any public rename.
Public table descriptions are restored inside the swap transaction.

Configured persistent stores checkpoint/reopen before indexed insertion, every
four completed inserts, and after each validated shadow. Thus one connection's
incremental shadow insertion lifetime is at most 200,000 rows. Reopen uses the
existing recorded analytical configuration, without changing resource settings.
Unconfigured and in-memory stores retain their connection, arbitrary session
settings, temporary relations, and database; bounded index lifetime is promised
only for the configured persistent production path, as directed by root.

`publish_validated_shadows` is a narrow addition to `_bulk_publication`. The
existing `publish_validated_shadow` signature and complete function body are
text-identical to the baseline. The new function validates all coupled shadows,
runs their completion callback, renames both tables, and drops both preceding
tables within one transaction. It explicitly attempts rollback on a COMMIT
exception because `DuckDBStore.transaction()` does not do so in that branch.

Standardized rows, exceptions, and the existing completed build-ledger update
remain atomic together. A failure leaves both prior outputs and their views in
place; the existing failed-build handler runs afterward. Routing/seeding already
occurred before statement publication and still does. Calendar coverage already
occurred after calendar publication and still does. No other side effect was
moved across an existing atomic boundary.

## Index migration, ownership, failure, and compatibility

0318 drops exactly the 27 optional secondary index names in the assigned audit,
and refuses a matching name attached to another table or marked UNIQUE/PRIMARY.
All required keys remain. The 13 current bootstrap declarations for revisions,
statement points, periods, and TTM are removed from `schema.py`; the other 14
definitions exist only in historical migration bodies, which remain untouched.
Runtime publication refuses residual secondary indexes instead of dropping them.

Reserved persistent artifacts are `<target>_bulk_stage`, `<target>_bulk_ordered`,
and `<target>_bulk_next`. They carry comment marker
`atx_db._fundamental_publication:v1`; creation and marking commit together.
Every reserved name is checked before retry cleanup. Unmarked tables, any views,
and occupied `<target>_bulk_previous` names cause refusal. Cleanup touches only
marked private artifacts, never canonical tables or an unmarked caller table.
Invalidated connections may reject cleanup; the original error propagates and
marked leftovers are eligible for cleanup on an explicit retry. No automatic
workload retry or connection recovery is introduced.

Configured persistent callers with unrelated temporary tables/registered views
now receive a fail-before-build error, with those relations retained. This is an
explicit compatibility precondition, accepted by root, because arbitrary caller
temporary state cannot survive the required connection recycling. In-memory and
unconfigured callers retain their session and remain supported. Owned concept
filters are released only after complete stage assembly; standardization inputs
are dropped after persistent output assembly and before recycling. Its repeated
cleanup is explicitly qualified to `temp.main`, protecting a persistent caller
table hidden by a standardization temporary name. Calendar temporary input/output
tables are dropped before the helper can recycle; registered pandas batches are
released by the existing `insert_frame` finally block.

No arithmetic, complete-universe, public physical-key, or existing global/coupled
atomicity contract is intentionally weakened. Scoped publication now copies the
complete retained universe into a replacement, so it has extra disk/SQL work.
Disk overlap includes live + unsorted stage + ordered stage during sorting, then
live + ordered stage + constrained shadow during construction, plus spill/WAL.
The standardized pair retains both complete output stages/shadows until both can
publish. This needs a root disk/headroom measurement; no estimate is claimed.

## Focused verification for root, not executed here

New file `tests/test_fundamental_publication.py` has 25 intended parameterized
cases, uncollected. It covers all eight physical targets, original out-of-scope
rows/clocks, PK/UNIQUE/CHECK/defaults, views, file-backed forced-small prefix/keyset
and reopen boundaries, recorded resource replay, caller-state refusal/retention,
owned-name collision refusal, failure before/through shadow build/publication,
both coupled swaps and completed-ledger rollback, a COMMIT-failure seam, migration
and bootstrap policy, and successful standardized scope/materialization behavior.

The all-eight pipeline fixture uses four revenue quarters and an unmapped tag,
checks exact 1,000 TTM values and an exception, and compares repeated payloads
across real file-backed reopens. Existing arithmetic/PIT/REIT fixtures below remain
the substantive semantic oracles; the new test does not replace those suites.

From `atx-db/`, under root's sole guarded runtime slot, run with `-n 0`:

```text
tests/test_fundamental_publication.py
tests/test_bulk_publication.py
tests/test_fundamental_concept_dictionary.py::test_scoped_revision_and_statement_refresh_preserves_other_concepts
tests/test_fundamental_concept_dictionary.py::test_duration_fact_without_period_start_is_excluded
tests/test_industry_templates.py::test_reit_ffo_affo_derives_from_us_gaap_inputs
tests/test_industry_templates.py::test_reported_reit_ffo_wins_over_derivation
tests/test_fundamental_period_dates.py::test_refresh_fundamental_periods_infers_four_date_model
tests/test_calendarization_bounded.py
tests/test_calendarization.py::test_calendar_aligned_ttm_emits_shared_calendar_quarter_for_offset_fye
tests/test_calendarization.py::test_quarterly_ttm_stitch_tags_annual_minus_9mo_path
tests/test_calendarization.py::test_ttm_restatement_uses_latest_visible_derived_quarter_and_anchor_time
tests/test_calendarization.py::test_ttm_refresh_can_scope_delete_and_scan_to_one_metric
tests/test_standardization.py::test_refresh_routes_custom_extension_and_records_unmapped_exception
tests/test_standardization.py::test_set_based_refresh_retains_revisions_classifies_quarters_and_writes_manifest
tests/test_standardization.py::test_refresh_derives_pit_discrete_q4_and_defers_to_reported_quarter
tests/test_standardization_composition_pit.py
```

Then run touched-path Ruff, strict mypy for the new helper/migration/shared bulk
module, and root's combined module/schema/migration contracts after pin updates.
Perform the one independent review specified by root. These are requested checks,
not receipts of execution.

Static comparison confirmed the adapted arithmetic SQL bodies are unchanged
apart from output relation references and REIT stage references; the one-table
publication body is unchanged. The scoped no-index diff check produced no
whitespace findings. Small fixtures cannot certify the 1 GB/one-thread/3 GiB
full-universe peak, indexed COMMIT behavior, scan pruning, disk demand, or run5
completion. Those remain measured production work after archive4 releases the
runtime slot; neither a fundamentals OOM nor successful capacity run is claimed.
