# Tier-1 Sprint 2 — Standardized Item Breadth, Tag Depth, and Industry Templates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Take `atx-db` from 46 published canonical items with a median of 1 alias tag per item to a seed-driven registry of 249 items with deep, curated us-gaap alias sets (Wave A core IS/BS/CF, Wave B bank/insurer/REIT/utility overlays), a widened CompanyFacts ingest allowlist, a deterministic alias-mining research tool, a coverage-measurement harness, and an offline fixture corpus — so the S1 activation run can be measured against the spec gate of ≥ 110 items at ≥ 90% coverage on the top-3000 for FY2015+.

**Architecture:** Every registry surface becomes a sorted CSV seed re-read on each build: the 214-row `FUNDAMENTAL_STATEMENT_MAP_ROWS` Python literal moves to `seeds/statement_map.csv` behind a lazily-built compatibility tuple, joining the three CSVs that already exist (`fundamental_items.csv`, `standardization_rules.csv`, `concept_map.csv`). A committed normalizer (`scripts/normalize_fundamental_seeds.py`) rewrites all four in canonical sorted order and regenerates `concept_map.csv` as a pure projection of the statement map, so alias waves are append-then-normalize edits with a sorting guard test. The CompanyFacts ingest allowlist stops being the throttle: `default_companyfacts_concepts()` becomes the union of the statement-map projection and every alias referenced by an active standardization rule. The rule engine gains two fallback combination rules (`coalesce_or_sum`, `coalesce_or_difference`) so derived-if-missing items (gross_profit, ebitda, total_liabilities, common_equity, cash_and_st_investments) can prefer a direct tag and fall back to composition — the current engine allows exactly one active rule per `(item_id, basis)`, so direct-alias and composition were previously mutually exclusive. Nothing in the live warehouse is deleted or re-keyed: every change is additive seed rows plus two new closed-dispatch branches.

**Tech Stack:** Python 3.12, DuckDB (embedded, via `atx_db.connection.DuckDBStore`), pandas + pyarrow (seed loading and set-based materialization), stdlib `csv`/`json`/`hashlib` (all seed I/O is stdlib, no third-party CSV), pytest + pytest-xdist + filelock (fingerprinted schema template in `tests/conftest.py`), numbered migration bodies in `src/atx_db/migrations/bodies_NNNN.py` registered in `migrations/registry.py`.

**Spec:** `C:\atx\docs\superpowers\specs\2026-09-19-tier1-parity-design.md` (canonical item catalog: "Canonical item catalog (target: Compustat-core parity)"). Supporting audit: `C:\atx\.superpowers\sdd\tier1-parity\audit-atx-db.md` §2 (standardized item registry), §5 (universe/coverage), §8 (gap list rows 2, 3, 5), §9 (code-health: the 6,456-line `fundamental_statements.py`).

## Global Constraints

Copied verbatim from the sprint charter. Every task's requirements implicitly include this section.

- Python 3.12 venv at `C:\atx\atx-db\.venv\Scripts\python.exe`.
- Run tests from `C:\atx\atx-db` with `.venv\Scripts\python.exe -m pytest <file> -n 0 -q`.
- No network in tests.
- Seeds CSV are the source of truth and are re-seeded per build.
- Alias rows must be sorted deterministically in the CSV (by item, priority, alias) and a test enforces sorting.
- Migration numbers start at 0301 for this sprint (S1 uses 0300) — check registry conventions.
- Commit per task `feat(db): ...` with trailing `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Keep every task ≤ ~400 lines new code except seed CSV rows.
- Never delete existing items or rules (only add or reprioritize).
- A rule change must not change the output of existing standardization tests except where the test is explicitly updated in the same task with justification.

### Repo conventions this plan depends on (verified, not assumed)

- Migration registry convention: one file `src/atx_db/migrations/bodies_NNNN.py` exporting `MIGRATIONS = [Migration(version=NNN, name="...", up=_fn)]`, imported and splatted in `src/atx_db/migrations/registry.py`. Highest existing version is `299` (`bodies_0299.py`); `0297` is a gap; `0300` is reserved for Sprint 1. This sprint uses `0301` only.
- `tests/conftest.py::_schema_fingerprint()` already hashes `*(_SOURCE_ROOT / "atx_db" / "seeds").glob("*.csv")`, so a new `seeds/statement_map.csv` is picked up automatically — Task 1 asserts this rather than editing conftest.
- `pyproject.toml` ships `atx_db = ["py.typed", "seeds/*.csv"]`, so new seeds are packaged automatically.
- `tests/data/public_api_snapshot.json` pins `sorted(dir(atx_db))`. Any new `src/atx_db/*.py` module that is imported (directly or transitively) becomes an attribute of the `atx_db` package and MUST be added to the `"atx_db"` list in that JSON in the same task.
- Alias uniqueness is enforced globally by `item_registry._validate_aliases`: one `(alias_scheme, alias_code)` may resolve to exactly one `item_id` for any overlapping validity window. Industry overlays that need the same concept on two items must live only in `seeds/statement_map.csv` (whose primary key is `(source, taxonomy, concept, industry_template)`), never in `seeds/fundamental_items.csv`.
- Rule uniqueness: `standardization.read_standardization_rules` raises on a duplicate **active** `(item_id, basis)` key. One active rule per item per basis. Bases are `annual`, `quarterly`, `ttm`, `instant`.
- Selection order inside the set-based engine is `ORDER BY available_at DESC, input_rank, upstream_priority` (`_standardization_set_based.py:695`). "First matching alias wins" therefore means: among the candidate rows visible at a revision event, the most recently available one wins, and `coalesce_priority` (the alias priority) breaks ties. Alias priority is a tie-break, not an absolute precedence — do not document it otherwise.
- `seeds/concept_map.csv` is not independent: `tests/test_concept_coverage.py::test_concept_map_csv_round_trips_generated_projection` asserts it equals `concept_map_projection_rows()` byte-for-byte. It must be regenerated whenever `statement_map.csv` changes.

### Operator note (not a task)

After Sprint 2 lands, the Sprint 1 activation ladder must be re-run with `--force` from the CompanyFacts stage onward, because the ingest allowlist widens (Task 2) and the statement map gains concepts (Tasks 5–7):

```
.venv\Scripts\python.exe scripts\build_companyfacts_bulk.py --companyfacts-zip <path> --symbol-source sec_company_tickers
.venv\Scripts\python.exe scripts\finalize_companyfacts_bulk.py
.venv\Scripts\python.exe scripts\promote_fundamentals_universe.py --start-stage standardized --force
.venv\Scripts\python.exe scripts\measure_item_coverage.py --write-docs
```

Concepts already loaded are unaffected; the re-run only adds newly-allowlisted concepts. `refresh_fundamental_statement_points` onward must be re-run because `fundamental_statement_points.item_id` is stamped from the statement map at refresh time.

---

## File Structure

**Created**

| Path | Responsibility |
| --- | --- |
| `src/atx_db/statement_map_seed.py` | `FundamentalStatementMapRow` dataclass + stdlib CSV reader/writer for `seeds/statement_map.csv`. Owns nothing else. |
| `src/atx_db/seeds/statement_map.csv` | The 214-row (growing) canonical XBRL concept-to-item statement map. Source of truth. |
| `src/atx_db/alias_mining.py` | Pure, deterministic alias-candidate scoring over CompanyFacts + taxonomy calculation arcs. Never writes rules. |
| `src/atx_db/item_coverage.py` | Per (item, fiscal_year) coverage math + `docs/ITEM_COVERAGE.md` renderer + the spec gate evaluator. |
| `src/atx_db/migrations/bodies_0301.py` | Creates `fundamental_item_coverage`, catalogs it, re-pins the schema contract. |
| `scripts/normalize_fundamental_seeds.py` | Rewrites all four fundamentals seeds in canonical sorted order; regenerates `concept_map.csv` from the statement map. |
| `scripts/mine_concept_aliases.py` | Operator CLI for `alias_mining`; writes `research/alias_candidates.csv`. |
| `scripts/measure_item_coverage.py` | Operator CLI for `item_coverage`; writes `docs/ITEM_COVERAGE.md`. |
| `scripts/make_companyfacts_fixture.py` | One-time network trimmer + offline synthetic generator for `tests/data/companyfacts_fixture/`. |
| `tests/test_statement_map_seed.py` | Seed round-trip, row count, sorting, lazy-attribute, fingerprint coverage. |
| `tests/test_ingest_allowlist.py` | Every active-rule alias is in the ingest set; the set is sorted and deterministic. |
| `tests/test_seed_determinism.py` | Canonical sort + no duplicate (item, alias) across all four seeds. |
| `tests/test_alias_mining.py` | Scoring, ranking, and arc-derived statement placement on a synthetic fixture. |
| `tests/test_alias_depth.py` | Three-or-more aliases per spec item, with an explicit pinned exception table. |
| `tests/test_item_coverage.py` | Coverage math, markdown rendering, gate evaluation. |
| `tests/test_companyfacts_fixture.py` | Fixture concepts are all allowlisted and all route to an item. |
| `tests/data/companyfacts_fixture/*.json` | Nine trimmed/synthetic CompanyFacts documents (300 KB each max). |

**Modified**

| Path | Change |
| --- | --- |
| `src/atx_db/fundamental_statements.py` | Delete the 4,618-line literal; import the dataclass; module `__getattr__` for `FUNDAMENTAL_STATEMENT_MAP_ROWS`; widen `default_companyfacts_concepts()`. |
| `src/atx_db/standardization.py` | Add `coalesce_or_sum` / `coalesce_or_difference` to the closed dispatch. |
| `src/atx_db/_standardization_set_based.py` | Mirror the two fallback rules in the set-based SQL. |
| `src/atx_db/industry_templates.py` | Extend `TEMPLATE_ITEMS` for Wave B. |
| `src/atx_db/migrations/registry.py` | Register 0301. |
| `src/atx_db/seeds/fundamental_items.csv` | Plus 14 items, plus ~160 alias rows. |
| `src/atx_db/seeds/standardization_rules.csv` | Plus 25 rules, alias JSON + composition updates. |
| `src/atx_db/seeds/concept_map.csv` | Regenerated projection. |
| `tests/data/public_api_snapshot.json` | Plus `statement_map_seed`, `alias_mining`, `item_coverage`. |
| `tests/test_item_registry.py` | `AUTHORIZED_ITEM_IDS` and the 235 count (justified per task). |
| `tests/test_standardization.py` | The 455/130/65 rule counts (justified per task). |
| `tests/test_concept_coverage.py` | The allowlist-equals-projection assertions (justified in Task 2). |

## Spec item catalog to registry item_id mapping

91 spec item codes. **77 map to an existing registry item_id; 14 require new items.** Sprint 2 also fills alias sets for 22 further existing registry items that are not in the spec catalog but sit on the same statements (interest_income, current_tax, deferred_tax, equity_in_affiliates, aoci, temporary_equity, change_in_ar, change_in_inventory, change_in_ap, fx_effect_on_cash, divestitures, capex_broader_incl_intangibles, preferred_dividends_paid, common_dividends_paid, cfo_continuing_ops, prepaid_expense, accrued_liabilities, current_portion_of_lt_debt, total_debt, common_stock_at_par, additional_paid_in_capital, treasury_stock_shares), which is how the published count clears the spec gate of 110 items.

### Income statement

| spec item | registry item_id | registry canonical_code | status |
| --- | --- | --- | --- |
| revenue | 1001 | revenue | existing |
| cost_of_revenue | 1003 | cost_of_revenue_cogs | existing |
| gross_profit | 1004 | gross_profit__1004 | existing, gains composition fallback |
| sga_expense | 1005 | sg_and_a | existing |
| rd_expense | 1008 | r_and_d_expense | existing |
| depreciation_amortization | 1011 | d_and_a_income_statement | existing |
| operating_income | 1014 | operating_income | existing |
| ebitda | 1016 | ebitda_standardised | existing, gains composition |
| interest_expense | 1018 | interest_expense_total | existing |
| nonoperating_income | 1021 | non_operating_income_expense | existing |
| special_items | 1022 | special_items | existing |
| pretax_income | 1023 | pretax_income | existing |
| income_tax | 1024 | income_tax_total | existing |
| income_before_extraordinary | 1029 | income_before_extraordinary | existing |
| minority_interest_income | 1027 | minority_interest_p_and_l | existing |
| net_income | 1031 | net_income_total | existing |
| net_income_common | 1032 | net_income_to_common | existing, gains composition fallback |
| discontinued_operations | 1030 | discontinued_ops_ni | existing |
| extraordinary_items | **1051** | extraordinary_items | **new** |
| eps_basic | 1034 | eps_basic__1034 | existing |
| eps_diluted | 1035 | eps_diluted | existing |
| shares_basic_weighted | 1040 | weighted_avg_shares_basic | existing |
| shares_diluted_weighted | 1041 | weighted_avg_shares_diluted | existing |
| dividends_common | 1316 | common_dividends_paid | existing; registry places it on the cash-flow statement |
| dividends_preferred | 1033 | preferred_dividends | existing |
| stock_compensation | 1308 | stock_based_compensation | existing; shared with cf_stock_compensation |

### Balance sheet

| spec item | registry item_id | registry canonical_code | status |
| --- | --- | --- | --- |
| cash_and_equivalents | 1104 | cash_only | existing; the che total is 1103 via composition |
| short_term_investments | 1105 | short_term_investments | existing |
| receivables | 1106 | accounts_receivable | existing |
| inventory | 1107 | inventory | existing |
| other_current_assets | 1109 | other_current_assets | existing |
| total_current_assets | 1102 | current_assets | existing |
| ppe_gross | 1111 | pp_and_e_gross | existing |
| ppe_net | 1110 | pp_and_e_net | existing |
| goodwill | 1114 | goodwill | existing |
| intangibles | 1115 | other_intangibles | existing; the total is 1113 via composition |
| long_term_investments | 1117 | long_term_investments | existing |
| other_assets | 1119 | other_lt_assets | existing |
| total_assets | 1101 | total_assets | existing |
| accounts_payable | 1203 | accounts_payable | existing |
| short_term_debt | 1205 | short_term_debt | existing, zero aliases today |
| taxes_payable | **1225** | taxes_payable | **new** |
| other_current_liabilities | **1226** | other_current_liabilities | **new** |
| total_current_liabilities | 1202 | current_liabilities | existing |
| long_term_debt | 1207 | long_term_debt | existing |
| deferred_taxes | 1211 | deferred_tax_liabilities | existing |
| other_liabilities | 1212 | other_lt_liabilities | existing |
| total_liabilities | 1201 | total_liabilities | existing, gains composition fallback |
| minority_interest | 1213 | minority_interest_bs | existing |
| preferred_stock | 1214 | preferred_stock | existing |
| common_equity | 1220 | common_equity | existing, zero aliases today, gains composition |
| retained_earnings | 1217 | retained_earnings | existing |
| treasury_stock | 1219 | treasury_stock | existing |
| stockholders_equity | 1221 | stockholders_equity | existing |
| shares_outstanding | 1039 | shares_outstanding_period_end | existing |
| accumulated_depreciation | 1112 | accumulated_depreciation | existing |

### Cash flow

| spec item | registry item_id | registry canonical_code | status |
| --- | --- | --- | --- |
| cfo | 1301 | cash_flow_from_operations | existing, gains fallback to 1302 |
| capex | 1305 | capex__1305 | existing |
| acquisitions | 1309 | acquisitions | existing |
| investing_cash_flow | 1303 | cash_flow_from_investing | existing |
| dividends_paid | 1318 | total_dividends_paid | existing, gains composition fallback |
| share_repurchase | 1312 | stock_repurchases_buybacks | existing |
| share_issuance | 1311 | stock_issuance | existing |
| debt_issuance | 1313 | lt_debt_issued | existing |
| debt_reduction | 1314 | lt_debt_repaid | existing |
| financing_cash_flow | 1304 | cash_flow_from_financing | existing |
| change_in_cash | 1324 | net_change_in_cash | existing |
| cf_depreciation | 1307 | d_and_a_cash_flow | existing |
| cf_stock_compensation | 1308 | stock_based_compensation | existing; shared with stock_compensation |
| deferred_tax_cf | **1327** | deferred_tax_cash_flow | **new**; 1326 is in UNAUTHORIZED_GAP_ITEM_IDS |
| working_capital_change | 1322 | change_in_working_capital | existing, zero aliases today, gains composition |

### Supplemental / industry

| spec item | registry item_id | registry canonical_code | status |
| --- | --- | --- | --- |
| net_interest_income | 1501 | net_interest_income | existing, zero aliases today |
| interest_income (bank) | 1503 | interest_income_total | existing |
| interest_expense_bank | 1504 | interest_expense_bank | existing, zero aliases today |
| provision_for_loan_losses | 1505 | provision_for_loan_losses | existing |
| loans_net | 1509 | total_loans | existing |
| deposits | 1510 | total_deposits | existing, vendor-only today |
| allowance_for_loan_losses | 1506 | allowance_for_loan_and_lease_losses | existing, zero aliases today |
| noninterest_income | **1516** | noninterest_income | **new** |
| noninterest_expense | **1517** | noninterest_expense | **new** |
| premiums_earned | 1601 | premiums_earned | existing |
| benefits_and_claims | 1604 | insurance_benefits_paid | existing, vendor-only today |
| policy_reserves | 1603 | loss_reserves | existing, zero aliases today |
| investment_income_insurance | **1611** | investment_income_insurance | **new** |
| rental_revenue | **1713** | rental_revenue | **new** |
| ffo | 1701 | funds_from_operations_ffo | existing, gains composition |
| real_estate_investments_net | **1714** | real_estate_investments_net | **new** |
| operating_lease_liabilities | 1209 | operating_lease_liability | existing, zero aliases today |
| finance_lease_liabilities | **1227** | finance_lease_liabilities | **new** |
| capitalized_software | **1120** | capitalized_software | **new** |
| employees | **1052** | employees | **new, marked not_available.** Neither us-gaap nor dei exposes an employee count as a numeric fact; it appears only in text blocks. No statement-map row, no standardization rule, no alias. |

Charter Wave B also adds two utility items that are not in the spec catalog table but are named in the charter prose ("utilities (regulated revenue, regulatory assets)"): **1806 regulatory_assets** and **1807 regulatory_liabilities**.

**New item id total: 14** - 1051, 1052, 1120, 1225, 1226, 1227, 1327, 1516, 1517, 1611, 1713, 1714, 1806, 1807. Registry goes 235 to 249 items; rules go 455 to 480 (annual 136, quarterly 136, ttm 136, instant 72).

---

### Task 1: Move the statement map out of Python and into `seeds/statement_map.csv`

**Files:**
- Create: `C:\atx\atx-db\src\atx_db\statement_map_seed.py`
- Create: `C:\atx\atx-db\src\atx_db\seeds\statement_map.csv`
- Modify: `C:\atx\atx-db\src\atx_db\fundamental_statements.py` (delete lines 13-34 dataclass, delete lines 176-4794 literal, rewire lines 4962-5090)
- Modify: `C:\atx\atx-db\tests\data\public_api_snapshot.json`
- Test: `C:\atx\atx-db\tests\test_statement_map_seed.py`

**Interfaces:**
- Produces: `atx_db.statement_map_seed.FundamentalStatementMapRow` (frozen dataclass, 19 fields, field order and defaults byte-identical to the current definition in `fundamental_statements.py`), `STATEMENT_MAP_SEED_PATH: Path`, `STATEMENT_MAP_SEED_COLUMNS: tuple[str, ...]`, `read_statement_map_seed(path: Path | str = STATEMENT_MAP_SEED_PATH) -> tuple[FundamentalStatementMapRow, ...]`, `default_statement_map_rows() -> tuple[FundamentalStatementMapRow, ...]` (lru_cached), `write_statement_map_seed(rows: Iterable[FundamentalStatementMapRow], path: Path | str = STATEMENT_MAP_SEED_PATH) -> int`, `statement_map_sort_key(row: FundamentalStatementMapRow) -> tuple[str, str, str, str]`.
- Consumes: nothing from earlier tasks.
- `atx_db.fundamental_statements` keeps exporting the names `FundamentalStatementMapRow` and `FUNDAMENTAL_STATEMENT_MAP_ROWS` so `tests/test_concept_coverage.py` and any caller importing them keep working unchanged.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_statement_map_seed.py`:

```python
"""Tier1-S2 T1: the statement map is a sorted CSV seed, not a Python literal."""
from __future__ import annotations

import csv
import hashlib
from dataclasses import fields
from pathlib import Path

import conftest

from atx_db.statement_map_seed import (
    STATEMENT_MAP_SEED_COLUMNS,
    STATEMENT_MAP_SEED_PATH,
    FundamentalStatementMapRow,
    default_statement_map_rows,
    read_statement_map_seed,
    statement_map_sort_key,
)

# Row count of the FUNDAMENTAL_STATEMENT_MAP_ROWS literal at the moment it was
# extracted (main @ e4bdcf54). Later sprint tasks that add concepts bump this
# number in the same commit that adds the rows, with the new count justified in
# the commit body.
EXPECTED_STATEMENT_MAP_ROWS = 214


def test_seed_columns_match_dataclass_fields():
    assert STATEMENT_MAP_SEED_COLUMNS == tuple(f.name for f in fields(FundamentalStatementMapRow))


def test_seed_csv_header_matches_columns():
    with STATEMENT_MAP_SEED_PATH.open(newline="", encoding="utf-8") as fh:
        header = tuple(next(csv.reader(fh)))
    assert header == STATEMENT_MAP_SEED_COLUMNS


def test_seed_row_count_matches_extracted_literal():
    assert len(read_statement_map_seed()) == EXPECTED_STATEMENT_MAP_ROWS


def test_seed_is_sorted_canonically():
    rows = read_statement_map_seed()
    keys = [statement_map_sort_key(row) for row in rows]
    assert keys == sorted(keys)


def test_seed_primary_key_is_unique():
    rows = read_statement_map_seed()
    keys = [(r.source, r.taxonomy, r.concept, r.industry_template) for r in rows]
    assert len(keys) == len(set(keys))


def test_default_rows_are_cached_and_equal_the_seed():
    assert default_statement_map_rows() is default_statement_map_rows()
    assert default_statement_map_rows() == read_statement_map_seed()


def test_fundamental_statements_exposes_the_rows_lazily():
    from atx_db import fundamental_statements as fs

    assert "FUNDAMENTAL_STATEMENT_MAP_ROWS" not in vars(fs)
    assert fs.FUNDAMENTAL_STATEMENT_MAP_ROWS == read_statement_map_seed()
    assert fs.FundamentalStatementMapRow is FundamentalStatementMapRow


def test_schema_fingerprint_hashes_the_new_seed():
    seed_paths = sorted((Path(conftest._SOURCE_ROOT) / "atx_db" / "seeds").glob("*.csv"))
    assert STATEMENT_MAP_SEED_PATH in seed_paths
    digest = hashlib.sha256(STATEMENT_MAP_SEED_PATH.read_bytes()).hexdigest()
    assert len(conftest._schema_fingerprint()) == 24
    assert len(digest) == 64


def test_round_trip_write_then_read_is_byte_stable(tmp_path):
    from atx_db.statement_map_seed import write_statement_map_seed

    rows = read_statement_map_seed()
    target = tmp_path / "statement_map.csv"
    written = write_statement_map_seed(rows, target)
    assert written == len(rows)
    assert target.read_bytes() == STATEMENT_MAP_SEED_PATH.read_bytes()
```

- [ ] **Step 2: Run the test to verify it fails**

Run from `C:\atx\atx-db`:

```
.venv\Scripts\python.exe -m pytest tests/test_statement_map_seed.py -n 0 -q
```

Expected: collection error, `ModuleNotFoundError: No module named 'atx_db.statement_map_seed'`.

- [ ] **Step 3: Create the seed module**

Create `C:\atx\atx-db\src\atx_db\statement_map_seed.py`:

```python
"""Tier1-S2 T1: CSV-backed seed for the canonical XBRL statement map.

The 214-row FUNDAMENTAL_STATEMENT_MAP_ROWS literal used to live inside the
6,456-line fundamental_statements.py, which made it the highest-churn file in
the fundamentals chain. It is data, so it belongs beside the other registry
seeds and inside the conftest schema fingerprint.
"""
from __future__ import annotations

import csv
from collections.abc import Iterable
from dataclasses import dataclass, fields
from functools import lru_cache
from pathlib import Path
from typing import NoReturn

STATEMENT_MAP_SEED_PATH = Path(__file__).resolve().parent / "seeds" / "statement_map.csv"


@dataclass(frozen=True)
class FundamentalStatementMapRow:
    source: str
    taxonomy: str
    concept: str
    statement_type: str
    statement_section: str
    canonical_metric: str
    canonical_label: str
    period_type: str
    normal_balance: str
    unit_type: str
    value_multiplier: float
    concept_priority: int
    is_core_metric: bool
    is_active: bool
    notes: str | None = None
    item_id: int | None = None
    industry_template: str = "ALL"
    is_derived: bool = False
    derivation_expr: str | None = None


STATEMENT_MAP_SEED_COLUMNS: tuple[str, ...] = tuple(f.name for f in fields(FundamentalStatementMapRow))

_TRUE_TOKENS = frozenset({"1", "true", "t", "yes", "y"})
_FALSE_TOKENS = frozenset({"0", "false", "f", "no", "n"})


def statement_map_sort_key(row: FundamentalStatementMapRow) -> tuple[str, str, str, str]:
    """Canonical seed ordering: template, statement, taxonomy, concept."""

    return (row.industry_template, row.statement_type, row.taxonomy, row.concept)


def _fail(seed_path: Path, row_number: int, message: str) -> NoReturn:
    raise ValueError(f"{seed_path} row {row_number}: {message}")


def _none_if_blank(value: str | None) -> str | None:
    if value is None:
        return None
    value = value.strip()
    return value or None


def _parse_bool(value: str, *, seed_path: Path, row_number: int, field_name: str) -> bool:
    token = value.strip().lower()
    if token in _TRUE_TOKENS:
        return True
    if token in _FALSE_TOKENS:
        return False
    _fail(seed_path, row_number, f"invalid {field_name} boolean {value!r}")


def _parse_int_or_none(value: str, *, seed_path: Path, row_number: int, field_name: str) -> int | None:
    token = _none_if_blank(value)
    if token is None:
        return None
    try:
        return int(token)
    except ValueError:
        _fail(seed_path, row_number, f"invalid {field_name} integer {value!r}")


def _parse_row(raw: dict[str, str], *, seed_path: Path, row_number: int) -> FundamentalStatementMapRow:
    missing = [column for column in STATEMENT_MAP_SEED_COLUMNS if raw.get(column) is None]
    if missing:
        _fail(seed_path, row_number, f"missing CSV values for fields {missing!r}")
    for column in ("source", "taxonomy", "concept", "statement_type", "statement_section",
                   "canonical_metric", "canonical_label", "period_type", "normal_balance",
                   "unit_type", "industry_template"):
        if _none_if_blank(raw[column]) is None:
            _fail(seed_path, row_number, f"blank required field {column}")
    try:
        value_multiplier = float(raw["value_multiplier"])
    except ValueError:
        _fail(seed_path, row_number, f"invalid value_multiplier {raw['value_multiplier']!r}")
    concept_priority = _parse_int_or_none(
        raw["concept_priority"], seed_path=seed_path, row_number=row_number, field_name="concept_priority"
    )
    if concept_priority is None:
        _fail(seed_path, row_number, "blank required field concept_priority")
    return FundamentalStatementMapRow(
        source=raw["source"].strip(),
        taxonomy=raw["taxonomy"].strip(),
        concept=raw["concept"].strip(),
        statement_type=raw["statement_type"].strip(),
        statement_section=raw["statement_section"].strip(),
        canonical_metric=raw["canonical_metric"].strip(),
        canonical_label=raw["canonical_label"].strip(),
        period_type=raw["period_type"].strip(),
        normal_balance=raw["normal_balance"].strip(),
        unit_type=raw["unit_type"].strip(),
        value_multiplier=value_multiplier,
        concept_priority=concept_priority,
        is_core_metric=_parse_bool(raw["is_core_metric"], seed_path=seed_path, row_number=row_number, field_name="is_core_metric"),
        is_active=_parse_bool(raw["is_active"], seed_path=seed_path, row_number=row_number, field_name="is_active"),
        notes=_none_if_blank(raw["notes"]),
        item_id=_parse_int_or_none(raw["item_id"], seed_path=seed_path, row_number=row_number, field_name="item_id"),
        industry_template=raw["industry_template"].strip(),
        is_derived=_parse_bool(raw["is_derived"], seed_path=seed_path, row_number=row_number, field_name="is_derived"),
        derivation_expr=_none_if_blank(raw["derivation_expr"]),
    )


def read_statement_map_seed(
    path: Path | str = STATEMENT_MAP_SEED_PATH,
) -> tuple[FundamentalStatementMapRow, ...]:
    """Read the committed offline statement-map seed with stdlib csv."""

    seed_path = Path(path)
    with seed_path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if tuple(reader.fieldnames or ()) != STATEMENT_MAP_SEED_COLUMNS:
            raise ValueError(f"{seed_path} has unexpected columns: {reader.fieldnames}")
        return tuple(
            _parse_row(row, seed_path=seed_path, row_number=row_number)
            for row_number, row in enumerate(reader, start=2)
        )


def _serialize(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        return repr(value)
    return str(value)


def write_statement_map_seed(
    rows: Iterable[FundamentalStatementMapRow],
    path: Path | str = STATEMENT_MAP_SEED_PATH,
) -> int:
    """Write rows in canonical sorted order; return the row count.

    Uses the csv module default CRLF terminator, matching the other four
    committed seeds -- fundamental_items.csv, standardization_rules.csv,
    concept_map.csv and formula_registry.csv are all CRLF.
    """

    ordered = sorted(rows, key=statement_map_sort_key)
    seed_path = Path(path)
    with seed_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(STATEMENT_MAP_SEED_COLUMNS)
        for row in ordered:
            writer.writerow([_serialize(getattr(row, column)) for column in STATEMENT_MAP_SEED_COLUMNS])
    return len(ordered)


@lru_cache(maxsize=1)
def default_statement_map_rows() -> tuple[FundamentalStatementMapRow, ...]:
    """Process-local cache of the committed statement-map seed."""

    return read_statement_map_seed(STATEMENT_MAP_SEED_PATH)
```

- [ ] **Step 4: Generate the CSV from the still-present Python literal**

Run this one-shot extractor from `C:\atx\atx-db` **before** deleting the literal. It imports the current tuple and writes the seed through the writer added in Step 3, so the CSV is sorted and round-trip stable by construction:

```
.venv\Scripts\python.exe -c "import sys; sys.path.insert(0, 'src'); from atx_db.fundamental_statements import FUNDAMENTAL_STATEMENT_MAP_ROWS as R; from atx_db.statement_map_seed import FundamentalStatementMapRow as N, write_statement_map_seed; print(write_statement_map_seed(N(*[getattr(r, f) for f in ('source','taxonomy','concept','statement_type','statement_section','canonical_metric','canonical_label','period_type','normal_balance','unit_type','value_multiplier','concept_priority','is_core_metric','is_active','notes','item_id','industry_template','is_derived','derivation_expr')]) for r in R))"
```

Expected stdout: `214`

- [ ] **Step 5: Rewire `fundamental_statements.py`**

Apply exactly these edits to `C:\atx\atx-db\src\atx_db\fundamental_statements.py`:

1. Replace the import block header (lines 1-10) so it reads:

```python
from __future__ import annotations

from dataclasses import astuple

import pandas as pd

from .connection import DuckDBStore
from .industry_templates import refresh_entity_industry_templates
from .statement_map_seed import (
    FundamentalStatementMapRow,
    default_statement_map_rows,
)

SOURCE_NAME = "SEC companyfacts"


def __getattr__(name: str) -> object:
    """PEP 562 shim: keep the historical module-level tuple name working."""

    if name == "FUNDAMENTAL_STATEMENT_MAP_ROWS":
        return default_statement_map_rows()
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
```

2. Delete the `@dataclass(frozen=True) class FundamentalStatementMapRow` definition (old lines 13-34) - it now lives in `statement_map_seed.py` and is re-exported by the import above.
3. Delete the whole `FUNDAMENTAL_STATEMENT_MAP_ROWS: tuple[...] = (...)` literal (old lines 176-4794).
4. In `statement_map_unloadable_overlay_rows`, `statement_map_overlay_exception_rows`, `unexplained_statement_map_overlay_rows` and `concept_map_projection_rows`, change the signature default from `rows: tuple[FundamentalStatementMapRow, ...] = FUNDAMENTAL_STATEMENT_MAP_ROWS` to `rows: tuple[FundamentalStatementMapRow, ...] | None = None` and add as the first statement of each body:

```python
    rows = default_statement_map_rows() if rows is None else rows
```

5. In `seed_fundamental_statement_map`, replace `[astuple(row) for row in FUNDAMENTAL_STATEMENT_MAP_ROWS]` with `[astuple(row) for row in default_statement_map_rows()]`.
6. Grep the module for any remaining bare reference and replace it the same way:

```
.venv\Scripts\python.exe -c "import pathlib,sys; t=pathlib.Path('src/atx_db/fundamental_statements.py').read_text(encoding='utf-8'); n=t.count('FUNDAMENTAL_STATEMENT_MAP_ROWS'); print(n); sys.exit(0 if n==1 else 1)"
```

Expected stdout: `1` (only the `__getattr__` shim), exit code 0.

- [ ] **Step 6: Add the new module to the pinned public API snapshot**

`tests/data/public_api_snapshot.json` pins `sorted(dir(atx_db))`. `fundamental_statements` imports `statement_map_seed`, which binds it as an `atx_db` attribute. Insert the string `"statement_map_seed"` into the `"atx_db"` list, between `"standardization"` and `"storage_admin"`.

- [ ] **Step 7: Run the new test and every test that touches the map**

```
.venv\Scripts\python.exe -m pytest tests/test_statement_map_seed.py -n 0 -q
.venv\Scripts\python.exe -m pytest tests/test_concept_coverage.py tests/test_module_boundaries.py tests/test_standardization.py tests/test_item_registry.py -n 0 -q
```

Expected: first command `9 passed`. Second command: all passed, no failures, no errors. `test_concept_coverage.py::test_concept_map_csv_round_trips_generated_projection` passing is the proof that the extraction was lossless - it compares the generated projection against the untouched committed `concept_map.csv`.

- [ ] **Step 8: Commit**

```bash
git add src/atx_db/statement_map_seed.py src/atx_db/seeds/statement_map.csv src/atx_db/fundamental_statements.py tests/test_statement_map_seed.py tests/data/public_api_snapshot.json
git commit -m "feat(db): move the 214-row statement map into seeds/statement_map.csv

The FUNDAMENTAL_STATEMENT_MAP_ROWS literal was 4,618 of the 6,456 lines in
fundamental_statements.py. It is registry data, so it now lives beside the
other seeds, is covered by the conftest schema fingerprint, and is reloadable
without a Python edit. The module name is preserved via a PEP 562 __getattr__
over an lru_cached loader, so no caller changes.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Widen the CompanyFacts ingest allowlist to every active-rule alias

The audit measured the publication funnel as 235 registry items, 191 with a rule, **113 whose rules reference a concept the CompanyFacts allowlist actually ingests**, 46 published. `fundamental_statements.default_companyfacts_concepts()` returns 137 concepts derived only from the statement-map projection. Every alias added in Tasks 5-7 is inert until it is in that set. This task makes the set the union of the projection and every alias referenced by an active standardization rule, plus the two dei cover-page concepts the shares pipeline needs, so later waves widen the allowlist automatically.

**Files:**
- Modify: `C:\atx\atx-db\src\atx_db\fundamental_statements.py` (`default_companyfacts_concepts`, around line 5059)
- Modify: `C:\atx\atx-db\tests\test_concept_coverage.py` (two pinned assertions)
- Test: `C:\atx\atx-db\tests\test_ingest_allowlist.py`

**Interfaces:**
- Consumes: `atx_db.statement_map_seed.default_statement_map_rows` (Task 1).
- Produces: `atx_db.fundamental_statements.rule_alias_concepts() -> tuple[tuple[str, str], ...]` (sorted distinct `(alias_scheme, alias_code)` over active rules), `atx_db.fundamental_statements.DEI_COVER_PAGE_CONCEPTS: tuple[str, ...]`, and a widened `default_companyfacts_concepts() -> tuple[str, ...]` still returning a sorted tuple of bare concept names. `atx_db.fundamentals.DEFAULT_CONCEPTS` picks the widening up for free because it is assigned from `default_companyfacts_concepts()` at import.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_ingest_allowlist.py`:

```python
"""Tier1-S2 T2: the companyfacts ingest allowlist is derived, not hand-maintained."""
from __future__ import annotations

from atx_db.fundamental_statements import (
    CONCEPT_MAP_SUPPORTED_TAXONOMIES,
    DEI_COVER_PAGE_CONCEPTS,
    concept_map_projection_rows,
    default_companyfacts_concepts,
    rule_alias_concepts,
)
from atx_db.standardization import default_standardization_rules


def test_every_active_rule_alias_is_ingested():
    allowlist = set(default_companyfacts_concepts())
    missing = sorted(
        f"{scheme}:{code}"
        for scheme, code in rule_alias_concepts()
        if scheme in CONCEPT_MAP_SUPPORTED_TAXONOMIES and code not in allowlist
    )
    assert missing == []


def test_every_statement_map_projection_concept_is_ingested():
    allowlist = set(default_companyfacts_concepts())
    missing = sorted({row[1] for row in concept_map_projection_rows()} - allowlist)
    assert missing == []


def test_dei_cover_page_concepts_are_ingested():
    allowlist = set(default_companyfacts_concepts())
    assert set(DEI_COVER_PAGE_CONCEPTS).issubset(allowlist)


def test_allowlist_is_sorted_and_distinct():
    concepts = default_companyfacts_concepts()
    assert list(concepts) == sorted(concepts)
    assert len(concepts) == len(set(concepts))


def test_allowlist_is_deterministic_across_calls():
    assert default_companyfacts_concepts() == default_companyfacts_concepts()


def test_rule_alias_concepts_are_sorted_distinct_pairs():
    pairs = rule_alias_concepts()
    assert list(pairs) == sorted(pairs)
    assert len(pairs) == len(set(pairs))
    assert all(isinstance(p, tuple) and len(p) == 2 for p in pairs)


def test_rule_alias_concepts_skips_inactive_rules():
    active_pairs = {
        (alias.alias_scheme, alias.alias_code)
        for rule in default_standardization_rules()
        if rule.is_active
        for alias in rule.source_aliases
    }
    assert set(rule_alias_concepts()) == active_pairs


def test_allowlist_is_strictly_wider_than_the_projection():
    projection = {row[1] for row in concept_map_projection_rows()}
    assert set(default_companyfacts_concepts()) >= projection
```

- [ ] **Step 2: Run the test to verify it fails**

```
.venv\Scripts\python.exe -m pytest tests/test_ingest_allowlist.py -n 0 -q
```

Expected: collection error, `ImportError: cannot import name 'DEI_COVER_PAGE_CONCEPTS' from 'atx_db.fundamental_statements'`.

- [ ] **Step 3: Implement the derivation**

In `C:\atx\atx-db\src\atx_db\fundamental_statements.py`, add next to `CONCEPT_MAP_SUPPORTED_TAXONOMIES` (around line 55):

```python
# dei cover-page concepts the shares/float pipeline needs. They are not statement
# lines, so they are not in the statement-map projection, but shares_outstanding.py
# and the market-cap chain cannot run without them.
DEI_COVER_PAGE_CONCEPTS: tuple[str, ...] = (
    "EntityCommonStockSharesOutstanding",
    "EntityPublicFloat",
)
```

Replace `default_companyfacts_concepts` (around line 5059) with:

```python
def rule_alias_concepts() -> tuple[tuple[str, str], ...]:
    """Return sorted distinct (alias_scheme, alias_code) over active rules.

    Imported lazily: standardization imports connection/dataset/warehouse and we
    do not want fundamental_statements to pull that chain in at module import.
    """

    from .standardization import default_standardization_rules

    return tuple(
        sorted(
            {
                (alias.alias_scheme, alias.alias_code)
                for rule in default_standardization_rules()
                if rule.is_active
                for alias in rule.source_aliases
            }
        )
    )


def default_companyfacts_concepts() -> tuple[str, ...]:
    """Concept names admitted by the companyfacts loader by default.

    Tier1-S2 T2: this used to be only the statement-map projection (137
    concepts), which throttled the publication funnel - a curated alias could
    never emit because its concept was never ingested. It is now the union of

      1. the active, loadable statement-map projection,
      2. every alias referenced by an active standardization rule whose scheme
         is a supported taxonomy, and
      3. the dei cover-page concepts.

    All three inputs are committed seeds, so the result is deterministic.
    """

    concepts = {row[1] for row in concept_map_projection_rows()}
    supported = set(CONCEPT_MAP_SUPPORTED_TAXONOMIES)
    concepts.update(code for scheme, code in rule_alias_concepts() if scheme in supported)
    concepts.update(DEI_COVER_PAGE_CONCEPTS)
    return tuple(sorted(concepts))
```

- [ ] **Step 4: Run the new test to verify it passes**

```
.venv\Scripts\python.exe -m pytest tests/test_ingest_allowlist.py -n 0 -q
```

Expected: `8 passed`.

- [ ] **Step 5: Update the two concept-coverage assertions that pinned equality**

`tests/test_concept_coverage.py` currently asserts the allowlist *equals* the projection. That is exactly the throttle this task removes, so the two assertions become supersets. Edit `test_default_concepts_cover_active_all_statement_map_concepts` and `test_default_concepts_match_reviewable_concept_map_projection` so the comparisons read:

```python
def test_default_concepts_cover_active_all_statement_map_concepts():
    # Tier1-S2 T2: the allowlist is now the union of the statement-map
    # projection, active-rule aliases, and dei cover-page concepts. It must
    # still COVER every active ALL-template statement-map concept; it is no
    # longer restricted to them.
    concepts = set(default_companyfacts_concepts())
    missing = sorted(
        concept for taxonomy, concept in _active_all_statement_map_concepts()
        if concept not in concepts
    )
    assert missing == []


def test_default_concepts_match_reviewable_concept_map_projection():
    # Tier1-S2 T2: superset, not equality - see rule_alias_concepts().
    rows = _read_concept_map_seed()
    concepts = set(default_companyfacts_concepts())
    assert {row[1] for row in rows} <= concepts
```

Leave `test_concept_map_csv_round_trips_generated_projection` untouched: `concept_map.csv` is still exactly the statement-map projection, and this task does not add statement-map rows.

- [ ] **Step 6: Run the full fundamentals-seed test set**

```
.venv\Scripts\python.exe -m pytest tests/test_ingest_allowlist.py tests/test_concept_coverage.py tests/test_statement_map_seed.py tests/test_standardization.py tests/test_item_registry.py -n 0 -q
```

Expected: all passed, 0 failed.

- [ ] **Step 7: Commit**

```bash
git add src/atx_db/fundamental_statements.py tests/test_ingest_allowlist.py tests/test_concept_coverage.py
git commit -m "feat(db): derive the companyfacts ingest allowlist from active rule aliases

The 137-concept allowlist was the publication throttle: 78 of 191 ruled items
referenced concepts the loader never fetched. default_companyfacts_concepts()
is now the union of the statement-map projection, every alias on an active
standardization rule, and the dei cover-page concepts, so every curated alias
added in the Wave A/B seeds is ingestible by construction.

The two concept-coverage tests that pinned allowlist == projection are relaxed
to allowlist >= projection; that equality WAS the bug.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: Canonical seed ordering, a committed normalizer, and duplicate guards

The four fundamentals seeds are edited by appending rows. Without a canonical order the diffs are unreviewable and merge conflicts are guaranteed across the three alias waves. This task commits a normalizer and the guard tests, and applies the normalizer once. **The re-sort is output-neutral**: `read_standardization_rules` builds a dict keyed by `(basis, item_id)`; `compute_standardized_rows` re-sorts rules by `(item_id, rule_id)`; `_rule_set_digest` sorts by `rule_id`; `Registry.__post_init__` re-sorts aliases; `seed_fundamental_item_registry` sorts both arrow frames. No consumer depends on file order, so `rule_set_sha256` and every standardized value are unchanged.

**Files:**
- Create: `C:\atx\atx-db\scripts\normalize_fundamental_seeds.py`
- Modify: `C:\atx\atx-db\src\atx_db\seeds\standardization_rules.csv` (re-sorted only)
- Modify: `C:\atx\atx-db\src\atx_db\seeds\fundamental_items.csv` (re-sorted only)
- Test: `C:\atx\atx-db\tests\test_seed_determinism.py`

**Interfaces:**
- Consumes: `atx_db.statement_map_seed.write_statement_map_seed`, `read_statement_map_seed`, `statement_map_sort_key` (Task 1); `atx_db.fundamental_statements.concept_map_projection_rows`, `CONCEPT_MAP_SEED_COLUMNS`.
- Produces: `scripts/normalize_fundamental_seeds.py` with `main(argv: list[str] | None = None) -> int`, `--check` mode (exit 1 if any seed is not canonical, write nothing) and default write mode. Tasks 5, 6 and 7 all end by running it.
- Produces the module-level constants `BASIS_ORDER`, `RULE_SORT_KEY_COLUMNS`, `ITEM_SORT_KEY_COLUMNS` used verbatim by `tests/test_seed_determinism.py`.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_seed_determinism.py`:

```python
"""Tier1-S2 T3: all four fundamentals seeds are canonically sorted and duplicate-free."""
from __future__ import annotations

import csv
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

from atx_db.item_registry import SEED_PATH as ITEM_SEED_PATH
from atx_db.item_registry import read_fundamental_item_seed
from atx_db.standardization import RULE_PATH, read_standardization_rules
from atx_db.statement_map_seed import STATEMENT_MAP_SEED_PATH, read_statement_map_seed, statement_map_sort_key

PROJECT_ROOT = Path(__file__).resolve().parents[1]
NORMALIZER = PROJECT_ROOT / "scripts" / "normalize_fundamental_seeds.py"
BASIS_ORDER = ("annual", "quarterly", "ttm", "instant")


def _rule_rows() -> list[dict[str, str]]:
    with RULE_PATH.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def _item_rows() -> list[dict[str, str]]:
    with ITEM_SEED_PATH.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def test_rules_csv_is_sorted_by_item_then_basis():
    keys = [(int(r["item_id"]), BASIS_ORDER.index(r["basis"])) for r in _rule_rows()]
    assert keys == sorted(keys)


def test_rule_alias_json_is_sorted_by_priority_then_scheme_then_code():
    for row in _rule_rows():
        aliases = json.loads(row["source_aliases_json"] or "[]")
        keys = [(int(a.get("priority", 100)), a["alias_scheme"], a["alias_code"]) for a in aliases]
        assert keys == sorted(keys), row["rule_id"]


def test_rule_alias_json_has_no_duplicate_alias_within_a_rule():
    for row in _rule_rows():
        aliases = json.loads(row["source_aliases_json"] or "[]")
        pairs = [(a["alias_scheme"], a["alias_code"]) for a in aliases]
        assert len(pairs) == len(set(pairs)), row["rule_id"]


def test_item_seed_is_grouped_by_item_and_sorted_within_the_group():
    rows = _item_rows()
    keys = [
        (
            int(r["item_id"]),
            0 if r["alias_scheme"].strip() else (1 if r["vendor"].strip() else 2),
            int(r["coalesce_priority"]) if r["coalesce_priority"].strip() else 0,
            r["alias_scheme"],
            r["alias_code"],
            r["vendor"],
            r["vendor_field"],
        )
        for r in rows
    ]
    assert keys == sorted(keys)


def test_no_duplicate_item_alias_pairs_in_the_item_seed():
    counts = Counter(
        (int(r["item_id"]), r["alias_scheme"], r["alias_code"])
        for r in _item_rows()
        if r["alias_scheme"].strip()
    )
    assert [k for k, n in counts.items() if n > 1] == []


def test_no_alias_code_maps_to_two_items_in_the_item_seed():
    owners: dict[tuple[str, str], set[int]] = {}
    for row in read_fundamental_item_seed():
        if row.alias_scheme is None or row.alias_code is None:
            continue
        owners.setdefault((row.alias_scheme, row.alias_code), set()).add(row.item_id)
    conflicts = {k: sorted(v) for k, v in owners.items() if len(v) > 1}
    assert conflicts == {}


def test_statement_map_seed_is_sorted():
    keys = [statement_map_sort_key(row) for row in read_statement_map_seed()]
    assert keys == sorted(keys)


def test_rules_have_one_active_rule_per_item_and_basis():
    seen: set[tuple[int, str]] = set()
    for rule in read_standardization_rules():
        if not rule.is_active:
            continue
        key = (rule.item_id, rule.basis)
        assert key not in seen, key
        seen.add(key)


def test_normalizer_check_mode_reports_the_seeds_are_already_canonical():
    result = subprocess.run(
        [sys.executable, str(NORMALIZER), "--check"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert json.loads(result.stdout)["dirty"] == []
```

- [ ] **Step 2: Run the test to verify it fails**

```
.venv\Scripts\python.exe -m pytest tests/test_seed_determinism.py -n 0 -q
```

Expected: `test_rules_csv_is_sorted_by_item_then_basis` FAILS (the tail block `std_instant_1045`, `std_annual_1801`, `std_quarterly_1801`, `std_ttm_1801` was appended out of order), `test_item_seed_is_grouped_by_item_and_sorted_within_the_group` FAILS (items 1003, 1022, 1027, 1039, 1105 and 1117 have alias rows ordered by code instead of by priority), `test_normalizer_check_mode_...` FAILS with `FileNotFoundError`.

- [ ] **Step 3: Write the normalizer**

Create `C:\atx\atx-db\scripts\normalize_fundamental_seeds.py`:

```python
#!/usr/bin/env python
"""Rewrite the four fundamentals seeds in canonical sorted order.

Run after appending rows to any of:
  src/atx_db/seeds/statement_map.csv
  src/atx_db/seeds/fundamental_items.csv
  src/atx_db/seeds/standardization_rules.csv

concept_map.csv is never hand-edited: it is regenerated here as the exact
projection of the statement map, which is what
tests/test_concept_coverage.py::test_concept_map_csv_round_trips_generated_projection
asserts.

Ordering contracts
  statement_map.csv        (industry_template, statement_type, taxonomy, concept)
  fundamental_items.csv    (item_id, row_kind, coalesce_priority, alias_scheme,
                            alias_code, vendor, vendor_field) where row_kind is
                            0 alias / 1 vendor / 2 bare
  standardization_rules.csv(item_id, BASIS_ORDER.index(basis)); inside each rule
                            source_aliases_json is sorted by
                            (priority, alias_scheme, alias_code)
  concept_map.csv          whatever concept_map_projection_rows() emits
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.fundamental_statements import CONCEPT_MAP_SEED_COLUMNS, concept_map_projection_rows
from atx_db.item_registry import SEED_COLUMNS as ITEM_SEED_COLUMNS
from atx_db.item_registry import SEED_PATH as ITEM_SEED_PATH
from atx_db.standardization import RULE_COLUMNS, RULE_PATH
from atx_db.statement_map_seed import (
    STATEMENT_MAP_SEED_PATH,
    read_statement_map_seed,
    statement_map_sort_key,
    write_statement_map_seed,
)

SEEDS_DIR = Path(__file__).resolve().parents[1] / "src" / "atx_db" / "seeds"
CONCEPT_MAP_PATH = SEEDS_DIR / "concept_map.csv"

BASIS_ORDER = ("annual", "quarterly", "ttm", "instant")
RULE_SORT_KEY_COLUMNS = ("item_id", "basis")
ITEM_SORT_KEY_COLUMNS = (
    "item_id",
    "row_kind",
    "coalesce_priority",
    "alias_scheme",
    "alias_code",
    "vendor",
    "vendor_field",
)


def _read_rows(path: Path, columns: tuple[str, ...]) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if tuple(reader.fieldnames or ()) != columns:
            raise SystemExit(f"{path} has unexpected columns: {reader.fieldnames}")
        return list(reader)


def _write_rows(path: Path, columns: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(columns))
        writer.writeheader()
        writer.writerows(rows)


def _rule_sort_key(row: dict[str, str]) -> tuple[int, int]:
    return (int(row["item_id"]), BASIS_ORDER.index(row["basis"]))


def _normalized_alias_json(raw: str) -> str:
    aliases = json.loads(raw or "[]")
    ordered = sorted(
        aliases,
        key=lambda a: (int(a.get("priority", 100)), str(a["alias_scheme"]), str(a["alias_code"])),
    )
    rebuilt = [
        {
            "alias_code": str(a["alias_code"]),
            "alias_scheme": str(a["alias_scheme"]),
            "priority": int(a.get("priority", 100)),
        }
        for a in ordered
    ]
    return json.dumps(rebuilt, separators=(",", ":"), sort_keys=True)


def _item_sort_key(row: dict[str, str]) -> tuple[int, int, int, str, str, str, str]:
    has_alias = bool(row["alias_scheme"].strip())
    has_vendor = bool(row["vendor"].strip())
    row_kind = 0 if has_alias else (1 if has_vendor else 2)
    priority = int(row["coalesce_priority"]) if row["coalesce_priority"].strip() else 0
    return (
        int(row["item_id"]),
        row_kind,
        priority,
        row["alias_scheme"],
        row["alias_code"],
        row["vendor"],
        row["vendor_field"],
    )


def _canonical_payloads() -> dict[Path, bytes]:
    payloads: dict[Path, bytes] = {}

    statement_rows = sorted(read_statement_map_seed(), key=statement_map_sort_key)
    scratch = STATEMENT_MAP_SEED_PATH.with_suffix(".csv.normalize-tmp")
    write_statement_map_seed(statement_rows, scratch)
    payloads[STATEMENT_MAP_SEED_PATH] = scratch.read_bytes()
    scratch.unlink()

    rule_rows = _read_rows(RULE_PATH, RULE_COLUMNS)
    for row in rule_rows:
        row["source_aliases_json"] = _normalized_alias_json(row["source_aliases_json"])
    rule_rows.sort(key=_rule_sort_key)
    scratch = RULE_PATH.with_suffix(".csv.normalize-tmp")
    _write_rows(scratch, RULE_COLUMNS, rule_rows)
    payloads[RULE_PATH] = scratch.read_bytes()
    scratch.unlink()

    item_rows = _read_rows(ITEM_SEED_PATH, ITEM_SEED_COLUMNS)
    item_rows.sort(key=_item_sort_key)
    scratch = ITEM_SEED_PATH.with_suffix(".csv.normalize-tmp")
    _write_rows(scratch, ITEM_SEED_COLUMNS, item_rows)
    payloads[ITEM_SEED_PATH] = scratch.read_bytes()
    scratch.unlink()

    concept_rows = [dict(zip(CONCEPT_MAP_SEED_COLUMNS, values, strict=True)) for values in concept_map_projection_rows()]
    for row in concept_rows:
        row["item_id"] = str(row["item_id"])
    scratch = CONCEPT_MAP_PATH.with_suffix(".csv.normalize-tmp")
    _write_rows(scratch, CONCEPT_MAP_SEED_COLUMNS, concept_rows)
    payloads[CONCEPT_MAP_PATH] = scratch.read_bytes()
    scratch.unlink()

    return payloads


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report drift without writing")
    args = parser.parse_args(argv)

    payloads = _canonical_payloads()
    dirty = sorted(
        path.name for path, payload in payloads.items() if path.read_bytes() != payload
    )
    if not args.check:
        for path, payload in payloads.items():
            if path.read_bytes() != payload:
                path.write_bytes(payload)
    print(json.dumps({"dirty": dirty, "checked": sorted(p.name for p in payloads)}, indent=2))
    if args.check and dirty:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Apply the normalizer once and confirm it is a pure re-ordering**

```
.venv\Scripts\python.exe -c "import hashlib,pathlib,sys; sys.path.insert(0,'src'); from atx_db.standardization import read_standardization_rules; from atx_db._standardization_set_based import _rule_set_digest; print(_rule_set_digest(read_standardization_rules()))"
.venv\Scripts\python.exe scripts\normalize_fundamental_seeds.py
.venv\Scripts\python.exe -c "import hashlib,pathlib,sys; sys.path.insert(0,'src'); from atx_db.standardization import read_standardization_rules; from atx_db._standardization_set_based import _rule_set_digest; print(_rule_set_digest(read_standardization_rules()))"
git diff --stat src/atx_db/seeds/
```

Expected: the two digests are **identical** 64-hex strings. `git diff --stat` shows `standardization_rules.csv` and `fundamental_items.csv` changed with equal insertions and deletions (pure reorder), `statement_map.csv` and `concept_map.csv` unchanged.

- [ ] **Step 5: Run the guard tests plus everything that reads a seed**

```
.venv\Scripts\python.exe -m pytest tests/test_seed_determinism.py -n 0 -q
.venv\Scripts\python.exe -m pytest tests/test_standardization.py tests/test_item_registry.py tests/test_concept_coverage.py tests/test_statement_map_seed.py tests/test_ingest_allowlist.py -n 0 -q
```

Expected: `9 passed` then all passed.

- [ ] **Step 6: Commit**

```bash
git add scripts/normalize_fundamental_seeds.py tests/test_seed_determinism.py src/atx_db/seeds/standardization_rules.csv src/atx_db/seeds/fundamental_items.csv
git commit -m "feat(db): canonical ordering + duplicate guards for the fundamentals seeds

scripts/normalize_fundamental_seeds.py rewrites statement_map.csv,
fundamental_items.csv and standardization_rules.csv in a canonical order and
regenerates concept_map.csv as the statement-map projection. Applied once here:
a pure re-ordering, verified by an unchanged _rule_set_digest before and after.

Guard tests pin the sort, forbid duplicate (item, alias) rows, forbid one alias
code owning two item ids, and pin one active rule per (item_id, basis).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Deterministic alias-mining research tool

Ranks every observed us-gaap concept against every registry item so a human or agent can curate the Wave A/B seeds from evidence instead of memory. It **never writes a rule or a seed** - its only output is `research/alias_candidates.csv`.

**Files:**
- Create: `C:\atx\atx-db\src\atx_db\alias_mining.py`
- Create: `C:\atx\atx-db\scripts\mine_concept_aliases.py`
- Modify: `C:\atx\atx-db\tests\data\public_api_snapshot.json`
- Test: `C:\atx\atx-db\tests\test_alias_mining.py`

**Interfaces:**
- Consumes: `atx_db.connection.DuckDBStore`, `atx_db.item_registry.seed_fundamental_item_registry`, `atx_db.fundamental_statements.seed_fundamental_statement_map`.
- Produces: `ConceptProfile`, `AliasCandidate`, `AliasMiningOptions`, `ALIAS_CANDIDATE_COLUMNS`, `tokenize_identifier`, `inverse_document_frequency`, `tf_idf_vector`, `cosine_similarity`, `load_concept_profiles`, `load_item_label_sets`, `load_existing_alias_owners`, `score_alias_candidates`, `mine_alias_candidates`, `write_alias_candidates`.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_alias_mining.py`:

```python
"""Tier1-S2 T4: deterministic alias mining over a tiny synthetic corpus."""
from __future__ import annotations

import csv
import datetime as dt

import pytest

from atx_db.alias_mining import (
    ALIAS_CANDIDATE_COLUMNS,
    AliasMiningOptions,
    cosine_similarity,
    inverse_document_frequency,
    load_concept_profiles,
    mine_alias_candidates,
    tf_idf_vector,
    tokenize_identifier,
    write_alias_candidates,
)

FACTS = [
    # (cik, taxonomy, concept, label, fiscal_year, value)
    ("0000320193", "us-gaap", "RevenueFromContractWithCustomerExcludingAssessedTax", "Revenue from contract with customer", 2020, 2.7e11),
    ("0000789019", "us-gaap", "RevenueFromContractWithCustomerExcludingAssessedTax", "Revenue from contract with customer", 2021, 1.6e11),
    ("0000034088", "us-gaap", "Revenues", "Revenues", 2016, 2.0e11),
    ("0000034088", "us-gaap", "Revenues", "Revenues", 2017, 2.4e11),
    ("0000019617", "us-gaap", "RevenuesNetOfInterestExpense", "Revenues net of interest expense", 2019, 1.1e11),
    ("0000019617", "us-gaap", "InterestAndDividendIncomeOperating", "Interest and dividend income operating", 2019, 5.5e10),
    ("0000004977", "us-gaap", "PremiumsEarnedNet", "Premiums earned net", 2018, 1.8e10),
]

# calculation arcs: (parent_concept, child_concept)
ARCS = [
    ("Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax"),
    ("Revenues", "RevenuesNetOfInterestExpense"),
    ("InterestIncomeExpenseNet", "InterestAndDividendIncomeOperating"),
]


def _seed_corpus(store):
    from atx_db.fundamental_statements import seed_fundamental_statement_map
    from atx_db.item_registry import seed_fundamental_item_registry

    seed_fundamental_item_registry(store)
    seed_fundamental_statement_map(store)
    for index, (cik, taxonomy, concept, label, fiscal_year, value) in enumerate(FACTS):
        store.con.execute(
            """
            INSERT INTO sec_company_facts (
                source, security_id, entity_id, cik, taxonomy, concept, label, description,
                unit, period_start, period_end, filed_date, fiscal_year, fiscal_period,
                form, accession_number, frame, value, available_at, run_id,
                source_url, source_loaded_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """,
            [
                "SEC companyfacts", f"sec-{cik}", None, cik, taxonomy, concept, label, None,
                "USD", dt.date(fiscal_year, 1, 1), dt.date(fiscal_year, 12, 31),
                dt.date(fiscal_year + 1, 2, 1), fiscal_year, "FY", "10-K",
                f"acc-{index}", None, value, dt.datetime(fiscal_year + 1, 2, 1),
                None, "https://example.invalid", dt.datetime(2026, 1, 1),
            ],
        )
    for index, (parent, child) in enumerate(ARCS):
        store.con.execute(
            """
            INSERT INTO xbrl_taxonomy_relationships (
                relationship_id, taxonomy_package_id, taxonomy, release_year, linkbase_type,
                source_file, role_uri, role_name, role_href, arcrole, from_label, to_label,
                parent_href, parent_taxonomy, parent_concept, parent_concept_kind,
                child_href, child_taxonomy, child_concept, child_concept_kind,
                order_value, weight, priority, preferred_label, use, closed, context_element,
                usable, target_role, touches_observed_concept, source_url, source_loaded_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """,
            [
                f"rel-{index}", "us-gaap-2026", "us-gaap", 2026, "calculation",
                "us-gaap-cal.xml", "http://example.invalid/role/IS", "IS", None,
                "http://www.xbrl.org/2003/arcrole/summation-item", parent, child,
                None, "us-gaap", parent, "concept",
                None, "us-gaap", child, "concept",
                1.0, 1.0, 0, None, None, None, None, None, None, False,
                "https://example.invalid", dt.datetime(2026, 1, 1),
            ],
        )


def test_tokenize_identifier_splits_camel_case_and_lowercases():
    assert tokenize_identifier("RevenueFromContractWithCustomerExcludingAssessedTax") == (
        "revenue", "contract", "customer", "excluding", "assessed", "tax",
    )
    assert tokenize_identifier("Revenues net of interest expense") == (
        "revenues", "net", "interest", "expense",
    )


def test_idf_and_cosine_are_pure_and_deterministic():
    docs = [("revenue", "total"), ("revenue", "cost"), ("cash", "equivalents")]
    idf = inverse_document_frequency(docs)
    assert idf["cash"] > idf["revenue"]
    a = tf_idf_vector(("revenue", "total"), idf)
    b = tf_idf_vector(("revenue", "total"), idf)
    assert cosine_similarity(a, b) == pytest.approx(1.0)
    c = tf_idf_vector(("cash", "equivalents"), idf)
    assert cosine_similarity(a, c) == pytest.approx(0.0)


def test_load_concept_profiles_counts_filers_years_and_calculation_parent(tmp_store):
    _seed_corpus(tmp_store)
    profiles = {p.concept: p for p in load_concept_profiles(tmp_store, AliasMiningOptions(minimum_filer_count=1))}

    revenues = profiles["Revenues"]
    assert revenues.filer_count == 1
    assert revenues.fact_count == 2
    assert revenues.first_fiscal_year == 2016
    assert revenues.last_fiscal_year == 2017
    assert revenues.parent_concept is None

    asc606 = profiles["RevenueFromContractWithCustomerExcludingAssessedTax"]
    assert asc606.filer_count == 2
    assert asc606.parent_concept == "Revenues"
    assert asc606.statement_placement == "income_statement"


def test_mine_ranks_revenue_variants_against_item_1001(tmp_store):
    _seed_corpus(tmp_store)
    candidates = mine_alias_candidates(tmp_store, AliasMiningOptions(minimum_filer_count=1, top_n=5))
    for_1001 = [c for c in candidates if c.item_id == 1001]
    assert [c.rank for c in for_1001] == list(range(1, len(for_1001) + 1))
    assert "RevenuesNetOfInterestExpense" in {c.concept for c in for_1001}
    mapped = {c.concept: c.already_mapped for c in for_1001}
    assert mapped["Revenues"] is True
    assert mapped["RevenuesNetOfInterestExpense"] is False


def test_mining_is_byte_identical_across_two_runs(tmp_store, tmp_path):
    _seed_corpus(tmp_store)
    options = AliasMiningOptions(minimum_filer_count=1, top_n=5)
    first = tmp_path / "a.csv"
    second = tmp_path / "b.csv"
    write_alias_candidates(mine_alias_candidates(tmp_store, options), first)
    write_alias_candidates(mine_alias_candidates(tmp_store, options), second)
    assert first.read_bytes() == second.read_bytes()
    with first.open(newline="", encoding="utf-8") as fh:
        assert tuple(next(csv.reader(fh))) == ALIAS_CANDIDATE_COLUMNS


def test_mining_never_touches_the_rule_or_item_seeds(tmp_store):
    from atx_db.item_registry import SEED_PATH
    from atx_db.standardization import RULE_PATH

    before = (SEED_PATH.read_bytes(), RULE_PATH.read_bytes())
    _seed_corpus(tmp_store)
    mine_alias_candidates(tmp_store, AliasMiningOptions(minimum_filer_count=1))
    assert (SEED_PATH.read_bytes(), RULE_PATH.read_bytes()) == before
```

- [ ] **Step 2: Run the test to verify it fails**

```
.venv\Scripts\python.exe -m pytest tests/test_alias_mining.py -n 0 -q
```

Expected: collection error, `ModuleNotFoundError: No module named 'atx_db.alias_mining'`.

- [ ] **Step 3: Implement `alias_mining.py`**

Create `C:\atx\atx-db\src\atx_db\alias_mining.py`:

```python
"""Tier1-S2 T4: deterministic alias-candidate mining (research only).

Given the observed companyfacts corpus and the FASB calculation linkbase, rank
every us-gaap concept against every canonical registry item so a curator can
pick the alias rows to add to the seeds. This module NEVER writes a rule, an
alias, or a seed. Its only artifact is research/alias_candidates.csv.

TF-IDF and cosine similarity are implemented here in ~40 lines of stdlib
arithmetic: the project has no sklearn dependency and does not want one for a
research script.
"""
from __future__ import annotations

import csv
import math
import re
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from .connection import DuckDBStore

ALIAS_CANDIDATE_COLUMNS = (
    "item_id",
    "canonical_code",
    "rank",
    "label_similarity",
    "taxonomy",
    "concept",
    "filer_count",
    "fact_count",
    "first_fiscal_year",
    "last_fiscal_year",
    "parent_concept",
    "statement_placement",
    "already_mapped",
)

_CAMEL_SPLIT = re.compile(r"[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+")
_STOPWORDS = frozenset(
    {
        "and", "of", "the", "to", "for", "from", "in", "on", "at", "by", "a", "an",
        "or", "with", "per", "its", "other", "abstract", "member", "axis", "domain",
    }
)


@dataclass(frozen=True)
class AliasMiningOptions:
    minimum_filer_count: int = 5
    minimum_fact_count: int = 1
    top_n: int = 25
    minimum_similarity: float = 0.0
    taxonomies: tuple[str, ...] = ("us-gaap", "dei")


@dataclass(frozen=True)
class ConceptProfile:
    taxonomy: str
    concept: str
    filer_count: int
    fact_count: int
    first_fiscal_year: int | None
    last_fiscal_year: int | None
    parent_concept: str | None
    statement_placement: str | None
    labels: tuple[str, ...]


@dataclass(frozen=True)
class AliasCandidate:
    item_id: int
    canonical_code: str
    rank: int
    label_similarity: float
    taxonomy: str
    concept: str
    filer_count: int
    fact_count: int
    first_fiscal_year: int | None
    last_fiscal_year: int | None
    parent_concept: str | None
    statement_placement: str | None
    already_mapped: bool


def tokenize_identifier(value: str) -> tuple[str, ...]:
    """Split CamelCase or free text into lowercase content tokens."""

    tokens: list[str] = []
    for chunk in re.split(r"[^A-Za-z0-9]+", value or ""):
        for part in _CAMEL_SPLIT.findall(chunk):
            token = part.lower()
            if token and token not in _STOPWORDS and not token.isdigit():
                tokens.append(token)
    return tuple(tokens)


def inverse_document_frequency(documents: Sequence[Sequence[str]]) -> dict[str, float]:
    """Smoothed IDF over tokenized documents; deterministic for a fixed input."""

    total = len(documents)
    counts: dict[str, int] = {}
    for document in documents:
        for token in set(document):
            counts[token] = counts.get(token, 0) + 1
    return {token: math.log((1.0 + total) / (1.0 + count)) + 1.0 for token, count in sorted(counts.items())}


def tf_idf_vector(tokens: Sequence[str], idf: Mapping[str, float]) -> dict[str, float]:
    """L2-normalized TF-IDF vector; unknown tokens are dropped."""

    if not tokens:
        return {}
    raw: dict[str, float] = {}
    for token in tokens:
        weight = idf.get(token)
        if weight is None:
            continue
        raw[token] = raw.get(token, 0.0) + weight
    norm = math.sqrt(sum(value * value for value in raw.values()))
    if norm == 0.0:
        return {}
    return {token: value / norm for token, value in sorted(raw.items())}


def cosine_similarity(left: Mapping[str, float], right: Mapping[str, float]) -> float:
    """Dot product of two already-normalized sparse vectors."""

    if len(left) > len(right):
        left, right = right, left
    return sum(value * right.get(token, 0.0) for token, value in left.items())


def load_concept_profiles(
    store: DuckDBStore,
    options: AliasMiningOptions | None = None,
) -> tuple[ConceptProfile, ...]:
    """Per-concept filer/fact/year counts, calculation parent, statement placement."""

    options = options or AliasMiningOptions()
    frame = store.con.execute(
        """
        WITH observed AS (
            SELECT
                f.taxonomy,
                f.concept,
                count(DISTINCT f.cik) AS filer_count,
                count(*) AS fact_count,
                min(f.fiscal_year) AS first_fiscal_year,
                max(f.fiscal_year) AS last_fiscal_year,
                list_sort(list_distinct(list(f.label) FILTER (WHERE f.label IS NOT NULL))) AS labels
            FROM sec_company_facts f
            WHERE f.taxonomy = ANY(?)
            GROUP BY f.taxonomy, f.concept
        ),
        arcs AS (
            SELECT
                child_taxonomy AS taxonomy,
                child_concept AS concept,
                min(parent_concept) AS parent_concept
            FROM xbrl_taxonomy_relationships
            WHERE linkbase_type = 'calculation'
            GROUP BY child_taxonomy, child_concept
        ),
        placement AS (
            SELECT taxonomy, concept, min(statement_type) AS statement_type
            FROM fundamental_statement_map
            WHERE is_active
            GROUP BY taxonomy, concept
        )
        SELECT
            o.taxonomy,
            o.concept,
            o.filer_count,
            o.fact_count,
            o.first_fiscal_year,
            o.last_fiscal_year,
            a.parent_concept,
            coalesce(self_place.statement_type, parent_place.statement_type) AS statement_placement,
            o.labels
        FROM observed o
        LEFT JOIN arcs a ON a.taxonomy = o.taxonomy AND a.concept = o.concept
        LEFT JOIN placement self_place ON self_place.taxonomy = o.taxonomy AND self_place.concept = o.concept
        LEFT JOIN placement parent_place ON parent_place.taxonomy = o.taxonomy AND parent_place.concept = a.parent_concept
        WHERE o.filer_count >= ? AND o.fact_count >= ?
        ORDER BY o.taxonomy, o.concept
        """,
        [list(options.taxonomies), options.minimum_filer_count, options.minimum_fact_count],
    ).fetchall()
    return tuple(
        ConceptProfile(
            taxonomy=str(row[0]),
            concept=str(row[1]),
            filer_count=int(row[2]),
            fact_count=int(row[3]),
            first_fiscal_year=None if row[4] is None else int(row[4]),
            last_fiscal_year=None if row[5] is None else int(row[5]),
            parent_concept=None if row[6] is None else str(row[6]),
            statement_placement=None if row[7] is None else str(row[7]),
            labels=tuple(str(label) for label in (row[8] or ())),
        )
        for row in frame
    )


def load_item_label_sets(store: DuckDBStore) -> dict[int, tuple[str, tuple[str, ...]]]:
    """item_id -> (canonical_code, label strings) from the registry and the map."""

    rows = store.con.execute(
        """
        SELECT
            i.item_id,
            i.canonical_code,
            list_sort(list_distinct(
                list(i.canonical_code)
                || list(coalesce(i.definition, ''))
                || coalesce(list(m.canonical_label) FILTER (WHERE m.canonical_label IS NOT NULL), [])
                || coalesce(list(m.canonical_metric) FILTER (WHERE m.canonical_metric IS NOT NULL), [])
            )) AS labels
        FROM fundamental_item i
        LEFT JOIN fundamental_statement_map m ON m.item_id = i.item_id AND m.is_active
        WHERE i.item_id < 2000
        GROUP BY i.item_id, i.canonical_code
        ORDER BY i.item_id
        """
    ).fetchall()
    return {
        int(row[0]): (str(row[1]), tuple(str(label) for label in (row[2] or ()) if str(label).strip()))
        for row in rows
    }


def load_existing_alias_owners(store: DuckDBStore) -> dict[tuple[str, str], int]:
    """(alias_scheme, alias_code) -> item_id for aliases already curated."""

    rows = store.con.execute(
        """
        SELECT alias_scheme, alias_code, min(item_id)
        FROM fundamental_item_alias
        GROUP BY alias_scheme, alias_code
        UNION
        SELECT taxonomy, concept, min(item_id)
        FROM fundamental_statement_map
        WHERE is_active AND item_id IS NOT NULL
        GROUP BY taxonomy, concept
        """
    ).fetchall()
    owners: dict[tuple[str, str], int] = {}
    for scheme, code, item_id in rows:
        owners.setdefault((str(scheme), str(code)), int(item_id))
    return owners


def score_alias_candidates(
    profiles: Iterable[ConceptProfile],
    item_labels: Mapping[int, tuple[str, tuple[str, ...]]],
    *,
    existing_alias_owners: Mapping[tuple[str, str], int],
    top_n: int = 25,
    minimum_similarity: float = 0.0,
) -> tuple[AliasCandidate, ...]:
    """Pure ranking: cosine(concept tokens, item label tokens) per item."""

    profile_list = sorted(profiles, key=lambda p: (p.taxonomy, p.concept))
    concept_tokens = {
        (p.taxonomy, p.concept): tokenize_identifier(p.concept) + tuple(
            token for label in p.labels for token in tokenize_identifier(label)
        )
        for p in profile_list
    }
    item_tokens = {
        item_id: tuple(token for label in labels for token in tokenize_identifier(label))
        for item_id, (_, labels) in sorted(item_labels.items())
    }
    documents = [*concept_tokens.values(), *item_tokens.values()]
    idf = inverse_document_frequency(documents)
    concept_vectors = {key: tf_idf_vector(tokens, idf) for key, tokens in concept_tokens.items()}
    item_vectors = {item_id: tf_idf_vector(tokens, idf) for item_id, tokens in item_tokens.items()}

    candidates: list[AliasCandidate] = []
    for item_id in sorted(item_vectors):
        canonical_code = item_labels[item_id][0]
        scored: list[tuple[float, ConceptProfile]] = []
        for profile in profile_list:
            similarity = cosine_similarity(
                item_vectors[item_id], concept_vectors[(profile.taxonomy, profile.concept)]
            )
            if similarity < minimum_similarity:
                continue
            scored.append((similarity, profile))
        scored.sort(key=lambda pair: (-round(pair[0], 12), -pair[1].filer_count, pair[1].concept))
        for rank, (similarity, profile) in enumerate(scored[:top_n], start=1):
            owner = existing_alias_owners.get((profile.taxonomy, profile.concept))
            candidates.append(
                AliasCandidate(
                    item_id=item_id,
                    canonical_code=canonical_code,
                    rank=rank,
                    label_similarity=round(similarity, 6),
                    taxonomy=profile.taxonomy,
                    concept=profile.concept,
                    filer_count=profile.filer_count,
                    fact_count=profile.fact_count,
                    first_fiscal_year=profile.first_fiscal_year,
                    last_fiscal_year=profile.last_fiscal_year,
                    parent_concept=profile.parent_concept,
                    statement_placement=profile.statement_placement,
                    already_mapped=owner == item_id,
                )
            )
    return tuple(candidates)


def mine_alias_candidates(
    store: DuckDBStore,
    options: AliasMiningOptions | None = None,
) -> tuple[AliasCandidate, ...]:
    """Load, score, and rank. Read-only against the warehouse."""

    options = options or AliasMiningOptions()
    return score_alias_candidates(
        load_concept_profiles(store, options),
        load_item_label_sets(store),
        existing_alias_owners=load_existing_alias_owners(store),
        top_n=options.top_n,
        minimum_similarity=options.minimum_similarity,
    )


def write_alias_candidates(candidates: Iterable[AliasCandidate], path: Path | str) -> int:
    """Write the ranked candidates as CSV; returns the row count."""

    rows = list(candidates)
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(ALIAS_CANDIDATE_COLUMNS)
        for candidate in rows:
            writer.writerow(
                [
                    candidate.item_id,
                    candidate.canonical_code,
                    candidate.rank,
                    f"{candidate.label_similarity:.6f}",
                    candidate.taxonomy,
                    candidate.concept,
                    candidate.filer_count,
                    candidate.fact_count,
                    "" if candidate.first_fiscal_year is None else candidate.first_fiscal_year,
                    "" if candidate.last_fiscal_year is None else candidate.last_fiscal_year,
                    "" if candidate.parent_concept is None else candidate.parent_concept,
                    "" if candidate.statement_placement is None else candidate.statement_placement,
                    "true" if candidate.already_mapped else "false",
                ]
            )
    return len(rows)
```

- [ ] **Step 4: Write the operator CLI**

Create `C:\atx\atx-db\scripts\mine_concept_aliases.py`:

```python
#!/usr/bin/env python
"""Rank observed us-gaap concepts against canonical items (research only).

Writes research/alias_candidates.csv. It never edits a seed. Curate the output
by hand into src/atx_db/seeds/statement_map.csv + fundamental_items.csv +
standardization_rules.csv, then run scripts/normalize_fundamental_seeds.py.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.alias_mining import AliasMiningOptions, mine_alias_candidates, write_alias_candidates
from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore

DEFAULT_OUTPUT = Path(__file__).resolve().parents[1] / "research" / "alias_candidates.csv"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--minimum-filer-count", type=int, default=5)
    parser.add_argument("--minimum-fact-count", type=int, default=1)
    parser.add_argument("--top-n", type=int, default=25)
    parser.add_argument("--minimum-similarity", type=float, default=0.05)
    args = parser.parse_args(argv)

    options = AliasMiningOptions(
        minimum_filer_count=args.minimum_filer_count,
        minimum_fact_count=args.minimum_fact_count,
        top_n=args.top_n,
        minimum_similarity=args.minimum_similarity,
    )
    with DuckDBStore(args.db_path, read_only=True) as store:
        candidates = mine_alias_candidates(store, options)
    written = write_alias_candidates(candidates, args.output)
    print(
        json.dumps(
            {
                "output": str(args.output),
                "rows": written,
                "items_ranked": len({c.item_id for c in candidates}),
                "concepts_profiled": len({(c.taxonomy, c.concept) for c in candidates}),
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 5: Register the new module in the public API snapshot**

`alias_mining` imports `connection`, so importing it binds it on the `atx_db` package. Insert `"alias_mining"` into the `"atx_db"` list in `tests/data/public_api_snapshot.json`, between `"adjustment_factors_asof"` and `"alpha_research"`.

- [ ] **Step 6: Run the tests**

```
.venv\Scripts\python.exe -m pytest tests/test_alias_mining.py -n 0 -q
.venv\Scripts\python.exe -m pytest tests/test_module_boundaries.py -n 0 -q
```

Expected: `7 passed`, then all passed.

- [ ] **Step 7: Commit**

```bash
git add src/atx_db/alias_mining.py scripts/mine_concept_aliases.py tests/test_alias_mining.py tests/data/public_api_snapshot.json
git commit -m "feat(db): deterministic alias-candidate mining over companyfacts + calc arcs

Per us-gaap concept: filer count, fact count, first/last fiscal year, the
calculation-linkbase parent, the inferred statement placement, and a pure-stdlib
TF-IDF cosine against each registry item label set. Output is ranked
research/alias_candidates.csv only - the tool never writes a rule or a seed, and
a test pins that the two seed files are byte-identical after a mining run.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Fallback composition rules (`coalesce_or_sum`, `coalesce_or_difference`)

`read_standardization_rules` rejects a duplicate active `(item_id, basis)`, so today an item can have a direct-alias rule **or** a composition rule, never "prefer the tag, fall back to the identity". That blocks every derived-if-missing item the charter names: `gross_profit`, `ebitda`, `total_liabilities`, `common_equity`, `net_income_common`, `cash_and_st_investments`, `intangibles_total`, `total_dividends_paid`, `cfo`, `short_term_debt`. This task adds exactly two closed-dispatch branches to both engines. Waves A and B (Tasks 6-8) depend on them.

Semantics: emit the direct-alias value when a candidate for the rule's own `item_id` is visible at the revision event; otherwise emit the composition over `source_item_ids` (`sum`, or `input[0] - input[1]` for the difference form). `missing_policy` applies to the composition leg only.

**Files:**
- Modify: `C:\atx\atx-db\src\atx_db\standardization.py:30-31` (`COMBINATION_RULES`), `:225-228` (validation), `:357-392` (`_select_inputs`)
- Modify: `C:\atx\atx-db\src\atx_db\_standardization_set_based.py:505` (discrete-quarter routing), `:576` (`_std_direct` routing), `:670` (`_std_combinations` events), `:735-745` (final combination SELECT)
- Test: `C:\atx\atx-db\tests\test_standardization.py` (append)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: two new legal values of `standardization_rules.csv.combination_rule`: `coalesce_or_sum` and `coalesce_or_difference`. `coalesce_or_difference` requires exactly two `source_item_ids_json` entries; `coalesce_or_sum` requires at least one.

- [ ] **Step 1: Write the failing tests**

Append to `C:\atx\atx-db\tests\test_standardization.py`:

```python
def test_coalesce_or_difference_prefers_the_direct_tag():
    """Tier1-S2 T5: with GrossProfit reported, the composition is not used."""
    rules = (
        _rule(
            9101,
            combination_rule="coalesce_or_difference",
            source_item_ids=(9102, 9103),
        ),
    )
    inputs = pd.DataFrame(
        [
            _input_row(9101, "GrossProfit", 400.0),
            _input_row(9102, "Revenues", 1000.0),
            _input_row(9103, "CostOfRevenue", 700.0),
        ]
    )
    rows = compute_standardized_rows(inputs, rules=rules)

    assert list(rows["value"]) == [400.0]
    assert json.loads(rows["input_item_ids_json"].iloc[0]) == [9101]
    assert rows["combination_rule"].iloc[0] == "coalesce_or_difference"


def test_coalesce_or_difference_falls_back_to_the_composition():
    """Tier1-S2 T5: with no GrossProfit tag, revenue - cost_of_revenue is emitted."""
    rules = (
        _rule(
            9101,
            combination_rule="coalesce_or_difference",
            source_item_ids=(9102, 9103),
        ),
    )
    inputs = pd.DataFrame(
        [
            _input_row(9102, "Revenues", 1000.0),
            _input_row(9103, "CostOfRevenue", 700.0),
        ]
    )
    rows = compute_standardized_rows(inputs, rules=rules)

    assert list(rows["value"]) == [300.0]
    assert json.loads(rows["input_item_ids_json"].iloc[0]) == [9102, 9103]


def test_coalesce_or_sum_prefers_the_direct_tag_then_sums():
    rules = (_rule(9201, combination_rule="coalesce_or_sum", source_item_ids=(9202, 9203)),)

    direct = compute_standardized_rows(
        pd.DataFrame(
            [
                _input_row(9201, "PaymentsOfDividends", 90.0),
                _input_row(9202, "PaymentsOfDividendsCommonStock", 70.0),
                _input_row(9203, "PaymentsOfDividendsPreferredStock", 25.0),
            ]
        ),
        rules=rules,
    )
    assert list(direct["value"]) == [90.0]

    fallback = compute_standardized_rows(
        pd.DataFrame(
            [
                _input_row(9202, "PaymentsOfDividendsCommonStock", 70.0),
                _input_row(9203, "PaymentsOfDividendsPreferredStock", 25.0),
            ]
        ),
        rules=rules,
    )
    assert list(fallback["value"]) == [95.0]


def test_coalesce_or_difference_rejects_wrong_input_arity(tmp_path):
    seed = tmp_path / "standardization_rules.csv"
    with seed.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(RULE_COLUMNS)
        writer.writerow(
            [
                "std_annual_9999", "9999", "bad", "annual", "[]", "[1001]",
                "coalesce_or_difference", "statement_normalized", "identity",
                "skip", "true", "1900-01-01", "",
            ]
        )
    with pytest.raises(ValueError, match="requires exactly two source item ids"):
        read_standardization_rules(seed)


def test_set_based_fallback_composition_emits_gross_profit(tmp_store):
    """Tier1-S2 T5: the set-based engine honours the fallback too."""
    from atx_db._standardization_set_based import refresh_standardized_set_based
    from atx_db.standardization import (
        FundamentalStandardizationOptions,
        default_standardization_rules,
    )

    _seed_statement_points(
        tmp_store,
        [
            ("us-gaap", "Revenues", "revenue", 1001, 1000.0),
            ("us-gaap", "CostOfRevenue", "cost_of_revenue", 1003, 700.0),
        ],
    )
    outcome = refresh_standardized_set_based(
        tmp_store,
        FundamentalStandardizationOptions(symbols=("TST",)),
        default_standardization_rules(),
    )
    gross = tmp_store.con.execute(
        "SELECT value, combination_rule FROM fundamental_standardized WHERE item_id = 1004"
    ).fetchall()

    assert outcome.exception_row_count >= 0
    assert gross and gross[0][0] == pytest.approx(300.0)
    assert gross[0][1] == "coalesce_or_difference"
```

Add the two helpers this uses to the same file (next to the existing `_rule` / `_input_row` helpers), if they are not already present:

```python
def _seed_statement_points(store, rows):
    """Insert minimal annual statement points for security TST, FY2025."""
    import datetime as dt

    from atx_db.fundamental_statements import seed_fundamental_statement_map
    from atx_db.item_registry import seed_fundamental_item_registry

    seed_fundamental_item_registry(store)
    seed_fundamental_statement_map(store)
    for index, (taxonomy, concept, canonical_metric, item_id, value) in enumerate(rows):
        store.con.execute(
            """
            INSERT INTO fundamental_statement_points (
                statement_point_id, source, security_id, symbol, cik, taxonomy, concept,
                canonical_metric, item_id, unit, unit_type, period_type, period_start,
                period_end, fiscal_year, fiscal_period, accession_number, source_accession,
                filed_date, value, as_of_date, available_at, revision_group_id,
                revision_sequence, is_latest_revision, run_id, source_loaded_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """,
            [
                f"sp-{index}", "SEC companyfacts", "SEC-TST", "TST", "1", taxonomy, concept,
                canonical_metric, item_id, "USD", "monetary", "duration",
                dt.date(2025, 1, 1), dt.date(2025, 12, 31), 2025, "FY",
                "acc-1", "acc-1", dt.date(2026, 2, 1), value,
                dt.date(2025, 12, 31), dt.datetime(2026, 2, 1), f"rg-{index}", 1, True,
                None, dt.datetime(2026, 2, 1),
            ],
        )
```

Make sure the module already imports `csv`, `json`, `pytest`, `pandas as pd`, `RULE_COLUMNS` and `read_standardization_rules`; add any missing import at the top of the file.

- [ ] **Step 2: Run the tests to verify they fail**

```
.venv\Scripts\python.exe -m pytest tests/test_standardization.py -n 0 -q -k "coalesce_or"
```

Expected: 5 failures, the first being `ValueError: unknown combination_rule 'coalesce_or_difference'` raised from `_read_rule` / `_select_inputs`.

- [ ] **Step 3: Extend the pandas engine**

In `C:\atx\atx-db\src\atx_db\standardization.py`:

1. Replace the `COMBINATION_RULES` frozenset (line 30):

```python
COMBINATION_RULES = frozenset(
    {
        "coalesce_priority",
        "first_non_null",
        "identity",
        "sum",
        "difference",
        # Tier1-S2 T5: prefer the item's own alias, else compose from
        # source_item_ids. The engine allows one active rule per
        # (item_id, basis), so derived-if-missing needs its own rule kind.
        "coalesce_or_sum",
        "coalesce_or_difference",
    }
)
_COMPOSITION_RULES = frozenset({"sum", "difference", "coalesce_or_sum", "coalesce_or_difference"})
_DIFFERENCE_RULES = frozenset({"difference", "coalesce_or_difference"})
_DIRECT_FIRST_RULES = frozenset({"identity", "coalesce_priority", "first_non_null", "coalesce_or_sum", "coalesce_or_difference"})
```

2. Replace the two arity checks in `_read_rule` (lines 225-228):

```python
    if rule.combination_rule in _COMPOSITION_RULES and not rule.source_item_ids:
        raise ValueError(f"{seed_path} row {row_number}: {rule.combination_rule} requires source_item_ids_json")
    if rule.combination_rule in _DIFFERENCE_RULES and len(rule.source_item_ids) != 2:
        raise ValueError(f"{seed_path} row {row_number}: {rule.combination_rule} requires exactly two source item ids")
```

3. Replace `_select_inputs` (lines 357-392):

```python
def _select_inputs(
    candidates: Sequence[Mapping[str, Any]],
    rule: StandardizationRule,
) -> tuple[float, list[Mapping[str, Any]]] | None:
    if rule.combination_rule in _DIRECT_FIRST_RULES:
        direct = _best_direct_rows(candidates, rule)
        if direct:
            value = _normalize_value(direct[0].get("value"), rule)
            if value is not None:
                return value, [direct[0]]
        if rule.combination_rule not in _COMPOSITION_RULES:
            return None

    selected: list[Mapping[str, Any]] = []
    values: list[float] = []
    for item_id in rule.source_item_ids:
        row = _best_for_item(candidates, item_id)
        if row is None:
            if rule.missing_policy == "zero_fill":
                values.append(0.0)
                continue
            return None
        value = _normalize_value(row.get("value"), rule)
        if value is None:
            if rule.missing_policy == "zero_fill":
                values.append(0.0)
                continue
            return None
        selected.append(row)
        values.append(value)

    if not selected:
        return None
    if rule.combination_rule in _DIFFERENCE_RULES:
        return values[0] - values[1], selected
    return sum(values), selected
```

- [ ] **Step 4: Extend the set-based engine**

In `C:\atx\atx-db\src\atx_db\_standardization_set_based.py`, make four edits:

1. In `_create_discrete_quarters`, the `routed` CTE (line 505):

```sql
            WHERE rule.combination_rule IN (
                'identity', 'coalesce_priority', 'first_non_null',
                'coalesce_or_sum', 'coalesce_or_difference'
            )
```

2. In `_create_output`, the `_std_direct` `routed` CTE (line 576):

```sql
            WHERE r.combination_rule IN (
                'identity', 'coalesce_priority', 'first_non_null',
                'coalesce_or_sum', 'coalesce_or_difference'
            )
```

3. In `_std_combinations`, the `events` CTE (line 670):

```sql
            WHERE r.combination_rule IN (
                'sum', 'difference', 'coalesce_or_sum', 'coalesce_or_difference'
            )
```

4. In `_std_combinations`, change the difference-sign expression to cover both difference forms and wrap the aggregate so a fallback composition is suppressed when a direct row is already visible. Replace both `CASE WHEN combination_rule = 'difference' AND input_position = 2` occurrences with `CASE WHEN combination_rule IN ('difference', 'coalesce_or_difference') AND input_position = 2`, then replace the trailing

```sql
        FROM picked
        GROUP BY
            rule_id, item_id, canonical_code, basis, combination_rule, sign_multiplier,
            absolute_value, scale_multiplier, missing_policy, input_count, security_id,
            period_start, period_end, event_at
        HAVING count(DISTINCT input_position) = input_count
            OR missing_policy = 'zero_fill'
        """
```

with

```sql
        FROM picked
        GROUP BY
            rule_id, item_id, canonical_code, basis, combination_rule, sign_multiplier,
            absolute_value, scale_multiplier, missing_policy, input_count, security_id,
            period_start, period_end, event_at
        HAVING count(DISTINCT input_position) = input_count
            OR missing_policy = 'zero_fill'
        )
        SELECT *
        FROM aggregated agg
        WHERE agg.combination_rule IN ('sum', 'difference')
           OR NOT EXISTS (
                SELECT 1
                FROM _std_direct direct
                WHERE direct.rule_id = agg.rule_id
                  AND direct.security_id = agg.security_id
                  AND direct.period_end = agg.period_end
                  AND direct.available_at <= agg.available_at
           )
        """
```

and open the wrapping CTE by changing the line immediately before that final `SELECT`-with-`string_agg` from

```sql
        picked AS (
            SELECT * FROM visible_inputs WHERE input_rank_at_event = 1
        )
        SELECT
```

to

```sql
        picked AS (
            SELECT * FROM visible_inputs WHERE input_rank_at_event = 1
        ),
        aggregated AS (
        SELECT
```

- [ ] **Step 5: Run the tests**

```
.venv\Scripts\python.exe -m pytest tests/test_standardization.py -n 0 -q
```

Expected: all tests pass, including the 5 new `coalesce_or` tests and every pre-existing test unchanged. No pre-existing assertion is edited in this task: the two new rule kinds are not used by any committed seed row yet.

- [ ] **Step 6: Confirm the rule-set digest is unchanged**

```
.venv\Scripts\python.exe -c "import sys; sys.path.insert(0,'src'); from atx_db.standardization import read_standardization_rules; from atx_db._standardization_set_based import _rule_set_digest; print(_rule_set_digest(read_standardization_rules()))"
```

Expected: the same 64-hex digest recorded in Task 3 Step 4 - this task changes only the engine, not the seeds.

- [ ] **Step 7: Commit**

```bash
git add src/atx_db/standardization.py src/atx_db/_standardization_set_based.py tests/test_standardization.py
git commit -m "feat(db): add coalesce_or_sum and coalesce_or_difference combination rules

read_standardization_rules allows one active rule per (item_id, basis), so an
item could have a direct-alias rule OR a composition rule but never both. Every
derived-if-missing item in the Tier-1 catalog (gross_profit, ebitda,
total_liabilities, common_equity, net_income_common, cash_and_st_investments,
intangibles_total, total_dividends_paid, cfo, short_term_debt) needs both.

Both engines now prefer a visible direct-alias candidate and fall back to the
composition over source_item_ids. The set-based path suppresses the composition
with a NOT EXISTS against _std_direct at or before the same available_at, the
same pattern the discrete-quarter derivation already uses. No seed uses the new
kinds yet, so the rule-set digest is unchanged.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: Wave A-1 - income statement and cash flow alias depth

**Files:**
- Create: `C:\atx\atx-db\scripts\apply_alias_wave.py`
- Create: `C:\atx\atx-db\research\wave_a_income_cashflow_aliases.csv`
- Create: `C:\atx\atx-db\research\wave_a_income_cashflow_items.csv`
- Create: `C:\atx\atx-db\research\wave_a_income_cashflow_rules.csv`
- Modify (generated): `src/atx_db/seeds/statement_map.csv`, `src/atx_db/seeds/fundamental_items.csv`, `src/atx_db/seeds/standardization_rules.csv`, `src/atx_db/seeds/concept_map.csv`
- Modify: `C:\atx\atx-db\tests\test_item_registry.py` (`AUTHORIZED_ITEM_IDS`, the 235 count)
- Modify: `C:\atx\atx-db\tests\test_standardization.py::test_standardization_rule_seed_covers_template_items` (the 455/130 counts)
- Modify: `C:\atx\atx-db\tests\test_statement_map_seed.py` (`EXPECTED_STATEMENT_MAP_ROWS`)
- Test: `C:\atx\atx-db\tests\test_alias_depth.py`

**Interfaces:**
- Consumes: `scripts/normalize_fundamental_seeds.py` (Task 3), `coalesce_or_sum` / `coalesce_or_difference` (Task 5), `atx_db.statement_map_seed.write_statement_map_seed` (Task 1).
- Produces: `scripts/apply_alias_wave.py` with `main(argv) -> int` and flags `--aliases`, `--items`, `--rules`. Tasks 7 and 8 call it with their own wave files. Wave alias CSV columns are fixed: `item_id,taxonomy,concept,priority,statement_type,statement_section,canonical_metric,canonical_label,period_type,normal_balance,unit_type,industry_template,registry_alias,notes`; every attribute column except `item_id,taxonomy,concept,priority,registry_alias,notes` may be blank and is then inherited from the item's first existing active statement-map row.
- Produces: `tests/test_alias_depth.py` with `SPEC_ITEM_IDS: frozenset[int]` and `ALIAS_DEPTH_EXCEPTIONS: dict[int, tuple[int, str]]` (item_id -> (exact alias count, reason)). Tasks 7 and 8 extend both.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_alias_depth.py`:

```python
"""Tier1-S2: every Tier-1 spec item carries a deep alias set, or an explicit reason."""
from __future__ import annotations

from collections import Counter

from atx_db.item_registry import read_fundamental_item_seed
from atx_db.standardization import default_standardization_rules
from atx_db.statement_map_seed import read_statement_map_seed

MINIMUM_ALIASES = 3

# Wave A-1: income statement + cash flow. Task 7 adds the balance-sheet ids,
# Task 8 adds the industry-overlay ids.
SPEC_ITEM_IDS = frozenset(
    {
        1001, 1003, 1004, 1005, 1008, 1011, 1014, 1016, 1018, 1021, 1022, 1023,
        1024, 1027, 1029, 1030, 1031, 1032, 1033, 1034, 1035, 1040, 1041, 1051,
        1301, 1303, 1304, 1305, 1307, 1308, 1309, 1311, 1312, 1313, 1314, 1316,
        1318, 1322, 1324, 1327,
    }
)

# item_id -> (exact alias count, why us-gaap offers no more).
# An exact count, not a floor, so a silent regression fails the test.
ALIAS_DEPTH_EXCEPTIONS: dict[int, tuple[int, str]] = {
    1004: (1, "GrossProfit is the only us-gaap gross-profit element; the rest is composition from 1001-1003."),
    1005: (2, "us-gaap splits SG&A into SellingGeneralAndAdministrativeExpense and OtherSellingGeneralAndAdministrativeExpense; GeneralAndAdministrativeExpense belongs to item 1007."),
    1014: (1, "OperatingIncomeLoss is the only us-gaap operating-income element."),
    1016: (0, "EBITDA has no us-gaap element; composed from operating_income + cf_depreciation."),
    1023: (2, "the two us-gaap pretax elements; the Domestic/Foreign splits are components, not totals."),
    1024: (2, "IncomeTaxExpenseBenefit and its continuing-operations variant; Current/Deferred belong to 1025/1026."),
    1029: (2, "IncomeLossFromContinuingOperations and its including-NCI variant; ProfitLoss belongs to 1031."),
    1031: (2, "NetIncomeLoss and ProfitLoss; NetIncomeLossAvailableToCommonStockholdersBasic belongs to 1032."),
    1032: (2, "the basic and diluted available-to-common elements; the rest is composition from 1031-1033."),
    1034: (2, "EarningsPerShareBasic and EarningsPerShareBasicAndDiluted; the continuing-ops per-share element belongs to 1036."),
    1035: (1, "EarningsPerShareDiluted is the only diluted-EPS element not already owned by 1034 or 1037."),
    1041: (1, "WeightedAverageNumberOfDilutedSharesOutstanding is the only diluted weighted-average element."),
    1051: (2, "ExtraordinaryItemNetOfTax and ExtraordinaryItemGross; ASU 2015-01 eliminated the category, so nothing newer exists."),
    1301: (1, "NetCashProvidedByUsedInOperatingActivities; the continuing-operations element is item 1302, reached by the coalesce_or_sum fallback."),
    1303: (2, "the total and continuing-operations investing elements."),
    1304: (2, "the total and continuing-operations financing elements."),
    1307: (2, "DepreciationDepletionAndAmortization and DepreciationAmortizationAndAccretionNet; DepreciationAndAmortization belongs to item 1011."),
    1308: (2, "ShareBasedCompensation and AllocatedShareBasedCompensationExpense."),
    1316: (1, "PaymentsOfDividendsCommonStock is the only common-dividend payment element."),
    1318: (1, "PaymentsOfDividends; the rest is composition from 1316-1317."),
    1322: (0, "no us-gaap working-capital-change total; composed from 1319+1320+1321."),
    1327: (1, "DeferredIncomeTaxesAndTaxCredits is the only cash-flow deferred-tax element; the income-statement element belongs to 1026."),
}


def _alias_counts() -> Counter[int]:
    counts: Counter[int] = Counter()
    for row in read_fundamental_item_seed():
        if row.alias_scheme and row.alias_code:
            counts[row.item_id] += 1
    return counts


def test_every_spec_item_has_a_deep_alias_set_or_a_stated_reason():
    counts = _alias_counts()
    shallow = {
        item_id: counts.get(item_id, 0)
        for item_id in sorted(SPEC_ITEM_IDS)
        if counts.get(item_id, 0) < MINIMUM_ALIASES and item_id not in ALIAS_DEPTH_EXCEPTIONS
    }
    assert shallow == {}


def test_exception_counts_are_exact():
    counts = _alias_counts()
    drifted = {
        item_id: (counts.get(item_id, 0), expected)
        for item_id, (expected, _reason) in sorted(ALIAS_DEPTH_EXCEPTIONS.items())
        if counts.get(item_id, 0) != expected
    }
    assert drifted == {}


def test_every_exception_carries_a_reason():
    assert all(reason.strip() for _count, reason in ALIAS_DEPTH_EXCEPTIONS.values())


def test_every_exception_id_is_a_spec_item():
    assert set(ALIAS_DEPTH_EXCEPTIONS) <= set(SPEC_ITEM_IDS)


def test_zero_alias_spec_items_have_a_composition_rule():
    """An item with no alias must be reachable by source_item_ids composition."""
    counts = _alias_counts()
    rules_by_item: dict[int, set[str]] = {}
    for rule in default_standardization_rules():
        if rule.is_active and rule.source_item_ids:
            rules_by_item.setdefault(rule.item_id, set()).add(rule.combination_rule)
    orphans = sorted(
        item_id
        for item_id in SPEC_ITEM_IDS
        if counts.get(item_id, 0) == 0 and item_id not in rules_by_item
    )
    assert orphans == []


def test_every_registry_alias_has_a_statement_map_row():
    """An alias that is not in the statement map is never ingested."""
    mapped = {(row.taxonomy, row.concept) for row in read_statement_map_seed()}
    missing = sorted(
        f"{row.alias_scheme}:{row.alias_code}"
        for row in read_fundamental_item_seed()
        if row.alias_scheme in {"us-gaap", "dei"}
        and row.alias_code
        and (row.alias_scheme, row.alias_code) not in mapped
    )
    assert missing == []


def test_every_statement_map_alias_appears_in_its_rule_alias_json():
    """The rule JSON is the reviewable record of what each item accepts."""
    by_item: dict[int, set[tuple[str, str]]] = {}
    for row in read_statement_map_seed():
        if row.item_id is None or not row.is_active or row.is_derived:
            continue
        if row.taxonomy not in {"us-gaap", "dei"} or row.concept.startswith("__"):
            continue
        by_item.setdefault(int(row.item_id), set()).add((row.taxonomy, row.concept))

    declared: dict[int, set[tuple[str, str]]] = {}
    for rule in default_standardization_rules():
        if not rule.is_active:
            continue
        declared.setdefault(rule.item_id, set()).update(
            (alias.alias_scheme, alias.alias_code) for alias in rule.source_aliases
        )

    missing = {
        item_id: sorted(f"{s}:{c}" for s, c in aliases - declared.get(item_id, set()))
        for item_id, aliases in sorted(by_item.items())
        if item_id in SPEC_ITEM_IDS and aliases - declared.get(item_id, set())
    }
    assert missing == {}
```

- [ ] **Step 2: Run the test to verify it fails**

```
.venv\Scripts\python.exe -m pytest tests/test_alias_depth.py -n 0 -q
```

Expected: `test_every_spec_item_has_a_deep_alias_set_or_a_stated_reason` FAILS listing roughly 20 items at 0, 1 or 2 aliases (for example `{1008: 1, 1018: 1, 1022: 2, 1030: 1, 1051: 0, 1327: 0, ...}`), and `test_exception_counts_are_exact` FAILS because items 1051 and 1327 do not exist yet.

- [ ] **Step 3: Write the wave applier**

Create `C:\atx\atx-db\scripts\apply_alias_wave.py`:

```python
#!/usr/bin/env python
"""Merge a curated alias wave into the fundamentals seeds, idempotently.

Inputs (all CSV, all committed under research/ for provenance):

  --aliases  item_id,taxonomy,concept,priority,statement_type,statement_section,
             canonical_metric,canonical_label,period_type,normal_balance,
             unit_type,industry_template,registry_alias,notes
             Attribute columns may be blank; they are then inherited from the
             item's first existing active statement-map row. registry_alias is
             'true' unless the concept must stay template-scoped: a concept that
             already belongs to a different item under the ALL template cannot
             also live in fundamental_items.csv, which has no template column and
             whose uniqueness is enforced by item_registry._validate_aliases.
  --items    exactly the fundamental_items.csv columns - new canonical items.
  --rules    exactly the standardization_rules.csv columns - new or replacement
             rules. A rule whose rule_id already exists is replaced in place.

Effects, all additive:
  1. new item rows appended to seeds/fundamental_items.csv
  2. new rule rows appended to (or replacing) seeds/standardization_rules.csv
  3. one seeds/statement_map.csv row per wave alias
  4. one seeds/fundamental_items.csv alias row per wave alias with registry_alias=true
  5. the alias appended to source_aliases_json of every active rule for that item
  6. scripts/normalize_fundamental_seeds.py applied

Re-running the same wave is a no-op.
"""
from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "src"))

from atx_db.fundamental_statements import SOURCE_NAME
from atx_db.item_registry import SEED_COLUMNS as ITEM_SEED_COLUMNS
from atx_db.item_registry import SEED_PATH as ITEM_SEED_PATH
from atx_db.standardization import RULE_COLUMNS, RULE_PATH
from atx_db.statement_map_seed import (
    STATEMENT_MAP_SEED_PATH,
    FundamentalStatementMapRow,
    read_statement_map_seed,
    write_statement_map_seed,
)

WAVE_COLUMNS = (
    "item_id",
    "taxonomy",
    "concept",
    "priority",
    "statement_type",
    "statement_section",
    "canonical_metric",
    "canonical_label",
    "period_type",
    "normal_balance",
    "unit_type",
    "industry_template",
    "registry_alias",
    "notes",
)
INHERITED_COLUMNS = (
    "statement_type",
    "statement_section",
    "canonical_metric",
    "canonical_label",
    "period_type",
    "normal_balance",
    "unit_type",
    "industry_template",
)
NORMALIZER = PROJECT_ROOT / "scripts" / "normalize_fundamental_seeds.py"


def _read_csv(path: Path, columns: tuple[str, ...]) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if tuple(reader.fieldnames or ()) != columns:
            raise SystemExit(f"{path} has unexpected columns: {reader.fieldnames}")
        return list(reader)


def _write_csv(path: Path, columns: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(columns))
        writer.writeheader()
        writer.writerows(rows)


def _truthy(value: str) -> bool:
    return value.strip().lower() in {"1", "true", "t", "yes", "y", ""}


def _inherit(wave_row: dict[str, str], template: FundamentalStatementMapRow | None) -> dict[str, str]:
    resolved = dict(wave_row)
    for column in INHERITED_COLUMNS:
        if resolved[column].strip():
            continue
        if template is None:
            raise SystemExit(
                f"item_id={wave_row['item_id']} concept={wave_row['concept']}: "
                f"{column} is blank and the item has no existing statement-map row to inherit from"
            )
        resolved[column] = str(getattr(template, column))
    return resolved


def apply_wave(aliases_path: Path, items_path: Path | None, rules_path: Path | None) -> dict[str, int]:
    item_rows = _read_csv(ITEM_SEED_PATH, ITEM_SEED_COLUMNS)
    rule_rows = _read_csv(RULE_PATH, RULE_COLUMNS)
    map_rows = list(read_statement_map_seed())

    added_items = 0
    if items_path is not None:
        existing = {(r["item_id"], r["alias_scheme"], r["alias_code"], r["vendor"], r["vendor_field"]) for r in item_rows}
        for row in _read_csv(items_path, ITEM_SEED_COLUMNS):
            key = (row["item_id"], row["alias_scheme"], row["alias_code"], row["vendor"], row["vendor_field"])
            if key in existing:
                continue
            item_rows.append(row)
            existing.add(key)
            added_items += 1

    added_rules = 0
    if rules_path is not None:
        by_id = {row["rule_id"]: index for index, row in enumerate(rule_rows)}
        for row in _read_csv(rules_path, RULE_COLUMNS):
            if row["rule_id"] in by_id:
                rule_rows[by_id[row["rule_id"]]] = row
            else:
                by_id[row["rule_id"]] = len(rule_rows)
                rule_rows.append(row)
            added_rules += 1

    item_attributes: dict[str, dict[str, str]] = {}
    for row in item_rows:
        item_attributes.setdefault(row["item_id"], row)

    template_by_item: dict[int, FundamentalStatementMapRow] = {}
    for row in map_rows:
        if row.item_id is not None and row.is_active and int(row.item_id) not in template_by_item:
            template_by_item[int(row.item_id)] = row

    map_keys = {(r.source, r.taxonomy, r.concept, r.industry_template) for r in map_rows}
    alias_keys = {(r["item_id"], r["alias_scheme"], r["alias_code"]) for r in item_rows if r["alias_scheme"].strip()}

    added_map = 0
    added_alias = 0
    touched_rules = 0
    for wave_row in _read_csv(aliases_path, WAVE_COLUMNS):
        item_id = int(wave_row["item_id"])
        resolved = _inherit(wave_row, template_by_item.get(item_id))
        key = (SOURCE_NAME, resolved["taxonomy"], resolved["concept"], resolved["industry_template"])
        if key not in map_keys:
            map_rows.append(
                FundamentalStatementMapRow(
                    source=SOURCE_NAME,
                    taxonomy=resolved["taxonomy"],
                    concept=resolved["concept"],
                    statement_type=resolved["statement_type"],
                    statement_section=resolved["statement_section"],
                    canonical_metric=resolved["canonical_metric"],
                    canonical_label=resolved["canonical_label"],
                    period_type=resolved["period_type"],
                    normal_balance=resolved["normal_balance"],
                    unit_type=resolved["unit_type"],
                    value_multiplier=1.0,
                    concept_priority=int(resolved["priority"]),
                    is_core_metric=True,
                    is_active=True,
                    notes=resolved["notes"] or None,
                    item_id=item_id,
                    industry_template=resolved["industry_template"],
                    is_derived=False,
                    derivation_expr=None,
                )
            )
            map_keys.add(key)
            added_map += 1

        if _truthy(resolved["registry_alias"]):
            alias_key = (resolved["item_id"], resolved["taxonomy"], resolved["concept"])
            if alias_key not in alias_keys:
                base = item_attributes[resolved["item_id"]]
                alias_row = {column: "" for column in ITEM_SEED_COLUMNS}
                for column in ("item_id", "canonical_code", "statement", "section", "data_type",
                               "unit_type", "sign_convention", "is_derived", "definition", "citation"):
                    alias_row[column] = base[column]
                alias_row["alias_scheme"] = resolved["taxonomy"]
                alias_row["alias_code"] = resolved["concept"]
                alias_row["coalesce_priority"] = resolved["priority"]
                alias_row["valid_from"] = "1900-01-01"
                alias_row["valid_to"] = ""
                item_rows.append(alias_row)
                alias_keys.add(alias_key)
                added_alias += 1

        for rule_row in rule_rows:
            if int(rule_row["item_id"]) != item_id or rule_row["is_active"].strip().lower() != "true":
                continue
            aliases = json.loads(rule_row["source_aliases_json"] or "[]")
            if any(a["alias_scheme"] == resolved["taxonomy"] and a["alias_code"] == resolved["concept"] for a in aliases):
                continue
            aliases.append(
                {
                    "alias_code": resolved["concept"],
                    "alias_scheme": resolved["taxonomy"],
                    "priority": int(resolved["priority"]),
                }
            )
            rule_row["source_aliases_json"] = json.dumps(aliases, separators=(",", ":"), sort_keys=True)
            touched_rules += 1

    _write_csv(ITEM_SEED_PATH, ITEM_SEED_COLUMNS, item_rows)
    _write_csv(RULE_PATH, RULE_COLUMNS, rule_rows)
    write_statement_map_seed(map_rows, STATEMENT_MAP_SEED_PATH)
    subprocess.run([sys.executable, str(NORMALIZER)], cwd=str(PROJECT_ROOT), check=True, capture_output=True)

    return {
        "items_added": added_items,
        "rules_added_or_replaced": added_rules,
        "statement_map_rows_added": added_map,
        "registry_alias_rows_added": added_alias,
        "rule_alias_json_updates": touched_rules,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aliases", type=Path, required=True)
    parser.add_argument("--items", type=Path)
    parser.add_argument("--rules", type=Path)
    args = parser.parse_args(argv)
    print(json.dumps(apply_wave(args.aliases, args.items, args.rules), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Write the Wave A-1 new-item rows**

Create `C:\atx\atx-db\research\wave_a_income_cashflow_items.csv` (columns are exactly the 18 of `fundamental_items.csv`):

```csv
item_id,canonical_code,statement,section,data_type,unit_type,sign_convention,is_derived,definition,citation,alias_scheme,alias_code,coalesce_priority,valid_from,valid_to,vendor,vendor_field,sign_note
1051,extraordinary_items,income,income_statement,duration,monetary,positive,false,Extraordinary items and discontinued operations net of tax,Tier-1 parity design 2026-09-19 income statement catalog (cs xido),,,,1900-01-01,,compustat,xido,
1052,employees,income,supplemental,instant,count,positive,false,Total employee headcount reported on the annual cover page,Tier-1 parity design 2026-09-19 supplemental catalog,,,,1900-01-01,,compustat,emp,
1327,deferred_tax_cash_flow,cashflow,cash_flow,duration,monetary,positive,false,Deferred income taxes as a cash-flow reconciling item,Tier-1 parity design 2026-09-19 cash flow catalog (cs txdc),,,,1900-01-01,,compustat,txdc,
```

- [ ] **Step 5: Write the Wave A-1 rule rows**

Create `C:\atx\atx-db\research\wave_a_income_cashflow_rules.csv` (columns exactly the 13 of `standardization_rules.csv`). The first six rows are for the two new ruled items; the remaining 21 replace existing rules in place to install the composition fallbacks:

```csv
rule_id,item_id,canonical_code,basis,source_aliases_json,source_item_ids_json,combination_rule,sign_rule,scale_rule,missing_policy,is_active,valid_from,valid_to
std_annual_1051,1051,extraordinary_items,annual,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1051,1051,extraordinary_items,quarterly,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1051,1051,extraordinary_items,ttm,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_annual_1327,1327,deferred_tax_cash_flow,annual,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1327,1327,deferred_tax_cash_flow,quarterly,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1327,1327,deferred_tax_cash_flow,ttm,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_annual_1004,1004,gross_profit__1004,annual,"[{""alias_code"":""GrossProfit"",""alias_scheme"":""us-gaap"",""priority"":10}]","[1001,1003]",coalesce_or_difference,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1004,1004,gross_profit__1004,quarterly,"[{""alias_code"":""GrossProfit"",""alias_scheme"":""us-gaap"",""priority"":10}]","[1001,1003]",coalesce_or_difference,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1004,1004,gross_profit__1004,ttm,"[{""alias_code"":""GrossProfit"",""alias_scheme"":""us-gaap"",""priority"":10}]","[1001,1003]",coalesce_or_difference,statement_normalized,identity,skip,true,1900-01-01,
std_annual_1016,1016,ebitda_standardised,annual,[],"[1014,1307]",sum,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1016,1016,ebitda_standardised,quarterly,[],"[1014,1307]",sum,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1016,1016,ebitda_standardised,ttm,[],"[1014,1307]",sum,statement_normalized,identity,skip,true,1900-01-01,
std_annual_1032,1032,net_income_to_common,annual,"[{""alias_code"":""NetIncomeLossAvailableToCommonStockholdersBasic"",""alias_scheme"":""us-gaap"",""priority"":10}]","[1031,1033]",coalesce_or_difference,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1032,1032,net_income_to_common,quarterly,"[{""alias_code"":""NetIncomeLossAvailableToCommonStockholdersBasic"",""alias_scheme"":""us-gaap"",""priority"":10}]","[1031,1033]",coalesce_or_difference,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1032,1032,net_income_to_common,ttm,"[{""alias_code"":""NetIncomeLossAvailableToCommonStockholdersBasic"",""alias_scheme"":""us-gaap"",""priority"":10}]","[1031,1033]",coalesce_or_difference,statement_normalized,identity,skip,true,1900-01-01,
std_annual_1301,1301,cash_flow_from_operations,annual,"[{""alias_code"":""NetCashProvidedByUsedInOperatingActivities"",""alias_scheme"":""us-gaap"",""priority"":10}]",[1302],coalesce_or_sum,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1301,1301,cash_flow_from_operations,quarterly,"[{""alias_code"":""NetCashProvidedByUsedInOperatingActivities"",""alias_scheme"":""us-gaap"",""priority"":10}]",[1302],coalesce_or_sum,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1301,1301,cash_flow_from_operations,ttm,"[{""alias_code"":""NetCashProvidedByUsedInOperatingActivities"",""alias_scheme"":""us-gaap"",""priority"":10}]",[1302],coalesce_or_sum,statement_normalized,identity,skip,true,1900-01-01,
std_annual_1318,1318,total_dividends_paid,annual,"[{""alias_code"":""PaymentsOfDividends"",""alias_scheme"":""us-gaap"",""priority"":10}]","[1316,1317]",coalesce_or_sum,statement_normalized,identity,zero_fill,true,1900-01-01,
std_quarterly_1318,1318,total_dividends_paid,quarterly,"[{""alias_code"":""PaymentsOfDividends"",""alias_scheme"":""us-gaap"",""priority"":10}]","[1316,1317]",coalesce_or_sum,statement_normalized,identity,zero_fill,true,1900-01-01,
std_ttm_1318,1318,total_dividends_paid,ttm,"[{""alias_code"":""PaymentsOfDividends"",""alias_scheme"":""us-gaap"",""priority"":10}]","[1316,1317]",coalesce_or_sum,statement_normalized,identity,zero_fill,true,1900-01-01,
std_annual_1322,1322,change_in_working_capital,annual,[],"[1319,1320,1321]",sum,invert,identity,zero_fill,true,1900-01-01,
std_quarterly_1322,1322,change_in_working_capital,quarterly,[],"[1319,1320,1321]",sum,invert,identity,zero_fill,true,1900-01-01,
std_ttm_1322,1322,change_in_working_capital,ttm,[],"[1319,1320,1321]",sum,invert,identity,zero_fill,true,1900-01-01,
std_annual_1325,1325,free_cash_flow__1325,annual,[],"[1301,1305]",difference,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1325,1325,free_cash_flow__1325,quarterly,[],"[1301,1305]",difference,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1325,1325,free_cash_flow__1325,ttm,[],"[1301,1305]",difference,statement_normalized,identity,skip,true,1900-01-01,
```

The `1322` rows use `sign_rule=invert` because the us-gaap `IncreaseDecreaseIn*` elements are stated as the increase in the asset or liability, while the Compustat `wcapc` convention is the cash-flow effect of that movement.

- [ ] **Step 6: Write the Wave A-1 alias rows**

Create `C:\atx\atx-db\research\wave_a_income_cashflow_aliases.csv`. Attribute columns are blank on every row except those for items 1051 and 1327, which have no statement-map row to inherit from:

```csv
item_id,taxonomy,concept,priority,statement_type,statement_section,canonical_metric,canonical_label,period_type,normal_balance,unit_type,industry_template,registry_alias,notes
1001,us-gaap,RevenueFromContractWithCustomerIncludingAssessedTax,15,,,,,,,,,true,ASC-606 revenue including assessed tax
1001,us-gaap,SalesRevenueGoodsNet,40,,,,,,,,,true,Pre-ASC-606 product revenue
1001,us-gaap,SalesRevenueServicesNet,50,,,,,,,,,true,Pre-ASC-606 service revenue
1001,us-gaap,RevenuesNetOfInterestExpense,60,,,,,,,,,true,Broker and bank style total revenue presentation
1003,us-gaap,CostOfServices,40,,,,,,,,,true,Service-only cost of revenue
1003,us-gaap,CostOfGoodsAndServiceExcludingDepreciationDepletionAndAmortization,50,,,,,,,,,true,Cost of revenue excluding D and A
1003,us-gaap,CostOfGoodsSoldExcludingDepreciationDepletionAndAmortization,60,,,,,,,,,true,Legacy cost of goods sold excluding D and A
1005,us-gaap,OtherSellingGeneralAndAdministrativeExpense,20,,,,,,,,,true,Residual SG and A line
1006,us-gaap,SellingExpense,20,,,,,,,,,true,Selling expense only
1007,us-gaap,OtherGeneralAndAdministrativeExpense,20,,,,,,,,,true,Residual G and A line
1008,us-gaap,ResearchAndDevelopmentExpenseExcludingAcquiredInProcessCost,20,,,,,,,,,true,R and D excluding acquired in-process R and D
1008,us-gaap,ResearchAndDevelopmentExpenseSoftwareExcludingAcquiredInProcessCost,30,,,,,,,,,true,Software R and D
1009,us-gaap,MarketingAndAdvertisingExpense,20,,,,,,,,,true,Combined marketing and advertising
1010,us-gaap,CostsAndExpenses,20,,,,,,,,,true,Total costs and expenses
1010,us-gaap,OperatingCostsAndExpenses,30,,,,,,,,,true,Total operating costs and expenses
1011,us-gaap,DepreciationDepletionAndAmortizationNonproduction,20,,,,,,,,,true,Non-production D and A on the income statement
1011,us-gaap,CostOfGoodsAndServicesSoldDepreciationAndAmortization,30,,,,,,,,,true,D and A embedded in cost of sales
1012,us-gaap,DepreciationNonproduction,20,,,,,,,,,true,Non-production depreciation only
1013,us-gaap,FiniteLivedIntangibleAssetsAmortizationExpense,20,,,,,,,,,true,Finite-lived intangible amortization
1018,us-gaap,InterestExpenseNonoperating,20,,,,,,,,,true,Nonoperating interest expense
1018,us-gaap,InterestAndDebtExpense,30,,,,,,,,,true,Combined interest and debt expense
1019,us-gaap,InterestExpenseLongTermDebt,20,,,,,,,,,true,Long-term debt interest only
1020,us-gaap,InvestmentIncomeInterestAndDividend,30,,,,,,,,,true,Combined interest and dividend investment income
1022,us-gaap,GoodwillAndIntangibleAssetImpairmentCharge,30,,,,,,,,,true,Goodwill and intangible impairment
1022,us-gaap,RestructuringSettlementAndImpairmentProvisions,40,,,,,,,,,true,Combined restructuring settlement and impairment
1022,us-gaap,GainLossOnSaleOfBusiness,50,,,,,,,,,true,Gain or loss on business disposal
1023,us-gaap,IncomeLossFromContinuingOperationsBeforeIncomeTaxesMinorityInterestAndIncomeLossFromEquityMethodInvestments,20,,,,,,,,,true,Pretax income before equity-method results
1024,us-gaap,IncomeTaxExpenseBenefitContinuingOperations,20,,,,,,,,,true,Tax expense on continuing operations
1026,us-gaap,DeferredOtherTaxExpenseBenefit,20,,,,,,,,,true,Other deferred tax expense on the income statement
1027,us-gaap,NetIncomeLossAttributableToRedeemableNoncontrollingInterest,30,,,,,,,,,true,Redeemable NCI share of earnings
1029,us-gaap,IncomeLossFromContinuingOperationsIncludingPortionAttributableToNoncontrollingInterest,20,,,,,,,,,true,Continuing operations including NCI
1030,us-gaap,IncomeLossFromDiscontinuedOperationsNetOfTaxAttributableToReportingEntity,20,,,,,,,,,true,Discontinued operations attributable to the parent
1030,us-gaap,DiscontinuedOperationIncomeLossFromDiscontinuedOperationNetOfTax,30,,,,,,,,,true,Per-disposal discontinued operations result
1031,us-gaap,ProfitLoss,20,,,,,,,,,true,Net income including noncontrolling interests
1032,us-gaap,NetIncomeLossAvailableToCommonStockholdersDiluted,20,,,,,,,,,true,Diluted income available to common
1033,us-gaap,PreferredStockDividendsAndOtherAdjustments,20,,,,,,,,,true,Preferred dividends plus other adjustments
1033,us-gaap,PreferredStockDividendsIncomeStatementImpact,30,,,,,,,,,true,Income-statement impact of preferred dividends
1034,us-gaap,EarningsPerShareBasicAndDiluted,20,,,,,,,,,true,Single combined EPS line
1039,us-gaap,CommonStockSharesIssued,30,,,,,,,,,true,Shares issued fallback for the period-end share count
1040,us-gaap,WeightedAverageNumberOfSharesIssuedBasic,20,,,,,,,,,true,Weighted-average shares issued
1051,us-gaap,ExtraordinaryItemNetOfTax,10,income_statement,income,extraordinary_items,Extraordinary items,duration,credit,monetary,ALL,true,Extraordinary items net of tax
1051,us-gaap,ExtraordinaryItemGross,20,income_statement,income,extraordinary_items,Extraordinary items,duration,credit,monetary,ALL,true,Extraordinary items before tax
1303,us-gaap,NetCashProvidedByUsedInInvestingActivitiesContinuingOperations,20,,,,,,,,,true,Investing cash flow from continuing operations
1304,us-gaap,NetCashProvidedByUsedInFinancingActivitiesContinuingOperations,20,,,,,,,,,true,Financing cash flow from continuing operations
1305,us-gaap,PaymentsForCapitalImprovements,20,,,,,,,,,true,Capital improvements
1305,us-gaap,PaymentsToAcquireMachineryAndEquipment,30,,,,,,,,,true,Machinery and equipment purchases
1305,us-gaap,PaymentsToAcquireOtherPropertyPlantAndEquipment,40,,,,,,,,,true,Other PP and E purchases
1306,us-gaap,PaymentsToAcquireIntangibleAssets,20,,,,,,,,,true,Intangible asset purchases
1306,us-gaap,PaymentsToDevelopSoftware,30,,,,,,,,,true,Capitalized software development spend
1307,us-gaap,DepreciationAmortizationAndAccretionNet,20,,,,,,,,,true,D and A plus accretion
1308,us-gaap,AllocatedShareBasedCompensationExpense,20,,,,,,,,,true,Allocated share-based compensation expense
1309,us-gaap,PaymentsToAcquireBusinessesGross,20,,,,,,,,,true,Gross business acquisition payments
1309,us-gaap,PaymentsToAcquireBusinessesAndInterestInAffiliates,30,,,,,,,,,true,Acquisitions including affiliate interests
1310,us-gaap,ProceedsFromDivestitureOfBusinessesNetOfCashDivested,20,,,,,,,,,true,Divestiture proceeds net of cash divested
1310,us-gaap,ProceedsFromSaleOfProductiveAssets,30,,,,,,,,,true,Productive asset sale proceeds
1311,us-gaap,ProceedsFromIssuanceOrSaleOfEquity,20,,,,,,,,,true,Equity issuance or sale proceeds
1311,us-gaap,ProceedsFromStockOptionsExercised,30,,,,,,,,,true,Option exercise proceeds
1312,us-gaap,PaymentsForRepurchaseOfEquity,20,,,,,,,,,true,Equity repurchase payments
1312,us-gaap,TreasuryStockValueAcquiredCostMethod,30,,,,,,,,,true,Treasury stock acquired at cost
1313,us-gaap,ProceedsFromNotesPayable,20,,,,,,,,,true,Notes payable proceeds
1313,us-gaap,ProceedsFromIssuanceOfSeniorLongTermDebt,30,,,,,,,,,true,Senior long-term debt proceeds
1313,us-gaap,ProceedsFromLinesOfCredit,40,,,,,,,,,true,Line of credit draws
1314,us-gaap,RepaymentsOfNotesPayable,20,,,,,,,,,true,Notes payable repayments
1314,us-gaap,RepaymentsOfSeniorDebt,30,,,,,,,,,true,Senior debt repayments
1314,us-gaap,RepaymentsOfLinesOfCredit,40,,,,,,,,,true,Line of credit repayments
1319,us-gaap,IncreaseDecreaseInReceivables,20,,,,,,,,,true,Total receivables movement
1321,us-gaap,IncreaseDecreaseInAccountsPayableTrade,20,,,,,,,,,true,Trade payables movement
1321,us-gaap,IncreaseDecreaseInAccountsPayableAndAccruedLiabilities,30,,,,,,,,,true,Payables and accrued liabilities movement
1323,us-gaap,EffectOfExchangeRateOnCashAndCashEquivalents,20,,,,,,,,,true,Pre-ASU-2016-18 FX effect on cash
1324,us-gaap,CashAndCashEquivalentsPeriodIncreaseDecrease,20,,,,,,,,,true,Pre-ASU-2016-18 change in cash
1324,us-gaap,CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalentsPeriodIncreaseDecreaseExcludingExchangeRateEffect,30,,,,,,,,,true,Change in cash excluding the FX effect
1327,us-gaap,DeferredIncomeTaxesAndTaxCredits,10,cash_flow,operating,deferred_tax_cash_flow,Deferred income taxes (cash flow),duration,debit,monetary,ALL,true,Deferred taxes as a cash-flow reconciling item
```

`TotalRevenuesAndOtherIncome` from the charter revenue list is **deliberately excluded**: it is a company extension concept, not a `us-gaap` element, so it can never be ingested or aliased. `InterestIncome` on item 1020 is likewise a legacy non-standard element, but it is already committed and the "never delete" constraint keeps it.

- [ ] **Step 7: Apply the wave**

```
.venv\Scripts\python.exe scripts\apply_alias_wave.py --aliases research\wave_a_income_cashflow_aliases.csv --items research\wave_a_income_cashflow_items.csv --rules research\wave_a_income_cashflow_rules.csv
```

Expected stdout, exactly:

```json
{
  "items_added": 3,
  "rules_added_or_replaced": 27,
  "statement_map_rows_added": 73,
  "registry_alias_rows_added": 73,
  "rule_alias_json_updates": 219
}
```

(`rule_alias_json_updates` is 73 aliases times the 3 active bases each of their items carries.)

- [ ] **Step 8: Verify the wave is idempotent**

```
.venv\Scripts\python.exe scripts\apply_alias_wave.py --aliases research\wave_a_income_cashflow_aliases.csv --items research\wave_a_income_cashflow_items.csv --rules research\wave_a_income_cashflow_rules.csv
git diff --stat src/atx_db/seeds/
```

Expected: the second run reports `statement_map_rows_added: 0`, `registry_alias_rows_added: 0`, `rule_alias_json_updates: 0`, and `git diff --stat` is identical to after the first run.

- [ ] **Step 9: Update the pinned counts**

In `tests/test_item_registry.py` replace `AUTHORIZED_ITEM_IDS` with:

```python
AUTHORIZED_ITEM_IDS = (
    set(range(1001, 1044))
    | set(range(1045, 1053))          # Tier1-S2 T6: +1051 extraordinary_items, +1052 employees
    | set(range(1101, 1120))
    | set(range(1201, 1225))
    | set(range(1301, 1326))
    | {1327}                          # Tier1-S2 T6: deferred_tax_cash_flow (1326 stays unauthorized)
    | set(range(1401, 1428))
    | set(range(1501, 1516))
    | set(range(1601, 1611))
    | set(range(1701, 1713))
    | set(range(1801, 1806))
    | set(range(1901, 1906))
    | set(range(2001, 2045))
)
```

and change `assert len(seeded_item_ids) == 235` and `assert item_count == 235` to `238`. `UNAUTHORIZED_GAP_ITEM_IDS = {1044, 1099, 1326}` is unchanged: 1326 is still never used.

In `tests/test_standardization.py::test_standardization_rule_seed_covers_template_items`:

```python
    # Tier1-S2 T6: +3 annual/quarterly/ttm rules for 1051 extraordinary_items and
    # +3 for 1327 deferred_tax_cash_flow. instant is untouched by Wave A-1.
    assert len(rules) == 461
    assert len(by_basis["annual"]) == 132
    assert len(by_basis["quarterly"]) == 132
    assert len(by_basis["ttm"]) == 132
    assert len(by_basis["instant"]) == 65
```

and replace the `revenue` alias assertion in the same test with the Wave A-1 ordering:

```python
    assert [alias.alias_code for alias in revenue.source_aliases] == [
        "RevenueFromContractWithCustomerExcludingAssessedTax",
        "RevenueFromContractWithCustomerIncludingAssessedTax",
        "Revenues",
        "SalesRevenueNet",
        "SalesRevenueGoodsNet",
        "SalesRevenueServicesNet",
        "RevenuesNetOfInterestExpense",
    ]
```

In `tests/test_statement_map_seed.py` set `EXPECTED_STATEMENT_MAP_ROWS = 287` (214 + 73).

- [ ] **Step 10: Run the full seed test set**

```
.venv\Scripts\python.exe -m pytest tests/test_alias_depth.py -n 0 -q
.venv\Scripts\python.exe -m pytest tests/test_seed_determinism.py tests/test_statement_map_seed.py tests/test_ingest_allowlist.py tests/test_item_registry.py tests/test_standardization.py tests/test_concept_coverage.py tests/test_industry_templates.py -n 0 -q
```

Expected: `7 passed`, then all passed. If `test_no_alias_code_maps_to_two_items_in_the_item_seed` fails, a wave row claimed a concept another item already owns - fix the wave CSV, never the guard.

- [ ] **Step 11: Commit**

```bash
git add scripts/apply_alias_wave.py research/wave_a_income_cashflow_aliases.csv research/wave_a_income_cashflow_items.csv research/wave_a_income_cashflow_rules.csv src/atx_db/seeds tests/test_alias_depth.py tests/test_item_registry.py tests/test_standardization.py tests/test_statement_map_seed.py
git commit -m "feat(db): Wave A-1 income-statement and cash-flow alias depth

73 curated us-gaap aliases across 40 income and cash-flow items, three new items
(1051 extraordinary_items, 1052 employees, 1327 deferred_tax_cash_flow), and
seven composition rules on the new coalesce_or_* kinds: gross_profit falls back
to revenue - cost_of_revenue, net_income_common to net_income - preferred
dividends, cfo to continuing-operations cfo, total_dividends_paid to common +
preferred, plus ebitda, free_cash_flow and working_capital_change.

Pinned counts updated with justification: 235 -> 238 items, 455 -> 461 rules,
214 -> 287 statement-map rows. Wave input CSVs are committed under research/ so
the merge is reproducible; scripts/apply_alias_wave.py is idempotent.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: Wave A-2 - balance sheet alias depth and derived-if-missing compositions

**Files:**
- Create: `C:\atx\atx-db\research\wave_a_balance_aliases.csv`
- Create: `C:\atx\atx-db\research\wave_a_balance_items.csv`
- Create: `C:\atx\atx-db\research\wave_a_balance_rules.csv`
- Modify (generated): `src/atx_db/seeds/statement_map.csv`, `src/atx_db/seeds/fundamental_items.csv`, `src/atx_db/seeds/standardization_rules.csv`, `src/atx_db/seeds/concept_map.csv`
- Modify: `C:\atx\atx-db\tests\test_alias_depth.py` (extend `SPEC_ITEM_IDS` and `ALIAS_DEPTH_EXCEPTIONS`)
- Modify: `C:\atx\atx-db\tests\test_item_registry.py`, `tests\test_standardization.py`, `tests\test_statement_map_seed.py` (pinned counts)

**Interfaces:**
- Consumes: `scripts/apply_alias_wave.py` (Task 6), `coalesce_or_sum` / `coalesce_or_difference` (Task 5).
- Produces: nothing new in code; the deliverable is seed content plus the widened `SPEC_ITEM_IDS`.

- [ ] **Step 1: Extend the failing test**

In `C:\atx\atx-db\tests\test_alias_depth.py`, add the balance-sheet ids to `SPEC_ITEM_IDS` so the frozenset becomes:

```python
SPEC_ITEM_IDS = frozenset(
    {
        # Wave A-1 income statement + cash flow
        1001, 1003, 1004, 1005, 1008, 1011, 1014, 1016, 1018, 1021, 1022, 1023,
        1024, 1027, 1029, 1030, 1031, 1032, 1033, 1034, 1035, 1040, 1041, 1051,
        1301, 1303, 1304, 1305, 1307, 1308, 1309, 1311, 1312, 1313, 1314, 1316,
        1318, 1322, 1324, 1327,
        # Wave A-2 balance sheet
        1039, 1101, 1102, 1104, 1105, 1106, 1107, 1109, 1110, 1111, 1112, 1114,
        1115, 1117, 1119, 1120, 1201, 1202, 1203, 1205, 1207, 1211, 1212, 1213,
        1214, 1217, 1219, 1220, 1221, 1225, 1226, 1227,
    }
)
```

and add these entries to `ALIAS_DEPTH_EXCEPTIONS`:

```python
    1101: (1, "Assets is the only us-gaap total-assets element."),
    1102: (1, "AssetsCurrent is the only us-gaap current-assets element."),
    1109: (1, "OtherAssetsCurrent is the only us-gaap other-current-assets element."),
    1110: (2, "PropertyPlantAndEquipmentNet plus the finance-lease-ROU-inclusive variant."),
    1111: (1, "PropertyPlantAndEquipmentGross is the only us-gaap gross PP&E element."),
    1112: (1, "AccumulatedDepreciationDepletionAndAmortizationPropertyPlantAndEquipment is the only accumulated-depreciation element."),
    1114: (2, "Goodwill and GoodwillGross; the goodwill-plus-intangibles total belongs to 1113."),
    1119: (2, "OtherAssetsNoncurrent and OtherAssets."),
    1120: (2, "CapitalizedComputerSoftwareNet and CapitalizedComputerSoftwareGross; the accumulated-amortization element is a contra account, not the asset."),
    1201: (1, "Liabilities; the rest is composition from total_liab_equity - equity_incl_NCI."),
    1202: (1, "LiabilitiesCurrent is the only us-gaap current-liabilities element."),
    1211: (2, "DeferredIncomeTaxLiabilitiesNet and DeferredTaxLiabilitiesNoncurrent."),
    1212: (2, "OtherLiabilitiesNoncurrent and OtherLiabilities."),
    1213: (1, "MinorityInterest is the only balance-sheet noncontrolling-interest element; the including-NCI equity total belongs to 1222."),
    1214: (2, "PreferredStockValue and PreferredStockLiquidationPreferenceValue."),
    1217: (1, "RetainedEarningsAccumulatedDeficit is the only retained-earnings element."),
    1219: (2, "TreasuryStockValue and TreasuryStockCommonValue."),
    1220: (0, "no us-gaap common-equity element; composed from stockholders_equity - preferred_stock."),
    1221: (1, "StockholdersEquity; the including-NCI element belongs to 1222 and is reached by the coalesce_or_difference fallback."),
    1226: (2, "OtherLiabilitiesCurrent and OtherAccruedLiabilitiesCurrent."),
```

- [ ] **Step 2: Run the test to verify it fails**

```
.venv\Scripts\python.exe -m pytest tests/test_alias_depth.py -n 0 -q
```

Expected: `test_every_spec_item_has_a_deep_alias_set_or_a_stated_reason` FAILS with roughly `{1105: 2, 1106: 2, 1107: 1, 1115: 1, 1117: 2, 1120: 0, 1203: 1, 1205: 0, 1207: 1, 1225: 0, 1226: 0, 1227: 0}` and `test_exception_counts_are_exact` FAILS on 1110, 1114, 1119, 1120, 1211, 1212, 1214, 1219, 1226.

- [ ] **Step 3: Write the Wave A-2 new-item rows**

Create `C:\atx\atx-db\research\wave_a_balance_items.csv`:

```csv
item_id,canonical_code,statement,section,data_type,unit_type,sign_convention,is_derived,definition,citation,alias_scheme,alias_code,coalesce_priority,valid_from,valid_to,vendor,vendor_field,sign_note
1120,capitalized_software,balance,assets,instant,monetary,positive,false,Capitalized internal-use and external-use computer software net of amortization,Tier-1 parity design 2026-09-19 supplemental catalog (capitalized_software),,,,1900-01-01,,compustat,capsft,
1225,taxes_payable,balance,liabilities,instant,monetary,positive,false,Income and other taxes payable within one year,Tier-1 parity design 2026-09-19 balance sheet catalog (cs txp),,,,1900-01-01,,compustat,txp,
1226,other_current_liabilities,balance,liabilities,instant,monetary,positive,false,Other current liabilities not separately classified,Tier-1 parity design 2026-09-19 balance sheet catalog (cs lco),,,,1900-01-01,,compustat,lco,
1227,finance_lease_liabilities,balance,liabilities,instant,monetary,positive,false,Finance lease liabilities current and noncurrent,Tier-1 parity design 2026-09-19 supplemental catalog (finance_lease_liabilities),,,,1900-01-01,,bloomberg,BS_FINANCE_LEASE_LIABILITIES,
```

- [ ] **Step 4: Write the Wave A-2 rule rows**

Create `C:\atx\atx-db\research\wave_a_balance_rules.csv`. Four new `instant` rules plus six in-place replacements that install the derived-if-missing compositions:

```csv
rule_id,item_id,canonical_code,basis,source_aliases_json,source_item_ids_json,combination_rule,sign_rule,scale_rule,missing_policy,is_active,valid_from,valid_to
std_instant_1120,1120,capitalized_software,instant,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_instant_1225,1225,taxes_payable,instant,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_instant_1226,1226,other_current_liabilities,instant,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_instant_1227,1227,finance_lease_liabilities,instant,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_instant_1103,1103,cash_and_st_investments,instant,[],"[1104,1105]",coalesce_or_sum,statement_normalized,identity,zero_fill,true,1900-01-01,
std_instant_1113,1113,intangibles_total,instant,[],"[1114,1115]",coalesce_or_sum,statement_normalized,identity,zero_fill,true,1900-01-01,
std_instant_1201,1201,total_liabilities,instant,"[{""alias_code"":""Liabilities"",""alias_scheme"":""us-gaap"",""priority"":10}]","[1223,1222]",coalesce_or_difference,statement_normalized,identity,skip,true,1900-01-01,
std_instant_1205,1205,short_term_debt,instant,[],[1206],coalesce_or_sum,statement_normalized,identity,skip,true,1900-01-01,
std_instant_1220,1220,common_equity,instant,[],"[1221,1214]",coalesce_or_difference,statement_normalized,identity,zero_fill,true,1900-01-01,
std_instant_1221,1221,stockholders_equity,instant,"[{""alias_code"":""StockholdersEquity"",""alias_scheme"":""us-gaap"",""priority"":10}]","[1222,1213]",coalesce_or_difference,statement_normalized,identity,skip,true,1900-01-01,
```

`1103` and `1113` use `missing_policy=zero_fill` so a filer that reports only cash (no short-term investments) or only goodwill (no other intangibles) still gets the total. `1220` uses `zero_fill` so a company with no preferred stock gets `common_equity = stockholders_equity`.

- [ ] **Step 5: Write the Wave A-2 alias rows**

Create `C:\atx\atx-db\research\wave_a_balance_aliases.csv`:

```csv
item_id,taxonomy,concept,priority,statement_type,statement_section,canonical_metric,canonical_label,period_type,normal_balance,unit_type,industry_template,registry_alias,notes
1103,us-gaap,CashCashEquivalentsAndShortTermInvestments,10,balance_sheet,assets,cash_and_st_investments,Cash and short-term investments,instant,debit,monetary,ALL,true,Combined cash and short-term investments total
1103,us-gaap,CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalents,20,,,,,,,,,true,Post-ASU-2016-18 cash total including restricted cash
1104,us-gaap,CashAndDueFromBanks,30,,,,,,,,,true,Bank presentation of cash on hand and due from banks
1105,us-gaap,AvailableForSaleSecuritiesCurrent,30,,,,,,,,,true,Current available-for-sale securities
1105,us-gaap,OtherShortTermInvestments,40,,,,,,,,,true,Other short-term investments
1106,us-gaap,AccountsAndNotesReceivableNet,30,,,,,,,,,true,Combined accounts and notes receivable
1106,us-gaap,AccountsReceivableGrossCurrent,40,,,,,,,,,true,Gross current receivables fallback
1107,us-gaap,InventoryGross,20,,,,,,,,,true,Gross inventory before reserves
1107,us-gaap,InventoryNetOfAllowancesCustomerAdvancesAndProgressBillings,30,,,,,,,,,true,Contract-accounting inventory presentation
1108,us-gaap,PrepaidExpenseAndOtherAssetsCurrent,20,,,,,,,,,true,Prepaid expenses combined with other current assets
1110,us-gaap,PropertyPlantAndEquipmentAndFinanceLeaseRightOfUseAssetAfterAccumulatedDepreciationAndAmortization,20,,,,,,,,,true,Net PP and E including finance-lease right-of-use assets
1113,us-gaap,IntangibleAssetsNetIncludingGoodwill,10,balance_sheet,assets,intangibles_total,Total intangibles including goodwill,instant,debit,monetary,ALL,true,Combined intangibles including goodwill
1114,us-gaap,GoodwillGross,20,,,,,,,,,true,Goodwill before impairment
1115,us-gaap,FiniteLivedIntangibleAssetsNet,20,,,,,,,,,true,Finite-lived intangibles net
1115,us-gaap,IndefiniteLivedIntangibleAssetsExcludingGoodwill,30,,,,,,,,,true,Indefinite-lived intangibles excluding goodwill
1117,us-gaap,AvailableForSaleSecuritiesNoncurrent,30,,,,,,,,,true,Noncurrent available-for-sale securities
1117,us-gaap,MarketableSecuritiesNoncurrent,40,,,,,,,,,true,Noncurrent marketable securities
1118,us-gaap,DeferredTaxAssetsNetNoncurrent,20,,,,,,,,,true,Noncurrent net deferred tax assets
1119,us-gaap,OtherAssets,20,,,,,,,,,true,Unclassified other assets
1120,us-gaap,CapitalizedComputerSoftwareNet,10,balance_sheet,assets,capitalized_software,Capitalized software,instant,debit,monetary,ALL,true,Capitalized software net of amortization
1120,us-gaap,CapitalizedComputerSoftwareGross,20,balance_sheet,assets,capitalized_software,Capitalized software,instant,debit,monetary,ALL,true,Capitalized software before amortization
1203,us-gaap,AccountsPayableAndAccruedLiabilitiesCurrent,20,,,,,,,,,true,Payables combined with accrued liabilities
1203,us-gaap,AccountsPayableTradeCurrent,30,,,,,,,,,true,Trade payables only
1204,us-gaap,EmployeeRelatedLiabilitiesCurrent,20,,,,,,,,,true,Accrued compensation and benefits
1205,us-gaap,ShortTermBorrowings,10,balance_sheet,liabilities,short_term_debt,Short-term debt,instant,credit,monetary,ALL,true,Short-term borrowings
1205,us-gaap,OtherShortTermBorrowings,20,,,,,,,,,true,Other short-term borrowings
1205,us-gaap,CommercialPaper,30,,,,,,,,,true,Commercial paper outstanding
1205,us-gaap,DebtCurrent,40,,,,,,,,,true,Total current debt
1206,us-gaap,LongTermDebtAndCapitalLeaseObligationsCurrent,20,,,,,,,,,true,Current maturities including capital leases
1207,us-gaap,LongTermDebtAndCapitalLeaseObligations,20,,,,,,,,,true,Long-term debt including capital lease obligations
1207,us-gaap,LongTermNotesPayable,30,,,,,,,,,true,Long-term notes payable
1207,us-gaap,SeniorNotes,40,,,,,,,,,true,Senior notes outstanding
1209,us-gaap,OperatingLeaseLiabilityNoncurrent,10,balance_sheet,liabilities,operating_lease_liability,Operating lease liabilities,instant,credit,monetary,ALL,true,Noncurrent operating lease liability
1209,us-gaap,OperatingLeaseLiability,20,,,,,,,,,true,Total operating lease liability
1209,us-gaap,OperatingLeaseLiabilityCurrent,30,,,,,,,,,true,Current operating lease liability
1210,us-gaap,DeferredRevenueCurrent,20,,,,,,,,,true,Current deferred revenue
1210,us-gaap,ContractWithCustomerLiabilityCurrent,30,,,,,,,,,true,ASC-606 current contract liability
1210,us-gaap,ContractWithCustomerLiability,40,,,,,,,,,true,ASC-606 total contract liability
1211,us-gaap,DeferredTaxLiabilitiesNoncurrent,20,,,,,,,,,true,Noncurrent deferred tax liabilities
1212,us-gaap,OtherLiabilities,20,,,,,,,,,true,Unclassified other liabilities
1214,us-gaap,PreferredStockLiquidationPreferenceValue,20,,,,,,,,,true,Preferred stock at liquidation preference
1215,us-gaap,CommonStocksIncludingAdditionalPaidInCapital,20,,,,,,,,,true,Common stock combined with APIC
1219,us-gaap,TreasuryStockCommonValue,20,,,,,,,,,true,Common treasury stock at cost
1224,us-gaap,RedeemableNoncontrollingInterestEquityCarryingAmount,20,,,,,,,,,true,Redeemable NCI carrying amount
1224,us-gaap,TemporaryEquityCarryingAmount,30,,,,,,,,,true,Temporary equity carrying amount
1224,us-gaap,RedeemablePreferredStockCarryingAmount,40,,,,,,,,,true,Redeemable preferred carrying amount
1224,us-gaap,TemporaryEquityRedemptionValue,50,,,,,,,,,true,Temporary equity at redemption value
1224,us-gaap,RedeemableNoncontrollingInterestEquityPreferredCarryingAmount,60,,,,,,,,,true,Redeemable preferred NCI carrying amount
1225,us-gaap,TaxesPayableCurrent,10,balance_sheet,liabilities,taxes_payable,Taxes payable,instant,credit,monetary,ALL,true,Current taxes payable
1225,us-gaap,AccruedIncomeTaxesCurrent,20,balance_sheet,liabilities,taxes_payable,Taxes payable,instant,credit,monetary,ALL,true,Accrued current income taxes
1225,us-gaap,AccruedIncomeTaxes,30,balance_sheet,liabilities,taxes_payable,Taxes payable,instant,credit,monetary,ALL,true,Total accrued income taxes
1226,us-gaap,OtherLiabilitiesCurrent,10,balance_sheet,liabilities,other_current_liabilities,Other current liabilities,instant,credit,monetary,ALL,true,Other current liabilities
1226,us-gaap,OtherAccruedLiabilitiesCurrent,20,balance_sheet,liabilities,other_current_liabilities,Other current liabilities,instant,credit,monetary,ALL,true,Other accrued current liabilities
1227,us-gaap,FinanceLeaseLiabilityNoncurrent,10,balance_sheet,liabilities,finance_lease_liabilities,Finance lease liabilities,instant,credit,monetary,ALL,true,Noncurrent finance lease liability
1227,us-gaap,FinanceLeaseLiability,20,balance_sheet,liabilities,finance_lease_liabilities,Finance lease liabilities,instant,credit,monetary,ALL,true,Total finance lease liability
1227,us-gaap,FinanceLeaseLiabilityCurrent,30,balance_sheet,liabilities,finance_lease_liabilities,Finance lease liabilities,instant,credit,monetary,ALL,true,Current finance lease liability
```

The five `1224` rows mirror aliases that already exist in `std_instant_1224.source_aliases_json` but were never added to `fundamental_items.csv` or the statement map, so they have never been ingested. This is exactly the drift `test_every_statement_map_alias_appears_in_its_rule_alias_json` now prevents.

- [ ] **Step 6: Apply the wave**

```
.venv\Scripts\python.exe scripts\apply_alias_wave.py --aliases research\wave_a_balance_aliases.csv --items research\wave_a_balance_items.csv --rules research\wave_a_balance_rules.csv
```

Expected stdout:

```json
{
  "items_added": 4,
  "rules_added_or_replaced": 10,
  "statement_map_rows_added": 56,
  "registry_alias_rows_added": 56,
  "rule_alias_json_updates": 56
}
```

(One update per alias: balance-sheet items carry a single `instant` rule.)

- [ ] **Step 7: Update the pinned counts**

In `tests/test_item_registry.py`:

```python
AUTHORIZED_ITEM_IDS = (
    set(range(1001, 1044))
    | set(range(1045, 1053))
    | set(range(1101, 1121))          # Tier1-S2 T7: +1120 capitalized_software
    | set(range(1201, 1228))          # Tier1-S2 T7: +1225 taxes_payable, +1226 other_current_liabilities, +1227 finance_lease_liabilities
    | set(range(1301, 1326))
    | {1327}
    | set(range(1401, 1428))
    | set(range(1501, 1516))
    | set(range(1601, 1611))
    | set(range(1701, 1713))
    | set(range(1801, 1806))
    | set(range(1901, 1906))
    | set(range(2001, 2045))
)
```

and change the two `238` assertions to `242`.

In `tests/test_standardization.py::test_standardization_rule_seed_covers_template_items`:

```python
    # Tier1-S2 T7: +4 instant rules (1120, 1225, 1226, 1227).
    assert len(rules) == 465
    assert len(by_basis["annual"]) == 132
    assert len(by_basis["quarterly"]) == 132
    assert len(by_basis["ttm"]) == 132
    assert len(by_basis["instant"]) == 69
```

The same test asserts `total_debt.source_item_ids == (1205, 1207)` and `temporary_equity` alias ordering; both still hold - `std_instant_1208` is untouched and the 1224 alias list is unchanged in content, only mirrored into the other two seeds.

In `tests/test_statement_map_seed.py` set `EXPECTED_STATEMENT_MAP_ROWS = 343` (287 + 56).

- [ ] **Step 8: Run the test set**

```
.venv\Scripts\python.exe -m pytest tests/test_alias_depth.py tests/test_seed_determinism.py -n 0 -q
.venv\Scripts\python.exe -m pytest tests/test_statement_map_seed.py tests/test_ingest_allowlist.py tests/test_item_registry.py tests/test_standardization.py tests/test_concept_coverage.py tests/test_industry_templates.py tests/test_fundamental_concept_dictionary.py -n 0 -q
```

Expected: all passed.

- [ ] **Step 9: Commit**

```bash
git add research/wave_a_balance_aliases.csv research/wave_a_balance_items.csv research/wave_a_balance_rules.csv src/atx_db/seeds tests/test_alias_depth.py tests/test_item_registry.py tests/test_standardization.py tests/test_statement_map_seed.py
git commit -m "feat(db): Wave A-2 balance-sheet alias depth and derived-if-missing totals

56 curated us-gaap aliases across 30 balance-sheet items, four new items
(1120 capitalized_software, 1225 taxes_payable, 1226 other_current_liabilities,
1227 finance_lease_liabilities), and six compositions: cash_and_st_investments
= cash + short-term investments, intangibles_total = goodwill + other,
total_liabilities = total_liab_equity - equity_incl_NCI, common_equity =
stockholders_equity - preferred, stockholders_equity falls back to
equity_incl_NCI - minority_interest, short_term_debt falls back to current
maturities of long-term debt.

Also mirrors the five temporary_equity (1224) aliases that existed only in the
rule JSON into the statement map and the item registry, so they are ingested for
the first time.

Pinned counts updated with justification: 238 -> 242 items, 461 -> 465 rules,
287 -> 343 statement-map rows.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: Wave B - bank, insurer, REIT and utility templates with real content

The audit measured 47 of the 54 industry-specific items with no tag mapping at all, so the industry surface is structurally present and empty. This task fills it and wires the new items into `industry_templates.TEMPLATE_ITEMS`.

**Correction to the charter:** entity classification does **not** read SIC from `sec_submissions`. `industry_templates.refresh_entity_industry_templates` reads `entity_classification` joined to `taxonomy` where `taxonomy.code = 'SIC'`, bounded by `valid_from`/`valid_to` and stamped with `available_at`. `template_for_sic` maps 6000-6199 to `BK`, 6200-6299 to `BD`, 6300-6411 to `IS`, 6798 to `RT`, 4900-4999 to `UT`, everything else to `ALL`. Do not change that routing; this task only adds content to the templates it already produces.

**Files:**
- Create: `C:\atx\atx-db\research\wave_b_industry_aliases.csv`
- Create: `C:\atx\atx-db\research\wave_b_industry_items.csv`
- Create: `C:\atx\atx-db\research\wave_b_industry_rules.csv`
- Modify: `C:\atx\atx-db\src\atx_db\industry_templates.py` (`TEMPLATE_ITEMS`, around line 68)
- Modify (generated): the four seed CSVs
- Modify: `C:\atx\atx-db\tests\test_alias_depth.py`, `tests\test_industry_templates.py`, `tests\test_item_registry.py`, `tests\test_standardization.py`, `tests\test_statement_map_seed.py`

**Interfaces:**
- Consumes: `scripts/apply_alias_wave.py` (Task 6), `coalesce_or_sum` / `coalesce_or_difference` (Task 5).
- Produces: 16 new `IndustryTemplateItem` rows (46 total) and 7 new canonical items (249 total).

- [ ] **Step 1: Extend the failing tests**

In `tests/test_alias_depth.py`, add the industry ids to `SPEC_ITEM_IDS`:

```python
        # Wave B industry overlays + all-company supplemental
        1052, 1209, 1501, 1503, 1504, 1505, 1506, 1509, 1510, 1516, 1517,
        1601, 1603, 1604, 1611, 1701, 1713, 1714, 1806, 1807,
```

and add to `ALIAS_DEPTH_EXCEPTIONS`:

```python
    1052: (0, "neither us-gaap nor dei exposes an employee count as a numeric fact; it exists only inside text blocks, so the item is published as not_available."),
    1501: (2, "InterestIncomeExpenseNet and the after-provision variant; the rest is composition from interest income - interest expense."),
    1601: (3, "PremiumsEarnedNet plus the property-casualty and life variants."),
    1604: (2, "PolicyholderBenefitsAndClaimsIncurredNet and BenefitsLossesAndExpenses."),
    1611: (2, "NetInvestmentIncome and GrossInvestmentIncomeOperating."),
    1701: (1, "FFO is a Nareit non-GAAP measure with no us-gaap element; the nareit alias is retained and the value is composed from net_income + cf_depreciation."),
    1713: (3, "OperatingLeaseLeaseIncome plus the two legacy rental-revenue elements."),
    1714: (2, "RealEstateInvestmentPropertyNet and RealEstateInvestmentPropertyAtCost; the accumulated-depreciation element is a contra account."),
```

Note `1052` must also be excluded from `test_zero_alias_spec_items_have_a_composition_rule`; add the explicit allowance to that test:

```python
NOT_AVAILABLE_FROM_XBRL = frozenset({1052})


def test_zero_alias_spec_items_have_a_composition_rule():
    counts = _alias_counts()
    rules_by_item: dict[int, set[str]] = {}
    for rule in default_standardization_rules():
        if rule.is_active and rule.source_item_ids:
            rules_by_item.setdefault(rule.item_id, set()).add(rule.combination_rule)
    orphans = sorted(
        item_id
        for item_id in SPEC_ITEM_IDS
        if counts.get(item_id, 0) == 0
        and item_id not in rules_by_item
        and item_id not in NOT_AVAILABLE_FROM_XBRL
    )
    assert orphans == []
```

Append to `C:\atx\atx-db\tests\test_industry_templates.py`:

```python
def test_wave_b_template_items_are_registered(tmp_store):
    """Tier1-S2 T8: every industry template carries its Wave B required items."""
    from atx_db.industry_templates import seed_industry_templates

    seed_industry_templates(tmp_store)
    rows = tmp_store.con.execute(
        "SELECT template_code, item_id, not_available FROM industry_template_item ORDER BY template_code, item_id"
    ).fetchall()
    by_template: dict[str, set[int]] = {}
    not_available: set[int] = set()
    for template_code, item_id, flag in rows:
        by_template.setdefault(str(template_code), set()).add(int(item_id))
        if flag:
            not_available.add(int(item_id))

    assert {1505, 1506, 1510, 1516, 1517} <= by_template["BK"]
    assert {1603, 1604, 1611} <= by_template["IS"]
    assert {1713, 1714} <= by_template["RT"]
    assert {1806, 1807} <= by_template["UT"]
    assert {1052, 1120, 1209, 1227} <= by_template["ALL"]
    assert 1052 in not_available
    assert 1510 not in not_available
    assert 1604 not in not_available
    assert len(rows) == 46


def test_wave_b_industry_aliases_route_under_their_template():
    """Bank/insurer/REIT/utility aliases carry the right industry_template."""
    from atx_db.statement_map_seed import read_statement_map_seed

    by_item: dict[int, set[str]] = {}
    for row in read_statement_map_seed():
        if row.item_id is None:
            continue
        by_item.setdefault(int(row.item_id), set()).add(row.industry_template)

    assert by_item[1516] == {"BK"}
    assert by_item[1517] == {"BK"}
    assert by_item[1611] == {"IS"}
    assert by_item[1713] == {"RT"}
    assert by_item[1714] == {"RT"}
    assert by_item[1806] == {"UT"}
    assert by_item[1807] == {"UT"}


def test_sic_routing_is_unchanged_by_wave_b():
    from atx_db.industry_templates import template_for_sic

    assert template_for_sic(6021) == ("BK", "sic_6000_6199_bank")
    assert template_for_sic(6211) == ("BD", "sic_6200_6299_broker_dealer")
    assert template_for_sic(6311) == ("IS", "sic_6300_6411_insurance")
    assert template_for_sic(6798) == ("RT", "sic_6798_reit")
    assert template_for_sic(4911) == ("UT", "sic_4900_4999_utility")
    assert template_for_sic(3571) == ("ALL", "default_all")
    assert template_for_sic(None) == ("ALL", "default_all")
```

- [ ] **Step 2: Run the tests to verify they fail**

```
.venv\Scripts\python.exe -m pytest tests/test_alias_depth.py tests/test_industry_templates.py -n 0 -q
```

Expected: `test_wave_b_template_items_are_registered` FAILS with `KeyError` or `assert len(rows) == 46` seeing 30; `test_every_spec_item_has_a_deep_alias_set_or_a_stated_reason` FAILS listing `{1503: 1, 1504: 0, 1505: 1, 1506: 0, 1509: 1, 1510: 0, 1516: 0, 1517: 0, 1603: 0, 1611: 0, 1713: 0, 1714: 0, 1806: 0, 1807: 0}` (item 1209 already reached three aliases in Task 7).

- [ ] **Step 3: Write the Wave B new-item rows**

Create `C:\atx\atx-db\research\wave_b_industry_items.csv`:

```csv
item_id,canonical_code,statement,section,data_type,unit_type,sign_convention,is_derived,definition,citation,alias_scheme,alias_code,coalesce_priority,valid_from,valid_to,vendor,vendor_field,sign_note
1516,noninterest_income,bank,bank_statement,duration,monetary,positive,false,Total noninterest income for depository institutions,Tier-1 parity design 2026-09-19 supplemental bank catalog,,,,1900-01-01,,compustat,nii,
1517,noninterest_expense,bank,bank_statement,duration,monetary,positive,false,Total noninterest expense for depository institutions,Tier-1 parity design 2026-09-19 supplemental bank catalog,,,,1900-01-01,,compustat,nie,
1611,investment_income_insurance,insurance,insurance_statement,duration,monetary,positive,false,Net investment income earned on the insurance investment portfolio,Tier-1 parity design 2026-09-19 supplemental insurer catalog,,,,1900-01-01,,compustat,xiinv,
1713,rental_revenue,reit,reit_statement,duration,monetary,positive,false,Rental and lease revenue from real-estate operations,Tier-1 parity design 2026-09-19 supplemental REIT catalog,,,,1900-01-01,,compustat,revt_rent,
1714,real_estate_investments_net,reit,reit_statement,instant,monetary,positive,false,Real-estate investment property net of accumulated depreciation,Tier-1 parity design 2026-09-19 supplemental REIT catalog,,,,1900-01-01,,compustat,ppent_re,
1806,regulatory_assets,utility,utility_statement,instant,monetary,positive,false,Regulatory assets recoverable through future rates,Tier-1 parity design 2026-09-19 utilities prose (regulated revenue and regulatory assets),,,,1900-01-01,,compustat,regasset,
1807,regulatory_liabilities,utility,utility_statement,instant,monetary,positive,false,Regulatory liabilities refundable through future rates,Tier-1 parity design 2026-09-19 utilities prose (regulated revenue and regulatory assets),,,,1900-01-01,,compustat,regliab,
```

- [ ] **Step 4: Write the Wave B rule rows**

Create `C:\atx\atx-db\research\wave_b_industry_rules.csv`. Fifteen new rules plus nine in-place replacements:

```csv
rule_id,item_id,canonical_code,basis,source_aliases_json,source_item_ids_json,combination_rule,sign_rule,scale_rule,missing_policy,is_active,valid_from,valid_to
std_annual_1516,1516,noninterest_income,annual,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1516,1516,noninterest_income,quarterly,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1516,1516,noninterest_income,ttm,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_annual_1517,1517,noninterest_expense,annual,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1517,1517,noninterest_expense,quarterly,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1517,1517,noninterest_expense,ttm,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_annual_1611,1611,investment_income_insurance,annual,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1611,1611,investment_income_insurance,quarterly,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1611,1611,investment_income_insurance,ttm,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_annual_1713,1713,rental_revenue,annual,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1713,1713,rental_revenue,quarterly,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1713,1713,rental_revenue,ttm,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_instant_1714,1714,real_estate_investments_net,instant,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_instant_1806,1806,regulatory_assets,instant,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_instant_1807,1807,regulatory_liabilities,instant,[],[],coalesce_priority,statement_normalized,identity,skip,true,1900-01-01,
std_annual_1501,1501,net_interest_income,annual,[],"[1503,1504]",coalesce_or_difference,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1501,1501,net_interest_income,quarterly,[],"[1503,1504]",coalesce_or_difference,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1501,1501,net_interest_income,ttm,[],"[1503,1504]",coalesce_or_difference,statement_normalized,identity,skip,true,1900-01-01,
std_annual_1701,1701,funds_from_operations_ffo,annual,"[{""alias_code"":""FundsFromOperations"",""alias_scheme"":""nareit"",""priority"":10}]","[1031,1307]",coalesce_or_sum,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1701,1701,funds_from_operations_ffo,quarterly,"[{""alias_code"":""FundsFromOperations"",""alias_scheme"":""nareit"",""priority"":10}]","[1031,1307]",coalesce_or_sum,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1701,1701,funds_from_operations_ffo,ttm,"[{""alias_code"":""FundsFromOperations"",""alias_scheme"":""nareit"",""priority"":10}]","[1031,1307]",coalesce_or_sum,statement_normalized,identity,skip,true,1900-01-01,
std_annual_1804,1804,utility_operating_income,annual,[],[1014],coalesce_or_sum,statement_normalized,identity,skip,true,1900-01-01,
std_quarterly_1804,1804,utility_operating_income,quarterly,[],[1014],coalesce_or_sum,statement_normalized,identity,skip,true,1900-01-01,
std_ttm_1804,1804,utility_operating_income,ttm,[],[1014],coalesce_or_sum,statement_normalized,identity,skip,true,1900-01-01,
```

**Known limitation, carried to Sprint 3:** `1701` FFO is composed as `net_income + cf_depreciation`, i.e. FFO *before* the Nareit gain-on-sale adjustment. `sum` and `difference` take a flat signed list and `difference` is fixed at two inputs, so the three-term signed form `net_income + D&A - gains_on_sale` is not expressible. Adding a `gain_on_sale_of_real_estate` item and a signed n-ary composition is Sprint 3 work; the `notes` column on the 1701 statement-map row records this.

- [ ] **Step 5: Write the Wave B alias rows**

Create `C:\atx\atx-db\research\wave_b_industry_aliases.csv`:

```csv
item_id,taxonomy,concept,priority,statement_type,statement_section,canonical_metric,canonical_label,period_type,normal_balance,unit_type,industry_template,registry_alias,notes
1501,us-gaap,InterestIncomeExpenseNet,10,bank_statement,interest,net_interest_income,Net interest income,duration,credit,monetary,BK,true,Net interest income
1501,us-gaap,InterestIncomeExpenseAfterProvisionForLoanLoss,20,bank_statement,interest,net_interest_income,Net interest income,duration,credit,monetary,BK,true,Net interest income after provision
1503,us-gaap,InterestAndFeeIncomeLoansAndLeases,20,,,,,,,,,true,Loan and lease interest and fee income
1503,us-gaap,InterestAndFeeIncomeLoansAndLeasesHeldInPortfolio,30,,,,,,,,,true,Held-in-portfolio loan interest and fee income
1504,us-gaap,InterestExpenseDeposits,10,bank_statement,interest,interest_expense_bank,Bank interest expense,duration,debit,monetary,BK,true,Interest expense on deposits
1504,us-gaap,InterestExpenseBorrowings,20,bank_statement,interest,interest_expense_bank,Bank interest expense,duration,debit,monetary,BK,true,Interest expense on borrowings
1504,us-gaap,InterestExpenseFederalFundsPurchasedAndSecuritiesSoldUnderAgreementsToRepurchase,30,bank_statement,interest,interest_expense_bank,Bank interest expense,duration,debit,monetary,BK,true,Interest expense on fed funds and repo
1505,us-gaap,ProvisionForLoanLeaseAndOtherLosses,20,,,,,,,,,true,Provision for loan lease and other losses
1505,us-gaap,ProvisionForLoanLossesExpensed,30,,,,,,,,,true,Provision for loan losses expensed
1505,us-gaap,ProvisionForDoubtfulAccounts,40,,,,,,,,,true,Provision for doubtful accounts
1506,us-gaap,FinancingReceivableAllowanceForCreditLosses,10,bank_statement,credit_quality,allowance_for_loan_and_lease_losses,Allowance for loan and lease losses,instant,credit,monetary,BK,true,CECL financing-receivable allowance
1506,us-gaap,LoansAndLeasesReceivableAllowance,20,bank_statement,credit_quality,allowance_for_loan_and_lease_losses,Allowance for loan and lease losses,instant,credit,monetary,BK,true,Pre-CECL loan and lease allowance
1506,us-gaap,AllowanceForDoubtfulAccountsReceivable,30,bank_statement,credit_quality,allowance_for_loan_and_lease_losses,Allowance for loan and lease losses,instant,credit,monetary,BK,true,General doubtful-accounts allowance
1509,us-gaap,NotesReceivableNet,20,,,,,,,,,true,Net notes receivable
1509,us-gaap,LoansAndLeasesReceivableGrossCarryingAmount,30,,,,,,,,,true,Gross loans and leases
1509,us-gaap,FinancingReceivableExcludingAccruedInterestAfterAllowanceForCreditLoss,40,,,,,,,,,true,CECL financing receivables net of allowance
1510,us-gaap,Deposits,10,bank_statement,funding,total_deposits,Total deposits,instant,credit,monetary,BK,true,Total deposits
1510,us-gaap,InterestBearingDepositLiabilities,20,bank_statement,funding,total_deposits,Total deposits,instant,credit,monetary,BK,true,Interest-bearing deposits
1510,us-gaap,NoninterestBearingDepositLiabilities,30,bank_statement,funding,total_deposits,Total deposits,instant,credit,monetary,BK,true,Noninterest-bearing deposits
1516,us-gaap,NoninterestIncome,10,bank_statement,noninterest,noninterest_income,Noninterest income,duration,credit,monetary,BK,true,Total noninterest income
1516,us-gaap,FeesAndCommissions,20,bank_statement,noninterest,noninterest_income,Noninterest income,duration,credit,monetary,BK,true,Fees and commissions
1516,us-gaap,NoninterestIncomeOtherOperatingIncome,30,bank_statement,noninterest,noninterest_income,Noninterest income,duration,credit,monetary,BK,true,Other operating noninterest income
1517,us-gaap,NoninterestExpense,10,bank_statement,noninterest,noninterest_expense,Noninterest expense,duration,debit,monetary,BK,true,Total noninterest expense
1517,us-gaap,OtherNoninterestExpense,20,bank_statement,noninterest,noninterest_expense,Noninterest expense,duration,debit,monetary,BK,true,Other noninterest expense
1517,us-gaap,LaborAndRelatedExpense,30,bank_statement,noninterest,noninterest_expense,Noninterest expense,duration,debit,monetary,BK,true,Salaries and employee benefits
1601,us-gaap,PremiumsEarnedNetPropertyAndCasualty,20,,,,,,,,,true,Property and casualty premiums earned
1601,us-gaap,PremiumsEarnedNetLife,30,,,,,,,,,true,Life premiums earned
1602,us-gaap,PremiumsWrittenNet,10,insurance_statement,premiums,premiums_written,Premiums written,duration,credit,monetary,IS,true,Net premiums written
1602,us-gaap,PremiumsWrittenGross,20,insurance_statement,premiums,premiums_written,Premiums written,duration,credit,monetary,IS,true,Gross premiums written
1603,us-gaap,LiabilityForFuturePolicyBenefits,10,insurance_statement,reserves,loss_reserves,Policy reserves,instant,credit,monetary,IS,true,Liability for future policy benefits
1603,us-gaap,LiabilityForFuturePolicyBenefitsAndUnearnedPremiums,20,insurance_statement,reserves,loss_reserves,Policy reserves,instant,credit,monetary,IS,true,Future policy benefits plus unearned premiums
1603,us-gaap,PolicyholderContractDeposits,30,insurance_statement,reserves,loss_reserves,Policy reserves,instant,credit,monetary,IS,true,Policyholder contract deposits
1604,us-gaap,PolicyholderBenefitsAndClaimsIncurredNet,10,insurance_statement,benefits,insurance_benefits_paid,Benefits and claims,duration,debit,monetary,IS,true,Policyholder benefits and claims incurred net
1604,us-gaap,BenefitsLossesAndExpenses,20,insurance_statement,benefits,insurance_benefits_paid,Benefits and claims,duration,debit,monetary,IS,true,Total benefits losses and expenses
1605,us-gaap,LiabilityForClaimsAndClaimsAdjustmentExpense,10,insurance_statement,reserves,unpaid_claim_liability,Unpaid claims,instant,credit,monetary,IS,true,Liability for claims and claims adjustment expense
1605,us-gaap,LiabilityForUnpaidClaimsAndClaimsAdjustmentExpenseNet,20,insurance_statement,reserves,unpaid_claim_liability,Unpaid claims,instant,credit,monetary,IS,true,Unpaid claims net of reinsurance
1605,us-gaap,LiabilityForUnpaidClaimsAndClaimsAdjustmentExpenseGross,30,insurance_statement,reserves,unpaid_claim_liability,Unpaid claims,instant,credit,monetary,IS,true,Unpaid claims gross of reinsurance
1609,us-gaap,Investments,10,insurance_statement,investments,investment_portfolio,Investment portfolio,instant,debit,monetary,IS,true,Total investments
1609,us-gaap,AvailableForSaleSecuritiesDebtSecurities,20,insurance_statement,investments,investment_portfolio,Investment portfolio,instant,debit,monetary,IS,true,Available-for-sale debt securities
1609,us-gaap,DebtSecuritiesAvailableForSaleExcludingAccruedInterest,30,insurance_statement,investments,investment_portfolio,Investment portfolio,instant,debit,monetary,IS,true,AFS debt securities excluding accrued interest
1611,us-gaap,NetInvestmentIncome,10,insurance_statement,investments,investment_income_insurance,Insurance investment income,duration,credit,monetary,IS,true,Net investment income
1611,us-gaap,GrossInvestmentIncomeOperating,20,insurance_statement,investments,investment_income_insurance,Insurance investment income,duration,credit,monetary,IS,true,Gross operating investment income
1713,us-gaap,OperatingLeaseLeaseIncome,10,reit_statement,revenue,rental_revenue,Rental revenue,duration,credit,monetary,RT,true,ASC-842 lessor lease income
1713,us-gaap,RealEstateRevenueNet,20,reit_statement,revenue,rental_revenue,Rental revenue,duration,credit,monetary,RT,true,Net real-estate revenue
1713,us-gaap,OperatingLeasesIncomeStatementLeaseRevenue,30,reit_statement,revenue,rental_revenue,Rental revenue,duration,credit,monetary,RT,true,Pre-ASC-842 lease revenue
1714,us-gaap,RealEstateInvestmentPropertyNet,10,reit_statement,assets,real_estate_investments_net,Real-estate investments net,instant,debit,monetary,RT,true,Real-estate investment property net
1714,us-gaap,RealEstateInvestmentPropertyAtCost,20,reit_statement,assets,real_estate_investments_net,Real-estate investments net,instant,debit,monetary,RT,true,Real-estate investment property at cost
1801,us-gaap,PublicUtilitiesRevenue,20,,,,,,,,,true,Public utilities revenue
1801,us-gaap,UtilityRevenue,30,,,,,,,,,true,Utility revenue
1805,us-gaap,UtilitiesOperatingExpenseDepreciationAndAmortization,10,utility_statement,expenses,utility_depreciation_amortization,Utility depreciation and amortization,duration,debit,monetary,UT,true,Utility D and A operating expense
1806,us-gaap,RegulatoryAssetsNoncurrent,10,utility_statement,regulatory,regulatory_assets,Regulatory assets,instant,debit,monetary,UT,true,Noncurrent regulatory assets
1806,us-gaap,RegulatoryAssets,20,utility_statement,regulatory,regulatory_assets,Regulatory assets,instant,debit,monetary,UT,true,Total regulatory assets
1806,us-gaap,RegulatoryAssetsCurrent,30,utility_statement,regulatory,regulatory_assets,Regulatory assets,instant,debit,monetary,UT,true,Current regulatory assets
1807,us-gaap,RegulatoryLiabilityNoncurrent,10,utility_statement,regulatory,regulatory_liabilities,Regulatory liabilities,instant,credit,monetary,UT,true,Noncurrent regulatory liabilities
1807,us-gaap,RegulatoryLiabilities,20,utility_statement,regulatory,regulatory_liabilities,Regulatory liabilities,instant,credit,monetary,UT,true,Total regulatory liabilities
1807,us-gaap,RegulatoryLiabilityCurrent,30,utility_statement,regulatory,regulatory_liabilities,Regulatory liabilities,instant,credit,monetary,UT,true,Current regulatory liabilities
```

Every one of these concepts is unowned today, so `registry_alias` is `true` throughout and `item_registry._validate_aliases` stays satisfied. The pre-existing `__VENDOR_ONLY__total_deposits` and `__VENDOR_ONLY__insurance_benefits_paid` statement-map rows and their entries in `STATEMENT_MAP_OVERLAY_EXCEPTION_REASONS` are **left in place** (never delete) - the new `us-gaap` rows have a different `concept` so the primary key does not collide.

- [ ] **Step 6: Apply the wave**

```
.venv\Scripts\python.exe scripts\apply_alias_wave.py --aliases research\wave_b_industry_aliases.csv --items research\wave_b_industry_items.csv --rules research\wave_b_industry_rules.csv
```

Expected stdout:

```json
{
  "items_added": 7,
  "rules_added_or_replaced": 24,
  "statement_map_rows_added": 56,
  "registry_alias_rows_added": 56,
  "rule_alias_json_updates": 150
}
```

- [ ] **Step 7: Extend `TEMPLATE_ITEMS`**

In `C:\atx\atx-db\src\atx_db\industry_templates.py`, append these 16 rows inside the `TEMPLATE_ITEMS` tuple, keeping the existing rows untouched:

```python
    # Tier1-S2 T8: Wave B content. Items with a curated us-gaap alias set are
    # not_available=False; employees stays True because no numeric XBRL fact exists.
    IndustryTemplateItem("ALL", 1052, "employees", "optional", True, "Employee counts appear only in 10-K text blocks; no us-gaap or dei numeric fact exists."),
    IndustryTemplateItem("ALL", 1120, "capitalized_software", "optional", False, "Capitalized internal- and external-use software."),
    IndustryTemplateItem("ALL", 1209, "operating_lease_liability", "required", False, "ASC-842 operating lease liabilities."),
    IndustryTemplateItem("ALL", 1227, "finance_lease_liabilities", "optional", False, "ASC-842 finance lease liabilities."),
    IndustryTemplateItem("BK", 1505, "provision_for_loan_losses", "required", False, "Bank credit provision."),
    IndustryTemplateItem("BK", 1506, "allowance_for_loan_and_lease_losses", "required", False, "Bank allowance for credit losses."),
    IndustryTemplateItem("BK", 1510, "total_deposits", "required", False, "Bank total deposits, now source-verified from us-gaap Deposits."),
    IndustryTemplateItem("BK", 1516, "noninterest_income", "required", False, "Bank noninterest income."),
    IndustryTemplateItem("BK", 1517, "noninterest_expense", "required", False, "Bank noninterest expense."),
    IndustryTemplateItem("IS", 1603, "loss_reserves", "required", False, "Insurance policy reserves."),
    IndustryTemplateItem("IS", 1604, "insurance_benefits_paid", "required", False, "Insurance benefits and claims, now source-verified."),
    IndustryTemplateItem("IS", 1611, "investment_income_insurance", "required", False, "Insurance net investment income."),
    IndustryTemplateItem("RT", 1713, "rental_revenue", "required", False, "REIT rental revenue."),
    IndustryTemplateItem("RT", 1714, "real_estate_investments_net", "required", False, "REIT real-estate investments net."),
    IndustryTemplateItem("UT", 1806, "regulatory_assets", "required", False, "Utility regulatory assets."),
    IndustryTemplateItem("UT", 1807, "regulatory_liabilities", "required", False, "Utility regulatory liabilities."),
```

- [ ] **Step 8: Update the pinned counts**

In `tests/test_item_registry.py`:

```python
AUTHORIZED_ITEM_IDS = (
    set(range(1001, 1044))
    | set(range(1045, 1053))
    | set(range(1101, 1121))
    | set(range(1201, 1228))
    | set(range(1301, 1326))
    | {1327}
    | set(range(1401, 1428))
    | set(range(1501, 1518))          # Tier1-S2 T8: +1516 noninterest_income, +1517 noninterest_expense
    | set(range(1601, 1612))          # Tier1-S2 T8: +1611 investment_income_insurance
    | set(range(1701, 1715))          # Tier1-S2 T8: +1713 rental_revenue, +1714 real_estate_investments_net
    | set(range(1801, 1808))          # Tier1-S2 T8: +1806 regulatory_assets, +1807 regulatory_liabilities
    | set(range(1901, 1906))
    | set(range(2001, 2045))
)
```

and change the two `242` assertions to `249`.

In `tests/test_standardization.py::test_standardization_rule_seed_covers_template_items`:

```python
    # Tier1-S2 T8: +3 each for 1516, 1517, 1611, 1713 and +1 each for 1714, 1806, 1807.
    assert len(rules) == 480
    assert len(by_basis["annual"]) == 136
    assert len(by_basis["quarterly"]) == 136
    assert len(by_basis["ttm"]) == 136
    assert len(by_basis["instant"]) == 72
```

In `tests/test_statement_map_seed.py` set `EXPECTED_STATEMENT_MAP_ROWS = 399` (343 + 56).

- [ ] **Step 9: Run the test set**

```
.venv\Scripts\python.exe -m pytest tests/test_alias_depth.py tests/test_industry_templates.py tests/test_seed_determinism.py -n 0 -q
.venv\Scripts\python.exe -m pytest tests/test_statement_map_seed.py tests/test_ingest_allowlist.py tests/test_item_registry.py tests/test_standardization.py tests/test_concept_coverage.py tests/test_fundamental_concept_dictionary.py tests/test_fundamental_fact_item_links.py -n 0 -q
```

Expected: all passed. `tests/test_concept_coverage.py::test_statement_map_overlay_exceptions_are_explicit` still passes because no vendor-only row or exception key was removed.

- [ ] **Step 10: Commit**

```bash
git add research/wave_b_industry_aliases.csv research/wave_b_industry_items.csv research/wave_b_industry_rules.csv src/atx_db/industry_templates.py src/atx_db/seeds tests/test_alias_depth.py tests/test_industry_templates.py tests/test_item_registry.py tests/test_standardization.py tests/test_statement_map_seed.py
git commit -m "feat(db): Wave B bank, insurer, REIT and utility template content

56 curated us-gaap aliases across the four financial templates and seven new
items (1516/1517 noninterest income and expense, 1611 insurance investment
income, 1713 rental_revenue, 1714 real_estate_investments_net, 1806/1807
regulatory assets and liabilities). total_deposits and insurance_benefits_paid
move from vendor-only to source-verified; the vendor-only rows and their
exception entries are retained.

net_interest_income falls back to interest income - interest expense,
utility_operating_income to the ALL-template operating income, and FFO to
net_income + cf_depreciation. FFO is therefore FFO-before-gains: the three-term
signed form needs an n-ary composition, carried to Sprint 3 and recorded in the
statement-map notes column.

TEMPLATE_ITEMS grows 30 -> 46, including employees as not_available=True since
no us-gaap or dei numeric employee-count fact exists.

Pinned counts updated with justification: 242 -> 249 items, 465 -> 480 rules,
343 -> 399 statement-map rows.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: Item-coverage measurement harness, migration 0301, and `docs/ITEM_COVERAGE.md`

The spec states the gate as "at least 110 items with at least 90% coverage on the top-3000 by market cap for FY2015+, measured and published as coverage metrics". This task builds the measurement, not the number: the tests check the harness arithmetic on fixtures, and the gate becomes the criterion the S1 activation run is evaluated against.

**Files:**
- Create: `C:\atx\atx-db\src\atx_db\item_coverage.py`
- Create: `C:\atx\atx-db\src\atx_db\migrations\bodies_0301.py`
- Create: `C:\atx\atx-db\scripts\measure_item_coverage.py`
- Modify: `C:\atx\atx-db\src\atx_db\migrations\registry.py`
- Modify: `C:\atx\atx-db\tests\data\public_api_snapshot.json`
- Test: `C:\atx\atx-db\tests\test_item_coverage.py`

**Interfaces:**
- Consumes: `atx_db.connection.DuckDBStore`, the `fundamental_standardized` table written by `_standardization_set_based.refresh_standardized_set_based`, and `universe_membership` (`DEFAULT_UNIVERSE_ID = "us_common_equity_liquid_v1"`).
- Produces: `ItemCoverageOptions`, `ITEM_COVERAGE_COLUMNS`, `compute_item_coverage_rows`, `load_item_coverage_inputs`, `refresh_item_coverage`, `render_item_coverage_markdown`, `evaluate_item_coverage_gate`, `ITEM_COVERAGE_TARGET_ITEMS = 110`, `ITEM_COVERAGE_TARGET_PCT = 90.0`, and the `fundamental_item_coverage` table at migration `0301`.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_item_coverage.py`:

```python
"""Tier1-S2 T9: per (item, fiscal_year) coverage math, rendering and gate."""
from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from atx_db.item_coverage import (
    ITEM_COVERAGE_COLUMNS,
    ITEM_COVERAGE_TARGET_ITEMS,
    ITEM_COVERAGE_TARGET_PCT,
    ItemCoverageOptions,
    compute_item_coverage_rows,
    evaluate_item_coverage_gate,
    refresh_item_coverage,
    render_item_coverage_markdown,
)

UNIVERSE = pd.DataFrame(
    {
        "security_id": ["S1", "S2", "S3", "S4"],
        "fiscal_year": [2020, 2020, 2020, 2020],
    }
)
STANDARDIZED = pd.DataFrame(
    [
        # S1 and S2 report revenue; S3 reports it as NULL; S4 does not report it.
        {"security_id": "S1", "item_id": 1001, "canonical_code": "revenue", "basis": "annual", "fiscal_year": 2020, "value": 10.0},
        {"security_id": "S2", "item_id": 1001, "canonical_code": "revenue", "basis": "annual", "fiscal_year": 2020, "value": 20.0},
        {"security_id": "S3", "item_id": 1001, "canonical_code": "revenue", "basis": "annual", "fiscal_year": 2020, "value": None},
        # only S1 reports total_assets
        {"security_id": "S1", "item_id": 1101, "canonical_code": "total_assets", "basis": "instant", "fiscal_year": 2020, "value": 99.0},
        # a quarterly row must not leak into the annual basis slice
        {"security_id": "S4", "item_id": 1001, "canonical_code": "revenue", "basis": "quarterly", "fiscal_year": 2020, "value": 5.0},
    ]
)


def test_coverage_columns_are_stable():
    assert ITEM_COVERAGE_COLUMNS == (
        "source",
        "universe_id",
        "item_id",
        "canonical_code",
        "basis",
        "fiscal_year",
        "n_securities",
        "n_with_value",
        "coverage_pct",
    )


def test_coverage_counts_distinct_securities_with_a_non_null_value():
    rows = compute_item_coverage_rows(STANDARDIZED, UNIVERSE, ItemCoverageOptions(bases=("annual",)))
    revenue = rows[(rows["item_id"] == 1001) & (rows["basis"] == "annual")].iloc[0]

    assert revenue["n_securities"] == 4
    assert revenue["n_with_value"] == 2
    assert revenue["coverage_pct"] == pytest.approx(50.0)


def test_coverage_is_zero_not_missing_for_an_item_no_one_reports():
    rows = compute_item_coverage_rows(
        STANDARDIZED,
        UNIVERSE,
        ItemCoverageOptions(bases=("annual",), item_ids=(1001, 1003)),
    )
    absent = rows[rows["item_id"] == 1003].iloc[0]

    assert absent["n_with_value"] == 0
    assert absent["coverage_pct"] == pytest.approx(0.0)
    assert absent["n_securities"] == 4


def test_coverage_never_exceeds_one_hundred_percent():
    duplicated = pd.concat([STANDARDIZED, STANDARDIZED], ignore_index=True)
    rows = compute_item_coverage_rows(duplicated, UNIVERSE, ItemCoverageOptions(bases=("annual",)))

    assert (rows["coverage_pct"] <= 100.0).all()
    assert rows[rows["item_id"] == 1001].iloc[0]["n_with_value"] == 2


def test_coverage_rows_are_sorted_deterministically():
    rows = compute_item_coverage_rows(STANDARDIZED, UNIVERSE, ItemCoverageOptions(bases=("annual", "instant")))
    keys = list(zip(rows["basis"], rows["item_id"], rows["fiscal_year"], strict=True))

    assert keys == sorted(keys)
    assert compute_item_coverage_rows(STANDARDIZED, UNIVERSE, ItemCoverageOptions(bases=("annual", "instant"))).equals(rows)


def test_gate_reports_items_meeting_the_spec_threshold():
    rows = pd.DataFrame(
        [
            {"item_id": i, "canonical_code": f"item_{i}", "basis": "annual", "fiscal_year": 2020,
             "n_securities": 100, "n_with_value": 95, "coverage_pct": 95.0}
            for i in range(1, 112)
        ]
        + [
            {"item_id": 9999, "canonical_code": "thin", "basis": "annual", "fiscal_year": 2020,
             "n_securities": 100, "n_with_value": 10, "coverage_pct": 10.0}
        ]
    )
    gate = evaluate_item_coverage_gate(rows, minimum_fiscal_year=2015)

    assert gate["items_meeting_threshold"] == 111
    assert gate["target_items"] == ITEM_COVERAGE_TARGET_ITEMS
    assert gate["target_coverage_pct"] == ITEM_COVERAGE_TARGET_PCT
    assert gate["status"] == "passed"
    assert gate["shortfall_items"] == 0


def test_gate_fails_below_the_item_threshold():
    rows = pd.DataFrame(
        [
            {"item_id": i, "canonical_code": f"item_{i}", "basis": "annual", "fiscal_year": 2020,
             "n_securities": 100, "n_with_value": 95, "coverage_pct": 95.0}
            for i in range(1, 47)
        ]
    )
    gate = evaluate_item_coverage_gate(rows, minimum_fiscal_year=2015)

    assert gate["items_meeting_threshold"] == 46
    assert gate["status"] == "degraded"
    assert gate["shortfall_items"] == 64


def test_markdown_renders_a_stable_document():
    rows = compute_item_coverage_rows(STANDARDIZED, UNIVERSE, ItemCoverageOptions(bases=("annual", "instant")))
    text = render_item_coverage_markdown(rows, generated_from="tests/test_item_coverage.py")

    assert text.startswith("# Standardized item coverage\n")
    assert "| item_id | canonical_code | basis | fiscal_year |" in text
    assert "tests/test_item_coverage.py" in text
    assert text == render_item_coverage_markdown(rows, generated_from="tests/test_item_coverage.py")
    assert "now()" not in text


def test_refresh_writes_the_coverage_table(tmp_store):
    tmp_store.con.execute(
        """
        INSERT INTO universe_membership (
            universe_id, security_id, symbol, valid_from, valid_to, as_of_date,
            available_at, reason, rules_json, decision_count, is_latest_revision,
            source, run_id, source_loaded_at
        ) VALUES
            ('us_common_equity_liquid_v1','S1','AAA',DATE '2020-01-01',NULL,DATE '2020-01-01',TIMESTAMP '2020-01-01',NULL,'{}',1,true,'test',NULL,TIMESTAMP '2020-01-01'),
            ('us_common_equity_liquid_v1','S2','BBB',DATE '2020-01-01',NULL,DATE '2020-01-01',TIMESTAMP '2020-01-01',NULL,'{}',1,true,'test',NULL,TIMESTAMP '2020-01-01')
        """
    )
    for security_id, value in (("S1", 10.0), ("S2", None)):
        tmp_store.con.execute(
            """
            INSERT INTO fundamental_standardized (
                standardized_id, source, upstream_source, security_id, symbol, cik,
                item_id, canonical_code, basis, period_start, period_end, fiscal_year,
                fiscal_period, value, unit_type, source_accession, filed_date, as_of_date,
                available_at, input_codes_json, input_item_ids_json, rule_id,
                combination_rule, is_latest_revision, run_id
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """,
            [
                f"std-{security_id}", "fundamental_standardization_v1", "test", security_id,
                security_id, "1", 1001, "revenue", "annual", dt.date(2020, 1, 1),
                dt.date(2020, 12, 31), 2020, "FY", value, "monetary", "acc", dt.date(2021, 2, 1),
                dt.date(2020, 12, 31), dt.datetime(2021, 2, 1), "[]", "[1001]",
                "std_annual_1001", "coalesce_priority", True, None,
            ],
        )

    written = refresh_item_coverage(tmp_store, ItemCoverageOptions(bases=("annual",), item_ids=(1001,)))
    row = tmp_store.con.execute(
        "SELECT n_securities, n_with_value, coverage_pct FROM fundamental_item_coverage WHERE item_id = 1001"
    ).fetchone()

    assert written == 1
    assert row == (2, 1, pytest.approx(50.0))


def test_migration_0301_is_registered_and_idempotent(tmp_store):
    from atx_db.migrations.registry import MIGRATIONS

    versions = [migration.version for migration in MIGRATIONS]
    assert 301 in versions
    assert versions == sorted(versions)
    recorded = tmp_store.con.execute(
        "SELECT count(*) FROM schema_migrations WHERE version = 301"
    ).fetchone()[0]
    catalogued = tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'fundamental_item_coverage'"
    ).fetchone()[0]

    assert recorded == 1
    assert catalogued == 1
```

- [ ] **Step 2: Run the test to verify it fails**

```
.venv\Scripts\python.exe -m pytest tests/test_item_coverage.py -n 0 -q
```

Expected: collection error, `ModuleNotFoundError: No module named 'atx_db.item_coverage'`.

- [ ] **Step 3: Write the migration**

Create `C:\atx\atx-db\src\atx_db\migrations\bodies_0301.py`:

```python
"""Tier1-S2 T9: published per-item, per-fiscal-year standardized coverage."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _fundamental_item_coverage(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_item_coverage (
            coverage_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            universe_id VARCHAR NOT NULL,
            item_id INTEGER NOT NULL,
            canonical_code VARCHAR NOT NULL,
            basis VARCHAR NOT NULL,
            fiscal_year INTEGER NOT NULL,
            n_securities BIGINT NOT NULL,
            n_with_value BIGINT NOT NULL,
            coverage_pct DOUBLE NOT NULL,
            as_of_date DATE,
            available_at TIMESTAMP,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_fundamental_item_coverage_item
            ON fundamental_item_coverage(item_id, basis, fiscal_year)
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,
            pit_notes,updated_at
        ) VALUES (
            'fundamental_item_coverage','quality','standardized_item',
            'universe_id + item_id + basis + fiscal_year',
            'Share of the standardized universe reporting each canonical item per fiscal year.',
            '["coverage_id"]',
            'Derived from fundamental_standardized rows whose available_at is already gated upstream; coverage rows are a measurement artifact and are never used as a signal input.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name,dataset_id,table_name,severity,threshold_value,
            comparator,enabled,failure_status,source,updated_at
        ) VALUES (
            'standardized_item_breadth_target',
            'fundamental_standardized',
            'fundamental_item_coverage',
            'warning',110.0,'gte',true,'warning',
            'atx_tier1_parity',now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("fundamental_item_coverage",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=301,
        name="fundamental_item_coverage",
        up=_fundamental_item_coverage,
    )
]
```

Register it in `C:\atx\atx-db\src\atx_db\migrations\registry.py`: add `from .bodies_0301 import MIGRATIONS as _MIGRATIONS_0301` in import order after `bodies_0299`, and `*_MIGRATIONS_0301,` as the last entry of the `MIGRATIONS` list. `0300` is reserved for Sprint 1 and is intentionally skipped here; `_validate_registry_versions` only requires ascending order and no duplicates, so the gap is legal.

- [ ] **Step 4: Write `item_coverage.py`**

Create `C:\atx\atx-db\src\atx_db\item_coverage.py`:

```python
"""Tier1-S2 T9: measure and publish standardized item coverage.

Coverage is (distinct securities in the universe with a non-null standardized
value) / (distinct securities in the universe) for each
(universe_id, item_id, basis, fiscal_year). The spec gate is
ITEM_COVERAGE_TARGET_ITEMS items at or above ITEM_COVERAGE_TARGET_PCT on the
top-3000 by market cap for fiscal years at or after 2015; this module measures
it, it does not assert it.
"""
from __future__ import annotations

import hashlib
from collections.abc import Sequence
from dataclasses import dataclass, field
from typing import Any

import pandas as pd

from .connection import DuckDBStore

DEFAULT_SOURCE = "fundamental_standardization_v1"
DEFAULT_UNIVERSE_ID = "us_common_equity_liquid_v1"
ITEM_COVERAGE_TARGET_ITEMS = 110
ITEM_COVERAGE_TARGET_PCT = 90.0
ITEM_COVERAGE_TARGET_TOP_N = 3000
ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR = 2015

ITEM_COVERAGE_COLUMNS = (
    "source",
    "universe_id",
    "item_id",
    "canonical_code",
    "basis",
    "fiscal_year",
    "n_securities",
    "n_with_value",
    "coverage_pct",
)


@dataclass(frozen=True)
class ItemCoverageOptions:
    source: str = DEFAULT_SOURCE
    universe_id: str = DEFAULT_UNIVERSE_ID
    bases: tuple[str, ...] = ("annual", "quarterly", "instant", "ttm")
    item_ids: tuple[int, ...] = field(default=())
    minimum_fiscal_year: int = 1990
    run_id: str | None = None


def _coverage_id(*parts: Any) -> str:
    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def compute_item_coverage_rows(
    standardized: pd.DataFrame,
    universe: pd.DataFrame,
    options: ItemCoverageOptions | None = None,
) -> pd.DataFrame:
    """Pure transform: standardized long facts + universe -> coverage rows."""

    options = options or ItemCoverageOptions()
    if universe.empty:
        return pd.DataFrame(columns=list(ITEM_COVERAGE_COLUMNS))

    universe_size = (
        universe.groupby("fiscal_year", sort=True)["security_id"].nunique().to_dict()
    )

    facts = standardized.copy()
    if not facts.empty:
        facts = facts[facts["basis"].isin(options.bases)]
        facts = facts[facts["fiscal_year"] >= options.minimum_fiscal_year]
        facts = facts[facts["value"].notna()]
        facts = facts[facts["security_id"].isin(set(universe["security_id"]))]

    labels: dict[int, str] = {}
    if not standardized.empty:
        labels = {
            int(item_id): str(code)
            for item_id, code in standardized[["item_id", "canonical_code"]]
            .drop_duplicates()
            .itertuples(index=False, name=None)
        }

    observed_items = sorted({int(value) for value in standardized.get("item_id", pd.Series(dtype=int))})
    requested_items = sorted(set(options.item_ids)) or observed_items

    records: list[dict[str, Any]] = []
    for basis in sorted(options.bases):
        basis_facts = facts[facts["basis"] == basis] if not facts.empty else facts
        for fiscal_year in sorted(universe_size):
            if fiscal_year < options.minimum_fiscal_year:
                continue
            n_securities = int(universe_size[fiscal_year])
            year_facts = (
                basis_facts[basis_facts["fiscal_year"] == fiscal_year]
                if not basis_facts.empty
                else basis_facts
            )
            per_item = (
                year_facts.groupby("item_id")["security_id"].nunique().to_dict()
                if not year_facts.empty
                else {}
            )
            for item_id in requested_items:
                n_with_value = int(per_item.get(item_id, 0))
                records.append(
                    {
                        "source": options.source,
                        "universe_id": options.universe_id,
                        "item_id": int(item_id),
                        "canonical_code": labels.get(int(item_id), f"item_{item_id}"),
                        "basis": basis,
                        "fiscal_year": int(fiscal_year),
                        "n_securities": n_securities,
                        "n_with_value": n_with_value,
                        "coverage_pct": round(100.0 * n_with_value / n_securities, 6) if n_securities else 0.0,
                    }
                )

    frame = pd.DataFrame(records, columns=list(ITEM_COVERAGE_COLUMNS))
    if frame.empty:
        return frame
    return frame.sort_values(["basis", "item_id", "fiscal_year"], kind="mergesort").reset_index(drop=True)


def load_item_coverage_inputs(
    store: DuckDBStore,
    options: ItemCoverageOptions | None = None,
) -> tuple[pd.DataFrame, pd.DataFrame]:
    """Read latest-revision standardized facts and the universe by fiscal year."""

    options = options or ItemCoverageOptions()
    standardized = store.con.execute(
        """
        SELECT security_id, item_id, canonical_code, basis, fiscal_year, value
        FROM fundamental_standardized
        WHERE source = ?
          AND is_latest_revision
          AND fiscal_year IS NOT NULL
        """,
        [options.source],
    ).df()
    universe = store.con.execute(
        """
        SELECT DISTINCT u.security_id, f.fiscal_year
        FROM universe_membership u
        JOIN (
            SELECT DISTINCT security_id, fiscal_year
            FROM fundamental_standardized
            WHERE source = ? AND fiscal_year IS NOT NULL
        ) f ON f.security_id = u.security_id
        WHERE u.universe_id = ?
          AND u.is_latest_revision
        """,
        [options.source, options.universe_id],
    ).df()
    return standardized, universe


def refresh_item_coverage(
    store: DuckDBStore,
    options: ItemCoverageOptions | None = None,
) -> int:
    """Recompute and replace the coverage rows for this source and universe."""

    options = options or ItemCoverageOptions()
    store.initialize()
    standardized, universe = load_item_coverage_inputs(store, options)
    frame = compute_item_coverage_rows(standardized, universe, options)
    if frame.empty:
        return 0
    frame = frame.copy()
    frame["coverage_id"] = [
        _coverage_id(row.source, row.universe_id, row.item_id, row.basis, row.fiscal_year)
        for row in frame.itertuples(index=False)
    ]
    frame["as_of_date"] = None
    frame["available_at"] = None
    frame["run_id"] = options.run_id
    with store.transaction():
        store.con.execute(
            "DELETE FROM fundamental_item_coverage WHERE source = ? AND universe_id = ?",
            [options.source, options.universe_id],
        )
        store.con.register("_item_coverage_frame", frame)
        try:
            store.con.execute(
                """
                INSERT INTO fundamental_item_coverage (
                    coverage_id, source, universe_id, item_id, canonical_code, basis,
                    fiscal_year, n_securities, n_with_value, coverage_pct,
                    as_of_date, available_at, run_id, source_loaded_at
                )
                SELECT
                    coverage_id, source, universe_id, item_id, canonical_code, basis,
                    fiscal_year, n_securities, n_with_value, coverage_pct,
                    CAST(as_of_date AS DATE), CAST(available_at AS TIMESTAMP), run_id, now()
                FROM _item_coverage_frame
                """
            )
        finally:
            store.con.unregister("_item_coverage_frame")
    return len(frame)


def evaluate_item_coverage_gate(
    frame: pd.DataFrame,
    *,
    minimum_items: int = ITEM_COVERAGE_TARGET_ITEMS,
    minimum_coverage_pct: float = ITEM_COVERAGE_TARGET_PCT,
    minimum_fiscal_year: int = ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR,
) -> dict[str, Any]:
    """Count items clearing the spec threshold in every in-scope fiscal year."""

    if frame.empty:
        return {
            "items_meeting_threshold": 0,
            "target_items": minimum_items,
            "target_coverage_pct": minimum_coverage_pct,
            "minimum_fiscal_year": minimum_fiscal_year,
            "shortfall_items": minimum_items,
            "status": "degraded",
        }
    scoped = frame[frame["fiscal_year"] >= minimum_fiscal_year]
    per_item = scoped.groupby("item_id")["coverage_pct"].min()
    meeting = int((per_item >= minimum_coverage_pct).sum())
    return {
        "items_meeting_threshold": meeting,
        "target_items": minimum_items,
        "target_coverage_pct": minimum_coverage_pct,
        "minimum_fiscal_year": minimum_fiscal_year,
        "shortfall_items": max(0, minimum_items - meeting),
        "status": "passed" if meeting >= minimum_items else "degraded",
    }


def render_item_coverage_markdown(
    frame: pd.DataFrame,
    *,
    generated_from: str,
    bases: Sequence[str] = ("annual",),
) -> str:
    """Render a clock-free markdown coverage report."""

    gate = evaluate_item_coverage_gate(frame)
    lines = [
        "# Standardized item coverage",
        "",
        f"Generated by `{generated_from}`. Regenerate with "
        "`.venv\\Scripts\\python.exe scripts\\measure_item_coverage.py --write-docs`.",
        "",
        "## Spec gate",
        "",
        f"- Target: at least {gate['target_items']} items at or above "
        f"{gate['target_coverage_pct']}% coverage for fiscal years at or after "
        f"{gate['minimum_fiscal_year']} on the top-{ITEM_COVERAGE_TARGET_TOP_N} by market cap.",
        f"- Observed: {gate['items_meeting_threshold']} items. Shortfall: {gate['shortfall_items']}.",
        f"- Status: {gate['status']}.",
        "",
        "## Coverage by item and fiscal year",
        "",
        "| item_id | canonical_code | basis | fiscal_year | n_securities | n_with_value | coverage_pct |",
        "| ---: | --- | --- | ---: | ---: | ---: | ---: |",
    ]
    scoped = frame[frame["basis"].isin(list(bases))] if not frame.empty else frame
    for row in scoped.sort_values(["basis", "item_id", "fiscal_year"], kind="mergesort").itertuples(index=False):
        lines.append(
            f"| {row.item_id} | {row.canonical_code} | {row.basis} | {row.fiscal_year} | "
            f"{row.n_securities} | {row.n_with_value} | {row.coverage_pct:.2f} |"
        )
    lines.append("")
    return "\n".join(lines)
```

- [ ] **Step 5: Write the operator CLI**

Create `C:\atx\atx-db\scripts\measure_item_coverage.py`:

```python
#!/usr/bin/env python
"""Measure standardized item coverage and optionally publish docs/ITEM_COVERAGE.md."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "src"))

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.item_coverage import (
    ItemCoverageOptions,
    evaluate_item_coverage_gate,
    load_item_coverage_inputs,
    compute_item_coverage_rows,
    refresh_item_coverage,
    render_item_coverage_markdown,
)

DOCS_PATH = PROJECT_ROOT / "docs" / "ITEM_COVERAGE.md"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--universe-id", default="us_common_equity_liquid_v1")
    parser.add_argument("--minimum-fiscal-year", type=int, default=1990)
    parser.add_argument("--basis", action="append", dest="bases")
    parser.add_argument("--write-docs", action="store_true")
    parser.add_argument("--run-id")
    args = parser.parse_args(argv)

    options = ItemCoverageOptions(
        universe_id=args.universe_id,
        bases=tuple(args.bases or ("annual", "quarterly", "instant", "ttm")),
        minimum_fiscal_year=args.minimum_fiscal_year,
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        written = refresh_item_coverage(store, options)
        standardized, universe = load_item_coverage_inputs(store, options)
        frame = compute_item_coverage_rows(standardized, universe, options)
    gate = evaluate_item_coverage_gate(frame)
    if args.write_docs:
        DOCS_PATH.parent.mkdir(parents=True, exist_ok=True)
        DOCS_PATH.write_text(
            render_item_coverage_markdown(frame, generated_from="scripts/measure_item_coverage.py"),
            encoding="utf-8",
        )
    print(json.dumps({"rows_written": written, "gate": gate, "docs": str(DOCS_PATH) if args.write_docs else None}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 6: Register the new module in the public API snapshot**

Insert `"item_coverage"` into the `"atx_db"` list in `tests/data/public_api_snapshot.json`, between `"is_53_week_period"` and `"item_registry"`.

- [ ] **Step 7: Run the tests**

```
.venv\Scripts\python.exe -m pytest tests/test_item_coverage.py -n 0 -q
.venv\Scripts\python.exe -m pytest tests/test_module_boundaries.py tests/test_schema_contract_v2.py -n 0 -q --run-slow
```

Expected: `10 passed`, then all passed. The schema-contract test must be run with `--run-slow` because `_refresh_schema_contract_v2_pin` changes the pinned contract hash.

- [ ] **Step 8: Commit**

```bash
git add src/atx_db/item_coverage.py src/atx_db/migrations/bodies_0301.py src/atx_db/migrations/registry.py scripts/measure_item_coverage.py tests/test_item_coverage.py tests/data/public_api_snapshot.json
git commit -m "feat(db): item-coverage harness, migration 0301, ITEM_COVERAGE.md generator

Per (universe, item, basis, fiscal_year) coverage over the standardized
universe: n_securities, n_with_value, coverage_pct, materialized into the new
fundamental_item_coverage table. evaluate_item_coverage_gate encodes the spec
target - at least 110 items at 90% or better for FY2015+ on the top 3000 - as a
measurement, not an assertion; the tests check the arithmetic on fixtures and
the S1 activation run is what the gate judges.

Migration numbering starts at 0301 for this sprint; 0300 is reserved for S1 and
the registry validator permits the gap.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 10: Offline CompanyFacts fixture corpus

Tests must not use the network, so the diverse-issuer corpus is committed. The script has two modes: `network` (the one-time operator run that trims real SEC documents) and `synthetic` (fully offline, deterministic, no clock, no RNG). **If the implementer cannot run the network mode, run `--mode synthetic` and commit those fixtures instead** - the test suite consumes whatever is in `tests/data/companyfacts_fixture/` either way.

**Files:**
- Create: `C:\atx\atx-db\scripts\make_companyfacts_fixture.py`
- Create: `C:\atx\atx-db\tests\data\companyfacts_fixture\*.json` (nine files)
- Test: `C:\atx\atx-db\tests\test_companyfacts_fixture.py`

**Interfaces:**
- Consumes: `atx_db.fundamental_statements.default_companyfacts_concepts` (Task 2), `atx_db.item_registry.default_registry`, `atx_db.statement_map_seed.read_statement_map_seed` (Task 1), `atx_db.standardization.compute_standardized_rows` (Task 5).
- Produces: `scripts/make_companyfacts_fixture.py` with `FIXTURE_CIKS: tuple[tuple[str, str], ...]`, `FIXTURE_DIR: Path`, `MAX_FIXTURE_BYTES = 300_000`, `trim_companyfacts(document, concepts, *, max_points_per_unit) -> dict`, `synthetic_companyfacts(cik, name, concepts, *, fiscal_years) -> dict`, `main(argv) -> int`.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_companyfacts_fixture.py`:

```python
"""Tier1-S2 T10: the committed CompanyFacts fixtures are offline, small, and routable."""
from __future__ import annotations

import json
from pathlib import Path

import pandas as pd
import pytest

from atx_db.fundamental_statements import default_companyfacts_concepts
from atx_db.item_registry import default_registry
from atx_db.standardization import compute_standardized_rows, default_standardization_rules
from atx_db.statement_map_seed import read_statement_map_seed

FIXTURE_DIR = Path(__file__).resolve().parent / "data" / "companyfacts_fixture"
MAX_FIXTURE_BYTES = 300_000
EXPECTED_FIXTURE_COUNT = 9


def _fixture_paths() -> list[Path]:
    return sorted(FIXTURE_DIR.glob("*.json"))


def _facts(document: dict) -> list[tuple[str, str, dict]]:
    rows: list[tuple[str, str, dict]] = []
    for taxonomy, concepts in document["facts"].items():
        for concept, payload in concepts.items():
            for unit, points in payload["units"].items():
                for point in points:
                    rows.append((taxonomy, concept, {**point, "unit": unit}))
    return rows


def test_fixture_corpus_exists_and_is_small():
    paths = _fixture_paths()
    assert len(paths) == EXPECTED_FIXTURE_COUNT
    oversized = {path.name: path.stat().st_size for path in paths if path.stat().st_size > MAX_FIXTURE_BYTES}
    assert oversized == {}


def test_every_fixture_concept_is_in_the_ingest_allowlist():
    allowlist = set(default_companyfacts_concepts())
    unknown: set[str] = set()
    for path in _fixture_paths():
        document = json.loads(path.read_text(encoding="utf-8"))
        for _taxonomy, concept, _point in _facts(document):
            if concept not in allowlist:
                unknown.add(concept)
    assert sorted(unknown) == []


def test_every_fixture_concept_routes_to_a_canonical_item():
    registry = default_registry()
    mapped = {
        (row.taxonomy, row.concept): row.item_id
        for row in read_statement_map_seed()
        if row.item_id is not None and row.is_active
    }
    unrouted: set[str] = set()
    for path in _fixture_paths():
        document = json.loads(path.read_text(encoding="utf-8"))
        for taxonomy, concept, _point in _facts(document):
            if registry.resolve_item(taxonomy, concept) is None and (taxonomy, concept) not in mapped:
                unrouted.add(f"{taxonomy}:{concept}")
    assert sorted(unrouted) == []


def test_fixture_documents_carry_the_required_shape():
    for path in _fixture_paths():
        document = json.loads(path.read_text(encoding="utf-8"))
        assert set(document) >= {"cik", "entityName", "facts"}
        assert str(document["cik"]).isdigit()
        assert document["facts"], path.name


def test_fixtures_cover_a_diverse_set_of_templates():
    """The corpus must exercise the ALL, BK, IS, RT and UT templates."""
    from atx_db.industry_templates import template_for_sic

    sics = {
        "0000320193": 3571,   # Apple - ALL
        "0000789019": 7372,   # Microsoft - ALL
        "0000019617": 6021,   # JPMorgan - BK
        "0001067983": 6331,   # Berkshire - IS
        "0001045609": 6798,   # Prologis - RT
        "0001326160": 4911,   # Duke Energy - UT
        "0000034088": 2911,   # Exxon - ALL
        "0000004977": 6321,   # Aflac - IS
    }
    templates = {template_for_sic(sic)[0] for sic in sics.values()}
    assert {"ALL", "BK", "IS", "RT", "UT"} <= templates
    present = {path.stem for path in _fixture_paths()}
    assert set(sics) <= present


def test_fixture_facts_standardize_into_many_distinct_items():
    """End-to-end: fixture facts -> candidate frame -> standardized rows."""
    registry = default_registry()
    mapped = {
        (row.taxonomy, row.concept): int(row.item_id)
        for row in read_statement_map_seed()
        if row.item_id is not None and row.is_active
    }
    records = []
    for path in _fixture_paths():
        document = json.loads(path.read_text(encoding="utf-8"))
        security_id = f"SEC-{document['cik']}"
        for taxonomy, concept, point in _facts(document):
            item_id = registry.resolve_item(taxonomy, concept) or mapped.get((taxonomy, concept))
            if item_id is None or point.get("val") is None or point.get("end") is None:
                continue
            start = point.get("start")
            records.append(
                {
                    "security_id": security_id,
                    "symbol": document["cik"],
                    "cik": document["cik"],
                    "item_id": item_id,
                    "canonical_metric": concept,
                    "concept": concept,
                    "taxonomy": taxonomy,
                    "unit": point["unit"],
                    "unit_type": "monetary",
                    "basis": "instant" if start is None else "annual",
                    "period_start": start,
                    "period_end": point["end"],
                    "fiscal_year": point.get("fy"),
                    "fiscal_period": point.get("fp"),
                    "accession_number": point.get("accn"),
                    "source_accession": point.get("accn"),
                    "filed_date": point.get("filed"),
                    "value": point["val"],
                    "available_at": pd.Timestamp(point.get("filed") or point["end"]),
                    "input_rank": 10,
                }
            )
    frame = pd.DataFrame.from_records(records)
    rows = compute_standardized_rows(frame, rules=default_standardization_rules())

    assert not rows.empty
    assert rows["item_id"].nunique() >= 40
    assert rows["security_id"].nunique() == EXPECTED_FIXTURE_COUNT


@pytest.mark.parametrize("path", _fixture_paths(), ids=lambda p: p.stem)
def test_each_fixture_is_valid_json_and_utf8(path: Path):
    json.loads(path.read_text(encoding="utf-8"))
```

- [ ] **Step 2: Run the test to verify it fails**

```
.venv\Scripts\python.exe -m pytest tests/test_companyfacts_fixture.py -n 0 -q
```

Expected: `test_fixture_corpus_exists_and_is_small` FAILS with `assert 0 == 9`.

- [ ] **Step 3: Write the fixture builder**

Create `C:\atx\atx-db\scripts\make_companyfacts_fixture.py`:

```python
#!/usr/bin/env python
"""Build the offline CompanyFacts fixture corpus under tests/data/companyfacts_fixture/.

Two modes:

  --mode network    ONE-TIME OPERATOR RUN. Downloads
                    https://data.sec.gov/api/xbrl/companyfacts/CIK##########.json
                    for the nine fixture CIKs, trims each document to the concepts
                    the rules CSV references plus the most frequent remaining
                    concepts, caps the points per unit, and writes the result.
                    Requires a contact User-Agent per SEC fair-access policy.
                    NEVER run from a test.

  --mode synthetic  Fully offline. Emits deterministic, clock-free, RNG-free
                    documents covering every allowlisted concept for the same
                    nine CIKs. Use this when the network run is not possible;
                    the test suite treats both the same way.

The outputs are committed. Tests read them; they never call this script.
"""
from __future__ import annotations

import argparse
import json
import sys
import urllib.request
from collections import Counter
from pathlib import Path
from typing import Any

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "src"))

from atx_db.fundamental_statements import default_companyfacts_concepts
from atx_db.statement_map_seed import read_statement_map_seed

FIXTURE_DIR = PROJECT_ROOT / "tests" / "data" / "companyfacts_fixture"
MAX_FIXTURE_BYTES = 300_000
COMPANYFACTS_URL = "https://data.sec.gov/api/xbrl/companyfacts/CIK{cik}.json"
DEFAULT_USER_AGENT = "atx-db fixture builder nathan.tormaschy@gmail.com"

# (zero-padded CIK, label). The ninth entry is a deliberately small issuer so the
# corpus is not all mega-caps; verify the CIK on EDGAR before the network run.
FIXTURE_CIKS: tuple[tuple[str, str], ...] = (
    ("0000004977", "AFLAC INC"),
    ("0000019617", "JPMORGAN CHASE & CO"),
    ("0000034088", "EXXON MOBIL CORP"),
    ("0000095029", "STURM RUGER & CO INC"),
    ("0000320193", "Apple Inc."),
    ("0000789019", "MICROSOFT CORP"),
    ("0001045609", "Prologis, Inc."),
    ("0001067983", "BERKSHIRE HATHAWAY INC"),
    ("0001326160", "DUKE ENERGY CORP"),
)

SYNTHETIC_FISCAL_YEARS = (2015, 2016, 2017, 2018, 2019, 2020, 2021, 2022, 2023, 2024)


def _rule_concepts() -> set[str]:
    """Concepts referenced by the seeds - the ones that must survive trimming."""

    return {row.concept for row in read_statement_map_seed() if row.is_active and not row.concept.startswith("__")}


def trim_companyfacts(
    document: dict[str, Any],
    concepts: set[str],
    *,
    max_points_per_unit: int = 24,
    extra_frequent: int = 50,
) -> dict[str, Any]:
    """Keep the seeded concepts plus the N most frequent others; cap the points."""

    facts = document.get("facts", {})
    frequency: Counter[str] = Counter()
    for taxonomy_payload in facts.values():
        for concept, payload in taxonomy_payload.items():
            frequency[concept] = sum(len(points) for points in payload.get("units", {}).values())
    keep = set(concepts) | {concept for concept, _ in frequency.most_common(extra_frequent)}

    trimmed_facts: dict[str, Any] = {}
    for taxonomy, taxonomy_payload in sorted(facts.items()):
        kept_concepts: dict[str, Any] = {}
        for concept, payload in sorted(taxonomy_payload.items()):
            if concept not in keep:
                continue
            units: dict[str, Any] = {}
            for unit, points in sorted(payload.get("units", {}).items()):
                ordered = sorted(
                    points,
                    key=lambda p: (str(p.get("end", "")), str(p.get("filed", "")), str(p.get("accn", ""))),
                )
                units[unit] = ordered[-max_points_per_unit:]
            if units:
                kept_concepts[concept] = {
                    "label": payload.get("label"),
                    "description": None,
                    "units": units,
                }
        if kept_concepts:
            trimmed_facts[taxonomy] = kept_concepts

    return {
        "cik": int(document["cik"]),
        "entityName": document.get("entityName", ""),
        "facts": trimmed_facts,
    }


def synthetic_companyfacts(
    cik: str,
    name: str,
    concepts: set[str],
    *,
    fiscal_years: tuple[int, ...] = SYNTHETIC_FISCAL_YEARS,
) -> dict[str, Any]:
    """Deterministic offline stand-in: no clock, no RNG, stable ordering."""

    by_taxonomy: dict[str, dict[str, Any]] = {}
    seed = int(cik)
    for row in sorted(read_statement_map_seed(), key=lambda r: (r.taxonomy, r.concept)):
        if row.concept not in concepts or row.concept.startswith("__") or not row.is_active:
            continue
        unit = "USD" if row.unit_type == "monetary" else ("shares" if row.unit_type == "shares" else "pure")
        points = []
        for index, fiscal_year in enumerate(fiscal_years):
            value = float((seed % 9973) + 1) * (index + 1) * (row.concept_priority or 10)
            point = {
                "end": f"{fiscal_year}-12-31",
                "val": value,
                "accn": f"{cik}-{fiscal_year}-000001",
                "fy": fiscal_year,
                "fp": "FY",
                "form": "10-K",
                "filed": f"{fiscal_year + 1}-02-15",
            }
            if row.period_type != "instant":
                point["start"] = f"{fiscal_year}-01-01"
            points.append(point)
        by_taxonomy.setdefault(row.taxonomy, {})[row.concept] = {
            "label": row.canonical_label,
            "description": None,
            "units": {unit: points},
        }
    return {
        "cik": int(cik),
        "entityName": name,
        "facts": {taxonomy: dict(sorted(payload.items())) for taxonomy, payload in sorted(by_taxonomy.items())},
    }


def _write(document: dict[str, Any], path: Path) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(document, indent=1, sort_keys=True, ensure_ascii=False).encode("utf-8")
    if len(payload) > MAX_FIXTURE_BYTES:
        raise SystemExit(f"{path.name} is {len(payload)} bytes, over the {MAX_FIXTURE_BYTES} cap")
    path.write_bytes(payload)
    return len(payload)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("network", "synthetic"), default="synthetic")
    parser.add_argument("--user-agent", default=DEFAULT_USER_AGENT)
    parser.add_argument("--max-points-per-unit", type=int, default=24)
    parser.add_argument("--extra-frequent", type=int, default=50)
    parser.add_argument("--output-dir", type=Path, default=FIXTURE_DIR)
    args = parser.parse_args(argv)

    allowlist = set(default_companyfacts_concepts())
    seeded = _rule_concepts() & allowlist
    written: dict[str, int] = {}
    for cik, name in FIXTURE_CIKS:
        target = args.output_dir / f"{cik}.json"
        if args.mode == "synthetic":
            document = synthetic_companyfacts(cik, name, seeded)
        else:
            request = urllib.request.Request(
                COMPANYFACTS_URL.format(cik=cik),
                headers={"User-Agent": args.user_agent, "Accept-Encoding": "gzip, deflate"},
            )
            with urllib.request.urlopen(request, timeout=120) as response:  # noqa: S310 - operator-only path
                raw = json.loads(response.read().decode("utf-8"))
            document = trim_companyfacts(
                raw,
                seeded,
                max_points_per_unit=args.max_points_per_unit,
                extra_frequent=args.extra_frequent,
            )
            document["facts"] = {
                taxonomy: {c: p for c, p in payload.items() if c in allowlist}
                for taxonomy, payload in document["facts"].items()
            }
            document["facts"] = {t: p for t, p in document["facts"].items() if p}
        written[target.name] = _write(document, target)
    print(json.dumps({"mode": args.mode, "bytes": written}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Build the fixtures**

Preferred (one-time operator run, needs network and the SEC fair-access User-Agent):

```
.venv\Scripts\python.exe scripts\make_companyfacts_fixture.py --mode network --user-agent "atx-db fixture builder nathan.tormaschy@gmail.com"
```

Fallback when no network is available:

```
.venv\Scripts\python.exe scripts\make_companyfacts_fixture.py --mode synthetic
```

Expected: JSON on stdout listing nine filenames, each under 300000 bytes. If the network run trips the size cap for a mega-cap (Berkshire and JPMorgan are the likely ones), re-run with `--max-points-per-unit 12 --extra-frequent 20` and record the flags used in the commit body.

- [ ] **Step 5: Verify the fixtures are network-free at test time**

```
.venv\Scripts\python.exe -m pytest tests/test_companyfacts_fixture.py -n 0 -q
.venv\Scripts\python.exe -c "import pathlib; t=pathlib.Path('tests/test_companyfacts_fixture.py').read_text(encoding='utf-8'); assert 'urllib' not in t and 'requests' not in t and 'http' not in t; print('no network references in the test')"
```

Expected: `15 passed` (8 named tests plus the 9-way parametrized JSON check, minus the two that collapse when the corpus is synthetic-only), then `no network references in the test`.

- [ ] **Step 6: Commit**

```bash
git add scripts/make_companyfacts_fixture.py tests/data/companyfacts_fixture tests/test_companyfacts_fixture.py
git commit -m "feat(db): offline CompanyFacts fixture corpus for nine diverse issuers

scripts/make_companyfacts_fixture.py has an operator-only network mode that
trims real SEC companyfacts documents to the seeded concepts plus the 50 most
frequent others, and a fully offline synthetic mode that is deterministic,
clock-free and RNG-free. Nine issuers cover the ALL, BK, IS, RT and UT industry
templates plus a small-cap name; every file is under 300 KB.

The committed fixtures are what the suite reads - no test ever calls the script
or the network. The end-to-end test asserts the fixture facts route to at least
40 distinct canonical items through the Wave A and Wave B seeds.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Sequencing and dependencies

```
T1 statement_map.csv
 |-> T2 ingest allowlist (needs default_statement_map_rows)
 |-> T3 seed determinism + normalizer (needs the statement-map writer)
        |-> T6 Wave A-1 ---> T7 Wave A-2 ---> T8 Wave B
        |      ^                                  |
        |      |                                  v
        |   T5 engine coalesce_or_* (independent of T1-T4; MUST land before T6)
        |
        |-> T4 alias mining (independent; informs T6-T8 curation)
                                                  |
                                                  v
                                       T9 coverage harness + 0301
                                                  |
                                                  v
                                       T10 fixture corpus
```

- T5 has no dependency on T1-T4 and can be implemented in parallel, but it must be merged before T6 because the Wave A-1 rules CSV uses `coalesce_or_difference`.
- T6, T7 and T8 are strictly sequential: each one's expected `apply_alias_wave` output and each pinned count depends on the previous wave's state.
- T4 is genuinely optional to the critical path; it is the tool that justifies the curated content in T6-T8 and should land before them so a reviewer can check a wave row against `research/alias_candidates.csv`.
- T10 depends on T2 (the allowlist the fixtures are filtered against) and on T8 (the test asserts 40+ routed items, which needs both waves).

## Anticipated challenges

1. **Alias ownership collisions.** `item_registry._validate_aliases` fails the whole build if one `(scheme, code)` maps to two `item_id`s. The waves are curated to avoid it and `tests/test_seed_determinism.py::test_no_alias_code_maps_to_two_items_in_the_item_seed` catches it at seed-read time. When a genuine template-specific overlay is needed, set `registry_alias=false` in the wave CSV so the row lands only in `statement_map.csv`, whose primary key includes `industry_template`.
2. **The statement map is what stamps `fundamental_statement_points.item_id`.** Adding a `fundamental_items.csv` alias alone routes only the `fundamental_xbrl_metric` path. Every wave row therefore writes both, which is why `apply_alias_wave.py` is a single tool rather than three manual edits.
3. **Pinned counts.** Three tests hard-code registry and rule totals (`235`, `455`, `130`, `65`) and one hard-codes the statement-map row count. Every wave updates them in the same commit with the delta justified in the commit body, exactly as the global constraint requires. If a count is off by a few, recount from the seeds rather than guessing - `.venv\Scripts\python.exe -c "import sys; sys.path.insert(0,'src'); from atx_db.standardization import read_standardization_rules as r; x=r(); print(len(x), {b: len({q.item_id for q in x if q.basis==b}) for b in ('annual','quarterly','ttm','instant')})"`.
4. **Schema-template churn.** `tests/conftest.py` fingerprints every `seeds/*.csv`, so each wave rebuilds the ~9 s template once. Under `-n 0` that cost lands on the first test of the run; do not mistake it for a hang.
5. **`_refresh_schema_contract_v2_pin` in migration 0301** changes the pinned schema-contract hash. `tests/test_schema_contract_v2.py` is marked `slow`; it must be run with `--run-slow` in Task 9 or the change ships unverified.
6. **FFO is approximate.** `1701` is composed as `net_income + cf_depreciation`, not the full Nareit definition. This is recorded in the statement-map `notes`, in the Task 8 commit body, and as a Sprint 3 item. Do not silently "fix" it with a two-term `difference`, which would produce a wrong number.

## Self-review against the spec

- **Canonical item catalog (IS/BS/CF/supplemental).** All 91 spec codes are mapped in the table above: 77 to existing `item_id`s, 14 to new ones. Income and cash flow land in Task 6, balance sheet in Task 7, supplemental and industry in Task 8.
- **"Deterministic tag->item rules".** Tasks 3 and 5: canonical seed ordering with a guard test, priority ordering per item inside `source_aliases_json`, and the documented selection order (`available_at DESC, input_rank`).
- **"Industry-aware templates for banks, insurers, REITs, utilities".** Task 8, routed through the existing SIC classification with `test_sic_routing_is_unchanged_by_wave_b` proving the routing is untouched.
- **"Target published breadth: at least 110 items with at least 90% coverage on the top-3000 for FY2015+, measured and published as coverage metrics".** Task 9 builds the measurement and encodes the target in `evaluate_item_coverage_gate` and `docs/ITEM_COVERAGE.md`. The registry reaches 249 items and every spec item has either a deep alias set or a composition rule, which is the precondition for clearing 110 published.
- **"Benchmark ... recorded in tests as fixtures (no network in tests)".** Task 10.
- **Charter item 1 (statement map to CSV).** Task 1, including the row-count test and the fingerprint assertion.
- **Charter item 2 (alias mining).** Task 4.
- **Charter item 3 (Wave A).** Tasks 5, 6, 7.
- **Charter item 4 (Wave B).** Task 8.
- **Charter item 5 (coverage harness).** Task 9.
- **Charter item 6 (fixture corpus).** Task 10.
- **Coordinator addendum (ingest allowlist).** Task 2, placed before every curated wave, with the `--force` re-run recorded as an operator note in Global Constraints rather than a task.
