"""Render checks of the v8 pitch config (docs/plans/mega-alpha-v8-pitch.config.json) on synthetic inputs.

The config is read as committed; only its root moves to a short temp directory (Windows 260-character path limit), where
``test_mega_report_v8.world`` writes a synthetic file at every v8 input path the config names and ``book_world`` one at
every path the book-level blocks (``v8_book``) read. No real data is read: every input is synthetic (the tracked
pre-registration and literature documents are read in one test, as the pitch reads them).

- every input present (verdicts filled in): 0 unavailable blocks, every placeholder resolved, every input listed as read;
- each v8 input missing: exactly one v8 block unavailable, its owner's, naming the path;
- each book input missing: exactly the book blocks that read it, each naming the path (and the one v8 owner of a
  shared file); a sealed or content-sealed file is refused;
- the page header: the session span and the final cell; the ladder refuses the committed pending verdicts;
- the planning value, the seal check on a sealed path, and the v7 config untouched.

Run: python -m pytest atx-impl/tools/test_mega_report_v8_render.py -q
"""
import copy
import datetime as dt
import hashlib
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_mega_report_pitch3 as T3  # noqa: E402
import test_mega_report_v8 as T  # noqa: E402
from engine_tools import research_window as RW  # noqa: E402
from mega_report import data as D  # noqa: E402
from mega_report import pitch as P  # noqa: E402
from mega_report import v8 as V  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
V8_CONFIG = REPO / 'docs/plans/mega-alpha-v8-pitch.config.json'
V7_CONFIG = REPO / 'docs/plans/mega-alpha-v7-pitch.config.json'
V7_CONFIG_SHA256 = '73f80583b14d93a602f0934a315e3d9c9cb808c36e65022fb03e8d017e975a3b'  # LF bytes at 39926caa
PLANNING = 'live net Sharpe .7 to 1.0 [est] against the TRAIN figure'
# the verdicts root records once the cells ran (the committed config says "pending run"): R-1..R-7 accepted (R-7 is
# V8-F, as committed) and one branch of Rulings E-38 / E-45 for the optional cells (Ruling PM4-8): R-6 accepted, so
# R-9's frontier is undefined (its pending verdict stays); R-8, R-10 and R-11 rejected on R-7 (the synthetic world gives
# a recorded reject a negative paired dSR); R-12 not formed (no library v8.2 candidate passed the screen, E-38)
VERDICTS = {'B0a': 'accepted (re-base)', 'B0b': 'ACCEPTED', 'B0c': 'accepted (baseline)',
            **{f'R-{i}': 'ACCEPTED' for i in range(1, 8)},
            'R-8': 'REJECTED', 'R-9a': 'pending run', 'R-9b': 'pending run', 'R-9c': 'pending run', 'R-10': 'REJECTED',
            'R-11': 'REJECTED', 'R-12': 'undefined (E-38: no library v8.2 candidate passed the screen; slot 51 unused)'}
PARENTS = {'R-10': 'R-7', 'R-11': 'R-7'}   # the last accepted cell before each (R-8 and R-10 rejected)
OPTIONAL = ('R-8', 'R-9a', 'R-9b', 'R-9c', 'R-10', 'R-11', 'R-12')   # the cells Ruling PM4-8 added to the ladder


def committed_config() -> dict:
    cfg = json.loads(V8_CONFIG.read_text(encoding='utf-8'))
    cfg['root'] = '.'  # the temp root the config is written into
    return cfg


def retarget_final(cfg: dict, key: str) -> dict:
    """v8.final and the top-level final (page header, book sections) moved to the ladder cell ``key``, as the PM sets
    them when the last accepted cell is not R-7: the V8-F book cell takes that cell's directory."""
    fc = next(c for c in cfg['v8']['cells'] if c['key'] == key)
    name = fc['dir'].rsplit('/', 1)[-1][len(cfg['cell_prefix']):]
    old = cfg['final']
    cfg['v8']['final'], cfg['final'] = key, name
    next(c for c in cfg['cells'] if c['group'] == 'v8-final')['dir'] = fc['dir']
    for it in cfg['rolling']['cells']:
        if it['cell'] == old:
            it['cell'] = name
    return cfg


def v8_config(verdicts: dict | None = None, parents: dict | None = None, final: str | None = None) -> dict:
    """The committed config with every cell's verdict recorded (``VERDICTS``, then ``verdicts``), the parents of
    ``PARENTS`` (then ``parents``) and every manual criterion part read (as after the runs: the PM's capacity-curve
    readings); ``final`` retargets V8-F."""
    cfg = committed_config()
    verdicts, parents = dict(VERDICTS, **(verdicts or {})), dict(PARENTS, **(parents or {}))
    for c in cfg['v8']['cells']:
        c['verdict'] = verdicts[c['key']]
        if c['key'] in parents:
            c['parent'] = parents[c['key']]
        for ch in (c.get('criterion') or {}).get('checks') or []:
            if 'metric' not in ch:
                ch['met'] = True
    return retarget_final(cfg, final) if final else cfg


INPUTS = V.inputs(v8_config())
BOOK_SPECS = [b for sec in v8_config()['layout'] for b in sec['blocks'] if isinstance(b, dict) and b['type'] == 'v8_book']


def _book_needs() -> list[list[str]]:
    with tempfile.TemporaryDirectory(prefix='v8n') as d:  # an empty root: the context reads nothing
        ctx = T.make_ctx(Path(d), v8_config())
        return [V.book_inputs(ctx, s) for s in BOOK_SPECS]


BOOK_NEEDS = _book_needs()
BOOK_INPUTS = list(dict.fromkeys(p for need in BOOK_NEEDS for p in need))


@pytest.fixture()
def root():
    d = Path(tempfile.mkdtemp(prefix='v8p'))
    try:
        yield d
    finally:
        shutil.rmtree(d, ignore_errors=True)


def text_of(html: str) -> str:
    return re.sub(r'\s+', ' ', re.sub(r'<[^>]+>', ' ', html))


# ============================================================================================ synthetic book world
ND, NI = 12, 64  # role geometry of the synthetic signal cache
IDS, THEMES = ('a', 'b', 'c'), ('value', 'value', 'low_risk')
ROLE_SHA = 'ab' * 32
DAILY_COLS = ('session_ns', 'return_observation', 'executed', 'net_return', 'gross_return', 'pretrade_gross_dollars',
              'traded_dollars', 'one_way_turnover_gmv', 'gross_leverage', 'net_leverage', 'posttrade_nav',
              'pretrade_nav', 'long_dollars', 'short_dollars', 'held_names', 'capped_fills', 'unfilled_dollars',
              'blocked_absent', 'blocked_short_names', 'fills', 'stale_names', 'planned_forced', 'planned_turnover',
              'trade_cost_return', 'linear_cost_dollars', 'impact_cost_dollars', 'trade_cost_dollars',
              'short_financing_gc_dollars', 'short_financing_warm_dollars', 'short_financing_special_dollars',
              'borrow_return', 'writeoff_return', 'long_financing_return')


def train_sessions() -> list[int]:
    """Weekday sessions of TRAIN (from the research window, never a literal)."""
    d, end = dt.date.fromisoformat(RW.TRAIN_BEGIN_DATE), dt.date.fromisoformat(RW.TRAIN_END_DATE)
    out = []
    while d < end:
        if d.weekday() < 5:
            out.append((d - dt.date(1970, 1, 1)).days * T.NS_PER_DAY)
        d += dt.timedelta(days=1)
    return out


def daily_text(seed: int, sessions: list[int]) -> str:
    rng = np.random.default_rng(seed)
    net = rng.normal(0.0004, 0.004, len(sessions))
    nav = 1e9 * np.cumprod(1 + net)
    lines = [','.join(DAILY_COLS)]
    for k, (s, r, v) in enumerate(zip(sessions, net, nav)):
        vals = (s, 0 if k == 0 else 1, 1, r, r + 0.0002, 0.0 if k == 0 else 0.97 * v, 4e7, 0.04 + 0.001 * (k % 7),
                0.97, 0.001, v, v, 0.49 * v, 0.48 * v, 800, 5 + k % 3, 1e5, k % 2, 1, 1500, 0, 1e6, 4e7, 1e-4,
                3e3, 2e3, 5e3, 100.0, 50.0, 10.0, 2e-5, 0.0, 1e-5)
        # float(x): numpy 2's repr of an np.float64 is 'np.float64(...)'; the same digits under numpy 1
        lines.append(','.join(repr(float(x)) if isinstance(x, float) else str(x) for x in vals))
    return '\n'.join(lines) + '\n'


def scen_block(sid: str, csv_sha: str, net: float) -> dict:
    tiers = {'gc': 0.8, 'warm': 0.15, 'special': 0.05}
    return {'scenario': sid, 'net_sharpe': net, 'gross_sharpe': net + 0.3, 'hac_t': 2.1, 'ann_mean': 0.07,
            'ann_vol': 0.06, 'max_drawdown': 0.05, 'daily_csv_sha256': csv_sha,
            'costs': {'summed_trade_cost_return': 0.1, 'summed_borrow_return': 0.02},
            'financing': {'summed_long_financing_return': 0.01, 'short_dollar_share_by_tier': {'mean': tiers, 'p95': tiers},
                          'short_financing_by_tier_dollars': {'gc': 1e6, 'warm': 5e5, 'special': 1e5},
                          'member_decisions_by_tier': {'gc': 9000, 'warm': 900, 'special': 90},
                          'spec': {'tier_fee_bps': {'gc': 25, 'warm': 150, 'special': 600}, 'long_spread_bps': 50,
                                   'short_spread_bps': 25},
                          'long_financing_dollars': 2e6, 'blocked_short_name_decisions': 12, 'blocked_short_dollars': 3e6},
            'calendar_year_returns': [{'year': y, 'net_compounded_return': 0.05 + 0.01 * k}
                                      for k, y in enumerate(V.train_years())]}


def write_cell(root: Path, rel_dir: str, sids: list[str], seed: int, sessions: list[int]) -> None:
    scen = []
    for k, sid in enumerate(sids):
        text = daily_text(seed + k, sessions)
        T.put(root, f'{rel_dir}/daily_{sid}.csv', text)
        T.put(root, f'{rel_dir}/events_{sid}.csv', f'session_ns,kind,pnl_dollars\n{sessions[5]},write-off,-12000.0\n')
        scen.append(scen_block(sid, hashlib.sha256(text.encode()).hexdigest(), 1.2 - 0.05 * k))
    T.put(root, f'{rel_dir}/summary.json', {'scenarios': scen, 'role_sha256': ROLE_SHA,
                                           'source_bindings': {'library_sha256': 'cd' * 32},
                                           'locate_in_aim': {'zeroed_special_short_aims': 3}})
    T.put(root, f'{rel_dir}/recipe.json', {'scenarios': [
        {'id': sid, 'cost_rule': 'sqrt-impact-v1', 'half_spread_bps': 2.0, 'commission_bps': 1.0, 'impact_y': 0.7,
         'impact_delta': 0.5, 'max_participation': 0.01, 'stale_exit_sessions': 5, 'terminal_haircut_long': -0.3,
         'terminal_haircut_short': 0.3, 'financing': {'id': 'swap-fin-v1', 'day_count': 360, 'long_spread_bps': 50,
                                                      'short_spread_bps': 25, 'block_special_shorts': True}}
        for sid in sids]})


def write_capacity(root: Path, cc: dict) -> None:
    d = cc['dir']
    rows = [{'multiple': m, 'equivalent_initial_nav': m * 1e9, 'net_sharpe': n, 'gross_sharpe': n + 0.3,
             'cost_bps_per_traded_dollar': c} for m, n, c in zip(T3.MULTS, T3.NETS, T3.COSTS)]
    T.put(root, cc['extras'], {'schema': 'atx.nav-v7-extras/v1', 'capacity': rows, 'capacity_rule': 'synthetic',
                               'capacity_x1_equals_primary_bit_for_bit': True})
    scen = []
    for b in [cc['primary'], *cc['beside']]:
        text = T3.daily_csv(2.7e10, 3.0e7)
        T.put(root, f"{d}/daily_{b['scenario']}.csv", text)
        scen.append({'scenario': b['scenario'], 'net_sharpe': 1.2, 'gross_sharpe': 1.5,
                     'daily_csv_sha256': hashlib.sha256(text.encode()).hexdigest()})
    T.put(root, cc['summary'], {'scenarios': scen})


def write_signals(root: Path, cfg: dict) -> None:
    ac, al = cfg['analysis'], cfg['alphas']
    dsl = {i: f'rank(close) * {k}' for k, i in enumerate(IDS)}
    T.put(root, al['library'], {'candidates': [{'id': i, 'theme': t, 'dsl': dsl[i]} for i, t in zip(IDS, THEMES)]})
    T.put(root, al['recipe'], {'lineage': [{'id': i, 'roster_order': k + 1, 'theme': t}
                                           for k, (i, t) in enumerate(zip(IDS, THEMES))]})
    role = al['roles'][0]
    # the admission is also the v8 member-horizon input: the v8 rows plus the library's members
    adm = T.admission()
    adm['candidates'] += [{'id': i, 'status': 'admitted'} for i in IDS]
    T.put(root, role['admission'], adm)
    T.put(root, role['weights'], {'weights': {i: 1 / len(IDS) for i in IDS}})
    T.put(root, f"{ac['role']}/manifest.json", {'dates': ND, 'instruments': NI, 'score_begin': 1, 'score_end': ND})
    T.put(root, f"{ac['role']}/member.u8", np.ones((ND, NI), dtype=np.uint8).tobytes())
    T.put(root, ac['fields_manifest'], {'fields': [{'name': 'close', 'sha256': '11' * 32}]})
    rng = np.random.default_rng(5)
    base = rng.normal(size=(ND, NI))
    entries = []
    for k, i in enumerate(IDS):
        dsl_sha = hashlib.sha256(dsl[i].encode()).hexdigest()
        stem = f'{i}.{dsl_sha[:16]}'
        data = np.ascontiguousarray((base * (1 - k) + rng.normal(size=(ND, NI))).astype('<f8')).tobytes()
        d = f"{ac['candidate_cache']}/{ROLE_SHA}"
        T.put(root, f'{d}/{stem}.f64', data)
        T.put(root, f'{d}/{stem}.json', {
            'schema': 'atx.dsl-candidate-signal/v2', 'candidate_id': i, 'dsl_sha256': dsl_sha,
            'library_sha256': 'cd' * 32, 'role_manifest_sha256': ROLE_SHA, 'role': 'train', 'dates': ND,
            'instruments': NI, 'bytes': len(data), 'payload': f'{stem}.f64',
            'payload_sha256': hashlib.sha256(data).hexdigest(), 'field_payload_sha256': {}})
        entries.append({'id': i, 'sidecar': f'{d}/{stem}.json'.replace('/', '\\'),
                        'payload': f'{d}/{stem}.f64'.replace('/', '\\'),
                        'payload_sha256': hashlib.sha256(data).hexdigest()})
    T.put(root, f"{ac['u_pass']}/summary.json", {'status': 'complete', 'roles': [
        {'role': 'train', 'manifest_sha256': ROLE_SHA, 'candidate_cache': {'directory': 'x', 'entries': entries}}]})
    ic = ['id,horizon,session_ns,rank_ic']
    for k, s in enumerate(train_sessions()[:80]):
        ic += [f'{i},5,{s},{0.01 * (j + 1) + 0.02 * np.sin(k + j)!r}' for j, i in enumerate(IDS)]
    T.put(root, f"{ac['u_pass']}/train_daily_ic.csv", '\n'.join(ic) + '\n')


def book_world(root: Path, cfg: dict) -> None:
    """A synthetic file at every path the config's book-level blocks read (``BOOK_INPUTS``)."""
    ses = train_sessions()
    by = {c['label']: c['dir'] for c in cfg['cells']}
    sids = [s['id'] for s in cfg['scenarios']]
    write_cell(root, by['V8-F (R-7 library v8.1)'], sids, 11, ses)
    write_cell(root, by['B0c baseline'], sids[:1], 31, ses)
    write_capacity(root, cfg['capacity_curve'])
    write_signals(root, cfg)


def full_world(root: Path, cfg: dict) -> None:
    T.world(root, cfg)
    book_world(root, cfg)


# ============================================================================================ the config itself
def test_v8_config_uses_registered_blocks_and_names_every_input():
    cfg = v8_config()
    types = [b if isinstance(b, str) else b['type'] for sec in cfg['layout'] for b in sec['blocks']]
    assert all(t in P.BLOCKS for t in types)
    assert set(V.BLOCKS) <= set(types)  # every v8 section is in the pitch
    assert [k for _, k, _ in INPUTS] == [   # R-9's frontier (undefined: R-6 accepted) and R-12 (not formed): none
        'v8.summ', *[f'v8.cells[{k}].paired' for k in ('B0b', 'R-1', 'R-2', 'R-3', 'R-4')], 'v8.cells[R-4].summary',
        'v8.cells[R-5].paired', 'v8.cells[R-5].summary', 'v8.cells[R-6].paired', 'v8.cells[R-6].summary',
        *[f'v8.cells[{k}].paired' for k in ('R-7', 'R-8', 'R-10', 'R-11')], 'v8.bundle', 'v8.diagnostics',
        'v8.member_horizon.card_index', 'v8.member_horizon.admission', 'v8.trial_ledger', 'v8.prereg', 'v8.literature']
    assert len({p for _, _, p in INPUTS}) == len(INPUTS)
    assert not any(D.path_is_sealed(p) for _, _, p in INPUTS)  # no input is refused by name
    assert cfg['v8']['re_screens'] == 8 and cfg['v8']['final'] in {c['key'] for c in cfg['v8']['cells']}


def test_book_group_trails_the_v8_sections_and_runs_every_book_block():
    cfg = v8_config()
    ids = [sec['id'] for sec in cfg['layout']]
    assert ids[-2:] == ['book', 'appendix']
    v8_secs = [sec for sec in cfg['layout'] if sec['id'] != 'book']
    assert not [b for sec in v8_secs for b in sec['blocks'] if isinstance(b, dict) and b['type'] == 'v8_book']
    assert {s['block'] for s in BOOK_SPECS} == set(V.BOOK_BLOCKS)
    assert [s.get('kind') for s in BOOK_SPECS if s['block'] == 'fig_corr'] == ['signal', 'ic', 'theme']
    # the book blocks run on the V8-F cell: the top-level final is v8.final's cell
    fc = next(c for c in cfg['v8']['cells'] if c['key'] == cfg['v8']['final'])
    assert next(c['dir'] for c in cfg['cells'] if c['dir'].endswith('/' + cfg['cell_prefix'] + cfg['final'])) == fc['dir']
    assert not any(D.path_is_sealed(p) for p in BOOK_INPUTS)
    assert 'nav_summ_json' not in (cfg.get('inputs') or {})  # the v8 summ keeps its one owner


def test_v7_config_is_untouched_and_has_no_v8_block():
    raw = V7_CONFIG.read_bytes().replace(b'\r\n', b'\n')
    assert hashlib.sha256(raw).hexdigest() == V7_CONFIG_SHA256
    cfg = json.loads(raw)
    assert 'v8' not in cfg
    assert not [b for sec in cfg['layout'] for b in sec['blocks']
                if (b if isinstance(b, str) else b['type']).startswith('v8_')]


# ============================================================================================ registered criteria
TEMPLATE = REPO / 'docs/plans/mega-alpha-scorecard-v8.template.md'
# review P-2: every registered part of each cell's mechanical criterion (plan section 9; rulings E-14/E-14a, E-31, E-36), as
# the committed config's checks (texts and labels aside; a manual part is {'met': None} until the PM reads it)
REGISTERED_CHECKS = {
    'R-1': [{'metric': 'tau_gmv_mean', 'per': 'mean_gross_leverage_all_rows', 'op': 'le'}],
    'R-2': [{'metric': 'tau_gmv_mean', 'op': 'le'}],
    'R-3': [{'met': None}, {'metric': 'tau_gmv_mean', 'op': 'lt'}],
    'R-4': [{'metric': 'tau_gmv_mean', 'op': 'le', 'factor': 0.85}],
    'R-5': [{'met': None}, {'met': None},
            {'source': 'summary', 'scenario': 'S3', 'metric': 'net_sharpe', 'op': 'ge'}],
    'R-6': [{'metric': 'cost_bps_traded', 'op': 'le'},
            {'source': 'summary', 'metric': 'v7.spo_v3_tripwire.status', 'op': 'eq', 'value': 'clear'},
            {'source': 'summary', 'metric': 'v7.spo_v3_books.{primary}.limits_unmet', 'op': 'eq', 'value': 0},
            {'source': 'summary', 'metric': 'v7.spo_v3_books.{primary}.aim_correlation_traded_after.mean', 'op': 'ge',
             'value': 0.9}],
    'R-7': [{'metric': 'tau_gmv_mean', 'op': 'le'}],
    # Ruling PM4-8: the optional cells. R-8: E-43's band [.8, 1.2] x sigma_star 5% on every TRAIN year's realised
    # volatility of the S2 net series; R-9: report only, no check; R-10 / R-11: R-1's criterion (E-44); R-12: R-2's and
    # R-7's (PM4-9)
    'R-8': [{'source': 'years', 'metric': 'ann_vol', 'op': 'between', 'lo': 0.04, 'hi': 0.06}],
    'R-9a': [], 'R-9b': [], 'R-9c': [],
    'R-10': [{'metric': 'tau_gmv_mean', 'per': 'mean_gross_leverage_all_rows', 'op': 'le'}],
    'R-11': [{'metric': 'tau_gmv_mean', 'per': 'mean_gross_leverage_all_rows', 'op': 'le'}],
    'R-12': [{'metric': 'tau_gmv_mean', 'op': 'le'}]}
# Rulings E-38 / E-45 / E-37: when each optional cell is defined; R-8 always (E-40)
DEFINED_IF = {'R-9a': {'rejected': ['R-6']}, 'R-9b': {'rejected': ['R-6']}, 'R-9c': {'rejected': ['R-6']},
              'R-10': {'accepted': ['R-6', 'R-1']}, 'R-11': {'accepted': ['R-6', 'R-1']}, 'R-12': {'accepted': ['R-6']}}


def test_committed_criteria_carry_every_registered_part():
    cells = committed_config()['v8']['cells']
    crit = {c['key']: c.get('criterion') or {} for c in cells}
    assert [c['key'] for c in cells][-len(OPTIONAL):] == list(OPTIONAL) and set(crit) == set(REGISTERED_CHECKS) | {
        'B0a', 'B0b', 'B0c'}
    for key, want in REGISTERED_CHECKS.items():
        got = [{k: v for k, v in ch.items() if k not in ('text', 'label')} for ch in crit[key].get('checks') or []]
        assert got == want, key
    r8 = REGISTERED_CHECKS['R-8'][0]
    assert (r8['lo'], r8['hi']) == (pytest.approx(0.8 * 0.05), pytest.approx(1.2 * 0.05))   # E-43, sigma_star .05
    by = {c['key']: c for c in cells}
    assert {k: {kk: v for kk, v in (c.get('defined_if') or {}).items() if kk != 'ruling'}
            for k, c in by.items() if c.get('defined_if')} == DEFINED_IF
    assert {k for k, c in by.items() if c.get('report_only')} == {'R-9a', 'R-9b', 'R-9c'}
    assert all(not by[k].get('paired') for k in ('R-9a', 'R-9b', 'R-9c'))           # no acceptance: no paired test
    assert all(by[k]['defined_if']['ruling'] == 'E-38, E-37' for k in ('R-9a', 'R-9b', 'R-9c'))
    assert by['R-10']['defined_if']['ruling'] == by['R-11']['defined_if']['ruling'] == 'E-38, E-45'
    assert by['R-12']['defined_if']['ruling'] == 'E-38' and 'PM4-9' in crit['R-12']['text']
    assert 'E-43' in crit['R-8']['text'] and all('E-44' in crit[k]['text'] for k in ('R-10', 'R-11'))
    assert [by[k]['n'] for k in OPTIONAL] == [48, 49, 50, 51, 49, 50, 51]            # N <= 51 in either branch
    assert 'S3 not lower' in crit['R-5']['text']
    assert all(s in crit['R-6']['text'] for s in ('cost per traded dollar not higher', 'tripwire clear', 'E-31',
                                                  'E-31a', '>= .9', 'E-14', 'E-14a'))
    assert crit['R-7']['text'].startswith('turnover not higher') and 'E-36' in crit['R-7']['text']
    assert 'marginal' not in json.dumps(crit['R-7']['checks'])  # rule 8: marginal IC gates nothing (Ruling E-36)
    # the E-14 threshold is the C++ registered constant v3_aim_correlation_min
    assert REGISTERED_CHECKS['R-6'][3]['value'] == 0.9


def test_scorecard_template_carries_every_registered_part():
    text = TEMPLATE.read_text(encoding='utf-8')
    rows = {ln.split('|')[1].strip(): ln for ln in text.splitlines() if re.match(r'\| R-\d \| `mega-nav-v8-r', ln)}
    assert 'S3 not lower' in rows['R-5']
    assert all(s in rows['R-6'] for s in ('cost per traded dollar not higher', 'tripwire clear', 'limits_unmet 0',
                                          'E-31', 'E-31a', '>= .9', 'E-14', 'E-14a'))
    assert 'turnover not higher' in rows['R-7'] and 'E-36' in rows['R-7'] and 'marginal t' not in rows['R-7']
    ladder = {ln.split('|')[2].strip(): ln for ln in text.splitlines() if re.match(r'\| \d+ \| R-\d \|', ln)}
    assert 'S3 not lower ({{NAV[R-5].scenarios[S3].net_sharpe}} vs parent' in ladder['R-5']
    for part in ('{{NAV[R-6].v7.spo_v3_tripwire.status}}', '.limits_unmet}}',
                 '.aim_correlation_traded_after.mean}}'):
        assert part in ladder['R-6'], part
    assert 'turnover not higher' in ladder['R-7'] and 'report only, gates nothing' in ladder['R-7']
    assert '| `NAV[K]` | `<cell K dir>/summary.json` |' in text
    # Ruling PM4-8: the optional cells, each with its criterion and the branch that defines it (E-38, E-45)
    cells = {ln.split('|')[1].strip(): ln for ln in text.splitlines() if re.match(r'\| R-\d+[a-c]? \| `mega-nav-v8-r', ln)}
    ladder = {ln.split('|')[2].strip(): ln for ln in text.splitlines() if re.match(r'\| \d+ \| R-\d+[a-c]? \|', ln)}
    assert set(OPTIONAL) <= set(cells) and set(OPTIONAL) <= set(ladder) and 'optional (' not in text
    assert all('[.8, 1.2]' in rows['R-8'] and 'E-43' in rows['R-8'] for rows in (cells, ladder))
    assert '{{SUMM[R-8].year_table.3.ann_vol}}' in ladder['R-8']
    for k in ('R-9a', 'R-9b', 'R-9c'):
        assert all('report' in rows[k] and 'no acceptance' in rows[k] and 'R-6 is rejected' in rows[k]
                   for rows in (cells, ladder)), k
        assert 'PAIRED[' not in ladder[k], k
    for k in ('R-10', 'R-11'):
        assert all('planned turnover per unit gross not higher' in rows[k] and 'E-44' in rows[k] and
                   'R-6 and R-1 are accepted' in rows[k] for rows in (cells, ladder)), k
    assert all('turnover not higher' in rows['R-12'] and 'PM4-9' in rows['R-12'] and 'R-6 is accepted' in rows['R-12']
               for rows in (cells, ladder))
    assert 'undefined (<ruling id>)' in text and 'PM4-10' in text


def test_r5_and_r6_criteria_read_their_nav_summaries(root):
    """R-6's tripwire, E-31 limits and E-14 correlation, and R-5's S3 against R-4's, on synthetic NAV summaries."""
    cfg = v8_config()
    T.world(root, cfg)
    rows = {r['key']: r for r in V.ladder_rows(T.make_ctx(root, cfg))[0]}
    assert rows['R-6']['crit_met'] is True
    assert 'S3 net Sharpe (terminal-adverse) 0.9 ge parent 0.9' in rows['R-5']['crit_detail']
    s3 = next(s['id'] for s in cfg['scenarios'] if s['key'] == 'S3')
    for edit in (lambda d: d['v7']['spo_v3_tripwire'].update(status='tripped (not voiding)'),
                 lambda d: next(iter(d['v7']['spo_v3_books'].values())).update(limits_unmet=1),
                 lambda d: next(iter(d['v7']['spo_v3_books'].values()))['aim_correlation_traded_after'].update(
                     mean=0.89)):
        doc = T.nav_summary(cfg)
        edit(doc)
        T.put(root, 'build-equity/mega-nav-v8-r6/summary.json', doc)
        assert {r['key']: r for r in V.ladder_rows(T.make_ctx(root, cfg))[0]}['R-6']['crit_met'] is False
    lower = T.nav_summary(cfg, s3=0.5)
    assert next(s for s in lower['scenarios'] if s['scenario'] == s3)['net_sharpe'] == 0.5
    T.put(root, 'build-equity/mega-nav-v8-r5/summary.json', lower)
    r5 = {r['key']: r for r in V.ladder_rows(T.make_ctx(root, cfg))[0]}['R-5']
    assert 'S3 net Sharpe (terminal-adverse) 0.5 ge parent 0.9' in r5['crit_detail'] and r5['crit_met'] is False


# ============================================================================================ render
def test_v8_pitch_renders_every_block_when_every_input_exists(root):
    cfg = v8_config()
    full_world(root, cfg)
    html = T.build(root, cfg)
    assert T.unavailable(html) == []
    assert 'unresolved' not in html and '>n/a<' not in html.split('id="summary"')[1].split('id="window"')[0]
    for tid in ('t-v8-bundle', 't-v8-bundle-years', 't-v8-ladder', 't-v8-year-matrix', 't-v8-year-R-7',
                't-v8-diagnostics', 't-v8-g1a-members', 't-v8-member-horizon', 't-v8-contradictions',
                't-appendix-files'):
        assert f'id="{tid}"' in html, tid
    for _, _, rel in INPUTS:  # each input read (seal check passed) and listed with its SHA-256
        sha = hashlib.sha256((root / rel).read_bytes()).hexdigest()
        assert f'data-sort="{rel}">{rel}</td><td class="t" data-sort="read">read</td>' in html, rel
        assert sha in html, rel
    lad = html.index('id="t-v8-ladder"')
    assert html.count('<tr', lad, html.index('</table>', lad)) == 1 + len(cfg['v8']['cells'])   # undefined rows too
    assert html.count('<span class="chip neutral">UNDEFINED</span>', lad, html.index('</table>', lad)) == 4
    book = T.section(html, 'book')
    for fid in ('fig-equity', 't-drawdowns', 'fig-returns', 'fig-rolling', 't-retstats', 't-worst', 't-stress',
                't-cost-model', 't-financing', 'fig-costdec', 't-costdec', 't-attrib', 'fig-cost', 'fig-turnover',
                'fig-capacity-curve', 'fig-exposure', 'fig-fills', 'fig-corr-signal', 'fig-corr-ic',
                'fig-corr-theme-sig_corr', 't-theme-corr'):
        assert f'id="{fid}"' in book, fid
    for rel in BOOK_INPUTS:
        if not rel.endswith('/'):
            assert f'data-sort="{rel}">{rel}</td><td class="t" data-sort="read">read</td>' in html, rel


def test_v8_pitch_states_the_planning_value_and_the_re_screens(root):
    cfg = v8_config()
    T.world(root, cfg)
    txt = text_of(T.build(root, cfg))
    assert f'Plan on a {PLANNING} +1.400 (S2, 2020-2023)' in txt
    assert 'admission trials this sprint 7 plus 8 re-screens' in txt
    assert 'Freeze gate met: yes' in txt and 'OD-1 window disclosure (v8-prereg item 1)' in txt


@pytest.mark.parametrize('block,key,rel', INPUTS, ids=[k for _, k, _ in INPUTS])
def test_each_missing_input_is_one_named_unavailable_block(root, block, key, rel):
    cfg = v8_config()
    T.world(root, cfg, skip=(rel,))
    na = [(n, m) for n, m in T.unavailable(T.build(root, cfg)) if n.startswith('v8_')]  # book inputs are absent
    assert [n for n, _ in na] == [block]
    assert f'{rel}: ' in na[0][1] and 'missing' in na[0][1]


def test_a_sealed_input_path_is_refused_before_it_is_opened(root):
    cfg = v8_config()
    T.world(root, cfg)
    sealed = 'build-equity/mega-nav-v8-summ-2024.json'
    shutil.copy(root / cfg['v8']['summ'], root / sealed)
    cfg['v8']['summ'] = sealed
    html = T.build(root, cfg)
    na = [(n, m) for n, m in T.unavailable(html) if n.startswith('v8_')]
    assert [n for n, _ in na] == ['v8_year_table'] and 'refused (sealed)' in na[0][1]
    assert f'data-sort="{sealed}">{sealed}</td><td class="t" data-sort="refused (sealed)">' in html


def test_v8_pitch_on_the_tracked_documents(root):
    """The pre-registration and the literature review as committed: item 1 and the contradictions table parse."""
    cfg = v8_config()
    T.world(root, cfg)
    for key in ('prereg', 'literature'):
        rel = V._rel(cfg['v8'][key])
        src = REPO / rel
        if not src.exists():
            pytest.skip(f'{rel} not in this checkout')
        shutil.copy(src, root / rel)
    txt = text_of(T.build(root, copy.deepcopy(cfg)))
    assert '1. Window. TRAIN is [2020-01-01, 2024-01-01). Hidden: 2024-01-01 and later.' in txt
    m = re.search(r'Where the v8 literature notes contradict or update v6 and v7 \((\d+) rows: (\d+) contradicts', txt)
    assert m and int(m.group(1)) >= 20 and int(m.group(2)) >= 5


# ============================================================================================ book blocks
@pytest.fixture(scope='module')
def book_root():
    d = Path(tempfile.mkdtemp(prefix='v8b'))
    try:
        full_world(d, v8_config())
        yield d
    finally:
        shutil.rmtree(d, ignore_errors=True)


def _owners_of(rel: str) -> list[str]:
    return sorted([s['block'] for s, need in zip(BOOK_SPECS, BOOK_NEEDS) if rel in need]
                  + [b for b, _, p in INPUTS if p == rel])


@pytest.mark.parametrize('rel', BOOK_INPUTS)
def test_each_missing_book_input_marks_exactly_its_readers(book_root, rel):
    """A missing book input: every book block that reads it (and the v8 owner of a shared file) is unavailable,
    naming the path; every other block renders."""
    src = book_root / rel.rstrip('/')
    aside = book_root / '_aside'
    aside.mkdir(exist_ok=True)
    shutil.move(str(src), str(aside / 'x'))
    try:
        na = T.unavailable(T.build(book_root, v8_config()))
    finally:
        shutil.move(str(aside / 'x'), str(src))
    assert sorted(n for n, _ in na) == _owners_of(rel)
    assert all(f'{rel}: ' in m and 'missing' in m for _, m in na), na


def test_shared_admission_is_one_v8_block_and_the_book_blocks_that_read_it():
    rel = next(p for b, _, p in INPUTS if b == 'v8_member_horizon' and p.endswith('admission.json'))
    assert rel in BOOK_INPUTS
    assert _owners_of(rel) == sorted(['fig_corr', 'fig_corr', 'fig_corr', 't_theme_corr', 'v8_member_horizon'])


def test_a_content_sealed_daily_csv_is_refused_and_the_header_drops_it(root):
    cfg = v8_config()
    full_world(root, cfg)
    s2 = cfg['scenarios'][0]['id']
    rel = f'build-equity/mega-nav-v8-r7/daily_{s2}.csv'
    text = (root / rel).read_text(encoding='utf-8')
    last = text.strip().splitlines()[-1].split(',')
    last[0] = str(T.session_ns(2024, 1, 2))  # a session after the seal (synthetic)
    (root / rel).write_text(text + ','.join(last) + '\n', encoding='utf-8')
    html = T.build(root, cfg)
    na = T.unavailable(html)
    readers = sorted(s['block'] for s, need in zip(BOOK_SPECS, BOOK_NEEDS) if rel in need)
    assert sorted(n for n, _ in na) == readers and len(readers) >= 10
    assert all(f'{rel}: refused (sealed content)' in m and 'refusing (sealed)' in m for _, m in na)
    head = html.split('</header>')[0]
    assert '<dt>Window</dt><dd>TRAIN 2020-2023 (research-window-v2): n/a</dd>' in head
    assert '2024-01-02' not in html


def test_a_sealed_book_path_is_refused_by_name(root):
    cfg = v8_config()
    full_world(root, cfg)
    cfg['capacity_curve']['summary'] = 'build-equity/mega-nav-v8-r7-stress-2024/summary.json'
    na = T.unavailable(T.build(root, cfg))
    assert [n for n, _ in na] == ['capacity_curve']
    assert 'build-equity/mega-nav-v8-r7-stress-2024/summary.json: refused (sealed)' in na[0][1]


def test_book_block_config_errors_are_visible(root):
    cfg = v8_config()
    full_world(root, cfg)
    book = next(sec for sec in cfg['layout'] if sec['id'] == 'book')
    book['blocks'] = [{'type': 'v8_book', 'block': 'fig_ladder'}, {'type': 'v8_book', 'block': 'fig_rolling'}]
    cfg['rolling']['cells'][1]['cell'] = 'v8-r9'
    na = T.unavailable(T.build(root, cfg))
    assert na[0][0] == 'v8_book' and "'fig_ladder' is not one of" in na[0][1]
    assert na[1] == ('fig_rolling', "fig_rolling: not available (config: KeyError: \"cell 'v8-r9' not among the "
                                    "configured cells\")")


# ============================================================================================ header, ladder checks
def test_header_shows_the_session_span_and_the_final_cell(root):
    cfg = v8_config()
    full_world(root, cfg)
    head = T.build(root, cfg).split('</header>')[0]
    ses = train_sessions()
    span = f'{D.ns_to_date(ses[1]).isoformat()} to {D.ns_to_date(ses[-1]).isoformat()} ({len(ses) - 1} return sessions)'
    assert f'<dt>Window</dt><dd>TRAIN 2020-2023 (research-window-v2): {span}</dd>' in head
    assert '<dt>Final cell</dt><dd>V8-F (R-7 library v8.1): v8-r7</dd>' in head
    assert span.startswith(RW.TRAIN_BEGIN_DATE[:4]) and f'to {int(RW.TRAIN_END_DATE[:4]) - 1}-' in span


def test_committed_config_refuses_its_pending_verdicts(root):
    """As committed (every verdict "pending run"), the ladder shows one refusal per cell whose paired test was read and
    one for v8.final (no accepted cell); every other block renders. With R-6 and R-1 still pending no optional cell is
    undefined yet: R-8, R-10, R-11 and R-12 read their paired tests; R-9's frontier cells have none."""
    cfg = committed_config()
    full_world(root, cfg)
    na = T.unavailable(T.build(root, cfg))
    assert {n for n, _ in na} == {'v8_ladder'} and len(na) == 13
    msgs = [m for _, m in na]
    assert all(m.startswith('v8_ladder: refused (') for m in msgs)
    assert sum("was read but the verdict is still pending ('pending run')" in m for m in msgs) == 12
    assert "v8.final: 'R-7' is not the last accepted cell: no cell of the ladder is accepted" in msgs[-1]


def test_top_level_final_must_be_the_v8_final_cell(root):
    cfg = v8_config()
    full_world(root, cfg)
    cfg['final'] = 'v8-b0c'
    na = T.unavailable(T.build(root, cfg))
    refusals = [m for n, m in na if n == 'v8_ladder']
    assert refusals == ["v8_ladder: refused (final: 'v8-b0c' is build-equity/mega-nav-v8-b0c, not the cell of v8.final "
                        "'R-7' (build-equity/mega-nav-v8-r7))"]


# ============================================================================================ optional cells (PM4-8)
# Rulings E-38 / E-45 / PM4-8 / PM4-10 on the committed config: every branch, each defined cell decided consistently
# with the synthetic world (a recorded reject has a negative paired dSR), each parent the last accepted cell before it,
# V8-F the last accepted cell. (verdicts, parents, V8-F, the undefined cells, the ruling each renders)
FRONTIER = ('R-9a', 'R-9b', 'R-9c')
PITCH_BRANCHES = {
    'R-6 and R-1 accepted': (
        {'R-8': 'ACCEPTED', 'R-10': 'ACCEPTED', 'R-11': 'ACCEPTED', 'R-12': 'ACCEPTED'},
        {'R-10': 'R-8', 'R-11': 'R-10'}, 'R-12', dict.fromkeys(FRONTIER, 'undefined (E-38, E-37)')),
    'R-6 accepted, R-1 rejected': (
        {'R-1': 'REJECTED', 'R-8': 'ACCEPTED', 'R-10': 'pending run', 'R-11': 'pending run', 'R-12': 'ACCEPTED'},
        {'R-2': 'B0c', 'R-12': 'R-8'}, 'R-12',
        dict(dict.fromkeys(FRONTIER, 'undefined (E-38, E-37)'), **dict.fromkeys(('R-10', 'R-11'),
                                                                                'undefined (E-38, E-45)'))),
    'R-6 rejected': (
        {'R-6': 'REJECTED', 'R-8': 'ACCEPTED', **dict.fromkeys(FRONTIER, 'reported (frontier, no acceptance)'),
         'R-10': 'pending run', 'R-11': 'pending run', 'R-12': 'pending run'},
        {'R-7': 'R-5'}, 'R-8',
        dict(dict.fromkeys(('R-10', 'R-11'), 'undefined (E-38, E-45)'), **{'R-12': 'undefined (E-38)'})),
    'PM4-10, R-10 not formed': (
        {'R-8': 'ACCEPTED', 'R-10': 'undefined (PM4-10: fewer than 2T members admitted, member cap 1/(2T) infeasible)',
         'R-11': 'ACCEPTED', 'R-12': 'ACCEPTED'},
        {'R-11': 'R-8'}, 'R-12',
        dict(dict.fromkeys(FRONTIER, 'undefined (E-38, E-37)'),
             **{'R-10': 'undefined (PM4-10: fewer than 2T members admitted, member cap 1/(2T) infeasible)'}))}


@pytest.mark.parametrize('name', list(PITCH_BRANCHES))
def test_v8_pitch_renders_every_branch_with_no_unavailable_block(root, name):
    """The whole pitch (v8 sections and the book sections on V8-F) in each branch: 0 unavailable blocks; each undefined
    cell keeps its ladder row as "undefined (ruling)", names no input and has no year table; R-9's frontier cells, when
    defined, are reported with no rule."""
    verdicts, parents, final, undefined = PITCH_BRANCHES[name]
    cfg = v8_config(verdicts, parents, final)
    full_world(root, cfg)
    assert not [k for _, k, _ in V.inputs(cfg) if any(f'[{u}]' in k for u in undefined)]
    html = T.build(root, cfg)
    assert T.unavailable(html) == [], name
    lad = html.index('id="t-v8-ladder"')
    ladder = html[lad:html.index('</table>', lad)]
    assert ladder.count('<span class="chip neutral">UNDEFINED</span>') == len(undefined)
    for key, verdict in undefined.items():
        assert f'<span class="sub">{verdict}</span>' in ladder and f'id="t-v8-year-{key}"' not in html, key
    reported = [k for k in FRONTIER if k not in undefined]
    assert ladder.count('<span class="chip neutral">REPORT</span>') == len(reported)
    assert all(f'id="t-v8-year-{k}"' in html for k in reported)
    head = html.split('</header>')[0]
    assert f"<dt>Final cell</dt><dd>V8-F (R-7 library v8.1): {cfg['final']}</dd>" in head and cfg['final'] == {
        'R-12': 'v8-r12', 'R-8': 'v8-r8'}[final]


def test_v8_pitch_branch_errors_are_refused_visibly(root):
    """A decided verdict on an undefined cell, an accept on a report-only frontier cell, and a parent left on an
    undefined cell are each one visible refusal (counted as unavailable)."""
    a = PITCH_BRANCHES['R-6 and R-1 accepted']
    cfg = v8_config(dict(a[0], **{'R-9b': 'REJECTED'}), a[1], a[2])
    T.world(root, cfg)
    assert [w for w, _ in T._checks(root, cfg)] == ['v8.cells[R-9b].verdict']
    c = PITCH_BRANCHES['R-6 rejected']
    cfg = v8_config(dict(c[0], **{'R-9c': 'ACCEPTED'}), c[1], c[2])
    T.world(root, cfg)
    assert T._checks(root, cfg) == [('v8.cells[R-9c].verdict', "recorded accepted ('ACCEPTED') but R-9c is a "
                                                               "report-only cell: no acceptance (plan R-9; Ruling "
                                                               "PM4-8)")]
    b = PITCH_BRANCHES['R-6 accepted, R-1 rejected']
    cfg = v8_config(b[0], dict(b[1], **{'R-12': 'R-11'}), b[2])
    T.world(root, cfg)
    assert T._checks(root, cfg) == [('v8.cells[R-12].parent', "'R-11' is undefined: an undefined cell is never a "
                                                              "parent (it was not formed)")]
    na = T.unavailable(T.build(root, cfg))
    assert [n for n, _ in na if n.startswith('v8_')] == ['v8_ladder']


def test_accepted_optional_cells_whose_parent_row_is_missing_are_refused(root):
    """Ruling PM5-12, finding R6C-1: R-10, R-11 and R-12 recorded accepted while the parent's nav_summ row their
    criterion compares with is missing from the v8 nav_summ JSON (a note, no unavailable block of its own) are refused,
    naming that row; the parent, accepted with its own row missing, is refused too. Before the fix both rules were n/a
    with nothing unread, and nothing was refused."""
    a = PITCH_BRANCHES['R-6 and R-1 accepted']
    cfg = v8_config(*a[:3])
    T.world(root, cfg)
    summ, by = cfg['v8']['summ'], {c['key']: c for c in cfg['v8']['cells']}
    head = "recorded accepted ('ACCEPTED') but the rule of v8-prereg item 5 is n/a: "
    assert T._checks(root, cfg) == []
    for key in ('R-10', 'R-11', 'R-12'):
        par = by[key]['parent']
        name = V._base(by[par]['dir'])
        T.put(root, summ, [r for r in T.summ_rows(cfg) if V._base(r['dir']) != name])
        assert T._checks(root, cfg) == [
            (f'v8.cells[{par}].verdict', f'{head}its nav_summ row ({name}) is missing from {summ}'),
            (f'v8.cells[{key}].verdict', f"{head}the parent's nav_summ row ({name}) is missing from {summ}")], key


# the v8 pitch before Ruling PM4-8 (root 43a0447d): the committed config of then is today's without OPTIONAL; its
# verdicts recorded as v8_config records them, on full_world. The normalised HTML's SHA-256 and length.
PRE_PM4_8_SHA256 = 'ae009759f39ce7d12e2452f497bb083974b1f6910de0be0c933a59a6e2891e2f'
PRE_PM4_8_BYTES = 643481


def test_v8_pitch_without_the_optional_cells_renders_the_pre_pm4_8_bytes(root):
    """Identity: the ladder machinery of Ruling PM4-8 (branch states, years checks, report-only cells) moves no byte of
    a config that carries none of the new cells."""
    cfg = v8_config()
    cfg['v8']['cells'] = [c for c in cfg['v8']['cells'] if c['key'] not in OPTIONAL]
    full_world(root, cfg)
    norm = T._normalise(T.build(root, cfg), root)
    assert (len(norm), hashlib.sha256(norm.encode('utf-8')).hexdigest()) == (PRE_PM4_8_BYTES, PRE_PM4_8_SHA256)
