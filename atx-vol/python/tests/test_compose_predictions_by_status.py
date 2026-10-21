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


# --- cross-source shape certificate (--calendar-certificate off|demote|refuse) -------------------
# One underlier, three expiries (chains 1 / 2 / 3 = 10 / 20 / 30 days). row_id -> (chain_id,
# expiry_days, strike, is_call). Row 23 is the only put: it shares chain 2 with the call rows that
# break, so it pins the certificate's (chain_id, is_call) unit.
GEOMETRY = {11: (1, 10, 100.0, 1), 12: (1, 10, 110.0, 1),
            21: (2, 20, 100.0, 1), 22: (2, 20, 110.0, 1), 23: (2, 20, 100.0, 0),
            31: (3, 30, 100.0, 1), 32: (3, 30, 110.0, 1)}
CONVEX_GEOMETRY = {11: (1, 10, 100.0, 1), 12: (1, 10, 110.0, 1), 13: (1, 10, 120.0, 1)}


def write_dataset(path: Path, geometry: dict = GEOMETRY) -> Path:
    path.mkdir()
    ids = sorted(geometry)
    n = len(ids)
    pl.DataFrame({"row_id": ids, "chain_id": [geometry[i][0] for i in ids], "underlier": ["T"] * n,
                  "root": ["T"] * n, "snapshot_ns": [1] * n, "expiry_days": [geometry[i][1] for i in ids],
                  "strike": [geometry[i][2] for i in ids], "is_call": [geometry[i][3] for i in ids],
                  "bid": [1.0] * n, "ask": [2.0] * n},
                 schema={"row_id": pl.UInt64, "chain_id": pl.UInt64, "underlier": pl.String,
                         "root": pl.String, "snapshot_ns": pl.Int64, "expiry_days": pl.Int32,
                         "strike": pl.Float64, "is_call": pl.UInt8, "bid": pl.Float64,
                         "ask": pl.Float64}).write_parquet(path / "quotes.parquet")
    pl.DataFrame({"row_id": ids, "reference_price": [1.0] * n, "reference_vol": [0.2] * n},
                 schema={"row_id": pl.UInt64, "reference_price": pl.Float64,
                         "reference_vol": pl.Float64}).write_parquet(path / "labels.parquet")
    return path


def write_source(path: Path, served: dict[int, str], refusal: str, geometry: dict = GEOMETRY,
                 price_column: str = "predicted_price") -> Path:
    """Rows in ``served`` are status ok at the given price text; every other row is ``refusal``."""
    ids = sorted(geometry)
    pl.DataFrame({"row_id": [str(i) for i in ids], price_column: [served.get(i) for i in ids],
                  "predicted_vol": ["0.2" if i in served else None for i in ids],
                  "status": ["ok" if i in served else refusal for i in ids],
                  "chain_id": [str(geometry[i][0]) for i in ids]}).write_csv(path, null_value="")
    return path


@pytest.fixture()
def calendar_case(tmp_path: Path) -> tuple[Path, list[Path]]:
    """a serves expiries 1 and 3; b serves expiry 2 with a calendar crossing at strike 100 (4.0 below
    a's 5.0 at the earlier expiry); c serves expiry 2 without one. The put row 23 is clean."""
    dataset = write_dataset(tmp_path / "cohort")
    a = write_source(tmp_path / "a.csv", {11: "5.0", 12: "2.0", 31: "7.0", 32: "4.0"}, "refused_a")
    b = write_source(tmp_path / "b.csv", {21: "4.0", 22: "3.0", 23: "1.0"}, "refused_b")
    c = write_source(tmp_path / "c.csv", {21: "6.0", 22: "3.0", 23: "1.5"}, "refused_c")
    return dataset, [a, b, c]


def run_certified(tmp_path: Path, inputs: list[Path], name: str, mode: str, dataset: Path | None,
                  labels: tuple[str, ...] = ("a", "b", "c")) -> tuple[int, Path, dict | None]:
    out = tmp_path / f"{name}.csv"
    argv = ["--inputs", *map(str, inputs), "--labels", *labels[:len(inputs)], "--output", str(out),
            "--calendar-certificate", mode]
    if dataset is not None:
        argv += ["--dataset", str(dataset)]
    code = cp.main(argv)
    receipt_path = Path(str(out) + ".run.json")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8")) if receipt_path.exists() else None
    return code, out, receipt


def test_certificate_off_is_byte_identical_to_the_plain_composition(tmp_path, calendar_case):
    dataset, inputs = calendar_case
    plain = tmp_path / "plain.csv"
    assert cp.main(["--inputs", *map(str, inputs), "--labels", "a", "b", "c", "--output", str(plain)]) == 0
    code, out, receipt = run_certified(tmp_path, inputs, "off", "off", None)
    assert code == 0
    assert out.read_bytes() == plain.read_bytes()
    got = pl.read_csv(out, infer_schema_length=0)
    assert got["source_model"].to_list() == ["a", "a", "b", "b", "b", "a", "a"]
    assert got["predicted_price"].to_list() == ["5.0", "2.0", "4.0", "3.0", "1.0", "7.0", "4.0"]
    assert "calendar_certificate" not in receipt
    assert receipt["notes"][0] == "rows from different models are not jointly arbitrage-checked"
    assert receipt["selection_inputs"] == "status column only; labels, references and errors are never read"


def test_refuse_drops_the_lower_priority_expiry_side_and_leaves_no_break(tmp_path, calendar_case):
    dataset, inputs = calendar_case
    code, out, receipt = run_certified(tmp_path, inputs, "refuse", "refuse", dataset)
    assert code == 0
    got = pl.read_csv(out, infer_schema_length=0)
    # Chain 2's CALL side came from b and crossed a's earlier expiry: refused, b's label kept.
    # Chain 2's PUT side (row 23, also b) is clean and stays served.
    assert got["status"].to_list() == ["ok", "ok", "calendar_arbitrage", "calendar_arbitrage", "ok", "ok", "ok"]
    assert got["predicted_price"].to_list() == ["5.0", "2.0", None, None, "1.0", "7.0", "4.0"]
    assert got["predicted_vol"].to_list() == ["0.2", "0.2", None, None, "0.2", "0.2", "0.2"]
    assert got["source_model"].to_list() == ["a", "a", "b", "b", "b", "a", "a"]
    assert got["source_index"].to_list() == ["0", "0", "1", "1", "1", "0", "0"]
    cert = receipt["calendar_certificate"]
    assert cert["mode"] == "refuse"
    assert cert["breaks_found"] == 1 and cert["breaks_found_by_kind"] == {"monotone": 0, "convex": 0, "calendar": 1}
    assert cert["rows_refused"] == 2 and cert["rows_demoted"] == 0
    assert cert["rows_refused_by_source_pair"] == {"a+b": 2}
    assert cert["rows_refused_by_status"] == {"calendar_arbitrage": 2}
    assert cert["residual_breaks"] == 0 and cert["residual_cross_source_breaks"] == 0
    assert cert["served_rows_before"] == 7 and cert["served_rows_after"] == 5
    assert [p["action"] for p in cert["passes"]] == ["refuse"]
    # The routing counts describe the status-only routing; the certificate reports its own numbers.
    assert receipt["counts"]["served_rows"] == 7
    assert "calendar certificate refuse" in receipt["notes"][0]
    assert receipt["selection_inputs"].endswith("never labels")


def test_demote_reroutes_the_expiry_side_to_the_next_serving_input(tmp_path, calendar_case):
    dataset, inputs = calendar_case
    code, out, receipt = run_certified(tmp_path, inputs, "demote", "demote", dataset)
    assert code == 0
    got = pl.read_csv(out, infer_schema_length=0)
    assert got["status"].to_list() == ["ok"] * 7
    assert got["source_model"].to_list() == ["a", "a", "c", "c", "b", "a", "a"]
    assert got["predicted_price"].to_list() == ["5.0", "2.0", "6.0", "3.0", "1.0", "7.0", "4.0"]
    cert = receipt["calendar_certificate"]
    assert cert["rows_demoted"] == 2 and cert["rows_refused"] == 0
    assert cert["rows_demoted_by_source_pair"] == {"a+b": 2}
    assert cert["residual_breaks"] == 0 and cert["served_rows_after"] == 7
    assert [p["action"] for p in cert["passes"]] == ["demote"]


def test_demote_without_a_later_serving_input_equals_refuse(tmp_path, calendar_case):
    dataset, inputs = calendar_case
    two = inputs[:2]
    assert run_certified(tmp_path, two, "demote2", "demote", dataset, ("a", "b"))[0] == 0
    assert run_certified(tmp_path, two, "refuse2", "refuse", dataset, ("a", "b"))[0] == 0
    assert (tmp_path / "demote2.csv").read_bytes() == (tmp_path / "refuse2.csv").read_bytes()
    receipt = json.loads((tmp_path / "demote2.csv.run.json").read_text(encoding="utf-8"))
    assert receipt["calendar_certificate"]["rows_refused"] == 2
    assert receipt["calendar_certificate"]["rows_demoted"] == 0


def test_single_source_breaks_are_not_the_certificates_business(tmp_path):
    # a crosses itself at strike 110 (3.0 at 10 d above 2.5 at 30 d); b's chain-2 node sits cleanly
    # between a's strike-100 prices. Nothing is touched; the receipt reports the single-source break.
    dataset = write_dataset(tmp_path / "cohort")
    a = write_source(tmp_path / "a.csv", {11: "5.0", 12: "3.0", 31: "7.0", 32: "2.5"}, "refused_a")
    b = write_source(tmp_path / "b.csv", {21: "6.0"}, "refused_b")
    plain = tmp_path / "plain.csv"
    assert cp.main(["--inputs", str(a), str(b), "--labels", "a", "b", "--output", str(plain)]) == 0
    code, out, receipt = run_certified(tmp_path, [a, b], "refuse", "refuse", dataset, ("a", "b"))
    assert code == 0
    assert out.read_bytes() == plain.read_bytes()
    cert = receipt["calendar_certificate"]
    assert cert["breaks_found"] == 1 and cert["single_source_breaks_found"] == 1
    assert cert["cross_source_breaks_found"] == 0
    assert cert["breaks_found_by_source_pair"] == {"a": {"monotone": 0, "convex": 0, "calendar": 1}}
    assert cert["rows_refused"] == 0 and cert["rows_demoted"] == 0
    assert cert["residual_breaks"] == 1 and cert["residual_single_source_breaks"] == 1
    assert cert["passes"] == []


def test_convex_and_monotone_breaks_across_sources_are_certified_too(tmp_path):
    # b's strike-110 price (9.0) sits above a's chord (6.0) in one chain: a cross-source convex break.
    dataset = write_dataset(tmp_path / "cohort", CONVEX_GEOMETRY)
    a = write_source(tmp_path / "a.csv", {11: "10.0", 13: "2.0"}, "refused_a", CONVEX_GEOMETRY)
    b = write_source(tmp_path / "b.csv", {12: "9.0"}, "refused_b", CONVEX_GEOMETRY)
    c = write_source(tmp_path / "c.csv", {12: "5.0"}, "refused_c", CONVEX_GEOMETRY)
    code, out, receipt = run_certified(tmp_path, [a, b, c], "refuse", "refuse", dataset)
    assert code == 0
    got = pl.read_csv(out, infer_schema_length=0)
    assert got["status"].to_list() == ["ok", "arbitrage_failure", "ok"]
    assert got["predicted_price"].to_list() == ["10.0", None, "2.0"]
    cert = receipt["calendar_certificate"]
    assert cert["breaks_found_by_kind"] == {"monotone": 0, "convex": 1, "calendar": 0}
    assert cert["convex_triples_checked"] == 1 and cert["pairs_checked"] == 0
    assert cert["rows_refused_by_status"] == {"arbitrage_failure": 1}
    assert cert["residual_breaks"] == 0
    code, out, receipt = run_certified(tmp_path, [a, b, c], "demote", "demote", dataset)
    assert code == 0
    got = pl.read_csv(out, infer_schema_length=0)
    assert got["predicted_price"].to_list() == ["10.0", "5.0", "2.0"]
    assert got["source_model"].to_list() == ["a", "c", "a"]
    assert receipt["calendar_certificate"]["rows_demoted"] == 1


def test_labels_moved_away_leave_the_certified_output_identical(tmp_path, calendar_case):
    dataset, inputs = calendar_case
    code, with_labels, first = run_certified(tmp_path, inputs, "with-labels", "refuse", dataset)
    assert code == 0
    (dataset / "labels.parquet").rename(tmp_path / "labels-moved.parquet")
    code, without, second = run_certified(tmp_path, inputs, "without-labels", "refuse", dataset)
    assert code == 0
    assert with_labels.read_bytes() == without.read_bytes()
    assert first["calendar_certificate"] == second["calendar_certificate"]
    assert first["counts"] == second["counts"]


def test_certificate_receipt_names_the_dataset_and_every_count(tmp_path, calendar_case):
    dataset, inputs = calendar_case
    code, out, receipt = run_certified(tmp_path, inputs, "refuse", "refuse", dataset)
    assert code == 0
    cert = receipt["calendar_certificate"]
    for key in ("mode", "pairs_checked", "cross_source_pairs", "breaks_found", "rows_demoted",
                "rows_refused", "residual_breaks"):
        assert key in cert, key
    # Calendar pairs at the call strikes 100 and 110: (1,2) (1,3) (2,3) each -> 6; the put row is alone.
    # Cross-source pairs are the four that pair an a-node with the b-node.
    assert cert["pairs_checked"] == 6 and cert["cross_source_pairs"] == 4
    assert cert["monotone_pairs_checked"] == 3 and cert["convex_triples_checked"] == 0
    assert cert["duplicate_strike_disagreements"] == 0
    assert cert["tolerance"] == cp.SHAPE_TOLERANCE == 1.0e-6
    assert cert["quotes"] == str((dataset / "quotes.parquet").resolve())
    assert cert["quotes_sha256"] == cp.sha256(dataset / "quotes.parquet")
    assert cert["refusal_status_by_kind"] == cp.REFUSAL_STATUS


def test_certificate_arguments_and_geometry_are_validated(tmp_path, calendar_case):
    dataset, inputs = calendar_case
    assert run_certified(tmp_path, inputs, "no-dataset", "refuse", None)[0] == 2
    assert run_certified(tmp_path, inputs, "off-with-dataset", "off", dataset)[0] == 2
    assert not (tmp_path / "no-dataset.csv").exists() and not (tmp_path / "off-with-dataset.csv").exists()
    # Every input row needs a quote row with the same chain id.
    short = write_dataset(tmp_path / "short", {k: v for k, v in GEOMETRY.items() if k != 32})
    assert run_certified(tmp_path, inputs, "short", "refuse", short)[0] == 2
    moved = write_dataset(tmp_path / "moved", {**GEOMETRY, 32: (9, 30, 110.0, 1)})
    assert run_certified(tmp_path, inputs, "moved", "refuse", moved)[0] == 2
    # The certificate needs predicted_price in every input.
    nameless = write_source(tmp_path / "nameless.csv", {21: "6.0"}, "refused_n", price_column="price")
    assert run_certified(tmp_path, [inputs[0], nameless], "nameless", "refuse", dataset, ("a", "n"))[0] == 2
