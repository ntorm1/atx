"""Native risk pack and bounded line exposure kernels; missing histories stay NULL.

The native CLI binds final release snapshots and immutable own-factor outputs.
Real source qualification and coverage must be measured independently.
"""
from __future__ import annotations

import datetime as dt
import argparse
import json
import uuid
import os
from pathlib import Path
import numpy as np


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


def exposure(session: dt.date, observations: list[dict], *, cutoff=None) -> dict:
    if len(observations) > 252:
        raise ValueError('bounded lookback required')
    dates = [r['session_date'] for r in observations]
    if dates != sorted(set(dates)) or any(d >= session for d in dates):
        raise ValueError('duplicate/unsorted/future predictor observations')
    if not observations:return {'beta_252':None,'ff5_betas':[None]*5,'ivol_ff5_252':None,'n_obs_capm':0,'n_obs_ff5':0,'risk_missing_reason':'missing_qualified_trailing_history','available_at':dt.datetime.combine(session,dt.time(22),dt.UTC)}
    if observations and cutoff is None:raise ValueError('pinned previous-session cutoff required')
    cutoff=cutoff or dt.datetime.combine(session-dt.timedelta(days=1),dt.time(22),dt.UTC)
    if cutoff.tzinfo is None or cutoff>dt.datetime.combine(session-dt.timedelta(days=1),dt.time(22),dt.UTC):raise ValueError('invalid historical predictor cutoff')
    for row in observations:
        for key in ('available_at','source_available_at','factor_available_at'):
            clock=row.get(key)
            if clock is not None and (clock.tzinfo is None or clock>=cutoff):raise ValueError('unavailable historical predictor observation')
    observations=[r if r.get('factor_available_at') is not None else {**r,**dict.fromkeys(FF5,float('nan'))} for r in observations]
    returns = [r.get('ret', np.nan) for r in observations]
    rf = [r.get('RF', np.nan) for r in observations]
    capm = regress(returns, [[r.get('MktRF', np.nan)] for r in observations], rf)
    ff = regress(returns, [[r.get(k, np.nan) for k in FF5] for r in observations], rf)
    return {'beta_252': capm['betas'][0], 'ff5_betas': ff['betas'], 'ivol_ff5_252': ff['idio_vol'],
            'n_obs_capm': capm['n_obs'], 'n_obs_ff5': ff['n_obs'],
            'risk_missing_reason':None if capm['betas'][0] is not None else 'missing_qualified_trailing_history',
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
        history.append({**r, 'session_date': dt.date.fromisoformat(r['session_date']),
                        **{key:dt.datetime.fromisoformat(r[key]) if isinstance(r.get(key),str) else r.get(key) for key in ('available_at','source_available_at','factor_available_at')}})
    out.update(exposure(day, history,cutoff=cutoff))
    out['missing_primitives'] = missing
    out['cap_quality'] = row.get('cap_quality', 'uncertified')
    if out['cap_quality'] != 'exact_source_certified':
        for field in STYLES:
            out[field] = None
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-spec', type=Path, required=True)
    parser.add_argument('--start',type=dt.date.fromisoformat,default=dt.date(2020,1,1))
    parser.add_argument('--end',type=dt.date.fromisoformat,required=True)
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    if not os.environ.get('ATX_GUARD_JOB'):
        raise RuntimeError('native data CLI requires run_memory_guarded.py')
    from ._factor_native import native_risk
    native_risk(json.loads(args.native_spec.read_text(encoding='utf-8')),args.destination,args.start,args.end)


if __name__ == '__main__':
    main()
