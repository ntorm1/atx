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
