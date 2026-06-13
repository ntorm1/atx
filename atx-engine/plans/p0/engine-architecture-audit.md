# atx-engine — Architecture Audit (read this first)

**Last reviewed:** 2026-06-01
**Source:** [`../../research/renaissance-worldquant-deep-dive.md`](../../research/renaissance-worldquant-deep-dive.md) (verified multi-agent research) · `.agents/cpp/agent.md` · atx-core stdlib design spec.
**Purpose:** Distill the research into the strategic positioning for the `atx-engine` module (`p0`). This is the doc you read before arguing scope. When the ROADMAP and a feature request disagree, this audit is the tie-breaker.

---

## 1. The thesis the engine is built to exploit

Two firms, one proven idea (research §I–§III, all ✅ verified):

> **A single weak signal is worthless. A very large number of weak, mostly-uncorrelated signals,
> combined into one unified model, produces extraordinary aggregate risk-adjusted return.**

- RenTech: ~50.75% per-trade directional edge → Medallion Sharpe ≈ 6.0 (early 2000s). The edge is
  invisible per-trade; the fortune is **breadth × consistency** (`IR ≈ IC·√BR`).
- WorldQuant: alphas as compact price-volume **formulas-that-are-code**, ~0.6–6.4-day holds, ~15.9%
  mean pairwise correlation, combined into a single **mega-alpha** — not traded individually.

**Engine consequence:** the system's job is not "find a magic signal." It is to (a) evaluate
thousands-to-millions of faint signals cheaply, (b) gate them on orthogonality/turnover, (c) combine
them into one unified target portfolio, and (d) backtest that honestly — point-in-time, bias-free,
with cost modeling that does not lie. Every architectural decision below serves that pipeline.

---

## 2. The two-layer architecture (the central design decision)

The research (§IV, §VIII) converges on a split that this module adopts as its spine:

```
┌──────────────────────── atx-engine (p0) ─────────────────────────┐
│                                                                   │
│  ┌─────────────────────────┐      ┌──────────────────────────┐   │
│  │  ALPHA RESEARCH LAYER    │      │  EXECUTION / BACKTEST     │   │
│  │  (throughput-first)      │      │  SPINE (correctness-first)│   │
│  │                          │      │                           │   │
│  │  columnar Frame eval     │      │  Disruptor event bus      │   │
│  │  formulaic-alpha vocab   │ ───▶ │  DataHandler (PIT replay) │   │
│  │  (rank/zscore/ts_*)      │ alpha│  Strategy → Portfolio     │   │
│  │  corr/turnover gates     │ pool │  ExecutionSim (√-impact)  │   │
│  │  → mega-alpha combiner   │      │  P&L / risk attribution   │   │
│  └─────────────────────────┘      └──────────────────────────┘   │
│         (Phases 3–4)                     (Phases 1–2)              │
└───────────────────────────────────────────────────────────────────┘
              both consume atx-core L0–L9 stdlib
```

- **Execution spine — event-driven, ring-buffer, fidelity-first.** Same code path runs backtest and
  (future) live, so results are reproducible and there is no sim/live skew. This is the RenTech
  "one unified system" discipline. Built on atx-core `concurrent::disruptor` (L4).
- **Alpha research layer — vectorized, columnar, throughput-first.** Cheap fan-out evaluation of
  formulaic alphas across the whole symbol panel. This is the WorldQuant WebSim role. Built on
  atx-core `series::Frame`/`Column` (L9) + `cross_section` (L6) + `simd` (L5).

**Build order rationale:** the spine is the harder *correctness* surface (ordering, look-ahead,
determinism), so it ships first (Phases 1–2). The research layer (Phases 3–4) plugs into a spine that
is already trustworthy.

---

## 3. Non-negotiable invariants (correctness, from research §IV + agent.md)

These are not features to be prioritized — they are properties every phase must preserve.

| Invariant | What it means | Where enforced |
|---|---|---|
| **Determinism** | Same input feed → byte-identical event sequence and fills. | Phase 1 (replay harness P1-6); single-producer bus. |
| **No look-ahead** | A datum is visible only when knowable at decision time. | Phase 1 sim-clock + point-in-time gate (P1-4). |
| **No survivorship bias** | Delisted symbols present with their final/delisting bar. | Phase 1 DataHandler (P1-5); data model. |
| **No data-snooping inflation** | Out-of-sample / walk-forward / deflated-Sharpe; corr gates. | Phase 5 validation; Phase 3 alpha gates. |
| **Honest cost** | √-impact (size-dependent) + spread + latency + partial fills, modeled *before* a strategy is believed. | Phase 2 ExecutionSim; Phase 5 calibration. |
| **No hot-path allocation** | Zero alloc in steady-state event loop. | atx-core arena/pool/fixed types; agent.md §1. |

---

## 4. Cost & risk — what the research mandates (verified)

- **Market impact is concave (√-law).** `impact_bps ≈ Y·σ·(Q/ADV)^δ`, `δ ≈ 0.5` default but
  **configurable 0.45–0.65** (research §V, ✅). For any fund at scale, **impact dominates** total cost;
  explicit costs are near-negligible. A backtest ignoring size-dependent impact misstates cost and
  passes losing strategies. → Phase 2 models it; Phase 5 fits `δ` and splits temporary vs permanent
  (Almgren-Chriss).
- **Barra-style factor risk model** `r = Xf + u`, `V = XFXᵀ + D` (research §VI, ✅). Collapses
  dimensionality (1,600 assets: 1.28M covariances → ~4,095 via ~90 factors) and provides neutralization
  targets. → Phase 4.
- **Singular alpha covariance (N≫T)** is the core combination obstacle (research §II/§VII, ✅).
  Combiner progression: rank-average → Ledoit-Wolf shrinkage / factor-imposed covariance → regularized
  regression. → Phase 4. (atx-core `regression` L7 provides ols/ridge/wls.)

---

## 5. Systems — what the research mandates (verified)

- **LMAX Disruptor ring buffer** is the validated event-bus pattern (research §VIII, ✅): single-producer
  needs no locks/CAS, ~8× throughput vs a queue, a 2023 C++ port ran ~2× a simple queue (t-stat 22.596).
  atx-core already specs `concurrent::disruptor` (L4) — the engine **wraps it**, does not reinvent it.
- **Columnar storage** for the research layer: contiguous `float64[]` per field enables SIMD-friendly
  formulaic-alpha evaluation. atx-core `series` (L9) provides `Column`/`Frame`.
- **Cache-line alignment, pre-allocation, power-of-two rings** — standard low-latency C++ (research
  §8.2), already baked into atx-core `platform`/`aligned`/`ring_buffer`.

> ⚠️ **Refuted figures — do NOT cite as design justification** (research Appendix B): "live costs 10×
> cheaper than TAQ"; Disruptor "52 ns/hop"; cache-warming "90% latency cut"; the IRJET stacking/RF
> ensemble result. Use the *qualitative* wins, not these numbers.

---

## 6. How this maps to atx-core (dependency posture)

The engine is a **consumer** of the atx-core stdlib; it adds zero general-purpose primitives. Every
engine component names the atx-core layer it stands on. As of 2026-06-01 only atx-core **L0** is built
(L1 `decimal` in progress; L2–L9 pending), so engine units declare **Pattern-B blocked-on**
cross-module dependencies (see ROADMAP §cross-module-deps). The engine plan is written against the
**full** atx-core design; green-gate per unit lands as the depended-on atx-core layer merges.

| Engine needs | atx-core layer | Status |
|---|---|---|
| event bus | L4 `concurrent::disruptor`, `spsc/mpmc_queue` | ⏳ pending |
| timestamps, sessions | L8 `time` | ⏳ pending |
| Price/Qty/Notional/Side/Symbol/Bar/Tick | L8 `domain` | ⏳ pending |
| zero-alloc containers | L3 `ring_buffer`,`fixed_vector`,`hash_map` | ⏳ pending |
| arena/pool (no hot-path alloc) | L2 `arena`,`object_pool`,`aligned` | ⏳ pending |
| rank/zscore/winsorize/neutralize | L6 `cross_section`,`rolling`,`online_stats` | ⏳ pending |
| SIMD reductions | L5 `simd` | ⏳ pending |
| ols/ridge/wls, factor math | L7 `linalg`,`regression` | ⏳ pending |
| columnar store | L9 `column`,`frame` | ⏳ pending |
| Result/log/macros/types | L0 (exist) | ✅ done |

---

## 7. Strategic positioning

| Dimension | RenTech (Medallion) | WorldQuant | **atx-engine target** |
|---|---|---|---|
| Signal style | secret ML/kernel ensemble | formulaic price-volume alphas | formulaic + linear/shrinkage combination |
| Combination | one ~500k-LOC unified model | mega-alpha (10⁵–10⁹ alphas) | one unified optimizer over a gated alpha pool |
| Horizon | intraday→multi-day | ~0.6–6.4 days | multi-horizon, daily-bar first; intraday later |
| Cost model | proprietary | gross-of-cost research | √-impact + spread + latency from Phase 2 |
| Infra | petabyte warehouse, one system | WebSim sandbox + alpha pool | single-box two-layer (spine + research) |
| Identity | — | — | **honest, deterministic, bias-free backtester for weak-signal aggregation in US equities** |

This is the section scope-creep argues with. If a proposed feature does not serve *deterministic,
bias-free, cost-honest weak-signal aggregation in US equities*, it does not ship in `p0`.

---

## 8. Provenance & caveats

- Research findings tagged ✅ verified / ⚠️ refuted / 📘 domain-knowledge in the source deep-dive.
  RenTech specifics are secondary-source estimates. WorldQuant alpha stats are gross of cost.
- This audit imports only the ✅-verified and 📘-standard guidance. Refuted figures (§5 warning) are
  explicitly excluded.
- Open questions carried into Phase 5 (research Appendix C): covariance regularization mechanics;
  event-driven vs vectorized verified guidance; signal-decay/horizon-blending math; impact exponent
  choice (0.45–0.65) and temporary-vs-permanent split.
