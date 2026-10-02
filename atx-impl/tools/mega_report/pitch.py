"""Blocks and analyses of the pitch report (config schema ``atx.mega-report-config/v2``).

Every block is ``fn(ctx, spec)`` where ``spec`` is the layout entry (``{"type": name, ...}``); every analysis is
``fn(ctx) -> dict``, computed once per build (``analysis``) from files the config names (read through the report
Registry, so each is hashed and listed). A failed analysis is cached as ``{'_error': ...}``; a block that needs it
raises and renders the report's one-line "not available" marker. Captions state the source of every number.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import math
import re
from pathlib import Path

import numpy as np

from . import analysis as A
from . import components as C
from . import data as D
from . import narrative as N

FILES_MARKER = '<!--mega-report:files-table-->'
CLASS_TOKENS = {'fundamental': 's2', 'price': 's1', 'positioning': 'flat'}


# ============================================================================================ helpers
def _acfg(ctx) -> dict:
    return ctx.cfg.get('analysis') or {}


def analysis(ctx, name: str):
    if name not in ctx.analyses:
        fn = ANALYSES.get(name)
        try:
            if fn is None:
                raise KeyError(f'unknown analysis {name!r}')
            ctx.analyses[name] = fn(ctx)
        except Exception as e:  # noqa: BLE001 - recorded, rendered as n/a by the caller
            ctx.analyses[name] = {'_error': f'{type(e).__name__}: {str(e)[:200]}'}
            if ctx.cfg.get('debug'):
                import traceback
                traceback.print_exc()
    return ctx.analyses[name]


def need(ctx, name: str) -> dict:
    res = analysis(ctx, name)
    if isinstance(res, dict) and '_error' in res:
        raise RuntimeError(f'analysis {name}: {res["_error"]}')
    return res


def alphas(ctx):
    if '_alphas' not in ctx.analyses:
        from .report import Alphas
        ctx.analyses['_alphas'] = Alphas(ctx)
    return ctx.analyses['_alphas']


def theme_class(ctx, theme: str) -> str:
    return (_acfg(ctx).get('theme_classes') or {}).get(theme, 'fundamental')


def combined_label(ctx) -> str:
    """The composition rule of the combined signal the IC figures name (``analysis.combined_label``; the v7 default)."""
    return str(_acfg(ctx).get('combined_label') or 'ew-theme-v1')


def _dates_ns(ns) -> list[dt.date]:
    return [D.ns_to_date(v) for v in ns]


def _cell(ctx, name: str):
    c = ctx.by.get(name)
    if c is None:
        raise KeyError(f'cell {name!r} not configured')
    return c


def _fmean(xs):
    v = [x for x in xs if C.is_num(x)]
    return float(np.mean(v)) if v else None


def _txt(ctx, s) -> str:
    return N.fill_text(ctx, s) if s else ''


# ============================================================================================ analyses
def an_ic(ctx) -> dict:
    t = ctx.reg.read_text(f"{_acfg(ctx)['u_pass']}/train_daily_ic.csv")
    if t is None:
        raise FileNotFoundError('u pass train_daily_ic.csv')
    out = {'ic': A.load_daily_ic(t)}
    w = _acfg(ctx).get('w_pass')
    tw = ctx.reg.read_text(f'{w}/train_daily_ic.csv') if w else None
    out['ic_w'] = A.load_daily_ic(tw) if tw else {}
    return out


def an_cands(ctx) -> dict:
    t = ctx.reg.read_text(f"{_acfg(ctx)['u_pass']}/train_candidates.jsonl")
    if t is None:
        raise FileNotFoundError('train_candidates.jsonl')
    return A.load_candidates(t)


def _ids(ctx):
    al = alphas(ctx)
    ids = [c['id'] for c in al.order]
    theme = {c['id']: c.get('theme') for c in al.order}
    adm = al.primary['adm'] if al.primary else {}
    admitted = [i for i in ids if (adm.get(i) or {}).get('status') == 'admitted']
    return ids, theme, adm, admitted


def an_ic_corr(ctx) -> dict:
    ic = need(ctx, 'ic')['ic']
    ids, theme, _, admitted = _ids(ctx)
    h = int(_acfg(ctx).get('ic_corr_horizon', 5))
    res = A.ic_matrix(ic, ids, h)
    res.update(A.offdiag_stats(res['r'], ids))
    sub = [ids.index(i) for i in admitted]
    res['admitted'] = A.offdiag_stats(res['r'][np.ix_(sub, sub)], admitted)
    res['horizon'] = h
    res['groups'] = [theme[i] for i in ids]
    return res


SIGNAL_SCHEMAS = ('atx.dsl-candidate-signal/v1', 'atx.dsl-candidate-signal/v2')
_HEX64 = re.compile(r'[0-9a-f]{64}')


def _signal_meta(ctx, sidecar: Path, role_sha) -> dict | None:
    """A candidate-signal sidecar (v1 or v2) of the role, or None (unreadable, another schema or role)."""
    m = ctx.reg.read_json(sidecar)
    if (not isinstance(m, dict) or m.get('schema') not in SIGNAL_SCHEMAS or not isinstance(m.get('candidate_id'), str)
            or not isinstance(m.get('payload'), str) or m.get('role_manifest_sha256') != role_sha):
        return None
    return m


def _library_dsl(ctx) -> dict:
    """id -> the IC runner's ``dsl_sha256`` of the report library's candidate: SHA-256 of ``candidates[].dsl`` (UTF-8),
    recomputed from the library JSON. A candidate without a DSL string has none (no entry can match it)."""
    out = {}
    for c in (alphas(ctx).lib or {}).get('candidates', []):
        if isinstance(c, dict) and isinstance(c.get('id'), str) and isinstance(c.get('dsl'), str):
            out[c['id']] = hashlib.sha256(c['dsl'].encode('utf-8')).hexdigest()
    return out


def _pinned_field_payloads(ctx) -> dict | None:
    """field name -> payload SHA-256 of the fields manifest the config names (``analysis.fields_manifest``); None when
    not configured or unreadable (then field payload versions are not checked, and several are ambiguous)."""
    rel = _acfg(ctx).get('fields_manifest')
    fm = ctx.reg.read_json(rel) if rel else None
    entries = fm.get('fields') if isinstance(fm, dict) else None
    if not isinstance(entries, list):
        return None
    return {e['name']: e.get('sha256') for e in entries if isinstance(e, dict) and isinstance(e.get('name'), str)}


def _same_signal(m: dict, dsl: dict, fields: dict | None) -> bool:
    """The entry is the report library's signal: its ``dsl_sha256`` is the library's for its id and, when a fields
    manifest is pinned, every field payload SHA-256 it records (v2) is that manifest's."""
    if m.get('dsl_sha256') != dsl.get(m['candidate_id']):
        return False
    fp = m.get('field_payload_sha256')
    if fields is None or not fp:
        return True
    return isinstance(fp, dict) and all(fields.get(k) == v for k, v in fp.items())


def _summary_signal_entries(ctx, ids, role_sha, dsl: dict, fields: dict | None) -> dict:
    """id -> (payload, meta) named by the TRAIN IC run's ``candidate_cache.entries[]`` (a v2 runner's summary.json:
    the exact entry, v2 ``<id>.<dsl16>`` or a v1 entry read in place) that is the report library's signal
    (``_same_signal``). Empty when absent or unusable."""
    u = _acfg(ctx).get('u_pass')
    s = ctx.reg.read_json(f'{u}/summary.json') if u else None
    roles = s.get('roles') if isinstance(s, dict) else None
    out = {}
    for r in roles if isinstance(roles, list) else []:
        cache = r.get('candidate_cache') if isinstance(r, dict) and r.get('manifest_sha256') == role_sha else None
        entries = cache.get('entries') if isinstance(cache, dict) else None
        for e in entries if isinstance(entries, list) else []:
            if not (isinstance(e, dict) and e.get('id') in ids and isinstance(e.get('sidecar'), str)
                    and isinstance(e.get('payload'), str)):
                continue
            # runner paths may carry Windows separators and are relative to its cwd (the report root)
            sidecar, payload = (ctx.reg.path(e[k].replace('\\', '/')) for k in ('sidecar', 'payload'))
            m = _signal_meta(ctx, sidecar, role_sha)
            if (m is not None and m['candidate_id'] == e['id'] and m.get('payload_sha256') == e.get('payload_sha256')
                    and payload.name == m['payload'] and not ctx.reg.sealed(payload) and payload.is_file()
                    and _same_signal(m, dsl, fields)):
                out[e['id']] = (payload, m)
    return out


def _signal_dirs(root: Path) -> list[Path]:
    """Entry directories under a --candidate-cache DIR: [<vm identity>/]<sha>/ and their v2 fp_<fk16>/ children."""
    def subdirs(d):
        try:
            return sorted(p for p in d.iterdir() if p.is_dir())
        except OSError:
            return []
    tops = [d for d in subdirs(root) if _HEX64.fullmatch(d.name)]
    tops += [d for v in subdirs(root) if not _HEX64.fullmatch(v.name) for d in subdirs(v) if _HEX64.fullmatch(d.name)]
    return [x for d in tops for x in [d, *(f for f in subdirs(d) if f.name.startswith('fp_'))]]


def _scanned_signal_entries(ctx, root: Path, ids, role_sha, lib_sha, dsl: dict,
                            fields: dict | None) -> tuple[dict, list]:
    """id -> (payload, meta) by walking the cache DIR (v1 ``<id>.json``, v2 ``<id>.<dsl16>.json``). Every entry must be
    the report library's signal (``_same_signal``: the library's DSL SHA-256 for the id, recomputed from the library
    JSON, and the pinned field payloads when configured). The recording library proves nothing for a v2 entry (shared
    by content key, it names only its first writer); a v1 entry must also be recorded by the report's library. An id
    left with several entries (another VM identity or field payload version) is ambiguous and skipped, not guessed."""
    found = {}
    for d in _signal_dirs(root):
        for j in sorted(d.glob('*.json')):
            m = _signal_meta(ctx, j, role_sha)
            if m is None or m['candidate_id'] not in ids or not _same_signal(m, dsl, fields):
                continue
            if lib_sha and m['schema'] == SIGNAL_SCHEMAS[0] and m.get('library_sha256') != lib_sha:
                continue
            found.setdefault(m['candidate_id'], []).append((d / m['payload'], m))
    metas = {i: hits[0] for i, hits in found.items() if len(hits) == 1}
    return metas, sorted(i for i, hits in found.items() if len(hits) > 1)


def an_sig_corr(ctx) -> dict:
    ac = _acfg(ctx)
    root = ctx.reg.path(ac['candidate_cache'])
    lib_sha = ctx.metric(ctx.final, 'sum.source_bindings.library_sha256')
    role_sha = ctx.metric(ctx.final, 'sum.role_sha256')
    role = ctx.reg.read_json(f"{ac['role']}/manifest.json")
    if role is None:
        raise FileNotFoundError('role manifest')
    nd, ni = int(role['dates']), int(role['instruments'])
    ids, theme, _, admitted = _ids(ctx)
    dsl, fields = _library_dsl(ctx), _pinned_field_payloads(ctx)
    # v2 runner: the summary names each entry; else (v1 runner, or entries gone) walk the cache for the rest
    metas, source, ambiguous = _summary_signal_entries(ctx, set(ids), role_sha, dsl, fields), 'summary entries', []
    if any(i not in metas for i in ids):
        scanned, ambiguous = _scanned_signal_entries(ctx, root, set(ids) - set(metas), role_sha, lib_sha, dsl, fields)
        source = 'summary entries + cache scan' if metas else 'cache scan'
        metas.update(scanned)
    missing = [i for i in ids if i not in metas]
    if missing:
        raise FileNotFoundError(f'cache payloads missing for {missing[:5]}'
                                + (f' (ambiguous: {ambiguous[:5]})' if ambiguous else ''))
    mms = []
    for i in ids:
        p, m = metas[i]
        if ctx.reg.sealed(p):  # mapped, not read: the payload passes the Registry's seal check before any access
            raise PermissionError(f'cache payload {ctx.reg.rel(p)}: refused (sealed)')
        if m.get('dates') != nd or m.get('instruments') != ni or p.stat().st_size != nd * ni * 8:
            raise ValueError(f'payload geometry mismatch for {i}')
        ctx.reg.files[ctx.reg.rel(p)] = {'status': 'memmap (sha256 = cache meta payload_sha256, not re-hashed)',
                                         'bytes': p.stat().st_size, 'sha256': m.get('payload_sha256')}
        mms.append(np.memmap(p, dtype='<f8', mode='r', shape=(nd, ni)))
    mb = ctx.reg.read_bytes(f"{ac['role']}/member.u8")
    member = np.frombuffer(mb, dtype=np.uint8).reshape(nd, ni)
    stride = int(ac.get('sig_corr_stride', 1))
    rows = list(range(int(role['score_begin']), int(role['score_end']), stride))
    res = A.signal_corr(lambda k, d: mms[k][d], ids, rows, lambda d: member[d] == 1)
    res.update(A.offdiag_stats(res['r'], ids))
    sub = [ids.index(i) for i in admitted]
    res['admitted'] = A.offdiag_stats(res['r'][np.ix_(sub, sub)], admitted)
    res['groups'] = [theme[i] for i in ids]
    res['stride'] = stride
    res['source'] = source
    res['match'] = ('dsl_sha256 = the library DSL' + (' and field payloads = the pinned fields manifest'
                                                       if fields is not None else ''))
    return res


def an_theme_corr(ctx) -> dict:
    out = {}
    for key in ('sig_corr', 'ic_corr'):
        r = analysis(ctx, key)
        if '_error' in r:
            continue
        ids, theme, _, admitted = _ids(ctx)
        sub = [ids.index(i) for i in admitted]
        out[key] = A.group_average(r['r'][np.ix_(sub, sub)], [theme[i] for i in admitted])
        out[key + '_all'] = A.group_average(r['r'], [theme[i] for i in ids])
    if not out:
        raise RuntimeError('no correlation matrix available')
    return out


def an_theme_year(ctx) -> dict:
    ic_all = need(ctx, 'ic')
    ids, theme, _, admitted = _ids(ctx)
    h = int(_acfg(ctx).get('theme_year_horizon', 21))
    by = A.ic_by_year(ic_all['ic'], ids, h, A.year_of_ns)
    years = sorted({y for v in by.values() for y in v})
    themes = list(dict.fromkeys(theme[i] for i in ids))
    mat, tot = {}, {}
    for t in themes:
        mem = [i for i in admitted if theme[i] == t and i in by]
        mat[t] = {y: _fmean([by[i].get(y) for i in mem]) for y in years}
        tot[t] = _fmean([A.ic_mean(ic_all['ic'], i, h) for i in mem])
    comb = A.ic_by_year(ic_all['ic_w'], ['__combined__'], h, A.year_of_ns).get('__combined__', {})
    return {'years': years, 'themes': themes, 'mat': mat, 'tot': tot, 'combined': comb, 'horizon': h,
            'combined_tot': A.ic_mean(ic_all['ic_w'], '__combined__', h), 'by_id': by}


def an_combined(ctx) -> dict:
    ic_all = need(ctx, 'ic')
    out = {}
    for h in _acfg(ctx).get('horizons', [5, 21, 63]):
        for key, src in (('w', ic_all['ic_w']), ('u', ic_all['ic'])):
            if ('__combined__', h) in src:
                v = src[('__combined__', h)][1]
                out[f'{key}{h}'] = {'mean': A.ic_mean(src, '__combined__', h), 'se': A.nw_se(v, 2 * h),
                                    'dates': int(np.isfinite(v).sum()),
                                    'pos_share': float(np.mean(v[np.isfinite(v)] > 0))}
    return out


def an_univ(ctx) -> dict:
    ac = _acfg(ctx)
    m = ctx.reg.read_json(f"{ac['role']}/manifest.json")
    u = m['universe']
    sb, se = int(m['score_begin']), int(m['score_end'])
    kept = np.array(u['kept_member_counts'][sb:se], dtype=float)
    base = np.array(u['base_member_counts'][sb:se], dtype=float)
    sess = np.frombuffer(ctx.reg.read_bytes(f"{ac['role']}/sessions.i64"), dtype='<i8')
    out = {'rule': u.get('rule'), 'id': u.get('id'), 'reasons': u.get('dropped_by_reason_score_window'),
           'reasons_all': u.get('dropped_by_reason'), 'dropped_share': u.get('dropped_member_share_score_window'),
           'dropped_share_all': u.get('dropped_member_share_all'), 'base_cells': u.get('base_member_cells'),
           'kept_cells': u.get('kept_member_cells'), 'non_operating_sic': u.get('non_operating_sic'),
           'limits': u.get('limits'), 'kept_min': float(kept.min()), 'kept_median': float(np.median(kept)),
           'kept_max': float(kept.max()), 'base_median': float(np.median(base)), 'membership_recipe': m.get('membership_recipe'),
           'dates': _dates_ns(sess[sb:se]), 'kept': kept.tolist(), 'base': base.tolist(),
           'first': D.ns_to_date(sess[sb]), 'last': D.ns_to_date(sess[se - 1]), 'sessions': se - sb,
           'scope_complete': (D.dig(ctx.analysis('fields'), 'bridge.scope_complete')),
           'instruments': m.get('instruments')}
    base_sw = float(base.sum())
    out['reason_share'] = {k: v / base_sw for k, v in (out['reasons'] or {}).items()} if base_sw else {}
    return out


def an_fields(ctx) -> dict:
    m = ctx.reg.read_json(_acfg(ctx)['fields_manifest'])
    if m is None:
        raise FileNotFoundError('fields manifest')
    al = alphas(ctx)
    used = {}
    rec = al.rec or {}
    for ln in rec.get('lineage', []):
        for f in ln.get('fields') or []:
            used.setdefault(f, []).append(ln['id'])
    rows = []
    for f in m['fields']:
        cov = f.get('coverage') or {}
        py = cov.get('per_year') or {}
        rows.append({'name': f['name'], 'pit': f.get('point_in_time'), 'clock': f.get('clock'),
                     'staleness': f.get('staleness'), 'units': f.get('units'),
                     'caveats': f.get('caveats') or [], 'vintage_safe_from': f.get('vintage_safe_from'),
                     'cov': {y: (py.get(y) or {}).get('finite_member_frac') for y in py},
                     'cov_score': (cov.get('score_window') or {}).get('finite_member_frac'),
                     'used_by': used.get(f['name'], [])})
    sc = m.get('source_checks') or {}
    bridge = D.dig(sc, 'issuer.identity_bridge') or {}
    return {'rows': rows, 'n_fields': len(rows), 'visibility': m.get('visibility_mark'), 'seal': m.get('seal'),
            'pit_def': m.get('point_in_time_definition'), 'n_pit': sum(1 for r in rows if r['pit']),
            'bridge': bridge, 'fund_lag': D.dig(sc, 'issuer.fund_lag_sessions'),
            'finra_sv_files': D.dig(sc, 'finra_short_volume.files_read'),
            'fund_rows_used': D.dig(sc, 'issuer.fund_events.rows_used'),
            'common_stock_verified': m.get('common_stock_verified'),
            'historical_vintage_verified': m.get('historical_vintage_verified'),
            'min_score_cov': min((r['cov_score'] for r in rows if C.is_num(r['cov_score'])), default=None)}


def _nav(ctx, cell, sid):
    d = cell.daily(sid)
    if d is None:
        raise FileNotFoundError(f'daily_{sid}.csv of {cell.name}')
    return d


def an_dd(ctx) -> dict:
    d = _nav(ctx, ctx.final, ctx.pid())
    dates, nav = D.nav_path(d)
    return {'rows': A.drawdowns(dates, nav, int(_acfg(ctx).get('drawdowns', 5)))}


def an_rstats(ctx) -> dict:
    cmp_name = _acfg(ctx).get('compare_cell')
    out = {}
    for key, cell in (('final', ctx.final), ('cmp', ctx.by.get(cmp_name) if cmp_name else None)):
        if cell is None:
            continue
        d = _nav(ctx, cell, ctx.pid())
        dates, r = D.returns(d)
        mo = D.monthly_returns(d)
        out[key] = {'daily': A.moments(r), 'monthly': A.moments(np.array(list(mo.values()))),
                    'worst': [{'date': a, 'r': b} for a, b in A.worst(dates, r, 5)], 'label': cell.label, 'series': dict(zip(dates, r.tolist())),
                    'months': mo}
    return out


def an_costdec(ctx) -> dict:
    out = {}
    for s in ctx.scen:
        d = ctx.full_daily(ctx.final, s['id'])
        if d is None:
            continue
        dates_all = d.dates()
        out[s['key']] = A.cost_by_year(d.cols, dates_all, d.ret_mask())
    if not out:
        raise FileNotFoundError('daily CSVs')
    tot = {}
    for k, yrs in out.items():
        t = {}
        for y in yrs.values():
            for kk, v in y.items():
                if isinstance(v, float) and kk not in ('lin_bps', 'imp_bps', 'cost_bps', 'net_comp'):
                    t[kk] = t.get(kk, 0.0) + v
        comp = 1.0
        for y in sorted(yrs):
            comp *= 1 + yrs[y]['net_comp']
        t['net_comp'] = comp - 1
        if t.get('traded'):
            t['lin_bps'] = t['lin_dollars'] / t['traded'] * 1e4
            t['imp_bps'] = t['imp_dollars'] / t['traded'] * 1e4
            t['cost_bps'] = t['lin_bps'] + t['imp_bps']
        tot[k] = t
    s2 = out.get(ctx.pkey, {})
    t2 = tot.get(ctx.pkey, {})
    all_drag = (t2.get('trade', 0) + t2.get('long_fin', 0) + t2.get('short_fin', 0)) if t2 else None
    share = {k: (t2.get(k) / (t2.get('trade', 0) + t2.get('long_fin', 0) + t2.get('short_fin', 0)))
             for k in ('linear', 'impact', 'long_fin', 'short_fin', 'trade')} if t2 else {}
    return {'by': out, 'tot': tot, 'years': sorted(s2), 'share': share, 'total_drag': all_drag,
            'fin_share': (t2.get('long_fin', 0) + t2.get('short_fin', 0)) /
                         (t2.get('trade', 0) + t2.get('long_fin', 0) + t2.get('short_fin', 0)) if t2 else None}


def _cell_costs(ctx, cell) -> dict:
    d = ctx.full_daily(cell, ctx.pid())
    if d is None:
        return {}
    tr = np.nansum(d.cols.get('traded_dollars', np.array([np.nan])))
    lin = np.nansum(d.cols.get('linear_cost_dollars', np.array([np.nan])))
    imp = np.nansum(d.cols.get('impact_cost_dollars', np.array([np.nan])))
    ex = d.cols['executed'] == 1
    nav = d.cols['pretrade_nav']
    return {'lin_bps': lin / tr * 1e4 if tr else None, 'imp_bps': imp / tr * 1e4 if tr else None,
            'cost_bps': (lin + imp) / tr * 1e4 if tr else None,
            'capped_day': float(np.nanmean(d.cols['capped_fills'][ex])) if 'capped_fills' in d.cols else None,
            'unfilled_nav': float(np.nanmean((d.cols['unfilled_dollars'] / nav)[ex])) if 'unfilled_dollars' in d.cols else None,
            'fills_day': float(np.nanmean(d.cols['fills'][ex])) if 'fills' in d.cols else None}


def an_cap(ctx) -> dict:
    pairs = []
    for p in _acfg(ctx).get('capacity') or []:
        pts = []
        for name in p['cells']:
            c = _cell(ctx, name)
            pt = {'name': name, 'label': c.label, 'L': ctx.metric(c, 'scen.construction.v5.aim_leverage'),
                  'gross': ctx.metric(c, 'summ.mean_gross_leverage_all_rows'), 'tau': ctx.metric(c, 'summ.tau_gmv_mean'),
                  'net': ctx.metric(c, 'summ.net_sharpe'), 'gross_sr': ctx.metric(c, 'summ.gross_sharpe'),
                  'summ_cost_bps': ctx.metric(c, 'summ.cost_bps_traded')}
            pt.update(_cell_costs(ctx, c))
            pts.append(pt)
        a, b = pts[0], pts[-1]
        if C.is_num(a.get('imp_bps')) and C.is_num(a.get('gross')) and C.is_num(b.get('gross')):
            b['imp_pred'] = a['imp_bps'] * math.sqrt(b['gross'] / a['gross'])
            b['cost_pred'] = a['lin_bps'] + b['imp_pred']
        pairs.append({'label': p['label'], 'points': pts,
                      'd_cost': (b.get('cost_bps') or 0) - (a.get('cost_bps') or 0),
                      'd_imp': (b.get('imp_bps') or 0) - (a.get('imp_bps') or 0),
                      'd_lin': (b.get('lin_bps') or 0) - (a.get('lin_bps') or 0),
                      'd_capped_pct': (b['capped_day'] / a['capped_day'] - 1) if a.get('capped_day') else None,
                      'd_unfilled_pct': (b['unfilled_nav'] / a['unfilled_nav'] - 1) if a.get('unfilled_nav') else None,
                      'd_scale_pct': (b['gross'] / a['gross'] - 1) if a.get('gross') else None})
    return {'pairs': pairs, 'p': {p['label'].split(' ')[0]: p for p in pairs}}


def an_e2e(ctx) -> dict:
    ref = _cell(ctx, _acfg(ctx)['e2e_reference'])
    p = D.paired(ctx.final.daily(ctx.pid()), ref.daily(ctx.pid()), ctx.annual)
    return {'ref': ref.label, **(p or {})}


def an_expo(ctx) -> dict:
    d = ctx.full_daily(ctx.final, ctx.pid())
    nav = d.cols['posttrade_nav']
    ok = np.isfinite(nav) & (nav > 0)
    dates = [x for x, k in zip(d.dates(), ok) if k]
    lo, sh = d.cols['long_dollars'][ok] / nav[ok], d.cols['short_dollars'][ok] / nav[ok]
    held = d.cols.get('held_names')
    return {'dates': dates, 'long': lo.tolist(), 'short': sh.tolist(), 'net': (lo - sh).tolist(),
            'long_mean': float(lo.mean()), 'short_mean': float(sh.mean()), 'net_max_abs': float(np.abs(lo - sh).max()),
            'held_mean': float(np.nanmean(held[ok])) if held is not None else None}


def an_fills(ctx) -> dict:
    d = ctx.full_daily(ctx.final, ctx.pid())
    ex = d.cols['executed'] == 1
    nav = d.cols['pretrade_nav']
    dates = [x for x, k in zip(d.dates(), ex) if k]
    ser = {'capped': d.cols['capped_fills'][ex], 'unfilled_bps': (d.cols['unfilled_dollars'] / nav)[ex] * 1e4,
           'blocked_absent': d.cols['blocked_absent'][ex], 'blocked_short': d.cols['blocked_short_names'][ex],
           'fills': d.cols['fills'][ex], 'stale': d.cols['stale_names'][ex],
           'forced_share': (d.cols['planned_forced'] / np.where(d.cols['planned_turnover'] > 0, d.cols['planned_turnover'], np.nan))[ex]}
    out = {'dates': dates, 'series': {k: v.tolist() for k, v in ser.items()}}
    for k, v in ser.items():
        v = v[np.isfinite(v)]
        out[k] = {'mean': float(v.mean()) if v.size else None, 'sum': float(v.sum()) if v.size else None,
                  'p95': float(np.quantile(v, .95)) if v.size else None, 'max': float(v.max()) if v.size else None}
    out['capped_share'] = out['capped']['sum'] / out['fills']['sum'] if out['fills']['sum'] else None
    return out


def an_monitor(ctx) -> dict:
    d = ctx.full_daily(ctx.final, ctx.pid())
    dates, r = D.returns(d)
    rm = d.ret_mask()
    gl = d.cols['gross_leverage'][rm]
    ser = {'tau': D.tau_sessions(d), 'gross_post_ramp': gl[63:], 'net_lev': d.cols['net_leverage'][rm],
           'roll63': D.rolling_sharpe(r, 63, ctx.annual), 'monthly': np.array(list(D.monthly_returns(d).values())),
           'daily': r, 'held': d.cols['held_names'][rm]}
    fl = analysis(ctx, 'fills')
    if '_error' not in fl:
        ser['capped'] = np.array(fl['series']['capped'])
        ser['unfilled_bps'] = np.array(fl['series']['unfilled_bps'])
    ic = analysis(ctx, 'ic')
    if '_error' not in ic and ('__combined__', 5) in ic['ic_w']:
        s, v = ic['ic_w'][('__combined__', 5)]
        mo: dict = {}
        for a, b in zip(s, v):
            if np.isfinite(b):
                x = D.ns_to_date(int(a))
                mo.setdefault((x.year, x.month), []).append(b)
        ser['ic5_month'] = np.array([np.mean(x) for x in mo.values()])
    cy = analysis(ctx, 'costdec')
    out = {}
    for k, v in ser.items():
        v = np.asarray(v, dtype=float)
        v = v[np.isfinite(v)]
        if v.size:
            q = np.quantile(v, [.01, .05, .5, .95, .99])
            out[k] = {'n': int(v.size), 'p1': float(q[0]), 'p5': float(q[1]), 'p50': float(q[2]), 'p95': float(q[3]),
                      'p99': float(q[4]), 'min': float(v.min()), 'max': float(v.max()), 'mean': float(v.mean())}
    if '_error' not in cy:
        v = np.array([y['cost_bps'] for y in cy['by'].get(ctx.pkey, {}).values() if C.is_num(y.get('cost_bps'))])
        if v.size:
            out['cost_bps_year'] = {'n': int(v.size), 'min': float(v.min()), 'max': float(v.max()), 'mean': float(v.mean()),
                                    'p5': None, 'p50': float(np.median(v)), 'p95': None, 'p1': None, 'p99': None}
    return out


def an_adm(ctx) -> dict:
    ids, theme, adm, admitted = _ids(ctx)
    st: dict = {}
    for i in ids:
        s = (adm.get(i) or {}).get('status')
        st[s] = st.get(s, 0) + 1
    al = alphas(ctx)
    w = al.primary['w'] if al.primary else {}
    themes = list(dict.fromkeys(theme[i] for i in ids))
    rules = al.primary['rules'] if al.primary else {}
    neg = [i for i in admitted if C.is_num((adm.get(i) or {}).get('hac_t')) and adm[i]['hac_t'] < 0]
    fast = [i for i in ids if C.is_num((adm.get(i) or {}).get('tau')) and adm[i]['tau'] >= 0.08]
    fast_w = sum((w or {}).get(i, 0.0) for i in fast)
    ts = [adm[i]['hac_t'] for i in admitted if C.is_num((adm.get(i) or {}).get('hac_t'))]
    return {'row': adm, 'median_t': float(np.median(ts)) if ts else None, 'n_t_gt_2': sum(1 for x in ts if x > 2),
            'n_t_gt_1': sum(1 for x in ts if x > 1), 'n': len(ids), 'n_admitted': len(admitted), 'status': st, 'n_themes': len(themes),
            'n_redundant': st.get('reject_redundant', 0), 'n_veto': st.get('reject_veto', 0),
            'rules': rules, 'neg_t_admitted': neg, 'n_neg_t_admitted': len(neg), 'fast': fast, 'fast_weight': fast_w,
            'max_w': max((w or {}).values() or [0.0]), 'theme_w': 1.0 / len(themes) if themes else None,
            'veto': [i for i in ids if (adm.get(i) or {}).get('status') == 'reject_veto']}


def an_misc(ctx) -> dict:
    f = ctx.final
    sr = ctx.metric(f, 'summ.net_sharpe')
    t = ctx.metric(f, 'summ.return_rows')
    years = t / ctx.annual if C.is_num(t) and C.is_num(ctx.annual) else None
    yr = ctx.years(f)
    tot = sum(v for v in yr.values() if C.is_num(v)) if yr else None
    first = min(yr) if yr else None
    lo_se = A.lo_se(sr, years) if C.is_num(sr) and years else None
    out = {'years': years, 'sr_se': lo_se, 'sr_minus_2se': sr - 2 * lo_se if lo_se else None,
           'sr_ci_lo': sr - 1.96 * lo_se if lo_se else None, 'sr_ci_hi': sr + 1.96 * lo_se if lo_se else None,
           'first_year': first, 'first_year_ret': yr.get(first) if yr else None,
           'first_year_share': (yr.get(first) / tot) if yr and tot else None,
           'val_se_2y': A.lo_se(sr, 2.0) if C.is_num(sr) else None}
    th = ctx.metric(f, 'rec.theta')
    out['half_life'] = math.log(0.5) / math.log(1 - th) if C.is_num(th) and 0 < th < 1 else None
    out['nonoverlap63'] = t / 63 if C.is_num(t) else None
    par = ctx.vs_parent(f)
    out['parent_t'] = par.get('t') if par else None
    return out


ANALYSES = {'ic': an_ic, 'cands': an_cands, 'ic_corr': an_ic_corr, 'sig_corr': an_sig_corr, 'theme_corr': an_theme_corr,
            'theme_year': an_theme_year, 'combined': an_combined, 'univ': an_univ, 'fields': an_fields, 'dd': an_dd,
            'rstats': an_rstats, 'costdec': an_costdec, 'cap': an_cap, 'e2e': an_e2e, 'expo': an_expo, 'fills': an_fills,
            'monitor': an_monitor, 'adm': an_adm, 'misc': an_misc}


# ============================================================================================ blocks: text
def blk_prose(ctx, spec) -> str:
    nb = (ctx.cfg.get('narrative_blocks') or {}).get(spec.get('key'), {})
    return (N.paragraphs(ctx, nb.get('paragraphs') or spec.get('paragraphs'))
            + N.check_list(ctx, nb.get('check') or spec.get('check')))


def blk_h3(ctx, spec) -> str:
    return f'<h3 class="sub">{N.fill_html(ctx, spec.get("text", ""))}</h3>'


def blk_callout(ctx, spec) -> str:
    c = (ctx.cfg.get('callouts') or {}).get(spec.get('key'), {})
    return C.callout(_txt(ctx, c.get('title', '')), [N.fill_html(ctx, x) for x in c.get('items', [])],
                     kind=c.get('kind', 'caveat'), paragraphs_html=[N.fill_html(ctx, x) for x in c.get('paragraphs', [])])


def blk_glossary(ctx, spec) -> str:
    entries = [{'term': e['term'], 'definition': N.fill_html(ctx, e['def']), 'group': e.get('group')}
               for e in ctx.cfg.get('glossary', [])]
    return C.glossary(entries)


def blk_facts(ctx, spec) -> str:
    fc = (ctx.cfg.get('facts') or {}).get(spec.get('key'), {})
    rows = [{'label': N.fill_html(ctx, r['label']), 'value': N.fill_html(ctx, r['value']),
             'src': N.fill_html(ctx, r.get('source', ''))} for r in fc.get('rows', [])]
    cols = [{'key': 'label', 'label': fc.get('label_head', 'Quantity'), 'kind': 'html', 'cls': 'wrap'},
            {'key': 'value', 'label': fc.get('value_head', 'Value'), 'kind': 'html', 'cls': 'wrap'},
            {'key': 'src', 'label': 'Source', 'kind': 'html', 'cls': 'wrap'}]
    return C.table(cols, rows, num=ctx.next_tab(), caption=_txt(ctx, fc.get('caption', '')), sortable=False,
                   tid=spec.get('id'))


def blk_risks(ctx, spec) -> str:
    rows = [{'risk': N.fill_html(ctx, r['risk']), 'ev': N.fill_html(ctx, r['evidence']),
             'resp': N.fill_html(ctx, r['response'])} for r in ctx.cfg.get('risks', [])]
    cols = [{'key': 'risk', 'label': 'Risk', 'kind': 'html', 'cls': 'wrap'},
            {'key': 'ev', 'label': 'Evidence (TRAIN)', 'kind': 'html', 'cls': 'wrap'},
            {'key': 'resp', 'label': 'Response / open item', 'kind': 'html', 'cls': 'wrap'}]
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-risks',
                   caption='Risk register: each risk with the TRAIN number that evidences it and the response or open '
                           'item; numbers are filled from the report inputs at build time.')


def blk_cellname(ctx, spec) -> str:
    name = ctx.final.name
    pat = re.compile(r'^(?P<lib>v\d+[a-z]*?)(?P<role>[lu]?)-(?P<comp>[a-z0-9]+)-t(?P<theta>[.\d]+)-d(?P<dust>[.\d]+)-'
                     r'(?P<rate>fixed|per-name)(?:-ob(?P<basis>[a-z]+))?(?:-x(?P<exit>[.\d]+))?(?P<loc>-loc)?'
                     r'(?:-L(?P<lev>[.\d]+))?$')
    m = pat.match(name)
    if not m:
        raise ValueError(f'cell name {name!r} does not parse')
    g = m.groupdict()
    meaning = (ctx.cfg.get('cellname_tokens') or {})
    rows = []
    order = [('lib', g['lib']), ('role', g['role'] or '(none)'), ('comp', g['comp']), ('theta', 't' + g['theta']),
             ('dust', 'd' + g['dust']), ('rate', g['rate']), ('basis', 'ob' + g['basis'] if g['basis'] else '(absent)'),
             ('exit', 'x' + g['exit'] if g['exit'] else '(absent)'), ('loc', 'loc' if g['loc'] else '(absent)'),
             ('lev', 'L' + g['lev'] if g['lev'] else '(absent)')]
    for k, tok in order:
        rows.append({'tok': tok, 'field': k, 'meaning': N.fill_html(ctx, meaning.get(k, ''))})
    cols = [{'key': 'tok', 'label': 'Token', 'kind': 'mono'}, {'key': 'field', 'label': 'Slot', 'kind': 'mono'},
            {'key': 'meaning', 'label': 'Meaning in this cell', 'kind': 'html', 'cls': 'wrap'}]
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-cellname',
                   caption=f"Decoding the final cell's directory name `{ctx.cfg.get('cell_prefix', '')}{name}` with the "
                           f"convention mega-nav-<lib><role>-<comp>-t<theta>-d<dust>-<rate>-ob<basis>-x<exit>[-loc][-L<lev>]; "
                           f"values cross-checked against its recipe.json.")


# ============================================================================================ blocks: data
def blk_flow(ctx, spec) -> str:
    stages = [{'title': _txt(ctx, s['title']), 'lines': [_txt(ctx, x) for x in s.get('lines', [])], 'kind': s.get('kind')}
              for s in ctx.cfg.get('flow', [])]
    svg = C.flow_diagram(stages, per_row=int(spec.get('per_row', 4)), aria='strategy pipeline')
    return C.figure(ctx.next_fig(), svg, _txt(ctx, spec.get('caption', 'Pipeline.')), 'fig-flow')


def blk_fields(ctx, spec) -> str:
    fs = need(ctx, 'fields')
    years = sorted({y for r in fs['rows'] for y in r['cov']})
    rows = []
    for r in fs['rows']:
        row = {'name': r['name'], 'pit': r['pit'], 'score': r['cov_score'], 'n_used': len(r['used_by']),
               'used': ', '.join(r['used_by'][:6]) + (' ...' if len(r['used_by']) > 6 else '') or None,
               'clock': (r['clock'] or '')[:90] + ('...' if r['clock'] and len(r['clock']) > 90 else ''),
               'clock__title': r['clock'], 'stale': (r['staleness'] or '')[:70], 'stale__title': r['staleness'],
               'cav': len(r['caveats']), 'cav__title': ' | '.join(r['caveats'])}
        for y in years:
            row[f'y{y}'] = r['cov'].get(y)
        rows.append(row)
    cols = [{'key': 'name', 'label': 'Field', 'kind': 'mono'}, {'key': 'pit', 'label': 'PIT', 'kind': 'chip'}]
    cols += [{'key': f'y{y}', 'label': f'Cov {y}', 'fmt': 'pct1'} for y in years]
    cols += [{'key': 'score', 'label': 'Cov TRAIN', 'fmt': 'pct1'}, {'key': 'n_used', 'label': '# alphas', 'fmt': 'int'},
             {'key': 'used', 'label': 'Used by', 'kind': 'text', 'cls': 'wrap', 'na_title': 'no library member reads it'},
             {'key': 'clock', 'label': 'Clock (hover for full text)', 'kind': 'text', 'cls': 'wrap'},
             {'key': 'cav', 'label': 'Caveats', 'fmt': 'int'}]
    cap = (f"The {fs['n_fields']} research fields of the TRAIN role ({fs['n_pit']} flagged point in time): coverage = "
           f"finite member cells / member cells per calendar year and over the TRAIN score window, the library members "
           f"that read each field, the visibility clock and the caveat count (hover for text). Source: "
           f"{Path(_acfg(ctx)['fields_manifest']).parent.name}/manifest.json fields[].coverage, clock, caveats; library "
           f"recipe lineage[].fields.")
    return C.table(cols, rows, num=ctx.next_tab(), caption=cap, tid='t-fields')


def blk_universe(ctx, spec) -> str:
    u = need(ctx, 'univ')
    rows = []
    for k, v in (u['reasons'] or {}).items():
        rows.append({'reason': k, 'sw': v, 'all': (u['reasons_all'] or {}).get(k), 'share': u['reason_share'].get(k)})
    rows.append({'reason': 'total dropped (score window)', 'sw': sum((u['reasons'] or {}).values()),
                 'all': sum((u['reasons_all'] or {}).values()), 'share': u['dropped_share'], '_cls': 'hl'})
    cols = [{'key': 'reason', 'label': 'Drop reason (first failing test)', 'kind': 'text'},
            {'key': 'sw', 'label': 'Member cells, TRAIN window', 'fmt': 'int'},
            {'key': 'all', 'label': 'Member cells, incl. warm-up', 'fmt': 'int'},
            {'key': 'share', 'label': 'Share of base member cells', 'fmt': 'pct1'}]
    tab = C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-universe',
                  caption=(f"Universe restriction {u['id']}: base member cells dropped by reason, in the order the rule "
                           f"tests them; the last row is the per-session dropped member share averaged over the TRAIN "
                           f"window. Source: {Path(_acfg(ctx)['role']).name}/manifest.json universe block."))
    series = [{'name': 'base members (top-3000 by 63-day $ADV)', 'x': u['dates'], 'y': u['base'], 'color': 'ref',
               'width': 1.6, 'end_label': f"base {C.fmt(u['base'][-1], 'int')}"},
              {'name': str(_acfg(ctx).get('universe_kept_label') or 'kept members (linked-operating-v1)'),
               'x': u['dates'], 'y': u['kept'], 'color': 's2', 'width': 2.2,
               'emph': True, 'end_label': f"kept {C.fmt(u['kept'][-1], 'int')}"}]
    svg = C.line_chart(series, height=260, y_fmt='int', y_label='Members per session', aria='universe size')
    lg = C.legend([{'name': s['name'], 'color': s['color'], 'width': 2} for s in series])
    fig = C.figure(ctx.next_fig(), svg,
                   f"Base and kept member counts per TRAIN session ({u['first']} to {u['last']}); kept min "
                   f"{C.fmt(u['kept_min'], 'int')}, median {C.fmt(u['kept_median'], 'int')}. Source: role manifest "
                   f"universe.base_member_counts / kept_member_counts, sessions.i64.", 'fig-universe', lg)
    return tab + fig


# ============================================================================================ blocks: alphas
def blk_library(ctx, spec) -> str:
    al = alphas(ctx)
    cands = need(ctx, 'cands')
    words = ctx.cfg.get('alpha_words') or {}
    hs = _acfg(ctx).get('horizons', [5, 21, 63])
    rows, cur = [], None
    for c in al.order:
        if c.get('theme') != cur:
            cur = c.get('theme')
            rows.append({'_group': cur})
        a = al.primary['adm'].get(c['id']) or {}
        r = {'id': c['id'], 'words': words.get(c['id']), 'dir': al.raw_dir(c['id']), 'tier': c.get('tier'),
             'st': al.status(a), 't': a.get('hac_t'), 'tau': a.get('tau'), 'w': al.weight(al.primary, c['id']),
             'rho': a.get('max_abs_rho'), 'rho__title': f"max |rho| with {a.get('max_abs_rho_with')}",
             'cov': (cands.get(c['id']) or {}).get(21, {}).get('coverage')}
        for h in hs:
            x = (cands.get(c['id']) or {}).get(h) or {}
            r[f'ic{h}'] = x.get('rank_mean')
            r[f'ic{h}_se'] = x.get('rank_se')
        rows.append(r)

    def ic_fmt(h):
        def f(v, row):
            if not C.is_num(v):
                return None
            return (f'{C.esc(C.fmt(v, "+.3f"))} <span class="sub" style="display:inline">'
                    f'({C.esc(C.fmt(row.get(f"ic{h}_se"), ".3f") or C.NA_TEXT)})</span>')
        return f
    cols = [{'key': 'id', 'label': 'Id', 'kind': 'mono'},
            {'key': 'words', 'label': 'Definition in words', 'kind': 'text', 'cls': 'wrap'},
            {'key': 'dir', 'label': 'Raw dir', 'fmt': '+d'}, {'key': 'tier', 'label': 'Tier', 'kind': 'text'},
            {'key': 'st', 'label': 'Admission', 'kind': 'text'}, {'key': 't', 'label': 'HAC t', 'fmt': '+.2f'},
            {'key': 'tau', 'label': 'Tau', 'fmt': '.4f'}, {'key': 'rho', 'label': 'Max |rho|', 'fmt': '.2f'},
            {'key': 'w', 'label': 'Weight', 'fmt': '.4f'}]
    cols += [{'key': f'ic{h}', 'label': f'Rank IC h{h} (SE)', 'fmt': ic_fmt(h)} for h in hs]
    cols += [{'key': 'cov', 'label': 'Coverage h21', 'fmt': 'pct1'}]
    cap = (f"The {len(al.order)} library candidates by theme: definition in words, raw literature direction (the DSL "
           f"embeds it so every prior sign is +1), literature tier, TRAIN admission status, HAC t and standalone daily "
           f"turnover tau of the neutralised gross-1 factor, largest |rho| of its factor return with another candidate, "
           f"ew-theme-v1 weight, and the mean daily cross-sectional rank IC at horizons {', '.join(map(str, hs))} sessions "
           f"with the runner's HAC SE; coverage = paired signal pairs / eligible pairs. Sources: "
           f"{al.primary['src'].get('admission')}, {al.primary['src'].get('weights')}, "
           f"{_acfg(ctx)['u_pass']}/train_candidates.jsonl; words: config alpha_words (from the DSL).")
    return C.table(cols, rows, num=ctx.next_tab(), caption=cap, sortable=False, tid='t-library')


def blk_ic_panel(ctx, spec) -> str:
    al = alphas(ctx)
    cands = need(ctx, 'cands')
    hs = _acfg(ctx).get('horizons', [5, 21, 63])
    rows = []
    for c in al.order:
        a = al.primary['adm'].get(c['id']) or {}
        adm = a.get('status') == 'admitted'
        x = cands.get(c['id']) or {}
        rows.append({'group': c.get('theme'), 'label': c['id'], 'filled': adm, 'color': 'accent' if adm else 'ref-2',
                     'values': {h: ((x.get(h) or {}).get('rank_mean'), (x.get(h) or {}).get('rank_se')) for h in hs}})
    comb = analysis(ctx, 'combined')
    if '_error' not in comb:
        rows.append({'group': 'combined signal', 'label': combined_label(ctx), 'filled': True, 'color': 's3',
                     'values': {h: ((comb.get(f'w{h}') or {}).get('mean'), (comb.get(f'w{h}') or {}).get('se')) for h in hs}})
    svg = C.ic_panel(rows, hs, panel_titles=[f'Rank IC, h = {h} sessions' for h in hs], aria='IC by horizon')
    lg = C.legend([{'name': 'Admitted', 'color': 'accent', 'kind': 'dot'},
                   {'name': 'Rejected', 'color': 'ref-2', 'kind': 'hollow'},
                   {'name': 'Combined signal (weighted pass)', 'color': 's3', 'kind': 'dot'},
                   {'name': 'Whisker: mean +- 1 SE', 'color': 'fg-2', 'width': 1}])
    cap = (f"Mean daily cross-sectional rank IC of each candidate's prior-signed signal with the forward return "
           f"close[d+1+h]/close[d+1]-1 over the TRAIN decisions, per horizon, with +-1 HAC SE (runner lag 2h); rows by "
           f"theme, own x scale per panel. The combined row is the {combined_label(ctx)} blend of the weighted pass "
           f"(Newey-West SE, "
           f"lag 2h, computed here). Sources: {_acfg(ctx)['u_pass']}/train_candidates.jsonl; "
           f"{_acfg(ctx).get('w_pass')}/train_daily_ic.csv (__combined__).")
    return C.figure(ctx.next_fig(), svg, cap, 'fig-ic-panel', lg)


def blk_decay(ctx, spec) -> str:
    al = alphas(ctx)
    cands = need(ctx, 'cands')
    hs = _acfg(ctx).get('horizons', [5, 21, 63])
    ids, theme, adm, admitted = _ids(ctx)
    panels = []
    for t in dict.fromkeys(theme[i] for i in ids):
        series = []
        mem = [i for i in ids if theme[i] == t]
        for i in mem:
            y = [((cands.get(i) or {}).get(h) or {}).get('rank_mean') for h in hs]
            ok = i in admitted
            series.append({'y': y, 'color': 'ref-2' if ok else 'ref', 'width': 1.0, 'dash': None if ok else '3 3',
                           'title': f'{i} ({"admitted" if ok else "rejected"})'})
        am = [i for i in mem if i in admitted]
        if am:
            y = [_fmean([((cands.get(i) or {}).get(h) or {}).get('rank_mean') for i in am]) for h in hs]
            series.append({'y': y, 'color': 'accent', 'width': 2.4, 'label': C.fmt(y[-1], '+.3f'),
                           'title': f'{t}: mean of {len(am)} admitted members'})
        panels.append({'title': f'{t} ({len(am)}/{len(mem)})', 'series': series})
    comb = analysis(ctx, 'combined')
    if '_error' not in comb:
        y = [(comb.get(f'w{h}') or {}).get('mean') for h in hs]
        panels.append({'title': f'combined {combined_label(ctx)}', 'series': [{'y': y, 'color': 's3', 'width': 2.4,
                                                                     'label': C.fmt(y[-1], '+.3f'), 'title': 'combined'}]})
    svg = C.multiples(panels, hs, cols=4, panel_h=150, y_fmt='+.2f', x_label='Horizon h, sessions (log scale)',
                      aria='IC term structure by theme')
    lg = C.legend([{'name': 'Theme mean over admitted members', 'color': 'accent', 'width': 2.4},
                   {'name': 'Admitted member', 'color': 'ref-2', 'width': 1},
                   {'name': 'Rejected member', 'color': 'ref', 'width': 1, 'dash': '3 3'},
                   {'name': f'Combined {combined_label(ctx)} signal', 'color': 's3', 'width': 2.4}])
    cap = (f"Alpha-decay proxy: mean rank IC at h = {', '.join(map(str, hs))} sessions per candidate, one panel per "
           f"theme (admitted / all members in the title), shared y scale. A persistent signal's cumulative-return IC "
           f"keeps rising with h; a flat or falling line means the information is used up within the first horizon. "
           f"Sources: {_acfg(ctx)['u_pass']}/train_candidates.jsonl; weighted-pass train_daily_ic.csv for the combined.")
    return C.figure(ctx.next_fig(), svg, cap, 'fig-decay', lg)


def blk_tau_ic(ctx, spec) -> str:
    al = alphas(ctx)
    cands = need(ctx, 'cands')
    h = int(_acfg(ctx).get('tau_ic_horizon', 5))
    pts = []
    vals = []
    for c in al.order:
        a = al.primary['adm'].get(c['id']) or {}
        ic = ((cands.get(c['id']) or {}).get(h) or {}).get('rank_mean')
        vals.append((abs(ic) if C.is_num(ic) else -1, c['id']))
        pts.append({'x': a.get('tau'), 'y': abs(ic) if C.is_num(ic) else None, 'id': c['id'], 'ic': ic,
                    'color': CLASS_TOKENS.get(theme_class(ctx, c.get('theme')), 's2'),
                    'hollow': C.is_num(ic) and ic < 0,
                    'title': f"{c['id']} ({c.get('theme')}): tau {C.fmt(a.get('tau'), '.4f')}, rank IC h{h} "
                             f"{C.fmt(ic, '+.4f')}, {al.status(a)}"})
    top = {i for _, i in sorted(vals, reverse=True)[:6]}
    for p in pts:
        if (C.is_num(p['x']) and p['x'] >= 0.08) or p['id'] in top:
            p['label'] = p['id']
    svg = C.scatter(pts, x_label='Standalone daily turnover tau (log scale)', y_label=f'|mean rank IC|, h = {h}',
                    x_fmt='.2f', y_fmt='.3f', height=420, aria='turnover vs IC', x_log=True)
    lg = C.legend([{'name': 'Fundamental themes', 'color': 's2', 'kind': 'dot'},
                   {'name': 'Price-based themes', 'color': 's1', 'kind': 'dot'},
                   {'name': 'Positioning / options themes', 'color': 'flat', 'kind': 'dot'},
                   {'name': 'Hollow: TRAIN IC sign against the prior', 'color': 'fg-3', 'kind': 'hollow'}])
    cap = (f"Cost efficiency of each signal: standalone daily one-way turnover of its neutralised factor (log x) vs "
           f"the absolute mean rank IC at h = {h}; labelled: fast sleeves (tau >= .08) and the six largest |IC|. "
           f"Sources: {al.primary['src'].get('admission')} tau; train_candidates.jsonl rank IC.")
    return C.figure(ctx.next_fig(), svg, cap, 'fig-tau-ic', lg)


def blk_theme_year(ctx, spec) -> str:
    ty = need(ctx, 'theme_year')
    rows = ty['themes'] + [f'combined ({combined_label(ctx)})']
    vals = [[ty['mat'][t].get(y) for y in ty['years']] for t in ty['themes']]
    vals.append([ty['combined'].get(y) for y in ty['years']])
    tot = [ty['tot'][t] for t in ty['themes']] + [ty['combined_tot']]
    hm = C.heatmap(rows, [str(y) for y in ty['years']], vals, value_fmt='+.3f', total=tot, total_label='TRAIN',
                   step=0.01, label_w=190, aria='IC by theme and year')
    cap = (f"Mean daily rank IC at h = {ty['horizon']} sessions per theme (equal-weight mean over its admitted members) "
           f"and calendar year of the decision, and for the combined {combined_label(ctx)} signal; 7-step diverging scale with "
           f"step .01. Sources: u-pass and weighted-pass train_daily_ic.csv.")
    return C.figure(ctx.next_fig(), hm, cap, 'fig-theme-year')


def blk_changes(ctx, spec) -> str:
    al = alphas(ctx)
    rec = al.rec or {}
    why = ctx.cfg.get('change_rationale') or {}
    rows = []
    for key, lib in (('v6_changes', 'v5.1 -> v6'), ('v61_changes', 'v6 -> v6.1')):
        for c in rec.get(key, []):
            if c.get('change') == 'smoothing':
                continue
            rows.append({'lib': lib, 'id': c['id'], 'chg': c.get('change'), 'detail': c.get('detail'),
                         'why': N.fill_html(ctx, why.get(c['id'], ''))})
    n_sm = sum(1 for c in rec.get('v6_changes', []) if c.get('change') == 'smoothing')
    rows.append({'lib': 'v5.1 -> v6', 'id': f'{n_sm} members', 'chg': 'smoothing',
                 'detail': 'decay_linear(R(x), 21) -> R(decay_linear(x, 21)) (or decay removed for the fast sleeves)',
                 'why': N.fill_html(ctx, why.get('_smoothing', ''))})
    cols = [{'key': 'lib', 'label': 'Step', 'kind': 'text'}, {'key': 'id', 'label': 'Id', 'kind': 'mono'},
            {'key': 'chg', 'label': 'Change', 'kind': 'text'},
            {'key': 'detail', 'label': 'Detail (recipe)', 'kind': 'text', 'cls': 'wrap'},
            {'key': 'why', 'label': 'Why (cited)', 'kind': 'html', 'cls': 'wrap'}]
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-changes',
                   caption=(f"Library changes v5.1 -> v6 -> v6.1 other than the smoothing switch (summarised in the last "
                            f"row): recipe v6_changes / v61_changes, with the rationale and its source. Source: "
                            f"{al.cfg.get('recipe')}."))


def blk_rules(ctx, spec) -> str:
    adm = need(ctx, 'adm')
    r = adm['rules']
    items = [('Screen', D.dig(ctx.analysis('adm'), 'rules.orientation')),
             ('Minimum TRAIN days', r.get('min_train_days')), ('Turnover limit tau', r.get('tau_limit')),
             ('Veto', f"HAC t < {r.get('veto_t')} ({D.dig(r, 'hac.kernel')} kernel, lag {D.dig(r, 'hac.lag')})"),
             ('Redundancy', f"|rho| > {r.get('rho_limit')}: {r.get('redundancy')}"),
             ('Tier order', ' > '.join(r.get('tier_order') or [])), ('Factor', r.get('factor')),
             ('Context', r.get('context')),
             ('Result', ', '.join(f'{k} {v}' for k, v in sorted(adm['status'].items())))]
    rows = [{'k': k, 'v': C.fmt(v, 'g') if C.is_num(v) else v} for k, v in items]
    cols = [{'key': 'k', 'label': 'Rule', 'kind': 'text'}, {'key': 'v', 'label': 'Value (verbatim)', 'kind': 'text', 'cls': 'wrap'}]
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-rules',
                   caption=f"Admission screen v4-prior-v1 as recorded in the admission file, and its outcome on the "
                           f"{adm['n']} candidates. Source: {alphas(ctx).primary['src'].get('admission')} rules, "
                           f"candidates[].status.")


def blk_corr(ctx, spec) -> str:
    kind = spec.get('kind', 'ic')
    if kind == 'theme':
        tc = need(ctx, 'theme_corr')
        key = spec.get('source', 'sig_corr')
        if key not in tc:
            raise RuntimeError(f'{key} not available')
        g = tc[key]
        m = g['mat'].tolist()
        svg = C.corr_matrix(g['groups'], m, label_w=170, cell_max=64, diag=True, aria='theme correlation')
        cap = (f"Theme-level average of the {'signal' if key == 'sig_corr' else 'IC-series'} correlation matrix over "
               f"admitted members: off-diagonal cells = mean correlation between members of two themes, diagonal = mean "
               f"within-theme correlation (NaN for single-member themes). Overall within {C.fmt(g['within'], '+.2f')}, "
               f"between {C.fmt(g['between'], '+.2f')}. Values x100.")
        return C.figure(ctx.next_fig(), svg, cap, f'fig-corr-theme-{key}')
    res = need(ctx, 'sig_corr' if kind == 'signal' else 'ic_corr')
    ids = res['ids']
    svg = C.corr_matrix(ids, res['r'].tolist(), groups=res['groups'], aria=f'{kind} correlation matrix')
    lg = ''
    if kind == 'signal':
        cap = (f"Candidate signal correlation: per TRAIN decision, the cross-sectional Spearman correlation of every "
               f"pair of candidate signals over that day's members (ranks over each signal's finite members, Pearson of "
               f"ranks on the names both cover), averaged over {res['n_days']} decisions (mean {C.fmt(res['mean_names'], 'int')} "
               f"members/day); rows in theme / roster order, values x100. Mean off-diagonal {C.fmt(res.get('mean'), '+.3f')}, "
               f"largest {C.fmt(res.get('max'), '+.2f')} ({res.get('max_pair')}), most negative {C.fmt(res.get('min'), '+.2f')} "
               f"({res.get('min_pair')}). Source: {_acfg(ctx)['candidate_cache']} payloads (raw signals, "
               f"date-major f64; entries matched by {res.get('match', 'dsl_sha256')}) and the role member.u8.")
    else:
        cap = (f"IC-series correlation: correlation across {res['dates']} TRAIN decisions of the candidates' daily rank IC "
               f"at h = {res['horizon']} (pairwise complete); it measures whether two signals win and lose on the same "
               f"days, a proxy for their factor-return correlation. Mean off-diagonal {C.fmt(res.get('mean'), '+.3f')}, "
               f"largest {C.fmt(res.get('max'), '+.2f')} ({res.get('max_pair')}), most negative "
               f"{C.fmt(res.get('min'), '+.2f')} ({res.get('min_pair')}). Values x100. Source: "
               f"{_acfg(ctx)['u_pass']}/train_daily_ic.csv rank_ic.")
    return C.figure(ctx.next_fig(), svg, cap, f'fig-corr-{kind}', lg)


def blk_theme_corr_table(ctx, spec) -> str:
    tc = need(ctx, 'theme_corr')
    rows = []
    for key, lab in (('sig_corr', 'Signal (cross-sectional Spearman)'), ('ic_corr', f'IC series (h = '
                                                                                   f'{_acfg(ctx).get("ic_corr_horizon", 5)})')):
        for sub, sl in (('', 'admitted'), ('_all', 'all candidates')):
            g = tc.get(key + sub)
            if not g:
                continue
            rows.append({'m': lab, 'set': sl, 'w': g['within'], 'b': g['between'], 'aw': g['abs_within'],
                         'ab': g['abs_between'], 'nw': g['n_within'], 'nb': g['n_between']})
    cols = [{'key': 'm', 'label': 'Correlation', 'kind': 'text'}, {'key': 'set', 'label': 'Set', 'kind': 'text'},
            {'key': 'w', 'label': 'Mean within theme', 'fmt': '+.3f'}, {'key': 'b', 'label': 'Mean between themes', 'fmt': '+.3f'},
            {'key': 'aw', 'label': 'Mean |r| within', 'fmt': '.3f'}, {'key': 'ab', 'label': 'Mean |r| between', 'fmt': '.3f'},
            {'key': 'nw', 'label': 'Pairs within', 'fmt': 'int'}, {'key': 'nb', 'label': 'Pairs between', 'fmt': 'int'}]
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-theme-corr',
                   caption='Average off-diagonal correlation of pairs in the same theme vs pairs in different themes, for '
                           'the signal and the IC-series matrices, over admitted members and over all candidates.')


# ============================================================================================ blocks: construction
def blk_steps(ctx, spec) -> str:
    rows = []
    for name in spec.get('cells', []):
        c = _cell(ctx, name)
        par = ctx.by.get(c.parent) if c.parent else None
        p = ctx.vs_parent(c)
        prior = spec.get('prior', '+')
        sign_ok = (p['dsr'] > 0) == (prior == '+') if p and C.is_num(p.get('dsr')) else None
        rows.append({'cell': f'{C.esc(c.label)}<span class="sub">{C.esc(c.name)}</span>', 'lever': c.lever,
                     'par': par.label if par else None, 'net': ctx.metric(c, 'summ.net_sharpe'),
                     'dsr': p.get('dsr') if p else None, 'se': p.get('se') if p else None, 't': p.get('t') if p else None,
                     'rho': p.get('rho') if p else None, 'ok': sign_ok, 'verdict': c.verdict,
                     'tau': ctx.metric(c, 'summ.tau_gmv_mean'), 'cost': ctx.metric(c, 'summ.cost_bps_traded'),
                     'gl': ctx.metric(c, 'summ.mean_gross_leverage_all_rows'), 'nl': ctx.metric(c, 'summ.mean_net_leverage_all_rows'),
                     '_cls': 'hl' if c is ctx.final else ''})
    span = max([abs(r['dsr']) for r in rows if C.is_num(r['dsr'])] or [1.0])
    cols = [{'key': 'cell', 'label': 'Cell', 'kind': 'html'}, {'key': 'lever', 'label': 'Lever', 'kind': 'text', 'cls': 'wrap'},
            {'key': 'par', 'label': 'Paired against', 'kind': 'text'},
            {'key': 'net', 'label': f"Net SR {ctx.primary['short']}", 'fmt': '+.3f'},
            {'key': 'dsr', 'label': 'dSR vs parent', 'fmt': '+.3f', 'bar': {'lo': -span, 'hi': span}},
            {'key': 'se', 'label': 'Memmel SE', 'fmt': '.3f'}, {'key': 't', 'label': 't', 'fmt': '+.2f'},
            {'key': 'rho', 'label': 'rho', 'fmt': '.3f'},
            {'key': 'ok', 'label': f"Sign = prior {spec.get('prior', '+')}", 'kind': 'chip'},
            {'key': 'tau', 'label': 'Tau mean', 'fmt': '.4f'}, {'key': 'cost', 'label': 'Cost bps/$', 'fmt': '.2f'},
            {'key': 'gl', 'label': 'Gross lev all', 'fmt': '.3f'}, {'key': 'nl', 'label': 'Net lev all', 'fmt': '+.4f'},
            {'key': 'verdict', 'label': 'Verdict (ledger)', 'kind': 'text'}]
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid=spec.get('id'),
                   caption=_txt(ctx, spec.get('caption', '')) + (
                       f" Paired dSR recomputed here from the daily_{ctx.pid()}.csv net returns on common return sessions "
                       f"with the Memmel (2003) SE; acceptance was the sign of dSR vs the pre-registered prior, not its size."))


def blk_exposure(ctx, spec) -> str:
    e = need(ctx, 'expo')
    svg = C.exposure_chart(e['dates'], e['long'], e['short'], e['net'], y_fmt='.2f',
                           y_label='Dollars / post-trade NAV', aria='long, short and net exposure')
    lg = C.legend([{'name': 'Long dollars / NAV', 'color': 's2', 'kind': 'bar'},
                   {'name': 'Short dollars / NAV (drawn below zero)', 'color': 's3', 'kind': 'bar'},
                   {'name': 'Net', 'color': 'fg', 'width': 1.6}])
    cap = (f"Long and short dollars of {ctx.final.label} as a fraction of post-trade NAV per session, "
           f"{ctx.primary['label']}; mean long {C.fmt(e['long_mean'], '.3f')}, mean short {C.fmt(e['short_mean'], '.3f')}, "
           f"max |net| {C.fmt(e['net_max_abs'], '.4f')}, mean held names {C.fmt(e['held_mean'], 'int')}. The ramp at the start "
           f"is theta partial deployment from zero. Source: daily_{ctx.pid()}.csv long_dollars, short_dollars, posttrade_nav.")
    return C.figure(ctx.next_fig(), svg, cap, 'fig-exposure', lg)


def blk_fills(ctx, spec) -> str:
    f = need(ctx, 'fills')
    s = f['series']
    panels = [{'label': 'Capped fills per session (1% ADV cap binds)', 'y': s['capped'], 'fmt': 'int', 'color': 's3',
               'note': f"mean {C.fmt(f['capped']['mean'], '.1f')}, total {C.fmt(f['capped']['sum'], 'int')} "
                       f"= {C.fmt(f['capped_share'], 'pct2')} of fills"},
              {'label': 'Unfilled dollars, bps of NAV', 'y': s['unfilled_bps'], 'fmt': '.0f', 'color': 'flat',
               'note': f"mean {C.fmt(f['unfilled_bps']['mean'], '.1f')} bps, p95 {C.fmt(f['unfilled_bps']['p95'], '.1f')}"},
              {'label': 'Orders blocked: name absent (stale)', 'y': s['blocked_absent'], 'fmt': 'int', 'color': 'ref-2',
               'note': f"mean {C.fmt(f['blocked_absent']['mean'], '.2f')}, total {C.fmt(f['blocked_absent']['sum'], 'int')}"},
              {'label': 'Short names blocked: special borrow tier', 'y': s['blocked_short'], 'fmt': 'int', 'color': 'tiers',
               'note': f"mean {C.fmt(f['blocked_short']['mean'], '.2f')}, total {C.fmt(f['blocked_short']['sum'], 'int')}"},
              {'label': 'Forced (nonmember) share of planned turnover', 'y': s['forced_share'], 'fmt': 'pct0', 'color': 's1',
               'note': f"mean {C.fmt(f['forced_share']['mean'], 'pct1')}"}]
    svg = C.fill_panel(f['dates'], panels, aria='execution frictions')
    cap = (f"Execution frictions per executed session of {ctx.final.label}, {ctx.primary['label']}: fills capped at the "
           f"participation limit, dollars left unfilled, orders blocked because the name printed no price, shorts "
           f"refused by the special-tier block (after locate-in-aim), and the nonmember-exit share of planned turnover. "
           f"Each panel has its own y scale. Source: daily_{ctx.pid()}.csv capped_fills, unfilled_dollars, blocked_absent, "
           f"blocked_short_names, planned_forced / planned_turnover.")
    return C.figure(ctx.next_fig(), svg, cap, 'fig-fills')


# ============================================================================================ blocks: costs
def blk_cost_model(ctx, spec) -> str:
    rec = ctx.recipe(ctx.final) or {}
    by_id = {s['id']: s for s in rec.get('scenarios', [])}
    rows = []
    for s in ctx.scen:
        r = by_id.get(s['id'])
        if not r:
            continue
        fin = r.get('financing') or {}
        tf = fin.get('tier_fee_bps') or {}
        rows.append({'sc': s['label'], 'rule': r.get('cost_rule'), 'hs': r.get('half_spread_bps'), 'com': r.get('commission_bps'),
                     'flat': r.get('flat_bps'), 'y': r.get('impact_y'), 'dl': r.get('impact_delta'),
                     'cap': r.get('max_participation'), 'stale': r.get('stale_exit_sessions'),
                     'hl': r.get('terminal_haircut_long'), 'hsh': r.get('terminal_haircut_short'), 'fin': fin.get('id'),
                     'dc': fin.get('day_count'), 'ls': fin.get('long_spread_bps'), 'ss': fin.get('short_spread_bps'),
                     'fs': fin.get('flat_short_bps'), 'gc': tf.get('gc'), 'wm': tf.get('warm'), 'sp': tf.get('special'),
                     'blk': fin.get('block_special_shorts'), '_cls': 'hl' if s['key'] == ctx.pkey else ''})
    cols = [{'key': 'sc', 'label': 'Scenario', 'kind': 'text'}, {'key': 'rule', 'label': 'Cost rule', 'kind': 'mono'},
            {'key': 'hs', 'label': 'Half-spread bps', 'fmt': 'g'}, {'key': 'com', 'label': 'Commission bps', 'fmt': 'g'},
            {'key': 'flat', 'label': 'Flat bps', 'fmt': 'g'}, {'key': 'y', 'label': 'Impact Y', 'fmt': 'g'},
            {'key': 'dl', 'label': 'Impact exponent', 'fmt': 'g'}, {'key': 'cap', 'label': 'Max participation', 'kind': 'text'},
            {'key': 'stale', 'label': 'Write-off after K absent', 'fmt': 'g'},
            {'key': 'hl', 'label': 'Terminal haircut long', 'fmt': '+g'}, {'key': 'hsh', 'label': 'Terminal haircut short', 'fmt': '+g'},
            {'key': 'fin', 'label': 'Financing', 'kind': 'mono'}, {'key': 'dc', 'label': 'Day count', 'kind': 'text'},
            {'key': 'ls', 'label': 'Long spread bps', 'fmt': 'g'}, {'key': 'ss', 'label': 'Short spread bps', 'fmt': 'g'},
            {'key': 'fs', 'label': 'Flat short bps', 'fmt': 'g'}, {'key': 'gc', 'label': 'GC fee bps', 'fmt': 'g'},
            {'key': 'wm', 'label': 'Warm fee bps', 'fmt': 'g'}, {'key': 'sp', 'label': 'Special fee bps', 'fmt': 'g'},
            {'key': 'blk', 'label': 'Special shorts blocked', 'kind': 'text'}]
    for r in rows:
        r['cap'] = C.fmt(r['cap'], 'pct1') + ' of ADV63' if C.is_num(r['cap']) else r['cap']
        r['blk'] = 'yes' if r['blk'] else 'no'
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-cost-model',
                   caption=(f"Cost and financing scenarios exactly as the final cell's recipe declares them. Trading cost "
                            f"per fill = (half-spread + commission) x |$| + Y x sigma63 x (|q| / ADV63)^exponent x |$| "
                            f"(sqrt-impact-v1) or flat bps x |$| (flat-bps-v1); financing accrues on pre-mark dollars x "
                            f"calendar days / day count. Source: {ctx.final.rel_dir}/recipe.json scenarios[]."))


def blk_financing(ctx, spec) -> str:
    f = ctx.final
    fin = ctx.metric(f, 'scen.financing') or {}
    sh = D.dig(fin, 'short_dollar_share_by_tier.mean') or {}
    p95 = D.dig(fin, 'short_dollar_share_by_tier.p95') or {}
    bt = fin.get('short_financing_by_tier_dollars') or {}
    md = fin.get('member_decisions_by_tier') or {}
    fee = D.dig(fin, 'spec.tier_fee_bps') or {}
    rows = []
    for t in ('gc', 'warm', 'special'):
        rows.append({'tier': t, 'fee': fee.get(t), 'md': md.get(t), 'sh': sh.get(t), 'p95': p95.get(t),
                     'usd': (bt.get(t) or 0) / 1e6 if C.is_num(bt.get(t)) else None})
    rows.append({'tier': 'long leg', 'fee': D.dig(fin, 'spec.long_spread_bps'), 'md': None, 'sh': None, 'p95': None,
                 'usd': (fin.get('long_financing_dollars') or 0) / 1e6, '_cls': 'hl'})
    cols = [{'key': 'tier', 'label': 'Tier', 'kind': 'text'}, {'key': 'fee', 'label': 'Fee / spread bps', 'fmt': 'g'},
            {'key': 'md', 'label': 'Member-decisions in tier', 'fmt': 'int', 'na_title': 'not applicable'},
            {'key': 'sh', 'label': 'Share of short $, mean', 'fmt': 'pct2', 'na_title': 'not applicable'},
            {'key': 'p95', 'label': 'Share of short $, p95', 'fmt': 'pct2', 'na_title': 'not applicable'},
            {'key': 'usd', 'label': 'Financing paid $M (3 yrs)', 'fmt': '.2f'}]
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-financing',
                   caption=(f"Borrow tiers of the primary book ({ctx.primary['label']}): tier fee (on top of the "
                            f"{C.fmt(D.dig(fin, 'spec.short_spread_bps'), 'g')} bps short spread), member-decisions per tier, "
                            f"share of short dollars per tier and the financing paid; blocked special shorts "
                            f"{C.fmt(fin.get('blocked_short_name_decisions'), 'int')} name-decisions "
                            f"(${C.fmt((fin.get('blocked_short_dollars') or 0) / 1e6, '.1f')}M); locate-in-aim zeroed "
                            f"{C.fmt(ctx.metric(f, 'sum.locate_in_aim.zeroed_special_short_aims'), 'int')} special-tier "
                            f"short aims before neutralisation. Source: summary.json scenarios[S2].financing, locate_in_aim."))


COMP = [('linear', 'Spread + commission', 's2'), ('impact', 'Market impact', 's1'),
        ('long_fin', 'Long financing', 'flat'), ('short_fin', 'Short financing (borrow)', 'tiers')]


def blk_costdec(ctx, spec) -> str:
    cd = need(ctx, 'costdec')
    by = cd['by'][ctx.pkey]
    years = cd['years']
    series = [{'name': lab, 'color': tok, 'label': True, 'values': [by[y][k] for y in years] + [cd['tot'][ctx.pkey][k] / len(years)]}
              for k, lab, tok in COMP]
    cats = [str(y) for y in years] + ['per-year mean']
    svg = C.bar_chart(cats, series, value_fmt='pct2', tick_fmt='pct1', height=300,
                      y_label='Drag, sum of daily returns (% of NAV)', aria='cost decomposition by year')
    lg = C.legend([{'name': lab, 'color': tok, 'kind': 'bar'} for _, lab, tok in COMP])
    fig = C.figure(ctx.next_fig(), svg,
                   f"Cost and financing drag of {ctx.final.label} by calendar year, {ctx.primary['label']}: arithmetic "
                   f"sum of the daily return components; the trading cost of return row t is split into spread+commission "
                   f"and impact by the dollars of its fill session t-1. Source: daily_{ctx.pid()}.csv trade_cost_return, "
                   f"linear_cost_dollars, impact_cost_dollars, long_financing_return, borrow_return.", 'fig-costdec', lg)
    rows = []
    for y in years + ['total']:
        o = by[y] if y != 'total' else cd['tot'][ctx.pkey]
        rows.append({'y': str(y), 'g': o['gross_ex_wo'], 'wo': o['writeoff'], 'lin': o['linear'], 'imp': o['impact'],
                     'lf': o['long_fin'], 'gc': o['short_gc'], 'wm': o['short_warm'], 'sp': o['short_special'],
                     'net': o['net'], 'nc': o['net_comp'], 'lb': o.get('lin_bps'), 'ib': o.get('imp_bps'),
                     'cb': o.get('cost_bps'), '_cls': 'hl' if y == 'total' else ''})
    cols = [{'key': 'y', 'label': 'Year', 'kind': 'text'}, {'key': 'g', 'label': 'Gross ex write-off', 'fmt': '+pct2'},
            {'key': 'wo', 'label': 'Write-offs', 'fmt': '+pct2'}, {'key': 'lin', 'label': 'Spread+comm.', 'fmt': 'pct2'},
            {'key': 'imp', 'label': 'Impact', 'fmt': 'pct2'}, {'key': 'lf', 'label': 'Long fin.', 'fmt': 'pct2'},
            {'key': 'gc', 'label': 'Short fin. GC', 'fmt': 'pct2'}, {'key': 'wm', 'label': 'Short fin. warm', 'fmt': 'pct2'},
            {'key': 'sp', 'label': 'Short fin. special', 'fmt': 'pct2'}, {'key': 'net', 'label': 'Net (sum)', 'fmt': '+pct2'},
            {'key': 'nc', 'label': 'Net (compounded)', 'fmt': '+pct2'}, {'key': 'lb', 'label': 'Spread+comm. bps/$', 'fmt': '.2f'},
            {'key': 'ib', 'label': 'Impact bps/$', 'fmt': '.2f'}, {'key': 'cb', 'label': 'Cost bps/$', 'fmt': '.2f'}]
    tab = C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-costdec',
                  caption=(f"Gross-to-net attribution of {ctx.final.label} by year, {ctx.primary['label']} (sums of daily "
                           f"returns, so gross - costs - financing = net (sum) exactly; the compounded column is the year's "
                           f"NAV return); bps/$ = cost dollars per traded dollar of fills in that year."))
    return fig + tab


def blk_attrib(ctx, spec) -> str:
    cd = need(ctx, 'costdec')
    rows = []
    for s in ctx.scen:
        by = cd['by'].get(s['key'])
        if not by:
            continue
        rows.append({'_group': f"{s['label']} ({s['id']})"})
        for y in cd['years'] + ['total']:
            o = by[y] if y != 'total' else cd['tot'][s['key']]
            rows.append({'y': str(y), 'g': o['gross_ex_wo'], 'wo': o['writeoff'], 'tc': o['trade'],
                         'fin': o['long_fin'] + o['short_fin'], 'net': o['net'], 'nc': o['net_comp'],
                         '_cls': 'hl' if y == 'total' else ''})
    cols = [{'key': 'y', 'label': 'Year', 'kind': 'text'}, {'key': 'g', 'label': 'Gross ex write-off', 'fmt': '+pct2'},
            {'key': 'wo', 'label': 'Write-offs', 'fmt': '+pct2'}, {'key': 'tc', 'label': 'Trading cost', 'fmt': 'pct2'},
            {'key': 'fin', 'label': 'Financing', 'fmt': 'pct2'}, {'key': 'net', 'label': 'Net (sum)', 'fmt': '+pct2'},
            {'key': 'nc', 'label': 'Net (compounded)', 'fmt': '+pct2'}]
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-attrib',
                   caption=(f"Gross / net attribution of {ctx.final.label} by year and cost scenario (sums of daily return "
                            f"components; the gross column differs across scenarios because fills, caps and write-offs "
                            f"differ). Source: daily_<scenario>.csv of {ctx.final.rel_dir}."))


def blk_capacity(ctx, spec) -> str:
    cap = need(ctx, 'cap')
    series, rows = [], []
    toks = ['s2', 'ref-2']
    for k, p in enumerate(cap['pairs']):
        tok = toks[k % 2]
        series.append({'name': f"{p['label']}: cost bps/$", 'color': tok, 'width': 1.8,
                       'points': [(q['L'], q['cost_bps'], f"{q['label']} {C.fmt(q['cost_bps'], '.2f')}") for q in p['points']],
                       'end_label': f"{p['label'].split(' ')[0]} total"})
        series.append({'name': f"{p['label']}: impact bps/$", 'color': tok, 'width': 1.2, 'dash': '4 3',
                       'points': [(q['L'], q['imp_bps'], C.fmt(q['imp_bps'], '.2f')) for q in p['points']],
                       'end_label': f"{p['label'].split(' ')[0]} impact"})
        b = p['points'][-1]
        if C.is_num(b.get('imp_pred')):
            series.append({'name': f"{p['label']}: sqrt-law prediction", 'color': 'fg-3', 'width': 1, 'markers': True,
                           'points': [(b['L'], b['imp_pred'], f"sqrt law {C.fmt(b['imp_pred'], '.2f')}")]})
        for q in p['points']:
            rows.append({'pair': p['label'], 'cell': f'{C.esc(q["label"])}<span class="sub">{C.esc(q["name"])}</span>',
                         'L': q['L'], 'g': q['gross'], 'tau': q['tau'], 'cb': q['cost_bps'], 'lb': q['lin_bps'],
                         'ib': q['imp_bps'], 'ip': q.get('imp_pred'), 'cf': q['capped_day'], 'uf': q['unfilled_nav'],
                         'gs': q['gross_sr'], 'net': q['net']})
    svg = C.xy_chart(series, x_label='Aim leverage L', y_label='Cost per traded $, bps', x_fmt='.2f', y_fmt='.1f',
                     height=340, aria='capacity proxy')
    lg = C.legend([{'name': s['name'], 'color': s['color'], 'width': max(s.get('width', 1.5), 1.5), 'dash': s.get('dash')}
                   for s in series])
    fig = C.figure(ctx.next_fig(), svg,
                   "Capacity proxy: cost per traded dollar (solid) and its impact part (dashed) at L = 1 and at the "
                   "deployed L for two otherwise-identical cell pairs; grey marker = the impact the square-root law "
                   "predicts from the L 1 cell scaled by the ratio of all-rows gross. Two points per pair: this is a "
                   "local slope, not a capacity curve. Sources: daily_S2.csv linear / impact cost and traded dollars; "
                   "nav_summ gross leverage and Sharpe.", 'fig-capacity', lg)
    cols = [{'key': 'pair', 'label': 'Pair', 'kind': 'text'}, {'key': 'cell', 'label': 'Cell', 'kind': 'html'},
            {'key': 'L', 'label': 'L', 'fmt': 'g'}, {'key': 'g', 'label': 'Gross lev all', 'fmt': '.3f'},
            {'key': 'tau', 'label': 'Tau mean', 'fmt': '.4f'}, {'key': 'cb', 'label': 'Cost bps/$', 'fmt': '.2f'},
            {'key': 'lb', 'label': 'Spread+comm. bps/$', 'fmt': '.2f'}, {'key': 'ib', 'label': 'Impact bps/$', 'fmt': '.2f'},
            {'key': 'ip', 'label': 'Impact, sqrt-law pred.', 'fmt': '.2f', 'na_title': 'reference point'},
            {'key': 'cf', 'label': 'Capped fills/day', 'fmt': '.1f'}, {'key': 'uf', 'label': 'Unfilled / NAV / day', 'fmt': 'pct3'},
            {'key': 'gs', 'label': 'Gross SR', 'fmt': '.3f'}, {'key': 'net', 'label': 'Net SR', 'fmt': '+.3f'}]
    tab = C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-capacity',
                  caption='The capacity-proxy cells: leverage, turnover, cost per traded dollar split into spread + '
                          'commission and impact (with the square-root-law prediction for the higher-L cell), participation-'
                          'cap frictions and Sharpe.')
    return fig + tab


# ============================================================================================ blocks: results
def blk_drawdowns(ctx, spec) -> str:
    dd = need(ctx, 'dd')
    rows = [{'k': i + 1, 'depth': r['depth'], 'peak': r['peak'].isoformat(), 'trough': r['trough'].isoformat(),
             'rec': r['recovery'].isoformat() if r['recovery'] else 'not recovered', 'down': r['down'], 'up': r['up'],
             'tot': r['total']} for i, r in enumerate(dd['rows'])]
    cols = [{'key': 'k', 'label': '#', 'fmt': 'int'}, {'key': 'depth', 'label': 'Depth', 'fmt': 'pct2'},
            {'key': 'peak', 'label': 'Peak', 'kind': 'mono'}, {'key': 'trough', 'label': 'Trough', 'kind': 'mono'},
            {'key': 'rec', 'label': 'Recovered', 'kind': 'mono'}, {'key': 'down', 'label': 'Sessions down', 'fmt': 'int'},
            {'key': 'up', 'label': 'Sessions to recover', 'fmt': 'int', 'na_title': 'not recovered by the window end'},
            {'key': 'tot', 'label': 'Total sessions', 'fmt': 'int', 'na_title': 'not recovered by the window end'}]
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-drawdowns',
                   caption=(f"Five deepest peak-to-trough drawdowns of the net NAV of {ctx.final.label}, "
                            f"{ctx.primary['label']}, with dates and recovery (first session back at the prior peak). "
                            f"Source: daily_{ctx.pid()}.csv net_return."))


def blk_retstats(ctx, spec) -> str:
    rs = need(ctx, 'rstats')
    rows = []
    for key in ('final', 'cmp'):
        if key not in rs:
            continue
        for per in ('daily', 'monthly'):
            m = rs[key][per]
            rows.append({'cell': rs[key]['label'], 'per': per, **m, '_cls': 'hl' if key == 'final' else ''})
    cols = [{'key': 'cell', 'label': 'Cell', 'kind': 'text'}, {'key': 'per', 'label': 'Returns', 'kind': 'text'},
            {'key': 'n', 'label': 'n', 'fmt': 'int'}, {'key': 'mean', 'label': 'Mean', 'fmt': '+pct3'},
            {'key': 'sd', 'label': 'SD', 'fmt': 'pct3'}, {'key': 'skew', 'label': 'Skew', 'fmt': '+.2f'},
            {'key': 'kurt', 'label': 'Kurtosis (raw)', 'fmt': '.1f'}, {'key': 'min', 'label': 'Min', 'fmt': '+pct2'},
            {'key': 'p5', 'label': 'p5', 'fmt': '+pct2'}, {'key': 'p50', 'label': 'Median', 'fmt': '+pct3'},
            {'key': 'p95', 'label': 'p95', 'fmt': '+pct2'}, {'key': 'max', 'label': 'Max', 'fmt': '+pct2'},
            {'key': 'pos_share', 'label': '% positive', 'fmt': 'pct1'}]
    t1 = C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-retstats',
                 caption=(f"Distribution of daily and calendar-month net returns ({ctx.primary['label']}) of the final cell "
                          f"and the comparison cell; population skew and raw kurtosis (normal = 3). Source: "
                          f"daily_{ctx.pid()}.csv net_return over return rows."))
    rows2 = []
    cmpser = rs.get('cmp', {}).get('series', {})
    for i, w_ in enumerate(rs['final']['worst']):
        d_, r = w_['date'], w_['r']
        rows2.append({'k': i + 1, 'd': d_.isoformat(), 'r': r, 'c': cmpser.get(d_)})
    cols2 = [{'key': 'k', 'label': '#', 'fmt': 'int'}, {'key': 'd', 'label': 'Session', 'kind': 'mono'},
             {'key': 'r', 'label': f"{rs['final']['label']} net", 'fmt': '+pct2'},
             {'key': 'c', 'label': f"{rs.get('cmp', {}).get('label', 'comparison')} same day", 'fmt': '+pct2'}]
    t2 = C.table(cols2, rows2, num=ctx.next_tab(), sortable=False, tid='t-worst',
                 caption='The five worst daily net returns of the final cell, with the comparison cell on the same session.')
    return t1 + t2


def blk_monitor(ctx, spec) -> str:
    mon = need(ctx, 'monitor')
    rows = []
    for r in ctx.cfg.get('monitoring', []):
        st = mon.get(r['series']) or {}
        f = r.get('fmt', '.4f')
        rows.append({'m': r['metric'], 'n': st.get('n'), 'p1': st.get('p1'), 'p5': st.get('p5'), 'p50': st.get('p50'),
                     'p95': st.get('p95'), 'p99': st.get('p99'), 'min': st.get('min'), 'max': st.get('max'),
                     'rule': N.fill_html(ctx, r.get('rule', '')), '_f': f})

    def mk(key):
        def fm(v, row):
            s = C.fmt(v, row['_f'])
            return None if s is None else C.esc(s)
        return fm
    cols = [{'key': 'm', 'label': 'Live metric', 'kind': 'text', 'cls': 'wrap'}, {'key': 'n', 'label': 'TRAIN n', 'fmt': 'int'}]
    cols += [{'key': k, 'label': k if k[0] == 'p' else k.capitalize(), 'fmt': mk(k)} for k in ('min', 'p1', 'p5', 'p50', 'p95', 'p99', 'max')]
    cols += [{'key': 'rule', 'label': 'Alarm rule (proposal)', 'kind': 'html', 'cls': 'wrap'}]
    return C.table(cols, rows, num=ctx.next_tab(), sortable=False, tid='t-monitor',
                   caption=(f"Monitoring plan: each live metric with its TRAIN distribution for {ctx.final.label} "
                            f"({ctx.primary['short']}), computed here from the daily CSV, the weighted-pass daily IC and the "
                            f"cost decomposition; the alarm rules are proposals, not pre-registered tests."))


def files_table(ctx) -> str:
    man = ctx.reg.manifest()
    rows = [{'path': m['path'], 'status': m['status'], 'bytes': m['bytes'], 'sha': m['sha256']} for m in man]
    return C.table([{'key': 'path', 'label': 'Path (relative to inputs root)', 'kind': 'mono'},
                    {'key': 'status', 'label': 'Status', 'kind': 'text'}, {'key': 'bytes', 'label': 'Bytes', 'fmt': 'int'},
                    {'key': 'sha', 'label': 'SHA-256', 'kind': 'mono'}], rows, sortable=True, tid='t-appendix-files',
                   num=None, caption=f'Every input file the generator touched ({len(rows)}), with its SHA-256 as read.')


def blk_files(ctx, spec) -> str:
    return FILES_MARKER


BLOCKS = {'prose': blk_prose, 'h3': blk_h3, 'callout': blk_callout, 'glossary': blk_glossary, 't_facts': blk_facts,
          't_risks': blk_risks, 't_cellname': blk_cellname, 'fig_flow': blk_flow, 't_fields': blk_fields,
          'universe': blk_universe, 't_library': blk_library, 'fig_ic_panel': blk_ic_panel, 'fig_decay': blk_decay,
          'fig_tau_ic': blk_tau_ic, 'fig_theme_year': blk_theme_year, 't_changes': blk_changes, 't_rules': blk_rules,
          'fig_corr': blk_corr, 't_theme_corr': blk_theme_corr_table, 't_steps': blk_steps, 'fig_exposure': blk_exposure,
          'fig_fills': blk_fills, 't_cost_model': blk_cost_model, 't_financing': blk_financing, 'costdec': blk_costdec,
          't_attrib': blk_attrib, 'capacity': blk_capacity, 't_drawdowns': blk_drawdowns, 't_retstats': blk_retstats,
          't_monitor': blk_monitor, 't_files': blk_files}

# pitch iteration 3 (platform v7): capacity curve, risk-model bias, report cards, monitor baseline, operating loop,
# integrity statistics, trial ledger -- config-driven blocks and analyses in pitch3.py
from . import pitch3 as _P3  # noqa: E402

BLOCKS.update(_P3.BLOCKS)
ANALYSES.update(_P3.ANALYSES)

# platform v8: year tables, cell ladder, cumulative test and freeze gate, diagnostics G-1..G-3, traded-horizon member
# columns, Appendix A trial accounting, OD-1 disclosure, literature contradictions -- config key ``v8``, in v8.py.
# New names only: a config without v8_* blocks renders exactly as before.
from . import v8 as _V8  # noqa: E402

BLOCKS.update(_V8.BLOCKS)
ANALYSES.update(_V8.ANALYSES)
