"""Cycle spec templates (research_spec.py) and the v8 specs under scripts/specs/v8 (platform v8 lane A2, task 3).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_spec.py

No real data: every v8 spec is planned in a temporary root holding a stand-in file for each input (the atx-db stage
paths of base-lo3 are re-pointed under that root, so nothing outside it is read); the template run test drives the fake
tools of test_research_cycle.py.
"""
from __future__ import annotations

import json
import re
import shutil
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
import research_add_alpha as RA  # noqa: E402

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


# ------------------------------------------------------------------ add-alpha on a v8 parent (A2 follow-up, task 1)
V71_IDS = json.loads((T.STRATEGIES / "libraries" / "v71.json").read_text(encoding="utf-8"))["members"]
F49 = "build-equity/train-2020-2023-lo1-fields-v9-f49"   # fields v9 + grp_ff12f49 (built with --reuse from v9)
SPECS = "scripts/specs/v8"


def files_of(root: Path) -> list[str]:
    return sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file())


def v8_root(tmp_path: Path) -> tuple[Path, Path]:
    """A root with base-lo1.json (its tools re-pointed at the fake ones of test_research_cycle.py, the IC exe at the
    fake --plan-only one), locked on stand-ins of its inputs, the committed registry and library v7.1."""
    doc = json.loads((V8 / "base-lo1.json").read_text(encoding="utf-8"))
    doc.update(python=sys.executable, exes={"ic": "bin/ic.cmd", "nav": "bin/nav.exe"})
    doc["runner"]["script"] = "scripts/runner.py"
    for section in ("fit", "card", "monitor", "summ"):
        doc[section]["script"] = f"scripts/{section}.py"
    root, _ = fake_root(tmp_path, doc)
    s = root / "atx-impl" / "strategies"
    for rel in ("alphas/registry.json", "libraries/v71.json", "fund_industry_ic_v71.json",
                "fund_industry_ic_v71.recipe.json"):
        (s / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(T.STRATEGIES / rel, s / rel)
    tools = {"scripts/runner.py": T.FAKE_RUNNER, "scripts/fit.py": "", "scripts/card.py": "",
             "scripts/monitor.py": T.FAKE_MONITOR, "scripts/summ.py": T.FAKE_SUMM_JSON, "bin/fake_ic.py": T.FAKE_IC_PLAN,
             "bin/ic.cmd": f'@"{sys.executable}" "%~dp0fake_ic.py" %*\n', "bin/nav.exe": "nav"}
    for rel, text in tools.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_text(text)
    path = root / SPECS / "base-lo1.json"
    path.parent.mkdir(parents=True)
    path.write_text(json.dumps(doc, indent=2), encoding="utf-8")
    assert RC.main(["lock", str(path), "--root", str(root), "--write"]) == RC.EXIT_OK
    return root, path


def parent_cell(root: Path, spec_path: Path) -> dict:
    """The parent cell's outputs the child pins (made-up content; the pool binds the role and the parent's weights)."""
    spec = RC.load_spec(spec_path)
    outs = RA.parent_outputs(spec, root)
    files = {f"{outs['u']}/summary.json": '{"status": "complete"}', f"{outs['w']}/summary.json": '{"status": "complete"}',
             f"{outs['u']}/orientations.json": json.dumps({"candidates": [{"id": m, "sign": 1} for m in V71_IDS]}),
             f"{outs['u']}/train_daily_ic.csv": "id,h,v\n" + "".join(f"{m},21,0.01\n" for m in V71_IDS),
             f"{outs['fit']}/admission.json": json.dumps({"candidates": [{"id": m, "status": "admitted"}
                                                                         for m in V71_IDS]}),
             f"{outs['fit']}/composition_weights.json": '{"weights": {}}',
             f"{outs['nav']}/summary.json": json.dumps({"status": "complete", "primary_scenario": "s2"})}
    for rel, text in files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(text.encode())                    # LF, as the fake u pass writes the child's
    (root / outs["nav"] / "daily_s2.csv").write_text("net_return\n0.001\n")   # text mode, as the fake NAV writes
    (root / outs["w"] / "train_combined.json").write_text(T.pool_manifest(root, spec["inputs"]["role"]["path"],
                                                                          f"{outs['fit']}/composition_weights.json"))
    return RA.parent_outputs(spec, root)


def add(root: Path, parent_spec: Path, cid: str, dsl: str, *extra: str) -> int:
    like = T.v71_entry("sue")
    return RC.main(["add-alpha", "--id", cid, "--dsl", dsl, "--theme", like["theme"], "--tier", like["tier"],
                    "--prior-sign", "1", "--citation", f"test citation for {cid}", "--origin", "prior", "--parent", "v71",
                    "--name", "v80", "--parent-spec", str(parent_spec), "--root", str(root), *extra])


def child_ic_files(root: Path, spec: dict, admitted: list[str]) -> None:
    """The fake u pass writes the child's member rows (the parent's rows of its members, byte for byte), the fake fit
    admits `admitted`."""
    members = [c["id"] for c in json.loads((root / spec["inputs"]["library"]["path"]).read_text())["candidates"]]
    T.behave(root, ic_files={Path(spec["ic"]["u_output"]).name + "-1": {
        "orientations.json": json.dumps({"candidates": [{"id": m, "sign": 1} for m in members]}),
        "train_daily_ic.csv": "id,h,v\n" + "".join(f"{m},21,0.01\n" for m in members)}},
        admission={"rules": {}, "candidates": [{"id": m, "status": "admitted", "sign_agrees": True,
                                                "failed_checks": []} for m in admitted]})


def test_derive_name_substitutes_the_parent_token_else_appends():
    assert RA.derive_name("build-equity/mega-v71w-train-ew", "v71", "v80") == "build-equity/mega-v80w-train-ew"
    assert RA.derive_name("build-equity/mega-nav-v8-b0a-lo1-v71-ew-L1.247", "v71", "v80") == \
        "build-equity/mega-nav-v8-b0a-lo1-v80-ew-L1.247"
    for name in ("build-equity/mega-v8-b0a-train-u", "x-v710", "x-dv71"):          # no token v71: appended
        assert RA.derive_name(name, "v71", "v80") == f"{name}-v80"
    assert RA.shared_stores({"fit": {"work_dir": "build-equity/fit-work"}})
    assert RA.shared_stores({"out_root": "o/", "fit": {"work_dir": "o/fit-work"}})
    assert not RA.shared_stores({"fit": {"work_dir": "build-equity/mega-fit-work-v70"}}) and not RA.shared_stores({})


def test_add_alpha_on_a_v8_base_spec_screens_and_runs(tmp_path):
    root, parent = v8_root(tmp_path)
    outs = parent_cell(root, parent)
    base = RC.load_spec(parent)
    assert add(root, parent, "v8_probe", "rank(decay_linear((be / at_lag4), 21))") == RC.EXIT_OK
    sp = root / SPECS / "lib-v80.json"
    spec = RC.load_spec(sp)
    assert all(item["sha256"] for item in spec["inputs"].values()) and spec["fields"]["manifest_sha256"]   # locked
    assert set(spec["inputs"]) == {"library", "recipe", "baseline_library", "role", "baseline_fields",
                                   "reference_admission", "reference_cell", "reference_combined", "reference_weights",
                                   "reference_daily", "reference_orientations", "reference_daily_ic"}
    assert spec["inputs"]["role"]["path"] == base["inputs"]["role"]["path"]          # kept; the builders' are not
    assert spec["inputs"]["reference_weights"]["path"] == f"{outs['fit']}/composition_weights.json"
    names = {k: spec[s][k2] for k, (s, k2) in {"u": ("ic", "u_output"), "w": ("ic", "w_output"), "fit": ("fit", "output"),
                                               "card": ("card", "output"), "nav": ("nav", "output"),
                                               "monitor": ("monitor", "output")}.items()}
    assert names == {"u": "build-equity/mega-v8-b0a-train-u-v80", "w": "build-equity/mega-v8-b0aw-train-ew-v80",
                     "fit": "build-equity/mega-weights-v8-b0a-ew-v80", "card": "build-equity/mega-cards-v8-b0a-v80",
                     "nav": base["nav"]["output"].replace("-v71-", "-v80-"),
                     "monitor": "build-equity/mega-monitor-v8-b0a-v80"}              # the name rule
    assert (spec["ic"]["cache"], spec["fit"]["work_dir"]) == (base["ic"]["cache"], "build-equity/fit-work")   # C-1
    assert spec["card"]["flags"] == base["card"]["flags"] and spec["runner"] == base["runner"]
    assert spec["fields"] == dict(base["fields"], manifest_sha256=spec["fields"]["manifest_sha256"])
    assert spec["gate"] == {"name": "p1-v80", "admitted": ["v8_probe"], "require": "any", "sign_agrees": True,
                            "report": []}
    assert not any("keys_in" in c for c in spec["compare"]) and "ref" in spec     # ref skipped: the parent's fields
    assert RS.run_refusal(spec, sp, root) is None
    child_ic_files(root, spec, ["v8_probe"])
    log = []
    assert T.run(root, sp, log, screen=True, capabilities=T.CAPS) == RC.EXIT_OK, log
    ran = T.calls(root)
    assert ran[:4] == ["mega-v8-b0a-train-u-v80-run1", "mega-weights-v8-b0a-ew-v80-run1", "mega-cards-v8-b0a-v80-run",
                       "mega-v8-b0a-train-u-v80-marginal-run"], ran
    assert "== ref: skipped" in " ".join(log) and (root / spec["marginal"]["output"] / "marginal_ic.json").is_file()
    assert T.run(root, sp, log, capabilities=T.CAPS) == RC.EXIT_OK, log                # the full run continues
    assert (root / names["nav"] / "summary.json").is_file() and (root / "build-equity" / "cycle-v80" /
                                                                   "cycle_verdict.json").is_file()


def test_add_alpha_on_a_v8_template_replaces_rescreens_and_records_exceptions(tmp_path, capsys):
    root, base = v8_root(tmp_path)
    tpl = json.loads((V8 / "r4-hold-band.json").read_text(encoding="utf-8"))
    tp = root / SPECS / "r4-hold-band.json"
    tp.write_text(json.dumps(tpl), encoding="utf-8")                   # parent null: planned on a nominal parent
    s = root / "atx-impl" / "strategies"
    reg = json.loads((s / "alphas" / "registry.json").read_text(encoding="utf-8"))
    reg["fields"]["grp_ff12f49"] = dict(reg["fields"]["grp_ff49"], basis="test: the FF49 regrouping (Ruling R2-e)")
    (s / "alphas" / "registry.json").write_bytes(RA.G.encode_data(reg))
    lo1_fields = RC.load_spec(base)["fields"]["list"]
    (root / F49).mkdir(parents=True)
    (root / F49 / "manifest.json").write_text(json.dumps({"fields": [{"name": n} for n in lo1_fields + ["grp_ff12f49"]]}))
    earn = "rank(decay_linear((sue + be + at + lt + che + debt + sale_ttm), 21))"    # 7 extra fields
    replace3 = ("--replaces", "sue", "--replaces", "droe", "--replaces", "chtax")
    shutil.copyfile(V8 / "base-b0c.json", tp.parent / "base-b0c.json")               # r4's nominal parent
    before = files_of(root)
    ruling = ("--exception", "max_extra_fields=8", "--exception-basis", "Ruling R2-b")
    assert add(root, tp, "earn_probe", earn, *replace3, *ruling) == RC.EXIT_USAGE     # template parent null
    assert "template parent is null" in capsys.readouterr().err and files_of(root) == before
    tpl["parent"] = "base-lo1.json"
    tp.write_text(json.dumps(tpl), encoding="utf-8")
    outs = parent_cell(root, tp)
    before = files_of(root)
    assert add(root, tp, "earn_probe", earn, *replace3) == RC.EXIT_USAGE              # 7 > 5 extra fields: K1
    for bad in (("--replaces", "nope"), ("--rescreen",), ("--exception", "max_extra_fields=8"),
                ("--exception", "max_nodes=8", "--exception-basis", "x")):
        assert add(root, tp, "earn_probe", earn, *replace3[:2], *bad) == RC.EXIT_USAGE, bad
    assert files_of(root) == before                                                  # nothing written
    assert add(root, tp, "earn_probe", earn, *replace3, *ruling) == RC.EXIT_OK
    q5 = re.sub(r"\bgrp_ff12\b", "grp_ff12f49", T.v71_entry("q5_eg")["dsl"])
    gpa = re.sub(r"\bgrp_ff12\b", "grp_ff12f49", T.v71_entry("gpa")["dsl"])
    assert add(root, tp, "q5_eg_f49", q5, "--replaces", "q5_eg", "--rescreen", "--fields", F49) == RC.EXIT_OK
    assert add(root, tp, "gpa_f49", gpa, "--replaces", "gpa", "--rescreen") == RC.EXIT_OK   # --fields is sticky
    assert add(root, tp, "gpa_f49", gpa, "--replaces", "gpa", "--rescreen") == RC.EXIT_OK   # identical: no-op
    lib = json.loads((s / "libraries" / "v80.json").read_text(encoding="utf-8"))
    want = [{"sue": "earn_probe", "q5_eg": "q5_eg_f49", "gpa": "gpa_f49"}.get(m, m) for m in V71_IDS
            if m not in ("droe", "chtax")]
    assert lib["members"] == want and lib["rescreens"] == ["q5_eg_f49", "gpa_f49"]    # in place, in roster order
    ex = {e["id"]: e for e in lib["budget_exceptions"]}
    assert set(ex) == {"qmj_safety", "q5_eg_f49", "earn_probe"} and ex["q5_eg_f49"]["max_extra_fields"] == 6
    assert ex["q5_eg_f49"]["basis"].startswith("inherited from q5_eg (replaced in place): q5_eg: prereg ruling")
    assert (ex["earn_probe"]["max_extra_fields"], ex["earn_probe"]["basis"]) == (8, "Ruling R2-b")
    rec = json.loads((s / "fund_industry_ic_v80.recipe.v2.json").read_text(encoding="utf-8"))
    assert rec["trials"]["admission_trials"] == 1 and rec["trials"]["rescreens"] == ["q5_eg_f49", "gpa_f49"]
    assert rec["trials"]["removed_parent_members"] == ["chtax", "droe", "gpa", "q5_eg", "sue"]
    stub = (s / "libraries" / "v80.prereg.md").read_text(encoding="utf-8")
    assert "| `gpa_f49` |" in stub and "re-screen (0 admission trials)" in stub and "1 admission trial(s) and 2" in stub
    sp = root / SPECS / "lib-v80.json"
    spec = RC.load_spec(sp)
    assert spec["gate"] == {"name": "p1-v80", "admitted": ["earn_probe"], "require": "any", "sign_agrees": True,
                            "report": ["q5_eg_f49", "gpa_f49"]}                     # 1 trial; the re-screens reported
    assert spec["fields"]["output"] == F49 and spec["fields"]["list"] == lo1_fields + ["grp_ff12f49"]
    assert spec["inputs"]["baseline_fields"]["dir"] == outs["fields"] != F49          # the ref identity's baseline
    assert spec["nav"]["output"].endswith("-v80") and "--hold-band" in spec["nav"]["flags"]   # the template's cell
    assert spec["inputs"]["reference_cell"]["dir"] == outs["nav"] == RC.load_spec(tp)["nav"]["output"]
    assert [c.get("keys_in") for c in spec["compare"]] == [None, "{input:library}", "{input:library}"]
    child_ic_files(root, spec, ["earn_probe", "q5_eg_f49", "gpa_f49"])
    log = []
    assert T.run(root, sp, log, screen=True, capabilities=T.CAPS) == RC.EXIT_OK, log
    same = lambda: sum(x.startswith("   IDENTICAL: ") for x in log)                     # noqa: E731
    assert "== ref: skipped (screen: runs with the full `run`)" in log and same() == 2   # the kept members' rows
    assert any(x.startswith("  q5_eg_f49: status") for x in log), log                 # a re-screen: reported
    n = len(T.calls(root))
    assert T.run(root, sp, log, capabilities=T.CAPS) == RC.EXIT_OK, log
    assert T.calls(root)[n].endswith("-v80-ref-run") and same() == 2 + 3              # new fields: ref runs
    check = dict(spec["compare"][1], a=spec["inputs"]["reference_orientations"]["path"],
                 b=f"{spec['ic']['u_output']}-1/orientations.json", keys=spec["inputs"]["baseline_library"]["path"],
                 keys_in=spec["inputs"]["library"]["path"])
    RC.compare_files(RC.Resolver(root), check)                                        # the 43 members both hold
    check.pop("keys_in")                                                              # without it: the replaced rows
    with pytest.raises(RC.CycleError, match="IDENTITY MISMATCH"):
        RC.compare_files(RC.Resolver(root), check)
