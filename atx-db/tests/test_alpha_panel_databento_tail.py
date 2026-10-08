import duckdb
import pytest
import pyarrow as pa
import pyarrow.parquet as pq

from atx_db.alpha_panel import databento_tail as D


class FakeClient:
    def __init__(self, cost="0.269906669855", fail=False):
        self.cost, self.fail, self.downloads = cost, fail, 0

    def json(self, method, params):
        assert params.get("dataset") == "EQUS.SUMMARY"
        return {"metadata.get_dataset_range": {}, "metadata.get_record_count": 2,
                "metadata.get_billable_size": 112, "metadata.get_cost": self.cost}[method]

    def download(self, dest):
        self.downloads += 1
        if self.fail:
            raise RuntimeError("Simulated failed stream")
        dest.write_text("ts_event,rtype,publisher_id,instrument_id,open,high,low,close,volume\n"
                        "2026-09-14T00:00:00Z,32,1,38,100,102,99,101,1234\n"
                        "2026-09-15T00:00:00Z,32,1,38,102,104,101,103,5678\n")
        return {"http_status": 200}


@pytest.mark.parametrize("cost", ["0.30001", "NaN", "Infinity", "-1"])
def test_cap_rejects_before_paid_request(tmp_path, cost):
    client = FakeClient(cost)
    with pytest.raises(RuntimeError, match="cap"):
        D.acquire(tmp_path, client)
    assert client.downloads == 0
    assert not (tmp_path / "paid_request_attempt.json").exists()


def test_failed_stream_cannot_be_rebilled(tmp_path):
    client = FakeClient(fail=True)
    with pytest.raises(RuntimeError, match="failed stream"):
        D.acquire(tmp_path, client)
    with pytest.raises(RuntimeError, match="no automatic retry"):
        D.acquire(tmp_path, client)
    assert client.downloads == 1


def test_successful_raw_is_reused_and_tamper_fails(tmp_path):
    client = FakeClient()
    receipt = D.acquire(tmp_path, client)
    assert D.acquire(tmp_path, client) == receipt
    assert client.downloads == 1
    (tmp_path / "bars.csv").write_text("tampered")
    with pytest.raises(RuntimeError, match="mismatch"):
        D.acquire(tmp_path, client)


def test_dated_symbols_preserve_unadjusted_raw_bars(tmp_path):
    receipt = D.acquire(tmp_path / "bronze", FakeClient())
    mappings = [dict(instrument_id=38, start_date="2026-09-14", end_date="2026-09-15", provider_symbol="OLD"),
                dict(instrument_id=38, start_date="2026-09-15", end_date="2026-10-01", provider_symbol="NEW")]
    manifest = D.normalize(tmp_path / "bronze/bars.csv", mappings, tmp_path / "silver", receipt)
    con = duckdb.connect()
    result = con.execute("SELECT provider_symbol,close,adjustment_basis FROM read_parquet(?) ORDER BY session_date",
                         [manifest["output"]["path"]]).fetchall()
    con.close()
    assert result == [("OLD", 101., "unadjusted"), ("NEW", 103., "unadjusted")]
    assert manifest["quality"]["total_return_compatible"] is False
    assert manifest["rows"] == 2
    assert manifest["sessions"][0]["symbol_missing"] == 0


def test_ambiguous_symbol_intervals_rejected(tmp_path):
    receipt = D.acquire(tmp_path / "bronze", FakeClient())
    mappings = [dict(instrument_id=38, start_date="2026-09-14", end_date="2026-10-01", provider_symbol=s) for s in ["A", "B"]]
    with pytest.raises(RuntimeError, match="Ambiguous"):
        D.normalize(tmp_path / "bronze/bars.csv", mappings, tmp_path / "silver", receipt)


def test_overlap_excludes_repaired_and_ambiguous_symbols(tmp_path):
    receipt = D.acquire(tmp_path / "bronze", FakeClient())
    D.normalize(tmp_path / "bronze/bars.csv", [dict(instrument_id=38, start_date=D.START, end_date=D.END,
                                                  provider_symbol="AAPL")], tmp_path / "silver", receipt)
    prices = tmp_path / "prices"
    (prices / "year=2026").mkdir(parents=True)
    (prices / "manifest.json").write_text("{}")
    import datetime as dt
    pq.write_table(pa.table(dict(session_date=[dt.date(2026,9,14)]*2 + [dt.date(2026,9,15)],
        ticker=["AAPL"]*3, security_id=[1,2,1], close=[101.,101.,103.], volume=[1234.,1234.,5678.],
        sid0_repaired=[False,False,True])), prices / "year=2026/p.parquet")
    audit = D.overlap_audit(tmp_path / "silver", prices)
    assert audit["counts"]["orats_overlap_rows"] == 3
    assert audit["counts"]["matched_rows"] == 0
    assert audit["days"] == []
