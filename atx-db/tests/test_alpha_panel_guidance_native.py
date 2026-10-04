"""Synthetic native Task10 pipeline; never a source handcheck/PASS claim."""
import datetime as dt,hashlib,json
from pathlib import Path
import duckdb,pyarrow as pa,pyarrow.parquet as pq,pytest
from atx_db.alpha_panel import common as C,ea_actuals as E
from atx_db.alpha_panel import guidance_native as N,guidance_publication as P,guidance_sources as S,guidance_kernels as K
from atx_db.alpha_panel.guidance_contract import CONFIG_SHA256
UTC=dt.UTC

def stamp(day,hour=12):return dt.datetime(2021,day[0],day[1],hour,tzinfo=UTC)
def write(path,items,schema=None):
 path.parent.mkdir(parents=True,exist_ok=True);pq.write_table(pa.Table.from_pylist(items,schema=schema),path)
 return {'sha256':C.sha256_file(path),'bytes':path.stat().st_size}
@pytest.fixture
def fixture(tmp_path,monkeypatch):
 lake=tmp_path/'lake';monkeypatch.setattr(C,'build_root',lambda:lake)
 def connect(**kwargs):
  con=duckdb.connect();con.execute("SET memory_limit='128MB'");con.execute('SET threads=1');con.execute("SET TimeZone='UTC'");return con
 monkeypatch.setattr(C,'connect',connect)
 times=[stamp((4,1)),stamp((5,1)),stamp((7,15))]
 texts=[b'For the second quarter of 2021, the company expects GAAP diluted EPS of $1.00 to $1.20. For the second quarter of 2021, the company expects GAAP consolidated revenue of $100 million to $110 million.',b'For the second quarter of 2021, the company expects GAAP diluted EPS of $1.20 to $1.40. For the second quarter of 2021, the company expects GAAP consolidated revenue of $120 million to $130 million.',b'Actual second-quarter results.']
 hashes=[hashlib.sha256(t).hexdigest() for t in texts];bodies=dict(zip(hashes,texts));monkeypatch.setattr(N,'read_document',lambda document:bodies.get(document['doc_sha256']))
 filings=[{'cik':1,'accession':f'a{i+1}','filing_date':t.date(),'available_at':t,'acceptance_clock':'sec_acceptance','vintage_risk':'original','form':'8-K','is_amendment':False} for i,t in enumerate(times)]
 sec=lake/'sec_filings';files={name:write(sec/name,items) for name,items in [('filings.parquet',filings),('eight_k_items.parquet',[{'cik':1,'accession':f'a{i+1}','item':'2.02'} for i in range(3)])]};C.write_json_atomic(sec/'manifest.json',{'status':'complete','files':files})
 catalog=tmp_path/'catalog.parquet';write(catalog,[{'cik':1,'accession':f'a{i+1}','doc_sha256':h,'source_doc':f'doc{i+1}.htm','doc_source':'fixture','form':'8-K','is_amendment':False} for i,h in enumerate(hashes)])
 release={name:None for name in E.RELEASE_SCHEMA.names};release.update(cik=1,accession='a3',filing_date=times[2].date(),available_at=times[2].replace(tzinfo=None),source_available_at=times[2].replace(tzinfo=None),acceptance_clock='sec_acceptance',period_end=dt.date(2021,6,30),currency='USD',period_basis='source',period_days=91,fiscal_year_label=2021,fiscal_quarter=2,fiscal_calendar_id='fye12-31',epsfiq=1.5,saleq=130e6,eps_split_comparable=True,reason='qualified',source_doc=['doc3.htm'],doc_sha256=[hashes[2]])
 ea=lake/'ea_actuals';generation='generations/fixture';products={name:generation+'/'+name for name in ('releases.parquet','precision_sample.parquet')};files={products[name]:write(ea/products[name],[release],E.RELEASE_SCHEMA) for name in products};m={'schema':E.SCHEMA,'stage':E.STAGE,'code':C.code_identity('ea_actuals','ea_publication','events_sources','common'),'status':'complete','phase':'parsed','generation':generation,'products':products,'files':files,'parser_version':E.VERSION,'input_manifests_sha256':{'sec_filings':{'sha256':C.sha256_file(sec/'manifest.json')}},'receipt':{'catalog_sha256':C.sha256_file(catalog)}};C.write_json_atomic(ea/generation/'manifest.json',m);C.write_json_atomic(ea/'releases_manifest.json',m)
 targets=[]
 for i in range(2):
  for measure in ('EPS','REVENUE'):
   targets.append({'cik':1,'accession':f'a{i+1}','measure':measure,'basis':'GAAP','accounting_basis_id':'gaap-total','period_type':'Q','fiscal_year':2021,'fiscal_quarter':2,'period_end':dt.date(2021,6,30),'fiscal_calendar_id':'fye12-31','currency':'USD','unit':'USD_PER_SHARE' if measure=='EPS' else 'USD','share_basis':'diluted' if measure=='EPS' else 'consolidated','share_basis_id':'split-1','evidence_id':'independent-original-target','source_sha256':hashes[i],'source_available_at':times[i]})
 actual=[{**targets[0 if measure=='EPS' else 1],'accession':'a3','source_sha256':hashes[2],'source_available_at':times[2],'value_field':'epsfiq' if measure=='EPS' else 'saleq','source_value':1.5 if measure=='EPS' else 130e6,'security_id':101} for measure in ('EPS','REVENUE')]
 close=stamp((7,14),20);price=[{'cik':1,'accession':'a3','security_id':101,'session_date':close.date(),'close':10.,'close_at':close,'available_at':stamp((7,14),21),'source_available_at':stamp((7,14),21),'currency':'USD','basis_id':'split-1','exchange_id':'NYSE','qualification_reason':'qualified','evidence_id':'independent-price','source_sha256':'f'*64}]
 calendar=[{'session_date':close.date(),'close_at':close,'exchange_id':'NYSE','source_available_at':stamp((1,1)),'evidence_id':'pinned-exchange-calendar','source_sha256':'e'*64}]
 members=[{'year':year,'cik':cik} for year in range(2020,2026) for cik in (1,2)]
 paths={'catalog':catalog}
 for name,data in [('targets',targets),('actual_basis',actual),('prices',price),('calendar',calendar),('members',members)]:
  path=tmp_path/(name+'.parquet');write(path,data);paths[name]=path
 rules={'target_calendar':'exact historical source identity','currency':'USD original source declaration','eps_share_basis':'basic/diluted and original split identity','actual_basis':'exact GAAP/ADJUSTED/source metric identity','price_basis':'same currency/share split basis, positive original observed prior session','members':'independent full member issuer-year population2020-2025'}
 contract=tmp_path/'evidence_contract.json';C.write_json_atomic(contract,{'schema':'atx.guidance-independent-evidence/v1','status':'reviewed','purpose':'guidance_comparability','files':{name:C.sha256_file(paths[name]) for name in ('targets','actual_basis','prices','calendar','members')},'rules':rules,'member_counts':{str(y):2 for y in range(2020,2026)}});paths['evidence_contract']=contract
 sources={name:{'path':str(path),'sha256':C.sha256_file(path)} for name,path in paths.items()};sources['evidence_contract']['reviewed_sha256']=sources['evidence_contract']['sha256'];sources.update(sec_filings={'stage':'sec_filings','sha256':C.sha256_file(sec/'manifest.json')},ea_actuals={'stage':'ea_actuals','sha256':C.sha256_file(ea/'releases_manifest.json')})
 return {'spec':{'version':'guidance_bindings_v2','sources':sources},'root':tmp_path/N.STAGE,'paths':paths,'bodies':bodies,'hashes':hashes,'lake':lake}

def read_product(root,manifest,name):
 m=C.read_json(Path(manifest));return pq.read_table(root/m['products'][name]).to_pylist()
def test_full_native_restart_features_public_validation_and_original_population(fixture):
 f=fixture;partial=N.parse(f['spec'],f['root'],max_new_releases=1)
 assert partial['published'] is False and not (f['root']/'manifest.json').exists()
 parsed=N.parse(f['spec'],f['root'],max_new_releases=2)
 assert parsed['cache_hits']==1 and parsed['new_units']==2
 assert len(read_product(f['root'],parsed['manifest'],'releases.parquet'))==3
 guidance=read_product(f['root'],parsed['manifest'],'guidance.parquet');assert len(guidance)==4 and all(r['qualification_reason']=='qualified' for r in guidance)
 built=N.build(f['spec'],f['root']);features=read_product(f['root'],built['manifest'],'guidance_features.parquet')
 eps=next(r for r in features if r['event_kind']=='actual' and r['measure']=='EPS');assert eps['guid_surprise']==pytest.approx(.02) and eps['prior_accession']=='a2'
 revenue=next(r for r in features if r['event_kind']=='actual' and r['measure']=='REVENUE');assert revenue['guid_surprise'] is None and revenue['feature_reason']=='unit_mismatch_total_revenue'
 assert next(r for r in features if r['accession']=='a2' and r['measure']=='REVENUE')['guid_revision']==pytest.approx(20e6)
 m=C.read_json(Path(built['manifest']));assert m['precision']['manual_reviewed']==0 and m['gate_status']['precision']=='FAIL';assert m['coverage']['2021']['denominator']==2 and m['coverage']['2021']['guiding_issuers']==1
 assert len(read_product(f['root'],built['manifest'],'precision_sample.parquet'))==3
 assert N.validate(f['spec'],f['root'])['status']=='valid'

def test_unknown_basis_missing_source_and_clock_do_not_drop_original_release(fixture):
 f=fixture;f['bodies'].pop(f['hashes'][1]);parsed=N.parse(f['spec'],f['root']);releases=read_product(f['root'],parsed['manifest'],'releases.parquet')
 assert len(releases)==3 and next(r for r in releases if r['accession']=='a2')['n_qualified']==0
 built=N.build(f['spec'],f['root']);eps=next(r for r in read_product(f['root'],built['manifest'],'guidance_features.parquet') if r['event_kind']=='actual' and r['measure']=='EPS')
 assert eps['prior_accession']=='a1'  # latest qualified original source; missing a2 has no admitted comparator values

def test_phase_and_reviewed_mapping_pin_refuse(fixture):
 f=fixture;spec=json.loads(json.dumps(f['spec']));spec['sources']['evidence_contract'].pop('reviewed_sha256')
 with pytest.raises(ValueError,match='reviewed'):S.bind(spec)
 path=f['lake']/'ea_actuals/releases_manifest.json';m=C.read_json(path);m['phase']='features';C.write_json_atomic(path,m)
 with pytest.raises(RuntimeError,match='phase'):S.bind(f['spec'])

def test_authenticated_unit_and_generation_drift_refuse(fixture):
 f=fixture;N.parse(f['spec'],f['root'],max_new_releases=1);unit=next((f['root']/'_units').rglob('rows.parquet'));unit.write_bytes(unit.read_bytes()+b'corrupt')
 with pytest.raises(ValueError,match='byte identity'):N.parse(f['spec'],f['root'])

def test_latest_session_price_clock_equality_no_reaction_fallback(fixture):
 f=fixture;N.parse(f['spec'],f['root']);identity=S.bind(f['spec']);con=N.connection(identity)
 try:
  m=C.read_json(f['root']/'parse_manifest.json');N.table(con,'native_guidance',f['root']/m['products']['guidance.parquet'])
  con.execute('CREATE TEMP TABLE changed_prices AS SELECT * FROM g_prices');con.execute("UPDATE changed_prices SET available_at=TIMESTAMPTZ '2021-07-15 12:00:00+00'");con.execute('CREATE OR REPLACE TEMP VIEW g_prices AS SELECT * FROM changed_prices')
  eps=next(r for r in N.issuer_features(con,1) if r['event_kind']=='actual' and r['measure']=='EPS');assert eps['guid_surprise'] is None and eps['feature_reason']=='price_clock_or_session_unqualified'
 finally:con.close()


def test_unknown_target_clock_basis_unit_and_share_proof_null_all_values(fixture):
 from atx_db.alpha_panel import guidance as G
 f=fixture;filing={'cik':1,'form':'8-K','is_amendment':False,'accession':'a1','filing_date':dt.date(2021,4,1),'available_at':stamp((4,1)),'acceptance_clock':'sec_acceptance','vintage_risk':'original'}
 candidate=G.extract_guidance(f['bodies'][f['hashes'][0]].decode(),G.ReportedPeriod(None),2021)[0];document={'doc_sha256':f['hashes'][0],'source_doc':'doc1.htm'}
 proof=pq.read_table(f['paths']['targets']).to_pylist()[0]
 assert N.qualify(candidate,filing,document,[proof])['qualification_reason']=='qualified'
 for changed in ({'source_available_at':stamp((4,2))},{'currency':'CAD'},{'unit':'USD'},{'share_basis':'basic_unknown'},{'share_basis':'basic'},{'fiscal_calendar_id':None},{'share_basis_id':None}):
  row=N.qualify(candidate,filing,document,[{**proof,**changed}]);assert row['qualification_reason']!='qualified' and row['low'] is None and row['point'] is None
 assert N.qualify(candidate,filing,document,[proof,proof])['midpoint'] is None
 assert N.qualify(candidate,{**filing,'acceptance_clock':'unresolved'},document,[proof])['available_at'] is None

def test_source_drift_and_population_subset_do_not_publish(fixture,monkeypatch):
 f=fixture;original=N.read_document;once=[False]
 def mutate(document):
  result=original(document)
  if not once[0]:once[0]=True;f['paths']['members'].write_bytes(f['paths']['members'].read_bytes()+b'changed')
  return result
 monkeypatch.setattr(N,'read_document',mutate)
 with pytest.raises(ValueError,match='pin mismatch'):N.parse(f['spec'],f['root'])
 assert not (f['root']/'manifest.json').exists()

def test_atomic_interruption_retains_prior_generation(fixture,monkeypatch):
 f=fixture;N.parse(f['spec'],f['root']);built=N.build(f['spec'],f['root']);before=(f['root']/'manifest.json').read_bytes();old=Path(built['manifest']).read_bytes()
 def interrupt(*args,**kwargs):raise RuntimeError('injected before activation')
 monkeypatch.setattr(N.S,'recheck',interrupt)
 with pytest.raises(RuntimeError,match='injected'):N.build(f['spec'],f['root'])
 assert (f['root']/'manifest.json').read_bytes()==before and Path(built['manifest']).read_bytes()==old
 assert P.validate(f['root'])['phase']=='features'

def test_boolean_handcheck_assertions_cannot_certify_precision(fixture):
 f=fixture;parsed=N.parse(f['spec'],f['root']);sample=read_product(f['root'],parsed['manifest'],'precision_sample.parquet')
 audit={'schema':'atx.guidance-handchecks/v1','parse_manifest_sha256':C.sha256_file(Path(parsed['manifest'])),'config_sha256':CONFIG_SHA256,'review_kind':'independent_source_release_text','cases':[{'cik':r['cik'],'accession':r['accession'],'reviewer':'fixture','source_text_evidence':'fixture','sample_key':r['sample_key'],'correct':True} for r in sample]}
 path=f['root']/'assertions.json';C.write_json_atomic(path,audit);sha=C.sha256_file(path)
 with pytest.raises(ValueError,match='handcheck'):N.audit(f['root'],Path(parsed['manifest']),{'path':str(path),'sha256':sha,'reviewed_sha256':sha})
 assert N.audit(f['root'],Path(parsed['manifest']))['pass'] is False

def test_public_value_tampering_rejects_authenticated_output(fixture):
 f=fixture;N.parse(f['spec'],f['root']);built=N.build(f['spec'],f['root']);m=C.read_json(Path(built['manifest']));path=f['root']/m['products']['guidance_features.parquet'];items=pq.read_table(path).to_pylist();items[0]['guid_surprise']=99.;write(path,items,N.FEATURE_SCHEMA)
 with pytest.raises(ValueError,match='byte identity'):N.validate(f['spec'],f['root'])


def test_publication_lock_refuses_concurrent_writer_and_releases_after_error(tmp_path):
 with N.lock(tmp_path):
  with pytest.raises(RuntimeError,match='busy'):
   with N.lock(tmp_path):pass
 with pytest.raises(RuntimeError):
  with N.lock(tmp_path):raise RuntimeError('interrupted')
 with N.lock(tmp_path):pass


def test_fixed_manual_denominator_does_not_shrink_to_available_sample(fixture):
 f=fixture;parsed=N.parse(f['spec'],f['root']);sample=read_product(f['root'],parsed['manifest'],'precision_sample.parquet');native=read_product(f['root'],parsed['manifest'],'guidance.parquet')
 fields=('measure','basis','accounting_basis_id','period_type','fiscal_year','fiscal_quarter','period_end','fiscal_calendar_id','currency','unit','share_basis','share_basis_id','low','high','point')
 cases=[{'cik':r['cik'],'accession':r['accession'],'reviewer':'synthetic-only','source_text_evidence':'synthetic original text only; not an actual handcheck','source_sha256':r['document_sha256'],'sample_key':r['sample_key'],'expected_guidance':[{k:g[k] for k in fields} for g in native if g['accession']==r['accession']]} for r in sample]
 path=f['root']/'manual-fixture.json';C.write_json_atomic(path,{'schema':'atx.guidance-handchecks/v1','parse_manifest_sha256':C.sha256_file(Path(parsed['manifest'])),'config_sha256':CONFIG_SHA256,'review_kind':'independent_source_release_text','cases':cases});sha=C.sha256_file(path)
 result=N.audit(f['root'],Path(parsed['manifest']),{'path':str(path),'sha256':sha,'reviewed_sha256':sha})
 assert result['required']==300 and result['selected']==3 and result['manual_reviewed']==3 and result['precision']==.01 and result['pass'] is False

def test_amendment_is_new_issue_vintage_and_not_precision_denominator_replacement(fixture):
 f=fixture;sec=f['lake']/'sec_filings';filings=pq.read_table(sec/'filings.parquet').to_pylist();filings.append({**filings[1],'accession':'a4','form':'8-K/A','is_amendment':True,'available_at':stamp((5,15)),'filing_date':dt.date(2021,5,15),'vintage_risk':'amendment'})
 items=pq.read_table(sec/'eight_k_items.parquet').to_pylist()+[{'cik':1,'accession':'a4','item':'7.01'}];files={name:write(sec/name,data) for name,data in [('filings.parquet',filings),('eight_k_items.parquet',items)]};C.write_json_atomic(sec/'manifest.json',{'status':'complete','files':files});f['spec']['sources']['sec_filings']['sha256']=C.sha256_file(sec/'manifest.json')
 catalog=pq.read_table(f['paths']['catalog']).to_pylist();catalog.append({**catalog[0],'accession':'a4','form':'8-K/A','is_amendment':True});write(f['paths']['catalog'],catalog);f['spec']['sources']['catalog']['sha256']=C.sha256_file(f['paths']['catalog'])
 targets=pq.read_table(f['paths']['targets']).to_pylist();targets.extend({**r,'accession':'a4','source_available_at':stamp((5,15))} for r in targets[:2]);write(f['paths']['targets'],targets);f['spec']['sources']['targets']['sha256']=C.sha256_file(f['paths']['targets'])
 contract=C.read_json(f['paths']['evidence_contract']);contract['files']['targets']=f['spec']['sources']['targets']['sha256'];C.write_json_atomic(f['paths']['evidence_contract'],contract);sha=C.sha256_file(f['paths']['evidence_contract']);f['spec']['sources']['evidence_contract'].update(sha256=sha,reviewed_sha256=sha)
 ea=f['lake']/'ea_actuals';m=C.read_json(ea/'releases_manifest.json');m['input_manifests_sha256']['sec_filings']['sha256']=f['spec']['sources']['sec_filings']['sha256'];m['receipt']['catalog_sha256']=f['spec']['sources']['catalog']['sha256'];C.write_json_atomic(ea/m['generation']/'manifest.json',m);C.write_json_atomic(ea/'releases_manifest.json',m);f['spec']['sources']['ea_actuals']['sha256']=C.sha256_file(ea/'releases_manifest.json')
 parsed=N.parse(f['spec'],f['root']);guidance=read_product(f['root'],parsed['manifest'],'guidance.parquet');assert len(guidance)==6 and len(read_product(f['root'],parsed['manifest'],'precision_sample.parquet'))==3
 amendment=next(r for r in guidance if r['accession']=='a4' and r['measure']=='EPS');assert amendment['is_amendment'] and amendment['available_at']==stamp((5,15))
 built=N.build(f['spec'],f['root']);eps=next(r for r in read_product(f['root'],built['manifest'],'guidance_features.parquet') if r['event_kind']=='actual' and r['measure']=='EPS');assert eps['prior_accession']=='a4' and eps['guid_surprise']==pytest.approx(.04)
 assert N.validate(f['spec'],f['root'])['status']=='valid'


def test_bounded_cache_reader_gzip_limit_sha_paths_and_no_mutation(tmp_path,monkeypatch):
 import gzip
 monkeypatch.setattr(N.ES,'EARNINGS_RELEASE_DIR',tmp_path)
 monkeypatch.setitem(N.CONFIG,'limits',{**N.CONFIG['limits'],'document_bytes':64})
 blob=b'original';sha=hashlib.sha256(blob).hexdigest();path=tmp_path/'objects'/sha[:2]/(sha+'.gz');path.parent.mkdir(parents=True);path.write_bytes(gzip.compress(blob));before=path.read_bytes()
 assert N.read_document({'doc_source':'landed_v2','doc_sha256':sha})==blob and path.read_bytes()==before
 huge=b'x'*1000;h=hashlib.sha256(huge).hexdigest();large=tmp_path/'objects'/h[:2]/(h+'.gz');large.parent.mkdir(parents=True,exist_ok=True);large.write_bytes(gzip.compress(huge))
 with pytest.raises(N.DocumentBoundExceeded):N.read_document({'doc_source':'landed_v2','doc_sha256':h})
 with pytest.raises(ValueError,match='identity'):N.read_document({'doc_source':'landed_v2','doc_sha256':'../escape'})
 with pytest.raises(ValueError,match='source'):N.read_document({'doc_source':'unknown','doc_sha256':sha})

def test_one_sided_values_preserved_without_inventing_midpoint(fixture):
 from atx_db.alpha_panel import guidance as G
 f=fixture;filing={'cik':1,'form':'8-K','is_amendment':False,'accession':'a1','filing_date':dt.date(2021,4,1),'available_at':stamp((4,1)),'acceptance_clock':'sec_acceptance','vintage_risk':'original'}
 text='For the second quarter of 2021, the company expects GAAP diluted EPS of at least $1.00.'
 candidates=G.extract_guidance(text,G.ReportedPeriod(None),2021);assert candidates
 proof=pq.read_table(f['paths']['targets']).to_pylist()[0]
 row=N.qualify(candidates[0],filing,{'doc_sha256':f['hashes'][0],'source_doc':'doc1.htm'},[proof])
 assert row['qualification_reason']=='qualified' and row['low']==1. and row['high'] is None and row['point'] is None and row['midpoint'] is None


def test_frozen_artifacts_primary_eps_and_sealed_evaluation_policy():
 from atx_db.alpha_panel.guidance_contract import CONFIG
 base=Path(__file__).resolve().parents[1]/'docs/datasets';config=C.read_json(base/'guidance.config.json');registry=C.read_json(base/'guidance.registry.json')
 assert config['config_sha256']==registry['config_sha256']==CONFIG_SHA256
 assert config['configuration']==CONFIG and config['bindings']==N.contract()['source_fields']
 assert CONFIG['candidate']['measure']=='EPS' and CONFIG['candidate']['economic_sign']==1
 assert CONFIG['evaluation_end_exclusive']=='2023-01-01' and CONFIG['precision']['required']==300 and CONFIG['precision']['threshold']==.90
 assert CONFIG['coverage']['years']==list(range(2020,2026))
 assert registry['required_phases']['guidance_features.parquet']==['features','validated']


def test_native_cli_contract_is_read_only_and_missing_bindings_never_legacy_fallback(tmp_path,monkeypatch,capsys):
 from atx_db.alpha_panel import guidance as G
 def unexpected(*args,**kwargs):raise AssertionError('legacy or data operation invoked')
 monkeypatch.setattr(G,'build',unexpected);monkeypatch.setattr(C,'connect',unexpected)
 assert G.main(['--native-phase','contract'])==0 and json.loads(capsys.readouterr().out)['config_sha256']==CONFIG_SHA256
 with pytest.raises(SystemExit):G.main([])
 with pytest.raises(ValueError,match='isolated'):N.parse({},tmp_path/'events')


def test_independently_pinned_actual_value_mismatch_cannot_fill_surprise(fixture):
 f=fixture;N.parse(f['spec'],f['root']);identity=S.bind(f['spec']);con=N.connection(identity)
 try:
  m=C.read_json(f['root']/'parse_manifest.json');N.table(con,'native_guidance',f['root']/m['products']['guidance.parquet']);con.execute('CREATE TEMP TABLE bad_actual_basis AS SELECT * FROM g_actual_basis');con.execute("UPDATE bad_actual_basis SET source_value=9 WHERE measure='EPS'");con.execute('CREATE OR REPLACE TEMP VIEW g_actual_basis AS SELECT * FROM bad_actual_basis')
  eps=next(r for r in N.issuer_features(con,1) if r['event_kind']=='actual' and r['measure']=='EPS');assert eps['actual_value'] is None and eps['guid_surprise'] is None and eps['qualification_reason']=='independent_actual_source_value_mismatch'
 finally:con.close()


def test_unqualified_native_actual_split_basis_cannot_be_overridden_by_metadata(fixture):
 f=fixture;N.parse(f['spec'],f['root']);con=N.connection(S.bind(f['spec']))
 try:
  m=C.read_json(f['root']/'parse_manifest.json');N.table(con,'native_guidance',f['root']/m['products']['guidance.parquet']);con.execute('CREATE TEMP TABLE unqualified_actual AS SELECT * FROM g_ea_actuals');con.execute('UPDATE unqualified_actual SET eps_split_comparable=false');con.execute('CREATE OR REPLACE TEMP VIEW g_ea_actuals AS SELECT * FROM unqualified_actual')
  eps=next(r for r in N.issuer_features(con,1) if r['event_kind']=='actual' and r['measure']=='EPS');assert eps['actual_value'] is None and eps['guid_surprise'] is None and eps['qualification_reason']=='native_actual_eps_share_basis_unqualified'
 finally:con.close()
