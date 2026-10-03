"""Ownership economics on an explicitly admitted, consistently reported cohort.

Inputs to these kernels are issuer/share-class scoped and already mapped with
dated evidence. Missing source, denominator or split evidence stays missing.
"""
from __future__ import annotations

import math
import argparse
import datetime as dt
import json
import hashlib
import calendar
from collections.abc import Mapping
from typing import Any
from pathlib import Path

from . import common as C

STAGE = "thirteenf_features"
SCHEMA = "atx.alpha-panel.thirteenf-features/v1"


def cohort_state(prior:list[dict[str,Any]],current:list[dict[str,Any]],cutoff:dt.datetime) -> dict[str,Any]:
    """R16 cohort state from originals public before cutoff, never final inventory.

    A prior-visible baseline is a known manager denominator, not the entire
    market population. Arrivals and missing outstanding reporters are explicit.
    """
    if cutoff.tzinfo is None:
        raise ValueError("cohort cutoff requires timezone")
    def originals(rows):
        result={}
        for row in rows:
            if row["form"]!="13F-HR":
                continue
            at=dt.datetime.fromisoformat(row["available_at"].replace("Z","+00:00"))
            if at.tzinfo is None:
                raise ValueError("cohort source clock requires timezone")
            if at>=cutoff:
                continue
            manager=str(int(row["filer_cik"]))
            if manager not in result or at>result[manager][0]:
                result[manager]=(at,row)
        return result
    previous,arrived=originals(prior),originals(current)
    def amendment_blocks(rows):
        blocked=set()
        lineage=[]
        for row in rows:
            at=dt.datetime.fromisoformat(row["available_at"].replace("Z","+00:00"))
            if at.tzinfo is None:
                raise ValueError("cohort source clock requires timezone")
            if at<cutoff and row["form"] in {"13F-HR/A","13F-NT","13F-NT/A"}:
                if row["form"]!="13F-HR/A" or row.get("source_qualified") is not True or row.get("amendment_type") not in {"RESTATEMENT","NEW HOLDINGS"}:
                    blocked.add(str(int(row["filer_cik"])))
                    reason=("notice_resolution_unqualified" if row["form"]!="13F-HR/A" else
                            "amendment_source_unqualified" if row.get("source_qualified") is not True else
                            "amendment_type_unresolved")
                else:
                    reason=None
                lineage.append(dict(accession=row["accession"],available_at=at.isoformat(),
                    form=row["form"],filer_cik=str(int(row["filer_cik"])),blocking_reason=reason))
        return blocked,lineage
    previous_blocked,previous_lineage=amendment_blocks(prior)
    current_blocked,current_lineage=amendment_blocks(current)
    lineage=[dict(accession=row["accession"],available_at=at.isoformat(),form=row["form"],
                  filer_cik=manager,blocking_reason=(None if row.get("qualified_original") is True
                  and row.get("source_qualified") is True else "original_source_unqualified"))
             for selected in (previous,arrived) for manager,(at,row) in selected.items()]
    lineage+=previous_lineage+current_lineage
    lineage=sorted(lineage,key=lambda row:(row["available_at"],row["accession"]))
    baseline=set(previous)
    entrants=set(arrived)-baseline
    outstanding=baseline-set(arrived)
    baseline_complete=bool(baseline) and not outstanding and all(
        previous[m][1].get("qualified_original") is True and arrived[m][1].get("qualified_original") is True
        and previous[m][1].get("source_qualified") is True and arrived[m][1].get("source_qualified") is True
        and m not in previous_blocked and m not in current_blocked
        for m in baseline)
    return {"prior_visible_baseline_manager_ids":sorted(baseline),
        "prior_visible_baseline_cohort_id":hashlib.sha256(json.dumps(sorted(baseline)).encode()).hexdigest(),
        "contemporaneous_arrived_manager_ids":sorted(arrived),"visible_entrant_manager_ids":sorted(entrants),
        "outstanding_manager_ids":sorted(outstanding),"comparison_population_managers":len(baseline),
        "prior_visible_baseline_complete":baseline_complete,
        "contemporaneous_arrived_complete":bool(arrived) and not current_blocked and all(row[1].get("qualified_original") is True and row[1].get("source_qualified") is True for row in arrived.values()),
        "retrospective_inventory_complete":None,"full_population_complete":False,
        "economic_ownership_zero_known":False,
        "source_accessions":sorted({row["accession"] for row in lineage}),"source_lineage":lineage}


def _quarter_end(date:dt.date) -> dt.date:
    month=((date.month-1)//3+1)*3
    return dt.date(date.year,month,calendar.monthrange(date.year,month)[1])


def _quarter_shift(date:dt.date,delta:int) -> dt.date:
    index=date.year*12+date.month-1+3*delta
    year,month=divmod(index,12)
    return _quarter_end(dt.date(year,month+1,1))


def build_cohort_vintages(filings:Path,input_manifest:Path,*,event_start:dt.datetime,
                         event_end:dt.datetime,max_events:int=1000) -> dict[str,Any]:
    """Native bounded actual-event metadata producer with R16 cohort lineage.

    This does not certify the contemporaneous entire-manager population. Input
    originals require explicit source qualification; unsupported rows are keyed
    diagnostics, not silently lost. Each late event also refreshes the following
    quarter's prior-baseline state, preserving earlier snapshots.
    """
    if event_start.tzinfo is None or event_end.tzinfo is None or event_start>=event_end or not 1<=max_events<=10000:
        raise ValueError("bounded timezone-aware cohort event window required")
    if C.read_json(input_manifest).get("status")!="complete":
        raise ValueError("cohort parent publication incomplete")
    input_hash=C.sha256_file(filings)
    manifest_hash=C.sha256_file(input_manifest)
    out=C.stage_dir("thirteenf_cohorts")
    con=C.connect(memory="128MB",threads=1,db_file="thirteenf_cohorts.duckdb")
    try:
        con.execute("""CREATE TABLE fil(accession VARCHAR,form VARCHAR,filer_cik VARCHAR,period_q DATE,
            available_at TIMESTAMPTZ,qualified_original BOOLEAN,source_qualified BOOLEAN,amendment_type VARCHAR,
            PRIMARY KEY(accession,available_at))""")
        con.execute("CREATE TABLE rejects(input_line BIGINT PRIMARY KEY,accession VARCHAR,reason VARCHAR,available_at TIMESTAMPTZ)")
        audit_clock=dt.datetime.now(dt.UTC)
        with filings.open(encoding="utf-8") as stream:
            for input_line,line in enumerate(stream,1):
                if not line.strip():
                    continue
                row=json.loads(line)
                try:
                    if not isinstance(row.get("available_at"),str):
                        raise ValueError("public_clock_missing")
                    clock=dt.datetime.fromisoformat(row["available_at"].replace("Z","+00:00"))
                    if clock.tzinfo is None:
                        raise ValueError("public_clock_timezone_missing")
                    report_value=row.get("report_date") or row.get("period_of_report")
                    if not report_value:
                        raise ValueError("report_date_missing")
                    report=dt.date.fromisoformat(report_value)
                    if report!=_quarter_end(report):
                        raise ValueError("report_date_cohort_inconsistent")
                    if report>clock.date():
                        raise ValueError("report_date_after_public_clock")
                    manager=str(int(row.get("filer_cik") or row["cik"]))
                    if row["form"] not in {"13F-HR","13F-HR/A","13F-NT","13F-NT/A"}:
                        raise ValueError("form_not_13f")
                    con.execute("INSERT INTO fil VALUES(?,?,?,?,?,?,?,?)",[
                        row["accession"],row["form"],manager,report,clock,
                        row.get("qualified_original") is True,row.get("source_qualified") is True,row.get("amendment_type")])
                except (KeyError,ValueError,TypeError) as exc:
                    con.execute("INSERT INTO rejects VALUES(?,?,?,?)",[input_line,row.get("accession"),str(exc),audit_clock])
        con.execute("""CREATE TABLE cohorts(period_q DATE,available_at TIMESTAMPTZ,cohort_id VARCHAR,
            prior_visible_baseline_manager_ids VARCHAR[],contemporaneous_arrived_manager_ids VARCHAR[],
            visible_entrant_manager_ids VARCHAR[],outstanding_manager_ids VARCHAR[],comparison_population_managers BIGINT,
            prior_visible_baseline_complete BOOLEAN,contemporaneous_arrived_complete BOOLEAN,
            retrospective_inventory_complete BOOLEAN,full_population_complete BOOLEAN,economic_ownership_zero_known BOOLEAN,
            source_accessions VARCHAR[],source_lineage VARCHAR,quality_status VARCHAR,PRIMARY KEY(period_q,available_at))""")
        cursor=con.cursor()
        cursor.execute("""SELECT DISTINCT available_at,period_q FROM fil WHERE available_at>=? AND available_at<?
            ORDER BY available_at,period_q LIMIT ?""",[event_start,event_end,max_events+1])
        events=cursor.fetchall()
        cursor.close()
        bounded=len(events)>max_events
        for clock,quarter in events[:max_events]:
            next_quarter=_quarter_shift(quarter,1)
            affected=[quarter]+([next_quarter] if next_quarter<=clock.date() else [])
            for period in affected:
                records=con.execute("""SELECT accession,form,filer_cik,available_at,qualified_original,source_qualified,amendment_type,period_q
                    FROM fil WHERE period_q IN(?,?) AND available_at<=? ORDER BY available_at,accession LIMIT 50001""",
                    [_quarter_shift(period,-1),period,clock]).fetchall()
                if len(records)>50000:
                    raise ValueError("cohort metadata row bound reached")
                prior,current=[],[]
                for accession,form,manager,at,qualified,source_qualified,amendment_type,row_q in records:
                    record=dict(accession=accession,form=form,filer_cik=manager,available_at=at.isoformat(),
                        qualified_original=qualified,source_qualified=source_qualified,amendment_type=amendment_type)
                    (current if row_q==period else prior).append(record)
                state=cohort_state(prior,current,clock+dt.timedelta(microseconds=1))
                con.execute("INSERT INTO cohorts VALUES("+",".join("?" for _ in range(16))+") ON CONFLICT DO NOTHING",[
                    period,clock,state["prior_visible_baseline_cohort_id"],state["prior_visible_baseline_manager_ids"],
                    state["contemporaneous_arrived_manager_ids"],state["visible_entrant_manager_ids"],state["outstanding_manager_ids"],
                    state["comparison_population_managers"],state["prior_visible_baseline_complete"],
                    state["contemporaneous_arrived_complete"],None,False,False,state["source_accessions"],json.dumps(state["source_lineage"]),"UNQUALIFIED_SOURCE_ADAPTERS"])
        if C.sha256_file(filings)!=input_hash or C.sha256_file(input_manifest)!=manifest_hash:
            raise RuntimeError("cohort source changed during build")
        rows=C.copy_to_parquet(con,"SELECT * FROM cohorts ORDER BY period_q,available_at",out / "cohorts.parquet")
        rejects=C.copy_to_parquet(con,"SELECT * FROM rejects ORDER BY accession",out / "rejected_filings.parquet")
        receipt={"rows":rows,"rejected_filings":rejects,"events_processed":min(len(events),max_events),
            "bound_reached":bounded,"event_start":event_start.isoformat(),"event_end":event_end.isoformat(),
            "gate_status":"UNQUALIFIED"}
        C.write_stage_manifest("thirteenf_cohorts","atx.alpha-panel.thirteenf-cohorts/v1",("thirteenf_features","common"),{
            "receipt":receipt,"gate_status":"UNQUALIFIED","input_evidence_sha256":input_hash,
            "input_manifests_sha256":{str(input_manifest):manifest_hash},
            "limits":"R16 baseline/arrived cohorts never certify whole population or economic zero; final source audit separate"})
        return receipt
    finally:
        con.close()


def dated_link_sql(holdings: str, links: str, cutoff: dt.datetime) -> str:
    """Keep every position and unmatched value on links known at the cutoff.

    Inputs have canonical ``available_at`` timestamps in UTC. Link validity is
    report-date validity; public availability is a separate strict condition.
    Multiple candidate security IDs never collapse to an arbitrary class.
    """
    if cutoff.tzinfo is None:
        raise ValueError("link cutoff must be timezone aware")
    h, l = [path.replace("'", "''") for path in (holdings, links)]
    at = cutoff.astimezone(dt.UTC).isoformat()
    return f"""WITH h AS (SELECT * FROM read_parquet('{h}',hive_partitioning=false)
        WHERE available_at < TIMESTAMPTZ '{at}'),
    candidates AS (
        SELECT h.accession,h.infotable_sk,count(DISTINCT l.security_id) AS n_ids,
               min(l.security_id) AS security_id,max(l.available_at) AS link_available_at,
               string_agg(DISTINCT l.basis,';') AS link_basis
        FROM h LEFT JOIN read_parquet('{l}',hive_partitioning=false) l
          ON h.cusip=l.cusip AND h.period_of_report>=l.valid_from
          AND (l.valid_to IS NULL OR h.period_of_report<=l.valid_to)
          AND l.available_at < TIMESTAMPTZ '{at}' AND l.cusip_check_ok
        GROUP BY h.accession,h.infotable_sk)
    SELECT h.*,CASE WHEN c.n_ids=1 THEN c.security_id END AS security_id,
           CASE WHEN c.n_ids=0 THEN 'dated_link_missing' WHEN c.n_ids>1 THEN 'share_class_ambiguous'
                ELSE 'dated_link_candidate' END AS link_quality,
           c.link_basis,c.link_available_at,
           CASE WHEN c.n_ids=1 THEN 0 ELSE h.value_usd END AS unmatched_value_usd,
           greatest(h.available_at,c.link_available_at) AS mapped_available_at
    FROM h JOIN candidates c USING(accession,infotable_sk)"""


def denominator_sql(mapped: str, shares: str, cutoff: dt.datetime) -> str:
    """Use source-backed line-level shares dated no later than holdings report.

    Source availability is independent of as-of date; conflicts and issuer-wide
    denominators for multiple share classes remain NULL with an explicit reason.
    """
    if cutoff.tzinfo is None:
        raise ValueError("denominator cutoff must be timezone aware")
    m, s = [path.replace("'", "''") for path in (mapped, shares)]
    at = cutoff.astimezone(dt.UTC).isoformat()
    return f"""WITH candidates AS (
        SELECT m.*,s.shrout,s.shrout_source,s.shrout_asof,s.shrout_conflict,
               s.issuer_equity_lines,s.shares_sec_line_level,s.available_at AS denominator_available_at,
               row_number() OVER(PARTITION BY m.accession,m.infotable_sk
                   ORDER BY s.session_date DESC,s.available_at DESC) AS rk
        FROM read_parquet('{m}',hive_partitioning=false) m
        LEFT JOIN read_parquet('{s}',hive_partitioning=false) s
          ON m.security_id=s.security_id AND s.session_date<=m.period_of_report
          AND s.shrout_asof<=m.period_of_report AND s.available_at<TIMESTAMPTZ '{at}')
    SELECT * EXCLUDE(rk),
        CASE WHEN shrout_source IS NOT NULL AND isfinite(shrout) AND shrout>0
             AND shrout_conflict=false AND (issuer_equity_lines=1 OR shares_sec_line_level=true)
             THEN shrout END AS denominator_shares,
        CASE WHEN shrout IS NULL OR shrout_source IS NULL THEN 'denominator_source_missing'
             WHEN shrout_conflict IS NULL OR shrout_conflict THEN 'denominator_conflict_or_unknown'
             WHEN NOT coalesce(issuer_equity_lines=1 OR shares_sec_line_level=true,false) THEN 'denominator_share_class_ambiguous'
             WHEN NOT isfinite(shrout) OR shrout<=0 THEN 'denominator_invalid'
             ELSE 'candidate_source_backed' END AS denominator_quality
    FROM candidates WHERE rk=1"""


def coverage_sql(members: str, features: str, *, staleness_days: int = 150) -> str:
    """Full benchmark denominator with strict PIT and freshest report-quarter.

    Normalized members contain session_date/security_id/cutoff for every member
    cell. A late amendment to an old quarter must not displace a newer report.
    Nonnull coverage and source-qualified coverage are reported separately.
    """
    if not 0 < staleness_days <= 365:
        raise ValueError("explicit reasonable ownership staleness required")
    m,f = [path.replace("'","''") for path in (members,features)]
    return f"""WITH j AS (
        SELECT m.*,a.period_q,a.inst_ownership,a.inst_ownership_change,a.inst_n_holders,
               a.inst_breadth_change,a.cohort_complete,a.quality_status
        FROM read_parquet('{m}',hive_partitioning=false) m
        LEFT JOIN LATERAL (
            SELECT * FROM read_parquet('{f}',hive_partitioning=false) a
            WHERE a.security_id=m.security_id AND a.available_at<m.cutoff
              AND a.period_q<=m.session_date
            ORDER BY a.period_q DESC,a.available_at DESC LIMIT 1) a ON true
        WHERE m.session_date>=DATE '2020-01-01'),
    metrics AS (
        SELECT * FROM j UNPIVOT INCLUDE NULLS(value FOR metric IN
            (inst_ownership,inst_ownership_change,inst_n_holders,inst_breadth_change)))
    SELECT year(session_date) AS year,metric,count(*) AS benchmark_cells,
        count(value) AS nonnull_cells,
        count(value) FILTER(WHERE date_diff('day',period_q,session_date)<={staleness_days}) AS fresh_nonnull_cells,
        count(value) FILTER(WHERE quality_status='QUALIFIED' AND cohort_complete
            AND date_diff('day',period_q,session_date)<={staleness_days}) AS qualified_cells,
        count(value) FILTER(WHERE value=0 AND quality_status='QUALIFIED' AND cohort_complete
            AND date_diff('day',period_q,session_date)<={staleness_days}) AS known_zero_cells,
        count(*) FILTER(WHERE period_q IS NULL) AS no_visible_feature_cells,
        count(*) FILTER(WHERE cohort_complete=false) AS incomplete_cohort_cells,
        count(*) FILTER(WHERE date_diff('day',period_q,session_date)>{staleness_days}) AS stale_cells
    FROM metrics GROUP BY 1,2 ORDER BY 1,2"""


def measure_coverage(specification: Path, features: Path, output: Path) -> dict[str,Any]:
    """Diagnostic coverage on independently pinned complete benchmark key sets.

    Membership specs name actual and expected key files with independent hashes.
    Duplicate/null/missing/extra date-security keys reject the whole measurement;
    denominator counts never shrink to available observations.
    """
    if output.exists():
        raise ValueError("coverage output exists; use a new evidence path")
    spec=C.read_json(specification)
    names={"member_equity","univ_all_common","univ_r3000p"}
    if set(spec)!=names:
        raise ValueError("all three explicit benchmark memberships required")
    feature_hash=C.sha256_file(features)
    spec_hash=C.sha256_file(specification)
    con=C.connect(memory="128MB",threads=1,db_file="thirteenf_coverage.duckdb")
    reports={}
    identities={}
    try:
        for name in sorted(names):
            item=spec[name]
            member,expected=Path(item["path"]),Path(item["expected_keys"])
            for label,path in (("path",member),("expected_keys",expected)):
                if C.sha256_file(path)!=item[label+"_sha256"]:
                    raise ValueError("membership source identity mismatch")
                identities[f"{name}:{label}"]={"path":str(path),"sha256":item[label+"_sha256"]}
            actual_sql="read_parquet('"+member.as_posix().replace("'","''")+"',hive_partitioning=false)"
            expected_sql="read_parquet('"+expected.as_posix().replace("'","''")+"',hive_partitioning=false)"
            for relation in (actual_sql,expected_sql):
                fields={r[0]:r[1] for r in con.execute(f"DESCRIBE SELECT * FROM {relation}").fetchall()}
                if fields.get("session_date")!="DATE" or fields.get("security_id") not in {"BIGINT","INTEGER","UBIGINT"}:
                    raise ValueError("membership keys require typed date/integer axes")
                if con.execute(f"SELECT count(*) FROM {relation}").fetchone()[0]==0:
                    raise ValueError("benchmark key population is empty")
                invalid=con.execute(f"""SELECT count(*) FROM (
                    SELECT session_date,security_id,count(*) n FROM {relation}
                    GROUP BY 1,2 HAVING session_date IS NULL OR security_id IS NULL OR count(*)!=1)""").fetchone()[0]
                if invalid:
                    raise ValueError("membership null or duplicate keys")
            missing=con.execute(f"""SELECT count(*) FROM (
                (SELECT session_date,security_id FROM {actual_sql} EXCEPT SELECT session_date,security_id FROM {expected_sql})
                UNION ALL
                (SELECT session_date,security_id FROM {expected_sql} EXCEPT SELECT session_date,security_id FROM {actual_sql}))""").fetchone()[0]
            if missing:
                raise ValueError("membership expected key set mismatch")
            if con.execute(f"SELECT count(*) FROM {actual_sql} WHERE cutoff IS NULL").fetchone()[0]:
                raise ValueError("membership cutoff missing")
            fields={r[0]:r[1] for r in con.execute(f"DESCRIBE SELECT * FROM {actual_sql}").fetchall()}
            if fields.get("cutoff")!="TIMESTAMP WITH TIME ZONE":
                raise ValueError("membership cutoff requires timezone-aware timestamp")
            if item.get("cutoff_policy") not in {"daily_previous_session_close", "monthly_month_end_close"}:
                raise ValueError("declared daily/monthly cutoff policy required")
            calendar_path=Path(item["calendar"])
            calendar_hash=item["calendar_sha256"]
            if C.sha256_file(calendar_path)!=calendar_hash:
                raise ValueError("calendar identity mismatch")
            identities[f"{name}:calendar"]={"path":str(calendar_path),"sha256":calendar_hash}
            calendar_sql="read_parquet('"+calendar_path.as_posix().replace("'","''")+"',hive_partitioning=false)"
            calendar_fields={r[0]:r[1] for r in con.execute(f"DESCRIBE SELECT * FROM {calendar_sql}").fetchall()}
            if calendar_fields.get("session_date")!="DATE" or calendar_fields.get("previous_session_close")!="TIMESTAMP WITH TIME ZONE":
                raise ValueError("pinned calendar requires typed session and previous close")
            if con.execute(f"""SELECT count(*) FROM (SELECT session_date FROM {calendar_sql}
                GROUP BY 1 HAVING session_date IS NULL OR count(*)!=1)""").fetchone()[0]:
                raise ValueError("calendar null or duplicate sessions")
            if item["cutoff_policy"]=="daily_previous_session_close":
                invalid=con.execute(f"""SELECT count(*) FROM {actual_sql} m LEFT JOIN {calendar_sql} c USING(session_date)
                    WHERE c.previous_session_close IS NULL OR m.cutoff!=c.previous_session_close
                    OR CAST(c.previous_session_close AT TIME ZONE 'UTC' AS DATE)>=m.session_date
                    OR CAST(c.previous_session_close AT TIME ZONE 'UTC' AS TIME)!=TIME '22:00:00'""").fetchone()[0]
            else:
                invalid=con.execute(f"""SELECT count(*) FROM {actual_sql} m
                    WHERE m.session_date!=(SELECT max(c.session_date) FROM {calendar_sql} c
                        WHERE date_trunc('month',c.session_date)=date_trunc('month',m.session_date))
                    OR m.cutoff!=(CAST(m.session_date AS TIMESTAMP)+INTERVAL 22 HOUR) AT TIME ZONE 'UTC'
                    OR NOT EXISTS(SELECT 1 FROM {calendar_sql} c
                        WHERE date_trunc('month',c.session_date)=date_trunc('month',m.session_date))""").fetchone()[0]
            if invalid:
                raise ValueError("membership cutoff differs from pinned previous session close or declared month-end close")
            query=coverage_sql(member.as_posix(),features.as_posix())
            rows=con.execute(query).fetchall()
            columns=[r[0] for r in con.description]
            reports[name]=[dict(zip(columns,row)) for row in rows]
        for identity in identities.values():
            if C.sha256_file(Path(identity["path"]))!=identity["sha256"]:
                raise RuntimeError("membership source changed during coverage")
        if C.sha256_file(features)!=feature_hash or C.sha256_file(specification)!=spec_hash:
            raise RuntimeError("coverage feature inputs changed")
        receipt={"schema":"atx.thirteenf-feature-coverage/v1","reports":reports,
            "input_sha256":{"features":feature_hash,"specification":spec_hash},
            "benchmark_sources":identities,"staleness_days":150,"gate_status":"UNQUALIFIED",
            "limits":"Membership publication/source-economic admission must be independently qualified"}
        C.write_json_atomic(output,receipt)
        return receipt
    finally:
        con.close()


def ownership_snapshot(current: Mapping[str, float], previous: Mapping[str, float] | None, *,
                       current_managers: set[str], previous_managers: set[str] | None,
                       cohort_complete: bool, previous_complete: bool,
                       denominator_shares: float | None,
                       prior_to_current_split: float | None,
                       denominator_available: bool,
                       manager_types: Mapping[str, str] | None = None,
                       manager_styles: Mapping[str, str] | None = None,
                       population_complete: bool = False,
                       table_absence_qualified: bool = False,
                       previous_table_absence_qualified: bool = False) -> dict[str, Any]:
    """Known absences become zero only within a complete reporting cohort.

    A cohort controls all contributors, including managers with no position in
    this security. New filers cannot masquerade as purchases. Common-manager
    changes remain diagnostic; full-cohort changes require identical membership.
    """
    for positions, cohort in ((current, current_managers), (previous or {}, previous_managers or set())):
        if not set(positions).issubset(cohort):
            raise ValueError("position manager outside reporting cohort")
        if any(not math.isfinite(v) or v < 0 for v in positions.values()):
            raise ValueError("long SH positions require finite nonnegative shares")
    shares = sum(current.values())
    holders = sum(v > 0 for v in current.values())
    denominator_ok = (denominator_available and denominator_shares is not None
                      and math.isfinite(denominator_shares) and denominator_shares > 0)
    current_absences_ok=set(current)==current_managers or table_absence_qualified
    level_ok=cohort_complete and population_complete and current_absences_ok
    ratio = shares / denominator_shares if level_ok and denominator_ok else None
    result: dict[str, Any] = {
        "inst_shares": shares if level_ok else None,
        "inst_n_holders": holders if level_ok else None,
        "inst_ownership": ratio,
        "ownership_above_one": ratio > 1 if ratio is not None else None,
        "observed_shares": shares, "contributing_managers": len(current_managers),
        "cohort_complete": cohort_complete,
        "population_complete":population_complete,
        "reported_holding_zero":cohort_complete and table_absence_qualified and shares==0,
        "observed_n_holders":holders,
        "inst_hhi": sum((v / shares) ** 2 for v in current.values()) if level_ok and shares else None,
        "inst_top5_share": sum(sorted(current.values(), reverse=True)[:5]) / shares if level_ok and shares else None,
        "inst_shares_change": None, "inst_breadth_change": None,
        "inst_ownership_change": None,
        "inst_new_positions": None, "inst_closed_positions": None,
        "inst_persistent_holders": None, "common_manager_share_change": None,
        "change_reason": None,
    }
    if manager_types is not None:
        by_type: dict[str, float] = {}
        for manager, value in current.items():
            label = manager_types.get(manager, "unknown")
            by_type[label] = by_type.get(label, 0.0) + value
        result["shares_by_filer_type"] = by_type if level_ok else None
        result["observed_shares_by_filer_type"]=by_type
    if manager_styles is not None:
        style = {"active": 0.0, "passive": 0.0, "unknown": 0.0}
        for manager, value in current.items():
            label = manager_styles.get(manager, "unknown")
            if label not in style:
                raise ValueError("manager style requires explicit active/passive/unknown evidence")
            style[label] += value
        result["shares_by_manager_style"] = style if level_ok else None
        result["observed_shares_by_manager_style"]=style
    split_ok = (prior_to_current_split is not None and math.isfinite(prior_to_current_split)
                and prior_to_current_split > 0)
    if previous is None or previous_managers is None:
        result["change_reason"] = "prior_cohort_missing"
        return result
    common = current_managers & previous_managers
    result["common_managers"] = len(common)
    prior_absences_ok=set(previous)==previous_managers or previous_table_absence_qualified
    absences_ok=current_absences_ok and prior_absences_ok
    if split_ok and cohort_complete and previous_complete and absences_ok:
        result["common_manager_share_change"] = sum(
            current.get(m, 0) - previous.get(m, 0) * prior_to_current_split for m in common)
    if not cohort_complete or not previous_complete:
        result["change_reason"] = "incomplete_reporting_cohort"
    elif current_managers != previous_managers:
        result["change_reason"] = "reporting_cohort_changed"
    elif not absences_ok:
        result["change_reason"]="reported_holding_absence_unqualified"
    elif not split_ok:
        result["change_reason"] = "split_evidence_missing"
    else:
        prior = sum(previous.values()) * prior_to_current_split
        result.update(inst_shares_change=shares - prior,
            inst_ownership_change=(shares - prior) / denominator_shares if denominator_ok else None,
            inst_breadth_change=holders - sum(v > 0 for v in previous.values()),
            inst_new_positions=sum(current.get(m, 0) > 0 and previous.get(m, 0) == 0 for m in common),
            inst_closed_positions=sum(current.get(m, 0) == 0 and previous.get(m, 0) > 0 for m in common),
            inst_persistent_holders=sum(current.get(m, 0) > 0 and previous.get(m, 0) > 0 for m in common))
    return result


def build(snapshots: Path, bindings: Path) -> dict[str, Any]:
    """Publish normalized source-qualified vintage snapshots with pinned inputs.

    Binding JSON must explicitly name stable final universe and fundamentals
    v11 manifests and every listed output identity. No default/latest bindings.
    This stage remains unqualified until native source adapters and independent
    mapping/cohort evidence are admitted.
    """
    config = C.read_json(bindings)
    required = {"universe", "fundamentals_v11", "thirteenf_quality"}
    if not required.issubset(config):
        raise ValueError("explicit final universe, fundamentals_v11 and thirteenf_quality bindings required")
    for name in required:
        source = config[name]
        manifest = Path(source["manifest"])
        if C.sha256_file(manifest) != source["sha256"]:
            raise ValueError(f"{name} manifest identity mismatch")
        if C.read_json(manifest).get("status") != "complete":
            raise ValueError(f"{name} producer publication incomplete")
        if not source.get("outputs"):
            raise ValueError(f"{name} final output identities required")
        for output in source["outputs"]:
            path = Path(output["path"])
            if path.stat().st_size != output["bytes"] or C.sha256_file(path) != output["sha256"]:
                raise ValueError(f"{name} output identity mismatch: {path}")
    input_hash = C.sha256_file(snapshots)
    binding_hash = C.sha256_file(bindings)
    out = C.stage_dir(STAGE)
    con = C.connect(memory="128MB", threads=1, db_file="thirteenf_features.duckdb")
    try:
        con.execute("""CREATE TABLE features (security_id BIGINT, period_q DATE, available_at TIMESTAMPTZ,
            inst_shares DOUBLE, inst_n_holders BIGINT, inst_ownership DOUBLE,
            inst_shares_change DOUBLE, inst_ownership_change DOUBLE, inst_breadth_change BIGINT, inst_new_positions BIGINT,
            inst_closed_positions BIGINT, inst_persistent_holders BIGINT, inst_hhi DOUBLE,
            inst_top5_share DOUBLE, ownership_above_one BOOLEAN, observed_shares DOUBLE,
            contributing_managers BIGINT, cohort_complete BOOLEAN, change_reason VARCHAR,
            common_manager_share_change DOUBLE, source_accessions VARCHAR[],
            denominator_shares DOUBLE, shares_by_filer_type VARCHAR, shares_by_manager_style VARCHAR,
            quality_status VARCHAR,population_complete BOOLEAN,reported_holding_zero BOOLEAN,
            observed_n_holders BIGINT,comparison_cohort_id VARCHAR,comparison_population_managers BIGINT,
            prior_visible_baseline_complete BOOLEAN,contemporaneous_arrived_complete BOOLEAN,
            retrospective_inventory_complete BOOLEAN,
            PRIMARY KEY(security_id, period_q, available_at))""")
        rows = 0
        with snapshots.open(encoding="utf-8") as stream:
            for line in stream:
                if not line.strip():
                    continue
                row = json.loads(line)
                clock = dt.datetime.fromisoformat(row["available_at"].replace("Z", "+00:00"))
                if clock.tzinfo is None:
                    raise ValueError("snapshot clock requires timezone")
                for evidence in row["evidence"]:
                    at = dt.datetime.fromisoformat(evidence["available_at"].replace("Z", "+00:00"))
                    if at.tzinfo is None or at > clock:
                        raise ValueError("snapshot uses future or undated evidence")
                if not row["evidence"] or not row["source_accessions"]:
                    raise ValueError("snapshot source lineage required")
                values = ownership_snapshot(row["current"], row.get("previous"),
                    current_managers=set(row["current_managers"]),
                    previous_managers=set(row["previous_managers"]) if row.get("previous_managers") is not None else None,
                    cohort_complete=row["cohort_complete"], previous_complete=row["previous_complete"],
                    denominator_shares=row["denominator_shares"],
                    prior_to_current_split=row["prior_to_current_split"],
                    denominator_available=row["denominator_available"],
                    manager_types=row.get("manager_types"), manager_styles=row.get("manager_styles"),
                    population_complete=row.get("full_population_complete",False),
                    table_absence_qualified=row.get("table_absence_qualified",False),
                    previous_table_absence_qualified=row.get("previous_table_absence_qualified",False))
                columns = ["inst_shares", "inst_n_holders", "inst_ownership", "inst_shares_change", "inst_ownership_change", "inst_breadth_change",
                    "inst_new_positions", "inst_closed_positions", "inst_persistent_holders", "inst_hhi", "inst_top5_share",
                    "ownership_above_one", "observed_shares", "contributing_managers", "cohort_complete",
                    "change_reason", "common_manager_share_change"]
                payload = [row["security_id"], row["period_q"], clock] + [values[k] for k in columns]
                payload += [row["source_accessions"], row["denominator_shares"] if row["denominator_available"] else None,
                            json.dumps(values["shares_by_filer_type"]) if values.get("shares_by_filer_type") is not None else None,
                            json.dumps(values["shares_by_manager_style"]) if values.get("shares_by_manager_style") is not None else None,
                            "UNQUALIFIED_NORMALIZED_ADAPTERS",values["population_complete"],values["reported_holding_zero"],
                            values["observed_n_holders"],
                            hashlib.sha256(json.dumps(sorted(row.get("previous_managers") or [])).encode()).hexdigest(),
                            len(row.get("previous_managers") or []),row.get("prior_visible_baseline_complete",False),
                            row.get("contemporaneous_arrived_complete",False),None]
                con.execute("INSERT INTO features VALUES (" + ",".join("?" for _ in payload) + ")", payload)
                rows += 1
        if C.sha256_file(snapshots) != input_hash or C.sha256_file(bindings) != binding_hash:
            raise RuntimeError("snapshot inputs changed during build")
        # Reverify producer files to reject overlapping upstream rewrites (R9).
        for name in required:
            source = config[name]
            if C.sha256_file(Path(source["manifest"])) != source["sha256"]:
                raise RuntimeError("bound producer changed during build")
            for output in source["outputs"]:
                path = Path(output["path"])
                if path.stat().st_size != output["bytes"] or C.sha256_file(path) != output["sha256"]:
                    raise RuntimeError("bound output changed during build")
        C.copy_to_parquet(con, "SELECT * FROM features ORDER BY security_id,period_q,available_at", out / "features.parquet")
        receipt = {"rows": rows, "gate_status": "UNQUALIFIED", "input_evidence_sha256": input_hash}
        C.write_stage_manifest(STAGE, SCHEMA, ("thirteenf_features", "common"), {
            "receipt": receipt, "gate_status": "UNQUALIFIED", "bindings_sha256": binding_hash,
            "input_manifests_sha256": {k: config[k]["sha256"] for k in required},
            "limits": "Native source admission, dated mapping/denominator adapters and coverage outstanding"})
        return receipt
    finally:
        con.close()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshots", type=Path)
    parser.add_argument("--bindings", type=Path)
    parser.add_argument("--coverage-members",type=Path)
    parser.add_argument("--features",type=Path)
    parser.add_argument("--coverage-output",type=Path)
    parser.add_argument("--cohort-filings",type=Path)
    parser.add_argument("--input-manifest",type=Path)
    parser.add_argument("--event-start",type=dt.datetime.fromisoformat)
    parser.add_argument("--event-end",type=dt.datetime.fromisoformat)
    parser.add_argument("--max-events",type=int,default=1000)
    args = parser.parse_args(argv)
    cohort_options=bool(args.input_manifest or args.event_start or args.event_end or args.max_events!=1000)
    if args.cohort_filings:
        if not (args.input_manifest and args.event_start and args.event_end) or any((args.snapshots,args.bindings,args.coverage_members,args.features,args.coverage_output)):
            parser.error("cohort mode requires explicit filing/manifest/event window only")
        receipt=build_cohort_vintages(args.cohort_filings,args.input_manifest,event_start=args.event_start,
                                      event_end=args.event_end,max_events=args.max_events)
        print(json.dumps(receipt,sort_keys=True))
        return 2 if receipt["bound_reached"] else 0
    if args.coverage_members:
        if not args.features or not args.coverage_output or args.snapshots or args.bindings or cohort_options:
            parser.error("coverage needs features/output and no build inputs")
        receipt=measure_coverage(args.coverage_members,args.features,args.coverage_output)
        print(json.dumps(receipt,sort_keys=True))
        return 0
    if not args.snapshots or not args.bindings or args.features or args.coverage_output or cohort_options:
        parser.error("build requires snapshots and explicit bindings")
    print(json.dumps(build(args.snapshots, args.bindings), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
