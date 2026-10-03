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
