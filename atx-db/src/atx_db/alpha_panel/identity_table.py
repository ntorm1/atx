"""U1/U2 identity deliverables: the dated link table, consumer bridge exports and the identity audit.

Inputs: ``identity/links_combined.parquet`` (strict r4 + ``snapshot-run-backfill-v1`` + ``finra-name-match-v1``
intervals, one CIK per line-day), the panel core intermediates (``_tmp/panel_core``: per line-session
``adv63``, ``ticker``, ``is_operating``, ``is_index``), membership (``_tmp/panel_member``) and FINRA
short-interest ``issueName`` rows (share class).

Outputs:

* ``identity/link_table.parquet`` (U1): one row per maximal run of sessions on which a line keeps the same
  (cik, link_tier, basis, is_issuer_primary, share_class). Columns ``security_id, cik, valid_from, valid_to
  (inclusive, both sessions the line traded), sessions, link_tier (strict|name|backfill), basis,
  is_issuer_primary, share_class, available_at, evidence_at, ever_member``.
  - ``is_issuer_primary``: the issuer's linked line with the highest prior 63-session dollar volume that
    session (ties: smallest security_id). ``adv63`` uses sessions before d only, so the flag is point in time.
  - ``share_class``: vendor ticker suffix (``BRK.B`` -> ``B``), else the ``Class X`` word of the latest FINRA
    ``issueName`` of the line disseminated before the session, else ``NULL`` (single class or unknown).
  - ``available_at``: when the link could first be known. Strict: the r4 link's own clock (13 r4 links are
    known after their start; consumers gate on it). Name: the dissemination of the FINRA row whose issue name
    matched on the run's first session (each later session is known by its own earlier dissemination). Backfill: the 2026-09-20 snapshot (not point in
    time; survivorship-biased, lines alive at the snapshot only).
  - ``evidence_at``: the clock of the dated evidence the link rests on: ``available_at`` for strict and name
    links; for backfill links ``valid_from`` 22:00 UTC (the CIK had filed a periodic report and the vendor line
    had traded by then; the choice of CIK still comes from the snapshot).
* ``export/identity-bridge-v2-{strict,pit,all}/`` in the consumer contract ``atx.identity-bridge/v1``
  (``links.parquet``: ``sr_id, cik, start, end_incl, available_at, primary (P|J), tier, basis, class_status`` plus
  ``link_tier, share_class, knowledge_at``). ``strict`` = r4 tiers; ``pit`` = strict + name; ``all`` = strict + name
  + backfill with ``available_at`` = ``evidence_at`` (``knowledge_at`` keeps the true clock) so a consumer that
  gates on ``available_at`` can run the survivorship-biased sensitivity deliberately.
* ``identity/audit.json`` (request section 5.4 and U2): per year the share of member / member_equity cells by
  link tier, ambiguous line-days, dead-line coverage, the multi-class issuer list.
"""

from __future__ import annotations

import sys
from typing import Any

from . import common as C

RULE = "identity-link-table-v1"
DEAD_BEFORE = "2026-09-01"  # a line whose last vendor session is before this date died before the snapshot


def _paths() -> dict[str, str]:
    root = C.build_root()
    return {
        "combined": (root / "identity" / "links_combined.parquet").as_posix(),
        "name_days": (root / "identity" / "links_name_days.parquet").as_posix(),
        "core": (root / "_tmp" / "panel_core" / "*.parquet").as_posix(),
        "member": (root / "_tmp" / "panel_member" / "*.parquet").as_posix(),
        "si": (root / "short_interest" / "si.parquet").as_posix(),
        "si_raw": (C.FINRA_SI_DIR / "raw" / "si_*.csv").as_posix(),
        "cal": C.calendar_path().as_posix(),
    }


def build() -> dict[str, Any]:
    p = _paths()
    con = C.connect(memory="600MB", threads=1, db_file="identity_table.duckdb")
    receipt: dict[str, Any] = {"rule": RULE}
    # line-sessions the line actually traded (panel core), with the inputs of the primary and class rules
    con.execute(f"""
        CREATE OR REPLACE TABLE core AS
        SELECT session_date, security_id, ticker, adv63, is_operating, is_index
        FROM read_parquet('{p["core"]}')
    """)
    con.execute("""
        CREATE OR REPLACE TABLE days (session_date DATE, security_id BIGINT, cik BIGINT, link_tier VARCHAR,
                                      basis VARCHAR, link_available_at TIMESTAMP, adv63 DOUBLE, ticker VARCHAR)
    """)
    for year in range(2017, 2027):  # bounded interval joins, one year at a time
        con.execute(f"""
            INSERT INTO days
            SELECT c.session_date, c.security_id, l.cik,
                   CASE WHEN l.tier IN ('backfill', 'name') THEN l.tier ELSE 'strict' END,
                   l.basis, CASE WHEN l.tier = 'name' THEN coalesce(n.available_at, l.available_at) ELSE l.available_at END,
                   c.adv63, c.ticker
            FROM (SELECT * FROM core WHERE year(session_date) = {year}) c
            JOIN (SELECT * FROM read_parquet('{p["combined"]}')
                  WHERE start <= DATE '{year}-12-31' AND end_incl >= DATE '{year}-01-01') l
              ON l.security_id = c.security_id AND c.session_date BETWEEN l.start AND l.end_incl
            LEFT JOIN (SELECT * FROM read_parquet('{p["name_days"]}') WHERE year(session_date) = {year}) n
              ON n.security_id = c.security_id AND n.session_date = c.session_date AND n.cik = l.cik
        """)
    receipt["link_days"] = con.execute("SELECT count(*), count(DISTINCT security_id), count(DISTINCT cik) FROM days").fetchone()
    receipt["ambiguous_line_days"] = con.execute(
        "SELECT count(*) FROM (SELECT session_date, security_id FROM days GROUP BY 1, 2 HAVING count(DISTINCT cik) > 1)"
    ).fetchone()[0]
    # share class: ticker suffix, else the latest FINRA issueName class word disseminated before the session
    con.execute(f"""
        CREATE OR REPLACE TABLE fr AS
        SELECT DISTINCT s.security_id, s.dissemination_date,
               regexp_extract(r.issueName, 'Class ([A-Z])( |$)', 1) AS cls
        FROM read_parquet('{p["si"]}') s
        JOIN (SELECT DISTINCT symbolCode, CAST(settlementDate AS DATE) AS settlement_date, issueName
              FROM read_csv('{p["si_raw"]}', delim='|', header=true, all_varchar=true, ignore_errors=true,
                            union_by_name=true)
              WHERE issueName IS NOT NULL) r
          ON r.symbolCode = s.symbol AND r.settlement_date = s.settlement_date
        WHERE s.security_id IS NOT NULL
    """)
    con.execute("""
        CREATE OR REPLACE TABLE fr_cls AS
        SELECT security_id, dissemination_date, CASE WHEN cls = '' THEN NULL ELSE cls END AS cls FROM fr
    """)
    con.execute("""
        CREATE OR REPLACE TABLE days2 AS
        SELECT d.*,
               coalesce(CASE WHEN regexp_matches(d.ticker, '^[A-Z]+\\.[A-Z]$') THEN right(d.ticker, 1) END, f.cls) AS share_class,
               row_number() OVER (PARTITION BY d.cik, d.session_date ORDER BY d.adv63 DESC NULLS LAST, d.security_id) = 1
                   AS is_issuer_primary,
               count(*) OVER (PARTITION BY d.cik, d.session_date) AS issuer_lines
        FROM days d
        ASOF LEFT JOIN fr_cls f ON d.security_id = f.security_id AND d.session_date > f.dissemination_date
    """)
    con.execute("DROP TABLE days")
    # islands: consecutive calendar sessions of the line with an unchanged key
    con.execute(f"""
        CREATE OR REPLACE TABLE isl AS
        WITH cal AS (SELECT session_date, row_number() OVER (ORDER BY session_date) AS k
                     FROM read_parquet('{p["cal"]}')),
        x AS (
            SELECT d.*, c.k,
                   c.k - row_number() OVER (PARTITION BY d.security_id, d.cik, d.link_tier, d.basis, d.is_issuer_primary,
                                            coalesce(d.share_class, '') ORDER BY c.k) AS grp
            FROM days2 d JOIN cal c USING (session_date)
        )
        SELECT security_id, cik, link_tier, basis, is_issuer_primary, share_class,
               min(session_date) AS valid_from, max(session_date) AS valid_to, count(*) AS sessions,
               min(link_available_at) AS link_available_at, max(issuer_lines) AS max_issuer_lines
        FROM x GROUP BY security_id, cik, link_tier, basis, is_issuer_primary, share_class, grp
    """)
    con.execute(f"""
        CREATE OR REPLACE TABLE ever AS
        SELECT DISTINCT security_id FROM read_parquet('{p["member"]}') WHERE member
    """)
    root = C.build_root()
    dest = root / "identity" / "link_table.parquet"
    receipt["link_table_rows"] = C.copy_to_parquet(con, """
        SELECT i.security_id, i.cik, i.valid_from, i.valid_to, i.sessions, i.link_tier, i.basis, i.is_issuer_primary,
               i.share_class, i.max_issuer_lines,
               i.link_available_at AS available_at,
               CASE WHEN i.link_tier = 'backfill' THEN CAST(i.valid_from AS TIMESTAMP) + INTERVAL 22 HOUR
                    ELSE i.link_available_at END AS evidence_at,
               e.security_id IS NOT NULL AS ever_member
        FROM isl i LEFT JOIN ever e USING (security_id)
        ORDER BY i.security_id, i.valid_from
    """, dest)
    receipt["link_table"] = con.execute(f"""
        SELECT link_tier, count(*) AS rows, count(DISTINCT security_id) AS lines, count(DISTINCT cik) AS ciks,
               sum(sessions) AS line_sessions, count(*) FILTER (WHERE available_at > CAST(valid_from AS TIMESTAMP) + INTERVAL 22 HOUR) AS rows_known_after_start
        FROM read_parquet('{dest.as_posix()}') GROUP BY 1 ORDER BY 1
    """).fetchall()
    receipt["share_class_lines"] = con.execute(f"""
        SELECT count(DISTINCT security_id) FILTER (WHERE share_class IS NOT NULL), count(DISTINCT security_id)
        FROM read_parquet('{dest.as_posix()}')
    """).fetchone()
    receipt["exports"] = export_bridges(con, dest)
    receipt["audit"] = audit(con, p)
    con.close()
    (root / "_tmp" / "identity_table.duckdb").unlink(missing_ok=True)
    C.write_json_atomic(root / "identity" / "link_table_manifest.json", {
        "schema": "atx.alpha-panel.identity-link-table/v1", "status": "complete", "rule": RULE,
        "code": C.code_identity("identity_table", "common"),
        "files": C.output_hashes(root / "identity", "link_table.parquet"),
        "inputs": {"links_combined": C.output_hashes(root / "identity", "links_combined.parquet")},
        "rule_text": __doc__, "receipt": receipt})
    return receipt


VARIANTS = {
    "strict": ("strict",),
    "pit": ("strict", "name"),
    "all": ("strict", "name", "backfill"),
}


def export_bridges(con, table) -> dict[str, Any]:
    out: dict[str, Any] = {}
    root = C.build_root()
    for name, tiers in VARIANTS.items():
        d = root / "export" / f"identity-bridge-v2-{name}"
        d.mkdir(parents=True, exist_ok=True)
        lst = ", ".join(f"'{t}'" for t in tiers)
        avail = "evidence_at" if name == "all" else "available_at"
        n = C.copy_to_parquet(con, f"""
            SELECT security_id AS sr_id, cik, valid_from AS start, valid_to AS end_incl,
                   {avail} AS available_at,
                   CASE WHEN is_issuer_primary THEN 'P' ELSE 'J' END AS "primary",
                   link_tier AS tier, basis, coalesce(share_class, 'common') AS class_status,
                   link_tier, share_class, available_at AS knowledge_at
            FROM read_parquet('{table.as_posix()}') WHERE link_tier IN ({lst})
            ORDER BY sr_id, start
        """, d / "links.parquet")
        stats = con.execute(f"""
            SELECT count(*), count(DISTINCT sr_id), count(DISTINCT cik), min(start), max(end_incl),
                   count(*) FILTER (WHERE available_at > CAST(start AS TIMESTAMP) + INTERVAL 22 HOUR)
            FROM read_parquet('{(d / "links.parquet").as_posix()}')
        """).fetchone()
        manifest = {
            "schema": "atx.identity-bridge/v1", "status": "complete", "rule": f"{RULE}/{name}",
            "rehearsal_identity": True, "instrument_namespace": "spiderrock.securityID", "mark_utc": "22:00:00",
            "tiers_kept": list(tiers),
            "available_at_basis": ("evidence_at for backfill rows (knowledge_at = 2026-09-20 snapshot: survivorship-"
                                   "biased, not point in time); the link's own clock for strict and name rows")
                                  if name == "all" else "the link's own knowledge clock (point in time)",
            "primary_rule": "P = the issuer's linked line with the highest prior-63-session dollar volume that session "
                            "(ties: smallest security_id); intervals split where the flag changes",
            "source": {"scope_complete": False, "link_table": C.output_hashes(table.parent, table.name)},
            "counts": {"rows": stats[0], "lines": stats[1], "ciks": stats[2], "min_start": str(stats[3]),
                       "max_end_incl": str(stats[4]), "rows_available_after_start_mark": stats[5]},
            "files": C.output_hashes(d, "links.parquet"),
            "code": C.code_identity("identity_table", "common"),
        }
        C.write_json_atomic(d / "manifest.json", manifest)
        out[name] = manifest["counts"]
    return out


def audit(con, p: dict[str, str]) -> dict[str, Any]:
    """Per-year tier shares over member and member_equity cells, dead-line coverage, multi-class issuers."""
    con.execute(f"""
        CREATE OR REPLACE TABLE cells AS
        SELECT m.session_date, m.security_id, m.member, c.is_operating AND NOT c.is_index AS operating,
               d.link_tier, d.is_issuer_primary, d.cik
        FROM read_parquet('{p["member"]}') m
        JOIN core c USING (session_date, security_id)
        LEFT JOIN days2 d USING (session_date, security_id)
        WHERE m.member
    """)
    con.execute("""
        CREATE OR REPLACE TABLE lastday AS SELECT security_id, max(session_date) AS last_session FROM core GROUP BY 1
    """)
    rows = con.execute(f"""
        SELECT year(c.session_date) AS y, c.operating,
               l.last_session < DATE '{DEAD_BEFORE}' AS dead,
               count(*) AS cells,
               count(*) FILTER (WHERE link_tier = 'strict') AS strict,
               count(*) FILTER (WHERE link_tier = 'name') AS name,
               count(*) FILTER (WHERE link_tier = 'backfill') AS backfill,
               count(*) FILTER (WHERE link_tier IS NULL) AS unlinked,
               count(DISTINCT c.security_id) AS lines,
               count(DISTINCT c.security_id) FILTER (WHERE link_tier IS NOT NULL) AS lines_linked
        FROM cells c JOIN lastday l USING (security_id)
        GROUP BY ALL ORDER BY ALL
    """).fetchall()
    per_year: dict[str, Any] = {}
    for y, operating, dead, cells, strict, name, backfill, unlinked, lines, lines_linked in rows:
        slot = per_year.setdefault(str(y), {})
        key = ("member_equity" if operating else "member_non_operating") + ("_dead_lines" if dead else "_alive_lines")
        slot[key] = {"cells": cells, "strict": strict, "name": name, "backfill": backfill, "unlinked": unlinked,
                     "lines": lines, "lines_linked": lines_linked}
    for y, slot in per_year.items():
        eq = [slot.get(k) for k in ("member_equity_alive_lines", "member_equity_dead_lines") if slot.get(k)]
        tot = {k: sum(s[k] for s in eq) for k in ("cells", "strict", "name", "backfill", "unlinked", "lines",
                                                  "lines_linked")}
        slot["member_equity"] = tot
        c = max(tot["cells"], 1)
        slot["member_equity_share"] = {
            "strict": round(tot["strict"] / c, 4), "strict_or_name": round((tot["strict"] + tot["name"]) / c, 4),
            "any_tier": round((c - tot["unlinked"]) / c, 4)}
        dead = slot.get("member_equity_dead_lines")
        if dead:
            slot["dead_line_share_linked"] = {
                "cells_any_tier": round((dead["cells"] - dead["unlinked"]) / max(dead["cells"], 1), 4),
                "cells_strict_or_name": round((dead["strict"] + dead["name"]) / max(dead["cells"], 1), 4),
                "lines_any_tier": round(dead["lines_linked"] / max(dead["lines"], 1), 4)}
    multi = con.execute("""
        SELECT cik, count(DISTINCT security_id) AS lines, string_agg(DISTINCT ticker, ',') AS tickers,
               min(session_date) AS first, max(session_date) AS last
        FROM days2 WHERE issuer_lines > 1 AND session_date >= DATE '2018-01-01'
        GROUP BY cik HAVING count(DISTINCT session_date) >= 21 ORDER BY lines DESC, cik
    """).fetchall()
    root = C.build_root()
    C.copy_to_parquet(con, """
        SELECT cik, count(DISTINCT security_id) AS lines, string_agg(DISTINCT ticker, ',') AS tickers,
               min(session_date) AS first_session, max(session_date) AS last_session,
               count(DISTINCT session_date) AS multi_line_sessions
        FROM days2 WHERE issuer_lines > 1
        GROUP BY cik HAVING count(DISTINCT session_date) >= 21 ORDER BY lines DESC, cik
    """, root / "identity" / "multi_class_issuers.parquet")
    ambiguous = con.execute("""
        SELECT year(session_date), count(*) FROM (
            SELECT session_date, security_id FROM days2 GROUP BY 1, 2 HAVING count(DISTINCT cik) > 1) GROUP BY 1 ORDER BY 1
    """).fetchall()
    result = {"per_year": per_year, "multi_class_issuers": len(multi),
              "multi_class_examples": [list(map(str, r)) for r in multi[:25]],
              "ambiguous_line_days_by_year": {str(y): n for y, n in ambiguous},
              "dead_rule": f"a line is dead when its last vendor session is before {DEAD_BEFORE}"}
    C.write_json_atomic(root / "identity" / "audit.json", result)
    return {"multi_class_issuers": len(multi), "years": list(per_year)}


def main(argv: list[str] | None = None) -> int:
    r = build()
    print({k: v for k, v in r.items() if k != "audit"})
    return 0


if __name__ == "__main__":
    sys.exit(main())
