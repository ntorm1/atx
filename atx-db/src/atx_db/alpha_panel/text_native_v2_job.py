"""B1 bounded issuer worker. Real I/O requires the host's heavy memory guard."""
from __future__ import annotations

import argparse
import datetime as dt
import gzip
import hashlib
import json
import math
import os
import shutil
import time
from collections import Counter, defaultdict
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

from atx_db import sec_http
from . import text_native_v2 as V

SUPPORT_START = '2018-06-30'
PRIMARY_START = '2020-01-01'
MAX_TRANSFER = 6_000_000_000
MAX_BODY = 40 * 1024**2


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024**2), b''):
            h.update(chunk)
    return {'bytes': path.stat().st_size, 'sha256': h.hexdigest()}


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.partial')
    with temporary.open('w', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, sort_keys=True, separators=(',', ':'), allow_nan=False)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def append_json(path, row):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a', encoding='utf-8', newline='\n') as stream:
        stream.write(json.dumps(row, sort_keys=True, allow_nan=False) + '\n')
        stream.flush()
        os.fsync(stream.fileno())


class Transport:
    """One adapter attempt per receipt, streamed bytes counted even on failure."""
    def __init__(self, ledger, max_transfer=MAX_TRANSFER):
        self.ledger, self.max_transfer = ledger, max_transfer
        self.transferred, self.attempts = 0, 0
        if ledger.exists():
            for line in ledger.read_text(encoding='utf-8').splitlines():
                row = json.loads(line)
                if row['event'] == 'start':
                    self.attempts += 1
                else:
                    self.transferred += row.get('bytes', 0)
            # An interrupted streaming attempt has an unknown final byte count.
            # The start reserves the entire response bound, so resume remains conservative.
            starts, ends = {}, set()
            for line in ledger.read_text(encoding='utf-8').splitlines():
                row = json.loads(line)
                if row['event'] == 'start':
                    starts[row['attempt']] = row['maximum']
                else:
                    ends.add(row['attempt'])
            self.transferred += sum(n for attempt, n in starts.items() if attempt not in ends)
        self.session = sec_http.sec_session(max_attempts=1)

    def get(self, url, maximum):
        if self.transferred + maximum > self.max_transfer:
            raise RuntimeError('all-attempt transfer budget exhausted')
        if shutil.disk_usage(Path('C:/')).free < 25 * 1024**3:
            raise RuntimeError('25 GiB disk floor')
        self.attempts += 1
        receipt = {'event': 'start', 'attempt': self.attempts, 'url': url,
                   'maximum': maximum, 'fetched_at': dt.datetime.now(dt.UTC).isoformat()}
        append_json(self.ledger, receipt)
        chunks, size, h = [], 0, hashlib.sha256()
        try:
            with self.session.get(url, stream=True, timeout=(20, 60), allow_redirects=False) as response:
                receipt['status'] = response.status_code
                # Refusals must stop immediately, never move to another URL/provider.
                for chunk in response.iter_content(chunk_size=65536):
                    size += len(chunk)
                    self.transferred += len(chunk)
                    h.update(chunk)
                    if size > maximum or self.transferred > self.max_transfer:
                        raise ValueError('bounded_response_exceeded')
                    chunks.append(chunk)
                if response.status_code in (403, 429):
                    raise sec_http.SecBlockedError(f'SEC refusal {response.status_code}: {url}')
                response.raise_for_status()
                if response.status_code != 200:
                    raise ValueError('redirect_or_non200_refused')
            return b''.join(chunks), {**receipt, 'bytes': size, 'sha256': h.hexdigest()}
        except BaseException as exc:
            receipt['error'] = type(exc).__name__ + ': ' + str(exc)[:500]
            raise
        finally:
            append_json(self.ledger, {**receipt, 'event': 'end', 'bytes': size, 'sha256': h.hexdigest()})


def quarters(end):
    return [(year, quarter) for year in range(2018, end.year + 1) for quarter in range(1, 5)
            if (year, quarter) >= (2018, 2) and (year, quarter) <= (end.year, (end.month - 1)//3 + 1)]


def parse_master(raw):
    for line in raw.decode('latin-1').splitlines():
        fields = line.split('|')
        if len(fields) != 5 or not fields[0].isdigit() or fields[2] not in V.ORIGINALS or fields[3] < SUPPORT_START:
            continue
        cik, _, form, filing_date, filename = fields
        match = __import__('re').search(r'(\d{10}-\d{2}-\d{6})\.txt$', filename)
        if match:
            yield {'cik': int(cik), 'accession': match.group(1), 'form': form,
                   'filing_date': filing_date, 'index_filename': filename}


def census(raw_root, transport, end):
    records, sources = {}, []
    for year, quarter in quarters(end):
        path = raw_root / f'{year}Q{quarter}.master.gz'
        receipt_path = path.with_suffix('.receipt.json')
        url = f'https://www.sec.gov/Archives/edgar/full-index/{year}/QTR{quarter}/master.gz'
        if path.exists():
            receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
            if digest(path) != receipt['stored']:
                raise ValueError('census pin mismatch')
            raw = gzip.decompress(path.read_bytes())
        else:
            try:
                compressed, receipt = transport.get(url, 32 * 1024**2)
            except Exception as exc:
                sources.append({'quarter': f'{year}Q{quarter}', 'url': url, 'unavailable': str(exc)})
                continue
            raw = gzip.decompress(compressed)
            if not raw.startswith(b'Description:') or b'CIK|Company Name|Form Type|Date Filed|Filename' not in raw:
                raise ValueError('not an SEC master index')
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(compressed)
            receipt.update(stored=digest(path), quarter=f'{year}Q{quarter}')
            atomic_json(receipt_path, receipt)
        parsed = list(parse_master(raw))
        horizon = max((r['filing_date'] for r in parsed), default=None)
        index_horizon = max((fields[3] for line in raw.decode('latin-1').splitlines()
                             if len(fields := line.split('|')) == 5 and fields[0].isdigit()), default=None)
        sources.append({'quarter': f'{year}Q{quarter}', 'url': url,
                        'fetched_at': receipt['fetched_at'], 'observed_original_filing_horizon': horizon,
                        'observed_index_filing_horizon': index_horizon,
                        'partial_quarter': (year, quarter) == (end.year, (end.month - 1)//3 + 1),
                        'sha256': receipt['sha256'], 'originals': len(parsed)})
        for row in parsed:
            key = (row['cik'], row['accession'])
            if key in records and records[key] != row:
                raise ValueError('conflicting master index accession')
            records[key] = row
    return sorted(records.values(), key=lambda r: (r['cik'], r['filing_date'], r['accession'])), sources


def select_issuers(records, maximum, issuer_count):
    by_issuer = defaultdict(list)
    for row in records:
        by_issuer[row['cik']].append(row)
    ranked = sorted(by_issuer, key=lambda cik: hashlib.sha256(f'b1-v2-whole-issuer:{cik}'.encode()).hexdigest())
    required_years = {r['filing_date'][:4] for r in records if r['filing_date'] >= PRIMARY_START}
    selected, total = [], 0
    for cik in ranked:
        rows = by_issuer[cik]
        # Stratification uses census membership only, never a later accounting/market fact.
        primary = [r for r in rows if r['filing_date'] >= PRIMARY_START]
        if {V.family(r['form']) for r in primary} != {'10-K', '10-Q'}:
            continue
        if {r['filing_date'][:4] for r in primary} != required_years:
            continue
        if total + len(rows) <= maximum:
            selected.append(cik)
            total += len(rows)
        if len(selected) == issuer_count:
            break
    if not selected:
        raise ValueError('no whole issuer fits pilot cap')
    return selected


def pack_profile(profile):
    if profile is None:
        return None
    return {'tf': dict(profile['tf']), 'doc': profile['doc'],
            'sections': {s: {**p, 'tf': dict(p['tf'])} for s, p in profile['sections'].items()}}


def unpack_profile(profile):
    if profile:
        profile['tf'] = Counter(profile['tf'])
        for p in profile['sections'].values():
            p['tf'] = Counter(p['tf'])
    return profile


def issuer_unit(output, cik, records, transport, interrupt_after=None):
    """Atomic per-filing snapshot; atomic complete issuer unit; bodies never persisted."""
    unit = output / 'units' / f'{cik}.json'
    state_path = output / 'working' / f'{cik}.json'
    if unit.exists():
        return json.loads(unit.read_text(encoding='utf-8'))
    state = json.loads(state_path.read_text(encoding='utf-8')) if state_path.exists() else {'rows': [], 'history': []}
    completed = {r['accession'] for r in state['rows']}
    history = [(r, unpack_profile(p)) for r, p in state['history']]
    for record in records:
        if record['accession'] in completed:
            continue
        meta, profile = dict(record), None
        meta.update(available_at=None, report_date=None, fiscal_period=None, fiscal_year=None, reason=None)
        url = V.index_url(record)
        try:
            raw_index, index_receipt = transport.get(url, 5 * 1024**2)
            meta.update(V.index_metadata(raw_index, record, url))
            meta['index_receipt'] = index_receipt
            del raw_index
            if not meta.get('primary_url'):
                meta['reason'] = meta.get('reason') or 'primary_document_unproven'
            else:
                raw, body_receipt = transport.get(meta['primary_url'], MAX_BODY)
                meta.update(V.fiscal_metadata(raw))
                meta['body_receipt'] = body_receipt
                meta['doc_bytes'] = len(raw)
                profile = V.profile_document(raw, record['form'])
                del raw
                if meta['fiscal_period'] is None or meta['fiscal_year'] is None:
                    meta['reason'] = meta.get('reason') or 'original_fiscal_period_unproven'
        except Exception as exc:
            meta['reason'] = meta.get('reason') or type(exc).__name__ + ':' + str(exc)[:300]
        # Prune before allocation retention; 550 days is the strict comparator bound.
        floor = dt.date.fromisoformat(record['filing_date']) - dt.timedelta(days=551)
        history = [(r, p) for r, p in history if r['filing_date'] >= floor.isoformat()]
        prior = V.select_prior(meta, [r for r, _ in history])
        prior_profile = next((p for r, p in history if prior and r['accession'] == prior['accession']), None)
        state['rows'].append(V.feature_row(meta, profile, prior, prior_profile))
        if profile:
            retained = sum(sum(p['tf'].values()) for _, p in history if p) + sum(profile['tf'].values())
            if retained > 500_000:
                raise ValueError('issuer_profile_word_bound_exceeded')
        history.append((meta, profile))
        state['history'] = [(r, pack_profile(p)) for r, p in history]
        atomic_json(state_path, state)
        print(json.dumps({'issuer': cik, 'completed_filings': len(state['rows']), 'requests': transport.attempts,
                          'transfer_bytes': transport.transferred}), flush=True)
        if interrupt_after and len(state['rows']) >= interrupt_after:
            raise InterruptedError('synthetic interruption')
    atomic_json(unit, state['rows'])
    # This is this worker's exact scratch file. Refusal propagates; no alternate cleanup.
    resolved = state_path.resolve()
    if output.resolve() not in resolved.parents:
        raise ValueError('cleanup escaped owned generation')
    state_path.unlink()
    return state['rows']


def census_counts(records):
    counts = Counter(r['filing_date'][:4] for r in records if r['filing_date'] >= PRIMARY_START)
    return dict(sorted(counts.items()))


def write_parts(output, rows):
    groups = defaultdict(list)
    for row in rows:
        public = {k: v for k, v in row.items() if k not in {'index_receipt', 'body_receipt'}}
        groups[V.year_part(row)].append(public)
    files = {}
    for year, part_rows in sorted(groups.items()):
        path = output / 'parts' / f'year={year}' / 'features.parquet'
        path.parent.mkdir(parents=True, exist_ok=True)
        # Keep clock fields UTC-aware and dates typed, including all-NULL groups.
        table = pa.Table.from_pylist(part_rows)
        for key in ('available_at', 'prior_available_at'):
            values = [dt.datetime.fromisoformat(r[key]).astimezone(dt.UTC) if r.get(key) else None for r in part_rows]
            index = table.schema.get_field_index(key)
            table = table.set_column(index, key, pa.array(values, type=pa.timestamp('us', tz='UTC')))
        for key in ('filing_date', 'report_date', 'prior_report_date'):
            values = [dt.date.fromisoformat(r[key]) if r.get(key) else None for r in part_rows]
            index = table.schema.get_field_index(key)
            table = table.set_column(index, key, pa.array(values, type=pa.date32()))
        pq.write_table(table, path, compression='zstd')
        files[path.relative_to(output).as_posix()] = digest(path)
    return files


def acceptance(output, rows, plan):
    manifest = json.loads((output / 'manifest.json').read_text(encoding='utf-8'))
    manifest_ok = (manifest.get('status') == 'complete' and manifest.get('rule') == V.RULE and
                   manifest.get('clock_rule') == V.CLOCK_RULE and manifest.get('schema') == V.SCHEMA and
                   manifest.get('staleness_days') == 400 and manifest.get('scope') == plan['scope'])
    decoded_rows = []
    for rel, pin in manifest['files'].items():
        manifest_ok &= digest(output / rel) == pin
        if rel.endswith('.parquet'):
            for row in pq.ParquetFile(output / rel).read().to_pylist():
                clock = row.get('available_at')
                manifest_ok &= (f'year={clock.year}' in rel) if clock else 'year=null' in rel
                decoded_rows.append(row)
    manifest_ok &= len(decoded_rows) == len(rows) and len({(r['cik'], r['accession']) for r in decoded_rows}) == len(rows)
    checks = {'i_manifest_and_year_parts': {'status': 'PASS' if manifest_ok else 'FAIL', 'rows': len(decoded_rows)},
              'ii_landed_over_census': {}, 'iii_comparator_share': {},
              'iv_originals_strict_prior_clock': {'status': 'PASS'}}
    # Master indexes have filing dates, not acceptance instants. Unknown census clocks
    # must never masquerade as measured acceptance-year denominators.
    primary = [r for r in rows if r.get('available_at') and r['available_at'][:10] >= PRIMARY_START]
    for year, denominator in plan['census_by_filing_year'].items():
        landed = [r for r in primary if r['available_at'][:4] == year and r.get('body_receipt')]
        checks['ii_landed_over_census'][year] = {
            'landed_originals_acceptance_year': len(landed), 'census_originals_filing_year': denominator,
            'pilot_share_of_filing_year_census': len(landed) / denominator if denominator else None,
            'exact_acceptance_year_census_denominator': None,
            'qualification': 'FAIL_exact_acceptance_year_denominator_unproven; pilot_not_full_cohort'}
        eligible = []
        unknown = 0
        for row in landed:
            if not all(row.get(k) is not None for k in ('fiscal_period', 'fiscal_year', 'report_date')):
                unknown += 1
                eligible.append(row)
                continue
            earlier_same_period = [r for r in rows if r['cik'] == row['cik'] and
                                   V.family(r['form']) == V.family(row['form']) and
                                   r.get('fiscal_period') == row['fiscal_period'] and
                                   r.get('fiscal_year') is not None and r['fiscal_year'] < row['fiscal_year']]
            unproven_earlier = any(r['cik'] == row['cik'] and V.family(r['form']) == V.family(row['form']) and
                                  r['filing_date'] < row['filing_date'] and
                                  (r.get('fiscal_period') is None or r.get('fiscal_year') is None)
                                  for r in rows)
            if earlier_same_period or unproven_earlier:
                eligible.append(row)
                unknown += int(not earlier_same_period)
        success = sum(r.get('prior_accession') is not None and r.get('doc_sim_cosine') is not None and
                      math.isfinite(r['doc_sim_cosine']) for r in eligible)
        share = success / len(eligible) if eligible else None
        checks['iii_comparator_share'][year] = {'eligible': len(eligible), 'finite_comparators': success,
                    'share': share, 'metadata_unknown': unknown, 'gate': 0.9,
                    'status': 'PASS' if share is not None and share >= 0.9 and not unknown else 'FAIL'}
    invalid = [r['accession'] for r in rows if r['form'] not in V.ORIGINALS or
               (r.get('prior_accession') and (r.get('prior_form') not in V.ORIGINALS or
                not r.get('prior_available_at') or not r.get('available_at') or
                dt.datetime.fromisoformat(r['prior_available_at']) >= dt.datetime.fromisoformat(r['available_at'])))]
    checks['iv_originals_strict_prior_clock'] = {'status': 'FAIL' if invalid else 'PASS', 'invalid_accessions': invalid}
    for name, result in checks.items():
        print(json.dumps({name: result}, sort_keys=True), flush=True)
    return checks


def inspect_incomplete(output):
    """Read-only blocked-run evidence. Never contacts SEC or seals a partial pilot."""
    plan = json.loads((output / 'plan.json').read_text(encoding='utf-8'))
    ledger = [json.loads(line) for line in (output / 'requests.jsonl').read_text(encoding='utf-8').splitlines()]
    starts = [r for r in ledger if r['event'] == 'start']
    ends = [r for r in ledger if r['event'] == 'end']
    refusals = [r for r in ends if r.get('status') in (403, 429)]
    rows = []
    for cik in plan['issuers']:
        unit = output / 'units' / f'{cik}.json'
        working = output / 'working' / f'{cik}.json'
        if unit.exists():
            rows.extend(json.loads(unit.read_text(encoding='utf-8')))
        elif working.exists():
            rows.extend(json.loads(working.read_text(encoding='utf-8'))['rows'])
    evidence = {'scope': plan['scope'], 'sources': plan['sources'],
                'primary_start': plan['primary_start'], 'support_start': plan['support_start'],
                'observed_source_horizon': plan['observed_source_horizon'],
                'census_originals': plan['census_originals'], 'census_by_filing_year': plan['census_by_filing_year'],
                'selected_issuers': plan['issuers'], 'selected_filings': len(plan['records']),
                'selected_by_filing_year': dict(sorted(Counter(r['filing_date'][:4] for r in plan['records']).items())),
                'selected_by_form': dict(Counter(r['form'] for r in plan['records'])),
                'completed_rows': len(rows), 'landed_bodies': sum(bool(r.get('body_receipt')) for r in rows),
                'attempts': len(starts), 'bytes': sum(r['bytes'] for r in ends),
                'unclosed_attempts': len(starts) - len(ends), 'refusals': refusals,
                'manifest_present': (output / 'manifest.json').exists(),
                'throughput_filings_per_second': 0.0 if not rows else None,
                'bytes_per_landed_filing': None, 'requests_per_landed_filing': None,
                'full_body_cost_extrapolation': None,
                'cost_reason': 'No primary documents landed: census speed cannot estimate document landing/parsing speed.'}
    checks = {'i_manifest_and_year_parts': {'status': 'FAIL', 'reason': 'pilot_incomplete_no_manifest'},
              'ii_landed_over_census': {year: {'landed_originals': 0, 'census_originals_filing_year': count,
                                                'pilot_share': 0.0, 'status': 'FAIL',
                                                'exact_acceptance_year_denominator': None}
                                       for year, count in plan['census_by_filing_year'].items()},
              'iii_comparator_share': {year: {'eligible': 0, 'finite_comparators': 0, 'share': None,
                                              'gate': 0.9, 'status': 'FAIL', 'reason': 'no_landed_filings'}
                                       for year in plan['census_by_filing_year']},
              'iv_originals_strict_prior_clock': {'status': 'FAIL', 'reason': 'unmeasured_no_landed_rows', 'violations': 0}}
    for name, result in checks.items():
        print(json.dumps({name: result}, sort_keys=True), flush=True)
    print(json.dumps({'incomplete_pilot_evidence': evidence}, sort_keys=True), flush=True)
    return {'checks': checks, 'evidence': evidence}


def run(args):
    if not os.environ.get('ATX_GUARD_JOB'):
        raise RuntimeError('real I/O requires run_memory_guarded.py --heavy')
    output, raw_root = args.output.resolve(), args.raw_root.resolve()
    if args.inspect:
        evidence = inspect_incomplete(output)
        if args.evidence_output:
            atomic_json(args.evidence_output, evidence)
        return
    manifest_path = output / 'manifest.json'
    if manifest_path.exists():
        plan = json.loads((output / 'plan.json').read_text(encoding='utf-8'))
        rows = []
        for cik in plan['issuers']:
            rows.extend(json.loads((output / 'units' / f'{cik}.json').read_text(encoding='utf-8')))
        acceptance(output, rows, plan)
        return
    output.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    transport = Transport(output / 'requests.jsonl', args.max_transfer)
    plan_path = output / 'plan.json'
    if plan_path.exists():
        plan = json.loads(plan_path.read_text(encoding='utf-8'))
        if plan['scope'] != args.mode or plan['maximum_filings'] != args.max_filings:
            raise ValueError('resume scope/cap mismatch')
    else:
        records, sources = census(raw_root, transport, dt.date.fromisoformat(args.source_end))
        issuers = (select_issuers(records, args.max_filings, args.issuers) if args.mode == 'pilot'
                   else sorted({r['cik'] for r in records}))
        selected = [r for r in records if r['cik'] in set(issuers)]
        if len(selected) > args.max_filings:
            raise ValueError('whole-issuer filing cap')
        plan = {'scope': args.mode, 'primary_start': PRIMARY_START, 'support_start': SUPPORT_START,
                'requested_source_end': args.source_end, 'sources': sources,
                'observed_source_horizon': max((s.get('observed_original_filing_horizon') or '' for s in sources), default=None),
                'census_originals': len(records), 'census_by_filing_year': census_counts(records),
                'issuers': issuers, 'records': selected, 'maximum_filings': args.max_filings,
                'selection_rule': 'sha256(b1-v2-whole-issuer:CIK); both_families; every_observed_primary_year; whole_issuers; budget_fit'}
        atomic_json(plan_path, plan)
        print(json.dumps({'plan': {k: v for k, v in plan.items() if k not in {'records', 'sources'}}}), flush=True)
    by_issuer = defaultdict(list)
    for record in plan['records']:
        by_issuer[record['cik']].append(record)
    rows = []
    for cik in plan['issuers']:
        rows.extend(issuer_unit(output, cik, by_issuer[cik], transport))
    files = write_parts(output, rows)
    for path in [plan_path, output / 'requests.jsonl', *(output / 'units' / f'{cik}.json' for cik in plan['issuers'])]:
        files[path.relative_to(output).as_posix()] = digest(path)
    wall = time.monotonic() - started
    landed = sum(bool(r.get('body_receipt')) for r in rows)
    measured = {'run_seconds': wall, 'rows': len(rows), 'landed_bodies': landed,
                'all_attempts': transport.attempts, 'all_attempt_bytes': transport.transferred,
                'filings_per_second': landed / wall, 'bytes_per_filing': transport.transferred / landed if landed else None,
                'requests_per_filing': transport.attempts / landed if landed else None}
    disk = sum(p.stat().st_size for p in output.rglob('*') if p.is_file())
    measured['persistent_bytes'] = disk
    measured['projected_full_persistent_bytes'] = disk / len(rows) * plan['census_originals'] if rows else None
    measured['projected_full_seconds'] = wall / landed * plan['census_originals'] if landed else None
    measured['projected_full_transfer_bytes'] = transport.transferred / landed * plan['census_originals'] if landed else None
    if measured['projected_full_persistent_bytes'] is None or measured['projected_full_persistent_bytes'] >= 16_000_000_000:
        raise ValueError('whole-cohort persistent disk projection exceeds 16 GB')
    manifest = {'schema': V.SCHEMA, 'status': 'complete', 'stage': 'text_native_v2',
                'rule': V.RULE, 'clock_rule': V.CLOCK_RULE, 'staleness_days': 400, 'scope': args.mode,
                'qualification': 'completed_materialization_only; pilot_not_full_cohort; acceptance_gates_separate',
                'primary_start': PRIMARY_START, 'support_start': SUPPORT_START,
                'source_horizon': plan['observed_source_horizon'], 'sources': plan['sources'],
                'excluded_null_available_at': sum(not r.get('available_at') for r in rows),
                'files': files, 'measurements': measured,
                'code': {str(Path(m.__file__).name): digest(Path(m.__file__)) for m in (V, V.TF, V.FT, sec_http)},
                'producer_code': digest(Path(__file__))}
    atomic_json(manifest_path, manifest)
    checks = acceptance(output, rows, plan)
    # Immutable manifest contains gate qualification at creation. No later mutation.
    atomic_json(output / 'acceptance.json', checks)
    print(json.dumps({'manifest_pin': digest(manifest_path), 'measurements': measured}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['pilot', 'full'], default='pilot')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--raw-root', type=Path, required=True)
    parser.add_argument('--source-end', default=dt.date.today().isoformat())
    parser.add_argument('--max-filings', type=int, default=2000)
    parser.add_argument('--max-transfer', type=int, default=MAX_TRANSFER)
    parser.add_argument('--issuers', type=int, default=3)
    parser.add_argument('--inspect', action='store_true', help='read-only evidence of incomplete pilot; no network')
    parser.add_argument('--evidence-output', type=Path)
    args = parser.parse_args()
    if args.mode == 'pilot' and (not 1 <= args.max_filings <= 2000 or not 0 < args.max_transfer <= MAX_TRANSFER or not 1 <= args.issuers <= 64):
        parser.error('pilot must be <=2000 filings and <=6GB; 1..64 whole issuers')
    run(args)


if __name__ == '__main__':
    main()
