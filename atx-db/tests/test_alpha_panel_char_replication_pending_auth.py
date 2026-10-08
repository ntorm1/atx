"""Pending-intent rollback authentication, in-memory metadata only; no DuckDB."""
import hashlib
import json
import pytest
from atx_db.alpha_panel import char_replication as R


def pending(root):
    previous=R.generation_path(root,'previous');target=R.generation_path(root,'candidate')
    report={'source_quality_certified':False,'readiness':'NOT_PRODUCTION_READY','gate_status':{'source_admission':'FAIL'}}
    R.immutable(previous/'char_replication.json',report)
    R.immutable(previous/'manifest.json',{'report':R.R.identity(previous/'char_replication.json'),'source_quality_certified':False,'readiness':'NOT_PRODUCTION_READY'})
    oldpin=R.R.identity(previous/'manifest.json');old={'current':{'generation':'previous','manifest':oldpin},'validation':{'generation':'previous','manifest':oldpin,'report':report}}
    R.immutable(target/'manifest.json',{'unqualified_test_target':True});pin=R.R.identity(target/'manifest.json')
    desired={'current':{'generation':'candidate','manifest':pin},'validation':{'generation':'candidate','manifest':pin,'report':report}}
    prior={}
    for key,value in old.items():
        payload=R.json_bytes(value);prior[key]={'text':payload.decode(),'identity':{'sha256':hashlib.sha256(payload).hexdigest(),'bytes':len(payload)}}
        R.atomic_pointer(R.pointer_paths(root)[key],payload)
    intent={'version':R.VERSION,'generation':'candidate','expected_bindings_sha256':'0'*64,'manifest':pin,'desired':desired,'prior':prior}
    file=root/R.STAGE/'publications/test/intent.json';R.immutable(file,intent)
    marker=root/R.STAGE/'publication.pending.json';R.C.write_json_atomic(marker,{'intent':'publications/test/intent.json','identity':R.R.identity(file)})
    # Uncatchable interruption leaves only the first proposed pointer written.
    R.atomic_pointer(R.pointer_paths(root)['current'],R.json_bytes(desired['current']))
    return file,marker,intent,{key:R.json_bytes(value) for key,value in old.items()}


def test_corrupt_target_manifest_restores_exact_prior_before_reject(tmp_path,monkeypatch):
    monkeypatch.setenv('ATX_GUARD_JOB','synthetic-only');file,marker,intent,old=pending(tmp_path)
    target=R.generation_path(tmp_path,'candidate')/'manifest.json';target.write_bytes(target.read_bytes()+b' ')
    def forbidden(*args):raise AssertionError('target corruption must reject before native admission')
    monkeypatch.setattr(R,'freeze',forbidden)
    with pytest.raises(ValueError,match='target manifest changed'):
        R.build(tmp_path,tmp_path/'unused.json','candidate',1,'0'*64)
    assert old=={key:path.read_bytes() for key,path in R.pointer_paths(tmp_path).items()}
    assert marker.exists() and file.exists() and target.read_bytes().endswith(b' ')
    assert not (tmp_path/R.STAGE/'.writer.lock').exists()


@pytest.mark.parametrize('damage',['journal','prior_bytes','prior_rehashed','prior_immutable','unrelated_pointer','desired_rehashed'])
def test_unrelated_journal_prior_and_pointer_changes_never_rollback(tmp_path,monkeypatch,damage):
    monkeypatch.setenv('ATX_GUARD_JOB','synthetic-only');file,marker,intent,old=pending(tmp_path)
    if damage=='journal':file.write_bytes(file.read_bytes()+b' ')
    elif damage in ('prior_bytes','prior_rehashed'):
        intent['prior']['current']['text']+=' '
        if damage=='prior_rehashed':
            payload=intent['prior']['current']['text'].encode();intent['prior']['current']['identity']={'sha256':hashlib.sha256(payload).hexdigest(),'bytes':len(payload)}
        file.write_bytes(R.json_bytes(intent))
        R.C.write_json_atomic(marker,{'intent':'publications/test/intent.json','identity':R.R.identity(file)})
    elif damage=='prior_immutable':
        previous=R.generation_path(tmp_path,'previous')/'manifest.json';previous.write_bytes(previous.read_bytes()+b' ')
    elif damage=='desired_rehashed':
        intent['desired']['validation']['generation']='unrelated'
        file.write_bytes(R.json_bytes(intent));R.C.write_json_atomic(marker,{'intent':'publications/test/intent.json','identity':R.R.identity(file)})
        R.atomic_pointer(R.pointer_paths(tmp_path)['validation'],R.json_bytes(intent['desired']['validation']))
    else:R.atomic_pointer(R.pointer_paths(tmp_path)['validation'],R.json_bytes({'unrelated':'do not overwrite'}))
    before={key:path.read_bytes() for key,path in R.pointer_paths(tmp_path).items()}
    with pytest.raises(ValueError):R.build(tmp_path,tmp_path/'unused.json','candidate',1,'0'*64)
    assert before=={key:path.read_bytes() for key,path in R.pointer_paths(tmp_path).items()}
    assert marker.exists() and file.exists()
