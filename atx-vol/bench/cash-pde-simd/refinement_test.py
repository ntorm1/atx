"""Adversarial reader controls against a retained complete native receipt."""
import argparse
import copy
import json
from pathlib import Path

import refinement_audit as audit


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    reference = audit.load_reference(args.reference)
    original = json.loads(args.report.read_bytes())
    result = audit.verify(reference, original)
    assert result['full_manifest'] == 50 and result['selected_outcomes'] == 12
    assert result['all_case_promotion'] is False and result['timing_qualified'] is False
    mutations = (
        lambda r: r['rows'].pop(),
        lambda r: r['manifest'].pop(),
        lambda r: r['rows'][0].update(id='unreviewed'),
        lambda r: r['rows'][1].update(space_level=3),
        lambda r: r['manifest'][0]['input'].update(rate=.125),
        lambda r: r['manifest'][0].update(selected=True),
        lambda r: r['rows'][0]['result']['binary64'].update(price='0'),
        lambda r: r['rows'][0]['work'].update(thomas_sweeps=0),
        lambda r: r['rows'][2]['result'].update(time_steps=1),
        lambda r: r.update(timed=True),
        lambda r: r.update(declaration_commit='unreviewed'),
        lambda r: r['grid_base'].update(time_grading=1.),
        lambda r: r['rows'][0].update(status='refused'),
    )
    for mutate in mutations:
        changed = copy.deepcopy(original)
        mutate(changed)
        try:
            audit.verify(reference, changed)
        except ValueError:
            continue
        raise AssertionError('mutant accepted')
    refusal = copy.deepcopy(original)
    refusal['rows'][0].update(status='refused', result=None, error='typed-grid-refusal')
    result = audit.verify(reference, refusal)
    assert result['results'][0]['classification'] == 'refused'
    assert 'unavailable' in result['axes']['DSPYp']['status']
    print('Complete12 receipt,13 invalid mutants and truthful refusal retention PASS.')


if __name__ == '__main__':
    main()
