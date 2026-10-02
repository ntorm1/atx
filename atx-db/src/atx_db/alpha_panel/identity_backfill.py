"""Second-tier identity links: snapshot ticker links extended back over a line's final trading run.

The strict links (``identity/links.parquet``, rule r4-links-asof-v1) are point-in-time evidence
links; they leave ~40% of the 2024 top-3,000 members unlinked (Alphabet among them) because the r4
rehearsal only admits dated reconstructed evidence. This module adds a clearly labelled second tier.

Rule ``snapshot-run-backfill-v1``:
1. Candidates: r4 ``current_ticker_verified`` P/J links open at the 2026-09-20 snapshot
   (``valid_until`` NULL): the SEC's current ticker -> CIK map verified against the vendor line.
2. The line's final contiguous run: consecutive vendor sessions of the line with no gap longer than
   21 sessions, ending at or after 2026-09-01 (the line is alive at the snapshot).
3. A run day d is linked when (a) the CIK filed a periodic report (10-K/10-Q/20-F/40-F family) on or
   before d, (b) d is after the last day the strict links tie this line to a DIFFERENT CIK, and
   (c) the strict links do not already link the line on d (strict wins).
4. Primary per (cik, day) in the combined set: the strict P line if any, else the smallest
   security_id among backfill lines whose snapshot role is P, else the smallest security_id.

Caveat (survivorship): only lines alive at the snapshot are backfilled, so issuers that died
before 2026 keep strict coverage only. The panel carries ``link_tier`` so a consumer can restrict
issuer fields to strict links. ``available_at`` of a backfill link is the snapshot time.

Outputs: ``identity/links_backfill.parquet`` (backfill intervals) and
``identity/links_combined.parquet`` (strict + backfill with re-derived primary), both with columns
``security_id, cik, start, end_incl, primary, tier, basis, available_at``.
"""

from __future__ import annotations

import sys
from typing import Any

from . import common as C

RUN_GAP_SESSIONS = 21
COMBINED_FROM = "2017-10-01"  # combined/strict day expansion starts here (panel warm-up is 2018)
ALIVE_FROM = "2026-09-01"
PERIODIC = ("10-K", "10-Q", "10-KT", "10-QT", "20-F", "40-F", "10-K/A", "10-Q/A", "20-F/A", "40-F/A")


def build() -> dict[str, Any]:
    root = C.build_root()
    con = C.connect(memory="600MB", threads=1, db_file="identity_backfill.duckdb")
    links = (C.IDENTITY_R4_DIR / "phases" / "export-001" / "security_company_links.part-0000.parquet").as_posix()
    strict = (root / "identity" / "links.parquet").as_posix()
    prices = (root / "prices" / "*" / "*.parquet").as_posix()
    cal = C.calendar_path().as_posix()
    sub = (C.FSDS_DIR / "sub" / "*.parquet").as_posix()
    cf = (C.COMPANYFACTS_DIR / "batch-*.parquet").as_posix()
    forms = ", ".join(f"'{f}'" for f in PERIODIC)
    receipt: dict[str, Any] = {"rule": "snapshot-run-backfill-v1"}

    con.execute(f"""
        CREATE OR REPLACE TABLE cand AS
        SELECT perm_security_id AS security_id, CAST(cik AS BIGINT) AS cik, link_primary AS snap_primary,
               available_at
        FROM read_parquet('{links}')
        WHERE link_basis = 'current_ticker_verified' AND link_primary IN ('P', 'J') AND valid_until IS NULL
    """)
    receipt["candidates"] = con.execute("SELECT count(*), count(DISTINCT security_id), count(DISTINCT cik) FROM cand").fetchone()
    # a line mapped to two CIKs at the snapshot is ambiguous
    con.execute("DELETE FROM cand WHERE security_id IN (SELECT security_id FROM cand GROUP BY 1 HAVING count(DISTINCT cik) > 1)")
    con.execute(f"""
        CREATE OR REPLACE TABLE first_filing AS
        SELECT cik, min(filed) AS first_filed FROM (
            SELECT CAST(cik AS BIGINT) AS cik, filed FROM read_parquet('{sub}') WHERE form IN ({forms})
            UNION ALL
            SELECT CAST(cik AS BIGINT) AS cik, filed_date AS filed FROM read_parquet('{cf}')
            WHERE form IN ({forms}) AND taxonomy = 'us-gaap'
        ) GROUP BY 1
    """)
    con.execute(f"""
        CREATE OR REPLACE TABLE pres AS
        WITH c AS (SELECT session_date, row_number() OVER (ORDER BY session_date) AS sidx FROM read_parquet('{cal}')),
        p AS (
            SELECT p.security_id, p.session_date, c.sidx
            FROM read_parquet('{prices}', hive_partitioning = false) p JOIN c USING (session_date)
            WHERE p.security_id IN (SELECT security_id FROM cand)
        ),
        g AS (
            SELECT *, CASE WHEN sidx - lag(sidx) OVER (PARTITION BY security_id ORDER BY sidx) > {RUN_GAP_SESSIONS}
                           THEN 1 ELSE 0 END AS brk
            FROM p
        ),
        r AS (SELECT *, sum(brk) OVER (PARTITION BY security_id ORDER BY sidx ROWS UNBOUNDED PRECEDING) AS run_id FROM g)
        SELECT r.* FROM r
        JOIN (SELECT security_id, max(run_id) AS last_run, max(session_date) AS last_day FROM r GROUP BY 1) l
          ON r.security_id = l.security_id AND r.run_id = l.last_run
        WHERE l.last_day >= DATE '{ALIVE_FROM}'
    """)
    con.execute(f"""
        CREATE OR REPLACE TABLE other_cik_end AS
        SELECT s.security_id, max(s.end_incl) AS other_end
        FROM read_parquet('{strict}') s JOIN cand c ON s.security_id = c.security_id AND s.cik <> c.cik
        GROUP BY 1
    """)
    con.execute("""
        CREATE OR REPLACE TABLE bf_days AS
        SELECT p.security_id, p.session_date, p.sidx, c.cik, c.snap_primary, c.available_at
        FROM pres p
        JOIN cand c USING (security_id)
        JOIN first_filing f ON f.cik = c.cik AND f.first_filed <= p.session_date
        LEFT JOIN other_cik_end o ON o.security_id = p.security_id
        WHERE (o.other_end IS NULL OR p.session_date > o.other_end)
    """)
    con.execute(f"""
        DELETE FROM bf_days WHERE (security_id, session_date) IN (
            SELECT b.security_id, b.session_date FROM bf_days b JOIN read_parquet('{strict}') s
              ON s.security_id = b.security_id AND b.session_date BETWEEN s.start AND s.end_incl)
    """)
    # combined daily state over sessions: strict days + backfill days, primary re-derived
    con.execute(f"""
        CREATE OR REPLACE TABLE strict_days AS
        WITH c AS (SELECT session_date, row_number() OVER (ORDER BY session_date) AS sidx FROM read_parquet('{cal}'))
        SELECT s.security_id, c.session_date, c.sidx, s.cik, s."primary" AS strict_primary, s.tier, s.basis, s.available_at
        FROM read_parquet('{strict}') s JOIN c ON c.session_date BETWEEN s.start AND s.end_incl
        WHERE c.session_date >= DATE '{COMBINED_FROM}'
    """)
    C.copy_to_parquet(con, "SELECT security_id, session_date FROM strict_days UNION SELECT security_id, session_date FROM bf_days",
                      root / "_tmp" / "identity_days_strict_backfill.parquet")
    names_path = root / "identity" / "links_name_days.parquet"
    name_union = ""
    if names_path.exists():
        name_union = f"""
            UNION ALL
            SELECT n.security_id, n.session_date, c.sidx, n.cik, NULL, NULL, 'name', 'finra_name_match', n.available_at
            FROM read_parquet('{names_path.as_posix()}') n
            JOIN (SELECT session_date, row_number() OVER (ORDER BY session_date) AS sidx FROM read_parquet('{cal}')) c
              USING (session_date)
            ANTI JOIN strict_days s ON s.security_id = n.security_id AND s.session_date = n.session_date
            ANTI JOIN bf_days b ON b.security_id = n.security_id AND b.session_date = n.session_date"""
    receipt["name_tier"] = names_path.exists()
    con.execute(f"""
        CREATE OR REPLACE TABLE comb_days AS
        WITH u AS (
            SELECT security_id, session_date, sidx, cik, strict_primary, NULL AS snap_primary, tier, basis, available_at
            FROM strict_days
            UNION ALL
            SELECT security_id, session_date, sidx, cik, NULL, snap_primary, 'backfill', 'snapshot_run_backfill', available_at
            FROM bf_days
            {name_union}
        ),
        pk AS (
            SELECT cik, session_date,
                   coalesce(min(security_id) FILTER (WHERE strict_primary = 'P'),
                            min(security_id) FILTER (WHERE tier = 'backfill' AND snap_primary = 'P'),
                            min(security_id)) AS p_sid
            FROM u GROUP BY 1, 2
        )
        SELECT u.security_id, u.session_date, u.sidx, u.cik, u.tier, u.basis, u.available_at,
               CASE WHEN u.security_id = pk.p_sid THEN 'P' ELSE 'J' END AS "primary"
        FROM u JOIN pk USING (cik, session_date)
    """)

    def compress(table: str, dest_name: str, where: str = "TRUE") -> int:
        sql = f"""
            WITH d AS (SELECT * FROM {table} WHERE {where}),
            k AS (
                SELECT *, sidx - row_number() OVER (PARTITION BY security_id, cik, "primary", tier, basis ORDER BY sidx) AS grp
                FROM d
            )
            SELECT security_id, cik, min(session_date) AS start, max(session_date) AS end_incl, "primary", tier, basis,
                   max(available_at) AS available_at
            FROM k GROUP BY security_id, cik, "primary", tier, basis, grp
            ORDER BY security_id, start
        """
        return C.copy_to_parquet(con, sql, root / "identity" / dest_name)

    receipt["backfill_rows"] = compress("comb_days", "links_backfill.parquet", "tier = 'backfill'")
    receipt["combined_rows"] = compress("comb_days", "links_combined.parquet")
    per_year = con.execute("""
        SELECT year(session_date) y, count(*) FILTER (WHERE tier NOT IN ('backfill', 'name')) strict_days,
               count(*) FILTER (WHERE tier = 'backfill') backfill_days, count(*) FILTER (WHERE tier = 'name') name_days,
               count(DISTINCT security_id) lines
        FROM comb_days GROUP BY 1 ORDER BY 1
    """).fetchall()
    receipt["line_days_by_year"] = {str(y): {"strict": int(a), "backfill": int(b), "name": int(c), "lines": int(n)}
                                    for y, a, b, c, n in per_year}
    C.write_json_atomic(root / "identity" / "backfill_manifest.json", receipt)
    return receipt


def main() -> int:
    print(build(), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
