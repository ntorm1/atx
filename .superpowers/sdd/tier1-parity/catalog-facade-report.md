# Concept catalog facade integration

After AR4 review-fix commit `29908a7c`, integrated the exact controller-approved facade patch from `catalog-memory-report.md`. The bounded helper already exists in commit `8a95a3ec`.

`fundamentals.refresh_xbrl_concept_catalog` now imports and returns `xbrl_catalog.refresh_concept_catalog(store)`. The old catalog-only `_statement_category` and `_json_values` functions, whole-facts DataFrame fetch, and pandas aggregation were removed from the facade module. Existing public exports and activation imports remain unchanged; no jobs, activation, migration, registry, or API-export edit was made.

Validation:

- New pure facade seam: `python -m pytest atx-db/tests/test_companyfacts_resilience.py::test_catalog_facade_delegates_without_reading_all_facts -o addopts= -n 0 -q` ? 1 passed in 1.24s. The store sentinel exposes no SQL/DataFrame API, so a direct scan in the facade would fail; the test verifies exact store forwarding and returned aggregate count.
- Ruff on `fundamentals.py` and `test_companyfacts_resilience.py`: clean.
- Strict mypy with silent dependency following on `fundamentals.py`: clean.
- `git diff --check`: clean. No remaining consumers import either removed private helper.
- The helper's 16 focused tests and live read-only aggregation evidence are recorded in `catalog-memory-report.md`; they were deliberately not repeated. No live refresh, write, or schema bootstrap was performed for this integration.

The controller's serial DB test slot was released after the AR4 fixture run and this pure seam check. A first independent helper/facade review and Critical C1 rereview remain controller-owned prerequisites; this report does not infer acceptance. The `fundamentals.py` edit lock is released when this facade commit lands.
