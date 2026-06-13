# Phase 3b — Alpha-DSL VM Completion (vocabulary · variadic args · conformance · mass eval · loop bridge) — Implementation Plan

**Module:** `atx-engine` (`p0`)
**Phase theme:** **Complete the signal-generation VM.** Phase 3 ships the alpha-expression compute core
(lex → Pratt parse → typecheck → hash-consed DAG → vectorized columnar VM) over the Alpha101 ∪ core-Qlib
operator set, emitting a per-alpha cross-sectional signal. Phase 3b/3c finishes the job so the VM is a
*complete, WorldQuant-grade signal generator* **before** Phase 4 builds combiners/risk/optimizers on top of
it: the **BRAIN-superset operator vocabulary** consultants actually use, **variadic/default arguments**,
an **Alpha101 conformance battery** that pins the disputed semantics, a **batch / cross-alpha evaluation
API** with measured CSE evidence, the **per-alpha PnL/position streams** Phase 4 consumes, and the
**`VmSignalSource` bridge** that finally lets the VM feed the real Phase-2 backtest loop.
**Status:** frozen at kickoff (fossil — scope changes go in the ledger's *Plan adjustments*, not here).
**Authority:** [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md). Quality bar:
[`../docs/implementation-quality.md`](../docs/implementation-quality.md).
**Read first:** [`phase-3-alpha-expression-dsl-implementation-plan.md`](phase-3-alpha-expression-dsl-implementation-plan.md)
(this **extends** it — same VM, same invariants, same operator-registry contract) and
[`engine-architecture-audit.md`](engine-architecture-audit.md) §2 (the two-layer split — this is still the
alpha-research layer) + §3 (invariants: determinism, no look-ahead).
**Grounding:** the new north-star research mandates exactly this completion set:
- [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md)
  §9.1 (*"Add the high-value BRAIN operators not yet in the registry … `ts_zscore`, `ts_backfill`,
  `ts_av_diff`, `winsorize`, `sigmoid`, `normalize`, and especially `trade_when` … these are what real
  consultants actually use"*), §9.3 (the delay-0/delay-1 knob), §9.6 (*"Design the VM to evaluate a batch of
  alphas, not one at a time"*), §5.2 (the exact paper operator semantics — the conformance spec).
- [`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
  §9.3 (cheap fan-out evaluation of thousands of formulaic alphas is the precondition for the combiner),
  §9.5 (one unified, deterministic system — the VM must feed the *same* loop as everything else).

**Roadmap position:** this is a **Phase 3 extension**, inserted between Phase 3 and Phase 4 on the
[`ROADMAP.md`](ROADMAP.md). It does **not** introduce combination/risk (that is Phase 4); it makes the
*signal layer* complete and correct, and it builds the bridge + the per-alpha streams Phase 4 needs as
inputs. Sequencing rationale (the user's): *"before we start implementation [of] optimizations, combiners,
etc., make sure the signal-generation VM engine is complete."*
**Upstream (atx-core):** all required layers landed (same posture as Phase 3 at its base — L5 `simd`, L6
`stats`, L9 `series`, L3/L2/L1/L0). **Not** blocked-on anything; targets fully green.
**Prerequisite (cross-phase):** Phase 3 (P3-0…P3-9) must **close** first — 3b/3c build on the as-built
`alpha::Dag`, `alpha::Program`, `alpha::Engine`, `alpha::SignalSet`, and the reference oracle. If Phase 3 is
mid-flight when 3b opens, the ledger records which Phase-3 SHAs it builds on.

**Sprint split (decided at design):** two sprints (scope ~10 units > the 4–7/sprint guidance; the `p0`
4/4b/4c precedent):

- **Sprint 3b — vocabulary + variadic args + conformance** (P3b-0 … P3b-4): the operator layer is complete
  and proven against the paper.
- **Sprint 3c — mass evaluation + VM→loop bridge** (P3c-0 … P3c-4): batch evaluation, the per-alpha streams,
  and the `VmSignalSource` bridge.

Each sprint opens its own ledger (`phase-3b-progress.md`, `phase-3c-progress.md`) at its own kickoff; this
plan is the frozen design for both.

---

## 1. What this sprint delivers

After Phase 3b/3c, the VM is a complete signal generator that (a) speaks the operators real WorldQuant
consultants use, (b) is proven against the paper's own alpha set, (c) evaluates a *batch* of alphas cheaply
and hands back the per-alpha PnL/position streams the combiner needs, and (d) actually drives the Phase-2
loop:

```cpp
namespace dsl = atx::engine::alpha;

// 1. The vocabulary now covers the BRAIN-superset (3b): variadic scale(x), trade_when, sigmoid, ...
dsl::Library lib;
auto program = dsl::compile({
  "alpha_a = scale(rank(ts_corr(close, volume, 10)))",                 // scale(x) -> a=1 default (P3b-1)
  "alpha_b = trade_when(volume > adv20, -delta(close, 1), abs(returns) > 0.1)",  // event-gated (P3b-3)
  "alpha_c = normalize(sigmoid(ts_zscore(vwap, 20)))",                 // BRAIN ops (P3b-2)
}, lib);
ATX_TRY(program);

// 2. Batch evaluation over ONE shared cross-alpha DAG (3c): N alphas in one pass.
dsl::Engine eng(panel, lib);
const dsl::SignalSet sigs = eng.evaluate_batch(*program);              // N alphas x dates x instruments (P3c-1)

// 3. Per-alpha PnL/position streams — the Phase-4 AlphaStore feed (3c).
const dsl::AlphaStreams streams = dsl::extract_streams(sigs, weight_policy, panel, sim);  // (P3c-2)
//   streams.pnl(i)        -> alpha i's net periodic return stream
//   streams.positions(i)  -> alpha i's per-date target weights

// 4. The VM finally drives the real Phase-2 loop (3c): VmSignalSource is live.
#define ATX_ENGINE_HAS_ALPHA_VM
loop::VmSignalSource src{std::move(*program), std::move(eng)};         // green-gated (P3c-3)
src.set_delay(loop::Delay::Next);                                      // delay-0 / delay-1 knob (P3c-3)
// ... loop.run(src, ...) — the seam Phase 2 always anticipated.
```

Concretely the phase ships:

**Sprint 3b:**
1. **Variadic / default / optional arguments** in the registry + parser + type-checker — so the real
   mined-alpha forms parse (`scale(x)` with `a=1` default; `group_neutralize(x, g[, cap])`; variadic
   `min`/`max`). This resolves the Phase-3 P3-2 deferred residual and is the foundation the new ops sit on.
2. **The BRAIN-superset operators** the research names as consultant staples: element-wise (`sigmoid`,
   `tanh`, `normalize`, `winsorize`), rolling (`ts_zscore`, `ts_backfill`, `ts_av_diff`, `ts_quantile`,
   `ts_scale`, `ts_count_nans`), and group (`group_count`, `group_mean`, `group_scale`). Each is one registry
   row + one opcode + one vectorized kernel + a differential test against the oracle.
3. **`trade_when` + stateful operators** — `trade_when(trigger, alpha, exit)` (the most-used BRAIN op: a
   *conditional recurrence* that holds the prior signal until an exit fires) and `hump` (a turnover-damping
   recurrence). These reuse the recurrence machinery `ema`/`wma` already established in Phase 3; they are
   causal by construction (read only the past + their own prior state).
4. **An Alpha101 conformance battery** — implement the expressible subset of the 101 alphas as a fixture
   suite and validate the VM against reference semantics, *resolving* the disputed conventions Phase 3 left
   as residuals: `indneutralize` (demean vs regression-residual), `signedpower` (`sign(x)·|x|^a`), `ts_{O}`
   non-integer window `floor(d)`, and the per-op NaN/`min_periods` policy.

**Sprint 3c:**
5. **A batch / cross-alpha evaluation API** (`Engine::evaluate_batch`) — compile and evaluate a *set* of
   alphas over **one shared hash-consed DAG** (the cross-alpha CSE that Phase-3's P3-4 designed but whose
   lever Phase 3's metric table left empty), emitting a `SignalSet` matrix, and **recording the measured
   unique/total-node ratio** on a mined-style battery — the throughput evidence.
6. **Per-alpha PnL/position stream extraction** (`extract_streams`) — run each batch-evaluated alpha's signal
   through the Phase-2 `WeightPolicy` + a lightweight cost-aware P&L accounting to emit the **PnL stream** and
   **position stream** per alpha. This is the precise, typed Phase-3 → Phase-4 handoff: it is exactly what
   `combine::AlphaStore`, the gates, and the combiner consume.
7. **The `VmSignalSource` green-gate + the delay-0/delay-1 knob** — define `ATX_ENGINE_HAS_ALPHA_VM`, wire
   `alpha::Program`/`alpha::Engine` into the Phase-2 `ISignalSource` seam, and write its red→green test
   (resolving the Phase-2 P2-3 cross-phase residual); plus an explicit **delay** execution-timing knob
   (delay-0 = trade on this bar's close, delay-1 = next bar's open; default delay-1, the safe case) — made an
   *execution/loop* concern, not baked into the alpha expression (WorldQuant §9.3).

**Not** in this phase (deliberate — the user's "before optimizations" boundary + the Phase-4 line):
- **No JIT, no computed-goto dispatch, no SIMD micro-tuning beyond what Phase 3 shipped** — pure performance
  optimization is explicitly out (the vectorized interpreter is the baseline; JIT stays future-work).
- **No combiner, no gates, no risk model, no optimizer** — that is Phase 4. 3b/3c stop at *complete signals +
  the streams Phase 4 ingests*.
- **No full BRAIN op zoo** (100+ ops, multi-region universes, vector/matrix ops) — ship the consultant-used
  subset; the registry makes any later op a one-row add.
- **No new general-purpose primitives in the engine** — kernels reuse atx-core L5/L6 (anti-roadmap #7).

**Maps to research:** WorldQuant §5.2 (operator semantics = conformance spec), §9.1 (the op list + the
element-wise/time-series `min/max` distinction), §9.3 (delay knob), §9.6 (batch eval); RenTech §9.3 (cheap
fan-out is the combiner precondition), §9.5 (one deterministic system).

---

## 2. Reference architecture (what extends, what's new)

Phase 3b/3c **extends the existing Phase-3 pipeline** — it does not rebuild it. The registry was explicitly
designed so a new operator is *one row + one opcode + one kernel*; the DAG was designed to hash-cons across
*all* alphas. 3b/3c realize that latent capacity and add the bridge.

```
  Phase-3 pipeline (unchanged spine)                        Phase 3b/3c additions
  ┌────────────────────────────────────────────┐
  │ source strings → lex → Pratt parse → AST     │   P3b-1: parser fills DEFAULT/VARIADIC args
  │   → typecheck (shape/dtype/lookback)          │           (scale(x)→scale(x,1); min/max variadic)
  │   → hash-consed DAG (CSE) → bytecode          │   P3b-2/3: NEW OpCodes (sigmoid…trade_when…)
  │   → vectorized columnar VM → SignalSet        │           + kernels + oracle entries
  └───────────────────────┬──────────────────────┘
                          │  one alpha → one signal vector (Phase 3)
                          ▼
   ┌─────────── BATCH path (P3c-1) ───────────────────────────────────────────┐
   │ compile_batch(N strings) → ONE shared DAG (cross-alpha CSE) → evaluate_batch│
   │ → SignalSet[N × dates × instruments];  record unique/total-node ratio        │
   └───────────────────────┬───────────────────────────────────────────────────┘
                          │  N signal streams
                          ▼
   ┌─────────── STREAMS (P3c-2) ──────────────────────────────────────────────┐
   │ per alpha i:  signal → WeightPolicy → cost-aware P&L → pnl[i], positions[i] │
   │ AlphaStreams  ───────────────────────────────────────────▶ (Phase-4 input) │
   └───────────────────────┬───────────────────────────────────────────────────┘
                          ▼
   ┌─────────── BRIDGE (P3c-3) ───────────────────────────────────────────────┐
   │ #define ATX_ENGINE_HAS_ALPHA_VM                                            │
   │ VmSignalSource{Program, Engine} : ISignalSource   + Delay knob (0/1)       │
   │ ──────────────────────────────────────────▶ Phase-2 BacktestLoop (unchanged)│
   └───────────────────────────────────────────────────────────────────────────┘
```

| Phase-3b/3c piece | Reference (research / open-source) | What we borrow / extend |
|---|---|---|
| Variadic/default args | WorldQuant `scale(x, a=1)`, `group_neutralize(x, g[, cap])` (WQ §5.2) | trailing-default fill at parse; min/max arity in the registry |
| BRAIN element-wise ops | WorldQuant BRAIN operator catalogue (WQ §9.1) | `sigmoid`/`tanh`/`normalize`/`winsorize` as element-wise opcodes |
| Rolling BRAIN ops | BRAIN `ts_zscore`/`ts_backfill`/`ts_av_diff`/`ts_quantile` (WQ §9.1); Qlib rolling | rolling kernels on the existing O(1)/cell accumulator pattern (L6) |
| `trade_when` / `hump` | BRAIN `trade_when(trigger, alpha, exit)` (WQ §9.1) | conditional recurrence; reuses the `ema`/`wma` state-carrying kernel mechanism |
| Conformance battery | Alpha101 (Kakushadze, arXiv:1601.00991) Appendix A + ref impl `yli188/WorldQuant_alpha101_code` (WQ §5.2/§5.3) | implement the expressible subset; pin the disputed semantics against the paper |
| Batch / cross-alpha DAG | Zipline Term-memoization; WorldQuant mass-evaluation (WQ §9.6); DSL deep-dive §5.3 | one DAG across N alphas (already designed in P3-4); the batch API + measured lever |
| Per-alpha PnL streams | WorldQuant per-alpha Sharpe/turnover/fitness (WQ §9.5) | signal → WeightPolicy → P&L; the typed Phase-4 input |
| `VmSignalSource` bridge | the Phase-2 `signal_source.hpp` seam (already written, compile-guarded) | flip `ATX_ENGINE_HAS_ALPHA_VM`; the adapter the header specifies |
| delay-0/delay-1 | WorldQuant delay convention (WQ §9.3) | execution-timing knob on the loop/source, not the expression |

> **Build-order note.** 3b (vocabulary + conformance) ships before 3c (mass eval + bridge) because a *correct*
> operator set is the prerequisite for trustworthy batch evaluation and for any signal the loop consumes.
> Proving the VM against the paper (P3b-4) is the gate that says "the signals are right" before we wire them
> into P&L (3c).

---

## 3. Performance & correctness model (carry Phase-3's, add the new concerns)

Phase 3b/3c inherit **every** Phase-3 rail (vectorized dispatch, hash-consed DAG/CSE, causal-by-construction
time-series ops, fixed NaN/`min_periods` policy, fixed rank tie-break + reduction order, zero hot-path
allocation, differential-vs-oracle correctness). The new concerns:

### 3.1 Stateful operators stay causal (P3b-3)

`trade_when`/`hump` carry **per-instrument state across dates** (the prior emitted value). This is the same
recurrence shape as `ema`/`wma`, which Phase 3 already evaluates as a forward scan. The rails:
- State is advanced **strictly forward** in date order; the kernel reads only `[≤t]` data + its own
  `state[t−1]`. A negative/forward reference is impossible by construction (there is no API to read `state[>t]`).
- State lives in a **pooled per-op buffer** sized `n_instruments` (no per-cell allocation); reset at the
  panel's first date.
- **Truncation-invariance still holds** (P3b-4/P3c-4): output at `t` is byte-identical whether or not rows
  `>t` exist — a stateful op cannot peek forward because the scan hasn't reached `>t` yet.

### 3.2 Conformance ≠ bit-equality with Python (P3b-4)

The Alpha101 reference impls are Python/pandas; exact bit-equality is neither achievable nor the goal. The
battery asserts **documented-tolerance** agreement (a stated ULP/relative tolerance per alpha for float
reductions) **and** pins the *semantic* choices (which `indneutralize`, which `signedpower`, `floor(d)`,
NaN/`min_periods`) so the engine's behavior is *defined*, not accidental. Where the paper is ambiguous, the
engine picks one variant, documents it, and the battery locks it.

### 3.3 Batch evaluation is the CSE lever made measurable (P3c-1)

Cross-alpha CSE is already structural (P3-4's one global DAG). The batch path's job is to (a) expose the
N-alpha entry point and (b) **record the measured** `unique-nodes / total-AST-nodes` ratio and cache-hit %
on a mined-style battery (high subexpression overlap) — the evidence Phase-3's metric table left as `—`.
Determinism extends unchanged: the batch result is independent of alpha submission order (sort by stable
alpha id before the ordered-reduction hash).

### 3.4 The bridge preserves determinism + no-look-ahead (P3c-3)

`VmSignalSource::evaluate(panel)` is **pure in the panel** (the Phase-2 `ISignalSource` contract): same panel
→ same `SignalView`, no hidden state, returns a borrowed view over the Engine's pooled slot (no per-call
allocation). The **delay** knob is an *execution* setting on the loop/source — it shifts *when* a computed
signal is acted on (this bar's close vs next bar's open), never *what data the signal reads*. The structural
no-look-ahead firewall (sealed `RollingPanel` rows, decide-`t`/fill-`t+1`) is unchanged and still dominant;
delay-0 is the *less* conservative choice and is opt-in.

### 3.5 Performance budgets (record measured; no invented figures)

| Path | Budget (intent) | Notes |
|---|---|---|
| new element-wise opcode (`sigmoid`/`tanh`/…) | ≤ ~1–2 ns/cell amortized | contiguous f64 loop; same as Phase-3 element-wise |
| new rolling opcode (`ts_zscore`/`ts_quantile`/…) | O(1)/cell where possible | reuse L6 rolling/online_stats accumulators |
| `trade_when`/`hump` (stateful) | O(1)/cell | one forward scan; per-instrument state slot |
| `evaluate_batch` (N alphas) | report **unique/total nodes** + cache-hit % | the cross-alpha CSE evidence (fills the P3-4 table) |
| `extract_streams` (N alphas) | O(N · dates · WeightPolicy) | cold-ish (research cadence); allocate-once per alpha acceptable |
| `VmSignalSource::evaluate` | zero hot-path allocation | borrowed pooled slot; the seam contract |

Benches report measured numbers with host/build/panel-shape context only (audit §5 warning).

---

## 4. Variadic / default arguments — the registry change (P3b-1, foundational)

Phase 3's `OpSig` carries a single `u8 arity`. 3b generalizes it to an arity *range* with trailing defaults,
the minimal change that admits the real forms without touching every consumer:

```cpp
namespace atx::engine::alpha {

struct OpSig {
  std::string_view name;
  atx::u8 min_arity{};            // REQUIRED operand count (was: arity)
  atx::u8 max_arity{};            // max operand count; == min_arity for fixed-arity ops
  OpCode  opcode{OpCode::Const};
  DType   out_dtype{DType::F64};
  bool    lookahead_safe{true};
  // Trailing defaults for optional args: defaults[k] is the literal value supplied
  // for argument (min_arity + k) when the call omits it. Length == max_arity - min_arity.
  // A NaN sentinel means "no scalar default" (the op's kernel handles absence, e.g.
  // group_neutralize's optional cap).
  std::array<atx::f64, kMaxDefaults> defaults{};
  Shape (*shape_of)(std::span<const Shape> args){nullptr};
};

}
```

**Parser change (P3b-1):** when a call site supplies `k` args with `min_arity ≤ k ≤ max_arity`, the parser
appends `Literal` nodes for the missing trailing args from `OpSig::defaults` (so the DAG/VM see a fully-applied
call — no special-casing downstream). `k < min_arity` or `k > max_arity` is a parse error (the existing
arity-mismatch path, now a range check).

**Type-checker change (P3b-3 of Phase 3 is unaffected):** the shape/dtype rules already operate on the
*materialized* arg list; after default-fill the arg count is fixed, so the checker is unchanged except it
validates the range, not a single number.

**Concrete rows enabled:**
- `scale(x, a=1)` → `min_arity=1, max_arity=2, defaults={1.0}` (matches the paper's default `a=1` and the
  existing `WeightPolicy::gross_normalize`).
- `group_neutralize(x, g[, cap])` → `min_arity=2, max_arity=3, defaults={NaN}` (NaN cap ⇒ "no cap"; the
  kernel reads the sentinel).
- `min(x, y, …)` / `max(x, y, …)` element-wise variadic → handled as left-folded binary `MinP`/`MaxP` at
  parse (a fold, not a true n-ary opcode), so the VM stays binary. (Documented: the 101-Alphas `min/max`
  *with a window* are time-series `ts_min/ts_max`; the variadic element-wise form is the BRAIN one — the
  shape lattice already disambiguates, WQ §5.2.)

This is the single most foundational 3b unit: the new ops in P3b-2/P3b-3 assume it.

---

## 5. The new operators (semantics, reproduced for the implementer)

### 5.1 Element-wise (P3b-2) — new `OpCode`s `Sigmoid`, `Tanh`, `Normalize`, `Winsorize`

| Operator | Arity | Shape | Semantics | NaN |
|---|---|---|---|---|
| `sigmoid(x)` | 1 | P→P | `1 / (1 + exp(−x))` | NaN→NaN |
| `tanh(x)` | 1 | P→P | `std::tanh(x)` | NaN→NaN |
| `normalize(x)` | 1 | P→V | cross-sectional demean (`x − mean_cs(x)`); optional std-divide knob (default demean only) | exclude NaN from mean |
| `winsorize(x, std=4)` | 1–2 | P→V | clamp cross-section to `mean ± std·σ` (in-formula outlier clamp; default `std=4`) | exclude NaN |

`normalize`/`winsorize` are cross-sectional (`P→V`, `shape_cross_section`); `sigmoid`/`tanh` element-wise
(`shape_unary`). All built-ins remain `lookahead_safe`.

### 5.2 Rolling (P3b-2) — new `OpCode`s `TsZscore`, `TsBackfill`, `TsAvDiff`, `TsQuantile`, `TsScale`, `TsCountNans`

| Operator | Arity | Semantics (trailing window `[t−d+1, t]`) |
|---|---|---|
| `ts_zscore(x, d)` | 2 | `(x − ts_mean(x,d)) / ts_std(x,d)` — rolling standardize |
| `ts_backfill(x, d)` | 2 | last valid (non-NaN) value within the trailing-`d` window (fills gaps causally; never forward) |
| `ts_av_diff(x, d)` | 2 | `x − ts_mean(x, d)` (deviation from trailing mean) |
| `ts_quantile(x, d)` | 2 | rolling empirical quantile (default median) of the trailing-`d` window |
| `ts_scale(x, d)` | 2 | rolling min-max scale: `(x − ts_min(x,d)) / (ts_max(x,d) − ts_min(x,d))` |
| `ts_count_nans(x, d)` | 2 | count of NaNs in the trailing-`d` window (data-quality signal) |

All are causal (trailing window only; `floor(d)` for non-integer per the paper); reuse the L6
rolling/online_stats accumulators for O(1)/cell where possible (`ts_zscore`/`ts_av_diff` from the rolling
mean/var; `ts_quantile`/`ts_scale` from a rolling order-stat/deque).

### 5.3 Group (P3b-2) — new `OpCode`s `CsCountG`, `CsMeanG`, `CsScaleG`

| Operator | Arity | Semantics (per date, per group) |
|---|---|---|
| `group_count(x, g)` | 2 | number of valid members in each group (broadcast to members) |
| `group_mean(x, g)` | 2 | mean of `x` within each group (broadcast to members) |
| `group_scale(x, g)` | 2 | scale within each group so `Σ|x| = 1` per group |

Group `P→V`, `DType` arg2 = `Group` (reuse the Phase-3 group-op dtype rail). Operate over valid+universe
mask, NaN-fill excluded (Phase-3 CS policy).

### 5.4 Stateful conditional recurrence (P3b-3) — new `OpCode`s `TradeWhen`, `Hump`

**`trade_when(trigger, alpha, exit)`** — the most-used BRAIN op (WQ §9.1). Per instrument, forward scan:
```
out[t] = NaN              if exit[t]  > 0      ; close / no position
       = alpha[t]         elif trigger[t] > 0  ; (re)enter with the new signal
       = out[t-1]         else                 ; HOLD the prior signal (the recurrence)
out[first_date] = (trigger>0 && !(exit>0)) ? alpha : NaN
```
`trigger`/`exit` are masks (or `>0` truthy panels). Stateful: carries `out[t−1]` per instrument. Causal:
reads only `[≤t]` + own prior state. This is the canonical event-gated alpha (e.g. "take a mean-reversion bet
only when volume spikes, hold until volatility breaks").

**`hump(x, threshold=0.01)`** — turnover damping. Per instrument:
```
out[t] = x[t]             if |x[t] − out[t-1]| > threshold
       = out[t-1]         else                                 ; suppress small changes (hold)
```
Reduces churn (a turnover lever); stateful, causal. Both reuse the `ema`/`wma` state-carrying kernel
mechanism Phase 3 established (a per-op `state[n_instruments]` pooled slot, advanced forward).

---

## 6. The conformance battery + semantic resolutions (P3b-4)

Implement the **expressible subset** of the 101 alphas (those built only from shipped operators — after 3b
that is the large majority) as fixed fixtures, and assert the VM matches reference semantics within a
documented tolerance. The battery is also where the disputed conventions get **pinned**:

| Disputed semantic (Phase-3 residual) | Resolution (engine choice, locked by the battery) |
|---|---|
| `indneutralize(x, g)` demean vs regression-residual | **Demean within group** (paper Appendix A: *"cross-sectionally demeaned within each group"*) is `indneutralize`; the regression-residual variant is the separate `group_neutralize`. Both already distinct opcodes — the battery confirms `indneutralize` = demean. |
| `signedpower(x, a)` | **`sign(x)·|x|^a`** (the paper's practical convention, WQ §5.2), not `x^a`. The oracle uses this; `power(x,a)` stays `x^a`. |
| `ts_{O}(x, d)` non-integer `d` | **`floor(d)`** (paper: *"non-integer `d` is `floor(d)`"*) — mined constants are routinely fractional (e.g. `8.22237`). Applied at compile (the window literal is folded then floored). |
| per-op NaN / `min_periods` | One policy table (Phase-3 §3.4), confirmed by the battery: full window required for `stddev`/`ts_corr`; partial allowed for `ts_mean`/`ts_sum`; documented per op. |
| `min/max` element-wise vs time-series | element-wise `min(x,y)` = `MinP`; windowed `min(x,d)`/`ts_min(x,d)` = `TsMin` — disambiguated by shape (WQ §5.2). The battery includes both forms. |

**Battery contents (examples that are expressible after 3b):** Alpha#4 `(-1*ts_rank(rank(low),9))`; Alpha#101
`((close-open)/((high-low)+.001))`; Alpha#6 `(-1*correlation(open,volume,10))`; plus a curated set covering
`rank`/`ts_rank`/`correlation`/`delta`/`decay_linear`/`scale`/`signedpower`/`indneutralize`. Each fixture: a
hand- or reference-computed expected output on a fixed synthetic panel, asserted within tolerance. Alphas that
need unshipped data fields or ops are listed as **out-of-battery** with the reason (no silent omission).

---

## 7. Cross-cutting gates (agent.md — every unit)

Identical to Phase 3 §7 (the standard atx bar): TDD failing-test-first; `/W4 /permissive- /WX` +
clang-tidy/format clean; ASan+UBSan every run (TSan on any parallel batch eval where supported, else
race-clean + Linux residual); `Result<T>`/`Status` for expected failures, never throw for control flow;
exhaustive `enum class` switches over `OpCode` (no `default`) — **every new opcode forces a /W4 warning at
each switch until its kernel is added** (the exhaustiveness rail is how we know no op is half-wired); weakest
sufficient type at interfaces; functions ≤ ~60 lines; `// SAFETY:` on every deviation; zero hot-path
allocation on the eval path; **every new opcode is matched bit-for-bit (documented ULP tolerance) against the
tree-walking oracle** (the Phase-3 P3-5 differential discipline — extended with an oracle entry per new op).

The **new** rails specific to 3b/3c:
- **Stateful-op causality (§3.1):** `EXPECT_DEATH`/assert there is no API to read `state[>t]`; truncation-
  invariance test per stateful op.
- **Variadic default-fill (§4):** a call omitting an optional arg parses to the *same DAG node* as the
  explicit form (`scale(x)` ≡ `scale(x,1)`) — assert structural equality.
- **Conformance tolerance (§3.2):** each battery alpha documents its tolerance; no loosening to force a pass.
- **Bridge purity (§3.4):** `VmSignalSource::evaluate` is pure in the panel + zero-alloc; the delay knob
  changes *timing*, not *data* (a delay-0 vs delay-1 test asserts identical signals, different fill bars).

---

## 8. Unit decomposition

Dispatch **sequential** (each consumes the prior's headers). TDD red→green→refactor within each. Sprint 3b
(P3b-0..P3b-4) completes the vocabulary + proves it; Sprint 3c (P3c-0..P3c-4) adds mass eval + the bridge.
None are upstream-blocked; all build on the as-built Phase-3 VM.

---

## SPRINT 3b — Vocabulary + variadic args + conformance

### P3b-0 — Sprint scaffold + ledger  *(marker commit)*

**Scope.** Open the Phase-3b ledger (`phase-3b-progress.md`) per [`../docs/sprint.md`](../docs/sprint.md);
freeze scope; record the Phase-3 base SHAs this sprint extends. No operator logic. (No new namespace —
everything lives in the existing `atx::engine::alpha`.)

**Acceptance.** Ledger marker commit `docs(p3b-0): open phase-3b sprint ledger` with `Base: <branch> @ <SHA>`
and the Phase-3 close SHA recorded as the prerequisite. Build stays green.

---

### P3b-1 — Variadic / default / optional arguments

**Scope.** Generalize `OpSig` arity to `[min_arity, max_arity]` + trailing `defaults`; make the parser
default-fill omitted trailing args; range-check arity (§4). The foundation for the new ops.

**Files.** Modify `include/atx/engine/alpha/registry.hpp` (the `OpSig` struct + every built-in row + the
`kMaxDefaults` constant); modify `include/atx/engine/alpha/parser.hpp` (call-parsing: default-fill + range
check); `tests/src/alpha_registry_test.cpp` + `alpha_parser_test.cpp` (extend).

**Algorithm.** §4. Built-in rows that were fixed-arity get `min_arity == max_arity` (no behavior change).
`scale` → `(1,2,{1.0})`; `group_neutralize` → `(2,3,{NaN})`. Parser: on a call, look up `OpSig`; if
`min ≤ k ≤ max`, append `Literal(defaults[k−min])` for each missing arg (skip the NaN sentinel for ops whose
kernel handles absence); else `ParseError` (arity range).

**Tests.** `scale(x)` parses ≡ `scale(x,1)` (same DAG node — structural equality); `scale(x,a)` still works;
`group_neutralize(x,g)` and `(x,g,cap)` both parse; too-few args → `ParseError`; too-many → `ParseError`;
every existing fixed-arity op unchanged (regression: the full Phase-3 parser suite stays green). Boundary:
op with all-optional args; the NaN-sentinel default path.

**Acceptance.** `/W4 /WX` clean; green; Phase-3 parser/registry suites unchanged. Commit
`feat(p3b-1): variadic/default operator arguments`.

---

### P3b-2 — BRAIN element-wise + rolling + group operators

**Scope.** Add the consultant-staple ops (§5.1–5.3): `sigmoid`, `tanh`, `normalize`, `winsorize` (element-
wise/CS); `ts_zscore`, `ts_backfill`, `ts_av_diff`, `ts_quantile`, `ts_scale`, `ts_count_nans` (rolling);
`group_count`, `group_mean`, `group_scale` (group). Each = one `OpCode` + one registry row + one VM kernel +
one oracle entry + differential tests.

**Files.** Modify `registry.hpp` (new `OpCode`s + rows), the VM kernel header(s) (`alpha/vm.hpp` /
`alpha/kernels_*.hpp` as built in Phase 3), the oracle (`alpha/reference.hpp` / P3-5), and add
`tests/src/alpha_ops_brain_test.cpp`.

**Implementation.** Element-wise: tight contiguous f64 loops (sigmoid/tanh), CS demean/clamp over the valid
mask (normalize/winsorize). Rolling: reuse L6 rolling/online_stats (`ts_zscore` from rolling mean/var,
`ts_quantile`/`ts_scale` from a rolling order-stat/deque, `ts_backfill` = last-valid scan), `floor(d)`
windows, per-op `min_periods`. Group: per-group reductions over the valid+universe mask, NaN-fill excluded.
Every new opcode added to the exhaustive VM + oracle switches (the /W4 rail surfaces any unwired op).

**Tests (differential + known-value).** `sigmoid`/`tanh` vs `std` on a fixture; `normalize` ⇒ mean≈0 over
valid set; `winsorize` ⇒ no value beyond `mean±std·σ`; `ts_zscore` vs hand mean/std on a ramp; `ts_backfill`
fills a NaN gap with the last valid (and **only** from the past — truncation-invariant); `ts_quantile` median
on a known window; `group_mean` per-group mean broadcast; `group_scale` ⇒ `Σ|x|=1` per group. **Every new op
matched against the oracle bit-for-bit (ULP tolerance).** Boundary: all-NaN window; 1-instrument universe;
single-member group; `d=1`.

**Acceptance.** `/W4 /WX` clean; differential-green; Phase-3 suites unchanged. Commit
`feat(p3b-2): brain-superset element-wise/rolling/group ops`.

---

### P3b-3 — `trade_when` + stateful conditional-recurrence ops

**Scope.** Add `trade_when(trigger, alpha, exit)` and `hump(x, threshold=0.01)` (§5.4) — stateful causal
recurrences reusing the `ema`/`wma` state-slot mechanism. The highest-value consultant op.

**Files.** `registry.hpp` (`OpCode::TradeWhen`, `OpCode::Hump` + rows; `trade_when` arity 3, `hump` arity
1–2 via P3b-1 defaults), the VM kernel header (state-carrying kernels), the oracle, and
`tests/src/alpha_trade_when_test.cpp`.

**Implementation.** A per-op pooled `state[n_instruments]` slot, reset at the panel's first date, advanced in
the forward date scan (the existing recurrence pattern). `trade_when`: the 3-way select of §5.4 (exit→NaN,
trigger→alpha, else hold prior). `hump`: hold-unless-moved-enough. `trigger`/`exit` consume Mask dtype (reuse
the Phase-3 mask rail). **SAFETY** comment on the state-slot reuse. The recurrence reads only `state[t−1]` +
`[≤t]` inputs — no forward reference exists.

**Tests (known-value + lookahead).** `trade_when`: a hand-traced 5-date fixture (enter on trigger, hold
through quiet days, close on exit, re-enter) ⇒ exact expected per-date output; first-date behavior;
all-trigger ⇒ = alpha; all-exit ⇒ all NaN. `hump`: small changes suppressed, large changes pass; threshold
boundary. **Lookahead:** truncating the panel after `t` leaves outputs `≤t` byte-identical (the scan can't
reach `>t`). Determinism: same input ⇒ identical state evolution. Boundary: 1 date; 1 instrument; NaN in
trigger/alpha/exit.

**Acceptance.** `/W4 /WX` clean; differential-green; truncation-invariance asserted. Commit
`feat(p3b-3): trade_when + stateful recurrence ops`.

---

### P3b-4 — Alpha101 conformance battery + semantic resolutions + close

**Scope.** Implement the expressible subset of the 101 alphas as fixtures, validate the VM against reference
semantics within documented tolerance, and **pin** the disputed conventions (§6). The "VM is complete &
correct" proof. Then the sprint-3b close.

**Files.** `tests/src/alpha_conformance_test.cpp` (the battery + fixtures); doc updates to the operator-table
appendix noting the locked semantics; the ledger.

**Design.** For each expressible alpha: a fixed synthetic panel, an expected output (hand-computed for simple
ones; cross-checked against the `yli188/WorldQuant_alpha101_code` reference semantics for complex ones, with
the divergence/tolerance documented). Assert VM output within the per-alpha tolerance. Lock: `indneutralize`
= demean; `signedpower` = `sign(x)·|x|^a`; `floor(d)` windows; the `min_periods` table; element-wise vs
time-series `min/max`. Out-of-battery alphas listed with the reason (missing field/op) — no silent omission.

**Tests.** the battery itself (one `TEST` per alpha or per semantic resolution); a meta-test asserting the
out-of-battery list is exhaustive (every 101 alpha is either in-battery or listed with a reason).

**Sprint 3b close (sprint.md ceremony).** Lift residuals (out-of-battery alphas; any rolling op left at
O(d)/cell pending an L6 accumulator; ULP tolerances to revisit) to the ROADMAP backlog; update the ROADMAP
Phase-3b status table (rows ✅); bump `Last reviewed`; write "What 3b proves / Next" (baton → 3c); merge
`--no-ff` (`merge: phase-3b — vm vocabulary + conformance`). **Do not push unless asked.**

**Acceptance.** Battery green within tolerance; semantics documented + locked; close items done. Commit
`docs(p3b-close): close phase-3b — 5 units, <M> tests`.

---

## SPRINT 3c — Mass evaluation + VM→loop bridge

### P3c-0 — Sprint scaffold + ledger  *(marker commit)*

**Scope.** Open the Phase-3c ledger; freeze scope; record the 3b close SHA as the base. No logic.

**Acceptance.** Marker commit `docs(p3c-0): open phase-3c sprint ledger` with `Base: <branch> @ <SHA>`.

---

### P3c-1 — Batch / cross-alpha evaluation API + CSE metrics

**Scope.** Expose `Engine::evaluate_batch` — compile and evaluate a *set* of alphas over the **one shared
hash-consed DAG** (the cross-alpha CSE already built in P3-4), emitting a `SignalSet` matrix; and **record the
measured** cross-alpha `unique/total-AST-node` ratio + cache-hit % on a mined-style battery (fills the empty
Phase-3 P3-4 metric table).

**Files.** `include/atx/engine/alpha/engine.hpp` (the `evaluate_batch` entry + `SignalSet` matrix accessors),
`tests/src/alpha_batch_test.cpp`, `bench/alpha_batch_bench.cpp`.

**Implementation.** `compile({...N strings...})` already folds into one DAG (P3-4); `evaluate_batch` runs the
linearized program once and scatters each `StoreAlpha` root into `SignalSet[alpha_id]`. Determinism:
**sort alphas by stable id** before the ordered-reduction hash so the batch result is independent of
submission order. Record the lever: `unique_nodes`, `total_ast_nodes`, ratio, cache-hit %.

**Tests (differential + determinism + metric).** N alphas evaluated as a batch == each evaluated singly
(cell-for-cell); two alphas sharing `rank(close)` ⇒ one `CsRank` node (assert `unique < total`); batch result
order-independent (shuffle submission order ⇒ identical hash); the metric is recorded (non-zero ratio on an
overlapping battery). Boundary: batch of 1 (== single eval); fully-disjoint alphas (ratio ≈ 1); fully-shared
(ratio small).

**Acceptance.** `/W4 /WX` clean; differential-green; CSE metric recorded (measured, host/build noted).
Commit `feat(p3c-1): batch cross-alpha evaluation + cse metrics`.

---

### P3c-2 — Per-alpha PnL / position stream extraction (the Phase-4 feed)

**Scope.** `extract_streams(SignalSet, WeightPolicy, panel, ExecutionSimulator) → AlphaStreams` — turn each
alpha's signal into the **PnL stream** + **position stream** the Phase-4 `combine::AlphaStore`, gates, and
combiner consume. The precise, typed Phase-3 → Phase-4 handoff.

**Files.** `include/atx/engine/alpha/streams.hpp` (`AlphaStreams` + `extract_streams`),
`tests/src/alpha_streams_test.cpp`.

**Data structure.**
```cpp
namespace atx::engine::alpha {
// Per-alpha realized streams over the panel's rebalance dates. Dense, id-aligned to the SignalSet.
struct AlphaStreams {
  // pnl_   [n_alphas x n_periods]   : alpha i's net periodic return (its own dollar-neutral book)
  // pos_   [n_alphas x n_periods x n_instruments] : alpha i's per-date target weights
  [[nodiscard]] std::span<const atx::f64> pnl(atx::usize alpha) const noexcept;
  [[nodiscard]] std::span<const atx::f64> positions(atx::usize alpha, atx::usize period) const noexcept;
  [[nodiscard]] atx::usize n_alphas() const noexcept;
  [[nodiscard]] atx::usize n_periods() const noexcept;
};
// For each alpha, for each rebalance date: signal -> WeightPolicy::to_target_weights -> positions;
// realized return = Σ_i w_i[t-1] * instrument_return_i[t] minus modeled cost (ExecutionSimulator).
[[nodiscard]] atx::core::Result<AlphaStreams>
extract_streams(const SignalSet&, const WeightPolicy&, const PanelView&,
                const exec::ExecutionSimulator&);
}
```

**Contract.** Reuses the *existing* Phase-2 `WeightPolicy` (winsorize→rank/zscore→dollar-neutral→scale) and a
lightweight cost-aware return accounting (the Phase-2 cost model) — **no new portfolio logic**; this is the
glue that produces the streams Phase 4 ingests. Cold-ish (research cadence); allocate-once-per-alpha is
acceptable (the `WeightPolicy` precedent). The position stream is what enables Phase-4's deferred
position-based combiner.

**Tests (known-value).** a single alpha with a known signal ⇒ hand-computed weights + realized PnL (matches a
direct Phase-2 loop run on that alpha — a differential check against the real loop); a 2-alpha set ⇒ two
independent streams; costs-off recovers the frictionless return; NaN signal ⇒ 0 weight (no opinion).
Boundary: flat signal ⇒ flat PnL; 1 period; 1 instrument.

**Acceptance.** `/W4 /WX` clean; green; the single-alpha stream matches a direct Phase-2 loop run. Commit
`feat(p3c-2): per-alpha pnl/position stream extraction`.

---

### P3c-3 — `VmSignalSource` green-gate + delay-0/delay-1 knob

**Scope.** Resolve the Phase-2 P2-3 cross-phase residual: define `ATX_ENGINE_HAS_ALPHA_VM`, wire
`alpha::Program`/`alpha::Engine` into the `loop::VmSignalSource` adapter the Phase-2 `signal_source.hpp`
already specifies, write its red→green test; add the **delay-0/delay-1** execution-timing knob.

**Files.** `include/atx/engine/loop/signal_source.hpp` (un-guard `VmSignalSource`; the body is already written
behind `#if defined(ATX_ENGINE_HAS_ALPHA_VM)`), the build (`CMakeLists.txt`: define the macro where the alpha
VM is available), a new `loop::Delay` enum + the loop's fill-timing, and `tests/src/vm_signal_source_test.cpp`
+ a loop-integration test.

**Implementation.** The adapter body exists (Phase-2 froze it): `evaluate(panel)` runs `engine_.run(program_,
panel)` and returns the borrowed alpha column; `max_lookback()` forwards `program_.max_lookback()`. 3c
*activates* it (macro + include + the alpha VM target as built in Phase 3) and proves it. **Delay knob:**
`enum class Delay : u8 { Same, Next }` (delay-0 = fill on this bar's close; delay-1 = next bar's open;
default `Next`). The knob lives on the loop/source (execution timing), **not** the alpha expression — the
*same* program runs delay-0 or delay-1 (WQ §9.3). The structural no-look-ahead firewall is unchanged;
delay-0 is the opt-in, less-conservative timing.

**Tests.** `VmSignalSource` wraps a compiled program + engine and produces the same signal as a direct
`Engine::evaluate` (the adapter is a thin pass-through); it runs in the **real `BacktestLoop`** producing
deterministic fills (the cross-phase integration that was blocked since Phase 2); **delay-0 vs delay-1**: the
*signals* are identical, the *fill bars* differ by one (a delay-0 fill on bar `t`, a delay-1 fill on `t+1`),
and delay-1 stays the default. Determinism: byte-identical replay. Boundary: a program with lookback `d`
sizes the loop's `RollingPanel` to `d`.

**Acceptance.** `/W4 /WX` clean; green; the VM drives the real loop; delay knob proven. Commit
`feat(p3c-3): vm signal source green-gate + delay knob`.

---

### P3c-4 — Integration · batch determinism · CSE evidence · bench · close

**Scope.** The proof for 3c + the sprint close. An integration suite proving batch determinism, the
cross-alpha CSE lever, the end-to-end VM→loop bridge, and the per-alpha streams feeding a (stub) Phase-4
consumer; then the bench; then the close.

**Design.**
- **Batch determinism:** a multi-alpha batch over a synthetic panel (incl. delisted symbols + NaN gaps) —
  twice, and single- vs multi-thread where parallel — hash the ordered `(alpha_id, date, instrument,
  value-bits)`; identical hashes; mutation (reorder alphas / perturb a value / add a late row) flips it.
- **CSE evidence:** record the cross-alpha unique/total-node ratio + cache-hit % on a mined-style battery
  (high overlap) — the throughput claim, measured.
- **Bridge E2E:** a compiled program runs through `VmSignalSource` → `BacktestLoop` → `Portfolio`, cost-honest
  and deterministic (costs-off recovers the frictionless equity); delay-0 and delay-1 both run.
- **Phase-4 readiness:** `extract_streams` output feeds a trivial consumer (e.g. compute one alpha's Sharpe)
  to prove the stream shape is exactly what Phase 4's `AlphaStore`/metrics expect.

**Bench.** `evaluate_batch` (alphas/s, ns/cell, CSE ratio), `extract_streams` (alphas/s), `VmSignalSource`
in-loop overhead. Measured only; host/build/panel recorded.

**Sprint 3c close (sprint.md ceremony).** Lift residuals (position-based combiner is Phase-4; parallel batch
Linux-TSan pass; any remaining O(d)/cell rolling op; computed-goto/JIT stay future-work); update ROADMAP
Phase-3b status table (3c rows ✅) + resolve the "event-driven vs hybrid" / "VmSignalSource blocked" notes;
bump `Last reviewed`; write "What Phase 3b/3c proves / Next sprint priorities" (baton → Phase 4); write/extend
`phase-3.md` user reference with the new ops + batch API + delay knob + the `VmSignalSource` usage; merge
`--no-ff` (`merge: phase-3c — mass eval + vm→loop bridge`). **Do not push unless asked.**

**Acceptance.** Integration/determinism/bridge suites green; CSE + bench recorded; close items done; ledger +
ROADMAP reconciled. Commit `docs(p3c-close): close phase-3c — 5 units, <M> tests`.

---

## 9. Sub-agent dispatch checklist (per unit)

Each brief includes (per [`../docs/sprint.md`](../docs/sprint.md)): worktree path + branch
(`worktree-phase-3b-vocab` / `worktree-phase-3c-bridge`, or in-place per the established engine workflow —
record in the ledger); the unit's scope **quoted** from this plan; acceptance criteria; the verbatim
*"Marker-commit pattern is mandatory: commit before stopping or work is lost."*; commit format
`feat(p3b-N)` / `feat(p3c-N)`; predecessor SHAs (incl. the Phase-3 close SHA); the ledger-row instructions;
and the handoff block below.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx):
Governed by .agents/cpp/agent.md (safety-critical-grade C++20). This phase EXTENDS the Phase-3 alpha VM — use
the as-built Phase-3 headers (alpha/registry.hpp, parser.hpp, typecheck.hpp, dag.hpp, bytecode.hpp, vm.hpp,
reference.hpp/oracle, engine.hpp) and atx-core (error.hpp, types.hpp, stats/*.hpp, series/*.hpp, simd.hpp) as
the positive style + API reference: module-level intent comment, grouped types/APIs, explicit
ownership/lifetime/error contracts, concise comments that explain invariants — never narrate code. Do NOT
rebuild the Phase-3 pipeline; add ops/args/API/bridge to it.

A new operator is ONE registry row + ONE OpCode + ONE vectorized kernel + ONE oracle entry. Every enum-class
switch over OpCode is EXHAUSTIVE (no default) — a new opcode FORCES a /W4 warning at each switch until its
kernel + oracle entry are added; that is the rail that proves no op is half-wired. Every fast-VM opcode must
match the tree-walking reference oracle bit-for-bit (documented ULP tolerance for float reductions).

Correctness rails are structural: time-series + STATEFUL ops (trade_when/hump) are causal — they read only the
trailing window and their own prior state, advanced forward in date order; there is NO API to read state[>t];
a negative/forward window is a COMPILE error. Cross-sectional ops operate over the valid+universe mask and
propagate NaN (never impute). floor(d) for non-integer windows. signedpower = sign(x)|x|^a. indneutralize =
demean within group; group_neutralize = regression-residual. Rank tie-break + reduction order are FIXED for
bit-reproducibility. Determinism: NO RNG; batch results are independent of alpha submission order (sort by
stable id before hashing).

The delay-0/delay-1 knob is an EXECUTION/loop concern (when a signal is acted on), NOT part of the alpha
expression (the same program runs both); it never changes what data the signal reads. VmSignalSource::evaluate
is PURE in the panel and ZERO-alloc (borrowed pooled slot). The eval hot path allocates nothing; parsing/
compiling/fitting and stream extraction are cold paths and may allocate.

No UB, no narrowing, no uninitialized vars, no owning raw pointers. const/constexpr/noexcept/[[nodiscard]]
where they hold. Expected failures (parse/arity-range/shape/unknown-field) -> Result<T>/Status, never throw.
Weakest sufficient type at interfaces (span/string_view). Functions <= ~60 lines. // SAFETY: on every
deviation (state-slot reuse, slot reinterpret, unchecked index in a bounds-proven loop).

TDD: failing GoogleTest first, watch it fail for the right reason, then implement. Cover happy path, boundaries
(empty/1-instrument/d=1/d>history/all-NaN/single-group/single-date), error paths (arity range, shape, unknown
field, negative lookback), invariant violations (EXPECT_DEATH; truncation-invariance for stateful ops). Build
/W4 /WX clean; clang-tidy/format clean; tests pass under ASan+UBSan. Phase-3 suites must stay green per unit.

A unit is done only when header + impl + tests + ledger row + build/test gate are all present. No TODO stubs,
no fake success paths, no untested skeletons. Prefer a smaller complete slice over a larger partial one.
```

---

## 10. Risks / watch-items

- **Stateful-op look-ahead (the #1 new risk).** `trade_when`/`hump` carry cross-date state; a careless kernel
  could read forward. Mitigation: the forward-scan structure (state advances with the date loop; `state[>t]`
  is unreachable) + the truncation-invariance test per stateful op (P3b-3). Treat any state access not
  indexed by the current scan position as a defect.
- **Conformance over-fitting to a reference impl.** The `yli188` Python repo is itself an interpretation, not
  the paper. Pin semantics to the **paper** (Appendix A) where they disagree; document each divergence + its
  tolerance (P3b-4). Don't chase bit-equality with pandas (different float order) — that's not the goal.
- **Variadic-arg blast radius.** Changing `OpSig::arity` → `min/max` touches every built-in row + the parser.
  Mitigation: fixed-arity ops get `min==max` (no behavior change) + the full Phase-3 parser/registry suites
  stay green as the regression guard (P3b-1).
- **Rolling-op O(d)/cell regression.** `ts_quantile`/`ts_scale` are tempting to write as a per-cell re-scan.
  Use the L6 rolling order-stat/deque for O(1)/cell where possible; if an op must ship O(d)/cell, say so in
  the ledger (a tracked residual), don't hide it.
- **Batch determinism vs submission order.** The combiner downstream must see order-independent results — sort
  by stable alpha id before the reduction hash (P3c-1). A test shuffles submission order and asserts an
  identical hash.
- **The bridge depends on Phase-3 close.** `VmSignalSource` needs the as-built `alpha::Engine::run` signature.
  If Phase 3 ships a different signature than the Phase-2 header assumed, update the adapter to the *as-built*
  API and note the delta in the ledger (the plan is a fossil; the disruptor + datetime work both proved the
  spec maps cleanly, but verify).
- **`extract_streams` ≠ a second portfolio.** It must reuse the Phase-2 `WeightPolicy` + cost model, not
  re-implement P&L. The guard: the single-alpha stream matches a direct Phase-2 loop run bit-for-bit
  (P3c-2). If it diverges, the glue is wrong, not the loop.
- **Scope creep into Phase 4.** Gates, metrics-as-admission, the combiner, and the risk model are Phase 4.
  3b/3c stop at *complete signals + the streams Phase 4 ingests*. `extract_streams` produces the streams; it
  does **not** gate, combine, or size them.
- **Performance temptation.** JIT, computed-goto, SIMD micro-tuning are explicitly out (the user's "before
  optimizations" boundary). The vectorized interpreter is the baseline; record benches, don't tune.
- **LSP false positives** without `compile_commands.json` — verify against real `cmake --build`; ignore
  phantom noise (every prior phase saw this).

---

## 11. Definition of done (Phase 3b/3c)

- **Sprint 3b:** P3b-0…P3b-4 implemented; the BRAIN-superset ops (sigmoid/tanh/normalize/winsorize, ts_zscore/
  ts_backfill/ts_av_diff/ts_quantile/ts_scale/ts_count_nans, group_count/mean/scale, trade_when, hump) +
  variadic/default args ship; **every new opcode matched against the oracle** (documented ULP tolerance); the
  Alpha101 conformance battery is green within tolerance and the disputed semantics are pinned + documented.
- **Sprint 3c:** P3c-0…P3c-4 implemented; `evaluate_batch` runs N alphas over one shared DAG with the
  measured cross-alpha CSE ratio recorded; `extract_streams` produces per-alpha PnL/position streams that
  match a direct Phase-2 loop run; `VmSignalSource` is green-gated and **drives the real `BacktestLoop`**
  (the Phase-2 P2-3 residual is resolved); the delay-0/delay-1 knob is proven.
- `/W4 /permissive- /WX` clean; clang-tidy (project policy) + format clean; tests pass under ASan+UBSan
  (TSan on parallel batch eval where supported).
- **Determinism** proven (batch order-independence; byte-identical replay; mutation detected). **Lookahead**
  proven (truncation-invariance, incl. stateful ops). NaN/universe policy enforced.
- **CSE lever** evidenced (cross-alpha unique/total-node ratio + cache-hit % recorded); benches recorded with
  host/build context (measured only).
- Zero steady-state allocation on the eval + `VmSignalSource::evaluate` hot paths (instrumented).
- Both ledgers closed; ROADMAP Phase-3b status + `Last reviewed` updated; `phase-3.md` user reference extended
  (new ops + batch API + delay knob + `VmSignalSource`); worktrees merged `--no-ff` (not pushed unless asked);
  the "VmSignalSource blocked-on Phase 3" cross-phase note on the ROADMAP is resolved.

---

## Appendix A — New operator vocabulary (added to the Phase-3 ISA)

Built on the Phase-3 registry (Appendix A there). Shapes: **S** scalar, **V** cross-section, **P** panel. All
new built-ins are lookahead-safe (causal); stateful ops are causal recurrences.

| Operator | Arity | Shape | Semantics | OpCode | Unit |
|---|---|---|---|---|---|
| `scale(x, a=1)` | 1–2 | P,S→V | rescale so Σ\|x\|=a (default a=1) — **default arg** | `CsScale` | P3b-1 |
| `group_neutralize(x, g[, cap])` | 2–3 | P,group[,S]→V | regression-residual neutralize — **optional arg** | `CsNeutG` | P3b-1 |
| `sigmoid(x)` | 1 | P→P | `1/(1+exp(−x))` | `Sigmoid` | P3b-2 |
| `tanh(x)` | 1 | P→P | `tanh(x)` | `Tanh` | P3b-2 |
| `normalize(x)` | 1 | P→V | cross-sectional demean | `Normalize` | P3b-2 |
| `winsorize(x, std=4)` | 1–2 | P,S→V | clamp CS to `mean ± std·σ` | `Winsorize` | P3b-2 |
| `ts_zscore(x, d)` | 2 | P,S→P | rolling `(x − ts_mean)/ts_std` | `TsZscore` | P3b-2 |
| `ts_backfill(x, d)` | 2 | P,S→P | last valid in trailing-d window (causal) | `TsBackfill` | P3b-2 |
| `ts_av_diff(x, d)` | 2 | P,S→P | `x − ts_mean(x,d)` | `TsAvDiff` | P3b-2 |
| `ts_quantile(x, d)` | 2 | P,S→P | rolling empirical quantile (default median) | `TsQuantile` | P3b-2 |
| `ts_scale(x, d)` | 2 | P,S→P | rolling min-max scale | `TsScale` | P3b-2 |
| `ts_count_nans(x, d)` | 2 | P,S→P | count NaNs in trailing-d window | `TsCountNans` | P3b-2 |
| `group_count(x, g)` | 2 | P,group→V | valid-member count per group | `CsCountG` | P3b-2 |
| `group_mean(x, g)` | 2 | P,group→V | mean within group | `CsMeanG` | P3b-2 |
| `group_scale(x, g)` | 2 | P,group→V | scale Σ\|x\|=1 within group | `CsScaleG` | P3b-2 |
| `trade_when(trigger, alpha, exit)` | 3 | mask,P,mask→P | event-gated conditional recurrence (hold prior until exit) | `TradeWhen` | P3b-3 |
| `hump(x, threshold=0.01)` | 1–2 | P,S→P | turnover-damping recurrence (hold unless moved > threshold) | `Hump` | P3b-3 |

---

## Appendix B — Source index

| Source | What we took | Primary reference |
|---|---|---|
| WorldQuant deep-dive §9.1 | the BRAIN-superset op list (`ts_zscore`, `ts_backfill`, `ts_av_diff`, `winsorize`, `sigmoid`, `normalize`, **`trade_when`**) | [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) |
| WorldQuant deep-dive §5.2 | exact operator semantics (the conformance spec): `signedpower`, `floor(d)`, `indneutralize` demean, `min/max` shape disambiguation | 101 Formulaic Alphas, arXiv:1601.00991 Appendix A |
| WorldQuant deep-dive §9.3 | delay-0/delay-1 as an execution knob, not part of the expression | BRAIN delay convention |
| WorldQuant deep-dive §9.6 | batch evaluation (a set of alphas, not one at a time); subexpression sharing is the lever | mass-evaluation framing |
| RenTech deep-dive §9.3/§9.5 | cheap fan-out is the combiner precondition; one deterministic system feeds the same loop | *The Man Who Solved the Market* + audit |
| Alpha101 reference impl | conformance cross-check (with documented divergences) | github.com/yli188/WorldQuant_alpha101_code |
| Phase-3 plan | the VM/registry/DAG/oracle this phase extends | [`phase-3-alpha-expression-dsl-implementation-plan.md`](phase-3-alpha-expression-dsl-implementation-plan.md) |
| Phase-4 plan | the `AlphaStreams` consumer (`combine::AlphaStore`/gates/combiner) | [`phase-4-signal-combination-risk-implementation-plan.md`](phase-4-signal-combination-risk-implementation-plan.md) |
| atx-core L5/L6 | simd kernels; rolling/online_stats accumulators; cross_section | `atx-core/include/atx/core/{simd.hpp,stats/*.hpp}` |

*Full URL index + confidence tags: the WorldQuant + RenTech research deep-dives' Reference sections.*
