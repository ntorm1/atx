"""WQ-explore: standalone TRAIN statistics, house admission rules, relation to the primary book, clusters, DSR.
Exploratory, in-sample (TRAIN 2020-2023), off-ledger. Reads f_primary.npz / f_wq.npz (wq_factors.py), the WQ IC run,
the primary fit / NAV reference dirs (read only) and the trial ledger (read only, for the house DSR variance and N).
Writes CSV / JSON under build-equity/wq-explore/stats/.
"""
from __future__ import annotations
import csv
import json
import math
import sys
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
for p in ("atx-impl/tools", "atx-engine/tools"):
    sys.path.insert(0, str(ROOT / p))
import fit_composition_weights as F  # noqa: E402
import backtest_integrity as BI  # noqa: E402
import nav_summ as NS  # noqa: E402
import research_window as rw  # noqa: E402

X = ROOT / "build-equity/wq-explore"
OUT = X / "stats"
ANN = math.sqrt(252)
PRIMARY_SCEN = "modeled-1bn-stale5-v1+swap-fin-v1"


def sharpe(x):
    x = x[np.isfinite(x)]
    sd = x.std(ddof=1)
    return float(x.mean() / sd * ANN) if sd > 0 else float("nan")


def hac_t(x):
    t = F.newey_west_t(x[np.isfinite(x)])
    return float("nan") if t is None else t


def corr(a, b):
    m = np.isfinite(a) & np.isfinite(b)
    if m.sum() < 250 or a[m].std() == 0 or b[m].std() == 0:
        return float("nan")
    return float(np.corrcoef(a[m], b[m])[0, 1])


def nav_net(path, begin):
    """NAV daily net return aligned to factor index i (decision d = begin + i realises at session d + 2)."""
    rows = list(csv.DictReader(open(path)))
    by = {int(r["session_index"]): float(r["net_return"]) for r in rows if r["return_observation"] == "1"}
    return by


def moments_dsr(x, n, variance):
    m = NS.net_moments(x)
    if n < 2:
        sr0 = 0.0
    else:
        sr0 = BI.expected_max_sr(variance, n)
    d = BI.deflated_sharpe(m["sr_daily"], m["sessions"], m["skew"], m["kurtosis"], sr0)
    return d, sr0 * ANN


def cluster(R, ids, thr):
    """Average-linkage agglomerative clustering on distance 1 - rho; merge while the closest pair has rho >= thr."""
    groups = [[i] for i in range(len(ids))]
    while len(groups) > 1:
        best, bi = -2.0, None
        for a in range(len(groups)):
            for b in range(a + 1, len(groups)):
                r = float(np.nanmean(R[np.ix_(groups[a], groups[b])]))
                if r > best:
                    best, bi = r, (a, b)
        if best < thr:
            break
        a, b = bi
        groups[a] += groups[b]
        del groups[b]
    return groups


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    zp, zw = np.load(X / "f_primary.npz"), np.load(X / "f_wq.npz")
    pid, pf = list(zp["ids"]), zp["f"]
    wid, wf, wtau, wcov = list(zw["ids"]), zw["f"], zw["tau"], zw["cov"]
    blend = zp["blend_f"]
    T = pf.shape[1]
    inv = {r["n"]: r for r in json.loads((Path(__file__).parent / "inventory.json").read_text())}
    adm = json.loads((ROOT / "build-equity/p9-b0-yf0-fit/admission.json").read_text())
    wts = json.loads((ROOT / "build-equity/p9-b0-yf0-fit/composition_weights.json").read_text())
    themes = wts["provenance"]["themes"]
    # house theme sleeves: r_t = sum_{k in t} a_k s_k f_k, flat decisions 0 (theme-erc-v1 fit_series)
    sleeve = {}
    for t, row in themes.items():
        r = np.zeros(T)
        for k, a in row["within_theme_shares"].items():
            r += a * np.nan_to_num(pf[pid.index(k)])
        sleeve[t] = r
    tnames = sorted(sleeve)
    S = np.column_stack([sleeve[t] for t in tnames])
    lin_blend = sum(w * np.nan_to_num(pf[pid.index(k)]) for k, w in wts["weights"].items() if w > 0)
    # NAV primary scenario
    role = json.loads((ROOT / "build-equity/train-2020-2023-lo3/manifest.json").read_text())
    begin = role["score_begin"]
    nav = nav_net(ROOT / f"build-equity/p9-b0-yf0-nav/daily_{PRIMARY_SCEN}.csv", begin)
    navf = np.array([nav.get(begin + i + 2, np.nan) for i in range(T)])
    print("primary: blend(reranked) factor SR", round(sharpe(blend), 3), "linear blend SR", round(sharpe(lin_blend), 3),
          "NAV net SR", round(sharpe(navf), 3), "corr(blend f, NAV net)", round(corr(blend, navf), 3),
          "nav rows aligned", int(np.isfinite(navf).sum()))
    # IC table
    ic = {}
    for line in open(X / "u-all/train_candidates.jsonl"):
        j = json.loads(line)
        if "horizons" in j:
            ic[j["id"]] = {h["horizon"]: (h["rank"]["mean"], h["rank"]["standard_error"], h["rank"]["valid_dates"],
                                          h["coverage"]["paired_signal_pairs"] / max(h["coverage"]["decision_eligible_pairs"], 1))
                           for h in j["horizons"]}
            ic[j["id"]]["status"] = (j.get("status"), j.get("reason"))
    admitted_primary = [c["id"] for c in adm["candidates"] if c["status"] == "admitted"]
    AP = np.array([pf[pid.index(k)] for k in admitted_primary])
    rows = []
    X3 = np.column_stack([np.ones(T)] + [sleeve[t] for t in ("reversal_seasonality", "low_risk", "price_momentum")])
    X12 = np.column_stack([np.ones(T), S])
    for i, k in enumerate(wid):
        f = wf[i]
        n = int(k[2:5])
        house = k.endswith("h")
        r = inv[n]
        live = np.isfinite(f)
        row = {"id": k, "paper": n, "form": "house" if house else "raw", "class": r["class"], "exact": r["exact"],
               "flags": "|".join(r["flags"]), "window": r["window"], "decay": r["house_decay"] if house else 0,
               "live_days": int(live.sum()), "coverage": round(float(wcov[i]), 4), "tau": float(wtau[i]),
               "mean_bps": float(np.nanmean(f) * 1e4) if live.any() else float("nan"),
               "sharpe": sharpe(f) if live.sum() > 2 else float("nan"),
               "hac_t": hac_t(f) if live.sum() > 2 else float("nan")}
        for h in (5, 21, 63):
            m, se, nd, cv = ic.get(k, {}).get(h, (float("nan"),) * 4)
            row[f"ic{h}"] = m
            row[f"ic{h}_t"] = m / se if se and se > 0 else float("nan")
        row["ic_dates21"] = ic.get(k, {}).get(21, (0, 0, 0, 0))[2]
        fz = np.nan_to_num(f)
        row["rho_blend"] = corr(f, blend)
        row["rho_nav"] = corr(f, navf)
        for t in tnames:
            row[f"rho_{t}"] = corr(f, sleeve[t])
        rp = [corr(f, AP[j]) for j in range(len(admitted_primary))]
        j = int(np.nanargmax(np.abs(rp))) if np.isfinite(rp).any() else 0
        row["max_rho_primary"] = rp[j]
        row["max_rho_primary_with"] = admitted_primary[j]
        for name, D in (("r2_3themes", X3), ("r2_12themes", X12)):
            if live.sum() > 250 and fz.std() > 0:
                b, *_ = np.linalg.lstsq(D, fz, rcond=None)
                e = fz - D @ b
                row[name] = 1 - e.var() / fz.var()
                if name == "r2_12themes":
                    al = e + b[0]
                    row["alpha12_bps"] = b[0] * 1e4
                    row["alpha12_hac_t"] = hac_t(al)
                    row["alpha12_ir"] = sharpe(al)
            else:
                row[name] = float("nan")
        # house checks (prior sign +1 as printed; no sign fitting)
        fails = []
        if live.sum() < 250:
            fails.append("insufficient")
        if not row["tau"] <= 0.70:
            fails.append("turnover")
        if row["hac_t"] < -2.0:
            fails.append("veto")
        row["checks"] = "|".join(fails) if fails else "pass"
        rows.append(row)
    # redundancy: against the primary's admitted members first (better tiers), then WQ in roster order, per form
    for form in ("raw", "house"):
        kept = []
        for row in rows:
            if row["form"] != form:
                continue
            if row["checks"] != "pass":
                row["status"] = "reject_" + row["checks"].split("|")[0]
                continue
            if row["coverage"] < 0.5:  # exploratory rule (not a house rule): a signal on under half the used names
                row["status"] = "reject_low_coverage"
                continue
            f = wf[wid.index(row["id"])]
            if abs(row["max_rho_primary"]) > 0.90:
                row["status"] = f"reject_redundant({row['max_rho_primary_with']})"
                continue
            red = [(abs(corr(f, wf[wid.index(o)])), o) for o in kept]
            red = [x for x in red if x[0] > 0.90]
            if red:
                row["status"] = f"reject_redundant({max(red)[1]})"
                continue
            row["status"] = "admitted"
            kept.append(row["id"])
        print(form, "admissible by the house screen:", len(kept), "of 101")
    cols = list(rows[0].keys())
    for r in rows:
        for c in r:
            if c not in cols:
                cols.append(c)
    with open(OUT / "wq_candidates.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow({c: (f"{v:.6g}" if isinstance(v, float) else v) for c, v in r.items()})
    # clusters and effective number
    summary = {}
    for form in ("raw", "house"):
        sel = [r for r in rows if r["form"] == form and r["live_days"] >= 250]
        ids = [r["id"] for r in sel]
        M = np.array([np.nan_to_num(wf[wid.index(k)]) for k in ids])
        M = M[:, :]
        ok = M.std(axis=1) > 0
        ids = [k for k, o in zip(ids, ok) if o]
        M = M[ok]
        R = np.corrcoef(M)
        ev = np.linalg.eigvalsh(R)[::-1]
        neff = float(ev.sum() ** 2 / (ev ** 2).sum())
        cum = np.cumsum(ev) / ev.sum()
        info = {"n": len(ids), "mean_pairwise_rho": float((R.sum() - len(ids)) / (len(ids) * (len(ids) - 1))),
                "mean_abs_pairwise_rho": float((np.abs(R).sum() - len(ids)) / (len(ids) * (len(ids) - 1))),
                "participation_ratio_neff": neff, "pcs_for_50pct": int(np.searchsorted(cum, 0.5) + 1),
                "pcs_for_80pct": int(np.searchsorted(cum, 0.8) + 1), "pcs_for_90pct": int(np.searchsorted(cum, 0.9) + 1),
                "top5_eig_share": [float(x) for x in (ev[:5] / ev.sum())]}
        # PC1..3 relation to themes
        w_, v_ = np.linalg.eigh(R)
        pcs = []
        Z = (M - M.mean(axis=1, keepdims=True)) / M.std(axis=1, keepdims=True)
        for j in (1, 2, 3):
            pc = v_[:, -j] @ Z
            pcs.append({"pc": j, "share": float(ev[j - 1] / ev.sum()), "rho_blend": corr(pc, blend),
                        "rho_themes": {t: round(corr(pc, sleeve[t]), 3) for t in tnames}})
        info["pcs"] = pcs
        for thr in (0.7, 0.5, 0.3):
            g = cluster(R, ids, thr)
            info[f"clusters_rho{thr}"] = len(g)
            if thr == 0.5:
                cl = []
                for grp in sorted(g, key=len, reverse=True):
                    cm = M[grp].mean(axis=0)
                    rt = {t: corr(cm, sleeve[t]) for t in tnames}
                    bt = max(rt, key=lambda t: abs(rt[t]))
                    b, *_ = np.linalg.lstsq(X12, cm, rcond=None)
                    r2 = 1 - (cm - X12 @ b).var() / cm.var()
                    cl.append({"size": len(grp), "members": [ids[i] for i in grp], "sleeve_sharpe": sharpe(cm),
                               "nearest_theme": bt, "rho_nearest": rt[bt], "rho_blend": corr(cm, blend),
                               "r2_12themes": float(r2)})
                info["clusters_rho0.5_detail"] = cl
        summary[form] = info
        print(form, {k: v for k, v in info.items() if k not in ("clusters_rho0.5_detail", "pcs")})
    # spanned share
    for form in ("raw", "house"):
        sel = [r for r in rows if r["form"] == form and np.isfinite(r.get("r2_12themes", np.nan))]
        summary[form]["median_r2_3themes"] = float(np.median([r["r2_3themes"] for r in sel]))
        summary[form]["median_r2_12themes"] = float(np.median([r["r2_12themes"] for r in sel]))
        summary[form]["mean_r2_12themes"] = float(np.mean([r["r2_12themes"] for r in sel]))
    # DSR for the best candidates
    recs = BI.ledger_read(ROOT / "build-equity/trials.jsonl")
    hv = BI.dsr_variance(recs, rw.WINDOW_ID)["variance_sr"]
    import dsr_total as DT
    ntot = DT.trial_breakdown(recs)["n_tot"] + 1
    nc = BI.ledger_n(recs, False)
    srs = np.array([r["sharpe"] / ANN for r in rows if np.isfinite(r["sharpe"])])
    own_v = float(srs.var(ddof=1))
    summary["dsr_inputs"] = {"house_variance_sr": hv, "house_n_construction_plus1": nc, "house_n_tot_plus1": ntot,
                             "own_variance_sr_202": own_v, "own_n": int(srs.size)}
    best = sorted([r for r in rows if r.get("status") == "admitted"], key=lambda r: -r["sharpe"])[:12]
    dsr_rows = []
    for r in best:
        f = wf[wid.index(r["id"])]
        x = f[np.isfinite(f)]
        d = {"id": r["id"], "sharpe": r["sharpe"], "tau": r["tau"]}
        d["psr_n1"] = moments_dsr(x, 1, 0.0)[0]
        d["dsr_n202_ownV"], d["sr0_n202_ownV"] = moments_dsr(x, int(srs.size), own_v)
        d["dsr_n101_ownV"] = moments_dsr(x, 101, own_v)[0]
        d["dsr_houseN%d_houseV" % nc], d["sr0_houseNc"] = moments_dsr(x, nc, hv)
        d["dsr_houseNtot%d_houseV" % ntot], d["sr0_houseNtot"] = moments_dsr(x, ntot, hv)
        d["dsr_n%d_houseV" % (ntot + 202)] = moments_dsr(x, ntot + 202, hv)[0]
        dsr_rows.append(d)
    summary["dsr_best"] = dsr_rows
    (OUT / "summary.json").write_text(json.dumps(summary, indent=1, default=float) + "\n")
    np.savez(OUT / "primary_series.npz", blend=blend, lin_blend=lin_blend, nav=navf, sleeves=S, themes=np.array(tnames))
    for d in dsr_rows:
        print({k: (round(v, 3) if isinstance(v, float) else v) for k, v in d.items()})


if __name__ == "__main__":
    main()
