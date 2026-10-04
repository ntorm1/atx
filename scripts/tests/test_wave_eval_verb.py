"""Ruling B2-10 (c): a P9 cell (a v2 wave manifest, atx.research-wave/v2) names the engine verb of its statistics.

Its locked spec names exes.eval and pins it (exes_sha256.eval, `lock --exes`), and the wave's verify refuses, fail
closed, a cell whose spec lacks either key or whose summ ran on another verb (nav_summ_run.engine_exe_sha256 of
summ.json / pbo.json); a v1 manifest keeps today's behaviour (ruling B2-10 (a): plan bytes and verdict unchanged).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_eval_verb.py

This lane's tree has no v2 manifest (lane E2 adds it, wave_kinds.py): the v2 case is the fixture's wave, run as v1 on
fakes, then verified with its manifest's schema set to atx.research-wave/v2 (the one key wave_kinds.is_v2 reads).
Synthetic files only (wave_fixture.py).
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import sys

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import research_cycle as RC  # noqa: E402
import research_wave  # noqa: E402
import wave_fixture as F  # noqa: E402
import wave_stage_record as WR  # noqa: E402
from wave_context import Wave  # noqa: E402
from wave_stage_util import StageError  # noqa: E402

ADMIT_ALL = {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", 1), "alpha_c": ("admitted", 1)}
CELL = "scripts/specs/v8/lib-w1.json"
# verify's plan line of a v1 manifest at the deletion commit 598d2199: its bytes do not move (ruling B2-10 (a))
V1_VERIFY_PLAN = ["#   mechanics rule v8-mech on the mechanics keys; C-13 binding of the cell's NAV; NAV exe sha256 of "
                  "the parent vs the cell (recorded; differing, the cell's ref must have run on the cell's exe); seal "
                  "scan of every log the wave produced (run dirs of the screen, the cell and its -gm copy, readers, "
                  "consoles)"]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def ran_wave(tmp_path: Path) -> tuple[Path, F.FakeCycle, list[str]]:
    """The fixture's v1 wave run to its result on fakes (no cell spec names exes.eval), and its plan lines."""
    root = F.build(tmp_path / "w")
    fake = F.FakeCycle(root, ADMIT_ALL)
    plan: list[str] = []
    assert research_wave.main(["plan", F.MANIFEST, "--root", str(root)], executor=fake, log=plan.append) == 0
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=fake, log=lambda s: None) == 0
    return root, fake, plan


def stage_outputs(root: Path, before: str) -> dict:
    """{stage: outputs} of the wave's receipts, in order, up to (without) the stage ``before``."""
    done = {}
    for p in sorted((root / "out/waves/w1/receipts").glob("*.json")):
        stage = p.stem.split("-", 1)[1]
        if stage == before:
            break
        done[stage] = json.loads(p.read_text(encoding="utf-8"))["outputs"]
    return done


def wave(root: Path, fake, schema: str | None = None) -> Wave:
    w = Wave(F.MANIFEST, root, executor=fake, log=lambda s: None)
    if schema:
        w.manifest["schema"] = schema
    return w


def set_spec(root: Path, **keys) -> None:
    doc = json.loads((root / CELL).read_text(encoding="utf-8"))
    for k, v in keys.items():
        if v is None:
            doc.pop(k, None)
        else:
            doc[k] = v
    F.write_json(root, CELL, doc)


def set_summ(root: Path, w: Wave, verb_sha: str | None, pbo_sha: str | None = None) -> str:
    """The cell's summ.json rows (and with ``pbo_sha`` a pbo.json) naming the verb of that hash (None: no verb)."""
    cyc = w.outputs(CELL)["cycle_dir"]
    rows = json.loads((root / cyc / "summ.json").read_text(encoding="utf-8"))
    run = {"argv": [], "engine_exe": "bin/atx-research-eval.exe", "engine_exe_sha256": verb_sha} if verb_sha else {}
    F.write_json(root, f"{cyc}/summ.json", [dict(r, nav_summ_run=run) for r in rows])
    if pbo_sha:
        F.write_json(root, f"{cyc}/pbo.json", {"pbo": 0.2, "nav_summ_run": {"engine_exe_sha256": pbo_sha}})
    return cyc


def test_a_v2_cell_without_exes_eval_is_refused_at_verify(tmp_path):
    root, fake, _ = ran_wave(tmp_path)
    done = stage_outputs(root, "verify")
    assert done["match"]["cell_spec"] == CELL and "exes" in json.loads((root / CELL).read_text())
    w = wave(root, fake, WR.V2_SCHEMA)
    with pytest.raises(StageError, match=rf"the v2 cell spec {CELL} lacks exes\.eval and exes_sha256\.eval: every "
                                         r"P9 cell names the engine verb"):
        WR.verify(w, done, lambda s: None)
    pin = sha(b"the eval verb")
    set_spec(root, exes={"ic": "bin/ic.exe", "nav": "bin/nav.exe", "eval": "bin/atx-research-eval.exe"})
    assert "lacks exes_sha256.eval:" in WR.eval_problem(wave(root, fake, WR.V2_SCHEMA), done)   # named, not pinned
    set_spec(root, exes_sha256={"eval": pin})
    set_summ(root, w, pin)
    out = WR.verify(wave(root, fake, WR.V2_SCHEMA), done, lambda s: None)   # named, pinned, summ on it: passes
    assert out["mechanics"]["pass"] and out["exes_sha256"]["cell"] == {"eval": pin}


def test_a_v2_cell_whose_summ_ran_on_another_verb_is_refused(tmp_path):
    root, fake, _ = ran_wave(tmp_path)
    done = stage_outputs(root, "verify")
    pin, other = sha(b"the eval verb"), sha(b"another eval verb")
    set_spec(root, exes={"ic": "bin/ic.exe", "nav": "bin/nav.exe", "eval": "bin/atx-research-eval.exe"},
             exes_sha256={"eval": pin})
    w = wave(root, fake, WR.V2_SCHEMA)
    cyc = set_summ(root, w, other)
    with pytest.raises(StageError, match=rf"verify: v2 cell \(ruling B2-10 \(c\)\): {re.escape(cyc)}/summ\.json row "
                                         rf".*engine verb {other} .*exes_sha256 pin of exes\.eval "
                                         rf"bin/atx-research-eval\.exe {pin}"):
        WR.verify(w, done, lambda s: None)
    set_summ(root, w, None)                                                    # a summ that names no verb
    assert "engine verb none named" in WR.eval_problem(w, done)
    set_summ(root, w, pin, pbo_sha=other)                                      # the PBO of another verb
    assert f"{cyc}/pbo.json: its statistics come from the engine verb {other}" in WR.eval_problem(w, done)
    (root / cyc / "pbo.json").unlink()
    (root / cyc / "summ.json").unlink()                                        # no summ rows: fail closed
    assert "has no nav_summ --json rows" in WR.eval_problem(w, done)


def test_a_v1_manifest_plans_and_verifies_as_before(tmp_path):
    """Ruling B2-10 (a): a v1 manifest's plan bytes and verdict are unchanged: no exes.eval asked for, the summ's verb
    recorded only, verify's plan line byte-identical to the deletion commit's."""
    root, fake, plan = ran_wave(tmp_path)
    done = stage_outputs(root, "verify")
    w = wave(root, fake)
    assert w.manifest["schema"] == "atx.research-wave/v1" and WR.eval_problem(w, done) is None
    assert WR.verify_plan(w, done) == V1_VERIFY_PLAN and V1_VERIFY_PLAN[0] in plan
    set_summ(root, w, sha(b"another eval verb"))                               # a v1 cell: recorded, not refused
    assert WR.eval_problem(w, done) is None and WR.verify(w, done, lambda s: None)["mechanics"]["pass"]
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text(encoding="utf-8"))
    assert res["verdict"]["rule"] == "pm7-34"                                  # the run judged and recorded the cell
    assert RC.EXES_PIN not in json.loads((root / CELL).read_text(encoding="utf-8"))
