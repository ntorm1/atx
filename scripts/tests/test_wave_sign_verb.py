"""P9 lane SIGN (ruling C3-11): the wave's PM7-35 sign rule is the IC exe's `sign` verb (atx-equity-strategy-ic sign,
the engine's research/admission pm7_35; atx-impl/src/strategy_sign_verb.hpp), called by the screen stage on the exe
its screen spec pins (exes.ic); the Python copy of the rule is retired.

- identity (the real exe; skipped when it is not built, ATX_EQUITY_IC_EXE overrides the path): on every
  (status, runner_sign, kind, prior) combination of the values test_research_wave.py's PM7-35 test enumerates (status
  admitted | reject_redundant, runner sign 1 | 0 | -1, kind add | replace, prior 1 | -1: their whole product, 24 rows,
  a superset of its eight cases and of the decision table of research_admission_sign_test.cpp) the verb's verdict and
  reason equal the fake verb's (wave_fixture.sign_oracle) and the pinned verdict column PINNED; the receipt pins the
  rows file. Until SIGN's deletion commit the test also held both to wave_rules.sign_pm7_35, the Python rule (the
  identity of record: the pre-deletion head, task-SIGN-report.md); root's identity on the ledgered screen rows is
  sign-identity-ledgered.py in the lane's sprint dir.
- the stage (fakes): the screen stage writes the rows file, runs `<exes.ic> sign --rule pm7-35 --rows .. --output ..`
  once, takes the decision from the receipt (wave-result.json keeps its screen layout) and records the exe, rows and
  receipt with their SHA-256; a string without an admission row, a verb that exits non-zero, a receipt that is not
  the verb's answer to the rows and an IC exe that is not its pin stop the screen; a retried screen writes a new pair;
  the spec stage refuses a receipt changed after the screen (STALE).
- route absent: the ledgered v1 manifests (scripts/specs/v8/waves) still validate and name pm7-35; the spec stage's
  inputs of a screen recorded before SIGN (no sign block) are those of before.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_sign_verb.py
"""
from __future__ import annotations

import hashlib
import json
import os
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
import wave_manifest as WM  # noqa: E402
import wave_rules as WR  # noqa: E402
import wave_stage_library as SL  # noqa: E402
from wave_context import Wave, stage_chain  # noqa: E402

REPO = HERE.parents[1]
EXE = Path(os.environ.get("ATX_EQUITY_IC_EXE", REPO / "build-equity" / "bin" / "atx-equity-strategy-ic.exe"))
STATE = "out/waves/w1"
ONE_DROPPED = {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", -1), "alpha_c": ("admitted", 1)}

# The values test_research_wave.py::test_sign_rule_pm7_35_as_ruled enumerates, and its eight cases.
STATUSES, SIGNS, KINDS, PRIORS = ("admitted", "reject_redundant"), (1, 0, -1), ("add", "replace"), (1, -1)
ENUMERATED = [("admitted", 1, "add", 1), ("admitted", 0, "add", 1), ("admitted", -1, "add", 1),
              ("reject_redundant", -1, "add", 1), ("admitted", 1, "add", -1), ("admitted", 1, "replace", 1),
              ("admitted", 0, "replace", 1), ("reject_redundant", 1, "replace", 1)]
COMBOS = [(s, g, k, p) for s in STATUSES for g in SIGNS for k in KINDS for p in PRIORS]
# The verdict of each COMBOS row (k keep, d drop): B1's decision table (research_admission_sign_test.cpp, the wave
# column), mirrored for prior -1. Pinned here so the identity outlives the Python rule.
PINNED = "kdkd" "kkdd" "dkdk" "kkdd" "kkdd" "kkdd"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def combo_rows() -> list[dict]:
    return [{"id": f"c{n:02d}", "kind": k, "prior_sign": p, "status": s, "runner_sign": g}
            for n, (s, g, k, p) in enumerate(COMBOS)]


def run_exe(tmp: Path, rows: list[dict]) -> tuple[subprocess.CompletedProcess, dict | None, bytes]:
    data = (json.dumps({"rows": rows}, indent=2) + "\n").encode("utf-8")
    (tmp / "rows.json").write_bytes(data)
    done = subprocess.run([str(EXE), "sign", "--rule", "pm7-35", "--rows", str(tmp / "rows.json"), "--output",
                           str(tmp / "receipt.json")], capture_output=True, text=True, timeout=120)
    receipt = json.loads((tmp / "receipt.json").read_text(encoding="utf-8")) if done.returncode == 0 else None
    return done, receipt, data


# ------------------------------------------------------------------ identity: the verb against the Python rule
def test_the_combinations_cover_the_python_test_and_the_decision_table():
    assert set(ENUMERATED) <= set(COMBOS) and len(COMBOS) == len(PINNED) == 24
    table = {(k, s == "admitted", g * p) for s, g, k, p in COMBOS}       # B1's table: kind, admitted, runner vs prior
    assert len(table) == 12


def test_the_fake_verb_is_the_pinned_column():
    for (s, g, k, p), want in zip(COMBOS, PINNED):
        row = {"id": "x", "kind": k, "prior_sign": p, "status": s, "runner_sign": g}
        assert F.sign_oracle(row)[0] == {"k": WR.KEEP, "d": WR.DROP}[want], row
    assert not hasattr(WR, "sign_pm7_35") and not hasattr(WR, "screen_decision")    # the Python rule is gone


@pytest.mark.skipif(not EXE.is_file(), reason=f"{EXE} not built (target atx-equity-strategy-ic)")
def test_the_verb_equals_the_pinned_rule_on_every_enumerated_combination(tmp_path):
    rows = combo_rows()
    done, receipt, data = run_exe(tmp_path, rows)
    assert done.returncode == 0, done.stderr
    assert receipt["schema"] == WR.SIGN_RECEIPT and receipt["rule"] == "pm7-35"
    assert receipt["sign_rule"] == "wave-zero-kept-v1" and receipt["rows_sha256"] == sha(data)
    assert [r["id"] for r in receipt["rows"]] == [r["id"] for r in rows]
    for want, got, pin in zip(rows, receipt["rows"], PINNED):
        assert (got["verdict"], got["reason"]) == F.sign_oracle(want), want
        assert got["verdict"][0] == pin, want
        assert {k: got[k] for k in WR.SIGN_ECHO} == want
    dec = WR.verb_decision("pm7-35", rows, receipt, sha(data))      # what the screen stage reads
    assert dec["kept"] == [r["id"] for r, pin in zip(rows, PINNED) if pin == "k"]
    assert done.stdout.startswith("sign: pm7-35 (wave-zero-kept-v1): 24 rows, kept 12, dropped 12 -> ")


@pytest.mark.skipif(not EXE.is_file(), reason=f"{EXE} not built (target atx-equity-strategy-ic)")
def test_the_real_verb_refuses_before_any_output(tmp_path):
    rows = combo_rows()[:2]
    del rows[1]["runner_sign"]                                     # a string the screen did not score
    done, receipt, _ = run_exe(tmp_path, rows)
    assert done.returncode == 1 and receipt is None and not (tmp_path / "receipt.json").exists()
    assert "row 2 (c01) has no admission row" in done.stderr and "a stop, not a drop" in done.stderr
    done = subprocess.run([str(EXE), "sign", "--rule", "pm7-36", "--rows", str(tmp_path / "rows.json"), "--output",
                           str(tmp_path / "receipt.json")], capture_output=True, text=True, timeout=120)
    assert done.returncode == 2 and "unknown --rule pm7-36 (known: pm7-35)" in done.stderr
    assert not (tmp_path / "receipt.json").exists()


# ------------------------------------------------------------------ the screen stage on fakes
def wave(root: Path, fake, *args: str) -> tuple[int, list[str]]:
    lines: list[str] = []
    code = research_wave.main([args[0], F.MANIFEST, "--root", str(root), *args[1:]], executor=fake,
                              log=lines.append)
    return code, lines


def outputs(root: Path, name: str) -> dict:
    return json.loads((root / STATE / "receipts" / name).read_text(encoding="utf-8"))["outputs"]


def failure(root: Path, name: str) -> str:
    return json.loads((root / STATE / "receipts" / name).read_text(encoding="utf-8"))["error"]


def sign_calls(fake) -> list[list[str]]:
    return [c for c in fake.calls if len(c) > 1 and c[1] == "sign"]


def test_the_screen_stage_runs_the_sign_verb_once_and_records_its_receipt(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, ONE_DROPPED)
    code, plan = wave(root, fake, "plan")
    assert code == 0 and fake.calls == []
    assert any("sign verb: <exes.ic> sign --rule pm7-35 --rows out/waves/w1/sign/rows-<k>.json --output "
               "out/waves/w1/sign/receipt-<k>.json" in x for x in plan), plan
    code, out = wave(root, fake, "run")
    assert code == 0, out
    rows_rel, receipt_rel = f"{STATE}/sign/rows-1.json", f"{STATE}/sign/receipt-1.json"
    assert sign_calls(fake) == [[str(root.resolve() / "bin/ic.exe"), "sign", "--rule", "pm7-35", "--rows", rows_rel,
                                 "--output", receipt_rel]]
    sent = json.loads((root / rows_rel).read_text(encoding="utf-8"))
    assert sent == {"rule": "pm7-35", "rows": [{"id": c, "kind": "add", "prior_sign": 1, "status": s,
                                                "runner_sign": g} for c, (s, g) in ONE_DROPPED.items()]}
    sc = outputs(root, "03-screen.json")
    assert sc["sign"] == {"exe": "bin/ic.exe", "exe_sha256": None, "rows": rows_rel,
                          "rows_sha256": sha((root / rows_rel).read_bytes()), "receipt": receipt_rel,
                          "receipt_sha256": sha((root / receipt_rel).read_bytes())}
    assert sc["decision"]["kept"] == ["alpha_a", "alpha_c"] and sc["decision"]["dropped"] == ["alpha_b"]
    assert outputs(root, "04-spec.json")["kind"] == "b-library"
    res = json.loads((root / STATE / "wave-result.json").read_text(encoding="utf-8"))
    assert set(res["screen"]) == {"spec", "gate_exit", "rows", "kept", "dropped", "sign_rule"}   # the layout of before
    assert [set(r) for r in res["screen"]["rows"]] == [{"id", "kind", "prior_sign", "status", "runner_sign",
                                                         "decision", "reason"}] * 3
    assert res["screen"]["rows"][1]["reason"] == ("addition admitted with runner sign -1 against prior +1: dropped "
                                                  "from the wave")


def test_a_string_without_an_admission_row_stops_the_screen_before_the_verb(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", 1)})
    code, _ = wave(root, fake, "run")
    err = failure(root, "03-screen.failed-1.json")
    assert code == 4 and "no admission row for ['alpha_c']" in err and "not a drop" in err
    assert sign_calls(fake) == [] and not (root / STATE / "sign").exists()


def test_a_verb_refusal_stops_the_screen(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, ONE_DROPPED, fail={"sign": 1})
    code, _ = wave(root, fake, "run")
    assert code == 4 and "sign (pm7-35): exit 1" in failure(root, "03-screen.failed-1.json")
    assert not (root / STATE / "receipts" / "03-screen.json").exists()


class Tampered(F.FakeCycle):
    """The sign verb's receipt, edited after it is written."""

    def __init__(self, root: Path, admission: dict, edit, **kw):
        super().__init__(root, admission, **kw)
        self.edit = edit

    def sign(self, argv):
        done = super().sign(argv)
        out = self.root / argv[argv.index("--output") + 1]
        doc = json.loads(out.read_text(encoding="utf-8"))
        self.edit(doc)
        F.write_json(self.root, argv[argv.index("--output") + 1], doc)
        return done


def _verdict(doc: dict, k: int, v: str) -> None:
    doc["rows"][k]["verdict"] = v


TAMPER = {
    "schema": (lambda d: d.update(schema="atx.other/v1"), "not a atx.sign-receipt/v1 receipt"),
    "rule": (lambda d: d.update(rule="pm7-36"), "the receipt is for rule 'pm7-36'"),
    "rows sha": (lambda d: d.update(rows_sha256="0" * 64), "on rows sha256 000000000000"),
    "a row missing": (lambda d: d["rows"].pop(), "the receipt lists 2 rows for 3"),
    "rows reordered": (lambda d: d["rows"].reverse(), "is not the screen's"),
    "an echo changed": (lambda d: d["rows"][0].update(status="reject_veto"), "is not the screen's"),
    "an unknown verdict": (lambda d: _verdict(d, 0, "maybe"), "has verdict 'maybe', not keep | drop"),
    "kept inconsistent": (lambda d: d.update(kept=["alpha_a"]), "kept / dropped do not list its verdicts"),
    "a flipped verdict": (lambda d: _verdict(d, 1, "keep"), "kept / dropped do not list its verdicts"),
}


@pytest.mark.parametrize("case", sorted(TAMPER))
def test_a_receipt_that_is_not_the_verbs_answer_stops_the_screen(tmp_path, case):
    edit, needle = TAMPER[case]
    root = F.build(tmp_path / "r")
    code, _ = wave(root, Tampered(root, ONE_DROPPED, edit), "run")
    err = failure(root, "03-screen.failed-1.json")
    assert code == 3 and needle in err and f"{STATE}/sign/receipt-1.json" in err, err
    assert not (root / STATE / "receipts" / "03-screen.json").exists()


def test_a_retried_screen_writes_a_new_pair_and_leaves_the_first(tmp_path):
    root = F.build(tmp_path / "r")
    code, _ = wave(root, Tampered(root, ONE_DROPPED, lambda d: d.update(rule="x")), "run")
    assert code == 3
    first = {p.name: p.read_bytes() for p in (root / STATE / "sign").iterdir()}
    assert sorted(first) == ["receipt-1.json", "rows-1.json"]
    fake = F.FakeCycle(root, ONE_DROPPED)
    code, out = wave(root, fake, "run")
    assert code == 0, out
    assert {p.name: p.read_bytes() for p in (root / STATE / "sign").iterdir() if p.name in first} == first
    assert outputs(root, "03-screen.json")["sign"]["receipt"] == f"{STATE}/sign/receipt-2.json"
    assert sign_calls(fake)[0][-1] == f"{STATE}/sign/receipt-2.json"


def test_the_spec_stage_refuses_a_receipt_changed_after_the_screen(tmp_path, capsys):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, ONE_DROPPED)
    assert wave(root, fake, "run", "--until", "screen")[0] == 0
    p = root / STATE / "sign" / "receipt-1.json"
    p.write_bytes(p.read_bytes() + b" ")
    capsys.readouterr()
    code, _ = wave(root, fake, "run")
    assert code == stage_chain.EXIT_STALE
    assert "STALE: screen.sign recorded receipt_sha256" in capsys.readouterr().err
    assert not (root / STATE / "receipts" / "04-spec.json").exists()


def test_the_ic_exe_must_be_its_pin(tmp_path):
    root = F.build(tmp_path / "r")
    w = Wave(F.MANIFEST, root, executor=F.FakeCycle(root, ONE_DROPPED), log=lambda s: None)
    F.write(root, "bin/ic.exe", "synthetic IC exe\n")
    spec = json.loads((root / F.PARENT).read_text(encoding="utf-8"))
    for pin, ok in ((None, True), (sha((root / "bin/ic.exe").read_bytes()), True), ("0" * 64, False)):
        doc = dict(spec, **({RC.EXES_PIN: {"ic": pin}} if pin else {}))
        F.write_json(root, "scripts/specs/v8/lib-x.json", doc)
        if ok:
            assert SL.sign_exe(w, "scripts/specs/v8/lib-x.json") == "bin/ic.exe"
        else:
            with pytest.raises(stage_chain.StageError, match="not its pin 000000000000") as exc:
                SL.sign_exe(w, "scripts/specs/v8/lib-x.json")
            assert exc.value.code == SL.EXIT_PIN
    pin = sha((root / "bin/ic.exe").read_bytes())                  # a pinned exe that is gone: refused, as
    (root / "bin/ic.exe").unlink()                                 # research_cycle's verify_exes refuses it
    F.write_json(root, "scripts/specs/v8/lib-x.json", dict(spec, **{RC.EXES_PIN: {"ic": pin}}))
    with pytest.raises(stage_chain.StageError, match=f"bin/ic.exe is missing, not its pin {pin[:12]}") as exc:
        SL.sign_exe(w, "scripts/specs/v8/lib-x.json")
    assert exc.value.code == SL.EXIT_PIN
    F.write_json(root, "scripts/specs/v8/lib-x.json", dict(spec, exes={"nav": "bin/nav.exe"}))
    with pytest.raises(stage_chain.StageError, match="spec exes needs ic"):    # a screen spec always runs exes.ic
        SL.sign_exe(w, "scripts/specs/v8/lib-x.json")


# ------------------------------------------------------------------ route absent: ledgered manifests, older screens
def test_ledgered_v1_manifests_still_validate_and_name_pm7_35():
    """Their results are ledgered and never edited (a git diff of this lane over scripts/specs/v8 and the v8 sprint's
    waves/ is empty); y-s.head.json is the head copy of Y-S without its candidates (test_wave_gross_match's rule)."""
    names = []
    for path in sorted((REPO / "scripts/specs/v8/waves").glob("y-*.json")):
        m = json.loads(path.read_text(encoding="utf-8"))
        assert m["schema"] == WM.SCHEMA, path.name
        assert WM.validate(m) == ([] if path.name != "y-s.head.json" else
                                  ["a wave has exactly one of candidates (a library wave) and rule_cell (a rule wave)"])
        if "candidates" in m:
            names.append(m["sign_rule"])
    assert names and set(names) == {"pm7-35"} and "pm7-35" in WR.SIGN_RULES


def test_a_screen_recorded_before_sign_keeps_the_spec_stage_inputs(tmp_path):
    root = F.build(tmp_path / "r")
    w = Wave(F.MANIFEST, root, executor=F.FakeCycle(root, ONE_DROPPED), log=lambda s: None)
    F.write_json(root, "out/fit/admission.json", {"candidates": []})
    done = {"screen": {"admission": "out/fit/admission.json",
                       "admission_sha256": sha((root / "out/fit/admission.json").read_bytes())}}
    assert SL.spec_inputs(w, done) == {"screen.admission_sha256": done["screen"]["admission_sha256"]}
