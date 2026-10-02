"""Stage ``identity_cover`` (S2.1): cover-page identity evidence from the SEC Financial Statement and Notes data sets.

Input: stage ``notes`` (``notes_fetch.py``): ``parts/source=*/{sub,txt_dei,num_dei,dim}.parquet``.
Output: ``identity_cover/cover_page.parquet``, one row per ``(adsh, security_key, coreg)``:

* ``security_key`` = the SEC DIM ``segments`` string of the cover fact's context (``'ClassOfStock=CommonClassA;'``;
  ``''`` when non-dimensional). Security-level dei facts (``TradingSymbol``, ``SecurityExchangeName``,
  ``Security12bTitle``, ``Security12gTitle``, ``NoTradingSymbolFlag``) and ``EntityCommonStockSharesOutstanding``
  that share a context share a row, so a cover listing several classes / securities gives several rows. A filing
  without any security-level fact keeps one row (``security_key = ''``) carrying its filing-level fields.
* ``exchange_norm``: the exchange with placeholders ('NONE', 'N/A', ...) nulled. ``instance_prefix``: the prefix of the
  filing's XBRL instance name ``<prefix>-YYYYMMDD`` (by EDGAR convention usually the ticker; heuristic evidence,
  reported separately in the coverage and never merged into ``trading_symbol``).
* ``coreg``: SEC's co-registrant name of a ``LegalEntityAxis`` context (NULL = the filer). ``entity_cik`` is the
  filer's CIK, or for a co-registrant row the ``dei:EntityCentralIndexKey`` tagged in that co-registrant's context
  (NULL when not tagged).
* Every form in the data sets is kept (10-K, 10-Q, 20-F, 40-F, 8-K, S-1, ..., amendments): 8-K covers carry the
  12(b) table too and date the ticker between periodic reports. Filter on ``form`` / ``is_amendment``.

Clock ``cover-accepted-v1``: ``available_at`` = the SUB acceptance time (``accepted_utc``, EDGAR's America/New_York
wall clock converted DST-aware); a submission without it gets ``filed 00:00 UTC + 46 h`` (``clock_basis``
``filed_plus_46h``). Nothing is backdated: a fact is known when its filing is accepted. An accession present in two
data sets is kept once (the earliest data set; ``n_data_sets``). ``vintage_risk`` is true only for rows from a
re-posted data-set file (``_N`` suffix): values are "as filed" per accession, so a regular data set carries none.

Per row: ``trading_symbol`` (trimmed as tagged), ``symbol_norm`` (upper case, whitespace removed; NULL for
placeholders such as 'N/A', 'NONE', '-'), ``security_exchange_name`` (dei enumeration as tagged: NYSE, NASDAQ,
NYSEAMER, NYSEArca, CBOE, BOX, ...), ``security_12b_title``, ``security_12g_title``, ``no_trading_symbol_flag``,
``class_member`` (the ClassOfStock member), ``exchange_member`` (EntityListingsExchange member), ``shares_outstanding``
with ``shares_outstanding_date`` (the reported cover date = SEC month-end ``ddate`` - ``datp`` days) and
``is_equity_like`` (heuristic on title and member: common / ordinary / ADS / capital stock / LP units / beneficial
interest, excluding preferred, depositary shares of preferred, warrants, rights, SPAC units and debt).

Filing-level fields repeated on each row (non-dimensional, filer context, lowest ``iprx``): ``public_float`` (dei
``EntityPublicFloat`` in USD, else SUB ``pubfloatusd``) with ``public_float_date`` and ``public_float_basis``,
``shares_outstanding_total`` (non-dimensional cover count, else the sum over ClassOfStock-only contexts),
``auditor_name``, ``auditor_location``, ``auditor_firm_id``, ``filer_category``, ``inc_state_country``,
``entity_registrant_name``, ``entity_file_number``, ``entity_tax_id``, ``address_line1``, ``address_line2``,
``address_city``, ``address_state``, ``address_zip``, ``address_country``, ``document_type``, ``shell_company``,
``small_business``, ``emerging_growth``, ``n_securities`` (rows of the filing with a symbol or a 12(b)/12(g) title).

Coverage (``coverage.json``, the S2.1 done test): per filing year from 2019, the share of CIKs that filed a
10-K/10-Q (+T, +/A) with at least one row with a real symbol (``symbol_norm``) or an exchange that year.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
from pathlib import Path
from typing import Any

from . import common as C
from . import notes_fetch as N

STAGE = "identity_cover"
SCHEMA = "atx.alpha-panel.identity-cover/v1"
CLOCK_RULE = "cover-accepted-v1"
FC1_HOURS = 46
DUCKDB_MEMORY = "300MB"                   # under a 0.6 GiB guard cap; tables live in a scratch db file
MODULES = ("cover_page", "notes_fetch", "common")
SECURITY_TAGS = ("TradingSymbol", "SecurityExchangeName", "Security12bTitle", "Security12gTitle", "NoTradingSymbolFlag")
FILING_TAGS = {
    "AuditorName": "auditor_name", "AuditorLocation": "auditor_location", "AuditorFirmId": "auditor_firm_id",
    "EntityFilerCategory": "filer_category", "EntityIncorporationStateCountryCode": "inc_state_country",
    "EntityRegistrantName": "entity_registrant_name", "EntityFileNumber": "entity_file_number",
    "EntityTaxIdentificationNumber": "entity_tax_id", "EntityAddressAddressLine1": "address_line1",
    "EntityAddressAddressLine2": "address_line2", "EntityAddressCityOrTown": "address_city",
    "EntityAddressStateOrProvince": "address_state", "EntityAddressPostalZipCode": "address_zip",
    "EntityAddressCountry": "address_country", "DocumentType": "document_type", "EntityShellCompany": "shell_company",
    "EntitySmallBusiness": "small_business", "EntityEmergingGrowthCompany": "emerging_growth",
}
PERIODIC = ("10-K", "10-Q", "10-KT", "10-QT", "10-K/A", "10-Q/A", "10-KT/A", "10-QT/A")
PLACEHOLDERS = ("N/A", "NA", "NONE", "-", "--", "---", "NOTAPPLICABLE", "NOT APPLICABLE", "NIL", "NULL", "TBD", "N.A.",
                "NOTRADINGSYMBOL", "NO TRADING SYMBOL", "")
#: title/member patterns (lower case) for ``is_equity_like``; exclusions win.
EQUITY_RE = (r"common|ordinary|american depositary|\bads\b|\badss\b|capital stock|beneficial interest|"
             r"limited partner|partnership units|common units|class [a-z] shares|\bshares\b|\bstock\b")
NON_EQUITY_RE = (r"preferred|preference|warrant|\brights?\b|\bnotes?\b|debenture|\bbonds?\b|\bdue\s+20\d\d|"
                 r"\d+(\.\d+)?\s*%|senior|subordinated")
#: units are equity only as partnership units (RE2 has no look-ahead, so two patterns)
UNIT_RE = r"\bunits?\b"
LP_RE = r"limited partner|partnership"
MEMBER_NON_EQUITY_RE = r"(?i)preferred|warrant|right|note|debenture|bond|due20|^units?"


def _symbol_norm(col: str) -> str:
    ph = ", ".join(f"'{p}'" for p in PLACEHOLDERS)
    s = f"upper(regexp_replace(trim({col}), '\\s+', '', 'g'))"
    return f"CASE WHEN {col} IS NULL OR {s} IN ({ph.replace(' ', '')}) OR upper(trim({col})) IN ({ph}) THEN NULL ELSE {s} END"


def _exchange_norm(col: str) -> str:
    """The exchange as tagged (trimmed), NULL for placeholders such as 'NONE' or 'N/A'."""
    ph = ", ".join(f"'{p}'" for p in PLACEHOLDERS)
    return f"CASE WHEN upper(trim({col})) IN ({ph}) THEN NULL ELSE nullif(trim({col}), '') END"


def _instance_prefix(col: str) -> str:
    """Upper-cased prefix of an EDGAR instance file name ``<prefix>-YYYYMMDD...`` (by convention often the ticker);
    heuristic evidence only, never merged into ``trading_symbol``."""
    return f"upper(nullif(regexp_extract(lower({col}), '^([a-z][a-z0-9.]{{0,9}})-(19|20)[0-9]{{6}}', 1), ''))"


def instance_prefix(name: str | None) -> str | None:
    """Python twin of :func:`_instance_prefix` (tested)."""
    import re

    m = re.match(r"^([a-z][a-z0-9.]{0,9})-(19|20)[0-9]{6}", (name or "").lower())
    return m.group(1).upper() if m else None


def equity_like_sql(title: str, member: str) -> str:
    """SQL twin of :func:`is_equity_like` (tested)."""
    t = f"lower(coalesce({title}, ''))"
    return (f"CASE WHEN {title} IS NOT NULL THEN regexp_matches({t}, '{EQUITY_RE}') AND NOT regexp_matches({t}, '{NON_EQUITY_RE}') "
            f"AND NOT (regexp_matches({t}, '{UNIT_RE}') AND NOT regexp_matches({t}, '{LP_RE}')) "
            f"WHEN {member} IS NOT NULL THEN NOT regexp_matches({member}, '{MEMBER_NON_EQUITY_RE}') "
            f"ELSE true END")


def is_equity_like(title: str | None, member: str | None) -> bool:
    import re

    if title is not None:
        t = title.lower()
        return (bool(re.search(EQUITY_RE, t)) and not re.search(NON_EQUITY_RE, t)
                and not (re.search(UNIT_RE, t) and not re.search(LP_RE, t)))
    if member is not None:
        return not re.search(MEMBER_NON_EQUITY_RE, member)
    return True


def axis_member(segments: str | None, axis: str) -> str | None:
    """Member of ``axis`` in a SEC DIM ``segments`` string (``'ClassOfStock=CommonClassA;'`` -> ``CommonClassA``)."""
    if not segments:
        return None
    for pair in segments.split(";"):
        if "=" in pair:
            a, m = pair.split("=", 1)
            if a == axis:
                return m
    return None


def _member_sql(col: str, axis: str) -> str:
    return f"nullif(regexp_extract({col}, '(^|;){axis}=([^;]*)', 2), '')"


def build() -> dict[str, Any]:
    t0 = time.perf_counter()
    receipt: dict[str, Any] = {"clock_rule": CLOCK_RULE}
    parts = N.parts_dir()
    nman = C.stage_dir(N.STAGE) / "manifest.json"
    if not nman.exists():
        raise RuntimeError("notes stage has no manifest: run notes_fetch finalize first")
    g = {p: (parts / "source=*" / f"{p}.parquet").as_posix() for p in ("sub", "txt_dei", "num_dei", "dim")}
    con = C.connect(memory=DUCKDB_MEMORY, threads=2, db_file="identity_cover.duckdb")
    keys = [p.name.split("=", 1)[1] for p in parts.glob("source=*")]
    con.execute("CREATE TABLE src (source VARCHAR, ord INTEGER)")
    con.executemany("INSERT INTO src VALUES (?, ?)", [(k, i) for i, k in enumerate(sorted(keys, key=N.order_key))])
    reposts = {k: v.get("repost", 0) for k, v in N.ledger().latest().items() if v.get("status") == "parsed"}
    con.execute("CREATE TABLE rep (source VARCHAR, repost INTEGER)")
    con.executemany("INSERT INTO rep VALUES (?, ?)", list(reposts.items()) or [("", 0)])
    with C.timed(receipt, "sub"):
        con.execute(f"""
            CREATE TABLE sub AS
            SELECT s.adsh, s.cik, s.form, s.period, s.fy, s.fp, s.filed, s.accepted_utc, s.pubfloatusd, s.floatdate,
                   s.instance,
                   s.source, coalesce(r.repost, 0) > 0 AS vintage_risk, count(*) OVER (PARTITION BY s.adsh) AS n_data_sets,
                   coalesce(s.accepted_utc, CAST(s.filed AS TIMESTAMP) AT TIME ZONE 'UTC' + INTERVAL {FC1_HOURS} HOUR) AS available_at,
                   CASE WHEN s.accepted_utc IS NULL THEN 'filed_plus_46h' ELSE 'accepted_utc' END AS clock_basis
            FROM read_parquet('{g['sub']}', hive_partitioning = false) s
            JOIN src USING (source) LEFT JOIN rep r USING (source)
            QUALIFY row_number() OVER (PARTITION BY s.adsh ORDER BY src.ord) = 1""")
    out = C.stage_dir(STAGE)
    tmp = C.build_root() / "_tmp" / "identity_cover_parts"
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    years = [r[0] for r in con.execute("SELECT DISTINCT year(filed) FROM sub WHERE filed IS NOT NULL ORDER BY 1").fetchall()]
    receipt["rows_per_year"] = {}
    with C.timed(receipt, "assemble"):
        for y in years:
            receipt["rows_per_year"][str(y)] = _build_year(con, parts, keys, y, tmp / f"{y}.parquet")
            print(f"identity_cover {y}: {receipt['rows_per_year'][str(y)]:,} rows", flush=True)
        receipt["rows"] = N.stitch_parquet([tmp / f"{y}.parquet" for y in years], out / "cover_page.parquet", 32768)
    shutil.rmtree(tmp, ignore_errors=True)
    dest = (out / "cover_page.parquet").as_posix()
    with C.timed(receipt, "checks"):
        receipt["checks"] = checks(con, dest)
        receipt["coverage"] = coverage(con, dest)
    C.write_json_atomic(out / "coverage.json", receipt["coverage"])
    receipt["timings_s"]["total"] = round(time.perf_counter() - t0, 1)
    payload = {
        "clock_rule": CLOCK_RULE,
        "clock_text": ("available_at = SUB accepted (EDGAR America/New_York wall clock, DST-aware) in UTC; else filed "
                       f"00:00 UTC + {FC1_HOURS} h (clock_basis). One row per (adsh, security_key, coreg)."),
        "input_manifests_sha256": {"notes": C.sha256_file(nman),
                                   **({"identity_link_table": C.sha256_file(lm)} if (lm := link_table_path().with_name(
                                       "link_table_manifest.json")).exists() else {})},
        "rules": {"is_equity_like": {"title_re": EQUITY_RE, "title_exclude_re": NON_EQUITY_RE, "unit_re": UNIT_RE, "lp_re": LP_RE,
                                     "member_exclude_re": MEMBER_NON_EQUITY_RE},
                  "symbol_placeholders": PLACEHOLDERS, "dedupe": "accession kept from the earliest data set; per "
                  "(accession, tag, context) the latest context end then lowest iprx"},
        "receipt": receipt,
    }
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)
    con.close()
    _drop_scratch("identity_cover.duckdb")
    return receipt


def _build_year(con, parts: Path, keys: list[str], y: int, dest: Path) -> int:
    """Cover rows of the accessions filed in year ``y`` (tables bounded to one year: the 0.6 GiB guard)."""
    # the data sets that supplied this year's accessions (in practice the year's own: filed dates lie in the span)
    srcs = [r[0] for r in con.execute(f"SELECT DISTINCT source FROM sub WHERE year(filed) = {y} ORDER BY 1").fetchall()]
    g_txt = [(parts / f"source={k}" / "txt_dei.parquet").as_posix() for k in srcs]
    g_num = [(parts / f"source={k}" / "num_dei.parquet").as_posix() for k in srcs]
    g_dim = [(parts / f"source={k}" / "dim.parquet").as_posix() for k in srcs]
    con.execute(f"CREATE OR REPLACE TABLE suby AS SELECT * FROM sub WHERE year(filed) = {y}")
    con.execute(f"""
        CREATE OR REPLACE TABLE dim AS
        SELECT d.dimhash, d.segments, d.source FROM read_parquet({g_dim}, hive_partitioning = false) d
        SEMI JOIN (SELECT dimh, source FROM read_parquet({g_txt}, hive_partitioning = false)
                   UNION SELECT dimh, source FROM read_parquet({g_num}, hive_partitioning = false)) u
          ON u.dimh = d.dimhash AND u.source = d.source""")
    tags = ", ".join(f"'{t}'" for t in (*SECURITY_TAGS, *FILING_TAGS, "EntityCentralIndexKey"))
    # one fact per (accession, tag, context): the latest context end, then the lowest iprx; only facts of the
    # data set that supplied the accession's SUB row
    con.execute(f"""
        CREATE OR REPLACE TABLE t AS
        SELECT x.adsh, x.tag, coalesce(x.segments, '') AS skey, x.coreg, x.value
        FROM (SELECT f.*, d.segments FROM read_parquet({g_txt}, hive_partitioning = false) f LEFT JOIN dim d ON d.dimhash = f.dimh AND d.source = f.source) x JOIN suby USING (adsh, source)
        WHERE x.tag IN ({tags})
        QUALIFY row_number() OVER (PARTITION BY x.adsh, x.tag, x.dimh, coalesce(x.coreg, '')
                                   ORDER BY x.ddate DESC, x.iprx, x.value) = 1""")
    con.execute(f"""
        CREATE OR REPLACE TABLE n AS
        SELECT x.adsh, x.tag, coalesce(x.segments, '') AS skey, x.coreg, x.value, x.uom,
               CAST(x.ddate - CAST(round(coalesce(x.datp, 0)) AS INTEGER) AS DATE) AS end_date
        FROM (SELECT f.*, d.segments FROM read_parquet({g_num}, hive_partitioning = false) f LEFT JOIN dim d ON d.dimhash = f.dimh AND d.source = f.source) x JOIN suby USING (adsh, source)
        WHERE x.tag IN ('EntityCommonStockSharesOutstanding', 'EntityPublicFloat') AND x.value IS NOT NULL
        QUALIFY row_number() OVER (PARTITION BY x.adsh, x.tag, x.dimh, coalesce(x.coreg, ''), x.uom
                                   ORDER BY x.ddate DESC, x.iprx) = 1""")
    sec_cols = ",\n".join(f"max(value) FILTER (WHERE tag = '{t}') AS {c}" for t, c in (
        ("TradingSymbol", "trading_symbol"), ("SecurityExchangeName", "security_exchange_name"),
        ("Security12bTitle", "security_12b_title"), ("Security12gTitle", "security_12g_title"),
        ("NoTradingSymbolFlag", "no_trading_symbol_raw")))
    fil_cols = ",\n".join(f"max(value) FILTER (WHERE tag = '{t}') AS {c}" for t, c in FILING_TAGS.items())
    con.execute(f"""
        CREATE OR REPLACE TABLE sec AS
        SELECT adsh, skey, coreg, {sec_cols} FROM t WHERE tag IN ({", ".join(f"'{x}'" for x in SECURITY_TAGS)})
        GROUP BY 1, 2, 3""")
    con.execute("""
        CREATE OR REPLACE TABLE sh AS
        SELECT adsh, skey, coreg, value AS shares_outstanding, end_date AS shares_outstanding_date FROM n
        WHERE tag = 'EntityCommonStockSharesOutstanding' AND uom = 'shares'""")
    con.execute(f"""
        CREATE OR REPLACE TABLE fil AS
        SELECT s.adsh, {fil_cols.replace('max(value)', 'max(t.value)').replace('WHERE tag', 'WHERE t.tag')}
        FROM suby s LEFT JOIN t ON t.adsh = s.adsh AND t.skey = '' AND t.coreg IS NULL
        GROUP BY 1""")
    con.execute("""
        CREATE OR REPLACE TABLE ecik AS
        SELECT adsh, coreg, try_cast(max(value) AS BIGINT) AS entity_cik FROM t
        WHERE tag = 'EntityCentralIndexKey' AND coreg IS NOT NULL GROUP BY 1, 2""")
    con.execute("""
        CREATE OR REPLACE TABLE shtot AS
        SELECT adsh, coalesce(max(shares_outstanding) FILTER (WHERE skey = ''),
                              sum(shares_outstanding) FILTER (WHERE regexp_full_match(skey, 'ClassOfStock=[^;]*;'))) AS shares_outstanding_total
        FROM sh WHERE coreg IS NULL GROUP BY 1""")
    con.execute("""
        CREATE OR REPLACE TABLE pf AS
        SELECT adsh, value AS public_float, end_date AS public_float_date FROM n
        WHERE tag = 'EntityPublicFloat' AND skey = '' AND coreg IS NULL AND uom = 'USD'""")
    con.execute("""
        CREATE OR REPLACE TABLE k AS
        SELECT DISTINCT adsh, skey, coreg FROM (SELECT adsh, skey, coreg FROM sec UNION ALL SELECT adsh, skey, coreg FROM sh)
        UNION ALL
        SELECT adsh, '' AS skey, CAST(NULL AS VARCHAR) AS coreg FROM suby
        WHERE adsh NOT IN (SELECT adsh FROM sec UNION SELECT adsh FROM sh)""")
    cm, em = _member_sql("k.skey", "ClassOfStock"), _member_sql("k.skey", "EntityListingsExchange")
    sql = f"""
        SELECT s.cik, k.adsh, s.form, s.form LIKE '%/A' AS is_amendment, s.period, s.fy, s.fp, s.filed,
               s.accepted_utc, s.available_at, s.clock_basis, s.source AS source_period, s.n_data_sets, s.vintage_risk,
               k.skey AS security_key, k.coreg,
               CASE WHEN k.coreg IS NULL THEN s.cik ELSE e.entity_cik END AS entity_cik,
               {cm} AS class_member, {em} AS exchange_member,
               nullif(trim(sec.trading_symbol), '') AS trading_symbol, {_symbol_norm('sec.trading_symbol')} AS symbol_norm,
               nullif(trim(sec.security_exchange_name), '') AS security_exchange_name,
               {_exchange_norm('sec.security_exchange_name')} AS exchange_norm,
               nullif(trim(sec.security_12b_title), '') AS security_12b_title,
               nullif(trim(sec.security_12g_title), '') AS security_12g_title,
               CASE lower(trim(sec.no_trading_symbol_raw)) WHEN 'true' THEN true WHEN 'false' THEN false END AS no_trading_symbol_flag,
               {equity_like_sql("coalesce(nullif(trim(sec.security_12b_title), ''), nullif(trim(sec.security_12g_title), ''))", cm)} AS is_equity_like,
               sh.shares_outstanding, sh.shares_outstanding_date, st.shares_outstanding_total,
               coalesce(pf.public_float, s.pubfloatusd) AS public_float,
               CASE WHEN pf.public_float IS NOT NULL THEN pf.public_float_date ELSE s.floatdate END AS public_float_date,
               CASE WHEN pf.public_float IS NOT NULL THEN 'dei_EntityPublicFloat'
                    WHEN s.pubfloatusd IS NOT NULL THEN 'sub_pubfloatusd' END AS public_float_basis,
               {", ".join(f"fil.{c}" for c in FILING_TAGS.values())},
               {_instance_prefix('s.instance')} AS instance_prefix
        FROM k JOIN suby s USING (adsh)
        LEFT JOIN sec ON sec.adsh = k.adsh AND sec.skey = k.skey AND sec.coreg IS NOT DISTINCT FROM k.coreg
        LEFT JOIN sh ON sh.adsh = k.adsh AND sh.skey = k.skey AND sh.coreg IS NOT DISTINCT FROM k.coreg
        LEFT JOIN ecik e ON e.adsh = k.adsh AND e.coreg = k.coreg
        LEFT JOIN shtot st ON st.adsh = k.adsh
        LEFT JOIN pf ON pf.adsh = k.adsh
        LEFT JOIN fil ON fil.adsh = k.adsh
    """
    con.execute(f"CREATE OR REPLACE TABLE cp AS {sql}")
    con.execute("""CREATE OR REPLACE TABLE nsec AS SELECT adsh, count(*) FILTER (WHERE trading_symbol IS NOT NULL
                      OR security_12b_title IS NOT NULL OR security_12g_title IS NOT NULL) AS n_securities
                    FROM cp GROUP BY 1""")
    return C.copy_to_parquet(con, """
        SELECT cp.*, nsec.n_securities FROM cp JOIN nsec USING (adsh)
        ORDER BY available_at, cik, adsh, security_key, coreg NULLS FIRST""", dest, 32768)


def checks(con, dest: str) -> dict[str, Any]:
    r = con.execute(f"""
        SELECT count(*), count(DISTINCT adsh), count(DISTINCT cik),
               count(*) FILTER (WHERE available_at IS NULL),
               count(*) FILTER (WHERE clock_basis = 'filed_plus_46h'),
               count(*) FILTER (WHERE trading_symbol IS NOT NULL), count(*) FILTER (WHERE symbol_norm IS NOT NULL),
               count(*) FILTER (WHERE exchange_norm IS NOT NULL),
               count(*) FILTER (WHERE shares_outstanding IS NOT NULL),
               count(*) FILTER (WHERE coreg IS NOT NULL), count(*) FILTER (WHERE coreg IS NOT NULL AND entity_cik IS NULL),
               count(*) FILTER (WHERE available_at < CAST(filed AS TIMESTAMP) AT TIME ZONE 'UTC' - INTERVAL 7 DAY),
               count(*) FILTER (WHERE vintage_risk), min(filed), max(filed)
        FROM read_parquet('{dest}')""").fetchone()
    names = ["rows", "accessions", "ciks", "available_at_null", "clock_filed_plus_46h", "with_trading_symbol",
             "with_symbol_norm", "with_exchange", "with_shares_outstanding", "coreg_rows", "coreg_rows_without_cik",
             "accepted_more_than_7d_before_filed", "vintage_risk_rows", "first_filed", "last_filed"]
    out = dict(zip(names, [v if not hasattr(v, "isoformat") else v.isoformat() for v in r], strict=True))
    out["dup_keys"] = con.execute(f"""SELECT count(*) - count(DISTINCT (adsh, security_key, coalesce(coreg, '')))
                                      FROM read_parquet('{dest}')""").fetchone()[0]
    out["exchanges"] = dict(con.execute(f"""SELECT coalesce(security_exchange_name, '(none)'), count(*) FROM read_parquet('{dest}')
                                            GROUP BY 1 ORDER BY 2 DESC LIMIT 20""").fetchall())
    out["forms_with_symbol"] = dict(con.execute(f"""SELECT coalesce(form, '(none)'), count(DISTINCT adsh) FROM read_parquet('{dest}')
                                                    WHERE symbol_norm IS NOT NULL GROUP BY 1 ORDER BY 2 DESC LIMIT 15""").fetchall())
    return out


def link_table_path() -> Path:
    return C.build_root() / "identity" / "link_table.parquet"


def coverage(con, dest: str) -> dict[str, Any]:
    """Per filing year: CIKs with a 10-K/10-Q (+T, +/A) that year, and the share with a dated ticker/exchange row
    (a) on their own periodic covers, (b) on any cover that year (8-K included); (c) = (b) without the CIKs whose
    only answer is ``NoTradingSymbolFlag`` = true. When ``identity/link_table.parquet`` exists, (b) is also given
    for the periodic filers linked to a vendor line during the year (``linked``) and for those whose line was ever a
    scorecard member (``member``): the traded-equity universe the link table v3 consumes."""
    per = ", ".join(f"'{f}'" for f in PERIODIC)
    lt = link_table_path()
    if lt.exists():
        linked = f"""SELECT DISTINCT y.y, l.cik, bool_or(l.ever_member) AS member
                     FROM read_parquet('{lt.as_posix()}') l, (SELECT DISTINCT year(filed) AS y FROM read_parquet('{dest}')) y
                     WHERE l.valid_from <= make_date(y.y, 12, 31) AND l.valid_to >= make_date(y.y, 1, 1) GROUP BY 1, 2"""
    else:
        linked = "SELECT CAST(NULL AS BIGINT) AS y, CAST(NULL AS BIGINT) AS cik, CAST(NULL AS BOOLEAN) AS member WHERE false"
    rows = con.execute(f"""
        WITH c AS (SELECT *, year(filed) AS y, (symbol_norm IS NOT NULL OR exchange_norm IS NOT NULL) AS hit
                   FROM read_parquet('{dest}') WHERE coreg IS NULL),
        den AS (SELECT DISTINCT y, cik FROM c WHERE form IN ({per})),
        own AS (SELECT DISTINCT y, cik FROM c WHERE form IN ({per}) AND hit),
        anyf AS (SELECT DISTINCT y, cik FROM c WHERE hit),
        nts AS (SELECT DISTINCT y, cik FROM c WHERE no_trading_symbol_flag),
        ins AS (SELECT DISTINCT y, cik FROM c WHERE hit OR instance_prefix IS NOT NULL),
        lk AS ({linked})
        SELECT d.y, count(*), count(o.cik), count(a.cik),
               count(*) FILTER (WHERE a.cik IS NULL AND n.cik IS NOT NULL),
               count(lk.cik), count(lk.cik) FILTER (WHERE a.cik IS NOT NULL),
               count(lk.cik) FILTER (WHERE lk.member), count(lk.cik) FILTER (WHERE lk.member AND a.cik IS NOT NULL),
               count(i.cik), count(lk.cik) FILTER (WHERE i.cik IS NOT NULL),
               count(lk.cik) FILTER (WHERE lk.member AND i.cik IS NOT NULL)
        FROM den d LEFT JOIN own o USING (y, cik) LEFT JOIN anyf a USING (y, cik) LEFT JOIN nts n USING (y, cik)
        LEFT JOIN lk USING (y, cik) LEFT JOIN ins i USING (y, cik)
        WHERE d.y >= 2019 GROUP BY 1 ORDER BY 1""").fetchall()
    out: dict[str, Any] = {"link_table": str(lt) if lt.exists() else None}
    for y, n, o, a, nts, nl, al, nm, am, ia, il, im in rows:
        out[str(y)] = {"periodic_filer_ciks": n, "own_periodic_cover_hit": o, "any_cover_hit": a,
                       "share_own_periodic": round(o / n, 4), "share_any_cover": round(a / n, 4),
                       "no_trading_symbol_flag_only": nts,
                       "share_any_cover_excl_no_symbol_flag": round(a / (n - nts), 4) if n > nts else None,
                       "linked_periodic_ciks": nl, "share_any_cover_linked": round(al / nl, 4) if nl else None,
                       "member_periodic_ciks": nm, "share_any_cover_member": round(am / nm, 4) if nm else None,
                       "share_with_instance_prefix": round(ia / n, 4),
                       "share_with_instance_prefix_linked": round(il / nl, 4) if nl else None,
                       "share_with_instance_prefix_member": round(im / nm, 4) if nm else None}
    return out


def _drop_scratch(db_file: str) -> None:
    for p in (C.build_root() / "_tmp" / db_file, C.build_root() / "_tmp" / (db_file + ".wal")):
        if p.exists():
            p.unlink()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("build")
    args = ap.parse_args(argv)
    if args.cmd == "build":
        rec = build()
        print(json.dumps(rec, default=str)[:6000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
