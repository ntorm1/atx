"""Cycle spec templates (research_spec.py) and the v8 specs under scripts/specs/v8 (platform v8 lane A2, task 3).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_spec.py

No real data: every v8 spec is planned in a temporary root holding a stand-in file for each input (the atx-db stage
paths of base-lo3 are re-pointed under that root, so nothing outside it is read); the template run test drives the fake
tools of test_research_cycle.py.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import research_cycle as RC  # noqa: E402
import research_spec as RS  # noqa: E402
import research_tree  # noqa: E402
import test_research_cycle as T  # noqa: E402  (the fake tools and fixtures)

V8 = HERE.parent / "specs" / "v8"
V8_SPECS = sorted(p.name for p in V8.glob("*.json"))
BASE_NULLS = {"inputs.role", "inputs.identity_bridge", "inputs.fund_events", "fields.manifest_sha256"}
CHILD_NULLS = BASE_NULLS | {"inputs.reference_cell", "inputs.reference_admission"}      # derived from the parent
LIB_NULLS = CHILD_NULLS | {"inputs.library", "inputs.recipe", "inputs.reference_combined", "inputs.reference_weights"}
# every pin root fills (`lock --write` after the runbook builds and the parent cell), per spec, as planned today
NULL_PINS = {"base-lo1.json": BASE_NULLS,
             "base-lo3.json": BASE_NULLS | {"inputs.sic_events", "inputs.reference_cell"},
             "base-b0c.json": CHILD_NULLS, "r1-comp-v8.json": CHILD_NULLS, "r2-lib-v80.json": LIB_NULLS,
             "r3-aim-gain.json": CHILD_NULLS, "r4-hold-band.json": CHILD_NULLS, "r5-adv-hold.json": CHILD_NULLS,
             "r6-spo-v3.json": CHILD_NULLS, "r7-lib-v81.json": LIB_NULLS}
FILLS = {"r6-spo-v3.json": ["<fill:nav.flags --risk-model>", "<fill:nav.flags --risk-model-sha256>"]}
FIT_DOWN = {"fit.output", "card.output", "ic.w_output", "nav.output", "monitor.output"}   # downstream of the fit
LIB_DOWN = FIT_DOWN | {"ic.u_output"}                                                       # downstream of the library
LIB_CHANGE = LIB_DOWN | {"inputs.library.path", "inputs.library.sha256", "inputs.recipe.path", "inputs.recipe.sha256",
                         "marginal.output", "marginal.pool", "marginal.themes", "gate.name", "gate.admitted",
                         "gate.require", "gate.sign_agrees", "gate.report"}
# a template renames exactly the outputs downstream of its change, and changes nothing else
EXPECTED_CHANGES = {"base-b0c.json": {"nav.output", "nav.flags"}, "r1-comp-v8.json": FIT_DOWN | {"fit.flags"},
                    "r2-lib-v80.json": LIB_CHANGE, "r3-aim-gain.json": FIT_DOWN | {"fit.flags"},
                    "r4-hold-band.json": {"nav.output", "nav.flags"}, "r5-adv-hold.json": {"nav.output", "nav.flags"},
                    "r6-spo-v3.json": {"nav.output", "nav.flags", "nav.rule"}, "r7-lib-v81.json": LIB_CHANGE}
MISSING = object()


def flat(node, pre: str = "") -> dict:
    if isinstance(node, dict):
        out = {}
        for k, v in node.items():
            out.update(flat(v, f"{pre}.{k}" if pre else k))
        return out
    return {pre: node}


def fake_root(tmp_path: Path, spec: dict) -> tuple[Path, dict]:
    """A root with a stand-in file for every input, the as-built fields manifest, the pool and an empty ledger; absolute
    input paths are re-pointed under the root (the returned spec)."""
    root = tmp_path / "root"
    spec = json.loads(json.dumps(spec))
    files = {"build-equity/trials.jsonl": ""}
    for key, item in spec["inputs"].items():
        if Path(item["path"]).is_absolute():
            item["dir"], item["path"] = f"ext/{key}", f"ext/{key}/manifest.json"
        committed = research_tree.REPO / item["path"]           # a pinned library or recipe of this worktree
        files[item["path"]] = committed.read_bytes() if item["path"].startswith("atx-impl/") and \
            committed.is_file() else "{}"
    role = spec["inputs"]["role"]
    files[role["path"]] = json.dumps({"universe": {"id": role["universe"]}, "dates": 1405, "score_begin": 399})
    f = spec["fields"]
    files[f"{f['output']}/manifest.json"] = json.dumps({"fields": [{"name": n} for n in f["list"]]})
    for rel, text in files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(text if isinstance(text, bytes) else text.encode())
    if "marginal" in spec:
        m = spec["marginal"]
        (root / spec["inputs"][m["pool"]]["path"]).write_text(T.pool_manifest(root, role["path"],
                                                                              spec["inputs"][m["themes"]]["path"]))
    return root, spec


@pytest.mark.parametrize("name", V8_SPECS)
def test_every_v8_spec_loads_and_plans(tmp_path, name):
    """Every spec loads (templates resolve on their nominal parents) and plans with the fake root; the null pins are
    reported (UNLOCKED, computed from the stand-in files), never crashed on; a template's run is refused until root
    sets its parent (and meets its requires / fills its values); a base spec has no refusal."""
    assert set(V8_SPECS) == set(NULL_PINS)
    path = V8 / name
    root, spec = fake_root(tmp_path, RC.load_spec(path))
    c = RC.Cycle(spec, RC.Resolver(root), spec_path=path, capabilities=T.CAPS)
    lines = RC.plan_lines(c)
    assert RC.plan_lines(c, lines_only=True) and not any("<sha256:" in x for x in lines if x.startswith("#"))
    unlocked = {f"inputs.{k}" for k, (_, _, how) in c.pins.items() if how.startswith("UNLOCKED")}
    unlocked |= {"fields.manifest_sha256"} if spec["fields"]["manifest_sha256"] is None else set()
    assert unlocked == NULL_PINS[name]
    assert any(x.startswith("# phase fields [pinned; done; as built") and "UNLOCKED" in x for x in lines)
    assert [x[len("# fill (root fills before `run`): "):] for x in lines if x.startswith("# fill")] == FILLS.get(name, [])
    doc = json.loads(path.read_text(encoding="utf-8"))
    refusal = RS.run_refusal(spec, path, research_tree.REPO)
    if RS.is_template(doc):
        assert doc["parent"] is None and any(x.startswith(f"# template {name}: parent null") for x in lines)
        assert refusal.startswith("run refused: template parent is null")
        with pytest.raises(RC.CycleError) as e:
            RC.run_cycle(c, clean=lambda r: True, log=lambda s: None)
        assert e.value.code == RC.EXIT_PIN and not (root / "calls.log").exists()
    else:
        assert refusal is None and not any(x.startswith("# template") for x in lines)
    assert all(st.kind != "skipped" or st.phase == "marginal" for st in c.steps())


@pytest.mark.parametrize("name", [n for n in V8_SPECS if n in EXPECTED_CHANGES])
def test_templates_differ_from_the_parent_only_by_the_registered_change(name):
    doc = json.loads((V8 / name).read_text(encoding="utf-8"))
    child = RC.load_spec(V8 / name)
    parent = RC.load_spec(V8 / doc["nominal_parent"])
    a, b = flat(parent), flat(child)
    changed = {k for k in set(a) | set(b) if a.get(k, MISSING) != b.get(k, MISSING)
               and k not in ("name", "description") and not k.startswith("inputs.reference_")}
    assert changed == EXPECTED_CHANGES[name]
    refs = {k: v for k, v in child["inputs"].items() if k.startswith("reference_")}     # the paired reference: parent
    want = {"reference_cell": {"dir": parent["nav"]["output"], "path": f"{parent['nav']['output']}/summary.json",
                               "sha256": None},
            "reference_admission": {"path": f"{parent['fit']['output']}/admission.json", "sha256": None}}
    if "marginal" in child:
        want.update(reference_combined={"path": f"{parent['ic']['w_output']}-1/train_combined.json", "sha256": None},
                    reference_weights={"path": f"{parent['fit']['output']}/composition_weights.json", "sha256": None})
    assert refs == want
    pn, cn = parent["nav"]["flags"], child["nav"]["flags"]
    nav_delta = {"base-b0c.json": pn + ["--warm-start-sessions", "60", "--capacity-curve"],
                 "r4-hold-band.json": pn + ["--hold-band", ".1"], "r5-adv-hold.json": pn + ["--adv-hold-q", ".1"],
                 "r6-spo-v3.json": [x for x in pn if x != "--capacity-curve"] + [
                     "--spo-alpha", "implied-aim", "--risk-model", "<fill:nav.flags --risk-model>", "--risk-model-sha256",
                     "<fill:nav.flags --risk-model-sha256>", "--spo-books", "primary"]}
    assert cn == nav_delta.get(name, pn)
    comp = {"r1-comp-v8.json": "ew-theme-std-v1", "r3-aim-gain.json": "ew-theme-aim-v1"}
    assert child["fit"]["flags"] == [comp.get(name, x) if x == "ew-theme-v1" else x for x in parent["fit"]["flags"]]
    assert (child["nav"]["rule"] == "spo-v3") == (name == "r6-spo-v3.json")


def test_v8_base_specs_carry_the_ruled_settings():
    lo1, lo3 = RC.load_spec(V8 / "base-lo1.json"), RC.load_spec(V8 / "base-lo3.json")
    for s in (lo1, lo3):
        assert s["summ"]["dsr_n"] == "ledger+1" and s["summ"]["cells_from_ledger"] is True
        assert s["runner"]["phases"] == {"u": {"seconds": 300, "max_rss_mib": 2560},
                                         "w": {"seconds": 300, "max_rss_mib": 2560}}      # IC phases (OD-2)
        assert s["fit"]["work_dir"] == "build-equity/fit-work"                            # C-1's store base
        assert RC.option_value(s["card"]["flags"], "--work-dir") == "build-equity/fit-work"
        assert s["fields"]["list"] == RC.load_spec(HERE.parent / "specs" / "v71.json")["fields"]["list"]
        assert s["summ"]["script"] == "atx-impl/tools/nav_summ.py" and "--protocol" in s["summ"]["extra"]
    assert lo3["inputs"]["reference_cell"]["dir"] == lo1["nav"]["output"]                 # B0b is paired with B0a
    assert (lo1["inputs"]["role"]["universe"], lo3["inputs"]["role"]["universe"]) == ("linked-operating-v1",
                                                                                      "linked-operating-v3")
    b0c = json.loads((V8 / "base-b0c.json").read_text(encoding="utf-8"))
    assert b0c["change"]["flags"]["nav"]["--warm-start-sessions"] == "60" and b0c["requires"]   # E-10, D-0


# ------------------------------------------------------------------ the template mechanism on the fake tools
def child_template(tmp_path: Path, **over) -> Path:
    doc = {"schema": RS.TEMPLATE_SCHEMA, "name": "child", "description": "a NAV-only change", "parent": None,
           "nominal_parent": "spec.json",
           "change": {"set": {"nav.output": "out/N2"}, "flags": {"nav": {"--hold-band": ".1"}},
                      "inputs": {"sic_events": {"dir": "role", "path": "role/manifest.json", "sha256": None}}}}
    doc.update(over)
    path = tmp_path / "child.json"
    path.write_text(json.dumps(doc), encoding="utf-8")
    return path


def test_template_runs_only_its_change_after_root_sets_the_parent(tmp_path):
    root, sp = T.make_root(tmp_path)
    assert T.run(root, sp) == RC.EXIT_OK                                   # the parent cell
    n = len(T.calls(root))
    cp = child_template(tmp_path)
    with pytest.raises(RC.CycleError, match="template parent is null") as e:
        T.run(root, cp)
    assert e.value.code == RC.EXIT_PIN and len(T.calls(root)) == n
    for over, needle in (({"requires": ["the NAV option X"]}, "child.json requires: the NAV option X"),
                         ({"change": {"set": {"nav.output": "out/N2"}, "flags": {"nav": {"--x": None}}}},
                          "unfilled value <fill:nav.flags --x>")):
        with pytest.raises(RC.CycleError, match=needle):
            T.run(root, child_template(tmp_path, parent="spec.json", **over))
    cp = child_template(tmp_path, parent="spec.json")
    log = []
    assert T.run(root, cp, log) == RC.EXIT_OK
    # fields, u, fit, w are the parent's (done); static_check and the fields baseline were the parent's identity
    # checks (dropped); only the NAV cell and the always-run summ execute, paired with the parent's cell
    assert T.calls(root)[n:] == ["N2-run", "summ --weights out/W/composition_weights.json --reference out/N "
                                           "--dsr-n 29 out/N2"]
    assert "== u: done (out/U-1)" in log and "== w: done (out/WT-1)" in log
    nav = next(s for s in RC.Cycle(RC.load_spec(cp), RC.Resolver(root)).steps() if s.phase == "nav")
    assert nav.argv[-2:] == ["--hold-band", ".1"] and nav.argv[nav.argv.index("--combined") + 1] == "out/WT-1/train_combined.json"


def test_template_lock_pins_what_it_adds_and_derives(tmp_path):
    root, sp = T.make_root(tmp_path)
    assert T.run(root, sp) == RC.EXIT_OK
    cp = child_template(tmp_path, parent="spec.json")
    assert RC.main(["lock", str(cp), "--root", str(root), "--write"]) == RC.EXIT_OK
    doc = json.loads(cp.read_text(encoding="utf-8"))
    assert doc["change"]["inputs"]["sic_events"]["sha256"] == RC.sha256_file(root / "role" / "manifest.json")
    assert doc["locked"] == {"reference_cell": {"path": "out/N/summary.json",
                                                "sha256": RC.sha256_file(root / "out" / "N" / "summary.json")},
                             "reference_admission": {"path": "out/W/admission.json",
                                                     "sha256": RC.sha256_file(root / "out" / "W" / "admission.json")}}
    assert "library" not in doc["change"]["inputs"]                        # the parent's pins stay in its file
    c = RC.Cycle(RC.load_spec(cp), RC.Resolver(root))
    assert {k: how for k, (_, _, how) in c.pins.items() if k in ("reference_cell", "sic_events")} == {
        "reference_cell": "locked, verified", "sic_events": "locked, verified"}
    (root / "out" / "N" / "summary.json").write_text('{"status": "complete", "changed": 1}')
    with pytest.raises(RC.CycleError, match="PIN MISMATCH reference_cell"):
        RC.Cycle(RC.load_spec(cp), RC.Resolver(root))
    assert RC.main(["lock", str(cp), "--root", str(root)]) == RC.EXIT_PIN          # never silently relocked
    assert RC.main(["lock", str(cp), "--root", str(root), "--relock", "--write"]) == RC.EXIT_OK


def test_template_refusals(tmp_path):
    root, sp = T.make_root(tmp_path)
    for over, needle in (({"parent": "child.json"}, "cycle"),
                         ({"change": {"set": {}}}, "nav.output must be a new cell"),
                         ({"extra": 1}, "unknown keys"),
                         ({"change": {"set": {"nav.output": "N2"}, "flags": {"nope": {"--x": "1"}}}}, "no nope section"),
                         ({"change": {"set": {"nav.output": "N2"}, "inputs": {"role": {"path": "r"}}}}, "path and sha256"),
                         ({"parent": None, "nominal_parent": None}, "must name a spec file"),
                         ({"parent": "nowhere.json"}, "not found")):
        with pytest.raises(RC.CycleError, match=needle) as e:
            RC.load_spec(child_template(tmp_path, **over))
        assert e.value.code == RC.EXIT_USAGE


def test_apply_flags_operations():
    base = ["--a", "1", "--flag", "--b", "x"]
    assert RS.apply_flags(base, {"--a": "2"}, "s") == ["--a", "2", "--flag", "--b", "x"]            # value replaced
    assert RS.apply_flags(base, {"--c": "3"}, "s") == base + ["--c", "3"]                            # appended
    assert RS.apply_flags(base, {"--flag": "v"}, "s") == ["--a", "1", "--flag", "v", "--b", "x"]      # value given
    assert RS.apply_flags(base, {"--a": False, "--flag": False}, "s") == ["--b", "x"]                # removed
    assert RS.apply_flags(base, {"--flag": True, "--new": True}, "s") == base + ["--new"]            # bare, idempotent
    assert RS.apply_flags(base, {"--z": None}, "s") == base + ["--z", "<fill:s --z>"]                # root fills
    with pytest.raises(RS.TemplateError, match="takes a value"):
        RS.apply_flags(base, {"--a": True}, "s")
    assert RS.fills({"x": ["<fill:s --z>", "y"], "z": {"w": "<fill:t>"}}) == ["<fill:s --z>", "<fill:t>"]


def test_null_fields_pin_plans_unlocked_and_lock_fills_it(tmp_path):
    root, sp = T.make_root(tmp_path)
    spec = json.loads(sp.read_text())
    spec.pop("static_check")
    spec["fields"] = {"output": "fields-base", "manifest_sha256": None, "list": ["fa"]}
    sp.write_text(json.dumps(spec), encoding="utf-8")
    st = next(s for s in T.cycle_of(root, sp).steps() if s.phase == "fields")
    got = RC.sha256_file(root / "fields-base" / "manifest.json")
    assert (st.kind, st.state, st.output) == ("pinned", "done", "fields-base") and f"{got} (UNLOCKED" in st.note
    assert RC.main(["lock", str(sp), "--root", str(root), "--write"]) == RC.EXIT_OK
    assert json.loads(sp.read_text())["fields"]["manifest_sha256"] == got
    assert "pinned" in next(s for s in T.cycle_of(root, sp).steps() if s.phase == "fields").note
    assert T.run(root, sp) == RC.EXIT_OK                                   # the parent cell
    cp = child_template(tmp_path, parent="spec.json", change={"set": {      # a template setting new as-built fields
        "nav.output": "out/N2", "fields": {"output": "out/F2", "manifest_sha256": None, "list": ["fa"]}}})
    (root / "out" / "F2").mkdir(parents=True)
    (root / "out" / "F2" / "manifest.json").write_text(json.dumps({"fields": [{"name": "fa"}]}))
    assert RC.main(["lock", str(cp), "--root", str(root), "--write"]) == RC.EXIT_OK
    assert json.loads(cp.read_text())["change"]["set"]["fields"]["manifest_sha256"] == RC.sha256_file(
        root / "out" / "F2" / "manifest.json")
    for bad in ("abc", 7):
        with pytest.raises(RC.CycleError, match="SHA-256 hex digest"):
            RC.validate_spec(dict(spec, fields=dict(spec["fields"], manifest_sha256=bad)))
