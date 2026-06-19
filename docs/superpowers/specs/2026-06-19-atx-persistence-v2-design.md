# atx-engine Persistence Layer v2 — Design Spec

**Date:** 2026-06-19
**Status:** Approved (design); pending implementation plan
**Scope:** On-disk persistence for the alpha lifecycle — DSL pipeline generation, backtest runs, portfolio management, run fingerprinting, per-alpha event history, and Dev/UAT/PROD environments.

---

## 1. Goals

Track *everything* about an alpha's life on disk:

1. **Identity & lineage** — what an alpha is, and how it was built (parents, mutation op, seed).
2. **Runs** — every backtest/research/combine/validate run, with the inputs that defined it (universe, fitting window, data snapshot, seed, cost model, position mode).
3. **Run fingerprints** — content hash of all run inputs → detect replays, guarantee reproducibility.
4. **Alpha ↔ run link** — which run produced/evaluated/admitted/combined/promoted which alpha, and the metrics at that run.
5. **Event history** — append-only timeline of *every* change to *any* alpha over time.
6. **Environments** — isolated Dev/UAT/PROD with a single-identity promotion model.

## 2. Decisions (locked)

| Fork | Decision |
|---|---|
| Env topology | **One SQLite file per environment** (`atx_dev.sqlite`, `atx_uat.sqlite`, `atx_prod.sqlite`). |
| Heavy time-series (PnL, per-instrument weights) | **Stay in binary mmap segments.** DB indexes them by `(content_hash, segment_id, dir_index)`; DB never stores big arrays. |
| Promotion model | **Single canonical identity advances stages.** One `canon_hash` per alpha; promotion is a recorded event/ledger entry, not a re-derivation. |
| Migration / backward compat | **None required — greenfield v2.** v2 schema replaces v1's `catalog.sqlite`, `dedup.sqlite`, `lifecycle.db`. |
| Cross-env "where does this alpha live" | **ATTACH-on-demand** across the three env files (no shared registry file), to honor strict one-DB-per-env. Revisit if cross-env queries become hot. |

## 3. Architecture

- **SQLite = brain:** identity, lineage, runs, fingerprints, events, lifecycle, metrics, env config, segment index.
- **Binary segments = muscle:** per-period PnL and per-period-per-instrument weight arrays remain in existing mmap segments. Rows point into them.
- **Universal join key:** `canon_hash` (u64, FNV-1a structural hash; cross-run and cross-platform stable). Threads alpha → lineage → runs → events → lifecycle → promotion → segment, and is the stable cross-DB key for promotion between env files.
- **DB access:** opened via existing `atx/core/db/sqlite.hpp` (`SQLITE_THREADSAFE=2`, one `Database` per thread). WAL mode. Timestamps are *passed in* by the caller (no wall-clock reads inside the engine) to keep runs reproducible/replayable.

### Module layout (new `include/atx/engine/store/`, sibling of `library/`)

One file per responsibility; each unit testable in isolation against `:memory:`:

| Unit | Job | Depends on |
|---|---|---|
| `db.hpp` | per-env open, WAL/pragma setup, migration runner | `atx/core/db/sqlite.hpp` |
| `schema.hpp` | DDL + `schema_meta` versioning | `db` |
| `fingerprint.hpp` | compute `run_fingerprint`; replay detection | content hashes |
| `run_recorder.hpp` | RAII: begin run → log inputs/events → commit/abort | `db`, `event_log`, `fingerprint` |
| `alpha_catalog.hpp` | upsert identity + lineage (replaces dedup) | `db` |
| `event_log.hpp` | append `alpha_event` + lifecycle projection | `db` |
| `universe_registry.hpp` | named, point-in-time universe defs | `db` |
| `segment_index.hpp` | map alpha → segment location (replaces catalog) | `db` |
| `env_config.hpp` | this env's cost/capacity/exec config | `db` |
| `promotion.hpp` | cross-env promote Dev→UAT→PROD | `db`, `event_log` |

## 4. Schema

All `INTEGER` hash columns hold u64 values (SQLite stores as signed 64-bit; bit pattern preserved).

### A. Identity & lineage
```sql
alpha(
  canon_hash      INTEGER PRIMARY KEY,   -- u64, cross-run stable
  alpha_id        INTEGER UNIQUE,        -- u32 pool index
  expr_source     TEXT NOT NULL,         -- unparsed DSL
  created_at      INTEGER NOT NULL,      -- epoch ms, caller-supplied
  first_run_id    TEXT NOT NULL          -- run that minted it
);
alpha_lineage(                           -- "how was it built" genealogy
  child_hash   INTEGER NOT NULL,
  parent_hash  INTEGER NOT NULL,
  mutation_op  INTEGER NOT NULL,         -- u16 op code
  seed         INTEGER NOT NULL,         -- rng seed of the edit
  PRIMARY KEY(child_hash, parent_hash)
);
```
`alpha.canon_hash` UNIQUE *is* the dedup index — `dedup.sqlite` removed.

### B. Universe & data snapshots ("what universe / what data")
```sql
universe(
  universe_id   TEXT PRIMARY KEY,        -- content hash of member set + rule
  name          TEXT, as_of INTEGER,
  rule          TEXT,                    -- optional generating rule
  content_hash  INTEGER NOT NULL
);
universe_member(universe_id TEXT, instrument_id INTEGER, PRIMARY KEY(universe_id, instrument_id));
data_snapshot(                           -- PIT data version (e.g. ORATS manifest)
  snapshot_id   TEXT PRIMARY KEY,
  source        TEXT, as_of INTEGER, content_hash INTEGER
);
```

### C. Runs & fingerprints (central new entity)
```sql
run(
  run_id          TEXT PRIMARY KEY,      -- ULID-style id, time component caller-supplied
  run_fingerprint INTEGER UNIQUE,        -- hash(engine_sha,config,universe,snapshot,seed,gates)
  kind            TEXT NOT NULL,         -- research|backtest|combine|validate
  status          TEXT NOT NULL,         -- running|committed|aborted
  engine_git_sha  TEXT, master_seed INTEGER,
  universe_id     TEXT REFERENCES universe,
  snapshot_id     TEXT REFERENCES data_snapshot,
  fit_start INTEGER, fit_end INTEGER,    -- fitting window
  bt_start  INTEGER, bt_end  INTEGER,    -- backtest range
  position_mode TEXT, sector_neutral INTEGER,
  rebalance_every INTEGER, cost_model TEXT,
  manifest_version_id INTEGER, result_digest INTEGER,
  started_at INTEGER, finished_at INTEGER
);
run_param(run_id TEXT, key TEXT, value TEXT, PRIMARY KEY(run_id,key));  -- long-tail config
run_alpha(                               -- alpha <-> run link
  run_id     TEXT, canon_hash INTEGER,
  role       TEXT NOT NULL,              -- mined|evaluated|admitted|combined|promoted
  PRIMARY KEY(run_id, canon_hash)
);
```
`run_fingerprint` UNIQUE → relaunching identical inputs collides → replay detected, recompute skipped.

### D. Evaluation & metrics (per alpha *per run* — metrics evolve)
```sql
alpha_metrics(run_id TEXT, canon_hash INTEGER,
  sharpe REAL, returns REAL, drawdown REAL, turnover REAL, margin REAL, fitness REAL,
  PRIMARY KEY(run_id, canon_hash));
eval_fold(run_id TEXT, canon_hash INTEGER, fold_id INTEGER, sharpe REAL, returns REAL, n_test INTEGER,
  PRIMARY KEY(run_id, canon_hash, fold_id));     -- CPCV folds (or pointer to FoldResult segment)
conviction(run_id TEXT, canon_hash INTEGER,
  dsr REAL, pbo REAL, stability REAL, explain_flag INTEGER, score REAL,
  PRIMARY KEY(run_id, canon_hash));
```

### E. Lifecycle & the general event table ("changes to any alpha over time")
```sql
alpha_event(                             -- append-only superset audit log
  event_id   INTEGER PRIMARY KEY AUTOINCREMENT,
  ts         INTEGER NOT NULL,           -- caller-supplied
  canon_hash INTEGER NOT NULL,
  event_type TEXT NOT NULL,              -- created|evaluated|metric_changed|lifecycle|
                                         -- promoted|demoted|retired|reparented|gate_pass|gate_fail|annotated
  run_id     TEXT,                       -- nullable (manual events)
  actor      TEXT NOT NULL,              -- system|run|user
  payload    TEXT                        -- JSON detail (old/new values, gate name, note)
);
lifecycle_journal(                       -- typed PIT projection (fast state-as-of queries)
  seq INTEGER PRIMARY KEY AUTOINCREMENT,
  canon_hash INTEGER, from_state INTEGER, to_state INTEGER,
  as_of_period INTEGER, run_id TEXT
);
```
Every state change writes **both** rows in one transaction: `lifecycle_journal` (preserves v1's fast `state_as_of` query) and `alpha_event` (unified timeline). Single write path → no drift. Lifecycle states unchanged from v1: Candidate→Admitted→Live→Decaying→{Live,Dead}→Recycled.

### F. Environment & promotion
```sql
env_config(                              -- describes THIS db's environment
  key TEXT PRIMARY KEY, value TEXT       -- cost_knobs, capacity_curve, exec_model, rebalance
);
promotion_ledger(                        -- cross-env promotions landing in this env
  promo_id   INTEGER PRIMARY KEY AUTOINCREMENT,
  canon_hash INTEGER NOT NULL,
  from_env   TEXT, to_env TEXT,
  justifying_run_id TEXT, approved_by TEXT, ts INTEGER
);
```

### G. Segment index (replaces `catalog.sqlite`)
```sql
segment(segment_id TEXT PRIMARY KEY, path TEXT, content_hash INTEGER,
  base_alpha_id INTEGER, n_alphas INTEGER, crc INTEGER, format_version INTEGER, created_by_run_id TEXT);
segment_alpha(canon_hash INTEGER, segment_id TEXT, dir_index INTEGER,
  PRIMARY KEY(canon_hash, segment_id));  -- locate PnL/position arrays
```

### H. Versioning
```sql
schema_meta(schema_version INTEGER, engine_version TEXT, applied_at INTEGER);
```

## 5. Key flows

- **Run write path:** `RunRecorder` opens with `status=running` and fingerprint computed up front. If fingerprint already `committed` → abort as replay. Else write `run`, `run_param`, link `run_alpha`, write per-alpha `alpha_metrics`/`eval_fold`/`conviction`, emit `alpha_event(evaluated)`. A single `COMMIT` flips `status=committed`. A crash leaves the row `running` → GC reclaims.
- **"How was alpha X built?"** — one join: `alpha` ⨝ `alpha_lineage` (parents/mutation/seed) ⨝ `run_alpha`/`run` (universe, fit window, snapshot, seed, cost model, position mode).
- **"What changed on alpha X over time?"** — `SELECT * FROM alpha_event WHERE canon_hash=? ORDER BY ts, event_id`. Timeline: created → evaluated → metric deltas → lifecycle moves → promotions.
- **Promotion Dev→UAT:** copy `alpha` + `alpha_lineage` + justifying `run` rows into `atx_uat.sqlite`; insert `promotion_ledger`; emit `alpha_event(promoted)` in both DBs. `canon_hash` keeps identity stable across files. Cross-env reads via `ATTACH`.
- **Replay / reproducibility:** `run_fingerprint = hash(engine_git_sha, normalized_config, universe.content_hash, snapshot.content_hash, master_seed, gate_config)`. Equal fingerprint ⇒ identical inputs.

## 6. Determinism, crash safety, concurrency

- Append-only `alpha_event` + `lifecycle_journal` with `AUTOINCREMENT seq` tie-break — matches v1's determinism contract.
- Content hashes for all identity/integrity (universe, snapshot, segment) → dedup and tamper detection for free.
- WAL + `run.status` makes partial runs detectable; orphan segments (a `content_hash` not referenced in `segment`) are GC-able.
- One writer per run; readers concurrent under WAL. No wall-clock reads in engine — timestamps caller-supplied.

## 7. Testing strategy

- `:memory:` SQLite per-unit tests (db, catalog, event_log, fingerprint, promotion, universe_registry, segment_index).
- Golden schema snapshot test (DDL diff guard against accidental schema drift).
- Property test: replay `alpha_event` stream → reconstruct lifecycle state, assert equals `lifecycle_journal` projection.
- Fingerprint tests: identical inputs → identical fingerprint; any single perturbed input → different fingerprint.
- Promotion round-trip test across two attached `:memory:` DBs.

## 8. Out of scope (YAGNI)

- Shared cross-env registry file (using ATTACH-on-demand instead).
- Moving heavy PnL/weight arrays into SQLite.
- Migrating/maintaining v1 store files.
- Multi-process write coordination beyond SQLite WAL single-writer.
