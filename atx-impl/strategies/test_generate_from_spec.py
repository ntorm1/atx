"""generate_from_spec.py (platform core migration slice 2, Ruling PM8-12): one spec-driven generator replaces the
generate_fund_ic_v*.py lineage.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/strategies/test_generate_from_spec.py

The committed spec specs/library-v71.json must reproduce v7.1's IC library (the bytes generate_fund_ic_v71.py wrote)
and its slim recipe, and verify every class-C artefact as frozen. On a synthetic strategies tree (a three-alpha
registry, a legacy parent, a K1 plan, and a fake IC exe that prints it), the spec's artefacts must equal byte for byte
what the existing generate_library.py writes, at the pinned SHA-256s below.
"""
from __future__ import annotations

import hashlib
import json
import shutil
import sys
from pathlib import Path

import generate_from_spec as S
import generate_library as G

HERE = Path(__file__).resolve().parent
V71_SPEC = HERE / "specs" / "library-v71.json"
V71_LIBRARY = "fund_industry_ic_v71.json"
V71_RECIPE = "fund_industry_ic_v71.recipe.v2.json"
SYN_LIBRARY = "fund_industry_ic_syn1.json"
SYN_RECIPE = "fund_industry_ic_syn1.recipe.v2.json"
SYN_LIBRARY_SHA256 = "d7252f3d9eed6e032fc697dc24f4de20715818a50a04a1b1d2865465e488d330"   # 1,567 bytes
SYN_RECIPE_SHA256 = "818269808fa6819c07f61c23e0bd85ae193877b57151289ea22ddbc88dfbbe55"    # 2,793 bytes


def sha(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


# ------------------------------------------------------------------ the committed v7.1 spec
def test_v71_spec_reproduces_v71(tmp_path, capsys):
    assert S.main(["--spec", str(V71_SPEC), "--check"]) == 0
    assert S.main(["--spec", "specs/library-v71.json", "--check"]) == 0          # resolved under the strategies dir
    out = tmp_path / "out"
    assert S.main(["--spec", str(V71_SPEC), "--out", str(out)]) == 0
    for name in (V71_LIBRARY, V71_RECIPE):
        assert (out / name).read_bytes() == (HERE / name).read_bytes(), name
    rec = json.loads((out / "receipt.json").read_text(encoding="utf-8"))
    spec = json.loads(V71_SPEC.read_text(encoding="utf-8"))
    assert rec["outputs"][V71_LIBRARY]["sha256"] == spec["outputs"][V71_LIBRARY] == \
        "787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259"
    assert rec["frozen_verified"] == sorted(spec["frozen"]) and rec["plan"]["rows"] == 48
    assert "48 K1 rows within budget" in capsys.readouterr().out


def test_v71_spec_freezes_the_whole_lineage():
    """Every library and legacy recipe the class-C generators wrote is either generated or frozen by the spec, so the
    generators can be deleted after root's identity run without losing a checked byte."""
    spec = json.loads(V71_SPEC.read_text(encoding="utf-8"))
    legacy = {p.name for p in HERE.glob("fund_industry_ic_v[4-7]*.json")} | {
        f"{n}{s}" for n in ("price_volume_ic96_v2", "pv_fields_ic121_v3", "slow_price_volume_ic48_v1")
        for s in (".json", ".recipe.json")}
    assert legacy <= set(spec["frozen"]) | set(spec["outputs"])
    assert len(spec["replaces"]) == 11 and all(name.endswith(".py") for name in spec["replaces"])
    assert spec["registry_pins"]["house_budget"] == \
        json.loads((HERE / V71_RECIPE).read_text(encoding="utf-8"))["generation"]["house_budget"]


# ------------------------------------------------------------------ a synthetic strategies tree
Alpha = tuple[str, str, str, str, int, list[str]]          # id, dsl, theme, tier, K1 lookback, K1 extra fields
ALPHAS: tuple[Alpha, ...] = (("a_value", "rank(f_book / close)", "value", "A", 0, ["f_book"]),
                             ("a_mom", "rank(ts_mean(close, 21))", "momentum", "B", 21, []),
                             ("a_vol", "rank(-ts_mean(volume, 63))", "liquidity", "B", 63, []))


def field(origin: str, basis: str) -> dict:
    return dict(formula_id=f"{origin}-v1", origin=origin, producer="synthetic", clock="close-lag1", basis=basis)


def synthetic_tree(tmp_path: Path, max_roster: int = 10) -> Path:
    root = tmp_path / "strategies"
    (root / "alphas").mkdir(parents=True)
    (root / "libraries").mkdir()
    reg = dict(schema=G.REGISTRY_SCHEMA,
               alphas=[dict(id=i, dsl=d, theme=t, tier=r, prior_sign=1, citation="Synthetic (2026)",
                            prior_sign_source="synthetic", form="R(x)", origin="prior",
                            notes=dict(formula=i, domain=None, deviation=None), added_in="syn1")
                       for i, d, t, r, _, _ in ALPHAS],
               fields={"close": field("base", "role close"), "raw_close": field("base", "role raw close"),
                       "volume": field("base", "role volume"), "f_book": field("fields-syn", "synthetic book value")},
               themes={"value": "cheap", "momentum": "trend", "liquidity": "thin"}, tier_scores={"A": 3, "B": 2},
               house_budget=dict(max_extra_fields=2, max_slots=4, max_prior_bars=126, max_dsl_bytes=256,
                                 max_roster=max_roster),
               candidate_defaults=dict(horizons=[5, 21], sign_policy="train-rank-ic21"))
    (root / G.REGISTRY_PATH).write_bytes(G.encode_data(reg))
    (root / "libraries" / "syn1.json").write_bytes(G.encode_data(dict(
        id="fund_industry_ic_syn1", parent="syn0", members=[a[0] for a in ALPHAS], budget_exceptions=[],
        prereg="synthetic fixture (no pre-registration)")))
    (root / "fund_industry_ic_syn0.json").write_bytes(G.encode(dict(
        schema=G.LIBRARY_SCHEMA, id="fund_industry_ic_syn0", candidates=[dict(id="a_value")])))
    plan = dict(mode="metadata-only-no-payload", candidates=[
        dict(id=i, dsl_sha256=sha(d.encode()), num_slots=2, required_lookback=lb, extra_fields=x, node_count=3)
        for i, d, _, _, lb, x in ALPHAS])
    (root / "plan.json").write_bytes(G.encode(plan))
    return root


def write_spec(root: Path, **change) -> Path:
    spec = dict(schema=S.SPEC_SCHEMA, library="syn1", registry_pins=dict(house_budget=dict(max_roster=3)),
                outputs={SYN_LIBRARY: SYN_LIBRARY_SHA256, SYN_RECIPE: SYN_RECIPE_SHA256},
                frozen={"fund_industry_ic_syn0.json": sha((root / "fund_industry_ic_syn0.json").read_bytes())},
                plan=dict(json="plan.json"))
    spec.update(change)
    path = root / "spec.json"
    path.write_bytes(G.encode(spec))
    return path


def test_synthetic_artefacts_equal_generate_library(tmp_path):
    root = synthetic_tree(tmp_path / "a")
    out = tmp_path / "out"
    assert S.main(["--spec", str(write_spec(root)), "--out", str(out), "--strategies", str(root)]) == 0
    # The existing path: generate_library.py writing next to itself, on a registry whose house budget is the pin.
    legacy = synthetic_tree(tmp_path / "b", max_roster=3)
    assert G.main(["--library", "syn1", "--plan-json", str(legacy / "plan.json"), "--strategies", str(legacy)]) == 0
    for name, pin in ((SYN_LIBRARY, SYN_LIBRARY_SHA256), (SYN_RECIPE, SYN_RECIPE_SHA256)):
        blob = (out / name).read_bytes()
        assert blob == (legacy / name).read_bytes() and sha(blob) == pin, name
    # The pin, not the registry file, decides the recorded house budget: unpinned, the recipe moves.
    assert G.documents(root, "syn1")[SYN_RECIPE] != (out / SYN_RECIPE).read_bytes()
    assert G.documents(root, "syn1")[SYN_LIBRARY] == (out / SYN_LIBRARY).read_bytes()
    # --check against committed copies of the same bytes.
    for name in (SYN_LIBRARY, SYN_RECIPE):
        shutil.copyfile(out / name, root / name)
    assert S.main(["--spec", str(write_spec(root)), "--check", "--strategies", str(root)]) == 0


def test_plan_from_the_ic_exe(tmp_path):
    """The K1 rows come from the IC executable run on the generated library (here a stand-in that checks its argv and
    the library SHA-256 it is given, then prints the saved rows)."""
    root = synthetic_tree(tmp_path)
    fake = tmp_path / "fake_ic.py"
    fake.write_text(
        "import hashlib, json, sys\n"
        "a = sys.argv[1:]\n"
        "assert a[0] == '--plan-only', a\n"
        "lib = open(a[a.index('--library') + 1], 'rb').read()\n"
        "assert hashlib.sha256(lib).hexdigest() == a[a.index('--library-sha256') + 1]\n"
        "assert a[a.index('--max-memory-mib') + 1] == '1536'\n"
        f"plan = json.load(open(r'{root / 'plan.json'}'))\n"
        "plan['library_sha256'] = hashlib.sha256(lib).hexdigest()\n"
        "print(json.dumps(plan))\n", encoding="utf-8")
    exe = tmp_path / "fake_ic.cmd"
    exe.write_text(f'@"{sys.executable}" "{fake}" %*\n', encoding="utf-8")
    plan = dict(exe=dict(exe=str(exe), train="role/manifest.json", train_sha256="0" * 64, train_fields="fields",
                         train_fields_sha256="1" * 64, max_memory_mib=1536))
    out = tmp_path / "out"
    assert S.main(["--spec", str(write_spec(root, plan=plan)), "--out", str(out), "--strategies", str(root)]) == 0
    assert json.loads((out / "receipt.json").read_text(encoding="utf-8"))["plan"] == dict(
        source=f"{exe} --plan-only", rows=3)


def test_refusals(tmp_path, capsys):
    root = synthetic_tree(tmp_path)
    run = lambda *extra, **change: S.main(["--spec", str(write_spec(root, **change)), "--strategies", str(root),
                                           *(extra or ("--out", str(tmp_path / "out")))])
    assert run(outputs={SYN_LIBRARY: "0" * 64, SYN_RECIPE: SYN_RECIPE_SHA256}) == 1
    assert "the spec pins 0000" in capsys.readouterr().err
    assert run(frozen={"fund_industry_ic_syn0.json": "1" * 64}) == 1
    assert run(outputs={SYN_LIBRARY: None, SYN_RECIPE: None}) == 0               # a new version: written, unpinned
    assert run("--check", outputs={SYN_LIBRARY: None, SYN_RECIPE: None}) == 1    # --check needs every pin
    assert run(outputs={SYN_LIBRARY: SYN_LIBRARY_SHA256}) == 1                   # an unnamed artefact
    plan = json.loads((root / "plan.json").read_text(encoding="utf-8"))
    plan["candidates"][1]["dsl_sha256"] = "2" * 64
    (root / "bad_plan.json").write_bytes(G.encode(plan))
    assert run(plan=dict(json="bad_plan.json")) == 1
    assert "a_mom: plan dsl_sha256" in capsys.readouterr().err
    assert run(registry_pins=dict(house_budget=dict(max_roster=2))) == 2          # 3 members over the pinned cap
    assert run(registry_pins=dict(roster=3)) == 2
    assert run(frozen={SYN_LIBRARY: "3" * 64}) == 2                              # both generated and frozen
    assert run(plan=dict(json="plan.json", exe={})) == 2
    assert run(extra_key=1) == 2
    assert S.main(["--spec", str(write_spec(root)), "--strategies", str(root)]) == 2   # neither --check nor --out
