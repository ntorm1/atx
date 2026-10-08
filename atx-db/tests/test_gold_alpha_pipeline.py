"""The build plan preserves the seal, dependencies and resumability."""

import argparse
import importlib.util
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "build_gold_alpha_panels.py"
SPEC = importlib.util.spec_from_file_location("gold_pipeline", SCRIPT)
P = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(P)


def arguments(root, **changes):
    return argparse.Namespace(root=root, panel_stage="panel_goldready", label_stage="labels_goldready",
                              years="2020-2026", memory="560MB", role=[], skip_silver=False,
                              dry_run=True, no_resume=False, **changes)


def test_plan_is_sequential_strict_sealed_and_consumer_ready(tmp_path):
    steps = P.plan(arguments(tmp_path))
    assert steps[0]["command"][2] == "atx_db.alpha_panel.silver_refresh"
    labels = next(s for s in steps if s["name"] == "strict_labels")
    assert "strict" in labels["command"] and "labels_goldready" in labels["command"]
    assert labels["command"][labels["command"].index("--years") + 1:][:5] == ["2019", "2020", "2021", "2022", "2023"]
    role = next(s for s in steps if s["name"] == "consumer_role")
    assert role["command"][role["command"].index("--end") + 1] == "2023-12-29"
    assert next(s for s in steps if s["name"] == "consumer_gold")["dependencies"][-1].endswith("role/manifest.json")
    legacy = next(s for s in steps if s["name"] == "legacy_sensitivity")
    assert legacy["env"]["ATX_ALPHA_LABEL_STAGE"] == "labels_goldready_legacy"
    assert "gold/selection.json" in legacy["dependencies"]


def test_resume_requires_exact_inputs_outputs_and_transitive_code(tmp_path):
    output = tmp_path / "output.json"
    output.write_text("first")
    step = {"command": ["python", "-m", "atx_db.alpha_panel.ic_report"], "artifacts": ["output.json"]}
    code = {"ic_report.py": {"sha256_lf": "abc"}}
    previous = {"status": "complete", "command": step["command"], "input_pins": {"source": "123"},
                "code_pins": code, "output_pins": P.pins(tmp_path, step["artifacts"])}
    assert P.reusable_step(tmp_path, previous, step, {"source": "123"}, code)
    assert not P.reusable_step(tmp_path, previous, step, {"source": "456"}, code)
    assert not P.reusable_step(tmp_path, previous, step, {"source": "123"}, {"changed": True})
    output.write_text("changed")
    assert not P.reusable_step(tmp_path, previous, step, {"source": "123"}, code)
    output.unlink()
    assert not P.reusable_step(tmp_path, previous, step, {"source": "123"}, code)


def test_live_owner_blocks_and_dead_owner_lock_is_recovered(tmp_path, monkeypatch):
    lock = tmp_path / "pipeline.lock"
    lock.write_text("123")
    monkeypatch.setattr(P, "pid_alive", lambda pid: True)
    with pytest.raises(RuntimeError, match="already running"):
        P.acquire_lock(lock)
    monkeypatch.setattr(P, "pid_alive", lambda pid: False)
    P.acquire_lock(lock)
    assert int(lock.read_text()) > 0


def test_skip_silver_still_verifies_dependencies(tmp_path, monkeypatch):
    args = arguments(tmp_path)
    args.skip_silver = True
    args.dry_run = False
    def reject(*_args, **_kwargs):
        raise ValueError("incomplete frozen silver signature")
    monkeypatch.setattr(P, "verify_stage", reject)
    with pytest.raises(ValueError, match="signature"):
        P.execute(args)
    assert not (tmp_path / "validation" / "gold_pipeline" / "pipeline.lock").exists()
    assert '"status": "failed"' in (tmp_path / "validation" / "gold_pipeline" / "quality.json").read_text()


def test_dry_run_never_creates_outputs(tmp_path):
    result = P.execute(arguments(tmp_path))
    assert result["dry_run"] and not list(tmp_path.iterdir())


def test_dependencies_distinguish_calendar_and_manifest_paths():
    assert P.dependency_path("calendar.parquet") == "calendar.parquet"
    assert P.dependency_path("panel_goldready") == "panel_goldready/manifest.json"
    assert P.dependency_path("prices/manifest.json") == "prices/manifest.json"


def test_export_replacement_is_atomic_and_cleans_stale_output(tmp_path):
    export = tmp_path / "export"
    export.mkdir()
    target, staged = export / "role", export / ".role.build"
    target.mkdir()
    (target / "old").write_text("old data")
    staged.mkdir()
    (staged / "new").write_text("new data")
    P.publish_export(staged, target, export)
    assert (target / "new").read_text() == "new data"
    assert not (target / "old").exists() and not staged.exists()
    assert list(export.iterdir()) == [target]
    with pytest.raises(ValueError, match="inside"):
        P.publish_export(tmp_path / "outside", target, export)


def test_oos_plan_freezes_before_first_holdout_read(tmp_path):
    args = arguments(tmp_path)
    args.evaluate_frozen_oos = True
    steps = P.plan(args)
    names = [s["name"] for s in steps]
    assert names.index("gold") < names.index("freeze_gold") < names.index("holdout_labels") < names.index("holdout_ic")
    assert "--authorized-frozen-holdout" in steps[names.index("holdout_labels")]["command"]
    assert "--authorized-oos" in steps[names.index("holdout_ic")]["command"]
    assert "prices/manifest.json" in steps[names.index("characteristics")]["dependencies"]


def test_oos_refuses_truncated_gold_years(tmp_path):
    with pytest.raises(SystemExit):
        P.main(["--root", str(tmp_path), "--years", "2020-2023", "--evaluate-frozen-oos", "--dry-run"])
