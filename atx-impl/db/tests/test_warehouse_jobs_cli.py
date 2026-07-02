from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path


def _load_warehouse_jobs_module():
    script = Path(__file__).resolve().parents[2] / "scripts" / "warehouse_jobs.py"
    spec = importlib.util.spec_from_file_location("warehouse_jobs_cli", script)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_run_all_help_exposes_resume_and_full_rebuild_flags():
    script = Path(__file__).resolve().parents[2] / "scripts" / "warehouse_jobs.py"

    completed = subprocess.run(
        [sys.executable, str(script), "run-all", "--help"],
        capture_output=True,
        check=True,
        text=True,
    )

    assert "--full-rebuild" in completed.stdout
    assert "--resume" in completed.stdout


def test_run_all_json_preserves_results_payload_keys():
    from db.dataset import DatasetLoadResult
    from db.orchestrator import OrchestratorResult

    warehouse_jobs = _load_warehouse_jobs_module()
    payload = warehouse_jobs.run_all_json(
        OrchestratorResult(
            run_id="orchestrator-run-1",
            status="succeeded",
            dataset_order=("a",),
        ),
        [
            DatasetLoadResult(
                dataset_id="a",
                rows_loaded=3,
                source="orchestrator:succeeded",
                details={"status": "succeeded"},
                run_id="orchestrator-run-1",
            )
        ],
    )

    assert "results" in payload
    assert set(payload["results"][0]) == {
        "dataset_id",
        "rows_loaded",
        "source",
        "run_id",
        "details",
    }
    assert payload["run_id"] == "orchestrator-run-1"
    assert payload["status"] == "succeeded"
