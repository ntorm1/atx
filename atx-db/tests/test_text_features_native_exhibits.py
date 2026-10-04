"""Original index-typed EX13 byte replay on a synthetic annual filing."""
import json

import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import filing_text as FT, filing_text_native as N, text_features_native as P
from tests.test_text_features_native import native


def test_original_EX13_source_is_authenticated_and_replayed(native):
    root,output,plan=native
    original=next(r for r in plan['originals'] if r['cik']==1 and r['accession'].split('-')[1]=='20')
    primary=root/original['path']
    text=primary.read_text()
    start=text.index('Item 1A. Risk Factors')
    end=text.index('Item 7. Management')
    primary.write_text(text[:start]+'Item 1A. Risk Factors\nRisk factors are incorporated by reference to the annual report to shareholders.\n'+text[end:])
    receipt_path=root/original['receipt']
    receipt=json.loads(receipt_path.read_text())
    receipt.update(N.pin(primary))
    receipt_path.write_text(json.dumps(receipt))
    proof=next(p for p in plan['acceptance_proofs'] if p['cik']==1 and p['accession']==original['accession'])
    index=root/proof['index_path']
    url=f"{FT.ARCHIVES}/1/{original['accession'].replace('-','')}/annual-report.htm"
    index.write_text(index.read_text()+f'<table><tr><td><a href="{url}">annual report</a></td><td>EX-13</td></tr></table>')
    index_receipt_path=root/proof['receipt']
    index_receipt=json.loads(index_receipt_path.read_text())
    index_receipt.update(N.pin(index))
    index_receipt_path.write_text(json.dumps(index_receipt))
    exhibit=root/'annual-report.htm'
    exhibit.write_text('<html><div>Risk Factors</div><div>'+'The company manufactures specialized electronic products and faces commercial competition. '*100+'</div><div>Consolidated Financial Statements</div></html>')
    exhibit_receipt=root/'exhibit.receipt.json'
    exhibit_receipt.write_text(json.dumps(dict(http_status=200,error=None,url=url,**N.pin(exhibit))))
    original.update(index_path=proof['index_path'],index_receipt=proof['receipt'],exhibits=[dict(path=exhibit.name,receipt=exhibit_receipt.name,url=url)])
    for path in (primary,receipt_path,index,index_receipt_path,exhibit,exhibit_receipt):
        plan['files'][path.name]=N.pin(path)
    generation=P.publish_native(root,output,plan,N.digest(plan))
    row=next(r for r in pq.read_table(generation/'text/features.parquet').to_pylist() if r['cik']==1 and r['accession']==original['accession'])
    assert row['risk_source']=='ex13' and row['risk_words']>50
    assert row['risk_sim_cosine'] is not None
    original['exhibits'][0]['url']='https://www.sec.gov/Archives/edgar/data/1/other.htm'
    with pytest.raises(ValueError,match='not typed EX-13'):
        list(N.authenticated_exhibits(root,plan,original))
