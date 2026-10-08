"""Strict reproduced TNIC conditional success and calibration clock failures."""
import datetime as dt
import json
from pathlib import Path

from atx_db.alpha_panel import common as C, filing_text_native as N, tnic as LT, tnic_native as T


def fixture(root,con,late_sic=False):
    con.execute('CREATE TABLE nt_text(cik BIGINT,accession VARCHAR,form VARCHAR,filing_date DATE,available_at TIMESTAMPTZ,business_vocabulary VARCHAR[],reason VARCHAR,business_reason VARCHAR)')
    con.execute('CREATE TABLE nt_inventory(cik BIGINT,form VARCHAR,filing_date DATE)')
    con.execute('CREATE TABLE nt_links(y INTEGER,cik BIGINT)')
    con.execute('CREATE TABLE nt_source_closure(y INTEGER,complete BOOLEAN,available_at TIMESTAMPTZ,source_sha256 VARCHAR)')
    for year in range(2018,2027):
        con.execute('INSERT INTO nt_source_closure VALUES (?,true,?,?)',[year,dt.datetime(year+1,1,1,tzinfo=dt.timezone.utc),'synthetic_complete_source_census'])
    plan={'files':{},'sic_sources':[],'code':{str(Path(LT.__file__).resolve()):N.pin(LT.__file__)}}
    for cik in range(1,9):
        group=(cik-1)//2
        # Each pair shares 40 distinct alphabetic words, each firm has 30
        # private words. Pair words appear in 25%, so strict >25% DF removes
        # none. Alphabetic construction respects the original tokenizer.
        def token(prefix,i):
            return prefix+chr(97+i//26)+chr(97+i%26)
        words=[token('shared'+chr(97+group),i) for i in range(40)]+[token('private'+chr(97+cik),i) for i in range(30)]
        for year in (2018,2019):
            clock=dt.datetime(year,3,1,12,tzinfo=dt.timezone.utc)
            con.execute('INSERT INTO nt_text VALUES (?,?,?,?,?,?,NULL,NULL)',[cik,f'{cik}-{year}','10-K',dt.date(year,3,1),clock,words])
            con.execute('INSERT INTO nt_inventory VALUES (?,?,?)',[cik,'10-K',dt.date(year,3,1)])
            con.execute('INSERT INTO nt_links VALUES (?,?)',[year,cik])
        source=f'sic-{cik}.json'
        (root/source).write_text(json.dumps({'cik':cik,'sic':str(1000+group*100)}))
        receipt=f'sic-{cik}.receipt.json'
        url=f'https://data.sec.gov/submissions/CIK{cik:010d}.json'
        (root/receipt).write_text(json.dumps(dict(http_status=200,error=None,url=url,fetched_at='2026-01-01T00:00:00Z' if late_sic else '2018-01-01T00:00:00Z',**N.pin(root/source))))
        plan['files'][source]=N.pin(root/source)
        plan['files'][receipt]=N.pin(root/receipt)
        plan['sic_sources'].append(dict(path=source,receipt=receipt,url=url))
    return plan


def test_strict_reproduction_conditionally_publishes_and_corpus_clock(tmp_path):
    con=C.connect(memory='128MB',threads=1,temp_dir=tmp_path/'scratch',db_file='native.duckdb')
    try:
        plan=fixture(tmp_path,con)
        T.build_native(con,tmp_path/'output',tmp_path,plan)
        assert con.execute('SELECT count(*) FROM nt_pairs WHERE year=2019').fetchone()[0]==8
        assert con.execute('SELECT count(*) FROM nt_peer_firms WHERE year=2019 AND n_peers>0').fetchone()[0]==8
        assert con.execute('SELECT min(available_at) FROM nt_pairs').fetchone()[0]==dt.datetime(2020,1,1,tzinfo=dt.timezone.utc)
        assert con.execute('SELECT count(*) FROM nt_pairs WHERE score<=threshold').fetchone()[0]==0
    finally:
        con.close()


def test_future_sic_snapshot_cannot_qualify_historical_fit(tmp_path):
    con=C.connect(memory='128MB',threads=1,temp_dir=tmp_path/'scratch',db_file='native.duckdb')
    try:
        plan=fixture(tmp_path,con,late_sic=True)
        T.build_native(con,tmp_path/'output',tmp_path,plan)
        assert con.execute('SELECT count(*) FROM nt_pairs').fetchone()[0]==0
        assert con.execute("SELECT calibration_reason FROM nt_years WHERE year=2019").fetchone()[0]=='prior_year_corpus_or_SIC_clock_unqualified'
    finally:
        con.close()


def test_fallback_never_counts_as_strict_gate(tmp_path):
    con=C.connect(memory='128MB',threads=1,temp_dir=tmp_path/'scratch',db_file='native.duckdb')
    try:
        plan=fixture(tmp_path,con)
        # All historical SICs distinct: threshold excludes the positive pairs.
        for item in plan['sic_sources']:
            path=tmp_path/item['path']
            raw=json.loads(path.read_text())
            raw['sic']=str(1000+int(raw['cik'])*100)
            path.write_text(json.dumps(raw))
            receipt_path=tmp_path/item['receipt']
            receipt=json.loads(receipt_path.read_text())
            receipt.update(N.pin(path))
            receipt_path.write_text(json.dumps(receipt))
            plan['files'][path.name]=N.pin(path)
            plan['files'][receipt_path.name]=N.pin(receipt_path)
        T.build_native(con,tmp_path/'output',tmp_path,plan)
        assert con.execute("SELECT count(*) FROM nt_pairs WHERE year=2019 AND peer_basis='nearest5_fallback'").fetchone()[0]==8
        assert con.execute('SELECT count(*) FROM nt_peer_firms WHERE year=2019 AND n_peers>0').fetchone()[0]==0
        assert con.execute('SELECT count(*) FROM nt_peer_firms WHERE year=2019 AND n_fallback_peers>0').fetchone()[0]==8
    finally:
        con.close()
