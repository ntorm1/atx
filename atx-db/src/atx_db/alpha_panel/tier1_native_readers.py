"""Task17 immutable accounting publications and complete native public readers.

Every admission requires caller-reviewed actual dependencies and validation pins.
No source/economic gate is granted by publication or exact replay alone.
"""
from __future__ import annotations
import contextlib
import datetime as dt
from pathlib import Path
import uuid

from . import common as C, fund_batch_release as F, compustat_std as S, ratios as Q
from . import private_tier1_admission as A, private_tier1_accounting as K

VERSION = A.VERSION
NAMES = ('standardized_v11', 'ratios_v11', 'ea_actuals', 'earnings_calendar_v2')


def freeze(name, spec, con):
    """Source preflight only; root must independently admit the resulting digest."""
    if spec.get('interface_version') != VERSION: raise ValueError('reviewed native interface version required')
    A.guard(con);A.execution(spec)
    if name == 'standardized_v11': return K.standard_sources(spec, con)[0]
    if name == 'ratios_v11': return K.ratio_sources(spec, con)[0]
    if name in ('ea_actuals', 'earnings_calendar_v2'):
        from . import private_tier1_earnings as E
        return E.replay(name, spec, con, source_only=True)
    raise ValueError('unknown native source interface')


def _generation(spec, name):
    lake = A.root(spec); stage = lake/(K.STD if name == 'standardized_v11' else K.RATIOS)
    generation = A.member(lake, spec['generation'])
    if generation.parent != stage/'generations': raise ValueError('explicit immutable v11 accounting generation required')
    return lake, stage, generation


def _manifest(name, spec, generation, source, evidence):
    lake = A.root(spec); stage = generation.parent.parent
    products = ('fundq.parquet', 'funda.parquet', 'facts.parquet') if name == 'standardized_v11' else (
        'fund_state.parquet', 'split_events.parquet', *sorted(p.relative_to(generation).as_posix() for p in generation.glob('monthly/year=*/*.parquet')))
    files = {str((generation/p).relative_to(stage)).replace('\\', '/'): A.pin(generation/p) for p in products}
    manifest = {'stage': stage.name, 'schema': S.SCHEMA if name == 'standardized_v11' else Q.SCHEMA,
                'status': 'complete', 'interface_version': VERSION, 'kind': name,
                'generation': generation.relative_to(stage).as_posix(), 'code': A.code(K.CODE),
                'files': files, 'source_snapshot_sha256': A.digest(source),
                'snapshot': A.pin(generation/'snapshot.json'), 'validation': A.pin(generation/'validation.json'),
                'qualified_fundamental_leaf': source['qualified_fundamental_leaf'],
                'source_quality_certified': False, 'readiness': 'NOT_PRODUCTION_READY',
                'gate_status': {k: 'PASS' if v.get('pass') else 'FAIL' for k,v in evidence.items() if isinstance(v,dict) and 'pass' in v},
                'products': {p: (generation/p).relative_to(stage).as_posix() for p in products},
                'units': {p.parent.name: A.pin(p) for p in sorted((generation/'units').glob('*/admission.json'))},
                'input_manifests_sha256': {str(Path(p).relative_to(lake)).replace('\\', '/'): v['sha256']
                                          for p,v in source['dependencies'].items() if Path(p).is_relative_to(lake) and Path(p).name.endswith('manifest.json')}}
    if name == 'standardized_v11':
        manifest.update(fund_stage=source['fund_stage'], earn_stage=source['earn_stage'],
                        rule=S.RULE, code_version=S.CODE_VERSION, build_date=spec['build_date'],
                        datadate_window=[S.DATADATE_MIN.isoformat(), (dt.date.fromisoformat(spec['build_date'])+dt.timedelta(days=S.DATADATE_MAX_DAYS)).isoformat()])
    else:
        manifest.update(rule=Q.RULE, standardized_generation=source['standardized_generation'],
                        macro_sql=Q.macros_sql(lake, generation.relative_to(lake).as_posix()))
        # Keep the exact native alias dependency needed by existing char admission.
        manifest['input_manifests_sha256'][K.STD] = C.sha256_file(lake/K.STD/'manifest.json')
    return manifest


def _check_snapshot(spec, source):
    if not F._sha(spec.get('snapshot_sha256')) or A.digest(source) != spec['snapshot_sha256']:
        raise ValueError('independent complete native source snapshot digest differs')


def _unit_products(name, identifier):
    return ('fundq.parquet','funda.parquet') if name == 'standardized_v11' else (
        ('fund_state.parquet',) if identifier.startswith('bucket-') else ('ratios_monthly.parquet',))


def _unit_start(generation, identifier, receipt, source):
    if set(receipt)!={'snapshot_sha256','attempt','start','files'}:raise ValueError('unsupported native unit admission protocol')
    attempt = A.member(generation, receipt['attempt'])
    if attempt.parent != generation/'attempts'/identifier or A.pin(attempt/'start.json') != receipt['start'] or C.read_json(attempt/'start.json') != {
            'snapshot_sha256': A.digest(source), 'unit': identifier, 'version': VERSION}:
        raise ValueError('missing at-compute native accounting provenance')


def _alias(stage,generation,expected):
    import os
    source=generation/'manifest.json'
    if A.pin(source)!=expected:raise ValueError('immutable manifest changed before alias activation')
    pending=stage/'manifest.json.partial'
    with pending.open('wb') as handle:
        handle.write(source.read_bytes());handle.flush();os.fsync(handle.fileno())
    if A.pin(pending)!=expected or A.pin(source)!=expected:
        raise ValueError('native manifest bytes changed during alias activation')
    os.replace(pending,stage/'manifest.json')


def _activate_locked(stage, generation, manifest_pin):
    # Companion intent remains until both native pointer and native reader alias
    # agree. Consumers refuse pending intent; original pointer helpers are intact.
    target = {'generation': generation.name, 'manifest': manifest_pin}
    intent = {'version': VERSION, 'current': target,
              'previous': C.read_json(stage/'current.json') if (stage/'current.json').exists() else None}
    F._transaction_target(stage, target)
    C.write_json_atomic(stage/'activation.json', intent)
    F._publish_pointer(stage, generation, manifest_pin)
    _alias(stage,generation,manifest_pin)
    (stage/'activation.json').unlink()


def resolve_current(stage):
    stage = Path(stage)
    if (stage/'activation.json').exists(): raise ValueError('unfinished accounting alias activation')
    generation = F.resolve_current(stage)
    if A.pin(stage/'manifest.json') != A.pin(generation/'manifest.json'):
        raise ValueError('accounting alias and current immutable generation differ')
    return generation


def activate(name, spec, con, rollback=False):
    A.guard(con); A.execution(spec); lake, stage, generation = _generation(spec, name)
    with F.writer_lock(stage):
        if (stage/'activation.json').exists() or (stage/'pointer-transaction.json').exists():
            raise ValueError('unfinished accounting activation; recover first')
        if rollback:
            prior = C.read_json(stage/'previous.json')
            if F._transaction_target(stage, prior) != generation or prior['manifest'] != spec['manifest_pin']:
                raise ValueError('independently pinned rollback generation differs')
        result = validate(name, spec, con)
        _activate_locked(stage, generation, spec['manifest_pin'])
        return result


def recover(name, spec, con):
    A.guard(con); A.execution(spec); lake, stage, generation = _generation(spec, name)
    with F.writer_lock(stage):
        intent = C.read_json(stage/'activation.json')
        if set(intent) != {'version','current','previous'} or intent['version'] != VERSION:
            raise ValueError('unsupported accounting activation intent')
        if F._transaction_target(stage, intent['current']) != generation or intent['current']['manifest'] != spec['manifest_pin']:
            raise ValueError('independently pinned recovery generation differs')
        if intent['previous'] is not None: F._transaction_target(stage, intent['previous'])
        actual = C.read_json(stage/'current.json') if (stage/'current.json').exists() else None
        if actual not in (intent['previous'], intent['current']):
            raise ValueError('unrelated accounting current pointer; interrupted transition cannot replace it')
        if actual == intent['current']:
            previous = C.read_json(stage/'previous.json') if (stage/'previous.json').exists() else None
            if previous != intent['previous']:
                raise ValueError('accounting recovery rollback history differs from authenticated transition')
        alias = A.pin(stage/'manifest.json') if (stage/'manifest.json').exists() else None
        allowed_aliases = [intent['current']['manifest'], None if intent['previous'] is None else intent['previous']['manifest']]
        if alias not in allowed_aliases:
            raise ValueError('unrelated accounting alias during recovery')
        result = validate(name, spec, con)
        transaction = stage/'pointer-transaction.json'
        if transaction.exists():
            tx = C.read_json(transaction)
            if tx != {'version': F.VERSION, 'current': intent['current'], 'previous': intent['previous']}:
                raise ValueError('native accounting pointer transaction differs from intent')
            F._finish_pointer(stage, tx)
        elif not (stage/'current.json').exists() or C.read_json(stage/'current.json') != intent['current']:
            F._publish_pointer(stage, generation, spec['manifest_pin'])
        F._transaction_target(stage, intent['current'])
        _alias(stage,generation,spec['manifest_pin'])
        (stage/'activation.json').unlink()
        return result


def build(name, spec, con, max_new_units=None):
    """New-only bounded accounting units; every continuation replays prior units."""
    if name not in ('standardized_v11', 'ratios_v11'): raise ValueError('Task9 producers remain native and frozen')
    if spec.get('interface_version') != VERSION: raise ValueError('reviewed native interface version required')
    if max_new_units is not None and (type(max_new_units) is not int or max_new_units<0): raise ValueError('invalid new-unit bound')
    A.guard(con); A.execution(spec); lake, stage, generation = _generation(spec, name)
    with F.writer_lock(stage), A.scratch(lake, 'accounting-') as scratch:
        if (stage/'activation.json').exists() or (stage/'pointer-transaction.json').exists(): raise ValueError('unfinished accounting pointer transaction')
        if (generation/'manifest.json').exists() and C.read_json(generation/'manifest.json').get('status') == 'complete':
            raise ValueError('immutable completed accounting generation; use activate for admitted retry')
        if name == 'standardized_v11':
            source, fund, earn, fx, frozen_fund = K.standard_sources(spec, con)
            _check_snapshot(spec, source)
            inp, facts_info = K.standard_inputs(con, spec, source, fund, earn, fx, frozen_fund, scratch)
            identifiers = [f'bucket-{bid:04d}' for bid in range(S.N_BUCKETS)]
            def compute(identifier, dest): K.write_standard_bucket(con, inp, int(identifier.split('-')[1]), dest)
        else:
            source, std = K.ratio_sources(spec, con); _check_snapshot(spec, source)
            Q.build_splits(con, lake); C.copy_to_parquet(con, 'SELECT * FROM spl ORDER BY cik,ex_date', scratch/'split_events.parquet')
            identifiers = [f'bucket-{bid:04d}' for bid in range(Q.N_BUCKETS)]
            identifiers += ['year-'+str(y) for y in sorted({dt.date.fromisoformat(d).year for d in source['sessions']})]
            def compute(identifier, dest):
                if identifier.startswith('bucket-'):
                    Q.build_state_bucket(con, std, f'cik % {Q.N_BUCKETS} = {int(identifier.split("-")[1])}', dest/'fund_state.parquet')
                else:
                    year = int(identifier.split('-')[1])
                    sessions = [dt.date.fromisoformat(d) for d in source['sessions'] if dt.date.fromisoformat(d).year == year]
                    session_sql = 'SELECT unnest(['+','.join("DATE '"+d.isoformat()+"'" for d in sessions)+']) AS session_date'
                    state = A.sql([generation/'units'/f'bucket-{bid:04d}'/'fund_state.parquet' for bid in range(Q.N_BUCKETS)])
                    C.copy_to_parquet(con, Q.consume_sql(Q.cells_sql(lake, session_sql, 'CAST(session_date AS TIMESTAMP)+INTERVAL 22 HOUR', str(year)), state,
                                                      A.sql([scratch/'split_events.parquet']))+' ORDER BY session_date,security_id', dest/'ratios_monthly.parquet', 32768)
        if (generation/'snapshot.json').exists():
            if C.read_json(generation/'snapshot.json') != source: raise ValueError('stale accounting continuation')
        elif generation.exists() and any(generation.iterdir()): raise ValueError('unbound accounting candidate')
        else: F._write_immutable(generation/'snapshot.json', source)
        complete = []; new = 0
        for identifier in identifiers:
            unit = generation/'units'/identifier; receipt = unit/'admission.json'
            if receipt.exists():
                expected = C.read_json(receipt)
                if expected.get('snapshot_sha256') != A.digest(source): raise ValueError('stale accounting unit')
                A.files(unit, expected, _unit_products(name, identifier))
                _unit_start(generation, identifier, expected, source)
                peer = scratch/identifier; peer.mkdir()
                compute(identifier, peer)
                for product in expected['files']: A.compare(con, unit/product, peer/product)
            elif max_new_units is None or new<max_new_units:
                attempt = generation/'attempts'/identifier/('attempt-'+uuid.uuid4().hex); attempt.mkdir(parents=True)
                F._write_immutable(attempt/'start.json', {'snapshot_sha256': A.digest(source), 'unit': identifier, 'version': VERSION})
                A.check_pins(spec['dependency_pins'], source['dependencies']); compute(identifier, attempt)
                peer = scratch/identifier; peer.mkdir(); compute(identifier, peer)
                if {p.name for p in attempt.glob('*.parquet')}!=set(_unit_products(name,identifier)) or {p.name for p in peer.glob('*.parquet')}!=set(_unit_products(name,identifier)):
                    raise ValueError('native accounting kernel produced incomplete/substituted unit products')
                for product in attempt.glob('*.parquet'): A.compare(con, product, peer/product.name)
                A.check_pins(spec['dependency_pins'], source['dependencies'])
                import os
                if unit.exists():
                    # Retain unsealed interrupted publications as forensic attempts;
                    # they never enter the new admitted unit or replace a peer.
                    os.rename(unit, generation/'attempts'/identifier/('unsealed-'+uuid.uuid4().hex))
                unit.mkdir(parents=True, exist_ok=False)
                for product in attempt.glob('*.parquet'): os.link(product, unit/product.name)
                F._write_immutable(receipt, {'snapshot_sha256': A.digest(source), 'attempt': attempt.relative_to(generation).as_posix(),
                                           'start': A.pin(attempt/'start.json'), 'files': {p.name: A.pin(p) for p in unit.glob('*.parquet')}})
                new += 1
            else: continue
            complete.append(identifier)
        A.check_pins(spec['dependency_pins'], source['dependencies'])
        if complete != identifiers: return {'status': 'partial_cache_only', 'published': False, 'missing_units': sorted(set(identifiers)-set(complete))}
        # Preserve any interrupted finalization before reconstructing all outputs.
        final_names = ('fundq.parquet','funda.parquet','facts.parquet','fund_state.parquet',
                       'split_events.parquet','monthly','manifest.json','validation.json')
        interrupted = [generation/p for p in final_names if (generation/p).exists()]
        if interrupted:
            retained = generation/'attempts'/('finalize-'+uuid.uuid4().hex); retained.mkdir(parents=True)
            import os
            for path in interrupted: os.rename(path, retained/path.name)
        if name == 'standardized_v11':
            for product in ('fundq', 'funda'):
                C.copy_to_parquet(con, 'SELECT * FROM '+A.sql([generation/'units'/u/(product+'.parquet') for u in identifiers])+S.ORDER,
                                  generation/(product+'.parquet'), 32768)
            import os
            os.link(scratch/'facts.parquet', generation/'facts.parquet')
            # Native connection-taking gates read this exact source binding.
            C.write_json_atomic(generation/'manifest.json', {'fund_stage': source['fund_stage'], 'earn_stage': source['earn_stage']})
            evidence = K.standard_evidence(con, inp, generation)
            (generation/'manifest.json').unlink()
            evidence['facts'] = facts_info
        else:
            C.copy_to_parquet(con, 'SELECT * FROM '+A.sql([generation/'units'/u/'fund_state.parquet' for u in identifiers if u.startswith('bucket-')])+' ORDER BY cik,available_at', generation/'fund_state.parquet', 32768)
            import os
            os.link(scratch/'split_events.parquet', generation/'split_events.parquet')
            for identifier in identifiers:
                if identifier.startswith('year-'):
                    destination = generation/'monthly'/('year='+identifier.split('-')[1])/'ratios_monthly.parquet'
                    destination.parent.mkdir(parents=True); os.link(generation/'units'/identifier/'ratios_monthly.parquet', destination)
            evidence = K.ratio_evidence(con, lake, generation, std)
        C.write_json_atomic(generation/'validation.json', evidence)
        manifest = _manifest(name, spec, generation, source, evidence)
        F._write_immutable(generation/'manifest.json', manifest)
        admission_spec = {**spec, 'manifest_pin': A.pin(generation/'manifest.json'), 'expected_validation_sha256': A.digest(evidence)}
        # Internal full replay validates new publication; consumer reuse requires
        # independent manifest/validation pins supplied later by root.
        validate(name, admission_spec, con)
        A.check_pins(spec['dependency_pins'], source['dependencies'])
        _activate_locked(stage, generation, A.pin(generation/'manifest.json'))
        return {'status': 'diagnostic_published', 'published': True, 'generation': str(generation), 'source_quality_certified': False}


def validate(name, spec, con):
    A.execution(spec)
    lake, stage, generation = _generation(spec, name)
    manifest = C.read_json(generation/'manifest.json'); before = A.pin(generation/'manifest.json')
    if before != spec['manifest_pin'] or manifest.get('interface_version') != VERSION or manifest.get('kind') != name or manifest.get('status') != 'complete':
        raise ValueError('unsupported/stale immutable accounting publication')
    source = C.read_json(generation/'snapshot.json'); _check_snapshot(spec, source)
    if manifest['snapshot'] != A.pin(generation/'snapshot.json') or manifest['validation'] != A.pin(generation/'validation.json'):
        raise ValueError('accounting source/evidence bytes changed')
    dependencies = A.files(stage, manifest)
    identifiers = [f'bucket-{bid:04d}' for bid in range(S.N_BUCKETS if name == 'standardized_v11' else Q.N_BUCKETS)]
    if name == 'ratios_v11': identifiers += ['year-'+str(y) for y in sorted({dt.date.fromisoformat(d).year for d in source['sessions']})]
    if set(manifest.get('units', {})) != set(identifiers) or {p.name for p in (generation/'units').iterdir()} != set(identifiers):
        raise ValueError('incomplete or substituted accounting unit inventory')
    for identifier in identifiers:
        unit = generation/'units'/identifier; receipt = unit/'admission.json'
        if A.pin(receipt) != manifest['units'][identifier]: raise ValueError('accounting unit admission bytes changed')
        row = C.read_json(receipt)
        attempt = A.member(generation, row['attempt'])
        if A.pin(attempt/'start.json') != row['start'] or C.read_json(attempt/'start.json') != {
                'snapshot_sha256': A.digest(source), 'unit': identifier, 'version': VERSION}:
            raise ValueError('missing at-compute native accounting provenance')
        if row['snapshot_sha256'] != A.digest(source): raise ValueError('stale accounting unit source binding')
        A.files(unit, row, _unit_products(name, identifier))
    # Consumer pins include produced bytes too; source pins already independently
    # bind the complete upstream generations, every actual85 extract and code.
    A.check_pins(spec['dependency_pins'], source['dependencies'])
    with A.scratch(lake, 'accounting-validate-') as scratch:
        if name == 'standardized_v11':
            refreshed, fund, earn, fx, frozen_fund = K.standard_sources(spec, con)
            if refreshed != source: raise ValueError('accounting upstream source snapshot changed')
            inp, info = K.standard_inputs(con, spec, source, fund, earn, fx, frozen_fund, scratch)
            A.compare(con, generation/'facts.parquet', scratch/'facts.parquet')
            for bid in range(S.N_BUCKETS):
                peer = scratch/f'bucket-{bid:04d}'; peer.mkdir(); K.write_standard_bucket(con, inp, bid, peer)
                for product in ('fundq', 'funda'):
                    published = scratch/(product+'-published.parquet')
                    C.copy_to_parquet(con, f'SELECT * FROM {A.sql([generation/(product+".parquet")])} WHERE cik % {S.N_BUCKETS}={bid}', published, 32768)
                    A.compare(con, generation/'units'/f'bucket-{bid:04d}'/(product+'.parquet'), peer/(product+'.parquet'))
                    A.compare(con, published, peer/(product+'.parquet'))
            evidence = K.standard_evidence(con, inp, generation); evidence['facts'] = info
            products = {'fundq': generation/'fundq.parquet', 'funda': generation/'funda.parquet'}
        else:
            refreshed, std = K.ratio_sources(spec, con)
            if refreshed != source: raise ValueError('ratio upstream source snapshot changed')
            Q.build_splits(con, lake); C.copy_to_parquet(con, 'SELECT * FROM spl ORDER BY cik,ex_date', scratch/'split_events.parquet')
            A.compare(con, generation/'split_events.parquet', scratch/'split_events.parquet')
            for bid in range(Q.N_BUCKETS):
                Q.build_state_bucket(con, std, f'cik % {Q.N_BUCKETS}={bid}', scratch/'state.parquet')
                C.copy_to_parquet(con, f'SELECT * FROM {A.sql([generation/"fund_state.parquet"])} WHERE cik % {Q.N_BUCKETS}={bid}', scratch/'state-published.parquet', 32768)
                A.compare(con, generation/'units'/f'bucket-{bid:04d}'/'fund_state.parquet', scratch/'state.parquet')
                A.compare(con, scratch/'state-published.parquet', scratch/'state.parquet')
            for year in sorted({dt.date.fromisoformat(d).year for d in source['sessions']}):
                session_sql = 'SELECT unnest(['+','.join("DATE '"+d+"'" for d in source['sessions'] if dt.date.fromisoformat(d).year==year)+']) AS session_date'
                C.copy_to_parquet(con, Q.consume_sql(Q.cells_sql(lake, session_sql, 'CAST(session_date AS TIMESTAMP)+INTERVAL 22 HOUR', str(year)),
                    A.sql([generation/'fund_state.parquet']), A.sql([generation/'split_events.parquet']))+' ORDER BY session_date,security_id', scratch/'monthly.parquet', 32768)
                A.compare(con, generation/'units'/f'year-{year}'/'ratios_monthly.parquet', scratch/'monthly.parquet')
                A.compare(con, generation/'monthly'/f'year={year}'/'ratios_monthly.parquet', scratch/'monthly.parquet')
            evidence = K.ratio_evidence(con, lake, generation, std)
            products = {'ratios_state': generation/'fund_state.parquet', **{p.relative_to(generation).as_posix(): p for p in generation.glob('monthly/year=*/*.parquet')}}
        if C.read_json(generation/'validation.json') != A.normalize(evidence): raise ValueError('original accounting gate evidence changed')
    if manifest != _manifest(name, spec, generation, source, evidence): raise ValueError('native accounting dependency/manifest contract differs')
    for identifier in identifiers:
        receipt=generation/'units'/identifier/'admission.json'
        if A.pin(receipt)!=manifest['units'][identifier]:raise ValueError('native unit provenance changed during replay')
        row=C.read_json(receipt);_unit_start(generation,identifier,row,source)
        A.files(receipt.parent,row,_unit_products(name,identifier))
    products['qualified_fundamental_leaf'] = Path(source['qualified_fundamental_leaf'])
    A.check_pins(spec['dependency_pins'], source['dependencies'])
    if A.pin(generation/'manifest.json') != before or A.files(stage, manifest) != dependencies:
        raise ValueError('accounting publication changed during full native replay')
    return A.envelope(name, spec, generation, evidence, products, source['dependencies'])


def admit(name, spec, con):
    if name not in NAMES or spec.get('interface_version') != VERSION:
        raise ValueError('explicit fixed reviewed native source interface required')
    A.guard(con); A.execution(spec)
    if name in ('standardized_v11', 'ratios_v11'): return validate(name, spec, con)
    from . import private_tier1_earnings as E
    return E.replay(name, spec, con)


@contextlib.contextmanager
def ratio_cells(spec, con, session_sql, cutoff):
    """Native daily formation cells over a fully independently admitted state.

The caller consumes the yielded SQL inside this scope. Its reviewed session SQL
and cutoff expression are the unchanged native cells_sql arguments. Each scope
rechecks actual source, generation and code pins after consumption.
"""
    result=admit('ratios_v11',spec,con)
    lake,stage,generation=_generation(spec,'ratios_v11')
    source=C.read_json(generation/'snapshot.json')
    before=A.files(stage,C.read_json(generation/'manifest.json'))
    A.check_pins(spec['dependency_pins'],source['dependencies'])
    query=_admitted_ratio_sql(result,session_sql,cutoff)
    try: yield query
    finally:
        A.check_pins(spec['dependency_pins'],source['dependencies'])
        if A.pin(generation/'manifest.json')!=spec['manifest_pin'] or A.files(stage,C.read_json(generation/'manifest.json'))!=before:
            raise ValueError('native daily ratio inputs changed during formation')


def _admitted_ratio_sql(admission, session_sql, cutoff):
    """Project unchanged formulas inside an already authenticated transaction.

    This is SQL construction, not admission. Golden's complete native admit_plan
    and consumed source inventory checks surround its entire build/replay, so
    reconstructing each unit does not repeat full financial admission. The
    standalone ratio_cells scope above retains its own complete admission.
    """
    spec=admission['spec']
    lake,stage,generation=_generation(spec,'ratios_v11')
    if admission['status']!='admitted_diagnostic' or Path(admission['products']['ratios_state']).resolve()!=generation/'fund_state.parquet':
        raise ValueError('fixed admitted native ratio state required')
    return _ratio_projection(lake,generation,session_sql,cutoff)


def _ratio_projection(lake, generation, session_sql, cutoff):
    """Pure unchanged native SQL construction; never authenticates sources."""
    return Q.consume_sql(Q.cells_sql(lake,session_sql,cutoff),
        A.sql([generation/'fund_state.parquet']),A.sql([generation/'split_events.parquet']))


def main(argv=None):
    import argparse,json
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=('contract','freeze','build','validate','activate','rollback','recover','audit'))
    parser.add_argument('--name',choices=NAMES)
    parser.add_argument('--contract',type=Path)
    parser.add_argument('--max-new-units',type=int)
    args=parser.parse_args(argv)
    if args.command=='contract':
        print(json.dumps({'execution':A.EXECUTION,'adapter_code':A.code(A.ADAPTER_CODE),'adapter_code_sha256':A.digest(A.code(A.ADAPTER_CODE))},indent=2));return
    spec=C.read_json(args.contract);lake=A.root(spec)
    with C.connect(memory='128MB',threads=1,temp_dir=lake/'_tmp'/('tier1-reader-'+uuid.uuid4().hex),db_file='reader.duckdb') as con:
        A.guard(con)
        if args.command=='freeze':result=freeze(args.name,spec,con)
        elif args.command=='build':result=build(args.name,spec,con,args.max_new_units)
        elif args.command=='validate':result=admit(args.name,spec,con)
        elif args.command=='recover':result=recover(args.name,spec,con)
        elif args.command=='audit':
            from . import private_tier1_earnings as E
            result=E.publish_audit(spec,spec['audit_standardized'],con,spec['expected_audit_sources_sha256'])
        else:result=activate(args.name,spec,con,rollback=args.command=='rollback')
    print(json.dumps(result,default=str,indent=2))

if __name__=='__main__':main()
