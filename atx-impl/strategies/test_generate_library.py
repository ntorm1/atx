"""generate_library.py (platform v8 lane A, task A-1): the alpha registry and the one library generator.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/strategies/test_generate_library.py

The K1 plan rows come from alphas/fixtures/v71_plan_k1.json, written by hand from contract K1 and the v7.1 recipe (lane
B-3 is not merged). Root re-checks against the exe: save `atx-equity-strategy-ic --plan-only --library
atx-impl/strategies/fund_industry_ic_v71.json --library-sha256 787c802e... --train <role manifest> --train-sha256 <pin>
--train-fields <fields-v9 dir> --train-fields-sha256 <pin>` to a file and set ATX_V71_PLAN_JSON to it.
"""
from __future__ import annotations

import copy
import hashlib
import json
import os
import shutil
from pathlib import Path

import pytest

import generate_library as G

HERE = Path(__file__).resolve().parent
V71_SHA256 = "787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259"
V70_SHA256 = "e7bae75c9dc3c4ac6b826289f39a50e26dc9c41ca80df4b272395a3e7362c162"
WAVE2 = ["ins_opp", "inst_best_ideas", "ftd_fail", "ea_overdue"]
BASE = {"close", "raw_close", "volume"}
PLAN = Path(os.environ.get("ATX_V71_PLAN_JSON") or HERE / "alphas" / "fixtures" / "v71_plan_k1.json")


def v71_recipe_v1() -> dict:
    return json.loads((HERE / "fund_industry_ic_v71.recipe.json").read_text(encoding="utf-8"))


def tree(tmp_path: Path) -> Path:
    """A copy of the registry, the library definitions and the legacy parent library in a temporary strategies dir."""
    root = tmp_path / "strategies"
    shutil.copytree(HERE / "alphas", root / "alphas")
    shutil.copytree(HERE / "libraries", root / "libraries")
    for name in ("fund_industry_ic_v70.json", "fund_industry_ic_v71.json", "fund_industry_ic_v71.recipe.v2.json"):
        shutil.copyfile(HERE / name, root / name)
    return root


def edit_registry(root: Path, change) -> None:
    path = root / G.REGISTRY_PATH
    reg = json.loads(path.read_text(encoding="utf-8"))
    change(reg)
    path.write_bytes(G.encode_data(reg))


# ------------------------------------------------------------------ the brief's acceptance tests
def test_v71_library_byte_identical():
    docs = G.documents(HERE, "v71")
    blob = docs["fund_industry_ic_v71.json"]
    assert hashlib.sha256(blob).hexdigest() == V71_SHA256
    assert blob == (HERE / "fund_industry_ic_v71.json").read_bytes()          # the committed IC library, byte for byte
    assert docs["fund_industry_ic_v71.recipe.v2.json"] == (HERE / "fund_industry_ic_v71.recipe.v2.json").read_bytes()
    assert G.main(["--library", "v71", "--check"]) == 0
    assert G.main(["--library", "v71", "--check", "--plan-json", str(PLAN)]) == 0


def test_plan_rows_equal_static_validation():
    """Ruling E-19 (K1, the exe's --plan-only, is the checker of record; R2-f). Member by member, the K1 rows of the 48
    v7.1 members equal the committed recipe's lineage in DSL SHA, prior bars (required lookback) and extra fields; the
    library maxima (slots, nodes, prior bars) equal the recipe's static-validation maxima; each member's slot and node
    figures are only required to be within the house budget (slots: the registry's max_slots or the member's recorded
    exception; nodes: the registry sets no node budget, so positive and at most the recorded library maximum); and
    generate_library.validate_plan accepts the plan. The recipe's per-member slot and node figures (the Python
    checker's estimates) stay as committed: the v7.1 recipe must regenerate byte for byte."""
    plan = json.loads(PLAN.read_text(encoding="utf-8"))
    rows = G.plan_rows(plan)
    rec = v71_recipe_v1()
    sv = rec["static_validation"]
    assert len(rows) == len(rec["lineage"]) == 48
    for ln in rec["lineage"]:
        r = rows[ln["id"]]
        assert r["dsl_sha256"] == ln["dsl_sha256"], ln["id"]
        assert r["required_lookback"] == ln["prior_bars"], ln["id"]
        assert sorted(r["extra_fields"]) == sorted(set(ln["fields"]) - BASE), ln["id"]
    assert max(r["num_slots"] for r in rows.values()) == sv["max_estimated_peak_slots"]
    assert max(r["node_count"] for r in rows.values()) == sv["max_dag_nodes"]
    assert max(r["required_lookback"] for r in rows.values()) == sv["max_prior_bars"]
    reg, lib = G.load_registry(HERE), G.load_library(HERE, "v71")
    house, exceptions = reg["house_budget"], {e["id"]: e for e in lib["budget_exceptions"]}
    for cid, r in rows.items():                                  # E-19: within the house budget, not equal
        assert r["num_slots"] <= exceptions.get(cid, {}).get("max_slots", house["max_slots"]), cid
        assert 0 < r["node_count"] <= sv["max_dag_nodes"], cid
    blob = (HERE / "fund_industry_ic_v71.json").read_bytes()
    assert G.validate_plan(reg, lib, json.loads(blob), blob, plan) == []


def test_unknown_theme_refused(tmp_path):
    root = tree(tmp_path)
    edit_registry(root, lambda reg: reg["alphas"][-1].update(theme="event_driven"))
    with pytest.raises(G.LibraryError, match="unknown theme 'event_driven'"):
        G.load_registry(root)
    assert G.main(["--library", "v71", "--check", "--strategies", str(root)]) == 2
    root2 = tree(tmp_path / "b")
    edit_registry(root2, lambda reg: reg["themes"].pop("ownership_flow"))   # a used theme dropped from the table
    with pytest.raises(G.LibraryError, match="unknown theme 'ownership_flow'"):
        G.load_registry(root2)


def test_origin_required(tmp_path):
    root = tree(tmp_path)
    edit_registry(root, lambda reg: reg["alphas"][0].pop("origin"))
    with pytest.raises(G.LibraryError, match="missing \\['origin'\\]"):
        G.load_registry(root)
    for bad in ("guess", None, "Prior"):
        edit_registry(root, lambda reg: reg["alphas"][0].update(origin=bad))
        with pytest.raises(G.LibraryError, match="origin must be one of prior, grid, mined"):
            G.load_registry(root)
    for good in G.ORIGINS:
        edit_registry(root, lambda reg: reg["alphas"][0].update(origin=good))
        assert G.load_registry(root)["alphas"][0]["origin"] == good


# ------------------------------------------------------------------ pinning the behaviour
def test_registry_seed_is_the_v71_library():
    """The registry was seeded from fund_industry_ic_v71.json and its recipe: 48 entries of origin prior, each carrying
    the recipe's prior-sign source, smoothing form and template notes; added_in = the first library holding that exact
    DSL; the field table is the library's declarations with the recipe's clock and origin."""
    reg = G.load_registry(HERE)
    rec = v71_recipe_v1()
    lib = json.loads((HERE / "fund_industry_ic_v71.json").read_text(encoding="utf-8"))
    assert [a["id"] for a in reg["alphas"]] == [c["id"] for c in lib["candidates"]]
    assert {a["origin"] for a in reg["alphas"]} == {"prior"}
    assert reg["tier_scores"] == {"A": 1.0, "A-": 0.9, "B+": 0.8, "B": 0.7, "B-": 0.55, "C+": 0.4}
    lineage, templates = ({r["id"]: r for r in rec[k]} for k in ("lineage", "templates"))
    for a in reg["alphas"]:
        assert a["prior_sign_source"] == lineage[a["id"]]["prior_sign_source"] and \
            a["form"] == lineage[a["id"]]["smoothing_form"], a["id"]
        assert a["notes"] == {k: templates[a["id"]][k] for k in G.NOTE_KEYS}, a["id"]
        assert G.tier_rank(reg, a["tier"]) == lineage[a["id"]]["tier_rank"], a["id"]
    added = {a["id"]: a["added_in"] for a in reg["alphas"]}
    assert [added[i] for i in WAVE2] == ["v71"] * 4 and added["sv_flow"] == "v61" and added["qmj_safety"] == "v70"
    assert added["within_ind_mom"] == "v4" and added["bm"] == "v6"          # bm's DSL was revised in v6
    assert list(reg["fields"]) == [f["name"] for f in lib["fields"]]
    assert {n: f["clock"] for n, f in reg["fields"].items()} == rec["data"]["field_clocks"]
    assert {n: f["origin"] for n, f in reg["fields"].items()} == rec["data"]["field_origin"]
    assert list(reg["themes"]) == [f["id"] for f in lib["families"]]


def test_slim_recipe_carries_what_the_fitter_and_ledger_read():
    rec = json.loads((HERE / "fund_industry_ic_v71.recipe.v2.json").read_text(encoding="utf-8"))
    lib = json.loads((HERE / "fund_industry_ic_v71.json").read_text(encoding="utf-8"))
    assert rec["schema"] == G.RECIPE_SCHEMA and rec["library"] == {"path": "fund_industry_ic_v71.json",
                                                                    "sha256": V71_SHA256}
    assert rec["parent"] == {"name": "v70", "library": "fund_industry_ic_v70.json", "sha256": V70_SHA256}
    assert rec["generation"]["new_members"] == WAVE2 and rec["trials"]["admission_trials"] == 4
    by_id = {c["id"]: c for c in lib["candidates"]}
    for row in rec["lineage"]:   # fit_composition_weights.load_priors: theme, tier, prior_sign agree with the library
        assert {k: row[k] for k in ("theme", "tier", "prior_sign")} == {k: by_id[row["id"]][k] for k in
                                                                        ("theme", "tier", "prior_sign")}
        assert row["origin"] == "prior" and row["dsl_sha256"] == hashlib.sha256(by_id[row["id"]]["dsl"].encode()).hexdigest()
    assert len((HERE / "fund_industry_ic_v71.recipe.v2.json").read_bytes()) < len(
        (HERE / "fund_industry_ic_v71.recipe.json").read_bytes()) / 5


def test_fitter_reads_the_same_priors_from_the_slim_recipe():
    """Acceptance (c) offline: fit_composition_weights.load_priors gives the same themes, tiers and prior signs from the
    slim recipe as from the v1 recipe; only the recipe pin differs (root then compares admission.json on real data)."""
    import sys
    sys.path.insert(0, str(HERE.parents[0] / "tools"))
    try:
        import fit_composition_weights as F
    finally:
        sys.path.pop(0)
    lib = HERE / "fund_industry_ic_v71.json"
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    cands = json.loads(lib.read_text(encoding="utf-8"))["candidates"]
    old, new = (F.load_priors(lib, sha(lib), r, sha(r), cands) for r in (HERE / "fund_industry_ic_v71.recipe.json",
                                                                         HERE / "fund_industry_ic_v71.recipe.v2.json"))
    assert {k: v for k, v in old.items() if k != "recipe_sha256"} == {k: v for k, v in new.items() if k != "recipe_sha256"}
    assert new["recipe_sha256"] == sha(HERE / "fund_industry_ic_v71.recipe.v2.json")


def test_plan_problems_name_each_member(tmp_path):
    reg, lib = G.load_registry(HERE), G.load_library(HERE, "v71")
    blob = (HERE / "fund_industry_ic_v71.json").read_bytes()
    doc = json.loads(blob)
    plan = json.loads(PLAN.read_text(encoding="utf-8"))
    bad = copy.deepcopy(plan)
    rows = {r["id"]: r for r in bad["candidates"]}
    rows["bm"]["dsl_sha256"] = "0" * 64                                   # another DSL
    rows["ep"]["num_slots"] = 8                                            # over the house budget (7)
    rows["qmj_safety"]["num_slots"] = 8                                    # its recorded exception: fine
    rows["q5_eg"]["extra_fields"] = rows["q5_eg"]["extra_fields"][:6]      # its exception (6 extra fields): fine
    rows["cfp"]["required_lookback"] = 315                                 # over 314 prior bars
    rows["sp"]["extra_fields"] = rows["sp"]["extra_fields"] + ["ins_cluster_buy"]   # not a registry field
    bad["candidates"] = [r for r in bad["candidates"] if r["id"] != "ea_overdue"] + [dict(rows["bm"], id="ghost")]
    problems = G.validate_plan(reg, lib, doc, blob, bad)
    text = "\n".join(problems)
    for needle in ("bm: plan dsl_sha256", "ep: 8 slots > 7 (house budget)", "cfp: 315 prior bars > 314",
                   "sp: reads ['ins_cluster_buy'], not registry fields", "ea_overdue: no plan row",
                   "outside the library: ['ghost']"):
        assert needle in text, needle
    assert not any(p.startswith(("qmj_safety", "q5_eg")) for p in problems)
    rows["qmj_safety"]["num_slots"] = 9
    assert "qmj_safety: 9 slots > 8 (recorded exception)" in G.validate_plan(reg, lib, doc, blob, bad)
    assert G.validate_plan(reg, lib, doc, blob, dict(plan, library_sha256="f" * 64))[0].startswith(
        "plan was made for library sha256 ffff")
    with pytest.raises(G.LibraryError, match="no candidates"):
        G.plan_rows({"mode": "metadata-only-no-payload", "candidates": 48})   # the pre-B-3 plan: a count only
    path = tmp_path / "bad_plan.json"
    path.write_text(json.dumps(bad))
    assert G.main(["--library", "v71", "--check", "--plan-json", str(path)]) == 1


def test_writing_needs_a_plan_and_a_new_library_round_trips(tmp_path):
    root = tree(tmp_path)
    assert G.main(["--library", "v71", "--strategies", str(root)]) == 2    # no plan: nothing is written
    lib = json.loads((root / "libraries" / "v71.json").read_text(encoding="utf-8"))
    child = dict(lib, id="fund_industry_ic_v72", parent="v71", members=lib["members"][:-1], budget_exceptions=[
        e for e in lib["budget_exceptions"]], prereg="test")
    (root / "libraries" / "v72.json").write_bytes(G.encode_data(child))
    plan = json.loads(PLAN.read_text(encoding="utf-8"))
    plan.pop("library_sha256")
    plan["candidates"] = [r for r in plan["candidates"] if r["id"] != "ea_overdue"]
    (tmp_path / "plan.json").write_text(json.dumps(plan))
    assert G.main(["--library", "v72", "--strategies", str(root), "--plan-json", str(tmp_path / "plan.json")]) == 0
    out = json.loads((root / "fund_industry_ic_v72.json").read_text(encoding="utf-8"))
    assert [c["id"] for c in out["candidates"]] == lib["members"][:-1]
    assert "ea_days_to_expected" not in {f["name"] for f in out["fields"]}   # only the fields its members name
    assert {f["name"] for f in out["fields"]} >= BASE                          # role base fields always
    rec = json.loads((root / "fund_industry_ic_v72.recipe.v2.json").read_text(encoding="utf-8"))
    assert rec["parent"]["library"] == "fund_industry_ic_v71.json" and rec["generation"]["new_members"] == []
    assert G.main(["--library", "v72", "--strategies", str(root), "--check"]) == 0
