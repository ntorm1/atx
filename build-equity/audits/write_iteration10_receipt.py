"""Freeze explicit-intent measurement without rewriting earlier checkpoints."""
from pathlib import Path
import hashlib
import json
import xml.etree.ElementTree as ET

ROOT = Path('C:/atx/.worktrees/equity-platform')
AUDITS = ROOT / 'build-equity/audits'
NATIVE = Path('C:/atx/data/equity_book_training_2013_intents_20260920')
DESTINATION = ROOT / 'atx-engine/reviews/2026-09-20-execution-representation-validation.json'
PRIOR = ROOT / 'atx-engine/reviews/2026-09-20-qp-polish-validation.json'
PRIOR_SHA = 'f01564db748ad5e64a1f58a1dd9bf9a98471fd369cf107d0b8def669783e7fc1'


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def pin(path):
    path = Path(path)
    if not path.is_absolute():
        path = ROOT / path
    payload = path.read_bytes()
    return {'path': str(path), 'sha256': hashlib.sha256(payload).hexdigest(),
            'bytes': len(payload)}


def main():
    assert not DESTINATION.exists(), 'checkpoint receipt is immutable'
    assert pin(PRIOR)['sha256'] == PRIOR_SHA
    tests = ET.parse(ROOT / 'build-equity/iteration10-tests.xml').getroot()
    cases = {case.attrib['name']: ('passed' if case.find('failure') is None and
             case.find('skipped') is None else 'not-passed')
             for case in tests.findall('testcase')}
    assert len(cases) == 52 and all(value == 'passed' for value in cases.values())
    measurement = read(AUDITS / 'iteration10-equity-book-measurement.json')
    verification = read(AUDITS / 'iteration10-allocation-proposal-verification.json')
    boundary = read(AUDITS / 'iteration10-observed-boundary-notes.json')
    comparison = read(AUDITS / 'iteration10-startup-native-comparison.json')
    assert measurement['status'] == 'failed' and measurement['exit_code'] == 1
    assert measurement['pin_recheck']['all_unchanged']
    assert verification['verification_status'] == 'verified-partial-allocation-evidence'
    assert verification['verified_proposal_count'] == 1
    assert verification['observed_complete_intervals'] == 6
    assert not verification['native_failure_independently_reproduced']
    assert boundary['boundary']['second_candidate_raw_weights_available'] is False
    assert comparison['status'] == 'passed-first-startup-intent-comparison'
    assert comparison['all_pins_unchanged']
    source_paths = [Path(name) for name in measurement['pin_recheck']['source_pins']]
    for path in source_paths:
        assert pin(path)['sha256'] == measurement['pin_recheck']['source_pins'][str(path)]['after']['sha256']
    preflight = measurement['preflight']
    assert pin(preflight['executable']['path'])['sha256'] == preflight['executable']['sha256']
    certs = read(NATIVE / 'allocation_certificates.json')
    assert len(certs['decisions']) == 1
    first = certs['decisions'][0]
    assert sum(w != 0 for w in first['proposed_weights_canonical_order']) == 381
    assert first['representation_l1_change'] <= first['representation_l1_budget']
    old = read(Path('C:/atx/data/equity_book_training_2013_qpfix_20260920') /
               'allocation_certificates.json')['decisions'][0]
    assert old['proposed_weights_canonical_order'] == first['continuous_weights_canonical_order']
    assert not (NATIVE / 'manifest.json').exists()
    assert not (NATIVE / 'report/manifest.json').exists()
    document_paths = ['atx-engine/docs/PLATFORM_PROGRESS.md', 'atx-engine/README.md',
                      'atx-impl/README.md', 'atx-impl/docs/EQUITY_BOOK_CONSTRAINED.md',
                      'atx-engine/reviews/2026-09-20-allocation-representation-review.md',
                      'atx-engine/reviews/2026-09-20-equity-execution-representation-design.md']
    test_paths = ['atx-engine/tests/book/book_replay_test.cpp',
                  'atx-impl/tests/equity_allocation_test.cpp',
                  'atx-impl/tests/replay_report_test.cpp',
                  'atx-impl/tests/stage_equity_baseline_test.cpp']
    evidence = sorted(set([p for p in (ROOT / 'build-equity').glob('iteration10*') if p.is_file()] +
                          [p for p in AUDITS.glob('iteration10*') if p.is_file()] +
                          [Path(__file__)]))
    receipt = {
        'date': '2026-09-20', 'checkpoint': 10, 'goal_status': 'active',
        'scope': 'Explicit hold/close/weight intents, bounded representation, complete economic recertification and original-window trial',
        'status': 'bounded_software_validation_complete_native_trial_recorded',
        'workspace': {'path': str(ROOT), 'branch': 'feat/equity-platform-20260920'},
        'prior_checkpoint': pin(PRIOR),
        'predeclared_plan': read(AUDITS / 'iteration10-representation-plan.json'),
        'tests': {'passed': 52, 'failed': 0, 'skipped': 0, 'new_cases': 7,
                  'wall_seconds': 26.70, 'cases': cases,
                  'qualification': 'Affected replay/allocation/report/stage contracts only. Prior dense-PCG timeout remains incomplete; full repository, sanitizers and throughput unqualified.'},
        'build': {'compiler': 'VS2022 clang-cl 18.1.8', 'preset': 'equity-dev',
                  'targets': ['atx-impl', 'atx-impl-tests', 'atx-engine-book-tests'],
                  'jobs': 4, 'cache_disabled': True, 'wrapper': 'scripts/atx-build.ps1',
                  'translation_units_checked': ['atx-engine/src/book/replay.cpp',
                      'atx-impl/src/equity_allocation.cpp', 'atx-impl/src/replay_report.cpp',
                      'atx-impl/src/stage_equity_book.cpp'],
                  'initial_environment_failure': 'Fourth sequential wrapper invocation failed vcvars64.bat exit 255 before compilation. Fresh PowerShell invocation completed stage check/build/tests; no source correction needed for this failure.'},
        'implementation': [
            'Shared checked scalar sizing supports TargetWeight, exact HoldCurrent and full Close.',
            'Existing fixed targets and weight callbacks retain their API and sizing order.',
            'Opt-in MachinePrecisionIntentsV1 uses a frozen causal per-name eta and aggregate ordinary correction budget.',
            'Mandatory exits and held mark validation precede ordinary representation; unused flat Close marks are ignored.',
            'Actual representable units, dollars, cash, fees, NAV and original exposure/turnover constraints are recertified.',
            'Raw continuous weights and QP scope are distinct from instructions, resolved weights, actual exposures and represented objective.',
            'Identified report provenance and CSV bind instruction semantics; incomplete replay publishes no accepted performance.',
            'No economic no-trade band, share rounding, price repair, future missingness selection or threshold retry.'
        ],
        'reviews': {'engine_intent_path': 'independent helper-owner review passed',
                    'helper_report_stage': 'independent replay-owner review passed',
                    'scope_notes': [
                        'Existing helper accounting reconciliation tolerance is looser than replay; replay retains the stricter final gate.',
                        'Pure representation boundary reports solver_used=false and does not certify QP optimality.'
                    ]},
        'native_measurement': measurement,
        'independent_proposal_verification': verification,
        'observed_boundary_notes': boundary,
        'independent_startup_comparison': comparison,
        'startup_continuous_weights_identical_to_prior_attempt': True,
        'native_evidence': [pin(p) for p in sorted(NATIVE.rglob('*')) if p.is_file()],
        'investment_evidence': {'complete_replay': False, 'recorded_certified_proposals': 1,
            'accepted_allocations_from_completed_report': 0,
            'observed_complete_intervals_verified_without_performance_publication': 6,
            'sharpe': None, 'capacity': None, 'live_orders_authorized': False,
            'qualification': 'No investment acceptance; source economics, publication, instrument eligibility, borrow and executable capacity remain unverified.'},
        'next_iteration': 'Review explicit execution-price availability for unheld entries and retain rejected-candidate diagnostics. Held missing marks remain failures; no conditional-entry policy was applied to this attempt.',
        'sources': [pin(p) for p in sorted(set(source_paths + [ROOT / p for p in test_paths]))],
        'documents': [pin(p) for p in document_paths],
        'evidence': [pin(p) for p in evidence],
        'references': ['https://www.cvxportfolio.com/en/1.5.0/simulator.html',
                       'https://docs.mosek.com/portfolio-cookbook/transaction.html',
                       'https://web.stanford.edu/~boyd/papers/pdf/cvx_portfolio.pdf',
                       'https://www.cvxportfolio.com/en/stable/simple_policies.html#cvxportfolio.Hold']
    }
    with DESTINATION.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(receipt, stream, sort_keys=True, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps(pin(DESTINATION)))


if __name__ == '__main__':
    main()
