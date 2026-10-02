"""Write the INTERIM copy of the v8 pitch config, render 3 (Ruling PM6-11; replaces the PM5-27 / PM5-28 interim
configs). The registered config is read, never edited; every value this script replaces is asserted first.

The book is R-2 (lib-v80.json, library v8.0), the last accepted cell; the ladder is B0a, B0b, B0c, R-1 (matched gross,
r1-comp-v8-gm), R-2, R-3, R-4 with their paired files; cells not run and V8-F are named once, as text, and feed no
block. Diagnostics G-1..G-3 are B0c's (assembled copy-only from the eight split files). No input of the R-1 run at
L 1.247, of R-3's step-(1) calibration run, or of any placeholder path (r7, v8.1, the V8-F bundle) is named.
Usage: interim_report_make_config.py"""
import json
from pathlib import Path

ROOT = Path('C:/atx-wt/pool-2')
SRC = ROOT / 'docs/plans/mega-alpha-v8-pitch.config.json'
DST = ROOT / 'docs/plans/mega-alpha-v8-pitch.interim.config.json'

BE = 'build-equity'
DIRS = {'B0a': f'{BE}/mega-nav-v8-b0a-lo1-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247',
        'B0b': f'{BE}/mega-nav-v8-b0b-lo3-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247',
        'B0c': f'{BE}/mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247',
        'R-1': f'{BE}/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474',
        'R-2': f'{BE}/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80',
        'R-3': f'{BE}/mega-nav-v8-r3-aim-t.05-d.1-fixed-obdelta-x.05-loc-L1.1264',
        'R-4': f'{BE}/mega-nav-v8-r4-hb.1-t.05-d.1-fixed-obdelta-x.05-loc-L1.247'}
PAIRED = {k: f'{BE}/v8-cells-{k.lower().replace("-", "")}-bundle.json' for k in ('B0b', 'R-1', 'R-2', 'R-3', 'R-4')}
SUMM = f'{BE}/mega-nav-v8-summ-interim3.json'
STRESS = f'{BE}/mega-nav-v8-r2-v80-stress'          # R-2's NAV argv + --cost-v2 (report only; S2 = R-2's bit for bit)
DIAG = f'{BE}/mega-diagnostics-v8-b0c/diagnostics-v8.json'   # assembled copy-only from the eight split files
CARDS_C2 = f'{BE}/mega-cards-v8-r1-std-v80-c2/index.json'      # R-2's cards + --ic-theta --marginal-ic (report only)
ADM_C2 = f'{BE}/mega-weights-v8-r1-std-v80-c2/admission.json'  # R-2's fit + --report-f-theta (report only)
U_PASS, W_PASS = f'{BE}/mega-v8-b0b-train-u-v80-1', f'{BE}/mega-v8-r1w-train-std-v80-1'
WEIGHTS = f'{BE}/mega-weights-v8-r1-std-v80'
FORBIDDEN = ('r7', 'v81', 'mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.247', 'r3-aim-t.05-d.1-fixed-obdelta-x.05'
             '-loc-L1.247', 'bundle-b0c-v8f')
PREFIX = 'mega-nav-'


def name(key):
    return DIRS[key].split('/')[-1][len(PREFIX):]


cfg = json.loads(SRC.read_text(encoding='utf-8'))
assert cfg['final'] == 'v8-r7' and cfg['v8']['final'] == 'R-7' and cfg['v8']['base'] == 'B0c'

cfg['title'] = 'Mega-alpha v8: strategy pitch (INTERIM, render 3, owner stop 2026-10-02)'
cfg['kicker'] = ('atx mega-alpha \u00b7 platform v8 \u00b7 INTERIM: the book is R-2, the last accepted cell; no cumulative '
                 'test and no freeze gate yet \u00b7 in-sample TRAIN 2020-2023 \u00b7 for a quantitative PM')

# ---------------------------------------------------------------- top-level cells: the book (R-2) and the ladder cells
cfg['final'] = name('R-2')
cfg['receipt_suffix'] = '-run/receipt.json'
cfg['groups'] = [{'key': 'v8-final', 'label': 'R-2 (interim book)', 'token': 'g-final'},
                 {'key': 'v8-baseline', 'label': 'B0c baseline', 'token': 'g-v5-reference'},
                 {'key': 'v8-accepted', 'label': 'accepted step', 'token': 'g-parent'},
                 {'key': 'v8-rebase', 'label': 're-base (B0a, B0b)', 'token': 'g-grid'},
                 {'key': 'v8-rejected', 'label': 'not accepted', 'token': 'g-rejected'}]
LABEL = {'B0a': 'B0a v7.1 on lo1', 'B0b': 'B0b v7.1 on lo3', 'B0c': 'B0c baseline', 'R-1': 'R-1 composition (gm)',
         'R-2': 'R-2 library v8.0 (book)', 'R-3': 'R-3 persistence gain', 'R-4': 'R-4 hold band'}
GROUP = {'B0a': 'v8-rebase', 'B0b': 'v8-rebase', 'B0c': 'v8-baseline', 'R-1': 'v8-accepted', 'R-2': 'v8-final',
         'R-3': 'v8-rejected', 'R-4': 'v8-rejected'}
cfg['cells'] = [{'dir': DIRS[k], 'label': LABEL[k], 'group': GROUP[k]} for k in ('R-2', 'B0c', 'B0a', 'B0b', 'R-1',
                                                                                 'R-3', 'R-4')]
cfg['inputs'] = {'nav_summ_json': SUMM,
                 'extra_json': {k.lower().replace('-', ''): PAIRED[k] for k in ('R-1', 'R-2', 'R-3', 'R-4')}}

assert cfg['equity']['extra'][0]['cell'] == 'v8-b0c'
cfg['equity']['extra'][0]['cell'] = name('B0c')
assert [c['cell'] for c in cfg['rolling']['cells']] == ['v8-r7', 'v8-b0c']
cfg['rolling']['cells'][0].update(cell=name('R-2'), label='R-2', short='R-2')
cfg['rolling']['cells'][1]['cell'] = name('B0c')
cfg['turnover'] = {'groups': ['v8-final', 'v8-baseline', 'v8-accepted', 'v8-rebase', 'v8-rejected']}
cfg['ladder'] = {'cells': [{'cell': name('B0c'), 'label': 'B0c baseline'},
                           {'cell': name('R-1'), 'label': 'R-1 composition\n(gross matched)'},
                           {'cell': name('R-2'), 'label': 'R-2 library v8.0', 'end_label': 'R-2 (interim book)'}]}
cfg['scatter'] = {'labels': [name(k) for k in DIRS]}

v7 = json.loads((ROOT / 'docs/plans/mega-alpha-v7-pitch.config.json').read_text(encoding='utf-8'))
assert cfg['analysis']['compare_cell'] == 'v8-b0c'
cfg['analysis'] = {'u_pass': U_PASS, 'w_pass': W_PASS, 'role': f'{BE}/train-2020-2023-lo3',
                   'candidate_cache': f'{BE}/mega-candidate-cache-v8-lo3',
                   'fields_manifest': f'{BE}/train-2020-2023-lo3-fields-v10/manifest.json',
                   'horizons': [5, 21, 63], 'ic_corr_horizon': 5, 'theme_year_horizon': 21, 'tau_ic_horizon': 5,
                   'sig_corr_stride': 1, 'drawdowns': 5, 'compare_cell': name('B0c'),
                   'theme_classes': v7['analysis']['theme_classes'], 'combined_label': 'ew-theme-std-v1',
                   'universe_kept_label': 'kept members (linked-operating-v3)'}
cfg['alphas'] = {'library': 'atx-impl/strategies/fund_industry_ic_v80.json',
                 'recipe': 'atx-impl/strategies/fund_industry_ic_v80.recipe.v2.json',
                 'roles': [{'key': 'lo3', 'label': 'lo3 (R-2, library v8.0)', 'primary': True,
                            'admission': f'{WEIGHTS}/admission.json',
                            'weights': f'{WEIGHTS}/composition_weights.json'}]}
cc = cfg['capacity_curve']
assert cc['dir'] == f'{BE}/mega-nav-v8-r7-stress' and [b['label'] for b in cc['beside']] == ['S2-KO', 'S2-FIM']
cfg['capacity_curve'] = dict(cc, label='the R-2 book (library v8.0, the interim book)', dir=STRESS,
                             extras=f'{STRESS}/v7_extras.json', summary=f'{STRESS}/summary.json')

# ---------------------------------------------------------------- v8: the ladder of the cells that ran
v8 = cfg['v8']
v8['summ'] = SUMM
del v8['bundle']                      # V8-F (cumulative test, freeze gate) is pending: no block reads it
v8['final'] = 'R-2'
v8['diagnostics'] = DIAG
v8['member_horizon'] = {'card_index': CARDS_C2, 'admission': ADM_C2}
assert v8['re_screens'] == 8
by = {c['key']: c for c in v8['cells']}
keep = []
for k in ('B0a', 'B0b', 'B0c', 'R-1', 'R-2', 'R-3', 'R-4'):
    c = by[k]
    assert c['verdict'] == 'pending run', k
    c['dir'] = DIRS[k]
    if k in PAIRED:
        c['paired'] = PAIRED[k]
    keep.append(c)
by['B0a']['verdict'] = 're-base ledgered'
by['B0b']['verdict'] = 'accepted'
by['B0c']['verdict'] = 'baseline by declaration'
assert by['R-1']['label'] == 'R-1 composition v8'
by['R-1']['label'] = 'R-1 composition v8 at matched gross (r1-comp-v8-gm)'
by['R-1']['verdict'] = 'accepted (spec r1-comp-v8-gm.json: L set so that its all-rows gross matches B0c, Ruling PM6-6)'
by['R-2']['verdict'] = 'accepted (library v8.0, spec lib-v80.json; the interim book)'
assert by['R-3']['criterion']['checks'][0]['met'] is None and 'at 2x NAV' in by['R-3']['criterion']['checks'][0]['text']
by['R-3']['criterion']['checks'][0]['text'] += ' (read from both cells\' capacity curves in cells batch 2d: lower)'
by['R-3']['criterion']['checks'][0]['met'] = False
by['R-3']['verdict'] = 'rejected (not accepted: rule 5; ledgered)'
assert by['R-4']['parent'] == 'R-3'
by['R-4']['parent'] = 'R-2'           # R-3 was not accepted: the last accepted cell before R-4 is R-2
by['R-4']['label'] = 'R-4 rank hysteresis (hold band .1)'
by['R-4']['verdict'] = 'rejected (not accepted: rule 5; ledgered)'
v8['cells'] = keep

# ---------------------------------------------------------------- narrative and callouts
para = cfg['narrative']['summary']['paragraphs']
assert para[1].startswith('**Result on TRAIN.**')
para[1] = ('**Interim result on TRAIN (owner stop, 2026-10-02).** Seven cells ran and are ledgered: B0a, B0b, the '
           'baseline B0c, R-1 (composition at matched gross) and R-2 (library v8.0), both accepted, and R-3 and R-4, '
           'not accepted. The book of this report is R-2, the last accepted cell: S2 net Sharpe {summ.net_sharpe|+.3f} '
           'against B0c\'s {@' + name('B0c') + ':summ.net_sharpe|+.3f}, at all-rows gross leverage '
           '{summ.mean_gross_leverage_all_rows|.4f} (B0c {@' + name('B0c') + ':summ.mean_gross_leverage_all_rows|.4f}) '
           'and daily turnover {summ.tau_gmv_mean|.4f} of GMV (B0c {@' + name('B0c') + ':summ.tau_gmv_mean|.4f}). '
           'No cumulative test of R-2 against B0c exists and the freeze gate is not evaluated: the differences are '
           'arithmetic, each accepted step is sign-level evidence.')
cfg['narrative']['diagnostics']['paragraphs'] = [
    'The diagnostics G-1..G-3 ran on B0c, the v8 baseline, and cost no trial; they were not re-run on R-2. They, and '
    'the member columns ic_theta, f_theta and marginal IC of R-2\'s library below, gate nothing and select nothing '
    '(v8-prereg item 8): they explain the book, they do not choose it. The diagnostics file was assembled copy-only from '
    'the eight split runs of cells batch 1b (its assembled_from record names each source and its SHA-256).']
cfg['narrative']['book']['paragraphs'] = [
    'The v7 pitch\'s book-level sections, run on R-2 (library v8.0, the last accepted cell; the interim book) over '
    'TRAIN 2020-2023: equity curve, drawdowns, returns, costs and turnover, capacity, exposures and signal correlation, '
    'then the v7 pitch\'s alpha-library figures on library v8.0. Each section checks every file it reads first and '
    'names the one it lacks. V8-F is pending: these are the figures of the last accepted cell, not of a frozen book.']

B0C = name('B0c')
L = ' / '.join(f"{k} {{@{name(k)}:scen.construction.v5.aim_leverage|g}}" for k in DIRS)
G = ' / '.join(f"{k} {{@{name(k)}:summ.mean_gross_leverage_all_rows|.4f}}" for k in DIRS)
P1 = ', '.join(f"{k} {{j:{k.lower().replace('-', '')}.paired.lw.p_one_sided|.4f}} (dSR "
               f"{{j:{k.lower().replace('-', '')}.paired.dsr|+.4f}})" for k in ('R-1', 'R-2', 'R-3', 'R-4'))
cfg['callouts'] = dict({
    'interim': {
        'title': 'Interim status (owner stop, 2026-10-02; Ruling PM6-11)',
        'kind': 'caveat',
        'items': [
            '**This is an interim report.** Its book (headline, section 5) is R-2 (library v8.0, spec lib-v80.json), the '
            'last accepted cell of the ladder; B0c is the v8 baseline.',
            '**No cumulative test and no freeze gate yet.** The cumulative paired test V8-F against B0c and the freeze '
            'gate (v8-prereg item 9) are not evaluated, so no improvement over B0c is claimed as tested; the headline '
            'differences are arithmetic.',
            '**Gains are sign-level per cell** (plan 12.2): the one-sided bootstrap p of each paired S2 net dSR against '
            'its parent is ' + P1 + '. R-1 and R-2 were accepted (dSR > 0, mechanics and criterion), R-3 and R-4 were '
            'not.',
            '**Gross is matched to the parent (Ruling PM6-6).** From R-1 on, each construction cell runs at the aim '
            'leverage L that puts its all-rows gross within .005 of its parent\'s. L by cell (summary.json): ' + L +
            '; all-rows gross (nav_summ): ' + G + '. R-4\'s directory name keeps the template\'s "L1.247" text; the L '
            'it ran at is the one above.',
            'The deflated Sharpe is not meaningful yet: its pre-registered cross-trial variance rests on the v8 cells '
            'only until the registered re-runs of the v7 cells on the four-year window are done (Ruling PM5-22).',
            'Trial accounting: TRAIN construction cells N {a:v8_ledger.n|d}; admission trials this sprint '
            '{a:v8_ledger.k|d} plus {a:v8_ledger.re_screens|d} re-screens; history reads 0; hidden 2024+ unread in this '
            'sprint (section 1).',
            'The diagnostics G-1..G-3 (section 3) exist for B0c only and are labelled B0c. The cells not run are named '
            'once, under the cell ladder (section 2).']},
    'pending': {
        'title': 'Pending at the owner stop (not run; they feed no block of this report)',
        'kind': 'note',
        'items': [
            'R-5 ADV holding cap (next in order; nothing on disk), R-6 target tracking spo-v3, R-7 library v8.1, R-8 '
            'ex-ante risk target, then R-9a..R-9c or R-10..R-12 by the branch R-6 sets (Rulings E-38, E-45).',
            'The registered re-runs of the ledgered v7 cells on the four-year role (Ruling PM5-22; they add 0 to N).',
            'V8-F: the last accepted cell after them, its cumulative paired test against B0c and the freeze gate '
            '(v8-prereg item 9).']},
    'capacity_r2': {
        'title': 'Capacity curve of R-2 (E-29: report only, gates nothing)',
        'kind': 'note',
        'items': [
            'Ruling PM4-5: the capacity books scale only the impact law (m^0.5) and the participation cap (1/m); the aim '
            'is the $1bn book\'s at every multiple, so the 2x and 4x rows are slightly optimistic on the aim cap.',
            'The curve and the S2-KO / S2-FIM books come from a report-only re-run of R-2\'s NAV argv with --cost-v2 '
            '(build-equity/mega-nav-v8-r2-v80-stress); its S2 book is R-2\'s bit for bit (integration-log.md, '
            'interim report render 3).']},
}, **cfg['callouts'])
plan = cfg['callouts']['planning']['items']
assert plan[0].startswith('Plan on a live net Sharpe') and '{a:v8_bundle.final_row.net_sharpe|+.3f}' in plan[0]
plan[0] = plan[0].replace('{a:v8_bundle.final_row.net_sharpe|+.3f}', '{summ.net_sharpe|+.3f}').replace(
    '(S2, 2020-2023)', '(S2, 2020-2023, R-2: the interim book; V8-F pending)')

# ---------------------------------------------------------------- layout
lay = {s['id']: s for s in cfg['layout']}
assert lay['summary']['blocks'] == [{'type': 'callout', 'key': 'planning'}, 'v8_bundle', {'type': 'callout',
                                                                                         'key': 'caveats'}]
lay['summary']['blocks'] = [{'type': 'callout', 'key': 'interim'},
                            {'type': 'h3', 'text': 'Headline: R-2 (the interim book) against B0c (the v8 baseline)'},
                            {'type': 'v8_headline', 'cells': ['B0c', 'R-2'], 'multiples': [2, 4]},
                            {'type': 'callout', 'key': 'planning'}, {'type': 'callout', 'key': 'caveats'}]
assert lay['cells']['blocks'] == ['v8_ladder', {'type': 'h3', 'text': 'Year tables, TRAIN 2020-2023'}, 'v8_year_table']
lay['cells']['blocks'] = ['v8_ladder', {'type': 'callout', 'key': 'pending'},
                          {'type': 'h3', 'text': 'Lever ladder of the accepted path B0c, R-1, R-2'}, 'fig_ladder',
                          {'type': 'h3', 'text': 'Year tables, TRAIN 2020-2023'}, 'v8_year_table',
                          {'type': 'h3', 'text': 'Turnover against cost, every ladder cell'}, 'fig_scatter']
assert lay['diagnostics']['blocks'][0] == 'v8_diagnostics' and lay['diagnostics']['blocks'][2] == 'v8_member_horizon'
lay['diagnostics']['blocks'] = [
    {'type': 'h3', 'text': 'Diagnostics G-1..G-3 of B0c (the v8 baseline)'}, 'v8_diagnostics',
    {'type': 'h3', 'text': 'Member columns of R-2\'s library v8.0 (report only, gates nothing)'}, 'v8_member_horizon']
book = lay['book']['blocks']
lay['book']['title'] = 'The book of R-2 (interim; the last accepted cell): returns, costs, capacity, exposures, alphas'
cap = {'type': 'v8_book', 'block': 'capacity_curve'}
i = book.index(cap)
book.insert(i, {'type': 'callout', 'key': 'capacity_r2'})
assert book[-1] == {'type': 'v8_book', 'block': 't_theme_corr'}
assert book[-5] == {'type': 'h3', 'text': 'Signal correlation of the final library'}
book[-5] = {'type': 'h3', 'text': 'Signal correlation of R-2\'s library v8.0'}
book += [{'type': 'h3', 'text': 'Alpha library v8.0 of R-2 (the v7 pitch\'s library figures)'}, 'universe',
         'fig_ic_panel', 'fig_decay', 'fig_tau_ic', 'fig_theme_year', 'fig_alpha_t', 'fig_theme_weights']
assert lay['appendix']['blocks'] == ['t_files']
lay['appendix']['title'] = 'Appendix: recipe pins per cell, input files'
lay['appendix']['blocks'] = ['t_pins', 't_files']

text = json.dumps(cfg, indent=2) + '\n'
for bad in FORBIDDEN:
    assert bad not in text, bad
DST.write_text(text, encoding='utf-8', newline='\n')
print(DST, DST.stat().st_size)
