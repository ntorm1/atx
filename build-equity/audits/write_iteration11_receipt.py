"""Freeze the observed-close experiment with compact, hash-bound evidence links."""
from pathlib import Path
import hashlib
import json
import xml.etree.ElementTree as ET

ROOT = Path('C:/atx/.worktrees/equity-platform')
AUDITS = ROOT / 'build-equity/audits'
NATIVE = Path('C:/atx/data/equity_book_training_2013_observed_close_20260920')
DESTINATION = ROOT / 'atx-engine/reviews/2026-09-20-execution-availability-validation.json'
PRIOR = ROOT / 'atx-engine/reviews/2026-09-20-execution-representation-validation.json'
PRIOR_SHA = '8337f0c8b5f82a94e4f35573f7af2737594d20816f1d76bc11dbd2a3ebea603f'


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
    tests = ET.parse(ROOT / 'build-equity/iteration11-tests.xml').getroot()
    cases = {case.attrib['name']: ('passed' if case.find('failure') is None and
             case.find('skipped') is None else 'not-passed')
             for case in tests.findall('testcase')}
    assert len(cases) == 21 and all(value == 'passed' for value in cases.values())
    measurement_path = AUDITS / 'iteration11-equity-book-measurement.json'
    verification_path = AUDITS / 'iteration11-allocation-proposal-verification.json'
    comparison_path = AUDITS / 'iteration11-startup-native-comparison.json'
    diagnostic_path = AUDITS / 'iteration11-held-gap-diagnostic-qualified.json'
    measurement, verification, comparison = map(read,
        (measurement_path, verification_path, comparison_path))
    diagnostic = read(diagnostic_path)
    assert measurement['status'] == 'failed' and measurement['exit_code'] == 1
    assert measurement['pin_recheck']['all_unchanged']
    assert verification['verification_status'] == 'verified-partial-allocation-evidence'
    assert verification['verified_proposal_count'] == 4
    assert verification['observed_complete_intervals'] == 18
    assert verification['native_failure_independently_reproduced']
    assert verification['observation_stop'] == {
        'kind': 'missing-held-mark', 'period': 19, 'instrument': 1063,
        'session_key_ns': '1367366400000000000', 'security_id': '146189',
        'incomplete_interval_borrow_debited': False}
    assert comparison['status'] == 'passed-first-startup-intent-comparison'
    assert comparison['all_pins_unchanged']
    source_paths = [Path(name) for name in measurement['pin_recheck']['source_pins']]
    for path in source_paths:
        assert pin(path)['sha256'] == measurement['pin_recheck']['source_pins'][str(path)]['after']['sha256']
    preflight = measurement['preflight']
    assert pin(preflight['executable']['path'])['sha256'] == preflight['executable']['sha256']
    snapshot = preflight['native_snapshot']
    assert pin(snapshot['path'])['sha256'] == snapshot['sha256']
    assert snapshot['verified_member_bytes'] and snapshot['member_count'] == 39
    certs = read(NATIVE / 'allocation_certificates.json')
    decisions = certs['decisions']
    assert [d['decision_period'] for d in decisions] == [0, 5, 10, 15]
    assert [d['execution_period'] for d in decisions] == [1, 6, 11, 16]
    assert [d['execution_fixed_zero_count'] for d in decisions] == [0, 2, 0, 0]
    assert [d['hold_count'] for d in decisions] == [0, 24, 59, 108]
    assert certs['last_callback_attempt']['status'] == 'certified-proposal-returned'
    assert not (NATIVE / 'manifest.json').exists()
    assert not (NATIVE / 'report/manifest.json').exists()
    failure = read(NATIVE / 'failure.json')
    document_paths = ['atx-engine/docs/PLATFORM_PROGRESS.md', 'atx-engine/README.md',
                      'atx-impl/README.md', 'atx-impl/docs/EQUITY_BOOK_CONSTRAINED.md',
                      'atx-engine/reviews/2026-09-20-iteration11-execution-availability-design.md']
    test_paths = ['atx-impl/tests/equity_allocation_test.cpp',
                  'atx-impl/tests/stage_equity_baseline_test.cpp']
    evidence = sorted(set([p for p in (ROOT / 'build-equity').glob('iteration11*') if p.is_file()] +
                          [p for p in AUDITS.glob('iteration11*') if p.is_file()] +
                          [Path(__file__)]))
    kept_fields = ['decision_period', 'execution_period', 'union_instruments',
        'execution_unavailable_count', 'execution_fixed_zero_count', 'fixed_zero_count',
        'required_zero_count', 'hold_count', 'close_count', 'actual_turnover',
        'traded_dollars', 'trade_cost', 'postfee_net', 'postfee_gross', 'postfee_max_name',
        'pretrade_nav', 'posttrade_nav', 'representation_l1_change',
        'representation_l1_budget', 'decision_fixed_zero_l1_change',
        'execution_fixed_zero_l1_change']
    receipt = {
        'date': '2026-09-20', 'checkpoint': 11, 'goal_status': 'active',
        'scope': 'Observed-close entry equalities, conditional allocation provenance, callback diagnostics and original-window trial',
        'status': 'bounded_software_validation_complete_native_trial_recorded',
        'workspace': {'path': str(ROOT), 'branch': 'feat/equity-platform-20260920'},
        'prior_checkpoint': pin(PRIOR),
        'predeclared_plan': read(AUDITS / 'iteration11-execution-availability-plan.json'),
        'tests': {'passed': 21, 'failed': 0, 'skipped': 0, 'new_cases': 3,
                  'wall_seconds': 16.57, 'cases': cases,
                  'qualification': 'Affected allocation/stage contracts only. Prior dense-PCG timeout remains incomplete; full repository, sanitizers and throughput unqualified.'},
        'build': {'compiler': 'VS2022 clang-cl 18.1.8', 'preset': 'equity-dev',
                  'targets': ['atx-impl', 'atx-impl-tests'], 'jobs': 4,
                  'cache_disabled': True, 'wrapper': 'scripts/atx-build.ps1',
                  'translation_units_checked': ['atx-impl/src/equity_allocation.cpp',
                                                'atx-impl/src/stage_equity_book.cpp'],
                  'initial_test_compile_failure': 'GoogleTest std::string-versus-JSON hash comparisons; four explicit JSON string extractions corrected the fixture. Production translation units and executable had already built; no production change for this correction.'},
        'implementation': [
            'Strict RequireRequestedMark remains public helper default; stage explicitly selects ObservedCloseEntryConstraintV1.',
            'Exactly unheld invalid current marks add weight=0 equalities before optimization; original union, frozen preference, eligibility and risk remain.',
            'All held marks are validated before exclusion; no price imputation, current-volume filter or permanent security deletion.',
            'One canonical reason vector governs QP boxes, exact-zero lifting, representation and final certification.',
            'Disjoint decision/execution lifting components and conditional solver scope remain separate from ordinary representation corrections.',
            'Record typed invalid-mark mappings, source/timing identities, reason digest and last callback status without inventing raw rejected candidates.',
            'Reserve additional JSON/identifier memory before replay and preserve exact producer/selected source bytes in a verified archive.'
        ],
        'review': {'helper': 'root static review passed',
                   'stage': 'independent helper-owner review passed',
                   'timing_limit': 'Availability conditional on the same observed close is a sampled-price research abstraction; available_by_order_submission remains unverified.'},
        'measurement': {'artifact': pin(measurement_path),
            'native_status': measurement['status'], 'exit_code': measurement['exit_code'],
            'native_wall_seconds': measurement['native_wall_seconds'],
            'monitor_wall_seconds': measurement['total_wall_seconds'],
            'os_peak_working_set_bytes': measurement['sampled_os_maxima_bytes']['PeakWorkingSetSize'],
            'os_peak_private_commit_bytes': measurement['sampled_os_maxima_bytes']['PeakPagefileUsage'],
            'all_input_source_executable_snapshot_pins_unchanged': True,
            'producer': preflight['executable'], 'native_snapshot': snapshot,
            'failure': failure['error'],
            'proposals': [{key: d[key] for key in kept_fields} for d in decisions]},
        'independent_verification': {'artifact': pin(verification_path),
            'proposal_count': 4, 'complete_observable_intervals': 18,
            'failure_independently_reproduced': True,
            'observation_stop': verification['observation_stop'],
            'last_callback_evidence': verification['last_callback_evidence'],
            'unchanged_checked_hashes': len(verification['input_hashes']),
            'scope_limit': 'All recorded representation and economic checks; no general QP optimality or source economics attestation. Native floating-point JSON mark digest is recorded, not independently recomputed; independent source-row bytes and integer reason digest are verified.'},
        'independent_startup_comparison': {'artifact': pin(comparison_path),
            'status': comparison['status'], 'all_pins_unchanged': True,
            'scope': 'Frozen startup oracle is used only after source-derived confirmation that availability adds no equality inside the original startup union.'},
        'held_gap_diagnostic': {'artifact': pin(diagnostic_path),
            'archive_ticker_annotation': 'PCS', 'vendor_security_id': '146189',
            'tri_research_units': -1888.5372719122356,
            'last_valid_mark_date': '2013-04-30', 'last_valid_mark': 11.84,
            'last_valid_signed_notional_usd': -22360.28129944087,
            'source_classification': 'source_row_absent',
            'event_applied': False,
            'scope': 'Archive annotation and measured TRI inventory; physical share basis, vendor identity mapping and short transition liabilities remain unverified.'},
        'investment_evidence': {'complete_replay': False, 'recorded_certified_proposals': 4,
            'accepted_allocations_from_completed_report': 0, 'sharpe': None, 'capacity': None,
            'live_orders_authorized': False,
            'qualification': 'No investment acceptance; source economics, publication, classification, shorting and executable capacity remain unverified.'},
        'next_iteration': 'Research and implement a stock-and-cash security-transition accounting seam; preserve current failure and require explicit source/basis evidence before admitting a real event.',
        'native_evidence': [pin(p) for p in sorted(NATIVE.rglob('*')) if p.is_file()],
        'sources': [pin(p) for p in sorted(set(source_paths + [ROOT / p for p in test_paths]))],
        'documents': [pin(p) for p in document_paths],
        'evidence': [pin(p) for p in evidence],
        'references': ['https://www.cvxportfolio.com/en/stable/_static/cvx_portfolio.pdf',
                       'https://raw.githubusercontent.com/cvxgrp/cvxportfolio/1.5.0/cvxportfolio/simulator.py',
                       'https://www.nyse.com/trade/auctions',
                       'https://www.sec.gov/Archives/edgar/data/1283699/000119312513193449/d527693d8k.htm']
    }
    with DESTINATION.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(receipt, stream, sort_keys=True, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps(pin(DESTINATION)))


if __name__ == '__main__':
    main()
