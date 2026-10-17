"""P9 lane AL-CLOCK: the NAV trade-rate row info-clock-rate-v1 through the wave kinds (lane E2's rule derivation in
wave_kinds.py; ruling ALCOMB-1: a rule enters a cell only through the atx.research-wave/v2 rules block), the print-only
--info-clock-print (ruling ALCLOCK-1: owned by no row, so free in a template or a cell's constants), and the lane's
registration and template (scripts/specs/p9/rules/info-clock-rate-v1.json, scripts/specs/p9/templates/info-clock.json).
Synthetic worlds only (wave_fixture.py's fake root and executor); this lane's NAV row is read from the committed
fixture atx-impl/tests/fixtures/nav_rule_info_clock_rate_v1.json, the row as the exe prints it (nav --list-rules --json,
atx-impl book/nav_rules.cpp), which InfoClockRate.RegistryRowAndParser checks against the exe's listing (review S-4).

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_info_clock_rule.py

  test_rule_flags_put_the_rate_on_the_nav_argv     rule_flags: {"nav": {"--rate": "info-clock-rate-v1"}}; no params
                                                   accepted; two rate rules in one wave are a conflict
  test_raw_rate_flag_refused_print_flag_free       raw_rule_flags (E2-8): --rate and its abbreviations are rule-owned,
                                                   --info-clock-print is free
  test_kind_rule_wave_puts_one_rate_and_one_rule   a fake-world `wave run --until spec` on a kind rule manifest naming
                                                   the row: the cell's NAV argv has --rule aim-partial-v5 once and
                                                   --rate info-clock-rate-v1 once (the C3-5 shape on fakes); a parent
                                                   --rate is replaced, its --info-clock-print kept
  test_constants_route_refuses_the_rate_not_the_print  preflight (E2-8): rule_cell.constants.flags nav --rate refused
                                                   (exit 3); nav --info-clock-print passes
  test_registration_and_template                   K-P9-11 keys, source_sample_end 2019 flagged, the template pin, no
                                                   rules / change.flags in the template, the fragment validates as a v2
                                                   manifest, the template resolves on its nominal parent (Y-F0)
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys

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
REGISTRATION = REPO / "scripts" / "specs" / "p9" / "rules" / "info-clock-rate-v1.json"
TEMPLATE = REPO / "scripts" / "specs" / "p9" / "templates" / "info-clock.json"
IC_FIXTURE = REPO / "atx-impl" / "tests" / "fixtures" / "composition_rules_list.json"   # D1's K-P9-6 table, pinned
# This lane's K-P9-7 row as the NAV exe lists it, pinned (the row alone: the table grows by appends; review S-4).
NAV_ROW_FIXTURE = REPO / "atx-impl" / "tests" / "fixtures" / "nav_rule_info_clock_rate_v1.json"
RULE = "info-clock-rate-v1"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def schema(properties: dict, required: list | None = None) -> dict:
    return {"type": "object", "additionalProperties": False, "required": required or [], "properties": properties}


LEVERAGE = {"type": "number", "x-flag": "--aim-leverage", "minimum": 1, "maximum": 2, "default": 1}
POSITIVE = {"type": "number", "exclusiveMinimum": 0}
# The NAV exe's K-P9-7 table as nav --list-rules --json prints it: lane C1's fixed-v1 row, lane C3's aim-partial-v5
# and per-name-v1 rows (each owns its x-flag's first token and its params' flags), and this lane's row (the fixture).
NAV_TABLE = {"schema": WK.NAV_RULES, "capabilities": ["leverage-rule-per-book", "nav-spec-v1"], "rules": [
    {"id": "fixed-v1", "kind": "leverage", "x-flag": "--aim-leverage", "incompatible": [],
     "params_schema": schema({"aim_leverage": LEVERAGE}, ["aim_leverage"])},
    {"id": "aim-partial-v5", "kind": "trade-rate", "x-flag": "--rule aim-partial-v5",
     "incompatible": ["aim-partial-v6", "spo-v1", "spo-v2", "spo-v3"], "description": "aim-partial-v5",
     "params_schema": schema({"trade_fraction": dict(POSITIVE, maximum=1, default=0.25, **{"x-flag": "--trade-fraction"}),
                              "dust_multiple": {"type": "number", "minimum": 0, "maximum": 0.5, "default": 0.0,
                                                "x-flag": "--dust-multiple"},
                              "exit_rate": dict(POSITIVE, maximum=1, default=1.0, **{"x-flag": "--exit-rate"})})},
    {"id": "per-name-v1", "kind": "trade-rate", "x-flag": "--rate per-name-v1",
     "incompatible": ["aim-partial-v6", "spo-v1", "spo-v2", "spo-v3", "two-speed-v1"], "description": "per-name-v1",
     "params_schema": schema({k: dict(POSITIVE, maximum=m, default=d, **{"x-flag": f})
                              for k, m, d, f in (("rate_rra", 1e6, 10.0, "--rate-rra"),
                                                 ("rate_lambda", 1e6, 0.2, "--rate-lambda"),
                                                 ("rate_min", 1, 0.01, "--rate-min"),
                                                 ("rate_max", 1, 0.15, "--rate-max"))})},
    json.loads(NAV_ROW_FIXTURE.read_text(encoding="utf-8"))]}


def ic_table() -> dict:
    """D1's committed K-P9-6 table with each params_schema in the manifest-params shape (ruling D1-PSCHEMA)."""
    doc = json.loads(IC_FIXTURE.read_text(encoding="utf-8"))
    for row in doc["rules"]:
        row["params_schema"] = {"type": "object", "additionalProperties": False,
                                "properties": {"rule": {"const": row["id"]}}}
    return doc


def tables() -> dict:
    return {"ic": ic_table(), "nav": json.loads(json.dumps(NAV_TABLE))}


class ClockFake(F.FakeCycle):
    """wave_fixture's executor plus the two exes' rule listings (K-P9-6 / K-P9-7)."""

    def __init__(self, root: Path, **kw):
        super().__init__(root, {}, **kw)
        self.tables = tables()

    def __call__(self, argv, root, env=None):
        if list(argv[-2:]) == list(WK.LIST_RULES):
            self.calls.append(list(argv))
            table = self.tables["nav" if argv[1:2] == ["nav"] else "ic"]
            return subprocess.CompletedProcess(argv, 0, json.dumps(table), "")
        return super().__call__(argv, root, env)


def clock_manifest(constants: dict | None = None) -> dict:
    """The kinds fixture "new NAV rule" (tiny-world layout) naming this lane's row instead of fixed-v1."""
    m = json.loads((KINDS_DIR / "new-nav-rule.json").read_text(encoding="utf-8"))
    m["wave"], m["out_dir"] = "k-clock", "waves/k-clock"
    m["rules"] = [{"name": RULE, "params": {}}]
    if constants is not None:
        m["rule_cell"] = dict(m["rule_cell"], constants=constants)
    return m


def world(tmp_path: Path, m: dict, parent_flags: list | None = None) -> Path:
    """The fake root with manifest ``m`` re-rooted on its paths (parent, library, fields pin, ledger, N, out_dir), the
    kinds fixture templates committed, and the parent spec's NAV flags replaced by ``parent_flags`` when given."""
    root = F.build(tmp_path)
    for t in (KINDS_DIR / "templates").glob("*.json"):
        dest = root / t.relative_to(REPO)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(t.read_bytes())
    if parent_flags is not None:
        parent = json.loads((root / F.PARENT).read_text(encoding="utf-8"))
        parent["nav"]["flags"] = parent_flags
        F.write_json(root, F.PARENT, parent)
    out = json.loads(json.dumps(m))
    out["parent"] = {"spec": F.PARENT, "library": "p0"}
    out["fields"] = {"dir": F.FIELDS, "manifest_sha256": sha((root / F.FIELDS / "manifest.json").read_bytes())}
    out["ledger"], out["expect"], out["out_dir"] = F.LEDGER, {"n_before": 2}, f"out/waves/{m['wave']}"
    F.write_json(root, F.MANIFEST, out)
    F.git(root, "add", "-A")
    F.git(root, "commit", "-q", "-m", "v2 manifest naming info-clock-rate-v1")
    return root


def wave(root: Path, fake, *args: str) -> tuple[int, list[str]]:
    log: list[str] = []
    code = research_wave.main([*args[:1], F.MANIFEST, "--root", str(root), *args[1:]], executor=fake, log=log.append)
    return code, log


def receipt(root: Path, wave_id: str, name: str) -> dict:
    return json.loads((root / "out/waves" / wave_id / "receipts" / name).read_text(encoding="utf-8"))


def nav_argv(root: Path, cell_spec: str) -> list[str]:
    """The cell's NAV argv as research_cycle builds it (Cycle.nav_step, the token list the exe receives after the
    runner) on placeholder combined / role pins; the fake world's spec inputs the verifying constructor opens are
    written first (placeholders: nothing runs)."""
    spec = RC.load_spec(root / cell_spec)
    for item in spec["inputs"].values():
        if isinstance(item, dict) and isinstance(item.get("path"), str) and not (root / item["path"]).exists():
            F.write_json(root, item["path"], {"fixture": "AL-CLOCK placeholder"})
    cycle = RC.Cycle(spec, RC.Resolver(root), spec_path=root / cell_spec)
    step = cycle.nav_step("nav", spec["nav"]["output"], "out/w-cell/train_combined.json", "0" * 64, spec["nav"],
                          f"{F.FIELDS}/manifest.json", "role/manifest.json", "0" * 64)
    return step.argv[step.argv.index("--") + 1:]


# ------------------------------------------------------------------ the derivation (wave_kinds)
def test_rule_flags_put_the_rate_on_the_nav_argv():
    t = tables()
    for rules in ([{"name": RULE}], [{"name": RULE, "params": {}}]):
        assert WK.rule_flags(rules, t) == ({"nav": {"--rate": RULE}}, [])
    # every constant is registered: the row takes no parameter
    flags, problems = WK.rule_flags([{"name": RULE, "params": {"theta_fast": 0.2}}], t)
    assert f"rules.{RULE}: unknown key 'theta_fast'" in problems and flags == {}
    # the book's one rate rule: with per-name-v1 the wave sets --rate twice, a conflict named at preflight
    _, problems = WK.rule_flags([{"name": RULE}, {"name": "per-name-v1"}], t)
    assert problems == [f"rules: nav --rate set to '{RULE}' and 'per-name-v1'"]
    # beside the parent's aim rule with its params, the rate is still derived once (how the aim rule's own --rule
    # reaches the cell is lane D3's C3-13 route, not asserted here)
    flags, problems = WK.rule_flags([{"name": "aim-partial-v5", "params": {"trade_fraction": 0.05}},
                                     {"name": RULE}], t)
    assert problems == [] and flags["nav"]["--rate"] == RULE and flags["nav"]["--trade-fraction"] == "0.05"


def test_raw_rate_flag_refused_print_flag_free():
    t = tables()
    derived = {"nav": {"--rate": RULE}}
    owned = WK.table_flags(t)["nav"]
    assert "--rate" in owned and "--info-clock-print" not in owned
    assert WK.raw_rule_flags({"nav": {"--rate": RULE}}, {}, t) == ["nav --rate"]          # named or not: owned
    assert WK.raw_rule_flags({"nav": {"--rate": RULE}}, derived, t) == ["nav --rate"]
    assert WK.raw_rule_flags({"nav": {f"--rate={RULE}": True}}, {}, t) == [f"nav --rate={RULE}"]
    assert WK.raw_rule_flags({"nav": {"--Rate": RULE}}, {}, t) == ["nav --Rate"]           # case-folded (E2-10)
    got = WK.raw_rule_flags({"nav": {"--rat": RULE}}, {}, t)
    assert len(got) == 1 and got[0].startswith("nav --rat (abbreviates ['--rate'")
    # ruling ALCLOCK-1: the print is no rule's flag; a reference and its cell may both carry it
    assert WK.raw_rule_flags({"nav": {"--info-clock-print": True, "--cadence": "1"}}, derived, t) == []


# ------------------------------------------------------------------ the wave in a fake world
def test_kind_rule_wave_puts_one_rate_and_one_rule(tmp_path):
    m = clock_manifest()
    for name, parent_flags in (("plain", None), ("printed", ["--aim-leverage", "{leverage}", "--rate", "fixed",
                                                             "--info-clock-print"])):
        root = world(tmp_path / name, m, parent_flags)
        code, log = wave(root, ClockFake(root), "run", "--until", "spec")
        assert code == 0, log
        pre = receipt(root, m["wave"], "01-preflight.json")["outputs"]["kind"]
        assert pre["kind"] == "rule" and set(pre["tables"]) == {"ic", "nav"}
        out = receipt(root, m["wave"], "04-spec.json")["outputs"]
        doc = json.loads((root / out["cell_spec"]).read_text(encoding="utf-8"))
        assert out["kind"] == "rule" and doc["change"]["flags"] == {"nav": {"--rate": RULE}}
        spec = RC.load_spec(root / out["cell_spec"])
        flags = spec["nav"]["flags"]
        assert spec["nav"]["rule"] == "aim-partial-v5" and "--rule" not in flags
        assert flags.count("--rate") == 1 and flags[flags.index("--rate") + 1] == RULE
        assert flags.count("--info-clock-print") == (1 if parent_flags else 0)
        argv = nav_argv(root, out["cell_spec"])
        assert argv[1] == "nav"
        assert argv.count("--rule") == 1 and argv[argv.index("--rule") + 1] == "aim-partial-v5"
        assert argv.count("--rate") == 1 and argv[argv.index("--rate") + 1] == RULE
        assert argv.count("--info-clock-print") == (1 if parent_flags else 0)
        assert "fixed" not in argv


def test_constants_route_refuses_the_rate_not_the_print(tmp_path):
    m = clock_manifest({"flags": {"nav": {"--rate": RULE}}})
    root = world(tmp_path / "rate", m)
    code, log = wave(root, ClockFake(root), "run", "--until", "preflight")
    assert code == 3, log
    err = receipt(root, m["wave"], "01-preflight.failed-1.json")["error"]
    assert "rule_cell.constants.flags sets ['nav --rate'], which the exes' rules own" in err
    m = clock_manifest({"flags": {"nav": {"--info-clock-print": True}}})
    root = world(tmp_path / "print", m)
    code, log = wave(root, ClockFake(root), "run", "--until", "preflight")
    assert code == 0, log


# ------------------------------------------------------------------ the registration and the template
def test_registration_and_template():
    reg = json.loads(REGISTRATION.read_text(encoding="utf-8"))
    raw = TEMPLATE.read_bytes()
    tpl = json.loads(raw)
    assert b"\r" not in raw and b"\r" not in REGISTRATION.read_bytes()
    assert reg["schema"] == "atx.p9-rule-registration/v1" and reg["id"] == RULE == reg["hypothesis"]
    assert len(RULE) <= 18                                     # W1-I1: an audit-exact cell id at pool-2's depth
    assert reg["cell"] == "P9-C" and reg["status"] == "proposed" and reg["data_class"] == "H"
    assert reg["source_sample_end"] == 2019 and "FLAGGED" in reg["source_sample_flag"]      # ruling ALCLOCK-2
    assert "FIXED CHOICE" in reg["fixed_choices"][0] and "ea_days_since" in reg["fixed_choices"][0]
    assert "ALCLOCK-3" in " ".join(reg["fixed_choices"]) and "C3-4" in reg["realised_leverage"]
    # ruling ALCLOCK-4 (review S-2): the stated bound and the realised figure the limits are judged on, verbatim
    leverage = reg["realised_leverage"]
    assert "ALCLOCK-4" in leverage
    assert ("planned gross <= (theta_f / theta_s) x gross(aim) by contraction outside the dust band, 2.589x at "
            "cadence 1; P9-C's mean gross matched to its parent by pm6-6-engine") in leverage
    assert ("realised gross leverage is read from the cell's own summary (max and mean over the cell) and printed "
            "beside the parent's") in leverage
    assert "the OD5-7 cap test applies to that realised figure" in leverage
    assert "the realised gross the OD5-7 cap reads is the parent's" not in leverage
    assert "DEC-16" in reg["restatement"] and reg["nav_flag"] == {"nav": {"--rate": RULE}}
    assert {c["name"] for c in reg["constants"]} == {"theta_fast", "theta_slow", "window_sessions", "field"}
    frag = reg["manifest_fragment"]
    assert frag["kind"] == "rule" and frag["gross_match"] == "pm6-6-engine"
    assert frag["rules"] == [{"name": RULE, "params": {}}]
    rc = frag["rule_cell"]
    assert rc["template"] == TEMPLATE.relative_to(REPO).as_posix() and rc["template_sha256"] == sha(raw)
    assert rc["source_sample_end"] == 2019 and rc["predicted_mechanism"] == reg["predicted_mechanism"]
    # ruling ALCOMB-1: the template carries no rules and no change.flags; NAV-only renames
    assert tpl["schema"] == "atx.research-cycle-template/v1" and tpl["name"] == "p9-info-clock"
    assert "rules" not in tpl and "flags" not in tpl["change"] and tpl["parent"] is None
    assert set(tpl["change"]) == {"set"} and set(tpl["change"]["set"]) == {"nav.output", "monitor.output"}
    assert "--info-clock-print" not in raw.decode("utf-8").split("Printed")[0]   # named on the parent (ALCLOCK-1)
    # the fragment is a valid v2 manifest beside root's keys (the kinds fixture's)
    m = json.loads((KINDS_DIR / "new-nav-rule.json").read_text(encoding="utf-8"))
    m.update({k: frag[k] for k in ("schema", "kind", "gross_match", "rules")}, rule_cell=dict(rc))
    assert WM.validate(m) == []
    # resolved on its nominal parent (Y-F0): the parent's aim-partial-v5 at .05, its capacity curve (ALCLOCK-3), no
    # rate or two-speed flag of its own; the rule comes only from the wave's rules block
    spec = RC.load_spec(TEMPLATE)
    nav = spec["nav"]
    assert nav["rule"] == "aim-partial-v5" and nav["flags"][nav["flags"].index("--trade-fraction") + 1] == ".05"
    assert "--capacity-curve" in nav["flags"]
    assert not {"--rate", "--two-speed", "--rule"} & set(nav["flags"])
    assert nav["output"] == "build-equity/mega-nav-p9-info-clock"
    assert spec["monitor"]["output"] == "build-equity/mega-monitor-p9-info-clock"
