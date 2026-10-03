"""Optional assertion replay checkpoints; data lives outside published source stages."""
from __future__ import annotations
import hashlib,json,sys
from pathlib import Path
from typing import Callable
from . import common as C,issuer_history as I
from atx_db import sec_http as H

SCHEMA='atx.issuer-class-replay/v1'

def freeze(value):
    return json.loads(json.dumps(value,sort_keys=True,default=str))

def digest(value)->str:
    return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':'),default=str).encode('utf-8')).hexdigest()

def file_digest(path:Path)->str:
    result=hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda:source.read(1024*1024),b''):result.update(chunk)
    return result.hexdigest()

def code_identity(parser_path:Path,rule:str,maximum:int)->dict:
    modules={'parser':parser_path,'replay':Path(__file__),'clock':Path(I.__file__),
             'common':Path(C.__file__),'source_store':Path(H.__file__)}
    return {'rule':rule,'primary_maximum':maximum,'python':sys.version,
            'code_sha256_lf':{key:hashlib.sha256(path.read_bytes().replace(b'\r\n',b'\n')).hexdigest() for key,path in modules.items()}}

def valid_assertion(value)->bool:
    return (isinstance(value,dict) and set(value)=={'status','reason','evidence'}
            and (value['status'] is None or type(value['status']) is bool)
            and isinstance(value['reason'],str)
            and (value['evidence'] is None or (isinstance(value['evidence'],list) and all(isinstance(item,str) for item in value['evidence'])))
            and (value['status'] is None or bool(value['evidence'])))

class AssertionReplay:
    """Reuse only exact source/code/plan matches; interrupted rows leave no valid entry."""
    def __init__(self,directory:Path,identity:dict):
        self.directory=directory;self.identity=freeze(identity);self.hits=0;self.computed=0;self.invalid=0;self.completed=[]
        self.assessed=[]
        directory.mkdir(parents=True,exist_ok=True)

    def capture(self,row:dict,store)->dict:
        identity=self.row_identity(row,store,'')
        self.assessed.append((freeze(row),store,identity))
        return identity

    def verify(self,row:dict,store,identity:dict)->None:
        if self.row_identity(row,store,'')!=identity:raise RuntimeError('source changed during assessment consumption')

    def row_identity(self,row:dict,store,name:str)->dict:
        sources={}
        for field in ('header_url','primary_url'):
            url=row[field];record=store.lookup(url)
            source={key:getattr(record,key,None) for key in ('url','status','error','sha256','bytes','fetched_at')}
            source['requested_url']=url;source['retained_object']=None
            if record and record.ok and record.sha256:
                for path in (store.object_path(record.sha256),store.raw_object_path(record.sha256)):
                    if path.exists():
                        source['retained_object']={'kind':path.suffix or 'raw','bytes':path.stat().st_size,'sha256':file_digest(path)}
                        break
            sources[field]=source
        return freeze({'schema':SCHEMA,'code':self.identity,'plan_row':row,'issuer_name':name,'sources':sources})

    def assertion(self,row:dict,store,name:str,process:Callable[[],dict])->dict:
        identity=self.row_identity(row,store,name);key=digest(identity)
        path=self.directory/(digest({'cik':row['cik'],'accession':row['accession']})+'.json')
        if path.exists():
            try:
                cached=C.read_json(path)
                result=cached['assertion']
                if cached['identity']==identity and cached['identity_sha256']==key and cached['assertion_sha256']==digest(result) and valid_assertion(result):
                    self.hits+=1;self.completed.append((row,store,name,key));return result
            except (ValueError,OSError,KeyError,TypeError):pass
            self.invalid+=1
        result=process()
        if not valid_assertion(result):raise ValueError('invalid replay assertion result')
        if self.row_identity(row,store,name)!=identity:raise RuntimeError('source changed during assertion replay')
        C.write_json_atomic(path,{'identity':identity,'identity_sha256':key,'assertion':result,'assertion_sha256':digest(result)})
        self.computed+=1;self.completed.append((row,store,name,key));return result

    def finish(self,current_code:dict)->dict:
        if current_code!=self.identity:raise RuntimeError('parser/helper code changed during replay')
        for row,store,identity in self.assessed:self.verify(row,store,identity)
        for row,store,name,key in self.completed:
            if digest(self.row_identity(row,store,name))!=key:raise RuntimeError('source changed before replay publication')
        return {'schema':SCHEMA,'directory':str(self.directory),'code_identity_sha256':digest(self.identity),
                'reused':self.hits,'computed':self.computed,'invalidated':self.invalid,'completed':len(self.completed),'assessed_sources':len(self.assessed),
                'completed_identities_sha256':digest([key for *_,key in self.completed])}
