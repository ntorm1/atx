"""Earnings-call transcripts adapter: call metadata per transcript version and the text components (S8.1).

Raw layout: S&P Capital IQ Transcripts style (FactSet CallStreet and LSEG StreetEvents differ in spelling only):
``transcripts_meta*.csv`` (TRANSCRIPTID, KEYDEVID, COMPANYID, TICKER, CUSIP, EVENTTYPE, MOSTIMPORTANTDATEUTC,
TRANSCRIPTCREATIONDATETIMEUTC, TRANSCRIPTPRESENTATIONTYPENAME, FISCALYEAR, FISCALQUARTER) and
``transcripts_components*.csv`` (TRANSCRIPTID, TRANSCRIPTCOMPONENTID, COMPONENTORDER, TRANSCRIPTCOMPONENTTYPENAME,
SPEAKERTYPENAME, TRANSCRIPTPERSONNAME, COMPONENTTEXT). All vendor times are UTC.

PIT rule ``transcripts-pit-v1``: each transcript version (preliminary, edited, proofed, audited) is its own vintage
row, visible from ``greatest(version creation time, call start)``; the audio was public at the call, but the text
record is not before its creation. A version without a creation time has no knowable clock and is visible from
the file's delivery (``delivery``, ``vintage_risk``; a fixed lag after the call could backdate an audited copy made
weeks later). Components inherit their version's clocks and identifiers. Vintages of one event (KEYDEVID) must have
non-decreasing clocks in creation order.

No free substitute exists (plan section 1.2). The nearest free text is the 8-K EX-99 earnings release, which lane EVT
parses for guidance (``events/guidance.parquet``); it is a press release, not a call.
"""

from __future__ import annotations

import datetime as dt
from pathlib import Path
from typing import ClassVar

import duckdb
import pyarrow as pa

from .contract import Adapter, TableSpec, ValidationReport, csv_write, write_receipt
from .mock import MockUniverse

META = ("transcript_id", "event_id", "vendor_security_id", "ticker", "cusip", "event_type", "event_at",
        "created_at", "version", "fiscal_year", "fiscal_quarter")
META_ALIASES = {"transcriptid": "transcript_id", "keydevid": "event_id", "companyid": "vendor_security_id",
                "eventtype": "event_type", "keydeveventtypename": "event_type", "mostimportantdateutc": "event_at",
                "event_datetime_utc": "event_at", "transcriptcreationdatetimeutc": "created_at",
                "creation_datetime_utc": "created_at", "transcriptpresentationtypename": "version",
                "fiscalyear": "fiscal_year", "fiscalquarter": "fiscal_quarter", "symbol": "ticker"}
COMP_ALIASES = {"transcriptid": "transcript_id", "transcriptcomponentid": "component_id",
                "componentorder": "component_order", "transcriptcomponenttypename": "component_type",
                "speakertypename": "speaker_type", "transcriptpersonname": "speaker_name",
                "componenttext": "text"}
VERSION_RANK = {"PRELIMINARY": 1, "EDITED": 2, "PROOFED": 3, "AUDITED COPY": 4, "FINAL": 4}

CALLS = TableSpec(
    name="calls",
    payload=(pa.field("transcript_id", pa.string()), pa.field("event_id", pa.string()),
             pa.field("event_type", pa.string()), pa.field("event_at", pa.timestamp("us")),
             pa.field("version", pa.string()), pa.field("version_rank", pa.int64()),
             pa.field("fiscal_year", pa.int64()), pa.field("fiscal_quarter", pa.int64()),
             pa.field("n_components", pa.int64()), pa.field("n_words", pa.int64()), pa.field("text_sha256", pa.string())),
    key=("transcript_id",),
    series=("event_id",),
    raw_glob="transcripts_meta*.csv",
    raw_columns=META,
    aliases=META_ALIASES, stale_days=None,
    pit_rule="each version visible from greatest(creation time, call start) UTC; no creation time -> delivery",
)
COMPONENTS = TableSpec(
    name="components",
    payload=(pa.field("transcript_id", pa.string()), pa.field("event_id", pa.string()),
             pa.field("component_id", pa.string()), pa.field("component_order", pa.int64()),
             pa.field("component_type", pa.string()), pa.field("speaker_type", pa.string()),
             pa.field("speaker_name", pa.string()), pa.field("text", pa.string()), pa.field("n_words", pa.int64())),
    key=("transcript_id", "component_id"),
    series=("transcript_id", "component_id"),
    raw_glob="transcripts_components*.csv",
    raw_columns=("transcript_id", "component_id", "component_order", "component_type", "speaker_type",
                 "speaker_name", "text"),
    aliases=COMP_ALIASES, stale_days=None,
    pit_rule="inherits the clocks and identifiers of its transcript version",
)


class TranscriptsAdapter(Adapter):
    name = "transcripts"
    product = "S&P Capital IQ Transcripts (or FactSet CallStreet / LSEG StreetEvents), earnings calls, US"
    pit_rule = "transcripts-pit-v1: version creation time (not the call time) gates the text"
    purchase = ("D3 decision point 6 (lowest): earnings-call transcripts with version creation times, 2019+ (D7). "
                "Text features (tone, Q&A evasiveness) only; no parity gate depends on it.")
    tables: ClassVar[dict[str, TableSpec]] = {"calls": CALLS, "components": COMPONENTS}

    def table_sql(self, table: str, raw: str) -> str:
        if table == "calls":
            return f"""
                WITH m AS ({self._clock_sql(raw)}),
                c AS (SELECT lic_str(transcript_id) AS tid, count(*) AS n, sum(len(string_split(trim(text), ' '))) AS w,
                             sha256(string_agg(coalesce(text, ''), chr(10) ORDER BY lic_int(component_order))) AS h
                      FROM raw_components_all GROUP BY 1)
                SELECT _file, lic_str(cusip) AS cusip, lic_str(ticker) AS ticker,
                       lic_str(vendor_security_id) AS vendor_security_id, CAST(NULL AS BIGINT) AS native_security_id,
                       CAST(_ev AS DATE) AS id_date, lic_str(transcript_id) AS transcript_id,
                       lic_str(event_id) AS event_id, lic_str(event_type) AS event_type, _ev AS event_at,
                       lic_str(version) AS version, {self._rank('version')} AS version_rank,
                       lic_int(fiscal_year) AS fiscal_year, lic_int(fiscal_quarter) AS fiscal_quarter,
                       coalesce(c.n, 0) AS n_components, coalesce(c.w, 0) AS n_words, c.h AS text_sha256,
                       _snap AS vendor_snapshot_at, _rule AS rule_available_at,
                       CASE WHEN _cr IS NULL THEN 'delivery' ELSE 'vendor_pit' END AS rule_basis,
                       _cr IS NULL AS rule_vintage_risk,
                       CASE WHEN lic_str(transcript_id) IS NULL THEN 'missing_transcript_id'
                            WHEN _ev IS NULL THEN 'missing_event_time' END AS _reject
                FROM m LEFT JOIN c ON c.tid = lic_str(m.transcript_id)"""
        if table == "components":
            return f"""
                WITH m AS ({self._clock_sql('raw_calls_meta_all')})
                SELECT r._file, lic_str(m.cusip) AS cusip, lic_str(m.ticker) AS ticker,
                       lic_str(m.vendor_security_id) AS vendor_security_id, CAST(NULL AS BIGINT) AS native_security_id,
                       CAST(m._ev AS DATE) AS id_date, lic_str(r.transcript_id) AS transcript_id,
                       lic_str(m.event_id) AS event_id, lic_str(r.component_id) AS component_id,
                       lic_int(r.component_order) AS component_order, lic_str(r.component_type) AS component_type,
                       lic_str(r.speaker_type) AS speaker_type, lic_str(r.speaker_name) AS speaker_name,
                       r.text AS text, len(string_split(trim(r.text), ' ')) AS n_words,
                       m._snap AS vendor_snapshot_at, m._rule AS rule_available_at,
                       CASE WHEN m._cr IS NULL THEN 'delivery' ELSE 'vendor_pit' END AS rule_basis,
                       m._cr IS NULL AS rule_vintage_risk,
                       CASE WHEN m.transcript_id IS NULL THEN 'orphan_component'
                            WHEN lic_str(r.component_id) IS NULL THEN 'missing_component_id' END AS _reject
                FROM {raw} r LEFT JOIN m ON lic_str(m.transcript_id) = lic_str(r.transcript_id)"""
        raise KeyError(table)

    @staticmethod
    def _clock_sql(meta: str) -> str:
        """Meta rows with the version clocks: creation time, else the file's delivery (via ``lic_receipts``)."""
        return f"""
            SELECT m0.*, lic_ts(m0.event_at) AS _ev, lic_ts(m0.created_at) AS _cr,
                   CASE WHEN lic_ts(m0.created_at) IS NULL THEN greatest(lic_ts(m0.event_at), rc.delivered_at)
                        ELSE greatest(lic_ts(m0.created_at), lic_ts(m0.event_at)) END AS _rule,
                   coalesce(lic_ts(m0.created_at), greatest(lic_ts(m0.event_at), rc.delivered_at)) AS _snap
            FROM {meta} m0 LEFT JOIN lic_receipts rc ON rc.file = m0._file"""

    @staticmethod
    def _rank(col: str) -> str:
        return "CASE upper(lic_str(" + col + ")) " + " ".join(
            f"WHEN '{k}' THEN {v}" for k, v in VERSION_RANK.items()) + " END"

    def _normalize(self, con, spec, files, identity, build_time):  # type: ignore[override]
        """Both tables read both raw families: register the other family as a helper view first."""
        from .contract import aliased_select

        raw_dir = files[0].parent
        meta = sorted(raw_dir.glob(CALLS.raw_glob))
        comp = sorted(raw_dir.glob(COMPONENTS.raw_glob))
        if meta:
            con.execute("CREATE OR REPLACE TEMP VIEW raw_calls_meta_all AS "
                        + aliased_select(con, meta, CALLS.raw_columns, CALLS.aliases))
        else:
            cols = ", ".join(f"CAST(NULL AS VARCHAR) AS {c}" for c in CALLS.raw_columns)
            con.execute(f"CREATE OR REPLACE TEMP VIEW raw_calls_meta_all AS SELECT {cols}, '' AS _file LIMIT 0")
        if comp:
            con.execute("CREATE OR REPLACE TEMP VIEW raw_components_all AS "
                        + aliased_select(con, comp, COMPONENTS.raw_columns, COMPONENTS.aliases))
        else:
            cols = ", ".join(f"CAST(NULL AS VARCHAR) AS {c}" for c in COMPONENTS.raw_columns)
            con.execute(f"CREATE OR REPLACE TEMP VIEW raw_components_all AS SELECT {cols}, '' AS _file LIMIT 0")
        return super()._normalize(con, spec, files, identity, build_time)

    def extra_checks(self, con: duckdb.DuckDBPyConnection, table: str, report: ValidationReport) -> None:
        if table == "calls":
            report.add(table, "version_rank_monotone", con.execute("""
                SELECT count(*) FROM (SELECT version_rank, lag(version_rank) OVER (PARTITION BY event_id
                       ORDER BY vendor_snapshot_at) AS prev FROM t_calls) WHERE version_rank < prev""").fetchone()[0],
                fatal=False)
            report.add(table, "available_after_call", con.execute(
                "SELECT count(*) FROM t_calls WHERE available_at < event_at").fetchone()[0])

    def substitute(self) -> None:
        return None

    def mock(self, raw_dir: Path, universe: MockUniverse | None = None, seed: int = 0) -> list[Path]:
        u = universe or MockUniverse()
        raw_dir = Path(raw_dir)
        raw_dir.mkdir(parents=True, exist_ok=True)
        meta, comps = [], []
        for ln in u.lines[:6]:
            for q, (y, m, dday) in enumerate(((2024, 1, 25), (2024, 4, 25), (2024, 7, 25), (2024, 10, 24))):
                ev = dt.datetime(y, m, dday, 21, 0)  # 17:00 ET call
                d = ev.date()
                if not ln.alive(d):
                    continue
                event_id = f"{ln.security_id}{q}"
                versions = [("Preliminary", ev + dt.timedelta(hours=3)), ("Edited", ev + dt.timedelta(hours=30))]
                if q == 3 and ln is u.lines[1]:
                    versions.append(("Audited Copy", None))  # no creation time -> delivery clock
                for k, (ver, created) in enumerate(versions):
                    tid = f"{event_id}{k}"
                    meta.append([tid, event_id, f"C{ln.security_id}", ln.ticker_on(d), ln.cusip_on(d),
                                 "Earnings Calls", ev.strftime("%Y-%m-%d %H:%M:%S"),
                                 created.strftime("%Y-%m-%d %H:%M:%S") if created else None, ver, y, q + 1])
                    for j, (ctype, stype, who, text) in enumerate((
                            ("Presentation Operator Message", "Operator", "Operator", "Good afternoon and welcome."),
                            ("Presenter Speech", "Executives", "CEO", f"Revenue grew {5 + q} percent this quarter."),
                            ("Question", "Analysts", "Analyst One", "How should we think about margins?"),
                            ("Answer", "Executives", "CFO", "We expect margins to expand modestly."))):
                        comps.append([tid, f"{tid}-{j}", j + 1, ctype, stype, who, text + (" Edited." if k else "")])
        comps.append(["NOSUCH", "NOSUCH-1", 1, "Question", "Analysts", "X", "orphan"])
        mp = csv_write(raw_dir / "transcripts_meta.csv",
                       ["TRANSCRIPTID", "KEYDEVID", "COMPANYID", "TICKER", "CUSIP", "EVENTTYPE", "MOSTIMPORTANTDATEUTC",
                        "TRANSCRIPTCREATIONDATETIMEUTC", "TRANSCRIPTPRESENTATIONTYPENAME", "FISCALYEAR",
                        "FISCALQUARTER"], meta)
        cp = csv_write(raw_dir / "transcripts_components.csv",
                       ["TRANSCRIPTID", "TRANSCRIPTCOMPONENTID", "COMPONENTORDER", "TRANSCRIPTCOMPONENTTYPENAME",
                        "SPEAKERTYPENAME", "TRANSCRIPTPERSONNAME", "COMPONENTTEXT"], comps)
        for p in (mp, cp):
            write_receipt(raw_dir, p, fetched_at=dt.datetime(2026, 7, 1, 6, 0), history_mode="pit_archive",
                          url=f"mock://transcripts/{p.name}", http_status=200)
        return [mp, cp]


ADAPTER = TranscriptsAdapter()
