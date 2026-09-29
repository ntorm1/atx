"""Static check of research library v6 (fund_industry_ic_v6.json) against a fields manifest; pure Python, no data.

Validates, independently of the generator:
  * unique candidate ids and unique DSL strings; roster size <= --max-roster (48);
  * every candidate carries a prior sign (prior_sign +1: the literature sign is embedded in the DSL), a theme that
    is a declared family, a tier and a citation;
  * every field a DSL string names exists in the given fields manifest (research fields) or is a role base field
    (close, raw_close, volume), and is declared by the library;
  * no forbidden operator: every call is in the allowlist below (incl. the platform-v7 W2 literature ops, W2_OPS);
    the denylist names the stateful recurrences whose value depends on the panel's first date (and the
    multi-output test builtin); a Group builder (bucket, group_cross) must feed a group operator;
  * IC-runner static limits: DSL <= 4096 bytes, <= 5 extra (non-base) fields per candidate (EXTRAS_EXCEPTIONS: the
    pre-registered per-candidate rulings, q5_eg 6 in library v7.0);
and prints the diff against the baseline library (v5.1): added, removed and changed (same id, new DSL) ids.
With --require-baseline-prefix (library v6.1 against baseline v6), the baseline's candidate entries must be an
identical prefix of the library's (same objects, same order) and --expect-added the exact appended ids.
Exit status 1 on any failure. The manifest is metadata only (field names); no field values are read.

Run: "C:/Program Files/Python312/python.exe" atx-impl/strategies/check_fund_ic_v6.py --manifest <fields manifest.json>
v6.1: ... check_fund_ic_v6.py --manifest <fields-v7 manifest.json> --library atx-impl/strategies/fund_industry_ic_v61.json
      --baseline atx-impl/strategies/fund_industry_ic_v6.json --require-baseline-prefix --expect-added sv_flow
v7.0: ... check_fund_ic_v6.py --manifest <fields-v7 manifest.json> --library atx-impl/strategies/fund_industry_ic_v70.json
      --baseline atx-impl/strategies/fund_industry_ic_v61.json --max-roster 56 --require-baseline-prefix
      --expect-added qmj_safety,nincr,q5_eg,smax5,res_mom_ind
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE_FIELDS = ('close', 'raw_close', 'volume')  # role price / volume fields (not in the research fields manifest)
# Platform-v7 W2 literature ops (atx-engine registry.cpp literature_ops; semantics in alpha/lit_ops.hpp). Allowing
# them here only makes a candidate expressible; admitting one is a pre-registered library revision.
W2_OPS = frozenset({
    'ts_topk_mean', 'ts_count_increases', 'ts_resid_on', 'ts_beta_on',                  # trailing window
    'ts_sum_mp', 'ts_mean_mp', 'ts_std_mp', 'ts_zscore_mp', 'ts_min_mp', 'ts_max_mp',  # min-periods family
    'decay_linear_mp', 'ts_corr_mp',
    'cs_resid_on', 'bucket', 'group_cross',                                             # cross-sectional
    'pack2', 'pack3',                                                                   # regressor packs
})
ALLOWED_OPS = frozenset({
    'abs', 'log', 'signedpower', 'power',                        # element-wise
    'rank', 'group_rank', 'group_neutralize', 'group_mean',      # cross-sectional (member-masked)
    'delay', 'ts_sum', 'stddev', 'ts_max', 'ts_backfill', 'decay_linear', 'correlation', 'ts_count_nans',  # trailing
}) | W2_OPS
# Explicit policy entries beyond the v6 set, each with its reason (root decision, platform-v7 W2):
POLICY_OPS = frozenset({
    'vec_sum',  # cross-sectional sum (engine CsVecSum): the q5 expected-growth FWL slope vec_sum(resid * g) /
                # vec_sum(resid^2) (Hou, Mo, Xue and Zhang 2021; task-W2-report.md)
    'sign',     # element-wise sign (engine Sign; NaN stays NaN): library v7.0 nincr's year-on-year increase
                # indicator max(sign(ni_q - ni_q_lag4), 0) (Barth, Elliott and Finn 1999; library-v7-draft W1-2)
    'max',      # element-wise two-operand max (engine MaxP; NaN if either operand is NaN, not std::max): the same
                # nincr indicator (v7-prereg.md "Library v7.0" Correction: max() and sign() via POLICY_OPS)
})
ALLOWED_OPS = ALLOWED_OPS | POLICY_OPS
DENIED_OPS = frozenset({'trade_when', 'hump', 'kalman_level', 'ou_filter', 'kalman', 'split2'})
GROUP_OPS = frozenset({'group_rank', 'group_neutralize', 'group_mean'})
GROUP_BUILDERS = frozenset({'bucket', 'group_cross'})  # yield a Group classifier, never a signal
MAX_DSL_BYTES = 4096
MAX_EXTRAS = 5
# Pre-registered per-candidate exceptions to MAX_EXTRAS (id -> limit), each a root ruling declared before any read:
EXTRAS_EXCEPTIONS = {
    'q5_eg': 6,  # library v7.0 (v7-prereg.md "Library v7.0"; library-v7-draft 3.5a): six fields-v7 fields, no new field
}
TOKEN = re.compile(r'\s*(?:(\d+(?:\.\d+)?)|([A-Za-z_][A-Za-z0-9_.]*)|(.))')


def names(dsl: str) -> tuple[list[str], list[str]]:
    """(operators, fields) named by a DSL string: an identifier followed by '(' is an operator, else a field."""
    toks = [next(g for g in m.groups() if g is not None) for m in TOKEN.finditer(dsl) if m.group(0).strip()]
    ops, fields = [], []
    for k, tok in enumerate(toks):
        if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.]*', tok):
            (ops if k + 1 < len(toks) and toks[k + 1] == '(' else fields).append(tok)
    return ops, fields


def manifest_fields(path: Path) -> set[str]:
    doc = json.loads(path.read_bytes())
    rows = doc['fields']
    out = {r['name'] if isinstance(r, dict) else r for r in rows}
    if not out:
        raise SystemExit(f'{path}: no fields')
    return out


def check(library: dict, fields_available: set[str], max_roster: int) -> list[str]:
    errors = []
    cands = library['candidates']
    ids = [c.get('id') for c in cands]
    families = {f['id'] for f in library['families']}
    declared = {f['name'] for f in library['fields']}
    if len(ids) != len(set(ids)):
        errors.append(f'duplicate ids: {sorted({i for i in ids if ids.count(i) > 1})}')
    dsls = [c.get('dsl') for c in cands]
    if len(dsls) != len(set(dsls)):
        errors.append('duplicate DSL strings')
    if len(cands) > max_roster:
        errors.append(f'roster {len(cands)} > {max_roster}')
    if not declared >= set(BASE_FIELDS):
        errors.append(f'library does not declare the base fields {BASE_FIELDS}')
    for f in sorted(declared - set(BASE_FIELDS) - fields_available):
        errors.append(f'declared field {f!r} is not in the manifest')
    for c in cands:
        cid = c.get('id')
        if c.get('prior_sign') != 1:
            errors.append(f'{cid}: prior_sign {c.get("prior_sign")!r} (must be +1: sign embedded in the DSL)')
        for key in ('theme', 'tier', 'citation'):
            if not c.get(key):
                errors.append(f'{cid}: missing {key}')
        if c.get('theme') not in families or c.get('family') != c.get('theme'):
            errors.append(f'{cid}: theme/family {c.get("theme")!r}/{c.get("family")!r} not a declared family')
        dsl = c.get('dsl') or ''
        if len(dsl.encode()) > MAX_DSL_BYTES:
            errors.append(f'{cid}: DSL longer than {MAX_DSL_BYTES} bytes')
        ops, used = names(dsl)
        for op in sorted(set(ops)):
            if op in DENIED_OPS:
                errors.append(f'{cid}: forbidden operator {op}')
            elif op not in ALLOWED_OPS:
                errors.append(f'{cid}: operator {op} not in the allowlist')
        for f in sorted(set(used)):
            if f not in fields_available and f not in BASE_FIELDS:
                errors.append(f'{cid}: field {f!r} not in the manifest')
            if f not in declared:
                errors.append(f'{cid}: field {f!r} not declared by the library')
        extras = set(used) - set(BASE_FIELDS)
        limit = EXTRAS_EXCEPTIONS.get(cid, MAX_EXTRAS)
        if len(extras) > limit:
            errors.append(f'{cid}: {len(extras)} extra fields > {limit} (IC runner plan budget)')
        if any(f.startswith('grp_') for f in used) and not set(ops) & GROUP_OPS:
            errors.append(f'{cid}: a group field without a group operator')
        if set(ops) & GROUP_BUILDERS and not set(ops) & GROUP_OPS:
            errors.append(f'{cid}: a group builder (bucket / group_cross) without a group operator')
    return errors


def prefix_errors(library: dict, baseline: dict, expect_added: list[str] | None) -> list[str]:
    """The baseline's candidate entries must open the library's list unchanged; the rest are the expected additions."""
    new, old = library['candidates'], baseline['candidates']
    errors = [f'baseline candidate {k} ({o.get("id")}) is not identical at the same position'
              for k, o in enumerate(old) if k >= len(new) or new[k] != o]
    added = [c.get('id') for c in new[len(old):]]
    if expect_added is not None and added != expect_added:
        errors.append(f'appended ids {added} != expected {expect_added}')
    return errors


def diff(library: dict, baseline: dict) -> dict[str, list[str]]:
    new = {c['id']: c['dsl'] for c in library['candidates']}
    old = {c['id']: c['dsl'] for c in baseline['candidates']}
    return dict(added=[i for i in new if i not in old], removed=[i for i in old if i not in new],
                changed=[i for i in new if i in old and new[i] != old[i]],
                unchanged=[i for i in new if i in old and new[i] == old[i]])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--manifest', type=Path, required=True, help='research fields manifest.json (metadata only)')
    parser.add_argument('--library', type=Path, default=HERE / 'fund_industry_ic_v6.json')
    parser.add_argument('--baseline', type=Path, default=HERE / 'fund_industry_ic_v5.json')
    parser.add_argument('--max-roster', type=int, default=48)
    parser.add_argument('--require-baseline-prefix', action='store_true',
                        help='the baseline candidate entries must be an identical prefix of the library (v6.1 vs v6)')
    parser.add_argument('--expect-added', default=None, help='comma-separated ids appended after the baseline prefix')
    args = parser.parse_args()
    blob = args.library.read_bytes()
    library, baseline = json.loads(blob), json.loads(args.baseline.read_bytes())
    available = manifest_fields(args.manifest)
    print(f'library {args.library.name} sha256 {hashlib.sha256(blob).hexdigest()} candidates '
          f'{len(library["candidates"])}; manifest {args.manifest} ({len(available)} fields, sha256 '
          f'{hashlib.sha256(args.manifest.read_bytes()).hexdigest()})')
    errors = check(library, available, args.max_roster)
    if args.require_baseline_prefix:
        expect = [x for x in args.expect_added.split(',') if x] if args.expect_added is not None else None
        prefix = prefix_errors(library, baseline, expect)
        errors += prefix
        print(f'baseline prefix: {len(baseline["candidates"])} entries ' + ('identical' if not prefix else 'DIFFER'))
    d = diff(library, baseline)
    print(f'vs {args.baseline.name}: added {len(d["added"])} {d["added"]}')
    print(f'  removed {len(d["removed"])} {d["removed"]}')
    print(f'  changed {len(d["changed"])} {d["changed"]}')
    print(f'  unchanged {len(d["unchanged"])} {d["unchanged"]}')
    ops = sorted({op for c in library['candidates'] for op in names(c['dsl'])[0]})
    used = sorted({f for c in library['candidates'] for f in names(c['dsl'])[1]})
    print(f'operators used {ops}')
    print(f'fields used {len(used)}: {used}')
    for e in errors:
        print(f'FAIL {e}')
    print('check: ' + ('FAILED' if errors else 'ok (ids unique, prior signs present, fields in manifest, no forbidden '
                                             f'ops, roster {len(library["candidates"])} <= {args.max_roster})'))
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
