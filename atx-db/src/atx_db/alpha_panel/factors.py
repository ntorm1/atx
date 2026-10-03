"""Own daily/monthly factors using explicit final source snapshots and frozen rules.

Native adapters stream bounded formation/return units into immutable Parquet
generations. Source qualification and real benchmark acceptance remain separate.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
import argparse
import os
import uuid
import itertools
import calendar
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
    'q_formation': 'independent 2x3x3; ME/IA annual June; ROE monthly; EG separate monthly 2x3',
    'q_EG': 'one-year dIA forecast; WLS ME weights; mean prior120 monthly coefficients minimum30; predictors winsorized1/99; all outcome clocks strict',
    'bab': 'rank beta; each leg dollar-weighted by distance from median rank; beta-scaled excess returns',
    'beta': '252 sessions, minimum 200 complete observations; intercept OLS',
    'bab_beta': 'daily log volatility252 min120; overlapping3day log correlation1260 min750; .6 beta+.4',
    'missing': 'NULL if any required portfolio leg absent; no zero filling',
    'benchmark_join': 'exact calendar month; 120 required months 2013-2022; no window shrink',
    'cap_uncertainty': 'exclude unresolved share-basis inputs including CIK 77159 until exact-source audit',
    'benchmark_thresholds': {'MktRF': .99, 'SMB': .90, 'UMD': .90, 'HML': .85,
                            'RMW': .75, 'CMA': .75, 'q_IA': .75, 'q_ROE': .75},
    'style_formulas': {'size':'log certified issuer ME','value':'BE/ME','momentum':'ret12_1',
        'volatility':'daily21 std ddof1','liquidity':'negative Amihud21','growth':'ATq/ATq4-1',
        'leverage':'debt_at','quality':'gprof'},
    'style_normalization': 'sample z score over certified all_common; no clipping',
    'staleness_days': {'accounting':400,'liquidity':10,'FTD':60,'threshold':10},
    'risk_holdout': 'all regressions missing after2022; latest nonreturn primitives allowed',
    'source_schema': 'provisional inspected native schema; missing REVT remainsmissing, no SALE substitution',
    'references': [
        'https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/f-f_5_factors_2x3.html',
        'https://theinvestmentcapm.com/uploads/1/2/2/6/122679606/q5_2020jan_stylerf.pdf',
        'https://theinvestmentcapm.com/uploads/1/2/2/6/122679606/securityanalysis_internetappendix_2019august.pdf',
        'https://theinvestmentcapm.com/uploads/1/2/2/6/122679606/q5_2018nov.pdf',
        'https://doi.org/10.1016/j.jfineco.2013.10.005'],
}


def config_hash() -> str:
    return hashlib.sha256(json.dumps(CONFIG, sort_keys=True).encode()).hexdigest()


def months_before(date:dt.date,n:int)->dt.date:
    ordinal=date.year*12+date.month-1-n
    y,m=ordinal//12,ordinal%12+1
    return dt.date(y,m,min(date.day,calendar.monthrange(y,m)[1]))


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
    if not rows or any(r.ret is None or not math.isfinite(r.ret) or not math.isfinite(r.me) or r.me<=0 for r in rows):
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
    out['q_EG'], _ = sort_2x3(rows, 'eg')
    return out


def eg_forecast(training: list[dict], predictors: list[dict], cutoff: dt.datetime) -> dict[int, float | None]:
    """Original q5 rolling WLS forecast, with all training outcomes independently PIT.

    Training rows have one fiscal one-year dIA target, log_q/Cop/dRoe predictors,
    ME weights and *both* outcome/predictor source clocks. The average of up to
    120 visible monthly slope estimates (minimum30) forms the one-year forecast.
    No forecast is imputed when the source histories are insufficient.
    """
    if cutoff.tzinfo is None:
        raise ValueError('UTC forecast cutoff required')
    keys = ('log_q', 'cop', 'droe')
    output = dict.fromkeys(r['cik'] for r in predictors)
    visible_predictors = [r for r in predictors if r.get('available_at') is not None and
        r['available_at'] < cutoff and all(r.get(k) is not None and math.isfinite(r[k]) for k in keys)]
    if not visible_predictors:
        return output
    slopes = []
    lower = cutoff.date().year*12+cutoff.date().month-120
    upper = cutoff.date().year*12+cutoff.date().month
    previous_month = None
    for month, group in itertools.groupby(training,key=lambda r:r['fit_month']):
        if previous_month is not None and month<=previous_month:
            raise ValueError('EG training months must be sorted/unique groups')
        previous_month=month
        ordinal = month.year*12+month.month
        if not lower <= ordinal < upper:
            continue
        records=[]
        for row in group:
            clocks=[row['outcome_available_at'],row['predictor_available_at']]
            if any(t.tzinfo is None for t in clocks):
                raise ValueError('training source clock missing')
            fit_cutoff=row['fit_cutoff']
            if fit_cutoff>=cutoff or any(t>=fit_cutoff for t in clocks):
                continue
            vals=[row.get(k) for k in ('dia',*keys,'me')]
            if any(v is None or not math.isfinite(v) for v in vals) or row['me']<=0:
                continue
            if row['outcome_period_end']>months_before(fit_cutoff.date(),4):
                continue
            records.append(row)
            if len(records)>20000:
                raise ValueError('EG bounded monthly cross-section exceeded')
        if len(records) < 5:
            continue
        x = np.array([[r[k] for k in keys] for r in records])
        x = np.clip(x, *np.quantile(x, [.01,.99], axis=0))
        x = np.column_stack((np.ones(len(x)), x))
        weights = np.sqrt([r['me'] for r in records])
        if np.linalg.matrix_rank(x) < 4:
            continue
        y = np.array([r['dia'] for r in records])
        slopes.append(np.linalg.lstsq(x*weights[:,None], y*weights, rcond=None)[0])
    if len(slopes) < 30:
        return output
    b = np.mean(slopes[-120:], axis=0)
    x = np.array([[r[k] for k in keys] for r in visible_predictors])
    x = np.clip(x, *np.quantile(x,[.01,.99],axis=0))
    for row, values in zip(visible_predictors,x):
        output[row['cik']] = float(np.r_[1,values]@b)
    return output


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


def bab_beta(stock: list[float | None], market: list[float | None]) -> float | None:
    if len(stock)!=len(market) or len(stock)>1260:
        raise ValueError('bounded aligned BAB history required')
    x=np.array([np.nan if v is None or v<=-1 else math.log1p(v) for v in stock])
    y=np.array([np.nan if v is None or v<=-1 else math.log1p(v) for v in market])
    valid=np.isfinite(x[-252:])&np.isfinite(y[-252:])
    if valid.sum()<120:
        return None
    sx,sy=np.std(x[-252:][valid],ddof=1),np.std(y[-252:][valid],ddof=1)
    if sy==0:
        return None
    x3=np.convolve(x,np.ones(3),'valid')
    y3=np.convolve(y,np.ones(3),'valid')
    good=np.isfinite(x3)&np.isfinite(y3)
    if good.sum()<750 or np.std(x3[good])==0 or np.std(y3[good])==0:
        return None
    return float(.6*np.corrcoef(x3[good],y3[good])[0,1]*sx/sy+.4)


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


def benchmark_gates(own: list[dict], reference: list[dict]) -> dict:
    """Exact 120-month denominator; never correlate a shortened available window."""
    thresholds = CONFIG['benchmark_thresholds']
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
    parser.add_argument('--native-spec', type=Path, required=True)
    parser.add_argument('--phase',choices=('build','validate'),default='build')
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    if not os.environ.get('ATX_GUARD_JOB'):
        raise RuntimeError('native data CLI requires run_memory_guarded.py')
    from ._factor_native import native_factors,validate_benchmarks
    spec=json.loads(args.native_spec.read_text(encoding='utf-8'))
    if args.phase=='build':
        native_factors(spec,args.destination)
    else:
        validate_benchmarks(spec,args.destination)


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
