from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


def _weekly_aapl_finra_frame() -> pd.DataFrame:
    return pd.DataFrame(
        [
            {
                "issueSymbolIdentifier": "AAPL",
                "MPID": "UBSA",
                "summaryTypeCode": "ATS_W_SMBL",
                "totalWeeklyShareQuantity": "12345678",
                "totalWeeklyTradeCount": "1000",
                "weekStartDate": "2025-01-06",
                "lastUpdateDate": "2025-01-20",
                "tierDescription": "T1",
                "available_at": "2025-01-20 12:00:00",
            },
            {
                "issueSymbolIdentifier": "AAPL",
                "MPID": "CROS",
                "summaryTypeCode": "ATS_W_SMBL",
                "totalWeeklyShareQuantity": "8901234",
                "totalWeeklyTradeCount": "800",
                "weekStartDate": "2025-01-06",
                "lastUpdateDate": "2025-01-20",
                "tierDescription": "T1",
                "available_at": "2025-01-20 12:00:00",
            },
            {
                "issueSymbolIdentifier": "AAPL",
                "MPID": "FIRMX",
                "summaryTypeCode": "OTC_W_SMBL",
                "totalWeeklyShareQuantity": "45678901",
                "totalWeeklyTradeCount": "5000",
                "weekStartDate": "2025-01-06",
                "lastUpdateDate": "2025-01-20",
                "tierDescription": "T1",
                "available_at": "2025-01-20 12:00:00",
            },
        ]
    )


def _daily_short_volume_frame() -> pd.DataFrame:
    return pd.DataFrame(
        [
            {
                "Date": "2026-01-02",
                "Symbol": "AAPL",
                "ShortVolume": "100",
                "ShortExemptVolume": "5",
                "TotalVolume": "200",
                "Market": "N",
                "available_at": "2026-01-03 22:00:00",
            },
            {
                "Date": "2026-01-02",
                "Symbol": "AAPL",
                "ShortVolume": "300",
                "ShortExemptVolume": "15",
                "TotalVolume": "600",
                "Market": "Q",
                "available_at": "2026-01-03 22:00:00",
            },
            {
                "Date": "2026-01-02",
                "Symbol": "MSFT",
                "ShortVolume": "100",
                "ShortExemptVolume": "0",
                "TotalVolume": "1000",
                "Market": "Q",
                "available_at": "2026-01-03 22:00:00",
            },
        ]
    )


def _write_csv(tmp_path, frame: pd.DataFrame, name: str = "weekly.csv"):
    path = tmp_path / name
    frame.to_csv(path, index=False)
    return path


def _write_pipe(tmp_path, frame: pd.DataFrame, name: str = "CNMSshvol20260102.txt"):
    path = tmp_path / name
    frame.to_csv(path, index=False, sep="|")
    return path


def test_refresh_offexchange_quality_report_summarizes_public_flow_surfaces(tmp_store, tmp_path):
    from atx_db.asof import offexchange_quality_report_asof
    from atx_db.offexchange import FinraOffExchangeOptions, load_offexchange_volume, refresh_offexchange_security_period
    from atx_db.offexchange_quality import OffExchangeQualityReportOptions, refresh_offexchange_quality_report
    from atx_db.short_volume import FinraShortVolumeDataset, FinraShortVolumeOptions, ShortVolumeMetricsDataset

    off_path = _write_csv(tmp_path, _weekly_aapl_finra_frame(), "weekly.csv")
    load_offexchange_volume(
        tmp_store,
        FinraOffExchangeOptions(source_file=off_path, source="offx-fixture", period_type="weekly"),
    )
    refresh_offexchange_security_period(tmp_store)

    short_path = _write_pipe(tmp_path, _daily_short_volume_frame())
    short_options = FinraShortVolumeOptions(source_file=short_path, source="short-fixture")
    assert FinraShortVolumeDataset().run(tmp_store, short_options).rows_loaded == 3
    assert ShortVolumeMetricsDataset().run(tmp_store, short_options).rows_loaded == 2

    rows = refresh_offexchange_quality_report(
        tmp_store,
        OffExchangeQualityReportOptions(
            source="quality-fixture",
            offexchange_source="offx-fixture",
            short_volume_source="short-fixture",
        ),
    )
    assert rows == 2

    reports = tmp_store.con.execute(
        """
        SELECT surface, row_count, security_count, venue_or_market_count,
               total_volume, ats_volume, non_ats_volume,
               short_volume, short_volume_ratio, ats_share_pct,
               high_short_flow_count, restated_key_count, bad_row_count,
               missing_available_at_count, is_latest_revision
        FROM offexchange_quality_report
        ORDER BY surface
        """
    ).df()
    offx = reports[reports["surface"] == "offexchange_volume"].iloc[0]
    assert offx.row_count == 3
    assert offx.security_count == 1
    assert offx.venue_or_market_count == 3
    assert offx.total_volume == pytest.approx(66925813.0)
    assert offx.ats_volume == pytest.approx(21246912.0)
    assert offx.non_ats_volume == pytest.approx(45678901.0)
    assert offx.ats_share_pct == pytest.approx(31.7470, abs=1e-4)
    assert offx.bad_row_count == 0
    assert bool(offx.is_latest_revision) is True

    short = reports[reports["surface"] == "finra_short_volume"].iloc[0]
    assert short.row_count == 3
    assert short.security_count == 2
    assert short.venue_or_market_count == 2
    assert short.total_volume == pytest.approx(1800.0)
    assert short.short_volume == pytest.approx(500.0)
    assert short.short_volume_ratio == pytest.approx(500.0 / 1800.0)
    assert short.high_short_flow_count == 1
    assert short.restated_key_count == 0
    assert short.missing_available_at_count == 0

    before = offexchange_quality_report_asof(tmp_store, as_of_date=dt.date(2025, 1, 19))
    assert before.empty
    after = offexchange_quality_report_asof(
        tmp_store,
        as_of_date=dt.date(2026, 1, 4),
        surfaces=("offexchange_volume", "finra_short_volume"),
    )
    assert set(after["surface"]) == {"offexchange_volume", "finra_short_volume"}


def test_offexchange_quality_report_preserves_report_revisions(tmp_store, tmp_path):
    from atx_db.asof import offexchange_quality_report_asof
    from atx_db.offexchange import FinraOffExchangeOptions, load_offexchange_volume
    from atx_db.offexchange_quality import OffExchangeQualityReportOptions, refresh_offexchange_quality_report

    base = _weekly_aapl_finra_frame()
    p1 = _write_csv(tmp_path, base, "v1.csv")
    options = OffExchangeQualityReportOptions(
        source="quality-fixture",
        offexchange_source="offx-fixture",
        include_short_volume=False,
    )
    load_offexchange_volume(
        tmp_store,
        FinraOffExchangeOptions(source_file=p1, source="offx-fixture", period_type="weekly"),
    )
    assert refresh_offexchange_quality_report(tmp_store, options) == 1

    restated = base.copy()
    restated.loc[restated["MPID"] == "UBSA", "totalWeeklyShareQuantity"] = "13000000"
    restated["available_at"] = "2025-02-01 12:00:00"
    p2 = _write_csv(tmp_path, restated, "v2.csv")
    load_offexchange_volume(
        tmp_store,
        FinraOffExchangeOptions(
            source_file=p2,
            source="offx-fixture",
            period_type="weekly",
            replace_source_file=False,
        ),
    )
    assert refresh_offexchange_quality_report(tmp_store, options) == 1

    revisions = tmp_store.con.execute(
        """
        SELECT ats_volume, restated_key_count, restatement_seq, is_latest_revision
        FROM offexchange_quality_report
        ORDER BY available_at
        """
    ).fetchall()
    assert revisions == [
        (21246912.0, 0, 0, False),
        (21901234.0, 3, 1, True),
    ]

    early = offexchange_quality_report_asof(tmp_store, as_of_date=dt.date(2025, 1, 25))
    assert early.iloc[0]["ats_volume"] == pytest.approx(21246912.0)
    late = offexchange_quality_report_asof(tmp_store, as_of_date=dt.date(2025, 2, 2))
    assert late.iloc[0]["ats_volume"] == pytest.approx(21901234.0)
