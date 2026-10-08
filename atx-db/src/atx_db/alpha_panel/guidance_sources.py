"""Explicit pinned sources; no acquisition, legacy phase/path or unreviewed fallback."""
from __future__ import annotations
import json
from pathlib import Path
from . import common as C,ea_actuals as E,ea_publication as P
from .guidance_contract import digest

def pinned(path,expected):
 path=Path(path).resolve()
 if not expected or not path.is_file() or C.sha256_file(path)!=expected:raise ValueError("source pin mismatch: "+str(path))
 return path

def bind(spec):
 if spec.get("version")!="guidance_bindings_v2":raise ValueError("explicit guidance_bindings_v2 required")
 sources=spec.get("sources",{});required={"sec_filings","ea_actuals","catalog","targets","actual_basis","prices","calendar","members","evidence_contract"}
 if set(sources)!=required:raise ValueError("complete exact source inventory required")
 bindings={};files={}
 for name in ("sec_filings","ea_actuals"):
  source=sources[name];stage=source.get("stage")
  if stage!=name:raise ValueError("semantic producer mismatch")
  manifest_name="releases_manifest.json" if name=="ea_actuals" else "manifest.json"
  record=E.verify_input(stage,manifest_name,("parsed",) if name=="ea_actuals" else None)
  if record['sha256']!=source.get('sha256'):raise ValueError("producer pin mismatch")
  manifest=C.read_json(Path(record['path']));bindings[name]=record
  if name=="ea_actuals":
   if manifest.get('schema')!=E.SCHEMA or manifest.get('stage')!=E.STAGE or manifest.get('parser_version')!=E.VERSION or manifest.get('code')!=C.code_identity('ea_actuals','ea_publication','events_sources','common'):raise ValueError('native actuals semantic/code identity mismatch')
   if not manifest.get('generation') or set(manifest.get('products',{}))!={"releases.parquet","precision_sample.parquet"}:raise ValueError("native immutable actuals parse generation required")
   directory=(C.build_root()/stage).resolve();generation=(directory/manifest['generation']).resolve()
   if generation.parent!=directory/'generations' or set(manifest['files'])!=set(manifest['products'].values()) or any(value!=manifest['generation']+'/'+name for name,value in manifest['products'].items()):raise ValueError('actuals exact generation/product paths required')
   immutable=generation/'manifest.json'
   if C.sha256_file(immutable)!=record['sha256']:raise ValueError("actuals immutable manifest mismatch")
   files[name]=P.file(stage,'releases.parquet',manifest_name)
  else:
   for product in ('filings.parquet','eight_k_items.parquet'):
    if product not in manifest['files']:raise ValueError("required SEC product absent")
    files[product]=C.build_root()/stage/product
 for name in required-{"sec_filings","ea_actuals"}:
  source=sources[name];path=pinned(source.get('path',''),source.get('sha256'))
  bindings[name]={"path":str(path),"sha256":source['sha256']};files[name]=path
 contract=json.loads(files['evidence_contract'].read_text())
 if sources['evidence_contract'].get('reviewed_sha256')!=bindings['evidence_contract']['sha256']:raise ValueError("separately expected reviewed evidence contract SHA required")
 if contract.get('schema')!='atx.guidance-independent-evidence/v1' or contract.get('status')!='reviewed' or contract.get('purpose')!='guidance_comparability':raise ValueError("unreviewed comparability evidence")
 for name in ('targets','actual_basis','prices','calendar','members'):
  if contract.get('files',{}).get(name)!=bindings[name]['sha256']:raise ValueError("comparability evidence data pin mismatch")
 if contract.get('rules')!={'target_calendar':'exact historical source identity','currency':'USD original source declaration','eps_share_basis':'basic/diluted and original split identity','actual_basis':'exact GAAP/ADJUSTED/source metric identity','price_basis':'same currency/share split basis, positive original observed prior session','members':'independent full member issuer-year population2020-2025'}:raise ValueError("unsupported evidence conventions")
 task9=C.read_json(Path(bindings['ea_actuals']['path']))
 if task9.get('input_manifests_sha256',{}).get('sec_filings',{}).get('sha256')!=bindings['sec_filings']['sha256']:raise ValueError("actuals SEC lineage incompatible")
 if task9.get('receipt',{}).get('catalog_sha256')!=bindings['catalog']['sha256']:raise ValueError("actuals catalog lineage incompatible")
 counts=contract.get('member_counts',{})
 if set(counts)!={str(y) for y in range(2020,2026)} or any(not isinstance(v,int) or isinstance(v,bool) or v<1 for v in counts.values()):raise ValueError('independent complete member-year counts required')
 return {"bindings":bindings,"files":{k:str(v) for k,v in files.items()},"contract_sha256":digest(contract),"member_counts":counts}

def recheck(spec,identity):
 if bind(spec)!=identity:raise ValueError("input changed during guidance operation")
