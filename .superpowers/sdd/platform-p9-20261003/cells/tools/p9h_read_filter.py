"""R15-WAVE-READ: allowlisted metadata or existing bundle rho keys only. Never launches a process."""
import hashlib
import json
from pathlib import Path
import re
import sys

WAVE = Path('build-equity/waves/p9h-cov-hedge')
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
    base = 'build-equity/mega-nav-p9l-label-terminal-gm'
    cell = 'build-equity/mega-nav-p9-theme-cov-hedge-gm'
    if Path(bundle.get('base', '')).as_posix() != base or Path(bundle.get('final', '')).as_posix() != cell:
        raise SystemExit('STOP: bundle pair differs from registered H/L paths')
    if sha(EVAL) != PIN or bundle.get('nav_summ_run', {}).get('engine_exe_sha256') != PIN:
        raise SystemExit('STOP: bundle eval producer differs from frozen pin')
    paired = bundle['paired']
    keys = ('sessions', 'rho', 'one_minus_rho', 'degenerate')
    print(json.dumps({'path': str(p), 'sha256': sha(p), 'base': base, 'final': cell,
                      'engine_exe_sha256': PIN, 'paired': {k: paired.get(k) for k in keys},
                      'not_supplied': [k for k in keys if k not in paired]}, indent=2))


if sys.argv[1] == 'metadata':
    metadata(sys.argv[2])
elif sys.argv[1] == 'rho':
    rho()
else:
    raise SystemExit('usage: p9h_read_filter.py metadata RAW_LOG | rho')
