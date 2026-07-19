#!/usr/bin/env python3
"""Generate a SYNTHETIC but schema-conformant "AMZN around earnings" CSV set.

This is a DEVELOPMENT HARNESS for ``amzn_earnings_report.py`` — it lets the
Python report be built and exercised without the C++ fitter. It is **not** real
data and must never be mistaken for the committed OPRA fixture.

It writes the exact files documented in ``amzn_report_schema.md``:

    meta.json  slices.csv  total_variance.csv  smiles.csv
    earnings_summary.csv  earnings_tenors.csv        (the two optional files)

The synthetic surface reproduces the qualitative structure of the real snapshot
(AMZN, 2018-04-26 15:45 ET, spot ~ $1519, 17 expiries, 1 DTE -> ~1.7 yr):

  * an earnings-elevated, declining ATM term structure (front ATM vol ~ 120 %,
    falling to ~ 30 % by 3 months), built from a censored diffusive curve plus
    discrete event variance ``n_earn * eMove^2`` -- so it is arb-consistent by
    construction;
  * a front-expiry **W-shape** (extreme negative ATF curvature, c2_eff ~ -1.1),
    flattening to a normal convex smile (c2_eff ~ +0.1) by 3-4 months;
  * total variance that is **strictly increasing in T at every k** (calendar-arb
    free -- verified before writing), with bounded SVI-like linear wings;
  * market quote points with plausible bid/ask error bars.

Model (per expiry, all shape/level scalars computed NUMERICALLY from it):

    w(k)      = w_base(k) + w_event(k)                       [total variance]
    w_base(k) = theta_diff + b*( rho*k + sqrt(k^2+sig^2) - sig )   [SVI-like]
    w_event(k)= Vev * exp( -k^2 / (2*kappa^2) )             [localized ATM hump]

    theta_diff = T * sigma_censored(T)^2      (diffusive ATM total variance)
    Vev        = n_earn(T) * eMove^2          (discrete event variance)
    w(0)       = theta_diff + Vev = T * sigma0^2   (dirty ATM total variance)

The event hump makes total variance locally *concave* near ATM on the front
expiry (negative c2_eff, the W middle) while the SVI base supplies the rising
outer arms; as T grows the hump's share of variance shrinks and the smile
relaxes to the ordinary convex base -> c2_eff crosses zero to ~ +0.1.

Usage:
    python make_synthetic_amzn_csvs.py --out <csvdir>

Pure numpy + stdlib. Fixed seed (deterministic). No network.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

# ── snapshot constants (mirror the real fixture header) ─────────────────────
SPOT = 1519.41
R = 0.019
Q = 0.0
SNAPSHOT_ISO = "2018-04-26T19:45:00Z"          # 15:45 ET
EVENT_INSTANTS = ["2018-04-26T20:00:00Z", "2018-07-26T20:00:00Z"]
CURVE = "CStar-C8"
SEED = 20180426

# earnings-decomposition parameters (censored ATM term curve + implied move)
EMOVE = 0.060                                   # implied per-event move / spot
SIG_ST = 0.360                                  # censored short-term vol
SIG_LT = 0.270                                  # censored long-term vol
SIG_DECAY = 3.5                                 # censored curve decay rate
EVENT_T = [0.0, 0.249]                          # event instants in year fraction

# per-expiry calendar days (weeklies -> monthlies -> LEAP), 17 expiries
DTES = [1, 2, 7, 14, 21, 28, 35, 49, 63, 91, 119, 154, 189, 245, 336, 490, 630]

# a mid expiry we deliberately flag reverted=1 (+ NaN diagnostics) so the
# report's de-emphasis / NaN-guard paths are exercised on synthetic data.
REVERTED_IDX = 8

DENSE_N = 121                                    # fitted-grid points per expiry
Z_FIT = 2.9                                      # fitted grid half-width (in z)
Z_QUOTE = 2.4                                    # market strikes half-width (in z)


def sigma_censored(T: float) -> float:
    """Diffusive (earnings-censored) ATM vol: lt + (st-lt) e^{-decay T}."""
    return SIG_LT + (SIG_ST - SIG_LT) * math.exp(-SIG_DECAY * T)


def n_earn(T: float) -> int:
    """Number of scheduled earnings events strictly before expiry T."""
    return sum(1 for et in EVENT_T if et < T)


# ── shape schedules (smooth in T); tuned so c2_eff(front) ~ -1.1 -----------
def svi_sig(T: float) -> float:
    """SVI curvature radius in k (ATM curvature ~ b/sig)."""
    return 0.055 + 0.42 * T


def svi_b(T: float) -> float:
    """SVI wing slope (variance per |k|), grows with T (widening smile)."""
    return 0.011 + 0.150 * T


def svi_rho(T: float) -> float:
    """SVI skew rho in (-1,0): downside puts richer; mild flattening in T."""
    return -0.35 - 0.10 * math.exp(-2.5 * T)


def kappa_frac(T: float) -> float:
    """Event-hump width in k as a fraction of ATM total vol sqrt(w0).

    Narrow (~0.6 z) on the front expiry -> a sharp ATM straddle premium and
    strong negative ATF curvature (the W middle); relaxes slightly with T.
    """
    return 0.60 + 1.00 * (1.0 - math.exp(-2.9 * T))


def total_variance(k, T, w0, theta_diff, Vev):
    """w(k) = SVI base (anchored at theta_diff) + localized event hump."""
    sig = svi_sig(T)
    b = svi_b(T)
    rho = svi_rho(T)
    kappa = kappa_frac(T) * math.sqrt(w0)
    base = theta_diff + b * (rho * k + np.sqrt(k * k + sig * sig) - sig)
    hump = Vev * np.exp(-(k * k) / (2.0 * kappa * kappa))
    return base + hump


def fit_iv(k, T, w0, theta_diff, Vev):
    return np.sqrt(np.maximum(total_variance(k, T, w0, theta_diff, Vev), 1e-12) / T)


def build_slice(dte: int):
    """Return a dict of all per-expiry scalars derived numerically from the model."""
    T = dte / 365.0
    F = SPOT * math.exp((R - Q) * T)
    ne = n_earn(T)
    sig_cen = sigma_censored(T)
    theta_diff = T * sig_cen * sig_cen
    Vev = ne * EMOVE * EMOVE
    w0 = theta_diff + Vev                       # dirty ATM total variance
    sigma0 = math.sqrt(w0 / T)                  # ATF / ATM vol

    # normalized smile f(z) = sigma(k)/sigma0, k = z*sqrt(w0); derive s2,c2_eff
    # numerically as f'(0), f''(0) (schema convention: f=1+s2 z+1/2 c2 z^2+...).
    h = 0.01
    sw0 = math.sqrt(w0)

    def f(z):
        k = z * sw0
        return math.sqrt(max(total_variance(np.array([k]), T, w0, theta_diff, Vev)[0], 1e-12) / T) / sigma0

    s2 = (f(h) - f(-h)) / (2.0 * h)
    c2_eff = (f(h) - 2.0 * f(0.0) + f(-h)) / (h * h)
    c2_base = 0.5 * c2_eff

    sig = svi_sig(T)
    b = svi_b(T)
    rho = svi_rho(T)
    kappa = kappa_frac(T) * math.sqrt(w0)
    c_left = b * (1.0 - rho)                     # left-wing variance slope
    c_right = b * (1.0 + rho)                    # right-wing variance slope

    # min Roper g (butterfly-arb margin): positive => arb-free. Synthetic proxy
    # that shrinks with the strength of the negative curvature but stays > 0.
    min_roper_g = max(0.004, 0.05 + 0.04 * min(c2_eff, 0.0))

    return {
        "T": T, "dte": dte, "F": F, "n_earn": ne,
        "sig_cen": sig_cen, "theta_diff": theta_diff, "Vev": Vev,
        "w0": w0, "sigma0": sigma0, "s2": s2,
        "c2_base": c2_base, "c2_eff": c2_eff,
        "C_left": c_left, "C_right": c_right,
        "svi_sig": sig, "svi_b": b, "svi_rho": rho, "kappa": kappa,
        "min_roper_g": min_roper_g,
    }


def dense_grid(T, w0, theta_diff, Vev):
    """Dense fitted grid over z in [-Z_FIT, Z_FIT], k clipped to [-1.5,1.5]."""
    z = np.linspace(-Z_FIT, Z_FIT, DENSE_N)
    k = np.clip(z * math.sqrt(w0), -1.5, 1.5)
    iv = fit_iv(k, T, w0, theta_diff, Vev)
    w = T * iv * iv
    return z, k, iv, w


def market_points(sl, rng):
    """Realistic market strikes for one expiry: dots + bid/ask error bars."""
    T, w0 = sl["T"], sl["w0"]
    theta_diff, Vev = sl["theta_diff"], sl["Vev"]
    sw0 = math.sqrt(w0)
    # ~ 17-27 strikes, denser near ATM; short expiries quote a narrower band.
    nstk = int(np.interp(T, [0.0, 0.5, 1.7], [17, 25, 29]))
    zq = Z_QUOTE * np.interp(T, [0.0, 0.05, 1.7], [0.72, 0.85, 1.0])
    zs = np.linspace(-zq, zq, nstk)
    rows = []
    for z in zs:
        k = float(np.clip(z * sw0, -1.5, 1.5))
        K = sl["F"] * math.exp(k)
        iv_fit = float(fit_iv(np.array([k]), T, w0, theta_diff, Vev)[0])
        # half bid/ask spread in vol: wider for short T and far wings.
        iv_err = 0.004 + 0.010 * math.exp(-6.0 * T) + 0.006 * abs(z) / Z_QUOTE
        iv_err *= 1.0 + 0.12 * abs(z)
        mkt = iv_fit + rng.normal(0.0, 0.28 * iv_err)   # mid noise < spread
        mkt = max(mkt, 0.02)
        leg = "P" if k < 0 else "C"
        vega = float(max(1e-6, math.exp(-0.5 * z * z)) * sl["F"] * math.sqrt(T) * 0.4)
        in_fit = 0 if abs(z) > 2.15 else 1
        rows.append({
            "K": K, "k": k, "z": float(z), "mkt_iv": mkt, "iv_err": iv_err,
            "bid_iv": mkt - iv_err, "ask_iv": mkt + iv_err,
            "leg": leg, "vega": vega, "in_fit": in_fit,
        })
    return rows


def iso_from_dte(dte: int) -> tuple[str, str]:
    """(expiry_date YYYYMMDD, expiry_iso) from the 2018-04-26 snapshot."""
    import datetime as _dt
    d = _dt.date(2018, 4, 26) + _dt.timedelta(days=dte)
    return d.strftime("%Y%m%d"), d.strftime("%Y-%m-%dT21:00:00Z")


def verify_calendar_arb(slices, grids) -> list[str]:
    """Check total variance is strictly increasing in T at each shared k.

    Returns a list of human-readable violation strings (empty == arb-free).
    """
    warns: list[str] = []
    kgrid = np.linspace(-0.6, 0.6, 61)          # near-money band (the proof zone)
    prev = None
    for sl, (_z, _k, _iv, _w) in zip(slices, grids):
        wk = total_variance(kgrid, sl["T"], sl["w0"], sl["theta_diff"], sl["Vev"])
        if prev is not None:
            diff = wk - prev
            if np.min(diff) <= 0.0:
                j = int(np.argmin(diff))
                warns.append(
                    f"calendar-arb: T={sl['T']:.4f} crosses previous at k={kgrid[j]:+.3f} "
                    f"(dw={diff[j]:+.2e})"
                )
        prev = wk
    return warns


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate synthetic AMZN-earnings CSVs.")
    ap.add_argument("--out", required=True, help="output directory for the CSV/JSON set")
    ap.add_argument("--no-earnings", action="store_true",
                    help="skip the optional earnings_*.csv files (to test fig-9 absence)")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(SEED)

    slices = [build_slice(d) for d in DTES]
    grids = [dense_grid(sl["T"], sl["w0"], sl["theta_diff"], sl["Vev"]) for sl in slices]

    warns = verify_calendar_arb(slices, grids)
    if warns:
        print("[synthetic] WARNING calendar-arb violations detected:")
        for w in warns:
            print("   " + w)
    else:
        print("[synthetic] calendar-arb check OK (total variance strictly increasing in T)")

    # ── meta.json ──────────────────────────────────────────────────────────
    meta = {
        "underlying": "AMZN", "snapshot_iso": SNAPSHOT_ISO, "spot": SPOT,
        "r": R, "q": Q, "n_expiries": len(DTES), "fit_ms": 396.9,
        "event_instants": EVENT_INSTANTS, "curve": CURVE,
        "synthetic": True,
    }
    (out / "meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")

    # ── slices.csv ─────────────────────────────────────────────────────────
    slice_cols = [
        "expiry_date", "expiry_iso", "T", "dte", "F", "sigma0", "atm_vol",
        "theta", "s2", "c2_base", "c2_eff", "C_left", "C_right",
        "beta0", "beta1", "beta2", "beta3", "beta4", "beta5", "beta6", "beta7",
        "beta8", "beta9", "beta10", "tier", "rmse_px", "vol_rmse",
        "min_roper_g", "n_quotes", "reverted",
    ]
    smile_rows: list[str] = []
    tv_rows: list[str] = []
    slice_lines = [",".join(slice_cols)]
    for i, (sl, (z, k, iv, w)) in enumerate(zip(slices, grids)):
        exp_date, exp_iso = iso_from_dte(sl["dte"])
        mkt = market_points(sl, rng)
        reverted = 1 if i == REVERTED_IDX else 0
        # 8-param C8 view (base 5 + 3 modes); beta8..10 unused at C8 tier.
        beta = [
            sl["sigma0"], sl["s2"], sl["c2_base"], sl["C_left"], sl["C_right"],
            sl["Vev"], sl["kappa"], sl["svi_sig"], 0.0, 0.0, 0.0,
        ]
        rmse_px = 0.09 + 0.6 * math.exp(-8.0 * sl["T"])
        vol_rmse = 0.0035 + 0.006 * math.exp(-8.0 * sl["T"])
        if reverted:                            # NaN diagnostics on the flagged slice
            rmse_px = float("nan")
            vol_rmse = float("nan")
        vals = [
            exp_date, exp_iso, f"{sl['T']:.6f}", sl["dte"], f"{sl['F']:.4f}",
            f"{sl['sigma0']:.6f}", f"{sl['sigma0']:.6f}", f"{sl['w0']:.8f}",
            f"{sl['s2']:.6f}", f"{sl['c2_base']:.6f}", f"{sl['c2_eff']:.6f}",
            f"{sl['C_left']:.6f}", f"{sl['C_right']:.6f}",
            *[f"{b:.6f}" for b in beta],
            "C8", f"{rmse_px:.6f}", f"{vol_rmse:.6f}",
            f"{sl['min_roper_g']:.6f}", len(mkt), reverted,
        ]
        slice_lines.append(",".join(str(v) for v in vals))

        for zz, kk, ii, ww in zip(z, k, iv, w):
            tv_rows.append(f"{exp_date},{sl['T']:.6f},{sl['dte']},{kk:.6f},"
                           f"{zz:.6f},{ww:.8f},{ii:.6f}")
        for m in mkt:
            smile_rows.append(
                f"{exp_date},{sl['T']:.6f},{sl['dte']},{m['K']:.4f},{m['k']:.6f},"
                f"{m['z']:.6f},{m['mkt_iv']:.6f},{m['iv_err']:.6f},"
                f"{m['bid_iv']:.6f},{m['ask_iv']:.6f},{m['leg']},"
                f"{m['vega']:.6f},{m['in_fit']}"
            )

    (out / "slices.csv").write_text("\n".join(slice_lines) + "\n", encoding="utf-8")
    (out / "total_variance.csv").write_text(
        "expiry_date,T,dte,k,z,w,fit_iv\n" + "\n".join(tv_rows) + "\n", encoding="utf-8")
    (out / "smiles.csv").write_text(
        "expiry_date,T,dte,K,k,z,mkt_iv,iv_err,bid_iv,ask_iv,leg,vega,in_fit\n"
        + "\n".join(smile_rows) + "\n", encoding="utf-8")

    # ── optional earnings files ────────────────────────────────────────────
    if not args.no_earnings:
        (out / "earnings_summary.csv").write_text(
            "iEMove,st,lt,decay,fit_error\n"
            f"{EMOVE:.6f},{SIG_ST:.6f},{SIG_LT:.6f},{SIG_DECAY:.6f},0.002500\n",
            encoding="utf-8")
        te_lines = ["nd,T,atm_dirty,atm_cen,n_earn,event_var_share"]
        for sl in slices:
            share = sl["Vev"] / sl["w0"]
            te_lines.append(
                f"{sl['dte']},{sl['T']:.6f},{sl['sigma0']:.6f},"
                f"{sl['sig_cen']:.6f},{sl['n_earn']},{share:.6f}")
        (out / "earnings_tenors.csv").write_text("\n".join(te_lines) + "\n", encoding="utf-8")

    # ── console summary ────────────────────────────────────────────────────
    print(f"[synthetic] wrote {len(DTES)} expiries to {out}")
    print(f"[synthetic] front: dte={slices[0]['dte']} T={slices[0]['T']:.4f} "
          f"sigma0={slices[0]['sigma0']*100:.1f}% c2_eff={slices[0]['c2_eff']:+.3f} "
          f"s2={slices[0]['s2']:+.3f}")
    print("[synthetic] c2_eff term structure: " + "  ".join(
        f"{sl['dte']}d:{sl['c2_eff']:+.2f}" for sl in slices))
    print("[synthetic] sigma0 term structure: " + "  ".join(
        f"{sl['dte']}d:{sl['sigma0']*100:.0f}%" for sl in slices))
    return 0 if not warns else 1


if __name__ == "__main__":
    raise SystemExit(main())
