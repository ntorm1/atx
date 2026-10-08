from __future__ import annotations

import json

import duckdb
import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import silver_refresh as S


@pytest.fixture()
def month_lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    con = duckdb.connect()
    con.execute("CREATE TABLE cal(session_date DATE)")
    con.execute("INSERT INTO cal VALUES ('2019-12-31'),('2020-01-02')")
    con.execute(f"COPY cal TO '{(tmp_path / 'calendar.parquet').as_posix()}' (FORMAT PARQUET)")
    prices = tmp_path / "prices" / "year=2020" / "prices.parquet"
    prices.parent.mkdir(parents=True)
    con.execute(f"COPY (SELECT DATE '2020-01-02' session_date,1::BIGINT security_id) TO '{prices.as_posix()}' (FORMAT PARQUET)")
    con.execute("""CREATE TABLE p AS SELECT DATE '2020-01-02' session_date,1::BIGINT security_id,
                   'dated'::VARCHAR link_tier,TIMESTAMP '2019-12-31 21:59:59' fund_clock,
                   true member_equity,100::BIGINT cik,1000.0 AS "at" """)
    path = tmp_path / "panel_goldready" / "year=2020" / "panel-01.parquet"
    path.parent.mkdir(parents=True)
    yield con, path
    con.close()


def publish(con, path):
    con.execute(f"COPY p TO '{path.as_posix()}' (FORMAT PARQUET)")


def test_validation_accepts_known_clock_and_exact_price_spine(month_lake):
    con, path = month_lake
    publish(con, path)
    report = S.validate_month(con, path, 2020, 1)
    assert report["rows"] == report["member_equity_linked_pit"] == 1
    assert report["non_pit_links"] == report["future_fundamental_clocks"] == 0


@pytest.mark.parametrize("assignment", ["link_tier='backfill'", "fund_clock=TIMESTAMP '2019-12-31 22:00:00'",
                                       "security_id=2"])
def test_validation_rejects_snapshot_links_boundary_clock_and_wrong_price_keys(month_lake, assignment):
    con, path = month_lake
    con.execute(f"UPDATE p SET {assignment}")
    publish(con, path)
    with pytest.raises(ValueError):
        S.validate_month(con, path, 2020, 1)


def test_resume_requires_same_sources_and_output_digest(tmp_path):
    path = tmp_path / "out.parquet"
    receipt = tmp_path / "receipt.json"
    path.write_bytes(b"original")
    receipt.write_text(json.dumps({"source_signature": "known", "sha256": C.sha256_file(path)}))
    assert S.reusable_month(path, receipt, "known")
    assert not S.reusable_month(path, receipt, "new-source")
    path.write_bytes(b"corrupt")
    assert not S.reusable_month(path, receipt, "known")
