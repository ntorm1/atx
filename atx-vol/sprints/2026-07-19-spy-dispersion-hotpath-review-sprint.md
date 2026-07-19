# SPY Dispersion Backtest Hot Path — Code-Review Sprint Plan (2026-07-19)

**Scope:** the library's flagship end-to-end path — `build-corpus` → `run-surface-backtest` (+ `run-backtest`, `build-schedule`, `run-projected-var`, `verify`) — spanning `examples/spy_dispersion_backtest.cpp` and `src/{dispersion*,listed_dispersion*,backtest,session,portfolio_pricer,priced_surface,surface_archive,surface_db,corpus,pricer_fitter}`.
**Mandate:** correctness · performance (**boost as much as possible**) · configurability · features/unwired · **move example→library, clean the API, improve robustness + modularity**.
**Evidence:** 4 deep-dive audits in `.agents/research/dispersion-hotpath-review/` (`00-SYNTHESIS.md` + `01`–`04`). Reviewed tree: `main` (AVX2 marks default-ON per user override; WS-S mmap load merged; two-pass American solve already fused).
**Baseline (rel-avx2, quiet host):** 82-session run best **218 ms**; 135-session best **405 ms** (~333 sessions/s, ~1,804 lot-repricings on the 82-run). corpus build ≈ **0.87 s/session** (135 sessions in 118 s). Golden `final_nav = 247.4065016443293` (82-sess) — the reproducibility pin.

---

## 0. Executive summary

The path is **correct where it counts** — look-ahead is structurally clean, vega-flat neutralization is right on both the straddle-book and listed-schedule paths, and PnL greek-attribution reconciles against an independent held-mark at 1e-8. **No Critical bugs.** The work splits into four themes:

| Theme | Headline | Ceiling |
|---|---|---|
| **Performance** | The 5-solve base-greek bundle is **~83% of solve volume and runs SCALAR per-contract** in production Auto; the dispersion book is 1 straddle/name so every SIMD group is a **1-wide singleton (75% empty lanes)**. | **~1.6–2.3×** replay (packing) → **~2.5–3×** with the rho-tier; **~2×** on `build-corpus`, which is the *real* wall for long runs (~290× heavier than replay). |
| **Correctness** | The **surface-backtest path freezes the universe at day-1** (not point-in-time); reconcile aborts on a deferred first roll; constituents can never *leave* the basket. | 3 HIGH, all bounded/fixable. |
| **Config / features** | Config is typed at the leaf but **"TSV-soup at the seam"** (surface CLI honors 6 of ~20 keys); frictions, financing, and risk-limits **exist but are unexposed**; no transaction-cost model. | Frictionless mid-fills + no capital limits = unrealistic PnL. |
| **Modularity** | **~620 of 761 driver LOC are library workflow** trapped in an example `main`; usable as a library on the *output* side, not the *input* side. | Blocks testing, reuse, live parity. |

**Recommended sprint order:** WS-M (extract the library seam) unblocks clean landing of everything else, but the HIGH correctness bugs (WS-C) and the top perf lever (WS-P1) are independent and can start in parallel. Suggested waves in §6.

---

## 1. WS-P — Performance ("boost speed as much as we can")

### Step cost model (the thing to beat)
Per step, daily-hedge, ~22-unique book, Auto ISA, `analytic_greeks=true`:
```
cost/step ≈ Σ_uniques [ execute.FullGreeks(5 solves: base, σ+, σ−, r+, r−)   ← 83% of solve volume, SCALAR per-contract
                       + compute_step.shifted_mark(1 solve, AVX2)
                       + compute_step.base_greeks(0 — REUSED from execute@prev via L1 stamp) ]
```
`backtest.cpp:1716` (execute bundle), `portfolio_pricer.cpp:1587-1596` (L1 base-risk reuse stamp). The 06-review's "base greeks solved twice" is **already fixed** — verify via the `BaseGreekReuseLanes` counter, do not re-attack it.

### P1 — Cross-uid greek packing  [BIGGEST LEVER · ~1.6–2.3× replay]
**Problem.** `portfolio_pricer.cpp:806-817` / `642-691` pack SIMD lanes *within* a `(uid,side)` group. A dispersion book is one straddle per name, so every group is a **singleton** → each `evaluate_batch` issues a 1-wide pack into a 4-lane AVX2 kernel (75% of lanes idle). The AL exercise boundary is **uid-agnostic once the surface is resolved to `(S,K,T,σ,r,q)`**, so puts from *different names* can share a pack.

**Fix — two-phase resolve-then-pack:**
```cpp
// Phase A: scalar-resolve each unique against ITS OWN surface (cheap, cache-warm)
struct GreekTuple { double S,K,T,sigma,r,q; Side side; uint32_t out_idx; };
std::vector<GreekTuple> puts, calls;
for (uint32_t u = 0; u < uniques.size(); ++u) {
    auto p = resolve_params(uniques[u], surfaces[u]);      // total_variance eval etc.
    (p.side == Side::Put ? puts : calls).push_back({p.S,p.K,p.T,p.sigma,p.r,p.q,p.side,u});
}
// Phase B: gather across ALL names into wide laned bundles, scatter back
american_put_greeks_batch(puts,  /*n≈11*/ out_greeks);      // one call, 4-lane packed
american_call_greeks_batch(calls, /*n≈11*/ out_greeks);     // needs P.CALL kernel (P1b)
// deterministic pack membership: mirror the marks tile schedule (portfolio_pricer.cpp:788-801)
```
Turns ~22 scalar 5-solve bundles into ~6 four-lane packed bundles.
**Prereqs:** P1a (flip the laned-greeks Auto gate) + P1b (call-side kernel) below.
**Gate:** greek parity vs scalar within adjoint/FD tolerance; deterministic across thread counts (pack membership must not depend on thread partition — reuse the marks tile-schedule invariant); dispersion NAV byte-identical if AVX2-greeks parity is exact, else PM-gated golden refresh; rel-avx2 replay best-of-7 before/after.
**Ref:** Arrow SoA + 64-byte alignment (topic 3); Intel SYCL "SoA required, ~2.5× AVX-512" (topic 4); Andersen-Lake boundary is state-only once resolved (topic 4).

#### P1a — Flip the laned-greeks Auto gate  [prereq · ~1.5–2× standalone on multi-strike books]
`priced_surface.cpp:1070-1073` hard-gates laned-greek dispatch on `resolved_price_isa == ForceAvx2` even though `kShipAvx2Greeks == true` (`american_boundary_batch.cpp:153`). Under Auto it silently falls to the scalar per-contract loop. Flip to dispatch on `avx2_greeks_selected(isa)`.
**BUT** greeks AVX2 vs scalar differ ~1e-13 (WS-H found this breaks `evaluate_batch == per-entry evaluate` bit-identity). Two options, PM-decide:
1. **Economic-parity gate** (recommended): relax the batch==per-entry greek contract to a tolerance (greeks are consumed at ~1e-6 economic precision), refresh affected golden fingerprints, keep Auto→AVX2.
2. Keep greeks scalar in Auto (status quo) and expose an explicit `PricingMode::FastGreeks` opt-in the dispersion config sets — reproducible-by-default, fast on request.
**Ref:** Intel SYCL "runtime CPU dispatch + portable fallback" (topic 4).

#### P1b — Call-side laned greeks kernel  [prereq · dispersion is 50% calls]
`priced_surface.cpp:1127-1135` sends calls to the scalar path; the laned kernel is PUT-native. Add `american_call_greeks_batch` (mirror the put kernel; call early-exercise boundary). Without it, P1/P1a only speed the put half.

### P2 — rho-drop risk tier  [~1.3–1.5×, composes with P1 → ~2.5–3×]
`portfolio_pricer.cpp:1595,1649` force `base_greek_needs.full()` so every base bundle computes `r±` even when the P&L rate-shift `dr ≈ 0` ⇒ `pnl_rho ≈ 0`. The K4 need-selectors are already wired (`priced_surface.cpp:648`). Drop `r±` when `dr==0`: **5 → 3 solves = −40%**.
```cpp
GreekNeeds needs = base_greek_needs;
if (pnl_config.dr == 0.0) needs.want_rho = false;   // pnl_rho becomes exactly 0
```
**Output change** (`pnl_rho` column → 0 when un-shocked) → PM-gated + golden refresh + a note in the tearsheet schema. Compose with P1.

### P3 — build-corpus throughput  [~2× · the REAL wall for long backtests]
Corpus build is **~290× heavier than replay** (≈0.87 s/session vs ≈3 ms/session) — it dominates wall-clock until a corpus is re-run ~290×. Two levers:
1. **Use the E-cores.** `surface_db_populate.cpp:252-254` pins fit fan-out to P-cores only → **8 E-cores idle**. Add a second-tier scheduler that offloads independent board fits to E-cores (lower priority, no bench-lease conflict). **+40–70%.**
2. **SIMD the de-Am inversion.** `calib.cpp:1019-1191` inverts American→European per strike **scalar**; it is batchable across a board's strikes (same shape as the greek packing). **~1.4–1.7×.**
Together ≈ **halve build wall (~25 surf/s)**.
**Ref:** Global-eSSVI warm-started parallel fit (topic 5); Andersen-Lake ~100k prices/s/CPU as the throughput target (topic 4).

### P4 — Async prefetch + zero-copy on the strategy load overload  [bounded, post-mmap]
`snapshot_cache.cpp:224-255` loads synchronously with no load/compute overlap despite the `backtest.hpp:322` prefetch promise; the strategy whole-board overload (`backtest.cpp:1838`,`1206-1220`) still owned-reconstructs instead of borrowing `PricedSurfaceView`. Single-digit-% now that mmap made opens cheap — do it for the pipelining win on larger universes, not this bench.
**Ref:** QuantStart "batch within an event"; LMAX single-threaded core + prefetch ring (topic 2).

**WS-P acceptance:** rel-avx2 replay best-of-7 improvement reported per lever; greek parity gate green; determinism double-run byte-identical (or PM-approved golden refresh with rationale); a `dispersion_replay_bench` + `surfdb_build_bench` added as regression guards.

---

## 2. WS-C — Correctness

### C1 — Point-in-time universe on the surface path  [HIGH · flagship path]
`spy_dispersion_backtest.cpp:531,604` build the strategy with `universe_at(front().date)` and never re-resolve → membership + weights are frozen at day-1 for the *entire* surface backtest and projected-VaR. The listed path correctly re-resolves per roll (`universe_at(ref.date)`).
**Fix:** re-resolve inside `DispersionStrategy::on_step`:
```cpp
void DispersionStrategy::on_step(const StepCtx& ctx) {
    const auto members = universe_.at(ctx.base_date);   // PIT snapshot for THIS step
    if (members != active_members_) rebalance_to(members);   // re-solve vega-flat legs
    ...
}
```
Fold in C3 (removal) so a name dropping out actually exits. Add a test: a mid-backtest reconstitution changes the served basket.
**Ref:** Bossu/JPM vega-neutral is re-solved *per rebalance* (topic 1); NautilusTrader PIT event model (topic 2).

### C2 — Reconcile abort on deferred first roll  [HIGH]
`listed_dispersion_reconciliation.cpp:240` requires `snapshots.front().date == rolls.front().roll_date`, but `build_schedule_command:389-413` legitimately defers the first roll via the coverage gate → `run-backtest` hard-aborts on valid inputs.
**Fix:** start the reconcile timeline at `rolls.front().roll_date` (skip leading flat dates, preserving row-count alignment):
```cpp
auto first = std::find_if(snapshots.begin(), snapshots.end(),
                          [&](auto& s){ return s.date >= rolls.front().roll_date; });
reconcile(std::span(first, snapshots.end()), rolls);   // leading flat dates carry no position
```

### C3 — Constituents can never leave the basket  [HIGH]
`universe_at` keeps the latest row per symbol and `read_universe:205` rejects `weight ≤ 0`, so there is **no way to encode a removal** (`dispersion_workflow.cpp:232-245`) — the basket only grows/reweights. A real index (GOOG/GOOGL, reconstitutions) needs removals.
**Fix (choose one):** (a) a `weight = 0` / `REMOVE` sentinel row that `universe_at` honors as "drop this symbol as of date"; or (b) treat each `effective_date` block as a **full PIT snapshot** (membership = exactly the rows on/before the latest effective_date ≤ query). (b) is cleaner and matches how index vendors publish.
**Ref:** ArcticDB versioned PIT snapshots (topic 3).

### C4 — Robustness cluster (MED)
- **M2 universe dedup / stable sort** (`read_universe`): reject duplicate `(effective_date,symbol)` keys; `std::sort` is non-stable so differing dup rows yield nondeterministic weights. → reproducibility bug. Fix: `std::stable_sort` + hard reject dups.
- **M1 `verify` numeric check** (`spy_dispersion_backtest.cpp:447`): only checks the external `reference_reconciliation.tsv` exists/size, never compares numbers, and no C++ writes it. Fix: native reference reconcile + numeric compare (fold into `verify_dispersion_run`, WS-M).
- **M3 float-exact entry-mark reconcile** (`reconciliation.hpp:87`, tol 0.0): bit-identical today between build-route `evaluate(Price)` and reconcile-route `fair_value`; a 1-ULP shift in either → silent hard-abort. Fix: few-ULP relative tolerance.
- **M4 `"SPY"` hardcoded in library** (`dispersion_workflow.cpp:224,238`): parameterize the index symbol.
- **M5 synthetic-expiry settlement** on clock-gap > `roll_dte`: settlement needs exact snapshot-ts match; document + guard.

**Preserve (do not regress):** look-ahead cleanliness, vega-flat neutralization, the 1e-8 greek-explain reconciliation. Add these as explicit regression assertions in the extracted library tests.

---

## 3. WS-X — Configurability + Features

### X1 — One strict typed `DispersionRunConfig` (kill the TSV-soup seam)
Today: `RunSpec{map<string,string>}` → `DispersionBacktestConfig` → `DispersionConfig` + `RunConfig`, hand-mapped; the surface CLI copies **6 of ~20** keys and silently ignores the rest; no unknown-key rejection.
**Fix:** a single typed struct the TSV **strictly** deserializes into (reject unknown keys, typed defaults, one round-trip):
```cpp
struct DispersionRunConfig {
    DateRange dates; std::string snapshot_suffix; fs::path opra_root, definitions;
    UniverseSpec universe; RateSource rate;                 // was flat_rate misrouted to fit only
    DispersionSide side = ShortIndexLongNames;              // X-expose
    WeightingScheme weighting = VegaNeutral;                // X4
    StrikeRule strike = AtmForwardStraddle;                 // X4
    DteBands dte; int roll_dte_days;
    HedgeSpec hedge{DeltaToZero, Cadence::Daily};           // X-expose
    Frictions frictions; Financing financing;               // X2 — currently OFF+unexposed
    RiskLimits limits;                                      // X3 — currently absent
    FitConfig fit;                                          // typed, was locked in driver
    SurfaceProvenancePolicy provenance = Compatibility;
    int multiplier = 100;                                   // was hardcoded
};
Result<DispersionRunConfig> read_dispersion_run_config(const fs::path&);  // strict
```

### X2 — Expose frictions + financing to dispersion  [HIGH realism]
`RunConfig` already carries frictions (`backtest.hpp:257`) and financing (`:269`); the dispersion path never sets them → **always frictionless mid-fills, no carry**. Wire `DispersionRunConfig.frictions/financing` through `dispersion_backtest_config_from_run_spec`. Default to a realistic spread, not zero.

### X3 — Risk limits / capital / drawdown-stop  [HIGH]
No capital, gross/net limits, or drawdown-stop anywhere in the loop. Add a `RiskLimits{max_gross_vega, max_gross_notional, capital, drawdown_stop}` checked in `on_step` before sizing; breach → clamp or halt with a recorded reason.

### X4 — Weighting-scheme + strike-rule policies
`dispersion.cpp:488-496` hardcodes one weighting (vega-neutral 1:1) and one strike rule (ATM-fwd straddle). Per the BNP desk note (topic 1) the leg-sizing weighting **must be a policy re-solved each rebalance**. Add:
```cpp
enum class WeightingScheme { VegaNeutral, ThetaNeutral, GammaNeutral, EqualVega };
enum class StrikeRule { AtmForwardStraddle, DeltaStrangle /*e.g. 25Δ*/, FixedMoneyness };
struct LegSizer { virtual LegSet size(const Basket&, const Surfaces&, const DispersionRunConfig&) = 0; };
```
Add a **correlation-gamma** risk metric to the board (Moontower: vega-neutral is short correlation convexity ≠ correlation-neutral).

### X5 — Tearsheet + benchmark-relative stats on the surface path
The surface path emits raw SoA and **never calls `tearsheet()`** → no headline stats. `TearSheet` is absolute-only. Add: call `tearsheet()` on the surface path, and extend it with **IR / alpha / beta / tracking-error vs a supplied vol benchmark** (Goodwin/Grinold-Kahn, topic 6).

### X6 — Transaction-cost model
Replace mid-fills with **spread + square-root market impact** (Almgren β≈0.6 / Obizhaeva-Wang), coefficients data-calibrated + config-driven (topic 6). $0.10–0.20/leg slippage moves option-strategy returns 10–30%, so this materially changes conclusions.
```cpp
double fill_price(Side s, double mid, double half_spread, double adv_frac, const CostModel& m) {
    double impact = m.k * std::pow(std::max(adv_frac, 0.0), m.beta);   // β≈0.6
    return mid + (s==Buy? +1:-1) * (half_spread + impact);
}
```
Also: `entry_every_n` is plumbed but ignored (`dispersion_backtest.cpp:28` vs `strategy.cpp:816-824`) — honor it; `run-projected-var` is half-wired (no verify gate, no test) — finish or gate it; `record_diagnostics` (implied-corr signal) is never enabled by the CLI — expose it.

**WS-X acceptance:** unknown-key rejection test; a frictioned+financed run differs from the frictionless golden by the expected cost; risk-limit breach test; tearsheet headline stats present on the surface path; weighting-scheme + strike-rule unit tests.

---

## 4. WS-M — Example → Library (modularity, robustness, API)

**~620 of 761 LOC in `spy_dispersion_backtest.cpp` are library workflow.** Extract into the library; leave the example a thin CLI. Common blocker to resolve first: **pinned admission fingerprints/thresholds are inline literals load-bearing for `verify`** — move them to named library constants (`DispersionCorpusPolicy` defaults) so extraction preserves byte-for-byte reproduction.

### Extraction map (block → proposed API)
| Driver block | Proposed library entry point |
|---|---|
| `build_corpus_command` | `build_dispersion_corpus(DispersionRunConfig, DispersionCorpusPolicy) -> Result<CorpusHandle>` |
| `build_schedule_command` | `build_listed_dispersion_schedule(...) -> Result<Schedule>` (roll-deferral + coverage gate) |
| `run_backtest_command` | `run_listed_dispersion_backtest(...) -> Result<DispersionBacktestOutcome>` (fold C2) |
| `run_surface_backtest_command` | `run_dispersion_backtest(Clock, DispersionUniverse, DispersionRunConfig) -> Result<DispersionBacktestOutcome>` (fold C1) |
| `run_projected_var_command` | `run_dispersion_projected_var(...) -> Result<VarReport>` + VaR serializers |
| `verify_command` | `verify_dispersion_run(run_dir) -> Result<VerifyReport>` (fold M1 numeric compare) |
| surface config assembly | `dispersion_backtest_config_from_run_spec(DispersionRunConfig)` |
| persist/verify occ_ess | dedicated `occ_ess` module |
| `load_listed_quotes`, inventory/methodology writers | `write_dispersion_artifacts(dir, outcome)` + IO helpers |

### Typed result
```cpp
struct DispersionBacktestOutcome {
    BacktestResult track;          // existing rich SoA: PnL track + 8-axis attribution + greeks + signals
    TearSheet sheet;               // headline stats incl. benchmark-relative (X5)
    std::vector<DropEvent> drops;  // missing-name / degraded events (surface reproducibility)
    std::vector<RollRecord> per_roll;
};
```
Sizing is already pure/stateless and missing-name handling already degrades cleanly (DropRenormalize, index-leg fatal) — preserve those; the win is a typed, testable, live-reusable surface.

**WS-M acceptance:** the example `main` shrinks to CLI-glue; each extracted API has a unit test; `verify` reproduces byte-identically from library constants (no example-only literals); a golden end-to-end test drives the library API (not the CLI).

---

## 5. Reference material (annotated — full bibliography in `04-web-references.md`)

- **Dispersion construction:** Bossu, Strasser, Guichard, *Variance Swaps* (JPMorgan, 2005) — vega-neutral leg sizing (single-stock vega notional matched to index vega). Moontower, *Dispersion for the Uninitiated* (2023) — vega-neutral ⇒ **short correlation convexity**, motivates the correlation-gamma metric (X4). BNP desk note — weighting must be a re-solved policy (X4).
- **Engine architecture:** NautilusTrader (GitHub) — ns-timestamp strict ordering ⇒ look-ahead structurally impossible + backtest/live parity (validates C1, motivates a shared time model). LMAX Disruptor / Fowler (2011) — single-threaded allocation-free core, event-sourced deterministic replay (P4, determinism). QuantStart — batch within an event, not across time (P1).
- **Data layout:** Apache Arrow columnar — 64-byte alignment = AVX-512 width, SoA columns, zero-copy/mmap (P1, P3). ArcticDB (Man Group) — versioned PIT "time-travel" reads ⇒ reproducible backtests (C3, reproducibility pin).
- **SIMD pricing:** Andersen-Lake-Offengenden, *High-Performance American Option Pricing* (SSRN 2547027, 2015) — Chebyshev boundary + Gauss-Legendre, ~100k prices/s/CPU (P1/P3 target). Schadner, *Explicit BS Implied Vol* (arXiv 2604.24480, 2026) — branchless closed-form IV, 3.4× vs Jäckel (*single-source, verify before load-bearing*). Intel SYCL BS (arXiv 2204.13740, 2022) — AVX-512 ~2.5× over SSE, SoA required, runtime dispatch (P1a).
- **Calibration:** Gatheral-Jacquier, *Arbitrage-Free SVI* (arXiv 1204.0646, 2013) — enforce arb-free as **hard bounds inside the fit loop** (relates to prior WS-C SVI work + P3). Global eSSVI (arXiv 2204.00312, 2022) — warm-started parallel global fit (P3).
- **Realism:** BSIC, *Transaction Cost Modelling* (2025) — spread + √-impact, Almgren β≈0.6 (X6). Goodwin, *Information Ratio* (FAJ 1998) / Grinold-Kahn — IR/alpha/beta/tracking-error reporting (X5).

---

## 6. Execution model (subagent-driven, git-tracked)

**Worktree:** fresh `wt-disp-hotpath` (branch `feat/disp-hotpath`) off local `main`. Build: `dev` preset = test gate, `rel-avx2` = perf; via `scripts/atx-build.ps1` (see prior sprint §1 for the wrong-tree/quoting rules). Golden pin: dispersion `final_nav = 247.4065016443293` (82-sess) must stay byte-identical unless a lever is an explicit PM-gated output change (P1a option 1, P2).

**Suggested waves** (dependency-ordered; file-ownership disjoint within a wave):

- **Wave 0 (parallel):**
  - *WS-M-extract* — carve out the library APIs + `DispersionCorpusPolicy` constants (unblocks testing). Owns `examples/spy_dispersion_backtest.cpp`, new `src/dispersion_run.cpp`, headers. **Do first / largest.**
  - *WS-P1a+P1b* — flip the laned-greeks Auto gate (PM-decide parity option) + add the call-side kernel. Owns `priced_surface.cpp`, `src/simd/american_*greeks*`. Independent of the refactor.
  - *WS-C-univ* — C1 PIT universe + C3 removal + C2 reconcile + C4 (dedup/tol/SPY). Owns `dispersion_workflow.cpp`, `dispersion_strategy.cpp`, `listed_dispersion_reconciliation.cpp`, `dispersion.cpp`.
- **Wave 1 (after M-extract + P1a/b):**
  - *WS-P1* — cross-uid greek packing (the big lever) on the now-clean pricer seam. Owns `portfolio_pricer.cpp`, `priced_surface.cpp`.
  - *WS-X-config* — typed `DispersionRunConfig` strict loader + expose side/frictions/financing/hedge/weighting/strike (X1,X2,X4). Consumes WS-M's config seam.
- **Wave 2:**
  - *WS-P2* (rho-tier, PM-gated) + *WS-P3* (build-corpus E-cores + SIMD de-Am).
  - *WS-X* remainder — risk limits (X3), tearsheet + benchmark stats (X5), transaction costs (X6).
- **Wave 3 (stretch):** WS-P4 (async prefetch/zero-copy), intraday/multi-snapshot, scenario sweep.

**Per-workstream deliverable:** compressed report (change, files, gate result, perf delta, determinism), conventional commits, PM integrates + gates each merge. Add `dispersion_replay_bench` + `surfdb_build_bench` as CI perf guards. Every correctness fix ships with a regression test; every perf lever ships a before/after ns/op and a parity/determinism proof.

**Projected outcome if Wave 0–2 land:** replay **~2.5–3×** (packing + rho-tier), build-corpus **~2×** (the real wall for long runs), realistic PnL (costs+financing), point-in-time universe, and a typed, testable, live-reusable dispersion library API — with the golden reproducibility pin preserved (or explicitly, auditably refreshed).
