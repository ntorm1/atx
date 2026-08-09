# Sprint 7 — Tradeable-Alpha Build: Results & Frontier

**Run dates:** 2026-06-27/28. **Branch:** `main` (p6 Sprint 7, subagent-driven).
**Harness:** `scripts/build-tradeable-alphas.ps1`. **CLI:** `atx-impl` @ `build/bin/atx-impl.exe`.

> **Verdict: documented frontier (honest null), north-star not measured this session.** The
> full-panel **prod** edge run is hours long and was deferred (operator to launch). What this
> session establishes: the end-to-end pipeline is **wired correct** (validated in 5.5 min on a dev
> panel), and the **binding constraints** on a tradeable result are now named with evidence. Per the
> sprint contract, a documented frontier naming the binding constraint is a valid S7 outcome.

---

## Environment

| Artifact | Dimensions | Notes |
|---|---|---|
| Full accept panel `work/accept/panel.bin` | (pre-built, 1.5 GB) | curated universe; **no meta sidecar** (predates the feature) |
| Full segs `work/accept/segs` | 2629 daily `.seg`, 3.4 GB | 2016→2026 |
| Unfiltered augment (rejected) | 23 348 inst × 2627 dates × 23 f, 10.8 GB | default universe = untradeable micro-cap junk |
| Filtered augment (canonical screen) | 6 431 inst × 2627 dates × 23 f | `--min-adv-usd 25M --min-price 1.0 --require-sector` |
| **Dev panel** `work/dev/dev-panel.bin` | **600 inst × 501 dates × 23 f** | top-300 ADV, 2022–23, augmented; the fast-iteration fixture |

Augment fields (S5): `returns, cap, IndClass.sector/industry/subindustry, dollar_volume, vwap,
adv{5,10,20,60}` → 23 total. (`IndClass.industry/subindustry` alias GICS sector — I5-HOOK, p7-A2.)

## Build profile (the explicit non-default opt-in; never a golden re-baseline)

`discover --gated --turnover-penalty-slope 0.1 --max-turnover-target 0.25 --protect-seed-elites
--mutate-seed-copies --deflate-selection --min-viable-raw 0.05 --enable-wrap-in-op --cost-bps-admit
10 --min-holding-days 5 --min-dsr 0.5 --min-sharpe 0.25 --min-fitness 1.0 --max-turnover 0.50
--reject-price-scale 0.5 --dsr-subwindows 3 --typed-fields --robust-holdout-frac 0.30 --oos-fraction
0.25 --population 300 --generations 15` → combine (holdout 0.25) → optimize (`--position-mode
--cost-bps 10`) → report.

Two corrections vs the plan, found by running it: (1) `--admit-seeds-presearch` does not exist
(use `--protect-seed-elites`/`--mutate-seed-copies`); (2) `discover` does not emit a panel, so the
downstream stages consume the augmented panel directly (commit `00553d4` also fixed discover needing
`--alpha-out`, not `--out`).

---

## The frontier (binding constraints, with evidence)

**BC-1 — Universe alignment is a precondition, not a knob.** The plan's step-1 augment passed no
universe flags → an **unfiltered 23 348-name** panel: penny stocks and illiquids that make any
admitted alpha uninvestable and the capacity/turnover metrics meaningless. The canonical accept
screen (`--min-adv-usd 25M --min-price 1.0 --require-sector`, precedent
`scripts/canonical-acceptance-run.ps1:88`) cuts it to 6 431 names. *The search universe and the
tradeable universe must be the same set.* → p7-A2/P7-C make the screen a search precondition; the
harness should expose universe params (follow-up).

**BC-2 — Search throughput is the operational ceiling.** The full-panel build profile (pop 300 ×
gen 15 over 6 431 × 2627 = 16.9 M cells, CPCV folds per genome) runs for **hours** even on
cores−1 workers — long enough that it was killed mid-run twice during this session. This is the
single biggest drag on the *iteration* loop, and it is exactly the p7 **Track B** thesis: 29 of 34
time-series ops are O(T·W) batch with no SIMD and no cross-instrument parallelism. Until eval-ms/
genome drops, every honest edge measurement is an overnight job.

**BC-3 — Breadth vs deflation tension at reduced scale.** On the dev panel with the **strict** build
profile + small search (pop 40 × gen 4), **zero** alphas cleared the gate — the strict floors
(`--min-fitness 1.0 --min-dsr 0.5 --dsr-subwindows 3`) correctly reject everything a tiny price-only
search finds. This is the p6 thesis restated: edge is a *breadth* problem (price monoculture), and
honest deflation will keep rejecting until the search has real signal families to draw on. → p7-A1
(wire DSR/PBO + cumulative-sweep trial-count) and p7-A2 (FINRA / IV-surface / liquidity families).

---

## What IS established this session

**Pipeline wiring — validated end-to-end (smoke, 331 s):** dev panel + loose gates + pop 40/gen 4 →
**49 admitted alphas**, combine→optimize→report all ran, a deployed book written (`books.bin`,
288 names held). Deployed-book summary:

| metric | value | reading |
|---|---|---|
| `portfolio_is_sharpe` | **+1.31** | **sign-correct** — the S6 deploy fix holds (book matches the positive blend) |
| `portfolio_oos_sharpe` | −3.39 | overfit junk **by design** (loose gates + tiny search); NOT an edge claim |
| `avg_names_held` | 288 | non-empty, sane footprint |

The smoke numbers are intentionally not edge-meaningful; the smoke profile answers *"does the wiring
work"*, not *"is there edge"*. It does, including the sign-correct deploy that p6-S6/S7-4 delivered.

**Speedup / test-efficiency (the session's concrete deliverable):** a `-Profile smoke|prod` split
(commit `bd922e4`) + a cached dev panel turns a full pipeline shake-out from **hours → 5.5 min** with
no change to the prod argv path. This is the harness needed to iterate p7 without overnight runs.

---

## Determinism note (S7-7)

- **Default-path byte-identity:** `AtxImplDiscover.*` slice — **30 passed, 1 skipped**
  (`W6_RediscoverLowVolCapacityAlpha` needs the `ATX_ALPHA101_PANEL` real-panel env). All new S7 CLI
  flags default to inert values, so the no-flag discover digest is byte-identical to pre-S7.
- **Factory goldens:** S7 edited only `atx-impl/{config,stage_*}` + `scripts/`; `atx-engine-factory-
  tests` links `atx-engine` only (no `atx-impl` dependency), so the factory golden+digest slice is
  **invariant by construction**. `oracle.hpp` untouched.
- **seq==parallel (build profile):** discover on the dev panel, identical config, `--workers 1`
  vs `--workers 4` → **identical** `stage=discover digest=01e4a4fb9f6fe27a`
  (`factory_digest=baccec82e16b5405`, admitted=10 both). The search digest is worker-count-invariant
  (F1 holds) — workers affect speed, never bits.
- The build profile is the explicit opt-in; it is **never** a golden re-baseline.

## Next (to actually measure the north-star)

1. Operator launches the **prod** run overnight: `build-tradeable-alphas.ps1 -Profile prod
   -Stage augment,discover` then `-Stage pipeline` (staged), on the canonical-screened panel.
2. If net-of-10bps OOS Sharpe > 0.8 with a sign-correct book → north-star met; record the scorecard.
3. If not → the reject-histogram dominant bucket names the next gate to address; feeds p7-A1/A2.
