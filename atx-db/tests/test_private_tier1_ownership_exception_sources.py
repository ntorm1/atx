"""Offline native ZIP/directory/99-part fixtures; never use retained source data."""
import copy
import datetime as dt
import hashlib
import json
import struct
import zipfile
from pathlib import Path

import pytest

from atx_db import _submissions_archive as A
from atx_db.alpha_panel import common as C, thirteenf_quality as Q
from atx_db.alpha_panel import private_tier1_ownership_exception_sources as H


def arrays(cik, sequence=1, date='2019-11-14', *, empty=False):
    return {'form':[] if empty else ['13F-HR'],
            'accessionNumber':[] if empty else [f'{cik:010d}-19-{sequence:06d}'],
            'filingDate':[] if empty else [date],
            'reportDate':[] if empty else ['2018-12-31' if date=='2019-02-14' else '2019-09-30'],
            # Deliberately unqualified raw clock, preserved exactly by the kernel.
            'acceptanceDateTime':[] if empty else [date+'T12:00:00']}


def encoded(document,size=None):
    if size:
        document['padding']=''
        raw=json.dumps(document,separators=(',',':')).encode()
        document['padding']='x'*(size-len(raw))
    result=json.dumps(document,separators=(',',':')).encode()
    if size:assert len(result)==size
    return result


def write_json(path,value):
    path.write_text(json.dumps(value,sort_keys=True),encoding='utf-8')


def fixture(root, bad=None):
    root.mkdir();archive=root/'submissions.zip';documents={}
    for cik in range(100,197):
        documents[f'CIK{cik:010d}.json']=encoded({'cik':cik,'filings':{'recent':arrays(cik,empty=cik!=100),'files':[]}})
    for cik,(name,size) in H.EXCEPTIONS.items():
        history=name[:-5]+'-submissions-001.json'
        ref={'name':history,'filingCount':1,'filingFrom':'2019-02-14','filingTo':'2019-02-14'}
        references=[ref]
        if cik==19617:
            if bad=='duplicate':references.append(dict(ref))
            if bad=='foreign':ref['name']='CIK0001665650-submissions-001.json'
            if bad=='missing':ref['name']=name[:-5]+'-submissions-999.json'
            if bad=='range':ref['filingFrom']='2019-02-13'
            if bad=='count':ref['filingCount']=2
            if bad=='malformed':ref['unexpected']=1
        recent=arrays(cik)
        if bad=='array' and cik==19617:recent['filingDate']=[]
        owner=1665650 if bad=='main_owner' and cik==19617 else cik
        if bad=='main_fraction' and cik==19617:owner=19617.4
        documents[name]=encoded({'cik':owner,'filings':{'recent':recent,'files':references}},size)
        history_document=arrays(cik,2,'2019-02-14')
        if bad=='history_owner' and cik==19617:history_document['cik']=1665650
        if bad=='history_fraction' and cik==19617:history_document['cik']=19617.4
        documents[history]=encoded(history_document,4*1024*1024+1 if bad=='history_bound' and cik==19617 else None)
        if bad=='unreferenced' and cik==19617:
            documents[name[:-5]+'-submissions-002.json']=encoded(arrays(cik,3,'2019-02-14'))
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as sink:
        for name,raw in documents.items():sink.writestr(name,raw)
    if bad=='crc':
        # Pin a native directory whose original central/local CRC is wrong.
        # No payload is decompressed by fixture setup before the helper reads it.
        raw=bytearray(archive.read_bytes());name=b'CIK0000019617.json'
        offset=raw.index(name,raw.index(b'PK\x01\x02'))-46
        header=struct.unpack_from('<I',raw,offset+42)[0]
        crc=struct.unpack_from('<I',raw,offset+16)[0]^1
        struct.pack_into('<I',raw,offset+16,crc);struct.pack_into('<I',raw,header+14,crc)
        archive.write_bytes(raw)
    records=[]
    with A.SubmissionsArchive(archive) as source:
        directory=source.index_path;directory_receipt=source._receipt_path
        archive_axis={'main_members':source.main_member_count,'history_members_in_directory':source.history_member_count,
                      'last_main_member':list(source.main_members())[-1],'partitions':99}
        for cik,(name,_) in H.EXCEPTIONS.items():
            items=[]
            for n,kind,raw in source._connection().execute('SELECT name,kind,record FROM members WHERE name=? OR (kind=2 AND substr(name,1,?)=?) ORDER BY name',(name,len(name[:-5])+1,name[:-5]+'-')):
                info=A._one_info(raw)
                items.append({'name':n,'kind':kind,'expanded_bytes':info.file_size,'compressed_bytes':info.compress_size,
                              'crc32':info.CRC,'central_record_sha256':hashlib.sha256(raw).hexdigest()})
            history=[item for item in items if item['kind']==2]
            records.append({'cik':cik,'main':next(item for item in items if item['kind']==1),
                            'history_directory_members':history,'history_directory_count':len(history)})
    pins={str(p.resolve()):H.identity(p) for p in (archive,directory,directory_receipt,Path(A.__file__),Path(Q.__file__))}
    parts=[];after=None;exceptions=[];counts={'main_members':0,'history_members':0,'filings':0}
    for part in range(99):
        output=root/f'task11-submissions-inventory-part{part:04d}-v1.jsonl'
        native=Q.inventory_submissions(archive,output,after=after,through='CIK9999999999.json',max_main=1,first=dt.date(2019,1,1))
        receipt=Path(str(output)+'.receipt.json')
        code_return=2 if native['exceptions'] or native['bound_reached'] else 0
        command=['.venv/Scripts/python.exe','-m','atx_db.alpha_panel.thirteenf_quality','inventory-submissions','--archive',archive.as_posix()]
        if after is not None:command.extend(['--after',after])
        command.extend(['--through','CIK9999999999.json','--max-main','1','--first','2019-01-01','--output',output.as_posix()])
        guard=root/f't11-submissions-inventory-part{part:04d}-v1.receipt.json'
        # Source-shaped historical receipt fixture, not actual job/resource evidence.
        write_json(guard,{'command':command,'returncode':code_return,'status':'failed' if code_return else 'completed',
                         'cap_hit':False,'stop_reason':None,'job_limit_gb':.2,'native_peak_job_memory_bytes':1,'job_cap_bytes':200_000_000})
        for path in (output,receipt,guard):pins[str(path.resolve())]=H.identity(path)
        parts.append({'part':part,'version':1,'after':after,'through_observed':native['last_main_member'],'counts':native['counts'],
                      'output_identity':pins[str(output.resolve())],'native_receipt':pins[str(receipt.resolve())],
                      'guard_receipt':pins[str(guard.resolve())]})
        after=native['last_main_member'];exceptions.extend(native['exceptions'])
        for field in counts:counts[field]+=native['counts'][field]
    prior=root/'prior-audit.json'
    prior_value={'status':'BOUNDED_INVENTORY_CHAIN_BYTES_AND_EXECUTION_VERIFIED','terminal_exhaustion':True,
                 'partitions':parts,'counts':counts,'unique_cik_accessions':counts['filings'],
                 'exceptions':exceptions,'last_main_member':after,'archive_axis':archive_axis,'pins':pins}
    write_json(prior,prior_value)
    metadata=root/'metadata-audit.json'
    write_json(metadata,{'status':'ORIGINAL_DIRECTORY_METADATA_VERIFIED','records':records,
                         'pins':{str(path.resolve()):H.identity(path) for path in (archive,directory,directory_receipt,prior,Path(A.__file__))},
                         'source_payloads_read':False,'exception_closure':False,'history_reference_closure':False,
                         'source_qualified':False,'gate_promotion':False})
    code={str(Path(module.__file__).resolve()):H.identity(module.__file__) for module in (A,C,Q,H)}
    plan={'schema':H.SCHEMA,'archive':archive.name,'directory':directory.name,'directory_receipt':directory_receipt.name,
          'prior_audit':prior.name,'metadata_audit':metadata.name,'first_filing_date':'2019-01-01',
          'files':{path.relative_to(root).as_posix():H.identity(path) for path in root.iterdir() if path.is_file()},'code':code}
    return plan,documents


@pytest.fixture
def connection(tmp_path):
    con=C.connect(memory='128MB',threads=1,temp_dir=tmp_path/'database',db_file='fixture.duckdb')
    yield con
    con.close()


def test_native_oversized_main_and_history_replay(tmp_path,connection):
    root=tmp_path/'sources';plan,documents=fixture(root)
    output=tmp_path/'generation'
    admission=H.build_exception_inventory(connection,root,output,plan,H.digest(plan))
    expected=[]
    archive_sha=plan['files'][plan['archive']]['sha256']
    for name,raw in documents.items():
        cik=int(name[3:13])
        expected.extend(Q.submissions_inventory(json.loads(raw),cik=cik,member=name,archive_sha256=archive_sha,first=dt.date(2019,1,1)))
    rows=list(H.iter_exception_rows(admission,batch_size=2))
    assert sorted(rows,key=lambda row:(row['cik'],row['accession']))==sorted(expected,key=lambda row:(row['cik'],row['accession']))
    assert admission['population']=={'prior_records':1,'supplement_records':4,'exact_overlap_keys':0,'full_reconciled_records':5}
    assert admission['qualification'] is False and admission['validation']['public_clocks_qualified'] is False
    assert admission['schemas']['submissions_inventory.parquet']==H.FIELDS
    assert all(row['available_at'].endswith('T12:00:00') for row in rows)
    assert not output.with_name(output.name+'.partial').exists()
    with pytest.raises(ValueError,match='manifest differs'):
        H.read_exception_inventory(connection,root,output,'0'*64,H.digest(plan))
    with pytest.raises(ValueError,match='immutable'):H.build_exception_inventory(connection,root,output,plan,H.digest(plan))


@pytest.mark.parametrize('bad,reason',[('duplicate','duplicate original history'),('foreign','foreign original history'),
                                     ('missing','absent from independently'),('range','count/range mismatch'),
                                     ('count','count/range mismatch'),('malformed','malformed original history'),
                                     ('array','malformed required original arrays'),('history_owner','history CIK mismatch'),
                                     ('main_owner','main CIK mismatch'),('main_fraction','main CIK mismatch'),
                                     ('history_fraction','history CIK mismatch')])
def test_original_reference_failures(tmp_path,connection,bad,reason):
    root=tmp_path/'sources';plan,_=fixture(root,bad)
    with pytest.raises(ValueError,match=reason):H.build_exception_inventory(connection,root,tmp_path/'generation',plan,H.digest(plan))
    assert not (tmp_path/'generation').exists()


def test_history_bound_before_payload_read(tmp_path,connection,monkeypatch):
    root=tmp_path/'sources';plan,_=fixture(root,'history_bound')
    native_read=A.SubmissionsArchive.read
    observed=[]
    def read(source,name):
        observed.append(name)
        return native_read(source,name)
    monkeypatch.setattr(A.SubmissionsArchive,'read',read)
    with pytest.raises(ValueError,match='byte bound'):H.build_exception_inventory(connection,root,tmp_path/'generation',plan,H.digest(plan))
    assert observed==['CIK0000019617.json']


def test_original_crc_replayed_by_unchanged_reader(tmp_path,connection):
    root=tmp_path/'sources';plan,_=fixture(root,'crc')
    with pytest.raises(zipfile.BadZipFile,match='CRC'):
        H.build_exception_inventory(connection,root,tmp_path/'generation',plan,H.digest(plan))


def test_prefix_directory_is_not_reference_closure(tmp_path,connection):
    root=tmp_path/'sources';plan,_=fixture(root,'unreferenced')
    admission=H.build_exception_inventory(connection,root,tmp_path/'generation',plan,H.digest(plan))
    manifest=json.loads((tmp_path/'generation'/'manifest.json').read_text())
    assert manifest['directory_exclusions']==[{'cik':19617,'member':'CIK0000019617-submissions-002.json','reason':'directory_member_not_declared_by_original_main'}]
    assert admission['population']['full_reconciled_records']==5 and admission['qualification'] is False


def test_stale_plan_and_missing_dependency(tmp_path,connection):
    root=tmp_path/'sources';plan,_=fixture(root)
    expected=H.digest(plan)
    wrong=copy.deepcopy(plan);wrong['files'].pop(next(name for name in wrong['files'] if name.endswith('.jsonl')))
    with pytest.raises(ValueError,match='plan mismatch'):H.build_exception_inventory(connection,root,tmp_path/'stale',wrong,expected)
    with pytest.raises(ValueError,match='dependency omitted'):H.build_exception_inventory(connection,root,tmp_path/'omitted',wrong,H.digest(wrong))
    with (root/'task11-submissions-inventory-part0000-v1.jsonl').open('a') as sink:sink.write('mutation\n')
    with pytest.raises(ValueError,match='changed pinned source'):H.build_exception_inventory(connection,root,tmp_path/'changed',plan,expected)


def test_full_original99_chain_cannot_be_truncated(tmp_path,connection):
    root=tmp_path/'sources';plan,_=fixture(root)
    prior=root/plan['prior_audit'];value=json.loads(prior.read_text())
    value['partitions'].pop();write_json(prior,value)
    metadata=root/plan['metadata_audit'];value=json.loads(metadata.read_text())
    value['pins'][str(prior.resolve())]=H.identity(prior);write_json(metadata,value)
    plan['files'][prior.name]=H.identity(prior);plan['files'][metadata.name]=H.identity(metadata)
    with pytest.raises(ValueError,match='99-part chain'):
        H.build_exception_inventory(connection,root,tmp_path/'generation',plan,H.digest(plan))


def test_complete_reader_detects_payload_mutation_during_replay(tmp_path,connection,monkeypatch):
    root=tmp_path/'sources';plan,_=fixture(root)
    output=tmp_path/'generation';admission=H.build_exception_inventory(connection,root,output,plan,H.digest(plan))
    native_materialize=H._materialize
    def materialize(*args):
        result=native_materialize(*args)
        with (output/'supplement.jsonl').open('a') as sink:sink.write('mutation\n')
        return result
    monkeypatch.setattr(H,'_materialize',materialize)
    with pytest.raises(ValueError,match='payload changed during replay'):
        H.read_exception_inventory(connection,root,output,admission['manifest_sha256'],admission['plan_sha256'])


def test_public_read_rechecks_original_source_on_early_close(tmp_path,connection,monkeypatch):
    root=tmp_path/'sources';plan,_=fixture(root)
    admission=H.build_exception_inventory(connection,root,tmp_path/'generation',plan,H.digest(plan))
    checks=[];original_check=H._check
    def checked(*args):
        checks.append(1);return original_check(*args)
    monkeypatch.setattr(H,'_check',checked)
    rows=H.iter_exception_rows(admission,batch_size=2);next(rows)
    with (root/'task11-submissions-inventory-part0098-v1.jsonl').open('a') as sink:sink.write('mutation\n')
    with pytest.raises(ValueError,match='changed pinned source'):rows.close()
    assert len(checks)==2


def test_public_many_batches_hash_whole_dependencies_only_twice(tmp_path,monkeypatch):
    """Transaction-cost instrumentation only; this is not native source admission."""
    import pyarrow as pa
    import pyarrow.parquet as pq
    root=tmp_path/'sources';root.mkdir();generation=tmp_path/'generation';generation.mkdir()
    plan={'schema':H.SCHEMA,'first_filing_date':'2019-01-01','files':{},
          'code':{str(Path(m.__file__).resolve()):H.identity(m.__file__) for m in (A,C,Q,H)}}
    for role in ('archive','directory','directory_receipt','prior_audit','metadata_audit'):
        path=root/(role+'.fixture');path.write_bytes(b'bounded instrumentation original')
        plan[role]=path.name;plan['files'][path.name]=H.identity(path)
    write_json(generation/'source-plan.json',plan)
    pq.write_table(pa.table({'row':range(173)}),generation/'submissions_inventory.parquet')
    files={p.name:H.identity(p) for p in generation.iterdir()}
    write_json(generation/'manifest.json',{'files':files})
    admission={'generation':str(generation),'source_root':str(root),
               'manifest_sha256':H.identity(generation/'manifest.json')['sha256'],
               'plan_sha256':H.digest(plan),'dependency_pins':plan['files'],'code':plan['code']}
    identities=[];original_identity=H.identity
    def identified(path):
        identities.append(Path(path).resolve());return original_identity(path)
    monkeypatch.setattr(H,'identity',identified)
    rows=list(H.iter_exception_rows(admission,batch_size=2))
    assert rows==[{'row':i} for i in range(173)]
    assert identities.count((root/plan['archive']).resolve())==2
    assert all(identities.count((root/name).resolve())==2 for name in plan['files'])


def test_replay_requires_file_backed_bounded_connection(tmp_path,connection):
    connection.execute('SET threads=2')
    with pytest.raises(ValueError,match='one thread'):H._bounded_connection(connection)
    import duckdb
    with duckdb.connect(config={'memory_limit':'128MB','threads':'1'}) as memory:
        memory.execute("SET TimeZone='UTC'")
        with pytest.raises(ValueError,match='file-backed'):H._bounded_connection(memory)


def test_original_owner_identifier_is_not_numeric_coercion():
    assert H._owner_matches(19617,19617)
    assert H._owner_matches('0000019617',19617)
    for value in (True,19617.0,19617.4,'19617.4','+19617','-19617',' 19617','19617 '):
        assert not H._owner_matches(value,19617)


@pytest.mark.parametrize('conflict',[False,True])
def test_disk_key_value_overlap_and_conflict(tmp_path,connection,conflict):
    root=tmp_path/'sources';plan,documents=fixture(root)
    prior=json.loads((root/plan['prior_audit']).read_text())
    name='CIK0000019617.json';part=prior['partitions'][97]
    assert part['through_observed']==name
    path=root/'task11-submissions-inventory-part0097-v1.jsonl'
    row=next(Q.submissions_inventory(json.loads(documents[name]),cik=19617,member=name,
                                    archive_sha256=plan['files'][plan['archive']]['sha256'],first=dt.date(2019,1,1)))
    if conflict:row['available_at']='changed-raw-lead'
    # Adversarial retained key/value overlap, not a claim that frozen producer
    # emitted its skipped main. This focused reconciliation kernel must detect it.
    path.write_text(json.dumps(row)+'\n',encoding='utf-8')
    receipt_path=Path(str(path)+'.receipt.json');receipt=json.loads(receipt_path.read_text())
    receipt['counts']['filings']=1;receipt['output_sha256']=H.identity(path)['sha256'];write_json(receipt_path,receipt)
    part['counts']['filings']=1;part['output_identity']=H.identity(path);part['native_receipt']=H.identity(receipt_path)
    prior['counts']['filings']+=1;prior['unique_cik_accessions']+=1
    prior['pins'][str(path.resolve())]=H.identity(path);prior['pins'][str(receipt_path.resolve())]=H.identity(receipt_path)
    supplemental=tmp_path/'supplement.jsonl'
    exact=dict(row);exact['available_at']='2019-11-14T12:00:00'
    supplemental.write_text(json.dumps(exact)+'\n',encoding='utf-8')
    destination=tmp_path/'reconciled';destination.mkdir()
    if conflict:
        with pytest.raises(ValueError,match='key/value disagreement'):H._reconcile(connection,root,plan,prior,supplemental,destination)
    else:
        assert H._reconcile(connection,root,plan,prior,supplemental,destination)=={'prior_records':2,'supplement_records':1,'exact_overlap_keys':1,'full_reconciled_records':2}
