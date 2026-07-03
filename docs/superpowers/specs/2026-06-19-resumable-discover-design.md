# Resumable Discover (crash-safe genetic search) — Design Spec

**Date:** 2026-06-19
**Status:** Approved (design); pending implementation plan.

**Goal:** Make the gated `discover` stage crash-resilient and resumable. A 30-minute
genetic search that crashes (OOM, kill, power loss) must resume from the last
completed generation, losing at most one generation of work — instead of losing
everything (the failure mode observed in run `bh0j2wti6`, which died ~08:11 after
~35 min and wrote nothing). All progress is persisted to the existing
`atx::engine::store` SQLite database.

**Architecture:** Three decoupled layers. (1) An abstract `SearchProgressSink` +
`SearchResumeState` in the engine `factory` module, wired into
`SearchDriver::run` as defaulted parameters — off by default, so all existing
callers are byte-identical. (2) A header-only `PipelineRecorder` + schema-v2
tables in `atx::engine::store`. (3) impl-side glue in `atx-impl` (a concrete
store-backed sink + fingerprint/open/resume logic) wired into the gated discover
stage behind two new flags.

**Tech stack:** C++20, clang-cl + Ninja, vendored SQLite amalgamation (in
`atx-core`, linked transitively), GoogleTest. Reuse `alpha::unparse` /
`alpha::parse_expr` (faithful genome round-trip), `store::fingerprint` (FNV-1a64),
`store::StoreDb` (WAL).

---

## Global Constraints (binding — every task inherits these)

- **Off-path byte-identical:** With no `--run-db`, impl opens no DB, builds no
  sink, and passes `nullptr`. Engine sink/resume params default `nullptr`. Every
  existing output (library digest, `_manifest.txt`, `.dsl` files, stage digest,
  `SearchResult.digest`) MUST be byte-identical to today. This is a tested invariant.
- **Resume correctness invariant (headline):** An uninterrupted gated discover and
  a discover interrupted after generation K then resumed produce a **byte-identical
  admitted alpha set** — the same `admitted_candidates` canon_hashes, the same `.dsl`
  files, and the same per-alpha metrics. Guaranteed by: faithful genome round-trip +
  pure `seed_for(master,gen,idx)` reproduction + deterministic evaluation + deterministic
  post-search admission (the final-generation population is reconstructed identically, so
  finalize/admission is identical). NOTE on `all_scored`: a resumed run's `all_scored`
  (the full distinct-structures-scored history) is a SUBSET of the uninterrupted run's —
  structures that were scored ONLY in generations 0..K-1 are not in the gen-K checkpoint
  population and are not re-scored. Those structures were never admitted, so the alpha DB
  is unaffected. The test asserts `admitted_candidates` multiset equality + `all_scored ⊆
  full.all_scored` (no spurious structures). NOTE: `SearchResult.digest` (and the manifest's
  `factory_digest` line, which derives from it) is a per-generation *within-run*
  fingerprint — a resumed run folds only the generations it executes in-process, so this
  ONE value may differ across a resume boundary. This does NOT affect the alpha DB
  content (the alphas + metrics are identical). The tests assert the admitted-set
  equality, not digest equality, for the resume case.
- **No new third-party dependencies.** Reuse store/fingerprint/unparse/parse_expr.
- **Determinism (F1/F2):** identical inputs ⇒ identical digest, across worker counts.
- **Store style:** the store module is header-only (all inline in `.hpp`). Keep the
  new `pipeline_progress.hpp` header-only too — **no CMake change** in engine.
- `atx-impl` already links `atx::engine` (PUBLIC) which transitively provides
  `atx::engine::store` + sqlite3 — **no CMake change** in impl.
- NEVER `git add -A`; stage explicit paths only. Local commits on the feature
  branch are authorized (trailer EXACTLY
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`). Do NOT push.

---

## Scope (locked)

- **In:** the gated discover genetic search only (the sole multi-minute / 30-min
  crash risk). It runs inside the engine: `stage_discover.cpp` (gated path) →
  `Factory::mine_into` → (`mine_into_oos` when `oos_fraction>0`) →
  `SearchDriver::run` generation loop.
- **Out (v1):** optimize/combine/panel resumability (seconds-to-minutes; the schema
  is generic enough to add them later with no migration). MultiProcess
  (`IExecutor`) checkpointing — our DB run is InProcess/sequential; the OOS path
  already runs sequentially.
- **Resume trigger:** explicit `--run-db <path>` + `--resume`. `--run-db` alone =
  record progress live, start fresh. `--resume` (requires `--run-db`) = look up the
  matching incomplete run by fingerprint and continue from the last checkpoint.

---

## Key facts (verified anchors)

- `SearchDriver` / `Genome` / `SearchConfig` / `SearchResult` live in
  `namespace atx::engine::factory` —
  [search_driver.hpp:97-475](../../../atx-engine/include/atx/engine/factory/search_driver.hpp).
- Run entry: `SearchResult run(const SearchConfig&, const combine::AlphaStore&)` —
  search_driver.hpp:286.
- The generation loop (search_driver.cpp:79-119): `pop = init_population(cfg)`
  then `for gen in 0..generations { scored = evaluate_generation(pop, cfg, gen, …);
  rank; best_fitness_per_gen.push_back(best_raw(scored)); if (gen < generations-1)
  pop = reproduce(scored, cfg, gen, …); } finalize(scored, …)`.
- `seed_for(master, gen, idx)` is a pure SplitMix mix — search_driver.hpp:183-194.
  Reproduction RNG depends ONLY on (master_seed, gen, child_index) → replaying from
  a saved generation is byte-identical.
- Faithful round-trip (the correctness lynchpin), contract in
  [serialize_genome.hpp:9](../../../atx-impl/src/serialize_genome.hpp) +
  unparse.hpp:10-24: `canonical_hash(parse_expr(unparse(ast))) == canonical_hash(ast)`.
  - genome → string: `std::string alpha::unparse(const Ast&)` (unparse.hpp:135-143).
  - string → genome: `alpha::parse_expr(src, lib)` + `factory::analyze_into(...)`
    (used by `SearchDriver::init_population`, search_driver.cpp:131-145).
  - SearchDriver owns `lib_` (search_driver.hpp:463), so serialize/deserialize of the
    population happens **inside** SearchDriver — the population crosses the sink
    boundary as plain DSL strings; the sink never needs the Library.
- Factory admit path (gated): `Factory::mine_into(cfg, lib_lib, gate)` — factory.hpp:238;
  dispatches to private `mine_into_oos(cfg, lib_lib, gate)` (factory.hpp:301) when
  `cfg.oos_fraction > 0`. Both construct/run a `SearchDriver`.
- Gated discover call site: `fac.mine_into(fcfg, liblib, gate)` — stage_discover.cpp:123.
- Store: `StoreDb::open(path)` (db.hpp:17, WAL + FK on), `fingerprint::compute(RunInputs)`
  / `fingerprint::is_replay` (fingerprint.hpp), `schema::create_all` idempotent,
  `kSchemaVersion=1` (schema.hpp:11) → bump to 2. Golden-schema drift guard test:
  `atx-engine/tests/store/store_schema_golden_test.cpp`.
- Build: store tests `atx-engine-store-tests`; search tests `atx-engine-factory-tests`;
  impl tests `atx-impl-tests`; impl exe `atx-impl`. Warm Release dir: `build-rel`
  (Ninja, `ATX_USE_PCH=OFF`, `ATX_TEST_GROUPS=all`).

---

## Component 1 — Engine sink interface (`factory`)

**New file:** `atx-engine/include/atx/engine/factory/search_progress.hpp`
(header-only, `namespace atx::engine::factory`).

```cpp
// A generation boundary snapshot handed to a progress sink. `population` is the
// already-serialized population (one canonical DSL string per genome) that is the
// INPUT to generation `generation` — i.e. the state needed to resume AT `generation`.
struct GenerationSnapshot {
  atx::usize generation{0};
  std::vector<std::string> population;   // unparse(g.ast) for each genome, canonical-order
  atx::f64 best_fitness{0.0};
  atx::f64 mean_fitness{0.0};
  atx::usize n_evaluated{0};             // distinct candidates scored so far (CanonSet size)
  atx::usize n_unique{0};                // distinct genomes in this generation's population
};

// Abstract progress sink. Default = none (nullptr). on_generation is called once per
// generation AFTER evaluation/ranking (metrics available) with the population that
// entered that generation. A non-Ok return ABORTS the search cleanly (propagated as
// the run's failure) — used both for real I/O errors and for crash-injection in tests.
class SearchProgressSink {
 public:
  virtual ~SearchProgressSink() = default;
  [[nodiscard]] virtual atx::core::Status on_generation(const GenerationSnapshot&) = 0;
};

// Resume input. When non-null and start_generation < generations, SearchDriver skips
// init_population, deserializes `population` into the gen-`start_generation` population,
// and runs the loop from `start_generation` (NOT 0). Empty population / start==0 ⇒
// behaves like a fresh run (still safe).
struct SearchResumeState {
  atx::usize start_generation{0};
  std::vector<std::string> population;
};
```

**`SearchDriver::run` signature change (search_driver.hpp:286 + .cpp):**
```cpp
[[nodiscard]] SearchResult run(const SearchConfig& cfg, const combine::AlphaStore& pool,
                               SearchProgressSink* sink = nullptr,
                               const SearchResumeState* resume = nullptr);
```
- **Resume:** if `resume && resume->start_generation > 0 && resume->start_generation
  < cfg.generations && !resume->population.empty()`, set `pop =
  deserialize_population(resume->population)` and `gen_start = resume->start_generation`;
  else `pop = init_population(cfg)` and `gen_start = 0`. Loop `for (gen = gen_start; …)`.
  All other run() state (`res`, `canon`, `fitness_cache`, `behavior_archive`,
  `engines`) initializes empty exactly as today — the resumed generations re-derive
  them deterministically. `best_fitness_per_gen` will contain only the gens actually
  run this process; that vector is telemetry, not part of the digest/admission.
- **Checkpoint:** inside the loop, after `evaluate_generation` + ranking + the
  `best_fitness_per_gen.push_back`, if `sink != nullptr` call
  `sink->on_generation({gen, serialize_population(pop), best_raw(scored), mean_raw(scored),
  canon.size(), pop.size()})`. Propagate a non-Ok return as the run result (clean abort).
  `pop` here is the unmodified generation input (evaluate scores, it does not mutate `pop`).
- **Private helpers (new, use `lib_`):**
  `std::vector<std::string> serialize_population(const std::vector<Genome>&) const;`
  (`unparse(g.ast)` each, in canonical-id order so the blob is deterministic) and
  `atx::core::Result<std::vector<Genome>> deserialize_population(const std::vector<std::string>&) const;`
  (`parse_expr(src, lib_)` + `analyze_into` each; set `canon_hash = canonical_hash(g)`).
- A new `static atx::f64 mean_raw(const std::vector<Scored>&)` helper (telemetry only).

**Determinism note:** off-path (`sink==nullptr && resume==nullptr`) the loop is the
byte-identical legacy path — the only added work is two null-pointer checks per generation.

---

## Component 2 — Store recorder + schema v2 (`atx::engine::store`)

**Schema (schema.hpp):** bump `kSchemaVersion` 1→2; add the five tables below to
`create_all` (all `CREATE TABLE IF NOT EXISTS` → idempotent; an existing v1 DB gains
them on next open). Update the golden-schema drift-guard test's expected table set
and the version assertion. Stamp logic: if a DB stamped at v1 opens, `create_all`
adds the tables; the `schema_meta` version is updated to 2 (extend the existing
stamp guard to allow an upward version bump, not just first-create).

```sql
CREATE TABLE IF NOT EXISTS pipeline_run (
  pipeline_run_id   TEXT PRIMARY KEY,
  fingerprint       INTEGER UNIQUE NOT NULL,
  stage             TEXT NOT NULL,                 -- 'discover'
  status            TEXT NOT NULL,                 -- running|completed|crashed|resumed|failed
  master_seed       INTEGER NOT NULL,
  population        INTEGER NOT NULL,
  total_generations INTEGER NOT NULL,
  last_generation   INTEGER NOT NULL DEFAULT -1,   -- last CHECKPOINTED gen (-1 = none)
  panel_path        TEXT,
  config_json       TEXT,                          -- normalized resumable inputs (audit)
  engine_git_sha    TEXT,
  created_at        INTEGER NOT NULL,
  updated_at        INTEGER NOT NULL,
  last_heartbeat_at INTEGER NOT NULL,
  finished_at       INTEGER
);
CREATE TABLE IF NOT EXISTS pipeline_checkpoint (   -- PROGRESSIVE resumable state
  pipeline_run_id  TEXT NOT NULL,
  generation       INTEGER NOT NULL,
  population_blob  TEXT NOT NULL,                   -- '\n'-joined canonical DSL exprs
  population_count INTEGER NOT NULL,
  state_hash       INTEGER NOT NULL,                -- FNV-1a64(population_blob) integrity
  created_at       INTEGER NOT NULL,
  PRIMARY KEY (pipeline_run_id, generation)
);
CREATE TABLE IF NOT EXISTS pipeline_iteration (    -- ITERATION metrics timeseries
  pipeline_run_id  TEXT NOT NULL,
  generation       INTEGER NOT NULL,
  best_fitness     REAL,
  mean_fitness     REAL,
  n_evaluated      INTEGER,
  n_unique         INTEGER,
  wall_ms          INTEGER,
  ts               INTEGER NOT NULL,
  PRIMARY KEY (pipeline_run_id, generation)
);
CREATE TABLE IF NOT EXISTS pipeline_event (        -- append-only EVENTS
  event_id        INTEGER PRIMARY KEY AUTOINCREMENT,
  pipeline_run_id TEXT NOT NULL,
  ts              INTEGER NOT NULL,
  event_type      TEXT NOT NULL,                   -- started|generation_complete|checkpoint_saved|resumed|completed|crashed|failed
  generation      INTEGER,
  payload         TEXT
);
CREATE TABLE IF NOT EXISTS pipeline_log (          -- append-only LOG lines
  log_id          INTEGER PRIMARY KEY AUTOINCREMENT,
  pipeline_run_id TEXT NOT NULL,
  ts              INTEGER NOT NULL,
  level           TEXT NOT NULL,                   -- info|warn|error
  generation      INTEGER,
  message         TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_pipeline_event_run ON pipeline_event(pipeline_run_id, ts, event_id);
CREATE INDEX IF NOT EXISTS ix_pipeline_log_run   ON pipeline_log(pipeline_run_id, ts, log_id);
```

**New file:** `atx-engine/include/atx/engine/store/pipeline_progress.hpp` (header-only,
`namespace atx::engine::store`). Mirrors `RunRecorder`'s style.

```cpp
struct PipelineRunRow {
  std::string pipeline_run_id;     // caller-supplied stable id (e.g. fingerprint hex)
  atx::u64    fingerprint{0};
  std::string stage;               // "discover"
  atx::u64    master_seed{0};
  atx::i64    population{0};
  atx::i64    total_generations{0};
  std::string panel_path;
  std::string config_json;
  std::string engine_git_sha;
  atx::i64    created_at{0};
};

// Result of a resume lookup.
struct ResumableRun {
  std::string pipeline_run_id;
  atx::i64    last_generation{-1};  // -1 = a row exists but no checkpoint yet
};

class PipelineRecorder {
 public:
  // Insert a new pipeline_run with status='running', last_generation=-1, heartbeat=now.
  // Emits a 'started' event. (No replay rejection: a discover run may legitimately
  // re-run; fingerprint UNIQUE means a second begin() for the same fingerprint errors —
  // callers resume() instead.)
  [[nodiscard]] static atx::core::Result<PipelineRecorder>
  begin(atx::core::db::Database& db, const PipelineRunRow& r);

  // Find an incomplete (status != 'completed', finished_at IS NULL) run with this
  // fingerprint and return its id + the MAX(generation) in pipeline_checkpoint (or -1).
  // Returns Err(NotFound) if none. Used by impl when --resume is set.
  [[nodiscard]] static atx::core::Result<ResumableRun>
  find_resumable(atx::core::db::Database& db, atx::u64 fingerprint);

  // Re-attach a recorder to an existing run row (for resume); sets status='resumed',
  // emits a 'resumed' event, bumps heartbeat.
  [[nodiscard]] static atx::core::Result<PipelineRecorder>
  resume(atx::core::db::Database& db, std::string_view pipeline_run_id, atx::i64 ts);

  // ONE transaction (BEGIN IMMEDIATE): upsert pipeline_checkpoint(generation, blob,
  // count, state_hash); insert pipeline_iteration row; UPDATE pipeline_run
  // SET last_generation=generation, updated_at=ts, last_heartbeat_at=ts; insert
  // 'generation_complete' + 'checkpoint_saved' events. Atomic ⇒ crash-safe.
  [[nodiscard]] atx::core::Status
  save_checkpoint(atx::i64 generation, std::string_view population_blob,
                  atx::i64 population_count, atx::f64 best_fitness, atx::f64 mean_fitness,
                  atx::i64 n_evaluated, atx::i64 n_unique, atx::i64 wall_ms, atx::i64 ts);

  // Load the population_blob of the highest-generation checkpoint (the resume point).
  [[nodiscard]] atx::core::Result<std::string>
  latest_population_blob() const;  // Err(NotFound) if no checkpoint

  [[nodiscard]] atx::core::Status heartbeat(atx::i64 ts);
  [[nodiscard]] atx::core::Status log(std::string_view level, atx::i64 generation,
                                      std::string_view message, atx::i64 ts);
  [[nodiscard]] atx::core::Status event(std::string_view event_type, atx::i64 generation,
                                        std::string_view payload, atx::i64 ts);
  // status='completed', finished_at=ts; emits 'completed'. mark_failed(ts,msg) ⇒
  // status='failed', finished_at=ts, 'failed' event (log carries msg).
  [[nodiscard]] atx::core::Status complete(atx::i64 ts);
  [[nodiscard]] atx::core::Status mark_failed(atx::i64 ts, std::string_view message);
};

// blob helpers (free functions): join '\n' / split '\n'; FNV-1a64 over the blob.
[[nodiscard]] std::string  join_population(const std::vector<std::string>&);
[[nodiscard]] std::vector<std::string> split_population(std::string_view);
[[nodiscard]] atx::u64     population_hash(std::string_view);
```

**Crash detection:** a `pipeline_run` with `finished_at IS NULL` and `status IN
('running','resumed','crashed')` is resumable. A stale heartbeat is informational
(reported by a future status command); v1 does not auto-expire — `--resume` matches
on fingerprint regardless of heartbeat age.

---

## Component 3 — Factory + impl wiring

**Factory (factory.hpp + factory.cpp):** add defaulted trailing params to the
sequential admit paths and forward them to `SearchDriver::run`:
```cpp
FactoryReport mine_into(const FactoryConfig& cfg, library::Library& lib_lib,
                        const combine::AlphaGate& gate,
                        SearchProgressSink* sink = nullptr,
                        const SearchResumeState* resume = nullptr);
// private:
FactoryReport mine_into_oos(const FactoryConfig& cfg, library::Library& lib_lib,
                            const combine::AlphaGate& gate,
                            SearchProgressSink* sink = nullptr,
                            const SearchResumeState* resume = nullptr);
```
`mine_into` forwards sink/resume to `mine_into_oos` (when `oos_fraction>0`) and to its
own `driver.run(...)`. `mine_into_oos` forwards to its (train-panel) `driver.run(...)`.
The `IExecutor` overload (factory.hpp:259) is **not** wired for checkpointing in v1; if
`sink != nullptr` on the MultiProcess substrate it returns
`Err(InvalidArgument, "checkpointing requires InProcess workers")` (matches the
existing OOS/MultiProcess restriction). Off-path (`nullptr`) all overloads are unchanged.

**impl config (config.hpp / config.cpp):** add to the discover block
```cpp
std::string run_db;      // --run-db  (SQLite progress DB path; "" = off, no store I/O)
bool        resume{false}; // --resume (requires --run-db; continue an incomplete matching run)
```
Parse `run-db` as a string value-flag; `resume` as a boolean flag. Reject `--resume`
without `--run-db` (`Err(InvalidArgument)`). Include both in the config round-trip writer.

**impl sink + glue (new files `atx-impl/src/store_progress_sink.{hpp,cpp}`):**
- `class StoreProgressSink final : public atx::engine::factory::SearchProgressSink`
  holding `store::PipelineRecorder&` + a monotonic generation wall-clock; `on_generation`
  → `recorder.save_checkpoint(snap.generation, store::join_population(snap.population),
  snap.population.size(), snap.best_fitness, snap.mean_fitness, snap.n_evaluated,
  snap.n_unique, wall_ms, now)`. Time via `std::chrono::system_clock` (impl-side; never
  in the engine determinism path).
- Free helper `compute_discover_fingerprint(const RunConfig&)` →
  `store::fingerprint::compute({engine_git_sha, config_normalized, …, master_seed,
  gate_config})` where `config_normalized` folds panel_path + population + generations +
  sorted seed_exprs + oos_fraction/embargo and `gate_config` folds the gate floors.

**impl gated discover (stage_discover.cpp, gated path ~line 85-130):** when
`cfg.run_db` non-empty:
1. `StoreDb::open(cfg.run_db)` (creates + bootstraps schema v2 if new).
2. `fp = compute_discover_fingerprint(cfg)`.
3. If `cfg.resume`: `find_resumable(db, fp)`; on hit, `latest_population_blob()` →
   `SearchResumeState{ last_generation /* resume AT the last checkpointed gen */ ,
   split_population(blob) }`, `recorder = resume(...)`, emit log. On miss, fall through
   to begin (log "no resumable run; starting fresh").
4. Else `recorder = begin(db, {…})`.
5. Construct `StoreProgressSink sink{recorder}`; pass `&sink` (and `&resume_state` or
   nullptr) to `fac.mine_into(fcfg, liblib, gate, &sink, resume_ptr)`.
6. On success `recorder.complete(now)` + log summary; on `mine_into` error
   `recorder.mark_failed(now, msg)` and propagate.
When `cfg.run_db` is empty: the existing call `fac.mine_into(fcfg, liblib, gate)` is
used verbatim (defaulted nullptrs) — **no store code runs**.

Resume semantics detail: the checkpoint for generation G stores the population that
ENTERED generation G. `find_resumable` returns `last_generation = G_max` (the highest
checkpointed gen). Resume sets `start_generation = G_max` and replays from G_max
(re-evaluating that one generation), so worst-case loss = 1 generation. If
`G_max >= generations-1`, the search re-runs the final generation and finalizes
identically.

---

## Testing

**Engine — `atx-engine-factory-tests`:**
- `PopulationRoundTrip`: `deserialize_population(serialize_population(pop))` yields
  genomes with identical `canon_hash` (and identical canonical order).
- `SinkCalledPerGeneration`: a fake sink records N `on_generation` calls for N
  generations with correct `generation` indices and population sizes.
- `OffPathByteIdentical`: `run(cfg, pool, nullptr, nullptr)` digest == legacy
  `run(cfg, pool)` digest (pin the off-path).
- **`ResumeProducesIdenticalSearch`** (discriminating): run a full search → reference
  `admitted_candidates` + `all_scored` canon_hashes; run with a fake sink that returns
  `Err` after generation K (simulated crash), capturing the gen-K population blob;
  resume via `SearchResumeState{K, split(blob)}`; assert the resumed run's
  `admitted_candidates` and `all_scored` canon_hash multisets equal the full run's.
  (Do NOT assert `SearchResult.digest` equality — see the resume invariant note; the
  digest folds per-generation and differs across a resume boundary by design.)

**Engine — `atx-engine-store-tests`:**
- Schema v2 golden guard updated: the five new tables present, version == 2,
  `create_all` idempotent (run twice on the same DB → no error, same table set), and a
  v1→v2 open adds the tables + bumps the stamp.
- `PipelineRecorderLifecycle`: begin → save_checkpoint(0) → save_checkpoint(1) →
  complete; assert `pipeline_run.status` transitions, `last_generation==1`, one
  iteration row per gen, `latest_population_blob()` returns gen-1 blob, events appended
  in order (`started`,`generation_complete`,`checkpoint_saved`,…,`completed`).
- `FindResumableReturnsLatestCheckpoint`: begin + 2 checkpoints, no complete →
  `find_resumable(fp)` returns that id with `last_generation==1`; after `complete`,
  `find_resumable` returns `Err(NotFound)`.
- `SaveCheckpointAtomic`: `population_hash(blob)` stored equals recomputed
  `population_hash`; checkpoint+iteration+events all present after one `save_checkpoint`.

**impl — `atx-impl-tests`:**
- `ConfigParsesRunDbResume` + round-trip; `ResumeWithoutRunDbRejected`.
- `DiscoverOffPathByteIdentical`: gated discover with no `--run-db` produces identical
  `_manifest.txt` + stage digest as the pre-change path (small synthetic panel).
- `DiscoverWithRunDbWritesProgress`: with `--run-db`, after a run the DB has a
  `pipeline_run` (status `completed`), `pipeline_iteration` rows == generations, ≥1
  `pipeline_checkpoint`, and `pipeline_event` includes `started`+`completed`.
- `DiscoverResumeEndToEnd`: run once with `--run-db` to completion (capture
  `_manifest.txt` digest); delete the alphas output dir; run again with
  `--run-db --resume` → identical admitted alphas (the resume path replays the search;
  on a completed run it begins fresh — so this test instead interrupts via a small
  generations count and verifies the second run's manifest equals a one-shot run's
  manifest). [Implementer: prefer the engine-level `ResumeProducesIdenticalSearch` as
  the authoritative correctness proof; the impl test asserts the wiring + DB rows.]

---

## Build & verify

- Build in the warm `build-rel` (Release, Ninja). Targets:
  `atx-engine-factory-tests`, `atx-engine-store-tests`, `atx-impl-tests`, and finally
  `atx-impl` (the exe — only after the discovery lock is confirmed free).
  `cmake --build c:/Users/natha/OneDrive/Desktop/atx/build-rel --target <t>`. `/W4 /WX` clean.
- Known pre-existing failures unrelated to this work (do not chase):
  `AlphaSlotPoolDeathTest.OverAcquire_Aborts`, `AlphaVm_ZeroAlloc.*`,
  `AtxImplPanel.BuildsPanelFromSegments`.

---

## Out of scope / future

- A `discover --run-db <p> --status` read command (list runs, % complete, last
  heartbeat). Trivial on this schema; deferred.
- Optimize/combine resumability (same tables, new `stage` value).
- MultiProcess (cross-worker) checkpointing (wire-format change).
- Heartbeat-age auto-expiry / crash GC.
