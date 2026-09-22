"""Write the immutable addendum to the checkpoint-13 and checkpoint-14 receipts.

Receipts are never mutated. This addendum records, from the final whole-branch review:
(I-1) the checkpoint-14 receipt pinned atx-impl/docs/EQUITY_IC.md before the docs pass
added a "Validation status" section that cites the receipt's own SHA (circular pin; the
receipt-time digest is retained inside the receipt, the post-docs digest is recorded here);
(I-2) the checkpoint-14 receipt's prose summary says "64 native tests" where every pinned
artifact says 63 (the summary string was wrong; the measured count stands);
(cp13 drift) the checkpoint-13 receipt pinned atx-engine/CMakeLists.txt, which checkpoint 14
legitimately edited by one line (src/eval/cross_section_ic.cpp); and the late close-out
edits made after the final review (PLATFORM_PROGRESS wording, README pointer, the confined
terminal-line exception path in stage_equity_ic.cpp). Run once; refuses to overwrite.
"""
from pathlib import Path
import hashlib
import json

ROOT = Path('C:/atx/.worktrees/equity-platform')
OUT = ROOT / 'atx-engine/reviews/2026-09-20-cross-section-ic-validation-addendum.json'
CP14 = ROOT / 'atx-engine/reviews/2026-09-20-cross-section-ic-validation.json'
CP13 = ROOT / 'atx-engine/reviews/2026-09-20-claims-aware-replay-validation.json'


def pin(rel):
    data = (ROOT / rel).read_bytes()
    return {'path': rel, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}


def main():
    assert not OUT.exists(), 'immutable addendum already exists'
    cp14 = json.loads(CP14.read_text(encoding='utf-8'))
    cp13 = json.loads(CP13.read_text(encoding='utf-8'))
    addendum = {
        'schema': 'atx-validation-receipt-addendum-v1',
        'date': '2026-09-20',
        'applies_to': {
            'cp14_receipt': {'path': str(CP14.relative_to(ROOT)),
                             'sha256': hashlib.sha256(CP14.read_bytes()).hexdigest()},
            'cp13_receipt': {'path': str(CP13.relative_to(ROOT)),
                             'sha256': hashlib.sha256(CP13.read_bytes()).hexdigest()},
        },
        'corrections': [
            {'id': 'I-2', 'receipt': 'cp14', 'field': 'summary',
             'wrong_text': '64 native tests incl. 24 state/validation/seal',
             'correct': '63 native EvalCrossSectionIc tests (24 of them T1 state/validation/'
                        'seal), as pinned by native_measurement.engine_tests_final and the '
                        'junit file',
             'measured_value_in_receipt': cp14['native_measurement']['engine_tests_final']},
            {'id': 'I-1', 'receipt': 'cp14', 'field': 'pins.docs_equity_ic',
             'note': 'pinned before the docs pass added a Validation status section citing '
                     'the receipt SHA; content accurate; circular by construction',
             'receipt_time_pin': cp14['pins']['docs_equity_ic'],
             'post_docs_pin': pin('atx-impl/docs/EQUITY_IC.md')},
            {'id': 'cp13-pin-drift', 'receipt': 'cp13', 'field': 'pins.engine_cmake',
             'note': 'checkpoint 14 added one source line (src/eval/cross_section_ic.cpp) '
                     'after src/eval/cpcv.cpp; the checkpoint-13 line src/book/claims_state.cpp '
                     'is unchanged',
             'receipt_time_pin': cp13['pins']['engine_cmake'],
             'current_pin': pin('atx-engine/CMakeLists.txt')},
        ],
        'post_review_close_out_edits': {
            'platform_progress': pin('atx-engine/docs/PLATFORM_PROGRESS.md'),
            'platform_progress_note': 'replaced the false "moves every number by less than '
                                      '0.002" sentence with the recomputed maxima (IC means '
                                      '0.0022, spreads 0.0008, ICIR 0.022 at h=21 / 0.080 at '
                                      'h=63) and corrected the h=63 blocks wording',
            'engine_readme': pin('atx-engine/README.md'),
            'engine_readme_note': 'replaced the stale "pending checkpoint 13 replay '
                                  'integration" pointer',
            'stage_source': pin('atx-impl/src/stage_equity_ic.cpp'),
            'stage_source_note': 'terminal ledger line construction confined in a try; an '
                                 'exception there is now reported as its own error and named '
                                 'in failure.json instead of unwinding past the append. Not '
                                 'exercised by the real-data run (which used the pinned '
                                 'pre-edit source); re-measured natively after the edit (see '
                                 'stage_tests_after_edit)',
        },
        'final_branch_review': pin('.superpowers/sdd/equity-platform-parent-goal/final-branch-review.md'),
        'open_items_reference': 'final-branch-review.md "Open items"',
    }
    tests = ROOT / 'build-equity/audits/iteration14-impl-closeout-tests.log'
    if tests.exists():
        text = tests.read_text(encoding='utf-8', errors='replace')
        line = next((l.strip() for l in text.splitlines()
                     if 'tests passed' in l and 'out of' in l), None)
        addendum['post_review_close_out_edits']['stage_tests_after_edit'] = {
            'log': pin('build-equity/audits/iteration14-impl-closeout-tests.log'),
            'result': line}
    OUT.write_text(json.dumps(addendum, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'addendum': str(OUT),
                      'sha256': hashlib.sha256(OUT.read_bytes()).hexdigest()}))


if __name__ == '__main__':
    main()
