"""WQ-explore: blend-level (admission-definition) factor statistics of every variant's saved blend, the WQ sleeve's
own statistics, and fixed-share mixtures of the primary blend with a 13-theme blend.

  wq_blend.py stats                      -> stats/blend_stats.json (+ printed table)
  wq_blend.py mix NAME LAMBDA OUTNAME    -> build-equity/wq-explore/OUTNAME/w/train_combined.* with the signal
                                            (1 - LAMBDA) * primary + LAMBDA * NAME (same support); the WQ theme then
                                            carries LAMBDA x its theme-erc weight. A hand-made signal: not a house
                                            composition rule.
Exploratory, in-sample, off-ledger.
"""
from __future__ import annotations
import hashlib
import json
import math
import shutil
import sys
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
sys.path.insert(0, str(HERE))
import wq_factors as WF  # noqa: E402
import wq_stats as WS  # noqa: E402
F = WF.F
X = ROOT / "build-equity/wq-explore"


def load_combined(name):
    j = json.loads((X / name / "w/train_combined.json").read_text())
    return np.fromfile(X / name / "w/train_combined.f64", dtype="<f8").reshape(j["dates"], j["instruments"]), j


def mix(name, lam, outname):
    p, _ = load_combined("primary-rel")
    c, j = load_combined(name)
    assert np.array_equal(np.isfinite(p), np.isfinite(c)), "supports differ"
    m = (1.0 - lam) * p + lam * c
    out = X / outname / "w"
    out.mkdir(parents=True, exist_ok=True)
    for f in j["files"]:
        if f != "train_combined.f64":
            shutil.copyfile(X / name / "w" / f, out / f)
    data = m.astype("<f8").tobytes()
    (out / "train_combined.f64").write_bytes(data)
    j["files"]["train_combined.f64"] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    (out / "train_combined.json").write_text(json.dumps(j, indent=2) + "\n")
    (out / "WQ_EXPLORE_NOTE.txt").write_text(
        f"hand-made mixture (1 - {lam}) * primary-rel + {lam} * {name}; the JSON's composition fields are {name}'s\n")
    print("wrote", out)


def stats():
    role, ctx = WF.context()
    zp = np.load(X / "stats/primary_series.npz")
    sleeves, tnames, navp = zp["sleeves"], list(zp["themes"]), zp["nav"]
    zw = np.load(X / "f_wq.npz")
    wid, wf = list(zw["ids"]), zw["f"]
    T = wf.shape[1]
    X12 = np.column_stack([np.ones(T), sleeves])
    recs = WS.BI.ledger_read(ROOT / "build-equity/trials.jsonl")
    hv = WS.BI.dsr_variance(recs, WS.rw.WINDOW_ID)["variance_sr"]
    import dsr_total as DT
    ntot = DT.trial_breakdown(recs)["n_tot"] + 1
    nc = WS.BI.ledger_n(recs, False)
    out = {}
    names = sorted(p.parent.parent.name for p in X.glob("*/w/train_combined.json"))
    base = None
    for name in ["primary-rel"] + [n for n in names if n != "primary-rel"]:
        sig, _ = load_combined(name)
        f, tau, _cov = WF.series(ctx, sig)
        row = {"blend_factor_sharpe": WS.sharpe(f), "blend_tau": tau, "blend_hac_t": WS.hac_t(f),
               "blend_mean_bps": float(np.nanmean(f) * 1e4)}
        if base is None:
            base = f
        else:
            ps = WS.NS.paired_stats(np.nan_to_num(f), np.nan_to_num(base))
            row.update(d_sharpe_vs_primary=ps["dsr"], memmel_se=ps["memmel_se"], rho_primary=ps["rho"],
                       cbb_ci95=ps["cbb_ci95"], lw_p=ps["lw"]["p_value"])
        fit = X / name / "fit/composition_weights.json"
        if fit.exists():
            w = json.loads(fit.read_text())
            th = w["provenance"]["themes"].get("price_volume")
            if th:
                s = np.zeros(T)
                for k, a in th["within_theme_shares"].items():
                    s += a * np.nan_to_num(wf[wid.index(k)])
                b, *_ = np.linalg.lstsq(X12, s, rcond=None)
                e = s - X12 @ b
                d = {"n": th["admitted_count"], "theme_weight": th["theme_weight"], "sharpe": WS.sharpe(s),
                     "hac_t": WS.hac_t(s), "mean_bps": float(s.mean() * 1e4),
                     "vol_ann": float(s.std(ddof=1) * math.sqrt(252)),
                     "tau_weighted_members": float(sum(a * zw["tau"][wid.index(k)]
                                                       for k, a in th["within_theme_shares"].items())),
                     "rho_primary_blend": WS.corr(s, base), "rho_primary_nav": WS.corr(s, navp),
                     "rho_themes": {t: WS.corr(s, sleeves[:, i]) for i, t in enumerate(tnames)},
                     "r2_12themes": float(1 - e.var() / s.var()), "alpha12_bps": float(b[0] * 1e4),
                     "alpha12_hac_t": WS.hac_t(e + b[0]), "alpha12_ir": WS.sharpe(e + b[0]),
                     "betas12": {t: float(b[i + 1]) for i, t in enumerate(tnames)}}
                X3 = np.column_stack([np.ones(T)] + [sleeves[:, tnames.index(t)] for t in
                                                     ("reversal_seasonality", "low_risk", "price_momentum")])
                b3, *_ = np.linalg.lstsq(X3, s, rcond=None)
                d["r2_3themes"] = float(1 - (s - X3 @ b3).var() / s.var())
                d["psr_n1"] = WS.moments_dsr(s, 1, 0.0)[0]
                d["dsr_n8_variants_houseV"] = WS.moments_dsr(s, 8, hv)[0]
                d["dsr_n202_houseV"] = WS.moments_dsr(s, 202, hv)[0]
                d[f"dsr_n{nc}_houseV"] = WS.moments_dsr(s, nc, hv)[0]
                d[f"dsr_n{ntot}_houseV"] = WS.moments_dsr(s, ntot, hv)[0]
                d[f"dsr_n{ntot + 202}_houseV"] = WS.moments_dsr(s, ntot + 202, hv)[0]
                row["wq_sleeve"] = d
        out[name] = row
        print(name, {k: (round(v, 3) if isinstance(v, float) else v) for k, v in row.items() if k != "wq_sleeve"})
        if "wq_sleeve" in row:
            d = row["wq_sleeve"]
            print("   sleeve", {k: (round(v, 3) if isinstance(v, float) else v) for k, v in d.items()
                                if k not in ("rho_themes", "betas12")})
            print("   rho_themes", {k: round(v, 2) for k, v in d["rho_themes"].items()})
    (X / "stats/blend_stats.json").write_text(json.dumps(out, indent=1, default=float) + "\n")


if __name__ == "__main__":
    if sys.argv[1] == "mix":
        mix(sys.argv[2], float(sys.argv[3]), sys.argv[4])
    else:
        stats()
