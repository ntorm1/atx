"""Original quarterly SEC-index corpus closure; offline only."""
import datetime as dt
import json

import pytest

from atx_db.alpha_panel import common as C, filing_text_native as N


def sources(root):
    plan={'files':{},'sec_indexes':[]}
    for quarter in range(1,5):
        path=f'index-{quarter}.idx'
        line='1|Synthetic issuer|10-K|2018-03-01|edgar/data/1/0000000001-18-000001.txt\n' if quarter==1 else ''
        (root/path).write_text('CIK|Company Name|Form Type|Date Filed|Filename\n'+line)
        receipt=f'index-{quarter}.receipt.json'
        url=f'https://www.sec.gov/Archives/edgar/full-index/2018/QTR{quarter}/master.idx'
        (root/receipt).write_text(json.dumps(dict(http_status=200,error=None,url=url,fetched_at='2019-01-02T00:00:00Z',**N.pin(root/path))))
        plan['files'][path]=N.pin(root/path)
        plan['files'][receipt]=N.pin(root/receipt)
        plan['sec_indexes'].append(dict(year=2018,quarter=quarter,path=path,receipt=receipt))
    return plan


def test_original_census_closure_and_unknown_years(tmp_path):
    con=C.connect(memory='128MB',threads=1,temp_dir=tmp_path/'scratch',db_file='native.duckdb')
    try:
        con.execute('CREATE TABLE nt_inventory(cik BIGINT,accession VARCHAR,form VARCHAR,filing_date DATE)')
        con.execute("INSERT INTO nt_inventory VALUES (1,'0000000001-18-000001','10-K','2018-03-01')")
        N.derive_corpus_closure(tmp_path,sources(tmp_path),con)
        assert con.execute('SELECT complete,available_at FROM nt_source_closure WHERE y=2018').fetchone()==(True,dt.datetime(2019,1,2,tzinfo=dt.timezone.utc))
        assert con.execute('SELECT complete,available_at FROM nt_source_closure WHERE y=2019').fetchone()==(False,None)
    finally:
        con.close()


def test_census_rejects_orphan_native_accession(tmp_path):
    con=C.connect(memory='128MB',threads=1,temp_dir=tmp_path/'scratch',db_file='native.duckdb')
    try:
        con.execute('CREATE TABLE nt_inventory(cik BIGINT,accession VARCHAR,form VARCHAR,filing_date DATE)')
        con.execute("INSERT INTO nt_inventory VALUES (1,'0000000001-18-000099','10-K','2018-03-01')")
        with pytest.raises(ValueError,match='corpus differs'):
            N.derive_corpus_closure(tmp_path,sources(tmp_path),con)
    finally:
        con.close()


def test_original_accepted_index_clock_and_DST_ambiguity(tmp_path):
    proofs=[]
    files={}
    for cik,local in ((1,'2018-03-01 07:00:00'),(2,'2018-11-04 01:30:00')):
        accession=f'{cik:010d}-18-000001'
        path=f'{cik}.index.htm'
        (tmp_path/path).write_text(f'<html><div>SEC Accession No. {accession}</div><div>CIK: {cik:010d}</div><div>Accepted</div><div>{local}</div></html>')
        receipt=f'{cik}.receipt.json'
        url=f"https://www.sec.gov/Archives/edgar/data/{cik}/{accession.replace('-','')}/{accession}-index.htm"
        (tmp_path/receipt).write_text(json.dumps(dict(http_status=200,error=None,url=url,**N.pin(tmp_path/path))))
        files[path]=N.pin(tmp_path/path)
        files[receipt]=N.pin(tmp_path/receipt)
        proofs.append(dict(cik=cik,accession=accession,index_path=path,receipt=receipt))
    result=N.acceptance_proofs(tmp_path,{'files':files,'acceptance_proofs':proofs})
    assert result[(1,'0000000001-18-000001')]==dt.datetime(2018,3,1,12,tzinfo=dt.timezone.utc)
    assert result[(2,'0000000002-18-000001')] is None
