"""R1 findings5/6/7: semantic filing identity, PIT cells and bounded originals."""
import datetime as dt
import json
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import common as C,filing_text_native as N,text_features_native as P,tnic_native as T
from tests.test_text_features_native import native


def test_accepted_body_must_authenticate_accession_and_owner(native):
    root,_,plan=native
    proof=plan['acceptance_proofs'][0]
    index=root/proof['index_path']
    for body in ('Accepted 2018-03-01 07:00:00',
                 'SEC Accession No. 0000000099-18-000001 CIK: 99 Accepted 2018-03-01 07:00:00',
                 f"SEC Accession No. {proof['accession']} CIK: 99 Accepted 2018-03-01 07:00:00"):
        index.write_text('<html>'+body+'</html>')
        plan['files'][proof['index_path']]=N.pin(index)
        receipt=root/proof['receipt']
        value=json.loads(receipt.read_text());value.update(N.pin(index));receipt.write_text(json.dumps(value))
        assert N.acceptance_proofs(root,plan)[(proof['cik'],proof['accession'])] is None
    acc=proof['accession']
    assert N.index_identity(f'<html>SEC Accession No. {acc} CIK: 1 CIK: 2</html>'.encode(),2,acc,1)
    assert not N.index_identity(f'<html>SEC Accession No. {acc} CIK: 2</html>'.encode(),2,acc,1)


def test_PIT_identity_resolves_null_and_keeps_ambiguous_future_rejected_cells(native,tmp_path):
    root,_,plan=native
    members=pq.read_table(root/'members.parquet').to_pylist()
    for row in members:
        if row['security_id']==1:row['cik']=None
    pq.write_table(pa.Table.from_pylist(members),root/'members.parquet')
    start=dt.date(2018,1,1);end=dt.date(2026,12,31);clock=dt.datetime(2018,1,1,tzinfo=dt.timezone.utc)
    def link(s,c,t='strict',a=clock):return dict(security_id=s,cik=c,valid_from=start,valid_to=end,link_tier=t,available_at=a)
    rows=[link(1,1,'dated'),link(1,99,'backfill'),link(1,99,a=dt.datetime(2030,1,1,tzinfo=dt.timezone.utc)),link(2,2),link(2,1)]
    pq.write_table(pa.Table.from_pylist(rows),root/'identity.parquet')
    con=C.connect(memory='128MB',threads=1,temp_dir=tmp_path/'scratch',db_file='native.duckdb')
    try:
        N.derive_population(root,plan,con)
        assert con.execute('SELECT count(*) FROM nt_members').fetchone()[0]==4
        assert con.execute('SELECT DISTINCT cik,identity_reason FROM nt_members WHERE security_id=1').fetchall()==[(1,None)]
        assert con.execute('SELECT DISTINCT cik,identity_reason FROM nt_members WHERE security_id=2').fetchall()==[(None,'ambiguous_native_identity')]
        assert con.execute('SELECT DISTINCT cik FROM nt_links').fetchall()==[(1,)]
    finally:con.close()


def test_current_profile_rejected_before_token_allocation(native,monkeypatch):
    root,_,plan=native
    original=plan['originals'][-1]
    (root/original['path']).write_text('<html>Item 1. Business '+('word '*500_001)+'</html>')
    record=dict(cik=original['cik'],accession=original['accession'],reason=None,primary_document='annual.htm',form='10-K')
    monkeypatch.setattr(P.TF,'section_profile',lambda _:pytest.fail('oversized current profile was allocated'))
    with pytest.raises(ValueError,match='bounded current issuer'):
        P._profile(root,record,N.original_map(root,plan),plan)


def test_SIC_and_EX13_index_bounds_apply_before_read(native,tmp_path):
    root,_,plan=native
    source=root/'oversized-sic.json';source.write_bytes(b'x'*(16*1024*1024+1))
    receipt=root/'sic.receipt.json';receipt.write_text('{}')
    sic_plan={'files':{source.name:N.pin(source),receipt.name:N.pin(receipt)},'sic_sources':[dict(path=source.name,receipt=receipt.name,url='unused')]}
    con=C.connect(memory='128MB',threads=1,temp_dir=tmp_path/'scratch',db_file='native.duckdb')
    try:
        with pytest.raises(ValueError,match='bounded original source'):T._sic_sources(root,sic_plan,con)
    finally:con.close()
    original=plan['originals'][0];proof=plan['acceptance_proofs'][0]
    index=root/proof['index_path'];index.write_bytes(b'x'*5_000_001)
    receipt=root/proof['receipt'];value=json.loads(receipt.read_text());value.update(N.pin(index));receipt.write_text(json.dumps(value))
    plan['files'][proof['index_path']]=N.pin(index)
    original.update(index_path=proof['index_path'],index_receipt=proof['receipt'],exhibits=[{}])
    with pytest.raises(ValueError,match='bounded original source'):list(N.authenticated_exhibits(root,plan,original))
