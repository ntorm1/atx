"""Fill the INTERIM scorecard v8 from the registered template (Ruling PM5-27). Every number comes from a file:
the interim nav_summ JSON (SUMM), the paired bundles (PAIRED[B0b]; B0c's information-only bundle), the NAV summary.json
of B0a / B0b / B0c (NAV[K]), B0c's v7_extras.json (capacity), the eight diagnostics split files (DIAG, per id), the
Appendix A stdout of nav_summ --ledger-n (APPX), v8-prereg.md and the literature review (verbatim text). Nothing of R-1
is opened. Usage: fill_scorecard_interim.py OUT.md APPX_STDOUT RENDER_HEAD UNAVAILABLE_COUNT"""
import glob
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path('C:/atx-wt/pool-2')
TEMPLATE = ROOT / 'docs/plans/mega-alpha-scorecard-v8.template.md'
OUT, APPX_STDOUT, HEAD, N_UNAV = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4]

DIRS = {'B0a': 'build-equity/mega-nav-v8-b0a-lo1-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247',
        'B0b': 'build-equity/mega-nav-v8-b0b-lo3-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247',
        'B0c': 'build-equity/mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247'}
SUMM_P = 'build-equity/mega-nav-v8-summ-interim.json'
PAIRED_P = {'B0b': 'build-equity/v8-cells-b0b-bundle.json'}
INFO_B0C_P = 'build-equity/v8-cells-b0c-bundle.json'
LEDGER_P = 'build-equity/trials.jsonl'
PREREG_P = '.superpowers/sdd/platform-v8-20260929/v8-prereg.md'
LIT_P = '.superpowers/sdd/platform-v8-20260929/reports/Equity long short alpha v8.md'
CAP_P = f"{DIRS['B0c']}/v7_extras.json"
R1 = ('R-1: constructed; mechanics fail (all-rows gross 1.0672, limit [.90, 1.05]); no return read; not ledgered; '
      'ruling open')
PEND, NOVF, NOTREAD = 'pending', 'pending (no V8-F)', 'not read (R-1)'
SCEN = {'S1': 'linear-6bps-stale5-v1+swap-fin-v1', 'S2': 'modeled-1bn-stale5-v1+swap-fin-v1',
        'tiers': 'modeled-1bn-stale5-v1+engine-tiers-v1', 'flat300': 'modeled-1bn-stale5-v1+flat-300-v0',
        'S3': 'modeled-1bn-terminal-adverse-v1+swap-fin-v1'}
SHA = {}


def read(rel):
    data = (ROOT / rel).read_bytes()
    SHA[rel] = hashlib.sha256(data).hexdigest()
    return data


def jload(rel):
    return json.loads(read(rel).decode('utf-8'))


def base(p):
    return str(p).replace('\\', '/').rstrip('/').split('/')[-1]


SUMM = {base(r['dir']): r for r in jload(SUMM_P)}
ROW = {k: SUMM[base(d)] for k, d in DIRS.items()}
PAIRED = {k: jload(p) for k, p in PAIRED_P.items()}
INFO_B0C = jload(INFO_B0C_P)
NAV = {k: jload(f'{d}/summary.json') for k, d in DIRS.items()}
CAP = jload(CAP_P)
read(LEDGER_P)
DIAG_FILES = sorted(glob.glob(str(ROOT / 'build-equity/b0c-diagnostics/diagnostics-v8-*.json')))
DIAG, DIAG_SRC, HDRS = {}, {}, set()
for f in DIAG_FILES:
    rel = Path(f).relative_to(ROOT).as_posix()
    d = jload(rel)
    HDRS.add((d['declaration'], d['window_id'], d['tool']['script_sha256']))
    for gid, e in d['diagnostics'].items():
        assert gid not in DIAG, gid
        DIAG[gid], DIAG_SRC[gid] = e, Path(rel).name
assert len(HDRS) == 1 and len(DIAG) == 10, (HDRS, sorted(DIAG))
DECL, WID, TOOLSHA = next(iter(HDRS))
APPX_TXT = APPX_STDOUT.read_text(encoding='utf-8')
APPX = [ln for ln in APPX_TXT.splitlines() if ln.startswith('TRAIN construction cells')]
assert len(APPX) == 1, APPX
APPX = APPX[0]
m = re.search(r'TRAIN construction cells (\d+); admission trials this sprint (\d+);.*validation reads before v8: (\d+) '
              r'\(.*history reads (\d+);', APPX)
N_APPX, K_APPX, VAL_APPX, HIST_APPX = m.groups()


def scen(doc, sid):
    return next(s for s in doc['scenarios'] if s.get('scenario') == sid)


def fmt(v, spec):
    if v is None:
        return 'n/a'
    if spec == 'int':
        return f'{int(v):d}'
    if spec.startswith('+pct') or spec.startswith('pct'):
        d = int(spec[-1])
        return f"{v * 100:{'+' if spec[0] == '+' else ''}.{d}f}%"
    return format(v, spec)


SPEC = {'net_sharpe': '+.3f', 'gross_sharpe': '+.3f', 'restated_net_sharpe': '+.3f', 'tau_gmv_mean': '.4f',
        'tau_gmv_p95': '.4f', 'cost_bps_traded': '.2f', 'mean_gross_leverage_all_rows': '.4f',
        'mean_net_leverage_all_rows': '+.4f', 'dsr': '.3f', 'memmel_se': '.3f', 'p_one_sided': '.4f',
        'p_value': '.4f', 'net_return': '+pct1', 'ann_vol': 'pct2', 'return_rows': 'int', 'n': 'int',
        'sessions': 'int', 'psr': '.4f', 'min_trl_years': '.2f', 'sr0_annual': '.4f', 'legacy_dsr': '.3f',
        'legacy_sr0_annual': '.3f'}


def dig(obj, path):
    cur = obj
    for part in path.split('.'):
        if isinstance(cur, list):
            cur = cur[int(part)]
        else:
            cur = cur[part]
    return cur


def summ(k, path):
    v = dig(ROW[k], path)
    last = path.split('.')[-1]
    spec = SPEC[last]
    if path.startswith('deflated') and last == 'dsr':
        spec = '.3f'
    return fmt(v, spec)


def paired(k, path):
    v = dig(PAIRED[k], path)
    last = path.split('.')[-1]
    return fmt(v, '+.3f' if path == 'paired.dsr' else SPEC[last])


def dnum(v):
    if isinstance(v, bool) or v is None:
        return str(v)
    if isinstance(v, int):
        return f'{v:,d}'
    if isinstance(v, float):
        return f'{v:.4f}' if abs(v) < 10 else f'{v:.1f}'
    return str(v)


def dval(v):
    if isinstance(v, list):
        return ', '.join(dval(x) for x in v)
    if isinstance(v, dict):
        return '; '.join(f'{k} {dval(x)}' for k, x in v.items())
    return dnum(v)


def diag(path):
    """DIAG.<path>: the header of the split files, or diagnostics.<id>.<rest> from the id's split file."""
    if path == 'declaration':
        return DECL
    if path == 'window_id':
        return WID
    if path == 'tool.script_sha256':
        return TOOLSHA
    m_ = re.match(r'diagnostics\.(G-\d[a-d])\.(.+)$', path)
    gid, rest = m_.groups()
    e = DIAG[gid]
    star = re.match(r'(.+)\[\*\]\.(\w+)$', rest)
    if star:
        coll, key = dig(e, star.group(1)), star.group(2)
        items = coll.items() if isinstance(coll, dict) else enumerate(coll)
        return '; '.join(f'{k} {dnum(x[key])}' for k, x in items)
    v = dig(e, rest)
    if rest.endswith(('net_sharpe', 'delta')) and isinstance(v, float):
        return f'{v:+.3f}'
    return dval(v)


PH = re.compile(r'\{\{([^{}]+)\}\}')


def resolve(expr, pm):
    """One placeholder: computed aliases from their file; V8-F / pending cells / absent aliases as pending; {{PM}}
    statements from the per-line list ``pm`` (in order)."""
    if expr.startswith('PM'):
        if not pm:
            raise KeyError(f'unhandled PM placeholder {{{{{expr}}}}}')
        return pm.pop(0)
    m_ = re.match(r'SUMM\[([^\]]+)\]\.(.+)$', expr)
    if m_:
        k, path = m_.groups()
        if k in ROW:
            return summ(k, path)
        return NOTREAD if k == 'R-1' else (NOVF if k == 'V8-F' else PEND)
    m_ = re.match(r'PAIRED\[([^\]]+)\]\.(.+)$', expr)
    if m_:
        k, path = m_.groups()
        if k in PAIRED:
            return paired(k, path)
        return NOTREAD if k == 'R-1' else PEND
    if expr.startswith('DIAG.'):
        return diag(expr[5:])
    if re.match(r'(BUNDLE|NAVF|ADM|W|CARDS|RECIPE|LIB)\b', expr) or re.match(r'(CAP|NAV)\[R-', expr):
        return NOVF if expr.startswith(('BUNDLE', 'NAVF', 'ADM', 'W.', 'CARDS', 'RECIPE')) else PEND
    raise KeyError(f'unhandled placeholder {{{{{expr}}}}}')


def fill(line, pm=()):
    pm = list(pm)
    out = PH.sub(lambda m_: resolve(m_.group(1).strip(), pm), line)
    if pm:
        raise ValueError(f'unused PM fills {pm} for line {line[:80]!r}')
    return out


T = TEMPLATE.read_text(encoding='utf-8').split('\n')
TSHA = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()


def tl(n):
    """Template line n (1-based)."""
    return T[n - 1]


def vol(k):
    return fmt(scen(NAV[k], SCEN['S2'])['ann_vol'], 'pct2')


DSR_NOTE = 'not meaningful yet: 3-cell variance, PM5-22'
b0c_name = base(DIRS['B0c'])
out = []
A = out.append

# ---------------------------------------------------------------- title and the interim section
A(fill(tl(1), ['2026-10-01; INTERIM at the owner stop']))
A('')
A('## Interim status (owner stop, 2026-10-01)')
A('')
A('This is an interim scorecard, ordered by the owner\'s stop of the v8 sprint (Ruling PM5-27). It is rendered from the '
  'registered template on the cells that exist; every cell that has not run is marked pending.')
A('')
A(f'- Run: B0a, B0b and B0c, on TRAIN 2020-2023 with S2 as the primary scenario. Each of them is ledgered.')
A(f'- B0c is the re-based v8 baseline (baseline by declaration, v8-prereg item 6). Its S2 net Sharpe is '
  f'{summ("B0c", "net_sharpe")} (section 1).')
A('- No improvement over B0c is claimed. No construction cell is accepted and no cumulative test (V8-F against B0c) '
  'exists.')
A(f'- {R1}.')
A('- The freeze gate (v8-prereg item 9) is not evaluated. V8-F does not exist.')
A('- The deflated Sharpe is not meaningful yet. Its pre-registered cross-trial variance rests on three cells (B0a, B0b, '
  'B0c) until the registered re-runs of the ledgered v7 cells on the four-year window are done (Ruling PM5-22). It is '
  'printed only where the tool prints it (sections 1 and 8), with this note beside it.')
A(f'- TRAIN construction cells N {N_APPX}; admission trials this sprint {K_APPX}; history reads {HIST_APPX}; hidden 2024+ '
  f'unread in this sprint; validation reads before v8: {VAL_APPX} (the Appendix A block below).')
A('- Cells still to run: the ruling on R-1, R-2..R-8, then R-9a..R-9c or R-10..R-12 (the branch set by R-6), the re-runs '
  'of the ledgered v7 cells on the four-year role (PM5-22; they add 0 to N), V8-F.')
A(f'- The pitch rendered from the interim config (`docs/plans/2026-10-01-mega-alpha-v8-interim-pitch.html`) shows '
  f'{N_UNAV} unavailable or refused blocks, each naming its missing input; integration-log.md section "interim report '
  f'(owner stop)" lists them.')
A('')
A('Appendix A block (`nav_summ.py --protocol v8 --ledger-n build-equity/trials.jsonl`, stdout verbatim; R-2 has not run, '
  'so no `_f49` re-screen exists and ruling R2-e\'s " plus 8 re-screens" is not inserted):')
A('')
A('```')
A(APPX)
A('```')
A('')

# ---------------------------------------------------------------- template header paragraph (lines 23-31)
A(fill(tl(24), [f'{HEAD[:8]} (render; the interim config commit)']))
for n in range(25, 31):
    A(tl(n))
A('')

# ---------------------------------------------------------------- inputs (lines 32-50)
A('## Inputs (interim: what exists at the owner stop)')
A('')
A(tl(34))
A(tl(35))
A(f"| `SUMM` | `{SUMM_P}` (sha256 `{SHA[SUMM_P]}`) | bounded `nav_summ.py --protocol v8 --dsr-ledger "
  f"build-equity/trials.jsonl --effective-n dirs --psr --json {SUMM_P}` over the B0a, B0b and B0c dirs (no `--ledger`: "
  f"no line written) |")
A(f"| `PAIRED[B0b]` | `{PAIRED_P['B0b']}` (sha256 `{SHA[PAIRED_P['B0b']]}`) | `nav_summ.py --protocol v8 --bundle <B0a dir> "
  f"<B0b dir> --bundle-json <path>` (cells batch 1a, bounded, no `--ledger`) |")
A(f"| `PAIRED[R-1 .. R-12]` | pending | R-1: not run (stopped at mechanics; no return read); R-2 .. R-12 not run |")
A(f"| B0c vs B0b (information only) | `{INFO_B0C_P}` (sha256 `{SHA[INFO_B0C_P]}`) | PM5-23 bundle of cells batch 1b "
  f"(B0c is the baseline by declaration; it gates nothing) |")
A('| `BUNDLE` | pending (no V8-F) | - |')
A(f"| `LEDGER` | `{LEDGER_P}` (sha256 `{SHA[LEDGER_P]}`) | the cycle's ledger lines (41 lines; unchanged by this report) |")
A(f"| `APPX` | `build-equity/mega-nav-v8-appx-interim-run/stdout.log` (sha256 "
  f"`{hashlib.sha256(APPX_STDOUT.read_bytes()).hexdigest()}`) | bounded `nav_summ.py --protocol v8 --ledger-n "
  f"build-equity/trials.jsonl` |")
A('| `DIAG` | not produced as one `diagnostics-v8.json` | E-18 ran G-1..G-3 on B0c as eight split runs; no tool verb '
  'combines them and they are not merged. Section 7 reads each id from its split file: '
  + ', '.join(f"`build-equity/b0c-diagnostics/{Path(f).name}` (`{SHA[Path(f).relative_to(ROOT).as_posix()][:12]}`)"
              for f in DIAG_FILES) + ' |')
A('| `CARDS`, `ADM`, `W`, `NAVF`, `LIB`, `RECIPE` | pending (no V8-F) | - |')
A('| `NAV[K]` | ' + '; '.join(f"`{DIRS[k]}/summary.json` (`{SHA[DIRS[k] + '/summary.json'][:12]}`)" for k in DIRS)
  + ' | the NAV runs of B0a, B0b, B0c (cost scenarios, whole-window S2 volatility) |')
A(f"| `CAP[R-3]`, `CAP[R-5]` | pending | - |")
A(f"| B0c capacity (E-29) | `{CAP_P}` (sha256 `{SHA[CAP_P]}`) | B0c's NAV run with `--capacity-curve` (report only) |")
A(f"| `PREREG` | `{PREREG_P}` | root (pins filled) |")
A(f"| `LIT` | `{LIT_P}` | the v8 literature review |")
A('')

# ---------------------------------------------------------------- cells (lines 52-74)
for n in range(52, 58):
    A(tl(n))
A(fill(tl(58), [b0c_name]))
r59 = fill(tl(59), [R1])
assert '| B0c | 41 |' in r59
A(r59.replace('| B0c | 41 |', f'| B0c | 41 (plan; not ledgered, N stays {N_APPX}) |'))
for n in range(60, 73):
    A(fill(tl(n), ['pending run']))
A('')
A(fill(tl(74), ['none at the owner stop (no construction cell is accepted; V8-F pending)']))
A('')

# ---------------------------------------------------------------- 1. headline (lines 76-107)
for n in range(76, 80):
    A(tl(n))
A(f'| **Final cell (V8-F)** | {NOVF} | **{NOVF}** | {NOVF} | {NOVF} | {NOVF} | freeze gate not evaluated |')
row81 = fill(tl(81), [b0c_name])
row81 = row81.replace(f"| {summ('B0c', 'deflated_ledger.dsr')} |",
                      f"| {summ('B0c', 'deflated_ledger.dsr')} (N {summ('B0c', 'deflated_ledger.n')}; {DSR_NOTE}) / "
                      f"{summ('B0c', 'deflated_effective_n.dsr')} / {summ('B0c', 'deflated_lo_null.dsr')} |")
A(row81)
row82 = fill(tl(82))
row82 = row82.replace(f"| {summ('B0a', 'deflated_ledger.dsr')} |",
                      f"| {summ('B0a', 'deflated_ledger.dsr')} (N {summ('B0a', 'deflated_ledger.n')}; {DSR_NOTE}) / "
                      f"{summ('B0a', 'deflated_effective_n.dsr')} / {summ('B0a', 'deflated_lo_null.dsr')} |")
A(row82)
A('')
A('**The pre-registered freeze gate is not evaluated: V8-F does not exist at the owner stop.** Cumulative paired test V8-F '
  'vs B0c: pending (`BUNDLE` not produced). Year net returns of V8-F: pending. Planning value: live net Sharpe .7 to 1.0 '
  '[est] against the TRAIN figure of V8-F, pending. The DSR column reads the ledger N and the effective N / Lo null of '
  'the three listed dirs; none of them is meaningful yet (PM5-22).')
A('')
for n in range(92, 96):
    A(tl(n))
for n in range(96, 103):
    A(fill(tl(n), ['not evaluated']))
A(fill(tl(103), ['NOT EVALUATED (no V8-F)']))
A('')
A('Beside the gate (gate nothing): pending (no V8-F). The gate is not evaluated, so it is not "unmet": OD-3 is not '
  'invoked.')
A('')

# ---------------------------------------------------------------- 2a. cells (lines 109-141)
for n in range(109, 122):
    A(tl(n))
A(fill(tl(122), [vol('B0a'), 're-base ledgered']))
A(fill(tl(123), [vol('B0b'), 'accepted']))
A(fill(tl(124), [vol('B0c')]).replace('| baseline |', '| baseline by declaration |'))
A(f'| 4 | R-1 | composition v8 | B0c | not ledgered (N stays {N_APPX}) | {NOTREAD} | {NOTREAD} | {NOTREAD} | '
  f'{NOTREAD} | {NOTREAD} | {NOTREAD} | {NOTREAD} | not run | not run | turnover per unit gross (executed: tau_gmv_mean / '
  f'mean_gross_leverage_all_rows, S2) not higher than the parent, PM5-11: not read | {R1} |')
for n in range(126, 139):
    cells = tl(n).split(' | ')
    num, key, lever, nplan = cells[0].lstrip('| '), cells[1], cells[2], cells[4]
    A(f'| {num} | {key} | {lever} | last accepted | {nplan} (plan) | {PEND} | {PEND} | {PEND} | {PEND} | {PEND} | {PEND} | '
      f'{PEND} | {PEND} | {PEND} | {PEND} | pending run |')
A('')
ib = INFO_B0C['paired']
A(f"B0c against B0b, information only (B0c is the baseline by declaration; gates nothing; `{INFO_B0C_P}`, PM5-23): dSR "
  f"{fmt(ib['dsr'], '+.3f')} (SE {fmt(ib['memmel_se'], '.3f')}), studentized bootstrap p one-sided "
  f"{fmt(ib['lw']['p_one_sided'], '.4f')}, two-sided {fmt(ib['lw']['p_value'], '.4f')}. The volatility column is the "
  f"whole-window annualised S2 volatility of each cell's NAV summary.json (`NAV[K]`).")
A('')
A('Not trials (identity checks and window re-runs of ledgered cells, v8-prereg item 2): no window re-run is ledgered (the '
  'Appendix A block counts 0 window re-run lines; the PM5-22 re-runs are not run); the identity checks of the '
  'integrations and Wave 0 are recorded in integration-log.md and add no trial. Invalid cells excluded by the defect rule '
  '(item 7): none (0 defect lines).')
A('')

# ---------------------------------------------------------------- 2b. year tables (lines 143-164)
for n in range(143, 150):
    A(tl(n))
A(fill(tl(150)))
for k in ('B0a', 'B0b'):
    A(fill(tl(150).replace('[B0c]', f'[{k}]')).replace('| B0c |', f'| {k} |', 1))
A(f'| R-1 | {NOTREAD} | {NOTREAD} | {NOTREAD} | {NOTREAD} | {NOTREAD} |')
A(f'| R-2 .. R-12 | {PEND} | {PEND} | {PEND} | {PEND} | {PEND} |')
A(f'| **V8-F** | {NOVF} | {NOVF} | {NOVF} | {NOVF} | {NOVF} |')
A('')
A('V8-F year table in full: pending (no V8-F). B0c, the baseline, in full (return rows, net Sharpe, return, volatility, '
  'turnover, cost per traded dollar):')
A('')
A(tl(156))
A(tl(157))
for n in range(158, 162):
    A(fill(tl(n).replace('[V8-F]', '[B0c]')))
A('')
A('Cumulative dSR V8-F vs B0c by year (`BUNDLE.years`): pending (no V8-F).')
A('')

# ---------------------------------------------------------------- 3. stresses (lines 166-176)
for n in range(166, 172):
    A(tl(n))
A(f'| **V8-F** | {NOVF} | **{NOVF}** | {NOVF} | {NOVF} | {NOVF} | {NOVF} |')


def sc(k, key):
    return fmt(scen(NAV[k], SCEN[key])['net_sharpe'], '+.3f')


A(fill(tl(173), [sc('B0c', 'S1'), sc('B0c', 'tiers'), sc('B0c', 'flat300'), sc('B0c', 'S3')]))
A(fill(tl(174), [sc('B0a', 'S1'), sc('B0a', 'tiers'), sc('B0a', 'flat300'), sc('B0a', 'S3')]))
A('')
A('Capacity (R-5 criterion, `CAP[R-5]`): pending (R-5 not run).')
caps = sorted(CAP['capacity'], key=lambda r: r['multiple'])
assert [r['multiple'] for r in caps] == [0.5, 1.0, 2.0, 4.0, 8.0]
A(f"Capacity of B0c (E-29, report only, gates nothing; `{CAP_P}`; x1 = the primary S2 book bit for bit: "
  f"{CAP['capacity_x1_equals_primary_bit_for_bit']}): net Sharpe at .5 / 1 / 2 / 4 / 8 x $1bn "
  + ' / '.join(fmt(r['net_sharpe'], '+.3f') for r in caps)
  + '; cost bps per traded dollar ' + ' / '.join(fmt(r['cost_bps_per_traded_dollar'], '.2f') for r in caps)
  + ". Ruling PM4-5: the capacity books scale only the impact law (m^0.5) and the participation cap (1/m); the aim is the "
    "$1bn book's at every multiple, so the 2x and 4x rows are slightly optimistic on the aim cap.")
A('')

# ---------------------------------------------------------------- 4-6. final library (lines 178-199)
A(tl(178))
A('')
A('Pending: V8-F does not exist at the owner stop. The walk-through is written with the final scorecard.')
A('')
A(fill(tl(185), ['pending (no V8-F)']))
A('')
for n in range(187, 193):
    A(tl(n))
A('| ' + ' | '.join([NOVF] * 12) + ' |')
A('')
A('Theme mass on the final role: pending (no V8-F).')
A('')
A(tl(197))
A('')
A('Pending (no V8-F library).')
A('')

# ---------------------------------------------------------------- 7. diagnostics (lines 201-217)
A(tl(201))
A('')
A('`DIAG` as one `diagnostics-v8.json` was not produced: E-18 ran the ten diagnostics on B0c as eight split runs, no tool '
  'verb combines them, and they are not merged (the pitch\'s diagnostics block is unavailable for this reason). Each row '
  'below reads `diagnostics.<id>` of the split file named in its last column; the eight files share one header.')
A('')
A(fill(tl(203)))
A(tl(204))
A('')
A(tl(206).rstrip(' |').rstrip() + ' | split file |')
A(tl(207) + '---|')
for n in range(208, 218):
    gid = tl(n).split(' | ')[0].lstrip('| ').strip()
    A(fill(tl(n)) + f' `{DIAG_SRC[gid]}` |')
A('')

# ---------------------------------------------------------------- 8. trial accounting (lines 219-233)
A(tl(219))
A('')
A('```')
A(APPX)
A(tl(223))
A(tl(224))
A(fill(tl(225), ['0 run: the PM5-22 re-runs are pending']))
A(f'  v8 construction cells: {R1}. R-2 .. R-12 pending -> N {N_APPX} (ledger).')
A(f'  admission: {K_APPX} (R-2 not started) plus 0 `_f49` re-screens (ruling R2-e; none run).')
A(fill(tl(228), [f'within (N {N_APPX}, admission {K_APPX})']))
A(fill(tl(229), ['none ledgered (0 window re-run lines, 0 defect lines; 1 protocol line)']))
A(f"  DSR (B0c, the last ledgered cell; V8-F pending; N = {summ('B0c', 'deflated_ledger.n')}, T "
  f"{summ('B0c', 'net_moments.sessions')}) -- {DSR_NOTE}:")
A(f"    OD-4 variance (cells scored on research-window-v2: {ROW['B0c']['deflated_ledger']['cells']}): DSR "
  f"{summ('B0c', 'deflated_ledger.dsr')}, SR0 {summ('B0c', 'deflated_ledger.sr0_annual')} ann")
A(f"    legacy variance (reported, gates nothing): DSR {summ('B0c', 'deflated_ledger.legacy_dsr')}, SR0 "
  f"{summ('B0c', 'deflated_ledger.legacy_sr0_annual')} ann")
A('```')
A('')

# ---------------------------------------------------------------- 9. OD-1 (lines 235-244)
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

# ---------------------------------------------------------------- 10. contradictions (lines 246-257)
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
OUT.write_text(text, encoding='utf-8', newline='\n')
print(OUT, OUT.stat().st_size, 'bytes; template sha256', TSHA)
for k, v in sorted(SHA.items()):
    print(f'read {k} {v}')
print('literature rows', len(rows), 'prereg item lines', len(item))
