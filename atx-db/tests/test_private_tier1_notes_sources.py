"""Synthetic original source replay/equivalence and R12 conditional-cell tests."""
import ast
import datetime as dt
import inspect
import json
from pathlib import Path

import duckdb
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import private_tier1_notes_sources as P
from atx_db.alpha_panel import notes_fetch as S, fund_notes as N, golden_sources as G
from tests.test_alpha_panel_notes import fixture_tables, write_zip


def connection(root):
    con=duckdb.connect(str(root/'notes.duckdb'))
    con.execute("SET memory_limit='128MB'; SET threads=1; SET TimeZone='UTC'")
    return con


def source_parts(root):
    """Tiny independently constructed original source products, no producer build."""
    tables=fixture_tables()
    original=write_zip(root/'2021q1_notes.zip',tables)
    parts=root/'parts'/'source=2021q1'
    parts.mkdir(parents=True)
    raw={}
    for table in ('sub','dim','txt','num'):
        raw[table]=pa.RecordBatch.from_arrays([pa.array([row[i] or None for row in tables[table]],type=pa.string()) for i in range(len(S.COLUMNS[table]))],names=list(S.COLUMNS[table]))
    sub=raw['sub']; naive,utc=S.accepted_clocks(sub.column('accepted').to_pylist())
    values=S.cast_batch(sub.drop_columns(['accepted']),'sub',{})
    values.update(accepted_et_naive=pa.array(naive,pa.timestamp('us')),accepted_utc=pa.array(utc,pa.timestamp('us',tz='UTC')),source=pa.array(['2021q1']*sub.num_rows))
    schema=S.out_schema('sub',(('accepted_et_naive',pa.timestamp('us')),('accepted_utc',pa.timestamp('us',tz='UTC'))))
    pq.write_table(pa.table([values[f.name] for f in schema],schema=schema),parts/'sub.parquet')
    periodic=pa.array([row[0] for row in tables['sub'] if row[list(S.COLUMNS['sub']).index('form')] in S.PERIODIC_FORMS])
    dim=raw['dim']
    segmented=pc.filter(dim.column('dimhash'),pc.fill_null(pc.match_substring_regex(dim.column('segments'),'(^|;)('+'|'.join(S.SEGMENT_AXES)+')='),False))
    selected={}
    selected['txt_dei']=raw['txt'].filter(pc.fill_null(pc.starts_with(raw['txt'].column('version'),'dei/'),False))
    num=raw['num']
    selected['num_dei']=num.filter(pc.fill_null(pc.starts_with(num.column('version'),'dei/'),False))
    standard=pc.and_(pc.not_equal(num.column('version'),num.column('adsh')),pc.match_substring_regex(num.column('tag'),S.ITEM_TAG_RE))
    mask=pc.and_(pc.is_in(num.column('adsh'),value_set=periodic),pc.or_(pc.fill_null(standard,False),pc.is_in(num.column('dimh'),value_set=segmented)))
    selected['num_items']=num.filter(pc.fill_null(mask,False))
    used=pa.array(sorted({v for batch in selected.values() for v in batch.column('dimh').to_pylist() if v and v!=S.NO_DIM}))
    selected['dim']=dim.filter(pc.is_in(dim.column('dimhash'),value_set=used))
    for name,batch in selected.items():
        table='txt' if name=='txt_dei' else 'dim' if name=='dim' else 'num'
        values=S.cast_batch(batch,table,{})
        values['source']=pa.array(['2021q1']*batch.num_rows)
        schema=S.out_schema(table)
        pq.write_table(pa.table([values[f.name] for f in schema],schema=schema),parts/(name+'.parquet'))
    return original,parts


def test_source_replay_item_original_sql_equivalence_and_tamper(tmp_path):
    original,parts=source_parts(tmp_path)
    with connection(tmp_path) as con:
        P.F._guard(con,tmp_path)
        proof=P.replay_source(con,'2021q1',original,parts)
        assert all(v['rows_plus_invalid_equal_lines_minus_header'] for v in proof['members'].values())
        # Read only the original projection expression as oracle. No whole
        # producer clone/invocation, monkeypatch, resource change or deletion.
        tree=ast.parse(inspect.getsource(N.build))
        expression=next(node.value for node in ast.walk(tree) if isinstance(node,ast.Assign) and any(isinstance(target,ast.Name) and target.id=='sql' for target in node.targets))
        scope={**N.__dict__,'ax_b':N.AXES['business'],'ax_g':N.AXES['geographic'],'ax_c':N.CUSTOMER_AXIS}
        oracle=eval(compile(ast.Expression(expression),'<unchanged native item projection>','eval'),scope)
        expressions=[node.args[0] for node in ast.walk(tree) if isinstance(node,ast.Call) and isinstance(node.func,ast.Attribute) and node.func.attr=='execute' and node.args and isinstance(node.args[0],ast.JoinedStr)]
        def original_relation(marker,extra):
            expression=next(expr for expr in expressions if any(isinstance(value,ast.Constant) and marker in str(value.value) for value in expr.values))
            statement=eval(compile(ast.Expression(expression),'<unchanged native source ordering/fact key>','eval'),{**scope,**extra})
            return __import__('re').sub(r'(?is)^\s*CREATE (?:OR REPLACE )?(?:TEMP )?TABLE\s+\w+\s+AS\s+','',statement)
        for year,query in P.item_sql(con,tmp_path/'parts',[2021,2022]):
            sub_oracle=original_relation('CREATE TABLE sub AS',{'g_sub':(tmp_path/'parts/source=*/sub.parquet').as_posix(),'per':', '.join(repr(f) for f in S.PERIODIC_FORMS)})
            source=tmp_path/'native-sub-oracle.parquet'
            P.C.copy_to_parquet(con,sub_oracle,source,row_group_size=32)
            P._compare(con,'SELECT * FROM sub',source)
            extra={'y':year,'g_num':[(parts/'num_items.parquet').as_posix()],'g_dim':[(parts/'dim.parquet').as_posix()]}
            for marker,table in (('CREATE OR REPLACE TABLE ny AS','ny'),('CREATE OR REPLACE TABLE dimy AS','dimy')):
                source=tmp_path/(table+'-oracle.parquet')
                P.C.copy_to_parquet(con,original_relation(marker,extra),source,row_group_size=32)
                P._compare(con,'SELECT * FROM '+table,source)
            target=tmp_path/f'items-{year}.parquet'
            P.C.copy_to_parquet(con,oracle,target,row_group_size=32)
            P._compare(con,query,target)
            assert con.execute('SELECT count(*) FROM ('+query+')').fetchone()[0]>0
        table=pq.ParquetFile(parts/'num_items.parquet').read()
        data=table.to_pylist()
        data[0]['value']=999999
        pq.write_table(pa.Table.from_pylist(data,schema=table.schema),parts/'num_items.parquet')
        with pytest.raises(ValueError,match='duplicate-preserving native notes replay'):
            P.replay_source(con,'2021q1',original,parts)


def test_r12_positive_count_hhi_and_missing_context_exclusions(tmp_path):
    # Canonical nine identity is required; synthetic proof documents below are
    # only unit inputs and cannot pass full original-body/source-plan admission.
    accession,original=next(iter(G.curated_notes().items()))
    period='2024-12-31'
    proof={'corroboration':{k:original[k] for k in ('primary_sha256','role','concept','members','basis')},'external_revenue_selection':{}}
    inventory={accession:{**original,'period':period,'proof_sha256':G.digest(proof)}}
    clock=dt.datetime(2025,2,1,tzinfo=dt.timezone.utc)
    row={'bseg_partition_source_adsh':accession,'source_accession':accession,'source_sha256':original['primary_sha256'],'period':period,'available_at':clock,'field':'bseg_n'}
    assert G.notes_admission(row,proof,inventory,clock+dt.timedelta(days=1))==(len(original['members']),'curated_reportable_count')
    row['field']='bseg_rev_hhi'
    assert G.notes_admission(row,proof,inventory,clock+dt.timedelta(days=1))[0] is None
    context={'basis':'reported_segment_measure','actual_context_ids':['a','b','c']}
    inventory[accession]['revenue_context_admission']=context
    row.update(revenue_context_admission=context,member_values=[float(i+1) for i in range(len(original['members']))],total=sum(range(1,len(original['members'])+1)),bridge=0)
    assert G.notes_admission(row,proof,inventory,clock+dt.timedelta(days=1))[0]>0
    row['bridge']=5
    assert G.notes_admission(row,proof,inventory,clock+dt.timedelta(days=1))[0] is None
    assert G.notes_admission(row,proof,inventory,clock)[1]=='not_available'
    applied={**proof,'selected_external_revenue':{}}
    assert G.notes_admission(row,applied,inventory,clock+dt.timedelta(days=1))[1]=='automatic_external_selection_withheld'


def test_original_context_units_period_entity_and_values(tmp_path):
    primary=b'''<html><xbrli:context id="total"><xbrli:entity><xbrli:identifier scheme="CIK">1</xbrli:identifier></xbrli:entity><xbrli:period><xbrli:startDate>2024-01-01</xbrli:startDate><xbrli:endDate>2024-12-31</xbrli:endDate></xbrli:period></xbrli:context><xbrli:unit id="usd"><xbrli:measure>iso4217:USD</xbrli:measure></xbrli:unit><ix:nonFraction name="us-gaap:Revenues" contextRef="total" unitRef="usd" scale="3">100</ix:nonFraction></html>'''
    assert P._original_context_values(primary,'us-gaap_Revenues','2024-12-31','USD',1)=={'total':(None,100000.)}
    assert P._original_context_values(primary,'us-gaap_Revenues','2023-12-31','USD',1)=={}
    assert P._original_context_values(primary,'us-gaap_Revenues','2024-12-31','EUR',1)=={}
    assert P._original_context_values(primary,'us-gaap_Revenues','2024-12-31','USD',2)=={}
    assert P._original_context_values(primary.replace(b'scale="3"',b'format="unreviewed"'),'us-gaap_Revenues','2024-12-31','USD',1)=={}


def test_full_native_replay_mapping_publication_and_source_drift(tmp_path,monkeypatch):
    # Literal native proof path resolves only inside this retained fixture.
    # No native function or resource option is patched.
    monkeypatch.chdir(tmp_path)
    original,parts=source_parts(tmp_path)
    raw=tmp_path/'data/raw/sec_notes_t14_pilot'
    raw.mkdir(parents=True)
    (raw/'receipts.jsonl').write_text('')
    native=tmp_path/'native'
    native.mkdir()
    products={}
    with connection(tmp_path) as con:
        P.F._guard(con,tmp_path)
        first=True
        emitted={k:[] for k in ('segments','recon','wide','features')}
        for year,sql in P.item_sql(con,tmp_path/'parts',[2021,2022]):
            if first:
                con.execute('CREATE TEMP TABLE business_census(adsh VARCHAR,member VARCHAR)')
                N.collect_business_census(con,tmp_path/'parts',[2021,2022])
                con.execute('CREATE TEMP TABLE business_census_original AS SELECT * FROM business_census')
                con.execute('CREATE TEMP TABLE structure_evidence(adsh VARCHAR,tag VARCHAR,proof_json VARCHAR)')
                first=False
            item=native/f'items/year={year}/items.parquet'
            P._copy(con,sql,item)
            products[f'items/year={year}/items.parquet']=item
            con.execute(f"CREATE OR REPLACE TEMP TABLE cur AS SELECT adsh,arg_max(uom,n) currency FROM(SELECT adsh,uom,count(*) n FROM read_parquet('{item.as_posix()}') WHERE uom NOT IN('shares','pure') AND regexp_full_match(uom,'[A-Z]{{3}}') GROUP BY 1,2) GROUP BY 1")
            seg=P._copy(con,N.segments_sql(item.as_posix()),tmp_path/f'seg-{year}.parquet')
            strict=P._copy(con,N.recon_sql(item.as_posix(),seg,year),tmp_path/f'strict-{year}.parquet')
            con.register('recon_ov',N.recon_overlay(con,item.as_posix(),seg,strict))
            recon=P._copy(con,N.recon_final_sql(strict),tmp_path/f'recon-{year}.parquet')
            con.unregister('recon_ov')
            wide=P._copy(con,N.wide_sql(item.as_posix()),tmp_path/f'wide-{year}.parquet')
            features=P._copy(con,N.features_sql(item.as_posix(),seg,wide,recon),tmp_path/f'features-{year}.parquet')
            for name,path in (('segments',seg),('recon',recon),('wide',wide),('features',features)):
                emitted[name].append(Path(path))
        for name,target in (('segments','segments.parquet'),('recon','segment_recon.parquet'),('wide','notes_wide.parquet')):
            products[target]=native/target
            P._copy(con,P._concat(emitted[name]),products[target])
        feature_raw=tmp_path/'features-raw.parquet'
        P._copy(con,P._concat(emitted['features']),feature_raw)
        products['segment_features.parquet']=native/'segment_features.parquet'
        P._copy(con,N.carry_sql(feature_raw.as_posix()),products['segment_features.parquet'])
        products['segment_census.parquet']=native/'segment_census.parquet'
        P._copy(con,"SELECT s.cik,s.adsh,s.form,s.period,s.filed,s.available_at,count(DISTINCT b.member) n_members,count(DISTINCT b.member)>=2 multi_segment,count(DISTINCT r.member) n_members_source_tagged,CASE WHEN max(e.adsh) IS NULL THEN 'current_period_numeric_tags' ELSE 'source_role_structure' END member_basis FROM sub s LEFT JOIN business_census b USING(adsh) LEFT JOIN business_census_original r USING(adsh) LEFT JOIN structure_evidence e USING(adsh) GROUP BY ALL",products['segment_census.parquet'])
        products['segment_structure.parquet']=native/'segment_structure.parquet'
        P._copy(con,'SELECT e.*,s.cik,s.period,s.filed,s.available_at,s.form FROM structure_evidence e JOIN(SELECT DISTINCT adsh,cik,period,filed,available_at,form FROM sub) s USING(adsh)',products['segment_structure.parquet'])
        for table in reversed(P.TABLES):
            if table!='recon_ov':con.execute('DROP TABLE IF EXISTS '+table)
        def jsonfile(name,value):
            path=tmp_path/name
            path.write_text(json.dumps(value,default=str))
            return path
        pin=P.F.pin(original)
        url='https://www.sec.gov/files/dera/data/financial-statement-notes-data-sets/2021q1_notes.zip'
        transport=jsonfile('transport.json',{'url':url,**pin,'status':'parsed','http_status':200})
        source_manifest=jsonfile('notes-source-manifest.json',{'data_sets':{'2021q1':{'url':url,**pin}}})
        curated=jsonfile('curated.json',G.curated_notes())
        (native/'manifest.json').write_text(json.dumps({'schema':N.SCHEMA,'gate_status':{'segments':{'status':'FAIL'},'reconciliation':{'status':'FAIL'}}}))
        plan={'schema':'atx.tier1-notes-source-plan/v1','source_root':str(tmp_path),'source_manifest':str(source_manifest),'source_receipts':str(transport),
              'rawroot':str(raw),'raw_receipts':str(raw/'receipts.jsonl'),'curated_inventory':str(curated),'years':[2021,2022],
              'archives':{'2021q1':{'original':str(original),'receipt':str(transport),'parts':{name:P.F.pin(parts/name) for name in P.SOURCE_FILES}}},
              'native_products':{name:P.F.pin(path) for name,path in products.items()}}
        plan_path=jsonfile('source-plan.json',plan)
        spec={'interface_version':P.VERSION,'root':str(tmp_path),'generation':str(native),'manifest_pin':P.F.pin(native/'manifest.json'),
              'native':{'plan_path':str(plan_path),'expected_plan_sha256':P.F.pin(plan_path)['sha256']},'mapped_generation':str(tmp_path/'mapped')}
        code=P.code_pins()
        review=jsonfile('synthetic-review.json',{'schema':'atx.tier1-notes-review/v1','status':'scoped_pass','interface_version':P.VERSION,'binding_sha256':P.F.digest(P._binding(spec)),'code_pins':code})
        spec['independent_review']={'path':str(review),'expected_sha256':P.F.pin(review)['sha256']}
        inputs=[plan_path,review,native/'manifest.json',source_manifest,transport,raw/'receipts.jsonl',curated,original,*products.values(),*(parts/name for name in P.SOURCE_FILES)]
        spec['dependency_pins']={**code,**{str(path):P.F.pin(path) for path in inputs}}
        _,_,_,validation,_,_=P._run(spec,con)
        spec['expected_validation_sha256']=P.F.digest(validation)
        built=P.materialize(spec,con)
        spec.update(mapped_manifest_pin=built['manifest_pin'],mapped_product_pins=built['product_pins'])
        admission=P.admit(spec,con)
        cells=list(P.iter_rows(admission,'notes_cells.parquet',con))
        assert len(cells)==4*validation['native_product_rows']['segment_census.parquet']
        assert {row['field'] for row in cells}==set(P.FIELDS)
        assert not admission['source_quality_certified']
        assert all(not row['qualified'] for row in cells)  # No curated actual originals in fixture.
        with original.open('ab') as stream:stream.write(b' changed')
        with pytest.raises(ValueError,match='dependency changed'):
            list(P.iter_rows(admission,'notes_cells.parquet',con))
