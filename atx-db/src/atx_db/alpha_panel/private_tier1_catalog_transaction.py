"""Authenticated two-file catalog publication with retained previous bytes."""
from pathlib import Path
import os
import shutil
from . import common as C, private_tier1_admission as A, fund_batch_release as F

VERSION='tier1-catalog-publication-v1'


def identity(path):return A.pin(path) if path.exists() else None


def journal(destination):return destination.with_suffix('.publication.json')


def finish(destination,intent,expected_transition_sha256):
    destination=Path(destination).resolve();proof=destination.with_suffix('.proof.json')
    if A.digest(intent)!=expected_transition_sha256 or intent.get('version')!=VERSION or intent.get('destination')!=str(destination):
        raise ValueError('independent catalog transition identity differs')
    def check_intent():
        if C.read_json(journal(destination))!=intent:raise ValueError('catalog publication intent changed')
    check_intent()
    candidate=A.absolute(intent['candidate']);candidate_proof=candidate.with_suffix('.proof.json')
    if candidate.name!=destination.name or candidate.parent.parent!=destination.parent or not candidate.parent.name.startswith('catalog-attempt-'):
        raise ValueError('catalog transition candidate escapes owned attempt')
    if A.pin(candidate)!=intent['target']['catalog'] or A.pin(candidate_proof)!=intent['target']['proof']:
        raise ValueError('catalog transition candidate/proof changed')
    actual={'catalog':identity(destination),'proof':identity(proof)}
    old,new=intent['previous'],intent['target']
    # Only old pair, first completed replacement, or complete target pair is a
    # valid interrupted state. An unrelated C may never be overwritten.
    allowed=(old,{'catalog':new['catalog'],'proof':old['proof']},new)
    if actual not in allowed:raise ValueError('unrelated catalog/proof state; recovery refused')
    for path,source,key in ((destination,candidate,'catalog'),(proof,candidate_proof,'proof')):
        check_intent()
        if identity(path)==new[key]:continue
        pending=path.with_name(path.name+'.publication-partial')
        with source.open('rb') as src,pending.open('wb') as dst:
            shutil.copyfileobj(src,dst,1024**2);dst.flush();os.fsync(dst.fileno())
        if A.pin(pending)!=new[key] or A.pin(source)!=new[key]:raise ValueError('catalog transition bytes changed')
        os.replace(pending,path)
    if {'catalog':identity(destination),'proof':identity(proof)}!=new:raise ValueError('catalog transition incomplete')
    check_intent();journal(destination).unlink()


def publish(destination,candidate,source_snapshot_sha256,code_pins,expected_previous=None):
    with F.writer_lock(Path(destination).resolve().parent/'.catalog-publication'):
        return _publish(destination,candidate,source_snapshot_sha256,code_pins,expected_previous)


def _publish(destination,candidate,source_snapshot_sha256,code_pins,expected_previous):
    destination=Path(destination).resolve();candidate=Path(candidate).resolve()
    if journal(destination).exists():raise ValueError('unfinished catalog transaction; authenticated recovery required')
    previous={'catalog':identity(destination),'proof':identity(destination.with_suffix('.proof.json'))}
    if previous!={'catalog':None,'proof':None} and previous!=expected_previous:
        raise ValueError('independent previous catalog/proof identities required')
    old=candidate.parent/'previous';old.mkdir()
    for path,key in ((destination,'catalog'),(destination.with_suffix('.proof.json'),'proof')):
        if previous[key] is not None:
            shutil.copyfile(path,old/path.name)
            if A.pin(path)!=previous[key] or A.pin(old/path.name)!=previous[key]:raise ValueError('previous catalog changed while retaining rollback bytes')
    intent={'version':VERSION,'destination':str(destination),'candidate':str(candidate),
        'previous':previous,'target':{'catalog':A.pin(candidate),'proof':A.pin(candidate.with_suffix('.proof.json'))},
        'snapshot_sha256':source_snapshot_sha256,'code_pins':code_pins}
    C.write_json_atomic(journal(destination),intent)
    finish(destination,intent,A.digest(intent))
    return intent
