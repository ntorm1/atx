# Sprint Plan — Automate Conditioning/Capacity Alpha Discovery in the GA Search

**Date:** 2026-06-20 · **Base:** `main` (alpha101 research `3ea5b5a` + pipeline-remediation `dbdd784`)
**Origin:** the manual research in [single-alpha-capacity-findings.md](single-alpha-capacity-findings.md),
which hand-built a tradeable alpha — `group_neutralize(signedpower(zscore(-1*ts_std(returns,20)),2),sector)`
— at **Sharpe 1.56, daily turnover 0.15, ~1,118 names/day at $50M+ ADV** on the real liquid panel.

**Goal:** make the genetic-algorithm search **discover alphas of this class automatically** — capacity-aware,
low-turnover, economically-sensible, robustly non-degenerate — instead of requiring a human sweep.

**Relationship to the existing roadmap:** complementary, not overlapping.
[tradeable-mega-alpha-roadmap.md](../../docs/superpowers/plans/2026-06-20-tradeable-mega-alpha-roadmap.md)
covers robustness accounting (Phase R), accumulation (M), covariance sizing (S), and a tradeability/cost
phase (T). This plan is the **missing discovery-side phase** — call it **Phase W (Weighting + Universe +
Seeding)** — that lets the GA *find* the alphas Phase T then sizes and trades. Where this plan and the
roadmap touch the same file (turnover, capacity), the tasks are noted as shared.

---

## 1. The manual process, written as an algorithm

The hand search that produced the 1.56 alpha was six repeatable steps:

1. **Define a capacity universe** — `in_universe ∧ close>$1 ∧ adv20≥$50M` (~1,118 names). Tradeable AND de-noised.
2. **Seed from economic priors** — start from documented anomalies (low-vol, momentum, reversal, 52-week-high,
   BAB), not random trees.
3. **Search the SIGNAL** — window sweeps (`ts_std` 10/20/40/60/126/252), sector-neutralization.
4. **Search the WEIGHTING/CONDITIONING** — the decisive step: the *same* low-vol signal scored
   0.44 (flat rank) → 0.87 (zscore) → 1.56 (`signedpower(z,2)`). Weighting tripled Sharpe.
5. **Tune the concentration knob** — `signedpower(z,p)`, p∈{1,1.5,2,2.5,3,4}: a smooth Sharpe↔turnover/dd dial.
6. **Verify non-degeneracy cheaply** — split-sample Sharpe (both halves positive) + monotone concentration
   frontier (Sharpe rises smoothly with p, no spike) + window robustness. All cheap, no OOS needed.

**Each step is currently un-automatable in the GA. Sections 2–3 say why; Section 4 turns them into tasks.**

---

## 2. GA pipeline review — current behavior (precise)

Confirmed by reading the discovery + fitness + genome code. The trading book and the search alphabet are
the bottlenecks, not the signal grammar.

| # | Area | Current behavior | File:line | Gap vs the manual process |
|---|------|------------------|-----------|---------------------------|
| 1 | **Book / weighting** | Signal→weights via a **fixed** `WeightPolicy`: winsorize @2.5% → **Rank** (default) → dollar-demean → gross-normalize. Default-constructed in discovery; **no CLI/config knobs, no raw/passthrough mode.** | `loop/weight_policy.hpp` (transform=Rank, winsorize_limit=0.025, ~:163-183); `stage_discover.cpp:354` (`WeightPolicy policy{}`) | **The headline blocker.** The policy *re-ranks* every signal → washes out any in-expression concentration; it *winsorizes the tails* → clamps the exact extreme-vol premium worth +0.5 Sharpe. A perfect low-vol signal still scores 0.44 because the book re-ranks it. Steps 4–5 are impossible. |
| 2 | **Operator alphabet** | GA can reach `zscore, rank, scale, winsorize, signedpower, power, group_neutralize/indneutralize, ts_std, ts_mean, delay, delta, correlation` (all IN). Hparam-peel ops excluded. | `registry.cpp:13-172`; `op_catalog.hpp:110-156`; `generate.hpp:47-57` | Alphabet is fine — the conditioning ops exist. The problem is reaching the right *composition* (see #3, #5). |
| 3 | **Mutation can't wrap** | Mutation = `op_swap` (same shape/dtype/arity bucket), `field_swap`, `jitter_const`. **No "wrap subtree in op".** A good signal can only be wrapped by random grammar generation or a crossover splice. | `mutation.hpp:14-19`; `op_catalog.hpp:88-91` | Step 4 needs to take a discovered signal and wrap it in `zscore(...)` / `signedpower(...,p)`. Mutation cannot create that structure → the conditioning lift is found only by luck. |
| 4 | **Constant search** | Window args jittered (log-normal σ=0.5, clamped [1,max_lookback], annealed); other numeric literals (`Scale`, e.g. the exponent of `signedpower`) jittered multiplicatively, unbounded. | `mutation.hpp:52-64`; `search_driver.cpp:904-913` | Good: IF a `signedpower(x,p)` exists, p **is** tunable (step 5 works). But it can never be introduced (#3). Wrap + existing jitter together unlock the concentration frontier. |
| 5 | **Seeding** | Gen-0 = `--seed-expr` strings (parsed) + grammar-sampled trees. **No fixture/template-library seeding.** | `search_driver.cpp:341-421` | Step 2 (economic priors) is only reachable by passing seeds one CLI flag at a time. No curated factor-template library. |
| 6 | **Universe** | Scored on the full `panel.in_universe` (~14,633 names). **No price/ADV/liquidity screen anywhere in discovery.** Cost objective exists but only when `target_aum>0`, and it's a Pareto nudge, never a universe filter. | `panel.hpp:166-172`; `streams.hpp:175-214`; `fitness.hpp:225-231` (`target_aum=0` default) | Step 1 (capacity universe) absent. The GA optimizes over illiquid names it can't trade and that add noise. |
| 7 | **Fitness / turnover** | WQ fitness `= sqrt(\|ret\|/max(turnover,0.125))·sharpe`, OOS-averaged, ×diversify×robust. Turnover **is** in the denominator (low turnover mildly rewarded) but floored at 0.125; loose hard gate `max_turnover=0.70`. | `combine/metrics.hpp:203-208`; `gate.hpp:70,113-114` | Turnover present but weak; 0.70 gate is far looser than the 0.30 capacity bar. `robust` factor is **inert** (weak_panel=nullptr). Shared with roadmap T1. |
| 8 | **Robustness filters** | DSR computed + **gated** (cheap, O(1)). PBO computed but **not gated**. No split-sample stability, no concentration-monotonicity check. | `fitness.cpp:287-304` (DSR); `pbo.hpp` (ungated) | Step 6's cheap non-degeneracy checks (split-half, monotone frontier) don't exist as early filters. |

---

## 3. The headline root cause

> **The GA cannot discover the alpha the manual process found, because the fixed `WeightPolicy` (winsorize@2.5% → Rank) overwrites the signal's conditioning and clamps its tail premium — and mutation cannot introduce the conditioning ops that beat Rank.**

Everything else (capacity universe, seeding, robustness) accelerates and de-risks the search; this one
structurally *caps* it. Concretely, on the manual evidence, the fixed policy holds every candidate at the
**0.44 (flat-rank) ceiling** of a signal whose reachable ceiling is **1.56**. Fixing the weighting is the
single highest-leverage change and gates the value of the rest.

---

## 4. Phase W — tasks

Determinism discipline (same as the remediation/roadmap sprints): every new path is **opt-in / flagged with a
byte-identical default**; stage digests fold the new knobs; `oracle.hpp` untouched; no golden re-baseline.

### W1 — Make the weighting/conditioning searchable *(highest leverage; do first)*
Two sub-parts; W1a is the unblock, W1b is the search.

- **W1a — Configurable WeightPolicy + a raw/passthrough mode.**
  Expose `transform` (Rank | ZScore | **Raw**), `winsorize_limit`, `industry_neutral`, `gross_leverage` as
  config/CLI knobs threaded into `stage_discover`'s `WeightPolicy` (today `WeightPolicy policy{}`,
  `stage_discover.cpp:354`). Add a **Raw/passthrough** transform that does NOT re-rank or winsorize, so an
  expression's own conditioning (`zscore`, `signedpower`, `winsorize`) survives to the book.
  - Files: `loop/weight_policy.hpp` (add `Transform::Raw`, skip winsorize when limit==0), `stage_discover.cpp`,
    `config.hpp` (flags), digest fold.
  - Acceptance: `--weight-transform raw --winsorize-limit 0` reproduces the alpha101 harness book bit-for-bit
    (pin against `single_alpha_capacity_test.cpp`'s demean+L1 path); default run byte-identical to today.

- **W1b — `wrap_in_op` mutation operator.**
  New mutation: pick a subtree, sample a shape/dtype-compatible **cross-sectional/elementwise wrapper**
  (`zscore`, `rank`, `winsorize`, `signedpower(·,p)`, `group_neutralize(·,sector)`), rebuild with the subtree
  as the wrapper's primary operand, re-analyze for validity. Wire into the adaptive-operator credit set.
  - Files: `mutation.hpp` (new op alongside op_swap/field_swap/jitter), `search_driver.cpp` (register +
    adaptive credit), `op_catalog.hpp` (wrapper candidate set).
  - Acceptance: starting from the genome `-1*ts_std(returns,20)`, the search can reach
    `signedpower(zscore(-1*ts_std(returns,20)),p)` within a bounded number of generations on a seeded run;
    `wrap_in_op` disabled → byte-identical to today.

### W2 — Capacity universe for discovery *(shared intent with roadmap T2)*
Add an optional capacity screen applied to the scoring universe: `in_universe ∧ close>min_price ∧
adv{W}≥min_adv`. Implement as a derived universe mask the discovery eval uses (the same screen
`single_alpha_capacity_test.cpp::capacity_universe` applies), gated by flags
(`--min-price`, `--min-adv`, `--adv-window`), default OFF (full universe = byte-identical).
- Files: `stage_discover.cpp` (build the mask, pass to the eval/universe), `panel`/`streams` universe plumbing,
  `config.hpp`.
- Acceptance: with `--min-adv 50e6 --min-price 1`, discovery scores ~1,000–1,200 names/day on the liquid panel;
  flags absent → identical to today. (Cleaner signal + only tradeable names.)

### W3 — Economic factor-template seeding
Seed gen-0 from a curated DSL **template library** (a fixture file of documented anomalies: low-vol, 12-1
momentum, 52-week-high, long-term reversal, BAB-via-`vec_avg`, plus their `zscore`/`signedpower`/sector-neutral
conditioned forms), in addition to grammar fill. Add `--seed-file <path>` reading the same `<id>: <dsl>` format
as `alpha101.txt`.
- Files: `search_driver.cpp:341-421` (seed-from-file path), a new `fixtures/factor_templates.txt`, `config.hpp`.
- Acceptance: a run with `--seed-file factor_templates.txt` starts with the templates as gen-0 members
  (verified in the population dump); empty/absent file → today's behavior.

### W4 — Cheap non-degeneracy early filters
Add two O(T) filters computed alongside DSR, gated for capacity/tradeable runs:
- **Split-sample stability:** first-half and second-half Sharpe both > 0 (and same sign as full). Reject
  single-regime artifacts before the expensive OOS/PBO stage.
- **Concentration-frontier monotonicity (optional, for `signedpower` genomes):** when a candidate carries a
  tunable concentration power, sample 2–3 neighboring powers; require Sharpe to move monotonically (no isolated
  spike) — the manual non-degeneracy test.
- Files: `fitness.cpp` (compute split-half Sharpe near `:287-304`), `gate.hpp` (record + optional floor),
  `config.hpp` (`--min-split-sharpe`, default off).
- Acceptance: a candidate with H2 Sharpe < 0 is rejected under the flag; flag off → byte-identical. (Also
  *activate* the inert `robust` factor by wiring a weak/holdout universe — overlaps roadmap R/T.)

### W5 — Turnover/capacity gate for tradeable runs *(shared with roadmap T1)*
For capacity runs, tighten `max_turnover` to a real bar (e.g. 0.30 two-sided) and surface turnover + the
capacity-universe name count as recorded admission metrics. Keep the default 0.70 so non-capacity runs are
unchanged. (The WQ-fitness turnover denominator already nudges low turnover; this makes the bar match the
capacity thesis.)
- Files: `gate.hpp`, `config.hpp`, manifest reporting.
- Acceptance: `--max-turnover 0.30` rejects fast (reversal-class) genomes; default run unchanged.

### W6 — Validation: auto-rediscover the manual alpha *(the acceptance test for the whole phase)*
An integration test / scripted run that, with W1–W5 enabled (raw weighting, capacity universe, factor-template
seeding, split-sample gate), recovers a low-vol-conditioning alpha with **Sharpe>1 and turnover<0.30** on the
real liquid panel **without it being a seed** (seed momentum/reversal templates only; require the search to
*reach* the low-vol-conditioned family via `wrap_in_op` + window jitter). Compare the discovered alpha's metrics
to the manual `lv_z_p2.0` baseline.
- Files: a new `*_test.cpp` (mirrors `single_alpha_capacity_test.cpp` harness) or a `sweep` smoke run.
- Acceptance: at least one discovered, non-seeded alpha clears Sharpe>1 ∧ turnover<0.30 ∧ both-halves-positive
  on the real panel; documented in an updated findings note.

---

## 5. Sequencing

1. **W1a + W1b** (configurable/raw WeightPolicy + `wrap_in_op`) — removes the structural cap. Validate the raw
   policy reproduces the alpha101 book bit-for-bit first.
2. **W2** (capacity universe) — tradeable + de-noised scoring.
3. **W3** (template seeding) — head-start from economic priors.
4. **W4 + W5** (cheap robustness + turnover bar) — keep the now-larger candidate stream honest and tradeable.
5. **W6** — prove the loop closes by auto-rediscovering the alpha.

W1 is the gate: until weighting is searchable, W2/W3 only feed candidates into a book that re-ranks away their
edge. Run W6 after each of W1→W5 as a regression to watch the discoverable ceiling climb (expected:
0.44 → ~0.9 after W1 → >1 after W2/W3 with `wrap_in_op` reaching the concentration).

## 6. Execution method

Per repo norm: decompose into per-task briefs and run **subagent-driven-development** (fresh implementer → task
review spec+quality → fix loop → ledger → final whole-branch review → finishing-a-development-branch). Each task
opt-in/flagged with a byte-identical default; determinism digests fold every new knob.

## 7. Open questions (resolve at brief time)

- **W1a:** does a `Raw` transform interact correctly with the existing dollar-neutral/gross-normalize stages, or
  should Raw bypass those too? (The manual book = demean + L1 only — match that.)
- **W1b:** wrapper-op candidate set and depth/▢complexity budget so `wrap_in_op` doesn't bloat trees; how it
  composes with parsimony.
- **W2:** apply the capacity mask to the scoring universe only, or also to the diversify/corr-pool? Interaction
  with `target_aum` cost (avoid double-counting capacity).
- **W3:** template library scope — OHLCV-only factors first; include the conditioned forms or let `wrap_in_op`
  discover conditioning from raw-signal seeds (cleaner test of W1b)?
- **W4:** split-sample split point (halves vs walk-forward) and its relationship to roadmap R2's rotating
  holdout — share the machinery.
- **Cross-cutting:** W2/W5 overlap roadmap T1/T2 and W4 overlaps R/S — coordinate file touch-points to avoid
  conflicting edits to `gate.hpp`/`fitness.cpp`/`config.hpp` if both sprints run.

## 8. Why this is the right next step

The alpha101 research stream proved the *edge* lives in conditioning + capacity + the low-vol family; the
remediation stream made the search *produce candidates*; the roadmap makes the pool *trustworthy and tradeable*.
This phase closes the loop: it lets the **search itself** traverse the conditioning/capacity space a human had to
walk by hand — turning a six-round manual sweep into one flagged discovery run.
