# Declarative Swap-Lane Strategy DSL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `StrategySpec`/`DeclarativeStrategy` with a fixed-expiry daily-restrike lifecycle and a variance-swap leg lane so the XOM strangle-vs-varswap comparison is a ~50-line spec, then delete `strangle_varswap.{hpp,cpp}` and its 600-line driver behind a track-parity gate.

**Architecture:** Move the swap-leg mechanics (contract transcription, entry solve, engine-accrual signal mirror) out of `strangle_varswap.cpp` into a reusable `swap_leg` module first, rewiring the old strategy onto it so its 71K test suite guards the move. Then add the grammar + interpreter mode, prove parity old-vs-new on a synthetic corpus (CI) and the real XOM 2026 db (fixture-gated), ship the compact example, and delete the old files.

**Tech Stack:** C++20, clang-cl `/W4 /WX`, GoogleTest, CMake presets via `scripts\atx-build.ps1` (MUST run from `C:\atx-wt\pool-5` — wrong-tree guard), existing atx-vol engine/DSL.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-08-02-strategy-swap-dsl-design.md`.
- Worktree `C:\atx-wt\pool-5`, branch `feat/strategy-swap-dsl`.
- TARGETED test filters only, never the full suite (standing user instruction).
- Build: `powershell -File scripts\atx-build.ps1` from the worktree root (debug preset default); test binary `build\bin\atx_vol_tests.exe --gtest_filter=...`.
- Empty `spec.swap_legs` + existing lifecycles must stay bit-identical (dispersion golden `final_nav = 24740.624124981368` untouched — guarded by `SurfaceDbDispersionBacktest` suite, do NOT run it per-task; it runs in Task 8's final certification).
- Never commit `atx-vol/{include/atx/vol/var.hpp, src/var.cpp, tests/var_test.cpp, bench/var_bench.cpp}` — untracked user WIP, not ours.
- Error policy: config errors `InvalidArgument` (fatal, first `on_step`); data failures soft + counted, never silent; NaN = not measured, 0.0 = measured zero.
- New public code documented in the codebase's contract-comment style (see strategy.hpp for the register).

## File Structure

- `include/atx/vol/swap_leg.hpp` + `src/swap_leg.cpp` (NEW) — swap-leg toolkit: `swap_contract_for_lot`, `solve_cycle_swap`, `SwapSignalProbe`.
- `include/atx/vol/strategy.hpp` — grammar additions (`SwapSizeSpec`, `SwapLegSpec`, `Holding::FixedExpiryRestrike`, `select_fixed_cycle_expiry`), `DeclarativeStrategy` members/accessors/signals override.
- `src/strategy.cpp` — validation + restrike interpreter + swap lane + probe wiring.
- `tests/swap_leg_test.cpp` (NEW), `tests/strategy_test.cpp` (extend), `tests/strategy_restrike_parity_test.cpp` (NEW; deleted again in Task 8).
- `examples/varswap_compare_example.cpp` (NEW), replaces `examples/strangle_varswap_driver.cpp`.
- DELETED in Task 8: `include/atx/vol/strangle_varswap.hpp`, `src/strangle_varswap.cpp`, `examples/strangle_varswap_driver.cpp`, `tests/strangle_varswap_test.cpp`, `tests/strategy_restrike_parity_test.cpp`.
- `atx-vol/CMakeLists.txt`, `atx-vol/CHANGELOG.md`, `atx-vol/README.md` — bookkeeping.

---

### Task 1: `swap_leg` module — contract transcription + `SwapSignalProbe` (move, rewire old strategy)

**Files:**
- Create: `atx-vol/include/atx/vol/swap_leg.hpp`, `atx-vol/src/swap_leg.cpp`
- Modify: `atx-vol/CMakeLists.txt:134` area (add `src/swap_leg.cpp` to the library source list), `atx-vol/include/atx/vol/strangle_varswap.hpp` (drop `SwapMirror`/`swap_contract`/mirror members, hold a `SwapSignalProbe`), `atx-vol/src/strangle_varswap.cpp` (delete moved code, call the module)
- Test: `atx-vol/tests/swap_leg_test.cpp` (NEW; register beside the other tests in CMakeLists — grep `strangle_varswap_test` for the pattern), plus the existing `StrangleVarswap*` suite as the regression net

**Interfaces:**
- Consumes: `SwapLot`, `RealizedVarianceSpec`, `PortfolioState`, `MarketSnapshot`, `SurfaceRef`, `detail::deriv_greeks_on_ref` (all existing).
- Produces (later tasks rely on these exact signatures):

```cpp
// swap_leg.hpp — namespace atx::vol
[[nodiscard]] DerivContract swap_contract_for_lot(const SwapLot &lot, std::int64_t base_ts,
                                                  const RealizedVarianceSpec &rv) noexcept;

class SwapSignalProbe {
public:
  void capture_pre_step(const PortfolioState &book);        // ids before the strategy mutates
  void refresh(const MarketSnapshot &base, const PortfolioState &book); // after a SUCCESSFUL step
  // swap_delta/gamma/vega/theta/rho as name->value pairs appended to `out`,
  // qty-scaled sums under the engine's mark config (DerivConfig{}); all five NaN
  // unless: stepped, base is the as-of snapshot, >=1 live lot, no desynced
  // mirror, every lot priced.
  void append_swap_greek_signals(const MarketSnapshot &base,
                                 std::vector<std::pair<std::string, double>> &out) const;
  [[nodiscard]] bool stepped() const noexcept;
private:
  struct Mirror { /* == old StrangleVsVarswapStrategy::SwapMirror, moved */ };
  std::vector<SwapLot> live_swaps_;
  std::vector<Mirror> mirrors_;
  std::vector<std::uint64_t> ids_before_step_;
  std::int64_t signal_ts_ns_{0};
  bool stepped_{false};
};
```

- [ ] **Step 1: Write the failing test** — `tests/swap_leg_test.cpp`. Build the smallest usable fixture: one synthetic eSSVI `PricedSurface` via the exact `make_surface` recipe in `examples/strategy_examples.cpp:46-80` (copy it into the test's anonymous namespace; uid 7, S=F=100, `kR=0.043`). Assert the transcription and the probe's core discipline:

```cpp
TEST(SwapLeg, ContractForLotCarriesResidualTenorAndStagedRv) {
  SwapLot lot;
  lot.kind = DerivKind::VarSwap;
  lot.strike_dec = 0.04;
  lot.notional = 1.0;
  lot.expiry_ts_ns = kBaseNow + 91 * kDayNs;
  RealizedVarianceSpec rv{};
  rv.annualization = 252.0;
  rv.n_obs_total = 63;
  const DerivContract c = swap_contract_for_lot(lot, kBaseNow, rv);
  EXPECT_EQ(c.kind, DerivKind::VarSwap);
  EXPECT_NEAR(c.maturity_t, 91.0 * kDayNs / static_cast<double>(kNsPerYear), 1e-15);
  EXPECT_EQ(c.strike_dec, 0.04);
  EXPECT_EQ(c.rv_spec.n_obs_total, 63u);
}

TEST(SwapLeg, ProbeReportsNaNBeforeAnyStepAndOnWrongSnapshot) { /* probe with no refresh():
  append_swap_greek_signals fills 5 NaN entries named swap_delta..swap_rho */ }

TEST(SwapLeg, ProbeSeedsOneStepLateAndMarksRestoredLotsDesynced) { /* two refresh() calls
  on consecutive-ts snapshots: lot adopted on refresh 1 takes NO fixing (seed on 2);
  a lot present in capture_pre_step's book WITHOUT a prior mirror => desynced => NaN greeks */ }
```

- [ ] **Step 2: Run to verify failure** — `powershell -File scripts\atx-build.ps1` from `C:\atx-wt\pool-5`, expect compile failure (`swap_leg.hpp` absent). That IS the red state for a new module; then keep the filter handy: `build\bin\atx_vol_tests.exe --gtest_filter=SwapLeg.*`
- [ ] **Step 3: Implement** — create `swap_leg.hpp/cpp`. MOVE (not rewrite): `swap_contract` body from `src/strangle_varswap.cpp:148-165` → `swap_contract_for_lot`; the `SwapMirror` struct and `refresh_signal_state` body from `strangle_varswap.cpp:384-443` → `Mirror` + `refresh` (drop the `swap_ids_before_step` parameter — the probe captures it itself in `capture_pre_step`, `strangle_varswap.cpp:277-284`); the greeks block of `signals()` from `strangle_varswap.cpp:445-497` → `append_swap_greek_signals` (keep `kEngineSwapMarkCfg` = `DerivConfig{}` as a file-local in swap_leg.cpp with its comment). Carry every contract comment with the code. Header doc: thread-safety = one probe per strategy per thread, same register as strategy.hpp's borrow notes.
- [ ] **Step 4: Rewire the old strategy** — `strangle_varswap.hpp` loses `SwapMirror`, `swap_contract`, `find_mirror`, `refresh_signal_state`, `live_swaps_`, `swap_mirrors_`, `signal_ts_ns_`, `stepped_`, `swap_ids_before_step_`; gains `SwapSignalProbe probe_;`. `on_step` becomes: `probe_.capture_pre_step(book); ATX_TRY_VOID(step(...)); probe_.refresh(base, book);`. `signals()` calls `probe_.append_swap_greek_signals(base, out)` then appends `strangle_vega` (NaN unless `probe_.stepped()` and ts matches — keep the as-of check by having the probe expose it via the NaN discipline; `strangle_vega` uses `last_strangle_vega_` guarded by the same condition, so keep a `std::int64_t signal_ts_ns_` mirror OR simplest: `strangle_vega` NaN unless the five probe values were attempted this snapshot — replicate the old `as_of` check with a small `last_step_ts_ns_` member the strategy stamps in `on_step`), `skipped_restrikes`, `skipped_swaps`. Entry solve (`open_cycle_swap`) keeps calling `swap_contract_for_lot` from the module.
- [ ] **Step 5: Run tests green** — `build\bin\atx_vol_tests.exe --gtest_filter=SwapLeg.*:StrangleVarswap*` Expected: all pass (the old suite proves the move changed nothing).
- [ ] **Step 6: Commit** — `git add -A -- atx-vol/include/atx/vol/swap_leg.hpp atx-vol/src/swap_leg.cpp atx-vol/include/atx/vol/strangle_varswap.hpp atx-vol/src/strangle_varswap.cpp atx-vol/tests/swap_leg_test.cpp atx-vol/CMakeLists.txt` then `git commit -m "refactor(vol): extract the swap-leg signal probe and contract bridge into swap_leg"`

### Task 2: `solve_cycle_swap` (move the entry solve, rewire old strategy)

**Files:**
- Modify: `atx-vol/include/atx/vol/swap_leg.hpp`, `atx-vol/src/swap_leg.cpp`, `atx-vol/include/atx/vol/strangle_varswap.hpp`, `atx-vol/src/strangle_varswap.cpp`
- Test: `atx-vol/tests/swap_leg_test.cpp`

**Interfaces:**
- Produces:

```cpp
// swap_leg.hpp
struct CycleSwapRequest {
  std::uint32_t uid{0};
  DerivKind kind{DerivKind::VarSwap};
  double cap_dec{0.0};
  double notional{1.0};
  double annualization{252.0};
  std::int64_t open_ts_ns{0};
  std::int64_t expiry_ts_ns{0};
  std::span<const std::int64_t> session_ts; // sorted; fixing schedule source
  DerivConfig deriv_cfg{};                  // ENTRY SOLVE ONLY
};
// Fair-struck, entry-solved lot with qty = target_vega / swap entry vega.
// `target_vega` is the DOLLAR vega the lot must match (sign carries).
// Every skip cause is Err(Unavailable, "<cause>"); `lot.id` is left 0 for the
// caller's watermark. Ok(lot) mutates nothing anywhere.
[[nodiscard]] Result<SwapLot> solve_cycle_swap(const SurfaceRef &surface,
                                               const CycleSwapRequest &req, double target_vega);
```

- [ ] **Step 1: Write the failing tests** in `swap_leg_test.cpp` (same synthetic-surface fixture; a 5-session `session_ts` grid spanning open→expiry):

```cpp
TEST(SwapLeg, SolveCycleSwapStrikesFairAndSizesToTargetVega) {
  // req over 4 accruable sessions; target_vega = 2500.0
  const Result<SwapLot> lot = solve_cycle_swap(surface_ref, req, 2500.0);
  ASSERT_TRUE(lot.has_value());
  EXPECT_GT(lot->strike_dec, 0.0);
  EXPECT_EQ(lot->n_obs_total, 3u);        // sessions in (open, expiry] minus 1
  // qty * entry vega reproduces the target within FP noise:
  const Result<DerivGreeks> g = detail::deriv_greeks_on_ref(
      surface_ref, swap_contract_for_lot(*lot, lot->start_ts_ns, staged_rv), req.deriv_cfg, {});
  ASSERT_TRUE(g.has_value());
  EXPECT_NEAR(lot->qty * g->vega, 2500.0, 1e-6 * 2500.0);
}
TEST(SwapLeg, SolveCycleSwapRefusesAOneSessionCycle) { /* 1 accruable session =>
  Err(Unavailable); message mentions the fixing count */ }
TEST(SwapLeg, SolveCycleSwapRefusesNonFiniteTargetVega) { /* NaN / 0.0 target => Err(Unavailable) */ }
```

- [ ] **Step 2: Verify red** — `build\bin\atx_vol_tests.exe --gtest_filter=SwapLeg.SolveCycleSwap*` (compile failure first, then failing asserts).
- [ ] **Step 3: Implement** — MOVE `open_cycle_swap`'s body (`strangle_varswap.cpp:178-267`) into `solve_cycle_swap`: finite/zero target-vega guard; fixing-window count via the two `upper_bound`s (`:193-206`, incl. the `kUint32Max` narrowing guard and its comment); lot assembly; fair strike via `deriv_price_on_ref` on `swap_contract_for_lot(lot, open_ts_ns, staged rv)` (keep the "read off the quote, DELIBERATELY" comment block); entry greeks via `deriv_greeks_on_ref`; `qty = target_vega / greeks->vega`. Each early-out becomes `Err(ErrorCode::Unavailable, ...)` with a one-line cause.
- [ ] **Step 4: Rewire** — `StrangleVsVarswapStrategy::open_cycle_swap` shrinks to: build `CycleSwapRequest` from `cfg_` + cycle state, call `solve_cycle_swap`, on `Ok` assign `lot.id = next_lot_id++` and push; on `Err` `++skipped_swap_cycles_`.
- [ ] **Step 5: Green** — `build\bin\atx_vol_tests.exe --gtest_filter=SwapLeg.*:StrangleVarswap*`
- [ ] **Step 6: Commit** — `git commit -m "refactor(vol): extract the per-cycle swap entry solve into swap_leg"` (add the four modified files + test).

### Task 3: Grammar + validation (`SwapLegSpec`, `SwapSizeSpec`, `FixedExpiryRestrike`, `select_fixed_cycle_expiry`)

**Files:**
- Modify: `atx-vol/include/atx/vol/strategy.hpp` (grammar structs near `SizeSpec`/`LifecycleSpec`; free function beside `lifecycle_decide`; `DeclarativeStrategy` members), `atx-vol/src/strategy.cpp` (validation + expiry selection only — no stepping logic yet)
- Test: `atx-vol/tests/strategy_test.cpp` (append a new `StrategyRestrikeValidation` + `SelectFixedCycleExpiry` section)

**Interfaces:**
- Produces (exact grammar from the spec §1; later tasks consume verbatim):

```cpp
struct SwapSizeSpec {
  enum class Kind : std::uint8_t { FixedQty = 0, TargetVega = 1, MatchGroupVega = 2 };
  Kind kind{Kind::MatchGroupVega};
  double value{0.0};
  double sign{+1.0};
  std::string group; // MatchGroupVega: option-leg group; empty = ALL option legs
};
struct SwapLegSpec {
  std::string symbol;
  std::uint32_t uid{0};
  DerivKind kind{DerivKind::VarSwap};
  double cap_dec{0.0};
  double notional{1.0};
  double annualization{252.0};
  SwapSizeSpec size{};
  DerivConfig deriv_cfg{};
  std::string group;
};
// StrategySpec gains: std::vector<SwapLegSpec> swap_legs;
// LifecycleSpec::Holding gains: FixedExpiryRestrike = 3
// Free function (beside lifecycle_decide):
//   first session in `sessions` at or after base_ts+tenor_ns; sessions.back()
//   when the anchor is past the end; 0 when no session > base_ts remains.
[[nodiscard]] std::int64_t select_fixed_cycle_expiry(std::span<const std::int64_t> sessions,
                                                     std::int64_t base_ts,
                                                     std::int64_t tenor_ns) noexcept;
// DeclarativeStrategy public accessors (counters wired in Tasks 4-5):
//   skipped_restrikes(), unopened_entry_steps(), skipped_swap_cycles()  (all std::uint64_t)
```

- Validation rules (first `on_step`, `Status` channel — the strategy gains `bool validated_{false}`; existing modes validate nothing new). `FixedExpiryRestrike` requires: non-empty sorted `session_ts`; >= 1 option leg; ALL option legs share one finite positive `tenor.target_T` with both snap flags false (`InvalidArgument` naming the leg otherwise); `SizeSpec::Kind::Weight` and any `CrossLegConstraint` other than `None` rejected `InvalidArgument`; `MissingNameSpec` must be default (`Error` policy). `swap_legs` non-empty requires `FixedExpiryRestrike` (`NotImplemented` otherwise); each swap leg: symbol or uid set; finite `notional > 0`; `annualization > 0`; capped kinds (`CappedVarSwap`/`CappedVolSwap` — check `DerivKind` enum in derivatives.hpp for exact names) require `cap_dec > 0`, uncapped require `cap_dec == 0`; `MatchGroupVega` group must name a group at least one option leg carries (empty = all); `FixedQty`/`TargetVega` require finite value, `TargetVega` value > 0.

- [ ] **Step 1: Failing tests** (`strategy_test.cpp`; construct `DeclarativeStrategy` + call `on_step` on any snapshot — validation fires before resolution, so an EMPTY `MarketSnapshot` fixture from the existing tests in that file is enough; grep `MarketSnapshot` in strategy_test.cpp and reuse its cheapest fixture):

```cpp
TEST(SelectFixedCycleExpiry, CeilSnapsFallsBackToLastAndExhausts) {
  const std::int64_t s[] = {100, 200, 300};
  EXPECT_EQ(select_fixed_cycle_expiry(s, 90, 100), 200);   // ceil: 190 -> 200
  EXPECT_EQ(select_fixed_cycle_expiry(s, 90, 250), 300);   // past end -> last
  EXPECT_EQ(select_fixed_cycle_expiry(s, 300, 100), 0);    // nothing after base
}
TEST(StrategyRestrikeValidation, RejectsMismatchedLegTenors) { /* two legs, target_T 0.25 vs 0.5
  => on_step returns InvalidArgument mentioning tenor */ }
TEST(StrategyRestrikeValidation, RejectsSwapLegsOutsideRestrikeMode) { /* HoldToExpiry +
  one swap leg => NotImplemented */ }
TEST(StrategyRestrikeValidation, RejectsCappedKindWithoutCap) { /* CappedVarSwap cap_dec 0
  => InvalidArgument */ }
TEST(StrategyRestrikeValidation, RejectsUnknownMatchGroup) { /* MatchGroupVega group "x",
  no option leg grouped "x" => InvalidArgument */ }
```

- [ ] **Step 2: Verify red** — `build\bin\atx_vol_tests.exe --gtest_filter=SelectFixedCycleExpiry.*:StrategyRestrikeValidation.*`
- [ ] **Step 3: Implement** — grammar structs + enumerator + accessors in strategy.hpp with contract comments (spec §1 semantics: cycle-expiry rule, keep-strikes policy, swap-leg open-once rule, `deriv_cfg` entry-solve-only caveat carried from strangle_varswap.hpp:79-88); `select_fixed_cycle_expiry` = `select_cycle_expiry` body from `strangle_varswap.cpp:90-101` (overflow-guarded anchor, `lower_bound`, last-session fallback, `> base_ts` gate); file-local `validate_restrike_spec(const StrategySpec&) -> Status` in strategy.cpp implementing the rule list above; `on_step` calls it once when `holding == FixedExpiryRestrike || !swap_legs.empty()`, then (this task) returns `Ok()` without stepping in restrike mode (Task 4 replaces that stub — tests here only exercise validation failures and the free function).
- [ ] **Step 4: Green** — same filter.
- [ ] **Step 5: Commit** — `git commit -m "feat(vol): add the swap-leg + fixed-expiry-restrike grammar and validation"`

### Task 4: Interpreter — option lane of `FixedExpiryRestrike`

**Files:**
- Modify: `atx-vol/src/strategy.cpp` (new `step_restrike` path dispatched from `on_step`), `atx-vol/include/atx/vol/strategy.hpp` (private members), `docs/superpowers/specs/2026-08-02-strategy-swap-dsl-design.md` (one line: accessor is `unopened_entry_steps`, lane-agnostic, superseding the spec's `unopened_strangle_steps`)
- Test: `atx-vol/tests/strategy_test.cpp` (`StrategyRestrike` section)

**Interfaces:**
- Consumes: `select_fixed_cycle_expiry`, `resolve_strike` (strategy.hpp:284), `SurfaceRef::full_greek_seed(K, T, side, analytic, execution)` (see `strangle_varswap.cpp:132`), existing `StructureSpec` expansion semantics.
- Produces: `DeclarativeStrategy` private: `std::int64_t cycle_expiry_ts_ns_{0}; std::int64_t cycle_tenor_ns_{0}; std::uint64_t skipped_restrikes_{0}; std::uint64_t unopened_entry_steps_{0}; std::uint64_t skipped_swap_cycles_{0}; double last_options_vega_{NaN}; std::int64_t last_step_ts_ns_{0};` and the flow Task 5 extends at the marked point.

Per-step flow (`step_restrike(base, step_index, book, next_lot_id, price_options)`):
1. `last_entry_seeds_.clear(); last_options_vega_ = NaN; last_step_ts_ns_ = base.ts_ns();`
2. Cycle roll: if `cycle_expiry_ts_ns_ <= base_ts` → `select_fixed_cycle_expiry(spec_.session_ts, base_ts, cycle_tenor_ns_)`; 0 ⇒ `Ok()` (grid exhausted); else `++cohort_counter_`, `cycle_opened = true`. (`cycle_tenor_ns_` computed in validation: `round(target_T * kNsPerYear)` with the range guard from `strangle_varswap.cpp:70-75`.)
3. Restrike-tick gate: `EveryNDays` with `step_index % n != 0` AND not `cycle_opened` ⇒ hold the book, `Ok()`.
4. Resolve every option leg pinned to the cycle: `T = (cycle_expiry_ts_ns_ - base_ts) / kNsPerYear`; per structure side, `resolve_strike(surface, TenorSpec{T, false, false}, side, selector)` then `surface->full_greek_seed(K, T, side, price_options.analytic_greeks, price_options.query_execution)`; mark = `seed->greeks().price`, rejected unless finite and `>= 0`. ANY soft failure (missing symbol/surface, failed strike, failed seed, bad mark) ⇒ keep-strikes: `book.lots.empty() ? ++unopened_entry_steps_ : ++skipped_restrikes_`; [SWAP-SKIP POINT — Task 5]; `Ok()`.
5. Sizing per leg's `SizeSpec` — `FixedContracts`: `qty = sign * value`; `TargetVega/TargetTheta/TargetGamma`: `qty = sign * scaled_target / (|Σ per-share structure greek| * 100.0)` with TargetTheta's 365.25 scaling — mirror the existing base-sizing arithmetic in `resolve_spec_impl` (strategy.cpp; grep `TargetTheta` for the block) rather than inventing new formulas; keep multiplier 100.0 as there.
6. Commit: `book.lots.clear();` push one `Lot` per resolved side (id from `next_lot_id++`, `contract = OptionContract{uid, K, T, side}`, qty, multiplier 100, `expiry_ts_ns = cycle_expiry_ts_ns_` EXACT int64, `cohort = cohort_counter_`, `entry_price = mark`); `last_options_vega_` = Σ per-share vega × qty × multiplier over the fresh lots (per group, retain a small `group -> vega` map local; total in the member); seeds → `last_entry_seeds_`.
7. [SWAP-OPEN POINT — Task 5] then `Ok()`.

- [ ] **Step 1: Failing tests** — synthetic corpus fixture: build 8 daily snapshots exactly as `examples/strategy_examples.cpp:128-141` does (surface uid 7, `session_ts` = the 8 timestamps), spec = 0.10-tenor Delta-0.40 strangle, `FixedContracts 100`, `EveryStep` + `FixedExpiryRestrike`:

```cpp
TEST(StrategyRestrike, FixesOneExpiryAndRestrikesDailyAtIt) {
  // drive on_step over 3 snapshots by hand (PortfolioState book; next_lot_id=1)
  // after step 0: 2 lots, expiry == select_fixed_cycle_expiry(sessions, ts0, tenor_ns), cohort 1
  // after step 1: 2 NEW lot ids, SAME expiry, cohort 1, strikes differ from step 0's
  // after step 2: same again; skipped counters all 0
}
TEST(StrategyRestrike, KeepsLiveStrikesWhenTheSurfaceCannotServeTheDelta) {
  // snapshot 1 replaced by one WITHOUT uid 7's surface (empty archive item set for that name):
  // book unchanged after step 1, skipped_restrikes()==1, unopened_entry_steps()==0
}
TEST(StrategyRestrike, CountsUnopenedStepsWhenNothingWasHeld) {
  // FIRST snapshot lacks the surface: book stays empty, unopened_entry_steps()==1
}
TEST(StrategyRestrike, EveryNDaysHoldsBetweenRestrikeTicks) {
  // entry_every_n=2: step 1 leaves lot ids unchanged; step 2 restrikes
}
```

- [ ] **Step 2: Verify red** — `build\bin\atx_vol_tests.exe --gtest_filter=StrategyRestrike.*`
- [ ] **Step 3: Implement** the flow above; `on_step` dispatches `if (spec_.lifecycle.holding == LifecycleSpec::Holding::FixedExpiryRestrike) return step_restrike(...);` before the existing flow (which stays byte-identical).
- [ ] **Step 4: Green** — same filter, plus `--gtest_filter=Strategy.*` (existing declarative suite untouched).
- [ ] **Step 5: Commit** — `git commit -m "feat(vol): interpret the fixed-expiry daily-restrike lifecycle in DeclarativeStrategy"`

### Task 5: Interpreter — swap lane + probe + signals

**Files:**
- Modify: `atx-vol/src/strategy.cpp`, `atx-vol/include/atx/vol/strategy.hpp` (`SwapSignalProbe probe_;` member — include swap_leg.hpp; `signals` override declaration)
- Test: `atx-vol/tests/strategy_test.cpp` (`StrategyRestrikeSwap` section)

**Interfaces:**
- Consumes: `solve_cycle_swap`, `SwapSignalProbe` (Task 1-2 signatures), the two marked points in Task 4's flow.
- Produces: `DeclarativeStrategy::signals(base)` override emitting, ONLY when `!spec_.swap_legs.empty()`: the probe's 5 columns + `options_vega` (`last_options_vega_`, guarded `base.ts_ns() == last_step_ts_ns_` else NaN) + `skipped_restrikes` + `skipped_swaps` (cumulative doubles). Empty vector otherwise (existing behavior).

Wiring: `on_step` (restrike mode, swap legs non-empty) does `probe_.capture_pre_step(book)` before `step_restrike`, `probe_.refresh(base, book)` after success. At SWAP-SKIP POINT: if `cycle_opened`, `skipped_swap_cycles_ += spec_.swap_legs.size()`. At SWAP-OPEN POINT: if `cycle_opened`, per swap leg: resolve uid (leg.uid or `base.uid_of(symbol)`), surface; target vega per `SwapSizeSpec::Kind` — `MatchGroupVega`: the group's slice of the just-computed group→vega map (empty group = total; missing group unreachable, validated); `TargetVega`: `sign * value`; `FixedQty`: skip the vega path, qty = value directly (still fair-struck: build the request, call `solve_cycle_swap` with a sentinel? NO — split: `solve_cycle_swap` keeps the vega contract; for `FixedQty` call it with `target_vega = NaN`? Also no. Cleanest: `solve_cycle_swap` gains nothing; the interpreter computes `target_vega` for the two vega kinds and for `FixedQty` calls `solve_cycle_swap` with `target_vega = 1.0` then overwrites `lot.qty = size.value` before assigning the id — the fair strike and validation are qty-independent; document inline). Each `Err` ⇒ `++skipped_swap_cycles_`; each `Ok` ⇒ `lot.id = next_lot_id++; book.swap_lots.push_back(lot);`.

- [ ] **Step 1: Failing tests** (same synthetic fixture, swap leg added: `DerivKind::VarSwap`, `MatchGroupVega` empty group):

```cpp
TEST(StrategyRestrikeSwap, OpensOneFairStruckEqualVegaSwapPerCycle) {
  // step 0: book.swap_lots.size()==1; strike_dec>0; expiry == option lots' expiry;
  // qty * entry vega ≈ last options vega (recompute entry vega via deriv_greeks_on_ref
  // exactly as the SwapLeg test does); step 1 (same cycle): STILL 1 swap lot, same id.
}
TEST(StrategyRestrikeSwap, SignalsCarryTheEightColumnsWithNaNDiscipline) {
  // after step 0: signals(base0) has swap_delta..swap_rho all finite? NO —
  // probe adopted the lot this step and greeks need a live mirror: they are
  // FINITE (lot adopted, not desynced, priced on this snapshot) — assert the five
  // are finite, options_vega finite, skipped_* == 0.  On a snapshot mismatch
  // (signals(base1) after stepping base0) all five + options_vega are NaN.
}
TEST(StrategyRestrikeSwap, OneLeggedCycleCountsSkippedSwaps) {
  // session grid truncated so the cycle holds one session => solve refuses =>
  // skipped_swap_cycles()==1, book.swap_lots empty, option lots still open.
}
TEST(StrategyRestrikeSwap, EmptySwapLegsEmitsNoSignals) { /* restrike spec without
  swap legs: signals() returns empty vector */ }
```

- [ ] **Step 2: Verify red** — `build\bin\atx_vol_tests.exe --gtest_filter=StrategyRestrikeSwap.*`
- [ ] **Step 3: Implement** per wiring above.
- [ ] **Step 4: Green** — `build\bin\atx_vol_tests.exe --gtest_filter=StrategyRestrike*:SwapLeg.*:Strategy.*`
- [ ] **Step 5: Commit** — `git commit -m "feat(vol): open and observe declarative swap legs through the cycle lifecycle"`

### Task 6: Parity gates — synthetic (CI) + XOM 2026 db (fixture-gated)

**Files:**
- Create: `atx-vol/tests/strategy_restrike_parity_test.cpp` (register in CMakeLists beside the other tests)
- Test: itself

**Interfaces:**
- Consumes: `StrangleVsVarswapStrategy` (old), `DeclarativeStrategy` (new), `run_backtest` / `run_timed` (`research/backtest_driver.hpp`), `Clock::from_manifest`, `BacktestResult` row fields (grep `nav`/`pnl_total`/`swap_pnl`/`swap_pv` in backtest.hpp for exact member names), `write_backtest_tsv` NOT needed.
- Produces: the deletion licence for Task 8.

Helper (file-local): `equivalent_spec(symbol, delta, tenor_T, contracts, session_ts) -> StrategySpec` — one strangle leg (Delta delta both wings), `FixedContracts contracts` sign +1, `tenor.target_T = tenor_T`, `EveryStep` + `FixedExpiryRestrike`, `HedgeSpec{DeltaToZero, Daily, 0.0}`, one `SwapLegSpec{VarSwap, MatchGroupVega{}}`, `session_ts` filled. Old config: `StrangleVarswapConfig{symbol, delta, tenor_T, contracts, session_ts}` defaults elsewhere.

```cpp
TEST(StrategyRestrikeParity, SyntheticCorpusTracksMatchTheOldStrategy) {
  // 10-snapshot synthetic corpus (strategy_examples recipe, drifting spot + vol);
  // run old and new through run_backtest with identical RunConfig; assert per-row:
  //   |a-b| <= 1e-9 * max(1,|a|) on nav, pnl_total, swap_pnl, swap_pv;
  // identical lot-id sequences (compare final books + per-row lot counts);
  // identical counters (old strategy accessors vs new accessors).
}
TEST(StrategyRestrikeParity, Xom2026DbTrackMatchesTheOldStrategy) {
  // GTEST_SKIP() unless std::filesystem::exists("C:/atx-data/surface-db/scratch-fitfix-2026");
  // build the Clock the way examples/strangle_varswap_driver.cpp does (probe_sessions,
  // driver lines ~199-288 — lift the minimal loader, not the arg parser);
  // XOM, delta 0.40, tenor 0.25, contracts 100; same assertions as above.
}
```

- [ ] **Step 1: Write both tests** (red: `equivalent_spec` compiles but assertions can't run until… they CAN run — Tasks 3-5 landed. Red here means: write asserts first, expect them to PASS if the implementation is right; TDD's failure-first is satisfied by deliberately asserting a WRONG tolerance (0.0) once to watch rows compared, then setting 1e-9. Alternatively perturb the spec (delta 0.39) to watch the harness catch a real mismatch, then fix to 0.40. Do the latter — proves the harness bites.)
- [ ] **Step 2: Verify the harness bites** — `build\bin\atx_vol_tests.exe --gtest_filter=StrategyRestrikeParity.Synthetic*` with delta 0.39: FAIL on row values. Restore 0.40.
- [ ] **Step 3: Green (synthetic)** — same filter. Investigate ANY mismatch to root cause (systematic-debugging); the likely benign one is a last-ulp strike difference from tenor re-derivation — if seen, document measured magnitude in the test comment; tolerance stays 1e-9.
- [ ] **Step 4: Green (db)** — build release CLI if needed: tests run against the DEBUG build; the db test only reads the surface db, so debug is fine. `build\bin\atx_vol_tests.exe --gtest_filter=StrategyRestrikeParity.Xom*` Expected: PASS (or SKIP off-box).
- [ ] **Step 5: Commit** — `git commit -m "test(vol): pin declarative-vs-bespoke strangle-varswap track parity"`

### Task 7: Example binary + plot reproduction

**Files:**
- Create: `atx-vol/examples/varswap_compare_example.cpp`
- Modify: `atx-vol/CMakeLists.txt` (add `atx-vol-varswap-compare-example` target beside the old driver's block at :539-540, same link libs)

**Interfaces:**
- Consumes: `equivalent_spec` shape from Task 6 (inline it — examples don't link tests), Clock loading as in the parity db test, `run_timed`, `write_backtest_tsv`, `attach_swap_columns` (driver :289 — lift if the TSV needs the swap column enrichment; check what the old driver appended and reproduce it so plot_fitfix.py's columns exist: `pnl_total` col 3, `swap_pnl` col 37 in the 34-comment-line track.tsv format).
- Produces: the deliverable example (~50-60 lines of body).

Body sketch (complete, adapt includes):

```cpp
int main(int argc, char **argv) {
  const std::string db = argc > 1 ? argv[1] : "C:/atx-data/surface-db/scratch-fitfix-2026";
  const std::string symbol = argc > 2 ? argv[2] : "XOM";
  constexpr double kDelta = 0.40, kTenorT = 0.25, kContracts = 100.0;
  auto clock = /* load manifest + probe symbol sessions, as the parity test does */;
  StrategySpec spec; /* strangle leg + swap leg + FixedExpiryRestrike + hedge, session_ts from clock */
  DeclarativeStrategy strat{std::move(spec)};
  auto outcome = run_timed(*clock, strat);
  if (!outcome) { std::fprintf(stderr, "%s\n", outcome.error().to_string().c_str()); return 1; }
  /* attach swap columns; write TSV; print tearsheet headline + counters */
}
```

- [ ] **Step 1: Write the example + CMake target**, build: `powershell -File scripts\atx-build.ps1`
- [ ] **Step 2: Run on the fixed db** — `build\bin\atx-vol-varswap-compare-example.exe` → TSV. Then regenerate the plot with the EXISTING script: `python <scratchpad>/plot_fitfix.py <old track.tsv> <new tsv> C:/atx-data/backtests/xom-strangle-varswap-2026-fitfix/cum_pnl_dsl.png` and diff the new track's cumulative sums against `C:/atx-data/backtests/xom-strangle-varswap-2026-fitfix/track.tsv` (python one-shot: max abs row delta on pnl_total + swap_pnl ≤ 1e-6 — the same run through the new path).
- [ ] **Step 3: Commit** — `git commit -m "feat(vol): reproduce the strangle-vs-varswap comparison from a declarative spec example"`

### Task 8: Delete the bespoke strategy; docs; final certification

**Files:**
- Delete: `atx-vol/include/atx/vol/strangle_varswap.hpp`, `atx-vol/src/strangle_varswap.cpp`, `atx-vol/examples/strangle_varswap_driver.cpp`, `atx-vol/tests/strangle_varswap_test.cpp`, `atx-vol/tests/strategy_restrike_parity_test.cpp`
- Modify: `atx-vol/CMakeLists.txt` (remove `src/strangle_varswap.cpp` :134, the driver target :539-540, both test registrations), `atx-vol/CHANGELOG.md` (new top entry), `atx-vol/README.md` (comparison-backtest section now points at the example + DSL)

- [ ] **Step 1: Port-audit** — read `tests/strangle_varswap_test.cpp` section headers; any behavior NOT already covered by `SwapLeg.*`/`StrategyRestrike*` (candidates: mirror desync on checkpoint restore, expiry-day fixing, n_obs arithmetic edge cases, signal NaN cases) gets ported into `swap_leg_test.cpp`/`strategy_test.cpp` FIRST (same TDD loop per ported test: red by breaking the assertion once, green).
- [ ] **Step 2: Delete** the five files + CMake entries; fix any straggler includes (`grep -r strangle_varswap atx-vol/`).
- [ ] **Step 3: CHANGELOG** — entry: REMOVED `StrangleVsVarswapStrategy` (bespoke) / ADDED declarative swap lane + `FixedExpiryRestrike` + `swap_leg` module; parity numbers from Task 6; migration note (old config → `equivalent_spec` shape). README: rewrite the strangle-varswap paragraph around the example.
- [ ] **Step 4: Final targeted certification** — `build\bin\atx_vol_tests.exe --gtest_filter=SwapLeg.*:StrategyRestrike*:Strategy.*:BacktestSwap*:SurfaceDbDispersionBacktest*` (the last suite proves the dispersion golden survived the interpreter edits). Expected: all pass.
- [ ] **Step 5: Commit** — `git commit -m "refactor(vol)!: retire the bespoke strangle-varswap strategy for the declarative DSL"`

---

## Self-review notes

- Spec coverage: §1 grammar → Task 3; lifecycle semantics → Task 4; swap semantics → Tasks 2+5; §2 module → Tasks 1-2; §3 interpreter/signals → Tasks 4-5; §4 parity → Task 6; §5 example → Task 7; §6 deletions → Task 8; error/perf/testing sections are global constraints. One spec deviation, recorded in Task 4: accessor named `unopened_entry_steps` (spec edit folded into that task's commit).
- `FixedQty` fair-strike path: interpreter overwrites qty after a `target_vega=1.0` solve (Task 5 wiring) — documented inline; `solve_cycle_swap`'s contract stays single-purpose.
- Type consistency: `swap_contract_for_lot`, `solve_cycle_swap(surface, CycleSwapRequest, double)`, `SwapSignalProbe::{capture_pre_step, refresh, append_swap_greek_signals, stepped}`, `select_fixed_cycle_expiry(span, int64, int64)`, accessors `skipped_restrikes/unopened_entry_steps/skipped_swap_cycles` — used identically across Tasks 1-8.
