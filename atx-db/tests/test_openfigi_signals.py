from __future__ import annotations

from typing import ClassVar

import duckdb

from atx_db.connection import DuckDBStore
from atx_db.openfigi_signals import OpenFigiSignalMapOptions, map_signal_cusips, rank_openfigi_candidates
from atx_db.thirteenf_signals import ensure_thirteenf_signal_schema


class _Response:
    status_code = 200
    headers: ClassVar[dict[str, str]] = {"ratelimit-limit": "25", "ratelimit-remaining": "24"}

    def raise_for_status(self) -> None:
        return None

    def json(self):
        return [
            {
                "data": [
                    {
                        "figi": "BBG000B9XRY4",
                        "compositeFIGI": "BBG000B9XRY4",
                        "shareClassFIGI": "BBG001S5N8V8",
                        "ticker": "AAPL",
                        "name": "APPLE INC",
                        "exchCode": "US",
                        "marketSector": "Equity",
                        "securityType": "Common Stock",
                        "securityType2": "Common Stock",
                    }
                ]
            }
        ]


class _Session:
    def __init__(self) -> None:
        self.headers: dict[str, str] = {}

    def post(self, *_args, **_kwargs):
        return _Response()

    def close(self) -> None:
        return None


def _store() -> DuckDBStore:
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(":memory:")
    ensure_thirteenf_signal_schema(store)
    store.con.execute(
        """
        CREATE TABLE raw_source_files (
            source_id VARCHAR, dataset_id VARCHAR, source_url VARCHAR,
            cache_path VARCHAR, sha256 VARCHAR, byte_count BIGINT,
            fetched_at TIMESTAMP, status VARCHAR, metadata_json VARCHAR
        )
        """
    )
    return store


def test_candidate_ranking_prefers_us_common_equity() -> None:
    ranked = rank_openfigi_candidates(
        (
            {"figi": "fund", "ticker": "AAPL", "marketSector": "Equity", "securityType2": "Fund"},
            {
                "figi": "common",
                "ticker": "AAPL",
                "marketSector": "Equity",
                "securityType2": "Common Stock",
                "compositeFIGI": "common",
                "exchCode": "US",
            },
        )
    )
    assert ranked[0]["figi"] == "common"
    assert all(candidate["figi"] != "fund" for candidate in ranked)


def test_map_signal_cusips_persists_selected_candidate(tmp_path) -> None:
    store = _store()
    try:
        store.con.execute(
            """
            INSERT INTO thirteenf_consensus_amendment_signals (
                signal_id, report_period, cusip, signal_available_at,
                distinct_filer_count, average_zscore, minimum_zscore,
                maximum_zscore, corrected_position_events, manager_ciks_json,
                signal_rank, is_stress_quarter
            ) VALUES (
                'signal', DATE '2020-03-31', '037833100', TIMESTAMP '2020-05-20',
                3, 4.0, 3.0, 5.0, 3, '[]', 1, false
            )
            """
        )
        store.con.execute(
            """
            INSERT INTO thirteenf_consensus_amendment_signals (
                signal_id, report_period, cusip, signal_available_at,
                distinct_filer_count, average_zscore, minimum_zscore,
                maximum_zscore, corrected_position_events, manager_ciks_json,
                signal_rank, is_stress_quarter
            ) VALUES
                ('low-rank', DATE '2020-03-31', '594918104', TIMESTAMP '2020-05-20',
                 3, 2.1, 2.0, 2.2, 3, '[]', 21, false),
                ('stress', DATE '2020-06-30', '02079K305', TIMESTAMP '2020-08-20',
                 3, 8.0, 7.0, 9.0, 3, '[]', 1, true)
            """
        )
        result = map_signal_cusips(
            store,
            OpenFigiSignalMapOptions(cache_dir=tmp_path, run_id="test-openfigi"),
            session=_Session(),
        )
        assert (result.requested_cusips, result.mapped_cusips, result.candidate_rows) == (1, 1, 1)
        assert store.con.execute(
            "SELECT cusip, ticker, figi, selected FROM v_thirteenf_signal_instruments"
        ).fetchone() == ("037833100", "AAPL", "BBG000B9XRY4", True)
        assert store.con.execute(
            "SELECT count(*) FROM raw_source_files WHERE dataset_id = 'openfigi_signal_map'"
        ).fetchone()[0] == 1
    finally:
        store.connection.close()
        store.connection = None
