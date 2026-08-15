# SpiderRock oracle RSI loop — design

## 2026-08-15 harness hard cutover

The workflow now has deterministic capability states, evaluated in order:
`missing_data -> missing_mode_a -> missing_conventions -> missing_mode_b -> ready`.
Capability freezes `refs/heads/oracle/canonical` (or the requested base before its
first creation) to a full SHA and reads only an already committed holdout digest
receipt. It never opens membership. A missing state runs exactly one fixed
implementation lane through Build, exact-SHA Review, optional Fix, fresh Review,
scoped verification in a newly leased worktree, and atomic canonical landing. It
returns BOOTSTRAP only after that exact reviewed SHA is canonical and the next state
is observed. It skips planner, ordinary Measure, Analyst, vol-sprint, Ratchet, and
every holdout benchmark. The four bootstrap deliveries are data/cohorts/hash, Mode
A, convention floor, then Mode B. There is no combined "missing tooling" path.

Only `ready` runs Measure (aggregate smoke+tune), the tool-less Analyst (a validated,
self-contained aggregate payload), vol-sprint, then Ratchet. A blocked/incomplete
sprint or any mandatory lane whose final review is not a fresh APPROVE returns
`FAILED` before holdout; it is not a REJECT and cannot increment the consecutive-
reject counter. Ratchet leases the exact reviewed integration SHA, then alone opens
holdout and recomputes the digest. ACCEPT atomically advances canonical to the final
Ratchet commit; REJECT leaves canonical unchanged. The returned structured result
includes pasted holdout evidence and evidence-indexed headline deltas. Licensed row
data and holdout membership are forbidden in prompts and reports.

Bootstrap artifacts and exact done conditions are authoritative in
`atx-vol/bench/oracle/CHARTER.md`. This section supersedes the older sequencing
prose below where they differ.

Date: 2026-08-15. Status: harness implemented; oracle capability bootstrap pending.
Depends on: vol DAG harness (f4d8bb9).

Goal: a recursive self-improvement loop that drives atx-vol's American-options fitting and
pricing pipeline toward reproducing SpiderRock's `srPrc` / `srVol` / greeks at
state-of-the-art speed, using `tbloptionintradayhist_30min_eqt_id_v2.00_2026-08-14.zip`
(Downloads, 2.45 GB → 15 GB TSV, ~33M rows) as the north-star oracle. Backwards
compatibility is explicitly NOT a constraint: structural changes land as hard cutovers,
recorded in CHANGELOG, never as opt-in configuration.

## 1. Oracle data foundation (one-time per drop)

**Source schema** (verified against the file): option key `okey_*` (incl. strike `okey_xx`,
`okey_cp`), slice `date` (UTC; 13:30 UTC = the ignored 9:30 ET slice), quote-time
`timestamp`, underlier `undSecKey_tk` + `uBid/uAsk/uPrc`, market `bidPrc/askPrc/bidSz/askSz/
bidIV/askIV`, **oracle outputs** `srPrc srVol de ga th ve rh ph vo va deDecay atmVol`,
**oracle inputs** `rate sdiv ddiv years`, quality `error`, `-99` = missing sentinel.

**Ingest** — `atx-vol/scripts/oracle_ingest.py` (stdlib + polars, script tier, not CMake):
1. Precondition: free-disk check (needs ~15 GB transient + ~2-3 GB parquet; extract target
   configurable, default `C:\atx-cache\oracle\`).
2. Extract TSV once → `pl.scan_csv(separator='\t')` → **lazy** transforms →
   `sink_parquet` (streaming; no full materialization ever):
   - drop the 9:30 ET slice (`date` == 13:30 UTC);
   - `bucket = timestamp.dt.truncate('5m')` — groups drifting quote times into slices;
   - `-99` → null everywhere; typed casts; derived `moneyness`, `dte`;
   - partition `date=YYYY-MM-DD/underlier=<tk>/` under
     `C:\atx-cache\oracle\spiderrock\` (out of git; future daily drops land beside it).
3. Delete the extracted TSV after a row-count + checksum manifest is written.

**Cohort manifests** — committed JSON under `atx-vol/bench/oracle/cohorts/`:
- `smoke` — 1 liquid underlier × 1 bucket (~10-30k rows; seconds — the inner loop);
- `tune` — ~10 underliers × 3 buckets, stratified by liquidity/moneyness/DTE;
- `holdout` — disjoint underliers AND disjoint buckets from `tune`; **never read by the
  attribution stage**; the accept gate runs on it (anti-overfit);
- `fullday` — everything; periodic sweep only.

## 2. Benchmark runner (the oracle gate's instrument)

`atx-vol-oracle-bench` — C++ tool (Arrow/Parquet already in vcpkg) + a thin polars report
driver. Two modes, run per cohort:

- **Mode A — model parity.** Feed SpiderRock's own inputs per row (`uPrc, rate, sdiv,
  ddiv, srVol`, their `years`) into the atx-vol American engine. Compare price → `srPrc`,
  greeks → `de ga th ve rh ph vo va deDecay`. Residual = engine + convention error ONLY.
- **Mode B — pipeline parity.** atx-vol fits its own surface from raw NBBO
  (`bidPrc/askPrc`, sizes) per underlier × expiry × bucket → own fair vol vs `srVol`,
  own fair value vs `srPrc`, own greeks. Residual = fit + engine error. (B minus A ≈ fit
  error — the loop's decomposition lever.)

**Scorecard** (JSON, small, committed to `atx-vol/bench/oracle/scorecards/iter-NNN.json`):
per mode × metric (price, vol, each greek): MAE/RMSE/P50/P95/P99/max + within-tolerance
rates (|Δprc| ≤ max(1 tick, 10% of spread); |Δvol| ≤ 10 bp; greeks ≤ 1% rel), bucketed by
moneyness × DTE × cp × liquidity tier; plus speed (rows/s priced, fits/s, wall) — perf
numbers from `rel-avx2` only, correctness numbers from `dev`.

**Oracle-suspect rule.** SpiderRock is the north star, not ground truth. A cell where
atx-vol disagrees with the oracle but the market sides with atx-vol (e.g. `srPrc` outside
NBBO while ours is inside; `error` column nonzero) is flagged oracle-suspect, excluded
from the ratchet, and listed in the scorecard — the loop must not learn their bugs.

## 3. Iteration 0 — convention resolution (mandatory first)

Before any improvement work: resolve comparison semantics, else every error is convention
noise. Round-trip their own numbers: price(their inputs, `srVol`) vs `srPrc` under
candidate conventions (theta per-day vs per-year; vega per vol-point vs per 1.0; `sdiv` as
continuous borrow; `ddiv` discrete; `years` daycount; `vo`/`va` = vanna/volga hypothesis;
sign conventions; per-share scaling). Output: a convention map committed at
`atx-vol/bench/oracle/CONVENTIONS.md` + the residual floor (how close a perfect clone
could get). Ledger line records the floor.

## 4. The loop — one iteration

```
Measure (smoke+tune, modes A+B)          → scorecard N
  → Attribute (tool-less aggregate payload) → ranked worst cells + 1-3 falsifiable
                                             hypotheses ("fix X ⇒ cell Y RMSE −Z%")
  → Improve (vol-sprint child workflow)    → plan → parallel lanes → review → fix
  → Ratchet gate (new exact-SHA lease)      → existing gates (atx_vol_fast, hygiene,
                                             ci/run_all_gates.ps1) PLUS oracle gate:
                                             scorecard on HOLDOUT must improve target
                                             cells, no aggregate regression > 2%, speed
                                             ≥ baseline pin
  → Commit + memory                        → accept: canonical CAS, scorecard pinned, ledger line
                                             with metric deltas, NORTHSTAR.md updated;
                                             reject: hypotheses marked REFUTED in ledger
                                             (negative results are memory), branch kept
```

**New pieces on the existing harness:**
- `.claude/agents/vol-analyst.md` — tool-less attribution stage: receives one validated,
  self-contained aggregate smoke/tune payload, ranks cells, and forms falsifiable
  hypotheses. It has no workspace reach and never receives paths, hashes, membership,
  or row-level data.
- `.claude/workflows/vol-oracle-iter.js` — the iteration DAG above; invokes `vol-sprint`
  as a child via `workflow()` (one nesting level — allowed); schema-typed edges
  (SCORECARD, ATTRIBUTION, RATCHET verdicts).
- `atx-vol/docs/oracle/NORTHSTAR.md` — mutable dashboard (current vs target metrics, open
  hypotheses, refuted list, oracle-suspect cells, convention map pointer). The loop's
  working brain; LEDGER.md stays the append-only history.

**Recursion & control.** Each iteration is one workflow run against the frozen
`refs/heads/oracle/canonical`. Trigger: manual ("next
iteration") first; `/loop` self-paced later once trust is earned. The analyst reads the
full scorecard + ledger history each time — the loop improves its own targeting from its
own record, including what failed. Stopping: targets met (Mode A: price MAE within a
tick, vol within 5 bp, greeks within 1% rel; Mode B: within 2× the Mode A floor), or 3
consecutive rejected iterations → escalate to user with the refuted-hypothesis list.

**Structural mandate.** Lanes may restructure APIs, engines, and surface
parameterization freely: no compat shims, no flags — hard cutover + CHANGELOG `BREAKING`.
Expected frontier directions (discovered by attribution, not pre-committed): discrete
dividend / early-exercise treatment, rate-borrow curve fidelity, surface
parameterization near expiry, batch-vectorized American pricing paths.

## 5. Sequencing (small → big, per user directive)

1. `missing_data`: ingest + all three cohorts + frozen holdout-membership hash.
2. `missing_mode_a`: Mode A tool + aggregate smoke capability receipt.
3. `missing_conventions`: smoke+tune convention resolution + residual floor at iter-000.
4. `missing_mode_b`: Mode B + aggregate smoke/tune capability receipt.
5. `ready`: iterations 1..k on smoke+tune, then holdout ratchet only after sprint PASS.
6. Widen: tune cohort growth, fullday sweeps, and additional daily drops.
7. Speed frontier: once accuracy plateaus, iterations may target speed alone (same
   ratchet, roles swapped: speed must improve, accuracy must not regress).

## Risks

- 15 GB transient disk (C: was recently near capacity — ingest checks first, target
  drive configurable).
- Single trading day → overfit risk even with holdout; mitigated by underlier+bucket
  disjointness now, more days later.
- Convention mismatch could masquerade as model error for the whole loop — hence
  iteration 0 is a hard prerequisite, gated on the round-trip residual floor.
- Token burn: each iteration ≈ one vol-sprint + 3 extra agents; manual trigger keeps the
  user in the loop budget-wise.
