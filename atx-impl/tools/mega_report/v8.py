"""Platform v8 sections of the pitch report (config schema ``atx.mega-report-config/v2``, config key ``v8``).

Pure components ``render_*(data, src, ...) -> str`` render one section from parsed data; ``data`` None renders the
report's one-line unavailable block naming ``src``. Thin blocks (layout ``type`` = the block name, registered in
``pitch.BLOCKS`` as pitch3's are) read their inputs through the report Registry, so the E-4 / E-11 seal check runs on
every path before it is opened and every file is hashed into the manifest, then call the components:

- ``v8_year_table`` <- ``v8.summ`` (nav_summ ``--protocol v8 --dsr-ledger --json`` over the v8 cells): net Sharpe by
  cell and TRAIN year, then each cell's year table (return rows, net Sharpe, return, volatility, turnover, cost per
  traded dollar);
- ``v8_ladder`` <- ``v8.cells[].paired`` (nav_summ ``--bundle PARENT CELL --bundle-json``): every v8 cell with its
  parent, N after it, the paired dSR and its one-sided bootstrap p, mechanics, the mechanical criterion named in its task,
  the pre-registered rule (v8-prereg item 5) and the ledger verdict;
- ``v8_bundle`` <- ``v8.bundle`` (nav_summ ``--bundle B0c V8-F --bundle-json``): the cumulative paired test and the
  freeze gate line items (v8-prereg item 9);
- ``v8_diagnostics`` <- ``v8.diagnostics`` (``diagnostics-v8.json``, schema ``atx.book-diagnostics/v1``): G-1a..G-3d,
  skipped entries with their reason, the G-1a member columns;
- ``v8_member_horizon`` <- ``v8.member_horizon.card_index`` + ``.admission``: ic_theta, f_theta and marginal IC per
  member, report only;
- ``v8_trial_accounting`` <- ``v8.trial_ledger``: the Appendix A block (``backtest_integrity.appendix_a_v8``) with
  "plus 8 re-screens" (ruling R2-e) and the budget;
- ``v8_od1`` <- ``v8.prereg``: the OD-1 window disclosure (v8-prereg item 1) with the research window's dates;
- ``v8_contradictions`` <- ``v8.literature``: the table of contradictions and updates to the v6 / v7 literature.

Each input has exactly one owning block (``inputs``): a missing, refused or malformed input renders one unavailable
block, its owner's, naming the path. A block that borrows an input (the ladder and the freeze gate read ``v8.summ``)
shows n/a with a note instead. Content seal: a year or a session at or after the research seal inside a document is
refused (ValueError), so nothing sealed is rendered even from a file whose name passed the path check.

The ladder refuses, with a visible block of the unavailable class (counted by the CLI), a ``v8.final`` that is not the
last accepted cell of the ladder, a top-level ``final`` (page header, book sections) that is not v8.final's cell, and a
cell whose paired test was read but whose verdict is missing or still pending.

``v8_book`` (layout ``{"type": "v8_book", "block": NAME, ...}``) runs one of the v7 pitch's book-level blocks
(``BOOK_BLOCKS``: equity curve, drawdowns, returns, costs, turnover, capacity curve, exposures, signal correlation) on
the top-level ``final`` cell after checking every file that block reads (``book_inputs``, derived from the config as
the block derives it): a missing, sealed or content-sealed file renders that block's unavailable block naming the path.
"""
from __future__ import annotations

import csv
import datetime as dt
import io
import json
import re

import backtest_integrity as BI  # atx-impl/tools is on sys.path for every mega_report entry
import nav_summ as NS
from engine_tools import research_window as RW

from . import components as C
from . import data as D
from . import pitch3 as P3

BLOCK_YEAR, BLOCK_LADDER, BLOCK_BUNDLE, BLOCK_DIAG = 'v8_year_table', 'v8_ladder', 'v8_bundle', 'v8_diagnostics'
BLOCK_MEMBER, BLOCK_TRIALS, BLOCK_OD1, BLOCK_CONTRA = ('v8_member_horizon', 'v8_trial_accounting', 'v8_od1',
                                                     'v8_contradictions')
BLOCK_BOOK = 'v8_book'
# the v7 pitch's book-level blocks that v8_book runs on the final cell (report.BLOCKS / pitch.BLOCKS names)
BOOK_BLOCKS = ('fig_equity', 't_drawdowns', 'fig_returns', 'fig_rolling', 't_retstats', 't_stress', 't_cost_model',
               't_financing', 'costdec', 't_attrib', 'fig_cost_drag', 'fig_turnover', 'capacity_curve', 'fig_exposure',
               'fig_fills', 'fig_corr', 't_theme_corr')
SCHEMAS = {'diagnostics': ('atx.book-diagnostics/v1',), 'card_index': ('atx.alpha-report-card-index/v1',),
           'admission': ('atx.dsl-admission/v1',)}
DIAG_IDS = ('G-1a', 'G-1b', 'G-1c', 'G-2a', 'G-2b', 'G-2c', 'G-3a', 'G-3b', 'G-3c', 'G-3d')
REPORT_ONLY = 'report only, gates nothing'
RE_SCREENS = 8  # ruling R2-e: the 8 `_f49` re-screens count 0 admission trials; every Appendix A block states them
REGISTERED_BOOTSTRAP = {'block': NS.DEFAULT_BLOCK, 'seed': NS.V8_SEED, 'draws': NS.V8_DRAWS}  # v8-prereg item 4
LITERATURE_HEADING = 'Where the new notes contradict or update v6 and v7'
# the figures each diagnostic's summary row quotes (paths into its result; ``*`` = every key)
HEADLINES = {
    'G-1a': ('weighted_ic_theta', 'negative_at_theta', 'negative_at_theta_weight', 'negative_marginal_t', 'unscored'),
    'G-1b': ('combined_turnover.*', 'fast.ids', 'fast.weight', 'fast.own_share', 'fast.attributed_share.*'),
    'G-1c': ('themes_model.ratio', 'themes_book.ratio', 'members_model.ratio'),
    'G-2a': ('mean_share.*', 'mean_ex_ante_vol_annual'),
    'G-2b': ('weighted_mean_ic.*.*',),
    'G-2c': ('held.p95', 'held.max', 'held.share_above_q', 'aim.p95', 'aim.share_above_q', 'aim.gross_share_above_q'),
    'G-3a': ('net_sharpe', 'restated_net_sharpe', 'delta', 'stress_fee_drag_annual'),
    'G-3b': ('members.*.retained',),
    'G-3c': ('delays.*.delta', 'delays.*.memmel_se'),
    'G-3d': ('adjusted_rand_index', 'effective_bets_themes', 'effective_bets_members', 'mean_rho_within_theme',
             'mean_rho_between_themes'),
}
MAX_LEAVES = 80
MD_LINK = re.compile(r'\[([^\]]+)\]\((https?://[^)\s]+)\)')
MD_BOLD = re.compile(r'\*\*([^*]+)\*\*')


# ============================================================================================ shared helpers
def unavailable(block: str, src: str, reason: str | None = None) -> str:
    """The report's one-line unavailable block (report._na_block's markup), naming the input path."""
    msg = f'{src}: {reason}' if reason else f'{src}: missing'
    return (f'<div class="unavailable" data-block="{C.esc(block)}">{C.esc(block)}: not available '
            f'({C.esc(msg[:320])})</div>')


def refused(block: str, what: str, reason: str) -> str:
    """A visible consistency error, in the unavailable block's markup so the CLI counts it."""
    return (f'<div class="unavailable" data-block="{C.esc(block)}">{C.esc(block)}: refused '
            f'({C.esc(f"{what}: {reason}"[:320])})</div>')


def _note(text: str) -> str:
    return f'<p class="ops-note">{C.esc(text)}</p>' if text else ''


def _base(path) -> str:
    return str(path or '').replace('\\', '/').rstrip('/').split('/')[-1]


def _rel(spec) -> str:
    try:
        return P3._spec(spec)[0]
    except ValueError:
        return str(spec)


def train_years() -> list[int]:
    """The calendar years of TRAIN, from the research window (never a literal)."""
    first = dt.date.fromisoformat(RW.TRAIN_BEGIN_DATE).year
    last = (dt.date.fromisoformat(RW.TRAIN_END_DATE) - dt.timedelta(days=1)).year
    return list(range(first, last + 1))


def refuse_sealed_years(years, what: str) -> None:
    """ValueError when a year at or after the first sealed year of the research window is in ``years``."""
    bad = sorted({int(y) for y in years if C.is_num(y) and int(y) >= RW.FIRST_SEALED_YEAR})
    if bad:
        raise ValueError(RW.seal_message(f'{what}: year {bad[0]}'))


def refuse_sealed_sessions(obj, what: str) -> None:
    """ValueError when a ``session_ns`` value (or list) anywhere in a parsed document is at or after the seal."""
    stack = [obj]
    while stack:
        cur = stack.pop()
        if isinstance(cur, dict):
            for k, v in cur.items():
                if k == 'session_ns':
                    vals = v if isinstance(v, list) else [v]
                    ns = [int(x) for x in vals if C.is_num(x)]
                    if ns and RW.is_sealed(max(ns)):
                        raise ValueError(RW.seal_message(f'{what}: a session'))
                else:
                    stack.append(v)
        elif isinstance(cur, list):
            stack.extend(cur)


def _v8(ctx) -> dict:
    return P3._conf(ctx, 'v8')


def _cells(v8: dict) -> list[dict]:
    cells = v8.get('cells')
    if not isinstance(cells, list) or not cells:
        raise KeyError('config v8.cells not set')
    return cells


def _cell(v8: dict, key) -> dict:
    for c in _cells(v8):
        if c.get('key') == key:
            return c
    raise KeyError(f'v8 cell {key!r} not in v8.cells')


def _short(v, width: int = 90) -> str:
    """One value as short text: numbers to 4 significant digits, lists shortened, dicts by size."""
    if v is None:
        return C.NA_TEXT
    if isinstance(v, bool):
        return 'yes' if v else 'no'
    if C.is_num(v):
        return C.fmt(v, 'd' if isinstance(v, int) else '.4g')
    if isinstance(v, str):
        return v if len(v) <= width else v[:width - 3] + '...'
    if isinstance(v, list):
        head = ', '.join(_short(x, 24) for x in v[:6])
        return f'[{head}' + (f', ... +{len(v) - 6} more]' if len(v) > 6 else ']')
    if isinstance(v, dict):
        return f'{{{len(v)} keys}}'
    return str(v)[:width]


def _expand(obj, path: str, label: str = '') -> list[tuple[str, object]]:
    """(label, value) pairs at ``path`` in ``obj``; ``*`` walks every key (dict) or index (list)."""
    head, _, rest = path.partition('.')
    if head == '*':
        items = list(obj.items()) if isinstance(obj, dict) else list(enumerate(obj)) if isinstance(obj, list) else []
        out = []
        for k, v in items:
            lab = f'{label}.{k}' if label else str(k)
            out += _expand(v, rest, lab) if rest else [(lab, v)]
        return out
    nxt = D.dig(obj, head) if isinstance(obj, (dict, list)) else None
    lab = f'{label}.{head}' if label else head
    if nxt is None:
        return []
    return _expand(nxt, rest, lab) if rest else [(lab, nxt)]


def _leaves(obj, prefix: str = '', out: list | None = None, depth: int = 0) -> list[tuple[str, str]]:
    """Scalar leaves of a nested result as (path, short text); lists of scalars and deep containers are shortened."""
    out = [] if out is None else out
    if isinstance(obj, dict) and depth < 4:
        for k, v in obj.items():
            _leaves(v, f'{prefix}.{k}' if prefix else str(k), out, depth + 1)
    elif isinstance(obj, list) and obj and all(isinstance(x, dict) for x in obj) and depth < 4:
        out.append((prefix, f'{len(obj)} rows'))
    else:
        out.append((prefix, _short(obj)))
    return out


def _md_inline(text: str) -> str:
    """Escaped text with markdown links (http/https only) and bold."""
    s = C.esc(text)
    s = MD_LINK.sub(lambda m: f'<a href="{m.group(2)}" rel="noopener">{m.group(1)}</a>', s)
    return MD_BOLD.sub(lambda m: f'<strong>{m.group(1)}</strong>', s)


# ============================================================================================ year table
YEAR_COLS = [{'key': 'year', 'label': 'Year', 'kind': 'text'},
             {'key': 'return_rows', 'label': 'Return rows', 'fmt': 'int', 'na_title': 'no return rows this year'},
             {'key': 'net_sharpe', 'label': 'Net Sharpe', 'fmt': '+.3f'},
             {'key': 'net_return', 'label': 'Net return, compounded', 'fmt': '+pct2'},
             {'key': 'ann_vol', 'label': 'Volatility, annualised', 'fmt': 'pct2'},
             {'key': 'tau_gmv_mean', 'label': 'Turnover tau, mean', 'fmt': '.4f'},
             {'key': 'cost_bps_traded', 'label': 'Cost bps per traded $', 'fmt': '.2f'}]


def year_rows(data) -> list[dict]:
    """The TRAIN years in order (plus any other year the table holds), each from its nav_summ year-table row; a TRAIN
    year without return rows stays empty (n/a). A sealed year is refused."""
    if not isinstance(data, list) or not all(isinstance(r, dict) and C.is_num(r.get('year')) for r in data):
        raise ValueError('year table: expected a list of {year, ...} rows (nav_summ year_table)')
    refuse_sealed_years([r['year'] for r in data], 'year table')
    by = {int(r['year']): r for r in data}
    return [dict(by.get(y) or {}, year=str(y)) for y in sorted(set(train_years()) | set(by))]


def _window_text() -> str:
    ys = train_years()
    return f'TRAIN {ys[0]}-{ys[-1]}, {RW.WINDOW_ID}'


def render_year_table(data, src: str, *, label: str = '', num: int | None = None, tid: str | None = None) -> str:
    """One cell's year table: per calendar year of its return rows (nav_summ ``year_table``)."""
    if data is None:
        return unavailable(BLOCK_YEAR, src)
    of = f' of {label}' if label else ''
    return C.table(YEAR_COLS, year_rows(data), num=num, tid=tid, sortable=False,
                   caption=(f"Year table{of} ({_window_text()}): per calendar year of the return rows, the return rows, "
                            f"net Sharpe (mean / sd x sqrt 252), compounded net return, annualised volatility, mean daily "
                            f"GMV turnover over the tau_t sessions and cost per traded dollar. A TRAIN year without "
                            f"return rows shows n/a. Source: {src}."))


def render_year_matrix(data, src: str, *, num: int | None = None) -> str:
    """Net Sharpe by cell (rows) and TRAIN year (columns), with the whole-window figure. ``data``: [{key, label, name,
    net_sharpe, year_table (list or None)}]."""
    if data is None:
        return unavailable(BLOCK_YEAR, src)
    years = train_years()
    rows = []
    for e in data:
        yt = year_rows(e['year_table']) if e.get('year_table') is not None else []
        by = {r['year']: r.get('net_sharpe') for r in yt}
        extra = sorted(y for y in by if int(y) not in years)
        years += [int(y) for y in extra if int(y) not in years]
        rows.append({'cell': f'{C.esc(e.get("label") or e.get("key"))}<span class="sub">{C.esc(e.get("name"))}</span>',
                     'all': e.get('net_sharpe'), **{f'y{y}': v for y, v in by.items()}})
    cols = [{'key': 'cell', 'label': 'Cell', 'kind': 'html'}]
    cols += [{'key': f'y{y}', 'label': str(y), 'fmt': '+.3f', 'na_title': 'no year table for this cell'} for y in years]
    cols += [{'key': 'all', 'label': 'TRAIN, whole window', 'fmt': '+.3f'}]
    return C.table(cols, rows, num=num, tid='t-v8-year-matrix', sortable=False,
                   caption=(f"S2 net Sharpe by cell and calendar year ({_window_text()}), each year from the cell's "
                            f"year table, and the whole-window net Sharpe of the same nav_summ row. Source: {src}."))


def an_summ(ctx) -> dict:
    """The v8 nav_summ JSON (``v8.summ``): rows by directory basename; every year table checked against the seal."""
    spec = _v8(ctx).get('summ')
    if not spec:
        raise KeyError('config v8.summ not set')
    rows = P3._load(ctx, spec, 'v8 nav_summ JSON')
    if not isinstance(rows, list) or not all(isinstance(r, dict) for r in rows):
        raise ValueError(f'{_rel(spec)}: expected a list of cell rows (nav_summ --json)')
    for r in rows:
        if r.get('year_table') is not None:
            year_rows(r['year_table'])  # shape and seal
    return {'rows': rows, 'by': {_base(r.get('dir')): r for r in rows}, 'src': P3._src(ctx, _rel(spec)),
            'rel': _rel(spec)}


def _summ(ctx) -> tuple[dict | None, str | None]:
    """(v8_summ analysis, None) or (None, reason) for a block that only borrows ``v8.summ``."""
    sm = ctx.analysis('v8_summ')
    if '_error' in sm:
        return None, f"nav_summ JSON {_rel(_v8(ctx).get('summ'))}: {sm['_error']}"
    return sm, None


def blk_year_table(ctx, spec) -> str:
    v8 = _v8(ctx)
    sm = ctx.analysis('v8_summ')
    if '_error' in sm:
        return unavailable(BLOCK_YEAR, _rel(v8.get('summ')), sm['_error'])
    keys = spec.get('cells')
    cells = [_cell(v8, k) for k in keys] if keys else _cells(v8)
    entries, notes = [], []
    for c in cells:
        r = sm['by'].get(_base(c.get('dir')))
        if r is None:
            notes.append(f"{c.get('key')}: {_base(c.get('dir'))} is not a row of the nav_summ JSON")
        elif r.get('year_table') is None:
            notes.append(f"{c.get('key')}: its nav_summ row has no year_table (run nav_summ with --protocol v8)")
        entries.append({'key': c.get('key'), 'label': c.get('label'), 'name': _base(c.get('dir')),
                        'net_sharpe': (r or {}).get('net_sharpe'), 'year_table': (r or {}).get('year_table')})
    parts = [render_year_matrix(entries, sm['src'], num=ctx.next_tab())]
    det = []
    for e in entries:
        if e['year_table'] is None:
            continue
        tab = render_year_table(e['year_table'], sm['src'], label=e['label'] or e['key'], tid=f"t-v8-year-{e['key']}")
        det.append(f'<details class="card"><summary><code>{C.esc(e["key"])}</code>{C.esc(e["label"] or "")}'
                   f'</summary>{tab}</details>')
    if det:
        parts.append(f'<details class="more"><summary>Year table of each cell</summary>{"".join(det)}</details>')
    parts.append(''.join(_note(n) for n in notes))
    return ''.join(parts)


# ============================================================================================ paired tests
def paired_of(doc, final_name: str | None = None, base_name: str | None = None) -> dict:
    """The paired test of a nav_summ ``--bundle`` document: shape, the two cells' names and the seal checked."""
    if not isinstance(doc, dict) or not isinstance(doc.get('paired'), dict) or not isinstance(doc.get('verdict'), dict):
        raise ValueError('not a nav_summ --bundle document (paired, verdict)')
    for tag, want in (('final', final_name), ('base', base_name)):
        got = _base(doc.get(tag))
        if want and got != want:
            raise ValueError(f'bundle {tag} is {got!r}, expected {want!r}')
    years = [y for y in doc.get('years') or [] if isinstance(y, dict)]
    refuse_sealed_years([y.get('year') for y in years], 'bundle years')
    for tag in ('base', 'final'):
        yt = (doc.get('year_table') or {}).get(tag)
        if yt is not None:
            year_rows(yt)
    p, v = doc['paired'], doc['verdict']
    lw = p.get('lw') or {}
    boot = {'block': p.get('block'), 'seed': p.get('seed'), 'draws': p.get('draws')}
    return {'dsr': p.get('dsr'), 'sr': p.get('sr'), 'sr_ref': p.get('sr_reference'), 'rho': p.get('rho'),
            'sessions': p.get('sessions'), 'se': p.get('memmel_se'), 't': p.get('t'), 'cbb': p.get('cbb_ci95'),
            'lw_ci': lw.get('ci95'), 'p2': lw.get('p_value'), 'p1': lw.get('p_one_sided'), **boot,
            'registered': boot == REGISTERED_BOOTSTRAP, 'alpha': v.get('alpha'), 'pass': v.get('pass'),
            'rule': v.get('rule'), 'protocol': doc.get('protocol'), 'years': years,
            'scenario': (doc.get('scenarios') or {}).get('final'), 'final': doc.get('final'), 'base': doc.get('base')}


def _ci(v) -> str | None:
    if isinstance(v, list) and len(v) == 2 and all(C.is_num(x) for x in v):
        return f'[{C.fmt(v[0], "+.3f")}, {C.fmt(v[1], "+.3f")}]'
    return None


def _cmp(v, op: str, ref) -> bool | None:
    if not (C.is_num(v) and C.is_num(ref)):
        return None
    return {'le': v <= ref, 'lt': v < ref, 'ge': v >= ref, 'gt': v > ref}[op]


def _metric(row, ch: dict):
    v = D.dig(row, ch['metric']) if row else None
    per = D.dig(row, ch['per']) if row and ch.get('per') else None
    if ch.get('per'):
        return v / per if C.is_num(v) and C.is_num(per) and per != 0 else None
    return v


def criterion_eval(crit: dict | None, row: dict | None, prow: dict | None) -> dict:
    """The mechanical criterion of a cell's task: ``checks`` [{metric, per?, op le|lt|ge|gt, factor?}] compare the
    cell's nav_summ row with its parent's x factor; a check without ``metric`` ({text, met}) is a part the report cannot
    compute (a capacity-curve or marginal-t reading): its ``met`` is the PM's reading, None until set. Without checks
    ``met`` is the configured reading of the whole criterion (or None)."""
    crit = crit or {}
    checks = crit.get('checks') or []
    if not checks:
        met = crit.get('met')
        return {'text': crit.get('text'), 'met': met if isinstance(met, bool) else None,
                'detail': 'as configured' if isinstance(met, bool) else None}
    parts, res = [], []
    for ch in checks:
        if 'metric' not in ch:
            met = ch.get('met') if isinstance(ch.get('met'), bool) else None
            res.append(met)
            parts.append(f"{ch.get('text') or 'manual part'}: "
                         + {True: 'met (PM reading)', False: 'not met (PM reading)', None: 'to be read (config met)'}[met])
            continue
        if ch.get('op') not in ('le', 'lt', 'ge', 'gt'):
            raise ValueError(f"mechanical criterion: unknown op {ch.get('op')!r}")
        v, pv = _metric(row, ch), _metric(prow, ch)
        f = ch.get('factor', 1.0)
        ref = pv * f if C.is_num(pv) else None
        res.append(_cmp(v, ch['op'], ref))
        name = ch['metric'] + (f" / {ch['per']}" if ch.get('per') else '')
        parts.append(f"{name} {_short(v)} {ch['op']} parent {_short(pv)}" + (f' x {f:g}' if f != 1.0 else ''))
    return {'text': crit.get('text'), 'met': None if any(r is None for r in res) else all(res),
            'detail': '; '.join(parts)}


def mechanics_eval(row: dict | None, checks: list[dict]) -> dict:
    items = [P3._gate_check(row or {}, ch) for ch in checks]
    res = [g['passed'] for g in items]
    return {'items': items, 'passed': None if not items or any(r is None for r in res) else all(res)}


# ============================================================================================ cell ladder
def _crit_html(r: dict) -> str | None:
    """Chip (when the criterion was evaluated or has checks), text and the evaluation detail."""
    if not r.get('crit_text') and r.get('crit_met') is None:
        return None
    if r.get('crit_met') is not None:
        chip = C.chip(r['crit_met']) + ' '
    else:
        chip = C.na('criterion not evaluated') + ' ' if r.get('crit_detail') else ''
    det = f'<span class="sub">{C.esc(r["crit_detail"])}</span>' if r.get('crit_detail') else ''
    return f'{chip}{C.esc(r.get("crit_text") or "")}{det}'


def render_cell_ladder(data, src: str, *, num: int | None = None) -> str:
    """The cell ladder: ``data`` = {'rows': [...], 'notes': [...], 'rules': verdict badge rules} (``ladder_rows``)."""
    if data is None:
        return unavailable(BLOCK_LADDER, src)
    rows = []
    for r in data['rows']:
        row = dict(r)
        row['cell_html'] = f'{C.esc(r["label"] or r["key"])}<span class="sub">{C.esc(r["name"])}</span>'
        row['crit_html'] = _crit_html(r)
        row['vh'] = C.verdict_html(r.get('verdict'), r.get('verdict_kind'), data.get('rules'))
        rows.append(row)
    span = max([abs(r['dsr']) for r in rows if C.is_num(r.get('dsr'))] or [0.1])
    cols = [{'key': 'k', 'label': '#', 'fmt': 'int'}, {'key': 'cell_html', 'label': 'Cell', 'kind': 'html'},
            {'key': 'parent', 'label': 'Parent', 'kind': 'text'},
            {'key': 'n', 'label': 'N after', 'fmt': 'int', 'na_title': 'not configured'},
            {'key': 'dsr', 'label': 'Paired S2 net dSR vs parent', 'fmt': P3._with_sub('+.3f', 'se_txt'),
             'bar': {'lo': -span, 'hi': span}},
            {'key': 'p1', 'label': 'Bootstrap p, one-sided', 'fmt': '.4f'},
            {'key': 'pos', 'label': 'dSR > 0', 'kind': 'chip'},
            {'key': 'mech', 'label': 'Mechanics', 'kind': 'chip', 'na_title': 'nav_summ row not available'},
            {'key': 'crit_html', 'label': 'Mechanical criterion (task)', 'kind': 'html', 'cls': 'wrap'},
            {'key': 'rule', 'label': 'Rule, v8-prereg item 5', 'kind': 'chip', 'na_title': 'an input of the rule is n/a'},
            {'key': 'vh', 'label': 'Verdict (ledger)', 'kind': 'html', 'na_title': 'no verdict configured'}]
    tab = C.table(cols, rows, num=num, tid='t-v8-ladder', sortable=False,
                  caption=(f"Every v8 cell in order ({_window_text()}, scenario S2): its parent, the trial count N after "
                           f"it, the paired S2 net dSR against the parent (Memmel SE; nav_summ --bundle PARENT CELL, "
                           f"studentized circular block bootstrap as registered), mechanics, the mechanical criterion "
                           f"named in its task (evaluated on the nav_summ rows of cell and parent where the config gives "
                           f"checks) and the pre-registered rule: accepted iff dSR > 0 AND mechanics AND the criterion. "
                           f"Single cells are sign-level evidence (plan 12.2). Sources: {src}."))
    return tab + ''.join(_note(n) for n in data.get('notes') or [])


def ladder_rows(ctx) -> tuple[list[dict], list[str], list[str], list[str]]:
    """(rows, unavailable blocks of missing paired inputs, notes, sources) of the configured v8 cells."""
    v8 = _v8(ctx)
    cells = _cells(v8)
    by_key = {c.get('key'): c for c in cells}
    sm, why = _summ(ctx)
    notes = [f'{why}; mechanics and the metric criteria are n/a'] if why else []
    mech_checks = v8.get('mechanics') or []
    rows, unav, srcs = [], [], [sm['src']] if sm else []

    def row_of(c):
        return sm['by'].get(_base(c.get('dir'))) if sm and c else None
    for i, c in enumerate(cells, 1):
        par = by_key.get(c.get('parent'))
        if c.get('parent') and par is None:
            raise KeyError(f"v8 cell {c.get('key')!r}: parent {c.get('parent')!r} not in v8.cells")
        r, pr = row_of(c), row_of(par)
        if sm and r is None:
            notes.append(f"{c.get('key')}: {_base(c.get('dir'))} is not a row of the nav_summ JSON")
        mech = mechanics_eval(r, mech_checks) if r else {'passed': None}
        row = {'k': i, 'key': c.get('key'), 'label': c.get('label'), 'name': _base(c.get('dir')),
               'parent': (par.get('label') or par.get('key')) if par else 'none (baseline)', 'n': c.get('n'),
               'mech': mech['passed'],
               'verdict': c.get('verdict'), 'verdict_kind': c.get('verdict_kind')}
        if not par:  # a baseline: no paired test, no acceptance rule
            ce = criterion_eval(c.get('criterion'), None, None)
            row.update(dsr='', p1='', pos='', rule='', crit_text=ce['text'], crit_met=ce['met'],
                       crit_detail=ce['detail'])
            rows.append(row)
            continue
        ce = criterion_eval(c.get('criterion'), r, pr)
        row.update(crit_text=ce['text'], crit_met=ce['met'], crit_detail=ce['detail'])
        pd = None
        if c.get('paired'):
            rel = _rel(c['paired'])
            doc, err = P3._try(ctx, c['paired'], f"paired test {c.get('key')}")
            row.update(paired_read=doc is not None, paired_src=rel)
            try:
                pd = paired_of(doc, _base(c.get('dir')), _base(par.get('dir'))) if doc is not None else None
            except ValueError as e:
                err = f'{type(e).__name__}: {e}'
            if pd is None:
                unav.append(unavailable(BLOCK_LADDER, rel, err))
            else:
                srcs.append(P3._src(ctx, rel))
        else:
            notes.append(f"{c.get('key')}: no paired test configured (v8.cells[].paired)")
        dsr = pd.get('dsr') if pd else None
        row.update(dsr=dsr, p1=pd.get('p1') if pd else None, pos=(dsr > 0) if C.is_num(dsr) else None,
                   se_txt=f"SE {C.fmt(pd.get('se'), '.3f')}" if pd and C.is_num(pd.get('se')) else None)
        parts = [row['pos'], row['mech'], row['crit_met']]
        row['rule'] = None if any(x is None for x in parts) else all(parts)
        rows.append(row)
    return rows, unav, notes, srcs


def ladder_checks(ctx, rows: list[dict]) -> list[tuple[str, str]]:
    """(config key, reason) of every consistency error of the ladder: a cell whose paired test was read but whose
    verdict is missing or still pending; ``v8.final`` not the last accepted cell; a top-level ``final`` (page header,
    book sections) that is not v8.final's cell."""
    v8 = _v8(ctx)
    rules = ctx.verdict_rules()
    kinds = {r['key']: r.get('verdict_kind') or C.verdict_kind(r.get('verdict'), rules) for r in rows}
    out = []
    for r in rows:
        k = kinds[r['key']]
        if r.get('paired_read') and k in (None, 'pending'):
            state = 'missing' if k is None else f"still pending ({r.get('verdict')!r})"
            out.append((f"v8.cells[{r['key']}].verdict",
                        f"its paired test {r.get('paired_src')} was read but the verdict is {state}"))
    final = v8.get('final')
    accepted = [r['key'] for r in rows if kinds[r['key']] == 'accepted']
    if not accepted:
        out.append(('v8.final', f'{final!r} is not the last accepted cell: no cell of the ladder is accepted'))
    elif final != accepted[-1]:
        out.append(('v8.final', f'{final!r} is not the last accepted cell of the ladder ({accepted[-1]!r})'))
    fc = next((c for c in _cells(v8) if c.get('key') == final), None)
    top = ctx.cfg.get('final')
    if top is not None:
        if ctx.final is None:
            out.append(('final', f'{top!r} is not among the configured cells'))
        elif fc is not None and _base(ctx.final.rel_dir) != _base(fc.get('dir')):
            out.append(('final', f"{top!r} is {ctx.final.rel_dir}, not the cell of v8.final {final!r} "
                                 f"({fc.get('dir')})"))
    return out


def guard_final_daily(ctx) -> None:
    """The page header quotes the session span of the top-level final cell's primary daily CSV: that file passes the
    content seal first (``input_status``; a sealed file is dropped and the header shows n/a)."""
    if ctx.final is not None and ctx.primary is not None:
        input_status(ctx, f"{ctx.final.rel_dir}/daily_{ctx.primary['id']}.csv")


def blk_ladder(ctx, spec) -> str:
    rows, unav, notes, srcs = ladder_rows(ctx)
    guard_final_daily(ctx)
    errs = ''.join(refused(BLOCK_LADDER, what, why) for what, why in ladder_checks(ctx, rows))
    return errs + ''.join(unav) + render_cell_ladder({'rows': rows, 'notes': notes, 'rules': ctx.verdict_rules()},
                                                     '; '.join(srcs) or C.NA_TEXT, num=ctx.next_tab())


# ============================================================================================ cumulative test, gate
def freeze_gate(row: dict | None, pd: dict, fg: dict, labels: tuple[str, str]) -> dict:
    """The freeze gate's line items (v8-prereg item 9): the configured checks on the final cell's nav_summ row (S2 net,
    mechanics, cell-count DSR) and the two paired items of the cumulative test; ``{"builtin": "paired_dsr"}`` /
    ``{"builtin": "paired_p"}`` place them in the list (else they follow the checks)."""
    alpha = fg.get('alpha', NS.BUNDLE_ALPHA)
    final, base = labels
    builtin = {
        'paired_dsr': {'name': f'Cumulative paired S2 net dSR, {final} vs {base}', 'value': C.fmt(pd.get('dsr'), '+.3f'),
                       'threshold': '> 0', 'passed': (pd['dsr'] > 0) if C.is_num(pd.get('dsr')) else None},
        'paired_p': {'name': 'Studentized circular-block bootstrap p, one-sided', 'value': C.fmt(pd.get('p1'), '.4f'),
                     'threshold': f'< {C.fmt(alpha, "g")}',
                     'passed': (pd['p1'] < alpha) if C.is_num(pd.get('p1')) else None}}
    items, used = [], set()
    for ch in fg.get('checks') or []:
        if ch.get('builtin') in builtin:
            items.append(builtin[ch['builtin']])
            used.add(ch['builtin'])
        else:
            items.append({k: v for k, v in P3._gate_check(row or {}, ch).items() if k != 'metric'})
    items += [v for k, v in builtin.items() if k not in used]
    res = [g['passed'] for g in items]
    return {'items': items, 'freeze': None if any(r is None for r in res) else all(res), 'alpha': alpha}


def an_bundle(ctx) -> dict:
    """The cumulative test (``v8.bundle``) with the freeze gate; the final cell's nav_summ row is borrowed."""
    v8 = _v8(ctx)
    spec = v8.get('bundle')
    if not spec:
        raise KeyError('config v8.bundle not set')
    base, final = _cell(v8, v8.get('base')), _cell(v8, v8.get('final'))
    pd = paired_of(P3._load(ctx, spec, 'v8 cumulative test (nav_summ --bundle)'), _base(final.get('dir')),
                   _base(base.get('dir')))
    fg = v8.get('freeze_gate') or {}
    sm, why = _summ(ctx)
    notes = [f"{why}; the gate's items on the final cell's nav_summ row are n/a"] if why else []
    row = sm['by'].get(_base(final.get('dir'))) if sm else None
    if sm and row is None:
        notes.append(f"{_base(final.get('dir'))} is not a row of the nav_summ JSON; its gate items are n/a")
    labels = (final.get('label') or final.get('key'), base.get('label') or base.get('key'))
    gate = freeze_gate(row, pd, fg, labels)
    if C.is_num(pd.get('alpha')) and pd['alpha'] != gate['alpha']:
        notes.append(f"nav_summ ran with --bundle-alpha {C.fmt(pd['alpha'], 'g')}, the registered alpha is "
                     f"{C.fmt(gate['alpha'], 'g')}; the gate uses the registered one")
    if not pd['registered']:
        notes.append(f"bootstrap block {pd['block']}, seed {pd['seed']}, {pd['draws']} draws is not the registered "
                     f"{REGISTERED_BOOTSTRAP['block']} / {REGISTERED_BOOTSTRAP['seed']} / {REGISTERED_BOOTSTRAP['draws']} "
                     f"(v8-prereg item 4; nav_summ --protocol v8)")
    own = bool(C.is_num(pd.get('dsr')) and pd['dsr'] > 0 and C.is_num(pd.get('p1')) and pd['p1'] < gate['alpha'])
    if isinstance(pd.get('pass'), bool) and pd['pass'] != own:
        notes.append(f"nav_summ's own bundle verdict ({'PASS' if pd['pass'] else 'FAIL'}) differs from the paired "
                     f"items here")
    if gate['freeze'] is False and fg.get('unmet_note'):
        notes.append(fg['unmet_note'])
    return {'paired': pd, 'final_row': row, 'gate': gate['items'], 'freeze': gate['freeze'], 'alpha': gate['alpha'],
            'name': fg.get('name', 'Freeze gate (v8-prereg item 9)'), 'labels': labels, 'notes': notes,
            'src': P3._src(ctx, _rel(spec)), 'summ_src': sm['src'] if sm else None}


def render_bundle_verdict(data, src: str, *, num: int | None = None, num_years: int | None = None) -> str:
    """The freeze gate panel, the cumulative paired statistics and the per-year dSR (``an_bundle``'s result)."""
    if data is None:
        return unavailable(BLOCK_BUNDLE, src)
    pd, (final, base) = data['paired'], data['labels']
    fz = data['freeze']
    gates = list(data['gate']) + [{'name': 'Freeze gate (all of the above)', 'value': 'MET' if fz else
                                   ('UNMET' if fz is False else None), 'threshold': 'all pass', 'passed': fz}]
    parts = [C.gate_panel(gates, title=f"{data['name']}: {final} vs {base}")]
    boot = (f"{C.fmt(pd.get('draws'), 'int')} draws, block {pd.get('block')}, seed {pd.get('seed')} "
            f"({'as registered' if pd['registered'] else 'NOT the registered bootstrap'})")
    stats = [('dSR = SR final - SR base', C.fmt(pd.get('dsr'), '+.3f')),
             (f'SR {final}', C.fmt(pd.get('sr'), '+.3f')), (f'SR {base}', C.fmt(pd.get('sr_ref'), '+.3f')),
             ('Correlation of daily nets rho', C.fmt(pd.get('rho'), '.3f')),
             ('Common return sessions', C.fmt(pd.get('sessions'), 'int')),
             ('Memmel SE (t)', (f"{C.fmt(pd.get('se'), '.3f')} (t {C.fmt(pd.get('t'), '+.2f')})"
                                if C.is_num(pd.get('se')) else None)),
             ('CBB percentile 95%', _ci(pd.get('cbb'))), ('Ledoit-Wolf studentized 95%', _ci(pd.get('lw_ci'))),
             ('Bootstrap p, two-sided', C.fmt(pd.get('p2'), '.4f')),
             ('Bootstrap p, one-sided (H0: dSR <= 0)', C.fmt(pd.get('p1'), '.4f')),
             ('Bootstrap', boot), ('Protocol, scenario', f"{pd.get('protocol')}, {pd.get('scenario')}"),
             ('nav_summ bundle verdict', ('PASS' if pd['pass'] else 'FAIL') if isinstance(pd.get('pass'), bool) else None)]
    parts.append(C.table([{'key': 's', 'label': 'Statistic', 'kind': 'text'},
                          {'key': 'v', 'label': 'Value', 'kind': 'text'}], [{'s': s, 'v': v} for s, v in stats],
                         num=num, tid='t-v8-bundle', sortable=False,
                         caption=(f"Cumulative paired test {final} vs {base} (v8-prereg items 4 and 9; the only test "
                                  f"with power, plan 12.2): paired S2 net dSR on the common return sessions, Memmel SE, "
                                  f"circular block bootstrap CI and the studentized bootstrap p. The gate's S2 net, "
                                  f"mechanics and cell-count DSR items read {final}'s nav_summ row "
                                  f"({data.get('summ_src') or C.NA_TEXT}). Source: {src}.")))
    yrows = [{'y': str(y.get('year')), 'n': y.get('sessions'), 'f': y.get('sr_final'), 'b': y.get('sr_base'),
              'd': y.get('dsr')} for y in pd['years']]
    if yrows:
        parts.append(C.table([{'key': 'y', 'label': 'Year', 'kind': 'text'}, {'key': 'n', 'label': 'Sessions', 'fmt': 'int'},
                              {'key': 'f', 'label': f'SR {final}', 'fmt': '+.3f'},
                              {'key': 'b', 'label': f'SR {base}', 'fmt': '+.3f'},
                              {'key': 'd', 'label': 'dSR', 'fmt': '+.3f'}], yrows, num=num_years,
                             tid='t-v8-bundle-years', sortable=False,
                             caption=f"The cumulative dSR by calendar year of the common sessions. Source: {src}."))
    parts.append(''.join(_note(n) for n in data['notes']))
    return ''.join(parts)


def blk_bundle(ctx, spec) -> str:
    b = ctx.analysis('v8_bundle')
    if '_error' in b:
        return unavailable(BLOCK_BUNDLE, _rel(_v8(ctx).get('bundle')), b['_error'])
    return render_bundle_verdict(b, b['src'], num=ctx.next_tab(), num_years=ctx.next_tab() if b['paired']['years']
                                 else None)


# ============================================================================================ member columns
MEMBER_COLS = [{'key': 'id', 'label': 'Member', 'kind': 'mono'}, {'key': 'theme', 'label': 'Theme', 'kind': 'text'},
               {'key': 'status', 'label': 'Status', 'kind': 'text'}, {'key': 'weight', 'label': 'Weight', 'fmt': '.4f'},
               {'key': 'ic_theta', 'label': 'ic_theta', 'fmt': '+.4f', 'na_title': 'unscored or not in the input'},
               {'key': 'f_theta', 'label': 'f_theta', 'fmt': '+.2e'},
               {'key': 'f_theta_hac_t', 'label': 'f_theta HAC t', 'fmt': '+.2f'},
               {'key': 'marginal_ic21', 'label': 'Marginal IC21', 'fmt': '+.4f'},
               {'key': 'marginal_hac_t', 'label': 'Marginal HAC t', 'fmt': '+.2f'},
               {'key': 'rho_txt', 'label': 'Max |rho| (member)', 'kind': 'text'}]


def render_member_horizon(data, src: str, *, num: int | None = None, block: str = BLOCK_MEMBER,
                          tid: str = 't-v8-member-horizon') -> str:
    """Traded-horizon member columns (task C-2): ``data`` = [{id, theme, status, weight, ic_theta, f_theta,
    f_theta_hac_t, marginal_ic21, marginal_hac_t, max_abs_rho, max_rho_member}], each key optional."""
    if data is None:
        return unavailable(block, src)
    rows = []
    for r in data:
        rho = r.get('max_abs_rho')
        rows.append(dict(r, rho_txt=(f"{C.fmt(rho, '.2f')} ({r.get('max_rho_member') or C.NA_TEXT})"
                                     if C.is_num(rho) else None)))
    cols = [c for c in MEMBER_COLS if c['key'] == 'id' or any(r.get(c['key']) is not None for r in rows)]
    return C.table(cols, rows, num=num, tid=tid, sortable=True,
                   caption=(f"Traded-horizon member columns, {REPORT_ONLY} (v8-prereg rule 8: nothing reads them "
                            f"to admit, weight or select): ic_theta = sum over h 1..63 of theta (1 - theta)^(h-1) x the "
                            f"lagged one-day rank IC at h, theta .05; f_theta = mean daily return of the theta-averaged "
                            f"sleeve book and its Newey-West t; marginal IC21 = the rank IC at h = 21 of the member's "
                            f"residual on the other members (contract K6) with its HAC t and the largest |rho| to another "
                            f"member. Source: {src}."))


def member_rows(index: dict | None, admission: dict | None) -> list[dict]:
    """Rows keyed by member id from a card index (ic_theta, marginal_ic21) and an admission file (f_theta)."""
    rows: dict = {}
    for c in (index or {}).get('candidates') or []:
        if isinstance(c, dict) and c.get('id'):
            rows[c['id']] = {k: c.get(k) for k in ('id', 'theme', 'status', 'ic_theta', 'marginal_ic21')}
    for c in (admission or {}).get('candidates') or []:
        if isinstance(c, dict) and c.get('id'):
            r = rows.setdefault(c['id'], {'id': c['id']})
            r.update({k: c.get(k) for k in ('f_theta', 'f_theta_hac_t')})
            for k in ('theme', 'status'):
                r[k] = r.get(k) or c.get(k)
    if index is not None and not isinstance(index.get('candidates'), list):
        raise ValueError('card index: no candidates list')
    if admission is not None and not isinstance(admission.get('candidates'), list):
        raise ValueError('admission: no candidates list')
    return list(rows.values())


def blk_member_horizon(ctx, spec) -> str:
    mh = _v8(ctx).get('member_horizon') or {}
    if not mh.get('card_index') and not mh.get('admission'):
        raise KeyError('config v8.member_horizon.card_index / .admission not set')
    parts, loaded, srcs = [], {}, []
    for key, what in (('card_index', 'card index'), ('admission', 'admission')):
        if not mh.get(key):
            continue
        doc, err = P3._try(ctx, mh[key], what, schemas=SCHEMAS[key])
        if doc is None:
            parts.append(unavailable(BLOCK_MEMBER, _rel(mh[key]), err))
        else:
            loaded[key] = doc
            srcs.append(P3._src(ctx, _rel(mh[key])))
    if loaded:
        parts.append(render_member_horizon(member_rows(loaded.get('card_index'), loaded.get('admission')),
                                           '; '.join(srcs), num=ctx.next_tab()))
    return ''.join(parts)


# ============================================================================================ diagnostics
def an_diag(ctx) -> dict:
    spec = _v8(ctx).get('diagnostics')
    if not spec:
        raise KeyError('config v8.diagnostics not set')
    doc = P3._load(ctx, spec, 'diagnostics', schemas=SCHEMAS['diagnostics'])
    if not isinstance(doc.get('diagnostics'), dict):
        raise ValueError(f'{_rel(spec)}: no diagnostics object')
    refuse_sealed_sessions(doc, 'diagnostics')
    return {'doc': doc, 'src': P3._src(ctx, _rel(spec))}


def diag_rows(doc: dict) -> list[dict]:
    """One row per diagnostic id (G-1a..G-3d first, any other id after): status, question, headline or reason."""
    d = doc['diagnostics']
    rows = []
    for gid in list(DIAG_IDS) + [k for k in d if k not in DIAG_IDS]:
        e = d.get(gid)
        if not isinstance(e, dict):
            rows.append({'id': gid, 'status': 'not in file', 'question': None, 'text': 'not run (not in the file)'})
            continue
        st = e.get('status')
        if st == 'ok':
            pairs = [p for path in HEADLINES.get(gid, ()) for p in _expand(e.get('result'), path)]
            pairs = pairs or _leaves(e.get('result'))[:4]
            text = '; '.join(f'{k} {_short(v, 60) if not isinstance(v, str) else v}' for k, v in pairs)
        else:
            text = f"skipped: {e.get('reason') or 'no reason given'}"
        rows.append({'id': gid, 'status': st, 'question': e.get('question'), 'text': text[:600], 'entry': e})
    return rows


def _inputs_text(inputs) -> str:
    """A diagnostic's pinned inputs ({name: {path, sha256}}) as 'name path (sha256 prefix)'."""
    if not isinstance(inputs, dict) or not inputs:
        return 'none recorded'
    out = []
    for k, v in inputs.items():
        if isinstance(v, dict):
            sha = f" (sha256 {str(v['sha256'])[:12]})" if v.get('sha256') else ''
            out.append(f"{k} {v.get('path') or C.NA_TEXT}{sha}")
        else:
            out.append(f'{k} {_short(v, 120)}')
    return '; '.join(out)


def _status_html(st) -> str:
    if st == 'ok':
        return C.badge('ok', 'ok')
    if st == 'skipped':
        return C.badge('pending', 'skipped')
    return C.badge('n/a', str(st or 'n/a'))


def render_diagnostics(data, src: str, *, num: int | None = None, num_members: int | None = None) -> str:
    """The G-1..G-3 section from a ``atx.book-diagnostics/v1`` document (``data`` = the parsed JSON)."""
    if data is None:
        return unavailable(BLOCK_DIAG, src)
    rows = diag_rows(data)
    wid = data.get('window_id')
    head = (f"{data.get('declaration') or 'no declaration in the file'}. Window {wid or C.NA_TEXT}"
            + ('' if wid == RW.WINDOW_ID else f' (the research window is {RW.WINDOW_ID}: DIFFERS)')
            + f"; tool {D.dig(data, 'tool.script') or C.NA_TEXT} sha256 {str(D.dig(data, 'tool.script_sha256'))[:12]}.")
    trs = [{'id': r['id'], 'st': _status_html(r['status']), 'q': r['question'], 'text': r['text']} for r in rows]
    parts = [_note(head), C.table([{'key': 'id', 'label': 'Id', 'kind': 'mono'},
                                   {'key': 'st', 'label': 'Status', 'kind': 'html'},
                                   {'key': 'q', 'label': 'Question', 'kind': 'text', 'cls': 'wrap'},
                                   {'key': 'text', 'label': 'Result (headline figures) or reason', 'kind': 'text',
                                    'cls': 'wrap'}], trs, num=num, tid='t-v8-diagnostics', sortable=False,
                                  caption=(f"Zero-trial book diagnostics G-1a..G-3d, descriptive only (v8-prereg rule 8: "
                                           f"none gates, selects or re-weights anything). A skipped entry shows its "
                                           f"reason; the full result of each is below. Source: {src}."))]
    det = []
    for r in rows:
        e = r.get('entry')
        if not e or r['status'] != 'ok':
            continue
        res = e.get('result')
        inner = ''
        if r['id'] == 'G-1a' and isinstance(D.dig(res, 'members'), list):
            inner += render_member_horizon(res['members'], f'{src} diagnostics.G-1a.result.members', num=num_members,
                                           block=BLOCK_DIAG, tid='t-v8-g1a-members')
            res = {k: v for k, v in res.items() if k != 'members'}
        lv = _leaves(res)
        more = f' ({len(lv) - MAX_LEAVES} more values in the file)' if len(lv) > MAX_LEAVES else ''
        inner += C.table([{'key': 'k', 'label': 'Result key', 'kind': 'mono'}, {'key': 'v', 'label': 'Value', 'kind': 'text',
                                                                                 'cls': 'wrap'}],
                         [{'k': k, 'v': v} for k, v in lv[:MAX_LEAVES]], sortable=False,
                         caption=f"{r['id']} result{more}; inputs: {_inputs_text(e.get('inputs'))}.")
        if e.get('method'):
            inner += f'<pre class="quote">{C.esc(str(e["method"]).strip())}</pre>'
        det.append(f'<details class="card"><summary><code>{C.esc(r["id"])}</code>{C.esc(r["question"] or "")}</summary>'
                   f'{inner}</details>')
    if det:
        parts.append(f'<details class="more"><summary>Full results</summary>{"".join(det)}</details>')
    return ''.join(parts)


def blk_diagnostics(ctx, spec) -> str:
    dg = ctx.analysis('v8_diag')
    if '_error' in dg:
        return unavailable(BLOCK_DIAG, _rel(_v8(ctx).get('diagnostics')), dg['_error'])
    has_members = isinstance(D.dig(dg['doc'], 'diagnostics.G-1a.result.members'), list)
    return render_diagnostics(dg['doc'], dg['src'], num=ctx.next_tab(), num_members=None if not has_members else
                              ctx.next_tab())


# ============================================================================================ trial accounting
def an_ledger(ctx) -> dict:
    """The Appendix A block of the trial ledger (``backtest_integrity.appendix_a_v8``, the text every result carries)
    with ruling R2-e's re-screens, N and k against the plan's budget."""
    v8 = _v8(ctx)
    spec = v8.get('trial_ledger')
    if not spec:
        raise KeyError('config v8.trial_ledger not set')
    rel = _rel(spec)
    P3._load(ctx, spec, 'trial ledger', text=True)  # the Registry: seal check before the open, SHA-256, pin
    records = BI.ledger_read(ctx.reg.path(rel))  # one parser: schema of every line and the v8 hash chain
    block = BI.appendix_a_v8(records)
    m = re.search(r'TRAIN construction cells (\d+); admission trials this sprint (\d+)', block)
    if not m:
        raise ValueError('Appendix A block: unexpected text (backtest_integrity.appendix_a_v8)')
    re_screens = v8.get('re_screens', RE_SCREENS)
    if re_screens:
        block = block.replace(m.group(0), f'{m.group(0)} plus {re_screens} re-screens', 1)
    counts = BI.trial_counts(records)
    budget = v8.get('budget') or {}
    return {'block': block, 'n': int(m.group(1)), 'k': int(m.group(2)), 're_screens': re_screens,
            'budget_n': budget.get('n'), 'budget_k': budget.get('admission'), 'lines': len(records),
            'zero_lines': sum(1 for c in counts if c == 0), 'excluded': len(BI.excluded_lines(records)),
            'chained': any('prev_sha256' in r for r in records), 'src': P3._src(ctx, rel)}


def render_trial_accounting(data, src: str) -> str:
    """Appendix A: the counts against the budget and the block itself (``an_ledger``'s result)."""
    if data is None:
        return unavailable(BLOCK_TRIALS, src)
    bn, bk, r = data['budget_n'], data['budget_k'], data['re_screens']
    kpis = [{'label': 'TRAIN construction cells N', 'value': C.fmt(data['n'], 'int'),
             'sub': (f"budget {C.fmt(bn, 'int')}: " + ('within' if data['n'] <= bn else 'EXCEEDED')) if C.is_num(bn)
             else 'no budget configured'},
            {'label': 'Admission trials this sprint', 'value': C.fmt(data['k'], 'int') + (f' + {r}' if r else ''),
             'sub': ((f"budget {C.fmt(bk, 'int')}: " + ('within' if data['k'] <= bk else 'EXCEEDED')) if C.is_num(bk)
                     else 'no budget configured') + (f'; + {r} re-screens at 0 trials (ruling R2-e)' if r else '')},
            {'label': 'Ledger lines', 'value': C.fmt(data['lines'], 'int'),
             'sub': f"{data['zero_lines']} add no trial ({data['excluded']} by the defect rule); hash chain "
                    + ('present' if data['chained'] else 'absent')}]
    return (f'<div class="panel">{C.kpi_strip(kpis)}</div><pre class="quote">{C.esc(data["block"])}</pre>'
            + _note(f"Appendix A block (v8-prereg, on every result): backtest_integrity.appendix_a_v8 over the trial "
                    f"ledger (N by the defect rule; admission trials on this window), with the re-screens of ruling R2-e "
                    f"stated so the reader can count them either way. Source: {src}."))


def blk_trial_accounting(ctx, spec) -> str:
    tl = ctx.analysis('v8_ledger')
    if '_error' in tl:
        return unavailable(BLOCK_TRIALS, _rel(_v8(ctx).get('trial_ledger')), tl['_error'])
    return render_trial_accounting(tl, tl['src'])


# ============================================================================================ OD-1 disclosure
def prereg_item(text: str, number: int) -> str:
    """Item ``number`` of a numbered list (its first line and the indented lines after it), whitespace-joined."""
    lines = text.splitlines()
    for i, ln in enumerate(lines):
        if ln.startswith(f'{number}. '):
            out = [ln]
            for nxt in lines[i + 1:]:
                if not nxt.strip() or not nxt[0].isspace():
                    break
                out.append(nxt)
            return ' '.join(' '.join(out).split())
    raise ValueError(f'pre-registration: item {number} not found')


def window_facts() -> list[str]:
    """The research window's statements (atx-impl/strategies/research_window.json via research_window.py)."""
    doc = RW.load()
    hidden = doc.get('hidden') or {}
    twice, never = hidden.get('read_twice_at_book_level') or [], hidden.get('never_read') or []
    ruling = doc.get('owner_ruling') or {}
    out = [f'Window {RW.WINDOW_ID}: TRAIN [{RW.TRAIN_BEGIN_DATE}, {RW.TRAIN_END_DATE}); sealed from {RW.SEAL_DATE}: '
           f'every tool and this report refuse a session, file or statistic on or after it.']
    if len(twice) == 2:
        out.append(f'Hidden but read twice at book level as validation before v8: [{twice[0]}, {twice[1]}) '
                   f'(not pristine).')
    if never and never[0]:
        out.append(f'Never read: {never[0]} and later.')
    if ruling.get('text'):
        out.append(f"Owner ruling {ruling.get('date') or ''}: {ruling['text']}.")
    return out


def render_od1(data, src: str) -> str:
    """The OD-1 disclosure: ``data`` = {'item': prereg item 1 text, 'facts': [...], 'notes': [...]}."""
    if data is None:
        return unavailable(BLOCK_OD1, src)
    items = [C.esc(x) for x in list(data.get('facts') or []) + list(data.get('notes') or [])]
    return C.callout('OD-1 window disclosure (v8-prereg item 1)', items, kind='caveat',
                     paragraphs_html=[C.esc(data['item']), C.esc(f'Sources: {src} item 1; the research window '
                                                                 f'{RW.WINDOW_ID}.')])


def blk_od1(ctx, spec) -> str:
    v8 = _v8(ctx)
    pre = v8.get('prereg')
    if not pre:
        raise KeyError('config v8.prereg not set')
    text, err = P3._try(ctx, pre, 'pre-registration', text=True)
    if text is None:
        return unavailable(BLOCK_OD1, _rel(pre), err)
    try:
        item = prereg_item(text, 1)
    except ValueError as e:
        return unavailable(BLOCK_OD1, _rel(pre), f'{type(e).__name__}: {e}')
    return render_od1({'item': item, 'facts': window_facts(), 'notes': v8.get('od1_notes') or []},
                      P3._src(ctx, _rel(pre)))


# ============================================================================================ contradictions
def markdown_table_after(text: str, heading: str) -> list[dict]:
    """The first markdown table after the heading line ending with ``heading``: rows {earlier, evidence, verdict}."""
    lines = text.splitlines()
    start = next((i for i, ln in enumerate(lines) if ln.lstrip('#').strip() == heading and ln.startswith('#')), None)
    if start is None:
        raise ValueError(f'heading {heading!r} not found')
    rows, seen = [], False
    for ln in lines[start + 1:]:
        if ln.startswith('#'):
            break
        if ln.startswith('|'):
            seen = True
            cells = [c.strip() for c in ln.strip().strip('|').split('|')]
            if all(set(c) <= set('-: ') for c in cells):
                continue
            rows.append(cells)
        elif seen:
            break
    if len(rows) < 2 or any(len(r) != 3 for r in rows):
        raise ValueError(f'no three-column table under {heading!r}')
    return [{'earlier': a, 'evidence': b, 'verdict': c} for a, b, c in rows[1:]]


def verdict_class(text: str) -> tuple[str, str]:
    """(badge kind, word) of a literature verdict: contradicts / updates / confirms / other."""
    t = (text or '').lower()
    if 'contradict' in t:
        return 'rejected', 'contradicts'
    if t.startswith('updates') or ' updates' in t:
        return 'warn', 'updates'
    if 'confirm' in t:
        return 'accepted', 'confirms'
    return 'n/a', 'other'


def render_contradictions(data, src: str, *, num: int | None = None) -> str:
    """The contradictions and updates to the v6 / v7 literature: ``data`` = [{earlier, evidence, verdict}]."""
    if data is None:
        return unavailable(BLOCK_CONTRA, src)
    rows, counts = [], {}
    for r in data:
        kind, word = verdict_class(r.get('verdict'))
        counts[word] = counts.get(word, 0) + 1
        rows.append({'e': _md_inline(r.get('earlier') or ''), 'n': _md_inline(r.get('evidence') or ''),
                     'v': C.badge(kind, word) + f'<span class="sub">{C.esc(r.get("verdict"))}</span>',
                     'v__sort': word})
    tally = ', '.join(f'{counts[w]} {w}' for w in ('contradicts', 'updates', 'confirms', 'other') if counts.get(w))
    return C.table([{'key': 'e', 'label': 'Earlier position (v6 / v7)', 'kind': 'html', 'cls': 'wrap'},
                    {'key': 'n', 'label': 'New evidence in the v8 notes', 'kind': 'html', 'cls': 'wrap'},
                    {'key': 'v', 'label': 'Verdict', 'kind': 'html'}], rows, num=num, tid='t-v8-contradictions',
                   sortable=True,
                   caption=(f"Where the v8 literature notes contradict or update v6 and v7 ({len(rows)} rows: {tally}). "
                            f"Contradicts = the earlier recommendation is withdrawn; updates = its number or scope "
                            f"changes; confirms = new evidence strengthens it. Literature figures, not measurements on "
                            f"the book. Source: {src}."))


def blk_contradictions(ctx, spec) -> str:
    lit = _v8(ctx).get('literature')
    if not lit:
        raise KeyError('config v8.literature not set')
    heading = lit.get('heading', LITERATURE_HEADING) if isinstance(lit, dict) else LITERATURE_HEADING
    text, err = P3._try(ctx, lit, 'literature report', text=True)
    if text is None:
        return unavailable(BLOCK_CONTRA, _rel(lit), err)
    try:
        rows = markdown_table_after(text, heading)
    except ValueError as e:
        return unavailable(BLOCK_CONTRA, _rel(lit), f'{type(e).__name__}: {e}')
    return render_contradictions(rows, f'{P3._src(ctx, _rel(lit))}, section "{heading}"', num=ctx.next_tab())


# ============================================================================================ book-level blocks
def _book_cell(ctx, name: str):
    c = ctx.by.get(name)
    if c is None:
        raise KeyError(f'cell {name!r} not among the configured cells')
    return c


def _sid(ctx, key: str) -> str:
    sc = ctx.sc.get(key)
    if sc is None:
        raise KeyError(f'scenario {key!r} not configured')
    return sc['id']


def book_inputs(ctx, spec) -> list[str]:
    """Every file the book-level block ``spec['block']`` reads (a directory ends in '/'), derived from the config as
    the block derives it, for the top-level ``final`` cell; KeyError on a config the block cannot run on."""
    name = spec.get('block')
    f = ctx.final
    if f is None:
        raise KeyError(f"final cell {ctx.cfg.get('final')!r} not among the configured cells")
    if ctx.primary is None:
        raise KeyError(f'primary scenario {ctx.pkey!r} not configured')
    pk, pid = ctx.pkey, ctx.primary['id']
    ac = ctx.cfg.get('analysis') or {}
    scen_ids = [s['id'] for s in ctx.scen]

    def daily(c, sid):
        return f'{c.rel_dir}/daily_{sid}.csv'

    def summary(c):
        return f'{c.rel_dir}/summary.json'

    def an(key):
        if not ac.get(key):
            raise KeyError(f'config analysis.{key} not set')
        return str(ac[key]).rstrip('/')

    def alphas():  # report.Alphas reads the library, the recipe and every role's admission and weights
        al = ctx.cfg.get('alphas') or {}
        if not al.get('library'):
            raise KeyError('config alphas.library not set')
        out = [al['library']] + ([al['recipe']] if al.get('recipe') else [])
        for r in al.get('roles') or []:
            out += [r[k] for k in ('admission', 'weights') if r.get(k)]
        return out

    def ic():
        w = [f"{str(ac['w_pass']).rstrip('/')}/train_daily_ic.csv"] if ac.get('w_pass') else []
        return [f"{an('u_pass')}/train_daily_ic.csv", *w, *alphas()]

    def signal():
        out = [f"{an('candidate_cache')}/", f"{an('role')}/manifest.json", f"{an('role')}/member.u8", summary(f)]
        out += [f"{str(ac['u_pass']).rstrip('/')}/summary.json"] if ac.get('u_pass') else []
        out += [ac['fields_manifest']] if ac.get('fields_manifest') else []
        return out + alphas()
    if name == 'fig_equity':
        ec = ctx.cfg.get('equity') or {}
        out = [daily(f, _sid(ctx, k)) for k in ec.get('scenarios') or [s['key'] for s in ctx.scen]]
        out += [daily(_book_cell(ctx, ex['cell']), _sid(ctx, ex.get('scenario', pk))) for ex in ec.get('extra') or []]
    elif name in ('t_drawdowns', 'fig_exposure', 'fig_fills'):
        out = [daily(f, pid)]
    elif name == 'fig_returns':
        out = [daily(f, pid), summary(f)]
    elif name == 'fig_rolling':
        rc = ctx.cfg.get('rolling') or {}
        if not rc.get('window'):
            raise KeyError('config rolling.window not set')
        sid = _sid(ctx, rc.get('scenario', pk))
        out = [daily(_book_cell(ctx, it['cell']), sid) for it in rc.get('cells') or []]
    elif name == 't_retstats':
        out = [daily(f, pid)] + ([daily(_book_cell(ctx, ac['compare_cell']), pid)] if ac.get('compare_cell') else [])
    elif name == 't_stress':
        out = [summary(f)] + [p for sid in scen_ids for p in (daily(f, sid), f'{f.rel_dir}/events_{sid}.csv')]
    elif name == 't_cost_model':
        out = [f'{f.rel_dir}/recipe.json']
    elif name == 't_financing':
        out = [summary(f)]
    elif name in ('costdec', 't_attrib', 'fig_cost_drag'):
        out = [daily(f, sid) for sid in scen_ids]
    elif name == 'fig_turnover':
        groups = (ctx.cfg.get('turnover') or {}).get('groups') or []
        out = [daily(c, pid) for c in ctx.cells if c.group in groups]
    elif name == 'capacity_curve':
        cc = P3._conf(ctx, 'capacity_curve')
        d = str(cc.get('dir', '')).rstrip('/')
        out = [_rel(cc.get('extras') or f'{d}/v7_extras.json'), _rel(cc.get('summary') or f'{d}/summary.json')]
        out += [f"{d}/daily_{b['scenario']}.csv" for b in [cc.get('primary'), *(cc.get('beside') or [])]
                if isinstance(b, dict) and b.get('scenario')]
    elif name == 'fig_corr':
        kind = spec.get('kind', 'ic')
        src = spec.get('source', 'sig_corr') if kind == 'theme' else ('sig_corr' if kind == 'signal' else 'ic_corr')
        out = signal() if src == 'sig_corr' else ic()
    elif name == 't_theme_corr':
        out = ic() + signal()
    else:
        raise KeyError(f"v8_book: block {name!r} is not one of {', '.join(BOOK_BLOCKS)}")
    return list(dict.fromkeys(out))


def _csv_last_session(text: str):
    """The largest ``session_ns`` of a CSV with that column (None without it)."""
    rows = csv.reader(io.StringIO(text))
    header = next(rows, None) or []
    if 'session_ns' not in header:
        return None
    i, best = header.index('session_ns'), None
    for r in rows:
        try:
            v = int(r[i])
        except (ValueError, IndexError):
            continue
        best = v if best is None or v > best else best
    return best


def _calendar_years(doc) -> list:
    out, stack = [], [doc]
    while stack:
        cur = stack.pop()
        if isinstance(cur, dict):
            for k, v in cur.items():
                if k == 'calendar_year_returns' and isinstance(v, list):
                    out += [y.get('year') for y in v if isinstance(y, dict)]
                else:
                    stack.append(v)
        elif isinstance(cur, list):
            stack.extend(cur)
    return out


def _content_seal(rel: str, data: bytes) -> None:
    """ValueError when a CSV has a session, or a JSON document a session or a calendar year, at or after the seal."""
    if rel.endswith('.csv'):
        last = _csv_last_session(data.decode('utf-8', errors='replace'))
        if last is not None and RW.is_sealed(last):
            raise ValueError(RW.seal_message(f'{rel}: a session'))
    elif rel.endswith('.json'):
        doc = json.loads(data.decode('utf-8', errors='replace'))
        refuse_sealed_sessions(doc, rel)
        refuse_sealed_years(_calendar_years(doc), rel)


def _input_status(ctx, rel: str) -> str | None:
    if rel.endswith('/'):
        d = rel.rstrip('/')
        if ctx.reg.sealed(d):
            return 'refused (sealed)'
        return None if ctx.reg.path(d).is_dir() else 'missing (directory)'
    data = ctx.reg.read_bytes(rel)
    if data is None:
        return (ctx.reg.files.get(P3._key(ctx, rel)) or {}).get('status', 'missing')
    try:
        _content_seal(rel, data)
    except json.JSONDecodeError:
        ctx.reg.files[P3._key(ctx, rel)]['status'] = 'unparseable json'
        return 'unparseable json'
    except ValueError as e:
        for c in ctx.cells:  # a daily CSV the header or a block would parse later is dropped
            pre = f'{c.rel_dir}/daily_'
            if rel.startswith(pre) and rel.endswith('.csv'):
                sid = rel[len(pre):-4]
                c._daily[sid] = None
                ctx._full[(c.name, sid)] = None
        return f'refused (sealed content): {e}'
    return None


def input_status(ctx, rel: str) -> str | None:
    """None when ``rel`` passed the path seal, exists and passes the content seal; else the reason (cached per build).
    Files are read through the Registry (hashed into the manifest); a directory is only seal-checked and stat-ed."""
    cache = ctx.analyses.setdefault('_v8_inputs', {})
    if rel not in cache:
        cache[rel] = _input_status(ctx, rel)
    return cache[rel]


def blk_book(ctx, spec) -> str:
    """A v7 book-level block on the final cell, after checking every file it reads (``book_inputs``): the first input
    not available is named (others counted), as the v8 blocks name theirs."""
    name = spec.get('block')
    if name not in BOOK_BLOCKS:
        raise KeyError(f"v8_book: block {name!r} is not one of {', '.join(BOOK_BLOCKS)}")
    try:
        need = book_inputs(ctx, spec)
    except (KeyError, ValueError) as e:
        return unavailable(name, 'config', f'{type(e).__name__}: {e}')
    bad = [(rel, why) for rel in need for why in [input_status(ctx, rel)] if why]
    if bad:
        rel, why = bad[0]
        more = [r for r, _ in bad[1:]]
        if more:
            why += (f"; also not available: {', '.join(more[:3])}"
                    + (f' and {len(more) - 3} more' if len(more) > 3 else ''))
        return unavailable(name, rel, why)
    from . import pitch as P  # imported here: pitch imports this module
    from . import report as R
    inner = {k: v for k, v in spec.items() if k != 'block'}
    inner['type'] = name
    try:
        return P.BLOCKS[name](ctx, inner) if name in P.BLOCKS else R.BLOCKS[name](ctx)
    except Exception as e:  # noqa: BLE001 - as report.build: a block never stops the build
        return R._na_block(name, e)


# ============================================================================================ inputs, registration
def inputs(cfg: dict) -> list[tuple[str, str, str]]:
    """(owner block, config key, path) of every v8 input the config names: the list the PM fills, and the one input
    per owner that a missing file marks unavailable."""
    v8 = cfg.get('v8') or {}
    out = []

    def add(block, key, spec):
        if spec:
            out.append((block, key, _rel(spec)))
    add(BLOCK_YEAR, 'v8.summ', v8.get('summ'))
    for c in v8.get('cells') or []:
        add(BLOCK_LADDER, f"v8.cells[{c.get('key')}].paired", c.get('paired'))
    add(BLOCK_BUNDLE, 'v8.bundle', v8.get('bundle'))
    add(BLOCK_DIAG, 'v8.diagnostics', v8.get('diagnostics'))
    mh = v8.get('member_horizon') or {}
    add(BLOCK_MEMBER, 'v8.member_horizon.card_index', mh.get('card_index'))
    add(BLOCK_MEMBER, 'v8.member_horizon.admission', mh.get('admission'))
    add(BLOCK_TRIALS, 'v8.trial_ledger', v8.get('trial_ledger'))
    add(BLOCK_OD1, 'v8.prereg', v8.get('prereg'))
    add(BLOCK_CONTRA, 'v8.literature', v8.get('literature'))
    return out


ANALYSES = {'v8_summ': an_summ, 'v8_bundle': an_bundle, 'v8_diag': an_diag, 'v8_ledger': an_ledger}
BLOCKS = {BLOCK_YEAR: blk_year_table, BLOCK_LADDER: blk_ladder, BLOCK_BUNDLE: blk_bundle, BLOCK_DIAG: blk_diagnostics,
          BLOCK_MEMBER: blk_member_horizon, BLOCK_TRIALS: blk_trial_accounting, BLOCK_OD1: blk_od1,
          BLOCK_CONTRA: blk_contradictions, BLOCK_BOOK: blk_book}
