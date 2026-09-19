"""Activation stages C: sharded reconciliation (injectable runner) and provider coverage."""

from __future__ import annotations

import datetime as dt
import subprocess
from pathlib import Path

import pytest

from atx_db.activation import ActivationOptions, stage_provider_coverage, stage_reconciliation


def _options(**overrides: object) -> ActivationOptions:
    base: dict[str, object] = dict(
        db_path=Path("unused.duckdb"),  # overridden by the tmp_store fixture's own path in these tests
        as_of_date=dt.date(2024, 1, 4),
        reconciliation_shards=4,
        memory_limit="512MB",
        threads=2,
        run_id="stages-c-test",
    )
    base.update(overrides)
    return ActivationOptions(**base)


def _completed(*, returncode: int = 0, stdout: str = "", stderr: str = "") -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess(args=["fake"], returncode=returncode, stdout=stdout, stderr=stderr)


# --- stage_reconciliation: injectable shard runner -------------------------------


def test_shard_runner_receives_the_expected_argv(tmp_store):
    captured: list[list[str]] = []

    def fake_runner(argv: list[str]) -> subprocess.CompletedProcess[str]:
        captured.append(argv)
        return _completed(returncode=0)

    options = _options(db_path=tmp_store.path, shard_runner=fake_runner)
    stage_reconciliation(tmp_store, options)

    assert len(captured) == 1
    argv = captured[0]
    assert "--db-path" in argv and argv[argv.index("--db-path") + 1] == str(tmp_store.path)
    assert "--shards" in argv and argv[argv.index("--shards") + 1] == "4"
    assert "--memory-limit" in argv and argv[argv.index("--memory-limit") + 1] == "512MB"
    assert "--threads" in argv and argv[argv.index("--threads") + 1] == "2"
    assert "--run-id-prefix" in argv and argv[argv.index("--run-id-prefix") + 1] == "stages-c-test-recon"
    assert argv[0] != "fake"  # argv[0] is sys.executable, not the fake's own args


def test_shard_runner_success_closes_and_reopens_the_store_and_parses_shard_lines(tmp_store):
    stdout = '{"step": "plan", "shard_count": 2}\n{"step": "shard", "shard": 1}\n{"step": "shard", "shard": 2}\n'

    def fake_runner(argv: list[str]) -> subprocess.CompletedProcess[str]:
        assert tmp_store.connection is None, "the store must be closed while the shard runner is 'running'"
        return _completed(returncode=0, stdout=stdout)

    options = _options(db_path=tmp_store.path, shard_runner=fake_runner)
    result = stage_reconciliation(tmp_store, options)

    assert tmp_store.connection is not None, "the store must be reopened after the shard runner returns"
    assert tmp_store.con.execute("SELECT 1").fetchone() == (1,)  # usable after reopen
    assert result.detail["shard_event_count"] == 3
    assert result.detail["last_shard_event"] == {"step": "shard", "shard": 2}


def test_shard_runner_nonzero_exit_raises_and_still_reopens_the_store(tmp_store):
    def fake_runner(argv: list[str]) -> subprocess.CompletedProcess[str]:
        return _completed(returncode=3, stderr="shard 2 crashed\n")

    options = _options(db_path=tmp_store.path, shard_runner=fake_runner)
    with pytest.raises(RuntimeError, match="exit code 3"):
        stage_reconciliation(tmp_store, options)
    assert tmp_store.connection is not None
    assert tmp_store.con.execute("SELECT 1").fetchone() == (1,)


def test_shard_runner_stderr_is_forwarded_to_our_stderr(tmp_store, capsys):
    def fake_runner(argv: list[str]) -> subprocess.CompletedProcess[str]:
        return _completed(returncode=0, stderr="shard 1 warning: slow join\n")

    options = _options(db_path=tmp_store.path, shard_runner=fake_runner)
    stage_reconciliation(tmp_store, options)
    captured = capsys.readouterr()
    assert "shard 1 warning: slow join" in captured.err


def test_shard_runner_raising_still_reopens_the_store_via_finally(tmp_store):
    def exploding_runner(argv: list[str]) -> subprocess.CompletedProcess[str]:
        raise OSError("failed to spawn interpreter")

    options = _options(db_path=tmp_store.path, shard_runner=exploding_runner)
    with pytest.raises(OSError, match="failed to spawn interpreter"):
        stage_reconciliation(tmp_store, options)
    assert tmp_store.connection is not None, "reopen() must run even when the runner itself raises"
    assert tmp_store.con.execute("SELECT 1").fetchone() == (1,)


def test_shard_runner_lines_that_are_not_json_are_skipped_not_raised(tmp_store):
    stdout = "not json\n{\"step\": \"shard\", \"shard\": 1}\n\n"

    def fake_runner(argv: list[str]) -> subprocess.CompletedProcess[str]:
        return _completed(returncode=0, stdout=stdout)

    options = _options(db_path=tmp_store.path, shard_runner=fake_runner)
    result = stage_reconciliation(tmp_store, options)
    assert result.detail["shard_event_count"] == 1
    assert result.detail["last_shard_event"] == {"step": "shard", "shard": 1}


# --- stage_provider_coverage: deterministic observed_at ---------------------------


def test_provider_coverage_uses_a_deterministic_observed_at_from_as_of_date(tmp_store):
    options = _options(db_path=tmp_store.path, as_of_date=dt.date(2024, 3, 15))
    stage_provider_coverage(tmp_store, options)
    rows = tmp_store.con.execute(
        "SELECT DISTINCT observed_at FROM api_schema_coverage_snapshot WHERE run_id = ?",
        [f"{options.run_id}-coverage"],
    ).fetchall()
    assert rows, "expected at least one provider coverage snapshot row"
    assert all(row[0] == dt.datetime(2024, 3, 15, 22, 0) for row in rows)
