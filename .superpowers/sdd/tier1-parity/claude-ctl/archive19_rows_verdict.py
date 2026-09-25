"""Read-only, bounded evidence for the archive19 `rows=0` question and the archive20 frontier.

256MB / one thread / read_only. Aggregate-only SQL; the only Python-side row sets are
small receipt CIK lists (<= archive member count) and the ZIP central-directory names.
Writes companyfacts-archive19-rows-verdict.json (refuses overwrite).
"""
import datetime as dt
import json
import re
import uuid
import zipfile
from pathlib import Path

import duckdb

CTL = Path(r'C:\atx\.superpowers\sdd\tier1-parity')
START = 'e27c8a4e-2d29-47bb-b657-fc1855ea4cec'  # archive19 dataset UUID (closed failed)
ARCHIVE19_PROCESSED = 6350
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
    out = CTL / 'companyfacts-archive19-rows-verdict.json'
    if out.exists():
        raise SystemExit('Refusing to overwrite verdict receipt')
    result: dict = {'observed_at_utc': dt.datetime.now(dt.UTC).isoformat(),
                    'config': {'memory_limit': '256MB', 'threads': 1, 'read_only': True}}
    with duckdb.connect('C:/atx/atx-db/data/warehouse.duckdb', read_only=True, config={
        'memory_limit': '256MB', 'threads': '1', 'preserve_insertion_order': 'false'
    }) as con:
        con.execute("SET TimeZone='UTC'")
        # 1. Lineage walk (same rule as _companyfacts_resume._lineage), newest first.
        lineage, prior, zip_path = [], START, None
        while prior is not None and len(lineage) < 40:
            row = con.execute(f"""
                SELECT run_id,status,started_at,finished_at,rows_loaded,
                       json_extract_string(params_json,'$.run_id'),
                       json_extract_string(params_json,'$.resume_from_run_id'),
                       json_extract_string(params_json,'$.companyfacts_zip')
                FROM dataset_runs WHERE dataset_id='sec_company_facts' AND run_id={_uuid(prior)}
            """).fetchone()
            assert row is not None, prior
            lineage.append({'dataset_run_id': row[0], 'status': row[1], 'started_at': row[2],
                            'finished_at': row[3], 'rows_loaded': row[4], 'activation_run': row[5],
                            'resume_from_run_id': row[6]})
            zip_path = zip_path or row[7]
            prior = row[6]
        result['lineage_newest_first'] = lineage
        result['lineage_length'] = len(lineage)
        lineage_ids = [item['dataset_run_id'] for item in lineage]
        in_list = ','.join(_uuid(item) for item in lineage_ids)
        # 2. Fact ownership by run (all owners, lineage flagged).
        facts = con.execute(f"""
            SELECT run_id, run_id IN ({in_list}) AS in_lineage, count(*), count(DISTINCT cik),
                   min(period_end), max(period_end)
            FROM sec_company_facts GROUP BY run_id ORDER BY count(*) DESC
        """).fetchall()
        result['facts_by_run_columns'] = ['run_id', 'in_lineage', 'rows', 'ciks', 'min_period_end', 'max_period_end']
        result['facts_by_run'] = facts
        result['facts_totals'] = con.execute(f"""
            SELECT count(*), count(DISTINCT cik), count(*) FILTER (WHERE run_id IN ({in_list})),
                   count(DISTINCT cik) FILTER (WHERE run_id IN ({in_list})), min(period_end), max(period_end)
            FROM sec_company_facts
        """).fetchone()
        result['facts_totals_columns'] = ['rows', 'ciks', 'lineage_rows', 'lineage_ciks', 'min_period_end', 'max_period_end']
        # 3. Point ownership by run.
        points = con.execute(f"""
            SELECT run_id, run_id IN ({in_list}) AS in_lineage, count(*), count(DISTINCT security_id),
                   min(period_end), max(period_end)
            FROM fundamental_points WHERE source='SEC companyfacts' GROUP BY run_id ORDER BY count(*) DESC
        """).fetchall()
        result['points_by_run_columns'] = ['run_id', 'in_lineage', 'rows', 'security_ids', 'min_period_end', 'max_period_end']
        result['points_by_run'] = points
        result['points_total_all_sources'] = con.execute('SELECT count(*) FROM fundamental_points').fetchone()[0]
        # 4. Source receipts per lineage run and status.
        receipts = con.execute(f"""
            SELECT json_extract_string(metadata_json,'$.run_id') AS run, status, count(*),
                   sum(try_cast(json_extract_string(metadata_json,'$.rows') AS BIGINT)),
                   min(fetched_at), max(fetched_at)
            FROM raw_source_files WHERE dataset_id='sec_company_facts' AND json_valid(metadata_json)
              AND json_extract_string(metadata_json,'$.run_id') IN ({in_list})
            GROUP BY run, status ORDER BY min(fetched_at)
        """).fetchall()
        result['receipts_by_run_status_columns'] = ['run_id', 'status', 'members', 'reported_rows', 'first_fetched', 'last_fetched']
        result['receipts_by_run_status'] = receipts
        # 5. Per-CIK latest disposition across the lineage (small: <= member count).
        cik_status = con.execute(f"""
            SELECT json_extract_string(metadata_json,'$.cik') AS cik,
                   list(DISTINCT status ORDER BY status)
            FROM raw_source_files WHERE dataset_id='sec_company_facts' AND json_valid(metadata_json)
              AND json_extract_string(metadata_json,'$.run_id') IN ({in_list})
            GROUP BY cik
        """).fetchall()
        fact_ciks = {row[0] for row in con.execute(
            "SELECT DISTINCT cik FROM sec_company_facts WHERE cik IS NOT NULL").fetchall()}
    statuses = {cik: set(values) for cik, values in cik_status}
    with zipfile.ZipFile(zip_path) as archive:
        targets = sorted({m.group(1) for name in archive.namelist() if (m := MEMBER.fullmatch(name))})
    result['zip_path'] = zip_path
    result['archive_targets'] = len(targets)

    def disposition(cik: str) -> str:
        found = statuses.get(cik, set())
        if 'loaded' in found:
            return 'loaded_receipt'
        if 'empty' in found:
            return 'empty_receipt'
        if 'unavailable' in found:
            return 'unavailable_receipt'
        if 'error' in found:
            return 'error_receipt'
        return 'no_lineage_receipt'

    def tally(indices) -> dict:
        counts: dict[str, int] = {}
        with_facts = 0
        for index in indices:
            kind = disposition(targets[index])
            counts[kind] = counts.get(kind, 0) + 1
            if kind == 'no_lineage_receipt' and targets[index] in fact_ciks:
                with_facts += 1
        counts['no_lineage_receipt_but_retained_facts'] = with_facts
        return counts

    result['dispositions_all'] = tally(range(len(targets)))
    result['dispositions_archive19_processed_prefix'] = tally(range(ARCHIVE19_PROCESSED))
    result['dispositions_remaining_after_archive19'] = tally(range(ARCHIVE19_PROCESSED, len(targets)))
    frontier = next((i for i, cik in enumerate(targets) if disposition(cik) == 'no_lineage_receipt'), None)
    last_loaded = max((i for i, cik in enumerate(targets) if disposition(cik) == 'loaded_receipt'), default=None)
    result['first_no_receipt_position'] = frontier
    result['first_no_receipt_cik'] = targets[frontier] if frontier is not None else None
    result['last_loaded_receipt_position'] = last_loaded
    result['last_loaded_receipt_cik'] = targets[last_loaded] if last_loaded is not None else None
    result['no_receipt_positions_before_last_loaded'] = sum(
        1 for i in range(last_loaded or 0) if disposition(targets[i]) == 'no_lineage_receipt')
    out.write_text(json.dumps(result, indent=2, default=str) + '\n', encoding='utf-8')
    print(json.dumps({k: result[k] for k in (
        'lineage_length', 'facts_totals', 'archive_targets', 'dispositions_all',
        'dispositions_archive19_processed_prefix', 'dispositions_remaining_after_archive19',
        'first_no_receipt_position', 'first_no_receipt_cik', 'last_loaded_receipt_position',
        'last_loaded_receipt_cik', 'no_receipt_positions_before_last_loaded')}, default=str))


if __name__ == '__main__':
    main()
