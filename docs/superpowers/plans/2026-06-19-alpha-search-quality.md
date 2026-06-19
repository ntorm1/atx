# Alpha-DSL Search Quality & Genetic-Algorithm Upgrade — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Raise search quality and per-eval efficiency of the genetic alpha-DSL search (`SearchDriver` + `Factory`) by fixing cold-start diversity, removing a dead/unsound novelty path, adding parsimony pressure, injecting diversity (immigrants + stagnation stop), and making the genetic operators adaptive — without weakening the F1/F2 determinism contracts.

**Architecture:** Five sequenced tasks, all in the `atx::engine::factory` subsystem. Each new behavior is controlled by a `SearchConfig` knob whose **default is the improved behavior**; the single frozen golden (`kGoldenDigest`, the `ScalarRaw` + legacy boundary path) is preserved by pinning those knobs to their legacy values in that one test, so determinism stays provable while the production (`MultiObjective`) path gets every improvement. No new third-party deps; store-coupled items (cross-run score cache, cumulative multiple-testing N) are explicitly deferred to a separate Phase-2 plan (see appendix).

**Tech Stack:** C++20, clang-cl + Ninja (warm Release `build-rel`), GoogleTest. Reuse `factory::generate_genome` (grammar sampler), `factory::canonical_hash`, `factory::pareto.hpp` (NSGA-II), `core::Xoshiro256pp`, `detail::seed_for`.

**Source review (rationale):** This plan implements findings #1–#4 and #7 from the structural review in this session. Findings #5 (semantic dedup), #6 (cross-run score cache), #8 (cumulative-N multiple-testing) are deferred — see the Phase-2 appendix and the per-item reasoning there.

## Global Constraints

- **Determinism is sacred (F1/F2).** Every task MUST keep: same-seed replay byte-identical, and digest worker-count-invariant. These are correctness contracts, not back-compat. The existing relative tests assert them and MUST stay green: `FactorySearchDriver.SameSeedReplaysByteIdentical`, `...EvalDigestIsWorkerInvariant...`, the worker-invariance run test, `FactoryBehavior` determinism/worker tests, `factory_oos_test` replay tests. NEVER introduce wall-clock / `Math.random` / thread-id / address into any engine determinism path. All RNG seeds via `detail::seed_for(master, axis, idx)`.
- **The frozen golden is the boundary anchor.** Exactly ONE absolute golden exists: `kGoldenDigest = 0xa83f0d3e0b41a18dULL` in `atx-engine/tests/factory/factory_nsga_search_test.cpp` (ScalarRaw, 96×6 fixture, seed 777). It MUST stay byte-identical. Mechanism: every trajectory-changing feature is gated by a `SearchConfig` knob; the boundary-pin config sets each knob to its legacy value. Task 1 introduces a single `legacy_pin_cfg()` helper in that test file and every later task adds ONE line pinning its new knob. If a task's verification shows the golden changed, the FIRST fix is to pin the knob — do NOT re-baseline the hex unless a knob genuinely cannot reproduce the legacy path (it always can here).
- **Knob defaults = improved behavior.** New `SearchConfig` fields default to the better behavior; `ScalarRaw` mode + the legacy-pin values reproduce today's output. `MultiObjective` (the struct default `objective_mode`) is the production path and gets all improvements by default.
- **Back-compat may break on the MultiObjective path.** Removing `novelty_w` and changing `MultiObjective` trajectories is allowed and expected (no absolute golden pins them). Update all references.
- **Namespaces:** search/genome/factory types are `atx::engine::factory`; fitness objective layout lives in `atx/engine/factory/fitness.hpp`.
- **Build dir:** `c:/Users/natha/OneDrive/Desktop/atx/build-rel` (warm Release). If absent/stale, configure with the dev preset and `-DATX_UNITY_BUILD=OFF` (pre-existing factory-test ODR collision under unity). `/W4 /WX` must stay clean.
- **Build command (per task):** `cmake --build c:/Users/natha/OneDrive/Desktop/atx/build-rel --target atx-engine-factory-tests`
- **Test command (per task):** run the whole factory group via ctest (gtest suite names are case-sensitive):
  `ctest --test-dir c:/Users/natha/OneDrive/Desktop/atx/build-rel -R 'NsgaSearch|FactorySearchDriver|FactoryBehavior|FactoryCost|FactoryOos|FactoryIntegration|FactoryMineInto|SearchProgress|Parsimony|RampedInit|Immigrant|AdaptiveOperator' --output-on-failure`
  (or run the built `atx-engine-factory-tests` executable with no filter to sweep the entire group).
- Test files are auto-discovered: `atx-engine/tests/CMakeLists.txt` GLOBs `factory/*_test.cpp` (`CONFIGURE_DEPENDS`). A new `*_test.cpp` needs NO CMake edit.
- NEVER `git add -A`; stage explicit paths. Commit locally when green (authorized): trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push. Do NOT edit this plan/spec during execution.
- **Worktree:** execute in an isolated worktree created via `superpowers:using-git-worktrees`; init submodules (`git submodule update --init --recursive`).

## Objective-vector layout (shared contract)

`FitnessReport.objectives` is a fixed `std::array<f64, kMaxObjectives>`. NSGA-II (`pareto.hpp`) maximizes every column; inactive columns must be UNIFORM across genomes (→ inert in dominance). Current + new slots:

| Slot | Meaning | Sign | Set by |
|------|---------|------|--------|
| 0 | `wq` (OOS WorldQuant fitness) | maximize | `finish_report` |
| 1 | `diversify` = 1−redundancy | maximize | `finish_report` |
| 2 | `robust` | maximize | `finish_report` |
| 3 | behavioral novelty | maximize | `behavioral_novelty_pass` (when active) |
| 4 | `−cost_bps` | maximize (cheaper better) | `finish_report` (when cost active) |
| **5** | **`−node_count` (parsimony)** | **maximize (smaller better)** | **`evaluate_generation` scoring (Task 2)** |

`kMaxObjectives` grows 5 → 6 in Task 2.

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `atx-engine/include/atx/engine/factory/search_driver.hpp` (mod) | `SearchConfig` knobs; remove `canonical_distance` + `novelty_penalize` decl; add immigrant/adaptive/stop decls | 1,3,4,5 |
| `atx-engine/src/factory/search_driver.cpp` (mod) | remove `novelty_penalize`; `behavioral_active`; parsimony objective; ramped init; immigrants + early-stop; adaptive operators + annealed σ | 1,2,3,4,5 |
| `atx-engine/include/atx/engine/factory/fitness.hpp` (mod) | `kMaxObjectives` 5→6; `kObjParsimony` constant | 2 |
| `atx-engine/include/atx/engine/factory/generate.hpp` (read) | `GenConfig`, `generate_genome` (consumed by ramped init) | 3 |
| `atx-engine/tests/factory/factory_nsga_search_test.cpp` (mod) | `legacy_pin_cfg()` helper; pin each new knob | 1,2,3,4,5 |
| `atx-engine/tests/factory/factory_behavior_test.cpp` (mod) | migrate `novelty_w` → `enable_behavioral_novelty` | 1 |
| `atx-engine/tests/factory/factory_search_driver_test.cpp` (mod) | migrate `small_search_cfg` off `novelty_w` | 1 |
| `atx-engine/tests/factory/search_quality_test.cpp` (new) | new behavior tests: parsimony, ramped init, immigrants, stop, adaptive ops | 2,3,4,5 |

---

### Task 1: Untangle the novelty knobs — delete the dead, unsound structural-novelty penalty

**Why:** `canonical_distance` ([search_driver.hpp:218-221](../../atx-engine/include/atx/engine/factory/search_driver.hpp#L218-L221)) is `popcount(h_a ^ h_b)/64` over canonical *hashes* — a hash avalanches, so a one-node difference and a totally-unrelated tree both score ~0.5. It is not a structural distance. `novelty_penalize` writes it into `.selection`, which in the default `MultiObjective` mode is **never read** (tournament uses `crowded_better`; elitism uses raw). Worse, `novelty_w` doubles as the on/off gate for the *real* (behavioral) novelty objective. Split the gate, delete the dead path.

**Files:**
- Modify: `atx-engine/include/atx/engine/factory/search_driver.hpp` — `SearchConfig` (remove `novelty_w`, add `enable_behavioral_novelty`); delete `canonical_distance` (218-221) and the `novelty_penalize` declaration.
- Modify: `atx-engine/src/factory/search_driver.cpp` — delete `novelty_penalize` definition + its call (line 111); rewrite `behavioral_active`; ensure `.selection` is set to `.fitness`.
- Modify: tests that reference `novelty_w` (see Step 1).

**Interfaces:**
- Produces: `SearchConfig::enable_behavioral_novelty` (bool, default `true`) replacing `novelty_w`.
- `behavioral_active(cfg) == (cfg.objective_mode == ObjectiveMode::MultiObjective && cfg.enable_behavioral_novelty)`.
- `.selection` field retained on `Scored` (still used by ScalarRaw tournament) but now equals `.fitness` (no penalty).

- [ ] **Step 1: Enumerate every `novelty_w` reference (repo-wide)**

Run: `rg -n "novelty_w|novelty_penalize|canonical_distance" c:/Users/natha/OneDrive/Desktop/atx/atx-engine`
Expected hits (must all be handled in this task): `search_driver.hpp` (field + `canonical_distance` + `novelty_penalize` decl + doc comments referencing `novelty_w` in the `SearchConfig`/`behavioral_active` comments), `search_driver.cpp` (`novelty_penalize` def, call at :111, `behavioral_active` at :506-508), `factory_behavior_test.cpp` (sets `novelty_w` > 0 to enable behavioral novelty), `factory_search_driver_test.cpp` (`small_search_cfg` sets `cfg.novelty_w = 0.1`), `factory_nsga_search_test.cpp` (boundary-pin sets novelty OFF). Record the list.

- [ ] **Step 2: Write the failing test (behavioral novelty toggles via the new flag)**

Add to a new file `atx-engine/tests/factory/search_quality_test.cpp`:

```cpp
// atx-engine/tests/factory/search_quality_test.cpp
#include <gtest/gtest.h>
#include <vector>
#include <string>
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atx::engine::factory {
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::loop::WeightPolicy;
using atx::engine::combine::AlphaStore;

// --- Fixture (mirrors factory_search_driver_test.cpp) ---------------------
namespace {
ExecutionSimulator frictionless_sim() {
  using namespace atx::engine::exec;
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{}, VolumeCapCfg{1.0}};
}
struct Lcg { std::uint64_t s;
  double next() noexcept { s = s*6364136223846793005ULL + 1442695040888963407ULL;
    return 2.0*(static_cast<double>(s>>11U)/static_cast<double>(1ULL<<53U)) - 1.0; } };
Panel fixture_panel(atx::usize dates, atx::usize insts) {
  std::vector<double> drift(insts);
  for (atx::usize j=0;j<insts;++j) drift[j]=0.006-0.0024*static_cast<double>(j);
  std::vector<double> close(dates*insts), px(insts,100.0); Lcg rng{0xA11Cu};
  for (atx::usize t=0;t<dates;++t) for (atx::usize j=0;j<insts;++j){
    px[j]*=(1.0+drift[j]+0.010*rng.next()); close[t*insts+j]=px[j]; }
  std::vector<double> rev(dates*insts,0.0);
  for (atx::usize t=1;t<dates;++t) for (atx::usize j=0;j<insts;++j){
    rev[t*insts+j] = -(close[t*insts+j]/close[(t-1)*insts+j]-1.0); }
  auto r = Panel::create(dates, insts, {"close","rev"}, {close, rev}, {});
  EXPECT_TRUE(r.has_value()); return std::move(r.value());
}
std::vector<std::string> seed_exprs() {
  return {"rank(close)","rank(rev)","ts_mean(close, 5)","ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))","delta(close, 2)"}; }
struct Fixture {
  Library lib{}; Panel panel = fixture_panel(96,6); WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver() { return SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close","rev"}}; }
};
SearchConfig base_cfg(atx::u64 seed) {
  SearchConfig c; c.master_seed=seed; c.population=16; c.generations=4;
  c.elites=2; c.k_tournament=3; c.p_cross=0.5; return c; }
} // namespace

// Behavioral novelty now toggles via enable_behavioral_novelty (NOT novelty_w).
TEST(SearchNoveltyKnob, BehavioralNoveltyTogglesDigest) {
  Fixture fx; auto d = fx.driver();
  AlphaStore pool_on{}, pool_off{};
  SearchConfig on = base_cfg(777);  on.objective_mode = ObjectiveMode::MultiObjective;
  on.enable_behavioral_novelty = true;
  SearchConfig off = on; off.enable_behavioral_novelty = false;
  const auto r_on  = d.run(on,  pool_on);
  const auto r_off = d.run(off, pool_off);
  // Behavioral novelty is a live 4th objective when ON -> different survivor set/digest.
  EXPECT_NE(r_on.digest, r_off.digest);
}
} // namespace atx::engine::factory
```

- [ ] **Step 3: Run the test — verify it fails to compile (`enable_behavioral_novelty` undefined)**

Run: `cmake --build c:/Users/natha/OneDrive/Desktop/atx/build-rel --target atx-engine-factory-tests`
Expected: error — `SearchConfig` has no member `enable_behavioral_novelty`.

- [ ] **Step 4: Edit `SearchConfig` — replace `novelty_w` with `enable_behavioral_novelty`**

In `search_driver.hpp`, in `struct SearchConfig`, DELETE the line:
```cpp
  atx::f64 novelty_w{0.1};    // weight of the anti-collapse novelty penalty
```
and the now-stale doc lines referencing the "ScalarRaw canonical-hash penalty" / `novelty_w` gate (the block at ~129-136). Replace the behavioral block with:
```cpp
  // S4.2 behavioral / phenotypic diversity. The behavioral novelty objective
  // (objectives[3]) is ACTIVE iff objective_mode == MultiObjective AND
  // enable_behavioral_novelty. This is the ONLY novelty mechanism: the old
  // structural canonical-hash penalty (a Hamming distance over hashes) was unsound
  // and is removed. `behavior_metric` selects the profile distance (PnlCorr default;
  // RankIc fork). `behavior_archive_cap` is the FIFO capacity C of the past-elite
  // descriptor archive; `behavior_k` is the k-nearest count for the novelty mean.
  bool enable_behavioral_novelty{true};
  BehaviorMetric behavior_metric{BehaviorMetric::PnlCorr};
  atx::usize behavior_archive_cap{64};
  atx::usize behavior_k{3};
```

- [ ] **Step 5: Delete `canonical_distance` and the `novelty_penalize` declaration**

In `search_driver.hpp`, DELETE the `canonical_distance` function (the doc block + body at 213-221). DELETE the `novelty_penalize` declaration (the `// ----- (3) novelty_penalize` comment block + the decl, ~lines around the private section).

- [ ] **Step 6: Edit `search_driver.cpp` — remove the penalty, fix `.selection`, fix `behavioral_active`**

(a) DELETE the `novelty_penalize(...)` definition (around 465-499).
(b) At the call site (line ~111), DELETE `novelty_penalize(scored, pool, cfg);`. The `.selection` field is already initialized to `.fitness` in `evaluate_generation` (`Scored s{g.clone(), cs.raw, cs.raw};` — third arg is `.selection`), so removing the penalty leaves `.selection == .fitness`. Verify that construction line still passes `cs.raw` as the third argument; no change needed there.
(c) Rewrite `behavioral_active`:
```cpp
[[nodiscard]] bool SearchDriver::behavioral_active(const SearchConfig &cfg) noexcept {
  return cfg.objective_mode == ObjectiveMode::MultiObjective && cfg.enable_behavioral_novelty;
}
```

- [ ] **Step 7: Migrate the three existing tests off `novelty_w`**

In `factory_behavior_test.cpp`: wherever a config sets `novelty_w = <positive>` to enable behavioral novelty, replace with `enable_behavioral_novelty = true;` (and any `novelty_w = 0.0` → `enable_behavioral_novelty = false;`).
In `factory_search_driver_test.cpp` `small_search_cfg`: DELETE the line `cfg.novelty_w = 0.1;`.
In `factory_nsga_search_test.cpp`: the boundary-pin config that set novelty OFF (via `novelty_w = 0.0` or the default) — replace with explicit `enable_behavioral_novelty = false;` (this is also where Step 8 adds the helper).

- [ ] **Step 8: Introduce `legacy_pin_cfg()` in the golden test**

In `factory_nsga_search_test.cpp`, add a single helper used by the boundary-pin (`kGoldenDigest`) tests that hard-pins EVERY current-and-future legacy knob, then route the golden tests through it:
```cpp
// The frozen boundary-pin config: ScalarRaw + every quality knob at its LEGACY
// value, so kGoldenDigest stays byte-identical as new knobs are added. Each new
// task appends ONE line here pinning its knob's legacy value.
SearchConfig legacy_pin_cfg(atx::u64 seed) {
  SearchConfig c;
  c.master_seed = seed;
  c.population = 16;
  c.generations = 5;
  c.elites = 2;
  c.k_tournament = 3;
  c.p_cross = 0.5;
  c.objective_mode = ObjectiveMode::ScalarRaw;   // legacy ranking
  c.enable_behavioral_novelty = false;           // Task 1: novelty off
  // (Task 2 appends: c.enable_parsimony = false;)
  // (Task 3 appends: c.seed_from_grammar = false;)
  // (Task 4 appends: c.n_immigrants = 0; c.stagnation_patience = 0;)
  // (Task 5 appends: c.adaptive_operators = false; c.jitter_anneal = false;)
  return c;
}
```
Replace the existing `kGoldenDigest`-test config construction with `legacy_pin_cfg(777)` (preserve the seed/pop/gens the golden was captured with — confirm they match the existing values; if the golden used different pop/gens, mirror them exactly here).

- [ ] **Step 9: Build + run the full factory group — golden must be byte-identical**

Run: `cmake --build c:/Users/natha/OneDrive/Desktop/atx/build-rel --target atx-engine-factory-tests`
Then: `ctest --test-dir c:/Users/natha/OneDrive/Desktop/atx/build-rel -R 'NsgaSearch|FactorySearchDriver|FactoryBehavior|SearchNoveltyKnob' --output-on-failure`
Expected: ALL pass, including `NsgaSearch.ScalarRaw_*` against `0xa83f0d3e0b41a18dULL` (byte-identical — `.selection` collapses to `.fitness` exactly as `novelty_w==0` did). If the golden changed, the boundary-pin config is not routed through `legacy_pin_cfg`; fix that, do NOT re-baseline.

- [ ] **Step 10: Commit**

```bash
git add atx-engine/include/atx/engine/factory/search_driver.hpp \
        atx-engine/src/factory/search_driver.cpp \
        atx-engine/tests/factory/search_quality_test.cpp \
        atx-engine/tests/factory/factory_behavior_test.cpp \
        atx-engine/tests/factory/factory_search_driver_test.cpp \
        atx-engine/tests/factory/factory_nsga_search_test.cpp
git commit -m "refactor(search): remove unsound structural-novelty penalty; split behavioral-novelty enable flag

canonical_distance was Hamming-over-hashes (noise, not a structural metric) and
.selection was unread in MultiObjective. Replace overloaded novelty_w with
enable_behavioral_novelty; behavioral_active gates on it. ScalarRaw boundary
golden unchanged (selection==fitness as before).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Parsimony — tree size as a minimized NSGA-II objective

**Why:** Mutation/crossover rebuild with no size cap; subtree crossover bloats trees, and bigger trees cost more to compile+evaluate (the dominant per-eval cost) and overfit. Add `−node_count` as objective slot 5 so smaller-and-equal-fitness alphas dominate — a Pareto parsimony pressure that costs no extra eval. Affects only `MultiObjective` (ScalarRaw ignores objectives), so the golden is untouched by construction.

**Files:**
- Modify: `atx-engine/include/atx/engine/factory/fitness.hpp` — `kMaxObjectives` 5→6; add `kObjParsimony`.
- Modify: `atx-engine/include/atx/engine/factory/search_driver.hpp` — `SearchConfig::enable_parsimony`.
- Modify: `atx-engine/src/factory/search_driver.cpp` — set slot 5 in the scoring merge.
- Modify: `atx-engine/tests/factory/factory_nsga_search_test.cpp` — pin `enable_parsimony=false` in `legacy_pin_cfg`.
- Modify: `atx-engine/tests/factory/search_quality_test.cpp` — parsimony test.

**Interfaces:**
- Consumes: `Genome::ast.nodes().size()`; the scoring merge block in `evaluate_generation` (search_driver.cpp ~398-462).
- Produces: `SearchConfig::enable_parsimony` (bool, default `true`); `kObjParsimony = 5`; `kMaxObjectives = 6`.

- [ ] **Step 1: Write the failing test (parsimony shrinks the admitted survivors)**

Add to `search_quality_test.cpp`:
```cpp
// Parsimony pressure: with it ON, the final survivors are no LARGER (total node
// count) than with it OFF, on the same seed/panel — and strictly smaller on at
// least one run where bloat occurs. Asserted as: mean survivor node-count(ON) <=
// mean survivor node-count(OFF).
TEST(Parsimony, ShrinksOrTiesSurvivors) {
  Fixture fx; auto d = fx.driver();
  AlphaStore pool_on{}, pool_off{};
  SearchConfig on = base_cfg(4242);
  on.objective_mode = ObjectiveMode::MultiObjective;
  on.generations = 6; on.enable_parsimony = true;
  SearchConfig off = on; off.enable_parsimony = false;
  const auto r_on  = d.run(on,  pool_on);
  const auto r_off = d.run(off, pool_off);
  auto mean_nodes = [](const std::vector<Genome>& gs) {
    if (gs.empty()) return 0.0; atx::usize tot=0;
    for (auto& g : gs) tot += g.ast.nodes().size();
    return static_cast<double>(tot)/static_cast<double>(gs.size()); };
  EXPECT_LE(mean_nodes(r_on.admitted_candidates), mean_nodes(r_off.admitted_candidates) + 1e-9);
}
```

- [ ] **Step 2: Run — verify it fails to compile (`enable_parsimony` undefined)**

Run: `cmake --build c:/Users/natha/OneDrive/Desktop/atx/build-rel --target atx-engine-factory-tests`
Expected: error — no member `enable_parsimony`.

- [ ] **Step 3: Grow the objective vector + add the slot constant**

In `fitness.hpp`, change:
```cpp
inline constexpr atx::usize kMaxObjectives = 5;
```
to:
```cpp
inline constexpr atx::usize kMaxObjectives = 6;
// Objective-slot indices (NSGA-II maximizes every column; inactive columns MUST be
// uniform across genomes -> inert). 0 wq, 1 diversify, 2 robust, 3 novelty,
// 4 -cost_bps, 5 -node_count (parsimony).
inline constexpr atx::usize kObjParsimony = 5;
```
Update the `FitnessReport.objectives` doc comment `{wq, diversify, robust, _, -cost_bps}` to `{wq, diversify, robust, novelty, -cost_bps, -node_count}`.

- [ ] **Step 4: Add the config knob**

In `search_driver.hpp` `SearchConfig`, after `enable_op_swap`, add:
```cpp
  // Parsimony pressure: when ON (MultiObjective only), objectives[kObjParsimony] =
  // -node_count makes a smaller, equally-fit tree Pareto-dominate a larger one.
  // ScalarRaw ignores objectives, so this never perturbs the boundary pin.
  bool enable_parsimony{true};
```

- [ ] **Step 5: Set the parsimony objective in the scoring merge**

In `search_driver.cpp`, in `evaluate_generation`, inside the parallel scoring block guarded by `if (it != score_j_of_ptr.end() && ss.has_value())`, AFTER the `if (rep.has_value()) { ... }` body (which assigns `score_slot[j].objectives = rep->objectives;`), append:
```cpp
      // S-quality: parsimony objective (slot 5). Set from the genome's node count
      // for the representative regardless of fitness success (node count is a pure
      // structural value, canon-cacheable). Bump n_objectives to cover slot 5; the
      // intervening slots (3 novelty, 4 cost) stay at their inert defaults until
      // their own passes fill them. MultiObjective-only effect (ScalarRaw ignores
      // objectives). Errored genomes get a node-count value too, but cannot be
      // ADMITTED (factory drops un-evaluable candidates), so no perverse incentive.
      if (cfg.enable_parsimony) {
        const atx::usize j = it->second;
        score_slot[j].objectives[alpha_factory_detail_parsimony_slot()] =
            -static_cast<atx::f64>(to_score[j]->ast.nodes().size());
        score_slot[j].n_objectives = static_cast<atx::u8>(
            std::max<atx::usize>(score_slot[j].n_objectives, kObjParsimony + 1U));
      }
```
Replace `alpha_factory_detail_parsimony_slot()` with the constant `kObjParsimony` (it is in `atx::engine::factory` scope via `fitness.hpp` include — no helper needed; the placeholder name is only to flag that you must use the constant). Confirm `<algorithm>` is included for `std::max` (it is used elsewhere in this file).

- [ ] **Step 6: Pin the legacy value in the golden test**

In `factory_nsga_search_test.cpp` `legacy_pin_cfg`, append:
```cpp
  c.enable_parsimony = false;  // Task 2: parsimony off on the boundary pin
```
(ScalarRaw ignores objectives so this is belt-and-suspenders, but keep the pin explicit so intent is unambiguous.)

- [ ] **Step 7: Build + run — parsimony test passes, golden byte-identical**

Run: `cmake --build c:/Users/natha/OneDrive/Desktop/atx/build-rel --target atx-engine-factory-tests`
Then: `ctest --test-dir c:/Users/natha/OneDrive/Desktop/atx/build-rel -R 'NsgaSearch|Parsimony|FactorySearchDriver|FactoryCost|FactoryBehavior' --output-on-failure`
Expected: `Parsimony.ShrinksOrTiesSurvivors` PASS; `NsgaSearch` golden byte-identical; cost-objective tests still pass (slot 4 unchanged; `kMaxObjectives` growth is additive and inert when cost off). If a cost test reads `objectives.size()`/`kMaxObjectives`, confirm it tolerates 6 (it should — the array is fixed-cap and extra slots default 0).

- [ ] **Step 8: Commit**

```bash
git add atx-engine/include/atx/engine/factory/fitness.hpp \
        atx-engine/include/atx/engine/factory/search_driver.hpp \
        atx-engine/src/factory/search_driver.cpp \
        atx-engine/tests/factory/factory_nsga_search_test.cpp \
        atx-engine/tests/factory/search_quality_test.cpp
git commit -m "feat(search): add parsimony objective (-node_count) to NSGA-II ranking

Slot 5 = -node_count makes a smaller equally-fit tree Pareto-dominate a larger
one, curbing crossover bloat (faster eval, less overfit). MultiObjective-only;
ScalarRaw boundary golden unchanged. kMaxObjectives 5->6.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Ramped grammar initialization — fix the cold-start diversity collapse

**Why:** [init_population](../../atx-engine/src/factory/search_driver.cpp#L210-L216) with the default `seed_from_grammar=false` fills all 16 slots with `seeds[i % seeds.size()].clone()` — gen-0 has only `seed_exprs.size()` distinct structures (often 2-6), the rest are dead clones. The GA starts from near-zero diversity. Flip the default ON and make the grammar fill *ramped* (varied depth) and *deduplicated* so gen-0 is N distinct valid trees.

**Files:**
- Modify: `atx-engine/include/atx/engine/factory/search_driver.hpp` — flip `seed_from_grammar` default to `true`; add `init_min_depth`.
- Modify: `atx-engine/src/factory/search_driver.cpp` — rewrite the grammar-fill loop in `init_population` (ramped depth + dedup-resample).
- Modify: `atx-engine/tests/factory/factory_nsga_search_test.cpp` — pin `seed_from_grammar=false`.
- Modify: `atx-engine/tests/factory/search_quality_test.cpp` — gen-0 distinctness test.

**Interfaces:**
- Consumes: `generate_genome(const GenConfig&, const Library&, Xoshiro256pp&)` ([generate.hpp]), `detail::seed_for`, `canonical_hash`, `GenConfig::max_depth`.
- Produces: `SearchConfig::seed_from_grammar` default `true`; `SearchConfig::init_min_depth` (usize, default `2`). The ramped fill varies `gen_cfg.max_depth` across `[init_min_depth, gen_cfg.max_depth]` per slot and resamples on canon-hash collision (bounded retries) so gen-0 distinctness is maximized.

- [ ] **Step 1: Write the failing test (gen-0 is mostly distinct)**

Add to `search_quality_test.cpp`. Use a `SearchProgressSink` to capture generation 0's population (the sink API is `on_generation(const GenerationSnapshot&)` with `population` = DSL strings and `n_unique`; mirror its use from `search_progress_test.cpp`):
```cpp
#include "atx/engine/factory/search_progress.hpp"
// ... in namespace atx::engine::factory ...
namespace {
struct CountingSink : SearchProgressSink {
  std::vector<atx::usize> distinct_per_gen;
  atx::core::Result<void> on_generation(const GenerationSnapshot& s) override {
    std::vector<std::string> pop = s.population;
    std::sort(pop.begin(), pop.end());
    pop.erase(std::unique(pop.begin(), pop.end()), pop.end());
    distinct_per_gen.push_back(pop.size());
    return atx::core::Ok();
  }
};
} // namespace

TEST(RampedInit, GenZeroIsMostlyDistinct) {
  Fixture fx; auto d = fx.driver();
  AlphaStore pool{};
  SearchConfig cfg = base_cfg(31337);
  cfg.objective_mode = ObjectiveMode::MultiObjective;
  cfg.population = 24; cfg.generations = 1;
  cfg.seed_from_grammar = true;                 // ramped grammar fill (the new default)
  CountingSink sink;
  (void)d.run(cfg, pool, &sink);
  ASSERT_FALSE(sink.distinct_per_gen.empty());
  // Gen-0 should be far more diverse than the seed count (6 seeds): expect the
  // grammar fill to push distinct structures well above the seed floor.
  EXPECT_GE(sink.distinct_per_gen.front(), cfg.population / 2);
}
```
(Confirm the exact `SearchProgressSink` / `GenerationSnapshot` field names and the `run(cfg, pool, sink)` overload against `search_progress.hpp`; adjust `on_generation` signature/return to match — it returns `atx::core::Result<void>` and an Err aborts the run.)

- [ ] **Step 2: Run — verify it fails (low distinctness today, or compile error on a new field)**

Run + filter `RampedInit`. Expected: with today's code (cycle fill), `distinct_per_gen.front()` ≈ 6 (seed count) → `EXPECT_GE(.., 12)` FAILS. (If `init_min_depth` is referenced before it exists, it is a compile error instead — add the field in Step 3 then re-run.)

- [ ] **Step 3: Add the knobs**

In `search_driver.hpp` `SearchConfig`: change `bool seed_from_grammar{false};` to `bool seed_from_grammar{true};` and update its doc comment to say ramped grammar fill is the default (the boundary pin sets it false). Add:
```cpp
  // Ramped init: the grammar fill samples tree depth across [init_min_depth,
  // gen_cfg.max_depth] per slot (a ramped-half-and-half analogue) and resamples on
  // a canon-hash collision (bounded retries) so gen-0 is maximally distinct.
  atx::usize init_min_depth{2};
```

- [ ] **Step 4: Rewrite the grammar-fill loop in `init_population`**

In `search_driver.cpp` `init_population`, replace the remainder-fill loop (the block at 228-238 that samples `generate_genome` with a fixed `cfg.gen_cfg`) with a ramped, deduplicated version:
```cpp
  // Track gen-0 structures so the grammar fill maximizes DISTINCT members.
  std::unordered_set<atx::u64> seen0;
  for (const Genome &g : pop) { seen0.insert(g.canon_hash); }
  constexpr atx::u64 kGenSeedAxis = 0xFFFFFFFFFFFFFFFFULL;
  constexpr atx::usize kMaxResample = 8U; // bounded retries; deterministic
  const atx::usize dmax = std::max<atx::usize>(cfg.gen_cfg.max_depth, cfg.init_min_depth);
  for (atx::usize i = n_seed_slots; i < cfg.population; ++i) {
    Genome chosen = seeds[i % seeds.size()].clone(); // deterministic fallback
    chosen.canon_hash = seeds[i % seeds.size()].canon_hash;
    for (atx::usize attempt = 0; attempt < kMaxResample; ++attempt) {
      // Ramped depth: slot i and the attempt index both perturb the seed AND the
      // sampler depth, so distinct slots explore distinct shapes. Pure fn of
      // (master_seed, axis, i, attempt) -> F1 deterministic.
      GenConfig gc = cfg.gen_cfg;
      const atx::usize span = (dmax >= cfg.init_min_depth) ? (dmax - cfg.init_min_depth + 1U) : 1U;
      gc.max_depth = cfg.init_min_depth + ((i + attempt) % span);
      Xoshiro256pp rng{detail::seed_for(cfg.master_seed,
                                        kGenSeedAxis ^ static_cast<atx::u64>(attempt), i)};
      auto gen = generate_genome(gc, lib_, rng);
      if (!gen.has_value()) { continue; }
      gen->canon_hash = canonical_hash(*gen);
      if (seen0.insert(gen->canon_hash).second) { chosen = std::move(*gen); break; }
      // collision: keep the last valid sample as a fallback even if not distinct
      chosen = std::move(*gen);
    }
    pop.push_back(std::move(chosen));
  }
  return pop;
```
Confirm `<unordered_set>` and `<algorithm>` are included (both are already used in this TU). Keep the existing early-return for `seeds.empty()` and the `n_seed_slots` seed-fill loop above unchanged.

- [ ] **Step 5: Pin the legacy value in the golden test**

In `legacy_pin_cfg`, append:
```cpp
  c.seed_from_grammar = false;  // Task 3: legacy cycle-fill on the boundary pin
```

- [ ] **Step 6: Build + run — ramped test passes, golden byte-identical**

Run build + `ctest ... -R 'NsgaSearch|RampedInit|FactorySearchDriver' --output-on-failure`.
Expected: `RampedInit.GenZeroIsMostlyDistinct` PASS; `NsgaSearch` golden byte-identical (boundary pin uses cycle fill). `FactorySearchDriver.SameSeedReplaysByteIdentical` still PASS (ramped fill is a pure function of the seed). If replay breaks, an impure input crept into the fill — audit the `seed_for` args.

- [ ] **Step 7: Commit**

```bash
git add atx-engine/include/atx/engine/factory/search_driver.hpp \
        atx-engine/src/factory/search_driver.cpp \
        atx-engine/tests/factory/factory_nsga_search_test.cpp \
        atx-engine/tests/factory/search_quality_test.cpp
git commit -m "feat(search): ramped+deduped grammar init, default on

init_population cycled seed clones (gen-0 distinct == seed count). Default
seed_from_grammar to true and fill remainder via ramped-depth grammar sampling
with canon-hash dedup, so gen-0 starts diverse. Boundary pin keeps cycle fill.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Random immigrants + stagnation early-stop

**Why:** Tiny fixed budget (default 16×5), no diversity-collapse insurance, no early stop. Inject `n_immigrants` fresh grammar genomes each generation (replacing the worst non-elite slots) and stop early after `stagnation_patience` generations with no best-raw improvement. Both are determinism-safe (immigrant seeds via `seed_for`; stop is a pure function of `best_fitness_per_gen`).

**Files:**
- Modify: `search_driver.hpp` — `SearchConfig::n_immigrants`, `SearchConfig::stagnation_patience`.
- Modify: `search_driver.cpp` — inject immigrants in `reproduce`; early-stop check in the `run` loop.
- Modify: `factory_nsga_search_test.cpp` — pin both to legacy (0).
- Modify: `search_quality_test.cpp` — immigrant + stop tests.

**Interfaces:**
- Produces: `SearchConfig::n_immigrants` (usize, default `2`); `SearchConfig::stagnation_patience` (usize, default `4`; `0` disables → run all generations, the legacy behavior).
- Immigrants occupy the LAST `min(n_immigrants, n_children)` child slots in `reproduce`, generated by `generate_genome` seeded by `seed_for(master_seed, gen, kImmigrantAxis ^ slot)`, REPLACING crossover/mutation children there (not added — population size is fixed). Elites are never displaced.
- Early-stop: after recording `best_fitness_per_gen[g]`, if the best has not strictly improved over the last `stagnation_patience` generations, break the loop (the run finalizes on the current `scored`). `best_fitness_per_gen` therefore may be shorter than `generations` — confirm no test asserts its exact length (the worker-invariance test compares vectors for EQUALITY across worker counts, which still holds; none assert `== generations`).

- [ ] **Step 1: Write the failing tests**

Add to `search_quality_test.cpp`:
```cpp
// Immigrants change the trajectory vs none (same seed) -> different digest.
TEST(Immigrant, ChangesTrajectory) {
  Fixture fx; auto d = fx.driver();
  AlphaStore p0{}, p1{};
  SearchConfig none = base_cfg(909); none.objective_mode = ObjectiveMode::MultiObjective;
  none.generations = 5; none.n_immigrants = 0; none.stagnation_patience = 0;
  SearchConfig some = none; some.n_immigrants = 3;
  const auto r0 = d.run(none, p0);
  const auto r1 = d.run(some, p1);
  EXPECT_NE(r0.digest, r1.digest);
}

// Stagnation stop: with patience small, a run on a trivially-converging config
// records FEWER generations than the budget. Use 1 seed so the population
// collapses fast, patience=2, generations=20.
TEST(StagnationStop, StopsEarly) {
  Library lib; Panel panel = fixture_panel(96,6); WeightPolicy policy;
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver d{lib, panel, policy, sim, {"rank(close)"}, {"close","rev"}};
  AlphaStore pool{};
  SearchConfig cfg = base_cfg(5);
  cfg.objective_mode = ObjectiveMode::MultiObjective;
  cfg.seed_from_grammar = false;   // single seed -> immediate convergence
  cfg.n_immigrants = 0;
  cfg.generations = 20; cfg.stagnation_patience = 2;
  const auto r = d.run(cfg, pool);
  EXPECT_LT(r.best_fitness_per_gen.size(), cfg.generations);
}
```

- [ ] **Step 2: Run — verify failures (compile error on new fields)**

Build; expected compile error: no member `n_immigrants`/`stagnation_patience`.

- [ ] **Step 3: Add the knobs**

In `SearchConfig`:
```cpp
  // Diversity insurance: replace the worst min(n_immigrants, n_children) non-elite
  // child slots each generation with fresh grammar genomes (seed_for-seeded -> F1).
  // 0 disables (legacy).
  atx::usize n_immigrants{2};
  // Stagnation early-stop: stop after this many generations with no strict
  // best-raw improvement. 0 disables (run the full budget; legacy behavior).
  atx::usize stagnation_patience{4};
```

- [ ] **Step 4: Inject immigrants in `reproduce`**

In `search_driver.cpp` `reproduce`, after the child slots are filled (the `det_pool.parallel_for(n_children, ...)` loop that writes `child_slot[p]`), and BEFORE assembling `next`, overwrite the last immigrant slots. Because the children are produced in parallel into `child_slot`, do the immigrant overwrite serially afterward (deterministic, no race):
```cpp
  // Diversity injection: overwrite the LAST n_imm non-elite slots with fresh
  // grammar genomes (deterministic seed per (gen, slot)). Skips when disabled or
  // when grammar sampling fails (keeps the crossover/mutation child). F1-safe:
  // seed is a pure fn of (master_seed, gen, slot).
  constexpr atx::u64 kImmigrantAxis = 0xA5A5A5A5A5A5A5A5ULL;
  const atx::usize n_imm = std::min(cfg.n_immigrants, n_children);
  for (atx::usize t = 0; t < n_imm; ++t) {
    const atx::usize p = n_children - 1U - t; // last slots
    Xoshiro256pp rng{detail::seed_for(cfg.master_seed,
                                      kImmigrantAxis ^ static_cast<atx::u64>(gen),
                                      n_elites + p)};
    auto imm = generate_genome(cfg.gen_cfg, lib_, rng);
    if (imm.has_value()) {
      imm->canon_hash = canonical_hash(*imm);
      child_slot[p] = std::move(*imm);
    }
  }
```
(Confirm `child_slot[p]` is the right type — it is `std::optional<Genome>` per the reproduce body; assign `child_slot[p] = std::move(*imm);`. If it is a bare `Genome`, drop the optional. Match the existing declaration.)

- [ ] **Step 5: Early-stop in the `run` loop**

In `run`, locate where `best_fitness_per_gen` gets its per-generation value pushed (the "Track best_fitness_per_gen" step). After pushing the value for generation `gen`, add a stagnation check that `break`s the generation loop:
```cpp
    // Stagnation early-stop (pure fn of best_fitness_per_gen; F1-safe). Stop when
    // the best raw fitness has not strictly improved over the last `patience`
    // generations. 0 disables.
    if (cfg.stagnation_patience > 0 &&
        res.best_fitness_per_gen.size() > cfg.stagnation_patience) {
      const atx::usize n = res.best_fitness_per_gen.size();
      const atx::f64 recent = res.best_fitness_per_gen[n - 1];
      const atx::f64 baseline = res.best_fitness_per_gen[n - 1 - cfg.stagnation_patience];
      if (!(recent > baseline)) { break; }
    }
```
Ensure this `break` lands AFTER the optional `sink->on_generation(...)` checkpoint for the current generation and BEFORE `reproduce` for the next (so a stopped run still checkpoints its final generation but does not waste a reproduce). Confirm the variable holding the result accumulator is named `res` (it is, per `SearchResult &res` threading).

- [ ] **Step 6: Pin legacy values in the golden test**

In `legacy_pin_cfg`, append:
```cpp
  c.n_immigrants = 0;          // Task 4: no immigrants on the boundary pin
  c.stagnation_patience = 0;   // Task 4: full budget on the boundary pin
```

- [ ] **Step 7: Build + run**

Build; `ctest ... -R 'NsgaSearch|Immigrant|StagnationStop|FactorySearchDriver' --output-on-failure`.
Expected: both new tests PASS; golden byte-identical; replay + worker-invariance PASS (immigrants and stop are deterministic functions of seed + history). If `StagnationStop` does not stop early, raise `generations` or lower `patience` in the test — but the mechanism, not the test, is the deliverable: verify by logging `best_fitness_per_gen.size()`.

- [ ] **Step 8: Commit**

```bash
git add atx-engine/include/atx/engine/factory/search_driver.hpp \
        atx-engine/src/factory/search_driver.cpp \
        atx-engine/tests/factory/factory_nsga_search_test.cpp \
        atx-engine/tests/factory/search_quality_test.cpp
git commit -m "feat(search): random immigrants + stagnation early-stop

Inject n_immigrants fresh grammar genomes into the worst non-elite slots each gen
(collapse insurance) and stop after stagnation_patience gens with no best-raw
gain. Both deterministic (seed_for / pure fn of history). Boundary pin disables
both (full budget, no immigrants).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Adaptive operator selection + annealed jitter σ

**Why:** Operators are equiprobable forever (`which = rng.next_u64() % 3`) and jitter σ is fixed at 0.5. Credit operators by realized fitness improvement and bias next generation's operator distribution toward what works; anneal σ over generations (coarse→fine). Per-generation operator weights are a serial function of the prior generation's credit (computed before the parallel child loop), so each child still draws its operator from its own RNG against fixed weights — F1-safe.

**Files:**
- Modify: `search_driver.hpp` — `SearchConfig::adaptive_operators`, `jitter_anneal`, `jitter_anneal_decay`; a small `OperatorCredit` POD threaded through the run.
- Modify: `search_driver.cpp` — operator-weighted draw in `mutate_one`; credit accumulation in `reproduce`/scoring; annealed σ in the jitter call.
- Modify: `factory_nsga_search_test.cpp` — pin legacy values.
- Modify: `search_quality_test.cpp` — adaptive-operator determinism test.

**Interfaces:**
- Produces: `SearchConfig::adaptive_operators` (bool, default `true`); `SearchConfig::jitter_anneal` (bool, default `true`); `SearchConfig::jitter_anneal_decay` (f64, default `0.9`).
- Operator weights: a `std::array<atx::f64,3> op_weights` (op_swap, field_swap, jitter), initialized uniform `{1,1,1}`, updated after each generation by adding each operator's mean child fitness gain (clamped ≥ a small floor so no operator starves). When `adaptive_operators` is false, weights stay uniform → the legacy `% 3` draw is reproduced EXACTLY (see Step 4 for the equivalence requirement).
- σ for `jitter_const`: `sigma_g = cfg.fitness`-independent base σ (the existing `JitterCfg.sigma`, default 0.5) × `pow(jitter_anneal_decay, gen)` when `jitter_anneal`, else constant.

> **Determinism caution:** the legacy boundary pin MUST reproduce the exact RNG stream. The current draw is `rng.next_u64() % 3`. The weighted draw MUST, under uniform weights, consume the SAME number of RNG words and select the SAME operator as `% 3`. Achieve this by: when `adaptive_operators` is false, keep the literal `rng.next_u64() % 3` path untouched; when true, use a separate weighted-draw helper. Do NOT unify the two draws — branch on the flag so the legacy stream is bit-identical.

- [ ] **Step 1: Write the failing test (adaptive ops deterministic + non-trivial)**

Add to `search_quality_test.cpp`:
```cpp
// Adaptive operators are deterministic (replay) AND change the trajectory vs the
// fixed-uniform draw on the same seed.
TEST(AdaptiveOperator, DeterministicAndDistinct) {
  Fixture fx; auto d = fx.driver();
  AlphaStore pa{}, pb{}, pf{};
  SearchConfig adapt = base_cfg(2024);
  adapt.objective_mode = ObjectiveMode::MultiObjective;
  adapt.generations = 6; adapt.n_immigrants = 0; adapt.stagnation_patience = 0;
  adapt.adaptive_operators = true; adapt.jitter_anneal = true;
  SearchConfig fixed = adapt; fixed.adaptive_operators = false; fixed.jitter_anneal = false;
  const auto a1 = d.run(adapt, pa);
  const auto a2 = d.run(adapt, pb);
  const auto f  = d.run(fixed, pf);
  EXPECT_EQ(a1.digest, a2.digest);   // F1 replay holds with adaptation on
  EXPECT_NE(a1.digest, f.digest);    // adaptation actually changes the search
}
```

- [ ] **Step 2: Run — verify failure (compile error on new fields)**

Build; expected: no member `adaptive_operators`.

- [ ] **Step 3: Add the knobs**

In `SearchConfig`:
```cpp
  // Adaptive operator selection: bias each generation's mutation-operator
  // distribution toward operators that produced fitness gains last generation.
  // OFF reproduces the fixed-uniform (% 3) draw bit-for-bit (boundary pin).
  bool adaptive_operators{true};
  // Jitter annealing: scale jitter_const's sigma by jitter_anneal_decay^gen
  // (coarse early, fine late). OFF keeps the constant JitterCfg.sigma.
  bool jitter_anneal{true};
  atx::f64 jitter_anneal_decay{0.9};
```

- [ ] **Step 4: Operator-weighted draw (flag-branched for stream parity)**

In `search_driver.cpp` `mutate_one`, thread the generation's operator weights and `gen` in (extend the signature, e.g. `mutate_one(const Genome&, const SearchConfig&, Xoshiro256pp&, const std::array<atx::f64,3>& op_w, atx::usize gen)`, updating `make_child` and its callers). Branch:
```cpp
  atx::u64 which;
  if (!cfg.adaptive_operators) {
    which = rng.next_u64() % 3;            // LEGACY stream — bit-identical
  } else {
    // Weighted draw over {op_swap, field_swap, jitter}. One rng word consumed
    // (same as legacy) -> uniform01 from the top 53 bits; pick by cumulative
    // weight. Under uniform weights this is NOT bit-identical to % 3, which is why
    // it lives behind the flag.
    const atx::f64 total = op_w[0] + op_w[1] + op_w[2];
    const atx::f64 u = (static_cast<atx::f64>(rng.next_u64() >> 11U) /
                        static_cast<atx::f64>(1ULL << 53U)) * total;
    which = (u < op_w[0]) ? 0U : (u < op_w[0] + op_w[1]) ? 1U : 2U;
  }
```
Keep the existing fallback cascade (op_swap → field_swap → jitter) exactly as-is below this draw. For the σ anneal, where `jitter_const` is invoked, set its `JitterCfg.sigma` to `base_sigma * (cfg.jitter_anneal ? std::pow(cfg.jitter_anneal_decay, static_cast<atx::f64>(gen)) : 1.0)` (include `<cmath>`; `base_sigma` is the current `JitterCfg{}.sigma` default 0.5 unless the config exposes it — keep using the existing source).

- [ ] **Step 5: Accumulate operator credit (serial, per generation)**

Maintain `std::array<atx::f64,3> op_weights{1.0, 1.0, 1.0}` as a `run`-local, passed by const-ref into `reproduce` and updated AFTER scoring each generation. Credit rule (deterministic, no RNG): for each non-elite child produced last generation, attribute its `(child_raw - parent_best_raw)` gain to the operator that made it; average per operator; `op_weights[o] = max(floor, op_weights[o] + mean_gain[o])` with `floor = 0.05`. Because tracking per-child operator identity across the parallel loop adds plumbing, the minimal deterministic implementation is: in `reproduce`, when `adaptive_operators`, record each child slot's chosen operator into a `std::vector<atx::u8>` (written by the same single-writer shard that produced the slot — no race), and in the next generation's pre-reproduce step compute `mean_gain[o]` from `scored` (match children back by canonical-id slot order). Update `op_weights` before the weighted draw. When `adaptive_operators` is false, leave `op_weights` uniform and never read the operator-id vector.

Document the exact data flow in a comment block; keep all of it inside `if (cfg.adaptive_operators)` so the legacy path allocates nothing and the RNG stream is untouched.

- [ ] **Step 6: Pin legacy values in the golden test**

In `legacy_pin_cfg`, append:
```cpp
  c.adaptive_operators = false;  // Task 5: fixed-uniform operator draw on the pin
  c.jitter_anneal = false;       // Task 5: constant sigma on the pin
```

- [ ] **Step 7: Build + run — replay holds, golden byte-identical**

Build; `ctest ... -R 'NsgaSearch|AdaptiveOperator|FactorySearchDriver|FactoryBehavior' --output-on-failure`.
Expected: `AdaptiveOperator.DeterministicAndDistinct` PASS (replay equal, adaptation distinct); golden byte-identical (legacy `% 3` path untouched); all replay/worker-invariance PASS. If the golden moved, the `adaptive_operators=false` branch is not bit-identical to the original `% 3` — restore the literal legacy line.

- [ ] **Step 8: Commit**

```bash
git add atx-engine/include/atx/engine/factory/search_driver.hpp \
        atx-engine/src/factory/search_driver.cpp \
        atx-engine/tests/factory/factory_nsga_search_test.cpp \
        atx-engine/tests/factory/search_quality_test.cpp
git commit -m "feat(search): adaptive operator selection + annealed jitter sigma

Per-generation operator weights credited by realized child fitness gain bias the
mutation draw toward productive operators; jitter sigma anneals decay^gen. Both
flag-gated; OFF reproduces the fixed-uniform (% 3) draw and constant sigma
bit-for-bit, so the ScalarRaw boundary golden is untouched.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-Review (run after all tasks)

1. **Determinism preserved:** every trajectory-changing feature is flag-gated; `legacy_pin_cfg` pins all five (`enable_behavioral_novelty=false`, `enable_parsimony=false`, `seed_from_grammar=false`, `n_immigrants=0`, `stagnation_patience=0`, `adaptive_operators=false`, `jitter_anneal=false`). `kGoldenDigest` byte-identical. Replay + worker-invariance green.
2. **Type consistency:** `kObjParsimony=5`, `kMaxObjectives=6`; `enable_behavioral_novelty` replaces `novelty_w` everywhere; `op_weights` is `std::array<atx::f64,3>` at every site; `mutate_one`/`make_child` signature change propagated to all callers.
3. **No perverse incentives:** parsimony cannot admit a degenerate genome (factory drops un-evaluable candidates at `detail_eval_streams`); errored/compile-failed genomes stay un-admittable.
4. **Placeholder scan:** `alpha_factory_detail_parsimony_slot()` in Task 2 Step 5 is a deliberate flag — replace with the constant `kObjParsimony`. No other placeholders.
5. **Spec coverage:** review findings #1 (Task 3), #2 (Task 1), #3 (Task 4), #4 (Task 2), #7 (Task 5). Findings #5/#6/#8 deferred (appendix).

## Phase-2 appendix — deferred items (separate plan recommended)

These are real but either store-coupled (different subsystem) or lower-confidence; they warrant their own spec/plan, not this sprint:

- **#6 Cross-run persistent score cache.** The in-process `fitness_cache` resets per run; the store persists only dedup, not scores. Persist `(canon_hash, panel_version) → CachedScore` in the resumable-discover store and warm the cache on run start. High value once budgets grow; couples to `atx::engine::store` schema (a new table) → own plan.
- **#8 Cumulative multiple-testing N.** DSR deflates by this run's distinct count only; across a long program the family-wise N is cumulative. Persist a program-level trial counter in the store and feed cumulative N into `holdout_dsr`/admission. Statistically the correct control for high-throughput search; store-coupled → own plan.
- **#5 Semantic / behavioral dedup.** Collapsing structurally-distinct-but-identical-signal genomes. NOTE: detection requires evaluating to obtain the signal, so it saves SCORING (CPCV+corr), not compile/eval — and prior profiling (obs 10062) says compile + engine setup dominate, already mitigated by engine reuse + per-program CSE. MEASURE the scoring share before building; likely lower ROI than assumed. Candidate for a measured spike, not a blind task.
- **Cross-generation subtree CSE.** Memoize evaluated subtree columns by subtree canon-hash across the whole generation (offspring share subtrees with parents). Largest potential eval win but deep VM/compile surgery and determinism-critical → its own design + plan.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-06-19-alpha-search-quality.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

**Which approach?**
