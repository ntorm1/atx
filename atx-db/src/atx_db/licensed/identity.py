"""Vendor identifiers -> TickerHistory3 ``security_id`` and CIK, point in time, with ``link_tier``.

Inputs (three interval tables, all dated):

* ``cusip_history`` (cusip8, security_id, valid_from, valid_to, basis): the lane-ID S2.3 stage
  ``security_master/cusip_history.parquet`` when it exists, else the union of the FTD map
  (``ftd/cusip_map.parquet``, first/last seen) and the 13F PIT map (``thirteenf/cusip_map_pit.parquet``, quarter
  intervals). Matching uses the first 8 characters (issuer + issue; I/B/E/S carries 8-character CUSIPs).
* ``ticker_history`` (ticker_key, security_id, valid_from, valid_to, basis): ``security_master/ticker_history.parquet``
  when it exists, else the vendor ticker runs of the prices stage. ``ticker_key`` strips ``.``, ``/``, ``-`` and
  whitespace (the FINRA canonical form, case-sensitive: ``BRK.B`` = ``BRK/B`` = ``BRKB``).
* ``cik_links`` (security_id, cik, valid_from, valid_to, link_tier): ``identity/link_table.parquet``.

Rule ``licensed-id-v1``, per row on its ``id_date``, first tier with a candidate wins; two or more distinct lines
in that tier make the row ``ambiguous`` (kept, no ``security_id``):

1. ``vendor_native``: the vendor carries a TickerHistory3 securityID (SpiderRock products);
2. ``cusip_dated``: a CUSIP interval contains ``id_date``;
3. ``cusip_undated``: a CUSIP interval lies within 400 days of ``id_date`` (the histories are observed intervals, not
   validity intervals; snapshot-dependent, so labelled apart like the link table's ``backfill``);
4. ``ticker_dated``: a ticker interval contains ``id_date`` (ticker reuse makes this the weakest tier);
5. else ``unmapped``.

``cik`` comes from the link-table interval containing ``id_date`` (a gap of up to 10 days after an interval is
bridged; ties go to the latest ``valid_from``, then strict < name < backfill) and ``cik_link_tier`` repeats its tier.
"""

from __future__ import annotations

import hashlib
import os
from dataclasses import dataclass, field
from pathlib import Path

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

from .contract import LAKE_ROOT, connect

UNDATED_WINDOW_DAYS = 400
CIK_GAP_DAYS = 10
TIER_ORDER = ("cusip_dated", "cusip_undated", "ticker_dated")

CUSIP_SCHEMA = pa.schema([("cusip8", pa.string()), ("security_id", pa.int64()), ("valid_from", pa.date32()),
                          ("valid_to", pa.date32()), ("basis", pa.string())])
TICKER_SCHEMA = pa.schema([("ticker_key", pa.string()), ("security_id", pa.int64()), ("valid_from", pa.date32()),
                           ("valid_to", pa.date32()), ("basis", pa.string())])
LINK_SCHEMA = pa.schema([("security_id", pa.int64()), ("cik", pa.int64()), ("valid_from", pa.date32()),
                         ("valid_to", pa.date32()), ("link_tier", pa.string())])


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


@dataclass
class IdentityResolver:
    cusip_history: pa.Table
    ticker_history: pa.Table
    cik_links: pa.Table
    inputs: dict[str, str] = field(default_factory=dict)

    @classmethod
    def from_rows(cls, cusips: list[tuple], tickers: list[tuple], links: list[tuple]) -> IdentityResolver:
        """From (cusip, security_id, from, to), (ticker, security_id, from, to), (security_id, cik, from, to, tier)."""
        con = connect()
        try:
            c = pa.table({"cusip8": [r[0][:8].upper() for r in cusips], "security_id": [r[1] for r in cusips],
                          "valid_from": [r[2] for r in cusips], "valid_to": [r[3] for r in cusips],
                          "basis": ["rows"] * len(cusips)}, schema=CUSIP_SCHEMA)
            con.register("tk_in", pa.table({"t": [r[0] for r in tickers]}))
            keys = [r[0] for r in con.execute("SELECT lic_ticker_key(t) FROM tk_in").fetchall()]
        finally:
            con.close()
        t = pa.table({"ticker_key": keys, "security_id": [r[1] for r in tickers],
                      "valid_from": [r[2] for r in tickers], "valid_to": [r[3] for r in tickers],
                      "basis": ["rows"] * len(tickers)}, schema=TICKER_SCHEMA)
        link = pa.table({"security_id": [r[0] for r in links], "cik": [r[1] for r in links],
                         "valid_from": [r[2] for r in links], "valid_to": [r[3] for r in links],
                         "link_tier": [r[4] for r in links]}, schema=LINK_SCHEMA)
        return cls(c, t, link)

    @classmethod
    def from_lake(cls, root: Path | None = None) -> IdentityResolver:
        """Build the three histories from the stage lake (see module docstring for the source order)."""
        root = Path(root or os.environ.get("ATX_ALPHA_PANEL_ROOT", LAKE_ROOT))
        inputs: dict[str, str] = {}

        def note(stage_manifest: Path) -> None:
            if stage_manifest.exists():
                inputs[stage_manifest.relative_to(root).as_posix()] = _sha(stage_manifest)

        con = connect(memory="300MB")
        try:
            sm = root / "security_master"
            v3 = sm / "cusip_history.parquet"
            if v3.exists() and {"cusip", "security_id", "valid_from", "valid_to"} <= set(pq.read_schema(v3).names):
                note(sm / "manifest.json")
                csql = (f"SELECT upper(left(cusip, 8)) AS cusip8, security_id, valid_from, valid_to, "
                        f"'cusip_history' AS basis FROM read_parquet('{v3.as_posix()}') WHERE security_id IS NOT NULL")
            else:
                parts = []
                ftd = root / "ftd" / "cusip_map.parquet"
                if ftd.exists():
                    note(root / "ftd" / "manifest.json")
                    parts.append(f"SELECT upper(left(cusip, 8)), security_id, first_seen, last_seen, 'ftd' "
                                 f"FROM read_parquet('{ftd.as_posix()}') WHERE security_id IS NOT NULL")
                tf = root / "thirteenf" / "cusip_map_pit.parquet"
                if tf.exists():
                    note(root / "thirteenf" / "manifest.json")
                    parts.append(f"SELECT upper(left(cusip, 8)), security_id, CAST(min(period_q) - INTERVAL 91 DAY AS DATE), "
                                 f"max(period_q), '13f' FROM read_parquet('{tf.as_posix()}') "
                                 "WHERE security_id IS NOT NULL GROUP BY 1, 2")
                csql = ("SELECT * FROM (" + " UNION ALL ".join(parts) + ") AS c(cusip8, security_id, valid_from, "
                        "valid_to, basis)") if parts else "SELECT NULL::VARCHAR, NULL::BIGINT, NULL::DATE, NULL::DATE, NULL::VARCHAR LIMIT 0"
            tv3 = sm / "ticker_history.parquet"
            if tv3.exists() and {"ticker", "security_id", "valid_from", "valid_to"} <= set(pq.read_schema(tv3).names):
                note(sm / "manifest.json")
                tsql = (f"SELECT lic_ticker_key(ticker), security_id, valid_from, valid_to, 'ticker_history' "
                        f"FROM read_parquet('{tv3.as_posix()}') WHERE security_id IS NOT NULL")
            else:
                note(root / "prices" / "manifest.json")
                glob = (root / "prices" / "year=*" / "prices.parquet").as_posix()
                tsql = (f"SELECT lic_ticker_key(ticker), security_id, min(session_date), max(session_date), 'prices' "
                        f"FROM read_parquet('{glob}', hive_partitioning = false) "
                        f"WHERE ticker IS NOT NULL AND security_id > 0 GROUP BY 1, 2")
            lt = root / "identity" / "link_table.parquet"
            note(root / "identity" / "link_table_manifest.json")
            lsql = (f"SELECT security_id, cik, valid_from, valid_to, link_tier FROM read_parquet('{lt.as_posix()}') "
                    f"WHERE cik IS NOT NULL")
            c = con.execute(csql).arrow().read_all().rename_columns(CUSIP_SCHEMA.names).cast(CUSIP_SCHEMA)
            t = con.execute(tsql).arrow().read_all().rename_columns(TICKER_SCHEMA.names).cast(TICKER_SCHEMA)
            link = con.execute(lsql).arrow().read_all().cast(LINK_SCHEMA)
        finally:
            con.close()
        return cls(c, t, link, inputs)

    def register(self, con: duckdb.DuckDBPyConnection) -> None:
        con.register("lic_cusip", self.cusip_history)
        con.register("lic_ticker", self.ticker_history)
        con.register("lic_links", self.cik_links)

    def resolve(self, con: duckdb.DuckDBPyConnection, src: str, dst: str) -> None:
        """``dst`` = ``src`` (needs _rid, cusip, ticker, native_security_id, id_date) + security_id, link_tier, cik,
        cik_link_tier. ``register`` must have been called on ``con``."""
        w = UNDATED_WINDOW_DAYS
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _id_cand AS
            SELECT s._rid, 1 AS tier_ord, h.security_id FROM {src} s JOIN lic_cusip h
              ON h.cusip8 = upper(left(trim(s.cusip), 8)) AND s.id_date BETWEEN h.valid_from AND h.valid_to
            WHERE s.native_security_id IS NULL
            UNION ALL
            SELECT s._rid, 2, h.security_id FROM {src} s JOIN lic_cusip h
              ON h.cusip8 = upper(left(trim(s.cusip), 8))
             AND s.id_date BETWEEN h.valid_from - INTERVAL {w} DAY AND h.valid_to + INTERVAL {w} DAY
            WHERE s.native_security_id IS NULL
            UNION ALL
            SELECT s._rid, 3, h.security_id FROM {src} s JOIN lic_ticker h
              ON h.ticker_key = lic_ticker_key(s.ticker) AND s.id_date BETWEEN h.valid_from AND h.valid_to
            WHERE s.native_security_id IS NULL
        """)
        tiers = "[" + ", ".join(f"'{t}'" for t in TIER_ORDER) + "]"
        con.execute("""
            CREATE OR REPLACE TEMP TABLE _id_best AS
            WITH per AS (SELECT _rid, tier_ord, count(DISTINCT security_id) AS n, min(security_id) AS sid
                         FROM _id_cand GROUP BY 1, 2)
            SELECT _rid, arg_min(n, tier_ord) AS n, arg_min(sid, tier_ord) AS sid, min(tier_ord) AS tier_ord
            FROM per GROUP BY 1
        """)
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _id_sec AS
            SELECT s.*,
                   CASE WHEN s.native_security_id IS NOT NULL THEN s.native_security_id
                        WHEN b.n = 1 THEN b.sid END AS security_id,
                   CASE WHEN s.native_security_id IS NOT NULL THEN 'vendor_native'
                        WHEN b.n = 1 THEN {tiers}[b.tier_ord]
                        WHEN b.n > 1 THEN 'ambiguous' ELSE 'unmapped' END AS link_tier
            FROM {src} s LEFT JOIN _id_best b USING (_rid)
        """)
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE {dst} AS
            WITH c AS (
                SELECT d._rid, l.cik, l.link_tier AS cik_link_tier,
                       row_number() OVER (PARTITION BY d._rid ORDER BY l.valid_from DESC,
                           CASE l.link_tier WHEN 'strict' THEN 0 WHEN 'name' THEN 1 ELSE 2 END) AS rn
                FROM _id_sec d JOIN lic_links l ON l.security_id = d.security_id
                 AND l.valid_from <= d.id_date AND d.id_date <= l.valid_to + INTERVAL {CIK_GAP_DAYS} DAY
            )
            SELECT d.*, c.cik, c.cik_link_tier FROM _id_sec d LEFT JOIN c ON c._rid = d._rid AND c.rn = 1
        """)
