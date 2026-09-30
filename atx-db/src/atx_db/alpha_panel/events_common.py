"""Shared helpers of the ``events`` stage tables (lane EVT): issuer -> security link, member issuers, manifest.

* :func:`attach_security` adds ``security_id``, ``link_tier`` and ``link_basis`` to an event table from
  ``identity/link_table.parquet``: the line linked to the event's CIK on the event date (issuer-primary line first,
  then tier strict > backfill > name, then the longer-lived line), ``link_basis = 'on_date'``; else the nearest line
  whose validity starts or ends within :data:`LINK_TOLERANCE_DAYS` of the date (an IPO priced the day before the
  first session, a target delisted the day after completion), ``'nearest_30d'``; else NULL / ``'unlinked'``.
* ``is_member_issuer``: the CIK is linked to a line that was ever a panel ``member_equity`` line.
* :func:`publish_table` rewrites ``events/manifest.json`` publish-last: one ``tables.<name>`` entry per table (rule
  text, rows, sources with their manifest SHA-256), and the SHA-256 of every parquet file in the stage.
"""

from __future__ import annotations

import datetime as dt
from pathlib import Path
from typing import Any

from . import common as C

STAGE = "events"
SCHEMA = "atx.alpha-panel.events/v1"
LINK_TOLERANCE_DAYS = 30
TIER_RANK_SQL = "CASE link_tier WHEN 'strict' THEN 0 WHEN 'backfill' THEN 1 WHEN 'name' THEN 2 ELSE 3 END"


def link_table_path() -> Path:
    return C.build_root() / "identity" / "link_table.parquet"


def sec_stage_path(name: str) -> Path:
    return C.build_root() / "sec_filings" / name


def attach_security(con: Any, table: str, out: str, cik_col: str = "cik", date_col: str = "event_date") -> None:
    """``CREATE TABLE out AS table + (security_id, link_tier, link_basis, is_member_issuer)``."""
    lt = link_table_path().as_posix()
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _lk AS
        SELECT security_id, cik, valid_from, valid_to, link_tier, coalesce(is_issuer_primary, false) AS prim,
               coalesce(sessions, 0) AS sessions, coalesce(ever_member, false) AS ever_member
        FROM read_parquet('{lt}') WHERE cik IS NOT NULL""")
    con.execute("CREATE OR REPLACE TEMP TABLE _mem AS SELECT DISTINCT cik FROM _lk WHERE ever_member")
    con.execute(f"CREATE OR REPLACE TEMP TABLE _ev AS SELECT row_number() OVER () AS _rid, * FROM {table}")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _hit AS
        SELECT _rid, security_id, link_tier, link_basis FROM (
            SELECT e._rid, l.security_id, l.link_tier,
                   CASE WHEN CAST(e.{date_col} AS DATE) BETWEEN l.valid_from AND l.valid_to THEN 'on_date'
                        ELSE 'nearest_30d' END AS link_basis,
                   row_number() OVER (PARTITION BY e._rid ORDER BY
                       (CAST(e.{date_col} AS DATE) BETWEEN l.valid_from AND l.valid_to) DESC,
                       least(abs(date_diff('day', CAST(e.{date_col} AS DATE), l.valid_from)),
                             abs(date_diff('day', CAST(e.{date_col} AS DATE), l.valid_to))),
                       l.prim DESC, {TIER_RANK_SQL.replace('link_tier', 'l.link_tier')}, l.sessions DESC,
                       l.security_id) AS rn
            FROM _ev e JOIN _lk l ON l.cik = e.{cik_col}
             AND CAST(e.{date_col} AS DATE) BETWEEN l.valid_from - INTERVAL {LINK_TOLERANCE_DAYS} DAY
                                                AND l.valid_to + INTERVAL {LINK_TOLERANCE_DAYS} DAY
        ) WHERE rn = 1""")
    con.execute(f"""
        CREATE OR REPLACE TABLE {out} AS
        SELECT e.* EXCLUDE (_rid), h.security_id, coalesce(h.link_tier, 'unlinked') AS link_tier,
               coalesce(h.link_basis, 'unlinked') AS link_basis, (m.cik IS NOT NULL) AS is_member_issuer
        FROM _ev e LEFT JOIN _hit h USING (_rid) LEFT JOIN _mem m ON m.cik = e.{cik_col}""")
    for t in ("_lk", "_mem", "_ev", "_hit"):
        con.execute(f"DROP TABLE IF EXISTS {t}")


def input_manifests() -> dict[str, Any]:
    """SHA-256 of the input stage manifests every events table reads (plan invariant 5)."""
    out: dict[str, Any] = {}
    for stage in ("sec_filings", "earnings_calendar", "identity", "delisting", "fundamentals"):
        man = C.build_root() / stage / "manifest.json"
        if man.exists():
            out[stage] = {"path": str(man), "sha256": C.sha256_file(man)}
    lt = link_table_path()
    if lt.exists():
        out["identity/link_table.parquet"] = {"sha256": C.sha256_file(lt)}
    return out


def publish_table(name: str, modules: tuple[str, ...], payload: dict[str, Any]) -> Path:
    """Update ``events/manifest.json`` with table ``name`` (publish-last; other tables' entries are kept)."""
    directory = C.stage_dir(STAGE)
    path = directory / "manifest.json"
    tables: dict[str, Any] = {}
    if path.exists():
        try:
            tables = C.read_json(path).get("tables", {})
        except (OSError, ValueError):
            tables = {}
    tables[name] = {"published_at": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
                    "code": C.code_identity(*modules), **payload}
    manifest = {"schema": SCHEMA, "status": "complete", "stage": STAGE, "tables": tables,
                "files": C.output_hashes(directory, "*.parquet"), "input_manifests_sha256": input_manifests(),
                "staleness": "event data, no staleness",
                "clock_rule": "available_at = EDGAR acceptance (UTC) of the disclosing filing (sec_filings rule); "
                              "a row is usable at session d only if available_at < 22:00 UTC of session d-1"}
    C.write_json_atomic(path, manifest)
    return path
