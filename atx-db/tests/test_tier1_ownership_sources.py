"""Offline original-byte ownership clock phase; no database or source jobs."""
import hashlib
import json
import datetime as dt
from atx_db.alpha_panel import private_tier1_ownership_sources as O
from atx_db.alpha_panel import private_tier1_admission as A


def test_original_accepted_clock_requires_matching_header_and_preserves_obligations(tmp_path):
    accession='0000000001-19-000001'
    raw=('<SEC-DOCUMENT>\nACCESSION NUMBER: '+accession+
         '\nCONFORMED SUBMISSION TYPE: 13F-HR\n<ACCEPTANCE-DATETIME>20190702100000\n</SEC-DOCUMENT>').encode()
    index=tmp_path/'index.htm'
    index.write_text('CIK 1 '+accession+' Accepted 2019-07-02 10:00:00',encoding='utf-8')
    receipt=tmp_path/'index.receipt.json'
    receipt.write_text(json.dumps({'url':'https://www.sec.gov/Archives/edgar/data/1/000000000119000001/'+accession+'-index.htm',
        'http_status':200,**A.pin(index)}),encoding='utf-8')
    files={'index.htm':A.pin(index),'index.receipt.json':A.pin(receipt)}
    plan={'files':files,'acceptance_proofs':[{'cik':1,'accession':accession,'index_path':'index.htm','receipt':'index.receipt.json'}]}
    pins={str((tmp_path/key).resolve()):identity for key,identity in files.items()}
    expected={'accession':accession,'filer_cik':1,'form':'13F-HR','available_at':'2019-07-02T10:00:00Z'}
    retained={'sha256':hashlib.sha256(raw).hexdigest(),'url':'https://www.sec.gov/Archives/edgar/data/1/'+accession+'.txt',
              'fetched_at':'2019-07-02T15:00:00Z'}
    result=O.original_clock_phase(tmp_path,expected,retained,raw,plan,pins)
    assert result['qualified_clock'] and result['available_at']=='2019-07-02T14:00:00+00:00'
    assert result['source_qualified'] is False and result['qualified_original'] is False
    assert 'documentary_value_unit_unverified' in result['exceptions']
    wrong=raw.replace(b'20190702100000',b'20190702090000')
    retained={**retained,'sha256':hashlib.sha256(wrong).hexdigest()}
    rejected=O.original_clock_phase(tmp_path,expected,retained,wrong,plan,pins)
    assert rejected['qualified_clock'] is False and rejected['available_at'] is None


def test_original_instruction_unit_preserves_retrieval_floor_and_ambiguity(tmp_path):
    body=tmp_path/'instruction.htm'
    body.write_text('Form 13F Column 4 Value must be reported in thousands of dollars. Column 5 Shares.',encoding='utf-8')
    receipt=tmp_path/'instruction.receipt.json'
    def bind():
        receipt.write_text(json.dumps({'url':'https://www.sec.gov/info/edgar/form13f-instructions.htm',
            'http_status':200,'fetched_at':'2030-01-01T00:00:00Z',**A.pin(body)}),encoding='utf-8')
        return {str(path.resolve()):A.pin(path) for path in (body,receipt)}
    rule={'path':body.name,'receipt':receipt.name}
    result=O.original_unit_phase(tmp_path,rule,dt.datetime(2019,7,2,14,tzinfo=dt.UTC),bind())
    assert result['value_unit']=='USD_THOUSANDS' and result['value_multiplier']==1000.0
    assert result['available_at']=='2030-01-01T00:00:00+00:00'
    body.write_text('Form 13F Column 4 Value in thousands of dollars, alternatively in dollars. Column 5 Shares.',encoding='utf-8')
    result=O.original_unit_phase(tmp_path,rule,dt.datetime(2019,7,2,14,tzinfo=dt.UTC),bind())
    assert result['value_unit'] is None and result['available_at'] is None


def test_original_manager_route_requires_explicit_cover_and_no_confidential_omission():
    accession='0000000001-19-000001'
    def source(confidential):
        return ('<SEC-DOCUMENT>\nACCESSION NUMBER: '+accession+'\nCONFORMED SUBMISSION TYPE: 13F-HR\n'
            '<ACCEPTANCE-DATETIME>20190702100000\n<XML><root><credentials><cik>1</cik></credentials>'
            '<reportCalendarOrQuarter>06-30-2019</reportCalendarOrQuarter><tableEntryTotal>0</tableEntryTotal>'
            '<tableValueTotal>0</tableValueTotal><otherIncludedManagersCount>0</otherIncludedManagersCount>'
            '<isConfidentialOmitted>'+confidential+'</isConfidentialOmitted></root></XML>\n</SEC-DOCUMENT>').encode()
    expected={'accession':accession,'filer_cik':1,'form':'13F-HR'}
    def derive(raw):
        retained={'sha256':hashlib.sha256(raw).hexdigest(),'url':'https://www.sec.gov/Archives/edgar/data/1/'+accession+'.txt',
                  'fetched_at':'2019-07-02T15:00:00Z'}
        parsed=O.Q.original_submission_evidence(raw,expected,retained)
        return O.original_manager_phase(raw,parsed,expected)
    positive=derive(source('false'))
    assert positive['manager_chain_resolved'] and positive['table_absence_qualified']
    withheld=derive(source('true'))
    assert not withheld['manager_chain_resolved'] and not withheld['table_absence_qualified']
