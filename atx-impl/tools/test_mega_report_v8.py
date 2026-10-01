"""Synthetic checks for the platform v8 sections of mega_report (v8.py): year tables, cell ladder, cumulative test and
freeze gate, diagnostics G-1..G-3, traded-horizon member columns, Appendix A trial accounting, OD-1 disclosure and the
literature contradictions; the content seal; and the v7 identity (a config without v8 blocks renders byte for byte as
before the v8 module existed).

``world(root, cfg)`` writes a synthetic input for every path ``v8.inputs(cfg)`` names (reused by
test_mega_report_v8_render.py on the v8 pitch config). Temp roots are short (Windows 260-character path limit).

Run: python -m pytest atx-impl/tools/test_mega_report_v8.py -q
"""
import copy
import datetime as dt
import hashlib
import html as htmllib
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import backtest_integrity as BI  # noqa: E402
import test_mega_report_pitch3 as T3  # noqa: E402
from engine_tools import research_window as RW  # noqa: E402
from mega_report import pitch as P  # noqa: E402
from mega_report import pitch3 as P3  # noqa: E402
from mega_report import report as R  # noqa: E402
from mega_report import v8 as V  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
PREREG = REPO / '.superpowers/sdd/platform-v8-20260929/v8-prereg.md'
LITERATURE = REPO / '.superpowers/sdd/platform-v8-20260929/reports/Equity long short alpha v8.md'
# the pitch3 synthetic world rendered with the v7 config shape, before this module existed (checkout ec47b8ce):
# the normalised HTML's SHA-256 and length. A config without v8 blocks must keep rendering these bytes.
V7_GOLDEN_SHA256 = 'c42c5fad23dfc6a630708ba52aa210cbd2e53bba94be92d3f36587cb42d21c69'
V7_GOLDEN_BYTES = 109868
NS_PER_DAY = 86_400_000_000_000


# ============================================================================================ synthetic world
def put(root: Path, rel: str, obj) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(obj if isinstance(obj, bytes) else (obj if isinstance(obj, str) else json.dumps(obj, indent=1)).encode())


def session_ns(y: int, m: int, d: int) -> int:
    return (dt.date(y, m, d) - dt.date(1970, 1, 1)).days * NS_PER_DAY


def year_table(i: int, years=None) -> list[dict]:
    return [{'year': y, 'return_rows': 250, 'net_return': 0.04 + 0.001 * i, 'net_sharpe': 0.8 + 0.1 * k + 0.01 * i,
             'ann_vol': 0.045, 'tau_gmv_mean': 0.040 - 0.002 * i, 'cost_bps_traded': 13.0 - 0.3 * i}
            for k, y in enumerate(years or V.train_years())]


def summ_rows(cfg: dict) -> list[dict]:
    """One nav_summ row per configured cell, in order: each cell's turnover 20% below the one before it and its cost
    lower, so every turnover and cost criterion of the committed config holds (R-4's "15% lower" included)."""
    rows = []
    for i, c in enumerate(cfg['v8']['cells']):
        rows.append({'dir': c['dir'].replace('/', '\\'), 'net_sharpe': 0.95 + 0.05 * i, 'gross_sharpe': 1.3,
                     'mean_gross_leverage_all_rows': 0.97, 'mean_net_leverage_all_rows': 0.004,
                     'tau_gmv_mean': 0.040 * 0.8 ** i, 'tau_gmv_p95': 0.05, 'cost_bps_traded': 13.0 - 0.3 * i,
                     'deflated_ledger': {'n': 37 + len(cfg['v8']['cells']), 'dsr': 0.96, 'legacy_dsr': 0.9},
                     'year_table': year_table(i)})
    return rows


def bundle_doc(final_dir: str, base_dir: str, dsr: float = 0.05, p1: float = 0.2, years=None) -> dict:
    ys = years or V.train_years()
    return {'base': base_dir, 'final': final_dir, 'protocol': 'v8',
            'scenarios': {'base': 'modeled-1bn-stale5-v1+swap-fin-v1', 'final': 'modeled-1bn-stale5-v1+swap-fin-v1'},
            'paired': {'sessions': 1004, 'sr': 1.1, 'sr_reference': 1.1 - dsr, 'dsr': dsr, 'rho': 0.97,
                       'memmel_se': 0.1, 't': dsr / 0.1, 'cbb_ci95': [dsr - 0.2, dsr + 0.2], 'cbb_valid_draws': 4999,
                       'lw': {'dsr_population': dsr, 'se': 0.1, 'ci95': [dsr - 0.21, dsr + 0.21], 'p_value': 2 * p1,
                              'p_one_sided': p1, 'valid_draws': 4999},
                       'draws': 4999, 'block': 21, 'seed': 20260929},
            'years': [{'year': y, 'sessions': 251, 'sr_final': 1.0, 'sr_base': 1.0 - dsr, 'dsr': dsr} for y in ys],
            'year_table': {'base': year_table(0, ys), 'final': year_table(1, ys)},
            'verdict': {'dsr_positive': dsr > 0, 'p_one_sided': p1, 'alpha': 0.1, 'pass': dsr > 0 and p1 < 0.1,
                        'rule': 'cumulative paired S2 net dSR > 0 and one-sided p < alpha'}}


def nav_summary(cfg: dict, s3: float = 0.9) -> dict:
    """A synthetic NAV summary.json of a cell: every configured scenario's net Sharpe (S3's ``s3``) with its TRAIN
    calendar years, and the spo-v3 report of the primary book (tripwire clear, limits met on every scored decision,
    traded-book aim correlation .95 after decision d's trades, Ruling E-14a), as the criteria of R-5 and R-6 read them."""
    primary = next(s['id'] for s in cfg['scenarios'] if s['key'] == cfg['primary_scenario'])
    scen = [{'scenario': s['id'], 'net_sharpe': s3 if s['key'] == 'S3' else 1.1,
             'calendar_year_returns': [{'year': y, 'net_compounded_return': 0.05} for y in V.train_years()]}
            for s in cfg['scenarios']]
    book = {'decisions': 1004, 'unconverged': 0, 'limits_unmet': 0,
            'aim_correlation_traded_after': {'mean': 0.95, 'min': 0.91, 'n': 1003},
            'aim_correlation_criterion': {'reads': 'aim_correlation_traded_after.mean', 'threshold': 0.9,
                                          'value': 0.95, 'met': True}}
    return {'primary_scenario': primary, 'scenarios': scen,
            'v7': {'spo_v3_books': {primary: book}, 'spo_v3_tripwire': {'status': 'clear', 'report_only': {primary: book}}}}


def diagnostics_doc(sealed: bool = False) -> dict:
    ses = [session_ns(2023, 6, 1), session_ns(2024 if sealed else 2023, 6, 2)]
    members = [{'id': 'bm', 'theme': 'value', 'weight': 0.05, 'sign': 1, 'ic1': 0.01, 'ic21': 0.02, 'ic_theta': 0.009,
                'ic_theta_source': 'card', 'retention': 0.9, 'f_theta': 1.2e-5, 'f_theta_hac_t': 1.1,
                'marginal_ic21': 0.004, 'marginal_hac_t': 0.8, 'k6_ic21': 0.02, 'max_abs_rho': 0.61,
                'max_rho_member': 'ep', 'contribution': 0.00045, 'contribution_share_abs': 0.3},
               {'id': 'smax', 'theme': 'low_risk', 'weight': 0.1, 'sign': 1, 'ic1': 0.02, 'ic21': 0.01, 'ic_theta': -0.002,
                'retention': 0.1, 'f_theta': None, 'f_theta_hac_t': None, 'marginal_ic21': -0.001,
                'marginal_hac_t': -0.4, 'max_abs_rho': None, 'max_rho_member': None}]

    def ok(result):
        return {'status': 'ok', 'inputs': {'cards': {'path': 'build-equity/mega-cards-v8-b0c'}}, 'method': 'synthetic',
                'result': result}
    diag = {'G-1a': ok({'members': members, 'weighted_ic_theta': 0.0021, 'negative_at_theta': ['smax'],
                        'negative_at_theta_weight': 0.1, 'negative_marginal_t': ['smax'], 'unscored': []}),
            'G-1b': ok({'combined_turnover': {'model': 0.031, 'book': 0.038}, 'fast': {'ids': ['a', 'b'], 'weight': 0.2,
                                                                                      'own_share': 0.7}}),
            'G-1c': ok({'themes_model': {'ratio': 0.42}, 'themes_book': {'ratio': 0.47}, 'members_model': {'ratio': 0.3}}),
            'G-2a': ok({'mean_share': {'market': 0.01, 'industry': 0.2, 'style': 0.3, 'specific': 0.49},
                        'mean_ex_ante_vol_annual': 0.04}),
            'G-2b': {'status': 'skipped', 'reason': 'no --u-pass given', 'inputs': {}, 'method': 'x', 'result': None},
            'G-2c': ok({'held': {'p95': 0.02, 'max': 0.3, 'share_above_q': 0.001}, 'aim': {'p95': 0.03}}),
            'G-3a': ok({'net_sharpe': 1.1, 'restated_net_sharpe': 1.02, 'delta': -0.08,
                        'series': {'session_ns': ses, 's2_fee_return': [0.0, 0.0]}}),
            'G-3b': ok({'members': {'bac': {'retained': 0.6}, 'smax': {'retained': 0.4}}}),
            'G-3c': ok({'delays': {'1': {'delta': -0.05, 'memmel_se': 0.02}, '2': {'delta': -0.09, 'memmel_se': 0.03}}}),
            'G-3d': ok({'adjusted_rand_index': 0.35, 'effective_bets_themes': 6.2, 'mean_rho_within_theme': 0.3})}
    for gid in diag:
        diag[gid]['question'] = f'question of {gid}'
    return {'schema': 'atx.book-diagnostics/v1', 'declaration': 'descriptive only (v8-prereg rule 8)',
            'window_id': RW.WINDOW_ID, 'tool': {'script': 'atx-impl/tools/book_diagnostics.py', 'script_sha256': 'ab' * 32},
            'diagnostics': diag}


def card_index() -> dict:
    return {'schema': 'atx.alpha-report-card-index/v1', 'ranking': 'fitness',
            'candidates': [{'id': 'bm', 'theme': 'value', 'status': 'admitted', 'ic_theta': 0.009, 'marginal_ic21': 0.004},
                           {'id': 'smax', 'theme': 'low_risk', 'status': 'admitted', 'ic_theta': -0.002}]}


def admission() -> dict:
    return {'schema': 'atx.dsl-admission/v1', 'candidates': [{'id': 'bm', 'status': 'admitted', 'f_theta': 1.2e-5,
                                                               'f_theta_hac_t': 1.1},
                                                              {'id': 'ear', 'status': 'reject_veto', 'f_theta': -3e-6,
                                                               'f_theta_hac_t': -2.2}]}


def write_ledger(path: Path, cfg: dict) -> None:
    def rec(i, kind, count=1, v8=True, cell=None):
        r = {'schema': BI.LEDGER_SCHEMA, 'kind': kind, 'count': count, 'trial_id': f'{i:016x}',
             'cell': cell or f'build-equity/mega-nav-legacy-{i}', 's2_net_sr': 1.0}
        if v8:
            r['window_id'] = RW.WINDOW_ID
        return r
    recs = [rec(1, 'construction', 37, v8=False), rec(2, 'protocol', 0)]
    cells = V.defined_cells(cfg['v8']['cells'], V.verdict_rules_of(cfg))   # an undefined cell adds 0: no line
    recs += [rec(10 + i, 'construction', cell=c['dir']) for i, c in enumerate(cells)]
    recs.append(rec(99, 'admission', 7))
    path.parent.mkdir(parents=True, exist_ok=True)
    BI.ledger_append(path, recs, chain=True)


PREREG_TEXT = ('# v8 pre-registration (synthetic)\n\n1. Window. TRAIN is [2020-01-01, 2024-01-01). Hidden: 2024-01-01 '
               'and later.\n   Disclosure: 2023 and 2024 were read twice at book level as validation.\n'
               '2. Trial count. N continues from 37.\n')
LITERATURE_TEXT = ('# notes\n\n## Where the new notes contradict or update v6 and v7\n\nIntro.\n\n'
                   '| Earlier position | New evidence in the v8 notes | Verdict |\n|---|---|---|\n'
                   '| v6 3.1: latest announcement return | insignificant ([Gerard-Jehl 2025](https://example.org/a)) | '
                   'Contradicts |\n| v7 S2: band width | cost^(1/2) | Updates (qualifies) |\n'
                   '| v6 lever R: borrow fees | about 0.12 | Confirms and quantifies |\n| v7 S6: insider buying | weak | '
                   'Flag only |\n\n## Conclusion\n')


def world(root: Path, cfg: dict, skip: tuple = ()) -> None:
    """A synthetic input at every path the config's v8 key names (``skip``: paths left out). A cell whose recorded
    verdict is a reject gets a negative paired dSR (the rule of v8-prereg item 5 then rejects it too)."""
    v8 = cfg['v8']
    by_key = {c['key']: c for c in v8['cells']}
    states = V.cell_states(v8['cells'], V.verdict_rules_of(cfg))
    writers: dict = {'v8.summ': lambda: summ_rows(cfg), 'v8.bundle': lambda: bundle_doc(
                   by_key[v8['final']]['dir'], by_key[v8['base']]['dir'], dsr=0.25, p1=0.04),
               'v8.diagnostics': diagnostics_doc, 'v8.member_horizon.card_index': card_index,
               'v8.member_horizon.admission': admission, 'v8.prereg': lambda: PREREG_TEXT,
               'v8.literature': lambda: LITERATURE_TEXT}
    for block, key, rel in V.inputs(cfg):
        if rel in skip:
            continue
        if key == 'v8.trial_ledger':
            write_ledger(root / rel, cfg)
            continue
        if re.fullmatch(r'v8\.cells\[(.+)\]\.summary', key):
            put(root, rel, nav_summary(cfg))
            continue
        m = re.fullmatch(r'v8\.cells\[(.+)\]\.paired', key)
        if m:
            c = by_key[m.group(1)]
            dsr = -0.02 if states[c['key']]['kind'] == 'rejected' else 0.05
            put(root, rel, bundle_doc(c['dir'], by_key[c['parent']]['dir'], dsr=dsr))
            continue
        put(root, rel, writers[key]())


# a small v8 config: a baseline, one accepted step (the final cell), the gate on it
MECH = [{'label': 'Gross leverage, mean over all rows', 'metric': 'mean_gross_leverage_all_rows', 'op': 'between',
         'lo': 0.9, 'hi': 1.05, 'fmt': '.4f', 'threshold_fmt': '.2f'},
        {'label': 'Net leverage, mean over all rows', 'metric': 'mean_net_leverage_all_rows', 'op': 'abs_le',
         'value': 0.02, 'fmt': '+.4f', 'threshold_fmt': '.2f'}]
BASE_CFG: dict = {
    'schema': 'atx.mega-report-config/v2', 'title': 'v8 test', 'sessions_per_year': 252, 'cell_prefix': 'mega-nav-',
    'scenarios': [{'key': 'S2', 'id': 'modeled-1bn-stale5-v1+swap-fin-v1', 'label': 'S2', 'short': 'S2'}],
    'primary_scenario': 'S2', 'verdict_badges': {'rules': []},
    'v8': {
        'summ': 'b/summ.json', 'bundle': 'b/bundle.json', 'base': 'B0c', 'final': 'R-1',
        'cells': [{'key': 'B0c', 'label': 'B0c baseline', 'dir': 'b/mega-nav-v8-b0c', 'n': 40,
                   'criterion': {'text': 'none (baseline by declaration)'}, 'verdict': 'accepted (baseline)'},
                  {'key': 'R-1', 'label': 'R-1 composition v8', 'dir': 'b/mega-nav-v8-r1', 'parent': 'B0c', 'n': 41,
                   'paired': 'b/paired-r1.json', 'verdict': 'ACCEPTED',
                   'criterion': {'text': 'turnover per unit gross not higher',
                                 'checks': [{'metric': 'tau_gmv_mean', 'per': 'mean_gross_leverage_all_rows',
                                             'op': 'le'}]}}],
        'mechanics': MECH,
        'freeze_gate': {'name': 'Freeze gate', 'alpha': 0.1, 'unmet_note': 'unmet: OD-3 is the lever',
                        'checks': [{'label': 'Net Sharpe, S2', 'metric': 'net_sharpe', 'op': 'ge', 'value': 1.0,
                                    'fmt': '+.3f'}, *MECH, {'builtin': 'paired_dsr'}, {'builtin': 'paired_p'},
                                   {'label': 'Deflated Sharpe, cell count', 'metric': 'deflated_ledger.dsr', 'op': 'ge',
                                    'value': 0.95}]},
        'diagnostics': 'b/diag/diagnostics-v8.json',
        'member_horizon': {'card_index': 'b/cards/index.json', 'admission': 'b/w/admission.json'},
        'trial_ledger': 'b/trials.jsonl', 'budget': {'n': 51, 'admission': 15},
        'prereg': '.superpowers/sdd/s-20260929/v8-prereg.md',
        'literature': {'path': '.superpowers/sdd/s-20260929/lit.md'},
        'od1_notes': ['2024 stays hidden but is not pristine.']},
    'layout': [{'id': 'sum', 'title': 'Summary', 'blocks': [
                   {'type': 'prose', 'paragraphs': ['dSR {a:v8_bundle.paired.dsr|+.3f}; N {a:v8_ledger.n|d}; TRAIN '
                                                    '{a:v8_bundle.final_row.net_sharpe|+.3f}']}, 'v8_bundle']},
               {'id': 'cells', 'title': 'Cells', 'blocks': ['v8_ladder', 'v8_year_table']},
               {'id': 'diag', 'title': 'Diagnostics', 'blocks': ['v8_diagnostics', 'v8_member_horizon']},
               {'id': 'prot', 'title': 'Protocol', 'blocks': ['v8_od1', 'v8_trial_accounting', 'v8_contradictions',
                                                               't_files']}]}


@pytest.fixture()
def root():
    d = Path(tempfile.mkdtemp(prefix='v8r'))
    try:
        yield d
    finally:
        shutil.rmtree(d, ignore_errors=True)


def cfg_for(**v8_over) -> dict:
    cfg = copy.deepcopy(BASE_CFG)
    cfg['root'] = '.'
    cfg['v8'].update(v8_over)
    return cfg


def build(root: Path, cfg: dict) -> str:
    p = root / 'cfg.json'
    p.write_text(json.dumps(cfg), encoding='utf-8')
    return R.build(p, stamp='test')


def make_ctx(root: Path, cfg: dict):
    return R.Ctx(cfg, json.dumps(cfg).encode(), root / 'cfg.json', root, 'test')


def unavailable(html_text: str) -> list[tuple[str, str]]:
    """(block, message) of every unavailable block, the message unescaped."""
    return [(n, htmllib.unescape(m)) for n, m in T3.unavailable(html_text)]


def section(html: str, sec_id: str) -> str:
    return T3.section(html, sec_id)


# ============================================================================================ registration, v7 identity
def test_blocks_and_analyses_are_registered_under_new_names():
    for name, fn in V.BLOCKS.items():
        assert name.startswith('v8_') and P.BLOCKS[name] is fn
        assert name not in P3.BLOCKS and name not in R.BLOCKS
    for name, fn in V.ANALYSES.items():
        assert name.startswith('v8_') and P.ANALYSES[name] is fn and name not in P3.ANALYSES


def _v7_render(root: Path) -> str:
    T3.world(root)
    cfg = copy.deepcopy(T3.BASE_CFG)
    cfg['root'] = '.'
    return build(root, cfg)


def _normalise(html: str, root: Path) -> str:
    html = re.sub(r'mega_report @ [0-9a-f]+', 'mega_report @ HEAD', html)
    return html.replace(root.resolve().as_posix(), 'ROOT')


def test_v7_config_renders_the_pre_v8_bytes(root):
    """The v7 config shape (pitch3's synthetic world) renders exactly the bytes it rendered before v8.py existed; only
    the generator's git head and the temp root are normalised."""
    html = _v7_render(root)
    assert len(html) == V7_GOLDEN_BYTES
    assert hashlib.sha256(_normalise(html, root).encode('utf-8')).hexdigest() == V7_GOLDEN_SHA256
    assert 'v8_' not in html and 't-v8-' not in html


def test_v7_render_is_unchanged_by_the_v8_registration(root, monkeypatch):
    with_v8 = _v7_render(root)
    for name in V.BLOCKS:
        monkeypatch.delitem(P.BLOCKS, name)
    for name in V.ANALYSES:
        monkeypatch.delitem(P.ANALYSES, name)
    assert build(root, dict(copy.deepcopy(T3.BASE_CFG), root='.')) == with_v8


# ============================================================================================ components: None
@pytest.mark.parametrize('fn,block', [(V.render_year_table, 'v8_year_table'), (V.render_year_matrix, 'v8_year_table'),
                                      (V.render_cell_ladder, 'v8_ladder'), (V.render_bundle_verdict, 'v8_bundle'),
                                      (V.render_diagnostics, 'v8_diagnostics'),
                                      (V.render_member_horizon, 'v8_member_horizon'),
                                      (V.render_trial_accounting, 'v8_trial_accounting'), (V.render_od1, 'v8_od1'),
                                      (V.render_contradictions, 'v8_contradictions')])
def test_none_renders_one_unavailable_block_naming_the_path(fn, block):
    html = fn(None, 'build-equity/x/y.json')
    assert unavailable(html) == [(block, f'{block}: not available (build-equity/x/y.json: missing)')]


# ============================================================================================ year table
def test_year_rows_cover_train_and_refuse_a_sealed_year():
    rows = V.year_rows([r for r in year_table(0) if r['year'] != 2021])
    assert [r['year'] for r in rows] == ['2020', '2021', '2022', '2023'] and rows[1] == {'year': '2021'}
    html = V.render_year_table(year_table(0), 'x.json', label='B0c')
    assert html.count('<tr>') == 5 and '+0.800' in html and 'Source: x.json' in html
    with pytest.raises(ValueError, match='sealed'):
        V.year_rows(year_table(0, [2022, 2023, 2024]))
    with pytest.raises(ValueError, match='nav_summ year_table'):
        V.year_rows({'year': 2020})


def test_year_table_block_matrix_details_and_absent_rows(root):
    cfg = cfg_for()
    world(root, cfg)
    rows = summ_rows(cfg)
    del rows[1]['year_table']
    put(root, 'b/summ.json', rows)
    html = build(root, cfg)
    sec = section(html, 'cells')
    assert 'id="t-v8-year-matrix"' in sec and 'id="t-v8-year-B0c"' in sec and 'id="t-v8-year-R-1"' not in sec
    assert 'R-1: its nav_summ row has no year_table' in sec and unavailable(html) == []


# ============================================================================================ paired tests, ladder
def test_paired_of_checks_names_shape_and_seal():
    doc = bundle_doc('b\\mega-nav-v8-r1', 'b/mega-nav-v8-b0c')
    pd = V.paired_of(doc, 'mega-nav-v8-r1', 'mega-nav-v8-b0c')
    assert pd['dsr'] == 0.05 and pd['p1'] == 0.2 and pd['registered'] is True
    with pytest.raises(ValueError, match="bundle final is 'mega-nav-v8-r1', expected 'other'"):
        V.paired_of(doc, 'other')
    with pytest.raises(ValueError, match='not a nav_summ --bundle'):
        V.paired_of({'paired': {}})
    with pytest.raises(ValueError, match='sealed'):
        V.paired_of(bundle_doc('a', 'b', years=[2023, 2024]))
    doc['paired']['seed'] = 20260927
    assert V.paired_of(doc)['registered'] is False


def test_criterion_eval_ops_per_and_factor():
    row, par = {'tau': 0.030, 'g': 1.0, 'c': 12.0}, {'tau': 0.040, 'g': 1.0, 'c': 12.5}
    assert V.criterion_eval({'checks': [{'metric': 'tau', 'per': 'g', 'op': 'le'}]}, row, par)['met'] is True
    assert V.criterion_eval({'checks': [{'metric': 'tau', 'op': 'le', 'factor': 0.85}]}, row, par)['met'] is True
    assert V.criterion_eval({'checks': [{'metric': 'tau', 'op': 'le', 'factor': 0.7}]}, row, par)['met'] is False
    assert V.criterion_eval({'checks': [{'metric': 'c', 'op': 'le'}]}, row, None)['met'] is None
    assert V.criterion_eval({'text': 'capacity at 4x', 'met': True}, None, None) == {
        'text': 'capacity at 4x', 'met': True, 'detail': 'as configured', 'unread': [], 'na': []}
    assert V.criterion_eval(None, row, par)['met'] is None
    mixed = {'checks': [{'metric': 'tau', 'op': 'lt'}, {'text': 'net at 2x not lower', 'met': None}]}
    ce = V.criterion_eval(mixed, row, par)
    assert ce['met'] is None and 'net at 2x not lower: to be read (config met)' in ce['detail']
    assert ce['unread'] == ['net at 2x not lower']
    mixed['checks'][1]['met'] = True
    assert V.criterion_eval(mixed, row, par)['met'] is True
    mixed['checks'][1]['met'] = False
    assert V.criterion_eval(mixed, row, par)['met'] is False
    with pytest.raises(ValueError, match='unknown op'):
        V.criterion_eval({'checks': [{'metric': 'tau', 'op': 'eq'}]}, row, par)  # eq only against a fixed value


# review P-2: the registered parts the nav_summ row does not carry are read from the cell's NAV summary.json
PRIMARY_ID = 'modeled.1bn+swap-fin-v1'  # an id holding a dot: {primary} is one key


def spo_summary(s3: float = 0.9, status: str = 'clear', unmet: int = 0, corr: float = 0.95) -> dict:
    book = {'limits_unmet': unmet, 'aim_correlation_traded_after': {'mean': corr},
            'aim_correlation_criterion': {'value': corr, 'met': corr >= 0.9}}
    return {'scenarios': [{'scenario': PRIMARY_ID, 'net_sharpe': 1.1}, {'scenario': 's3.id', 'net_sharpe': s3}],
            'v7': {'spo_v3_tripwire': {'status': status}, 'spo_v3_books': {PRIMARY_ID: book}}}


SPO_DOCS = {'scenarios': {'S2': PRIMARY_ID, 'S3': 's3.id'}, 'primary': PRIMARY_ID}
R6_CHECKS = {'checks': [
    {'source': 'summary', 'metric': 'v7.spo_v3_tripwire.status', 'op': 'eq', 'value': 'clear'},
    {'source': 'summary', 'metric': 'v7.spo_v3_books.{primary}.limits_unmet', 'op': 'eq', 'value': 0},
    {'source': 'summary', 'metric': 'v7.spo_v3_books.{primary}.aim_correlation_traded_after.mean', 'op': 'ge',
     'value': 0.9}]}


def test_criterion_eval_summary_checks_against_a_value():
    """R-6's tripwire, E-31 limits and E-14 correlation: fixed thresholds on the cell's own summary (no parent read)."""
    ok = V.criterion_eval(R6_CHECKS, None, None, dict(SPO_DOCS, cell=spo_summary()))
    assert ok['met'] is True and ok['unread'] == []
    assert 'v7.spo_v3_tripwire.status clear eq clear' in ok['detail']
    assert 'v7.spo_v3_books.{primary}.aim_correlation_traded_after.mean 0.95 ge 0.9' in ok['detail']
    for bad in (spo_summary(status='tripped (not voiding: --specific-ceiling-void off)'), spo_summary(unmet=2),
                spo_summary(corr=0.85)):
        assert V.criterion_eval(R6_CHECKS, None, None, dict(SPO_DOCS, cell=bad))['met'] is False
    assert V.criterion_eval(R6_CHECKS, None, None, dict(SPO_DOCS, cell=None))['met'] is None  # summary not read
    assert V.criterion_eval(R6_CHECKS, None, None, dict(SPO_DOCS, cell=spo_summary(), primary='S9'))['met'] is None
    with pytest.raises(ValueError, match='unknown source'):
        V.criterion_eval({'checks': [{'source': 'card', 'metric': 'x', 'op': 'le'}]}, None, None)
    with pytest.raises(ValueError, match='unknown op'):
        V.criterion_eval({'checks': [{'metric': 'x', 'op': 'between', 'value': 1}]}, None, None)


def test_criterion_eval_summary_scenario_against_the_parent():
    """R-5's "S3 not lower": a cost scenario's net Sharpe in the cell's summary against the parent's."""
    s3 = {'checks': [{'label': 'S3 net Sharpe', 'source': 'summary', 'scenario': 'S3', 'metric': 'net_sharpe',
                      'op': 'ge'}]}
    equal = V.criterion_eval(s3, None, None, dict(SPO_DOCS, cell=spo_summary(0.8), parent=spo_summary(0.8)))
    assert equal['met'] is True and equal['detail'] == 'S3 net Sharpe 0.8 ge parent 0.8'
    assert V.criterion_eval(s3, None, None, dict(SPO_DOCS, cell=spo_summary(0.7), parent=spo_summary(0.8)))['met'] \
        is False
    assert V.criterion_eval(s3, None, None, dict(SPO_DOCS, cell=spo_summary(0.8)))['met'] is None  # parent not read
    other = dict(SPO_DOCS, scenarios={'S3': 'not-a-scenario'})
    assert V.criterion_eval(s3, None, None, dict(other, cell=spo_summary(), parent=spo_summary()))['met'] is None


def summary_cfg() -> dict:
    """cfg_for() with an R-1 criterion that reads the NAV summaries (R-5 / R-6 style)."""
    cfg = cfg_for()
    cfg['v8']['cells'][1]['criterion'] = {'text': 'cost not higher; tripwire clear; S2 not lower', 'checks': [
        {'metric': 'cost_bps_traded', 'op': 'le'},
        {'source': 'summary', 'metric': 'v7.spo_v3_tripwire.status', 'op': 'eq', 'value': 'clear'},
        {'source': 'summary', 'scenario': 'S2', 'metric': 'net_sharpe', 'op': 'ge'}]}
    return cfg


def test_ladder_reads_the_nav_summaries_its_criteria_name(root):
    cfg = summary_cfg()
    assert [k for _, k, _ in V.inputs(cfg)][:4] == ['v8.summ', 'v8.cells[B0c].summary', 'v8.cells[R-1].paired',
                                                    'v8.cells[R-1].summary']
    world(root, cfg)
    rows, unav, _, srcs = V.ladder_rows(make_ctx(root, cfg))
    assert unav == [] and rows[1]['crit_met'] is True and rows[1]['rule'] is True
    assert any(s.startswith('b/mega-nav-v8-r1/summary.json (sha256 ') for s in srcs)
    doc = nav_summary(cfg)
    doc['v7']['spo_v3_tripwire']['status'] = 'tripped (not voiding: --specific-ceiling-void off)'
    put(root, 'b/mega-nav-v8-r1/summary.json', doc)
    rows = V.ladder_rows(make_ctx(root, cfg))[0]
    assert rows[1]['crit_met'] is False and rows[1]['rule'] is False
    (root / 'b/mega-nav-v8-b0c/summary.json').unlink()  # the parent's: one unavailable block, the part n/a
    rows, unav, _, _ = V.ladder_rows(make_ctx(root, cfg))
    assert len(unav) == 1 and 'b/mega-nav-v8-b0c/summary.json' in unav[0] and rows[1]['crit_met'] is None
    sealed = nav_summary(cfg)
    sealed['scenarios'][0]['calendar_year_returns'].append({'year': 2024, 'net_compounded_return': 0.01})
    put(root, 'b/mega-nav-v8-b0c/summary.json', sealed)
    unav = V.ladder_rows(make_ctx(root, cfg))[1]
    assert len(unav) == 1 and 'sealed' in unav[0]
    world(root, cfg)
    html = build(root, cfg)
    assert unavailable(html) == []
    assert 'v7.spo_v3_tripwire.status clear eq clear' in htmllib.unescape(section(html, 'cells'))


def test_ladder_rows_rule_and_verdicts(root):
    cfg = cfg_for()
    world(root, cfg)
    rows, unav, notes, srcs = V.ladder_rows(make_ctx(root, cfg))
    b0c, r1 = rows
    assert unav == [] and notes == [] and len(srcs) == 2
    assert b0c['dsr'] == '' and b0c['rule'] == '' and b0c['mech'] is True and b0c['parent'] == 'none (baseline)'
    assert r1['dsr'] == 0.05 and r1['p1'] == 0.2 and r1['pos'] is True and r1['mech'] is True
    assert r1['crit_met'] is True and r1['rule'] is True and r1['n'] == 41 and r1['parent'] == 'B0c baseline'
    html = build(root, cfg)
    sec = section(html, 'cells')
    assert 'id="t-v8-ladder"' in sec and '<span class="chip pass">ACCEPTED</span>' in sec


def test_ladder_missing_paired_input_is_one_block_and_summ_is_borrowed(root):
    cfg = cfg_for()
    world(root, cfg, skip=('b/paired-r1.json', 'b/summ.json'))
    html = build(root, cfg)
    na = unavailable(html)
    assert sorted(n for n, _ in na) == ['v8_ladder', 'v8_year_table']  # summ's owner is the year table, not the ladder
    assert 'b/paired-r1.json' in dict(na)['v8_ladder'] and 'b/summ.json' in dict(na)['v8_year_table']
    assert 'mechanics and the metric criteria are n/a' in section(html, 'cells')


def test_ladder_paired_file_of_another_cell_is_refused(root):
    cfg = cfg_for()
    world(root, cfg)
    put(root, 'b/paired-r1.json', bundle_doc('b/mega-nav-v8-r9', 'b/mega-nav-v8-b0c'))
    na = dict(unavailable(build(root, cfg)))
    assert "bundle final is 'mega-nav-v8-r9', expected 'mega-nav-v8-r1'" in na['v8_ladder']


def _checks(root: Path, cfg: dict) -> list[tuple[str, str]]:
    ctx = make_ctx(root, cfg)
    return V.ladder_checks(ctx, V.ladder_rows(ctx)[0])


def test_ladder_checks_final_must_be_the_last_accepted_cell(root):
    cfg = cfg_for()
    world(root, cfg)
    assert _checks(root, cfg) == []
    assert _checks(root, cfg_for(final='B0c')) == [('v8.final', "'B0c' is not the last accepted cell of the ladder "
                                                                "('R-1')")]
    kind = cfg_for()
    kind['v8']['cells'][1].update(verdict='kept', verdict_kind='accepted')  # an explicit kind overrides the text
    assert _checks(root, kind) == []
    rej = cfg_for()
    rej['v8']['cells'][1]['verdict'] = 'REJECTED (dSR < 0)'
    put(root, 'b/paired-r1.json', bundle_doc('b/mega-nav-v8-r1', 'b/mega-nav-v8-b0c', dsr=-0.02))  # the rule rejects
    assert _checks(root, rej) == [('v8.final', "'R-1' is not the last accepted cell of the ladder ('B0c')")]


# review P-3: the recorded verdict against the rule of v8-prereg item 5, and the parent against the ladder
def test_ladder_checks_refuse_a_verdict_the_rule_contradicts(root):
    cfg = cfg_for()
    world(root, cfg)
    assert _checks(root, cfg) == []
    # an accept the rule rejects (dSR < 0)
    put(root, 'b/paired-r1.json', bundle_doc('b/mega-nav-v8-r1', 'b/mega-nav-v8-b0c', dsr=-0.02))
    assert _checks(root, cfg) == [
        ('v8.cells[R-1].verdict', "recorded accepted ('ACCEPTED') but the rule of v8-prereg item 5 rejects it "
                                  "(dSR > 0 no, mechanics yes, criterion yes)")]
    # a reject the rule accepts (dSR > 0, mechanics, criterion); v8.final then has no accepted R-1
    world(root, cfg)
    rej = cfg_for()
    rej['v8']['cells'][1]['verdict'] = 'REJECTED'
    assert _checks(root, rej) == [
        ('v8.cells[R-1].verdict', "recorded rejected ('REJECTED') but the rule of v8-prereg item 5 accepts it "
                                  "(dSR > 0 yes, mechanics yes, criterion yes)"),
        ('v8.final', "'R-1' is not the last accepted cell of the ladder ('B0c')")]
    # an accept whose rule is n/a because a manual criterion part is unread; read, it passes
    unread = cfg_for()
    unread['v8']['cells'][1]['criterion']['checks'].append({'text': 'net at 4x higher', 'met': None})
    assert _checks(root, unread) == [
        ('v8.cells[R-1].verdict', "recorded accepted ('ACCEPTED') but its criterion is not read: net at 4x higher "
                                  "(set the part's met)")]
    unread['v8']['cells'][1]['criterion']['checks'][-1]['met'] = True
    assert _checks(root, unread) == []
    unread['v8']['cells'][1]['criterion']['checks'][-1]['met'] = False
    assert [w for w, _ in _checks(root, unread)] == ['v8.cells[R-1].verdict']  # the rule rejects
    # an accept whose rule is n/a for a missing input: that input's own unavailable block, no refusal here
    (root / 'b/paired-r1.json').unlink()
    assert _checks(root, cfg) == []


NA_HEAD = "recorded accepted ('ACCEPTED') but the rule of v8-prereg item 5 is n/a: "


def test_an_accept_whose_rule_is_na_is_refused_naming_the_part(root):
    """Ruling PM5-12, finding R6C-1: an accepted rule-bearing cell whose rule is n/a for an input with no unavailable
    block of its own is refused, naming the part (its nav_summ row or its parent's, a metric of either, a mechanics
    metric, no paired test configured, a paired dSR missing from its file); an n/a whose input is its own unavailable
    block (the nav_summ JSON, the paired test, a NAV summary.json) is counted there and not refused twice."""
    cfg = cfg_for()

    def checks_with(edit=None, conf=cfg):
        world(root, conf)
        rows = summ_rows(conf)
        if edit:
            edit(rows)
        put(root, 'b/summ.json', rows)
        return _checks(root, conf)
    r1 = 'v8.cells[R-1].verdict'
    assert checks_with() == []
    assert checks_with(lambda rows: rows.pop(1)) == [
        (r1, f'{NA_HEAD}its nav_summ row (mega-nav-v8-r1) is missing from b/summ.json')]
    assert checks_with(lambda rows: rows.pop(0)) == [   # the baseline carries no rule; R-1 compares with its row
        (r1, f"{NA_HEAD}the parent's nav_summ row (mega-nav-v8-b0c) is missing from b/summ.json")]
    assert checks_with(lambda rows: rows[1].pop('tau_gmv_mean')) == [
        (r1, f'{NA_HEAD}tau_gmv_mean / mean_gross_leverage_all_rows of the cell n/a in its nav_summ row')]
    assert checks_with(lambda rows: rows[0].update(mean_gross_leverage_all_rows=0.0)) == [
        (r1, f'{NA_HEAD}tau_gmv_mean / mean_gross_leverage_all_rows of the parent n/a in its nav_summ row')]
    assert checks_with(lambda rows: rows[1].pop('mean_net_leverage_all_rows')) == [
        (r1, f'{NA_HEAD}mechanics Net leverage, mean over all rows n/a in its nav_summ row')]
    assert checks_with(conf=cfg_for(mechanics=[])) == [(r1, f'{NA_HEAD}no mechanics configured (v8.mechanics)')]
    no_paired = cfg_for()
    del no_paired['v8']['cells'][1]['paired']
    assert checks_with(conf=no_paired) == [(r1, f'{NA_HEAD}no paired test configured (v8.cells[].paired)')]
    world(root, cfg)
    doc = bundle_doc('b/mega-nav-v8-r1', 'b/mega-nav-v8-b0c')
    doc['paired']['dsr'] = None
    put(root, 'b/paired-r1.json', doc)
    assert _checks(root, cfg) == [(r1, f'{NA_HEAD}the paired dSR is n/a in b/paired-r1.json')]
    # an unread manual part and a missing row: both named
    unread = cfg_for()
    unread['v8']['cells'][1]['criterion']['checks'].append({'text': 'net at 4x higher', 'met': None})
    assert checks_with(lambda rows: rows.pop(1), unread) == [
        (r1, "recorded accepted ('ACCEPTED') but its criterion is not read: net at 4x higher (set the part's met); and "
             "the rule of v8-prereg item 5 is n/a: its nav_summ row (mega-nav-v8-r1) is missing from b/summ.json")]
    # a missing input is its own unavailable block: no refusal on top of it
    world(root, cfg)
    (root / 'b/summ.json').unlink()
    rows = V.ladder_rows(make_ctx(root, cfg))[0]
    assert rows[1]['rule'] is None and (rows[1]['na_parts'], rows[1]['na_inputs']) == ([], ['b/summ.json'])
    assert _checks(root, cfg) == []
    nav = summary_cfg()
    world(root, nav)
    (root / 'b/mega-nav-v8-b0c/summary.json').unlink()
    rows = V.ladder_rows(make_ctx(root, nav))[0]
    assert rows[1]['na_parts'] == [] and rows[1]['na_inputs'] == ['b/mega-nav-v8-b0c/summary.json']
    assert _checks(root, nav) == []


def three_cells(r1_verdict: str = 'ACCEPTED', r2_parent: str = 'R-1') -> dict:
    """cfg_for() plus R-2 (parented on ``r2_parent``, accepted, the final cell)."""
    cfg = cfg_for(final='R-2')
    r2 = copy.deepcopy(cfg['v8']['cells'][1])
    r2.update(key='R-2', label='R-2 library v8.0', dir='b/mega-nav-v8-r2', parent=r2_parent, n=42,
              paired='b/paired-r2.json', verdict='ACCEPTED')
    cfg['v8']['cells'][1]['verdict'] = r1_verdict
    cfg['v8']['cells'].append(r2)
    return cfg


def test_ladder_checks_the_parent_is_the_last_accepted_cell(root):
    def parents(cfg, r1_dsr=0.05):
        world(root, cfg)
        put(root, 'b/paired-r1.json', bundle_doc('b/mega-nav-v8-r1', 'b/mega-nav-v8-b0c', dsr=r1_dsr))
        return [(w, why) for w, why in _checks(root, cfg) if w.endswith('.parent')]
    assert parents(three_cells()) == []
    assert parents(three_cells('REJECTED (dSR < 0)'), r1_dsr=-0.02) == [
        ('v8.cells[R-2].parent', "'R-1' is rejected: a rejected cell is never a parent")]
    assert parents(three_cells('REJECTED (dSR < 0)', r2_parent='B0c'), r1_dsr=-0.02) == []
    assert parents(three_cells(r2_parent='B0c')) == [
        ('v8.cells[R-2].parent', "'B0c' is not the last accepted cell before 'R-2' ('R-1')")]
    # an undecided cell before it: the ladder is not settled up to R-2, the parent check waits
    pending = three_cells('pending run', r2_parent='B0c')
    assert parents(pending) == []
    assert [w for w, _ in _checks(root, pending)] == ['v8.cells[R-1].verdict']  # its read paired test, pending
    # the whole ladder, all accepted and consistent, renders with no refusal
    cfg = three_cells()
    world(root, cfg)
    assert [n for n, _ in unavailable(build(root, cfg))] == []


def test_ladder_checks_a_read_paired_test_needs_a_verdict(root):
    cfg = cfg_for()
    world(root, cfg)
    cfg['v8']['cells'][1]['verdict'] = 'pending run'
    assert _checks(root, cfg) == [
        ('v8.cells[R-1].verdict', "its paired test b/paired-r1.json was read but the verdict is still pending "
                                  "('pending run')"),
        ('v8.final', "'R-1' is not the last accepted cell of the ladder ('B0c')")]
    del cfg['v8']['cells'][1]['verdict']
    assert _checks(root, cfg)[0] == ('v8.cells[R-1].verdict', 'its paired test b/paired-r1.json was read but the '
                                                              'verdict is missing')
    (root / 'b/paired-r1.json').unlink()  # no paired JSON: the missing verdict is not a consistency error
    assert [w for w, _ in _checks(root, cfg)] == ['v8.final']


def test_ladder_checks_the_top_level_final(root):
    cfg = cfg_for()
    world(root, cfg)
    cfg['cells'] = [{'dir': 'b/mega-nav-v8-r1', 'label': 'V8-F'}, {'dir': 'b/mega-nav-v8-b0c', 'label': 'B0c'}]
    cfg['final'] = 'v8-r1'
    assert _checks(root, cfg) == []
    cfg['final'] = 'v8-b0c'
    assert _checks(root, cfg) == [('final', "'v8-b0c' is b/mega-nav-v8-b0c, not the cell of v8.final 'R-1' "
                                            "(b/mega-nav-v8-r1)")]
    cfg['final'] = 'v8-r2'
    assert _checks(root, cfg) == [('final', "'v8-r2' is not among the configured cells")]


def test_ladder_refusals_render_above_the_table_and_count_as_unavailable(root):
    cfg = cfg_for(final='B0c')
    world(root, cfg)
    html = build(root, cfg)
    sec = section(html, 'cells')
    na = [(n, m) for n, m in unavailable(html) if n == 'v8_ladder']
    assert na == [('v8_ladder', "v8_ladder: refused (v8.final: 'B0c' is not the last accepted cell of the ladder "
                                "('R-1'))")]
    assert sec.index('v8_ladder: refused') < sec.index('id="t-v8-ladder"')


# ============================================================================================ optional cells (PM4-8)
# Ruling E-43: R-8's realised volatility of the S2 net series inside [.8, 1.2] x 5% in each TRAIN year
R8_CHECK = {'label': 'realised vol', 'source': 'years', 'metric': 'ann_vol', 'op': 'between', 'lo': 0.04, 'hi': 0.06}


def test_years_check_reads_every_train_year_of_the_cells_own_year_table():
    crit = {'text': 'vol band', 'checks': [R8_CHECK]}
    ys = V.train_years()

    def table(vol):  # one TRAIN year's volatility changed
        return {'year_table': [dict(r, ann_vol=vol(r['year'])) for r in year_table(0)]}
    ok = V.criterion_eval(crit, table(lambda y: 0.045), None)
    each = ', '.join(f'{y} 0.045' for y in ys)
    assert ok == {'text': 'vol band', 'met': True, 'unread': [], 'na': [],
                  'detail': f'realised vol by TRAIN year ({each}) between 0.04 and 0.06'}
    for vol, met in ((0.06, True), (0.04, True), (0.0601, False), (0.0399, False), (None, None)):
        got = V.criterion_eval(crit, table(lambda y, v=vol: v if y == ys[2] else 0.05), None)['met']
        assert got is met, vol
    gap = {'year_table': [r for r in year_table(0) if r['year'] != ys[1]]}   # a TRAIN year without return rows: n/a
    assert V.criterion_eval(crit, gap, None)['met'] is None and f'{ys[1]} n/a' in V.criterion_eval(crit, gap, None)[
        'detail']
    assert V.criterion_eval(crit, None, None)['met'] is None                   # no nav_summ row
    assert V.criterion_eval(crit, {'net_sharpe': 1.0}, None)['met'] is None     # a row without a year table
    other = {'year_table': year_table(0) + [{'year': ys[0] - 1, 'ann_vol': 0.5}]}   # a year outside TRAIN: not read
    assert V.criterion_eval(crit, other, {'year_table': []})['met'] is True     # the parent's table is never read
    for bad in (dict(R8_CHECK, op='le'), {k: v for k, v in R8_CHECK.items() if k != 'hi'}, dict(R8_CHECK, lo='x')):
        with pytest.raises(ValueError, match='a years check takes op between'):
            V.criterion_eval({'checks': [bad]}, table(lambda y: 0.05), None)


def optional_cfg(verdicts: dict, parents: dict | None = None, final: str = 'R-1') -> dict:
    """cfg_for() plus the optional cells of Rulings E-38 / E-45 on a small ladder: R-6 (the branch cell, on R-1), R-8
    (the volatility band), R-9a (a report-only frontier cell, defined only if R-6 is rejected), R-10 (only if R-6 and R-1
    are accepted) and R-12 (only if R-6 is accepted); ``verdicts`` and ``parents`` by key."""
    cfg = cfg_for(final=final)

    def cell(key, parent, **kw):
        c = {'key': key, 'label': f'{key} cell', 'dir': f'b/mega-nav-v8-{key.lower()}', 'parent': parent, 'n': 41,
             'paired': f'b/paired-{key.lower()}.json',
             'criterion': {'text': 'turnover not higher', 'checks': [{'metric': 'tau_gmv_mean', 'op': 'le'}]}}
        c.update(kw)
        return c
    cfg['v8']['cells'] += [
        cell('R-6', 'R-1'), cell('R-8', 'R-6', criterion={'text': 'vol band', 'checks': [R8_CHECK]}),
        {k: v for k, v in cell('R-9a', 'R-8', report_only=True, criterion={'text': 'none: report only'},
                               defined_if={'rejected': ['R-6'], 'ruling': 'E-38, E-37'}).items() if k != 'paired'},
        cell('R-10', 'R-8', defined_if={'accepted': ['R-6', 'R-1'], 'ruling': 'E-38, E-45'}),
        cell('R-12', 'R-10', defined_if={'accepted': ['R-6'], 'ruling': 'E-38'})]
    for c in cfg['v8']['cells']:
        c['verdict'] = verdicts.get(c['key'], c.get('verdict', 'pending run'))
        c['parent'] = (parents or {}).get(c['key'], c.get('parent'))
    return cfg


# the three branches of E-38 / E-45 with every defined cell decided consistently with the synthetic world
ALL_ACCEPTED = {'R-6': 'ACCEPTED', 'R-8': 'ACCEPTED', 'R-10': 'ACCEPTED', 'R-12': 'ACCEPTED'}
BRANCHES = {
    'R-6 and R-1 accepted': (ALL_ACCEPTED, {}, 'R-12', {'R-9a'}),
    'R-1 rejected': ({'R-1': 'REJECTED', 'R-6': 'ACCEPTED', 'R-8': 'ACCEPTED', 'R-12': 'ACCEPTED'},
                     {'R-6': 'B0c', 'R-12': 'R-8'}, 'R-12', {'R-9a', 'R-10'}),
    'R-6 rejected': ({'R-6': 'REJECTED', 'R-8': 'ACCEPTED', 'R-9a': 'reported (frontier, no acceptance)'},
                     {'R-8': 'R-1'}, 'R-8', {'R-10', 'R-12'}),
    'PM4-10': (dict(ALL_ACCEPTED, **{'R-10': 'undefined (PM4-10: fewer than 2T members, cap 1/(2T) infeasible)'}),
               {'R-12': 'R-8'}, 'R-12', {'R-9a', 'R-10'})}


def _kinds(cfg: dict) -> dict:
    return {k: s['kind'] for k, s in V.cell_states(cfg['v8']['cells'], []).items()}


def test_cell_states_follow_the_recorded_branch():
    """Rulings E-38 / E-45: the branch is read from the recorded verdicts of R-6 and R-1; an open condition leaves the
    cell as recorded; an "undefined ..." verdict records a cell that could not be formed (Ruling PM4-10)."""
    pending = _kinds(optional_cfg({}))
    assert {k: pending[k] for k in ('R-6', 'R-9a', 'R-10', 'R-12')} == dict.fromkeys(('R-6', 'R-9a', 'R-10', 'R-12'),
                                                                                    'pending')
    assert _kinds(optional_cfg({'R-6': 'defect: rerun decided blind'}))['R-9a'] == 'pending'   # a defect: still open
    for name, (verdicts, parents, _, undefined) in BRANCHES.items():
        kinds = _kinds(optional_cfg(verdicts, parents))
        assert {k for k, v in kinds.items() if v == 'undefined'} == undefined, name
    st = V.cell_states(optional_cfg(ALL_ACCEPTED)['v8']['cells'], [])
    assert st['R-9a'] == {'kind': 'undefined', 'recorded': 'pending', 'verdict': 'undefined (E-38, E-37)',
                          'why': 'defined only if R-6 is rejected (E-38, E-37); R-6 is accepted'}
    st = V.cell_states(optional_cfg(BRANCHES['R-1 rejected'][0])['v8']['cells'], [])
    assert st['R-10']['why'] == 'defined only if R-6 is accepted and R-1 is accepted (E-38, E-45); R-1 is rejected'
    st = V.cell_states(optional_cfg(BRANCHES['R-6 rejected'][0])['v8']['cells'], [])
    assert (st['R-9a']['kind'], st['R-9a']['recorded']) == ('report', 'other')    # a recorded frontier cell: report
    assert st['R-12']['why'].endswith('; R-6 is rejected')
    st = V.cell_states(optional_cfg(BRANCHES['PM4-10'][0])['v8']['cells'], [])
    assert st['R-10']['verdict'].startswith('undefined (PM4-10: ') and st['R-12']['kind'] == 'accepted'
    cells = optional_cfg({})['v8']['cells']
    cells[-1]['defined_if'] = {'accepted': ['R-13']}
    with pytest.raises(KeyError, match="defined_if names 'R-13', not an earlier cell"):
        V.cell_states(cells, [])
    cells[-1]['defined_if'] = {'ruling': 'E-38'}
    with pytest.raises(KeyError, match='defined_if names no cell'):
        V.cell_states(cells, [])


@pytest.mark.parametrize('name', list(BRANCHES))
def test_each_branch_renders_with_no_unavailable_block(root, name):
    """Every branch of E-38 / E-45 (and a PM4-10 cell): 0 unavailable blocks; an undefined cell renders
    "undefined (ruling)" in the ladder, names no input, reads nothing and is not in the year table."""
    verdicts, parents, final, undefined = BRANCHES[name]
    cfg = optional_cfg(verdicts, parents, final)
    world(root, cfg)
    for key in undefined:   # an undefined cell's files are absent too: nothing of it is read
        (root / f'b/paired-{key.lower()}.json').unlink(missing_ok=True)
    assert not [k for _, k, _ in V.inputs(cfg) if any(f'[{u}]' in k for u in undefined)]
    html = build(root, cfg)
    assert unavailable(html) == [], name
    sec = htmllib.unescape(section(html, 'cells'))
    rows = {r['key']: r for r in V.ladder_rows(make_ctx(root, cfg))[0]}
    for key in undefined:
        r = rows[key]
        assert r['verdict_kind'] == 'undefined' and r['crit_text'].startswith('not formed: ')
        assert r['crit_text'].endswith('adds 0 to N, no input read') and f'id="t-v8-year-{key}"' not in sec
        assert (r['dsr'], r['mech'], r['rule'], r['n']) == ('', '', '', '')
    assert sec.count('<span class="chip neutral">UNDEFINED</span>') == len(undefined)
    if name == 'R-6 rejected':
        assert rows['R-9a']['verdict_kind'] == 'report' and rows['R-9a']['rule'] == ''
        assert rows['R-9a']['crit_detail'].startswith('report only, no acceptance: S2 net Sharpe ')
        assert '<span class="chip neutral">REPORT</span>' in sec and 'id="t-v8-year-R-9a"' in sec
    else:
        assert '<span class="sub">undefined (E-38, E-37)</span>' in sec


def test_branch_checks_refuse_a_verdict_an_undefined_or_report_only_cell_cannot_carry(root):
    cfg = optional_cfg(dict(ALL_ACCEPTED, **{'R-9a': 'REJECTED (no frontier)'}), final='R-12')
    world(root, cfg)
    assert _checks(root, cfg) == [
        ('v8.cells[R-9a].verdict', "recorded rejected ('REJECTED (no frontier)') but the cell is undefined (defined "
                                   "only if R-6 is rejected (E-38, E-37); R-6 is accepted): an undefined cell adds 0 "
                                   "and has no verdict")]
    rejected = BRANCHES['R-6 rejected']
    for verdict in ('ACCEPTED', 'REJECTED'):
        cfg = optional_cfg(dict(rejected[0], **{'R-9a': verdict}), rejected[1], 'R-8')
        world(root, cfg)
        assert _checks(root, cfg) == [
            ('v8.cells[R-9a].verdict', f"recorded {verdict.lower()} ({verdict!r}) but R-9a is a report-only cell: no "
                                       f"acceptance (plan R-9; Ruling PM4-8)")]
    cfg = optional_cfg(rejected[0], rejected[1], 'R-8')                  # pending: open, no refusal
    cfg['v8']['cells'][4]['verdict'] = 'pending run'
    world(root, cfg)
    assert _checks(root, cfg) == []


def test_parent_checks_skip_undefined_cells_and_never_take_one_as_parent(root):
    """Plan section 9, parent = the last accepted cell: an undefined cell is never a parent and is skipped when the
    last accepted cell is found; a report-only cell is never a parent either."""
    verdicts, parents, final, _ = BRANCHES['R-1 rejected']
    cfg = optional_cfg(verdicts, dict(parents, **{'R-12': 'R-10'}), final)   # R-12 on the undefined R-10
    world(root, cfg)
    assert _checks(root, cfg) == [('v8.cells[R-12].parent',
                                   "'R-10' is undefined: an undefined cell is never a parent (it was not formed)")]
    cfg = optional_cfg(verdicts, dict(parents, **{'R-12': 'R-6'}), final)    # not the last accepted (R-8)
    world(root, cfg)
    assert _checks(root, cfg) == [('v8.cells[R-12].parent', "'R-6' is not the last accepted cell before 'R-12' "
                                                            "('R-8')")]
    rejected = BRANCHES['R-6 rejected']
    cfg = optional_cfg(rejected[0], rejected[1], 'R-8')
    after = copy.deepcopy(cfg['v8']['cells'][4])                             # a cell after the frontier, on R-9a
    after.update(key='R-9b', dir='b/mega-nav-v8-r9b', parent='R-9a')
    cfg['v8']['cells'].insert(5, after)
    world(root, cfg)
    assert ('v8.cells[R-9b].parent', "'R-9a' is report only: a report-only cell has no acceptance and is never a "
                                     "parent") in _checks(root, cfg)


def test_r8_accepted_against_its_volatility_band_is_refused(root):
    """Ruling E-43: R-8 is accepted on rule 5 with the band as its criterion; an accept with one TRAIN year outside the
    band is refused, and the reject is consistent."""
    def one_year_out(cfg):   # the last TRAIN year's realised volatility above 1.2 x 5%
        rows = summ_rows(cfg)
        next(r for r in rows if r['dir'].endswith('mega-nav-v8-r-8'))['year_table'][-1]['ann_vol'] = 0.07
        put(root, 'b/summ.json', rows)
    cfg = optional_cfg(ALL_ACCEPTED, final='R-12')
    world(root, cfg)
    assert _checks(root, cfg) == []
    one_year_out(cfg)
    assert _checks(root, cfg) == [('v8.cells[R-8].verdict', "recorded accepted ('ACCEPTED') but the rule of v8-prereg "
                                                            "item 5 rejects it (dSR > 0 yes, mechanics yes, criterion "
                                                            "no)")]
    cfg = optional_cfg(dict(ALL_ACCEPTED, **{'R-8': 'REJECTED (vol band)'}), {'R-10': 'R-6'}, 'R-12')
    world(root, cfg)
    one_year_out(cfg)
    rows = {r['key']: r for r in V.ladder_rows(make_ctx(root, cfg))[0]}
    assert (rows['R-8']['pos'], rows['R-8']['crit_met'], rows['R-8']['rule']) == (False, False, False)
    assert _checks(root, cfg) == []


def test_r8_accepted_with_its_band_unread_is_refused_naming_the_year(root):
    """Ruling PM5-12, finding R6C-1: R-8 recorded accepted while its band cannot be read on its own year table (a TRAIN
    year missing, a year whose volatility is n/a for one return row, no year table) is refused, naming the year or the
    table; the ladder shows the refusal. Before the fix the rule was n/a with nothing unread, and nothing was refused.
    Recorded rejected, the n/a rule does not contradict it."""
    ys = V.train_years()

    def r8_checks(cfg, edit):
        world(root, cfg)
        rows = summ_rows(cfg)
        edit(next(r for r in rows if r['dir'].endswith('mega-nav-v8-r-8')))
        put(root, 'b/summ.json', rows)
        return _checks(root, cfg)

    def drop_last_year(r):
        r['year_table'] = [y for y in r['year_table'] if y['year'] != ys[-1]]
    cfg = optional_cfg(ALL_ACCEPTED, final='R-12')
    r8 = 'v8.cells[R-8].verdict'
    assert r8_checks(cfg, drop_last_year) == [
        (r8, f'{NA_HEAD}realised vol: TRAIN year {ys[-1]} is not in its year table (no return rows)')]
    assert [n for n, _ in unavailable(build(root, cfg)) if n.startswith('v8_')] == ['v8_ladder']
    assert r8_checks(cfg, lambda r: r['year_table'][1].update(return_rows=1, ann_vol=None)) == [
        (r8, f'{NA_HEAD}realised vol: TRAIN year {ys[1]}: ann_vol n/a (1 return rows)')]
    assert r8_checks(cfg, lambda r: r.pop('year_table')) == [
        (r8, f'{NA_HEAD}realised vol: its nav_summ row has no year_table (run nav_summ with --protocol v8)')]
    rejected = optional_cfg(dict(ALL_ACCEPTED, **{'R-8': 'REJECTED (vol band)'}), {'R-10': 'R-6'}, 'R-12')
    assert r8_checks(rejected, drop_last_year) == []


def test_config_without_optional_cells_is_unchanged_by_the_branch_machinery():
    """No defined_if, report_only or "undefined ..." verdict: every cell is defined and keeps its recorded kind."""
    cfg = three_cells()
    states = V.cell_states(cfg['v8']['cells'], [])
    assert {k: (s['kind'], s['why']) for k, s in states.items()} == {
        'B0c': ('accepted', None), 'R-1': ('accepted', None), 'R-2': ('accepted', None)}
    assert V.defined_cells(cfg['v8']['cells'], []) == cfg['v8']['cells']


# ============================================================================================ cumulative test, gate
def test_freeze_gate_items_order_and_states():
    pd = V.paired_of(bundle_doc('f', 'b', dsr=0.25, p1=0.04))
    fg = BASE_CFG['v8']['freeze_gate']
    row = summ_rows(cfg_for())[1]
    g = V.freeze_gate(row, pd, fg, ('R-1', 'B0c'))
    assert [i['name'] for i in g['items']] == ['Net Sharpe, S2', MECH[0]['label'], MECH[1]['label'],
                                               'Cumulative paired S2 net dSR, R-1 vs B0c',
                                               'Studentized circular-block bootstrap p, one-sided',
                                               'Deflated Sharpe, cell count']
    assert g['freeze'] is True and g['alpha'] == 0.1
    assert V.freeze_gate(None, pd, fg, ('R-1', 'B0c'))['freeze'] is None  # no nav_summ row: undetermined
    assert V.freeze_gate(row, V.paired_of(bundle_doc('f', 'b', dsr=0.25, p1=0.1)), fg, ('a', 'b'))['freeze'] is False
    tail = V.freeze_gate(row, pd, {'checks': [{'label': 'x', 'metric': 'net_sharpe', 'op': 'ge', 'value': 0}]}, ('a', 'b'))
    assert [i['name'][:10] for i in tail['items']] == ['x', 'Cumulative', 'Studentize']


def test_bundle_block_met_and_unmet(root):
    cfg = cfg_for()
    world(root, cfg)
    sec = section(build(root, cfg), 'sum')
    assert 'Freeze gate: R-1 composition v8 vs B0c baseline' in sec and '>MET<' in sec and 'id="t-v8-bundle"' in sec
    assert 'id="t-v8-bundle-years"' in sec and '4,999 draws, block 21, seed 20260929 (as registered)' in sec
    assert 'dSR +0.250; N 39; TRAIN +1.000' in re.sub(r'<[^>]+>', '', sec)
    put(root, 'b/bundle.json', bundle_doc('b/mega-nav-v8-r1', 'b/mega-nav-v8-b0c', dsr=-0.02, p1=0.6))
    sec = section(build(root, cfg), 'sum')
    assert 'UNMET' in sec and 'unmet: OD-3 is the lever' in sec


def test_bundle_notes_protocol_alpha_and_missing_summ(root):
    cfg = cfg_for()
    world(root, cfg, skip=('b/summ.json',))
    doc = bundle_doc('b/mega-nav-v8-r1', 'b/mega-nav-v8-b0c', dsr=0.25, p1=0.04)
    doc['paired']['draws'], doc['verdict']['alpha'] = 2000, 0.05
    put(root, 'b/bundle.json', doc)
    b = V.an_bundle(make_ctx(root, cfg))
    assert b['freeze'] is None and b['final_row'] is None
    text = ' '.join(b['notes'])
    assert 'b/summ.json' in text and 'is not the registered 21 / 20260929 / 4999' in text and '--bundle-alpha 0.05' in text


# ============================================================================================ diagnostics, members
def test_diag_rows_headlines_skipped_and_absent():
    doc = diagnostics_doc()
    del doc['diagnostics']['G-3d']
    rows = {r['id']: r for r in V.diag_rows(doc)}
    assert list(rows) == list(V.DIAG_IDS)
    assert rows['G-2b']['status'] == 'skipped' and rows['G-2b']['text'] == 'skipped: no --u-pass given'
    assert 'themes_model.ratio 0.42' in rows['G-1c']['text'] and 'members.bac.retained 0.6' in rows['G-3b']['text']
    assert 'delays.2.delta -0.09' in rows['G-3c']['text'].replace('−', '-')
    assert rows['G-3d']['status'] == 'not in file'


def test_diagnostics_render_and_content_seal(root):
    html = V.render_diagnostics(diagnostics_doc(), 'd.json', num=3, num_members=4)
    assert 'id="t-v8-diagnostics"' in html and 'id="t-v8-g1a-members"' in html and '>SKIPPED<' in html
    assert V.REPORT_ONLY in html and 'no --u-pass given' in html
    with pytest.raises(ValueError, match='sealed'):
        V.refuse_sealed_sessions(diagnostics_doc(sealed=True), 'diagnostics')
    cfg = cfg_for()
    world(root, cfg)
    put(root, 'b/diag/diagnostics-v8.json', diagnostics_doc(sealed=True))
    na = dict(unavailable(build(root, cfg)))
    assert list(na) == ['v8_diagnostics'] and 'refusing (sealed)' in na['v8_diagnostics']


def test_member_rows_merge_index_and_admission():
    rows = {r['id']: r for r in V.member_rows(card_index(), admission())}
    assert rows['bm'] == {'id': 'bm', 'theme': 'value', 'status': 'admitted', 'ic_theta': 0.009,
                          'marginal_ic21': 0.004, 'f_theta': 1.2e-5, 'f_theta_hac_t': 1.1}
    assert rows['ear']['status'] == 'reject_veto' and rows['smax'].get('f_theta') is None
    html = V.render_member_horizon(list(rows.values()), 's')
    assert 'report only, gates nothing' in html and 'Weight' not in html


def test_member_block_one_block_per_missing_input(root):
    cfg = cfg_for()
    world(root, cfg, skip=('b/w/admission.json',))
    html = build(root, cfg)
    na = unavailable(html)
    assert [n for n, _ in na] == ['v8_member_horizon'] and 'b/w/admission.json' in na[0][1]
    assert 'id="t-v8-member-horizon"' in html  # the card index still renders


# ============================================================================================ trial accounting
def test_appendix_a_block_states_the_re_screens(root):
    cfg = cfg_for()
    world(root, cfg)
    tl = V.an_ledger(make_ctx(root, cfg))
    assert (tl['n'], tl['k'], tl['re_screens'], tl['chained']) == (39, 7, 8, True)
    assert tl['block'].startswith('TRAIN construction cells 39; admission trials this sprint 7 plus 8 re-screens; '
                                  f'window {RW.WINDOW_ID} (2020-2023); hidden 2024+ unread')
    html = V.render_trial_accounting(tl, tl['src'])
    assert 'budget 51: within' in html and '7 + 8' in html and 'ruling R2-e' in html
    assert V.an_ledger(make_ctx(root, cfg_for(re_screens=0)))['block'].count('re-screens') == 0


def test_ledger_with_a_broken_chain_is_unavailable(root):
    cfg = cfg_for()
    world(root, cfg)
    p = root / 'b/trials.jsonl'
    lines = p.read_text(encoding='utf-8').splitlines()
    lines[2] = lines[2].replace('"s2_net_sr":1.0', '"s2_net_sr":1.5')
    p.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    na = dict(unavailable(build(root, cfg)))
    assert list(na) == ['v8_trial_accounting'] and 'hash chain broken' in na['v8_trial_accounting']


# ============================================================================================ OD-1, contradictions
def test_prereg_item_synthetic_and_real():
    assert V.prereg_item(PREREG_TEXT, 1).startswith('1. Window. TRAIN is [2020-01-01, 2024-01-01).')
    assert 'read twice' in V.prereg_item(PREREG_TEXT, 1) and 'N continues' not in V.prereg_item(PREREG_TEXT, 1)
    with pytest.raises(ValueError, match='item 7'):
        V.prereg_item(PREREG_TEXT, 7)
    real = V.prereg_item(PREREG_TEXT if not PREREG.exists() else PREREG.read_text(encoding='utf-8'), 1)
    assert 'TRAIN is [2020-01-01, 2024-01-01)' in real and '2025 and later has never been read' in real


def test_od1_block_quotes_the_window(root):
    cfg = cfg_for()
    world(root, cfg)
    sec = section(build(root, cfg), 'prot')
    assert 'OD-1 window disclosure (v8-prereg item 1)' in sec and f'Window {RW.WINDOW_ID}: TRAIN [2020-01-01, ' in sec
    assert 'Never read: 2025-01-01 and later.' in sec and '2024 stays hidden but is not pristine.' in sec


def test_contradictions_synthetic_and_real():
    rows = V.markdown_table_after(LITERATURE_TEXT, V.LITERATURE_HEADING)
    assert [V.verdict_class(r['verdict'])[1] for r in rows] == ['contradicts', 'updates', 'confirms', 'other']
    html = V.render_contradictions(rows, 'lit.md')
    assert '<a href="https://example.org/a" rel="noopener">Gerard-Jehl 2025</a>' in html
    assert '4 rows: 1 contradicts, 1 updates, 1 confirms, 1 other' in html
    with pytest.raises(ValueError, match='not found'):
        V.markdown_table_after('# x\n', V.LITERATURE_HEADING)
    if LITERATURE.exists():
        real = V.markdown_table_after(LITERATURE.read_text(encoding='utf-8'), V.LITERATURE_HEADING)
        kinds = [V.verdict_class(r['verdict'])[1] for r in real]
        assert len(real) >= 20 and kinds.count('contradicts') >= 5 and 'updates' in kinds and 'confirms' in kinds


# ============================================================================================ inputs, full render
def test_inputs_name_one_owner_per_path():
    ins = V.inputs(cfg_for())
    assert [(b, k) for b, k, _ in ins] == [
        ('v8_year_table', 'v8.summ'), ('v8_ladder', 'v8.cells[R-1].paired'), ('v8_bundle', 'v8.bundle'),
        ('v8_diagnostics', 'v8.diagnostics'), ('v8_member_horizon', 'v8.member_horizon.card_index'),
        ('v8_member_horizon', 'v8.member_horizon.admission'), ('v8_trial_accounting', 'v8.trial_ledger'),
        ('v8_od1', 'v8.prereg'), ('v8_contradictions', 'v8.literature')]
    assert len({p for _, _, p in ins}) == len(ins)


def test_full_render_every_input_present(root):
    cfg = cfg_for()
    world(root, cfg)
    html = build(root, cfg)
    assert unavailable(html) == [] and 'unresolved' not in html
    for tid in ('t-v8-bundle', 't-v8-ladder', 't-v8-year-matrix', 't-v8-diagnostics', 't-v8-member-horizon',
                't-v8-contradictions', 't-appendix-files'):
        assert f'id="{tid}"' in html, tid
    for _, _, rel in V.inputs(cfg):
        assert f'<td class="m t" data-sort="{rel}">{rel}</td><td class="t" data-sort="read">read</td>' in html, rel


def test_each_missing_input_is_one_named_unavailable_block(root):
    cfg = cfg_for()
    for block, key, rel in V.inputs(cfg):
        for p in root.iterdir():
            shutil.rmtree(p) if p.is_dir() else p.unlink()
        world(root, cfg, skip=(rel,))
        na = unavailable(build(root, cfg))
        assert [n for n, _ in na] == [block], key
        assert rel in na[0][1], key
