"""The wave manifest v2 kinds (contract K-P9-9, P9 lane E2; wave_kinds.py, wave_stage_kinds.py) and the committed kinds
fixtures (scripts/specs/p9/kinds/, gate G-P2). Synthetic worlds only (wave_fixture.py's fake root and executor).

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_kinds.py

  test_the_ten_fixtures_load_and_cover_the_kinds      every fixture validates; the 8 K-P9-9 kinds and the 10 OR
                                                      section 6 rows are covered; template pins are the files' SHA-256
  test_v1_manifests_keep_their_validation             v8 wave manifests: the problems of before (none, or the head's)
  test_v2_refusals                                    each kind's block, cross-kind blocks, F-8 raw rule flags
  test_schema_subset                                  params_schema keywords; an unknown keyword is refused
  test_rule_flags_from_the_tables                     composition flags (COMPOSITION_FLAGS), NAV x-flags, both / neither
                                                      tables, bad params; D1's committed table refuses manifest params
                                                      until D2's schema (D1-PSCHEMA)
  test_every_fixture_plans_in_a_fake_world            `wave plan` exits 0 on each fixture re-rooted into the fake world
  test_rule_kinds_write_the_derived_constants         rule (IC and NAV rows), horizon, leverage without a template: the
                                                      cell the spec stage writes carries the derived flags and names
  test_field_wave_builds_then_registers_on_the_build  the register stage builds the fields dir, add-alpha uses it
  test_role_wave_builds_role_and_fields               the cell's inputs.role and fields dir are the builds'
  test_walk_forward_is_schema_only                    plan prints it; preflight refuses to run it
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import research_cycle as RC  # noqa: E402
import research_wave  # noqa: E402
import wave_fixture as F  # noqa: E402
import wave_kinds as WK  # noqa: E402
import wave_manifest as WM  # noqa: E402

REPO = HERE.parents[1]
KINDS_DIR = REPO / "scripts" / "specs" / "p9" / "kinds"
IC_FIXTURE = REPO / "atx-impl" / "tests" / "fixtures" / "composition_rules_list.json"   # D1's K-P9-6 table, pinned
OR6_ROWS = {"new-signal.json": "add", "replaced-signal.json": "replace", "new-field.json": "field",
            "new-combination-rule.json": "rule", "new-nav-rule.json": "rule", "leverage-change.json": "leverage",
            "new-cost-model.json": "rule", "new-universe.json": "role", "new-horizon.json": "horizon",
            "walk-forward.json": "walk-forward"}
SCHEMA_ONLY_FIXTURES = ("new-cost-model.json", "walk-forward.json")      # wave 3: lanes C3 and D3


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def nav_row(rid: str, flag: str, properties: dict, required: list, incompatible: list) -> dict:
    """A K-P9-7 row in lane C1's shape (strategy_risk_target.cpp rule_row: x-flag on the rule and on every param)."""
    return {"id": rid, "kind": "leverage", "x-flag": flag, "incompatible": incompatible,
            "params_schema": {"type": "object", "additionalProperties": False, "required": required,
                              "properties": properties}}


LEVERAGE = {"type": "number", "x-flag": "--aim-leverage", "minimum": 1, "maximum": 2, "default": 1}
STORE = {"risk_model": {"type": "string", "x-flag": "--risk-model"},
         "risk_model_sha256": {"type": "string", "x-flag": "--risk-model-sha256", "pattern": "^[0-9a-f]{64}$"}}
NAV_TABLE = {"schema": WK.NAV_RULES, "capabilities": ["leverage-rule-per-book"], "rules": [
    nav_row("fixed-v1", "--aim-leverage", {"aim_leverage": LEVERAGE}, ["aim_leverage"],
            ["vol-target-v1", "risk-target-v1"]),
    nav_row("vol-target-v1", "--vol-target vol-target-v1", dict(STORE, aim_leverage=LEVERAGE),
            ["aim_leverage", "risk_model", "risk_model_sha256"], ["risk-target-v1"]),
    nav_row("risk-target-v1", "--risk-target S", dict(STORE, aim_leverage=LEVERAGE, sigma_star={
        "type": "number", "x-flag": "--risk-target", "exclusiveMinimum": 0, "maximum": 1}),
            ["aim_leverage", "sigma_star", "risk_model", "risk_model_sha256"], ["vol-target-v1"])]}


def ic_table(manifest_params: bool = True) -> dict:
    """D1's committed K-P9-6 table; ``manifest_params`` replaces each params_schema with the shape ruling D1-PSCHEMA
    asks of D2 (the wave manifest's params: here, none but the row's own rule key)."""
    doc = json.loads(IC_FIXTURE.read_text(encoding="utf-8"))
    if manifest_params:
        for row in doc["rules"]:
            row["params_schema"] = {"type": "object", "additionalProperties": False,
                                    "properties": {"rule": {"const": row["id"]}}}
    return doc


class KindsFake(F.FakeCycle):
    """wave_fixture's executor plus the exes' rule listings (K-P9-6 / K-P9-7) and the field and role builders."""

    def __init__(self, root: Path, *, ic=None, nav=None, **kw):
        super().__init__(root, {}, **kw)
        self.ic, self.nav = ic_table() if ic is None else ic, NAV_TABLE if nav is None else nav

    def __call__(self, argv, root, env=None):
        if list(argv[-2:]) == list(WK.LIST_RULES):
            self.calls.append(list(argv))
            table = self.nav if argv[1:2] == ["nav"] else self.ic
            return subprocess.CompletedProcess(argv, 0 if table else 2, json.dumps(table) if table else "", "")
        if argv[0] != "git" and Path(argv[1]).name == "run_bounded_research.py":
            cmd = argv[argv.index("--") + 1:]
            if Path(cmd[1]).name in (Path(WK.FIELD_BUILDER).name, Path(WK.ROLE_BUILDER).name):
                return self.build(argv, cmd)
        return super().__call__(argv, root, env)

    def build(self, argv: list[str], cmd: list[str]):
        self.calls.append(list(argv))
        run_dir = argv[argv.index("--output") + 1]
        if Path(cmd[1]).name == Path(WK.FIELD_BUILDER).name:
            out = cmd[cmd.index("--output") + 1]
            names = cmd[cmd.index("--fields") + 1].split(",")
            F.write_json(self.root, f"{out}/manifest.json", {"status": "complete", "seal": {"exclusive_end":
                                                                                            "2024-01-01"},
                                                              "fields": [{"name": n} for n in names]})
        else:
            F.write_json(self.root, f"{cmd[cmd.index('--out') + 1]}/manifest.json", {"dates": 300})
        F.write_json(self.root, f"{run_dir}/receipt.json", {"outcome": "completed", "exit_code": 0})
        return self.ok(argv)


def load_fixture(name: str) -> dict:
    return json.loads((KINDS_DIR / name).read_text(encoding="utf-8"))


def rerooted(m: dict, root: Path) -> dict:
    """A tiny-world fixture on the fake world's paths: parent, library, fields pin, ledger, N and out_dir."""
    out = json.loads(json.dumps(m))
    out["parent"] = {"spec": F.PARENT, "library": "p0"}
    out["fields"] = {"dir": F.FIELDS, "manifest_sha256": sha((root / F.FIELDS / "manifest.json").read_bytes())}
    out["ledger"], out["expect"], out["out_dir"] = F.LEDGER, {"n_before": 2}, f"out/waves/{m['wave']}"
    return out


def world(tmp_path: Path, m: dict) -> Path:
    """The fake root with manifest ``m`` (re-rooted) and the fixture templates committed."""
    root = F.build(tmp_path)
    for t in (KINDS_DIR / "templates").glob("*.json"):
        dest = root / t.relative_to(REPO)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(t.read_bytes())
    F.write_json(root, F.MANIFEST, rerooted(m, root))
    F.git(root, "add", "-A")
    F.git(root, "commit", "-q", "-m", "v2 manifest")
    return root


def wave(root: Path, fake, *args: str) -> tuple[int, list[str]]:
    log: list[str] = []
    code = research_wave.main([*args[:1], F.MANIFEST, "--root", str(root), *args[1:]], executor=fake, log=log.append)
    return code, log


# ------------------------------------------------------------------ the fixtures as data
def test_the_ten_fixtures_load_and_cover_the_kinds():
    names = sorted(p.name for p in KINDS_DIR.glob("*.json"))
    assert names == sorted(OR6_ROWS)
    for name, kind in OR6_ROWS.items():
        m = load_fixture(name)
        assert m["schema"] == WK.SCHEMA_V2 and m["kind"] == kind and WM.validate(m) == [], name
        rc = m.get("rule_cell")
        if rc:
            assert sha((REPO / rc["template"]).read_bytes()) == rc["template_sha256"], name
            assert b"\r" not in (REPO / rc["template"]).read_bytes()
    assert set(OR6_ROWS.values()) == set(WK.KINDS)                          # all 8 kinds of K-P9-9
    assert sum(name not in SCHEMA_ONLY_FIXTURES for name in OR6_ROWS) == 8   # 8 runnable + 2 schema only
    assert load_fixture("walk-forward.json")["kind"] in WK.SCHEMA_ONLY


def test_v1_manifests_keep_their_validation():
    for p in sorted((REPO / "scripts" / "specs" / "v8" / "waves").glob("*.json")):
        got = WM.validate(json.loads(p.read_text(encoding="utf-8")))
        assert got == ([] if p.name != "y-s.head.json" else
                       ["a wave has exactly one of candidates (a library wave) and rule_cell (a rule wave)"]), p.name


@pytest.mark.parametrize("name, edit, needle", [
    ("new-signal.json", lambda m: m.update(kind="horizon"), "kind horizon is a rule wave"),
    ("new-signal.json", lambda m: m.update(kind="bogus"), "kind must be one of"),
    ("new-signal.json", lambda m: m.update(rules=[{"name": "x-v1"}]), "rules is the block of kind rule"),
    ("replaced-signal.json", lambda m: m["candidates"][0].update(kind="add", replaces=[], rescreen=False),
     "one or more candidates of kind replace"),
    ("new-combination-rule.json", lambda m: m.pop("rules"), "kind rule needs its block rules"),
    ("new-combination-rule.json", lambda m: m["rule_cell"].update(constants={"flags": {"fit": {
        "--composition": "ew-theme-std-v1"}}}), "review F-8"),
    ("new-combination-rule.json", lambda m: m.update(rules=[{"name": "A B"}]), "rules[0] must be"),
    ("leverage-change.json", lambda m: m.update(gross_match="pm6-6"), 'gross_match must be "none"'),
    ("leverage-change.json", lambda m: m["leverage"].update(value="2"), ""),
    ("leverage-change.json", lambda m: m["leverage"].update(value=2.0), "leverage must be"),
    ("new-horizon.json", lambda m: m["horizon"].update(days=7), "horizon must be"),
    ("new-horizon.json", lambda m: m["horizon"].update(flags={}), "horizon must be"),
    ("new-field.json", lambda m: m["field"].update(output="scripts/fields-x"), "inside the code pathspec"),
    ("new-field.json", lambda m: m["field"].update(builder_args=["--reuse", "x"]), "may not set ['--reuse']"),
    ("new-field.json", lambda m: m["field"].update(seconds=601), "seconds"),
    ("new-universe.json", lambda m: m["role"].update(builder_args=["role", "--out", "x"]), "may not set ['--out']"),
    ("new-universe.json", lambda m: m["role"].update(builder_args=["project"]), 'verb "role"'),
    ("walk-forward.json", lambda m: m["walk_forward"].update(lag=1), "walk_forward must be"),
    ("walk-forward.json", lambda m: m.update(rule_cell={"template": "t.json", "template_sha256": "0" * 64}),
     "has no rule_cell"),
])
def test_v2_refusals(name, edit, needle):
    m = load_fixture(name)
    edit(m)
    got = "; ".join(WM.validate(m))
    assert (needle in got and got) if needle else got == "", got


def test_a_v1_manifest_may_not_carry_a_kind():
    m = F.manifest()
    m["fields"]["manifest_sha256"] = "0" * 64
    assert WM.validate(m) == []
    assert "unknown ['kind']" in "; ".join(WM.validate(dict(m, kind="add")))


# ------------------------------------------------------------------ the rule tables
def test_schema_subset():
    s = {"type": "object", "required": ["a"], "additionalProperties": False,
         "properties": {"a": {"type": "integer", "minimum": 1}, "b": {"type": "array", "items": {"type": "string"},
                                                                       "maxItems": 2}}}
    assert WK.schema_problems({"a": 2, "b": ["x"]}, s, "p") == []
    assert WK.schema_problems({"a": 0}, s, "p") == ["p.a: 0 violates minimum 1"]
    assert WK.schema_problems({"a": True}, s, "p") == ["p.a: True is not of type integer"]
    assert WK.schema_problems({"a": 1, "c": 1}, s, "p") == ["p: unknown key 'c'"]
    assert WK.schema_problems({"a": 1, "b": ["x", "y", "z"]}, s, "p") == ["p.b: 3 items (allowed 0..2)"]
    assert "not supported" in WK.schema_problems({}, {"oneOf": []}, "p")[0]           # refused, never ignored
    for row in json.loads(IC_FIXTURE.read_text(encoding="utf-8"))["rules"]:            # D1's schemas use the subset
        assert not [p for p in WK.schema_problems({}, row["params_schema"], row["id"]) if "not supported" in p]
    for row in NAV_TABLE["rules"]:
        assert not [p for p in WK.schema_problems({}, row["params_schema"], row["id"]) if "not supported" in p]


def test_rule_flags_from_the_tables():
    tables = {"ic": ic_table(), "nav": NAV_TABLE}
    flags, problems = WK.rule_flags([{"name": "two-speed-v1"}, {"name": "theme-erc-v1", "params": {}}], tables)
    assert problems == [] and flags == {"fit": {"--two-speed": "two-speed-v1", "--theme-erc": "theme-erc-v1"},
                                        "nav": {"--two-speed": "two-speed-v1"}}
    flags, problems = WK.rule_flags([{"name": "fixed-v1", "params": {"aim_leverage": 1.5}}], tables)
    assert problems == [] and flags == {"nav": {"--aim-leverage": "1.5"}}
    store = {"risk_model": "risk/r.atxcov", "risk_model_sha256": "a" * 64, "aim_leverage": 1}
    assert WK.rule_flags([{"name": "vol-target-v1", "params": store}], tables) == (
        {"nav": {"--risk-model": "risk/r.atxcov", "--risk-model-sha256": "a" * 64, "--aim-leverage": "1",
                 "--vol-target": "vol-target-v1"}}, [])
    flags, problems = WK.rule_flags([{"name": "risk-target-v1", "params": dict(store, sigma_star=0.1)}], tables)
    assert problems == [] and flags["nav"]["--risk-target"] == "0.1"
    assert WK.rule_flags([{"name": "fixed-v1", "params": {"aim_leverage": 3}}], tables)[1] == \
        ["rules.fixed-v1.aim_leverage: 3 violates maximum 2"]
    assert "in neither exe's rule table" in WK.rule_flags([{"name": "cost-sqrt-v2"}], tables)[1][0]
    both = {"ic": ic_table(), "nav": dict(NAV_TABLE, rules=NAV_TABLE["rules"] + [{"id": "two-speed-v1"}])}
    assert "listed by both" in WK.rule_flags([{"name": "two-speed-v1"}], both)[1][0]
    assert "set to" in WK.rule_flags([{"name": "ew-theme-std-v1"}, {"name": "ic-shrink-v1"}], tables)[1][0]
    # D1's committed schemas describe the weights-file block, not the manifest's params (ruling D1-PSCHEMA: D2
    # corrects them): a manifest naming ew-theme-std-v1 is refused against them, fail closed
    got = WK.rule_flags([{"name": "ew-theme-std-v1"}], {"ic": ic_table(manifest_params=False)})[1]
    assert got == ["rules.ew-theme-std-v1: missing ['rerank', 'themes']"]
    assert WK.checked_params("two-speed-v1", {}, ic_table(False)["rules"][-1]) == {"rule": "two-speed-v1"}
    template = {"change": {"flags": {"fit": {"--two-speed": "two-speed-v1"}}}}
    assert WK.owned_flags({"fit": {"--two-speed": "two-speed-v1"}}, template) == ["fit --two-speed"]


# ------------------------------------------------------------------ the fixtures in a fake world
@pytest.mark.parametrize("name", sorted(OR6_ROWS))
def test_every_fixture_plans_in_a_fake_world(tmp_path, name):
    m = load_fixture(name)
    root = world(tmp_path, m)
    code, log = wave(root, KindsFake(root), "plan")
    assert code == 0, log
    text = "\n".join(log)
    if m["kind"] in ("rule", "leverage"):
        assert "--list-rules --json" in text
    if m["kind"] == "field":
        assert "prepare_research_fields.py --registry atx-engine/tools/field_registry.json --fields a,b,vol_126" in text
    if m["kind"] == "role":
        assert "prepare_recent_research.py role --universe linked-operating-v1" in text and "--out role-k-role" in text
    if m["kind"] in WK.SCHEMA_ONLY:
        assert "schema only until lane D3" in text


def cell_of(root: Path) -> dict:
    res = json.loads((root / "out/waves" / json.loads((root / F.MANIFEST).read_text())["wave"] / "receipts" /
                      "04-spec.json").read_text())
    return res["outputs"]


@pytest.mark.parametrize("name, check", [
    ("new-combination-rule.json", lambda d, s: d["change"]["flags"] == {"fit": {"--composition": "ew-theme-std-v1"}}
     and s["fit"]["flags"] == ["--composition", "ew-theme-std-v1"]),
    ("new-nav-rule.json", lambda d, s: d["change"]["flags"] == {"nav": {"--aim-leverage": "1.5"}}
     and s["nav"]["flags"] == ["--aim-leverage", "1.5"]),
    ("new-horizon.json", lambda d, s: d["change"]["flags"] == {"nav": {"--cadence": "5"}} and
     d["change"]["set"] == {"ic.u_output": "out/u-p0-h5", "fit.output": "out/fit-p0-h5", "card.output":
                            "out/card-p0-h5", "ic.w_output": "out/w-p0-h5", "nav.output": "out/nav-p0-L1.1474-h5"}),
    ("leverage-change.json", lambda d, s: d["parent"] == "lib-p0.json" and
     d["change"]["set"] == {"nav.leverage": "2.0", "nav.output": "out/nav-p0-L2.0"} and s["nav"]["leverage"] == "2.0"),
])
def test_rule_kinds_write_the_derived_constants(tmp_path, name, check):
    root = world(tmp_path, load_fixture(name))
    fake = KindsFake(root)
    code, log = wave(root, fake, "run", "--until", "spec")
    assert code == 0, log
    out = cell_of(root)
    doc = json.loads((root / out["cell_spec"]).read_text())
    assert out["kind"] == "rule" and check(doc, RC.load_spec(root / out["cell_spec"])), doc
    pre = json.loads((root / "out/waves" / load_fixture(name)["wave"] / "receipts" / "01-preflight.json").read_text())
    kind = pre["outputs"]["kind"]
    assert kind["kind"] == load_fixture(name)["kind"]
    if kind["kind"] == "rule":
        assert set(kind["tables"]) == {"ic", "nav"} and all(len(v) == 64 for v in kind["tables"].values())


def test_rule_wave_refuses_a_name_no_table_lists(tmp_path):
    root = world(tmp_path, load_fixture("new-cost-model.json"))      # schema only until C3 lists cost laws
    code, log = wave(root, KindsFake(root), "run", "--until", "spec")
    assert code == 3
    root = world(tmp_path / "b", load_fixture("new-combination-rule.json"))
    code, _ = wave(root, KindsFake(root, ic=ic_table(manifest_params=False)), "run", "--until", "spec")
    assert code == 3                                                 # D1's schemas, before D2's (D1-PSCHEMA)


def test_field_wave_builds_then_registers_on_the_build(tmp_path):
    root = world(tmp_path, load_fixture("new-field.json"))
    fake = KindsFake(root)
    code, log = wave(root, fake, "run", "--until", "register")
    assert code == 0, log
    builds = [c for c in fake.calls if c[1:2] and Path(c[1]).name == "run_bounded_research.py"]
    assert len(builds) == 1 and "--reuse" in builds[0] and builds[0][builds[0].index("--fields") + 1] == "a,b,vol_126"
    adds = [c for c in fake.calls if "add-alpha" in c]
    assert adds and all(c[c.index("--fields") + 1] == "fields-k-field" for c in adds)
    reg = json.loads((root / "out/waves/k-field/receipts/02-register.json").read_text())["outputs"]
    assert reg["build"]["fields"]["dir"] == "fields-k-field" and reg["build"]["fields"]["rows"] == ["vol_126"]
    assert reg["build"]["fields"]["manifest_sha256"] == sha((root / "fields-k-field/manifest.json").read_bytes())
    code, log = wave(root, fake, "run", "--until", "register")      # resumed: nothing builds twice
    assert code == 0 and len([c for c in fake.calls if c[1:2] and Path(c[1]).name == "run_bounded_research.py"]) == 1


def test_role_wave_builds_role_and_fields(tmp_path):
    root = world(tmp_path, load_fixture("new-universe.json"))
    fake = KindsFake(root)
    code, log = wave(root, fake, "run", "--until", "spec")
    assert code == 0, log
    doc = json.loads((root / cell_of(root)["cell_spec"]).read_text())
    role_sha = sha((root / "role-k-role/manifest.json").read_bytes())
    assert doc["change"]["inputs"]["role"]["path"] == "role-k-role/manifest.json"
    assert doc["change"]["set"]["fields.output"] == "fields-k-role"
    builds = [c for c in fake.calls if c[1:2] and Path(c[1]).name == "run_bounded_research.py"]
    assert [Path(c[c.index("--") + 2]).name for c in builds] == ["prepare_recent_research.py",
                                                                 "prepare_research_fields.py"]
    assert builds[1][builds[1].index("--role-sha256") + 1] == role_sha and "--reuse" not in builds[1]


def test_walk_forward_is_schema_only(tmp_path):
    root = world(tmp_path, load_fixture("walk-forward.json"))
    code, log = wave(root, KindsFake(root), "run", "--until", "preflight")
    assert code == 3
