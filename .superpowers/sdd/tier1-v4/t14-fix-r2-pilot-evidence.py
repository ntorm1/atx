"""Compact case-key and receipt provenance, no extrapolation from the pilot."""
import json,hashlib,gzip,re
from pathlib import Path
artifact=Path('../.superpowers/sdd/tier1-v4')
raw=Path('data/raw/sec_notes_t14_pilot')
cases=[json.loads(s) for s in (artifact/'t14-fix-r2-causal-cases.jsonl').read_text().splitlines()]
receipts=[json.loads(s) for s in (raw/'receipts.jsonl').read_text().splitlines()]
selection=json.loads((artifact/'t14-fix-r2-pilot-selection.json').read_text())
(artifact/'t14-fix-r2-pilot-selection-keys.json').write_text(json.dumps([
 {k:r[k] for k in ('adsh','cik','filed','status','cause','sic','issuer','request')}
 for r in selection],indent=2),encoding='utf-8')
annotations={
 '0001140361-23-014888':('safe_container_identity','Forian Note 16 explicitly identifies Information and Software, Services and Other as three operating/reportable segments. Raw DIM uses ReportableLegalEntities; admit that exact declared container, preserving all members.'),
 '0001722482-24-000012':('safe_container_identity','Avantor segment policy explicitly states its three geographic operating segments Americas, Europe and AMEA are the reportable segments. Raw DIM uses ReportableGeographicalComponents; admit exact container without regrouping.'),
 '0001982518-25-000007':('safe_container_identity','Smith Douglas Note 18 separately presents Southeast and Central reportable segment information and corporate reconciliation. Raw DIM uses ReportableGeographicalComponents; admit exact container without removing any member.'),
 '0001466538-20-000026':('not_equivalent_total','Cowen segment Economic Income revenues are explicitly reconciled to US GAAP with Funds Consolidation and Other Adjustments. Arithmetic equality of an alternate concept does not establish equivalent company revenue.'),
 '0000950170-23-004361':('source_supported_hierarchy_requires_general_parser','Martin Marietta segment table explicitly nests East and West under Total Building Materials and separately presents Magnesia Specialties. Production landing lacks filing-specific presentation/definition roles. General role-aware hierarchy ingestion is not implemented; no numeric roll-up deletion was reinstated.'),
}
keys=[]
for case in cases:
 annotation=annotations.get(case['adsh'])
 keys.append({k:case[k] for k in ('adsh','cik','filed','status','cause') }|{'pilot_conclusion':annotation[0] if annotation else 'unknown_local_vs_new_source'})
(artifact/'t14-fix-r2-case-keys.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in keys),encoding='utf-8')
verified=[]
for key,(decision,reason) in annotations.items():
 rs=[r for r in receipts if r['adsh']==key]
 checks=[]
 for r in rs:
  if r.get('path'):
   with gzip.open(raw/r['path'],'rb') as stream: payload=stream.read()
   checks.append(hashlib.sha256(payload).hexdigest()==r['sha256'])
 verified.append({'adsh':key,'decision':decision,'reason':reason,'sources':rs,'raw_sha_matches':all(checks)})
summary={'annotations':verified,'baseline_residual_cases':3183,'safe_container_cases':3,
 'source_supported_hierarchy_parser_unimplemented':1,'known_inequivalent_alternate_total':1,
 'other_case_causality_not_certified':3178,'no_extrapolation_from_pilot':True,
 'case_key_index':'t14-fix-r2-case-keys.jsonl','raw_receipts_sha256':hashlib.sha256((raw/'receipts.jsonl').read_bytes()).hexdigest()}
(artifact/'t14-fix-r2-pilot-evidence.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
assert all(r['raw_sha_matches'] for r in verified)
print(json.dumps({'baseline':3183,'annotated':len(verified),'all_annotated_raw_sha_match':True}))
