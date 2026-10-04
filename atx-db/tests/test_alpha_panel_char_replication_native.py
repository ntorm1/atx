"""Synthetic native Task8 records; upstream release admission is explicitly mocked."""
import calendar
import csv
import datetime as dt
import io
import json
from pathlib import Path
import zipfile
import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import char_replication as R
from atx_db.alpha_panel import char_replication_native as N
from atx_db.alpha_panel import char_monthly_evidence as E
from atx_db.alpha_panel.char_monthly_registry import JKP153,FREEZE_SHA256


@pytest.fixture
def con():
    db=duckdb.connect();db.execute("SET memory_limit='128MB'");db.execute('SET threads=1');db.execute("SET TimeZone='UTC'")
    yield db
    db.close()


def write(root,name,doc):
    file=root/name;file.parent.mkdir(parents=True,exist_ok=True);file.write_text(json.dumps(doc,default=str,sort_keys=True))
    return {'path':name,'identity':N.R.identity(file),'expected_sha256':N.R.identity(file)['sha256']}


def filepin(root,name):
    pin=N.R.identity(root/name);return {'path':name,'identity':pin,'expected_sha256':pin['sha256']}


def product(root,name):
    return {'files':{name:N.R.identity(root/name)},'source':'independent synthetic source','clock':'synthetic source publication/complete realized window','admission':'synthetic reviewed before return run'}


def metadata(version,**values):return {'version':version,'registry_sha256':FREEZE_SHA256,'config_sha256':R.CONFIG_SHA,**values}


@pytest.fixture
def fixture(tmp_path,con,monkeypatch):
    root=tmp_path;formation=dt.date(2012,12,31);entry=dt.date(2013,1,1);exit=dt.date(2013,1,31)
    dates=[]
    for year in range(2012,2023):
        for month in range(1,13):dates.extend([dt.date(year,month,1),dt.date(year,month,calendar.monthrange(year,month)[1])])
    dates=sorted(set(dates));con.execute('CREATE TABLE cm_calendar(session_date DATE)');con.executemany('INSERT INTO cm_calendar VALUES (?)',[(d,) for d in dates])
    native=[];raw=[]
    catalog_native={'be_me':{'direction':1,'weighting':'vw_cap','unit':'decimal_return','sign_to_own':1},'age':{'direction':-1,'weighting':'vw_cap','unit':'decimal_return','sign_to_own':1}}
    for i,holding in enumerate(R.MONTHS):
        y,m=map(int,holding.split('-'));day=dt.date(y,m,calendar.monthrange(y,m)[1])
        for name,direction in (('be_me',1),('age',-1)):
            ret=(i%7+1)*.001*direction
            row={'location':'usa','name':name,'freq':'monthly','weighting':'vw_cap','direction':direction,'n_stocks':30,'n_stocks_min':10,'date':day,'ret':ret}
            raw.append(row);native.append({'month_end':day,'date':day,**{k:row[k] for k in N.RAW_JKP_COLUMNS if k!='date'}})
    text=io.StringIO();writer=csv.DictWriter(text,fieldnames=N.RAW_JKP_COLUMNS);writer.writeheader();writer.writerows(raw)
    with zipfile.ZipFile(root/'jkp.zip','w') as archive:archive.writestr('jkp.csv',text.getvalue())
    archive_pin=filepin(root,'jkp.zip')
    table=pa.Table.from_pylist(native).select(N.JKP_COLUMNS)
    footer={'dataset':'bench_jkp_us_factors','loader_version':'x3-benchmarks-v1','info':{'unit':'decimal_return'},'sources':[archive_pin['identity']]}
    pq.write_table(table.replace_schema_metadata({b'atx.benchmark':json.dumps(footer).encode()}),root/'jkp.parquet')
    with (root/'PredictorLSretWide.csv').open('w',newline='') as stream:
        writer=csv.writer(stream);writer.writerow(['date','BM','AgeRisk'])
        for i,holding in enumerate(R.MONTHS):writer.writerow([holding+'-01',(i%7+1)*.1,-(i%7+1)*.1])
    (root/'authoritative.txt').write_text('Synthetic metadata: native BM/AgeRisk source uses equal_weight; percent returns; JKP signed vw_cap decimals.')
    products={'jkp':product(root,'jkp.parquet'),'osap':product(root,'PredictorLSretWide.csv')}
    contracts={}
    for kind in ('jkp','osap'):
        entries={n:{'status':'excluded','reason':'no admitted fixture match; original denominator retained','metadata_id':'authoritative.txt'} for n in JKP153}
        for name,native_name in (('be_me','be_me' if kind=='jkp' else 'BM'),('age','age' if kind=='jkp' else 'AgeRisk')):
            entries[name]={'status':'mapped','native_name':native_name,'sign':1,'weighting':'vw_cap' if kind=='jkp' else 'equal_weight','metadata_id':'authoritative.txt'}
        natives=catalog_native if kind=='jkp' else {n:{'weighting':'equal_weight','unit':'percent','sign_to_own':1} for n in ('BM','AgeRisk')}
        catalog=write(root,kind+'-catalog.json',metadata('char_replication_'+kind+'_catalog_v1',entries=entries,native=natives,metadata={'authoritative.txt':N.R.identity(root/'authoritative.txt')}))
        admission=write(root,kind+'-admission.json',metadata('char_replication_mapping_admission_v1',phase='frozen_before_any_return_run',mapping_sha256=N.R.digest(entries),catalog_identity=catalog['identity']))
        doc=metadata('char_replication_'+kind+'_contract_v1',product=products[kind],entries=entries,catalog=catalog,admission_record=admission)
        if kind=='jkp':doc.update(raw_archive=archive_pin,zip_member='jkp.csv')
        contracts[kind]=write(root,kind+'-contract.json',doc)
    clock=dt.datetime(2012,12,30,20,tzinfo=dt.timezone.utc)
    rows=[];audits=[];labels=[];price_rows=[];action_rows=[];classifications={};windows={}
    for sid in range(1,31):
        rows.append({'session_date':formation,'security_id':sid,'cik':sid,'issuer_me':100.+sid,'univ_all_common':True,'available_at':dt.datetime(2012,12,31,22,tzinfo=dt.timezone.utc),'source_available_at':clock,**{n:float(sid) if n!='eq_dur' else None for n in JKP153}})
        cls=write(root,f'proofs/class-{sid}.json',metadata('char_monthly_classification_proof_v1',formation=str(formation),security_id=sid,cik=sid,classification='primary',source_available_at=clock.isoformat(),nyse=True,issuer_me=100.+sid))
        classifications[str(sid)]=cls
        audits.append({'formation':formation,'security_id':sid,'cik':sid,'available_at':clock,'source_available_at':clock,'me':100.+sid,'nyse':True,'primary_security':True,'certified':True,'classification':'primary','classification_proof':str(sid),'exclusion_reason':None})
        values=[100.,100.*(1+sid*.001),100.*(1+sid*.001)**2]
        for day,close in zip((formation,entry,exit),values):
            price_rows.append({'security_id':sid,'session_date':day,'close':close});action_rows.append({'security_id':sid,'session_date':day,'split_multiplier':1.,'cash_dividend':0.,'terminal_treatment':'none'})
        labels.append({'formation':formation,'security_id':sid,'cik':sid,'entry_date':entry,'exit_date':exit,'ret':(1+sid*.001)**2-1,'return_certified':True})
    (root/'char_monthly/generations/fixture/month=2012-12').mkdir(parents=True)
    pq.write_table(pa.Table.from_pylist(rows),root/'char_monthly/generations/fixture/month=2012-12/char_monthly.parquet')
    generation=root/'char_monthly/generations/fixture';feature={'files':{'month=2012-12/char_monthly.parquet':N.R.identity(generation/'month=2012-12/char_monthly.parquet')},'receipts':{}}
    for name in ('manifest','snapshot','validation'):write(root,f'char_monthly/generations/fixture/{name}.json',feature if name=='manifest' else {})
    pq.write_table(pa.Table.from_pylist(price_rows),root/'prices.parquet');pq.write_table(pa.Table.from_pylist(action_rows),root/'actions.parquet')
    source=write(root,'label-source.json',metadata('char_replication_label_source_v1',convention='unadjusted_close_split_cash_per_prior_share_v1',currency='USD',prices=product(root,'prices.parquet'),actions=product(root,'actions.parquet')))
    for label in labels:
        sid=label['security_id'];sources={'price_source':filepin(root,'prices.parquet'),'corporate_action_source':filepin(root,'actions.parquet')}
        mark={'session_date':str(formation),'security_id':sid,'cik':sid,'price':100.,'basis':'previous_month_close_total_return',**sources}
        observations=[{'session_date':str(day),'security_id':sid,'cik':sid,'total_return':sid*.001,**sources} for day in (entry,exit)]
        windows[f'{formation}/{sid}']=write(root,f'proofs/window-{sid}.json',metadata('char_monthly_complete_window_v1',formation=str(formation),security_id=sid,cik=sid,terminal_treatment='none',start_mark=mark,observations=observations))
    pq.write_table(pa.Table.from_pylist(audits),root/'formation.parquet');pq.write_table(pa.Table.from_pylist(labels),root/'labels.parquet')
    products.update(formation_quality=product(root,'formation.parquet'),labels=product(root,'labels.parquet'))
    fcontract=write(root,'formation-contract.json',metadata('char_monthly_formation_classification_v1',product=products['formation_quality'],classification_proofs=classifications))
    lcontract=write(root,'label-contract.json',metadata('char_monthly_label_contract_v1',product=products['labels'],windows=windows))
    binding={'version':'char_replication_bindings_v1','registry_sha256':FREEZE_SHA256,'config_sha256':R.CONFIG_SHA,'feature_generation':'char_monthly/generations/fixture','feature_manifest':N.R.identity(generation/'manifest.json'),'products':products,'jkp_contract':contracts['jkp'],'osap_contract':contracts['osap'],'formation_contract':fcontract,'label_contract':lcontract,'label_source_contract':source}
    write(root,'binding.json',binding)
    calls=[]
    def upstream(*args):calls.append(args[1]);return feature
    monkeypatch.setattr(N.M,'validate_generation',upstream)
    monkeypatch.setenv('ATX_GUARD_JOB','synthetic-fixture')
    return root,binding,feature,calls


def test_real_native_adapter_complete_month_and_source_economics(fixture,con):
    root,binding,feature,calls=fixture
    context=N.admit(root,binding,con,R.CONFIG_SHA)
    result=N.month(root,binding,context,con,'2013-01')
    assert calls==[root/'char_monthly/generations/fixture']
    assert result['population']['primary_rows']==30 and result['population']['unknown_rows']==0
    assert result['outcomes']['be_me']['own']>0 and result['outcomes']['age']['own']<0
    assert result['outcomes']['age']['jkp']==-.001  # already signed; never direction squared
    assert result['outcomes']['be_me']['osap']==pytest.approx(.001)  # percent -> decimal once; actual EW metadata
    assert result['outcomes']['eq_dur']['own'] is None
    assert len(result['outcomes'])==153 and len(result['proof_pins'])==62
    missing=N.month(root,binding,context,con,'2013-02')
    assert all(v['own'] is None and v['reason']=='missing_requested_feature_history' for v in missing['outcomes'].values())


@pytest.mark.parametrize('damage',['jkpslice','jkpvalue','double_sign','osap_weight','duplicate_month','future_label','partial_label','missing_inventory','catalog_exclusion','source_drift'])
def test_native_admission_or_replay_refuses_forgery(fixture,con,damage):
    root,binding,feature,calls=fixture
    if damage in ('jkpslice','jkpvalue'):
        file=root/'jkp.parquet';table=pq.read_table(file);rows=table.to_pylist()
        rows[0]['location' if damage=='jkpslice' else 'ret']='can' if damage=='jkpslice' else .99
        pq.write_table(pa.Table.from_pylist(rows,schema=table.schema),file);binding['products']['jkp']=product(root,'jkp.parquet')
        doc=E.read_metadata(root/'jkp-contract.json');doc['product']=binding['products']['jkp'];binding['jkp_contract']=write(root,'jkp-contract.json',doc)
    elif damage=='double_sign':
        doc=E.read_metadata(root/'jkp-contract.json');doc['entries']['age']['sign']=-1;binding['jkp_contract']=write(root,'jkp-contract.json',doc)
    elif damage=='osap_weight':
        doc=E.read_metadata(root/'osap-contract.json');doc['entries']['be_me']['weighting']='vw_cap';binding['osap_contract']=write(root,'osap-contract.json',doc)
    elif damage=='duplicate_month':
        with (root/'PredictorLSretWide.csv').open('a') as stream:stream.write('2013-01-15,1,-1\n')
        binding['products']['osap']=product(root,'PredictorLSretWide.csv');doc=E.read_metadata(root/'osap-contract.json');doc['product']=binding['products']['osap'];binding['osap_contract']=write(root,'osap-contract.json',doc)
    elif damage in ('future_label','partial_label'):
        table=pq.read_table(root/'labels.parquet');rows=table.to_pylist();rows[0]['exit_date']=dt.date(2023,1,1) if damage=='future_label' else dt.date(2013,1,15)
        pq.write_table(pa.Table.from_pylist(rows,schema=table.schema),root/'labels.parquet');binding['products']['labels']=product(root,'labels.parquet')
        doc=E.read_metadata(root/'label-contract.json');doc['product']=binding['products']['labels'];binding['label_contract']=write(root,'label-contract.json',doc)
    elif damage=='missing_inventory':
        doc=E.read_metadata(root/'osap-contract.json');doc['entries'].pop('eq_dur');binding['osap_contract']=write(root,'osap-contract.json',doc)
    elif damage=='catalog_exclusion':
        doc=E.read_metadata(root/'jkp-contract.json');doc['entries']['eq_dur']['reason']='changed after metadata freeze';binding['jkp_contract']=write(root,'jkp-contract.json',doc)
    else:(root/'prices.parquet').write_bytes(b'changed source')
    with pytest.raises((ValueError,RuntimeError)):
        context=N.admit(root,binding,con,R.CONFIG_SHA)
        N.month(root,binding,context,con,'2013-01')


def test_native_label_reconstructs_actual_close_split_cash_not_asserted_proof(fixture,con):
    root,binding,feature,calls=fixture;context=N.admit(root,binding,con,R.CONFIG_SHA)
    label=N.S.records(con,'SELECT * FROM cr_labels WHERE cik=1')[0]
    table=pq.read_table(root/'prices.parquet');rows=table.to_pylist();rows[1]['close']+=10
    pq.write_table(pa.Table.from_pylist(rows,schema=table.schema),root/'prices.parquet')
    source=context['label_source'];source['prices']=product(root,'prices.parquet')
    key=f"{label['formation']}/1";pin=context['labels']['windows'][key];doc=E.read_metadata(root/pin['path'])
    for row in [doc['start_mark']]+doc['observations']:row['price_source']=filepin(root,'prices.parquet')
    context['labels']['windows'][key]=write(root,pin['path'],doc)
    with pytest.raises(ValueError,match='economics'):
        N.native_label(root,label,context['calendar'],context['labels']['windows'],source,con)


def test_unknown_common_issuer_and_middle_window_cannot_shrink_portfolio(fixture,con):
    root,binding,feature,calls=fixture;context=N.admit(root,binding,con,R.CONFIG_SHA)
    key='2012-12-31/15';context['labels']['windows'].pop(key)
    result=N.month(root,binding,context,con,'2013-01')
    assert all(v['own'] is None and v['reason']=='missing_independent_complete_monthly_label' for v in result['outcomes'].values())
    file=context['generation']/'month=2012-12/char_monthly.parquet';table=pq.read_table(file);rows=table.to_pylist()
    row=dict(rows[0]);row.update(security_id=31,cik=31,issuer_me=131.);rows.append(row)
    pq.write_table(pa.Table.from_pylist(rows,schema=table.schema),file)
    result=N.month(root,binding,context,con,'2013-01')
    assert result['population']['common_rows']==31 and result['population']['unknown_rows']==1
    assert all(v['own'] is None and v['reason']=='unknown_independent_formation_population' for v in result['outcomes'].values())


@pytest.mark.parametrize('missing',['nyse','issuer_me'])
def test_every_issuer_size_reference_requires_independent_proof(fixture,con,missing):
    root,binding,feature,calls=fixture;context=N.admit(root,binding,con,R.CONFIG_SHA)
    pin=context['formation']['classification_proofs']['15'];doc=E.read_metadata(root/pin['path']);doc.pop(missing)
    context['formation']['classification_proofs']['15']=write(root,pin['path'],doc)
    result=N.month(root,binding,context,con,'2013-01')
    assert result['population']['primary_rows']==30
    assert all(v['own'] is None and v['reason']=='invalid_independent_issuer_size_cap' for v in result['outcomes'].values())


def test_external_binding_pin_full_source_snapshot_and_byte_drift(fixture,con):
    root,binding,feature,calls=fixture;target=root/'binding.json';expected=N.R.identity(target)['sha256']
    member=root/'upstream/member.txt';member.parent.mkdir();member.write_text('complete producer member')
    manifest=write(root,'upstream/manifest.json',{'files':{'member.txt':N.R.identity(member)}})
    write(root,'char_monthly/generations/fixture/snapshot.json',{'source':{'manifests':{'upstream/manifest.json':manifest['identity']},'external':{'extra':{'path':str(root/'upstream/manifest.json'),**manifest['identity']}}}})
    for external in (None,'0'*64):
        with pytest.raises(ValueError):R.freeze(root,target,con,external)
    with pytest.raises(ValueError):R.freeze(root,Path('binding.json'),con,expected)
    assert not calls
    snapshot,context=R.freeze(root,target,con,expected)
    assert snapshot['expected_bindings_sha256']==expected
    assert str((root/'prices.parquet').resolve()) in snapshot['inputs']
    assert str((root/'proofs/window-15.json').resolve()) in snapshot['inputs']
    assert str((root/'authoritative.txt').resolve()) in snapshot['inputs']
    assert str(member.resolve()) in snapshot['inputs']
    R.monthly_receipt(root,snapshot,context,con,'2013-01')
    (root/'authoritative.txt').write_text('tampered after externally admitted snapshot')
    with pytest.raises(ValueError,match='snapshot changed'):R.monthly_receipt(root,snapshot,context,con,'2013-02')


def test_typed_numeric_cells_and_identities_cannot_be_boolean():
    assert not N.finite(True) and not N.finite(False) and N.finite(1.)
    with pytest.raises(ValueError):N.identity_pin({'sha256':'0'*64,'bytes':True})
    with pytest.raises(ValueError):N.identity_pin({'sha256':'0'*64,'bytes':1,'invented':'marker'})
