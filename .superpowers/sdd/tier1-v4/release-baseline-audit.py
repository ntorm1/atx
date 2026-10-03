"""Read-only, streaming artifact audit; no prices, labels or returns are decoded."""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path('C:/atx')
LAKE = ROOT / 'atx-db/data/alpha_panel/v1'
WORK = ROOT / '.superpowers/sdd/tier1-v4'
STAGES = ('universe', 'compustat_std', 'ratios', 'fundamentals_v10',
          'fundamentals_v11', 'fundamentals_notes', 'notes', 'earnings_calendar',
          'earnings_calendar_v2', 'ea_actuals', 'factors', 'risk_inputs',
          'char_monthly', 'characteristics', 'gold', 'golden_panel')


def sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def audit_stage(stage: str) -> dict:
    directory = LAKE / stage
    manifest = directory / 'manifest.json'
    if not manifest.exists():
        return {'manifest_present': False, 'directory_present': directory.exists(),
                'physical_integrity': 'NOT_MEASURED', 'release_qualification': 'NOT_ESTABLISHED'}
    before = sha(manifest)
    data = json.loads(manifest.read_text(encoding='utf-8'))
    files = data.get('files', {})
    results = []
    if isinstance(files, dict):
        for name, expected in sorted(files.items()):
            if not isinstance(expected, dict):
                results.append({'path': name, 'status': 'UNSUPPORTED_ENTRY'})
                continue
            path = (directory / name).resolve()
            if not path.is_relative_to(directory.resolve()):
                results.append({'path': name, 'status': 'OUTSIDE_STAGE'})
                continue
            if not path.is_file():
                results.append({'path': name, 'status': 'MISSING'})
                continue
            stat_before = path.stat()
            actual = sha(path)
            stat_after = path.stat()
            expected_size = expected.get('bytes')
            match = (actual == expected.get('sha256') and stat_after.st_size == expected_size
                     and (stat_before.st_size, stat_before.st_mtime_ns)
                     == (stat_after.st_size, stat_after.st_mtime_ns))
            results.append({'path': name, 'bytes': stat_after.st_size, 'sha256': actual,
                            'status': 'MATCH' if match else 'MISMATCH'})
    drift = []
    for name, expected in data.get('code', {}).items():
        if not isinstance(expected, dict) or not name.endswith('.py'):
            continue
        source = ROOT / 'atx-db/src/atx_db/alpha_panel' / name
        if source.is_file():
            raw = source.read_bytes()
            current = hashlib.sha256(raw).hexdigest()
            lf = hashlib.sha256(raw.replace(b'\r\n', b'\n')).hexdigest()
            if current != expected.get('sha256') and lf != expected.get('sha256_lf'):
                drift.append({'path': str(source.relative_to(ROOT)), 'sha256': current})
    after = sha(manifest)
    status = 'PASS' if results and all(x['status'] == 'MATCH' for x in results) and before == after else 'FAIL'
    return {'manifest_present': True, 'manifest_sha256': before, 'manifest_unchanged': before == after,
            'physical_integrity': status, 'files_verified': len(results), 'files': results,
            'current_code_drift': drift, 'recorded_gate_status': data.get('gate_status'),
            'recorded_input_manifests_sha256': data.get('input_manifests_sha256'),
            'release_qualification': 'NOT_ESTABLISHED'}


def main() -> None:
    evidence = {'schema': 'atx.tier1-v4.release-baseline/v1',
                'generated_at_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
                'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                'audit_kind': 'bytes_and_manifest_metadata_only',
                'return_data_decoded': False,
                'scope': 'Snapshot integrity is separate from acceptance, source validity and current-code reproducibility.',
                'stages': {name: audit_stage(name) for name in STAGES}}
    progress = LAKE / 'panel_goldready/refresh_progress.json'
    if progress.exists():
        obj = json.loads(progress.read_text(encoding='utf-8'))
        months = obj.get('months', {})
        evidence['foreign_silver_progress'] = {'path': str(progress.relative_to(ROOT)),
            'sha256': sha(progress), 'months_recorded': len(months),
            'first_month': min(months, default=None), 'last_month': max(months, default=None),
            'published_manifest_present': (progress.parent / 'manifest.json').exists(),
            'status': 'FOREIGN_OWNED_NOT_REBUILT_OR_QUALIFIED'}
    destination = WORK / 'release-baseline-v1.json'
    if destination.exists():
        raise SystemExit('Immutable baseline exists; choose a new evidence version.')
    destination.write_text(json.dumps(evidence, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print(json.dumps({'artifact': str(destination), 'sha256': sha(destination),
        'stages': {name: {'physical_integrity': row['physical_integrity'],
                          'files_verified': row.get('files_verified', 0),
                          'code_drift': len(row.get('current_code_drift', []))}
                   for name, row in evidence['stages'].items()},
        'foreign_silver_progress': evidence.get('foreign_silver_progress')}))


if __name__ == '__main__':
    main()
