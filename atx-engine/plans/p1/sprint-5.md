# Sprint S5 — Learned Signals & ML Combiner (user reference)

**Status:** ✅ CLOSED 2026-06-11 (`feat/atx-core-stdlib`, unmerged). **Spec:** [`sprint-5-learned-signals-ml-combiner.md`](sprint-5-learned-signals-ml-combiner.md) · **Plan:** [`sprint-5-learned-signals-ml-combiner-implementation-plan.md`](sprint-5-learned-signals-ml-combiner-implementation-plan.md) · **Ledger:** [`sprint-5-progress.md`](sprint-5-progress.md)

S5 is the engine's **learned-prediction** layer — the first time the pool's constituents *and* its combiner are **learned, not just formulaic**. It completes the discover→predict→combine arc: S3 discovers formulaic alphas, S5 adds learned alphas (linear, gradient-boosted-tree, HMM-regime, alpha-of-alphas stacking) that admit through the **exact same** `ISignalSource` → `library::admit` seam a mined alpha does. One header-only inline subsystem; **no atx-core changes, no edits to `combine`/`eval`/`library`/`factory`/`alpha` source** — S5 is purely additive.

```
atx/engine/learn/   — atx::engine::learn   (PIT features → latent/interactions → linear/GBT/HMM models → stacking combiner → e2e pipeline)
```

**The one fact that defines this sprint:** *a learned model is just an alpha that must survive the same anti-snooping gate.* The engine learns higher-order structure (latent factors, tree interactions, regimes, alpha-of-alphas) **WITHOUT ever learning to look ahead or to fool its own scorer** — every fitted object is trailing-fit / apply-forward (M2), every search winner is **deflated** with N = the ML trial count (M3), and a pure-noise panel admits **nothing**. ML overfits hardest at this N — the p0 anti-roadmap's exact warning — so the ML ban is lifted only *because* the S1 validation spine deflates it.

---

## What shipped (8 units)

| Unit | Header | What it is |
|---|---|---|
| **S5-0** | `learn/fwd.hpp` | Scaffold + the M1–M8 contract doc-block + the atx-core L7 Eigen-link smoke-test (the §0.8 build risk, retired first). |
| **S5-1** | `learn/feature_matrix.hpp`, `learn/train.hpp` | PIT **feature matrix** (each (date×instrument) row; raw panel fields + stored alpha streams as columns), **multi-horizon** forward-return labels `Y[h]`, and the CPCV date-fold scaffold (`date_label_spans`, `expand_date_folds`, `seed_for`). |
| **S5-2** | `learn/latent.hpp` | **Hidden features**: trailing-fit PCA latent factors + top-IC interaction pairs, applied forward (PIT, truncation-invariant). |
| **S5-3** | `learn/elastic_net.hpp`, `learn/linear_alpha.hpp`, `learn/learned_source.hpp` | **Linear learned alpha**: a Pattern-B RNG-free elastic-net (L1+L2 cyclic coordinate descent) + ridge baseline, per-horizon CPCV walk-forward, §0.6 horizon blend, emitted through `LearnedSignalSource : combine::ISignalSource` (M6/M7 zero-alloc evaluate). |
| **S5-4** | `learn/gbt.hpp` (+ `learned_source.hpp`) | **Deterministic histogram GBT learned alpha**: train-quantile bin edges, seeded row/feature subsample, first-max tie-break, genuine-OOF deflation. Captures interactions the linear model can't. |
| **S5-5** | `learn/hmm.hpp` | **Log-space Baum-Welch HMM** + a PIT (forward-only) **regime posterior** (`regime_posterior_at`) — RenTech's noisy-channel heritage. |
| **S5-6** | `learn/ensemble.hpp` | **Stacking mega-combiner** (alpha-of-alphas): the pool alphas' cross-sections are meta-features; a nonlinear base is admitted **only if it beats a linear combination OOS-after-deflation**; regime-conditional weighting via the S5-5 posterior; plugs into the library as a learned alpha. |
| **S5-7** | `learn/pipeline.hpp`, `bench/learn_bench.cpp` | The end-to-end **integration proofs** (determinism, anti-snooping, nonlinear-vs-linear, regime, multi-horizon, worker-invariance), a thin reuse-only pipeline harness, and the micro-bench. |

---

## How a learned alpha enters the pool

A learned model is fit on a `FeatureMatrix`, wrapped as an `ISignalSource`, and admitted exactly like a mined alpha:

```cpp
// 1. Build PIT features (raw fields + stored alpha streams), with multi-horizon labels.
FeatureMatrix fm = build_features(panel, alpha_store, FeatureSpec{...});

// 2. Fit a learned alpha (deterministic for a fixed master_seed). Linear or GBT:
LearnedModel m = fit_linear(fm, aug, LinearAlphaCfg{...});   // or fit_gbt(fm, aug, GbtCfg{...})

// 3. The deflated, genuinely-out-of-fold gate (N = trial_count). A no-edge fit -> dsr <= 0.
f64 dsr = oos_deflated_sharpe(m, fm);

// 4. Emit + admit through the SAME seam a formulaic alpha uses (M6):
LearnedSignalSource src{std::move(m), spec, base_panel};     // ISignalSource, zero-alloc evaluate (M7)
//    ... build a library::AlphaCandidate (prov.expr_source = "learned:...") and library::Library::admit(...).
```

The **stacking** combiner (`fit_stack`) does the same one level up: the pool's alphas are the meta-features, and the nonlinear alpha-of-alphas is admitted only when `oos_dsr_nonlinear > 0 && oos_ic_nonlinear > oos_ic_linear`.

---

## Invariants the harness proves (M1–M8)

- **M1 Determinism** — same data + seed ⇒ **byte-identical** params AND emitted signal; every RNG-bearing unit ships a same-seed-replay test, and the end-to-end digest is worker-count invariant. Seeds derive from a fixed `(master_seed, tag, a, b)` mix (`seed_for`); GBT is a deterministic histogram with first-max tie-breaks; elastic-net is RNG-free; HMM init is seeded; all reductions are order-fixed.
- **M2 Fit/apply firewall** — every fitted object (standardization, PCA basis, coeffs, GBT splits, HMM `(A,B,π)`+posterior, horizon weights) is trailing-fit / apply-forward, **truncation-invariant** (tested).
- **M3 No snooping** — CPCV training + deflated Sharpe with N = the ML trial count; a no-edge panel is **rejected**, a real edge **survives the same bar**, and the nonlinear stack is admitted **only if it beats linear** OOS-after-deflation. Both directions are seed-swept.
- **M4 Differential correctness** — each kernel is bounded against an obviously-correct reference (elastic-net vs atx-core ridge at α=0 + the soft-threshold closed form; GBT stump vs an O(n²) brute-force threshold; HMM forward-loglik == backward-loglik), never a "returned something" check.
- **M5 Pattern B** — the three new kernels (elastic-net CD, histogram GBT, log-space HMM forward-backward) ship engine-local and are recorded as atx-core L7 promotion requests; ridge/PCA/linalg/RNG/hash are consumed from atx-core, never reimplemented.
- **M6 A learned model is just an alpha** — emits via `combine::ISignalSource`, admits via `library::Library::admit`.
- **M7 No hot-path alloc** — a fitted model's `evaluate()` reuses pre-sized scratch (zero per-date heap allocation).
- **M8** — PIT, NaN-verbatim, no survivorship.

---

## Notes & open residuals (→ p1 ROADMAP backlog)

- **Three Pattern-B kernels → atx-core L7**: `elastic_net`, the histogram GBT split-finder, and the log-space HMM forward-backward are atx-core promotion requests (lift-with-tests, not rewrites).
- **Blended-prediction OOS-IC metric**: §0.6 is proven at the *weighting* level (the blend demotes a no-skill horizon); a full "blended prediction's OOS IC strictly beats the best single horizon's" proof needs per-horizon OOF retention + a new metric (deferred).
- **Live re-eval adapter** (§0.1): deferred until a public DSL compile-to-bytecode API exists; `Provenance.expr_source` is record-keeping only.
- **`PoolView`/`worst_corr_to_pool` swap** (M6): consume S4b's pool-corr seam when it lands (no fork).

---

## Baton → S7

S7 (portfolio + lifecycle) operates the combined book over time: it consumes the learned `ISignalSource`s and the `learn::StackingCombiner` as ordinary pool members, runs them through the multi-period optimizer + decay monitor + capacity bound, and retires dead learned alphas through the risk-factor extractor exactly as it does formulaic ones. The **HMM regime posterior** is available to S7's regime-aware rebalancing schedule.
