"""Figure for the stat-arb cluster study from its saved outputs (no raw data read) -> OUT/statarb-cluster-study.png."""
import datetime as dt
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

OUT = Path("C:/atx-wt/pool-2/build-equity/statarb-cluster-v1")


def main():
    a = json.loads((OUT / "analyze.json").read_text())
    m = np.load(OUT / "model_series.npz", allow_pickle=False)
    uni = {r["feature"]: r for r in a["univariate"]}
    fig, ax = plt.subplots(2, 2, figsize=(14, 9.5))

    g = ax[0, 0]
    names = ["c10", "c30", "c100", "ff49", "rnd30"]
    tight = [json.loads((OUT / f"groups_{n}.json").read_text())["summary"]["tight_mean"] for n in names]
    pur = [json.loads((OUT / f"groups_{n}.json").read_text())["summary"]["ff12_purity"] for n in names]
    x = np.arange(len(names))
    g.bar(x - 0.2, tight, 0.4, label="mean within-group corr (market-residual returns)")
    g.bar(x + 0.2, pur, 0.4, label="FF12 purity (share in modal SIC sector)", color="0.7")
    g.set_xticks(x, ["k-means K=10", "k-means K=30", "k-means K=100", "SIC FF49", "random K=30"])
    g.set_title("A. Clusters: data-driven groups are ~2x tighter than SIC industries", loc="left", fontsize=10)
    g.legend(fontsize=8, frameon=False)

    g = ax[0, 1]
    feats = ["c30_dev_1", "c30_dev_5", "c30_sscore", "c30_grp_dev_5", "c30_grp_mkt_5", "c30_nbr_mkt_5",
             "c30_grp_mom_12_1", "rnd30_dev_5", "ff49_dev_5", "rev_5"]
    for j, (t, lab) in enumerate((("y1", "t+1 (same-close fill)"), ("y2", "t+2 (our clock)"), ("y5", "t+2..t+6"))):
        g.barh(np.arange(len(feats)) + (j - 1) * 0.27, [uni[f][t]["nw_t"] for f in feats], 0.27, label=lab)
    g.axvline(a["bonferroni_abs_t"], color="tab:red", ls="--", lw=1)
    g.axvline(-a["bonferroni_abs_t"], color="tab:red", ls="--", lw=1, label=f"Bonferroni |t|={a['bonferroni_abs_t']:.2f}")
    g.axvline(0, color="k", lw=0.6)
    g.set_yticks(np.arange(len(feats)), feats, fontsize=8)
    g.invert_yaxis()
    g.set_title("B. Daily rank-IC Newey-West t, TRAIN 2020-22: none significant", loc="left", fontsize=10)
    g.legend(fontsize=7, frameon=False, loc="lower right")

    g = ax[1, 0]
    per = m["period"]
    t = [dt.datetime.fromtimestamp(int(s) / 1e9, dt.UTC) for s in m["sessions"]]
    hold = np.flatnonzero(per == "HOLD")
    for key, lab, st in (("hgb_FULL", "boosted trees, controls+SIC+clusters", dict(color="tab:blue", lw=1.8)),
                         ("hgb_CTRL+FF49", "boosted trees, controls+SIC (no clusters)", dict(color="tab:orange", lw=1.4)),
                         ("ols_FULL", "linear, controls+SIC+clusters", dict(color="0.5", lw=1.2, ls="--"))):
        r = np.nan_to_num(m[f"ret|{key}|hl5"][hold])
        tau = np.nan_to_num(m[f"tau|{key}|hl5"][hold])
        g.plot([t[i] for i in hold], np.cumprod(1 + r), label=f"{lab}: gross", **st)
        g.plot([t[i] for i in hold], np.cumprod(1 + r - 5e-4 * tau), alpha=0.45, **{**st, "lw": 1})
    g.axhline(1, color="0.8", lw=0.8)
    g.set_title("C. HOLD 2022 (fit on 2020-21): unit-gross neutral books, EMA hl 5; faint = net 5 bps", loc="left",
                fontsize=10)
    g.legend(fontsize=8, frameon=False)
    g.grid(alpha=0.25)

    g = ax[1, 1]
    fm = a["fama_macbeth"]["M2_ctrl+ff49+c30full"]
    keys = [k for k in fm["y2"] if k != "intercept"]
    for j, tt in enumerate(("y2", "y5")):
        g.barh(np.arange(len(keys)) + (j - 0.5) * 0.4, [fm[tt][k]["nw_t"] for k in keys], 0.4, label=tt)
    for v in (-2, 2):
        g.axvline(v, color="0.6", ls=":", lw=1)
    for v in (-3, 3):
        g.axvline(v, color="tab:red", ls="--", lw=1)
    g.axvline(0, color="k", lw=0.6)
    g.set_yticks(np.arange(len(keys)), keys, fontsize=7)
    g.invert_yaxis()
    g.set_title("D. Fama-MacBeth t, all features jointly (controls + SIC + clusters K=30)", loc="left", fontsize=10)
    g.legend(fontsize=8, frameon=False)

    fig.suptitle("Stat-arb cluster divergence study - TRAIN 2020-2022 only (validation sealed), top-3000 ADV US stocks",
                 fontsize=12)
    fig.tight_layout()
    fig.savefig(OUT / "statarb-cluster-study.png", dpi=130)
    print(OUT / "statarb-cluster-study.png")


if __name__ == "__main__":
    main()
