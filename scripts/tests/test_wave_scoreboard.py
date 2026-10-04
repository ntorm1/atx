"""The book scoreboard (wave_scoreboard.py, platform v8 lane YINFRA): lineage of accepted books from wave results and
the ledger only, with the ledger cross-checks.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_scoreboard.py
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import wave_fixture as F  # noqa: E402
import research_cycle as RC  # noqa: E402
import research_wave  # noqa: E402
import wave_queue as Q  # noqa: E402
import wave_scoreboard as S  # noqa: E402

W2 = "scripts/specs/v8/waves/w2.json"


def run_wave(root: Path, manifest: str, fake) -> None:
    lines: list[str] = []
    assert research_wave.main(["run", manifest, "--root", str(root)], executor=fake, log=lines.append) == 0, lines


def two_waves(tmp_path: Path, second_accepted: bool) -> Path:
    """Wave w1 (accepted) on the fixture's parent, then wave w2 emitted from the queue on w1's result."""
    root = F.build(tmp_path / "r")
    run_wave(root, F.MANIFEST, F.FakeCycle(root, {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", 1),
                                                  "alpha_c": ("admitted", 1)}))
    for cid in ("beta_a", "beta_b"):
        d = dict(F.candidate(cid), schema=Q.SCHEMA, status="proposed", wave=None,
                 history=[{"status": "proposed", "at": "2026-10-02", "by": "lane"}])
        Q.write(root, Q.transition(d, "pinned", "pm", "2026-10-02"))
    head = {k: v for k, v in F.manifest(wave="w2", library="w2", out_dir="out/waves/w2").items() if k != "candidates"}
    head["fields"] = json.loads((root / F.MANIFEST).read_text())["fields"]
    F.write_json(root, "head.json", head)
    assert RC.main(["candidates", "emit", "--root", str(root), "--head", str(root / "head.json"), "--select",
                    "beta_a,beta_b", "--output", W2, "--after", str(root / "out/waves/w1/wave-result.json")]) == 0
    F.git(root, "add", "-A", "scripts")
    F.git(root, "commit", "-q", "-m", "wave w2 manifest")
    run_wave(root, W2, F.FakeCycle(root, {"beta_a": ("admitted", 1), "beta_b": ("admitted", 1)},
                                   dsr=0.02 if second_accepted else -0.02))
    return root


def test_the_lineage_follows_accepted_books(tmp_path, capsys):
    root = two_waves(tmp_path, second_accepted=True)
    m2 = json.loads((root / W2).read_text())
    assert m2["parent"] == {"spec": "scripts/specs/v8/lib-w1.json", "library": "w1"} and m2["expect"] == {"n_before": 3}
    out = root / "board.json"
    assert RC.main(["scoreboard", "--root", str(root), "--results", "out/waves/*/wave-result.json",
                    "--json", str(out)]) == 0
    b = json.loads(out.read_text())
    assert [r["wave"] for r in b["lineage"]] == ["(parent)", "w1", "w2"]
    assert [r["cell"] for r in b["lineage"]] == [F.PARENT, "scripts/specs/v8/lib-w1.json", "scripts/specs/v8/lib-w2.json"]
    assert [r["n"] for r in b["lineage"]] == [2, 3, 4] and b["ledger"]["n"] == 4
    assert b["lineage"][1]["net_sharpe"] == 1.2 and b["lineage"][1]["x4_net_sharpe"] == 1.1
    assert all(c["ledgered"] and c["s2_net_sr_equal"] for c in b["checks"])
    text = capsys.readouterr().out
    assert "### Accepted lineage" in text and "| w2 | ACCEPTED | `scripts/specs/v8/lib-w2.json` |" in text
    assert {c["status"] for c in Q.load(root).values()} == {"in-book"}


def test_a_rejected_wave_is_listed_but_not_in_the_lineage(tmp_path):
    root = two_waves(tmp_path, second_accepted=False)
    b = S.board(root, ["out/waves/*/wave-result.json"])
    assert [r["wave"] for r in b["lineage"]] == ["(parent)", "w1"]
    assert [(r["wave"], r["verdict"]) for r in b["waves"]] == [("w1", "ACCEPTED"), ("w2", "NOT ACCEPTED")]
    assert Q.load(root)["beta_a"]["status"] == "admitted"                          # kept by the screen, cell rejected


def test_the_ledger_checks_and_conflicting_copies(tmp_path, capsys):
    root = two_waves(tmp_path, second_accepted=True)
    p = root / "out/waves/w2/wave-result.json"
    doc = json.loads(p.read_text())
    doc["stats"]["cell"]["net_sharpe"] = 9.9                                        # not what the ledger holds
    p.write_text(json.dumps(doc))
    assert RC.main(["scoreboard", "--root", str(root), "--results", "out/waves/*/wave-result.json"]) == 1
    assert "MISMATCH" in capsys.readouterr().out
    copy = root / "sprint/waves/w2/wave-result.json"                                # the record stage's copy differs
    assert RC.main(["scoreboard", "--root", str(root), "--results", "out/waves/*/wave-result.json",
                    "--results", "sprint/waves/*/wave-result.json"]) == 2
    assert "differ" in capsys.readouterr().err and copy.is_file()
