"""Ownership economics on an explicitly admitted, consistently reported cohort.

Inputs to these kernels are issuer/share-class scoped and already mapped with
dated evidence. Missing source, denominator or split evidence stays missing.
"""
from __future__ import annotations

import math
import argparse
import datetime as dt
import json
from collections.abc import Mapping
from typing import Any
from pathlib import Path

from . import common as C

STAGE = "thirteenf_features"
SCHEMA = "atx.alpha-panel.thirteenf-features/v1"


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


def ownership_snapshot(current: Mapping[str, float], previous: Mapping[str, float] | None, *,
                       current_managers: set[str], previous_managers: set[str] | None,
                       cohort_complete: bool, previous_complete: bool,
                       denominator_shares: float | None,
                       prior_to_current_split: float | None,
                       denominator_available: bool,
                       manager_types: Mapping[str, str] | None = None,
                       manager_styles: Mapping[str, str] | None = None) -> dict[str, Any]:
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
    ratio = shares / denominator_shares if cohort_complete and denominator_ok else None
    result: dict[str, Any] = {
        "inst_shares": shares if cohort_complete else None,
        "inst_n_holders": holders if cohort_complete else None,
        "inst_ownership": ratio,
        "ownership_above_one": ratio > 1 if ratio is not None else None,
        "observed_shares": shares, "contributing_managers": len(current_managers),
        "cohort_complete": cohort_complete,
        "inst_hhi": sum((v / shares) ** 2 for v in current.values()) if cohort_complete and shares else None,
        "inst_top5_share": sum(sorted(current.values(), reverse=True)[:5]) / shares if cohort_complete and shares else None,
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
        result["shares_by_filer_type"] = by_type if cohort_complete else None
    if manager_styles is not None:
        style = {"active": 0.0, "passive": 0.0, "unknown": 0.0}
        for manager, value in current.items():
            label = manager_styles.get(manager, "unknown")
            if label not in style:
                raise ValueError("manager style requires explicit active/passive/unknown evidence")
            style[label] += value
        result["shares_by_manager_style"] = style if cohort_complete else None
    split_ok = (prior_to_current_split is not None and math.isfinite(prior_to_current_split)
                and prior_to_current_split > 0)
    if previous is None or previous_managers is None:
        result["change_reason"] = "prior_cohort_missing"
        return result
    common = current_managers & previous_managers
    result["common_managers"] = len(common)
    if split_ok and cohort_complete and previous_complete:
        result["common_manager_share_change"] = sum(
            current.get(m, 0) - previous.get(m, 0) * prior_to_current_split for m in common)
    if not cohort_complete or not previous_complete:
        result["change_reason"] = "incomplete_reporting_cohort"
    elif current_managers != previous_managers:
        result["change_reason"] = "reporting_cohort_changed"
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
            quality_status VARCHAR, PRIMARY KEY(security_id, period_q, available_at))""")
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
                    manager_types=row.get("manager_types"), manager_styles=row.get("manager_styles"))
                columns = ["inst_shares", "inst_n_holders", "inst_ownership", "inst_shares_change", "inst_ownership_change", "inst_breadth_change",
                    "inst_new_positions", "inst_closed_positions", "inst_persistent_holders", "inst_hhi", "inst_top5_share",
                    "ownership_above_one", "observed_shares", "contributing_managers", "cohort_complete",
                    "change_reason", "common_manager_share_change"]
                payload = [row["security_id"], row["period_q"], clock] + [values[k] for k in columns]
                payload += [row["source_accessions"], row["denominator_shares"] if row["denominator_available"] else None,
                            json.dumps(values["shares_by_filer_type"]) if values.get("shares_by_filer_type") is not None else None,
                            json.dumps(values["shares_by_manager_style"]) if values.get("shares_by_manager_style") is not None else None,
                            "UNQUALIFIED_NORMALIZED_ADAPTERS"]
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
    parser.add_argument("--snapshots", type=Path, required=True)
    parser.add_argument("--bindings", type=Path, required=True)
    args = parser.parse_args(argv)
    print(json.dumps(build(args.snapshots, args.bindings), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
