"""The wave manifest v2 kinds (contract K-P9-9, P9 lane E2; wave_kinds.py, wave_stage_kinds.py) and the committed kinds
fixtures (scripts/specs/p9/kinds/, gate G-P2). Synthetic worlds only (wave_fixture.py's fake root and executor).

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_kinds.py

  test_the_ten_fixtures_load_and_cover_the_kinds      every fixture validates; the 8 K-P9-9 kinds and the 10 OR
                                                      section 6 rows are covered; template pins are the files' SHA-256
  test_v1_manifests_keep_their_validation             v8 wave manifests: the problems of before (none, or the head's)
  test_v2_refusals                                    each kind's block, cross-kind blocks, F-8 raw rule flags
  test_schema_subset                                  params_schema keywords; an unknown keyword, type or malformed
                                                      keyword value is refused (S3); placeholders (S4)
  test_rule_flags_from_the_tables                     composition flags (COMPOSITION_FLAGS), NAV x-flags, both / neither
                                                      tables, bad params; D1's committed table refuses manifest params
                                                      until D2's schema (D1-PSCHEMA); raw rule flags (R2)
  test_every_fixture_plans_in_a_fake_world            `wave plan` exits 0 on each fixture re-rooted into the fake world
  test_rule_kinds_write_the_derived_constants         rule (IC and NAV rows), horizon, leverage without a template: the
                                                      cell the spec stage writes carries the derived flags and names
  test_rule_wave_refuses_raw_rule_flags               review E2 R2: constants.flags / change.flags setting a rule's flag
  test_raw_rule_flags_refused_on_the_resolved_cell    ruling E2-8 (R2b, N2): set / unset routes, =value, abbreviations,
                                                      every template kind; the message names token and route
  test_raw_rule_flag_guard_allows_free_flags_and_names_unmapped_ic_rules  E2-8: --cadence free; N1 unmapped IC rule
  test_nav_leverage_is_set_only_by_a_leverage_wave    ruling E2-10 (R2c): nav.leverage by any route, any other kind
  test_leverage_wave_cap_and_table_and_the_matched_l_prime  E2-10: [1, 2] passes, > 2.0 and no table refused; L'
  test_leverage_wave_refuses_a_value_outside_the_nav_table  review E2 S9: fixed-v1's aim_leverage range at preflight
  test_field_wave_builds_then_registers_on_the_build  the register stage builds the fields dir, add-alpha uses it; an
                                                      output this wave's build did not make is refused (S6)
  test_build_logs_are_seal_scanned                    review E2 R1: the builds' logs are in verify's seal scan
  test_verify_plan_line_states_the_rule_cell_refusal  ruling E2-6: v1 verify plan bytes unchanged; v2 states W1-I2 (a)
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
import research_tree as RT  # noqa: E402
import research_wave  # noqa: E402
import wave_fixture as F  # noqa: E402
import wave_kinds as WK  # noqa: E402
import wave_manifest as WM  # noqa: E402
import wave_seal  # noqa: E402
import wave_stage_kinds as SK  # noqa: E402
from wave_context import Wave  # noqa: E402

REPO = HERE.parents[1]
KINDS_DIR = REPO / "scripts" / "specs" / "p9" / "kinds"
IC_FIXTURE = REPO / "atx-impl" / "tests" / "fixtures" / "composition_rules_list.json"   # D1's K-P9-6 table, pinned
OR6_ROWS = {"new-signal.json": "add", "replaced-signal.json": "replace", "new-field.json": "field",
            "new-combination-rule.json": "rule", "new-nav-rule.json": "rule", "leverage-change.json": "leverage",
            "new-cost-model.json": "rule", "new-universe.json": "role", "new-horizon.json": "horizon",
            "walk-forward.json": "walk-forward"}
SCHEMA_ONLY_FIXTURES = ("new-cost-model.json", "walk-forward.json")      # wave 3: lanes C3 and D3
UNMAPPED = "zz-unmapped-rule-v0"   # ruling E2-12: a synthetic IC rule id no lane registers (never in COMPOSITION_FLAGS)


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


# D1's params_schema of two rows as committed at base 1239a5ff (composition_rules_list.json): ew-theme-std-v1's
# describes the weights-file block, not a manifest's params (ruling D1-PSCHEMA). Frozen here because D2 re-pins
# that fixture (wave2-carry D2): the fail-closed checks below keep testing D1's shape after that merge.
D1_SCHEMAS = {
    "ew-theme-std-v1": {"type": "object", "required": ["rule", "rerank", "themes"], "properties": {
        "rerank": {"type": "boolean"}, "rule": {"const": "ew-theme-std-v1"},
        "themes": {"type": "object", "additionalProperties": {"type": "string", "pattern": "^[a-z0-9_]{1,64}$"}}}},
    "two-speed-v1": {"type": "object", "additionalProperties": False, "required": ["rule"],
                     "properties": {"rule": {"const": "two-speed-v1"}}}}


def d1_table() -> dict:
    """The committed table with D1_SCHEMAS in place of its rows' schemas (D1's state, whatever D2 commits)."""
    doc = ic_table(manifest_params=False)
    for row in doc["rules"]:
        if row["id"] in D1_SCHEMAS:
            row["params_schema"] = json.loads(json.dumps(D1_SCHEMAS[row["id"]]))
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
        F.write_json(self.root, f"{run_dir}/receipt.json", {"outcome": "completed", "exit_code": 0,   # K-P9-10
                                                            "argv_sha256": RT.argv_sha256(cmd[1:])})
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
    ("new-field.json", lambda m: m["field"].update(output="./fields/"), "is the wave's fields dir"),   # review S5
    ("new-field.json", lambda m: m["field"].update(output="Fields"), "is the wave's fields dir"),
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
    # review E2 S3: a type list or an unknown type is a problem (no crash); a malformed keyword value is refused
    assert "schema type ['string', 'null'] not supported" in WK.schema_problems("x", {"type": ["string", "null"]},
                                                                               "p")[0]
    assert "not supported" in WK.schema_problems(1, {"type": "float"}, "p")[0]
    for bad in ({"type": "object", "properties": []}, {"required": "a"}, {"items": []}, {"enum": "ab"},
                {"additionalProperties": "no"}):
        assert "refused, not ignored" in WK.schema_problems({}, bad, "p")[0], bad
    # review E2 S4: a multi-letter placeholder is a param's value, never emitted literally
    row = {"x-flag": "--risk-target SIGMA", "params_schema": {"properties": {"sigma": {"x-flag": "--risk-target"}}}}
    with pytest.raises(WK.KindError, match="needs a param value"):
        WK.nav_flags("r", {}, row)
    assert WK.nav_flags("r", {"sigma": 0.1}, row) == {"nav": {"--risk-target": "0.1"}}
    assert WK.nav_flags("v", {}, {"x-flag": "--vol-target vol-target-v1"}) == {"nav": {"--vol-target": "vol-target-v1"}}
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
    got = WK.rule_flags([{"name": "ew-theme-std-v1"}], {"ic": d1_table()})[1]
    assert got == ["rules.ew-theme-std-v1: missing ['rerank', 'themes']"]
    two_speed = next(r for r in d1_table()["rules"] if r["id"] == "two-speed-v1")
    assert WK.checked_params("two-speed-v1", {}, two_speed) == {"rule": "two-speed-v1"}
    # review E2 R2: a raw rule flag (named rule's, or any table rule's x-flag / param x-flag / composition flag)
    derived = {"fit": {"--two-speed": "two-speed-v1"}}
    assert WK.raw_rule_flags({"fit": {"--two-speed": "two-speed-v1"}}, derived, {}) == ["fit --two-speed"]
    assert WK.raw_rule_flags({"nav": {"--vol-target": "vol-target-v1", "--risk-model": "r", "--cadence": "5"},
                              "fit": {"--theme-erc": "theme-erc-v1", "--min-names": "100"}}, {}, tables) == \
        ["fit --theme-erc", "nav --risk-model", "nav --vol-target"]       # not named; --cadence / --min-names: free
    assert WK.raw_rule_flags({"nav": {"--risk-target": "0.1", "--aim-leverage": "2.0"}}, {}, tables) == \
        ["nav --aim-leverage", "nav --risk-target"]
    assert WK.table_flags(tables)["nav"] >= {"--aim-leverage", "--vol-target", "--risk-target", "--risk-model",
                                             "--risk-model-sha256", "--two-speed"}
    # ruling E2-8: "=value" stripped, an argparse abbreviation refused; a free flag stays free
    assert WK.raw_rule_flags({"fit": {"--theme-erc=theme-erc-v1": True, "--theme-er": "x", "--min-names": "1"}}, {},
                             tables) == ["fit --theme-er (abbreviates ['--theme-erc'])", "fit --theme-erc=theme-erc-v1"]
    assert WK.abbreviated("--the", WK.table_flags(tables)["fit"]) == ["--theme-erc", "--theme-resid", "--theme-tsmom"]
    assert WK.abbreviated("--", {"--x"}) == [] and WK.abbreviated("--theme-erc", {"--theme-erc"}) == []
    # ruling E2-8 (review N1): an IC rule COMPOSITION_FLAGS does not map is named (ruling E2-12: a synthetic id no
    # lane can register, so MR-4's real rows never make it mapped)
    assert WK.unmapped_ic_rules(tables) == [] and WK.unmapped_ic_rules({}) == []
    assert UNMAPPED not in WK.COMPOSITION_FLAGS
    more = dict(ic_table(), rules=ic_table()["rules"] + [{"id": UNMAPPED}])
    assert WK.unmapped_ic_rules({"ic": more}) == [UNMAPPED]


# ------------------------------------------------------------------ the fixtures in a fake world
@pytest.mark.parametrize("name", sorted(OR6_ROWS))
def test_every_fixture_plans_in_a_fake_world(tmp_path, name):
    m = load_fixture(name)
    root = world(tmp_path, m)
    code, log = wave(root, KindsFake(root), "plan")
    assert code == 0, log
    text = "\n".join(log)
    assert "--list-rules --json" in text and "ruling E2-8" in text           # every v2 kind reads both tables
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
    code, _ = wave(root, KindsFake(root, ic=d1_table()), "run", "--until", "spec")
    assert code == 3                                                 # D1's schemas, before D2's (D1-PSCHEMA)


def preflight_error(root: Path, m: dict) -> str:
    return json.loads((root / "out/waves" / m["wave"] / "receipts" / "01-preflight.failed-1.json").read_text())["error"]


@pytest.mark.parametrize("flags, owned", [
    ({"nav": {"--aim-leverage": "2.0"}}, "['nav --aim-leverage']"),                 # the named rule's own x-flag
    ({"nav": {"--vol-target": "vol-target-v1"}}, "['nav --vol-target']"),            # a rule the manifest never names
    ({"fit": {"--theme-erc": "theme-erc-v1"}}, "['fit --theme-erc']"),               # a composition rule's flag
])
def test_rule_wave_refuses_raw_rule_flags(tmp_path, flags, owned):
    """Review E2 R2 (F-8, D1-PSCHEMA): rule_cell.constants.flags (and the template's change.flags) may not set a flag
    a rule of the exes' tables owns, whether the manifest names that rule or not: preflight refuses (exit 3) instead
    of merged_constants silently overriding it, or the rule running with no params_schema check. A flag no rule owns
    stays allowed."""
    m = load_fixture("new-nav-rule.json")
    m["rule_cell"] = dict(m["rule_cell"], constants={"flags": flags})
    root = world(tmp_path / "c", m)
    code, log = wave(root, KindsFake(root), "run", "--until", "preflight")
    assert code == 3, log
    err = preflight_error(root, m)
    assert f"rule_cell.constants.flags sets {owned}, which the exes' rules own" in err
    tpl = json.loads((KINDS_DIR / "templates" / "rule-nav.json").read_text())   # the same flags in the template
    m = load_fixture("new-nav-rule.json")
    root = world(tmp_path / "t", m)
    tpl["change"]["flags"] = flags
    F.write_json(root, m["rule_cell"]["template"], tpl)
    m["rule_cell"]["template_sha256"] = sha((root / m["rule_cell"]["template"]).read_bytes())
    F.write_json(root, F.MANIFEST, rerooted(m, root))
    F.git(root, "commit", "-q", "-am", "a template writing a rule flag")
    code, log = wave(root, KindsFake(root), "run", "--until", "preflight")
    assert code == 3 and f"rule_cell.template change.flags sets {owned}" in preflight_error(root, m), log
    m = load_fixture("new-nav-rule.json")                            # a flag no rule owns: allowed
    m["rule_cell"] = dict(m["rule_cell"], constants={"flags": {"nav": {"--cadence": "5"}}})
    root = world(tmp_path / "ok", m)
    code, log = wave(root, KindsFake(root), "run", "--until", "preflight")
    assert code == 0, log


def set_template(root: Path, m: dict, edit) -> None:
    """The fixture's template edited by ``edit`` in the world ``root``, re-pinned in the manifest and committed."""
    tpl = json.loads((root / m["rule_cell"]["template"]).read_text(encoding="utf-8"))
    edit(tpl)
    F.write_json(root, m["rule_cell"]["template"], tpl)
    m["rule_cell"]["template_sha256"] = sha((root / m["rule_cell"]["template"]).read_bytes())
    F.write_json(root, F.MANIFEST, rerooted(m, root))
    F.git(root, "commit", "-q", "-am", "the template edited")


def constants(**const):
    return lambda m: m["rule_cell"].update(constants=const)


NAV_SET = ["--aim-leverage", "{leverage}"]                          # the fake parent's nav.flags
RULE_NAV = "scripts/specs/p9/kinds/templates/rule-nav.json"
E2_8_PROBES = [   # (fixture, manifest edit, template edit, the refusal's text): review E2 R2b / N2 probes
    ("new-nav-rule.json", constants(set={"nav.flags": NAV_SET + ["--vol-target", "vol-target-v1"]}), None,
     "rule_cell.constants.set nav.flags: the cell's nav.flags adds token(s) ['--vol-target']"),
    ("new-combination-rule.json", constants(set={"fit.flags": ["--theme-erc", "theme-erc-v1"]}), None,
     "rule_cell.constants.set fit.flags: the cell's fit.flags adds token(s) ['--theme-erc']"),
    ("new-combination-rule.json", None, lambda t: t["change"]["set"].update({"fit.flags": ["--theme-erc", "x"]}),
     "rule_cell.template change.set fit.flags: the cell's fit.flags adds token(s) ['--theme-erc']"),
    ("new-nav-rule.json", constants(flags={"fit": {"--theme-erc=theme-erc-v1": True}}), None,
     "rule_cell.constants.flags sets ['fit --theme-erc=theme-erc-v1'], which the exes' rules own"),
    ("new-nav-rule.json", constants(flags={"fit": {"--theme-er": "theme-erc-v1"}}), None,
     "rule_cell.constants.flags sets [\"fit --theme-er (abbreviates ['--theme-erc'])\"]"),
    ("new-combination-rule.json", constants(flags={"fit": {"--composition=ew-theme-std-v1": True}}), None,
     "rule_cell.constants.flags sets ['fit --composition=ew-theme-std-v1']"),
    ("new-horizon.json", lambda m: m["horizon"]["flags"]["nav"].update({"--vol-target": "vol-target-v1"}), None,
     "horizon.flags sets ['nav --vol-target'], which the exes' rules own"),
    ("new-universe.json", constants(set={"nav.flags": NAV_SET + ["--risk-target", "0.1"]}), None,
     "rule_cell.constants.set nav.flags: the cell's nav.flags adds token(s) ['--risk-target']"),
    ("new-universe.json", None, lambda t: t["change"].update(unset=["nav.flags"]),
     "rule_cell.template change.unset nav.flags: the cell's nav.flags drops token(s) ['--aim-leverage']"),
    ("new-universe.json", None, lambda t: t["change"].update(flags={"nav": {"--risk": "0.1"}}),
     "rule_cell.template change.flags sets [\"nav --risk (abbreviates ['--risk-model', '--risk-model-sha256', "
     "'--risk-target'])\"]"),
    ("leverage-change.json", lambda m: m.update(rule_cell={"template": RULE_NAV, "template_sha256": "", "constants": {
        "set": {"nav.flags": NAV_SET + ["--two-speed", "two-speed-v1"]}}}), None,
     "rule_cell.constants.set nav.flags: the cell's nav.flags adds token(s) ['--two-speed']"),
    # ruling E2-10 / review E2 S2: another case and a single dash count as the owned flag at preflight
    ("new-nav-rule.json", constants(set={"nav.flags": NAV_SET + ["--Vol-Target", "vol-target-v1"]}), None,
     "rule_cell.constants.set nav.flags: the cell's nav.flags adds token(s) ['--Vol-Target']"),
    ("new-nav-rule.json", constants(flags={"fit": {"-theme-erc": "theme-erc-v1"}}), None,
     "rule_cell.constants.flags sets ['fit -theme-erc'], which the exes' rules own"),
    ("new-combination-rule.json", constants(set={"fit.flags": ["-theme-erc", "x"]}), None,
     "rule_cell.constants.set fit.flags: the cell's fit.flags adds token(s) ['-theme-erc']"),
    # review E2 S3: a flag in a free flag's value slot is refused with the map entry that put it there named
    ("new-nav-rule.json", constants(flags={"nav": {"--cadence": "--vol-target"}}), None,
     "rule_cell.constants.flags nav --cadence --vol-target: the cell's nav.flags adds token(s) ['--vol-target']"),
]


@pytest.mark.parametrize("name, edit, tedit, needle", E2_8_PROBES)
def test_raw_rule_flags_refused_on_the_resolved_cell(tmp_path, name, edit, tedit, needle):
    """Ruling E2-8 (review E2 R2b, N2): a rule flag the manifest does not name is refused at preflight (exit 3, before
    register) by whatever route it would reach the cell: constants.set / the template's change.set or change.unset
    replacing a section's flag list (the cell's RESOLVED lists are compared with the parent's under the named rules
    only), an "=value" spelling, an argparse abbreviation, and in every template kind (rule, horizon, role,
    leverage). The message names the token and its route. Each probe passed preflight at 05a0fcd1."""
    m = load_fixture(name)
    if edit:
        edit(m)
    if m.get("rule_cell") and not m["rule_cell"].get("template_sha256"):
        m["rule_cell"]["template_sha256"] = sha((REPO / m["rule_cell"]["template"]).read_bytes())
    root = world(tmp_path / "w", m)
    if tedit:
        set_template(root, m, tedit)
    code, log = wave(root, KindsFake(root), "run", "--until", "preflight")
    assert code == 3, log
    err = preflight_error(root, m)
    assert needle in err, err
    assert not (root / "out/waves" / m["wave"] / "receipts" / "02-register.json").exists()


def test_raw_rule_flag_guard_allows_free_flags_and_names_unmapped_ic_rules(tmp_path):
    """Ruling E2-8: a flag no rule owns stays free by every route (here constants.set nav.flags adding --cadence);
    an IC rule the exe lists but COMPOSITION_FLAGS does not map stops every kind by name (review N1: a missed MR-4);
    the guard needs both tables."""
    m = load_fixture("new-nav-rule.json")
    m["rule_cell"]["constants"] = {"set": {"nav.flags": NAV_SET + ["--cadence", "5"]}}
    root = world(tmp_path / "free", m)
    code, log = wave(root, KindsFake(root), "run", "--until", "preflight")
    assert code == 0, log
    more = dict(ic_table(), rules=ic_table()["rules"] + [{"id": UNMAPPED, "params_schema": {}}])
    for name in ("new-signal.json", "new-nav-rule.json", "new-horizon.json"):
        m = load_fixture(name)
        root = world(tmp_path / name[:-5], m)
        code, log = wave(root, KindsFake(root, ic=more), "run", "--until", "preflight")
        assert code == 3, (name, log)
        assert f"the IC exe's rule table lists ['{UNMAPPED}'], which wave_kinds.COMPOSITION_FLAGS does not " \
               "map" in preflight_error(root, m), name
    m = load_fixture("new-signal.json")
    root = world(tmp_path / "noic", m)
    code, log = wave(root, KindsFake(root, ic={}), "run", "--until", "preflight")
    assert code == 3 and "the IC exe's rule table could not be read" in preflight_error(root, m), log


def gross_none(m: dict) -> None:
    m["gross_match"] = "none"


def nav_cell(**over) -> dict:
    """The fake parent's nav section (wave_fixture.cell_spec) with ``over``."""
    return dict(F.cell_spec("p0", F.PARENT_NAV, reference_nav="out/nav-base")["nav"], **over)


E2_10_PROBES = [  # (fixture, manifest edit, template edit, the refusal's text): review E2 R2c, the 5.0 probe
    ("new-horizon.json", lambda m: (gross_none(m), constants(set={"nav.leverage": "5.0"})(m)), None,
     "rule_cell.constants.set nav.leverage = '5.0': the cell's nav.leverage would be '5.0' where the parent has "
     "'1.1474'"),
    ("new-combination-rule.json", lambda m: (gross_none(m), constants(set={"nav.leverage": "5.0"})(m)), None,
     "rule_cell.constants.set nav.leverage = '5.0': the cell's nav.leverage would be '5.0' where the parent has "
     "'1.1474'"),
    ("new-universe.json", gross_none, lambda t: t["change"]["set"].update({"nav.leverage": "5.0"}),
     "rule_cell.template change.set nav.leverage = '5.0': the cell's nav.leverage would be '5.0'"),
    ("new-universe.json", None, lambda t: t["change"]["set"].update({"nav.leverage": "5.0"}),    # pm6-6 as well
     "rule_cell.template change.set nav.leverage = '5.0': the cell's nav.leverage would be '5.0'"),
    ("new-horizon.json", constants(set={"nav": nav_cell(leverage="5.0")}), None,                 # a whole nav section
     "rule_cell.constants.set nav: the cell's nav.leverage would be '5.0' where the parent has '1.1474'"),
    ("new-universe.json", None, lambda t: t["change"].update(unset=["nav.leverage"]),
     "rule_cell.template change.unset nav.leverage = '(unset)': the cell's nav.leverage would be None"),
    ("leverage-change.json", lambda m: m.update(rule_cell={"template": RULE_NAV, "template_sha256": ""}),
     lambda t: t["change"]["set"].update({"nav.leverage": "1.5"}),                # overridden silently: refused
     "rule_cell.template change.set nav.leverage = '1.5': the cell's nav.leverage would be '2.0' where "
     "leverage.value gives '2.0'"),
]


@pytest.mark.parametrize("name, edit, tedit, needle", E2_10_PROBES)
def test_nav_leverage_is_set_only_by_a_leverage_wave(tmp_path, name, edit, tedit, needle):
    """Ruling E2-10 (review E2 R2c): nav.leverage is fixed-v1's aim_leverage (research_cycle substitutes it into
    --aim-leverage {leverage}), a rule parameter. Any kind but leverage that sets or changes it, by constants.set, the
    template's change.set / unset or a whole nav section, with gross_match "none" or pm6-6, is refused at preflight
    (exit 3, before register), naming the key, the value and the route; in a leverage wave a raw nav.leverage (which
    leverage.value would override) is refused too. The 5.0 probes passed preflight and spec at d0054b32."""
    m = load_fixture(name)
    if edit:
        edit(m)
    if m.get("rule_cell") and not m["rule_cell"].get("template_sha256"):
        m["rule_cell"]["template_sha256"] = sha((REPO / m["rule_cell"]["template"]).read_bytes())
    root = world(tmp_path / "w", m)
    if tedit:
        set_template(root, m, tedit)
    code, log = wave(root, KindsFake(root), "run", "--until", "preflight")
    assert code == 3, log
    err = preflight_error(root, m)
    assert needle in err and "ruling E2-10" in err, err
    assert not (root / "out/waves" / m["wave"] / "receipts" / "02-register.json").exists()


def test_leverage_wave_cap_and_table_and_the_matched_l_prime(tmp_path):
    """Ruling E2-10: a leverage wave inside the table's [1, 2] passes; above 2.0 it is refused even when the NAV table
    would allow more (OD-P9-3); a NAV table with no fixed-v1 range refuses it (nothing to range-check against). The
    gross-match stage still publishes its L' (a v2 horizon cell at pm6-6, mismatched calibration -> the -gm copy at
    L', written after preflight)."""
    m = load_fixture("leverage-change.json")
    m["leverage"] = dict(m["leverage"], value="1.5")
    root = world(tmp_path / "ok", m)
    code, log = wave(root, KindsFake(root), "run", "--until", "spec")
    assert code == 0, log
    assert json.loads((root / cell_of(root)["cell_spec"]).read_text())["change"]["set"]["nav.leverage"] == "1.5"
    wide = json.loads(json.dumps(NAV_TABLE))
    wide["rules"][0]["params_schema"]["properties"]["aim_leverage"]["maximum"] = 3
    m["leverage"] = dict(m["leverage"], value="2.5")
    root = world(tmp_path / "cap", m)
    code, log = wave(root, KindsFake(root, nav=wide), "run", "--until", "preflight")
    err = preflight_error(root, m)
    assert code == 3 and "leverage.value 2.5 is above 2.0, the P9 leverage cap (ruling E2-10" in err, log
    assert "lies outside" not in err                                    # the table allows it: the cap refuses
    m["leverage"] = dict(m["leverage"], value="1.5")
    root = world(tmp_path / "nofixed", m)
    code, log = wave(root, KindsFake(root, nav=dict(NAV_TABLE, rules=NAV_TABLE["rules"][1:])), "run", "--until",
                     "preflight")
    assert code == 3 and "lists no fixed-v1 aim_leverage range" in preflight_error(root, m), log
    m = load_fixture("new-horizon.json")                                # gross matching: L' published
    root = world(tmp_path / "gm", m)
    fake = KindsFake(root, gross_per_l={f"p9k-horizon-{m['wave']}": 0.9604 / 1.1474})
    code, log = wave(root, fake, "run", "--until", "match")
    assert code == 0, log
    got = json.loads((root / "out/waves" / m["wave"] / "receipts" / "06-match.json").read_text())["outputs"]
    assert got["corrected"] is True and got["cell_spec"].endswith("-gm.json") and got["leverage"] != "1.1474"
    gm = json.loads((root / got["cell_spec"]).read_text())
    assert gm["change"]["set"]["nav.leverage"] == got["leverage"]


def test_leverage_wave_refuses_a_value_outside_the_nav_table(tmp_path):
    """Review E2 S9: a leverage wave's value outside the NAV exe's fixed-v1 aim_leverage range is refused at preflight
    (exit 3); inside it is accepted and recorded with the bounds."""
    m = load_fixture("leverage-change.json")
    m["leverage"] = dict(m["leverage"], value="2.5")                 # NAV_TABLE: aim_leverage in [1, 2]
    root = world(tmp_path / "out", m)
    code, log = wave(root, KindsFake(root), "run", "--until", "preflight")
    assert code == 3, log
    assert "leverage.value 2.5 lies outside the NAV exe's fixed-v1 aim_leverage range [1.0, 2.0]" in \
        preflight_error(root, m)
    m = load_fixture("leverage-change.json")
    root = world(tmp_path / "in", m)
    code, log = wave(root, KindsFake(root), "run", "--until", "preflight")
    assert code == 0, log
    pre = json.loads((root / "out/waves" / m["wave"] / "receipts" / "01-preflight.json").read_text())["outputs"]
    assert pre["kind"]["leverage"] == {"value": m["leverage"]["value"], "bounds": [1.0, 2.0]}


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
    # review E2 S6: an existing output is reused only as this wave's last build of it with this command made it
    m = load_fixture("new-field.json")
    other = world(tmp_path / "other", m)
    F.write_json(other, "fields-k-field/manifest.json", {"status": "complete", "seal": {"exclusive_end": "2024-01-01"},
                                                         "fields": [{"name": n} for n in ("a", "b", "vol_126")]})
    code, log = wave(other, KindsFake(other), "run", "--until", "register")   # made elsewhere (no run of this wave)
    failed = other / "out/waves/k-field/receipts/02-register.failed-1.json"
    assert code == 3 and "last fields run (none) did not complete it" in json.loads(failed.read_text())["error"], log
    receipt = root / "out/waves/k-field/builds/fields-run1/receipt.json"
    receipt.write_text(json.dumps(dict(json.loads(receipt.read_text()), argv_sha256="0" * 64)))
    (root / "out/waves/k-field/receipts/02-register.json").unlink()   # the stage runs again
    code, log = wave(root, fake, "run", "--until", "register")      # another command made it: refused
    failed = root / "out/waves/k-field/receipts/02-register.failed-1.json"
    assert code == 3 and "fields-run1) did not complete it" in json.loads(failed.read_text())["error"], log


class SealedBuild(KindsFake):
    """KindsFake whose builds print ``log`` to their run dir's stdout.log (what the bounded runner captures)."""

    def __init__(self, root: Path, log: str, **kw):
        super().__init__(root, **kw)
        self.log = log

    def build(self, argv: list[str], cmd: list[str]):
        done = super().build(argv, cmd)
        F.write(self.root, f"{argv[argv.index('--output') + 1]}/stdout.log", self.log)
        return done


def test_build_logs_are_seal_scanned(tmp_path):
    """Review E2 R1: the field and role builds' run dirs (<out_dir>/builds/<what>-run<k>, wave_stage_kinds.BUILDS) are
    in the seal scan (wave_seal.wave_logs, which verify and record read): a build log naming a date at or after the
    seal is a hit, and verify refuses the wave (synthetic post-seal text, ruling T2-SYN); a clean build log is scanned
    and verify passes. Every build name run_build accepts is a scanned base."""
    m = load_fixture("new-field.json")
    w = None
    build_log = f"out/waves/{m['wave']}/builds/fields-run1/stdout.log"
    for label, text, hits in (("clean", "fields read through 2023-12-29\n", 0),
                              ("sealed", "fields read through 2024-06-28\n", 1)):
        root = world(tmp_path / label, m)
        fake = SealedBuild(root, text)
        fake.admission = {"p9k_vol": ("admitted", 1)}                # the screen admits the wave's string
        code, log = wave(root, fake, "run", "--until", "register")
        assert code == 0, log
        w = Wave(F.MANIFEST, root, executor=fake, log=lambda s: None)
        logs = wave_seal.wave_logs(w, {})
        assert build_log in logs, logs
        seal = wave_seal.scan(w, logs)
        assert seal["tokens_at_or_after_seal"] == hits
        assert [h["file"] for h in seal["where"]] == ([build_log] if hits else [])
        code, log = wave(root, fake, "run", "--until", "verify")
        receipts = root / "out/waves" / m["wave"] / "receipts"
        if hits:
            assert code == 4, log
            failed = json.loads((receipts / "07-verify.failed-1.json").read_text())
            assert build_log in json.dumps(failed) and "at or after the seal" in json.dumps(failed)
        else:
            assert code == 0, log
            scanned = json.loads((receipts / "07-verify.json").read_text())["outputs"]["seal_scan"]
            assert scanned["tokens_at_or_after_seal"] == 0 and scanned["files"] >= len(logs)
    assert [Path(b).name for b in SK.build_bases(w)] == list(SK.BUILDS) == ["fields", "role", "role-fields"]
    with pytest.raises(SK.StageError, match="not one of"):
        SK.run_build(w, "other", "x", [], ["python"], {"seconds": 1, "max_rss_mib": 64})


V1_VERIFY_LINE = ("#   mechanics rule v8-mech on the mechanics keys; C-13 binding of the cell's NAV; NAV exe sha256 of the "
                  "parent vs the cell (recorded; differing, the cell's ref must have run on the cell's exe); seal scan "
                  "of every log the wave produced (run dirs of the screen, the cell and its -gm copy, readers, "
                  "consoles)")


def verify_lines(log: list[str]) -> list[str]:
    return [line for line in log if line.startswith("#   mechanics rule v8-mech")]


def test_verify_plan_line_states_the_rule_cell_refusal(tmp_path):
    """Ruling E2-6: a v1 manifest's verify plan line keeps its bytes (hash-only plan identity); a v2 rule wave's line
    (and every v2 kind's: the refusal is keyed on the cell's ref phase) states ruling W1-I2 (a)'s refusal instead of
    "recorded"."""
    root = F.build(tmp_path / "v1")
    code, log = wave(root, KindsFake(root), "plan")
    assert code == 0 and verify_lines(log) == [V1_VERIFY_LINE], log
    assert not [line for line in log if "--list-rules" in line or "E2-8" in line]   # ruling E2-8 is v2 only
    refusal = ("(a cell without a ref construction, e.g. a rule cell, is refused when the two NAVs' exes differ or "
               "only one is recorded, by receipt or exes_sha256 pin (ruling W1-I2 (a)); with one, differing, the "
               "cell's ref must have run on the cell's exe)")
    for name in ("new-nav-rule.json", "new-combination-rule.json", "leverage-change.json", "new-signal.json"):
        root = world(tmp_path / name[:-5], load_fixture(name))
        code, log = wave(root, KindsFake(root), "plan")
        lines = verify_lines(log)
        assert code == 0 and len(lines) == 1 and "recorded;" not in lines[0], (name, log)
        assert refusal in lines[0], (name, lines)


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
