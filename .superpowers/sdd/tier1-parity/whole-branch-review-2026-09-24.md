# Whole-branch static review — 2026-09-24

**Review result:** one Important and one Moderate finding. This is a static review of candidate `727e6b90d8e1e942ca1f91428655924d039950cd` against merge base `938ff7f178f21c2150ec83ca50cfee1e6542ed9a`. Current main was `9562d3155bebac6585b505f686b2710cfc944154`; no merge was reviewed or authorized. The review covered all 50 changed `atx-db` paths and pertinent unchanged release and activation interfaces. The four unrelated tracked working-tree EOL changes were excluded by reading the committed candidate.

## Findings

### Important — FQ2 label evidence omits an economic validity field

`atx-db/src/atx_db/fundamental_signal_evaluation.py:368-375` computes each selected-label SHA from `_fq2_join` without `is_stitched`. The join's validity rule at lines 197 and 212 uses that field: a delisted label without stitching and a non-delisted label with stitching are invalid. If `is_stitched` on a selected source row changes after a completed evaluation, the selected-label and sample digests can remain equal while reevaluation changes label validity, labeled counts, and possibly decile returns. This weakens the stored “exact selected economic fields” evidence described by migration 0325 and the evaluation document; it does not imply that a read-side verifier currently runs.

Carry `is_stitched` through `_fq2_join` and the versioned selected-label digest. Audit the digest projection against every source field used by the label-status CASE, then add a focused fixture showing a stitching-field change alters the evidence digest. Existing completed runs would need an explicit compatibility policy if the digest definition changes.

### Moderate — empty issuer as-of result loses its column contract

`atx-db/src/atx_db/asof/fundamentals.py:532-533,568-569` initializes `result_columns` empty and fills it only after a nonempty derived page. An issuer may have a visible source-owner mapping but no `derived_metric_values` visible by `content_as_of`. The first query then has zero rows, the loop exits, and `issuer_derived_asof` returns a DataFrame with no columns. Before this branch, DuckDB's `.df()` preserved the selected columns even for zero rows. A caller accessing `result["derived_value_id"]` now raises `KeyError` instead of receiving an empty typed series. Obtain the query's column names from its cursor even when the first page is empty, and cover the owner-present/derived-absent case.

## Coverage matrix

| Area | Changed paths reviewed | Static disposition |
| --- | --- | --- |
| DL1 publishing, DSL, lineage, and issuer readers | `src/atx_db/_derived_annual.py`, `_derived_pit.py`, `derived_dsl.py`, `derived_lineage.py`, `derived_metrics.py`, `api/catalog.py`, `api/service.py`, `asof/fundamentals.py` | One Moderate as-of finding above. Checked whole-revision selection, selected source CIK leaves, dependency and clock proof, legacy NULL rejection, page/byte bounds, and issuer owner versus market identity. |
| FQ1/FQ2 panel, labels, evaluation, desk | `src/atx_db/_forward_return_publication.py`, `fundamental_signal_research.py`, `fundamental_signal_evaluation.py` | One Important digest finding above. Checked observed decision/entry sessions, current-root freshness, prior comparison leaves, owner ambiguity, latest label revision, basis/version recording, split and decile ordering, terminal policy checks, and validated desk input eligibility. |
| Migrations and integration | `src/atx_db/migrations/__init__.py`, `bodies_0323.py`, `bodies_0324.py`, `bodies_0325.py`, `registry.py` | 0323-0325 registration, added columns, tables, catalog/schema pins, and nullable legacy label provenance checked statically. Live migration/apply and full schema verification remain open. |
| Recovery and operator status | `src/atx_db/dataset.py`, `pipeline_status.py`, `cli.py` | Failure-ledger retry preserves the original exception and uses the recorded connection budget. Pipeline status reads bounded ledgers, keeps process liveness unknown and production readiness unassessed. No additional concrete finding. |
| Scripts and prepared SQL | `scripts/evaluate_fundamental_signals.py`, `generate_data_dictionary.py`, `measure_tier1_readiness.py`, `read_fundamental_desk_screen.py`, `research_fundamental_signals.py`, `sql/research/fundamental-desk-screen-acceptance.sql`, `fundamental-signal-decile-acceptance.sql` | Checked bounded read paths, selected-run linkage, report wording, and prepared SQL against producer grains. The readiness inventory presents recorded evidence without digest validation or certification. |
| Documentation and measurement artifact | `README.md`, `docs/DATA_DICTIONARY.md`, `FUNDAMENTAL_DESK_SCREEN_ACCEPTANCE.md`, `FUNDAMENTAL_SIGNAL_EVALUATION.md`, `FUNDAMENTAL_SIGNAL_READINESS.md`, `FUNDAMENTAL_SIGNAL_RESEARCH.md`, `ISSUER_CONTENT_QUERY.md`, `PIPELINE_STATUS.md`, `PRODUCTION_RUNBOOK.md`, `TIER1_ACTIVATION_STATUS.md`, `TIER1_INTERIM_READINESS_2026-09-24.md`, `docs/measurements/tier1-interim-2026-09-24.json` | Checked contract descriptions and aggregate status against current program rulings. The interim inventory explicitly reports missing downstream outputs and unmeasured release gates. The Important digest finding requires corresponding evaluation text to remain precise. |
| Test and snapshot changes | `tests/conftest.py`, `data/public_api_snapshot.json`, `test_dataset_failure_recovery.py`, `test_derived_selected_lineage.py`, `test_forward_return_publication.py`, `test_fundamental_desk_panel.py`, `test_fundamental_signal_evaluation.py`, `test_fundamental_signal_readiness.py`, `test_fundamental_signal_research.py`, `test_issuer_content_query.py`, `test_issuer_selected_lineage.py`, `test_pipeline_status.py` | Reviewed focused coverage and schema-bootstrap resource settings. The two findings need focused regression cases; existing reported focused passes are not whole-branch runtime proof. |

The unchanged `publication.py` release path dynamically exports physical columns and pins a physical schema hash separately from the public record schema hash; migration 0323's additional fields therefore do not silently reuse an old physical manifest. Issuer API and desk documentation distinguish accounting owner IDs from dated tradable market securities. Existing source and historical identity limitations remain open requirements, not new findings from this branch.

## Verification limits and next gates

### Root repair disposition

The Important label-evidence finding is repaired in `09fed606`; the Moderate
empty-schema finding is repaired in `9e0b4ffa`. The repair reports are
`fq2-label-evidence-fix-report.md` and `iq2-empty-schema-fix-report.md`.
All 17 tests in the two affected files passed together under the 1.5 GiB guard,
measured peak 0.630894 GiB; scoped Ruff passed at 0.074970 GiB. Receipts are
`whole-branch-fixes-tests1-memory.json` and `whole-branch-fixes-ruff1-memory.json`.
Root accepted the fixes on the implementer reports and focused proof under
the one-pass ruling. No Critical finding requires another review. This is
repair acceptance after the static review, not a claim that the reviewer
reviewed a different candidate or executed any tests.

No Python, tests, imports, database queries, network calls, live writes, source edits, commits, or merge were performed in this review. The prior task reviews and focused test receipts were read as existing evidence, not rerun. Static inspection cannot certify the new migrations, numeric behavior, live source completeness, research results, or release eligibility.

After fixes, retain this reviewed candidate SHA and record each changed path and repair SHA. The program's one-pass rule accepts Important repairs with the implementer's report and focused evidence; a genuinely necessary Critical repair needs explicit follow-up review. Full schema/numeric verification, full non-slow suite, complete source/downstream build, measured annual-item/provider/quality and research gates, first release, and explicit user permission before main merge remain open.
