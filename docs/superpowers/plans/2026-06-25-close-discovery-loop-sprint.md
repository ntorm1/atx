# Sprint Plan — Close the Discovery Loop

**Date:** 2026-06-25
**Author:** diagnostic + plan by Claude (Opus 4.8), for review
**Status:** DRAFT — awaiting review before execution
**Sprint goal (one line):** Make the discovery pipeline find *and admit* the known-tradeable
liquidity alpha (`illiq`, Sharpe 1.22 / turnover 6% on the liquid universe), turning the
empirical "why can't 21 minutes produce a tradeable alpha?" finding into a closed loop.

---

## 0. Why this sprint exists — the empirical diagnosis

This sprint is grounded in live measurement on the real 10.5-year panel
(`work/accept/panel.bin`, 6,431 names × 2,627 days × 12 fields), run 2026-06-25 against a
freshly built `main` (post R1–R4). The diagnostic overturned the stale doc narrative.

### 0.1 What is already fixed (verified in current code — NOT this sprint's job)
The diagnostic docs (`PIPELINE_REMEDIATION_PLAN.md`, `STATUS.md`, phaseD findings,
mega-roadmap) describe a *historical* pipeline. The following "confirmed defects" are
**already fixed on main** and must not be re-touched:

| Stale claim | Live reality (verified) |
|---|---|
| Cost model unwired; all Sharpe gross | `--cost-bps` flows to per-period turnover charge + `round_trip_cost_bps` (`stage_optimize.cpp:134,175,209`) |
| Stagnation early-kill wastes ~60% budget | Early-stop requires `best_flat AND mean_flat`; old bug documented in code (`search_driver.cpp:316-338`) |
| Library wiped each run (`remove_all`) | No `remove_all` in `stage_discover.cpp`; cross-run accumulation guard present |
| No portfolio OOS Sharpe | walk-forward OOS telemetry shipped (T7 / Phase D) |
| Field-type / price-scale / deflation holes | R1–R4 shipped (`--typed-fields`, `--reject-price-scale`, `--dsr-subwindows`, `--deflate-selection`) |

### 0.2 The live experiments and their results

| Experiment | Configuration | Result |
|---|---|---|
| Capacity sweep, 9 canonical price/vol factors | screened universe (~1,151 liquid names) | best Sharpe 0.26; **all** show H1<0<H2 regime split; full-sample ≈ 0 |
| Capacity sweep, 8 orthogonal factors | same | **`illiq = group_neutralize(zscore(-1*adv20),sector)` → Sharpe 1.22, H1 1.07, H2 1.83, turnover 0.061 — the only clean pass (Sharpe>1 & turnover<0.30)** |
| `discover --gated` strict R1–R4 profile | default (full 6,431-name) universe | **0 admits** |
| `discover --gated` loose, illiq-seeded | full universe | admits only **degenerate zero-metric junk** (`-(sign(high))`, `log(sign(vec_sum(close)))`); illiq → pre-screen-fail bucket, `oos_sharpe ≤ 0` |
| `discover --gated` strict + capacity screen + illiq-seed | screened universe | **0 admits** — illiq rejected even where its capacity Sharpe is 1.22 |
| `discover` modest population (≥12) | 1.6 GB panel | **OOM-killed** (exit 127) |

Reject histogram captured on the loose full-universe run:
`reject_histogram = 3,0,0,5,0,0,0,0` (Accept=3 — all degenerate; 5 seeds in the
pre-screen-fail/sentinel bucket), `evaluated=14`.

### 0.3 The five compounding root causes (ranked by leverage)

1. **The easily-searchable axis is edge-poor on this period.** Price/vol volatility factors
   are regime-fragile (negative 2016–2020, strong 2021+, full-sample ≈ 0). Real information
   poverty in that axis; more compute cannot manufacture edge that is not present.
2. **The search runs on the wrong universe.** discover defaults to the full 6,431-name
   universe. The illiquidity premium lives only in the ~1,150 liquid names; on the full
   universe illiq's factory OOS Sharpe is ≤ 0 (micro-cap noise). The capacity screen
   (`--min-adv`/`--min-price`) exists but is not in the default/canonical search path.
3. **The seed catalog points away from the edge.** `factor_templates.txt` is entirely
   price/return factors — zero liquidity, zero IV-surface. The GA warm-starts into the
   regime-fragile neighborhood.
4. **Compute starvation.** ~0.8 cand/s, ~630 distinct evals over a ~10¹² program space
   (~10⁻¹⁰ coverage); OOMs at pop≥12. Cannot discover `zscore(adv20)` de novo.
5. **Admission-metric misalignment.** The factory admits on the OOS-holdout (last 25% + DSR
   + 3 subwindows + split + its own book construction), strict enough that even illiq
   (full-sample 1.22, both halves positive) does not clear. The R3 `--dsr-subwindows` gate
   may over-reject regime-dependent-but-tradeable factors. **This is the crux blocking
   "pipeline admits a known-tradeable factor."**

### 0.4 Headline
A tradeable alpha **does exist in the current data** (illiq). 21 minutes finds nothing
because the search starts in the wrong neighborhood (seeds), on the wrong universe
(unscreened), too starved to escape (compute), and the admission metric rejects even the
good factor when handed it (metric misalignment + R3 strictness).

### 0.5 Scope of THIS sprint
Causes **2, 3, and 5** — the alignment + admission-metric work that is cheap, mostly
zero-determinism-risk, mine to own, and directly testable ("does the pipeline now admit
illiq?"). Causes **1** (information breadth → Track B) and **4** (compute → P5) are
sequenced as a roadmap appendix (§6); they are larger and gated on decisions outside this
sprint. This sprint is runnable WITHOUT P5: acceptance seeds illiq directly at small
population, so it does not need de-novo discovery throughput.

---

## 1. Global Constraints (BINDING — copy verbatim into every task brief and reviewer prompt)

This sprint touches the search/admission path, which carries pinned determinism digests.
The following is the same contract enforced through R1–R4 and is non-negotiable.

- **Flag-absent / default path BYTE-IDENTICAL.** Any new behavior is opt-in and default-OFF.
  When the new flag/config is absent: ZERO new computation, ZERO new persisted bytes; the
  pinned F1 search digest (`SearchResult::digest`), the admission digest, the MultiObjective
  and ScalarRaw goldens, and existing OOS golden/determinism digests are UNCHANGED.
- **`atx-engine/tests/factory/oracle.hpp` UNTOUCHED.** No pinned golden literal edited. The
  only sanctioned re-baseline is `version_id` via an opt-in admission-changing flag, and only
  with explicit reviewer sign-off in that task.
- **seq == parallel invariant.** `Factory::mine_into_oos` (serial) and
  `mine_into_oos_parallel` must gate identically. Any value computed at admission/fitness time
  must be a pure function of the candidate + a serially-determined scalar — never worker
  id/order/count. Existing seq==parallel and twice-run tests must pass with any new flag ON.
- **NEVER `git add -A`** (stage explicit paths). **NEVER push.** Commit trailer EXACTLY:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- **Three test classes for any admission-changing flag:** (a) off-path byte-identity,
  (b) on-path genuine RED→GREEN (digest/admit diverges), (c) twice-run reproducibility, plus
  seq==parallel where the admission path is touched.
- **Pre-existing out-of-scope failures:** 3 atx-impl test failures unrelated to this work;
  do not chase them.
- **Process:** Subagent-Driven Development. Per task: implementer (model per §5) →
  review-package → task reviewer (spec + quality + determinism) → fix loop (Critical/Important
  only) → ledger update. Final whole-branch review on the most capable model. Ledger at
  `.superpowers/sdd/progress.md`.

---

## 2. Sprint architecture

```
Phase A  (alignment; config + fixture; zero engine-digest risk)
  A1  Capacity screen in the canonical search profile
  A2  Liquidity + orthogonal seed catalog
        |
Phase B  (observability + admission-metric audit; the crux)
  B1  Make the discovery funnel observable on zero-admit (reject_histogram + per-seed metrics)
  B2  Reconcile capacity-test Sharpe vs factory OOS-holdout metric for illiq (diagnostic)
  B3  Recalibrate the admission metric / R3 subwindow gate  (opt-in; gated on B2 findings)
        |
Phase C  (close the loop)
  C1  End-to-end acceptance: discover→combine→optimize(--cost-bps)→report on screened
      universe + liquidity seeds; net-of-cost portfolio OOS Sharpe (or documented null)
```

Dependency: A1+A2 are independent and can land first. B1 precedes B2 (B2 needs the
observability). B3 is gated on B2's verdict (only if B2 proves over-rejection). C1 requires
A1+A2+B3.

---

## 3. Tasks (detailed)

### Task A1 — Capacity screen in the canonical search profile
**Problem.** discover/sweep default to the full 6,431-name universe; the tradeable edge lives
in the ~1,150 liquid names. The screen flags exist (`--min-price`, `--min-adv`/`--min-adv-usd`,
`--adv-window`; `config.cpp:156-160`) and the W2 derived-panel screen works
(`[W2 capacity screen] ... ~1151 names/day`), but they are not in the recommended discovery
invocation or the acceptance script's `sweep` stage.

**Spec.**
- Do NOT change any default in engine/config (that would be a digest event). Instead:
  - Add the capacity-screen flags to the canonical `sweep` invocation in
    `scripts/canonical-acceptance-run.ps1` (it currently screens only at the `panel` stage;
    the `sweep`/discover search must screen too, OR consume a pre-screened panel).
  - Add params `[double]$MinPrice = 1.0`, `[double]$MinAdvUsd = 5e7`, `[int]$AdvWindow = 20`
    and thread `--min-price --min-adv $MinAdvUsd --adv-window $AdvWindow` into the sweep stage.
  - Document in the script NOTES that the search universe must equal the tradeable universe.
- Fix the latent script bug found during diagnosis: the `sweep` stage in the acceptance script
  is missing `--alpha-out` and `--seed-file`/`--seed-expr`, so it would fail
  ("sweep: --panel and --alpha-out required", "at least one --seed-expr required",
  `stage_sweep.cpp:60-70`). Add both.
**Determinism.** Script-only (no engine bytes). Zero digest impact.
**Tests.** A PowerShell parse check (ParseFile clean, no non-ASCII — the prior portability
trap) + a dry-run assertion that the composed sweep command contains the screen flags. No
engine test.
**Anchors.** `config.cpp:156-160`; `stage_discover.cpp` W2 screen block (search "W2 capacity
screen"); `stage_sweep.cpp:60-70`; `scripts/canonical-acceptance-run.ps1:86-112`.

### Task A2 — Liquidity + orthogonal seed catalog
**Problem.** The warm-start catalog omits the axes that carry edge. The GA never starts near
the liquidity factor.
**Spec.**
- Add a committed seed fixture `atx-impl/tests/fixtures/orthogonal_templates.txt`
  (`<id>: <dsl>` format, same as `factor_templates.txt`) with the empirically-motivated
  orthogonal seeds, verbatim DSL confirmed to parse + analyze on the panel:
  - `illiq1: group_neutralize(zscore(-1*adv20),sector)`  (the Sharpe-1.22 winner)
  - `illiq2: group_neutralize(zscore(-1*dollar_volume),sector)`
  - `illiq3: rank(-1*adv20)`
  - `iv_term: group_neutralize(zscore(atmCenI_126d - atmCenI_21d),sector)`
  - `iv_vrp:  group_neutralize(zscore(atmCenI_21d - ts_std(returns,21)),sector)`
  - `iv_lo:   group_neutralize(zscore(-1*atmCenI_21d),sector)`
  - keep 2–3 price/vol controls (low-vol p3, 12-1 momentum) for diversity.
- Decide the disposition of the uncommitted diagnostic change (8 candidate rows the probe
  added to `single_alpha_capacity_test.cpp`): either (a) keep them as a committed
  regression-style "orthogonal axis" probe (recommended — it is how illiq was found and it
  pins the regime-split finding), or (b) revert. This is a §4 decision.
**Determinism.** Fixture-only; not read on any default path (only when `--seed-file` points at
it). Zero digest impact.
**Tests.** A unit test that `read_seed_file` parses all entries and each analyzes clean
against the discovery Library (mirror any existing seed-file test). If the diagnostic test
rows are kept, the existing capacity test already exercises them.
**Anchors.** `config.cpp:69-80` (`--seed-expr`/`--seed-file`, `read_seed_file` at `:424-441`);
`atx-impl/tests/fixtures/factor_templates.txt`; `single_alpha_capacity_test.cpp:80-107`.

### Task B1 — Make the discovery funnel observable on zero-admit
**Problem.** When zero alphas clear the gate, `run_discover_gated` returns `Err`
(`stage_discover.cpp:578`) BEFORE building the reject-histogram string (`:583-587`) or writing
the manifest (`:619-630`). The operator is blind to *why* nothing admitted — the single most
expensive gap in this whole investigation.
**Spec.**
- On the zero-admit path, before returning, emit to stderr (and/or a `_manifest.txt` written
  even on zero-admit): `evaluated`, `duplicates`, the full `reject_histogram` (indexed by
  `library::AdmitKind`, array<8>), and per-seed one-line OOS diagnostics
  (`is_sharpe/oos_sharpe/oos_turnover/dsr` per evaluated seed) — the same fields already
  printed on the success path (`stage_discover.cpp` manifest block / the loose-run output we
  captured). Keep returning non-zero exit (zero-admit is still a failure) but make it
  *observable*.
- This is pure observability: it changes only diagnostic output, not admission logic, not any
  digest. Guard it so the success-path output is byte-identical to today.
**Determinism.** No admission/digest change. The success path stays byte-identical; new bytes
appear only on the previously-output-less zero-admit failure path.
**Tests.** (a) A test that a zero-admit gated run (constructed to admit nothing) writes a
manifest containing `reject_histogram=` and at least one `alpha[..] oos_sharpe=` line.
(b) Byte-identity of the success-path manifest vs a golden (admit case unchanged).
**Anchors.** `stage_discover.cpp:578` (the Err), `:583-587` (rej string), `:619-630` (manifest
writer), `:628 evaluated`/`:630 reject_histogram`; `factory.hpp:253` (array<8>);
`library::AdmitKind` enum order [Accept,Duplicate,RejectSharpe,RejectFitness,RejectTurnover,
RejectCorrelated,RejectPriceScale,RejectDsrSubwindow].

### Task B2 — Reconcile capacity-test Sharpe vs factory OOS-holdout metric (diagnostic)
**Problem.** illiq scores Sharpe 1.22 on the capacity test's book (full sample, daily
dollar-neutral, gross-1, L1-normalized, PnL at d+1, screened universe) but is rejected by the
factory's OOS-holdout admission (`oos_sharpe ≤ 0` on full universe; still rejected on screened
universe). We must localize the gap before changing any gate.
**Spec.** Pure diagnostic + test, NO production change:
- Add a focused test (e.g. `factory_metric_alignment_test.cpp`) that, on the screened panel,
  evaluates the SAME illiq expression through both paths and prints, side by side:
  - capacity-test book metrics (reuse the capacity-test helper or replicate its recipe), and
  - the factory OOS-holdout path: in-sample Sharpe, holdout Sharpe, de-annualized
    per-period Sharpe, DSR (with trial_count), the 3 sub-window DSRs, split_ok, turnover,
    price_scale loading, and which `AdmitKind` bucket it lands in.
- Identify the divergence dimension(s): (i) full-sample vs last-25%-holdout window, (ii) book
  construction / neutralization differences, (iii) DSR de-annualization (`/ sqrt(252)`),
  (iv) the 3-subwindow AND requirement, (v) the price-scale loading (illiquidity correlates
  with low price → R2 may flag it), (vi) turnover definition.
- Output: a short written finding in `.superpowers/sdd/briefs/B2-report.md` ranking the
  divergence causes by contribution, with the per-dimension numbers.
**Determinism.** Test-only; touches no production code, no digest.
**Tests.** The diagnostic test itself (asserts both paths run and prints the metrics; may
`GTEST_SKIP` the verdict when `ATX_ALPHA101_PANEL` is unset, mirroring the capacity test).
**Anchors.** `factory.cpp:413` (pre-screen `dsr >= min_dsr && split_ok`); `factory.cpp:1219-1265`
(serial subwindow block) and `:1639-1686` (parallel mirror); `holdout_dsr` recipe (drops
structural zero, `/sqrt(252)`, trial_count); `single_alpha_capacity_test.cpp` book recipe
(~`:200-285`); R2 `price_scale_loading` (`factory.cpp:1211`).

### Task B3 — Recalibrate the admission metric / R3 subwindow gate (opt-in; gated on B2)
**Problem.** If B2 proves the strict admission metric over-rejects a genuinely tradeable,
both-halves-positive factor, the gate is mis-calibrated for regime-dependent edge.
**Spec (only the parts B2 justifies; each opt-in, default-OFF):**
- If the 3-subwindow AND is the culprit: add an opt-in tolerance — e.g. `--dsr-subwindows-min-pass <K>`
  requiring K-of-N subwindows to clear rather than all-N (default = N = today's behavior =
  byte-identical). OR allow the subwindow bar to be a fraction of `min_dsr`.
- If the holdout window (last 25%) is the culprit vs full-sample robustness: consider an
  opt-in walk-forward admission that credits a factor positive across BOTH in-sample halves
  AND the holdout (not just the tail), behind a flag.
- If R2 price-scale is (correctly) flagging illiquidity as a low-price tilt: this is a
  *legitimate* rejection — document it; the answer is then a price-neutral illiquidity
  construction (e.g. `rank(-adv20)` residualized on price), added to the seed catalog (A2),
  NOT a gate change.
**Determinism.** Admission-changing → strict opt-in/default-OFF per §1, with the three test
classes + seq==parallel. If any change re-baselines `version_id`, that is its own reviewed
sub-step with explicit sign-off. Off-path byte-identity proven by golden.
**Tests.** off-path byte-identity (flag absent ⇒ pinned digest unchanged); on-path RED→GREEN
(flag on ⇒ illiq admits, digest diverges); twice-run reproducibility; seq==parallel with flag
on.
**Anchors.** same as B2; `factory.hpp` `dsr_subwindows` field + array<8> histogram;
`config.cpp:319` valueless-flag list; `library.hpp` `AdmitKind` (extend only if a new reject
kind is genuinely needed — prefer reusing existing buckets).

### Task C1 — End-to-end acceptance (close the loop)
**Problem.** Prove the composed fix produces a tradeable number (or a clean decisive null).
**Spec.** A reproducible run script (extend `canonical-acceptance-run.ps1` or a new
`scripts/close-loop-acceptance.ps1`):
- discover/sweep with: capacity screen (A1) + liquidity seeds (A2) + B3's calibrated profile,
  on `work/accept/panel.bin`, small population (illiq is seeded directly — no de-novo
  discovery needed), strict gates otherwise.
- combine `--conviction` → optimize `--rebalance daily --cost-bps 10` → report.
- Print: admitted count, the admitted expressions, net-of-cost portfolio OOS Sharpe,
  oos_turnover, breadth_effective_n.
**Acceptance criteria (either is a successful sprint outcome):**
- GREEN: ≥1 illiq-class alpha admitted with holdout OOS Sharpe > 0 and turnover < 0.30; net
  portfolio OOS Sharpe > 0. The pipeline produces a tradeable alpha.
- DOCUMENTED NULL: if still zero/negative after A1+A2+B3, B2's report localizes the exact
  remaining cause (information vs metric vs compute), which deterministically selects the next
  roadmap item (§6).
**Determinism.** Script + run only; no engine bytes beyond B-phase flags (all opt-in).
**Note on compute.** C1 runs at small population by design (seeded). A meaningful *de-novo*
21-minute run is gated on P5 (§6) to fix the pop≥12 OOM.

---

## 4. Decisions required before / during execution (for review)

1. **Diagnostic test rows.** Keep the 8 orthogonal-factor candidate rows the probe added to
   `single_alpha_capacity_test.cpp` (recommended — pins the regime-split + illiq findings as a
   committed probe), or revert them? (Folded into A2.)
2. **Scope confirmation.** This sprint = causes 2/3/5 (alignment + admission metric). Confirm
   Track B (cause 1) and P5 (cause 4) stay as the §6 roadmap, not in this sprint.
3. **B3 trigger.** B3 only proceeds if B2 proves over-rejection. If B2 finds the rejection is
   *legitimate* (e.g. illiq is a real low-price tilt that R2 correctly flags), B3 becomes
   "add a price-neutral illiquidity seed" (A2) instead of a gate change. Confirm this
   evidence-gated branch.
4. **version_id re-baseline.** If B3 changes admission behavior on a non-opt-in path, it
   re-baselines `version_id`. Confirm the policy: opt-in flag (preferred, no re-baseline) vs a
   sanctioned `version_id` bump.

---

## 5. Model selection (SDD)

| Task | Model | Rationale |
|---|---|---|
| A1, A2 | cheap tier | mechanical: script params + fixture file + parse tests |
| B1 | mid tier | touches stage output; needs care for success-path byte-identity |
| B2 | mid/high tier | diagnostic reasoning across two metric recipes |
| B3 | high tier | determinism-sensitive admission change |
| C1 | mid tier | run orchestration + reporting |
| Final whole-branch review | most capable | determinism audit across the branch |

---

## 6. Roadmap appendix — causes 1 and 4 (sequenced, NOT this sprint)

- **Track B — information breadth (cause 1).** FINRA short interest (`si_dtc/si_util/si_chg`),
  IV/earnings seeds, factor-neutral seeds, regime-OOS analyzer — built in
  `.claude/worktrees/track-b-information-structure` (B1–B4 done, 8 tests green), NOT on main.
  Adds new panel fields → schema/digest event → **cross-track + determinism decision (user)**.
  Trigger: C1 yields a documented null pointing at information (not metric/compute).
- **P5 — compute (cause 4).** Fixes the pop≥12 OOM and the ~0.8 cand/s throughput
  (10²–10³× target); worktree `p5-sprint1-perf`, S1-0 done. Trigger: a meaningful de-novo
  21-minute discovery run is needed (after C1 proves the seeded loop closes).
- **Regime-conditioning (bonus).** `RegimeCombiner` exists but is unwired; harvest the strong
  H2-regime price/vol edge the full-sample gate discards. Trigger: after C1, if regime-split
  factors are worth salvaging.

---

## 7. Definition of done (this sprint)

- A1+A2+B1 landed (cheap, zero-digest), each review-clean.
- B2 report localizes the capacity-vs-factory metric gap with numbers.
- B3 (if triggered) lands opt-in/default-OFF with all determinism tests green; off-path
  byte-identity proven; oracle.hpp untouched.
- C1 produces either a GREEN tradeable number or a DOCUMENTED NULL that selects the next
  roadmap item.
- Final whole-branch review: MERGE-READY; determinism PASS; commit trailers correct; not
  pushed.
