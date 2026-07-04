# p9 — Tradeable Mega-Book: Live Levers + Capacity/Turnover-Native Search

**Status:** design (brainstorming output, pending user review → writing-plans)
**Date:** 2026-07-04
**Author:** subagent-driven development, atx-engine
**Predecessor:** p8 "Mega-Alpha Activation & Book Assembly" (merged to main @ c7c7b44)
**Northstar:** generate production-quality, **high-Sharpe, high-capacity, low-turnover** alpha DSLs and combine them into a **mega portfolio**.

---

## 1. Problem statement

p8 *built* the mega-book machinery (factor risk model, MetaBook/MetaAllocator, nonlinear + regime combination, robustness battery, cost-in-selection, deflation gates) and wired most of it as opt-in stages. But a code-level inventory of the merged branch (`f149568`) shows the flagship production recipe (`atx-impl/scripts/build-megaalpha-book.ps1 -Profile prod`) sets flags that are **silent no-ops on the real path**:

| Prod flag | Code-confirmed reality |
|---|---|
| `--dead-alpha-factors` | Complete no-op — `build_risk_model(..., dead_lib=nullptr, dead_ids={}, ...)` unconditionally at `stage_optimize.cpp:260,267`. The Kakushadze-Yu crowding-factor de-levering (the built fix for the measured **N_eff = 8.76** crowding collapse) never fires. |
| `--risk-model factor` | Reaches `stage_optimize` only. `run_combine` hardcodes `RiskModelConfig{}` (Diagonal) at `stage_combine.cpp:765,771`; `stage_metabook` has no `RiskModelConfig` parameter at all. The assembled book uses raw/diagonal covariance. |
| `--capacity-curve` | Dead marker; the curve computes whenever `has_volume && report_aum>0` regardless of the flag. |
| GP aim-portfolio trading | `risk::gp_turnover_native_step`/`gp_aim_and_value` built + tested, **zero call sites** in `atx-impl/src`. Live turnover control is a crude linear blend `w := prev + trade_rate·(target − prev)` (`stage_optimize.cpp:191`). |
| capacity / turnover in search | `kMaxObjectives = 7` (`fitness.hpp:183`) has **no capacity slot and no turnover slot**. The GA never optimizes *for* capacity or turnover — only post-hoc screens. |
| `--robustness-battery` | Only 1 of 4 checks (`noise_control`) reachable; `sub_universe`/`alt_neutralization`/`param_perturbation` have no config surface. Prod recipe never sets the flag. |
| ML alpha generation | `learn/{autoencoder_alpha,tcn_alpha,nn_source,learned_source}` built + engine-tested, **zero call sites in generation**. Raw alphas are 100% symbolic-GP over ~85 DSL ops. |

**Consequence:** today a V1 mega-book run would *not* de-crowd, *not* use factor covariance in the book, *not* GP-trade, and *not* optimize for capacity or turnover — regardless of the flags passed. The recipe is a Potemkin book.

**p9 thesis:** *activate the built-but-dead levers so the book actually bites, then add the two first-class objectives the search structurally lacks, then the genuinely-new generation/combination levers.* "Activate, then extend."

Note (already closed this session, on main): p8's `--impact-in-selection` seam was wired (21317c7) and the robustness battery reached the OOS admit path (28ad58b + f0fca68). p9 builds on that state.

---

## 2. Non-negotiable contract (inherited from p8)

Every new capability is **opt-in behind a config field defaulting to today's behavior**, so the no-flag path is **byte-identical**. The pinned goldens MUST stay unchanged with none of the new flags asserted:
`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `FactoryOos.MineIntoOffPathDigestUnchanged`, the `AtxImplDiscover` determinism slice, `LibraryVerdict.AdmitKindEnumFrozenPrefix`.

**Four test classes per opt-in** (the p8 discipline):
(a) off-path byte-identity, (b) on-path RED→GREEN behavioral proof, (c) twice-run determinism, (d) seq==parallel where an admission/eval path is touched.

**Byte-identity verification:** element-wise `std::bit_cast<std::uint64_t>` (matches signed zeros).

**Frozen / untouchable:** `alpha/oracle.hpp`; frozen estimation bodies in `src/*/*.cpp`; append-only enums pinned by frozen-prefix tests.

**Testing style (user directive):** targeted **short deterministic fixtures only — no long-running full-panel computation sweeps.** Synthetic panels serialized to temp `.bin`, small population×generation budgets, single-threaded ctest.

**Commit discipline:** stage explicit paths (never `git add -A`); never push; trailer exactly `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Execution:** subagent-driven development — serial per-sprint implementer → review package → reviewer → fix loop for Critical/Important → ledger. Dedicated `p9` worktree branched from `main`.

---

## 3. Sprint series

Ordering is **activate → extend**: S1–S3 make built machinery fire (cheap, high-ROI, each makes the prod recipe more honest); S4 adds the missing first-class objectives (the "attack the source" lever); S5 adds book-level gates + the first real synthetic-panel numbers; S6 is the greenfield capstone (explicit cut-point if budget tightens); S7 corrects the recipe.

### S1 — Activate crowding defense (dead-alpha-factors live)
- **Goal:** the Kakushadze-Yu dead-alpha-factor de-levering actually fires on the runnable path — the direct counter to the measured N_eff = 8.76 crowding collapse.
- **Roots:** `atx-impl/src/stage_optimize.cpp` (the two `build_risk_model(..., nullptr, {}, ...)` sites ~:260,267), `atx-impl/src/config.{hpp,cpp}`, `atx-impl/src/stage_discover.cpp` (source of the accumulating library dir).
- **Change:** thread the accumulating `library::Library` (the discover library dir already in the pipeline) into `build_risk_model` as `dead_lib` + the admitted `dead_ids`, so crowded holdings-overlap directions are extracted and de-levered. Keep the fail-open contract: `dead_lib==nullptr` (default / empty lib) → byte-identical no-op.
- **Determinism:** no library / empty library → byte-identical (existing fail-open). Populated library → book de-levers crowded directions only when `--dead-alpha-factors` is set.
- **Acceptance tests:** (b) synthetic pool of N alphas sharing one direction → post-wire the crowded direction's exposure drops / portfolio eRank rises vs the no-dead-factor book; (a) empty lib byte-identical to legacy; (c) twice-run; (d) seq==parallel.
- **Northstar:** mega-portfolio de-crowding.

### S2 — Factor covariance reaches combine + metabook
- **Goal:** `--risk-model factor` governs the whole book, not just `stage_optimize`.
- **Roots:** `atx-impl/src/stage_combine.{cpp,hpp}` (`run_combine` Diagonal hardcode ~:765,771), `atx-impl/src/stage_metabook.{cpp,hpp}` (add the deferred `RiskModelConfig`-parameterized `build_metabook_result`/`run_metabook` overload — the p8-S2 documented seam), `config.{hpp,cpp}`.
- **Change:** thread `RiskModelConfig` (built from `cfg.risk_model`) into `run_combine`; add the additive `model_at` Factor overload to metabook (mirror `stage_optimize`'s additive-overload pattern). Diagonal kind stays the default path.
- **Determinism:** Diagonal (default) → combine + metabook digests byte-identical. Factor kind → digests change only when set; more diversified book (max|w| drops, cf. p8-S3-4's 0.164→0.143 precedent).
- **Acceptance tests:** (b) Factor-vs-Diagonal combine digest differs + more diversified; metabook Factor `model_at` PIT-correct (trailing window strictly < step); (a) Diagonal byte-identical; (c) twice-run.
- **Northstar:** the assembled mega-book uses de-noised factor covariance, not raw.

### S3 — Wire Garleanu-Pedersen aim-portfolio trading
- **Goal:** the live book uses the engine's own low-turnover machinery, not the crude linear blend.
- **Roots:** `atx-impl/src/stage_optimize.cpp` (linear `trade_rate` blend ~:191), `risk/garleanu_pedersen.{hpp,cpp}` (call the built `gp_turnover_native_step`/`gp_aim_and_value`), `config.{hpp,cpp}`.
- **Change:** behind `--gp-trading` (inert default = existing linear blend), replace the partial-trade step with the GP aim-portfolio trade: aim in front of the moving Markowitz target, trade partially toward the aim, weighting slower-decaying (higher-autocorrelation) predictors more. Reuse the built engine function; do not re-derive.
- **Determinism:** `--gp-trading` off → existing linear-blend book byte-identical.
- **Acceptance tests:** (b) on a mean-reverting synthetic fixture, GP path yields lower turnover at matched gross Sharpe vs the linear blend; (a) off byte-identical; (c) twice-run; (d) seq==parallel if the optimize path is parallelized.
- **Northstar:** the direct low-turnover lever.

### S4 — Capacity + turnover as first-class NSGA objectives
- **Goal:** the GA *optimizes for* high-capacity, low-turnover alphas at generation time — "attack the source."
- **Roots:** `atx-engine/include/atx/engine/factory/fitness.hpp` (objective vector, `kMaxObjectives` 7→9), `factory/fitness.cpp` (compute the two objective columns), `factory/search_driver.*` (NSGA reads the widened vector only when enabled), `config.{hpp,cpp}`, `stage_discover.cpp`.
- **Change:** add `kObjCapacity` (√-law-impact-derived per-alpha capacity score — reuse `cost::capacity_point`/`risk::capacity`) and `kObjTurnover` (driven by the signal's first-order autocorrelation / alpha-decay half-life — slower decay = better, per GP + the combination-turnover literature). Opt-in via `--capacity-objective` / `--turnover-objective`. **Inert-default byte-identity:** the two columns are computed but excluded from the selection/domination vector unless their flag is set (the widened vector width is gated, so `NsgaSearch.ScalarRaw` golden is unchanged when off).
- **Determinism:** objectives off → NSGA selection byte-identical (goldens hold). On → domination front shifts toward high-capacity/low-turnover.
- **Acceptance tests:** (b) with capacity objective on, a high-ADV/low-impact alpha dominates an equal-Sharpe low-capacity one (front-membership flips); with turnover objective on, a high-autocorrelation (slow-decay) alpha dominates an equal-Sharpe churny one; (a) both off → ScalarRaw + front byte-identical; (c) twice-run; (d) seq==parallel.
- **Northstar:** generate high-capacity, low-turnover alpha DSLs at the source.

### S5 — Book-level gates + capacity-in-optimizer + full robustness surface + synthetic-panel smoke
- **Goal:** the book-level northstar bars (turnover < 0.20/day, capacity > $100M) become measurable + enforceable; produce the first *real* (synthetic-panel) book-level numbers.
- **Roots:** `stage_metabook.cpp` / `stage_optimize.cpp` (book-level netted turnover measure + gate), `risk/optimizer.hpp` (participation-rate cap as a QP constraint — capacity inside construction, not just the report curve), `factory/factory.cpp` + `config.{hpp,cpp}` (expose all 4 battery checks — currently only `noise_control`), `loop/*` (non-zero `BorrowModel` wire so financing cost is non-zero), a new synthetic-panel end-to-end smoke test.
- **Change:** (i) cross-sleeve-netted **book** turnover measurement + opt-in gate (the northstar bar is stated at book level; today only a per-alpha CPCV admission ceiling exists); (ii) participation-rate cap constraint in the optimizer QP; (iii) `--robustness-battery` config surface for `sub_universe`/`alt_neutralization`/`param_perturbation`; (iv) thread a non-default borrow rate. Then a **synthetic-panel** `run_all` smoke that produces a real (if synthetic) scorecard row — the first honest book-level numbers, labeled synthetic. **No long real-panel sweep** — synthetic panel serialized to a temp `.bin`, small budget.
- **Determinism:** every gate/cap/battery-check off → byte-identical; borrow default 0 preserves existing digests.
- **Acceptance tests:** (b) book turnover gate rejects a > threshold book, admits a < threshold one; participation cap bounds max participation on a thin-ADV fixture; each battery check reachable + rejects its constructed artifact; synthetic smoke produces finite, deterministic scorecard numbers; (a) all off byte-identical; (c) twice-run.
- **Northstar:** book-level bars measurable + enforceable; first real evidence.

### S6 — Greenfield capstone: ML alpha source + NCO (explicit cut-point)
- **Goal:** the biggest genuinely-new raw-quality + combination levers. **If budget tightens, this sprint is the cut — S1–S5 + S7 stand alone as a complete series.**
- **Roots:** `learn/{autoencoder_alpha,tcn_alpha}` (built, zero generation call sites), `factory/search_driver.*` / `factory/genome.*` (inject learned seeds into the gen-0 pool), `fund::MetaAllocator` (add NCO), `config.{hpp,cpp}`.
- **Change:** (i) behind `--ml-seeds`, wire the learned autoencoder/TCN alpha source as a deterministic seed provider into the GA gen-0 pool (currently only used in *combination* via Stack, never in *generation*); (ii) NCO (Nested Clustered Optimization) as an opt-in `MetaAllocator` method (HRP/ERC successor). Meta-labeling/triple-barrier noted as future work unless it lands cheaply.
- **Determinism:** off → byte-identical; ML source seeded (no thread/time).
- **Acceptance tests:** (b) ML seeds enter gen-0 deterministically + are valid genomes; NCO allocation == HRP on a single-cluster reduction, more robust on a nested-cluster fixture; (a) off byte-identical; (c) twice-run.
- **Northstar:** learned nonlinear generation + robust hierarchical assembly.

### S7 — Correct the prod recipe (V1-ready)
- **Goal:** the flagship recipe finally does what it claims.
- **Roots:** `atx-impl/scripts/build-megaalpha-book.ps1` + `atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1`.
- **Change:** enable the now-live flags (dead-alpha-lib, factor-cov-in-combine, `--gp-trading`, `--capacity-objective`/`--turnover-objective`, book turnover gate, participation cap, full `--robustness-battery`, `--deflate-selection`); drop or annotate the ex-no-op flags; update the Pester DryRun argv assertions. Leave the actual full-panel V1 run as the operator step.
- **Determinism:** DryRun-only; no execution in-sprint.
- **Acceptance tests:** Pester argv assertions for smoke + prod profiles green.
- **Northstar:** a V1 run would exercise every lever honestly.

---

## 4. Dependencies & ordering

```
S1 (dead-factor wire) ─┐
S2 (factor cov→combine)─┼─→ S5 (book gates + synthetic smoke exercises S1-S4) ─→ S7 (recipe)
S3 (GP trading)      ───┘                                    ↑
S4 (objectives) ─────────────────────────────────────────────┘
S6 (ML-gen + NCO) — independent capstone, slots before S7
```
- S1/S2/S3/S4 are largely independent (different files) → could parallelize planning, but implement **serial** (one git index, SDD rule).
- S5 depends on S1–S4 being live (its synthetic smoke is the first run that exercises them together).
- S7 depends on all prior (it turns their flags on).

## 5. Acceptance bars (series-level)
- Every sprint: pinned goldens byte-identical on the no-flag path; default Unity-ON build links; new tests green.
- S5 synthetic smoke: a finite, deterministic, honest book-level scorecard row (net-of-cost Sharpe, DSR, PBO, book turnover, capacity zero-crossing) — labeled synthetic, not the real V1.
- S7: prod recipe argv exercises dead-factor de-crowding, factor covariance in combine + metabook, GP trading, capacity/turnover objectives, book turnover gate, full battery.

## 6. Risks & mitigations
- **Objective-vector width change (S4)** risks the NSGA golden. Mitigation: gate the vector width behind the flags — width 7 (unchanged golden) when both objectives off.
- **GP trading (S3)** could destabilize turnover if alpha-decay estimates are noisy. Mitigation: short mean-revert fixture proving the turnover win; off-path byte-identity; warm-up fallback to the linear blend.
- **ML-gen (S6)** is the largest/riskiest surface. Mitigation: it's the explicit cut-point; seeded-deterministic; feeds only gen-0 seeds (search still filters via all gates).
- **Book-level turnover gate (S5)** is a new metric with no precedent. Mitigation: measure-before-gate (report the number first, gate opt-in second).

## 7. Explicitly deferred (not in p9)
- Running the real full-panel V1 (operator step; p9 makes it meaningful).
- True GICS industry/sub-industry ingestion (data gap, carried p6→p8).
- Meta-labeling / triple-barrier / sample-uniqueness weighting (greenfield; noted in S6, lands only if cheap).
- `ResearchFast` EvalMode (10–100× throughput) — large infra lever, its own module.
- The two pre-existing `RobustPipelineE2E` failures (confirmed pre-p8; triage separately).
- FINRA short-interest / IV-surface augment stage (needs ORATS-seg/symbology infra).
