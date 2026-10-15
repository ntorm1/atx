"""OD-3 history-read tooling (v8-od3-protocol.md section 2, gap T1; P9 lane OD3P). Synthetic data only.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_od3_tooling.py

T1  carry_weights.py: TRAIN admission and weights carried to history eras (PM7-29 choice 1). The tests of T1 are the
    xpre branch's (`f68058e2`), unchanged.
OD3P --drop-list (Rulings PRE-20, PRE-31, OD3P-1): the list is checked and recorded in carry.json and changes no
    weight: every weights file is the bytes of the carry without the list, for ew-theme-std, theme-erc and ic-shrink
    bases alike (the IC exe's --absent-members applies the list on history, research_roles.py); an id that is not a
    member, a repeated or malformed id, pending rules, a base that applies a listed rule and no --era are refused.
    ALCLOCK-2: the committed list carries info-clock-rate-v1 in rules (2019) and mv-aim-v1 alone pending.
Identity: without --drop-list, every published byte equals the branch tool's (blob 186655d9, sha256 5aebb3a7...).
T2  nav_summ --pool-one-sided (OD3P item 6, Ruling PRE-21; follow-up round OD3P-S): the pooled row's paired block from
    the engine verb's --one-sided (lw.p_one_sided, the same resamples) plus the bundle's verdict rule, labelled "bundle
    rule, not G-B8" (Ruling OD3P-T2V); refused without --pool and a reference.
T5  a pool whose last era (E3) is an already-ledgered TRAIN cell: the ledger skips E3's era line and the read adds 0;
    the lines are the same with --pool-one-sided.
Identity: without --pool-one-sided, nav_summ equals root's blob 9fd6bd0a (fcb0200e; sha256 50fa3503...): stdout,
    stderr, the JSON documents minus nav_summ_run, the ledger lines, and (fake verb) the verb-call sequence.
The T2 / T5 / identity tests are the xpre branch's (`f68058e2`) re-done on the engine verb (B2's nav_summ: no Python
statistic), each on two verbs (fixture ``verb``): ``fake`` = EngineStats.run answered by B2's frozen statistics
(atx-engine/tests/fixtures/eval_tie/frozen_statistics.py, the retired Python verbatim); ``real`` = the built
atx-research-eval at nav_summ.default_engine_exe() ($ATX_EQUITY_BIN when set, else build-equity/bin of this checkout:
the path root's nav_summ suites use), a NAMED skip when it is not built (Ruling OD3P-SKIP: root's run executes it).
T3 is dropped (Ruling PRE-21).
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys

import numpy as np
import pytest

import backtest_integrity as BI
import carry_weights as CW
import nav_summ as NS
from test_nav_summ import SCEN, write_nav
from test_nav_summ_pool import history_rows, write_dir

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import research_ledger as RL  # noqa: E402

CARRY_BLOB = "186655d9baada80ad10d33019fb1128325146cd0"     # carry_weights.py at f68058e2 (sha256 5aebb3a7...)
FLAGS_FILE = REPO / ".superpowers" / "sdd" / "platform-p9-20261003" / "p9-prereg-flags.md"
LIST_FILE = REPO / "scripts" / "specs" / "p9" / "od3" / "flagged_members.json"
NAV_SUMM_BLOB = "9fd6bd0a6032b4512c93c50fcc25dbc123c8b518"   # root's nav_summ.py at fcb0200e (B2 + T2 in root)
NAV_SUMM_SHA256 = "50fa350357e656670b5454a7a2f11aba8aff7740a2ec9be37b90c22b9bbca434"
FROZEN = REPO / "atx-engine" / "tests" / "fixtures" / "eval_tie" / "frozen_statistics.py"
DRAWS = ["--draws", "200"]
T2_LABEL = "bundle rule, not G-B8"                             # Ruling OD3P-T2V


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


STANDARDISE = {           # theme_standardise rows: ew-theme-std re-derives nothing; theme-erc / ic-shrink verify W
    "ew-theme-std-v1": {"rule": "ew-theme-std-v1", "rerank": True, "themes": dict(THEMES)},
    "theme-erc-v1": {"rule": "theme-erc-v1", "rerank": True, "themes": dict(THEMES),
                     "theme_shares": {"value": 0.25, "momentum": 0.5, "quality": 0.25}},
    "ic-shrink-v1": {"rule": "ic-shrink-v1", "rerank": True, "themes": dict(THEMES), "lambda": 0.5},
}


@pytest.mark.parametrize("base", sorted(STANDARDISE))
def test_drop_carries_train_bytes_and_only_records_the_list(book, base, capsys):
    """OD3P-1: the carry zeroes nothing; every weights file is the plain carry's bytes (so W's weights, signs and
    theme blocks, and the runner's verify of a theme-erc / ic-shrink base, are untouched); carry.json gains the record."""
    c = book
    c["w"].write_bytes(CW.canonical_bytes(book_doc(c["roles"]["E3"][1], theme_standardise=STANDARDISE[base])))
    plain, dropped = c["tmp"] / "PLAIN", c["tmp"] / "DROP"
    assert CW.main(carry_argv(c, plain)) == 0 and CW.main(drop_argv(c, dropped)) == 0
    capsys.readouterr()
    files = sorted(p.name for p in dropped.iterdir())
    assert files == sorted(p.name for p in plain.iterdir())
    for name in files:
        if name != "carry.json":                                                    # every weights byte: no drop
            assert (dropped / name).read_bytes() == (plain / name).read_bytes(), name
    w_bytes = c["w"].read_bytes()
    assert (dropped / "composition_weights.json").read_bytes() == w_bytes          # the anchor: TRAIN's bytes
    for era in ("E1", "E2"):
        doc = json.loads((dropped / f"composition_weights.{era}.json").read_bytes())
        assert doc["weights"] == json.loads(w_bytes)["weights"] and "drop" not in doc["carried_from"]
    rec, rec_plain = (json.loads((d / "carry.json").read_bytes()) for d in (dropped, plain))
    assert rec.pop("drop") == {"rule": CW.DROP_RULE, "list_sha256": sha(c["l"].read_bytes()), "members": ["b", "d"],
                               "source_sha256": "33" * 32, "rules": ["mom-volman-v1", "theme-cov-hedge-v1"]}
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
    # ALCLOCK-2: info-clock-rate-v1 registers source_sample_end 2019 and is flagged; mv-aim-v1 stays pending (root
    # resolves it from COV-MV's registration before the read)
    assert doc["rules"] == ["theme-cov-hedge-v1", "mom-volman-v1", "info-clock-rate-v1"]
    assert doc["rule_source_sample_end"] == {"theme-cov-hedge-v1": 2017, "mom-volman-v1": 2016,
                                             "info-clock-rate-v1": 2019}
    assert doc["rules_pending"] == ["mv-aim-v1"]
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
    assert era["weights"] == {k: 1 / len(ids) for k in ids}                 # OD3P-1: no weight is changed
    rec = json.loads((tmp_path / "out0" / "carry.json").read_bytes())
    assert rec["drop"]["members"] == sorted(doc["members"]) and rec["drop"]["list_sha256"] == sha(resolved.read_bytes())


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


# ------------------------------------------------------------------ the engine verb: fake / real (OD3P-SKIP)
def load_frozen():
    """B2's frozen statistics (the retired Python verbatim), by path under a private name (no import of a deleted
    module; as test_nav_summ_engine.py loads it)."""
    spec = importlib.util.spec_from_file_location("b2_frozen_statistics_od3p_test", FROZEN)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FS = load_frozen()


def digest(nets: dict | None) -> str | None:
    """A net series (session_ns -> net) as a short digest: the verb-call record names series, not temp paths."""
    if nets is None:
        return None
    return sha(json.dumps([[int(s), float(v)] for s, v in nets.items()]).encode("ascii"))[:16]


def values(nets: dict) -> np.ndarray:
    return np.array(list(nets.values()), dtype=np.float64)


class FakeVerb:
    """The engine verb atx-research-eval answered by B2's frozen statistics, in the verb's JSON shape, for the options
    nav_summ sends here: none (net moments of one cell), --paired CSV --draws N --block B --seed S [--one-sided] (the
    paired block of one cell; the CSV maps back to its series through the EngineStats' own ``_csv``) and --dsr-n N
    (the DSR rows). Any other option or shape fails the test. Each call is recorded as (cells, reference, options)
    with series digests. ``force`` = (dsr, p_one_sided) replaces those two in a --one-sided block (verdict cases)."""

    VALUED = ("--paired", "--draws", "--block", "--seed", "--dsr-n")

    def __init__(self):
        self.calls: list[tuple] = []
        self.force: tuple | None = None

    def install(self, monkeypatch, module) -> None:
        fake = self

        def run(engine, cells, *extra):
            return fake.answer(engine, cells, [str(a) for a in extra])

        monkeypatch.setattr(module.EngineStats, "run", run)

    def answer(self, engine, cells: list[dict], extra: list[str]) -> dict:
        opts, flags, k = {}, [], 0
        while k < len(extra):
            key = extra[k]
            if key == "--one-sided" and not flags:
                flags.append(key)
                k += 1
                continue
            if key not in self.VALUED or key in opts or k + 1 >= len(extra):
                raise AssertionError(f"fake verb: unsupported options {extra}")
            opts[key] = extra[k + 1]
            k += 2
        by_path = {path: nets for nets, path in engine._csv.values()}
        ref = by_path[opts["--paired"]] if "--paired" in opts else None
        self.calls.append(([digest(c) for c in cells], digest(ref),
                           sorted((key, v) for key, v in opts.items() if key != "--paired"), flags))
        if "--paired" in opts:
            assert set(opts) == {"--paired", "--draws", "--block", "--seed"} and len(cells) == 1, extra
            a, b = NS.align(cells[0], ref)
            paired = None
            if a.size >= 3:
                p = FS.paired_stats(a, b, int(opts["--draws"]), int(opts["--block"]), int(opts["--seed"]),
                                    one_sided=bool(flags))
                if flags and self.force is not None:
                    p["dsr"], p["lw"]["p_one_sided"] = self.force
                lw = p["lw"]
                paired = dict(p, one_minus_rho=1.0 - p["rho"], degenerate=False, se=lw["se"],
                              p_one=lw.get("p_one_sided"), p_two=lw["p_value"], reference=opts["--paired"])
            return json.loads(json.dumps({"cells": [{"paired": paired}]}))
        assert not flags, extra
        if "--dsr-n" in opts:
            rows = FS.dsr_rows([FS.net_moments(values(c)) for c in cells], int(opts["--dsr-n"]))
            return json.loads(json.dumps({"cells": [{"dsr": {"cross": row}} for row in rows]}))
        assert not opts and len(cells) == 1, extra
        return json.loads(json.dumps({"cells": [{"net_moments": FS.net_moments(values(cells[0]))}]}))


class Verb:
    """The verb a test runs nav_summ on: ``argv`` names it (--engine-stats EXE, also for a module loaded from a blob,
    whose default path resolves from its temp location); ``adopt`` patches another module's EngineStats the same way."""

    def __init__(self, exe: Path, fake: FakeVerb | None, monkeypatch):
        self.exe, self.fake, self.monkeypatch = exe, fake, monkeypatch
        self.argv = ["--engine-stats", str(exe)]

    def adopt(self, module) -> None:
        if self.fake is not None:
            self.fake.install(self.monkeypatch, module)

    def mark(self) -> int:
        return len(self.fake.calls) if self.fake is not None else 0

    def calls_since(self, k: int) -> list | None:
        return self.fake.calls[k:] if self.fake is not None else None


def fake_verb_of(tmp: Path, monkeypatch) -> Verb:
    exe = tmp / "fake-bin" / "atx-research-eval.exe"           # EngineStats needs an existing file to name and hash
    exe.parent.mkdir()
    exe.write_bytes(b"fake verb: nav_summ.EngineStats.run is patched by test_od3_tooling.FakeVerb\n")
    fake = FakeVerb()
    fake.install(monkeypatch, NS)
    return Verb(exe, fake, monkeypatch)


@pytest.fixture(params=["fake", "real"])
def verb(request, tmp_path, monkeypatch) -> Verb:
    if request.param == "real":
        exe = NS.default_engine_exe()                           # the path root's nav_summ suites use
        if not exe.is_file():
            pytest.skip(f"the engine verb atx-research-eval is not built at {exe} ($ATX_EQUITY_BIN, else "
                        "build-equity/bin): Ruling OD3P-SKIP, root's run with the verb built executes this case")
        return Verb(exe, None, monkeypatch)
    return fake_verb_of(tmp_path, monkeypatch)


@pytest.fixture()
def fake_verb(tmp_path, monkeypatch) -> Verb:
    return fake_verb_of(tmp_path, monkeypatch)


# ------------------------------------------------------------------ T2 / T5: the history pool (xpre f68058e2)
def era_dirs(tmp: Path, tag: str, seed: int) -> list[Path]:
    """E1 (2015) and E2 (2017) history eras and E3, a TRAIN era (2020), of one book (synthetic nets)."""
    out = []
    for k, (eid, first) in enumerate((("E1", "2015-01-02"), ("E2", "2017-01-03"), ("E3", "2020-01-02"))):
        rows = history_rows(first, 200, seed + k)
        out.append(write_dir(tmp / f"{tag}-{eid}", rows, role_sha=f"{k + 1}{k + 1}" * 32))
    return out


@pytest.fixture()
def read(tmp_path):
    """Two books (R2, B0c) over E1..E3, and a chained ledger holding three TRAIN construction cells on the research
    window: R2's E3 and B0c's E3 (the TRAIN cells the read re-reads) and one more (V needs two or more cells). The
    Sharpe values written into the lines are fixture constants."""
    wid = BI.window_id()
    r2, b0c = era_dirs(tmp_path, "R2", 10), era_dirs(tmp_path, "B0c", 20)
    other = write_nav(tmp_path / "OTHER", list(0.0004 + 0.01 * np.random.default_rng(3).normal(size=250)))
    ledger = tmp_path / "trials.jsonl"
    lines = []
    for d, sr in ((b0c[2], 1.13), (other, 0.9), (r2[2], 1.26)):
        lines.append(BI.ledger_record("construction", str(d), d / "summary.json", d / f"daily_{SCEN}.csv", SCEN,
                                      NS.net_series(NS.load_daily(d, SCEN)), sr, research_window_id=wid,
                                      origin="prior"))
    BI.ledger_append(ledger, lines, chain=True)
    return {"tmp": tmp_path, "r2": r2, "b0c": b0c, "ledger": ledger}


def pool_argv(read: dict, *extra: str) -> list[str]:
    return ["--pool", *map(str, read["r2"]), "--pool-ids", "E1,E2,E3", "--pool-reference", *map(str, read["b0c"]),
            "--protocol", "v8", *DRAWS, *extra]


def run_json(tmp: Path, argv: list[str], capsys, name: str = "out") -> tuple[list[dict], str]:
    out = tmp / f"{name}.json"
    assert NS.main(argv + ["--json", str(out)]) == 0
    return json.loads(out.read_text(encoding="utf-8")), capsys.readouterr().out


def pooled_pair(read: dict) -> tuple[np.ndarray, np.ndarray]:
    """The pooled pair R2 vs B0c over E1..E3, aligned era by era (the eras are in date order)."""
    a, b = [], []
    for d, ref in zip(read["r2"], read["b0c"]):
        x, y = NS.align(NS.net_series(NS.load_daily(d, SCEN)), NS.net_series(NS.load_daily(ref, SCEN)))
        a += list(x)
        b += list(y)
    return np.array(a), np.array(b)


def test_t2_pool_one_sided_adds_the_p_and_the_verdict_only(read, verb, capsys):
    """With --pool-one-sided only lw.p_one_sided and verdict are added to the pooled row (every other key, of every
    row, is the plain run's) and one line is printed; p_one_sided is the frozen Python's on the pooled pair exactly,
    dSR within 1e-12; the verdict is the bundle's rule, labelled (OD3P-T2V); the fake verb saw exactly one
    --one-sided call: the pooled pair."""
    k0 = verb.mark()
    plain, out_plain = run_json(read["tmp"], pool_argv(read, *verb.argv), capsys, "plain")
    k1 = verb.mark()
    flag, out_flag = run_json(read["tmp"], pool_argv(read, "--pool-one-sided", *verb.argv), capsys, "flag")
    p = flag[-1]["paired"]
    verdict, p1 = p.pop("verdict"), p["lw"].pop("p_one_sided")
    for doc in (flag, plain):
        for r in doc:
            r.pop("nav_summ_run")
    assert json.dumps(flag, sort_keys=True) == json.dumps(plain, sort_keys=True)
    extra = [line for line in out_flag.splitlines() if line not in out_plain.splitlines()]
    assert len(extra) == 1 and extra[0].startswith("   pooled paired one-sided p "), extra
    a, b = pooled_pair(read)
    want = FS.paired_stats(a, b, 200, NS.DEFAULT_BLOCK, NS.V8_SEED, one_sided=True)
    assert p1 == want["lw"]["p_one_sided"] and p1 is not None
    assert math.isclose(p["dsr"], want["dsr"], rel_tol=1e-12, abs_tol=1e-15)
    passed = bool(want["dsr"] > 0 and p1 < NS.BUNDLE_ALPHA)
    assert verdict == {"dsr_positive": bool(want["dsr"] > 0), "p_one_sided": p1, "alpha": NS.BUNDLE_ALPHA,
                       "pass": passed, "rule": verdict["rule"]}
    assert T2_LABEL in verdict["rule"] and "OD3P-T2V" in verdict["rule"]
    assert extra[0] == (f"   pooled paired one-sided p {p1:.4f} (engine verb --one-sided, same resamples); verdict "
                        f"({T2_LABEL}) dSR > 0 and p < {NS.BUNDLE_ALPHA}: {'PASS' if passed else 'FAIL'}")
    if verb.fake is not None:
        plain_calls, flag_calls = verb.fake.calls[k0:k1], verb.fake.calls[k1:]
        assert [c[:3] for c in flag_calls] == [c[:3] for c in plain_calls]      # the same calls in the same order
        one = [c for c in flag_calls if c[3]]
        pooled = digest(pooled_nets(read["r2"], ["E1", "E2", "E3"]))
        pooled_ref = digest(pooled_nets(read["b0c"], ["R1", "R2", "R3"]))
        assert len(one) == 1 and one[0][:2] == ([pooled], pooled_ref) and one[0][3] == ["--one-sided"], one
        assert not any(c[3] for c in plain_calls)


def pooled_nets(dirs: list[Path], ids: list[str]) -> dict:
    """The pooled net series of era dirs, as nav_summ builds the pooled row's and the pooled reference's."""
    return NS.net_series(NS.pooled_daily([str(d) for d in dirs], ids, argparse.Namespace(scenario=None))[0])


@pytest.mark.parametrize("dsr, p1, alpha, passed", [
    (0.4, 0.05, None, True),
    (0.4, 0.2, None, False),
    (0.4, NS.BUNDLE_ALPHA, None, False),
    (0.0, 0.01, None, False),
    (-0.3, 0.01, None, False),
    (float("nan"), 0.01, None, False),
    (0.4, None, None, False),
    (0.4, 0.05, 0.01, False),
    (0.4, 0.005, 0.01, True),
], ids=["pass", "fail_p", "fail_p_at_alpha", "fail_dsr_zero", "fail_dsr_negative", "fail_dsr_nan", "fail_p_null",
        "bundle_alpha_fail", "bundle_alpha_pass"])
def test_t2_verdict_is_the_bundle_rule_on_the_pooled_pair(read, fake_verb, capsys, dsr, p1, alpha, passed):
    """The verdict of --pool-one-sided is the bundle's rule (paired_verdict, one rule in code): PASS iff dSR > 0 (a
    finite dSR) and the one-sided p is not null and below --bundle-alpha (default BUNDLE_ALPHA), on the verb's
    numbers (forced here on the fake); printed with the label "bundle rule, not G-B8" (Ruling OD3P-T2V)."""
    fake_verb.fake.force = (dsr, p1)
    extra = [] if alpha is None else ["--bundle-alpha", str(alpha)]
    listed = [str(read["r2"][2]), "--reference", str(read["b0c"][2])]         # a listed dir's paired block stays
    rows, out = run_json(read["tmp"], listed + pool_argv(read, "--pool-one-sided", *extra, *fake_verb.argv), capsys)
    v = rows[-1]["paired"]["verdict"]
    want_alpha = NS.BUNDLE_ALPHA if alpha is None else alpha
    assert {k: v[k] for k in ("dsr_positive", "p_one_sided", "alpha", "pass")} == {
        "dsr_positive": bool(math.isfinite(dsr) and dsr > 0), "p_one_sided": p1, "alpha": want_alpha, "pass": passed}
    assert T2_LABEL in v["rule"]
    shown = "na" if p1 is None else f"{p1:.4f}"
    assert (f"   pooled paired one-sided p {shown} (engine verb --one-sided, same resamples); verdict ({T2_LABEL}) "
            f"dSR > 0 and p < {want_alpha}: {'PASS' if passed else 'FAIL'}") in out.splitlines()
    assert "verdict" not in rows[0]["paired"] and "p_one_sided" not in rows[0]["paired"]["lw"]   # the pooled row only
    assert [c[3] for c in fake_verb.fake.calls].count(["--one-sided"]) == 1


def test_t2_pool_one_sided_refuses_without_its_inputs(read, fake_verb, capsys):
    """--pool-one-sided needs --pool and a reference (--pool-reference or --reference): exit 2 (usage), nothing run."""
    k = fake_verb.mark()
    for argv in (["--pool-one-sided", str(read["r2"][0]), "--reference", str(read["b0c"][0])],     # no --pool
                 ["--pool", *map(str, read["r2"]), "--pool-one-sided"]):                       # no reference
        with pytest.raises(SystemExit) as e:
            NS.main(argv + fake_verb.argv)
        assert e.value.code == 2
        assert "--pool-one-sided needs --pool and a reference" in capsys.readouterr().err
    assert fake_verb.calls_since(k) == []
    assert NS.main(["--pool", *map(str, read["r2"]), "--pool-one-sided", "--reference", str(read["b0c"][2]),
                    *DRAWS, *fake_verb.argv]) == 0                              # --reference alone is a reference
    assert "verdict (bundle rule, not G-B8)" in capsys.readouterr().out


def comparable(records: list[dict]) -> list[dict]:
    """Ledger lines without the script provenance (run, recorded_by: each script's own SHA-256) and the chain link."""
    return [{k: v for k, v in r.items() if k not in ("run", "recorded_by", "prev_sha256")} for r in records]


def test_t5_pool_over_a_ledgered_train_era_adds_nothing(read, verb, capsys):
    """xpre T5 on the verb: E3's era line is R2's TRAIN cell line already (the same trial_id), so the read appends the
    E1 and E2 era lines and the pooled line and adds 0 trials, V and N unchanged; with --pool-one-sided the ledger's
    bytes are the same."""
    copy = read["tmp"] / "trials-one-sided.jsonl"
    shutil.copyfile(read["ledger"], copy)
    before = BI.ledger_read(read["ledger"])
    run_json(read["tmp"], pool_argv(read, "--origin", "prior", "--ledger", str(read["ledger"]), *verb.argv), capsys)
    after = BI.ledger_read(read["ledger"])
    new = after[len(before):]
    assert [r["window"]["label"] for r in new] == ["ERA E1", "ERA E2", "POOL"]
    assert new[2]["eras"][2]["trial_id"] == before[2]["trial_id"]
    assert BI.trial_counts(after) == BI.trial_counts(before) + [0, 0, 0]          # a history read adds 0 (E-41)
    assert [r["trial_id"] for r in BI.history_read_lines(after)] == [new[2]["trial_id"]]
    wid = BI.window_id()
    assert BI.dsr_variance(after, wid) == BI.dsr_variance(before, wid)            # history lines stay out of V
    assert BI.ledger_n(after, True) == BI.ledger_n(before, True)
    assert RL.cells(read["ledger"]) == [Path(d).as_posix() for d in (read["b0c"][2], read["tmp"] / "OTHER",
                                                                      read["r2"][2])]
    run_json(read["tmp"], pool_argv(read, "--origin", "prior", "--ledger", str(copy), "--pool-one-sided",
                                    *verb.argv), capsys, "one-sided")
    assert copy.read_bytes() == read["ledger"].read_bytes()                     # the same lines, byte for byte


# ------------------------------------------------------------------ identity: --pool-one-sided absent
def load_nav_summ_blob(tmp: Path):
    """Root's nav_summ.py (blob 9fd6bd0a, the LF blob bytes) loaded under a private name."""
    try:
        text = subprocess.run(["git", "cat-file", "-p", NAV_SUMM_BLOB], capture_output=True, check=True,
                              cwd=Path(NS.__file__).resolve().parent).stdout
    except (OSError, subprocess.CalledProcessError):
        pytest.skip(f"git history with the nav_summ blob {NAV_SUMM_BLOB[:8]} is unavailable")
    assert sha(text) == NAV_SUMM_SHA256
    path = tmp / "old" / "nav_summ_9fd6bd0a.py"
    path.parent.mkdir(exist_ok=True)
    path.write_bytes(text)
    spec = importlib.util.spec_from_file_location("nav_summ_9fd6bd0a", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def without_run(doc):
    if isinstance(doc, list):
        return [without_run(r) for r in doc]
    return {k: v for k, v in doc.items() if k != "nav_summ_run"}


def test_flag_absent_nav_summ_equals_roots_blob(read, verb, capsys):
    """Without --pool-one-sided, nav_summ equals root's blob 9fd6bd0a on the verb: stdout, stderr, the JSON document
    minus nav_summ_run and (fake) the verb-call sequence, for a pooled row with a pooled reference (v8), two history
    eras (v7), a listed dir with --reference, --bundle with --bundle-json (the shared paired_verdict) and the ledger
    lines a pooled read appends (without the script provenance and the chain link)."""
    old = load_nav_summ_blob(read["tmp"])
    verb.adopt(old)
    r2, b0c = [str(d) for d in read["r2"]], [str(d) for d in read["b0c"]]
    cases = {
        "pooled_v8": (pool_argv(read), "--json"),
        "history_v7": (["--pool", *r2[:2], "--pool-reference", *b0c[:2], *DRAWS], "--json"),
        "listed_reference": ([r2[2], "--reference", b0c[2], *DRAWS], "--json"),
        "bundle": ([r2[2], "--bundle", b0c[2], r2[2], "--protocol", "v8", *DRAWS], "--bundle-json"),
    }
    for name, (argv, flag) in cases.items():
        seen = {}
        for tag, module in (("new", NS), ("old", old)):
            out = read["tmp"] / f"id-{name}-{tag}.json"
            k = verb.mark()
            assert module.main(argv + verb.argv + [flag, str(out)]) == 0
            got = capsys.readouterr()
            seen[tag] = (got.out, got.err, json.dumps(without_run(json.loads(out.read_text(encoding="utf-8"))),
                                                      sort_keys=True), verb.calls_since(k))
        assert seen["new"] == seen["old"], name
        assert seen["new"][3] is None or seen["new"][3], name                    # (fake) the verb was called
    ledgers = {}                                                                  # the ledger lines a read appends
    for tag, module in (("new", NS), ("old", old)):
        path = read["tmp"] / f"ledger-{tag}.jsonl"
        shutil.copyfile(read["ledger"], path)
        k = verb.mark()
        assert module.main(pool_argv(read, "--origin", "prior", "--ledger", str(path), *verb.argv)) == 0
        got = capsys.readouterr()
        ledgers[tag] = (got.out.replace(str(path), "LEDGER"), got.err, comparable(BI.ledger_read(path)),
                        verb.calls_since(k))
    assert ledgers["new"] == ledgers["old"]
    assert len(ledgers["new"][2]) == len(BI.ledger_read(read["ledger"])) + 3
