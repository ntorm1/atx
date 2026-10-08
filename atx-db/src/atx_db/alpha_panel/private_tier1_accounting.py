"""Connection-taking native accounting kernels with explicit admitted dependencies."""
from __future__ import annotations
import datetime as dt
from pathlib import Path
import random

from . import common as C, compustat_std as S, ratios as Q
from . import fund_batch_release as F, fund_release_runtime as R
from . import private_tier1_admission as A

STD = 'compustat_std_v11'
RATIOS = 'ratios_v11'
CODE = ('tier1_native_readers', 'private_tier1_admission', 'private_tier1_accounting',
        'private_tier1_earnings', 'compustat_std', 'ratios', 'fund_catalog', 'fund_items', 'fund_fx', 'common')


def fundamental(spec):
    """Full immutable public admission uses the separately pinned R23 connection."""
    lake = A.root(spec); item = spec['fundamentals']; stage = lake/F.STAGE
    generation = A.member(lake, item['generation'])
    if generation.parent != stage/'generations' or A.pin(generation/'manifest.json') != item['manifest_pin']:
        raise ValueError('explicit independently pinned final fundamental generation required')
    runtime = R.load_runtime(item['execution_contract'], item['expected_contract_sha256'],
                             item['expected_runtime_code_sha256'])
    with R.connection(stage, runtime) as (con, resources), F.writer_lock(stage):
        proof = R.check_proof(stage, generation, runtime, item['snapshot_sha256'])
        manifest = F.validate_generation(stage, generation, con, item['extract_admission'],
                                         item['expected_admission_sha256'], True)
        R._recheck_proof(stage, generation, runtime, item['snapshot_sha256'], proof)
    if A.digest(manifest) != item['expected_validation_sha256']:
        raise ValueError('independent final fundamental validation digest differs')
    snapshot = F.read(generation/'snapshot.json')
    if F.digest(snapshot) != item['snapshot_sha256']:
        raise ValueError('independently pinned canonical fundamental snapshot differs')
    leaf = generation/F.SEMANTIC_STAGE
    dependencies=financial_dependencies(stage,generation,manifest,snapshot,runtime,proof)
    return leaf, snapshot, dependencies


def financial_dependencies(stage,generation,manifest,snapshot,runtime,proof):
    """Actual byte inventory obtained only after complete native admission."""
    leaf=generation/F.SEMANTIC_STAGE
    dependencies = {str(generation/'manifest.json'): A.pin(generation/'manifest.json'),
                    str(generation/'snapshot.json'): A.pin(generation/'snapshot.json'),
                    str(R.proof_path(stage, generation)): proof,
                    str(Path(runtime['contract_path'])): runtime['contract_identity'], **runtime['code'], **snapshot['code']}
    dependencies.update(A.files(generation, manifest))
    dependencies[str(leaf/'manifest.json')] = manifest['native_manifest']
    dependencies[str(snapshot['binding_path'])] = snapshot['binding_identity']
    preparation=snapshot['preparation'];original=Path(preparation['source']['root'])
    dependencies[str(original/'fsds-staging-manifest.json')]=preparation['source']['manifest']
    dependencies.update({str(A.member(original,relative)):value for relative,value in preparation['source']['files'].items()})
    dependencies[str(Path(snapshot['binding']['prep_root'])/'prepare.json')]=snapshot['prepare_summary']
    if len(preparation['units'])!=F.PREPARATION_UNITS:raise ValueError('complete original144 preparation units required')
    for path,unit in preparation['units'].items():
        target=A.absolute(path);side=target.with_name(target.name+'.prepare.json')
        if C.read_json(side)!=unit:raise ValueError('original preparation sidecar differs')
        dependencies[str(target)]=unit['output'];dependencies[str(side)]=A.pin(side)
        dependencies.update(unit['binding']['dependencies'])
    for product in snapshot['products'].values():
        if product is not None: dependencies.update(product['files'])
    extract = Path(snapshot['binding']['extract_root']).resolve()
    # The selected list is derived from the fully validated frozen native85,
    # never accepted as an arbitrary caller-selected directory or subset.
    if set(snapshot['extract']['files']) != {'plan.json'} | {
            f'batch-{i:04d}'+suffix for i in F.REQUIRED for suffix in ('.parquet', '.stats.parquet', '.json')}:
        raise ValueError('missing exact complete85 extract inventory')
    dependencies.update({str(extract/p): v for p, v in snapshot['extract']['files'].items()})
    dependencies[snapshot['binding']['archive']['path']] = {k: snapshot['binding']['archive'][k] for k in ('sha256', 'bytes')}
    admission = snapshot['extract_admission']
    dependencies[admission['path']] = admission['identity']
    dependencies.update({v['path']: v['identity'] for v in admission['document']['receipts'].values()})
    return dependencies


def standard_sources(spec, con):
    lake = A.root(spec); leaf, frozen_fund, dependencies = fundamental(spec)
    from . import tier1_native_readers as T
    earnings = T.admit('earnings_calendar_v2', spec['earnings'], con)
    announcement = earnings['products']['announcements.parquet']
    dependencies.update(spec['earnings']['dependency_pins'])
    if Path(frozen_fund['binding']['fsds_root']).resolve() != C.FSDS_DIR.resolve():
        raise ValueError('native standardized FSDS reader differs from admitted financial snapshot')
    for pattern in ('identity/link_table_v3.parquet', '_tmp/panel_member/year=*.parquet'):
        dependencies.update(A.selected(lake, pattern))
    if {str(lake/'_tmp/panel_member'/f'year={year}.parquet') for year in S.GATE_YEARS} - set(dependencies):
        raise ValueError('all original standardized gate years required')
    dependencies.update(A.selected(C.FSDS_DIR, 'num/*.parquet'))
    dependencies.update(A.selected(C.FSDS_DIR, 'sub/*.parquet'))
    fx = frozen_fund['products']['fx']
    fx_path = None if fx is None else Path(next(iter(fx['files'])))
    # Native standardized FX loading has no path parameter. Load these exact
    # admitted rows directly and pass them through its unchanged Inputs object.
    fx_rows = [] if fx_path is None else [tuple(r[c] for c in ('obs_date', 'currency', 'usd_per_ccy', 'available_at'))
                                      for r in __import__('pyarrow.parquet', fromlist=['read_table']).read_table(fx_path).to_pylist()]
    dependencies.update(A.code(CODE));dependencies.update(A.execution(spec))
    A.check_pins(spec['dependency_pins'], dependencies)
    result = {'version': A.VERSION, 'kind': 'standardized_v11', 'fund_stage': leaf.relative_to(lake).as_posix(),
              'earn_stage': announcement.parent.relative_to(lake).as_posix(),
              'fundamental_snapshot_sha256': F.digest(frozen_fund),
              'dependencies': dependencies, 'build_date': spec['build_date'], 'buckets': S.N_BUCKETS,
              'qualified_fundamental_leaf': str(leaf)}
    return result, leaf, announcement.parent, fx_rows, frozen_fund


def prepare_facts(con, fund, extract, dest, dmax):
    """Identical native preparation SQL, with only its actual85 reader supplied.

Native prepare_facts hardcodes finalleaf/_work/cf; an immutable financial leaf
cannot gain that directory. Source list comes only from full native admission.
"""
    con.execute('CREATE OR REPLACE TEMP TABLE fact_spec (taxonomy VARCHAR, concept VARCHAR, key VARCHAR, rnk INTEGER, ukind VARCHAR)')
    con.executemany('INSERT INTO fact_spec VALUES (?, ?, ?, ?, ?)', S.fact_spec_rows())
    source = A.sql([extract/f'batch-{i:04d}.parquet' for i in F.REQUIRED])
    con.execute(f"""CREATE OR REPLACE TEMP TABLE fraw AS
        SELECT CAST(b.cik AS BIGINT) AS cik, s.key, s.rnk, s.ukind, b.unit, b.period_start, b.period_end,
               datediff('day', b.period_start, b.period_end) AS dur, b.value, b.accession_number AS accession
        FROM {source} b JOIN fact_spec s ON s.taxonomy=b.taxonomy AND s.concept=b.concept
        WHERE b.period_start IS NOT NULL AND b.value IS NOT NULL
          AND datediff('day', b.period_start, b.period_end) BETWEEN 1 AND 450
          AND ((s.ukind='shares' AND b.unit='shares') OR (s.ukind='per_share' AND b.unit LIKE '%/shares')
               OR (s.ukind='money' AND b.unit NOT LIKE '%/%' AND b.unit<>'shares'))""")
    n_raw, n_out = con.execute(f"SELECT count(*),count(*) FILTER(WHERE period_end<DATE '{S.DATADATE_MIN}' OR period_end>DATE '{dmax}') FROM fraw").fetchone()
    con.execute(f"""CREATE OR REPLACE TEMP TABLE fdedup AS
        SELECT cik,key,rnk,ukind,unit,period_start,period_end,dur,accession,max(value) AS value,count(DISTINCT value) AS n_values
        FROM fraw WHERE period_end BETWEEN DATE '{S.DATADATE_MIN}' AND DATE '{dmax}' GROUP BY ALL""")
    conflicts = con.execute('SELECT count(*) FROM fdedup WHERE n_values>1').fetchone()[0]
    n = C.copy_to_parquet(con, f"""SELECT f.cik,f.key,f.rnk,f.ukind,f.unit,f.period_start,f.period_end,f.dur,f.value,f.accession,k.clock_utc
        FROM fdedup f JOIN (SELECT DISTINCT cik,accession,clock_utc FROM {A.sql([fund/'events.parquet'])}) k
        ON k.cik=f.cik AND k.accession=f.accession ORDER BY f.cik,f.key,f.period_end,f.accession""", dest)
    con.execute('DROP TABLE fraw'); con.execute('DROP TABLE fdedup')
    return {'source': extract.as_posix(), 'batches': 85, 'rows_read': int(n_raw), 'excluded_outside_datadate_window': int(n_out),
            'conflicting_duplicates_max_kept': int(conflicts), 'rows': n}


def standard_inputs(con, spec, source, fund, earn, fx_rows, frozen_fund, scratch):
    dmax = dt.date.fromisoformat(spec['build_date'])+dt.timedelta(days=S.DATADATE_MAX_DAYS)
    facts = scratch/'facts.parquet'
    info = prepare_facts(con, fund, Path(frozen_fund['binding']['extract_root']), facts, dmax)
    return S.Inputs(fund, earn, facts if info['rows'] else None, fx_rows, dmax), info


def write_standard_bucket(con, inp, bid, dest):
    S.derive_subset(con, inp, f'cik % {S.N_BUCKETS} = {bid}')
    for product, relation in (('fundq', 'q_out'), ('funda', 'a_out')):
        C.copy_to_parquet(con, 'SELECT * FROM '+relation, dest/(product+'.parquet'), 32768)


def cutoff(con, inp, out):
    """Original seeded member/all/FX pools, cutoffs and native normalized cells."""
    S.member_ciks(con)
    member = [r[0] for r in con.execute('SELECT DISTINCT cik FROM mem ORDER BY 1').fetchall()]
    all_ = [r[0] for r in con.execute('SELECT DISTINCT cik FROM '+A.sql([out/'fundq.parquet'])+' ORDER BY 1').fetchall()]
    fx = [r[0] for r in con.execute('SELECT DISTINCT cik FROM '+A.sql([out/'fundq.parquet'])+' WHERE fx_converted ORDER BY 1').fetchall()]
    rng = random.Random(S.CUTOFF_SEED)
    ciks = sorted(set(rng.sample(member, min(S.CUTOFF_CIKS//2, len(member)))+
                      rng.sample(all_, min(S.CUTOFF_CIKS-S.CUTOFF_CIKS//2, len(all_)))+
                      rng.sample(fx, min(S.CUTOFF_FX_CIKS, len(fx)))))
    result = {'seed': S.CUTOFF_SEED, 'cutoffs': [c.isoformat() for c in S.CUTOFFS],
              'compared_rows': 0, 'compared_cells': 0, 'differences': 0, 'row_set_differences': 0,
              'examples': [], 'ciks': len(ciks), 'fx_converted_ciks': len(set(ciks)&set(fx))}
    if ciks:
        where = 'cik IN ('+', '.join(map(str, ciks))+')'
        for cut in S.CUTOFFS:
            S.derive_subset(con, inp, where, cut)
            for name, relation in (('fundq', 'q_out'), ('funda', 'a_out')):
                pub = con.execute(f'SELECT * FROM {A.sql([out/(name+".parquet")])} WHERE {where} AND available_at<{S._ts(cut)} {S.ORDER}').fetchall()
                cols = [d[0] for d in con.description]
                reb = con.execute(f'SELECT * FROM {relation} {S.ORDER}').fetchall()
                if cols != [d[0] for d in con.description]:
                    result['differences'] += 1
                    result['examples'].append({'cutoff': cut.isoformat(), 'table': name, 'columns_differ': True})
                    continue
                def key(row): return tuple(row[cols.index(c)] for c in ('cik', 'datadate', 'accession', 'available_at'))
                left, right = {key(r): r for r in pub}, {key(r): r for r in reb}
                result['row_set_differences'] += len(set(left)^set(right))
                if set(left) != set(right) and len(result['examples']) < 10:
                    result['examples'].append({'cutoff': cut.isoformat(), 'table': name,
                        'only_published': [list(k) for k in sorted(set(left)-set(right))[:3]],
                        'only_rebuilt': [list(k) for k in sorted(set(right)-set(left))[:3]]})
                for k in set(left)&set(right):
                    result['compared_rows'] += 1; result['compared_cells'] += len(cols)
                    for i, column in enumerate(cols):
                        if S._norm(left[k][i]) != S._norm(right[k][i]):
                            result['differences'] += 1
                            if len(result['examples']) < 10:
                                result['examples'].append({'cutoff': cut.isoformat(), 'table': name, 'key': list(k),
                                    'column': column, 'published': str(left[k][i]), 'rebuilt': str(right[k][i])})
    result['pass'] = result['differences']==0 and result['row_set_differences']==0 and result['compared_rows']>0
    return result


def standard_evidence(con, inp, out):
    S.member_ciks(con)
    return A.normalize({'coverage': S.coverage_gate(con, out), 'rdq': S.rdq_gate(con, out, inp.earn),
                        'sum4': S.sum4_gate(con, out), 'balance': S.balance_gate(con, out),
                        'fsds': S.fsds_gate(con, out), 'cutoff': cutoff(con, inp, out),
                        'handcheck': {'pass': False, 'status': 'FAIL_PENDING_ORIGINAL_30_SOURCE_HANDCHECKS'}})


def ratio_sources(spec, con):
    lake = A.root(spec)
    from . import tier1_native_readers as T
    std = T.admit('standardized_v11', spec['standardized'], con)
    dependencies = dict(spec['standardized']['dependency_pins'])
    alias = lake/STD/'manifest.json'
    if A.pin(alias) != spec['standardized']['manifest_pin']:
        raise ValueError('native standardized alias differs from independently admitted immutable generation')
    dependencies[str(alias)] = A.pin(alias)
    for pattern in ('corporate_actions/vendor_events.parquet', 'identity/link_table_v3.parquet',
                    'calendar.parquet', 'prices/year=*/prices.parquet', 'market/shares_daily/year=*/shares_daily.parquet',
                    '_tmp/panel_member/year=*.parquet', 'panel/year=*/*.parquet', 'universe/year=*/*.parquet'):
        dependencies.update(A.selected(lake, pattern))
    for pattern in ('security_master/finra_names.parquet', 'universe/manifest.json'):
        dependencies.update(A.selected(lake, pattern, required=False))
    dependencies.update(A.code(CODE));dependencies.update(A.execution(spec))
    A.check_pins(spec['dependency_pins'], dependencies)
    sessions = Q.month_end_sessions(con, lake)
    if not sessions: raise ValueError('native monthly session population is empty')
    frozen = {'version': A.VERSION, 'kind': 'ratios_v11', 'dependencies': dependencies,
              'sessions': [d.isoformat() for d in sessions], 'buckets': Q.N_BUCKETS,
              'standardized_generation': str(Path(std['products']['fundq']).parent),
              'qualified_fundamental_leaf': str(std['products']['qualified_fundamental_leaf'])}
    return frozen, Path(std['products']['fundq']).parent


def ratio_evidence(con, lake, out, std):
    Q.member_cells(con, lake, out)
    basis = {str(y): {'member_cells': n, 'linked': l, 'with_me': m} for y,n,l,m in con.execute(
        'SELECT year,count(*),count(cik),count(me) FROM mcells GROUP BY 1 ORDER BY 1').fetchall()}
    masks = Q.structural_masks(con, std)
    evidence = {'basis': basis, 'coverage': Q.coverage_gate(con, masks),
                'spearman': Q.spearman_gate(con, lake), 'ranges': Q.ranges_gate(con),
                'handcheck': {'pass': False, 'status': 'FAIL_PENDING_ORIGINAL_SOURCE_HANDCHECKS'}}
    for population in ('univ_all_common', 'univ_r3000p'):
        if not Q.universe_cells(con, lake, out.relative_to(lake).as_posix(), population):
            raise ValueError('missing original ratio universe denominator')
        evidence['coverage_'+population] = Q.coverage_gate(con, masks, basis=population)
    return A.normalize(evidence)
