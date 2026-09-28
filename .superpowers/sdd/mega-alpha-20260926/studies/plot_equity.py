"""Equity curves of the mega-alpha books from NAV daily CSVs (book level only) -> PNG."""
import csv
import datetime as dt
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.dates as mdates  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

BE = "C:/atx-wt/pool-2/build-equity"
PRIMARY = "modeled-1bn-stale5-v1+swap-fin-v1"
S1 = "linear-6bps-stale5-v1+swap-fin-v1"


def load(nav_dir, scenario=PRIMARY):
    rows = [r for r in csv.DictReader(open(f"{BE}/{nav_dir}/daily_{scenario}.csv")) if r["return_observation"] == "1"]
    t = [dt.datetime.fromtimestamp(int(r["session_ns"]) / 1e9, dt.UTC) for r in rows]
    net = np.array([float(r["net_return"]) for r in rows])
    gross = np.array([float(r["gross_return"]) for r in rows])
    return t, net, gross


def sharpe(x):
    return x.mean() / x.std(ddof=1) * np.sqrt(252)


def panel(ax, title, train_dir, val_dir):
    tt, tn, tg = load(train_dir)
    vt, vn, vg = load(val_dir)
    _, vs1, _ = load(val_dir, S1)
    _, ts1, _ = load(train_dir, S1)
    # Chain TRAIN then VAL so the curve is continuous; the split line marks where the frozen book goes out of sample.
    t = tt + vt
    for series, label, style in (
        (np.concatenate([tg, vg]), "gross", dict(color="0.55", lw=1.1, ls="--")),
        (np.concatenate([ts1, vs1]), "net, S1 linear 6 bps", dict(color="tab:green", lw=1.1)),
        (np.concatenate([tn, vn]), "net, S2 $1bn x swap-fin (primary)", dict(color="tab:blue", lw=1.8)),
    ):
        ax.plot(t, np.cumprod(1 + series), label=label, **style)
    ax.axvline(vt[0], color="tab:red", lw=1)
    ax.axhline(1, color="0.8", lw=0.8)
    ax.axvspan(vt[0], vt[-1], color="tab:red", alpha=0.05)
    ymax = ax.get_ylim()[1]
    ax.text(tt[len(tt) // 2], ymax, f"TRAIN (in-sample)\nnet SR {sharpe(tn):+.2f}  gross {sharpe(tg):+.2f}",
            ha="center", va="top", fontsize=9)
    ax.text(vt[len(vt) // 2], ymax, f"VALIDATION (out-of-sample)\nnet SR {sharpe(vn):+.2f}  gross {sharpe(vg):+.2f}",
            ha="center", va="top", fontsize=9, color="tab:red")
    ax.set_title(title, fontsize=11, loc="left")
    ax.set_ylabel("growth of $1 (NAV)")
    ax.legend(loc="lower left", fontsize=8, frameon=False)
    ax.xaxis.set_major_locator(mdates.YearLocator())
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y"))
    ax.grid(alpha=0.25)


def main(out):
    fig, axes = plt.subplots(2, 1, figsize=(12, 9), sharex=True)
    panel(axes[0], "v4.1 frozen book (prior-signed fundamentals + industry + price/SI/IV; band 2, fraction .25) "
          "— validation trial #2", "mega-nav-v4-train-b2-f.25", "mega-nav-v4-VAL-b2-f.25")
    panel(axes[1], "v3 frozen book (data-mined signs, MV weights; band 1, fraction 1) — validation trial #1",
          "mega-nav-v3-plain-b1", "mega-nav-v3-VAL")
    fig.suptitle("Mega-alpha equity curves, $1bn NAV, daily rebalance, market-neutral", fontsize=13)
    fig.tight_layout()
    fig.savefig(out, dpi=140)
    print(out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else f"{BE}/plots/mega-alpha-equity-curves.png")
