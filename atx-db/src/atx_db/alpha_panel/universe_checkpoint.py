"""Authenticated year scratch; never treats legacy receipts as certification."""
from pathlib import Path
import json
from . import common as C

def canonical(value):return json.loads(json.dumps(value,sort_keys=True,default=str))

def identity(inputs,modules,years):
    inventory={}
    for name,sha in inputs.items():
        path=C.build_root()/name
        manifest=path if path.suffix=='.json' else path/'manifest.json'
        if C.sha256_file(manifest)!=sha:raise RuntimeError('input manifest changed')
        data=C.read_json(manifest);files={}
        for filename,declared in data.get('files',{}).items():
            target=manifest.parent/filename
            actual={'sha256':C.sha256_file(target),'bytes':target.stat().st_size}
            if actual!=declared:raise RuntimeError('input payload differs from manifest: '+str(target))
            files[filename]=actual
        inventory[name]={'manifest':sha,'files':files}
    calendar=C.build_root()/'calendar.parquet'
    inventory['calendar.parquet']={'sha256':C.sha256_file(calendar),'bytes':calendar.stat().st_size}
    inventory['required_panel_years']={str(y):{p.name:{'sha256':C.sha256_file(p),'bytes':p.stat().st_size} for p in sorted((C.build_root()/'panel'/f'year={y}').glob('*.parquet'))} for y in years}
    return canonical({'schema':'atx.universe-year-cache/v1','years':list(years),'inputs':inventory,'code':{k:v for k,v in C.code_identity(*modules).items() if isinstance(v,dict)}})

class YearCache:
    def __init__(self,directory,ident,columns):
        self.directory=directory;self.identity=canonical(ident);self.columns=set(columns)
        directory.mkdir(parents=True,exist_ok=True)
        stamp=directory/'identity.json'
        if stamp.exists():
            if C.read_json(stamp)!=self.identity:raise RuntimeError('stale universe cache; preserved for inspection')
        elif any(directory.iterdir()):raise RuntimeError('unauthenticated universe cache; preserved for inspection')
        else:C.write_json_atomic(stamp,self.identity)

    def payload(self,year):return self.directory/f'year={year}'/'universe.parquet'
    def receipt(self,year):return self.directory/f'daily_{year}.json'

    def validate(self,year,expected_dates):
        payload=self.payload(year);receipt=self.receipt(year)
        if not receipt.exists():
            if payload.parent.exists() and any(payload.parent.iterdir()):raise RuntimeError('incomplete year payload; preserved for inspection')
            return None
        try:
            done=C.read_json(receipt)
            if not (done['identity']==self.identity and done['year']==year):raise ValueError("invalid checkpoint")
            if not (type(done['rows']) is int and done['rows']>=0):raise ValueError("invalid checkpoint")
            if not (set(done['daily'])==set(expected_dates)):raise ValueError("invalid checkpoint")
            if not (all(set(row)==self.columns and all(type(v) is int and v>=0 for v in row.values()) for row in done['daily'].values())):raise ValueError("invalid checkpoint")
            if not (done['payload']=={'sha256':C.sha256_file(payload),'bytes':payload.stat().st_size}):raise ValueError("invalid checkpoint")
            import pyarrow.parquet as pq
            if not (pq.read_metadata(payload).num_rows==done['rows']):raise ValueError("invalid checkpoint")
            if not (all(sum(row[column] for row in done['daily'].values())<=done['rows'] for column in self.columns)):raise ValueError("invalid checkpoint")
            if not (done['daily_sha256']==__import__('hashlib').sha256(json.dumps(done['daily'],sort_keys=True,separators=(',',':')).encode()).hexdigest()):raise ValueError("invalid checkpoint")
        except (AssertionError,KeyError,ValueError,OSError,TypeError) as exc:raise RuntimeError('invalid universe year checkpoint; preserved for inspection') from exc
        return done

    def seal(self,year,rows,daily,expected_dates):
        payload=self.payload(year)
        C.write_json_atomic(self.receipt(year),{'identity':self.identity,'year':year,'rows':rows,'daily':daily,
            'daily_sha256':__import__('hashlib').sha256(json.dumps(daily,sort_keys=True,separators=(',',':')).encode()).hexdigest(),
            'payload':{'sha256':C.sha256_file(payload),'bytes':payload.stat().st_size}})
        return self.validate(year,expected_dates)
