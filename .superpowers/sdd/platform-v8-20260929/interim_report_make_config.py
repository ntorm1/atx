"""Write the INTERIM copy of the v8 pitch config (Ruling PM5-27). The registered config is read, never edited."""
import json
from pathlib import Path

ROOT = Path('C:/atx-wt/pool-2')
SRC = ROOT / 'docs/plans/mega-alpha-v8-pitch.config.json'
DST = ROOT / 'docs/plans/mega-alpha-v8-pitch.interim.config.json'

B0C_DIR = 'build-equity/mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247'
B0C_NAME = B0C_DIR.split('/')[-1][len('mega-nav-'):]
R1_TEXT = ('R-1: constructed; mechanics fail (all-rows gross 1.0672, limit [.90, 1.05]); no return read; '
           'not ledgered; ruling open')

cfg = json.loads(SRC.read_text(encoding='utf-8'))

cfg['title'] = 'Mega-alpha v8: strategy pitch (INTERIM, owner stop 2026-10-01)'
cfg['kicker'] = ('atx mega-alpha \u00b7 platform v8 \u00b7 INTERIM at the owner stop: B0a, B0b, B0c only \u00b7 '
                 'in-sample TRAIN 2020-2023 \u00b7 for a quantitative PM')

# the B0c cell: its real NAV dir (top-level cells, the book blocks' references to it)
assert cfg['cells'][1]['label'] == 'B0c baseline'
cfg['cells'][1]['dir'] = B0C_DIR
assert cfg['equity']['extra'][0]['cell'] == 'v8-b0c'
cfg['equity']['extra'][0]['cell'] = B0C_NAME
assert cfg['rolling']['cells'][1]['cell'] == 'v8-b0c'
cfg['rolling']['cells'][1]['cell'] = B0C_NAME
assert cfg['analysis']['compare_cell'] == 'v8-b0c'
cfg['analysis']['compare_cell'] = B0C_NAME

# the capacity curve: B0c's (E-29: priced inline by --capacity-curve; x1 = the primary S2 book). B0c priced no
# S2-KO / S2-FIM books, so none stands beside it.
cc = cfg['capacity_curve']
cfg['capacity_curve'] = {'label': 'the B0c book (the v8 baseline)', 'dir': B0C_DIR,
                         'extras': f'{B0C_DIR}/v7_extras.json', 'summary': f'{B0C_DIR}/summary.json',
                         'sr_threshold': cc['sr_threshold'], 'primary': cc['primary'], 'beside': []}

v8 = cfg['v8']
v8['summ'] = 'build-equity/mega-nav-v8-summ-interim.json'
v8['re_screens'] = 0  # R-2 not started: no _f49 re-screen ran (cells batch 2a)
by = {c['key']: c for c in v8['cells']}
by['B0a']['verdict'] = 're-base ledgered'
by['B0b']['paired'] = 'build-equity/v8-cells-b0b-bundle.json'
by['B0b']['verdict'] = 'accepted'
by['B0c']['dir'] = B0C_DIR
by['B0c']['verdict'] = 'baseline by declaration'
by['R-1']['verdict'] = R1_TEXT
assert by['R-1']['dir'] == 'build-equity/mega-nav-v8-r1'  # a placeholder: never R-1's NAV output
for k, c in by.items():
    if k not in ('B0a', 'B0b', 'B0c', 'R-1'):
        assert c['verdict'] == 'pending run', k

# narrative: the V8-F result paragraph becomes the interim result (B0c from the v8 nav_summ JSON, row 3 = B0c)
para = cfg['narrative']['summary']['paragraphs']
assert para[1].startswith('**Result on TRAIN.**')
para[1] = ('**Interim result on TRAIN (owner stop, 2026-10-01).** Three cells ran: B0a (the v7.1 book re-based on '
           '2020-2023), B0b (role lo3, accepted against B0a) and B0c (B0b with delisting returns and a 60-session warm '
           'start), the v8 baseline by declaration. B0c earns S2 net Sharpe {a:v8_summ.rows.2.net_sharpe|+.3f} at '
           'all-rows gross leverage {a:v8_summ.rows.2.mean_gross_leverage_all_rows|.3f} and daily turnover '
           '{a:v8_summ.rows.2.tau_gmv_mean|.4f} of GMV (row {a:v8_summ.rows.2.dir|str}). No construction cell is '
           'accepted and no cumulative test exists, so no improvement over B0c is claimed; V8-F and the freeze gate '
           'are not evaluated.')

cfg['callouts'] = dict({
    'interim': {
        'title': 'Interim status (owner stop, 2026-10-01; Ruling PM5-27)',
        'kind': 'caveat',
        'items': [
            'Run: B0a, B0b and B0c, each ledgered. B0c is the re-based v8 baseline (prereg rule 6).',
            'No improvement over B0c is claimed: no construction cell is accepted and no cumulative test (V8-F '
            'against B0c) exists.',
            R1_TEXT + '.',
            'The freeze gate (v8-prereg item 9) is not evaluated: V8-F does not exist.',
            'The deflated Sharpe is not meaningful yet: its pre-registered cross-trial variance rests on three cells '
            '(B0a, B0b, B0c) until the registered re-runs of the v7 cells on the four-year window are done (Ruling '
            'PM5-22). It is printed only where the tool prints it.',
            'Trial accounting: TRAIN construction cells N {a:v8_ledger.n|d}; admission trials this sprint '
            '{a:v8_ledger.k|d}; history reads 0; hidden 2024+ unread in this sprint; validation reads before v8: 2 '
            '(the Appendix A block, section 1).',
            'Still to run: the ruling on R-1, R-2..R-8, then R-9 or R-10..R-12 (the branch of R-6), the re-runs of '
            'the ledgered v7 cells on the four-year role (PM5-22), V8-F.',
            'Cells not run render as pending. A block whose input does not exist renders unavailable and names the '
            'file; the ladder states its refusals. The ladder counts N after R-1 and every later cell by the plan; '
            'the ledger holds the N of the Appendix A block.'
        ]},
    'capacity_b0c': {
        'title': 'Capacity curve of B0c (E-29: report only, gates nothing)',
        'kind': 'note',
        'items': [
            'Ruling PM4-5: the capacity books scale only the impact law (m^0.5) and the participation cap (1/m); the '
            'aim is the $1bn book\'s at every multiple, so the 2x and 4x rows are slightly optimistic on the aim cap.',
            'B0c\'s NAV priced no S2-KO or S2-FIM book, so no other impact law stands beside the curve.'
        ]},
    'interim_book': {
        'title': 'V8-F does not exist at the owner stop',
        'kind': 'note',
        'items': [
            'Every block of this section reads the final cell\'s files and renders unavailable, naming the first file '
            'it lacks. B0c\'s capacity curve is in section 2.'
        ]},
}, **cfg['callouts'])

lay = {s['id']: s for s in cfg['layout']}
lay['summary']['blocks'].insert(0, {'type': 'callout', 'key': 'interim'})
cells_blocks = lay['cells']['blocks']
assert cells_blocks[-1] == 'v8_year_table'
cells_blocks[-1] = {'type': 'v8_year_table', 'cells': ['B0a', 'B0b', 'B0c']}
cells_blocks += [{'type': 'h3', 'text': 'Capacity curve of B0c, the v8 baseline (E-29, report only)'},
                 {'type': 'callout', 'key': 'capacity_b0c'},
                 {'type': 'v8_book', 'block': 'capacity_curve'}]
book = lay['book']['blocks']
cap = {'type': 'v8_book', 'block': 'capacity_curve'}
assert cap in book
book.remove(cap)
book.insert(0, {'type': 'callout', 'key': 'interim_book'})
lay['book']['title'] = 'The final book (V8-F): pending at the owner stop'

DST.write_text(json.dumps(cfg, indent=2) + '\n', encoding='utf-8', newline='\n')
print(DST, DST.stat().st_size)
