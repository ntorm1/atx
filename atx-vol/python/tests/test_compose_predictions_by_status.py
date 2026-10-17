"""Status-routed composition: first-served routing and the opt-in --restrict row lists."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path

import polars as pl
import pytest


TOOLS = Path(__file__).resolve().parents[2] / "tools"
SPEC = importlib.util.spec_from_file_location("compose_predictions_by_status",
                                              TOOLS / "compose_predictions_by_status.py")
cp = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cp)

ROWS = ["11", "12", "13", "14", "15"]
CHAINS = ["7", "7", "7", "8", "8"]


def write_input(path: Path, statuses: list[str], prices: list[str], extra: dict | None = None) -> Path:
    frame = pl.DataFrame({"row_id": ROWS, "predicted_price": prices, "predicted_vol": ["0.2"] * 5,
                          "status": statuses, "chain_id": CHAINS, **(extra or {})})
    frame.write_csv(path, null_value="")
    return path


@pytest.fixture()
def inputs(tmp_path: Path) -> list[Path]:
    a = write_input(tmp_path / "a.csv", ["ok", "ok", "refused_a", "refused_a", "ok"],
                    ["1.0", "1.1", None, None, "1.4"])
    b = write_input(tmp_path / "b.csv", ["refused_b", "ok", "ok", "refused_b", "ok"],
                    [None, "2.1", "2.2", None, "2.4"], {"grade": [None, "A", "B", None, "A"]})
    c = write_input(tmp_path / "c.csv", ["ok", "ok", "ok", "refused_c", "ok"],
                    ["3.0", "3.1", "3.2", None, "3.4"])
    return [a, b, c]


def run(tmp_path: Path, inputs: list[Path], name: str, *extra: str) -> tuple[int, Path]:
    out = tmp_path / f"{name}.csv"
    code = cp.main(["--inputs", *map(str, inputs), "--labels", "a", "b", "c", "--output", str(out), *extra])
    return code, out


def write_rows(path: Path, rows: list[str]) -> Path:
    pl.DataFrame({"row_id": rows}, schema={"row_id": pl.String}).write_csv(path)
    return path


def test_first_served_input_wins_and_unserved_rows_copy_input_zero(tmp_path, inputs):
    code, out = run(tmp_path, inputs, "plain")
    assert code == 0
    got = pl.read_csv(out, infer_schema_length=0)
    assert got["source_model"].to_list() == ["a", "a", "b", "a", "a"]
    assert got["status"].to_list() == ["ok", "ok", "ok", "refused_a", "ok"]
    assert got["predicted_price"].to_list() == ["1.0", "1.1", "2.2", None, "1.4"]
    assert got.columns[-2:] == ["source_model", "source_index"]
    receipt = json.loads(Path(str(out) + ".run.json").read_text(encoding="utf-8"))
    assert "restrictions" not in receipt
    assert receipt["counts"]["fallback_rows_from_input_0"] == 1
    assert "fallback_rows" not in receipt["counts"]
    assert all("rows_allowed" not in s for s in receipt["counts"]["per_source"])


def test_restricted_input_serves_only_listed_rows(tmp_path, inputs):
    rows = write_rows(tmp_path / "payers.csv", ["11", "13", "14"])
    code, out = run(tmp_path, inputs, "routed", "--restrict", f"0={rows}")
    assert code == 0
    got = pl.read_csv(out, infer_schema_length=0)
    # 12 and 15 are served by input 0 but not listed: they fall through to b.
    # 14 is served by nobody and listed for input 0: its refusal row comes from a.
    assert got["source_model"].to_list() == ["a", "b", "b", "a", "b"]
    assert got["predicted_price"].to_list() == ["1.0", "2.1", "2.2", None, "2.4"]
    assert got["status"].to_list()[3] == "refused_a"
    receipt = json.loads(Path(str(out) + ".run.json").read_text(encoding="utf-8"))
    (entry,) = receipt["restrictions"]
    assert entry["index"] == 0 and entry["rows_allowed"] == 3 and entry["sha256"] == cp.sha256(rows)
    first = receipt["counts"]["per_source"][0]
    assert first["served_rows_in_input"] == 3 and first["served_rows_allowed"] == 1
    assert first["rows_selected_as_served"] == 1 and first["fallback_rows_from_input"] == 1
    assert "allowed" in receipt["selection_rule"]


def test_unlisted_unserved_row_falls_back_to_first_allowed_input(tmp_path, inputs):
    rows = write_rows(tmp_path / "payers.csv", ["11"])
    code, out = run(tmp_path, inputs, "routed", "--restrict", f"0={rows}")
    assert code == 0
    got = pl.read_csv(out, infer_schema_length=0)
    # Row 14 is refused everywhere and input 0 may not serve it: input b's refusal is copied.
    assert got.row(3, named=True)["status"] == "refused_b"
    assert got.row(3, named=True)["source_model"] == "b"
    receipt = json.loads(Path(str(out) + ".run.json").read_text(encoding="utf-8"))
    assert receipt["counts"]["fallback_rows_from_input_0"] == 0
    assert receipt["counts"]["fallback_rows"] == 1
    assert receipt["counts"]["fallback_status_counts"] == [{"status": "refused_b", "rows": 1}]


def test_restricting_to_every_row_reproduces_the_unrouted_bytes(tmp_path, inputs):
    rows = write_rows(tmp_path / "all.csv", list(reversed(ROWS)))
    assert run(tmp_path, inputs, "plain")[0] == 0
    assert run(tmp_path, inputs, "routed", "--restrict", f"0={rows}", "--restrict", f"2={rows}")[0] == 0
    assert (tmp_path / "plain.csv").read_bytes() == (tmp_path / "routed.csv").read_bytes()


@pytest.mark.parametrize("spec_rows, spec", [
    (["11", "99"], "0={rows}"),        # unknown row id
    (["11", "11"], "0={rows}"),        # duplicate row id
    (["11"], "3={rows}"),              # index out of range
    (["11"], "x={rows}"),              # malformed index
    (["11"], "0"),                     # missing path
])
def test_invalid_restrictions_are_refused(tmp_path, inputs, spec_rows, spec):
    rows = write_rows(tmp_path / "rows.csv", spec_rows)
    code, out = run(tmp_path, inputs, "bad", "--restrict", spec.format(rows=rows))
    assert code == 2
    assert not out.exists()


def test_a_restriction_needs_a_row_id_column_and_one_index_once(tmp_path, inputs):
    other = tmp_path / "other.csv"
    pl.DataFrame({"id": ["11"]}).write_csv(other)
    assert run(tmp_path, inputs, "bad1", "--restrict", f"0={other}")[0] == 2
    rows = write_rows(tmp_path / "rows.csv", ["11"])
    assert run(tmp_path, inputs, "bad2", "--restrict", f"1={rows}", "--restrict", f"1={rows}")[0] == 2


def test_rows_allowed_by_no_input_are_refused(tmp_path, inputs):
    rows = write_rows(tmp_path / "rows.csv", ["11"])
    specs = [x for i in range(3) for x in ("--restrict", f"{i}={rows}")]
    code, out = run(tmp_path, inputs, "orphans", *specs)
    assert code == 2
    assert not out.exists()
