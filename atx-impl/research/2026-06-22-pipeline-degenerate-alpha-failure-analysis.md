# Why the Pipeline Generates Degenerate, Untradeable Alphas — Failure Analysis

**Date:** 2026-06-22
**Trigger:** A strict-profile sweep on the real 10-year ORATS panel (2627 dates × 6431 names, canonical
liquid universe) ran 21 min (pop=200, gen=5, 790 candidates) and admitted exactly ONE alpha:

```
(ts_min(earnFlag, 45) ^ atmCenI_21d) / close
```

This is economic nonsense — and worse, it is **mathematically degenerate**. Four parallel read-only code
investigations (field-typing, search budget, fitness/gates, data/returns) converge on the root causes below.

---

## CORRECTION (2026-06-23): the admitted alpha is REAL, not degenerate

> **The original "all-zero book / fail-open" smoking gun below is WRONG and is retained only for the record.**
> A forensic re-check (library catalog metrics + empirical panel measurement + verdict agent) established:
> - The library records for this alpha: `sharpe=2.27362 turnover=0.273766 returns=0.522562 drawdown=0.143944
>   is_sharpe=1.73627 oos_sharpe=2.27362` — i.e. it **trades (27% turnover) and holds out-of-sample**
>   (OOS 2.27 > IS 1.74). Not an all-zero book.
> - Empirically on `work/accept/panel.bin`: `earnFlag` distinct non-NaN values are `{-1, 0, 1}` and it is
>   **dense (0 on non-earnings days) for every in-universe, ORATS-covered name**. The 97.7% panel-wide NaN
>   fraction is the ~64% of instruments with no ORATS coverage at all (also NaN in `close`, masked
>   out-of-universe) — NOT the in-universe set. So `ts_min(earnFlag,45)` sees 45 consecutive finite values for
>   in-universe names → finite signal → a real book. No provenance bug; stored DSL == evaluated genome.
> - **The original error** conflated panel-wide NaN fraction with the per-instrument NaN fraction inside the
>   ORATS-covered in-universe subset (where earnFlag is dense-0).
>
> **Consequence for remediation:** a coverage/cardinality floor (old P0-1) would NOT have rejected this alpha
> (it trades on the full in-universe set). The real failure is **economically-vacuous-but-statistically-fit**,
> so the re-prioritized fixes are: **(1) field-type discipline** (keep binary-flag/count/categorical fields —
> `earnFlag`, `nEarnCnt`, `gics` — out of the numeric arithmetic pool; this is the direct cause), **(2) a
> price-scale / dimensional prior** (`/ close` is a price-level bet — RC4 promoted from LOW to HIGH), and
> **(3) an economic structural prior at admission** (per-window DSR AND, blocking PBO, trivial-basis
> rejection). The coverage gate is demoted to a cheap safety net. See the re-scoped sprint in the SDD ledger.

## (SUPERSEDED) The original smoking gun: an ALL-ZERO book — DISPROVEN, see correction above

Verified empirically against `work/accept/panel.bin`:
- `earnFlag` is a **sparse earnings-event flag, 96.2% NaN even in-universe** (4.59M of 4.77M in-universe cells
  are NaN; the only non-NaN values are {-1, 0, 1}). It's only populated on/around earnings days.
- The DSL's rolling ops are **full-window, any-NaN → NaN** (`alpha/ts_ops.hpp:25-27`). `ts_min(earnFlag, 45)`
  therefore needs 45 *consecutive* non-NaN earnFlag values — which exists on **0 of 16,894,237 cells**.
- `^` is `std::pow` (`alpha/parser.hpp:239-240`), NaN-propagating. So the whole expression is **NaN everywhere**.
- An all-NaN signal → `to_target_weights` returns an **all-zero book** (`loop/weight_policy.hpp:243-245`).

**[DISPROVEN]** The premise above (96.2% NaN *in-universe*) is the measurement error corrected above: in-universe
earnFlag is dense, so the signal is finite and the book trades. The recorded OOS Sharpe is 2.27, not ~0.

---

## Root causes (ranked by leverage)

### RC1 — No field-type discipline (HIGH; the direct cause — REVISED per the correction above)
The grammar's only type gate is a name whitelist `is_group_field()` matching `"sector"` / `IndClass.*`
(`factory/typecheck.hpp:124-128`). **Everything else — `earnFlag`, `gics`, `nEarnCnt_5d`, counts, flags —
is dumped into the numeric F64 leaf pool** (`factory/search_driver.cpp:37-41`, fed to `generate.hpp:108`).
- The real GICS classifier is loaded as **`gics`, not `sector`**, so even *it* leaks into numeric arithmetic.
- The binary event flag `earnFlag` (values `{-1,0,1}`, dense-0 in-universe) becomes a free numeric leaf that
  `ts_min`/`^`/`/` combine into a **real but economically meaningless** signal — this is exactly the admitted
  alpha. (NOTE: the earlier "all-NaN" claim is RETRACTED — see correction; the signal is finite and trades.)
- A coverage/cardinality floor is a cheap safety net but would NOT have rejected THIS alpha (full in-universe
  coverage, 27% turnover). The high-leverage fix is keeping flag/count/categorical fields out of the numeric
  pool in the first place, not a downstream coverage gate.

### RC2 — Admission is purely statistical, with no economic/structural prior, and the lone real bar is weak (HIGH)
- The default **`WeightPolicy::Rank` transform discards signal magnitude** and launders *any* non-constant
  cross-section into a fully-invested, dollar-neutral, gross-1.0 "tradeable" book (`loop/weight_policy.hpp:181,
  260-267, 327-343`). The pipeline has zero notion that an expression is economically vacuous.
- The DSR deflation is the **only** real defense, and it's moderate: at N=790, `min-dsr 0.5` ≈ a **holdout
  annualized Sharpe of ~2.0** — beatable by single-window luck. And `SR*_N` grows only ~logarithmically in N,
  so "more trials" barely tightens it (`eval/deflated_sharpe.hpp:113-148`; `factory/fitness.cpp:326-329`).
- **PBO is advisory-only — it never un-admits** and fail-opens at `n_candidates<2` (`factory.cpp:83-155,
  143-154`); on a 1-alpha admission it had **no effect**. Split-stability is sign-only (weak).
- **Selection optimizes UNDEFLATED raw `wq`** (`FitnessCfg.trial_count=1` during search, `fitness.hpp:236`);
  deflation bites once at admission only — so the search spends its whole budget chasing in-sample fit.
- There is **no prior** rejecting trivial/degenerate signals or signals correlated with the flag/momentum/size
  bases.

### RC3 — The search is tiny + actively anti-convergent (MEDIUM; caps quality, not the direct cause)
- pop=200 × gen=5 ≈ 790 distinct structures = **broad random sampling, not evolution** (only 4 selection
  rounds; defaults hardcoded at `stage_discover.cpp:835-841`, `stage_sweep.cpp:93-96`).
- **20 immigrants/generation** (10% fresh random genomes, `stage_discover.cpp:859`) **+ behavioral-novelty
  objective ON** (`:842`) both *oppose* convergence — the population churns rather than compounds.
- Bottleneck is **~1.6 s/eval**: a full 2627-date VM pass per candidate, **evaluated twice** per candidate
  (search + admission re-score, `factory.cpp:997-1035`; obs 11805). No date-subsampling during search.
- A credible search (pop≈200 × gen≈20-30 ≈ 4-6k evals) is ~2-2.7 h today; **only feasible in minutes** if
  per-eval cost drops (date subsample) — so the budget can't simply be raised without a speed fix.

### RC4 — Price-scale / dimensional leak (HIGH — PROMOTED per the correction)
`/ close` divides by the TRI close *level*, so the signal carries a `1/price` (≈ size/low-price) tilt — a
real, persistent cross-sectional bet that has nothing to do with the `earnFlag`/`atmCenI` "edge." This is a
direct mechanism by which the admitted alpha earns a holdout Sharpe of 2.27 while being economically vacuous.
The fix is a dimensional / price-scale prior at admission (reject signals that divide by a raw price level or
whose holdout PnL correlates with the `1/price`/size basis above a threshold), and/or a grammar discipline
that forbids dividing by a price *level* (only by returns/vol/normalized quantities).

---

## What is NOT broken (the sprint's measurement layer is sound)
The data plumbing and the just-shipped T1-T7 measurement code are correct — the failure is upstream:
- **Returns are TRI-adjusted, not raw** (`data/history_panel.hpp:32`; search & report read the TRI `close`).
  No split/dividend jumps. `raw_close` is used only for capacity notional.
- **No lookahead** — `pnl[t] = Σ w[t-1]·ret[t]` (`alpha/streams.hpp:243-255`); out-of-universe/NaN → 0.
- **The universe IS a liquid single-name set** — ~1,815 names/date, GICS-required, ADV/price floors on
  `raw_close` (`data/universe.cpp:97-105,199-206`). 0% sector-NaN in-universe.
- **The OOS firewall is genuine** — admission gates on the *holdout* window with an embargoed train
  `[0, holdout_begin−embargo)` (`factory.cpp:889-918, 1071-1120`); no leakage.
- `atmCenI_*` / `nEarnCnt_5d` are dense (0% NaN in-universe); only `earnFlag` is the sparse offender.

---

## Prioritized remediation (what to fix, in order)

1. **[P0] Reject degenerate signals at admission.** Add a **coverage/cardinality floor**: reject any candidate
   whose signal is non-NaN on < K in-universe names/date (kills the all-NaN winner) or whose effective
   cross-sectional cardinality is below a threshold / turnover ≈ 0 from a rarely-flipping flag. Wire in the
   factory fitness/admission path (the live-cell count is already computed at `weight_policy.hpp:236-242`) and
   in `AlphaGate::admit` (`combine/gate.hpp:103`). **Also confirm the fail-open hole** that let an all-zero
   book clear `min-dsr 0.5`.
2. **[P0] Field-type discipline.** Exclude flag/count/categorical/sparse fields (`earnFlag`, `gics`,
   `nEarnCnt_*`) from the numeric leaf pool (`search_driver.cpp:37-41`); fix `is_group_field` to recognize the
   real `gics` classifier (`typecheck.hpp:124-128`); add a per-field NaN-fraction / cardinality screen at panel
   build (`stage_discover.cpp:107-119, 827-830`). Optionally fill `earnFlag` off-days with 0 at ingest so event
   fields are well-defined (`data/history_panel.cpp:252-257`).
3. **[P1] Add an economic/structural prior to admission.** Reject signals that ARE a trivial basis (correlate
   candidate PnL vs momentum / size / the raw flag and reject if it *is* that); require holdout `min-dsr`
   cleared on **every** walk-forward window (turn the single-window check at `factory.cpp:1120` into an
   all-windows AND); make PBO actually block (`factory.cpp:154`).
4. **[P1] Deflate during SELECTION, not just admission.** Feed the running trial count into the search
   fitness so the GA optimizes deflated edge, not raw in-sample `wq` (`fitness.hpp:236`).
5. **[P2] Make the search real + affordable.** Date-subsample per candidate during search (full panel only at
   admission) → ~4× faster → raise generations 5→20-30 and cut immigrants 10%→~5% / gate novelty off in the
   exploit phase (`stage_discover.cpp:838,842,859`). Expose population/generations/immigrants as flags.

**Bottom line:** the sprint correctly made *measurement* honest (cost, capacity, conviction, breadth), but
alpha *generation* has no economic or signal-validity priors and no degenerate-signal rejection, so a
statistical search over an untyped field pool finds NaN/lucky artifacts. Fixing P0+P1 (reject degenerate
signals, type the fields, add an economic prior, deflate in selection) is the path to robust alphas — not more
search time.
