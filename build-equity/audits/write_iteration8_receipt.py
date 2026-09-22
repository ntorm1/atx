"""Freeze the first constrained-book checkpoint before later solver work."""
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path('C:/atx/.worktrees/equity-platform')
AUDITS = ROOT / 'build-equity/audits'
NATIVE = Path('C:/atx/data/equity_book_training_2013_20260920')
DESTINATION = ROOT / 'atx-engine/reviews/2026-09-20-constrained-book-validation.json'


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def pin(path):
    path = Path(path)
    if not path.is_absolute():
        path = ROOT / path
    data = path.read_bytes()
    return {'path': str(path), 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}


def tests(path):
    root = ET.parse(path).getroot()
    return {case.attrib['name']: case.find('failure') is None and case.find('skipped') is None
            for case in root.findall('testcase')}


initial = tests(ROOT / 'build-equity/iteration8-tests.xml')
last = tests(ROOT / 'build-equity/iteration8-tolerance-tests.xml')
assert len(initial) == 177 and sum(initial.values()) == 175
assert len(last) == 8 and all(last.values())
latest = dict(initial)
latest.update(last)
assert len(latest) == 177 and all(latest.values())
measurement = read(AUDITS / 'iteration8-equity-book-measurement.json')
verification = read(AUDITS / 'iteration8-allocation-proposal-verification.json')
assert measurement['exit_code'] == 1 and measurement['pin_recheck']['all_unchanged']
assert verification['verification_status'] == 'no-allocation-evidence'
assert verification['proposal_count'] == 0
assert not (NATIVE / 'manifest.json').exists() and not (NATIVE / 'report/manifest.json').exists()
certificates = read(NATIVE / 'allocation_certificates.json')
assert certificates['decisions'] == []
test_sources = ['atx-engine/tests/book/book_replay_test.cpp',
    'atx-engine/tests/risk/risk_kkt_ldl_test.cpp', 'atx-impl/tests/equity_allocation_test.cpp',
    'atx-impl/tests/config_equity_book_test.cpp', 'atx-impl/tests/replay_policy_stage_test.cpp',
    'atx-impl/tests/replay_report_test.cpp', 'atx-impl/tests/stage_equity_baseline_test.cpp']
sources = [Path(p) for p in measurement['pin_recheck']['source_pins']]
for path in sources:
    assert pin(path)['sha256'] == measurement['pin_recheck']['source_pins'][str(path)]['after']['sha256']
evidence = sorted(set([p for p in (ROOT / 'build-equity').glob('iteration8*') if p.is_file()] +
    [p for p in AUDITS.glob('iteration8*') if p.is_file()] + [Path(__file__)]))
receipt = {
    'date': '2026-09-20', 'goal_status': 'active', 'checkpoint': 8,
    'scope': 'Marked-holdings policy replay, constrained preference implementation and bounded factor workspace',
    'status': 'software_cases_pass_native_numerical_feasibility_failed_before_first_allocation',
    'workspace': {'path': str(ROOT), 'branch': 'feat/equity-platform-20260920',
        'base_commit': 'dffb609b7a3ccf874b88a07ae407a16961140c84',
        'recovery': read(AUDITS / 'iteration8-worktree-recovery.json')},
    'tests': {'unique_latest_passed': 177, 'unique_latest_failed': 0, 'new_cases': 24,
        'initial': {'passed': 175, 'failed': 2, 'wall_seconds': 70.67},
        'diagnostic_reproduction': {'passed': 0, 'failed': 2, 'wall_seconds': 0.66},
        'affected_final_recheck': {'passed': 8, 'failed': 0, 'wall_seconds': 4.98},
        'scope': 'Book replay, selected QP/constraint/factorization cases, identified pipeline/report and allocation/stage/config cases.',
        'not_run': 'Full repository suite, sanitizer or throughput benchmark.',
        'latest_cases': latest},
    'build': {'compiler': 'VS2022 clang-cl 18.1.8', 'preset': 'equity-dev',
        'jobs': 4, 'targets': ['atx-impl-tests', 'atx-impl', 'atx-engine-book-tests', 'atx-engine-risk-tests'],
        'wrapper': 'scripts/atx-build.ps1', 'cache_disabled_for_final_build': True,
        'initial_failures': ['New worktree Databento submodule needed initialization.',
            'One JSON/string_view comparison overload needed explicit string extraction.',
            'Cross-worktree cached PCH could not load; regenerated only owned PCH outputs with CCACHE_DISABLE=1.']},
    'implementation': ['Preserved fixed replay accounting path and added marked-holdings allocation callback/owned records.',
        'Policy report binds actual allocation dollars, validated canonical recipe and parents; bounded wrapper checks every captured artifact ID.',
        '63 adjacent lagged returns, fixed diagonal risk/preference objective, net/gross/name/full-L1 constraints and independent post-fee certification.',
        'Conservative admission plus 256MiB actual-symbolic factor payload cap on both ADMM and polish.',
        'Original complete training window and weekly schedule enforced; observed source fields and semantic axes checked exactly.',
        'Failed attempts retain proposal evidence; complete report and book manifests publish last.'],
    'numerical_findings': ['Initial two-name polished candidate had actual L1 0.20000001199999984: individual row error about4e-9 accumulated past economic tolerance1e-8.',
        'Derived internal row tolerance accounts for aggregate error and fee amplification; fixed1200 iterations and economic limits unchanged.',
        'No ordinary weight clipping, rescaling, reduced universe, window shortening or looser economic tolerance introduced.',
        'Native first allocation still failed tighter feasibility at row4115; error rounded sub-micro violation to0.000000. Numerical convergence/economic feasibility not established.',
        'Next review targets unregularized polish residuals, current primal/dual certification and feasible-candidate acceptance.'],
    'native_measurement': measurement,
    'independent_verification': verification,
    'native_evidence': [pin(p) for p in sorted(NATIVE.rglob('*')) if p.is_file()],
    'investment_evidence': {'complete_replay': False, 'accepted_allocations': 0,
        'sharpe': None, 'capacity': None, 'live_orders_authorized': False,
        'source_quality': '104 original required rows absent and6 OHLC-order quarantines remain unresolved from checkpoint7.'},
    'prior_receipts': [pin('atx-engine/reviews/2026-09-19-equity-baseline-validation.json'),
        pin('atx-engine/reviews/2026-09-20-source-reconciliation-validation.json')],
    'sources': [pin(p) for p in sorted(set(sources + [ROOT / p for p in test_sources]))],
    'documents': [pin(p) for p in ['atx-engine/docs/PLATFORM_PROGRESS.md', 'atx-engine/README.md',
        'atx-impl/README.md', 'atx-impl/docs/EQUITY_BOOK_CONSTRAINED.md',
        'atx-engine/reviews/2026-09-20-terminal-cash-event-design.md']],
    'evidence': [pin(p) for p in evidence],
}
with DESTINATION.open('x', encoding='utf-8') as stream:
    json.dump(receipt, stream, indent=2, sort_keys=True)
    stream.write('\n')
print(json.dumps(pin(DESTINATION)))
