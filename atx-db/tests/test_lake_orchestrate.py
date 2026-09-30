"""Orchestrator on fake stages: a changed input plans only its downstream; a changed leaf module plans only that
leaf; ``--run`` executes due stages in topological order and legacy stages become incremental via the sidecar."""

from __future__ import annotations

import os
import subprocess
import sys
import textwrap
from pathlib import Path

from atx_db.stagelake.contract import SRC_ROOT, Output, Stage, validate
from atx_db.stagelake.orchestrate import execute, plan

# src1 -> mid -> leaf1 ; src1 -> leaf2 ; src2 -> leaf3 ; legacy (reads src2, records no binding)
GRAPH = {"src1": (), "src2": (), "mid": ("src1",), "leaf1": ("mid",), "leaf2": ("src1",), "leaf3": ("src2",),
         "legacy": ("src2",)}

COMMON = '''
"""Fake stage publisher: one Parquet output and a contract manifest (input bindings unless legacy)."""
import hashlib, json, os, sys
from pathlib import Path
import pyarrow as pa, pyarrow.parquet as pq


def publish(name, inputs, legacy=False):
    root = Path(os.environ["ATX_ALPHA_PANEL_ROOT"])
    out = root / name
    out.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.table({"k": [int(os.environ.get("FAKE_V", "1"))], "available_at": pa.array([0], pa.timestamp("us"))}), out / "data.parquet")
    src = Path(sys.modules["__main__"].__file__)
    blob = src.read_bytes()
    man = {"schema": "atx.test/v1", "status": "complete", "stage": name,
           "code": {src.name: {"sha256": hashlib.sha256(blob).hexdigest(),
                               "sha256_lf": hashlib.sha256(blob.replace(b"\\r\\n", b"\\n")).hexdigest()}},
           "files": {"data.parquet": {"bytes": (out / "data.parquet").stat().st_size,
                                      "sha256": hashlib.sha256((out / "data.parquet").read_bytes()).hexdigest()}}}
    if not legacy:
        man["input_manifests_sha256"] = {f"{i}/manifest.json": hashlib.sha256((root / i / "manifest.json").read_bytes()).hexdigest()
                                         for i in inputs}
    with open(root / "_runs.log", "a") as fh:
        fh.write(name + "\\n")
    (out / "manifest.json").write_text(json.dumps(man, sort_keys=True))
'''


def _fixture(tmp_path: Path) -> tuple[Path, Path, list[Stage], dict[str, str]]:
    src = tmp_path / "src"
    pkg = src / "fakelake"
    pkg.mkdir(parents=True)
    (pkg / "__init__.py").write_text("")
    (pkg / "common.py").write_text(COMMON)
    for name, inputs in GRAPH.items():
        (pkg / f"s_{name}.py").write_text(textwrap.dedent(f"""
            from fakelake.common import publish
            publish({name!r}, {list(inputs)!r}, legacy={name == 'legacy'})
        """))
    stages = validate([Stage(n, "PLAT", "atx.test/v1", f"fakelake.s_{n}", inputs=i, guard_gb=0.2,
                             outputs=(Output(f"{n}/data.parquet"),)) for n, i in GRAPH.items()])
    root = tmp_path / "lake"
    root.mkdir()
    env = dict(os.environ, ATX_ALPHA_PANEL_ROOT=str(root), PYTHONPATH=os.pathsep.join([str(src), str(SRC_ROOT)]))
    return root, src, stages, env


def _runner(env: dict[str, str]):
    def run(stage: Stage, cmd: list[str], cap: float, log: Path) -> int:
        return subprocess.call([sys.executable, *cmd], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    return run


def _due(root: Path, stages: list[Stage], src: Path) -> dict[str, list[str]]:
    return {i.stage: i.reasons for i in plan(root, stages, src_root=src) if i.due}


def _runs(root: Path) -> list[str]:
    p = root / "_runs.log"
    return p.read_text().split() if p.exists() else []


def test_first_run_builds_all_in_topological_order_then_nothing_is_due(tmp_path: Path) -> None:
    root, src, stages, env = _fixture(tmp_path)
    items = plan(root, stages, src_root=src)
    assert all(i.due and i.action == "run" for i in items)
    rec = execute(root, stages, items, _runner(env), src_root=src)
    assert [r["rc"] for r in rec] == [0] * len(GRAPH)
    order = _runs(root)
    assert order.index("src1") < order.index("mid") < order.index("leaf1") and order.index("src2") < order.index("leaf3")
    # legacy records no input binding: the orchestrator's sidecar binds exactly its published manifest
    assert (root / "_lake" / "bindings" / "legacy.json").is_file()
    assert _due(root, stages, src) == {}


def test_changed_input_plans_only_its_downstream(tmp_path: Path) -> None:
    root, src, stages, env = _fixture(tmp_path)
    execute(root, stages, plan(root, stages, src_root=src), _runner(env), src_root=src)
    # an identical republish changes nothing (deterministic bytes) ...
    subprocess.check_call([sys.executable, "-m", "fakelake.s_src1"], env=env)
    assert _due(root, stages, src) == {}
    # ... new data in src1 (same code) changes its manifest
    subprocess.check_call([sys.executable, "-m", "fakelake.s_src1"], env={**env, "FAKE_V": "2"})
    due = _due(root, stages, src)
    assert set(due) == {"mid", "leaf1", "leaf2"}
    assert due["mid"] == ["input changed: src1"] and due["leaf1"] == ["upstream due: mid"]
    before = len(_runs(root))
    rec = execute(root, stages, plan(root, stages, src_root=src), _runner(env), src_root=src)
    ran = [r["stage"] for r in rec]
    assert sorted(ran) == ["leaf1", "leaf2", "mid"] and ran.index("mid") < ran.index("leaf1")  # topological
    assert len(_runs(root)) == before + 3 and _due(root, stages, src) == {}


def test_changed_leaf_code_plans_only_that_leaf(tmp_path: Path) -> None:
    root, src, stages, env = _fixture(tmp_path)
    execute(root, stages, plan(root, stages, src_root=src), _runner(env), src_root=src)
    leaf = src / "fakelake" / "s_leaf3.py"
    leaf.write_text(leaf.read_text() + "# v2\n")
    assert _due(root, stages, src) == {"leaf3": ["code changed: s_leaf3.py"]}
    # line endings alone do not make a stage due (LF-normalised SHA)
    other = src / "fakelake" / "s_leaf1.py"
    other.write_bytes(other.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
    assert set(_due(root, stages, src)) == {"leaf3"}


def test_platform_module_change_needs_strict_flag_and_failure_stops_the_run(tmp_path: Path) -> None:
    root, src, stages, env = _fixture(tmp_path)
    execute(root, stages, plan(root, stages, src_root=src), _runner(env), src_root=src)
    (src / "fakelake" / "common.py").write_text(COMMON + "\n# platform tweak\n")
    assert _due(root, stages, src) == {}  # common.py is platform code (not recorded by these fakes either)
    (src / "fakelake" / "s_mid.py").write_text("raise SystemExit(3)\n")
    items = plan(root, stages, src_root=src)
    assert [i.stage for i in items if i.due] == ["mid", "leaf1"]
    rec = execute(root, stages, items, _runner(env), src_root=src)
    assert [(r["stage"], r["rc"]) for r in rec] == [("mid", 3)]  # leaf1 never ran


def test_only_and_from_select_the_reported_stages(tmp_path: Path) -> None:
    root, src, stages, _ = _fixture(tmp_path)
    assert [i.stage for i in plan(root, stages, only=["leaf3"], src_root=src)] == ["leaf3"]
    assert {i.stage for i in plan(root, stages, start="src1", src_root=src)} == {"src1", "mid", "leaf1", "leaf2"}
