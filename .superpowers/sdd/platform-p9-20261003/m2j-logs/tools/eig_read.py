"""D-COV-EIG (Ruling COV-13 (b); zero trials): what the lowest eigen ranks of the two D-COV stores are made of.

usage: python eig_read.py V11_DIR COVV1_DIR

Opens, per store, exactly three files and nothing else: `manifest.json` (factor names `recipe.factors`, `geometry`,
`layouts`, `model`, the `files` pins), `factor_covariance.f64` (the forecast factor blocks) and `factor_structural.u8`
(the structural flags). Never opens bias.csv, bias_summary.json, factor_returns.csv, diagnostics.csv or any other
payload: no return, no realised value, no bias statistic is read. Before reading, each store's manifest.json SHA-256
must equal its M2i pin (handoff-root-m2i.md "Store pins") and both payloads must equal their `files` pins (SHA-256 and
size); otherwise the tool refuses (exit 2).

Per session t of the store (the eigen family's own rule, atx-impl/src/strategy_risk_model.cpp BiasHarness::form_extended):
the block = the factors whose forecast variance F_t[k, k] is finite; the block is formed only when every entry is
finite and the smallest eigenvalue of the symmetric eigensolution is > 0; ranks ascend by eigenvalue (eigen-00 the
smallest). A formed block is scored by the harness at the next session, so the sessions read here are the formed
sessions t with a next session (t + 1 < dates); rank k exists at t when the block has more than k factors. The count
per rank is printed so it can be checked against T_k of gate report item 4 (d).

For ranks 00, 01, 02, per session: the eigenvalue over the median of that session's block eigenvalues; the unit
eigenvector's squared loadings (sum 1) split into a partition of the block: (i) factors flagged structural at t
(factor_structural.u8 = 1), (ii) the industry block (ind_ff1..ind_ff49, ind_residual) plus the market factor, not
flagged at t, (iii) the style factors, not flagged at t. Prints, per recipe and rank: the session count; the median over
sessions of eigenvalue / median eigenvalue; the median over sessions of share (i), of share (ii), of share (iii) and of
share (i) + (ii) (the per-session sum, the reading rule's quantity); for information the medians of the unpartitioned
industry + market share and style share (structural members included); the five factor names with the largest median
over sessions of squared loading (0 in a session where the factor is outside the block). Reading rule (COV-13 (b),
fixed before the run): a rank is "structurally null" iff the median over sessions of share (i) + (ii) is >= .99.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import numpy as np

PINS = {  # manifest.json SHA-256 of record (handoff-root-m2i.md, W3-10 / PRE-29)
    "p9-m2i-dcov-v11": "397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7",
    "p9-m2i-dcov-covv1": "ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3",
}
OPENED = ("manifest.json", "factor_covariance.f64", "factor_structural.u8")
RANKS = (0, 1, 2)
THRESHOLD = 0.99


def sha_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def load(store: Path):
    raw = (store / "manifest.json").read_bytes()
    got = sha_bytes(raw)
    want = PINS.get(store.name)
    if want is None or got != want:
        raise SystemExit(f"REFUSED {store.as_posix()}: manifest.json sha256 {got} != pin {want}")
    man = json.loads(raw.decode("utf-8"))
    payload = {}
    for name in OPENED[1:]:
        data = (store / name).read_bytes()
        rec = (man.get("files") or {}).get(name) or {}
        if sha_bytes(data) != rec.get("sha256") or len(data) != rec.get("bytes"):
            raise SystemExit(f"REFUSED {store.as_posix()}: {name} does not match its files pin")
        payload[name] = data
    names = list(man["recipe"]["factors"])
    geo = man["geometry"]
    dates, nf = int(geo["dates"]), int(geo["factors"])
    if len(names) != nf:
        raise SystemExit(f"REFUSED {store.as_posix()}: {len(names)} factor names vs geometry.factors {nf}")
    cov = np.frombuffer(payload["factor_covariance.f64"], dtype="<f8")
    flags = np.frombuffer(payload["factor_structural.u8"], dtype=np.uint8)
    if cov.size != dates * nf * nf or flags.size != dates * nf:
        raise SystemExit(f"REFUSED {store.as_posix()}: payload sizes do not match dates x factors")
    return man, names, cov.reshape(dates, nf, nf), flags.reshape(dates, nf)


def read_store(store: Path) -> dict:
    man, names, cov, flags = load(store)
    dates, nf = flags.shape
    market = np.array([n == "market" for n in names])
    industry = np.array([n.startswith("ind_") for n in names])
    style = np.array([n.startswith("style_") for n in names])
    if not np.all(market | industry | style) or market.sum() != 1:
        raise SystemExit(f"REFUSED {store.as_posix()}: a factor name is neither market, ind_* nor style_*")
    per_rank = {k: {"ratio": [], "s1": [], "s2": [], "s3": [], "s12": [], "ind_all": [], "sty_all": [],
                    "sq": []} for k in RANKS}
    count_by_rank = np.zeros(nf, dtype=np.int64)
    formed = 0
    flagged_sessions = 0
    max_partition_error = 0.0
    for t in range(dates):
        diag = np.diagonal(cov[t])
        block = np.flatnonzero(np.isfinite(diag))
        m = block.size
        if m == 0:
            continue
        f = cov[t][np.ix_(block, block)]
        if not np.all(np.isfinite(f)):
            continue
        values, vectors = np.linalg.eigh(f)
        if not values.min() > 0:
            continue
        formed += 1
        if t + 1 >= dates:  # formed at the last session: never scored
            continue
        count_by_rank[:m] += 1
        fl = flags[t, block] == 1
        flagged_sessions += bool(fl.any())
        med = float(np.median(values))
        for k in RANKS:
            if k >= m:
                continue
            u2 = vectors[:, k] ** 2
            s1 = float(u2[fl].sum())
            s2 = float(u2[~fl & (market[block] | industry[block])].sum())
            s3 = float(u2[~fl & style[block]].sum())
            max_partition_error = max(max_partition_error, abs(s1 + s2 + s3 - 1.0))
            row = per_rank[k]
            row["ratio"].append(values[k] / med)
            row["s1"].append(s1)
            row["s2"].append(s2)
            row["s3"].append(s3)
            row["s12"].append(s1 + s2)
            row["ind_all"].append(float(u2[market[block] | industry[block]].sum()))
            row["sty_all"].append(float(u2[style[block]].sum()))
            full = np.zeros(nf)
            full[block] = u2
            row["sq"].append(full)
    return {"model": man.get("model"), "names": names, "dates": dates, "formed": formed,
            "count_by_rank": count_by_rank, "per_rank": per_rank, "flagged_sessions": flagged_sessions,
            "max_partition_error": max_partition_error, "layouts": man.get("layouts", {})}


def main(argv: list[str]) -> int:
    stores = [Path(a) for a in argv]
    print("D-COV-EIG read (COV-13 (b)); files opened per store: " + ", ".join(OPENED))
    verdicts = {}
    for store in stores:
        r = read_store(store)
        print(f"\n## {store.as_posix()}  model {r['model']}  (manifest pin verified; payload pins verified)")
        print(f"layout factor_covariance.f64: {r['layouts'].get('factor_covariance.f64')}")
        print(f"layout factor_structural.u8: {r['layouts'].get('factor_structural.u8')}")
        print(f"dates {r['dates']}; formed blocks {r['formed']}; scored sessions per rank (blocks with > k factors, "
              f"next session present): ranks 00-47 {sorted(set(r['count_by_rank'][:48].tolist()))}, "
              f"48 {r['count_by_rank'][48]}, 49 {r['count_by_rank'][49]}, 50 {r['count_by_rank'][50]}, "
              f"51+ {r['count_by_rank'][51:].tolist()}")
        print(f"scored sessions with at least one structural factor in the block: {r['flagged_sessions']}; "
              f"max |share(i) + (ii) + (iii) - 1| {r['max_partition_error']:.2e}")
        names = r["names"]
        for k in RANKS:
            row = r["per_rank"][k]
            n = len(row["ratio"])
            if n == 0:
                verdicts[(store.name, k)] = None
                print(f"\neigen-{k:02d}  sessions 0: nothing to read")
                continue
            med ={key: float(np.median(row[key])) for key in ("ratio", "s1", "s2", "s3", "s12", "ind_all", "sty_all")}
            sq = np.median(np.vstack(row["sq"]), axis=0)
            top = np.argsort(-sq, kind="stable")[:5]
            null = med["s12"] >= THRESHOLD
            verdicts[(store.name, k)] = null
            print(f"\neigen-{k:02d}  sessions {n}")
            print(f"  median eigenvalue / session median eigenvalue  {med['ratio']:.6e}")
            print(f"  median share (i) structural                    {med['s1']:.6f}")
            print(f"  median share (ii) industry + market (unflagged) {med['s2']:.6f}")
            print(f"  median share (iii) style (unflagged)           {med['s3']:.6f}")
            print(f"  median share (i) + (ii), per session           {med['s12']:.6f}")
            print(f"  info: median industry + market share (all)     {med['ind_all']:.6f}; "
                  f"style share (all) {med['sty_all']:.6f}")
            print("  five largest median squared loadings: " +
                  ", ".join(f"{names[a]} {sq[a]:.4f}" for a in top))
            print(f"  reading rule (i) + (ii) >= {THRESHOLD}: {'STRUCTURALLY NULL' if null else 'not structurally null'}")
    print("\n## Verdicts")
    for (store, k), null in verdicts.items():
        print(f"{store} eigen-{k:02d}: " +
              ("no sessions" if null is None else "structurally null" if null else "not structurally null"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
