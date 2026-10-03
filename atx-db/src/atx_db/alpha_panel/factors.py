"""Frozen, offline factor formation kernels; no implicit lake/source selection.

Engineering checkpoint only. Production adapters and source certification must precede
publication. A caller supplies one bounded cross-section, never an entire history.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
import argparse
import os
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import numpy as np

END = dt.date(2022, 12, 31)
START = dt.date(2013, 1, 1)
FACTORS = ('MktRF', 'SMB', 'HML', 'RMW', 'CMA', 'UMD', 'STREV', 'LTREV',
           'q_ME', 'q_IA', 'q_ROE', 'q_EG', 'BAB')
CONFIG = {
    'rule': 'task6-frozen-v1', 'start': str(START), 'end': str(END),
    'universe': 'univ_all_common', 'breakpoints': 'NYSE issuer ME median; signal 30/70',
    'accounting_formation': 'June annual; previous fiscal year visible before formation cutoff',
    'price_formation': 'monthly; UMD months -12:-2; STREV -1; LTREV -60:-13',
    'weights': 'lagged certified issuer ME; line returns combined within issuer first',
    'q_formation': 'independent 2x3x3 ME/IA/ROE; monthly; EG separate 2x3',
    'q_EG': 'unavailable until published expected-growth model/source adapter is certified',
    'bab': 'rank beta; each leg dollar-weighted by distance from median rank; beta-scaled excess returns',
    'beta': '252 sessions, minimum 200 complete observations; intercept OLS',
    'missing': 'NULL if any required portfolio leg absent; no zero filling',
    'benchmark_join': 'exact calendar month; 120 required months 2013-2022; no window shrink',
    'cap_uncertainty': 'exclude unresolved share-basis inputs including CIK 77159 until exact-source audit',
}


def config_hash() -> str:
    return hashlib.sha256(json.dumps(CONFIG, sort_keys=True).encode()).hexdigest()


@dataclass(frozen=True)
class Stock:
    cik: int
    me: float
    ret: float | None
    nyse: bool
    signals: dict[str, float | None]
    available_at: dt.datetime
    cap_certified: bool = False


def visible(rows: Iterable[Stock], cutoff: dt.datetime) -> list[Stock]:
    if cutoff.tzinfo is None:
        raise ValueError('cutoff requires UTC timezone')
    out = []
    for row in rows:
        if row.available_at.tzinfo is None:
            raise ValueError('source clock missing UTC timezone')
        if row.available_at < cutoff and row.cap_certified and math.isfinite(row.me) and row.me > 0:
            out.append(row)
    if len({r.cik for r in out}) != len(out):
        raise ValueError('issuer returns/ME must be consolidated before formation')
    return out


def vw(rows: list[Stock]) -> float | None:
    # Missing returns must not silently renormalize the portfolio.
    if not rows or any(r.ret is None or not math.isfinite(r.ret) for r in rows):
        return None
    return sum(r.me * r.ret for r in rows) / sum(r.me for r in rows)


def mean(values: list[float | None]) -> float | None:
    return None if not values or any(v is None for v in values) else sum(values) / len(values)


def spread(high: list[float | None], low: list[float | None]) -> float | None:
    h, l = mean(high), mean(low)
    return None if h is None or l is None else h - l


def sort_2x3(rows: list[Stock], signal: str) -> tuple[float | None, float | None]:
    eligible = [r for r in rows if r.signals.get(signal) is not None and math.isfinite(r.signals[signal])]
    reference = [r for r in eligible if r.nyse]
    if not reference:
        return None, None
    size = float(np.quantile([r.me for r in reference], .5))
    lo, hi = np.quantile([r.signals[signal] for r in reference], [.3, .7])
    cells = {(s, v): [] for s in (0, 1) for v in (0, 1, 2)}
    for r in eligible:
        v = 0 if r.signals[signal] <= lo else 2 if r.signals[signal] > hi else 1
        cells[(int(r.me > size), v)].append(r)
    p = {k: vw(v) for k, v in cells.items()}
    return spread([p[0, 2], p[1, 2]], [p[0, 0], p[1, 0]]), spread(
        [p[0, i] for i in range(3)], [p[1, i] for i in range(3)])


def q_factors(rows: list[Stock]) -> dict:
    fields = ('investment', 'roe')
    good = [r for r in rows if all(r.signals.get(k) is not None and math.isfinite(r.signals[k]) for k in fields)]
    ref = [r for r in good if r.nyse]
    out = dict.fromkeys(('q_ME', 'q_IA', 'q_ROE', 'q_EG'))
    if not ref:
        return out
    size = np.quantile([r.me for r in ref], .5)
    breaks = {k: np.quantile([r.signals[k] for r in ref], [.3, .7]) for k in fields}
    cells = {(s, i, p): [] for s in range(2) for i in range(3) for p in range(3)}
    for r in good:
        tags = [0 if r.signals[k] <= breaks[k][0] else 2 if r.signals[k] > breaks[k][1] else 1 for k in fields]
        cells[int(r.me > size), *tags].append(r)
    p = {k: vw(v) for k, v in cells.items()}
    out['q_ME'] = spread([p[0, i, j] for i in range(3) for j in range(3)], [p[1, i, j] for i in range(3) for j in range(3)])
    out['q_IA'] = spread([p[s, 0, j] for s in range(2) for j in range(3)], [p[s, 2, j] for s in range(2) for j in range(3)])
    out['q_ROE'] = spread([p[s, i, 2] for s in range(2) for i in range(3)], [p[s, i, 0] for s in range(2) for i in range(3)])
    return out


def bab(rows: list[Stock], rf: float | None) -> float | None:
    good = [r for r in rows if r.signals.get('beta') is not None and math.isfinite(r.signals['beta'])]
    if len(good) < 2 or rf is None or any(r.ret is None for r in good):
        return None
    beta = np.array([r.signals['beta'] for r in good])
    ranks = np.array([1 + (beta < v).sum() + .5*((beta == v).sum()-1) for v in beta])
    center = ranks.mean()
    low, high = np.maximum(center-ranks, 0), np.maximum(ranks-center, 0)
    if low.sum() == 0 or high.sum() == 0:
        return None
    low, high = low/low.sum(), high/high.sum()
    bl, bh = low @ beta, high @ beta
    if bl <= 0 or bh <= 0:
        return None
    ret = np.array([r.ret for r in good])
    return float((low @ ret-rf)/bl - (high @ ret-rf)/bh)


def factor_period(date: dt.date, rows: Iterable[Stock], cutoff: dt.datetime, rf: float | None) -> dict:
    # Boundary before materializing inputs or doing any portfolio computation (R5).
    if not START <= date <= END:
        raise ValueError('factor return date outside frozen 2013-2022 window')
    stocks = visible(rows, cutoff)
    out = dict.fromkeys(FACTORS)
    m = vw(stocks)
    out['MktRF'] = None if m is None or rf is None else m - rf
    smb = []
    for name, signal, sign in [('HML', 'bm', 1), ('RMW', 'op', 1), ('CMA', 'investment', -1),
                               ('UMD', 'momentum', 1), ('STREV', 'strev', -1), ('LTREV', 'ltrev', -1)]:
        ls, sz = sort_2x3(stocks, signal)
        out[name] = None if ls is None else sign * ls
        if name in ('HML', 'RMW', 'CMA'):
            smb.append(sz)
    out['SMB'] = mean(smb)
    out.update(q_factors(stocks))
    out['BAB'] = bab(stocks, rf)
    return {'session_date': date, 'available_at': dt.datetime.combine(date, dt.time(22), dt.UTC),
            'n_universe': len(stocks), 'config_sha256': config_hash(), **out}


def bind_snapshot(manifest: Path, expected_sha256: str, required_stage: str) -> dict:
    """Validate exact final snapshot and every declared output; no directory/default fallback."""
    raw = manifest.read_bytes()
    if hashlib.sha256(raw).hexdigest() != expected_sha256:
        raise ValueError('stale input manifest')
    obj = json.loads(raw)
    if obj.get('status') != 'complete' or obj.get('stage') != required_stage:
        raise ValueError('input is not the explicit completed stage')
    files = obj.get('files', {})
    if not files:
        raise ValueError('input has no output hashes')
    for name, identity in files.items():
        path = (manifest.parent / name).resolve()
        if not path.is_relative_to(manifest.parent.resolve()):
            raise ValueError('output escapes snapshot')
        if path.stat().st_size != identity['bytes']:
            raise ValueError('partial/stale output size')
        h = hashlib.sha256()
        with path.open('rb') as f:
            for block in iter(lambda: f.read(1024 * 1024), b''):
                h.update(block)
        if h.hexdigest() != identity['sha256']:
            raise ValueError('partial/stale output hash')
    if manifest.read_bytes() != raw:
        raise ValueError('producer changed manifest during verification')
    return obj


def require_release(binding: dict) -> dict:
    stages = ('fundamentals_v11', 'compustat_std_v11', 'ratios_v11', 'universe', 'prices_history')
    if set(binding) != set(stages):
        raise ValueError('all explicit final release bindings required; old stages forbidden')
    return {stage: bind_snapshot(Path(binding[stage]['manifest']), binding[stage]['sha256'], stage) for stage in stages}


def build(prepared: Path, binding: dict, destination: Path) -> Path:
    """Stream certified prepared issuer-period rows into immutable generation.

    Prepared input is an explicit adapter contract, not a permissive inferred lake
    join. Each JSONL row includes exact release/config identities and formation clocks.
    One period is materialized; max 20,000 issuer rows. Source adapter must supply
    dated membership, issuer cap certification, lagged ME, and source provenance.
    """
    from . import common as C
    require_release(binding)
    generation = destination / ('generation=' + uuid.uuid4().hex)
    generation.mkdir(parents=True, exist_ok=False)
    digest = C.sha256_file(prepared)
    config = config_hash()
    identities = {k: v['sha256'] for k, v in binding.items()}
    output = generation / 'factors.jsonl'
    active, stocks, meta, count = None, [], None, 0
    def flush(handle):
        nonlocal count
        if meta is None:
            return
        result = factor_period(active[1], stocks, meta['cutoff'], meta['rf'])
        result['freq'] = active[0]
        handle.write(json.dumps(result, default=str, sort_keys=True)+'\n')
        count += 1
    with prepared.open(encoding='utf-8') as source, output.open('x', encoding='utf-8') as target:
        for line in source:
            row = json.loads(line)
            date = dt.date.fromisoformat(row['session_date'])
            # Reject before any return is parsed/aggregated.
            if not START <= date <= END:
                raise ValueError('prepared return date outside holdout boundary')
            key = (row['freq'], date)
            if key[0] not in ('D', 'M') or (active is not None and key < active):
                raise ValueError('prepared periods must be sorted by frequency/date')
            if row['config_sha256'] != config or row['input_manifests_sha256'] != identities:
                raise ValueError('stale prepared input/config')
            cutoff = dt.datetime.fromisoformat(row['cutoff'])
            formation = dt.date.fromisoformat(row['formation_date'])
            formation_close = dt.datetime.combine(formation, dt.time(22), dt.UTC)
            if cutoff.tzinfo is None or cutoff > formation_close or formation >= date or row['membership_basis'] != 'dated_history' or row['cap_basis'] != 'exact_source_certified':
                raise ValueError('unknown/future eligibility or uncertified economic basis')
            if active != key:
                flush(target)
                active, stocks = key, []
                meta = {'cutoff': cutoff, 'rf': row['rf']}
            if meta != {'cutoff': cutoff, 'rf': row['rf']}:
                raise ValueError('inconsistent period cutoff/RF')
            if len(stocks) >= 20000:
                raise ValueError('bounded period issuer limit exceeded')
            stocks.append(Stock(row['cik'], row['me_lag'], row['ret'], row['nyse'], row['signals'],
                                dt.datetime.fromisoformat(row['source_available_at']), True))
        flush(target)
    if C.sha256_file(prepared) != digest:
        raise ValueError('prepared source changed during build')
    require_release(binding)  # R9 recheck before publish-last pointer.
    manifest = {'stage': 'factors', 'schema': 'atx.alpha-panel.factors/checkpoint-v1', 'status': 'complete',
                'rows': count, 'config': CONFIG, 'config_sha256': config,
                'input_manifests_sha256': identities, 'prepared_sha256': digest,
                'files': {'factors.jsonl': {'bytes': output.stat().st_size, 'sha256': C.sha256_file(output)}},
                'gate_status': {'production_acceptance': 'FAIL', 'reason': 'source certification and real gates pending'}}
    C.write_json_atomic(generation/'manifest.json', manifest)
    C.write_json_atomic(destination/'manifest.json', {**manifest, 'generation': generation.name,
        'files': {generation.name+'/'+k: v for k, v in manifest['files'].items()}})
    return generation/'manifest.json'


def benchmark_gates(own: list[dict], reference: list[dict]) -> dict:
    """Exact 120-month denominator; never correlate a shortened available window."""
    thresholds = {'MktRF': .99, 'SMB': .90, 'UMD': .90, 'HML': .85,
                  'RMW': .75, 'CMA': .75, 'q_IA': .75, 'q_ROE': .75}
    for row in own + reference:
        if row['session_date'] > END:
            raise ValueError('post-2022 benchmark input')
    def index(rows):
        out = {}
        for row in rows:
            key = (row['session_date'].year, row['session_date'].month)
            if key in out:
                raise ValueError('duplicate benchmark month')
            out[key] = row
        return out
    a, b = index(own), index(reference)
    months = [(y, m) for y in range(2013, 2023) for m in range(1, 13)]
    gates = {}
    for factor, threshold in thresholds.items():
        pairs = [(a.get(k, {}).get(factor), b.get(k, {}).get(factor)) for k in months]
        missing = sum(x is None or y is None or not math.isfinite(x) or not math.isfinite(y) for x, y in pairs)
        rho = None
        if missing == 0:
            x, y = np.array(pairs).T
            if np.std(x) > 0 and np.std(y) > 0:
                rho = float(np.corrcoef(x, y)[0, 1])
        gates[factor] = {'status': 'PASS' if rho is not None and rho >= threshold else 'FAIL',
                         'rho': rho, 'threshold': threshold, 'required_months': 120, 'missing_months': missing}
    return gates


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--release-bindings', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    build(args.prepared, json.loads(args.release_bindings.read_text(encoding='utf-8')), args.destination)


if __name__ == '__main__':
    main()


def monthly_compound(daily: Iterable[dict]) -> dict[str, float | None]:
    rows = []
    for r in daily:
        if r['session_date'] > END:
            raise ValueError('post-2022 aggregate input')
        rows.append(r)
    if len({(r['session_date'].year, r['session_date'].month) for r in rows}) != 1:
        raise ValueError('one bounded month required')
    # Long-short spreads cannot be compounded as if they were self-financing asset returns.
    raise NotImplementedError('monthly factors require monthly stock returns and the frozen formation portfolios')
