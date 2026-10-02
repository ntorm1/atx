"""U3 security classification inputs: dated FINRA issue names, listing / delisting dates, directory snapshot.

The vendor file carries no security names. Two name sources exist:

* FINRA consolidated short-interest rows (twice monthly, every exchange-listed line; ``issueName`` truncated to
  30 characters, ``marketClassCode`` = listing market). Point in time: a row is known at its dissemination
  date 22:00 UTC. Classified with ``atx_db.universe_us_listed.classify_security_type`` (A2 patterns) plus the
  truncation rule below.
* The Nasdaq Trader directory snapshot (``nasdaqlisted`` / ``otherlisted``, 2026-09-18): full names and the ETF
  flag, current lines only. Not point in time (``directory_*`` columns; read only for lines still trading at
  the snapshot, as ``atx_db.research.spine.classify_lines`` does).

Outputs (stage ``security_master``):

* ``finra_names.parquet``: ``security_id, settlement_date, dissemination_date, available_at, symbol,
  issue_name, market_class, exchange, finra_type, share_class``.
* ``lines.parquet``: one row per vendor line: ``first_session, last_session, left_censored`` (first session is
  the vendor file's first date), ``delisting_date`` (last session when before 2026-09-01, else NULL),
  ``last_ticker``, ``directory_name, directory_etf, directory_type`` (snapshot, non-PIT), ``is_index_line``.

The panel joins both (as-of for the FINRA rows) and derives the per-session flags; see ``docs/ALPHA_PANEL.md``.
"""

from __future__ import annotations

import sys
from typing import Any

from . import common as C

RULE = "security-master-v1"
DEAD_BEFORE = "2026-09-01"
INDEX_ID_MIN = 1_000_000_000_000
EXCHANGE_BY_CLASS = {
    "NNM": "XNAS", "NSC": "XNAS", "NCM": "XNAS", "NGM": "XNAS", "NMS": "XNAS", "NASDAQ": "XNAS",
    "NYSE": "XNYS", "AMEX": "XASE", "ARCA": "ARCX", "BATS": "BATS", "IEXG": "IEXG", "OTC": "OTC", "OTCBB": "OTC",
}
TRUNCATED_SUFFIX = {"W": "warrant", "U": "unit", "R": "right"}


import re

# Exchange-traded products whose (often truncated) names carry no ETF / fund word: sponsor brands and leverage or
# inverse wording. Name evidence only (never the vendor earnings flags, which are unreliable).
ETP_PATTERN = re.compile(
    r"\b(PROSHARES|DIREXION|ISHARES|SPDR|INVESCO DB|POWERSHARES|VELOCITYSHARES|IPATH|E-?TRACS|MICROSECTORS|"
    r"GRANITESHARES|GLOBAL X|VANECK|WISDOMTREE|XTRACKERS|FIRST TRUST|SPROTT PHYSICAL|ABERDEEN STANDARD|"
    r"TEUCRIUM|UNITED STATES (OIL|GASOLINE|NATURAL GAS|BRENT|12 MONTH|COMMODITY)|BARCLAYS BANK PLC|"
    r"CREDIT SUISSE AG|CREDIT SUISSE FI|UBS AG|JPMORGAN CHASE FINANCIAL|INVESCO BULLETSHARES|BULLETSHARES|"
    r"DELTASHARES|INDEX-LINKED)"
    r"|\b(ULTRA ?PRO|ULTRASHORT|ULTRA SHORT|DAILY (BULL|BEAR|INVERSE|LEVERAGED)|[23]X (LONG|SHORT|INVERSE)|"
    r"(BULL|BEAR) [23]X|LEVERAGED|INVERSE|\d+X LONG|\d+X SHORT|EXCHANGE TRADED)\b")
SPAC_PATTERN = re.compile(r"\bACQUISITIO|\bCAPITAL ACQUISITION\b|\bBLANK CHECK\b|\bTONTINE\b")
# preferred-like and hybrid securities: a coupon, mandatory convertibles, tangible equity units, trust preferreds
PREFERRED_PATTERN = re.compile(r"\d+(\.\d+)?\s?%|\bMANDATORY\b|TANGIBLE EQUITY|TANGIBL|CAPITAL TRUST|CALLABLE TRUS|"
                               r"PREFERRED|\bPFD\b|DEPOSITARY SH|\bSERIES [A-Z]\b")
ADR_PATTERN = re.compile(r"AMERICAN DEPOS|AMERICAN DEP$|AMERICAN DE$|NEW YORK REGISTRY|\bADS REPRESENTING")
# symbol suffixes in the FINRA / CQS spelling (C.PRK, ACP.RT, AIG.WS, IPOC.U)
SYMBOL_SUFFIX = ((re.compile(r"\.PR[A-Z]?$|\.P[A-Z]$"), "preferred"), (re.compile(r"\.W[ST][A-Z]?$|\.W$"), "warrant"),
                 (re.compile(r"\.U[N]?$"), "unit"), (re.compile(r"\.RT$|\.R$"), "right"))
TRUNCATED_WORD = {"W": "WARRANT", "U": "UNIT", "R": "RIGHT"}


def classify_finra(name: str | None, symbol: str | None) -> str:
    """A2 name classes on a FINRA issue name; a 30-character name cut to a trailing one-letter token is resolved
    by the Nasdaq fifth-letter symbol convention (``...W`` warrant, ``...U`` unit, ``...R`` right). Names A2 leaves
    as ``common_unverified`` become ``ETF`` on an ETP sponsor / leverage pattern and ``spac`` on a blank-check
    acquisition-company pattern."""
    from ..universe_us_listed import classify_security_type

    kind = classify_security_type(name)
    text = (name or "").rstrip()
    sym = (symbol or "").strip().upper()
    for pattern, label in SYMBOL_SUFFIX:
        if pattern.search(sym):
            return label
    if kind in ("common_unverified", "common") and len(text) >= 29 and len(sym) == 5 and sym[-1] in TRUNCATED_WORD:
        last = text.split(" ")[-1].upper() if " " in text else ""
        if last and TRUNCATED_WORD[sym[-1]].startswith(last.rstrip(".")):
            return TRUNCATED_SUFFIX[sym[-1]]
    if kind == "common_unverified":
        upper = text.upper()
        if ETP_PATTERN.search(upper):
            return "ETF"
        if ADR_PATTERN.search(upper):
            return "ADR"
        if PREFERRED_PATTERN.search(upper):
            return "preferred"
        if SPAC_PATTERN.search(upper):
            return "spac"
    return kind

def share_class_of(name: str | None) -> str | None:
    import re

    m = re.search(r"\bClass ([A-Z])\b", name or "")
    return m.group(1) if m else None


def build() -> dict[str, Any]:
    from ..research.spine import read_directory, symbol_variants
    from ..universe_us_listed import classify_security_type

    root = C.build_root()
    out = C.stage_dir("security_master")
    con = C.connect(memory="350MB", threads=1)
    con.create_function("classify_finra", classify_finra, ["VARCHAR", "VARCHAR"], "VARCHAR", null_handling="special")
    con.create_function("share_class_of", share_class_of, ["VARCHAR"], "VARCHAR", null_handling="special")
    si = (root / "short_interest" / "si.parquet").as_posix()
    raw = (C.FINRA_SI_DIR / "raw" / "si_*.csv").as_posix()
    receipt: dict[str, Any] = {"rule": RULE}
    con.execute(f"""
        CREATE TABLE raw AS
        SELECT symbolCode AS symbol, CAST(settlementDate AS DATE) AS settlement_date,
               any_value(issueName) AS issue_name, any_value(marketClassCode) AS market_class
        FROM read_csv('{raw}', delim='|', header=true, all_varchar=true, ignore_errors=true, union_by_name=true)
        WHERE symbolCode IS NOT NULL AND settlementDate IS NOT NULL
        GROUP BY 1, 2
    """)
    ex = " ".join(f"WHEN '{k}' THEN '{v}'" for k, v in EXCHANGE_BY_CLASS.items())
    receipt["finra_names"] = C.copy_to_parquet(con, f"""
        SELECT s.security_id, s.settlement_date, s.dissemination_date,
               CAST(s.dissemination_date AS TIMESTAMP) + INTERVAL 22 HOUR AS available_at,
               s.symbol, r.issue_name, coalesce(r.market_class, s.market_class) AS market_class,
               CASE upper(coalesce(r.market_class, s.market_class)) {ex} ELSE NULL END AS exchange,
               classify_finra(r.issue_name, s.symbol) AS finra_type, share_class_of(r.issue_name) AS share_class
        FROM read_parquet('{si}') s LEFT JOIN raw r USING (symbol, settlement_date)
        WHERE s.security_id IS NOT NULL
        ORDER BY s.security_id, s.settlement_date
    """, out / "finra_names.parquet")
    receipt["finra_types"] = con.execute(f"""
        SELECT finra_type, count(*), count(DISTINCT security_id) FROM read_parquet('{(out / "finra_names.parquet").as_posix()}')
        GROUP BY 1 ORDER BY 2 DESC
    """).fetchall()
    prices = (root / "prices" / "*" / "*.parquet").as_posix()
    th = C.TICKERHISTORY.as_posix()
    first_file = con.execute(f"SELECT min(tradingDate), max(tradingDate) FROM read_parquet('{th}')").fetchone()
    con.execute(f"""
        CREATE TABLE lines AS
        SELECT securityID AS security_id, min(tradingDate) AS first_session, max(tradingDate) AS last_session,
               arg_max(ticker_tk, tradingDate) AS last_ticker, count(*) AS vendor_rows
        FROM read_parquet('{th}') WHERE securityID > 0 GROUP BY 1
    """)
    # directory snapshot: only for lines still trading at the snapshot (a reused symbol names a later holder)
    directory = read_directory()
    snap = [r for r in con.execute(f"""
        SELECT security_id, last_ticker FROM lines WHERE last_session >= DATE '{first_file[1]}' - INTERVAL 14 DAY
    """).fetchall()]
    rows = []
    for sid, ticker in snap:
        entry = next((directory[v] for v in symbol_variants(ticker or "") if v in directory), None)
        if entry is not None:
            rows.append((sid, entry["name"], entry["etf"] == "Y",
                         classify_security_type(entry["name"], etf=entry["etf"] == "Y",
                                                test_issue=entry["test_issue"] == "Y")))
    import pyarrow as pa

    dir_tbl = pa.table({"security_id": [r[0] for r in rows], "directory_name": [r[1] for r in rows],
                        "directory_etf": [r[2] for r in rows], "directory_type": [r[3] for r in rows]})
    con.register("dir_tbl", dir_tbl)
    receipt["lines"] = C.copy_to_parquet(con, f"""
        SELECT l.security_id, l.first_session, l.last_session, l.first_session = DATE '{first_file[0]}' AS left_censored,
               CASE WHEN l.last_session < DATE '{DEAD_BEFORE}' THEN l.last_session END AS delisting_date,
               l.last_ticker, l.vendor_rows, d.directory_name, d.directory_etf, d.directory_type,
               l.security_id >= {INDEX_ID_MIN} AS is_index_line
        FROM lines l LEFT JOIN dir_tbl d USING (security_id) ORDER BY 1
    """, out / "lines.parquet")
    receipt["directory_matched_lines"] = len(rows)
    receipt["snapshot_lines"] = len(snap)
    C.write_stage_manifest("security_master", "atx.alpha-panel.security-master/v1", ("security_master", "common"), {
        "rule": RULE, "rule_text": __doc__, "staleness": "FINRA names: as-of join, 45-day staleness like short "
        "interest; lines: static", "sources": {"tickerhistory": C.file_identity(C.TICKERHISTORY),
                                               "si": C.output_hashes(root / "short_interest", "si.parquet"),
                                               "directory": [str(p) for p in ("nasdaqlisted.txt", "otherlisted.txt")]},
        "receipt": receipt})
    return receipt


def main(argv: list[str] | None = None) -> int:
    print(build())
    return 0


if __name__ == "__main__":
    sys.exit(main())
