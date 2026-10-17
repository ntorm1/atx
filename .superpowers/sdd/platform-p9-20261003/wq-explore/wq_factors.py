"""WQ-explore: admission-definition factor series (centered tied rank, price-risk-v1 residual, gross 1,
f = sum(q * fwd), execution delay 1) for every candidate of an IC-runner summary, using the fitter's own code
(fit_composition_weights.Context / standalone_turnover) and the house price-risk context read from fit-work
(read only). Exploratory, off-ledger, TRAIN only.

usage: wq_factors.py SUMMARY_JSON OUT_NPZ [--combined TRAIN_COMBINED_JSON]
"""
from __future__ import annotations
import json
import sys
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
for p in ("atx-impl/tools", "atx-engine/tools"):
    sys.path.insert(0, str(ROOT / p))
import fit_composition_weights as F  # noqa: E402

ROLE = ROOT / "build-equity/train-2020-2023-lo3/manifest.json"
ROLE_SHA = "e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4"
WORK = ROOT / "build-equity/fit-work"


def context():
    role = F.RoleManifest(ROLE, ROLE_SHA)
    ctx = F.WorkStore(WORK, role).load_context()
    if ctx is None:
        print("context: not in fit-work, building in memory", file=sys.stderr)
        ctx = F.Context.build(role)
    return role, ctx


def series(ctx, signal):
    q, live = ctx.book(signal, 1)
    f = ctx.factor_returns(q)
    slab = signal[ctx.begin:ctx.end][:, ctx.columns]
    fin = (ctx.used & np.isfinite(slab)).sum(axis=1)
    cov = np.where(ctx.used_rows > 0, fin / np.maximum(ctx.used_rows, 1), np.nan)
    # distinct values per date among used finite cells (ties diagnostic), sampled every 50th decision
    return np.where(live, f, np.nan), float(F.standalone_turnover(q)), float(np.nanmean(cov))


def main():
    summary, out = Path(sys.argv[1]), Path(sys.argv[2])
    role, ctx = context()
    shape = (role.dates, role.instruments)
    s = json.loads(summary.read_text())
    entries = s["roles"][0]["candidate_cache"]["entries"]
    ids, fs, taus, covs = [], [], [], []
    for e in entries:
        sig = np.fromfile(ROOT / e["payload"].replace("\\", "/"), dtype="<f8").reshape(shape)
        f, tau, cov = series(ctx, sig)
        ids.append(e["id"]); fs.append(f); taus.append(tau); covs.append(cov)
        print(e["id"], f"tau={tau:.4f} cov={cov:.3f} mean={np.nanmean(f):.3e}", flush=True)
    extra = {}
    if "--combined" in sys.argv:
        cj = Path(sys.argv[sys.argv.index("--combined") + 1])
        sig = np.fromfile(cj.with_suffix(".f64"), dtype="<f8").reshape(shape)
        f, tau, cov = series(ctx, sig)
        extra = {"blend_f": f, "blend_tau": tau}
        print("blend", tau, np.nanmean(f) / np.nanstd(f, ddof=1) * np.sqrt(252))
    sessions = np.fromfile(ROLE.parent / "sessions.i64", dtype="<i8")[ctx.begin:ctx.end]
    np.savez(out, ids=np.array(ids), f=np.array(fs), tau=np.array(taus), cov=np.array(covs), sessions=sessions,
             **extra)


if __name__ == "__main__":
    main()
