import datetime as dt
import hashlib
import json
from dataclasses import replace
import pytest
from atx_db.alpha_panel import mna_native_v2 as N
from atx_db.alpha_panel.terminal_mna_v2 import UTC


def auxiliary():
    clock = dt.datetime(2020,1,3,17,30,tzinfo=UTC)
    bulk = json.dumps({'cik':1,'filings':{'recent':{'accessionNumber':['0000000001-20-000001'],
                                                  'acceptanceDateTime':['2020-01-03T17:30:00Z']}}}).encode()
    native = b'''<ACCEPTANCE-DATETIME>20200103123000
ACCESSION NUMBER: 0000000001-20-000001
FILER:
 CENTRAL INDEX KEY: 0000000001
</SEC-HEADER>
<DOCUMENT><TEXT><p>clock fixture</p></TEXT></DOCUMENT>'''
    bp = N.BulkClockPin('bulk',hashlib.sha256(bulk).hexdigest(),1,'0000000001-20-000001',clock,clock)
    np = N.NativePin('sgml',hashlib.sha256(native).hexdigest(),bp.accession,1,clock,clock,'native_sec_acceptance')
    receipts={}
    for body,pin in ((bulk,bp),(native,np)):
        receipts[pin.source_id]=json.dumps({'url':'https://www.sec.gov/original/'+pin.source_id,
                                           'status':200,'sha256':pin.sha256,'bytes':len(body),'error':None,
                                           'fetched_at':str(pin.fetched_at)}).encode()
    return {'document':(bulk,bp,(native,np))},receipts


def test_i3_auxiliary_originals_and_receipts_are_independently_frozen():
    clocks, receipts = auxiliary()
    frozen = N.clock_inventory(clocks, receipts)
    bulk,bp,(native,np)=clocks['document']
    html=b'<p>original financial document</p>'
    docpin=replace(np,source_id='document',sha256=hashlib.sha256(html).hexdigest(),clock_basis='native_sec_bulk_acceptance')
    N.replay_native({'document':html},(docpin,),admission_id='fixture',expected_code=N.code_pins(),
                    bulk_clocks=clocks,expected_auxiliary=frozen,clock_receipts=receipts)
    changed = bulk+b' '
    swapped={'document':(changed,replace(bp,sha256=hashlib.sha256(changed).hexdigest()),(native,np))}
    newreceipts=dict(receipts);record=json.loads(newreceipts['bulk']);record.update(sha256=hashlib.sha256(changed).hexdigest(),bytes=len(changed))
    newreceipts['bulk']=json.dumps(record).encode()
    with pytest.raises(ValueError,match='frozen auxiliary'):
        N.replay_native({'document':html},(docpin,),admission_id='fixture',expected_code=N.code_pins(),
                        bulk_clocks=swapped,expected_auxiliary=frozen,clock_receipts=newreceipts)
    with pytest.raises(ValueError,match='receipt inventory'):
        N.clock_inventory(clocks,{})
