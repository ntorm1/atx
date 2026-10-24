"""No-solve integrity checks for bounded working-set work, not price acceptance."""
import json
import math
from pathlib import Path


def require(ok, why):
    if not ok:
        raise ValueError(why)


TOTALS = ('invocations', 'slot_detail_invocations_omitted', 'synthetic_invocations',
          'injected_refusals', 'full_variables_sum',
          'full_rows_sum', 'bound_rows_sum', 'initial_scan_rows', 'initial_violated_rows',
          'selection_scan_rows', 'attempts', 'reduced_solves', 'full_solves', 'iterations',
          'factorizations', 'directions', 'full_scan_rows', 'newly_added_rows',
          'workspace_bytes_requested_sum', 'allocation_refusals', 'capacity_refusals')
SLOT_COUNTS = ('selected_rows', 'iterations', 'factorizations', 'directions', 'matrix_nonzeros',
               'factor_nonzeros_peak', 'iterate_scan_rows', 'publication_scan_rows',
               'violated_published_rows', 'newly_added_rows', 'outcome')
FIRST_COUNTS = ('full_variables', 'full_rows', 'bound_rows', 'initial_scan_rows',
                'initial_violated_rows', 'selection_scan_rows', 'full_scan_rows',
                'workspace_bytes', 'fallback_reason', 'injected_refusals')


def counts(record, keys):
    require(all(type(record[k]) is int and record[k] >= 0 for k in keys), 'invalid count')


def verify_work(a, budget=100, *, allow_synthetic=False):
    require(type(budget) is int and budget > 0, 'invalid per-solve budget')
    try:
        require(a['schema'] == 'joint-working-set-work-v1', 'work schema')
        counts(a, TOTALS)
        calls = a['invocations']
        require(a['synthetic_invocations'] <= calls, 'synthetic invocation count')
        require(allow_synthetic or a['synthetic_invocations'] == a['injected_refusals'] == 0,
                'synthetic aggregate in normal report')
        require(a['slot_detail_invocations_omitted'] == max(0, calls-1), 'omitted call count')
        require(a['attempts'] == a['reduced_solves']+a['full_solves'] <= 4*calls, 'attempt cap')
        require(a['reduced_solves'] <= 3*calls and a['full_solves'] <= calls, 'solve caps')
        require(a['iterations'] <= budget*a['attempts'], 'iteration cap')
        require(a['factorizations'] <= a['iterations'], 'factor count')
        require(a['directions'] <= 2*a['factorizations'], 'direction count')
        f = a['first_invocation']
        counts(f, FIRST_COUNTS)
        require(type(f['present']) is bool and f['present'] == (calls != 0), 'first call scope')
        require(type(f['synthetic_control']) is bool, 'synthetic type')
        require(allow_synthetic or not f['synthetic_control'], 'synthetic result in normal report')
        require(a['synthetic_invocations'] >= int(f['synthetic_control']) and
                a['injected_refusals'] >= f['injected_refusals'], 'synthetic aggregate join')
        require(f['synthetic_control'] or f['injected_refusals'] == 0, 'unlabeled injection')
        require(f['fallback_reason'] <= 7, 'unknown fallback reason')
        require(f['bound_rows'] == 2*f['full_variables'], 'full bound denominator')
        require(f['initial_scan_rows'] == f['full_rows'], 'initial full scan')
        require(f['initial_violated_rows'] <= f['initial_scan_rows'], 'initial violations')
        require(f['selection_scan_rows'] in (0, f['full_rows']), 'selection scan')
        slots = f['solves']
        require(type(slots) is list and len(slots) <= 4, 'slot count')
        sums = dict(iterations=0, factorizations=0, directions=0, newly_added_rows=0)
        scans = f['initial_scan_rows']+f['selection_scan_rows']
        full = reduced = 0
        for i, s in enumerate(slots):
            counts(s, SLOT_COUNTS)
            require(type(s['full_fallback']) is bool, 'fallback type')
            require(s['outcome'] in range(1, 6), 'unattempted/unknown slot')
            require(f['bound_rows'] <= s['selected_rows'] <= f['full_rows'], 'selected row bounds')
            if s['full_fallback']:
                full += 1
                require(i == len(slots)-1 and s['selected_rows'] == f['full_rows'], 'full fallback scope')
            else:
                reduced += 1
                if i:
                    previous = slots[i-1]
                    require(s['selected_rows'] == previous['selected_rows']+previous['newly_added_rows'],
                            'selection must add every discovered omitted row')
            require(s['iterations'] <= budget and s['factorizations'] <= s['iterations'], 'slot budget')
            require(s['directions'] <= 2*s['factorizations'], 'slot directions')
            require(s['iterate_scan_rows'] == s['publication_scan_rows'] and
                    s['iterate_scan_rows'] in (0, f['full_rows']), 'complete paired scans')
            require(s['violated_published_rows'] <= s['publication_scan_rows'], 'published violations')
            require(s['newly_added_rows'] <= f['full_rows']-s['selected_rows'], 'new rows')
            for key in ('min_iterate_slack', 'min_published_slack'):
                require(type(s[key]) in (float, int) and math.isfinite(s[key]), 'nonfinite slack metadata')
            scans += s['iterate_scan_rows']+s['publication_scan_rows']
            for key in sums:
                sums[key] += s[key]
        require(full <= 1 and reduced <= 3, 'first call solve caps')
        require(scans == f['full_scan_rows'], 'lost/full scan work')
        require((full != 0) == (f['fallback_reason'] != 0), 'fallback reason scope')
        first = dict(attempts=len(slots), reduced_solves=reduced, full_solves=full,
                     synthetic_invocations=int(f['synthetic_control']),
                     injected_refusals=f['injected_refusals'],
                     full_variables_sum=f['full_variables'], full_rows_sum=f['full_rows'],
                     bound_rows_sum=f['bound_rows'], workspace_bytes_requested_sum=f['workspace_bytes'],
                     **sums)
        for key in ('initial_scan_rows', 'initial_violated_rows', 'selection_scan_rows', 'full_scan_rows'):
            first[key] = f[key]
        for key, value in first.items():
            require(a[key] == value if calls <= 1 else a[key] >= value, 'first/aggregate work join: '+key)
        if calls == 0:
            require(not slots and all(a[key] == 0 for key in TOTALS), 'empty aggregate')
    except (KeyError, TypeError, IndexError) as error:
        raise ValueError('incomplete work evidence') from error
    return True


def verify_native(report, budget=100):
    """The benchmark stores each chain once in graded_stats.slices.

    This joins work only; the independent thirteen actual-API gates are separate.
    It makes no assumption that global work always consists of just one invocation.
    """
    slices = report['graded_stats']['slices']
    require(type(slices) is list and slices, 'no chain denominator')
    totals = dict.fromkeys(TOTALS, 0)
    for item in slices:
        work = item['timing']['joint_working_set']
        verify_work(work, budget)
        for key in totals:
            totals[key] += work[key]
    require(totals['invocations'] > 0, 'no enabled projection work')
    return dict(schema='joint-working-set-work-summary-v1', chains=len(slices), totals=totals,
                financial_acceptance='separate actual-API oracle required', timing_qualified=False)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('native', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    require(not args.output.exists(), 'output already exists')
    args.output.write_text(json.dumps(verify_native(json.loads(args.native.read_text())), indent=2)+'\n')
