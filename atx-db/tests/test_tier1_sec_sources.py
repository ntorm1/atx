"""Full original SEC source publication/reader fixture; no production IO."""
import json
import zipfile
from pathlib import Path
import duckdb
import pytest
from atx_db import _submissions_archive as SA
from atx_db.alpha_panel import private_tier1_admission as A
from atx_db.alpha_panel import private_tier1_event_replay as E
from atx_db.alpha_panel import private_tier1_sec_sources as S


def _json(path,value):
    path.write_text(json.dumps(value),encoding='utf-8');return A.pin(path)


def test_full_sec_source_publication_reader_and_early_close_original_mutation(tmp_path):
    source=tmp_path/'originals';source.mkdir();stage=tmp_path/'source-products';stage.mkdir()
    accession='0000000001-19-000001'
    recent={'accessionNumber':[accession],'filingDate':['2019-01-02'],
        'reportDate':['2019-01-01'],'acceptanceDateTime':['2019-01-02T15:00:00Z'],
        'form':['8-K'],'items':['5.02'],'primaryDocument':['original.htm']}
    history={**recent,'accessionNumber':['0000000001-18-000001'],'filingDate':['2018-01-02'],
        'reportDate':['2018-01-01'],'acceptanceDateTime':['2018-01-02T15:00:00Z']}
    archive=source/'submissions.zip';history_name='CIK0000000001-submissions-001.json'
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as output:
        output.writestr('CIK0000000001.json',json.dumps({'cik':1,'name':'Original Issuer',
            'filings':{'recent':recent,'files':[{'name':history_name}]}}))
        output.writestr(history_name,json.dumps(history))
    with SA.SubmissionsArchive(archive) as native:
        index,directory_receipt=native.index_path,native._receipt_path
    receipt=source/'download.receipt.json'
    _json(receipt,{'url':'https://www.sec.gov/Archives/edgar/daily-index/bulkdata/submissions.zip',
        'http_status':200,'error':None,'fetched_at':'2026-01-01T00:00:00Z',**A.pin(archive)})
    accepted=source/'original-index.htm'
    accepted.write_text(f'<html>CIK: 1 {accession} Accepted 2019-01-02 15:00:00</html>',encoding='utf-8')
    accepted_receipt=source/'original-index.receipt.json'
    _json(accepted_receipt,{'url':f'https://www.sec.gov/Archives/edgar/data/1/000000000119000001/{accession}-index.htm',
        'http_status':200,'error':None,'fetched_at':'2019-01-03T00:00:00Z',**A.pin(accepted)})
    clocks={'files':{accepted.name:A.pin(accepted),accepted_receipt.name:A.pin(accepted_receipt)},
        'acceptance_proofs':[{'cik':1,'accession':accession,'index_path':accepted.name,'receipt':accepted_receipt.name}]}
    clock_path=tmp_path/'clock-plan.json';clock_pin=_json(clock_path,clocks)
    extraction={str(path):A.pin(path) for path in (archive,receipt,index,directory_receipt)}
    extraction.update({**E.code_pins(),str(Path(SA.__file__).resolve()):A.pin(SA.__file__)})
    clock_dependencies={**E.code_pins(),**A.code(('filing_text_native','filing_text')),
        str(accepted):A.pin(accepted),str(accepted_receipt):A.pin(accepted_receipt)}
    plan={'schema':'atx.tier1-sec-original-plan/v1','archive_path':str(archive),'receipt_path':str(receipt),
        'source_root':str(source),'clock_plan':str(clock_path),'clock_plan_pin':clock_pin,
        'extraction_dependency_pins':extraction,'clock_dependency_pins':clock_dependencies}
    plan_path=tmp_path/'source-plan.json';plan_pin=_json(plan_path,plan)
    execution=tmp_path/'execution.json';execution_pin=_json(execution,S.EXECUTION)
    spec={'interface_version':S.VERSION,'plan_path':str(plan_path),'plan_pin':plan_pin,
        'execution_contract':str(execution),'execution_pin':execution_pin,
        'expected_code_sha256':A.digest(S.code_pins()),'stage_root':str(stage)}
    review=tmp_path/'independent-fixture-review.json'
    review_pin=_json(review,{'schema':'atx.tier1-sec-original-review/v1','status':'scoped_pass',
        'interface_version':S.VERSION,'binding_sha256':A.digest(S.binding(spec)),'code_pins':S.code_pins()})
    spec['independent_review']={'path':str(review),'pin':review_pin}
    spec['source_dependency_pins']={**extraction,**clock_dependencies,**S.code_pins(),
        str(plan_path):plan_pin,str(execution):execution_pin,str(clock_path):clock_pin,str(review):review_pin}
    with duckdb.connect(str(tmp_path/'replay.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        result=S.materialize(spec,con,'fixture')
        assert result['source_quality_certified'] is False
        spec.update({'generation':result['generation'],'manifest_pin':result['manifest_pin'],
            'product_pins':result['product_pins'],'metadata_pins':result['metadata_pins'],
            'expected_validation_sha256':result['validation_sha256']})
        generation=Path(result['generation'])
        spec['dependency_pins']={**spec['source_dependency_pins'],
            str(generation/'manifest.json'):result['manifest_pin'],
            str(generation/'start.json'):result['metadata_pins']['start.json'],
            **{str(generation/name):pin for name,pin in result['product_pins'].items()}}
        admitted=S.admit(spec,con)
        assert set(admitted['products'])==set(S.PRODUCTS)
        assert admitted['validation']['original_population']=={'main':1,'history':1,'raw_rows':2,'profile_rows':1}
        assert admitted['validation']['rows']['raw_filings.parquet']==2
        assert admitted['validation']['rows']['original_accepted.parquet']==2
        with S.consuming(admitted,con):
            sql=A.sql([admitted['products']['original_accepted.parquet']])
            assert con.execute('SELECT count(*) FROM '+sql+' WHERE original_accepted_clock_supported').fetchone()[0]==1
            assert con.execute('SELECT count(*) FROM '+sql+" WHERE original_accepted_reason='original_Accepted_evidence_missing'").fetchone()[0]==1
        # A retained source original changes while a consumer raises: finally
        # must detect that change rather than minting successful consumption.
        with pytest.raises(ValueError,match='dependency bytes changed'):
            with S.consuming(admitted,con):
                accepted.write_text(accepted.read_text()+' changed original',encoding='utf-8')
                raise RuntimeError('early consumer termination')
