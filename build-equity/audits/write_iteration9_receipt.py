"""Freeze the numerical repair checkpoint without rewriting earlier evidence."""
from pathlib import Path
import hashlib
import json
import xml.etree.ElementTree as ET

ROOT = Path('C:/atx/.worktrees/equity-platform')
AUDITS = ROOT / 'build-equity/audits'
NATIVE = Path('C:/atx/data/equity_book_training_2013_qpfix_20260920')
DESTINATION = ROOT / 'atx-engine/reviews/2026-09-20-qp-polish-validation.json'
PRIOR = ROOT / 'atx-engine/reviews/2026-09-20-constrained-book-validation.json'
PRIOR_SHA = 'c417ffdfa6594b94f24712004060a47f848cc0250e17b0f5b5339c03ebc2c5b1'


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def pin(path):
    path = Path(path)
    if not path.is_absolute():
        path = ROOT / path
    payload = path.read_bytes()
    return {'path': str(path), 'sha256': hashlib.sha256(payload).hexdigest(),
            'bytes': len(payload)}


def cases(path):
    root = ET.parse(path).getroot()
    return {case.attrib['name']: 'passed' if case.find('failure') is None and
            case.find('skipped') is None else
            (case.find('failure').attrib.get('message', 'failed')
             if case.find('failure') is not None else 'skipped')
            for case in root.findall('testcase')}


def main():
    assert not DESTINATION.exists(), 'checkpoint receipt is immutable'
    assert pin(PRIOR)['sha256'] == PRIOR_SHA
    initial = cases(ROOT / 'build-equity/iteration9-tests.xml')
    final = cases(ROOT / 'build-equity/iteration9-cone-contract-tests.xml')
    assert len(initial) == 182 and list(initial.values()).count('passed') == 179
    assert final and all(value == 'passed' for value in final.values())
    retired = ['RiskCone.ZeroConeIsByteIdenticalToS84Path',
               'RiskCone.ZeroSectorConeZeroImpactIsByteIdenticalToS85aPath']
    latest = dict(initial)
    for name in retired:
        assert latest.pop(name) == 'Failed'
    latest.update(final)
    incomplete = {key: value for key, value in latest.items() if value != 'passed'}
    assert incomplete == {'RiskQpAugment.MatchesDenseOracleAcrossBattery': 'Timeout'}
    assert sum(value == 'passed' for value in latest.values()) == 181
    measurement = read(AUDITS / 'iteration9-equity-book-measurement.json')
    verification = read(AUDITS / 'iteration9-allocation-proposal-verification.json')
    assert verification['verification_status'] == 'verified-partial-allocation-evidence'
    assert verification['native_failure_independently_reproduced']
    assert measurement['pin_recheck']['all_unchanged']
    source_paths = [Path(name) for name in measurement['pin_recheck']['source_pins']]
    for path in source_paths:
        assert pin(path)['sha256'] == measurement['pin_recheck']['source_pins'][str(path)]['after']['sha256']
    certs = read(NATIVE / 'allocation_certificates.json')
    complete = (NATIVE / 'manifest.json').is_file()
    report_complete = (NATIVE / 'report/manifest.json').is_file()
    assert complete == report_complete
    oracle = read(AUDITS / 'iteration9-startup-oracle.json')
    evidence = sorted(set([p for p in (ROOT / 'build-equity').glob('iteration9*') if p.is_file()] +
                         [p for p in AUDITS.glob('iteration9*') if p.is_file()] +
                         [Path(__file__)]))
    comparison = read(AUDITS / 'iteration9-startup-native-comparison.json')
    assert comparison['status'] == 'passed-first-proposal-comparison'
    assert comparison['all_pins_unchanged']
    document_paths = ['atx-engine/docs/PLATFORM_PROGRESS.md', 'atx-engine/README.md',
                      'atx-impl/README.md', 'atx-impl/docs/EQUITY_BOOK_CONSTRAINED.md',
                      'atx-engine/reviews/2026-09-20-qp-polish-review.md',
                      'atx-engine/reviews/2026-09-20-cone-determinism-migration.md',
                      'atx-engine/reviews/2026-09-20-iteration10-terminal-cash-slice.md']
    test_paths = ['atx-engine/tests/risk/risk_qp_polish_test.cpp',
                  'atx-engine/tests/risk_cone_test.cpp',
                  'atx-engine/tests/risk_qp_augment_test.cpp']
    receipt = {
        'date': '2026-09-20', 'checkpoint': 9, 'goal_status': 'active',
        'scope': 'Unregularized QP polish refinement, coherent primal/dual acceptance, same-window constrained book trial',
        'status': 'bounded_software_validation_complete_native_trial_recorded',
        'workspace': {'path': str(ROOT), 'branch': 'feat/equity-platform-20260920'},
        'prior_checkpoint': pin(PRIOR),
        'predeclared_plan': read(AUDITS / 'iteration9-numerical-repair-plan.json'),
        'validation_scope_amendment': read(AUDITS / 'iteration9-validation-scope-review.json'),
        'tests': {'latest_passed': 181, 'latest_failed_assertions': 0,
                  'incomplete_timeout_cases': incomplete, 'new_cases': 3,
                  'initial_passed': 179, 'initial_failed_digest_assertions': 2,
                  'initial_timeout_cases': 1, 'initial_wall_seconds': 160.61,
                  'first_migrated_checks': {'passed': 16, 'failed': 2, 'wall_seconds': 1.91,
                     'cause': 'New 1e-10 assertion was stricter than this legacy fixture caller contract; production solver controls unchanged.'},
                  'retired_historical_digest_cases': retired, 'replacement_and_recheck_cases': final,
                  'latest_cases': latest,
                  'qualification': 'Selected affected regressions only; dense reference battery, full repository, sanitizers and throughput are unqualified.'},
        'build': {'compiler': 'VS2022 clang-cl 18.1.8', 'preset': 'equity-dev',
                  'targets': ['atx-impl', 'atx-impl-tests', 'atx-engine-risk-tests'],
                  'jobs': 4, 'cache_disabled': True, 'wrapper': 'scripts/atx-build.ps1'},
        'implementation': [
            'Refine the unregularized active KKT system through the existing capped regularized factor.',
            'Seed correction from ADMM primal and selected duals to retain useful free epigraph coordinates.',
            'Publish coherent primal/dual pairs after finite, normal-cone, linear/conic feasibility and residual checks.',
            'Apply incumbent objective comparison only when the incumbent is itself feasible.',
            'Preserve ordinary ADMM fallback, mandatory factor-memory rejection and fixed four polish solves.',
            'Replace historical output digests with independent dense KKT optimality and direct inert-feature byte equivalence.',
            'Preserve precise feasibility diagnostics and original financial/data controls.'
        ],
        'native_measurement': measurement,
        'independent_proposal_verification': verification,
        'independent_startup_comparison': comparison,
        'representation_diagnostic': read(AUDITS / 'iteration9-support-diagnostic.json'),
        'next_iteration': 'Review a declared causal zero/no-trade representation rule with full portfolio recertification. No saved proposal threshold or source mark repair was applied.',
        'startup_oracle': {
            'artifact': pin(AUDITS / 'iteration9-startup-oracle.json'),
            'status': oracle['status'], 'union_instruments': oracle['union_instruments'],
            'risk_context_rows': [oracle['first_risk_context_row'], oracle['decision_context_row']],
            'objective': oracle['certificate']['primal_objective_full_canonical'],
            'max_kkt_residual': oracle['certificate']['max_kkt_residual'],
            'representable_execution': oracle['representable_execution'],
            'scope': oracle['scope']},
        'native_evidence': [pin(p) for p in sorted(NATIVE.rglob('*')) if p.is_file()],
        'investment_evidence': {'complete_replay': complete,
            'recorded_proposals': len(certs['decisions']),
            'accepted_allocations_from_completed_report': len(certs['decisions']) if complete else 0,
            'sharpe': None, 'capacity': None, 'live_orders_authorized': False,
            'qualification': 'No investment acceptance; adjustment economics, publication, instrument eligibility, borrow and executable capacity remain unverified.'},
        'sources': [pin(p) for p in sorted(set(source_paths + [ROOT / p for p in test_paths]))],
        'documents': [pin(p) for p in document_paths],
        'evidence': [pin(p) for p in evidence],
        'references': ['https://github.com/osqp/osqp/blob/v1.0.0/src/polish.c',
                       'https://arxiv.org/pdf/1711.08013']
    }
    with DESTINATION.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(receipt, stream, sort_keys=True, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps(pin(DESTINATION)))


if __name__ == '__main__':
    main()
