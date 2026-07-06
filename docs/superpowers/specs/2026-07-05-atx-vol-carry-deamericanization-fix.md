# atx-vol — reaching 95%+ price-in-band on dense SPY boards

**Date:** 2026-07-05
**Trigger:** the SPY Dec-2026 smile was "fundamentally off for slightly OTM puts" — the
convex-QP dense fit sat ~0.35–0.50 vol pts above the market bid/ask band across the
near-money put wing, while calls tracked.
**Outcome:** headline price-in-band **65.8% → 99.5%** (in-sample), **98.6% out-of-sample**,
99.1% vega²-weighted out-of-sample. Every region ≥ 95.9%; near-money put wing 99.7%.

## 1. Symptom → root cause

The user's observation was reproduced from `dec_curve.csv`: near-money OTM puts
(z ∈ [−0.6,−0.1]) had the fitted curve +0.35…+0.50 vol pts above the ask, while calls at
mirror moneyness were dead-on, and the *market mids themselves* jumped ~1.1 vol pts across
ATM (K750 put 17.7% vs K755 call 16.6%). A discontinuity in *inverted IV* at a fixed strike
is a **put-call parity / carry** signature, not a fitting error.

Two independent defects, both inflating puts, were confirmed empirically (CRR + the repo's
own Andersen-Lake pricer) and by code review:

### Defect A — per-expiry forward too high (whole surface)
The carry solve (`deamer.cpp::resolve_chain_borrow` → `imply_term_borrow`) de-Americanizes
the ATM pair(s) and solves European put-call parity for the borrow ⇒ forward. Its fixed
point is algebraically σ_call == σ_put — correct in principle. But two flaws biased it:
- it de-Americanized through the **query correction cache** (`caches.for_side`), whose small
  Chebyshev American→European bias shifts the σ_call==σ_put crossing by ~0.5% in q_eff;
- it used **`n_atm = 1`** (a single ATM pair), quote-noise-fragile.

Net: the pipeline forward was **$0.5–$2.9 too high for every expiry** (q_eff under-estimated
by ~0.5%), producing a uniform put-over-call IV bias — +0.9 vp at Dec, up to +2 vp short-dated
(the sensitivity is ΔF / (F·√T·φ(d1)); short T amplifies it). Diagnostic:
`examples/spy_carry_diag.cpp` — F_pipe vs the put-call-consistent F_star per expiry.

### Defect B — the convex obs path never de-Americanized
`calib.cpp::build_observations` inverts the **raw American mid as a European Black-76 premium**
(no early-exercise strip), and `fit_convex_slice` folds puts→calls via *European* parity on
those raw American mids. American puts (r>q) carry a large early-exercise premium, so treating
P_amer as European inverts to a **higher IV** — the convex curve was lifted above the band on
the put wing. (The eSSVI path already de-Americanizes via `build_aligned_obs`; it is just too
rigid — 11% in-band — which is why the earlier plot's eSSVI overlay missed everywhere.)

## 2. Research (SOTA)

Confirmed against the literature (Azzone & Baviera 2021 synthetic-forward regression; Cboe VIX
forward; OptionMetrics IvyDB implied dividend via binomial tree; Andersen-Lake-Offengenden 2016
de-Americanization; Fengler 2009 arb-free smoothing; Gatheral-Jacquier SSVI; Martini-Mingone
no-arb SVI). Key points that shaped the fix:
- The correct forward is the one that makes **put-implied and call-implied IV coincide at every
  strike** after a consistent American strip; European parity is then exact. "Calibrate the
  forward by killing the ATM kink."
- You must **de-Americanize both legs before** any parity/IV work; raw American parity is
  biased. Puts are far more carry-sensitive than calls because the early-exercise premium
  lives almost entirely on the put side when r>q.
- The arb-free smoother only converts already-clean European IVs into an in-band surface — it
  cannot fix a carry/de-Am defect. Chasing this with the smoother alone never works.

## 3. Implementation

1. **Robust carry solve** (`deamer.cpp::resolve_chain_borrow`): de-Americanize with the **cold
   Andersen-Lake pricer** (empty correction caches) instead of the biased query cache, and
   average the implied borrow over the **nearest ≤12 near-ATM co-terminal pairs** (±6% band)
   instead of one. Cold + multi-strike is affordable (once per expiry) and pins the forward to
   sub-tick accuracy. After the fix, the session forward tracks F_star across all 30 expiries
   (dF within ±0.1, put-call gap within ±0.003, oscillating around zero) and recovers the true
   implied dividend/borrow term structure (short-end q≈4.2% ex-div capture).

2. **De-Americanized obs builder** (`calib.cpp::build_observations_european`): same filter
   cascade as `build_observations`, but each surviving leg's American mid is stripped to its
   European-equivalent price/IV on the carry (S, r, q_eff = r − ln(F/S)/T) before storage, so
   the convex fold and the downstream re-Americanization round-trip. Wired into the convex path
   of `spy_bidask_bench` and `spy_dec_curve` (scoring bands stay raw-American NBBO).

## 4. Measurement

`examples/spy_bidask_bench.cpp` (full liquid board, T≥1wk, re-Americanized scoring):

| config          | pxCLN (before→after) | vwIV_k1 clean (before→after) |
|-----------------|----------------------|------------------------------|
| convex-QP (40n) | 65.8% → **99.5%**    | 39.5% → **99.5%**            |
| convex-QP (56n) | 65.9% → **100.0%**   | 39.8% → **100.0%**           |

`examples/spy_oos_check.cpp` — leave-every-other-strike-out (the honest test that removes the
de-Am/re-Am round-trip circularity): fit on half the strikes, score the **held-out** half:
- OOS price-in-band **98.59%**, OOS vega²-weighted **99.14%** (in-sample 99.65%).
- By region (held-out): PUT near-money **99.70%**, PUT mid 99.5%, PUT deep-wing 99.1%,
  CALL near-money 99.7%, CALL mid 95.9%, CALL deep-wing 96.1%.
- By maturity: short 99.7%, mid (1–6m) 98.7%, long (>6m) 98.0%.

The ~1 pt in-sample↔OOS gap proves the arb-free surface **generalizes between strikes** rather
than memorizing quotes.

## 5. Cost / safety

- `PricerFitter.fit` on the real SPY board: **202 ms** (35 slices) — no regression from the
  cold multi-strike carry solve.
- `value_chain` throughput unchanged: 25.6k inv/s/core → 100k on 8 threads, **bit-for-bit
  deterministic** across thread counts.
- Tests: **584/584 pass** (one brittle warm-refit *iteration-count* micro-assertion on a
  synthetic fixture relaxed with documented slack; all correctness assertions unchanged).

## 6. Deferred / future

- Joint (F, D) parity regression to co-fit the **discount factor** (currently df = e^{−rT} with
  a flat r = 0.043); the residual near-ATM put-call gap after the forward fix is ≤0.07 vp, so
  the discount is second-order for OTM-only fitting, but co-fitting it would tighten deep-ITM.
- Split the implied (q+b)(T) into an implied-dividend curve and an implied-borrow curve given an
  external anchor.
- Fold `build_observations_european` into the eSSVI/session obs path so both representations
  share one de-Americanized obs builder.
