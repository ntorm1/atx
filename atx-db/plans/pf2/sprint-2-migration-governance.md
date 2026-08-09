# Sprint PF2-S2 — Migration governance + backup/checkpoint/DR

**Goal:** turn the migration runner from an honest-but-unguarded loop into a governed apply path — make the append-only invariant real (checksum-enforced), add an advisory apply-lock, and stand up the missing backup/CHECKPOINT/verify/restore surface so the next WAL-replay crash is a scripted recovery, not a hand-made `.bak`. Reserved migrations 0100-0102.

**Mandate / Owns:** NEW `db/migration_admin.py` (backup/checkpoint/restore + `.bak` retention), `db/migrations.py` runner hardening (checksum write+verify + advisory apply-lock), NEW `scripts/warehouse_migrate.py`; `db/tests/test_migration_governance.py`.

**Must NOT touch:** the migration *bodies* — no landed `up()` function is edited or renumbered (S2-0 exists to make editing one *fail a check*). The schema contract itself is PF2-S1's (`db/schema_contract.py`); this sprint *calls* its drift check to verify post-apply, it does not author it. Content tables (`fundamental_*`, `xbrl_*`) are read-only here.

**Depends on:** the pf1 migration framework (`Migration` dataclass, `MIGRATIONS`, `apply_pending_migrations`) and **PF2-S1's schema contract** — S2-1's post-apply verify runs the S1 drift check against the live schema. Sequential after PF2-S1 (S1 lands 0097-0099; this sprint takes 0100-0102). Reconcile to S1's landed `schema_contract` symbol names at implementation time.

---

## Baseline / where the cycles go

The migration framework is disciplined but the governance around it is entirely tribal. Measured 2026-07-03 against `db/migrations.py`, `db/connection.py`, `db/schema.py`, and the `db/` directory.

1. **The `checksum` column exists and is never written — the append-only invariant is unenforced.** `schema.py` creates `schema_migrations (version VARCHAR PRIMARY KEY, description VARCHAR NOT NULL, checksum VARCHAR, applied_at TIMESTAMP …)`, but `apply_pending_migrations` inserts only `(version, description, applied_at)` via `INSERT INTO schema_migrations (version, description, applied_at) VALUES (?, ?, CURRENT_TIMESTAMP)` with `[str(migration.version).zfill(4), migration.name]`. Nothing hashes an `up()` body; nothing re-validates it. Editing a *landed* migration silently changes what a version "means" with zero detection — the `Migration` frozen dataclass is `(version:int, name:str, up:Callable)` and is **forward-only** (no `down`/reverse field), so there is no compensating path either.

2. **No apply-lock beyond the in-process transaction.** `apply_pending_migrations` runs each `migration.up(conn)` inside `conn.execute("BEGIN TRANSACTION")` … `COMMIT` with `ROLLBACK` on exception — correct per-migration atomicity, but nothing guards two *processes/agents* both entering the loop. Migrations mix DDL with **data repair** (v83 `_repair_identifier_spine_self_overlaps_s5`), so a concurrent apply is not merely wasteful, it can double-apply a repair. `_schema_is_current` short-circuits `initialize()` when `max(schema_migrations.version) >= max(m.version for m in MIGRATIONS)`, which means a tampered landed body on an already-current DB is never re-examined at all.

3. **Zero backup/CHECKPOINT/restore code, and the evidence is six unmanaged multi-GB artifacts.** There is no `CHECKPOINT`/`EXPORT`/backup/restore call anywhere in `db/*.py`. The two real 2026-06-29 WAL-replay crashes (S5g, S5k — an `ALTER TABLE` + `CREATE INDEX` in the *same* transaction triggered a DuckDB internal error on WAL replay) were recovered by hand, leaving in `db/`: `atx_impl.duckdb.pre-s5g-wal-recovery.20260629-074343.bak` (549 MB) + `…wal.failed-s5g-migration.20260629-074343.bak` (231 KB), the s5k pair (`…094756…`), `atx_impl.duckdb.pre-s39-broad-bars.bak` (731 MB), and `atx_impl.duckdb.pre-s42-fundamentals.bak` (6.4 GB). ~8.7 GB of hand-made `.bak` sits beside the live 14.1 GB DB with no registry and no retention policy.

4. **Migration tests assert a hand-maintained version list, not `MIGRATIONS`.** `test_migrations.py::test_migrations_recorded_after_bootstrap` hard-codes `assert 1 in versions … assert 52 in versions` — a literal ladder that stops at 52 and is not derived from `MIGRATIONS` (which now tops out at v88 `xbrl_validation_dimensional_evidence`, non-contiguous with gaps reserved during planning). New migrations are not auto-covered, and there is **no forward-migration-on-populated-DB test** (every test bootstraps a fresh `tmp_store`), so the one scenario that actually crashed in production — applying new DDL over a large existing DB — is untested.

**Already good — do not regress:**
- **Per-migration atomicity.** The `BEGIN TRANSACTION` / `up()` / `COMMIT` with `ROLLBACK`-on-exception loop, and the `version ~ '^[0-9]+$'` applied-set read, stay exactly as they are. Governance wraps this loop; it does not rewrite it.
- **The WAL-split convention.** Schema and index ship as *separate* migration numbers (realized pairs 17/18, 19/20, 23/24, 25/26, 65/66, 75/76, 79/80) — the code-level lesson of S5g/S5k. This sprint keeps its own 0100/0101 as a schema/index pair and codifies the rule, never relaxes it.
- **`_schema_is_current` fast-path.** The head-version short-circuit that saves the ~1-2s idempotent rebuild on hot loaders stays. S2-0 only ensures the checksum verify has an always-run entry point (the migrate script) that the fast-path does not swallow.
- **`_configure_session`'s absolute `temp_directory`.** The `SET temp_directory = ?` spill-dir fix stays untouched; backups must not clobber the `.{name}.duckdb_tmp` dir.

---

## PIT / determinism + production contract

ROADMAP clauses **(B)** append-only catalogued migrations, **(C)** offline/no-network tests, and **(F)** backup-before-migrate apply in full — **(F) is *defined* by this sprint.**

- **(B)** Migrations **0100-0102 only**; never renumber or edit a landed body (S2-0 makes that a hard failure). Split schema from index across numbers (0100 schema, 0101 index — the S5g/S5k precedent). Every new table/column seeds `table_catalog` + `field_catalog` in the same migration.
- **(C)** All tests run against in-memory / template-copy DuckDB with fixture rows and a temp backup dir under `tmp_path`. No network. Backup/restore round-trips run on a small template DB in pytest; the live 14.1 GB CHECKPOINT+backup is operator-run and recorded in the ledger.
- **(F)** Every live apply is preceded by a scripted `CHECKPOINT` + timestamped DB+WAL backup and followed by a verify against the PF2-S1 contract; the WAL-split discipline and the S5g/S5k recovery runbook become documented tooling, not tribal knowledge.

---

## Tasks

### S2-0 — Checksum-enforced append-only invariant + advisory apply-lock *(the big one)*

**Root cause:** `apply_pending_migrations` never writes the `checksum` column and never re-validates a landed body, and there is no lock guarding concurrent entry to the apply loop. An edited landed `up()` is undetectable; a concurrent apply can double-run a data-repair migration like `_repair_identifier_spine_self_overlaps_s5`.

**Fix:** in `migrations.py` add `_migration_source_checksum(m)` (sha256 over `inspect.getsource(m.up)`, normalized) and `verify_migration_checksums(conn)` (the append-only invariant: for every applied version, the stored `checksum` must equal the current source hash — mismatch ⇒ raise, naming the tampered version). Extend `apply_pending_migrations` to write the checksum in the tracking INSERT and to run `verify_migration_checksums` before applying pending work. Add an `acquire_apply_lock(conn, run_id)` / `release_apply_lock(conn)` context manager backed by a `migration_apply_lock` sentinel row (holder run_id + heartbeat) so a second apply aborts fast with a clear message instead of a file-lock stack trace. Migration **0100** `_migration_governance_schema` creates `migration_apply_lock` + backfills `checksum` for already-landed rows; **0101** `_migration_governance_indexes` adds their indexes (schema/index split). Wire `verify_migration_checksums` as an always-run step in `scripts/warehouse_migrate.py` so `_schema_is_current`'s fast-path cannot swallow tamper detection.

**PIT:** (B) 0100/0101 catalogued, schema/index split. (C) checksum + lock tested on in-memory DB.

**Accept:** editing any landed `up()` body makes `verify_migration_checksums` (and a fresh apply) fail, naming the version; a bootstrapped DB has a non-null `checksum` on every `schema_migrations` row; a second concurrent `apply_pending_migrations` raises the lock error rather than double-applying; re-apply on a clean DB is still `[]`.

### S2-1 — Scripted pre-flight CHECKPOINT + timestamped backup + post-apply verify + restore

**Root cause:** there is no backup/CHECKPOINT/restore code; clause (F) is undefined and every prior backup was hand-made.

**Fix:** NEW `db/migration_admin.py` with pure, injectable functions: `checkpoint(conn)` (issues `CHECKPOINT`), `backup_database(db_path, label)` (copies the `.duckdb` **and** its `.wal` to `…{label}.<UTC-stamp>.bak` beside the DB, matching the existing `pre-sNN-…` naming and returning the paths), `verify_schema(conn)` (delegates to the PF2-S1 `schema_contract` drift check — post-migration schema must match the contract), and `restore_database(bak_path, target)`. NEW `scripts/warehouse_migrate.py` (sibling of `scripts/build_quant_warehouse.py`) orchestrates the governed apply: acquire lock → `checkpoint` → `backup_database` → `apply_pending_migrations` → `verify_schema`; on verify failure, `restore_database` from the pre-flight `.bak` and exit non-zero. Migration **0100** also creates a `migration_backup_registry` (backup path, sha256, bytes, versions_before/after, run_id, created_at) that `backup_database` writes a row into; **0102** is held as documented in-range headroom (retention-policy catalog rows / a registry view), keeping the sprint strictly within 0100-0102.

**PIT:** (F) apply is backup-guarded and verify-gated. (B) registry catalogued in 0100. (C) round-trip on a template DB under `tmp_path`.

**Accept:** `scripts/warehouse_migrate.py` on a template DB produces a timestamped DB+WAL `.bak` + a `migration_backup_registry` row, applies pending migrations, and passes `verify_schema`; a deliberately drift-injected schema makes verify fail and triggers `restore_database` back to the known-good state; live smoke (operator) CHECKPOINTs + backs up the 14.1 GB DB and records counts + run_id in the ledger.

### S2-2 — Codified S5g/S5k WAL-recovery runbook + `.bak` retention policy

**Root cause:** the S5g/S5k recovery (restore the pre-migration `.bak`, discard the failed `.wal`, re-apply with schema/index split) lives only in operator memory, and ~8.7 GB of `.bak` (down to the 6.4 GB `pre-s42-fundamentals.bak`) sits unmanaged with no policy.

**Fix:** in `db/migration_admin.py` add `recover_from_wal_failure(db_path, backup_path)` — the S5g/S5k routine as executable, documented tooling (restore pre-migration `.bak`, drop the `…wal.failed-*` file, re-run `apply_pending_migrations` under the WAL-split discipline) — and `enforce_backup_retention(dir, keep_latest, min_age)` / `prune_backups(...)` implementing a `.bak` retention policy that registers survivors in `migration_backup_registry` and never deletes an un-registered or in-flight backup. Document (inline + this file's runbook section) the exact ALTER-then-CREATE-INDEX-in-separate-migrations rule that prevents the crash class.

**PIT:** (F) recovery + retention are the standing invariant. (C) retention tested against fabricated `.bak` fixtures in a temp dir; no real 6.4 GB file touched in pytest.

**Accept:** `recover_from_wal_failure` reproduces a known-good state from a fixture pre/post pair; `enforce_backup_retention` prunes to policy on fabricated `.bak` files while refusing to delete un-registered ones; the runbook names the S5g/S5k artifacts and the schema/index-split rule.

### S2-3 — MIGRATIONS-derived migration tests + forward-on-populated-DB test

**Root cause:** `test_migrations.py` asserts a hand-maintained literal ladder (`assert 1…assert 52`) not derived from `MIGRATIONS`, so v88 and beyond are uncovered, and no test applies new DDL over a populated DB — the exact shape that crashed as S5g/S5k.

**Fix:** in NEW `db/tests/test_migration_governance.py`, derive coverage from the source of truth: assert every `m.version for m in MIGRATIONS` is recorded post-bootstrap, versions are unique and applied in sorted order, and each carries a non-null `checksum`. Add a **forward-migration-on-populated-DB** test: build a template DB seeded to an *older* head, insert representative content rows, then run the governed apply and assert new migrations land, existing rows survive, `verify_migration_checksums` passes, and `verify_schema` matches the PF2-S1 contract. Add a checksum-tamper test (monkeypatch one `up()` ⇒ verify raises), an advisory-lock re-entrancy test, and a `backup_database`→`restore_database` round-trip test.

**PIT:** (C) all in-memory / template-copy, no network. (B) tests assert the catalog rows for 0100-0102.

**Accept:** adding a new `MIGRATIONS` entry needs no test edit to be covered; the populated-DB forward test is green; tamper/lock/backup-restore tests each red on the fault and green on the fix.

---

## Sequencing & expected compounding

**S2-0 → S2-1 → S2-2 → S2-3.** S2-0 is load-bearing: checksum enforcement + the apply-lock make the append-only invariant *real*, and every later step assumes a trustworthy, single-writer apply. S2-1 builds the backup/verify/restore surface on top of that governed apply (its verify needs PF2-S1's contract). S2-2 codifies recovery + retention using S2-1's `migration_admin` primitives and registry. S2-3 tests all three against `MIGRATIONS` and a populated DB. The compounding: once an edited landed migration *fails a check* and every apply auto-backs-up + verifies, the migration path stops being the warehouse's most dangerous operation — the content sprints (PF2-S3…S10) churn the schema on rails that catch a bad body before it lands and can restore in one command if a WAL replay fails again.

## Risks / guardrails

- **Checksum brittleness vs. real tamper.** Whitespace/comment reformatting of a landed body would trip the hash. Mitigate by normalizing source in `_migration_source_checksum` (strip trailing whitespace, normalize newlines) and documenting that any semantic edit to a landed migration is forbidden by design — the check is *meant* to fire.
- **Fast-path hides tamper.** `_schema_is_current` skips `apply_pending_migrations` on a current DB, so tamper detection must have an always-run entry point (`scripts/warehouse_migrate.py` calls `verify_migration_checksums` directly). Do not move the verify *inside* the short-circuited branch.
- **Never widen scope to "fix" a migration.** If a landed body is genuinely wrong, the remedy is a new forward migration in a later range, never an edit — the checksum check enforces this.
- **Backup safety.** `backup_database` copies both `.duckdb` and `.wal` and must run *after* `CHECKPOINT`; it must not touch the `.{name}.duckdb_tmp` spill dir. Retention never deletes an un-registered or in-flight `.bak`. Stay strictly within migrations **0100-0102**, schema/index split.

## Bench / acceptance

- Editing a landed migration **fails a check** (`verify_migration_checksums` names the version); every `schema_migrations` row carries a non-null `checksum` after bootstrap.
- A governed live apply **auto-backs-up + verifies**: `scripts/warehouse_migrate.py` produces a timestamped DB+WAL `.bak` + `migration_backup_registry` row and passes the PF2-S1 drift verify; a drift-injected schema triggers `restore_database`.
- **Migration tests are derived from `MIGRATIONS`**, not the hand-list; the forward-migration-on-populated-DB test is green.
- `python -m pytest atx-impl\db\tests\test_migration_governance.py -q` green (and the full `atx-impl\db\tests -q` suite stays green before commit).
- **Live-DB smoke** recorded in the ledger: the operator-run CHECKPOINT + 14.1 GB backup (path, bytes, sha, versions_before/after, run_id) and a restore that reproduces a known state.
- **Ledger row appended** to `WAREHOUSE_PARITY_TRANCHES.md` (start/end SHA, domains, verification commands, live-DB smoke with exact counts + run_id, caveats/next); `PARITY_GAP.md` platform-row status updated (backup/checkpoint/DR + checksum enforcement now present).

**Process:** never `git add -A` (stage explicit paths); never push unless asked. New module ⇒ new `test_*.py`. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
