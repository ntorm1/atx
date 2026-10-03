"""Bounded single-line exposure kernel. Missing factor history remains NULL.

No production source adapter/publication is enabled at this checkpoint.
"""
from __future__ import annotations

import datetime as dt
import argparse
import json
import uuid
from pathlib import Path
import numpy as np

from .factors import END

FF5 = ('MktRF', 'SMB', 'HML', 'RMW', 'CMA')
STYLES = ('size', 'value', 'momentum', 'volatility', 'liquidity', 'growth', 'leverage', 'quality')


def regress(stock_returns, factor_returns, rf, *, minimum=200):
    """OLS over at most 252 already date-aligned rows, including intercept."""
    y = np.asarray(stock_returns, dtype=float)
    f = np.asarray(factor_returns, dtype=float)
    r = np.asarray(rf, dtype=float)
    if y.ndim != 1 or f.ndim != 2 or len(y) != len(f) or r.shape != y.shape or len(y) > 252:
        raise ValueError('one bounded, aligned 252-session window required')
    ok = np.isfinite(y) & np.isfinite(r) & np.isfinite(f).all(axis=1)
    n = int(ok.sum())
    missing = {'betas': [None] * f.shape[1], 'idio_vol': None, 'n_obs': n}
    if n < minimum:
        return missing
    x = np.column_stack((np.ones(n), f[ok]))
    if np.linalg.matrix_rank(x) < x.shape[1]:
        return missing
    b = np.linalg.lstsq(x, y[ok] - r[ok], rcond=None)[0]
    residual = y[ok] - r[ok] - x @ b
    return {'betas': b[1:].tolist(), 'idio_vol': float(np.sqrt(np.sum(residual ** 2) / (n-x.shape[1]))),
            'n_obs': n}


def exposure(session: dt.date, observations: list[dict]) -> dict:
    if len(observations) > 252:
        raise ValueError('bounded lookback required')
    dates = [r['session_date'] for r in observations]
    if dates != sorted(set(dates)) or any(d > session for d in dates):
        raise ValueError('duplicate/unsorted/future predictor observations')
    # Exposures are non-return products, but no prohibited return statistic is computed after 2022.
    if session > END:
        return {'beta_252': None, 'ff5_betas': [None]*5, 'ivol_ff5_252': None,
                'risk_missing_reason': 'own_factor_history_ends_2022',
                'available_at': dt.datetime.combine(session, dt.time(22), dt.UTC)}
    returns = [r.get('ret', np.nan) for r in observations]
    rf = [r.get('RF', np.nan) for r in observations]
    capm = regress(returns, [[r.get('MktRF', np.nan)] for r in observations], rf)
    ff = regress(returns, [[r.get(k, np.nan) for k in FF5] for r in observations], rf)
    return {'beta_252': capm['betas'][0], 'ff5_betas': ff['betas'], 'ivol_ff5_252': ff['idio_vol'],
            'n_obs_capm': capm['n_obs'], 'n_obs_ff5': ff['n_obs'],
            'available_at': dt.datetime.combine(session, dt.time(22), dt.UTC)}


def zscores(values: list[float | None]) -> list[float | None]:
    x = np.array([np.nan if v is None else v for v in values], dtype=float)
    good = np.isfinite(x)
    if good.sum() < 2 or np.std(x[good], ddof=1) == 0:
        return [None] * len(x)
    # Fixed sample-standard-deviation z score; no outcome-based clipping/tuning.
    mu, sd = np.mean(x[good]), np.std(x[good], ddof=1)
    return [float((v-mu)/sd) if np.isfinite(v) else None for v in x]


def pack(row: dict) -> dict:
    """Consume explicitly dated primitives; never manufacture classification/style/borrow."""
    day = dt.date.fromisoformat(row['session_date'])
    cutoff = dt.datetime.fromisoformat(row['cutoff'])
    if cutoff.tzinfo is None:
        raise ValueError('UTC cutoff required')
    latest_allowed = dt.datetime.combine(day-dt.timedelta(days=1), dt.time(22), dt.UTC)
    if cutoff > latest_allowed:
        raise ValueError('daily primitive cutoff exceeds previous close')
    out = {'session_date': day, 'security_id': row['security_id']}
    missing = []
    for field in ('ff49', *STYLES, 'adv_21', 'adv_63', 'spread_cs_21', 'borrow_proxy'):
        item = row.get('primitives', {}).get(field)
        if item is None:
            out[field] = None
            missing.append(field)
            continue
        clock = dt.datetime.fromisoformat(item['available_at'])
        if clock.tzinfo is None or clock >= cutoff:
            raise ValueError('unavailable primitive '+field)
        out[field] = item['value']
    history = []
    for r in row['observations']:
        history.append({**r, 'session_date': dt.date.fromisoformat(r['session_date'])})
    out.update(exposure(day, history))
    out['missing_primitives'] = missing
    out['cap_quality'] = row.get('cap_quality', 'uncertified')
    if out['cap_quality'] != 'exact_source_certified':
        for field in STYLES:
            out[field] = None
    return out


def build(prepared: Path, binding: dict, destination: Path) -> Path:
    from . import common as C
    from .factors import require_release, config_hash
    require_release(binding)
    identity = {k: v['sha256'] for k, v in binding.items()}
    source_sha = C.sha256_file(prepared)
    generation = destination/('generation='+uuid.uuid4().hex)
    generation.mkdir(parents=True, exist_ok=False)
    output = generation/'risk_inputs.jsonl'
    n, complete = 0, 0
    previous = None
    with prepared.open(encoding='utf-8') as source, output.open('x', encoding='utf-8') as target:
        for line in source:
            row = json.loads(line)
            if row['input_manifests_sha256'] != identity or row['config_sha256'] != config_hash():
                raise ValueError('stale prepared risk inputs')
            key = (row['session_date'], row['security_id'])
            if previous is not None and key <= previous:
                raise ValueError('risk input keys must be unique/sorted')
            previous = key
            result = pack(row)
            n += 1
            complete += not result['missing_primitives'] and result['beta_252'] is not None and all(
                b is not None for b in result['ff5_betas'])
            target.write(json.dumps(result, sort_keys=True, default=str)+'\n')
    if C.sha256_file(prepared) != source_sha:
        raise ValueError('prepared risk source changed')
    require_release(binding)
    coverage = complete/n if n else 0
    manifest = {'stage': 'risk_inputs', 'schema': 'atx.alpha-panel.risk-inputs/checkpoint-v1',
                'status': 'complete', 'input_manifests_sha256': identity,
                'prepared_sha256': source_sha, 'config_sha256': config_hash(), 'rows': n,
                'files': {'risk_inputs.jsonl': {'bytes': output.stat().st_size, 'sha256': C.sha256_file(output)}},
                'gate_status': {'exposure_coverage': {'status': 'PASS' if coverage >= .99 else 'FAIL',
                                'complete': complete, 'denominator': n, 'coverage': coverage},
                                'universe_denominator_certification': 'FAIL'}}
    C.write_json_atomic(generation/'manifest.json', manifest)
    C.write_json_atomic(destination/'manifest.json', {**manifest, 'generation': generation.name,
        'files': {generation.name+'/'+k: v for k, v in manifest['files'].items()}})
    return generation/'manifest.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--release-bindings', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    build(args.prepared, json.loads(args.release_bindings.read_text(encoding='utf-8')), args.destination)


if __name__ == '__main__':
    main()
