# Sprint PF2-S10 — Quality-as-SLO gating + observability + storage + reproducible rebuild

**Goal:** promote the ~288 warehouse quality checks from *recorded rows* to *enforced SLO gates* wired into the pf1 orchestrator (severity taxonomy, thresholds-as-data, halt-on-critical); add data observability (per-dataset freshness SLA + row-count anomaly over `data_quality_checks` history); add storage management (VACUUM/CHECKPOINT/compaction + DB-size monitoring) and a partitioned/incremental lake; ship a deterministic whole-warehouse rebuild + DR runbook. This is the **production capstone** — it closes with the parity-ledger flip and a whole-branch review. Reserved migrations 0128–0131.

**Mandate / Owns:** a gating extension in `db/quality.py` (severity + thresholds-as-data + a gate evaluator), NEW `db/observability.py` (freshness SLA + anomaly), NEW `db/storage_admin.py` (VACUUM/CHECKPOINT/compaction/size), a partitioning + incremental extension in `db/lake.py`, a gate-wiring change in `db/orchestrator.py`, NEW `db/tests/test_quality_gating.py` + `db/tests/test_observability.py`.

**Must NOT touch:** the ~288 existing `SqlQualityCheck` bodies (their `sql`/`threshold` stay byte-identical — severity is *added* metadata, never a rewrite of what a check computes); PF2-S1's schema-contract/drift surface (`db/schema_contract.py` — schema-drift is clause (E), not duplicated here); PF2-S2's `db/migration_admin.py` backup/`.bak` retention (coordinate, do not re-implement); any content sprint's `compute_*`/`run()` bodies. This sprint gates and monitors surfaces; it never changes what they compute.

**Depends on:** PF2-S1 (`table_catalog`/`field_catalog` contract + catalog reader — every new table catalogues through it), PF2-S2 (backup/CHECKPOINT primitive this sprint's storage compaction coordinates with), **all content sprints S3–S9** (the surfaces gated/monitored must exist), and **pf1-S2** (`db/orchestrator.py` — `DatasetOrchestrator.run/.resume`, `etl_job_runs`/`etl_job_steps`/`etl_job_audit`, `RetryPolicy` — the engine gates wire into). LAST sprint in pf2.

---

## Baseline / where the cycles go

Quality checks are written but never consumed; observability and storage management do not exist. All measured this session against the live warehouse.

1. **Quality is UNGATED — a failed check is just a row.** `run_warehouse_quality_checks(store, *, daily_macro_stale_days=10, monthly_macro_stale_days=70, record=True)` (`quality.py:6020`) iterates the ~288 `SqlQualityCheck` specs from the one giant `_check_specs(*, daily_macro_stale_days, monthly_macro_stale_days)` factory (`quality.py:101`, running to line 6017) and appends each outcome via `quality_check(...)` → `INSERT INTO data_quality_checks (check_id, dataset_id, table_name, check_name, status, observed_value, threshold_value, details_json, checked_at)` (`warehouse.py:161`). It is **append-only**, `check_id` is a fresh `uuid4()`, and there is **no severity column**. Grep confirms `run_warehouse_quality_checks` is called **only from tests + the `db/__init__.py` export — zero calls from `orchestrator.py` or `jobs.py`**. A `failed` check halts nothing; the orchestrator walks its DAG unaware a check ever ran.

2. **Only two failure levels; thresholds are hard-coded.** `SqlQualityCheck` (`quality.py:43`, frozen) carries `failure_status: FailureStatus` where `FailureStatus = Literal["failed", "warning"]` and `comparator: Comparator = Literal["eq","le","ge"]`, with the `threshold: float` baked into each of the ~288 literals inline. There is no `critical` tier that could *halt* a run and no thresholds-as-data surface — retuning a bound means editing Python and redeploying.

3. **History is written but NEVER read.** `data_quality_checks` accumulates one row per check per run forever, but nothing trends it: no baseline, no row-count anomaly detection, no "this check regressed vs its 30-run median." The append-only ledger is pure write-side.

4. **Freshness = two macro-staleness checks only.** The sole freshness signals are `stale_daily_macro_observations` and `stale_monthly_macro_observations` (`quality.py:3463`/`3480`, thresholds `daily_macro_stale_days`/`monthly_macro_stale_days`, defaults 10/70 days over `macro_observations`). There is **no general per-dataset freshness SLA.** `dataset_watermarks` is populated by `refresh_warehouse_watermarks` (`watermarks.py:766`, over `WATERMARK_QUERIES`) but **nothing evaluates a mark against an SLA** — the watermark is recorded, never asserted fresh.

5. **No storage management anywhere.** `connection.py` (`DuckDBStore` / `connect(path, *, read_only)`) issues **no CHECKPOINT, no VACUUM, no compaction** (grep: zero matches). The single-file DB has grown to ~14 GB with no compaction path; multi-GB `.bak` artifacts proliferate (PF2-S2 owns `.bak` retention). Nothing tracks DB size over time.

6. **The lake is a full snapshot with no partitioning.** `LakehouseExporter.export_objects` (`lake.py:202`) walks `DEFAULT_EXPORT_OBJECTS` and for each runs `COPY (SELECT * FROM <obj>) TO part-00000.parquet (FORMAT PARQUET, COMPRESSION ZSTD)`, writing a single `part-00000.parquet` + `_manifest.json` whose `"partition_columns": []` (`lake.py:267`). Every run is a **full re-snapshot** — no date/entity partitioning, no incremental/CDC, no retention of prior runs.

7. **No reproducible whole-warehouse rebuild.** The orchestrator has `run`/`resume` over a DAG with `etl_job_runs`/`etl_job_steps`/`etl_job_audit` + `RetryPolicy`, but there is **no generic date-range replay** and **no single deterministic "rebuild the warehouse from source"** command with a DR runbook a fresh operator can follow.

**Already good — do not regress:**
- **The ~288 checks and their `data_quality_checks` audit trail.** Every `SqlQualityCheck.sql`/`threshold`/`comparator` and the append-only insert path stay byte-identical. Severity + thresholds-as-data are *additive metadata* over the existing specs; a passing check keeps meaning exactly what it meant. `INTERNAL_ONLY_EXPORT_FORBIDDEN_COLUMN` / `_export_scan_internal_cusip_sql` boundary check is untouched.
- **The orchestrator contract.** `DatasetOrchestrator.run/.resume`, the `etl_job_steps` resume cursor, `create_run_manifest`/`record_audit`, and `RetryPolicy` semantics stay as pf1-S2 shipped them. The gate is a new *hook* between step-run and step-commit, not a rewrite of `_execute`.
- **The lake manifest/audit.** `lake_export_runs`/`lake_export_files`, `LakeExportResult`, `_schema_sha256`, the per-object `_manifest.json` + `sha256`/`schema_sha256` lineage all stay; partitioning is additive (`partition_columns` becomes non-empty where declared).

---

## PIT / determinism + production contract

ROADMAP clauses **(B)** append-only catalogued migrations and **(C)** offline/no-network tests apply in full; **(F)** backup-before-migrate governs the storage/CHECKPOINT work; **(G) quality-gated is DEFINED by this sprint.**

- **(B)** Migrations **0128–0131** only, forward-only/idempotent. `0128` = `data_quality_checks.severity` (`ADD COLUMN IF NOT EXISTS`) + `quality_check_registry` (thresholds/severity-as-data); `0129` = `dataset_freshness_sla` + `data_quality_anomaly` (observability); `0130` = `warehouse_storage_stats` + lake partition-spec catalog rows; `0131` = indexes + `warehouse_rebuild_runs` (reproducible-rebuild log) + reserved, split schema-vs-index per the S5g/S5k WAL precedent. Every new table/column seeds `table_catalog` + `field_catalog` in the same migration (through PF2-S1's contract). Never edit a landed migration; never renumber.
- **(C)** Every test runs against in-memory / template-copy DuckDB with fixture rows: a fixture warehouse with a planted critical failure (S10-0), a stale watermark + a row-count outlier (S10-1), a synthetic size/partition slice (S10-2). No SEC/FRED/vendor network; the storage CHECKPOINT/VACUUM smoke and the live rebuild are operator-run and recorded in the ledger.
- **(F)** Any storage compaction / CHECKPOINT reuses PF2-S2's backup pre-flight — a compaction never runs without a timestamped DB+WAL backup first, per the standing WAL-split discipline.
- **(G) Quality-gated *(defined here).*** A check authored `severity=critical` is wired into the orchestrator and **halts the affected run**: quality results are *consumed*, not merely recorded. `error` degrades the run to `partial`; `warning` records only. Gating is deterministic — same inputs + same registry → same halt/degrade decision and the same `etl_job_audit` trail.

---

## Tasks

### S10-0 — Quality-as-SLO gating wired into the orchestrator *(the big one)*

**Root cause:** `run_warehouse_quality_checks` (`quality.py:6020`) writes `data_quality_checks` rows and returns; **no `orchestrator.py`/`jobs.py` path calls it**, so a `failed` check never stops a pipeline. `SqlQualityCheck.failure_status` is only `{failed, warning}` (`quality.py:40,43`) with thresholds hard-coded inline — there is no `critical` tier and no data-driven bound to gate on.

**Fix:** (1) Add a **severity taxonomy as data** — migration **0128** adds `data_quality_checks.severity` (`ADD COLUMN IF NOT EXISTS`, values `critical`/`error`/`warning`, catalogued) and a `quality_check_registry` table (`check_name` PK, `dataset_id`, `severity`, `threshold_value`, `comparator`, `enabled`) that carries **thresholds-as-data**; seed it from the ~288 existing specs (default `severity` derived from today's `failure_status`: `failed`→`error`, `warning`→`warning`; a curated load-bearing subset promoted to `critical`). `SqlQualityCheck` gains an optional `severity` that the registry overrides — the ~288 `sql` bodies are untouched. (2) Add a **gate evaluator** in `quality.py` (e.g. `evaluate_quality_gate(store, dataset_id) -> GateResult`) that runs the specs for one dataset, records them (unchanged path) with severity, and returns the worst severity among failures. (3) **Wire it into `orchestrator.py`**: after a step's `run()` succeeds, call the gate for that `dataset_id` before committing the step — a `critical` failure raises `OrchestratorRunError` (step → `failed`, run halts, `record_audit(action="step_quality_gate_halt")`); an `error` marks the run `partial`; `warning` records only. Gating is opt-in per run (`gate=True` param on `DatasetOrchestrator.run`, default preserving today's behavior for the content sprints' tests) so it is adopted incrementally per clause (G).

**PIT:** (B) 0128 catalogued, `ADD COLUMN IF NOT EXISTS`. (G) same registry + same facts → same halt decision; the gate reads only already-loaded rows (no lookahead).

**Accept-with-fixture:** a fixture warehouse with one planted `critical` check failure, driven through `DatasetOrchestrator.run(gate=True)`, **halts** at that dataset with `etl_job_runs.status='failed'`, a `step_quality_gate_halt` audit row, and the downstream closure left `pending`; the same warehouse with the failure planted as `error` completes with run status `partial`; with `warning` completes `succeeded`; `gate=False` reproduces today's ungated walk. Severity/threshold changes are made in `quality_check_registry`, not Python.

### S10-1 — Observability: freshness SLA + row-count anomaly

**Root cause:** freshness = only the two macro-staleness checks (`quality.py:3463`/`3480`); `dataset_watermarks` (`watermarks.py:766`) is written but never asserted against an SLA; `data_quality_checks` history is never trended (no anomaly detection).

**Fix:** NEW `db/observability.py`. (1) **Freshness SLA** — migration **0129** `dataset_freshness_sla` (`dataset_id` PK, `max_lag_days`, `severity`, `enabled`); `evaluate_freshness_slas(store, as_of)` joins each row to its `dataset_watermarks` mark and emits a `data_quality_checks` row (via the existing `quality_check` path, with severity) when `as_of - watermark > max_lag_days` — a general per-dataset SLA generalizing the two macro checks. (2) **Row-count anomaly** — `data_quality_anomaly` table (`dataset_id`, `check_name`, `baseline_median`, `baseline_mad`, `observed_value`, `z_score`, `is_anomaly`, `checked_at`); `detect_rowcount_anomalies(store, *, window)` builds a baseline (median/MAD) from the prior `window` `observed_value`s in `data_quality_checks` for count-style checks and flags an outlier — finally *reading* the history clause 3 identified as write-only. Both emit gated checks so a freshness or anomaly breach can carry `severity=critical` and halt via S10-0.

**PIT:** (B) 0129 catalogued. (C) fixtures: a stale watermark past its `max_lag_days`, and a count series with one planted outlier. (D) baseline is a pure function of the recorded history → deterministic z-score.

**Accept-with-fixture:** a fixture with a watermark older than its SLA emits a freshness `data_quality_checks` row of the configured severity; a fresh one emits nothing; a count history `[100,101,99,100,140]` flags the `140` as anomalous with a recorded `z_score` while `[100,101,99,100,101]` flags nothing; both surfaces route through the S10-0 gate.

### S10-2 — Storage admin (VACUUM/CHECKPOINT/compaction + size monitoring) + partitioned/incremental lake

**Root cause:** `connection.py` issues no CHECKPOINT/VACUUM/compaction (grep: zero matches); the ~14 GB file has no compaction path and nothing tracks its size. `lake.py` writes a single `part-00000.parquet` with `"partition_columns": []` (`lake.py:242,267`) — a full snapshot per run, no partitioning/incremental/retention.

**Fix:** NEW `db/storage_admin.py` — `checkpoint_and_compact(store)` (issues `CHECKPOINT`, coordinated with PF2-S2's backup pre-flight per clause (F)); `record_storage_stats(store)` → migration **0130** `warehouse_storage_stats` (`checked_at`, `db_size_bytes`, `wal_size_bytes`, per-table `row_count`/`byte_count` from `duckdb_tables()`), the size signal that can itself be a gated growth check. Extend `lake.py`: a per-object `partition_columns` spec (date/entity, catalogued in 0130) so `export_objects` writes `COPY ... TO <dir> (FORMAT PARQUET, PARTITION_BY (...))` with a populated `_manifest.json.partition_columns`, plus **incremental** export (only partitions whose watermark advanced) and **prior-run retention** (keep N prior `export_run_id` trees rather than overwriting `part-00000.parquet`). Fully backward-compatible: an object with no declared partition spec keeps today's single-file full-snapshot path.

**PIT:** (B) 0130 catalogued, split schema/index into 0131. (F) CHECKPOINT/compaction only behind PF2-S2 backup. (C) fixture DB + tiny partitioned slice — no multi-GB data in pytest.

**Accept-with-fixture:** `record_storage_stats` writes a `warehouse_storage_stats` row with a non-zero `db_size_bytes`; `checkpoint_and_compact` runs clean on a fixture DB; a partition-declared object exports to multiple partition dirs with `partition_columns` populated in its manifest and reconciling row counts; an unchanged partition is skipped on the second incremental export while a moved watermark re-exports exactly its partition; an undeclared object still writes the single `part-00000.parquet`.

### S10-3 — Reproducible full-rebuild + DR runbook + parity-ledger flip + whole-branch review

**Root cause:** the orchestrator resumes a run but there is no generic date-range replay and no one-command deterministic "rebuild the warehouse from source" with a DR runbook; pf2's parity work is not yet reflected in `provider_parity_matrix`.

**Fix:** (1) A deterministic **full-rebuild** entry point (a `scripts/warehouse_rebuild.py` driving `DatasetOrchestrator.run(full_rebuild=True, gate=True)` over the whole DAG with an optional `--since/--until` date-range replay), logging to migration **0131** `warehouse_rebuild_runs` (`rebuild_run_id`, `git_sha`, `since`, `until`, `status`, per the orchestrator manifest) so a rebuild is auditable and reproducible (same source + same `git_sha` → same rows). (2) A **DR runbook** documenting the recover-from-`.bak` → CHECKPOINT → migrate → rebuild → gate → verify sequence (referencing PF2-S2's backup/restore and this sprint's gates). (3) **Parity-ledger flip** — update `db/parity.py` `PROVIDER_PARITY_ROWS` (`ProviderParityRow` frozen dataclass: `provider`, `warehouse_domain`, `parity_status`, `limitations`, `next_gap`, ...) to advance `parity_status` on the axes pf2 closed, then `seed_provider_parity_matrix(store)` into `provider_parity_matrix`. (4) The **whole-branch review** across all pf2 sprints.

**PIT:** (B) 0131 catalogued, indexes split from schema. (D) rebuild is a pure function of source + `git_sha` + params.

**Accept-with-fixture:** the rebuild command drives the full DAG through the gate and writes one `warehouse_rebuild_runs` row with the captured `git_sha`; a fixture rebuild is byte-identical on re-run; the DR runbook exists and is followed once operator-side; `provider_parity_matrix` reflects the flipped `parity_status` after `seed_provider_parity_matrix`.

---

## Sequencing & expected compounding

**S10-0 → S10-1 → S10-2 → S10-3.** S10-0 (severity + gate wiring) is load-bearing — every later observability and freshness signal routes through the *same* gate, so the taxonomy and the orchestrator hook must land first. S10-1 (freshness + anomaly) reuses S10-0's severity path to escalate a stale/anomalous surface to a halting `critical`. S10-2 (storage + partitioned lake) is independent of the gate SQL but its DB-growth signal becomes a gated check once S10-0 exists. S10-3 (rebuild + DR + parity flip + review) is last — it drives the whole gated, monitored, storage-managed warehouse end-to-end and records the outcome. The compounding: once `critical` checks halt runs, the ~288 checks become **trustworthy SLO gates** instead of write-only rows; freshness + anomaly turn `dataset_watermarks` and `data_quality_checks` history from recorded-but-ignored into live monitors; and the deterministic rebuild proves the whole platform is operationally reproducible — the capstone that makes pf2 trustworthy in production.

---

## Risks / guardrails

- **A gate halts a run it should not (false-positive critical).** Central risk: over-promoting a flaky check to `critical` stalls the whole warehouse. Mitigate by keeping `critical` a small, curated, data-driven set in `quality_check_registry` (retunable without a deploy), defaulting existing checks to `error`/`warning`, and making gating opt-in per run (`gate=True`) so content-sprint tests are unaffected until adopted per clause (G).
- **Gating changes what an existing check *means*.** The ~288 `SqlQualityCheck.sql`/`threshold` bodies stay byte-identical; severity is additive metadata, `data_quality_checks.severity` is `ADD COLUMN IF NOT EXISTS`. A passing check passes exactly as before.
- **CHECKPOINT/VACUUM on the 14 GB live file corrupts or stalls.** Never run storage compaction without PF2-S2's backup pre-flight (clause (F)); split schema/index across 0130/0131 per the S5g/S5k WAL precedent; test compaction only on fixture DBs — the live CHECKPOINT is operator-run and recorded.
- **Lake partitioning breaks the existing single-file consumers.** Partitioning is opt-in per object; undeclared objects keep the `part-00000.parquet` full-snapshot path and their `sha256`/`schema_sha256` manifest lineage.
- **Duplicating PF2-S1/S2 surfaces.** Schema-drift stays clause (E)/`schema_contract.py`; `.bak` retention + restore stay PF2-S2/`migration_admin.py`. This sprint *consumes* both, never re-implements them.
- **Migration/WAL safety.** Stay strictly within **0128–0131**; every new table/column catalogues in the same migration; preserve a timestamped DB+WAL backup before any live apply.

---

## Bench / acceptance

- A **planted `critical` check halts** a gated orchestrator run (`etl_job_runs.status='failed'`, `step_quality_gate_halt` audit row, downstream `pending`); `error`→`partial`; `warning`→`succeeded`; `gate=False` reproduces today's ungated behavior.
- **Freshness SLA + row-count anomaly** surface: a watermark past its `max_lag_days` and a planted count outlier each emit a severity-tagged `data_quality_checks` / `data_quality_anomaly` row routed through the gate.
- **Storage tracked + compacted**: `warehouse_storage_stats` records `db_size_bytes`; `checkpoint_and_compact` runs clean; a partition-declared lake object exports to multiple partitions with `partition_columns` populated and incremental skip proven.
- **Deterministic rebuild**: the full-rebuild command drives the whole gated DAG and writes a reproducible `warehouse_rebuild_runs` row; a fixture rebuild is byte-identical on re-run; DR runbook present.
- `python -m pytest atx-impl\db\tests\test_quality_gating.py atx-impl\db\tests\test_observability.py -q` green **offline**; full `python -m pytest atx-impl\db\tests -q` green before commit.
- **Live smoke** recorded in the ledger: a gated live run (with the halt exercised on a deliberately-tripped critical, then reverted), a `warehouse_storage_stats` row with the real `db_size_bytes`, a freshness-SLA sweep, and the deterministic rebuild's `rebuild_run_id` + `git_sha` + exact per-dataset counts.
- **Whole-branch review** across all pf2 sprints; **parity-ledger flip** — `db/parity.py` `ProviderParityRow.parity_status` advanced on the closed axes and re-seeded via `seed_provider_parity_matrix` into `provider_parity_matrix`; `PARITY_GAP.md` status updated and a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live-DB smoke with exact counts + `run_id`, caveats/next).

**Process:** never `git add -A` (stage explicit paths); never push unless asked. New module ⇒ new `test_*.py`. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
