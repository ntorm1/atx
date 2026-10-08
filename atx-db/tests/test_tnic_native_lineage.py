"""R1 findings1/2: complete encoded calibration dependencies and original DF cohort."""
import datetime as dt
import pytest
from atx_db.alpha_panel import common as C,tnic_native as T
from tests.test_tnic_native import fixture


@pytest.mark.parametrize('missing',[True,False])
def test_filter_dependency_survives_later_calibration(tmp_path,missing):
    con=C.connect(memory='128MB',threads=1,temp_dir=tmp_path/'scratch',db_file='native.duckdb')
    try:
        plan=fixture(tmp_path,con)
        con.execute("INSERT INTO nt_text SELECT cik,replace(accession,'2019','2020'),form,filing_date+INTERVAL 1 YEAR,available_at+INTERVAL 1 YEAR,business_vocabulary,reason,business_reason FROM nt_text WHERE year(filing_date)=2019")
        con.execute('INSERT INTO nt_inventory SELECT cik,form,filing_date+INTERVAL 1 YEAR FROM nt_inventory WHERE year(filing_date)=2019')
        con.execute('INSERT INTO nt_links SELECT 2020,cik FROM nt_links WHERE y=2019')
        if missing:
            con.execute('UPDATE nt_source_closure SET complete=false,available_at=NULL WHERE y=2018')
        else:
            con.execute("UPDATE nt_source_closure SET available_at=TIMESTAMPTZ '2030-01-01' WHERE y=2018")
        T.build_native(con,tmp_path/'output',tmp_path,plan)
        clock=con.execute('SELECT available_at FROM nt_years WHERE year=2020').fetchone()[0]
        assert clock is None if missing else clock==dt.datetime(2030,1,1,tzinfo=dt.timezone.utc)
        if missing:
            assert con.execute('SELECT count(*) FROM nt_pairs WHERE year=2020').fetchone()[0]==0
    finally:con.close()


def test_short_raw_documents_do_not_change_DF(tmp_path):
    con=C.connect(memory='128MB',threads=1,temp_dir=tmp_path/'scratch',db_file='native.duckdb')
    try:
        plan=fixture(tmp_path,con)
        shared=con.execute('SELECT business_vocabulary[1] FROM nt_text LIMIT 1').fetchone()[0]
        for cik in range(20,60):
            con.execute("INSERT INTO nt_text VALUES (?,?, '10-K',DATE '2018-03-01',TIMESTAMPTZ '2018-03-01',?,NULL,NULL)",[cik,str(cik),[shared]])
        con.execute("INSERT INTO nt_text VALUES (1,'latest-short','10-K',DATE '2018-04-01',TIMESTAMPTZ '2018-04-01',?,NULL,NULL)",[[shared]])
        T.build_native(con,tmp_path/'output',tmp_path,plan)
        assert con.execute('SELECT count(DISTINCT accession) FROM nt_tokens WHERE y=2018').fetchone()[0]==8
        assert con.execute('SELECT count(*) FROM nt_common WHERE y=2018 AND word=?',[shared]).fetchone()[0]==0
        assert con.execute('SELECT docs FROM nt_years WHERE year=2018').fetchone()[0]==49
        assert con.execute('SELECT accession FROM nt_peer_candidates WHERE y=2018 AND cik=1').fetchone()[0]=='1-2018'
        assert con.execute('SELECT count(*) FROM nt_pairs WHERE year=2019').fetchone()[0]==8
    finally:con.close()
