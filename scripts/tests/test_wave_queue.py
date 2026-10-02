"""The candidate queue (wave_queue.py, platform v8 lane YINFRA): proposals, pins, the refusals, emit, and the wave's
record stage setting each queued candidate's status.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_queue.py
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import wave_fixture as F  # noqa: E402
import research_cycle as RC  # noqa: E402
import research_wave  # noqa: E402
import wave_manifest as WM  # noqa: E402
import wave_queue as Q  # noqa: E402

DIR = "q"


def propose(root: Path, cid: str, *extra: str, dsl: str | None = None, hypothesis: str | None = None) -> int:
    return RC.main(["candidates", "new", "--root", str(root), "--dir", DIR, "--id", cid, "--dsl",
                    dsl or f"rank(ts_mean({cid}_field, 20))", "--theme", "value", "--tier", "B", "--prior-sign", "1",
                    "--citation", "Synthetic 2026", "--origin", "prior", "--hypothesis", hypothesis or f"h-{cid}",
                    "--by", "lane-ysig", "--at", "2026-10-02", *extra])


def doc(root: Path, cid: str) -> dict:
    return json.loads((root / DIR / f"{cid}.json").read_text())


def test_propose_validate_and_the_refusals(tmp_path, capsys):
    root = tmp_path
    assert propose(root, "alpha_a", "--fields", "a,b") == 0
    d = doc(root, "alpha_a")
    assert d["status"] == "proposed" and d["wave"] is None and d["dsl_sha256"] == WM.dsl_sha256(d["dsl"])
    assert d["fields"] == ["a", "b"] and d["history"] == [{"status": "proposed", "at": "2026-10-02", "by": "lane-ysig"}]
    assert RC.main(["candidates", "validate", "--root", str(root), "--dir", DIR]) == 0
    assert propose(root, "alpha_a") == 2                                            # proposed once
    assert propose(root, "alpha_b", dsl=d["dsl"]) == 2                              # the same DSL twice
    assert "queued already as alpha_a" in capsys.readouterr().err
    assert propose(root, "alpha_c", hypothesis="h-alpha_a") == 2                    # a second variant
    assert "second variant of hypothesis 'h-alpha_a'" in capsys.readouterr().err
    assert propose(root, "alpha_c", "--ruling", "PM8-1", hypothesis="h-alpha_a") == 0   # allowed by a ruling
    F.write_json(root, Q.REGISTRY, {"alphas": [{"id": "alpha_d", "dsl": "rank(other)"},
                                               {"id": "alpha_e", "dsl": "rank(alpha_e_field)"}]})
    assert propose(root, "alpha_d") == 2                                            # registered with another DSL
    assert "holds alpha_d with another DSL" in capsys.readouterr().err
    assert propose(root, "alpha_f", dsl="rank(alpha_e_field)") == 2                 # registered under another id
    assert "registered as alpha_e" in capsys.readouterr().err
    (root / DIR / "alpha_c.json").write_text(json.dumps(dict(doc(root, "alpha_c"), dsl="rank(x)")))
    assert RC.main(["candidates", "validate", "--root", str(root), "--dir", DIR]) == 2
    assert "not the SHA-256 of the frozen DSL" in capsys.readouterr().err


def test_pin_transitions_and_emit(tmp_path, capsys):
    root = tmp_path
    for cid in ("alpha_a", "alpha_b"):
        assert propose(root, cid) == 0
    head = {k: v for k, v in F.manifest().items() if k != "candidates"}
    head["fields"] = dict(head["fields"], manifest_sha256="0" * 64)
    F.write_json(root, "head.json", head)
    emit = ["candidates", "emit", "--root", str(root), "--dir", DIR, "--head", str(root / "head.json")]
    assert RC.main(emit + ["--select", "alpha_a,alpha_b", "--output", "w.json"]) == 2   # not pinned
    assert "not pinned" in capsys.readouterr().err
    assert RC.main(["candidates", "pin", "--root", str(root), "--dir", DIR, "--id", "alpha_a", "--id", "alpha_b",
                    "--by", "pm", "--ruling", "PM8-2", "--at", "2026-10-03"]) == 0
    assert doc(root, "alpha_a")["history"][-1] == {"status": "pinned", "at": "2026-10-03", "by": "pm",
                                                   "note": "ruling PM8-2"}
    with pytest.raises(Q.QueueError, match="final"):
        Q.transition(dict(doc(root, "alpha_a"), status="in-book"), "pinned", "pm", "2026-10-03")
    with pytest.raises(Q.QueueError, match="proposed -> in-book"):
        Q.transition(dict(doc(root, "alpha_a"), status="proposed"), "in-book", "pm", "2026-10-03")
    assert RC.main(emit + ["--select", "alpha_b,alpha_a", "--output", "w.json"]) == 0
    m = json.loads((root / "w.json").read_text())
    assert WM.validate(m) == [] and [c["id"] for c in m["candidates"]] == ["alpha_b", "alpha_a"]
    assert not {"status", "wave", "history", "schema"} & set(m["candidates"][0])
    assert RC.main(emit + ["--select", "alpha_a", "--output", "w.json"]) == 2          # written once
    F.write_json(root, "result.json", {"next_parent": {"spec": "scripts/specs/v8/lib-x.json", "library": "x"},
                                       "ledger": {"n_after": 7}})
    assert RC.main(emit + ["--select", "alpha_a", "--output", "w2.json", "--after", str(root / "result.json")]) == 0
    m2 = json.loads((root / "w2.json").read_text())
    assert m2["parent"] == {"spec": "scripts/specs/v8/lib-x.json", "library": "x"} and m2["expect"] == {"n_before": 7}


def queue_root(tmp_path: Path, pin: bool = True) -> Path:
    """A wave_fixture root whose three candidates are queued (and pinned)."""
    root = F.build(tmp_path / "r")
    for c in F.manifest()["candidates"]:
        d = dict(c, schema=Q.SCHEMA, status="proposed", wave=None,
                 history=[{"status": "proposed", "at": "2026-10-02", "by": "lane"}])
        if pin:
            d = Q.transition(d, "pinned", "pm", "2026-10-02")
        Q.write(root, d)
    F.git(root, "add", "-A")
    F.git(root, "commit", "-q", "-m", "queue")
    return root


def test_the_wave_sets_the_queue_status_of_its_candidates(tmp_path):
    root = queue_root(tmp_path)
    fake = F.FakeCycle(root, {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", -1), "alpha_c": ("admitted", 0)})
    lines: list[str] = []
    code = research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=fake, log=lines.append)
    assert code == 0, lines
    q = Q.load(root)
    assert {cid: (c["status"], c["wave"]) for cid, c in q.items()} == {
        "alpha_a": ("in-book", "w1"), "alpha_b": ("dropped", "w1"), "alpha_c": ("in-book", "w1")}
    assert q["alpha_b"]["history"][-1]["by"] == "research_cycle.py wave"
    assert F.git(root, "log", "-1", "--format=%s").startswith("wave w1: queue status of alpha_a, alpha_b, alpha_c")
    assert F.git(root, "status", "--porcelain", "--", "scripts") == ""
    assert RC.main(["candidates", "validate", "--root", str(root)]) == 0


def test_preflight_refuses_a_queued_candidate_that_is_not_pinned(tmp_path):
    root = queue_root(tmp_path, pin=False)
    lines: list[str] = []
    code = research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=F.FakeCycle(root, {}),
                              log=lines.append)
    err = json.loads((root / "out/waves/w1/receipts/01-preflight.failed-1.json").read_text())["error"]
    assert code == 3 and "queue status 'proposed', not pinned" in err
