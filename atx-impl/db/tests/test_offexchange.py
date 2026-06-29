from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


# Blueprint (datasets/off_exchange_transparency.md s6) deterministic AAPL fixture:
#   total 66,925,813 = ATS 21,246,912 (UBSA 12,345,678 + CROS 8,901,234)
#                    + non_ATS 45,678,901  -> ats_share_pct 31.7470%
def _weekly_aapl_finra_frame() -> pd.DataFrame:
    return pd.DataFrame(
        [
            {
                "issueSymbolIdentifier": "AAPL", "MPID": "UBSA",
                "summaryTypeCode": "ATS_W_SMBL", "totalWeeklyShareQuantity": "12345678",
                "totalWeeklyTradeCount": "1000", "weekStartDate": "2025-01-06",
                "lastUpdateDate": "2025-01-20", "tierDescription": "T1",
            },
            {
                "issueSymbolIdentifier": "AAPL", "MPID": "CROS",
                "summaryTypeCode": "ATS_W_SMBL", "totalWeeklyShareQuantity": "8901234",
                "totalWeeklyTradeCount": "800", "weekStartDate": "2025-01-06",
                "lastUpdateDate": "2025-01-20", "tierDescription": "T1",
            },
            {
                "issueSymbolIdentifier": "AAPL", "MPID": "FIRMX",
                "summaryTypeCode": "OTC_W_SMBL", "totalWeeklyShareQuantity": "45678901",
                "totalWeeklyTradeCount": "5000", "weekStartDate": "2025-01-06",
                "lastUpdateDate": "2025-01-20", "tierDescription": "T1",
            },
        ]
    )


def _write_csv(tmp_path, frame, name="weekly.csv"):
    path = tmp_path / name
    frame.to_csv(path, index=False)
    return path


def test_normalize_maps_finra_fields_and_derives_venue_class():
    from db.offexchange import normalize_offexchange_rows, FinraOffExchangeOptions

    out = normalize_offexchange_rows(
        _weekly_aapl_finra_frame(),
        options=FinraOffExchangeOptions(period_type="weekly"),
    )
    assert set(out["symbol"]) == {"AAPL"}
    assert sorted(out["venue_class"].unique()) == ["ATS", "non_ATS"]
    ats = out[out["mpid"] == "UBSA"].iloc[0]
    assert ats["venue_class"] == "ATS"
    assert ats["total_share_quantity"] == 12345678.0
    assert ats["period_type"] == "weekly"
    otc = out[out["mpid"] == "FIRMX"].iloc[0]
    assert otc["venue_class"] == "non_ATS"


def test_load_volume_and_upsert_venues(tmp_store, tmp_path):
    from db.offexchange import load_offexchange_volume, FinraOffExchangeOptions

    path = _write_csv(tmp_path, _weekly_aapl_finra_frame())
    n = load_offexchange_volume(tmp_store, FinraOffExchangeOptions(source_file=path, period_type="weekly"))
    assert n == 3

    venues = tmp_store.con.execute(
        "SELECT mpid, venue_class FROM offexchange_venue ORDER BY mpid"
    ).fetchall()
    assert venues == [("CROS", "ATS"), ("FIRMX", "non_ATS"), ("UBSA", "ATS")]


def test_materialize_security_period_ats_share(tmp_store, tmp_path):
    from db.offexchange import (
        load_offexchange_volume,
        refresh_offexchange_security_period,
        FinraOffExchangeOptions,
    )

    path = _write_csv(tmp_path, _weekly_aapl_finra_frame())
    load_offexchange_volume(tmp_store, FinraOffExchangeOptions(source_file=path, period_type="weekly"))
    rows = refresh_offexchange_security_period(tmp_store)
    assert rows == 1

    rec = tmp_store.con.execute(
        """
        SELECT ats_share_quantity, non_ats_share_quantity, total_share_quantity,
               ats_share_pct, ats_venue_count, restatement_detected
        FROM offexchange_security_period
        WHERE symbol = 'AAPL' AND period_type = 'weekly' AND summary_start_date = DATE '2025-01-06'
        """
    ).fetchone()
    assert rec[0] == 21246912.0
    assert rec[1] == 45678901.0
    assert rec[2] == 66925813.0
    assert rec[3] == pytest.approx(31.7470, abs=1e-4)
    assert rec[4] == 2          # two ATS venues
    assert rec[5] is False      # no restatement yet


def test_restatement_sets_is_latest_and_flag(tmp_store, tmp_path):
    from db.offexchange import (
        load_offexchange_volume,
        refresh_offexchange_security_period,
        FinraOffExchangeOptions,
    )

    base = _weekly_aapl_finra_frame()
    # First publication.
    p1 = _write_csv(tmp_path, base.assign(available_at="2025-01-20 12:00:00"), "v1.csv")
    load_offexchange_volume(tmp_store, FinraOffExchangeOptions(source_file=p1, period_type="weekly"))

    # FINRA restates UBSA upward in a later publication.
    restated = base.copy()
    restated.loc[restated["MPID"] == "UBSA", "totalWeeklyShareQuantity"] = "13000000"
    p2 = _write_csv(tmp_path, restated.assign(available_at="2025-02-01 12:00:00"), "v2.csv")
    load_offexchange_volume(
        tmp_store,
        FinraOffExchangeOptions(source_file=p2, period_type="weekly", replace_source_file=False),
    )

    latest_ubsa = tmp_store.con.execute(
        """
        SELECT total_share_quantity
        FROM offexchange_volume
        WHERE symbol='AAPL' AND mpid='UBSA' AND is_latest
        """
    ).fetchall()
    assert latest_ubsa == [(13000000.0,)]

    refresh_offexchange_security_period(tmp_store)
    rec = tmp_store.con.execute(
        """
        SELECT ats_share_quantity, restatement_detected
        FROM offexchange_security_period WHERE symbol='AAPL'
        """
    ).fetchone()
    assert rec[0] == 13000000.0 + 8901234.0
    assert rec[1] is True


def test_dataset_run_records_quality_and_asof_visibility(tmp_store, tmp_path):
    from db.offexchange import FinraOffExchangeDataset, FinraOffExchangeOptions
    from db.asof import offexchange_volume_asof

    path = _write_csv(
        tmp_path,
        _weekly_aapl_finra_frame().assign(available_at="2025-01-20 12:00:00"),
    )
    result = FinraOffExchangeDataset().run(
        tmp_store, FinraOffExchangeOptions(source_file=path, period_type="weekly")
    )
    assert result.rows_loaded == 3

    checks = tmp_store.con.execute(
        "SELECT count(*) FROM data_quality_checks WHERE dataset_id = 'offexchange_volume'"
    ).fetchone()[0]
    assert checks >= 1

    # Not visible before FINRA published it...
    before = offexchange_volume_asof(tmp_store, as_of_date=dt.date(2025, 1, 10))
    assert before.empty
    # ...visible after.
    after = offexchange_volume_asof(tmp_store, as_of_date=dt.date(2025, 2, 1))
    assert len(after) == 3
