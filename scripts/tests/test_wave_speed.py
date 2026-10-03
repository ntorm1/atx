"""The wave's speed rules (platform v8 lane YINFRA, item 4) and the replacing-wave marginal (PM6-8 (i)).

  reuse_screen_marginal  the b library's cell spec has no marginal phase (no second 134.6-175.5 s pass) when the
                         screen ran the b library's marginal mode; the screen's per-row fields of the kept strings
                         are carried into wave-result.json (report only)
  screen_first           a b library runs `run --screen` before its cell (its u pass without the unread blend)
Both default on and change no input of a decision; off, the b library runs as add-alpha derives it.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_speed.py
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
import wave_scoreboard as S  # noqa: E402

ONE_DROPPED = {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", -1), "alpha_c": ("admitted", 1)}
B_SPEC = "scripts/specs/v8/lib-w1b.json"


def run(root: Path, fake) -> list[str]:
    lines: list[str] = []
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=fake, log=lines.append) == 0, lines
    return lines


def cycle_runs(fake) -> list[list[str]]:
    return [F.unrooted(c[2:]) for c in fake.calls if Path(c[1]).name == "research_cycle.py" and c[2] in ("run", "lock")]


def test_the_b_cell_reuses_the_screen_marginal_and_screens_first(tmp_path):
    root = F.build(tmp_path / "r")
    fake = F.FakeCycle(root, ONE_DROPPED)
    run(root, fake)
    b = json.loads((root / B_SPEC).read_text())
    assert "marginal" not in b and "phases" not in b["runner"]                         # no second marginal pass
    assert RC.load_spec(root / B_SPEC)["inputs"]["reference_weights"]                  # every pin kept
    assert "marginal" in json.loads((root / "scripts/specs/v8/lib-w1.json").read_text())   # the screen kept it
    runs = cycle_runs(fake)
    k = runs.index(["run", B_SPEC, "--screen"])
    assert runs[k - 1] == ["lock", B_SPEC] and runs[k + 1] == ["run", B_SPEC, "--stop-after", "nav"]
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text())
    assert res["marginal"]["source"].startswith("the screen (scripts/specs/v8/lib-w1.json): per-row fields carried")
    assert [r["id"] for r in res["marginal"]["rows"]] == ["alpha_a", "alpha_c"]          # the kept strings' rows


def test_speed_off_runs_the_b_library_as_add_alpha_derives_it(tmp_path):
    root = F.build(tmp_path / "r", speed={"reuse_screen_marginal": False, "screen_first": False})
    fake = F.FakeCycle(root, ONE_DROPPED)
    run(root, fake)
    b = json.loads((root / B_SPEC).read_text())
    assert b["marginal"]["themes"] == "reference_weights" and b["runner"]["phases"] == {"marginal": {"seconds": 360}}
    assert ["run", B_SPEC, "--screen"] not in cycle_runs(fake) and ["lock", B_SPEC] not in cycle_runs(fake)


def test_a_replacing_wave_runs_its_marginal_on_the_pool_only(tmp_path):
    cands = [F.candidate("alpha_a"), F.candidate("alpha_r", kind="replace", replaces=["m1"])]
    root = F.build(tmp_path / "r", candidates=cands, speed={"reuse_screen_marginal": False})
    fake = F.FakeCycle(root, {"alpha_a": ("admitted", 1), "alpha_r": ("admitted", -1)})
    run(root, fake)
    for spec in ("scripts/specs/v8/lib-w1.json",):                                      # the screen library
        m = json.loads((root / spec).read_text())["marginal"]
        assert "themes" not in m and m["output"] == "out/u-w1-marginal-poolonly"
    assert ["lock", "scripts/specs/v8/lib-w1.json"] in cycle_runs(fake)
    b = json.loads((root / B_SPEC).read_text())["marginal"]                             # alpha_r dropped: b adds only
    assert b["themes"] == "reference_weights"


def test_a_ruled_marginal_runs_pool_only_with_its_cap_and_the_b_cell_reuses_it(tmp_path):
    """PM8-15 (integration of Y): an add-alpha wave on a theme-erc parent with more themes than the marginal verb
    takes runs the marginal on the pool only (PM6-8 (i)) under the ruled phase cap; the b library carries the rows."""
    rule = {"ruling": "PM8-15", "pool_only": True, "seconds": 720}
    root = F.build(tmp_path / "r", marginal=rule)
    fake = F.FakeCycle(root, ONE_DROPPED)
    lines = run(root, fake)
    s = json.loads((root / "scripts/specs/v8/lib-w1.json").read_text())
    assert "themes" not in s["marginal"] and s["marginal"]["output"] == "out/u-w1-marginal-poolonly"
    assert s["runner"]["phases"]["marginal"] == {"seconds": 720}
    assert any("marginal as ruled (PM8-15)" in x for x in lines)
    b = json.loads((root / B_SPEC).read_text())
    assert "marginal" not in b and "phases" not in b["runner"]                         # the same mode: rows carried
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text())
    assert [r["id"] for r in res["marginal"]["rows"]] == ["alpha_a", "alpha_c"]
    off = F.build(tmp_path / "s", marginal=rule, speed={"reuse_screen_marginal": False})
    run(off, F.FakeCycle(off, ONE_DROPPED))
    b = json.loads((off / B_SPEC).read_text())
    assert "themes" not in b["marginal"] and b["runner"]["phases"]["marginal"] == {"seconds": 720}
    assert F.WM.validate(F.manifest(marginal={"pool_only": True})) and F.WM.validate(F.manifest(marginal={
        "ruling": "PM8-15", "seconds": 0}))                                             # a ruling and a positive cap


def test_the_scoreboard_prints_wall_seconds_by_phase(tmp_path, capsys):
    root = F.build(tmp_path / "r")
    run(root, F.FakeCycle(root, ONE_DROPPED, gross_per_l={"w1b": 0.9604 / 1.1474}))   # calibration + matched NAV
    b = S.board(root, ["out/waves/*/wave-result.json"])
    t = b["timings"][0]
    assert t["wave"] == "w1" and t["phases"]["nav"]["runs"] == 2 and t["phases"]["nav"]["seconds"] == 83.0
    assert RC.main(["scoreboard", "--root", str(root), "--results", "out/waves/*/wave-result.json", "--timings"]) == 0
    assert "### Wall-clock by phase" in capsys.readouterr().out
