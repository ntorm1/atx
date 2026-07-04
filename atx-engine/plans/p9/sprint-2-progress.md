# Sprint 2 progress ledger — Factor Cov → Combine + Metabook (+ R3/R4 crowding-reaches-mega-book)

One line per clean review (ROADMAP §141). Newest last. Executed per the plan **AMENDMENT** (S2-0 shared header → S2-1 combine → S2-2 metabook+wire → S2-3 proof).

| Unit | Commit  | Deliverable                                                                              | Review |
|------|---------|------------------------------------------------------------------------------------------|--------|
| S2-0 | e27c258 | extract dead-alpha wire into shared `dead_alpha_wire.hpp`; `stage_optimize.cpp` includes it | —   |
| S2-1 | 717fbd0 | `run_combine` 0-/1-arg overloads build `RiskModelConfig` from `cfg.risk_model`            | —      |
| S2-2 | e299b1a | additive 3-arg `build_metabook_result`/`run_metabook`; Factor loop threads dead-alpha wire | —    |
| S2-3 | dcd9576 | `MetabookDeadAlphaWire` — crowding delevers the mega-book (R3/R4 closed) + fail-open guard | —     |

**Review (clean):** SHIP. Adversarial reviewer confirmed the honesty gate is genuine, not faked:
- `CrowdingDeleversMegaBook` bites on the real mega-book path — both `build_risk_model` call sites (`stage_metabook.cpp:557,568`) thread `dead_lib_ptr, dead_ids, dead_as_of`; augmented FactorModel flows `model_at → Sleeve::run → PortfolioOptimizer::solve` at λ=1.0, so the crowded weight strictly shrinks (|w_delev|=0.0883 < |w_base|=0.0946) with bit-level `EXPECT_NE`. RED reason legit (nullptr ⇒ byte-identical). Debug scaffold removed pre-commit.
- Determinism contract satisfied at combine + metabook: off-path Diagonal-default byte-identical (`std::bit_cast` cell-by-cell), on-path RED→GREEN, twice-run, order-invariance.
- S2-0 is a verbatim relocation; 19/19 S1 tests still byte-identical.
- Frozen-file discipline held (8 files touched; no `config.*`, `stage_run.cpp`, `dispatch.cpp`, `stage_riskmodel.*`, `library/*`, or the frozen combine 3-arg overload).
- PIT holds (`fit_end = period+1`; PitA/PitB assert no future leakage).
- Full `atx-impl-tests`: **285 passed / 0 failed / 3 pre-existing env-skips**.

**Minor (non-blocking, deferred):** cross-cutting 1-flag/both-stages test (superseded by amendment); default-styles CLI delever proven by inspection + fail-open rather than a dedicated fittable-fixture test (10-name toy panel can't support a K≥4 styles-on fit — honest limitation documented in the test header).
