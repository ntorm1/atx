"""Assemble the mega-alpha HTML report from a declarative config.

``build(config_path) -> html``. No statistic lives in this file: every value is read from the config or from a
file the config names (nav_summ --json rows, per-cell summary.json / daily / events CSVs / runner receipts, the
library and recipe JSON, admission and composition-weights JSON, the trial-accounting source lines). A missing or
unreadable input renders as an explicit n/a; a block that cannot be built renders a one-line "not available"
marker instead of stopping the build.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
import re
import traceback
from pathlib import Path

import numpy as np

from . import components as C
from . import data as D
from . import theme as T

MONTHS = ('Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec')
LEQ = '≤'
GEQ = '≥'


def _na_block(name: str, err: Exception) -> str:
    return (f'<div class="unavailable" data-block="{C.esc(name)}">{C.esc(name)}: not available '
            f'({C.esc(type(err).__name__)}: {C.esc(str(err)[:200])})</div>')


def _finite(xs) -> list[float]:
    return [float(x) for x in xs if C.is_num(x)]


def _list(arr) -> list:
    return [float(v) if np.isfinite(v) else None for v in np.asarray(arr, dtype=np.float64)]


class Ctx:
    """Loaded inputs plus figure/table counters for one build."""

    def __init__(self, cfg: dict, cfg_bytes: bytes, cfg_path: Path, root: Path, stamp: str):
        self.cfg, self.cfg_path, self.root, self.stamp = cfg, cfg_path, root, stamp
        self.cfg_sha = hashlib.sha256(cfg_bytes).hexdigest()
        self.reg = D.Registry(root)
        self.annual = cfg.get('sessions_per_year')
        self.dsr_n = cfg.get('dsr_n')
        self.scen = list(cfg.get('scenarios', []))
        self.sc = {s['key']: s for s in self.scen}
        self.pkey = cfg.get('primary_scenario')
        self.primary = self.sc.get(self.pkey)
        inp = cfg.get('inputs', {})
        self.summ_rows = self.reg.read_json(inp['nav_summ_json']) if inp.get('nav_summ_json') else None
        norm = dict(cfg)
        prefix = cfg.get('cell_prefix', '')
        norm['cells'] = []
        for c in cfg.get('cells', []):
            c = dict(c)
            par = c.get('parent')
            if par and '/' in par:
                base = par.rstrip('/').split('/')[-1]
                c['parent'] = base[len(prefix):] if prefix and base.startswith(prefix) else base
            norm['cells'].append(c)
        self.cells = D.load_cells(self.reg, norm, D.summ_index(self.summ_rows))
        self.by = {c.name: c for c in self.cells}
        self.final = self.by.get(cfg.get('final'))
        self.groups = {g['key']: g for g in cfg.get('groups', [])}
        self.tags = cfg.get('exe_tags', {})
        self.fig = 0
        self.tab = 0
        self._lo: dict = {}
        self._par: dict = {}

    # ---------------------------------------------------------------- numbering
    def next_fig(self) -> int:
        self.fig += 1
        return self.fig

    def next_tab(self) -> int:
        self.tab += 1
        return self.tab

    # ---------------------------------------------------------------- metrics
    def pid(self) -> str | None:
        return self.primary['id'] if self.primary else None

    def lo(self, cell) -> dict | None:
        if cell.name not in self._lo:
            m = (cell.summ or {}).get('net_moments')
            ok = isinstance(self.dsr_n, int) and C.is_num(self.annual)
            self._lo[cell.name] = D.lo_dsr(m, self.dsr_n, self.annual) if ok else None
        return self._lo[cell.name]

    def vs_parent(self, cell) -> dict | None:
        if cell.name not in self._par:
            par = self.by.get(cell.parent) if cell.parent else None
            pid = self.pid()
            ok = par is not None and pid and C.is_num(self.annual)
            self._par[cell.name] = D.paired(cell.daily(pid), par.daily(pid), self.annual) if ok else None
        return self._par[cell.name]

    def metric(self, cell, path: str):
        """``summ.<path>`` nav_summ row, ``sum.<path>`` summary.json, ``scen[:KEY].<path>`` a summary scenario
        (primary by default), ``lo.<path>`` Lo-null DSR, ``parent.<path>`` paired vs the configured parent."""
        if cell is None:
            return None
        ns, _, rest = path.partition('.')
        if ns == 'summ':
            return D.dig(cell.summ, rest)
        if ns == 'sum':
            return D.dig(cell.summary, rest)
        if ns.startswith('scen'):
            key = ns.split(':', 1)[1] if ':' in ns else self.pkey
            sc = self.sc.get(key)
            return D.dig(cell.scen(sc['id']), rest) if sc else None
        if ns == 'lo':
            return D.dig(self.lo(cell), rest)
        if ns == 'parent':
            return D.dig(self.vs_parent(cell), rest)
        return None

    def years(self, cell, key=None) -> dict:
        key = key or self.pkey
        out = {}
        for y in self.metric(cell, f'scen:{key}.calendar_year_returns') or []:
            if isinstance(y, dict) and 'year' in y:
                out[y['year']] = y.get('net_compounded_return')
        return out

    def fill(self, text, cell=None):
        """Replace ``{metric.path}`` or ``{metric.path|spec}`` in config text with the value for ``cell``."""
        if not text:
            return text
        cell = cell or self.final

        def sub(m):
            return C.fmt(self.metric(cell, m.group(1)), m.group(2) or 'g') or C.NA_TEXT
        return re.sub(r'\{([A-Za-z0-9_:.-]+)(?:\|([^{}]+))?\}', sub, text)

    # ---------------------------------------------------------------- groups and gates
    def gtok(self, cell) -> str:
        return (self.groups.get(cell.group) or {}).get('token', 'ref')

    def glabel(self, cell) -> str:
        return (self.groups.get(cell.group) or {}).get('label', cell.group or '')

    def ghollow(self, cell) -> bool:
        return bool((self.groups.get(cell.group) or {}).get('hollow'))

    def dot(self, cell) -> str:
        h = ' hollow' if self.ghollow(cell) else ''
        return f'<span class="dot{h}" style="--c:var(--{C.esc(self.gtok(cell))})"></span>'

    def gate(self, cell, g: dict) -> dict:
        spec, tspec = g.get('fmt', '.3f'), g.get('threshold_fmt', g.get('fmt', '.3f').lstrip('+'))
        vals, ths, passed = [], [], True
        for ch in g.get('checks', []):
            v = self.metric(cell, ch['metric'])
            vals.append(C.fmt(v, spec) or C.NA_TEXT)
            op = ch['op']
            if op == 'between':
                ths.append(f"[{C.fmt(ch['lo'], tspec)}, {C.fmt(ch['hi'], tspec)}]")
                ok = C.is_num(v) and ch['lo'] <= v <= ch['hi']
            elif op == 'le':
                ths.append(f"{LEQ} {C.fmt(ch['value'], tspec)}")
                ok = C.is_num(v) and v <= ch['value']
            elif op == 'ge':
                ths.append(f"{GEQ} {C.fmt(ch['value'], tspec)}")
                ok = C.is_num(v) and v >= ch['value']
            elif op == 'abs_le':
                ths.append(f"|x| {LEQ} {C.fmt(ch['value'], tspec)}")
                ok = C.is_num(v) and abs(v) <= ch['value']
            else:
                raise ValueError(f'unknown gate op {op!r}')
            if not C.is_num(v):
                passed = None
            elif passed is not None:
                passed = passed and ok
        return {'name': self.fill(g['name'], cell), 'value': ' / '.join(vals), 'threshold': ' / '.join(ths),
                'passed': passed}

    def mechanics(self, cell):
        gs = [g for g in self.cfg.get('gates', []) if g.get('mechanics')]
        res = [self.gate(cell, g)['passed'] for g in gs]
        if not gs or any(r is None for r in res):
            return None
        return all(res)


# ============================================================================================ blocks
def blk_kpi(ctx: Ctx) -> str:
    f = ctx.final
    items = []
    for k in ctx.cfg.get('kpis', []):
        parts = [C.fmt(ctx.metric(f, m), k.get('fmt', '.3f')) for m in k.get('metrics', [])]
        value = None if all(p is None for p in parts) else ' / '.join(p or C.NA_TEXT for p in parts)
        items.append({'label': ctx.fill(k['label']), 'value': value, 'sub': ctx.fill(k.get('sub'))})
    head = f'<p class="panel-title">{C.esc(f.label if f else C.NA_TEXT)} · {C.esc(f.name if f else "")}</p>'
    return f'<div class="panel">{head}{C.kpi_strip(items)}</div>'


def blk_gates(ctx: Ctx) -> str:
    gates = [ctx.gate(ctx.final, g) for g in ctx.cfg.get('gates', [])]
    return C.gate_panel(gates, title=ctx.cfg.get('gate_title', 'Pre-registered conditions'))


def blk_cells(ctx: Ctx) -> str:
    pid = ctx.pid()
    years = sorted({y for c in ctx.cells for y in ctx.years(c)})
    nets = _finite(ctx.metric(c, 'summ.net_sharpe') for c in ctx.cells)
    dps = _finite(ctx.metric(c, 'parent.dsr') for c in ctx.cells)
    rows = []
    for c in ctx.cells:
        yr = ctx.years(c)
        par = ctx.by.get(c.parent) if c.parent else None
        r = {'cell': f'{C.esc(c.label)}<span class="sub">{C.esc(c.name)}</span>', 'cell__sort': c.label,
             'group': f'{ctx.dot(c)}{C.esc(ctx.glabel(c))}', 'group__sort': ctx.glabel(c),
             'net': ctx.metric(c, 'summ.net_sharpe'), 'gross': ctx.metric(c, 'summ.gross_sharpe'),
             'hac': ctx.metric(c, 'summ.hac_t'), 'mu': ctx.metric(c, 'scen.ann_mean'), 'vol': ctx.metric(c, 'scen.ann_vol'),
             'mdd': ctx.metric(c, 'scen.max_drawdown'),
             'gl': ctx.metric(c, 'summ.mean_gross_leverage_all_rows'), 'nl': ctx.metric(c, 'summ.mean_net_leverage_all_rows'),
             'tm': ctx.metric(c, 'summ.tau_gmv_mean'), 'tp': ctx.metric(c, 'summ.tau_gmv_p95'),
             'cost': ctx.metric(c, 'summ.cost_bps_traded'),
             'dpar': ctx.metric(c, 'parent.dsr'), 'dpar_se': ctx.metric(c, 'parent.se'),
             'par': par.label if par else None,
             'dref': ctx.metric(c, 'summ.paired.dsr') if ctx.metric(c, 'summ.paired.t') is not None else None,
             'dref_se': ctx.metric(c, 'summ.paired.memmel_se'),
             'dsrx': ctx.metric(c, 'summ.deflated.dsr'), 'dsrlo': ctx.metric(c, 'lo.dsr'),
             'mech': ctx.mechanics(c), 'verdict': c.verdict, 'exe': c.exe(ctx.tags),
             '_cls': 'hl' if c is ctx.final else ''}
        for y in years:
            r[f'y{y}'] = yr.get(y)
        if c.parent and par is None:
            r['par'] = None
        rows.append(r)

    def with_se(key):
        def f(v, row):
            if not C.is_num(v):
                return None
            se = C.fmt(row.get(f'{key}_se'), '.3f')
            return f'{C.esc(C.fmt(v, "+.3f"))} <span class="sub" style="display:inline">({C.esc(se or C.NA_TEXT)})</span>'
        return f

    span = max([abs(x) for x in dps] or [1.0])
    cols = [{'key': 'cell', 'label': 'Cell', 'kind': 'html'}, {'key': 'group', 'label': 'Group', 'kind': 'html'},
            {'key': 'net', 'label': f"Net SR {ctx.primary['short'] if ctx.primary else ''}", 'fmt': '+.3f',
             'bar': {'lo': min([0.0] + nets), 'hi': max([0.0] + nets)}},
            {'key': 'gross', 'label': 'Gross SR', 'fmt': '.3f'}, {'key': 'hac', 'label': 'HAC t', 'fmt': '.2f'},
            {'key': 'mu', 'label': 'Mean/yr', 'fmt': '+pct2'}, {'key': 'vol', 'label': 'Vol/yr', 'fmt': 'pct2'},
            {'key': 'mdd', 'label': 'MDD', 'fmt': 'pct1'}]
    cols += [{'key': f'y{y}', 'label': str(y), 'fmt': '+pct1'} for y in years]
    cols += [{'key': 'gl', 'label': 'Gross lev all', 'fmt': '.3f'}, {'key': 'nl', 'label': 'Net lev all', 'fmt': '+.4f'},
             {'key': 'tm', 'label': 'Tau mean', 'fmt': '.4f'}, {'key': 'tp', 'label': 'Tau p95', 'fmt': '.4f'},
             {'key': 'cost', 'label': 'Cost bps/$', 'fmt': '.2f'},
             {'key': 'dpar', 'label': 'dSR vs parent (SE)', 'fmt': with_se('dpar'), 'bar': {'lo': -span, 'hi': span}},
             {'key': 'par', 'label': 'Parent', 'kind': 'text', 'na_title': 'no parent configured'},
             {'key': 'dref', 'label': 'dSR vs REF (SE)', 'fmt': with_se('dref')},
             {'key': 'dsrx', 'label': 'DSR x-cell', 'fmt': '.3f'}, {'key': 'dsrlo', 'label': 'DSR Lo', 'fmt': '.3f'},
             {'key': 'mech', 'label': 'Mechanics', 'kind': 'chip'}, {'key': 'verdict', 'label': 'Verdict', 'kind': 'text'},
             {'key': 'exe', 'label': 'NAV exe', 'kind': 'mono'}]
    n = ctx.next_tab()
    src = ctx.cfg.get('inputs', {}).get('nav_summ_json', 'nav_summ json')
    cap = (f"All {len(ctx.cells)} cells, scenario {ctx.primary['label'] if ctx.primary else C.NA_TEXT}: Sharpe, HAC t, "
           f"annualised mean and vol, maximum drawdown, calendar-year net return, mean gross and net leverage over all "
           f"CSV rows, daily one-way GMV turnover, cost per traded dollar, paired dSR vs the configured parent and vs "
           f"the nav_summ reference (Memmel SE), cross-cell and Lo-null deflated Sharpe at N = {ctx.dsr_n}, mechanics "
           f"gates. Sources: {Path(src).name}, per-cell summary.json, daily_{pid}.csv, runner receipts.")
    return C.table(cols, rows, num=n, caption=cap, tid='t-cells')


def _nav_series(ctx: Ctx, cell, sc: dict, **kw) -> dict | None:
    d = cell.daily(sc['id']) if cell else None
    if d is None:
        return None
    dates, nav = D.nav_path(d)
    if not dates:
        return None
    s = {'x': dates, 'y': nav.tolist(), 'dd': D.drawdown(nav).tolist(), 'last': float(nav[-1])}
    s.update(kw)
    return s


def blk_equity(ctx: Ctx) -> str:
    ec, f = ctx.cfg.get('equity', {}), ctx.final
    series, missing = [], []
    for key in ec.get('scenarios') or [s['key'] for s in ctx.scen]:
        sc = ctx.sc[key]
        emph = key == ctx.pkey
        s = _nav_series(ctx, f, sc, name=f"{f.label} {sc['label']}", color=sc['token'], width=2.4 if emph else 1.3,
                        emph=emph, trough=emph, short=sc['short'])
        if s is None:
            missing.append(sc['label'])
            continue
        series.append(s)
    for ex in ec.get('extra', []):
        c, sc = ctx.by.get(ex['cell']), ctx.sc[ex.get('scenario', ctx.pkey)]
        label = ex.get('label') or (c.label if c else ex['cell'])
        s = _nav_series(ctx, c, sc, name=f"{label} {sc['short']}", color=ex.get('color', 'ref'),
                        width=ex.get('width', 1.5), dash=ex.get('dash'), trough=ex.get('trough', False),
                        short=ex.get('short', label))
        if s is None:
            missing.append(label)
            continue
        series.append(s)
    for s in series:
        s['end_label'] = f"{s['short']} {C.fmt(s['last'], '.3f')}"
    nav = C.line_chart(series, height=380, y_fmt='.2f', ref_values=(1.0,), y_label='NAV, net of costs (start 1.0)',
                       aria='net NAV equity curves')
    dd = C.drawdown_chart([dict(s, y=s['dd']) for s in series], height=180, y_label='Drawdown from running peak',
                          aria='drawdown')
    lg = C.legend([{'name': s['name'], 'color': s['color'], 'dash': s.get('dash'), 'width': max(s['width'], 2.0),
                    'value': C.fmt(s['last'], '.3f')} for s in series]
                  + [{'name': f'{m}: {C.NA_TEXT}', 'color': 'rule', 'kind': 'bar'} for m in missing])
    extras = ', '.join(s['name'] for s in series if not s['name'].startswith(f.label))
    cap = (f"Net NAV (start 1.0 on the session before the first return row) of {f.label} under each cost scenario "
           f"({ctx.primary['short']} emphasised) and {extras or 'no reference cell'}; drawdown from the running peak "
           f"below, emphasised trough labelled. Source: daily_<scenario>.csv net_return over return rows of "
           f"{f.rel_dir} and the reference cell dirs.")
    return C.figure(ctx.next_fig(), nav + dd, cap, 'fig-equity', lg)


def blk_returns(ctx: Ctx) -> str:
    f, sc = ctx.final, ctx.primary
    d = f.daily(sc['id'])
    if d is None:
        raise FileNotFoundError(f'daily_{sc["id"]}.csv')
    m = D.monthly_returns(d)
    years = sorted({y for y, _ in m})
    vals = [[m.get((y, mo)) for mo in range(1, 13)] for y in years]
    yearly = D.yearly_returns(d)
    hm = C.heatmap([str(y) for y in years], list(MONTHS), vals, total=[yearly.get(y) for y in years],
                   total_label='Year', aria='monthly net returns')
    ys = sorted({y for s in ctx.scen for y in ctx.years(f, s['key'])})
    bser = [{'name': s['label'], 'color': s['token'], 'label': s['key'] == ctx.pkey,
             'values': [ctx.years(f, s['key']).get(y) for y in ys]} for s in ctx.scen]
    bars = C.bar_chart([str(y) for y in ys], bser, value_fmt='+pct1', tick_fmt='+pct0', height=260,
                       y_label='Calendar-year net return', aria='calendar-year net return by scenario')
    lg = C.legend([{'name': s['label'], 'color': s['token'], 'kind': 'bar'} for s in ctx.scen])
    body = (f'<p class="panel-label">(a) Monthly net return, {C.esc(sc["label"])}</p>{hm}'
            f'<p class="panel-label">(b) Calendar-year net return by scenario</p>{lg}{bars}')
    cap = (f"(a) Compounded monthly net return of {f.label}, {sc['label']}, months x years, with the compounded "
           f"calendar-year return; 7-step diverging scale, neutral within half a step of zero. (b) Calendar-year "
           f"net return of {f.label} per cost scenario, {sc['short']} labelled. Sources: daily_{sc['id']}.csv "
           f"net_return (a); summary.json calendar_year_returns (b).")
    return C.figure(ctx.next_fig(), body, cap, 'fig-returns')


def blk_rolling(ctx: Ctx) -> str:
    rc = ctx.cfg.get('rolling', {})
    w, sc = rc.get('window'), ctx.sc[rc.get('scenario', ctx.pkey)]
    series = []
    for it in rc.get('cells', []):
        c = ctx.by.get(it['cell'])
        d = c.daily(sc['id']) if c else None
        if d is None:
            continue
        dates, r = D.returns(d)
        rs = _list(D.rolling_sharpe(r, int(w), ctx.annual))
        last = next((v for v in reversed(rs) if v is not None), None)
        label = it.get('label') or c.label
        series.append({'name': f'{label} {sc["short"]}', 'x': dates, 'y': rs, 'color': it.get('color', 's2'),
                       'width': it.get('width', 1.6), 'dash': it.get('dash'), 'emph': it.get('emph', False),
                       'end_label': f"{it.get('short', label)} {C.fmt(last, '+.2f')}", 'last': last})
    svg = C.line_chart(series, height=300, y_fmt='+.1f', ref_values=(0.0,),
                       y_label=f'Rolling {w}-session net Sharpe (annualised)', aria='rolling Sharpe')
    lg = C.legend([{'name': s['name'], 'color': s['color'], 'dash': s.get('dash'), 'width': 2.0,
                    'value': C.fmt(s['last'], '+.2f')} for s in series])
    cap = (f"Trailing {w}-session Sharpe of daily net returns (mean / sd x sqrt({ctx.annual})), {sc['label']}, "
           f"{' vs '.join(s['name'] for s in series)}; first value after {w} return rows. Source: daily_{sc['id']}.csv "
           f"net_return over return rows.")
    return C.figure(ctx.next_fig(), svg, cap, 'fig-rolling', lg)


def blk_cost_drag(ctx: Ctx) -> str:
    f = ctx.final
    series = []
    for s in ctx.scen:
        d = f.daily(s['id'])
        if d is None:
            continue
        dates, drag = D.cost_drag(d)
        if not drag.size:
            continue
        emph = s['key'] == ctx.pkey
        series.append({'name': f"{f.label} {s['label']}", 'x': dates, 'y': drag.tolist(), 'color': s['token'],
                       'width': 2.4 if emph else 1.3, 'emph': emph, 'last': float(drag[-1]),
                       'end_label': f"{s['short']} {C.fmt(float(drag[-1]), '.3f')}"})
    svg = C.line_chart(series, height=300, y_fmt='.3f', ref_values=(0.0,), y_label='Gross NAV minus net NAV',
                       aria='cumulative cost drag')
    lg = C.legend([{'name': s['name'], 'color': s['color'], 'width': 2.0, 'value': C.fmt(s['last'], '.3f')} for s in series])
    cap = (f"Cumulative cost drag of {f.label} per scenario: NAV compounded from gross_return minus NAV compounded "
           f"from net_return (trading cost, financing and write-offs), both starting at 1.0. Source: "
           f"daily_<scenario>.csv gross_return and net_return over return rows.")
    return C.figure(ctx.next_fig(), svg, cap, 'fig-cost', lg)


def blk_stress(ctx: Ctx) -> str:
    f = ctx.final
    years = sorted({y for s in ctx.scen for y in ctx.years(f, s['key'])})
    base = ctx.metric(f, 'scen.net_sharpe')
    rows = []
    for s in ctx.scen:
        k = s['key']
        d = f.daily(s['id'])
        nav = D.nav_path(d)[1] if d is not None else np.array([])
        ev = f.events(s['id'])
        wo = [e for e in ev or [] if e.get('kind') == ctx.cfg.get('writeoff_kind', 'write-off')]
        net = ctx.metric(f, f'scen:{k}.net_sharpe')
        csv_sha = ctx.metric(f, f'scen:{k}.daily_csv_sha256')
        file_sha = (ctx.reg.files.get(f'{f.rel_dir}/daily_{s["id"]}.csv') or {}).get('sha256')
        r = {'sc': s['label'], 'id': s['id'], 'net': net,
             'dnet': (net - base) if C.is_num(net) and C.is_num(base) else None,
             'gross': ctx.metric(f, f'scen:{k}.gross_sharpe'), 'hac': ctx.metric(f, f'scen:{k}.hac_t'),
             'mu': ctx.metric(f, f'scen:{k}.ann_mean'), 'vol': ctx.metric(f, f'scen:{k}.ann_vol'),
             'mdd': ctx.metric(f, f'scen:{k}.max_drawdown'), 'nav': float(nav[-1]) if nav.size else None,
             'tc': ctx.metric(f, f'scen:{k}.costs.summed_trade_cost_return'),
             'sf': ctx.metric(f, f'scen:{k}.costs.summed_borrow_return'),
             'lf': ctx.metric(f, f'scen:{k}.financing.summed_long_financing_return'),
             'wo_n': len(wo) if ev is not None else None,
             'wo_pnl': sum(e['pnl_dollars'] for e in wo) / 1e6 if ev is not None else None,
             'sha': csv_sha[:12] if csv_sha else None,
             'shaok': (csv_sha == file_sha) if csv_sha and file_sha else None,
             '_cls': 'hl' if k == ctx.pkey else ''}
        for y in years:
            r[f'y{y}'] = ctx.years(f, k).get(y)
        rows.append(r)
    nets = _finite(r['net'] for r in rows)
    cols = [{'key': 'sc', 'label': 'Scenario', 'kind': 'text'}, {'key': 'id', 'label': 'Scenario id', 'kind': 'mono'},
            {'key': 'net', 'label': 'Net SR', 'fmt': '+.3f', 'bar': {'lo': min([0.0] + nets), 'hi': max([0.0] + nets)}},
            {'key': 'dnet', 'label': f"vs {ctx.primary['short']}", 'fmt': '+.3f'},
            {'key': 'gross', 'label': 'Gross SR', 'fmt': '.3f'}, {'key': 'hac', 'label': 'HAC t', 'fmt': '.2f'},
            {'key': 'mu', 'label': 'Mean/yr', 'fmt': '+pct2'}, {'key': 'vol', 'label': 'Vol/yr', 'fmt': 'pct2'},
            {'key': 'mdd', 'label': 'MDD', 'fmt': 'pct1'}]
    cols += [{'key': f'y{y}', 'label': str(y), 'fmt': '+pct1'} for y in years]
    cols += [{'key': 'nav', 'label': 'Final NAV', 'fmt': '.3f'},
             {'key': 'tc', 'label': 'Trade cost sum', 'fmt': 'pct2'}, {'key': 'sf', 'label': 'Short fin. sum', 'fmt': 'pct2'},
             {'key': 'lf', 'label': 'Long fin. sum', 'fmt': 'pct2'},
             {'key': 'wo_n', 'label': 'Write-offs', 'fmt': 'int'}, {'key': 'wo_pnl', 'label': 'Write-off P&L $M', 'fmt': '+.2f'},
             {'key': 'sha', 'label': 'Daily CSV sha256', 'kind': 'mono'},
             {'key': 'shaok', 'label': 'File = summary', 'kind': 'chip'}]
    cap = (f"Cost and financing stresses of {f.label} ({f.name}): per scenario Sharpe, change vs "
           f"{ctx.primary['short']}, mean, vol, drawdown, calendar-year net return, final NAV, summed cost and "
           f"financing returns, write-off events and P&L; the daily CSV hash in summary.json checked against the file. "
           f"Sources: summary.json scenarios, daily_<scenario>.csv, events_<scenario>.csv.")
    return C.table(cols, rows, num=ctx.next_tab(), caption=cap, tid='t-stress')


def blk_ladder(ctx: Ctx) -> str:
    lc = ctx.cfg.get('ladder', {})
    items = lc.get('cells', [])
    cells = [ctx.by.get(it['cell']) for it in items]
    pid = ctx.pid()
    if not cells or cells[0] is None:
        raise KeyError('ladder start cell')
    start = {'label': items[0].get('label', cells[0].label), 'value': ctx.metric(cells[0], 'summ.net_sharpe'),
             'color': 'ref-2'}
    steps = []
    for (pi, prev), (ci, cur) in zip(list(enumerate(cells))[:-1], list(enumerate(cells))[1:]):
        p = D.paired(cur.daily(pid), prev.daily(pid), ctx.annual) if cur and prev else None
        lab = items[ci].get('label', cur.label if cur else items[ci]['cell'])
        if cur and prev and cur.parent and cur.parent != prev.name:
            lab += '\n(vs ' + (items[pi].get('label') or prev.label) + ')'
        steps.append({'label': lab, 'delta': p['dsr'] if p else None, 'se': p['se'] if p else None})
    last = cells[-1]
    end = {'label': items[-1].get('end_label', last.label if last else C.NA_TEXT),
           'value': ctx.metric(last, 'summ.net_sharpe'), 'color': 'accent'}
    svg = C.waterfall(start, steps, end, height=360, value_fmt='+.3f', total_fmt='.3f',
                      y_label=f"Net Sharpe, {ctx.primary['short']}", aria='lever ladder')
    lg = C.legend([{'name': 'Start / end cell net Sharpe', 'color': 'ref-2', 'kind': 'bar'},
                   {'name': 'Positive step', 'color': 'pos', 'kind': 'bar'},
                   {'name': 'Negative step', 'color': 'neg', 'kind': 'bar'},
                   {'name': 'Whisker: step +- 1 Memmel SE', 'color': 'fg-2', 'width': 1}])
    cap = (f"Lever ladder from {start['label']} to {end['label']}: each accepted step's paired net-Sharpe difference "
           f"vs its predecessor on common return sessions ({ctx.primary['short']}), whiskers +- 1 Memmel (2003) SE; "
           f"end bar = the last cell's net Sharpe. Sources: daily_{pid}.csv net_return of each ladder cell; "
           f"nav_summ net_sharpe.")
    return C.figure(ctx.next_fig(), svg, cap, 'fig-ladder', lg)


def blk_scatter(ctx: Ctx) -> str:
    sc = ctx.cfg.get('scatter', {})
    labels = set(sc.get('labels', []))
    pts = []
    for c in ctx.cells:
        x, y, s = ctx.metric(c, 'summ.tau_gmv_mean'), ctx.metric(c, 'summ.cost_bps_traded'), ctx.metric(c, 'summ.net_sharpe')
        pts.append({'x': x, 'y': y, 'size': s, 'color': ctx.gtok(c), 'hollow': ctx.ghollow(c),
                    'label': c.label if c.name in labels else None,
                    'title': f"{c.label} ({c.name}): tau {C.fmt(x, '.4f')}, cost {C.fmt(y, '.2f')} bps/$, "
                             f"net SR {C.fmt(s, '+.3f')}"})
    svg = C.scatter(pts, x_label='Daily turnover tau, mean (GMV units)', y_label='Cost, bps per traded $',
                    x_fmt='.3f', y_fmt='.1f', size_label=f"Net SR {ctx.primary['short']}", size_fmt='+.2f', height=440,
                    aria='turnover vs cost')
    present = []
    for c in ctx.cells:
        if c.group not in present:
            present.append(c.group)
    lg = C.legend([{'name': (ctx.groups.get(g) or {}).get('label', g), 'color': (ctx.groups.get(g) or {}).get('token', 'ref'),
                    'kind': 'hollow' if (ctx.groups.get(g) or {}).get('hollow') else 'dot'} for g in present])
    n_ok = sum(1 for p in pts if C.is_num(p['x']) and C.is_num(p['y']))
    cap = (f"All {len(ctx.cells)} cells ({n_ok} with both values): mean daily one-way GMV turnover vs cost per traded "
           f"dollar, {ctx.primary['label']}; colour = cell group, area = net Sharpe. Source: "
           f"{Path(ctx.cfg.get('inputs', {}).get('nav_summ_json', '')).name} tau_gmv_mean, cost_bps_traded, net_sharpe.")
    return C.figure(ctx.next_fig(), svg, cap, 'fig-scatter', lg)


def blk_turnover(ctx: Ctx) -> str:
    groups = ctx.cfg.get('turnover', {}).get('groups') or []
    pid = ctx.pid()
    rows = []
    for c in ctx.cells:
        if c.group not in groups:
            continue
        d = c.daily(pid)
        rows.append({'label': c.label, 'color': ctx.gtok(c), 'stats': D.distribution(D.tau_sessions(d)) if d else None})
    svg = C.box_plot(rows, value_label='Daily one-way turnover tau (GMV units)', value_fmt='.3f', aria='turnover distribution')
    lg = C.legend([{'name': (ctx.groups.get(g) or {}).get('label', g), 'color': (ctx.groups.get(g) or {}).get('token', 'ref'),
                    'kind': 'bar'} for g in groups]
                  + [{'name': 'Median', 'color': 'fg', 'width': 1.5}, {'name': 'Mean', 'color': 'fg', 'kind': 'hollow'}])
    cap = (f"Distribution of daily one-way GMV turnover for the {len(rows)} cells in groups {', '.join(groups)} "
           f"({ctx.primary['short']}): box p25-p75, whiskers p5-p95, median tick, mean ring; executed sessions with "
           f"positive pre-trade gross, deployment session excluded. Source: daily_{pid}.csv one_way_turnover_gmv.")
    return C.figure(ctx.next_fig(), svg, cap, 'fig-turnover', lg)


# ---------------------------------------------------------------------------------------------- alphas
class Alphas:
    """Library candidates in (theme, roster) order, with lineage, change vs the previous library and roles."""

    def __init__(self, ctx: Ctx):
        ac = ctx.cfg.get('alphas', {})
        self.cfg = ac
        self.lib = ctx.reg.read_json(ac['library']) if ac.get('library') else None
        self.rec = ctx.reg.read_json(ac['recipe']) if ac.get('recipe') else None
        if self.lib is None:
            raise FileNotFoundError(ac.get('library', 'library'))
        rec = self.rec or {}
        self.lin = {r['id']: r for r in rec.get('lineage', [])}
        self.chg = {c['id']: c for c in rec.get(ac.get('changes_key', ''), [])}
        themes = [t['theme'] for t in rec.get('themes', [])]
        for c in self.lib.get('candidates', []):
            if c.get('theme') not in themes:
                themes.append(c.get('theme'))
        self.themes = themes
        self.order = sorted(self.lib.get('candidates', []),
                            key=lambda c: (themes.index(c.get('theme')), (self.lin.get(c['id']) or {}).get('roster_order', 0)))
        self.roles = []
        for r in ac.get('roles', []):
            adm = ctx.reg.read_json(r['admission']) if r.get('admission') else None
            w = ctx.reg.read_json(r['weights']) if r.get('weights') else None
            self.roles.append({'key': r['key'], 'label': r.get('label', r['key']), 'primary': r.get('primary', False),
                               'adm': {c['id']: c for c in (adm or {}).get('candidates', [])},
                               'rules': (adm or {}).get('rules') or {}, 'w': (w or {}).get('weights'),
                               'adm_ok': adm is not None, 'w_ok': w is not None, 'src': r})
        self.primary = next((r for r in self.roles if r['primary']), self.roles[0] if self.roles else None)

    def change(self, cid: str) -> str | None:
        c = self.chg.get(cid)
        if not c:
            return None
        rule = (self.cfg.get('change_labels') or {}).get(c.get('change'))
        if not rule:
            return c.get('change')
        for needle, label in (rule.get('contains') or {}).items():
            if needle in (c.get('detail') or ''):
                return label
        return rule.get('default', c.get('change'))

    @staticmethod
    def status(a: dict | None) -> str | None:
        if not a:
            return None
        s = a.get('status')
        return f"{s} ({a['redundant_with']})" if s == 'reject_redundant' and a.get('redundant_with') else s

    def raw_dir(self, cid: str):
        return (self.lin.get(cid) or {}).get('raw_prior_direction')

    def weight(self, role, cid):
        if not role or role['w'] is None:
            return None
        return role['w'].get(cid, 0.0)


def blk_alpha_t(ctx: Ctx) -> str:
    A = Alphas(ctx)
    role = A.primary
    rows = []
    for c in A.order:
        a = role['adm'].get(c['id']) if role else None
        adm = bool(a) and a.get('status') == 'admitted'
        rd = A.raw_dir(c['id'])
        rows.append({'group': c.get('theme'), 'label': c['id'], 'value': (a or {}).get('hac_t'), 'filled': adm,
                     'color': 'accent' if adm else 'ref-2',
                     'cells': [C.fmt(rd, '+d') if isinstance(rd, int) else C.NA_TEXT, A.status(a) or C.NA_TEXT],
                     'title': f"{c['id']} ({c.get('theme')}): HAC t {C.fmt((a or {}).get('hac_t'), '+.2f')}, "
                              f"{A.status(a) or C.NA_TEXT}, raw prior direction {rd}"})
    veto = (role or {}).get('rules', {}).get('veto_t')
    refs = [{'value': veto, 'label': f'veto t {C.fmt(veto, "+.1f")}'}] if C.is_num(veto) else []
    svg = C.dot_plot(rows, value_label=f"TRAIN HAC t, role {role['label'] if role else C.NA_TEXT}", value_fmt='+.2f',
                     columns=[('Raw dir', 70), ('Status', 250)], ref_lines=refs, label_w=170, aria='candidate HAC t')
    lg = C.legend([{'name': 'Admitted', 'color': 'accent', 'kind': 'dot'},
                   {'name': 'Rejected (redundant, veto, turnover or data)', 'color': 'ref-2', 'kind': 'hollow'},
                   {'name': 'Veto threshold', 'color': 'fg-3', 'dash': '3 3', 'width': 1}])
    src = role['src'].get('admission') if role else C.NA_TEXT
    cap = (f"TRAIN HAC t of each of the {len(rows)} library candidates' standalone neutralized factor on role "
           f"{role['label'] if role else C.NA_TEXT}, grouped by theme; raw literature direction (the prior sign is "
           f"embedded in the DSL) and admission status at left. Sources: {src} (hac_t, status, rules.veto_t); "
           f"{A.cfg.get('recipe', '')} lineage.")
    return C.figure(ctx.next_fig(), svg, cap, 'fig-alpha-t', lg)


def blk_theme_weights(ctx: Ctx) -> str:
    A = Alphas(ctx)
    role = A.primary
    if not role or role['w'] is None:
        raise FileNotFoundError('composition weights')
    theme_of = {c['id']: c.get('theme') for c in A.order}
    mass = {t: 0.0 for t in A.themes}
    count = {t: 0 for t in A.themes}
    for cid, w in role['w'].items():
        t = theme_of.get(cid)
        if t is None or not C.is_num(w):
            continue
        mass[t] = mass.get(t, 0.0) + w
        count[t] = count.get(t, 0) + (1 if w > 0 else 0)
    cats = [t for t in A.themes if t in mass]
    notes = [f"{count[t]} members x {C.fmt(mass[t] / count[t], '.4f')}" if count[t] else '0 members' for t in cats]
    svg = C.bar_chart(cats, [{'name': 'theme weight', 'color': 'accent', 'values': [mass[t] for t in cats]}],
                      horizontal=True, value_fmt='.4f', notes=notes, note_w=190, label_w=190, aria='theme weights')
    cap = (f"Theme weight (sum of member weights) of the composition on role {role['label']}, with admitted member "
           f"count x member weight. Source: {role['src'].get('weights')} weights; theme from {A.cfg.get('library')}.")
    return C.figure(ctx.next_fig(), svg, cap, 'fig-themes')


def blk_alpha_table(ctx: Ctx) -> str:
    A = Alphas(ctx)
    rows = []
    for c in A.order:
        rd = A.raw_dir(c['id'])
        r = {'id': c['id'], 'theme': c.get('theme'), 'tier': c.get('tier'), 'dir': rd, 'chg': A.change(c['id'])}
        for ro in A.roles:
            a = ro['adm'].get(c['id'])
            r[f"st_{ro['key']}"] = A.status(a) if ro['adm_ok'] else None
            r[f"t_{ro['key']}"] = (a or {}).get('hac_t')
            r[f"tau_{ro['key']}"] = (a or {}).get('tau')
            r[f"w_{ro['key']}"] = A.weight(ro, c['id'])
        rows.append(r)
    cols = [{'key': 'id', 'label': 'Id', 'kind': 'mono'}, {'key': 'theme', 'label': 'Theme', 'kind': 'text'},
            {'key': 'tier', 'label': 'Tier', 'kind': 'text'}, {'key': 'dir', 'label': 'Raw dir', 'fmt': '+d'},
            {'key': 'chg', 'label': 'Change vs previous library', 'kind': 'text'}]
    for ro in A.roles:
        ws = _finite(r[f"w_{ro['key']}"] for r in rows)
        cols += [{'key': f"st_{ro['key']}", 'label': f"Status {ro['label']}", 'kind': 'text'},
                 {'key': f"t_{ro['key']}", 'label': f"HAC t {ro['label']}", 'fmt': '+.2f'},
                 {'key': f"tau_{ro['key']}", 'label': f"Tau {ro['label']}", 'fmt': '.4f'},
                 {'key': f"w_{ro['key']}", 'label': f"Weight {ro['label']}", 'fmt': '.4f',
                  'bar': {'lo': 0.0, 'hi': max(ws or [1.0])}}]
    srcs = '; '.join(f"{ro['label']}: {ro['src'].get('admission')}, {ro['src'].get('weights')}" for ro in A.roles)
    cap = (f"Library candidates ({len(rows)}): theme, tier, raw literature direction, change vs the previous library, "
           f"and per role the admission status, TRAIN HAC t, standalone turnover tau and composition weight. "
           f"Sources: {A.cfg.get('library')}; {A.cfg.get('recipe')} (lineage, {A.cfg.get('changes_key')}); {srcs}.")
    return C.table(cols, rows, num=ctx.next_tab(), caption=cap, tid='t-alphas')


def blk_dsl(ctx: Ctx) -> str:
    A = Alphas(ctx)
    role = A.primary
    rows, cur = [], None
    for c in A.order:
        if c.get('theme') != cur:
            cur = c.get('theme')
            rows.append({'_group': cur})
        a = role['adm'].get(c['id']) if role else None
        rows.append({'id': c['id'], 'st': A.status(a), 'w': A.weight(role, c['id']), 'cit': c.get('citation'),
                     'dsl': f'<code class="dsl">{C.esc(c.get("dsl"))}</code>' if c.get('dsl') else None})
    cols = [{'key': 'id', 'label': 'Id', 'kind': 'mono'},
            {'key': 'st', 'label': f"Status {role['label'] if role else ''}", 'kind': 'text'},
            {'key': 'w', 'label': 'Weight', 'fmt': '.4f'},
            {'key': 'cit', 'label': 'Citation', 'kind': 'text', 'cls': 'wrap'},
            {'key': 'dsl', 'label': 'DSL (verbatim)', 'kind': 'html', 'cls': 'dsl'}]
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, cls='dsl-table',
                   caption=(f"Alpha DSL strings verbatim per theme with the citation from the library generator; status "
                            f"and weight on role {role['label'] if role else C.NA_TEXT}. Source: {A.cfg.get('library')} "
                            f"candidates[].dsl, citation."))


def blk_pins(ctx: Ctx) -> str:
    rows = []
    for c in ctx.cells:
        def sha(path):
            v = ctx.metric(c, path)
            return v[:8] if isinstance(v, str) and v else None
        loc = ctx.metric(c, 'sum.locate_in_aim.zeroed_special_short_aims')
        rows.append({'cell': f'{C.esc(c.label)}<span class="sub">{C.esc(c.name)}</span>', 'cell__sort': c.label,
                     'exe': c.exe(ctx.tags), 'recipe': sha('sum.recipe_sha256'),
                     'lib': sha('sum.source_bindings.library_sha256'), 'w': sha('sum.composition_weights_sha256'),
                     'comb': sha('sum.combined_sha256'), 'role': sha('sum.role_sha256'), 'rule': ctx.metric(c, 'sum.rule'),
                     'ob': ctx.metric(c, 'sum.order_basis'), 'loc': loc,
                     'theta': ctx.metric(c, 'scen.construction.v5.theta'), 'dust': ctx.metric(c, 'scen.construction.v5.dust_multiple'),
                     'exit': ctx.metric(c, 'scen.construction.v5.exit_rate'), 'L': ctx.metric(c, 'scen.construction.v5.aim_leverage'),
                     'rate': ctx.metric(c, 'scen.construction.v5.rate'), 'nz': ctx.metric(c, 'scen.construction.neutralize'),
                     '_cls': 'hl' if c is ctx.final else ''})
    nt = 'not recorded in this summary.json'
    cols = [{'key': 'cell', 'label': 'Cell', 'kind': 'html'}, {'key': 'exe', 'label': 'NAV exe', 'kind': 'mono'},
            {'key': 'recipe', 'label': 'Recipe', 'kind': 'mono'}, {'key': 'lib', 'label': 'Library', 'kind': 'mono', 'na_title': nt},
            {'key': 'w', 'label': 'Weights', 'kind': 'mono'}, {'key': 'comb', 'label': 'Combined', 'kind': 'mono'},
            {'key': 'role', 'label': 'Role', 'kind': 'mono'}, {'key': 'rule', 'label': 'Rule', 'kind': 'mono'},
            {'key': 'ob', 'label': 'Order basis', 'kind': 'text', 'na_title': nt},
            {'key': 'loc', 'label': 'Locate-in-aim zeroed', 'fmt': 'int', 'na_title': nt},
            {'key': 'theta', 'label': 'Theta', 'fmt': 'g'}, {'key': 'dust', 'label': 'Dust', 'fmt': 'g'},
            {'key': 'exit', 'label': 'Exit rate', 'fmt': 'g', 'na_title': nt}, {'key': 'L', 'label': 'L', 'fmt': 'g'},
            {'key': 'rate', 'label': 'Rate', 'kind': 'text'}, {'key': 'nz', 'label': 'Neutralize', 'kind': 'text', 'na_title': nt}]
    cap = (f"Recipe pins per cell: NAV executable (runner receipt, tag from config), SHA-256 prefixes of the run "
           f"recipe, library, composition weights, combined signal and role manifest, and the construction flags. "
           f"Sources: summary.json (top level and {ctx.primary['short']} construction), "
           f"<cell>{ctx.cfg.get('receipt_suffix', '')}.")
    return C.table(cols, rows, num=ctx.next_tab(), caption=cap, tid='t-pins')


def blk_checks(ctx: Ctx) -> str:
    f, pid = ctx.final, ctx.pid()
    d = f.daily(pid)
    tol = ctx.cfg.get('check_tolerance')
    out = []

    def add(name, file_v, calc_v, spec):
        ok = None
        if C.is_num(file_v) and C.is_num(calc_v) and C.is_num(tol):
            ok = abs(file_v - calc_v) <= tol
        out.append({'name': name, 'value': C.fmt(file_v, spec), 'threshold': C.fmt(calc_v, spec), 'passed': ok,
                    'chip': 'MATCH' if ok else ('DIFF' if ok is False else None)})

    r = D.returns(d)[1] if d else np.array([])
    add('Net Sharpe (nav_summ vs daily CSV)', ctx.metric(f, 'summ.net_sharpe'), D.sharpe(r, ctx.annual) if r.size else None, '+.6f')
    tau = D.tau_sessions(d) if d else np.array([])
    add('Tau mean (nav_summ vs daily CSV)', ctx.metric(f, 'summ.tau_gmv_mean'), float(tau.mean()) if tau.size else None, '.6f')
    nav = D.nav_path(d)[1] if d else np.array([])
    add('Maximum drawdown (summary vs daily CSV)', ctx.metric(f, 'scen.max_drawdown'),
        float(-D.drawdown(nav).min()) if nav.size else None, '.6f')
    add('Total net return (summary vs daily CSV)', ctx.metric(f, 'scen.total_net_return'),
        float(nav[-1] - 1.0) if nav.size else None, '+.6f')
    yr = D.yearly_returns(d) if d else {}
    for y, v in sorted(ctx.years(f).items()):
        add(f'{y} net return (summary vs daily CSV)', v, yr.get(y), '+.6f')
    ref = ctx.by.get(ctx.cfg.get('reference'))
    p = D.paired(d, ref.daily(pid), ctx.annual) if ref and d is not None else None
    add(f"dSR vs {ref.label if ref else 'reference'} (nav_summ vs daily CSV)", ctx.metric(f, 'summ.paired.dsr'),
        p['dsr'] if p else None, '+.6f')
    add('Memmel SE vs reference (nav_summ vs daily CSV)', ctx.metric(f, 'summ.paired.memmel_se'), p['se'] if p else None, '.6f')
    return C.gate_panel(out, headers=('Statistic', 'File value', 'Recomputed', 'Result'),
                        title=f'Recomputation checks, {f.label}, {ctx.primary["short"]}, tolerance {C.fmt(tol, "g")}')


def blk_trials(ctx: Ctx) -> str:
    tc = ctx.cfg.get('trial_accounting', {})
    rows = []
    for r in tc.get('rows', []):
        src = r.get('source') or {}
        q = D.quote_source(ctx.reg, src['path'], src.get('match', ''), src.get('lines', 1)) if src.get('path') else None
        comp = None
        how = r.get('computed')
        if how:
            kind, _, arg = how.partition(':')
            if kind == 'groups':
                comp = sum(1 for c in ctx.cells if c.group in arg.split(','))
            elif kind == 'nav_summ_rows':
                comp = len(ctx.summ_rows) if isinstance(ctx.summ_rows, list) else None
            elif kind == 'final':
                comp = ctx.metric(ctx.final, arg)
        cnt = r.get('count')
        ok = (comp == cnt) if C.is_num(comp) and C.is_num(cnt) else None
        rows.append({'family': r.get('family'), 'count': cnt, 'detail': r.get('detail'),
                     'comp': comp if how else '', 'ok': ok if how else '',
                     'src': f"{src.get('path')}:{q['line']}" if q else None,
                     'quote': f'<pre class="quote">{C.esc(q["text"])}</pre>' if q else None})
    cols = [{'key': 'family', 'label': 'Family', 'kind': 'text'}, {'key': 'count', 'label': 'Count', 'fmt': 'g'},
            {'key': 'detail', 'label': 'Detail', 'kind': 'text', 'cls': 'wrap'},
            {'key': 'comp', 'label': 'Recomputed', 'fmt': 'g', 'na_title': 'no recomputation rule configured'},
            {'key': 'ok', 'label': 'Match', 'kind': 'chip'},
            {'key': 'src', 'label': 'Source line', 'kind': 'mono'},
            {'key': 'quote', 'label': 'Quoted source', 'kind': 'html', 'cls': 'dsl'}]
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-trials',
                   caption=(f"{tc.get('title', 'Trial accounting')}: counts from the config, each quoted verbatim from "
                            f"its source line; recomputed where a rule is configured (cell groups in the config, "
                            f"nav_summ rows, the final cell's deflated.n)."))


BLOCKS = {'kpi': blk_kpi, 'gates': blk_gates, 't_cells': blk_cells, 'fig_equity': blk_equity,
          'fig_returns': blk_returns, 'fig_rolling': blk_rolling, 'fig_cost_drag': blk_cost_drag, 't_stress': blk_stress,
          'fig_ladder': blk_ladder, 'fig_scatter': blk_scatter, 'fig_turnover': blk_turnover, 'fig_alpha_t': blk_alpha_t,
          'fig_theme_weights': blk_theme_weights, 't_alphas': blk_alpha_table, 't_dsl': blk_dsl, 't_pins': blk_pins,
          'checks': blk_checks, 't_trials': blk_trials}


# ============================================================================================ page
def _header(ctx: Ctx, cfg_path: Path) -> str:
    f = ctx.final
    d = f.daily(ctx.pid()) if f else None
    win = C.NA_TEXT
    if d is not None:
        dates = D.returns(d)[0]
        if dates:
            win = f'{dates[0].isoformat()} to {dates[-1].isoformat()} ({len(dates)} return sessions)'
    tw = ctx.cfg.get('train_window', {})
    gen_head = D.git_head(Path(__file__).resolve().parent)
    root_head = D.git_head(ctx.root)
    meta = [('Window', f"{tw.get('label', C.NA_TEXT)}: {win}"),
            ('Primary scenario', f"{ctx.primary['label']} ({ctx.primary['id']})" if ctx.primary else C.NA_TEXT),
            ('Final cell', f'{f.label}: {f.name}' if f else C.NA_TEXT),
            ('Generated', ctx.stamp), ('Config', f'{cfg_path.name} sha256 {ctx.cfg_sha}'),
            ('Inputs root', f'{ctx.root.as_posix()} @ {root_head or C.NA_TEXT}'),
            ('Generator', f'mega_report @ {gen_head or C.NA_TEXT}')]
    dl = ''.join(f'<div><dt>{C.esc(k)}</dt><dd>{C.esc(v)}</dd></div>' for k, v in meta)
    man = ctx.reg.manifest()
    frows = [{'path': m['path'], 'status': m['status'], 'bytes': m['bytes'], 'sha': m['sha256']} for m in man]
    ftab = C.table([{'key': 'path', 'label': 'Path (relative to inputs root)', 'kind': 'mono'},
                    {'key': 'status', 'label': 'Status', 'kind': 'text'}, {'key': 'bytes', 'label': 'Bytes', 'fmt': 'int'},
                    {'key': 'sha', 'label': 'SHA-256', 'kind': 'mono'}], frows, sortable=True, tid='t-files')
    n_read = sum(1 for m in man if m['status'] == 'read')
    toc = ''.join(f'<a href="#{C.esc(s["id"])}"><span class="n">{i}</span>{C.esc(s["title"])}</a>'
                  for i, s in enumerate(ctx.cfg.get('layout', []), 1))
    return (f'<header class="doc-head"><div class="row"><p class="eyebrow">{C.esc(ctx.cfg.get("kicker", ""))}</p>'
            f'<button type="button" id="theme-btn" class="theme-btn" hidden>Theme</button></div>'
            f'<h1>{C.esc(ctx.cfg.get("title", "Report"))}</h1><dl class="meta">{dl}</dl>'
            f'<details class="files"><summary>Input files: {n_read} read, {len(man) - n_read} not read</summary>{ftab}'
            f'</details></header><nav class="toc" aria-label="Sections">{toc}</nav>')


def build(config_path, *, root: str | None = None, stamp: str | None = None) -> str:
    """Render the report for ``config_path``; ``root`` overrides the config root, ``stamp`` the generated-at text."""
    cfg_path = Path(config_path)
    raw = cfg_path.read_bytes()
    cfg = json.loads(raw.decode('utf-8'))
    root_p = Path(root or cfg['root'])
    if not root_p.is_absolute():
        root_p = (cfg_path.parent / root_p).resolve()
    stamp = stamp or dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')
    ctx = Ctx(cfg, raw, cfg_path, root_p, stamp)
    sections = []
    for i, sec in enumerate(cfg.get('layout', []), 1):
        parts = []
        for b in sec.get('blocks', []):
            fn = BLOCKS.get(b)
            try:
                if fn is None:
                    raise KeyError(f'unknown block {b!r}')
                if ctx.final is None and b not in ('t_cells', 'fig_scatter', 't_pins', 't_trials'):
                    raise KeyError(f"final cell {cfg.get('final')!r} not among the configured cells")
                parts.append(fn(ctx))
            except Exception as e:  # a block never stops the build; it renders its own n/a marker
                parts.append(_na_block(b, e))
                if cfg.get('debug'):
                    traceback.print_exc()
        sections.append(C.section(i, sec['title'], ''.join(parts), sec.get('id')))
    head = _header(ctx, cfg_path)
    return ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">'
            f'<title>{C.esc(cfg.get("title", "Report"))}</title>'
            '<link rel="preconnect" href="https://fonts.googleapis.com">'
            '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>'
            f'<link rel="stylesheet" href="{C.esc(T.FONTS_HREF)}"><style>{T.css()}</style></head>'
            f'<body><div class="page">{head}<main>{"".join(sections)}</main></div>'
            f'<script>{T.SCRIPT}</script></body></html>\n')
