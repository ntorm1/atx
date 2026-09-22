"""Write the immutable checkpoint-14 validation receipt (cross-sectional forecast evaluation).

Stage 1 only: the frozen 189-observation 2013 training context. Binds the design (all
revisions' rulings live inside it), engine and stage sources, the independent exact oracle
(v1 and v2) and comparator, every native measurement log incl. the failed builds, the
real-data run's measurement plus its acceptance re-evaluation, the trial ledger state, and
the review records. Run once; refuses to overwrite. Historical receipt writers are never
rerun.
"""
from pathlib import Path
import hashlib
import json

ROOT = Path('C:/atx/.worktrees/equity-platform')
DATA = Path('C:/atx/data/equity_ic_training_2013_20260920')
OUT = ROOT / 'atx-engine/reviews/2026-09-20-cross-section-ic-validation.json'
SDD = '.superpowers/sdd/equity-platform-parent-goal/'

PINNED = {
    'design': 'atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md',
    'research': SDD + 'research-alpha-pipeline.md',
    'predecessor_receipt': 'atx-engine/reviews/2026-09-20-claims-aware-replay-validation.json',
    'engine_header': 'atx-engine/include/atx/engine/eval/cross_section_ic.hpp',
    'engine_source': 'atx-engine/src/eval/cross_section_ic.cpp',
    'engine_test': 'atx-engine/tests/eval/eval_cross_section_ic_test.cpp',
    'engine_cmake': 'atx-engine/CMakeLists.txt',
    'ledger_header': 'atx-impl/src/trial_ledger.hpp',
    'ledger_source': 'atx-impl/src/trial_ledger.cpp',
    'ledger_test': 'atx-impl/tests/trial_ledger_test.cpp',
    'stage_header': 'atx-impl/src/stage_equity_ic.hpp',
    'stage_source': 'atx-impl/src/stage_equity_ic.cpp',
    'stage_config_test': 'atx-impl/tests/config_equity_ic_test.cpp',
    'stage_test': 'atx-impl/tests/stage_equity_ic_test.cpp',
    'impl_config_hpp': 'atx-impl/src/config.hpp',
    'impl_config_cpp': 'atx-impl/src/config.cpp',
    'impl_dispatch': 'atx-impl/src/dispatch.cpp',
    'impl_cmake': 'atx-impl/CMakeLists.txt',
    'docs_equity_ic': 'atx-impl/docs/EQUITY_IC.md',
    'oracle_v1_script': 'build-equity/audits/iteration14_cross_section_oracle.py',
    'oracle_v1': 'build-equity/audits/iteration14-cross-section-oracle.json',
    'oracle_v2_script': 'build-equity/audits/iteration14_cross_section_oracle_v2.py',
    'oracle_v2': 'build-equity/audits/iteration14-cross-section-oracle-v2.json',
    'comparator': 'build-equity/audits/iteration14_native_comparator.py',
    'comparison_v1_oracle_prefix': 'build-equity/audits/iteration14-native-comparison-v1oracle.json',
    'comparison_v2_final': 'build-equity/audits/iteration14-native-comparison-v2.json',
    'configure_log': 'build-equity/audits/iteration14-configure.log',
    'configure_log_t4': 'build-equity/audits/iteration14-configure-t4.log',
    'check_t1': 'build-equity/audits/iteration14-check-t1.log',
    'build_t1': 'build-equity/audits/iteration14-build-t1.log',
    'tests_t1': 'build-equity/audits/iteration14-eval-t1-tests.log',
    'build_t1fix1_failed': 'build-equity/audits/iteration14-build-t1fix1.log',
    'build_t1fix2': 'build-equity/audits/iteration14-build-t1fix2.log',
    'tests_t1fix2': 'build-equity/audits/iteration14-eval-t1fix2-tests.log',
    'build_t2t3': 'build-equity/audits/iteration14-build-t2t3.log',
    'tests_t2t3': 'build-equity/audits/iteration14-eval-t2t3-tests.log',
    'build_t2t3fix1': 'build-equity/audits/iteration14-build-t2t3fix1.log',
    'tests_t2t3fix1_final_engine': 'build-equity/audits/iteration14-eval-t2t3fix1-tests.log',
    'junit_engine_final': 'build-equity/iteration14-eval-t2t3fix1-tests.xml',
    'eval_group_regression_t1': 'build-equity/audits/iteration14-eval-group-tests.log',
    'check_t4_unconfigured_failed': 'build-equity/audits/iteration14-check-t4.log',
    'check_t4': 'build-equity/audits/iteration14-check-t4b.log',
    'build_t4_blocked_by_inflight_engine': 'build-equity/audits/iteration14-build-t4.log',
    'build_t4fix1_failed': 'build-equity/audits/iteration14-build-t4fix1.log',
    'build_t4fix1b': 'build-equity/audits/iteration14-build-t4fix1b.log',
    'tests_t4fix1b': 'build-equity/audits/iteration14-impl-t4fix1b-tests.log',
    'build_t5': 'build-equity/audits/iteration14-build-t5.log',
    'tests_t5': 'build-equity/audits/iteration14-impl-t5-tests.log',
    'build_t5fix1': 'build-equity/audits/iteration14-build-t5fix1.log',
    'tests_t5fix1_final_stage': 'build-equity/audits/iteration14-impl-t5fix1-tests.log',
    'junit_stage_final': 'build-equity/iteration14-impl-t5fix1-tests.xml',
    'build_atx_impl': 'build-equity/audits/iteration14-build-atx-impl.log',
    'full_regression_wrapper': 'build-equity/audits/iteration14-impl-full-wrapper.log',
    'full_regression_log': 'build-equity/audits/iteration14-impl-full-tests.log',
    'smoke_rerun_preexisting_failure': 'build-equity/audits/iteration14-smoke-rerun.log',
    'runner': 'build-equity/audits/iteration14_run_equity_ic.py',
    'run_dryrun': 'build-equity/audits/iteration14-run-dryrun2.log',
    'run_attempt1_measurement': 'build-equity/audits/iteration14-equity-ic-measurement.json',
    'run_attempt1_reevaluation_script': 'build-equity/audits/iteration14_reevaluate_attempt1.py',
    'run_attempt1_reevaluation': 'build-equity/audits/iteration14-equity-ic-measurement-attempt1-reeval.json',
    'trial_ledger': 'atx-engine/reviews/trial-ledger.jsonl',
    'design_review': SDD + 'cp14-design-review.md',
    'design_rereview': SDD + 'cp14-design-rereview.md',
    'design_rereview2': SDD + 'cp14-design-rereview2.md',
    't1_report': SDD + 'cp14-task-T1-report.md',
    't1_review': SDD + 'cp14-task-T1-review.md',
    't1_rereview': SDD + 'cp14-task-T1-rereview.md',
    't2t3_report': SDD + 'cp14-task-T2T3-report.md',
    't2t3_review': SDD + 'cp14-task-T2T3-review.md',
    't4_report': SDD + 'cp14-task-T4-report.md',
    't4_review': SDD + 'cp14-task-T4-review.md',
    't5_report': SDD + 'cp14-task-T5-report.md',
    't5_review': SDD + 'cp14-task-T5-review.md',
    't7a_report': SDD + 'cp14-task-T7a-report.md',
    'engine_test_executable': 'build-equity/bin/atx-engine-eval-tests.exe',
    'impl_test_executable': 'build-equity/bin/atx-impl-tests.exe',
    'impl_executable': 'build-equity/bin/atx-impl.exe',
}
DATA_FILES = ['coverage.csv', 'ic.csv', 'ic_decay.csv', 'ic_summary.json', 'manifest.json',
              'quantile_spread.csv', 'request.json', 'seal.json', 'signal_autocorr.csv']


def pin_path(path, rel):
    data = path.read_bytes()
    return {'path': rel, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}


def passed_line(rel):
    for line in (ROOT / rel).read_text(encoding='utf-8', errors='replace').splitlines():
        if 'tests passed' in line and 'out of' in line:
            return line.strip()
    return None


def main():
    assert not OUT.exists(), 'immutable receipt already exists'
    pins = {key: pin_path(ROOT / rel, rel) for key, rel in PINNED.items()}
    data_pins = {name: pin_path(DATA / name, str(DATA / name)) for name in DATA_FILES}
    sidecar = ROOT / 'atx-engine/reviews/trial-ledger.jsonl.manifest.json'
    if sidecar.exists():
        pins['trial_ledger_sidecar'] = pin_path(sidecar, 'atx-engine/reviews/trial-ledger.jsonl.manifest.json')
    summary = json.loads((DATA / 'ic_summary.json').read_text(encoding='utf-8'))
    comparison = json.loads((ROOT / PINNED['comparison_v2_final']).read_text(encoding='utf-8'))
    reeval = json.loads((ROOT / PINNED['run_attempt1_reevaluation']).read_text(encoding='utf-8'))
    measurement = json.loads((ROOT / PINNED['run_attempt1_measurement']).read_text(encoding='utf-8'))
    ledger_lines = [json.loads(line) for line in
                    (ROOT / PINNED['trial_ledger']).read_text(encoding='utf-8').splitlines() if line]
    headline = []
    for entry in summary['series']:
        full = entry['full']
        ci = full.get('ic_mean_ci', {})
        headline.append({
            'signal': entry['signal'], 'variant': entry['variant'],
            'restriction': entry['restriction'], 'horizon': entry['horizon'],
            'dates_emitted': full.get('dates_emitted'), 'ic_mean': full.get('ic_mean'),
            'rank_ic_mean': full.get('rank_ic_mean'), 'icir': full.get('icir'),
            'ic_mean_ci_lo': ci.get('lo'), 'ic_mean_ci_hi': ci.get('hi'),
            'ic_ci_reportable': ci.get('reportable'),
            'spread_gross_mean': full.get('spread_gross_mean'),
            'spread_net_mean': full.get('spread_net_mean'),
            'spread_reportable': entry.get('spread_reportable'),
        })
    receipt = {
        'schema': 'atx-cross-section-ic-validation-v1',
        'checkpoint': 14,
        'date': '2026-09-20',
        'status': 'validated_stage1_sign_and_shape_only',
        'summary': (
            'Cross-sectional forecast evaluation (Pearson and rank IC per date, ICIR with circular '
            'block bootstrap, signal autocorrelation and implied turnover, decile spread gross and '
            'net for a gross-2.0 book with calendar-day borrow drag, per-date coverage, common-'
            'sample decay column, both forward-return variants, ex-34-audited-ID restriction) of '
            'the equity path\'s three deployed signals on the frozen 189-observation 2013 training '
            'context. Engine unit (64 native tests incl. 24 state/validation/seal) agrees with an '
            'independent exact oracle on 42/42 cases; the atx-impl equity-ic subcommand ran once '
            'on real data under a structural 2023-2025 seal, pre-registered N=30 trials in a hash-'
            'chained ledger before computing, and completed. Sign-and-shape evidence only.'),
        'sign_and_shape_statement': summary.get('sign_and_shape_statement'),
        'pre_registration': {
            'trial_id': ledger_lines[0].get('trial_id') if ledger_lines else None,
            'trial_count_declared': ledger_lines[0].get('trial_count_declared') if ledger_lines else None,
            'ledger_statuses': [line.get('status') for line in ledger_lines],
            'ledger_lines': len(ledger_lines),
            'design_note_sha256_embedded_in_stage': next(
                (parent.get('sha256') for parent in ledger_lines[0].get('parents', [])
                 if parent.get('role') == 'design-note'), None) if ledger_lines else None,
        },
        'native_measurement': {
            'toolchain': 'clang-cl 18.1.8, preset equity-dev (groups risk;data;core;book;eval), '
                         'static Debug, /W4 /permissive- /WX, 4 build jobs, ccache disabled, PCH '
                         'enabled (no hygiene claim)',
            'engine_tests_final': passed_line(PINNED['tests_t2t3fix1_final_engine']),
            'engine_eval_group_regression_at_t1': passed_line(PINNED['eval_group_regression_t1']),
            'trial_ledger_tests': passed_line(PINNED['tests_t4fix1b']),
            'stage_tests_final': passed_line(PINNED['tests_t5fix1_final_stage']),
            'full_regression': passed_line(PINNED['full_regression_wrapper']),
            'full_regression_note': '983/988; 4 NOT_BUILT placeholders (targets outside the '
                                    'configured groups) and 1 pre-existing failure '
                                    'StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard '
                                    '(invalid stod argument; stage_run.cpp and the test last '
                                    'modified before this checkpoint; in-process RunConfig, no CLI '
                                    'path; not caused by checkpoint 13/14; open defect)',
            'failed_attempts_preserved': [
                'iteration14-build-t1fix1.log: static_assert on kSealedBeginNs caught a wrong day '
                'count (19723 = 2024-01-01) before any run; fixed to 19358',
                'iteration14-build-t4.log: atx-impl-tests build blocked by the in-flight engine TU',
                'iteration14-build-t4fix1.log: -Wunused-result in trial_ledger_test.cpp:206',
                'iteration14-check-t4.log: single-TU check before reconfigure (no object target)',
            ],
        },
        'oracle_comparison': {
            'oracle_v1_defect': 'four F3 fixtures placed the last observed mark at t+h, which '
                                'ruling A-6 assigns to the ordinary return; re-cut as v2 with '
                                'unchanged expected values plus one new precedence case',
            'final_status': comparison['status'], 'final_counts': comparison['counts'],
            'comparator_enum_enforcement': 'unreportable_reason codes 1 and 2 asserted; full '
                                           '0..4 enum enforcement deferred',
        },
        'real_data_run': {
            'attempt': 1,
            'command_line': measurement.get('command_line'),
            'exit_code': measurement.get('exit_code'),
            'wall_seconds': measurement.get('wall_seconds'),
            'runner_accepted_as_written': measurement.get('accepted'),
            'runner_defect': reeval.get('defect'),
            'accepted_reevaluated': reeval.get('accepted_reevaluated'),
            'pins_digest_unchanged': reeval.get('pins_digest_unchanged'),
            'observations': summary.get('observations'),
            'instruments': summary.get('instruments'),
            'common_sample_dates': summary.get('common_sample_dates'),
            'cost_model_provenance': summary.get('cost_model_provenance'),
            'output_directory': str(DATA),
            'output_pins': data_pins,
            'headline_full_sample': headline,
        },
        'review': {
            'design': 'Fix required (8 Critical) -> Fix required narrowly (1 Critical) -> Pass; '
                      'six ruling blocks §11.1-§11.9 recorded in the design',
            't1': 'SPEC/QUALITY Fix required (1 comment + 3 test gaps) -> Pass after one round; '
                  'RR-1..RR-3 rulings',
            't2t3': 'pass with findings (1 Critical export gap, 7 Important) -> fixed in one '
                    'round, gated by 63/63 + comparator 42/42 (no re-review per directive)',
            't4': 'pass with findings (4 Important: tail integrity, throwing fs call, torn-append '
                  'claim, concurrency) -> fixed in one round, gated by 16/16',
            't5': 'pass with findings, SAFE TO RUN: no (Critical: dangling pre-registration on '
                  'failure) -> fixed in one round, gated by 16/16, then run',
            'process_note': 'user directive mid-checkpoint: fewer review/test cycles; scoped '
                            're-reviews dropped for fix rounds from T2T3 onward',
        },
        'qualifications': [
            summary.get('sign_and_shape_statement'),
            'Stage 1 only (189 observations, 2013 training slice); Stage 2 (2013-2019 panel) '
            'not run. Bootstrap intervals at h=63 are null by pre-registered prediction.',
            'No model was fitted; the three signals are the deployed ones read from the '
            'published baseline. IC is not net P&L; the net decile spread restates the deployed '
            'cost constants and is not a replay.',
            'Survivorship: dropping missing forward returns is upward-biased; both variants and '
            'the ex-34 restriction are reported; only three terminal events are priced; PCS is '
            'never applied; the 29 unclassified audited IDs are not flagged terminal.',
            'The 2023-2025 seal is non-vacuous by code and vacuous by data at Stage 1 '
            '(dates_at_or_after_sealed == 0 says nothing about 2013 leakage).',
            'Not throughput, capacity, Sharpe, alpha-acceptance or production-readiness evidence. '
            'No live trading or broker actions performed or authorized.',
            'One pre-existing unrelated test failure (StageRunSyntheticSmoke) remains open.',
        ],
        'pins': pins,
    }
    OUT.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'receipt': str(OUT),
                      'sha256': hashlib.sha256(OUT.read_bytes()).hexdigest(),
                      'engine_tests': receipt['native_measurement']['engine_tests_final'],
                      'stage_tests': receipt['native_measurement']['stage_tests_final'],
                      'oracle': receipt['oracle_comparison']['final_counts'],
                      'ledger': receipt['pre_registration']['ledger_statuses']}))


if __name__ == '__main__':
    main()
