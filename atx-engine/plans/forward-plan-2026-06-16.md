# ATX-Engine — Code Review + Forward Plan vs. the RenTech / WorldQuant Goal

Date: 2026-06-16
Author: review + roadmap pass
Scope: `atx-engine` (C++ alpha-research / backtest engine), measured against
`research/renaissance-worldquant-deep-dive.md` and `research/renaissance-technologies-systems-deep-dive.md`.
Supersedes nothing; builds on `reviews/atx-engine-code-review-2026-06-14.md` (whose P0/P1 findings are
re-confirmed open below) and the P2 / P3 roadmaps.

---

## 0. Verdict (one paragraph)

The **research/discovery half** of the engine is mature and genuinely in the RenTech/WorldQuant mold:
a WorldQuant-style alpha DSL + VM, a genetic factory with NSGA-II Pareto search, ML/DL signal sources,
a mega-alpha combiner with correlation/turnover gates, a Barra-style factor risk model, a square-root
cost model with temp/permanent impact split, and a strong out-of-sample discipline stack (CPCV,
deflated Sharpe, PBO, lockbox). ~1,700 tests green on `main`. The thesis the research docs describe —
*many weak, weakly-correlated signals → one unified model, scored honestly* — is structurally present.

What is **not** yet true: (1) the execution/cost spine has confirmed **P0 correctness bugs** that
silently bias backtest truth — and at a RenTech 50.75%-hit-rate edge, "a few bps of cost error flips
the sign" (per `renaissance-technologies-systems-deep-dive.md` §9.2.1), so these must close before any
real-data conclusion is trusted; (2) the "one unified system" exists as libraries but has **no
production entry point** — `engine.hpp` is still `int step(int)`, no CLI, no run manifest, risk/borrow/
factor-neutralization not wired into the main loop; (3) the honest-at-scale conclusions RenTech is
famous for (survivorship-free universe, capacity curve as a first-class output) are **documented as
gaps, not yet closed**. The next push is therefore **harden → assemble → validate honestly**, in that
order, before chasing more signal sophistication.

---

## 1. Goal → Built scorecard

Pillars are the nine design mandates from the two research dossiers (WQ deep-dive Part IX; RenTech
deep-dive §9). Status reflects code on `main` + confirmed spot-checks.

| # | Goal pillar (from research) | Built? | Where | Gap that matters |
|---|---|---|---|---|
| 1 | **PIT correctness** — bitemporal, no look-ahead, survivorship/delisting, data-quality gates | 🟡 Strong-but-incomplete | `data/real_panel`, `data/corporate_actions`, `data/adjust`, `RollingPanel` structural gate; real ORATS ingestion (P3 S1) | **Survivorship/delisting recovery deferred** (listed-only universe bias documented, not fixed). Bitemporal restatement for fundamentals not built (price-vol only so far). |
| 2 | **Vectorized research layer** — WQ operator vocab, cheap mass eval | ✅ Strong | `alpha/` (lexer/parser/typecheck/vm/bytecode), BRAIN `ts_*`/`cs_*` ops, `factory/` | Throughput fine; `n_workers` in `search_driver` discarded (single-thread fallback). |
| 3 | **Mega-alpha combination** — N≫T, shrinkage/factor covariance, correlation gates | ✅ Strong | `combine/` (combiner/correlation/gate/metrics/store), `risk/shrinkage`, `learn/` ensemble | Re-screen happens in combine/book, not pre-admit, on some paths. |
| 4 | **Honest cost** — √-impact, temp/perm split, calibration, capacity | 🟡 Built-with-P0-bugs | `exec/execution_sim`, `cost/` (temp_perm/borrow/calibration/capacity) | **P0: capacity unit bug; P0: limit fills through limit; P1: borrow not in main loop.** |
| 5 | **Barra factor risk** — V=XFXᵀ+D, neutralization | 🟡 Built-not-wired | `risk/factor_model`, `exposures`, `specific_risk`, `shrinkage`, `psd_repair`, `stat_factor_model` | **Factor-neutralize not reachable from `BacktestLoop`**; `dead_factor` returns `NotImplemented`; K=1 placeholder factor model in book stage. |
| 6 | **Unified optimizer** — one brain, internal netting | 🟡 Built-with-bug | `risk/optimizer`, `multi_horizon`, `qp_solver`, `fund/meta_book`+`netting`; S8 augmented-QP/ADMM/LDLᵀ in flight | **P0: cap-projection can break dollar-neutrality**; stacked-MPC `NotImplemented`; augmented-QP ~20s/run blocks 3 MHO tests. |
| 7 | **Out-of-sample discipline** — CPCV, deflated Sharpe, PBO, lockbox, walk-forward | ✅ Strong (as tests) | `eval/` (cpcv/deflated_sharpe/pbo/lockbox/regime_slice), `validation/bias_audit` | **Not wired as run-gates** — discipline lives in tests, not the production run path. |
| 8 | **Deterministic sim==live, one code path** | 🟡 Sim-only | deterministic spine + canonical digests (P2 S7) | **No live/paper path; no unified runner** — `engine.hpp` placeholder, no CLI/manifest. |
| 9 | **Capacity / impact realism reporting** | 🟡 Pieces-only | `cost/capacity` curve + root-find | **Capacity not a first-class backtest output**; no multi-size stress; blocked by pillar-4 unit bug. |
| — | *Systems: LMAX Disruptor ring buffer* | ⬜ Not built | `bus/event_bus` (not a pre-allocated ring) | Deferred; only relevant when a latency-bound live/HFT path is opened. Not blocking now. |
| — | *Distributed scale-out* | ✅ Single-box | `parallel/` (shm worker, process executor, scheduler) | N-node network transport deferred (atx-core L4). |

**Reading of the table:** discovery (pillars 2,3,7) is done; the failing/incomplete pillars are all in
the **execution → portfolio → risk → run** spine (4,5,6,8,9) plus the **data honesty** edge (1). That is
exactly where the research docs say the money and the lies both live (RenTech §9.2: cost model is the
"secret weapon"; §9.1: PIT/survivorship is the first durable edge).

---

## 2. The three gaps that actually block progress

Everything sophisticated is already built. The blockers are unglamorous:

**Gap A — Correctness debt in the cost/risk spine (silent backtest corruption).**
Re-confirmed open today against `main`:
- `risk/capacity.hpp:239-240` — participation = `shares / dollar_adv` (shares ÷ dollars); understates
  impact by ≈ instrument price → capacity curves too optimistic.
- `exec/execution_sim.hpp:327-337` (`limit_marketable`) gates on raw `ref`; `emit_fill` (`:374-375`)
  then applies slippage+temp impact, so a buy-limit can fill **above** its limit. Comment admits it's a
  "deferred residual."
- `risk/optimizer.hpp:324-359` + `loop/weight_policy.hpp:246-259` — cap-clip/renorm by absolute mass
  can break `Σw=0`; the prior review found a 4-name case returning net ≈ 0.234 on a "neutral" book.
- `loop/market.hpp:109-132` — absent instruments retain stale `volumes_` → phantom liquidity; an order
  can fill against a delisted/absent name's old volume.
- Permanent-impact mark shift (`execution_sim.hpp:377`) is not reliably re-marked into loop equity
  (`backtest_loop.hpp:318-322`) and is overwritten by the next slice (`market.hpp:119-121`).

These are not cosmetic. Per the engine's own doc, the entire P&L of a weak-signal book "lives in the
last fraction of a basis point of cost accuracy." **Block A must precede any real-data scorecard**, or
P3 S2's honest numbers will be honestly wrong.

**Gap B — No unified production entry point (the "one brain" exists only as parts).**
`engine.hpp` is a placeholder; the only `main` is the shm worker. There is no `run_backtest(config)`,
no CLI, no deterministic run manifest (git SHA / config hash / data manifest / seed). `industry_neutral`,
the Barra factor model, and borrow/financing are implemented and tested but **cannot be driven through
`BacktestLoop`** (`weight_policy.hpp:191-206` needs a `group_map` that `backtest_loop.hpp:283` never
supplies). RenTech's defining systems choice (deep-dive §6.1) is *one* unified model with *one* cost/risk/
data path; the engine has the organs but not the spine that connects them to a reproducible run.

**Gap C — Conclusions aren't honest-at-scale yet.**
Survivorship/delisting recovery is deferred, so today's universe is listed-only (upward-biased). Capacity
is not reported per strategy, so "edge as a function of size" — RenTech's single most important business
fact (deep-dive §5.6, §9.6) — is invisible. OOS gates (PBO/DSR) exist as tests but don't gate a run.

---

## 3. Forward roadmap (sequenced, with rationale)

Order is dependency- and truth-driven, not difficulty-driven. Each block is a sprint-sized unit.

| Block | Name | Why now | Rough size | Unblocks |
|---|---|---|---|---|
| **A** | **Spine Hardening** — fix the 5 confirmed P0/P1 cost/risk correctness bugs, with failing tests first | Every downstream number depends on it; cheapest high-value work in the repo | S (~1 sprint) | trustworthy P3 S2; trustworthy capacity |
| **B** | **Unified Runner** — `run_backtest(BacktestConfig)` + `atx-engine-run` CLI + deterministic manifest; wire group-map/factor-model/borrow into `BacktestLoop`; promote PBO/DSR/bias-audit to run-gates | Turns "parts" into RenTech's "one brain"; makes every later result reproducible & comparable | M (~1–1.5 sprints) | P3 S2 runs through one path; P2 S8 capstone; future live path |
| **C** | **Real-Data Scorecard (P3 S2)** — run the *unchanged* hardened pipeline on the 5,321-symbol real panel; publish stylized-fact baselines + post-deflation/net-of-cost scorecard + caveats as first-class results | The moment of truth: does the factory find real alpha or fit noise? Only meaningful after A+B | M | go/no-go on the whole thesis |
| **D** | **Honest-at-Scale** — survivorship/delisting recovery (kill listed-only bias); capacity curve as a first-class backtest output + multi-size stress | The two things that separate a credible result from a flattering one (deep-dive §9.1, §9.6) | M | defensible external claims |
| **E** | **Capstone + Optimizer Perf (P2 S8)** — orchestrate full pipeline, open lockbox once, publish robust-alpha report; land S8 augmented-QP perf to re-enable the 3 MHO tests | The "done-gate" proof; perf makes the unified optimizer usable at high turnover | M | v3 "Unified Fund" close |
| **F+** | **Frontier** — bitemporal fundamentals + alt-data breadth; live/paper path (sim==live); Disruptor ring buffer; intraday/multi-asset; N-node distributed | Pushes toward RenTech *scale* once the core is honest and assembled | L (multi-sprint) | future capacity |

**The single most important sequencing claim:** do **A before C**. Running the real-data scorecard
(the natural, exciting next step) on top of the capacity unit bug + limit penetration + dollar-neutrality
break would produce a confidently-wrong verdict on the entire thesis. Harden first.

---

## 4. Detailed implementation plan — Block A: Spine Hardening

Discipline: **TDD, test-first.** For each bug, add a failing regression that pins the *correct*
behavior, watch it fail, fix, watch it pass. No behavior change without a red test first. Keep every
existing boundary-pin/golden-digest test green (the P3 S1 digest `0x2a22a873483d9157` and the P2 S7
5-path digest must not move except where a fix intentionally changes fills — if a digest moves, it is
re-pinned with a written justification in the progress ledger).

### A1 — Capacity participation unit bug  *(P0, smallest, do first)*
- **File:** `include/atx/engine/risk/capacity.hpp:234-240`.
- **Bug:** `adv = dollar_adv(...)` (dollars), then `shares = notional/price`, `part = shares/adv` mixes
  shares over dollars.
- **Fix:** `const atx::f64 part = notional / adv;` (notional ÷ dollar-ADV). Delete the `shares`
  intermediate or rename to make the unit explicit.
- **Test (red first):** `capacity_test.cpp` — hand-computed case, price=$50, known volume/AUM/impact
  coeffs, assert expected participation and bps cost; the assertion must fail under the old `shares/adv`.
- **Risk:** capacity curves shift down (less optimistic). Expected and correct.

### A2 — Limit orders fill through their limit  *(P0)*
- **Files:** `execution_sim.hpp` — `limit_marketable` (`:327-337`), `volume_capped_qty` (`:342-358`),
  `emit_fill` (`:362-389`).
- **Bug:** marketability checks raw `ref`; the post-cost `fill_px` (slip+temp) can pierce the limit.
- **Fix:** separate **peek** from **commit**. Compute the candidate post-cost `fill_px` *before*
  consuming bar volume; for a buy-limit reject/cap when `fill_px > limit`, for a sell-limit when
  `fill_px < limit`. `volume_capped_qty` currently mutates the per-bar accumulator (`add_vol_filled`),
  so split it: a pure `peek_fillable()` (no mutation) used for the price test, then `commit_fill()` that
  tallies volume only after the price is accepted. Choose and document the contract: **cap to the limit
  price** vs **reject the slice**. Recommend *reject* (simpler, matches a marketable-limit contract);
  revisit capping only if a strategy needs it.
- **Tests (red first):** buy-limit `ref==limit` with bps/slippage making `fill_px>limit` → no fill (or
  capped fill at limit, per chosen contract); symmetric sell-limit; both again with temp impact on.
- **Risk:** fewer limit fills in some strategies; correct.

### A3 — Cap projection can break dollar-neutrality  *(P0)*
- **Files:** `risk/optimizer.hpp:291-299, 324-359` (`cap_clip_renorm`); mirror in
  `loop/weight_policy.hpp:246-259`.
- **Bug:** clip-then-gross-renorm by absolute mass skews the two sides → `Σw ≠ 0`.
- **Fix:** when dollar-neutral is requested, project the **positive and negative sides separately** onto
  capped simplexes of `gross/2` each, then concatenate (preserves both `Σw=0` and `Σ|w|=gross` and
  per-name caps). If infeasible (caps too tight for the requested gross at the given long/short counts),
  return a typed error rather than silently de-neutralizing. Factor the projection into one shared
  helper used by both `optimizer` and `weight_policy` so they cannot drift.
- **Tests (red first):** the prior review's 4-name capped-neutral case (expect net≈0, was 0.234);
  feasible and infeasible cap cases in *both* `WeightPolicy` and `PortfolioOptimizer`; property test:
  random feasible inputs → `|Σw| < 1e-9` and `max|wᵢ| ≤ cap+1e-9`.

### A4 — Stale bar volume → phantom liquidity  *(P0)*
- **Files:** `loop/market.hpp:109-132`; consumer `execution_sim.hpp:342-357`.
- **Bug:** `volumes_` persists for instruments absent from the current slice; `bar_volume()` returns it
  → fills against liquidity that isn't there (incl. delisted names).
- **Fix:** separate **mark** from **executable volume**. Keep last mark for valuation if needed, but make
  `bar_volume()` return 0 for any instrument not present in the *current* slice. Implement via a
  per-slice epoch counter (`volume_epoch_`): on `update_prices()` bump the epoch and stamp each updated
  instrument; `bar_volume()` returns the stored value only if its stamp == current epoch, else 0. Avoids
  an O(universe) clear each slice.
- **Tests (red first):** bar at t1 has volume, t2 omits the instrument, open order at t2 → no fill;
  delisted final bar fills on the valid final bar but not on a later absent slice.

### A5 — Permanent-impact persistence & equity marking  *(P1, decide-then-implement)*
- **Files:** `exec/execution_sim.hpp:377` (`apply_permanent_impact`), `loop/backtest_loop.hpp:251-259,
  318-322`, `loop/market.hpp:119-121`.
- **Bug:** the perm-impact mark shift lands after `mark_to_market`, is missed by the equity sample, and
  is overwritten by the next slice — so "permanent" impact neither persists nor marks reliably.
- **Decision required (pick one, document it):**
  - **(a) Truly persistent:** carry a per-instrument cumulative impact offset that `Market::update_prices`
    *adds on top of* each incoming mark (offset survives slice updates); re-mark the portfolio **after**
    settlement, before sampling equity. Most faithful to the model.
  - **(b) Not persistent:** drop the mark shift; fold the information-leak cost into the explicit fill
    price/cost path. Simpler, but loses the "your trading moves the mark for the rest of the book" effect.
  - **Recommendation:** (a) — it is the behavior the cost research describes (permanent = lasting,
    information-driven move) and the thing that makes capacity curves bite at size.
- **Tests (red first):** buy that emits perm impact → same-slice post-settlement equity reflects the
  shifted mark; a later slice's incoming mark is offset, not overwritten; a no-impact run is byte-identical.

### A6 — (carry, P1) Spread half/full-spread convention
- **Files:** `loop/market.hpp:73-77` (doc: half-spread) vs `execution_sim.hpp:391-410` (`spread*0.5/ref`).
- **Fix:** pin one convention (recommend: field is **full** spread, execution crosses half). Rename to
  `full_spread`, drop the inconsistent `*0.5` or the doc, update calibration + tests to the pinned
  contract. Pure definition fix — do it inside A so cost numbers are unambiguous before C.

**Block A exit criteria:** all six items have red→green regressions; full suite green; any moved golden
digest re-pinned with written justification; a one-page note in the progress ledger stating, per fix,
the old vs new behavior and which digests moved and why.

---

## 5. Block B outline (next, so A's fixes are reachable)

Not full task-level yet (open after A lands), but the shape:

1. **`BacktestConfig` struct + `run_backtest(const BacktestConfig&) -> BacktestRunResult`** — the single
   production facade replacing `engine.hpp`'s placeholder. Validates config, builds the collaborator
   graph (feed → panel → signal → weight-policy → exec → portfolio → report), runs, returns result +
   manifest.
2. **`atx-engine-run` CLI** — subcommands `validate-config` and `run-backtest`; emits a deterministic
   **run manifest** (git SHA, config hash, data manifest digest, seed, calendar, build flags, report
   paths). This is the reproducibility spine RenTech's "one model" discipline implies.
3. **Wire the implemented-but-unreachable knobs into `BacktestLoop`:** thread a universe-aligned
   `group_map` (enables `industry_neutral` safely — or reject it at construction when absent); add an
   optional Barra factor-model neutralization step; add a borrow/financing accrual step on a configurable
   schedule with report attribution.
4. **Promote OOS discipline to run-gates:** PBO/DSR thresholds, no-lookahead truncation check,
   survivorship/delisting check, with fail/waive semantics recorded in the manifest. Discipline moves
   from "tested" to "enforced."
5. **Evaluation-grade `BacktestResult`:** equity→returns adapter; Sharpe/Sortino/IR/drawdown/hit-rate/
   turnover/exposure/cost-attribution wired into report output.

Block B turns the engine from a library of correct parts into RenTech's one auditable, reproducible
system — the precondition for trusting C, D, and E.

---

## 6. What this buys, mapped back to the goal

- **A** makes the cost model honest → satisfies the research docs' single loudest mandate (cost fidelity
  dominates at a 50.75% edge).
- **B** assembles the "one unified model, one cost/risk/data path, deterministic, reproducible" system
  (RenTech §6.1, §9.5).
- **C** is the empirical test of the weak-signal thesis on real US equities.
- **D** makes the answer *honest at scale* (survivorship-free, capacity-aware — RenTech §9.1, §9.6).
- **E** closes the v3 capstone proof and makes the unified optimizer fast enough for the high-turnover
  regime the thesis lives in.

Sophistication (more ML, intraday, alt-data, Disruptor, N-node) is **F+** — deliberately last. The
research is unambiguous (RenTech §9.7, Patterson "do the simple things right"): the edge is data + cost +
breadth + discipline, not model exoticism. The engine already has the sophistication; it needs the
honesty and the spine.

---

## 7. Immediate next action

Open **Block A (Spine Hardening)** as a sprint: write the six failing regressions first (A1→A6), then
fix in the order A1, A2, A4, A3, A6, A5 (cheapest/most-isolated first, decision-bearing A5 last). Then
proceed to Block B.
