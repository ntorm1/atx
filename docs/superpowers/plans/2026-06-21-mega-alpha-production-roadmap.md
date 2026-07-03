# Mega-Alpha Production Roadmap (post-merge, 2026-06-21)

**Standing #1 priority:** generate real, robust, low-turnover, high-capacity alphas AND a
downstream tradeable **mega-alpha** (combined portfolio) with a reported net-of-cost **out-of-sample**
Sharpe at a target AUM.

This roadmap is written AFTER merging the robustness sprint (R1/R2/R3/M1) and the W-task series
(W1a/W1b/W2/W4a/W4b/W5/W6) onto `main` (`02e21e7`). It supersedes the sequencing in
`2026-06-20-tradeable-mega-alpha-roadmap.md` for what comes next, and folds in the pipeline-gap and
W-series code reviews below.

---

## 1. Code review — robustness sprint (R1/R2/R3/M1)

Determinism contract holds across all four tasks (whole-branch review verified: byte-identical
defaults, report-only PBO out of the digest fold, R2 geometry overflow-safe, seq==parallel). Residual
issues, by severity:

- **[Important] R3b `oos_pbo` duplicates W4b `pbo`.** Merge artifact: both compute a run-level CSCV
  PBO over the admitted holdout set, with different `n_splits` rules (R3b: largest even ≤ min(T_h,16);
  W4b `finalize_run_pbo`: min(8,periods) even), into two report fields. W4b has the advisory gate +
  diagnostics; R3b is record-only. **Reconcile to ONE** (keep W4b's gate-capable computation; have the
  manifest's `oos_pbo` read `rep.pbo`). Tracked inline at `factory.hpp` FactoryReport.
- **[Important] R1 cross-process counter is best-effort.** `snapshot()` sidecar write is `(void)`-ignored
  (`library.hpp`). A write fault silently resets cumulative N → 0 on the next process, dropping back to
  pre-R1 deflation with no error. In-process sweep (M1) is unaffected (in-memory counter). Fix: surface
  the write error on the gated/sweep persistence path (hard-fail, like R1 wired into discover).
- **[Minor] R2 walk-forward is built but never defaulted on.** `oos_n_windows` only rotates when
  `--oos-windows` is passed; no default config (sweep, W6 acceptance) uses it, so accumulation still
  reuses the terminal holdout unless asked. See A4.
- **[Minor] M1 `robustness_gate=false` hardcoded** in `stage_sweep.cpp` — the regime-robustness gate is
  scaffolded but off. See D2.
- **[Minor] M1 `sweep` not wired into `run_all`** — manual-chaining only. See A1.

## 2. Code review — W-task series (W1a/W1b/W2/W4a/W4b/W5/W6)

Strong, disciplined, determinism-safe at defaults. Per feature:

- **W1a (configurable WeightPolicy + Raw transform):** sound; `Raw` passthrough is the correct choice
  for pre-conditioned DSL alphas (re-ranking would destroy the low-vol scaling signal). Gaps:
  `industry_neutral` knob exists but is rejected in discovery (dead, footgun); no test that `Raw`
  produces a DIFFERENT digest from `Rank` (a silent fall-through to Rank wouldn't be caught).
- **W2 (capacity screen):** highest-impact W feature — masks the search universe to liquid names
  (close>$1, rolling ADV≥$X), PIT-correct (ADV warm-up NaN excludes look-ahead), fail-closed, pure.
  This is what makes "high-capacity" structural rather than hoped-for. Gap: `W2_...ActiveChangesUniverse`
  test uses trivial `min_adv=1.0` and never asserts names are actually excluded.
- **W4a (split-sample floor + weak-panel robust factor):** split floor rejects single-regime artifacts
  (H1 strong / H2 dead); robust factor multiplies fitness by how well the WQ holds on a seeded
  sub-universe. Both opt-in, digest-safe at -inf/nullptr defaults. Gaps: the weak panel is a RANDOM
  subset, not a strictly-lower-liquidity one (a true capacity stress would use the harder sub-universe);
  split floor runs on the same stream as the gate (full-panel), so "stable" can differ on a real holdout;
  no guard against weak_panel/train-window misalignment when OOS + robust are both on.
- **W4b (run-level CSCV-PBO advisory gate):** correct CSCV (Bailey/LdP), fail-open on infeasible,
  default 1.0 = skipped. Advisory only (warns, never un-admits). Gap: computed on full-panel admitted
  PnL, not holdout — duplicated by R3b (see §1).
- **W5 (capacity-universe metric):** recorded-only, pure, off-path byte-identical. Gap: no floor on
  `names_per_day` — an over-aggressive screen (e.g. $500M ADV → 50 names) gets no warning beyond the
  manifest value (too thin to diversify).
- **W6 (auto-rediscover acceptance test):** the real-panel verdict (Sharpe>1 ∧ turnover<0.30 against
  the known Sharpe-1.56 low-vol alpha) is **data-gated** (`ATX_ALPHA101_PANEL`); the CI/synthetic path
  sets all bars to −1e9 and `GTEST_SKIP`s the verdict. **So the acceptance criterion is never
  machine-verified in standard CI** — it's a smoke test there.

### The single convergent defect (W + R)
Every quality screen — DSR deflation, split floor, PBO, gate floors — runs on the **in-sample
(full-panel)** PnL for a standard non-accumulation `discover`. The OOS path exists (R2/R3, `mine_into_oos`)
but is OFF unless `--library-dir`/`--oos-fraction`. And the **combination step has NO out-of-sample
validation at all**: `combine` fits blend weights on the full history, `report` never emits a portfolio
Sharpe (IS or OOS), and `run_all` calls UNGATED discover — not sweep, not the library. So today the
engine cannot emit "a tradeable mega-alpha with an OOS Sharpe" because nothing computes that number.

---

## 3. Roadmap — prioritized

### Phase A — Close the production loop (PRIORITY #1: produce the mega-alpha with OOS metrics)
The keystone. Until this lands there is no tradeable-mega-alpha output to trust.

- **A1 — Route `run_all` through the gated/sweep+library path.** `stage_run.cpp` calls `run_discover`
  ungated; switch to gated discover (or `sweep` when `--sweep-runs≥1`) into `--library-dir`, and feed
  `combine --library-dir` (not the loose `.dsl` dir). Without this the automated pipeline never touches
  the validated/accumulated library. (Agent-2 P0.)
- **A2 — Combine-level train/holdout split + reported portfolio OOS Sharpe. THE keystone metric.**
  `combine`: fit blend weights on `[0, T_is)` only; apply to the `[T_is, T)` holdout. `report`: split the
  equity curve IS/OOS, compute portfolio Sharpe / drawdown / turnover / capacity on the HOLDOUT, emit as
  named kvs (`portfolio_sharpe`, `portfolio_oos_sharpe`, `oos_turnover`, …) and in `summary.txt`. This is
  the number the whole project exists to produce. (Agent-2 P1+P4.)
- **A3 — Reconcile the duplicate PBO** (§1): one run-level PBO, holdout-based, gate-capable; manifest
  `oos_pbo` reads it. Removes the merge wart and the double-count risk in any later conviction score.
- **A4 — Default the validation knobs ON for accumulation.** Thread R2 `oos_n_windows` (e.g. 4 walk-
  forward windows) into the sweep's default config so a K-run sweep rotates the holdout instead of reusing
  the terminal one; keep single-run discover byte-identical. Add a real-panel sweep→combine→report
  acceptance test that asserts a finite positive portfolio OOS Sharpe (the machine-verified version of W6).

**Phase A exit:** `run_all` (or `sweep→combine→optimize→report`) emits ONE mega-alpha with a net-of-cost
OOS Sharpe, OOS turnover, and capacity footprint at a stated AUM — reproducibly, from the accumulated
validated library.

### Phase B — Capacity at the portfolio level (the "high-capacity" requirement, end-to-end)
W2 makes the per-alpha universe liquid; the BOOK must also be deployable at AUM.

- **B1 — Real per-name ADV into combine de-correlation.** `stage_combine.cpp` passes a `capacity(pool, 1.0)`
  stub to `decorrelate_weights`; thread the panel ADV field so crowding/capacity weighting is real.
- **B2 — ADV-participation constraint in the optimizer.** `MultiPeriodOptimizer` has no ADV limit; add a
  per-name cap `|w_i|·AUM ≤ λ·ADV_i` (e.g. 5% participation) so the optimized book is tradeable, not just
  diagnosed. (Agent-2 P3.)
- **B3 — Capacity curve at target AUM in report** → pick the deployable AUM where OOS Sharpe (A2) still
  clears the bar. Closes the "high-capacity" loop with a number.

### Phase C — Discovery quality + throughput (more, better alphas feeding A/B)
- **C1 — PIPELINE_REMEDIATION P0** (`atx-impl/PIPELINE_REMEDIATION_PLAN.md`): kill premature stagnation
  patience, gate on DSR not noisy Sharpe, exclude categorical fields from the numeric grammar. ~0.5 day,
  expected to take admits from ~2 → dozens. Do this EARLY — it raises the alpha supply A/B consume.
- **C2 — Redundant-eval cache + confirm parallel OOS** (the R-sprint already proved seq==parallel for the
  OOS path; verify the accumulation sweep runs the parallel substrate and reuses the SignalSet cache).
- **C3 — Resumable discover** (`2026-06-19-resumable-discover.md` T1–T7): SQLite checkpoint/resume,
  off-path byte-identical — makes large multi-seed sweeps crash-safe.

### Phase D — Conviction + regime depth (robustness, RenTech gaps; opt-in)
- **D1 — Conviction score → fractional-Kelly sizing** (S10a): continuous 0–1 from DSR/PBO/split-stability;
  scale position size by conviction instead of binary admit. Reuses A3's single PBO (no double-count).
- **D2 — Enable the regime-robustness gate in sweep** (flip M1's `robustness_gate`) once D-tier validates
  it, AND the regime-posterior combiner (S10b) — opt-in, NOT HMM-mandatory (research guardrail: HMM is one
  tool, do not over-architect around it; multi-horizon/decay alternatives are equally valid).
- **D3 — Breadth instrumentation (IR=IC·√N_eff) + walk-forward re-fit harness** (S10c).

---

## 4. Sequencing & acceptance

1. **C1** first (½ day) — cheap, unblocks alpha supply; lets A/B run on a real library.
2. **Phase A** (A1→A2→A3→A4) — the priority-#1 deliverable. A2 is the keystone; nothing in B/D is worth
   doing until a mega-alpha OOS Sharpe exists to move.
3. **Phase B** — make that mega-alpha deployable at AUM.
4. **Phase C2/C3, then Phase D** — scale and deepen once the loop produces a trustworthy, tradeable number.

**Global acceptance (unchanged):** every new path opt-in/flagged with a byte-identical default;
determinism sacred (F1 digest byte-identical across substrates/worker-counts; twice-run identical);
`oracle.hpp` untouched; the only sanctioned re-baseline remains the library manifest `version_id`.

**North-star metric:** net-of-cost mega-alpha **OOS** Sharpe at target AUM, reported alongside
cross-sweep-deflated DSR, the single reconciled run-level PBO, OOS turnover, and the capacity curve.
