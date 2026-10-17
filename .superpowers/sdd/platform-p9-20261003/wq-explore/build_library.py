"""WQ-explore: write lib_wq_all.json (the 101 alphas, raw and in the house decay form) and inventory.json."""
import hashlib, json, sys
from pathlib import Path
sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import wq_translate as wt

FIELDS = ["close", "raw_close", "volume", "open_adj", "high_adj", "low_adj", "me_company", "grp_ff12", "grp_ff49",
          "grp_sic2"]
CITE = "Kakushadze (2016, arXiv:1601.00991) 101 Formulaic Alphas, Alpha#%d"


def cand(cid, family, dsl, n):
    return {"id": cid, "family": family, "dsl": dsl, "horizons": [5, 21, 63], "sign_policy": "train-rank-ic21",
            "theme": "price_volume", "tier": "C+", "tier_rank": 6, "prior_sign": 1, "citation": CITE % n}


def main():
    a = wt.all_alphas()
    cands, inv = [], []
    for n in range(1, 102):
        r = a[n]
        hf, d = wt.house_form(r["dsl"], r["window"])
        cands.append(cand(f"wq{n:03d}", "wq_raw", r["dsl"], n))
        cands.append(cand(f"wq{n:03d}h", "wq_house", hf, n))
        inv.append({"n": n, "class": r["cls"], "exact": r["exact"], "flags": r["flags"], "cluster": r["cluster"],
                    "window": r["window"], "house_decay": d, "note": r["note"], "dsl": r["dsl"], "house_dsl": hf})
    lib = {"schema": "atx.dsl-ic-library/v1", "id": "wq_explore_all",
           "fields": [{"name": f, "basis": "as the primary library / fields-v15 manifest"} for f in FIELDS],
           "families": [{"id": "wq_raw", "description": "Kakushadze (2016) alphas as printed (exploratory)"},
                        {"id": "wq_house", "description": "the same in the house form rank(decay_linear(x, 5 | 21))"}],
           "candidates": cands}
    data = (json.dumps(lib, indent=1) + "\n").encode()
    (HERE / "lib_wq_all.json").write_bytes(data)
    (HERE / "inventory.json").write_text(json.dumps(inv, indent=1) + "\n")
    print(len(cands), hashlib.sha256(data).hexdigest())


if __name__ == "__main__":
    main()
