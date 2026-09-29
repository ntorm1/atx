"""Pitch iteration 3 (platform v7): seven config-driven sections of the pitch report.

Layout block ``type`` <- config key:
- ``capacity_curve`` <- ``capacity_curve``: net Sharpe and cost per traded dollar of the S2 book replayed at NAV
  multiples of the base NAV (the replay's capacity pass, v7_extras.json), the S2-KO / S2-FIM cost-law books beside S2;
- ``t_risk_bias`` <- ``risk_bias``: bias statistic b by portfolio family (factor, random, book when present), the
  structural-factor count and the drop counters of the risk-model bias harness;
- ``t_report_cards`` <- ``report_cards``: one compact row per library member per card set, detail on expand;
- ``t_monitor_baseline`` <- ``monitor_baseline``: M1-M4 status, alarm / warn / ok counts, the alarming sleeve named;
- ``ops_loop`` <- ``ops_loop``: the daily operating loop (decide parity and wall time, share orders, reconcile
  self-breaks and planted-break catches, holdings-emission overhead);
- ``t_integrity`` <- ``integrity``: cell-count DSR beside effective-N DSR (never substituted), Lo-null DSR, PSR(0),
  MinTRL, CSCV PBO and the pre-registered freeze gate;
- ``t_trial_ledger`` <- ``trial_ledger``: every ledger trial with its verdict, N over the ledger, validation spent.

An input is a path relative to the report root or ``{"path": ..., "sha256": ...}``; a pinned SHA-256 must equal the
file's as read (the Registry hashes and lists every file) or the input is refused. Every JSON input's schema id is
checked. A block whose input is absent, unparseable, refused or of another schema raises, and the report renders its
one-line "not available" marker instead; absence inside a block (one card set, one book, one decision) renders n/a cells
and a named note. Captions name the file (and SHA-256 prefix) behind every number.
"""
from __future__ import annotations

import json
import math
import re
from pathlib import Path

import numpy as np

from . import components as C
from . import data as D
from . import narrative as N

SCHEMAS = {
    'extras': ('atx.nav-v7-extras/v1',), 'bias': ('atx.risk-bias/v1', 'atx.risk-bias/v2'),
    'card_index': ('atx.alpha-report-card-index/v1',), 'card': ('atx.alpha-report-card/v1',),
    'monitor': ('atx.book-monitor/v1',), 'decision': ('atx.book-decision/v1',), 'reconcile': ('atx.book-reconcile/v1',),
    'receipt': ('atx.bounded-research-run/v1',), 'holdings': ('atx.nav-holdings/v1', 'atx.nav-holdings/v2'),
    'ledger': ('atx.trial-ledger/v1',), 'risk_manifest': ('atx.risk-model/v1',),
}
GRID_CAP_DAYS = 999.5  # the card's decay fit searches half-lives on 0.25..1000 days; 1000 = no decay seen
STATUS_KIND = {'ok': 'ok', 'warn': 'warn', 'alarm': 'alarm', 'n/a': 'n/a', 'complete': 'ok', 'completed': 'ok'}


# ============================================================================================ inputs
def _spec(v) -> tuple[str, str | None]:
    if isinstance(v, str) and v:
        return v, None
    if isinstance(v, dict) and isinstance(v.get('path'), str) and v['path']:
        return v['path'], v.get('sha256')
    raise ValueError(f'input spec {str(v)[:80]!r}: expected a path or {{"path", "sha256"}}')


def _key(ctx, rel: str) -> str:
    return ctx.reg.rel(ctx.reg.path(rel))


def _sha(ctx, rel: str) -> str | None:
    return (ctx.reg.files.get(_key(ctx, rel)) or {}).get('sha256')


def _src(ctx, rel: str) -> str:
    """``path (sha256 <12 hex>)`` of a file the Registry read, or the bare path."""
    s = _sha(ctx, rel)
    return f'{rel} (sha256 {s[:12]})' if s else rel


def _load(ctx, spec, what: str, *, text: bool = False, schemas=None):
    """Read one input (JSON by default); refuse absent, unparseable, pin-mismatched or wrong-schema files."""
    rel, pin = _spec(spec)
    data = ctx.reg.read_text(rel) if text else ctx.reg.read_json(rel)
    st = ctx.reg.files.get(_key(ctx, rel)) or {}
    if data is None:
        if st.get('status') == 'unparseable json':
            raise ValueError(f'{what}: {rel} is not valid JSON')
        raise FileNotFoundError(f'{what}: {rel} ({st.get("status", "missing")})')
    if pin and st.get('sha256') != pin:
        raise ValueError(f'{what}: {rel} sha256 {str(st.get("sha256"))[:12]} != pinned {str(pin)[:12]}')
    if schemas is not None:
        sch = data.get('schema') if isinstance(data, dict) else None
        if sch not in schemas:
            raise ValueError(f'{what}: {rel} schema {sch!r}, expected {" or ".join(schemas)}')
    return data


def _try(ctx, spec, what: str, **kw):
    """(data, None) or (None, reason) -- for inputs whose absence only blanks part of a block."""
    try:
        return _load(ctx, spec, what, **kw), None
    except (OSError, ValueError) as e:
        return None, f'{type(e).__name__}: {e}'


def _conf(ctx, key: str) -> dict:
    c = ctx.cfg.get(key)
    if not isinstance(c, dict):
        raise KeyError(f'config key {key!r} not set')
    return c


def _need(ctx, name: str) -> dict:
    res = ctx.analysis(name)
    if isinstance(res, dict) and '_error' in res:
        raise RuntimeError(f'analysis {name}: {res["_error"]}')
    return res


def _txt(ctx, s) -> str:
    return N.fill_text(ctx, s) if s else ''


def _num(v):
    return v if C.is_num(v) else None


def _div(a, b, k: float = 1.0):
    return a / b * k if C.is_num(a) and C.is_num(b) and b != 0 else None


def _diff(a, b):
    return a - b if C.is_num(a) and C.is_num(b) else None


def _mn(x, k=1e6):
    return x / k if C.is_num(x) else None


def _with_sub(spec: str, sub_key: str):
    """Table fmt: the value, then its ``sub_key`` text in small type (as the pitch's value (SE) cells)."""
    def f(v, row):
        s = C.fmt(v, spec)
        if s is None:
            return None
        sub = row.get(sub_key)
        return C.esc(s) + (f' <span class="sub" style="display:inline">({C.esc(sub)})</span>' if sub else '')
    return f


def _note(text: str) -> str:
    return f'<p class="ops-note">{C.esc(text)}</p>' if text else ''


# ============================================================================================ 1. capacity curve
def _crossing(points, thr):
    """First NAV at which the series falls through ``thr`` (linear in log NAV between grid points)."""
    ps = [(x, y) for x, y in points if C.is_num(x) and x > 0 and C.is_num(y)]
    if not ps or not C.is_num(thr):
        return None, 'n/a'
    if ps[0][1] < thr:
        return None, f'below {C.fmt(thr, "g")} already at the smallest NAV'
    for (xa, ya), (xb, yb) in zip(ps, ps[1:]):
        if ya >= thr > yb:
            t = (ya - thr) / (ya - yb)
            return math.exp(math.log(xa) + t * (math.log(xb) - math.log(xa))), 'interpolated'
    return None, f'at or above {C.fmt(thr, "g")} at every NAV of the grid'


def _daily_costs(ctx, rel: str, pin=None) -> dict:
    """Summed traded and trade-cost dollars of a daily CSV (all rows) and its SHA-256."""
    text = _load(ctx, {'path': rel, 'sha256': pin} if pin else rel, 'daily CSV', text=True)
    d = D.parse_daily(text, rel, columns=('session_ns', 'return_observation', 'net_return', 'traded_dollars',
                                          'trade_cost_dollars', 'linear_cost_dollars', 'impact_cost_dollars'))
    if d is None or 'traded_dollars' not in d.cols:
        raise ValueError(f'daily CSV {rel}: no traded_dollars column')
    tr = float(np.nansum(d.cols['traded_dollars']))
    if 'trade_cost_dollars' in d.cols:
        cost = float(np.nansum(d.cols['trade_cost_dollars']))
    else:
        cost = float(np.nansum(d.cols.get('linear_cost_dollars', np.array([np.nan])))
                     + np.nansum(d.cols.get('impact_cost_dollars', np.array([np.nan]))))
    return {'traded': tr, 'cost': cost, 'cost_bps': _div(cost, tr, 1e4), 'sha256': _sha(ctx, rel)}


def an_capcurve(ctx) -> dict:
    cc = _conf(ctx, 'capacity_curve')
    d = str(cc.get('dir', '')).rstrip('/')
    ex_spec = cc.get('extras') or f'{d}/v7_extras.json'
    ex = _load(ctx, ex_spec, 'capacity extras', schemas=SCHEMAS['extras'])
    raw = ex.get('capacity')
    if not isinstance(raw, list) or not raw:
        raise ValueError(f'capacity extras {_spec(ex_spec)[0]}: no capacity[] rows')
    rows = []
    for r in raw:
        if not isinstance(r, dict) or not C.is_num(r.get('multiple')) or r['multiple'] <= 0:
            raise ValueError(f'capacity row without a positive multiple: {str(r)[:80]}')
        rows.append(dict(r))
    rows.sort(key=lambda r: r['multiple'])
    base = cc.get('base_nav')
    if not C.is_num(base):
        one = next((r for r in rows if r['multiple'] == 1 and C.is_num(r.get('equivalent_initial_nav'))), None)
        base = one['equivalent_initial_nav'] if one else None
    for r in rows:
        nav = r.get('equivalent_initial_nav')
        r['nav'] = nav if C.is_num(nav) else (r['multiple'] * base if C.is_num(base) else None)
        r['nav_bn'] = _mn(r['nav'], 1e9)
    thr = cc.get('sr_threshold', 1.0)
    cross, how = _crossing([(r['nav'], r.get('net_sharpe')) for r in rows], thr)
    x1 = next((r for r in rows if r['multiple'] == 1), None)
    x_max = rows[-1]
    out = {'rows': rows, 'base_nav': base, 'base_nav_bn': _mn(base, 1e9), 'threshold': thr, 'cross_nav': cross,
           'cross_nav_bn': _mn(cross, 1e9), 'cross_how': how, 'rule': ex.get('capacity_rule'),
           'x1_identity': ex.get('capacity_x1_equals_primary_bit_for_bit'), 'extras_src': _src(ctx, _spec(ex_spec)[0]),
           'x1': x1, 'max': x_max, 'max_multiple': x_max['multiple'],
           'sr_retained_max': _div(x_max.get('net_sharpe'), (x1 or {}).get('net_sharpe')),
           'cost_rise_max': _diff(x_max.get('cost_bps_per_traded_dollar'), (x1 or {}).get('cost_bps_per_traded_dollar'))}
    # the cost-law books beside the primary: summary.json scenario block + daily CSV traded / cost dollars
    summ_spec = cc.get('summary') or f'{d}/summary.json'
    summ, summ_err = _try(ctx, summ_spec, 'stress summary')
    scen = {s.get('scenario'): s for s in (summ or {}).get('scenarios') or [] if isinstance(s, dict)}
    books, notes = [], []
    for b in [cc.get('primary')] + list(cc.get('beside') or []):
        if not isinstance(b, dict) or not b.get('scenario'):
            continue
        sc = scen.get(b['scenario'])
        row = {'label': b.get('label', b['scenario']), 'scenario': b['scenario'], 'token': b.get('token', 's2'),
               'primary': b is cc.get('primary')}
        if sc is None:
            notes.append(f"{row['label']}: scenario {b['scenario']} not in {_spec(summ_spec)[0]}"
                         + (f' ({summ_err})' if summ_err else ''))
        else:
            cap = sc.get('capacity') or {}
            row.update({'net_sharpe': _num(sc.get('net_sharpe')), 'gross_sharpe': _num(sc.get('gross_sharpe')),
                        'ann_mean_net': _num(sc.get('ann_mean')), 'ann_vol': _num(sc.get('ann_vol')),
                        'max_drawdown': _num(sc.get('max_drawdown')),
                        'trade_cost_dollars': _num(D.dig(sc, 'costs.trade_cost_dollars')),
                        'capped_share': _div(cap.get('capped_fills'), cap.get('fills')),
                        'unfilled_dollars': _num(cap.get('unfilled_dollars')),
                        'participation_p95_bound': _num(cap.get('participation_p95_upper_bound')),
                        'daily_turnover_gmv_mean': _num(D.dig(sc, 'daily_turnover_gmv.mean')),
                        'financing_dollars': _num(D.dig(sc, 'financing.total_financing_dollars')),
                        'csv_sha_summary': sc.get('daily_csv_sha256')})
        rel = f"{d}/daily_{b['scenario']}.csv"
        try:
            dc = _daily_costs(ctx, rel, b.get('daily_sha256'))
            row.update({'traded_dollars': dc['traded'], 'cost_bps_per_traded_dollar': dc['cost_bps'],
                        'daily_src': _src(ctx, rel),
                        'csv_ok': (dc['sha256'] == row['csv_sha_summary']) if row.get('csv_sha_summary') else None})
        except (OSError, ValueError) as e:
            notes.append(f"{row['label']}: cost per traded dollar n/a ({type(e).__name__}: {e})")
        books.append(row)
    out.update({'books': books, 'book': {b['label']: b for b in books}, 'notes': notes,
                'summary_src': _src(ctx, _spec(summ_spec)[0]) if summ else None, 'label': cc.get('label', 'S2 book')})
    return out


def blk_capacity_curve(ctx, spec) -> str:
    cc = _need(ctx, 'capcurve')
    rows, books = cc['rows'], cc['books']
    thr = cc['threshold']
    x_ticks = [r['nav_bn'] for r in rows if C.is_num(r['nav_bn'])]
    x_at = cc['base_nav_bn'] if C.is_num(cc['base_nav_bn']) else 1.0
    # the base-NAV label goes below its point: the cost-law books' labels sit above it at the same x
    net = {'name': 'S2 net Sharpe', 'color': 's2', 'width': 2.4,
           'points': [(r['nav_bn'], r.get('net_sharpe'), C.fmt(r.get('net_sharpe'), '+.3f'),
                       'below' if r['nav_bn'] == x_at else 'above') for r in rows],
           'end_label': f"net {C.fmt(rows[-1].get('net_sharpe'), '+.3f') or C.NA_TEXT}"}
    gross = {'name': 'S2 gross Sharpe', 'color': 'ref-2', 'width': 1.4, 'dash': '5 3',
             'points': [(r['nav_bn'], r.get('gross_sharpe')) for r in rows],
             'end_label': f"gross {C.fmt(rows[-1].get('gross_sharpe'), '.3f') or C.NA_TEXT}"}
    cost = {'name': 'S2 cost per traded $', 'color': 's2', 'width': 2.4,
            'points': [(r['nav_bn'], r.get('cost_bps_per_traded_dollar'),
                        C.fmt(r.get('cost_bps_per_traded_dollar'), '.2f')) for r in rows],
            'end_label': f"S2 {C.fmt(rows[-1].get('cost_bps_per_traded_dollar'), '.2f') or C.NA_TEXT} bps"}
    side_sr, side_cost, lg_side = [], [], []
    for b in books:
        if b['primary']:
            continue
        side_sr.append({'name': f"{b['label']} net Sharpe", 'color': b['token'], 'width': 1.5,
                        'points': [(x_at, b.get('net_sharpe'), f"{b['label']} {C.fmt(b.get('net_sharpe'), '+.3f')}")]})
        side_cost.append({'name': f"{b['label']} cost per traded $", 'color': b['token'], 'width': 1.5,
                          'points': [(x_at, b.get('cost_bps_per_traded_dollar'),
                                      f"{b['label']} {C.fmt(b.get('cost_bps_per_traded_dollar'), '.2f')}")]})
        lg_side.append({'name': f"{b['label']} (at ${C.fmt(x_at, 'g')}bn)", 'color': b['token'], 'kind': 'dot'})
    refs = [{'value': thr, 'label': f'net SR {C.fmt(thr, "g")}'}] if C.is_num(thr) else []
    xl = 'Book NAV, $bn (log scale)'
    sr_svg = C.xy_chart([gross, net] + side_sr, x_label=xl, y_label='Sharpe ratio, annualised', x_fmt='g',
                        y_fmt='.2f', height=300, x_log=True, x_ticks=x_ticks, y_refs=refs, aria='capacity: Sharpe vs NAV')
    cost_svg = C.xy_chart([cost] + side_cost, x_label=xl, y_label='Cost per traded dollar, bps', x_fmt='g',
                          y_fmt='.1f', height=260, x_log=True, x_ticks=x_ticks, aria='capacity: cost vs NAV')
    lg = C.legend([{'name': 'S2 net Sharpe / cost per traded $', 'color': 's2', 'width': 2.4},
                   {'name': 'S2 gross Sharpe', 'color': 'ref-2', 'dash': '5 3', 'width': 1.5}] + lg_side
                  + ([{'name': f'Net SR {C.fmt(thr, "g")}', 'color': 'fg-3', 'dash': '4 3', 'width': 1}] if refs else []))
    body = (f'<p class="panel-label">(a) Net and gross Sharpe vs book NAV</p>{sr_svg}'
            f'<p class="panel-label">(b) Cost per traded dollar vs book NAV</p>{cost_svg}')
    cross = (f"net SR {C.fmt(thr, 'g')} is crossed near ${C.fmt(cc['cross_nav_bn'], '.1f')}bn ({cc['cross_how']} "
             f"linearly in log NAV between grid points: a derived reading, not a replay)"
             if C.is_num(cc['cross_nav_bn']) else f"net SR {C.fmt(thr, 'g')}: {cc['cross_how']}")
    ident = {True: 'yes', False: 'NO'}.get(cc['x1_identity'], C.NA_TEXT)
    srcs = '; '.join(x for x in [cc['extras_src'], cc.get('summary_src')] if x)
    daily = ', '.join(b['daily_src'] for b in books if b.get('daily_src'))
    cap = (f"Capacity curve of {cc['label']}: the S2 book replayed at NAV multiples "
           f"{', '.join(C.fmt(r['multiple'], 'g') for r in rows)} of ${C.fmt(cc['base_nav_bn'], 'g')}bn with the "
           f"square-root impact scaled by m^0.5 and the 1%-of-ADV cap binding on the NAV-m book; {cross}. Beside it at "
           f"${C.fmt(x_at, 'g')}bn, the same S2 book priced with the other pre-registered impact laws "
           f"({', '.join(b['label'] for b in books if not b['primary']) or 'none configured'}); x1 row = S2 bit for "
           f"bit: {ident}. Sources: {srcs}" + (f"; cost-law cost per traded $ from {daily}" if daily else '') + '.')
    fig = C.figure(ctx.next_fig(), body, cap, 'fig-capacity-curve', lg)
    trows = [{'_group': f"NAV multiples of ${C.fmt(cc['base_nav_bn'], 'g')}bn (S2 book, capacity pass)"}]
    for r in rows:
        trows.append({'book': f"{C.fmt(r['multiple'], 'g')}x", 'nav': r['nav_bn'], 'net': _num(r.get('net_sharpe')),
                      'gross': _num(r.get('gross_sharpe')), 'mu': _num(r.get('ann_mean_net')), 'vol': _num(r.get('ann_vol')),
                      'mdd': _num(r.get('max_drawdown')), 'cb': _num(r.get('cost_bps_per_traded_dollar')),
                      'tr': _mn(r.get('traded_dollars'), 1e9), 'tc': _mn(r.get('trade_cost_dollars')),
                      'cs': _num(r.get('capped_share')), 'uf': _mn(r.get('unfilled_dollars')),
                      'pp': _num(r.get('participation_p95_bound')), 'tau': _num(r.get('daily_turnover_gmv_mean')),
                      'fin': _mn(r.get('financing_dollars')), 'ok': '', '_cls': 'hl' if r['multiple'] == 1 else ''})
    if books:
        trows.append({'_group': f"Impact laws at ${C.fmt(x_at, 'g')}bn (same S2 construction; summary.json + daily CSV)"})
    for b in books:
        trows.append({'book': b['label'] + (' (primary)' if b['primary'] else ''), 'nav': x_at,
                      'net': b.get('net_sharpe'), 'gross': b.get('gross_sharpe'), 'mu': b.get('ann_mean_net'),
                      'vol': b.get('ann_vol'), 'mdd': b.get('max_drawdown'), 'cb': b.get('cost_bps_per_traded_dollar'),
                      'tr': _mn(b.get('traded_dollars'), 1e9), 'tc': _mn(b.get('trade_cost_dollars')),
                      'cs': b.get('capped_share'), 'uf': _mn(b.get('unfilled_dollars')),
                      'pp': b.get('participation_p95_bound'), 'tau': b.get('daily_turnover_gmv_mean'),
                      'fin': _mn(b.get('financing_dollars')), 'ok': b.get('csv_ok'), '_cls': 'hl' if b['primary'] else ''})
    nets = [r['net'] for r in trows if C.is_num(r.get('net'))]
    cols = [{'key': 'book', 'label': 'Book', 'kind': 'text'}, {'key': 'nav', 'label': 'NAV $bn', 'fmt': 'g'},
            {'key': 'net', 'label': 'Net SR', 'fmt': '+.3f', 'bar': {'lo': min([0.0] + nets), 'hi': max([0.0] + nets)}},
            {'key': 'gross', 'label': 'Gross SR', 'fmt': '.3f'}, {'key': 'mu', 'label': 'Net mean/yr', 'fmt': '+pct2'},
            {'key': 'vol', 'label': 'Vol/yr', 'fmt': 'pct2'}, {'key': 'mdd', 'label': 'MDD', 'fmt': 'pct1'},
            {'key': 'cb', 'label': 'Cost bps/$', 'fmt': '.2f'}, {'key': 'tr', 'label': 'Traded $bn', 'fmt': '.1f'},
            {'key': 'tc', 'label': 'Trade cost $M', 'fmt': '.1f'},
            {'key': 'cs', 'label': 'Capped share of fills', 'fmt': 'pct2'},
            {'key': 'uf', 'label': 'Unfilled $M', 'fmt': '.1f'},
            {'key': 'pp', 'label': 'Participation p95 (bound)', 'fmt': 'pct2'},
            {'key': 'tau', 'label': 'Tau mean', 'fmt': '.4f'}, {'key': 'fin', 'label': 'Financing $M', 'fmt': '.1f'},
            {'key': 'ok', 'label': 'Daily CSV = summary', 'kind': 'chip'}]
    tab = C.table(cols, trows, num=ctx.next_tab(), sortable=False, tid='t-capacity-curve',
                  caption=(f"Capacity and cost-law books: Sharpe, return, cost per traded dollar, traded and cost "
                           f"dollars, participation-cap frictions and financing (dollar columns of the capacity rows are "
                           f"the NAV-m book's divided by m, as the replay declares). Sources: {srcs}."))
    notes = ''.join(_note(n) for n in cc['notes'])
    return fig + tab + notes


# ============================================================================================ 2. risk model bias
def an_bias(ctx) -> dict:
    rb = _conf(ctx, 'risk_bias')
    summ = _load(ctx, rb.get('summary'), 'bias summary', schemas=SCHEMAS['bias'])
    fams = summ.get('families')
    if not isinstance(fams, dict) or not fams:
        raise ValueError('bias summary: no families')
    order = list(rb.get('families') or ['factor', 'random', 'book'])
    order += [f for f in fams if f not in order]
    rows, absent = [], []
    for f in order:
        x = fams.get(f)
        if not isinstance(x, dict):
            absent.append(f)
            continue
        r = {'family': f, 'status': x.get('status'), 'series': x.get('series'), 'ok': x.get('series_ok'),
             'empty': x.get('series_empty'), 'refused': x.get('series_refused'), 'obs': x.get('observations'),
             'kurt': x.get('pooled_kurtosis'), 'b_full': D.dig(x, 'full_sample.b_mean'),
             'b_full_med': D.dig(x, 'full_sample.b_median'), 'in_band': D.dig(x, 'full_sample.series_in_band'),
             'in_kband': D.dig(x, 'full_sample.series_in_kurtosis_band'),
             'dropped': x.get('dropped_factor_exposures'), 'uncov': x.get('uncovered_name_returns'),
             'warmup': x.get('warmup_excluded'), 'missing': x.get('missing_returns'), 'drop_share': x.get('dropped_share')}
        for w in ('63', '252'):
            band = D.dig(x, f'rolling.{w}.band')
            b = D.dig(x, f'rolling.{w}.b_mean')
            r[f'b{w}'] = b
            r[f'band{w}'] = band
            r[f'share{w}'] = D.dig(x, f'rolling.{w}.share_in_band')
            r[f'kshare{w}'] = D.dig(x, f'rolling.{w}.share_in_kurtosis_band')
            r[f'inb{w}'] = (band[0] <= b <= band[1]) if (C.is_num(b) and isinstance(band, list) and len(band) == 2
                                                        and all(C.is_num(v) for v in band)) else None
        rows.append(r)
    out = {'rows': rows, 'fam': {r['family']: r for r in rows}, 'absent': absent, 'schema': summ.get('schema'),
           'definition': summ.get('definition'), 'summary_src': _src(ctx, _spec(rb['summary'])[0]),
           'dropped_total': sum(r['dropped'] for r in rows if C.is_num(r['dropped'])),
           'refused_total': sum(r['refused'] for r in rows if C.is_num(r['refused'])),
           'label': rb.get('label', 'risk model')}
    if rb.get('manifest'):
        m, err = _try(ctx, rb['manifest'], 'risk manifest', schemas=SCHEMAS['risk_manifest'])
        sf = (m or {}).get('structural_factors') if m else None
        facs = sf.get('factors') if isinstance(sf, dict) else None
        out.update({'manifest_src': _src(ctx, _spec(rb['manifest'])[0]) if m else None, 'manifest_err': err,
                    'structural_factors': len(facs) if isinstance(facs, list) else (0 if m and sf is None else None),
                    'structural_names': [f.get('factor') for f in facs or [] if isinstance(f, dict)],
                    'sessions_with_structural': D.dig(sf, 'sessions_with_structural'),
                    'unforecast_exposure_sessions': D.dig(sf, 'forecast_sessions_with_unforecast_exposure'),
                    'min_corr_scale': D.dig(sf, 'min_correlation_scale'),
                    'factors': D.dig(m, 'geometry.factors'), 'styles': D.dig(m, 'geometry.styles'),
                    'dates': D.dig(m, 'geometry.dates'), 'instruments': D.dig(m, 'geometry.instruments')})
    return out


def blk_risk_bias(ctx, spec) -> str:
    b = _need(ctx, 'bias')
    fam = b['fam']
    kpis = [{'label': 'Structural factors', 'value': C.fmt(b.get('structural_factors'), 'int'),
             'sub': (f"of {C.fmt(b.get('factors'), 'int')}; {C.fmt(b.get('sessions_with_structural'), 'int')} sessions"
                     if C.is_num(b.get('factors')) else 'risk manifest not configured')},
            {'label': 'Dropped exposures', 'value': C.fmt(b['dropped_total'], 'int'),
             'sub': f"refused series {C.fmt(b['refused_total'], 'int')}"}]
    for f in ('factor', 'random', 'book'):
        if f in fam:
            kpis.append({'label': f'b {f}, 252 / 63', 'value': ' / '.join(C.fmt(fam[f][k], '.3f') or C.NA_TEXT
                                                                        for k in ('b252', 'b63')),
                         'sub': f"status {fam[f]['status'] or C.NA_TEXT}; band 1 +- sqrt(2/T)"})
    strip = C.kpi_strip(kpis)
    trows = []
    for r in b['rows']:
        row = dict(r)
        row['st'] = C.badge(STATUS_KIND.get(r['status'], 'n/a') if r['status'] else None, r['status'])
        row['series_txt'] = (f"{C.fmt(r['ok'], 'int') or C.NA_TEXT} ok / {C.fmt(r['empty'], 'int') or C.NA_TEXT} empty"
                             f" / {C.fmt(r['refused'], 'int') or C.NA_TEXT} refused")
        for w in ('63', '252'):
            band = r[f'band{w}']
            row[f'bandtxt{w}'] = (f"band {C.fmt(band[0], '.3f')}-{C.fmt(band[1], '.3f')}"
                                  if isinstance(band, list) and len(band) == 2 else None)
        trows.append(row)
    cols = [{'key': 'family', 'label': 'Family', 'kind': 'mono'}, {'key': 'st', 'label': 'Status', 'kind': 'html'},
            {'key': 'series_txt', 'label': 'Series', 'kind': 'text'}, {'key': 'obs', 'label': 'Observations', 'fmt': 'int'},
            {'key': 'b_full', 'label': 'b full sample, mean', 'fmt': '.3f'},
            {'key': 'b63', 'label': 'b 63-day, mean', 'fmt': _with_sub('.3f', 'bandtxt63')},
            {'key': 'inb63', 'label': 'In band 63', 'kind': 'chip'},
            {'key': 'share63', 'label': 'Windows in band 63', 'fmt': 'pct1'},
            {'key': 'b252', 'label': 'b 252-day, mean', 'fmt': _with_sub('.3f', 'bandtxt252')},
            {'key': 'inb252', 'label': 'In band 252', 'kind': 'chip'},
            {'key': 'share252', 'label': 'Windows in band 252', 'fmt': 'pct1'},
            {'key': 'kshare252', 'label': 'In kurtosis band 252', 'fmt': 'pct1'},
            {'key': 'kurt', 'label': 'Pooled kurtosis', 'fmt': '.2f'},
            {'key': 'dropped', 'label': 'Dropped (unforecast factor)', 'fmt': 'int', 'na_title': 'not recorded (schema v1)'},
            {'key': 'uncov', 'label': 'Uncovered name returns', 'fmt': 'int', 'na_title': 'not recorded (schema v1)'},
            {'key': 'warmup', 'label': 'Warm-up excluded', 'fmt': 'int', 'na_title': 'not recorded (schema v1)'},
            {'key': 'missing', 'label': 'Missing returns', 'fmt': 'int'}]
    absent = (f" Family {', '.join(b['absent'])} not in this run (the book family needs the risk verb with "
              f"--book-weights)." if b['absent'] else '')
    struct = ''
    if b.get('manifest_src'):
        struct = (f" Structural forecasts (factors with < 63 returns, class-prior blend): {C.fmt(b.get('structural_factors'), 'int')}"
                  f" factors ({', '.join(x for x in b['structural_names'][:12] if x)}"
                  f"{' ...' if len(b['structural_names']) > 12 else ''}); sessions with an exposed but unforecast "
                  f"factor {C.fmt(b.get('unforecast_exposure_sessions'), 'int')}; source {b['manifest_src']}.")
    elif b.get('manifest_err'):
        struct = f" Risk manifest not available ({b['manifest_err']})."
    tab = C.table(cols, trows, num=ctx.next_tab(), sortable=False, tid='t-risk-bias',
                  caption=(f"Forecast bias of {b['label']} by test-portfolio family: z = realised return / forecast vol at "
                           f"t-1, b = SD(z) (1 = unbiased; below 1 the model over-forecasts risk), mean over rolling "
                           f"63- and 252-session windows with the band 1 +- sqrt(2/T) (in-band chips computed here), the "
                           f"share of windows inside it and inside the kurtosis-widened band, and the harness's drop "
                           f"counters (an observation with an unforecast exposure is excluded whole).{absent}{struct} "
                           f"Source: {b['summary_src']} ({b['schema']})."))
    return f'<div class="panel">{strip}</div>' + tab


# ============================================================================================ 3. report cards
def _card_row(card: dict, hs, tol) -> dict:
    iby = card.get('ic_by_year') or {}
    adm = card.get('admission') or {}
    wq = card.get('worldquant') or {}
    fit = D.dig(card, 'decay.fit') or {}
    rc = card.get('runner_check') or {}
    ok, have, gaps = True, False, []
    for h in hs:
        x = rc.get(str(h)) or {}
        if C.is_num(x.get('runner_valid_dates')) and x['runner_valid_dates'] > 0:
            have = True
        mm, gap = x.get('valid_date_mismatches'), x.get('abs_mean_diff')
        if C.is_num(mm) and mm > 0:
            ok = False
            gaps.append(f'h{h}: {int(mm)} date mismatches')
        if C.is_num(gap) and gap > tol:
            ok = False
            gaps.append(f'h{h}: |mean gap| {C.fmt(gap, ".1e")}')
        elif C.is_num(gap):
            gaps.append(f'h{h}: |mean gap| {C.fmt(gap, ".1e")}')
    runner = ok if have else (False if not ok else None)
    r = {'id': card.get('id'), 'theme': card.get('theme'), 'tier': card.get('tier'), 'status': adm.get('status'),
         'turnover': D.dig(card, 'turnover.mean'), 'half_life': fit.get('half_life_days'),
         'beyond': fit.get('beyond_window'), 'r2': fit.get('r2'), 'sharpe': wq.get('sharpe'), 'fitness': wq.get('fitness'),
         'margin': wq.get('margin_bps'), 'coverage': D.dig(card, 'coverage.all'),
         'rho_sig': D.dig(card, 'correlation.signal.max_abs.rho'), 'rho_sig_with': D.dig(card, 'correlation.signal.max_abs.with'),
         'rho_pnl': D.dig(card, 'correlation.pnl.max_abs.rho'), 'rho_pnl_with': D.dig(card, 'correlation.pnl.max_abs.with'),
         'runner': runner, 'runner_note': '; '.join(gaps) or ('no runner IC at any horizon' if not have else ''),
         'payload_ok': adm.get('payload_matches_admission'), 'hac_t': adm.get('hac_t'),
         'failed': adm.get('failed_checks') or []}
    for h in hs:
        r[f'ic{h}'] = D.dig(iby, f'{h}.all.mean')
        r[f'ir{h}'] = D.dig(iby, f'{h}.all.ir')
    return r


def _card_set(ctx, s: dict, hs, tol) -> dict:
    d = str(s.get('dir', '')).rstrip('/')
    idx = _load(ctx, s.get('index') or f'{d}/index.json', f"card index {s.get('key', d)}", schemas=SCHEMAS['card_index'])
    cands = idx.get('candidates')
    if not isinstance(cands, list):
        raise ValueError(f'card index {d}/index.json: no candidates[]')
    man, _ = _try(ctx, f'{d}/manifest.json', 'card manifest')
    files = (man or {}).get('files') if isinstance(man, dict) else None
    rows, cards, missing, mism = [], {}, [], []
    for c in sorted((c for c in cands if isinstance(c, dict) and c.get('id')),
                    key=lambda c: (c.get('rank') if C.is_num(c.get('rank')) else 1e9, c['id'])):
        rel = f"{d}/card-{c['id']}.json"
        card, err = _try(ctx, rel, f"card {c['id']}", schemas=SCHEMAS['card'])
        if card is None:
            missing.append(c['id'])
            r = {'id': c['id'], 'theme': c.get('theme'), 'status': c.get('status'), 'runner': None,
                 'runner_note': err, 'fitness': c.get('fitness'), 'sharpe': c.get('sharpe'), 'turnover': c.get('turnover')}
        else:
            r = _card_row(card, hs, tol)
            cards[c['id']] = card
        r['rank'] = c.get('rank')
        r['manifest_ok'] = (files.get(f"card-{c['id']}.json") == _sha(ctx, rel)) if isinstance(files, dict) and card else None
        if r['manifest_ok'] is False:
            mism.append(c['id'])
        rows.append(r)
    runner = [r['runner'] for r in rows]
    return {'key': s.get('key', d), 'label': s.get('label', d), 'dir': d, 'rows': rows, 'cards': cards,
            'missing': missing, 'manifest_mismatch': mism, 'n': len(rows),
            'n_admitted': sum(1 for r in rows if r.get('status') == 'admitted'),
            'runner_pass': runner.count(True), 'runner_fail': runner.count(False), 'runner_na': runner.count(None),
            'index_src': _src(ctx, _spec(s.get('index') or f'{d}/index.json')[0]), 'manifest_src': _src(ctx, f'{d}/manifest.json') if man else None,
            'ranking': idx.get('ranking'), 'window': idx.get('window'), 'href': s.get('href')}


def an_cards(ctx) -> dict:
    rc = _conf(ctx, 'report_cards')
    hs = [int(h) for h in rc.get('horizons', [5, 21, 63])]
    tol = rc.get('runner_tolerance', 5e-4)
    sets, errors = [], {}
    for s in rc.get('sets') or []:
        try:
            sets.append(_card_set(ctx, s, hs, tol))
        except (OSError, ValueError) as e:
            errors[s.get('key', s.get('dir'))] = f'{type(e).__name__}: {e}'
            sets.append({'key': s.get('key', s.get('dir')), 'label': s.get('label', s.get('dir')), 'error': errors[s.get('key', s.get('dir'))]})
    if not any('rows' in s for s in sets):
        raise FileNotFoundError('no report-card set readable: ' + '; '.join(f'{k}: {v}' for k, v in errors.items()))
    first = next(s for s in sets if 'rows' in s)
    return {'sets': sets, 'horizons': hs, 'tolerance': tol, 'errors': errors, 'n': first['n'],
            'n_admitted': first['n_admitted'], 'runner_pass': first['runner_pass'], 'runner_fail': first['runner_fail'],
            'runner_na': first['runner_na']}


def _card_detail(card: dict, hs) -> str:
    iby = card.get('ic_by_year') or {}
    years = sorted({y for h in hs for y in (iby.get(str(h)) or {}) if y != 'all'}) + ['all']
    rows = []
    for h in hs:
        rows.append({'m': f'Rank IC h{h}, mean', **{f'y{y}': D.dig(iby, f'{h}.{y}.mean') for y in years}, '_f': '+.4f'})
    rows.append({'m': 'Rank IC h21, IR (mean / sd daily)', **{f'y{y}': D.dig(iby, f'21.{y}.ir') for y in years}, '_f': '+.3f'})
    rows.append({'m': 'Book Sharpe (gross, unneutralised)', **{f'y{y}': D.dig(card, f'worldquant.sharpe_by_year.{y}') for y in years},
                 '_f': '+.3f'})
    rows.append({'m': 'Turnover, mean daily', **{f'y{y}': D.dig(card, f'turnover.by_year.{y}.mean') for y in years}, '_f': '.4f'})
    rows.append({'m': 'Coverage', **{f'y{y}': D.dig(card, f'coverage.{y}') for y in years}, '_f': 'pct1'})

    def fm(v, row):
        s = C.fmt(v, row['_f'])
        return None if s is None else C.esc(s)
    cols = [{'key': 'm', 'label': 'Metric', 'kind': 'text'}] + [{'key': f'y{y}', 'label': str(y), 'fmt': fm} for y in years]
    t = C.table(cols, rows, sortable=False)
    size = D.dig(card, 'ic_by_size_tercile.groups') or {}
    fit = D.dig(card, 'decay.fit') or {}
    adm = card.get('admission') or {}
    facts = [f"size terciles IC h{D.dig(card, 'ic_by_size_tercile.horizon') or 21}: "
             + ', '.join(f"{k} {C.fmt(D.dig(size, f'{k}.mean'), '+.4f') or C.NA_TEXT}" for k in ('small', 'mid', 'large')),
             f"decay: half-life {C.fmt(fit.get('half_life_days'), '.1f') or C.NA_TEXT} d"
             + (' (beyond the 63-session window)' if fit.get('beyond_window') else '')
             + f", r2 {C.fmt(fit.get('r2'), '.2f') or C.NA_TEXT}",
             f"admission: {adm.get('status') or C.NA_TEXT}, HAC t {C.fmt(adm.get('hac_t'), '+.2f') or C.NA_TEXT}, "
             f"tau {C.fmt(adm.get('tau'), '.4f') or C.NA_TEXT}, payload = admission's: "
             f"{ {True: 'yes', False: 'NO'}.get(adm.get('payload_matches_admission'), C.NA_TEXT)}"
             + (f", failed checks {', '.join(map(str, adm.get('failed_checks')))}" if adm.get('failed_checks') else ''),
             f"max |rho| signal {C.fmt(D.dig(card, 'correlation.signal.max_abs.rho'), '+.2f') or C.NA_TEXT} "
             f"({D.dig(card, 'correlation.signal.max_abs.with') or C.NA_TEXT}), pnl "
             f"{C.fmt(D.dig(card, 'correlation.pnl.max_abs.rho'), '+.2f') or C.NA_TEXT} "
             f"({D.dig(card, 'correlation.pnl.max_abs.with') or C.NA_TEXT})"]
    return t + ''.join(_note(f) for f in facts)


def blk_report_cards(ctx, spec) -> str:
    rc = _need(ctx, 'cards')
    hs = rc['horizons']
    out = []
    for s in rc['sets']:
        if 'error' in s:
            out.append(f'<div class="unavailable" data-block="t_report_cards">report cards {C.esc(s["label"])}: '
                       f'not available ({C.esc(s["error"][:240])})</div>')
            continue
        rows = []
        for r in s['rows']:
            row = dict(r)
            row['id_html'] = f'<code>{C.esc(r["id"])}</code>'
            row['id_html__sort'] = r['id']
            st = r.get('status')
            row['st'] = C.badge(None if not isinstance(st, str) else ('accepted' if st == 'admitted' else 'rejected'),
                                st.replace('reject_', '') if isinstance(st, str) else None)
            row['st__sort'] = r.get('status')
            hl = r.get('half_life')
            row['hl_txt'] = ('fit grid cap' if C.is_num(hl) and hl >= GRID_CAP_DAYS
                             else ('beyond the 63-day window' if r.get('beyond') else None))
            row['rho_txt'] = r.get('rho_sig_with')
            row['runner__title'] = r.get('runner_note')
            rows.append(row)
        cols = [{'key': 'rank', 'label': 'Fitness rank', 'fmt': 'int'},
                {'key': 'id_html', 'label': 'Member', 'kind': 'html'}, {'key': 'theme', 'label': 'Theme', 'kind': 'text'},
                {'key': 'tier', 'label': 'Tier', 'kind': 'text'}, {'key': 'st', 'label': 'Admission', 'kind': 'html'}]
        cols += [{'key': f'ic{h}', 'label': f'Rank IC h{h}', 'fmt': '+.4f', 'na_title': 'no runner IC for this member'}
                 for h in hs]
        cols += [{'key': 'ir21', 'label': 'IR h21', 'fmt': '+.3f'},
                 {'key': 'turnover', 'label': 'Turnover/day', 'fmt': '.4f'},
                 {'key': 'half_life', 'label': 'Decay half-life, d', 'fmt': _with_sub('.1f', 'hl_txt')},
                 {'key': 'sharpe', 'label': 'Book SR (gross)', 'fmt': '+.3f'}, {'key': 'fitness', 'label': 'Fitness', 'fmt': '.3f'},
                 {'key': 'margin', 'label': 'Margin bps', 'fmt': '.1f'}, {'key': 'coverage', 'label': 'Coverage', 'fmt': 'pct1'},
                 {'key': 'rho_sig', 'label': 'Max |rho| signal', 'fmt': _with_sub('+.2f', 'rho_txt')},
                 {'key': 'runner', 'label': 'Runner check', 'kind': 'chip', 'na_title': 'no runner IC to check against'},
                 {'key': 'manifest_ok', 'label': 'Card = manifest', 'kind': 'chip', 'na_title': 'no card manifest'}]
        notes = []
        if s['missing']:
            notes.append(f"cards missing or unreadable: {', '.join(s['missing'])}")
        if s['manifest_mismatch']:
            notes.append(f"card file differs from the card manifest: {', '.join(s['manifest_mismatch'])}")
        cap = (f"Alpha report cards, {s['label']}: {s['n']} members ({s['n_admitted']} admitted), ranked by "
               f"{s.get('ranking') or 'the index order'}. Rank IC = the IC runner's own daily rank IC per horizon (DSL "
               f"orientation) over the card window; turnover and book Sharpe of the member's own dollar-neutral rank book "
               f"(gross of costs, not neutralised); decay half-life from the lagged one-day IC h = 1..63; runner check = the "
               f"card's IC vs the runner's train_daily_ic.csv at every horizon: 0 valid-date mismatches and |mean gap| "
               f"<= {C.fmt(rc['tolerance'], 'g')} (PASS {s['runner_pass']}, FAIL {s['runner_fail']}, n/a {s['runner_na']}: "
               f"no runner IC). Sources: {s['index_src']}, {s['dir']}/card-<id>.json"
               + (f", {s['manifest_src']}" if s.get('manifest_src') else '') + '.')
        out.append(C.table(cols, rows, num=ctx.next_tab(), caption=cap, tid=f"t-cards-{s['key']}"))
        out.append(''.join(_note(n) for n in notes))
        det = []
        for r in s['rows']:
            card = s['cards'].get(r['id'])
            if card is None:
                continue
            link = (f' <a href="{C.esc(s["href"].format(id=r["id"]))}">card page</a>' if s.get('href') else '')
            det.append(f'<details class="card"><summary><code>{C.esc(r["id"])}</code>{C.esc(r.get("theme") or "")} '
                       f'&middot; tier {C.esc(r.get("tier") or C.NA_TEXT)} &middot; {C.esc(r.get("status") or C.NA_TEXT)}'
                       f'{link}</summary>{_card_detail(card, hs)}</details>')
        if det:
            out.append(f'<details class="more"><summary>Card detail by member ({len(det)}): IC by year and horizon, '
                       f'book Sharpe, turnover, coverage, size terciles, decay, admission, correlations</summary>'
                       f'{"".join(det)}</details>')
    return ''.join(out)


# ============================================================================================ 4. monitor baseline
def an_monitor_base(ctx) -> dict:
    mc = _conf(ctx, 'monitor_baseline')
    m = _load(ctx, mc.get('path'), 'monitor', schemas=SCHEMAS['monitor'])
    ch = m.get('checks')
    if not isinstance(ch, dict):
        raise ValueError('monitor: no checks')
    counts = {'ok': 0, 'warn': 0, 'alarm': 0, 'n/a': 0}
    for k, v in ch.items():
        st = (v or {}).get('status') if isinstance(v, dict) else None
        counts[st if st in counts else 'n/a'] += 1
    members = D.dig(ch, 'M2.members') or {}
    flagged, alarming = [], []
    for name, x in members.items():
        if not isinstance(x, dict) or x.get('status') in (None, 'ok'):
            continue
        subs = {k: v for k, v in x.items() if isinstance(v, dict) and 'status' in v}
        bad = [k for k, v in subs.items() if v.get('status') in ('warn', 'alarm')]
        row = {'member': name, 'status': x.get('status'), 'flags': bad}
        for k, v in subs.items():
            row[k] = v.get('value')
            row[f'{k}_st'] = v.get('status')
            row[f'{k}_band'] = (v.get('p5'), v.get('p95'))
        flagged.append(row)
        if x.get('status') == 'alarm':
            what = [f"{k} {C.fmt(subs[k].get('value'), '.2f')}" for k in bad if subs[k].get('status') == 'alarm']
            alarming.append(f"{name} ({', '.join(what)})")
    flagged.sort(key=lambda r: (0 if r['status'] == 'alarm' else 1, r['member']))
    return {'status': m.get('status'), 'mode': m.get('mode'), 'in_sample': m.get('in_sample'), 'action': m.get('action'),
            'checks': ch, 'check_counts': counts, 'sleeve_counts': D.dig(ch, 'M2.counts') or {}, 'flagged': flagged,
            'alarming': alarming, 'alarming_names': [r['member'] for r in flagged if r['status'] == 'alarm'],
            'n_alarming': sum(1 for r in flagged if r['status'] == 'alarm'),
            'src': _src(ctx, _spec(mc['path'])[0]), 'cusum': D.dig(ch, 'M2.cusum') or {}}


def _mon_rows(mb) -> list[dict]:
    ch = mb['checks']
    rows = []
    m1 = ch.get('M1') or {}
    band = m1.get('band') or [None, None]
    rows.append({'check': 'M1 book forecast bias, b = SD(z) over the last window', 'status': m1.get('status'),
                 'value': C.fmt(m1.get('value'), '.3f'), 'thr': (f"band {C.fmt(band[0], '.3f')}-{C.fmt(band[1], '.3f')}, "
                                                                 f"window {m1.get('window')}"),
                 'detail': m1.get('reason') or f"realised NAV vol {C.fmt(m1.get('realized_nav_vol_annual'), 'pct2')}"})
    m2 = ch.get('M2') or {}
    cnt = m2.get('counts') or {}
    cs = m2.get('cusum') or {}
    rows.append({'check': f"M2 sleeve drift: IC h{m2.get('ic_horizon')} 63/252 and turnover 63 vs TRAIN p5-p95; CUSUM",
                 'status': m2.get('status'),
                 'value': ' / '.join(f"{k} {cnt.get(k, 0)}" for k in ('alarm', 'warn', 'ok', 'n/a')),
                 'thr': (f"CUSUM h {C.fmt(cs.get('h'), 'g')} sigma, k {C.fmt(cs.get('k'), 'g')} sigma, warn at "
                         f"{C.fmt(cs.get('warn'), 'g')}; ARL0 {C.fmt(cs.get('arl0_years'), '.1f')} y"),
                 'detail': ('alarm: ' + '; '.join(mb['alarming'])) if mb['alarming'] else 'no sleeve in alarm'})
    m3 = ch.get('M3') or {}
    cr, fr = m3.get('cost_ratio') or {}, m3.get('fill_rate') or {}
    rb = m3.get('cost_ratio_band') or [None, None]
    rows.append({'check': f"M3 execution: cost per $ vs the model over {m3.get('window')} sessions; fill rate",
                 'status': m3.get('status'),
                 'value': f"ratio {C.fmt(cr.get('value'), '.3f')}; fill {C.fmt(fr.get('value'), 'pct2')}",
                 'thr': (f"ratio in [{C.fmt(rb[0], 'g')}, {C.fmt(rb[1], 'g')}], TRAIN p5-p95 {C.fmt(cr.get('p5'), '.2f')}-"
                         f"{C.fmt(cr.get('p95'), '.2f')}; fill >= p5 {C.fmt(fr.get('p5'), 'pct1')}"),
                 'detail': (f"cost {C.fmt(cr.get('cost_per_dollar_bps'), '.2f')} vs model "
                            f"{C.fmt(cr.get('model_cost_per_dollar_bps'), '.2f')} bps/$; opportunity cost "
                            f"{str(m3.get('opportunity_cost') or C.NA_TEXT)[:60]}")})
    m4 = ch.get('M4') or {}
    rows.append({'check': f"M4 crowding: mean pairwise corr of sleeve returns over {m4.get('window')} sessions",
                 'status': m4.get('status'), 'value': C.fmt(m4.get('mean_pairwise_corr'), '.3f'),
                 'thr': f"warn > p95 {C.fmt(m4.get('p95'), '.3f')}, alarm > p99 {C.fmt(m4.get('p99'), '.3f')}",
                 'detail': f"stock-level comomentum: {str(m4.get('comomentum_stock_level') or C.NA_TEXT)[:70]}"})
    for k, v in ch.items():
        if k in ('M1', 'M2', 'M3', 'M4') or not isinstance(v, dict):
            continue
        rows.append({'check': f'{k}', 'status': v.get('status'), 'value': None, 'thr': None,
                     'detail': v.get('reason') or ''})
    return rows


def blk_monitor_baseline(ctx, spec) -> str:
    mb = _need(ctx, 'monitor_base')
    cc, sc = mb['check_counts'], mb['sleeve_counts']
    kpis = [{'label': 'Monitor status', 'value': str(mb['status'] or C.NA_TEXT).upper(),
             'sub': f"mode {mb['mode']}{' (in sample)' if mb['in_sample'] else ''}"},
            {'label': 'Checks alarm / warn / ok / n/a', 'value': ' / '.join(str(cc.get(k, 0)) for k in ('alarm', 'warn', 'ok', 'n/a')),
             'sub': 'M1-M4 and the decision check'},
            {'label': 'Sleeves alarm / warn / ok', 'value': ' / '.join(str(sc.get(k, 0)) for k in ('alarm', 'warn', 'ok')),
             'sub': f"n/a {sc.get('n/a', 0)}"},
            {'label': 'Alarming sleeve', 'value': ', '.join(mb['alarming_names']) or 'none',
             'sub': (mb['alarming'][0].split('(', 1)[-1].rstrip(')') if mb['alarming'] else 'no alarm')}]
    rows = []
    for r in _mon_rows(mb):
        rows.append({**r, 'st': C.badge(STATUS_KIND.get(r['status'], 'n/a') if r['status'] else None, r['status'])})
    cols = [{'key': 'check', 'label': 'Check', 'kind': 'text', 'cls': 'wrap'}, {'key': 'st', 'label': 'Status', 'kind': 'html'},
            {'key': 'value', 'label': 'Value', 'kind': 'text'}, {'key': 'thr', 'label': 'Threshold / band', 'kind': 'text', 'cls': 'wrap'},
            {'key': 'detail', 'label': 'Detail', 'kind': 'text', 'cls': 'wrap'}]
    t1 = C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-monitor-baseline',
                 caption=(f"Book monitor baseline (flags only; action: {mb['action'] or C.NA_TEXT}): each check's status, "
                          f"value and threshold. Replayed over TRAIN, so the thresholds are the TRAIN distribution the "
                          f"baseline itself sets; the first live read is the real test. Source: {mb['src']}."))
    subs = [('ic_cusum', 'IC CUSUM', '.2f'), ('turnover_cusum', 'Turnover CUSUM', '.2f'), ('ic63', 'IC 63', '+.4f'),
            ('ic252', 'IC 252', '+.4f'), ('turnover63', 'Turnover 63', '.4f')]
    frows = []
    for f in mb['flagged']:
        row = {'member': f'<code>{C.esc(f["member"])}</code>', 'member__sort': f['member'],
               'st': C.badge(STATUS_KIND.get(f['status'], 'n/a'), f['status']), 'st__sort': f['status']}
        for k, _, spec_ in subs:
            v, st = f.get(k), f.get(f'{k}_st')
            s = C.fmt(v, spec_)
            if s is None:
                row[k] = None
                continue
            b = f.get(f'{k}_band') or (None, None)
            band = (f' <span class="sub" style="display:inline">(p5-p95 {C.esc(C.fmt(b[0], spec_))} to '
                    f'{C.esc(C.fmt(b[1], spec_))})</span>') if C.is_num(b[0]) and C.is_num(b[1]) else ''
            row[k] = C.esc(s) + band + (' ' + C.badge(STATUS_KIND.get(st, 'n/a'), st) if st and st != 'ok' else '')
            row[f'{k}__sort'] = v
        frows.append(row)
    fcols = [{'key': 'member', 'label': 'Sleeve', 'kind': 'html'}, {'key': 'st', 'label': 'Status', 'kind': 'html'}]
    fcols += [{'key': k, 'label': lab, 'kind': 'html'} for k, lab, _ in subs]
    t2 = C.table(fcols, frows, num=ctx.next_tab(), sortable=False, tid='t-monitor-sleeves',
                 caption=(f"The {len(frows)} sleeves not at ok (alarm first): last-window IC and turnover against their "
                          f"TRAIN p5-p95, CUSUM statistics (in sigma units), flagged sub-checks marked. Source: {mb['src']} "
                          f"checks.M2.members.")) if frows else _note('Every sleeve is ok.')
    return f'<div class="panel">{C.kpi_strip(kpis)}</div>' + t1 + t2


# ============================================================================================ 5. operating loop
def an_ops(ctx) -> dict:
    oc = _conf(ctx, 'ops_loop')
    decs, notes = [], []
    for it in oc.get('decisions') or []:
        d = str(it.get('dir', '')).rstrip('/')
        dj, err = _try(ctx, f'{d}/decision.json', f"decision {it.get('asof')}", schemas=SCHEMAS['decision'])
        row = {'asof': it.get('asof') or (dj or {}).get('asof'), 'dir': d}
        if dj is None:
            notes.append(f"decision {row['asof']}: {err}")
            decs.append(row)
            continue
        os_ = dj.get('orders_shares') or {}
        health = dj.get('health') or {}
        row.update({'status': dj.get('status'), 'mismatches': D.dig(dj, 'replay_parity.mismatches'),
                    'names': D.dig(dj, 'replay_parity.names'), 'health': health.get('status'),
                    'health_warn': [c.get('check') for c in health.get('checks') or [] if isinstance(c, dict)
                                    and c.get('status') not in (None, 'ok')],
                    'members': D.dig(dj, 'decision.members'), 'gross': D.dig(dj, 'decision.planned_gross'),
                    'turnover': D.dig(dj, 'decision.planned_turnover'), 'held': D.dig(dj, 'decision.planned_held_names'),
                    'tc': D.dig(dj, 'transfer_coefficient.desired_over_sigma_target'),
                    'orders': D.dig(dj, 'orders.count'), 'share_orders': os_.get('orders'), 'buys': os_.get('buys'),
                    'sells': os_.get('sells'), 'short_sales': os_.get('short_sales'), 'exits': os_.get('exits'),
                    'refused': os_.get('refused_no_close'), 'dropped_min': D.dig(os_, 'dropped_min_notional.count'),
                    'rounded0': D.dig(os_, 'rounded_to_zero.count'), 'residual_abs': D.dig(os_, 'residual_notional.abs'),
                    'above_cap': D.dig(os_, 'participation.above_cap'), 'lot': os_.get('lot_size'),
                    'min_notional': os_.get('min_notional'), 'nav_dollars': os_.get('nav_dollars'),
                    'buy_notional': os_.get('buy_notional'), 'sell_notional': os_.get('sell_notional'),
                    'src': _src(ctx, f'{d}/decision.json')})
        if it.get('run'):
            rc, rerr = _try(ctx, f"{str(it['run']).rstrip('/')}/receipt.json", 'decide receipt', schemas=SCHEMAS['receipt'])
            if rc is None:
                notes.append(f"decide receipt {row['asof']}: {rerr}")
            else:
                row.update({'wall': rc.get('wall_seconds'), 'rss_mib': _div(rc.get('sampled_peak_tree_rss_bytes'), 2 ** 20),
                            'outcome': rc.get('outcome'), 'exit': rc.get('exit_code')})
        decs.append(row)
    recs = []
    for it in oc.get('reconcile') or []:
        d = str(it.get('dir', '')).rstrip('/')
        rj, err = _try(ctx, f'{d}/reconcile.json', f"reconcile {it.get('label')}", schemas=SCHEMAS['reconcile'])
        row = {'label': it.get('label', d), 'dir': d, 'expect': it.get('expect') or {}}
        if rj is None:
            notes.append(f"reconcile {row['label']}: {err}")
            row['caught'] = None
            recs.append(row)
            continue
        cnt = rj.get('counts') or {}
        row.update({k: cnt.get(k) for k in ('compared', 'ok', 'explained', 'missing', 'extra', 'quantity', 'unmapped',
                                             'unexplained_breaks')})
        row.update({'status': rj.get('status'), 'asof': rj.get('broker_asof'), 'decision_asof': rj.get('decision_asof'),
                    'ca': D.dig(rj, 'corporate_actions.applied'), 'broker': D.dig(rj, 'inputs.broker.path'),
                    'tol': D.dig(rj, 'tolerance.shares'), 'src': _src(ctx, f'{d}/reconcile.json')})
        exp = row['expect']
        row['caught'] = all(cnt.get(k) == v for k, v in exp.items()) if exp else None
        row['expect_txt'] = ', '.join(f'{k} {v}' for k, v in exp.items()) or None
        recs.append(row)
    out = {'decisions': decs, 'reconcile': recs, 'notes': notes}
    nr = oc.get('nav_runs') or {}
    if nr.get('off') and nr.get('on'):
        off, e1 = _try(ctx, f"{str(nr['off']).rstrip('/')}/receipt.json", 'nav receipt (off)', schemas=SCHEMAS['receipt'])
        on, e2 = _try(ctx, f"{str(nr['on']).rstrip('/')}/receipt.json", 'nav receipt (on)', schemas=SCHEMAS['receipt'])
        if off is None or on is None:
            notes.append(f'holdings-emission overhead: {e1 or e2}')
        out.update({'wall_off': (off or {}).get('wall_seconds'), 'wall_on': (on or {}).get('wall_seconds'),
                    'max_ratio': nr.get('max_ratio'),
                    'nav_src': ', '.join(_src(ctx, f"{str(nr[k]).rstrip('/')}/receipt.json") for k in ('off', 'on'))})
        out['wall_ratio'] = _div(out['wall_on'], out['wall_off'])
    if oc.get('holdings'):
        hm, err = _try(ctx, oc['holdings'], 'holdings manifest', schemas=SCHEMAS['holdings'])
        if hm is None:
            notes.append(f'holdings manifest: {err}')
        out.update({'holdings_rows': (hm or {}).get('rows'), 'holdings_sessions': (hm or {}).get('sessions'),
                    'holdings_format': D.dig(hm, 'format.id'), 'holdings_src': _src(ctx, _spec(oc['holdings'])[0]) if hm else None})
    ok = [r for r in decs if 'status' in r]
    walls = [r['wall'] for r in decs if C.is_num(r.get('wall'))]
    out.update({'n_decisions': len(decs), 'n_decisions_read': len(ok),
                'parity_total': sum(r['mismatches'] for r in ok if C.is_num(r.get('mismatches'))) if ok else None,
                'wall_max': max(walls) if walls else None, 'wall_min': min(walls) if walls else None,
                'self_breaks': next((r.get('unexplained_breaks') for r in recs if r.get('expect', {}).get('unexplained_breaks') == 0), None),
                'expect_met': sum(1 for r in recs if r.get('caught') is True),
                'expect_n': sum(1 for r in recs if r.get('expect'))})
    if not ok and not any('compared' in r for r in recs):
        raise FileNotFoundError('no decision.json or reconcile.json readable: ' + '; '.join(notes))
    return out


def blk_ops_loop(ctx, spec) -> str:
    op = _need(ctx, 'ops')
    oc = ctx.cfg.get('ops_loop') or {}
    parts = []
    stages = [{'title': _txt(ctx, s.get('title')), 'lines': [_txt(ctx, x) for x in s.get('lines', [])], 'kind': s.get('kind')}
              for s in oc.get('flow') or []]
    if stages:
        svg = C.flow_diagram(stages, per_row=int(oc.get('flow_per_row', len(stages) if len(stages) <= 4 else 3)),
                             aria='daily operating loop')
        parts.append(C.figure(ctx.next_fig(), svg, _txt(ctx, oc.get('flow_caption', 'The daily operating loop.')),
                              'fig-ops-loop'))
    gates = []
    ok_decs = [r for r in op['decisions'] if 'status' in r]
    if ok_decs:
        gates.append({'name': f"Decide replay parity (bitwise target weights), {len(ok_decs)} as-of dates",
                      'value': f"{C.fmt(op['parity_total'], 'int')} mismatches", 'threshold': '0',
                      'passed': op['parity_total'] == 0 if C.is_num(op['parity_total']) else None})
    for r in op['reconcile']:
        if r.get('expect'):
            keys = list(r['expect'])
            gates.append({'name': f"Reconcile, {r['label']}: {' / '.join(k.replace('_', ' ') for k in keys)}",
                          'value': ' / '.join(str(r.get(k)) for k in keys),
                          'threshold': ' / '.join(str(r['expect'][k]) for k in keys), 'passed': r.get('caught')})
    if C.is_num(op.get('wall_ratio')):
        mr = op.get('max_ratio')
        gates.append({'name': 'Holdings emission overhead (NAV wall, on / off)',
                      'value': f"{C.fmt(op['wall_on'], '.1f')} / {C.fmt(op['wall_off'], '.1f')} s = {C.fmt(op['wall_ratio'], '.2f')}x",
                      'threshold': f"<= {C.fmt(mr, 'g')}x" if C.is_num(mr) else '', 'passed': op['wall_ratio'] <= mr if C.is_num(mr) else None})
    if gates:
        parts.append(C.gate_panel(gates, headers=('Operating-loop check', 'Value', 'Expected', 'Result'),
                                  title='Operating loop acceptance (TRAIN as-of dates; expected values from the lane briefs)'))
    rows = []
    for r in op['decisions']:
        row = dict(r)
        row['st'] = C.badge(STATUS_KIND.get(r.get('status'), 'n/a') if r.get('status') else None, r.get('status'))
        row['par'] = (r.get('mismatches') == 0) if C.is_num(r.get('mismatches')) else None
        row['par__title'] = f"{r.get('mismatches')} mismatches over {r.get('names')} names"
        row['hl'] = C.badge(STATUS_KIND.get(r.get('health'), 'n/a') if r.get('health') else None, r.get('health'))
        row['hl_sub'] = ', '.join(r.get('health_warn') or [])
        if row['hl_sub']:
            row['hl'] += f'<span class="sub">{C.esc(row["hl_sub"])}</span>'
        row['bs'] = f"{C.fmt(r.get('buys'), 'int') or C.NA_TEXT} / {C.fmt(r.get('sells'), 'int') or C.NA_TEXT}" if 'buys' in r else None
        row['res'] = r.get('residual_abs')
        rows.append(row)
    cols = [{'key': 'asof', 'label': 'As-of', 'kind': 'mono'}, {'key': 'st', 'label': 'Status', 'kind': 'html'},
            {'key': 'par', 'label': 'Replay parity', 'kind': 'chip'}, {'key': 'wall', 'label': 'Wall s', 'fmt': '.1f'},
            {'key': 'rss_mib', 'label': 'Peak RSS MiB', 'fmt': 'int'}, {'key': 'hl', 'label': 'Health', 'kind': 'html'},
            {'key': 'members', 'label': 'Members', 'fmt': 'int'}, {'key': 'held', 'label': 'Planned names', 'fmt': 'int'},
            {'key': 'gross', 'label': 'Planned gross', 'fmt': '.3f'}, {'key': 'turnover', 'label': 'Planned turnover', 'fmt': 'pct2'},
            {'key': 'tc', 'label': 'TC (target)', 'fmt': '.3f'}, {'key': 'orders', 'label': 'Weight orders', 'fmt': 'int'},
            {'key': 'share_orders', 'label': 'Share orders', 'fmt': 'int'}, {'key': 'bs', 'label': 'Buys / sells', 'kind': 'text'},
            {'key': 'short_sales', 'label': 'Short sales', 'fmt': 'int'}, {'key': 'exits', 'label': 'Exits', 'fmt': 'int'},
            {'key': 'refused', 'label': 'Refused (no close)', 'fmt': 'int'},
            {'key': 'dropped_min', 'label': 'Dropped < min notional', 'fmt': 'int'},
            {'key': 'rounded0', 'label': 'Rounded to 0 lots', 'fmt': 'int'},
            {'key': 'res', 'label': 'Rounding residual |$|', 'fmt': 'int'},
            {'key': 'above_cap', 'label': 'Orders above ADV cap', 'fmt': 'int'}]
    first = next((r for r in op['decisions'] if 'lot' in r), {})
    srcs = '; '.join(r['src'] for r in op['decisions'] if r.get('src'))
    parts.append(C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-ops-decide',
                         caption=(f"decide at each configured TRAIN as-of date from the replay's own holdings: status, bitwise "
                                  f"target parity with the replay, wall time and peak memory (run receipt), health checks "
                                  f"(warnings named), the planned book and the share orders (lot {C.fmt(first.get('lot'), 'g')}, "
                                  f"minimum notional ${C.fmt(first.get('min_notional'), 'int')}, sized to "
                                  f"${C.fmt(_mn(first.get('nav_dollars'), 1e9), 'g')}bn; exits are kept below the minimum). "
                                  f"Sources: {srcs or C.NA_TEXT}; the run dirs' receipt.json.")))
    rrows = []
    for r in op['reconcile']:
        rrows.append({**r, 'caught_chip': r.get('caught'), 'st': C.badge(STATUS_KIND.get(r.get('status'), 'n/a')
                                                                          if r.get('status') else None, r.get('status'))})
    rcols = [{'key': 'label', 'label': 'Broker file', 'kind': 'text'}, {'key': 'st', 'label': 'Status', 'kind': 'html'},
             {'key': 'decision_asof', 'label': 'Decision', 'kind': 'mono'}, {'key': 'asof', 'label': 'Broker as-of', 'kind': 'mono'},
             {'key': 'compared', 'label': 'Compared', 'fmt': 'int'}, {'key': 'ok', 'label': 'OK', 'fmt': 'int'},
             {'key': 'explained', 'label': 'Explained (corp. action)', 'fmt': 'int'},
             {'key': 'missing', 'label': 'Missing', 'fmt': 'int'}, {'key': 'extra', 'label': 'Extra', 'fmt': 'int'},
             {'key': 'quantity', 'label': 'Quantity', 'fmt': 'int'}, {'key': 'unmapped', 'label': 'Unmapped', 'fmt': 'int'},
             {'key': 'unexplained_breaks', 'label': 'Unexplained breaks', 'fmt': 'int'},
             {'key': 'expect_txt', 'label': 'Expected', 'kind': 'text', 'na_title': 'no expectation configured'},
             {'key': 'caught_chip', 'label': 'As expected', 'kind': 'chip'}]
    rsrc = '; '.join(r['src'] for r in op['reconcile'] if r.get('src'))
    parts.append(C.table(rcols, rrows, num=ctx.next_tab(), sortable=False, tid='t-ops-reconcile',
                         caption=(f"reconcile of the decision's expected holdings against a broker position file (tolerance "
                                  f"{C.fmt(next((r.get('tol') for r in op['reconcile'] if C.is_num(r.get('tol'))), None), 'g')} "
                                  f"shares, corporate actions applied between the decision and the broker as-of), per "
                                  f"configured broker file with the counts expected of it (config ops_loop.reconcile[].expect: "
                                  f"a self run feeds the expected holdings back and must show no break; a planted run must show "
                                  f"exactly its planted breaks, a split explained by its corporate-action row). Sources: "
                                  f"{rsrc or C.NA_TEXT}.")))
    extra = []
    if op.get('holdings_src'):
        extra.append(f"Holdings written by the replay: {C.fmt(op.get('holdings_rows'), 'int')} rows over "
                     f"{C.fmt(op.get('holdings_sessions'), 'int')} sessions, format {op.get('holdings_format')} "
                     f"({op['holdings_src']}).")
    if op.get('nav_src'):
        extra.append(f"NAV wall with / without holdings emission from {op['nav_src']}.")
    parts.append(''.join(_note(x) for x in extra + op['notes']))
    return ''.join(parts)


# ============================================================================================ 6. integrity
def _psr_at(row: dict, sr_star: float) -> dict:
    for b in D.dig(row, 'psr.benchmarks') or []:
        if isinstance(b, dict) and C.is_num(b.get('sr_star_annual')) and abs(b['sr_star_annual'] - sr_star) < 1e-12:
            return b
    return {}


def _gate_check(row: dict, ch: dict) -> dict:
    v = D.dig(row, ch['metric'])
    op = ch.get('op')
    f = ch.get('fmt', '.3f')
    tf = ch.get('threshold_fmt', f.lstrip('+'))
    if op == 'between':
        thr, ok = f"[{C.fmt(ch['lo'], tf)}, {C.fmt(ch['hi'], tf)}]", (lambda x: ch['lo'] <= x <= ch['hi'])
    elif op == 'ge':
        thr, ok = f">= {C.fmt(ch['value'], tf)}", (lambda x: x >= ch['value'])
    elif op == 'le':
        thr, ok = f"<= {C.fmt(ch['value'], tf)}", (lambda x: x <= ch['value'])
    elif op == 'abs_le':
        thr, ok = f"|x| <= {C.fmt(ch['value'], tf)}", (lambda x: abs(x) <= ch['value'])
    else:
        raise ValueError(f'freeze gate: unknown op {op!r}')
    return {'name': ch.get('label', ch['metric']), 'value': C.fmt(v, f), 'threshold': thr,
            'passed': ok(v) if C.is_num(v) else None, 'metric': ch['metric']}


def _integrity_row(r: dict, c: dict, base: str, gate: dict, run: str) -> dict:
    p0, p5 = _psr_at(r, 0.0), _psr_at(r, 0.5)
    checks = [_gate_check(r, ch) for ch in gate.get('checks') or []]
    res = [g['passed'] for g in checks]
    return {'label': c.get('label', base), 'name': base, 'run': run, 'present': True, 'net': r.get('net_sharpe'),
            'n': D.dig(r, 'deflated.n'), 'dsr': D.dig(r, 'deflated.dsr'), 'sr0': D.dig(r, 'deflated.sr0_annual'),
            'dsr_eff': D.dig(r, 'deflated_effective_n.dsr'), 'n_eff': D.dig(r, 'deflated_effective_n.n_eff'),
            'n_trials': D.dig(r, 'deflated_effective_n.n_trials'), 'sr0_eff': D.dig(r, 'deflated_effective_n.sr0_annual'),
            'dsr_lo': D.dig(r, 'deflated_lo_null.dsr'), 'sr0_lo': D.dig(r, 'deflated_lo_null.sr0_annual'),
            'psr0': p0.get('psr'), 'mintrl0_y': p0.get('min_trl_years'), 'mintrl0_s': p0.get('min_trl_sessions'),
            'psr5': p5.get('psr'), 'mintrl5_y': p5.get('min_trl_years'), 'alpha': D.dig(r, 'psr.alpha'),
            'skew': D.dig(r, 'net_moments.skew'), 'kurt': D.dig(r, 'net_moments.kurtosis'),
            'sessions': D.dig(r, 'net_moments.sessions'), 'checks': checks,
            'freeze': (None if not checks or any(x is None for x in res) else all(res))}


def an_integrity(ctx) -> dict:
    """Rows per configured cell per nav_summ run (``runs`` [{label, nav_summ, primary}] or one ``nav_summ``); the
    primary run (flagged, else the last) carries the freeze gate and the headline figures; the others show how the
    same cell's statistics moved with N."""
    ic = _conf(ctx, 'integrity')
    runs_cfg = list(ic.get('runs') or ([{'nav_summ': ic['nav_summ']}] if ic.get('nav_summ') else []))
    if not runs_cfg:
        raise KeyError("integrity: set runs[] or nav_summ")
    pk = next((k for k, rc in enumerate(runs_cfg) if rc.get('primary')), len(runs_cfg) - 1)
    prefix = ctx.cfg.get('cell_prefix', '')
    gate = ic.get('freeze_gate') or {}
    fin_name = str(ic.get('final') or '')
    runs, rows, missing, notes, finals = [], [], [], [], []
    primary = None
    for k, rc in enumerate(runs_cfg):
        rel = _spec(rc.get('nav_summ'))[0]
        label = rc.get('label') or Path(rel).stem
        try:
            rows_all = _load(ctx, rc.get('nav_summ'), f'nav_summ JSON {label}')
            if not isinstance(rows_all, list) or not all(isinstance(r, dict) for r in rows_all):
                raise ValueError(f'nav_summ JSON {rel}: expected a list of cell rows (nav_summ --json)')
        except (OSError, ValueError) as e:
            if k == pk:
                raise
            notes.append(f'nav_summ run {label}: {type(e).__name__}: {e}')
            continue
        by = {str(r.get('dir', '')).replace('\\', '/').rstrip('/').split('/')[-1]: r for r in rows_all}
        run_rows = []
        for c in ic.get('cells') or []:
            base = str(c.get('dir', '')).rstrip('/').split('/')[-1]
            r = by.get(base) or by.get(prefix + base)
            if r is None:
                missing.append(f'{base} ({label})')
                run_rows.append({'label': c.get('label', base), 'name': base, 'run': label, 'present': False})
                continue
            run_rows.append(_integrity_row(r, c, base, gate, label))
        fin = next((r for r in run_rows if r['present'] and r['name'] in (fin_name, prefix + fin_name)),
                   next((r for r in run_rows if r['present']), None))
        run = {'label': label, 'rows': run_rows, 'n_rows': len(rows_all), 'src': _src(ctx, rel), 'primary': k == pk,
               'final': fin, 'n_cells': max([r['n'] for r in run_rows if C.is_num(r.get('n'))], default=None)}
        runs.append(run)
        rows += run_rows
        if fin is not None:
            finals.append(fin)
        if k == pk:
            primary = run
    if primary is None:  # unreachable: a failed primary run raised above
        raise RuntimeError('integrity: primary nav_summ run not loaded')
    pbos = []
    for p in ic.get('pbo') or []:
        lab = p.get('label') if isinstance(p, dict) else _spec(p)[0]
        pj, err = _try(ctx, p, f'PBO {lab}')
        if pj is None or not isinstance(pj, dict) or not C.is_num(pj.get('pbo')):
            notes.append(f"PBO {lab}: {err or 'no pbo value'}")
            pbos.append({'label': lab, 'pbo': None})
            continue
        pbos.append({'label': lab, 'pbo': pj.get('pbo'), 'n': pj.get('n_candidates'), 'splits': pj.get('splits'),
                     'splits_total': pj.get('splits_total'), 'exhaustive': pj.get('exhaustive'), 'blocks': pj.get('blocks'),
                     'width': pj.get('block_width'), 'loss': pj.get('prob_winner_oos_loss'),
                     'slope': pj.get('degradation_slope'), 'is_sr': pj.get('winner_is_sr_annual_mean'),
                     'oos_sr': pj.get('winner_oos_sr_annual_mean'), 'cells': [str(x).split('/')[-1] for x in pj.get('cells') or []],
                     'src': _src(ctx, _spec(p)[0])})
    return {'runs': runs, 'rows': rows, 'missing': missing, 'pbo': pbos, 'notes': notes, 'finals': finals,
            'final': primary['final'], 'gate': gate, 'pbo_max': ic.get('pbo_max'), 'n_rows': primary['n_rows'],
            'primary_label': primary['label'], 'src': primary['src'], 'n_cells': primary['n_cells']}


def blk_integrity(ctx, spec) -> str:
    it = _need(ctx, 'integrity')
    fin = it['final']
    parts = []
    if fin:
        g = it['gate']
        gates = [{k: v for k, v in c.items() if k != 'metric'} for c in fin['checks']]
        gates.append({'name': 'Freeze gate (all of the above)', 'value': 'MET' if fin['freeze'] else
                      ('UNMET' if fin['freeze'] is False else None), 'threshold': 'all pass', 'passed': fin['freeze']})
        parts.append(C.gate_panel(gates, title=f"{g.get('name', 'Freeze gate')}: {fin['label']}, nav_summ "
                                               f"{it['primary_label']} (N {C.fmt(fin.get('n'), 'int')})"))
    rows = []
    multi = len(it['runs']) > 1
    for run in it['runs']:
        if multi:
            rows.append({'_group': f"nav_summ {run['label']}: {run['n_rows']} cells"
                                   + (' (primary: gate and headline figures)' if run['primary'] else '')})
        for r in run['rows']:
            row = dict(r)
            row['cell'] = f'{C.esc(r["label"])}<span class="sub">{C.esc(r["name"])}</span>'
            row['n_txt'] = f"N {C.fmt(r.get('n'), 'int')}; SR0 {C.fmt(r.get('sr0'), '.3f')}" if r.get('present') else None
            row['eff_txt'] = (f"N_eff {C.fmt(r.get('n_eff'), 'int')} of {C.fmt(r.get('n_trials'), 'int')}; SR0 "
                              f"{C.fmt(r.get('sr0_eff'), '.3f')}") if r.get('present') and C.is_num(r.get('n_eff')) else None
            row['lo_txt'] = f"SR0 {C.fmt(r.get('sr0_lo'), '.3f')}" if C.is_num(r.get('sr0_lo')) else None
            row['m0_txt'] = f"{C.fmt(r.get('mintrl0_s'), 'int')} sessions" if C.is_num(r.get('mintrl0_s')) else None
            row['fz'] = r.get('freeze')
            row['_cls'] = 'hl' if fin is not None and r is fin else ''
            rows.append(row)
    cols = [{'key': 'cell', 'label': 'Cell', 'kind': 'html'}, {'key': 'net', 'label': 'Net SR S2', 'fmt': '+.3f'},
            {'key': 'dsr', 'label': 'DSR, cell count', 'fmt': _with_sub('.3f', 'n_txt'), 'na_title': 'not in the nav_summ row'},
            {'key': 'dsr_eff', 'label': 'DSR, effective N', 'fmt': _with_sub('.3f', 'eff_txt'),
             'na_title': 'not computed (nav_summ without --effective-n); never replaced by the cell-count DSR'},
            {'key': 'dsr_lo', 'label': 'DSR, Lo single-cell null', 'fmt': _with_sub('.3f', 'lo_txt')},
            {'key': 'psr0', 'label': 'PSR(0)', 'fmt': '.3f'},
            {'key': 'mintrl0_y', 'label': 'MinTRL vs SR 0, years', 'fmt': _with_sub('.2f', 'm0_txt'),
             'na_title': 'undefined: SR at or below the benchmark'},
            {'key': 'psr5', 'label': 'PSR(0.5)', 'fmt': '.3f'},
            {'key': 'mintrl5_y', 'label': 'MinTRL vs SR 0.5, years', 'fmt': '.2f', 'na_title': 'undefined: SR at or below the benchmark'},
            {'key': 'skew', 'label': 'Skew', 'fmt': '+.2f'}, {'key': 'kurt', 'label': 'Kurtosis', 'fmt': '.1f'},
            {'key': 'sessions', 'label': 'Sessions', 'fmt': 'int'},
            {'key': 'fz', 'label': 'Freeze gate', 'kind': 'chip', 'na_title': 'a gate input is missing'}]
    miss = f" Not in the nav_summ file: {', '.join(it['missing'])}." if it['missing'] else ''
    srcs = '; '.join(r['src'] for r in it['runs'])
    parts.append(C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-integrity',
                         caption=(f"Integrity statistics per cell and nav_summ run: deflated Sharpe against the cell-count "
                                  f"null (N = every scored TRAIN construction cell; SR0 from the cross-cell spread of "
                                  f"per-session Sharpe ratios; the pre-registered gate statistic) and, beside it and never "
                                  f"in its place, against the effective number of independent trials (ONC clusters of the "
                                  f"trial ledger); the Lo (2002) single-cell null; probabilistic Sharpe vs SR* 0 and 0.5 with "
                                  f"the minimum track record length at 95%; daily-return skew and kurtosis; the freeze gate "
                                  f"evaluated on each row's own cell-count DSR.{miss} Sources: {srcs}.")))
    if it['pbo']:
        prow = []
        for p in it['pbo']:
            prow.append({**p, 'sp': (f"{C.fmt(p.get('splits'), 'int')} of {C.fmt(p.get('splits_total'), 'int')}"
                                     + (' (exhaustive)' if p.get('exhaustive') else '')) if C.is_num(p.get('splits')) else None,
                         'bl': f"{p.get('blocks')} x {p.get('width')}" if p.get('blocks') else None,
                         'ok': (p['pbo'] <= it['pbo_max']) if C.is_num(p.get('pbo')) and C.is_num(it['pbo_max']) else None,
                         'cells_txt': ', '.join(p.get('cells') or []) or None})
        pcols = [{'key': 'label', 'label': 'Candidate set', 'kind': 'text'}, {'key': 'n', 'label': 'Cells', 'fmt': 'int'},
                 {'key': 'pbo', 'label': 'PBO', 'fmt': '.3f'},
                 {'key': 'ok', 'label': f"PBO <= {C.fmt(it['pbo_max'], 'g')}", 'kind': 'chip'},
                 {'key': 'loss', 'label': 'P(winner loses OOS)', 'fmt': '.3f'},
                 {'key': 'slope', 'label': 'OOS on IS slope', 'fmt': '+.3f'},
                 {'key': 'is_sr', 'label': 'Winner IS SR, mean', 'fmt': '+.3f'},
                 {'key': 'oos_sr', 'label': 'Winner OOS SR, mean', 'fmt': '+.3f'},
                 {'key': 'sp', 'label': 'CSCV splits', 'kind': 'text'}, {'key': 'bl', 'label': 'Blocks x sessions', 'kind': 'text'},
                 {'key': 'cells_txt', 'label': 'Cells', 'kind': 'text', 'cls': 'wrap'}]
        psrc = '; '.join(p['src'] for p in it['pbo'] if p.get('src'))
        parts.append(C.table(pcols, prow, num=ctx.next_tab(), sortable=False, tid='t-pbo',
                             caption=(f"Probability of backtest overfitting by combinatorially symmetric cross-validation "
                                      f"(Bailey et al. 2017) over each configured candidate set: PBO = share of splits whose "
                                      f"in-sample winner ranks below the out-of-sample median; a winner is accepted only if "
                                      f"PBO <= {C.fmt(it['pbo_max'], 'g')} (pre-registered). Sources: {psrc or C.NA_TEXT}.")))
    parts.append(''.join(_note(n) for n in it['notes']))
    return ''.join(parts)


# ============================================================================================ 7. trial ledger
def an_ledger(ctx) -> dict:
    tl = _conf(ctx, 'trial_ledger')
    text = _load(ctx, tl.get('path'), 'trial ledger', text=True)
    rows, bad = [], 0
    for ln in text.splitlines():
        if not ln.strip():
            continue
        try:
            r = json.loads(ln)
        except json.JSONDecodeError:
            bad += 1
            continue
        if not isinstance(r, dict) or r.get('schema') not in SCHEMAS['ledger']:
            bad += 1
            continue
        rows.append(r)
    if not rows:
        raise ValueError(f"trial ledger {_spec(tl['path'])[0]}: no {SCHEMAS['ledger'][0]} lines ({bad} unreadable)")
    kind = tl.get('kind')
    prefix = tl.get('cell_prefix', ctx.cfg.get('cell_prefix', ''))
    eras = [(e['label'], re.compile(e['match'])) for e in tl.get('eras') or [] if e.get('label') and e.get('match')]
    verdicts = tl.get('verdicts') or {}
    rules = ctx.verdict_rules()
    out_rows, cum, era_n = [], 0, {}
    for i, r in enumerate(rows, 1):
        if kind and r.get('kind') != kind:
            continue
        base = str(r.get('cell', '')).replace('\\', '/').rstrip('/').split('/')[-1]
        name = base[len(prefix):] if prefix and base.startswith(prefix) else base
        cnt = r.get('count') if C.is_num(r.get('count')) else 1
        cum += cnt
        era = next((lab for lab, rx in eras if rx.search(base)), 'other')
        era_n[era] = era_n.get(era, 0) + cnt
        cell = ctx.by.get(name)
        v = verdicts.get(name, verdicts.get(base))
        vtext = (v.get('verdict') if isinstance(v, dict) else v) if v is not None else (cell.verdict if cell else None)
        label = (v.get('label') if isinstance(v, dict) else None) or (cell.label if cell else None)
        # an explicit kind wins: the ledger entry's ``kind``, else (verdict from the cell config) the cell's verdict_kind
        explicit = v.get('kind') if isinstance(v, dict) else (ctx.verdict_kind_of(name) if v is None and cell else None)
        vkind = explicit if vtext else None
        vkind = vkind or C.verdict_kind(vtext, rules)
        out_rows.append({'k': i, 'trial_id': r.get('trial_id'), 'cell': base, 'name': name, 'era': era,
                         'label': label, 'kind': r.get('kind'),
                         'window': D.dig(r, 'window.label'), 'sessions': D.dig(r, 'window.sessions'),
                         'sr': r.get('s2_net_sr'), 'count': cnt, 'cum': cum,
                         'head': str(D.dig(r, 'recorded_by.git_head') or '')[:8] or None,
                         'verdict': vtext, 'vkind': vkind})
    era_order = list(dict.fromkeys(r['era'] for r in out_rows))
    cum_by_era, run = [], 0
    for e in era_order:
        run += era_n[e]
        cum_by_era.append({'era': e, 'n': era_n[e], 'cum': run})
    kinds = {}
    for r in out_rows:
        kinds[r['vkind'] or 'none'] = kinds.get(r['vkind'] or 'none', 0) + 1
    val = tl.get('validation') or {}
    hold = tl.get('holdout') or {}
    windows = sorted({r['window'] for r in out_rows if r['window']})
    return {'rows': out_rows, 'n': cum, 'lines': len(rows), 'unreadable': bad, 'eras': cum_by_era, 'kinds': kinds,
            'validation_spent': val.get('spent'), 'validation_budget': val.get('budget'),
            'validation_window': val.get('window'), 'validation_note': val.get('note'),
            'holdout_window': hold.get('window'), 'holdout_status': hold.get('status'), 'windows': windows,
            'src': _src(ctx, _spec(tl['path'])[0]), 'kind_filter': kind}


def blk_trial_ledger(ctx, spec) -> str:
    tl = _need(ctx, 'ledger')
    integ = ctx.analysis('integrity') if isinstance(ctx.cfg.get('integrity'), dict) else {'_error': 'not configured'}
    n_summ = integ.get('n_cells') if '_error' not in integ else None
    match = (tl['n'] == n_summ) if C.is_num(n_summ) else None
    k = tl['kinds']
    vb = tl['validation_budget']
    kpis = [{'label': 'TRAIN trials in the ledger', 'value': C.fmt(tl['n'], 'int'),
             'sub': (f"= nav_summ N {C.fmt(n_summ, 'int')}" if match else
                     (f"nav_summ N {C.fmt(n_summ, 'int')}: DIFFERS" if match is False else 'nav_summ N not configured'))},
            {'label': 'Accepted / rejected / defect', 'value': f"{k.get('accepted', 0)} / {k.get('rejected', 0)} / {k.get('defect', 0)}",
             'sub': f"other {k.get('other', 0)}, no verdict {k.get('none', 0)}"},
            {'label': 'Validation trials spent', 'value': (f"{C.fmt(tl['validation_spent'], 'int')}"
                                                           + (f" of {C.fmt(vb, 'int')}" if C.is_num(vb) else ''))
             if C.is_num(tl['validation_spent']) else None,
             'sub': ' '.join(x for x in [tl['validation_window'], tl['validation_note']] if x) or None},
            {'label': 'Holdout', 'value': (f"{tl['holdout_window']} {tl['holdout_status']}" if tl['holdout_window'] else None),
             'sub': 'never read by any trial'}]
    parts = [f'<div class="panel">{C.kpi_strip(kpis)}</div>']
    eras = tl['eras']
    if eras:
        cats = [f"{e['era']} (+{e['n']})" for e in eras]
        svg = C.bar_chart(cats, [{'name': 'cumulative N', 'color': 'accent', 'label': True,
                                  'values': [e['cum'] for e in eras]}], value_fmt='int', tick_fmt='int', height=260,
                          y_label='Cumulative TRAIN construction trials N', aria='trial count over the ledger')
        parts.append(C.figure(ctx.next_fig(), svg,
                              (f"N over the ledger: cumulative count of TRAIN construction trials after each era, in "
                               f"ledger order (era = the config's cell-name patterns; +n = trials in the era). The DSR "
                               f"deflates by this N. Source: {tl['src']}."), 'fig-ledger-n'))
    rows = []
    for r in tl['rows']:
        rows.append({**r, 'cell_html': f'{C.esc(r["label"] or r["name"])}<span class="sub">{C.esc(r["cell"])}</span>',
                     'cell_html__sort': r['name'], 'vh': C.verdict_html(r['verdict'], r['vkind']),
                     'vh__sort': r['verdict'], 'tid': r['trial_id']})
    cols = [{'key': 'k', 'label': '#', 'fmt': 'int'}, {'key': 'cell_html', 'label': 'Cell', 'kind': 'html'},
            {'key': 'vh', 'label': 'Verdict', 'kind': 'html', 'na_title': 'no verdict configured for this cell'},
            {'key': 'sr', 'label': 'S2 net SR', 'fmt': '+.3f'}, {'key': 'cum', 'label': 'N after', 'fmt': 'int'},
            {'key': 'era', 'label': 'Era', 'kind': 'text'}, {'key': 'window', 'label': 'Window', 'kind': 'text'},
            {'key': 'count', 'label': 'Count', 'fmt': 'int'}, {'key': 'head', 'label': 'Scored at', 'kind': 'mono'},
            {'key': 'tid', 'label': 'Trial id', 'kind': 'mono'}]
    bad = f" {tl['unreadable']} ledger lines were unreadable or of another schema and are not counted." if tl['unreadable'] else ''
    parts.append(C.table(cols, rows, num=ctx.next_tab(), sortable=True, tid='t-trial-ledger',
                         caption=(f"Trial ledger: every {tl['kind_filter'] or 'recorded'} trial in the order it was scored, "
                                  f"with its window ({', '.join(tl['windows']) or C.NA_TEXT}), the S2 net Sharpe recorded "
                                  f"with it, the running N and the verdict (config trial_ledger.verdicts, else the "
                                  f"configured cell's verdict; badge by the verdict rules).{bad} Source: {tl['src']}.")))
    return ''.join(parts)


ANALYSES = {'capcurve': an_capcurve, 'bias': an_bias, 'cards': an_cards, 'monitor_base': an_monitor_base, 'ops': an_ops,
            'integrity': an_integrity, 'ledger': an_ledger}
BLOCKS = {'capacity_curve': blk_capacity_curve, 't_risk_bias': blk_risk_bias, 't_report_cards': blk_report_cards,
          't_monitor_baseline': blk_monitor_baseline, 'ops_loop': blk_ops_loop, 't_integrity': blk_integrity,
          't_trial_ledger': blk_trial_ledger}
