# Sprint 4 — Capacity + Turnover as First-Class NSGA Objectives

**Goal:** the GA optimizes FOR high-capacity, low-turnover alphas, not just high-Sharpe
ones. Add `kObjCapacity` (a √-law-impact-derived per-alpha capacity score) and
`kObjTurnover` (a signal first-order-autocorrelation / alpha-decay persistence score —
slower decay is better) as two new, OPT-IN NSGA-II objective columns. CRITICAL
determinism: the objective-vector width MUST stay 7 when both objectives are off (else
`NsgaSearch.ScalarRaw_ReproducesGoldenDigest` breaks) — the two columns are computed
ONLY when their flag is set (no off-path compute at all, mirroring every existing S4.3/
S3-0 opt-in gate in this file), and excluded from the domination/selection vector unless
flagged. Zero new estimator math: capacity reuses `cost::round_trip_cost_bps` +
`cost::capacity_point` (the existing √-impact cost model and zero-crossing helper);
turnover reuses `atx::engine::alpha::detail::ou_ar1_fit` (the existing AR(1) OLS fitter
the VM's `ou_rolling` ops already use).

**Owns (exclusive):**
`atx-engine/include/atx/engine/factory/fitness.hpp`,
`atx-engine/src/factory/fitness.cpp`,
`atx-engine/include/atx/engine/factory/search_driver.hpp` (SearchConfig fields +
`evaluate_generation`'s `gen_fit` derivation only — no other SearchDriver mechanics
change), `atx-impl/src/config.hpp`, `atx-impl/src/config.cpp`,
`atx-impl/src/stage_discover.cpp` (the `SearchConfig`/`FactoryConfig` assembly sites +
CLI validation only); tests under `atx-engine/tests/factory/` and `atx-impl/tests/`.

**Must NOT touch:** `alpha/oracle.hpp` (untouchable every sprint); frozen estimation
bodies in `src/*/*.cpp` (`cost::round_trip_cost_bps`'s own coefficients, `risk/capacity.hpp`'s
`capacity_curve`/`impact_cost_bps` bodies, `alpha/ts_ops.hpp`'s `ou_ar1_fit` body — S4
CALLS these, it does not re-derive them); `factory/pareto.hpp` (the NSGA-II primitives
are already generic over `n_objectives` — S4 needs zero edits there, see the
Architecture note); `factory/search_state.hpp` / `factory/search_progress.hpp` (both
already size off `kMaxObjectives` — S4 needs zero edits there either, see S4-0);
`stage_optimize.cpp`, `stage_combine.cpp`, `risk/garleanu_pedersen.*` (S1/S2/S3);
`stage_metabook.cpp`, `risk/optimizer.hpp`, `factory/factory.cpp`, `loop/*` (S5);
`learn/*`, `fund/*` (S6); `atx-impl/scripts/build-megaalpha-book.ps1` (S7).

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

## The gap (verified file:line)

| Gap | File:line | Evidence |
|---|---|---|
| No capacity slot in the objective vector | `factory/fitness.hpp:183-188` | `inline constexpr atx::usize kMaxObjectives = 7;` and the vector `{wq, diversify, robust, novelty, -cost_bps, -node_count, dsr}` — capacity and turnover have no column at all. |
| The search cannot tell a high-ADV/low-impact alpha from a low-ADV/high-impact one at equal Sharpe | `factory/fitness.cpp:459-481` (`finish_report`) | `objectives[0..2] = {wq, diversify, robust}`; `objectives[4] = -cost_bps` only when `target_aum>0`, and `cost_bps` is a SINGLE-AUM figure, not a capacity headroom measure — nothing rewards spare capacity. |
| The search cannot tell a persistent (slow-decay) alpha from a churny one at equal Sharpe | `factory/fitness.cpp:307-312`, `444-458` (FitnessReport.turnover / the turnover-penalty branch) | `turnover` is the OOS mean bar-to-bar L1 weight change (a MAGNITUDE), never an autocorrelation/persistence measure; the opt-in penalty only discounts `raw` by excess turnover, it does not let NSGA trade persistence against Sharpe. |
| The sqrt-law capacity math exists but is untouched by fitness scoring | `cost/capacity.hpp:64-92` (`capacity_point`), `221-254` (`emit_capacity_scorecard`); `risk/capacity.hpp:271-295` (`capacity_curve`) | All three are POST-HOC reporting helpers over a `PanelView`/`loop`-layer book — never called from `factory/fitness.cpp`, which scores over the disjoint `alpha::Panel` research type. |
| An AR(1)/persistence fitter already exists but is VM-internal | `alpha/ts_ops.hpp:1025-1112` (`OuAr1Fit`, `ou_ar1_fit`, `ou_halflife_of`) | Built for the `ou_rolling` VM op family (`alpha::detail`); never called from `factory/` — though `factory/search_driver.cpp:58`, `factory/mutation.cpp`, `factory/op_catalog.{hpp,cpp}` already reuse OTHER `alpha::detail::` helpers across the module boundary, so this reuse is precedented, not novel. |

---

## Architecture note — what "capacity + turnover as NSGA objectives" actually means

`fitness.hpp`'s objective vector is a **fixed-slot scheme** (documented at `fitness.hpp:270-274`):
slots 0-2 are always live; 3 (novelty), 4 (cost), 5 (parsimony), 6 (dsr) are filled by
DIFFERENT call sites (3/4 inside `finish_report`; 5/6 inside `SearchDriver::evaluate_generation`,
`search_driver.cpp:748-775`) and each bump is a `std::max(n_objectives, slot+1)`, never a hard
assignment — so a slot that is inactive between two active ones stays at its **uniform zero
default**, which is mathematically inert in `pareto.hpp`'s pure-max dominance (every genome
shares the same value on that axis, so it never breaks a tie or flips a front). This is WHY
`assign_pareto_ranks` (`search_driver.cpp:1295-1340`) and `pareto.hpp`'s `dominates` /
`fast_nondominated_sort` / `crowding_distance` need **zero edits** for S4: they already read
`k = max(s.n_objectives)` across the scored set and build the `ObjMatrix` over exactly that
width. S4's entire job is to (a) give slots 7/8 real values and (b) bump `n_objectives`
correctly when their flag is set — the NSGA machinery downstream is already generic.

The two new columns need **strm/panel** (the candidate's realized position/PnL streams and the
research panel) to compute, and those live ONLY inside `detail::fitness_core` — exactly why
`cost_bps`/`selection_cost_bps` are ALSO computed there rather than in `finish_report` (which
has neither in scope; see `fitness.hpp:461-470`'s documented SEAM). S4 follows that same seam:

1. **S4-0** appends `kObjCapacity=7`/`kObjTurnover=8` (bumping `kMaxObjectives` 7→9), adds the
   two inert-default gate bools to BOTH `factory::SearchConfig` (the CLI/registry-facing name,
   per the ROADMAP) and `factory::FitnessCfg` (the internal wiring seam `fitness_core`/
   `finish_report` actually read — mirrors how `target_aum`/`cost_selection` already live on
   `FitnessCfg` alone, and how `deflate_selection` already lives on BOTH `SearchConfig` and is
   copied into `RunConfig`).
2. **S4-1** implements `capacity_sqrt_law_score` (reuses `book_cost_bps` + `cost::capacity_point`)
   and wires it into `fitness_core`/`finish_report`, gated on `FitnessCfg::capacity_objective`.
3. **S4-2** implements `turnover_autocorr_score` (reuses `alpha::detail::ou_ar1_fit`) and wires
   it the same way, gated on `FitnessCfg::turnover_objective`.
4. **S4-3** threads `SearchConfig::capacity_objective`/`turnover_objective` down into the
   per-generation `FitnessCfg` (`search_driver.cpp`'s `gen_fit` derivation, `:703-706`) and up
   through the CLI (`config.hpp`/`config.cpp`/`stage_discover.cpp`), with a fail-loud guard
   (capacity needs a positive `--target-aum` anchor — the ROADMAP anti-roadmap guardrail: "a
   flag that parses but does nothing is a defect").
5. **S4-4** proves the four determinism classes end-to-end.

---

## Determinism contract (Sprint 4)

Every new capability lives behind a config field with an inert default, so the no-flag path is
byte-identical:

- `SearchConfig::capacity_objective = false`, `SearchConfig::turnover_objective = false` — inert;
  no compute, no objective-vector change.
- `FitnessCfg::capacity_objective = false`, `FitnessCfg::turnover_objective = false` — the actual
  gate `fitness_core`/`finish_report` read; mirrors the SearchConfig fields 1:1.
- `kObjCapacity = 7`, `kObjTurnover = 8` are APPEND-ONLY new slots; `kMaxObjectives` grows 7→9.
  **The width used by NSGA-II is `n_objectives`, never `kMaxObjectives`** (see the Architecture
  note) — so widening the fixed-size backing array is, by itself, inert everywhere off-path.

At the inert defaults, `fitness_core` performs NO extra compute (no AUM sweep, no AR(1) fits) and
`finish_report` never touches `objectives[7]`/`objectives[8]` — `n_objectives` is exactly what it
would have been pre-S4 (3 baseline; 4/5/6 if novelty/cost/parsimony/dsr happen to be active). This
is the exact discipline `cost_bps` (`target_aum>0`) and `selection_cost_bps`
(`cost_selection.impact_in_selection && selection_aum>0`) already use.

**Four test classes per opt-in field (mandatory):**
(a) off-path byte-identity — `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`
(`kGoldenDigest = 0xff95ac12512e0e91ULL`) unchanged with both flags appended `false` to
`legacy_pin_cfg()`; a new unit pin that `FitnessCfg{}`/`SearchConfig{}` default both flags off and
`kMaxObjectives==9`/`kObjCapacity==7`/`kObjTurnover==8` (frozen-prefix, append-only).
(b) on-path RED→GREEN — capacity-objective-on makes a high-ADV/low-impact alpha DOMINATE an
equal-Sharpe low-capacity one (front-membership flips, proved directly via `factory::dominates`
at the exact widths `finish_report` produces); turnover-objective-on makes a high-autocorr
slow-decay alpha dominate an equal-Sharpe churny one, same proof shape.
(c) twice-run — identical inputs to `capacity_sqrt_law_score`/`turnover_autocorr_score`/
`pool_aware_fitness` produce bit-identical outputs.
(d) seq==parallel — `SearchDriver::run` with both objectives on and `n_workers` ∈ {1,2,4}
produces an identical digest (mirrors `ScalarRaw_DigestInvariantAcrossWorkers`).

---

## Dependency / wiring map

```
factory/fitness.hpp:183           <- S4-0 kMaxObjectives 7->9; kObjCapacity=7/kObjTurnover=8
factory/fitness.hpp FitnessCfg    <- S4-0 capacity_objective/turnover_objective (inert gates)
factory/fitness.hpp FitnessReport <- S4-0 capacity_score/turnover_autocorr (report projections)
factory/fitness.hpp FitnessCore   <- S4-0 capacity_score/turnover_autocorr (compute carriers)
cost/capacity.hpp:64 capacity_point       <- S4-1 zero-crossing AUM reuse (frozen, call-only)
cost::round_trip_cost_bps (cost_aware.hpp)<- S4-1 the ONE cost model, reused via book_cost_bps
factory/fitness.cpp book_cost_bps:501     <- S4-1 reused VERBATIM per AUM grid point
alpha/ts_ops.hpp:1035 ou_ar1_fit          <- S4-2 AR(1) OLS fit reuse (frozen, call-only)
factory/fitness.cpp fitness_core:285      <- S4-1/S4-2 gated compute (strm/panel in scope here)
factory/fitness.cpp finish_report:421     <- S4-1/S4-2 gated objectives[7]/[8] write + n_objectives bump
factory/search_driver.hpp SearchConfig    <- S4-0 capacity_objective/turnover_objective (CLI-facing)
factory/search_driver.cpp:703 gen_fit     <- S4-3 SearchConfig -> FitnessCfg copy (the missing hop)
factory/search_driver.cpp assign_pareto_ranks:1308 <- UNCHANGED (already generic over n_objectives)
atx-impl/src/config.hpp RunConfig         <- S4-3 capacity_objective/turnover_objective
atx-impl/src/config.cpp apply_flag_value  <- S4-3 --capacity-objective / --turnover-objective
atx-impl/src/stage_discover.cpp           <- S4-3 sc.*/fcfg.search.* wiring + fail-loud guard
tests/factory/fitness_capacity_turnover_test.cpp <- NEW, S4-1/S4-2/S4-4 (auto-globbed)
tests/factory/factory_nsga_search_test.cpp       <- S4-3/S4-4 (legacy_pin_cfg + new TESTs)
atx-impl/tests/ (config parse test)              <- S4-3 CLI round-trip + validation-error test
```

---

## Tasks

### S4-0 — Open ledger + `SearchConfig`/`FitnessCfg` fields + enum/constant append (do first)

**Goal:** create the sprint ledger marker; append `kObjCapacity`/`kObjTurnover` and grow
`kMaxObjectives`; add the four inert-default bool fields (two on `SearchConfig`, two on
`FitnessCfg`); add the two report-projection fields on `FitnessReport` and the two compute
carriers on `FitnessCore`. No behavior change — nothing reads the new fields non-inertly yet.

**Files:** `atx-engine/plans/p9/sprint-4-progress.md` (NEW, ledger marker commit),
`atx-engine/include/atx/engine/factory/fitness.hpp`,
`atx-engine/include/atx/engine/factory/search_driver.hpp`.

**Steps:**

1. In `fitness.hpp` (around `:183-188`), append-only:
   ```cpp
   inline constexpr atx::usize kMaxObjectives = 9; // S4: 7->9 (+capacity +turnover)
   // Objective-slot indices ... 0 wq, 1 diversify, 2 robust, 3 novelty,
   // 4 -cost_bps, 5 -node_count (parsimony), 6 dsr (deflated-Sharpe, R4).
   inline constexpr atx::usize kObjParsimony  = 5;
   inline constexpr atx::usize kObjDeflation  = 6; // R4: deflated-Sharpe selection objective
   // S4: capacity/turnover — APPEND-ONLY (frozen-prefix pin: slots 0-6 unchanged).
   inline constexpr atx::usize kObjCapacity   = 7; // S4-1: sqrt-law impact capacity score
   inline constexpr atx::usize kObjTurnover   = 8; // S4-2: signal first-order autocorrelation
   ```
2. `FitnessCfg` (append at struct end, after `cost_selection`):
   ```cpp
   // S4: capacity/turnover NSGA objective gates. SearchDriver::evaluate_generation
   // copies the SAME-NAMED SearchConfig-level flag into the per-generation FitnessCfg
   // (gen_fit) because only fitness_core/finish_report (which hold strm/panel) can
   // compute the columns. Both default false: FitnessCfg{} is the pre-S4 struct plus
   // two inert bools appended at the end -- no aggregate-init break, no digest drift.
   bool capacity_objective = false; // S4-1: gates the kObjCapacity compute
   bool turnover_objective = false; // S4-2: gates the kObjTurnover compute
   ```
3. `FitnessReport` (append after `turnover`):
   ```cpp
   // S4: bounded, finite projections of the two new NSGA columns -- ALSO the exact
   // value written into objectives[kObjCapacity]/[kObjTurnover] when the gate is on.
   // 0.0 (the default) when off -- the boundary-pin no-op, same convention as
   // cost_bps/turnover above. Do NOT enter `raw`; pure reporting + the objective copy.
   atx::f64 capacity_score{0.0};    // S4-1: bounded [0,1) sqrt-law capacity score
   atx::f64 turnover_autocorr{0.0}; // S4-2: |w|-weighted mean AR(1) coefficient
   ```
4. `detail::FitnessCore` (append after `selection_cost_bps`, and update the field-order
   comment at `fitness.cpp:392-394` to list the two new names last):
   ```cpp
   atx::f64 capacity_score{0.0};
   atx::f64 turnover_autocorr{0.0};
   ```
5. `search_driver.hpp`'s `SearchConfig` (append after `min_viable_raw`):
   ```cpp
   // S4: capacity + turnover as first-class NSGA objectives (kObjCapacity=7,
   // kObjTurnover=8; fitness.hpp). Both default false -> the FitnessCfg mirror stays
   // off -> zero extra compute -> objective-vector width is whatever it was pre-S4
   // (golden-preservation: ROADMAP's S4 registry note). See evaluate_generation's
   // gen_fit derivation for the SearchConfig->FitnessCfg wire (S4-3).
   bool capacity_objective{false}; // --capacity-objective
   bool turnover_objective{false}; // --turnover-objective
   ```
6. Create `atx-engine/plans/p9/sprint-4-progress.md` with a single marker line (ledger
   convention: "append one line per clean review").

**Determinism:** pure addition; every new field is appended at a struct's END (no
aggregate-initializer reordering); `search_state.hpp`'s `CachedScore::objectives` and
`search_progress.hpp`'s checkpoint (de)serializer both size off `kMaxObjectives` directly
(`search_progress.hpp:199,224,242`) — they need **zero code changes**, they simply serialize
2 more (currently-zero) slots. (NOTE for the ledger: this DOES widen the on-disk
`--resume` checkpoint record width, exactly as the S4.1 novelty (3→4) and R4 dsr (6→7... wait,
kMaxObjectives 6→7) bumps already did — a pre-S4 checkpoint is not expected to resume
byte-compatibly across this change; this is an accepted, precedented consequence, not a new
risk class.)

**Accept:**
- Project compiles (dev preset, Unity ON, debug + release).
- Every existing `factory_*`/`fitness_*`/`stage_discover_*` suite green with ZERO other
  edits yet (this task changes only struct shapes + constants).
- NEW `FitnessCapacityTurnover.DefaultsAndEnumPin` (new file, see S4-1):
  ```cpp
  TEST(FitnessCapacityTurnover, DefaultsAndEnumPin) {
    using namespace atx::engine::factory;
    static_assert(kMaxObjectives == 9, "S4 must grow kMaxObjectives 7->9");
    static_assert(kObjCapacity == 7, "kObjCapacity must be appended at slot 7");
    static_assert(kObjTurnover == 8, "kObjTurnover must be appended at slot 8");
    const FitnessCfg fc{};
    EXPECT_FALSE(fc.capacity_objective);
    EXPECT_FALSE(fc.turnover_objective);
    const SearchConfig sc{};
    EXPECT_FALSE(sc.capacity_objective);
    EXPECT_FALSE(sc.turnover_objective);
    const FitnessReport fr{};
    EXPECT_EQ(fr.capacity_score, 0.0);
    EXPECT_EQ(fr.turnover_autocorr, 0.0);
  }
  ```

---

### S4-1 — `capacity_sqrt_law_score`: the kObjCapacity column

**Goal:** a pure, testable function that scores a candidate's √-impact capacity headroom at
a given AUM anchor, reusing the ONE cost model (`cost::round_trip_cost_bps`, already reused
verbatim by `book_cost_bps`) and the existing zero-crossing helper `cost::capacity_point`.
Wire it into `fitness_core`, gated on `cfg.capacity_objective`.

**Root cause:** `fitness.cpp:501-562`'s `book_cost_bps` prices cost at a SINGLE `target_aum`;
nothing sweeps an AUM grid to find where the candidate's edge is exhausted, and nothing
projects that into a bounded NSGA column. `cost::capacity_point` (`cost/capacity.hpp:64-92`)
already does the zero-crossing math but over a `std::span<const risk::CapacityPoint>` curve —
S4-1 builds that curve from `alpha::Panel`-native pieces already in `fitness.cpp`, it does not
re-derive the crossing math.

**Files:** `atx-engine/include/atx/engine/factory/fitness.hpp` (declaration),
`atx-engine/src/factory/fitness.cpp` (implementation + `#include "atx/engine/cost/capacity.hpp"`),
`atx-engine/tests/factory/fitness_capacity_turnover_test.cpp` (NEW).

**Steps:**

1. RED — add the failing test first (new file):
   ```cpp
   // fitness_capacity_turnover_test.cpp
   #include <cmath>
   #include <vector>
   #include <gtest/gtest.h>
   #include "atx/core/types.hpp"
   #include "atx/engine/alpha/panel.hpp"
   #include "atx/engine/alpha/streams.hpp"
   #include "atx/engine/cost/calibration.hpp"
   #include "atx/engine/factory/fitness.hpp"

   namespace atxtest_fitness_capacity_turnover_test {
   using atx::f64;
   using atx::usize;
   using atx::engine::alpha::AlphaStreams;
   using atx::engine::alpha::Panel;
   using atx::engine::exec::CommissionCfg;
   using atx::engine::exec::CommissionMode;
   using atx::engine::exec::ImpactCfg;
   using atx::engine::exec::SlippageCfg;
   using atx::engine::exec::SlippageMode;
   using atx::engine::factory::capacity_sqrt_law_score;
   namespace cost = atx::engine::cost;

   [[nodiscard]] Panel make_panel(usize dates, usize insts, std::vector<std::string> fields,
                                  std::vector<std::vector<f64>> cols) {
     auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), {});
     EXPECT_TRUE(r.has_value()) << "panel fixture must build";
     return std::move(r.value());
   }

   // Deterministic alternating +/-1% oscillation (sigma>0, no RNG) -- ONLY the
   // "volume" column differs between the two fixtures (ADV, hence participation).
   [[nodiscard]] Panel capacity_panel(f64 volume_level) {
     constexpr usize kDates = 10;
     std::vector<f64> close(kDates);
     std::vector<f64> volume(kDates, volume_level);
     f64 px = 100.0;
     for (usize t = 0; t < kDates; ++t) {
       px *= (t % 2 == 0) ? 1.01 : 0.99;
       close[t] = px;
     }
     return make_panel(kDates, 1, {"close", "volume"}, {close, volume});
   }

   [[nodiscard]] AlphaStreams full_weight_strm(usize periods, f64 per_period_edge) {
     AlphaStreams s;
     s.n_alphas_ = 1;
     s.n_periods_ = periods;
     s.n_instruments_ = 1;
     s.pnl_flat.assign(periods, per_period_edge); // constant small positive OOS edge
     s.pos_flat.assign(periods, 0.0);
     s.pos_flat[periods - 1] = 1.0; // full weight, last period
     return s;
   }

   TEST(CapacityObjective, HighAdvLowImpactScoresAboveLowAdv) {
     const Panel high_adv = capacity_panel(1.0e7);  // deep ADV -> low participation
     const Panel low_adv  = capacity_panel(1.0e2);   // thin ADV -> high participation
     const AlphaStreams strm = full_weight_strm(10, 0.001); // 10 bps gross edge/period
     const cost::CalibratedCost cc{ImpactCfg{0.8, 0.5, 0.3}, SlippageCfg{}, cost::FitReport{}};
     constexpr f64 kTargetAum = 1.0e6;

     const f64 score_high = capacity_sqrt_law_score(strm, high_adv, cc, kTargetAum);
     const f64 score_low  = capacity_sqrt_law_score(strm, low_adv, cc, kTargetAum);

     EXPECT_GT(score_high, score_low)
         << "the deep-ADV/low-impact book must score a HIGHER capacity headroom";
     EXPECT_GT(score_low, 0.0) << "sanity: the thin-ADV book still has SOME capacity";
     EXPECT_LE(score_high, 1.0);
     EXPECT_LE(score_low, 1.0);
   }

   TEST(CapacityObjective, NonPositiveTargetAumIsADocumentedNoOp) {
     const Panel panel = capacity_panel(1.0e6);
     const AlphaStreams strm = full_weight_strm(10, 0.001);
     const cost::CalibratedCost cc{ImpactCfg{0.8, 0.5, 0.3}, SlippageCfg{}, cost::FitReport{}};
     EXPECT_EQ(capacity_sqrt_law_score(strm, panel, cc, 0.0), 0.0);
     EXPECT_EQ(capacity_sqrt_law_score(strm, panel, cc, -1.0), 0.0);
   }

   TEST(CapacityObjective, PureFunction_TwiceRunByteIdentical) {
     const Panel panel = capacity_panel(5.0e5);
     const AlphaStreams strm = full_weight_strm(10, 0.0007);
     const cost::CalibratedCost cc{ImpactCfg{0.6, 0.6, 0.2}, SlippageCfg{}, cost::FitReport{}};
     const f64 a = capacity_sqrt_law_score(strm, panel, cc, 2.5e5);
     const f64 b = capacity_sqrt_law_score(strm, panel, cc, 2.5e5);
     EXPECT_EQ(a, b);
   }

   TEST(CapacityObjective, NeverInfOrNaN) {
     // A near-zero impact coefficient (Y~0) -> the curve never crosses zero on the
     // grid -> capacity_point returns +inf -- the EXACT case the bounded transform
     // must absorb without ever handing NSGA an inf/NaN objective.
     const Panel panel = capacity_panel(1.0e9);
     const AlphaStreams strm = full_weight_strm(10, 0.001);
     const cost::CalibratedCost cc{ImpactCfg{1.0e-9, 0.5, 0.0}, SlippageCfg{}, cost::FitReport{}};
     const f64 score = capacity_sqrt_law_score(strm, panel, cc, 1.0e6);
     EXPECT_TRUE(std::isfinite(score));
     EXPECT_GE(score, 0.0);
     EXPECT_LE(score, 1.0);
   }
   } // namespace atxtest_fitness_capacity_turnover_test
   ```
   **Expected FAIL:** `capacity_sqrt_law_score` is undeclared — compile error.

2. GREEN — declare in `fitness.hpp` (public `factory::` namespace, right after
   `book_cost_bps`'s declaration at `:242-244`):
   ```cpp
   // =========================================================================
   //  capacity_sqrt_law_score — S4-1: the kObjCapacity NSGA column.
   //
   //  Sweeps a small log-spaced AUM grid centered on `target_aum` through the SAME
   //  cost model book_cost_bps already prices (cost::round_trip_cost_bps — the ONE
   //  cost surface), builds the (aum, net_edge_bps) curve using the candidate's OWN
   //  realized OOS PnL as the AUM-independent gross edge (1e4 * mean(strm.pnl(0)) —
   //  the SAME quantity risk::capacity.hpp's gross_edge_bps measures for a fitted
   //  book, just already computed by the WQ eval pipeline), and reduces it to a
   //  BOUNDED, FINITE [0,1) score via cost::capacity_point's zero-crossing AUM:
   //
   //      score = capacity_aum / (capacity_aum + target_aum)   (+inf capacity -> 1.0)
   //
   //  BOUNDED so it can never hand NSGA-II's crowding_distance (pareto.hpp) an
   //  unbounded/+inf objective — two genomes tied at a raw +inf capacity_aum would
   //  produce (+inf - +inf)/+inf == NaN in the crowding-distance gap term; this
   //  transform makes that structurally impossible (isinf is special-cased to a
   //  finite 1.0 before the ratio is ever formed).
   //
   //  target_aum <= 0 -> 0.0 (documented degenerate: no AUM anchor, matches
   //  book_cost_bps's own target_aum<=0 guard). PURE; NO RNG; bit-deterministic.
   //  Relies on book_cost_bps(aum) being monotone non-decreasing in aum for a FIXED
   //  weight vector (participation scales linearly with aum; round_trip_cost_bps is
   //  monotone non-decreasing in participation) -- an already-established property
   //  of the S4.3 cost module, not new math; capacity_point's own ATX_CHECK asserts
   //  it at runtime.
   // =========================================================================
   [[nodiscard]] atx::f64 capacity_sqrt_law_score(const alpha::AlphaStreams &strm,
                                                  const alpha::Panel &panel,
                                                  const cost::CalibratedCost &cost,
                                                  atx::f64 target_aum) noexcept;
   ```
3. Implement in `fitness.cpp` (add `#include "atx/engine/cost/capacity.hpp"` near the other
   cost includes; add after `book_cost_bps`'s definition, `:501-562`):
   ```cpp
   namespace detail {
   // S4-1: log-spaced AUM grid, 2 decades either side of `center`, ascending.
   // Mirrors cost::compute_capacity_vector's grid-building CONVENTION
   // (cost/capacity.hpp:145-161) at a coarser resolution -- this objective only
   // needs to bracket the zero-crossing for GA selection pressure, not emit a
   // diagnostic-grade curve, so 8 points is ample and far cheaper per genome.
   inline constexpr atx::usize kCapacityObjGridPoints = 8U;

   [[nodiscard]] std::vector<atx::f64> capacity_obj_aum_grid(atx::f64 center) {
     std::vector<atx::f64> grid;
     grid.reserve(kCapacityObjGridPoints);
     const atx::f64 log_lo = std::log(0.1 * center);
     const atx::f64 log_hi = std::log(10.0 * center);
     const atx::f64 denom = static_cast<atx::f64>(kCapacityObjGridPoints - 1U);
     for (atx::usize k = 0U; k < kCapacityObjGridPoints; ++k) {
       const atx::f64 frac = static_cast<atx::f64>(k) / denom;
       grid.push_back(std::exp(log_lo + frac * (log_hi - log_lo)));
     }
     return grid;
   }
   } // namespace detail

   [[nodiscard]] atx::f64 capacity_sqrt_law_score(const alpha::AlphaStreams &strm,
                                                  const alpha::Panel &panel,
                                                  const cost::CalibratedCost &cost,
                                                  atx::f64 target_aum) noexcept {
     if (target_aum <= 0.0 || strm.n_alphas() == 0U || strm.n_periods() == 0U) {
       return 0.0; // no AUM anchor -> no capacity signal (mirrors book_cost_bps's guard)
     }
     const std::span<const atx::f64> pnl0 = strm.pnl(0U);
     atx::f64 sum = 0.0;
     for (const atx::f64 p : pnl0) { sum += p; }
     const atx::f64 gross_edge_bps =
         pnl0.empty() ? 0.0 : 1.0e4 * (sum / static_cast<atx::f64>(pnl0.size()));

     const std::vector<atx::f64> grid = detail::capacity_obj_aum_grid(target_aum);
     std::vector<risk::CapacityPoint> curve;
     curve.reserve(grid.size());
     for (const atx::f64 aum : grid) { // ascending -> capacity_point's monotonicity precondition
       const atx::f64 cost_bps_at_aum = book_cost_bps(strm, panel, cost, aum); // the ONE cost model
       curve.push_back(risk::CapacityPoint{aum, gross_edge_bps - cost_bps_at_aum});
     }
     const atx::f64 capacity_aum =
         cost::capacity_point(std::span<const risk::CapacityPoint>{curve});
     if (std::isinf(capacity_aum)) {
       return 1.0; // ample capacity (never crosses zero on the grid) -> saturate
     }
     return capacity_aum / (capacity_aum + target_aum);
   }
   ```
4. Build/test wrapper (PowerShell, self-contained):
   ```powershell
   cmake --preset dev
   cmake --build --preset dev --target atx_engine_tests -j
   ctest --preset dev -R "CapacityObjective" --output-on-failure
   ```
5. Commit:
   ```
   git add atx-engine/include/atx/engine/factory/fitness.hpp atx-engine/src/factory/fitness.cpp atx-engine/tests/factory/fitness_capacity_turnover_test.cpp
   git commit -m "PF-S4 S4-1 capacity_sqrt_law_score: kObjCapacity column (unwired)

   Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
   ```

**Accept:** all 4 tests above GREEN; every pre-existing `factory_*` suite still green
(the function is new and unwired — `fitness_core`/`finish_report` do not call it yet, so
this task alone cannot perturb any existing digest).

---

### S4-2 — `turnover_autocorr_score`: the kObjTurnover column

**Goal:** a pure, testable function that scores a candidate's signal persistence — the
|last-period-weight|-weighted mean first-order autocorrelation of each instrument's OWN
target-weight time series — reusing the EXISTING AR(1) OLS fitter `ou_ar1_fit`
(`alpha/ts_ops.hpp:1035`, `namespace atx::engine::alpha::detail`) rather than defining a new
one (confirmed via search: no `factory/`-local autocorrelation helper exists; this is the
"if none, define a small deterministic per-alpha first-order autocorrelation" case, resolved
by finding — and reusing — the VM's own `ou_ar1_fit`, which already IS exactly that).

**Root cause:** `FitnessReport::turnover` (`fitness.cpp:307-312`, `234`) is the mean OOS
bar-to-bar L1 weight CHANGE (a magnitude); nothing measures whether the position series is
slowly-decaying (persistent, needs little rebalancing) or churny (needs constant rebalancing)
at the SAME magnitude of change — the two are different axes and only the second is a genuine
"alpha decay" measure.

**Files:** `fitness.hpp` (declaration), `fitness.cpp` (implementation + `#include
"atx/engine/alpha/ts_ops.hpp"`), `fitness_capacity_turnover_test.cpp` (extend).

**Steps:**

1. RED — append to the test file:
   ```cpp
   using atx::engine::factory::turnover_autocorr_score;

   // An EXACT (noise-free) AR(1) process x[t] = mu + (x0-mu)*phi^t satisfies
   // x[t] = mu*(1-phi) + phi*x[t-1] -- OLS recovers b==phi with ZERO residual.
   // mu=0.5, phi=0.9, x0=1.0 -> a slowly-decaying, highly persistent series.
   [[nodiscard]] AlphaStreams persistent_strm(usize periods) {
     AlphaStreams s;
     s.n_alphas_ = 1; s.n_periods_ = periods; s.n_instruments_ = 1;
     s.pnl_flat.assign(periods, 0.0);
     s.pos_flat.resize(periods);
     for (usize t = 0; t < periods; ++t) {
       s.pos_flat[t] = 0.5 + 0.5 * std::pow(0.9, static_cast<f64>(t));
     }
     return s;
   }

   // An EXACT phi=-1 alternation: x[t] = -x[t-1] -- maximal churn, b==-1.0.
   [[nodiscard]] AlphaStreams churny_strm(usize periods) {
     AlphaStreams s;
     s.n_alphas_ = 1; s.n_periods_ = periods; s.n_instruments_ = 1;
     s.pnl_flat.assign(periods, 0.0);
     s.pos_flat.resize(periods);
     for (usize t = 0; t < periods; ++t) {
       s.pos_flat[t] = (t % 2 == 0) ? 0.5 : -0.5;
     }
     return s;
   }

   TEST(TurnoverObjective, PersistentSeriesScoresAboveChurnySeries) {
     const AlphaStreams slow = persistent_strm(20);
     const AlphaStreams churn = churny_strm(20);
     const f64 score_slow = turnover_autocorr_score(slow);
     const f64 score_churn = turnover_autocorr_score(churn);
     EXPECT_NEAR(score_slow, 0.9, 1e-6) << "exact AR(1) phi=0.9 must recover b~=0.9";
     EXPECT_NEAR(score_churn, -1.0, 1e-6) << "exact alternation must recover b~=-1.0";
     EXPECT_GT(score_slow, score_churn);
   }

   TEST(TurnoverObjective, ConstantSeriesDegenerateFitIsSkippedNotZeroed) {
     // Instrument 0 constant (zero predictor variance -> NaN fit, must be SKIPPED);
     // instrument 1 the persistent series -- the score must reflect ONLY inst 1.
     AlphaStreams s;
     s.n_alphas_ = 1; s.n_periods_ = 20; s.n_instruments_ = 2;
     s.pnl_flat.assign(20, 0.0);
     s.pos_flat.assign(40, 0.0);
     for (usize t = 0; t < 20; ++t) {
       s.pos_flat[t * 2 + 0] = 1.0;                                    // constant
       s.pos_flat[t * 2 + 1] = 0.5 + 0.5 * std::pow(0.9, static_cast<f64>(t)); // persistent
     }
     EXPECT_NEAR(turnover_autocorr_score(s), 0.9, 1e-6);
   }

   TEST(TurnoverObjective, ZeroLastPeriodWeightExcludesInstrument) {
     AlphaStreams s = churny_strm(20);
     // Zero the last-period weight for the (only) instrument -> no contributor.
     s.pos_flat.back() = 0.0;
     EXPECT_EQ(turnover_autocorr_score(s), 0.0);
   }

   TEST(TurnoverObjective, PureFunction_TwiceRunByteIdentical) {
     const AlphaStreams s = persistent_strm(15);
     EXPECT_EQ(turnover_autocorr_score(s), turnover_autocorr_score(s));
   }
   ```
   **Expected FAIL:** `turnover_autocorr_score` undeclared — compile error.

2. GREEN — declare in `fitness.hpp` (public `factory::` namespace, after
   `capacity_sqrt_law_score`):
   ```cpp
   // =========================================================================
   //  turnover_autocorr_score — S4-2: the kObjTurnover NSGA column.
   //
   //  The |last-period-weight|-weighted mean first-order autocorrelation (AR(1)
   //  coefficient b) of each instrument's OWN target-weight time series
   //  (strm.positions(0, t)[i] across t=0..n_periods-1), reusing
   //  atx::engine::alpha::detail::ou_ar1_fit (ts_ops.hpp) -- the SAME AR(1) OLS
   //  fitter the VM's ou_rolling ops already use. Cross-module detail:: reuse is
   //  precedented (factory/search_driver.cpp:58, factory/mutation.cpp,
   //  factory/op_catalog.{hpp,cpp} already reach into alpha::detail::). NO new
   //  estimator math.
   //
   //  A persistent (slowly-decaying) position series has b -> 1 (needs little
   //  rebalancing); a churny one has b -> 0 or negative. b is a raw OLS slope over
   //  finite input -- never +inf/NaN except ou_ar1_fit's OWN documented degenerate
   //  return (< 2 valid lag pairs or zero predictor variance), which this function
   //  SKIPS (does not zero-in) so a constant/dead instrument never drags a real
   //  signal toward 0. Instruments with a zero/NaN last-period weight are also
   //  skipped (no notional, no turnover signal to weight in -- mirrors
   //  book_cost_bps's dead-name skip). 0.0 when no instrument contributes.
   //  PURE; NO RNG; bit-deterministic.
   // =========================================================================
   [[nodiscard]] atx::f64 turnover_autocorr_score(const alpha::AlphaStreams &strm) noexcept;
   ```
3. Implement in `fitness.cpp` (add `#include "atx/engine/alpha/ts_ops.hpp"`; add after
   `capacity_sqrt_law_score`):
   ```cpp
   [[nodiscard]] atx::f64 turnover_autocorr_score(const alpha::AlphaStreams &strm) noexcept {
     const atx::usize insts = strm.n_instruments();
     const atx::usize periods = strm.n_periods();
     if (insts == 0U || periods < 3U || strm.n_alphas() == 0U) {
       return 0.0; // need >= 2 lag pairs (ou_ar1_fit's own floor)
     }
     const std::span<const atx::f64> last_w = strm.positions(0U, periods - 1U);
     std::vector<atx::f64> series;
     series.reserve(periods);
     atx::f64 wsum = 0.0;
     atx::f64 acc = 0.0;
     for (atx::usize i = 0U; i < insts; ++i) { // ascending inst -> order-fixed reduction
       const atx::f64 wi = last_w[i];
       if (std::isnan(wi) || wi == 0.0) {
         continue; // dead name -> no turnover signal to weight in
       }
       series.clear();
       for (atx::usize t = 0U; t < periods; ++t) { // ascending period -> order-fixed
         series.push_back(strm.positions(0U, t)[i]);
       }
       const alpha::detail::OuAr1Fit fit =
           alpha::detail::ou_ar1_fit(std::span<const atx::f64>{series});
       if (std::isnan(fit.b)) {
         continue; // degenerate fit -- SKIP, do not zero-in a real neighbour's signal
       }
       const atx::f64 abs_w = std::abs(wi);
       acc += abs_w * fit.b;
       wsum += abs_w;
     }
     return (wsum == 0.0) ? 0.0 : acc / wsum;
   }
   ```
4. Build/test wrapper:
   ```powershell
   cmake --preset dev
   cmake --build --preset dev --target atx_engine_tests -j
   ctest --preset dev -R "TurnoverObjective" --output-on-failure
   ```
5. Commit:
   ```
   git add atx-engine/include/atx/engine/factory/fitness.hpp atx-engine/src/factory/fitness.cpp atx-engine/tests/factory/fitness_capacity_turnover_test.cpp
   git commit -m "PF-S4 S4-2 turnover_autocorr_score: kObjTurnover column (unwired)

   Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
   ```

**Accept:** all 5 tests GREEN; every pre-existing suite unaffected (still unwired).

---

### S4-3 — Gate the compute, bump the width, wire the CLI

**Goal:** wire `capacity_sqrt_law_score`/`turnover_autocorr_score` into `fitness_core`/
`finish_report` behind `FitnessCfg::capacity_objective`/`turnover_objective`; thread
`SearchConfig::capacity_objective`/`turnover_objective` into the per-generation `FitnessCfg`;
add the two CLI flags end-to-end with a fail-loud validation guard. `assign_pareto_ranks`
and every `pareto.hpp` primitive need **zero edits** (Architecture note) — this task's only
job is making `n_objectives`/`objectives[7]`/`objectives[8]` correct at the source.

**Files:** `atx-engine/src/factory/fitness.cpp` (`fitness_core`, `finish_report`),
`atx-engine/src/factory/search_driver.cpp` (`gen_fit` derivation, `:703-706`),
`atx-impl/src/config.hpp`, `atx-impl/src/config.cpp`, `atx-impl/src/stage_discover.cpp`,
`atx-engine/tests/factory/factory_nsga_search_test.cpp` (extend `legacy_pin_cfg`).

**Steps:**

1. RED — extend `factory_nsga_search_test.cpp` (append to `legacy_pin_cfg`, per the file's own
   convention "Each new task appends ONE line here pinning its knob's legacy value"):
   ```cpp
   c.capacity_objective = false; // S4: capacity objective off on the boundary pin
   c.turnover_objective = false; // S4: turnover objective off on the boundary pin
   ```
   and add the RED test (fails until step 2/3 land):
   ```cpp
   TEST(NsgaSearch, CapacityTurnoverObjectives_DefaultOff_ByteIdentical) {
     Library lib{};
     Panel panel = fixture_panel(96, 6);
     WeightPolicy policy{};
     ExecutionSimulator sim = frictionless_sim();
     SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};
     AlphaStore pool{};
     const SearchResult r = driver.run(legacy_pin_cfg(777), pool);
     EXPECT_EQ(r.digest, kGoldenDigest)
         << "ScalarRaw boundary pin broken by kMaxObjectives growth (7->9).";
   }
   ```
   (This is initially GREEN by construction since the fields are inert at this point — the
   genuinely RED step is the S4-1/S4-2 wiring tests below, which fail to compile/assert until
   step 2 lands.)

2. GREEN — wire `fitness_core` (`fitness.cpp:285-399`), inserting after the existing (6b)
   selection-cost block and before the `return atx::core::Ok(FitnessCore{...})`:
   ```cpp
   // (6c) S4-1: sqrt-law capacity objective, GATED on cfg.capacity_objective (mirrors
   // the S4.3 cost gate -- zero compute at all when off, preserving both the
   // off-path byte-identity AND the off-path perf cost).
   atx::f64 capacity_score = 0.0;
   if (cfg.capacity_objective) {
     capacity_score = capacity_sqrt_law_score(strm, panel, cfg.cost, cfg.target_aum);
   }
   // (6d) S4-2: turnover/alpha-decay objective, GATED on cfg.turnover_objective.
   atx::f64 turnover_autocorr = 0.0;
   if (cfg.turnover_objective) {
     turnover_autocorr = turnover_autocorr_score(strm);
   }
   ```
   and append the two fields to the return aggregate init (order MUST match the struct):
   ```cpp
   return atx::core::Ok(FitnessCore{std::move(agg.oos_pnl), wq, robust, dsr.dsr,
                                    dsr.haircut_sharpe, cost_bps, agg.turnover,
                                    split.sharpe_h1, split.sharpe_h2, split.stable,
                                    selection_cost_bps, capacity_score, turnover_autocorr});
   ```
3. Wire `finish_report` (`fitness.cpp:421-497`), inserting after the existing S4.3 cost block
   (`if (cost_active) {...}`) and before the S4.2 descriptor copy:
   ```cpp
   // S4-1: kObjCapacity -- active iff cfg.capacity_objective. std::max (not a hard
   // assignment) so this never REGRESSES n_objectives if cost_active already bumped
   // it to 5 -- mirrors search_driver.cpp's kObjParsimony/kObjDeflation bump pattern.
   rep.capacity_score = core.capacity_score;
   if (cfg.capacity_objective) {
     rep.objectives[kObjCapacity] = core.capacity_score;
     rep.n_objectives = static_cast<atx::u8>(
         std::max<atx::usize>(rep.n_objectives, kObjCapacity + 1U));
   }
   // S4-2: kObjTurnover -- active iff cfg.turnover_objective.
   rep.turnover_autocorr = core.turnover_autocorr;
   if (cfg.turnover_objective) {
     rep.objectives[kObjTurnover] = core.turnover_autocorr;
     rep.n_objectives = static_cast<atx::u8>(
         std::max<atx::usize>(rep.n_objectives, kObjTurnover + 1U));
   }
   ```
4. Wire `search_driver.cpp`'s `gen_fit` derivation (`:703-706`), appending:
   ```cpp
   FitnessCfg gen_fit = cfg.fitness;
   if (cfg.deflate_selection) {
     gen_fit.trial_count = cfg.prior_trial_count + std::max<atx::usize>(1U, canon.size());
   }
   // S4-3: thread the SearchConfig-level objective gates into the per-generation
   // FitnessCfg -- fitness_core/finish_report (which alone have strm/panel in scope)
   // read the FitnessCfg mirror, never SearchConfig directly. Both default false on
   // BOTH structs, so this copy is a no-op unless the caller set the SearchConfig
   // flag (byte-identical off-path).
   gen_fit.capacity_objective = cfg.capacity_objective;
   gen_fit.turnover_objective = cfg.turnover_objective;
   ```
5. CLI: `config.hpp` (near `deflate_selection`):
   ```cpp
   // --capacity-objective / --turnover-objective (S4): opt-in NSGA objectives.
   // capacity_objective REQUIRES --target-aum > 0 (its AUM anchor) -- validated in
   // stage_discover.cpp (fail-loud, not a silent no-op). Both default false ->
   // byte-identical to today.
   bool capacity_objective = false; // --capacity-objective -> SearchConfig
   bool turnover_objective = false; // --turnover-objective -> SearchConfig
   ```
   `config.cpp` — add to the valueless-boolean dispatch:
   ```cpp
   if (flag == "capacity-objective") { cfg.capacity_objective = true; return atx::core::Ok(); } // S4-3
   if (flag == "turnover-objective") { cfg.turnover_objective = true; return atx::core::Ok(); } // S4-3
   ```
   and append `|| flag == "capacity-objective" || flag == "turnover-objective"` to the
   valueless-flags if-chain in `parse_args`.
6. `stage_discover.cpp` — in the `sc` build block (near `sc.fitness.turnover_penalty_slope`,
   `:1039-1040`):
   ```cpp
   sc.capacity_objective = cfg.capacity_objective; // S4: kObjCapacity gate
   sc.turnover_objective = cfg.turnover_objective;  // S4: kObjTurnover gate
   ```
   and in the `fcfg.search.*` overrides block (near `fcfg.search.fitness.target_aum`, `:583`):
   ```cpp
   fcfg.search.capacity_objective = cfg.capacity_objective; // also fixes the ungated path
   fcfg.search.turnover_objective = cfg.turnover_objective;
   ```
   and the fail-loud guard (alongside the existing `--industry-neutral` guard,
   `:1004-1009`):
   ```cpp
   if (cfg.capacity_objective && cfg.target_aum <= 0.0) {
       return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
           "discover: --capacity-objective requires --target-aum > 0 (the capacity "
           "score's AUM anchor); without it the objective would be a silent no-op.");
   }
   ```
   and the manifest KV audit trail (`:185`, `:807`):
   ```cpp
   kv_d("capacity_objective", cfg.capacity_objective ? 1.0 : 0.0);
   kv_d("turnover_objective", cfg.turnover_objective ? 1.0 : 0.0);
   ...
   mf << "capacity_objective=" << cfg.capacity_objective << '\n';
   mf << "turnover_objective=" << cfg.turnover_objective << '\n';
   ```
7. Build/test wrapper:
   ```powershell
   cmake --preset dev
   cmake --build --preset dev --target atx_engine_tests atx_impl_tests -j
   ctest --preset dev -R "NsgaSearch|CapacityObjective|TurnoverObjective|FitnessCapacityTurnover" --output-on-failure
   ctest --preset dev -R "config_parse|stage_discover" --output-on-failure
   ```
8. Commit:
   ```
   git add atx-engine/src/factory/fitness.cpp atx-engine/src/factory/search_driver.cpp atx-impl/src/config.hpp atx-impl/src/config.cpp atx-impl/src/stage_discover.cpp atx-engine/tests/factory/factory_nsga_search_test.cpp
   git commit -m "PF-S4 S4-3 wire capacity/turnover objectives: gated width + CLI

   Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
   ```

**Accept:**
- `NsgaSearch.ScalarRaw_ReproducesGoldenDigest` unchanged (`kGoldenDigest` untouched).
- `NsgaSearch.CapacityTurnoverObjectives_DefaultOff_ByteIdentical` GREEN.
- A new `atx-impl` config-parse test: `--capacity-objective`/`--turnover-objective` round-trip
  to `RunConfig` true/true; `--capacity-objective` WITHOUT `--target-aum` returns
  `Err(InvalidArgument)` from `run_discover_gated`/`run_discover`.
- `atx-impl`'s `AtxImplDiscover` determinism slice unchanged when neither flag is passed.

---

### S4-4 — The four determinism classes, end-to-end

**Goal:** close the loop with integration-level proofs of all four mandatory classes,
using the REAL `factory::dominates` primitive for the RED→GREEN front-membership flip (no
flaky full-GA-run needed for that proof) plus a full `SearchDriver::run` for seq==parallel.

**Files:** `atx-engine/tests/factory/factory_nsga_search_test.cpp` (extend),
`atx-engine/tests/factory/fitness_capacity_turnover_test.cpp` (extend).

**Steps:**

1. (b) RED→GREEN, capacity — append to `factory_nsga_search_test.cpp`:
   ```cpp
   TEST(NsgaSearch, CapacityObjective_FlipsFrontMembership) {
     using atx::engine::factory::dominates;
     using atx::engine::factory::kObjCapacity;
     std::array<atx::f64, 9> high{}; // equal wq/diversify/robust; differ ONLY on capacity
     std::array<atx::f64, 9> low{};
     high[0] = low[0] = 0.5;
     high[1] = low[1] = 0.8;
     high[2] = low[2] = 1.0;
     high[kObjCapacity] = 0.9; // deep-ADV/low-impact
     low[kObjCapacity]  = 0.1; // thin-ADV/high-impact

     // OFF (width 7 -- the capacity slot excluded): neither dominates.
     EXPECT_FALSE(dominates({high.data(), 7}, {low.data(), 7}));
     EXPECT_FALSE(dominates({low.data(), 7}, {high.data(), 7}));

     // ON (width 8 -- capacity slot 7 included): high STRICTLY dominates low.
     EXPECT_TRUE(dominates({high.data(), 8}, {low.data(), 8}))
         << "capacity-objective-on must make the high-ADV/low-impact alpha dominate "
            "the equal-Sharpe low-capacity one";
     EXPECT_FALSE(dominates({low.data(), 8}, {high.data(), 8}));
   }
   ```
2. (b) RED→GREEN, turnover:
   ```cpp
   TEST(NsgaSearch, TurnoverObjective_FlipsFrontMembership) {
     using atx::engine::factory::dominates;
     using atx::engine::factory::kObjTurnover;
     std::array<atx::f64, 9> slow_decay{}; // slot 7 (capacity) inert-default 0 on BOTH
     std::array<atx::f64, 9> churny{};
     slow_decay[0] = churny[0] = 0.5;
     slow_decay[1] = churny[1] = 0.8;
     slow_decay[2] = churny[2] = 1.0;
     slow_decay[kObjTurnover] = 0.9;  // persistent, slow decay
     churny[kObjTurnover]     = -1.0; // maximal churn

     EXPECT_FALSE(dominates({slow_decay.data(), 7}, {churny.data(), 7}));
     EXPECT_TRUE(dominates({slow_decay.data(), 9}, {churny.data(), 9}))
         << "turnover-objective-on must make the slow-decay alpha dominate the "
            "equal-Sharpe churny one";
     EXPECT_FALSE(dominates({churny.data(), 9}, {slow_decay.data(), 9}));
   }
   ```
3. (b) end-to-end variant — append to `fitness_capacity_turnover_test.cpp`, proving the SAME
   flip with REAL `pool_aware_fitness` output (not hand-built numbers):
   ```cpp
   TEST(FitnessCapacityTurnover, EndToEndObjectivesReflectCapacityGap) {
     FitnessCfg cfg{};
     cfg.capacity_objective = true;
     cfg.target_aum = 1.0e6;
     // ... build two genomes/panels differing only in "volume" (as in S4-1's
     // fixture), call pool_aware_fitness on each, and assert:
     //   rep_high->objectives[kObjCapacity] > rep_low->objectives[kObjCapacity];
     //   rep_high->n_objectives >= kObjCapacity + 1.
   }
   ```
4. (c) twice-run — append:
   ```cpp
   TEST(FitnessCapacityTurnover, TwiceRun_ObjectivesBitIdentical) {
     FitnessCfg cfg{};
     cfg.capacity_objective = true;
     cfg.turnover_objective = true;
     cfg.target_aum = 5.0e5;
     // ... call pool_aware_fitness twice on the SAME genome/panel/pool/cfg and
     // EXPECT_EQ every entry of objectives[0..8] (memcmp the two arrays).
   }
   ```
5. (d) seq==parallel — append:
   ```cpp
   TEST(NsgaSearch, CapacityTurnoverObjectives_DigestInvariantAcrossWorkers) {
     Library lib{};
     Panel panel = fixture_panel(96, 6); // NOTE: no "volume" field -> capacity
     // saturates to a UNIFORM 1.0 for every candidate here (harmless for (d),
     // which needs only cross-worker reproducibility, not discrimination).
     WeightPolicy policy{};
     ExecutionSimulator sim = frictionless_sim();
     SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};
     const std::array<usize, 3> worker_counts{1, 2, 4};
     atx::u64 first_digest = 0;
     for (usize wi = 0; wi < worker_counts.size(); ++wi) {
       SearchConfig cfg = legacy_pin_cfg(777);
       cfg.objective_mode = ObjectiveMode::MultiObjective;
       cfg.capacity_objective = true;
       cfg.turnover_objective = true;
       cfg.fitness.target_aum = 1.0e6;
       cfg.n_workers = worker_counts[wi];
       AlphaStore pool{};
       const SearchResult r = driver.run(cfg, pool);
       if (wi == 0) { first_digest = r.digest; }
       else { EXPECT_EQ(r.digest, first_digest) << "digest changed at n_workers=" << worker_counts[wi]; }
     }
   }
   ```
6. Build/test wrapper:
   ```powershell
   cmake --preset dev
   cmake --build --preset dev --target atx_engine_tests -j
   ctest --preset dev -R "NsgaSearch|FitnessCapacityTurnover" --output-on-failure
   ```
7. Commit:
   ```
   git add atx-engine/tests/factory/factory_nsga_search_test.cpp atx-engine/tests/factory/fitness_capacity_turnover_test.cpp
   git commit -m "PF-S4 S4-4 four determinism classes for capacity/turnover objectives

   Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
   ```

**Accept:** all tests above GREEN; the full `atx_engine_tests`/`atx_impl_tests` suites green;
`NsgaSearch.ScalarRaw_ReproducesGoldenDigest` and `FactoryOos.MineIntoOffPathDigestUnchanged`
unchanged; `LibraryVerdict.AdmitKindEnumFrozenPrefix` unaffected (S4 does not touch
`library::`).

---

## Sequencing

1. **S4-0 first** (ledger + struct/constant shapes) — every later unit depends on it.
2. **S4-1** and **S4-2** in parallel after S4-0 (disjoint: two independent new public
   functions in the same two files, no shared lines — land S4-1's commit, then S4-2's, to
   keep the diff reviewable).
3. **S4-3** after both — it is the ONLY task that touches `fitness_core`/`finish_report`'s
   control flow, `search_driver.cpp`, and the CLI; it depends on S4-1/S4-2's functions existing.
4. **S4-4** last — integration proofs over the fully-wired path.

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| An unbounded capacity AUM (possibly +inf) reaches `pareto.hpp`'s `crowding_distance` | Two genomes tied at +inf produce `(+inf-+inf)/range == NaN`, corrupting crowding-distance-based selection non-deterministically across ties | `capacity_sqrt_law_score`'s bounded `[0,1)` saturating transform (S4-1) makes an unbounded/inf objective value structurally impossible — `NeverInfOrNaN` pins this. |
| `book_cost_bps(aum)` is assumed monotone non-decreasing in `aum` (the precondition `cost::capacity_point`'s `ATX_CHECK` asserts) | A non-monotone cost model would ABORT the whole eval in debug builds | This is an EXISTING property of `cost::round_trip_cost_bps` (participation scales linearly with `aum` for a fixed weight vector, and the sqrt-impact law is monotone in participation) — S4 does not introduce new risk here, only a new CALLER of the same guarded contract. |
| `capacity_objective=true` with `target_aum<=0` (no AUM anchor) | A parsed flag that silently does nothing — the ROADMAP anti-roadmap "fail-loud, never silent no-op" guardrail | `stage_discover.cpp`'s validation guard (S4-3 step 6) rejects this combination with a loud `Err`, not a quiet 0.0 score. |
| Growing `kMaxObjectives` 7→9 widens the on-disk `--resume` checkpoint record (`search_progress.hpp`) | A checkpoint captured pre-S4 cannot resume byte-compatibly post-S4 | Accepted, precedented consequence (every prior `kMaxObjectives`/`n_objectives` growth — novelty 3→4, dsr 6→7 width bump — already did this); out of S4's scope to solve, documented in S4-0's ledger entry. |
| `turnover_autocorr_score`'s per-instrument AR(1) fit is `O(periods)` per instrument, on top of `book_cost_bps`'s existing `O(insts)` windowed cost | Slower fitness eval when BOTH objectives are on | Both are COLD, once-per-distinct-candidate calls (the file's own documented perf ethos, `fitness.hpp:54-55`); gated compute means the cost is paid ONLY when the flags are set — never on the off-path or in the ScalarRaw boundary-pin path. |
| A degenerate AR(1) fit (`ou_ar1_fit` returns NaN `b`) silently ZEROING an instrument's contribution instead of being skipped | Would bias the weighted mean toward 0 for a genuinely well-behaved neighbour set, corrupting the persistence signal | `turnover_autocorr_score` explicitly SKIPS (via `continue`, not zero-injection) a degenerate instrument — `ConstantSeriesDegenerateFitIsSkippedNotZeroed` pins this. |
| Reaching into `atx::engine::alpha::detail::ou_ar1_fit` crosses a module's "detail" encapsulation boundary | Could be flagged as an inappropriate cross-module reach | Precedented: `factory/search_driver.cpp:58`, `factory/mutation.cpp`, `factory/op_catalog.{hpp,cpp}` already reuse OTHER `alpha::detail::` helpers from `factory/` — S4 follows the SAME established pattern, not a new one. |

---

## Bench / acceptance (sprint close)

- **Default byte-identity:** `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`
  (`kGoldenDigest = 0xff95ac12512e0e91ULL`) and `FactoryOos.MineIntoOffPathDigestUnchanged`
  unchanged with default `SearchConfig`/`FitnessCfg` (both new flags false).
- **Per-task RED→GREEN:** each opt-in has a test RED before the wire, GREEN after (S4-1's 4
  tests, S4-2's 5 tests, S4-3's config/discover tests, S4-4's front-membership-flip tests).
- **Capacity-model win, measured:** `CapacityObjective.HighAdvLowImpactScoresAboveLowAdv`
  quantifies the deep-ADV vs thin-ADV score gap on a controlled fixture;
  `NsgaSearch.CapacityObjective_FlipsFrontMembership` proves the front-membership consequence.
- **Turnover-model win, measured:** `TurnoverObjective.PersistentSeriesScoresAboveChurnySeries`
  quantifies the exact AR(1) recovery (`b≈0.9` vs `b≈-1.0`);
  `NsgaSearch.TurnoverObjective_FlipsFrontMembership` proves the front-membership consequence.
- **Twice-run + seq==parallel** on both objective paths (S4-4 steps 4/5).
- **Dev-panel smoke:** `--capacity-objective --turnover-objective --target-aum 1e6` on the
  short synthetic discover fixture completes and the manifest KV block records both flags
  (no long-running full-panel sweep — per the ROADMAP's testing directive).

---

## Out of scope

- Book-level (cross-sleeve-netted) capacity/turnover constraints in the optimizer's QP —
  Sprint 5 (`--participation-cap`, `--book-turnover-gate`).
- A realized-edge (rather than last-period-weight-proxy) capacity estimate — an existing,
  documented limitation of `capacity_for_alpha`/`compute_capacity_vector` (`cost/capacity.hpp:120-125`)
  that S4's `capacity_sqrt_law_score` inherits by the same convention; out of this sprint's scope
  to fix.
- Exposing `--capacity-objective`/`--turnover-objective` in the prod recipe
  (`build-megaalpha-book.ps1`) — Sprint 7.
- Re-deriving `cost::round_trip_cost_bps`, `cost::capacity_point`, or `alpha::detail::ou_ar1_fit`
  — all three are frozen, call-only reuse.
