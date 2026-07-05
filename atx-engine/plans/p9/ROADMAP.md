# p9 — Tradeable Mega-Book: Live Levers + Capacity/Turnover-Native Search (ROADMAP)

Branch: `feat/p9` (from `main` @ c7c7b44)
Worktree: `C:\atx-wt\p9`
Design spec: `docs/superpowers/specs/2026-07-04-p9-tradeable-mega-book-design.md`
Execution: subagent-driven development, serial, one branch.

**Thesis: "Activate, then extend."** p8 built the mega-book machinery but left much of it inert on the runnable path — the prod recipe sets flags that are silent no-ops. p9 (S1–S3) wires the built-but-dead levers so the book actually de-crowds / uses factor covariance / GP-trades; then (S4) adds capacity + turnover as first-class NSGA objectives — the two the search structurally lacks; then (S5) adds book-level gates + the first real (synthetic-panel) numbers; then (S6, cut-point) the greenfield ML-generation + NCO; then (S7) corrects the prod recipe. **Zero new estimator math — every sprint wires or lightly extends existing, tested engine code.**

---

## The Potemkin-book evidence (code-confirmed, f149568 / c7c7b44)

| Prod flag | Reality | Fixed by |
|---|---|---|
| `--dead-alpha-factors` | no-op: `build_risk_model(..., dead_lib=nullptr, dead_ids={}, ...)` at `stage_optimize.cpp:260,267` | **S1** |
| `--risk-model factor` | reaches `stage_optimize` only; `run_combine` hardcodes Diagonal (`stage_combine.cpp:765,771`); metabook has no `RiskModelConfig` | **S2** |
| GP aim-portfolio trading | `risk::gp_turnover_native_step` built, zero call sites; live control is linear blend (`stage_optimize.cpp:191`) | **S3** |
| capacity / turnover in search | `kMaxObjectives=7` (`fitness.hpp:183`) — no capacity slot, no turnover slot | **S4** |
| `--capacity-curve` dead marker; no book-level turnover gate; only 1/4 battery checks reachable; borrow=0 | post-hoc only; no optimizer capacity constraint | **S5** |
| ML alpha generation (`learn/{autoencoder,tcn}`) | built, zero generation call sites | **S6** |
| prod recipe sets no-ops, omits live flags | `build-megaalpha-book.ps1` | **S7** |

---

## Determinism contract (p9 — paste into every sprint brief)

Every new capability lives behind a config field with an **inert default**, so the no-flag path is **byte-identical**. Pinned goldens MUST stay unchanged with none of the new flags asserted:
`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `FactoryOos.MineIntoOffPathDigestUnchanged`, the `AtxImplDiscover` determinism slice, `LibraryVerdict.AdmitKindEnumFrozenPrefix`.

**Four test classes per opt-in field (mandatory):** (a) off-path byte-identity, (b) on-path RED→GREEN behavioral proof, (c) twice-run determinism, (d) seq==parallel where an admission/eval path is touched.

**Byte-identity:** element-wise `std::bit_cast<std::uint64_t>` (matches signed zeros).
**Untouchable:** `alpha/oracle.hpp`; frozen estimation bodies in `src/*/*.cpp`; append-only enums pinned by frozen-prefix tests.
**Testing (user directive):** short deterministic fixtures only — **no long-running full-panel sweeps.** Synthetic panels serialized to temp `.bin`, small population×generation budgets, single-threaded ctest.
**Build/test wrappers:** self-contained PowerShell wrappers (subagent env does not persist) — DEFAULT `dev` preset (Unity ON). Never trust clangd/LSP squiggles — only wrapper exit 0 + ctest pass counts.
**Commits:** stage explicit paths (never `git add -A`); never push; trailer exactly `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

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

## SHARED CONFIG-FIELD REGISTRY (authoritative — every sprint plan MUST use these exact names)

All fields are inert-default. New CLI flags parse in `atx-impl/src/config.cpp`; fields live in `atx-impl/src/config.hpp` (`RunConfig`) or the named engine config struct. **Append-only** for enums.

| Sprint | Struct / file | New field(s) — exact name : type = inert default | New CLI flag |
|---|---|---|---|
| S1 | `RunConfig` (config.hpp) | `dead_alpha_lib_dir : std::string = ""` (empty ⇒ reuse discover lib dir ⇒ fail-open no-op) | `--dead-alpha-lib-dir` |
| S1 | (reuses p8) `RunConfig.dead_alpha_factors : bool = false` | — gate; when true AND a non-empty library is threaded, dead factors fire | `--dead-alpha-factors` |
| S2 | `stage_metabook` API | new overload `build_metabook_result(..., const risk::RiskModelConfig& risk_cfg)` / `run_metabook(cfg, risk_cfg)`; default overload keeps Diagonal | (reuses `--risk-model`) |
| S2 | `run_combine` (stage_combine.cpp) | thread `risk::RiskModelConfig` from `cfg.risk_model` (no new field — remove the hardcoded `RiskModelConfig{}`) | (reuses `--risk-model`) |
| S3 | `RunConfig` | `gp_trading : bool = false` ; `gp_risk_aversion : f64 = 0.0` (0 ⇒ inert) ; `gp_trade_cost_scale : f64 = 0.0` | `--gp-trading`, `--gp-risk-aversion`, `--gp-trade-cost-scale` |
| S4 | `factory::SearchConfig` (search_driver.hpp) | `capacity_objective : bool = false` ; `turnover_objective : bool = false` | `--capacity-objective`, `--turnover-objective` |
| S4 | `factory/fitness.hpp` | `kObjCapacity = 7`, `kObjTurnover = 8` (append-only; `kMaxObjectives` 7→9); active-width gated so width==7 when both objectives off | — |
| S5 | `RunConfig` | `book_turnover_gate : f64 = 0.0` (0 ⇒ off) ; `participation_cap : f64 = 0.0` (0 ⇒ off) ; `borrow_bps : f64 = 0.0` | `--book-turnover-gate`, `--participation-cap`, `--borrow-bps` |
| S5 | `factory::FactoryConfig` / `eval::BatteryConfig` surface | `robustness_sub_universe : bool = false` ; `robustness_alt_neutralization : bool = false` ; `robustness_param_perturb : bool = false` (noise_control already wired p8) | `--robustness-sub-universe`, `--robustness-alt-neutralization`, `--robustness-param-perturb` |
| S6 | `RunConfig` | `ml_seeds : bool = false` ; `ml_seed_model_dir : std::string = ""` | `--ml-seeds`, `--ml-seed-model-dir` |
| S6 | `fund::AllocatorMethod` (append-only) | `Nco` appended at the END (after existing HRP/ERC/InverseVol) | `--sleeve-method nco` |
| S7 | — | no new fields; recipe wiring only | — |

**Golden-preservation note (S4):** the NSGA objective vector width MUST remain 7 when both `capacity_objective` and `turnover_objective` are false, or `NsgaSearch.ScalarRaw` breaks. Compute the two columns but exclude them from the domination/selection vector unless their flag is set.

---

## The 7 sprints

- **S1 — Activate crowding defense.** Thread the accumulating `library::Library` into `build_risk_model` (`stage_optimize.cpp:260,267`, currently `nullptr`) so Kakushadze-Yu dead-alpha-factor de-levering fires. Fail-open: empty/no library ⇒ byte-identical. Roots: `stage_optimize.cpp`, `config.{hpp,cpp}`, `stage_discover.cpp`. Northstar: de-crowd N_eff=8.76.
- **S2 — Factor covariance → combine + metabook.** Thread `RiskModelConfig` into `run_combine` (kill the Diagonal hardcode `stage_combine.cpp:765,771`) + add the deferred `model_at` Factor overload to metabook. Diagonal default byte-identical. Roots: `stage_combine.{cpp,hpp}`, `stage_metabook.{cpp,hpp}`, `config.*`.
- **S3 — Wire GP aim-portfolio trading.** Behind `--gp-trading`, replace the linear `trade_rate` blend (`stage_optimize.cpp:191`) with `gp_turnover_native_step`/`gp_aim_and_value` (built, zero call sites). Off ⇒ byte-identical. Roots: `stage_optimize.cpp`, `risk/garleanu_pedersen.*`, `config.*`.
- **S4 — Capacity + turnover first-class objectives.** Add `kObjCapacity` (√-law impact capacity score) + `kObjTurnover` (signal first-order autocorrelation / alpha-decay half-life). Gated width. Roots: `factory/fitness.{hpp,cpp}`, `factory/search_driver.*`, `config.*`, `stage_discover.cpp`.
- **S5 — Book-level gates + capacity-in-QP + full battery + synthetic smoke.** Cross-sleeve-netted book turnover gate; participation cap in the optimizer QP; expose the 3 unreachable battery checks; non-zero borrow; a synthetic-panel `run_all` smoke producing the first real (synthetic) scorecard row. Off ⇒ byte-identical. Roots: `stage_metabook.cpp`, `stage_optimize.cpp`, `risk/optimizer.hpp`, `factory/factory.cpp`, `loop/*`, `config.*`, new smoke test.
- **S6 — Greenfield capstone (cut-point): ML alpha source + NCO.** Wire `learn/{autoencoder,tcn}` as a deterministic gen-0 seed source (`--ml-seeds`); NCO as an opt-in `AllocatorMethod`. Off ⇒ byte-identical. Roots: `learn/*`, `factory/search_driver.*`, `factory/genome.*`, `fund::MetaAllocator`, `config.*`. **If budget tightens, cut S6 — S1–S5 + S7 is a complete series.**
- **S7 — Correct the prod recipe (V1-ready).** Update `build-megaalpha-book.ps1` to enable the now-live flags + drop/annotate ex-no-ops; update Pester DryRun assertions. Real full-panel V1 = operator step. Roots: `atx-impl/scripts/build-megaalpha-book.ps1` + its Pester test.

---

## Dependency / ordering

```
S1 (dead-factor wire) ─┐
S2 (factor cov→combine)─┼─→ S5 (book gates + synthetic smoke exercises S1-S4 together) ─→ S7 (recipe)
S3 (GP trading)      ───┘                                          ↑
S4 (objectives) ───────────────────────────────────────────────────┘
S6 (ML-gen + NCO) — independent capstone, slots before S7
```
Implement serial (one git index). S1→S2→S3→S4→S5→S6→S7. S6 may be cut.

---

## Ownership boundaries (per sprint — do not edit another sprint's files)

- **S1:** `stage_optimize.cpp` (the two build_risk_model sites), `config.{hpp,cpp}`, `stage_discover.cpp` (library dir source), tests under `atx-impl/tests/` + `atx-engine/tests/risk/`.
- **S2:** `stage_combine.{cpp,hpp}`, `stage_metabook.{cpp,hpp}`, `config.{hpp,cpp}` (shared — S2 adds only via the registry), tests.
- **S3:** `stage_optimize.cpp` (the trade-blend site only — coordinate with S1's edits: S1 owns the build_risk_model call, S3 owns the partial-trade step; both in stage_optimize.cpp, land S1 first), `risk/garleanu_pedersen.*` (call only, do not edit the frozen body), `config.*`, tests.
- **S4:** `factory/fitness.{hpp,cpp}`, `factory/search_driver.*`, `config.*`, `stage_discover.cpp`, tests under `atx-engine/tests/factory/`.
- **S5:** `stage_metabook.cpp`/`stage_optimize.cpp` (book turnover measure), `risk/optimizer.hpp` (QP cap), `factory/factory.cpp` (battery surface), `loop/*` (borrow), `config.*`, new synthetic smoke test.
- **S6:** `learn/*` (call only), `factory/search_driver.*`/`genome.*` (seed injection), `fund/*` (NCO), `config.*`, tests.
- **S7:** `atx-impl/scripts/build-megaalpha-book.ps1` + `atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1` only.

**Cross-sprint seam (S1↔S3):** both touch `stage_optimize.cpp`. S1 lands the dead-factor `build_risk_model` wire; S3 lands the GP partial-trade step. Land S1 first; S3 rebases on it. Neither edits the other's region.

---

## Anti-roadmap / guardrails (carried from p7/p8)

- **No HMM as the spine** (rentech-structure-mapping refutation, 0-3 vote). Regime is an overlay, never the backbone.
- **No new signals.** p9 activates/combines/constructs; it does not invent alphas (except S6 wiring the *already-built* ML source).
- **No golden re-baseline.** Opt-in only; the no-flag path is byte-identical. The one allowed exception is a documented correctness fix (none currently anticipated in p9).
- **No long sweeps.** Short synthetic fixtures. V1 real-panel run is the operator's, not p9's.
- **Fail-loud, never silent no-op.** Every flag that is set MUST change behavior or emit a loud warning (the p8 `--selection-aum`/battery-OOS lesson). A flag that parses but does nothing is a defect.

---

## Ledger

`sprint-N-progress.md` per sprint (append one line per clean review). This ROADMAP is the cross-sprint source of truth for config-field names + ownership.

---

## RECONCILIATION ADDENDUM (post-plan-writing — AUTHORITATIVE; overrides the registry/ownership above where they conflict)

The 7 plan-writers grounded in real code and surfaced 12 corrections. These are binding; the implementer follows the addendum where it differs from earlier sections.

**R3 (architectural — the load-bearing one): `--metabook` SUBSTITUTES `run_optimize`.** `stage_run.cpp:127` — `if (cfg.metabook) run_metabook() else run_optimize()`. The flagship mega-book recipe sets `--metabook`, so **`stage_optimize` never runs on the mega-book path.** Therefore every lever MUST reach the metabook path, not only `stage_optimize`:
- **Dead-factors (S1) + factor-cov (S2)** reach the mega-book via **S2's new per-step `build_risk_model` inside `run_metabook`** (the metabook `model_at`). S1 wires `stage_optimize`'s three sites; S2's metabook overload is the mega-book's build_risk_model site. Both consume the SAME `RiskModelConfig` (kind=Factor, dead_alpha_factors=true) + the SAME dead-lib.
- **Book-level turnover gate (S5-1)** reaches `stage_metabook` — the mega-book's fund book IS measured + gated (`stage_metabook.cpp:773-780`). ✓ **CORRECTION (S7 review):** the **participation cap (S5-2)** does NOT reach `stage_metabook` — S5-2 landed as optimizer-only (`stage_optimize.cpp:414-438`; its commit touched only `stage_optimize.cpp` + its test). The earlier "S5-1/S5-2 wire both" claim was false for the cap; a metabook/HRP-sleeve participation bound is unbuilt engine work (a p10/S8 item). The S7 prod recipe therefore does NOT emit `--participation-cap` on the metabook argv (it would parse-but-ignore). Likewise `group_neutralize` is optimizer/combine-only and is not carried on metabook.
- **GP dynamic trading (S3)** reaches ONLY `stage_optimize`'s position-mode blend (S3 is honestly scoped to it). The **mega-book's low-turnover** is delivered by: S4 turnover objective (selects slow-decay alphas at source) + metabook cross-sleeve netting (built) + `cost.kappa=turnover_penalty` (built) + S5 book-turnover gate. **Full GP aim-portfolio inside the MetaBook driver is DOCUMENTED FUTURE WORK** (fund/* scope; a candidate p10/S8 item) — do NOT force it into p9. A cheap future lever: expose metabook's hardcoded `mh.trade_rate=1.0` (`stage_metabook.cpp:62`) as an inert-default config.

**R4 (S1↔S2 seam — makes dead-factors reach the mega-book):** S1 exposes dead-lib resolution as a shared helper (`resolve_dead_alpha_lib_dir`/`maybe_open_dead_lib`/`collect_dead_alpha_ids` in a header both stages include). S2's metabook `build_risk_model` overload REUSES that helper so dead-factors reach the mega-book. Land S1 before S2.

**R1 (S1):** there are **three** `build_risk_model` call sites in `stage_optimize.cpp`, not two — `:252` (Diagonal/default branch, via default args) in addition to `:260,267` (Factor branch). `--dead-alpha-factors` WITHOUT `--risk-model factor` routes through `:252`. S1 fixes all three.

**R2 (S1):** no lifecycle driver marks alphas `Dead` (zero `.mark(Dead)` sites). `dead_ids` = **the admitted pool** (`state NOT IN {Candidate,Recycled}`) — this is the correct Kakushadze-Yu semantics (crowding factors from the holdings overlap of already-admitted alphas), NOT a `LifecycleState::Dead` filter (which would be a permanent no-op). CONFIRMED correct.

**R5 (S2):** `cfg.risk_model` (RiskModelConfig) already shipped in p8-S1; `run_combine`'s 3-arg overload is already Factor-aware. S2 = kill the `RiskModelConfig{}` hardcode at `stage_combine.cpp:765,771` (build from `cfg.risk_model`) + add the metabook overload. Chosen interpretation: the existing 2-arg `run_metabook` becomes `cfg.risk_model`-aware (the only reading that satisfies "governs the whole book" with zero hub edits).

**R6 (S7):** the prod `optimize` argv branch is DEAD (never runs). S7 moves the now-live flags onto the stages that DO run: dead-factors/dead-lib/group-neutralize/book-turnover-gate/participation-cap → metabook argv; `--risk-model factor` → combine + metabook; capacity/turnover-objective/robustness-*/deflate-selection → discover argv; borrow → report argv. GP flags reachable only via the explicit `-Stage optimize` companion run (documented in the V1 runbook).

**R7/R8 (S3):** `gp_trade_cost_scale → kappa = trade_rate/(1+scale)` is an S3-invented caller-side scalar (the frozen GP body has no cost-matrix param); inert at 0. GP position-mode always rides a whole-panel Diagonal V (Factor-covariance GP = future). Both documented.

**R9 (S6 — ownership correction):** ML-seed→Genome is impossible directly (`factory::Genome` IS `alpha::Ast`; no `unparse(Ast)` exists). The seam is UPSTREAM: materialize each learned model's score as a Panel column (`__ml_ae_alpha`/`__ml_tcn_alpha`, NaN outside coverage) + feed trivial `zscore(__ml_*)` strings into the EXISTING `seed_exprs` mechanism in `stage_discover.cpp`. **`factory/search_driver.*` + `factory/genome.*` need ZERO edits** — S6 ownership is Panel + `stage_discover.cpp` + `learn/*` (call) + `fund/*` (NCO), NOT search_driver/genome.

**R10 (S6 — registry correction):** the allocator enum is **`fund::RiskBudgetMethod`** (not `AllocatorMethod`); append `Nco` at index 3 (after existing HRP/ERC/InverseVol). NCO reuses `atx::core::cluster::cluster` + `hrp_weights` (intra-cluster) + `erc_log_barrier` (inter-cluster) verbatim; no RMT (no q=N/T available).

**R11 (S4 — registry refinement):** `capacity_objective`/`turnover_objective` flags live on BOTH `FitnessCfg` and `SearchConfig` (SearchConfig→FitnessCfg threaded in `evaluate_generation`). `pareto.hpp`/`assign_pareto_ranks` are already generic over `n_objectives` (zero edits). Fail-loud: `--capacity-objective` requires `--target-aum>0`. New helpers: `capacity_sqrt_law_score(strm,panel,cost,target_aum)` + `turnover_autocorr_score` (reuses existing `alpha::detail::ou_ar1_fit`).

**R12 (S5 — root correction):** `cost::BorrowModel`/`loop::BacktestLoop` are UNREACHABLE from the CLI (zero construction sites in `atx-impl/src`). S5 wires borrow into `book::accumulate_report` (the accumulator the pipeline actually calls), NOT `loop/*`. Book-netted-turnover helper `book_turnover_per_day` is NEW (inputs `risk::MultiPeriodResult.turnover[s]` + `fund::MetaBookResult.report.turnover_net[s]` already exist).
