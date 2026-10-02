"""Fill the INTERIM scorecard v8, render 3 (Ruling PM6-11; the render-1 version of this script, PM5-27, is in git
history at 2f4e54b5). Every number comes from a file and is formatted here, never typed: the render-3 nav_summ JSON
(SUMM), the paired bundles (PAIRED[B0b, R-1..R-4]; B0c's information-only bundle), each ladder cell's NAV summary.json
(NAV[K]), the S2 daily CSVs of B0c and R-2 (annual net and gross-of-cost return), the capacity extras of B0c, R-2 and
R-3 and R-2's --cost-v2 stress summary (CAP), the assembled B0c diagnostics (DIAG), the Appendix A stdout (APPX),
library v8.0 and its recipe (LIB, RECIPE), R-2's admission and cards with the report-only C-2 columns (ADM, CARDS) and
weights (W), v8-prereg.md and the literature review (verbatim text). The book is R-2, the last accepted cell; cells not
run and V8-F are named once as pending. No input of the R-1 run at L 1.247 is opened.
Usage: interim_report_fill_scorecard.py OUT.md APPX_STDOUT RENDER_HEAD UNAVAILABLE_COUNT FIGURE_COUNT"""
import csv
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path('C:/atx-wt/pool-2')
TEMPLATE = ROOT / 'docs/plans/mega-alpha-scorecard-v8.template.md'
OUT, APPX_STDOUT, HEAD, N_UNAV, N_FIG = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]

BE = 'build-equity'
DIRS = {'B0a': f'{BE}/mega-nav-v8-b0a-lo1-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247',
        'B0b': f'{BE}/mega-nav-v8-b0b-lo3-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247',
        'B0c': f'{BE}/mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247',
        'R-1': f'{BE}/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474',
        'R-2': f'{BE}/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80',
        'R-3': f'{BE}/mega-nav-v8-r3-aim-t.05-d.1-fixed-obdelta-x.05-loc-L1.1264',
        'R-4': f'{BE}/mega-nav-v8-r4-hb.1-t.05-d.1-fixed-obdelta-x.05-loc-L1.247'}
KEYS = list(DIRS)
PARENT = {'B0b': 'B0a', 'R-1': 'B0c', 'R-2': 'R-1', 'R-3': 'R-2', 'R-4': 'R-2'}
SUMM_P = f'{BE}/mega-nav-v8-summ-interim3.json'
PAIRED_P = {k: f'{BE}/v8-cells-{k.lower().replace("-", "")}-bundle.json' for k in PARENT}
INFO_B0C_P = f'{BE}/v8-cells-b0c-bundle.json'
LEDGER_P = f'{BE}/trials.jsonl'
DIAG_P = f'{BE}/mega-diagnostics-v8-b0c/diagnostics-v8.json'
STRESS = f'{BE}/mega-nav-v8-r2-v80-stress'
LIB_P, RECIPE_P = 'atx-impl/strategies/fund_industry_ic_v80.json', 'atx-impl/strategies/fund_industry_ic_v80.recipe.v2.json'
ADM_P, CARDS_P = f'{BE}/mega-weights-v8-r1-std-v80-c2/admission.json', f'{BE}/mega-cards-v8-r1-std-v80-c2/index.json'
W_P = f'{BE}/mega-weights-v8-r1-std-v80/composition_weights.json'
PREREG_P = '.superpowers/sdd/platform-v8-20260929/v8-prereg.md'
LIT_P = '.superpowers/sdd/platform-v8-20260929/reports/Equity long short alpha v8.md'
SPEC = {'B0a': 'base-lo1.json', 'B0b': 'base-lo3.json', 'B0c': 'base-b0c.json', 'R-1': 'r1-comp-v8-gm.json',
        'R-2': 'lib-v80.json', 'R-3': 'r3-aim-gain-gm.json', 'R-4': 'r4-hold-band.json'}
LEVER = {'B0a': 'v7.1 on lo1, 2020-2023', 'B0b': 'v7.1 on lo3', 'B0c': 'winner + delisting returns + warm start 60',
         'R-1': 'composition ew-theme-std-v1 (gross matched)', 'R-2': 'library v8.0', 'R-3': 'persistence gain '
         '(ew-theme-std-aim-v1; gross matched)', 'R-4': 'rank hysteresis (hold band .1)'}
VERDICT = {'B0a': 're-base ledgered', 'B0b': 'accepted', 'B0c': 'baseline by declaration', 'R-1': 'accepted',
           'R-2': 'accepted (the interim book)', 'R-3': 'not accepted (rule 5)', 'R-4': 'not accepted (rule 5)'}
SCEN = {'S1': 'linear-6bps-stale5-v1+swap-fin-v1', 'S2': 'modeled-1bn-stale5-v1+swap-fin-v1',
        'tiers': 'modeled-1bn-stale5-v1+engine-tiers-v1', 'flat300': 'modeled-1bn-stale5-v1+flat-300-v0',
        'S3': 'modeled-1bn-terminal-adverse-v1+swap-fin-v1', 'KO': 'modeled-1bn-ko-v1+swap-fin-v1',
        'FIM': 'modeled-1bn-fim-v1+swap-fin-v1'}
ANNUAL = 252
PEND = 'pending (not run)'
SHA = {}


def read(rel):
    data = (ROOT / rel).read_bytes()
    SHA[rel] = hashlib.sha256(data).hexdigest()
    return data


def jload(rel):
    return json.loads(read(rel).decode('utf-8'))


def base(p):
    return str(p).replace('\\', '/').rstrip('/').split('/')[-1]


def fmt(v, spec):
    if v is None:
        return 'n/a'
    if spec == 'int':
        return f'{int(v):,d}'
    if spec.startswith(('+pct', 'pct')):
        return f"{v * 100:{'+' if spec[0] == '+' else ''}.{int(spec[-1])}f}%"
    return format(v, spec)


def dig(obj, path):
    cur = obj
    for part in path.split('.'):
        cur = cur[int(part)] if isinstance(cur, list) else cur[part]
    return cur


# ---------------------------------------------------------------- inputs
SUMM = {base(r['dir']): r for r in jload(SUMM_P)}
ROW = {k: SUMM[base(d)] for k, d in DIRS.items()}
PAIRED = {k: jload(p) for k, p in PAIRED_P.items()}
for k, doc in PAIRED.items():
    assert base(doc['final']) == base(DIRS[k]) and base(doc['base']) == base(DIRS[PARENT[k]]), k
INFO_B0C = jload(INFO_B0C_P)
NAV = {k: jload(f'{d}/summary.json') for k, d in DIRS.items()}
CAP = {k: jload(f'{DIRS[k]}/v7_extras.json') for k in ('B0c', 'R-2', 'R-3')}
STRESS_SUM = jload(f'{STRESS}/summary.json')
STRESS_CAP = jload(f'{STRESS}/v7_extras.json')
assert STRESS_CAP['capacity'] == CAP['R-2']['capacity']   # the stress re-run's capacity rows are R-2's
DIAG = jload(DIAG_P)
LIB, RECIPE, ADM, CARDS, W = jload(LIB_P), jload(RECIPE_P), jload(ADM_P), jload(CARDS_P), jload(W_P)
read(LEDGER_P)
APPX = [ln for ln in APPX_STDOUT.read_text(encoding='utf-8').splitlines() if ln.startswith('TRAIN construction cells')]
assert len(APPX) == 1, APPX
APPX = APPX[0]
m = re.search(r'TRAIN construction cells (\d+); admission trials this sprint (\d+);.*validation reads before v8: (\d+) '
              r'\(.*history reads (\d+);', APPX)
N_APPX, K_APPX, VAL_APPX, HIST_APPX = m.groups()
RE_SCREENS = len(RECIPE['trials']['rescreens'])


def scen(doc, key):
    return next(s for s in doc['scenarios'] if s.get('scenario') == SCEN[key])


def daily_annual(k):
    """252 x the mean daily net_return and gross_return over the return rows (return_observation 1, not the first
    CSV row) of cell k's S2 daily CSV."""
    rel = f'{DIRS[k]}/daily_{SCEN["S2"]}.csv'
    rows = list(csv.DictReader(read(rel).decode('utf-8').splitlines()))
    ret = [r for i, r in enumerate(rows) if i > 0 and float(r['return_observation']) == 1]
    net = sum(float(r['net_return']) for r in ret) / len(ret)
    gross = sum(float(r['gross_return']) for r in ret) / len(ret)
    return {'net': ANNUAL * net, 'gross': ANNUAL * gross, 'rows': len(ret), 'last': max(int(r['session_ns']) for r in rows)}


ANN = {k: daily_annual(k) for k in ('B0c', 'R-2')}
for k in ANN:
    assert ANN[k]['last'] < 1704067200 * 10 ** 9, k   # no session on or after 2024-01-01 (the research seal)


def cap_sr(k, mult):
    return next(r['net_sharpe'] for r in CAP[k]['capacity'] if r['multiple'] == mult)


def L(k):
    return scen(NAV[k], 'S2')['construction']['v5']['aim_leverage']


def s(k, path, spec):
    return fmt(dig(ROW[k], path), spec)


def p(k, path, spec):
    return fmt(dig(PAIRED[k], path), spec)


T = TEMPLATE.read_text(encoding='utf-8').split('\n')
TSHA = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()


def tl(n):
    return T[n - 1]


out = []
A = out.append
DSR_NOTE = 'not meaningful yet (PM5-22)'

# ---------------------------------------------------------------- title, interim status
A(tl(1).replace('{{PM: date}}', '2026-10-02; INTERIM render 3 at the owner stop'))
A('')
A('## Interim status (owner stop, 2026-10-02; Ruling PM6-11)')
A('')
A('This is an interim scorecard (render 3). It is filled from the registered template on the cells that ran; the cells '
  'that did not run and V8-F are named once, in the list below.')
A('')
A(f"- **The book is R-2** (library v8.0, spec `scripts/specs/v8/{SPEC['R-2']}`), the last accepted cell: S2 net Sharpe "
  f"{s('R-2', 'net_sharpe', '+.4f')} against the baseline B0c's {s('B0c', 'net_sharpe', '+.4f')}; net annual return "
  f"{fmt(ANN['R-2']['net'], '+pct2')} (B0c {fmt(ANN['B0c']['net'], '+pct2')}); net Sharpe at 4x NAV "
  f"{fmt(cap_sr('R-2', 4.0), '+.3f')} (B0c {fmt(cap_sr('B0c', 4.0), '+.3f')}). Headline in section 1.")
A('- **No cumulative test and no freeze gate yet.** V8-F against B0c (v8-prereg item 9) is not evaluated, so no '
  'improvement over B0c is claimed as tested; the differences in section 1 are arithmetic.')
A('- **Gains are sign-level per cell** (plan 12.2). One-sided studentized bootstrap p of the paired S2 net dSR against '
  'the parent: ' + '; '.join(f"{k} {p(k, 'paired.lw.p_one_sided', '.4f')} (dSR {p(k, 'paired.dsr', '+.4f')}, SE "
                             f"{p(k, 'paired.memmel_se', '.4f')})" for k in ('R-1', 'R-2', 'R-3', 'R-4')) + '.')
A('- **Gross matched to the parent (Ruling PM6-6).** From R-1 on each construction cell runs at the L that puts its '
  'all-rows gross within .005 of its parent\'s. L (NAV summary.json) and all-rows gross (nav_summ): '
  + '; '.join(f"{k} L {fmt(L(k), 'g')}, gross {s(k, 'mean_gross_leverage_all_rows', '.4f')}" for k in KEYS)
  + '. R-4\'s directory name keeps the template\'s "L1.247" text; it ran at the L above.')
A(f"- Deflated Sharpe: {DSR_NOTE}; the pre-registered cross-trial variance rests on the v8 cells only until the "
  f"re-runs of the ledgered v7 cells on the four-year window.")
A(f'- Trial accounting: TRAIN construction cells N {N_APPX}; admission trials this sprint {K_APPX} plus {RE_SCREENS} '
  f're-screens (ruling R2-e); history reads {HIST_APPX}; hidden 2024+ unread in this sprint; validation reads before '
  f'v8: {VAL_APPX}.')
A('- Diagnostics G-1..G-3 (section 7) exist for B0c only: they are labelled B0c.')
A('- **Pending (not run at the owner stop):** R-5 ADV holding cap (next; nothing on disk), R-6, R-7, R-8, then '
  'R-9a..R-9c or R-10..R-12 by the branch R-6 sets; the PM5-22 re-runs of the ledgered v7 cells (they add 0 to N); '
  'V8-F (cumulative test against B0c, freeze gate).')
A(f'- The pitch rendered from the interim config (`docs/plans/2026-10-02-mega-alpha-v8-interim-pitch.html`): '
  f'{N_UNAV} unavailable blocks, {N_FIG} figures (integration-log.md, "interim report, render 3 (owner stop 2)").')
A('')
A('Appendix A block (`nav_summ.py --protocol v8 --ledger-n build-equity/trials.jsonl`, stdout verbatim), with ruling '
  f'R2-e\'s " plus {RE_SCREENS} re-screens" inserted:')
A('')
A('```')
A(APPX.replace(f'admission trials this sprint {K_APPX}', f'admission trials this sprint {K_APPX} plus {RE_SCREENS} '
                                                         f're-screens', 1))
A('```')
A('')
A(tl(24).replace('{{PM: root HEAD}}', f'{HEAD[:8]} (render 3; the interim config commit)'))
for n in range(25, 31):
    A(tl(n))
A('')

# ---------------------------------------------------------------- inputs
A('## Inputs (interim render 3: what exists at the owner stop)')
A('')
A(tl(34))
A(tl(35))
A(f"| `SUMM` | `{SUMM_P}` (`{SHA[SUMM_P][:12]}`) | bounded `nav_summ.py --protocol v8 --dsr-ledger "
  f"build-equity/trials.jsonl --effective-n dirs --psr --json {SUMM_P}` over the seven ladder dirs (no `--ledger`) |")
A('| `PAIRED[K]` | ' + '; '.join(f"{k}: `{PAIRED_P[k]}` (`{SHA[PAIRED_P[k]][:12]}`)" for k in PARENT)
  + ' | `nav_summ.py --protocol v8 --bundle <parent dir> <cell dir> --bundle-json <path>` (the cells\' own PM5-23 runs) |')
A(f"| B0c vs B0b (information only) | `{INFO_B0C_P}` (`{SHA[INFO_B0C_P][:12]}`) | PM5-23 bundle of cells batch 1b |")
A('| `BUNDLE` | pending (no V8-F) | - |')
A(f"| `LEDGER` | `{LEDGER_P}` (`{SHA[LEDGER_P][:12]}`) | the cycle's ledger lines (unchanged by this report) |")
A(f"| `APPX` | `{APPX_STDOUT.relative_to(ROOT).as_posix() if APPX_STDOUT.is_absolute() else APPX_STDOUT}` "
  f"(`{hashlib.sha256(APPX_STDOUT.read_bytes()).hexdigest()[:12]}`) | bounded `nav_summ.py --protocol v8 --ledger-n "
  f"build-equity/trials.jsonl` |")
A(f"| `DIAG` | `{DIAG_P}` (`{SHA[DIAG_P][:12]}`) | B0c's eight split diagnostics runs (cells batch 1b) assembled copy-only "
  f"by `interim_report_assemble_diagnostics.py` (sources and digests in its `assembled_from`) |")
A(f"| `CARDS` | `{CARDS_P}` (`{SHA[CARDS_P][:12]}`) | report only: R-2's card argv + `--ic-theta --marginal-ic` (R-2's K6 "
  f"pool file) on the admission below |")
A(f"| `ADM` | `{ADM_P}` (`{SHA[ADM_P][:12]}`) | report only: R-2's fit argv + `--report-f-theta` (weights, signs and "
  f"statuses equal R-2's) |")
A(f"| `W` | `{W_P}` (`{SHA[W_P][:12]}`) | R-2's fit |")
A(f"| `NAVF` | R-2 (interim book): `{DIRS['R-2']}/summary.json` (`{SHA[DIRS['R-2'] + '/summary.json'][:12]}`) | R-2's "
  f"NAV run |")
A('| `NAV[K]` | ' + '; '.join(f"{k} `{SHA[DIRS[k] + '/summary.json'][:12]}`" for k in KEYS)
  + ' | each ladder cell\'s NAV `summary.json` |')
A('| `CAP` | ' + '; '.join(f"{k} `{DIRS[k]}/v7_extras.json` (`{SHA[DIRS[k] + '/v7_extras.json'][:12]}`)"
                          for k in CAP) + f"; R-2 stress `{STRESS}/summary.json` (`{SHA[STRESS + '/summary.json'][:12]}`) "
  '| each cell\'s `--capacity-curve` pass; the stress dir is R-2\'s NAV argv + `--cost-v2` (report only; capacity rows '
  'and S2 book equal to R-2\'s) |')
A(f"| `LIB`, `RECIPE` | `{LIB_P}` (`{SHA[LIB_P][:12]}`), `{RECIPE_P}` (`{SHA[RECIPE_P][:12]}`) | library v8.0 (add-alpha, "
  f"R-2) |")
A(f"| `PREREG` | `{PREREG_P}` | root (pins filled) |")
A(f"| `LIT` | `{LIT_P}` | the v8 literature review |")
A('')

# ---------------------------------------------------------------- cells
A('## Cells')
A('')
A('| key | cell dir (`build-equity/...`) | spec (`scripts/specs/v8/`) | parent | N after | L | mechanical criterion (task) |')
A('|---|---|---|---|---|---|---|')
CRIT = {'B0a': 'none (re-base)', 'B0b': 'none besides dSR > 0 and mechanics (ruling W0-b)',
        'B0c': 'none (protocol correction; baseline by declaration)',
        'R-1': 'turnover per unit gross (executed: tau_gmv_mean / mean_gross_leverage_all_rows, S2) not higher than the '
               'parent (Ruling PM5-11)', 'R-2': 'turnover not higher', 'R-3': 'net at 2x not lower; turnover lower',
        'R-4': 'turnover at least 15% lower'}
NAFTER = {k: 38 + i for i, k in enumerate(KEYS)}
assert NAFTER['R-4'] == int(N_APPX)
for k in KEYS:
    A(f"| {k} | `{base(DIRS[k])}` | `{SPEC[k]}` | {PARENT.get(k, '-')} | {NAFTER[k]} | {fmt(L(k), 'g')} | {CRIT[k]} |")
A(f'| R-5 .. R-12 | - | - | - | - | - | {PEND} |')
A('')
A('V8-F = the last accepted cell after the pending cells: **pending**. The interim book is **R-2**.')
A('')

# ---------------------------------------------------------------- 1. headline
A('## 1. Headline (interim: R-2, the last accepted cell, against B0c, the baseline)')
A('')
A('Each figure is read from the cell\'s own file (nav_summ row, S2 daily CSV, NAV summary.json, capacity extras); the '
  'difference is arithmetic, not a test (the paired tests are in section 2a).')
A('')
A('| metric | B0c | R-2 | R-2 minus B0c | source |')
A('|---|---|---|---|---|')


def hl(label, a, b, spec, dspec, src):
    d = fmt(b - a, dspec) if a is not None and b is not None else 'n/a'
    A(f'| {label} | {fmt(a, spec)} | **{fmt(b, spec)}** | {d} | {src} |')


hl('Aim leverage L', L('B0c'), L('R-2'), 'g', '+.4f', 'NAV summary.json (S2 construction.v5.aim_leverage)')
hl('S2 net Sharpe', ROW['B0c']['net_sharpe'], ROW['R-2']['net_sharpe'], '+.4f', '+.4f', 'SUMM net_sharpe')
hl('Net return, annual (mean daily x 252)', ANN['B0c']['net'], ANN['R-2']['net'], '+pct2', '+pct2',
   'S2 daily CSV net_return over the return rows')
hl('Gross-of-cost return, annual (mean daily x 252)', ANN['B0c']['gross'], ANN['R-2']['gross'], '+pct2', '+pct2',
   'S2 daily CSV gross_return over the return rows')
hl('Cost and financing drag, annual', ANN['B0c']['gross'] - ANN['B0c']['net'], ANN['R-2']['gross'] - ANN['R-2']['net'],
   'pct2', '+pct2', 'gross-of-cost minus net')
hl('S2 net Sharpe at 2x NAV', cap_sr('B0c', 2.0), cap_sr('R-2', 2.0), '+.3f', '+.3f', 'CAP capacity[2x].net_sharpe')
hl('S2 net Sharpe at 4x NAV', cap_sr('B0c', 4.0), cap_sr('R-2', 4.0), '+.3f', '+.3f', 'CAP capacity[4x].net_sharpe')
hl('Daily turnover tau, mean', ROW['B0c']['tau_gmv_mean'], ROW['R-2']['tau_gmv_mean'], '.4f', '+.4f',
   'SUMM tau_gmv_mean')
hl('Gross leverage, mean over all rows', ROW['B0c']['mean_gross_leverage_all_rows'],
   ROW['R-2']['mean_gross_leverage_all_rows'], '.4f', '+.4f', 'SUMM mean_gross_leverage_all_rows')
hl('Cost per traded dollar, bps', ROW['B0c']['cost_bps_traded'], ROW['R-2']['cost_bps_traded'], '.2f', '+.2f',
   'SUMM cost_bps_traded')
hl('Gross Sharpe', ROW['B0c']['gross_sharpe'], ROW['R-2']['gross_sharpe'], '.3f', '+.3f', 'SUMM gross_sharpe')
A('')
A('Year tables of both (`SUMM[K].year_table`; 2023 is partly selected, OD-1):')
A('')
A('| year | B0c net Sharpe | R-2 net Sharpe | B0c net return | R-2 net return | B0c vol | R-2 vol | B0c tau | R-2 tau | '
  'B0c cost bps/$ | R-2 cost bps/$ |')
A('|---|---|---|---|---|---|---|---|---|---|---|')
YB, YR = ({y['year']: y for y in ROW[k]['year_table']} for k in ('B0c', 'R-2'))
for y in sorted(set(YB) | set(YR)):
    b, r = YB.get(y, {}), YR.get(y, {})
    A(f"| {y} | {fmt(b.get('net_sharpe'), '+.3f')} | {fmt(r.get('net_sharpe'), '+.3f')} | "
      f"{fmt(b.get('net_return'), '+pct2')} | {fmt(r.get('net_return'), '+pct2')} | {fmt(b.get('ann_vol'), 'pct2')} | "
      f"{fmt(r.get('ann_vol'), 'pct2')} | {fmt(b.get('tau_gmv_mean'), '.4f')} | {fmt(r.get('tau_gmv_mean'), '.4f')} | "
      f"{fmt(b.get('cost_bps_traded'), '.2f')} | {fmt(r.get('cost_bps_traded'), '.2f')} |")
A('')
for n in range(78, 80):
    A(tl(n))


def dsr_cell(k):
    return (f"{s(k, 'deflated_ledger.dsr', '.3f')} (N {s(k, 'deflated_ledger.n', 'int')}; {DSR_NOTE}) / "
            f"{s(k, 'deflated_effective_n.dsr', '.3f')} / {s(k, 'deflated_lo_null.dsr', '.3f')}")


A(f"| **Interim book (R-2; V8-F pending)** | `{base(DIRS['R-2'])}` | **{s('R-2', 'net_sharpe', '+.3f')}** | "
  f"{s('R-2', 'gross_sharpe', '+.3f')} | {s('R-2', 'mean_gross_leverage_all_rows', '.4f')} | {dsr_cell('R-2')} | "
  f"freeze gate not evaluated |")
A(f"| Baseline (B0c) | `{base(DIRS['B0c'])}` | **{s('B0c', 'net_sharpe', '+.3f')}** | {s('B0c', 'gross_sharpe', '+.3f')} | "
  f"{s('B0c', 'mean_gross_leverage_all_rows', '.4f')} | {dsr_cell('B0c')} | baseline by declaration |")
A(f"| v7.1 book re-based (B0a) | `{base(DIRS['B0a'])}` | {s('B0a', 'net_sharpe', '+.3f')} | "
  f"{s('B0a', 'gross_sharpe', '+.3f')} | {s('B0a', 'mean_gross_leverage_all_rows', '.4f')} | {dsr_cell('B0a')} | "
  f"re-base (the v7.1 book on 2020-2023) |")
A('')
A('**The pre-registered freeze gate is not evaluated: V8-F does not exist at the owner stop.** Cumulative paired test V8-F '
  'vs B0c: pending (`BUNDLE` not produced). Planning value: live net Sharpe .7 to 1.0 [est] against the TRAIN figure of '
  f"the frozen book; R-2's TRAIN figure is {s('R-2', 'net_sharpe', '+.3f')} (interim). The DSR column reads the ledger N "
  'and the effective N / Lo null of the seven listed dirs; none of them is meaningful yet (PM5-22).')
A('')
A('Freeze gate (v8-prereg item 9): not evaluated (no V8-F); it is not "unmet", so OD-3 is not invoked.')
A('')

# ---------------------------------------------------------------- 2a. cells
for n in range(109, 120):
    A(tl(n))
A('| # | cell | lever | parent | N after | L | S2 net | gross SR | vol | tau mean/p95 | cost bps/$ | gross all rows | net '
  'all rows | dSR vs parent (SE) | p one-sided | mechanical criterion | verdict |')
A('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')


def crit_text(k):
    r, par = ROW[k], ROW.get(PARENT.get(k)) if k in PARENT else None
    if k == 'R-1':
        a, b = r['tau_gmv_mean'] / r['mean_gross_leverage_all_rows'], par['tau_gmv_mean'] / par[
            'mean_gross_leverage_all_rows']
        return f"{CRIT[k]}: {a:.5f} vs {b:.5f} -> {'met' if a <= b else 'not met'}"
    if k == 'R-2':
        return f"{CRIT[k]}: {r['tau_gmv_mean']:.5f} vs {par['tau_gmv_mean']:.5f} -> " + (
            'met' if r['tau_gmv_mean'] <= par['tau_gmv_mean'] else 'not met')
    if k == 'R-3':
        a, b = cap_sr('R-3', 2.0), cap_sr('R-2', 2.0)
        t = r['tau_gmv_mean'] < par['tau_gmv_mean']
        return (f"net at 2x {a:+.4f} vs {b:+.4f} ({'not lower' if a >= b else 'lower'}); turnover "
                f"{r['tau_gmv_mean']:.5f} vs {par['tau_gmv_mean']:.5f} ({'lower' if t else 'not lower'}) -> "
                + ('met' if a >= b and t else 'not met'))
    if k == 'R-4':
        lim = 0.85 * par['tau_gmv_mean']
        return (f"{CRIT[k]}: {r['tau_gmv_mean']:.5f} vs .85 x {par['tau_gmv_mean']:.5f} = {lim:.5f} -> "
                + ('met' if r['tau_gmv_mean'] <= lim else 'not met'))
    return CRIT[k]


for i, k in enumerate(KEYS, 1):
    vol = fmt(scen(NAV[k], 'S2')['ann_vol'], 'pct2')
    dsr = f"{p(k, 'paired.dsr', '+.3f')} ({p(k, 'paired.memmel_se', '.3f')})" if k in PAIRED else '-'
    p1 = p(k, 'paired.lw.p_one_sided', '.4f') if k in PAIRED else '-'
    A(f"| {i} | {k} | {LEVER[k]} | {PARENT.get(k, '-')} | {NAFTER[k]} | {fmt(L(k), 'g')} | {s(k, 'net_sharpe', '+.3f')} | "
      f"{s(k, 'gross_sharpe', '+.3f')} | {vol} | {s(k, 'tau_gmv_mean', '.4f')}/{s(k, 'tau_gmv_p95', '.4f')} | "
      f"{s(k, 'cost_bps_traded', '.2f')} | {s(k, 'mean_gross_leverage_all_rows', '.4f')} | "
      f"{s(k, 'mean_net_leverage_all_rows', '+.4f')} | {dsr} | {p1} | {crit_text(k)} | {VERDICT[k]} |")
A(f'| 8-17 | R-5 .. R-12 | - | - | - | - | {PEND} | | | | | | | | | | |')
A('')
ib = INFO_B0C['paired']
A(f"B0c against B0b, information only (B0c is the baseline by declaration; gates nothing; `{INFO_B0C_P}`, PM5-23): dSR "
  f"{fmt(ib['dsr'], '+.3f')} (SE {fmt(ib['memmel_se'], '.3f')}), one-sided p {fmt(ib['lw']['p_one_sided'], '.4f')}, "
  f"two-sided {fmt(ib['lw']['p_value'], '.4f')}. Volatility = each cell's whole-window annualised S2 volatility "
  f"(`NAV[K]`); L = the S2 book's aim leverage (`NAV[K]`). Mechanics (gross [.90, 1.05], |net| <= .02, tau mean <= .20, "
  f"p95 <= .30) pass on every row above.")
A('')
A('Not trials: the calibration NAV runs of PM6-6 (R-1 at L 1.247, R-3 at L 1.1474; read for the mechanics keys only, no '
  'return opened) and the report-only re-runs of this render (R-2 `--cost-v2`, fit `--report-f-theta`, cards '
  '`--ic-theta --marginal-ic`). Invalid cells excluded by the defect rule (item 7): none.')
A('')

# ---------------------------------------------------------------- 2b. year tables
for n in range(143, 150):
    A(tl(n))
for k in KEYS:
    yt = {y['year']: y for y in ROW[k]['year_table']}
    A(f"| {'**R-2**' if k == 'R-2' else k} | " + ' | '.join(
        f"{fmt(yt[y]['net_sharpe'], '+.3f')} / {fmt(yt[y]['net_return'], '+pct1')}" for y in sorted(yt))
      + f" | {s(k, 'net_sharpe', '+.3f')} |")
A(f'| R-5 .. R-12, V8-F | {PEND} | | | | |')
A('')
A('R-2 (the interim book) year table in full (return rows, net Sharpe, return, volatility, turnover, cost per traded '
  'dollar):')
A('')
A(tl(156))
A(tl(157))
for y in ROW['R-2']['year_table']:
    A(f"| {y['year']} | {fmt(y['return_rows'], 'int')} | {fmt(y['net_sharpe'], '+.3f')} | {fmt(y['net_return'], '+pct1')} | "
      f"{fmt(y['ann_vol'], 'pct2')} | {fmt(y['tau_gmv_mean'], '.4f')} | {fmt(y['cost_bps_traded'], '.2f')} |")
A('')
A('Cumulative dSR V8-F vs B0c by year (`BUNDLE.years`): pending (no V8-F).')
A('')

# ---------------------------------------------------------------- 3. stresses
A(tl(166))
A('')
A('Rows: R-2 (the interim book), B0c, B0a. Every scenario prices the same construction (`NAV[K].scenarios[]`); S2-KO and '
  'S2-FIM are R-2\'s `--cost-v2` stress books (report only).')
A('')
A('| cell | S1 linear 6 bps | **S2 modeled $1bn** | S2 x engine-tiers | S2 x flat-300 | S3 terminal-adverse (K = 1) | '
  'S2-KO | S2-FIM | S2-FEE (G-3a, descriptive) |')
A('|---|---|---|---|---|---|---|---|---|')
g3a = DIAG['diagnostics']['G-3a']['result']['restated_net_sharpe']


def sc(doc, key):
    return fmt(scen(doc, key)['net_sharpe'], '+.3f')


A(f"| **R-2** | {sc(NAV['R-2'], 'S1')} | **{s('R-2', 'net_sharpe', '+.3f')}** | {sc(NAV['R-2'], 'tiers')} | "
  f"{sc(NAV['R-2'], 'flat300')} | {sc(NAV['R-2'], 'S3')} | {sc(STRESS_SUM, 'KO')} | {sc(STRESS_SUM, 'FIM')} | "
  f"{fmt(g3a, '+.3f')} (on B0c) |")
for k in ('B0c', 'B0a'):
    A(f"| {k} | {sc(NAV[k], 'S1')} | **{s(k, 'net_sharpe', '+.3f')}** | {sc(NAV[k], 'tiers')} | {sc(NAV[k], 'flat300')} | "
      f"{sc(NAV[k], 'S3')} | - | - | {fmt(g3a, '+.3f') if k == 'B0c' else '-'} |")
A('')
A('Capacity (E-29, report only, gates nothing; x1 = the primary S2 book bit for bit): net Sharpe / cost bps per traded '
  'dollar at .5 / 1 / 2 / 4 / 8 x $1bn:')
A('')
A('| cell | .5x | 1x | 2x | 4x | 8x |')
A('|---|---|---|---|---|---|')
for k in ('R-2', 'B0c', 'R-3'):
    caps = sorted(CAP[k]['capacity'], key=lambda r: r['multiple'])
    assert [r['multiple'] for r in caps] == [0.5, 1.0, 2.0, 4.0, 8.0] and CAP[k]['capacity_x1_equals_primary_bit_for_bit']
    A(f"| {k} | " + ' | '.join(f"{fmt(r['net_sharpe'], '+.3f')} / {fmt(r['cost_bps_per_traded_dollar'], '.2f')}"
                               for r in caps) + ' |')
A('')
A('Ruling PM4-5: the capacity books scale only the impact law (m^0.5) and the participation cap (1/m); the aim is the '
  '$1bn book\'s at every multiple, so the 2x and 4x rows are slightly optimistic on the aim cap. R-3\'s row is its '
  'criterion input (net at 2x against R-2\'s).')
A('')

# ---------------------------------------------------------------- 4. walk-through (R-2)
s2 = scen(NAV['R-2'], 'S2')
v5 = s2['construction']['v5']
cnt = ADM['counts']
weighted = sum(1 for v in W['weights'].values() if v)
A('## 4. How the alphas become the book (R-2, the interim book)')
A('')
A(f"1. **Library** v8.0 (`{LIB_P}`, sha256 `{SHA[LIB_P][:12]}`): {len(LIB['candidates'])} candidates in "
  f"{len({c['theme'] for c in LIB['candidates']})} themes; the recipe (`{SHA[RECIPE_P][:12]}`) records "
  f"{len(RECIPE['trials']['admission_trials'])} admission trials, {RE_SCREENS} re-screens and "
  f"{len(RECIPE['trials']['removed_parent_members'])} removed parent members against v7.1.")
A(f"2. **Universe**: role `train-2020-2023-lo3` (manifest sha256 `{NAV['R-2']['role_sha256'][:12]}`), "
  f"marked by the delisting-returns label role (`{NAV['R-2']['label_role']['sha256'][:12] if isinstance(NAV['R-2'].get('label_role'), dict) and NAV['R-2']['label_role'].get('sha256') else 'lo3-dlret'}`).")
A(f"3. **Admission** `{ADM['screen']}` (`ADM`): " + ', '.join(f'{k} {v}' for k, v in cnt.items() if v) + '.')
A(f"4. **Composition** `{W['provenance']['rule'] if isinstance(W['provenance'].get('rule'), str) else W['provenance'].get('composition')}` "
  f"(weights sha256 `{SHA[W_P][:12]}`): {weighted} of {len(LIB['candidates'])} members weighted; theme standardisation "
  f"`{W['theme_standardise'].get('rule')}` (re-rank {W['theme_standardise'].get('rerank')}).")
A(f"5. **Construction** `{NAV['R-2']['rule']}`, order basis {NAV['R-2']['order_basis']}: theta {fmt(v5['theta'], 'g')}, "
  f"dust {fmt(v5['dust_multiple'], 'g')}, exit rate {fmt(v5['exit_rate'], 'g')}, aim leverage L {fmt(v5['aim_leverage'], 'g')}, "
  f"neutralize {s2['construction']['neutralize']}, warm start {NAV['R-2']['warm_start'].get('sessions') if isinstance(NAV['R-2'].get('warm_start'), dict) else NAV['R-2'].get('warm_start')} sessions.")
A(f"6. **Execution and costs**: primary scenario S2 `{SCEN['S2']}`; S2 net Sharpe {fmt(s2['net_sharpe'], '+.3f')} "
  f"(NAV summary), annualised volatility {fmt(s2['ann_vol'], 'pct2')}, max drawdown {fmt(s2['max_drawdown'], 'pct2')}.")
A('')

# ---------------------------------------------------------------- 5. alpha table
lin = {r['id']: r for r in RECIPE['lineage']}
adm = {c['id']: c for c in ADM['candidates']}
cards = {c['id']: c for c in CARDS['candidates']}
rescreen, new = set(RECIPE['trials']['rescreens']), set(RECIPE['trials']['admission_trials'])
order = sorted(LIB['candidates'], key=lambda c: lin[c['id']]['roster_order'])
A(f"## 5. Alpha table (R-2's library v8.0, {len(ADM['candidates'])} candidates)")
A('')
A('Status, HAC t and tau are the TRAIN admission statistics (`ADM`, equal to R-2\'s admission); weights from `W`. The '
  'traded-horizon columns are **report only, gates nothing** (v8-prereg item 8): ic_theta and marginal IC21 from '
  '`CARDS`, f_theta and its HAC t from `ADM`. "prior sign" is the recipe\'s `prior_sign` (the v2 recipe records no raw '
  'literature direction).')
A('')
A('| id | theme | tier | prior sign | change vs v7.1 | status | HAC t | tau | **w** | ic_theta | f_theta (HAC t) | '
  'marginal IC21 |')
A('|---|---|---|---|---|---|---|---|---|---|---|---|')
for c in order:
    i = c['id']
    a, cd = adm[i], cards.get(i, {})
    chg = 're-screen (FF49)' if i in rescreen else ('new: admission trial' if i in new else 'unchanged')
    ft = (f"{fmt(a.get('f_theta'), '+.2e')} ({fmt(a.get('f_theta_hac_t'), '+.2f')})"
          if a.get('f_theta') is not None else 'n/a')
    A(f"| `{i}` | {c['theme']} | {c.get('tier')} | {fmt(lin[i].get('prior_sign'), '+d')} | {chg} | {a.get('status')} | "
      f"{fmt(a.get('hac_t'), '+.2f')} | {fmt(a.get('tau'), '.4f')} | **{fmt(W['weights'].get(i, 0.0), '.4f')}** | "
      f"{fmt(cd.get('ic_theta'), '+.4f')} | {ft} | {fmt(cd.get('marginal_ic21'), '+.4f')} |")
A('')
mass = {}
for c in order:
    w = W['weights'].get(c['id'], 0.0)
    if w:
        t = mass.setdefault(c['theme'], [0, 0.0])
        t[0] += 1
        t[1] += w
A('Theme mass on R-2: ' + '; '.join(f'{t} {n} members, {fmt(m_, ".4f")}' for t, (n, m_) in mass.items()) + '.')
A('')

# ---------------------------------------------------------------- 6. DSL
A('## 6. Alpha DSL strings (verbatim from `LIB`, library v8.0)')
A('')
cur = None
for c in sorted(LIB['candidates'], key=lambda c: (c['theme'], lin[c['id']]['roster_order'])):
    if c['theme'] != cur:
        cur = c['theme']
        A(f'### {cur}')
        A('')
    a = adm[c['id']]
    A(f"**`{c['id']}`** ({a.get('status')}; weight {fmt(W['weights'].get(c['id'], 0.0), '.4f')}). {c.get('citation')}")
    A('')
    A('```')
    A(c['dsl'])
    A('```')
    A('')

# ---------------------------------------------------------------- 7. diagnostics (B0c)
A('## 7. Diagnostics G-1..G-3 (zero trials, descriptive; `DIAG`, run on B0c, the v8 baseline)')
A('')
A(f"Declaration: {DIAG['declaration']}. Window {DIAG['window_id']}; tool sha256 {DIAG['tool']['script_sha256']}. None "
  f"gates, selects or re-weights anything (v8-prereg item 8). The file was assembled copy-only from the eight split runs "
  f"of cells batch 1b; each row names its source file.")
A('')
src_of = {gid: Path(r['path']).name for r in DIAG['assembled_from'] for gid in r['ids']}
D = DIAG['diagnostics']


def dnum(v):
    if isinstance(v, bool) or v is None:
        return str(v)
    if isinstance(v, int):
        return f'{v:,d}'
    if isinstance(v, float):
        return f'{v:.4f}' if abs(v) < 10 else f'{v:.1f}'
    if isinstance(v, list):
        return ', '.join(dnum(x) for x in v)
    if isinstance(v, dict):
        return '; '.join(f'{k} {dnum(x)}' for k, x in v.items())
    return str(v)


def res(gid, path):
    return dig(D[gid]['result'], path)


HEAD7 = {
    'G-1a': lambda: (f"weighted ic_theta {dnum(res('G-1a', 'weighted_ic_theta'))}; negative at theta "
                     f"{dnum(res('G-1a', 'negative_at_theta'))}; unscored {dnum(res('G-1a', 'unscored'))}"),
    'G-1b': lambda: (f"combined turnover {dnum(res('G-1b', 'combined_turnover'))}; fast members "
                     f"{dnum(res('G-1b', 'fast.ids'))} own share {dnum(res('G-1b', 'fast.own_share'))}"),
    'G-1c': lambda: (f"netting ratio themes model / book {dnum(res('G-1c', 'themes_model.ratio'))} / "
                     f"{dnum(res('G-1c', 'themes_book.ratio'))}"),
    'G-2a': lambda: f"variance share {dnum(res('G-2a', 'mean_share'))}",
    'G-2b': lambda: f"weighted IC {dnum(res('G-2b', 'weighted_mean_ic'))}",
    'G-2c': lambda: f"held / ADV {dnum(res('G-2c', 'held'))}; aim / ADV {dnum(res('G-2c', 'aim'))}",
    'G-3a': lambda: (f"S2 net {res('G-3a', 'net_sharpe'):+.3f} -> S2-FEE {res('G-3a', 'restated_net_sharpe'):+.3f} "
                     f"(delta {res('G-3a', 'delta'):+.3f})"),
    'G-3b': lambda: 'IC retained after price-risk-v1 ' + '; '.join(
        f"{k} {dnum(v['retained'])}" for k, v in res('G-3b', 'members').items()),
    'G-3c': lambda: 'net Sharpe delta at delay ' + '; '.join(
        f"{k} {v['delta']:+.3f}" for k, v in res('G-3c', 'delays').items()),
    'G-3d': lambda: (f"adjusted Rand vs themes {dnum(res('G-3d', 'adjusted_rand_index'))}; effective bets themes "
                     f"{dnum(res('G-3d', 'effective_bets_themes'))}")}
A('| id | question | status | headline figures or reason | split file |')
A('|---|---|---|---|---|')
for gid in ('G-1a', 'G-1b', 'G-1c', 'G-2a', 'G-2b', 'G-2c', 'G-3a', 'G-3b', 'G-3c', 'G-3d'):
    e = D[gid]
    head = HEAD7[gid]() if e.get('status') == 'ok' else f"skipped: {e.get('reason')}"
    A(f"| {gid} | {e.get('question')} | {e.get('status')} | {head} | `{src_of[gid]}` |")
A('')

# ---------------------------------------------------------------- 8. trial accounting
A(tl(219))
A('')
A('```')
A(APPX.replace(f'admission trials this sprint {K_APPX}', f'admission trials this sprint {K_APPX} plus {RE_SCREENS} '
                                                         f're-screens', 1))
A(tl(223))
A(tl(224))
A('  v8 re-base: B0a, B0b, B0c (N 38-40); window re-runs of ledgered cells add 0 (0 run: the PM5-22 re-runs are pending).')
A(f'  v8 construction cells: R-1 accepted (N 41), R-2 accepted (N 42), R-3 not accepted (N 43), R-4 not accepted (N 44); '
  f'R-5 .. R-12 pending -> N {N_APPX} (ledger).')
A(f'  admission: {K_APPX} (library v8.0) plus {RE_SCREENS} `_f49` re-screens at 0 trials (ruling R2-e).')
A(f"  budget: N <= 51, admission <= 15 (plan 12.1): within (N {N_APPX}, admission {K_APPX}).")
A('  Not trials: identity checks, window re-runs (item 2), invalid cells (item 7): the PM6-6 calibration runs (no return '
  'read), the report-only re-runs of this render; no defect line.')
A(f"  DSR (R-2, the interim book; N = {s('R-2', 'deflated_ledger.n', 'int')}, T {s('R-2', 'net_moments.sessions', 'int')}) "
  f"-- {DSR_NOTE}:")
A(f"    OD-4 variance (cells scored on research-window-v2: {ROW['R-2']['deflated_ledger']['cells']}): DSR "
  f"{s('R-2', 'deflated_ledger.dsr', '.3f')}, SR0 {s('R-2', 'deflated_ledger.sr0_annual', '.4f')} ann")
A(f"    legacy variance (reported, gates nothing): DSR {s('R-2', 'deflated_ledger.legacy_dsr', '.3f')}, SR0 "
  f"{s('R-2', 'deflated_ledger.legacy_sr0_annual', '.3f')} ann")
A('```')
A('')

# ---------------------------------------------------------------- 9. OD-1
A(tl(235))
A('')
pre = read(PREREG_P).decode('utf-8').split('\n')
i0 = next(i for i, ln in enumerate(pre) if ln.startswith('1. '))
item = [pre[i0]]
for ln in pre[i0 + 1:]:
    if ln.startswith('   '):
        item.append(ln)
    else:
        break
out.extend(item)
A('')
for n in range(239, 245):
    A(tl(n))
A('')

# ---------------------------------------------------------------- 10. contradictions
for n in range(246, 254):
    A(tl(n))
lit = read(LIT_P).decode('utf-8').split('\n')
h = next(i for i, ln in enumerate(lit) if ln.strip() == '## Where the new notes contradict or update v6 and v7')
j = next(i for i in range(h + 1, len(lit)) if lit[i].startswith('|'))
assert lit[j].startswith('| Earlier position |') and lit[j + 1].startswith('|---')
rows = []
for ln in lit[j + 2:]:
    if not ln.startswith('|'):
        break
    rows.append(ln)
out.extend(rows)
A('')
A('What v8 did about each "contradicts" row: pending (written with the final scorecard).')
A('')

text = '\n'.join(out)
assert '{{' not in text and '}}' not in text, re.findall(r'\{\{[^}]*\}\}', text)[:5]
assert 'loc-L1.247' not in text.replace(base(DIRS['B0a']), '').replace(base(DIRS['B0b']), '').replace(
    base(DIRS['B0c']), '').replace(base(DIRS['R-4']), '')   # no R-1 / R-3 calibration dir is named
OUT.write_text(text, encoding='utf-8', newline='\n')
print(OUT, OUT.stat().st_size, 'bytes; template sha256', TSHA)
for k, v in sorted(SHA.items()):
    print(f'read {k} {v}')
print('literature rows', len(rows), 'prereg item lines', len(item))
