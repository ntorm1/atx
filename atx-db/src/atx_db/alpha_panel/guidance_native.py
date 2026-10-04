"""Bounded native Task10 parsing, comparability features and public validation."""
from __future__ import annotations
import datetime as dt,hashlib,json,os,re,gzip,zlib
from contextlib import contextmanager
from pathlib import Path
import pyarrow as pa,pyarrow.parquet as pq
from . import common as C,guidance as G,events_sources as ES
from . import guidance_sources as S,guidance_publication as P,guidance_kernels as K
from .guidance_contract import CONFIG,CONFIG_SHA256,digest
STAGE='events_guidance_v2'
TEXT=['candidate_id','accession','source_doc','doc_sha256','measure','basis','accounting_basis_id','period_type','fiscal_calendar_id','currency','unit','share_basis','share_basis_id','value_type','evidence','qualification_reason']
GUIDANCE_SCHEMA=pa.schema([('cik',pa.int64()),('form',pa.string()),('is_amendment',pa.bool_()),('filing_date',pa.date32()),('period_end',pa.date32()),('fiscal_year',pa.int32()),('fiscal_quarter',pa.int32()),('available_at',pa.timestamp('us',tz='UTC')),('source_available_at',pa.timestamp('us',tz='UTC')), *((n,pa.string()) for n in TEXT), *((n,pa.float64()) for n in ('low','high','point','midpoint'))])
RELEASE_SCHEMA=pa.schema([('cik',pa.int64()),('form',pa.string()),('is_amendment',pa.bool_()),('accession',pa.string()),('filing_date',pa.date32()),('available_at',pa.timestamp('us',tz='UTC')),('source_available_at',pa.timestamp('us',tz='UTC')),('n_documents',pa.int32()),('n_candidates',pa.int32()),('n_qualified',pa.int32()),('reason',pa.string()),('sample_key',pa.string()),('document_sha256',pa.list_(pa.string()))])
FEATURE_SCHEMA=pa.schema([*GUIDANCE_SCHEMA,('event_kind',pa.string()),('guid_revision',pa.float64()),('guid_surprise',pa.float64()),('actual_value',pa.float64()),('prior_accession',pa.string()),('feature_reason',pa.string()),('security_id',pa.int64()),('price_session',pa.date32()),('price_close',pa.float64()),('price_available_at',pa.timestamp('us',tz='UTC'))])

def sqlpath(path):return str(path).replace('\\','/').replace("'","''")
def table(con,name,path):con.execute(f"CREATE OR REPLACE TEMP VIEW {name} AS SELECT * FROM read_parquet('{sqlpath(path)}')")
def rows(con,query,parameters=()):
 cursor=con.execute(query,parameters);names=[c[0] for c in cursor.description]
 return [dict(zip(names,r)) for r in cursor.fetchall()]
@contextmanager
def lock(root):
 root.mkdir(parents=True,exist_ok=True);path=root/'.publication.lock';handle=open(path,'a+b')
 try:
  handle.seek(0);handle.write(b'0');handle.flush();handle.seek(0)
  if os.name=='nt':
   import msvcrt
   try:msvcrt.locking(handle.fileno(),msvcrt.LK_NBLCK,1)
   except OSError:raise RuntimeError('guidance publication busy')
  else:
   import fcntl
   try:fcntl.flock(handle.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
   except OSError:raise RuntimeError('guidance publication busy')
  yield
 finally:handle.close()  # OS releases ownership even after interruption; persistent lock file is harmless.
def connection(identity):
 con=C.connect(memory='128MB',threads=1,db_file='guidance-native-'+str(os.getpid())+'.duckdb')
 for name,path in identity['files'].items():
  if name!='evidence_contract':table(con,'g_'+name.replace('.','_'),path)
 for year,expected in identity['member_counts'].items():
  if con.execute('SELECT count(DISTINCT cik) FROM g_members WHERE year=?',[int(year)]).fetchone()[0]!=expected:raise ValueError('independent member population count mismatch')
 if con.execute('SELECT count(*) FROM (SELECT exchange_id,session_date FROM g_calendar GROUP BY 1,2 HAVING count(*)>1)').fetchone()[0]:raise ValueError('ambiguous exchange calendar identity')
 return con

def evidence_ok(proof,tau,sha=None):
 return bool(proof and proof.get('evidence_id') and proof.get('source_sha256') and bool(re.fullmatch('[0-9a-f]{64}',proof['source_sha256'])) and K.clock(proof.get('source_available_at')) is not None and K.clock(proof['source_available_at'])<=tau and (sha is None or proof['source_sha256']==sha))
def qualify(candidate,filing,document,proofs):
 tau=K.clock(filing.get('available_at')) if filing.get('acceptance_clock') and 'unresolved' not in filing.get('acceptance_clock','') and 'unresolved' not in (filing.get('vintage_risk') or '') else None
 row={n:None for n in GUIDANCE_SCHEMA.names};row.update(candidate_id=digest({'document':document['doc_sha256'],'candidate':vars(candidate)}),form=filing['form'],is_amendment=filing['is_amendment'],cik=filing['cik'],accession=filing['accession'],filing_date=filing['filing_date'],available_at=tau,source_available_at=K.clock(filing.get('available_at')),source_doc=document.get('source_doc'),doc_sha256=document['doc_sha256'],measure=candidate.measure,basis=candidate.basis,period_type=candidate.period_type,fiscal_year=candidate.fiscal_year,fiscal_quarter=candidate.fiscal_quarter,value_type=candidate.value_type,evidence=candidate.evidence,unit=candidate.unit)
 reason='qualified'
 if tau is None:reason='original_issue_clock_unqualified'
 elif candidate.period_basis!='stated' or candidate.fiscal_year is None:reason='target_inferred_or_unknown'
 elif candidate.basis not in ('GAAP','ADJUSTED'):reason='accounting_basis_unspecified'
 elif len(proofs)!=1 or not evidence_ok(proofs[0],tau,document['doc_sha256']):reason='target_currency_share_evidence_missing_or_ambiguous'
 else:
  p=proofs[0];row.update({k:p.get(k) for k in ('period_end','fiscal_calendar_id','currency','share_basis','share_basis_id','accounting_basis_id')})
  if p.get('unit')!=candidate.unit or p.get('currency')!='USD' or not p.get('fiscal_calendar_id') or not p.get('accounting_basis_id') or not p.get('share_basis_id') or p.get('period_end') is None:reason='target_currency_unit_basis_unqualified'
  elif candidate.period_type not in ('Q','FY') or (candidate.period_type=='Q' and candidate.fiscal_quarter not in (1,2,3,4)) or (candidate.period_type=='FY' and candidate.fiscal_quarter is not None):reason='fiscal_target_identity_invalid'
  elif p['period_end']<tau.date():reason='target_not_forward'
  elif candidate.measure=='EPS' and (p.get('share_basis') not in ('basic','diluted') or set(re.findall(r'\b(basic|diluted)\b',candidate.evidence.lower()))!={p.get('share_basis')}):reason='eps_share_basis_unknown_or_incompatible'
  elif candidate.measure=='REVENUE' and p.get('share_basis')!='consolidated':reason='revenue_scope_unqualified'
  elif candidate.value_type not in ('range','point','floor','ceiling'):reason='guidance_value_type_unsupported'
  elif candidate.value_type=='floor' and not K.finite(candidate.low):reason='invalid_guidance_floor'
  elif candidate.value_type=='ceiling' and not K.finite(candidate.high):reason='invalid_guidance_ceiling'
  elif candidate.value_type=='range' and (not K.finite(candidate.low) or not K.finite(candidate.high) or candidate.low>candidate.high):reason='invalid_guidance_range'
  elif candidate.value_type=='point' and not K.finite(candidate.point):reason='invalid_guidance_point'
 row['qualification_reason']=reason
 if reason=='qualified':row.update(low=candidate.low if candidate.value_type in ('range','floor') else None,high=candidate.high if candidate.value_type in ('range','ceiling') else None,point=candidate.point if candidate.value_type=='point' else None);row['midpoint']=K.midpoint(row)
 return row


class DocumentBoundExceeded(ValueError):pass

def read_document(document):
 """Cache-only bounded streaming read; never load a ledger, request HTTP or mutate raw sources."""
 sha=document.get('doc_sha256','')
 if not re.fullmatch('[0-9a-f]{64}',sha):raise ValueError('invalid catalog object identity')
 source=document.get('doc_source')
 if source=='landed_v2':root=ES.EARNINGS_RELEASE_DIR
 elif source=='sec_events':root=ES.RAW_DIR
 else:raise ValueError('unsupported native document source')
 for compressed in (True,False):
  path=root/'objects'/sha[:2]/(sha+'.gz' if compressed else sha)
  try:
   with (gzip.open(path,'rb') if compressed else path.open('rb')) as handle:blob=handle.read(CONFIG['limits']['document_bytes']+1)
  except FileNotFoundError:continue
  except (OSError,EOFError,zlib.error):return None
  if len(blob)>CONFIG['limits']['document_bytes']:raise DocumentBoundExceeded('source document bound exceeded')
  return blob
 return None

def parse(spec,root,*,max_new_releases=None):
 if root.name!=STAGE:raise ValueError('isolated events_guidance_v2 destination required')
 if max_new_releases is not None and max_new_releases<0:raise ValueError('nonnegative new-release budget required')
 identity=S.bind(spec);frozen={'inputs':identity,'config_sha256':CONFIG_SHA256,'code':P.code()};run_key=digest(frozen);cache=root/'_units'/run_key
 with lock(root):
  con=connection(identity);work=None
  try:
   con.execute("CREATE TEMP VIEW eligible AS SELECT DISTINCT f.cik,f.accession,f.form,f.is_amendment,f.filing_date,f.available_at,f.acceptance_clock,f.vintage_risk FROM g_filings_parquet f WHERE f.form IN ('8-K','8-K/A') AND f.filing_date>=DATE '2019-01-01' AND EXISTS (SELECT 1 FROM g_eight_k_items_parquet i WHERE i.cik=f.cik AND i.accession=f.accession AND i.item IN ('2.02','7.01'))")
   # Original issuer/accession identity must be unique; conflicting SEC vintages refuse.
   if con.execute('SELECT count(*) FROM (SELECT cik,accession FROM eligible GROUP BY 1,2 HAVING count(*)>1)').fetchone()[0]:raise ValueError('ambiguous original filing identity')
   work=P.create(root);writers={name:pq.ParquetWriter(work/name,schema,compression='zstd') for name,schema in [('guidance.parquet',GUIDANCE_SCHEMA),('releases.parquet',RELEASE_SCHEMA)]};sample=[];new=hits=offset=total=0
   try:
    while True:
     batch=rows(con,'SELECT * FROM eligible ORDER BY cik,accession LIMIT 512 OFFSET ?',[offset])
     if not batch:break
     offset+=len(batch)
     for filing in batch:
      key=digest([filing['cik'],filing['accession']]);unit_identity={'frozen':run_key,'filing':digest(filing)};data=P.unit(cache,key,unit_identity)
      audit_path=cache/key/'release.json'
      if data is not None:
       audit=C.read_json(audit_path)
       receipt=C.read_json(cache/key/'receipt.json')
       if digest(audit)!=receipt.get('audit_sha256'):raise ValueError('unit release audit mismatch')
       parsed=pq.read_table(data).to_pylist();hits+=1
      else:
       if max_new_releases is not None and new>=max_new_releases:
        S.recheck(spec,identity);return {'status':'partial_cache_only','published':False,'run_key':run_key,'new_units':new,'cache_hits':hits}
       documents=rows(con,'SELECT * FROM g_catalog WHERE cik=? AND accession=? ORDER BY doc_sha256 LIMIT 65',[filing['cik'],filing['accession']])
       if len(documents)>CONFIG['limits']['documents_per_release']:raise ValueError('release document bound exceeded')
       parsed=[];seen=set();reasons=[]
       for document in documents:
        sha=document['doc_sha256']
        if sha in seen:continue
        seen.add(sha)
        try:blob=read_document(document)
        except DocumentBoundExceeded:reasons.append('document_size_unsupported');continue
        except ValueError:reasons.append('document_source_unsupported');continue
        if blob is None or hashlib.sha256(blob).hexdigest()!=sha:reasons.append('missing_or_corrupt_source');continue
        if len(blob)>CONFIG['limits']['document_bytes']:reasons.append('document_size_unsupported');continue
        text=ES.document_text(blob,document.get('source_doc') or '')
        candidates=G.extract_guidance(text,G.ReportedPeriod(None),filing['filing_date'].year)
        if len(candidates)>CONFIG['limits']['candidates_per_document']:reasons.append('candidate_bound_unsupported');continue
        for candidate in candidates:
         proofs=rows(con,'SELECT * FROM g_targets WHERE cik=? AND accession=? AND measure=? AND basis=? AND period_type=? AND fiscal_year=? AND fiscal_quarter IS NOT DISTINCT FROM ? LIMIT 2',[filing['cik'],filing['accession'],candidate.measure,candidate.basis,candidate.period_type,candidate.fiscal_year,candidate.fiscal_quarter])
         parsed.append(qualify(candidate,filing,document,proofs))
       # Conflicting equal-key values across exhibits are all retained, explicitly NULL.
       groups={}
       for r in parsed:groups.setdefault(tuple(r.get(k) for k in K.KEY),[]).append(r)
       for group in groups.values():
        if len({(r['low'],r['high'],r['point'],r['qualification_reason']) for r in group})>1:
         for r in group:r.update(low=None,high=None,point=None,midpoint=None,qualification_reason='conflicting_original_exhibits')
       parsed=list({r['candidate_id']:r for r in parsed}.values())
       qualified=sum(r['qualification_reason']=='qualified' for r in parsed)
       audit={'form':filing['form'],'is_amendment':filing['is_amendment'],'cik':filing['cik'],'accession':filing['accession'],'filing_date':filing['filing_date'],'available_at':K.clock(filing['available_at']) if filing['acceptance_clock'] and 'unresolved' not in filing['acceptance_clock'] and 'unresolved' not in (filing['vintage_risk'] or '') else None,'source_available_at':K.clock(filing['available_at']),'document_sha256':sorted(seen),'n_documents':len(documents),'n_candidates':len(parsed),'n_qualified':qualified,'reason':'qualified' if qualified else ';'.join(sorted(set(reasons+[r['qualification_reason'] for r in parsed]))) or 'missing_or_no_supported_guidance','sample_key':hashlib.sha256((str(filing['cik'])+'/'+filing['accession']+'/task10-precision-300-v1').encode()).hexdigest()}
       data=P.unit(cache,key,unit_identity,parsed,GUIDANCE_SCHEMA,audit);new+=1
      # JSON audit dates are converted by explicit schema after authenticated resume.
      for name in ('filing_date',):
       if isinstance(audit[name],str):audit[name]=dt.date.fromisoformat(audit[name])
      for name in ('available_at','source_available_at'):audit[name]=K.clock(audit.get(name))
      writers['guidance.parquet'].write_table(pa.Table.from_pylist(parsed,schema=GUIDANCE_SCHEMA));writers['releases.parquet'].write_table(pa.Table.from_pylist([audit],schema=RELEASE_SCHEMA));total+=1
      if not audit['is_amendment'] and 2020<=audit['filing_date'].year<=2025:
       sample.append(audit);sample=sorted(sample,key=lambda r:r['sample_key'])[:300]
   finally:
    for writer in writers.values():writer.close()
   P.write(work/'precision_sample.parquet',sample,RELEASE_SCHEMA);S.recheck(spec,identity)
   payload={'frozen':frozen,'input_manifests_sha256':identity['bindings'],'run_key':run_key,'release_count':total,'precision_sample':{'required':300,'selected':len(sample),'manual_reviewed':0,'selection':CONFIG['precision']['selection']},'gate_status':{'precision':'FAIL_unreviewed','coverage':'FAIL_unmeasured'}}
   manifest=P.publish(root,'parsed',work,payload,('guidance.parquet','releases.parquet','precision_sample.parquet'))
   return {'status':'complete','manifest':str(manifest),'new_units':new,'cache_hits':hits,'published':True}
  finally:con.close()


def actual_events(con,cik):
 output=[]
 originals=rows(con,'SELECT * FROM g_ea_actuals WHERE cik=? ORDER BY available_at,accession LIMIT 10001',[cik])
 if len(originals)>CONFIG['limits']['rows_per_issuer']:raise ValueError('issuer actual history bound exceeded')
 for release in originals:
  proofs=rows(con,'SELECT * FROM g_actual_basis WHERE cik=? AND accession=? ORDER BY measure,basis LIMIT 10001',[cik,release['accession']])
  if not proofs:
   proofs=[{'measure':measure,'basis':'GAAP','period_type':'Q','period_end':release.get('period_end'),'fiscal_year':release.get('fiscal_year_label'),'fiscal_quarter':release.get('fiscal_quarter'),'fiscal_calendar_id':release.get('fiscal_calendar_id'),'currency':release.get('currency'),'unit':'USD_PER_SHARE' if measure=='EPS' else 'USD','share_basis':None,'share_basis_id':None} for measure in ('EPS','REVENUE')]
  for proof in proofs:
   tau=K.clock(release.get('available_at'));reason='qualified'
   row={n:None for n in GUIDANCE_SCHEMA.names};row.update(candidate_id=digest({'release':release['accession'],'basis_proof':proof}),form='8-K',is_amendment=False,cik=cik,accession=release['accession'],filing_date=release['filing_date'],available_at=tau,source_available_at=K.clock(release.get('source_available_at')),qualification_reason='qualified')
   row.update({k:proof.get(k) for k in K.KEY if k!='cik'})
   field=proof.get('value_field');value=release.get(field) if field in ('epsfiq','saleq') else None
   if tau is None or not evidence_ok(proof,tau) or proof['source_sha256'] not in (release.get('doc_sha256') or []):reason='actual_original_evidence_unqualified'
   elif row['basis'] not in ('GAAP','ADJUSTED') or row['currency']!='USD' or not row['fiscal_calendar_id'] or not row['accounting_basis_id'] or not row['share_basis_id']:reason='actual_accounting_currency_share_basis_unknown'
   elif row['period_type']!='Q' or row['fiscal_year'] is None or row['fiscal_quarter'] not in (1,2,3,4) or row['period_end']!=release.get('period_end') or row['fiscal_year']!=release.get('fiscal_year_label') or row['fiscal_quarter']!=release.get('fiscal_quarter') or row['fiscal_calendar_id']!=release.get('fiscal_calendar_id'):reason='actual_target_calendar_incompatible'
   elif row['basis']!='GAAP':reason='native_actuals_adjusted_basis_unsupported'
   elif row['measure']=='EPS' and not release.get('eps_split_comparable'):reason='native_actual_eps_share_basis_unqualified'
   elif row['measure']=='EPS' and (field!='epsfiq' or row['unit']!='USD_PER_SHARE' or row['share_basis']!='diluted'):reason='native_actual_eps_share_basis_incompatible'
   elif row['measure']=='REVENUE' and (field!='saleq' or row['unit']!='USD' or row['share_basis']!='consolidated'):reason='native_actual_revenue_basis_incompatible'
   elif row['measure'] not in ('EPS','REVENUE') or not K.finite(value):reason='actual_value_missing'
   elif not K.finite(proof.get('source_value')) or proof['source_value']!=value:reason='independent_actual_source_value_mismatch'
   elif row['period_end'] is None or row['period_end']>=tau.date():reason='actual_period_not_completed'
   row.update(qualification_reason=reason,actual_value=value if reason=='qualified' else None,event_kind='actual',source_doc=';'.join(release.get('source_doc') or []),doc_sha256=proof.get('source_sha256'),security_id=proof.get('security_id'))
   output.append(row)
 # Duplicated or contradictory source-basis qualifications do not count as independent actuals.
 keys={}
 for row in output:keys.setdefault((row['accession'],tuple(row.get(k) for k in K.KEY)),[]).append(row)
 for group in keys.values():
  if len(group)>1:
   for row in group:row.update(actual_value=None,qualification_reason='ambiguous_actual_basis_mapping')
 return list({row['candidate_id']:row for row in output}.values())

def issuer_features(con,cik):
 guidance=rows(con,'SELECT * FROM native_guidance WHERE cik=? ORDER BY available_at,accession LIMIT 10001',[cik])
 if len(guidance)>CONFIG['limits']['rows_per_issuer']:raise ValueError('issuer guidance history bound exceeded')
 output=[]
 for row in guidance:output.append({**row,**K.revision(row,guidance),'event_kind':'guidance'})
 for actual in actual_events(con,cik):
  prices=rows(con,'SELECT * FROM g_prices WHERE cik=? AND accession=? LIMIT 2',[cik,actual['accession']])
  price=prices[0] if len(prices)==1 else None;last=None
  if price and actual['available_at'] is not None:
   sessions=rows(con,'SELECT * FROM g_calendar WHERE exchange_id=? AND close_at<? ORDER BY close_at DESC LIMIT 1',[price.get('exchange_id'),actual['available_at']])
   if sessions:
    session=sessions[0]
    if evidence_ok(session,actual['available_at']) and price.get('close_at')==session['close_at'] and evidence_ok(price,actual['available_at']) and price.get('security_id')==actual.get('security_id'):last=session['session_date']
  result=K.surprise(actual,guidance,price,last)
  if actual['measure']=='REVENUE':result.update(guid_surprise=None,feature_reason='unit_mismatch_total_revenue')
  output.append({**actual,**result,'price_session':last,'price_close':price.get('close') if price else None,'price_available_at':K.clock(price.get('available_at')) if price else None})
 return output

def coverage(con):
 if con.execute('SELECT count(*) FROM (SELECT year,cik FROM g_members GROUP BY 1,2 HAVING count(*)>1)').fetchone()[0]:raise ValueError('duplicate full member issuer-year denominator')
 output={}
 for year in CONFIG['coverage']['years']:
  expected={r[0] for r in con.execute('SELECT cik FROM g_members WHERE year=?',[year]).fetchall()}
  guiding={r[0] for r in con.execute("SELECT DISTINCT cik FROM native_guidance WHERE year(filing_date)=? AND qualification_reason='qualified' AND (low IS NOT NULL OR high IS NOT NULL OR point IS NOT NULL)",[year]).fetchall()}
  output[str(year)]={'population':'member_issuers','denominator':len(expected),'guiding_issuers':len(expected&guiding),'share':len(expected&guiding)/len(expected) if expected else None,'status':'MEASURED' if expected else 'FAIL_missing_member_population','threshold':None}
 return output

def audit(root,parse_manifest,manual=None):
 m=P.validate(root,parse_manifest);sample=pq.read_table(root/m['products']['precision_sample.parquet']).to_pylist();expected={(r['cik'],r['accession']):r for r in sample}
 result={'required':300,'selected':len(sample),'manual_reviewed':0,'correct':0,'precision':None,'pass':False,'reason':'no_independent_source_handchecks'}
 if manual is None:return result
 path=S.pinned(manual.get('path',''),manual.get('sha256'))
 if manual.get('reviewed_sha256')!=manual.get('sha256'):raise ValueError('separately expected manual audit pin required')
 data=C.read_json(path)
 if data.get('schema')!='atx.guidance-handchecks/v1' or data.get('parse_manifest_sha256')!=C.sha256_file(parse_manifest) or data.get('config_sha256')!=CONFIG_SHA256 or data.get('review_kind')!='independent_source_release_text':raise ValueError('incompatible manual source audit')
 judgments=data.get('cases',[]);seen=set();correct=0
 for case in judgments:
  key=(case.get('cik'),case.get('accession'))
  if key not in expected or key in seen:raise ValueError('missing/extra/duplicate precision audit identity')
  if not case.get('reviewer') or not case.get('source_text_evidence') or not isinstance(case.get('expected_guidance'),list) or case.get('sample_key')!=expected[key]['sample_key']:raise ValueError('unqualified independent handcheck')
  source_hashes=case.get('source_sha256',[])
  if not source_hashes or set(source_hashes)!=set(expected[key]['document_sha256']):raise ValueError('handcheck original source identity mismatch')
  fields=('measure','basis','accounting_basis_id','period_type','fiscal_year','fiscal_quarter','period_end','fiscal_calendar_id','currency','unit','share_basis','share_basis_id','low','high','point')
  native=[]
  for batch in pq.ParquetFile(root/m['products']['guidance.parquet']).iter_batches(batch_size=512):
   native.extend({field:r.get(field) for field in fields} for r in batch.to_pylist() if (r['cik'],r['accession'])==key and r['qualification_reason']=='qualified')
  if any(set(r)!=set(fields) for r in case['expected_guidance']):raise ValueError('incomplete independent field judgments')
  seen.add(key);correct+=set(map(digest,native))==set(map(digest,case['expected_guidance']))
 if seen!=set(expected):raise ValueError('incomplete fixed manual sample')
 result.update(manual_reviewed=len(seen),correct=correct,precision=correct/300,**{'pass':len(expected)==300 and len(seen)==300 and correct/300>=.90},reason='qualified_complete_handchecks' if len(expected)==300 else 'original_300_release_population_incomplete',manual_pin={'path':str(path),'sha256':manual['sha256']})
 return result

def build(spec,root,*,manual=None):
 if root.name!=STAGE:raise ValueError('isolated events_guidance_v2 destination required')
 identity=S.bind(spec)
 with lock(root):
  alias=root/'parse_manifest.json';parsed=P.validate(root,alias)
  if parsed['phase']!='parsed' or parsed['frozen']['inputs']!=identity:raise ValueError('parse/source lineage incompatible')
  parse_path=root/parsed['generation']/'manifest.json';parse_record=P.record(parse_path)
  con=connection(identity)
  try:
   table(con,'native_guidance',root/parsed['products']['guidance.parquet']);work=P.create(root)
   writer=pq.ParquetWriter(work/'guidance_features.parquet',FEATURE_SCHEMA,compression='zstd')
   try:
    # Page issuer identities independently from feature/history SQL cursors.
    con.execute('CREATE TEMP TABLE feature_issuers AS SELECT cik FROM native_guidance UNION SELECT cik FROM g_ea_actuals');offset=0
    while True:
     issuers=con.execute('SELECT cik FROM feature_issuers ORDER BY cik LIMIT 512 OFFSET ?',[offset]).fetchall()
     if not issuers:break
     offset+=len(issuers)
     for (cik,) in issuers:writer.write_table(pa.Table.from_pylist(issuer_features(con,cik),schema=FEATURE_SCHEMA))
   finally:writer.close()
   cov=coverage(con);precision=audit(root,parse_path,manual)
   C.write_json_atomic(work/'coverage.json',cov);C.write_json_atomic(work/'audit.json',precision)
   # Copy exact admitted immutable parse bytes, preserving original parse lineage.
   import shutil
   for name in ('guidance.parquet','releases.parquet','precision_sample.parquet'):shutil.copyfile(root/parsed['products'][name],work/name)
   S.recheck(spec,identity);P.verify(parse_path,parse_record)
   payload={'frozen':parsed['frozen'],'input_manifests_sha256':identity['bindings'],'parse_lineage':{'path':str(parse_path.resolve()),'record':parse_record},'precision':precision,'coverage':cov,'gate_status':{'precision':'PASS' if precision['pass'] else 'FAIL','coverage_reporting':'PASS' if all(v['denominator'] for v in cov.values()) else 'FAIL'}}
   manifest=P.publish(root,'features',work,payload,('guidance.parquet','guidance_features.parquet','releases.parquet','precision_sample.parquet','coverage.json','audit.json'))
   return {'status':'complete','manifest':str(manifest),'published':True,'gate_status':payload['gate_status']}
  finally:con.close()


def validate_units(con,root,m):
 run_key=digest(m['frozen']);cache=root/'_units'/run_key;offset=0;count=0
 query="SELECT DISTINCT f.cik,f.accession,f.form,f.is_amendment,f.filing_date,f.available_at,f.acceptance_clock,f.vintage_risk FROM g_filings_parquet f WHERE f.form IN ('8-K','8-K/A') AND f.filing_date>=DATE '2019-01-01' AND EXISTS (SELECT 1 FROM g_eight_k_items_parquet i WHERE i.cik=f.cik AND i.accession=f.accession AND i.item IN ('2.02','7.01')) ORDER BY cik,accession LIMIT 512 OFFSET ?"
 while True:
  batch=rows(con,query,[offset])
  if not batch:break
  offset+=len(batch)
  for filing in batch:
   key=digest([filing['cik'],filing['accession']]);identity={'frozen':run_key,'filing':digest(filing)};path=P.unit(cache,key,identity)
   if path is None:raise ValueError('original release unit missing')
   receipt=C.read_json(cache/key/'receipt.json');audit=C.read_json(cache/key/'release.json')
   if digest(audit)!=receipt.get('audit_sha256'):raise ValueError('unit release audit mismatch')
   for field in ('filing_date',):audit[field]=dt.date.fromisoformat(audit[field]) if isinstance(audit[field],str) else audit[field]
   for field in ('available_at','source_available_at'):audit[field]=K.clock(audit[field])
   actual=rows(con,'SELECT * FROM native_releases WHERE cik=? AND accession=?',[filing['cik'],filing['accession']])
   if len(actual)!=1 or digest(actual[0])!=digest(audit):raise ValueError('full original release population/audit mismatch')
   expected=pq.read_table(path).to_pylist();published=rows(con,'SELECT * FROM native_guidance WHERE cik=? AND accession=?',[filing['cik'],filing['accession']])
   if sorted(map(digest,expected))!=sorted(map(digest,published)):raise ValueError('authenticated native parse rows mismatch')
   count+=1
 if con.execute('SELECT count(*) FROM native_releases').fetchone()[0]!=count:raise ValueError('extra original release audit rows')
 if con.execute('SELECT count(*) FROM native_guidance g WHERE NOT EXISTS(SELECT 1 FROM native_releases r WHERE r.cik=g.cik AND r.accession=g.accession)').fetchone()[0]:raise ValueError('extra guidance source population')

def validate(spec,root,manifest=None):
 if root.name!=STAGE:raise ValueError('isolated events_guidance_v2 destination required')
 m=P.validate(root,manifest);identity=S.bind(spec)
 if m['frozen']['inputs']!=identity:raise ValueError('published source lineage changed')
 con=connection(identity)
 try:
  table(con,'native_guidance',root/m['products']['guidance.parquet'])
  bad=con.execute("SELECT count(*) FROM native_guidance WHERE (qualification_reason!='qualified' AND (low IS NOT NULL OR high IS NOT NULL OR point IS NOT NULL OR midpoint IS NOT NULL)) OR (qualification_reason='qualified' AND (available_at IS NULL OR period_end IS NULL OR fiscal_calendar_id IS NULL OR accounting_basis_id IS NULL OR share_basis_id IS NULL OR currency!='USD'))").fetchone()[0]
  if bad:raise ValueError('guidance qualification invariant failed')
  sample=pq.read_table(root/m['products']['precision_sample.parquet']).to_pylist();releases=root/m['products']['releases.parquet'];table(con,'native_releases',releases)
  validate_units(con,root,m)
  expected=rows(con,'SELECT * FROM native_releases WHERE NOT is_amendment AND year(filing_date) BETWEEN 2020 AND 2025 ORDER BY sample_key LIMIT 300')
  if sample!=expected:raise ValueError('fixed precision population changed')
  if m['phase'] in ('features','validated'):
   cov=coverage(con)
   if cov!=m['coverage'] or cov!=C.read_json(root/m['products']['coverage.json']):raise ValueError('coverage denominator drift')
   table(con,'published_features',root/m['products']['guidance_features.parquet'])
   # Independent regeneration with exact full event rows; no success-only validation.
   con.execute('CREATE TEMP TABLE feature_issuers AS SELECT cik FROM native_guidance UNION SELECT cik FROM g_ea_actuals');offset=0
   while True:
    issuers=con.execute('SELECT cik FROM feature_issuers ORDER BY cik LIMIT 512 OFFSET ?',[offset]).fetchall()
    if not issuers:break
    offset+=len(issuers)
    for (cik,) in issuers:
     expected=pa.Table.from_pylist(issuer_features(con,cik),schema=FEATURE_SCHEMA).to_pylist();actual=rows(con,'SELECT * FROM published_features WHERE cik=?',[cik])
     if sorted(map(digest,actual))!=sorted(map(digest,expected)):raise ValueError('feature recomputation mismatch')
   precision=C.read_json(root/m['products']['audit.json'])
   if precision!=m['precision']:raise ValueError('manual precision evidence drift')
   if precision.get('manual_pin'):
    checked=audit(root,Path(m['parse_lineage']['path']),{**precision['manual_pin'],'reviewed_sha256':precision['manual_pin']['sha256']})
    if checked!=precision:raise ValueError('manual precision recomputation mismatch')
   elif precision['pass']:raise ValueError('precision PASS without handchecks')
  S.recheck(spec,identity)
  return {'status':'valid','phase':m['phase'],'gate_status':m['gate_status'],'source_quality_certified':False}
 finally:con.close()


def contract():
 """Read-only schema/interface checkpoint; never opens data or creates a stage."""
 return {'version':'guidance_bindings_v2','config_sha256':CONFIG_SHA256,'configuration':CONFIG,'source_fields':{
 'catalog':['cik','accession','doc_sha256','source_doc','doc_source'],
 'targets':list(K.KEY)+['accession','source_sha256','source_available_at','evidence_id'],
 'actual_basis':list(K.KEY)+['accession','source_sha256','source_available_at','evidence_id','value_field','source_value','security_id'],
 'prices':['cik','accession','security_id','session_date','close','close_at','available_at','source_available_at','currency','basis_id','exchange_id','qualification_reason','evidence_id','source_sha256'],
 'calendar':['exchange_id','session_date','close_at','source_available_at','evidence_id','source_sha256'],
 'members':['year','cik']},'publication_fields':{name:str(schema) for name,schema in [('guidance',GUIDANCE_SCHEMA),('features',FEATURE_SCHEMA),('releases',RELEASE_SCHEMA)]}}
