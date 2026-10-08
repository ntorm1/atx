from __future__ import annotations

import duckdb

from atx_db.alpha_panel import source_audit as A


def test_source_coverage_uses_availability_year_and_reports_missing_clock(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    stage = tmp_path / "events"
    stage.mkdir()
    con = duckdb.connect()
    try:
        path = stage / "events.parquet"
        con.execute(f"""COPY (SELECT DATE '2019-12-31' AS period_end,
                           TIMESTAMP '2020-01-02 22:00:00' AS available_at)
                        TO '{path.as_posix()}' (FORMAT PARQUET)""")
        report = A.measure_source(con, "events", "events.parquet", "available_at")
        assert report["annual"] == [{"year": 2020, "rows": 1, "first_available": "2020-01-02 22:00:00",
                                     "last_available": "2020-01-02 22:00:00"}]
        assert A.measure_source(con, "events", "events.parquet", "unknown_clock")["status"] == "clock_column_missing"
        assert A.measure_source(con, "missing", "events.parquet", "available_at")["status"] == "missing"
    finally:
        con.close()
