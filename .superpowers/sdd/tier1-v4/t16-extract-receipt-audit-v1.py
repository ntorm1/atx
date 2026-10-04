"""Controller audit of one completed guarded native source-validation receipt."""
import argparse
import ast
import hashlib
import json
import os
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--batch', type=int, required=True)
parser.add_argument('--run', required=True)
args = parser.parse_args()
if not os.environ.get('ATX_GUARD_JOB') or args.batch not in range(85):
    raise RuntimeError('External guard and required batch needed')
workspace = Path(__file__).resolve().parent
if not re.fullmatch(rf't16-extract-r19-b{args.batch:04d}-v[1-9][0-9]*', args.run):
    raise RuntimeError('Unexpected original run name')
out = workspace / f'logs/t16-extract-r19-b{args.batch:04d}-admission-v1.json'
if out.exists():
    raise RuntimeError('Preserve previous independent admission')

def read(path):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise RuntimeError('Duplicate JSON key')
            result[key] = value
        return result
    return json.loads(Path(path).read_text(encoding='utf-8-sig'), object_pairs_hook=pairs)

def pin(path):
    path = Path(path)
    if not path.is_absolute() or not path.is_file():
        raise RuntimeError('Expected an existing absolute file')
    digest = hashlib.sha256()
    before = path.stat()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError('File changed during hash')
    return {'sha256': digest.hexdigest(), 'bytes': after.st_size}

binding_path = workspace / 'task16-fund-reader-binding-v1.json'
binding_pin = pin(binding_path)
if binding_pin['sha256'] != '8c63e1ba186a81c70c9f620ebd14cb9a4e4cbdd36b9faf44e5154dd5546894b3':
    raise RuntimeError('Independent binding pin changed')
binding = read(binding_path)
directory = Path('C:/atx/atx-db/data/alpha_panel/v1/fund_extract_validation_v1/r19_20261004_001')
receipt_path = directory / f'batch-{args.batch:04d}.json'
guard_path = workspace / f'logs/{args.run}.receipt.json'
stdout_path = workspace / f'logs/{args.run}.out.log'
stderr_path = workspace / f'logs/{args.run}.err.log'
guard_pin, stdout_pin, stderr_pin = pin(guard_path), pin(stdout_path), pin(stderr_path)
guard = read(guard_path)
expected_command = ['.venv/Scripts/python.exe', '-m', 'atx_db.alpha_panel.fund_batch_release',
                    'validate-extract', '--bindings', binding_path.as_posix(), '--batch', str(args.batch),
                    '--receipt', receipt_path.as_posix()]
if guard['command'] != expected_command or guard['status'] != 'completed' or guard['returncode'] != 0 or guard['cap_hit']:
    raise RuntimeError('No exact successful guarded native execution')
if guard['job_limit_gb'] > 1 or guard['native_peak_job_memory_bytes'] > guard['job_cap_bytes']:
    raise RuntimeError('Guard resource bound not proved')
if stderr_pin['bytes']:
    raise RuntimeError('Native stderr requires independent review')
stdout = ast.literal_eval(stdout_path.read_text().strip())
receipt_pin = pin(receipt_path)
if stdout != {'status': 'one_batch_validated', 'batch': args.batch, 'receipt': str(receipt_path),
              'identity': receipt_pin, 'whole_source_admitted': False}:
    raise RuntimeError('Native execution output does not bind actual receipt')
receipt = read(receipt_path)
if set(receipt) != {'version', 'batch', 'source', 'validation'} or receipt['version'] != 'fund-extract-validation-v1' or type(receipt['batch']) is not int or receipt['batch'] != args.batch:
    raise RuntimeError('Unexpected receipt schema/key')
source = receipt['source']
if source['archive'] != {'path': binding['archive']['path'], 'sha256': binding['archive']['sha256'], 'bytes': binding['archive']['bytes']}:
    raise RuntimeError('Archive binding differs')
extract = Path(binding['extract_root'])
if source['extract_root'] != str(extract):
    raise RuntimeError('Wrong source extract root')
actual_pins = {source['archive']['path']: pin(source['archive']['path']), str(extract / 'plan.json'): pin(extract / 'plan.json')}
if actual_pins[source['archive']['path']] != {key: source['archive'][key] for key in ('sha256', 'bytes')} or actual_pins[str(extract / 'plan.json')] != source['plan']:
    raise RuntimeError('Current archive/plan bytes differ from native validation')
expected_files = {f'batch-{args.batch:04d}' + suffix for suffix in ('.parquet', '.stats.parquet', '.json')}
if set(source['files']) != expected_files or len(source['members']) != 85:
    raise RuntimeError('Abbreviated extraction source identity')
for name, expected in source['files'].items():
    path = extract / name
    actual_pins[str(path)] = pin(path)
    if actual_pins[str(path)] != expected:
        raise RuntimeError('Current batch bytes differ from native validation')
if not source['code'] or not any(Path(path).name == 'fund_batch_release.py' for path in source['code']):
    raise RuntimeError('No native adapter code identity')
for path, expected in source['code'].items():
    actual_pins[path] = pin(path)
    if actual_pins[path] != expected:
        raise RuntimeError('Current code/contract drift')
contract = read(Path('C:/atx/atx-db/docs/datasets/fundamentals_release_v1.contract.json'))
if source['contract'] != contract:
    raise RuntimeError('Frozen contract differs')
native = read(extract / f'batch-{args.batch:04d}.json')
for field in ('member_rows', 'counts', 'members', 'rows', 'first_cik', 'last_cik', 'uncompressed_bytes'):
    if receipt['validation'][field] != native[field]:
        raise RuntimeError('Native diagnostic population changed')
for path, expected in actual_pins.items():
    if pin(path) != expected:
        raise RuntimeError('Before/after independent byte check differs')
if pin(receipt_path) != receipt_pin or pin(guard_path) != guard_pin or pin(stdout_path) != stdout_pin or pin(stderr_path) != stderr_pin or pin(binding_path) != binding_pin:
    raise RuntimeError('Execution evidence changed during independent admission')
result = {'status': 'ONE_GUARDED_SOURCE_REPLAY_INDEPENDENTLY_PINNED', 'batch': args.batch,
          'native_receipt': {'path': str(receipt_path), 'identity': receipt_pin},
          'guard_receipt': {'path': str(guard_path), 'identity': guard_pin}, 'stdout': stdout_pin,
          'binding': binding_pin, 'current_actual_byte_pins': actual_pins,
          'rows': native['rows'], 'members': native['members'],
          'native_run_seconds': guard['run_seconds'], 'native_peak_bytes': guard['native_peak_job_memory_bytes'],
          'qualification': 'Only one exact guarded native source replay admitted. Full85 extraction inventory, financial units, source economics and production gates remain open.'}
with out.open('x', encoding='utf-8') as stream:
    json.dump(result, stream, indent=2)
    stream.write('\n')
print(json.dumps({key: result[key] for key in ('status', 'batch', 'native_receipt', 'rows', 'members', 'native_run_seconds', 'native_peak_bytes')}), flush=True)
