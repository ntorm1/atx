"""Offline primary reportability tests; values never determine the economic cut."""
import pytest
from atx_db.alpha_panel import fund_notes_semantic as P, fund_notes_structure as S

def document(statement='We have two reportable segments: Alpha and Beta.',unit=True,year='2024-12-31',table=True):
    contexts=''.join(f'<xbrli:context id="{m}"><xbrli:period><xbrli:startDate>2024-01-01</xbrli:startDate><xbrli:endDate>{year}</xbrli:endDate></xbrli:period><xbrldi:explicitMember dimension="us-gaap:StatementBusinessSegmentsAxis">x:{m}Member</xbrldi:explicitMember></xbrli:context>' for m in ['Alpha','Beta','Product'])
    units='<xbrli:unit id="USD"><xbrli:measure>iso4217:USD</xbrli:measure></xbrli:unit>' if unit else ''
    facts=''.join(f'<tr><td><ix:nonFraction name="us-gaap:Revenues" contextRef="{m}" unitRef="USD">50</ix:nonFraction></td></tr>' for m in ['Alpha','Beta','Product'])
    return (contexts+units+'<p>'+statement+'</p>'+('<table>'+facts+'</table>' if table else facts)).encode()

def roles():
    edges=[('us-gaap_Revenues',S.AXIS),(S.AXIS,S.DOMAIN),(S.DOMAIN,'x_AlphaMember'),(S.DOMAIN,'x_BetaMember'),(S.DOMAIN,'x_ProductMember')]
    return [{'role':'segment-note','kind':'presentationLink','invalid':[],'edges':[(a,b,'parent-child') for a,b in edges]}]

def cut(body, role=None):
    ctx=S.primary_contexts(body,'us-gaap_Revenues','2024-12-31')
    return P.semantic_cut(body,'us-gaap_Revenues','2024-12-31',{'Alpha','Beta','Product'},roles() if role is None else role,ctx)

def test_explicit_current_list_and_table_selects_reportable_above_products():
    body=document()
    proof=cut(body)
    assert proof['status']=='verified_structure' and proof['members']==['Alpha','Beta']
    claim=proof['corroboration']['claim']
    assert claim['certified'] and 'We have two' in body.decode()[claim['start_offset']:claim['end_offset']]
    assert proof['unit_refs']==['USD']

@pytest.mark.parametrize('statement',[
    'Previously, we have two reportable segments: Alpha and Beta.',
    'If approved, we have two reportable segments: Alpha and Beta.',
    'Following reorganization, we have two reportable segments: Alpha and Beta.',
    'In 2023, we have two reportable segments: Alpha and Beta.',
    'We have two operating segments: Alpha and Beta.',
    'We have two product lines: Alpha and Beta.',
    'We have two reportable segments: Alphabeta and Beta.',
])
def test_uncorroborated_or_temporal_list_refuses_certification(statement):
    assert cut(document(statement))['status']!='verified_structure'

def test_missing_context_unit_table_and_invalid_role_refuse():
    assert cut(document(unit=False))['status']=='missing_or_conflicting_units'
    assert cut(document(year='2023-12-31'))['status']=='missing_primary_context_identity'
    assert cut(document(table=False))['status']=='missing_current_revenue_table'
    assert cut(document(),[])['status']=='no_sound_primary_matched_role'
    bad=roles();bad[0]['invalid']=[{'use':'prohibited'}]
    assert cut(document(),bad)['status']=='no_sound_primary_matched_role'

def test_conflicting_primary_lists_and_named_all_other():
    assert cut(document('We have two reportable segments: Alpha and Beta.</p><p>We have two reportable segments: Alpha and Product.'))['status']=='conflicting_primary_lists'
    parsed=P.Primary('<p>We have two reportable segments: Alpha and All Other.</p>')
    claims=P.reportable_lists(parsed,'2024-12-31',{'Alpha','AllOther','OtherProduct'})
    assert claims[0]['members']==['AllOther','Alpha'] and claims[0]['quality']=='explicit_named_reportable_list'

def test_source_signed_bridge_exact_period_table_and_unit():
    body=document().decode()
    context='<xbrli:context id="elim"><xbrli:period><xbrli:startDate>2024-01-01</xbrli:startDate><xbrli:endDate>2024-12-31</xbrli:endDate></xbrli:period><xbrldi:explicitMember dimension="srt:ConsolidationItemsAxis">us-gaap:IntersegmentEliminationMember</xbrldi:explicitMember></xbrli:context>'
    fact='<tr><td><ix:nonFraction name="us-gaap:Revenues" contextRef="elim" unitRef="USD" sign="-">10</ix:nonFraction></td></tr>'
    body=(context+body.replace('</td></tr>',fact.replace('<tr><td>','').replace('</td></tr>','')+'</td></tr>',1)).encode()
    role=roles();role[0]['edges'].append((S.DOMAIN,'us-gaap_IntersegmentEliminationMember','parent-child'))
    bridge=P.source_bridge_contexts(body,'us-gaap_Revenues','2024-12-31',{'Alpha','Beta'},role)
    assert bridge['IntersegmentElimination']['context_ids']==['elim']
    assert bridge['IntersegmentElimination']['segments']=='ConsolidationItems=IntersegmentElimination;'
    assert not P.source_bridge_contexts(body,'us-gaap_Revenues','2023-12-31',{'Alpha','Beta'},role)
    assert not P.source_bridge_contexts(body.replace(b'contextRef="elim" unitRef="USD"',b'contextRef="elim" unitRef="EUR"'),'us-gaap_Revenues','2024-12-31',{'Alpha','Beta'},role)
    separate=(context+document().decode().replace('</table>',fact+'</table>')).encode()
    assert not P.source_bridge_contexts(separate,'us-gaap_Revenues','2024-12-31',{'Alpha','Beta'},role)

def test_independent_named_disclosure_without_numeric_axis():
    from atx_db.alpha_panel.fund_notes_disclosure import named_current_disclosures as named
    text='We currently have two reportable segments: Industrial Services and Water Systems.'
    proof=named(text,'2024-12-31')[0]
    assert proof['names']==['Industrial Services','Water Systems']
    assert proof['certified_current_named_disclosure'] and not proof['certified']
    assert text[proof['start_offset']:proof['list_end_offset']]==proof['source_text']
    for prefix in ['In 2020, ','Previously, ','If approved, ','After reorganization, ']:assert not named(prefix+text,'2024-12-31')
    assert not named('We have two operating segments: Alpha and Beta.','2024-12-31')
    assert not named('We have two reportable segments: Alpha provides services and Beta sells products.','2024-12-31')

def test_explicit_reportable_corporate_name_remains_an_economic_member():
    parsed=P.Primary('<p>We have three reportable segments: Personal Systems, Printing and Corporate Investments.</p>')
    claims=P.reportable_lists(parsed,'2024-12-31',{'PersonalSystems','Printing','CorporateInvestments'})
    assert claims[0]['quality']=='explicit_named_reportable_list'
    assert claims[0]['members']==['CorporateInvestments','PersonalSystems','Printing']

@pytest.mark.parametrize('statement',[
    'We have two reportable segments: Gamma and Delta. Our products Alpha and Beta generate revenue.',
    'We have two reportable segments:</p><p>Previously Alpha and Beta.',
    'We have two reportable segments: Alpha provides services and Beta sells products.',
    'We have two reportable segments: Gamma and Beta. Alpha also generates revenue.',
    'We have two reportable segments:</p><p>Alpha and</p><p>Previously Beta.',
    'We have two reportable segments:</p><p>Alpha and</p><p>Beta in 2023.',
    'We have two reportable segments:</p><p>Alpha and Beta, which will be reorganized.',
    'We have two reportable segments:</p><p>Alpha</p><p>Beta</p><p>Previously these were our segments.',
    'We have two reportable segments: Alpha and Beta. Previously these were our segments.',
    'We have two reportable segments: Alpha and Beta.</p><p>These segments were previously reported.',
    'We have two reportable segments: Alpha and Beta.</p><p>As of 2023 these were the segments.',
    'We have two reportable segments:</p><p>Alpha</p><p>Our products Beta generate revenue.',
])
def test_declared_list_never_fills_count_from_unrelated_or_qualified_names(statement):
    assert cut(document(statement))['status']!='verified_structure'

@pytest.mark.parametrize('statement',[
    'We have two reportable segments:</p><p>Alpha and Beta.',
    'We have two reportable segments:</p><p>Alpha</p><p>Beta.',
    'We have two reportable segments:</p><p>Alpha and</p><p>Beta.',
    'We have two reportable segments:</p><p>(1) Alpha;</p><p>(2) Beta.',
    'We have two reportable segments: Alpha and Beta. Our products Product generate revenue.',
    'We have two reportable segments: Alpha and Beta (2024).',
    'We have two reportable segments: Alpha and Beta.</p><p>Given the market trends, we expect prices to change.',
])
def test_genuine_bounded_multiline_declared_lists_are_certifiable(statement):
    proof=cut(document(statement))
    assert proof['status']=='verified_structure' and proof['members']==['Alpha','Beta']
    assert len(proof['corroboration']['claim']['list_items'])==2

def test_syntactic_list_preserves_and_within_a_member_name():
    parsed=P.Primary('<p>We have two reportable segments: Oil and Natural Gas, Carbon Management.</p>')
    claim=P.reportable_lists(parsed,'2024-12-31',{'OilAndNaturalGasSegment','CarbonManagementSegment'})[0]
    assert claim['quality']=='explicit_named_reportable_list'
    assert claim['members']==['CarbonManagementSegment','OilAndNaturalGasSegment']

def external_document(heading='Sales to external customers',year='2024-12-31'):
    body=document(year=year).decode()
    facts=''.join(f'<tr><td>{m}<ix:nonFraction name="us-gaap:RevenueFromContractWithCustomerExcludingAssessedTax" contextRef="{m}" unitRef="USD">40</ix:nonFraction></td></tr>' for m in ['Alpha','Beta'])
    total='<ix:nonFraction name="us-gaap:RevenueFromContractWithCustomerExcludingAssessedTax" contextRef="company" unitRef="USD">80</ix:nonFraction>'
    body=body.replace('</table>',f'<tr><td>{heading}</td></tr>'+facts+f'<tr><td>Total sales{total}</td></tr></table>')
    body='<xbrli:context id="company"><xbrli:period><xbrli:startDate>2024-01-01</xbrli:startDate><xbrli:endDate>'+year+'</xbrli:endDate></xbrli:period></xbrli:context>'+body
    role=roles();role[0]['edges'].append(('us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax',S.AXIS,'parent-child'))
    return body.encode(),role

def test_external_source_rows_select_measure_before_any_values():
    body,role=external_document()
    proof=P.source_external_revenue(body,'2024-12-31',{'Alpha','Beta'},role)
    assert proof['concept']=='us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax'
    assert proof['currency']=='USD' and proof['segments']=={'Alpha':'BusinessSegments=Alpha;','Beta':'BusinessSegments=Beta;'}
    assert proof['source_label']=='Sales to external customers'
    assert proof['company_total_context_ids']==['company']
    assert body.decode()[proof['start_offset']:proof['end_offset']].count('nonFraction')==4
    for heading in ['Total sales','Intersegment sales','External sales may increase']:
        bad,role=external_document(heading)
        assert not P.source_external_revenue(bad,'2024-12-31',{'Alpha','Beta'},role)
    assert not P.source_external_revenue(body,'2023-12-31',{'Alpha','Beta'},role)
    assert not P.source_external_revenue(body,'2024-12-31',{'Alpha','Beta','Product'},role)
    assert not P.source_external_revenue(body.replace(b'contextRef="Beta" unitRef="USD"',b'contextRef="Beta" unitRef="EUR"'),'2024-12-31',{'Alpha','Beta'},role)
    assert not P.source_external_revenue(body.replace(b'Total sales',b'Unknown total'),'2024-12-31',{'Alpha','Beta'},role)
    assert not P.source_external_revenue(body.replace(b'contextRef="company"',b'contextRef="missing"'),'2024-12-31',{'Alpha','Beta'},role)
    assert not P.source_external_revenue(body.replace(b'<tr><td>Alpha',b'<tr><td>Intersegment sales</td></tr><tr><td>Alpha',1),'2024-12-31',{'Alpha','Beta'},role)

def test_customer_revenue_row_requires_complete_annual_member_identity():
    body,role=external_document()
    text=body.decode();start=text.index('<tr><td>Sales to external customers')
    text=text[:start]+text[start:].replace('<tr><td>Sales to external customers</td></tr><tr><td>Alpha','<tr><td>Revenues from customers Alpha').replace('</td></tr><tr><td>Beta',' Beta')
    text=text.replace('</td></tr><tr><td>Total sales',' Total sales')
    proof=P.source_external_revenue(text.encode(),'2024-12-31',{'Alpha','Beta'},role)
    assert proof['concept']=='us-gaap_RevenueFromContractWithCustomerExcludingAssessedTax'
    assert len(proof['source_rows'])==1
