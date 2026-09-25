"""Frozen W0 G0 evidence runner. Only pre-2020 inputs; never writes C:/atx."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid

import psutil

TREE = Path('C:/atx-wt/pool-3')
OUT = Path('C:/atx-wt/g0-data/bc5cc646_20260925')
DATA = Path('C:/atx/data')
BIN = TREE / 'build-equity-rel/bin'
SHA = 'bc5cc646'
BASE_SHA = 'bc5cc646b46f6a7c23a60e87d28dfa9b972671ec'
START = {2013:'2013-04-04', 2014:'2014-01-02', 2015:'2015-01-02',
         2016:'2016-01-04', 2017:'2017-01-03', 2018:'2018-01-02', 2019:'2019-01-02'}
CELLS = [(year, cut) for year in range(2013, 2020) for cut in (1000,3000)
         if (year,cut) != (2017,3000)]

def context(year, cut=1000):
    assert 2013 <= year <= 2019
    return DATA / f'equity_scorecard16_ctx_{year}_t{cut}_20260920/context.bin'

def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

@contextmanager
def heavy_lease(name):
    # The plan expressly permits this sole coordination write below C:/atx.
    path=DATA/'.heavy-run.lock'
    token=uuid.uuid4().hex
    record=dict(run_id='aes-w0-g0-codex-20260925',name=name,pid=os.getpid(),
                process_created=psutil.Process().create_time(),token=token,
                acquired_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
    try:
        descriptor=os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY)
    except FileExistsError as error:
        raise RuntimeError(f'Heavy slot held; refusing to overwrite: {path.read_text()}') from error
    with os.fdopen(descriptor,'w',encoding='utf-8') as stream:
        json.dump(record,stream,indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    try:
        yield
    finally:
        current=json.loads(path.read_text(encoding='utf-8'))
        if current.get('token')!=token:
            raise RuntimeError('Heavy lock ownership changed; will not remove it')
        path.unlink()

def run(name, exe, args, env=None, expected=0):
    logs = OUT / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    receipt = logs / f'{name}.receipt.json'
    if receipt.exists():
        raise RuntimeError(f'Will not overwrite receipt: {receipt}')
    argv = [str(exe), *map(str, args)]
    source_diff=subprocess.run(['git','-C',str(TREE),'diff','--',
        'atx-impl/src/equity_baseline_views.hpp','atx-impl/src/stage_equity_ic.cpp'],
        capture_output=True,check=True).stdout
    if Path(exe).parent==OUT/'bin/unpinned':
        source_sha=BASE_SHA
    elif Path(exe).parent==OUT/'bin/corrected':
        source_sha=(OUT/'bin/corrected-source-sha.txt').read_text(encoding='utf-8-sig').strip()
    else:
        source_sha=subprocess.run(['git','-C',str(TREE),'rev-parse','HEAD'],
            capture_output=True,text=True,check=True).stdout.strip()
    record = dict(name=name, argv=argv, cwd=str(TREE), source_sha=source_sha,
                  cp21_source_diff_sha256=hashlib.sha256(source_diff).hexdigest(),
                  binary_sha256=digest(exe), env=env or {}, expected_exit=expected,
                  started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
    receipt.write_text(json.dumps(record, indent=2), encoding='utf-8')
    print(f'START {name}', flush=True)
    start, peak = time.monotonic(), 0
    child_env = dict(os.environ)
    child_env.update(env or {})
    with heavy_lease(name), (logs / f'{name}.out').open('w', encoding='utf-8') as stdout, \
         (logs / f'{name}.err').open('w', encoding='utf-8') as stderr:
        child = subprocess.Popen(argv, cwd=TREE, env=child_env, stdout=stdout,
                                 stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
        process = psutil.Process(child.pid)
        while child.poll() is None:
            try:
                memory = process.memory_info()
                peak = max(peak, memory.rss, getattr(memory, 'peak_wset', 0))
            except psutil.NoSuchProcess:
                pass
            time.sleep(0.25)
        code = child.wait()
    record.update(exit_code=code, wall_seconds=round(time.monotonic()-start,3),
                  peak_working_set_bytes=peak,
                  completed_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
    receipt.write_text(json.dumps(record, indent=2), encoding='utf-8')
    print(f'END {name} exit={code} wall_s={record["wall_seconds"]} peak_gb={peak/2**30:.3f}',
          flush=True)
    if code != expected:
        print((logs / f'{name}.err').read_text(encoding='utf-8')[-3000:], flush=True)
        raise RuntimeError(f'{name}: exit {code}, expected {expected}')

def preflight():
    records=[]
    seal = 1577836800000000000
    paths=[context(year,cut) for year,cut in CELLS]
    paths.append(DATA/'tickerhistory_training_native_20260919/context.bin')
    for path in paths:
        manifest=path.with_name('context.bin.manifest.json')
        value=json.loads(manifest.read_text(encoding='utf-8'))
        sessions=list(map(int,value['axes']['session_keys']))
        if not sessions or max(sessions)>=seal:
            raise RuntimeError(f'Unsealed context: {path}')
        records.append(dict(path=str(path),artifact_id=value['artifact_id'],
                            last_session_ns=max(sessions), sha256=digest(path)))
    (OUT/'input_contexts.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    audit=OUT/'data/equity_source_reconciliation_2013_20260919/manifest.json'
    audit.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(DATA/'equity_source_reconciliation_2013_20260919/manifest.json',audit)
    assert digest(audit)=='5aa7119684701827529591aa199a860b2e83a38eff17b3d5eb1168cc057032c1'
    (OUT/'ledger').mkdir(exist_ok=True)
    print('Preflight: 14 context date axes sealed before 2020; exact payload SHA256 recorded.')

def l9():
    run('l9',BIN/'atx-impl.exe',[
        'equity-mine','--train-contexts',';'.join(str(context(y)) for y in range(2013,2017)),
        '--validation-contexts',';'.join(str(context(y)) for y in range(2016,2019)),
        '--membership',DATA/'equity_universe_pit_2013_2019_20260920/membership.bin',
        '--membership-cut',0,'--train-start','2013-01-01','--validation-start','2017-01-01',
        '--holdout-start','2019-01-01','--holdout','off','--seal','2020-01-01',
        '--fixture',TREE/'atx-impl/tests/fixtures/alpha101.txt','--population',192,
        '--generations',15,'--threads',2,'--seed',20260923,'--delay',1,'--cost-bps',5,
        '--min-names',100,'--max-validate',100,'--max-corr',0.7,'--gate','by',
        '--smooth-windows','5;10','--fdr-q',0.10,'--rw-alpha',0.10,'--n-boot',1000,
        '--max-abs-log-return',1.5,'--adj-raw-log-tol',0.10,'--max-working-bytes',5000000000,
        '--out',OUT/f'data/equity_mine_l9_guard_g0_{SHA}'])

def l10():
    run('l10',BIN/'atx-impl-tests.exe',['--gtest_filter=FundamentalZoo.RealDataIcReport'],{
        'ATX_L10_FUND_POINTS':str(DATA/'equity_fund_fields_l10v2_20260923/points.csv'),
        'ATX_L10_CONTEXTS':';'.join(str(context(y,c).parent) for y,c in CELLS if y<2019),
        'ATX_L10_SURVIVOR_CONTEXTS':';'.join(str(context(2018,c).parent) for c in (1000,3000)),
        'ATX_L10_FUNDZOO_OUT':str(OUT/f'data/equity_fund_zoo_ic_l10_g0_{SHA}')})

def l7():
    target=OUT/f'data/l7_riskmodel_scorecard_pit_2014_t1000_g0_{SHA}'
    target.mkdir(parents=True,exist_ok=False)
    run('l7',BIN/'atx-engine-bench.exe',['--benchmark_filter=BM_L7RealScorecard'],{
        'ATX_L7_REAL_DIR':str(DATA/'l7_riskmodel_raw_pit_2014_t1000_20260923'),
        'ATX_L7_OUT_DIR':str(target)})
    assert (target/'l7_scorecards.json').is_file()

def baseline(mode='frozen'):
    suffix='' if mode=='frozen' else '_'+mode
    policy=[] if mode=='frozen' else ['--replay-delisting-policy',
                                    'abort' if mode=='abort_control' else 'terminal-return']
    run('base2013'+suffix,BIN/'atx-impl.exe',[
        'equity-baseline','--panel',DATA/'tickerhistory_training_native_20260919/context.bin',
        '--out',OUT/f'data/equity_baseline_training_2013_g0_{SHA}{suffix}',
        '--evaluation-start','2013-04-04','--evaluation-end','2014-01-01',
        '--max-working-bytes',3000000000,'--report-aum',100000000,
        '--replay-execution-delay',1,'--replay-trade-bps',5,
        '--replay-annual-borrow-bps',365,'--replay-day-basis',365,*policy],
        expected=0 if mode=='corrected' else 1)

def baseline_corrected():
    baseline('corrected')

def baseline_abort_control():
    baseline('abort_control')

def cp21():
    membership=DATA/'equity_universe_pit_2013_2019_20260920/membership.bin'
    for year,cut in CELLS:
        base=OUT/f'data/equity_g0cp21_base_{year}_t{cut}_{SHA}'
        common=['--panel',context(year,cut),'--evaluation-start',START[year],
                '--evaluation-end',f'{year+1}-01-01','--max-working-bytes',3000000000,
                '--membership',membership]
        run(f'base_{year}_t{cut}',BIN/'atx-impl.exe',['equity-baseline',*common,'--out',base,
            '--min-dollar-adv',50000000,'--dollar-adv-window',21,
            '--replay-trade-bps',5,'--replay-annual-borrow-bps',365])
        run(f'ic_{year}_t{cut}',BIN/'atx-impl.exe',['equity-ic',*common,'--baseline-dir',base,
            '--out',OUT/f'data/equity_g0cp21_ic_{year}_t{cut}_{SHA}',
            '--trial-ledger',OUT/'ledger/trial-ledger-g0-cp21.jsonl'])

def scorecard():
    run('cp21_scorecard',Path(sys.executable),[
        TREE/'build-equity/audits/iteration16_equity_scorecard.py','--cells',
        *[f'{OUT}/data/equity_g0cp21_ic_{y}_t{c}_{SHA}:{y}:{c}' for y,c in CELLS],
        '--anchor',DATA/'equity_ic_training_2013_20260920',
        '--out',OUT/f'data/equity_g0cp21_scorecard_{SHA}',
        '--headline-variant','include_audited_terminal_v1','--headline-restriction','full',
        '--cp16-design',TREE/'atx-engine/reviews/2026-09-20-iteration16-alpha-scorecard-design.md',
        '--declared-n',570,'--n-note',f'G0 truth-delta W0 {SHA}: as-of membership, delay 1; no new trials',
        '--title',f'G0 cp21 W0 {SHA}; as-of membership, delay 1'])

def verify():
    scripts=OUT/'scripts'
    scripts.mkdir(exist_ok=True)
    for name in ('g0_measure.py','g0_compare.py'):
        shutil.copyfile(TREE/'.superpowers/sdd/w0'/name,scripts/name)
    shutil.copyfile(TREE/'build-equity/audits/iteration16_equity_scorecard.py',
                    scripts/'iteration16_equity_scorecard.py')
    verified=[]
    for manifest in sorted((OUT/'data').glob('*/manifest.json')):
        value=json.loads(manifest.read_text(encoding='utf-8-sig'))
        entries=value.get('files',[])
        for entry in entries:
            filename=entry.get('filename',entry.get('path'))
            path=manifest.parent/filename
            if not path.is_relative_to(OUT):
                raise RuntimeError(f'Output path escaped root: {path}')
            actual=digest(path)
            assert actual==entry['sha256'],(path,actual,entry['sha256'])
            size=entry.get('size_bytes',entry.get('bytes'))
            if size is not None:
                assert path.stat().st_size==int(size),(path,'size mismatch')
        if entries:
            assert not (manifest.parent/'.pending').exists(),manifest
        verified.append(dict(manifest=str(manifest.relative_to(OUT)),
                             manifest_sha256=digest(manifest),bound_files_verified=len(entries)))
    artifact=OUT/'g0-artifact-manifest.json'
    files=[dict(path=str(path.relative_to(OUT)),size_bytes=path.stat().st_size,sha256=digest(path))
           for path in sorted(OUT.rglob('*')) if path.is_file() and path!=artifact]
    record=dict(schema='atx.g0-truth-delta-evidence/v1',frozen_source_sha=BASE_SHA,
                created_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
                verified_engine_manifests=verified,files=files,
                caveats=['same prebuilt contexts; D0 panel construction changes not measured',
                         'cp21 frozen scorecard metadata still labels membership year-union',
                         'historical peak RAM missing for L10 and L7',
                         'baseline failure retained and explicitly classified in report'])
    artifact.write_text(json.dumps(record,indent=2),encoding='utf-8')
    print('Verified',len(verified),'engine manifests;',len(files),'evidence files; published manifest last.')

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('phase',choices=['preflight','l9','l10','l7','baseline',
        'baseline_corrected','baseline_abort_control','cp21','scorecard','verify'])
    phase=parser.parse_args().phase
    frozen=OUT/'bin/unpinned'
    if phase in ('baseline_corrected','baseline_abort_control'):
        BIN=OUT/'bin/corrected'
    elif phase not in ('cp21','scorecard','preflight','verify') and frozen.exists():
        BIN=frozen
    globals()[phase]()
