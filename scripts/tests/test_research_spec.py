"""Cycle spec templates (research_spec.py) and the v8 specs under scripts/specs/v8 (platform v8 lane A2, task 3).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_spec.py

No real data: every v8 spec is planned in a temporary root holding a stand-in file for each input (the atx-db stage
paths of base-lo3 are re-pointed under that root, so nothing outside it is read); the template run test drives the fake
tools of test_research_cycle.py.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import inspect
import json
import os
import re
import shutil
import subprocess
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

# the v8 spec dir this file reads; ATX_TEST_V8_SPECS points it at a copy (only
# test_the_whole_file_passes_with_a_generated_spec_present sets it, for the run of this file in its subprocess)
V8 = Path(os.environ.get("ATX_TEST_V8_SPECS") or HERE.parent / "specs" / "v8")


def add_alpha_content(doc, lib: str) -> bool:
    """A plain spec as research_add_alpha.derive_spec writes it for library ``lib``: its description names add-alpha
    and the library, gated p1-<lib>, pinning the parent's library as its baseline and comparing its u pass with the
    parent's rows."""
    return isinstance(doc, dict) and not RS.is_template(doc) and \
        str(doc.get("description", "")).startswith(f"Library {lib} = ") and \
        "(research_cycle.py add-alpha; derived from the " in doc["description"] and \
        (doc.get("gate") or {}).get("name") == f"p1-{lib}" and "baseline_library" in (doc.get("inputs") or {}) and \
        [c.get("name") for c in doc.get("compare") or []][-2:] == ["parent-orientations", "parent-train-daily-ic"]


def generated_by_add_alpha(path: Path) -> bool:
    """Ruling PM5-24: a library spec `research_cycle.py add-alpha` generated (R-2, R-7, R-12), not an authored one,
    recognised from its content as research_add_alpha.derive_spec writes it: a plain spec in lib-NAME.json (add-alpha's
    name) with add-alpha's content for library NAME (add_alpha_content)."""
    m = re.fullmatch(r"lib-(.+)\.json", path.name)
    return m is not None and add_alpha_content(json.loads(path.read_text(encoding="utf-8")), m[1])


def add_alpha_copy(path: Path) -> bool:
    """A spec add-alpha generated, copied by hand at another leverage (Ruling PM6-6, e.g. lib-v81-gm.json): a plain spec
    in lib-NAME-gm.json named NAME-gm with add-alpha's content for library NAME (add_alpha_content)."""
    m = re.fullmatch(r"lib-(.+)-gm\.json", path.name)
    doc = json.loads(path.read_text(encoding="utf-8")) if m else None
    return m is not None and isinstance(doc, dict) and doc.get("name") == f"{m[1]}-gm" and add_alpha_content(doc, m[1])


# Review F-9 (P9 P0-FIX): a spec's kind, read from its content, decides the pins `lock --write` fills (spec_null_pins);
# no list of file names. A -gm copy (PM6-6) takes the kind of what it copies: a template copy is a template, a copy of
# add-alpha's spec is an add-alpha copy.
TEMPLATE, BASE, ADD_ALPHA_COPY, GENERATED_KIND = "template", "base", "add-alpha copy", "generated"


def spec_kind(path: Path) -> str:
    """template (a child: a cell written as its parent plus the registered change), generated (add-alpha's own spec),
    add-alpha copy (a hand copy of one, add_alpha_copy) or base (any other plain spec: a root of the chain)."""
    if RS.is_template(json.loads(path.read_text(encoding="utf-8"))):
        return TEMPLATE
    return GENERATED_KIND if generated_by_add_alpha(path) else ADD_ALPHA_COPY if add_alpha_copy(path) else BASE


def nominal_spec(path: Path) -> dict:
    """A spec file resolved on its nominal chain: each template up the chain on its nominal_parent, whatever parent
    root set (as the authored specs plan)."""
    doc = json.loads(path.read_text(encoding="utf-8"))
    if not RS.is_template(doc):
        return RC.load_spec(path)
    return RS.resolve(dict(doc, parent=None), path, nominal_spec, research_tree.REPO)


def spec_null_pins(path: Path) -> set[str]:
    """Every pin root fills with `lock --write` (after the runbook builds and the parent cell), as the spec is authored
    and planned on its nominal parent, by the spec's kind (review F-9):
      a plain spec (base, add-alpha copy, generated): each input but a committed library or recipe (an atx-impl/ file
          it is authored with, checked against its bytes), and an as-built fields manifest;
      a template: its nominal parent's, less the inputs research_spec drops from a parent (IDENTITY_INPUTS, DERIVED),
          plus the inputs it derives from that parent (reference_cell, reference_admission; reference_combined and
          reference_weights when the resolved spec has a marginal section; reference_resid_parent when its fit flags
          add --theme-resid), those it adds (change.inputs) and the fields manifest when the resolved fields are as
          built."""
    doc = json.loads(path.read_text(encoding="utf-8"))
    if not RS.is_template(doc):
        out = {f"inputs.{k}" for k, item in doc["inputs"].items() if committed(item["path"]) is None}
        return out | ({"fields.manifest_sha256"} if RC.as_built(doc.get("fields")) else set())
    nominal = dict(doc, parent=None)
    parent, _ = RS.parent_of(nominal, path, research_tree.REPO)
    resolved = nominal_spec(path)
    change = doc.get("change") or {}
    fit_ops = (change.get("flags") or {}).get("fit") or {}
    derived = {"reference_cell", "reference_admission"} | \
        ({"reference_combined", "reference_weights"} if "marginal" in resolved else set()) | \
        ({"reference_resid_parent"} if isinstance(fit_ops.get(RS.RESID_FLAG), str) else set())
    unset = set(change.get("unset") or [])
    out = spec_null_pins(parent) - {f"inputs.{k}" for k in RS.IDENTITY_INPUTS + RS.DERIVED} - unset
    out |= {f"inputs.{k}" for k in derived} - unset
    out |= {f"inputs.{k}" for k in change.get("inputs") or {}}
    out.discard("fields.manifest_sha256")
    return out | ({"fields.manifest_sha256"} if RC.as_built(resolved.get("fields")) else set())


GENERATED = sorted(p.name for p in V8.glob("*.json") if generated_by_add_alpha(p))
V8_SPECS = sorted(p.name for p in V8.glob("*.json") if p.name not in GENERATED)        # the authored specs
BASE_NULLS = {"inputs.role", "inputs.identity_bridge", "inputs.fund_events", "fields.manifest_sha256"}
CHILD_NULLS = BASE_NULLS | {"inputs.reference_cell", "inputs.reference_admission",       # derived from the parent
                            "inputs.label_role"}                                          # B0c's (E-25), inherited
LIB_NULLS = CHILD_NULLS | {"inputs.library", "inputs.recipe", "inputs.reference_combined", "inputs.reference_weights"}
STORE_FILLS = ["<fill:nav.flags --risk-model>", "<fill:nav.flags --risk-model-sha256>"]
FILLS = {"r6-spo-v3.json": STORE_FILLS, "r8.json": STORE_FILLS}   # R-8: the risk store (lane RISK)
FILLS["r6-spo-v3-gm.json"] = STORE_FILLS                                                  # PM6-6: R-6's store
FILLS["y-vol-target.json"] = STORE_FILLS                                     # v8 Y (YCOMB): the risk target's store


def fill_options(name: str | None = None) -> list[tuple[str, str]]:
    """(section, option) of every value root fills in the spec ``name`` (FILLS), or in any spec when None."""
    found = [re.fullmatch(r"<fill:(\w+)\.flags (\S+)>", x) for n, xs in FILLS.items() if name in (None, n) for x in xs]
    assert all(found), FILLS
    return sorted({(m[1], m[2]) for m in found if m})


def well_filled(option: str, value) -> bool:
    """A value root filled is well formed (Ruling PM5-24): a 64-hex digest for a *sha256 option, else a path as the
    specs write them (relative POSIX segments, or under a drive)."""
    form = r"[0-9a-f]{64}" if option.endswith("sha256") else r"(?:[A-Za-z]:/)?[\w.\-]+(?:/[\w.\-]+)+"
    return isinstance(value, str) and re.fullmatch(form, value) is not None
FIT_DOWN = {"fit.output", "card.output", "ic.w_output", "nav.output", "monitor.output"}   # downstream of the fit
LIB_DOWN = FIT_DOWN | {"ic.u_output"}                                                       # downstream of the library
LIB_CHANGE = LIB_DOWN | {"inputs.library.path", "inputs.library.sha256", "inputs.recipe.path", "inputs.recipe.sha256",
                         "marginal.output", "marginal.pool", "marginal.themes", "gate.name", "gate.admitted",
                         "gate.require", "gate.sign_agrees", "gate.report"}
# a template renames exactly the outputs downstream of its change, and changes nothing else
LABEL_ROLE = {"inputs.label_role.dir", "inputs.label_role.path", "inputs.label_role.sha256"}   # Ruling E-25
W_3072 = {"runner.phases.w.max_rss_mib", "ic.w_flags.--max-memory-mib"}                          # Ruling E-28
EXPECTED_CHANGES = {"base-b0c.json": {"nav.output", "nav.flags"} | LABEL_ROLE,
                    "r1-comp-v8.json": FIT_DOWN | {"fit.flags"} | W_3072,
                    "r2-lib-v80.json": LIB_CHANGE, "r3-aim-gain.json": FIT_DOWN | {"fit.flags"},
                    "r4-hold-band.json": {"nav.output", "nav.flags"}, "r5-adv-hold.json": {"nav.output", "nav.flags"},
                    "r6-spo-v3.json": {"nav.output", "nav.flags", "nav.rule"}, "r7-lib-v81.json": LIB_CHANGE,
                    "r8.json": {"nav.output", "nav.flags"},
                    "r10.json": FIT_DOWN | {"fit.flags"}}             # its nominal parent R-1 carries W_3072
EXPECTED_CHANGES["r11.json"] = FIT_DOWN | {"fit.flags"}                                   # v8 R-11 (lane ORTH)
# Ruling PM6-6: R-1's registered change at the aim leverage that matches the parent's all-rows S2 gross
EXPECTED_CHANGES["r1-comp-v8-gm.json"] = EXPECTED_CHANGES["r1-comp-v8.json"] | {"nav.leverage"}
EXPECTED_CHANGES["r3-aim-gain-gm.json"] = EXPECTED_CHANGES["r3-aim-gain.json"] | {"nav.leverage"}     # PM7-4
EXPECTED_CHANGES["r6-spo-v3-gm.json"] = EXPECTED_CHANGES["r6-spo-v3.json"] | {"nav.leverage"}         # PM6-6
# Ruling PM7-21: R-9a / R-9b change theta (--trade-fraction) and are report-only ("verdict": false)
EXPECTED_CHANGES["r9a.json"] = EXPECTED_CHANGES["r9b.json"] = {"nav.output", "nav.flags", "verdict"}
THETA = {"r9a.json": ".03", "r9b.json": ".04"}
EXPECTED_CHANGES["x-theme-erc.json"] = FIT_DOWN | {"fit.flags"}                          # v8 X (lane XCOMB)
EXPECTED_CHANGES["x-inv-vol.json"] = {"nav.output", "nav.flags"}                          # v8 X (lane XCOMB)
EXPECTED_CHANGES["y-vol-target.json"] = {"nav.output", "nav.flags", "nav.leverage"}       # v8 Y: X-10's L, managed
EXPECTED_CHANGES["y-norm-score.json"] = {"nav.output", "nav.flags"}                       # v8 Y (lane YCOMB)
EXPECTED_CHANGES["y-theme-tsmom.json"] = FIT_DOWN | {"fit.flags"}                        # v8 Y (lane YCOMB)
EXPECTED_CHANGES["y-two-speed.json"] = FIT_DOWN | {"fit.flags", "nav.flags"}            # v8 Y-5: fit and NAV
FIT_APPENDED = {"r11.json": ["--theme-resid", "theme-resid-v1"]}                          # options a template appends
FIT_APPENDED["x-theme-erc.json"] = ["--theme-erc", "theme-erc-v1"]                       # v8 X (lane XCOMB)
FIT_APPENDED["y-theme-tsmom.json"] = ["--theme-tsmom", "theme-tsmom-v1"]                 # v8 Y (lane YCOMB)
FIT_APPENDED["y-two-speed.json"] = ["--two-speed", "two-speed-v1"]                       # v8 Y-5 (lane YCOMB)
MISSING = object()


@pytest.fixture(autouse=True)
def label_role_input(monkeypatch):
    """Ruling E-25: lane R45 adds inputs.label_role to research_cycle (INPUT_KEYS and the NAV step's --label-role
    MANIFEST --label-role-sha256 SHA); until that merges the v8 specs are checked with the key registered here."""
    if "label_role" not in RC.INPUT_KEYS:
        monkeypatch.setattr(RC, "INPUT_KEYS", RC.INPUT_KEYS + ("label_role",))


def flat(node, pre: str = "") -> dict:
    if isinstance(node, dict):
        out = {}
        for k, v in node.items():
            out.update(flat(v, f"{pre}.{k}" if pre else k))
        return out
    return {pre: node}


def committed(rel: str) -> bytes | None:
    """The bytes of a pinned library or recipe of this worktree (an atx-impl/ file): a fake root holds it as committed,
    so its pin is verified; None for every other input, which a fake root stands in for."""
    path = research_tree.REPO / rel
    return path.read_bytes() if rel.startswith("atx-impl/") and path.is_file() else None


def as_authored(spec: dict, name: str | None = None) -> dict:
    """A copy of a live v8 spec (a resolved spec, or a spec or template file's doc; ``name`` its file name) as its
    author committed it, before root's runbook steps on it: every pin that `lock --write` fills is null again -- each
    input's but a committed library's or recipe's (authored with that file and checked against its bytes), the as-built
    fields manifest's and a template's derived ones ("locked") --, a template's parent is null (planned on its nominal
    parent) and every value root fills (FILLS of ``name``) is null again (its "<fill:...>" placeholder). A template's
    own inputs (change.inputs) are null whatever the file: each is authored null and filled by its lock (E-25's label
    role; R-2's and R-7's library and recipe, which add-alpha commits after the template, PM6-10).

    Rulings PM5-20, PM5-24 (FIX-6, rounds 1 and 2): the fixtures plan the live specs on stand-in files, whose digests no
    real pin matches, and hold a template's registered change against its nominal parent; they plan and compare this
    copy, the same whether root has locked a spec, set a template's parent or filled its values or not. What a test
    asserts about the spec's content is unchanged; only those premises are (test_the_fixtures_plan_a_locked_spec_as_
    unlocked, test_the_fixtures_plan_every_template_on_every_plausible_parent)."""
    spec = copy.deepcopy(spec)
    if RS.is_template(spec):
        spec["parent"] = None
        change = spec.get("change") or {}
        for section, option in fill_options(name) if name else []:
            ((change.get("flags") or {}).get(section) or {})[option] = None
        items, sets = (change.get("inputs") or {}).values(), change.get("set") or {}
        fields = sets.get("fields")
        if "fields.manifest_sha256" in sets:
            sets["fields.manifest_sha256"] = None
        spec.pop("locked", None)
        held = lambda item: False                                             # noqa: E731
    else:
        items, fields = spec["inputs"].values(), spec.get("fields")
        held = lambda item: committed(item["path"]) is not None              # noqa: E731
    if RC.as_built(fields):
        fields["manifest_sha256"] = None
    for item in items:
        if not held(item):
            item["sha256"] = None
    return spec


def authored_dir(src: Path, dest: Path) -> Path:
    """``dest`` holding every spec file of the directory ``src`` as authored (a template resolves on its nominal
    parent, next to it)."""
    dest.mkdir(parents=True, exist_ok=True)
    for p in sorted(src.glob("*.json")):
        doc = as_authored(json.loads(p.read_text(encoding="utf-8")), p.name)
        (dest / p.name).write_text(json.dumps(doc, indent=2), encoding="utf-8")
    return dest


@pytest.fixture
def authored_v8(tmp_path) -> Path:
    """The v8 specs as authored, in a temporary copy: what a test reads that holds a template to its nominal parent."""
    return authored_dir(V8, tmp_path / "v8-authored")


def fake_root(tmp_path: Path, spec: dict) -> tuple[Path, dict]:
    """A root with a stand-in file for every input, the as-built fields manifest, the pool and an empty ledger; the
    returned spec is the planned copy: its pins as authored (as_authored) and absolute input paths re-pointed under the
    root."""
    root = tmp_path / "root"
    spec = as_authored(spec)
    files: dict[str, str | bytes] = {"build-equity/trials.jsonl": ""}
    for key, item in spec["inputs"].items():
        if Path(item["path"]).is_absolute():
            item["dir"], item["path"] = f"ext/{key}", f"ext/{key}/manifest.json"
        held = committed(item["path"])                          # a pinned library or recipe of this worktree, as is
        files[item["path"]] = "{}" if held is None else held
    role = spec["inputs"]["role"]
    files[role["path"]] = json.dumps({"universe": {"id": role["universe"]}, "dates": 1405, "score_begin": 399})
    f = spec["fields"]
    files[f"{f['output']}/manifest.json"] = json.dumps({"fields": [{"name": n} for n in f["list"]]})
    for rel, text in files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(text if isinstance(text, bytes) else text.encode())
    if "marginal" in spec:          # the pool binds the role and, with marginal.themes, those weights (research_cycle
        m = spec["marginal"]        # check_marginal_bindings); without themes (R-2 on the combined signal alone, PM6-8)
        weights = spec["inputs"][m["themes"]]["path"] if m.get("themes") else role["path"]   # it binds the role only
        pool = spec["inputs"][m.get("pool", "reference_combined")]["path"]
        (root / pool).write_text(T.pool_manifest(root, role["path"], weights))
    return root, spec


def plan_on_stand_ins(tmp_path: Path, path: Path) -> tuple[Path, dict, RC.Cycle, list[str]]:
    """The fixture path of a v8 spec file: loaded (a template resolved up its parent chain), planned on a fake root
    (its pins as authored); (root, planned spec, cycle, plan lines)."""
    root, spec = fake_root(tmp_path, RC.load_spec(path))
    c = RC.Cycle(spec, RC.Resolver(root), spec_path=path, capabilities=T.CAPS)
    return root, spec, c, RC.plan_lines(c)


def unlocked_pins(c: RC.Cycle, spec: dict) -> set[str]:
    """The pins a plan reports UNLOCKED (computed from the file at plan time)."""
    pins = {f"inputs.{k}" for k, (_, _, how) in c.pins.items() if how.startswith("UNLOCKED")}
    return pins | ({"fields.manifest_sha256"} if spec["fields"]["manifest_sha256"] is None else set())


def check_nominal_plan(tmp_path: Path, specs: Path, name: str) -> None:
    """The spec ``name`` of the directory ``specs`` as authored (a template on its nominal parent) loads and plans with
    the fake root; the pins a lock fills are reported (UNLOCKED, computed from the stand-in files), never crashed on; a
    template's run is refused while its parent is null (and while its requires are open / values unfilled); a base
    spec has no refusal."""
    path = authored_dir(specs, tmp_path / "authored") / name
    root, spec, c, lines = plan_on_stand_ins(tmp_path / "nominal", path)
    assert RC.plan_lines(c, lines_only=True) and not any("<sha256:" in x for x in lines if x.startswith("#"))
    assert unlocked_pins(c, spec) == spec_null_pins(path), name                  # by the spec's kind (review F-9)
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
    same_fields = (spec["inputs"].get("baseline_fields") or {}).get("path") == f"{spec['fields']['output']}/manifest.json"
    assert all(st.kind != "skipped" or st.phase == "marginal" or       # ref: skipped on the parent's fields (X-3 gm)
               (same_fields and st.phase in ("ref", "ref" + RC.COMPARE_SUFFIX)) for st in c.steps())


def check_live_plan(tmp_path: Path, specs: Path, name: str) -> dict:
    """The spec ``name`` as it stands in ``specs`` (locked or not; a template on the parent root set, else on its
    nominal one) plans with the fake root: a parent root set names another spec of that directory and pairs the cell
    with it (reference_cell is the parent's NAV cell); every pin a lock fills is UNLOCKED (on a chain of registered
    specs at least the registry's: a chain on base-lo3 adds its own); every value root fills is its placeholder or well
    formed; `run` is refused for a null parent only, while the template's own requires are open and while a value is
    unfilled. Returns the planned spec."""
    path = specs / name
    doc = json.loads(path.read_text(encoding="utf-8"))
    _, spec, c, lines = plan_on_stand_ins(tmp_path / "live", path)
    assert RC.plan_lines(c, lines_only=True) and not any("<sha256:" in x for x in lines if x.startswith("#")), name
    stand_ins = {f"inputs.{k}" for k, item in spec["inputs"].items() if committed(item["path"]) is None}
    assert unlocked_pins(c, spec) >= stand_ins | {"fields.manifest_sha256"}, name
    held = {f"inputs.{k}" for k, item in spec["inputs"].items()
            if item["sha256"] and committed(item["path"]) is not None}
    links = RS.chain(path, research_tree.REPO)
    above = ({p.name for p, _, _, _ in links} | {links[-1][2].name if links else name}) - {name}
    if spec_kind(path) != GENERATED_KIND and all((specs / n).is_file() and spec_kind(specs / n) in (BASE, TEMPLATE)
                                                 for n in above):
        # (an add-alpha parent, lib-v80.json, or a hand copy of one, has fewer; a lock's pin on a committed file, e.g.
        # R-2's library once add-alpha wrote it (PM6-10), is checked against its bytes, not a stand-in: it may stand LOCKED)
        assert unlocked_pins(c, spec) >= spec_null_pins(path) - held, name
    for section, option in fill_options():                    # PM5-24: a value root fills is its placeholder or well
        value = RC.option_value((spec.get(section) or {}).get("flags") or [], option)          # formed (own, inherited)
        assert value in (None, f"<fill:{section}.flags {option}>") or well_filled(option, value), (name, option, value)
    if not RS.is_template(doc):
        return spec
    parent, nominal = RS.parent_of(doc, path, research_tree.REPO)
    assert nominal == (doc["parent"] is None) and parent.parent == specs.resolve() and parent.name != name, name
    assert any(x.startswith(f"# template {name}: parent {'null' if nominal else parent.name}") for x in lines), name
    assert spec["inputs"]["reference_cell"]["path"] == f"{RC.load_spec(parent)['nav']['output']}/summary.json", name
    refusal = RS.run_refusal(spec, path, research_tree.REPO) or ""
    assert ("template parent is null" in refusal) == nominal, name
    assert all(f"{name} requires: {r}" in refusal for r in doc.get("requires", [])), name
    assert all(f"unfilled value {x}" in refusal for x in RS.fills(spec)), name           # refused while one is left
    return spec


@pytest.mark.parametrize("name", V8_SPECS)
def test_every_v8_spec_loads_and_plans(tmp_path, name):
    """Every live authored spec plans as authored (check_nominal_plan) and as it stands (check_live_plan): the same
    whether root has locked it, set a template's parent or filled its values or not. The authored set is the registry
    (a spec add-alpha generated is checked by test_every_generated_spec_plans_and_parents_the_templates)."""
    # review F-9 (no file list): the kind from independent evidence: a template by its schema; a lib-NAME-gm.json
    # beside a generated lib-NAME.json is that spec's hand copy (PM6-6), recognised from the copy's own content; any
    # other plain spec is a base (review E1t0 minor 4: the old line could not fail)
    copied = V8 / re.sub(r"-gm\.json$", ".json", name)
    want = TEMPLATE if RS.is_template(json.loads((V8 / name).read_text(encoding="utf-8"))) else \
        ADD_ALPHA_COPY if name.startswith("lib-") and name.endswith("-gm.json") and copied.is_file() and \
        generated_by_add_alpha(copied) else BASE
    assert spec_kind(V8 / name) == want, name
    check_nominal_plan(tmp_path, V8, name)
    check_live_plan(tmp_path, V8, name)


def test_spec_kind_null_pins(tmp_path, authored_v8):
    """Review F-9 (P9 P0-FIX): the pins a spec's lock fills follow from its kind, read from its content, not from a
    list of file names: a base spec's are its stand-in inputs and the fields manifest; a template's its nominal
    parent's less the parent's identity inputs, plus what it derives and adds (R-1: B0c's label role and the paired
    reference; R-2: its library, recipe and pool; R-11: the resid parent); an add-alpha copy's every input but the
    committed library and recipe. A new cell of each kind (a template, a -gm copy of add-alpha's spec) plans with no
    edit of this file."""
    kinds = {n: spec_kind(V8 / n) for n in ("base-lo1.json", "base-b0c.json", "x-theme-erc-gm.json",
                                            "lib-v81-gm.json")}
    assert kinds == {"base-lo1.json": BASE, "base-b0c.json": TEMPLATE, "x-theme-erc-gm.json": TEMPLATE,
                     "lib-v81-gm.json": ADD_ALPHA_COPY}
    assert all(spec_kind(V8 / n) == GENERATED_KIND for n in GENERATED)
    want = {"base-lo1.json": BASE_NULLS, "base-lo3.json": BASE_NULLS | {"inputs.sic_events", "inputs.reference_cell"},
            "r1-comp-v8.json": CHILD_NULLS, "r2-lib-v80.json": LIB_NULLS, "x-theme-erc-gm.json": CHILD_NULLS,
            "r11.json": CHILD_NULLS | {"inputs.reference_resid_parent"},
            "lib-v81-gm.json": {"inputs.role", "inputs.label_role", "inputs.baseline_fields", "fields.manifest_sha256",
                                "inputs.reference_admission", "inputs.reference_cell", "inputs.reference_combined",
                                "inputs.reference_weights", "inputs.reference_daily", "inputs.reference_orientations",
                                "inputs.reference_daily_ic"}}
    assert {n: spec_null_pins(authored_v8 / n) for n in want} == want
    specs = tmp_path / "v8"
    shutil.copytree(V8, specs)
    cell = json.loads((specs / "x-theme-erc.json").read_text(encoding="utf-8"))          # a new template cell
    cell["name"] = "x-theme-erc-p9"
    cell["change"]["set"]["nav.output"] = cell["change"]["set"]["nav.output"] + "-p9"
    (specs / "x-theme-erc-p9.json").write_text(json.dumps(cell, indent=2), encoding="utf-8")
    lib = next(n for n in GENERATED if not (V8 / n.replace(".json", "-gm.json")).exists())
    copy_ = json.loads((specs / lib).read_text(encoding="utf-8"))                          # add-alpha's, by hand
    copy_["name"] = f"{copy_['name']}-gm"
    copy_["nav"] = dict(copy_["nav"], leverage="1.5", output=copy_["nav"]["output"] + "-L1.5")
    gm = lib.replace(".json", "-gm.json")
    (specs / gm).write_text(json.dumps(copy_, indent=2), encoding="utf-8")
    assert (spec_kind(specs / "x-theme-erc-p9.json"), spec_kind(specs / gm)) == (TEMPLATE, ADD_ALPHA_COPY)
    for k, name in enumerate(("x-theme-erc-p9.json", gm)):
        check_nominal_plan(tmp_path / f"new-{k}", specs, name)


def test_the_fixtures_plan_a_locked_spec_as_unlocked(tmp_path):
    """Ruling PM5-20: a copy of every v8 spec, locked as `lock --write` locks it (relocked where root already has: each
    pin it fills an arbitrary digest no stand-in has, a committed library or recipe its own), plans through the fixture
    paths of the live spec (as authored: the same pins reported UNLOCKED; as it stands); the planned copy with a locked
    pin put back is refused. So the suite does not depend on whether root has locked base-lo1, base-lo3 or a template
    on them."""
    specs = tmp_path / "v8"
    shutil.copytree(V8, specs)

    def lock_reads(self, rel):                        # what `lock` reads, without a file under any root
        held = committed(rel)
        return hashlib.sha256(f"arbitrary {rel}".encode() if held is None else held).hexdigest()
    with pytest.MonkeyPatch.context() as mp:
        mp.setattr(RC.Resolver, "sha", lock_reads)
        for name in V8_SPECS + GENERATED:             # base specs fill inputs and fields, templates change.inputs
            locked, _ = RC.lock(specs / name, tmp_path / "no-root", relock=True)   # and "locked" (the derived pins)
            (specs / name).write_text(json.dumps(locked, indent=2), encoding="utf-8")
    for name in V8_SPECS + GENERATED:                 # (a generated spec: add-alpha's, PM5-24)
        path = specs / name
        locked = RC.load_spec(path)
        assert all(item["sha256"] for item in locked["inputs"].values()), name          # every pin filled
        assert locked["fields"]["manifest_sha256"], name
        if name in GENERATED:
            check_generated(tmp_path / name, specs, name)
        else:
            check_nominal_plan(tmp_path / name, specs, name)
            check_live_plan(tmp_path / name, specs, name)
        root, spec, _, _ = plan_on_stand_ins(tmp_path / name / "control", path)
        role = dict(spec["inputs"]["role"], sha256=locked["inputs"]["role"]["sha256"])
        with pytest.raises(RC.CycleError, match="PIN MISMATCH role") as e:             # the lock's pin, on a stand-in
            RC.Cycle(dict(spec, inputs=dict(spec["inputs"], role=role)), RC.Resolver(root), spec_path=path,
                     capabilities=T.CAPS)
        assert e.value.code == RC.EXIT_PIN, name


# the templates in the order root runs their cells (task-CELLS-brief.md): each on the last accepted cell's spec, B0c
# (accepted by declaration) on the winner of B0a / B0b; R-10 and R-11 run only if R-1 and R-6 were accepted (E-38).
# R-2 and R-7 run as add-alpha's lib-v80.json / lib-v81.json: their templates stand in for those cells as parents.
CELLS = ("base-b0c.json", "r1-comp-v8.json", "r2-lib-v80.json", "r3-aim-gain.json", "r4-hold-band.json",
         "r5-adv-hold.json", "r6-spo-v3.json", "r7-lib-v81.json", "r8.json", "r10.json", "r11.json")
ONLY_IF = {"r10.json": {"r1-comp-v8.json", "r6-spo-v3.json"}, "r11.json": {"r1-comp-v8.json", "r6-spo-v3.json"}}
WINNERS = ("base-lo1.json", "base-lo3.json")


def ran_parents(name: str, winner: str, accepted: set[str]) -> dict[str, str] | None:
    """The parent root set in every template that ran up to and including ``name`` (each on the last cell accepted
    before it), or None when ``name`` or an accepted cell could not run (its condition unmet)."""
    parents, last, done = {}, winner, set()
    for cell in CELLS[:CELLS.index(name) + 1]:
        if not ONLY_IF.get(cell, set()) <= done:
            if cell == name or cell in accepted:
                return None
            continue
        parents[cell] = last
        if cell in accepted or cell == CELLS[0]:
            last = cell
            done.add(cell)
    return parents


def plausible_histories() -> list[tuple[str, dict[str, str]]]:
    """(template, the parents root set) for each template on each plausible accepted predecessor P, after two
    histories: every cell up to P accepted (B0a won), and only P and the cells P and the template need accepted (B0b
    won); the cells in between ran and were rejected."""
    out: list[tuple[str, dict[str, str]]] = []
    for name in CELLS:
        for pred in WINNERS + CELLS[:CELLS.index(name)]:
            dense = set(CELLS[:CELLS.index(pred) + 1]) if pred in CELLS else set()
            sparse = {pred, *ONLY_IF.get(name, ()), *ONLY_IF.get(pred, ())}
            for winner, accepted in zip(WINNERS, (dense, sparse)):
                parents = ran_parents(name, winner, accepted)
                if parents and parents[name] == pred and (name, parents) not in out:
                    out.append((name, parents))
    return out


def test_the_fixtures_plan_every_template_on_every_plausible_parent(tmp_path):
    """FIX-6 round 1: root sets each template's parent to the last accepted cell's spec, cell by cell. For each template
    on each plausible accepted predecessor, in a copy of the v8 specs with the parent of every cell that ran set: the
    copy as authored is byte for byte the live specs' as authored (so every assertion held against a nominal parent
    reads the same files); the template plans with the parent set (check_live_plan) and still differs from its nominal
    parent only by its registered change (check_registered_change); the rules the chain decides follow it (E-27: R-3's
    aim rule by whether R-1 was accepted; E-44, E-45: R-10's shrink rule by whether R-3 was, on an R-1 chain only;
    R-11 keeps its parent's composition). Round 2 (PM5-24): every cell that ran carries the values root filled (R-6,
    R-8: the risk store and its digest); the template itself is filled in the dense history, still to fill in the
    sparse one, and its run is refused exactly while a value is unfilled."""
    cases = plausible_histories()
    preds: dict[str, set[str]] = {}
    for name, parents in cases:
        preds.setdefault(name, set()).add(parents[name])
    later = {n: set(CELLS[:k]) for k, n in enumerate(CELLS) if k and n not in ONLY_IF}
    assert preds == {CELLS[0]: set(WINNERS), **later, "r10.json": {"r6-spo-v3.json", "r7-lib-v81.json", "r8.json"},
                     "r11.json": {"r6-spo-v3.json", "r7-lib-v81.json", "r8.json", "r10.json"}}
    want = {p.name: p.read_bytes() for p in authored_dir(V8, tmp_path / "authored").glob("*.json")}
    for i, (name, parents) in enumerate(cases):
        specs = tmp_path / f"h{i}" / "v8"
        shutil.copytree(V8, specs)
        dense = parents[CELLS[0]] == WINNERS[0]
        store = f"build-equity/atx-risk-v1.1-{parents[CELLS[0]][5:8]}/manifest.json"   # what root fills (R-6, R-8)
        for cell, parent in parents.items():
            doc = dict(json.loads((specs / cell).read_text(encoding="utf-8")), parent=parent)
            for section, option in fill_options(cell):          # filled, or (the template, sparse) still to fill
                doc["change"]["flags"][section][option] = None if cell == name and not dense else \
                    hashlib.sha256(store.encode()).hexdigest() if option.endswith("sha256") else store
            (specs / cell).write_text(json.dumps(doc, indent=2), encoding="utf-8")
        authored = authored_dir(specs, tmp_path / f"h{i}" / "authored")
        assert {p.name: p.read_bytes() for p in authored.glob("*.json")} == want, (name, parents)
        spec = check_live_plan(tmp_path / f"h{i}", specs, name)
        assert bool(RS.fills(spec)) == (name in FILLS and not dense), (name, parents)
        check_registered_change(authored, name)
        chain, cell = set(), name
        while cell in parents:
            cell = parents[cell]
            chain.add(cell)
        composition = RC.option_value(spec["fit"]["flags"], "--composition")
        if name == "r3-aim-gain.json":
            assert composition == ("ew-theme-std-aim-v1" if "r1-comp-v8.json" in chain else "ew-theme-aim-v2"), parents
        if name == "r10.json":
            assert composition == ("ic-shrink-aim-v1" if "r3-aim-gain.json" in chain else "ic-shrink-v1"), parents
        if name == "r11.json":
            parent_fit = RC.load_spec(specs / parents[name])["fit"]["flags"]
            assert composition == RC.option_value(parent_fit, "--composition"), parents
            assert RC.option_value(spec["fit"]["flags"], "--theme-resid") == "theme-resid-v1", parents


def check_generated(tmp_path: Path, specs: Path, name: str) -> None:
    """Ruling PM5-24: the generated library spec ``name`` of ``specs`` loads and plans through the fixture path (every
    pin a lock fills UNLOCKED; `run` not refused) and is accepted as a parent: every template that runs after B0c plans
    on it (check_live_plan), but one whose composition map lacks its composition, which refuses it at load (R-10 on a
    parent that is not standardised, E-45)."""
    path = specs / name
    assert generated_by_add_alpha(path) and spec_kind(path) == GENERATED_KIND, name
    _, spec, c, lines = plan_on_stand_ins(tmp_path / "plan", path)
    assert RC.plan_lines(c, lines_only=True) and not any("<sha256:" in x for x in lines if x.startswith("#")), name
    stand_ins = {f"inputs.{k}" for k, item in spec["inputs"].items() if committed(item["path"]) is None}
    assert unlocked_pins(c, spec) >= stand_ins | {"fields.manifest_sha256"}, name
    assert RS.run_refusal(spec, path, research_tree.REPO) is None, name
    check_parents(tmp_path, specs, name, CELLS[1:])


def check_parents(tmp_path: Path, specs: Path, name: str, children) -> dict[str, dict]:
    """The spec ``name`` of ``specs`` as the parent root sets in each template of ``children``: each plans on it
    (check_live_plan), but one whose composition map lacks its composition, which refuses it at load. Returns
    {child: planned spec}."""
    composition = RC.option_value(RC.load_spec(specs / name)["fit"]["flags"], "--composition")
    planned = {}
    for k, child in enumerate(children):
        on = tmp_path / f"on-{k}" / "v8"
        shutil.copytree(specs, on)
        doc = dict(json.loads((on / child).read_text(encoding="utf-8")), parent=name)
        (on / child).write_text(json.dumps(doc, indent=2), encoding="utf-8")
        mapped = ((doc["change"].get("flags") or {}).get("fit") or {}).get("--composition")
        if isinstance(mapped, dict) and composition not in mapped:
            with pytest.raises(RC.CycleError, match="maps the parent's value"):
                RC.load_spec(on / child)
            continue
        planned[child] = check_live_plan(tmp_path / f"on-{k}", on, child)
    return planned


def test_every_generated_spec_plans_and_parents_the_templates(tmp_path):
    """Ruling PM5-24: every spec add-alpha generated into scripts/specs/v8 (none before R-2; lib-v80.json, lib-v81.json,
    R-12's after them) passes check_generated; none is in the authored registry."""
    assert not set(GENERATED) & set(V8_SPECS) and not any(generated_by_add_alpha(V8 / n) for n in V8_SPECS)
    for k, name in enumerate(GENERATED):
        check_generated(tmp_path / f"g{k}", V8, name)
        print(f"generated spec checked: {name}")


def test_r1_at_matched_gross_parents_the_later_templates(tmp_path):
    """Ruling PM6-6 (PM6-10: the suite accepts root's hand-written spec): r1-comp-v8-gm.json, R-1's registered change at
    the aim leverage that matches the parent's gross, is accepted as the parent of every template that may run after
    R-1 (R-2 runs as add-alpha's lib-v80.json on it), each reading it as R-1: R-3 the standardised aim rule (E-27),
    R-10 the shrink rule (E-44), R-11 its composition."""
    planned = check_parents(tmp_path, V8, "r1-comp-v8-gm.json", CELLS[2:])
    assert set(planned) == set(CELLS[2:])                                    # no composition map refuses it
    composition = {n: RC.option_value(s["fit"]["flags"], "--composition") for n, s in planned.items()}
    assert (composition["r3-aim-gain.json"], composition["r10.json"], composition["r11.json"]) == (
        "ew-theme-std-aim-v1", "ic-shrink-v1", "ew-theme-std-v1")


def test_the_whole_file_passes_with_a_generated_spec_present(tmp_path):
    """Ruling PM5-24: the R-2 path on stand-ins (v8_root, parent_cell, add-alpha v71 -> PROBE on base-lo1) generates
    lib-PROBE.json; with its tools back on base-lo1's (v8_root points them at the fake ones), as add-alpha writes it
    from the live base, it goes into a copy of scripts/specs/v8. The rule recognises it from its content: the same file
    under another name, without add-alpha's marks, or as a template is authored. Then this whole file runs on the copy
    (a subprocess with ATX_TEST_V8_SPECS; this test deselected) and passes, the generated spec checked in it, next to
    those root generated (lib-v80.json since R-2). PM6-10: the probe is not named v80, whose library root committed (a
    fake root holds a committed library as is, so it would stand in for the probe's own and fail its pin)."""
    root, parent = v8_root(tmp_path / "made")
    parent_cell(root, parent)
    assert add(root, parent, "v8_probe", "rank(decay_linear((be / at_lag4), 21))", name=PROBE) == RC.EXIT_OK
    doc = json.loads((root / SPECS / f"lib-{PROBE}.json").read_text(encoding="utf-8"))
    assert committed(doc["inputs"]["library"]["path"]) is None, doc["inputs"]["library"]   # a stand-in, not a file
    base = json.loads((V8 / "base-lo1.json").read_text(encoding="utf-8"))
    doc.update(python=base["python"], exes=base["exes"], runner=dict(doc["runner"], script=base["runner"]["script"]))
    for section in ("fit", "card", "monitor", "summ"):
        doc[section]["script"] = base[section]["script"]
    specs = tmp_path / "v8"
    shutil.copytree(V8, specs)
    (specs / f"lib-{PROBE}.json").write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    assert generated_by_add_alpha(specs / f"lib-{PROBE}.json")
    other = tmp_path / "other"
    other.mkdir()
    lib = f"lib-{PROBE}.json"
    for file, bad in ((f"{PROBE}.json", doc), ("lib-v81.json", doc), (lib, dict(doc, description="by hand")),
                      (lib, dict(doc, gate=dict(doc["gate"], name="b0a-readout"))),
                      (lib, {k: v for k, v in doc.items() if k != "compare"}),
                      (lib, dict(doc, schema=RS.TEMPLATE_SCHEMA))):
        (other / file).write_text(json.dumps(bad), encoding="utf-8")
        assert not generated_by_add_alpha(other / file), file
    run = subprocess.run([sys.executable, "-m", "pytest", "-q", "-rA", "-p", "no:cacheprovider", "--basetemp",
                          str(tmp_path / "inner"), str(Path(__file__).resolve()), "-k",
                          "not test_the_whole_file_passes_with_a_generated_spec_present"],
                         env=dict(os.environ, ATX_TEST_V8_SPECS=str(specs)), cwd=research_tree.REPO,
                         capture_output=True, text=True, timeout=600)
    tail = run.stdout[-4000:] + run.stderr[-2000:]
    assert run.returncode == 0 and re.search(r"\n\d+ passed, 1 deselected in ", run.stdout), tail   # nothing failed
    for name in sorted({lib, *GENERATED}):                                                 # (each was in the run)
        assert f"generated spec checked: {name}" in run.stdout, tail


def check_registered_change(specs: Path, name: str) -> None:
    """The template ``name`` of ``specs``, a directory of specs as authored (authored_dir), differs from its nominal
    parent only by its registered change."""
    doc = json.loads((specs / name).read_text(encoding="utf-8"))
    child = RC.load_spec(specs / name)
    parent = RC.load_spec(specs / doc["nominal_parent"])
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
    if RS.RESID_FLAG in ((doc["change"].get("flags") or {}).get("fit") or {}):   # v8 R-11: the re-fit's parent check
        want["reference_resid_parent"] = {"path": f"{parent['fit']['output']}/composition_weights.json", "sha256": None}
    assert refs == want
    pn, cn = parent["nav"]["flags"], child["nav"]["flags"]
    nav_delta = {"base-b0c.json": pn + ["--warm-start-sessions", "60", "--capacity-curve"],
                 "r4-hold-band.json": pn + ["--hold-band", ".1"], "r5-adv-hold.json": pn + ["--adv-hold-q", ".1"],
                 "r6-spo-v3.json": pn + [                                   # review F-14 / E-37: the curve stays
                     "--spo-alpha", "implied-aim", "--risk-model", "<fill:nav.flags --risk-model>", "--risk-model-sha256",
                     "<fill:nav.flags --risk-model-sha256>", "--spo-books", "primary"],
                 # R-8: the registered constants spelled out; --capacity-curve (E-29) is already the parent's
                 "r8.json": pn + ["--risk-target", ".05", "--risk-target-bias", "1.15", "--risk-target-cadence", "21",
                                  "--risk-model", "<fill:nav.flags --risk-model>", "--risk-model-sha256",
                                  "<fill:nav.flags --risk-model-sha256>"],
                 "x-inv-vol.json": pn + ["--vol-scale", "inv-vol-v1"],                  # v8 X (lane XCOMB)
                 # v8 Y (lane YCOMB): vol-target-v1 on the risk target's store (root fills it)
                 "y-vol-target.json": pn + ["--vol-target", "vol-target-v1", "--risk-model",
                                            "<fill:nav.flags --risk-model>", "--risk-model-sha256",
                                            "<fill:nav.flags --risk-model-sha256>"],
                 "y-norm-score.json": pn + ["--rank-shape", "norm-score-v1"],             # v8 Y (lane YCOMB)
                 "y-two-speed.json": pn + ["--two-speed", "two-speed-v1"]}                # v8 Y-5 (lane YCOMB)
    nav_delta["r6-spo-v3-gm.json"] = nav_delta["r6-spo-v3.json"]              # PM6-6: R-6's change, its L apart
    for r9, theta in THETA.items():                                          # PM7-21: the parent's argv, theta set
        nav_delta[r9] = [theta if k and pn[k - 1] == "--trade-fraction" else x for k, x in enumerate(pn)]
    spo = ("r6-spo-v3.json", "r6-spo-v3-gm.json")
    assert cn == nav_delta.get(name, pn)
    assert "--capacity-curve" in cn or name not in ("r5-adv-hold.json", "x-inv-vol.json") + spo   # E-29: the 4x report
    comp = {"r1-comp-v8.json": ("ew-theme-v1", "ew-theme-std-v1"),
            "r1-comp-v8-gm.json": ("ew-theme-v1", "ew-theme-std-v1"),                     # PM6-6: R-1's change
            "r3-aim-gain.json": ("ew-theme-v1", "ew-theme-aim-v2"),                       # E-27b
            "r3-aim-gain-gm.json": ("ew-theme-v1", "ew-theme-aim-v2"),                    # PM6-6 / PM7-4: R-3's change
            "r10.json": ("ew-theme-std-v1", "ic-shrink-v1")}               # (the parent's --composition, the cell's;
    # r10 derives its rule from any parent composition, Ruling E-44: test_r10_derives_its_rule_from_the_parent...)
    old, new = comp.get(name, (None, None))
    assert child["fit"]["flags"] == [new if x == old else x for x in parent["fit"]["flags"]] + \
        FIT_APPENDED.get(name, [])
    assert (child["nav"]["rule"] == "spo-v3") == (name in spo)


@pytest.mark.parametrize("name", [n for n in V8_SPECS if n in EXPECTED_CHANGES])
def test_templates_differ_from_the_parent_only_by_the_registered_change(authored_v8, name):
    check_registered_change(authored_v8, name)


def test_v8_base_specs_carry_the_ruled_settings():
    lo1, lo3 = RC.load_spec(V8 / "base-lo1.json"), RC.load_spec(V8 / "base-lo3.json")
    for s in (lo1, lo3):
        assert s["summ"]["dsr_n"] == "ledger+1" and s["summ"]["cells_from_ledger"] is True
        assert s["runner"]["phases"] == {"u": {"seconds": 300, "max_rss_mib": 2560},       # IC phases (OD-2)
                                         "w": {"seconds": 300, "max_rss_mib": 2560},
                                         "card": {"seconds": 300, "max_rss_mib": 2560}}   # the card on 4 years
        assert s["fit"]["work_dir"] == "build-equity/fit-work"                            # C-1's store base
        assert RC.option_value(s["card"]["flags"], "--work-dir") == "build-equity/fit-work"
        assert s["fields"]["list"] == RC.load_spec(HERE.parent / "specs" / "v71.json")["fields"]["list"]
        assert s["summ"]["script"] == "atx-impl/tools/nav_summ.py" and "--protocol" in s["summ"]["extra"]
        # review C-2 / C-1 (FIX-C): the K5 class in summ.origin (v7.1's members are all prior), never --origin in
        # extra; a verdict spec names its ledger (nav_summ --dsr-ledger). Templates inherit the summ block.
        assert s["summ"]["origin"] == "prior" and "--origin" not in s["summ"]["extra"]
        assert s["verdict"] is True and s["summ"]["ledger"] == "build-equity/trials.jsonl"
    assert lo3["inputs"]["reference_cell"]["dir"] == lo1["nav"]["output"]                 # B0b is paired with B0a
    assert (lo1["inputs"]["role"]["universe"], lo3["inputs"]["role"]["universe"]) == ("linked-operating-v1",
                                                                                      "linked-operating-v3")
    b0c = json.loads((V8 / "base-b0c.json").read_text(encoding="utf-8"))
    assert b0c["change"]["flags"]["nav"] == {"--warm-start-sessions": "60", "--capacity-curve": True}   # D-0, E-29
    # E-25 / PM6-5: the label role is the winner's role rebuilt by R15 (lo3-dlret since B0b won, batch 1b's lock
    # 2ad09c13; lo1-dlret while planned on B0a); its pin is the lock's (as_authored: null)
    winner = {"base-lo1.json": lo1, "base-lo3.json": lo3}[b0c["parent"] or b0c["nominal_parent"]]   # E-10
    label = f"{winner['inputs']['role']['dir']}-dlret"
    assert label == "build-equity/train-2020-2023-lo3-dlret"                              # the live spec: B0b's role
    assert as_authored(b0c)["change"]["inputs"] == {"label_role": {"dir": label, "path": f"{label}/manifest.json",
                                                                   "sha256": None}}
    docs = {n: json.loads((V8 / n).read_text(encoding="utf-8")) for n in V8_SPECS}
    assert {n for n, d in docs.items() if d.get("requires")} == {"r2-lib-v80.json", "r7-lib-v81.json"}   # add-alpha
    assert all("lib-v8" in d["requires"][0] for n, d in docs.items() if d.get("requires"))


def test_r1_runs_the_weighted_pass_under_3072_mib_and_its_children_inherit_it(tmp_path, authored_v8):
    """Ruling E-28: ew-theme-std-v1 (and its aim variant) runs the w pass under 3,072 MiB (runner cap and the IC's own
    --max-memory-mib); the u pass stays 2,560; a template on R-1 inherits both."""
    for name, parent in (("r1-comp-v8.json", None), ("r3-aim-gain.json", str(authored_v8 / "r1-comp-v8.json"))):
        path = authored_v8 / name
        if parent:
            path = tmp_path / name
            path.write_text(json.dumps(dict(json.loads((authored_v8 / name).read_text(encoding="utf-8")),
                                            parent=parent)), encoding="utf-8")
        root, spec = fake_root(tmp_path / f"root-{name[:2]}", RC.load_spec(path))
        c = RC.Cycle(spec, RC.Resolver(root), spec_path=path, capabilities=T.CAPS)
        assert [c.phase_caps(p)["max_rss_mib"] for p in ("u", "w", "card", "nav")] == [2560, 3072, 2560, 1536]
        steps = {s.phase: s for s in c.steps()}
        ic = {p: steps[p].argv[steps[p].argv.index("--") + 1:] for p in ("u", "w")}
        assert [ic[p].count("--max-memory-mib") for p in ("u", "w")] == [1, 1]
        assert (RC.option_value(ic["u"], "--max-memory-mib"), RC.option_value(ic["w"], "--max-memory-mib")) == (
            "2560", "3072")
        assert RC.option_value(spec["fit"]["flags"], "--composition") == (
            "ew-theme-std-v1" if name.startswith("r1") else "ew-theme-std-aim-v1")
    spec = RC.load_spec(authored_v8 / "r1-comp-v8.json")
    RC.validate_spec(dict(spec, ic=dict(spec["ic"], w_flags={"--save-combined": True, "--workers": "2"})))
    for bad in ({"--output": "x"}, {"--workers": None}, "--max-memory-mib 3072", {"--max-memory-mib": True}):
        with pytest.raises(RC.CycleError, match="w_flags") as e:
            RC.validate_spec(dict(spec, ic=dict(spec["ic"], w_flags=bad)))
        assert e.value.code == RC.EXIT_USAGE


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
    assert RS.apply_flags(base, {"--a": {"1": "7", "2": "8"}}, "s") == ["--a", "7", "--flag", "--b", "x"]  # by parent
    for ops in ({"--a": {"3": "7"}}, {"--flag": {"1": "7"}}, {"--z": {"1": "7"}}, {"--a": {"1": True}}):
        with pytest.raises(RS.TemplateError, match="maps the parent's value"):
            RS.apply_flags(base, ops, "s")
    assert RS.fills({"x": ["<fill:s --z>", "y"], "z": {"w": "<fill:t>"}}) == ["<fill:s --z>", "<fill:t>"]


def test_r3_maps_the_parents_composition_to_its_aim_rule(tmp_path, authored_v8):
    """Ruling E-27: R-3's gains go on top of the parent's composition: ew-theme-std-aim-v1 on an R-1 parent,
    ew-theme-aim-v2 on an ew-theme-v1 parent (Rulings E-27a, E-27b: the same within-theme gains and member cap, review
    F-10); both are fitter compositions. The v5 rule ew-theme-aim-v1 is refused in a v8 spec, naming v2 (E-27b)."""
    sys.path.insert(0, str(research_tree.REPO / "atx-impl" / "tools"))
    import fit_composition_weights as fcw
    doc = json.loads((authored_v8 / "r3-aim-gain.json").read_text(encoding="utf-8"))
    assert "requires" not in doc
    for parent, want in (("base-b0c.json", "ew-theme-aim-v2"), ("r1-comp-v8.json", "ew-theme-std-aim-v1")):
        path = tmp_path / f"r3-on-{parent}"
        path.write_text(json.dumps(dict(doc, parent=str(authored_v8 / parent))), encoding="utf-8")
        spec = RC.load_spec(path)
        assert RC.option_value(spec["fit"]["flags"], "--composition") == want and want in fcw.PRIOR_COMPOSITIONS
        assert want in fcw.AIM_RULES                                       # the fitter computes the aim records
    assert (RC.V5_AIM_RULE, RC.V8_AIM_RULE) == (fcw.AIM_RULE_ID, fcw.AIM_V2_RULE_ID)   # the fitter's ids (one record)
    v8 = RC.load_spec(V8 / "base-lo1.json")                                # verdict true: a v8 spec
    v5 = dict(v8, fit=dict(v8["fit"], flags=RS.apply_flags(v8["fit"]["flags"], {"--composition": "ew-theme-aim-v1"},
                                                            "fit.flags")))
    with pytest.raises(RC.CycleError, match="ew-theme-aim-v1 is the v5 R4' rule.*ew-theme-aim-v2") as e:
        RC.validate_spec(v5)
    assert e.value.code == RC.EXIT_USAGE
    with pytest.raises(RC.CycleError, match="ew-theme-aim-v2"):            # --protocol v8 makes a v8 spec too
        RC.validate_spec(dict(v5, verdict=False, summ=dict(v8["summ"], extra=["--protocol", "v8"])))
    RC.validate_spec(dict(v5, verdict=False, summ=dict(v8["summ"], extra=["--effective-n", "dirs"])))  # v7: as before


# review R6B-C-4: Ruling E-27b reads the argv the tools parse, in every spelling argparse accepts (allow_abbrev is on in
# the fitter and in nav_summ): "=", any unique prefix, a repeat (the last wins)
V5_AIM = "ew-theme-aim-v1"
COMPOSITION_SPELLINGS = {"pair": ["--composition", V5_AIM], "equals": [f"--composition={V5_AIM}"],
                         "abbreviation": ["--compo", V5_AIM], "abbreviation-equals": [f"--compo={V5_AIM}"],
                         "shortest": ["--c", V5_AIM], "repeat-last": ["--composition", "ew-theme-v1", "--composition",
                                                                      V5_AIM],
                         "repeat-first": ["--composition", V5_AIM, "--composition", "ew-theme-v1"]}
PROTOCOL_SPELLINGS = {"pair": ["--protocol", "v8"], "equals": ["--protocol=v8"], "abbreviation": ["--proto", "v8"],
                      "abbreviation-equals": ["--prot=v8"], "shortest": ["--pr", "v8"]}
FIT_REQUIRED = ["--library", "l", "--library-sha256", "0", "--train", "t", "--train-sha256", "0", "--orientations", "o",
                "--orientations-sha256", "0", "--runner-summary", "s", "--runner-summary-sha256", "0", "--screen",
                "v4-prior-v1", "--output", "w"]


def tool_parser(main) -> argparse.ArgumentParser:
    """The argparse parser a tool's main() builds, caught at its parse_args (nothing parsed or run)."""
    class Caught(Exception):
        pass
    seen = []

    def catch(self, args=None, namespace=None):
        seen.append(self)
        raise Caught
    with pytest.MonkeyPatch.context() as mp:
        mp.setattr(argparse.ArgumentParser, "parse_args", catch)
        with pytest.raises(Caught):
            main([])
    return seen[0]


def e27b_variant(doc: dict, composition: list | None = None, protocol: list | None = None) -> dict:
    """A v8 spec (base-lo1: verdict true) with fit.flags' --composition written as ``composition``; with ``protocol``
    the spec is v8 only through summ.extra's --protocol written that way (verdict false)."""
    doc = copy.deepcopy(doc)
    if composition:
        doc["fit"]["flags"] = RS.apply_flags(doc["fit"]["flags"], {"--composition": False}, "fit.flags") + composition
    if protocol:
        doc["verdict"] = False
        doc["summ"]["extra"] = RS.apply_flags(doc["summ"]["extra"], {"--protocol": False}, "summ.extra") + protocol
    return doc


def e27b_cases(doc: dict) -> list[tuple[str, dict]]:
    return ([(f"composition {n}", e27b_variant(doc, sp)) for n, sp in COMPOSITION_SPELLINGS.items()]
            + [(f"protocol {n}", e27b_variant(doc, ["--composition", V5_AIM], sp)) for n, sp in PROTOCOL_SPELLINGS.items()])


def test_e27b_reads_what_the_fitter_and_nav_summ_parse():
    """Every spelling of the cases is one the real parsers read as ew-theme-aim-v1 / --protocol v8 (so the guard must
    see it), and the guard reads it."""
    sys.path.insert(0, str(research_tree.REPO / "atx-impl" / "tools"))
    import fit_composition_weights as fcw
    import nav_summ as NS
    for name, sp in COMPOSITION_SPELLINGS.items():
        assert fcw.parse_args(FIT_REQUIRED + sp).composition == ("ew-theme-v1" if name == "repeat-first" else V5_AIM)
        assert V5_AIM in RC.argparse_values(sp, "--composition"), name   # a repeat with v1 anywhere is refused too
    parser = tool_parser(NS.main)
    for name, sp in PROTOCOL_SPELLINGS.items():
        assert parser.parse_args(sp).protocol == "v8" and RC.argparse_values(sp, "--protocol") == ["v8"], name
    with pytest.raises(SystemExit):                    # --p is ambiguous in nav_summ (--pbo, --psr, --pool): it fails
        parser.parse_args(["--p", "v8"])
    assert RC.argparse_values(["--p", "v8"], "--protocol") == ["v8"]                  # counted anyway (the safe side)
    assert RC.argparse_values(["--pool", "a", "--protocol", "v7", "--", "--protocol=v8"], "--protocol") == ["v7"]
    assert RC.argparse_values(["--psr", "--protocol"], "--protocol") == [None]
    assert RC.argparse_values(["--orientation", "prior", "--screen", "s"], "--composition") == []


def test_e27b_refuses_every_spelling_at_plan_run_and_lock(tmp_path, capsys):
    base = json.loads((V8 / "base-lo1.json").read_text(encoding="utf-8"))
    sp = tmp_path / "spec.json"
    for name, doc in e27b_cases(base):
        sp.write_text(json.dumps(doc), encoding="utf-8")
        for verb in ("plan", "run", "lock"):
            argv = [verb, str(sp), "--root", str(tmp_path), *(["--write"] if verb == "lock" else [])]
            assert RC.main(argv) == RC.EXIT_USAGE, (name, verb)
            assert "ew-theme-aim-v1 is the v5 R4' rule" in capsys.readouterr().err, (name, verb)
        assert json.loads(sp.read_text(encoding="utf-8")) == doc, name                  # lock --write wrote nothing
    # the cycle's own readers see one two-token pair: other spellings are refused (--protocol in every spec, a v8
    # spec's --composition); outside v8 the composition is free, as before
    for doc, needle in ((e27b_variant(base, ["--composition=ew-theme-v1"]), "spec fit.flags: write --composition once"),
                        (e27b_variant(base, ["--composition", "ew-theme-v1", "--composition", "ew-theme-v1"]),
                         "spec fit.flags: write --composition once"),
                        (e27b_variant(base, ["--composition", "ew-theme-v1"], ["--protocol=v7"]),
                         "spec summ.extra: write --protocol once")):
        with pytest.raises(RC.CycleError, match=needle) as e:
            RC.validate_spec(doc)
        assert e.value.code == RC.EXIT_USAGE
    v7 = e27b_variant(base, [f"--composition={V5_AIM}"], ["--effective-n", "dirs"])
    RC.validate_spec(v7)                                         # a v7 spec may fit the v5 rule, in any spelling
    RC.validate_spec(e27b_variant(base, ["--composition", "ew-theme-v1"]))              # the committed form


def test_e27b_refuses_every_spelling_at_add_alpha(tmp_path, capsys):
    """add-alpha loads the parent spec (and validates the child it derives from it) before any write."""
    root, parent = v8_root(tmp_path)
    parent_cell(root, parent)
    doc = json.loads(parent.read_text(encoding="utf-8"))
    probe = ("v8_probe", "rank(decay_linear((be / at_lag4), 21))")
    for name, bad in e27b_cases(doc):
        parent.write_text(json.dumps(bad, indent=2), encoding="utf-8")
        before = files_of(root)
        assert add(root, parent, *probe) == RC.EXIT_USAGE, name
        assert "ew-theme-aim-v1 is the v5 R4' rule" in capsys.readouterr().err, name
        assert files_of(root) == before, name                                           # nothing written
    parent.write_text(json.dumps(doc, indent=2), encoding="utf-8")
    assert add(root, parent, *probe) == RC.EXIT_OK                                      # the parent as committed


def test_r10_derives_its_rule_from_the_parent_and_runs_the_w_pass_at_3072(tmp_path, authored_v8):
    """R-10 (Ruling E-38, lane COMB2), Ruling E-44 (fix round 1) and Ruling E-45 (integration 6 part B): R-10 runs on the
    last accepted parent and is defined only on a standardised one (rerank-true theme_standardise, R-1 accepted); the
    fit flag derives the rule from it: ew-theme-std-v1 -> ic-shrink-v1, R-3 on R-1 (ew-theme-std-aim-v1) ->
    ic-shrink-aim-v1, both fitter compositions (the variant reads the aim gains). B0c (ew-theme-v1), R-3 on B0c
    (ew-theme-aim-v2) and any other composition refuse at load; no parent maps ew-theme-aim-v1 (E-27b). The w pass runs
    under Ruling E-28's 3,072 MiB (the template sets it, as R-1's chain has it); the u pass stays 2,560."""
    sys.path.insert(0, str(research_tree.REPO / "atx-impl" / "tools"))
    import fit_composition_weights as fcw
    doc = json.loads((authored_v8 / "r10.json").read_text(encoding="utf-8"))
    assert "requires" not in doc and doc["nominal_parent"] == "r1-comp-v8.json"
    assert doc["change"]["flags"]["fit"]["--composition"] == {"ew-theme-std-v1": "ic-shrink-v1",
                                                              "ew-theme-std-aim-v1": "ic-shrink-aim-v1"}
    assert {"ic-shrink-v1", "ic-shrink-aim-v1"} <= set(fcw.PRIOR_COMPOSITIONS)
    assert "ic-shrink-aim-v1" in fcw.AIM_RULES and "ic-shrink-v1" not in fcw.AIM_RULES
    r3_doc = json.loads((authored_v8 / "r3-aim-gain.json").read_text(encoding="utf-8"))
    parents = {}
    for name, grand in (("r3-on-r1", "r1-comp-v8.json"), ("r3-on-b0c", "base-b0c.json")):
        parents[name] = tmp_path / f"{name}.json"
        parents[name].write_text(json.dumps(dict(r3_doc, parent=str(authored_v8 / grand))), encoding="utf-8")
    cases = ((None, "ew-theme-std-v1", "ic-shrink-v1"),                         # nominal: R-1
             (str(parents["r3-on-r1"]), "ew-theme-std-aim-v1", "ic-shrink-aim-v1"))
    for parent, before in ((str(authored_v8 / "base-b0c.json"), "ew-theme-v1"),   # E-45: not standardised, refused
                           (str(parents["r3-on-b0c"]), "ew-theme-aim-v2")):
        assert RC.option_value(RC.load_spec(Path(parent))["fit"]["flags"], "--composition") == before
        path = tmp_path / f"r10-on-{Path(parent).stem}.json"
        path.write_text(json.dumps(dict(doc, parent=parent)), encoding="utf-8")
        with pytest.raises(RC.CycleError, match="maps the parent's value"):
            RC.load_spec(path)
    for k, (parent, before, want) in enumerate(cases):
        path = authored_v8 / "r10.json"
        if parent:
            path = tmp_path / f"r10-on-{Path(parent).stem}.json"
            path.write_text(json.dumps(dict(doc, parent=parent)), encoding="utf-8")
            assert RC.option_value(RC.load_spec(Path(parent))["fit"]["flags"], "--composition") == before
        spec = RC.load_spec(path)
        assert RC.option_value(spec["fit"]["flags"], "--composition") == want, parent
        root, planned = fake_root(tmp_path / f"root-{k}", spec)
        c = RC.Cycle(planned, RC.Resolver(root), spec_path=path, capabilities=T.CAPS)
        assert [c.phase_caps(p)["max_rss_mib"] for p in ("u", "w", "card", "nav")] == [2560, 3072, 2560, 1536], parent
        steps = {s.phase: s for s in c.steps()}
        ic = {p: steps[p].argv[steps[p].argv.index("--") + 1:] for p in ("u", "w")}
        assert (RC.option_value(ic["u"], "--max-memory-mib"), RC.option_value(ic["w"], "--max-memory-mib")) == (
            "2560", "3072"), parent
    v6 = tmp_path / "v6-parent.json"                                            # an unmapped composition is refused
    v6_doc = dict(r3_doc, parent=str(authored_v8 / "base-b0c.json"))
    v6_doc["change"] = dict(r3_doc["change"], flags={"fit": {"--composition": "ew-theme-v6"}})
    v6.write_text(json.dumps(v6_doc), encoding="utf-8")
    path = tmp_path / "r10-on-v6.json"
    path.write_text(json.dumps(dict(doc, parent=str(v6))), encoding="utf-8")
    with pytest.raises(RC.CycleError, match="maps the parent's value"):
        RC.load_spec(path)


def test_r11_appends_theme_resid_to_the_parents_fit(tmp_path, authored_v8):
    """v8 R-11 (lane ORTH): theme-resid-v1 keeps the parent's composition (its member weights and theme shares) and adds
    the fitter flag --theme-resid; on an R-1 parent the w pass inherits Ruling E-28's 3,072 MiB."""
    sys.path.insert(0, str(research_tree.REPO / "atx-impl" / "tools"))
    import fit_composition_weights as fcw
    doc = json.loads((authored_v8 / "r11.json").read_text(encoding="utf-8"))
    assert doc["nominal_parent"] == "r1-comp-v8.json" and "requires" not in doc
    # Rulings E-44, PM4-4: R-10 and R-11 record rule 5 AND R-1's mechanical criterion, in R-1's words; Ruling PM5-11:
    # the criterion names its statistic (executed turnover per unit gross on S2), never "planned" turnover
    r1_criterion = ("paired S2 net dSR > 0 against the parent AND mechanics AND turnover per unit gross (executed: "
                    "tau_gmv_mean / mean_gross_leverage_all_rows, S2) not higher than the parent")
    r10_doc = json.loads((authored_v8 / "r10.json").read_text(encoding="utf-8"))
    r1_doc = json.loads((authored_v8 / "r1-comp-v8.json").read_text(encoding="utf-8"))
    for d in (doc, r10_doc, r1_doc):
        assert r1_criterion in d["description"] and "planned turnover" not in d["description"], d["name"]
        assert "PM5-11" in d["description"], d["name"]
    assert all("composition-cell criterion of R-1, plan 12.1" in d["description"] for d in (doc, r10_doc))
    assert "gates nothing" not in doc["description"] and "E-45" in doc["description"]
    path = tmp_path / "r11-on-r1.json"
    path.write_text(json.dumps(dict(doc, parent=str(authored_v8 / "r1-comp-v8.json"))), encoding="utf-8")
    spec = RC.load_spec(path)
    assert RC.option_value(spec["fit"]["flags"], "--composition") == "ew-theme-std-v1"
    assert RC.option_value(spec["fit"]["flags"], "--theme-resid") == "theme-resid-v1"
    required = ["--library", "l", "--library-sha256", "0", "--train", "t", "--train-sha256", "0", "--orientations", "o",
                "--orientations-sha256", "0", "--runner-summary", "s", "--runner-summary-sha256", "0", "--screen",
                "v4-prior-v1", "--output", "w"]
    assert fcw.parse_args(required + ["--theme-resid", "theme-resid-v1"]).theme_resid == "theme-resid-v1"  # the fitter's
    assert fcw.parse_args(required).theme_resid is None                                                    # flag absent
    root, spec = fake_root(tmp_path / "root", spec)
    c = RC.Cycle(spec, RC.Resolver(root), spec_path=path, capabilities=T.CAPS)
    assert [c.phase_caps(p)["max_rss_mib"] for p in ("u", "w", "card", "nav")] == [2560, 3072, 2560, 1536]


def test_r11_checks_its_re_fit_against_the_parent_cells_weights(tmp_path, authored_v8):
    """Finding R6B-O-5: the cell whose template adds --theme-resid derives reference_resid_parent = the parent's fit
    composition_weights.json, and its single-window fit step passes it with its pin (--theme-resid-parent,
    --theme-resid-parent-sha256; the fitter refuses a re-fit that is not that file plus the block). A pooled (era) fit,
    a template child of the cell and an add-alpha child never carry it (it is derived, and add-alpha re-derives every
    reference_ input); no other template derives it."""
    sys.path.insert(0, str(research_tree.REPO / "atx-impl" / "tools"))
    import fit_composition_weights as fcw
    doc = json.loads((authored_v8 / "r11.json").read_text(encoding="utf-8"))
    path = tmp_path / "r11-on-r1.json"
    path.write_text(json.dumps(dict(doc, parent=str(authored_v8 / "r1-comp-v8.json"))), encoding="utf-8")
    spec = RC.load_spec(path)
    r1 = RC.load_spec(authored_v8 / "r1-comp-v8.json")
    want = f"{r1['fit']['output']}/composition_weights.json"
    assert spec["inputs"]["reference_resid_parent"] == {"path": want, "sha256": None}     # derived, as authored
    root, planned = fake_root(tmp_path / "root", spec)
    c = RC.Cycle(planned, RC.Resolver(root), spec_path=path, capabilities=T.CAPS)
    fit = next(s for s in c.steps() if s.phase == "fit")
    tool = fit.argv[fit.argv.index("--") + 1:]
    assert RC.option_value(tool, "--theme-resid") == "theme-resid-v1"
    assert RC.option_value(tool, "--theme-resid-parent") == want
    assert RC.option_value(tool, "--theme-resid-parent-sha256") == RC.sha256_file(root / want)
    assert want in [fit.argv[k + 1] for k, x in enumerate(fit.argv[:fit.argv.index("--")]) if x == "--bind"]
    args = fcw.parse_args(tool[2:])                                   # the fitter parses the argv as built
    assert (args.theme_resid_parent, args.theme_resid_parent_sha256) == (want, RC.sha256_file(root / want))
    c.fit_pool = (["--era-id", "E3"], [])                            # a pooled history fit: no parent check
    pooled = next(s for s in c.steps() if s.phase == "fit")
    assert "--theme-resid-parent" not in pooled.argv and "--theme-resid" in pooled.argv
    child = tmp_path / "r11-child.json"                              # a NAV-only child of the cell
    r4 = json.loads((authored_v8 / "r4-hold-band.json").read_text(encoding="utf-8"))
    child.write_text(json.dumps(dict(r4, parent=str(path))), encoding="utf-8")
    inherited = RC.load_spec(child)
    assert "reference_resid_parent" not in inherited["inputs"]
    assert RC.option_value(inherited["fit"]["flags"], "--theme-resid") == "theme-resid-v1"
    assert all("reference_resid_parent" not in RC.load_spec(d / n)["inputs"]     # as authored and as root set them
               for d in (authored_v8, V8) for n in V8_SPECS if n != "r11.json")
    # add-alpha (R-12 on an accepted R-11) carries no reference_ input of its parent over (derive_spec re-derives them)
    assert 'not k.startswith("reference_")' in inspect.getsource(RA.derive_spec)


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


PROBE = "v8probe"     # the library name of a wave made on stand-ins: no committed atx-impl file is named after it


def files_of(root: Path) -> list[str]:
    return sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file())


def v71_seed_registry() -> bytes:
    """The committed registry's v7.1 seed: its first alphas, fields and themes, those of library v7.1 (Ruling PM6-10:
    the seed is a prefix, root's registrations are appends; test_generate_library.py checks both). Each add-alpha wave
    here starts from v7.1, whatever root has registered since (library v8.0's 15 alphas for R-2)."""
    reg = json.loads((T.STRATEGIES / "alphas" / "registry.json").read_text(encoding="utf-8"))
    lib = json.loads((T.STRATEGIES / "fund_industry_ic_v71.json").read_text(encoding="utf-8"))
    names, themes = [f["name"] for f in lib["fields"]], [f["id"] for f in lib["families"]]
    seed = dict(reg, alphas=reg["alphas"][:len(V71_IDS)], fields={n: reg["fields"][n] for n in names},
                themes={t: reg["themes"][t] for t in themes})
    assert [a["id"] for a in seed["alphas"]] == V71_IDS and list(reg["fields"])[:len(names)] == names
    assert list(reg["themes"])[:len(themes)] == themes
    return RA.G.encode_data(seed)


def v8_root(tmp_path: Path) -> tuple[Path, Path]:
    """A root with base-lo1.json (its tools re-pointed at the fake ones of test_research_cycle.py, the IC exe at the
    fake --plan-only one), locked on stand-ins of its inputs (fake_root's unlocked copy, re-pinned to the stand-ins'
    digests by `lock --write`), the committed registry's v7.1 seed (v71_seed_registry) and library v7.1."""
    doc = json.loads((V8 / "base-lo1.json").read_text(encoding="utf-8"))
    doc.update(python=sys.executable, exes={"ic": "bin/ic.cmd", "nav": "bin/nav.exe"})
    doc["runner"]["script"] = "scripts/runner.py"
    for section in ("fit", "card", "monitor", "summ"):
        doc[section]["script"] = f"scripts/{section}.py"
    root, doc = fake_root(tmp_path, doc)
    s = root / "atx-impl" / "strategies"
    for rel in ("libraries/v71.json", "fund_industry_ic_v71.json", "fund_industry_ic_v71.recipe.json"):
        (s / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(T.STRATEGIES / rel, s / rel)
    (s / "alphas").mkdir(parents=True)
    (s / "alphas" / "registry.json").write_bytes(v71_seed_registry())
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


def add(root: Path, parent_spec: Path, cid: str, dsl: str, *extra: str, name: str = "v80") -> int:
    like = T.v71_entry("sue")
    return RC.main(["add-alpha", "--id", cid, "--dsl", dsl, "--theme", like["theme"], "--tier", like["tier"],
                    "--prior-sign", "1", "--citation", f"test citation for {cid}", "--origin", "prior", "--parent", "v71",
                    "--name", name, "--parent-spec", str(parent_spec), "--root", str(root), *extra])


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
    assert spec["card"]["flags"] == base["card"]["flags"] and spec["runner"] == dict(   # + the marginal cap (5f)
        base["runner"], phases=dict(base["runner"]["phases"], marginal=RA.MARGINAL_CAPS))
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


def test_add_alpha_on_a_v8_template_removes_replaces_rescreens_and_records_exceptions(tmp_path, capsys, authored_v8):
    root, base = v8_root(tmp_path)
    tpl = json.loads((authored_v8 / "r4-hold-band.json").read_text(encoding="utf-8"))
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
    remove3 = ("--removes", "sue", "--removes", "droe", "--removes", "chtax")          # draft E3
    shutil.copyfile(authored_v8 / "base-b0c.json", tp.parent / "base-b0c.json")      # r4's nominal parent
    before = files_of(root)
    ruling = ("--exception", "max_extra_fields=8", "--exception-basis", "Ruling R2-b")
    assert add(root, tp, "earn_probe", earn, *remove3, *ruling) == RC.EXIT_USAGE      # template parent null
    assert "template parent is null" in capsys.readouterr().err and files_of(root) == before
    tpl["parent"] = "base-lo1.json"
    tp.write_text(json.dumps(tpl), encoding="utf-8")
    outs = parent_cell(root, tp)
    before = files_of(root)
    q5 = re.sub(r"\bgrp_ff12\b", "grp_ff12f49", T.v71_entry("q5_eg")["dsl"])
    gpa = re.sub(r"\bgrp_ff12\b", "grp_ff12f49", T.v71_entry("gpa")["dsl"])
    assert add(root, tp, "earn_probe", earn, *remove3) == RC.EXIT_USAGE               # 7 > 5 extra fields: K1
    for cid, dsl, *bad in (("earn_probe", earn, *remove3[:2], "--removes", "nope"),
                           ("earn_probe", earn, *remove3[:2], "--rescreen"),
                           ("earn_probe", earn, *remove3[:2], "--replaces", "gpa"),
                           ("earn_probe", earn, *remove3[:2], "--exception", "max_extra_fields=8"),
                           ("earn_probe", earn, *remove3[:2], "--exception", "max_nodes=8", "--exception-basis", "x"),
                           ("q5_eg_f49", q5, "--replaces", "nope", "--rescreen", "--fields", F49),
                           ("q5_eg_f49", q5, "--replaces", "q5_eg", "--replaces", "gpa", "--rescreen")):
        assert add(root, tp, cid, dsl, *bad) == RC.EXIT_USAGE, bad
    assert files_of(root) == before                                                  # nothing written
    assert add(root, tp, "earn_probe", earn, *remove3, *ruling) == RC.EXIT_OK
    assert add(root, tp, "q5_eg_f49", q5, "--replaces", "q5_eg", "--rescreen", "--fields", F49) == RC.EXIT_OK
    assert add(root, tp, "gpa_f49", gpa, "--replaces", "gpa", "--rescreen") == RC.EXIT_OK   # --fields is sticky
    assert add(root, tp, "gpa_f49", gpa, "--replaces", "gpa", "--rescreen") == RC.EXIT_OK   # identical: no-op
    assert add(root, tp, "earn_probe", earn, *remove3, *ruling) == RC.EXIT_OK        # identical: no-op
    lib = json.loads((s / "libraries" / "v80.json").read_text(encoding="utf-8"))
    want = [{"q5_eg": "q5_eg_f49", "gpa": "gpa_f49"}.get(m, m) for m in V71_IDS if m not in ("sue", "droe", "chtax")]
    assert lib["members"] == want + ["earn_probe"]                                   # E3 appended; E4 in place
    assert lib["rescreens"] == ["q5_eg_f49", "gpa_f49"]
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


def test_add_alpha_child_of_a_labelled_parent_labels_its_ref_and_nav(tmp_path):
    """Review F-8: the child of a labelled parent (inputs.label_role, e.g. B0c) reproduces the parent's NAV in its ref
    phase and compares it byte for byte with the parent's labelled S2 daily CSV; so ref (and nav) carry --label-role and
    --label-role-sha256 with the parent's own pin, and a label role changed since the parent ran stops `lock`."""
    root, parent = v8_root(tmp_path)
    label = "build-equity/train-2020-2023-lo1-dlret"
    (root / label).mkdir(parents=True)
    (root / label / "manifest.json").write_text('{"labels": "delisting returns"}')
    doc = json.loads(parent.read_text(encoding="utf-8"))
    doc["inputs"]["label_role"] = {"dir": label, "path": f"{label}/manifest.json", "sha256": None}
    parent.write_text(json.dumps(doc, indent=2), encoding="utf-8")
    assert RC.main(["lock", str(parent), "--root", str(root), "--write"]) == RC.EXIT_OK
    pin = json.loads(parent.read_text(encoding="utf-8"))["inputs"]["label_role"]["sha256"]
    assert pin == RC.sha256_file(root / label / "manifest.json")
    parent_cell(root, parent)
    f2 = "build-equity/train-2020-2023-lo1-fields-v9-b"                 # other fields: the ref is not skipped
    (root / f2).mkdir(parents=True)
    (root / f2 / "manifest.json").write_text(json.dumps({"fields": [{"name": n} for n in doc["fields"]["list"]],
                                                         "build": "b"}))
    probe = ("v8_probe", "rank(decay_linear((be / at_lag4), 21))", "--fields", f2)
    (root / label / "manifest.json").write_text('{"labels": "rebuilt"}')   # not the label the parent's NAV used
    assert add(root, parent, *probe) == RC.EXIT_PIN                     # the child keeps the parent's pin: unlocked
    (root / label / "manifest.json").write_text('{"labels": "delisting returns"}')
    assert add(root, parent, *probe) == RC.EXIT_OK                       # the same wave, now locked
    sp = root / SPECS / "lib-v80.json"
    spec = RC.load_spec(sp)
    assert spec["inputs"]["label_role"] == {"dir": label, "path": f"{label}/manifest.json", "sha256": pin}
    assert spec["compare"][0]["name"] == "ref-s2-daily" and spec["compare"][0]["a"] == "{input:reference_daily}"
    steps = {st.phase: st for st in RC.Cycle(spec, RC.Resolver(root), spec_path=sp, capabilities=T.CAPS).steps()}
    assert steps["ref"].state == "pending"                              # the parent's fields differ: ref runs
    want = ["--label-role", f"{label}/manifest.json", "--label-role-sha256", pin]
    for phase in ("ref", "nav"):
        argv = steps[phase].argv
        assert argv[-4:] == want and ["--bind", f"{label}/manifest.json"] == argv[argv.index("--") - 2:argv.index("--")]
    (root / label / "manifest.json").write_text('{"labels": "rebuilt"}')   # not the label the parent's NAV used
    with pytest.raises(RC.CycleError, match="label_role pin") as e:
        RC.lock(sp, root)
    assert e.value.code == RC.EXIT_PIN


# ------------------------------------------------------------------ cache gc on the v8 specs (A2 follow-up, task 2)
def test_cache_gc_apply_with_the_v8_specs_keeps_the_shared_stores(tmp_path, monkeypatch):
    """C-1: the v8 specs name the store base build-equity/fit-work; the fitter, the card and the monitor extend it with
    <role sha16>-<window id> themselves (the monitor reads every window of its role): --apply deletes none of it."""
    import research_gc
    monkeypatch.setattr(RC, "window_id", lambda: "research-window-v2")
    root = tmp_path / "root"
    specs = [V8 / n for n in V8_SPECS + GENERATED]                                   # authored and add-alpha's
    named ={RC.load_spec(p)["fit"]["work_dir"] for p in specs} | {RC.load_spec(p)["ic"]["cache"] for p in specs}
    assert "build-equity/fit-work" in named and len(named) == 3                       # + the lo1 and lo3 caches
    shared = [f"fit-work/{'0123456789abcdef'}-research-window-v2", f"fit-work/{'fedcba9876543210'}-research-window-v1",
              "mega-candidate-cache-v8-lo1", "mega-candidate-cache-v8-lo3"]
    stale = ["mega-candidate-cache-v71", "candidate-cache/0123456789abcdef-research-window-v2", "mega-fit-work-v71"]
    for rel in shared + stale:
        (root / "build-equity" / rel / ("a" * 64)).mkdir(parents=True)
        (root / "build-equity" / rel / ("a" * 64) / "x.f64").write_bytes(b"\0" * 2048)
    (root / "build-equity" / "fit-work" / "0123456789abcdef-research-window-v2" / "context").mkdir()
    log = []
    rep = research_gc.gc(specs, root, ["build-equity"], apply=True, log=log.append)
    assert sorted(rep["keep"]) == sorted(f"build-equity/{r}" for r in shared)
    assert sorted(rep["deleted"]) == sorted(f"build-equity/{r}" for r in stale)
    assert all((root / "build-equity" / r / ("a" * 64) / "x.f64").is_file() for r in shared)   # nothing of the stores
    assert not any((root / "build-equity" / r).exists() for r in stale)
    assert any(x.startswith("keep  build-equity/fit-work/fedcba9876543210-research-window-v1") and
               "(store base build-equity/fit-work named by " in x and "v8-b0a-lo1" in x for x in log), log


def test_x_theme_erc_appends_its_flag_to_the_parents_fit(tmp_path, authored_v8):
    """v8 X (lane XCOMB): theme-erc-v1 keeps the parent's composition (its within-theme shares) and adds the fitter flag
    --theme-erc; the description registers the rule's constants, the gross matching and R-1's criterion; on an R-1
    parent the w pass inherits Ruling E-28's 3,072 MiB."""
    sys.path.insert(0, str(research_tree.REPO / "atx-impl" / "tools"))
    import fit_composition_weights as fcw
    doc = json.loads((authored_v8 / "x-theme-erc.json").read_text(encoding="utf-8"))
    assert doc["nominal_parent"] == "r1-comp-v8.json" and doc["parent"] is None and "requires" not in doc
    for text in ("10000 sweeps", "1e-10", "1/(2T)", "PM6-6", "tau_gmv_mean / mean_gross_leverage_all_rows",
                 "paired S2 net dSR > 0 against the parent AND mechanics"):
        assert text in doc["description"], text
    path = tmp_path / "x-theme-erc-on-r1.json"
    path.write_text(json.dumps(dict(doc, parent=str(authored_v8 / "r1-comp-v8.json"))), encoding="utf-8")
    spec = RC.load_spec(path)
    assert RC.option_value(spec["fit"]["flags"], "--composition") == "ew-theme-std-v1"
    assert RC.option_value(spec["fit"]["flags"], "--theme-erc") == "theme-erc-v1"
    required = ["--library", "l", "--library-sha256", "0", "--train", "t", "--train-sha256", "0", "--orientations", "o",
                "--orientations-sha256", "0", "--runner-summary", "s", "--runner-summary-sha256", "0", "--screen",
                "v4-prior-v1", "--output", "w"]
    assert fcw.parse_args(required + ["--theme-erc", "theme-erc-v1"]).theme_erc == "theme-erc-v1"  # the fitter's flag
    assert fcw.parse_args(required).theme_erc is None                                                # flag absent
    root, spec = fake_root(tmp_path / "root", spec)
    c = RC.Cycle(spec, RC.Resolver(root), spec_path=path, capabilities=T.CAPS)
    assert [c.phase_caps(p)["max_rss_mib"] for p in ("u", "w", "card", "nav")] == [2560, 3072, 2560, 1536]


def test_x_inv_vol_appends_its_flag_to_the_parents_nav(tmp_path, authored_v8):
    """v8 X (lane XCOMB): inv-vol-v1 is a NAV-only change, nav --vol-scale inv-vol-v1 after the parent's flags (the
    4x NAV report --capacity-curve is the parent's); the description registers the rule's constants, the gross
    matching and the capacity criterion."""
    doc = json.loads((authored_v8 / "x-inv-vol.json").read_text(encoding="utf-8"))
    assert doc["nominal_parent"] == "base-b0c.json" and doc["parent"] is None and "requires" not in doc
    for text in (".25 x median", "session d + 1", "PM6-6", "net Sharpe at 4x NAV", "cost_bps_traded",
                 "paired S2 net dSR > 0 against the parent AND mechanics", "recipe pin mismatch"):
        assert text in doc["description"], text
    path = tmp_path / "x-inv-vol-on-b0c.json"
    path.write_text(json.dumps(dict(doc, parent=str(authored_v8 / "base-b0c.json"))), encoding="utf-8")
    spec = RC.load_spec(path)
    parent = RC.load_spec(authored_v8 / "base-b0c.json")
    assert spec["nav"]["flags"] == parent["nav"]["flags"] + ["--vol-scale", "inv-vol-v1"]
    assert "--capacity-curve" in spec["nav"]["flags"]
    assert spec["fit"]["flags"] == parent["fit"]["flags"] and spec["fit"]["output"] == parent["fit"]["output"]
