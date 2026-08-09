from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


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


def _write_pipe(tmp_path, frame: pd.DataFrame, name: str = "CNMSshvol20260102.txt"):
    path = tmp_path / name
    frame.to_csv(path, index=False, sep="|")
    return path


def test_normalize_finra_pipe_fields():
    from atx_db.short_volume import FinraShortVolumeOptions, normalize_short_volume_rows

    out = normalize_short_volume_rows(
        _daily_short_volume_frame(),
        options=FinraShortVolumeOptions(source="fixture"),
    )
    assert list(out["symbol"]) == ["AAPL", "AAPL", "MSFT"]
    assert set(out["market_code"]) == {"N", "Q"}
    assert out.loc[0, "short_volume"] == 100
    assert out.loc[0, "short_exempt_volume"] == 5
    assert out.loc[0, "total_volume"] == 200
    assert out.loc[0, "available_at"] == pd.Timestamp("2026-01-03 22:00:00")


def test_compute_short_volume_metrics_rolls_up_and_ranks():
    from atx_db.short_volume import FinraShortVolumeOptions, compute_short_volume_metrics, normalize_short_volume_rows

    rows = normalize_short_volume_rows(
        _daily_short_volume_frame(),
        options=FinraShortVolumeOptions(source="fixture"),
    )
    metrics = compute_short_volume_metrics(rows, run_id="run-1").sort_values("symbol").reset_index(drop=True)

    aapl = metrics[metrics["symbol"] == "AAPL"].iloc[0]
    assert aapl.short_volume == 400
    assert aapl.short_exempt_volume == 20
    assert aapl.total_volume == 800
    assert aapl.short_volume_ratio == pytest.approx(0.5)
    assert aapl.short_exempt_ratio == pytest.approx(0.025)
    assert aapl.market_count == 2
    assert aapl.dominant_market_code == "Q"
    assert aapl.dominant_market_total_volume == 600
    assert aapl.dominant_market_share_pct == pytest.approx(75.0)
    assert aapl.short_volume_ratio_percentile == pytest.approx(1.0)
    assert bool(aapl.is_high_short_flow) is True

    msft = metrics[metrics["symbol"] == "MSFT"].iloc[0]
    assert msft.short_volume_ratio == pytest.approx(0.1)
    assert msft.short_volume_ratio_percentile == pytest.approx(0.5)
    assert bool(msft.is_high_short_flow) is False


def test_load_restatement_preserves_raw_asof_visibility(tmp_store, tmp_path):
    from atx_db.asof import finra_short_volume_asof
    from atx_db.short_volume import FinraShortVolumeOptions, load_finra_short_volume

    base = _daily_short_volume_frame().iloc[[0]].copy()
    p1 = _write_pipe(tmp_path, base, "v1.txt")
    load_finra_short_volume(tmp_store, FinraShortVolumeOptions(source_file=p1, source="fixture"))

    restated = base.copy()
    restated["ShortVolume"] = "150"
    restated["available_at"] = "2026-01-04 22:00:00"
    p2 = _write_pipe(tmp_path, restated, "v2.txt")
    load_finra_short_volume(
        tmp_store,
        FinraShortVolumeOptions(source_file=p2, source="fixture", replace_source_file=False),
    )

    current_latest = tmp_store.con.execute(
        """
        SELECT short_volume, restatement_seq
        FROM finra_short_volume
        WHERE symbol='AAPL' AND market_code='N' AND is_latest
        """
    ).fetchone()
    assert current_latest == (150.0, 1)

    early = finra_short_volume_asof(tmp_store, as_of_date=dt.date(2026, 1, 3), symbols=["AAPL"])
    assert len(early) == 1
    assert early.iloc[0]["short_volume"] == 100

    late = finra_short_volume_asof(tmp_store, as_of_date=dt.date(2026, 1, 5), symbols=["AAPL"])
    assert len(late) == 1
    assert late.iloc[0]["short_volume"] == 150


def test_dataset_materializes_metric_revisions_and_asof(tmp_store, tmp_path):
    from atx_db.asof import short_volume_metrics_asof
    from atx_db.short_volume import FinraShortVolumeDataset, FinraShortVolumeOptions, ShortVolumeMetricsDataset

    base = _daily_short_volume_frame().iloc[[0]].copy()
    p1 = _write_pipe(tmp_path, base, "metric-v1.txt")
    options1 = FinraShortVolumeOptions(source_file=p1, source="fixture")
    assert FinraShortVolumeDataset().run(tmp_store, options1).rows_loaded == 1
    assert ShortVolumeMetricsDataset().run(tmp_store, options1).rows_loaded == 1

    restated = base.copy()
    restated["ShortVolume"] = "150"
    restated["available_at"] = "2026-01-04 22:00:00"
    p2 = _write_pipe(tmp_path, restated, "metric-v2.txt")
    options2 = FinraShortVolumeOptions(source_file=p2, source="fixture", replace_source_file=False)
    assert FinraShortVolumeDataset().run(tmp_store, options2).rows_loaded == 1
    assert ShortVolumeMetricsDataset().run(tmp_store, options2).rows_loaded == 1

    revisions = tmp_store.con.execute(
        "SELECT short_volume, is_latest_revision, restatement_seq FROM short_volume_metrics ORDER BY available_at"
    ).fetchall()
    assert revisions == [(100.0, False, 0), (150.0, True, 1)]

    early = short_volume_metrics_asof(tmp_store, as_of_date=dt.date(2026, 1, 3), symbols=["AAPL"])
    assert early.iloc[0]["short_volume_ratio"] == pytest.approx(0.5)
    late = short_volume_metrics_asof(tmp_store, as_of_date=dt.date(2026, 1, 5), symbols=["AAPL"])
    assert late.iloc[0]["short_volume_ratio"] == pytest.approx(0.75)
