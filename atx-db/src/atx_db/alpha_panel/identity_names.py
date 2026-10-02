"""Third-tier identity links for lines no strict or backfill link covers: FINRA issue name -> SEC filer.

Rule ``finra-name-match-v1`` (point in time):
1. For each FINRA short-interest row mapped to a security_id (stage S), take the settlement's
   ``issueName`` (FINRA truncates it to 30 characters). The name is public on the dissemination date.
2. Stem both names: lower case, ``&`` -> ``and``, punctuation removed; the FINRA name is cut at the
   first security-class token (class, common, ordinary, ordinary shares, ads, adr, units, warrant,
   preferred, depositary, shares, cl, com, ...); trailing corporate-form tokens (inc, corp, co, ltd, plc,
   llc, lp, sa, nv, ag, se, the, ...) are dropped from both, repeatedly.
3. SEC names: FSDS SUB conformed names as printed on periodic reports the CIK filed within the 450
   days up to the dissemination date (filed <= dissemination); renames are covered by the per-filing name.
4. A match is an equal stem, or, when the FINRA name was truncated (30 characters) and its stem has at
   least 12 characters, an SEC stem that starts with it. It is accepted only when exactly one CIK matches.
5. The link applies to sessions d with dissemination < d <= dissemination + 45 days, until the next
   settlement of the line takes over, and only on days without a strict or backfill link.

Output: ``identity/links_name_days.parquet`` (security_id, session_date, cik, available_at).
"""

from __future__ import annotations

import re
import sys
from typing import Any

from . import common as C

CLASS_TOKENS = {
    "class", "cl", "common", "commo", "comm", "com", "cmn", "ordinary", "ord", "ads", "adr", "ade", "units", "unit",
    "warrant", "warrants", "wt", "wts", "preferred", "pfd", "depositary", "depository", "shares", "share", "shs",
    "stock", "stk", "sponsored", "spon", "representing", "rights", "rt", "notes", "note", "series", "ser",
    "subordinate", "subordinated", "voting", "nonvoting",
}
FORM_TOKENS = {
    "inc", "incorporated", "corp", "corporation", "co", "company", "ltd", "limited", "plc", "llc", "lp", "l", "p",
    "sa", "nv", "ag", "se", "the", "de", "del", "holdings", "holding", "group", "hldgs", "hldg", "companies", "cos",
    "international", "intl", "ltda", "spa", "asa", "ab", "oyj", "bhd", "tbk", "kk",
}
PERIODIC = ("10-K", "10-Q", "10-KT", "10-QT", "20-F", "40-F", "10-K/A", "10-Q/A", "20-F/A", "40-F/A")


def _clean(name: str | None) -> list[str]:
    if not name:
        return []
    s = name.lower().replace("&", " and ")
    s = re.sub(r"[^a-z0-9 ]+", " ", s)
    return s.split()


def stem_finra(name: str | None) -> str:
    toks = _clean(name)
    for i, t in enumerate(toks):
        if t in CLASS_TOKENS and i > 0:
            toks = toks[:i]
            break
    while toks and toks[-1] in FORM_TOKENS:
        toks.pop()
    return " ".join(toks)


def stem_sec(name: str | None) -> str:
    toks = _clean(name)
    # SEC conformed names may carry a state suffix like "/de/" -> tokens "de"
    while toks and toks[-1] in FORM_TOKENS:
        toks.pop()
    return " ".join(toks)


def build() -> dict[str, Any]:
    root = C.build_root()
    con = C.connect(memory="600MB", threads=2, db_file="identity_names.duckdb")
    con.create_function("stem_finra", stem_finra, ["VARCHAR"], "VARCHAR")
    con.create_function("stem_sec", stem_sec, ["VARCHAR"], "VARCHAR")
    raw_glob = (C.FINRA_SI_DIR / "raw" / "si_*.csv").as_posix()
    si = (root / "short_interest" / "si.parquet").as_posix()
    sub = (C.FSDS_DIR / "sub" / "*.parquet").as_posix()
    combined_days = root / "_tmp" / "identity_days_strict_backfill.parquet"
    receipt: dict[str, Any] = {"rule": "finra-name-match-v1"}
    forms = ", ".join(f"'{f}'" for f in PERIODIC)

    con.execute(f"""
        CREATE OR REPLACE TABLE fr AS
        SELECT DISTINCT symbolCode AS symbol, CAST(settlementDate AS DATE) AS settlement_date, issueName AS issue_name
        FROM read_csv('{raw_glob}', delim='|', header=true, all_varchar=true, ignore_errors=true, union_by_name=true)
        WHERE symbolCode IS NOT NULL AND issueName IS NOT NULL AND settlementDate IS NOT NULL
    """)
    con.execute(f"""
        CREATE OR REPLACE TABLE rows AS
        SELECT s.security_id, s.settlement_date, s.dissemination_date, fr.issue_name,
               stem_finra(fr.issue_name) AS fstem, length(fr.issue_name) >= 30 AS truncated
        FROM read_parquet('{si}') s JOIN fr USING (symbol, settlement_date)
        WHERE s.security_id IS NOT NULL
    """)
    receipt["finra_rows"] = con.execute("SELECT count(*), count(DISTINCT security_id) FROM rows").fetchone()
    con.execute(f"""
        CREATE OR REPLACE TABLE secnames AS
        WITH f AS (
            SELECT CAST(cik AS BIGINT) AS cik, name, former, changed, filed FROM read_parquet('{sub}') WHERE form IN ({forms})
        )
        SELECT DISTINCT cik, stem_sec(name) AS sstem, filed FROM f
    """)
    # candidate matches, point in time: SEC filing on or before dissemination, within 450 days
    # stem-level candidate pairs first (equi-joins on distinct stems), then the dated filter
    con.execute("""
        CREATE OR REPLACE TABLE fst AS
        SELECT DISTINCT fstem, truncated FROM rows WHERE length(fstem) >= 4
    """)
    con.execute("""
        CREATE OR REPLACE TABLE pairs AS
        SELECT DISTINCT f.fstem, f.truncated, n.sstem FROM fst f JOIN (SELECT DISTINCT sstem FROM secnames) n ON n.sstem = f.fstem
        UNION
        SELECT DISTINCT f.fstem, f.truncated, n.sstem
        FROM (SELECT * FROM fst WHERE truncated AND length(fstem) >= 12) f
        JOIN (SELECT DISTINCT sstem, substr(sstem, 1, 12) AS k FROM secnames WHERE length(sstem) >= 12) n
          ON n.k = substr(f.fstem, 1, 12) AND starts_with(n.sstem, f.fstem)
    """)
    con.execute("""
        CREATE OR REPLACE TABLE m AS
        SELECT r.security_id, r.settlement_date, r.dissemination_date, r.issue_name, r.fstem, n.cik
        FROM rows r
        JOIN pairs p ON p.fstem = r.fstem AND p.truncated = r.truncated
        JOIN secnames n ON n.sstem = p.sstem
        WHERE n.filed <= r.dissemination_date AND n.filed > r.dissemination_date - INTERVAL 450 DAY
    """)
    con.execute("""
        CREATE OR REPLACE TABLE uniq AS
        SELECT security_id, settlement_date, dissemination_date, any_value(cik) AS cik, any_value(issue_name) AS issue_name
        FROM (SELECT DISTINCT security_id, settlement_date, dissemination_date, cik, issue_name FROM m)
        GROUP BY 1, 2, 3 HAVING count(DISTINCT cik) = 1
    """)
    receipt["settlement_matches"] = con.execute("SELECT count(*), count(DISTINCT security_id), count(DISTINCT cik) FROM uniq").fetchone()
    cal = C.calendar_path().as_posix()
    con.execute(f"""
        CREATE OR REPLACE TABLE days AS
        WITH s AS (
            SELECT u.*, lead(dissemination_date) OVER (PARTITION BY security_id ORDER BY settlement_date) AS next_dissem
            FROM uniq u
        )
        SELECT s.security_id, c.session_date, s.cik, CAST(s.dissemination_date AS TIMESTAMP) + INTERVAL 22 HOUR AS available_at,
               s.issue_name
        FROM s JOIN read_parquet('{cal}') c
          ON c.session_date > s.dissemination_date AND c.session_date <= s.dissemination_date + INTERVAL 45 DAY
         AND (s.next_dissem IS NULL OR c.session_date <= s.next_dissem)
        SEMI JOIN (SELECT security_id, session_date FROM read_parquet('{(root / "prices" / "*" / "*.parquet").as_posix()}',
                                                                      hive_partitioning = false)) px
          ON px.security_id = s.security_id AND px.session_date = c.session_date
    """)
    if combined_days.exists():
        con.execute(f"""
            DELETE FROM days WHERE (security_id, session_date) IN
              (SELECT security_id, session_date FROM read_parquet('{combined_days.as_posix()}'))
        """)
    rows = C.copy_to_parquet(con, "SELECT security_id, session_date, cik, available_at, issue_name FROM days",
                             root / "identity" / "links_name_days.parquet")
    receipt["name_link_days"] = rows
    receipt["sample"] = [list(map(str, r)) for r in con.execute(
        "SELECT DISTINCT security_id, cik, issue_name FROM days ORDER BY random() LIMIT 25").fetchall()]
    C.write_json_atomic(root / "identity" / "name_manifest.json", receipt)
    return receipt


def main() -> int:
    print(build(), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
