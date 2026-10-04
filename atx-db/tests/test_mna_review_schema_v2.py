import hashlib
import json
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import mna_publish_v2 as P
from tests.test_mna_publish_v2 import inputs


@pytest.mark.parametrize('empty', [False, True])
def test_i4_refreshed_hash_cannot_admit_wrong_physical_type(tmp_path, empty):
    contract,sha,objects,pins,original,manifests,receipts=inputs()
    if empty:
        value=json.loads(contract);value['source_pins']=[];value['source_receipts']={};value['candidate_source_links']={}
        contract=P._json(value);sha=hashlib.sha256(contract).hexdigest();objects={};pins=();receipts={}
    kw={'candidate_original':original,'input_manifests':manifests,'receipts':receipts}
    directory=P.publish_value_only(tmp_path,contract,sha,objects,pins,**kw)
    path=directory/'mna.parquet';table=pq.read_table(path)
    fields=[pa.field(field.name,pa.string() if field.name=='cash_per_share' else field.type) for field in table.schema]
    rows=table.to_pylist()
    for row in rows:
        if row['cash_per_share'] is not None: row['cash_per_share']=str(row['cash_per_share'].normalize())
    pq.write_table(pa.Table.from_pylist(rows,schema=pa.schema(fields)),path)
    manifest=json.loads((directory/'manifest.json').read_text());manifest['files'][path.name]=hashlib.sha256(path.read_bytes()).hexdigest()
    (directory/'manifest.json').write_text(json.dumps(manifest))
    with pytest.raises(ValueError,match='physical Arrow'):
        P.validate_publication(directory,contract,sha,objects,pins,**kw)
