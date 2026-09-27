"""Deterministic 38-candidate research library v5.1 = frozen v4 (37) + one v5.1 addition; no data read.

Library v5.1 (T34b) implements the v4 pre-registration section "v5.1 family", R1'': the frozen library v4
(fund_industry_ic_v4.json, 37 candidates) is copied unchanged (ids, families, fields, DSL bytes, lineage and
templates; pinned by library and recipe SHA-256 and re-derived from its own generator), then exactly one
prior-signed candidate is appended as a separate, disclosed family:

  opex_at   profitability_quality   B   Novy-Marx (2011, Review of Finance) operating leverage

opex_at = operating costs / total assets, ranked within FF12 industry like every theme 1-3 member of v4. The
fields-v6 TRAIN panel has no opex_ttm (nor xsga_ttm or cogs_ttm; T34a), so operating costs are the proxy
sale_ttm - oi_ttm = COGS + SG&A (incl. R&D) + D&A + other operating items (the paper: COGS + XSGA). No producer
change; the data pins are unchanged. The DSL is the pre-registered string, byte for byte.

The v4.2 generator asserts that every addition is cross-section ranked and outside the within-industry themes;
controller ruling R-b replaces that rule for v5.1 with v4's own: an addition to a theme 1-3 (here
profitability_quality) is ranked group_rank(., grp_ff12). Every other v4.2 check is kept: the frozen v4 pins,
the v4 re-validation, <= 5 extras per candidate (IC runner plan budget), <= 7 estimated slots per candidate (the
v4 library maximum, so max_compiled_slots does not grow), declared fields cover the referenced ones, and the
registry cross-check. The static validator is the pinned v4 generator's, unmodified: opex_at needs no registry
row beyond v4's. The frozen v4 generator and its files are not modified. The sign is embedded (prior_sign +1).

Run with --check to verify the committed exact JSON bytes.
"""
from __future__ import annotations

import argparse
import copy
import functools
import hashlib
import importlib.util
import json
import sys
from pathlib import Path
from types import ModuleType

HERE = Path(__file__).resolve().parent
V4_GENERATOR = 'generate_fund_ic_v4.py'
V4_LIBRARY = 'fund_industry_ic_v4.json'
V4_RECIPE = 'fund_industry_ic_v4.recipe.json'
V4_LIBRARY_SHA256 = 'daa9663e43119102fbb42aba3be9b57961cd34925920c50baee7f340f0e1eddf'
V4_RECIPE_SHA256 = '62b510f12a79cfacd2e74d64e020f20cee202cadb82153653b2e1065ec250c81'
V4_CANDIDATES = 37
TOTAL_CANDIDATES = 38
LIBRARY = 'fund_industry_ic_v5.json'
RECIPE = 'fund_industry_ic_v5.recipe.json'
LIBRARY_ID = 'fund_industry_ic_v5'
PREREG = ('.superpowers/sdd/mega-alpha-20260926/v4-prereg.md sections R1 (v4, frozen) and "v5.1 family" R1\'\' '
          '(declared 2026-09-27 after the T34a breadth check, commit c8192c46; disclosed; before any v5.1 TRAIN read)')
MAX_EXTRAS_PER_CANDIDATE = 5  # IC runner plan budget (controller ruling, T27; kept by R-b)
MAX_SLOTS_PER_CANDIDATE = 7   # the v4 library maximum: the runner charges only the library max (ruling R-b)
ADDED_THEME = 'profitability_quality'
WITHIN_FF12 = 'within_industry_grp_ff12'
# Prereg v5.1 R1'' (verbatim): the generator must render exactly these bytes.
PREREG_DSL = {'opex_at': 'decay_linear(group_rank((((sale_ttm - oi_ttm) / at) + (0 * log(at))), grp_ff12), 21)'}


def _load_v4(name: str) -> ModuleType:
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, HERE / V4_GENERATOR)
    assert spec is not None and spec.loader is not None, V4_GENERATOR
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclass annotation resolution needs the module registered
    spec.loader.exec_module(module)
    return module


@functools.cache
def _pinned_v4() -> tuple[ModuleType, bytes, bytes]:
    """An untouched copy of the v4 generator, its library and recipe bytes checked against the pins."""
    module = _load_v4('_frozen_generate_fund_ic_v4_for_v5')
    docs = module.documents()
    library_bytes, recipe_bytes = docs[V4_LIBRARY], docs[V4_RECIPE]
    assert hashlib.sha256(library_bytes).hexdigest() == V4_LIBRARY_SHA256, 'frozen v4 library bytes changed'
    assert hashlib.sha256(recipe_bytes).hexdigest() == V4_RECIPE_SHA256, 'frozen v4 recipe bytes changed'
    for name, blob in docs.items():
        assert (HERE / name).read_bytes() == blob, f'committed {name} differs from its generator'
    return module, library_bytes, recipe_bytes


def frozen_v4() -> tuple[bytes, bytes]:
    """The v4 library and recipe bytes from an untouched copy of their generator, checked against the pins."""
    return _pinned_v4()[1:]


def v4() -> ModuleType:
    """The pinned v4 generator (validator, grammar, Spec, expression); not extended for v5.1."""
    return _pinned_v4()[0]


def parse(text: str):
    return v4().parse(text)


def registry_crosscheck(engine_root: Path | None = None) -> str:
    return v4().registry_crosscheck(engine_root)


def candidate_block(library_bytes: bytes) -> bytes:
    """The bytes of a library's candidates array between its brackets (the rows as encoded in the file)."""
    head, tail = b'  "candidates": [\n', b'\n  ]\n}\n'
    start = library_bytes.index(head) + len(head)
    assert library_bytes.endswith(tail) and library_bytes.count(head) == 1
    return library_bytes[start:-len(tail)]


# ---- the v5.1 addition ----------------------------------------------------------
def new_specs() -> list:
    m = v4()
    g = m.grammar()
    Expr, binary, call = g.Expr, g.binary, g.call
    F = {f.name: Expr(f.name, f.prior_bars) for f in m.FIELDS}
    zero = Expr('0')
    div = lambda a, b: binary(a, '/', b)
    sub = lambda a, b: binary(a, '-', b)
    add = lambda a, b: binary(a, '+', b)
    mul = lambda a, b: binary(a, '*', b)

    def positive(x, *domains):  # v4's guard: x + 0 * log(d) is NaN unless every d > 0
        for d in domains:
            x = add(x, mul(zero, call('log', d)))
        return x

    at = F['at']
    operating_costs = sub(F['sale_ttm'], F['oi_ttm'])  # proxy for COGS + XSGA (no opex_ttm in fields-v6)
    Spec = m.Spec
    return [
        Spec('opex_at', ADDED_THEME, 'B', positive(div(operating_costs, at), at), WITHIN_FF12,
             'Novy-Marx (2011, RF) operating leverage; proxy opex = sale_ttm - oi_ttm (includes D&A) because '
             'fields-v6 has no opex_ttm',
             '(sale_ttm - oi_ttm) / at', 1, 'non-positive total assets -> NaN (house guard)',
             'operating costs = revenue - operating income = COGS + SG&A (incl. R&D) + D&A + other operating items '
             '(impairments, restructuring); the paper uses COGS + XSGA, excluding DP. fields-v6 has no opex_ttm, '
             'xsga_ttm or cogs_ttm (T34a), so no producer change (prereg v5.1 R1\'\'); OperatingIncomeLoss is '
             'single-concept, and FF12 Money revenue includes interest (ranked within its own industry only)'),
    ]


THEME_DESCRIPTIONS = {
    'profitability_quality': 'Profitability and quality (gross, operating, cash, ROE, ROA, low accruals, F-score) and '
                             'operating leverage (operating costs to assets), ranked within FF12 industry; high '
                             'quality and high operating leverage predict higher returns.',
}
EXPECTED_TURNOVER_NEW = {
    'opex_at': 'very low (< 0.02/day, as gpa): accounting ranks step at filings; s21 decay',
}


def documents() -> dict[str, bytes]:
    m = v4()
    library_v4_bytes, recipe_v4_bytes = frozen_v4()
    library_v4, recipe_v4 = json.loads(library_v4_bytes), json.loads(recipe_v4_bytes)
    assert len(library_v4['candidates']) == V4_CANDIDATES
    static = []
    for cand, row in zip(library_v4['candidates'], recipe_v4['lineage'], strict=True):
        assert cand['id'] == row['id'] and row['dsl_sha256'] == hashlib.sha256(cand['dsl'].encode()).hexdigest()
        check = m.validate(cand['dsl'], row['prior_bars'])
        assert check['native_prior_bars'] == row['native_prior_bars'] and check['fields'] == row['fields']
        static.append(check)
    candidates, lineage, templates = (copy.deepcopy(library_v4['candidates']), copy.deepcopy(recipe_v4['lineage']),
                                      copy.deepcopy(recipe_v4['templates']))
    added = []
    for order, spec in enumerate(new_specs(), start=V4_CANDIDATES + 1):
        assert spec.theme in m.THEME_IDS and spec.tier in m.TIER_RANK and spec.raw_prior_direction in (-1, 1)
        # Ruling R-b: v4's own rule (a theme 1-3 member is ranked within FF12) replaces v4.2's cross-section-only rule.
        in_block = spec.theme in m.WITHIN_INDUSTRY_THEMES and spec.id not in m.WITHIN_INDUSTRY_EXCEPTIONS
        assert spec.ranking.startswith('within_industry') == in_block, spec.id
        assert spec.theme == ADDED_THEME and spec.ranking == WITHIN_FF12, spec.id
        assert spec.smoothing == m.SMOOTHING or spec.smoothing_reason, spec.id
        assert parse(spec.base.text)[3] == spec.base.lookback, spec.id
        expr = m.expression(spec)
        assert expr.text == PREREG_DSL[spec.id], (spec.id, expr.text)
        tree = parse(expr.text)[0]
        ranked = tree[2] if tree[1] == 'TsDecayLinear' else tree
        assert ranked[:2] == ('call', 'CsRankG') and ranked[3] == ('field', m.WITHIN_INDUSTRY), spec.id
        check = m.validate(expr.text, expr.lookback)
        assert check['estimated_peak_slots'] <= MAX_SLOTS_PER_CANDIDATE, (spec.id, check['estimated_peak_slots'])
        static.append(check)
        labels = dict(theme=spec.theme, tier=spec.tier, tier_rank=m.TIER_RANK[spec.tier], prior_sign=1,
                      citation=spec.citation)
        candidates.append(dict(id=spec.id, family=spec.theme, dsl=expr.text, horizons=[5, 21, 63],
                               sign_policy='train-rank-ic21', **labels))
        lineage.append(dict(id=spec.id, family=spec.theme, roster_order=order, **labels,
                            raw_prior_direction=spec.raw_prior_direction, ranking=spec.ranking,
                            smoothing_sessions=spec.smoothing, prior_bars=expr.lookback,
                            native_prior_bars=check['native_prior_bars'],
                            dsl_sha256=hashlib.sha256(expr.text.encode()).hexdigest(), fields=check['fields']))
        templates.append(dict(id=spec.id, theme=spec.theme, formula=spec.formula, base_dsl=spec.base.text,
                              base_prior_bars=spec.base.lookback, raw_prior_direction=spec.raw_prior_direction,
                              domain=spec.domain or None, deviation=spec.deviation or None,
                              smoothing_sessions=spec.smoothing, smoothing_reason=spec.smoothing_reason or None))
        added.append(spec)
    total = len(candidates)
    ids = [c['id'] for c in candidates]
    assert total == TOTAL_CANDIDATES and len(set(ids)) == total and len({c['dsl'] for c in candidates}) == total
    assert [s.id for s in added] == list(PREREG_DSL)
    assert candidates[:V4_CANDIDATES] == library_v4['candidates'] and lineage[:V4_CANDIDATES] == recipe_v4['lineage']
    assert all(len(s['extra_fields']) <= MAX_EXTRAS_PER_CANDIDATE for s in static), 'IC runner plan budget'
    assert all(s['estimated_peak_slots'] <= MAX_SLOTS_PER_CANDIDATE for s in static), 'library slot maximum'
    declared = {f['name'] for f in library_v4['fields']}
    referenced = set().union(*(set(s['fields']) for s in static))
    assert referenced <= declared and declared - referenced <= {'raw_close', 'volume'} - referenced
    families = [dict(f, description=THEME_DESCRIPTIONS.get(f['id'], f['description'])) for f in library_v4['families']]
    library = dict(schema='atx.dsl-ic-library/v1', id=LIBRARY_ID, fields=library_v4['fields'], families=families,
                   candidates=candidates)
    encode = lambda obj: (json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + '\n').encode()
    library_bytes = encode(library)
    # The frozen rows are byte-identical in the file itself: v4's candidates array is a prefix of v5.1's.
    assert candidate_block(library_bytes).startswith(candidate_block(library_v4_bytes) + b',\n')

    extras = sorted(declared - set(m.BASE_FIELDS))
    recipe = copy.deepcopy(recipe_v4)
    members = {t['theme']: list(t['members']) for t in recipe['themes']}
    for spec in added:
        members[spec.theme].append(spec.id)
    within = recipe_v4['within_industry']
    hygiene = ('The v4 roster (37) is frozen and copied byte-identically. The one addition, opex_at, with its theme, '
               'tier, sign, citation and DSL, is the v5.1 family R1\'\' (declared 2026-09-27 after the T34a breadth '
               'check, which read manifest metadata and source code only, and before any v5.1 TRAIN read), '
               'implemented verbatim. The implementer read no v4/v5 per-candidate TRAIN performance output '
               '(admission statistics, daily IC files) and no validation data. Disclosed: the operating-cost proxy '
               'includes D&A and other operating items (fields-v6 has no opex_ttm); opex/at = sale/at - oi/at shares '
               'asset turnover with gpa (sale/at - cogs/at), so a high rank correlation with gpa is expected and the '
               'redundancy screen may drop opex_at; it still counts as one disclosed trial.')
    recipe.update(
        id=f'{LIBRARY_ID}_initial',
        library=dict(path=LIBRARY, sha256=hashlib.sha256(library_bytes).hexdigest()),
        preregistration=PREREG,
        generation=dict(recipe_v4['generation'], rule='frozen-v4-37-plus-opex_at-v51', revision='initial',
                        candidates=total,
                        frozen_v4=dict(generator=V4_GENERATOR, library=V4_LIBRARY, library_sha256=V4_LIBRARY_SHA256,
                                       recipe=V4_RECIPE, recipe_sha256=V4_RECIPE_SHA256, candidates=V4_CANDIDATES,
                                       copy='ids, families (description of profitability_quality updated), fields, '
                                            'DSL bytes, lineage and templates identical'),
                        new=dict(candidates=len(added), ids=[s.id for s in added], registry_rows={},
                                 ranking_rule='ruling R-b: a profitability_quality addition is ranked '
                                              'group_rank(., grp_ff12), as v4 themes 1-3 (replaces the v4.2 '
                                              'cross-section-only rule for additions)'),
                        generator_sha256=hashlib.sha256(Path(__file__).read_bytes().replace(b'\r\n', b'\n')).hexdigest(),
                        v4_generator_sha256=recipe_v4['generation']['generator_sha256']),
        themes=[dict(t, members=members[t['theme']], description=THEME_DESCRIPTIONS.get(t['theme'], t['description']),
                     expected_turnover_v51_additions={s.id: EXPECTED_TURNOVER_NEW[s.id] for s in added
                                                      if s.theme == t['theme']} or None)
                for t in recipe_v4['themes']],
        within_industry=dict(within, members=list(within['members']) + [s.id for s in added
                                                                       if s.ranking.startswith('within_industry')]),
        v51_additions=[dict(id=s.id, theme=s.theme, prereg='v5.1 R1\'\'', dsl=PREREG_DSL[s.id],
                            data_proxy='sale_ttm - oi_ttm for operating costs (no opex_ttm in fields-v6)')
                       for s in added],
        family_fixing=dict(fixed_before_measurement=True, measurement_consulted=False, statement=hygiene,
                           v4_statement=recipe_v4['family_fixing']['statement']),
        templates=templates,
        lineage=lineage,
        admission=dict(policy='v4-prior-v1 (T23; prereg R3; unchanged by v5 R3\' and v5.1 R1\'\')', trials=total,
                       v4_policy=recipe_v4['admission']['policy']),
        composition=dict(recipe_v4['composition'],
                         v51='ew-theme-v1 and ew-theme-aim-v1 re-fit on the 38 (prereg v5.1 R1\'\'); themes '
                             'unchanged (the fitter\'s V4_THEMES)'),
        trials=dict(recipe_v4['trials'], generated_candidates=total, admission_trials=total, new_candidates=len(added),
                    reused_v4_candidates=V4_CANDIDATES, reruns='v4 admission attempts remain prior trials of the 37',
                    v51_family=dict(admission=total, composition_added=2, construction_added=1,
                                    construction='reference cell only', statement='separate disclosed family '
                                                                                  '(prereg v5.1 R1\'\')')),
        static_validation=dict(recipe_v4['static_validation'],
                               rule=recipe_v4['static_validation']['rule'] + '; v5.1 adds no registry row (the pinned '
                                    'v4 validator, unmodified)',
                               max_prior_bars=max(s['prior_bars'] for s in static),
                               max_native_prior_bars=max(s['native_prior_bars'] for s in static),
                               max_dag_nodes=max(s['dag_nodes'] for s in static),
                               max_estimated_peak_slots=max(s['estimated_peak_slots'] for s in static),
                               max_estimated_peak_slots_limit=MAX_SLOTS_PER_CANDIDATE,
                               extra_field_capacity=max(len(s['extra_fields']) for s in static),
                               max_extras_per_candidate_limit=MAX_EXTRAS_PER_CANDIDATE,
                               declared_extra_fields=len(extras),
                               extra_field_users={f: [c['id'] for c, s in zip(candidates, static) if f in s['fields']]
                                                  for f in extras}),
        qualification=dict(recipe_v4['qualification'],
                           native_parse_vm='pending: root --plan-only compile against fields-v6 (T39)',
                           prior_phase='v1-v4 and v4.2 files and generators are unchanged by v5.1'))
    recipe['data'] = dict(recipe_v4['data'],
                          operating_costs='sale_ttm - oi_ttm = COGS + SG&A (incl. R&D) + D&A + other operating items; '
                                          'fields-v6 32565c32 has no opex_ttm, xsga_ttm or cogs_ttm (T34a); no '
                                          'producer change, data pins unchanged')
    return {LIBRARY: library_bytes, RECIPE: encode(recipe)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--engine-root', type=Path, default=None,
                        help='repository root whose atx-engine sources the registry cross-check reads (default: this tree)')
    args = parser.parse_args()
    docs = documents()
    for name, expected in docs.items():
        path = HERE / name
        if args.check:
            if not path.is_file() or path.read_bytes() != expected:
                raise SystemExit(f'fixed artifact differs: {name}')
        else:
            path.write_bytes(expected)
        print(f'{name} {hashlib.sha256(expected).hexdigest()} {len(expected)} bytes')
    library, recipe = json.loads(docs[LIBRARY]), json.loads(docs[RECIPE])
    sv = recipe['static_validation']
    print(f"candidates {len(library['candidates'])} (frozen v4 {V4_CANDIDATES} byte-identical, new "
          f"{len(library['candidates']) - V4_CANDIDATES}); themes {len(library['families'])}; max prior bars "
          f"{sv['max_prior_bars']}; max dag nodes {sv['max_dag_nodes']}; max peak slots {sv['max_estimated_peak_slots']} "
          f"(limit {MAX_SLOTS_PER_CANDIDATE}); extra-field capacity {sv['extra_field_capacity']} "
          f"(limit {MAX_EXTRAS_PER_CANDIDATE})")
    for theme in recipe['themes']:
        print(f"  {theme['index']} {theme['theme']}: {', '.join(theme['members'])}")
    print(registry_crosscheck(args.engine_root))


if __name__ == '__main__':
    main()
