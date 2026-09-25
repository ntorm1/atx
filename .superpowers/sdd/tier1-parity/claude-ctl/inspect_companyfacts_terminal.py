"""Read-only terminal inspection / acceptance evidence for one CompanyFacts archive attempt.

Usage (run ONLY after the attempt's guard has exited, under run_memory_guarded.py --job-gb 0.5):
  python inspect_companyfacts_terminal.py --activation-run activation-companyfacts-archive20 --out <new.json>

256MB / one thread / read_only; aggregate SQL only; Python holds only per-CIK receipt/count
maps (<= archive member count) and the ZIP central-directory names. Refuses to overwrite --out.
Acceptance (Outcome 1): activation 'completed' + dataset 'succeeded'; every archive member has a
lineage disposition (members_without_disposition == 0); every loaded receipt's retained fact count
equals its receipt rows; facts == points per owning run; failed == 0.
"""
import argparse
import datetime as dt
import json
import re
import uuid
import zipfile
from pathlib import Path

import duckdb

MEMBER = re.compile(r'CIK(\d{10})\.json')


def _lit(value: str) -> str:
    if not isinstance(value, str) or '\x00' in value:
        raise ValueError('bad literal')
    return "'" + value.replace("'", "''") + "'"


def _uuid(value: str) -> str:
    if str(uuid.UUID(value)) != value:
        raise ValueError('non-canonical UUID')
    return _lit(value)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--activation-run', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--db', default='C:/atx/atx-db/data/warehouse.duckdb')
    args = parser.parse_args()
    if args.out.exists():
        raise SystemExit('Refusing to overwrite inspection receipt')
    if not re.fullmatch(r'[A-Za-z0-9_.-]+', args.activation_run):
        raise SystemExit('bad activation run id')
    r: dict = {'observed_at_utc': dt.datetime.now(dt.UTC).isoformat(), 'activation_run': args.activation_run,
               'config': {'memory_limit': '256MB', 'threads': 1, 'read_only': True}}
    with duckdb.connect(args.db, read_only=True, config={
        'memory_limit': '256MB', 'threads': '1', 'preserve_insertion_order': 'false'
    }) as con:
        con.execute("SET TimeZone='UTC'")
        r['activation'] = con.execute(f"""
            SELECT stage,run_id,status,started_at,finished_at,rows,left(error,600)
            FROM activation_stage_runs WHERE run_id={_lit(args.activation_run)}""").fetchall()
        dataset = con.execute(f"""
            SELECT run_id,status,started_at,finished_at,rows_loaded,left(error_message,600),
                   json_extract_string(params_json,'$.resume_from_run_id'),
                   json_extract_string(params_json,'$.companyfacts_zip')
            FROM dataset_runs WHERE dataset_id='sec_company_facts' AND json_valid(params_json)
              AND json_extract_string(params_json,'$.run_id')={_lit(args.activation_run + '-companyfacts')}
        """).fetchall()
        r['dataset'] = dataset
        assert len(dataset) == 1, dataset
        start, zip_path = dataset[0][0], dataset[0][7]
        started, finished = dataset[0][2], dataset[0][3]
        lineage, prior = [], start
        while prior is not None and len(lineage) < 40:
            row = con.execute(f"""
                SELECT run_id,status,started_at,finished_at,rows_loaded,
                       json_extract_string(params_json,'$.run_id'),
                       json_extract_string(params_json,'$.resume_from_run_id')
                FROM dataset_runs WHERE dataset_id='sec_company_facts' AND run_id={_uuid(prior)}""").fetchone()
            assert row is not None, prior
            lineage.append(row[:6])
            prior = row[6]
        r['lineage_newest_first'] = lineage
        in_list = ','.join(_uuid(row[0]) for row in lineage)
        # Loader details as persisted by the loader's final rows_loaded quality check.
        window = (f"checked_at BETWEEN TIMESTAMP {_lit(str(started))} AND TIMESTAMP {_lit(str(finished))}"
                  if finished is not None else f"checked_at >= TIMESTAMP {_lit(str(started))}")
        checks = con.execute(f"""
            SELECT check_name,status,observed_value,checked_at,details_json FROM data_quality_checks
            WHERE dataset_id='sec_company_facts' AND table_name='sec_company_facts' AND {window}
            ORDER BY checked_at""").fetchall()
        keep = ('outcome', 'completed_targets', 'resumed_loaded_targets', 'replayed_targets', 'loaded_targets',
                'empty_target_count', 'empty_target_reasons', 'unavailable_target_count',
                'unavailable_target_reasons', 'failed_target_count', 'failure_types', 'facts',
                'fundamental_points', 'unresolved_security_targets', 'unresolved_entity_targets',
                'archive_member_count', 'archive_cik_member_count', 'target_count', 'resume_verified_rows',
                'previously_completed_targets', 'connection_reopens', 'resume_from_run_id', 'archive_sha256')
        r['quality_checks'] = []
        for name, status, observed, at, details in checks:
            try:
                parsed = json.loads(details) if details else {}
            except ValueError:
                parsed = {}
            r['quality_checks'].append({'check': name, 'status': status, 'observed': observed, 'at': at,
                                        'details': {k: parsed.get(k) for k in keep if k in parsed}})
        r['attempt_facts'] = con.execute(f"""SELECT count(*),count(DISTINCT cik),min(period_end),max(period_end)
            FROM sec_company_facts WHERE run_id={_uuid(start)}""").fetchone()
        r['attempt_points'] = con.execute(f"""SELECT count(*),count(DISTINCT security_id),min(period_end),max(period_end)
            FROM fundamental_points WHERE run_id={_uuid(start)}""").fetchone()
        r['facts_by_run'] = con.execute(f"""
            SELECT run_id, run_id IN ({in_list}), count(*), count(DISTINCT cik), min(period_end), max(period_end)
            FROM sec_company_facts GROUP BY run_id ORDER BY count(*) DESC""").fetchall()
        r['points_by_run'] = con.execute(f"""
            SELECT run_id, run_id IN ({in_list}), count(*), count(DISTINCT security_id)
            FROM fundamental_points WHERE source='SEC companyfacts' GROUP BY run_id ORDER BY count(*) DESC""").fetchall()
        r['facts_totals'] = con.execute(f"""
            SELECT count(*), count(DISTINCT cik), count(*) FILTER (WHERE run_id IN ({in_list})),
                   count(DISTINCT cik) FILTER (WHERE run_id IN ({in_list})), min(period_end), max(period_end)
            FROM sec_company_facts""").fetchone()
        r['receipts_by_run_status'] = con.execute(f"""
            SELECT json_extract_string(metadata_json,'$.run_id') AS run, status, count(*),
                   sum(try_cast(json_extract_string(metadata_json,'$.rows') AS BIGINT)), min(fetched_at), max(fetched_at)
            FROM raw_source_files WHERE dataset_id='sec_company_facts' AND json_valid(metadata_json)
              AND json_extract_string(metadata_json,'$.run_id') IN ({in_list})
            GROUP BY run, status ORDER BY min(fetched_at)""").fetchall()
        receipt_rows = con.execute(f"""
            SELECT json_extract_string(metadata_json,'$.cik'), status,
                   try_cast(json_extract_string(metadata_json,'$.rows') AS BIGINT),
                   coalesce(json_extract_string(metadata_json,'$.empty_reason'),
                            json_extract_string(metadata_json,'$.unavailable_reason'),
                            json_extract_string(metadata_json,'$.error_type'))
            FROM raw_source_files WHERE dataset_id='sec_company_facts' AND json_valid(metadata_json)
              AND json_extract_string(metadata_json,'$.run_id') IN ({in_list})""").fetchall()
        fact_counts = dict(con.execute("SELECT cik, count(*) FROM sec_company_facts GROUP BY cik").fetchall())
    by_cik: dict[str, dict] = {}
    for cik, status, rows, reason in receipt_rows:
        by_cik.setdefault(cik, {})[status] = (rows, reason)
    with zipfile.ZipFile(Path('C:/atx/atx-db') / zip_path if not Path(zip_path).is_absolute() else zip_path) as archive:
        targets = sorted({m.group(1) for name in archive.namelist() if (m := MEMBER.fullmatch(name))})
    order = ('loaded', 'empty', 'unavailable', 'error')
    dispositions: dict[str, int] = {}
    reasons: dict[str, int] = {}
    mismatched_loaded, missing_positions = [], []
    for position, cik in enumerate(targets):
        found = by_cik.get(cik, {})
        kind = next((status for status in order if status in found), 'none')
        dispositions[kind] = dispositions.get(kind, 0) + 1
        if kind == 'none':
            missing_positions.append(position)
            continue
        rows, reason = found[kind]
        if kind != 'loaded':
            key = f'{kind}:{reason}'
            reasons[key] = reasons.get(key, 0) + 1
        elif fact_counts.get(cik, 0) != rows:
            mismatched_loaded.append({'cik': cik, 'receipt_rows': rows, 'retained_facts': fact_counts.get(cik, 0)})
    archive_ciks = set(targets)
    stale = [cik for cik in fact_counts if cik not in archive_ciks or 'loaded' not in by_cik.get(cik, {})]
    r.update(archive_targets=len(targets), dispositions=dispositions, non_loaded_reasons=reasons,
             members_without_disposition=len(missing_positions),
             first_member_without_disposition=(missing_positions[0] if missing_positions else None),
             loaded_receipt_fact_count_mismatches=len(mismatched_loaded),
             loaded_receipt_fact_count_mismatch_sample=mismatched_loaded[:20],
             fact_ciks_without_lineage_loaded_receipt=len(stale), stale_fact_cik_sample=sorted(stale)[:20])
    act = [row for row in r['activation'] if row[0] == 'companyfacts_load']
    r['acceptance'] = {
        'activation_completed': bool(act) and act[0][2] == 'completed',
        'dataset_succeeded': dataset[0][1] == 'succeeded',
        'all_members_have_disposition': not missing_positions,
        'loaded_receipts_match_retained_facts': not mismatched_loaded,
        'no_error_dispositions': dispositions.get('error', 0) == 0,
        'no_stale_non_lineage_facts': not stale,
    }
    args.out.write_text(json.dumps(r, indent=2, default=str) + '\n', encoding='utf-8')
    print(json.dumps({k: r[k] for k in ('activation', 'dataset', 'attempt_facts', 'attempt_points', 'facts_totals',
                                        'archive_targets', 'dispositions', 'non_loaded_reasons',
                                        'members_without_disposition', 'first_member_without_disposition',
                                        'loaded_receipt_fact_count_mismatches',
                                        'fact_ciks_without_lineage_loaded_receipt', 'acceptance')}, default=str))


if __name__ == '__main__':
    main()
