# Sprint 2 — Real-Alpha Production

**Goal:** turn the existing-but-dormant defenses on, kill the degenerate-alpha class on the real
ORATS universe, and scale breadth into a deflation-surviving, capacity-bounded **mega-alpha**.

**Scored against** (every task): OOS-DSR ↑, turnover ↓, %ADV capacity ↑, PBO ↓, # robust admits ↑,
combined-Sharpe > best-single.

This sprint is **not** "build new capability." Audit (2026-06-24) confirmed the discover→gate→
combine→optimize→report machine is built and, since the Jun-19 remediation plan, the orphaned
combiners (`AlphaCombiner`/`conviction`/`decorrelate_weights`/library-backed `AlphaStore`) are now
**driven** in `stage_combine.cpp`, and library accumulation exists via `--library-dir`
(`stage_discover.cpp:387`). The defect is that the robustness gates are **opt-in and were OFF** for
the canonical run, so junk passed a vacuous first-alpha gate.

---

## The evidence

Canonical 10-year run (`work/accept/`): 25.2M rows, 23,348 securities, 2,627 dates, 1.5 GB panel,
790 evals → **one admit**:

```
((ts_min(earnFlag, 45) ^ atmCenI_21d) / close)   oos_sharpe=2.27 fitness=3.14 turnover=0.27
```

Why it is degenerate (not edge):
- `earnFlag` is binary 0/1. `ts_min(earnFlag,45)` = min over 45 days ≈ **0 always** (would be 1 only
  if every day in the window had the flag set).
- `^` is power; `0 ^ atmCenI` = **0**. The numerator is ≈0 in nearly every cell → an ultra-sparse,
  near-constant signal whose Sharpe is a sparsity/few-bets artifact.
- `/ close` is a **price-scale tilt** (loads low-price names) — precisely the R2 target.

The defenses that reject this class already exist:
- **R1 typed-fields** (`--typed-fields`, `config.cpp:38`) — keeps categorical fields (`sector`,
  binary flags) out of the numeric grammar.
- **R2 price-scale gate** (`--reject-price-scale`, `config.cpp:166`) — rejects `/close`-style tilts.
- **min-dsr** deflation gate (default 0 → OFF unless set).

They were all OFF in the canonical run, and the **first-alpha gate is vacuous** (empty pool →
`diversify=1.0` free; `robust` term inert with no weak-universe panel).

---

## Tasks

### S2-0 — Re-run canonical with defenses ON and prove the junk is rejected *(do first — highest information)*
- Re-execute the canonical acceptance pipeline ([../../../scripts/canonical-acceptance-run.ps1](../../../scripts/canonical-acceptance-run.ps1))
  adding: `--typed-fields --reject-price-scale <θ> --min-dsr 0.5 --library-dir <stable>`.
- **Accept:** `((ts_min(earnFlag,45) ^ atmCenI_21d)/close)` is **rejected**; the run's `reject_hist`
  shows the new buckets firing. If it still admits, R1/R2 have a hole — debug *that* before anything
  else in this sprint.
- **Deliverable:** a committed before/after manifest diff in `work/accept/` (or a fresh `work/`),
  plus a one-paragraph finding.

### S2-1 — Make defenses default-ON for production paths
- **Root cause:** gates are opt-in (defaults preserve legacy byte-identity), which shipped junk in
  the canonical run.
- **Fix:** flip defaults to ON for the `run`/`sweep` production stages (`stage_run.cpp`,
  `stage_sweep.cpp`); keep an explicit opt-out sentinel so the byte-identity *tests* stay green.
  Document the new defaults in `STATUS.md`.
- **Accept:** a no-flag `run` rejects the degenerate class; the determinism tests still pass via the
  opt-out path.

### S2-2 — Non-vacuous first-alpha gate
- **Root cause:** pool-of-one rides `diversify=1.0` for free and the `robust` factor is inert with no
  weak-universe panel (`fitness.cpp:276`).
- **Fix:**
  1. Wire a **weak-universe panel** (e.g. ex-top-decile-ADV, or a cap-quintile subset) so the
     `robust` term measures real sub-universe stability instead of returning 1.0.
  2. Require a **minimum absolute deflated-Sharpe on a real OOS slice** for *every* admit including
     the first — a pool of one must still clear a statistical-significance bar, not just the
     diversify free pass.
  3. Add a **"has time variation" structural reject**: roots with `required_lookback==0` and a single
     static field leaf (the `zscore(sector)` / constant-tilt class) are rejected at typecheck
     (`typecheck.cpp:327-393`) as a belt-and-suspenders to R1.
- **Accept:** synthetic degenerate seeds (constant tilt, sparse-flag, price-scale) are all rejected
  with the pool empty; a known-good options-vol seed (e.g. `ts_argmax(atmCenI_126d,20)`) still admits.

### S2-3 — Fix under-production (get to *many* robust alphas)
- **Root cause(s)** (from the Jun-19 remediation plan — verify which already landed, apply the rest):
  premature stagnation early-stop on a monotone elitism-pinned curve; admission gated on a noisy
  raw-holdout Sharpe floor instead of DSR; pop-60 too small for a 5-objective frontier; weak
  fresh-blood injection.
- **Fix:** stagnation stop based on *collapse* (best **and** mean flat) or epsilon-improvement; gate on
  DSR/PSR significance not a raw Sharpe point estimate; `population ≥ 200`; `n_immigrants ≥ pop/10`;
  drop the parsimony attractor to a tie-break.
- **Accept:** a deep run yields **dozens** of admits, none degenerate; `reject_hist` no longer
  dominated by the holdout-Sharpe bucket.

### S2-4 — Add orthogonal data (the breadth axis)
- **Root cause:** the panel exposes only ~12–16 ORATS columns. The rubric (3-axis search; "100×
  models → 10× predictions") needs *many* datasets. Low field breadth caps how many uncorrelated
  alphas can exist.
- **Fix:**
  1. Finish and merge the **FINRA short-interest** track (`data/finra_short.hpp`, augment stage already
     started on `track-b-information-structure`) — an orthogonal, causal feature family.
  2. Surface more of the ORATS payload already in the zip: IV term-structure
     (`atmCenI_21d`/`atmCenI_126d` are in; add more tenors/skew if present), earnings counts, return
     factors — fields read but currently projected away (`orats_history` reads 71 cols, keeps 16).
- **Accept:** panel field count rises; the discover search finds admits whose top constituents draw on
  the new fields; pairwise correlation of the admitted pool stays under the gate.

### S2-5 — Multi-seed accumulate → drive the mega-alpha
- **Root cause:** breadth thesis needs accumulation; a single run can't populate a low-correlation
  pool. Accumulation now exists (`--library-dir`) and combine is wired but unproven at scale.
- **Fix:** run **N seeds** into one stable `--library-dir`; then `combine` builds the mega book via
  `ShrinkageMv` + `conviction` (`apply_conviction`) + `decorrelate_weights`
  (`stage_combine.cpp:113-209`). Assert `combo.weights.size() == streams.n_alphas()` and that the
  blend is keyed by `AlphaId`, not directory-sort order (the latent silent-mismatch the remediation
  plan flagged).
- **Accept:** combined OOS Sharpe **>** best single constituent; max pairwise corr in the admitted pool
  < 0.3 (gate enforced); the mega-alpha survives DSR/PBO/CPCV on the AuditExact path.

### S2-6 — Portfolio realism: capacity + neutralization
- **Root cause:** factor neutralization is post-hoc, not a constraint in the optimizer; capacity is
  caller-supplied and not coupled to the optimizer inner loop; survivorship bias documented but
  unfixed.
- **Fix:**
  1. Add **sector/beta neutrality as explicit optimizer constraints** (`risk/optimizer.hpp` currently
     enforces only Σw=0, Σ|w|≤L, |w_i|≤cap).
  2. Couple the **√-law capacity/impact** model to remaining-ADV inside the optimize loop
     (`cost/capacity.hpp`) so capacity claims are earned, not asserted.
  3. Report a **capacity curve as a first-class scorecard output** (AUM at which net edge → 0), per the
     RenTech rubric.
  4. Track the **survivorship caveat** (`universe.hpp`) explicitly in every published scorecard until
     delisted-symbol recovery is built (own follow-up).
- **Accept:** the mega-alpha scorecard reports sector/beta exposures ≈ 0, a capacity curve, and a
  turnover/cost-adjusted net Sharpe — not just gross.

---

## Sequencing

1. **S2-0** today — one re-run answers "do the defenses work on real data?".
2. **S2-1 + S2-2** — make ON the default and close the vacuous-gate hole, so no future run can ship
   junk.
3. **S2-3 + S2-4** — produce many, diverse, real alphas (this is where Sprint 1's speed pays for
   itself — a 200-pop, multi-seed, many-field search is only affordable once the hot path is fast).
4. **S2-5** — accumulate and combine into the mega-alpha.
5. **S2-6** — make the portfolio honest (capacity, neutralization, net of cost).

## Definition of done

A committed, reproducible scorecard for a **mega-alpha** built from a multi-seed library on the real
ORATS liquid universe, that: contains **no** degenerate constituent; has combined OOS-DSR clearing the
significance bar with PBO low; reports turnover, %ADV capacity curve, and cost-adjusted net Sharpe;
and whose every constituent and the combined book were finalized through the **`AuditExact`** path
(Sprint 1 §Determinism contract).
