"""S2.3 rule ``cusip-history-v1`` on a fixture lake: FTD runs, extended ranges, OpenFIGI snapshot runs clipped
to the line, the 13F name alias (fragments only, never another valid CUSIP), derived ISINs, 13F coverage."""

import datetime as dt

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import cusip_history as CH

D = dt.date


def _write(path, cols: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.table(cols), path)


@pytest.fixture()
def lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    lines = [(1, D(2013, 1, 2), D(2026, 9, 18)), (2, D(2016, 1, 4), D(2026, 9, 18)), (3, D(2013, 1, 2), D(2026, 9, 18)),
             (4, D(2021, 1, 4), D(2026, 9, 18))]
    _write(tmp_path / "security_master" / "lines.parquet",
           {"security_id": [r[0] for r in lines], "first_session": [r[1] for r in lines],
            "last_session": [r[2] for r in lines]})
    wd = tmp_path / "_tmp" / "identity_v3"
    runs = [  # cusip, security_id, run, obs_from, obs_to, n_obs, available_at, first_available_at, symbol, description
        ("H1467J104", 1, 0, D(2019, 1, 10), D(2019, 6, 20), 40, dt.datetime(2019, 2, 7), dt.datetime(2019, 2, 7), "CB", "CHUBB"),
        ("084670702", 2, 0, D(2019, 1, 10), D(2019, 12, 20), 50, dt.datetime(2019, 2, 7), dt.datetime(2019, 2, 7), "BRK.B", "BRK B"),
        ("82509L107", 3, 0, D(2019, 1, 10), D(2019, 12, 20), 50, dt.datetime(2019, 2, 7), dt.datetime(2019, 2, 7), "SHOP", "SHOPIFY"),
    ]
    cols = ["cusip", "security_id", "run", "obs_from", "obs_to", "n_obs", "available_at", "first_available_at", "symbol",
            "description"]
    _write(wd / "ftd_cusip_runs.parquet", {c: [r[i] for r in runs] for i, c in enumerate(cols)})
    q = [  # period_q, cusip, value_sh, filers, name, option_title, value_sh_option_title
        (D(2019, 3, 31), "H1467J104", 100.0, 50, "CHUBB LIMITED", False, None),
        (D(2019, 3, 31), "004432874", 5.0, 1, "Chubb Ltd", False, None),           # ISIN fragment -> alias
        (D(2019, 3, 31), "084670702", 200.0, 60, "BERKSHIRE HATHAWAY INC DEL CL B", True, 20.0),  # one row titled CALL
        (D(2019, 3, 31), "084670108", 50.0, 40, "BERKSHIRE HATHAWAY INC DEL CL A", False, None),  # never aliased
        (D(2019, 3, 31), "82509L107", 30.0, 20, "SHOPIFY INC CL A", False, None),
        (D(2019, 3, 31), "09857LAN8", 10.0, 5, "BOOKING HOLDINGS INC", False, None),  # debt typed SH
        (D(2022, 3, 31), "30303M102", 70.0, 30, "META PLATFORMS INC CL A", False, None),  # mapped by OpenFIGI only
    ]
    cols = ["period_q", "cusip", "value_sh", "filers", "name", "option_title", "value_sh_option_title"]
    _write(wd / "thirteenf_cusip_q.parquet", {c: [r[i] for r in q] for i, c in enumerate(cols)})
    _write(tmp_path / "security_master" / "figi.parquet",
           {"query_kind": ["cusip"], "cusip_field": ["30303M102"], "security_id": [4], "ticker": ["META"],
            "name": ["META PLATFORMS INC-CLASS A"], "fetched_at": [dt.datetime(2026, 9, 29, 23)],
            "is_us_composite": [True]})
    _write(tmp_path / "sec_filings" / "issuer_profile.parquet",
           {"cik": [100, 200, 300], "state_of_incorporation": ["Z3", "DE", "A6"]})
    _write(tmp_path / "identity" / "link_table.parquet",
           {"security_id": [1, 2, 3], "cik": [100, 200, 300], "valid_to": [D(2026, 9, 18)] * 3})
    _write(tmp_path / "thirteenf" / "cusip_map_pit.parquet",
           {"period_q": [D(2019, 3, 31)] * 3, "cusip": ["H1467J104", "084670702", "82509L107"], "security_id": [1, 2, 3]})
    return tmp_path


def test_cusip_history_rules(lake) -> None:
    rec = CH.build()
    con = duckdb.connect()
    rows = {(r[0], r[1]): r[2:] for r in con.execute(f"""
        SELECT cusip, security_id, basis, valid_from, valid_to, cusip_kind, isin, vintage_risk
        FROM read_parquet('{(lake / 'security_master' / 'cusip_history.parquet').as_posix()}')""").fetchall()}
    # FTD run extended to the line's own span (no other CUSIP on the line), clipped at 730 days around evidence
    assert rows[("H1467J104", 1)][:3] == ("ftd_symbol", D(2017, 1, 10), D(2021, 6, 19))
    assert rows[("084670702", 2)][1] == D(2017, 1, 10)          # the 730-day clip binds before the line start
    # 13F name alias for the ISIN fragment; the valid BRK.A CUSIP is never aliased to BRK.B
    assert rows[("004432874", 1)][0] == "thirteenf_name_alias" and rows[("004432874", 1)][3] == "thirteenf_alias"
    assert not any(k[0] == "084670108" for k in rows)
    # OpenFIGI snapshot run: the CUSIP's 13F span on the line that carries today's ticker, not point in time
    assert rows[("30303M102", 4)][0] == "openfigi_ticker" and rows[("30303M102", 4)][5] == "snapshot_non_pit"
    # ISINs: US issuer, Canadian (A6) issuer -> CA prefix
    assert rows[("084670702", 2)][4] == "US0846707026"
    assert rows[("82509L107", 3)][4] == "CA82509L1076"
    assert rec["isin_check_valid"][0] == rec["isin_check_valid"][1]
    cov = CH.measure()["2019-03-31"]
    assert cov["stage_pit_map"] == round(330 / 395, 5)
    assert cov["history_all"] == round(335 / 395, 5)                         # + the aliased fragment
    # the debt row and the CALL-titled part of BRK.B leave the base; the rest of BRK.B stays
    assert cov["equity_only_history_all"] == round(315 / 365, 5)
