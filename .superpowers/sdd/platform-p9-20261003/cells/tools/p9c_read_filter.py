"""R15-WAVE-READ: allowlisted metadata or existing bundle rho keys only. Never launches a process."""
import hashlib
import csv
import json
import math
from pathlib import Path
import re
import sys

WAVE = Path('build-equity/waves/p9c-info-clock')
STAGES = ('preflight', 'register', 'screen', 'spec', 'run', 'match', 'verify', 'judge', 'record')
EVAL = Path('build-equity-rel/bin/atx-research-eval.exe')
PIN = 'e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c'


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def doc(p):
    return json.loads(Path(p).read_text(encoding='utf-8-sig'))


def metadata(path):
    # Reconstruct stage labels; never echo a raw line or an exception/verdict payload.
    raw = Path(path).read_bytes()
    text = raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig', errors='replace')
    for line in text.splitlines():
        m = re.fullmatch(r'== (preflight|register|screen|spec|run|match|verify|judge|record)(?:: done \([0-9]{2}-[a-z]+\.json\))?', line)
        if m:
            print('stage: ' + m.group(1))
    for i, stage in enumerate(STAGES, 1):
        p = WAVE / 'receipts' / f'{i:02}-{stage}.json'
        if p.is_file():
            r = doc(p)
            print(json.dumps({'stage': stage, 'receipt': str(p), 'seconds': r.get('seconds')}))
        for failed in sorted((WAVE / 'receipts').glob(f'{i:02}-{stage}.failed-*.json')):
            print(json.dumps({'stage': stage, 'failed_receipt': str(failed), 'contents': 'withheld'}))
    print(json.dumps({'raw_console': str(path), 'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw)}))


def rho():
    p = WAVE / 'bundle.json'
    bundle = doc(p)
    judge = doc(WAVE / 'receipts' / '08-judge.json')
    if sha(p) != judge['outputs']['bundle']['sha256']:
        raise SystemExit('STOP: bundle hash differs from judge receipt')
    base = 'build-equity/mega-nav-p9-theme-cov-hedge-gm'
    cell = 'build-equity/mega-nav-p9-info-clock-gm'
    if Path(bundle.get('base', '')).as_posix() != base or Path(bundle.get('final', '')).as_posix() != cell:
        raise SystemExit('STOP: bundle pair differs from registered C/H paths')
    if sha(EVAL) != PIN or bundle.get('nav_summ_run', {}).get('engine_exe_sha256') != PIN:
        raise SystemExit('STOP: bundle eval producer differs from frozen pin')
    paired = bundle.get('paired')
    if not isinstance(paired, dict) or type(paired.get('sessions')) is not int or paired['sessions'] < 2:
        raise SystemExit('STOP: invalid paired sessions/structure')
    value = paired.get('rho')
    if type(value) not in (int, float) or not math.isfinite(value) or not -1 <= value <= 1:
        raise SystemExit('STOP: invalid paired rho')
    if value == 0.999999:
        raise SystemExit('STOP: rho lies on the printed threshold boundary')
    if 'degenerate' not in paired and 'one_minus_rho' not in paired:
        flag = False
        provenance = 'inferred from documented nondegenerate encoding, nav_summ.py:249-273; R15-V2-RHO'
    elif paired.get('degenerate') is True and type(paired.get('one_minus_rho')) in (int, float) and \
            math.isfinite(paired['one_minus_rho']) and 0 <= paired['one_minus_rho'] < 1e-6:
        flag = True
        provenance = 'supplied engine degenerate flag and stable difference in pinned bundle'
    else:
        raise SystemExit('STOP: inconsistent paired degeneracy encoding')
    keys = ('sessions', 'rho', 'one_minus_rho', 'degenerate')
    print(json.dumps({'path': str(p), 'sha256': sha(p), 'base': base, 'final': cell,
                      'engine_exe_sha256': PIN, 'paired': {k: paired.get(k) for k in keys},
                      'not_supplied': [k for k in keys if k not in paired],
                      'one_minus_rho_text': paired.get('one_minus_rho', 'not supplied by bundle'),
                      'threshold_flag': flag, 'threshold_flag_provenance': provenance}, indent=2))


def mechanics():
    verify = doc(WAVE / 'receipts/07-verify.json')['outputs']['mechanics']
    print(json.dumps({'pass': verify['pass'], 'rows': [
        {k: row[k] for k in ('check', 'value', 'limit', 'pass')} for row in verify['rows']]}, indent=2))


def prints():
    logs = Path('.superpowers/sdd/platform-p9-20261003/cells/logs')
    logged = doc(logs / 'p9c-rho-first.txt')
    assert logged['sha256'] == sha(WAVE / 'bundle.json')
    assert doc(WAVE / 'receipts/07-verify.json')['outputs']['mechanics']['pass'] is True
    judge = doc(WAVE / 'receipts/08-judge.json')['outputs']
    assert sha(WAVE / 'bundle.json') == judge['bundle']['sha256']
    bundle = doc(WAVE / 'bundle.json')
    paired = bundle['paired']
    verdict = judge['verdict']
    result = {'decision': {k: verdict[k] for k in ('rule', 'accepted', 'dsr')},
              'checks': {k: verdict['checks'][k] for k in ('dsr_positive', 'mechanics')},
              'paired': {k: paired.get(k) for k in ('dsr', 'memmel_se', 'sr', 'sr_reference')},
              'p_two_sided': paired.get('lw', {}).get('p_value'),
              'p_one_sided': paired.get('lw', {}).get('p_one_sided'),
              'books': {side: {k: judge['book'][side][k] for k in ('cost_bps_traded', 'x4_net_sharpe')}
                        for side in ('cell', 'parent')},
              'expected_effect_nondeciding': '+.02 to +.06 net Sharpe; book gross +~5%; capacity down [est]'}
    print(json.dumps(result, indent=2))


def clock_prints():
    logs = Path('.superpowers/sdd/platform-p9-20261003/cells/logs')
    assert doc(logs/'p9c-rho-first.txt')['sha256'] == sha(WAVE/'bundle.json')
    assert doc(WAVE/'receipts/07-verify.json')['outputs']['mechanics']['pass'] is True
    out = {}
    for side, directory in [('parent','build-equity/mega-nav-p9-theme-cov-hedge-gm'),
                            ('cell','build-equity/mega-nav-p9-info-clock-gm')]:
        path = Path(directory)/'summary.json'
        scenarios = []
        for s in doc(path)['scenarios']:
            clock = s['info_clock']
            selected = {k: clock[k] for k in ('rate','window_sessions','theta_fast','theta_slow',
                'name_days','fast_name_days','missing_name_days','fast_share','planned_turnover',
                'fast_planned_turnover','turnover_share','definition')}
            selected['field'] = {k: clock['field'][k] for k in ('name','sha256')}
            scenarios.append({'scenario':s['scenario'],'primary':s['primary'],'info_clock':selected,
                'exposure':{k:s['exposure'][k] for k in ('max_gross_leverage','mean_gross_leverage')}})
        capacity = Path(directory)/'capacity_curve.csv'
        with capacity.open(newline='') as f:
            rows = list(csv.DictReader(f))
        cost = [r for r in rows if float(r['multiple']) == 1 and r['book']=='capacity-x1-v1+swap-fin-v1']
        x4 = [r for r in rows if float(r['multiple']) == 4 and r['book']=='capacity-x4-v1+swap-fin-v1']
        assert len(cost) == len(x4) == 1
        out[side] = {'source':str(path),'sha256':sha(path),'scenarios':scenarios,
            'capacity_source':str(capacity),'capacity_sha256':sha(capacity),
            'cost_bps_per_traded_dollar':float(cost[0]['cost_bps_per_traded_dollar']),
            'x4_net_sharpe':float(x4[0]['net_sharpe'])}
    print(json.dumps(out,indent=2))


if sys.argv[1] == 'metadata':
    metadata(sys.argv[2])
elif sys.argv[1] == 'rho':
    rho()
elif sys.argv[1] == 'mechanics':
    mechanics()
elif sys.argv[1] == 'prints':
    prints()
elif sys.argv[1] == 'clock':
    clock_prints()
else:
    raise SystemExit('usage: p9c_read_filter.py metadata RAW_LOG | mechanics | rho | prints | clock')
