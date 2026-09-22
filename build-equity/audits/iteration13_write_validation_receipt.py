"""Write the immutable checkpoint-13 validation receipt (claims-aware replay seam, T1+T2).

Checkpoint 13 was MINIMAL-CLOSED by parent ruling after an alpha-priority re-plan:
T1 (bounded claims-book state + two-phase atomic commit) and T2 (claims-aware replay seam)
are complete, reviewed and natively measured; T3 engine scenarios B/C, T4 atx-impl allocation
certification and T5 equity-book wiring are DEFERRED, not done. This receipt binds only the
bytes that exist. Run once; refuses to overwrite. Historical receipt writers are never rerun.
"""
from pathlib import Path
import hashlib
import json

ROOT = Path('C:/atx/.worktrees/equity-platform')
OUT = ROOT / 'atx-engine/reviews/2026-09-20-claims-aware-replay-validation.json'

PINNED = {
    'design': 'atx-engine/reviews/2026-09-20-iteration13-claims-aware-replay-design.md',
    'predecessor_receipt': 'atx-engine/reviews/2026-09-20-security-transition-validation.json',
    'research_reprioritization': '.superpowers/sdd/equity-platform-parent-goal/research-alpha-pipeline.md',
    'claims_state_header': 'atx-engine/include/atx/engine/book/claims_state.hpp',
    'claims_state_source': 'atx-engine/src/book/claims_state.cpp',
    'claims_state_test': 'atx-engine/tests/book/book_claims_state_test.cpp',
    'replay_header': 'atx-engine/include/atx/engine/book/replay.hpp',
    'replay_source': 'atx-engine/src/book/replay.cpp',
    'claims_replay_test': 'atx-engine/tests/book/book_claims_replay_test.cpp',
    'engine_cmake': 'atx-engine/CMakeLists.txt',
    'configure_log': 'build-equity/audits/iteration13-configure.log',
    'check_log_t1': 'build-equity/audits/iteration13-check-t1.log',
    'tests_log_t1': 'build-equity/audits/iteration13-claims-state-tests.log',
    'build_log_t1_fix1': 'build-equity/audits/iteration13-build-t1fix1.log',
    'tests_log_t1_fix1': 'build-equity/audits/iteration13-claims-state-tests-fix1.log',
    'junit_t1': 'build-equity/iteration13-claims-state-tests.xml',
    'junit_t1_fix1': 'build-equity/iteration13-claims-state-tests-fix1.xml',
    'tests_log_book_baseline_pre_t2': 'build-equity/audits/iteration13-book-baseline-tests.log',
    'junit_book_baseline_pre_t2': 'build-equity/iteration13-book-baseline-tests.xml',
    'build_log_t2_failed': 'build-equity/audits/iteration13-build-t2.log',
    'build_log_t2_fixed': 'build-equity/audits/iteration13-build-t2b.log',
    'tests_log_book_t2': 'build-equity/audits/iteration13-book-t2-tests.log',
    'junit_book_t2': 'build-equity/iteration13-book-t2-tests.xml',
    'build_log_t2_fix1': 'build-equity/audits/iteration13-build-t2fix1.log',
    'tests_log_book_t2_fix1': 'build-equity/audits/iteration13-book-t2fix1-tests.log',
    'junit_book_t2_fix1': 'build-equity/build-equity/iteration13-book-t2fix1.xml',
    't1_report': '.superpowers/sdd/equity-platform-parent-goal/task-T1-report.md',
    't1_review': '.superpowers/sdd/equity-platform-parent-goal/task-T1-review.md',
    't1_rereview': '.superpowers/sdd/equity-platform-parent-goal/task-T1-rereview.md',
    't2_report': '.superpowers/sdd/equity-platform-parent-goal/task-T2-report.md',
    't2_review': '.superpowers/sdd/equity-platform-parent-goal/task-T2-review.md',
    't2_fix_report': '.superpowers/sdd/equity-platform-parent-goal/task-T2-fix-report.md',
    't2_rereview': '.superpowers/sdd/equity-platform-parent-goal/task-T2-rereview.md',
    'producer_executable': 'build-equity/bin/atx-engine-book-tests.exe',
}


def pin(rel):
    data = (ROOT / rel).read_bytes()
    return {'path': rel, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}


def count_passed(rel):
    text = (ROOT / rel).read_text(encoding='utf-8', errors='replace')
    for line in text.splitlines():
        if 'tests passed' in line and 'out of' in line:
            return line.strip()
    return None


def main():
    assert not OUT.exists(), 'immutable receipt already exists'
    pins = {key: pin(rel) for key, rel in PINNED.items()}
    receipt = {
        'schema': 'atx-claims-aware-replay-validation-v1',
        'checkpoint': 13,
        'date': '2026-09-20',
        'status': 'minimal_close_t1_t2_only',
        'summary': (
            'Bounded claims-book state with two-phase atomic security-transition and cash-claim '
            'commits (T1) and a claims-aware replay seam over scheduled intents with an event '
            'batch policy (T2) compiled, built and measured natively under clang-cl 18 (equity-dev, '
            'static Debug, /W4 /WX). Each task was independently reviewed and fixed in one round '
            'and re-reviewed clean. The default (empty event batch) replay path is pinned '
            'bit-identical to replay_scheduled_intents at the interval and per-allocation level. '
            'Checkpoint was minimal-closed after an alpha-priority re-plan; T3-T5 are deferred.'),
        'native_measurement': {
            't1_claims_state': {
                'initial': count_passed(PINNED['tests_log_t1']),
                'post_fix_round_1': count_passed(PINNED['tests_log_t1_fix1']),
            },
            'book_group_regex': '^(BookAllocation|BookPipeline|BookReplay|BookReport|ClaimsState|'
                                'ClaimsReplay|DecayController|DecayMonitor|ReportBorrow|'
                                'SecurityTransition)\\.',
            'book_group_pre_t2_baseline': count_passed(PINNED['tests_log_book_baseline_pre_t2']),
            'book_group_post_t2': count_passed(PINNED['tests_log_book_t2']),
            'book_group_post_t2_fix1': count_passed(PINNED['tests_log_book_t2_fix1']),
            't2_first_build': 'FAILED: replay.cpp(644) -Werror,-Wmissing-field-initializers '
                              '(pending_claims); fixed by explicit 17-field aggregate init; '
                              'failed log preserved (build_log_t2_failed)',
            'toolchain': 'clang-cl 18.1.8, preset equity-dev, static Debug, 4 build jobs, '
                         'ccache disabled, PCH enabled (no hygiene/include-clean claim)',
        },
        'review': {
            't1': '0 Critical / 3 Important addressed in one fix round; F1 CMake blast-radius '
                  'false positive (diff verified one line); D1-D5 + Minors F4-F12 deferred',
            't2': 'SPEC Pass; QUALITY 0 Critical / 4 Important addressed in one fix round '
                  '(dead period-0/terminal batch branch deleted, structural bound documented; '
                  'per-allocation bit-identity; row-published settlement identity with moving '
                  'prices; retired-target rejection coverage); 11 original + 2 new Minors deferred',
            't2_deviations': 'all 10 accepted, incl. no marked_equities parameter on '
                             'commit_cash_claim_payment and batch for period p requested at '
                             'iteration p-1 (first valuation of p is the end mark of (p-1,p])',
        },
        'deferred_by_ruling': [
            'T3 engine fixture scenarios B (short payable with borrow) and C (atomic rejections)',
            'T4 atx-impl claims-aware allocation certification',
            'T5 atx-impl equity-book wiring, movements/claims CSV, --transitions flag',
            'Reason: research-alpha-pipeline.md; design section 8 lists eight unmet preconditions '
            'for real PCS admission that T3-T5 supply none of; alpha->tradeable pipeline '
            '(checkpoint 14 cross-sectional forecast evaluation) prioritized',
        ],
        'qualifications': [
            'Synthetic-fixture evidence only; no real corporate action (including MetroPCS/T-Mobile '
            '2013-05-01) has been admitted, replayed or backtested; that admission remains rejected.',
            'The claims-aware seam is not wired into atx-impl; equity-book output is unchanged.',
            'Numerical netting does not prove stock-loan discharge; no physical delivery attested.',
            'Not throughput, capacity, alpha, Sharpe or production-readiness evidence. No live '
            'trading or broker actions performed or authorized.',
        ],
        'pins': pins,
    }
    OUT.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'receipt': str(OUT),
                      'sha256': hashlib.sha256(OUT.read_bytes()).hexdigest(),
                      'measurements': receipt['native_measurement']}))


if __name__ == '__main__':
    main()
