"""Deterministic 39-candidate research library v6.1 = library v6 + sv_flow (FINRA long-horizon shorting flow).

Library v6.1 implements the v4 pre-registration section "## v6.1 sub-alpha" (owner-directed 2026-09-28, declared
before any read of the field or its returns; .superpowers/sdd/mega-alpha-20260926/v4-prereg.md) as specified by
task-V61-brief.md: library v6 (fund_industry_ic_v6.json, pinned by SHA-256 and re-derived from its own generator)
with its 38 candidate entries byte-identical and in the same order, plus ONE appended member:

  sv_flow  short_interest  B-  rank(-1 * group_neutralize(sv_ratio126, grp_ff12))

sv_ratio126 is the new producer field (atx-engine/tools/prepare_research_fields.py, formula id
finra-cnms-ratio126-lag1-v1): at session d, sum(ShortVolume) / sum(TotalVolume) of the FINRA consolidated NMS daily
short sale volume files over the sessions d-126..d-1 on which the line's PIT ticker appears (NaN below 63 such
sessions or on a zero volume sum). The member demeans it within FF12 with the DSL's group-demean op
(group_neutralize, the op and grp_ idiom of ind_adj_rev_5 / within_ind_mom; vm.hpp CsNeutG -> cs_group_demean_row:
member-masked valid set, a NaN value or NaN label stays NaN) and embeds the NEGATIVE prior sign (Wang, Yan and Zheng
2020: heavy long-term shorting flow predicts lower returns) in the DSL, so prior_sign is +1 as for every v4+ member
(the fitter refuses -1). The house wrapper R = rank(.) of a cross-section theme is the only op on top: no
time-series op (the field is already the 126-session aggregate). One variant; no window or sign search.

No TRAIN return statistic and no 2023+ data were read. Run with --check to verify the committed exact JSON bytes.
"""
from __future__ import annotations

import argparse
import functools
import hashlib
import importlib.util
import json
import sys
from pathlib import Path
from types import ModuleType

HERE = Path(__file__).resolve().parent
V6_GENERATOR = 'generate_fund_ic_v6.py'
V6_LIBRARY = 'fund_industry_ic_v6.json'
V6_RECIPE = 'fund_industry_ic_v6.recipe.json'
V6_LIBRARY_SHA256 = '5ee66d137c60527c4b00f948e8b15c04f2bddc789e988b660a4254a6df6e896a'
V6_RECIPE_SHA256 = '36c084522237b756d2847bc3e589855fafe589bba55b5111642c444afecfc72b'
V6_CANDIDATES = 38
LIBRARY = 'fund_industry_ic_v61.json'
RECIPE = 'fund_industry_ic_v61.recipe.json'
LIBRARY_ID = 'fund_industry_ic_v61'
PREREG = ('.superpowers/sdd/mega-alpha-20260926/v4-prereg.md section "## v6.1 sub-alpha" (owner-directed 2026-09-28; '
          'declared before any read of the field or its returns); task brief task-V61-brief.md')
MAX_ROSTER = 48
MAX_EXTRAS_PER_CANDIDATE = 5
MAX_SLOTS_PER_CANDIDATE = 7

SV_FIELD = 'sv_ratio126'
SV_FORMULA_ID = 'finra-cnms-ratio126-lag1-v1'
SV_FIELD_BASIS = (
    'FINRA consolidated NMS daily short sale volume ratio: sum(ShortVolume) / sum(TotalVolume) of the CNMSshvol files '
    'dated on the sessions d-126..d-1 on which the line\'s PIT ticker appears (the short-interest producer\'s symbol '
    'map); NaN below 63 such sessions or on a zero TotalVolume sum; the file dated d is never used at d (formula id '
    f'{SV_FORMULA_ID}; producer atx-engine/tools/prepare_research_fields.py, fields-v7)')
SV_FIELD_CLOCK = ('finra-cnms-ratio126-lag1-v1: files dated d-126..d-1 (lag 1 session); ticker map from TickerHistory3 '
                  'rows dated <= each file date')
SV_ORIGIN = 'fields_v7'
MEMBER = dict(
    id='sv_flow', theme='short_interest', tier='B-', ranking='cross_section',
    citation='Wang, Yan and Zheng (2020, JFE) long-term shorting flows predict negative returns; FINRA Reg SHO daily '
             'short sale volume (consolidated NMS)',
    raw_prior_direction=-1, prior_sign_source='Wang-Yan-Zheng 2020',
    formula='sv_ratio126 demeaned within FF12: sv_ratio126 - mean over the FF12 group of the valid set (members with '
            'a finite value and label); prior sign negative',
    domain='NaN when sv_ratio126 is NaN (< 63 mapped sessions or a zero volume sum) or grp_ff12 is NaN (no link or no '
           'visible SIC); a singleton FF12 group demeans to 0',
    deviation='the paper (2010-2015 FINRA daily files) averages the daily shorting-flow ratio over a long past window; '
              'here the 126-session ratio of sums (the volume-weighted mean of the daily ratio), one window (prereg: no '
              'window search), demeaned within FF12 (v6-literature 3.8 proposal ii: industry-demeaned); FINRA short '
              'volume includes market-maker hedging shorts (about half of off-exchange volume): only the long-window '
              'aggregate is used; no abnormal-flow (short-horizon) variant',
    form='R(x)', expected_turnover='low: the 126-session aggregate moves by about 1/126 of its window per session; FF12 '
                                   'demeaning adds industry-mean moves; no decay (the aggregate is its own smoothing); '
                                   'measured at admission (P1 tau screen)')
SHORT_INTEREST_DESCRIPTION = ('FINRA short interest ratio, days to cover, one-month change in short interest and the '
                              '126-session FINRA shorting flow (short sale volume / volume, within FF12); heavy or '
                              'rising shorting predicts lower returns.')
PROMOTION_TESTS = [
    'P1 admission v4-prior-v1 on the restricted TRAIN role lo1: runner sign agrees with the prior, not redundant '
    '(|rho| <= .90 with every admitted member, incl. si_ratio / dtc / si_change), tau within the screen limit',
    'P2 book: library v6.1 x ew-theme-v1 refit on lo1 x the final construction (aim-partial-v5 theta .05 dust .1 fixed, '
    'delta orders, exit .05, locate-in-aim, liquidity cache, price-risk-v1) with L FIXED at 1.247; paired dSR(net) vs '
    'mega-nav-v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247 must be > 0',
    'P3 R6\' mechanics on that cell (all-rows gross in [.90, 1.05], |mean net| <= .02, tau mean <= .20 / p95 <= .30) '
    'and S2 net >= 1.0',
]


def _load(name: str, file: str) -> ModuleType:
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    assert spec is not None and spec.loader is not None, file
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclass annotation resolution needs the module registered
    spec.loader.exec_module(module)
    return module


@functools.cache
def pinned_v6() -> tuple[ModuleType, bytes, bytes]:
    """The v6 generator, its library and recipe bytes re-derived and checked against the pins and the files."""
    module = _load('_frozen_generate_fund_ic_v6_for_v61', V6_GENERATOR)
    docs = module.documents()
    library_bytes, recipe_bytes = docs[V6_LIBRARY], docs[V6_RECIPE]
    assert hashlib.sha256(library_bytes).hexdigest() == V6_LIBRARY_SHA256, 'frozen v6 library bytes changed'
    assert hashlib.sha256(recipe_bytes).hexdigest() == V6_RECIPE_SHA256, 'frozen v6 recipe bytes changed'
    for name, blob in docs.items():
        assert (HERE / name).read_bytes() == blob, f'committed {name} differs from its generator'
    return module, library_bytes, recipe_bytes


def v4() -> ModuleType:
    """The v6 generator's private v4 validator copy (registry rows power / ts_count_nans)."""
    return pinned_v6()[0].v4()


def field_table() -> dict:
    """The v4 field table plus the v6.1 producer field (the pinned v4 validator module is not modified)."""
    m = v4()
    return dict(m.FIELD_BY_NAME, **{SV_FIELD: m.Field(SV_FIELD, SV_FIELD_BASIS, SV_FIELD_CLOCK, SV_ORIGIN)})


def parse(text: str):
    return v4().parse(text, field_table())


def validate(dsl: str, expected_lookback: int) -> dict:
    """v4.validate over the extended field table."""
    m = v4()
    tree, shape, _, lookback, native, text = parse(dsl)
    nodes, slots = m.grammar().peak_slots(tree)
    assert text == dsl and shape == 'panel' and lookback == expected_lookback <= m.MAX_PRIOR_BARS, dsl
    assert native <= lookback and nodes <= m.MAX_SLOTS and slots <= MAX_SLOTS_PER_CANDIDATE, dsl
    assert len(dsl.encode()) <= m.MAX_DSL_BYTES, dsl
    fields = sorted(m.fields_of(tree))
    return dict(prior_bars=lookback, native_prior_bars=native, dag_nodes=nodes, estimated_peak_slots=slots,
                fields=fields, extra_fields=[f for f in fields if f not in m.BASE_FIELDS])


def sv_flow_parts():
    """(base, expression) of sv_flow: base = -1 * group_neutralize(sv_ratio126, grp_ff12); expression = rank(base)."""
    m = v4()
    g = m.grammar()
    base = g.binary(g.Expr('-1'), '*', g.call('group_neutralize', g.Expr(SV_FIELD), g.Expr(m.WITHIN_INDUSTRY)))
    return base, g.call('rank', base)


def documents() -> dict[str, bytes]:
    m = v4()
    _, library_v6_bytes, recipe_v6_bytes = pinned_v6()
    library_v6, recipe_v6 = json.loads(library_v6_bytes), json.loads(recipe_v6_bytes)
    assert len(library_v6['candidates']) == V6_CANDIDATES
    base, expr = sv_flow_parts()
    check = validate(expr.text, expr.lookback)
    tree = parse(expr.text)[0]
    # rank(bin(-1 * CsNeutG(field, group))): one masked demean under the house rank, no time-series op anywhere
    assert tree[:2] == ('call', 'CsRank') and tree[2][:2] == ('bin', '*') and tree[2][2] == ('num', -1.0)
    assert tree[2][3] == ('call', 'CsNeutG', ('field', SV_FIELD), ('field', m.WITHIN_INDUSTRY))
    assert not any(op.startswith('Ts') for op in m.opcodes_of(tree)) and check['prior_bars'] == 0
    assert len(check['extra_fields']) <= MAX_EXTRAS_PER_CANDIDATE
    x = MEMBER
    assert x['theme'] in m.THEME_IDS and x['theme'] not in m.WITHIN_INDUSTRY_THEMES and x['tier'] in m.TIER_RANK
    labels = dict(theme=x['theme'], tier=x['tier'], tier_rank=m.TIER_RANK[x['tier']], prior_sign=1,
                  citation=x['citation'])
    candidate = dict(id=x['id'], family=x['theme'], dsl=expr.text, horizons=[5, 21, 63], sign_policy='train-rank-ic21',
                     **labels)
    assert list(candidate) == list(library_v6['candidates'][0]), 'candidate key order differs from v6'
    candidates = library_v6['candidates'] + [candidate]
    ids = [c['id'] for c in candidates]
    assert len(ids) == len(set(ids)) == V6_CANDIDATES + 1 <= MAX_ROSTER
    assert len({c['dsl'] for c in candidates}) == len(candidates)
    field_decl = dict(name=SV_FIELD, basis=SV_FIELD_BASIS)
    families = [dict(f, description=SHORT_INTEREST_DESCRIPTION) if f['id'] == x['theme'] else f
                for f in library_v6['families']]
    library = dict(schema=library_v6['schema'], id=LIBRARY_ID, fields=library_v6['fields'] + [field_decl],
                   families=families, candidates=candidates)
    encode = lambda obj: (json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + '\n').encode()
    library_bytes = encode(library)
    # the 38 v6 entries are a byte-identical prefix of the v6.1 candidate list (same encoder, same depth)
    region = lambda blob: blob.split(b'"candidates": [\n', 1)[1]
    assert region(library_bytes).startswith(region(library_v6_bytes)[:-len(b'\n  ]\n}\n')] + b',\n')

    order = len(candidates)
    lineage_row = dict(id=x['id'], family=x['theme'], roster_order=order, **labels,
                       raw_prior_direction=x['raw_prior_direction'], prior_sign_source=x['prior_sign_source'],
                       ranking=x['ranking'], smoothing_form=x['form'], smoothing_sessions=1,
                       prior_bars=expr.lookback, native_prior_bars=check['native_prior_bars'],
                       dsl_sha256=hashlib.sha256(expr.text.encode()).hexdigest(), fields=check['fields'],
                       v51_change='added in v6.1')
    assert list(lineage_row) == list(recipe_v6['lineage'][0])
    template = dict(id=x['id'], theme=x['theme'], formula=x['formula'], base_dsl=base.text,
                    base_prior_bars=base.lookback, raw_prior_direction=x['raw_prior_direction'], domain=x['domain'],
                    deviation=x['deviation'], smoothing_form=x['form'], smoothing_sessions=1)
    assert list(template) == list(recipe_v6['templates'][0])
    themes = []
    for t in recipe_v6['themes']:
        if t['theme'] == x['theme']:
            t = dict(t, description=SHORT_INTEREST_DESCRIPTION, members=t['members'] + [x['id']],
                     expected_turnover_v61={x['id']: x['expected_turnover']})
        themes.append(t)
    needs = []
    for row in recipe_v6['needs_new_field']:
        if 'FINRA shorting flow' in row['hypothesis']:
            row = dict(row, v61=f'resolved: producer field {SV_FIELD} ({SV_FORMULA_ID}: ratio of 126-session sums, '
                                'lag 1 session, the short-interest symbol map); member sv_flow')
        needs.append(row)
    assert sum('v61' in r for r in needs) == 1
    sv6 = recipe_v6['static_validation']
    users = dict(sv6['extra_field_users'])
    for f in check['extra_fields']:
        users[f] = users.get(f, []) + [x['id']]
    static = dict(sv6, rule=sv6['rule'] + '; v6.1 adds the field sv_ratio126 (fields-v7), no registry row',
                  max_prior_bars=max(sv6['max_prior_bars'], check['prior_bars']),
                  max_native_prior_bars=max(sv6['max_native_prior_bars'], check['native_prior_bars']),
                  max_dag_nodes=max(sv6['max_dag_nodes'], check['dag_nodes']),
                  max_estimated_peak_slots=max(sv6['max_estimated_peak_slots'], check['estimated_peak_slots']),
                  extra_field_capacity=max(sv6['extra_field_capacity'], len(check['extra_fields'])),
                  declared_extra_fields=sv6['declared_extra_fields'] + 1,
                  extra_field_users=dict(sorted(users.items())), sv_flow=check)
    data6 = recipe_v6['data']
    data = dict(data6, fields=data6['fields'] + [field_decl],
                field_clocks=dict(data6['field_clocks'], **{SV_FIELD: SV_FIELD_CLOCK}),
                field_origin=dict(data6['field_origin'], **{SV_FIELD: SV_ORIGIN}),
                requires=dict(data6['requires'], fields_v7='the fields-v7 manifest: the fields-v6b list plus '
                                                           'sv_ratio126 (--finra-short-volume)'),
                short_volume=dict(field=SV_FIELD, formula_id=SV_FORMULA_ID, window_sessions=126, min_sessions=63,
                                  lag_sessions=1, source='C:/atx/atx-db/data/raw/finra_short_volume (CNMSshvol'
                                                         'YYYYMMDD.txt.gz, receipt manifest.csv)'))
    hygiene = ('v6.1 adds exactly the pre-registered member (v4-prereg "## v6.1 sub-alpha": field, window, min count, '
               'lag, FF12 demean, negative prior sign, theme, tier) implemented literally; the implementer read no '
               'TRAIN return statistic and no data dated 2023 or later, and did not relate the field to returns. The '
               'field\'s producer coverage (member cells finite) was read to size the fields phase; it is not a return '
               'statistic. The 38 v6 entries are byte-identical.')
    gen6 = recipe_v6['generation']
    recipe = dict(
        schema=recipe_v6['schema'], id=f'{LIBRARY_ID}_initial',
        library=dict(path=LIBRARY, sha256=hashlib.sha256(library_bytes).hexdigest()),
        preregistration=PREREG,
        generation=dict(rule='v6-plus-sv-flow-v61', revision='initial', candidates=len(candidates),
                        families=gen6['families'], family_is_theme=True, one_variant_per_hypothesis=True,
                        max_roster=MAX_ROSTER,
                        wrapper=('v6 members byte-identical; sv_flow = R(x) = rank(-1 * group_neutralize(sv_ratio126, '
                                 'grp_ff12)): the house cross-section rank over the FF12 group demean, no time-series '
                                 'op (the field is the 126-session aggregate)'),
                        appended=[x['id']], v6_wrapper=gen6['wrapper'],
                        max_prior_bars=gen6['max_prior_bars'], complete_observations=gen6['complete_observations'],
                        grammar_helpers=gen6['grammar_helpers'],
                        parent_v6=dict(generator=V6_GENERATOR, library=V6_LIBRARY, library_sha256=V6_LIBRARY_SHA256,
                                       recipe=V6_RECIPE, recipe_sha256=V6_RECIPE_SHA256, candidates=V6_CANDIDATES,
                                       generator_sha256=gen6['generator_sha256']),
                        generator_sha256=hashlib.sha256(Path(__file__).read_bytes().replace(b'\r\n', b'\n')).hexdigest()),
        orientation=recipe_v6['orientation'],
        labels=dict(recipe_v6['labels'], tier_basis_v61='sv_flow takes the v6-literature grade B- (2010-15 evidence '
                                                        'only; section 4 table)'),
        themes=themes,
        within_industry=recipe_v6['within_industry'],
        industry_level_price_members=recipe_v6['industry_level_price_members'],
        industry_demeaned_members=dict(group_field=m.WITHIN_INDUSTRY, operator='group_neutralize', members=[x['id']],
                                       statement='sv_flow is ranked cross-sectionally (theme short_interest is not a '
                                                 'within-industry theme) after the FF12 demean the prereg declares'),
        v6_changes=recipe_v6['v6_changes'],
        v61_changes=[dict(id=x['id'], change='added', detail='prereg v6.1: FINRA long-horizon shorting flow, theme '
                                                              'short_interest, tier B-, prior sign negative')],
        needs_new_field=needs,
        family_fixing=dict(fixed_before_measurement=True, measurement_consulted=False, statement=hygiene,
                           v6_statement=recipe_v6['family_fixing']['statement']),
        templates=recipe_v6['templates'] + [template],
        lineage=recipe_v6['lineage'] + [lineage_row],
        data=data,
        admission=dict(policy='v4-prior-v1 (T23; prereg R3; prereg v6.1 P1)', trials=1,
                       trials_basis='prereg v6.1 trial accounting: admission +1 (only sv_flow is new; the 38 v6 '
                                    'members are identical definitions on identical data)',
                       v6_policy=recipe_v6['admission']['policy']),
        composition=dict(policy='prereg v6.1 P2: ew-theme-v1 refit on lo1 (+1 composition)',
                         themes='unchanged 9 families (the fitter\'s V4_THEMES)'),
        promotion_tests=PROMOTION_TESTS,
        trials=dict(generated_candidates=len(candidates), admission_trials=1, new_candidates=1,
                    unchanged_candidates=V6_CANDIDATES, orientation_fits=0, composition_recipes=1,
                    construction_recipes=1, variants_per_hypothesis=1, dsr_n=29, family_or_template_selection=False,
                    validation_selection=False),
        static_validation=static,
        qualification=dict(native_parse_vm='pending: root --plan-only compile against fields-v7 (u phase)',
                           empirical='unmeasured at source freeze',
                           prior_phase='v1-v6 files and generators are unchanged by v6.1'))
    return {LIBRARY: library_bytes, RECIPE: encode(recipe)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--check', action='store_true')
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
    library = json.loads(docs[LIBRARY])
    print(f"candidates {len(library['candidates'])}: v6 {V6_CANDIDATES} byte-identical + {library['candidates'][-1]['id']} "
          f"{library['candidates'][-1]['dsl']}")


if __name__ == '__main__':
    main()
