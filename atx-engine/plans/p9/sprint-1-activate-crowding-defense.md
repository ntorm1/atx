# Sprint 1 — Activate Crowding Defense (dead-alpha-factors live)

**Goal:** thread the accumulating `library::Library` into the `build_risk_model` call sites in
`atx-impl/src/stage_optimize.cpp` so the already-built Kakushadze-Yu dead-alpha-factor de-levering
(`risk/dead_factor.hpp`, shipped p8, zero call sites in the runnable optimize path) actually fires.
Fail-open on every axis: no library configured, no library on disk, or an empty admitted pool all
reproduce today's exact `nullptr`/`{}` behavior byte-for-byte. This is a **wiring** sprint — zero new
estimator math; `extract_dead_factors` / `augment_factor_model` / `FactorModel::create` are frozen
and called, never re-derived.

**Owns (exclusive):**
`atx-impl/src/stage_optimize.cpp` (the three `build_risk_model` call sites — see the orphan-gap table;
note this is THREE sites, not the two the ROADMAP evidence table names — see the Architecture note),
`atx-impl/src/config.hpp`, `atx-impl/src/config.cpp` (the new `--dead-alpha-lib-dir` flag only — every
other flag in those files is untouched);
tests under `atx-impl/tests/`.

**Must NOT touch:** `alpha/oracle.hpp` (untouchable every sprint); `atx-impl/src/stage_riskmodel.{hpp,cpp}`
(the p8-S1 producer — frozen; S1 of p9 calls its existing `build_risk_model` overload, it does not
add parameters or re-derive its logic); `atx-engine/include/atx/engine/risk/dead_factor.hpp` and
`factor_model.hpp` (frozen estimator/extraction math — call only); `atx-engine/include/atx/engine/library/*`
(the Library/LifecycleJournal/DedupIndex facade — read-only consumer, no new methods needed);
`atx-impl/src/stage_optimize.cpp`'s position-mode branch (`cfg.position_mode` block, the linear
`trade_rate` blend around line 191) — **that is Sprint 3's GP-trading seam**; this sprint's edits are
confined to the MVO covariance-source-selection block (today ~lines 248-285) and never touch the
position-mode branch. `atx-impl/src/stage_discover.cpp` — read for context only (it is where
`--library-dir` / the accumulating-library convention lives), **not edited**: S1 needs no change
there, it only reuses the `cfg.library_dir` field discover already populates.

---

## Implementation-quality handoff block (paste verbatim into every subagent brief)

```text
Implementation quality standard:
Use the surrounding engine headers as the style reference. Prefer clear module-level intent,
grouped constants/types/APIs, explicit ownership and lifecycle rules, named error contracts, and
concise comments that explain invariants, non-obvious control flow, or domain semantics.

Prioritize full end-to-end implementation over partial stubs. A unit is not done until the public
API, implementation, tests, docs/ledger row, and build/test gate are complete. Do not leave TODO
placeholders, fake success paths, unused APIs, or untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership, ordering, and tricky
domain rules. Do not comment obvious assignments.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and lifecycle.
- Names are domain-accurate and consistent with nearby engine code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new abstractions.
```

---

## The orphan gap (verified file:line)

| Gap | File:line | Evidence |
|---|---|---|
| Diagonal-kind path never threads a library (**not named in the ROADMAP evidence table — a third site**) | `stage_optimize.cpp:252` | `ATX_TRY(auto artifact, build_risk_model(research, risk_cfg));` — 2-arg call; `dead_lib`/`dead_ids` fall back to `build_risk_model`'s own defaults (`nullptr`, `{}` — `stage_riskmodel.hpp:149-151`). This is the DEFAULT path (`--risk-model` unset ⇒ `RiskModelKind::Diagonal`), so a user running `--dead-alpha-factors` alone (the single-lever activation the p9 thesis names) gets a **silent no-op today**, independent of the Factor-kind gap below. |
| Factor-kind per-step loop never threads a library | `stage_optimize.cpp:260` | `auto factor_artifact = build_risk_model(research, risk_cfg, {}, nullptr, {}, 0, fit_end);` — explicit `nullptr`, `{}` (ROADMAP-cited). |
| Factor-kind warm-up fallback never threads a library | `stage_optimize.cpp:267-268` | `ATX_TRY(auto diag_artifact, build_risk_model(research, diag_fallback_cfg, {}, nullptr, {}, 0, fit_end));` — same (ROADMAP-cited); harmless regardless since `diag_fallback_cfg` is a fresh default (`dead_alpha_factors=false`), but left symmetric with the other two sites so no call site in this function still spells `nullptr` literally. |
| No CLI/config surface to point at an on-disk library from `optimize` | `atx-impl/src/config.hpp` (no `dead_alpha_lib_dir` field), `config.cpp` (no `--dead-alpha-lib-dir` parse arm) | `cfg.dead_alpha_factors` (the gate) and `cfg.risk_model` / `cfg.library_dir` (discover's accumulating-library flag) already exist (`config.hpp:199,307-309`); nothing lets `optimize` name a library directory. |
| The library never marks any alpha `Dead` (or `Live`/`Decaying`) | grep `atx-impl/src` for `LifecycleState::Dead`, `LifecycleState::Live`, `LifecycleState::Decaying`, `.mark(` | **zero matches anywhere in `atx-impl/src`.** `library::Library::admit()` performs exactly one journal transition (`Candidate → Admitted`, `library.hpp:226`); nothing in the runnable pipeline ever drives an alpha further along the `Admitted → Live → Decaying → Dead` spine (`lifecycle.hpp:9-19`). **Consequence:** if S1 gated `dead_ids` on `LifecycleState::Dead` literally, the wire would be a **permanent no-op against every real accumulating library** the current pipeline can produce — see the Architecture note for the resolution this sprint adopts. |

---

## Architecture note — what "thread the library" actually means here

`build_risk_model` (`atx-impl/src/stage_riskmodel.hpp:145-152`) already has the full parameter surface
S1 needs — it shipped in p8 with a documented fail-open contract:

```cpp
[[nodiscard]] atx::core::Result<atx::engine::data::FactorModelArtifact>
build_risk_model(const atx::engine::alpha::Panel& research,
                  const atx::engine::risk::RiskModelConfig& cfg,
                  std::span<const atx::u32> group_id = {},
                  const atx::engine::library::Library* dead_lib = nullptr,
                  std::span<const atx::engine::combine::AlphaId> dead_ids = {},
                  atx::usize dead_as_of = 0,
                  atx::usize fit_end = 0);
```

Internally (`stage_riskmodel.cpp:269-307`), after the base `(X, F, D)` is assembled for **either**
`RiskModelKind`, it augments **iff** `cfg.dead_alpha_factors && dead_lib != nullptr && !dead_ids.empty()`
— all three conditions caller-controlled, all three currently forced false/null by `stage_optimize.cpp`.
S1's entire job is to make those three arguments **real** at all three call sites, gated by three
fail-open checks S1 owns:

1. **The augmentation gate.** `risk_cfg.dead_alpha_factors` (already threaded from
   `cfg.dead_alpha_factors` in the zero-arg `run_optimize` forwarder, `stage_optimize.cpp:45`) — if
   false, S1 never even attempts to open a library.
2. **Directory resolution.** New `RunConfig::dead_alpha_lib_dir` wins when set; otherwise fall back to
   the existing `RunConfig::library_dir` (the discover-stage accumulating-library convention,
   `config.hpp:193-199`, `stage_discover.cpp:532,549-550`) — this is the literal "reuse discover lib
   dir" clause in the ROADMAP's config registry. Both empty ⇒ no library ⇒ `nullptr`.
3. **On-disk existence.** `library::Library::open` (`library.hpp:173-176`) constructs a
   `LifecycleJournal`, whose ctor **`ATX_ASSERT`-aborts** if the sqlite file cannot be created
   (`lifecycle.hpp:117-121`, `open_or_abort`) — which happens whenever the *parent directory* does not
   exist. A resolved-but-nonexistent directory (an operator sets `--dead-alpha-factors` before any
   `--library-dir` discover run has ever populated one) MUST NOT crash the optimize stage — crowding
   defense is a risk-reduction enhancement, never a hard dependency (mirrors `stage_riskmodel.hpp`'s
   own documented fail-open guard verbatim). S1 therefore checks `std::filesystem::exists` +
   `is_directory` **before** calling `Library::open`, and treats "missing" as the same `nullptr` no-op.

**The `dead_ids` policy decision (the sprint's one real judgment call).** The ROADMAP's S1 change note
says thread the library in "as `dead_lib` + **the admitted `dead_ids`**". Read literally as
"AlphaIds currently in `LifecycleState::Dead`", this is unimplementable against any real library
today — nothing in this codebase ever transitions an alpha to `Dead` (see the orphan-gap table). S1
resolves this by reading "the admitted dead_ids" as **the admitted pool**: every AlphaId whose
`state_as_of(id, dead_as_of)` is neither `Candidate` (not yet admitted as of this PIT query — the
existing `state_as_of` PIT contract, `lifecycle.hpp:150-164`) nor `Recycled` (GC'd/reclaimed). Until a
future sprint adds an aging/retirement driver, every admitted alpha in the accumulating library IS the
crowding-defense population — the practical reading that makes the R6 crowding fix bite against the
libraries this pipeline can actually produce, rather than shipping a wire that is correct on paper and
inert on every real input. **This is exactly the kind of interpretive fork the plan review should
confirm before implementation** (see Risks / guardrails).

`dead_as_of` (the PIT period the library's holdings/state are read at) is resolved **once**, before
either the Diagonal single-model branch or the Factor per-step loop, as
`lib.n_periods() > 0 ? lib.n_periods() - 1 : 0` — the library's own most-recent stored period. The
library's holdings/period axis has no established alignment with the research panel's date axis (a
library accumulated across many discover runs over a different panel/date range than today's optimize
run), so S1 does not attempt to line up `dead_as_of` with each rebalance step's `fit_end`; it uses one
constant "latest known crowding snapshot" for the whole run. This matches `dead_factor.hpp`'s own
documented cadence ("a dead alpha is demoted infrequently... COLD path") and is called out explicitly
as a documented approximation, not a silent one.

The library is opened **once** per `run_optimize` call (outside both the Diagonal branch and the
Factor per-step loop) — `Library::open` does real sqlite I/O; reopening it per rebalance step would be
wasteful and is unnecessary since the resolved `dead_lib_ptr`/`dead_ids`/`dead_as_of` triple is reused
identically by every step.

---

## Determinism contract (Sprint 1, p9)

Inherited verbatim from the p9 ROADMAP. Every new capability lives behind an inert default:

- `RunConfig::dead_alpha_lib_dir = ""` — inert; empty routes through the existing `library_dir`
  fallback, and both-empty (or "set but no library on disk") preserves today's `nullptr`/`{}` call.
- `RiskModelConfig::dead_alpha_factors = false` (reused from p8, already threaded) — inert; the
  augmentation gate itself.

At the inert defaults, all three `build_risk_model` call sites in `stage_optimize.cpp` pass the exact
same `nullptr`/`{}` they pass today — same code path, same input, so the books/turnover/cost digest is
unchanged **by construction**, not by parallel-maintained duplicate logic.

**Four test classes (mandatory):**
(a) **off-path byte-identity** — `dead_alpha_factors=false`, OR no library resolved, OR a resolved
    directory that does not exist, OR a resolved library with zero admitted alphas: every one of
    these reproduces the pre-wire books digest exactly.
(b) **on-path RED→GREEN** — a synthetic crowded-pool library (alphas whose holdings concentrate on one
    instrument) makes the optimizer size DOWN that instrument (vs. the same run with the wire
    unavailable) once `--dead-alpha-factors` + a populated `--dead-alpha-lib-dir` are both set.
(c) **twice-run** — the SAME on-disk library + panel + config produce byte-identical books across two
    independent `run_optimize` calls (no RNG, no clock, no map iteration in the new code).
(d) **seq==parallel** — `stage_optimize.cpp`'s per-step loop is single-threaded (S1 introduces no
    thread-pool/parallel path), so the live analog of this class is: **dead-id collection order never
    changes the result** — `extract_dead_factors` internally sorts its input by ascending `AlphaId`
    before accumulating (`dead_factor.hpp:194-198`, the R1 bit-reproducibility contract), so a test
    that feeds `build_risk_model` the SAME dead-id set in two different orders (ascending vs. shuffled)
    must produce byte-identical artifacts — proving the wire's chosen collection order (ascending, by
    construction) is not load-bearing, and guarding against a future change to the extraction routine
    silently breaking that guarantee.

**Byte-identity:** matched at the `StageResult::digest` (u64 wyhash over the emitted books panel) AND
the raw `books_out` file bytes (the existing `stage_optimize_riskmodel_test.cpp::DiagonalByteIdentical`
pattern), plus an explicit `std::bit_cast<std::uint64_t>` per-cell comparison on the reloaded weight
spans for the on-path test (matches signed zeros; the same idiom `data_universe_test.cpp:195` and
`search_progress_test.cpp:453` already use in this codebase).

---

## Dependency / wiring map

```
config.hpp: RunConfig::dead_alpha_lib_dir (NEW, S1-0)         ─┐
config.cpp: apply_flag_value("dead-alpha-lib-dir") (NEW, S1-0) ─┼─→ read by stage_optimize.cpp's
config.hpp: RunConfig::dead_alpha_factors (EXISTS, p8)         ─┘   run_optimize(cfg, risk_cfg) body

stage_optimize.cpp (S1-1, NEW anonymous-namespace helpers):
  resolve_dead_alpha_lib_dir(cfg)      -> cfg.dead_alpha_lib_dir, else cfg.library_dir
  maybe_open_dead_lib(cfg, risk_cfg)   -> gate + fs::exists guard -> std::optional<library::Library>
  collect_dead_alpha_ids(lib, as_of)  -> library::Library::n_alphas()/state_as_of() (READ-ONLY)
        │
        ▼ (dead_lib_ptr, dead_ids, dead_as_of resolved ONCE before the branch)
stage_optimize.cpp:252  build_risk_model(research, risk_cfg, {}, dead_lib_ptr, dead_ids, dead_as_of)
stage_optimize.cpp:260  build_risk_model(research, risk_cfg, {}, dead_lib_ptr, dead_ids, dead_as_of, fit_end)
stage_optimize.cpp:267  build_risk_model(research, diag_fallback_cfg, {}, dead_lib_ptr, dead_ids, dead_as_of, fit_end)
        │
        ▼ (all three call the SAME frozen p8 entry point; no signature change)
stage_riskmodel.cpp:280  cfg.dead_alpha_factors && dead_lib!=nullptr && !dead_ids.empty()
        -> risk::extract_dead_factors (dead_factor.hpp, FROZEN) -> risk::augment_factor_model (FROZEN)
```

---

## Tasks

### S1-0 — `RunConfig::dead_alpha_lib_dir` + `--dead-alpha-lib-dir` CLI plumbing (do first)

**Goal:** add the one new config-registry field. No behavior change anywhere yet — nothing reads it
outside this unit's own parse-layer test.

**Files:**
- Modify `atx-impl/src/config.hpp` — add the field immediately after `bool group_neutralize = false;`
  (currently line 309).
- Modify `atx-impl/src/config.cpp` — add one `apply_flag_value` string-flag arm immediately after the
  `library-dir` arm (currently line 65).
- Create `atx-impl/tests/config_dead_alpha_lib_dir_test.cpp` (auto-globbed by
  `atx-impl/tests/CMakeLists.txt`'s `file(GLOB ... "*_test.cpp")` — no CMake edit needed).

**Interfaces:**
- Consumes: nothing new.
- Produces: `atx::impl::RunConfig::dead_alpha_lib_dir : std::string` (default `""`); parses
  `--dead-alpha-lib-dir <path>` into it; records `"dead-alpha-lib-dir"` into `cfg.set_flags` via the
  existing `apply_flag` wrapper (automatic — no extra code needed for that part).

**Steps:**

1. **Write the failing test** (`atx-impl/tests/config_dead_alpha_lib_dir_test.cpp`):
   ```cpp
   #include <gtest/gtest.h>
   #include "config.hpp"

   using atx::impl::parse_args;
   using atx::impl::RunConfig;

   namespace {
   atx::core::Result<RunConfig> parse(std::vector<std::string> args) {
       std::vector<char*> argv;
       argv.reserve(args.size());
       for (auto& a : args) argv.push_back(a.data());
       return parse_args(static_cast<int>(argv.size()), argv.data());
   }
   } // namespace

   TEST(ConfigParse, DeadAlphaLibDir_RoundTrip) {
       auto r = parse({"atx-impl", "optimize", "--dead-alpha-lib-dir", "/tmp/mylib",
                       "--dead-alpha-factors"});
       ASSERT_TRUE(r.has_value()) << r.error().message();
       EXPECT_EQ(r->dead_alpha_lib_dir, "/tmp/mylib");
       EXPECT_TRUE(r->dead_alpha_factors);
       EXPECT_TRUE(r->set_flags.count("dead-alpha-lib-dir"));
   }

   TEST(ConfigParse, DeadAlphaLibDir_OmittedIsInert) {
       auto r = parse({"atx-impl", "optimize"});
       ASSERT_TRUE(r.has_value());
       EXPECT_EQ(r->dead_alpha_lib_dir, "");
       EXPECT_FALSE(r->set_flags.count("dead-alpha-lib-dir"));
   }
   ```

2. **Run to fail** (the field doesn't exist yet — a compile error, the RED state for a plumbing unit):
   ```powershell
   <scratch>\p9-build.ps1 -Target atx-impl-tests
   ```
   Expected FAIL: `error: no member named 'dead_alpha_lib_dir' in 'atx::impl::RunConfig'` (or
   `--dead-alpha-lib-dir` reported as `unknown flag` if the header compiles but the flag doesn't parse
   — either is an acceptable RED signal for this unit).

3. **Minimal impl:**
   - `atx-impl/src/config.hpp`, immediately after line 309 (`bool group_neutralize = false; ...`):
     ```cpp
     // --dead-alpha-lib-dir (p9 S1): the on-disk library::Library directory whose ADMITTED alpha
     // pool is threaded into build_risk_model's dead_lib/dead_ids as the Kakushadze-Yu crowding-
     // factor source (stage_optimize.cpp's build_risk_model call sites — the Potemkin-book gap).
     // "" (default) FALLS BACK to cfg.library_dir (the discover accumulating library, if
     // --library-dir was set); if THAT is also "" -- or the resolved directory does not exist on
     // disk -- the wire is a documented fail-open no-op (matches build_risk_model's own
     // dead_lib==nullptr contract, stage_riskmodel.hpp:120-126).
     std::string dead_alpha_lib_dir; // --dead-alpha-lib-dir ("" = fall back to --library-dir, else off)
     ```
   - `atx-impl/src/config.cpp`, immediately after line 65 (`if (flag == "library-dir") ...`):
     ```cpp
     if (flag == "dead-alpha-lib-dir") { cfg.dead_alpha_lib_dir = value; return atx::core::Ok(); } // S1 (p9)
     ```

4. **Run to pass:**
   ```powershell
   <scratch>\p9-build.ps1 -Target atx-impl-tests
   <scratch>\p9-ctest.ps1 -R ConfigParse.DeadAlphaLibDir
   ```
   Expected: 2/2 green; no other `ConfigParse.*` test regresses (run the full `ConfigParse` filter too).

5. **Commit:**
   ```
   git add atx-impl/src/config.hpp atx-impl/src/config.cpp \
           atx-impl/tests/config_dead_alpha_lib_dir_test.cpp
   git commit -m "$(cat <<'EOF'
   PF-P9 S1-0: add --dead-alpha-lib-dir config field + CLI parse arm

   Config-registry plumbing only -- nothing reads the field outside this
   unit's own parse-layer test yet. Falls back to --library-dir when unset
   (the S1 architecture note's resolution order); inert empty default.

   Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
   EOF
   )"
   ```

---

### S1-1 — Wire the three `build_risk_model` call sites in `stage_optimize.cpp`

**Goal:** make the dead-lib/dead-ids arguments real at all three call sites, behind the three
fail-open guards from the Architecture note. This is the sprint's core fix.

**Files:**
- Modify `atx-impl/src/stage_optimize.cpp`:
  - includes: add `<filesystem>`; add `#include "atx/engine/library/library.hpp"`.
  - namespace aliases (after `namespace risk = atx::engine::risk;`, currently line 29): add
    `namespace combine = atx::engine::combine;` and `namespace library = atx::engine::library;`.
  - new anonymous-namespace helpers (placed after the aliases, before `run_optimize`'s zero-arg
    definition at line 40): `resolve_dead_alpha_lib_dir`, `maybe_open_dead_lib`, `collect_dead_alpha_ids`.
  - the 2-arg `run_optimize(cfg, risk_cfg)` body: resolve `dead_lib_opt`/`dead_lib_ptr`/`dead_ids`/
    `dead_as_of` once, immediately before the `if (risk_cfg.kind == risk::RiskModelKind::Diagonal)`
    block (currently line 251); edit the three call sites at (today's) lines 252, 260, 267-268.
- Create `atx-impl/tests/stage_optimize_dead_alpha_wire_test.cpp`.

**Interfaces:**
- Consumes: `atx::engine::library::Library::open/n_alphas/n_periods/state_as_of` (all pre-existing,
  public, `library.hpp:173-320`); `atx::impl::build_risk_model` (pre-existing 7-arg overload,
  `stage_riskmodel.hpp:145-152`, unchanged signature).
- Produces (internal linkage, `stage_optimize.cpp` only — not exported, matches
  `stage_riskmodel.cpp`'s own private-helper precedent):
  ```cpp
  [[nodiscard]] std::string resolve_dead_alpha_lib_dir(const RunConfig& cfg);
  [[nodiscard]] std::optional<library::Library>
  maybe_open_dead_lib(const RunConfig& cfg, const risk::RiskModelConfig& risk_cfg);
  [[nodiscard]] std::vector<combine::AlphaId>
  collect_dead_alpha_ids(const library::Library& lib, atx::usize dead_as_of);
  ```

**Steps:**

1. **Write the failing tests** (`atx-impl/tests/stage_optimize_dead_alpha_wire_test.cpp` — suite
   `AtxImplOptimizeDeadAlphaWire`). Reuses the `make_trend_research` / `make_pair_combo` fixture
   pattern from `stage_optimize_riskmodel_test.cpp` and the `make_dead_lib`-style crowded-pool fixture
   from `stage_riskmodel_dead_factor_test.cpp`, adapted to build the library ON DISK at a path the CLI
   config can point to (rather than passed as an in-process `Library*`):
   ```cpp
   #include <bit>
   #include <cmath>
   #include <filesystem>
   #include <fstream>
   #include <numbers>
   #include <vector>

   #include <gtest/gtest.h>

   #include "config.hpp"
   #include "serialize_panel.hpp"
   #include "stages.hpp"

   #include "atx/engine/alpha/panel.hpp"
   #include "atx/engine/combine/gate.hpp"
   #include "atx/engine/combine/metrics.hpp"
   #include "atx/engine/library/library.hpp"
   #include "atx/engine/library/lifecycle.hpp"
   #include "atx/engine/library/record.hpp"

   namespace atxtest_stage_optimize_dead_alpha_wire {

   namespace fs = std::filesystem;
   namespace alpha = atx::engine::alpha;
   namespace risk = atx::engine::risk;
   namespace lib = atx::engine::library;
   using atx::f64;
   using atx::usize;

   // ... make_trend_research / make_pair_combo copied verbatim from
   // stage_optimize_riskmodel_test.cpp (kept local; no cross-file test dependency) ...

   [[nodiscard]] lib::GateConfig permissive_gate_cfg() {
     lib::GateConfig cfg;
     cfg.min_sharpe = -1e9; cfg.min_fitness = -1e9;
     cfg.max_turnover = 1e9; cfg.max_pool_corr = 1.1;
     return cfg;
   }
   [[nodiscard]] atx::engine::combine::AlphaMetrics passing_metrics() {
     atx::engine::combine::AlphaMetrics m{};
     m.sharpe = 5.0; m.turnover = 0.05; m.returns = 1.0;
     m.drawdown = 0.1; m.margin = 10.0; m.fitness = 5.0; m.holding_days = 20.0;
     return m;
   }

   // Build an ON-DISK library at `dir` with n_dead alphas all concentrated on
   // instrument `center` (rank-1 overlap -- same fixture shape as p8's
   // stage_riskmodel_dead_factor_test.cpp), then FLUSH and let the Library
   // object go out of scope so a later independent Library::open(dir, ...)
   // (the one stage_optimize.cpp's wire performs) sees every admit on disk --
   // a live in-process instance's un-flushed memtable is invisible to a
   // second Library::open of the same directory.
   void seed_crowded_library(const fs::path& dir, usize n_dead, usize m, usize center) {
     std::error_code ec;
     fs::remove_all(dir, ec);
     fs::create_directories(dir);
     lib::Library library = lib::Library::open(dir.string(), permissive_gate_cfg(), {777ULL});
     const atx::engine::combine::AlphaGate gate{permissive_gate_cfg()};
     constexpr usize kT = 2U;
     std::vector<std::vector<f64>> pnls(n_dead), positions(n_dead);
     std::vector<lib::AlphaId> ids;
     for (usize k = 0; k < n_dead; ++k) {
       pnls[k].assign(kT, 0.0);
       pnls[k][1] = 0.01 + 0.0001 * static_cast<f64>(k);
       positions[k].assign(kT * m, 0.0);
       for (usize i = 0; i < m; ++i) {
         const f64 d = (static_cast<f64>(i) - static_cast<f64>(center)) / static_cast<f64>(m);
         positions[k][1 * m + i] = std::cos(std::numbers::pi * d);
       }
       const lib::AlphaCandidate cand{0x300ULL + k, pnls[k], positions[k], passing_metrics(),
                                      lib::Provenance{"dead", std::vector<atx::u64>{}, 0, 100 + k},
                                      0U, nullptr};
       const auto v = library.admit(cand, gate);
       ASSERT_EQ(v.kind, lib::AdmitKind::Accept);
       ids.push_back(v.id);
     }
     ASSERT_TRUE(library.flush_all().has_value());
     // NOTE: no LifecycleState::Dead transition here -- see the S1 ledger's
     // "admitted pool" policy note; the wire treats every admitted (non-
     // Candidate, non-Recycled) alpha as the crowding-defense population.
   }

   class AtxImplOptimizeDeadAlphaWire : public ::testing::Test {
   protected:
     fs::path tmp_dir_;
     void SetUp() override {
       tmp_dir_ = fs::temp_directory_path() / "atx_p9_s1_wire_test";
       std::error_code ec; fs::remove_all(tmp_dir_, ec);
       fs::create_directories(tmp_dir_);
     }
     void TearDown() override { std::error_code ec; fs::remove_all(tmp_dir_, ec); }
   };

   // -- (a) off-path byte-identity: three independent fail-open guards ---------
   TEST_F(AtxImplOptimizeDeadAlphaWire, FailOpen_FlagOffByteIdentical) {
     constexpr usize M = 10, D = 40;
     const fs::path research_path = tmp_dir_ / "research.bin";
     const fs::path combo_path = tmp_dir_ / "combo.bin";
     ASSERT_TRUE(make_trend_research(research_path, M, D).has_value());
     ASSERT_TRUE(make_pair_combo(combo_path, M, D).has_value());
     seed_crowded_library(tmp_dir_ / "lib", 2U, M, 3U);

     atx::impl::RunConfig cfg;
     cfg.panel = research_path.string(); cfg.combo = combo_path.string();
     cfg.gross = 1.0; cfg.name_cap = 1.0; cfg.rebalance = "weekly";
     cfg.risk_aversion = 1.0; cfg.set_flags.emplace("risk-aversion");
     cfg.dead_alpha_lib_dir = (tmp_dir_ / "lib").string(); // populated, but the GATE is off
     cfg.dead_alpha_factors = false;

     cfg.books_out = (tmp_dir_ / "books_gate_off.bin").string();
     auto r_off = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(r_off.has_value()) << r_off.error().message();

     cfg.dead_alpha_lib_dir = ""; // no library at all, matches pre-wire literally
     cfg.books_out = (tmp_dir_ / "books_legacy.bin").string();
     auto r_legacy = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(r_legacy.has_value());

     EXPECT_EQ(r_off->digest, r_legacy->digest)
         << "dead_alpha_factors=false must ignore a populated --dead-alpha-lib-dir entirely";
   }

   TEST_F(AtxImplOptimizeDeadAlphaWire, FailOpen_MissingDirByteIdentical) {
     constexpr usize M = 10, D = 40;
     const fs::path research_path = tmp_dir_ / "research2.bin";
     const fs::path combo_path = tmp_dir_ / "combo2.bin";
     ASSERT_TRUE(make_trend_research(research_path, M, D).has_value());
     ASSERT_TRUE(make_pair_combo(combo_path, M, D).has_value());

     atx::impl::RunConfig cfg;
     cfg.panel = research_path.string(); cfg.combo = combo_path.string();
     cfg.gross = 1.0; cfg.name_cap = 1.0; cfg.rebalance = "weekly";
     cfg.risk_aversion = 1.0; cfg.set_flags.emplace("risk-aversion");
     cfg.dead_alpha_factors = true;
     cfg.dead_alpha_lib_dir = (tmp_dir_ / "does_not_exist").string(); // never created

     cfg.books_out = (tmp_dir_ / "books_missing_dir.bin").string();
     auto r_missing = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(r_missing.has_value()) << r_missing.error().message(); // MUST NOT abort/crash

     cfg.dead_alpha_factors = false;
     cfg.books_out = (tmp_dir_ / "books_legacy2.bin").string();
     auto r_legacy = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(r_legacy.has_value());

     EXPECT_EQ(r_missing->digest, r_legacy->digest)
         << "a --dead-alpha-lib-dir that does not exist on disk must fail OPEN, not abort";
   }

   // -- (b) on-path RED->GREEN: crowded pool must shrink the crowded direction --
   TEST_F(AtxImplOptimizeDeadAlphaWire, CrowdedPoolDelevers) {
     constexpr usize M = 10, D = 40;
     const usize center = 3U;
     const fs::path research_path = tmp_dir_ / "research3.bin";
     const fs::path combo_path = tmp_dir_ / "combo3.bin";
     ASSERT_TRUE(make_trend_research(research_path, M, D).has_value());
     ASSERT_TRUE(make_pair_combo(combo_path, M, D).has_value()); // long-first-half/short-second-half
     seed_crowded_library(tmp_dir_ / "lib3", /*n_dead=*/3U, M, center);

     atx::impl::RunConfig cfg;
     cfg.panel = research_path.string(); cfg.combo = combo_path.string();
     cfg.gross = 1.0; cfg.name_cap = 1.0; cfg.rebalance = "weekly";
     cfg.risk_aversion = 1.0; cfg.set_flags.emplace("risk-aversion");

     cfg.dead_alpha_factors = false;
     cfg.books_out = (tmp_dir_ / "books_baseline.bin").string();
     auto baseline_sr = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(baseline_sr.has_value()) << baseline_sr.error().message();

     cfg.dead_alpha_factors = true;
     cfg.dead_alpha_lib_dir = (tmp_dir_ / "lib3").string();
     cfg.books_out = (tmp_dir_ / "books_delevered.bin").string();
     auto delev_sr = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(delev_sr.has_value()) << delev_sr.error().message();

     auto baseline_r = atx::impl::read_panel((tmp_dir_ / "books_baseline.bin").string());
     auto delev_r = atx::impl::read_panel((tmp_dir_ / "books_delevered.bin").string());
     ASSERT_TRUE(baseline_r.has_value()); ASSERT_TRUE(delev_r.has_value());
     const auto wfid_b = *baseline_r->field_id("weight");
     const auto wfid_d = *delev_r->field_id("weight");
     const usize last = baseline_r->dates() - 1;
     const auto w_base = baseline_r->field_cross_section(wfid_b, last);
     const auto w_delev = delev_r->field_cross_section(wfid_d, last);

     EXPECT_LT(std::fabs(w_delev[center]), std::fabs(w_base[center]))
         << "expected the crowded instrument's weight to shrink once the dead-alpha "
            "wire is live: base=" << w_base[center] << " delevered=" << w_delev[center];
     // Bit-exact sanity: the two runs must NOT be byte-identical once the gate is
     // live (else the wire silently did nothing) -- bit_cast per the p9 byte-
     // identity idiom, applied here to prove a DIFFERENCE, not an equality.
     EXPECT_NE(std::bit_cast<std::uint64_t>(w_base[center]),
              std::bit_cast<std::uint64_t>(w_delev[center]));
   }

   } // namespace atxtest_stage_optimize_dead_alpha_wire
   ```

2. **Run to fail:**
   ```powershell
   <scratch>\p9-build.ps1 -Target atx-impl-tests
   <scratch>\p9-ctest.ps1 -R AtxImplOptimizeDeadAlphaWire
   ```
   Expected FAIL: `CrowdedPoolDelevers` RED — `w_delev[center]` equals `w_base[center]` exactly (both
   `EXPECT_LT` and `EXPECT_NE` fail) because `stage_optimize.cpp` still passes `nullptr`/`{}`
   regardless of `cfg.dead_alpha_lib_dir`/`cfg.dead_alpha_factors`. (`FailOpen_*` tests pass trivially
   pre-wire too, since "always nullptr" already satisfies "fail open" — they exist to catch a
   *regression* once the wire lands, not to prove new behavior; only `CrowdedPoolDelevers` is a true
   RED here.)

3. **Minimal impl** (`atx-impl/src/stage_optimize.cpp`):

   Includes (top of file, alongside the existing `<optional>` etc.):
   ```cpp
   #include <filesystem>
   ```
   and
   ```cpp
   #include "atx/engine/library/library.hpp"
   ```

   Namespace aliases (after `namespace risk  = atx::engine::risk;`, line 29):
   ```cpp
   namespace combine = atx::engine::combine;
   namespace library = atx::engine::library;
   ```

   New anonymous-namespace helpers (after the aliases, before the zero-arg `run_optimize` at line 40):
   ```cpp
   namespace {

   // S1 (p9): resolve the on-disk library directory the dead-alpha crowding wire
   // reads from. --dead-alpha-lib-dir wins when set; otherwise fall back to the
   // discover stage's own accumulating --library-dir (the "library dir already in
   // the pipeline" the p9 ROADMAP names as S1's source). Neither set -> "" -> the
   // caller's fail-open no-op (maybe_open_dead_lib below).
   [[nodiscard]] std::string resolve_dead_alpha_lib_dir(const RunConfig& cfg) {
       return !cfg.dead_alpha_lib_dir.empty() ? cfg.dead_alpha_lib_dir : cfg.library_dir;
   }

   // S1 (p9): open the accumulating library for the dead-alpha-factor wire, or
   // return nullopt on any of three fail-open conditions: (1) the augmentation
   // gate itself is off; (2) no directory resolves anywhere; (3) the resolved
   // directory does not exist yet (an operator ran --dead-alpha-factors before any
   // --library-dir discover run created it -- a MISSING library is a documented
   // no-op, not a hard failure: crowding defense is a risk-reduction enhancement,
   // never a run-blocking dependency -- mirrors build_risk_model's own dead_lib==
   // nullptr contract, stage_riskmodel.hpp:120-126). GateConfig{} is inert here:
   // this handle only ever calls the READ methods (n_alphas/n_periods/
   // state_as_of); the gate floors matter only to admit()/try_admit(), never
   // invoked on this path.
   [[nodiscard]] std::optional<library::Library>
   maybe_open_dead_lib(const RunConfig& cfg, const risk::RiskModelConfig& risk_cfg) {
       if (!risk_cfg.dead_alpha_factors) {
           return std::nullopt;
       }
       const std::string dir = resolve_dead_alpha_lib_dir(cfg);
       if (dir.empty()) {
           return std::nullopt;
       }
       std::error_code ec;
       if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
           return std::nullopt;
       }
       return library::Library::open(dir, combine::GateConfig{}, {cfg.seed});
   }

   // S1 (p9): the "admitted dead-alpha pool" -- every AlphaId the library already
   // admitted as of `dead_as_of`. The codebase today has NO driver that walks an
   // alpha through Live->Decaying->Dead (grep of atx-impl/src for
   // LifecycleState::Dead / .mark( is empty), so gating on LifecycleState::Dead
   // literally would make this wire a permanent no-op against every real
   // accumulating library. Until a future sprint adds that lifecycle-aging
   // driver, "dead" here means "already admitted, not yet recycled":
   // state_as_of(id, dead_as_of) NOT IN {Candidate, Recycled} -- Candidate
   // excludes an id not yet admitted as of this PIT query (the state_as_of PIT
   // contract, lifecycle.hpp:150-164); Recycled excludes a GC'd/reclaimed slot.
   // Ascending AlphaId order by construction; NOT load-bearing for
   // extract_dead_factors' own bit-reproducibility (it re-sorts internally,
   // dead_factor.hpp:194-198) -- see the DeadIdOrderInvariant proof (S1-2).
   [[nodiscard]] std::vector<combine::AlphaId>
   collect_dead_alpha_ids(const library::Library& lib, atx::usize dead_as_of) {
       std::vector<combine::AlphaId> ids;
       const atx::u64 n = lib.n_alphas();
       ids.reserve(static_cast<atx::usize>(n));
       for (atx::u64 a = 0; a < n; ++a) {
           const combine::AlphaId id{static_cast<atx::u32>(a)};
           const auto st = lib.state_as_of(id, dead_as_of);
           if (st.has_value() && *st != library::LifecycleState::Candidate &&
               *st != library::LifecycleState::Recycled) {
               ids.push_back(id);
           }
       }
       return ids;
   }

   } // namespace
   ```

   In `run_optimize(const RunConfig& cfg, const risk::RiskModelConfig& risk_cfg)`, immediately before
   `std::optional<risk::FactorModel> single_model;` (today's line 248):
   ```cpp
   // S1 (p9): resolve the dead-alpha wire ONCE, shared by both the Diagonal
   // single-model branch and the Factor per-step loop below (Library::open does
   // real sqlite I/O; reopening it per rebalance step would be wasteful and
   // unnecessary since the resolved triple is identical for every step -- the
   // library's own holdings/period axis has no established alignment with the
   // per-step fit_end, so a single "latest known crowding snapshot" is used for
   // the whole run rather than attempting a per-step correspondence).
   std::optional<library::Library> dead_lib_opt = maybe_open_dead_lib(cfg, risk_cfg);
   const library::Library* dead_lib_ptr = dead_lib_opt.has_value() ? &*dead_lib_opt : nullptr;
   std::vector<combine::AlphaId> dead_ids;
   atx::usize dead_as_of = 0;
   if (dead_lib_ptr != nullptr) {
       dead_as_of = dead_lib_ptr->n_periods() > 0 ? dead_lib_ptr->n_periods() - 1 : 0;
       dead_ids = collect_dead_alpha_ids(*dead_lib_ptr, dead_as_of);
   }
   ```

   Then the three call-site edits (today's lines 252 / 260 / 267-268):
   ```cpp
   // BEFORE (line 252):
   ATX_TRY(auto artifact, build_risk_model(research, risk_cfg));
   // AFTER:
   ATX_TRY(auto artifact,
           build_risk_model(research, risk_cfg, /*group_id=*/{}, dead_lib_ptr, dead_ids, dead_as_of));
   ```
   ```cpp
   // BEFORE (line 260):
   auto factor_artifact = build_risk_model(research, risk_cfg, {}, nullptr, {}, 0, fit_end);
   // AFTER:
   auto factor_artifact =
       build_risk_model(research, risk_cfg, {}, dead_lib_ptr, dead_ids, dead_as_of, fit_end);
   ```
   ```cpp
   // BEFORE (lines 267-268):
   ATX_TRY(auto diag_artifact, build_risk_model(research, diag_fallback_cfg, {}, nullptr,
                                                {}, 0, fit_end));
   // AFTER:
   ATX_TRY(auto diag_artifact, build_risk_model(research, diag_fallback_cfg, {}, dead_lib_ptr,
                                                dead_ids, dead_as_of, fit_end));
   ```
   (The last site is inert regardless — `diag_fallback_cfg` is a fresh default `RiskModelConfig` with
   `dead_alpha_factors=false` — but is threaded for symmetry so no call in this function still spells
   a literal `nullptr`, matching the ROADMAP's citation of this exact line.)

4. **Run to pass:**
   ```powershell
   <scratch>\p9-build.ps1 -Target atx-impl-tests
   <scratch>\p9-ctest.ps1 -R AtxImplOptimizeDeadAlphaWire
   <scratch>\p9-ctest.ps1 -R AtxImplOptimizeRiskModel   # p8's existing suite -- must stay green
   <scratch>\p9-ctest.ps1 -R AtxImplDeadFactor          # p8's stage_riskmodel-level suite -- unaffected
   ```
   Expected: all green, including `CrowdedPoolDelevers` now GREEN.

5. **Commit:**
   ```
   git add atx-impl/src/stage_optimize.cpp \
           atx-impl/tests/stage_optimize_dead_alpha_wire_test.cpp
   git commit -m "$(cat <<'EOF'
   PF-P9 S1-1: thread the accumulating library into build_risk_model

   Wires all three build_risk_model call sites in stage_optimize.cpp (the
   Diagonal single-model path at :252, the Factor per-step loop at :260, and
   its warm-up diagonal fallback at :267) to a real library::Library resolved
   from --dead-alpha-lib-dir (falling back to --library-dir), instead of the
   literal nullptr/{} every site passed before. Fail-open on three axes:
   dead_alpha_factors=false, no directory resolved, or a resolved directory
   that does not exist on disk -- all three reproduce the pre-wire digest
   exactly. "Dead" ids are read as the admitted pool (state_as_of NOT IN
   {Candidate, Recycled}), since nothing in this codebase yet drives an alpha
   to LifecycleState::Dead -- see the sprint ledger's architecture note.

   Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
   EOF
   )"
   ```

---

### S1-2 — Determinism hardening: twice-run + dead-id order invariance

**Goal:** close out the mandatory (c)/(d) test classes with end-to-end proofs against the just-shipped
wire (S1-1 built a deterministic path by construction — no RNG, no clock, no unordered-map iteration —
so this unit is a proof/regression-guard, not a new-behavior RED→GREEN; stated honestly rather than
manufacturing a fake RED).

**Files:**
- Create `atx-impl/tests/stage_optimize_dead_alpha_determinism_test.cpp`.

**Interfaces:**
- Consumes: `atx::impl::run_optimize` (public, unchanged signature); `atx::impl::build_risk_model`
  (public, unchanged signature — used directly for the order-invariance proof, mirroring
  `stage_riskmodel_dead_factor_test.cpp`'s own `InertOff` pattern of comparing
  `data::serialize_artifact(...)`).
- Produces: no new production code in this unit — tests only.

**Steps:**

1. **Write the tests** (suite `AtxImplOptimizeDeadAlphaDeterminism`):
   ```cpp
   #include <algorithm>
   #include <cmath>
   #include <filesystem>
   #include <fstream>
   #include <numbers>
   #include <vector>

   #include <gtest/gtest.h>

   #include "config.hpp"
   #include "serialize_panel.hpp"
   #include "stage_riskmodel.hpp"
   #include "stages.hpp"

   #include "atx/engine/alpha/panel.hpp"
   #include "atx/engine/combine/gate.hpp"
   #include "atx/engine/combine/metrics.hpp"
   #include "atx/engine/data/factor_model_artifact.hpp"
   #include "atx/engine/library/library.hpp"
   #include "atx/engine/library/lifecycle.hpp"
   #include "atx/engine/library/record.hpp"
   #include "atx/engine/risk/factor_model.hpp"

   namespace atxtest_stage_optimize_dead_alpha_determinism {
   // ... fixture helpers identical to S1-1's file (make_trend_research,
   // make_pair_combo, permissive_gate_cfg, passing_metrics, seed_crowded_library)
   // -- kept local per this test suite's own no-cross-file-dependency convention.

   // -- (c) twice-run: the full wire (open-library -> collect-ids -> augment ->
   //    optimize) is byte-identical across two independent run_optimize calls. --
   TEST_F(..., TwiceRunByteIdentical) {
     // seed_crowded_library once; run_optimize(cfg) twice with dead_alpha_factors=true
     // + dead_alpha_lib_dir pointed at the same on-disk fixture; EXPECT_EQ digests
     // + EXPECT_EQ raw books_out bytes (ifstream comparison, matching
     // stage_optimize_riskmodel_test.cpp::TwiceRunFactorByteIdentical's pattern).
   }

   // -- (d) seq==parallel analog: dead-id collection order is not load-bearing. --
   TEST(AtxImplOptimizeDeadAlphaDeterminism, DeadIdOrderInvariant) {
     // Build a small research panel + a library with 3 dead alphas (ascending
     // AlphaId 0,1,2 by construction). Call atx::impl::build_risk_model TWICE
     // directly (bypassing run_optimize/collect_dead_alpha_ids entirely) with the
     // SAME dead_ids set passed in two different orders -- ascending {0,1,2} and
     // shuffled {2,0,1} -- and assert byte-identical artifacts via
     // data::serialize_artifact equality (extract_dead_factors sorts internally,
     // dead_factor.hpp:194-198 -- this proves that guarantee still holds and that
     // collect_dead_alpha_ids's own ascending-scan order is not a correctness
     // dependency, only a documented convention).
     risk::RiskModelConfig cfg;
     cfg.dead_alpha_factors = true;
     std::vector<atx::engine::combine::AlphaId> ascending{{0}, {1}, {2}};
     std::vector<atx::engine::combine::AlphaId> shuffled{{2}, {0}, {1}};
     auto a1 = atx::impl::build_risk_model(panel, cfg, {}, &library, ascending, as_of);
     auto a2 = atx::impl::build_risk_model(panel, cfg, {}, &library, shuffled, as_of);
     ASSERT_TRUE(a1.has_value()); ASSERT_TRUE(a2.has_value());
     EXPECT_EQ(atx::engine::data::serialize_artifact(*a1),
              atx::engine::data::serialize_artifact(*a2));
   }
   } // namespace
   ```

2. **Run:**
   ```powershell
   <scratch>\p9-build.ps1 -Target atx-impl-tests
   <scratch>\p9-ctest.ps1 -R AtxImplOptimizeDeadAlphaDeterminism
   ```
   Expected: both green immediately (S1-1's implementation is already deterministic by construction —
   this unit's value is the regression-guard, not a behavior change).

3. **Commit:**
   ```
   git add atx-impl/tests/stage_optimize_dead_alpha_determinism_test.cpp
   git commit -m "$(cat <<'EOF'
   PF-P9 S1-2: twice-run + dead-id order-invariance proofs for the S1-1 wire

   Closes the mandatory (c)/(d) determinism test classes: the full library-
   open -> collect-ids -> augment -> optimize path reproduces byte-identical
   books across two independent runs, and build_risk_model's result does not
   depend on the CALLER's dead_ids ordering (extract_dead_factors' own
   ascending re-sort absorbs it) -- guarding the wire's ascending-scan
   collection order against ever becoming a silent correctness dependency.

   Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
   EOF
   )"
   ```

---

## Sequencing

1. **S1-0 first** (config field + CLI flag) — S1-1's tests need `cfg.dead_alpha_lib_dir` to exist.
2. **S1-1** (the wire + its own off-path/on-path tests) — the core fix; must land before S1-2, which
   tests the wire S1-1 builds.
3. **S1-2** (twice-run + order-invariance hardening) — pure additive tests, no production-code changes.

Strictly serial (one git index, matches the ROADMAP's SDD rule); no unit can be parallelized against
another since each depends on the previous unit's production-code change.

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| **The "admitted dead_ids" reading is an interpretive call, not literal spec text.** Nothing in `atx-impl/src` ever transitions an alpha to `LifecycleState::Dead` (verified: zero `.mark(`/`LifecycleState::{Live,Decaying,Dead}` hits). A literal `== Dead` filter would make the wire a **permanent no-op** against every real library this pipeline produces. | The sprint could ship "wired" code that never fires outside a hand-built test fixture — the exact Potemkin-book failure mode this sprint exists to fix. | S1 uses `state_as_of NOT IN {Candidate, Recycled}` (the whole admitted pool) as documented above. **Flag this decision for reconciler sign-off before implementation** — if a stricter reading is intended, a follow-up sprint must first add the missing lifecycle-aging driver (a materially bigger, out-of-scope feature) before `== Dead` becomes meaningful. |
| **Three call sites, not two.** The ROADMAP evidence table cites only `stage_optimize.cpp:260,267` (the Factor-kind branch); `stage_optimize.cpp:252` (the Diagonal-kind branch — today's default, and the path a user gets from `--dead-alpha-factors` alone without `--risk-model factor`) has the identical gap via `build_risk_model`'s default arguments. | Fixing only the two ROADMAP-cited lines would leave the *most common* activation recipe (`--dead-alpha-factors` alone) still silently inert. | S1-1 edits all three sites; the orphan-gap table documents the discrepancy explicitly so the reconciler can confirm/amend the ROADMAP's own citation. |
| `library::Library::open` `ATX_ASSERT`-aborts (not a `Result`) if the sqlite file cannot be created — happens whenever the resolved directory's parent path does not exist. | A typo'd or not-yet-created `--dead-alpha-lib-dir`/`--library-dir` would crash the whole `optimize` stage instead of failing open. | S1-1's `maybe_open_dead_lib` checks `std::filesystem::exists` + `is_directory` **before** calling `Library::open`; the "directory missing" acceptance test asserts `run_optimize` returns `Ok`, not a crash. |
| `dead_as_of`'s library-period axis has no defined alignment with the research panel's per-step `fit_end`. | Picking the wrong `dead_as_of` could silently exclude every real dead alpha (an out-of-range read is rejected by `extract_dead_factors`'s own guard, `dead_factor.hpp:189-192`, which `ATX_TRY`-propagates as an `Err` that `build_risk_model` returns — on the Factor per-step call this is swallowed by the existing "warm-up fallback" `has_value()` check and silently degrades to the diagonal fallback for that step, masking the failure). | S1 uses one constant `dead_as_of = lib.n_periods()-1` (the library's own latest snapshot) for the whole run rather than attempting a per-step correspondence — always in-range by construction. Documented as an approximation, not silently assumed. |
| `--library-dir`'s discover-side convention wipes the directory each run **unless** `--library-dir` was explicitly set (`stage_discover.cpp:532,552-553`, `accumulate == !cfg.library_dir.empty()`) — i.e. the accumulating-library behavior this sprint depends on is itself opt-in on the discover side. | If an operator runs discover WITHOUT `--library-dir`, the per-run library under `<alpha_out>/_library` is wiped every run and never accumulates — `--dead-alpha-lib-dir` pointed at it would see at most one run's alphas, or nothing if optimize runs after the directory was wiped by a later discover invocation. | Out of S1's control (a pipeline-orchestration/operator concern, not a code defect) — documented here so the reconciler understands the wire's real-world precondition: a stable `--library-dir` discover convention must already be in use for S1 to have any effect beyond the unit tests' self-contained fixtures. |
| Cross-sprint seam: S3 also edits `stage_optimize.cpp` (the `trade_rate` linear blend inside the `cfg.position_mode` branch, ~line 191). | A careless S1 diff touching unrelated regions of the file could create a merge/rebase conflict with S3. | S1's edits are confined to includes, namespace aliases, the new anonymous-namespace helper block, and the MVO covariance-source-selection block (today ~lines 248-285) — the position-mode branch (lines ~150-220) is untouched. Land S1 first; S3 rebases on it (per the ROADMAP's explicit ordering). |
| `GateConfig{}` passed to `Library::open` in `maybe_open_dead_lib` is unused for S1's read-only calls but still a required constructor argument. | Could look like a wiring bug ("why is the gate config empty?") on review. | Documented inline: this handle never calls `admit()`/`try_admit()`, so the gate floors are inert; only the read methods (`n_alphas`, `n_periods`, `state_as_of`) are exercised. |

---

## Bench / acceptance (sprint close)

- **Default byte-identity:** `AtxImplOptimizeRiskModel.DiagonalByteIdentical` (p8, pre-existing) and the
  new `FailOpen_*` tests all green with the S1 wire compiled in; the pinned
  `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `FactoryOos.MineIntoOffPathDigestUnchanged`, the
  `AtxImplDiscover` determinism slice, and `LibraryVerdict.AdmitKindEnumFrozenPrefix` goldens are
  unaffected (this sprint never touches discover/factory/library-verdict code paths).
- **Per-task RED→GREEN:** `CrowdedPoolDelevers` RED before S1-1's implementation, GREEN after.
- **Crowding win, measured:** on the synthetic crowded-pool fixture, the crowded instrument's absolute
  book weight strictly shrinks once `--dead-alpha-factors` fires against a populated
  `--dead-alpha-lib-dir`, vs. the identical run with the gate off — the concrete, quantified S1 claim
  (the de-crowd direction of the ROADMAP's N_eff = 8.76 northstar, though the northstar number itself is
  a real-panel measurement out of this sprint's synthetic-fixture scope).
- **Twice-run + dead-id order-invariance** proven end to end (S1-2).
- **All three `build_risk_model` call sites in `stage_optimize.cpp` free of a literal `nullptr`
  dead-lib argument** — grep `atx-impl/src/stage_optimize.cpp` for `nullptr` in a `build_risk_model(`
  call returns zero hits after S1-1.

---

## Out of scope

- The `--risk-model factor` Diagonal hardcode in `run_combine`/`stage_metabook` — Sprint 2.
- The linear `trade_rate` blend / GP aim-portfolio trading — Sprint 3 (same file, disjoint region; see
  the cross-sprint seam risk row).
- Adding a lifecycle-aging driver that actually walks an alpha through
  `Admitted → Live → Decaying → Dead` — a materially bigger feature than this wiring sprint; noted as
  the natural follow-up if the reconciler wants the literal `LifecycleState::Dead` reading instead of
  S1's "admitted pool" policy.
- Any change to `stage_discover.cpp`'s accumulating-library logic (the `--library-dir` wipe-vs-accumulate
  convention) — read for context only, not edited.
- Any change to `risk/dead_factor.hpp`, `risk/factor_model.hpp`, or `stage_riskmodel.{hpp,cpp}` — frozen
  p8 math/producer; S1 calls the existing 7-arg `build_risk_model` overload verbatim.
- Per-step (`fit_end`-aligned) `dead_as_of` resolution against the library's own period axis — S1 uses
  one constant latest-snapshot `dead_as_of` for the whole run (documented approximation); a genuine
  per-step correspondence would need a defined mapping between the library's period axis and the
  research panel's date axis, which does not exist today.
- `RiskModelConfig::group_neutralize` / factor-industry neutralization — already wired by p8 (S1-5),
  untouched here.
