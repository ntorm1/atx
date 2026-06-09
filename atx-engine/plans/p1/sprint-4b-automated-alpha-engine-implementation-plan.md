# Sprint S4b — The Automated Alpha Engine — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fuse the S3 factory and the S4 library into one automated, persistent, deflation-gated alpha-discovery engine with end-to-end seeded replay.

**Architecture:** Add three pieces on top of the two finished halves: (1) `unparse(Ast)→string` so every admitted alpha carries a round-trippable formula; (2) a `PoolView` seam so `pool_aware_fitness` scores marginal correlation against *either* the ephemeral `combine::AlphaStore` (S3 tests) *or* the persistent library at O(neighbors); (3) `Factory::mine_into(library)` (the real admit path) driven by a `ResearchDriver` continuous mine→admit→repeat loop. Library = single source of truth; the S1 deflation bar stays factory-side, wrapped around `library::admit`.

**Tech Stack:** C++20, GoogleTest (auto-globbed `*_test.cpp`), Google Benchmark (`*_bench.cpp`, `ATX_BUILD_BENCH=ON`), header-only `atx::engine::{alpha,factory,library,combine,exec}` over `atx::core`.

---

## Discipline (BINDING — every task, every commit)

Shared branch `feat/atx-core-stdlib`. A concurrent effort writes FOREIGN files on this branch — build against them, NEVER commit them.

- **Explicit pathspecs ONLY.** `git add -- <paths>` and `git commit -- <paths>`. NEVER `git add -A` / `.` / `-a`.
- **Do NOT push.** Local commits only.
- **Do NOT touch:** `atx-core/*`, p0 `ROADMAP.md`, `.agents/*`, `.clang-tidy`, `.clangd`, `.vscode/*`, `research/*`, `.gitmodules`, other sprints' ledgers. atx-core stays untouched (no new general-purpose primitive — verify `git diff <base>..HEAD -- atx-core/` is empty at close).
- **Every commit message ends with** the trailer:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
- **After each commit, verify:** `git merge-base --is-ancestor HEAD HEAD` (exit 0) AND `git show HEAD --stat` shows ONLY this unit's files.
- **clang-tidy is disabled.** The gate is the `cmake --build` under `/W4 /permissive- /WX` + strict-FP. It must be clean (zero warnings) AND the gtest suite green.
- **Base SHA:** `518af61` (the S4b spec commit). All S4b work descends from here.

**Build & test convention** (every "Run" step): use the repo's already-configured CMake build directory (confirm its path from `CMakePresets.json` / the existing `build*/` dir before the first build). Targets:
- Tests: `cmake --build <build> --target atx-engine-tests` then run `<build>/…/atx-engine-tests --gtest_filter=<Suite>.*`.
- Bench: configure with `-DATX_BUILD_BENCH=ON`, `cmake --build <build> --target atx-engine-bench`.
New `*_test.cpp` / `*_bench.cpp` files are auto-discovered via `CONFIGURE_DEPENDS` glob — re-run CMake configure so the glob picks them up.

**Spec:** [`sprint-4b-automated-alpha-engine.md`](sprint-4b-automated-alpha-engine.md). **Ledger (created in S4b-0):** [`sprint-4b-progress.md`](sprint-4b-progress.md).

---

## File structure (created / modified)

| File | Unit | Responsibility |
|---|---|---|
| `atx-engine/plans/p1/sprint-4b-progress.md` | S4b-0 | ledger (base SHA, per-unit table, commits, residuals) |
| `atx-engine/include/atx/engine/alpha/unparse.hpp` | S4b-1 | `unparse(Ast, ExprId)→string` + whole-Ast overload |
| `atx-engine/tests/alpha_unparse_test.cpp` | S4b-1 | round-trip-through-`canonical_hash` soundness |
| `atx-engine/include/atx/engine/factory/pool_view.hpp` | S4b-2 | `PoolView` seam + `AlphaStorePool` + `LibraryPool` backings |
| `atx-engine/include/atx/engine/factory/fitness.hpp` (modify) | S4b-2 | `pool_aware_fitness(const PoolView&, …)` overload |
| `atx-engine/include/atx/engine/library/library.hpp` (modify) | S4b-2 | additive `worst_corr_to_pool(span) const` accessor |
| `atx-engine/tests/factory_pool_view_test.cpp` | S4b-2 | backing-equality + O(neighbors) cost |
| `atx-engine/include/atx/engine/factory/factory.hpp` (modify) | S4b-3 | `Factory::mine_into(library)` + telemetry on `FactoryReport` |
| `atx-engine/tests/factory_mine_into_test.cpp` | S4b-3 | mine→library admit bridge |
| `atx-engine/include/atx/engine/factory/research_driver.hpp` | S4b-4 | `ResearchDriver` continuous loop + `ResearchReport` |
| `atx-engine/tests/factory_research_driver_test.cpp` | S4b-4 | loop / stop-condition / checkpoint-resume |
| `atx-engine/tests/research_engine_test.cpp` | S4b-5 | E2E F1/F4/F6 + unparse-at-scale capstone |
| `atx-engine/bench/research_bench.cpp` | S4b-5 | mined/sec, admitted/hour, O(neighbors) speedup |

---

## Task S4b-0: Marker + ledger + as-built seam record

**Files:** Create `atx-engine/plans/p1/sprint-4b-progress.md`

- [ ] **Step 1: Write the ledger** mirroring the S3/S4 `sprint-3-progress.md` shape. Sections:
  - **Header:** sprint title, `Status: 🚧 OPEN`, base SHA `518af61`, the shared-branch note (explicit-pathspec discipline), links to the spec + ROADMAP.
  - **Per-unit table** with rows S4b-0…S4b-5, columns `Unit | Title | Status | Commit(s)`, all ⬜ except S4b-0 (fill at end of this task).
  - **As-built seam** block (copy the verified API block from the spec §"The seam — as-built API"): `library::AlphaCandidate`, `library::Library::admit`, `online_corr_to_pool`, `Provenance`; plus the facts gathered: `pool_aware_fitness` signature, `FitnessReport{wq,redundancy,diversify,robust,raw,dsr,haircut_sharpe}`, `FitnessCfg{trial_count,cpcv,book_size}`, `Factory{ctor(lib,panel,sim,policy), mine(cfg,store,gate)}`, `Genome{ast,analysis,canon_hash}`, `SearchConfig`/`SearchResult`, `detail::seed_for(master,gen,idx)`.
  - **Kickoff risks:** (a) the `PoolView` seam must NOT fork the corr math (one implementation per backing, both return *max* |corr| so they agree); (b) the deflation bar stays factory-side (`dsr ≥ min_dsr` AND `admit==Accept`); (c) whole-engine replay must include the persisted manifest `version_id`; (d) `unparse` must round-trip through `canonical_hash`, not merely "look right".
  - **Commits table** (empty placeholder) and **Residuals / Baton** (empty placeholder).

- [ ] **Step 2: Commit**

```bash
git add -- atx-engine/plans/p1/sprint-4b-progress.md
git commit -m "docs(s4b-0): open ledger — automated alpha engine (factory↔library)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git merge-base --is-ancestor HEAD HEAD; echo "ancestor:$?"
git show HEAD --stat
```
Expected: ancestor:0; stat shows ONLY `sprint-4b-progress.md`.

---

## Task S4b-1: `unparse(Ast)→string` + round-trip soundness

The riskiest unit — the formula record. There is **no** Ast→string in the codebase today. The load-bearing contract is **round-trip through the canonical key**, not textual prettiness.

**Files:**
- Create: `atx-engine/include/atx/engine/alpha/unparse.hpp`
- Test: `atx-engine/tests/alpha_unparse_test.cpp`
- **Read first (do NOT modify):** `atx-engine/include/atx/engine/alpha/parser.hpp` (`Expr` struct lines 73-98: `Kind`, `opcode`, `value`, `name_id`, `a/b/c` children, `hparams[2]`, `n_hparams`; `Ast` lines 131-156: `nodes_`, `strings_`, `roots_`; `parse_expr(string_view, Library&)→Result<Ast>` lines 711-722), `atx-engine/include/atx/engine/alpha/registry.hpp` (`OpCode` enum lines 75-166 — the full op table; `OpSig{name, opcode, min_arity/max_arity, n_hparams, …}`; `Library::find(name)→OpSig*`), and `atx-engine/include/atx/engine/factory/canonical.hpp` (`canonical_hash(ast, root)→u64` lines 261-264).

- [ ] **Step 1: Write the failing round-trip test.** This is the spec — it defines correctness for ALL inputs without enumerating every op's rendering.

```cpp
// atx-engine/tests/alpha_unparse_test.cpp
#include <gtest/gtest.h>
#include <string>
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/unparse.hpp"
#include "atx/engine/factory/canonical.hpp"

namespace {
using namespace atx::engine;

// Round-trips losslessly THROUGH THE CANONICAL KEY: unparse → parse → same canonical_hash.
void expect_round_trips(const std::string& src) {
  alpha::Library lib = alpha::Library::builtins();           // confirm the builtin-table ctor name
  auto a0 = alpha::parse_expr(src, lib);
  ASSERT_TRUE(a0.has_value()) << "fixture must parse: " << src;
  const std::string text = alpha::unparse(*a0, a0->root());  // confirm root accessor on Ast
  auto a1 = alpha::parse_expr(text, lib);
  ASSERT_TRUE(a1.has_value()) << "unparse must re-parse: " << text;
  EXPECT_EQ(factory::canonical_hash(*a0, a0->root()),
            factory::canonical_hash(*a1, a1->root()))
      << "round-trip changed the canonical key\n src=" << src << "\n txt=" << text;
}

TEST(AlphaUnparse, RoundTripsLeafField)        { expect_round_trips("close"); }
TEST(AlphaUnparse, RoundTripsBinaryOp)         { expect_round_trips("add(close, open)"); }
TEST(AlphaUnparse, RoundTripsConstAndWindow)   { expect_round_trips("ts_mean(close, 8)"); }
TEST(AlphaUnparse, RoundTripsFractionalConst)  { expect_round_trips("decay_linear(close, 8.22237)"); }
TEST(AlphaUnparse, RoundTripsNested)           { expect_round_trips("ts_mean(add(close, mul(open, 0.5)), 5)"); }
TEST(AlphaUnparse, RoundTripsRankAndCs)        { expect_round_trips("cs_rank(ts_sum(close, 10))"); }

// Fail-on-bad: a deliberately-wrong printer (swaps operands of a NON-commutative op) flips the hash.
TEST(AlphaUnparse, WrongOrderFlipsHash) {
  alpha::Library lib = alpha::Library::builtins();
  auto a = alpha::parse_expr("sub(close, open)", lib);
  ASSERT_TRUE(a.has_value());
  auto b = alpha::parse_expr("sub(open, close)", lib);
  ASSERT_TRUE(b.has_value());
  EXPECT_NE(factory::canonical_hash(*a, a->root()),
            factory::canonical_hash(*b, b->root()))
      << "sub is non-commutative — operand order MUST change the canonical key";
}
```

> Implementer: confirm the exact builtin-`Library` constructor (`builtins()` vs a global), the `Ast::root()`/single-root accessor, and the `canonical_hash` arity from the headers read above; adjust the harness lines to the real API. The CONTRACT (round-trip equality) does not change.

- [ ] **Step 2: Run the test, verify it fails to compile** (`unparse.hpp` does not exist). Run: `cmake --build <build> --target atx-engine-tests`. Expected: FAIL — `unparse.hpp: No such file` / `unparse` undeclared.

- [ ] **Step 3: Implement `unparse`.** A recursive descent over `Ast::nodes_`. For each `Expr`:
  - **Leaf field** (`Kind` = field/ident): emit the field name via `strings_[name_id]`.
  - **Const** (`Kind` = const/number): emit `value` with **enough precision to round-trip a double** (use `std::format("{}", value)` or a shortest-round-trip path; the fractional-const test `8.22237` is the guard — if the canonical key folds constants, match its representation).
  - **Op node:** look up the op NAME from the registry by `opcode` (`OpSig::name`), emit `name(` then the present children `a`/`b`/`c` (only `min_arity..` that exist) comma-joined, then any `hparams[0..n_hparams)` as trailing numeric args **in the slot order the parser expects** (confirm whether windows are children or hparams from the parser — `ts_mean(close, 8)`: is `8` a child const or an hparam? render it back in the SAME position so it re-parses).
  - Provide both `unparse(const Ast&, ExprId root)→std::string` and a whole-Ast overload `unparse(const Ast&)→std::string` (renders the single/anonymous root).
  - Header-only, `namespace atx::engine::alpha`, `[[nodiscard]]`, no allocation in a hot path is required (this is record-time, not eval-time).

- [ ] **Step 4: Run the tests, verify all pass.** Run: `…/atx-engine-tests --gtest_filter=AlphaUnparse.*`. Expected: 7/7 PASS. Iterate the printer until the round-trip holds for every fixture (this is TDD — the hash equality drives the exact textual form).

- [ ] **Step 5: Full-suite regression + warnings-clean build.** Run the whole `atx-engine-tests` + the strict build. Expected: all green, zero `/W4 /WX` warnings.

- [ ] **Step 6: Commit**

```bash
git add -- atx-engine/include/atx/engine/alpha/unparse.hpp atx-engine/tests/alpha_unparse_test.cpp
git commit -m "feat(s4b-1): unparse(Ast)->string with round-trip-through-canonical-hash soundness

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git merge-base --is-ancestor HEAD HEAD; echo "ancestor:$?"
git show HEAD --stat
```
Expected: ancestor:0; stat shows ONLY the two S4b-1 files.

---

## Task S4b-2: Library-backed pool-aware fitness (the scale lever)

Introduce a `PoolView` seam so `pool_aware_fitness` scores marginal correlation against either backing with **one** corr implementation per backing. **Lock:** the single operation is `worst_corr(span<const f64> pnl) const → f64` = **max |corr|** of the candidate against the pool. Both backings return max so they agree; the new fitness overload defines `redundancy = worst_corr`, `diversify = clamp(1 − redundancy, 0, 1)`. The **legacy** `pool_aware_fitness(const combine::AlphaStore&, …)` (mean-based `corr_to_pool`) is **untouched** — the green S3 suite depends on it.

**Files:**
- Create: `atx-engine/include/atx/engine/factory/pool_view.hpp`
- Modify: `atx-engine/include/atx/engine/factory/fitness.hpp` (add an overload; do not change the existing one)
- Modify: `atx-engine/include/atx/engine/library/library.hpp` (add ONE additive read-only accessor)
- Test: `atx-engine/tests/factory_pool_view_test.cpp`

- [ ] **Step 1: Add the `Library::worst_corr_to_pool` accessor (additive).** In `library.hpp`, add a public const method that exposes exactly what `verdict_for` already computes internally (do not duplicate the corr math — factor the existing call):

```cpp
/// Max |corr| of `pnl` against the current pool, via the SimHash neighbor scan
/// (O(neighbors)). Empty pool => 0.0 (matches the empty-pool gate convention).
/// Read-only: the corr index's reusable scratch is logically const (the same
/// const_cast verdict_for already uses).
[[nodiscard]] atx::f64 worst_corr_to_pool(std::span<const atx::f64> pnl) const {
  if (!corr_.has_value()) { return 0.0; }
  CorrNeighborIndex &idx = const_cast<CorrNeighborIndex &>(*corr_); // logical-const scratch
  return online_corr_to_pool(pnl, store_, idx);
}
```
> This is the ONLY change to the merged S4 facade — additive, under explicit-pathspec discipline. (Optionally refactor `verdict_for`'s corr line to call this, to guarantee one code path — keep behavior identical.)

- [ ] **Step 2: Write the `PoolView` seam** in `pool_view.hpp`:

```cpp
#pragma once
#include <span>
#include "atx/core/types.hpp"
#include "atx/engine/combine/store.hpp"        // combine::AlphaStore
#include "atx/engine/factory/fitness.hpp"      // corr_to_pool, Reduce
#include "atx/engine/library/library.hpp"      // library::Library

namespace atx::engine::factory {

// The single operation fitness needs from a pool: max |corr| of a candidate pnl
// against the pool. Two backings, ONE corr implementation each, both return max.
struct PoolView {
  virtual ~PoolView() = default;
  [[nodiscard]] virtual atx::f64 worst_corr(std::span<const atx::f64> pnl) const = 0;
};

// O(N) exact scan — the ephemeral store (S3 fixtures).
class AlphaStorePool final : public PoolView {
 public:
  explicit AlphaStorePool(const combine::AlphaStore &pool) noexcept : pool_{pool} {}
  [[nodiscard]] atx::f64 worst_corr(std::span<const atx::f64> pnl) const override {
    return corr_to_pool(pnl, pool_, Reduce::Max);   // confirm the corr_to_pool signature
  }
 private:
  const combine::AlphaStore &pool_;
};

// O(neighbors) SimHash scan — the persistent library (scale / the real engine).
class LibraryPool final : public PoolView {
 public:
  explicit LibraryPool(const library::Library &lib) noexcept : lib_{lib} {}
  [[nodiscard]] atx::f64 worst_corr(std::span<const atx::f64> pnl) const override {
    return lib_.worst_corr_to_pool(pnl);
  }
 private:
  const library::Library &lib_;
};

} // namespace atx::engine::factory
```
> Implementer: confirm `corr_to_pool(span, const AlphaStore&, Reduce)` exact signature (fitness.hpp:109-129) and the `Reduce::Max` enumerator name; adapt.

- [ ] **Step 3: Add the `pool_aware_fitness` overload** taking a `const PoolView&` instead of `const combine::AlphaStore&`. Body is the existing function with the corr term swapped: `redundancy = view.worst_corr(candidate_pnl)`, `diversify = clamp(1 - redundancy, 0, 1)`; everything else (wq, robust, raw = wq×diversify×robust, dsr via deflated_sharpe at `cfg.trial_count`, haircut) identical. Factor the shared body so wq/dsr math is not duplicated (DRY — extract a private helper that takes the precomputed `redundancy`).

- [ ] **Step 4: Write the failing equality + cost test:**

```cpp
// atx-engine/tests/factory_pool_view_test.cpp  (sketch — wire to the existing
// factory test fixtures for Panel/sim/policy; mirror factory_integration_test.cpp setup)
TEST(FactoryPoolView, LibraryBackedDiversifyMatchesAlphaStore) {
  // Build a small pool of K alphas in BOTH a combine::AlphaStore and a temp-dir
  // library (same pnl streams). For a candidate pnl, assert:
  //   AlphaStorePool.worst_corr(c) == LibraryPool.worst_corr(c)  (within S4-3 recall:
  //   use a fixture where recall==1.0 — reuse the orthogonal-basis fixture pattern
  //   from the S4-5 corr tests so neighbors are exact).
  EXPECT_NEAR(store_view.worst_corr(cand_pnl), lib_view.worst_corr(cand_pnl), 1e-12);
}
TEST(FactoryPoolView, FitnessOverloadAgreesAcrossBackings) {
  // pool_aware_fitness(genome, AlphaStorePool{store}, ...).diversify
  //   == pool_aware_fitness(genome, LibraryPool{lib}, ...).diversify   (same recall caveat)
}
```
> Use a recall==1.0 fixture (orthogonal/equal-norm basis, as in the S4-5 corr tests) so the SimHash neighbor set is exact and the equality is non-vacuous. Document the recall caveat in a comment.

- [ ] **Step 5: Run → fail (overload undeclared / values differ), implement, run → pass.** `--gtest_filter=FactoryPoolView.*`.

- [ ] **Step 6: Full-suite regression** (the legacy `pool_aware_fitness` path and all S3 factory tests MUST stay green — proof the legacy overload was untouched). Strict build clean.

- [ ] **Step 7: Commit**

```bash
git add -- atx-engine/include/atx/engine/factory/pool_view.hpp atx-engine/include/atx/engine/factory/fitness.hpp atx-engine/include/atx/engine/library/library.hpp atx-engine/tests/factory_pool_view_test.cpp
git commit -m "feat(s4b-2): PoolView seam — library-backed pool-aware fitness at O(neighbors)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git merge-base --is-ancestor HEAD HEAD; echo "ancestor:$?"
git show HEAD --stat
```
Expected: ancestor:0; stat shows ONLY the four S4b-2 files.

---

## Task S4b-3: Factory→library admit bridge (`mine_into`)

Add the **real** admit path. The existing ephemeral-`AlphaStore` `mine()` stays test-only for the S3 suite. Two entry points, shared internals.

**Files:**
- Modify: `atx-engine/include/atx/engine/factory/factory.hpp`
- Test: `atx-engine/tests/factory_mine_into_test.cpp`
- **Read first:** `factory.hpp:178-287` (`mine`), `:329-354` (`rank_by_deflated_fitness`), `:302-308` (`detail_eval_streams`/`extract_streams`).

- [ ] **Step 1: Extend `FactoryReport`** with library-growth telemetry (additive fields, default-initialized so `mine()` is unaffected):

```cpp
struct FactoryReport {
  // ... existing fields (admitted, evaluated, dedup_pct, cse_pct, trials, seed, digest) ...
  atx::usize duplicates{0};                 // library-wide dedup hits (S4b)
  atx::u64   library_n_alphas_before{0};    // library.n_alphas() at run start
  atx::u64   library_n_alphas_after{0};     // ... at run end (delta = admitted)
  std::array<atx::usize, 6> reject_histogram{}; // indexed by library::AdmitKind
};
```

- [ ] **Step 2: Add a `PoolView` overload of `rank_by_deflated_fitness`** (so ranking can score against the library): same body, replace `const combine::AlphaStore& pool` with `const PoolView& pool` and the inner `pool_aware_fitness(..., pool, ...)` call with the S4b-2 overload. Keep the existing `AlphaStore` overload for `mine()`.

- [ ] **Step 3: Write `Factory::mine_into`:**

```cpp
[[nodiscard]] FactoryReport mine_into(const FactoryConfig &cfg,
                                      library::Library &lib,
                                      const combine::AlphaGate &gate) {
  FactoryReport rep{};
  rep.library_n_alphas_before = lib.n_alphas();

  // 1. Run the seeded search (same SearchDriver path as mine()).
  SearchDriver driver{lib_for_search_, panel_, policy_, sim_, cfg.seed_exprs, cfg.panel_fields};
  const SearchResult res = driver.run(cfg.search, /* pool view? */);
  rep.evaluated = res.trial_count; rep.trials = res.trial_count; rep.seed = res.seed;

  // 2. Deflation N = the running trial count (the 5f57a34 auto-scaling, carried forward).
  FitnessCfg admit_fit = cfg.search.fitness;
  if (res.trial_count > 0U) { admit_fit.trial_count = res.trial_count; }

  // 3. Rank against the LIBRARY pool (O(neighbors)).
  LibraryPool view{lib};
  std::vector<Ranked> ranked = rank_by_deflated_fitness(res.all_scored, admit_fit, view);

  // 4. Per ranked candidate: realize streams -> metrics -> dsr -> AlphaCandidate -> gate+admit.
  Digest dg{res.digest};                                  // fold each admission decision (F1/F2)
  for (const Ranked &r : ranked) {
    const Genome &g = res.all_scored[r.idx];
    auto streams = detail_eval_streams(g);                // owned pnl + pos_flat
    if (!streams.has_value()) { continue; }               // 0-alpha guard (S3-6 705cf22)
    const combine::AlphaMetrics m =
        combine::compute_metrics(streams->pnl, streams->pos_flat, n_instruments_, cfg.book_size);
    const auto fr = pool_aware_fitness(g, view, panel_, policy_, sim_, admit_fit);
    if (!fr.has_value()) { continue; }
    const bool deflation_ok = fr->dsr >= cfg.min_dsr;     // the S1 bar stays factory-side
    library::Provenance prov{ alpha::unparse(g.ast),      // S4b-1
                              g_parent_hashes(g), g_mutation_op(g), res.seed };
    library::AlphaCandidate cand{ g.canon_hash, streams->pnl, streams->pos_flat, m,
                                  std::move(prov), cfg_as_of_, /*source=*/nullptr };
    library::AdmitKind kind = library::AdmitKind::RejectFitness; // sentinel if we skip admit
    if (deflation_ok) {
      const library::AdmitVerdict v = lib.admit(cand, gate);
      kind = v.kind;
      if (kind == library::AdmitKind::Accept)    { ++rep.admitted; }
      if (kind == library::AdmitKind::Duplicate) { ++rep.duplicates; }
    }
    ++rep.reject_histogram[static_cast<atx::usize>(kind)];
    dg.fold(static_cast<atx::u64>(kind));                 // admission decision into the digest
    dg.fold(g.canon_hash);
  }
  rep.digest = dg.value();
  rep.library_n_alphas_after = lib.n_alphas();
  // dedup_pct / cse_pct as in mine().
  return rep;
}
```
> Implementer: this is the integration shape, not final — reconcile names with the real `mine()` body (`SearchDriver` ctor args, `detail_eval_streams` field names `pnl`/`pos_flat`, `Ranked::idx`, the digest-folding helper `mine()` already uses, how `mine()` derives `n_instruments`, `as_of`, and the parent_hashes/mutation_op accessors on `Genome`). Reuse `mine()`'s exact helpers — do NOT invent a second digest or streams path. `unparse` comes from S4b-1.

- [ ] **Step 4: Write the failing bridge test:**

```cpp
TEST(FactoryMineInto, MinesAndAdmitsIntoPersistentLibrary) {
  // temp-dir library; real-signal panel (reuse factory_integration_test fixtures).
  library::Library lib = library::Library::open(tmpdir, gate_cfg, {kSeed});
  Factory f{lib_dsl, panel, sim, policy};
  FactoryReport rep = f.mine_into(cfg, lib, gate);
  EXPECT_GT(rep.admitted, 0u);
  EXPECT_EQ(lib.n_alphas(), rep.library_n_alphas_after);
  EXPECT_EQ(rep.library_n_alphas_after - rep.library_n_alphas_before, rep.admitted);
  // every admitted alpha carries a round-trippable formula:
  // for each admitted id, parse(get(id).provenance.expr_source) -> same canonical_hash. (light check)
}
TEST(FactoryMineInto, DeflationBarStaysFactorySide) {
  // pure-noise panel: rep.admitted == 0 even though library::admit alone (no dsr bar) might pass.
}
TEST(FactoryMineInto, SeededRunFoldsAdmissionsIntoDigest) {
  // two mine_into runs, same seed, fresh temp-dirs -> equal rep.digest.
}
```

- [ ] **Step 5: Run → fail, implement, run → pass** (`--gtest_filter=FactoryMineInto.*`), then full-suite regression (S3 `FactoryIntegration` MUST stay green — proof `mine()` untouched) + strict build clean.

- [ ] **Step 6: Commit**

```bash
git add -- atx-engine/include/atx/engine/factory/factory.hpp atx-engine/tests/factory_mine_into_test.cpp
git commit -m "feat(s4b-3): Factory::mine_into — factory->library admit bridge (deflation factory-side)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git merge-base --is-ancestor HEAD HEAD; echo "ancestor:$?"
git show HEAD --stat
```
Expected: ancestor:0; stat shows ONLY the two S4b-3 files.

---

## Task S4b-4: `ResearchDriver` — the continuous automated engine

Owns a `library::Library` and drives a budget-bounded mine→admit→repeat loop over a **fixed** research panel until a stop condition (budget exhausted OR K consecutive runs admit nothing new). Seed axis extends to `(master_seed, run, gen, idx)`.

**Files:**
- Create: `atx-engine/include/atx/engine/factory/research_driver.hpp`
- Test: `atx-engine/tests/factory_research_driver_test.cpp`

- [ ] **Step 1: Define `ResearchConfig` + `ResearchReport`:**

```cpp
struct ResearchConfig {
  FactoryConfig per_run;        // the inner mine config (search budget per run)
  atx::usize max_runs;          // hard budget
  atx::usize patience;          // stop after `patience` consecutive zero-admit runs (novelty exhaustion)
  atx::u64   master_seed;       // the engine seed; per-run seed = mix(master_seed, run)
};
struct ResearchReport {
  atx::usize runs;
  atx::usize total_mined;
  atx::usize total_admitted;
  atx::usize total_duplicates;
  atx::u64   library_size;
  std::array<atx::usize, 6> lifecycle_histogram{}; // by library::LifecycleState
  atx::f64   dedup_pct;
  atx::u64   digest;            // folds every per-run FactoryReport.digest in run order
  atx::u32   manifest_version_id;
  atx::u64   seed;
};
```

- [ ] **Step 2: Write `ResearchDriver`:**

```cpp
class ResearchDriver {
 public:
  ResearchDriver(library::Library &lib, const alpha::Panel &panel,
                 const exec::ExecutionSimulator &sim, const WeightPolicy &policy,
                 const combine::AlphaGate &gate) noexcept;

  [[nodiscard]] ResearchReport run(const ResearchConfig &cfg) {
    ResearchReport rep{}; rep.seed = cfg.master_seed;
    Digest dg{cfg.master_seed};
    atx::usize dry = 0;
    Factory factory{lib_dsl_, panel_, sim_, policy_};
    for (atx::usize r = 0; r < cfg.max_runs && dry < cfg.patience; ++r) {
      FactoryConfig run_cfg = cfg.per_run;
      run_cfg.search.master_seed = detail::seed_for_run(cfg.master_seed, r); // (master,run)->run seed
      const FactoryReport fr = factory.mine_into(run_cfg, lib_, gate_);
      rep.runs = r + 1; rep.total_mined += fr.evaluated;
      rep.total_admitted += fr.admitted; rep.total_duplicates += fr.duplicates;
      dg.fold(fr.digest);
      dry = (fr.admitted == 0) ? dry + 1 : 0;
    }
    const library::LibraryManifest m = lib_.snapshot();   // checkpoint (flushes + seals)
    rep.library_size = lib_.n_alphas();
    rep.manifest_version_id = m.version_id;
    rep.digest = dg.value();
    // dedup_pct + lifecycle_histogram from the library state.
    return rep;
  }
 private:
  library::Library &lib_;
  // ... panel_, sim_, policy_, gate_, lib_dsl_ ...
};
```
> `detail::seed_for_run(master, run)` is a new pure SplitMix mix (mirror `detail::seed_for`'s style — NO worker/thread/time). Per-run it sets `SearchConfig.master_seed`, so `SearchDriver`'s existing `(gen, idx)` derivation composes to the full `(master, run, gen, idx)` without touching `SearchDriver`.

- [ ] **Step 3: Write the failing tests:**

```cpp
TEST(ResearchDriver, GrowsLibraryAcrossRuns) {
  // real-signal panel: after run(), lib.n_alphas() > 0 and rep.total_admitted == growth.
}
TEST(ResearchDriver, StopsOnPatienceWhenNoveltyExhausted) {
  // tiny grammar -> after `patience` zero-admit runs, rep.runs < max_runs.
}
TEST(ResearchDriver, CheckpointResumeReplaysIdentically) {
  // run engine -> snapshot version_id V; reopen library at same dir -> snapshot version_id == V
  //   (rebuild_equals(manifest, dir, cfg, seeds) == true).
}
```

- [ ] **Step 4: Run → fail, implement, run → pass** (`--gtest_filter=ResearchDriver.*`), full-suite regression + strict build clean.

- [ ] **Step 5: Commit**

```bash
git add -- atx-engine/include/atx/engine/factory/research_driver.hpp atx-engine/tests/factory_research_driver_test.cpp
git commit -m "feat(s4b-4): ResearchDriver — continuous mine->admit->repeat over a fixed panel

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git merge-base --is-ancestor HEAD HEAD; echo "ancestor:$?"
git show HEAD --stat
```
Expected: ancestor:0; stat shows ONLY the two S4b-4 files.

---

## Task S4b-5: E2E capstone + anti-snooping-at-scale + bench + close

**Files:**
- Test: `atx-engine/tests/research_engine_test.cpp`
- Bench: `atx-engine/bench/research_bench.cpp`
- Modify (close): `sprint-4b-progress.md`, `ROADMAP.md`; Create `sprint-4b.md`; mark spec `✅ closed`.

- [ ] **Step 1: Write the capstone E2E suite** `ResearchEngine` (non-vacuous: fail-on-bad AND pass-on-good):

```cpp
TEST(ResearchEngine, SeededEngineReplaysByteIdentical) {            // F1
  // two ResearchDriver runs, SAME ResearchConfig, fresh temp-dir libraries:
  //   EXPECT_EQ(repA.digest, repB.digest);
  //   EXPECT_EQ(repA.manifest_version_id, repB.manifest_version_id);
}
TEST(ResearchEngine, NoiseGrowsLibraryByNothing) {                  // F4 at scale
  // pure-noise panel, large budget -> rep.total_admitted == 0 (deflation N = running trial count).
  // real-signal panel, same gate+dsr bar -> rep.total_admitted > 0.
}
TEST(ResearchEngine, CrossRunDedupNeverReadmits) {                  // F6
  // seed run-2 with an expression structurally-equivalent to a run-1 admit ->
  //   it is counted in duplicates, library size unchanged by it.
}
TEST(ResearchEngine, UnparseRoundTripsThroughCanonicalHash) {      // S4b-1 at scale
  // for EVERY admitted alpha: parse(get(id).provenance.expr_source) -> same canonical_hash
  //   as the stored canon_hash.
}
```

- [ ] **Step 2: Run → (fail/iterate) → all pass.** `--gtest_filter=ResearchEngine.*`. These ride entirely on S4b-1…S4b-4 — if a contract is weak, fix the unit, not the test.

- [ ] **Step 3: Write the bench** `research_bench.cpp` (Google Benchmark form, mirror `factory_bench.cpp`): report **alphas mined/sec**, **admitted/hour**, **dedup %**, library **growth curve**, and the **online-corr-to-pool speedup vs the O(N) `AlphaStorePool` scan** (the scale-lever number — bench `LibraryPool.worst_corr` vs `AlphaStorePool.worst_corr` at growing pool sizes). **No ideal-speedup claim** — report the measured ratio only.

- [ ] **Step 4: Build the bench** (`-DATX_BUILD_BENCH=ON`, target `atx-engine-bench`) — confirm it compiles and runs (a few iterations); record the headline numbers for the ledger.

- [ ] **Step 5: Full-suite regression.** Entire `atx-engine-tests` green; strict `/W4 /permissive- /WX` build clean. Confirm `git diff 518af61..HEAD -- atx-core/` is EMPTY (atx-core untouched).

- [ ] **Step 6: Close ceremony** (per [`../docs/sprint.md`](../docs/sprint.md)):
  - Fill `sprint-4b-progress.md`: flip every unit row ✅ with its commit SHA; complete the commits table; write **"What S4b proves"** (F1 whole-engine replay incl. manifest version_id; F4 noise→~0 at scale; F6 cross-run dedup; the O(neighbors) speedup number; unparse soundness) + the **baton** to S5 (combiner over the now-populated library) / S7 (re-eval adapter + decay monitor); lift any **residuals**.
  - `ROADMAP.md`: add an **S4b** row between S4 and S5 (✅ CLOSED + close SHA), bump `Last reviewed` to the close date. (Do NOT touch p0 ROADMAP.)
  - Mark the spec `sprint-4b-automated-alpha-engine.md` header `✅ CLOSED` with the close SHA.
  - Create `sprint-4b.md` (the user reference: the `ResearchDriver`/`mine_into`/`PoolView`/`unparse` public API + the five proven contracts), mirroring `sprint-3.md`'s shape.

- [ ] **Step 7: Commit (close)**

```bash
git add -- atx-engine/tests/research_engine_test.cpp atx-engine/bench/research_bench.cpp atx-engine/plans/p1/sprint-4b-progress.md atx-engine/plans/p1/ROADMAP.md atx-engine/plans/p1/sprint-4b-automated-alpha-engine.md atx-engine/plans/p1/sprint-4b.md
git commit -m "feat(s4b-5): E2E research engine — F1/F4/F6 + unparse soundness, bench, close S4b

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git merge-base --is-ancestor HEAD HEAD; echo "ancestor:$?"
git show HEAD --stat
```
Expected: ancestor:0; stat shows ONLY the six S4b-5 files (no atx-core, no foreign files).

---

## Self-review (run after the plan, before execution)

**Spec coverage:** S4b-0 ledger ✅ · S4b-1 unparse+round-trip ✅ · S4b-2 PoolView + library-backed fitness + `worst_corr_to_pool` accessor ✅ · S4b-3 `mine_into` + telemetry + deflation-factory-side ✅ · S4b-4 `ResearchDriver` + `(master,run,gen,idx)` + checkpoint/resume ✅ · S4b-5 F1/F4/F6 + unparse-at-scale + bench + close ✅. The five load-bearing contracts each map to a named test (F1→SeededEngineReplaysByteIdentical incl. version_id; F4→NoiseGrowsLibraryByNothing; F6→CrossRunDedupNeverReadmits; scale-lever→bench ratio; unparse→RoundTripsThroughCanonicalHash + per-unit round-trip).

**Placeholder scan:** the deliberately-light spots (`unparse` printer body, `mine_into` helper-name reconciliation, fixture wiring) are explicitly delegated to TDD with file:line refs because guessing 50 opcodes' rendering / re-deriving `mine()`'s private helpers blind would be wrong code — the round-trip/equality/digest TESTS are fully specified and ARE the contract.

**Type consistency:** `PoolView::worst_corr(span)` used identically in `pool_view.hpp`, the fitness overload, and `rank_by_deflated_fitness`'s new overload. `FitnessReport.diversify`/`dsr`, `FactoryReport` additive fields, `library::AdmitKind` (6 values → `reject_histogram[6]`), `ResearchReport.manifest_version_id` (== `LibraryManifest.version_id`) consistent across S4b-3/4/5. `unparse(Ast, ExprId)` consistent S4b-1 → S4b-3 → S4b-5.

**Discipline:** every commit is explicit-pathspec + co-author trailer + ancestor/stat verify; atx-core-untouched check at close; no push.
