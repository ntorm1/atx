"""Task5 bounded original-source recovery and immutable CompanyFacts extract reuse.

Network is explicit and only operates on a controller-approved target JSON. Counts every
adapter attempt (including redirects/retries) before issuance, persists the count and receipts,
and uses the shared SEC limiter at one request/second. Never fetches submissions/indexes.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
from pathlib import Path

from atx_db import sec_http
from . import common as C

MAX_ATTEMPTS = 12
MAX_DOCUMENT = 24 * 1024**2
MAX_TOTAL = 288 * 1024**2


def qualified_original_facts(html: str) -> list[dict]:
    """Retain qualified identity for independent checks; local-name collisions are never standard facts.

    This does not certify unit/currency, main-statement scope, source clock or economic equivalence.
    A blank numeric element is missing. A printed dash is not an explicit numeric zero here.
    """
    from .compustat_std import parse_ixbrl
    facts = parse_ixbrl(html)
    for fact in facts:
        uri = fact.get('namespace_uri') or ''
        fact['standard_taxonomy'] = (
            'us-gaap' if re.fullmatch(r'https?://fasb.org/us-gaap/[^/]+', uri) else
            'dei' if re.fullmatch(r'https?://xbrl.sec.gov/dei/[^/]+', uri) else
            'ifrs-full' if re.fullmatch(r'https?://xbrl.ifrs.org/taxonomy/[^/]+/ifrs-full', uri) else None)
        fact['source_numeric_qualified'] = fact.get('numeric_text') not in ('', '-', '—', '–')
    return facts


class AttemptBudget:
    def __init__(self, root: Path, limiter=None):
        self.root=root
        root.mkdir(parents=True,exist_ok=True)
        self.path=root/'attempt-counter.json'
        if not self.path.exists() and any((root/name).exists() for name in ('attempts.jsonl','receipts.jsonl')):
            raise RuntimeError('Existing Task5 ledgers require durable attempt-counter reconciliation')
        self.state=C.read_json(self.path) if self.path.exists() else {'attempts':0,'raw_bytes':0}
        if not (0 <= self.state['attempts'] <= MAX_ATTEMPTS and 0 <= self.state['raw_bytes'] <= MAX_TOTAL):
            raise RuntimeError('Invalid durable Task5 attempt/byte counter')
        ledger=root/'attempts.jsonl'
        if ledger.exists():
            recorded=[json.loads(line)['attempt'] for line in ledger.read_text(encoding='utf8').splitlines() if line.strip()]
            if recorded and max(recorded)>self.state['attempts']:
                raise RuntimeError('Task5 counter is behind durable attempt ledger')
        self.limiter=limiter or sec_http.SecRateLimiter(rate_per_s=1.0)

    def acquire(self,label=None):
        if self.state['attempts']>=MAX_ATTEMPTS:
            raise RuntimeError('Task5 all-attempt cap exhausted')
        self.state['attempts']+=1
        C.write_json_atomic(self.path,self.state)
        granted=self.limiter.acquire(label)
        with (self.root/'attempts.jsonl').open('a',encoding='utf8') as f:
            f.write(json.dumps({'attempt':self.state['attempts'],'url':label,'granted_at':granted})+'\n')
        return granted

    def record_response(self,*args,**kwargs):
        return self.limiter.record_response(*args,**kwargs)


def acquire(targets: list[dict], root: Path) -> dict:
    if len(targets)>12 or len({r['accession'] for r in targets})!=len(targets):
        raise ValueError('At most12 unique approved primary documents')
    budget=AttemptBudget(root); cache_hits=0
    with sec_http.sec_session(limiter=budget,max_attempts=1) as session:
        for r in targets:
            url=r['url']
            if not sec_http.is_sec_url(url) or '/Archives/edgar/data/' not in url or not url.endswith(('.htm','.html')):
                raise ValueError('Target must be a specific SEC primary HTML document')
            path=root/(r['accession']+'.htm')
            if path.exists():
                cache_hits+=1
                continue
            if budget.state['raw_bytes']+MAX_DOCUMENT>MAX_TOTAL:
                raise RuntimeError('Task5 total raw byte cap reached')
            receipt={**r,'fetched_at':dt.datetime.now(dt.UTC).isoformat(),'path':str(path)}
            try:
                with session.get(url,timeout=30,stream=True) as response:
                    receipt['http_status']=response.status_code
                    response.raise_for_status()
                    data=sec_http.read_bounded_response(response,MAX_DOCUMENT)
                    receipt.update(bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
                    temp=path.with_suffix('.partial');temp.write_bytes(data);temp.replace(path)
                    budget.state['raw_bytes']+=len(data)
                    C.write_json_atomic(budget.path,budget.state)
            except Exception as exc:
                receipt['error']=type(exc).__name__+':'+str(exc)
            with (root/'receipts.jsonl').open('a',encoding='utf8') as ledger:
                ledger.write(json.dumps(receipt)+'\n')
    return {**budget.state,'cache_hits':cache_hits}


def reuse_extract(source: Path, target: Path) -> dict:
    """Hardlink immutable extract/preparation inputs only; never link outputs or mutable part files."""
    src=source/'_work'/'cf';dst=target/'_work'/'cf';dst.mkdir(parents=True,exist_ok=True)
    approved=C.read_json(source/'manifest.json')['sources']
    expected=approved['companyfacts_batches']
    receipts=sorted(src.glob('batch-[0-9][0-9][0-9][0-9].json'))
    if not receipts:raise FileNotFoundError('No approved extraction receipts')
    if len(receipts)!=len(expected):raise ValueError('Approved extraction inventory count differs')
    inventory=[]
    for receipt in receipts:
        record=C.read_json(receipt);p=src/record['file']
        if p.parent!=src or not re.fullmatch(r'batch-\d{4}\.parquet',p.name):
            raise ValueError('Unsupported extraction inventory member')
        pinned=expected.get(p.name,{})
        if (record['parquet_sha256']!=pinned.get('extract_sha256')
                or p.stat().st_size!=record['parquet_bytes'] or C.sha256_file(p)!=record['parquet_sha256']):
            raise ValueError('Source extraction receipt hash/size mismatch')
        inventory.extend((p,receipt))
    plan=src/'plan.json'
    if plan.exists():
        if C.sha256_file(plan)!=approved['companyfacts_extract_plan']['sha256']:
            raise ValueError('Approved extraction plan hash mismatch')
        inventory.append(plan)
    count=logical=0
    for p in inventory:
        dest=dst/p.name
        if dest.exists():
            if dest.stat().st_size!=p.stat().st_size or C.sha256_file(dest)!=C.sha256_file(p):
                raise ValueError('Cached immutable extract differs')
        elif p.suffix=='.parquet':os.link(p,dest)
        else:shutil.copyfile(p,dest)  # mutable metadata never shares a hardlink with the old stage
        count+=1;logical+=p.stat().st_size
    if not count:raise FileNotFoundError(src)
    return {'source':str(source),'source_manifest_sha256':C.sha256_file(source/'manifest.json'),
            'files':count,'logical_bytes':logical,'new_data_bytes':sum(p.stat().st_size for p in inventory if p.suffix!='.parquet')}


def main(argv=None):
    ap=argparse.ArgumentParser(description=__doc__);sub=ap.add_subparsers(dest='cmd',required=True)
    p=sub.add_parser('reuse');p.add_argument('--source',default='fundamentals_v10');p.add_argument('--stage',default='fundamentals_v11')
    p=sub.add_parser('acquire');p.add_argument('--targets',type=Path,required=True)
    args=ap.parse_args(argv)
    if args.cmd=='reuse':print(reuse_extract(C.stage_dir(args.source),C.stage_dir(args.stage)))
    else:print(acquire(json.loads(args.targets.read_text(encoding='utf-8-sig')),
                      C.PACKAGE_ROOT/'data/raw/sec_fundamentals_depth'))
    return 0

if __name__=='__main__':raise SystemExit(main())
