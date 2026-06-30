"""Resilience of the SEC companyfacts loader for large backfills.

A single CIK that 404s / times out must not abort a multi-hundred-CIK backfill when
``skip_failed_targets`` is set; the failed targets are recorded and the load continues.
Default behavior (skip_failed_targets=False) still raises on the first failure. No real
network: the SEC session and target resolver are monkeypatched.
"""
from __future__ import annotations

import requests

from db.fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions


_PAYLOAD = {
    "facts": {
        "us-gaap": {
            "Assets": {
                "label": "Assets",
                "units": {
                    "USD": [
                        {
                            "end": "2023-12-31",
                            "val": 100.0,
                            "accn": "0000000001-24-000001",
                            "fy": 2023,
                            "fp": "FY",
                            "form": "10-K",
                            "filed": "2024-02-15",
                        }
                    ]
                },
            }
        }
    }
}


class _FakeResp:
    def __init__(self, fail: bool):
        self._fail = fail

    def raise_for_status(self):
        if self._fail:
            raise requests.HTTPError("404 Not Found")

    def json(self):
        return _PAYLOAD


class _FakeSession:
    def get(self, url, timeout=None):
        # The second CIK always fails.
        return _FakeResp(fail="CIK0000000002" in url)


_TARGETS = [
    ("AAA", "0000000001", "SEC-CIK-0000000001"),
    ("BBB", "0000000002", "SEC-CIK-0000000002"),
]


def _patch(monkeypatch):
    monkeypatch.setattr("db.fundamentals.sec_session", lambda ua: _FakeSession())
    monkeypatch.setattr("db.fundamentals.resolve_companyfacts_targets", lambda store, opts: _TARGETS)


def test_skip_failed_targets_continues_past_a_bad_cik(tmp_store, monkeypatch):
    _patch(monkeypatch)
    res = SecCompanyFactsDataset().run(
        tmp_store,
        SecCompanyFactsOptions(symbols=("AAA", "BBB"), skip_failed_targets=True, max_attempts=2),
    )
    assert res.details["loaded_targets"] == 1
    assert res.details["failed_target_count"] == 1
    assert res.details["failed_targets"][0]["cik"] == "0000000002"
    # The good CIK's facts landed.
    loaded = tmp_store.con.execute("SELECT COUNT(DISTINCT cik) FROM sec_company_facts").fetchone()[0]
    assert loaded == 1


def test_default_raises_on_first_failure(tmp_store, monkeypatch):
    _patch(monkeypatch)
    import pytest

    with pytest.raises(RuntimeError):
        SecCompanyFactsDataset().run(
            tmp_store,
            SecCompanyFactsOptions(symbols=("AAA", "BBB")),  # skip_failed_targets defaults False
        )


def _seed_loaded_facts(store):
    store.con.execute(
        "INSERT INTO sec_company_facts "
        "(source, security_id, cik, taxonomy, concept, unit, period_end, filed_date, value, available_at, source_url) "
        "VALUES "
        "('cf','SEC-CIK-0000000001','0000000001','us-gaap','Assets','USD',DATE '2023-12-31',DATE '2024-02-15',100.0,TIMESTAMP '2024-02-15',?),"
        "('cf','SEC-CIK-0000000002','0000000002','us-gaap','Assets','USD',DATE '2023-12-31',DATE '2024-02-15',200.0,TIMESTAMP '2024-02-15',?)",
        ["https://data.sec.gov/x", "https://data.sec.gov/y"],
    )
    store.con.execute(
        "INSERT INTO sec_company_tickers (cik, ticker, title, security_id) VALUES "
        "('0000000001','AAA','Alpha Inc','SEC-CIK-0000000001'),"
        "('0000000002','BBB','Beta Inc','SEC-CIK-0000000002')"
    )


def test_ciks_from_loaded_facts_returns_loaded_universe(tmp_store):
    """loaded_facts resolver re-targets exactly the securities already in sec_company_facts."""
    from db.fundamentals import ciks_from_loaded_facts

    _seed_loaded_facts(tmp_store)
    targets = ciks_from_loaded_facts(tmp_store)
    assert {(cik, sid) for _, cik, sid in targets} == {
        ("0000000001", "SEC-CIK-0000000001"),
        ("0000000002", "SEC-CIK-0000000002"),
    }
    # representative ticker resolved from sec_company_tickers
    by_cik = {cik: ticker for ticker, cik, _ in targets}
    assert by_cik["0000000001"] == "AAA"


def test_loaded_facts_dispatch_and_limit(tmp_store):
    from db.fundamentals import resolve_companyfacts_targets

    _seed_loaded_facts(tmp_store)
    full = resolve_companyfacts_targets(tmp_store, SecCompanyFactsOptions(symbol_source="loaded_facts"))
    assert len(full) == 2
    limited = resolve_companyfacts_targets(
        tmp_store, SecCompanyFactsOptions(symbol_source="loaded_facts", symbol_limit=1)
    )
    assert len(limited) == 1


def test_normalize_drops_non_us_gaap_taxonomies():
    """IFRS (ifrs-full) facts are dropped at load — only us-gaap/dei are kept — so
    they neither leave catalog concepts unmapped nor collide on canonical metric names."""
    from db.fundamentals import normalize_companyfacts

    payload = {
        "facts": {
            "us-gaap": {"Assets": {"units": {"USD": [
                {"end": "2023-12-31", "val": 100.0, "accn": "x-1", "fy": 2023, "fp": "FY", "form": "10-K", "filed": "2024-02-15"}
            ]}}},
            "ifrs-full": {"Assets": {"units": {"CLP": [
                {"end": "2023-12-31", "val": 999.0, "accn": "x-1", "fy": 2023, "fp": "FY", "form": "20-F", "filed": "2024-02-15"}
            ]}}},
        }
    }
    facts, points = normalize_companyfacts(
        payload, symbol="ZZZ", security_id="SEC-CIK-0000000009", cik="0000000009",
        source_url="https://data.sec.gov/", concepts={"Assets"}, run_id=None,
    )
    assert set(facts["taxonomy"]) == {"us-gaap"}
    assert (facts["unit"] == "CLP").sum() == 0
