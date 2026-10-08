"""Typed original-SQL oracle over tiny physical event populations."""
import ast
import datetime as dt
import gzip
import hashlib
import json
import zipfile
from pathlib import Path
import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import private_tier1_event_replay as P
from atx_db.alpha_panel import private_tier1_admission as A
from atx_db.alpha_panel import events_capital as K,events_governance as G,events_common as E
from atx_db.alpha_panel import events_sources as ES
from atx_db import sec_http as SH
from atx_db import _submissions_archive as SA
from atx_db.alpha_panel import sec_filings as SF


def _native_sql_oracle(module,con,paths,hits):
    """Execute original literal SQL phase statements, without native build/globals.

    Resource/publication/download statements are outside this SQL oracle. Native
    module globals and functions are never changed. The test supplies only its
    physical source filenames to the original local SQL expression names.
    """
    source=ast.parse(Path(module.__file__).read_text())
    body=next(node.body for node in source.body if isinstance(node,ast.FunctionDef) and node.name=='build')
    def hit_files(qids):return [str(hits[qid]) for qid in qids]
    def hits_sql(qids):
        files=','.join("'"+path+"'" for path in hit_files(qids))
        return f"SELECT DISTINCT adsh FROM read_parquet([{files}],union_by_name=true) WHERE upper(coalesce(file_type,'')) IN ('8-K','8-K/A') OR upper(coalesce(file_type,'')) LIKE 'EX-99%'"
    env={**vars(module),'con':con,'filings':str(paths['filings']),
        'prof':str(paths['profile']),'lines':str(paths['lines']),'lt':str(paths['links']),
        'items_p':str(paths['items']),'_hits_list':lambda qids:','.join("'"+path+"'" for path in hit_files(qids)),
        '_hit_files':hit_files,'_hits_sql':hits_sql,'cover_path':lambda:paths['cover']}
    first='forms' if module is K else 'item_list'
    start=next(index for index,node in enumerate(body) if isinstance(node,ast.Assign)
        and any(isinstance(target,ast.Name) and target.id==first for target in node.targets))
    finish=next(index for index,node in enumerate(body) if isinstance(node,ast.Expr)
        and isinstance(node.value,ast.Call) and isinstance(node.value.func,ast.Attribute)
        and node.value.func.attr=='attach_security')
    exec(compile(ast.Module(body=body[start:finish],type_ignores=[]),'<native-event-sql-oracle>','exec'),env)
    attach=ast.parse(Path(E.__file__).read_text())
    statements=next(node.body for node in attach.body if isinstance(node,ast.FunctionDef) and node.name=='attach_security')
    # Skip only docstring and global filename resolution; execute every original
    # attach SQL statement/tie-break/drop using the fixture's explicit path.
    env.update(vars(E));env.update({'con':con,'lt':str(paths['links']),
        'table':'ev' if module is K else 'k2','out':'ev2' if module is K else 'k3',
        'cik_col':'cik','date_col':'filing_date'})
    exec(compile(ast.Module(body=statements[2:],type_ignores=[]),'<native-link-sql-oracle>','exec'),env)
    rows=next(node for node in body if isinstance(node,ast.Assign)
        and any(isinstance(target,ast.Name) and target.id=='rows' for target in node.targets))
    return eval(compile(ast.Expression(rows.value.args[1]),'<native-event-output-sql>','eval'),env)


def test_capital_governance_full_typed_native_sql_oracle_and_transaction(tmp_path):
    paths={}
    with duckdb.connect(str(tmp_path/'sources.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        con.execute('''CREATE TABLE filings(cik BIGINT,accession VARCHAR,form VARCHAR,is_amendment BOOLEAN,
            filing_date DATE,report_date DATE,available_at TIMESTAMP,acceptance_clock VARCHAR,vintage_risk VARCHAR)''')
        rows=[(1,'reg','S-1',False,'2019-01-01'),(1,'ipo','424B4',False,'2019-02-01'),
            (1,'follow','424B4',False,'2019-03-01'),(1,'shelf1','424B5',False,'2019-04-01'),
            (1,'shelf2','424B5',False,'2019-04-04'),(2,'convert','8-K',False,'2019-05-01'),
            (2,'convert','8-K',False,'2019-05-01'),(3,'spin','10-12B',False,'2019-06-01'),
            (2,'officer','8-K',False,'2019-07-01'),(2,'auditor','8-K',False,'2019-07-02'),
            (4,'gc','10-K',False,'2019-07-03')]
        for cik,acc,form,amend,date in rows:
            con.execute("INSERT INTO filings VALUES(?,?,?,?,?,?,?,'native','unqualified')",[cik,acc,form,amend,date,date,date+' 20:00:00'])
        con.execute("CREATE TABLE items AS SELECT *,CASE accession WHEN 'officer' THEN '5.02' ELSE '4.01' END AS item FROM filings WHERE accession IN ('officer','auditor')")
        con.execute("CREATE TABLE profile AS SELECT 1::BIGINT cik,'6770'::VARCHAR sic")
        con.execute("CREATE TABLE lines AS SELECT * FROM (VALUES(101::BIGINT,DATE '2019-02-01',false),(202::BIGINT,DATE '2019-01-01',false),(303::BIGINT,DATE '2019-06-02',false)) t(security_id,first_session,left_censored)")
        con.execute("CREATE TABLE links AS SELECT * FROM (VALUES(101::BIGINT,1::BIGINT,DATE '2019-02-01',DATE '2020-01-01','strict',true,100,true),(202,2,DATE '2019-01-01',DATE '2020-01-01','name',false,90,true),(203,2,DATE '2019-01-01',DATE '2020-01-01','strict',true,80,true),(303,3,DATE '2019-06-02',DATE '2020-01-01','strict',true,50,false)) t(security_id,cik,valid_from,valid_to,link_tier,is_issuer_primary,sessions,ever_member)")
        con.execute("CREATE TABLE cover AS SELECT * FROM (VALUES(2::BIGINT,TIMESTAMP '2019-01-01','Before','10-K'),(2,TIMESTAMP '2020-01-01','FutureAfter','10-K')) t(entity_cik,available_at,auditor_name,form)")
        for key in ('filings','items','profile','lines','links','cover'):
            paths[key]=tmp_path/(key+'.parquet');con.execute(f"COPY {key} TO '{paths[key].as_posix()}' (FORMAT PARQUET)")
        allq={*K.CONVERT_QIDS,'spinoff','going_concern',*(query for group in G.DEPART_QIDS.values() for query in group)}
        hits={}
        for qid in sorted(allq):
            acc='convert' if qid.startswith('convert') else 'spin' if qid=='spinoff' else 'gc' if qid=='going_concern' else 'officer'
            typ='10-12B' if qid=='spinoff' else '10-K' if qid=='going_concern' else 'EX-99.1'
            hits[qid]=tmp_path/(qid+'.parquet')
            con.execute(f"COPY(SELECT '{acc}'::VARCHAR adsh,'{typ}'::VARCHAR file_type UNION ALL SELECT '{acc}','EX-10.1') TO '{hits[qid].as_posix()}' (FORMAT PARQUET)")
    for name,module in (('capital',K),('governance',G)):
        required=set(K.CONVERT_QIDS)|{'spinoff'} if name=='capital' else {query for group in G.DEPART_QIDS.values() for query in group}|{'going_concern'}
        selected={qid:hits[qid] for qid in required}
        with duckdb.connect(str(tmp_path/(name+'-oracle.duckdb')),config={'memory_limit':'128MB','threads':'1'}) as oracle:
            oracle.execute("SET TimeZone='UTC'");sql=_native_sql_oracle(module,oracle,paths,selected)
            expected=tmp_path/(name+'-expected.parquet');oracle.execute(f"COPY({sql}) TO '{expected.as_posix()}' (FORMAT PARQUET)")
        with duckdb.connect(str(tmp_path/(name+'-private.duckdb')),config={'memory_limit':'128MB','threads':'1'}) as con:
            con.execute("SET TimeZone='UTC'")
            for key,path in {**paths,**selected}.items():con.execute('CREATE VIEW src_'+key+' AS SELECT * FROM '+A.sql([path]))
            roles=('filings','profile','lines','links') if name=='capital' else ('filings','items','links','cover')
            relations={key:'src_'+key for key in roles};query_relations={qid:'src_'+qid for qid in required}
            pins={**P.code_pins(),**{str(path):A.pin(path) for path in [*paths.values(),*selected.values()]}}
            with P.projection(name,con,relations,query_relations,pins) as sql:
                actual='('+sql+')';original=A.sql([expected])
                assert con.execute('DESCRIBE SELECT * FROM '+actual).fetchall()==con.execute('DESCRIBE SELECT * FROM '+original).fetchall()
                assert not con.execute(f'SELECT 1 FROM ((SELECT * FROM {actual} EXCEPT ALL SELECT * FROM {original}) UNION ALL (SELECT * FROM {original} EXCEPT ALL SELECT * FROM {actual})) LIMIT 1').fetchone()
                if name=='capital':assert con.execute("SELECT n_filings FROM ("+sql+") WHERE event_type='convert_pricing'").fetchone()[0]==2
                else:assert con.execute("SELECT auditor_name_after FROM ("+sql+") WHERE item='4.01'").fetchone()[0]=='FutureAfter'
            assert 'ev' not in {row[0] for row in con.execute('SHOW TABLES').fetchall()}
            with pytest.raises(ValueError,match='binding changed'):
                with P.projection(name,con,relations,query_relations,pins):relations['links']='redirected'


def test_complete_original_event_query_corpus_axis_and_missing_page(tmp_path):
    root=tmp_path/'originals';root.mkdir();records=[];objects={};products={};expected_rows={}
    queries=sorted(set(K.CONVERT_QIDS)|{'spinoff'})
    for qid in queries:
        forms,query=ES.QUERIES[qid];rows=[]
        for index,(start,end) in enumerate(ES.year_windows(ES.WINDOW_START,ES.WINDOW_END)):
            hits=[]
            if index==0:
                hits=[{'_id':'000000000119000001:original.htm','_source':{'adsh':'000000000119000001',
                    'file_type':'EX-99.1','form':'8-K','ciks':['1'],'file_date':'2019-02-01'}}]
            payload={'hits':{'total':{'value':len(hits),'relation':'eq'},'hits':hits}}
            body=json.dumps(payload).encode();sha=hashlib.sha256(body).hexdigest()
            path=root/'objects'/sha[:2]/(sha+'.gz');path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(gzip.compress(body,mtime=0));objects[str(path)]=A.pin(path)
            records.append({'url':ES.fts_url(query,forms,start,end),'status':200,'sha256':sha,
                'bytes':len(body),'fetched_at':'2026-09-20T00:00:00+00:00','error':None})
            rows.extend(ES.parse_hits(payload,qid,(start,end)))
        products[qid]=tmp_path/(qid+'.parquet')
        pq.write_table(pa.Table.from_pylist(rows,schema=ES.HIT_SCHEMA),products[qid]);expected_rows[qid]=rows
    ledger=root/SH.FETCH_LEDGER_NAME
    # A failed retry is retained and does not overwrite an earlier terminal200
    # under unchanged native FetchLedgerStore terminal-record semantics.
    records.append({**records[0],'status':403,'sha256':None,'bytes':0,'error':'hold'})
    ledger.write_text(''.join(json.dumps(record)+'\n' for record in records))
    pins={str(ledger):A.pin(ledger),**P.code_pins(),str(Path(SH.__file__).resolve()):A.pin(SH.__file__),
        **A.code(('private_tier1_factor_replay',)),**objects,
        **{str(path):A.pin(path) for path in products.values()}}
    with duckdb.connect(str(tmp_path/'query-replay.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        result=P.replay_query_corpus('capital',con,root,products,tmp_path/'expected',pins)
        assert result['complete_query_population'] and not result['source_quality_certified']
        assert result['failed_attempts']==1
        assert all(item['population']['hits_unique']==1 for item in result['queries'].values())
        # Independently repin a new source snapshot missing a required zero-hit
        # original page: selected hit documents alone cannot close the axis.
        missing=records[1]['url'];reduced=[record for record in records if record['url']!=missing]
        ledger.write_text(''.join(json.dumps(record)+'\n' for record in reduced));pins[str(ledger)]=A.pin(ledger)
        with pytest.raises(ValueError,match='complete original event query page absent'):
            P.replay_query_corpus('capital',con,root,products,tmp_path/'missing',pins)


def test_complete_sec_original_raw_extraction_matches_native_and_keeps_history(tmp_path):
    main={'cik':'1','name':'Original Issuer','sic':'6770','filings':{'recent':{
        'accessionNumber':[f'0000000001-19-{index:06d}' for index in range(67)],
        'form':['8-K']*67,'filingDate':['2019-01-02']*66+['2006-01-01'],
        'acceptanceDateTime':['2019-01-02T20:00:00Z']*67,'items':['5.02']*65},
        'files':[{'name':'CIK0000000001-submissions-001.json','filingCount':1}]}}
    history={'accessionNumber':['0000000001-18-000001'],'form':['10-K'],
        'filingDate':['2018-01-02'],'acceptanceDateTime':['2018-01-02T21:00:00Z']}
    archive=tmp_path/'original-submissions.zip'
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as output:
        output.writestr('CIK0000000001.json',json.dumps(main))
        output.writestr('CIK0000000001-submissions-001.json',json.dumps(history))
        output.writestr('retained-unrecognized.txt','not an SEC filing member')
    with SA.SubmissionsArchive(archive) as source:index=source.index_path;directory_receipt=source._receipt_path
    receipt=tmp_path/'original-download-receipt.json'
    receipt.write_text(json.dumps({'url':'https://www.sec.gov/Archives/edgar/daily-index/bulkdata/submissions.zip',
        'http_status':200,'error':None,'fetched_at':'2026-01-01T00:00:00Z',**A.pin(archive)}))
    pins={str(archive):A.pin(archive),str(receipt):A.pin(receipt),str(index):A.pin(index),
        str(directory_receipt):A.pin(directory_receipt),**P.code_pins(),str(Path(SA.__file__).resolve()):A.pin(SA.__file__)}
    expected=tmp_path/'native-raw.parquet'
    native=SF._FilingBuffer(expected)
    native.add(SF.flatten_filings(SF.columnar_block(main,0),1,0))
    native.add(SF.flatten_filings(SF.columnar_block(history,1),1,1));native.close()
    expected_profile=tmp_path/'native-profile.parquet';SF._write_profiles([SF.profile_row(main,1)],expected_profile)
    with duckdb.connect(str(tmp_path/'sec-raw.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        result=P.extract_sec_raw(con,archive,receipt,tmp_path/'private-extract',pins)
        assert result['population']=={'main':1,'history':1,'raw_rows':67,'profile_rows':1}
        assert not result['source_quality_certified']
        for key,path in (('raw_filings.parquet',expected),('raw_profiles.parquet',expected_profile)):
            actual=A.sql([result['products'][key]]);original=A.sql([path])
            assert con.execute('DESCRIBE SELECT * FROM '+actual).fetchall()==con.execute('DESCRIBE SELECT * FROM '+original).fetchall()
            assert not con.execute(f'SELECT 1 FROM ((SELECT * FROM {actual} EXCEPT ALL SELECT * FROM {original}) UNION ALL (SELECT * FROM {original} EXCEPT ALL SELECT * FROM {actual})) LIMIT 1').fetchone()
        assert con.execute('SELECT count(*) FROM '+A.sql([result['products']['raw_filings.parquet']])+' WHERE source=1').fetchone()[0]==1
        con.execute('CREATE TABLE admitted_raw AS SELECT * FROM '+A.sql([result['products']['raw_filings.parquet']]))
        consume_pins={**pins,**{str(path):A.pin(path) for path in result['products'].values()}}
        with P.sec_filings_scope(con,'admitted_raw',consume_pins) as queries:
            assert con.execute('SELECT count(*) FROM filings').fetchone()[0]==67
            assert con.execute("SELECT count(*) FROM filings WHERE accession='0000000001-18-000001'").fetchone()[0]==1
            assert 'clock_files' in queries and 'items' in queries
        assert 'filings' not in {row[0] for row in con.execute('SHOW TABLES').fetchall()}
        changed=dict(consume_pins)
        with pytest.raises(ValueError,match='dependency binding changed'):
            with P.sec_filings_scope(con,'admitted_raw',changed):
                changed.pop(str(result['products']['raw_filings.parquet']))
        assert 'filings' not in {row[0] for row in con.execute('SHOW TABLES').fetchall()}
        con.execute('CREATE TABLE items(sentinel INTEGER)')
        with pytest.raises(ValueError,match='namespace invalid'):
            with P.sec_filings_scope(con,'admitted_raw',consume_pins):
                pytest.fail('occupied native namespace must refuse before assembly')
        assert con.execute('DESCRIBE items').fetchone()[0]=='sentinel'


def test_original_accepted_full_population_missing_clock_and_early_close(tmp_path):
    index=tmp_path/'original-index.htm';accession='0000000001-19-000001'
    index.write_text(f'<html>CIK: 1 {accession} Accepted 2019-01-02 15:00:00</html>')
    receipt=tmp_path/'original-index.receipt.json'
    receipt.write_text(json.dumps({'url':f'https://www.sec.gov/Archives/edgar/data/1/000000000119000001/{accession}-index.htm',
        'http_status':200,'error':None,'fetched_at':'2019-01-03T00:00:00Z',**A.pin(index)}))
    plan={'files':{index.name:A.pin(index),receipt.name:A.pin(receipt)},
        'acceptance_proofs':[{'cik':1,'accession':accession,'index_path':index.name,'receipt':receipt.name}]}
    pins={**P.code_pins(),**A.code(('filing_text_native','filing_text')),
        str(index):A.pin(index),str(receipt):A.pin(receipt)}
    with duckdb.connect(str(tmp_path/'accepted.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        con.execute('CREATE TABLE full_filings(cik BIGINT,accession VARCHAR,acceptance_raw VARCHAR)')
        con.executemany('INSERT INTO full_filings VALUES(?,?,?)',
            [(1,f'0000000001-19-{number:06d}','2019-01-02T15:00:00Z') for number in range(1,101)])
        with P.accepted_clock_scope(con,tmp_path,'full_filings',plan,A.digest(plan),pins) as sql:
            assert con.execute('SELECT count(*) FROM ('+sql+')').fetchone()[0]==100
            assert con.execute('SELECT count(*) FROM ('+sql+') WHERE original_accepted_clock_supported').fetchone()[0]==1
            row=con.execute('SELECT original_accepted_at,acceptance_raw FROM ('+sql+') WHERE original_accepted_clock_supported').fetchone()
            assert row[0].isoformat()=='2019-01-02T20:00:00+00:00'
            assert row[1]=='2019-01-02T15:00:00Z'
            assert con.execute('SELECT count(*) FROM ('+sql+") WHERE original_accepted_reason='original_Accepted_evidence_missing'").fetchone()[0]==99
        assert 't17_event_original_accepted' not in {row[0] for row in con.execute('SHOW TABLES').fetchall()}
        with pytest.raises(ValueError):
            with P.accepted_clock_scope(con,tmp_path,'full_filings',plan,A.digest(plan),pins):
                index.write_text(index.read_text()+' changed original')
                raise RuntimeError('consumer closed early')
        assert 't17_event_original_accepted' not in {row[0] for row in con.execute('SHOW TABLES').fetchall()}


def test_event_body_full_native_ledger_population_terminal_failures_and_missing(tmp_path):
    from types import SimpleNamespace
    root=tmp_path/'native-events';root.mkdir();records=[];retained={};bodies={}
    for number in range(1,4):
        url=ES.document_url(1,f'0000000001-19-{number:06d}',f'original{number}.htm')
        body=f'<html>original body {number}</html>'.encode();sha=hashlib.sha256(body).hexdigest()
        records.append({'url':url,'status':200,'sha256':sha,'bytes':len(body),
            'fetched_at':'2019-01-03T00:00:00Z','error':None})
        bodies[url]=(sha,body)
        if number<3:
            path=root/'objects'/sha[:2]/(sha+'.gz');path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(gzip.compress(body,mtime=0));retained[str(path)]=A.pin(path)
    records.extend([{**records[0],'status':403,'sha256':None,'bytes':0,'error':'held'},
                    {**records[1],'status':404,'sha256':None,'bytes':0,'error':None}])
    ledger=root/SH.FETCH_LEDGER_NAME;ledger.write_text(''.join(json.dumps(row)+'\n' for row in records))
    native=ES.fetched_documents(SimpleNamespace(ledger_path=ledger))
    pins={str(ledger):A.pin(ledger),**P.code_pins(),str(Path(SH.__file__).resolve()):A.pin(SH.__file__),**retained}
    with duckdb.connect(str(tmp_path/'event-bodies.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        with P.event_body_scope(con,root,pins) as reader:
            rows=list(reader.rows())
            assert [(row['url'],row['sha256']) for row in rows]==native
            assert [row['body_present'] for row in rows]==[True,True,False]
            assert reader.read(rows[0]['url'],rows[0]['sha256'])==bodies[rows[0]['url']][1]
            assert reader.read(rows[1]['url'],rows[1]['sha256']) is None
            assert reader.read(rows[2]['url'],rows[2]['sha256']) is None
        assert 't17_event_body_registry' not in {row[0] for row in con.execute('SHOW TABLES').fetchall()}
        with pytest.raises(ValueError,match='presence/selection changed'):
            with P.event_body_scope(con,root,pins) as reader:
                missing=list(reader.rows())[2];sha=missing['sha256']
                path=root/'objects'/sha[:2]/(sha+'.gz');path.parent.mkdir(parents=True,exist_ok=True)
                path.write_bytes(gzip.compress(bodies[missing['url']][1],mtime=0))
                raise RuntimeError('body appeared while consumer exited early')
        assert 't17_event_body_registry' not in {row[0] for row in con.execute('SHOW TABLES').fetchall()}


def test_task9_landed_registry_matches_native_latest_success_and_missing_original(tmp_path):
    """Full native units-done oracle; no global path/resource replacement."""
    import ast
    import inspect
    root=tmp_path/'original-task9';root.mkdir()
    bodies=[b'old original',b'last successful original',b'missing original']
    shas=[hashlib.sha256(body).hexdigest() for body in bodies]
    accession='0000000001-19-000001';missing='0000000002-19-000002'
    rows=[{'accession':accession,'outcome':'document_ok','exhibit':'old.htm','document_sha256':shas[0]},
          {'accession':accession,'outcome':'document_ok','exhibit':'new.htm','document_sha256':shas[1]},
          {'accession':accession,'outcome':'document_failed','exhibit':'failed.htm','document_sha256':shas[0]},
          {'accession':missing,'outcome':'document_ok','exhibit':None,'document_sha256':shas[2]}]
    ledger=root/SH.UNITS_DONE_NAME
    ledger.write_text(''.join(json.dumps(row)+'\n' for row in rows)+'{torn\n')
    original=root/'objects'/shas[1][:2]/(shas[1]+'.gz');original.parent.mkdir(parents=True)
    original.write_bytes(gzip.compress(bodies[1],mtime=0))
    native=ast.parse(inspect.getsource(ES.landed_release_index))
    namespace={'EARNINGS_RELEASE_DIR':root,'SH':SH,'json':json}
    exec(compile(native,'<unchanged-native-landed-release-index>','exec'),namespace)
    expected=namespace['landed_release_index']()
    pins={str(ledger):A.pin(ledger),str(original):A.pin(original),**P.code_pins(),
          str(Path(SH.__file__).resolve()):A.pin(SH.__file__)}
    with duckdb.connect(str(tmp_path/'landed.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        con.execute("SET temp_directory='"+(tmp_path/'spill').as_posix()+"'")
        with P.landed_body_scope(con,root,pins) as reader:
            actual=list(reader.rows())
            assert {row['accession']:(row['file'],row['sha']) for row in actual}==expected
            assert [row['body_present'] for row in actual]==[True,False]
            assert reader.read(shas[1])==bodies[1]
            assert reader.read(shas[2]) is None
            assert not con.execute("SELECT 1 FROM t17_event_landed_registry WHERE file='failed.htm'").fetchone()
        assert not {row[0] for row in con.execute('SHOW TABLES').fetchall()}.intersection(
            ('t17_event_landed_attempts','t17_event_landed_registry'))
        with pytest.raises(ValueError,match='presence/selection changed'):
            with P.landed_body_scope(con,root,pins):
                appeared=root/'objects'/shas[2][:2]/shas[2];appeared.parent.mkdir(parents=True,exist_ok=True)
                appeared.write_bytes(bodies[2])
                raise RuntimeError('consumer stopped early')
