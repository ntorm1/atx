# Sprint 5 — Book-Level Gates + Capacity-in-Optimizer + Full Robustness Battery + Synthetic Smoke

**Goal:** make the book-level northstar bars (turnover < 0.20/day, capacity > $100M) **measurable
and enforceable**, and produce the **first real (synthetic-panel) scorecard row**. Concretely: (i) a
cross-sleeve-netted **book-level** turnover rate, measured unconditionally and gated opt-in
(today only a *per-alpha* CPCV admission ceiling exists, `RunConfig::max_turnover`,
`config.hpp:67`); (ii) a participation-rate cap **inside** the optimizer's QP construction, not just
`stage_report.cpp`'s post-hoc %ADV curve; (iii) a config surface for the 3 unreachable
`eval::RobustnessBattery` checks (`sub_universe`/`alt_neutralization`/`param_perturbation` — the
struct fields already exist, tested, at `eval/robustness_battery.hpp:91-115`; only the admission-time
caller never threads them); (iv) a non-zero borrow/financing debit reaching the book P&L the
runnable pipeline actually computes; (v) a short synthetic-panel exercise of the whole S1–S5 lever
stack producing one honest, finite, deterministic scorecard row. Every lever is opt-in behind an
inert-default `RunConfig`/engine-config field; the no-flag path stays byte-identical.

**Owns (exclusive):**
`atx-impl/src/stage_optimize.cpp` (book-turnover measure+gate; participation-cap `ConstraintSet`/
`CapacityRef` construction), `atx-impl/src/stage_metabook.cpp` (book-turnover measure+gate on the
fund book), `atx-impl/src/book_shape.hpp` (NEW shared `book_turnover_per_day` helper — a neutral
house header both files already/newly include), `atx-engine/include/atx/engine/factory/factory.hpp`
+ `atx-engine/src/factory/factory.cpp` (the 3-check battery surface: `FactoryConfig` fields + both
`battery_cfg` construction sites + the `robustness_battery_passes` reeval extension),
`atx-engine/include/atx/engine/book/report.hpp` (borrow-debit field + parameter — see the S5-4
root-cause correction below), `atx-impl/src/stage_report.cpp` (thread `cfg.borrow_bps` into
`accumulate_report`), `atx-impl/src/config.{hpp,cpp}` (the 6 new `RunConfig` fields per the ROADMAP
registry + their CLI flags), `atx-impl/src/stage_discover.cpp` (minimal, additive: the 3 new battery
bools must join the *existing* `fcfg.robustness_battery = cfg.robustness_battery;` threading line —
see the S5-3 ownership note), a NEW synthetic-panel smoke test; tests under `atx-impl/tests/` and
`atx-engine/tests/{book,factory}/`.

**Must NOT touch:** `alpha/oracle.hpp` (untouchable every sprint); frozen estimation bodies in
`src/*/*.cpp`; `risk/{factor_model,dead_factor,shrinkage,eigen_adjust}.hpp` bodies (S1's territory —
S5 only *consumes* `RiskModelConfig`/the Factor path, never re-derives it); `stage_combine.cpp`
(S2/S3's file); `factory/fitness.{hpp,cpp}` and `factory/search_driver.*` (S4's objective-vector
work — S5 does not touch `kMaxObjectives`); `risk/garleanu_pedersen.*` (S3's file); `cost/borrow.hpp`
and `loop/backtest_loop.hpp` (the existing S4-3c "B5 fix" — already correct, already inert-wired for
the event-driven engine path; S5 does **not** edit either file — see the S5-4 note for why);
`eval/robustness_battery.hpp` itself (the `BatteryConfig`/`RobustnessScenario`/`RobustnessBattery`
types are already built and tested — S5 is a caller-side wiring sprint, not an estimator sprint);
`factory/param_search.hpp`/`factory/mutation.hpp` bodies (S5 *calls* `extract_free_constants` /
`instantiate`, it does not modify them).

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

Comments should be intelligent and sparse: explain why, invariants, ownership, ordering, or tricky
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
| Book turnover is measured *per period* but never as a comparable *per-day rate*, and never gated | `stage_optimize.cpp` MVO path builds `risk::MultiPeriodResult.turnover[s]`; `stage_metabook.cpp:590-593` sidecar prints `turnover_net`/`turnover_gross` per period; `run_metabook` sums them into `fund_turnover_net`/`fund_turnover_gross` kvs (`stage_metabook.cpp:604-616`) | Neither path normalizes by rebalance-day spacing (a weekly book's per-period number isn't comparable to a daily book's — the northstar bar is stated **per day**), and neither path can reject a run. The only existing turnover *ceiling* is `RunConfig::max_turnover` (`config.hpp:67`, "AlphaGate per-alpha turnover cap") — a **per-alpha admission** floor, not a book-level construction gate. |
| Participation cap machinery is fully built (S8.4) but never threaded at the runnable call site | `risk/constraints.hpp:133-137` (`ParticipationCap{adv_frac}`), `risk/reference_data.hpp:45-51` (`CapacityRef{adv,shares_out,price,nav,horizon_days}`), `risk/multi_period.hpp:104,106,162-168` (`MultiPeriodConfig::constraints`/`::ref`, already threaded into the inner `PortfolioOptimizer` every period) | `stage_optimize.cpp`'s `risk::MultiPeriodConfig mc;` construction (the `mc.single.*` block, ~line 287-296) never sets `mc.constraints`/`mc.ref` — `PortfolioOptimizer::solve` (`optimizer.hpp:141-163`) therefore always takes the `!constraints` fast path. The only reachable %ADV participation number is `stage_report.cpp`'s **post-hoc** capacity-footprint metric (`stage_report.cpp:522-530`, computed *after* the book is already sized) — never a binding constraint at construction time. |
| 3 of 4 robustness-battery checks have no config surface, though the checks themselves are built+tested | `eval/robustness_battery.hpp:91-115` (`BatteryConfig` — all 4 bools: `sub_universe`, `alt_neutralization`, `noise_control`, `param_perturbation`); `factory.hpp:236-247` (FactoryConfig's own doc: *"sub_universe/alt_neutralization need a liquidity-ADV / group_map input this admit site does not carry yet, and param_perturbation needs an AST-level numeric-param jitter that does not exist yet"*) | `factory.cpp:521-523` (inside `mine()`) and `factory.cpp:1208-1210` (inside `mine_into_oos`'s admit loop) both hardcode `battery_cfg.noise_control = true;` only. The SHARED `detail::robustness_battery_passes` reeval lambda (`factory.cpp:196-238`) handles only `ScenarioKind::NoiseControl` — every other kind hits the `Err("scenario not supported this wave")` branch (`factory.cpp:206-207`). `CandidateInputs.adv`/`.group_id` (`robustness_battery.hpp:153-154`) are never populated — only `.input_values` (from panel "close", `factory.cpp:182-190`). |
| Borrow/financing is built + already inert-wired, but unreachable from the runnable pipeline | `cost/borrow.hpp:84-87` (`BorrowModel{annual_rate, day_count}`), `:121-145` (`daily_borrow`/`accrue_borrow` — short-notional-only charge); `loop/backtest_loop.hpp:186-192` ("S4-3c: `borrow` (trailing, inert-default `cost::BorrowModel{}`)... accrues short-borrow financing once per bar") | **Root-cause correction vs. the ROADMAP's assumed site:** `grep -rn "BacktestLoop{" atx-impl/src` returns **zero hits** — the runnable CLI pipeline never constructs the event-driven `BacktestLoop`/`Market`/`Portfolio` machinery at all. The book P&L the pipeline actually computes is the vectorized `atx::engine::book::accumulate_report` (`book/report.hpp:238-309`, called from `stage_report.cpp:381-383`), whose `detail::accumulate_period` (`report.hpp:143-160`) charges `pnl_cost` from `cost_bps` only — **no borrow term exists on this path at all**, wired or not. `loop/backtest_loop.hpp`'s own borrow wiring is correct and already inert-safe; it is simply off the reachable graph. |

---

## Architecture note — what "book-level gates + capacity-in-optimizer" actually means

None of S5's five levers require new estimator math; every one is a *caller-side threading* problem
against machinery p8/S1–S4 already built:

1. **Book turnover.** `risk::MultiPeriodOptimizer::run` (`multi_period.hpp:131-183`) already returns a
   `turnover[s]` series (L1 move from the prior realized book) for both the single-blend book
   (`stage_optimize.cpp`) and, netted across sleeves, the fund book (`stage_metabook.cpp`'s
   `result.report.turnover_net`, produced by the frozen `fund::MetaBook::run`). S5-1 adds ONE shared,
   pure reduction — `book_turnover_per_day` — that turns either series into a day-normalized rate, and
   ONE opt-in fail-closed check (mirroring the already-landed `blocking_pbo` escalation pattern:
   `factory.hpp:222-235`) at both call sites. **Measure before gate** (design-spec risk mitigation,
   §6): the rate is *always* computed and surfaced as a kv; only the reject is opt-in.
2. **Participation cap.** The S8.4 augmented-QP dispatch (`PortfolioOptimizer::solve`,
   `optimizer.hpp:141-163`) already routes to `solve_augmented`/`ConstrainedQpSolver` whenever a
   non-minimal `ConstraintSet` is attached, and `MultiPeriodOptimizer` already forwards
   `cfg.constraints`/`cfg.ref` into the inner optimizer every period (`multi_period.hpp:165-168`).
   S5-2's entire job is: build a `CapacityRef` from the research panel's "volume"/"close" fields (the
   same 20-day trailing-ADV convention `stage_report.cpp:522-530` documents), build a `ConstraintSet`
   that carries the cap **plus** the existing gross/name-cap settings forward (the augmented path does
   **not** fall back to `cfg.single` — attaching `.part` alone would silently drop gross-leverage and
   the name cap unless `.gross`/`.pos` are populated too; this is the one correctness trap in this
   unit), and thread both into `mc` only when `cfg.participation_cap > 0.0`.
3. **Battery surface.** `eval::BatteryConfig`/`RobustnessScenario`/`RobustnessBattery::run` are
   complete (`eval/robustness_battery.hpp`). The gap is entirely in
   `factory::detail::robustness_battery_passes`'s reeval lambda, which only builds a `SubUniverse`,
   `AltNeutralization`, or `ParamPerturbation` scenario's *supporting Panel/Genome*, never scores it.
   S5-3 extends that ONE lambda (shared by both call sites) plus populates the two currently-empty
   `CandidateInputs` fields (`adv` from "volume"; `group_id` from a documented liquidity-quantile
   proxy — see S5-3). `param_perturbation` reuses `factory::extract_free_constants`/`instantiate`
   (`param_search.hpp:92,128-129`) — building blocks that already exist; only the caller-side wiring
   ("does not exist yet" per `factory.hpp:241`) is new.
4. **Borrow.** S5-4 does **not** touch `cost/borrow.hpp`/`loop/backtest_loop.hpp` (see the gap table's
   root-cause correction) — it adds the *same economic concept* (a short-notional financing debit,
   expressed in the bps-per-period convention `cost_bps` already uses, since a realized book weight IS
   already a fraction-of-NAV — no `Market`/`Portfolio` objects are needed to compute it) directly in
   `book::accumulate_report`, the accumulator the runnable pipeline actually calls.
5. **Synthetic smoke.** `write_panel`/`read_panel` (`serialize_panel.hpp`) plus direct
   `alpha::Panel::create(...)` construction is the established pattern for a self-contained stage test
   (`stage_optimize_riskmodel_test.cpp:46-89`) — no zip/CSV fixture needed. `run_all` itself hard-requires
   `--zip`/`--out` (it always runs `run_load` then `run_panel` first, `stage_run.cpp:62-71`), so S5-5
   drives the *reachable* stage graph directly (`run_discover`→`run_combine`→`run_optimize`/
   `run_metabook`→`run_report`) against a synthetic `panel.bin`, exactly reproducing `run_all`'s stage
   sequence minus the two zip-only stages.

---

## Determinism contract (Sprint 5)

Every field below is opt-in with an inert 0.0/false default (ROADMAP §SHARED CONFIG-FIELD REGISTRY,
S5 row):

- `RunConfig::book_turnover_gate : f64 = 0.0` — 0 ⇒ the rate is measured and surfaced but never
  rejects.
- `RunConfig::participation_cap : f64 = 0.0` — 0 ⇒ `mc.constraints` stays `std::nullopt`; the fast
  path runs, byte-identical to today.
- `RunConfig::robustness_sub_universe / ::robustness_alt_neutralization / ::robustness_param_perturb
  : bool = false` — each requires **both** its own flag **and** `--robustness-battery` (the existing
  master switch); all off ⇒ `battery_cfg` is exactly today's noise-control-only construction.
- `RunConfig::borrow_bps : f64 = 0.0` — 0 ⇒ `pnl_borrow[s] == 0.0` exactly, every period.

At every inert default, `stage_optimize`/`stage_metabook`/`stage_report`'s books/report digests are
byte-identical to pre-S5. The pinned goldens (`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
`FactoryOos.MineIntoOffPathDigestUnchanged`, the `AtxImplDiscover` determinism slice,
`LibraryVerdict.AdmitKindEnumFrozenPrefix`) MUST stay unchanged with none of S5's flags asserted.

**Four test classes per opt-in field (mandatory):** (a) off-path byte-identity — element-wise
`std::bit_cast<std::uint64_t>` on the books/report digests; (b) on-path RED→GREEN — a constructed
fixture where the gate/cap/check provably bites; (c) twice-run — identical digest/kvs twice; (d)
seq==parallel where an admission/eval path is touched (S5-1/S5-2/S5-4 touch no parallel path — their
"(d)" class is discharged by citing the already-proven determinism of the code they call, exactly as
S1-2's own plan did for the per-fit-window Factor path; S5-3's admit loop genuinely has a serial vs.
parallel substrate — `mine_into` vs. `mine_into_oos_parallel` — so its "(d)" is a real test).

---

## Dependency / wiring map

```
book_shape.hpp: NEW book_turnover_per_day(turnover, sched_periods) -> f64   <- S5-1 (shared helper)
  consumed by:
    stage_optimize.cpp   MVO + position-mode branches -> RunConfig.book_turnover_gate
    stage_metabook.cpp   run_metabook (fund-netted turnover)                <- S5-1

risk::MultiPeriodConfig::constraints / ::ref (multi_period.hpp:104,106, ALREADY threaded
  into PortfolioOptimizer every period, multi_period.hpp:165-168)
  <- S5-2 builds risk::ConstraintSet{.gross,.pos,.part} + risk::CapacityRef from
     research's "volume"/"close", set behind RunConfig.participation_cap > 0.0

eval::BatteryConfig (robustness_battery.hpp, ALREADY has all 4 fields)
  <- S5-3 factory.cpp:521-523,1208-1210 read 3 new FactoryConfig bools (in addition to
     the existing robustness_battery-gated noise_control)
  <- S5-3 factory.cpp:196-238 robustness_battery_passes' reeval lambda extended:
       SubUniverse        -> re-mask alt_panel's universe to sc.keep_instruments
       AltNeutralization   -> re-label the panel's group-classified field to sc.alt_group_id
       ParamPerturbation   -> factory::extract_free_constants + factory::instantiate (param_search.hpp)
  <- RunConfig's 3 new bools thread through the EXISTING
     `fcfg.robustness_battery = cfg.robustness_battery;` line in stage_discover.cpp (additive
     sibling lines only — see the S5-3 ownership note)

book::accumulate_report (book/report.hpp:238-309)
  <- S5-4 new atx::f64 borrow_bps = 0.0 parameter; detail::accumulate_period (report.hpp:143-160)
     folds short_weight[s] * borrow_bps * 1e-4 into pnl_net
  <- stage_report.cpp:381-383 threads cfg.borrow_bps into the call

atx-impl/tests/stage_run_synthetic_smoke_test.cpp  <- S5-5 (NEW; test-only; no production code)
  drives run_discover -> run_combine -> run_optimize|run_metabook -> run_report directly
  against a write_panel-serialized synthetic Panel (mirrors stage_optimize_riskmodel_test.cpp's
  make_correlated_research pattern), all S1-S5 flags ON, then all OFF (companion byte-identity run)
```

---

## Tasks

### S5-0 — Open ledger + config plumbing (do first; every unit reads these fields)

**Goal:** create the sprint ledger marker; add the 6 new `RunConfig` fields (ROADMAP registry, exact
names) + their CLI flags. No behavior change — nothing reads them non-inertly yet.

**Wiring — `atx-impl/src/config.hpp`** (append after `incremental_panel`, line 384, before the
`set_flags` comment block — the established "append at struct END, aggregate-init order is
load-bearing" convention, `config.hpp:293-301`):

```cpp
// -- p9 S5: book-level gates / capacity-in-optimizer / full robustness battery / borrow --
// All fields inert-default; a run asserting none of them is byte-identical to pre-S5.
// --book-turnover-gate (S5-1): cross-sleeve-netted book turnover, expressed as a PER-DAY
// L1 rate (mean per-period turnover / average rebalance-day spacing). 0.0 (default) = the
// rate is measured and surfaced (stage_optimize/stage_metabook kvs: "book_turnover_per_day")
// but never rejects. Active when > 0.0: a measured rate exceeding this threshold fails the
// stage CLOSED (mirrors --blocking-pbo's escalation; the books/sidecar are already written
// by the time the check runs, so a rejected run's own diagnostics stay inspectable).
double book_turnover_gate = 0.0;
// --participation-cap (S5-2): ADV participation fraction rho bounding |w_i| INSIDE the
// optimizer's QP construction (risk::ParticipationCap.adv_frac), not just the post-hoc
// stage_report.cpp capacity curve. 0.0 (default) = mc.constraints stays unset -> the fast
// (non-augmented) PortfolioOptimizer path runs, byte-identical to today.
double participation_cap = 0.0;
// --borrow-bps (S5-4): flat per-period financing charge (bps) on the book's SHORT weight
// (mirrors --cost-bps's own bps-per-period convention). 0.0 (default) = pnl_borrow is
// exactly 0.0 every period -> report digest byte-identical to today.
double borrow_bps = 0.0;
// --robustness-sub-universe / --robustness-alt-neutralization / --robustness-param-perturb
// (S5-3): expose the 3 currently-unreachable eval::BatteryConfig checks (noise_control is
// already wired via --robustness-battery alone, p8 final-wave). Each requires BOTH its own
// flag AND --robustness-battery; all false (default) = battery_cfg construction is exactly
// today's noise-control-only shape -> byte-identical admitted set/digest.
bool robustness_sub_universe = false;
bool robustness_alt_neutralization = false;
bool robustness_param_perturb = false;
```

**Wiring — `atx-impl/src/config.cpp`:**
- Boolean block (mirror the existing pattern at `config.cpp:44-52`, appended alongside the
  p8-final-wave `robustness-battery` line):
  ```cpp
  if (flag == "robustness-sub-universe")      { cfg.robustness_sub_universe      = true; return atx::core::Ok(); } // S5-0 (S5-3)
  if (flag == "robustness-alt-neutralization") { cfg.robustness_alt_neutralization = true; return atx::core::Ok(); } // S5-0 (S5-3)
  if (flag == "robustness-param-perturb")      { cfg.robustness_param_perturb      = true; return atx::core::Ok(); } // S5-0 (S5-3)
  ```
- Double-flag block (mirror the `cost-bps` non-negative guard at `config.cpp:308-315`):
  ```cpp
  if (flag == "book-turnover-gate") {
      ATX_TRY_VOID(parse_double(cfg.book_turnover_gate));
      if (cfg.book_turnover_gate < 0.0) {
          return atx::core::Err(EC::InvalidArgument, "--book-turnover-gate must be >= 0: got " + std::string(value));
      }
      return atx::core::Ok();
  }
  if (flag == "participation-cap") {
      ATX_TRY_VOID(parse_double(cfg.participation_cap));
      if (cfg.participation_cap < 0.0) {
          return atx::core::Err(EC::InvalidArgument, "--participation-cap must be >= 0: got " + std::string(value));
      }
      return atx::core::Ok();
  }
  if (flag == "borrow-bps") {
      ATX_TRY_VOID(parse_double(cfg.borrow_bps));
      if (cfg.borrow_bps < 0.0) {
          return atx::core::Err(EC::InvalidArgument, "--borrow-bps must be >= 0: got " + std::string(value));
      }
      return atx::core::Ok();
  }
  ```

**Determinism:** pure addition; every existing `apply_flag`/`apply_flag_value` arm untouched;
`RunConfig{}` default-constructs to the inert values.

**Accept:**
- `config_s5_book_gates_test.cpp` (NEW, `atx-impl/tests/`, mirrors `config_megabook_flags_test.cpp`'s
  shape): `RunConfig{}.book_turnover_gate/.participation_cap/.borrow_bps == 0.0` and the 3 new bools
  `== false`; each of the 6 flags parses via `parse_args`, is recorded in `cfg.set_flags` under its
  canonical dashed key, and a negative value on the 3 double flags returns `Err(InvalidArgument)`.
- Existing config-parsing tests unaffected; `atx-impl-tests` links and passes.
- **Wrapper:**
  ```powershell
  cmake --build --preset dev --target atx-impl-tests
  ctest --preset dev -R ConfigS5BookGates
  ```
  Expected: RED (target doesn't exist / fields undefined) before the edit; GREEN after.
- **Commit:** `atx-impl/src/config.hpp`, `atx-impl/src/config.cpp`,
  `atx-impl/tests/config_s5_book_gates_test.cpp`, `atx-engine/plans/p9/sprint-5-progress.md` (open
  ledger marker).
  ```
  docs(p9-s5): S5-0 open ledger + RunConfig book-gate/capacity/battery/borrow fields

  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  ```

---

### S5-1 — Book-level cross-sleeve-netted turnover: measure, then opt-in gate

**Goal:** a single shared helper that turns EITHER book's per-period turnover series into a
comparable **per-day** rate; surface it unconditionally; reject a run that opts into the gate and
breaches it.

**Root cause:** `stage_optimize.cpp`'s `MultiPeriodResult.turnover[s]` and `stage_metabook.cpp`'s
`result.report.turnover_net[s]` (already cross-sleeve-netted by the frozen `fund::MetaBook::run`,
S2-3) are both PER-PERIOD numbers. A weekly-rebalance book's per-period turnover isn't directly
comparable to a daily-rebalance book's, so the northstar bar ("turnover < 0.20/day") cannot be read
off either series today, and nothing rejects a breach — the only ceiling is the per-alpha
`RunConfig::max_turnover` AlphaGate floor (`config.hpp:67`), which gates individual alpha admission,
never the assembled book.

**Wiring — NEW helper in `atx-impl/src/book_shape.hpp`** (already included by `stage_optimize.cpp`;
add the include to `stage_metabook.cpp` too — a neutral, ownership-free house header, not another
sprint's exclusive file):

```cpp
// Convert a per-rebalance-period L1 turnover series into a book-level DAILY rate: mean
// per-period turnover divided by the average trading-day spacing between rebalances. A
// schedule with < 2 periods (or non-advancing periods) has no spacing to divide by -- reports
// the raw per-period mean instead (an honest degenerate: there is no "day" yet to normalize
// against). This is the SAME normalization stage_report.cpp's ann/step-spacing logic already
// uses for annualizing Sharpe (mirrors that convention rather than inventing a new one).
[[nodiscard]] inline atx::f64
book_turnover_per_day(std::span<const atx::f64> turnover, std::span<const atx::usize> sched_periods) {
    if (turnover.empty()) return 0.0;
    atx::f64 sum = 0.0;
    for (atx::f64 t : turnover) sum += t;
    const atx::f64 mean = sum / static_cast<atx::f64>(turnover.size());
    if (sched_periods.size() < 2 || sched_periods.back() <= sched_periods.front()) return mean;
    const atx::f64 span_days = static_cast<atx::f64>(sched_periods.back() - sched_periods.front());
    const atx::f64 step_days = span_days / static_cast<atx::f64>(sched_periods.size() - 1);
    return mean / step_days;
}
```

**Wiring — `stage_optimize.cpp`** (both the MVO branch, after `mpo.run`, and the position-mode
branch, after the per-period `turnover` vector is filled — same helper, same guard):

```cpp
// S5-1: measure FIRST (always -- the design-spec's "measure before gate" mitigation), gate
// opt-in second. Computed AFTER write_books so a rejected run's own books/sidecar stay on
// disk and inspectable (mirrors --blocking-pbo's documented "already persisted" caveat).
ATX_TRY(auto sr, write_books(flat, result.turnover, result.cost_bps));
const atx::f64 book_turnover = book_turnover_per_day(result.turnover, sched.periods);
sr.kvs.emplace_back("book_turnover_per_day", std::to_string(book_turnover));
if (cfg.book_turnover_gate > 0.0 && book_turnover > cfg.book_turnover_gate) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "optimize: book turnover " + std::to_string(book_turnover) +
                              "/day exceeds --book-turnover-gate " +
                              std::to_string(cfg.book_turnover_gate));
}
return atx::core::Ok(std::move(sr));
```

(Position-mode branch: identical shape, reusing its own already-computed `turnover` vector and
`sched.periods`.)

**Wiring — `stage_metabook.cpp`'s `run_metabook`** (after the existing `sr.kvs` assembly, using
`result.report.turnover_net` — the S2-3 cross-sleeve-netted series — and `sched.periods` already in
scope):

```cpp
const atx::f64 book_turnover = book_turnover_per_day(result.report.turnover_net, sched.periods);
sr.kvs.emplace_back("book_turnover_per_day", std::to_string(book_turnover));
if (cfg.book_turnover_gate > 0.0 && book_turnover > cfg.book_turnover_gate) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "metabook: fund book turnover " + std::to_string(book_turnover) +
                              "/day exceeds --book-turnover-gate " +
                              std::to_string(cfg.book_turnover_gate));
}
```

**Determinism:** `book_turnover_gate == 0.0` ⇒ the `if` short-circuits false ⇒ never evaluated as a
reject; the ADDED kv is a pure-function string of an already-computed number, appended after
`sr.digest` is set — it changes `sr.kvs`'s SIZE/content but never `sr.digest` (the books panel bytes),
and `run_all`'s own outer kvs vector (the 6 `{stage_name, hex(digest)}` pairs asserted by
`MegaBookGraph_InertByteIdentical`) never touches a stage's *internal* kvs — so no pinned golden
observes this addition.

**Accept:**
- `stage_optimize_book_turnover_test.cpp` (NEW): (a) `BookTurnoverGate_OffPathByteIdentical` —
  `cfg.book_turnover_gate == 0.0` (default) → identical `books.bin` digest to a pre-S5-1 baseline run
  on a fixed fixture. (b) `BookTurnoverGate_RedGreen` — two fixtures: a near-flat-price panel (book
  barely moves rebalance-to-rebalance ⇒ low turnover) and a violently-alternating-sign panel (the
  combined alpha flips sign every rebalance ⇒ near-maximal turnover). With
  `cfg.book_turnover_gate` set between the two measured rates: the low-turnover fixture's
  `run_optimize` returns `Ok` and its `book_turnover_per_day` kv is `< book_turnover_gate`; the
  high-turnover fixture returns `Err`. (c) `BookTurnoverGate_TwiceRun` — same cfg+panel twice ⇒
  identical `book_turnover_per_day` kv string and identical accept/reject outcome. (d) documented N/A
  — `book_turnover_per_day` is a pure, order-fixed reduction over an already-proven-deterministic
  `MultiPeriodResult.turnover` (no parallel_for touches this stage); the comment in the test file
  states this explicitly rather than silently omitting class (d).
- `stage_metabook_book_turnover_test.cpp` (NEW): the same 3 tests (a/b/c) against `run_metabook`'s
  fund-netted series, using a 2-sleeve fixture (so the netted number is genuinely a cross-sleeve
  measurement, not a trivial single-sleeve passthrough).
- **Wrapper:**
  ```powershell
  cmake --build --preset dev --target atx-impl-tests
  ctest --preset dev -R "StageOptimizeBookTurnover|StageMetabookBookTurnover"
  ```
  Expected: RED (helper/gate absent) before the edit; GREEN after; every pre-existing
  `AtxImplOptimize*`/`AtxImplMetabook*` suite unchanged.
- **Commit:** `atx-impl/src/book_shape.hpp`, `atx-impl/src/stage_optimize.cpp`,
  `atx-impl/src/stage_metabook.cpp`, `atx-impl/tests/stage_optimize_book_turnover_test.cpp`,
  `atx-impl/tests/stage_metabook_book_turnover_test.cpp`.
  ```
  feat(p9-s5): S5-1 book-level netted turnover rate + opt-in gate

  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  ```

---

### S5-2 — Participation-rate cap as a QP constraint (capacity inside construction)

**Goal:** thread a real `%ADV` participation bound into the optimizer's construction-time QP, not
just the post-hoc report curve, reusing the already-built-and-tested S8.4 augmented-dispatch
machinery.

**Root cause:** `risk::ConstraintSet::part` (`ParticipationCap`, `constraints.hpp:133-137,244`) and
`risk::CapacityRef` (`reference_data.hpp:45-51`) are fully built; `risk::MultiPeriodConfig` already
carries `constraints`/`ref` straight into the inner `PortfolioOptimizer` every period
(`multi_period.hpp:104,106,162-168`). `stage_optimize.cpp`'s `mc` construction (the `mc.single.*`
block only) never populates either field, so `PortfolioOptimizer::solve` (`optimizer.hpp:141-163`)
always takes the fast (non-augmented) path — participation is only ever checked *after* the book is
already sized, in `stage_report.cpp`'s 20-day-trailing-ADV capacity-footprint metric
(`stage_report.cpp:522-530`).

**The one correctness trap:** `is_minimal_constraint_set` (`optimizer.hpp:231-233`) returns `false`
the moment `.part` is populated (`part` is NOT one of the exempted fields), so attaching a
`ParticipationCap` alone routes to `solve_augmented`, which materializes `ConstraintSet::gross`/
`::pos` **independently of `cfg`** (`optimizer.hpp:245-267`) — it does **not** fall back to
`mc.single.gross_leverage`/`.name_cap`. The `ConstraintSet` built here MUST carry the existing
gross/dollar-neutral/name-cap settings forward explicitly, or they silently vanish once the
augmented path activates.

**Wiring — `stage_optimize.cpp`** (MVO branch, before `MultiPeriodOptimizer mpo; mpo.cfg = mc;`):

```cpp
// S5-2: participation-rate cap (inert unless --participation-cap > 0). Building a
// CapacityRef needs a per-instrument ADV + a current mark; reuse the SAME 20-day
// trailing dollar-ADV convention stage_report.cpp's capacity-footprint metric documents
// (stage_report.cpp:522-530), anchored at the panel's LAST date (research.dates()-1) --
// the optimizer has one whole-panel model on the Diagonal path and a per-step model on
// the Factor path, but the participation reference panel itself is a single as-of-latest
// snapshot either way (mirrors stage_combine.cpp's own dollar_adv anchor).
if (cfg.participation_cap > 0.0) {
    ATX_TRY(const auto vol_fid, research.field_id("volume"));
    ATX_TRY(const auto cls_fid, research.field_id("close"));
    constexpr atx::usize kAdvWindow = 20;
    const atx::usize last = D - 1;
    const atx::usize win_begin = (last + 1 > kAdvWindow) ? (last + 1 - kAdvWindow) : 0;
    std::vector<atx::f64> adv(M, 0.0);
    std::vector<atx::f64> price(M, 0.0);
    for (atx::usize i = 0; i < M; ++i) {
        atx::f64 sum = 0.0;
        atx::usize n = 0;
        for (atx::usize t = win_begin; t <= last; ++t) {
            const atx::f64 v = research.field_all(vol_fid)[t * M + i];
            if (!std::isnan(v)) { sum += v; ++n; }
        }
        adv[i] = (n > 0) ? sum / static_cast<atx::f64>(n) : 0.0;
        price[i] = research.field_all(cls_fid)[last * M + i];
    }
    risk::ConstraintSet cs;
    cs.gross.gross_leverage = gross_val;
    cs.gross.dollar_neutral = true;
    cs.pos = risk::PositionCap{name_cap_val};
    cs.part = risk::ParticipationCap{cfg.participation_cap};
    mc.constraints = std::move(cs);
    mc.ref.adv = adv;          // NOTE: CapacityRef spans are BORROWED -- adv/price must
    mc.ref.price = price;      // outlive mpo.run(...) below (they do; same scope).
    mc.ref.nav = cfg.report_aum > 0.0 ? cfg.report_aum : 1e9; // reuse the existing AUM
                                                               // field stage_report.cpp
                                                               // already assumes for
                                                               // capacity metrics, so
                                                               // construction-time and
                                                               // post-hoc capacity share
                                                               // ONE dollar scale.
    mc.ref.horizon_days = 1.0; // conservative 1-day participation horizon
}
```

**Determinism (inert default):** `participation_cap == 0.0` ⇒ `mc.constraints` stays `std::nullopt`
⇒ `PortfolioOptimizer::solve`'s `if (constraints && ...)` guard is false ⇒ `solve_fast` runs exactly
as today — the existing S8.4 fast-path pin already covers this branch; S5-2 does not re-prove it, only
that the gate correctly keys off `cfg.participation_cap`.

**Accept:**
- `stage_optimize_participation_cap_test.cpp` (NEW): (a) `ParticipationCap_OffPathByteIdentical` —
  `participation_cap == 0.0` → identical books digest to pre-S5-2. (b)
  `ParticipationCap_BoundsThinAdvName` — a fixture with one THIN-ADV instrument (tiny "volume") and
  the rest liquid, plus an alpha tilt concentrated on the thin name. WITHOUT the cap the thin name's
  realized weight sizes up toward `name_cap`; WITH `--participation-cap` set tight, the SAME name's
  realized weight is (i) strictly lower than the uncapped run's and (ii) at or under the closed-form
  bound `cfg.participation_cap * 1.0 * adv_i * price_i / mc.ref.nav`. (c)
  `ParticipationCap_TwiceRunByteIdentical` — identical augmented-path books digest across two runs.
  (d) documented N/A with citation — the per-period augmented solve inherits `ConstrainedQpSolver`'s
  own proven ADMM determinism (no thread/time, fixed iteration count); no parallel entry point exists
  for `run_optimize` today.
- **Wrapper:**
  ```powershell
  cmake --build --preset dev --target atx-impl-tests
  ctest --preset dev -R StageOptimizeParticipationCap
  ```
  Expected: RED before the edit (flag parses but never reaches `mc`); GREEN after.
- **Commit:** `atx-impl/src/stage_optimize.cpp`,
  `atx-impl/tests/stage_optimize_participation_cap_test.cpp`.
  ```
  feat(p9-s5): S5-2 participation-rate cap inside the optimizer QP construction

  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  ```

---

### S5-3 — Expose the 3 unreachable robustness-battery checks

**Goal:** `--robustness-battery` + the 3 new sub-flags make `sub_universe`/`alt_neutralization`/
`param_perturbation` genuinely reachable and rejecting, not just config-parseable.

**Root cause:** `eval::BatteryConfig` (`robustness_battery.hpp:91-115`) already has all 4 toggles,
built and tested (`atx-engine/tests/eval/robustness_battery_test.cpp`,
`atx-engine/tests/factory/robustness_battery_wire_test.cpp`). The gap is entirely on the caller side:
`factory.cpp:521-523` (inside `mine()`) and `factory.cpp:1208-1210` (inside `mine_into_oos`'s admit
loop) both hardcode only `battery_cfg.noise_control = true;`. Both sites call the SAME shared
`detail::robustness_battery_passes` (`factory.cpp:172-242`), whose `reeval` lambda's `if` guard
(`factory.cpp:198`) accepts only `ScenarioKind::NoiseControl` — every other kind returns
`Err("scenario not supported this wave")`. `CandidateInputs.adv`/`.group_id` are never populated.

**Wiring — `atx-engine/include/atx/engine/factory/factory.hpp`** (append to `FactoryConfig`, after
`robustness_battery`):

```cpp
// --- p9 S5-3: expose the 3 checks robustness_battery's own doc (above) deferred. ---
// Each requires robustness_battery == true to have any effect (mirrors the "opt-in inside
// opt-in" shape blocking_pbo already uses against max_pbo). All false (default) preserves
// the exact noise-control-only BatteryConfig this wave shipped.
bool robustness_sub_universe = false;
bool robustness_alt_neutralization = false;
bool robustness_param_perturb = false;
```

**Wiring — `factory.cpp`, both `battery_cfg` construction sites** (identical 3-line addition at
`:521-523` and `:1208-1210`):

```cpp
if (cfg.robustness_battery) {
  battery_cfg.noise_control = true;
  battery_cfg.sub_universe = cfg.robustness_sub_universe;
  battery_cfg.alt_neutralization = cfg.robustness_alt_neutralization;
  battery_cfg.param_perturbation = cfg.robustness_param_perturb;
  battery_cfg.seed = res.seed ^ 0x526F627573742121ULL; // (run_seed at the mine_into_oos site)
}
```

**Wiring — `robustness_battery_passes`'s reeval lambda** (`factory.cpp:172-242`): populate the two
empty `CandidateInputs` fields, then extend the `if`/switch to score all 4 kinds.

```cpp
// ADV proxy (sub_universe ranking): panel "volume", empty when the field is absent ->
// sub_universe marks itself inapplicable (the header's documented graceful degrade).
std::vector<atx::f64> adv_col;
const auto vol_id = panel.field_id("volume");
if (vol_id.has_value()) {
  const auto vol_all = panel.field_all(*vol_id);
  adv_col.assign(vol_all.begin(), vol_all.end());
  inputs.adv = std::span<const atx::f64>{adv_col};
}
// Neutralization group proxy (alt_neutralization): this admit site carries no true
// industry/GICS group_map (factory.hpp's own doc flags this gap). Documented, honest
// stand-in: a deterministic liquidity-QUANTILE bucket over the SAME adv_col (kNBuckets
// buckets, ascending instrument order) -- NOT a fabricated industry taxonomy, a coarse but
// real, order-fixed grouping. Empty (and the check gracefully inapplicable) when volume is
// absent, same as adv.
std::vector<atx::u32> group_col;
if (vol_id.has_value()) {
  constexpr atx::usize kNBuckets = 5;
  std::vector<atx::usize> order(insts);
  std::iota(order.begin(), order.end(), atx::usize{0});
  std::stable_sort(order.begin(), order.end(),
                   [&](atx::usize a, atx::usize b) { return adv_col[a] < adv_col[b]; });
  group_col.assign(insts, 0U);
  for (atx::usize r = 0; r < insts; ++r) {
    const atx::usize bucket = (r * kNBuckets) / std::max<atx::usize>(insts, 1);
    group_col[order[r]] = static_cast<atx::u32>(std::min(bucket, kNBuckets - 1));
  }
  inputs.group_id = std::span<const atx::u32>{group_col};
}
```

```cpp
const eval::Reevaluator reeval =
    [&](const eval::RobustnessScenario &sc) -> atx::core::Result<atx::f64> {
  if (sc.kind == eval::ScenarioKind::ParamPerturbation) {
    // S5-3: the "AST-level numeric-param jitter" factory.hpp's doc says doesn't exist yet
    // is really just an un-wired CALLER over existing building blocks: extract_free_constants
    // gives the candidate's free Window/Scale/Hparam literals (param_search.hpp:92); each
    // dim's CURRENT value is the Ast's Literal payload (Expr::value, parser.hpp:74,86, read
    // via Ast::node(id), parser.hpp:135); instantiate rebuilds the genome at a jittered point
    // (param_search.hpp:128-129). A 0-dim space (no free constant) has nothing to jitter --
    // every draw reproduces the SAME candidate, so the resulting edge is constant across
    // draws (CV == 0, an honest "no knife-edge risk because there is no free parameter"
    // pass, not a special case to detect).
    const factory::ParamSpace space = factory::extract_free_constants(cand);
    std::vector<atx::f64> x(space.dims());
    for (atx::usize k = 0; k < space.dims(); ++k) {
      x[k] = cand.ast.node(space.dim[k].id).value * sc.param_scale;
    }
    ATX_TRY(factory::Genome jittered, factory::instantiate(cand, space, x));
    ATX_TRY(const FitnessReport fit, pool_aware_fitness(jittered, pool, panel_, policy_, sim_, admit_fit));
    return atx::core::Ok(fit.dsr);
  }
  if (sc.kind == eval::ScenarioKind::SubUniverse) {
    std::vector<std::string> fields;
    std::vector<std::vector<atx::f64>> cols;
    fields.reserve(n_fields);
    cols.reserve(n_fields);
    for (atx::usize f = 0; f < n_fields; ++f) {
      fields.emplace_back(panel.field_name(f));
      const auto col = panel.field_all(static_cast<alpha::FieldId>(f));
      cols.emplace_back(col.begin(), col.end());
    }
    std::vector<std::uint8_t> keep(insts, 0U);
    for (const atx::usize i : sc.keep_instruments) { if (i < insts) keep[i] = 1U; }
    std::vector<std::uint8_t> universe(dates * insts);
    for (atx::usize t = 0; t < dates; ++t)
      for (atx::usize i = 0; i < insts; ++i)
        universe[t * insts + i] =
            (panel.in_universe(static_cast<alpha::DateIdx>(t), i) && keep[i]) ? 1U : 0U;
    ATX_TRY(alpha::Panel alt_panel,
           alpha::Panel::create(dates, insts, std::move(fields), std::move(cols), std::move(universe)));
    ATX_TRY(const FitnessReport fit, pool_aware_fitness(cand, pool, alt_panel, policy_, sim_, admit_fit));
    return atx::core::Ok(fit.dsr);
  }
  if (sc.kind == eval::ScenarioKind::AltNeutralization && vol_id.has_value()) {
    // Re-label the SAME liquidity-bucket field CandidateInputs.group_id was built from with
    // the battery's seeded permutation, injected as a synthetic panel field a
    // group-neutralizing wrapper op (mutation.hpp's wrap_group_neutralize/wrap_indneutralize)
    // can read. A candidate whose DSL never groups by it is UNAFFECTED (pool_aware_fitness
    // returns the same edge) -- correctly reported as "not group-dependent", not a defect.
    std::vector<std::string> fields;
    std::vector<std::vector<atx::f64>> cols;
    fields.reserve(n_fields + 1);
    cols.reserve(n_fields + 1);
    for (atx::usize f = 0; f < n_fields; ++f) {
      fields.emplace_back(panel.field_name(f));
      const auto col = panel.field_all(static_cast<alpha::FieldId>(f));
      cols.emplace_back(col.begin(), col.end());
    }
    fields.emplace_back("__s5_group");
    std::vector<atx::f64> grp_col(dates * insts, 0.0);
    for (atx::usize t = 0; t < dates; ++t)
      for (atx::usize i = 0; i < insts; ++i)
        grp_col[t * insts + i] = (i < sc.alt_group_id.size()) ? static_cast<atx::f64>(sc.alt_group_id[i]) : 0.0;
    cols.push_back(std::move(grp_col));
    std::vector<std::uint8_t> universe(dates * insts);
    for (atx::usize t = 0; t < dates; ++t)
      for (atx::usize i = 0; i < insts; ++i)
        universe[t * insts + i] = panel.in_universe(static_cast<alpha::DateIdx>(t), i) ? 1U : 0U;
    ATX_TRY(alpha::Panel alt_panel,
           alpha::Panel::create(dates, insts, std::move(fields), std::move(cols), std::move(universe)));
    ATX_TRY(const FitnessReport fit, pool_aware_fitness(cand, pool, alt_panel, policy_, sim_, admit_fit));
    return atx::core::Ok(fit.dsr);
  }
  if (sc.kind != eval::ScenarioKind::NoiseControl || !close_id.has_value()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "robustness_battery_passes: scenario not supported this wave");
  }
  /* ... existing NoiseControl body, unchanged ... */
};
```

**Ownership note (deviation from the ROADMAP's literal S5 root list, flagged honestly):** the
`RunConfig` → `FactoryConfig` threading for `robustness_battery` already lives in
`stage_discover.cpp` (`fcfg.robustness_battery = cfg.robustness_battery;`), a file the ROADMAP's S5
row does not list. Leaving the 3 new bools unthreaded there would parse a flag that never reaches
`FactoryConfig` — a silent no-op, explicitly forbidden by the ROADMAP's own anti-roadmap guardrail
("Fail-loud, never silent no-op"). S5-3 makes the **minimal, additive** touch: 3 sibling lines next to
the existing threading line, not a new claim over the file.

**Accept:**
- `atx-engine/tests/factory/robustness_battery_full_wire_test.cpp` (NEW): (a)
  `AllThreeOffPathByteIdentical` — the 3 new bools false (only `robustness_battery` true) →
  identical admitted set/digest vs. pre-S5-3. (b) per-check RED→GREEN: `SubUniverseRejectsIlliquidEdge`
  (one high-ADV name carries the whole edge; restricting to top-N-by-ADV excluding it collapses the
  edge ⇒ rejected; a broad-edge candidate survives ⇒ admitted); `AltNeutralizationRejectsGroupTilt`
  (a candidate whose signal is a pure liquidity-bucket step function collapses under ANY bucket
  permutation ⇒ rejected; an idiosyncratic candidate survives); `ParamPerturbationRejectsKnifeEdge`
  (a fixture where fitness is a narrow spike at one literal value; a jitter band crossing the spike
  swings the edge ⇒ high CV ⇒ rejected; a stable candidate's CV stays low ⇒ admitted). (c)
  `AllThreeTwiceRunByteIdentical`. (d) `SerialParallelAgreeWithFullBattery` — `mine_into` (serial) vs.
  `mine_into_oos_parallel` (the proven bit-identical `ProcessExecutor` substrate) admit the identical
  set/digest with all 3 checks on (each check's RNG derives from `cfg.seed`-salted independent
  streams per the header's own doc — never thread/time).
- **Wrapper:**
  ```powershell
  cmake --build --preset dev --target atx-engine-tests
  ctest --preset dev -R RobustnessBatteryFullWire
  ```
  Expected: RED (the 3 checks always hit `Err("scenario not supported this wave")`, so the RED-GREEN
  fixtures reject universally regardless of construction) before the edit; GREEN after.
- **Commit:** `atx-engine/include/atx/engine/factory/factory.hpp`,
  `atx-engine/src/factory/factory.cpp`, `atx-impl/src/config.hpp`, `atx-impl/src/config.cpp`,
  `atx-impl/src/stage_discover.cpp`,
  `atx-engine/tests/factory/robustness_battery_full_wire_test.cpp`.
  ```
  feat(p9-s5): S5-3 expose sub_universe/alt_neutralization/param_perturbation battery checks

  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  ```

---

### S5-4 — Non-zero borrow financing reaching the book P&L

**Goal:** `--borrow-bps > 0` debits the book's realized net P&L by a short-notional financing charge.

**Root cause (corrected from the ROADMAP's assumed root):** `cost::BorrowModel`/`daily_borrow`/
`accrue_borrow` (`cost/borrow.hpp:84-145`) and `loop::BacktestLoop`'s trailing `borrow` parameter
(`loop/backtest_loop.hpp:186-192`, the "B5 fix") are correct and already inert-wired — but
`grep -rn "BacktestLoop{" atx-impl/src` returns zero hits: the runnable CLI pipeline never
constructs the event-driven engine at all. The book P&L it actually computes is
`atx::engine::book::accumulate_report` (`book/report.hpp:238-309`, called from
`stage_report.cpp:381-383`), whose `detail::accumulate_period` (`report.hpp:143-160`) has **no borrow
term on any path** — wired or not. S5-4 therefore does not touch `cost/borrow.hpp` or
`loop/backtest_loop.hpp`; it adds the same economic concept directly to the accumulator the pipeline
reaches.

**Wiring — `atx-engine/include/atx/engine/book/report.hpp`:**

```cpp
struct PeriodAccum {
  atx::f64 pnl_gross;
  atx::f64 pnl_cost;
  atx::f64 pnl_borrow; // S5-4: short-notional financing debit, bps-per-period convention
  atx::f64 gross;
  atx::f64 net;
};

[[nodiscard]] inline PeriodAccum accumulate_period(const std::vector<atx::f64> &book,
                                                   std::span<const atx::f64> r,
                                                   const alpha::Panel &panel, atx::usize date,
                                                   atx::f64 cost_bps,
                                                   atx::f64 borrow_bps = 0.0) noexcept {
  atx::f64 pnl_gross = 0.0;
  atx::f64 gross = 0.0;
  atx::f64 net = 0.0;
  atx::f64 short_w = 0.0; // S5-4: sum of |w_i| over SHORT names only (weight-space notional)
  for (atx::usize i = 0; i < book.size(); ++i) {
    const atx::f64 w = book[i];
    gross += std::fabs(w);
    net += w;
    if (w < 0.0) short_w += -w;
    const bool live = i < r.size() && !std::isnan(r[i]) && panel.in_universe(date, i);
    if (live) pnl_gross += w * r[i];
  }
  return PeriodAccum{pnl_gross, cost_bps * 1e-4, short_w * borrow_bps * 1e-4, gross, net};
}
```

```cpp
struct BookReport {
  /* ... existing fields ... */
  std::vector<atx::f64> pnl_borrow; // S5-4: short-notional financing debit per period (bps -> fraction)
};
```

`accumulate_report` gains a defaulted trailing parameter (`atx::f64 borrow_bps = 0.0`) so the ONE
other existing caller (`book/pipeline.hpp:303`) stays byte-identical automatically; the loop body
folds `pnl_net = acc.pnl_gross - acc.pnl_cost - acc.pnl_borrow` and pushes `acc.pnl_borrow` into the
new `rep.pnl_borrow` vector.

**Wiring — `atx-impl/src/stage_report.cpp:381-383`:**

```cpp
ATX_TRY(auto rep,
        book::accumulate_report(mpr, retpanel, ret_fid, sched, V,
                                capacity_gross, libr, 0, cfg.borrow_bps));
```

**Determinism (inert default):** `borrow_bps == 0.0` ⇒ `short_w * 0.0 * 1e-4 == 0.0` exactly, every
period ⇒ `pnl_net`/`equity_curve`/`write_report`'s TSVs byte-identical to pre-S5-4.

**Accept:**
- `atx-engine/tests/book/report_borrow_test.cpp` (NEW): (a) `BorrowZeroByteIdentical` —
  `borrow_bps == 0.0` → `pnl_borrow` all exactly `0.0`; `pnl_net`/`equity_curve` identical to a
  pre-S5-4 baseline call. (b) `BorrowDebitsShortLeg` — a 2-period, 2-instrument fixture book
  `{+0.5, -0.5}` held both periods, `borrow_bps = 50.0`: `pnl_borrow[s] == 0.5 * 50.0 * 1e-4` exactly
  (closed-form), and `pnl_net` is strictly lower than the same fixture run at `borrow_bps = 0`. (c)
  `BorrowTwiceRunByteIdentical` — identical `pnl_borrow`/`pnl_net` series across two calls. (d) N/A —
  `accumulate_report` is a pure sequential reduction touching no parallel/admission path (documented
  in the test file rather than silently omitted).
- `stage_report_borrow_test.cpp` (NEW, `atx-impl/tests/`): threading proof — `cfg.borrow_bps` set on
  a `run_report` call produces a non-zero `pnl_borrow`/`total_pnl_cost`-adjacent summary figure;
  absent, the summary is byte-identical to pre-S5-4.
- **Wrapper:**
  ```powershell
  cmake --build --preset dev --target atx-engine-tests
  cmake --build --preset dev --target atx-impl-tests
  ctest --preset dev -R "ReportBorrow|StageReportBorrow"
  ```
  Expected: RED (no `pnl_borrow` field / no 8th `accumulate_report` parameter) before the edit; GREEN
  after.
- **Commit:** `atx-engine/include/atx/engine/book/report.hpp`, `atx-impl/src/stage_report.cpp`,
  `atx-engine/tests/book/report_borrow_test.cpp`, `atx-impl/tests/stage_report_borrow_test.cpp`.
  ```
  feat(p9-s5): S5-4 thread non-zero borrow financing into book::accumulate_report

  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  ```

---

### S5-5 — Synthetic-panel smoke: the first real (synthetic) scorecard row

**Goal:** exercise S1–S5's whole lever stack together on one short, deterministic, synthetic panel,
producing one finite, honest (labeled-synthetic) book-level scorecard row — no long real-panel sweep.

**Wiring — NEW `atx-impl/tests/stage_run_synthetic_smoke_test.cpp` only (no production code):**

1. Build a short synthetic `alpha::Panel` DIRECTLY (mirrors `stage_optimize_riskmodel_test.cpp`'s
   `make_correlated_research`, `:66-89`) — e.g. M=12 instruments × D=80 dates, a common-shock +
   idiosyncratic-noise structure (so the Factor risk model / dead-alpha-factor / battery checks have
   real, non-degenerate signal to bite on) plus a deliberately wide "volume" spread across instruments
   (so participation-cap and sub_universe have real thin/thick-ADV contrast). `write_panel(panel,
   tmp_path)`.
2. Drive the reachable stage graph directly — `run_all`'s own sequence minus its two zip-only stages
   (`stage_run.cpp:62-71` hard-requires `--zip`/`--out`): `run_discover(c_disc)` (gated=true,
   `library_dir` set, `--dead-alpha-factors`, `--risk-model factor`, `--group-neutralize`,
   `--robustness-battery` + all 3 new sub-flags — assuming S1–S4 have landed per the sprint's serial
   dependency order) → `run_combine(c_comb)` → `run_optimize(c_opt)` (`--book-turnover-gate` and
   `--participation-cap` set LOOSE — non-binding, but genuinely evaluated) → `run_report(c_rep)`
   (`--borrow-bps` non-zero).
3. Read back `run_report`'s `StageResult.kvs` (avoids parsing `summary.txt`): assert
   `std::isfinite` on every numeric kv (`portfolio_sharpe`, the capacity-footprint fields
   `stage_report.cpp` already emits, `book_turnover_per_day` from S5-1) — the FIRST scorecard row
   exercising the whole series together, explicitly labeled `synthetic` in the test name/assertions
   (never conflated with a real V1 run).
4. A companion `SyntheticSmoke_AllFlagsOffByteIdentical` test: the SAME synthetic panel, every S1–S5
   flag at its struct default, run TWICE with and without every flag EXPLICITLY asserted at its inert
   value (mirrors `MegaBookGraph_InertByteIdentical`'s exact shape, `stage_run_megabook_test.cpp:133-183`,
   but self-contained here per the house convention that each test TU duplicates its own fixture
   rather than reaching into another sprint's file) — asserts every stage digest identical.
5. `SyntheticSmoke_TwiceRunByteIdentical`: run the whole on-flags sequence twice; every stage digest
   and every `book_turnover_per_day`/summary kv string identical.

**Determinism:** single-threaded ctest, small population×generation budget (mirrors the
`stage_run_megabook_test.cpp` fixture's `population=12, generations=3`); no long real-panel sweep.

**Accept:**
- `SyntheticSmoke_AllFlagsOffByteIdentical` — byte-identical stage digests, flags-off vs. flags
  never-mentioned.
- `SyntheticSmoke_OnFlagsProducesFiniteScorecard` — every numeric kv `std::isfinite`; the
  `book_turnover_per_day`/participation/battery/borrow kvs are present and reflect non-sentinel
  (genuinely-computed) values given the constructed fixture's deliberate thin/thick-ADV and
  correlated-group structure.
- `SyntheticSmoke_TwiceRunByteIdentical` — identical digests/kvs across two runs.
- (d): N/A beyond what S1–S5's own per-unit tests already prove — the smoke itself is single-threaded
  per the determinism contract; documented explicitly in the test file.
- **Wrapper:**
  ```powershell
  cmake --build --preset dev --target atx-impl-tests
  ctest --preset dev -R SyntheticSmoke
  ```
  Expected: RED (file doesn't exist) before the unit; GREEN after, with every prior `AtxImplDiscover`/
  `StageRunMegaBook`/`FactoryOos` suite still green (no regression).
- **Commit:** `atx-impl/tests/stage_run_synthetic_smoke_test.cpp`,
  `atx-engine/plans/p9/sprint-5-progress.md` (close-out row).
  ```
  test(p9-s5): S5-5 synthetic-panel smoke exercising the full S1-S5 lever stack

  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  ```

---

## Sequencing

1. **S5-0 first** (ledger + config fields) — every later unit reads `RunConfig`'s new fields.
2. **S5-1** and **S5-2** in parallel after S5-0 (disjoint: S5-1 touches `stage_optimize.cpp`'s
   post-`mpo.run` tail + `stage_metabook.cpp`; S5-2 touches `stage_optimize.cpp`'s pre-`mpo.run`
   `mc` construction — land S5-1 first if both touch the same file in one PR, to keep the diff
   readable; they do not conflict semantically).
3. **S5-3** — independent of S5-1/S5-2 (different files entirely: `factory/factory.{hpp,cpp}`,
   `stage_discover.cpp`).
4. **S5-4** — independent (`book/report.hpp`, `stage_report.cpp`).
5. **S5-5 last** — exercises S5-0 through S5-4 together (plus S1–S4, assumed already landed per the
   ROADMAP's serial ordering) in one synthetic-panel smoke.

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| Attaching `ConstraintSet.part` alone silently drops gross-leverage/name-cap (the augmented path does not read `cfg.single`) | A participation-capped book loses its dollar-neutral/gross/name-cap discipline | S5-2 explicitly populates `cs.gross`/`cs.pos` from the SAME `gross_val`/`name_cap_val` the fast path already resolves — the RED→GREEN test (b) additionally asserts the capped book stays dollar-neutral and within `name_cap`, not just under the participation bound. |
| `borrow_bps` wired into the wrong accumulator (mirroring the ROADMAP's literal `loop/*` citation) would be dead code | S5-4 ships with zero observable effect on any runnable pipeline output | Root-caused up front via `grep -rn "BacktestLoop{" atx-impl/src` (zero hits) — S5-4 wires `book::accumulate_report` instead, the accumulator `stage_report.cpp` actually calls; documented explicitly as a correction, not a silent deviation. |
| `book_turnover_gate`'s day-normalization is a NEW metric with no precedent (design-spec risk #4) | A miscalibrated rate either never gates or rejects everything | Measure-before-gate: the rate is ALWAYS surfaced as a kv (S5-1), so an operator can observe real numbers before ever setting the gate; the RED→GREEN fixture pins the rate's sign/direction against two deliberately extreme (near-zero and near-maximal) synthetic books. |
| `alt_neutralization`'s liquidity-quantile group proxy could be mistaken for a real industry/GICS group_map | Overclaiming what the check validates | Documented explicitly, in both the plan and the code comment, as a coarse deterministic stand-in — not a fabricated taxonomy — consistent with the ROADMAP's own precedent of falling back to "the existing industry group_map" only where one genuinely exists. |
| `param_perturbation`'s 0-free-constant genomes trivially "pass" (CV=0) | Could look like a false sense of robustness for a template with no tunable literal | Documented as an honest degenerate (no free parameter ⇒ no knife-edge risk to detect), not silently special-cased away; covered by the RED→GREEN fixture's STABLE-candidate arm (a genome WITH free constants whose edge is flat across jitters) rather than relying on the 0-dim case to prove anything. |
| S5-3's stage_discover.cpp touch oversteps the ROADMAP's literal S5 ownership row | Ownership ambiguity / merge friction with a hypothetical parallel sprint | Flagged explicitly in the S5-3 unit as a minimal, additive, necessary correction (3 sibling lines next to an existing threading site) — the alternative (leaving the flags unthreaded) is a silent no-op, which the ROADMAP's own anti-roadmap guardrail forbids. |

---

## Bench / acceptance (sprint close)

- **Default byte-identity:** the pinned `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
  `FactoryOos.MineIntoOffPathDigestUnchanged`, `AtxImplDiscover` determinism slice, and
  `LibraryVerdict.AdmitKindEnumFrozenPrefix` all unchanged with every S5 field at its default.
- **Per-task RED→GREEN:** each opt-in field has a test RED before its wire and GREEN after (S5-0
  through S5-4).
- **Book-level bars measurable:** `book_turnover_per_day` surfaced unconditionally on both
  `run_optimize` and `run_metabook`; the participation cap's realized bound is checkable directly
  against the closed-form `%ADV` formula.
- **First real (synthetic) scorecard row:** S5-5's smoke produces a finite, twice-run-identical,
  explicitly-synthetic book-level scorecard exercising dead-alpha de-crowding, factor covariance,
  (if landed) GP trading and capacity/turnover objectives, book turnover gate, participation cap, the
  full 4-check robustness battery, and non-zero borrow — together, for the first time.
- **Twice-run + off-path byte-identity** proven per unit; **seq==parallel** proven where a genuine
  parallel substrate exists (S5-3's `mine_into` vs. `mine_into_oos_parallel`), documented N/A with
  citation where it does not (S5-1, S5-2, S5-4).

---

## Out of scope

- Any change to `risk/{factor_model,dead_factor,shrinkage,eigen_adjust}.hpp`, `stage_combine.cpp`,
  `factory/fitness.{hpp,cpp}`, `factory/search_driver.*`, or `risk/garleanu_pedersen.*` — S1/S2/S3/S4's
  files; S5 only consumes what they land.
- Editing `cost/borrow.hpp` or `loop/backtest_loop.hpp` — already correct and inert-wired for the
  event-driven engine path; S5-4 wires the SAME concept into the vectorized accumulator the CLI
  pipeline actually reaches, deliberately leaving the event-driven path untouched.
- A true GICS/industry group_map as the `alt_neutralization` source — data-gap backlog (carried
  p6→p8→p9); S5-3 uses a documented liquidity-quantile proxy instead.
- A real full-panel V1 run — the operator's step; S5-5 is a short synthetic smoke only.
- `--capacity-curve`'s own semantics (S4-owned pass-through marker, `config.hpp:336-343`) — untouched;
  S5-5's smoke merely reads the kvs it already emits.
