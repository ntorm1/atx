"""Task10 whole-task repair1: source provenance, blockers and complete replay."""
import datetime as dt,hashlib,json
from pathlib import Path
import pyarrow.parquet as pq,pytest
import importlib.util
_fixture_spec=importlib.util.spec_from_file_location("t10_native_fixtures",Path(__file__).with_name("test_alpha_panel_guidance_native.py"))
_fixture_module=importlib.util.module_from_spec(_fixture_spec);_fixture_spec.loader.exec_module(_fixture_module)
fixture,write,stamp,read_product=(_fixture_module.fixture,_fixture_module.write,_fixture_module.stamp,_fixture_module.read_product)
from atx_db.alpha_panel import guidance as G,guidance_native as N,guidance_publication as P,guidance_kernels as K,common as C

def repin(f):
 sources=f['spec']['sources'];contract=C.read_json(f['paths']['evidence_contract'])
 for name,path in f['paths'].items():
  if name!='evidence_contract':sources[name]['sha256']=C.sha256_file(path)
 for name in contract['files']:contract['files'][name]=sources[name]['sha256']
 C.write_json_atomic(f['paths']['evidence_contract'],contract);sha=C.sha256_file(f['paths']['evidence_contract']);sources['evidence_contract'].update(sha256=sha,reviewed_sha256=sha)
 sec=f['lake']/'sec_filings';manifest=C.read_json(sec/'manifest.json')
 for name in manifest['files']:manifest['files'][name]=write(sec/name,pq.read_table(sec/name).to_pylist())
 C.write_json_atomic(sec/'manifest.json',manifest);sources['sec_filings']['sha256']=C.sha256_file(sec/'manifest.json')
 ea=f['lake']/'ea_actuals';m=C.read_json(ea/'releases_manifest.json');m['input_manifests_sha256']['sec_filings']['sha256']=sources['sec_filings']['sha256'];m['receipt']['catalog_sha256']=sources['catalog']['sha256'];C.write_json_atomic(ea/m['generation']/'manifest.json',m);C.write_json_atomic(ea/'releases_manifest.json',m);sources['ea_actuals']['sha256']=C.sha256_file(ea/'releases_manifest.json')

def replace_body(f,accession,text):
 blob=text.encode();sha=hashlib.sha256(blob).hexdigest();f['bodies'][sha]=blob
 catalog=pq.read_table(f['paths']['catalog']).to_pylist()
 for row in catalog:
  if row['accession']==accession:row['doc_sha256']=sha
 write(f['paths']['catalog'],catalog)
 targets=pq.read_table(f['paths']['targets']).to_pylist()
 for row in targets:
  if row['accession']==accession:row['source_sha256']=sha
 write(f['paths']['targets'],targets);repin(f)

@pytest.mark.parametrize('text,origin',[('<h2>Business Outlook</h2><table><tr><td>Metric</td><td>Q2 FY2021</td></tr><tr><td>GAAP diluted EPS</td><td>$1.20 to $1.40</td></tr></table>','table'),('<h2>Second Quarter 2021 Outlook</h2><p>The company expects GAAP diluted EPS of $1.20 to $1.40.</p>','heading')])
def test_r1_explicit_table_and_heading_native_publication_replay(fixture,text,origin):
 f=fixture;replace_body(f,'a2',text);parsed=N.parse(f['spec'],f['root']);raw=read_product(f['root'],parsed['manifest'],'guidance.parquet');latest=next(r for r in raw if r['accession']=='a2')
 assert latest['qualification_reason']=='qualified' and latest['period_origin']==origin and latest['period_resolution']=='stated' and latest['low']==1.2 and latest['point'] is None
 built=N.build(f['spec'],f['root']);events=read_product(f['root'],built['manifest'],'guidance_features.parquet');assert next(r for r in events if r['accession']=='a2')['guid_revision']==pytest.approx(.2)
 assert next(r for r in events if r['event_kind']=='actual' and r['measure']=='EPS')['guid_surprise']==pytest.approx(.02)
 assert N.validate(f['spec'],f['root'])['status']=='valid'
 assert C.read_json(Path(built['manifest']))['coverage']['2021']['denominator']==2

@pytest.mark.parametrize('text',['Business Outlook\nMetric | Q2 |\nGAAP diluted EPS | $1.20 to $1.40 |','Second Quarter Outlook\nThe company expects GAAP diluted EPS of $1.20 to $1.40.'])
def test_r1_inferred_table_and_heading_stay_refused(fixture,text):
 candidates=G.extract_guidance(text,G.ReportedPeriod(dt.date(2021,3,31)),2021);assert candidates and candidates[0].extras['period_resolution']=='inferred'
 f=fixture;proof=pq.read_table(f['paths']['targets']).to_pylist()[0];filing={'cik':1,'accession':'a1','form':'8-K','is_amendment':False,'filing_date':dt.date(2021,4,1),'available_at':stamp((4,1)),'acceptance_clock':'sec_acceptance','vintage_risk':'original'}
 row=N.qualify(candidates[0],filing,{'doc_sha256':proof['source_sha256'],'source_doc':'source.htm'},[proof]);assert row['qualification_reason']=='target_inferred_or_unknown' and row['low'] is None and not row['comparator_identities']

@pytest.mark.parametrize('conflict',[False,True])
def test_r1_latest_duplicate_or_conflicting_identity_blocks_native_eps_surprise(fixture,conflict):
 f=fixture;proofs=pq.read_table(f['paths']['targets']).to_pylist();latest=next(r for r in proofs if r['accession']=='a2' and r['measure']=='EPS');proofs.append({**latest,'accounting_basis_id':'other-accounting' if conflict else latest['accounting_basis_id']});write(f['paths']['targets'],proofs);repin(f)
 parsed=N.parse(f['spec'],f['root']);raw=read_product(f['root'],parsed['manifest'],'guidance.parquet');bad=next(r for r in raw if r['accession']=='a2' and r['measure']=='EPS');assert bad['midpoint'] is None and len(bad['comparator_identities'])==2
 assert bad['period_end']==dt.date(2021,6,30) and bad['accounting_basis_id']==(None if conflict else 'gaap-total')
 built=N.build(f['spec'],f['root']);eps=next(r for r in read_product(f['root'],built['manifest'],'guidance_features.parquet') if r['event_kind']=='actual' and r['measure']=='EPS')
 assert eps['guid_surprise'] is None and eps['feature_reason']=='latest_prior_ambiguous_or_unqualified' and eps['prior_accession'] is None
 # Union blocks each explicitly affected scope, but not a distinct valid calendar/target.
 probe={**next(r for r in raw if r['accession']=='a1' and r['measure']=='EPS'),'available_at':stamp((6,1))}
 assert K.revision(probe,raw)['guid_revision'] is None
 unrelated={**probe,'fiscal_quarter':3,'period_end':dt.date(2021,9,30)};assert not K.affects(unrelated,bad)
 assert not K.affects({**probe,'share_basis':'basic'},bad)
 assert N.validate(f['spec'],f['root'])['status']=='valid'


def test_r1_native_later_revision_cannot_skip_ambiguous_issue(fixture):
 f=fixture;targets=pq.read_table(f['paths']['targets']).to_pylist();bad=next(r for r in targets if r['accession']=='a2' and r['measure']=='EPS');targets.append(dict(bad))
 blob=b'For the second quarter of 2021, the company expects GAAP diluted EPS of $1.60 to $1.80.';sha=hashlib.sha256(blob).hexdigest();f['bodies'][sha]=blob
 catalog=pq.read_table(f['paths']['catalog']).to_pylist();catalog.append({**catalog[0],'accession':'a4','doc_sha256':sha});write(f['paths']['catalog'],catalog)
 sec=f['lake']/'sec_filings';filings=pq.read_table(sec/'filings.parquet').to_pylist();filings.append({**filings[0],'accession':'a4','filing_date':dt.date(2021,6,1),'available_at':stamp((6,1))});write(sec/'filings.parquet',filings)
 items=pq.read_table(sec/'eight_k_items.parquet').to_pylist();items.append({'cik':1,'accession':'a4','item':'7.01'});write(sec/'eight_k_items.parquet',items)
 targets.append({**bad,'accession':'a4','source_sha256':sha,'source_available_at':stamp((6,1))});write(f['paths']['targets'],targets);repin(f)
 N.parse(f['spec'],f['root']);built=N.build(f['spec'],f['root']);events=read_product(f['root'],built['manifest'],'guidance_features.parquet');revision=next(r for r in events if r['accession']=='a4')
 assert revision['guid_revision'] is None and revision['feature_reason']=='latest_prior_ambiguous_or_unqualified'
 # A newly independently qualified issue repairs only subsequent comparisons.
 assert next(r for r in events if r['event_kind']=='actual' and r['measure']=='EPS')['guid_surprise']==pytest.approx(-.02)
 assert N.validate(f['spec'],f['root'])['status']=='valid'


def test_r1_semantic_replay_rejects_absent_source_issuer_despite_authentic_bytes(fixture):
 f=fixture;N.parse(f['spec'],f['root']);built=N.build(f['spec'],f['root']);manifest=Path(built['manifest']);m=C.read_json(manifest);path=f['root']/m['products']['guidance_features.parquet'];items=pq.read_table(path).to_pylist();items.append({**items[0],'cik':999});write(path,items,N.FEATURE_SCHEMA)
 m['files']['guidance_features.parquet']=P.record(path);C.write_json_atomic(manifest,m);C.write_json_atomic(f['root']/'manifest.json',m)
 assert P.validate(f['root'])['phase']=='features'  # Full declared byte/schema/row identity is consistent.
 with pytest.raises(ValueError,match='orphan feature issuer'):N.validate(f['spec'],f['root'])
