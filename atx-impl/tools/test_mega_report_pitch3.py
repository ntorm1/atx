"""Synthetic checks for the pitch iteration 3 sections of mega_report (pitch3.py): capacity curve, risk-model bias,
report cards, monitor baseline, operating loop, integrity statistics, trial ledger, and verdict badges in the cell
table. Every section is rendered from small fixtures written under a temporary report root, with the input present,
absent and malformed; an absent or malformed input must render the "not available" marker, never stop the build.

Run: python -m pytest atx-impl/tools/test_mega_report_pitch3.py -q
"""
import copy
import hashlib
import json
import math
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mega_report import components as C  # noqa: E402
from mega_report import pitch as P  # noqa: E402
from mega_report import pitch3 as P3  # noqa: E402
from mega_report import report as R  # noqa: E402

S2, KO, FIM = 'modeled-1bn-stale5-v1+swap-fin-v1', 'modeled-1bn-ko-v1+swap-fin-v1', 'modeled-1bn-fim-v1+swap-fin-v1'
MULTS = [0.5, 1.0, 2.0, 4.0, 8.0]
NETS = [1.3, 1.2, 1.1, 0.95, 0.8]
COSTS = [10.0, 12.0, 14.0, 16.0, 18.0]


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def put(root: Path, rel: str, obj) -> bytes:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    data = obj if isinstance(obj, bytes) else (obj if isinstance(obj, str) else json.dumps(obj, indent=1)).encode()
    p.write_bytes(data)
    return data


# ============================================================================================ fixtures
def daily_csv(traded: float, cost: float) -> str:
    rows = ['session_ns,return_observation,net_return,traded_dollars,trade_cost_dollars']
    for k in range(3):
        rows.append(f'{1600000000000000000 + k * 86400000000000},1,0.001,{traded / 3},{cost / 3}')
    return '\n'.join(rows) + '\n'


def write_capacity(root: Path) -> None:
    rows = [{'multiple': m, 'book': f'capacity-x{m}', 'equivalent_initial_nav': m * 1e9, 'net_sharpe': n,
             'gross_sharpe': n + 0.3, 'ann_mean_net': 0.05, 'ann_vol': 0.045, 'max_drawdown': 0.04,
             'cost_bps_per_traded_dollar': c, 'traded_dollars': 2.5e10 * m, 'trade_cost_dollars': 3e7 * m,
             'capped_share': 0.01 * m, 'unfilled_dollars': 1e8 * m, 'participation_p95_bound': 0.002 * m,
             'daily_turnover_gmv_mean': 0.038, 'financing_dollars': 1.6e7 * m}
            for m, n, c in zip(MULTS, NETS, COSTS)]
    put(root, 'stress/v7_extras.json', {'schema': 'atx.nav-v7-extras/v1', 'capacity': rows[::-1],
                                        'capacity_rule': 'capacity by replay (synthetic)',
                                        'capacity_x1_equals_primary_bit_for_bit': True})
    scen = []
    for sid, net, traded, cost in ((S2, 1.2, 2.7e10, 3.24e7), (KO, 1.25, 2.7e10, 2.7e7), (FIM, 1.33, 2.8e10, 2.1e7)):
        data = put(root, f'stress/daily_{sid}.csv', daily_csv(traded, cost))
        scen.append({'scenario': sid, 'net_sharpe': net, 'gross_sharpe': 1.6, 'ann_mean': 0.06, 'ann_vol': 0.047,
                     'max_drawdown': 0.045, 'costs': {'trade_cost_dollars': cost},
                     'capacity': {'capped_fills': 60, 'fills': 12000, 'unfilled_dollars': 2e8,
                                  'participation_p95_upper_bound': 0.0039},
                     'daily_turnover_gmv': {'mean': 0.038}, 'financing': {'total_financing_dollars': 1.7e7},
                     'daily_csv_sha256': sha(data)})
    put(root, 'stress/summary.json', {'scenarios': scen})


def write_bias(root: Path, book: bool = False) -> None:
    def fam(b63, b252, dropped=0):
        return {'status': 'ok', 'series': 10, 'series_ok': 9, 'series_empty': 1, 'series_refused': 0,
                'observations': 5000, 'pooled_kurtosis': 5.0, 'dropped_factor_exposures': dropped,
                'uncovered_name_returns': 0, 'warmup_excluded': 12, 'missing_returns': 3, 'dropped_share': 0.0,
                'full_sample': {'b_mean': 0.99, 'b_median': 0.98, 'series_in_band': 5, 'series_in_kurtosis_band': 8},
                'rolling': {'63': {'b_mean': b63, 'band': [0.82, 1.18], 'share_in_band': 0.6,
                                   'share_in_kurtosis_band': 0.8},
                            '252': {'b_mean': b252, 'band': [0.91, 1.09], 'share_in_band': 0.5,
                                    'share_in_kurtosis_band': 0.7}}}
    fams = {'factor': fam(0.97, 0.986), 'random': fam(0.907, 0.898)}
    if book:
        fams['book'] = fam(1.01, 1.02)
    put(root, 'risk/bias_summary.json', {'schema': 'atx.risk-bias/v2', 'definition': 'b = SD(z)', 'families': fams})
    put(root, 'risk/manifest.json', {'schema': 'atx.risk-model/v1', 'geometry': {'factors': 62, 'styles': 11},
                                     'structural_factors': {'factors': [{'factor': 'ind_ff6'}, {'factor': 'style_beta'}],
                                                            'sessions_with_structural': 40,
                                                            'forecast_sessions_with_unforecast_exposure': 0}})


def card(cid: str, gap63: float = 1e-5, valid: int = 700, theme: str = 'value', status: str = 'admitted') -> dict:
    iby = {h: {y: {'mean': 0.01 * k, 'ir': 0.1 * k, 'n': 250, 'sd': 0.1} for k, y in enumerate(['2020', '2021', 'all'], 1)}
           for h in ('5', '21', '63')}
    rc = {h: {'valid_date_mismatches': 0, 'abs_mean_diff': (gap63 if h == '63' else 1e-6), 'runner_valid_dates': valid,
              'card_valid_dates': valid} for h in ('5', '21', '63')}
    return {'schema': 'atx.alpha-report-card/v1', 'id': cid, 'theme': theme, 'tier': 'B',
            'admission': {'status': status, 'hac_t': 1.5, 'tau': 0.02, 'payload_matches_admission': True},
            'ic_by_year': iby, 'runner_check': rc, 'turnover': {'mean': 0.02, 'by_year': {'2020': {'mean': 0.021}}},
            'decay': {'fit': {'half_life_days': 120.0, 'beyond_window': True, 'r2': 0.9}},
            'worldquant': {'sharpe': 0.8, 'fitness': 0.5, 'margin_bps': 40.0, 'sharpe_by_year': {'2020': 0.7}},
            'coverage': {'2020': 0.9, 'all': 0.91},
            'correlation': {'signal': {'max_abs': {'rho': 0.6, 'with': 'x'}}, 'pnl': {'max_abs': {'rho': 0.7, 'with': 'y'}}},
            'ic_by_size_tercile': {'horizon': 21, 'groups': {'small': {'mean': 0.02}}}}


def write_cards(root: Path, d: str = 'cards') -> None:
    files = {}
    for c in (card('alpha'), card('beta', gap63=2e-3, status='reject_redundant'), card('gamma', valid=0)):
        files[f"card-{c['id']}.json"] = sha(put(root, f"{d}/card-{c['id']}.json", c))
    files['card-beta.json'] = '0' * 64  # a tampered card
    ids = ['alpha', 'beta', 'gamma', 'delta']  # delta has no card file
    put(root, f'{d}/index.json', {'schema': 'atx.alpha-report-card-index/v1', 'ranking': 'fitness',
                                  'candidates': [{'id': i, 'rank': k + 1, 'status': 'admitted', 'theme': 'value'}
                                                 for k, i in enumerate(ids)]})
    put(root, f'{d}/manifest.json', {'files': files})


def write_monitor(root: Path) -> None:
    def sub(v, st='ok'):
        return {'value': v, 'status': st, 'p5': 0.0, 'p95': 0.1, 'n': 63}
    members = {'hot': {'status': 'alarm', 'ic63': sub(0.02), 'turnover_cusum': {'value': 9.5, 'status': 'alarm'}},
               'luke': {'status': 'warn', 'ic63': sub(0.2, 'warn')},
               'calm': {'status': 'ok', 'ic63': sub(0.02)}}
    put(root, 'monitor/monitor.json', {
        'schema': 'atx.book-monitor/v1', 'status': 'alarm', 'mode': 'baseline', 'in_sample': True, 'action': 'none',
        'checks': {'M1': {'status': 'n/a', 'band': [0.82, 1.18], 'window': 63, 'reason': 'no family=book rows'},
                   'M2': {'status': 'alarm', 'ic_horizon': 5, 'counts': {'alarm': 1, 'warn': 1, 'ok': 1, 'n/a': 0},
                          'cusum': {'h': 5.0, 'k': 0.5, 'warn': 2.5, 'arl0_years': 18.6}, 'members': members},
                   'M3': {'status': 'ok', 'window': 21, 'cost_ratio': {'value': 1.01, 'p5': 0.8, 'p95': 1.5},
                          'cost_ratio_band': [0.7, 1.3], 'fill_rate': {'value': 0.99, 'p5': 0.98}},
                   'M4': {'status': 'ok', 'window': 63, 'mean_pairwise_corr': 0.08, 'p95': 0.18, 'p99': 0.2},
                   'decision': {'status': 'n/a', 'reason': 'no --decide'}}})


def write_ops(root: Path) -> None:
    for d, mm in (('2021-08-04', 0), ('2022-10-12', 0)):
        put(root, f'ops/decide/{d}/decision.json', {
            'schema': 'atx.book-decision/v1', 'asof': d, 'status': 'complete',
            'replay_parity': {'mismatches': mm, 'names': 500},
            'health': {'status': 'warn', 'checks': [{'check': 'data_freshness', 'status': 'warn'},
                                                    {'check': 'gross_leverage', 'status': 'ok'}]},
            'decision': {'members': 400, 'planned_gross': 0.92, 'planned_turnover': 0.037, 'planned_held_names': 420},
            'transfer_coefficient': {'desired_over_sigma_target': 0.75}, 'orders': {'count': 300},
            'orders_shares': {'orders': 298, 'buys': 150, 'sells': 148, 'short_sales': 80, 'exits': 7,
                              'refused_no_close': 2, 'dropped_min_notional': {'count': 0}, 'rounded_to_zero': {'count': 0},
                              'residual_notional': {'abs': 3000.0}, 'participation': {'above_cap': 1},
                              'lot_size': 1, 'min_notional': 500.0, 'nav_dollars': 1e9}})
        put(root, f'ops/decide-{d}-run/receipt.json', {'schema': 'atx.bounded-research-run/v1', 'wall_seconds': 1.9,
                                                       'sampled_peak_tree_rss_bytes': 3.5e8, 'outcome': 'completed',
                                                       'exit_code': 0})
    zero = {'compared': 100, 'ok': 100, 'explained': 0, 'missing': 0, 'extra': 0, 'quantity': 0, 'unmapped': 0,
            'unexplained_breaks': 0}
    mut = dict(zero, compared=101, ok=97, explained=1, missing=1, extra=1, quantity=1, unexplained_breaks=3)
    for name, cnt in (('self', zero), ('mut', mut)):
        put(root, f'ops/recon-{name}/reconcile.json', {
            'schema': 'atx.book-reconcile/v1', 'status': 'complete', 'counts': cnt, 'decision_asof': '2022-10-12',
            'broker_asof': '2022-10-13', 'tolerance': {'shares': 0.5}, 'corporate_actions': {'applied': cnt['explained']}})
    put(root, 'ops/nav-off-run/receipt.json', {'schema': 'atx.bounded-research-run/v1', 'wall_seconds': 30.0})
    put(root, 'ops/nav-on-run/receipt.json', {'schema': 'atx.bounded-research-run/v1', 'wall_seconds': 36.0})
    put(root, 'ops/holdings/manifest.json', {'schema': 'atx.nav-holdings/v2', 'rows': 12345, 'sessions': 755,
                                             'format': {'id': 'f64'}})


def summ_row(name: str, net: float, dsr, n: int, dsr_eff=0.95, gross=0.97) -> dict:
    r = {'dir': f'build\\mega-nav-{name}', 'net_sharpe': net, 'mean_gross_leverage_all_rows': gross,
         'mean_net_leverage_all_rows': 0.004, 'tau_gmv_mean': 0.038, 'tau_gmv_p95': 0.048,
         'deflated_lo_null': {'dsr': 0.51, 'sr0_annual': 1.22},
         'psr': {'alpha': 0.05, 'benchmarks': [{'sr_star_annual': 0.0, 'psr': 0.978, 'min_trl_years': 1.99,
                                                'min_trl_sessions': 501.0},
                                               {'sr_star_annual': 0.5, 'psr': 0.66, 'min_trl_years': 12.0}]},
         'net_moments': {'skew': -0.5, 'kurtosis': 9.0, 'sessions': 754}}
    if dsr is not None:
        r['deflated'] = {'dsr': dsr, 'n': n, 'sr0_annual': 0.44}
    if dsr_eff is not None:
        r['deflated_effective_n'] = {'dsr': dsr_eff, 'n_eff': 5, 'n_trials': n, 'sr0_annual': 0.22}
    return r


def write_integrity(root: Path) -> None:
    put(root, 'summ/n32.json', [summ_row('final', 1.239, 0.903, 32), summ_row('other', 1.1, 0.88, 32)])
    put(root, 'summ/n33.json', [summ_row('final', 1.239, 0.766, 33, dsr_eff=0.823), summ_row('other', 1.1, 0.7, 33),
                                summ_row('noeff', 1.0, 0.96, 33, dsr_eff=None),
                                summ_row('nocc', 1.3, None, 33, dsr_eff=0.99)])
    put(root, 'summ/pbo.json', {'pbo': 0.35, 'n_candidates': 4, 'splits': 12870, 'splits_total': 12870,
                                'exhaustive': True, 'blocks': 16, 'block_width': 47, 'prob_winner_oos_loss': 0.03,
                                'degradation_slope': -0.98, 'winner_is_sr_annual_mean': 1.3,
                                'winner_oos_sr_annual_mean': 1.22, 'cells': ['build/mega-nav-final']})


def ledger_line(i: int, cell: str, sr: float) -> str:
    return json.dumps({'schema': 'atx.trial-ledger/v1', 'kind': 'construction', 'count': 1, 'trial_id': f'{i:016x}',
                       'cell': f'build/mega-nav-{cell}', 's2_net_sr': sr, 'window': {'label': 'TRAIN', 'sessions': 754},
                       'recorded_by': {'git_head': 'ab' * 20}})


def write_ledger(root: Path, n: int = 4, bad: bool = True) -> None:
    cells = ['v5-a', 'v5-b', 'final', 'v61u-spo-v1'][:n]
    lines = [ledger_line(i, c, 0.5 + 0.1 * i) for i, c in enumerate(cells, 1)]
    if bad:
        lines.insert(2, '{not json')
    put(root, 'ledger/trials.jsonl', '\n'.join(lines) + '\n\n')


def write_cells(root: Path) -> None:
    put(root, 'nav/mega-nav-final/summary.json', {'status': 'complete', 'scenarios': []})


def world(root: Path) -> None:
    write_capacity(root)
    write_bias(root)
    write_cards(root)
    write_monitor(root)
    write_ops(root)
    write_integrity(root)
    write_ledger(root)
    write_cells(root)


BASE_CFG = {
    'schema': 'atx.mega-report-config/v2', 'title': 'P3 test', 'sessions_per_year': 252, 'cell_prefix': 'mega-nav-',
    'verdict_badges': {'rules': []},
    'cells': [{'dir': 'nav/mega-nav-final', 'label': 'Final', 'group': 'final', 'verdict': 'PROMOTED'},
              {'dir': 'nav/mega-nav-v70-new', 'label': 'v7.0 cell', 'group': 'v7-trial', 'verdict': 'REJECTED (dSR < 0)'},
              {'dir': 'nav/mega-nav-spo2', 'label': 'spo-v2', 'group': 'v7-trial', 'verdict': 'pending run'},
              {'dir': 'nav/mega-nav-uni', 'label': 'universe lo3', 'group': 'v7-trial',
               'verdict': 'not adopted', 'verdict_kind': 'rejected'}],
    'groups': [{'key': 'final', 'label': 'Final', 'token': 'g-final'}],
    'capacity_curve': {'dir': 'stress', 'label': 'the test book', 'sr_threshold': 1.0,
                       'primary': {'label': 'S2', 'scenario': S2},
                       'beside': [{'label': 'S2-KO', 'scenario': KO, 'token': 'flat'},
                                  {'label': 'S2-FIM', 'scenario': FIM, 'token': 'tiers'}]},
    'risk_bias': {'summary': 'risk/bias_summary.json', 'manifest': 'risk/manifest.json', 'label': 'test risk model'},
    'report_cards': {'horizons': [5, 21, 63], 'runner_tolerance': 5e-4,
                     'sets': [{'key': 'v61', 'label': 'set one', 'dir': 'cards'}]},
    'monitor_baseline': {'path': 'monitor/monitor.json'},
    'ops_loop': {'decisions': [{'asof': d, 'dir': f'ops/decide/{d}', 'run': f'ops/decide-{d}-run'}
                               for d in ('2021-08-04', '2022-10-12')],
                 'reconcile': [{'label': 'self', 'dir': 'ops/recon-self', 'expect': {'unexplained_breaks': 0, 'missing': 0}},
                               {'label': 'planted', 'dir': 'ops/recon-mut',
                                'expect': {'missing': 1, 'extra': 1, 'quantity': 1, 'explained': 1}}],
                 'nav_runs': {'off': 'ops/nav-off-run', 'on': 'ops/nav-on-run', 'max_ratio': 1.3},
                 'holdings': 'ops/holdings/manifest.json',
                 'flow': [{'title': 'replay', 'lines': ['{a:ops.holdings_rows|int} rows']}, {'title': 'decide', 'kind': 'key'}]},
    'integrity': {'runs': [{'label': 'n32', 'nav_summ': 'summ/n32.json'},
                           {'label': 'n33', 'nav_summ': 'summ/n33.json', 'primary': True}],
                  'final': 'final', 'pbo_max': 0.2,
                  'cells': [{'dir': 'final', 'label': 'Final'}, {'dir': 'other', 'label': 'Other'},
                            {'dir': 'noeff', 'label': 'No eff-N'}, {'dir': 'nocc', 'label': 'No cell count'}],
                  'pbo': [{'label': 'grid', 'path': 'summ/pbo.json'}],
                  'freeze_gate': {'name': 'Freeze gate', 'checks': [
                      {'label': 'Net SR', 'metric': 'net_sharpe', 'op': 'ge', 'value': 1.0},
                      {'label': 'Gross', 'metric': 'mean_gross_leverage_all_rows', 'op': 'between', 'lo': 0.9, 'hi': 1.05},
                      {'label': 'Net', 'metric': 'mean_net_leverage_all_rows', 'op': 'abs_le', 'value': 0.02},
                      {'label': 'Tau', 'metric': 'tau_gmv_mean', 'op': 'le', 'value': 0.2},
                      {'label': 'DSR cell count', 'metric': 'deflated.dsr', 'op': 'ge', 'value': 0.95}]}},
    'trial_ledger': {'path': 'ledger/trials.jsonl', 'kind': 'construction',
                     'eras': [{'label': 'v7 spo', 'match': 'spo'}, {'label': 'v5', 'match': '^mega-nav-v5-'},
                              {'label': 'final', 'match': 'final$'}],
                     'verdicts': {'v61u-spo-v1': {'label': 'spo-v1', 'verdict': 'REJECTED as a defect'},
                                  'v5-b': {'verdict': 'grid cell', 'kind': 'rejected'}},
                     'validation': {'spent': 2, 'budget': 3, 'window': 'read twice'},
                     'holdout': {'window': 'later years', 'status': 'reserved'}},
    'layout': [
        {'id': 'cap', 'title': 'Capacity', 'blocks': [
            {'type': 'prose', 'paragraphs': ['SR {a:capcurve.x1.net_sharpe|+.3f} crossing ${a:capcurve.cross_nav_bn|.2f}bn '
                                             'KO {a:capcurve.book.S2-KO.net_sharpe|+.3f}']}, 'capacity_curve']},
        {'id': 'risk', 'title': 'Risk', 'blocks': [
            {'type': 'prose', 'paragraphs': ['b {a:bias.fam.factor.b252|.3f}; structural {a:bias.structural_factors|d}']},
            't_risk_bias']},
        {'id': 'cards', 'title': 'Cards', 'blocks': [
            {'type': 'prose', 'paragraphs': ['{a:cards.runner_pass|d} of {a:cards.n|d} pass']}, 't_report_cards']},
        {'id': 'ops', 'title': 'Ops', 'blocks': [
            {'type': 'prose', 'paragraphs': ['parity {a:ops.parity_total|d}; {a:ops.expect_met|d} of {a:ops.expect_n|d}; '
                                             'alarm {a:monitor_base.alarming_names}']},
            'ops_loop', 't_monitor_baseline']},
        {'id': 'int', 'title': 'Integrity', 'blocks': [
            {'type': 'prose', 'paragraphs': ['DSR {a:integrity.finals.0.dsr|.3f} to {a:integrity.final.dsr|.3f}; '
                                             'PBO {a:integrity.pbo.0.pbo|.3f}']},
            't_integrity', 't_trial_ledger']},
        {'id': 'cells', 'title': 'Cells', 'blocks': ['t_cells']}]}


def cfg_for(root: Path, **over) -> dict:
    cfg = copy.deepcopy(BASE_CFG)
    cfg['root'] = str(root)
    for k, v in over.items():
        if v is None:
            cfg.pop(k, None)
        else:
            cfg[k] = v
    return cfg


def build(root: Path, cfg: dict) -> str:
    p = root / 'cfg.json'
    p.write_text(json.dumps(cfg), encoding='utf-8')
    return R.build(p, stamp='test')


def make_ctx(root: Path, cfg: dict):
    raw = json.dumps(cfg).encode()
    return R.Ctx(cfg, raw, root / 'cfg.json', root, 'test')


def section(html: str, sec_id: str) -> str:
    i = html.index(f'<section class="sec" id="{sec_id}">')
    j = html.find('<section class="sec"', i + 10)
    return html[i:j if j > 0 else len(html)]


def unavailable(html: str) -> list[str]:
    return re.findall(r'<div class="unavailable" data-block="([^"]+)">([^<]*)</div>', html)


@pytest.fixture()
def root(tmp_path):
    world(tmp_path)
    return tmp_path


# ============================================================================================ registration
def test_blocks_and_analyses_are_registered():
    for name in ('capacity_curve', 't_risk_bias', 't_report_cards', 't_monitor_baseline', 'ops_loop', 't_integrity',
                 't_trial_ledger'):
        assert P.BLOCKS[name] is P3.BLOCKS[name]
    for name in ('capcurve', 'bias', 'cards', 'monitor_base', 'ops', 'integrity', 'ledger'):
        assert P.ANALYSES[name] is P3.ANALYSES[name]


# ============================================================================================ full render
def test_full_render_has_every_section_no_unavailable_and_valid_svgs(root):
    html = build(root, cfg_for(root))
    assert unavailable(html) == []
    for tid in ('t-capacity-curve', 't-risk-bias', 't-cards-v61', 't-monitor-baseline', 't-monitor-sleeves',
                't-ops-decide', 't-ops-reconcile', 't-integrity', 't-pbo', 't-trial-ledger', 't-cells'):
        assert f'id="{tid}"' in html, tid
    for fid in ('fig-capacity-curve', 'fig-ops-loop', 'fig-ledger-n'):
        assert f'id="{fid}"' in html, fid
    svgs = re.findall(r'<svg class="chart".*?</svg>', html, flags=re.S)
    assert len(svgs) >= 4
    assert all(C.validate_svg(s) == [] for s in svgs)
    assert 'unresolved' not in html  # every prose placeholder on the new analyses resolved


def test_every_number_names_its_file_with_sha(root):
    html = build(root, cfg_for(root))
    for rel in ('stress/v7_extras.json', 'risk/bias_summary.json', 'cards/index.json', 'monitor/monitor.json',
                'ops/decide/2021-08-04/decision.json', 'ops/recon-self/reconcile.json', 'summ/n33.json',
                'summ/pbo.json', 'ledger/trials.jsonl'):
        s = sha((root / rel).read_bytes())
        assert f'{rel} (sha256 {s[:12]})' in html, rel


# ============================================================================================ 1. capacity
def test_crossing_interpolates_in_log_nav():
    x, how = P3._crossing([(0.5e9, 1.3), (1e9, 1.2), (2e9, 1.1), (4e9, 0.95), (8e9, 0.8)], 1.0)
    assert how == 'interpolated' and x == pytest.approx(2 ** (1 + 0.1 / 0.15) * 1e9, rel=1e-12)
    assert P3._crossing([(1e9, 0.9), (2e9, 0.8)], 1.0)[0] is None
    assert 'every NAV' in P3._crossing([(1e9, 1.5), (2e9, 1.4)], 1.0)[1]


def test_capacity_present(root):
    ctx = make_ctx(root, cfg_for(root))
    cc = P3.an_capcurve(ctx)
    assert [r['multiple'] for r in cc['rows']] == MULTS  # sorted from the reversed file
    assert cc['cross_nav_bn'] == pytest.approx(2 ** (1 + 0.1 / 0.15), rel=1e-9)
    assert cc['sr_retained_max'] == pytest.approx(0.8 / 1.2)
    ko = cc['book']['S2-KO']
    assert ko['cost_bps_per_traded_dollar'] == pytest.approx(2.7e7 / 2.7e10 * 1e4)
    assert ko['csv_ok'] is True and cc['notes'] == []
    html = build(root, cfg_for(root))
    sec = section(html, 'cap')
    assert '+1.250' in sec and '+1.330' in sec and 'x1 row = S2 bit for bit: yes' in sec
    assert '$3.17bn' in sec  # the prose placeholder


def test_capacity_absent_extras_is_marked_not_available(root):
    (root / 'stress/v7_extras.json').unlink()
    html = build(root, cfg_for(root))
    na = unavailable(html)
    assert [n for n, _ in na] == ['capacity_curve'] and 'FileNotFoundError' in na[0][1]
    assert 'id="t-risk-bias"' in html  # the other sections still render


def test_capacity_malformed_schema_and_pin_mismatch(root):
    put(root, 'stress/v7_extras.json', {'schema': 'something/v9', 'capacity': []})
    na = unavailable(build(root, cfg_for(root)))
    assert na and na[0][0] == 'capacity_curve' and 'schema' in na[0][1]
    write_capacity(root)
    cfg = cfg_for(root)
    cfg['capacity_curve']['extras'] = {'path': 'stress/v7_extras.json', 'sha256': 'f' * 64}
    na = unavailable(build(root, cfg))
    assert na and 'sha256' in na[0][1] and 'pinned' in na[0][1]
    put(root, 'stress/v7_extras.json', b'{"schema": ')
    na = unavailable(build(root, cfg_for(root)))
    assert na and 'not valid JSON' in na[0][1]


def test_capacity_missing_cost_law_csv_blanks_that_book_only(root):
    (root / f'stress/daily_{FIM}.csv').unlink()
    cc = P3.an_capcurve(make_ctx(root, cfg_for(root)))
    assert cc['book']['S2-FIM'].get('cost_bps_per_traded_dollar') is None
    assert cc['book']['S2-FIM']['net_sharpe'] == 1.33
    assert any('S2-FIM' in n for n in cc['notes'])
    html = build(root, cfg_for(root))
    assert unavailable(html) == [] and 'S2-FIM: cost per traded dollar n/a' in html


# ============================================================================================ 2. risk bias
def test_bias_present_without_and_with_book(root):
    b = P3.an_bias(make_ctx(root, cfg_for(root)))
    assert [r['family'] for r in b['rows']] == ['factor', 'random'] and b['absent'] == ['book']
    assert b['structural_factors'] == 2 and b['fam']['random']['inb252'] is False and b['fam']['factor']['inb63'] is True
    html = build(root, cfg_for(root))
    assert 'Family book not in this run' in html
    write_bias(root, book=True)
    b = P3.an_bias(make_ctx(root, cfg_for(root)))
    assert [r['family'] for r in b['rows']] == ['factor', 'random', 'book'] and b['absent'] == []


def test_bias_absent_and_malformed(root):
    (root / 'risk/manifest.json').unlink()
    html = build(root, cfg_for(root))
    assert unavailable(html) == [] and 'Risk manifest not available' in html
    (root / 'risk/bias_summary.json').unlink()
    na = unavailable(build(root, cfg_for(root)))
    assert [n for n, _ in na] == ['t_risk_bias']
    put(root, 'risk/bias_summary.json', {'schema': 'atx.risk-bias/v2', 'families': {}})
    na = unavailable(build(root, cfg_for(root)))
    assert na and 'no families' in na[0][1]


# ============================================================================================ 3. report cards
def test_cards_rows_runner_check_and_manifest(root):
    rc = P3.an_cards(make_ctx(root, cfg_for(root)))
    s = rc['sets'][0]
    by = {r['id']: r for r in s['rows']}
    assert by['alpha']['runner'] is True and by['beta']['runner'] is False and by['gamma']['runner'] is None
    assert s['missing'] == ['delta'] and s['manifest_mismatch'] == ['beta']
    assert by['alpha']['ic21'] == pytest.approx(0.03) and by['alpha']['manifest_ok'] is True
    assert (rc['runner_pass'], rc['runner_fail'], rc['runner_na']) == (1, 1, 2)
    html = build(root, cfg_for(root))
    assert 'cards missing or unreadable: delta' in html and 'details class="card"' in html
    assert '>REDUNDANT<' in html and '>ADMITTED<' in html


def test_cards_second_set_absent_first_still_renders(root):
    cfg = cfg_for(root)
    cfg['report_cards']['sets'].append({'key': 'v70', 'label': 'set two', 'dir': 'cards-v70'})
    html = build(root, cfg)
    assert 'id="t-cards-v61"' in html and 'report cards set two: not available' in html
    write_cards(root, 'cards-v70')
    html = build(root, cfg)
    assert 'id="t-cards-v70"' in html and 'report cards set two: not available' not in html


def test_cards_all_absent_or_malformed(root):
    (root / 'cards/index.json').unlink()
    na = unavailable(build(root, cfg_for(root)))
    assert [n for n, _ in na] == ['t_report_cards'] and 'no report-card set readable' in na[0][1]
    put(root, 'cards/index.json', {'schema': 'atx.alpha-report-card-index/v1', 'candidates': 'nope'})
    na = unavailable(build(root, cfg_for(root)))
    assert na and na[0][0] == 't_report_cards'


# ============================================================================================ 4. monitor baseline
def test_monitor_present_names_the_alarming_sleeve(root):
    mb = P3.an_monitor_base(make_ctx(root, cfg_for(root)))
    assert mb['alarming_names'] == ['hot'] and 'turnover_cusum 9.50' in mb['alarming'][0]
    assert mb['check_counts'] == {'ok': 2, 'warn': 0, 'alarm': 1, 'n/a': 2}
    assert [f['member'] for f in mb['flagged']] == ['hot', 'luke']
    html = build(root, cfg_for(root))
    sec = section(html, 'ops')
    assert 'alarm: hot (turnover_cusum 9.50)' in sec and 'alarm hot' in sec


def test_monitor_absent_and_malformed(root):
    (root / 'monitor/monitor.json').unlink()
    na = unavailable(build(root, cfg_for(root)))
    assert [n for n, _ in na] == ['t_monitor_baseline']
    put(root, 'monitor/monitor.json', {'schema': 'atx.book-monitor/v0', 'checks': {}})
    na = unavailable(build(root, cfg_for(root)))
    assert na and 'schema' in na[0][1]


# ============================================================================================ 5. operating loop
def test_ops_present_gates(root):
    op = P3.an_ops(make_ctx(root, cfg_for(root)))
    assert op['parity_total'] == 0 and op['wall_ratio'] == pytest.approx(1.2)
    assert [r['caught'] for r in op['reconcile']] == [True, True] and (op['expect_met'], op['expect_n']) == (2, 2)
    assert op['decisions'][0]['health_warn'] == ['data_freshness'] and op['holdings_rows'] == 12345
    html = build(root, cfg_for(root))
    sec = section(html, 'ops')
    assert 'Operating loop acceptance' in sec and '12,345 rows' in sec and 'FAIL' not in sec.split('t-ops-decide')[0]


def test_ops_planted_break_missed_and_partial_absence(root):
    cfg = cfg_for(root)
    cfg['ops_loop']['reconcile'][1]['expect']['extra'] = 2  # the planted run no longer matches
    (root / 'ops/decide/2021-08-04/decision.json').unlink()
    op = P3.an_ops(make_ctx(root, cfg))
    assert op['reconcile'][1]['caught'] is False and op['n_decisions_read'] == 1
    html = build(root, cfg)
    assert unavailable(html) == [] and 'chip fail' in section(html, 'ops')
    assert 'decision 2021-08-04: FileNotFoundError' in html


def test_ops_all_absent(root):
    cfg = cfg_for(root)
    cfg['ops_loop'] = {'decisions': [{'asof': '2021-08-04', 'dir': 'nowhere'}], 'reconcile': [{'label': 'x', 'dir': 'nope'}]}
    na = unavailable(build(root, cfg))
    assert [n for n, _ in na] == ['ops_loop'] and 'no decision.json or reconcile.json readable' in na[0][1]


# ============================================================================================ 6. integrity
def test_integrity_runs_final_and_gate_reads_only_the_cell_count_dsr(root):
    it = P3.an_integrity(make_ctx(root, cfg_for(root)))
    assert [f['dsr'] for f in it['finals']] == [0.903, 0.766] and it['final']['run'] == 'n33'
    assert it['n_cells'] == 33 and it['missing'] == ['noeff (n32)', 'nocc (n32)']
    rows = {(r['run'], r['name']): r for r in it['rows']}
    assert rows[('n33', 'final')]['freeze'] is False  # DSR .766 < .95 although eff-N is .823 and SR passes
    noeff = rows[('n33', 'noeff')]
    assert noeff['dsr_eff'] is None and noeff['dsr'] == 0.96 and noeff['freeze'] is True
    nocc = rows[('n33', 'nocc')]
    assert nocc['dsr'] is None and nocc['dsr_eff'] == 0.99 and nocc['freeze'] is None  # never substituted
    assert next(c for c in nocc['checks'] if c['metric'] == 'deflated.dsr')['passed'] is None
    assert it['pbo'][0]['pbo'] == 0.35
    html = build(root, cfg_for(root))
    assert 'never replaced by the cell-count DSR' in html and 'UNMET' in html


def test_integrity_absent_primary_non_primary_and_pbo(root):
    (root / 'summ/n32.json').unlink()
    (root / 'summ/pbo.json').unlink()
    it = P3.an_integrity(make_ctx(root, cfg_for(root)))
    assert len(it['runs']) == 1 and any('n32' in n for n in it['notes']) and any('PBO grid' in n for n in it['notes'])
    html = build(root, cfg_for(root))
    assert 't_integrity' not in [n for n, _ in unavailable(html)]
    (root / 'summ/n33.json').unlink()
    na = unavailable(build(root, cfg_for(root)))
    assert 't_integrity' in [n for n, _ in na]
    put(root, 'summ/n33.json', {'rows': []})
    na = dict(unavailable(build(root, cfg_for(root))))
    assert 'expected a list of cell rows' in na['t_integrity']


def test_integrity_single_nav_summ_form(root):
    cfg = cfg_for(root)
    cfg['integrity'].pop('runs')
    cfg['integrity']['nav_summ'] = 'summ/n33.json'
    it = P3.an_integrity(make_ctx(root, cfg))
    assert len(it['runs']) == 1 and it['final']['dsr'] == 0.766


# ============================================================================================ 7. trial ledger
def test_ledger_present_counts_verdicts_eras_and_n_match(root):
    tl = P3.an_ledger(make_ctx(root, cfg_for(root)))
    assert tl['n'] == 4 and tl['unreadable'] == 1
    assert [(e['era'], e['cum']) for e in tl['eras']] == [('v5', 2), ('final', 3), ('v7 spo', 4)]
    by = {r['name']: r for r in tl['rows']}
    assert by['v61u-spo-v1']['vkind'] == 'defect' and by['v61u-spo-v1']['label'] == 'spo-v1'
    assert by['final']['vkind'] == 'accepted' and by['v5-a']['vkind'] is None
    assert by['v5-b']['vkind'] == 'rejected'  # an explicit kind wins over the text rules
    html = build(root, cfg_for(root))
    assert '1 ledger lines were unreadable' in html and '2 of 3' in html
    # ledger N (4) differs from the integrity N (33): flagged, not hidden
    assert 'nav_summ N 33: DIFFERS' in html


def test_ledger_absent_and_all_malformed(root):
    (root / 'ledger/trials.jsonl').unlink()
    na = unavailable(build(root, cfg_for(root)))
    assert [n for n, _ in na] == ['t_trial_ledger']
    put(root, 'ledger/trials.jsonl', '{bad\n{"schema": "other/v1"}\n')
    na = dict(unavailable(build(root, cfg_for(root))))
    assert 'no atx.trial-ledger/v1 lines (2 unreadable)' in na['t_trial_ledger']


# ============================================================================================ verdict badges
def test_cell_table_takes_new_cells_with_verdict_badges(root):
    html = build(root, cfg_for(root))
    sec = section(html, 'cells')
    assert sec.count('<tr') >= 5 and 'v7.0 cell' in sec and 'universe lo3' in sec
    assert '<span class="chip pass">ACCEPTED</span><span class="sub">PROMOTED</span>' in sec
    assert '<span class="chip fail">REJECTED</span><span class="sub">REJECTED (dSR &lt; 0)</span>' in sec
    assert '<span class="chip neutral">PENDING</span>' in sec
    assert '<span class="chip fail">REJECTED</span><span class="sub">not adopted</span>' in sec  # verdict_kind override
    plain = build(root, cfg_for(root, verdict_badges=None))
    assert 'chip neutral' not in section(plain, 'cells') and 'pending run' in section(plain, 'cells')


def test_verdict_rules_from_config_come_first(root):
    cfg = cfg_for(root, verdict_badges={'rules': [{'kind': 'defect', 'match': 'pending'}]})
    sec = section(build(root, cfg), 'cells')
    assert '<span class="chip warn">DEFECT</span><span class="sub">pending run</span>' in sec


def test_missing_config_key_marks_only_that_block(root):
    html = build(root, cfg_for(root, risk_bias=None, trial_ledger=None))
    names = [n for n, _ in unavailable(html)]
    assert sorted(names) == ['t_risk_bias', 't_trial_ledger']
    assert 'id="t-capacity-curve"' in html


def test_xy_chart_log_axis_ticks_and_labels():
    s = C.xy_chart([{'name': 'a', 'color': 's2', 'points': [(0.5, 1.0, 'lo'), (8, 2.0, 'hi', 'below')]},
                    {'name': 'b', 'color': 'flat', 'points': [(1, 1.5, 'b', 'left')]}],
                   x_label='x', y_label='y', x_fmt='g', x_log=True, x_ticks=[0.5, 1, 2, 4, 8],
                   y_refs=[{'value': 1.2, 'label': 'ref'}])
    assert C.validate_svg(s) == [] and all(f'>{t}<' in s for t in ('0.5', '1', '2', '4', '8')) and '>ref<' in s
    xs = [float(m) for m in re.findall(r'<circle cx="([\d.]+)"', s)]  # series a: 0.5, 8; series b: 1
    assert len(xs) == 3
    # log axis: 0.5 -> 1 is one doubling of four, to within the 0.1 px rounding of the coordinates
    assert math.isclose((xs[2] - xs[0]) / (xs[1] - xs[0]), 0.25, abs_tol=1e-3)
