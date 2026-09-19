# atx-db Tier-1 Parity — Sprint 4 (Universe, Delistings, Identity, Quality Gates, Publication) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the last four Tier-1 gaps between the warehouse and a quant-usable cross-section: a defined, point-in-time, survivorship-free universe (`universe_us_listed`); delisting events built from real public evidence with an explicit, testable terminal-return policy so the critical survivorship gate stops passing on an empty anti-join; measured quality gates (accounting identities, cross-source shares, item and derived-metric coverage) that drive every public schema's `degraded`→`available` condition; and a reproducible full-universe publication with a release manifest and a release-over-release diff — then retire the surfaces Sprint 3 superseded and make the docs tell the truth.

**Architecture:** Three new deterministic builders sit on the Sprint 1–3 stack and register as activation stages. `universe_us_listed.py` classifies each listing line's `security_type` from the Nasdaq directory fields plus ordered name patterns, maps the directory `exchange` letter to a MIC-style `exchange_code`, and compresses per-bar daily decisions into the PIT interval table `universe_us_listed_membership`, whose `valid_to` extends `lookback_days` trading sessions past the last bar — the same interval-compression shape `universe.compute_universe_membership_intervals` already uses. `delisting_evidence.py` materialises four independent public evidence streams (SEC Form 25 / 25-NSE, Nasdaq Trader deletes, SEC Form 15, archive last-trade inference) into `delisting_evidence`, then folds them into `delisting_events` by a fixed precedence so the delist reason is attributed rather than guessed. `delisting.py` gains an explicit, configurable Shumway-convention terminal return for performance-related delists with no observed DLRET, and `quality/checks_survivorship.py` gains a coverage check that fails when `delisting_events` is non-empty while `delisting_terminal_returns` is empty — the exact vacuity the audit flags. `quality/checks_identities.py` is a new leaf check module carrying the spec's three accounting identities, the 5%/95% cross-source shares test, and derived-metric family coverage, all as `SqlQualityCheck` specs. `provider_coverage.py` learns one new SLO dimension — `item_count_basis='coverage_gate'` — so the `standardized` schema's condition flips on measured item coverage clearing the spec threshold rather than a raw distinct-code count, and every public schema (including Sprint 3's two) gets an SLO row so `refresh_provider_coverage` can no longer raise. `publication.py` writes one Parquet file per release dataset plus a single manifest (schema sha256, query sha256, parquet sha256, row counts, activation stage run ids) and a key-level diff against the previous release. `scripts/generate_data_dictionary.py` renders `docs/DATA_DICTIONARY.md` from the committed registries alone — no warehouse required — so CI can fail on stale docs.

**Tech Stack:** Python 3.12, DuckDB 1.5.x (embedded, via `atx_db.connection.DuckDBStore`; window frames, `ntile`, `sha256()`, `read_parquet`, `COPY ... TO ... (FORMAT PARQUET)`), pandas + pyarrow (interval compression and Parquet verification only), stdlib `csv`/`json`/`hashlib`/`re`/`dataclasses`, pytest 9 + pytest-xdist + filelock, numbered migration bodies in `src/atx_db/migrations/bodies_NNNN.py` registered in `migrations/registry.py`, GitHub Actions.

**Spec:** `C:\atx\docs\superpowers\specs\2026-09-19-tier1-parity-design.md` — sections "Identity", "Market data from `tbltickerhistory`" (delisting paragraph), "Universe", "Quality gates (published metrics, not claims)", "Serving".

**Supporting audit:** `C:\atx\.superpowers\sdd\tier1-parity\audit-atx-db.md` — §4.2/§4.3 (bars and corporate actions; the `split_factor`-holds-`returnFactor` hazard), §4.4 (delisting machinery vs delisting data; the vacuous critical gate), §5 (two coexisting universes; measured coverage), §8 gaps #7 (delisting returns), #9 (LEI/FIGI unpopulated), #10 (publication/serving tier), #11 (segments/footnotes unpublished), #13 (CI runs 6 of 165 test files), and §9 "Other notes" (27 dead `derived` registry items, orphan `warehouse_template.duckdb`, stale parity docs).

**Upstream sprints this plan consumes:**

- Sprint 1 — `C:\atx\docs\superpowers\plans\2026-09-19-tier1-s1-foundation.md`. Landed on `feat/tier1-parity`: migration `0300` (`activation_stage_runs`), `atx_db.clock.utc_today()` / `resolve_as_of_date()`, and `atx_db.activation` with `STAGE_ORDER: tuple[str, ...]`, `STAGES: dict[str, Callable[[DuckDBStore, ActivationOptions], StageResult]]`, `StageResult(rows: int, detail: dict[str, object])`, `ActivationOptions` (frozen dataclass with `as_dict()` / `ledger_params()`), `begin_stage`, `finish_stage`, `completed_stages`, `select_stages`, `run_activation`, and the `atx-db activate` subcommand.
- Sprint 2 — `...-s2-standardization.md`. Migration `0301` and `atx_db.item_coverage` with `ItemCoverageOptions(source, universe_id, bases, item_ids, minimum_fiscal_year, run_id)`, `ITEM_COVERAGE_COLUMNS`, `compute_item_coverage_rows`, `load_item_coverage_inputs`, `refresh_item_coverage(store, options=None) -> int`, `render_item_coverage_markdown`, `evaluate_item_coverage_gate(frame, *, minimum_items=ITEM_COVERAGE_TARGET_ITEMS, minimum_coverage_pct=ITEM_COVERAGE_TARGET_PCT, minimum_fiscal_year=ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR) -> dict[str, Any]` (keys `items_meeting_threshold`, `target_items`, `target_coverage_pct`, `minimum_fiscal_year`, `shortfall_items`, `status`), `ITEM_COVERAGE_TARGET_ITEMS = 110`, `ITEM_COVERAGE_TARGET_PCT = 90.0`, table `fundamental_item_coverage`, and `docs/ITEM_COVERAGE.md`.
- Sprint 3 — `...-s3-derived-engine.md`. Migrations `0302`–`0303`, `atx_db.derived_registry` (`DERIVED_SOURCE_NAME = "atx-db declarative derived metrics v1"`, `default_derived_definitions()`, `topological_order`), the tables `derived_metric_definitions` / `derived_metric_values` / `market_daily_metrics` (spine columns `market_daily_id, source, security_id, symbol, trade_date, close, adj_close, volume, shares_outstanding, shares_source, shares_reconciliation_ratio, <30 metric columns including market_cap>, fundamental_available_at, available_at, inputs_hash, as_of_date, is_latest_revision, run_id, source_loaded_at`), `atx_db.market_daily` (`MARKET_DAILY_SOURCE_NAME = "atx-db daily market panel v1"`, `END_OF_DAY_HOURS = 22`, `shares_reconciliation_report`), `atx_db.panel_export` (`PANEL_EXPORT_CONTRACT_VERSION`, `export_panel_quarterly`, `export_panel_daily_market`), `atx_db.derived_factor_projection`, and the `derived-metrics` / `market-daily-1d` `RecordSchema`s in `api/catalog.py`.

## Global Constraints

Copied verbatim from the sprint charter. Every task's requirements implicitly include this section.

- Python 3.12 venv at `C:\atx\atx-db\.venv\Scripts\python.exe`.
- Run tests from `C:\atx\atx-db` with `.venv\Scripts\python.exe -m pytest <file> -n 0 -q`.
- No network in tests (all connectors injected/fixture-driven).
- No `eval`.
- No wall-clock reads except `atx_db.warehouse.now_utc_naive()` for load stamps and `atx_db.clock.utc_today()` at script edges.
- Deterministic ordering (`ORDER BY` on every insert-select).
- Never delete existing public tables.
- Commits `feat(db): ...` / `refactor(db): ...` / `docs(db): ...` ending with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Each task ≤ ~450 lines of new code (seed rows and generated Markdown excluded).
- TDD per task: write the failing test first, run it, see it fail, then implement.

### Repo facts this plan depends on (verified by reading the files, not assumed)

- **Migration registry head is `0300`.** `src/atx_db/migrations/registry.py` ends with `*_MIGRATIONS_0300,` and `_validate_registry_versions()` enforces ascending, unique versions at import. Sprint 2 takes `0301`, Sprint 3 takes `0302`–`0303`. **Sprint 4 takes `0304`–`0309`.** Pattern (verified in `bodies_0299.py` / `bodies_0300.py`): a module exporting `MIGRATIONS = [Migration(version=NNN, name="...", up=_fn)]`, imported and splatted in `registry.py`; catalog fields are refreshed with `bodies_0001_0137._catalog_fields_for_tables(conn, (...))` and the contract pin with `bodies_0140_0143._refresh_schema_contract_v2_pin(conn)`.
- `equity_daily_bars` (`schema.py:536` plus later `ALTER`s): `source, security_id VARCHAR, vendor_security_id, symbol, trade_date DATE, open, high, low, close, adjusted_close, volume, vwap, dividend_amount, split_factor, is_adjusted, available_at TIMESTAMP, run_id, source_loaded_at, as_of_date, is_latest_revision, shares_outstanding, market_cap_usd`. `available_at = trade_date + 22h` (`ticker_history.py:392`). **`split_factor` holds the vendor `returnFactor`, not a split ratio** (audit §4.3) — nothing in this plan reads it.
- `securities` (`schema.py:290`): `security_id VARCHAR PRIMARY KEY, entity_id, issuer_id, primary_symbol, name, asset_class, country, currency, active, first_seen_date, last_seen_date, source, source_loaded_at`. The CIK entity key is the string `'CIK-' || cik` (see `identifiers_lei.compute_lei_alias_rows`).
- `security_identifier_history` (`schema.py:309`): `security_id, id_type, id_value, internal_cusip, valid_from, valid_to, as_of_date, available_at, source, run_id, source_loaded_at, is_latest_revision`. `internal_cusip` is non-exportable, enforced by `export_scan_internal_cusip_leak`.
- `nasdaq_symbol_directory` (`schema.py:127`): `directory, symbol, security_name, market_category, exchange, cqs_symbol, etf, test_issue, financial_status, round_lot_size, next_shares, nasdaq_symbol, as_of_date, source_url, run_id, source_loaded_at`. `symbol_directory.normalize_nasdaq_listed` writes `directory='nasdaqlisted'` with `exchange='NASDAQ'`; `normalize_other_listed` writes `directory='otherlisted'` with `exchange` carrying the raw CQS venue letter.
- `nasdaq_listing_events` (`schema.py:149`): `event_id, symbol, security_id, company_name, nasdaq_action, bx_action, psx_action, effective_date, primary_listing_market, as_of_date, source_file_created_at, source_url, run_id, source_loaded_at`.
- `sec_submissions` (`schema.py:169`): `security_id, cik, accession_number, filing_date, report_date, acceptance_datetime, form, primary_document, primary_doc_description, file_number, film_number, items, size, is_xbrl, is_inline_xbrl, act, source_url, run_id, source_loaded_at`.
- `delisting_events` (`schema.py:393`): 31 columns; `listing_status_source`, `source_listing_status_id`, `symbol`, `delist_date`, `as_of_date`, `available_at`, `delist_code`, `delist_reason`, `delisting_return_type`, `return_policy`, `return_confidence`, `evidence_source`, `evidence_source_table`, `method`, `evidence_confidence` are all `NOT NULL`. `security_id` is nullable.
- `delisting.py`: `DELIST_CODE_ROWS` (2 rows, `NASDAQ_DELETE` / `SNAPSHOT_ABSENCE`), `seed_delist_code_dim(store, *, source=DEFAULT_CODE_SOURCE) -> int`, `TERMINAL_RETURN_POLICY_ROWS` (6 rows, **every `default_return` is `None`**, seeded by migration `0185`, read back by `load_terminal_return_policy_dim`), `POLICY_DIM_COLUMNS`, `POLICY_TERMINAL_RETURN_COLUMNS`, `_TERMINAL_RETURN_BASIS_CONTRACT`, `apply_terminal_return_policy(events, corporate_actions, policy_dim)`, `compute_delisting_terminal_returns`, `refresh_delisting_terminal_returns`, `DelistingEventOptions(source, listing_status_source, include_snapshot_absence, apply_shumway_warther_imputation, run_id)`.
- `quality/_types.py`: `SqlQualityCheck(dataset_id, table_name, check_name, sql, threshold, comparator='eq', required_tables=(), warn_if_missing=True, failure_status='failed', detail_sql=None, severity=None)`, `QualityResult`, `QualityRegistryEntry`, `GateResult`, `Comparator = Literal['eq','le','ge']`, `Severity = Literal['critical','error','warning']`.
- `quality/_checks.py:422 _check_specs(*, daily_macro_stale_days, monthly_macro_stale_days, valuation_stale_gap_days)` concatenates `_market_reference_check_specs + _fundamental_check_specs + _ownership_check_specs + _feature_catalog_check_specs + _estimate_check_specs + _analytic_check_specs` plus compiled referential checks. Each `*_check_specs` factory lives in its own leaf module and is imported with an `as _x_check_specs` alias. `_resolve_spec(spec, registry)` applies registry overrides and returns `None` only when a registry row exists and is disabled — **a spec with no registry row still runs**.
- `quality/__init__.py` re-exports everything from `_types`, `_checks`, `_runner` and then `globals().pop(...)`s a fixed list of private factory names and leaf module names. A new leaf module and its `_x_check_specs` alias must be added to that pop list.
- `api/catalog.py`: `FieldSpec`, `RecordSchema(dataset, code, version, title, description, source_table, time_column, natural_key, fields, item_column=None, basis_column=None, supports_vintages=True, max_sync_rows=50_000)`, `DatasetSpec`, `_PIT_FIELDS`, `DATASETS` (two specs: `ATX.US.FUNDAMENTALS`, `ATX.US.EQUITIES`), `get_dataset`, `get_schema`, `public_catalog`, `public_schema`, `_record_schema_sha256`.
- `api/service.py:456-505` builds every range query as `SELECT b.* ... row_number() OVER (PARTITION BY <natural_key> ORDER BY coalesce(b.available_at, b.source_loaded_at), b.source_loaded_at, coalesce(b.run_id, ''))`. **Any relation backing a `RecordSchema` must therefore expose `available_at`, `source_loaded_at`, `run_id`, `as_of_date` and `security_id`**, and `provider_coverage._schema_stats` additionally requires the `time_column`.
- `provider_coverage.py`: `ProviderCoverageSlo` (10 fields), `DEFAULT_PROVIDER_COVERAGE_SLOS` (8 rows), `_active_slo(store, dataset_id, schema_code)` **raises `RuntimeError` when no active `api_schema_coverage_slo` row exists**, `_schema_stats`, `_evaluate_slos`, `refresh_provider_coverage`, `ProviderCoverageDataset`. The SLO table is created and seeded by migration `0274` and re-seeded from the same Python tuple by `0299`.
- `lake.py`: `DEFAULT_EXPORT_OBJECTS`, `LakehouseExporter`, `LakeExportResult`, and the package-private helpers `_schema_sha256(schema: list[dict]) -> str` and `_object_schema(store, object_name) -> list[dict]`. Sprint 3's `panel_export.py` already imports both with `from .lake import _object_schema, _schema_sha256`.
- `jobs.py`: `DATASET_REGISTRY: dict[str, tuple[type[Dataset], OptionFactory]]` (line 1230) and `DATASET_DEPENDENCIES: dict[str, tuple[str, ...]]` (line 1418); `_apply_dataset_dependencies()` raises `RuntimeError` on any `DATASET_DEPENDENCIES` key absent from `DATASET_REGISTRY`, so the two dicts are always edited together. `market_cap`, `enterprise_value` and `valuation_multiples` are registered there; `GovernedUniverseMembershipDataset` (`universe.py`), `LeiAliasDataset` and `FigiAliasDataset` are **not**.
- `identifiers_lei.py`: `SOURCE_NAME = "GLEIF"`, `DATASET_ID = "identifiers_lei"`, `LeiLoadOptions(lei_file, lei_level2_file=None, source=SOURCE_NAME, as_of_date=None, run_id=None)`, `parse_gleif_file(path)` (**CSV only, offline**), `parse_gleif_level2_file`, `derive_cik_lei_crosswalk`, `split_unambiguous_cik_lei_crosswalk`, `compute_lei_alias_rows`, `compute_entity_parent_edges`, `LeiAliasDataset`.
- `identifiers_figi.py`: `SOURCE_NAME = "OpenFIGI"`, `DATASET_ID = "identifiers_figi"`, `FigiLoadOptions(figi_file, source=SOURCE_NAME, as_of_date=None, run_id=None)`, `parse_openfigi_file(path)` (**CSV or the saved `POST /v3/mapping` JSON pair, offline**), `compute_figi_alias_rows`, `FigiAliasDataset`. Both loaders already route ambiguous matches to `identifier_resolution_candidates` / `identifier_resolution_decisions`; **neither makes a network call**, so "offline-injectable" is already satisfied and this sprint only has to schedule them.
- Canonical item codes used by the accounting-identity checks, verified present in `src/atx_db/seeds/fundamental_items.csv`: `total_assets` (1101), `total_liabilities` (1201), `stockholders_equity` (1221), `minority_interest_bs` (1213), `revenue` (1001), `cost_of_revenue_cogs` (1003), `gross_profit__1004` (1004), `cash_flow_from_operations` (1301), `cash_flow_from_investing` (1303), `cash_flow_from_financing` (1304), `fx_effect_on_cash` (1323), `net_change_in_cash` (1324). There is **no** `gross_profit`, `cogs`, `cfo`, `cfi`, `cff`, `change_in_cash` or `minority_interest` code — the spec's mnemonics are not the seed's codes.
- `factor_definition` carries `declared_in VARCHAR NOT NULL` and (via `bodies_0156_0159`) `valid_from DATE` / `valid_to DATE`. `bodies_0240.py` declares `investment_low_abnormal_capex` with `declared_in='atx_db.abnormal_capex'`; `bodies_0241.py` declares `risk_operating_leverage` with `declared_in='atx_db.operating_leverage'`.
- `C:\atx\atx-db\warehouse_template.duckdb` (50.3 MB) is **untracked and gitignored** (`git check-ignore -v` reports `.gitignore:102:*.duckdb`; `git ls-files --error-unmatch` fails). Deleting it is a local operator action, never a commit.
- `tests/conftest.py` fixtures: `_schema_template` (session), `tmp_store`, `fresh_store`, `built_warehouse(name) -> Path`. The fingerprint hashes `api/catalog.py`, `connection.py`, `schema.py`, `schema_contract.py`, `fundamental_statements.py`, `parity.py`, all `migrations/*.py` and all `seeds/*.csv` — so every migration and catalog edit in this plan invalidates the template automatically and no conftest change is needed.
- `docs/` holds `FUNDAMENTALS_PROVIDER_DESIGN.md`, `PARITY_GAP.md`, `PRODUCTION_RUNBOOK.md`, `ROADMAP_PARITY.md`, `SAAS_PLATFORM_ARCHITECTURE.md`, `TESTING.md`, `WAREHOUSE_PARITY_NEXT_AGENT_README.md`, `WAREHOUSE_PARITY_TRANCHES.md`, `XBRL_DQC_SUBSET.md`. `docs/ITEM_COVERAGE.md` is created by Sprint 2 and `docs/DATA_DICTIONARY.md` by this sprint.

### Deviations from the charter, with rationale

Six refinements; everything else in the charter is implemented as written.

1. **`fundamentals-core` is not registered as a new schema code.** The charter asks for it "where missing". It is not missing: `api/catalog.py` already publishes `fundamental_standardized` under the schema code `standardized` (`FUNDAMENTALS_SCHEMA`). A second schema code over the same relation would create two contract hashes for one table and two SLO rows racing for one condition. Task 7 therefore registers the three genuinely missing codes — `security-master`, `universe`, `delistings` — and Task 8's release dataset named `fundamentals_core` cites the existing `standardized` schema in its manifest.
2. **`security-master` is served by a new view `v_security_master_public`, not `v_security_master_current`.** The existing view exposes a `cusip` column (internal-only by policy, guarded by `export_scan_internal_cusip_leak`) and carries no `as_of_date` / `available_at` / `run_id`, which `api/service.py` and `provider_coverage._schema_stats` both require. Migration 0307 creates a PIT-shaped, CUSIP-free view instead of mutating the existing one.
3. **Market-cap decile is recorded at the interval's `valid_from`, not carried in the interval state.** Deciles change almost daily; including one in the state tuple would shatter every membership interval into per-day rows. The decile is an interval-start attribute, documented as such in the table catalog and the data dictionary; a per-date decile remains obtainable from `market_daily_metrics.market_cap` directly.
4. **Universe decisions are emitted on each security's own bar sessions, and `valid_to` is extended forward by `lookback_days` sessions at interval close.** This is equivalent to the spec's "≥1 trade in the prior 20 trading days" and costs O(bars) instead of O(securities × sessions); `universe.py` already uses the own-bar decision grid.
5. **"Retirement wave 2" deletes exactly two modules, not the three the reverse-import graph nominates.** See ruling 1 below.
6. **The Shumway terminal-return default is off unless the operator turns it on.** `DelistingTerminalReturnOptions.performance_delisting_return` defaults to `None`; the `-0.30` convention is a named constant applied only when the option is set. A silently imputed return would be worse than an honest gap, and the new coverage gate makes the gap visible either way.

### Rulings required before implementation (recorded here, resolved by the sprint owner)

1. **Retirement wave 2 is smaller than the charter implies.** A reverse-import scan over every `.py` in `C:\atx\atx-db` (run, not assumed — reproduced in Task 10 Step 0) shows that after Sprint 3 deletes its 18 modules, exactly four modules lose a module-level importer: `net_operating_assets` (already deleted by Sprint 3), and `cash_flow_profitability`, `fundamental_signals`, `quarterly_revenue_margin_confirmation`. **All three survivors keep their own `scripts/build_*.py` operator entry point**, so "only importer retired" does not make them dead. The genuinely dead modules are the two with *no* entry point at all and no importer after Sprint 3: `abnormal_capex` and `operating_leverage` (audit §9 names five such modules; Sprint 3 deletes the other three). This plan retires those two and leaves the three script-backed modules alone. Retiring the three script-backed modules would be a separate decision about deleting live operator surfaces, not a consequence of Sprint 3.
2. **Sprint 3 leaves `refresh_provider_coverage` broken and this plan fixes it.** Sprint 3 Task 9 attaches `DERIVED_METRICS_SCHEMA` and `MARKET_DAILY_SCHEMA` to the two `DatasetSpec`s but adds no `api_schema_coverage_slo` rows. `refresh_provider_coverage` iterates every schema of every dataset and calls `_active_slo`, which raises `RuntimeError("no active coverage SLO for ...")` when the row is absent — so the `provider_coverage` activation stage fails on the first run after Sprint 3 lands. Task 7 adds the missing rows. If Sprint 3 is still in flight the owner may prefer to fix it there; Task 7 is written to be idempotent either way (`INSERT OR REPLACE`).
3. **`SNAPSHOT_ABSENCE` stays disabled by default.** `DelistingEventOptions.include_snapshot_absence` defaults to `False` and this plan does not change that. The new `archive_last_trade` evidence stream is strictly better: it anchors to an actual cessation of trading rather than a directory-snapshot gap, and it carries the archive-end guard the snapshot rule lacks.

## File structure

**Created**

| Path | Responsibility |
| --- | --- |
| `src/atx_db/migrations/bodies_0304.py` | `universe_us_listed_membership` table, catalog rows, lake partition spec, quality-check registry rows. |
| `src/atx_db/universe_us_listed.py` | Security-type classification, exchange mapping, the PIT US-listed universe builder, the `universe_us_listed(store, as_of_date)` accessor, `UniverseUsListedDataset`. |
| `src/atx_db/migrations/bodies_0305.py` | `delisting_evidence` table, catalog rows, lake partition spec. |
| `src/atx_db/delisting_evidence.py` | The four public delisting evidence streams, reason classification by precedence, the `delisting_events` fold, `DelistingEvidenceDataset`. |
| `src/atx_db/migrations/bodies_0306.py` | Four new `delist_code_dim` rows, the `performance_unknown` terminal-return policy row, the survivorship-coverage check registry row. |
| `src/atx_db/quality/checks_identities.py` | Accounting-identity, cross-source-shares and derived-metric-family coverage `SqlQualityCheck` specs (leaf module). |
| `src/atx_db/migrations/bodies_0307.py` | `v_security_master_public` view, `api_schema_coverage_slo.item_count_basis` column, SLO rows for all six unseeded schemas, identity-check registry rows. |
| `src/atx_db/migrations/bodies_0308.py` | `publication_releases` / `publication_release_datasets` tables and their catalog rows. |
| `src/atx_db/publication.py` | `publish_release`, the release manifest, the release-over-release diff, `read_release_manifest`. |
| `src/atx_db/migrations/bodies_0309.py` | Deprecation marks for `market_cap` / `enterprise_value` / `valuation_multiples`; `valid_to` on the two retired factor definitions. |
| `scripts/build_universe_us_listed.py` | Operator CLI for the universe builder. |
| `scripts/build_delisting_evidence.py` | Operator CLI for the delisting evidence builder. |
| `scripts/publish_release.py` | Operator CLI wrapper for `publish_release`. |
| `scripts/generate_data_dictionary.py` | Deterministic `docs/DATA_DICTIONARY.md` generator with `--check`. |
| `docs/DATA_DICTIONARY.md` | Generated output, committed. |
| `tests/data/gleif_level1_sample.csv` | Offline GLEIF Level-1 fixture. |
| `tests/data/gleif_level2_sample.csv` | Offline GLEIF Level-2 fixture. |
| `tests/data/openfigi_mapping_sample.json` | Offline OpenFIGI `POST /v3/mapping` fixture. |
| `tests/test_universe_us_listed.py` | Classification table, exchange mapping, interval compression, gap extension, decile attachment, unresolved tail retention. |
| `tests/test_delisting_evidence.py` | Each evidence stream, precedence fold, reason classification, archive-end guard. |
| `tests/test_delisting_terminal_policy.py` | Shumway policy application, opt-in default, the non-vacuous survivorship coverage gate. |
| `tests/test_identifier_activation.py` | LEI/FIGI activation stages, offline fixtures, jobs registration. |
| `tests/test_quality_identities.py` | The three accounting identities, the shares 5%/95% test, derived-metric family coverage. |
| `tests/test_provider_coverage_slos.py` | Every public schema has an active SLO; `item_count_basis='coverage_gate'` flips the standardized condition. |
| `tests/test_publication.py` | Release manifest keys and hashes, Parquet round-trip, the added/removed/changed diff, CLI. |
| `tests/test_data_dictionary.py` | Generator determinism, `--check` staleness detection, committed file is current. |
| `tests/test_retirement_wave2.py` | The two retired modules are gone; the deprecated surfaces are marked and unscheduled. |

**Modified**

| Path | Change |
| --- | --- |
| `src/atx_db/migrations/registry.py` | Import and splat `_MIGRATIONS_0304` … `_MIGRATIONS_0309` (two lines per task that adds one). |
| `src/atx_db/delisting.py` | `SHUMWAY_PERFORMANCE_DELISTING_RETURN`, four new `DELIST_CODE_ROWS`, the `performance_unknown` policy row, `apply_performance_delisting_policy`, `DelistingTerminalReturnOptions.performance_delisting_return`. |
| `src/atx_db/quality/checks_survivorship.py` | `SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME` spec added to `survivorship_check_specs`. |
| `src/atx_db/quality/_checks.py` | Import and concatenate `identity_check_specs as _identity_check_specs`. |
| `src/atx_db/quality/__init__.py` | Add `_identity_check_specs` and `checks_identities` to the pop list. |
| `src/atx_db/activation.py` | `ActivationOptions` gains `gleif_level1_file`, `gleif_level2_file`, `openfigi_mapping_file`; four new stages; `STAGE_ORDER` extended. |
| `src/atx_db/jobs.py` | Register `UniverseUsListedDataset`, `DelistingEvidenceDataset`, `LeiAliasDataset`, `FigiAliasDataset`; unregister `MarketCapDataset` / `EnterpriseValueDataset` / `ValuationMultiplesDataset` (Task 10). |
| `src/atx_db/api/catalog.py` | `SECURITY_MASTER_SCHEMA`, `UNIVERSE_SCHEMA`, `DELISTINGS_SCHEMA`; attach to the two `DatasetSpec`s. |
| `src/atx_db/provider_coverage.py` | `ProviderCoverageSlo.item_count_basis`; six new `DEFAULT_PROVIDER_COVERAGE_SLOS` rows; coverage-gate item count in `refresh_provider_coverage`. |
| `src/atx_db/cli.py` | `atx-db publish-release` subcommand. |
| `src/atx_db/lake.py` | `DEFAULT_EXPORT_OBJECTS` gains `universe_us_listed_membership` and `delisting_evidence`. |
| `tests/data/public_api_snapshot.json` | +4 module names (Tasks 2, 3, 6, 8), −2 module names (Task 10). |
| `README.md`, `docs/PRODUCTION_RUNBOOK.md` | Universe / delisting / publication / data-dictionary sections. |
| `docs/PARITY_GAP.md`, `docs/WAREHOUSE_PARITY_NEXT_AGENT_README.md`, `docs/WAREHOUSE_PARITY_TRANCHES.md`, `docs/ROADMAP_PARITY.md` | Historical banners. |
| `.github/workflows/atx-db.yml` | Data-dictionary check step; ruff/mypy over the new modules. |

**Deleted (Task 10 only)**

`src/atx_db/abnormal_capex.py`, `src/atx_db/operating_leverage.py`, `tests/test_abnormal_capex.py`, `tests/test_operating_leverage.py`.

---
### Task 1: Migration 0304 — `universe_us_listed_membership`, catalog rows, lake contract, quality registry

**Files:**
- Create: `src/atx_db/migrations/bodies_0304.py`
- Modify: `src/atx_db/migrations/registry.py`
- Modify: `src/atx_db/lake.py` (`DEFAULT_EXPORT_OBJECTS`)
- Test: `tests/test_universe_us_listed.py` (new; this task adds only the schema section)

**Interfaces:**
- Consumes: `atx_db.migrations._runner.Migration`, `atx_db.migrations.bodies_0001_0137._catalog_fields_for_tables`, `atx_db.migrations.bodies_0140_0143._refresh_schema_contract_v2_pin` (all existing).
- Produces:
  - Table `universe_us_listed_membership(membership_id VARCHAR PRIMARY KEY, universe_id VARCHAR NOT NULL, security_id VARCHAR NOT NULL, symbol VARCHAR, valid_from DATE NOT NULL, valid_to DATE, available_at TIMESTAMP NOT NULL, security_type VARCHAR NOT NULL, exchange_code VARCHAR NOT NULL, has_cik BOOLEAN NOT NULL, cik VARCHAR, market_cap_decile INTEGER, reason VARCHAR NOT NULL, rules_json VARCHAR NOT NULL, decision_count INTEGER NOT NULL, as_of_date DATE NOT NULL, source VARCHAR NOT NULL, run_id VARCHAR, source_loaded_at TIMESTAMP NOT NULL DEFAULT now())`
  - `table_catalog` + `dataset_catalog` + `field_catalog` rows for it, a `lake_partition_specs` row partitioned on `as_of_date`, and two `quality_check_registry` rows: `universe_us_listed_overlapping_intervals` (critical, `eq` 0) and `universe_us_listed_missing_decile` (warning, `eq` 0).
  - `atx_db.migrations.bodies_0304.MIGRATIONS: list[Migration]` — one `Migration(version=304, name="universe_us_listed_membership", up=_universe_us_listed_membership)`.
  - `atx_db.lake.DEFAULT_EXPORT_OBJECTS` gains `"universe_us_listed_membership"`.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_universe_us_listed.py`:

```python
"""Tier1-S4 T1/T2: the point-in-time US-listed universe."""

from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest


UNIVERSE_TABLE = "universe_us_listed_membership"

EXPECTED_COLUMNS = (
    "membership_id",
    "universe_id",
    "security_id",
    "symbol",
    "valid_from",
    "valid_to",
    "available_at",
    "security_type",
    "exchange_code",
    "has_cik",
    "cik",
    "market_cap_decile",
    "reason",
    "rules_json",
    "decision_count",
    "as_of_date",
    "source",
    "run_id",
    "source_loaded_at",
)


def _columns(store, relation):
    rows = store.con.execute(
        """
        SELECT column_name
        FROM duckdb_columns()
        WHERE schema_name = 'main' AND table_name = ?
        ORDER BY column_index
        """,
        [relation],
    ).fetchall()
    return tuple(str(row[0]) for row in rows)


def test_membership_table_has_the_pit_interval_shape(tmp_store):
    assert _columns(tmp_store, UNIVERSE_TABLE) == EXPECTED_COLUMNS


def test_membership_table_is_catalogued(tmp_store):
    row = tmp_store.con.execute(
        "SELECT layer, grain, natural_key_json FROM table_catalog WHERE table_name = ?",
        [UNIVERSE_TABLE],
    ).fetchone()
    assert row is not None
    assert row[0] == "serving"
    assert row[1] == "universe_id,security_id,valid_from"
    assert json.loads(row[2]) == ["universe_id", "security_id", "valid_from"]


def test_membership_fields_are_catalogued(tmp_store):
    count = tmp_store.con.execute(
        "SELECT count(*) FROM field_catalog WHERE table_name = ?",
        [UNIVERSE_TABLE],
    ).fetchone()[0]
    assert int(count) == len(EXPECTED_COLUMNS)


def test_membership_has_a_lake_partition_spec(tmp_store):
    row = tmp_store.con.execute(
        "SELECT partition_columns_json, watermark_column FROM lake_partition_specs WHERE object_name = ?",
        [UNIVERSE_TABLE],
    ).fetchone()
    assert row is not None
    assert json.loads(row[0]) == ["as_of_date"]
    assert row[1] == "available_at"


@pytest.mark.parametrize(
    "check_name,severity",
    [
        ("universe_us_listed_overlapping_intervals", "critical"),
        ("universe_us_listed_missing_decile", "warning"),
    ],
)
def test_membership_quality_checks_are_registered(tmp_store, check_name, severity):
    row = tmp_store.con.execute(
        "SELECT severity, threshold_value, comparator, enabled "
        "FROM quality_check_registry WHERE check_name = ?",
        [check_name],
    ).fetchone()
    assert row is not None
    assert row[0] == severity
    assert float(row[1]) == 0.0
    assert row[2] == "eq"
    assert bool(row[3]) is True


def test_membership_is_a_default_lake_export_object():
    from atx_db.lake import DEFAULT_EXPORT_OBJECTS

    assert UNIVERSE_TABLE in DEFAULT_EXPORT_OBJECTS
```

- [ ] **Step 2: Run it and watch it fail**

Run: `.venv\Scripts\python.exe -m pytest tests/test_universe_us_listed.py -n 0 -q`
Expected: FAIL — `test_membership_table_has_the_pit_interval_shape` compares `()` to the 19-column tuple because the relation does not exist, and `test_membership_is_a_default_lake_export_object` fails on the missing entry.

- [ ] **Step 3: Create the migration body**

Create `C:\atx\atx-db\src\atx_db\migrations\bodies_0304.py`:

```python
"""Point-in-time US-listed universe membership intervals."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _universe_us_listed_membership(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS universe_us_listed_membership (
            membership_id VARCHAR PRIMARY KEY,
            universe_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            valid_from DATE NOT NULL,
            valid_to DATE,
            available_at TIMESTAMP NOT NULL,
            security_type VARCHAR NOT NULL,
            exchange_code VARCHAR NOT NULL,
            has_cik BOOLEAN NOT NULL,
            cik VARCHAR,
            market_cap_decile INTEGER,
            reason VARCHAR NOT NULL,
            rules_json VARCHAR NOT NULL,
            decision_count INTEGER NOT NULL,
            as_of_date DATE NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE INDEX IF NOT EXISTS idx_universe_us_listed_membership_asof
            ON universe_us_listed_membership(universe_id, valid_from, valid_to);
        CREATE INDEX IF NOT EXISTS idx_universe_us_listed_membership_security
            ON universe_us_listed_membership(security_id, valid_from);
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "universe_us_listed_membership",
                "serving",
                "universe_membership",
                "universe_id,security_id,valid_from",
                "Interval-keyed point-in-time US-listed equity universe: one row per "
                "contiguous run of identical (security_type, exchange_code, has_cik, reason) "
                "state for a security. Members with no resolved CIK are retained with "
                "has_cik=false and reason='member_no_cik' so the unresolved tail is counted, "
                "never dropped.",
                '["universe_id","security_id","valid_from"]',
                "valid_from/valid_to are economic dates on the archive trading-session grid; "
                "available_at is the earliest timestamp a consumer could have known the "
                "interval opened. market_cap_decile is the decile AT valid_from only and must "
                "never be read as a per-date attribute; use market_daily_metrics.market_cap "
                "for a dated decile.",
            )
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id,source_system_id,name,description,grain,primary_table,
            pit_column,available_at_column,updated_at
        ) VALUES (?,?,?,?,?,?,'as_of_date','available_at',now())
        """,
        [
            (
                "universe_us_listed",
                "atx_derived",
                "US-listed equity universe",
                "Securities with at least one trade in the trailing lookback window on an "
                "eligible US exchange, restricted to common/ADR/REIT/LP security types.",
                "universe_id,security_id,valid_from",
                "universe_us_listed_membership",
            )
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO lake_partition_specs (
            object_name,partition_columns_json,watermark_column,updated_at
        ) VALUES (?,?,'available_at',now())
        """,
        [("universe_us_listed_membership", '["as_of_date"]')],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name,dataset_id,table_name,severity,threshold_value,
            comparator,enabled,failure_status,source,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,now())
        """,
        [
            (
                "universe_us_listed_overlapping_intervals",
                "universe_us_listed",
                "universe_us_listed_membership",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_tier1_parity",
            ),
            (
                "universe_us_listed_missing_decile",
                "universe_us_listed",
                "universe_us_listed_membership",
                "warning",
                0.0,
                "eq",
                True,
                "warning",
                "atx_tier1_parity",
            ),
        ],
    )
    _catalog_fields_for_tables(conn, ("universe_us_listed_membership",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=304,
        name="universe_us_listed_membership",
        up=_universe_us_listed_membership,
    )
]
```

- [ ] **Step 4: Register the migration**

In `C:\atx\atx-db\src\atx_db\migrations\registry.py`, add the import beside the other body imports, in numeric order:

```python
from .bodies_0304 import MIGRATIONS as _MIGRATIONS_0304
```

and append the splat as the final entry of the `MIGRATIONS` list, after `*_MIGRATIONS_0303,`:

```python
    *_MIGRATIONS_0304,
```

- [ ] **Step 5: Add the lake export object**

In `C:\atx\atx-db\src\atx_db\lake.py`, inside `DEFAULT_EXPORT_OBJECTS`, insert `"universe_us_listed_membership",` immediately after the existing `"listing_status_intervals",` entry so the reference block stays grouped by domain.

- [ ] **Step 6: Run the test**

Run: `.venv\Scripts\python.exe -m pytest tests/test_universe_us_listed.py -n 0 -q`
Expected: PASS — `7 passed`. The first run rebuilds the fingerprinted schema template (~3 minutes); later runs are ~1 s.

- [ ] **Step 7: Run the governance guards**

Run: `.venv\Scripts\python.exe -m pytest tests/test_migration_governance.py tests/test_schema_contract.py tests/test_quality_smoke.py -n 0 -q --run-slow`
Expected: PASS — the registry is still ascending and unique, and the contract pin matches the new column set.

- [ ] **Step 8: Commit**

```
git add src/atx_db/migrations/bodies_0304.py src/atx_db/migrations/registry.py src/atx_db/lake.py tests/test_universe_us_listed.py
git commit -m "feat(db): migration 0304 universe_us_listed_membership interval table

Adds the point-in-time US-listed universe membership table, its catalog and
field-catalog rows, a lake partition spec on as_of_date, and the overlapping-
interval (critical) and missing-decile (warning) quality-check registry rows.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: `universe_us_listed.py` — classification, PIT intervals, deciles, accessor

**Files:**
- Create: `src/atx_db/universe_us_listed.py`
- Create: `scripts/build_universe_us_listed.py`
- Modify: `src/atx_db/jobs.py` (`DATASET_REGISTRY`, `DATASET_DEPENDENCIES`, one option factory)
- Modify: `src/atx_db/activation.py` (`stage_universe_us_listed`, `STAGES`, `STAGE_ORDER`)
- Modify: `tests/data/public_api_snapshot.json` (+`universe_us_listed`)
- Test: `tests/test_universe_us_listed.py` (extend)

**Interfaces:**
- Consumes: `atx_db.connection.DuckDBStore`; `atx_db.dataset.Dataset`, `DatasetLoadResult`; `atx_db.warehouse.insert_frame`, `json_dumps`, `quality_check`, `symbol_key`; `atx_db.clock.resolve_as_of_date`; `atx_db.market_daily.MARKET_DAILY_SOURCE_NAME` (Sprint 3 Task 6); `atx_db.activation.StageResult`, `ActivationOptions`, `STAGES`, `STAGE_ORDER` (Sprint 1 Tasks 3/5/7).
- Produces:
  - `atx_db.universe_us_listed.UNIVERSE_SOURCE_NAME: str = "atx-db us-listed universe builder"`
  - `atx_db.universe_us_listed.DEFAULT_US_LISTED_UNIVERSE_ID: str = "us_listed_v1"`
  - `atx_db.universe_us_listed.EXCHANGE_CODE_BY_DIRECTORY_EXCHANGE: dict[str, str]` — `{"NASDAQ": "XNAS", "N": "XNYS", "A": "XASE", "P": "ARCX", "Z": "BATS"}`
  - `atx_db.universe_us_listed.EXCHANGE_LABELS: dict[str, str]` — `{"XNAS": "NASDAQ", "XNYS": "NYSE", "XASE": "NYSE American", "ARCX": "ARCA", "BATS": "BATS"}`
  - `atx_db.universe_us_listed.ELIGIBLE_EXCHANGE_CODES: tuple[str, ...]` — sorted `("ARCX", "BATS", "XASE", "XNAS", "XNYS")`
  - `atx_db.universe_us_listed.ELIGIBLE_SECURITY_TYPES: tuple[str, ...]` — `("ADR", "LP", "REIT", "common")`
  - `atx_db.universe_us_listed.SECURITY_TYPE_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...]` — the ordered classification table.
  - `atx_db.universe_us_listed.classify_security_type(security_name: object, *, etf: object = None, test_issue: object = None) -> str`
  - `atx_db.universe_us_listed.exchange_code_for(directory_exchange: object) -> str | None`
  - `atx_db.universe_us_listed.UniverseUsListedOptions` — frozen dataclass: `universe_id: str = DEFAULT_US_LISTED_UNIVERSE_ID`, `name: str`, `description: str`, `lookback_days: int = 20`, `market_source: str = MARKET_DAILY_SOURCE_NAME`, `start_date: dt.date | None = None`, `end_date: dt.date | None = None`, `security_ids: tuple[str, ...] | None = None`, `source: str = UNIVERSE_SOURCE_NAME`, `as_of_date: dt.date | None = None`, `run_id: str | None = None`.
  - `atx_db.universe_us_listed.UNIVERSE_OUTPUT_COLUMNS: tuple[str, ...]` — the 18 insertable columns of `universe_us_listed_membership` (everything except `source_loaded_at`).
  - `atx_db.universe_us_listed.build_universe_decision_sql(*, has_security_filter: bool, has_start_date: bool, has_end_date: bool) -> str`
  - `atx_db.universe_us_listed.load_universe_decisions(store, options) -> pd.DataFrame`
  - `atx_db.universe_us_listed.compute_universe_us_listed_intervals(decisions: pd.DataFrame, sessions: pd.DataFrame, options: UniverseUsListedOptions) -> pd.DataFrame`
  - `atx_db.universe_us_listed.refresh_universe_us_listed(store, options: UniverseUsListedOptions | None = None) -> int`
  - `atx_db.universe_us_listed.universe_us_listed(store, as_of_date: dt.date, *, universe_id: str = DEFAULT_US_LISTED_UNIVERSE_ID, require_cik: bool = False) -> pd.DataFrame`
  - `atx_db.universe_us_listed.UniverseUsListedDataset(Dataset)` with `dataset_id = "universe_us_listed"`.
  - `atx_db.jobs._universe_us_listed_options(params: dict[str, Any]) -> UniverseUsListedOptions`; `DATASET_REGISTRY["universe_us_listed"]`; `DATASET_DEPENDENCIES["universe_us_listed"] = ("market_daily", "tbltickerhistory_daily")`.
  - `atx_db.activation.stage_universe_us_listed(store, options) -> StageResult`; `STAGE_ORDER` gains `"universe_us_listed"` immediately before `"provider_coverage"`.

**Membership contract.** A security is a member on session `S` when all of the following hold on the bar that anchors the decision:

1. It has at least one `equity_daily_bars` row within the trailing `lookback_days` trading sessions of the archive session grid (`sessions` = the distinct `trade_date` values of `equity_daily_bars`, ranked ascending). Decisions are emitted on the security's own bar sessions; the trailing window is realised by extending `valid_to` forward by `lookback_days - 1` sessions when an interval closes.
2. Its `exchange_code`, mapped from the newest `nasdaq_symbol_directory` snapshot with `as_of_date <= S`, is in `ELIGIBLE_EXCHANGE_CODES`.
3. Its `security_type`, classified from that same snapshot row, is in `ELIGIBLE_SECURITY_TYPES`.

`has_cik` is `securities.entity_id LIKE 'CIK-%'` and `cik` is the suffix. Members with `has_cik = false` are **written** with `reason = 'member_no_cik'` — they are the market universe's unresolved tail, retained and countable, never dropped. Securities failing (2) or (3) are not written; their counts land in the `quality_check` details and the `DatasetLoadResult`, so the exclusion is reported rather than silent.

- [ ] **Step 1: Write the failing classification tests**

Append to `C:\atx\atx-db\tests\test_universe_us_listed.py`:

```python
CLASSIFICATION_CASES = (
    ("Apple Inc. - Common Stock", "common"),
    ("Alphabet Inc. - Class C Capital Stock", "common"),
    ("Simon Property Group, Inc. Common Stock", "common"),
    ("Taiwan Semiconductor Manufacturing Company Ltd. American Depositary Shares", "ADR"),
    ("Banco Santander, S.A. ADR", "ADR"),
    ("Prologis, Inc. Common Stock (REIT)", "REIT"),
    ("Realty Income Corporation Real Estate Investment Trust", "REIT"),
    ("Enterprise Products Partners L.P.", "LP"),
    ("Energy Transfer LP Common Units", "unit"),
    ("Bank of America Corporation Depositary Shares Series GG", "preferred"),
    ("Wells Fargo & Company 7.5% Preferred Series L", "preferred"),
    ("Churchill Capital Corp VII Warrant", "warrant"),
    ("Ajax Capital Rights", "right"),
    ("iShares Core S&P 500 ETF", "fund"),
    ("iPath Series B S&P 500 VIX Short-Term Futures ETN", "ETN"),
    ("Morgan Stanley Emerging Markets Domestic Debt Fund, Inc.", "fund"),
    ("Goldman Sachs Group 6.125% Notes due 2060", "note"),
)


@pytest.mark.parametrize("security_name,expected", CLASSIFICATION_CASES)
def test_classify_security_type(security_name, expected):
    from atx_db.universe_us_listed import classify_security_type

    assert classify_security_type(security_name) == expected


def test_etf_flag_beats_the_name():
    from atx_db.universe_us_listed import classify_security_type

    assert classify_security_type("Vanguard Total Stock Market", etf=True) == "ETF"


def test_test_issue_flag_wins_outright():
    from atx_db.universe_us_listed import classify_security_type

    assert classify_security_type("Apple Inc. - Common Stock", test_issue=True) == "test"


def test_only_four_security_types_are_eligible():
    from atx_db.universe_us_listed import ELIGIBLE_SECURITY_TYPES

    assert ELIGIBLE_SECURITY_TYPES == ("ADR", "LP", "REIT", "common")


@pytest.mark.parametrize(
    "raw,expected",
    [
        ("NASDAQ", "XNAS"),
        ("N", "XNYS"),
        ("A", "XASE"),
        ("P", "ARCX"),
        ("Z", "BATS"),
        ("V", None),
        ("", None),
        (None, None),
    ],
)
def test_exchange_code_for(raw, expected):
    from atx_db.universe_us_listed import exchange_code_for

    assert exchange_code_for(raw) == expected
```

- [ ] **Step 2: Run it and watch it fail**

Run: `.venv\Scripts\python.exe -m pytest tests/test_universe_us_listed.py -n 0 -q -k "classify or exchange_code or eligible"`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.universe_us_listed'` on every case.

- [ ] **Step 3: Write the classification half of the module**

Create `C:\atx\atx-db\src\atx_db\universe_us_listed.py`:

```python
"""Point-in-time US-listed equity universe.

Implements the Tier-1 spec's ``universe_us_listed(as_of_date)``: securities with at
least one trade in the trailing ``lookback_days`` trading sessions, listed on NYSE,
NASDAQ, NYSE American, ARCA or BATS, whose security type is common stock, an ADR, a
REIT or an LP. ETFs, ETNs, closed-end funds, warrants, rights, units, preferreds and
notes are excluded. Securities with no resolved CIK stay in the universe (it is the
market universe) but carry ``has_cik = false`` so the fundamentals universe can subset
without losing the tail.

Everything here is deterministic: the trading-session grid is the archive's own
distinct ``equity_daily_bars.trade_date`` values, classification is an ordered regex
table, and the interval writer emits a stable-sorted frame.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import re
from dataclasses import asdict, dataclass

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .market_daily import MARKET_DAILY_SOURCE_NAME
from .warehouse import insert_frame, json_dumps, quality_check, symbol_key


UNIVERSE_SOURCE_NAME = "atx-db us-listed universe builder"
DEFAULT_US_LISTED_UNIVERSE_ID = "us_listed_v1"

# nasdaq_symbol_directory.exchange is 'NASDAQ' for nasdaqlisted.txt and the raw CQS
# venue letter for otherlisted.txt. 'V' (IEX) is deliberately absent: the spec's
# eligible venue set is NYSE, NASDAQ, NYSE American, ARCA and BATS.
EXCHANGE_CODE_BY_DIRECTORY_EXCHANGE: dict[str, str] = {
    "NASDAQ": "XNAS",
    "N": "XNYS",
    "A": "XASE",
    "P": "ARCX",
    "Z": "BATS",
}
EXCHANGE_LABELS: dict[str, str] = {
    "XNAS": "NASDAQ",
    "XNYS": "NYSE",
    "XASE": "NYSE American",
    "ARCX": "ARCA",
    "BATS": "BATS",
}
ELIGIBLE_EXCHANGE_CODES: tuple[str, ...] = tuple(sorted(EXCHANGE_LABELS))
ELIGIBLE_SECURITY_TYPES: tuple[str, ...] = ("ADR", "LP", "REIT", "common")

# Ordered classification table; first match wins, so the exclusions that can masquerade
# as an eligible type (a preferred ADS, a partnership *unit*) are tested first.
SECURITY_TYPE_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("ETN", re.compile(r"\bETNS?\b|EXCHANGE[- ]TRADED NOTE")),
    ("preferred", re.compile(r"\bPREFERRED\b|\bPREFERENCE\b|\bPFD\b|DEPOSITARY (?:SHARE|SHS|SHARES)")),
    ("warrant", re.compile(r"\bWARRANTS?\b|\bWTS?\b")),
    ("right", re.compile(r"\bRIGHTS?\b")),
    ("unit", re.compile(r"\bUNITS?\b")),
    ("note", re.compile(r"\bNOTES?\b|\bDEBENTURES?\b|\bBONDS?\b|\bSUBORDINATED\b")),
    ("ADR", re.compile(r"AMERICAN DEPOSITARY|AMERICAN DEPOSITORY|\bADR\b|\bADS\b")),
    ("REIT", re.compile(r"\bREIT\b|REAL ESTATE INVESTMENT TRUST")),
    ("LP", re.compile(r"\bL\.?P\.?\b|LIMITED PARTNERSHIP")),
    ("fund", re.compile(r"\bETFS?\b|\bFUND\b|CLOSED[- ]END|\bINDEX TRUST\b|\bPORTFOLIO\b")),
)


def _clean_name(value: object) -> str:
    if value is None:
        return ""
    try:
        if pd.isna(value):
            return ""
    except (TypeError, ValueError):
        pass
    return str(value).strip().upper()


def _flag(value: object) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return False
    try:
        if pd.isna(value):
            return False
    except (TypeError, ValueError):
        pass
    return str(value).strip().lower() in {"1", "t", "true", "y", "yes"}


def classify_security_type(
    security_name: object,
    *,
    etf: object = None,
    test_issue: object = None,
) -> str:
    """Classify one listing line into a security type.

    Precedence: the directory's own ``test_issue`` and ``etf`` booleans outrank the
    name, then :data:`SECURITY_TYPE_PATTERNS` is walked in order, then ``"common"``.
    Pure and total -- always returns a label, never raises.
    """

    if _flag(test_issue):
        return "test"
    if _flag(etf):
        return "ETF"
    name = _clean_name(security_name)
    if not name:
        return "unknown"
    for label, pattern in SECURITY_TYPE_PATTERNS:
        if pattern.search(name):
            return label
    return "common"


def exchange_code_for(directory_exchange: object) -> str | None:
    """Map ``nasdaq_symbol_directory.exchange`` to an eligible MIC-style code, or None."""

    raw = _clean_name(directory_exchange)
    if not raw:
        return None
    return EXCHANGE_CODE_BY_DIRECTORY_EXCHANGE.get(raw)
```

- [ ] **Step 4: Run the classification tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_universe_us_listed.py -n 0 -q -k "classify or exchange_code or eligible"`
Expected: PASS — `29 passed` (17 classification cases, 8 exchange cases, plus the flag and eligibility tests).

- [ ] **Step 5: Write the failing interval-compression tests**

Append to `C:\atx\atx-db\tests\test_universe_us_listed.py`:

```python
def _sessions(dates):
    return pd.DataFrame(
        {
            "trade_date": [dt.date.fromisoformat(d) for d in dates],
            "session_rank": range(1, len(dates) + 1),
        }
    )


def _decision(security_id, date, rank, **overrides):
    row = {
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date.fromisoformat(date),
        "session_rank": rank,
        "available_at": pd.Timestamp(f"{date} 22:00:00"),
        "security_type": "common",
        "exchange_code": "XNAS",
        "has_cik": True,
        "cik": "0000320193",
        "market_cap_decile": 9,
    }
    row.update(overrides)
    return row


SESSION_DATES = [
    "2024-01-02", "2024-01-03", "2024-01-04", "2024-01-05", "2024-01-08",
    "2024-01-09", "2024-01-10", "2024-01-11", "2024-01-12", "2024-01-16",
]


def _options(**overrides):
    from atx_db.universe_us_listed import UniverseUsListedOptions

    return UniverseUsListedOptions(lookback_days=3, run_id="test-run", **overrides)


def test_contiguous_decisions_collapse_to_one_interval():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", "2024-01-02", 1),
            _decision("SEC-1", "2024-01-03", 2),
            _decision("SEC-1", "2024-01-04", 3),
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(SESSION_DATES), _options())
    assert len(out) == 1
    row = out.iloc[0]
    assert row["valid_from"] == dt.date(2024, 1, 2)
    # last bar is session 3; lookback_days=3 extends membership to session 5.
    assert row["valid_to"] == dt.date(2024, 1, 8)
    assert int(row["decision_count"]) == 3
    assert row["reason"] == "member"


def test_a_state_change_opens_a_new_interval_with_no_gap():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", "2024-01-02", 1, exchange_code="XNYS"),
            _decision("SEC-1", "2024-01-03", 2, exchange_code="XNYS"),
            _decision("SEC-1", "2024-01-04", 3, exchange_code="XNAS"),
            _decision("SEC-1", "2024-01-05", 4, exchange_code="XNAS"),
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(SESSION_DATES), _options())
    assert list(out["exchange_code"]) == ["XNYS", "XNAS"]
    assert out.iloc[0]["valid_to"] == dt.date(2024, 1, 3)
    assert out.iloc[1]["valid_from"] == dt.date(2024, 1, 4)


def test_a_gap_longer_than_the_lookback_splits_the_interval():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", "2024-01-02", 1),
            _decision("SEC-1", "2024-01-12", 9),
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(SESSION_DATES), _options())
    assert len(out) == 2
    assert out.iloc[0]["valid_to"] == dt.date(2024, 1, 4)
    assert out.iloc[1]["valid_from"] == dt.date(2024, 1, 12)


def test_an_interval_reaching_the_archive_end_stays_open():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame([_decision("SEC-1", "2024-01-16", 10)])
    out = compute_universe_us_listed_intervals(decisions, _sessions(SESSION_DATES), _options())
    assert pd.isna(out.iloc[0]["valid_to"])


def test_the_unresolved_cik_tail_is_retained_and_labelled():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame(
        [_decision("SEC-2", "2024-01-02", 1, has_cik=False, cik=None)]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(SESSION_DATES), _options())
    assert len(out) == 1
    assert bool(out.iloc[0]["has_cik"]) is False
    assert out.iloc[0]["reason"] == "member_no_cik"


def test_the_decile_is_taken_at_valid_from_only():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame(
        [
            _decision("SEC-1", "2024-01-02", 1, market_cap_decile=4),
            _decision("SEC-1", "2024-01-03", 2, market_cap_decile=7),
        ]
    )
    out = compute_universe_us_listed_intervals(decisions, _sessions(SESSION_DATES), _options())
    assert len(out) == 1
    assert int(out.iloc[0]["market_cap_decile"]) == 4


def test_output_is_row_order_independent_and_stably_sorted():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    rows = [
        _decision("SEC-2", "2024-01-03", 2),
        _decision("SEC-1", "2024-01-02", 1),
        _decision("SEC-1", "2024-01-03", 2),
    ]
    sessions = _sessions(SESSION_DATES)
    first = compute_universe_us_listed_intervals(pd.DataFrame(rows), sessions, _options())
    second = compute_universe_us_listed_intervals(
        pd.DataFrame(list(reversed(rows))), sessions, _options()
    )
    pd.testing.assert_frame_equal(first, second)
    assert list(first["security_id"]) == ["SEC-1", "SEC-2"]


def test_membership_id_is_a_stable_content_hash():
    from atx_db.universe_us_listed import compute_universe_us_listed_intervals

    decisions = pd.DataFrame([_decision("SEC-1", "2024-01-02", 1)])
    sessions = _sessions(SESSION_DATES)
    a = compute_universe_us_listed_intervals(decisions, sessions, _options())
    b = compute_universe_us_listed_intervals(decisions, sessions, _options())
    assert a.iloc[0]["membership_id"] == b.iloc[0]["membership_id"]
    assert len(a.iloc[0]["membership_id"]) == 64
```

- [ ] **Step 6: Run and watch it fail**

Run: `.venv\Scripts\python.exe -m pytest tests/test_universe_us_listed.py -n 0 -q -k "interval or tail or decile or membership_id or state_change or gap or archive_end"`
Expected: FAIL — `ImportError: cannot import name 'compute_universe_us_listed_intervals'`.

- [ ] **Step 7: Add the options, the decision SQL and the interval compressor**

Append to `C:\atx\atx-db\src\atx_db\universe_us_listed.py`:

```python
UNIVERSE_OUTPUT_COLUMNS: tuple[str, ...] = (
    "membership_id",
    "universe_id",
    "security_id",
    "symbol",
    "valid_from",
    "valid_to",
    "available_at",
    "security_type",
    "exchange_code",
    "has_cik",
    "cik",
    "market_cap_decile",
    "reason",
    "rules_json",
    "decision_count",
    "as_of_date",
    "source",
    "run_id",
)

_INTERVAL_STATE_COLUMNS = ("security_type", "exchange_code", "has_cik", "reason")


@dataclass(frozen=True)
class UniverseUsListedOptions:
    universe_id: str = DEFAULT_US_LISTED_UNIVERSE_ID
    name: str = "US-listed equity universe"
    description: str = (
        "Securities with at least one trade in the trailing lookback window on NYSE, "
        "NASDAQ, NYSE American, ARCA or BATS, restricted to common stock, ADRs, REITs "
        "and LPs. Members without a resolved CIK are retained and flagged."
    )
    lookback_days: int = 20
    market_source: str = MARKET_DAILY_SOURCE_NAME
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    security_ids: tuple[str, ...] | None = None
    source: str = UNIVERSE_SOURCE_NAME
    as_of_date: dt.date | None = None
    run_id: str | None = None


def _rules(options: UniverseUsListedOptions) -> dict[str, object]:
    return {
        "lookback_days": options.lookback_days,
        "eligible_exchange_codes": list(ELIGIBLE_EXCHANGE_CODES),
        "eligible_security_types": list(ELIGIBLE_SECURITY_TYPES),
        "market_source": options.market_source,
        "decile_basis": "market_daily_metrics.market_cap at valid_from",
    }


def _empty_output() -> pd.DataFrame:
    return pd.DataFrame(columns=list(UNIVERSE_OUTPUT_COLUMNS))


def _membership_id(*parts: object) -> str:
    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def compute_universe_us_listed_intervals(
    decisions: pd.DataFrame,
    sessions: pd.DataFrame,
    options: UniverseUsListedOptions,
) -> pd.DataFrame:
    """Compress per-session universe decisions into deterministic validity intervals.

    ``decisions`` carries one row per (security_id, trading session the security
    traded on) with the attributes resolved as of that session:
    ``security_id, symbol, as_of_date, session_rank, available_at, security_type,
    exchange_code, has_cik, cik, market_cap_decile``. ``sessions`` is the archive
    trading-session grid (``trade_date``, ``session_rank``), ascending and gap-free in
    rank.

    A new interval opens when any of ``_INTERVAL_STATE_COLUMNS`` changes or when the
    session-rank gap to the previous decision exceeds ``lookback_days`` -- the latter is
    the spec's "at least one trade in the prior N trading days" rule expressed on the
    grid. An interval closes at ``min(last decision rank + lookback_days - 1, archive
    end)``; an interval whose extension reaches the last known session stays OPEN
    (``valid_to`` NULL) because the archive cannot prove the name stopped trading.
    ``market_cap_decile`` is the decile observed at ``valid_from`` only (deviation 3).

    Pure and stable-sorted: the same decisions in any row order yield a byte-identical
    frame.
    """

    if decisions is None or decisions.empty or sessions is None or sessions.empty:
        return _empty_output()

    grid = sessions.copy()
    grid["session_rank"] = grid["session_rank"].astype(int)
    grid = grid.sort_values("session_rank", kind="mergesort").reset_index(drop=True)
    rank_to_date = dict(zip(grid["session_rank"], grid["trade_date"]))
    last_rank = int(grid["session_rank"].iloc[-1])

    frame = decisions.copy()
    frame["security_id"] = frame["security_id"].astype("string")
    frame["session_rank"] = frame["session_rank"].astype(int)
    frame["has_cik"] = frame["has_cik"].astype(bool)
    frame["reason"] = ["member" if flag else "member_no_cik" for flag in frame["has_cik"]]
    frame = frame.sort_values(
        ["security_id", "session_rank"], kind="mergesort"
    ).reset_index(drop=True)

    rules_json = json_dumps(_rules(options))
    rows: list[dict[str, object]] = []

    def close(current: dict[str, object], final_rank: int) -> None:
        extended = final_rank + options.lookback_days - 1
        current["valid_to"] = None if extended >= last_rank else rank_to_date[extended]
        rows.append({key: value for key, value in current.items() if not key.startswith("_")})

    for security_id, group in frame.groupby("security_id", sort=True, dropna=False):
        current: dict[str, object] | None = None
        previous_rank: int | None = None
        for row in group.itertuples(index=False):
            state = tuple(getattr(row, column) for column in _INTERVAL_STATE_COLUMNS)
            rank = int(row.session_rank)
            gapped = previous_rank is not None and (rank - previous_rank) > options.lookback_days
            if current is None or current["_state"] != state or gapped:
                if current is not None:
                    close(current, int(previous_rank))
                current = {
                    "_state": state,
                    "membership_id": _membership_id(
                        options.universe_id, security_id, row.as_of_date, *state
                    ),
                    "universe_id": options.universe_id,
                    "security_id": str(security_id),
                    "symbol": symbol_key(getattr(row, "symbol", None)),
                    "valid_from": row.as_of_date,
                    "valid_to": None,
                    "available_at": pd.Timestamp(row.available_at).to_pydatetime(),
                    "security_type": str(row.security_type),
                    "exchange_code": str(row.exchange_code),
                    "has_cik": bool(row.has_cik),
                    "cik": getattr(row, "cik", None),
                    "market_cap_decile": None
                    if pd.isna(getattr(row, "market_cap_decile", None))
                    else int(row.market_cap_decile),
                    "reason": str(row.reason),
                    "rules_json": rules_json,
                    "decision_count": 0,
                    "as_of_date": row.as_of_date,
                    "source": options.source,
                    "run_id": options.run_id,
                }
            current["decision_count"] = int(current["decision_count"]) + 1
            previous_rank = rank
        if current is not None:
            close(current, int(previous_rank))

    if not rows:
        return _empty_output()
    return (
        pd.DataFrame.from_records(rows, columns=list(UNIVERSE_OUTPUT_COLUMNS))
        .sort_values(["security_id", "valid_from"], kind="mergesort")
        .reset_index(drop=True)
    )
```

- [ ] **Step 8: Run the interval tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_universe_us_listed.py -n 0 -q -k "interval or tail or decile or membership_id or state_change or gap or archive_end"`
Expected: PASS — `8 passed`.

- [ ] **Step 9: Add the decision loader, the refresh, the accessor and the dataset**

Append to `C:\atx\atx-db\src\atx_db\universe_us_listed.py`:

```python
def build_universe_decision_sql(
    *,
    has_security_filter: bool,
    has_start_date: bool,
    has_end_date: bool,
) -> str:
    """SQL for the per-session universe decision grid.

    One row per (security_id, session the security traded on) with the newest listing
    snapshot visible on that session, the CIK resolution from ``securities.entity_id``
    and the market-cap decile from ``market_daily_metrics``. Placeholder order:
    ``[market_source]`` then, when present, ``[start_date]``, ``[end_date]``.
    """

    filters = [
        "b.security_id IS NOT NULL",
        "b.trade_date IS NOT NULL",
        "b.close IS NOT NULL",
        "b.close > 0",
    ]
    if has_security_filter:
        filters.append("b.security_id IN (SELECT security_id FROM universe_security_filter)")
    bar_window = []
    if has_start_date:
        bar_window.append("s.trade_date >= ?")
    if has_end_date:
        bar_window.append("s.trade_date <= ?")
    emit = f"WHERE {' AND '.join(bar_window)}" if bar_window else ""
    return f"""
        WITH sessions AS (
            SELECT trade_date,
                   row_number() OVER (ORDER BY trade_date) AS session_rank
            FROM (SELECT DISTINCT trade_date FROM equity_daily_bars WHERE trade_date IS NOT NULL)
        ),
        bars AS (
            SELECT
                b.security_id,
                b.trade_date,
                any_value(b.symbol ORDER BY b.available_at DESC, b.source DESC) AS symbol,
                min(b.available_at) AS available_at
            FROM equity_daily_bars b
            WHERE {" AND ".join(filters)}
            GROUP BY b.security_id, b.trade_date
        ),
        directory AS (
            SELECT
                d.symbol,
                d.as_of_date,
                d.security_name,
                d.exchange,
                d.etf,
                d.test_issue,
                row_number() OVER (
                    PARTITION BY d.symbol, d.as_of_date
                    ORDER BY d.directory, d.source_loaded_at DESC
                ) AS rn
            FROM nasdaq_symbol_directory d
        ),
        deciles AS (
            SELECT
                m.security_id,
                m.trade_date,
                ntile(10) OVER (PARTITION BY m.trade_date ORDER BY m.market_cap) AS market_cap_decile
            FROM market_daily_metrics m
            WHERE m.source = ? AND m.market_cap IS NOT NULL AND m.is_latest_revision
        ),
        listing AS (
            SELECT
                bars.security_id,
                bars.trade_date,
                dir.security_name,
                dir.exchange,
                dir.etf,
                dir.test_issue,
                row_number() OVER (
                    PARTITION BY bars.security_id, bars.trade_date
                    ORDER BY dir.as_of_date DESC
                ) AS rn
            FROM bars
            JOIN directory dir
              ON dir.rn = 1
             AND dir.symbol = bars.symbol
             AND dir.as_of_date <= bars.trade_date
        )
        SELECT
            bars.security_id,
            bars.symbol,
            s.trade_date AS as_of_date,
            s.session_rank,
            bars.available_at,
            listing.security_name,
            listing.exchange,
            listing.etf,
            listing.test_issue,
            sec.entity_id LIKE 'CIK-%' AS has_cik,
            CASE WHEN sec.entity_id LIKE 'CIK-%' THEN substr(sec.entity_id, 5) END AS cik,
            deciles.market_cap_decile
        FROM bars
        JOIN sessions s ON s.trade_date = bars.trade_date
        LEFT JOIN listing
          ON listing.security_id = bars.security_id
         AND listing.trade_date = bars.trade_date
         AND listing.rn = 1
        LEFT JOIN securities sec ON sec.security_id = bars.security_id
        LEFT JOIN deciles
          ON deciles.security_id = bars.security_id
         AND deciles.trade_date = bars.trade_date
        {emit}
        ORDER BY bars.security_id, s.session_rank
    """


def load_universe_decisions(
    store: DuckDBStore,
    options: UniverseUsListedOptions,
) -> tuple[pd.DataFrame, pd.DataFrame, dict[str, int]]:
    """Run the decision SQL and apply the pure classification/eligibility filters.

    Returns ``(eligible_decisions, sessions, exclusion_counts)``. ``exclusion_counts``
    carries one entry per rejection reason so the excluded tail is reported rather than
    discarded silently.
    """

    store.initialize()
    registered = False
    if options.security_ids is not None:
        ids = sorted({str(value) for value in options.security_ids if value})
        store.con.register("universe_security_filter", pd.DataFrame({"security_id": ids}))
        registered = True
    params: list[object] = [options.market_source]
    if options.start_date is not None:
        params.append(options.start_date)
    if options.end_date is not None:
        params.append(options.end_date)
    sql = build_universe_decision_sql(
        has_security_filter=options.security_ids is not None,
        has_start_date=options.start_date is not None,
        has_end_date=options.end_date is not None,
    )
    try:
        frame = store.con.execute(sql, params).df()
        sessions = store.con.execute(
            """
            SELECT trade_date, row_number() OVER (ORDER BY trade_date) AS session_rank
            FROM (SELECT DISTINCT trade_date FROM equity_daily_bars WHERE trade_date IS NOT NULL)
            ORDER BY trade_date
            """
        ).df()
    finally:
        if registered:
            store.con.unregister("universe_security_filter")

    if frame.empty:
        return frame, sessions, {"no_bars": 0}

    frame["as_of_date"] = pd.to_datetime(frame["as_of_date"], errors="coerce").dt.date
    sessions["trade_date"] = pd.to_datetime(sessions["trade_date"], errors="coerce").dt.date
    frame["exchange_code"] = [exchange_code_for(value) for value in frame["exchange"]]
    frame["security_type"] = [
        classify_security_type(name, etf=etf, test_issue=test)
        for name, etf, test in zip(frame["security_name"], frame["etf"], frame["test_issue"])
    ]
    no_listing = frame["exchange_code"].isna()
    bad_exchange = ~no_listing & ~frame["exchange_code"].isin(ELIGIBLE_EXCHANGE_CODES)
    bad_type = ~no_listing & ~bad_exchange & ~frame["security_type"].isin(ELIGIBLE_SECURITY_TYPES)
    exclusions = {
        "no_listing_reference": int(no_listing.sum()),
        "not_eligible_exchange": int(bad_exchange.sum()),
        "not_eligible_security_type": int(bad_type.sum()),
    }
    eligible = frame[~(no_listing | bad_exchange | bad_type)].reset_index(drop=True)
    return eligible, sessions, exclusions
```

Continue appending to `C:\atx\atx-db\src\atx_db\universe_us_listed.py`:

```python
def refresh_universe_us_listed(
    store: DuckDBStore,
    options: UniverseUsListedOptions | None = None,
) -> int:
    """Rebuild ``universe_us_listed_membership`` for one universe id."""

    options = options or UniverseUsListedOptions()
    if options.lookback_days < 1:
        raise ValueError("lookback_days must be positive")
    decisions, sessions, exclusions = load_universe_decisions(store, options)
    intervals = compute_universe_us_listed_intervals(decisions, sessions, options)

    with store.transaction():
        store.con.execute("DELETE FROM universes WHERE universe_id = ?", [options.universe_id])
        store.con.execute(
            "INSERT INTO universes (universe_id, name, description, rules_json) VALUES (?, ?, ?, ?)",
            [
                options.universe_id,
                options.name,
                options.description,
                json_dumps(
                    {
                        key: (value.isoformat() if isinstance(value, dt.date) else value)
                        for key, value in asdict(options).items()
                    }
                    | {"rules": _rules(options)}
                ),
            ],
        )
        predicates = ["universe_id = ?", "source = ?"]
        params: list[object] = [options.universe_id, options.source]
        if options.start_date is not None:
            predicates.append("coalesce(valid_to, valid_from) >= ?")
            params.append(options.start_date)
        if options.end_date is not None:
            predicates.append("valid_from <= ?")
            params.append(options.end_date)
        store.con.execute(
            f"DELETE FROM universe_us_listed_membership WHERE {' AND '.join(predicates)}",
            params,
        )
        rows = 0
        if not intervals.empty:
            rows = insert_frame(
                store,
                intervals,
                "universe_us_listed_membership",
                "universe_us_listed_membership_insert",
            )

    with_cik = int(intervals["has_cik"].sum()) if not intervals.empty else 0
    quality_check(
        store,
        dataset_id="universe_us_listed",
        table_name="universe_us_listed_membership",
        check_name="rows_loaded",
        status="passed" if rows > 0 else "warning",
        observed_value=float(rows),
        threshold_value=1.0,
        details={
            "universe_id": options.universe_id,
            "intervals": rows,
            "intervals_with_cik": with_cik,
            "intervals_without_cik": rows - with_cik,
            "excluded_decisions": exclusions,
            "rules": _rules(options),
        },
    )
    return rows


def universe_us_listed(
    store: DuckDBStore,
    as_of_date: dt.date,
    *,
    universe_id: str = DEFAULT_US_LISTED_UNIVERSE_ID,
    require_cik: bool = False,
) -> pd.DataFrame:
    """The universe as it stood on ``as_of_date``.

    Only intervals whose ``available_at`` is at or before the end of ``as_of_date`` are
    visible, so the accessor is point-in-time by construction. ``require_cik=True``
    narrows to the fundamentals universe; the default returns the full market universe
    including the unresolved tail.
    """

    store.initialize()
    cutoff = dt.datetime.combine(as_of_date, dt.time(23, 59, 59))
    predicates = [
        "universe_id = ?",
        "valid_from <= ?",
        "(valid_to IS NULL OR valid_to >= ?)",
        "available_at <= ?",
    ]
    params: list[object] = [universe_id, as_of_date, as_of_date, cutoff]
    if require_cik:
        predicates.append("has_cik")
    return store.con.execute(
        f"""
        SELECT security_id, symbol, security_type, exchange_code, has_cik, cik,
               market_cap_decile, valid_from, available_at
        FROM universe_us_listed_membership
        WHERE {" AND ".join(predicates)}
        ORDER BY security_id
        """,
        params,
    ).df()


class UniverseUsListedDataset(Dataset):
    """Dataset wrapper so the universe builder is a first-class DAG node."""

    dataset_id = "universe_us_listed"
    source_name = UNIVERSE_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: UniverseUsListedOptions) -> DatasetLoadResult:
        rows = refresh_universe_us_listed(store, options)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"universe_id": options.universe_id, "rules": _rules(options)},
            run_id=options.run_id,
        )
```

- [ ] **Step 10: Write the end-to-end warehouse test**

First confirm the quality-result relation and column names actually written by `warehouse.quality_check`:

Run: `.venv\Scripts\python.exe -c "import inspect, atx_db.warehouse as w; print(inspect.getsource(w.quality_check))"`
Expected: the INSERT target and its column list; use those exact names in the assertion below instead of the placeholders if they differ.

Append to `C:\atx\atx-db\tests\test_universe_us_listed.py`:

```python
def _seed_universe_warehouse(store):
    store.con.execute(
        "INSERT INTO securities (security_id, entity_id, primary_symbol, name, source) VALUES "
        "('SEC-AAPL', 'CIK-0000320193', 'AAPL', 'Apple Inc.', 'test'),"
        "('SEC-SPY', NULL, 'SPY', 'SPDR S&P 500 ETF Trust', 'test'),"
        "('SEC-TAIL', NULL, 'TAIL', 'Tail Holdings Inc.', 'test')"
    )
    bars = []
    for day in ("2024-01-02", "2024-01-03", "2024-01-04"):
        for security_id, symbol in (("SEC-AAPL", "AAPL"), ("SEC-SPY", "SPY"), ("SEC-TAIL", "TAIL")):
            bars.append(
                f"('test','{security_id}','{symbol}',DATE '{day}',10.0,1000,"
                f"TIMESTAMP '{day} 22:00:00',DATE '{day}',true)"
            )
    store.con.execute(
        "INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, volume, "
        "available_at, as_of_date, is_latest_revision) VALUES " + ",".join(bars)
    )
    store.con.execute(
        "INSERT INTO nasdaq_symbol_directory "
        "(directory, symbol, security_name, exchange, etf, test_issue, as_of_date, source_url) VALUES "
        "('nasdaqlisted','AAPL','Apple Inc. - Common Stock','NASDAQ',false,false,DATE '2024-01-01','file://t'),"
        "('nasdaqlisted','SPY','SPDR S&P 500 ETF Trust','NASDAQ',true,false,DATE '2024-01-01','file://t'),"
        "('otherlisted','TAIL','Tail Holdings Inc. Common Stock','N',false,false,DATE '2024-01-01','file://t')"
    )


def test_refresh_writes_members_and_retains_the_unresolved_tail(tmp_store):
    from atx_db.universe_us_listed import (
        UniverseUsListedOptions,
        refresh_universe_us_listed,
        universe_us_listed,
    )

    _seed_universe_warehouse(tmp_store)
    rows = refresh_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=2, run_id="t"))
    assert rows == 2  # AAPL and TAIL; SPY is an ETF and is excluded

    members = universe_us_listed(tmp_store, dt.date(2024, 1, 3))
    assert sorted(members["security_id"]) == ["SEC-AAPL", "SEC-TAIL"]
    assert set(members["exchange_code"]) == {"XNAS", "XNYS"}

    fundamentals = universe_us_listed(tmp_store, dt.date(2024, 1, 3), require_cik=True)
    assert list(fundamentals["security_id"]) == ["SEC-AAPL"]
    assert list(fundamentals["cik"]) == ["0000320193"]


def test_refresh_reports_the_excluded_tail(tmp_store):
    from atx_db.universe_us_listed import (
        UniverseUsListedOptions,
        load_universe_decisions,
    )

    _seed_universe_warehouse(tmp_store)
    _eligible, _sessions, exclusions = load_universe_decisions(
        tmp_store, UniverseUsListedOptions(lookback_days=2, run_id="t")
    )
    assert exclusions["not_eligible_security_type"] == 3  # SPY on three sessions
    assert exclusions["not_eligible_exchange"] == 0


def test_refresh_is_idempotent(tmp_store):
    from atx_db.universe_us_listed import UniverseUsListedOptions, refresh_universe_us_listed

    _seed_universe_warehouse(tmp_store)
    options = UniverseUsListedOptions(lookback_days=2, run_id="t")
    first = refresh_universe_us_listed(tmp_store, options)
    second = refresh_universe_us_listed(tmp_store, options)
    assert first == second
    total = tmp_store.con.execute("SELECT count(*) FROM universe_us_listed_membership").fetchone()[0]
    assert int(total) == second


def test_membership_intervals_never_overlap(tmp_store):
    from atx_db.universe_us_listed import UniverseUsListedOptions, refresh_universe_us_listed

    _seed_universe_warehouse(tmp_store)
    refresh_universe_us_listed(tmp_store, UniverseUsListedOptions(lookback_days=2, run_id="t"))
    overlaps = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM universe_us_listed_membership a
        JOIN universe_us_listed_membership b
          ON a.universe_id = b.universe_id
         AND a.security_id = b.security_id
         AND a.membership_id <> b.membership_id
         AND a.valid_from <= coalesce(b.valid_to, DATE '9999-12-31')
         AND b.valid_from <= coalesce(a.valid_to, DATE '9999-12-31')
        """
    ).fetchone()[0]
    assert int(overlaps) == 0
```

- [ ] **Step 11: Run the full universe test file**

Run: `.venv\Scripts\python.exe -m pytest tests/test_universe_us_listed.py -n 0 -q`
Expected: PASS — `44 passed`.

- [ ] **Step 12: Add the operator script**

Create `C:\atx\atx-db\scripts\build_universe_us_listed.py`:

```python
"""Operator entry point for the point-in-time US-listed universe."""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path

from atx_db.clock import utc_today
from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.universe_us_listed import (
    DEFAULT_US_LISTED_UNIVERSE_ID,
    UniverseUsListedOptions,
    refresh_universe_us_listed,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="build-universe-us-listed")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--universe-id", default=DEFAULT_US_LISTED_UNIVERSE_ID)
    parser.add_argument("--lookback-days", type=int, default=20)
    parser.add_argument("--start-date", type=dt.date.fromisoformat)
    parser.add_argument("--end-date", type=dt.date.fromisoformat)
    parser.add_argument("--run-id")
    args = parser.parse_args(argv)

    options = UniverseUsListedOptions(
        universe_id=args.universe_id,
        lookback_days=args.lookback_days,
        start_date=args.start_date,
        end_date=args.end_date,
        as_of_date=args.end_date or utc_today(),
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        rows = refresh_universe_us_listed(store, options)
    print(json.dumps({"universe_id": options.universe_id, "intervals": rows}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 13: Register the dataset and the activation stage**

In `C:\atx\atx-db\src\atx_db\jobs.py`:

1. Add the import beside the other dataset imports:
   ```python
   from .universe_us_listed import UniverseUsListedDataset, UniverseUsListedOptions
   ```
2. Add the option factory beside the other `_*_options` helpers:
   ```python
   def _universe_us_listed_options(params: dict[str, Any]) -> UniverseUsListedOptions:
       default = UniverseUsListedOptions()
       return UniverseUsListedOptions(
           universe_id=params.get("universe_id", default.universe_id),
           lookback_days=int(params.get("lookback_days", default.lookback_days)),
           market_source=params.get("market_source", default.market_source),
           start_date=_as_date(params.get("start_date")),
           end_date=_as_date(params.get("end_date")),
           run_id=params.get("run_id"),
       )
   ```
   (`_as_date` is the existing helper `jobs.py` already uses for date params; confirm its name with
   `.venv\Scripts\python.exe -c "import atx_db.jobs as j; print([n for n in dir(j) if 'date' in n])"`
   and use whatever it is called.)
3. Add to `DATASET_REGISTRY`:
   ```python
   UniverseUsListedDataset.dataset_id: (UniverseUsListedDataset, _universe_us_listed_options),
   ```
4. Add to `DATASET_DEPENDENCIES`:
   ```python
   "universe_us_listed": ("market_daily", "tbltickerhistory_daily"),
   ```

In `C:\atx\atx-db\src\atx_db\activation.py`, add the stage and extend the order:

```python
def stage_universe_us_listed(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Rebuild the point-in-time US-listed universe from bars + directory + deciles."""

    from .universe_us_listed import UniverseUsListedOptions, refresh_universe_us_listed

    rows = refresh_universe_us_listed(
        store,
        UniverseUsListedOptions(as_of_date=options.as_of_date, run_id=options.run_id),
    )
    return StageResult(rows=rows, detail={"table": "universe_us_listed_membership"})


STAGES["universe_us_listed"] = stage_universe_us_listed
```

and insert `"universe_us_listed",` into `STAGE_ORDER` immediately before `"provider_coverage"` (after Sprint 3's `"market_daily"`).

- [ ] **Step 14: Refresh the public API snapshot**

Run: `.venv\Scripts\python.exe -c "import json,pathlib,atx_db; p=pathlib.Path('tests/data/public_api_snapshot.json'); d=json.loads(p.read_text()); d['atx_db']=sorted(set(d['atx_db'])|{'universe_us_listed'}); p.write_text(json.dumps(d, indent=2, sort_keys=True)+chr(10))"`
Expected: no output; `git diff --stat tests/data/public_api_snapshot.json` shows one added line.

- [ ] **Step 15: Run the affected suites**

Run: `.venv\Scripts\python.exe -m pytest tests/test_universe_us_listed.py tests/test_module_boundaries.py tests/test_import.py -n 0 -q`
Expected: PASS — `44 passed` plus the boundary and import suites green.

- [ ] **Step 16: Commit**

```
git add src/atx_db/universe_us_listed.py src/atx_db/jobs.py src/atx_db/activation.py scripts/build_universe_us_listed.py tests/test_universe_us_listed.py tests/data/public_api_snapshot.json
git commit -m "feat(db): point-in-time US-listed universe with retained unresolved tail

Adds universe_us_listed.py: an ordered security-type classification table over the
Nasdaq directory, the eligible-exchange mapping, a per-session decision grid, and a
deterministic interval compressor whose valid_to extends lookback_days trading
sessions past the last bar. Members with no resolved CIK are retained and flagged
has_cik=false; ineligible exchanges and security types are counted, not silently
dropped. Registers the builder as a DAG dataset and an activation stage.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---
### Task 3: Migration 0305 and `delisting_evidence.py` — four public evidence streams

**Files:**
- Create: `src/atx_db/migrations/bodies_0305.py`
- Create: `src/atx_db/delisting_evidence.py`
- Create: `scripts/build_delisting_evidence.py`
- Modify: `src/atx_db/migrations/registry.py`, `src/atx_db/lake.py`, `src/atx_db/jobs.py`, `src/atx_db/activation.py`, `tests/data/public_api_snapshot.json`
- Test: `tests/test_delisting_evidence.py` (new)

**Interfaces:**
- Consumes: `atx_db.connection.DuckDBStore`; `atx_db.dataset.Dataset`, `DatasetLoadResult`; `atx_db.warehouse.insert_frame`, `json_dumps`, `quality_check`; `atx_db.delisting.seed_delist_code_dim` (existing); `atx_db.activation.StageResult`, `ActivationOptions`, `STAGES`, `STAGE_ORDER`.
- Produces:
  - Table `delisting_evidence(evidence_id VARCHAR PRIMARY KEY, source VARCHAR NOT NULL, security_id VARCHAR, symbol VARCHAR NOT NULL, evidence_kind VARCHAR NOT NULL, evidence_rank INTEGER NOT NULL, delist_date DATE NOT NULL, reason_category VARCHAR NOT NULL, reason_confidence VARCHAR NOT NULL, delist_code VARCHAR NOT NULL, evidence_source_table VARCHAR NOT NULL, source_event_id VARCHAR, as_of_date DATE NOT NULL, available_at TIMESTAMP NOT NULL, details_json VARCHAR, run_id VARCHAR, source_loaded_at TIMESTAMP NOT NULL DEFAULT now())`, its catalog rows and a `lake_partition_specs` row on `as_of_date`.
  - `atx_db.migrations.bodies_0305.MIGRATIONS` — `Migration(version=305, name="delisting_evidence", up=_delisting_evidence)`.
  - `atx_db.delisting_evidence.DELISTING_EVIDENCE_SOURCE: str = "atx_delisting_evidence_v1"`
  - `atx_db.delisting_evidence.DELISTING_EVENT_SOURCE: str = "atx_delisting_public_evidence_v1"`
  - `atx_db.delisting_evidence.ARCHIVE_GAP_SESSIONS: int = 30`
  - `atx_db.delisting_evidence.MERGER_LOOKBACK_DAYS: int = 365`
  - `atx_db.delisting_evidence.MERGER_FORMS: tuple[str, ...]` = `("425", "DEFM14A", "S-4", "S-4/A", "SC 14D9", "SC TO-T")`
  - `atx_db.delisting_evidence.EVIDENCE_PRECEDENCE: tuple[tuple[str, int, str, str, str], ...]` — `(evidence_kind, evidence_rank, delist_code, default_reason_category, reason_confidence)` for `sec_form_25`, `nasdaq_delete`, `sec_form_15`, `archive_last_trade`.
  - `atx_db.delisting_evidence.REASON_CATEGORIES: tuple[str, ...]` = `("bankruptcy", "exchange_delist", "merger_acquisition", "unknown", "voluntary")`
  - `atx_db.delisting_evidence.EVIDENCE_COLUMNS: tuple[str, ...]` — the 16 insertable columns.
  - `atx_db.delisting_evidence.DelistingEvidenceOptions` — frozen dataclass: `source: str = DELISTING_EVIDENCE_SOURCE`, `event_source: str = DELISTING_EVENT_SOURCE`, `archive_gap_sessions: int = ARCHIVE_GAP_SESSIONS`, `merger_lookback_days: int = MERGER_LOOKBACK_DAYS`, `include_archive_inference: bool = True`, `as_of_date: dt.date | None = None`, `run_id: str | None = None`.
  - `atx_db.delisting_evidence.build_archive_last_trade_sql() -> str`, `build_nasdaq_delete_sql() -> str`, `build_sec_form_sql() -> str`, `build_bankruptcy_overlay_sql() -> str`
  - `atx_db.delisting_evidence.refresh_delisting_evidence(store, options=None) -> int`
  - `atx_db.delisting_evidence.fold_evidence_into_delisting_events(store, options=None) -> int`
  - `atx_db.delisting_evidence.DelistingEvidenceDataset(Dataset)` with `dataset_id = "delisting_evidence"`.
  - `atx_db.jobs._delisting_evidence_options`; `DATASET_REGISTRY["delisting_evidence"]`; `DATASET_DEPENDENCIES["delisting_evidence"] = ("tbltickerhistory_daily", "nasdaq_listing_events", "sec_submissions")`.
  - `atx_db.activation.stage_delisting_evidence`; `STAGE_ORDER` gains `"delisting_evidence"` immediately before `"universe_us_listed"`.

**Evidence and precedence contract.**

| `evidence_kind` | rank | source relation | `delist_code` | reason | confidence |
| --- | --- | --- | --- | --- | --- |
| `sec_form_25` | 1 | `sec_submissions` where `form IN ('25','25-NSE')` | `SEC_FORM_25` | `merger_acquisition` when a `MERGER_FORMS` filing for the same CIK falls within `merger_lookback_days` before the Form 25; else `exchange_delist` for `25-NSE` (exchange-initiated) and `voluntary` for a bare `25` (issuer-initiated) | high |
| `nasdaq_delete` | 2 | `nasdaq_listing_events` where any of `nasdaq_action`/`bx_action`/`psx_action` is `'D'` | `NASDAQ_DELETE` | `exchange_delist` | high |
| `sec_form_15` | 3 | `sec_submissions` where `form LIKE '15-%'` | `SEC_FORM_15` | `voluntary` | medium |
| `archive_last_trade` | 4 | `equity_daily_bars` | `ARCHIVE_LAST_TRADE` | `unknown` | low |

A **bankruptcy overlay** upgrades any row to `reason_category='bankruptcy'`, confidence `high`, when the newest `nasdaq_symbol_directory` snapshot at or before `delist_date` carries a `financial_status` containing `'Q'` (Nasdaq's bankruptcy flag). It never downgrades a higher-ranked reason to `unknown`.

`archive_last_trade` fires only when the security's last bar is more than `archive_gap_sessions` trading sessions before the archive's last session (so the archive end itself never manufactures a delist) and no later bar exists for that `security_id`.

`fold_evidence_into_delisting_events` writes one `delisting_events` row per `(security_id, delist_date)` choosing the minimum `evidence_rank`, with `listing_status_source = evidence_kind`, `source_listing_status_id = evidence_id`, `evidence_source_table` from the winning row, `method = 'public_evidence_precedence'`, `delisting_return = NULL`, `delisting_return_type = 'UNOBSERVED'`, `return_policy = 'none'`, `return_confidence = 'none'` and `inferred_from_absence = (evidence_kind = 'archive_last_trade')`. It never touches rows written by `delisting.refresh_delisting_events`, because it scopes its `DELETE` to `source = options.event_source`.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_delisting_evidence.py`:

```python
"""Tier1-S4 T3: public delisting evidence streams and their precedence fold."""

from __future__ import annotations

import datetime as dt

import pytest


def _seed_bars(store, security_id, symbol, days):
    values = ",".join(
        f"('test','{security_id}','{symbol}',DATE '{day}',10.0,1000,"
        f"TIMESTAMP '{day} 22:00:00',DATE '{day}',true)"
        for day in days
    )
    store.con.execute(
        "INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, volume, "
        "available_at, as_of_date, is_latest_revision) VALUES " + values
    )


def _business_days(start, count):
    day = dt.date.fromisoformat(start)
    out = []
    while len(out) < count:
        if day.weekday() < 5:
            out.append(day.isoformat())
        day += dt.timedelta(days=1)
    return out


SESSIONS = _business_days("2024-01-01", 60)


def _seed_two_securities(store):
    store.con.execute(
        "INSERT INTO securities (security_id, entity_id, primary_symbol, name, source) VALUES "
        "('SEC-LIVE','CIK-0000000001','LIVE','Live Corp','test'),"
        "('SEC-GONE','CIK-0000000002','GONE','Gone Corp','test')"
    )
    _seed_bars(store, "SEC-LIVE", "LIVE", SESSIONS)
    _seed_bars(store, "SEC-GONE", "GONE", SESSIONS[:10])


def test_archive_last_trade_fires_only_past_the_gap(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    rows = refresh_delisting_evidence(
        tmp_store, DelistingEvidenceOptions(archive_gap_sessions=30, run_id="t")
    )
    assert rows == 1
    row = tmp_store.con.execute(
        "SELECT security_id, evidence_kind, delist_date, reason_category, reason_confidence, "
        "evidence_rank, delist_code FROM delisting_evidence"
    ).fetchone()
    assert row[0] == "SEC-GONE"
    assert row[1] == "archive_last_trade"
    assert row[2] == dt.date.fromisoformat(SESSIONS[9])
    assert row[3] == "unknown"
    assert row[4] == "low"
    assert int(row[5]) == 4
    assert row[6] == "ARCHIVE_LAST_TRADE"


def test_a_security_still_trading_at_the_archive_end_is_never_inferred(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    live = tmp_store.con.execute(
        "SELECT count(*) FROM delisting_evidence WHERE security_id = 'SEC-LIVE'"
    ).fetchone()[0]
    assert int(live) == 0


def test_a_short_gap_is_not_a_delist(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    rows = refresh_delisting_evidence(
        tmp_store, DelistingEvidenceOptions(archive_gap_sessions=55, run_id="t")
    )
    assert rows == 0


def test_form_25_nse_is_an_exchange_delist(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, form, "
        "acceptance_datetime, source_url) VALUES "
        "('SEC-GONE','0000000002','0000000002-24-000001',DATE '2024-01-16','25-NSE',"
        "TIMESTAMP '2024-01-16 17:00:00','file://t')"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    row = tmp_store.con.execute(
        "SELECT reason_category, evidence_rank, available_at FROM delisting_evidence "
        "WHERE evidence_kind = 'sec_form_25'"
    ).fetchone()
    assert row[0] == "exchange_delist"
    assert int(row[1]) == 1
    assert row[2] == dt.datetime(2024, 1, 16, 17, 0, 0)


def test_form_25_after_a_merger_filing_is_a_merger(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, form, "
        "acceptance_datetime, source_url) VALUES "
        "('SEC-GONE','0000000002','0000000002-23-000009',DATE '2023-11-01','DEFM14A',"
        "TIMESTAMP '2023-11-01 17:00:00','file://t'),"
        "('SEC-GONE','0000000002','0000000002-24-000001',DATE '2024-01-16','25',"
        "TIMESTAMP '2024-01-16 17:00:00','file://t')"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    reason = tmp_store.con.execute(
        "SELECT reason_category FROM delisting_evidence WHERE evidence_kind = 'sec_form_25'"
    ).fetchone()[0]
    assert reason == "merger_acquisition"


def test_a_bare_form_25_with_no_merger_evidence_is_voluntary(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, form, "
        "acceptance_datetime, source_url) VALUES "
        "('SEC-GONE','0000000002','0000000002-24-000001',DATE '2024-01-16','25',"
        "TIMESTAMP '2024-01-16 17:00:00','file://t')"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    reason = tmp_store.con.execute(
        "SELECT reason_category FROM delisting_evidence WHERE evidence_kind = 'sec_form_25'"
    ).fetchone()[0]
    assert reason == "voluntary"
```

Continue appending to `C:\atx\atx-db\tests\test_delisting_evidence.py`:

```python
def test_nasdaq_delete_and_form_15_are_captured(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO nasdaq_listing_events (event_id, symbol, security_id, nasdaq_action, "
        "effective_date, as_of_date, source_url) VALUES "
        "('EV-1','GONE','SEC-GONE','D',DATE '2024-01-17',DATE '2024-01-17','file://t')"
    )
    tmp_store.con.execute(
        "INSERT INTO sec_submissions (security_id, cik, accession_number, filing_date, form, "
        "acceptance_datetime, source_url) VALUES "
        "('SEC-GONE','0000000002','0000000002-24-000002',DATE '2024-02-01','15-12B',"
        "TIMESTAMP '2024-02-01 17:00:00','file://t')"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    kinds = {
        str(row[0]): (str(row[1]), int(row[2]))
        for row in tmp_store.con.execute(
            "SELECT evidence_kind, reason_category, evidence_rank FROM delisting_evidence"
        ).fetchall()
    }
    assert kinds["nasdaq_delete"] == ("exchange_delist", 2)
    assert kinds["sec_form_15"] == ("voluntary", 3)
    assert kinds["archive_last_trade"] == ("unknown", 4)


def test_bankruptcy_overlay_upgrades_the_reason(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO nasdaq_symbol_directory (directory, symbol, security_name, exchange, etf, "
        "test_issue, financial_status, as_of_date, source_url) VALUES "
        "('nasdaqlisted','GONE','Gone Corp - Common Stock','NASDAQ',false,false,'Q',"
        "DATE '2024-01-10','file://t')"
    )
    refresh_delisting_evidence(tmp_store, DelistingEvidenceOptions(run_id="t"))
    row = tmp_store.con.execute(
        "SELECT reason_category, reason_confidence FROM delisting_evidence"
    ).fetchone()
    assert row[0] == "bankruptcy"
    assert row[1] == "high"


def test_the_fold_keeps_the_highest_precedence_evidence(tmp_store):
    from atx_db.delisting_evidence import (
        DelistingEvidenceOptions,
        fold_evidence_into_delisting_events,
        refresh_delisting_evidence,
    )

    _seed_two_securities(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO nasdaq_listing_events (event_id, symbol, security_id, nasdaq_action, "
        "effective_date, as_of_date, source_url) VALUES "
        "('EV-1','GONE','SEC-GONE','D',DATE '2024-01-12',DATE '2024-01-12','file://t')"
    )
    options = DelistingEvidenceOptions(run_id="t")
    refresh_delisting_evidence(tmp_store, options)
    events = fold_evidence_into_delisting_events(tmp_store, options)
    assert events == 2  # one per distinct (security_id, delist_date)
    winner = tmp_store.con.execute(
        "SELECT listing_status_source, delist_code, delist_reason, inferred_from_absence "
        "FROM delisting_events WHERE source = ? AND delist_date = DATE '2024-01-12'",
        [options.event_source],
    ).fetchone()
    assert winner[0] == "nasdaq_delete"
    assert winner[1] == "NASDAQ_DELETE"
    assert winner[2] == "exchange_delist"
    assert bool(winner[3]) is False


def test_the_fold_never_touches_the_listing_status_source(tmp_store):
    from atx_db.delisting_evidence import (
        DelistingEvidenceOptions,
        fold_evidence_into_delisting_events,
        refresh_delisting_evidence,
    )

    tmp_store.con.execute(
        "INSERT INTO delisting_events (delisting_event_id, source, listing_status_source, "
        "source_listing_status_id, symbol, delist_date, as_of_date, available_at, delist_code, "
        "delist_reason, delisting_return_type, return_policy, return_confidence, evidence_source, "
        "evidence_source_table, method, evidence_confidence) VALUES "
        "('legacy','atx_delisting_proxy_v1','legacy_src','legacy_id','OLD',DATE '2020-01-01',"
        "DATE '2020-01-01',TIMESTAMP '2020-01-01 22:00:00','NASDAQ_DELETE','exchange_delete',"
        "'UNOBSERVED','none','none','listing_status_intervals','listing_status_intervals',"
        "'trading_system_delete_action','high')"
    )
    _seed_two_securities(tmp_store)
    options = DelistingEvidenceOptions(run_id="t")
    refresh_delisting_evidence(tmp_store, options)
    fold_evidence_into_delisting_events(tmp_store, options)
    survivors = tmp_store.con.execute(
        "SELECT count(*) FROM delisting_events WHERE source = 'atx_delisting_proxy_v1'"
    ).fetchone()[0]
    assert int(survivors) == 1


def test_refresh_is_idempotent(tmp_store):
    from atx_db.delisting_evidence import DelistingEvidenceOptions, refresh_delisting_evidence

    _seed_two_securities(tmp_store)
    options = DelistingEvidenceOptions(run_id="t")
    first = refresh_delisting_evidence(tmp_store, options)
    second = refresh_delisting_evidence(tmp_store, options)
    assert first == second
    total = tmp_store.con.execute("SELECT count(*) FROM delisting_evidence").fetchone()[0]
    assert int(total) == second
```

- [ ] **Step 2: Run and watch it fail**

Run: `.venv\Scripts\python.exe -m pytest tests/test_delisting_evidence.py -n 0 -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.delisting_evidence'` on all 11 tests.

- [ ] **Step 3: Create migration 0305**

Create `C:\atx\atx-db\src\atx_db\migrations\bodies_0305.py`:

```python
"""Public delisting evidence streams with explicit precedence."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _delisting_evidence(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS delisting_evidence (
            evidence_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR,
            symbol VARCHAR NOT NULL,
            evidence_kind VARCHAR NOT NULL,
            evidence_rank INTEGER NOT NULL,
            delist_date DATE NOT NULL,
            reason_category VARCHAR NOT NULL,
            reason_confidence VARCHAR NOT NULL,
            delist_code VARCHAR NOT NULL,
            evidence_source_table VARCHAR NOT NULL,
            source_event_id VARCHAR,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            details_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE INDEX IF NOT EXISTS idx_delisting_evidence_security
            ON delisting_evidence(security_id, delist_date, evidence_rank);
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "delisting_evidence",
                "reference",
                "delisting_evidence",
                "security_id,delist_date,evidence_kind",
                "One row per independent piece of public evidence that a security stopped "
                "trading: SEC Form 25/25-NSE, a Nasdaq Trader delete action, SEC Form 15, or "
                "a last-trade gap in the ticker-history archive. evidence_rank encodes "
                "precedence (1 = strongest); delisting_events keeps the minimum rank per "
                "(security_id, delist_date).",
                '["security_id","delist_date","evidence_kind"]',
                "available_at is the filing acceptance time for SEC evidence, the Nasdaq "
                "file creation time for delete actions, and last trade_date + 22 hours for "
                "archive inference. It is never a wall-clock read.",
            )
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id,source_system_id,name,description,grain,primary_table,
            pit_column,available_at_column,updated_at
        ) VALUES (?,?,?,?,?,?,'as_of_date','available_at',now())
        """,
        [
            (
                "delisting_evidence",
                "atx_derived",
                "Public delisting evidence",
                "Attributed public evidence for delisting date and reason.",
                "security_id,delist_date,evidence_kind",
                "delisting_evidence",
            )
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO lake_partition_specs (
            object_name,partition_columns_json,watermark_column,updated_at
        ) VALUES (?,?,'available_at',now())
        """,
        [("delisting_evidence", '["as_of_date"]')],
    )
    _catalog_fields_for_tables(conn, ("delisting_evidence",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=305, name="delisting_evidence", up=_delisting_evidence)
]
```

Register it in `registry.py` exactly as in Task 1 Step 4 (`from .bodies_0305 import MIGRATIONS as _MIGRATIONS_0305`, then `*_MIGRATIONS_0305,`), and add `"delisting_evidence",` to `lake.DEFAULT_EXPORT_OBJECTS` immediately after the existing `"delisting_events",` entry.

- [ ] **Step 4: Write the evidence module — constants and the four SQL builders**

Create `C:\atx\atx-db\src\atx_db\delisting_evidence.py`:

```python
"""Public delisting evidence: four independent streams, one precedence fold.

``delisting.refresh_delisting_events`` derives delist dates from
``listing_status_intervals``, i.e. from Nasdaq Trader evidence alone, and can never say
*why* a name stopped trading. This module adds three further public streams -- SEC Form
25 / 25-NSE, SEC Form 15, and a last-trade gap in the ticker-history archive -- attributes
a reason to each, and folds them into ``delisting_events`` under a fixed precedence.

Every timestamp is sourced: SEC acceptance datetimes, the Nasdaq file creation time, or
last ``trade_date`` + 22 hours (the archive's own end-of-day convention). Nothing here
reads a clock.
"""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .delisting import seed_delist_code_dim
from .warehouse import json_dumps, quality_check


DELISTING_EVIDENCE_SOURCE = "atx_delisting_evidence_v1"
DELISTING_EVENT_SOURCE = "atx_delisting_public_evidence_v1"
ARCHIVE_GAP_SESSIONS = 30
MERGER_LOOKBACK_DAYS = 365
END_OF_DAY_HOURS = 22

REASON_CATEGORIES: tuple[str, ...] = (
    "bankruptcy",
    "exchange_delist",
    "merger_acquisition",
    "unknown",
    "voluntary",
)

# Forms whose presence shortly before a Form 25 makes the delist a merger/acquisition.
MERGER_FORMS: tuple[str, ...] = ("425", "DEFM14A", "S-4", "S-4/A", "SC 14D9", "SC TO-T")

# (evidence_kind, evidence_rank, delist_code, default_reason_category, reason_confidence)
EVIDENCE_PRECEDENCE: tuple[tuple[str, int, str, str, str], ...] = (
    ("sec_form_25", 1, "SEC_FORM_25", "exchange_delist", "high"),
    ("nasdaq_delete", 2, "NASDAQ_DELETE", "exchange_delist", "high"),
    ("sec_form_15", 3, "SEC_FORM_15", "voluntary", "medium"),
    ("archive_last_trade", 4, "ARCHIVE_LAST_TRADE", "unknown", "low"),
)

EVIDENCE_COLUMNS: tuple[str, ...] = (
    "evidence_id",
    "source",
    "security_id",
    "symbol",
    "evidence_kind",
    "evidence_rank",
    "delist_date",
    "reason_category",
    "reason_confidence",
    "delist_code",
    "evidence_source_table",
    "source_event_id",
    "as_of_date",
    "available_at",
    "details_json",
    "run_id",
)


@dataclass(frozen=True)
class DelistingEvidenceOptions:
    source: str = DELISTING_EVIDENCE_SOURCE
    event_source: str = DELISTING_EVENT_SOURCE
    archive_gap_sessions: int = ARCHIVE_GAP_SESSIONS
    merger_lookback_days: int = MERGER_LOOKBACK_DAYS
    include_archive_inference: bool = True
    as_of_date: dt.date | None = None
    run_id: str | None = None


def _evidence_id_expression(kind_literal: str) -> str:
    """Deterministic content hash for one evidence row."""

    return (
        "sha256(concat_ws('|', "
        f"'{kind_literal}', coalesce(security_id, ''), symbol, CAST(delist_date AS VARCHAR)"
        "))"
    )


def build_archive_last_trade_sql() -> str:
    """Last bar per security, more than ``?`` sessions before the archive's last session.

    Placeholder order: ``[archive_gap_sessions]``.
    """

    return f"""
        WITH sessions AS (
            SELECT trade_date,
                   row_number() OVER (ORDER BY trade_date) AS session_rank
            FROM (SELECT DISTINCT trade_date FROM equity_daily_bars WHERE trade_date IS NOT NULL)
        ),
        archive_end AS (SELECT max(session_rank) AS last_rank FROM sessions),
        last_bar AS (
            SELECT
                b.security_id,
                max(b.trade_date) AS delist_date,
                any_value(b.symbol ORDER BY b.trade_date DESC) AS symbol
            FROM equity_daily_bars b
            WHERE b.security_id IS NOT NULL AND b.trade_date IS NOT NULL AND b.close IS NOT NULL
            GROUP BY b.security_id
        )
        SELECT
            last_bar.security_id,
            last_bar.symbol,
            last_bar.delist_date,
            CAST(last_bar.delist_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR AS available_at,
            'equity_daily_bars' AS evidence_source_table,
            CAST(NULL AS VARCHAR) AS source_event_id,
            archive_end.last_rank - s.session_rank AS gap_sessions
        FROM last_bar
        JOIN sessions s ON s.trade_date = last_bar.delist_date
        CROSS JOIN archive_end
        WHERE archive_end.last_rank - s.session_rank > ?
        ORDER BY last_bar.security_id
    """


def build_nasdaq_delete_sql() -> str:
    """Nasdaq Trader delete actions on any of the three venue columns."""

    return """
        SELECT
            e.security_id,
            e.symbol,
            coalesce(e.effective_date, e.as_of_date) AS delist_date,
            coalesce(
                e.source_file_created_at,
                CAST(coalesce(e.effective_date, e.as_of_date) AS TIMESTAMP) + INTERVAL 22 HOUR
            ) AS available_at,
            'nasdaq_listing_events' AS evidence_source_table,
            e.event_id AS source_event_id
        FROM nasdaq_listing_events e
        WHERE coalesce(e.effective_date, e.as_of_date) IS NOT NULL
          AND (
            upper(coalesce(e.nasdaq_action, '')) = 'D'
            OR upper(coalesce(e.bx_action, '')) = 'D'
            OR upper(coalesce(e.psx_action, '')) = 'D'
          )
        ORDER BY e.security_id, delist_date, e.event_id
    """
```

Continue appending to `C:\atx\atx-db\src\atx_db\delisting_evidence.py`:

```python
def build_sec_form_sql(*, form_kind: str) -> str:
    """SEC Form 25/25-NSE or Form 15 evidence.

    ``form_kind`` is ``"form_25"`` or ``"form_15"``. For ``form_25`` the placeholder
    order is ``[*MERGER_FORMS, merger_lookback_days]``; ``form_15`` takes none.
    """

    if form_kind not in {"form_25", "form_15"}:
        raise ValueError(f"unknown form_kind: {form_kind!r}")
    if form_kind == "form_15":
        return """
            SELECT
                s.security_id,
                coalesce(sec.primary_symbol, s.security_id) AS symbol,
                coalesce(s.filing_date, CAST(s.acceptance_datetime AS DATE)) AS delist_date,
                coalesce(
                    s.acceptance_datetime,
                    CAST(s.filing_date AS TIMESTAMP) + INTERVAL 22 HOUR
                ) AS available_at,
                'sec_submissions' AS evidence_source_table,
                s.accession_number AS source_event_id,
                'voluntary' AS reason_category
            FROM sec_submissions s
            LEFT JOIN securities sec ON sec.security_id = s.security_id
            WHERE s.form LIKE '15-%'
              AND coalesce(s.filing_date, CAST(s.acceptance_datetime AS DATE)) IS NOT NULL
            ORDER BY s.security_id, delist_date, s.accession_number
        """
    placeholders = ", ".join("?" for _ in MERGER_FORMS)
    return f"""
        WITH form25 AS (
            SELECT
                s.security_id,
                s.cik,
                s.form,
                coalesce(s.filing_date, CAST(s.acceptance_datetime AS DATE)) AS delist_date,
                coalesce(
                    s.acceptance_datetime,
                    CAST(s.filing_date AS TIMESTAMP) + INTERVAL 22 HOUR
                ) AS available_at,
                s.accession_number
            FROM sec_submissions s
            WHERE s.form IN ('25', '25-NSE')
              AND coalesce(s.filing_date, CAST(s.acceptance_datetime AS DATE)) IS NOT NULL
        ),
        merger_evidence AS (
            SELECT DISTINCT f.accession_number
            FROM form25 f
            JOIN sec_submissions m
              ON m.cik = f.cik
             AND m.form IN ({placeholders})
             AND coalesce(m.filing_date, CAST(m.acceptance_datetime AS DATE)) <= f.delist_date
             AND coalesce(m.filing_date, CAST(m.acceptance_datetime AS DATE))
                 >= f.delist_date - CAST(? AS INTEGER)
        )
        SELECT
            form25.security_id,
            coalesce(sec.primary_symbol, form25.security_id) AS symbol,
            form25.delist_date,
            form25.available_at,
            'sec_submissions' AS evidence_source_table,
            form25.accession_number AS source_event_id,
            CASE
                WHEN me.accession_number IS NOT NULL THEN 'merger_acquisition'
                WHEN form25.form = '25-NSE' THEN 'exchange_delist'
                ELSE 'voluntary'
            END AS reason_category
        FROM form25
        LEFT JOIN securities sec ON sec.security_id = form25.security_id
        LEFT JOIN merger_evidence me ON me.accession_number = form25.accession_number
        ORDER BY form25.security_id, form25.delist_date, form25.accession_number
    """


def build_bankruptcy_overlay_sql() -> str:
    """Securities whose newest directory snapshot at or before the delist date is bankrupt.

    Nasdaq's ``financial_status`` flag ``Q`` means "bankrupt"; the composite codes (``EQ``,
    ``HQ``, ...) contain it, so a substring test is the right predicate.
    """

    return """
        SELECT DISTINCT e.evidence_id
        FROM delisting_evidence e
        JOIN (
            SELECT
                d.symbol,
                d.as_of_date,
                d.financial_status,
                row_number() OVER (PARTITION BY d.symbol, d.as_of_date ORDER BY d.directory) AS rn
            FROM nasdaq_symbol_directory d
            WHERE d.financial_status IS NOT NULL
        ) dir
          ON dir.rn = 1
         AND dir.symbol = e.symbol
         AND dir.as_of_date <= e.delist_date
        WHERE e.source = ?
          AND contains(upper(dir.financial_status), 'Q')
        ORDER BY e.evidence_id
    """
```

- [ ] **Step 5: Write the refresh and the precedence fold**

Continue appending to `C:\atx\atx-db\src\atx_db\delisting_evidence.py`:

```python
def _stream_insert_sql(
    kind: str,
    rank: int,
    delist_code: str,
    default_reason: str,
    confidence: str,
    *,
    body: str,
    reason_from_body: bool,
) -> str:
    """Wrap one evidence stream query in the shared INSERT projection."""

    reason = "body.reason_category" if reason_from_body else f"'{default_reason}'"
    return f"""
        INSERT OR REPLACE INTO delisting_evidence (
            {", ".join(EVIDENCE_COLUMNS)}, source_loaded_at
        )
        SELECT
            {_evidence_id_expression(kind)},
            ? AS source,
            body.security_id,
            body.symbol,
            '{kind}' AS evidence_kind,
            {rank} AS evidence_rank,
            body.delist_date,
            {reason} AS reason_category,
            '{confidence}' AS reason_confidence,
            '{delist_code}' AS delist_code,
            body.evidence_source_table,
            body.source_event_id,
            body.delist_date AS as_of_date,
            body.available_at,
            ? AS details_json,
            ? AS run_id,
            now()
        FROM ({body}) AS body
        WHERE body.symbol IS NOT NULL AND body.delist_date IS NOT NULL
        ORDER BY body.security_id, body.delist_date
    """
```

Continue appending to `C:\atx\atx-db\src\atx_db\delisting_evidence.py`:

```python
def refresh_delisting_evidence(
    store: DuckDBStore,
    options: DelistingEvidenceOptions | None = None,
) -> int:
    """Rebuild every public delisting evidence stream for this source."""

    options = options or DelistingEvidenceOptions()
    store.initialize()
    seed_delist_code_dim(store)
    details = json_dumps(
        {
            "archive_gap_sessions": options.archive_gap_sessions,
            "merger_lookback_days": options.merger_lookback_days,
            "merger_forms": list(MERGER_FORMS),
        }
    )
    by_kind = {row[0]: row for row in EVIDENCE_PRECEDENCE}

    with store.transaction():
        store.con.execute("DELETE FROM delisting_evidence WHERE source = ?", [options.source])

        kind, rank, code, reason, confidence = by_kind["sec_form_25"]
        store.con.execute(
            _stream_insert_sql(
                kind, rank, code, reason, confidence,
                body=build_sec_form_sql(form_kind="form_25"),
                reason_from_body=True,
            ),
            [options.source, details, options.run_id, *MERGER_FORMS, options.merger_lookback_days],
        )

        kind, rank, code, reason, confidence = by_kind["nasdaq_delete"]
        store.con.execute(
            _stream_insert_sql(
                kind, rank, code, reason, confidence,
                body=build_nasdaq_delete_sql(),
                reason_from_body=False,
            ),
            [options.source, details, options.run_id],
        )

        kind, rank, code, reason, confidence = by_kind["sec_form_15"]
        store.con.execute(
            _stream_insert_sql(
                kind, rank, code, reason, confidence,
                body=build_sec_form_sql(form_kind="form_15"),
                reason_from_body=True,
            ),
            [options.source, details, options.run_id],
        )

        if options.include_archive_inference:
            kind, rank, code, reason, confidence = by_kind["archive_last_trade"]
            store.con.execute(
                _stream_insert_sql(
                    kind, rank, code, reason, confidence,
                    body=build_archive_last_trade_sql(),
                    reason_from_body=False,
                ),
                [options.source, details, options.run_id, options.archive_gap_sessions],
            )

        store.con.execute(
            f"""
            UPDATE delisting_evidence
            SET reason_category = 'bankruptcy', reason_confidence = 'high'
            WHERE evidence_id IN ({build_bankruptcy_overlay_sql()})
            """,
            [options.source],
        )

        rows = int(
            store.con.execute(
                "SELECT count(*) FROM delisting_evidence WHERE source = ?", [options.source]
            ).fetchone()[0]
        )

    by_reason = {
        str(row[0]): int(row[1])
        for row in store.con.execute(
            "SELECT reason_category, count(*) FROM delisting_evidence WHERE source = ? "
            "GROUP BY reason_category ORDER BY reason_category",
            [options.source],
        ).fetchall()
    }
    quality_check(
        store,
        dataset_id="delisting_evidence",
        table_name="delisting_evidence",
        check_name="rows_loaded",
        status="passed" if rows > 0 else "warning",
        observed_value=float(rows),
        threshold_value=1.0,
        details={"source": options.source, "by_reason_category": by_reason},
    )
    return rows


def fold_evidence_into_delisting_events(
    store: DuckDBStore,
    options: DelistingEvidenceOptions | None = None,
) -> int:
    """Materialize one ``delisting_events`` row per (security_id, delist_date).

    The winner is the minimum ``evidence_rank``, ties broken by ``evidence_id`` so the
    result is stable. Rows written by ``delisting.refresh_delisting_events`` are never
    touched: the DELETE is scoped to ``options.event_source``.
    """

    options = options or DelistingEvidenceOptions()
    store.initialize()
    with store.transaction():
        store.con.execute(
            "DELETE FROM delisting_events WHERE source = ?", [options.event_source]
        )
        store.con.execute(
            """
            INSERT OR REPLACE INTO delisting_events (
                delisting_event_id, source, listing_status_source, source_listing_status_id,
                security_id, symbol, delist_date, as_of_date, available_at, delist_code,
                delist_reason, delisting_return, delisting_return_type, is_return_imputed,
                return_policy, return_confidence, evidence_source, evidence_source_table,
                source_event_id, method, evidence_confidence, inferred_from_absence,
                details_json, run_id
            )
            WITH ranked AS (
                SELECT
                    e.*,
                    row_number() OVER (
                        PARTITION BY coalesce(e.security_id, e.symbol), e.delist_date
                        ORDER BY e.evidence_rank, e.evidence_id
                    ) AS rn
                FROM delisting_evidence e
                WHERE e.source = ?
            )
            SELECT
                sha256(concat_ws('|', ?, coalesce(security_id, symbol), CAST(delist_date AS VARCHAR))),
                ? AS source,
                evidence_kind AS listing_status_source,
                evidence_id AS source_listing_status_id,
                security_id,
                symbol,
                delist_date,
                as_of_date,
                available_at,
                delist_code,
                reason_category AS delist_reason,
                CAST(NULL AS DOUBLE) AS delisting_return,
                'UNOBSERVED' AS delisting_return_type,
                false AS is_return_imputed,
                'none' AS return_policy,
                'none' AS return_confidence,
                'public_evidence' AS evidence_source,
                evidence_source_table,
                source_event_id,
                'public_evidence_precedence' AS method,
                reason_confidence AS evidence_confidence,
                evidence_kind = 'archive_last_trade' AS inferred_from_absence,
                details_json,
                ? AS run_id
            FROM ranked
            WHERE rn = 1
            ORDER BY coalesce(security_id, symbol), delist_date
            """,
            [options.source, options.event_source, options.event_source, options.run_id],
        )
        rows = int(
            store.con.execute(
                "SELECT count(*) FROM delisting_events WHERE source = ?", [options.event_source]
            ).fetchone()[0]
        )
    return rows


class DelistingEvidenceDataset(Dataset):
    dataset_id = "delisting_evidence"
    source_name = DELISTING_EVIDENCE_SOURCE

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: DelistingEvidenceOptions) -> DatasetLoadResult:
        evidence_rows = refresh_delisting_evidence(store, options)
        event_rows = fold_evidence_into_delisting_events(store, options)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=evidence_rows,
            source=options.source,
            details={"evidence_rows": evidence_rows, "delisting_events": event_rows},
            run_id=options.run_id,
        )
```

- [ ] **Step 6: Run the evidence tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_delisting_evidence.py -n 0 -q`
Expected: PASS — `11 passed`. If the archive-gap test fails by one session, print the grid with
`.venv\Scripts\python.exe -c "import datetime as dt; print(len([d for d in range(60)]))"` and re-check
that `SESSIONS[:10]` leaves at least 31 sessions of gap before the archive end.

- [ ] **Step 7: Add the operator script**

Create `C:\atx\atx-db\scripts\build_delisting_evidence.py`:

```python
"""Operator entry point for the public delisting evidence streams."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from atx_db.clock import utc_today
from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.delisting_evidence import (
    ARCHIVE_GAP_SESSIONS,
    MERGER_LOOKBACK_DAYS,
    DelistingEvidenceOptions,
    fold_evidence_into_delisting_events,
    refresh_delisting_evidence,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="build-delisting-evidence")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--archive-gap-sessions", type=int, default=ARCHIVE_GAP_SESSIONS)
    parser.add_argument("--merger-lookback-days", type=int, default=MERGER_LOOKBACK_DAYS)
    parser.add_argument("--no-archive-inference", action="store_true")
    parser.add_argument("--run-id")
    args = parser.parse_args(argv)

    options = DelistingEvidenceOptions(
        archive_gap_sessions=args.archive_gap_sessions,
        merger_lookback_days=args.merger_lookback_days,
        include_archive_inference=not args.no_archive_inference,
        as_of_date=utc_today(),
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        evidence = refresh_delisting_evidence(store, options)
        events = fold_evidence_into_delisting_events(store, options)
    print(json.dumps({"evidence_rows": evidence, "delisting_events": events}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 8: Register the dataset and the activation stage**

In `src/atx_db/jobs.py` add `from .delisting_evidence import DelistingEvidenceDataset, DelistingEvidenceOptions`, the factory

```python
def _delisting_evidence_options(params: dict[str, Any]) -> DelistingEvidenceOptions:
    default = DelistingEvidenceOptions()
    return DelistingEvidenceOptions(
        archive_gap_sessions=int(params.get("archive_gap_sessions", default.archive_gap_sessions)),
        merger_lookback_days=int(params.get("merger_lookback_days", default.merger_lookback_days)),
        include_archive_inference=bool(
            params.get("include_archive_inference", default.include_archive_inference)
        ),
        run_id=params.get("run_id"),
    )
```

the `DATASET_REGISTRY` entry `DelistingEvidenceDataset.dataset_id: (DelistingEvidenceDataset, _delisting_evidence_options),` and the dependency `"delisting_evidence": ("tbltickerhistory_daily", "nasdaq_listing_events", "sec_submissions"),`.

In `src/atx_db/activation.py`:

```python
def stage_delisting_evidence(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Build public delisting evidence and fold it into delisting_events."""

    from .delisting_evidence import (
        DelistingEvidenceOptions,
        fold_evidence_into_delisting_events,
        refresh_delisting_evidence,
    )

    evidence_options = DelistingEvidenceOptions(
        as_of_date=options.as_of_date, run_id=options.run_id
    )
    evidence = refresh_delisting_evidence(store, evidence_options)
    events = fold_evidence_into_delisting_events(store, evidence_options)
    return StageResult(rows=evidence, detail={"delisting_events": events})


STAGES["delisting_evidence"] = stage_delisting_evidence
```

and insert `"delisting_evidence",` into `STAGE_ORDER` immediately before `"universe_us_listed"`.

- [ ] **Step 9: Refresh the public API snapshot**

Run: `.venv\Scripts\python.exe -c "import json,pathlib; p=pathlib.Path('tests/data/public_api_snapshot.json'); d=json.loads(p.read_text()); d['atx_db']=sorted(set(d['atx_db'])|{'delisting_evidence'}); p.write_text(json.dumps(d, indent=2, sort_keys=True)+chr(10))"`
Expected: no output; one added line in the diff.

- [ ] **Step 10: Run the affected suites**

Run: `.venv\Scripts\python.exe -m pytest tests/test_delisting_evidence.py tests/test_delisting.py tests/test_module_boundaries.py tests/test_migration_governance.py -n 0 -q --run-slow`
Expected: PASS — `11 passed` plus the existing delisting, boundary and governance suites green (the legacy `atx_delisting_proxy_v1` rows are untouched by the new source).

- [ ] **Step 11: Commit**

```
git add src/atx_db/delisting_evidence.py src/atx_db/migrations/bodies_0305.py src/atx_db/migrations/registry.py src/atx_db/lake.py src/atx_db/jobs.py src/atx_db/activation.py scripts/build_delisting_evidence.py tests/test_delisting_evidence.py tests/data/public_api_snapshot.json
git commit -m "feat(db): public delisting evidence streams with attributed reasons

Adds migration 0305 (delisting_evidence) and delisting_evidence.py: SEC Form 25/25-NSE,
Nasdaq Trader deletes, SEC Form 15, and archive last-trade inference guarded by an
archive-end check, each carrying an attributed reason_category and a sourced
available_at. A fixed precedence fold writes one delisting_events row per
(security_id, delist_date) under its own source, leaving the listing-status proxy rows
untouched.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---
### Task 4: Migration 0306 — an explicit Shumway terminal-return policy and a non-vacuous survivorship gate

**Files:**
- Create: `src/atx_db/migrations/bodies_0306.py`
- Modify: `src/atx_db/delisting.py`
- Modify: `src/atx_db/quality/checks_survivorship.py`
- Modify: `src/atx_db/migrations/registry.py`
- Test: `tests/test_delisting_terminal_policy.py` (new)

**Interfaces:**
- Consumes: `atx_db.delisting.POLICY_TERMINAL_RETURN_COLUMNS`, `TERMINAL_RETURN_POLICY_ROWS`, `POLICY_DIM_COLUMNS`, `load_terminal_return_policy_dim`, `DELIST_CODE_ROWS`, `DEFAULT_CODE_SOURCE`, `_empty_policy_terminal_return_frame` (all existing); `atx_db.quality._types.SqlQualityCheck`; `atx_db.delisting_evidence.DELISTING_EVENT_SOURCE` (Task 3).
- Produces:
  - `atx_db.delisting.SHUMWAY_PERFORMANCE_DELISTING_RETURN: float = -0.30`
  - `atx_db.delisting.SHUMWAY_NASDAQ_PERFORMANCE_DELISTING_RETURN: float = -0.55`
  - `atx_db.delisting.PERFORMANCE_DELIST_REASONS: frozenset[str]` = `{"bankruptcy", "exchange_delist", "unknown"}`
  - `atx_db.delisting.PERFORMANCE_TERMINAL_RETURN_POLICY_CODE: str = "performance_unknown"`
  - Four new `DELIST_CODE_ROWS` entries — `SEC_FORM_25`, `SEC_FORM_15`, `ARCHIVE_LAST_TRADE`, `NASDAQ_FINANCIAL_STATUS_BANKRUPT` — so `delist_code_dim` covers every code Task 3 can emit and `delisting_code_reconciliation` stops reporting `unmapped`.
  - A seventh `TERMINAL_RETURN_POLICY_ROWS` entry `("performance_unknown", "performance_delist", "shumway_default", False, -0.30, False, "...")`.
  - `atx_db.delisting.apply_performance_delisting_policy(events: pd.DataFrame, policy_dim: pd.DataFrame, *, performance_return: float | None, reasons: frozenset[str] = PERFORMANCE_DELIST_REASONS) -> pd.DataFrame` — returns `POLICY_TERMINAL_RETURN_COLUMNS`.
  - `atx_db.delisting.DelistingTerminalReturnOptions.performance_delisting_return: float | None = None` (new field; the convention is **opt-in**).
  - `atx_db.quality.checks_survivorship.SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME: str = "delisting_events_without_terminal_return"` and its spec, appended to `survivorship_check_specs()`.
  - `atx_db.migrations.bodies_0306.MIGRATIONS` — `Migration(version=306, name="delisting_terminal_return_policy", up=_delisting_terminal_return_policy)`, which re-seeds `delist_code_dim` and `terminal_return_policy_dim` from the widened Python tuples and registers the new check.

**Why this makes the critical gate non-vacuous.** `survivorship_forward_return_drops_delisted_names` anti-joins `delisting_terminal_returns` against the panel. With a public-only warehouse that table is empty, so the anti-join is empty, so the check is GREEN while proving nothing (audit §4.4, §8 #7). The new companion check counts `delisting_events` rows that have **no** `delisting_terminal_returns` row, so the "no terminal returns at all" state is now a measured, non-zero failure instead of a silent pass. The two checks together say: *every delisting we know about has a terminal return, and every one of those is spliced into the panel.*

**The Shumway convention, stated.** Shumway (1997, *Journal of Finance* 52(1), "The Delisting Bias in CRSP Data") estimates a mean delisting return of about **−30%** for NYSE/AMEX performance-related delistings; Shumway & Warther (1999, *JF* 54(6)) estimate about **−55%** for Nasdaq. Both constants are exported; neither is applied unless `performance_delisting_return` is set, and whichever is used is recorded in `delisting_terminal_returns.terminal_return_policy` as `performance_unknown` with `return_basis='shumway_default'` so a consumer can always filter imputed rows out.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_delisting_terminal_policy.py`:

```python
"""Tier1-S4 T4: the explicit Shumway terminal-return policy and the coverage gate."""

from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


def _events():
    return pd.DataFrame(
        [
            {
                "security_id": "SEC-1",
                "symbol": "GONE",
                "delist_date": dt.date(2024, 1, 12),
                "as_of_date": dt.date(2024, 1, 12),
                "available_at": pd.Timestamp("2024-01-12 22:00:00"),
                "delist_reason": "exchange_delist",
                "successor_security_id": None,
            },
            {
                "security_id": "SEC-2",
                "symbol": "MERGED",
                "delist_date": dt.date(2024, 2, 1),
                "as_of_date": dt.date(2024, 2, 1),
                "available_at": pd.Timestamp("2024-02-01 22:00:00"),
                "delist_reason": "merger_acquisition",
                "successor_security_id": "SEC-3",
            },
        ]
    )


def _policy_dim(store):
    from atx_db.delisting import load_terminal_return_policy_dim

    return load_terminal_return_policy_dim(store)


def test_the_policy_row_exists_and_carries_the_shumway_default(tmp_store):
    from atx_db.delisting import (
        PERFORMANCE_TERMINAL_RETURN_POLICY_CODE,
        SHUMWAY_PERFORMANCE_DELISTING_RETURN,
    )

    row = tmp_store.con.execute(
        "SELECT corporate_action_type, terminal_return_basis, default_return, is_observed_required "
        "FROM terminal_return_policy_dim WHERE policy_code = ?",
        [PERFORMANCE_TERMINAL_RETURN_POLICY_CODE],
    ).fetchone()
    assert row is not None
    assert row[0] == "performance_delist"
    assert row[1] == "shumway_default"
    assert float(row[2]) == SHUMWAY_PERFORMANCE_DELISTING_RETURN
    assert bool(row[3]) is False


@pytest.mark.parametrize(
    "code", ["SEC_FORM_25", "SEC_FORM_15", "ARCHIVE_LAST_TRADE", "NASDAQ_FINANCIAL_STATUS_BANKRUPT"]
)
def test_every_evidence_delist_code_is_seeded(tmp_store, code):
    count = tmp_store.con.execute(
        "SELECT count(*) FROM delist_code_dim WHERE delist_code = ?", [code]
    ).fetchone()[0]
    assert int(count) == 1


def test_the_policy_is_off_by_default(tmp_store):
    from atx_db.delisting import apply_performance_delisting_policy

    out = apply_performance_delisting_policy(
        _events(), _policy_dim(tmp_store), performance_return=None
    )
    assert out.empty


def test_the_policy_only_fires_on_performance_reasons(tmp_store):
    from atx_db.delisting import POLICY_TERMINAL_RETURN_COLUMNS, apply_performance_delisting_policy

    out = apply_performance_delisting_policy(
        _events(), _policy_dim(tmp_store), performance_return=-0.30
    )
    assert list(out.columns) == list(POLICY_TERMINAL_RETURN_COLUMNS)
    assert list(out["security_id"]) == ["SEC-1"]
    assert float(out.iloc[0]["terminal_return"]) == pytest.approx(-0.30)
    assert out.iloc[0]["terminal_return_source"] == "policy"
    assert out.iloc[0]["terminal_return_policy"] == "performance_unknown"
    assert out.iloc[0]["return_basis"] == "shumway_default"


def test_the_policy_never_invents_an_available_at(tmp_store):
    from atx_db.delisting import apply_performance_delisting_policy

    events = _events()
    events.loc[0, "available_at"] = pd.NaT
    out = apply_performance_delisting_policy(
        events, _policy_dim(tmp_store), performance_return=-0.30
    )
    assert out.empty


def test_the_policy_is_row_order_independent(tmp_store):
    from atx_db.delisting import apply_performance_delisting_policy

    events = _events()
    policy = _policy_dim(tmp_store)
    a = apply_performance_delisting_policy(events, policy, performance_return=-0.30)
    b = apply_performance_delisting_policy(
        events.iloc[::-1].reset_index(drop=True), policy, performance_return=-0.30
    )
    pd.testing.assert_frame_equal(a, b)
```

Continue appending to `C:\atx\atx-db\tests\test_delisting_terminal_policy.py`:

```python
def _seed_one_delisting_event(store):
    store.con.execute(
        "INSERT INTO delisting_events (delisting_event_id, source, listing_status_source, "
        "source_listing_status_id, security_id, symbol, delist_date, as_of_date, available_at, "
        "delist_code, delist_reason, delisting_return_type, return_policy, return_confidence, "
        "evidence_source, evidence_source_table, method, evidence_confidence) VALUES "
        "('ev-1','atx_delisting_public_evidence_v1','nasdaq_delete','e-1','SEC-1','GONE',"
        "DATE '2024-01-12',DATE '2024-01-12',TIMESTAMP '2024-01-12 22:00:00','NASDAQ_DELETE',"
        "'exchange_delist','UNOBSERVED','none','none','public_evidence','nasdaq_listing_events',"
        "'public_evidence_precedence','high')"
    )


def _coverage_check(store):
    from atx_db.quality.checks_survivorship import (
        SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME,
        survivorship_check_specs,
    )

    spec = next(
        s
        for s in survivorship_check_specs()
        if s.check_name == SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME
    )
    return spec, float(store.con.execute(spec.sql).fetchone()[0])


def test_the_coverage_check_is_part_of_the_survivorship_specs():
    from atx_db.quality.checks_survivorship import (
        SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME,
        survivorship_check_specs,
    )

    names = {spec.check_name for spec in survivorship_check_specs()}
    assert SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME in names
    assert len(survivorship_check_specs()) == 3


def test_a_delisting_event_with_no_terminal_return_fails_the_coverage_check(tmp_store):
    _seed_one_delisting_event(tmp_store)
    spec, observed = _coverage_check(tmp_store)
    assert observed == 1.0
    assert spec.threshold == 0.0
    assert spec.comparator == "le"
    assert spec.severity == "error"


def test_the_coverage_check_clears_once_a_terminal_return_exists(tmp_store):
    _seed_one_delisting_event(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO delisting_terminal_returns (terminal_return_id, source, security_id, symbol, "
        "delist_date, as_of_date, available_at, terminal_return, terminal_return_source, "
        "terminal_return_policy, return_basis) VALUES "
        "('tr-1','atx_delisting_terminal_return_v1','SEC-1','GONE',DATE '2024-01-12',"
        "DATE '2024-01-12',TIMESTAMP '2024-01-12 22:00:00',-0.30,'policy','performance_unknown',"
        "'shumway_default')"
    )
    _spec, observed = _coverage_check(tmp_store)
    assert observed == 0.0


def test_the_coverage_check_is_registered(tmp_store):
    from atx_db.quality.checks_survivorship import SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME

    row = tmp_store.con.execute(
        "SELECT severity, threshold_value, comparator, enabled FROM quality_check_registry "
        "WHERE check_name = ?",
        [SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME],
    ).fetchone()
    assert row is not None
    assert row[0] == "error"
    assert float(row[1]) == 0.0
    assert row[2] == "le"
    assert bool(row[3]) is True
```

> Before running, confirm the `delisting_terminal_returns` column list with
> `.venv\Scripts\python.exe -c "from atx_db.delisting import TERMINAL_RETURN_COLUMNS; print(TERMINAL_RETURN_COLUMNS)"`
> and trim the INSERT above to the columns that are actually `NOT NULL`.

- [ ] **Step 2: Run and watch it fail**

Run: `.venv\Scripts\python.exe -m pytest tests/test_delisting_terminal_policy.py -n 0 -q`
Expected: FAIL — `ImportError: cannot import name 'apply_performance_delisting_policy' from 'atx_db.delisting'` and the same for `SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME`.

- [ ] **Step 3: Name the convention and widen the two dimension tuples**

In `C:\atx\atx-db\src\atx_db\delisting.py`, immediately after `DEFAULT_CODE_SOURCE`, add:

```python
# Shumway (1997), "The Delisting Bias in CRSP Data", Journal of Finance 52(1): the mean
# delisting return for NYSE/AMEX performance-related delistings is about -30%. Shumway &
# Warther (1999), JF 54(6), estimate about -55% for Nasdaq. These are conventions, not
# observations: nothing applies them unless an operator sets
# DelistingTerminalReturnOptions.performance_delisting_return, and any row produced this
# way is stamped terminal_return_policy='performance_unknown' and
# return_basis='shumway_default' so it is always filterable.
SHUMWAY_PERFORMANCE_DELISTING_RETURN = -0.30
SHUMWAY_NASDAQ_PERFORMANCE_DELISTING_RETURN = -0.55
PERFORMANCE_TERMINAL_RETURN_POLICY_CODE = "performance_unknown"
PERFORMANCE_DELIST_REASONS = frozenset({"bankruptcy", "exchange_delist", "unknown"})
```

Append four tuples to `DELIST_CODE_ROWS`, in the field order the existing `NASDAQ_DELETE` tuple uses (confirm against `seed_delist_code_dim`'s INSERT column list before pasting):

```python
    (
        "SEC_FORM_25",
        "ATX_PUBLIC_EVIDENCE",
        None,
        None,
        "SEC_FORM_25_NOTIFICATION",
        "exchange_delist",
        "SEC Form 25 / 25-NSE notification of removal from listing and registration. "
        "25-NSE is exchange-initiated; a bare 25 is issuer-initiated.",
        "DELISTED_FORM_25",
        True,
        SHUMWAY_PERFORMANCE_DELISTING_RETURN,
        "performance_related_when_no_merger_evidence",
        DEFAULT_CODE_SOURCE,
    ),
    (
        "SEC_FORM_15",
        "ATX_PUBLIC_EVIDENCE",
        None,
        None,
        "SEC_FORM_15_DEREGISTRATION",
        "voluntary",
        "SEC Form 15 deregistration / suspension of the duty to file. A voluntary exit "
        "from reporting, not a performance delisting.",
        "DEREGISTERED_FORM_15",
        False,
        None,
        "not_allowed_voluntary_deregistration",
        DEFAULT_CODE_SOURCE,
    ),
    (
        "ARCHIVE_LAST_TRADE",
        "ATX_PUBLIC_EVIDENCE",
        None,
        None,
        "ARCHIVE_TRADING_CEASED",
        "dropped",
        "Trading ceased in the ticker-history archive more than the configured session gap "
        "before the archive end, with no later bar. Lowest-confidence evidence.",
        "NO_LONGER_TRADING_IN_ARCHIVE",
        True,
        SHUMWAY_PERFORMANCE_DELISTING_RETURN,
        "performance_related_unknown_reason",
        DEFAULT_CODE_SOURCE,
    ),
    (
        "NASDAQ_FINANCIAL_STATUS_BANKRUPT",
        "ATX_PUBLIC_EVIDENCE",
        None,
        None,
        "NASDAQ_FINANCIAL_STATUS_Q",
        "bankruptcy",
        "The Nasdaq symbol-directory financial_status carried the bankruptcy flag (Q) on "
        "or before the delist date.",
        "BANKRUPT",
        True,
        SHUMWAY_PERFORMANCE_DELISTING_RETURN,
        "performance_related_bankruptcy",
        DEFAULT_CODE_SOURCE,
    ),
```

Append one row to `TERMINAL_RETURN_POLICY_ROWS`:

```python
    (
        "performance_unknown",
        "performance_delist",
        "shumway_default",
        False,
        SHUMWAY_PERFORMANCE_DELISTING_RETURN,
        False,
        "Performance-related delisting with no observed DLRET: apply the documented "
        "Shumway (1997) -30% convention. Opt-in only; never applied unless "
        "DelistingTerminalReturnOptions.performance_delisting_return is set.",
    ),
```

Add the new option field to `DelistingTerminalReturnOptions`:

```python
    performance_delisting_return: float | None = None
```

- [ ] **Step 4: Add the pure policy function**

Append to `C:\atx\atx-db\src\atx_db\delisting.py`, immediately after `apply_terminal_return_policy`:

```python
def apply_performance_delisting_policy(
    events: pd.DataFrame,
    policy_dim: pd.DataFrame,
    *,
    performance_return: float | None,
    reasons: frozenset[str] = PERFORMANCE_DELIST_REASONS,
) -> pd.DataFrame:
    """Apply the documented Shumway convention to performance-related delists.

    ``events`` carries ``security_id, symbol, delist_date, as_of_date, available_at,
    delist_reason`` (the ``delisting_events`` shape Task 3 writes) and optionally
    ``successor_security_id``. A row qualifies when its ``delist_reason`` is in
    ``reasons`` AND ``performance_return`` is not None AND the event carries a real
    ``available_at`` -- the function never invents a timestamp, exactly like
    :func:`apply_terminal_return_policy`.

    Returns a :data:`POLICY_TERMINAL_RETURN_COLUMNS` frame with
    ``terminal_return_source='policy'``, ``terminal_return_policy='performance_unknown'``
    and ``return_basis='shumway_default'``. Pure, stable-sorted, and empty whenever
    ``performance_return`` is None -- the convention is opt-in.
    """

    if performance_return is None:
        return _empty_policy_terminal_return_frame()
    if events is None or events.empty or policy_dim is None or policy_dim.empty:
        return _empty_policy_terminal_return_frame()
    if "delist_reason" not in events.columns:
        return _empty_policy_terminal_return_frame()
    policy = policy_dim[policy_dim["policy_code"] == PERFORMANCE_TERMINAL_RETURN_POLICY_CODE]
    if policy.empty:
        return _empty_policy_terminal_return_frame()
    basis = str(policy.iloc[0]["terminal_return_basis"])

    frame = events.copy().reset_index(drop=True)
    eligible = frame["delist_reason"].astype("string").isin(sorted(reasons))
    eligible &= pd.to_datetime(frame["available_at"], errors="coerce").notna()
    frame = frame[eligible]
    if frame.empty:
        return _empty_policy_terminal_return_frame()

    out = pd.DataFrame(
        {
            "security_id": frame["security_id"],
            "symbol": frame.get("symbol"),
            "delist_date": frame["delist_date"],
            "as_of_date": frame.get("as_of_date"),
            "available_at": pd.to_datetime(frame["available_at"]),
            "terminal_return": float(performance_return),
            "terminal_return_ex_div": pd.NA,
            "terminal_return_source": "policy",
            "terminal_return_policy": PERFORMANCE_TERMINAL_RETURN_POLICY_CODE,
            "crsp_dlstcd": pd.NA,
            "return_basis": basis,
            "successor_security_id": frame.get("successor_security_id"),
            "return_observation_id": pd.NA,
        }
    )
    out = out.drop_duplicates(subset=["security_id", "delist_date"], keep="first")
    return (
        out.sort_values(["security_id", "delist_date"], kind="mergesort", na_position="last")
        .reset_index(drop=True)[POLICY_TERMINAL_RETURN_COLUMNS]
    )
```

Then, inside `compute_delisting_terminal_returns`, union this frame with the existing
observed/policy frames **after** them, so an observed or corporate-action-derived return
always wins for the same `(security_id, delist_date)`. Locate the existing concat of
observed and policy frames and add the new frame as the last element of that list, keeping
the existing dedupe-by-precedence logic unchanged. Pass
`performance_return=options.performance_delisting_return` from
`refresh_delisting_terminal_returns`.

- [ ] **Step 5: Add the non-vacuous coverage check**

In `C:\atx\atx-db\src\atx_db\quality\checks_survivorship.py`, add the constant next to the two existing check-name constants:

```python
SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME = "delisting_events_without_terminal_return"
```

add the SQL beside `_SURVIVORSHIP_SQL`:

```python
# The critical drop check anti-joins delisting_terminal_returns against the panel, so an
# EMPTY delisting_terminal_returns makes it pass on an empty set -- "no delisted name with
# a known terminal return was dropped" is not "the universe is complete" (audit 4.4 / 8 #7).
# This companion check measures the other side: every delisting_events row must have a
# terminal return. Zero delistings is still zero here, but the moment any delisting evidence
# exists with no terminal return the gate goes RED instead of silently green.
_TERMINAL_RETURN_COVERAGE_SQL = """
SELECT count(*)::DOUBLE
FROM delisting_events e
LEFT JOIN delisting_terminal_returns t
  ON t.security_id = e.security_id
 AND t.delist_date = e.delist_date
WHERE e.security_id IS NOT NULL
  AND t.terminal_return_id IS NULL
"""
```

add the spec factory beside `_reconciliation_spec`:

```python
def _terminal_return_coverage_spec() -> SqlQualityCheck:
    return SqlQualityCheck(
        dataset_id="delisting_terminal_returns",
        table_name="delisting_terminal_returns",
        check_name=SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME,
        sql=_TERMINAL_RETURN_COVERAGE_SQL,
        threshold=0.0,
        comparator="le",
        required_tables=("delisting_events", "delisting_terminal_returns"),
        warn_if_missing=True,
        failure_status="failed",
        severity="error",
    )
```

and extend the public factory:

```python
    return (_survivorship_spec(), _reconciliation_spec(), _terminal_return_coverage_spec())
```

Also add a thin accessor beside the two existing ones so operators can run it alone:

```python
def delisting_terminal_return_coverage_check(
    store: DuckDBStore, *, checked_at: dt.datetime | None = None
) -> QualityResult:
    """Count delisting events carrying no terminal return (the anti-vacuity gate)."""

    return _run_single_check(store, _terminal_return_coverage_spec(), checked_at=checked_at)
```

- [ ] **Step 6: Create migration 0306**

Create `C:\atx\atx-db\src\atx_db\migrations\bodies_0306.py`:

```python
"""Explicit Shumway terminal-return policy and the non-vacuous survivorship gate."""

from __future__ import annotations

import duckdb

from ..delisting import (
    DELIST_CODE_ROWS,
    POLICY_DIM_COLUMNS,
    TERMINAL_RETURN_POLICY_ROWS,
)
from ..quality.checks_survivorship import SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME
from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _delisting_terminal_return_policy(conn: duckdb.DuckDBPyConnection) -> None:
    conn.executemany(
        f"""
        INSERT OR REPLACE INTO terminal_return_policy_dim (
            {", ".join(POLICY_DIM_COLUMNS)}
        ) VALUES ({", ".join("?" for _ in POLICY_DIM_COLUMNS)})
        """,
        [tuple(row) for row in TERMINAL_RETURN_POLICY_ROWS],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO delist_code_dim (
            delist_code, code_scheme, crsp_dlstcd, vendor_code, reason_code, reason_category,
            description, status_label, imputation_allowed, default_imputed_return,
            imputation_note, source
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)
        """,
        [tuple(row) for row in DELIST_CODE_ROWS],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name,dataset_id,table_name,severity,threshold_value,
            comparator,enabled,failure_status,source,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,now())
        """,
        [
            (
                SURVIVORSHIP_TERMINAL_RETURN_COVERAGE_CHECK_NAME,
                "delisting_terminal_returns",
                "delisting_terminal_returns",
                "error",
                0.0,
                "le",
                True,
                "failed",
                "atx_tier1_parity",
            )
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=306,
        name="delisting_terminal_return_policy",
        up=_delisting_terminal_return_policy,
    )
]
```

> **Check the two INSERT column lists against the live DDL before running** with
> `.venv\Scripts\python.exe -c "from atx_db.connection import DuckDBStore; s=DuckDBStore('build/probe.duckdb'); s.initialize(); print(s.con.execute(\"SELECT column_name FROM duckdb_columns() WHERE table_name IN ('delist_code_dim','terminal_return_policy_dim') ORDER BY table_name, column_index\").fetchall())"`
> and align them; `POLICY_DIM_COLUMNS` is already the authoritative list for the policy table.

Register it in `registry.py` (`from .bodies_0306 import MIGRATIONS as _MIGRATIONS_0306`, then `*_MIGRATIONS_0306,`).

- [ ] **Step 7: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_delisting_terminal_policy.py -n 0 -q`
Expected: PASS — `14 passed`.

- [ ] **Step 8: Run the existing delisting and quality suites**

Run: `.venv\Scripts\python.exe -m pytest tests/test_delisting.py tests/test_delisting_returns.py tests/test_quality_gating.py tests/test_quality_smoke.py -n 0 -q --run-slow`
Expected: PASS. `test_delisting_returns.py` asserts the policy-dim contents; if it pins the row count at 6, update that assertion to 7 and add the `performance_unknown` row to its expected set — the change is intentional and belongs in this commit.

- [ ] **Step 9: Commit**

```
git add src/atx_db/delisting.py src/atx_db/quality/checks_survivorship.py src/atx_db/migrations/bodies_0306.py src/atx_db/migrations/registry.py tests/test_delisting_terminal_policy.py tests/test_delisting_returns.py
git commit -m "feat(db): explicit Shumway terminal-return policy and a non-vacuous survivorship gate

Names the -30% (NYSE/AMEX) and -55% (Nasdaq) Shumway delisting-return conventions,
adds an opt-in performance_unknown terminal-return policy row and the four delist_code
rows the public evidence streams emit, and adds
delisting_events_without_terminal_return: an error-severity check that fails when a
delisting event carries no terminal return, so an empty delisting_terminal_returns can
no longer make the critical survivorship check pass on an empty anti-join.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---
### Task 5: Identifier master — GLEIF and OpenFIGI as offline activation stages

**Files:**
- Modify: `src/atx_db/activation.py`
- Modify: `src/atx_db/jobs.py`
- Modify: `scripts/warehouse_activate.py`, `src/atx_db/cli.py` (three new flags each)
- Create: `tests/data/gleif_level1_sample.csv`, `tests/data/gleif_level2_sample.csv`, `tests/data/openfigi_mapping_sample.json`
- Test: `tests/test_identifier_activation.py` (new)

**Interfaces:**
- Consumes: `atx_db.identifiers_lei.LeiAliasDataset`, `LeiLoadOptions`, `parse_gleif_file`, `SEC_REGISTRATION_AUTHORITY_ID`, `DATASET_ID` (`"identifiers_lei"`); `atx_db.identifiers_figi.FigiAliasDataset`, `FigiLoadOptions`, `parse_openfigi_file`, `DATASET_ID` (`"identifiers_figi"`); `atx_db.activation.ActivationOptions`, `StageResult`, `STAGES`, `STAGE_ORDER`.
- Produces:
  - `ActivationOptions` gains `gleif_level1_file: Path | None = None`, `gleif_level2_file: Path | None = None`, `openfigi_mapping_file: Path | None = None` (JSON-safe through the existing `ledger_params()` `Path`→`str` coercion).
  - `atx_db.activation.stage_identifiers_lei(store, options) -> StageResult`
  - `atx_db.activation.stage_identifiers_figi(store, options) -> StageResult`
  - `STAGE_ORDER` gains `"identifiers_lei"` then `"identifiers_figi"`, immediately after `"companyfacts_load"`.
  - `atx_db.jobs._lei_options`, `atx_db.jobs._figi_options`; `DATASET_REGISTRY["identifiers_lei"]`, `DATASET_REGISTRY["identifiers_figi"]`; `DATASET_DEPENDENCIES["identifiers_lei"] = ("sec_security_master",)`, `DATASET_DEPENDENCIES["identifiers_figi"] = ("identifier_resolution_decisions",)`.

**Why no downloader.** Both connectors already take a file path and parse it offline (`parse_gleif_file` rejects anything that is not `.csv`; `parse_openfigi_file` accepts a CSV or a saved `POST /v3/mapping` request/response pair). The GLEIF golden copy is a bulk operator download and the OpenFIGI mapping is a rate-limited batch job, so neither belongs behind the activation `Downloader` protocol. The stages take paths and **skip cleanly** — `StageResult(rows=0, detail={"skipped": "no file supplied"})` — when no file is staged, which keeps `atx-db activate` runnable end-to-end on a machine with no vendor files.

- [ ] **Step 1: Create the offline fixtures**

First confirm the exact Golden Copy headers `parse_gleif_file` reads:

Run: `.venv\Scripts\python.exe -c "import inspect, atx_db.identifiers_lei as m; print(inspect.getsource(m._parsed_level1_frame))"`
Expected: the `raw.get(...)` column names; match them exactly in the fixture below.

Create `C:\atx\atx-db\tests\data\gleif_level1_sample.csv`:

```csv
LEI,Entity.LegalName,Entity.RegistrationAuthority.RegistrationAuthorityID,Entity.RegistrationAuthority.RegistrationAuthorityEntityID,Entity.LegalAddress.Country
HWUPKR0MPOU8FGXBT394,APPLE INC.,RA000453,0000320193,US
549300SBEXQCOWQO0L54,MICROSOFT CORPORATION,RA000453,0000789019,US
5493006MHB84DD0ZWV18,SOME FOREIGN AG,RA000589,HRB12345,DE
```

Create `C:\atx\atx-db\tests\data\gleif_level2_sample.csv`:

```csv
Relationship.StartNode.NodeID,Relationship.EndNode.NodeID,Relationship.RelationshipType,Relationship.RelationshipPeriods.StartDate,Relationship.RelationshipPeriods.EndDate
549300SBEXQCOWQO0L54,HWUPKR0MPOU8FGXBT394,IS_DIRECTLY_CONSOLIDATED_BY,2019-01-01,
```

Create `C:\atx\atx-db\tests\data\openfigi_mapping_sample.json`:

```json
{
  "requests": [
    {"idType": "ID_CUSIP", "idValue": "037833100"},
    {"idType": "ID_CUSIP", "idValue": "594918104"},
    {"idType": "ID_CUSIP", "idValue": "000000000"}
  ],
  "responses": [
    {"data": [{"figi": "BBG000B9XRY4", "ticker": "AAPL", "name": "APPLE INC"}]},
    {"data": [{"figi": "BBG000BPH459", "ticker": "MSFT", "name": "MICROSOFT CORP"}]},
    {"error": "No identifier found."}
  ]
}
```

- [ ] **Step 2: Write the failing test**

Create `C:\atx\atx-db\tests\test_identifier_activation.py`:

```python
"""Tier1-S4 T5: GLEIF and OpenFIGI run as offline activation stages."""

from __future__ import annotations

import datetime as dt
from pathlib import Path

import pytest


FIXTURES = Path(__file__).resolve().parent / "data"
GLEIF_L1 = FIXTURES / "gleif_level1_sample.csv"
GLEIF_L2 = FIXTURES / "gleif_level2_sample.csv"
OPENFIGI = FIXTURES / "openfigi_mapping_sample.json"


def _options(**overrides):
    from atx_db.activation import ActivationOptions

    return ActivationOptions(as_of_date=dt.date(2024, 6, 28), run_id="t", **overrides)


def _seed_apple(store):
    store.con.execute(
        "INSERT INTO securities (security_id, entity_id, primary_symbol, name, source) VALUES "
        "('SEC-AAPL','CIK-0000320193','AAPL','Apple Inc.','test')"
    )


def test_the_three_file_options_exist_and_default_to_none():
    options = _options()
    assert options.gleif_level1_file is None
    assert options.gleif_level2_file is None
    assert options.openfigi_mapping_file is None


def test_stage_order_places_identifiers_after_companyfacts():
    from atx_db.activation import STAGE_ORDER

    order = list(STAGE_ORDER)
    assert order.index("companyfacts_load") < order.index("identifiers_lei")
    assert order.index("identifiers_lei") < order.index("identifiers_figi")
    assert order.index("identifiers_figi") < order.index("standardized")


def test_both_stages_are_registered():
    from atx_db.activation import STAGES

    assert "identifiers_lei" in STAGES
    assert "identifiers_figi" in STAGES


@pytest.mark.parametrize("stage", ["identifiers_lei", "identifiers_figi"])
def test_stages_skip_cleanly_with_no_file(tmp_store, stage):
    from atx_db.activation import STAGES

    result = STAGES[stage](tmp_store, _options())
    assert result.rows == 0
    assert result.detail["skipped"] == "no file supplied"


def test_lei_stage_attaches_an_lei_alias(tmp_store):
    from atx_db.activation import STAGES

    _seed_apple(tmp_store)
    result = STAGES["identifiers_lei"](
        tmp_store, _options(gleif_level1_file=GLEIF_L1, gleif_level2_file=GLEIF_L2)
    )
    assert result.rows >= 1
    row = tmp_store.con.execute(
        "SELECT id_value, valid_to FROM security_identifier_history "
        "WHERE security_id = 'SEC-AAPL' AND id_type = 'LEI'"
    ).fetchone()
    assert row[0] == "HWUPKR0MPOU8FGXBT394"
    assert row[1] is None


def test_the_foreign_registration_authority_is_not_crosswalked():
    from atx_db.identifiers_lei import SEC_REGISTRATION_AUTHORITY_ID, parse_gleif_file

    frame = parse_gleif_file(GLEIF_L1)
    assert SEC_REGISTRATION_AUTHORITY_ID == "RA000453"
    assert int(frame["cik"].notna().sum()) == 2


def test_figi_stage_attaches_a_figi_alias(tmp_store):
    from atx_db.activation import STAGES

    _seed_apple(tmp_store)
    tmp_store.con.execute(
        "INSERT INTO security_identifier_history (security_id, id_type, id_value, valid_from, "
        "as_of_date, available_at, source) VALUES "
        "('SEC-AAPL','CUSIP','037833100',DATE '2020-01-01',DATE '2020-01-01',"
        "TIMESTAMP '2020-01-01 22:00:00','test')"
    )
    result = STAGES["identifiers_figi"](tmp_store, _options(openfigi_mapping_file=OPENFIGI))
    assert result.rows >= 1
    figi = tmp_store.con.execute(
        "SELECT id_value FROM security_identifier_history "
        "WHERE security_id = 'SEC-AAPL' AND id_type = 'FIGI'"
    ).fetchone()[0]
    assert figi == "BBG000B9XRY4"


def test_the_unmatched_cusip_lands_in_the_resolution_ledger(tmp_store):
    from atx_db.activation import STAGES

    _seed_apple(tmp_store)
    STAGES["identifiers_figi"](tmp_store, _options(openfigi_mapping_file=OPENFIGI))
    candidates = tmp_store.con.execute(
        "SELECT count(*) FROM identifier_resolution_candidates"
    ).fetchone()[0]
    assert int(candidates) >= 1


def test_both_datasets_are_registered_in_the_dag():
    from atx_db.jobs import DATASET_DEPENDENCIES, DATASET_REGISTRY

    assert "identifiers_lei" in DATASET_REGISTRY
    assert "identifiers_figi" in DATASET_REGISTRY
    assert DATASET_DEPENDENCIES["identifiers_lei"] == ("sec_security_master",)
    assert DATASET_DEPENDENCIES["identifiers_figi"] == ("identifier_resolution_decisions",)


def test_no_fixture_parse_touches_the_network(monkeypatch):
    import socket

    def _boom(*_args, **_kwargs):
        raise AssertionError("network access attempted")

    monkeypatch.setattr(socket, "create_connection", _boom)
    from atx_db.identifiers_figi import parse_openfigi_file
    from atx_db.identifiers_lei import parse_gleif_file

    assert not parse_gleif_file(GLEIF_L1).empty
    assert not parse_openfigi_file(OPENFIGI).empty
```

- [ ] **Step 3: Run and watch it fail**

Run: `.venv\Scripts\python.exe -m pytest tests/test_identifier_activation.py -n 0 -q`
Expected: FAIL — `TypeError: ActivationOptions.__init__() got an unexpected keyword argument 'gleif_level1_file'` and `KeyError: 'identifiers_lei'`.

- [ ] **Step 4: Add the options and the two stages**

In `C:\atx\atx-db\src\atx_db\activation.py`, add three fields to `ActivationOptions`, immediately after `cache_dir`:

```python
    gleif_level1_file: Path | None = None
    gleif_level2_file: Path | None = None
    openfigi_mapping_file: Path | None = None
```

(`ledger_params()` already coerces `Path` values to `str`, so the ledger payload stays JSON-safe with no further change.)

Add the two stages beside the other stage functions:

```python
def stage_identifiers_lei(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Load a GLEIF Level-1 golden-copy CSV and attach LEI aliases to the entity spine.

    Offline by construction: ``parse_gleif_file`` accepts only a local ``.csv``. The
    stage is a clean no-op when the operator has not staged a golden copy, so a full
    activation still completes on a machine with no vendor files.
    """

    from .identifiers_lei import LeiAliasDataset, LeiLoadOptions

    if options.gleif_level1_file is None:
        return StageResult(rows=0, detail={"skipped": "no file supplied"})
    result = LeiAliasDataset().load(
        store,
        LeiLoadOptions(
            lei_file=Path(options.gleif_level1_file),
            lei_level2_file=None
            if options.gleif_level2_file is None
            else Path(options.gleif_level2_file),
            as_of_date=options.as_of_date,
            run_id=options.run_id,
        ),
    )
    return StageResult(
        rows=int(result.rows_loaded),
        detail={"file": str(options.gleif_level1_file), **dict(result.details or {})},
    )


def stage_identifiers_figi(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Load an OpenFIGI mapping export and attach FIGI/TICKER aliases.

    Offline by construction: ``parse_openfigi_file`` accepts a local CSV or a saved
    ``POST /v3/mapping`` request/response pair. Ambiguous and unmatched CUSIPs are routed
    to ``identifier_resolution_candidates`` by the loader, never merged blindly.
    """

    from .identifiers_figi import FigiAliasDataset, FigiLoadOptions

    if options.openfigi_mapping_file is None:
        return StageResult(rows=0, detail={"skipped": "no file supplied"})
    result = FigiAliasDataset().load(
        store,
        FigiLoadOptions(
            figi_file=Path(options.openfigi_mapping_file),
            as_of_date=options.as_of_date,
            run_id=options.run_id,
        ),
    )
    return StageResult(
        rows=int(result.rows_loaded),
        detail={"file": str(options.openfigi_mapping_file), **dict(result.details or {})},
    )


STAGES["identifiers_lei"] = stage_identifiers_lei
STAGES["identifiers_figi"] = stage_identifiers_figi
```

and insert `"identifiers_lei",` and `"identifiers_figi",` into `STAGE_ORDER` immediately after `"companyfacts_load"`.

- [ ] **Step 5: Register both datasets in the DAG**

In `C:\atx\atx-db\src\atx_db\jobs.py` add the imports and factories:

```python
from .identifiers_figi import FigiAliasDataset, FigiLoadOptions
from .identifiers_lei import LeiAliasDataset, LeiLoadOptions


def _lei_options(params: dict[str, Any]) -> LeiLoadOptions:
    lei_file = params.get("lei_file")
    if not lei_file:
        raise ValueError("identifiers_lei requires params['lei_file']")
    level2 = params.get("lei_level2_file")
    return LeiLoadOptions(
        lei_file=Path(lei_file),
        lei_level2_file=None if not level2 else Path(level2),
        as_of_date=_as_date(params.get("as_of_date")),
        run_id=params.get("run_id"),
    )


def _figi_options(params: dict[str, Any]) -> FigiLoadOptions:
    figi_file = params.get("figi_file")
    if not figi_file:
        raise ValueError("identifiers_figi requires params['figi_file']")
    return FigiLoadOptions(
        figi_file=Path(figi_file),
        as_of_date=_as_date(params.get("as_of_date")),
        run_id=params.get("run_id"),
    )
```

Add to `DATASET_REGISTRY`:

```python
    LeiAliasDataset.dataset_id: (LeiAliasDataset, _lei_options),
    FigiAliasDataset.dataset_id: (FigiAliasDataset, _figi_options),
```

and to `DATASET_DEPENDENCIES`:

```python
    "identifiers_lei": ("sec_security_master",),
    "identifiers_figi": ("identifier_resolution_decisions",),
```

- [ ] **Step 6: Add the three CLI flags**

In `scripts/warehouse_activate.py` and in `cli.py`'s `activate` subparser, add:

```python
    parser.add_argument("--gleif-level1-file", type=Path)
    parser.add_argument("--gleif-level2-file", type=Path)
    parser.add_argument("--openfigi-mapping-file", type=Path)
```

and thread them into the `ActivationOptions(...)` construction as
`gleif_level1_file=args.gleif_level1_file`, `gleif_level2_file=args.gleif_level2_file`,
`openfigi_mapping_file=args.openfigi_mapping_file`.

- [ ] **Step 7: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_identifier_activation.py -n 0 -q`
Expected: PASS — `11 passed`.

- [ ] **Step 8: Run the neighbouring suites**

Run: `.venv\Scripts\python.exe -m pytest tests/test_identifiers_lei.py tests/test_identifiers_figi.py tests/test_identifier_spine.py tests/test_cli_activate.py -n 0 -q --run-slow`
Expected: PASS — the connectors are unchanged; only their scheduling is new. `test_cli_activate.py` may pin the length of `STAGE_ORDER`; update that number in this commit if so.

- [ ] **Step 9: Commit**

```
git add src/atx_db/activation.py src/atx_db/jobs.py src/atx_db/cli.py scripts/warehouse_activate.py tests/test_identifier_activation.py tests/data/gleif_level1_sample.csv tests/data/gleif_level2_sample.csv tests/data/openfigi_mapping_sample.json tests/test_cli_activate.py
git commit -m "feat(db): schedule the GLEIF and OpenFIGI identifier loaders

Adds identifiers_lei and identifiers_figi activation stages plus DAG registrations for
the two existing offline connectors, three ActivationOptions file paths and their CLI
flags, and offline GLEIF Level-1/Level-2 and OpenFIGI mapping fixtures. Both stages
skip cleanly when no vendor file is staged, so a full activation still completes
without them. LEI and FIGI now populate security_identifier_history with PIT validity.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---
### Task 6: `quality/checks_identities.py` — accounting identities, cross-source shares, coverage gates

**Files:**
- Create: `src/atx_db/quality/checks_identities.py`
- Modify: `src/atx_db/quality/_checks.py`, `src/atx_db/quality/__init__.py`
- Modify: `tests/data/public_api_snapshot.json` (+`checks_identities` under `atx_db.quality`)
- Test: `tests/test_quality_identities.py` (new)

**Interfaces:**
- Consumes: `atx_db.quality._types.SqlQualityCheck`; lazily inside the factory (mirroring `_runner`'s `signal_eval` precedent) `atx_db.item_coverage.ITEM_COVERAGE_TARGET_ITEMS`, `ITEM_COVERAGE_TARGET_PCT`, `ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR` (Sprint 2 Task 9).
- Produces:
  - `IDENTITY_TOLERANCE_PCT: float = 0.005`, `IDENTITY_TOLERANCE_ABS: float = 1_000_000.0`
  - `SHARES_TOLERANCE: float = 0.05`, `SHARES_PASS_RATE: float = 0.95`
  - `AccountingIdentity` — frozen dataclass `(check_name: str, codes: tuple[str, ...], lhs: str, rhs: str, scale: str, severity: str, description: str)`
  - `ACCOUNTING_IDENTITIES: tuple[AccountingIdentity, ...]` — `balance_sheet_identity_violations`, `gross_profit_identity_violations`, `cash_flow_identity_violations`
  - `identity_violation_sql(identity: AccountingIdentity) -> str`
  - `SHARES_CROSS_SOURCE_CHECK_NAME: str = "shares_cross_source_disagreement"`
  - `DERIVED_FAMILY_COVERAGE_CHECK_NAME: str = "derived_metric_families_without_values"`
  - `ITEM_COVERAGE_CHECK_NAME: str = "fundamental_item_coverage_below_target"`
  - `identity_check_specs(**_ignored: object) -> tuple[SqlQualityCheck, ...]` — six specs
  - `atx_db.quality._checks._check_specs` concatenates `_identity_check_specs(**common_kwargs)`

**Identity definitions** (canonical codes verified against `seeds/fundamental_items.csv`; the spec's Compustat-style mnemonics are *not* the seed's codes):

| check | identity | item ids |
| --- | --- | --- |
| `balance_sheet_identity_violations` | `total_assets = total_liabilities + stockholders_equity + coalesce(minority_interest_bs, 0)` | 1101, 1201, 1221, 1213 |
| `gross_profit_identity_violations` | `gross_profit__1004 = revenue - cost_of_revenue_cogs` | 1004, 1001, 1003 |
| `cash_flow_identity_violations` | `cash_flow_from_operations + cash_flow_from_investing + cash_flow_from_financing + coalesce(fx_effect_on_cash, 0) = net_change_in_cash` | 1301, 1303, 1304, 1323, 1324 |

A filing is a violation when **every required code is present** and `abs(lhs - rhs) > greatest(abs(scale) * 0.005, 1e6)`. Filings missing an input are not violations — that is a coverage question, measured by the item-coverage check, not an identity question.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_quality_identities.py`:

```python
"""Tier1-S4 T6: accounting identities, cross-source shares, and coverage gates."""

from __future__ import annotations

import pytest


def _spec(name):
    from atx_db.quality.checks_identities import identity_check_specs

    return next(spec for spec in identity_check_specs() if spec.check_name == name)


def _observed(store, name):
    return float(store.con.execute(_spec(name).sql).fetchone()[0])


def _insert_standardized(store, rows):
    values = ",".join(
        "('{sid}-{code}-{pe}','test','sec','{sid}','{cik}',{item},'{code}','{basis}',"
        "DATE '{pe}',{value},TIMESTAMP '{pe} 22:00:00',DATE '{pe}',1,true,"
        "'acc-{sid}-{pe}')".format(**row)
        for row in rows
    )
    store.con.execute(
        "INSERT INTO fundamental_standardized (standardized_id, source, upstream_source, "
        "security_id, cik, item_id, canonical_code, basis, period_end, value, available_at, "
        "as_of_date, revision_sequence, is_latest_revision, source_accession) VALUES " + values
    )


def _balance_rows(sid, assets, liabilities, equity, minority=None):
    rows = [
        dict(sid=sid, cik="0000000001", item=1101, code="total_assets", basis="instant",
             pe="2024-03-31", value=assets),
        dict(sid=sid, cik="0000000001", item=1201, code="total_liabilities", basis="instant",
             pe="2024-03-31", value=liabilities),
        dict(sid=sid, cik="0000000001", item=1221, code="stockholders_equity", basis="instant",
             pe="2024-03-31", value=equity),
    ]
    if minority is not None:
        rows.append(
            dict(sid=sid, cik="0000000001", item=1213, code="minority_interest_bs",
                 basis="instant", pe="2024-03-31", value=minority)
        )
    return rows


def test_a_balanced_balance_sheet_is_not_a_violation(tmp_store):
    _insert_standardized(tmp_store, _balance_rows("SEC-1", 1_000_000_000, 600_000_000, 400_000_000))
    assert _observed(tmp_store, "balance_sheet_identity_violations") == 0.0


def test_minority_interest_is_part_of_the_identity(tmp_store):
    _insert_standardized(
        tmp_store,
        _balance_rows("SEC-1", 1_000_000_000, 600_000_000, 350_000_000, minority=50_000_000),
    )
    assert _observed(tmp_store, "balance_sheet_identity_violations") == 0.0


def test_a_break_beyond_tolerance_is_a_violation(tmp_store):
    _insert_standardized(tmp_store, _balance_rows("SEC-1", 1_000_000_000, 600_000_000, 300_000_000))
    assert _observed(tmp_store, "balance_sheet_identity_violations") == 1.0


def test_a_break_inside_the_relative_tolerance_is_not_a_violation(tmp_store):
    # 4e6 break on 1e9 assets; greatest(0.5% * 1e9, 1e6) = 5e6, so this passes.
    _insert_standardized(tmp_store, _balance_rows("SEC-1", 1_000_000_000, 600_000_000, 396_000_000))
    assert _observed(tmp_store, "balance_sheet_identity_violations") == 0.0


def test_a_small_filing_still_gets_the_one_million_dollar_floor(tmp_store):
    # 0.5% of 1e7 is 5e4, so the 1e6 absolute floor governs; a 9e5 break must pass.
    _insert_standardized(tmp_store, _balance_rows("SEC-1", 10_000_000, 6_000_000, 3_100_000))
    assert _observed(tmp_store, "balance_sheet_identity_violations") == 0.0


def test_a_filing_missing_an_input_is_never_a_violation(tmp_store):
    rows = _balance_rows("SEC-1", 1_000_000_000, 600_000_000, 400_000_000)
    _insert_standardized(tmp_store, rows[:2])
    assert _observed(tmp_store, "balance_sheet_identity_violations") == 0.0


def test_the_gross_profit_identity_catches_a_break(tmp_store):
    rows = [
        dict(sid="SEC-2", cik="0000000002", item=1001, code="revenue", basis="quarterly",
             pe="2024-03-31", value=500_000_000),
        dict(sid="SEC-2", cik="0000000002", item=1003, code="cost_of_revenue_cogs",
             basis="quarterly", pe="2024-03-31", value=300_000_000),
        dict(sid="SEC-2", cik="0000000002", item=1004, code="gross_profit__1004",
             basis="quarterly", pe="2024-03-31", value=150_000_000),
    ]
    _insert_standardized(tmp_store, rows)
    assert _observed(tmp_store, "gross_profit_identity_violations") == 1.0


def test_the_cash_flow_identity_ties_out(tmp_store):
    rows = [
        dict(sid="SEC-3", cik="0000000003", item=1301, code="cash_flow_from_operations",
             basis="quarterly", pe="2024-03-31", value=100_000_000),
        dict(sid="SEC-3", cik="0000000003", item=1303, code="cash_flow_from_investing",
             basis="quarterly", pe="2024-03-31", value=-40_000_000),
        dict(sid="SEC-3", cik="0000000003", item=1304, code="cash_flow_from_financing",
             basis="quarterly", pe="2024-03-31", value=-20_000_000),
        dict(sid="SEC-3", cik="0000000003", item=1323, code="fx_effect_on_cash",
             basis="quarterly", pe="2024-03-31", value=1_000_000),
        dict(sid="SEC-3", cik="0000000003", item=1324, code="net_change_in_cash",
             basis="quarterly", pe="2024-03-31", value=41_000_000),
    ]
    _insert_standardized(tmp_store, rows)
    assert _observed(tmp_store, "cash_flow_identity_violations") == 0.0
```

Continue appending to `C:\atx\atx-db\tests\test_quality_identities.py`:

```python
def _insert_market_daily(store, rows):
    values = ",".join(
        "('{sid}-{td}','atx-db daily market panel v1','{sid}','{sym}',DATE '{td}',{ratio},"
        "TIMESTAMP '{td} 22:00:00','h',DATE '{td}',true)".format(**row)
        for row in rows
    )
    store.con.execute(
        "INSERT INTO market_daily_metrics (market_daily_id, source, security_id, symbol, "
        "trade_date, shares_reconciliation_ratio, available_at, inputs_hash, as_of_date, "
        "is_latest_revision) VALUES " + values
    )


def test_shares_agreement_inside_five_percent_passes(tmp_store):
    _insert_market_daily(
        tmp_store,
        [dict(sid="SEC-1", sym="A", td="2024-01-02", ratio=1.02),
         dict(sid="SEC-1", sym="A", td="2024-01-03", ratio=0.99)],
    )
    assert _observed(tmp_store, "shares_cross_source_disagreement") == 0.0


def test_a_security_outside_five_percent_fails_the_share_check(tmp_store):
    _insert_market_daily(
        tmp_store,
        [dict(sid="SEC-1", sym="A", td="2024-01-02", ratio=1.02),
         dict(sid="SEC-2", sym="B", td="2024-01-02", ratio=1.40),
         dict(sid="SEC-2", sym="B", td="2024-01-03", ratio=1.38)],
    )
    assert _observed(tmp_store, "shares_cross_source_disagreement") == pytest.approx(0.5)


def test_securities_with_only_one_share_source_are_out_of_scope(tmp_store):
    _insert_market_daily(tmp_store, [dict(sid="SEC-1", sym="A", td="2024-01-02", ratio="NULL")])
    assert _observed(tmp_store, "shares_cross_source_disagreement") == 0.0


def test_a_derived_family_with_no_values_is_reported(tmp_store):
    tmp_store.con.execute(
        "INSERT INTO derived_metric_definitions (metric_code, family, expression, metric_window, "
        "inputs_json, requires_market, description, version, topological_rank) VALUES "
        "('gross_margin','profitability','item:gross_profit__1004 / item:revenue','q','[]',"
        "false,'d','1',1),"
        "('payout_ratio','payout','item:dividends_paid / item:net_income_total','q','[]',"
        "false,'d','1',2)"
    )
    tmp_store.con.execute(
        "INSERT INTO derived_metric_values (derived_value_id, source, security_id, metric_code, "
        "metric_window, period_end, value, available_at, inputs_hash, as_of_date) VALUES "
        "('v1','atx-db declarative derived metrics v1','SEC-1','gross_margin','q',"
        "DATE '2024-03-31',0.4,TIMESTAMP '2024-05-01 22:00:00','h',DATE '2024-03-31')"
    )
    assert _observed(tmp_store, "derived_metric_families_without_values") == 1.0


def test_item_coverage_shortfall_is_measured(tmp_store):
    from atx_db.item_coverage import ITEM_COVERAGE_TARGET_ITEMS

    tmp_store.con.execute(
        "INSERT INTO fundamental_item_coverage (coverage_id, source, universe_id, item_id, "
        "canonical_code, basis, fiscal_year, n_securities, n_with_value, coverage_pct) VALUES "
        "('c1','t','us_listed_v1',1101,'total_assets','instant',2020,100,99,99.0)"
    )
    assert _observed(tmp_store, "fundamental_item_coverage_below_target") == float(
        ITEM_COVERAGE_TARGET_ITEMS - 1
    )


def test_every_identity_spec_declares_its_required_tables():
    from atx_db.quality.checks_identities import identity_check_specs

    specs = identity_check_specs()
    assert len(specs) == 6
    for spec in specs:
        assert spec.required_tables
        assert spec.warn_if_missing is True
        assert spec.severity in {"warning", "error", "critical"}


def test_the_identity_specs_are_part_of_the_production_sweep():
    from atx_db.quality._checks import _check_specs

    names = {
        spec.check_name
        for spec in _check_specs(
            daily_macro_stale_days=10, monthly_macro_stale_days=70, valuation_stale_gap_days=30
        )
    }
    assert "balance_sheet_identity_violations" in names
    assert "shares_cross_source_disagreement" in names
    assert "derived_metric_families_without_values" in names
    assert "fundamental_item_coverage_below_target" in names
```

- [ ] **Step 2: Run and watch it fail**

Run: `.venv\Scripts\python.exe -m pytest tests/test_quality_identities.py -n 0 -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.quality.checks_identities'`.

- [ ] **Step 3: Write the check module**

Create `C:\atx\atx-db\src\atx_db\quality\checks_identities.py`:

```python
"""Tier1-S4: measured quality gates over the standardized and daily-market layers.

Six specs:

* three per-filing accounting identities (balance sheet, gross profit, cash flow) with
  the spec's 0.5%-or-$1M tolerance;
* the cross-source shares test (archive vs dei within 5% for at least 95% of securities);
* derived-metric family coverage (every family in ``derived_metric_definitions`` must emit
  at least one value);
* item coverage against Sprint 2's published target.

This module is a LEAF of ``atx_db.quality``: it imports only ``._types`` at module level, so
it cannot introduce an import cycle inside the package (enforced by
``test_decomposed_package_import_graphs_are_acyclic``). The Sprint 2 coverage constants are
imported lazily inside the factory, mirroring ``_runner``'s ``signal_eval`` precedent.
"""

from __future__ import annotations

from dataclasses import dataclass

from ._types import SqlQualityCheck


IDENTITY_TOLERANCE_PCT = 0.005
IDENTITY_TOLERANCE_ABS = 1_000_000.0
SHARES_TOLERANCE = 0.05
SHARES_PASS_RATE = 0.95

SHARES_CROSS_SOURCE_CHECK_NAME = "shares_cross_source_disagreement"
DERIVED_FAMILY_COVERAGE_CHECK_NAME = "derived_metric_families_without_values"
ITEM_COVERAGE_CHECK_NAME = "fundamental_item_coverage_below_target"


@dataclass(frozen=True)
class AccountingIdentity:
    """One per-filing identity, expressed over pivoted canonical codes.

    ``lhs``/``rhs``/``scale`` are SQL fragments over the pivoted column names, which are
    exactly the canonical codes in ``codes``. ``codes`` also drives the NOT NULL guard: a
    filing missing any required code is skipped, never counted as a violation. Optional
    codes are the ones absent from ``codes`` but referenced through ``coalesce`` in the
    fragments.
    """

    check_name: str
    codes: tuple[str, ...]
    lhs: str
    rhs: str
    scale: str
    severity: str
    description: str


ACCOUNTING_IDENTITIES: tuple[AccountingIdentity, ...] = (
    AccountingIdentity(
        check_name="balance_sheet_identity_violations",
        codes=("total_assets", "total_liabilities", "stockholders_equity"),
        lhs="total_assets",
        rhs="total_liabilities + stockholders_equity + coalesce(minority_interest_bs, 0)",
        scale="total_assets",
        severity="error",
        description="assets = liabilities + equity (+ minority interest)",
    ),
    AccountingIdentity(
        check_name="gross_profit_identity_violations",
        codes=("gross_profit__1004", "revenue", "cost_of_revenue_cogs"),
        lhs="gross_profit__1004",
        rhs="revenue - cost_of_revenue_cogs",
        scale="revenue",
        severity="error",
        description="gross profit = revenue - cost of revenue",
    ),
    AccountingIdentity(
        check_name="cash_flow_identity_violations",
        codes=(
            "cash_flow_from_operations",
            "cash_flow_from_investing",
            "cash_flow_from_financing",
            "net_change_in_cash",
        ),
        lhs=(
            "cash_flow_from_operations + cash_flow_from_investing + cash_flow_from_financing "
            "+ coalesce(fx_effect_on_cash, 0)"
        ),
        rhs="net_change_in_cash",
        scale="net_change_in_cash",
        severity="error",
        description="cfo + cfi + cff + fx = change in cash",
    ),
)

# Every code any identity can reference, pivoted once.
_PIVOT_CODES: tuple[str, ...] = (
    "cash_flow_from_financing",
    "cash_flow_from_investing",
    "cash_flow_from_operations",
    "cost_of_revenue_cogs",
    "fx_effect_on_cash",
    "gross_profit__1004",
    "minority_interest_bs",
    "net_change_in_cash",
    "revenue",
    "stockholders_equity",
    "total_assets",
    "total_liabilities",
)


def identity_violation_sql(identity: AccountingIdentity) -> str:
    """Count filings whose ``identity`` breaks by more than 0.5% of scale or $1M.

    The pivot keys on ``(security_id, period_end, basis, source_accession)`` -- one filing's
    view of one period -- and keeps the newest revision per code so a restatement is scored
    once, on its restated values.
    """

    code_list = ", ".join(f"'{code}'" for code in _PIVOT_CODES)
    pivot = ",\n            ".join(
        f"max(CASE WHEN canonical_code = '{code}' THEN value END) AS {code}"
        for code in _PIVOT_CODES
    )
    present = " AND ".join(f"{code} IS NOT NULL" for code in identity.codes)
    return f"""
        WITH latest AS (
            SELECT
                security_id,
                period_end,
                basis,
                source_accession,
                canonical_code,
                arg_max(value, (revision_sequence, available_at)) AS value
            FROM fundamental_standardized
            WHERE is_latest_revision
              AND value IS NOT NULL
              AND canonical_code IN ({code_list})
            GROUP BY security_id, period_end, basis, source_accession, canonical_code
        ),
        wide AS (
            SELECT
                security_id,
                period_end,
                basis,
                source_accession,
                {pivot}
            FROM latest
            GROUP BY security_id, period_end, basis, source_accession
        )
        SELECT count(*)::DOUBLE
        FROM wide
        WHERE {present}
          AND abs(({identity.lhs}) - ({identity.rhs}))
              > greatest(abs({identity.scale}) * {IDENTITY_TOLERANCE_PCT},
                         {IDENTITY_TOLERANCE_ABS})
    """
```

Continue appending to `C:\atx\atx-db\src\atx_db\quality\checks_identities.py`:

```python
# Fraction of two-source securities whose median |dei/archive - 1| exceeds the tolerance.
# The spec asks for "within 5% for at least 95% of securities", so the observed value is
# the FAILING fraction and the threshold is 1 - 0.95 = 0.05.
_SHARES_SQL = f"""
WITH per_security AS (
    SELECT
        security_id,
        median(abs(shares_reconciliation_ratio - 1.0)) AS median_abs_gap
    FROM market_daily_metrics
    WHERE is_latest_revision
      AND shares_reconciliation_ratio IS NOT NULL
      AND shares_reconciliation_ratio > 0
    GROUP BY security_id
)
SELECT
    CASE
        WHEN count(*) = 0 THEN 0.0
        ELSE (count(*) FILTER (WHERE median_abs_gap > {SHARES_TOLERANCE}))::DOUBLE / count(*)
    END
FROM per_security
"""

_DERIVED_FAMILY_SQL = """
SELECT count(*)::DOUBLE
FROM (
    SELECT d.family
    FROM derived_metric_definitions d
    LEFT JOIN (
        SELECT DISTINCT metric_code
        FROM derived_metric_values
        WHERE is_latest_revision
    ) v ON v.metric_code = d.metric_code
    GROUP BY d.family
    HAVING count(v.metric_code) = 0
)
"""


def _item_coverage_sql(*, target_items: int, target_pct: float, minimum_fiscal_year: int) -> str:
    """How many items short of the published target the warehouse is.

    An item counts only when its coverage clears ``target_pct`` in EVERY in-scope fiscal
    year -- the same all-years rule ``item_coverage.evaluate_item_coverage_gate`` applies,
    so the gate and the published ITEM_COVERAGE.md can never disagree.
    """

    return f"""
        SELECT greatest(0, {target_items} - count(*))::DOUBLE
        FROM (
            SELECT item_id
            FROM fundamental_item_coverage
            WHERE fiscal_year >= {minimum_fiscal_year}
            GROUP BY item_id
            HAVING min(coverage_pct) >= {target_pct}
        )
    """


def identity_check_specs(**_ignored: object) -> tuple[SqlQualityCheck, ...]:
    """The six Tier-1 measured gates.

    Accepts and ignores the ``daily_macro_stale_days`` / ``monthly_macro_stale_days`` /
    ``valuation_stale_gap_days`` common kwargs so the factory is interchangeable with the
    other ``*_check_specs`` factories.
    """

    from ..item_coverage import (
        ITEM_COVERAGE_TARGET_ITEMS,
        ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR,
        ITEM_COVERAGE_TARGET_PCT,
    )

    specs: list[SqlQualityCheck] = [
        SqlQualityCheck(
            dataset_id="fundamental_standardized",
            table_name="fundamental_standardized",
            check_name=identity.check_name,
            sql=identity_violation_sql(identity),
            threshold=0.0,
            comparator="le",
            required_tables=("fundamental_standardized",),
            warn_if_missing=True,
            failure_status="failed",
            severity=identity.severity,
        )
        for identity in ACCOUNTING_IDENTITIES
    ]
    specs.append(
        SqlQualityCheck(
            dataset_id="market_daily",
            table_name="market_daily_metrics",
            check_name=SHARES_CROSS_SOURCE_CHECK_NAME,
            sql=_SHARES_SQL,
            threshold=round(1.0 - SHARES_PASS_RATE, 10),
            comparator="le",
            required_tables=("market_daily_metrics",),
            warn_if_missing=True,
            failure_status="failed",
            severity="error",
        )
    )
    specs.append(
        SqlQualityCheck(
            dataset_id="derived_metrics",
            table_name="derived_metric_values",
            check_name=DERIVED_FAMILY_COVERAGE_CHECK_NAME,
            sql=_DERIVED_FAMILY_SQL,
            threshold=0.0,
            comparator="le",
            required_tables=("derived_metric_definitions", "derived_metric_values"),
            warn_if_missing=True,
            failure_status="warning",
            severity="warning",
        )
    )
    specs.append(
        SqlQualityCheck(
            dataset_id="fundamental_item_coverage",
            table_name="fundamental_item_coverage",
            check_name=ITEM_COVERAGE_CHECK_NAME,
            sql=_item_coverage_sql(
                target_items=ITEM_COVERAGE_TARGET_ITEMS,
                target_pct=ITEM_COVERAGE_TARGET_PCT,
                minimum_fiscal_year=ITEM_COVERAGE_TARGET_MINIMUM_FISCAL_YEAR,
            ),
            threshold=0.0,
            comparator="le",
            required_tables=("fundamental_item_coverage",),
            warn_if_missing=True,
            failure_status="warning",
            severity="warning",
        )
    )
    return tuple(specs)
```

- [ ] **Step 4: Wire the factory into the production sweep**

In `C:\atx\atx-db\src\atx_db\quality\_checks.py`, add the aliased import beside the other five:

```python
from .checks_identities import identity_check_specs as _identity_check_specs
```

and add it to the concatenation inside `_check_specs`:

```python
    single_table_checks = (
        _market_reference_check_specs(**common_kwargs)
        + _fundamental_check_specs(**common_kwargs)
        + _identity_check_specs(**common_kwargs)
        + _ownership_check_specs(**common_kwargs)
        + _feature_catalog_check_specs(**common_kwargs)
        + _estimate_check_specs(**common_kwargs)
        + _analytic_check_specs(**common_kwargs)
    )
```

In `C:\atx\atx-db\src\atx_db\quality\__init__.py`, add `"_identity_check_specs",` and `"checks_identities",` to the tuple of names passed to `globals().pop(...)`, keeping the list alphabetical.

- [ ] **Step 5: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_quality_identities.py -n 0 -q`
Expected: PASS — `15 passed`.

- [ ] **Step 6: Run the quality suites and the boundary guard**

Run: `.venv\Scripts\python.exe -m pytest tests/test_quality_identities.py tests/test_quality_smoke.py tests/test_quality_gating.py tests/test_module_boundaries.py -n 0 -q`
Expected: PASS. If `test_quality_smoke.py` pins the total number of production specs, raise it by 6 in this commit.

- [ ] **Step 7: Refresh the public API snapshot**

Run: `.venv\Scripts\python.exe -m pytest tests/test_module_boundaries.py -n 0 -q`
Expected: PASS. If it fails on the `atx_db.quality` list, add `"checks_identities"` to that list in `tests/data/public_api_snapshot.json` and re-run.

- [ ] **Step 8: Commit**

```
git add src/atx_db/quality/checks_identities.py src/atx_db/quality/_checks.py src/atx_db/quality/__init__.py tests/test_quality_identities.py tests/data/public_api_snapshot.json
git commit -m "feat(db): measured accounting-identity, shares and coverage quality gates

Adds quality/checks_identities.py with per-filing balance-sheet, gross-profit and
cash-flow identity checks at the spec's 0.5%-or-\$1M tolerance, the archive-vs-dei shares
cross-source test (5% tolerance, 95% of securities), derived-metric family coverage, and
an item-coverage shortfall gate that reuses Sprint 2's published target. All six run in
the standard production sweep.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---
### Task 7: Migration 0307 — three new public schemas, SLO rows for every schema, measured condition flips

**Files:**
- Create: `src/atx_db/migrations/bodies_0307.py`
- Modify: `src/atx_db/api/catalog.py`, `src/atx_db/provider_coverage.py`, `src/atx_db/migrations/registry.py`
- Test: `tests/test_provider_coverage_slos.py` (new)

**Interfaces:**
- Consumes: `atx_db.api.catalog.FieldSpec`, `RecordSchema`, `DatasetSpec`, `_PIT_FIELDS`, `DATASETS`; `atx_db.provider_coverage.ProviderCoverageSlo`, `DEFAULT_PROVIDER_COVERAGE_SLOS`, `_active_slo`, `_schema_stats`, `refresh_provider_coverage`; `atx_db.item_coverage.evaluate_item_coverage_gate` (Sprint 2); the `universe_us_listed_membership` (Task 1) and `delisting_events` tables.
- Produces:
  - View `v_security_master_public(security_id, entity_id, issuer_id, primary_symbol, name, asset_class, country, currency, active, cik, lei, figi, as_of_date, available_at, source, run_id, source_loaded_at)` — **no `cusip` column**, so the public contract cannot leak the internal identifier.
  - Column `api_schema_coverage_slo.item_count_basis VARCHAR NOT NULL DEFAULT 'distinct'`.
  - `atx_db.provider_coverage.ProviderCoverageSlo.item_count_basis: str = "distinct"` (last field, defaulted, so every existing construction keeps working).
  - `atx_db.provider_coverage.ITEM_COUNT_BASES: tuple[str, ...]` = `("coverage_gate", "distinct")`
  - `atx_db.provider_coverage.coverage_gate_item_count(store) -> int | None`
  - Five new `DEFAULT_PROVIDER_COVERAGE_SLOS` rows — `ATX.US.FUNDAMENTALS/derived-metrics`, `ATX.US.FUNDAMENTALS/security-master`, `ATX.US.EQUITIES/market-daily-1d`, `ATX.US.EQUITIES/universe`, `ATX.US.EQUITIES/delistings` — and `ATX.US.FUNDAMENTALS/standardized` switches to `item_count_basis="coverage_gate"` with `minimum_item_count=110`.
  - `atx_db.api.catalog.SECURITY_MASTER_SCHEMA` (code `security-master`, `ATX.US.FUNDAMENTALS`), `UNIVERSE_SCHEMA` (code `universe`, `ATX.US.EQUITIES`), `DELISTINGS_SCHEMA` (code `delistings`, `ATX.US.EQUITIES`).
  - `atx_db.migrations.bodies_0307.MIGRATIONS` — `Migration(version=307, name="public_schema_coverage_slos", up=_public_schema_coverage_slos)`.

**The bug this task closes.** `refresh_provider_coverage` walks every schema of every dataset and calls `_active_slo`, which raises when `api_schema_coverage_slo` has no active row. Sprint 3 attached two schemas without adding SLO rows, so the `provider_coverage` activation stage raises as soon as Sprint 3 lands. The migration re-seeds from the widened Python tuple with `INSERT OR REPLACE`, so it repairs the warehouse whether or not Sprint 3 ships a fix of its own, and `test_every_public_schema_has_an_active_slo` makes the omission impossible to repeat.

**The measured condition flip.** Today the `standardized` schema's `item_count` is `count(DISTINCT canonical_code)`: a code present for one security counts the same as one covering 95% of the universe. With `item_count_basis='coverage_gate'` the measured `item_count` becomes `evaluate_item_coverage_gate(...)["items_meeting_threshold"]` — the number of items clearing 90% coverage in *every* in-scope fiscal year. The schema then flips `degraded`→`available` only when coverage is real, and `docs/ITEM_COVERAGE.md`, the `fundamental_item_coverage_below_target` check (Task 6) and the published condition all read one number.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_provider_coverage_slos.py`:

```python
"""Tier1-S4 T7: every public schema is measured, and the measure is meaningful."""

from __future__ import annotations

import datetime as dt

import pytest


def test_every_public_schema_has_an_active_slo(tmp_store):
    from atx_db.api.catalog import DATASETS

    missing = []
    for dataset in DATASETS:
        for schema in dataset.schemas:
            count = tmp_store.con.execute(
                "SELECT count(*) FROM api_schema_coverage_slo "
                "WHERE dataset_id = ? AND schema_code = ? AND is_active",
                [dataset.code, schema.code],
            ).fetchone()[0]
            if int(count) == 0:
                missing.append(f"{dataset.code}/{schema.code}")
    assert missing == []


def test_refresh_provider_coverage_covers_every_schema(tmp_store):
    from atx_db.api.catalog import DATASETS
    from atx_db.provider_coverage import ProviderCoverageOptions, refresh_provider_coverage

    snapshots = refresh_provider_coverage(
        tmp_store,
        ProviderCoverageOptions(observed_at=dt.datetime(2024, 6, 28, 12, 0, 0), run_id="t"),
    )
    assert len(snapshots) == sum(len(dataset.schemas) for dataset in DATASETS)


@pytest.mark.parametrize(
    "dataset_code,schema_code",
    [
        ("ATX.US.FUNDAMENTALS", "security-master"),
        ("ATX.US.EQUITIES", "universe"),
        ("ATX.US.EQUITIES", "delistings"),
    ],
)
def test_the_three_new_schemas_are_registered(dataset_code, schema_code):
    from atx_db.api.catalog import get_schema

    schema = get_schema(dataset_code, schema_code)
    assert schema.code == schema_code
    assert "security_id" in schema.field_names


def test_the_security_master_schema_never_exposes_cusip(tmp_store):
    from atx_db.api.catalog import get_schema

    schema = get_schema("ATX.US.FUNDAMENTALS", "security-master")
    assert "cusip" not in schema.field_names
    columns = {
        str(row[0])
        for row in tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = ?",
            [schema.source_table],
        ).fetchall()
    }
    assert "cusip" not in columns
    assert {"security_id", "as_of_date", "available_at", "run_id", "source_loaded_at"} <= columns


def test_every_schema_relation_carries_the_columns_the_service_needs(tmp_store):
    from atx_db.api.catalog import DATASETS

    required = {"security_id", "available_at", "source_loaded_at", "run_id", "as_of_date"}
    for dataset in DATASETS:
        for schema in dataset.schemas:
            columns = {
                str(row[0])
                for row in tmp_store.con.execute(
                    "SELECT column_name FROM duckdb_columns() WHERE table_name = ?",
                    [schema.source_table],
                ).fetchall()
            }
            assert required <= columns, f"{dataset.code}/{schema.code} missing {required - columns}"
            assert schema.time_column in columns


def test_item_count_basis_is_declared_per_schema():
    from atx_db.provider_coverage import DEFAULT_PROVIDER_COVERAGE_SLOS

    by_key = {(slo.dataset_id, slo.schema_code): slo for slo in DEFAULT_PROVIDER_COVERAGE_SLOS}
    assert by_key[("ATX.US.FUNDAMENTALS", "reported")].item_count_basis == "distinct"
    assert by_key[("ATX.US.FUNDAMENTALS", "standardized")].item_count_basis == "coverage_gate"


def test_the_coverage_gate_drives_the_standardized_item_count(tmp_store):
    from atx_db.provider_coverage import coverage_gate_item_count

    assert coverage_gate_item_count(tmp_store) is None
    tmp_store.con.execute(
        "INSERT INTO fundamental_item_coverage (coverage_id, source, universe_id, item_id, "
        "canonical_code, basis, fiscal_year, n_securities, n_with_value, coverage_pct) VALUES "
        "('c1','t','us_listed_v1',1101,'total_assets','instant',2020,100,99,99.0),"
        "('c2','t','us_listed_v1',1201,'total_liabilities','instant',2020,100,50,50.0)"
    )
    assert coverage_gate_item_count(tmp_store) == 1


def test_a_thin_standardized_layer_reports_degraded(tmp_store):
    from atx_db.provider_coverage import ProviderCoverageOptions, refresh_provider_coverage

    tmp_store.con.execute(
        "INSERT INTO fundamental_standardized (standardized_id, source, upstream_source, "
        "security_id, cik, item_id, canonical_code, basis, period_end, value, available_at, "
        "as_of_date, revision_sequence, is_latest_revision) VALUES "
        "('s1','t','sec','SEC-1','0000000001',1101,'total_assets','instant',DATE '2024-03-31',"
        "1.0,TIMESTAMP '2024-05-01 22:00:00',DATE '2024-03-31',1,true)"
    )
    snapshots = {
        (s.dataset_id, s.schema_code): s
        for s in refresh_provider_coverage(
            tmp_store,
            ProviderCoverageOptions(observed_at=dt.datetime(2024, 6, 28, 12, 0, 0), run_id="t"),
        )
    }
    standardized = snapshots[("ATX.US.FUNDAMENTALS", "standardized")]
    assert standardized.condition == "degraded"
    assert any(f["metric"] == "item_count" for f in standardized.failed_slos)
    assert standardized.item_count == 0  # no coverage rows, so the gate reports zero items
```

- [ ] **Step 2: Run and watch it fail**

Run: `.venv\Scripts\python.exe -m pytest tests/test_provider_coverage_slos.py -n 0 -q`
Expected: FAIL — `test_every_public_schema_has_an_active_slo` lists the five unseeded schemas, `get_schema` raises `KeyError: 'security-master'`, and `coverage_gate_item_count` does not exist.

- [ ] **Step 3: Add the three record schemas**

Append to `C:\atx\atx-db\src\atx_db\api\catalog.py`, immediately before the `DATASETS` tuple:

```python
SECURITY_MASTER_SCHEMA = RecordSchema(
    dataset="ATX.US.FUNDAMENTALS",
    code="security-master",
    version="1.0.0",
    title="US equity security master",
    description=(
        "Current-state security spine with its open CIK, LEI and FIGI identifiers. "
        "CUSIP is internal-only by policy and is never part of this contract."
    ),
    source_table="v_security_master_public",
    time_column="as_of_date",
    natural_key=("security_id",),
    supports_vintages=False,
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("entity_id", "entity_id", "string", "Issuing entity key; 'CIK-<cik>' for SEC filers."),
        FieldSpec("issuer_id", "issuer_id", "string", "Issuer grouping key."),
        FieldSpec("primary_symbol", "primary_symbol", "string", "Current primary ticker."),
        FieldSpec("name", "name", "string", "Security name."),
        FieldSpec("asset_class", "asset_class", "string", "ATX asset class.", nullable=False),
        FieldSpec("country", "country", "string", "Country of listing.", nullable=False),
        FieldSpec("currency", "currency", "string", "Trading currency.", nullable=False),
        FieldSpec("active", "active", "boolean", "Whether the listing is currently active.", nullable=False),
        FieldSpec("cik", "cik", "string", "SEC Central Index Key.", filterable=True),
        FieldSpec("lei", "lei", "string", "Legal Entity Identifier (GLEIF).", filterable=True),
        FieldSpec("figi", "figi", "string", "Financial Instrument Global Identifier (OpenFIGI).", filterable=True),
        *_PIT_FIELDS,
    ),
)


UNIVERSE_SCHEMA = RecordSchema(
    dataset="ATX.US.EQUITIES",
    code="universe",
    version="1.0.0",
    title="US-listed equity universe membership",
    description=(
        "Interval-keyed point-in-time universe of US-listed common stock, ADRs, REITs and "
        "LPs. Members with no resolved CIK are present with has_cik=false."
    ),
    source_table="universe_us_listed_membership",
    time_column="valid_from",
    natural_key=("universe_id", "security_id", "valid_from"),
    fields=(
        FieldSpec("universe_id", "universe_id", "string", "Universe identifier.", nullable=False, filterable=True),
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("symbol", "symbol", "string", "Ticker at the start of the interval."),
        FieldSpec("valid_from", "valid_from", "date", "First session of the membership interval.", nullable=False),
        FieldSpec("valid_to", "valid_to", "date", "Last session of the interval; null while open."),
        FieldSpec("security_type", "security_type", "string", "common, ADR, REIT or LP.", nullable=False, filterable=True),
        FieldSpec("exchange_code", "exchange_code", "string", "XNYS, XNAS, XASE, ARCX or BATS.", nullable=False, filterable=True),
        FieldSpec("has_cik", "has_cik", "boolean", "Whether the security resolves to an SEC filer.", nullable=False, filterable=True),
        FieldSpec("cik", "cik", "string", "SEC Central Index Key when resolved."),
        FieldSpec("market_cap_decile", "market_cap_decile", "int32", "Market-cap decile AT valid_from only."),
        FieldSpec("reason", "reason", "string", "member or member_no_cik.", nullable=False),
        FieldSpec("decision_count", "decision_count", "int32", "Sessions backing the interval.", nullable=False),
        *_PIT_FIELDS,
    ),
)


DELISTINGS_SCHEMA = RecordSchema(
    dataset="ATX.US.EQUITIES",
    code="delistings",
    version="1.0.0",
    title="US equity delisting events",
    description=(
        "One delisting event per security and delist date, attributed to the "
        "highest-precedence public evidence and carrying its terminal-return state."
    ),
    source_table="delisting_events",
    time_column="delist_date",
    natural_key=("source", "security_id", "delist_date"),
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier."),
        FieldSpec("symbol", "symbol", "string", "Ticker at delisting.", nullable=False),
        FieldSpec("delist_date", "delist_date", "date", "Date trading ceased.", nullable=False),
        FieldSpec("delist_code", "delist_code", "string", "Warehouse delist code.", nullable=False, filterable=True),
        FieldSpec("delist_reason", "delist_reason", "string", "Attributed reason category.", nullable=False, filterable=True),
        FieldSpec("delisting_return", "delisting_return", "float64", "Terminal return when known.", "ratio"),
        FieldSpec("delisting_return_type", "delisting_return_type", "string", "OBSERVED, POLICY or UNOBSERVED.", nullable=False),
        FieldSpec("is_return_imputed", "is_return_imputed", "boolean", "Whether the terminal return is a policy convention.", nullable=False),
        FieldSpec("return_policy", "return_policy", "string", "Policy code that produced the return.", nullable=False),
        FieldSpec("evidence_source", "evidence_source", "string", "Evidence family.", nullable=False),
        FieldSpec("evidence_confidence", "evidence_confidence", "string", "high, medium or low.", nullable=False),
        FieldSpec("inferred_from_absence", "inferred_from_absence", "boolean", "Whether the event was inferred rather than filed.", nullable=False),
        FieldSpec("source", "source", "string", "ATX source adapter.", nullable=False, filterable=True),
        *_PIT_FIELDS,
    ),
)
```

Attach them inside `DATASETS`: add `SECURITY_MASTER_SCHEMA,` to the `ATX.US.FUNDAMENTALS` `schemas` tuple (after `RESTATEMENTS_SCHEMA` and Sprint 3's `DERIVED_METRICS_SCHEMA`), and change the `ATX.US.EQUITIES` `schemas` tuple to `(DAILY_BARS_SCHEMA, MARKET_DAILY_SCHEMA, UNIVERSE_SCHEMA, DELISTINGS_SCHEMA)`.

- [ ] **Step 4: Extend `provider_coverage.py`**

In `C:\atx\atx-db\src\atx_db\provider_coverage.py`:

1. Add the new field to `ProviderCoverageSlo`, **last**, so every positional construction keeps working, and the module constant beside it:
   ```python
       item_count_basis: str = "distinct"
   ```
   ```python
   ITEM_COUNT_BASES: tuple[str, ...] = ("coverage_gate", "distinct")
   ```
2. Add the measured helper:
   ```python
   def coverage_gate_item_count(store: DuckDBStore) -> int | None:
       """Items clearing Sprint 2's published coverage target in every in-scope fiscal year.

       Returns None when ``fundamental_item_coverage`` is absent so a warehouse that has
       not measured coverage yet falls back to the distinct-code count instead of
       reporting a spurious zero.
       """

       from .item_coverage import evaluate_item_coverage_gate

       if not _relation_exists(store, "fundamental_item_coverage"):
           return None
       frame = store.con.execute(
           "SELECT item_id, fiscal_year, coverage_pct FROM fundamental_item_coverage "
           "ORDER BY item_id, fiscal_year"
       ).df()
       return int(evaluate_item_coverage_gate(frame)["items_meeting_threshold"])
   ```
3. In `_active_slo`, add `item_count_basis` to the SELECT list and pass `item_count_basis=str(row[10])` to the `ProviderCoverageSlo(...)` construction.
4. In `refresh_provider_coverage`, immediately after the branch that sets `item_count`, add:
   ```python
               if slo.item_count_basis == "coverage_gate":
                   measured = coverage_gate_item_count(store)
                   if measured is not None:
                       item_count = measured
   ```
   so the gate replaces the distinct-code count only for schemas that ask for it.
5. Append five rows to `DEFAULT_PROVIDER_COVERAGE_SLOS`, and edit the existing `standardized` row's `minimum_item_count` to `110` with the keyword argument `item_count_basis="coverage_gate"`:

```python
    ProviderCoverageSlo(
        "ATX.US.FUNDAMENTALS",
        "derived-metrics",
        dt.date(2009, 1, 1),
        15.0,
        2_500,
        120,
        120.0,
        _FUNDAMENTAL_CITATION,
        "Institutional target for declaratively computed PIT derived metrics.",
    ),
    ProviderCoverageSlo(
        "ATX.US.FUNDAMENTALS",
        "security-master",
        dt.date(2009, 1, 1),
        15.0,
        5_000,
        None,
        120.0,
        _FUNDAMENTAL_CITATION,
        "Institutional target for the US equity security master spine.",
    ),
    ProviderCoverageSlo(
        "ATX.US.EQUITIES",
        "market-daily-1d",
        dt.date(2010, 1, 1),
        10.0,
        5_000,
        30,
        7.0,
        DATABENTO_METADATA_URL,
        "Institutional target for the wide daily market panel.",
    ),
    ProviderCoverageSlo(
        "ATX.US.EQUITIES",
        "universe",
        dt.date(2010, 1, 1),
        10.0,
        4_000,
        None,
        7.0,
        DATABENTO_METADATA_URL,
        "Institutional target for survivorship-free US-listed universe membership.",
    ),
    ProviderCoverageSlo(
        "ATX.US.EQUITIES",
        "delistings",
        dt.date(2010, 1, 1),
        10.0,
        500,
        None,
        30.0,
        DATABENTO_METADATA_URL,
        "Institutional target for attributed public delisting events.",
    ),
```

- [ ] **Step 5: Create migration 0307**

Create `C:\atx\atx-db\src\atx_db\migrations\bodies_0307.py`:

```python
"""Public security-master view, per-schema coverage SLOs, and the measured item-count basis."""

from __future__ import annotations

import duckdb

from ..provider_coverage import DEFAULT_PROVIDER_COVERAGE_SLOS
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _public_schema_coverage_slos(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        "ALTER TABLE api_schema_coverage_slo "
        "ADD COLUMN IF NOT EXISTS item_count_basis VARCHAR NOT NULL DEFAULT 'distinct'"
    )
    conn.execute(
        """
        CREATE OR REPLACE VIEW v_security_master_public AS
        SELECT
            s.security_id,
            s.entity_id,
            s.issuer_id,
            s.primary_symbol,
            s.name,
            s.asset_class,
            s.country,
            s.currency,
            s.active,
            max(CASE WHEN h.id_type = 'CIK' THEN h.id_value END) AS cik,
            max(CASE WHEN h.id_type = 'LEI' THEN h.id_value END) AS lei,
            max(CASE WHEN h.id_type = 'FIGI' THEN h.id_value END) AS figi,
            coalesce(
                max(h.as_of_date),
                s.last_seen_date,
                s.first_seen_date,
                CAST(s.source_loaded_at AS DATE)
            ) AS as_of_date,
            coalesce(max(h.available_at), s.source_loaded_at) AS available_at,
            s.source,
            CAST(NULL AS VARCHAR) AS run_id,
            s.source_loaded_at
        FROM securities s
        LEFT JOIN security_identifier_history h
          ON h.security_id = s.security_id
         AND h.id_type IN ('CIK', 'LEI', 'FIGI')
         AND h.valid_to IS NULL
        GROUP BY
            s.security_id, s.entity_id, s.issuer_id, s.primary_symbol, s.name,
            s.asset_class, s.country, s.currency, s.active, s.last_seen_date,
            s.first_seen_date, s.source, s.source_loaded_at
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO api_schema_coverage_slo (
            dataset_id,schema_code,slo_version,expected_history_start,
            minimum_history_years,minimum_security_count,minimum_item_count,
            maximum_freshness_lag_days,citation,description,item_count_basis,is_active,
            valid_from,valid_to,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,true,TIMESTAMP '1900-01-01',NULL,now())
        """,
        [
            (
                slo.dataset_id,
                slo.schema_code,
                slo.slo_version,
                slo.expected_history_start,
                slo.minimum_history_years,
                slo.minimum_security_count,
                slo.minimum_item_count,
                slo.maximum_freshness_lag_days,
                slo.citation,
                slo.description,
                slo.item_count_basis,
            )
            for slo in DEFAULT_PROVIDER_COVERAGE_SLOS
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "v_security_master_public",
                "serving",
                "security",
                "security_id",
                "Public security-master contract: the securities spine with its open CIK, "
                "LEI and FIGI aliases. Deliberately excludes the internal-only CUSIP that "
                "v_security_master_current exposes.",
                '["security_id"]',
                "as_of_date and available_at are the newest identifier-interval stamps; "
                "the view is current-state, not bitemporal.",
            )
        ],
    )
    _catalog_fields_for_tables(conn, ("v_security_master_public",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=307,
        name="public_schema_coverage_slos",
        up=_public_schema_coverage_slos,
    )
]
```

> If `_catalog_fields_for_tables` only handles base tables, drop that call and let the
> `catalog_completeness_check` cover the view instead; confirm with
> `.venv\Scripts\python.exe -c "import inspect; from atx_db.migrations.bodies_0001_0137 import _catalog_fields_for_tables as f; print(inspect.getsource(f))"`.

Register it in `registry.py` (`from .bodies_0307 import MIGRATIONS as _MIGRATIONS_0307`, then `*_MIGRATIONS_0307,`).

- [ ] **Step 6: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_provider_coverage_slos.py tests/test_provider_coverage.py -n 0 -q`
Expected: PASS — `8 passed` in the new file plus the existing coverage suite. If `test_provider_coverage.py` pins the snapshot count at 8, run the new file once to read the exact schema count the catalog now yields (7 existing + 2 from Sprint 3 + 3 new) and update that assertion in this commit.

- [ ] **Step 7: Run the API and contract suites**

Run: `.venv\Scripts\python.exe -m pytest tests/test_public_api.py tests/test_schema_contract.py tests/test_module_boundaries.py tests/test_quality_smoke.py -n 0 -q --run-slow`
Expected: PASS. If the boundary snapshot pins `atx_db.api.catalog` symbols, add `SECURITY_MASTER_SCHEMA`, `UNIVERSE_SCHEMA` and `DELISTINGS_SCHEMA` to `tests/data/public_api_snapshot.json` and re-run.

- [ ] **Step 8: Commit**

```
git add src/atx_db/api/catalog.py src/atx_db/provider_coverage.py src/atx_db/migrations/bodies_0307.py src/atx_db/migrations/registry.py tests/test_provider_coverage_slos.py tests/test_provider_coverage.py tests/data/public_api_snapshot.json
git commit -m "feat(db): publish security-master, universe and delistings schemas with measured SLOs

Registers three new public record schemas over a CUSIP-free v_security_master_public
view, universe_us_listed_membership and delisting_events, and seeds an
api_schema_coverage_slo row for every public schema including Sprint 3 derived-metrics
and market-daily-1d, without which refresh_provider_coverage raises. Adds
item_count_basis so the standardized schema condition flips on measured item coverage
clearing the published threshold rather than on a raw distinct-code count.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---
### Task 8: Migration 0308 and `publication.py` — full-universe releases with a manifest and a diff

**Files:**
- Create: `src/atx_db/migrations/bodies_0308.py`
- Create: `src/atx_db/publication.py`
- Create: `scripts/publish_release.py`
- Modify: `src/atx_db/cli.py`, `src/atx_db/migrations/registry.py`, `tests/data/public_api_snapshot.json`
- Test: `tests/test_publication.py` (new)

**Interfaces:**
- Consumes: `atx_db.connection.DuckDBStore`; `atx_db.lake._object_schema`, `_schema_sha256` (same-package private import, the precedent Sprint 3's `panel_export.py` established); `atx_db.warehouse.now_utc_naive`, `file_sha256`, `json_dumps`; `atx_db.api.catalog.get_schema`, `_record_schema_sha256`; `atx_db.derived_registry.DERIVED_SOURCE_NAME`; `atx_db.market_daily.MARKET_DAILY_SOURCE_NAME`; `atx_db.universe_us_listed.DEFAULT_US_LISTED_UNIVERSE_ID` (Task 2).
- Produces:
  - Tables `publication_releases(release_id VARCHAR PRIMARY KEY, contract_version VARCHAR NOT NULL, out_dir VARCHAR NOT NULL, manifest_sha256 VARCHAR NOT NULL, dataset_count INTEGER NOT NULL, total_rows BIGINT NOT NULL, previous_release_id VARCHAR, created_at TIMESTAMP NOT NULL, run_id VARCHAR, source_loaded_at TIMESTAMP NOT NULL DEFAULT now())` and `publication_release_datasets(release_id VARCHAR NOT NULL, dataset_name VARCHAR NOT NULL, object_name VARCHAR NOT NULL, schema_code VARCHAR, parquet_path VARCHAR NOT NULL, row_count BIGINT NOT NULL, byte_count BIGINT NOT NULL, schema_sha256 VARCHAR NOT NULL, query_sha256 VARCHAR NOT NULL, parquet_sha256 VARCHAR NOT NULL, rows_added BIGINT, rows_removed BIGINT, rows_changed BIGINT, source_loaded_at TIMESTAMP NOT NULL DEFAULT now(), PRIMARY KEY (release_id, dataset_name))`, plus their catalog rows.
  - `atx_db.migrations.bodies_0308.MIGRATIONS` — `Migration(version=308, name="publication_releases", up=_publication_releases)`.
  - `atx_db.publication.PUBLICATION_CONTRACT_VERSION: str = "1.0.0"`
  - `atx_db.publication.ReleaseDataset` — frozen dataclass `(name: str, object_name: str, key_columns: tuple[str, ...], schema_dataset: str | None, schema_code: str | None)`.
  - `atx_db.publication.RELEASE_DATASETS: tuple[ReleaseDataset, ...]` — the six datasets below.
  - `atx_db.publication.ReleaseDatasetResult` — frozen dataclass `(name, object_name, parquet_path: Path, row_count: int, byte_count: int, schema_sha256: str, query_sha256: str, parquet_sha256: str, rows_added: int | None, rows_removed: int | None, rows_changed: int | None)`.
  - `atx_db.publication.ReleaseResult` — frozen dataclass `(release_id: str, out_dir: Path, manifest_path: Path, manifest_sha256: str, previous_release_id: str | None, datasets: tuple[ReleaseDatasetResult, ...])`.
  - `atx_db.publication.release_query(dataset: ReleaseDataset) -> str`
  - `atx_db.publication.row_digest_sql(columns: list[str]) -> str`
  - `atx_db.publication.diff_against(store, dataset, current_parquet: Path, previous_parquet: Path) -> tuple[int, int, int]`
  - `atx_db.publication.activation_stage_run_ids(store) -> dict[str, str]`
  - `atx_db.publication.publish_release(store, release_id: str, out_dir: Path | str, *, previous_dir: Path | str | None = None, run_id: str | None = None) -> ReleaseResult`
  - `atx_db.publication.read_release_manifest(path: Path | str) -> dict[str, object]`
  - `atx-db publish-release --db-path --release-id --out-dir [--previous-dir] [--run-id]` and `scripts/publish_release.py` with the same flags.

**The six release datasets.**

| `name` | `object_name` | `key_columns` | schema cited in the manifest |
| --- | --- | --- | --- |
| `security_master` | `v_security_master_public` | `("security_id",)` | `ATX.US.FUNDAMENTALS/security-master` |
| `universe` | `universe_us_listed_membership` | `("universe_id", "security_id", "valid_from")` | `ATX.US.EQUITIES/universe` |
| `delistings` | `delisting_events` | `("source", "security_id", "delist_date")` | `ATX.US.EQUITIES/delistings` |
| `fundamentals_core` | `fundamental_standardized` | `("standardized_id",)` | `ATX.US.FUNDAMENTALS/standardized` |
| `derived_metrics` | `derived_metric_values` | `("derived_value_id",)` | `ATX.US.FUNDAMENTALS/derived-metrics` |
| `market_daily` | `market_daily_metrics` | `("market_daily_id",)` | `ATX.US.EQUITIES/market-daily-1d` |

**The manifest.** One JSON file, `<out_dir>/<release_id>/manifest.json`, written with `sort_keys=True, separators=(",", ":")` so its own sha256 is reproducible:

```json
{
  "release_id": "2026-09-19",
  "contract_version": "1.0.0",
  "created_at": "2026-09-19T00:00:00",
  "previous_release_id": "2026-09-12",
  "activation_stage_run_ids": {"standardized": "warehouse-activate", "market_daily": "warehouse-activate"},
  "datasets": [
    {
      "name": "universe",
      "object": "universe_us_listed_membership",
      "schema": "ATX.US.EQUITIES/universe",
      "schema_sha256": "<lake._schema_sha256 of the exported columns>",
      "record_schema_sha256": "<api.catalog._record_schema_sha256 of the public contract>",
      "query_sha256": "<sha256 of the exact SQL text executed>",
      "parquet": "universe.parquet",
      "parquet_sha256": "<sha256 of the file bytes>",
      "row_count": 1234,
      "byte_count": 45678,
      "diff": {"rows_added": 12, "rows_removed": 3, "rows_changed": 5}
    }
  ]
}
```

`created_at` is the only clock read and it goes through `warehouse.now_utc_naive()`, which the Global Constraints explicitly permit for load stamps. It is lineage: nothing in a release is keyed on it.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_publication.py`:

```python
"""Tier1-S4 T8: scheduled full-universe publication with a manifest and a diff."""

from __future__ import annotations

import json

import pyarrow.parquet as pq
import pytest


def _seed_release_inputs(store, *, universe_rows=2):
    store.con.execute(
        "INSERT INTO securities (security_id, entity_id, primary_symbol, name, source) VALUES "
        "('SEC-1','CIK-0000000001','AAA','Alpha Inc','test'),"
        "('SEC-2','CIK-0000000002','BBB','Beta Inc','test')"
    )
    values = ",".join(
        f"('m-{i}','us_listed_v1','SEC-{i}','AAA',DATE '2024-01-02',NULL,"
        f"TIMESTAMP '2024-01-02 22:00:00','common','XNAS',true,'000000000{i}',5,'member',"
        f"'{{}}',1,DATE '2024-01-02','t','r')"
        for i in range(1, universe_rows + 1)
    )
    store.con.execute(
        "INSERT INTO universe_us_listed_membership (membership_id, universe_id, security_id, "
        "symbol, valid_from, valid_to, available_at, security_type, exchange_code, has_cik, cik, "
        "market_cap_decile, reason, rules_json, decision_count, as_of_date, source, run_id) "
        "VALUES " + values
    )


def test_publish_release_writes_a_parquet_per_dataset(tmp_store, tmp_path):
    from atx_db.publication import RELEASE_DATASETS, publish_release

    _seed_release_inputs(tmp_store)
    result = publish_release(tmp_store, "2026-09-19", tmp_path, run_id="t")
    assert len(result.datasets) == len(RELEASE_DATASETS)
    for dataset in result.datasets:
        assert dataset.parquet_path.exists()
        assert dataset.parquet_path.parent == tmp_path / "2026-09-19"


def test_the_universe_parquet_round_trips(tmp_store, tmp_path):
    from atx_db.publication import publish_release

    _seed_release_inputs(tmp_store)
    result = publish_release(tmp_store, "2026-09-19", tmp_path, run_id="t")
    universe = next(d for d in result.datasets if d.name == "universe")
    table = pq.read_table(universe.parquet_path)
    assert table.num_rows == 2
    assert universe.row_count == 2
    assert "security_id" in table.column_names


def test_the_manifest_carries_every_required_key(tmp_store, tmp_path):
    from atx_db.publication import PUBLICATION_CONTRACT_VERSION, publish_release, read_release_manifest

    _seed_release_inputs(tmp_store)
    result = publish_release(tmp_store, "2026-09-19", tmp_path, run_id="t")
    manifest = read_release_manifest(result.manifest_path)
    assert manifest["release_id"] == "2026-09-19"
    assert manifest["contract_version"] == PUBLICATION_CONTRACT_VERSION
    assert manifest["previous_release_id"] is None
    assert isinstance(manifest["activation_stage_run_ids"], dict)
    for entry in manifest["datasets"]:
        assert set(entry) == {
            "name", "object", "schema", "schema_sha256", "record_schema_sha256",
            "query_sha256", "parquet", "parquet_sha256", "row_count", "byte_count", "diff",
        }
        assert len(entry["query_sha256"]) == 64
        assert len(entry["parquet_sha256"]) == 64
        assert len(entry["schema_sha256"]) == 64


def test_two_identical_releases_produce_identical_hashes(tmp_store, tmp_path):
    from atx_db.publication import publish_release

    _seed_release_inputs(tmp_store)
    first = publish_release(tmp_store, "r1", tmp_path, run_id="t")
    second = publish_release(tmp_store, "r2", tmp_path, run_id="t")
    by_name_first = {d.name: d for d in first.datasets}
    by_name_second = {d.name: d for d in second.datasets}
    for name, dataset in by_name_first.items():
        assert dataset.query_sha256 == by_name_second[name].query_sha256
        assert dataset.schema_sha256 == by_name_second[name].schema_sha256
        assert dataset.row_count == by_name_second[name].row_count


def test_the_diff_reports_added_removed_and_changed(tmp_store, tmp_path):
    from atx_db.publication import publish_release

    _seed_release_inputs(tmp_store)
    first = publish_release(tmp_store, "r1", tmp_path, run_id="t")

    tmp_store.con.execute(
        "UPDATE universe_us_listed_membership SET market_cap_decile = 9 "
        "WHERE membership_id = 'm-1'"
    )
    tmp_store.con.execute(
        "DELETE FROM universe_us_listed_membership WHERE membership_id = 'm-2'"
    )
    tmp_store.con.execute(
        "INSERT INTO universe_us_listed_membership (membership_id, universe_id, security_id, "
        "symbol, valid_from, valid_to, available_at, security_type, exchange_code, has_cik, cik, "
        "market_cap_decile, reason, rules_json, decision_count, as_of_date, source, run_id) VALUES "
        "('m-3','us_listed_v1','SEC-3','CCC',DATE '2024-01-02',NULL,"
        "TIMESTAMP '2024-01-02 22:00:00','common','XNAS',false,NULL,3,'member_no_cik','{}',1,"
        "DATE '2024-01-02','t','r')"
    )

    second = publish_release(
        tmp_store, "r2", tmp_path, previous_dir=first.out_dir, run_id="t"
    )
    universe = next(d for d in second.datasets if d.name == "universe")
    assert universe.rows_added == 1
    assert universe.rows_removed == 1
    assert universe.rows_changed == 1
    assert second.previous_release_id == "r1"


def test_the_release_is_recorded_in_the_warehouse(tmp_store, tmp_path):
    from atx_db.publication import publish_release

    _seed_release_inputs(tmp_store)
    publish_release(tmp_store, "2026-09-19", tmp_path, run_id="t")
    header = tmp_store.con.execute(
        "SELECT dataset_count, previous_release_id FROM publication_releases WHERE release_id = ?",
        ["2026-09-19"],
    ).fetchone()
    assert int(header[0]) == 6
    assert header[1] is None
    rows = tmp_store.con.execute(
        "SELECT count(*) FROM publication_release_datasets WHERE release_id = ?",
        ["2026-09-19"],
    ).fetchone()[0]
    assert int(rows) == 6


def test_the_cli_publishes_a_release(tmp_store, tmp_path, built_warehouse, capsys):
    from atx_db import cli

    db_path = built_warehouse("publish_release.duckdb")
    code = cli.main(
        [
            "publish-release",
            "--db-path",
            str(db_path),
            "--release-id",
            "cli-1",
            "--out-dir",
            str(tmp_path),
        ]
    )
    assert code == 0
    payload = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
    assert payload["release_id"] == "cli-1"
    assert payload["dataset_count"] == 6
    assert (tmp_path / "cli-1" / "manifest.json").exists()
```

- [ ] **Step 2: Run and watch it fail**

Run: `.venv\Scripts\python.exe -m pytest tests/test_publication.py -n 0 -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.publication'` and, for the last test, `argparse` rejecting the unknown command `publish-release`.

- [ ] **Step 3: Create migration 0308**

Create `C:\atx\atx-db\src\atx_db\migrations\bodies_0308.py`:

```python
"""Publication release ledger: one row per release, one per release dataset."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _publication_releases(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS publication_releases (
            release_id VARCHAR PRIMARY KEY,
            contract_version VARCHAR NOT NULL,
            out_dir VARCHAR NOT NULL,
            manifest_sha256 VARCHAR NOT NULL,
            dataset_count INTEGER NOT NULL,
            total_rows BIGINT NOT NULL,
            previous_release_id VARCHAR,
            created_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE TABLE IF NOT EXISTS publication_release_datasets (
            release_id VARCHAR NOT NULL,
            dataset_name VARCHAR NOT NULL,
            object_name VARCHAR NOT NULL,
            schema_code VARCHAR,
            parquet_path VARCHAR NOT NULL,
            row_count BIGINT NOT NULL,
            byte_count BIGINT NOT NULL,
            schema_sha256 VARCHAR NOT NULL,
            query_sha256 VARCHAR NOT NULL,
            parquet_sha256 VARCHAR NOT NULL,
            rows_added BIGINT,
            rows_removed BIGINT,
            rows_changed BIGINT,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (release_id, dataset_name)
        );
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "publication_releases",
                "control",
                "publication_release",
                "release_id",
                "One row per scheduled full-universe publication, with the manifest hash "
                "and the release it diffs against.",
                '["release_id"]',
                "created_at is a warehouse load stamp and is never a point-in-time key.",
            ),
            (
                "publication_release_datasets",
                "control",
                "publication_release_dataset",
                "release_id,dataset_name",
                "One row per dataset in a release: Parquet path, row count, schema/query/"
                "file hashes, and the added/removed/changed counts against the previous "
                "release.",
                '["release_id","dataset_name"]',
                "Lineage only; the published Parquet files carry the PIT columns.",
            ),
        ],
    )
    _catalog_fields_for_tables(
        conn, ("publication_releases", "publication_release_datasets")
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=308, name="publication_releases", up=_publication_releases)
]
```

Register it in `registry.py` (`from .bodies_0308 import MIGRATIONS as _MIGRATIONS_0308`, then `*_MIGRATIONS_0308,`).

- [ ] **Step 4: Write `publication.py`**

Create `C:\atx\atx-db\src\atx_db\publication.py`:

```python
"""Scheduled full-universe publication: Parquet, one manifest, one diff.

A release is a directory of Parquet files plus a single ``manifest.json`` that pins, per
dataset: the exported column schema (``lake._schema_sha256``), the public record contract
(``api.catalog._record_schema_sha256``), the exact SQL text executed, the file bytes, the
row count, and the added/removed/changed counts against the previous release. Every hash
is content-addressed, so two releases of an unchanged warehouse agree on everything except
the load stamp.

The diff is computed in DuckDB over the two Parquet files with a full outer join on the
dataset's key columns and a row digest over every exported column, so it is exact rather
than a row-count delta.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

from .api.catalog import _record_schema_sha256, get_schema
from .connection import DuckDBStore
from .lake import _object_schema, _schema_sha256
from .warehouse import file_sha256, now_utc_naive


PUBLICATION_CONTRACT_VERSION = "1.0.0"
MANIFEST_NAME = "manifest.json"


@dataclass(frozen=True)
class ReleaseDataset:
    name: str
    object_name: str
    key_columns: tuple[str, ...]
    schema_dataset: str | None
    schema_code: str | None

    @property
    def schema_ref(self) -> str | None:
        if self.schema_dataset is None or self.schema_code is None:
            return None
        return f"{self.schema_dataset}/{self.schema_code}"


RELEASE_DATASETS: tuple[ReleaseDataset, ...] = (
    ReleaseDataset("security_master", "v_security_master_public", ("security_id",),
                   "ATX.US.FUNDAMENTALS", "security-master"),
    ReleaseDataset("universe", "universe_us_listed_membership",
                   ("universe_id", "security_id", "valid_from"),
                   "ATX.US.EQUITIES", "universe"),
    ReleaseDataset("delistings", "delisting_events", ("source", "security_id", "delist_date"),
                   "ATX.US.EQUITIES", "delistings"),
    ReleaseDataset("fundamentals_core", "fundamental_standardized", ("standardized_id",),
                   "ATX.US.FUNDAMENTALS", "standardized"),
    ReleaseDataset("derived_metrics", "derived_metric_values", ("derived_value_id",),
                   "ATX.US.FUNDAMENTALS", "derived-metrics"),
    ReleaseDataset("market_daily", "market_daily_metrics", ("market_daily_id",),
                   "ATX.US.EQUITIES", "market-daily-1d"),
)


@dataclass(frozen=True)
class ReleaseDatasetResult:
    name: str
    object_name: str
    parquet_path: Path
    row_count: int
    byte_count: int
    schema_sha256: str
    query_sha256: str
    parquet_sha256: str
    rows_added: int | None
    rows_removed: int | None
    rows_changed: int | None


@dataclass(frozen=True)
class ReleaseResult:
    release_id: str
    out_dir: Path
    manifest_path: Path
    manifest_sha256: str
    previous_release_id: str | None
    datasets: tuple[ReleaseDatasetResult, ...]

    @property
    def total_rows(self) -> int:
        return sum(dataset.row_count for dataset in self.datasets)
```

Continue appending to `C:\atx\atx-db\src\atx_db\publication.py`:

```python
def _quote(identifier: str) -> str:
    if not identifier.replace("_", "").isalnum() or identifier[0].isdigit():
        raise ValueError(f"unsafe identifier: {identifier!r}")
    return f'"{identifier}"'


def _sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def release_query(dataset: ReleaseDataset, columns: list[str]) -> str:
    """The exact SQL a release executes for one dataset.

    Columns are listed explicitly (never ``SELECT *``) so a warehouse column added after
    the release was cut cannot silently change the export, and the ORDER BY is the
    dataset's key so the Parquet bytes are reproducible.
    """

    projection = ", ".join(_quote(column) for column in columns)
    order = ", ".join(_quote(column) for column in dataset.key_columns)
    return f"SELECT {projection} FROM {_quote(dataset.object_name)} ORDER BY {order}"


def row_digest_sql(alias: str, columns: list[str]) -> str:
    """A content hash over every exported column of one row."""

    parts = ", ".join(f"CAST({alias}.{_quote(column)} AS VARCHAR)" for column in columns)
    return f"sha256(concat_ws('\u001f', {parts}))"


def diff_against(
    store: DuckDBStore,
    dataset: ReleaseDataset,
    columns: list[str],
    current_parquet: Path,
    previous_parquet: Path,
) -> tuple[int, int, int]:
    """(added, removed, changed) between two release Parquet files for one dataset.

    A key present only in the current file is ADDED, only in the previous file is REMOVED,
    and present in both with a different row digest is CHANGED. Non-key column additions
    between releases are not handled: the digest is taken over the CURRENT column list on
    both sides, and a schema change is already visible as a differing ``schema_sha256``.
    """

    join = " AND ".join(
        f"cur.{_quote(column)} IS NOT DISTINCT FROM prev.{_quote(column)}"
        for column in dataset.key_columns
    )
    first_key = _quote(dataset.key_columns[0])
    row = store.con.execute(
        f"""
        WITH cur AS (
            SELECT *, {row_digest_sql("t", columns)} AS _digest
            FROM read_parquet(?) AS t
        ),
        prev AS (
            SELECT *, {row_digest_sql("t", columns)} AS _digest
            FROM read_parquet(?) AS t
        )
        SELECT
            count(*) FILTER (WHERE prev.{first_key} IS NULL)::BIGINT AS added,
            count(*) FILTER (WHERE cur.{first_key} IS NULL)::BIGINT AS removed,
            count(*) FILTER (
                WHERE cur.{first_key} IS NOT NULL
                  AND prev.{first_key} IS NOT NULL
                  AND cur._digest <> prev._digest
            )::BIGINT AS changed
        FROM cur
        FULL OUTER JOIN prev ON {join}
        """,
        [str(current_parquet), str(previous_parquet)],
    ).fetchone()
    return int(row[0]), int(row[1]), int(row[2])


def activation_stage_run_ids(store: DuckDBStore) -> dict[str, str]:
    """The newest completed run id per activation stage, for release provenance."""

    rows = store.con.execute(
        """
        SELECT stage, run_id
        FROM (
            SELECT stage, run_id,
                   row_number() OVER (PARTITION BY stage ORDER BY started_at DESC, run_id DESC) AS rn
            FROM activation_stage_runs
            WHERE status = 'completed'
        )
        WHERE rn = 1
        ORDER BY stage
        """
    ).fetchall()
    return {str(stage): str(run_id) for stage, run_id in rows}


def read_release_manifest(path: Path | str) -> dict[str, object]:
    """Read a release manifest written by :func:`publish_release`."""

    return json.loads(Path(path).read_text(encoding="utf-8"))
```

Continue appending to `C:\atx\atx-db\src\atx_db\publication.py`:

```python
def publish_release(
    store: DuckDBStore,
    release_id: str,
    out_dir: Path | str,
    *,
    previous_dir: Path | str | None = None,
    run_id: str | None = None,
) -> ReleaseResult:
    """Publish every release dataset to ``<out_dir>/<release_id>/`` with one manifest.

    ``previous_dir`` is a prior release directory; when given, each dataset is diffed
    against the matching Parquet file in it and the added/removed/changed counts land in
    both the manifest and ``publication_release_datasets``. A dataset with no counterpart
    in the previous release reports ``None`` for all three rather than claiming every row
    is new.
    """

    store.initialize()
    release_dir = Path(out_dir) / release_id
    release_dir.mkdir(parents=True, exist_ok=True)
    previous_manifest: dict[str, object] | None = None
    previous_root = None if previous_dir is None else Path(previous_dir)
    if previous_root is not None and (previous_root / MANIFEST_NAME).exists():
        previous_manifest = read_release_manifest(previous_root / MANIFEST_NAME)

    results: list[ReleaseDatasetResult] = []
    manifest_datasets: list[dict[str, object]] = []
    for dataset in RELEASE_DATASETS:
        schema = _object_schema(store, dataset.object_name)
        columns = [str(column["name"]) for column in schema]
        query = release_query(dataset, columns)
        parquet_path = release_dir / f"{dataset.name}.parquet"
        store.con.execute(
            f"COPY ({query}) TO ? (FORMAT PARQUET, COMPRESSION ZSTD)",
            [str(parquet_path)],
        )
        row_count = int(
            store.con.execute(
                "SELECT count(*) FROM read_parquet(?)", [str(parquet_path)]
            ).fetchone()[0]
        )
        added = removed = changed = None
        if previous_root is not None:
            previous_parquet = previous_root / f"{dataset.name}.parquet"
            if previous_parquet.exists():
                added, removed, changed = diff_against(
                    store, dataset, columns, parquet_path, previous_parquet
                )
        record_sha = (
            None
            if dataset.schema_dataset is None or dataset.schema_code is None
            else _record_schema_sha256(get_schema(dataset.schema_dataset, dataset.schema_code))
        )
        result = ReleaseDatasetResult(
            name=dataset.name,
            object_name=dataset.object_name,
            parquet_path=parquet_path,
            row_count=row_count,
            byte_count=parquet_path.stat().st_size,
            schema_sha256=_schema_sha256(schema),
            query_sha256=_sha256_text(query),
            parquet_sha256=file_sha256(parquet_path),
            rows_added=added,
            rows_removed=removed,
            rows_changed=changed,
        )
        results.append(result)
        manifest_datasets.append(
            {
                "name": dataset.name,
                "object": dataset.object_name,
                "schema": dataset.schema_ref,
                "schema_sha256": result.schema_sha256,
                "record_schema_sha256": record_sha,
                "query_sha256": result.query_sha256,
                "parquet": parquet_path.name,
                "parquet_sha256": result.parquet_sha256,
                "row_count": result.row_count,
                "byte_count": result.byte_count,
                "diff": {
                    "rows_added": result.rows_added,
                    "rows_removed": result.rows_removed,
                    "rows_changed": result.rows_changed,
                },
            }
        )

    previous_release_id = (
        None if previous_manifest is None else str(previous_manifest["release_id"])
    )
    created_at = now_utc_naive()
    manifest = {
        "release_id": release_id,
        "contract_version": PUBLICATION_CONTRACT_VERSION,
        "created_at": created_at.isoformat(),
        "previous_release_id": previous_release_id,
        "activation_stage_run_ids": activation_stage_run_ids(store),
        "datasets": manifest_datasets,
    }
    payload = json.dumps(manifest, sort_keys=True, separators=(",", ":"))
    manifest_path = release_dir / MANIFEST_NAME
    manifest_path.write_text(payload, encoding="utf-8")
    manifest_sha256 = _sha256_text(payload)

    with store.transaction():
        store.con.execute(
            "DELETE FROM publication_release_datasets WHERE release_id = ?", [release_id]
        )
        store.con.execute("DELETE FROM publication_releases WHERE release_id = ?", [release_id])
        store.con.execute(
            """
            INSERT INTO publication_releases (
                release_id, contract_version, out_dir, manifest_sha256, dataset_count,
                total_rows, previous_release_id, created_at, run_id
            ) VALUES (?,?,?,?,?,?,?,?,?)
            """,
            [
                release_id,
                PUBLICATION_CONTRACT_VERSION,
                str(release_dir),
                manifest_sha256,
                len(results),
                sum(result.row_count for result in results),
                previous_release_id,
                created_at,
                run_id,
            ],
        )
        store.con.executemany(
            """
            INSERT INTO publication_release_datasets (
                release_id, dataset_name, object_name, schema_code, parquet_path, row_count,
                byte_count, schema_sha256, query_sha256, parquet_sha256, rows_added,
                rows_removed, rows_changed
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)
            """,
            [
                (
                    release_id,
                    result.name,
                    result.object_name,
                    dataset.schema_ref,
                    str(result.parquet_path),
                    result.row_count,
                    result.byte_count,
                    result.schema_sha256,
                    result.query_sha256,
                    result.parquet_sha256,
                    result.rows_added,
                    result.rows_removed,
                    result.rows_changed,
                )
                for dataset, result in zip(RELEASE_DATASETS, results)
            ],
        )

    return ReleaseResult(
        release_id=release_id,
        out_dir=release_dir,
        manifest_path=manifest_path,
        manifest_sha256=manifest_sha256,
        previous_release_id=previous_release_id,
        datasets=tuple(results),
    )
```

- [ ] **Step 5: Add the CLI subcommand and the operator script**

In `C:\atx\atx-db\src\atx_db\cli.py`, inside `_build_parser`, immediately before `return parser`:

```python
    release = commands.add_parser(
        "publish-release",
        help="Publish the full-universe Parquet release and its manifest",
    )
    release.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    release.add_argument("--release-id", required=True)
    release.add_argument("--out-dir", type=Path, required=True)
    release.add_argument("--previous-dir", type=Path)
    release.add_argument("--run-id")
```

and inside `main`, beside the other command branches:

```python
    if args.command == "publish-release":
        from .publication import publish_release

        with DuckDBStore(args.db_path) as store:
            result = publish_release(
                store,
                args.release_id,
                args.out_dir,
                previous_dir=args.previous_dir,
                run_id=args.run_id,
            )
        _json(
            {
                "release_id": result.release_id,
                "out_dir": str(result.out_dir),
                "manifest": str(result.manifest_path),
                "manifest_sha256": result.manifest_sha256,
                "previous_release_id": result.previous_release_id,
                "dataset_count": len(result.datasets),
                "total_rows": result.total_rows,
            }
        )
        return 0
```

Create `C:\atx\atx-db\scripts\publish_release.py`:

```python
"""Operator entry point for a scheduled full-universe publication."""

from __future__ import annotations

import sys

from atx_db import cli


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    return cli.main(["publish-release", *args])


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 6: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_publication.py -n 0 -q`
Expected: PASS — `7 passed`.

If the `COPY (...) TO ?` parameterised form is rejected by the installed DuckDB, substitute a
quoted literal path built with `str(parquet_path).replace("'", "''")` and keep the rest
unchanged; note the substitution in the commit body.

- [ ] **Step 7: Refresh the public API snapshot and run the neighbouring suites**

Run: `.venv\Scripts\python.exe -c "import json,pathlib; p=pathlib.Path('tests/data/public_api_snapshot.json'); d=json.loads(p.read_text()); d['atx_db']=sorted(set(d['atx_db'])|{'publication'}); p.write_text(json.dumps(d, indent=2, sort_keys=True)+chr(10))"`
Expected: no output.

Run: `.venv\Scripts\python.exe -m pytest tests/test_publication.py tests/test_module_boundaries.py tests/test_lake.py tests/test_migration_governance.py -n 0 -q --run-slow`
Expected: PASS.

- [ ] **Step 8: Commit**

```
git add src/atx_db/publication.py src/atx_db/migrations/bodies_0308.py src/atx_db/migrations/registry.py src/atx_db/cli.py scripts/publish_release.py tests/test_publication.py tests/data/public_api_snapshot.json
git commit -m "feat(db): scheduled full-universe publication with manifest and release diff

Adds migration 0308 (publication_releases, publication_release_datasets) and
publication.py: six release datasets exported to Parquet with an explicit column list and
a key ORDER BY, one manifest pinning the exported schema hash, the public record-contract
hash, the exact query text, the file bytes and the activation stage run ids, and an exact
added/removed/changed diff against the previous release computed in DuckDB over the two
Parquet files. Exposed as atx-db publish-release.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---
### Task 9: `scripts/generate_data_dictionary.py` and `docs/DATA_DICTIONARY.md`

**Files:**
- Create: `scripts/generate_data_dictionary.py`
- Create: `docs/DATA_DICTIONARY.md` (generated, committed)
- Test: `tests/test_data_dictionary.py` (new)

**Interfaces:**
- Consumes: `atx_db.item_registry.read_fundamental_item_seed`, `FundamentalItemSeedRow`, `SEED_PATH`; `atx_db.derived_registry.default_derived_definitions` (Sprint 3 Task 3); `atx_db.api.catalog.DATASETS`, `RecordSchema`, `public_schema`; `atx_db.universe_us_listed.ELIGIBLE_SECURITY_TYPES`, `EXCHANGE_LABELS` (Task 2); `atx_db.delisting_evidence.EVIDENCE_PRECEDENCE`, `REASON_CATEGORIES` (Task 3); `atx_db.publication.RELEASE_DATASETS` (Task 8).
- Produces:
  - `scripts/generate_data_dictionary.py` with `DATA_DICTIONARY_PATH: Path`, `MARKET_DAILY_SPINE_COLUMNS: tuple[str, ...]`, `render_data_dictionary() -> str`, `main(argv: list[str] | None = None) -> int` supporting `--check` (exit 1 and print a unified diff when the committed file is stale, write nothing) and the default write mode.
  - `docs/DATA_DICTIONARY.md` — sections: *Canonical statement items*, *Derived metrics*, *Daily market panel*, *Universe*, *Delistings*, *Public API schemas*, *Release datasets*.

**Why it needs no warehouse.** Every input is a committed registry or a Python constant, so the generator is a pure function of the repository. That is what lets CI run it in `--check` mode on a machine with no DuckDB file and fail the build on stale docs (charter item 8).

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_data_dictionary.py`:

```python
"""Tier1-S4 T9: the data dictionary is generated, deterministic and never stale."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "scripts" / "generate_data_dictionary.py"


def _load_generator():
    spec = importlib.util.spec_from_file_location("generate_data_dictionary", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules["generate_data_dictionary"] = module
    spec.loader.exec_module(module)
    return module


def test_the_generator_is_deterministic():
    module = _load_generator()
    assert module.render_data_dictionary() == module.render_data_dictionary()


def test_the_committed_dictionary_is_current():
    module = _load_generator()
    committed = module.DATA_DICTIONARY_PATH.read_text(encoding="utf-8")
    assert committed == module.render_data_dictionary()


def test_check_mode_passes_on_a_current_file():
    module = _load_generator()
    assert module.main(["--check"]) == 0


def test_check_mode_fails_on_a_stale_file(tmp_path, monkeypatch, capsys):
    module = _load_generator()
    stale = tmp_path / "DATA_DICTIONARY.md"
    stale.write_text("# stale\n", encoding="utf-8")
    monkeypatch.setattr(module, "DATA_DICTIONARY_PATH", stale)
    assert module.main(["--check"]) == 1
    assert "DATA_DICTIONARY.md is stale" in capsys.readouterr().out


def test_write_mode_refreshes_the_file(tmp_path, monkeypatch):
    module = _load_generator()
    target = tmp_path / "DATA_DICTIONARY.md"
    monkeypatch.setattr(module, "DATA_DICTIONARY_PATH", target)
    assert module.main([]) == 0
    assert target.read_text(encoding="utf-8") == module.render_data_dictionary()


def test_the_dictionary_covers_every_required_section():
    module = _load_generator()
    text = module.render_data_dictionary()
    for heading in (
        "## Canonical statement items",
        "## Derived metrics",
        "## Daily market panel",
        "## Universe",
        "## Delistings",
        "## Public API schemas",
        "## Release datasets",
    ):
        assert heading in text


def test_every_public_schema_appears():
    from atx_db.api.catalog import DATASETS

    module = _load_generator()
    text = module.render_data_dictionary()
    for dataset in DATASETS:
        for schema in dataset.schemas:
            assert f"{dataset.code}/{schema.code}" in text


def test_the_universe_vocabulary_is_documented():
    from atx_db.universe_us_listed import ELIGIBLE_SECURITY_TYPES, EXCHANGE_LABELS

    module = _load_generator()
    text = module.render_data_dictionary()
    for value in (*ELIGIBLE_SECURITY_TYPES, *EXCHANGE_LABELS):
        assert value in text


def test_the_delisting_vocabulary_is_documented():
    from atx_db.delisting_evidence import EVIDENCE_PRECEDENCE, REASON_CATEGORIES

    module = _load_generator()
    text = module.render_data_dictionary()
    for reason in REASON_CATEGORIES:
        assert reason in text
    for kind, _rank, _code, _reason, _confidence in EVIDENCE_PRECEDENCE:
        assert kind in text


def test_the_dictionary_never_touches_the_warehouse(monkeypatch):
    import duckdb

    def _boom(*_args, **_kwargs):
        raise AssertionError("the data dictionary must not open a database")

    monkeypatch.setattr(duckdb, "connect", _boom)
    module = _load_generator()
    assert module.render_data_dictionary()
```

- [ ] **Step 2: Run and watch it fail**

Run: `.venv\Scripts\python.exe -m pytest tests/test_data_dictionary.py -n 0 -q`
Expected: FAIL — `FileNotFoundError` for `scripts/generate_data_dictionary.py` on every test.

- [ ] **Step 3: Write the generator**

First confirm the derived-definition field names Sprint 3 produced:

Run: `.venv\Scripts\python.exe -c "from atx_db.derived_registry import default_derived_definitions as d; import dataclasses; print([f.name for f in dataclasses.fields(d()[0])])"`
Expected: the definition's field names (`metric_code`, `family`, `expression`, `metric_window`, ...). Use those exact attribute names below.

Create `C:\atx\atx-db\scripts\generate_data_dictionary.py`:

```python
"""Render docs/DATA_DICTIONARY.md from the committed registries.

Pure function of the repository: the item seed, the derived-metric seed, the public
record schemas and the universe/delisting vocabularies. It never opens a warehouse, so CI
can run it in --check mode and fail the build when the committed file is stale.
"""

from __future__ import annotations

import argparse
import difflib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

from atx_db.api.catalog import DATASETS  # noqa: E402
from atx_db.delisting_evidence import EVIDENCE_PRECEDENCE, REASON_CATEGORIES  # noqa: E402
from atx_db.derived_registry import default_derived_definitions  # noqa: E402
from atx_db.item_registry import read_fundamental_item_seed  # noqa: E402
from atx_db.publication import RELEASE_DATASETS  # noqa: E402
from atx_db.universe_us_listed import (  # noqa: E402
    ELIGIBLE_SECURITY_TYPES,
    EXCHANGE_LABELS,
    SECURITY_TYPE_PATTERNS,
)

DATA_DICTIONARY_PATH = ROOT / "docs" / "DATA_DICTIONARY.md"

# The fixed spine of market_daily_metrics (migration 0302); the metric columns come from
# the derived seed's window='daily' rows, so the two can never drift apart here.
MARKET_DAILY_SPINE_COLUMNS: tuple[tuple[str, str], ...] = (
    ("market_daily_id", "Content hash of (source, security_id, trade_date)."),
    ("source", "Producing engine identity."),
    ("security_id", "Stable ATX security identifier."),
    ("symbol", "Ticker on the bar."),
    ("trade_date", "Trading session date."),
    ("close", "Session close price, unadjusted."),
    ("adj_close", "Split/dividend adjusted close; the only return input."),
    ("volume", "Reported share volume."),
    ("shares_outstanding", "Point-in-time shares, dei preferred over archive."),
    ("shares_source", "'dei' or 'archive'."),
    ("shares_reconciliation_ratio", "dei shares / archive shares on the same session."),
    ("fundamental_available_at", "Availability of the newest fundamental input."),
    ("available_at", "Earliest timestamp a consumer may use the row."),
    ("inputs_hash", "Content hash of the inputs that produced the row."),
    ("as_of_date", "Economic observation date."),
    ("is_latest_revision", "Chain-head flag."),
    ("run_id", "Producing run."),
    ("source_loaded_at", "Warehouse load stamp; never a signal input."),
)

_ESCAPES = str.maketrans({"|": "\|", "\n": " ", "\r": " "})


def _cell(value: object) -> str:
    if value is None:
        return ""
    return str(value).strip().translate(_ESCAPES)


def _table(headers: tuple[str, ...], rows: list[tuple[object, ...]]) -> list[str]:
    lines = ["| " + " | ".join(headers) + " |"]
    lines.append("| " + " | ".join("---" for _ in headers) + " |")
    lines.extend("| " + " | ".join(_cell(value) for value in row) + " |" for row in rows)
    return lines


def _item_section() -> list[str]:
    seen: dict[int, tuple[object, ...]] = {}
    for row in read_fundamental_item_seed():
        if row.item_id in seen:
            continue
        seen[row.item_id] = (
            row.item_id,
            row.canonical_code,
            row.statement,
            row.section,
            row.unit_type,
            "yes" if row.is_derived else "no",
            row.definition,
        )
    lines = [
        "## Canonical statement items",
        "",
        f"{len(seen)} items, from `src/atx_db/seeds/fundamental_items.csv`. One row per "
        "`item_id`; alias rows are collapsed.",
        "",
    ]
    lines.extend(
        _table(
            ("item_id", "canonical_code", "statement", "section", "unit_type", "derived", "definition"),
            [seen[item_id] for item_id in sorted(seen)],
        )
    )
    return lines


def _derived_section() -> list[str]:
    definitions = sorted(default_derived_definitions(), key=lambda d: (d.family, d.metric_code))
    lines = [
        "## Derived metrics",
        "",
        f"{len(definitions)} metrics, from `src/atx_db/seeds/derived_metric_definitions.csv`. "
        "Every value carries `available_at = max(input available_at)` and an `inputs_hash`.",
        "",
    ]
    lines.extend(
        _table(
            ("family", "metric", "window", "expression", "description"),
            [
                (d.family, d.metric_code, d.metric_window, d.expression, d.description)
                for d in definitions
            ],
        )
    )
    return lines


def _market_daily_section() -> list[str]:
    daily = sorted(
        (d for d in default_derived_definitions() if d.metric_window == "daily"),
        key=lambda d: d.metric_code,
    )
    lines = [
        "## Daily market panel",
        "",
        "`market_daily_metrics` — one row per (security_id, trade_date). The spine is fixed; "
        "the metric columns are exactly the `window='daily'` rows of the derived seed.",
        "",
        "### Spine columns",
        "",
    ]
    lines.extend(_table(("column", "meaning"), list(MARKET_DAILY_SPINE_COLUMNS)))
    lines.extend(["", "### Metric columns", ""])
    lines.extend(
        _table(
            ("column", "family", "expression"),
            [(d.metric_code, d.family, d.expression) for d in daily],
        )
    )
    return lines
```

Continue appending to `C:\atx\atx-db\scripts\generate_data_dictionary.py`:

```python
def _universe_section() -> list[str]:
    lines = [
        "## Universe",
        "",
        "`universe_us_listed_membership` -- interval-keyed point-in-time membership. "
        "`valid_to` extends `lookback_days` trading sessions past the security's last bar; "
        "an interval that reaches the archive end stays open. `market_cap_decile` is the "
        "decile AT `valid_from` only.",
        "",
        "### Eligible exchanges",
        "",
    ]
    lines.extend(
        _table(
            ("exchange_code", "venue"),
            [(code, EXCHANGE_LABELS[code]) for code in sorted(EXCHANGE_LABELS)],
        )
    )
    lines.extend(
        [
            "",
            "### Security types",
            "",
            "Eligible: "
            + ", ".join(f"`{value}`" for value in ELIGIBLE_SECURITY_TYPES)
            + ". Every other label is excluded from the universe and counted in the "
            "build's quality-check details.",
            "",
        ]
    )
    lines.extend(
        _table(
            ("security_type", "matched by"),
            [
                *[(label, f"`{pattern.pattern}`") for label, pattern in SECURITY_TYPE_PATTERNS],
                ("ETF", "the directory `etf` boolean"),
                ("test", "the directory `test_issue` boolean"),
                ("common", "no pattern matched"),
                ("unknown", "no security name available"),
            ],
        )
    )
    lines.extend(
        [
            "",
            "### Membership reasons",
            "",
            "- `member` -- in the universe with a resolved CIK (the fundamentals universe).",
            "- `member_no_cik` -- in the market universe, CIK unresolved. Retained and "
            "counted, never dropped.",
        ]
    )
    return lines


def _delisting_section() -> list[str]:
    lines = [
        "## Delistings",
        "",
        "`delisting_evidence` holds one row per independent piece of public evidence; "
        "`delisting_events` keeps the minimum `evidence_rank` per "
        "(security_id, delist_date).",
        "",
        "### Evidence precedence",
        "",
    ]
    lines.extend(
        _table(
            ("rank", "evidence_kind", "delist_code", "default reason", "confidence"),
            [
                (rank, kind, code, reason, confidence)
                for kind, rank, code, reason, confidence in sorted(
                    EVIDENCE_PRECEDENCE, key=lambda row: row[1]
                )
            ],
        )
    )
    lines.extend(
        [
            "",
            "### Reason categories",
            "",
            ", ".join(f"`{value}`" for value in REASON_CATEGORIES) + ".",
            "",
            "A Nasdaq `financial_status` bankruptcy flag (`Q`) on or before the delist date "
            "upgrades any row to `bankruptcy` with `high` confidence.",
        ]
    )
    return lines


def _api_section() -> list[str]:
    lines = ["## Public API schemas", ""]
    for dataset in DATASETS:
        for schema in dataset.schemas:
            lines.extend(
                [
                    f"### {dataset.code}/{schema.code}",
                    "",
                    f"{schema.title} (v{schema.version}) over `{schema.source_table}`; "
                    f"time column `{schema.time_column}`; natural key "
                    f"`{', '.join(schema.natural_key)}`.",
                    "",
                ]
            )
            lines.extend(
                _table(
                    ("field", "type", "unit", "nullable", "filterable", "description"),
                    [
                        (
                            field.name,
                            field.data_type,
                            field.unit,
                            "yes" if field.nullable else "no",
                            "yes" if field.filterable else "no",
                            field.description,
                        )
                        for field in schema.fields
                    ],
                )
            )
            lines.append("")
    return lines


def _release_section() -> list[str]:
    lines = [
        "## Release datasets",
        "",
        "`atx-db publish-release` writes one Parquet file per dataset plus a single "
        "`manifest.json` pinning the exported schema hash, the public record-contract hash, "
        "the exact query text, the file bytes, and the diff against the previous release.",
        "",
    ]
    lines.extend(
        _table(
            ("dataset", "relation", "key columns", "public schema"),
            [
                (d.name, d.object_name, ", ".join(d.key_columns), d.schema_ref or "")
                for d in RELEASE_DATASETS
            ],
        )
    )
    return lines
```

Finish `C:\atx\atx-db\scripts\generate_data_dictionary.py`:

```python
def render_data_dictionary() -> str:
    """Render the full dictionary. Deterministic: no clock, no warehouse, sorted throughout."""

    lines: list[str] = [
        "# atx-db data dictionary",
        "",
        "Generated by `scripts/generate_data_dictionary.py`. **Do not edit by hand** -- run",
        "`python scripts/generate_data_dictionary.py` after changing any registry; CI fails",
        "when this file is stale.",
        "",
        "Sources of truth: `src/atx_db/seeds/fundamental_items.csv`,",
        "`src/atx_db/seeds/derived_metric_definitions.csv`, `src/atx_db/api/catalog.py`,",
        "`src/atx_db/universe_us_listed.py`, `src/atx_db/delisting_evidence.py`,",
        "`src/atx_db/publication.py`.",
        "",
    ]
    for section in (
        _item_section(),
        _derived_section(),
        _market_daily_section(),
        _universe_section(),
        _delisting_section(),
        _api_section(),
        _release_section(),
    ):
        lines.extend(section)
        lines.append("")
    while lines and lines[-1] == "":
        lines.pop()
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="generate-data-dictionary")
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit 1 and print a diff when the committed file is stale; write nothing.",
    )
    args = parser.parse_args(argv)

    rendered = render_data_dictionary()
    if args.check:
        current = (
            DATA_DICTIONARY_PATH.read_text(encoding="utf-8")
            if DATA_DICTIONARY_PATH.exists()
            else ""
        )
        if current == rendered:
            return 0
        print(f"{DATA_DICTIONARY_PATH.name} is stale; run scripts/generate_data_dictionary.py")
        sys.stdout.writelines(
            difflib.unified_diff(
                current.splitlines(keepends=True),
                rendered.splitlines(keepends=True),
                fromfile="committed",
                tofile="generated",
                n=2,
            )
        )
        return 1
    DATA_DICTIONARY_PATH.parent.mkdir(parents=True, exist_ok=True)
    DATA_DICTIONARY_PATH.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Generate the committed dictionary**

Run: `.venv\Scripts\python.exe scripts\generate_data_dictionary.py`
Expected: no output; `docs/DATA_DICTIONARY.md` appears.

Run: `.venv\Scripts\python.exe scripts\generate_data_dictionary.py --check`
Expected: no output, exit 0.

- [ ] **Step 5: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_data_dictionary.py -n 0 -q`
Expected: PASS — `10 passed`.

- [ ] **Step 6: Commit**

```
git add scripts/generate_data_dictionary.py docs/DATA_DICTIONARY.md tests/test_data_dictionary.py
git commit -m "docs(db): generated data dictionary with a CI staleness check

Adds scripts/generate_data_dictionary.py, a pure function of the committed registries
(item seed, derived-metric seed, public record schemas, universe and delisting
vocabularies, release datasets) that renders docs/DATA_DICTIONARY.md deterministically
and supports --check so CI fails on stale docs. It never opens a warehouse.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---
### Task 10: Retirement wave 2 — unschedule the superseded valuation surfaces, delete two dead modules

**Files:**
- Create: `src/atx_db/migrations/bodies_0309.py`
- Delete: `src/atx_db/abnormal_capex.py`, `src/atx_db/operating_leverage.py`, `tests/test_abnormal_capex.py`, `tests/test_operating_leverage.py`
- Modify: `src/atx_db/jobs.py`, `src/atx_db/migrations/registry.py`, `tests/data/public_api_snapshot.json`
- Test: `tests/test_retirement_wave2.py` (new)

**Interfaces:**
- Consumes: `atx_db.jobs.DATASET_REGISTRY`, `DATASET_DEPENDENCIES`, `_apply_dataset_dependencies`.
- Produces:
  - `atx_db.migrations.bodies_0309.DEPRECATED_TABLES: tuple[str, ...]` = `("enterprise_value", "market_cap", "valuation_multiples")`
  - `atx_db.migrations.bodies_0309.RETIRED_FACTOR_IDS: tuple[str, ...]` = `("investment_low_abnormal_capex", "risk_operating_leverage")`
  - `atx_db.migrations.bodies_0309.DEPRECATION_PREFIX: str` = `"[DEPRECATED 2026-09-19; superseded by market_daily_metrics] "`
  - `atx_db.migrations.bodies_0309.MIGRATIONS` — `Migration(version=309, name="retire_valuation_surfaces", up=_retire_valuation_surfaces)`.
  - `DATASET_REGISTRY` loses `market_cap`, `enterprise_value`, `valuation_multiples`; `DATASET_DEPENDENCIES` loses the same three keys (they must go together or `_apply_dataset_dependencies` raises).

- [ ] **Step 0: Reproduce the reverse-import scan before deleting anything**

Write the scan to a scratch file and run it rather than fighting shell quoting:

```python
# scratch/retirement_scan.py
import collections, pathlib, re

root = pathlib.Path(".")
mods = {p.stem for p in (root / "src/atx_db").glob("*.py") if p.stem != "__init__"}
s3_deleted = {
    "altman_distress", "beneish_m_score", "net_operating_assets", "rsst_accruals",
    "quarterly_working_capital_accruals", "asset_turnover_change", "annual_margin_change",
    "quarterly_gross_margin_change", "quarterly_profitability_change", "external_financing",
    "net_debt_financing", "net_issuance", "net_payout", "enterprise_yield", "rd_intensity",
    "rd_increase", "tax_expense_momentum", "tax_to_book_income",
}
files = [f for f in root.rglob("*.py") if ".venv" not in f.parts and "__pycache__" not in f.parts]
texts = {f.as_posix(): f.read_text(encoding="utf-8", errors="ignore") for f in files}
importers = collections.defaultdict(set)
for name in mods:
    patterns = [rf"from \.{name} import", rf"from atx_db\.{name} import", rf"import atx_db\.{name}\b"]
    for path, text in texts.items():
        if path.endswith(f"src/atx_db/{name}.py"):
            continue
        if any(re.search(p, text) for p in patterns):
            importers[name].add(path)

orphans = []
for name in sorted(mods - s3_deleted):
    own = texts.get(f"src/atx_db/{name}.py", "")
    if "fundamental_factor_values" not in own:
        continue
    if (root / "scripts" / f"build_{name}.py").exists():
        continue
    live = {p for p in importers[name] if p.startswith("src/")}
    live -= {f"src/atx_db/{d}.py" for d in s3_deleted}
    if not live:
        orphans.append(name)
print(orphans)
```

Run: `.venv\Scripts\python.exe scratch\retirement_scan.py`
Expected: `['abnormal_capex', 'operating_leverage']` — and nothing else. **If the scan prints anything else, stop and get a ruling**: the retirement set is defined by this scan, not by the plan text. Delete the scratch file afterwards; it is not part of the commit.

**What is retired and why:**

| module | operator entry point | module importers after Sprint 3 | action |
| --- | --- | --- | --- |
| `abnormal_capex` | none | none | **delete** |
| `operating_leverage` | none | none | **delete** |
| `cash_flow_profitability` | `scripts/build_cash_flow_profitability.py` | lost `altman_distress` | keep — still runnable |
| `fundamental_signals` | `scripts/build_fundamental_signals.py` | lost `altman_distress` | keep — still runnable |
| `quarterly_revenue_margin_confirmation` | `scripts/build_quarterly_revenue_margin_confirmation.py` | lost `quarterly_gross_margin_change` | keep — still runnable |

**What is deprecated but kept.** Per the Global Constraints, `market_cap`, `enterprise_value` and `valuation_multiples` keep their tables and every row. They lose their `DATASET_REGISTRY` entries so the orchestrator stops refreshing them, and their `table_catalog` / `dataset_catalog` descriptions gain the deprecation banner pointing at `market_daily_metrics`, which now carries `market_cap`, `enterprise_value` and the valuation ratios on one clock. `valuation_multiples.py` and `enterprise_value.py` stay importable; only their `jobs.py` imports, factories and registry entries go.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_retirement_wave2.py`:

```python
"""Tier1-S4 T10: retirement wave 2 -- unscheduled valuation surfaces, two dead modules gone."""

from __future__ import annotations

import importlib
import json
from pathlib import Path

import pytest


RETIRED_MODULES = ("abnormal_capex", "operating_leverage")
DEPRECATED_TABLES = ("enterprise_value", "market_cap", "valuation_multiples")
KEPT_MODULES = (
    "cash_flow_profitability",
    "fundamental_signals",
    "quarterly_revenue_margin_confirmation",
)


@pytest.mark.parametrize("module_name", RETIRED_MODULES)
def test_the_retired_modules_are_gone(module_name):
    with pytest.raises(ModuleNotFoundError):
        importlib.import_module(f"atx_db.{module_name}")


@pytest.mark.parametrize("module_name", RETIRED_MODULES)
def test_the_retired_test_files_are_gone(module_name):
    assert not (Path(__file__).resolve().parent / f"test_{module_name}.py").exists()


@pytest.mark.parametrize("module_name", KEPT_MODULES)
def test_the_script_backed_modules_are_kept(module_name):
    assert importlib.import_module(f"atx_db.{module_name}") is not None
    scripts = Path(__file__).resolve().parents[1] / "scripts"
    assert (scripts / f"build_{module_name}.py").exists()


@pytest.mark.parametrize("dataset_id", DEPRECATED_TABLES)
def test_the_deprecated_datasets_are_unscheduled(dataset_id):
    from atx_db.jobs import DATASET_DEPENDENCIES, DATASET_REGISTRY

    assert dataset_id not in DATASET_REGISTRY
    assert dataset_id not in DATASET_DEPENDENCIES


def test_no_surviving_dependency_points_at_a_removed_dataset():
    from atx_db.jobs import DATASET_DEPENDENCIES, DATASET_REGISTRY

    for dataset_id, dependencies in DATASET_DEPENDENCIES.items():
        for dependency in dependencies:
            assert dependency in DATASET_REGISTRY, f"{dataset_id} -> {dependency}"


@pytest.mark.parametrize("table_name", DEPRECATED_TABLES)
def test_the_deprecated_tables_still_exist(tmp_store, table_name):
    count = tmp_store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'main' AND table_name = ?",
        [table_name],
    ).fetchone()[0]
    assert int(count) == 1


@pytest.mark.parametrize("table_name", DEPRECATED_TABLES)
def test_the_deprecated_tables_are_marked_in_the_catalog(tmp_store, table_name):
    from atx_db.migrations.bodies_0309 import DEPRECATION_PREFIX

    description = tmp_store.con.execute(
        "SELECT description FROM table_catalog WHERE table_name = ?", [table_name]
    ).fetchone()[0]
    assert str(description).startswith(DEPRECATION_PREFIX)


def test_the_deprecation_is_idempotent(tmp_store):
    from atx_db.migrations.bodies_0309 import DEPRECATION_PREFIX

    description = tmp_store.con.execute(
        "SELECT description FROM table_catalog WHERE table_name = 'market_cap'"
    ).fetchone()[0]
    assert str(description).count(DEPRECATION_PREFIX) == 1


def test_the_retired_factor_definitions_are_closed(tmp_store):
    from atx_db.migrations.bodies_0309 import RETIRED_FACTOR_IDS

    placeholders = ", ".join("?" for _ in RETIRED_FACTOR_IDS)
    rows = tmp_store.con.execute(
        f"SELECT factor_id, valid_to FROM factor_definition WHERE factor_id IN ({placeholders}) "
        "ORDER BY factor_id",
        list(RETIRED_FACTOR_IDS),
    ).fetchall()
    assert [str(row[0]) for row in rows] == sorted(RETIRED_FACTOR_IDS)
    for _factor_id, valid_to in rows:
        assert valid_to is not None


def test_the_public_api_snapshot_no_longer_lists_the_retired_modules():
    snapshot = json.loads(
        (Path(__file__).resolve().parent / "data" / "public_api_snapshot.json").read_text()
    )
    for module_name in RETIRED_MODULES:
        assert module_name not in snapshot["atx_db"]
```

- [ ] **Step 2: Run and watch it fail**

Run: `.venv\Scripts\python.exe -m pytest tests/test_retirement_wave2.py -n 0 -q`
Expected: FAIL — `atx_db.abnormal_capex` still imports, the three datasets are still registered, and `atx_db.migrations.bodies_0309` does not exist.

- [ ] **Step 3: Create migration 0309**

Create `C:\atx\atx-db\src\atx_db\migrations\bodies_0309.py`:

```python
"""Retirement wave 2: mark the valuation surfaces Sprint 3 superseded, close two factors."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


DEPRECATED_TABLES: tuple[str, ...] = ("enterprise_value", "market_cap", "valuation_multiples")
RETIRED_FACTOR_IDS: tuple[str, ...] = (
    "investment_low_abnormal_capex",
    "risk_operating_leverage",
)
DEPRECATION_PREFIX = "[DEPRECATED 2026-09-19; superseded by market_daily_metrics] "
RETIREMENT_DATE = "2026-09-19"


def _retire_valuation_surfaces(conn: duckdb.DuckDBPyConnection) -> None:
    # Tables and rows are kept -- only the catalog prose and the scheduling change.
    # The prefix guard makes a re-run a no-op instead of stacking banners.
    for table_name in DEPRECATED_TABLES:
        conn.execute(
            """
            UPDATE table_catalog
            SET description = ? || coalesce(description, ''), updated_at = now()
            WHERE table_name = ?
              AND coalesce(description, '') NOT LIKE ?
            """,
            [DEPRECATION_PREFIX, table_name, DEPRECATION_PREFIX + "%"],
        )
        conn.execute(
            """
            UPDATE dataset_catalog
            SET description = ? || coalesce(description, ''),
                metadata_json = '{"deprecated":true,"superseded_by":"market_daily_metrics"}',
                updated_at = now()
            WHERE dataset_id = ?
              AND coalesce(description, '') NOT LIKE ?
            """,
            [DEPRECATION_PREFIX, table_name, DEPRECATION_PREFIX + "%"],
        )
    placeholders = ", ".join("?" for _ in RETIRED_FACTOR_IDS)
    conn.execute(
        f"""
        UPDATE factor_definition
        SET valid_to = DATE '{RETIREMENT_DATE}',
            declared_in = 'retired',
            updated_at = now()
        WHERE factor_id IN ({placeholders})
          AND valid_to IS NULL
        """,
        list(RETIRED_FACTOR_IDS),
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=309, name="retire_valuation_surfaces", up=_retire_valuation_surfaces)
]
```

Register it in `registry.py` (`from .bodies_0309 import MIGRATIONS as _MIGRATIONS_0309`, then `*_MIGRATIONS_0309,`).

> If `dataset_catalog` has no row for one of the three names, the `UPDATE` is a harmless
> no-op; confirm with
> `.venv\Scripts\python.exe -c "from atx_db.connection import DuckDBStore; s=DuckDBStore('build/probe2.duckdb'); s.initialize(); print(s.con.execute(\"SELECT dataset_id FROM dataset_catalog WHERE dataset_id IN ('market_cap','enterprise_value','valuation_multiples')\").fetchall())"`
> and, if a row is missing, insert it in the same migration rather than leaving the catalog silent.

- [ ] **Step 4: Unschedule the three datasets**

In `C:\atx\atx-db\src\atx_db\jobs.py`:

1. Delete the three `DATASET_REGISTRY` entries:
   ```python
       MarketCapDataset.dataset_id: (MarketCapDataset, _market_cap_options),
       EnterpriseValueDataset.dataset_id: (EnterpriseValueDataset, _enterprise_value_options),
       ValuationMultiplesDataset.dataset_id: (ValuationMultiplesDataset, _valuation_multiples_options),
   ```
2. Delete the three `DATASET_DEPENDENCIES` keys `"market_cap"`, `"enterprise_value"`, `"valuation_multiples"` (they must go in the same edit — `_apply_dataset_dependencies()` raises on a dependency key with no registry entry).
3. Delete the now-unused option factories `_market_cap_options`, `_enterprise_value_options`, `_valuation_multiples_options` and the imports `from .enterprise_value import EnterpriseValueDataset, EnterpriseValueOptions` and the `MarketCapDataset` / `MarketCapOptions` / `ValuationMultiplesDataset` / `ValuationMultiplesOptions` names from the `valuation_multiples` import.

The modules themselves stay: `scripts/build_*.py` operators and any analyst code importing `refresh_market_cap` keep working; only the scheduled DAG node is gone.

- [ ] **Step 5: Delete the two dead modules**

```
git rm src/atx_db/abnormal_capex.py src/atx_db/operating_leverage.py tests/test_abnormal_capex.py tests/test_operating_leverage.py
```

- [ ] **Step 6: Update the public API snapshot**

Run: `.venv\Scripts\python.exe -c "import json,pathlib; p=pathlib.Path('tests/data/public_api_snapshot.json'); d=json.loads(p.read_text()); d['atx_db']=sorted(set(d['atx_db'])-{'abnormal_capex','operating_leverage'}); p.write_text(json.dumps(d, indent=2, sort_keys=True)+chr(10))"`
Expected: no output; two removed lines in the diff.

- [ ] **Step 7: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_retirement_wave2.py tests/test_module_boundaries.py tests/test_import.py -n 0 -q`
Expected: PASS — `14 passed` in the new file plus the boundary and import suites green.

- [ ] **Step 8: Run the job-graph and migration suites**

Run: `.venv\Scripts\python.exe -m pytest tests/test_jobs.py tests/test_orchestrator.py tests/test_migration_governance.py -n 0 -q --run-slow`
Expected: PASS. Any test asserting a fixed `len(DATASET_REGISTRY)` must drop by 3 (and rise by 4 from Tasks 2, 3 and 5) — settle the number by running the suite and pinning what it reports.

- [ ] **Step 9: Commit**

```
git add -A src/atx_db/jobs.py src/atx_db/migrations/bodies_0309.py src/atx_db/migrations/registry.py tests/test_retirement_wave2.py tests/data/public_api_snapshot.json tests/test_jobs.py
git commit -m "refactor(db): retirement wave 2 -- unschedule superseded valuation surfaces

market_cap, enterprise_value and valuation_multiples lose their DATASET_REGISTRY and
DATASET_DEPENDENCIES entries and gain a catalog deprecation banner pointing at
market_daily_metrics, which now carries the same quantities on one clock. Tables and
rows are kept. Deletes abnormal_capex and operating_leverage, the only two factor
modules left with neither an operator entry point nor a module importer after Sprint 3,
and closes their factor_definition rows with valid_to.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---
### Task 11: Documentation truth pass and the CI staleness gate

**Files:**
- Modify: `README.md`, `docs/PRODUCTION_RUNBOOK.md`
- Modify: `docs/PARITY_GAP.md`, `docs/WAREHOUSE_PARITY_NEXT_AGENT_README.md`, `docs/WAREHOUSE_PARITY_TRANCHES.md`, `docs/ROADMAP_PARITY.md`
- Modify: `.github/workflows/atx-db.yml`
- Test: `tests/test_docs_banners.py` (new)

**Interfaces:**
- Consumes: `scripts/generate_data_dictionary.py` (Task 9).
- Produces:
  - `HISTORICAL_BANNER` — a fixed four-line block at the top of each of the four superseded parity documents, naming `docs/FUNDAMENTALS_PROVIDER_DESIGN.md` and `docs/superpowers/specs/2026-09-19-tier1-parity-design.md` as current.
  - A `Data dictionary` CI step running `python scripts/generate_data_dictionary.py --check`.
  - `tests/test_docs_banners.py` pinning the banner so a future edit cannot silently un-deprecate the four documents.

**Why banners and not deletions.** `PARITY_GAP.md` (117 KB) and `WAREHOUSE_PARITY_TRANCHES.md` (180 KB) are the only record of how the warehouse got here, and the tranche ledger is append-only by design. They are wrong only about *currency*, not about history. A banner fixes the currency problem without destroying the record; audit §9 asks for exactly this ("the design doc should be declared the single source of measured truth and the others marked historical").

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_docs_banners.py`:

```python
"""Tier1-S4 T11: superseded parity documents are marked historical, current ones are named."""

from __future__ import annotations

from pathlib import Path

import pytest


DOCS = Path(__file__).resolve().parents[1] / "docs"
HISTORICAL = (
    "PARITY_GAP.md",
    "ROADMAP_PARITY.md",
    "WAREHOUSE_PARITY_NEXT_AGENT_README.md",
    "WAREHOUSE_PARITY_TRANCHES.md",
)
BANNER_MARKER = "> **HISTORICAL — superseded.**"
CURRENT_DESIGN = "docs/FUNDAMENTALS_PROVIDER_DESIGN.md"
CURRENT_SPEC = "docs/superpowers/specs/2026-09-19-tier1-parity-design.md"


@pytest.mark.parametrize("name", HISTORICAL)
def test_each_superseded_document_carries_the_banner(name):
    text = (DOCS / name).read_text(encoding="utf-8")
    head = "\n".join(text.splitlines()[:8])
    assert BANNER_MARKER in head
    assert CURRENT_DESIGN in head
    assert CURRENT_SPEC in head


@pytest.mark.parametrize("name", HISTORICAL)
def test_the_banner_sits_above_the_original_title(name):
    lines = (DOCS / name).read_text(encoding="utf-8").splitlines()
    banner_index = next(i for i, line in enumerate(lines) if BANNER_MARKER in line)
    title_index = next(i for i, line in enumerate(lines) if line.startswith("# "))
    assert banner_index < title_index


def test_the_current_documents_are_not_banned():
    for name in ("FUNDAMENTALS_PROVIDER_DESIGN.md", "DATA_DICTIONARY.md", "PRODUCTION_RUNBOOK.md"):
        text = (DOCS / name).read_text(encoding="utf-8")
        assert BANNER_MARKER not in text


def test_the_runbook_documents_the_new_surfaces():
    text = (DOCS / "PRODUCTION_RUNBOOK.md").read_text(encoding="utf-8")
    for token in (
        "atx-db publish-release",
        "universe_us_listed_membership",
        "delisting_evidence",
        "generate_data_dictionary.py",
        "warehouse_template.duckdb",
    ):
        assert token in text


def test_the_readme_points_at_the_data_dictionary():
    text = (DOCS.parent / "README.md").read_text(encoding="utf-8")
    assert "docs/DATA_DICTIONARY.md" in text
    assert "docs/ITEM_COVERAGE.md" in text


def test_the_ci_workflow_checks_the_data_dictionary():
    workflow = (
        DOCS.parents[1] / ".github" / "workflows" / "atx-db.yml"
    ).read_text(encoding="utf-8")
    assert "scripts/generate_data_dictionary.py --check" in workflow
    assert 'pytest -q -n 4 -m "not slow"' in workflow
```

- [ ] **Step 2: Run and watch it fail**

Run: `.venv\Scripts\python.exe -m pytest tests/test_docs_banners.py -n 0 -q`
Expected: FAIL — no banner in any of the four documents, and the CI workflow has no data-dictionary step.

- [ ] **Step 3: Add the banner**

Insert this block as the **first four lines** of each of `docs/PARITY_GAP.md`, `docs/ROADMAP_PARITY.md`, `docs/WAREHOUSE_PARITY_NEXT_AGENT_README.md` and `docs/WAREHOUSE_PARITY_TRANCHES.md`, above the existing `# ` title, followed by one blank line:

```markdown
> **HISTORICAL — superseded.** This document describes the pre-restructure `atx-impl/db`
> layout and carries row counts that later measurement has replaced. It is kept as a record
> of how the warehouse was built, not as a statement of what it contains. Current measured
> truth: `docs/FUNDAMENTALS_PROVIDER_DESIGN.md`. Current design contract:
> `docs/superpowers/specs/2026-09-19-tier1-parity-design.md`. Current field-level reference:
> `docs/DATA_DICTIONARY.md`.
```

Leave every other line of those four files untouched — `WAREHOUSE_PARITY_TRANCHES.md` is an append-only ledger and rewriting it would destroy the record.

- [ ] **Step 4: Extend the runbook**

Append to `C:\atx\atx-db\docs\PRODUCTION_RUNBOOK.md`, after the "Activation from scratch" section Sprint 1 added:

````markdown
## Universe, delistings and identifiers

The activation ladder builds these in order after the market panel:

```powershell
atx-db activate --db-path $env:ATX_DB_PATH --only identifiers_lei,identifiers_figi `
  --gleif-level1-file data/vendor/gleif_level1.csv `
  --openfigi-mapping-file data/vendor/openfigi_mapping.json
atx-db activate --db-path $env:ATX_DB_PATH --only delisting_evidence,universe_us_listed
```

- `delisting_evidence` materialises SEC Form 25/25-NSE, Nasdaq Trader deletes, SEC Form 15
  and archive last-trade inference, then folds them into `delisting_events` by precedence.
  The archive rule fires only when the last bar is more than 30 trading sessions before the
  archive's last session, so extending the archive never manufactures delistings.
- `universe_us_listed` rebuilds `universe_us_listed_membership`. Members with no resolved
  CIK are kept with `has_cik = false`; use
  `universe_us_listed(store, as_of, require_cik=True)` for the fundamentals universe and the
  default for the market universe.
- The GLEIF and OpenFIGI stages are no-ops when no vendor file is staged, so the ladder
  completes without them.

### Terminal returns

`delisting_terminal_returns` stays empty until either a licensed DLRET file is injected
(`DelistingReturnObservationDataset`) or the operator opts into the documented Shumway
convention by setting `DelistingTerminalReturnOptions.performance_delisting_return`
(`-0.30` NYSE/AMEX, `-0.55` Nasdaq). The `delisting_events_without_terminal_return` check is
an **error**-severity gate precisely so an empty table is visible rather than silently green.

## Publishing a release

```powershell
atx-db publish-release --db-path $env:ATX_DB_PATH --release-id 2026-09-19 `
  --out-dir data/releases --previous-dir data/releases/2026-09-12
```

Writes six Parquet files plus `manifest.json` under `data/releases/2026-09-19/`, records the
release in `publication_releases` / `publication_release_datasets`, and reports
added/removed/changed rows per dataset against the previous release. Publish only after
`run_warehouse_quality_checks` reports no `critical` failure.

## Docs and housekeeping

- `python scripts/generate_data_dictionary.py` regenerates `docs/DATA_DICTIONARY.md`. CI runs
  it with `--check` and fails on a stale file, so run it in any commit that touches a
  registry, a public schema, or the universe/delisting vocabularies.
- `warehouse_template.duckdb` at the repository root is an orphan from an older test harness:
  untracked, gitignored by `*.duckdb`, and referenced by nothing (`tests/conftest.py` builds
  its template under `.pytest_cache/db_schema_templates/`). Delete it locally to reclaim
  ~50 MB:

  ```powershell
  Remove-Item C:\atx\atx-db\warehouse_template.duckdb
  ```

  There is nothing to commit; the file was never tracked.
````

- [ ] **Step 5: Extend the README**

In `C:\atx\atx-db\README.md`, add a "Reference documentation" subsection under "Development":

```markdown
### Reference documentation

| Document | What it is |
| --- | --- |
| `docs/DATA_DICTIONARY.md` | Generated field-level reference for items, derived metrics, the daily market panel, the universe, delistings, the public API schemas and the release datasets. Regenerate with `python scripts/generate_data_dictionary.py`. |
| `docs/ITEM_COVERAGE.md` | Generated per-item, per-fiscal-year standardized coverage against the published target. |
| `docs/FUNDAMENTALS_PROVIDER_DESIGN.md` | The single source of measured live-warehouse numbers. |
| `docs/superpowers/specs/2026-09-19-tier1-parity-design.md` | The Tier-1 parity design contract. |
| `docs/PRODUCTION_RUNBOOK.md` | Activation, universe, delistings, identifiers, publication, housekeeping. |
| `docs/PARITY_GAP.md`, `docs/ROADMAP_PARITY.md`, `docs/WAREHOUSE_PARITY_*.md` | **Historical.** Kept as a build record; superseded by the two documents above. |
```

- [ ] **Step 6: Extend the CI workflow**

In `C:\atx\.github\workflows\atx-db.yml`, inside the `quality` job (the one Sprint 1 renamed), add a step immediately before the "Full non-slow test suite" step:

```yaml
      - name: Data dictionary is current
        run: python scripts/generate_data_dictionary.py --check
```

Extend the existing ruff step with this sprint's new files:

```yaml
          src/atx_db/universe_us_listed.py
          src/atx_db/delisting_evidence.py
          src/atx_db/publication.py
          src/atx_db/quality/checks_identities.py
          scripts/generate_data_dictionary.py
          scripts/build_universe_us_listed.py
          scripts/build_delisting_evidence.py
          scripts/publish_release.py
          tests/test_universe_us_listed.py
          tests/test_delisting_evidence.py
          tests/test_delisting_terminal_policy.py
          tests/test_identifier_activation.py
          tests/test_quality_identities.py
          tests/test_provider_coverage_slos.py
          tests/test_publication.py
          tests/test_data_dictionary.py
          tests/test_retirement_wave2.py
          tests/test_docs_banners.py
```

and the mypy step with the four `src/` modules plus `scripts/generate_data_dictionary.py` only (the test files go to ruff alone).

- [ ] **Step 7: Validate the workflow YAML**

Run: `.venv\Scripts\python.exe -c "import yaml,pathlib; d=yaml.safe_load(pathlib.Path('../.github/workflows/atx-db.yml').read_text()); print(sorted(d['jobs'])); print([s.get('name') for s in d['jobs']['quality']['steps']])"`
Expected: `['quality']`, then a step list containing `Data dictionary is current` immediately before `Full non-slow test suite`.

- [ ] **Step 8: Run the documentation tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_docs_banners.py tests/test_data_dictionary.py -n 0 -q`
Expected: PASS — `19 passed`.

- [ ] **Step 9: Run the full non-slow suite exactly as CI will**

Run: `.venv\Scripts\python.exe scripts\generate_data_dictionary.py --check`
Expected: no output, exit 0.

Run: `.venv\Scripts\python.exe -m pytest -q -n 4 -m "not slow" --tb=short tests`
Expected: PASS — the whole fast lane green, roughly 3 minutes with a warm schema template. Any failure here is a defect this sprint introduced; fix it before committing rather than deferring.

- [ ] **Step 10: Delete the orphan template (local only)**

Run: `.venv\Scripts\python.exe -c "import pathlib; p=pathlib.Path('warehouse_template.duckdb'); print(p.exists(), p.stat().st_size if p.exists() else 0)"`
Expected: `True` and a size near `52746240`.

Run: `git check-ignore -v warehouse_template.duckdb`
Expected: a line naming `.gitignore` and the `*.duckdb` rule — confirming the file is ignored and untracked.

Run: `Remove-Item C:\atx\atx-db\warehouse_template.duckdb`
Expected: no output, and `git status --porcelain` is unchanged. Nothing is committed; the runbook note is the durable artifact.

- [ ] **Step 11: Commit**

```
git add README.md docs/PRODUCTION_RUNBOOK.md docs/PARITY_GAP.md docs/ROADMAP_PARITY.md docs/WAREHOUSE_PARITY_NEXT_AGENT_README.md docs/WAREHOUSE_PARITY_TRANCHES.md ../.github/workflows/atx-db.yml tests/test_docs_banners.py
git commit -m "docs(db): mark the superseded parity docs historical and gate the data dictionary in CI

Adds a historical banner to PARITY_GAP, ROADMAP_PARITY and the two WAREHOUSE_PARITY
documents naming FUNDAMENTALS_PROVIDER_DESIGN.md and the Tier-1 spec as current,
documents the universe, delisting, identifier and publication surfaces plus the orphan
warehouse_template.duckdb cleanup in the runbook, points the README at the two generated
references, and makes CI run the data-dictionary generator in --check mode alongside the
full non-slow suite so stale docs fail the build.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Sprint completion checklist

- [ ] Migrations `0304`–`0309` are registered in `registry.py`, ascending, no gaps or duplicates (`tests/test_migration_governance.py`).
- [ ] `STAGE_ORDER` reads: `migrate, security_master, symbol_directory, ticker_history_extract, ticker_history_publish, sec_bulk_download, submissions_load, companyfacts_load, identifiers_lei, identifiers_figi, statement_points, periods, ttm, calendarization, standardized, industry_templates, reconciliation, derived_metrics, market_daily, delisting_evidence, universe_us_listed, provider_coverage`.
- [ ] `refresh_provider_coverage` runs clean and returns one snapshot per public schema, with no `RuntimeError`.
- [ ] `run_warehouse_quality_checks` emits the six new identity/coverage checks and the new survivorship coverage gate.
- [ ] `atx-db publish-release` produces a manifest whose `manifest_sha256` is stable across two runs over an unchanged warehouse.
- [ ] `python scripts/generate_data_dictionary.py --check` exits 0.
- [ ] `python -m pytest -q -n 4 -m "not slow" tests` is green.
- [ ] `docs/PARITY_GAP.md`, `docs/ROADMAP_PARITY.md` and both `WAREHOUSE_PARITY_*.md` carry the historical banner; `docs/FUNDAMENTALS_PROVIDER_DESIGN.md` and the Tier-1 spec do not.
