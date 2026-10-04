"""WQ-explore: candidate "worldquant" themes as IC libraries = the primary library (67) + selected WQ strings.
The registry's unused 13th theme `price_volume` stands for the WorldQuant theme (the fitter admits only registered
themes; the registry is not edited). Selection rules read TRAIN statistics: exploratory, in-sample."""
import csv, hashlib, json, sys
from pathlib import Path
sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
X = ROOT / "build-equity/wq-explore"


def fl(r, k):
    try:
        return float(r[k])
    except ValueError:
        return float("nan")


def main():
    rows = list(csv.DictReader(open(X / "stats/wq_candidates.csv")))
    by = {r["id"]: r for r in rows}
    summ = json.loads((X / "stats/summary.json").read_text())
    adm_h = [r["id"] for r in rows if r["form"] == "house" and r["status"] == "admitted"]
    adm_r = [r["id"] for r in rows if r["form"] == "raw" and r["status"] == "admitted"]
    decor = []
    for c in summ["house"]["clusters_rho0.5_detail"]:
        m = [k for k in c["members"] if k in adm_h]
        if m:
            decor.append(min(m, key=lambda k: fl(by[k], "tau")))  # one per cluster: the slowest (no return read)
    variants = {
        "slow": ("price_volume", [k for k in adm_h if fl(by[k], "tau") <= 0.20]),
        "all": ("price_volume", adm_h),
        "decor": ("price_volume", decor),
        "exact": ("price_volume", [k for k in adm_h if by[k]["exact"] == "True"]),
        "exactslow": ("price_volume", [k for k in adm_h if by[k]["exact"] == "True" and fl(by[k], "tau") <= 0.20]),
        "raw": ("price_volume", adm_r),
        "top10": ("price_volume", sorted(adm_h, key=lambda k: -fl(by[k], "sharpe"))[:10]),  # in-sample pick: upper bound
        "foldrev": ("reversal_seasonality", [k for k in adm_h if fl(by[k], "tau") <= 0.20]),
    }
    wq = {c["id"]: c for c in json.loads((HERE / "lib_wq_all.json").read_text())["candidates"]}
    base = json.loads((ROOT / "atx-impl/strategies/fund_industry_ic_v8ysb.json").read_text())
    out = {}
    for name, (theme, ids) in variants.items():
        lib = json.loads(json.dumps(base))
        lib["id"] = f"wq_explore_{name}"
        have = {f["name"] for f in lib["fields"]}
        for f in ("open_adj", "high_adj", "low_adj", "grp_sic2"):
            if f not in have:
                lib["fields"].append({"name": f, "basis": "fields-v15 manifest"})
        fam = "price_volume" if theme == "price_volume" else theme
        if theme == "price_volume":
            lib["families"].append({"id": "price_volume", "description": "WQ-explore: Kakushadze (2016) formulaic "
                                    "alphas (exploratory; the registry's price_volume theme stands for worldquant)"})
        for k in ids:
            c = dict(wq[k]); c["family"] = fam; c["theme"] = theme
            lib["candidates"].append(c)
        data = (json.dumps(lib, indent=1) + "\n").encode()
        (HERE / f"lib_{name}.json").write_bytes(data)
        out[name] = {"theme": theme, "members": ids, "n": len(ids), "library_sha256": hashlib.sha256(data).hexdigest()}
        print(name, len(ids), out[name]["library_sha256"][:12])
    (HERE / "variants.json").write_text(json.dumps(out, indent=1) + "\n")


if __name__ == "__main__":
    main()
