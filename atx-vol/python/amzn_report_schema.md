# AMZN-earnings report — CSV schema (contract between C++ emitter and Python plotter)

The C++ `amzn_earnings_report` tool writes these files to `--out <dir>`. The Python
`amzn_earnings_report.py` reads them and renders the VolaDynamics figure set. All
angles/curvature use the **VolaDynamics convention** `f(z)=1+s2·z+½·c2·z²`
(so reported `c2` = f''(0), `s2` = f'(0), `z = log(K/F)/(σ0√T)`).

## meta.json  (one object)
```
{ "underlying":"AMZN", "snapshot_iso":"2018-04-26T19:45:00Z", "spot":1519.41,
  "r":0.019, "q":0.0, "n_expiries":17, "fit_ms":396.9,
  "event_instants":["2018-04-26T20:00:00Z","2018-07-26T20:00:00Z"],
  "curve":"CStar-C8" }
```

## slices.csv  (one row per expiry, ascending T)
`expiry_date,expiry_iso,T,dte,F,sigma0,atm_vol,theta,s2,c2_base,c2_eff,C_left,C_right,beta0,beta1,beta2,beta3,beta4,beta5,beta6,beta7,beta8,beta9,beta10,tier,rmse_px,vol_rmse,min_roper_g,n_quotes,reverted`
- `sigma0` = ATF vol = sqrt(theta/T); `c2_eff` = f''(0) (VolaDynamics c2, incl. modes);
  `c2_base` = the atx base field (= ½·c2_eff for a base-only curve).
- `tier` ∈ {C5,C8,C12,C16}; `reverted` ∈ {0,1}; `min_roper_g` ≥ 0 ⇔ butterfly-arb-free.

## total_variance.csv  (dense FITTED grid; long format)
`expiry_date,T,dte,k,z,w,fit_iv`
- One block per expiry over a dense k=log(K/F) grid (≈121 pts, k∈[−1.5,1.5] clipped to
  the slice's quoted range±). `w = T·fit_iv²` (total variance); `z = k/(σ0√T)`.
- Drives: fig 1 (NS surface, x=z), fig 4 (total variance, x=k → "lines don't cross"),
  and the fitted curve overlaid in figs 2/3/8.

## smiles.csv  (MARKET quote points; long format)
`expiry_date,T,dte,K,k,z,mkt_iv,iv_err,bid_iv,ask_iv,leg,vega,in_fit`
- One row per surviving market strike. `mkt_iv` = European-equivalent implied vol of the
  mid; `iv_err` = half (ask_iv−bid_iv) (the error bar); `leg` ∈ {C,P} (OTM leg used);
  `in_fit` ∈ {0,1} (survived the fit filter). Drives market dots + error bars in figs
  2/3/5/8.

## earnings.csv  (OPTIONAL — emitted only if earnings decomposition wired)
Header row of scalars then per-tenor rows, OR two files:
- `earnings_summary.csv`: `iEMove,st,lt,decay,fit_error`  (iEMove = implied per-event move
  as a fraction of spot; st/lt/decay = censored-ATM term-curve params σ_C(T)=lt+(st−lt)e^{−decay·T}).
- `earnings_tenors.csv`: `nd,T,atm_dirty,atm_cen,n_earn,event_var_share`
  (SR tenor grid; atm_cen = earnings-censored ATM vol; event_var_share = n·eMove²/w_atm).

## Figures the Python must render (matplotlib, self-contained, saves PNGs + index.html)
1. **NS-space surface** — fit_iv vs z, first ~10 expiries overlaid (W-shape near-term).
2. **Front-expiry strike space** — market dots + fit_iv vs K; annotate the W + c2_eff.
3. **Front-expiry NS space** — market dots + fit_iv vs z; annotate negative c2_eff.
4. **Total variance vs k** — w vs k, all expiries, "lines don't intersect (ordered by T)".
5. **Total variance with error bars** — market w±(2·iv_err·σ·T proxy) vs fitted, per near expiry.
6. **3-param term structure** — sigma0(T), s2(T), c2_eff(T); mark c2_eff front ≈ −1.1, flat after 3–4 mo.
7. **8-param term structure** — all 8 C8 params vs T (base 5 + 3 active modes).
8. **Multi-expiry panels** — market vs fit per expiry (grid, i=0,1,3,4,5,6,9-style).
9. (if earnings.csv) **Earnings decomposition** — dirty vs censored ATM term curve + iEMove + event_var_share(T).

Theme: clean, publication-style; light background; consistent expiry colormap (viridis by T);
title band "AMZN around earnings — 2018-04-26 15:45 ET (real OPRA cbbo-1m)".
