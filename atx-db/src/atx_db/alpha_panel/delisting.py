"""U4: terminal events and delisting returns for every line whose vendor history ends before the snapshot.

Population: vendor lines with a last session L in 2018-01-02..2026-08-31 (``security_master/lines.parquet``
``delisting_date``). For each line:

* **Continuation** (not a terminal event, rule of the consumer's T33a study): the same vendor ticker appears on a
  different securityID within 30 calendar days after L, or another line linked to the same CIK trades more than 5
  sessions after L (a share-class or listing successor), unless an 8-K Item 1.03 falls in the window.
* **Cause** (rule R of T33a, first match wins), with the CIK linked at L (``identity/link_table.parquet``, any tier):
  1. 8-K Item 1.03 in [L-60, L+30] -> ``performance`` (bankruptcy);
  2. 8-K Item 2.01 or 5.01 in [L-60, L+30], or a target-side merger form (DEFM14A, DEFM14C, PREM14A, PREM14C,
     SC 14D9, SC TO-T, SC 13E3, 425) in [L-365, L+30] -> ``mna``;
  3. 8-K Item 3.01 or an issuer Form 25 in [L-60, L+30] -> ``performance`` (a bare 25-NSE is ``unknown``);
  4. a non-common line -> ``non_common`` (ETP liquidation at NAV, trust redemption, unit separation: the last close
     stands): the latest FINRA name before L classifies it as ETF / fund / ETN (including the ETP sponsor and
     leverage patterns of ``security_master``), unit / warrant / right, preferred, note or blank-check SPAC. The
     vendor earnings flags are not used (owner ruling 2026-09-28: unreliable);
  5. otherwise ``unknown`` (``unknown_unlinked`` when no CIK is linked at L).
* **Delisting return** ``dlret`` (from the last vendor close to the proceeds; imputed, never observed: atx-db has
  no post-delisting price source): ``mna`` and ``non_common`` 0; ``performance`` Shumway (1997) -0.30 for NYSE /
  NYSE American / Arca / Cboe listings and Shumway-Warther (1999) -0.55 for Nasdaq listings (listing market from
  the last FINRA row); ``unknown`` NULL (the consumer chooses; ``dlret_if_performance`` gives the Shumway value).
* ``available_at``: the later of the first session after L (22:00 UTC) and the classifying filing's acceptance.

Output ``delisting/events.parquet``; coverage per year of termination in the manifest.
"""

from __future__ import annotations

import sys
from typing import Any

from . import common as C

RULE = "delisting-rule-r-v1"
MERGER_FORMS = ("DEFM14A", "DEFM14C", "PREM14A", "PREM14C", "SC 14D9", "SC TO-T", "SC 13E3", "425")
NON_COMMON = ("ETF", "fund", "ETN", "unit", "warrant", "right", "preferred", "note")
SHUMWAY_NYSE = -0.30
SHUMWAY_NASDAQ = -0.55


def has_item(items: str | None, item: str) -> bool:
    """Whole-token 8-K item match ("2.01" in "2.01,9.01" but not in "12.01")."""
    import re

    return bool(items) and re.search(r"(^|[^0-9.])" + re.escape(item) + r"([^0-9]|$)", items) is not None


def build() -> dict[str, Any]:
    root = C.build_root()
    con = C.connect(memory="500MB", threads=1, db_file="delisting.duckdb")
    con.create_function("has_item", has_item, ["VARCHAR", "VARCHAR"], "BOOLEAN", null_handling="special")
    sm = root / "security_master"
    filings = (root / "sec_filings" / "filings.parquet").as_posix()
    links = (root / "identity" / "link_table.parquet").as_posix()
    th = C.TICKERHISTORY.as_posix()
    cal = C.calendar_path().as_posix()
    con.execute(f"""
        CREATE TABLE term AS
        SELECT l.security_id, l.last_session AS last_session, l.last_ticker,
               (SELECT min(session_date) FROM read_parquet('{cal}') c WHERE c.session_date > l.last_session) AS next_session
        FROM read_parquet('{(sm / 'lines.parquet').as_posix()}') l
        WHERE l.delisting_date IS NOT NULL AND l.last_session >= DATE '2018-01-02' AND NOT l.is_index_line
    """)
    con.execute(f"""
        CREATE TABLE tk AS
        SELECT DISTINCT securityID AS security_id, upper(trim(ticker_tk)) AS ticker, tradingDate AS d
        FROM read_parquet('{th}') WHERE securityID > 0 AND tradingDate >= DATE '2018-01-01' AND ticker_tk IS NOT NULL
    """)
    con.execute(f"""
        CREATE TABLE t2 AS
        SELECT t.*, k.cik, k.link_tier,
               f.finra_type, f.exchange,
               EXISTS (SELECT 1 FROM tk WHERE tk.ticker = upper(trim(t.last_ticker)) AND tk.security_id <> t.security_id
                       AND tk.d > t.last_session AND tk.d <= t.last_session + INTERVAL 30 DAY) AS ticker_reused_30d,
               EXISTS (SELECT 1 FROM read_parquet('{links}') o WHERE o.cik = k.cik AND o.security_id <> t.security_id
                       AND o.valid_to > t.last_session + INTERVAL 7 DAY AND o.valid_from <= t.last_session + INTERVAL 30 DAY)
                   AS cik_successor
        FROM term t
        LEFT JOIN read_parquet('{links}') k
          ON k.security_id = t.security_id AND t.last_session BETWEEN k.valid_from AND k.valid_to
        ASOF LEFT JOIN (SELECT security_id, dissemination_date, finra_type, exchange
                        FROM read_parquet('{(sm / 'finra_names.parquet').as_posix()}')) f
          ON t.security_id = f.security_id AND t.last_session >= f.dissemination_date
    """)
    forms = ", ".join(f"'{f}'" for f in MERGER_FORMS)
    con.execute(f"""
        CREATE TABLE ev AS
        SELECT t.security_id, s.form, s.items, s.filing_date, s.acceptance_utc,
               date_diff('day', t.last_session, s.filing_date) AS dd
        FROM t2 t JOIN read_parquet('{filings}') s ON s.cik = t.cik
        WHERE s.filing_date BETWEEN t.last_session - INTERVAL 365 DAY AND t.last_session + INTERVAL 30 DAY
          AND (s.form IN ('8-K', '8-K/A', '25', '25-NSE') OR s.form IN ({forms}))
    """)
    con.execute("""
        CREATE TABLE cls AS
        WITH w AS (
            SELECT security_id,
                   min(acceptance_utc) FILTER (WHERE form LIKE '8-K%' AND dd >= -60 AND has_item(items, '1.03')) AS a_bk,
                   min(acceptance_utc) FILTER (WHERE (form LIKE '8-K%' AND dd >= -60 AND (has_item(items, '2.01') OR has_item(items, '5.01')))
                                               OR form NOT IN ('8-K', '8-K/A', '25', '25-NSE')) AS a_mna,
                   min(acceptance_utc) FILTER (WHERE dd >= -60 AND ((form LIKE '8-K%' AND has_item(items, '3.01')) OR form = '25'))
                       AS a_perf,
                   count(*) FILTER (WHERE form = '25-NSE' AND dd >= -60) AS n_25nse
            FROM ev GROUP BY 1
        )
        SELECT t.*, w.a_bk, w.a_mna, w.a_perf, coalesce(w.n_25nse, 0) AS n_25nse,
               (t.ticker_reused_30d OR t.cik_successor) AND w.a_bk IS NULL AS continued,
               CASE WHEN w.a_bk IS NOT NULL THEN 'performance'
                    WHEN w.a_mna IS NOT NULL THEN 'mna'
                    WHEN w.a_perf IS NOT NULL THEN 'performance'
                    WHEN t.finra_type IN ('ETF', 'fund', 'ETN', 'unit', 'warrant', 'right', 'preferred', 'note', 'spac') THEN 'non_common'
                    WHEN t.cik IS NULL THEN 'unknown_unlinked'
                    ELSE 'unknown' END AS cause,
               CASE WHEN w.a_bk IS NOT NULL THEN 'item_1.03' WHEN w.a_mna IS NOT NULL THEN 'item_2.01_5.01_or_merger_form'
                    WHEN w.a_perf IS NOT NULL THEN 'item_3.01_or_form_25'
                    WHEN t.finra_type IN ('ETF', 'fund', 'ETN', 'unit', 'warrant', 'right', 'preferred', 'note', 'spac') THEN 'finra_name_type'
                    ELSE NULL END AS cause_basis
        FROM t2 t LEFT JOIN w USING (security_id)
    """)
    out = C.stage_dir("delisting")
    n = C.copy_to_parquet(con, f"""
        SELECT security_id, last_session, last_ticker, cik, link_tier, finra_type, exchange, continued,
               security_id IN (SELECT security_id FROM read_parquet('{(root / "_tmp" / "panel_member" / "*.parquet").as_posix()}')
                               WHERE member) AS ever_member,
               ticker_reused_30d, cik_successor, cause, cause_basis, n_25nse,
               CASE WHEN continued THEN NULL
                    WHEN cause IN ('mna', 'non_common') THEN 0.0
                    WHEN cause = 'performance' THEN CASE WHEN exchange = 'XNAS' THEN {SHUMWAY_NASDAQ} ELSE {SHUMWAY_NYSE} END
                    END AS dlret,
               CASE WHEN exchange = 'XNAS' THEN {SHUMWAY_NASDAQ} ELSE {SHUMWAY_NYSE} END AS dlret_if_performance,
               greatest(CAST(next_session AS TIMESTAMP) + INTERVAL {C.MARK_HOUR_UTC} HOUR,
                        coalesce(CASE cause WHEN 'performance' THEN coalesce(a_bk, a_perf) WHEN 'mna' THEN a_mna END,
                                 CAST(next_session AS TIMESTAMP) + INTERVAL {C.MARK_HOUR_UTC} HOUR)) AS available_at,
               true AS dlret_imputed
        FROM cls ORDER BY last_session, security_id
    """, out / "events.parquet")
    per_year = con.execute(f"""
        SELECT year(last_session), ever_member, continued, cause, count(*)
        FROM read_parquet('{(out / 'events.parquet').as_posix()}') GROUP BY ALL ORDER BY ALL
    """).fetchall()
    con.close()
    (root / "_tmp" / "delisting.duckdb").unlink(missing_ok=True)
    receipt = {"rule": RULE, "rows": n, "per_year": [list(map(str, r)) for r in per_year]}
    C.write_stage_manifest("delisting", "atx.alpha-panel.delisting/v1", ("delisting", "common"), {
        "rule": RULE, "rule_text": __doc__, "staleness": "event data",
        "sources": {"filings": C.output_hashes(root / "sec_filings", "filings.parquet"),
                    "link_table": C.output_hashes(root / "identity", "link_table.parquet"),
                    "security_master": C.output_hashes(sm, "*.parquet")}, "receipt": receipt})
    return receipt


def main() -> int:
    print(build())
    return 0


if __name__ == "__main__":
    sys.exit(main())
