"""Offline source replay interruption, identity and corruption regressions."""
import gzip,hashlib,json
from collections import Counter
from pathlib import Path
from types import SimpleNamespace
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import common as C,issuer_history as I,issuer_classes as K,issuer_class_replay as R

class Store:
    def __init__(self,root):self.root=root;self.records={};self.reads=Counter()
    def load(self):return self
    def lookup(self,url):return self.records.get(url)
    def object_path(self,sha):return self.root/'objects'/sha[:2]/(sha+'.gz')
    def raw_object_path(self,sha):return self.root/'objects'/sha[:2]/sha
    def replace(self,url,payload):
        sha=hashlib.sha256(payload).hexdigest();path=self.object_path(sha);path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(gzip.compress(payload,mtime=0))
        self.records[url]=SimpleNamespace(url=url,sha256=sha,bytes=len(payload),status=200,error=None,ok=True,fetched_at='fixture')
    def read(self,record,maximum=None):
        self.reads[record.url]+=1;raw=gzip.decompress(self.object_path(record.sha256).read_bytes())
        if hashlib.sha256(raw).hexdigest()!=record.sha256:raise ValueError('raw SHA mismatch')
        if maximum is not None and len(raw)>maximum:raise ValueError('source oversized')
        return raw

def primary(text):return ('<ix:nonNumeric name="dei:EntityCentralIndexKey">12</ix:nonNumeric>'+text).encode()

@pytest.fixture
def lake(tmp_path,monkeypatch):
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path/'lake'))
    raw=tmp_path/'raw';raw.mkdir();(raw/I.H.FETCH_LEDGER_NAME).write_text('fixture receipts',encoding='utf-8')
    monkeypatch.setattr(I,'RAW',raw);store=Store(raw);monkeypatch.setattr(I.H,'FetchLedgerStore',lambda root:store)
    stage=C.stage_dir(K.STAGE);rows=[];published=[]
    for number,(day,assertion) in enumerate((('20210528','We are a REIT.'),('20210602','We are no longer a REIT.')),1):
        accession=f'0000000012-21-{number:06d}';header='header-'+accession;document='primary-'+accession
        rows.append({'cik':12,'accession':accession,'form':'10-Q','filing_date':day,'header_url':header,'primary_url':document,'primary_document':'original.htm'})
        store.replace(header,f'<SEC-HEADER><ACCEPTANCE-DATETIME>{day}100000\nACCESSION NUMBER: {accession}\nCONFORMED SUBMISSION TYPE: 10-Q\nFILED AS OF DATE: {day}\nCONFORMED PERIOD OF REPORT: 20210331\nCOMPANY DATA:\nCOMPANY CONFORMED NAME: Example Corp\nCENTRAL INDEX KEY: 12\nSTATE OF INCORPORATION: DE\nBUSINESS ADDRESS:\nSTATE: NY\n</SEC-HEADER>'.encode())
        store.replace(document,primary(assertion))
    C.write_json_atomic(stage/'plan.json',{'rows':rows});C.write_json_atomic(stage/'fetch_status.json',{})
    def publish(*args,**kwargs):
        published.append(args);C.write_json_atomic(stage/'manifest.json',{'fixture':True})
    monkeypatch.setattr(C,'write_stage_manifest',publish)
    return SimpleNamespace(stage=stage,store=store,rows=rows,published=published,cache=tmp_path/'task-scratch/replay',raw=raw)

def test_interrupted_loader_has_no_publication_then_resumes_remaining_original(lake,monkeypatch):
    original=K.reit_assertion;calls=[]
    def interrupt(raw,name,cik):
        calls.append(raw)
        if len(calls)==2:raise KeyboardInterrupt('guard termination fixture')
        return original(raw,name,cik)
    monkeypatch.setattr(K,'reit_assertion',interrupt)
    with pytest.raises(KeyboardInterrupt):K.load(lake.cache)
    assert len(list(lake.cache.glob('*.json')))==1
    assert not lake.published and not (lake.stage/'manifest.json').exists()
    assert not (lake.stage/'reit_status.parquet').exists()
    resumed=[]
    def record(raw,name,cik):resumed.append(raw);return original(raw,name,cik)
    monkeypatch.setattr(K,'reit_assertion',record)
    result=K.load(lake.cache)
    assert len(resumed)==1 and b'no longer' in resumed[0]
    assert result['replay_checkpoint']['reused']==1 and result['replay_checkpoint']['computed']==1
    assert result['observations']==2 and len(lake.published)==1
    assert lake.store.reads[lake.rows[0]['primary_url']]==1
    assert {row['accession']:row['reit_status'] for row in pq.read_table(lake.stage/'reit_status.parquet').to_pylist()}=={lake.rows[0]['accession']:True,lake.rows[1]['accession']:False}
    cached_tables={name:(lake.stage/name).read_bytes() for name in ('reit_status.parquet','incorporation.parquet')}
    cached_assessment=C.read_json(lake.stage/'assessment.json')
    K.load()
    assert all((lake.stage/name).read_bytes()==payload for name,payload in cached_tables.items())
    assert C.read_json(lake.stage/'assessment.json')==cached_assessment

def replay_one(lake,identity=None,row=None,name='Example Corp'):
    row=row or lake.rows[0];cache=R.AssertionReplay(lake.cache,identity or {'parser':'original'})
    def process():return K.reit_assertion(lake.store.read(lake.store.lookup(row['primary_url']),maximum=K.PRIMARY_MAXIMUM),name,row['cik'])
    result=cache.assertion(row,lake.store,name,process)
    return cache,result

@pytest.mark.parametrize('change',['code','plan','issuer_name','receipt','primary_bytes','header_bytes'])
def test_changed_identity_never_reuses_certified_assertion(lake,change):
    replay_one(lake);identity={'parser':'original'};row=dict(lake.rows[0]);name='Example Corp'
    if change=='code':identity={'parser':'changed clock/helper code'}
    elif change=='plan':row['primary_document']='different.htm'
    elif change=='issuer_name':name='Another Corp'
    elif change=='receipt':lake.store.records[row['primary_url']].bytes+=1
    elif change=='primary_bytes':lake.store.replace(row['primary_url'],primary('We are no longer a REIT.'))
    else:
        header=lake.store.lookup(row['header_url']);lake.store.replace(row['header_url'],lake.store.read(header)+b'\nchanged source')
    cache,result=replay_one(lake,identity,row,name)
    assert cache.hits==0 and cache.computed==1 and cache.invalid==1
    assert result['status'] is (False if change=='primary_bytes' else True)

@pytest.mark.parametrize('damage',['syntax','checksum','typed_payload','identity'])
def test_corrupt_checkpoint_recomputes_from_verified_source(lake,damage):
    replay_one(lake);path=next(lake.cache.glob('*.json'));value=C.read_json(path)
    if damage=='syntax':path.write_text('{',encoding='utf-8')
    else:
        if damage=='checksum':value['assertion']['status']=False
        elif damage=='typed_payload':
            value['assertion']['status']=1;value['assertion_sha256']=R.digest(value['assertion'])
        else:value['identity_sha256']='corrupt'
        C.write_json_atomic(path,value)
    cache,result=replay_one(lake)
    assert cache.hits==0 and cache.computed==1 and cache.invalid==1 and result['status'] is True

def test_corrupt_retained_bytes_cannot_inherit_old_positive(lake):
    replay_one(lake);record=lake.store.lookup(lake.rows[0]['primary_url'])
    lake.store.object_path(record.sha256).write_bytes(gzip.compress(primary('We are no longer a REIT.'),mtime=0))
    with pytest.raises(ValueError,match='raw SHA mismatch'):replay_one(lake)

@pytest.mark.parametrize('change',['source','code'])
def test_source_or_code_change_after_checkpoint_refuses_finish(lake,change):
    cache,_=replay_one(lake);identity={'parser':'original'}
    if change=='code':identity={'parser':'changed'}
    else:lake.store.replace(lake.rows[0]['primary_url'],primary('We are no longer a REIT.'))
    with pytest.raises(RuntimeError,match='changed'):cache.finish(identity)

@pytest.mark.parametrize('change',['plan','receipts'])
def test_changed_plan_or_receipts_refuses_entire_source_publication(lake,monkeypatch,change):
    original=K.reit_assertion;changed=False
    def mutate(raw,name,cik):
        nonlocal changed
        if not changed:
            changed=True
            if change=='plan':C.write_json_atomic(lake.stage/'plan.json',{'rows':lake.rows[:1]})
            else:(lake.raw/I.H.FETCH_LEDGER_NAME).write_text('different receipts',encoding='utf-8')
        return original(raw,name,cik)
    monkeypatch.setattr(K,'reit_assertion',mutate)
    with pytest.raises(RuntimeError,match='changed'):K.load(lake.cache)
    assert not lake.published and not (lake.stage/'reit_status.parquet').exists()

def test_duplicate_plan_keys_refuse_publication(lake):
    C.write_json_atomic(lake.stage/'plan.json',{'rows':[lake.rows[0],lake.rows[0]]})
    with pytest.raises(ValueError,match='duplicate'):K.load(lake.cache)
    assert not lake.published

def test_missing_header_is_accounted_as_unknown_and_never_cached_as_false(lake):
    del lake.store.records[lake.rows[0]['header_url']]
    result=K.load(lake.cache)
    rows=C.read_json(lake.stage/'assessment.json')['rows']
    assert result['observations']==2 and len(rows)==2
    assert rows[0]['status'] is None and rows[0]['reason']=='source_not_fetched_or_failed'
    assert result['replay_checkpoint']['completed']==1
