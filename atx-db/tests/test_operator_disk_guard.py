from __future__ import annotations

import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest


@pytest.fixture(scope="module")
def guard():
    path = Path(__file__).resolve().parents[2] / ".superpowers/sdd/tier1-parity/run_memory_guarded.py"
    spec = importlib.util.spec_from_file_location("operator_disk_guard", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("free_delta, expected_failure", [(-1, "low_disk"), (0, None), (1, None)])
def test_disk_floor_uses_resolved_target_and_allows_exact_floor(guard, tmp_path, monkeypatch, free_delta, expected_failure):
    observed_paths = []
    free = 3 * guard.GIB + free_delta

    def usage(path):
        observed_paths.append(path)
        return SimpleNamespace(free=free)

    monkeypatch.setattr(guard.shutil, "disk_usage", usage)
    sample, failure = guard.sample_disk(tmp_path / ".", 3.0)
    assert observed_paths == [tmp_path.resolve()]
    assert failure == expected_failure
    assert sample == {"path": str(tmp_path.resolve()), "free_gb": free / guard.GIB, "min_free_gb": 3.0}


def test_disk_measurement_error_fails_closed_without_recording_exception(guard, tmp_path, monkeypatch):
    def denied(path):
        raise PermissionError("private operating-system details")

    monkeypatch.setattr(guard.shutil, "disk_usage", denied)
    sample, failure = guard.sample_disk(tmp_path, 3.0)
    assert failure == "disk_measurement_error"
    assert sample == {"path": str(tmp_path.resolve()), "free_gb": None, "min_free_gb": 3.0}


def test_missing_disk_target_fails_closed_before_measurement(guard, tmp_path, monkeypatch):
    def forbidden(path):
        pytest.fail("A missing target must not measure an unrelated ancestor volume")

    monkeypatch.setattr(guard.shutil, "disk_usage", forbidden)
    path = tmp_path / "missing"
    sample, failure = guard.sample_disk(path, 3.0)
    assert failure == "disk_measurement_error"
    assert sample == {"path": str(path), "free_gb": None, "min_free_gb": 3.0}


@pytest.mark.parametrize("disk_path, floor", [
    (Path("data"), None), (None, 3.0), (Path("data"), 0.0), (Path("data"), -1.0),
    (Path("data"), float("inf")), (Path("data"), float("-inf")), (Path("data"), float("nan")),
])
def test_disk_guard_rejects_partial_or_nonpositive_nonfinite_policy(guard, disk_path, floor):
    with pytest.raises(ValueError):
        guard.validate_disk_options(disk_path, floor)


@pytest.mark.parametrize("disk_path, floor", [(None, None), (Path("data"), 3.0)])
def test_disk_guard_is_opt_in_and_accepts_valid_policy(guard, disk_path, floor):
    assert guard.validate_disk_options(disk_path, floor) is None
