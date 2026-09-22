"""Re-evaluate the attempt-1 equity-ic measurement's acceptance without re-running.

Attempt 1 (iteration14-equity-ic-measurement.json) reported accepted=false solely because
the runner's pins_unchanged predicate compared the recorded ``path`` strings, which the
runner wrote absolute before the run and relative after it (runner defect, fixed in
iteration14_run_equity_ic.py at the re-pin site). Every pin's SHA-256 and byte count is
identical before and after. A re-run would append two more lines to the tracked trial
ledger and declare 30 further trials for no new information, so the acceptance is
re-derived here from the recorded digests only. The attempt-1 file is left untouched.
"""
from pathlib import Path
import hashlib
import json

ROOT = Path('C:/atx/.worktrees/equity-platform')
SOURCE = ROOT / 'build-equity/audits/iteration14-equity-ic-measurement.json'
OUT = ROOT / 'build-equity/audits/iteration14-equity-ic-measurement-attempt1-reeval.json'


def digest_view(pins):
    return {key: (entry['sha256'], entry['bytes']) for key, entry in pins.items()}


def main():
    assert not OUT.exists(), 'immutable re-evaluation already exists'
    report = json.loads(SOURCE.read_text(encoding='utf-8'))
    before = report['source_and_input_pins_before']
    after = report['source_and_input_pins_after']
    path_only = [key for key in before
                 if before[key]['sha256'] == after[key]['sha256']
                 and before[key]['bytes'] == after[key]['bytes']
                 and before[key]['path'] != after[key]['path']]
    digests_unchanged = digest_view(before) == digest_view(after)
    reeval = {
        'schema': 'atx-equity-ic-measurement-reevaluation-v1',
        'source_measurement': {'path': str(SOURCE),
                               'sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest()},
        'defect': 'runner compared recorded path strings (absolute before, relative after); '
                  'digests and byte counts were identical for every pin',
        'pins_total': len(before),
        'pins_path_only_differences': path_only,
        'pins_digest_unchanged': digests_unchanged,
        'exit_code': report['exit_code'],
        'ledger_two_lines_per_run': report['ledger_two_lines_per_run'],
        'output_missing': report['output_evidence']['missing'],
        'budget_breach': report.get('budget_breach'),
        'accepted_reevaluated': bool(report['exit_code'] == 0 and digests_unchanged
                                     and report['ledger_two_lines_per_run']
                                     and not report['output_evidence']['missing']
                                     and report.get('budget_breach') is None),
        'no_rerun_reason': 'a re-run would append two ledger lines and declare 30 further '
                           'trials for no new information; the attempt-1 run itself is valid',
    }
    OUT.write_text(json.dumps(reeval, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: reeval[k] for k in ('pins_total', 'pins_digest_unchanged',
                                              'accepted_reevaluated')}
                     | {'path_only': len(path_only), 'out': str(OUT)}))


if __name__ == '__main__':
    main()
