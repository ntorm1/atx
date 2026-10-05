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
  P9 lane D3 (rulings D3-1, D3-2, W3-8a, C3-3, E2-17; every refusal has teeth, task-D3-report.md "Part A"):
  test_every_fixture_runs_preflight_on_c3s_rows       G-P2: all 10 fixtures runnable (none schema only) on lane C3's
                                                      trade-rate and cost-law rows
  test_the_cost_model_fixture_runs_to_spec_on_c3s_rows  W3-8a: cost-sqrt-v2 derives --cost-law cost-sqrt-v2
  test_walk_forward_runs_to_spec                      the kind's fitter flags (D3-2) and renamed outputs on an
                                                      --engine-fit parent; refusals: the parent, the block, a raw
                                                      --walk-forward* in any other kind (E2-8 via KIND_FLAGS)
  test_c3_3_*                                         ruling C3-3: x-flag-aliases owned as the row's flag
  test_e2_17_*                                        ruling E2-17: inputs.* only by the kinds and keys of KIND_INPUTS
  test_the_live_rule_tables                           skipped unless ATX_EQUITY_BIN names a bin with both exes: the
                                                      exes' live tables, the 10 fixtures on them (root M2h's check)
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

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
SCHEMA_ONLY_FIXTURES: tuple = ()      # wave 3, lane D3 (W3-8a): new-cost-model (C3's rows) and walk-forward flipped
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
    assert sum(name not in SCHEMA_ONLY_FIXTURES for name in OR6_ROWS) == 10  # 10 runnable, 0 schema only (W3-8a)
    assert WK.SCHEMA_ONLY == () and load_fixture("walk-forward.json")["kind"] not in WK.SCHEMA_ONLY


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
    # the committed IC table lists AL-COMB's theme-cov-hedge-v1 (root merge, ruling E2-14), MR-4 maps --theme-hedge
    assert WK.abbreviated("--the", WK.table_flags(tables)["fit"]) == ["--theme-erc", "--theme-hedge", "--theme-resid",
                                                                      "--theme-tsmom"]
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
    assert "schema only" not in text                                         # none left (lane D3, W3-8a)
    if m["kind"] == "walk-forward":
        assert "'--walk-forward-min-decisions': '21'" in text and "outputs renamed -wf2s21m21" in text


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
    root = world(tmp_path, load_fixture("new-cost-model.json"))      # E2's NAV_TABLE has no cost-law row (C3's: NAV_C3)
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
    # review E2 S4: research_spec.resolve iterates a dict-typed unset by key; its route is named as a list's
    ("new-universe.json", None, lambda t: t["change"].update(unset={"nav.leverage": True}),
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
    assert code == 3, log                                               # before the failed receipt is read
    err = preflight_error(root, m)
    assert "leverage.value 2.5 is above 2.0, the P9 leverage cap (ruling E2-10" in err, err
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


def probe(tmp_path: Path, name: str, edit, tedit, *args: str, **fake) -> tuple[int, list[str], Path, dict]:
    """The fixture ``name`` with the manifest edit ``edit`` and the template edit ``tedit`` in a fake world, run with
    ``args`` (default: up to preflight)."""
    m = load_fixture(name)
    if edit:
        edit(m)
    if m.get("rule_cell") and not m["rule_cell"].get("template_sha256"):
        m["rule_cell"]["template_sha256"] = sha((REPO / m["rule_cell"]["template"]).read_bytes())
    root = world(tmp_path / "w", m)
    if tedit:
        set_template(root, m, tedit)
    code, log = wave(root, KindsFake(root, **fake), *(args or ("run", "--until", "preflight")))
    return code, log, root, m


def tpl_set(**sets):
    """A template edit adding ``sets`` to its change.set (the template may have none yet)."""
    return lambda t: t["change"].setdefault("set", {}).update(sets)


def stopped_at_preflight(code: int, log: list[str], root: Path, m: dict, *needles: str) -> str:
    assert code == 3, log
    err = preflight_error(root, m)
    assert all(n in err for n in needles), err
    assert not (root / "out/waves" / m["wave"] / "receipts" / "02-register.json").exists()
    return err


# ------------------------------------------------------------------ ruling E2-14 (review E2 R3a): the match flags
CAL = "--calibrate-gross"
R3A_PROBES = [   # (fixture, manifest edit, template edit, the route and token the refusal names)
    ("new-horizon.json", lambda m: (gross_none(m), constants(flags={"nav": {CAL: "0.95"}})(m)), None,
     "rule_cell.constants.flags nav --calibrate-gross (reads as ['--calibrate-gross'])"),
    ("new-combination-rule.json", None, lambda t: t["change"].update(flags={"nav": {CAL: "0.95"}}),
     "rule_cell.template change.flags nav --calibrate-gross (reads as ['--calibrate-gross'])"),
    ("new-universe.json", constants(set={"nav.flags": NAV_SET + [CAL, "0.95"]}), None,
     "rule_cell.constants.set nav.flags: the cell's nav.flags carries ['--calibrate-gross']"),
    ("new-horizon.json", lambda m: (gross_none(m), m["horizon"]["flags"]["nav"].update({CAL: "0.95"})), None,
     "horizon.flags nav --calibrate-gross (reads as ['--calibrate-gross'])"),
    ("leverage-change.json", lambda m: m.update(rule_cell={"template": RULE_NAV, "template_sha256": "", "constants": {
        "flags": {"nav": {CAL: "0.95"}}}}), None, "rule_cell.constants.flags nav --calibrate-gross"),
    # spellings: case and =value, a single dash, an abbreviation of both, a free flag's value slot
    ("new-nav-rule.json", lambda m: (m.update(gross_match="pm6-6"), constants(flags={"nav": {
        "--Calibrate-Gross=0.95": True}})(m)), None, "rule_cell.constants.flags nav --Calibrate-Gross=0.95"),
    ("new-nav-rule.json", constants(set={"nav.flags": NAV_SET + ["-calibrate-tolerance", "0.001"]}), None,
     "rule_cell.constants.set nav.flags: the cell's nav.flags carries ['-calibrate-tolerance']"),
    ("new-horizon.json", constants(flags={"nav": {"--calib": "0.95"}}), None,
     "rule_cell.constants.flags nav --calib (reads as ['--calibrate-gross', '--calibrate-tolerance'])"),
    ("new-nav-rule.json", constants(flags={"nav": {"--cadence": CAL}}), None,
     "rule_cell.constants.flags nav --cadence --calibrate-gross"),
    ("new-nav-rule.json", constants(flags={"nav": {"--cadence": {"5": CAL}}}), None,       # a parent-value map
     "rule_cell.constants.flags nav --cadence {'5': '--calibrate-gross'}"),
    # any section; a whole nav section; the template's own sets
    ("new-combination-rule.json", constants(set={"fit.flags": [CAL, "0.95"]}), None,
     "rule_cell.constants.set fit.flags: the cell's fit.flags carries ['--calibrate-gross']"),
    ("new-horizon.json", constants(set={"nav": nav_cell(flags=NAV_SET + [CAL, "0.95"])}), None,
     "rule_cell.constants.set nav: the cell's nav.flags carries ['--calibrate-gross']"),
    ("new-universe.json", None, tpl_set(**{"nav.flags": NAV_SET + ["--calibrate-tolerance=0.01"]}),
     "rule_cell.template change.set nav.flags: the cell's nav.flags carries ['--calibrate-tolerance=0.01']"),
]


@pytest.mark.parametrize("name, edit, tedit, needle", R3A_PROBES)
def test_match_flags_are_the_gross_match_stages_own(tmp_path, name, edit, tedit, needle):
    """Ruling E2-14 (review E2 R3a): P9 C2's nav --calibrate-gross / --calibrate-tolerance make the NAV exe publish
    its run at an L' it picks itself, so only the gross-match stage (pm6-6-engine) puts them on an argv. From any
    other route (an option map key or value, constants.set, the template's change.set / change.flags, horizon.flags, a
    whole section), in any v2 kind with a rule cell, any section and any spelling (case, =value, a single dash, an
    abbreviation), preflight refuses them (exit 3, before register), naming the token and the route. Each probe passed
    preflight at d2cea6a6 (the merge of root into the lane)."""
    stopped_at_preflight(*probe(tmp_path, name, edit, tedit), needle, "ruling E2-14", "pm6-6-engine")


def test_match_flags_inherited_from_the_parent_are_refused(tmp_path):
    """Ruling E2-14: a parent spec whose lists carry a match flag (a run recipe named as a parent instead of its cell
    of record) would put it on the cell's argv: refused in a library kind (add-alpha's copy of the parent's lists) and
    in a rule kind, naming the parent spec; the match flag's helper reads every spelling and no free flag or value."""
    for name in ("new-signal.json", "new-horizon.json"):
        m = load_fixture(name)
        root = world(tmp_path / name[:-5], m)
        spec = json.loads((root / F.PARENT).read_text())
        spec["nav"]["flags"] += [CAL, "0.95"]
        F.write_json(root, F.PARENT, spec)
        F.git(root, "commit", "-q", "-am", "a parent carrying a match flag")
        code, log = wave(root, KindsFake(root), "run", "--until", "preflight")
        stopped_at_preflight(code, log, root, m, f"the parent spec {F.PARENT} nav.flags (the cell inherits it): the "
                             "cell's nav.flags carries ['--calibrate-gross']", "ruling E2-14")
    reads = WK.match_flag_reads
    assert reads(CAL) == reads(CAL + "=0.9") == reads("-Calibrate-Gross") == [CAL]
    assert reads("--CALIBRATE-TOL") == ["--calibrate-tolerance"] and reads("--calib") == list(WK.MATCH_FLAGS)
    for free in ("--cadence", "--capacity-curve", "0.95", "-0.5", "--", "-", "--calibrate-grossx", None, 1.5):
        assert reads(free) == [], free


TRADE_RATE = nav_row("zz-trade-rate-v0", "--trade-rate zz-trade-rate-v0", {}, [], [])   # C3's rows: not yet listed


@pytest.mark.parametrize("mode", ["pm6-6", "pm6-6-engine"])
def test_the_match_stage_still_writes_its_own_leverage(tmp_path, mode):
    """Ruling E2-14: the reservation leaves the gross-match stage's own write alone, in both modes, on P9-C's shape
    (ruling E2-13 (b)): a kind rule wave naming a NAV rule that is no leverage rule (a synthetic trade-rate row), the
    template keeping the parent's --aim-leverage {leverage}, the gross mismatched. Preflight passes; L' (4 decimals)
    reaches the cell of record's nav.leverage and so its --aim-leverage. Python pm6-6: the -gm copy. Engine (P9 C2):
    the run recipe -cal at the cell's L carries --calibrate-gross G_parent and --calibrate-tolerance (the match stage
    writes them after preflight); the cell of record -gm is the plain recipe at L' (no --calibrate-*), and verify pins
    its digest (C2 R4, merged with root)."""
    m = load_fixture("new-nav-rule.json")
    m.update(gross_match=mode, rules=[{"name": TRADE_RATE["id"]}])
    root = world(tmp_path / mode, m)
    fake = KindsFake(root, nav=dict(NAV_TABLE, rules=NAV_TABLE["rules"] + [TRADE_RATE]),
                     gross_per_l={f"p9k-rule-nav-{m['wave']}": 0.9604 / 1.1474})
    code, log = wave(root, fake, "run", "--until", "verify")
    assert code == 0, log
    receipts = root / "out/waves" / m["wave"] / "receipts"
    pre = json.loads((receipts / "01-preflight.json").read_text())["outputs"]["kind"]
    assert pre["flags"] == {"nav": {"--trade-rate": TRADE_RATE["id"]}}
    mt = json.loads((receipts / "06-match.json").read_text())["outputs"]
    lev = mt["leverage"]
    assert mt["corrected"] is True and lev != "1.1474" and lev == f"{float(lev):.4f}"
    record = mt.get("record_spec") or mt["cell_spec"]
    assert record.endswith("-gm.json")
    nav = RC.load_spec(root / record)["nav"]
    assert nav["leverage"] == lev and nav["flags"][:2] == NAV_SET and "--trade-rate" in nav["flags"]
    assert not [f for f in nav["flags"] if WK.match_flag_reads(f)]
    if mode == "pm6-6":
        assert "record_spec" not in mt
        return
    run = RC.load_spec(root / mt["cell_spec"])["nav"]                       # the run recipe: the cell's L, calibrating
    assert mt["engine"] is True and mt["cell_spec"].endswith("-cal.json") and run["leverage"] == "1.1474"
    assert run["flags"][run["flags"].index(CAL) + 1] == repr(mt["g_parent"]) and "--calibrate-tolerance" in run["flags"]
    verify = json.loads((receipts / "07-verify.json").read_text())["inputs"]
    assert verify["match.record_spec_digest"] == mt["record_spec_digest"]


# ------------------------------------------------------------------ ruling E2-15: the reference side of a cell
REF_OK = {"output": "out/ref-k", "combined": "library"}      # a reference construction with no flags / L of its own
E2_15_ROUTES = [   # (fixture, manifest edit, template edit, the route the refusal names): ruling E2-15 (b)
    ("new-horizon.json", constants(set={"ref": REF_OK}), None, "rule_cell.constants.set ref"),
    ("new-combination-rule.json", constants(set={"ref.leverage": "1.1474"}), None,
     "rule_cell.constants.set ref.leverage"),
    ("new-horizon.json", constants(flags={"ref": {"--cadence": "5"}}), None, "rule_cell.constants.flags ref"),
    ("new-universe.json", None, tpl_set(**{"ref.flags": NAV_SET}), "rule_cell.template change.set ref.flags"),
    ("new-universe.json", None, lambda t: t["change"].update(unset=["ref"]), "rule_cell.template change.unset ref"),
    ("new-universe.json", None, lambda t: t["change"].update(unset={"ref.leverage": True}),
     "rule_cell.template change.unset ref.leverage"),
    ("new-universe.json", None, lambda t: t["change"].update(flags={"ref": {"--cadence": "5"}}),
     "rule_cell.template change.flags ref"),
    ("new-universe.json", None, lambda t: t.update(ref=REF_OK), "rule_cell.template ref (a literal top-level key)"),
    ("leverage-change.json", lambda m: m.update(rule_cell={"template": RULE_NAV, "template_sha256": "", "constants": {
        "set": {"ref": REF_OK}}}), None, "rule_cell.constants.set ref"),
    ("new-universe.json", constants(set={"ref": dict(REF_OK, output="out/ref-other")}), tpl_set(ref=REF_OK),
     "rule_cell.constants.set ref"),                     # replacing the template's own block is a manifest route
]


@pytest.mark.parametrize("name, edit, tedit, route", E2_15_ROUTES)
def test_a_cell_may_not_set_its_ref_section(tmp_path, name, edit, tedit, route):
    """Ruling E2-15 (b) (review E2 S1): the reference of a v2 cell is half of its paired test, so no route adds,
    replaces, unsets or edits its ref section (constants.set / .flags, the template's change.set ref.* / change.unset /
    change.flags, a literal key, in every kind with a rule cell); it comes only from the lineage parent's cell of record
    or the template's own pinned block (change.set ref, whole). Refused at preflight naming key and route; the
    constants.set ref probe passed preflight and spec at d2cea6a6."""
    stopped_at_preflight(*probe(tmp_path, name, edit, tedit), f"{route}: a cell may not add, replace or edit its ref "
                         "section by any route (ruling E2-15 (b))")


E2_15_SIDE = [   # (the template's own ref block, what the refusal says): ruling E2-15 (a)
    (dict(REF_OK, leverage="5.0"), "rule_cell.template change.set ref: the cell's ref.leverage would be '5.0' where "
     "the parent has '1.1474'; the reference side is judged as the cell's (ruling E2-15 (a))"),
    (dict(REF_OK, flags=NAV_SET + ["--vol-target", "vol-target-v1"]),
     "rule_cell.template change.set ref: the cell's ref.flags adds token(s) ['--vol-target']"),
    (dict(REF_OK, flags=["--aim-leverage", "5.0"]),
     "rule_cell.template change.set ref: the cell's ref.flags gives token(s) ['--aim-leverage'] other values"),
    (dict(REF_OK, flags=NAV_SET + ["--Vol-Tar", "vol-target-v1"]),
     "rule_cell.template change.set ref: the cell's ref.flags adds token(s) ['--Vol-Tar']"),
    (dict(REF_OK, flags=NAV_SET + [CAL, "0.95"]),
     "rule_cell.template change.set ref: the cell's ref.flags carries ['--calibrate-gross']"),
]


@pytest.mark.parametrize("ref, needle", E2_15_SIDE)
def test_the_reference_side_is_guarded_as_the_cell(tmp_path, ref, needle):
    """Ruling E2-15 (a): the template's own ref block is allowed, and its resolved side (research_cycle's ref_step: the
    cell's nav with the ref's own rule / leverage / flags) is judged as the cell's: E2-10 on its leverage, E2-8 on its
    flags (another value of an owned flag, an unnamed rule, a case variant), E2-14 on the match flags. Review S1's
    probe (a ref at L 5.0 with an unnamed --vol-target) passed preflight and spec at d2cea6a6."""
    stopped_at_preflight(*probe(tmp_path, "new-combination-rule.json", gross_none, tpl_set(ref=ref)), needle)


def test_the_templates_own_ref_block_is_allowed(tmp_path):
    """Ruling E2-15 (b): the template's own pinned ref block with no flags or leverage of its own runs the cell's
    construction; the wave passes preflight and the spec stage writes the cell with it."""
    code, log, root, _ = probe(tmp_path, "new-combination-rule.json", gross_none, tpl_set(ref=REF_OK),
                               "run", "--until", "spec")
    assert code == 0, log
    assert RC.load_spec(root / cell_of(root)["cell_spec"])["ref"] == REF_OK


def nav_receipt(root: Path, run_dir: str, exe: str) -> None:
    """A completed NAV run receipt recording ``exe`` as its executable_sha256 (test_wave_driver's helper's layout)."""
    F.write_json(root, f"{run_dir}/receipt.json", {"outcome": "completed", "exit_code": 0, "wall_seconds": 2.0,
                                                   "sampled_peak_tree_rss_bytes": 3 << 20, "executable_sha256": exe})


@pytest.mark.parametrize("route, parent_exe, refused", [
    ("none", "a" * 64, "W1-I2"),                   # no ref: refused as before (the control)
    ("constants", "a" * 64, "E2-15 (b)"),          # the manifest sets a ref: stops at preflight, never reaches verify
    ("template", "a" * 64, "E2-15 (c)"),           # the template's own ref, run on the cell's exe: still refused
    ("template", "b" * 64, None),                  # like with like: the ref does not refuse a sound pair
])
def test_w1_i2_a_holds_through_a_ref(tmp_path, route, parent_exe, refused):
    """Ruling E2-15 (c): W1-I2 (a) refuses a rule cell whose paired parent NAV was made by another NAV exe through
    every ref route. summ pairs a rule cell with the parent's NAV (inputs.reference_cell), never with its ref, so a
    ref that ran on the cell's exe proves nothing about that pair. The probe at d2cea6a6 (parent NAV by exe a, the
    cell's NAV and its ref by exe b): verify exit 4 without a ref, exit 0 (nav_exe ref "ran") with a ref set by
    constants.set or by the template's own block."""
    m = load_fixture("new-horizon.json")
    gross_none(m)
    if route == "constants":
        constants(set={"ref": REF_OK})(m)
    root = world(tmp_path / route, m)
    if route == "template":
        set_template(root, m, tpl_set(ref=REF_OK))
    fake = KindsFake(root)
    nav_receipt(root, f"{F.PARENT_NAV}-run", parent_exe)
    code, log = wave(root, fake, "run", "--until", "match")
    if route == "constants":
        stopped_at_preflight(code, log, root, m, "ruling E2-15 (b)")
        return
    assert code == 0, log
    receipts = root / "out/waves" / m["wave"] / "receipts"
    mt = json.loads((receipts / "06-match.json").read_text())["outputs"]
    nav_receipt(root, f"{mt['nav']}-run", "b" * 64)
    spec = RC.load_spec(root / mt["cell_spec"])
    if route == "template":
        assert spec["ref"] == REF_OK
        nav_receipt(root, f"{RC.Cycle(spec, RC.Resolver(root), verify=False).out(REF_OK['output'])}-run", "b" * 64)
    code, log = wave(root, fake, "run", "--until", "verify")
    if refused:
        assert code == 4, log
        text = json.loads((receipts / "07-verify.failed-1.json").read_text())["error"]
        assert f"rule cell: its paired parent NAV was made by NAV exe {'a' * 64}" in text and refused in text, text
    else:
        assert code == 0, log
        exe = json.loads((receipts / "07-verify.json").read_text())["outputs"]["nav_exe"]
        assert exe == {"parent": "b" * 64, "cell": "b" * 64, "equal": True, "ref": "ran"}


def test_non_finite_params_are_refused_at_preflight(tmp_path):
    """Review E2 S2: json.loads reads NaN / Infinity, and NaN passes every minimum / maximum comparison: a non-finite
    number is refused by the params_schema check (fixed-v1's aim_leverage NaN passed preflight at ea7d9dde)."""
    for v in (float("nan"), float("inf"), float("-inf")):
        got = WK.schema_problems(v, LEVERAGE, "p")
        assert len(got) == 1 and got[0].startswith(f"p: {v!r} is not a finite number"), got
    assert WK.schema_problems(2.0, LEVERAGE, "p") == [] and WK.schema_problems(1, LEVERAGE, "p") == []
    for label, v in (("nan", float("nan")), ("inf", float("inf"))):
        code, log, root, m = probe(tmp_path / label, "new-nav-rule.json",
                                   lambda m, v=v: m.update(rules=[{"name": "fixed-v1", "params": {"aim_leverage": v}}]),
                                   None)
        stopped_at_preflight(code, log, root, m, f"rules.fixed-v1.aim_leverage: {v!r} is not a finite number")


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
    refusal = ("(a rule cell, with or without its template's own ref construction (ruling E2-15 (c)), or a library "
               "cell without one is refused when the two NAVs' exes differ or only one is recorded, by receipt or "
               "exes_sha256 pin (ruling W1-I2 (a)); a library cell with one, differing, its ref must have run on the "
               "cell's exe)")
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


# ------------------------------------------------------------------ P9 lane D3: lane C3's rows (W3-8a, C3-3)
def c3_row(rid: str, kind: str, flag: str | None, props: dict | None = None, aliases: list | None = None) -> dict:
    """A K-P9-7 row in lane C3's shape (atx-impl/src/book/nav_rules.cpp row_json: x-flag null for a row no flag turns
    on; x-flag-aliases only on a row with a legacy spelling, ruling C3-3)."""
    row = {"id": rid, "kind": kind, "x-flag": flag, "incompatible": [],
           "params_schema": {"type": "object", "additionalProperties": False, "required": [],
                             "properties": props or {}}}
    if aliases is not None:
        row[WK.ALIASES_KEY] = aliases
    return row


def num(flag: str) -> dict:
    return {"type": "number", "x-flag": flag}


COST_V2 = "--cost-v2"                     # cost-sqrt-v2's legacy spelling (C3's x-flag-aliases, ruling C3-3)
C3_ROWS = [   # lane C3's trade-rate and cost-law rows (nav_rules.cpp); test_the_live_rule_tables compares them live
    c3_row("aim-partial-v5", "trade-rate", "--rule aim-partial-v5", {
        "trade_fraction": num("--trade-fraction"), "dust_multiple": num("--dust-multiple"),
        "exit_rate": num("--exit-rate")}),
    c3_row("per-name-v1", "trade-rate", "--rate per-name-v1", {
        "rate_rra": num("--rate-rra"), "rate_lambda": num("--rate-lambda"), "rate_min": num("--rate-min"),
        "rate_max": num("--rate-max")}),
    c3_row("two-speed-nav-v1", "trade-rate", None),
    *[c3_row(rid, "cost-law", None) for rid in ("flat-bps-v1", "sqrt-impact-v1", "ko-invariance-v1", "fim-live-v1")],
    c3_row("cost-sqrt-v2", "cost-law", "--cost-law cost-sqrt-v2", aliases=[COST_V2]),
]
NAV_C3 = dict(NAV_TABLE, rules=NAV_TABLE["rules"] + C3_ROWS)


def edit_parent(root: Path, edit) -> None:
    """The fake world's parent spec (the wave's lineage parent's cell of record) edited by ``edit``, committed."""
    spec = json.loads((root / F.PARENT).read_text(encoding="utf-8"))
    edit(spec)
    F.write_json(root, F.PARENT, spec)
    F.git(root, "commit", "-q", "-am", "the parent spec edited")


def run_probe(tmp_path: Path, name: str, edit=None, tedit=None, pedit=None, *args: str, **fake):
    """probe() with a parent edit ``pedit`` too: (code, log, root, manifest); default run: up to preflight."""
    m = load_fixture(name)
    if edit:
        edit(m)
    if m.get("rule_cell") and not m["rule_cell"].get("template_sha256"):
        m["rule_cell"]["template_sha256"] = sha((REPO / m["rule_cell"]["template"]).read_bytes())
    root = world(tmp_path / "w", m)
    if pedit:
        edit_parent(root, pedit)
    if tedit:
        set_template(root, m, tedit)
    code, log = wave(root, KindsFake(root, **fake), *(args or ("run", "--until", "preflight")))
    return code, log, root, m


def preflight_kind_out(root: Path, m: dict) -> dict:
    receipt = root / "out/waves" / m["wave"] / "receipts" / "01-preflight.json"
    return json.loads(receipt.read_text())["outputs"]["kind"]


def engine_fit(spec: dict, *more: str) -> None:
    """A parent fitted under the C++ composition fit (D2's fitter flag --engine-fit), with ``more`` fit flags."""
    spec["fit"]["flags"] = [*spec["fit"]["flags"], WK.WALK_FORWARD_PARENT_FIT, *more]


def with_cost_v2(spec: dict) -> None:
    """A parent whose NAV runs cost model v2 by its legacy spelling (a pre-C3 cell of record)."""
    spec["nav"]["flags"] = [*spec["nav"]["flags"], COST_V2]


@pytest.mark.parametrize("name", sorted(OR6_ROWS))
def test_every_fixture_runs_preflight_on_c3s_rows(tmp_path, name):
    """G-P2, ruling W3-8a: all ten kinds fixtures are runnable, none schema only: each passes preflight (exit 0) in the
    fake world served E2's NAV table plus lane C3's trade-rate and cost-law rows (the walk-forward fixture on a parent
    fitted under --engine-fit, the only parent it accepts)."""
    pedit = engine_fit if load_fixture(name)["kind"] == "walk-forward" else None
    code, log, root, m = run_probe(tmp_path, name, None, None, pedit, nav=NAV_C3)
    assert code == 0, log
    assert preflight_kind_out(root, m)["kind"] == m["kind"]


def test_the_cost_model_fixture_runs_to_spec_on_c3s_rows(tmp_path):
    """Ruling W3-8a (P14, G-P2): new-cost-model.json names cost-sqrt-v2; on lane C3's rows its flag is the row's x-flag
    (--cost-law cost-sqrt-v2) and the cell the spec stage writes carries it (on a table without the row the name is in
    neither table: test_rule_wave_refuses_a_name_no_table_lists)."""
    code, log, root, m = run_probe(tmp_path, "new-cost-model.json", None, None, None, "run", "--until", "spec",
                                   nav=NAV_C3)
    assert code == 0, log
    assert preflight_kind_out(root, m)["flags"] == {"nav": {"--cost-law": "cost-sqrt-v2"}}
    out = cell_of(root)
    doc = json.loads((root / out["cell_spec"]).read_text())
    assert out["kind"] == "rule" and doc["change"]["flags"] == {"nav": {"--cost-law": "cost-sqrt-v2"}}
    assert RC.load_spec(root / out["cell_spec"])["nav"]["flags"] == NAV_SET + ["--cost-law", "cost-sqrt-v2"]


# ------------------------------------------------------------------ P9 lane D3: kind walk-forward (D3-1, D3-2)
WF_FLAGS = {"--walk-forward": "expanding", "--walk-forward-lag": "2", "--walk-forward-step": "21",
            "--walk-forward-min-decisions": "21"}


def test_walk_forward_runs_to_spec(tmp_path):
    """Kind walk-forward out of SCHEMA_ONLY (P14, PRE-35): on a parent fitted under --engine-fit the wave passes
    preflight and the spec stage writes the cell next to the parent spec: the parent's construction with the fitter's
    flags under ruling D3-2's names at the registered constants (D3-1: expanding, lag 2, step 21, min_decisions 21) and
    every output downstream of the fit renamed -wf2s21m21 (the u pass, the paired reference and L are the parent's)."""
    assert tuple(WF_FLAGS) == WK.WALK_FORWARD_FLAGS                                  # ruling D3-2's names, in order
    m = load_fixture("walk-forward.json")
    assert m["walk_forward"] == {"window": "expanding", "lag": 2, "step": 21, "min_decisions": 21}   # D3-1
    assert WK.walk_forward_flags(m["walk_forward"]) == {"fit": WF_FLAGS}
    short = {"lag": 2, "step": 21}                       # min_decisions left out: the fitter's own default, no m token
    assert WK.walk_forward_flags(short) == {"fit": {k: v for k, v in WF_FLAGS.items() if "min" not in k}}
    assert WK.walk_forward_token(short) == "wf2s21" and WK.walk_forward_token(m["walk_forward"]) == "wf2s21m21"
    code, log, root, m = run_probe(tmp_path, "walk-forward.json", None, None, engine_fit, "run", "--until", "spec")
    assert code == 0, log
    pre = preflight_kind_out(root, m)
    assert pre["kind"] == "walk-forward" and pre["walk_forward"] == m["walk_forward"]
    assert pre["flags"] == {"fit": WF_FLAGS}
    assert set(pre["tables"]) == {"ic", "nav"} and all(len(v) == 64 for v in pre["tables"].values())
    out = cell_of(root)
    assert out["cell_spec"] == "scripts/specs/v8/lib-p0-wf2s21m21-k-walk-forward.json" and out["kind"] == "rule"
    doc = json.loads((root / out["cell_spec"]).read_text())
    assert doc["parent"] == Path(F.PARENT).name and doc["change"]["flags"] == {"fit": WF_FLAGS}
    assert doc["change"]["set"] == {"fit.output": "out/fit-p0-wf2s21m21", "card.output": "out/card-p0-wf2s21m21",
                                    "ic.w_output": "out/w-p0-wf2s21m21", "nav.output": "out/nav-p0-L1.1474-wf2s21m21"}
    spec = RC.load_spec(root / out["cell_spec"])
    assert spec["fit"]["flags"] == ["--engine-fit", "--walk-forward", "expanding", "--walk-forward-lag", "2",
                                    "--walk-forward-step", "21", "--walk-forward-min-decisions", "21"]
    assert spec["ic"]["u_output"] == "out/u-p0" and spec["nav"]["leverage"] == "1.1474"
    assert spec["inputs"]["reference_cell"]["dir"] == F.PARENT_NAV and out["reference_nav"] == F.PARENT_NAV


@pytest.mark.parametrize("pedit, needle", [
    (None, "the parent spec's fit.flags must carry --engine-fit (ruling ALCOMB-7"),
    (lambda s: engine_fit(s, "--theme-tsmom", "theme-tsmom-v1"), "refused on a parent fitted with ['--theme-tsmom']"),
    (lambda s: engine_fit(s, "--theme-resid", "theme-resid-v1"), "refused on a parent fitted with ['--theme-resid']"),
    (lambda s: engine_fit(s, "--era=e1"), "refused on a parent fitted with ['--era']"),
])
def test_walk_forward_refuses_a_parent_it_cannot_refit(tmp_path, pedit, needle):
    """The walk-forward cell refits its parent's fit with the C++ composition exe: a parent not fitted under
    --engine-fit (tiny.json's Python ew-theme-v1 fit, the fake world's), or fitted with --theme-tsmom, --theme-resid or
    --era (their TRAIN-fitted schedules, orders and pools are not refitted per block), is refused at preflight."""
    stopped_at_preflight(*run_probe(tmp_path, "walk-forward.json", None, None, pedit), needle,
                         "kind walk-forward")


@pytest.mark.parametrize("edit", [
    lambda wf: wf.update(lag=1), lambda wf: wf.update(lag=True), lambda wf: wf.update(lag="2"),
    lambda wf: wf.update(step=0), lambda wf: wf.update(min_decisions=1), lambda wf: wf.update(min_decisions=2.5),
    lambda wf: wf.update(window="rolling"), lambda wf: wf.update(rows=[]), lambda wf: wf.pop("step"),
])
def test_walk_forward_block_refusals(edit):
    """The walk_forward block (K-P9-8): lag >= 2 (fitted on decisions <= d-2), step > 0, min_decisions >= 2, integers
    (no bool), window expanding only, no other key; refused when the manifest is read (exit 2)."""
    m = load_fixture("walk-forward.json")
    assert WM.validate(m) == []
    edit(m["walk_forward"])
    assert "walk_forward must be {lag: >= 2" in "; ".join(WM.validate(m))


WF_RAW = [   # (fixture, manifest edit, template edit, the refusal's text): KIND_FLAGS (E2-8) in every other kind
    ("new-combination-rule.json", constants(flags={"fit": {"--walk-forward": "expanding"}}), None,
     "rule_cell.constants.flags sets ['fit --walk-forward'], which the exes' rules own"),
    ("new-combination-rule.json", constants(flags={"fit": {"--walk-forward-l": "2"}}), None,
     "rule_cell.constants.flags sets [\"fit --walk-forward-l (abbreviates ['--walk-forward-lag'])\"]"),
    ("new-universe.json", None, lambda t: t["change"].update(flags={"fit": {"--Walk-Forward-Step": "21"}}),
     "rule_cell.template change.flags sets ['fit --Walk-Forward-Step']"),
    ("new-horizon.json", constants(set={"fit.flags": ["--walk-forward", "expanding"]}), None,
     "rule_cell.constants.set fit.flags: the cell's fit.flags adds token(s) ['--walk-forward']"),
    ("leverage-change.json", lambda m: m.update(rule_cell={"template": RULE_NAV, "template_sha256": "", "constants": {
        "set": {"fit.flags": ["--walk-forward-lag=2"]}}}), None,
     "rule_cell.constants.set fit.flags: the cell's fit.flags adds token(s) ['--walk-forward-lag=2']"),
]


@pytest.mark.parametrize("name, edit, tedit, needle", WF_RAW)
def test_walk_forward_flags_are_the_kinds_own(tmp_path, name, edit, tedit, needle):
    """Kind walk-forward's fitter flags (WK.KIND_FLAGS) are owned like a rule's (ruling E2-8): no other kind reaches
    them by any route or spelling (an option map, constants.set / change.set of fit.flags, =value, another case, an
    abbreviation); preflight refuses (exit 3, before register), naming token and route."""
    stopped_at_preflight(*run_probe(tmp_path, name, edit, tedit), needle, "ruling E2-8")


# ------------------------------------------------------------------ P9 lane D3: ruling C3-3 (x-flag-aliases)
C3_3_PROBES = [   # (fixture, manifest edit, template edit, parent edit, the refusal's text)
    ("new-horizon.json", lambda m: m["horizon"]["flags"]["nav"].update({COST_V2: True}), None, None,
     "horizon.flags sets ['nav --cost-v2'], which the exes' rules own"),
    ("new-nav-rule.json", constants(flags={"nav": {COST_V2: True}}), None, None,
     "rule_cell.constants.flags sets ['nav --cost-v2'], which the exes' rules own"),
    ("new-universe.json", None, lambda t: t["change"].update(flags={"nav": {COST_V2: True}}), None,
     "rule_cell.template change.flags sets ['nav --cost-v2'], which the exes' rules own"),
    ("new-nav-rule.json", constants(flags={"nav": {"--cost": True}}), None, None,               # an abbreviation
     "rule_cell.constants.flags sets [\"nav --cost (abbreviates ['--cost-law', '--cost-v2'])\"]"),
    ("new-nav-rule.json", constants(flags={"nav": {"-Cost-V2": True}}), None, None,             # dash and case
     "rule_cell.constants.flags sets ['nav -Cost-V2'], which the exes' rules own"),
    ("new-combination-rule.json", constants(set={"nav.flags": NAV_SET + [COST_V2]}), None, None,
     "rule_cell.constants.set nav.flags: the cell's nav.flags adds token(s) ['--cost-v2']"),
    ("new-universe.json", None, tpl_set(**{"nav.flags": NAV_SET + [COST_V2]}), None,
     "rule_cell.template change.set nav.flags: the cell's nav.flags adds token(s) ['--cost-v2']"),
    # inherited from the lineage parent: removed or changed only by a wave whose rules name cost-sqrt-v2
    ("new-nav-rule.json", constants(set={"nav.flags": NAV_SET}), None, with_cost_v2,
     "rule_cell.constants.set nav.flags: the cell's nav.flags drops token(s) ['--cost-v2']"),
    ("new-universe.json", None, lambda t: t["change"].update(flags={"nav": {COST_V2: False}}), with_cost_v2,
     "rule_cell.template change.flags sets ['nav --cost-v2'], which the exes' rules own"),
    ("new-horizon.json", constants(set={"nav.flags": NAV_SET + [COST_V2]}), None,                # spelling switched
     lambda s: s["nav"]["flags"].extend(["--cost-law", "cost-sqrt-v2"]),
     "the cell's nav.flags adds token(s) ['--cost-v2']"),
    # the named row: its x-flag and its legacy spelling, one of the two, never both (cell side, ref side)
    ("new-cost-model.json", None, None, with_cost_v2,
     "rules names cost-sqrt-v2, which derives nav --cost-law, but the cell's nav.flags still carry its legacy "
     "spelling --cost-v2 (ruling C3-3"),
    ("new-cost-model.json", None, tpl_set(ref=dict(REF_OK, flags=NAV_SET + ["--cost-law", "cost-sqrt-v2", COST_V2])),
     None, "rules names cost-sqrt-v2, which derives nav --cost-law, but the cell's ref.flags still carry its legacy "
           "spelling --cost-v2 (ruling C3-3"),
]


@pytest.mark.parametrize("name, edit, tedit, pedit, needle", C3_3_PROBES)
def test_c3_3_an_alias_is_owned_as_its_rows_flag(tmp_path, name, edit, tedit, pedit, needle):
    """Ruling C3-3: a NAV row's legacy spelling (x-flag-aliases; C3's cost-sqrt-v2 lists --cost-v2) is the owned flag
    it stands for (E2-8): no raw route adds, removes or changes it, in any spelling, unless the wave's rules name the
    row; and a wave naming the row may not leave the alias beside the derived x-flag (cell or ref side). Refused at
    preflight (exit 3, before register), naming token and route."""
    stopped_at_preflight(*run_probe(tmp_path, name, edit, tedit, pedit, nav=NAV_C3), needle)


def test_c3_3_an_inherited_alias_passes_and_the_named_row_drops_it(tmp_path):
    """Ruling C3-3, the allowed routes: an alias inherited unchanged from the lineage parent passes every kind's
    preflight; a wave naming cost-sqrt-v2 drops the parent's --cost-v2 (its template's change.flags) and its cell
    runs --cost-law cost-sqrt-v2 alone."""
    for name in ("new-horizon.json", "new-nav-rule.json", "new-universe.json"):
        code, log, _, _ = run_probe(tmp_path / name[:-5], name, None, None, with_cost_v2, nav=NAV_C3)
        assert code == 0, (name, log)
    code, log, root, m = run_probe(tmp_path / "named", "new-cost-model.json", None,
                                   lambda t: t["change"].update(flags={"nav": {COST_V2: False}}), with_cost_v2,
                                   "run", "--until", "spec", nav=NAV_C3)
    assert code == 0, log
    spec = RC.load_spec(root / cell_of(root)["cell_spec"])
    assert spec["nav"]["flags"] == NAV_SET + ["--cost-law", "cost-sqrt-v2"]
    assert SK.alias_exempt(Wave(F.MANIFEST, root, executor=KindsFake(root), log=lambda s: None),
                           {"nav": NAV_C3}) == {COST_V2}


@pytest.mark.parametrize("aliases, needle", [
    ("--cost-v2", "not a non-empty list of distinct long options"),
    ([], "not a non-empty list of distinct long options"),
    (["cost-v2"], "not a non-empty list of distinct long options"),
    (["--Cost-V2"], "not a non-empty list of distinct long options"),
    (["--cost-v2=1"], "not a non-empty list of distinct long options"),
    (["--cost v2"], "not a non-empty list of distinct long options"),
    (["--"], "not a non-empty list of distinct long options"),
    ([None], "not a non-empty list of distinct long options"),
    ([COST_V2, COST_V2], "not a non-empty list of distinct long options"),
    (["--cost-law"], "lists the alias --cost-law, which a rule of the tables owns as its own flag"),
    (["--aim-leverage"], "lists the alias --aim-leverage, which a rule of the tables owns as its own flag"),
    (["--rate-rra"], "lists the alias --rate-rra, which a rule of the tables owns as its own flag"),
    (["--two-speed"], "lists the alias --two-speed, which a rule of the tables owns as its own flag"),
])
def test_c3_3_a_malformed_alias_field_stops_the_wave(tmp_path, aliases, needle):
    """Ruling C3-3, fail closed: an x-flag-aliases field the guard cannot read (not a non-empty list of distinct
    lowercase long options), an alias two rows list, or an alias that is a rule's own flag (naming its row would exempt
    another rule's flag) stops every v2 wave at preflight, naming the row."""
    assert WK.alias_problems({"nav": NAV_C3}) == [] and WK.flag_aliases({"nav": NAV_C3}) == {COST_V2: "cost-sqrt-v2"}
    bad = json.loads(json.dumps(NAV_C3))
    next(r for r in bad["rules"] if r["id"] == "cost-sqrt-v2")[WK.ALIASES_KEY] = aliases
    assert needle in "; ".join(WK.alias_problems({"nav": bad}))
    if aliases == ["--cost-law"]:                                        # one end to end: preflight stops the wave
        stopped_at_preflight(*run_probe(tmp_path, "new-horizon.json", nav=bad), "'cost-sqrt-v2' lists the alias "
                             "--cost-law", "ruling C3-3")


def test_c3_3_two_rows_may_not_share_an_alias():
    bad = json.loads(json.dumps(NAV_C3))
    next(r for r in bad["rules"] if r["id"] == "aim-partial-v5")[WK.ALIASES_KEY] = [COST_V2]   # a constant pair
    assert WK.alias_problems({"nav": bad}) == ["the NAV exe's rows 'aim-partial-v5' and 'cost-sqrt-v2' both list the "
                                               "alias --cost-v2 (ruling C3-3: an alias stands for one row's flag)"]


@pytest.mark.parametrize("rid", ["fixed-v1", "risk-target-v1", "flat-bps-v1"])
def test_c3_3_an_alias_stands_for_a_constant_pair(rid):
    """Ruling C3-3, fail closed: the guard reads an alias as a bare switch standing for its row's whole x-flag
    ``--flag VALUE`` (wave_stage_kinds.reads), so a row whose x-flag takes a param value (fixed-v1's --aim-leverage,
    risk-target-v1's --risk-target S) or has none (flat-bps-v1) may list none."""
    bad = json.loads(json.dumps(NAV_C3))
    next(r for r in bad["rules"] if r["id"] == rid)[WK.ALIASES_KEY] = ["--legacy-x"]
    got = "; ".join(WK.alias_problems({"nav": bad}))
    assert f"the NAV exe's row '{rid}' lists x-flag-aliases but its x-flag" in got, got


# ------------------------------------------------------------------ P9 lane D3: ruling E2-17 (inputs.*)
OTHER_INPUT = {"path": "out/other/train_combined.json", "sha256": None}
E2_17_PROBES = [   # (fixture, manifest edit, template edit, the refusal's text): E2 re-review 4 O2 and every route
    ("new-combination-rule.json", constants(set={"inputs.reference_combined": OTHER_INPUT}), None,   # the O2 probe
     "inputs.reference_combined by rule_cell.constants.set inputs.reference_combined: a kind rule wave may set or "
     "change no inputs.* key (ruling E2-17"),
    ("new-nav-rule.json", None, lambda t: t["change"].update(inputs={"library": OTHER_INPUT}),
     "inputs.library by rule_cell.template change.inputs.library: a kind rule wave may set or change no inputs.* key"),
    ("new-nav-rule.json", None, lambda t: t["change"].update(unset=["inputs.reference_cell"]),
     "inputs.reference_cell by rule_cell.template change.unset inputs.reference_cell: a kind rule wave"),
    ("new-nav-rule.json", None, tpl_set(**{"inputs.role.path": "role-other/manifest.json"}),
     "inputs.role by rule_cell.template change.set inputs.role.path: a kind rule wave"),
    ("new-combination-rule.json", constants(set={"inputs": {"library": OTHER_INPUT}}), None,         # whole section
     "inputs.* by rule_cell.constants.set inputs"),
    ("new-nav-rule.json", constants(set={"inputs.role": {"path": "role/manifest.json", "sha256": None}}), None,
     "inputs.role by rule_cell.constants.set inputs.role: a kind rule wave"),           # the parent's value: a route
    ("new-horizon.json", constants(set={"inputs.role": OTHER_INPUT}), None,
     "inputs.role by rule_cell.constants.set inputs.role: a kind horizon wave may set or change no inputs.* key"),
    ("new-universe.json", None, lambda t: t["change"].update(inputs={"library": OTHER_INPUT}),
     "inputs.library by rule_cell.template change.inputs.library: a kind role wave may set or change inputs.role"),
    ("leverage-change.json", lambda m: m.update(rule_cell={"template": RULE_NAV, "template_sha256": "", "constants": {
        "set": {"inputs.reference_admission": OTHER_INPUT}}}), None,
     "inputs.reference_admission by rule_cell.constants.set inputs.reference_admission: a kind leverage wave"),
]


@pytest.mark.parametrize("name, edit, tedit, needle", E2_17_PROBES)
def test_e2_17_inputs_only_by_the_kinds_that_name_them(tmp_path, name, edit, tedit, needle):
    """Ruling E2-17: in a v2 wave inputs.* is set or changed only by the kinds whose schema defines an input change
    and only the keys they name (WK.KIND_INPUTS: a role wave's inputs.role); any other kind or key, by any route
    (constants.set, the template's change.set / change.unset / change.inputs, a whole inputs section; even to the
    parent's own value), is refused at preflight (exit 3, before register), naming key and route. The O2 probe (a rule
    cell's constants.set inputs.reference_combined) passed preflight at E2's head 80c322ef."""
    stopped_at_preflight(*run_probe(tmp_path, name, edit, tedit), needle)


def test_e2_17_a_role_wave_changes_inputs_role(tmp_path):
    """Ruling E2-17, the allowed route: a role wave's template may name inputs.role (KIND_INPUTS), and its cell's
    inputs.role is the register stage's build."""
    code, log, root, _ = run_probe(tmp_path, "new-universe.json", None, lambda t: t["change"].update(
        inputs={"role": {"path": "role/manifest.json", "sha256": None}}), None, "run", "--until", "spec")
    assert code == 0, log
    assert RC.load_spec(root / cell_of(root)["cell_spec"])["inputs"]["role"]["path"] == "role-k-role/manifest.json"


def test_e2_17_a_resolved_difference_without_a_route_is_refused():
    """Ruling E2-17: a resolved inputs difference no raw route names (inputs_changed: an ordinary key against the
    parent's, a derived key against the parent's own outputs) is refused as "the resolved cell"; the cell every
    template on this parent gets (its derived reference = the parent's NAV) has none."""
    parent = F.cell_spec("p0", F.PARENT_NAV, reference_nav="out/nav-base")
    cell = json.loads(json.dumps(parent))
    cell["inputs"]["reference_cell"] = {"dir": F.PARENT_NAV, "path": f"{F.PARENT_NAV}/summary.json", "sha256": None}
    w = SimpleNamespace(manifest={"schema": WK.SCHEMA_V2, "kind": "horizon"})
    assert SK.inputs_changed(parent, cell) == [] and SK.inputs_refusal(w, parent, cell, {}, None) == []
    cell["inputs"]["library"] = OTHER_INPUT
    stale = dict(parent["inputs"]["reference_cell"])                     # the parent's own reference, not its NAV
    assert SK.inputs_changed(parent, dict(cell, inputs=dict(cell["inputs"], reference_cell=stale))) == \
        ["library", "reference_cell"]
    got = SK.inputs_refusal(w, parent, cell, {}, None)
    assert len(got) == 1 and got[0].startswith("inputs.library by the resolved cell (no raw route names it): a kind "
                                               "horizon wave may set or change no inputs.* key (ruling E2-17"), got
    w.manifest["kind"] = "role"
    assert SK.inputs_refusal(w, parent, dict(cell, inputs=dict(cell["inputs"], library=parent["inputs"]["library"],
                                                               role=OTHER_INPUT)), {}, None) == []


# ------------------------------------------------------------------ P9 lane D3: the live tables (root M2h's check)
LIVE_EXES = ("atx-equity-strategy-ic.exe", "atx-equity-strategy-targets.exe")


def live_bin() -> Path | None:
    """ATX_EQUITY_BIN when it names a bin dir holding both exes (the IC exe and the NAV exe), else None."""
    raw = os.environ.get("ATX_EQUITY_BIN")
    return Path(raw) if raw and all((Path(raw) / exe).is_file() for exe in LIVE_EXES) else None


def live_listing(exe: Path, nav: bool) -> tuple[str, dict | None]:
    done = subprocess.run(WK.listing_argv(str(exe), nav), capture_output=True, text=True, timeout=120, check=False)
    assert done.returncode == 0, done.stderr
    return done.stdout, WK.parse_listing(done.stdout, WK.NAV_RULES if nav else WK.IC_RULES)


@pytest.mark.skipif(live_bin() is None, reason="ATX_EQUITY_BIN does not name a bin dir with both exes")
def test_the_live_rule_tables(tmp_path):
    """Root M2h's live-table check (m2h-logs/e2-live-table-check.py) as a test, plus lane C3's rows: the exes'
    --list-rules --json envelopes through E2's own argv and parser; the IC table is the committed fixture these tests
    read; every IC rule mapped; fixed-v1's range [1, 2]; C3's cost-law row and its alias (ruling C3-3) as NAV_C3
    mirrors them; then all ten kinds fixtures pass preflight in the fake world served the LIVE tables (walk-forward on
    an --engine-fit parent) and the cost-model and combination-rule fixtures run to spec. No data is read."""
    bin_dir = live_bin()
    ic_text, ic = live_listing(bin_dir / LIVE_EXES[0], False)
    _, nav = live_listing(bin_dir / LIVE_EXES[1], True)
    assert ic is not None and nav is not None
    fixture = IC_FIXTURE.read_text(encoding="utf-8")
    assert ic_text.replace("\r\n", "\n") == fixture.replace("\r\n", "\n") and ic == json.loads(fixture)
    assert ic_table(False) == ic
    tables = {"ic": ic, "nav": nav}
    assert WK.unmapped_ic_rules(tables) == []
    assert WK.rule_flags([{"name": "ew-theme-std-v1"}], {"ic": ic}) == \
        ({"fit": {"--composition": "ew-theme-std-v1"}}, [])
    for rid, flag in (("theme-cov-hedge-v1", "--theme-hedge"), ("mom-volman-v1", "--mom-volman")):
        assert WK.rule_flags([{"name": rid}], {"ic": ic}) == ({"fit": {flag: rid}}, []), rid
    rows = {r["id"]: r for r in nav["rules"]}
    lev = rows["fixed-v1"]["params_schema"]["properties"]["aim_leverage"]
    assert (lev.get("minimum"), lev.get("maximum")) == (1, 2), lev
    assert WK.rule_flags(load_fixture("new-nav-rule.json")["rules"], tables) == ({"nav": {"--aim-leverage": "1.5"}}, [])
    assert WK.alias_problems(tables) == [] and WK.flag_aliases(tables) == {COST_V2: "cost-sqrt-v2"}
    assert WK.rule_flags(load_fixture("new-cost-model.json")["rules"], tables) == \
        ({"nav": {"--cost-law": "cost-sqrt-v2"}}, [])
    for mine in C3_ROWS:                                 # NAV_C3 mirrors the live rows the guard reads
        live = rows[mine["id"]]
        props = live["params_schema"]["properties"]
        assert (live["kind"], live["x-flag"], live.get(WK.ALIASES_KEY)) == \
            (mine["kind"], mine["x-flag"], mine.get(WK.ALIASES_KEY)), mine["id"]
        assert {k: p.get("x-flag") for k, p in props.items()} == \
            {k: p["x-flag"] for k, p in mine["params_schema"]["properties"].items()}, mine["id"]
    for name in sorted(OR6_ROWS):
        pedit = engine_fit if OR6_ROWS[name] == "walk-forward" else None
        until = "spec" if name in ("new-cost-model.json", "new-combination-rule.json", "walk-forward.json") \
            else "preflight"
        code, log, _, _ = run_probe(tmp_path / name[:-5], name, None, None, pedit, "run", "--until", until, ic=ic,
                                    nav=nav)
        assert code == 0, (name, log)
