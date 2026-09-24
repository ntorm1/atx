from __future__ import annotations

import datetime as dt
import json
from types import SimpleNamespace

import duckdb
import pytest

from atx_db._companyfacts_resume import verify_companyfacts_resume
from atx_db.connection import DuckDBStore

_RUN_ID = "10000000-0000-0000-0000-000000000001"
_SOURCE_URL = "https://example.invalid/companyfacts.zip"
_SHARED_SECURITY = "shared-historical-security"


def _retained_store(tmp_path, *, damage=None):
    """Only the verifier's source contract; no full warehouse bootstrap needed."""
    archive = tmp_path / "companyfacts.zip"
    archive.write_bytes(b"retained source archive")
    options = SimpleNamespace(
        companyfacts_zip=archive, symbol_source="archive_members", symbol_limit=None,
        symbol_offset=0, skip_loaded_targets=False, as_of_date=dt.date(2026, 9, 20),
        universe_id="full_us", concepts=("Assets",), resume_from_run_id=_RUN_ID,
        run_id="10000000-0000-0000-0000-000000000002",
    )
    path = tmp_path / "retained.duckdb"
    with duckdb.connect(str(path), config={"memory_limit": "64MB", "threads": "1"}) as con:
        con.execute("""CREATE TABLE dataset_runs (
            dataset_id VARCHAR, run_id VARCHAR, source VARCHAR, status VARCHAR,
            started_at TIMESTAMP, finished_at TIMESTAMP, params_json VARCHAR)""")
        params = {
            key: getattr(options, key) for key in (
                "symbol_source", "symbol_limit", "symbol_offset", "skip_loaded_targets", "universe_id",
            )
        }
        params.update(companyfacts_zip=str(archive), as_of_date="2026-09-20", concepts=["Assets"])
        con.execute("""INSERT INTO dataset_runs VALUES (
            'sec_company_facts', ?, 'SEC companyfacts', 'failed',
            TIMESTAMP '2026-09-20 00:00:00', TIMESTAMP '2026-09-20 02:00:00', ?)""",
            [_RUN_ID, json.dumps(params)])
        con.execute("""CREATE TABLE raw_source_files (
            dataset_id VARCHAR, cache_path VARCHAR, source_url VARCHAR, sha256 VARCHAR,
            byte_count BIGINT, fetched_at TIMESTAMP, status VARCHAR, metadata_json VARCHAR)""")
        con.execute("""CREATE TABLE sec_company_facts (
            source VARCHAR, security_id VARCHAR, concept VARCHAR, taxonomy VARCHAR, unit VARCHAR,
            period_start DATE, period_end DATE, filed_date DATE, fiscal_year INTEGER,
            fiscal_period VARCHAR, form VARCHAR, accession_number VARCHAR, value DOUBLE,
            available_at TIMESTAMP, run_id VARCHAR, cik VARCHAR, source_url VARCHAR, entity_id VARCHAR)""")
        for cik in ("0000000001", "0000000002"):
            source_url = f"{_SOURCE_URL}#CIK{cik}.json"
            metadata = {
                "run_id": _RUN_ID, "cik": cik, "symbol": f"CIK{cik}", "source_mode": "bulk_zip",
                "archive_sha256": "archive-digest", "allowlist_sha256": "allowlist-digest", "rows": 1,
            }
            con.execute("""INSERT INTO raw_source_files VALUES (
                'sec_company_facts', ?, ?, 'archive-digest', ?, TIMESTAMP '2026-09-20 01:00:00',
                'loaded', ?)""", [str(archive), source_url, archive.stat().st_size, json.dumps(metadata)])
            con.execute("""INSERT INTO sec_company_facts VALUES (
                'SEC companyfacts', ?, 'Assets', 'us-gaap', 'USD', NULL, DATE '2026-06-30',
                DATE '2026-08-01', 2026, 'Q2', '10-Q', ?, 123,
                TIMESTAMP '2026-08-01 22:00:00', ?, ?, ?, NULL)""",
                [_SHARED_SECURITY, f"{cik}-26-000001", _RUN_ID, cik, source_url])
        con.execute("""CREATE TABLE fundamental_points AS SELECT
            source, security_id, concept AS metric, taxonomy, unit, period_start, period_end,
            filed_date AS as_of_date, fiscal_year, fiscal_period, form, accession_number, value,
            available_at, run_id, cast(NULL AS VARCHAR) AS symbol FROM sec_company_facts""")
        if damage == "value":
            con.execute("UPDATE fundamental_points SET value=124 WHERE accession_number LIKE '0000000002%'")
        elif damage == "foreign_duplicate":
            con.execute("""INSERT INTO fundamental_points SELECT * REPLACE ('foreign' AS run_id)
                FROM fundamental_points WHERE accession_number LIKE '0000000002%'""")
    store = DuckDBStore(path, read_only=True)
    store.analytical_memory_limit = "64MB"
    store.analytical_threads = 1
    store.reopen()
    return store, options


def _verify(store, options):
    return verify_companyfacts_resume(
        store, options, archive_sha256="archive-digest", allowlist_sha256="allowlist-digest",
        target_ciks={"0000000001", "0000000002"}, source_url_prefix=_SOURCE_URL,
        unresolved_prefix="SEC-COMPANYFACTS-UNRESOLVED-CIK-", duplicate_members=0,
    )


def test_readonly_proof_releases_each_scan_and_preserves_shared_security_evidence(tmp_path, monkeypatch):
    store, options = _retained_store(tmp_path)
    released = []
    original_close = store.close

    def observed_close():
        released.append(store.con)
        original_close()

    monkeypatch.setattr(store, "close", observed_close)
    try:
        verified, details = _verify(store, options)
        assert details["resume_proof_connection_reopens"] == len(released) == 3
        assert len({id(con) for con in [*released, store.con]}) == 4
        assert details["resume_verified_rows"] == details["previously_completed_targets"] == 2
        assert set(verified) == {"0000000001", "0000000002"}
        for member in verified.values():
            assert member.rows == 1 and member.run_id == _RUN_ID
            assert member.unresolved is not None
            assert member.unresolved["entity_unresolved_count"] == 1
            assert member.unresolved["available_at"] == dt.datetime(2026, 8, 1, 22)
    finally:
        store.close()


@pytest.mark.parametrize("damage", ["value", "foreign_duplicate"])
def test_phase_recycling_cannot_hide_shared_security_point_damage(tmp_path, damage):
    store, options = _retained_store(tmp_path, damage=damage)
    try:
        with pytest.raises(ValueError, match="retained fact/point evidence does not match"):
            _verify(store, options)
    finally:
        store.close()
