# atx-db Tier-1 v2 — Sprint 1: Bounded Build Platform, B0 Raw Completion, Research Foundation, First Real Results

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan
> task-by-task. Steps use checkbox (`- [ ]`) syntax. **No TDD** (user directive): acceptance is a real-data
> measurement; tests only where the index §5.4 allows (kill-and-resume drills and migrations on real-shaped data are
> allowed and required here).

**Goal:** Make every heavy job fit in ≤ 1.0 GiB as resumable ledgered batches, use that to finish the raw layer
(CompanyFacts all 20,390 members, submissions all 991,042 members, CVX wave) in the production warehouse, and — in
parallel, without touching the warehouse — stand up the Parquet research store, freeze qualification policy v4, and
produce the first real cross-sectional results on ~40 price and trading-friction characteristics.

**Architecture:** Three platform pieces: bounded DuckDB sessions applied at connect time (so WAL replay is bounded
too); a `build_batches` ledger plus a slice runner that admits work through the unchanged memory guard and exits after
≤ 8 batches or 10 minutes; append-only `__next` staging tables published by one short swap. CompanyFacts and
submissions are rebuilt by extracting the retained archives to Parquet without the warehouse lock, then loading ~80–100
batches. Research gets its own immutable Parquet data plane (lake, per-feature content-addressed store, label matrix,
evaluation cache), so the price wave runs from the retained `TickerHistory3.parquet` while the warehouse is busy.

**Tech Stack:** Python, DuckDB 1.5.5, PyArrow (zstd Parquet), `run_memory_guarded.py`, existing research stats core.

**Spec:** `docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md` (§3, §4 memory doctrine, §5, rulings R-1…R-7,
R-11, R-12). Nodes 1.1–1.13.

## Global Constraints

Every task includes the index §3 (safety, SEC user agent `atx-db/0.1 atx-research@example.com` only, Write/Edit-only
edits, SERIALIZED/token files, WAL rules, HEAVY token for any prod open), §4 (M1–M8: ≤ 1.0 GiB job cap, ≤ 2M rows per
transaction, ledgered slices, no ART on bulk tables, lean processes, bounded settings, re-engineer on OOM, never lower
guard floors) and §5 (tokens, ≤ 8 agents, agent memory gate, `OPENBLAS_NUM_THREADS=1`, commit hygiene, trailer
`Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`). Base commit: the S0 integration gate sha.

Paths are relative to `C:\atx\atx-db\` unless they start with `.superpowers/` or `docs/`.

## Review Focus

1. **Kill mid-batch.** The guard hard-terminates the process (TerminateJobObject) at phys < 1.5 or commit < 3 GiB at
   any instant. Expected: rerun resumes from the ledger, final rows equal a clean run, 0 duplicate natural keys, no
   stale `running` rows, no WAL left after recovery.
2. **Equivalence with the old loaders.** The staged CompanyFacts/submissions rebuild must reproduce the old loader's
   rows exactly for already-verified CIKs (digest equality) and the old dispositions (1,327 empty, 43 unavailable).
3. **Look-ahead in research inputs built outside the warehouse.** Price-wave inputs come straight from the vendor
   Parquet: vendor share runs start at the cover date before the filing is public (A8), the 2021-01-04 vendor factor
   artifact biases returns (VA1), and stray bars exist on exchange closures. Expected: lagged shares, repaired returns,
   calendar-filtered sessions.
4. **Pre-registration order.** No wave may read a forward return before its catalog rows and policy v4 hash are frozen
   (RX7 / ruling R-6); holdout formations stay sealed in Sprint 1.
5. **Contention.** Research workers must never open the warehouse; a HEAVY writer slice must never be blocked by a
   research reader holding the file.

---

### Task 1: [1.1] Bounded sessions and lean workers

**Lane:** M · **Tokens:** — · **Depends on:** 0.15 · **Peak target:** 0.6 GiB

**Files:**
- Modify: `src/atx_db/connection.py:47-151` (`DuckDBStore.__enter__` connects with config; `reopen()` shares it)
- Modify: `src/atx_db/cli.py:78-93` (add `max_temp_directory_size`, `temp_directory`)
- Modify: `src/atx_db/__init__.py` (lazy exports via PEP 562 `__getattr__`; `OPENBLAS_NUM_THREADS` default)

**Interfaces:**
- Produces:
  ```python
  # connection.py
  DEFAULT_TEMP_DIR = Path("C:/atx/atx-db/data/tmp/duckdb")

  def bounded_config(memory_limit: str = "384MB", threads: int = 1, *,
                     temp_directory: Path = DEFAULT_TEMP_DIR,
                     max_temp_directory_size: str = "40GB") -> dict[str, object]:
      """Config applied at duckdb.connect(), so WAL replay and initialize() run inside the budget."""
      return {"memory_limit": memory_limit, "threads": threads, "preserve_insertion_order": False,
              "temp_directory": str(temp_directory), "max_temp_directory_size": max_temp_directory_size}
  ```
  `DuckDBStore(path, *, memory_limit=..., threads=..., read_only=False)` uses `bounded_config` on first connect and on
  every `reopen()`.
- `atx_db/__init__.py` first two lines: `import os` and `os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")`, before
  anything imports numpy; every public name resolves lazily.

- [ ] **Step 1:** Implement `bounded_config` and route both connect paths through it.
- [ ] **Step 2:** Convert `atx_db/__init__.py` to lazy exports (keep `__all__` and the public API snapshot names
  resolvable: `tests/data/public_api_snapshot.json` lists them).
- [ ] **Step 3 (measure):** In a fresh interpreter: `import atx_db` then read private bytes
  (`psutil.Process().memory_info().private` if psutil is installed, else PowerShell `Get-Process -Id <pid>`). Target ≤ 120 MB.
- [ ] **Step 4 (drill, allowed test):** create a scratch DuckDB, write 2M rows into a table with a `DEFAULT now()`
  column, terminate the process before `CHECKPOINT` (leaving a WAL), reopen through `DuckDBStore` and assert
  `SELECT current_setting('memory_limit')` equals the bounded value on the first statement and the guard peak during
  replay ≤ 0.6 GiB.
- [ ] **Step 5:** Commit.

**Acceptance:** `import atx_db` ≤ 120 MB private; drill passes; `grep -n "duckdb.connect(" src/atx_db/connection.py`
shows only bounded calls.

---

### Task 2: [1.2] MIG-B — migration 0329 (build ledger, drop bulk PKs), 0327 item 7 removal, full-copy kill drill

**Lane:** MIG · **Tokens:** MIG · **Depends on:** 0.15, 1.1 · **Peak target:** 1.0 GiB (drill on a prod copy)

**Files:**
- Create: `src/atx_db/migrations/bodies_0329.py`
- Modify: `src/atx_db/migrations/registry.py` (register 0329), `src/atx_db/migrations/bodies_0327.py:1059-1079` (item 7)
- Modify: `src/atx_db/schema.py` (DDL of the affected tables), `tests/test_migrations.py` (checksums)

**Interfaces:**
- Produces tables (no `DEFAULT now()`, no PK/ART):
  ```sql
  CREATE TABLE build_runs (
      stage VARCHAR NOT NULL, run_key VARCHAR NOT NULL, spec_json VARCHAR NOT NULL,
      status VARCHAR NOT NULL,            -- 'open' | 'published' | 'abandoned'
      created_at TIMESTAMP NOT NULL, published_at TIMESTAMP, code_sha VARCHAR, note VARCHAR);
  CREATE TABLE build_batches (
      stage VARCHAR NOT NULL, run_key VARCHAR NOT NULL, batch_id INTEGER NOT NULL,
      lo VARCHAR, hi VARCHAR, input_sha256 VARCHAR,
      rows_in BIGINT NOT NULL, rows_out BIGINT NOT NULL,
      started_at TIMESTAMP NOT NULL, finished_at TIMESTAMP NOT NULL,
      process_id INTEGER, note VARCHAR);   -- a row exists only for a COMMITTED batch
  CREATE TABLE equity_bar_unit_corrections (   -- ledger for the A9 vendor-shares x1000 correction
      security_id VARCHAR NOT NULL, run_start DATE NOT NULL, run_end DATE NOT NULL,
      unit_basis VARCHAR NOT NULL, multiplier DOUBLE NOT NULL, evidence VARCHAR NOT NULL, created_at TIMESTAMP NOT NULL);
  ```
- Drops the VARCHAR sha256 PRIMARY KEY (by create/copy/swap, tables empty per 0.1) on: the 8 tables in
  `_fundamental_publication._KEYS` (`_fundamental_publication.py:11-20`), `derived_metric_values`,
  `market_daily_metrics` (`migrations/bodies_0302.py:117-158`), `equity_price_metrics`
  (`equity_price_metrics.py:462-501` DDL), `forward_returns_survivorship_safe` (`bodies_0185_0188.py:257`),
  `daily_adjustment_factors`. Id columns stay; physical order will follow `(security_id, date)` at load time.
- 0327 item 7 (the 32M-row in-place `UPDATE equity_daily_bars SET shares_outstanding = shares_outstanding * 1000 …`)
  is removed from the migration; 0327 keeps only the unit **inventory and abort rule** (A9: unit = file-format evidence
  AND median agree; thousands run with any row > 1e8 aborts). The correction itself becomes the batch stage
  `bars_unit_correction` built in 1.3 and run in 1.8.

- [ ] **Step 1:** Confirm from the 0.1 receipt that every table whose PK is dropped has 0 rows. If one is non-empty, it is
  swapped in ≤ 2M-row buckets inside the migration only if ≤ 2M rows total; otherwise stop and report.
- [ ] **Step 2:** Write `bodies_0329.py` using `refresh_schema_contract_pin_by_swap` from 0.2 for the contract re-pin.
- [ ] **Step 3:** Remove item 7 from 0327 (0327 is unapplied in prod: allowed); re-baseline checksums; rebuild the
  template once.
- [ ] **Step 4 (drill on a full prod copy, allowed e2e):** copy `data/warehouse.duckdb` to the scratch directory (12 GiB;
  file copy only, never open prod itself), then under `run_memory_guarded.py --job-gb 1.0`: apply every pending
  migration through 0329 with the bounded config; in a second run terminate after the 0328 COMMIT log line; reopen
  (bounded), finish migrating, run `verify_schema` and one `schema_contract` write.
- [ ] **Step 5:** Record wall time and guard peak per migration; commit.

**Acceptance:** on the prod copy: migrate to 0329 completes with guard peak ≤ 1.0 GiB; the kill drill recovers with
`verify_schema == ()`; `duckdb_constraints()` shows no PRIMARY KEY on the 13 listed tables; no WAL after the final
CHECKPOINT. The copy is deleted after the drill (it is scratch).

---

### Task 3: [1.3] Batch ledger runtime, slice runner, stale-run recovery, and the bars unit-correction stage

**Lane:** M · **Tokens:** — · **Depends on:** 1.1, 1.2 · **Peak target:** 0.3 GiB runtime; 0.8 GiB for the drill

**Files:**
- Create: `src/atx_db/batch_runner.py`, `src/atx_db/bars_unit_correction.py`
- Create: `scripts/run_slices.py`, `scripts/recover_stale_run.py`
- Create: `tests/test_batch_runner_kill_resume.py` (the one allowed kill/resume e2e test)

**Interfaces:**
- Produces:
  ```python
  # batch_runner.py
  @dataclass(frozen=True)
  class BatchSpec:
      batch_id: int
      lo: str | None = None
      hi: str | None = None
      input_sha256: str | None = None
      payload: Mapping[str, object] = field(default_factory=dict)

  @dataclass(frozen=True)
  class BatchResult:
      rows_in: int
      rows_out: int
      note: str | None = None

  @dataclass(frozen=True)
  class BatchStage:
      name: str
      plan: Callable[[duckdb.DuckDBPyConnection, str], list[BatchSpec]]          # (conn, run_key) -> batches
      prepare: Callable[[duckdb.DuckDBPyConnection, str], None]                   # create __next tables once
      build: Callable[[duckdb.DuckDBPyConnection, BatchSpec, str], BatchResult]   # inside the batch transaction
      finalize: Callable[[duckdb.DuckDBPyConnection, str], dict[str, object]]     # validate + swap + receipt
      memory_limit: str = "384MB"
      threads: int = 1

  @dataclass(frozen=True)
  class SliceResult:
      stage: str
      run_key: str
      committed: tuple[int, ...]
      remaining: int
      stopped: Literal["complete", "max_batches", "max_seconds", "error"]

  BATCH_STAGES: dict[str, BatchStage]           # registry; 1.3 registers "bars_unit_correction"

  def register_stage(stage: BatchStage) -> None: ...
  def open_run(conn, stage: str, run_key: str, spec: Mapping[str, object], code_sha: str) -> None: ...
  def pending_batches(conn, stage: str, run_key: str) -> list[BatchSpec]: ...
  def run_slice(db_path: Path, stage: str, run_key: str, *, max_batches: int = 8, max_seconds: int = 600,
                checkpoint_every: int = 4, recycle_every: int = 4) -> SliceResult: ...
  def finalize_run(db_path: Path, stage: str, run_key: str) -> dict[str, object]: ...
  ```
  Per batch, exactly: `BEGIN` → `stage.build(conn, spec, run_key)` → `INSERT INTO build_batches …` → `COMMIT`. A
  `CHECKPOINT` every `checkpoint_every` batches and at slice end; reconnect every `recycle_every` batches.
- CLI (used by the OPS agent):
  ```
  python scripts/run_slices.py --db <path> --stage <name> --run-key <key> [--spec-json <file>]
         --job-gb 0.8 --max-batches 8 --max-minutes 10 [--until-complete --sleep 60] [--finalize]
  ```
  With `--until-complete`, each slice is a **fresh guarded subprocess** (`run_memory_guarded.py --job-gb <x> -- python
  scripts/run_slices.py … --single-slice`); a guard preflight refusal or a guard kill sleeps 60 s and retries; after the
  last batch it runs one finalize slice. It writes one JSON line per slice to
  `.superpowers/sdd/tier1-v2/receipts/<stage>-<run_key>.jsonl` (admitted/refused, batches committed, guard peak, rc).
- `scripts/recover_stale_run.py --db <path>`: refuses if a live writer PID is recorded in the newest guard receipt;
  opens exclusively with `bounded_config("256MB")`; marks `activation_stage_runs` / `dataset_runs` rows still `running`
  as `failed` (explicit timestamps; small keyed UPDATE only on those small tables); `CHECKPOINT`; asserts no WAL and
  `verify_schema == ()`.
- `bars_unit_correction` stage: `plan` = 16 buckets of `hash(security_id) % 16`; `prepare` = `CREATE TABLE
  equity_daily_bars__next AS SELECT * FROM equity_daily_bars WITH NO DATA`; `build` = insert the bucket's bars with
  `shares_outstanding`/`market_cap_usd` multiplied by 1000 on the runs the A9 inventory classifies as thousands, and the
  matching `equity_bar_unit_corrections` rows; `finalize` = row-count equality, 0 duplicate `(security_id, trade_date,
  source)`, swap `equity_daily_bars` ↔ `__next` via `_bulk_publication.publish_validated_shadows`
  (`_bulk_publication.py:84-120`; drop/recreate secondary indexes inside the swap if any), `CHECKPOINT`, no-WAL assert.

- [ ] **Step 1:** Implement `batch_runner.py` and the CLI. Read `run_memory_guarded.py` for its exact CLI and refusal
  exit codes before wiring `--until-complete`.
- [ ] **Step 2:** Implement `bars_unit_correction.py` reusing the A9 unit rule functions (do not re-derive the rule).
- [ ] **Step 3:** Implement `recover_stale_run.py`.
- [ ] **Step 4 (the kill/resume e2e test):** on a scratch DuckDB holding 2M real bars copied from
  `TickerHistory3.parquet`, run the stage with `max_batches=16`; terminate the worker mid-batch three times at random
  points; rerun to completion. Assert final rows equal a clean run (`EXCEPT ALL` both ways = 0), 0 duplicate keys, no
  `running` rows after `recover_stale_run`, no WAL.
- [ ] **Step 5 (real-data drill):** run `bars_unit_correction` on the 1.2 prod **copy** (if deleted, re-copy) through
  `run_slices.py --until-complete --job-gb 0.8`. Record per-slice guard peaks and admission counts. AAPL and NVDA stored
  shares must match the A9 verified unit evidence (AAPL vendor 15,204,137 thousands → 15.2B shares) and the 10-Q cover
  counts.
- [ ] **Step 6:** Commit; the report documents the stage contract for other lanes.

**Acceptance:** kill/resume test green; real-copy drill completes with every slice ≤ 0.8 GiB guard peak; every
corrected run appears in `equity_bar_unit_corrections`; `.superpowers/sdd/tier1-parity/claude-ctl/autolaunch-archive21.ps1`
is marked retired in the runbook (not deleted, not run).

---

### Task 4: [1.4] CF-R extract — CompanyFacts archive to Parquet batches (no warehouse)

**Lane:** D · **Tokens:** — · **Depends on:** 1.1 · **Peak target:** 0.5 GiB

**Files:**
- Create: `src/atx_db/companyfacts_stage.py`, `scripts/stage_companyfacts.py`

**Interfaces:**
- Produces:
  ```python
  @dataclass(frozen=True)
  class CompanyFactsBatch:
      batch_id: int
      first_cik: int
      last_cik: int
      members: tuple[str, ...]          # zip member names in CIK order
      uncompressed_bytes: int

  def archive_sha256(zip_path: Path) -> str: ...
  def plan_companyfacts_batches(zip_path: Path, target_uncompressed_bytes: int = 230_000_000) -> list[CompanyFactsBatch]: ...
  def extract_companyfacts_batch(zip_path: Path, batch: CompanyFactsBatch, out_dir: Path) -> dict[str, int]: ...
  ```
  Output directory `data/staging/companyfacts/<archive_sha16>/`:
  - `batch-NNNN.parquet` (zstd, 128k-row row groups; written to `.tmp` then renamed) with exactly the columns
    `sec_company_facts` needs plus the inputs of the `fundamental_points` projection (derive the list from
    `_replace_facts`, `fundamentals.py:1399-1490`);
  - `members.parquet`: `cik, member, crc32, compressed_bytes, uncompressed_bytes, disposition, reason, rows, payload_cik`
    with disposition vocabulary identical to today's: `loaded`, `empty(<reason>)`, `unavailable(empty_archive_placeholder)`;
  - `manifest.json`: archive sha256, rule version, planner parameters, batch list with row counts and file sha256.
- Normalization must reproduce `normalize_companyfacts` (`fundamentals.py:652-738`) exactly: taxonomy filter
  `us-gaap`/`dei`, the concept allowlist, the `end ≤ filed` rule, `available_at = filed + 22h`, numeric coercion, the
  exact `{}` placeholder rule, the payload-CIK check. Emit PyArrow column arrays, not dict rows or pandas.

- [ ] **Step 1:** Plan from the ZIP central directory (member names, CRC32, sizes; < 1 s measured). Expect ~80 batches
  (17.98 GiB / 230 MB), mean ~255 members.
- [ ] **Step 2:** Implement per-member parsing with the stdlib `json` (no new dependencies) into column builders; one
  member resident at a time (max member 8.8 MB JSON).
- [ ] **Step 3 (equivalence):** for 20 CIKs sampled across the size distribution, compare the extracted rows with
  `normalize_companyfacts` output: multiset equality on every column.
- [ ] **Step 4 (real run):** extract all batches under `run_memory_guarded.py --job-gb 0.5`; measure the first 3 batches'
  peak before running the rest; resume skips any batch whose file and manifest row exist with the same archive sha.
- [ ] **Step 5:** Commit code (not the data).

**Acceptance:** all 20,390 members in `members.parquet`; 1,327 empty (1,272 / 53 / 2 by reason) and 43 unavailable,
matching the ops-a counts; ~55–60M fact rows total; Parquet ≈ 1.3–1.6 GB; peak ≤ 0.5 GiB; wall time recorded.

---

### Task 5: [1.5] CF-R load, verify and publish (batch stage `companyfacts_rebuild`)

**Lane:** D · **Tokens:** ACT (stage-mode wiring) · **Depends on:** 1.3, 1.4 · **Peak target:** 0.8 GiB

**Files:**
- Create: `src/atx_db/companyfacts_rebuild.py`
- Modify: `src/atx_db/activation.py:926-955` (`stage_companyfacts_load`: `--companyfacts-mode staged` delegating to the
  batch stage; the legacy per-member path stays for tests only)

**Interfaces:**
- Consumes: `data/staging/companyfacts/<archive_sha16>/` from 1.4; `BatchStage` API from 1.3.
- Produces batch stage `companyfacts_rebuild` (run_key = `<archive_sha16>`):
  - `prepare`: create `sec_company_facts__next` and `fundamental_points__next` from the live DDL (no indexes); copy
    non-SEC `fundamental_points` rows and `cf_retained` rows (CIKs whose member is an `unavailable` placeholder or absent
    from the archive — preserves "placeholders keep prior data"); create `cf_candidates__next`.
  - `build` (one transaction per batch):
    ```sql
    CREATE TEMP TABLE b AS SELECT * FROM read_parquet(?);
    -- identity resolution, set-based: port fundamentals.py:437-513 onto
    CREATE TEMP TABLE resolved AS <as-of identifier join over (SELECT DISTINCT cik, available_at FROM b)>;
    INSERT INTO sec_company_facts__next SELECT <columns> FROM b JOIN resolved USING (cik, available_at);
    INSERT INTO fundamental_points__next SELECT <points projection, symbol = NULL> FROM b JOIN resolved USING (cik, available_at);
    INSERT INTO cf_candidates__next SELECT <unresolved per-CIK summary> FROM resolved WHERE <unresolved>;
    DROP TABLE b; DROP TABLE resolved;
    ```
    No Python row loop; the per-row `.at[]` loop in `resolve_company_facts_identifiers` (`fundamentals.py:389-551`) is
    replaced by SQL.
  - `finalize` (verify, then publish):
    1. counts: `__next` = Σ ledger `rows_out` + retained rows;
    2. 0 duplicates on `(cik, taxonomy, concept, unit, period_start, period_end, accession, filed_date)`;
    3. **digest equality** for the 9,462 lineage-verified CIKs, reusing `_companyfacts_resume._projection` /
       `_digest_sums` (`_companyfacts_resume.py:108-127`) without `run_id`, `__next` vs live; any difference listed per
       CIK and the publish refused;
    4. dispositions equal to 1.4's manifest;
    5. one transaction: swap `sec_company_facts` and `fundamental_points`; replace the
       `match_method='companyfacts_cik_spine_asof'` rows of `identifier_resolution_candidates` (it has two ART indexes:
       DELETE those rows + INSERT, a small keyed operation); one `dataset_runs` row (`mode=staged_rebuild`, archive sha)
       and one `raw_source_files` receipt per batch;
    6. `CHECKPOINT`; assert no WAL.

- [ ] **Step 1:** Implement the stage; register it in `BATCH_STAGES`.
- [ ] **Step 2:** Wire `--companyfacts-mode staged` in `activation.py` (ACT token).
- [ ] **Step 3 (dress rehearsal on a prod copy at 0329):** run `run_slices.py --stage companyfacts_rebuild --until-complete
  --job-gb 0.8` then `--finalize`; kill one slice deliberately; record per-batch seconds (target ≤ 60 s) and peaks.
- [ ] **Step 4:** Commit.

**Acceptance (rehearsal):** digest equality for all 9,462 CIKs; counts reconcile; 0 duplicates; every batch ≤ 60 s and
≤ 0.8 GiB; swap + CHECKPOINT leaves no WAL; `sec_company_facts` covers every loadable member.

---

### Task 6: [1.6] Submissions staged rebuild (extract + batch stage `submissions_rebuild`)

**Lane:** D · **Tokens:** ACT (stage-mode wiring) · **Depends on:** 1.3 (reuse 1.4 helpers) · **Peak target:** 0.8 GiB

**Files:**
- Create: `src/atx_db/submissions_stage.py`, `src/atx_db/submissions_rebuild.py`, `scripts/stage_submissions.py`
- Modify: `src/atx_db/activation.py:885` (`--submissions-mode staged`)

**Interfaces:**
- Extract: main members (`CIK##########.json`) in CIK order with their history members
  (`CIK##########-submissions-NNN.json`) to ~100 Parquet batches of ~55 MB JSON each under
  `data/staging/submissions/<archive_sha16>/`, ≤ 350 MB Python, no pandas; `members.parquet` with CRC32 per member.
  Keep `acceptanceDateTime`, `items` (8-K item list), `form`, `filingDate`, `reportDate`, `accessionNumber`,
  `primaryDocument`, `isXBRL`, `isInlineXBRL` — 3.4 and 3.9 consume `items` and `acceptanceDateTime`.
- Load: batch stage `submissions_rebuild` appending into `sec_submissions__next` (replaces the pandas
  accumulate + `DELETE … USING` flush at `sec_submissions.py:317-334`), finalize with counts, 0 duplicate
  `(cik, accession_number)`, swap, CHECKPOINT, no WAL.

- [ ] **Step 1:** Implement extract + stage; equivalence on 200 CIKs against the existing loader (`sec_submissions.py:600-797`).
- [ ] **Step 2:** Rehearse on the prod copy with one deliberate kill.
- [ ] **Step 3:** Commit.

**Acceptance (rehearsal):** 991,042 members read; `scope_complete=true`; `missing_history_members=0`; rows = Σ batches;
the 8-K Item 2.02 candidate count reproduces 426,151; batches ≤ 0.8 GiB.

---

### Task 7: [1.7] SEC network hygiene and fetch/load split

**Lane:** X · **Tokens:** ACT (`activation.py` edits) · **Depends on:** 0.15 · **Peak target:** 0.6 GiB

**Files:**
- Create: `src/atx_db/sec_http.py`
- Modify: `src/atx_db/security_master.py:34-84` (use `sec_http`), `src/atx_db/press_release.py` (fetch worker writes
  files + JSONL ledger; loader reads cache), `src/atx_db/activation.py:371,376,547,920`, `src/atx_db/finra.py:87`,
  `src/atx_db/fsds_baseline.py` (share the limiter), every module that still hard-codes the UA string
  (`git grep -n "atx-research@example.com" src scripts`)

**Interfaces:**
- Produces:
  ```python
  APPROVED_SEC_USER_AGENT = "atx-db/0.1 atx-research@example.com"

  def validate_sec_user_agent(value: str | None) -> str:
      """Return APPROVED_SEC_USER_AGENT; raise ValueError for any other non-empty value."""

  class SecRateLimiter:
      """Cross-process token bucket (file lock at data/cache/.sec_rate.lock + state file); default 5 req/s."""
      def __init__(self, rate_per_s: float = 5.0, lock_path: Path = ...): ...
      def acquire(self) -> None: ...

  def sec_get(url: str, *, limiter: SecRateLimiter, timeout: float = 30.0) -> bytes: ...
  ```
  Fetch workers never hold a DuckDB writer; they write content-addressed files plus `fetch-ledger.jsonl` (url, sha256,
  status, fetched_at) and resume by skipping ledgered urls.

- [ ] **Step 1:** Implement `sec_http.py`; replace every UA literal and limiter; `ATX_SEC_USER_AGENT` / `--sec-user-agent`
  go through `validate_sec_user_agent`; fix the error text at `activation.py:376` (it suggests a non-approved agent).
- [ ] **Step 2:** Split `press_release.py` into fetch (files + ledger) and load (cache → Parquet → batch append).
- [ ] **Step 3 (real, within existing approval):** pre-fetch the CVX acceptance wave (the approved `cvx-eps-desk2`
  candidate set from the A7 receipts) with the new worker; kill once mid-run; resume.
- [ ] **Step 4:** Commit.

**Acceptance:** `git grep -n "@" -- src scripts | grep -i "user.agent\|user_agent"` finds only
`APPROVED_SEC_USER_AGENT`; measured rate ≤ 5 req/s across two concurrent workers; resume after kill has 0 duplicate
fetches; no DuckDB writer open during fetch.

---

### Task 8: [1.8] B0 heavy run — migrate, unit correction, CompanyFacts, submissions, CVX (real warehouse)

**Lane:** OPS · **Tokens:** HEAVY (writer), ACT (invocation only) · **Depends on:** 0.1, 1.2, 1.3, 1.5, 1.6, 1.7 ·
**Peak target:** ≤ 1.0 GiB per slice

**Files:**
- Create: `.superpowers/sdd/tier1-v2/receipts/b0-*.json[l]`, `.superpowers/sdd/tier1-v2/receipts/b0-summary.md`

- [ ] **Step 1 (preconditions):** S0 gate PASS; 1.2–1.7 reviewed and committed; pinned export of HEAD created; no WAL
  on prod; watcher pid 8072 not running (if it is, wait for its deadline — never stop it, never use the archive21 path).
- [ ] **Step 2 (backup):** file-copy `data/warehouse.duckdb` to `data/warehouse.duckdb.pre-b0.<UTC>.bak`; record sha256.
  Keep the 2026-09-24 backup until B1 passes.
- [ ] **Step 3 (migrate):** governed migrate to head 0329 under `run_memory_guarded.py --job-gb 1.0` with the bounded
  config; `verify_schema == ()`; no WAL.
- [ ] **Step 4:** `run_slices.py --stage bars_unit_correction --until-complete --job-gb 0.8` then `--finalize`.
- [ ] **Step 5:** `run_slices.py --stage companyfacts_rebuild --until-complete --job-gb 0.8` then `--finalize` (digest
  equality must pass, else stop: never publish on mismatch).
- [ ] **Step 6:** `run_slices.py --stage submissions_rebuild --until-complete --job-gb 0.8` then `--finalize`.
- [ ] **Step 7:** CVX earnings-release load from the 1.7 cache (loader only), then `entity_classification` (one small
  transaction, ~13k CIKs).
- [ ] **Step 8:** Write `b0-summary.md`: admission rate, slices, kills and recoveries, peaks, wall/admitted time, counts.

**Acceptance:** index §2 "Raw completeness" row satisfied on prod; every slice ≤ 1.0 GiB; no WAL at the end; the
receipts name the export sha.

---

### Task 9: [1.9] Research store foundation — Parquet lake, per-feature store, label matrix, evaluation cache, workers

**Lane:** R · **Tokens:** — · **Depends on:** 0.15 · **Peak target:** 0.6 GiB per worker

**Files:**
- Create: `src/atx_db/research/research_lake.py` (L1 snapshots), `src/atx_db/research/feature_store.py` (L2),
  `src/atx_db/research/label_matrix.py` (L3), `src/atx_db/research/eval_cache.py` (L4),
  `src/atx_db/research/workers.py`
- Modify: `src/atx_db/research/evaluation.py` — **only** add an input adapter `load_feature_table_from_store(...)`
  (EVAL token: coordinate with 1.10's holder; if held, put the diff in the report for 1.10's owner)

**Interfaces:**
- Layout (root `data/research/`):
  ```
  lake/<snapshot_id>/<dataset>/year=YYYY/part-*.parquet      + lake/<snapshot_id>/_manifest.json
  features/<basis>/<feature_id>/<feature_sha>.parquet        (eom DATE, line_id VARCHAR, owner_id VARCHAR,
                                                              raw DOUBLE, signed DOUBLE, rank_u DOUBLE, z DOUBLE,
                                                              reason INT8, clock_offset_s INT32)
  labels/<label_sha>/h=<K>/year=YYYY.parquet                 (eom, line_id, owner_id, entry_date, exit_date,
                                                              ret DOUBLE, ret_exc DOUBLE, basis VARCHAR, reason INT8)
  eval/<feature_sha>/<label_sha>/<eval_spec_sha>.parquet     (one row per evaluation cell)
  manifests/<wave>/<manifest_sha>.json                       (list of {feature_id, feature_sha, basis})
  ```
- Produces:
  ```python
  def compute_feature_sha(feature_id: str, catalog_row: Mapping[str, object], code_digest: str,
                          input_digests: Mapping[str, str]) -> str: ...   # sha256 of canonical JSON
  class FeatureStore:
      def __init__(self, root: Path = Path("C:/atx/atx-db/data/research")): ...
      def path(self, basis: str, feature_id: str, feature_sha: str) -> Path: ...
      def has(self, basis: str, feature_id: str, feature_sha: str) -> bool: ...
      def write(self, basis: str, feature_id: str, feature_sha: str,
                batches: Iterable[pa.RecordBatch], meta: Mapping[str, object]) -> Path: ...   # tmp + rename
      def scan(self, basis: str, feature_ids: Sequence[str], years: Sequence[int] | None = None
               ) -> duckdb.DuckDBPyRelation: ...
  def write_manifest(root: Path, wave: str, entries: Sequence[Mapping[str, str]]) -> str: ...  # returns manifest sha
  def export_lake_snapshot(db_path: Path, snapshot_id: str, datasets: Mapping[str, str]) -> Path: ...
      # datasets: name -> SELECT; COPY (...) TO ... (FORMAT PARQUET, PARTITION_BY (year), COMPRESSION ZSTD); read-only
  def run_jobs(jobs: Sequence[Callable[[], None]] | Sequence[list[str]], max_workers: int = 2) -> list[int]: ...
      # subprocess per job, env OPENBLAS_NUM_THREADS=1, DuckDB 256MB/1 thread
  ```
- Cross-sectional transforms (winsor, z, rank_normal, industry/size neutral) are **imported** from the existing R2b
  functions in `research/features.py` — no forks.

- [ ] **Step 1:** Implement the modules.
- [ ] **Step 2 (oracle parity):** on the R2b/R3b fixture store, build 5 features through both paths; assert the feature
  values match within 1e-12 and R3b evaluation cells match exactly.
- [ ] **Step 3 (scale probe, real):** write one synthetic-free feature (log price at month end) for 2012-04 → 2026-08
  from `TickerHistory3.parquet`; record file size, write seconds, peak.
- [ ] **Step 4:** Commit.

**Acceptance:** parity passes; the real probe stays ≤ 0.6 GiB; add-one-feature path documented in the report.

---

### Task 10: [1.10] Qualification policy v4, power study, wave pre-registration and trial registry

**Lane:** R · **Tokens:** EVAL · **Depends on:** 0.15 (0.10 catalog metadata) · **Peak target:** 0.4 GiB

**Files:**
- Create: `src/atx_db/seeds/research_qualification_policy_v4.json`, `src/atx_db/research/trial_registry.py`,
  `docs/research/POLICY_V4_AND_POWER.md`, `scripts/research_power_study.py`
- Modify: `src/atx_db/research/qualification.py`, `src/atx_db/research/evaluation.py` (family scope, investable slice,
  Jegadeesh–Titman calendar-time portfolios, population coverage)

**Interfaces:**
- Policy v4 (key fields; exact thresholds go into the JSON and the doc):
  ```json
  {
    "policy_id": "r4-qualification-v4",
    "primary_cell": {"variant": "rank_normal", "horizon_months": 3},
    "gating_family": "primary_cells_over_eligible_hypotheses",
    "reported_family": "all_cells",
    "investable_slice": {"min_price_usd": 5.0, "min_me_percentile_nyse": 20},
    "replication_gate": {"one_sided_p_max": 0.05, "bh_q": 0.10, "investable_one_sided_p_max": 0.05,
                         "size_bucket_sign_agreement": true},
    "discovery_gate": {"hlz_abs_z_min": 3.0, "dsr_min": 0.95, "bh_q": 0.05, "investable_one_sided_p_max": 0.05},
    "holdout": {"rule": "sign_consistent_and_no_significant_shrink", "shrink_test_one_sided_p_min": 0.05,
                "opened_once_per_wave": true, "requires_final_labels": true},
    "coverage": {"against": "catalog_population", "min_share": 0.60, "min_formation_share": 0.90},
    "long_horizon": {"method": "jegadeesh_titman_calendar_time", "holding_months": [6, 12]},
    "split": {"selection_end": "2023-12", "holdout_start": "2024-01"},
    "inference": "ewc_fixed_b",
    "trial_registry": "cumulative_across_waves"
  }
  ```
- `trial_registry.py`: `register_wave(wave: str, catalog_digest: str, policy_sha: str, feature_ids: Sequence[str],
  cells: Sequence[tuple[str, str, int]]) -> str` (append-only JSONL under `data/research/registry/`, refuses re-register
  with a different digest); `trials_so_far() -> int` (feeds BH family bookkeeping and DSR `n_trials`);
  `open_holdout(wave: str) -> None` (refuses a second open).

- [ ] **Step 1:** Implement gating per the JSON; keep EWC fixed-b as the inference core (`research/stats.py`).
- [ ] **Step 2:** Implement the JT calendar-time portfolio (month-t return = average of the K cohorts formed at
  t−1…t−K) in `evaluation.py`; this is the long-horizon evidence.
- [ ] **Step 3 (power study):** `scripts/research_power_study.py`: ≥ 400 null runs × 200 null features at T = 130
  (selection) and 30 (holdout); planted-signal power curve for ICIR₃ₘ ∈ {0.2, 0.3, …, 0.8}. Publish the table (the
  review's approximations: HLZ path needs ICIR₃ₘ ≈ 0.60, BH + |z| ≥ 2 ≈ 0.35; the old holdout gate ≈ 0.65).
- [ ] **Step 4:** Freeze v4 with the existing freeze mechanism (R3c/R4 `--policy` registration); record its sha.
- [ ] **Step 5:** Commit.

**Acceptance:** null false-qualification rate ≤ nominal for both evidence classes; power curve published; v4 frozen
and registered before any real return is read by 1.13.

---

### Task 11: [1.11] Canonical XNYS session calendar and the entry-clock rule

**Lane:** R · **Tokens:** PANEL · **Depends on:** 0.15 · **Peak target:** 0.3 GiB

**Files:**
- Modify: `src/atx_db/calendar.py:43-53` (rule-based session table replaces `DISTINCT trade_date`)
- Modify: `src/atx_db/research/panel.py:493-535` (import `nyse_full_day_closures` / `xnys_sessions` from `calendar.py`)
- Create: `docs/methodology/CLOCKS.md`

**Interfaces:**
- Produces in `calendar.py`:
  ```python
  def xnys_sessions(start: date, end: date) -> list[date]: ...          # regular + special full-day closures
  def xnys_early_closes(start: date, end: date) -> dict[date, time]: ... # 13:00 ET days
  def next_session(d: date) -> date: ...
  def decision_cutoff_utc(d: date) -> datetime: ...                     # d 22:00 UTC (house rule)
  def reconcile_sessions_with_bars(bar_dates: Iterable[date], start: date, end: date) -> dict[str, list[date]]:
      """{'missing': sessions without bars, 'extra': bar dates that are closures/weekends}"""
  ```
  Special closures include at least 2012-10-29, 2012-10-30, 2018-12-05, 2025-01-09 (plus 2007-01-02 and 2004-06-11 for
  pre-2012 extensions).
- Rule documented in `CLOCKS.md`: "a feature dated t is known at t 22:00 UTC; positions enter at the close of the next
  session; FC1 fundamentals are usable at SEC filing date + 46 h".

- [ ] **Step 1:** Implement; move the panel's calendar functions; keep panel behavior identical (panel QUERY_VERSION
  unchanged if outputs are identical — verify on the panel fixture).
- [ ] **Step 2 (real):** reconcile against the distinct `trade_date`s of `TickerHistory3.parquet` (2012-03-26 → 2026-09-18).
- [ ] **Step 3:** Commit. (The critical DQC "calendar reconciliation" is registered in 5.1; `_forward_return_publication`
  switches in 3.5; `identity_reconstruction.py:1317` switches in 3.3.)

**Acceptance:** 0 missing sessions; every extra bar date listed with its row count (stray bars on closures); early
closes listed.

---

### Task 12: [1.12] Price wave inputs — session-calendar natives, monthly spine and provisional labels (no returns read)

**Lane:** R · **Tokens:** PANEL, CAT · **Depends on:** 0.10, 1.9, 1.11 · **Peak target:** 0.6 GiB per year pass

**Files:**
- Create: `src/atx_db/research/price_natives.py`, `src/atx_db/research/spine.py`
- Modify: `src/atx_db/research/panel.py` (`NATIVE_FEATURES` registers the new natives; seed-metric price rows stay)
- Modify: `src/atx_db/seeds/research_anomaly_catalog.csv` (wave `w1_price` rows; CAT)

**Interfaces:**
- Input: `TickerHistory3.parquet` read directly (no warehouse), with: VA1 artifact repair applied to adjusted-close
  returns (`_vendor_artifact` helper); A9 unit rule for vendor shares; sessions from `xnys_sessions` (stray closure bars
  dropped); P8/R1d events are not needed (returns come from repaired adjusted close).
- `spine.py` produces the lake dataset `spine_monthly` (JKP layout): `eom, line_id, price, shares_lagged,
  me_line, ret_1m, dollar_volume_21d, n_sessions_21d, universe_basis, size_grp`, where
  `shares_lagged` = vendor shares lagged 90 calendar days (A8 domestic modeled lag; label
  `me_basis=vendor_shares_lag90`) and the universe is lines classified common by the A2 `security_type` classifier
  applied to the line's name/ticker history (label `universe_basis=name_pattern_reconstructed`); ETFs, funds, units,
  warrants, preferreds excluded.
- `label_matrix` (provisional): h ∈ {1, 3, 6, 12} months; entry at the close of the first session after `eom`; exit at
  the close of the h-th month end's next session; total return from repaired adjusted close; a line that stops trading
  inside the window gets `ret = NULL, reason = terminal_pending` (counted, never imputed); `label_sha` stamped
  `provisional=true`.
- `price_natives.py`: one DuckDB window pass per calendar year (≈ 2.5M bars) emitting month-end rows only:

| feature_id | definition | min obs | sign | theme |
|---|---|---|---|---|
| `ret_12_1` | Π(1+r) months t−12…t−2 − 1 (session-based) | 200 sessions | + | momentum |
| `ret_6_1`, `ret_9_1` | months t−6…t−2 / t−9…t−2 | 100 / 150 | + | momentum |
| `ret_12_7` | months t−12…t−7 | 100 | + | momentum |
| `ret_36_13`, `ret_60_13` | long-term reversal: months t−36…t−13 / t−60…t−13 | 400 / 800 | − | investment (JKP places `ret_60_12` there) |
| `chmom` | ret_6_1(t) − ret_6_1(t−6) | 200 | − | momentum |
| `frog_in_pan` | sign(ret_12_1) × (%neg days − %pos days) over t−12…t−2 | 200 | − | momentum |
| `seas_1_1an` | same-calendar-month return 12 months ago | 15 | + | seasonality |
| `seas_2_5an` | mean same-month return, years 2–5 | 3 of 4 | + | seasonality |
| `ret_1_0` | last month return | 15 | − | short_term_reversal |
| `rvol_21d`, `rvol_252d` | stdev daily log returns | 15 / 200 | − | low_risk |
| `rmax5_21d`, `rmax1_21d` | mean of top-5 / max daily return, 21 sessions | 15 | − | low_risk |
| `rskew_252d` | skewness of daily returns | 200 | − | low_risk |
| `beta_ew_252d`, `ivol_ew_252d`, `ivol_ew_21d` | CAPM on the bar-only EW market (P3 sealed EW market convention) | 200 / 15 | − | low_risk |
| `beta_dimson_252d` | Σ of betas on market t−1, t, t+1 | 200 | − | low_risk |
| `beta_down_252d` | beta on days market < 0 | 100 | − | low_risk |
| `coskew_252d` | Harvey–Siddique coskewness | 200 | − | low_risk |
| `beta_bab_1260d` | ρ(3-day returns, 5y) × σ_i/σ_m (1y) | 750 | − | low_risk |
| `zero_trade_21d`, `zero_trade_252d` | share of sessions with zero volume | 15 / 200 | + | size (illiquidity) |
| `turnover_126d`, `turnover_252d` | mean volume / shares_lagged | 100 / 200 | − | low_risk |
| `std_turn_126d`, `std_dvol_126d` | stdev turnover / dollar volume | 100 | − | low_risk |
| `ami_126d`, `ami_252d` | mean |r| / dollar volume | 100 / 200 | + | size |
| `bidask_cs_21d` | Corwin–Schultz high-low spread | 15 | + | size |
| `bidask_ar_21d` | Abdi–Ranaldo close-high-low spread | 15 | + | size |
| `prc_log` | log(price) | 1 | − | size |
| `prc_highprc_252d` | price / 252-session high | 200 | + | momentum |
| `me_line_log` | log(me_line) | 1 | − | size |
| `dolvol_126d` | log mean dollar volume | 100 | − | size |
| `price_delay_52w` | Hou–Moskowitz D1 on weekly returns vs EW market | 40 weeks | + | low_risk |

- [ ] **Step 1:** Implement `spine.py` and the provisional label builder (in `label_matrix.py` from 1.9).
- [ ] **Step 2:** Implement `price_natives.py` (one blocking window operator per statement; per-year pass with a
  lookback prefix of 1,260 sessions for the 5-year windows; month-end rows only).
- [ ] **Step 3:** Add wave `w1_price` catalog rows (feature ids above, `evidence_class=replication` with the cited sign,
  `population=all`, `jkp_theme`, `publication_year`); record the new catalog digest.
- [ ] **Step 4 (real, no returns read):** build natives and spine for 2012-04 → 2026-08 into the lake/feature store;
  coverage table per feature × year. Labels are built but **not joined to features** in this task.
- [ ] **Step 5:** Commit.

**Acceptance:** each native ≥ 4,500 lines/month for 2014–2025 (from 2015 for 3-year and 2017 for 5-year windows);
per-year pass ≤ 0.6 GiB; label coverage and `terminal_pending` counts by year reported.

---

### Task 13: [1.13] Price wave evaluation on real data (selection sample only) — first real results

**Lane:** R · **Tokens:** EVAL (read), CAT (freeze) · **Depends on:** 1.10, 1.12 · **Peak target:** 0.6 GiB × 2 workers

**Files:**
- Create: `docs/research/WAVE1_PRICE_PROVISIONAL.md`, `scripts/research_run_wave.py`

**Interfaces:**
- Consumes: policy v4 sha; `register_wave("w1_price", catalog_digest, policy_sha, feature_ids, cells)`; the feature
  store; provisional labels.
- Produces: evaluation cells in `data/research/eval/` and a provisional ledger (`grade_basis=provisional_labels`,
  holdout sealed).

- [ ] **Step 1:** `register_wave` **before** any label join (refuse to run otherwise).
- [ ] **Step 2:** `scripts/research_run_wave.py --wave w1_price --formations 2013-01..2023-12 --workers 2`: per feature,
  the existing R3b metrics (rank IC/ICIR, EW/VW deciles, FM with size/value/momentum controls where available, decay,
  turnover) plus the v4 investable slice and JT K = 6, 12.
- [ ] **Step 3:** Write `WAVE1_PRICE_PROVISIONAL.md`: per-feature table (coverage, mean IC, ICIR, EWC z, investable IC,
  decile L/S EW and VW with t, JT-12 t, turnover), sorted by theme, with the provisional-label caveat and the
  `terminal_pending` share per year.
- [ ] **Step 4:** Commit the doc and script.

**Acceptance:** every `w1_price` feature has a cell for h ∈ {1,3,6,12} over the selection sample; holdout formations
(2024-01 →) untouched (trial registry shows the holdout closed); workers ≤ 0.6 GiB each.
