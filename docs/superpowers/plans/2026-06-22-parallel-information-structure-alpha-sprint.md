# Sprint Plan — Information & Structure (Parallel Track B)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Date:** 2026-06-22
**Goal (shared with Track A):** produce **real, robust, high-capacity, high-Sharpe, low-turnover** tradeable alphas and a downstream mega-alpha with a **positive net-of-cost out-of-sample Sharpe at target AUM**.
**Branch base:** `main`. **Run in a dedicated git worktree** so it executes concurrently with Track A (`docs/superpowers/plans/2026-06-21-high-quality-alpha-generation-sprint.md`).
**Method:** subagent-driven-development (SDD) on an isolated worktree branch (e.g. `track-b/information-structure`).

---

## Why a second track, and how it differs

Track A's central insight is that the **search / cost / combine machinery already exists and is merely dormant** — so Track A is *activation + tuning on the current data and DSL*. Track A's own acceptance section (line 184) names the seam this track occupies:

> "If the OOS Sharpe is still ≤ 0 after T1–T5 on the full panel … the signal sources in the current DSL/field set do not carry net-of-cost OOS edge, and the next sprint must expand the **data** (new fields/datasets …) rather than the search."

Track B runs that "next sprint" **in parallel as a hedge**, plus two structure levers Track A does not touch:

| Lever | Track A (activate the machine) | Track B (widen + structure the inputs) |
|---|---|---|
| **Information set** | fixed: price/vol/sector + the existing fields the search already uses | **add** short-interest (FINRA), and **activate** the present-but-unused IV-surface (`atmCenI_*`) and earnings (`earnFlag`,`nEarnCnt_5d`) fields |
| **Robustness mechanism** | sweep-wide deflation (T2), strict gates (T3), diversity *objective weight* (T5) — all in `fitness.hpp`/`factory.hpp` | **factor-neutral-by-construction**: wrap signals in `residualize(·, sector, size)` in the **seed DSL** (`cs_ops`, not `fitness`) |
| **OOS test** | single holdout + conviction walk-forward in combine (T7) | **regime-conditional + purged-embargo** OOS in a **new standalone analyzer** (vol-tercile, `regime/*`) |
| **Validation loop** | C++ pipeline only | **Python `atxpy` offline pre-screen** on candidate alphas before the costly C++ run |

Both tracks share the same dormant-machinery theme — but in **disjoint subsystems**. Track A wakes the *search/cost/combine* engine; Track B wakes the *data-ingestion / cross-sectional-structure / regime-eval* subsystems. **The two merge cleanly because they touch disjoint files (matrix below), and the merged run gets BOTH the activated machinery AND the widened, structured, regime-tested signal set.**

---

## Global Constraints (binding — copy into every task brief)

- **Determinism is sacred.** `alpha/oracle.hpp` MUST NOT be touched. The F1 search digest stays byte-identical on the default (flag-absent) path. Every Track-B capability is **opt-in behind a flag / new subcommand**; the default path is byte-identical to today's.
- **Field-append discipline.** New panel fields (B1) are **appended at the end** of the field vector and only when the opt-in flag is present, so existing field ids and existing-field column digests are unchanged. New fields change the panel digest *only* under the flag, and change `version_id` (sanctioned) *only* when a seed/run actually consumes them.
- **Net-of-cost requires Track A's T1.** Until T1 lands on `main` (or is cherry-picked into this worktree), label every Sharpe **"gross."** B6 acceptance states net only after the T1 cost wiring is present.
- **Canonical Universe & Rebalance are shared.** Reuse Track A's universe screen (single-name GICS, `close>1`, `adv20>$25M`, rolling, `--rebalance daily`) verbatim — Track B augments that panel, it does not redefine it. The augmentation is a **post-build merge step** (B1) so it composes on top of whatever universe panel Track A's harness produces.
- NEVER `git add -A`; stage explicit paths. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push.
- Pre-existing failures out of scope: `AtxImplPanel.BuildsPanelFromSegments`, `Alpha101Orats.*`, `AlphaSlotPoolDeathTest.*`, `AlphaVm_ZeroAlloc.*`, `RegimeCombineDeathTest.*`.

---

## File-collision matrix (the parallel-safety artifact — read FIRST)

**Track A owns (Track B MUST NOT modify these):**
`atx-impl/src/stage_optimize.cpp`, `atx-impl/src/stage_combine.cpp`, `atx-engine/include/atx/engine/factory/fitness.hpp`, `factory/factory.hpp`, `factory/research_driver.{hpp,cpp}`, `alpha/ts_ops.hpp`, `cost/cost_aware.hpp`, `cost/calibration.hpp`, `cost/capacity.hpp`, `combine/crowding.hpp`, `loop/weight_policy.hpp`.

**Track B owns (new or Track-A-untouched):**

| Track B task | Files (C = create, M = modify) | Track A touches it? |
|---|---|---|
| B1 FINRA loader + augment stage | C `atx-engine/include/atx/engine/data/finra_short.hpp`, C `atx-engine/src/data/finra_short.cpp`, C `atx-impl/src/stage_augment.cpp`; reuse `data/adapt_feature.hpp` (read), `data/dataset.hpp` (read) | **No** |
| B2 IV/earnings seed family | M `atx-impl/tests/fixtures/factor_templates.txt` (append), C `atx-impl/tests/fixtures/iv_earnings_templates.txt` | **No** (A never edits seed fixtures) |
| B3 factor-neutral seed family | M `atx-impl/tests/fixtures/factor_templates.txt` (append), C `atx-impl/tests/fixtures/neutralized_templates.txt`; reuse `alpha/cs_ops.hpp` (read; extend only if a classifier is missing) | **No** |
| B4 regime-conditional OOS analyzer | C `atx-impl/src/stage_regime_oos.cpp`; reuse `eval/regime_slice.hpp` (read), `regime/series.hpp`/`regime/loader.hpp` (read) | **No** (A only reads `stage_report.cpp`) |
| B5 atxpy offline pre-screen | C `python/scripts/prescreen_alphas.py`; reuse `python/src/atxpy/backtest.py` (read), `eval.py` (read) | **No** |
| Subcommand registration | M `atx-impl/src/dispatch.cpp` (append two routes: `augment`, `regime-oos`) | **Coordination point — see Merge protocol** |

The **only** shared C++ source is `dispatch.cpp` (subcommand routing). Both tracks only ever **append** routes there → different lines → trivial merge. All other Track-B edits are new files or the seed-fixture text file Track A never opens. **This matrix is the contract; if a task needs to edit a Track-A file, STOP and re-scope.**

---

## Phase B0 — Widen the information set

### B1 — FINRA short-interest → panel field (the new orthogonal signal)
**Why:** short interest / days-to-cover / utilization is a well-documented cross-sectional return predictor that is **uncorrelated with the price/vol/sector signals the search currently mines** — exactly the orthogonal source Track A's acceptance note calls for. The downloader already exists and emits parquet; **no loader consumes it yet.**
**Anchors:**
- `python/scripts/download_finra_short_interest.py` — emits Hive-partitioned parquet at `data/short_interest/date=YYYY-MM-DD/part-*.parquet`; columns include `symbol`, `settlement_date`, `current_short_position_quantity`, `average_daily_volume_quantity`, `days_to_cover_quantity`, `change_percent`.
- `atx-engine/include/atx/engine/data/adapt_feature.hpp:29` — `merge_features_into_panel()` (the sanctioned extensibility point: align a feature `Dataset` onto the price `Panel`'s date×name axis and append columns).
- `atx-engine/include/atx/engine/data/dataset.hpp` — `Dataset` ctor used to stage external columns (pattern mirrored from `regime/source_csv.hpp`).
- `atx-engine/include/atx/engine/alpha/panel.hpp:132-141,183` — `field_id()` / `field_names_` (append-at-end keeps existing ids stable).
- `atx-impl/src/serialize_panel.cpp` — `read_panel()` / `write_panel()` (the augment stage reads `research.bin`, writes `research_aug.bin`).
**Design:**
- **B1a — Loader.** New `finra_short.{hpp,cpp}`: read the parquet partitions for the panel date range, key by (settlement_date, symbol), forward-fill the bi-monthly FINRA cadence onto trading days **causally** (a value is visible only on/after its settlement publication date — never look-ahead). Emit three aligned derived fields: `si_dtc` (= `days_to_cover_quantity`), `si_util` (= `current_short_position_quantity / (shares_outstanding)` — use panel `shares`/`market_cap` to form float-relative utilization), `si_chg` (= `change_percent`). NaN where absent.
- **B1b — Augment stage.** New `stage_augment.cpp` (`atx-impl augment`): `read_panel(research.bin)` → build the FINRA `Dataset` via B1a → `merge_features_into_panel()` (append `si_dtc`,`si_util`,`si_chg`) → `write_panel(research_aug.bin)`. Pure post-processing; the canonical universe panel is the input, unchanged.
**Determinism:** entirely opt-in (you only get augmented fields if you run `augment` and point downstream at `research_aug.bin`). The base panel build is untouched ⇒ byte-identical default. Augmented panel has a *new* digest (expected) only when the stage is run.
**Tests (`atx-engine/tests/data/finra_short_test.cpp`, `atx-impl/tests/augment_test.cpp`):**
- Causality: a short-interest value with settlement date `t` is NaN on `t-1` and present on `t` (no look-ahead).
- Append-only: after augment, the first N field ids and their columns are bitwise-identical to the input panel; new fields are at ids N, N+1, N+2.
- Round-trip: `read_panel(write_panel(aug)) == aug` (digest matches).
- A name with no FINRA record carries NaN (and a DSL `backfill`/guard handles it without poisoning cross-section).

### B2 — Activate the present-but-unused IV-surface & earnings fields (seed family)
**Why:** the panel **already carries** `atmCenI_21d`, `atmCenI_126d` (ATM implied move, 21d/126d), `earnFlag`, and `nEarnCnt_5d` — but the seed catalog uses **only price/vol/sector**. These are zero-marginal-cost orthogonal signals (already loaded, never mined). Activating them widens the search's source set with no new data.
**Anchors:**
- `atx-engine/include/atx/engine/data/orats_history.hpp:21-24` — `kOratsFields` lists `atmCenI_21d`,`atmCenI_126d`,`earnFlag`,`nEarnCnt_5d` (present in panel).
- `atx-impl/tests/fixtures/factor_templates.txt` — current 9 economic templates (price/vol/sector only); the W3 GA warm-start catalog parsed by `factory/search_driver.hpp:308` (`parse_seed_exprs`).
**Design:** new fixture `iv_earnings_templates.txt` with sector-neutral seed expressions over the dormant fields, e.g.:
- IV term-structure slope: `rank(atmCenI_21d / atmCenI_126d)` (front-vs-back implied move).
- IV vs realized gap: `rank(atmCenI_21d - ts_std(delta(close)/delay(close,1), 21))` (implied minus realized).
- Earnings-proximity gate: `trade_when(earnFlag == 0, <base signal>, 0)` style avoidance, and `nEarnCnt_5d`-conditioned sizing.
Each template wrapped `group_neutralize(·, sector)` to match the catalog's load-bearing pattern. Feed via the existing `--seed-file` / seed-catalog mechanism (no engine change).
**Determinism:** seeds change which alphas the search explores ⇒ opt-in seed file ⇒ `version_id` re-baseline only; search digest path (no seed file) byte-identical. No `oracle.hpp`/`ts_ops.hpp` change.
**Tests (`atx-impl/tests/seed_parse_test.cpp`):** every line in `iv_earnings_templates.txt` parses + type-checks against the augmented panel field set; each references at least one of the four dormant fields; discovery with the file admits ≥1 alpha whose dominant field is an IV/earnings field.

---

## Phase B1 — Bake structure into the signals (cs_ops, not fitness)

### B3 — Factor-neutral-by-construction seed family
**Why:** the dominant reason in-sample alphas die OOS is undeclared exposure to crowded style factors (sector, size, beta). Track A attacks this *statistically* (diversity weight, sweep-wide deflation). Track B attacks it *structurally*: make each alpha **residual to sector and size by construction** in its DSL, so what survives is the idiosyncratic, harder-to-arb component. This is a different mechanism in a different file (`cs_ops` DSL vs `fitness` objective) and the two compose.
**Anchors:**
- `atx-engine/include/atx/engine/alpha/cs_ops.hpp:354-453` — `cs_residualize_row()` (Frisch-Waugh-Lovell partial-out: demean within `g`, regress on covariate `z`, emit residual; bit-exact summation order).
- `atx-engine/include/atx/engine/alpha/registry.hpp:120` + `src/alpha/registry.cpp:43-58` + `alpha/vm.hpp:675-677` — `CsResidualize` opcode already registered & dispatched (no new op needed for sector+one-covariate).
- `atx-impl/tests/fixtures/factor_templates.txt` — append target.
**Design:** new fixture `neutralized_templates.txt` wrapping each base signal in `residualize(<signal>, sector, market_cap)` (sector groups + size covariate) — sector-AND-size neutral. Where a base template currently does `group_neutralize(x, sector)` (sector-only), add a size-neutral sibling. **Only if** a needed multi-covariate form is missing (e.g. residualize on size *and* beta simultaneously), extend `cs_ops` via the 3-file registration (`registry.hpp` enum + `registry.cpp` metadata + `vm.hpp` dispatch) — but **prefer composition of existing ops** (nest residualize) to avoid touching the ISA.
**Determinism:** seed-only (opt-in file) ⇒ `version_id` re-baseline. If `cs_ops` is extended, the new opcode is additive (new enum value, new dispatch case) and unreachable on the no-seed path ⇒ search digest byte-identical; prove with the factory golden-digest tests staying green.
**Tests (`atx-impl/tests/seed_parse_test.cpp`, `atx-engine/tests/alpha/cs_residualize_test.cpp` if op extended):** neutralized templates parse/type-check; the residualized signal has ≈0 cross-sectional correlation to `market_cap` and ≈0 mean within each sector (within float tolerance); if op extended, oracle-vs-VM bit-exact and golden digest unchanged on no-op path.

---

## Phase B2 — Condition the OOS test (regime, not multiple-testing)

### B4 — Regime-conditional + purged-embargo OOS analyzer (new standalone stage)
**Why:** a single blended OOS Sharpe hides regime dependence — an edge that only works in low-vol is not robust. Track A measures selection integrity (T2 sweep-wide deflation) and a single holdout (T7 conviction-WF). Track B adds the **orthogonal robustness lens**: per-regime OOS Sharpe of the *final book*, plus a purged/embargoed split so train/test don't bleed. The regime machinery is built and dormant (mirrors Track A's theme, different subsystem).
**Anchors:**
- `atx-engine/include/atx/engine/eval/regime_slice.hpp:1-100` — vol-tercile {low,mid,high} deterministic partition + per-regime Sharpe + walk-forward slices + robustness verdict (no fit, no look-ahead).
- `atx-engine/include/atx/engine/regime/series.hpp`, `regime/loader.hpp`, `atx-impl/src/stage_regime.cpp:15-83` — macro/regime store (VIX/MOVE/NFCI) and CLI loader (optional richer regimes).
- `atx-impl/src/stage_report.cpp:219-305` — the existing IS/OOS split + `sharpe_of(pnl_net)` logic to **mirror (read-only)**, NOT edit.
- `atx-impl/src/serialize_panel.cpp` — read the combo book + panel.
**Design:** new `stage_regime_oos.cpp` (`atx-impl regime-oos`): reads `--books combo.bin --panel research_aug.bin`, replays the book's `pnl_net` per rebalance period (same `sharpe_of` math as `stage_report`, copied not shared), then (i) slices the **OOS window only** by vol-tercile via `regime_slice.hpp` and reports per-regime OOS Sharpe + turnover; (ii) recomputes the headline OOS Sharpe under a **purged-embargo** split (drop the `embargo` periods straddling the holdout boundary so no leakage). Emits `regime_oos.txt` sidecar. Read-only w.r.t. all other stages.
**Determinism:** pure analyzer; produces a report, mutates no book. Adding the subcommand is additive in `dispatch.cpp`. No engine/oracle change.
**Tests (`atx-impl/tests/regime_oos_test.cpp`):** on a synthetic book with a known low-vol-only edge, high-vol-regime Sharpe ≈ 0 while low-vol Sharpe > 0; purged-embargo OOS Sharpe ≤ naive OOS Sharpe (removing straddle periods can't inflate it); the headline number reconciles with `stage_report`'s `portfolio_oos_sharpe` when embargo=0 and regime=all.

---

## Phase B3 — Fast offline validation loop

### B5 — atxpy pre-screen (validate candidates before the costly C++ run)
**Why:** the C++ discovery run is expensive; a Python pre-screen on a handful of candidate expressions (including the B2/B3 seed families) shortens the loop and catches dead signals before committing compute. Uses the existing `atxpy` facade — no C++ change.
**Anchors:**
- `python/src/atxpy/backtest.py:34-60+` — `run_backtest(bars_df, …)` accepts a pandas bars frame `[symbol,timestamp,open,high,low,close,volume]`; can carry extra columns (`si_dtc`,`si_util`,…) merged from the FINRA parquet.
- `python/src/atxpy/eval.py:1-67` — Sharpe / deflated-Sharpe / PBO / CPCV metrics.
- `python/scripts/download_finra_short_interest.py` — the parquet source to merge into bars.
**Design:** new `python/scripts/prescreen_alphas.py`: load ORATS bars + merge FINRA parquet → run each candidate alpha (price/IV/earnings/short-interest, factor-neutral variants) through `backtest.py` → rank by deflated-Sharpe / PBO from `eval.py` → print a shortlist the C++ discovery should seed. Offline research only; not on the determinism path.
**Determinism:** N/A (Python research tool, no engine artifacts).
**Tests (`python/tests/test_prescreen.py`):** a known-positive synthetic signal ranks above a known-noise signal; FINRA merge is causal (no future short-interest in a past bar); script runs end-to-end on a 1-symbol fixture.

---

## Phase B-exit — Joint acceptance

### B6 — Augmented, structured, regime-tested acceptance run
**Why:** prove the widened/structured signal set moves the headline number, and that it composes with Track A.
**Design:** a single reproducible harness:
1. Build the **Canonical Universe** panel (Track A's screen) → `research.bin`.
2. `atx augment` (B1) → `research_aug.bin` (short-interest fields appended).
3. Discovery seeded with `iv_earnings_templates.txt` (B2) + `neutralized_templates.txt` (B3).
4. Combine → optimize `--rebalance daily` → report. **If Track A's T1 cost wiring is present** in this worktree, charge `--cost-bps` and report **net**; else label **gross**.
5. `atx regime-oos` (B4) → per-regime + purged-embargo OOS report.
6. (Optional pre-step) `prescreen_alphas.py` (B5) to pick the seed shortlist.
**Acceptance:**
1. Discovery admits ≥1 alpha whose dominant field is short-interest/IV/earnings (the widened set carries through).
2. The augmented+neutralized book's **OOS Sharpe ≥ the price/vol-only baseline** on the same held-out window (the widening/structuring helps, or is a decisive null).
3. **Per-regime OOS Sharpe is positive in ≥2 of 3 vol terciles** (edge is not a single-regime artifact); purged-embargo OOS Sharpe stays positive.
4. Determinism intact: no-flag/no-seed/no-augment paths byte-identical; `oracle.hpp`/search-digest untouched; only `version_id` re-baselined by opt-in seeds/augment.
5. Files touched are exactly the Track-B set; `git diff --name-only` against Track A's branch shows **only `dispatch.cpp` in common** (different appended lines).

**Decisive-null clause (mirrors Track A):** if the augmented/neutralized/regime-tested book still has OOS Sharpe ≤ 0 net of cost, that narrows the next sprint to *signal transformation* (richer DSL ops / nonlinear features / ML alphas via `atxpy.learn`) rather than more raw fields.

---

## Sequencing

```
B0 (widen information set — independent of Track A)
  B1 FINRA loader + augment stage ──┐
  B2 IV/earnings seed family         │   (B1/B2 independent; B2 needs no B1)
        │                            │
        ▼                            ▼
B1 (structure)  B3 factor-neutral seed family   (needs B2's catalog convention)
        │
        ▼
B2 (eval)  B4 regime-conditional OOS analyzer   (independent; can start anytime)
        │
        ▼
B3 (loop)  B5 atxpy pre-screen                  (independent; can start anytime)
        │
        ▼
B-exit  B6 joint acceptance run                 (needs B1-B4; net number needs A-T1)
```

B4 and B5 are independent of B1–B3 and can be built first as the measurement scaffolding (build the *judge* before the *contestants*). B6 is the only task with a cross-track dependency (A's T1 for the net number) — and even that degrades gracefully to a labeled-gross result.

---

## Merge protocol (Track A ⇄ Track B)

1. **Single shared file: `dispatch.cpp`.** Both tracks only append subcommand routes. Resolve by taking both append blocks (Track B adds `augment` + `regime-oos`; Track A adds none per its plan, but if it does, different names → different lines).
2. **Seed fixtures.** Track B appends to `factor_templates.txt` and adds new fixture files; Track A never edits seed fixtures → no conflict.
3. **Panel digest.** Track B's augmented panel is a *new artifact* (`research_aug.bin`); it does not redefine Track A's `research.bin`. The merged acceptance run chains A's panel → B's augment.
4. **Determinism gates.** After merge, run the **factory golden-digest tests** and confirm green on every no-flag path — this is the joint proof that neither track perturbed the default search.
5. **Order of merge is irrelevant** (disjoint files); merge whichever lands first, rebase the other, re-run the digest gate.

---

## Notes for the SDD controller

- B1 is the only sizeable C++ build (loader + stage) — standard model implementer, opus review (causality / look-ahead is the failure mode; the append-only digest invariant is the determinism guard).
- B2/B3 are **text fixtures + parse tests** — cheap implementer; the value is in expression design, not code volume. B3 escalates to an opus implementer **only if** `cs_ops` must be extended (ISA change → golden-digest proof required).
- B4 is a read-only analyzer — standard implementer; correctness = it reconciles with `stage_report` on the degenerate (embargo=0, regime=all) case.
- B5 is Python research tooling — cheap; off the determinism path entirely.
- Every opt-in task keeps the no-flag/no-seed path byte-identical and proves it with the factory golden-digest tests staying green — same discipline as Track A.
- **Worktree:** create via the `superpowers:using-git-worktrees` skill before B1; branch `track-b/information-structure` off `main`.
