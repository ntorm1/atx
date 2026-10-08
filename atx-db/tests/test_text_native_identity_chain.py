"""R1 findings5/6: original owner correspondence and NULL-member text coverage."""
import json
import pyarrow as pa
import pyarrow.parquet as pq
from atx_db.alpha_panel import common as C,filing_text_native as N,text_features_native as P
from tests.test_text_features_native import native


def test_semantic_owner_requires_original_submission_record(native,tmp_path):
    root,_,plan=native
    proof=plan['acceptance_proofs'][0]
    proof['archives_cik']=99
    index=root/proof['index_path']
    index.write_text(f"<html>SEC Accession No. {proof['accession']} CIK: 1 CIK: 99 Accepted 2018-03-01 07:00:00</html>")
    receipt=root/proof['receipt'];value=json.loads(receipt.read_text())
    value.update(url=f"https://www.sec.gov/Archives/edgar/data/99/{proof['accession'].replace('-','')}/{proof['accession']}-index.htm",**N.pin(index))
    receipt.write_text(json.dumps(value));plan['files'][proof['index_path']]=N.pin(index)
    con=C.connect(memory='128MB',threads=1,temp_dir=tmp_path/'scratch',db_file='native.duckdb')
    try:
        N.derive_population(root,plan,con)
        assert con.execute('SELECT available_at,reason FROM nt_inventory WHERE cik=1 AND accession=?',[proof['accession']]).fetchone()==(None,'unknown_acceptance_owner_chain')
    finally:con.close()


def test_null_member_CIK_resolves_for_text_coverage(native):
    root,output,plan=native
    rows=pq.read_table(root/'members.parquet').to_pylist()
    for row in rows:row['cik']=None
    pq.write_table(pa.Table.from_pylist(rows),root/'members.parquet')
    plan['files']['members.parquet']=N.pin(root/'members.parquet')
    generation=P.publish_native(root,output,plan,N.digest(plan))
    manifest=json.loads((generation/'manifest.json').read_text())
    year=next(r for r in manifest['coverage'] if r['year']==2020)
    assert year['member_cells']==year['text_cells']==2
    assert year['unresolved_identity_cells']==0 and year['linked_10k_filers']==2
