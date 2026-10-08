import pytest
from atx_db.alpha_panel import issuer_classes as I

def doc(text,ciks=(12,)):
    return (''.join(f'<ix:nonNumeric name="dei:EntityCentralIndexKey">{c}</ix:nonNumeric>' for c in ciks)+text).encode()

def test_direct_named_issuer_and_tax_election():
    result=I.reit_assertion(doc('Hudson Pacific Properties, Inc. is a real estate investment trust.'),'HUDSON PACIFIC PROPERTIES INC.',12)
    assert result['status'] is True
    assert I.reit_assertion(doc('We have elected to be taxed as a real estate investment trust.'),'ISSUER INC',12)['status'] is True

def test_subsidiary_prospective_and_conditional_are_unknown():
    for text in ('Our subsidiary is a real estate investment trust.','We intend to qualify as a REIT.','We are a holding company and our subsidiary is a real estate investment trust.','We are an adviser to real estate investment trusts.','If we are a real estate investment trust, these rules apply.','We would qualify as a real estate investment trust if approved.','We have elected to be taxed as a REIT commencing next year.'):
        assert I.reit_assertion(doc(text),'ISSUER INC',12)['status'] is None

def test_multi_issuer_generic_unknown_unless_named_definition():
    assert I.reit_assertion(doc('The Company is a real estate investment trust.',(12,99)),'ISSUER INC',12)['status'] is None
    text='Issuer Inc. (the Company) owns properties. The Company is a real estate investment trust.'
    assert I.reit_assertion(doc(text,(12,99)),'ISSUER INC',12)['status'] is True
    assert I.reit_assertion(doc('The Company is a REIT.',(99,)),'ISSUER INC',12)['status'] is None

def test_current_termination_and_contradiction():
    assert I.reit_assertion(doc('We are no longer a REIT.'),'ISSUER INC',12)['status'] is False
    both=doc('We are a real estate investment trust. We are no longer a REIT.')
    assert I.reit_assertion(both,'ISSUER INC',12)['status'] is None
    assert I.reit_assertion(doc('We are no longer taxed as a REIT.'),'ISSUER INC',12)['status'] is False
    assert I.reit_assertion(doc('We will no longer be taxed as a REIT next year.'),'ISSUER INC',12)['status'] is None
    assert I.reit_assertion(doc('The Company is no longer required to operate under REIT rules.'),'ISSUER INC',12)['status'] is False
    assert I.reit_assertion(doc('Our subsidiary is no longer required to operate under REIT rules.'),'ISSUER INC',12)['status'] is None
    assert I.reit_assertion(doc('The Company approved a plan to terminate its REIT status next year.'),'ISSUER INC',12)['status'] is None
    for text in ('If we are no longer a REIT, corporate taxes apply.','When the Company is no longer required to operate under REIT rules, it may change operations.'):
        assert I.reit_assertion(doc(text),'ISSUER INC',12)['status'] is None

def test_original_report_period_is_provenance_not_availability():
    import datetime as dt
    assert I.report_period(b'CONFORMED PERIOD OF REPORT: 20210930')==dt.date(2021,9,30)
    assert I.report_period(b'CONFORMED PERIOD OF REPORT: 20211340') is None

def test_larger_retry_preserves_success_and_terminal_unknown(tmp_path):
    from types import SimpleNamespace
    import json
    class Store:
        def __init__(self):self.records={};self.calls=[]
        def lookup(self,url):return self.records.get(url)
        def fetch(self,url,**kwargs):
            self.calls.append(('fetch',url,kwargs['maximum']))
            return self.records[url]
        def ensure(self,url,**kwargs):
            self.calls.append(('ensure',url,kwargs['maximum']))
            return self.records[url],False
    store=Store();bad=SimpleNamespace(error=I.H.RESPONSE_TOO_LARGE,status=200,sha256=None)
    good=SimpleNamespace(error=None,status=200,sha256='verified')
    store.records={'large':bad,'good':good};retried=set();path=tmp_path/'retries.jsonl'
    assert I.fetch_source(store,'good',object(),retried,path) is good
    I.fetch_source(store,'large',object(),retried,path)
    I.fetch_source(store,'large',object(),retried,path)
    assert store.calls==[('ensure','good',64_000_000),('fetch','large',64_000_000),('ensure','large',64_000_000)]
    assert json.loads(path.read_text())['error']==I.H.RESPONSE_TOO_LARGE
    assert retried=={'large'}

def test_audit_keeps_recent_periodic_after_old_annual_and_excludes_future():
    import datetime as dt
    rows=[{'form':form,'accession':acc,'filing_date':dt.date.fromisoformat(day)} for form,acc,day in
          [('10-K','annual','2021-03-15'),('10-Q','post-close','2021-11-10'),('10-Q','future','2022-03-15')]]
    selected=I.audit_observations(rows,dt.date(2021,11,30))
    assert {r['accession'] for r in selected}=={'annual','post-close'}
    assert next(r for r in selected if r['accession']=='post-close')['priority']==-2


@pytest.mark.parametrize('text',[
    'We are a REIT only if the IRS approves our election.',
    'The Company is no longer required to operate under REIT rules if its revocation is accepted.',
    'Assuming the IRS approves the pending election and all other conditions are met, we are a REIT.',
    'We are a REIT provided that the IRS approves our election.',
    'We are a REIT; only if the IRS approves our election.',
    'Unless all of the unusually extensive requirements established by the IRS have been satisfied, we are a REIT.',
    'We are no longer a REIT subject to acceptance of the pending revocation.',
    'Assuming the I.R.S. approves the election, we are a REIT.',
    'We are no longer a REIT upon acceptance of our revocation.',
    'We are a REIT contingent on approval of our proposed election.',
])
def test_complete_conditional_assertion_context_is_never_an_event(text):
    result=I.reit_assertion(doc(text),'Example Corp',12)
    assert result['status'] is None
    assert result['evidence'] is None


def test_unrelated_sentence_condition_does_not_cancel_current_assertion():
    assert I.reit_assertion(doc('We are a REIT. If prices rise, revenue may improve.'),'Example Corp',12)['status'] is True


@pytest.mark.parametrize('text',[
    'We have elected to be treated as a REIT for U.S. federal income tax and intend to maintain this status in future periods.',
    'The Company has elected to be taxed as a REIT as defined under Section 856 of the Code for U.S. federal income tax purposes and expects to continue to operate to qualify as a REIT.',
])
def test_completed_election_with_separate_continuance_intention_is_current(text):
    assert I.reit_assertion(doc(text),'Example Corp',12)['status'] is True
    assert I.reit_assertion(doc(text[:-1]+' if the IRS approves our election.'),'Example Corp',12)['status'] is None


def test_class_prepare_extensions_retain_annual_audit_and_followup_paths(tmp_path,monkeypatch):
    import json,zipfile
    from atx_db.alpha_panel import common as C
    monkeypatch.setattr(C,'PACKAGE_ROOT',tmp_path)
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path/'lake'))
    cache=tmp_path/'data/cache';cache.mkdir(parents=True)
    cohort=tmp_path/'cohort.json';audit=tmp_path/'audit.json'
    C.write_json_atomic(cohort,{'candidates':[{'cik':12,'first_candidate':'2018-01-02','sic_values':[6500]}]})
    C.write_json_atomic(audit,{'samples':[{'cik_u':12,'session_date':'2022-12-31'}]})
    records=[('0000000012-21-000001','10-K','2021-06-01'),('0000000012-22-000001','10-K','2022-06-01')]
    def archive():
        body={'filings':{'recent':{'accessionNumber':[r[0] for r in records],'form':[r[1] for r in records],'filingDate':[r[2] for r in records],'primaryDocument':['original.htm']*len(records)}}}
        with zipfile.ZipFile(cache/'submissions.zip','w',compression=zipfile.ZIP_DEFLATED) as z:z.writestr('CIK0000000012.json',json.dumps(body))
    archive();first=I.prepare(cohort,audit)
    records.extend([('0000000012-22-000002','10-K','2022-11-01'),('0000000012-22-000003','10-Q','2022-12-01')]);archive()
    C.write_json_atomic(C.stage_dir(I.STAGE)/'assessment.json',{'rows':[{'cik':12,'status':True}]})
    second=I.prepare(cohort,audit,status_followup=True)
    assert {r['accession'] for r in first['rows']}<={r['accession'] for r in second['rows']}
    assert '0000000012-22-000003' in {r['accession'] for r in second['rows']}
    C.write_json_atomic(C.stage_dir(I.STAGE)/'assessment.json',{'rows':[{'cik':12,'status':None}]})
    C.write_json_atomic(audit,{'samples':[]})
    third=I.prepare(cohort,audit,status_followup=True)
    assert {r['accession'] for r in second['rows']}=={r['accession'] for r in third['rows']}
def test_completed_election_survives_separate_future_termination_sentence():
    result=I.reit_assertion(doc('We have elected to be taxed as a REIT. We will terminate our election next year.'),'Example Corp',12)
    assert result['status'] is True
    assert I.reit_assertion(doc('We have elected to be taxed as a REIT effective next year.'),'Example Corp',12)['status'] is None
