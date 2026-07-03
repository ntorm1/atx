# Sprint 4 — Cost-Aware Admission Gates

**Goal:** wire transaction cost and holding-period discipline into the admission gate so the pipeline screens on NET-of-cost viability, not frictionless fitness — all opt-in, default byte-identical.

**Owns (exclusive):**
`atx-engine/include/atx/engine/combine/{gate,metrics}.hpp`,
`atx-engine/include/atx/engine/library/library.hpp`,
`atx-engine/include/atx/engine/cost/cost_aware.hpp`,
`atx-engine/include/atx/engine/eval/deflated_sharpe.hpp`;
tests under `atx-engine/tests/combine/` and `atx-engine/tests/library/`.

**Must NOT touch:** `factory/{fitness,search_driver,generate,genome,behavior,mutation,pool_view}.hpp` (Sprint 3), `factory/factory.cpp` / `factory.hpp` (Sprint 2), `atx-impl/src/{config.hpp,config.cpp,stage_*.cpp}` (Sprint 7), `oracle.hpp` (untouchable by every sprint).

---

## The cost-awareness gap (verified file:line)

The gate today screens for a frictionless world. Three computable quantities exist in the headers but none of them is gated.

| Gap | File:line | Evidence |
|---|---|---|
| Gate and library verdict use frictionless `metrics.fitness < min_fitness` | `gate.hpp:110`; `library.hpp:412` | `if (metrics.fitness < cfg.min_fitness) return GateVerdict::RejectFitness;` / `if (c.metrics.fitness < cfg.min_fitness) return AdmitKind::RejectFitness;` |
| `cost_adjusted_fitness` exists but nothing calls it | `cost_aware.hpp:169` | `cost_adjusted_fitness(const AlphaMetrics& m, f64 rt_cost_bps)` — pure function, zero call sites |
| `holding_days = 1/max(turnover,eps)` is computed but never gated | `metrics.hpp:201` | `const atx::f64 holding_days = 1.0 / turnover_eps;` — field stored in `AlphaMetrics`, no floor in `GateConfig` |
| WQ fitness formula `sqrt(|returns|/max(turnover,0.125))*sharpe` | `metrics.hpp:203` (comment), `metrics.hpp:208` (compute) | Frictionless by construction — `returns` is gross, no cost subtracted before computing the numerator |
| `max_turnover_for(rt_cost_bps, horizon_days)` and `cost_aware_knobs(...)` emit a tightened gate ceiling | `cost_aware.hpp:132`, `cost_aware.hpp:151` | Pure free functions, emit-only, not wired into `GateConfig` at construction |
| `deflated_sharpe` takes a caller-supplied per-period SR computed from frictionless PnL | `deflated_sharpe.hpp:136` | Signature: `deflated_sharpe(f64 sr, usize T, f64 skew, f64 exkurt, usize N, optional<f64> var)` — no net-of-cost path |

---

## Determinism contract (Sprint 4)

Sprint 4 follows the **p6 (A) Opt-in / default-byte-identical** contract (ROADMAP §Shared determinism contract, case A). Every new capability lives behind a new `GateConfig` field or a new function overload:

- New `GateConfig` fields: `rt_cost_bps = 0.0`, `min_holding_days = 0.0` — both default to their inert values.
- At the inert default, `cost_adjusted_fitness(m, 0.0) == m.fitness` and `holding_days >= 0.0` is always true, so `AlphaGate::admit` and `Library::verdict_for` are **byte-identical** to today.
- The existing `deflated_sharpe` signature and behavior are **unchanged**; the net-of-cost path is a new overload / helper.
- `AdmitKind` enumerator order is FROZEN (stable reject-histogram index). If a new `RejectHoldingPeriod` value is added it MUST be appended at the END, and a histogram-layout test must be updated to confirm the size change is intentional.
- Sprint 7 turns the fields on via `atx-impl/src/config.cpp`; this sprint does not touch the CLI.

**Three test classes per opt-in field (mandatory):**
(a) off-path byte-identity — default fields, output digest/verdict unchanged,
(b) on-path RED → GREEN — opt-in field non-zero, a qualifying candidate flips,
(c) twice-run — same inputs, same verdict on second call (no hidden state).

---

## Tasks

### S4-0 — Add `rt_cost_bps` and `min_holding_days` to `GateConfig` (plumbing; do first)

**Root cause:** `GateConfig` (`gate.hpp:67–72`) has no cost or holding-period fields. Both `AlphaGate::admit` (`gate.hpp:110`) and `Library::verdict_for` (`library.hpp:412`) read `cfg.min_fitness` directly against frictionless `metrics.fitness`.

**Fix:** Add two fields to `GateConfig`:
```
atx::f64 rt_cost_bps     = 0.0;   // round-trip cost in bps; 0 => inert (frictionless)
atx::f64 min_holding_days = 0.0;  // holding-period floor in periods; 0 => inert
```
No logic changes in this task — the fields exist but nothing reads them yet. `GateConfig` is aggregate-initialized in declaration order; append after `max_pool_corr` so existing aggregate initializers are backward-compatible.

**Determinism:** pure addition. Existing goldens unchanged. No tests beyond a compile check, but confirm `sizeof(GateConfig)` change does not affect any serialized format (it does not — `GateConfig` is an in-memory config struct, not stored).

**Accept:** project compiles; existing gate and library tests still green; no aggregate-initializer breakage.

---

### S4-1 — Net-of-cost fitness floor in `AlphaGate::admit` and `Library::verdict_for`

**Root cause:** both admission paths gate on `metrics.fitness < cfg.min_fitness` using the frictionless `AlphaMetrics::fitness` field. `cost_adjusted_fitness` at `cost_aware.hpp:169` subtracts `turnover * rt_cost_bps * kFitnessScale` from raw fitness but is never called.

**Fix:** when `cfg.rt_cost_bps > 0.0`, substitute `cost::cost_adjusted_fitness(metrics, cfg.rt_cost_bps)` for `metrics.fitness` in the fitness floor check — in BOTH:
- `AlphaGate::admit` (`gate.hpp:110`): `const atx::f64 eff_fitness = (cfg.rt_cost_bps > 0.0) ? cost_adjusted_fitness(metrics, cfg.rt_cost_bps) : metrics.fitness;`
- `Library::verdict_for` (`library.hpp:412`): identical substitution so the two paths remain verdict-equivalent.

Include `atx/engine/cost/cost_aware.hpp` in `gate.hpp` (it already includes `metrics.hpp`; add the cost include). `library.hpp` includes `gate.hpp`; no additional include needed there.

**Determinism:** `rt_cost_bps = 0.0` (default) ⇒ `cost_adjusted_fitness(m, 0.0) = m.fitness - 0 = m.fitness` ⇒ byte-identical to today. The branch is a pure arithmetic guard — no allocation, no state.

**Accept:**
- (a) `gate_netcost_offpath`: `rt_cost_bps=0.0` — admit/reject digest byte-identical to today's golden.
- (b) `gate_netcost_onpath`: synthesize a candidate with `gross fitness=1.1`, `turnover=0.8`, `rt_cost_bps=10.0` ⇒ `cost_adjusted_fitness = 1.1 - 0.8*10.0*0.1 = 0.3 < 1.0` ⇒ `RejectFitness`; same candidate with `rt_cost_bps=0.0` ⇒ `Accept` (assuming corr clears). Same test via `Library::verdict_for`.
- (c) twice-run: same inputs, same verdict on second call.

---

### S4-2 — Holding-period floor via `min_holding_days`

**Root cause:** `metrics.hpp:201` computes `holding_days = 1.0 / turnover_eps` and stores it in `AlphaMetrics::holding_days`, but no gate checks it. An alpha trading every period (`turnover ≈ 1.0`, `holding_days ≈ 1`) passes even when `min_holding_days = 5`.

**Fix:** in `AlphaGate::admit` and `Library::verdict_for`, after the turnover floor check (preserving the §5.2 fixed check order), add:
```
if (cfg.min_holding_days > 0.0 && metrics.holding_days < cfg.min_holding_days) {
    return GateVerdict::RejectTurnover; // or RejectHoldingPeriod — see below
}
```

**On the enum question:** if reusing `RejectTurnover` is semantically close enough (holding-period is a turnover derivative), reuse it — no histogram-layout change, no test update needed. If a distinct `RejectHoldingPeriod` value is added to `GateVerdict` and `AdmitKind`, it MUST be appended at the END of each enum (the layout is FROZEN — `gate.hpp:84–90`, `library.hpp:113–122`). A histogram-layout test must document the new count and assert no existing index shifts.

**Determinism:** `min_holding_days = 0.0` (default) ⇒ the guard `> 0.0` is false ⇒ the branch never fires ⇒ byte-identical to today.

**Accept:**
- (a) `holding_floor_offpath`: `min_holding_days=0.0` — verdicts byte-identical.
- (b) `holding_floor_onpath`: candidate with `turnover=1.0` (`holding_days=1.0`), `min_holding_days=5.0` ⇒ rejects; same candidate with `min_holding_days=0.0` ⇒ passes the holding check.
- (c) twice-run: same inputs, same verdict.
- (d) If `RejectHoldingPeriod` added: histogram-layout test passes with new enum size; no existing index shifts.

---

### S4-3 — Expose cost-derived `max_turnover` helper on `GateConfig`

**Root cause:** `cost_aware.hpp:132` has `max_turnover_for(rt_cost_bps, horizon_days)` and `cost_aware.hpp:151` has `cost_aware_knobs(...)` — both compute a tightened `max_turnover` ceiling, but the derivation is only accessible to callers who use `CostKnobs` directly. Sprint 7 needs to derive a `max_turnover` from cost calibration and inject it into `GateConfig::max_turnover`.

**Fix:** add a free function in `cost_aware.hpp` (or a `static` helper on `GateConfig`, but prefer a free function to avoid a circular include):
```cpp
// Returns a GateConfig with max_turnover tightened for `rt_cost_bps` and `horizon_days`.
// All other fields keep their defaults. The default max_turnover (0.70) is UNCHANGED
// when this function is not called — Sprint 7 calls it during config construction.
[[nodiscard]] inline combine::GateConfig gate_config_for_cost(
    atx::f64 rt_cost_bps, atx::f64 horizon_days) noexcept {
    combine::GateConfig cfg{};
    cfg.max_turnover = max_turnover_for(rt_cost_bps, horizon_days);
    return cfg;
}
```

**Determinism:** pure addition. Does not change `GateConfig`'s default `max_turnover = 0.70` (`gate.hpp:70`). No existing golden is affected.

**Accept:** unit test calls `gate_config_for_cost(10.0, 20.0)` and asserts `max_turnover < 0.70` (cost-tightened); calls `gate_config_for_cost(0.0, 20.0)` and asserts `max_turnover == 0.70` (zero cost ⇒ default ceiling). Existing gate goldens green.

---

### S4-4 — Net-of-cost DSR helper (new overload, existing `deflated_sharpe` unchanged)

**Root cause:** `deflated_sharpe.hpp:136` takes a caller-supplied per-period Sharpe computed from the frictionless PnL stream. There is no path to deflate against a net-of-cost return series. `compute_metrics` (`metrics.hpp:184`) returns only mean turnover — no per-period turnover vector is exposed for a net-per-period construction.

**Fix — two additions:**

1. **Opt-in `compute_metrics` variant** in `metrics.hpp`: add an overload (or an opt-in out-parameter) that, when a non-null `std::vector<atx::f64>*` output pointer is supplied, also fills a per-period turnover vector `u[t]` (same values as the `mean_turnover` inner loop computes). The default `compute_metrics` signature (`metrics.hpp:184`) is **unchanged**. Name the variant `compute_metrics_with_turnover` to avoid overload ambiguity.

2. **Net-of-cost DSR helper** in `deflated_sharpe.hpp`: add a free function that, given a per-period PnL series, a per-period turnover series, and `rt_cost_bps`, computes the net per-period return `r_net[t] = r_gross[t] - turnover[t] * rt_cost_bps / 1e4`, derives net moments (mean/std/skew/exkurt over `r_net[1..T)`), and calls the existing `deflated_sharpe`. Name it `deflated_sharpe_net_cost`. The existing `deflated_sharpe` signature at `deflated_sharpe.hpp:136` is **untouched**.

**Determinism:** both additions are new symbols. No existing path changes. The existing `deflated_sharpe` tests are byte-identical.

**Accept:**
- `netcost_dsr_onpath`: a synthetic high-turnover PnL series (`turnover[t] ≈ 0.8`, `rt_cost_bps=10.0`) ⇒ `deflated_sharpe_net_cost` DSR < `deflated_sharpe` DSR on the same gross series.
- `netcost_dsr_zero_cost`: `rt_cost_bps=0.0` ⇒ `deflated_sharpe_net_cost` DSR equals `deflated_sharpe` DSR within floating-point epsilon.
- Existing `deflated_sharpe` test suite unchanged and green.

---

## Sequencing

1. **S4-0 first** (field plumbing) — S4-1 and S4-2 read the new fields.
2. **S4-1 and S4-2 in parallel** — disjoint logic paths within `gate.hpp`/`library.hpp`; S4-1 touches the fitness check, S4-2 touches the turnover/holding check.
3. **S4-3** — independent; no dependency on S4-1/S4-2.
4. **S4-4** — independent; touches `metrics.hpp` and `deflated_sharpe.hpp` only.

S4-3 and S4-4 may run concurrently with S4-1/S4-2 after S4-0 lands.

---

## Risks / guardrails

- **Enumerator order corruption.** `GateVerdict` and `AdmitKind` order is FROZEN (reject-histogram index). Any new enumerator MUST append at END. CI: compile check + histogram-layout assertion.
- **Double-include of `cost_aware.hpp`.** `cost_aware.hpp` already includes `gate.hpp` and `metrics.hpp` (`cost_aware.hpp:51–52`). Adding a `#include "atx/engine/cost/cost_aware.hpp"` in `gate.hpp` would be circular. Instead, move the `cost_adjusted_fitness` formula inline into `gate.hpp` (it is two lines: `turnover * rt_cost_bps * kFitnessScale`) or extract it to a shared `combine/cost_util.hpp` that neither `gate.hpp` nor `cost_aware.hpp` includes. Preferred: inline the one-liner.
- **`Library::verdict_for` and `AlphaGate::admit` divergence.** Both paths must apply the SAME cost-adjusted fitness formula in the SAME check order. The acceptance test MUST exercise both paths independently (the library's `verdict_for` test is distinct from the gate test).
- **`compute_metrics` default path unchanged.** The new `compute_metrics_with_turnover` variant must not alter the default `compute_metrics` return values or performance. Test: both called on the same inputs ⇒ `AlphaMetrics` fields byte-identical.
- **Sprint 7 dependency.** S4 makes cost-gating reachable but does not wire CLI flags. Sprint 7 (`atx-impl/src/config.cpp`) sets `GateConfig.rt_cost_bps` and `GateConfig.min_holding_days` from the CLI panel calibration. S4 must not touch those files.

---

## Bench / acceptance (sprint close)

- **Default-field byte-identity:** run the existing `gate_*` and `library_*` golden suites with default `GateConfig` ⇒ zero verdict changes, zero digest changes, zero new test failures.
- **Per-task RED → GREEN:** each opt-in field has a test that starts failing (RED) before the implementation and passes (GREEN) after; see individual task acceptance above.
- **Twice-run determinism:** every new test class includes a "call twice, compare results" assertion.
- **`cost_adjusted_fitness` reachable:** a Sprint 7 caller can set `cfg.rt_cost_bps = X` and observe that a high-turnover alpha that previously passed `min_fitness` is now rejected.
- **`max_turnover_for` reachable:** `gate_config_for_cost` unit test green; Sprint 7 can invoke it during config construction.
- **net-DSR reachable:** `deflated_sharpe_net_cost` unit test green; net DSR < gross DSR for a high-turnover stream.

---

## Out of scope

- Wiring `rt_cost_bps` / `min_holding_days` to the CLI or calibrating cost from the execution panel — Sprint 7.
- The turnover penalty in the search's raw fitness signal — Sprint 3.
- Factory admission ladder changes — Sprint 2.
- Any `atx-impl` source files.
