"""Write the immutable checkpoint-12 validation receipt (security transition planner).

Binds the bytes of the design, source, oracle, comparator, native measurement attempts,
comparison outputs and review records that exist at the time of writing. Run once; refuses
to overwrite. Historical receipt writers are never rerun.
"""
from pathlib import Path
import hashlib
import json

ROOT = Path('C:/atx/.worktrees/equity-platform')
OUT = ROOT / 'atx-engine/reviews/2026-09-20-security-transition-validation.json'

PINNED = {
    'design': 'atx-engine/reviews/2026-09-20-iteration12-security-transition-design.md',
    'guide': 'atx-engine/docs/SECURITY_TRANSITIONS.md',
    'data_header': 'atx-engine/include/atx/engine/data/security_transition.hpp',
    'book_header': 'atx-engine/include/atx/engine/book/security_transition.hpp',
    'source': 'atx-engine/src/book/security_transition.cpp',
    'test': 'atx-engine/tests/book/security_transition_test.cpp',
    'oracle_script': 'build-equity/audits/iteration12_transition_oracle.py',
    'oracle': 'build-equity/audits/iteration12-transition-oracle.json',
    'comparator': 'build-equity/audits/iteration12_native_comparator.py',
    'plan': 'build-equity/audits/iteration12-security-transition-plan.json',
    'configure_log_stop_time': 'build-equity/audits/iteration12-configure.log',
    'check_log_stop_time_source': 'build-equity/audits/iteration12-check.log',
    'build_log_stop_time_source': 'build-equity/audits/iteration12-build.log',
    'check_log_fix_round1': 'build-equity/audits/iteration12-check-fix1.log',
    'build_log_fix_round2': 'build-equity/audits/iteration12-build-fix2.log',
    'runner_attempt1': 'build-equity/audits/iteration12_run_transition_checks.py',
    'runner_attempt2': 'build-equity/audits/iteration12_run_transition_checks_attempt2.py',
    'runner_attempt3': 'build-equity/audits/iteration12_run_transition_checks_attempt3.py',
    'runner_attempt4': 'build-equity/audits/iteration12_run_transition_checks_attempt4.py',
    'measurement_attempt1': 'build-equity/audits/iteration12-native-measurement.json',
    'measurement_attempt2': 'build-equity/audits/iteration12-native-measurement-attempt2.json',
    'measurement_attempt3': 'build-equity/audits/iteration12-native-measurement-attempt3.json',
    'measurement_attempt4': 'build-equity/audits/iteration12-native-measurement-attempt4.json',
    'snapshot_stop_time': 'build-equity/audits/iteration12-native-snapshot.zip',
    'snapshot_v2_post_fix': 'build-equity/audits/iteration12-native-snapshot-v2.zip',
    'tests_log_attempt3': 'build-equity/audits/iteration12-tests-attempt3.log',
    'tests_log_attempt4': 'build-equity/audits/iteration12-tests-attempt4.log',
    'junit_attempt3': 'build-equity/iteration12-tests-attempt3.xml',
    'junit_attempt4': 'build-equity/iteration12-tests-attempt4.xml',
    'comparison_attempt3': 'build-equity/audits/iteration12-native-comparison.json',
    'comparison_attempt4': 'build-equity/audits/iteration12-native-comparison-attempt4.json',
    'review': '.superpowers/sdd/equity-platform-parent-goal/task-C-review.md',
    'fix_report': '.superpowers/sdd/equity-platform-parent-goal/task-C-fix-report.md',
    'rereview_round1': '.superpowers/sdd/equity-platform-parent-goal/task-C-rereview.md',
    'rereview_round2': '.superpowers/sdd/equity-platform-parent-goal/task-C-rereview2.md',
    'comparator_report': '.superpowers/sdd/equity-platform-parent-goal/task-B-report.md',
    'producer_executable': 'build-equity/bin/atx-engine-book-tests.exe',
}


def pin(rel):
    data = (ROOT / rel).read_bytes()
    return {'path': rel, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}


def main():
    assert not OUT.exists(), 'immutable receipt already exists'
    pins = {key: pin(rel) for key, rel in PINNED.items()}
    m4 = json.loads((ROOT / PINNED['measurement_attempt4']).read_text(encoding='utf-8'))
    c4 = json.loads((ROOT / PINNED['comparison_attempt4']).read_text(encoding='utf-8'))
    c3 = json.loads((ROOT / PINNED['comparison_attempt3']).read_text(encoding='utf-8'))
    receipt = {
        'schema': 'atx-security-transition-validation-v1',
        'checkpoint': 12,
        'date': '2026-09-20',
        'status': 'validated_synthetic_first_slice',
        'summary': (
            'Pure signed stock-and-cash corporate transition planner and cash-claim payment '
            'planner compiled, built and measured natively under clang-cl 18 (equity-dev, static '
            'Debug). Native fixture values agree with the independent exact Fraction/Decimal '
            'oracle on all five exported cases. An independent implementation review found 0 '
            'Critical / 4 Important findings; three numerical fixes (same-product successor '
            'valuation, non-tautological leg-scaled bridge reconciliation, symmetric absorbed-'
            'addition guard) plus targeted regression tests were applied and re-reviewed clean '
            'in two rounds; the fixed source was re-measured (attempt 4).'),
        'native_measurement': {
            'attempts': {
                '1': 'wrapper rejected ctest -O as an ambiguous PowerShell parameter; CTest never ran',
                '2': 'CTest ran, 3/3 passed, but PowerShell bound --verbose as a common parameter; '
                     '0 measurement lines captured',
                '3': 'stop-time source; -VV; 3/3 passed; 5 measurement lines; comparator pass 5/5',
                '4': 'post-review-fix source; -VV; 4/4 passed (new targeted group); 5 measurement '
                     'lines; comparator pass 5/5',
            },
            'final_attempt': 4,
            'attempt4_exit_code': m4['exit_code'],
            'attempt4_pins_unchanged': m4['pins_unchanged'],
            'attempt4_marker_lines': m4['measurement_marker_lines'],
            'attempt4_tests': [
                'SecurityTransition.SignedStockCashAndFullPaymentPreserveAttributedExposureAndNav',
                'SecurityTransition.IndependentTriScalesPreserveEconomicsAndUnprovedBasisRejects',
                'SecurityTransition.OrderingIdentityAndAdmissionFailuresAreAtomic',
                'SecurityTransition.AdmittedMarkToleranceAndAbsorbedAdditionsAreEnforced',
            ],
            'attempt4_passed': '4/4',
            'toolchain': 'clang-cl 18.1.8, preset equity-dev, static Debug, 4 build jobs, '
                         'ccache disabled, PCH enabled (no hygiene/include-clean claim)',
        },
        'oracle_comparison': {
            'native_cases_required_and_found': c4['required_case_ids'],
            'attempt3_status': c3['status'], 'attempt4_status': c4['status'],
            'attempt4_counts': c4['counts'],
            'oracle_only_not_measured_natively': 9,
            'bound_policy': c4['bound_policy'],
            'independence': c4['independence'],
        },
        'review': {
            'independent_implementation_review': '0 Critical, 4 Important, 9 Minor (task-C-review.md)',
            'fix_round_1': 'Important #1-#3 addressed; #4 open (missing #2 regression test)',
            'fix_round_2': 'Important #4 addressed; added() made symmetric; no new breakage',
            'deferred': 'nine original Minors plus five re-review deferrals (task-C-rereview2.md); '
                        'F1-style CMake blast-radius false positive not applicable here',
        },
        'qualifications': [
            'Synthetic-fixture admission only; no real corporate action (including MetroPCS/T-Mobile '
            '2013-05-01) has been admitted, replayed or backtested.',
            'Five of fourteen oracle scenarios executed natively; the other nine are recorded as '
            'not_measured_natively and are not counted as passed.',
            'Replay integration is not part of this checkpoint (see checkpoint 13 design).',
            'Numerical netting does not prove stock-loan discharge; no physical delivery attested.',
            'Not throughput, capacity, Sharpe or production-readiness evidence. No live trading or '
            'broker actions performed or authorized.',
            'Snapshots are selected-source-plus-producer archives, not portable rebuild bundles.',
        ],
        'pins': pins,
    }
    OUT.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'receipt': str(OUT),
                      'sha256': hashlib.sha256(OUT.read_bytes()).hexdigest()}))


if __name__ == '__main__':
    main()
