import datetime as dt
import html
import json
from types import SimpleNamespace
import pytest
from atx_db.alpha_panel import issuer_history as I

def header(state='DE',cik=12,changed='20171229'):
    return f'''<SEC-HEADER><ACCEPTANCE-DATETIME>20171229170000
ACCESSION NUMBER: 0000000012-17-000001
CONFORMED SUBMISSION TYPE: 10-K
FILED AS OF DATE: 20171229
DATE AS OF CHANGE: {changed}
FILER:
 COMPANY DATA:
 CENTRAL INDEX KEY: {cik:010d}
 STATE OF INCORPORATION: {state}
 BUSINESS ADDRESS:
 STATE: NY
</SEC-HEADER>'''.encode()

def test_exact_issuer_and_actual_conservative_clock():
    obs=I.parse_header(header(),12,'0000000012-17-000001')
    assert obs['inc_state_country']=='DE'
    assert obs['accepted_at']==dt.datetime(2017,12,29,22,tzinfo=dt.UTC)
    assert obs['available_at']==dt.datetime(2017,12,30,4,59,59,tzinfo=dt.UTC)
    assert I.parse_header(header(cik=99),12,'0000000012-17-000001') is None
    assert I.parse_header(header(),12,'wrong') is None
    assert I.parse_header(header().replace(b'STATE OF INCORPORATION',b'STATE OF BUSINESS'),12,'0000000012-17-000001') is None
    assert I.parse_header(header('XX'),12,'0000000012-17-000001') is None
    assert I.parse_header(header('SC'),12,'0000000012-17-000001')['inc_state_country']=='SC'

def test_later_change_cannot_backdate_and_html_entities():
    raw=html.escape(header('E9',changed='20190102').decode()).encode()
    obs=I.parse_header(raw,12,'0000000012-17-000001')
    assert obs['inc_state_country']=='E9'
    assert obs['available_at']==dt.datetime(2019,1,3,4,59,59,tzinfo=dt.UTC)

def test_multi_entity_header_never_uses_other_company_or_address_state():
    other=b'COMPANY DATA:\n CENTRAL INDEX KEY: 99\n STATE OF INCORPORATION: E9\n BUSINESS ADDRESS:\n STATE: NY\n'
    obs=I.parse_header(header().replace(b'FILER:',b'FILER:\n'+other),12,'0000000012-17-000001')
    assert obs['inc_state_country']=='DE'
    contradictory=header().replace(b'</SEC-HEADER>',b'COMPANY DATA:\n CENTRAL INDEX KEY: 12\n STATE OF INCORPORATION: E9\n</SEC-HEADER>')
    assert I.parse_header(contradictory,12,'0000000012-17-000001') is None
    noncorporate=header().replace(b'CONFORMED SUBMISSION TYPE: 10-K',b'CONFORMED SUBMISSION TYPE: 4')
    assert I.parse_header(noncorporate,12,'0000000012-17-000001') is None

def test_pre_eligibility_selection_and_later_redomicile():
    first=dt.date(2018,1,2)
    rows=[{'accession':str(i),'form':f,'filing_date':d} for i,f,d in [(1,'10-K',dt.date(2017,12,1)),(2,'4',dt.date(2017,12,31)),(3,'8-K',dt.date(2018,5,1)),(4,'10-Q',dt.date(2018,11,1)),(5,'10-K',dt.date(2023,2,1))]]
    selected=I.select_observations(rows,first)
    assert [r['accession'] for r in selected]==['1','4']
    assert selected[0]['filing_date']<first

def test_every_http_attempt_paced_persisted_and_capped(tmp_path,monkeypatch):
    now=[100.0];grants=[]
    def sleep(n):now[0]+=n
    inner=SimpleNamespace(acquire=lambda label:grants.append(label) or now[0],lock_path=tmp_path/'shared')
    monkeypatch.setattr(I.H,'default_sec_limiter',lambda:inner)
    path=tmp_path/'attempts.jsonl'
    limiter=I.RoundLimiter(path,cap=2,clock=lambda:now[0],sleep=sleep)
    limiter.acquire('first');limiter.acquire('retry')
    with pytest.raises(I.RequestBudgetExhausted):limiter.acquire('redirect')
    records=[json.loads(s) for s in path.read_text().splitlines()]
    assert records[1]['time']-records[0]['time']>=1
    assert grants==['first','retry']
    resumed=I.RoundLimiter(path,cap=2,clock=lambda:now[0],sleep=sleep)
    with pytest.raises(I.RequestBudgetExhausted):resumed.acquire('resume')
