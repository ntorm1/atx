# Resumable Discover (crash-safe genetic search) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the gated `discover` genetic search crash-resilient and resumable, persisting per-generation progress to the `atx::engine::store` SQLite DB so a crash loses ≤1 generation instead of the whole run.

**Architecture:** An abstract `SearchProgressSink` + `SearchResumeState` are wired into `SearchDriver::run` as defaulted (nullptr) params — off-path byte-identical. A header-only `PipelineRecorder` + 5 schema-v2 tables persist checkpoints/iterations/events/logs. `Factory::mine_into`/`mine_into_oos` forward the sink/resume. impl adds `--run-db`/`--resume`, a store-backed sink, and a run fingerprint, wired into the gated discover stage.

**Tech Stack:** C++20, clang-cl + Ninja (warm `build-rel`, Release), vendored SQLite (in `atx-core`, linked transitively), GoogleTest. Reuse `alpha::unparse`/`alpha::parse_expr`, `store::fingerprint`, `store::StoreDb`.

**Spec:** `docs/superpowers/specs/2026-06-19-resumable-discover-design.md` (read for rationale + verified anchors).

## Global Constraints

- **Off-path byte-identical:** no `--run-db` ⇒ impl opens no DB, builds no sink, passes `nullptr`; engine params default `nullptr`; every existing output (library digest, `_manifest.txt`, `.dsl`, stage digest, `SearchResult.digest`) is byte-identical to today. Tested.
- **Resume correctness invariant:** crash-after-gen-K + resume ⇒ byte-identical **admitted alpha set** (`all_scored` + `admitted_candidates` canon_hashes, `.dsl` files, per-alpha metrics) vs an uninterrupted run. NOT asserted: `SearchResult.digest` / manifest `factory_digest` — that is a per-generation within-run fingerprint and differs across a resume boundary by design (does not affect alpha DB content).
- **No new third-party dependencies.** Reuse store/fingerprint/unparse/parse_expr.
- **Determinism (F1/F2):** identical inputs ⇒ identical digest, across worker counts. NEVER put wall-clock/system time/RNG into any engine code on the determinism path — time lives impl-side only.
- **Store + impl are header-only / already-linked:** `pipeline_progress.hpp` is header-only (no engine CMake change); `atx-impl` already links `atx::engine` (no impl CMake change for store/sqlite).
- **Namespaces:** search/genome/factory types are `atx::engine::factory`; store types are `atx::engine::store`; impl glue is `atx::impl`.
- NEVER `git add -A`; stage explicit paths only. Commit locally when green (authorized): trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push. Do NOT create docs/spec/plan files.
- Build the impl TEST target (`atx-impl-tests`), NOT the `atx-impl` exe, until told the discovery lock is free. Build command: `cmake --build c:/Users/natha/OneDrive/Desktop/atx/build-rel --target <target>`. `/W4 /WX` must be clean.
- Pre-existing failures unrelated to this work (do NOT chase): `AlphaSlotPoolDeathTest.OverAcquire_Aborts`, `AlphaVm_ZeroAlloc.*`, `AtxImplPanel.BuildsPanelFromSegments`.

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `atx-engine/include/atx/engine/factory/search_progress.hpp` (new) | `SearchProgressSink`, `GenerationSnapshot`, `SearchResumeState` | 2 |
| `atx-engine/include/atx/engine/factory/search_driver.hpp` (mod) | `run()` sig + private serialize/deserialize/mean_raw decls | 1,2 |
| `atx-engine/src/factory/search_driver.cpp` (mod) | serialize/deserialize impl; `run()` sink+resume body | 1,2 |
| `atx-engine/tests/factory/search_progress_test.cpp` (new) | engine search tests | 1,2 |
| `atx-engine/include/atx/engine/store/schema.hpp` (mod) | schema v2: 5 new tables, version bump | 3 |
| `atx-engine/tests/store/store_schema_golden_test.cpp` (mod) | golden guard for v2 | 3 |
| `atx-engine/include/atx/engine/store/pipeline_progress.hpp` (new) | `PipelineRecorder` + blob helpers | 4 |
| `atx-engine/tests/store/store_pipeline_progress_test.cpp` (new) | recorder tests | 4 |
| `atx-engine/include/atx/engine/factory/factory.hpp` (mod) | `mine_into`/`mine_into_oos` sink/resume params | 5 |
| `atx-engine/src/factory/factory.cpp` (mod) | forward sink/resume to `driver.run` | 5 |
| `atx-engine/tests/factory/factory_oos_test.cpp` (mod) or new | factory forwarding test | 5 |
| `atx-impl/src/config.hpp` / `config.cpp` (mod) | `--run-db`/`--resume` flags | 6 |
| `atx-impl/tests/...config test` (mod) | flag parse tests | 6 |
| `atx-impl/src/store_progress_sink.hpp` / `.cpp` (new) | `StoreProgressSink` + `compute_discover_fingerprint` | 7 |
| `atx-impl/src/stage_discover.cpp` (mod) | gated-path wiring | 7 |
| `atx-impl/tests/...discover test` (new/mod) | impl integration tests | 7 |

---

### Task 1: Engine — population serialize/deserialize helpers on SearchDriver

**Files:**
- Modify: `atx-engine/include/atx/engine/factory/search_driver.hpp` (add 3 private member decls near the other private helpers, ~line 448-459)
- Modify: `atx-engine/src/factory/search_driver.cpp` (add 2 member function impls)
- Test: `atx-engine/tests/factory/search_progress_test.cpp` (new)

**Interfaces:**
- Consumes: `alpha::unparse(const Ast&)` (atx-engine/include/atx/engine/alpha/unparse.hpp), `alpha::parse_expr(src, lib_)` (parser.hpp), `factory::analyze_into(Ast)` / `factory::canonical_hash(const Genome&)` (genome.hpp/canonical.hpp), `factory::canon_less` (search_driver.hpp:200, detail ns). Genome: `genome.hpp` (`Ast ast; Analysis analysis; u64 canon_hash;`).
- Produces (private members of `SearchDriver`):
  - `std::vector<std::string> serialize_population(const std::vector<Genome>& pop) const;` — returns `unparse(g.ast)` for each genome, in canonical-id order (sort indices by `detail::canon_less`), so the output is deterministic & insertion-order-independent.
  - `atx::core::Result<std::vector<Genome>> deserialize_population(const std::vector<std::string>& exprs) const;` — for each expr: `parse_expr(expr, lib_)` → `analyze_into(...)` → set `canon_hash = canonical_hash(g)`. Propagates the first parse/analyze error.

- [ ] **Step 1: Write the failing test (`search_progress_test.cpp`)**

Build a `SearchDriver` exactly as the existing search tests do (find the pattern in `atx-engine/tests/factory/` — likely `factory_search_*` or `search_driver_test.cpp`: it needs a `Library`, a small synthetic `Panel`, a `WeightPolicy`, an `ExecutionSimulator`, seed exprs, panel fields). Expose the two new helpers for testing via a tiny friend or a thin public test shim ONLY if they are private — prefer: make the test exercise them through `run()` in Task 2, and for THIS task test round-trip via a `friend` test or by temporarily constructing genomes. Simplest robust approach: add a `friend` declaration for the test fixture, or test the round-trip property at the population level.

```cpp
// atx-engine/tests/factory/search_progress_test.cpp
#include <gtest/gtest.h>
#include "atx/engine/factory/search_driver.hpp"
// + the same includes/fixtures the existing search_driver test uses to build a driver.

namespace atx::engine::factory {
// Test fixture builds: Library lib; Panel panel (small synthetic, a few fields/dates);
// WeightPolicy policy; exec::ExecutionSimulator sim; seed_exprs; panel_fields.
// (Copy the construction from the existing search_driver/factory test fixture.)

TEST(SearchProgressRoundTrip, PopulationCanonHashesPreserved) {
  // Arrange: build a driver with >=2 distinct in-grammar seed exprs.
  auto fx = MakeSearchFixture(/* seeds */ {"rank(close)", "delta(close, 5)"});
  SearchDriver driver(fx.lib, fx.panel, fx.policy, fx.sim, fx.seed_exprs, fx.panel_fields);

  // Build an initial population via the driver's own init path (Task-1 test helper:
  // expose init_population through a friend, OR parse the same seeds here).
  std::vector<Genome> pop = fx.parse_population({"rank(close)", "delta(close, 5)"});
  ASSERT_GE(pop.size(), 2u);
  std::vector<atx::u64> before;
  for (auto& g : pop) before.push_back(g.canon_hash);

  // Act: serialize -> deserialize.
  std::vector<std::string> blob = driver.serialize_population(pop);
  auto round = driver.deserialize_population(blob);
  ASSERT_TRUE(round.has_value());

  // Assert: same count, and the MULTISET of canon_hashes is identical (serialize sorts
  // canonical, so compare sorted).
  ASSERT_EQ(round->size(), pop.size());
  std::vector<atx::u64> after;
  for (auto& g : *round) after.push_back(g.canon_hash);
  std::sort(before.begin(), before.end());
  std::sort(after.begin(), after.end());
  EXPECT_EQ(before, after);
}
}  // namespace atx::engine::factory
```

Add `friend class SearchProgressRoundTrip_PopulationCanonHashesPreserved_Test;` (and any fixture helper friend) to `SearchDriver` so the test can call the private helpers and `init_population`. Add the new test file to `atx-engine/tests/CMakeLists.txt` factory group source list if file globbing is not automatic (check how the existing factory tests are listed; the group is `atx-engine-factory-tests`).

- [ ] **Step 2: Run test to verify it fails (does not compile — helpers undefined)**

Run: `cmake --build c:/Users/natha/OneDrive/Desktop/atx/build-rel --target atx-engine-factory-tests`
Expected: compile error — `serialize_population` / `deserialize_population` not members of `SearchDriver`.

- [ ] **Step 3: Add the private member declarations (search_driver.hpp)**

In `class SearchDriver`, in the `private:` block near the result-assembly helpers (~line 448-459, before the `lib_` members), add:

```cpp
  // ----- resumable-checkpoint serialization (header-only contract: round-trip
  //  through the canonical key, canonical_hash(parse_expr(unparse(ast)))==canonical_hash(ast)).
  //  serialize: unparse each genome's single root to a DSL string, in CANONICAL-ID order
  //  (detail::canon_less) so the blob is insertion-order-independent & deterministic.
  //  deserialize: parse_expr against lib_ + analyze_into + canonical_hash, rebuilding a
  //  structurally-identical genome (Expr::op re-resolved against lib_). -----
  [[nodiscard]] std::vector<std::string> serialize_population(const std::vector<Genome>& pop) const;
  [[nodiscard]] atx::core::Result<std::vector<Genome>>
  deserialize_population(const std::vector<std::string>& exprs) const;
```

Add the test friend(s) just inside `class SearchDriver {` (after `public:` or in private):
```cpp
  friend struct SearchProgressTestAccess;  // test-only: exercises private serialize/init helpers
```
(Define `struct SearchProgressTestAccess` in the test file to call `init_population`, `serialize_population`, `deserialize_population`.)

Ensure includes are present: `unparse.hpp` (`#include "atx/engine/alpha/unparse.hpp"`) and `genome.hpp`/`canonical.hpp` are already included (canonical.hpp is, line 88; genome.hpp line 92). Add `#include "atx/engine/alpha/unparse.hpp"` to search_driver.hpp if not transitively present (verify; `<algorithm>` for std::sort may also be needed).

- [ ] **Step 4: Implement in search_driver.cpp**

```cpp
std::vector<std::string> SearchDriver::serialize_population(const std::vector<Genome>& pop) const {
  // canonical-id order (value-based, insertion-independent) -> deterministic blob.
  std::vector<atx::usize> order(pop.size());
  for (atx::usize i = 0; i < pop.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](atx::usize a, atx::usize b) { return detail::canon_less(pop[a], pop[b]); });
  std::vector<std::string> out;
  out.reserve(pop.size());
  for (atx::usize i : order) out.push_back(alpha::unparse(pop[i].ast));
  return out;
}

atx::core::Result<std::vector<Genome>>
SearchDriver::deserialize_population(const std::vector<std::string>& exprs) const {
  std::vector<Genome> out;
  out.reserve(exprs.size());
  for (const auto& src : exprs) {
    ATX_TRY(auto ast, alpha::parse_expr(src, lib_));
    ATX_TRY(auto g, analyze_into(std::move(ast)));   // factory::analyze_into -> Genome
    g.canon_hash = canonical_hash(g);
    out.push_back(std::move(g));
  }
  return atx::core::Ok(std::move(out));
}
```

Verify the exact spelling of `analyze_into` (genome.hpp) and that `alpha::unparse(const Ast&)` (single-arg overload over the AST's single root) exists at unparse.hpp:135-143 — `serialize_genome.cpp` uses `alpha::unparse(g.ast)`, mirror it. Add `#include <algorithm>` to the .cpp if needed.

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build c:/Users/natha/OneDrive/Desktop/atx/build-rel --target atx-engine-factory-tests` then `ctest --test-dir c:/Users/natha/OneDrive/Desktop/atx/build-rel -R SearchProgressRoundTrip --output-on-failure`
Expected: `PopulationCanonHashesPreserved` PASS.

- [ ] **Step 6: Commit**

```bash
git add atx-engine/include/atx/engine/factory/search_driver.hpp atx-engine/src/factory/search_driver.cpp atx-engine/tests/factory/search_progress_test.cpp atx-engine/tests/CMakeLists.txt
git commit -m "feat(search): genome population serialize/deserialize for checkpointing

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Engine — `SearchProgressSink` + resume wiring in `SearchDriver::run`

**Files:**
- Create: `atx-engine/include/atx/engine/factory/search_progress.hpp`
- Modify: `atx-engine/include/atx/engine/factory/search_driver.hpp` (run() sig + `mean_raw` decl + include)
- Modify: `atx-engine/src/factory/search_driver.cpp` (run() body)
- Test: `atx-engine/tests/factory/search_progress_test.cpp` (extend)

**Interfaces:**
- Consumes: Task 1's `serialize_population`/`deserialize_population`; `detail::seed_for`; the existing `run()` internals (`init_population`, `evaluate_generation`, `reproduce`, `finalize`, `best_raw`, `CanonSet`, `fitness_cache`, `BehavioralArchive`, `engines`).
- Produces:
  - `factory::SearchProgressSink` (abstract), `factory::GenerationSnapshot`, `factory::SearchResumeState` — full definitions below.
  - New `run` overload signature: `SearchResult run(const SearchConfig& cfg, const combine::AlphaStore& pool, SearchProgressSink* sink = nullptr, const SearchResumeState* resume = nullptr);`
  - `static atx::f64 SearchDriver::mean_raw(const std::vector<Scored>&);`

- [ ] **Step 1: Create `search_progress.hpp`**

```cpp
#pragma once

// atx::engine::factory — resumable-search progress sink + resume state.
//
// A SearchProgressSink receives one GenerationSnapshot per generation (after that
// generation has been evaluated/ranked). The snapshot's `population` is ALREADY
// serialized (one canonical DSL string per genome) — the population that ENTERED that
// generation, i.e. exactly the state needed to resume AT that generation. The sink
// owns no engine types and never touches the Library, so a store-backed implementation
// can live entirely outside the engine. Default = no sink (nullptr) => byte-identical
// legacy search (F1). on_generation returning non-Ok ABORTS the run cleanly.

#include <string>
#include <vector>

#include "atx/core/error.hpp"  // atx::core::Status
#include "atx/core/types.hpp"  // atx::usize, atx::f64

namespace atx::engine::factory {

struct GenerationSnapshot {
  atx::usize generation{0};
  std::vector<std::string> population;  // unparse(g.ast) per genome, canonical-id order
  atx::f64 best_fitness{0.0};
  atx::f64 mean_fitness{0.0};
  atx::usize n_evaluated{0};            // distinct candidates scored so far (CanonSet size)
  atx::usize n_unique{0};               // genomes in this generation's population
};

class SearchProgressSink {
 public:
  virtual ~SearchProgressSink() = default;
  [[nodiscard]] virtual atx::core::Status on_generation(const GenerationSnapshot&) = 0;
};

// When non-null with 0 < start_generation < cfg.generations and a non-empty population,
// SearchDriver::run SKIPS init_population, deserializes `population` as the gen-
// start_generation population, and runs the loop from start_generation. Otherwise the
// run starts fresh from generation 0 (safe no-op resume).
struct SearchResumeState {
  atx::usize start_generation{0};
  std::vector<std::string> population;
};

}  // namespace atx::engine::factory
```

- [ ] **Step 2: Write failing tests (extend `search_progress_test.cpp`)**

```cpp
#include "atx/engine/factory/search_progress.hpp"

namespace atx::engine::factory {

// A fake sink: records every snapshot; optionally returns Err after `fail_after_gen`.
struct RecordingSink : SearchProgressSink {
  std::vector<GenerationSnapshot> seen;
  int fail_after_gen = -1;  // -1 = never fail
  atx::core::Status on_generation(const GenerationSnapshot& s) override {
    seen.push_back(s);
    if (fail_after_gen >= 0 && static_cast<int>(s.generation) >= fail_after_gen)
      return atx::core::Err(atx::core::ErrorCode::Internal, "injected crash");
    return atx::core::Ok();
  }
};

TEST(SearchProgress, SinkCalledPerGeneration) {
  auto fx = MakeSearchFixture({"rank(close)", "delta(close, 5)"});
  SearchDriver driver(fx.lib, fx.panel, fx.policy, fx.sim, fx.seed_exprs, fx.panel_fields);
  SearchConfig cfg; cfg.master_seed = 7; cfg.population = 6; cfg.generations = 4;
  cfg.objective_mode = ObjectiveMode::ScalarRaw; cfg.novelty_w = 0.0;  // pin determinism
  RecordingSink sink;
  combine::AlphaStore pool;  // empty pool, as the existing search tests use
  SearchResult r = driver.run(cfg, pool, &sink, nullptr);
  EXPECT_EQ(sink.seen.size(), cfg.generations);
  for (atx::usize i = 0; i < sink.seen.size(); ++i) {
    EXPECT_EQ(sink.seen[i].generation, i);
    EXPECT_EQ(sink.seen[i].population.size(), cfg.population);
  }
}

TEST(SearchProgress, OffPathByteIdentical) {
  auto fx = MakeSearchFixture({"rank(close)", "delta(close, 5)"});
  SearchDriver d1(fx.lib, fx.panel, fx.policy, fx.sim, fx.seed_exprs, fx.panel_fields);
  SearchConfig cfg; cfg.master_seed = 7; cfg.population = 6; cfg.generations = 4;
  combine::AlphaStore pool;
  SearchResult legacy = d1.run(cfg, pool);                 // 2-arg legacy call
  SearchResult with_null = d1.run(cfg, pool, nullptr, nullptr);
  EXPECT_EQ(legacy.digest, with_null.digest);
  EXPECT_EQ(legacy.trial_count, with_null.trial_count);
}

TEST(SearchProgress, ResumeProducesIdenticalSearch) {
  auto fx = MakeSearchFixture({"rank(close)", "delta(close, 5)"});
  SearchDriver d(fx.lib, fx.panel, fx.policy, fx.sim, fx.seed_exprs, fx.panel_fields);
  SearchConfig cfg; cfg.master_seed = 7; cfg.population = 6; cfg.generations = 5;
  cfg.objective_mode = ObjectiveMode::ScalarRaw; cfg.novelty_w = 0.0;
  combine::AlphaStore pool;

  // Full uninterrupted run -> reference digest.
  SearchResult full = d.run(cfg, pool);

  // "Crash" after generation K=2: capture the gen-2 snapshot population.
  RecordingSink crash; crash.fail_after_gen = 2;
  SearchResult crashed = d.run(cfg, pool, &crash, nullptr);
  // The run aborts; the last recorded snapshot is generation 2 (the one that triggered).
  ASSERT_FALSE(crash.seen.empty());
  const auto& cp = crash.seen.back();
  ASSERT_EQ(cp.generation, 2u);

  // Resume AT generation 2 from that population.
  SearchResumeState rs; rs.start_generation = cp.generation; rs.population = cp.population;
  SearchResult resumed = d.run(cfg, pool, nullptr, &rs);

  // HARD assertion: the admitted alpha set + all distinct scored structures are
  // byte-identical (this is what becomes the alpha DB). Compare canon_hash MULTISETS.
  auto hashes = [](const std::vector<Genome>& v) {
    std::vector<atx::u64> h; for (auto& g : v) h.push_back(g.canon_hash);
    std::sort(h.begin(), h.end()); return h;
  };
  EXPECT_EQ(hashes(resumed.admitted_candidates), hashes(full.admitted_candidates));
  EXPECT_EQ(hashes(resumed.all_scored), hashes(full.all_scored));
  // Do NOT assert resumed.digest == full.digest — the digest folds per-generation and
  // a resumed run executes fewer generations in-process, so this within-run fingerprint
  // differs across a resume boundary by design (see the resume invariant). The alpha
  // DB content (admitted set + metrics) is what must match, and does.
}

}  // namespace atx::engine::factory
```

- [ ] **Step 3: Update `search_driver.hpp` — run() signature, mean_raw, include**

Add include near the other factory includes (~line 96): `#include "atx/engine/factory/search_progress.hpp"`.

Change the `run` declaration (line 286) to:
```cpp
  [[nodiscard]] SearchResult run(const SearchConfig &cfg, const combine::AlphaStore &pool,
                                 SearchProgressSink *sink = nullptr,
                                 const SearchResumeState *resume = nullptr);
```

Add a private static helper decl near `best_raw` (~line 453):
```cpp
  // Mean RAW fitness over the scored set (telemetry for the progress sink; NOT part of
  // the digest/admission). NaN/inf-safe: skips non-finite; empty -> 0.
  [[nodiscard]] static atx::f64 mean_raw(const std::vector<Scored> &scored);
```

- [ ] **Step 4: Update `run()` in search_driver.cpp**

Locate `run()` (search_driver.cpp:37-123). Apply these surgical changes, preserving ALL existing logic:

1. **Initial population + start generation.** Replace `pop = init_population(cfg)` (~line 74) with:
```cpp
  std::vector<Genome> pop;
  atx::usize gen_start = 0;
  if (resume != nullptr && resume->start_generation > 0 &&
      resume->start_generation < cfg.generations && !resume->population.empty()) {
    auto restored = deserialize_population(resume->population);
    if (!restored) {  // a corrupt/incompatible checkpoint -> fail loud, do NOT silently restart
      SearchResult err_res;
      err_res.seed = cfg.master_seed;
      return err_res;  // (or propagate; run() returns SearchResult by value — empty result)
    }
    pop = std::move(*restored);
    gen_start = resume->start_generation;
  } else {
    pop = init_population(cfg);
  }
```
> If `run()` cannot return an error (it returns `SearchResult` not `Result<>`), a failed deserialize should be surfaced via the sink/caller path; for the engine test, deserialize of a valid blob always succeeds. Keep the empty-result fallback but add a code comment that impl validates the blob hash before calling.

2. **Loop start.** Change the generation loop (~line 79) from `for (atx::usize gen = 0; gen < cfg.generations; ++gen)` to `for (atx::usize gen = gen_start; gen < cfg.generations; ++gen)`.

3. **Digest-fold invariance.** The per-generation digest fold uses the generation index, so folding only gens `gen_start..N-1` yields a digest that differs from a full run. This is EXPECTED and fine — `SearchResult.digest` is a within-run fingerprint, and the resumable correctness contract is on the ADMITTED SET + `all_scored`, which are reconstructed identically because the population at gen_start is identical and `seed_for(master,gen,idx)` is pure. Do NOT attempt to back-fill the skipped-generation digest. (The Step-2 test asserts admitted/all_scored canon_hashes, not digest, for the resume case.)

4. **Sink call.** After the per-generation ranking and `res.best_fitness_per_gen.push_back(best_raw(scored));` (~line 112) and BEFORE the `if (gen < generations-1) pop = reproduce(...)` line, insert:
```cpp
    if (sink != nullptr) {
      GenerationSnapshot snap;
      snap.generation = gen;
      snap.population = serialize_population(pop);  // the population that ENTERED gen `gen`
      snap.best_fitness = best_raw(scored);
      snap.mean_fitness = mean_raw(scored);
      snap.n_evaluated = canon.size();
      snap.n_unique = pop.size();
      auto st = sink->on_generation(snap);
      if (!st) {  // sink requested abort (real I/O error or injected crash) -> stop cleanly
        finalize(scored, canon, res);  // produce a well-formed (partial) result
        return res;
      }
    }
```
> `pop` is the generation input and is NOT mutated by `evaluate_generation` (it only scores into `scored`/`canon`/`fitness_cache`). Confirm this by reading evaluate_generation; if `pop` IS mutated, snapshot a copy taken at loop top instead.

5. **mean_raw impl** (add near `best_raw`):
```cpp
atx::f64 SearchDriver::mean_raw(const std::vector<Scored>& scored) {
  atx::f64 sum = 0.0; atx::usize n = 0;
  for (const auto& s : scored) {
    if (std::isfinite(s.fitness)) { sum += s.fitness; ++n; }
  }
  return n ? sum / static_cast<atx::f64>(n) : 0.0;
}
```
Add `#include <cmath>` to the .cpp if not present.

- [ ] **Step 5: Run tests**

Run: `cmake --build c:/Users/natha/OneDrive/Desktop/atx/build-rel --target atx-engine-factory-tests` then `ctest --test-dir c:/Users/natha/OneDrive/Desktop/atx/build-rel -R "SearchProgress" --output-on-failure`
Expected: `SinkCalledPerGeneration`, `OffPathByteIdentical`, `ResumeProducesIdenticalSearch` PASS. Also run the EXISTING search/factory determinism tests to confirm off-path byte-identical: `ctest --test-dir .../build-rel -R "factory" --output-on-failure` — all previously-passing tests still pass.

- [ ] **Step 6: Commit**

```bash
git add atx-engine/include/atx/engine/factory/search_progress.hpp atx-engine/include/atx/engine/factory/search_driver.hpp atx-engine/src/factory/search_driver.cpp atx-engine/tests/factory/search_progress_test.cpp
git commit -m "feat(search): SearchProgressSink + resume in SearchDriver::run (off-path byte-identical)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Store — schema v2 (5 pipeline tables) + golden guard

**Files:**
- Modify: `atx-engine/include/atx/engine/store/schema.hpp` (bump `kSchemaVersion` 1→2; add 5 tables + 2 indexes to `create_all`; allow upward version restamp)
- Modify: `atx-engine/tests/store/store_schema_golden_test.cpp`
- Test target: `atx-engine-store-tests`

**Interfaces:**
- Consumes: `StoreDb::open` (db.hpp), `schema::create_all`, `schema::kSchemaVersion`, the existing `schema_meta` stamp logic.
- Produces: schema v2 with tables `pipeline_run`, `pipeline_checkpoint`, `pipeline_iteration`, `pipeline_event`, `pipeline_log` (+ indexes `ix_pipeline_event_run`, `ix_pipeline_log_run`); `kSchemaVersion == 2`.

- [ ] **Step 1: Write/extend failing test (`store_schema_golden_test.cpp`)**

Read the existing golden test first (it pins the expected table set + version). Add the 5 new tables to its expected-set, bump the expected version to 2, and add:
```cpp
TEST(StoreSchemaGolden, PipelineTablesPresentAtV2) {
  auto db = atx::engine::store::StoreDb::open_memory();
  ASSERT_TRUE(db.has_value());
  // schema_version == 2
  auto v = db->schema_version();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 2);
  // each pipeline table exists
  for (const char* t : {"pipeline_run","pipeline_checkpoint","pipeline_iteration",
                        "pipeline_event","pipeline_log"}) {
    auto* stmt = *db->db().prepare_cached(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1");
    ASSERT_TRUE(stmt->bind(1, t).has_value());
    auto step = stmt->step();
    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(*step, atx::core::db::Statement::Step::Row) << "missing table " << t;
    (void)stmt->reset();  // if the API requires reset before reuse
  }
}

TEST(StoreSchemaGolden, CreateAllIdempotent) {
  auto db = atx::engine::store::StoreDb::open_memory();
  ASSERT_TRUE(db.has_value());
  // create_all again on the same db must not error.
  ASSERT_TRUE(atx::engine::store::schema::create_all(db->db()).has_value());
}
```
Match the EXACT prepared-statement / Result API the other store tests use (read `store_db_test.cpp` for the idiom; e.g. `ATX_TRY`-style or `.has_value()`/`*`). Update the existing golden-set assertion to include the 5 tables so the drift guard stays accurate.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build c:/Users/natha/OneDrive/Desktop/atx/build-rel --target atx-engine-store-tests` then `ctest --test-dir .../build-rel -R StoreSchemaGolden --output-on-failure`
Expected: FAIL — version is 1, pipeline tables absent.

- [ ] **Step 3: Edit schema.hpp**

1. Bump the constant (schema.hpp:11): `inline constexpr int kSchemaVersion = 2;`
2. In `create_all`, after the existing `CREATE TABLE`/`CREATE INDEX` statements and before the `schema_meta` stamp, append the 5 tables + 2 indexes (use the EXACT DDL from the spec §Component 2). Keep `CREATE TABLE IF NOT EXISTS` / `CREATE INDEX IF NOT EXISTS`.
3. **Upward restamp.** The existing stamp writes `schema_meta` once at creation. Extend it so opening a v1 DB updates the recorded version to 2 after `create_all` adds the new tables. Concretely, replace the first-create stamp guard with an idempotent upsert: if `schema_meta` is empty, INSERT `(2,'v2',0)`; else `UPDATE schema_meta SET schema_version=2, engine_version='v2'` (bounded single row). Read the current stamp code (schema.hpp:72-90) and mirror its statement style. Do NOT change the v1 columns or the other 19 tables.

- [ ] **Step 4: Run tests**

Run: `cmake --build .../build-rel --target atx-engine-store-tests` then `ctest --test-dir .../build-rel -R "StoreSchema" --output-on-failure` and the full store suite `ctest ... -R "Store" --output-on-failure`.
Expected: golden v2 tests PASS; all other store tests still PASS (the 19 v1 tables unchanged).

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/store/schema.hpp atx-engine/tests/store/store_schema_golden_test.cpp
git commit -m "feat(store): schema v2 — pipeline progress/checkpoint/iteration/event/log tables

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Store — `PipelineRecorder` + blob helpers

**Files:**
- Create: `atx-engine/include/atx/engine/store/pipeline_progress.hpp` (header-only, inline — mirror `run_recorder.hpp` style)
- Test: `atx-engine/tests/store/store_pipeline_progress_test.cpp` (new; add to `atx-engine-store-tests` group)

**Interfaces:**
- Consumes: `atx::core::db::Database` (prepare_cached/bind/step/changes), `atx::core::db::Transaction` (RAII BEGIN/COMMIT/ROLLBACK), `atx::core::Result`/`Status`/`ErrorCode`, `ATX_TRY`/`ATX_TRY_VOID`. Schema v2 tables from Task 3. FNV-1a64 constants — reuse `fingerprint::kFnvOffset`/`kFnvPrime` or `fingerprint::fold_*` (fingerprint.hpp).
- Produces: the `PipelineRunRow`, `ResumableRun`, `PipelineRecorder` (begin/find_resumable/resume/save_checkpoint/latest_population_blob/heartbeat/log/event/complete/mark_failed) and free helpers `join_population`/`split_population`/`population_hash` exactly as in the spec §Component 2.

- [ ] **Step 1: Write failing tests (`store_pipeline_progress_test.cpp`)**

```cpp
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/pipeline_progress.hpp"

namespace atx::engine::store {

static PipelineRunRow MakeRow(atx::u64 fp) {
  PipelineRunRow r;
  r.pipeline_run_id = "run-" + std::to_string(fp);
  r.fingerprint = fp; r.stage = "discover"; r.master_seed = 7;
  r.population = 6; r.total_generations = 5; r.panel_path = "/p.bin";
  r.config_json = "{}"; r.engine_git_sha = "deadbeef"; r.created_at = 1000;
  return r;
}

TEST(PipelineRecorder, Lifecycle) {
  auto db = StoreDb::open_memory(); ASSERT_TRUE(db.has_value());
  auto rec = PipelineRecorder::begin(db->db(), MakeRow(111));
  ASSERT_TRUE(rec.has_value());
  ASSERT_TRUE(rec->save_checkpoint(0, "a\nb", 2, 1.0, 0.5, 2, 2, 100, 1001).has_value());
  ASSERT_TRUE(rec->save_checkpoint(1, "c\nd", 2, 2.0, 1.0, 4, 2, 120, 1002).has_value());
  auto blob = rec->latest_population_blob();
  ASSERT_TRUE(blob.has_value());
  EXPECT_EQ(*blob, "c\nd");
  ASSERT_TRUE(rec->complete(2000).has_value());
  // status == 'completed', last_generation == 1
  auto* s = *db->db().prepare_cached("SELECT status,last_generation FROM pipeline_run WHERE fingerprint=111");
  ASSERT_EQ(*s->step(), atx::core::db::Statement::Step::Row);
  EXPECT_EQ(s->column_text(0), "completed");   // match the column API used elsewhere
  EXPECT_EQ(s->column_i64(1), 1);
}

TEST(PipelineRecorder, FindResumableReturnsLatestCheckpoint) {
  auto db = StoreDb::open_memory(); ASSERT_TRUE(db.has_value());
  auto rec = PipelineRecorder::begin(db->db(), MakeRow(222));
  ASSERT_TRUE(rec.has_value());
  ASSERT_TRUE(rec->save_checkpoint(0, "x", 1, 1, 1, 1, 1, 1, 1).has_value());
  ASSERT_TRUE(rec->save_checkpoint(1, "y", 1, 1, 1, 1, 1, 1, 1).has_value());
  auto found = PipelineRecorder::find_resumable(db->db(), 222);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->last_generation, 1);
  // after complete -> not resumable
  ASSERT_TRUE(rec->complete(9).has_value());
  auto none = PipelineRecorder::find_resumable(db->db(), 222);
  EXPECT_FALSE(none.has_value());  // Err(NotFound)
}

TEST(PipelineRecorder, BlobHelpersRoundTrip) {
  std::vector<std::string> v{"rank(close)", "delta(close, 5)"};
  auto j = join_population(v);
  auto back = split_population(j);
  EXPECT_EQ(back, v);
  EXPECT_EQ(population_hash(j), population_hash(join_population(back)));
}

}  // namespace atx::engine::store
```
Adjust the column-read API (`column_text`/`column_i64` etc.) to match what the other store tests use (read `store_run_recorder_test.cpp`).

- [ ] **Step 2: Run to verify it fails** (header missing) — `cmake --build .../build-rel --target atx-engine-store-tests` → compile error.

- [ ] **Step 3: Implement `pipeline_progress.hpp`**

Write the header per spec §Component 2 (full code). Key implementation points:
- `begin`: INSERT pipeline_run (status='running', last_generation=-1, updated_at=created_at, last_heartbeat_at=created_at, finished_at=NULL); append 'started' event. Return a `PipelineRecorder` holding `Database&` + `pipeline_run_id` (copy). Use `Transaction` for the insert+event.
- `save_checkpoint`: ONE `Transaction::begin(db)`: `INSERT OR REPLACE INTO pipeline_checkpoint(...)` with `population_hash(blob)`; `INSERT OR REPLACE INTO pipeline_iteration(...)`; `UPDATE pipeline_run SET last_generation=?, updated_at=?, last_heartbeat_at=?`; INSERT 'generation_complete' + 'checkpoint_saved' events; commit.
- `latest_population_blob`: `SELECT population_blob FROM pipeline_checkpoint WHERE pipeline_run_id=?1 ORDER BY generation DESC LIMIT 1`; Err(NotFound) if no row.
- `find_resumable` (static): `SELECT pipeline_run_id FROM pipeline_run WHERE fingerprint=?1 AND finished_at IS NULL AND status<>'completed' LIMIT 1`; then `SELECT COALESCE(MAX(generation),-1) FROM pipeline_checkpoint WHERE pipeline_run_id=?`. Err(NotFound) if no run row.
- `resume` (static): `UPDATE pipeline_run SET status='resumed', last_heartbeat_at=?2, updated_at=?2 WHERE pipeline_run_id=?1`; changes()==1 guard; append 'resumed' event; return recorder.
- `heartbeat`, `log`, `event`, `complete` (status='completed', finished_at), `mark_failed` (status='failed', finished_at + a log line).
- helpers: `join_population` ('\n' join), `split_population` (split on '\n', drop a trailing empty), `population_hash` (FNV-1a64 over the bytes — reuse `fingerprint::fold_bytes(fingerprint::kFnvOffset, blob)`).

Use `static_cast<atx::i64>(u64)` when binding u64 to SQLite (mirror `run_recorder`/`fingerprint::is_replay`).

- [ ] **Step 4: Run tests** — `cmake --build .../build-rel --target atx-engine-store-tests` then `ctest --test-dir .../build-rel -R "PipelineRecorder" --output-on-failure`. Expected: 3 tests PASS. Full store suite still green.

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/store/pipeline_progress.hpp atx-engine/tests/store/store_pipeline_progress_test.cpp atx-engine/tests/CMakeLists.txt
git commit -m "feat(store): PipelineRecorder — checkpoint/iteration/event/log write path + resume lookup

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Factory — thread sink/resume through `mine_into` / `mine_into_oos`

**Files:**
- Modify: `atx-engine/include/atx/engine/factory/factory.hpp` (sigs at :238 and :301; the IExecutor overload :259 guard; include search_progress.hpp via search_driver.hpp which already includes it)
- Modify: `atx-engine/src/factory/factory.cpp` (forward sink/resume to `driver.run`)
- Test: `atx-engine/tests/factory/factory_oos_test.cpp` (extend) or `search_progress_test.cpp`

**Interfaces:**
- Consumes: Task 2's `SearchProgressSink`/`SearchResumeState` and the new `SearchDriver::run(cfg,pool,sink,resume)`.
- Produces: `mine_into(cfg, lib_lib, gate, SearchProgressSink* sink=nullptr, const SearchResumeState* resume=nullptr)` and private `mine_into_oos(cfg, lib_lib, gate, sink=nullptr, resume=nullptr)`; the IExecutor overload errors if `sink!=nullptr` on MultiProcess.

- [ ] **Step 1: Write failing test (extend factory tests)**

```cpp
TEST(FactorySinkForwarding, MineIntoForwardsSinkPerGeneration) {
  // Build a Factory + a small library + gate as the existing factory tests do.
  auto fx = MakeFactoryFixture(/* seeds, panel, gate floors */);
  factory::RecordingSink sink;   // reuse the sink from search_progress_test (move to a shared test header, or redefine)
  FactoryConfig cfg = fx.cfg; cfg.search.generations = 3; cfg.search.population = 6;
  // oos OFF for this test (legacy mine_into path)
  FactoryReport rep = fx.factory.mine_into(cfg, fx.lib_lib, fx.gate, &sink, nullptr);
  EXPECT_EQ(sink.seen.size(), cfg.search.generations);
}

TEST(FactorySinkForwarding, OffPathDigestUnchanged) {
  auto fx = MakeFactoryFixture(/* ... */);
  FactoryConfig cfg = fx.cfg;
  FactoryReport a = fx.factory.mine_into(cfg, fx.lib_lib, fx.gate);                 // legacy
  // fresh library for the second run to compare the search digest
  auto fx2 = MakeFactoryFixture(/* identical */);
  FactoryReport b = fx2.factory.mine_into(cfg, fx2.lib_lib, fx2.gate, nullptr, nullptr);
  EXPECT_EQ(a.digest, b.digest);
}
```
Put `RecordingSink` in a shared test header (e.g. `atx-engine/tests/factory/test_progress_sink.hpp`) so both Task 2 and Task 5 tests use it. (If sharing is awkward, redefine locally.)

- [ ] **Step 2: Run to verify it fails** — extra args not accepted by `mine_into`.

- [ ] **Step 3: Edit factory.hpp + factory.cpp**

- factory.hpp:238 — `mine_into` add `, SearchProgressSink *sink = nullptr, const SearchResumeState *resume = nullptr` to the signature.
- factory.hpp:301 — `mine_into_oos` add the same two defaulted params.
- factory.hpp:259 — the IExecutor overload: leave its signature as-is (no checkpoint params) BUT if a future caller passes a sink it goes through the 3-arg+gate overload. Add (in factory.cpp) at the top of the IExecutor overload: if the substrate is MultiProcess, the existing behavior stands (no sink). No new param needed here.
- factory.cpp:
  - In `mine_into(cfg, lib, gate, sink, resume)`: at the top, `if (cfg.oos_fraction > 0.0) return mine_into_oos(cfg, lib_lib, gate, sink, resume);` (the existing top guard — add the args). At the `driver.run(cfg.search, search_pool)` call site (~factory.cpp:150-151), change to `driver.run(cfg.search, search_pool, sink, resume)`.
  - In `mine_into_oos(...)`: at its `driver.run(...)` call over the TRAIN panel, pass `sink, resume`.
  - The InProcess `mine_into(cfg, lib, gate, exec)` overload delegates to `mine_into(cfg, lib, gate)` — it does not carry a sink (checkpointing is InProcess-sequential only). Leave it; the gated discover stage calls the sequential `mine_into(cfg,lib,gate,...)` directly.

- [ ] **Step 4: Run tests** — `cmake --build .../build-rel --target atx-engine-factory-tests` then `ctest --test-dir .../build-rel -R "FactorySinkForwarding|MineIntoOos|factory" --output-on-failure`. Expected: new tests PASS; all existing factory/OOS determinism tests still PASS (off-path digest unchanged).

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/factory/factory.hpp atx-engine/src/factory/factory.cpp atx-engine/tests/factory/factory_oos_test.cpp atx-engine/tests/factory/test_progress_sink.hpp atx-engine/tests/factory/search_progress_test.cpp
git commit -m "feat(factory): forward SearchProgressSink + resume through mine_into/mine_into_oos

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: impl — `--run-db` / `--resume` config flags

**Files:**
- Modify: `atx-impl/src/config.hpp` (discover block, near `oos_fraction` ~line 49), `atx-impl/src/config.cpp` (`apply_flag_value` / boolean-flag parsing + the `--config` round-trip writer)
- Test: the existing impl config test (find it under `atx-impl/tests/`; the target is `atx-impl-tests`)

**Interfaces:**
- Consumes: the `RunConfig` struct + `set_flags` tracking + `apply_flag_value` (config.cpp), the config round-trip writer.
- Produces: `RunConfig::run_db` (std::string, default ""), `RunConfig::resume` (bool, default false); flags `--run-db <path>`, `--resume`; validation that `--resume` requires `--run-db`.

- [ ] **Step 1: Write failing tests**

```cpp
TEST(Config, ParsesRunDbAndResume) {
  const char* argv[] = {"atx-impl","discover","--panel","p.bin","--alpha-out","o",
                        "--gated","--run-db","prog.db","--resume"};
  auto cfg = parse_args(/* argc */ 11, argv);   // match the actual parse_args signature
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->run_db, "prog.db");
  EXPECT_TRUE(cfg->resume);
}

TEST(Config, ResumeWithoutRunDbRejected) {
  const char* argv[] = {"atx-impl","discover","--panel","p.bin","--alpha-out","o",
                        "--gated","--resume"};
  auto cfg = parse_args(9, argv);
  EXPECT_FALSE(cfg.has_value());  // Err(InvalidArgument)
}
```
Match the EXACT `parse_args` signature + Result type the existing impl config tests use (read one).

- [ ] **Step 2: Run to verify it fails** — fields/flags absent.

- [ ] **Step 3: Implement**

- config.hpp discover block:
```cpp
  std::string run_db;        // --run-db  (SQLite progress DB; "" = off, no store I/O)
  bool resume = false;       // --resume  (requires --run-db; continue an incomplete matching run)
```
- config.cpp: parse `--run-db` as a string value-flag (mirror an existing string value-flag like `--panel`/`--alpha-out`, registering in `set_flags` the same way). Parse `--resume` as a boolean flag (mirror `--gated`). After arg parsing for the discover subcommand, validate: `if (cfg.resume && cfg.run_db.empty()) return Err(InvalidArgument, "--resume requires --run-db");`. Add both to the config round-trip writer if it enumerates discover fields (so round-trips stay lossless).

- [ ] **Step 4: Run tests** — `cmake --build .../build-rel --target atx-impl-tests` then `ctest --test-dir .../build-rel -R "Config" --output-on-failure`. Expected: both PASS, existing config tests green.

- [ ] **Step 5: Commit**

```bash
git add atx-impl/src/config.hpp atx-impl/src/config.cpp atx-impl/tests/<config_test_file>
git commit -m "feat(discover): add --run-db / --resume flags (resume requires run-db)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: impl — `StoreProgressSink` + fingerprint + gated-discover wiring

**Files:**
- Create: `atx-impl/src/store_progress_sink.hpp` / `atx-impl/src/store_progress_sink.cpp` (add `.cpp` to `atx-impl-core` sources in `atx-impl/CMakeLists.txt`)
- Modify: `atx-impl/src/stage_discover.cpp` (gated path, ~line 85-130)
- Test: `atx-impl/tests/...discover/store test` (new file in `atx-impl-tests`)

**Interfaces:**
- Consumes: `factory::SearchProgressSink`/`GenerationSnapshot`/`SearchResumeState`; `store::StoreDb`/`PipelineRecorder`/`join_population`/`split_population`; `store::fingerprint::compute`/`RunInputs`; `RunConfig` (run_db/resume/seed/population/generations/seed_exprs/min_* /oos_*); `Factory::mine_into(cfg,lib,gate,sink,resume)`.
- Produces: `atx::impl::StoreProgressSink` (final SearchProgressSink), `atx::impl::compute_discover_fingerprint(const RunConfig&) -> atx::u64`.

- [ ] **Step 1: Write failing tests (`atx-impl/tests/store_discover_test.cpp` or extend the discover test)**

```cpp
TEST(DiscoverStore, OffPathByteIdentical) {
  // Build a tiny synthetic research panel + a temp alpha-out dir (reuse the existing
  // discover test's panel fixture). Run gated discover with NO --run-db twice; the
  // _manifest.txt content + stage digest must match (and match the pre-change baseline
  // if one is pinned). Assert manifest bytes equal across two runs.
}

TEST(DiscoverStore, WithRunDbWritesProgress) {
  // Run gated discover with --run-db <temp.db> on a tiny panel, small generations.
  // After it completes, open the db and assert:
  //   - one pipeline_run row, status='completed'
  //   - COUNT(pipeline_iteration) == generations
  //   - COUNT(pipeline_checkpoint) >= 1
  //   - pipeline_event has 'started' and 'completed'
}

TEST(DiscoverStore, FingerprintStableAndSensitive) {
  RunConfig a = MakeDiscoverCfg(); RunConfig b = a;
  EXPECT_EQ(compute_discover_fingerprint(a), compute_discover_fingerprint(b));
  b.seed += 1;
  EXPECT_NE(compute_discover_fingerprint(a), compute_discover_fingerprint(b));
}
```
A full gated discover on a tiny synthetic panel may be heavy; keep population/generations tiny (e.g. population=4, generations=2) and the panel small. Reuse whatever panel/library fixture the existing gated-discover test uses (read `atx-impl/tests/` for it). If a full gated run is impractical in a unit test, split: unit-test `compute_discover_fingerprint` + `StoreProgressSink` against a `PipelineRecorder` directly (feed it synthetic `GenerationSnapshot`s and assert the DB rows), and assert the off-path byte-identical manifest with the existing discover fixture.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement `store_progress_sink.{hpp,cpp}`**

```cpp
// store_progress_sink.hpp
#pragma once
#include "atx/engine/factory/search_progress.hpp"
#include "atx/engine/store/pipeline_progress.hpp"
#include "config.hpp"
namespace atx::impl {

class StoreProgressSink final : public atx::engine::factory::SearchProgressSink {
 public:
  explicit StoreProgressSink(atx::engine::store::PipelineRecorder& rec) : rec_{rec} {}
  [[nodiscard]] atx::core::Status
  on_generation(const atx::engine::factory::GenerationSnapshot& s) override;
 private:
  atx::engine::store::PipelineRecorder& rec_;
};

// Deterministic FNV-1a64 over the resumable inputs (panel, seed, population, generations,
// sorted seed_exprs, oos_fraction/embargo, gate floors). Two identical configs -> same fp.
[[nodiscard]] atx::u64 compute_discover_fingerprint(const RunConfig& cfg);

}  // namespace atx::impl
```
```cpp
// store_progress_sink.cpp
#include "store_progress_sink.hpp"
#include <chrono>
#include "atx/engine/store/fingerprint.hpp"
namespace atx::impl {
static atx::i64 now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
atx::core::Status StoreProgressSink::on_generation(
    const atx::engine::factory::GenerationSnapshot& s) {
  const std::string blob = atx::engine::store::join_population(s.population);
  return rec_.save_checkpoint(static_cast<atx::i64>(s.generation), blob,
      static_cast<atx::i64>(s.population.size()), s.best_fitness, s.mean_fitness,
      static_cast<atx::i64>(s.n_evaluated), static_cast<atx::i64>(s.n_unique),
      /*wall_ms*/ 0, now_unix());
}
atx::u64 compute_discover_fingerprint(const RunConfig& cfg) {
  namespace fp = atx::engine::store::fingerprint;
  atx::u64 h = fp::kFnvOffset;
  h = fp::fold_string(h, cfg.panel);
  h = fp::fold_u64(h, cfg.seed);
  h = fp::fold_u64(h, static_cast<atx::u64>(cfg.population));
  h = fp::fold_u64(h, static_cast<atx::u64>(cfg.generations));
  std::vector<std::string> seeds = cfg.seed_exprs;  // copy + sort -> order-independent
  std::sort(seeds.begin(), seeds.end());
  for (const auto& e : seeds) h = fp::fold_string(h, e);
  // gate floors + oos (use the actual RunConfig field names/types)
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.min_sharpe));
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.min_fitness));
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.max_turnover));
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.max_pool_corr));
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.min_dsr));
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.oos_fraction));
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.oos_embargo));
  return h;
}
}  // namespace atx::impl
```
Verify `fold_string`/`fold_u64`/`kFnvOffset` are accessible (they are inline in `fingerprint.hpp`). Add `#include <bit>` and `#include <algorithm>`. Use the exact RunConfig field names (min_sharpe etc. — confirm spelling in config.hpp).

- [ ] **Step 4: Wire `stage_discover.cpp` gated path**

In the gated path, after `FactoryConfig fcfg` is built and BEFORE `fac.mine_into(fcfg, liblib, gate)` (~line 123):
```cpp
  if (!cfg.run_db.empty()) {
    ATX_TRY(auto store, atx::engine::store::StoreDb::open(cfg.run_db));
    const atx::u64 fp = compute_discover_fingerprint(cfg);
    std::optional<atx::engine::factory::SearchResumeState> resume_state;
    std::optional<atx::engine::store::PipelineRecorder> rec;
    if (cfg.resume) {
      auto found = atx::engine::store::PipelineRecorder::find_resumable(store.db(), fp);
      if (found && found->last_generation >= 0) {
        // re-attach + load the last checkpoint population
        ATX_TRY(auto r, atx::engine::store::PipelineRecorder::resume(store.db(), found->pipeline_run_id, now_unix()));
        rec.emplace(std::move(r));
        ATX_TRY(auto blob, rec->latest_population_blob());
        resume_state.emplace();
        resume_state->start_generation = static_cast<atx::usize>(found->last_generation);
        resume_state->population = atx::engine::store::split_population(blob);
      }
    }
    if (!rec) {  // fresh run
      atx::engine::store::PipelineRunRow row;
      row.pipeline_run_id = /* hex of fp */;
      row.fingerprint = fp; row.stage = "discover"; row.master_seed = cfg.seed;
      row.population = static_cast<atx::i64>(cfg.population);
      row.total_generations = static_cast<atx::i64>(cfg.generations);
      row.panel_path = cfg.panel; row.config_json = ""; row.engine_git_sha = "";
      row.created_at = now_unix();
      ATX_TRY(auto r, atx::engine::store::PipelineRecorder::begin(store.db(), row));
      rec.emplace(std::move(r));
    }
    atx::impl::StoreProgressSink sink{*rec};
    const auto* rs = resume_state ? &*resume_state : nullptr;
    auto rep_res = fac.mine_into(fcfg, liblib, gate, &sink, rs);
    if (!rep_res) { (void)rec->mark_failed(now_unix(), rep_res.error().message()); return ... propagate; }
    (void)rec->complete(now_unix());
    // continue with *rep_res exactly as the existing code uses the report
  } else {
    // EXISTING call, verbatim:
    const factory::FactoryReport rep = fac.mine_into(fcfg, liblib, gate);
    // ... existing manifest/.dsl writing unchanged ...
  }
```
> IMPORTANT off-path discipline: structure the branch so the `cfg.run_db.empty()` path executes the EXACT existing code (no store headers touched at runtime, identical control flow), guaranteeing byte-identical output. If `mine_into` returns `FactoryReport` by value (not Result), there is no `rep_res.error()` — adjust to the actual return type (factory.hpp:238 returns `FactoryReport`, NOT Result; so there is no error branch — drop `mark_failed` on the normal path and call `complete` after the call returns). Keep `now_unix()` accessible (move it to the sink header as an inline free function, or duplicate locally).
Add `#include "store_progress_sink.hpp"` + `#include "atx/engine/store/db.hpp"` + `#include "atx/engine/store/pipeline_progress.hpp"` + `<optional>` to stage_discover.cpp.

- [ ] **Step 5: CMake** — add `src/store_progress_sink.cpp` to the `atx-impl-core` `add_library` source list in `atx-impl/CMakeLists.txt`.

- [ ] **Step 6: Run tests** — `cmake --build .../build-rel --target atx-impl-tests` then `ctest --test-dir .../build-rel -R "DiscoverStore|Discover" --output-on-failure`. Expected: new tests PASS; existing discover tests green.

- [ ] **Step 7: Commit**

```bash
git add atx-impl/src/store_progress_sink.hpp atx-impl/src/store_progress_sink.cpp atx-impl/src/stage_discover.cpp atx-impl/CMakeLists.txt atx-impl/tests/store_discover_test.cpp
git commit -m "feat(discover): wire store-backed progress sink + resume into gated discover

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Final integration (controller, after all tasks reviewed)

1. Build the impl exe once the discovery lock is free: `cmake --build c:/Users/natha/OneDrive/Desktop/atx/build-rel --target atx-impl`.
2. Whole-branch review (subagent-driven-development final review).
3. Relaunch the megaalpha DB generation under the new feature (crash-safe), with a safer worker count to avoid the prior OOM:
   `atx-impl discover --panel C:/atx-run/megaalpha_db/panel_enriched.bin --alpha-out C:/atx-run/megaalpha_db/alphas --gated --seed 7 --population 60 --generations 15 --min-sharpe 0.5 --min-fitness 0.15 --max-turnover 0.40 --max-pool-corr 0.5 --min-dsr 0.0 --target-aum 1e9 --oos-fraction 0.25 --oos-embargo 0.01 --workers 8 --run-db C:/atx-run/megaalpha_db/progress.db <seed-exprs…>`
   If it crashes, re-run the identical command with `--resume` appended.
</content>
