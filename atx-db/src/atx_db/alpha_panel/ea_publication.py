"""Immutable earnings generations with atomic, phase-specific activation."""
from __future__ import annotations
import os
import uuid
from pathlib import Path
from typing import Any
from . import common as C


def create(stage: str) -> Path:
    path=C.stage_dir(stage)/'_pending'/uuid.uuid4().hex
    path.mkdir(parents=True)
    return path


def file(stage: str,name: str,manifest_name: str='manifest.json') -> Path:
    manifest=C.read_json(C.build_root()/stage/manifest_name)
    if manifest.get('generation') and name not in manifest.get('products',{}):
        raise RuntimeError('product unavailable in active publication phase')
    return C.build_root()/stage/manifest.get('products',{}).get(name,name)


def publish(stage: str,work: Path,phase: str,payload: dict[str,Any],outputs: tuple[str,...]) -> dict[str,Any]:
    root=C.stage_dir(stage)
    relative=Path('generations')/work.name
    files={str(relative/name).replace('\\','/'):{'bytes':(work/name).stat().st_size,'sha256':C.sha256_file(work/name)} for name in outputs}
    manifest={**payload,'status':'complete','phase':phase,'generation':relative.as_posix(),
              'files':files,'products':{name:(relative/name).as_posix() for name in outputs}}
    C.write_json_atomic(work/'manifest.json',manifest)
    target=root/relative
    target.parent.mkdir(parents=True,exist_ok=True)
    os.rename(work,target)  # a unique generation can never replace an earlier one
    C.write_json_atomic(root/'manifest.json',manifest)
    if phase=='parsed':
        C.write_json_atomic(root/'releases_manifest.json',manifest)
    return manifest


def activate(stage: str,generation: str) -> None:
    """Explicit rollback restores a complete generation and its pinned parse alias."""
    root=C.build_root()/stage
    path=(root/generation).resolve()
    if path.parent != (root/'generations').resolve():
        raise ValueError('rollback target must be an owned immutable generation')
    manifest=C.read_json(path/'manifest.json')
    if manifest.get('status')!='complete': raise RuntimeError('incomplete rollback generation')
    for name,record in manifest['files'].items():
        output=root/name
        if output.stat().st_size!=record['bytes'] or C.sha256_file(output)!=record['sha256']:
            raise RuntimeError('rollback generation output mismatch')
    if manifest.get('parse_generation'):
        source=C.read_json(root/manifest['parse_generation']/'manifest.json')
        if C.sha256_file(root/manifest['parse_generation']/'manifest.json')!=manifest['parse_lineage']['sha256']:
            raise RuntimeError('rollback parse lineage mismatch')
        for name,record in source['files'].items():
            if (root/name).stat().st_size!=record['bytes'] or C.sha256_file(root/name)!=record['sha256']:
                raise RuntimeError('rollback parse output mismatch')
        C.write_json_atomic(root/'releases_manifest.json',source)
    elif manifest['phase']=='parsed':
        C.write_json_atomic(root/'releases_manifest.json',manifest)
    C.write_json_atomic(root/'manifest.json',manifest)
