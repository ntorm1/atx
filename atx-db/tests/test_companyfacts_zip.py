"""S45: offline SEC companyfacts.zip bulk fetcher for the fundamentals loader.

SEC publishes the entire XBRL company-facts universe as a single nightly bulk archive
(`companyfacts.zip`, ~1.4 GB) at
``https://www.sec.gov/Archives/edgar/daily-index/xbrl/companyfacts.zip`` containing one
``CIK##########.json`` per filer — byte-identical JSON to the per-CIK
``data.sec.gov/.../companyfacts/CIK*.json`` endpoint. Backfilling from a locally-downloaded
zip replaces N throttled network round trips with one download plus N lazy member reads,
and removes the per-CIK 404/throttle/retry burden. Download is a one-time operator step;
these tests run purely against a tiny fixture zip built on disk — no network.
"""
from __future__ import annotations

import json
import zipfile

from atx_db.fundamentals import (
    SecCompanyFactsDataset,
    SecCompanyFactsOptions,
    _make_companyfacts_zip_fetcher,
)


def _companyfacts_payload(cik: str = "0000320193") -> dict:
    return {
        "cik": int(cik),
        "entityName": "Apple Inc.",
        "facts": {
            "us-gaap": {
                "Assets": {
                    "label": "Assets",
                    "units": {
                        "USD": [
                            {
                                "end": "2023-12-31",
                                "val": 100.0,
                                "accn": "0000320193-24-000001",
                                "fy": 2023,
                                "fp": "FY",
                                "form": "10-K",
                                "filed": "2024-02-15",
                            }
                        ]
                    },
                }
            }
        },
    }


def _write_companyfacts_zip(path, ciks=("0000320193",)):
    with zipfile.ZipFile(path, "w") as zf:
        for cik in ciks:
            zf.writestr(f"CIK{cik}.json", json.dumps(_companyfacts_payload(cik)))
    return path


class TestCompanyFactsZipFetcher:
    def test_returns_payload_for_present_cik(self, tmp_path):
        zpath = _write_companyfacts_zip(tmp_path / "companyfacts.zip")
        fetch = _make_companyfacts_zip_fetcher(zpath)
        payload = fetch("320193")
        assert payload is not None
        assert "us-gaap" in payload["facts"]

    def test_returns_none_for_missing_cik(self, tmp_path):
        zpath = _write_companyfacts_zip(tmp_path / "companyfacts.zip")
        fetch = _make_companyfacts_zip_fetcher(zpath)
        assert fetch("999999") is None

    def test_returns_none_for_non_numeric_cik(self, tmp_path):
        zpath = _write_companyfacts_zip(tmp_path / "companyfacts.zip")
        fetch = _make_companyfacts_zip_fetcher(zpath)
        assert fetch("not-a-cik") is None

    def test_zero_pads_bare_integer_cik(self, tmp_path):
        # Member names are zero-padded to CIK10; a bare integer must still resolve.
        zpath = _write_companyfacts_zip(tmp_path / "companyfacts.zip")
        fetch = _make_companyfacts_zip_fetcher(zpath)
        assert fetch(320193) is not None


class TestCompanyFactsZipLoad:
    def test_load_reads_from_zip_without_network(self, tmp_store, tmp_path, monkeypatch):
        zpath = _write_companyfacts_zip(tmp_path / "companyfacts.zip")

        # Any network use must blow up — proves the zip path is fully offline.
        def _boom(ua):
            raise AssertionError("sec_session must not be called when companyfacts_zip is set")

        monkeypatch.setattr("atx_db.fundamentals.sec_session", _boom)
        monkeypatch.setattr(
            "atx_db.fundamentals.resolve_companyfacts_targets",
            lambda store, opts: [("AAPL", "0000320193", "SEC-CIK-0000320193")],
        )
        res = SecCompanyFactsDataset().run(
            tmp_store,
            SecCompanyFactsOptions(companyfacts_zip=zpath, concepts=("Assets",)),
        )
        assert res.rows_loaded >= 1
        n = tmp_store.con.execute(
            "SELECT count(*) FROM sec_company_facts WHERE cik = '0000320193'"
        ).fetchone()[0]
        assert n >= 1

    def test_missing_zip_member_is_skipped_not_fatal(self, tmp_store, tmp_path, monkeypatch):
        # A CIK absent from the bulk zip must be recorded as a failed target and skipped
        # (skip_failed_targets defaults on for zip loads via the option), not abort the run.
        zpath = _write_companyfacts_zip(tmp_path / "companyfacts.zip", ciks=("0000320193",))
        monkeypatch.setattr("atx_db.fundamentals.sec_session", lambda ua: None)
        monkeypatch.setattr(
            "atx_db.fundamentals.resolve_companyfacts_targets",
            lambda store, opts: [
                ("AAPL", "0000320193", "SEC-CIK-0000320193"),
                ("GHOST", "0009999999", "SEC-CIK-0009999999"),
            ],
        )
        res = SecCompanyFactsDataset().run(
            tmp_store,
            SecCompanyFactsOptions(
                companyfacts_zip=zpath, concepts=("Assets",), skip_failed_targets=True
            ),
        )
        assert res.details["loaded_targets"] == 1
        assert res.details["failed_target_count"] == 1
