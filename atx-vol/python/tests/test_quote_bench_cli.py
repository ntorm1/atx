"""Post-implementation native boundary checks; no market-data fixture required."""
from __future__ import annotations

import csv
import json
import math
import os
from pathlib import Path
import subprocess

import pytest


@pytest.fixture
def binary() -> Path:
    configured = os.environ.get("ATX_QUOTE_BENCH_BINARY")
    if not configured:
        pytest.skip("ATX_QUOTE_BENCH_BINARY is not configured")
    path = Path(configured)
    assert path.is_file()
    return path


def make_quotes(path: Path) -> list[float]:
    # Independent closed-form European values: with zero rate/yield the
    # American call has no early-exercise premium.
    values = []
    def phi(x: float) -> float:
        return 0.5 * math.erfc(-x / math.sqrt(2.0))

    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow([
            "row_id", "chain_id", "strike", "is_call", "spot", "bid", "ask",
            "years", "years_calendar", "rate", "sdiv", "ddiv_pv",
            "input_status", "exercise",
        ])
        for row, strike in enumerate(range(75, 126, 2)):
            sigma_t = 0.3 * math.sqrt(0.5)
            d1 = math.log(100.0 / strike) / sigma_t + 0.5 * sigma_t
            d2 = d1 - sigma_t
            price = 100.0 * phi(d1) - strike * phi(d2)
            values.append(price)
            writer.writerow([
                row, 17, strike, 1, 100, price - 0.001, price + 0.001,
                0.5, 0.5, 0, 0, 0, "ok", "American",
            ])
    return values


def invoke(binary: Path, source: Path, output: Path, *extra: str):
    return subprocess.run(
        [str(binary), "--input", str(source), "--output", str(output), *extra],
        capture_output=True, text=True, timeout=60, check=False,
    )


def test_native_fit_recovers_independent_flat_call_values(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    expected = make_quotes(source)
    result = invoke(binary, source, output)
    assert result.returncode == 0, result.stderr
    with output.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == len(expected)
    assert all(row["status"] == "ok" for row in rows)
    assert max(abs(float(row["predicted_vol"]) - 0.3) for row in rows) < 0.001
    assert max(abs(float(row["predicted_price"]) - value)
               for row, value in zip(rows, expected)) < 0.03
    first = output.read_bytes()
    assert invoke(binary, source, output).returncode == 0
    assert output.read_bytes() == first
    report = json.loads(result.stdout)
    assert report["served_rows"] == len(expected)
    assert report["fitted_chains"] == 1


def test_native_rejects_label_columns(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    lines = source.read_text().splitlines()
    source.write_text(lines[0] + ",srVol\n" +
                      "\n".join(line + ",0.3" for line in lines[1:]) + "\n")
    result = invoke(binary, source, output)
    assert result.returncode != 0
    assert "quote-only input schema" in result.stderr
    assert not output.exists()


def test_native_rejects_duplicate_ids_and_oversized_cohort(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    result = invoke(binary, source, output, "--max-rows", "2")
    assert result.returncode != 0
    assert "exceeds --max-rows" in result.stderr
    lines = source.read_text().splitlines()
    source.write_text("\n".join(lines + [lines[1]]) + "\n")
    result = invoke(binary, source, output)
    assert result.returncode != 0
    assert "Duplicate row_id" in result.stderr


def test_native_preserves_source_when_report_aliases_input(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    original = source.read_bytes()
    result = invoke(binary, source, output, "--report", str(source))
    assert result.returncode != 0
    assert source.read_bytes() == original


def test_native_preserves_hardlinked_source(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "alias.csv"
    make_quotes(source)
    original = source.read_bytes()
    os.link(source, output)
    result = invoke(binary, source, output)
    assert result.returncode != 0
    assert source.read_bytes() == original


def test_native_rejects_mixed_expiry_chain(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    with source.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    rows[-1]["years"] = "0.6"
    with source.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    result = invoke(binary, source, output)
    assert result.returncode == 0, result.stderr
    with output.open(newline="") as stream:
        assert all(row["status"] == "inconsistent_chain" for row in csv.DictReader(stream))


def test_native_counts_adjusted_deliverables_as_unsupported(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "predictions.csv"
    make_quotes(source)
    lines = source.read_text().splitlines()
    source.write_text(lines[0] + ",root\n" +
                      "\n".join(line + ",AAA1" for line in lines[1:]) + "\n")
    result = invoke(binary, source, output)
    assert result.returncode == 0, result.stderr
    with output.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert all(row["status"] == "unsupported_deliverable" for row in rows)
    assert all(row["predicted_price"] == "" for row in rows)
