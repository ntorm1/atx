"""Label-free universe builder (tools/build_universe_dataset.py): label isolation, adapter parity, filters."""
from __future__ import annotations

import datetime as dt
import importlib.util
import json
from pathlib import Path

import polars as pl
import pyarrow as pa
import pyarrow.parquet as pq
import pytest


TOOLS = Path(__file__).resolve().parents[2] / "tools"


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


qb = _load("quote_benchmark")
bud = _load("build_universe_dataset")
UTC = dt.timezone.utc
T1030 = dt.datetime(2026, 10, 14, 14, 30, tzinfo=UTC)   # 10:30 New York
T1500 = dt.datetime(2026, 10, 14, 19, 0, tzinfo=UTC)    # 15:00 New York


def source_table(*, labels: bool = True, label_value: float = 1.1) -> pa.Table:
    """Eight raw rows: six at 10:30 (AAA x2 ok, BBB x2 ok, CCC crossed + CCC European), two at 15:00."""
    unds = ["AAA", "AAA", "BBB", "BBB", "CCC", "CCC", "AAA", "BBB"]
    dates = [T1030] * 6 + [T1500] * 2
    n = len(unds)
    table = pa.table({
        "okey_tk": unds, "okey_dt": [dt.date(2027, 1, 15)] * n,
        "okey_xx": [90., 100., 95., 105., 10., 12., 100., 100.],
        "okey_cp": ["Call", "Put", "Call", "Put", "Call", "Put", "Call", "Put"],
        "date": dates, "timestamp": [d + dt.timedelta(seconds=1) for d in dates],
        "undSecKey_tk": unds,
        "uBid": [99.99] * 4 + [9.99] * 2 + [99.99] * 2, "uAsk": [100.01] * 4 + [10.01] * 2 + [100.01] * 2,
        "bidPrc": [1., 1., 1., 0., .7, 1.3, 1., 1.], "askPrc": [1.2, 1.2, 1.2, 1.1, .6, 1.4, 1.2, 1.2],
        "bidSz": [10] * n, "askSz": [10] * n, "years": [.3] * n, "yearsC": [.25] * n,
        "rate": [.04] * n, "sdiv": [.01] * n, "ddiv": [0.] * 6 + [.5, .5], "ddivPv": [0.] * 6 + [.49, .49],
        "exType": ["American"] * 5 + ["European"] + ["American"] * 2,
        "bidIV": [998.] * n, "synSpot": [9999.] * n,   # vendor fields the allowlist excludes
    })
    if labels:
        table = (table.append_column("srPrc", pa.array([label_value] * n))
                 .append_column("srVol", pa.array([.3] * n)))
    return table


def build(tmp_path: Path, name: str, table: pa.Table, *extra: str, buckets: str = "1030") -> dict:
    source = tmp_path / f"{name}.parquet"
    pq.write_table(table, source, row_group_size=3)
    args = bud.parser().parse_args(["--source", str(source), "--output", str(tmp_path / name),
                                    "--buckets", buckets, "--batch-size", "2", *extra])
    return bud.build(args)


def test_build_is_label_free_and_run_compatible(tmp_path):
    out = tmp_path / "universe"
    manifest = build(tmp_path, "universe", source_table())
    assert not (out / "labels.parquet").exists()
    assert pq.read_schema(out / "quotes.parquet") == qb.QUOTE_SCHEMA
    assert set(pq.read_schema(out / "quotes.parquet").names).isdisjoint(bud.LABEL_BLOCKLIST)
    assert set(manifest["files"]) == {"quotes.parquet", "quotes.csv"}
    assert manifest["labels_parquet"] is None and manifest["label_columns_read"] == []
    assert manifest["source"]["columns_read"] == list(qb.INPUT_COLUMNS)
    assert manifest["rows_by_input_status"] == {"invalid_option_quote": 1, "ok": 4,
                                                "unsupported_exercise": 1}
    assert manifest["selection"]["selected_underliers"] == ["AAA", "BBB", "CCC"]
    assert manifest["selection"]["chains"] == 3 and manifest["rows_selected"] == 6
    assert manifest["snapshot"]["first"]["new_york"].endswith("T10:30:00-04:00")
    assert manifest["snapshot"]["last"] == manifest["snapshot"]["first"]
    quotes = pl.read_parquet(out / "quotes.parquet")
    assert quotes["split"].unique().to_list() == ["universe"]
    assert quotes["row_id"].to_list() == [0, 1, 2, 3, 4, 5]   # the adapter's source row index
    written = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    assert written["files"] == manifest["files"]
    # quote_benchmark.py run / shape_cohort.py build accept the folder as a dataset.
    assert qb.verify_dataset(out)["rows_selected"] == 6
    assert "quotes.csv" in qb.verify_dataset(out)["files"]


def test_labels_are_neither_required_nor_read(tmp_path, monkeypatch):
    requested: list[list[str] | None] = []
    original = pq.ParquetFile.iter_batches

    def recording(self, *args, **kwargs):
        requested.append(list(kwargs.get("columns") or []))
        return original(self, *args, **kwargs)

    monkeypatch.setattr(pq.ParquetFile, "iter_batches", recording)
    with_labels = build(tmp_path, "with", source_table())
    without = build(tmp_path, "without", source_table(labels=False))
    poisoned = build(tmp_path, "poison", source_table(label_value=777.0))
    assert requested and all(cols == list(qb.INPUT_COLUMNS) for cols in requested)
    for name in ("quotes.parquet", "quotes.csv"):
        assert with_labels["files"][name]["sha256"] == without["files"][name]["sha256"]
        assert with_labels["files"][name]["sha256"] == poisoned["files"][name]["sha256"]


def test_rows_match_the_prepare_adapter_except_split(tmp_path):
    table = source_table()
    universe = build(tmp_path, "u", table)
    source = tmp_path / "p.parquet"
    pq.write_table(table, source, row_group_size=3)
    args = qb.parser().parse_args(["prepare", "--source", str(source), "--output", str(tmp_path / "p"),
                                  "--split", "all", "--buckets", "1030", "--csv", "--batch-size", "2"])
    prepared = qb.prepare(args)
    assert prepared["rows_selected"] == universe["rows_selected"] == 6
    ours = pl.read_parquet(tmp_path / "u" / "quotes.parquet")
    theirs = pl.read_parquet(tmp_path / "p" / "quotes.parquet")
    assert ours.drop("split").equals(theirs.drop("split"))
    assert set(theirs["split"].unique().to_list()) <= {"development", "validation"}
    assert ours["split"].unique().to_list() == ["universe"]
    assert (tmp_path / "p" / "labels.parquet").exists() and not (tmp_path / "u" / "labels.parquet").exists()


def test_symbols_file_tags_and_refusals(tmp_path):
    roster = tmp_path / "roster.txt"
    roster.write_text("# drawn sample\nBBB\n\n", encoding="utf-8")
    manifest = build(tmp_path, "sample", source_table(), "--symbols-file", str(roster),
                     "--tag", "draw_seed=L5u-1pct-v1", "--tag", "note=x=y")
    assert manifest["selection"]["selected_underliers"] == ["BBB"]
    assert manifest["selection"]["symbols_file"] == {"path": str(roster.resolve()),
                                                     "sha256": qb.sha256(roster), "underliers": 1}
    assert manifest["tags"] == {"draw_seed": "L5u-1pct-v1", "note": "x=y"}
    assert manifest["rows_selected"] == 2
    with pytest.raises(FileExistsError):
        build(tmp_path, "sample", source_table())
    with pytest.raises(ValueError, match="source missing input columns"):
        build(tmp_path, "nocol", source_table().drop_columns(["rate"]))
    with pytest.raises(ValueError, match="KEY=VALUE"):
        build(tmp_path, "badtag", source_table(), "--tag", "novalue")
    with pytest.raises(ValueError, match="at least one"):
        build(tmp_path, "nobucket", source_table(), buckets=",")
    with pytest.raises(ValueError, match="selection is empty"):
        build(tmp_path, "empty", source_table(), buckets="0930")
    both = build(tmp_path, "both", source_table(), buckets="1030,1500")
    assert both["rows_selected"] == 8 and both["snapshot"]["last"]["new_york"].endswith("T15:00:00-04:00")
