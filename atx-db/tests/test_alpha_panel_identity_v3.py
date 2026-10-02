"""S2.5 identity v3: symbol splitting and the dated filing-evidence tier on a fixture lake."""

import datetime as dt

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import identity_v3 as V


@pytest.mark.parametrize("raw,want", [
    ("N/A", []), ("none", []), ("Z AND ZG", ["Z", "ZG"]), ("vicr", ["VICR"]), ("VIACA,VIAC", ["VIACA", "VIAC"]),
    ("JWA/JWB", ["JWA", "JWB"]), ("NYSE: KRC", ["KRC"]), ("BIO BIOB", ["BIO", "BIOB"]), ("(SIRI)", ["SIRI"]),
    ("(NYSE:FBC)", ["FBC"]), ("LEN, LEN.B", ["LEN", "LEN.B"]), ("N O G", ["NOG"]), ("QADA_QADB", ["QADA", "QADB"]),
    ("[ SHCR ]", ["SHCR"]), ("BRK/B", ["BRK/B"]), ("NYSE/TRN", ["TRN"]), ("NASDAQ GS: MSFT", ["MSFT"]),
])
def test_split_symbols(raw, want) -> None:
    assert V.split_symbols(raw) == want


def _write(path, cols: dict, schema=None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.table(cols, schema=schema), path)


def _sessions() -> list[dt.date]:
    d, out = dt.date(2019, 1, 2), []
    while d <= dt.date(2019, 12, 31):
        if d.weekday() < 5:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


@pytest.fixture()
def lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    sess = _sessions()
    lines = {1: "AAA", 2: "BBB", 3: "O"}
    rows = [(d, sid, tk) for d in sess for sid, tk in lines.items()]
    _write(tmp_path / "_tmp" / "shortflow" / "vendor" / "vendor_tickers_2019.parquet",
           {"d": [r[0] for r in rows], "security_id": [r[1] for r in rows], "tk": [r[2] for r in rows],
            "ck": [r[2] for r in rows], "sid_repaired": [False] * len(rows)})
    _write(tmp_path / "_tmp" / "panel_core" / "bucket_00.parquet",
           {"session_date": [r[0] for r in rows], "security_id": [r[1] for r in rows]})

    def ts(y, m, d, h):
        return dt.datetime(y, m, d, h)

    form4 = [  # (cik, symbol, accession, filing_date, available_at)
        (100, "AAA", "a1", dt.date(2019, 2, 1), ts(2019, 2, 1, 21)),
        (100, "aaa", "a2", dt.date(2019, 3, 1), ts(2019, 3, 1, 15)),
        (300, "N O G", "a3", dt.date(2019, 4, 1), ts(2019, 4, 1, 15)),     # NOG: no line (never the 'O' line)
        (300, "N O G", "a4", dt.date(2019, 4, 2), ts(2019, 4, 2, 15)),
        (500, "NASDAQ: AAA", "a5", dt.date(2019, 9, 4), ts(2019, 9, 4, 14)),
        (600, "N/A", "a6", dt.date(2019, 5, 1), ts(2019, 5, 1, 14)),
        (700, "BBB", "a7", dt.date(2019, 1, 10), ts(2019, 1, 10, 14)),   # a single filing never links (weight 1)
    ]
    _write(tmp_path / "insider" / "transactions" / "year=2019" / "2019q1.parquet",
           {"issuer_cik": [r[0] for r in form4], "issuer_symbol": [r[1] for r in form4],
            "accession": [r[2] for r in form4], "filing_date": [r[3] for r in form4],
            "available_at": [r[4] for r in form4]})
    cover = [  # (cik, symbol, adsh, filed, available_at UTC)
        (400, "BBB", "c1", dt.date(2019, 6, 3), dt.datetime(2019, 6, 3, 20, tzinfo=dt.timezone.utc)),
        (500, "AAA", "c2", dt.date(2019, 9, 3), dt.datetime(2019, 9, 3, 12, tzinfo=dt.timezone.utc)),
        (800, "BBB", "c3", dt.date(2019, 6, 3), dt.datetime(2019, 6, 3, 23, tzinfo=dt.timezone.utc)),  # debt line
        (400, "O", "c4", dt.date(2019, 6, 3), dt.datetime(2019, 6, 3, 20, tzinfo=dt.timezone.utc)),  # co-registrant
    ]
    _write(tmp_path / "identity_cover" / "cover_page.parquet",
           {"cik": [r[0] for r in cover], "trading_symbol": [r[1] for r in cover], "symbol_norm": [r[1] for r in cover],
            "adsh": [r[2] for r in cover], "filed": [r[3] for r in cover], "available_at": [r[4] for r in cover],
            "security_exchange_name": ["NYSE"] * 4, "is_equity_like": [True, True, False, True],
            "coreg": [None, None, None, "SUB LLC"], "entity_cik": [None, None, None, None]},
           schema=pa.schema([("cik", pa.int64()), ("trading_symbol", pa.string()), ("symbol_norm", pa.string()),
                             ("adsh", pa.string()), ("filed", pa.date32()), ("available_at", pa.timestamp("us", tz="UTC")),
                             ("security_exchange_name", pa.string()), ("is_equity_like", pa.bool_()),
                             ("coreg", pa.string()), ("entity_cik", pa.int64())]))
    return tmp_path


def test_dated_tier_on_fixtures(lake) -> None:
    rec = V.evidence()
    assert rec["cover_page"] is True
    V.dated()
    import duckdb

    got = duckdb.connect().execute(f"""
        SELECT security_id, cik, min(session_date), max(session_date), bool_and(available_at <= CAST(session_date AS TIMESTAMP) + INTERVAL 22 HOUR)
        FROM read_parquet('{(lake / '_tmp' / 'identity_v3' / 'dated_days.parquet').as_posix()}')
        GROUP BY 1, 2 ORDER BY 1, 3""").fetchall()
    assert got == [
        (1, 100, dt.date(2019, 3, 1), dt.date(2019, 9, 2), True),     # second filing makes weight 2 (point in time)
        (1, 500, dt.date(2019, 9, 3), dt.date(2019, 12, 31), True),   # reorganisation: the recent issuer wins
        (2, 400, dt.date(2019, 6, 3), dt.date(2019, 12, 31), True),   # one cover page suffices; debt row ignored
    ]


@pytest.fixture()
def table_lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    sess = [dt.date(2021, 3, d) for d in (1, 2, 3, 4, 5)]
    _write(tmp_path / "calendar.parquet", {"session_date": sess})
    lines = {1: ("AAA", 9.0), 2: ("BBB", 5.0), 3: ("CCC", 5.0), 4: ("DDD", 5.0), 5: ("EEE", 5.0), 6: ("AAA.B", 1.0),
             7: ("AAA.PR", 0.5)}
    rows = [(d, sid, tk, adv) for d in sess for sid, (tk, adv) in lines.items()]
    _write(tmp_path / "_tmp" / "panel_core" / "bucket_00.parquet",
           {"session_date": [r[0] for r in rows], "security_id": [r[1] for r in rows], "ticker": [r[2] for r in rows],
            "adv63": [r[3] for r in rows], "is_operating": [True] * len(rows), "is_index": [False] * len(rows)})
    _write(tmp_path / "_tmp" / "panel_member" / "year=2021.parquet",
           {"session_date": [r[0] for r in rows], "security_id": [r[1] for r in rows], "member": [True] * len(rows)})
    _write(tmp_path / "panel" / "year=2021" / "panel-03.parquet",
           {"session_date": [r[0] for r in rows], "security_id": [r[1] for r in rows],
            "member_equity": [r[1] != 7 for r in rows]})
    t0 = dt.datetime(2021, 2, 1)
    comb = [  # security_id, cik, start, end_incl, primary, tier, basis, available_at
        (1, 10, sess[0], sess[-1], "P", "high", "reconstructed_high", t0),
        (2, 20, sess[0], sess[-1], "P", "backfill", "snapshot_run_backfill", dt.datetime(2026, 9, 20)),
        (3, 31, sess[0], sess[-1], "P", "backfill", "snapshot_run_backfill", dt.datetime(2026, 9, 20)),
    ]
    cols = ["security_id", "cik", "start", "end_incl", "primary", "tier", "basis", "available_at"]
    _write(tmp_path / "identity" / "links_combined.parquet", {c: [r[i] for r in comb] for i, c in enumerate(cols)})
    wd = tmp_path / "_tmp" / "identity_v3"
    dated = [(2, d, 20, t0, 3.0, 3.0, 1, ["form345"]) for d in sess[2:]] + \
            [(4, d, 40, t0, 2.0, 2.0, 1, ["cover_page"]) for d in sess] + \
            [(6, d, 10, t0, 2.0, 2.0, 1, ["form345"]) for d in sess] + \
            [(7, d, 10, t0, 2.0, 2.0, 1, ["form345"]) for d in sess] + \
            [(1, d, 99, t0, 2.0, 2.0, 1, ["form345"]) for d in sess]        # disagrees with strict: strict wins
    cols = ["security_id", "session_date", "cik", "available_at", "w_window", "w_recent", "n_candidates", "src"]
    _write(wd / "dated_days.parquet", {c: [r[i] for r in dated] for i, c in enumerate(cols)})
    names = [(3, d, 30, t0) for d in sess]
    _write(wd / "name_days_full.parquet", {"security_id": [r[0] for r in names], "session_date": [r[1] for r in names],
                                           "cik": [r[2] for r in names], "available_at": [r[3] for r in names]})
    _write(tmp_path / "short_interest" / "si.parquet",
           {"security_id": [1], "symbol": ["AAA"], "settlement_date": [dt.date(2021, 2, 26)],
            "dissemination_date": [dt.date(2021, 2, 26)]})
    _write(tmp_path / "security_master" / "finra_names.parquet",
           {"security_id": [7, 1], "dissemination_date": [dt.date(2021, 2, 26)] * 2, "finra_type": ["preferred", "common"],
            "share_class": [None, "A"]})
    _write(tmp_path / "security_master" / "line_listing.parquet",
           {"security_id": [4], "listing_basis": ["sec_confirmed"], "cik": [40]})
    return tmp_path


def test_link_table_v3_on_fixtures(table_lake) -> None:
    import duckdb

    rec = V.table()
    assert rec["ambiguous_line_days"] == 0
    con = duckdb.connect()
    t = (table_lake / "identity" / "link_table_v3.parquet").as_posix()
    got = {r[0]: r[1:] for r in con.execute(f"""
        SELECT security_id, list(DISTINCT cik ORDER BY cik), list(DISTINCT link_tier ORDER BY link_tier),
               list(DISTINCT linktype ORDER BY linktype), list(DISTINCT linkprim ORDER BY linkprim), sum(sessions)
        FROM read_parquet('{t}') GROUP BY 1""").fetchall()}
    assert got[1] == ([10], ["strict"], ["LC"], ["P"], 5)          # strict beats the disagreeing dated CIK
    assert got[2] == ([20], ["backfill", "dated"], ["LU"], ["P"], 5)  # dated from day 3, backfill before
    assert got[3] == ([30], ["name"], ["LU"], ["P"], 5)            # the name tier outranks the snapshot tier
    assert got[4] == ([40], ["dated"], ["LC"], ["P"], 5)           # cover page -> researched
    assert 5 not in got                                           # unlinked
    assert got[6] == ([10], ["dated"], ["LU"], ["J"], 5)           # second common class of issuer 10
    assert got[7] == ([10], ["dated"], ["LU"], ["N"], 5)           # preferred line of issuer 10
    assert rec["exports"]["pit"]["lines"] == 6 and rec["exports"]["strict"]["lines"] == 1
    y = rec["audit"]["per_year"]["2021"]
    assert y["cells"] == 30 and y["unlinked"] == 5 and y["backfill"] == 2
    assert y["share"]["pit_strict_dated_name"] == round(23 / 30, 4)
    sc = con.execute(f"SELECT DISTINCT share_class FROM read_parquet('{t}') WHERE security_id = 6").fetchall()
    assert sc == [("B",)]                                          # vendor ticker suffix
    sc1 = con.execute(f"SELECT DISTINCT share_class FROM read_parquet('{t}') WHERE security_id = 1").fetchall()
    assert sc1 == [("A",)]                                         # FINRA issue name class (security master)
    ds = con.execute(f"SELECT DISTINCT dated_sources FROM read_parquet('{t}') WHERE security_id = 4").fetchall()
    assert ds == [(["cover_page"],)]


def test_share_exchange_history_runs(tmp_path, monkeypatch) -> None:
    import duckdb

    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    sess = [dt.date(2021, 3, d) for d in (1, 2, 3, 4, 5)]
    _write(tmp_path / "calendar.parquet", {"session_date": sess})
    rows = [(d, 1, "XNYS" if d.day <= 3 else "XNAS", "common", False, False, False) for d in sess] + \
           [(d, 2, "ARCX", "etf", True, False, False) for d in sess] + \
           [(d, 3, "XNAS", "common", False, True, False) for d in sess[:2]]       # ADR, not every session
    cols = ["session_date", "security_id", "exchange", "security_type", "is_etf", "is_adr", "is_fpi"]
    data = {c: [r[i] for r in rows] for i, c in enumerate(cols)}
    data |= {"is_adr_likely": [False] * len(rows), "is_lp": [False] * len(rows), "is_reit": [False] * len(rows)}
    _write(tmp_path / "panel" / "year=2021" / "panel-03.parquet", data)
    rec = V.codes()
    got = duckdb.connect().execute(f"""
        SELECT security_id, valid_from, valid_to, sessions, exchcd, shrcd, available_at
        FROM read_parquet('{(tmp_path / 'security_master' / 'share_exchange_history.parquet').as_posix()}')
        ORDER BY 1, 2""").fetchall()
    assert [(r[0], r[3], r[4], r[5]) for r in got] == [(1, 3, 1, 11), (1, 2, 3, 11), (2, 5, 4, 73), (3, 2, 3, 31)]
    assert got[1][1] == sess[3] and got[1][6] == dt.datetime(2021, 3, 3, 22)   # stamped d-1 22:00 UTC
    assert rec["rows"] == 4


def _bridge_table(tmp_path) -> "Path":
    D, T = dt.date, dt.datetime
    rows = [  # id, from, to, tier, available_at, evidence_at
        (1, D(2022, 1, 3), D(2022, 6, 30), "strict", T(2022, 1, 3, 22), T(2022, 1, 3, 22)),
        (2, D(2023, 6, 1), D(2026, 9, 18), "strict", T(2023, 6, 1, 22), T(2023, 6, 1, 22)),      # crosses the seal
        (3, D(2024, 2, 1), D(2025, 1, 31), "dated", T(2024, 2, 1, 22), T(2024, 2, 1, 22)),       # starts after
        (4, D(2022, 3, 1), D(2026, 9, 18), "backfill", T(2026, 9, 20), T(2022, 3, 1, 22)),       # snapshot stamp
    ]
    cols = ["security_id", "cik", "valid_from", "valid_to", "link_tier", "available_at", "evidence_at"]
    data = {c: [r[(0, 0, 1, 2, 3, 4, 5)[i]] if c != "cik" else 100 + r[0] for r in rows] for i, c in enumerate(cols)}
    data |= {"is_issuer_primary": [True] * 4, "basis": ["b"] * 4, "share_class": [None] * 4,
             "linktype": ["LC"] * 4, "linkprim": ["P"] * 4, "sessions": [1.0] * 4}
    path = tmp_path / "identity" / "link_table_v3.parquet"
    _write(path, data)
    return path


def test_bridge_exports_seal(tmp_path, monkeypatch) -> None:
    import json

    import duckdb

    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    table = _bridge_table(tmp_path)
    con = duckdb.connect()
    V.export_bridges(con, table)                                   # default seal 2024-01-01
    q = lambda n: con.execute(f"""SELECT sr_id, start, end_incl, available_at, knowledge_at FROM read_parquet(
        '{(tmp_path / 'export' / f'identity-bridge-v3-{n}' / 'links.parquet').as_posix()}') ORDER BY 1""").fetchall()
    pit = {r[0]: r for r in q("pit")}
    assert sorted(pit) == [1, 2]                                   # id 3 starts after the seal: dropped
    assert pit[2][2] == dt.date(2023, 12, 31)                      # crossing interval censored
    assert pit[1][2] == dt.date(2022, 6, 30)                       # untouched
    allr = {r[0]: r for r in q("all")}
    assert sorted(allr) == [1, 2, 4]                               # backfill kept (evidence_at is the clock)
    assert allr[4][2] == dt.date(2023, 12, 31) and allr[4][4] is None   # 2026 snapshot stamp nulled
    m = json.loads((tmp_path / "export" / "identity-bridge-v3-all" / "manifest.json").read_text())
    assert m["seal"]["consumer_seal_date"] == "2024-01-01" and m["seal"]["applied"] is True
    assert m["seal"]["max_end_incl"] == "2023-12-31"
    assert m["seal"]["censored_rows"] == 2                         # ids 2 and 4 crossed; id 1 ended 2022 (not counted)
    mp = json.loads((tmp_path / "export" / "identity-bridge-v3-pit" / "manifest.json").read_text())
    assert mp["seal"]["censored_rows"] == 1                        # id 2 only (backfill is not in pit)
    assert m["counts"]["max_end_incl"] == "2023-12-31"
    V.export_bridges(con, table, seal=None)                        # disabled: unclipped
    assert {r[0]: r for r in q("pit")}[3][1] == dt.date(2024, 2, 1)
    m = json.loads((tmp_path / "export" / "identity-bridge-v3-pit" / "manifest.json").read_text())
    assert m["seal"]["applied"] is False and m["seal"]["max_end_incl"] == "2026-09-18"
    assert m["seal"]["censored_rows"] == 0
