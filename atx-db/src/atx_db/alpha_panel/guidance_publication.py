"""Authenticated immutable units and isolated atomic guidance activation."""
from __future__ import annotations
import json,os,uuid
from pathlib import Path
import pyarrow as pa,pyarrow.parquet as pq
from . import common as C
from .guidance_contract import CONFIG_SHA256,digest

def code():return C.code_identity('guidance','guidance_contract','guidance_kernels','guidance_sources','guidance_publication','guidance_native','ea_publication','ea_actuals','events_sources','common')
def record(path):
 result={'sha256':C.sha256_file(path),'bytes':path.stat().st_size}
 if path.suffix=='.parquet':
  parquet=pq.ParquetFile(path);result.update(rows=parquet.metadata.num_rows,schema_sha256=digest(str(parquet.schema_arrow)))
 return result
def write(path,rows,schema):
 path.parent.mkdir(parents=True,exist_ok=True);temporary=path.with_suffix('.partial')
 pq.write_table(pa.Table.from_pylist(rows,schema=schema),temporary,compression='zstd');os.replace(temporary,path)
def verify(path,expected):
 if not path.is_file() or path.stat().st_size!=expected.get('bytes') or C.sha256_file(path)!=expected.get('sha256'):raise ValueError('publication byte identity mismatch')
 if record(path)!=expected:raise ValueError('publication schema/row identity mismatch')
def unit(cache,key,identity,rows=None,schema=None,audit=None):
 folder=cache/key;receipt=folder/'receipt.json';data=folder/'rows.parquet'
 if receipt.exists():
  saved=C.read_json(receipt)
  if saved.get('identity')!=identity:raise ValueError('stale unit identity')
  verify(data,saved['output'])
  if pq.ParquetFile(data).metadata.num_rows!=saved['rows']:raise ValueError('unit row identity mismatch')
  return data
 if rows is None:return None
 folder.mkdir(parents=True,exist_ok=True);write(data,rows,schema)
 if audit is not None:C.write_json_atomic(folder/'release.json',audit)
 C.write_json_atomic(receipt,{'identity':identity,'output':record(data),'rows':len(rows),'status':'complete','audit_sha256':digest(C.read_json(folder/'release.json')) if audit is not None else None})
 return data

def publish(root,phase,work,payload,products):
 generation='generations/'+work.name;files={name:record(work/name) for name in products}
 manifest={**payload,'schema':'atx.events.guidance/v2','stage':'events_guidance_v2','status':'complete','phase':phase,'generation':generation,
  'config_sha256':CONFIG_SHA256,'code':code(),'files':files,'products':{name:generation+'/'+name for name in products},'source_quality_certified':False,'readiness':'NOT_PRODUCTION_READY'}
 C.write_json_atomic(work/'manifest.json',manifest);target=root/generation;target.parent.mkdir(parents=True,exist_ok=True);os.rename(work,target)
 C.write_json_atomic(root/('parse_manifest.json' if phase=='parsed' else 'manifest.json'),manifest)
 if phase=='parsed':C.write_json_atomic(root/'manifest.json',manifest)
 return target/'manifest.json'
def create(root):
 work=root/'_pending'/uuid.uuid4().hex;work.mkdir(parents=True);return work

def validate(root,manifest_path=None):
 root=root.resolve();path=Path(manifest_path or root/'manifest.json').resolve();before=record(path);m=C.read_json(path)
 if m.get('schema')!='atx.events.guidance/v2' or m.get('stage')!='events_guidance_v2' or m.get('status')!='complete' or m.get('config_sha256')!=CONFIG_SHA256 or m.get('code')!=code():raise ValueError('incompatible guidance publication')
 generation=(root/m.get('generation','')).resolve()
 if generation.parent!=root/'generations':raise ValueError('invalid immutable generation')
 if record(generation/'manifest.json')!=before:raise ValueError('activation/immutable manifest mismatch')
 expected={'guidance.parquet','releases.parquet','precision_sample.parquet'} if m.get('phase')=='parsed' else {'guidance.parquet','guidance_features.parquet','releases.parquet','precision_sample.parquet','coverage.json','audit.json'} if m.get('phase') in ('features','validated') else set()
 if not expected or set(m.get('files',{}))!=expected or set(m.get('products',{}))!=expected:raise ValueError('incomplete phase product inventory')
 for name in expected:
  if m['products'][name]!=m['generation']+'/'+name:raise ValueError('product path mismatch')
  verify(generation/name,m['files'][name])
 if m.get('parse_lineage'):
  parent=Path(m['parse_lineage']['path']);verify(parent,m['parse_lineage']['record']);validate(root,parent)
 if record(path)!=before:raise ValueError('publication changed during validation')
 return m
