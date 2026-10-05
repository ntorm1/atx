"""Bounded B1 acceptance: one issuer unit and one Parquet part at a time."""
from __future__ import annotations

import datetime as dt
import json
import math
from collections import Counter, defaultdict

import pyarrow.parquet as pq

from . import text_native_v2 as V


def issuer_counts(rows, primary_start='2020-01-01'):
    landed, accepted, eligible, finite, unknown = Counter(), Counter(), Counter(), Counter(), Counter()
    invalid = []
    null_clocks = 0
    for row in rows:
        if not row.get('available_at'):
            null_clocks += 1
        if row['form'] not in V.ORIGINALS:
            invalid.append(row['accession'])
        if row.get('prior_accession'):
            prior = next((r for r in rows if r['accession'] == row['prior_accession']), None)
            if (prior is None or V.select_prior(row, rows) != prior or
                    prior['form'] not in V.ORIGINALS or
                    row.get('prior_available_at') != prior.get('available_at')):
                invalid.append(row['accession'])
        if not row.get('available_at') or row['available_at'][:10] < primary_start:
            continue
        year = row['available_at'][:4]
        accepted[year] += 1
        if not row.get('body_receipt'):
            continue
        landed[year] += 1
        if not all(row.get(k) is not None for k in ('fiscal_period', 'fiscal_year', 'report_date')):
            eligible[year] += 1
            unknown[year] += 1
            continue
        earlier = [r for r in rows if r['cik'] == row['cik'] and
                   V.family(r['form']) == V.family(row['form']) and
                   r['filing_date'] < row['filing_date']]
        same_period = any(r.get('fiscal_period') == row['fiscal_period'] and
                          r.get('fiscal_year') is not None and r['fiscal_year'] < row['fiscal_year']
                          for r in earlier)
        unresolved = any(r.get('fiscal_period') is None or r.get('fiscal_year') is None for r in earlier)
        # Only a proven first filing of this period is excluded. Unknown facts cannot
        # make a missing comparison disappear from the denominator.
        if same_period or unresolved:
            eligible[year] += 1
            unknown[year] += int(not same_period)
            finite[year] += int(bool(row.get('prior_accession')) and row.get('doc_sim_cosine') is not None and
                                math.isfinite(row['doc_sim_cosine']))
    return {'landed': landed, 'accepted': accepted, 'eligible': eligible, 'finite': finite, 'unknown': unknown,
            'invalid': invalid, 'null_clocks': null_clocks}


def _serialized(value):
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    return value


def verify_stage(output, plan, digest, *, emit=True):
    manifest = json.loads((output / 'manifest.json').read_text(encoding='utf-8'))
    okay = (manifest.get('status') == 'complete' and manifest.get('rule') == V.RULE and
            manifest.get('clock_rule') == V.CLOCK_RULE and manifest.get('schema') == V.SCHEMA and
            manifest.get('staleness_days') == 400 and manifest.get('scope') == plan['scope'])
    expected_total, decoded_total = 0, 0
    stats = {key: Counter() for key in ('landed', 'accepted', 'eligible', 'finite', 'unknown')}
    null_clocks, invalid = 0, []
    for rel, pin in manifest['files'].items():
        okay &= digest(output / rel) == pin
    parts = defaultdict(list)
    for rel in manifest['files']:
        if rel.endswith('.parquet'):
            parts[rel.rsplit('/', 1)[-1]].append(rel)
    for cik in plan['issuers']:
        unit_rel = f'units/{cik}.json'
        okay &= unit_rel in manifest['files']
        rows = json.loads((output / unit_rel).read_text(encoding='utf-8'))
        expected_total += len(rows)
        okay &= len({r['accession'] for r in rows}) == len(rows) and all(r['cik'] == cik for r in rows)
        unit_stats = issuer_counts(rows)
        for key in stats:
            stats[key].update(unit_stats[key])
        null_clocks += unit_stats['null_clocks']
        invalid.extend(unit_stats['invalid'])
        by_key = {r['accession']: r for r in rows}
        filenames = [f'features-{cik}.parquet']
        # Support the initial producer's small global parts without retaining
        # the cohort. New generations use one issuer part inside each year.
        if 'features.parquet' in parts:
            filenames.append('features.parquet')
        seen = set()
        for filename in filenames:
            for rel in parts.get(filename, []):
                for row in pq.ParquetFile(output / rel).read().to_pylist():
                    if row['cik'] != cik:
                        continue
                    clock = row.get('available_at')
                    okay &= f'year={clock.year}/' in rel if clock else 'year=null/' in rel
                    accession = row['accession']
                    expected = by_key.get(accession)
                    okay &= accession not in seen and expected is not None
                    seen.add(accession)
                    if expected:
                        public = {k: v for k, v in expected.items() if k not in {'index_receipt', 'body_receipt'}}
                        okay &= public == {k: _serialized(v) for k, v in row.items()}
                    decoded_total += 1
        okay &= seen == set(by_key)
    okay &= expected_total == decoded_total == len(plan['records'])
    checks = {'i_manifest_and_year_parts': {'status': 'PASS' if okay else 'FAIL', 'rows': decoded_total,
                                            'excluded_null_available_at': null_clocks},
              'ii_landed_over_census': {}, 'iii_comparator_share': {},
              'iv_originals_strict_prior_clock': {'status': 'FAIL' if invalid else 'PASS', 'invalid_accessions': invalid}}
    for year, denominator in plan['census_by_filing_year'].items():
        exact = stats['accepted'][year] if plan['scope'] == 'full' and not null_clocks and expected_total == plan['census_originals'] else None
        checks['ii_landed_over_census'][year] = {
            'landed_originals_acceptance_year': stats['landed'][year], 'census_originals_filing_year': denominator,
            'pilot_share_of_filing_year_census': stats['landed'][year] / denominator if denominator else None,
            'exact_acceptance_year_census_denominator': exact,
            'status': 'PASS' if exact is not None and exact == stats['landed'][year] else 'FAIL',
            'qualification': 'pilot_not_full_cohort; master_index_dates_are_filing_dates' if plan['scope'] == 'pilot' else 'full_materialization'}
        n, successes = stats['eligible'][year], stats['finite'][year]
        share = successes / n if n else None
        checks['iii_comparator_share'][year] = {
            'eligible': n, 'finite_comparators': successes, 'share': share, 'metadata_unknown': stats['unknown'][year],
            'gate': 0.9, 'status': 'PASS' if share is not None and share >= 0.9 and not stats['unknown'][year] else 'FAIL'}
    if emit:
        for name, result in checks.items():
            print(json.dumps({name: result}, sort_keys=True), flush=True)
    return checks
