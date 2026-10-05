"""OD-3 history-read tooling (v8-od3-protocol.md section 2, gap T1; P9 lane OD3P). Synthetic data only.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_od3_tooling.py

T1  carry_weights.py: TRAIN admission and weights carried to history eras (PM7-29 choice 1). The tests of T1 are the
    xpre branch's (`f68058e2`), unchanged.
OD3P --drop-list (Rulings PRE-20, PRE-31): the flagged members weigh a pinned 0 on the history eras, the kept members'
    weights are TRAIN's byte for byte, the anchor file is TRAIN's; an id that is not a member is refused, and so are a
    base that applies a listed rule and a composition whose weights the IC runner re-derives.
Identity: without --drop-list, every published byte equals the branch tool's (blob 186655d9, sha256 5aebb3a7...).
The branch's T2 / T5 tests (nav_summ) are re-done on the engine verb after lane B2's nav_summ is in root (T2 pending in
task-OD3P-report.md); T3 is dropped (Ruling PRE-21).
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess

import pytest

import carry_weights as CW

REPO = Path(__file__).resolve().parents[2]
CARRY_BLOB = "186655d9baada80ad10d33019fb1128325146cd0"     # carry_weights.py at f68058e2 (sha256 5aebb3a7...)
FLAGS_FILE = REPO / ".superpowers" / "sdd" / "platform-p9-20261003" / "p9-prereg-flags.md"
LIST_FILE = REPO / "scripts" / "specs" / "p9" / "od3" / "flagged_members.json"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ------------------------------------------------------------------ T1: carry_weights.py (the branch's tests)
def weights_doc(train_sha: str) -> dict:
    return {"schema": "atx.dsl-composition-weights/v2", "library_sha256": "11" * 32, "train_manifest_sha256": train_sha,
            "weights": {"a": 0.25, "b": 0.75}, "signs": {"a": 1, "b": -1},
            "theme_standardise": {"rule": "ew-theme-std-v1", "rerank": True, "themes": {"a": "value", "b": "momentum"}},
            "provenance": {"rule": "ew-theme-std-v1", "orientations_sha256": "22" * 32,
                           "weighted_standalone_turnover": 0.051}}


@pytest.fixture()
def carry_inputs(tmp_path):
    roles = {}
    for name in ("E1", "E2", "E3"):
        p = tmp_path / f"role-{name}.json"
        p.write_text(json.dumps({"role": name}), encoding="utf-8")
        roles[name] = (str(p), sha(p.read_bytes()))
    w = tmp_path / "composition_weights.json"
    w.write_bytes(CW.canonical_bytes(weights_doc(roles["E3"][1])))
    a = tmp_path / "admission.json"
    a.write_text(json.dumps({"candidates": [{"id": "a", "status": "admitted"}]}), encoding="utf-8")
    return {"tmp": tmp_path, "roles": roles, "w": w, "a": a}


def carry_argv(c: dict, out: Path, eras=("E1", "E2"), anchor="E3") -> list[str]:
    argv = ["--weights", str(c["w"]), "--weights-sha256", sha(c["w"].read_bytes()), "--admission", str(c["a"]),
            "--admission-sha256", sha(c["a"].read_bytes())]
    for e in eras:
        argv += ["--era", e, *c["roles"][e]]
    return argv + ["--anchor", *c["roles"][anchor], "--output", str(out)]


def test_carry_publishes_train_bytes_at_the_anchor_and_rebinds_each_era(carry_inputs, capsys):
    c = carry_inputs
    out = c["tmp"] / "W"
    assert CW.main(carry_argv(c, out)) == 0
    capsys.readouterr()
    assert sorted(p.name for p in out.iterdir()) == ["admission.json", "carry.json", "composition_weights.E1.json",
                                                     "composition_weights.E2.json", "composition_weights.json"]
    assert (out / "composition_weights.json").read_bytes() == c["w"].read_bytes()        # the anchor is TRAIN's file
    assert (out / "admission.json").read_bytes() == c["a"].read_bytes()
    train = json.loads(c["w"].read_text(encoding="utf-8"))
    for era in ("E1", "E2"):
        doc = json.loads((out / f"composition_weights.{era}.json").read_text(encoding="utf-8"))
        block = doc.pop("carried_from")
        assert doc.pop("train_manifest_sha256") == c["roles"][era][1]
        assert doc == {k: v for k, v in train.items() if k != "train_manifest_sha256"}   # weights, signs, themes
        assert block == {"rule": CW.RULE, "weights_sha256": sha(c["w"].read_bytes()),
                         "admission_sha256": sha(c["a"].read_bytes()), "train_manifest_sha256": c["roles"]["E3"][1]}
        assert (out / f"composition_weights.{era}.json").read_bytes() == CW.canonical_bytes(
            dict(train, train_manifest_sha256=c["roles"][era][1], carried_from=block))
    rec = json.loads((out / "carry.json").read_text(encoding="utf-8"))
    assert [(e["id"], e["role_sha256"]) for e in rec["eras"]] == [("E1", c["roles"]["E1"][1]),
                                                                  ("E2", c["roles"]["E2"][1])]


@pytest.mark.parametrize("case", ["weights_pin", "admission_pin", "role_pin", "anchor_not_train", "dup_id", "bad_id",
                                  "era_is_anchor", "exists", "carried_twice", "no_binding"])
def test_carry_refusals_publish_nothing(carry_inputs, case, capsys):
    c = carry_inputs
    out = c["tmp"] / "W"
    argv = carry_argv(c, out)
    if case == "weights_pin":
        argv[argv.index("--weights-sha256") + 1] = "0" * 64
    elif case == "admission_pin":
        argv[argv.index("--admission-sha256") + 1] = "0" * 64
    elif case == "role_pin":
        argv[argv.index("--era") + 3] = "0" * 64
    elif case == "anchor_not_train":
        argv = carry_argv(c, out, eras=("E1", "E3"), anchor="E2")
    elif case == "dup_id":
        argv = carry_argv(c, out, eras=("E1", "E1"))
    elif case == "bad_id":
        argv[argv.index("--era") + 1] = "E-1"
    elif case == "era_is_anchor":
        argv = carry_argv(c, out, eras=("E1", "E3"))
    elif case == "exists":
        out.mkdir()
    elif case == "carried_twice":
        c["w"].write_bytes(CW.canonical_bytes(dict(weights_doc(c["roles"]["E3"][1]), carried_from={"rule": "x"})))
        argv = carry_argv(c, out)
    elif case == "no_binding":
        doc = weights_doc(c["roles"]["E3"][1])
        doc.pop("train_manifest_sha256")
        c["w"].write_bytes(CW.canonical_bytes(doc))
        argv = carry_argv(c, out)
    assert CW.main(argv) == 1
    assert "carry_weights: refused:" in capsys.readouterr().err
    assert (not out.exists() or not any(out.iterdir())) and not (c["tmp"] / "W.pending").exists()


# ------------------------------------------------------------------ OD3P: the drop list (PRE-20, PRE-31)
BOOK = {"a": 0.125, "b": 0.0625, "c": 0.1875, "d": 0.3125, "e": 0.3125}      # a synthetic book of five members
THEMES = {"a": "value", "b": "value", "c": "momentum", "d": "momentum", "e": "quality"}


def book_doc(train_sha: str, **over) -> dict:
    doc = {"schema": "atx.dsl-composition-weights/v2", "library_sha256": "11" * 32, "train_manifest_sha256": train_sha,
           "weights": dict(BOOK), "signs": {k: (1 if k != "c" else -1) for k in BOOK},
           "theme_standardise": {"rule": "ew-theme-std-v1", "rerank": True, "themes": dict(THEMES)},
           "provenance": {"rule": "ew-theme-std-v1", "orientations_sha256": "22" * 32,
                          "weighted_standalone_turnover": 0.051}}
    doc.update(over)
    return doc


def list_doc(members, rules=("theme-cov-hedge-v1", "mom-volman-v1"), pending=()) -> dict:
    return {"schema": CW.DROP_SCHEMA, "source": {"path": "flags.md", "sha256": "33" * 32}, "members": list(members),
            "rules": list(rules), "rules_pending": list(pending)}


@pytest.fixture()
def book(carry_inputs):
    c = carry_inputs
    c["w"].write_bytes(CW.canonical_bytes(book_doc(c["roles"]["E3"][1])))
    c["l"] = c["tmp"] / "flagged_members.json"
    c["l"].write_text(json.dumps(list_doc(["b", "d"]), indent=2), encoding="utf-8")
    return c


def drop_argv(c: dict, out: Path, **kw) -> list[str]:
    return carry_argv(c, out, **kw) + ["--drop-list", str(c["l"]), "--drop-list-sha256", sha(c["l"].read_bytes())]


def test_drop_pins_the_listed_members_to_zero_on_the_history_eras_only(book, capsys):
    c = book
    plain, dropped = c["tmp"] / "PLAIN", c["tmp"] / "DROP"
    assert CW.main(carry_argv(c, plain)) == 0 and CW.main(drop_argv(c, dropped)) == 0
    capsys.readouterr()
    w_bytes = c["w"].read_bytes()
    train = json.loads(w_bytes)
    assert (dropped / "composition_weights.json").read_bytes() == w_bytes          # the anchor: TRAIN's bytes
    assert (dropped / "admission.json").read_bytes() == c["a"].read_bytes()
    block = {"rule": CW.DROP_RULE, "list_sha256": sha(c["l"].read_bytes()), "members": ["b", "d"],
             "source_sha256": "33" * 32}
    train_lines = w_bytes.decode().splitlines()
    for era in ("E1", "E2"):
        name = f"composition_weights.{era}.json"
        era_doc, plain_doc = (json.loads((d / name).read_bytes()) for d in (dropped, plain))
        assert era_doc["weights"]["b"] == era_doc["weights"]["d"] == 0.0           # dropped: a pinned zero
        kept = {k: v for k, v in era_doc["weights"].items() if k not in ("b", "d")}
        assert kept == {k: v for k, v in train["weights"].items() if k not in ("b", "d")}
        era_lines = (dropped / name).read_bytes().decode().splitlines()
        for k in kept:                                                               # kept: TRAIN's bytes
            line = next(t for t in train_lines if t.startswith(f'    "{k}": '))
            assert line in era_lines
        assert era_doc["carried_from"]["drop"] == block
        # every other byte is the plain carry's: signs, themes, provenance, the binding, the carried_from block
        assert era_doc == dict(plain_doc, weights=dict(plain_doc["weights"], b=0.0, d=0.0),
                               carried_from=dict(plain_doc["carried_from"], drop=block))
        assert (dropped / name).read_bytes() == CW.canonical_bytes(era_doc)
    rec, rec_plain = (json.loads((d / "carry.json").read_bytes()) for d in (dropped, plain))
    assert rec.pop("drop") == dict(block, rules=["mom-volman-v1", "theme-cov-hedge-v1"])
    assert [(e["id"], e["role_sha256"], e["file"]) for e in rec.pop("eras")] == [
        (e["id"], e["role_sha256"], e["file"]) for e in rec_plain.pop("eras")]
    assert rec == rec_plain


@pytest.mark.parametrize("case, needle", [
    ("unknown_id", "are not members of the book"),
    ("repeated_id", "repeats an id"),
    ("bad_id", "must match"),
    ("empty", "members is empty"),
    ("pending", "rules_pending"),
    ("schema", "drop list schema"),
    ("source", "source must be"),
    ("pin", "drop list .* is .*, pinned"),
    ("flagged_rule_block", "applies flagged rule"),
    ("flagged_rule_provenance", "applies flagged rule"),
    ("theme_erc", "re-derives and verifies"),
    ("v3", "weights object keyed by member id"),
    ("no_era", "one or more --era"),
])
def test_drop_refusals_publish_nothing(book, case, needle, capsys):
    c = book
    out = c["tmp"] / "W"
    lst = list_doc(["b", "d"])
    doc = book_doc(c["roles"]["E3"][1])
    if case == "unknown_id":
        lst["members"].append("zz")
    elif case == "repeated_id":
        lst["members"].append("b")
    elif case == "bad_id":
        lst["members"].append("b-2")
    elif case == "empty":
        lst["members"] = []
    elif case == "pending":
        lst["rules_pending"] = ["info-clock-rate-v1"]
    elif case == "schema":
        lst["schema"] = "atx.od3-flagged-members/v0"
    elif case == "source":
        lst["source"] = {"path": "flags.md"}
    elif case == "flagged_rule_block":
        doc["theme_schedule"] = {"rule": "mom-volman-v1", "blocks": []}
    elif case == "flagged_rule_provenance":
        doc["provenance"]["rule"] = "theme-cov-hedge-v1"
    elif case == "theme_erc":
        doc["theme_standardise"]["rule"] = "theme-erc-v1"
    elif case == "v3":
        doc["schema"] = "atx.dsl-composition-weights/v3"
    c["l"].write_text(json.dumps(lst), encoding="utf-8")
    c["w"].write_bytes(CW.canonical_bytes(doc))
    argv = drop_argv(c, out, eras=() if case == "no_era" else ("E1", "E2"))
    if case == "pin":
        argv[-1] = "0" * 64
    assert CW.main(argv) == 1
    err = capsys.readouterr().err
    assert "carry_weights: refused:" in err and re.search(needle, err), err
    assert not out.exists() and not (c["tmp"] / "W.pending").exists()


def test_drop_list_and_its_pin_go_together(book, capsys):
    c = book
    for extra in (["--drop-list", str(c["l"])], ["--drop-list-sha256", sha(c["l"].read_bytes())]):
        with pytest.raises(SystemExit) as e:
            CW.main(carry_argv(c, c["tmp"] / "W") + extra)
        assert e.value.code == 2
    capsys.readouterr()


# ------------------------------------------------------------------ the committed list (PRE-31)
def final_flag_list(text: str) -> tuple[list[str], list[str]]:
    section = text.split("## FINAL FLAG LIST", 1)[1]
    pick = lambda label: [x.strip() for x in re.search(rf"\*\*{label} \(\d+\):\*\* (.*)", section).group(1).split(",")]  # noqa: E731
    return pick("Flagged"), pick("Flagged-unverified")


def test_the_committed_list_is_the_final_flag_list_of_its_source(tmp_path, capsys):
    doc = json.loads(LIST_FILE.read_text(encoding="utf-8"))
    if not FLAGS_FILE.is_file():
        pytest.skip(f"{FLAGS_FILE} is not in this checkout")
    raw = FLAGS_FILE.read_bytes().replace(b"\r\n", b"\n")                 # the blob's LF bytes (autocrlf checkouts)
    assert sha(raw) == doc["source"]["sha256"] == "7a089e5233cb69a329bd09f42f2dfff48f638101a421751f03b2291aa2d94bbe"
    flagged, unverified = final_flag_list(raw.decode("utf-8"))
    assert (len(flagged), len(unverified)) == (10, 2)
    assert doc["members"] == flagged + unverified                          # the 12 ids, in the file's order
    assert doc["classes"] == {"flagged": flagged, "flagged-unverified": unverified}
    assert doc["rules"] == ["theme-cov-hedge-v1", "mom-volman-v1"]
    # as committed, the tool refuses it until root resolves the pending rules (PRE-31); resolved, it carries the book
    roles = {}
    for name in ("E1", "E3"):
        p = tmp_path / f"role-{name}.json"
        p.write_text(name, encoding="utf-8")
        roles[name] = (str(p), sha(p.read_bytes()))
    ids = doc["members"] + ["keep_a", "keep_b"]
    w = tmp_path / "w.json"
    w.write_bytes(CW.canonical_bytes(book_doc(roles["E3"][1], weights={k: 1 / len(ids) for k in ids},
                                              signs={k: 1 for k in ids}, theme_standardise={
                                                  "rule": "ew-theme-std-v1", "rerank": True,
                                                  "themes": {k: "t" for k in ids}})))
    a = tmp_path / "a.json"
    a.write_text("{}", encoding="utf-8")
    resolved = tmp_path / "resolved.json"
    resolved.write_text(json.dumps(dict(doc, rules_pending=[])), encoding="utf-8")
    for lst, code in ((LIST_FILE, 1), (resolved, 0)):
        argv = ["--weights", str(w), "--weights-sha256", sha(w.read_bytes()), "--admission", str(a),
                "--admission-sha256", sha(a.read_bytes()), "--era", "E1", *roles["E1"], "--anchor", *roles["E3"],
                "--output", str(tmp_path / f"out{code}"), "--drop-list", str(lst), "--drop-list-sha256",
                sha(lst.read_bytes())]
        assert CW.main(argv) == code
    assert "rules_pending" in capsys.readouterr().err
    era = json.loads((tmp_path / "out0" / "composition_weights.E1.json").read_bytes())
    assert sorted(k for k, v in era["weights"].items() if v == 0.0) == sorted(doc["members"])
    assert era["weights"]["keep_a"] == era["weights"]["keep_b"] == 1 / len(ids)


# ------------------------------------------------------------------ identity: no --drop-list
def load_branch_tool(tmp: Path):
    try:
        text = subprocess.run(["git", "cat-file", "-p", CARRY_BLOB], capture_output=True, check=True,
                              cwd=Path(CW.__file__).resolve().parent).stdout
    except (OSError, subprocess.CalledProcessError):
        pytest.skip(f"git history with the carry_weights blob {CARRY_BLOB[:8]} is unavailable")
    assert sha(text) == "5aebb3a7ce8ff3f845d3151bf44d5b1648a14705be4421b3fa0768a3db33227c"
    path = tmp / "old" / "carry_weights_f68058e2.py"
    path.parent.mkdir(exist_ok=True)
    path.write_bytes(text)
    spec = importlib.util.spec_from_file_location("carry_weights_f68058e2", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("which", ["two_member_book", "five_member_book"])
def test_without_a_drop_list_every_byte_is_the_branch_tools(book, which, capsys):
    c = book
    if which == "two_member_book":
        c["w"].write_bytes(CW.canonical_bytes(weights_doc(c["roles"]["E3"][1])))
    old = load_branch_tool(c["tmp"])
    outs = {}
    for tag, module in (("new", CW), ("old", old)):
        out = c["tmp"] / f"W-{tag}"
        assert module.main(carry_argv(c, out)) == 0
        printed = json.loads(capsys.readouterr().out)
        outs[tag] = ({p.name: p.read_bytes() for p in sorted(out.iterdir())}, printed["files"], printed["rule"])
    assert outs["new"] == outs["old"]
    for tag, module in (("new", CW), ("old", old)):                   # and the same refusal text
        assert module.main(carry_argv(c, c["tmp"] / f"W-{tag}")) == 1
        outs[tag] = capsys.readouterr().err.replace(f"W-{tag}", "W")
    assert outs["new"] == outs["old"]
