"""Freeze independent time/space/domain refinement evidence for public corpora."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

import numpy as np
import scipy

from pde_reference import price_group


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_new(path, data):
    with Path(path).open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(data, stream, indent=2, allow_nan=False)
        stream.write('\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--groups', nargs='*', help='Explicit group IDs; omitted means every frozen group')
    parser.add_argument('--nx', type=int, default=2049)
    parser.add_argument('--nt', type=int, default=512)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    corpus_path = Path(args.corpus).resolve()
    corpus = json.loads(corpus_path.read_text())
    if corpus.get('schema') != 'american-put-comparator-corpus-v1':
        raise ValueError('Only the declared continuous-carry put corpus is supported')
    ids = [g['id'] for g in corpus['groups']]
    if len(set(ids)) != len(ids) or (args.groups is not None and
            (len(set(args.groups)) != len(args.groups) or not set(args.groups) <= set(ids))):
        raise ValueError('Duplicate or missing group IDs')
    groups = [g for g in corpus['groups'] if args.groups is None or g['id'] in args.groups]
    if any(g.get('side', 'put') != 'put' or g.get('dividends') for g in groups):
        raise ValueError('Corpus model must be continuous-carry puts')
    if not groups or args.nx < 33 or args.nx % 2 != 1 or args.nt < 4:
        raise ValueError('Empty selection or invalid base grid')
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    nx, nt = args.nx, args.nt
    grids = [('base', nx, nt, 1.), ('time', nx, 2 * nt, 1.),
             ('space', 2 * nx - 1, 2 * nt, 1.), ('time_fine', 2 * nx - 1, 4 * nt, 1.),
             ('space_fine', 4 * nx - 3, 4 * nt, 1.),
             # Expand domain at unchanged nominal dx, so this is a boundary test.
             ('domain', 6 * (nx - 1) + 1, 4 * nt, 1.5)]
    repo = source.parents[2]
    receipt = dict(schema='atx-independent-pde-reference-v1', started_utc=datetime.now(timezone.utc).isoformat(),
        command=sys.argv, input_path=str(corpus_path), input_sha256=digest(corpus_path),
        source_commit=subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
        dirty_status=subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain'], text=True),
        source_sha256={p.name: digest(p) for p in [Path(__file__), source / 'pde_reference.py']},
        python=sys.version, numpy=np.__version__, scipy=scipy.__version__, platform=platform.platform(),
        thread_environment={k: os.environ.get(k) for k in ['OMP_NUM_THREADS', 'MKL_NUM_THREADS', 'OPENBLAS_NUM_THREADS']},
        corpus_groups=len(corpus['groups']), selected_groups=len(groups),
        selected_group_ids=[g['id'] for g in groups], selected_cases=sum(len(g['spots']) for g in groups),
        refinement_grids=grids, numerical_method_independent_of_alo=True,
        certified_error_bounds=False, cash_dividend_accuracy_established=False,
        greek_accuracy_established=False, timing_is_performance_claim=False, results=[])
    write_new(output / 'started.json', receipt)
    for index, group in enumerate(groups):
        row = dict(group=group, grids=[], empirical_indicator_usd=None,
                   status='unresolved', analytic_prices=None)
        for label, nodes, steps, scale in grids:
            start = time.perf_counter()
            try:
                value = price_group(group, nx=nodes, nt=steps, domain_scale=scale)
                row['grids'].append(dict(label=label, status='finite', elapsed_s=time.perf_counter() - start, **value))
            except (ValueError, ArithmeticError, OverflowError) as error:
                row['grids'].append(dict(label=label, status='refused', reason=str(error),
                                        nx=nodes, nt=steps, domain_scale=scale, elapsed_s=time.perf_counter() - start))
        if all(g['status'] == 'finite' for g in row['grids']):
            p = [np.array(g['prices']) for g in row['grids']]
            # An explicitly empirical indicator, not a rigorous error bound.
            # Include separate last time/space/domain differences, no cancellation
            # through a combined refinement or Richardson extrapolation.
            indicator = 4. * np.maximum.reduce([np.abs(p[3] - p[2]), np.abs(p[4] - p[3]), np.abs(p[5] - p[4])])
            row['empirical_indicator_usd'] = indicator.tolist()
            row['reference_prices'] = p[4].tolist()
            row['status'] = 'refinement_complete'
            if group['r'] <= 0. and group['q'] >= 0.:
                def black(spot):
                    vol = group['sigma'] * math.sqrt(group['T'])
                    d1 = (math.log(spot / group['K']) +
                          (group['r'] - group['q'] + .5 * group['sigma'] ** 2) * group['T']) / vol
                    return .5 * group['K'] * math.exp(-group['r'] * group['T']) * math.erfc((d1 - vol) / math.sqrt(2)) - \
                        .5 * spot * math.exp(-group['q'] * group['T']) * math.erfc(d1 / math.sqrt(2))
                row['analytic_prices'] = [black(s) for s in group['spots']]
                row['analytic_residuals_usd'] = np.abs(p[4] - np.array(row['analytic_prices'])).tolist()
        name = f'group-{index:03d}.json'
        write_new(output / name, row)
        receipt['results'].append(dict(id=group['id'], path=name, sha256=digest(output / name), status=row['status']))
        print(f'{index + 1}/{len(groups)} {group["id"]}: {row["status"]}', flush=True)
    receipt['finished_utc'] = datetime.now(timezone.utc).isoformat()
    if digest(corpus_path) != receipt['input_sha256'] or any(digest(source / name) != value
            for name, value in receipt['source_sha256'].items()):
        raise RuntimeError('Reference source/input changed during execution')
    write_new(output / 'receipt.json', receipt)


if __name__ == '__main__':
    main()
