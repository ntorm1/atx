"""Task6 immutable, bounded, resumable publication primitives."""
from __future__ import annotations
import contextlib
import datetime as dt
import hashlib
import json
import os
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
from . import common as C


@contextlib.contextmanager
def publisher_lock(destination: Path):
    destination.mkdir(parents=True, exist_ok=True)
    path = destination/'.publisher.lock'
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        raise RuntimeError('publisher lock exists; inspect owner before recovery') from None
    try:
        os.write(fd, json.dumps({'pid': os.getpid(), 'created_at': str(dt.datetime.now(dt.UTC))}).encode())
        os.close(fd)
        yield
    finally:
        path.unlink(missing_ok=True)


class Generation:
    """One immutable per-unit artifact; existing units reused only at matching hashes."""
    def __init__(self, destination: Path, stage: str, identity: dict, schema: pa.Schema):
        self.destination, self.stage, self.identity, self.schema = destination, stage, identity, schema
        self.token = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
        self.path = destination/('generation='+self.token)
        self.path.mkdir(parents=True, exist_ok=True)
        self.receipt = self.path/'progress.json'
        self.state = json.loads(self.receipt.read_text()) if self.receipt.exists() else {'identity': identity, 'units': {}}
        if self.state['identity'] != identity:
            raise ValueError('stale generation identity')

    def done(self, unit: str) -> bool:
        item = self.state['units'].get(unit)
        if item is None:
            return False
        p = self.path/item['path']
        if not p.exists() or p.stat().st_size != item['bytes'] or C.sha256_file(p) != item['sha256']:
            raise ValueError('completed unit has stale/partial output')
        return True

    def write(self, unit: str, rows) -> dict:
        if self.done(unit):
            return self.state['units'][unit]
        # Unit names are generated dates, never untrusted filesystem paths.
        if '/' in unit or '\\' in unit or '..' in unit:
            raise ValueError('invalid unit name')
        dest = self.path/(unit+'.parquet')
        partial = dest.with_suffix('.parquet.partial')
        count, batch = 0, []
        with pq.ParquetWriter(partial, self.schema, compression='zstd') as writer:
            for row in rows:
                batch.append(row)
                if len(batch) == 1024:
                    writer.write_table(pa.Table.from_pylist(batch, schema=self.schema), row_group_size=1024)
                    count += len(batch)
                    batch.clear()
            if batch:
                writer.write_table(pa.Table.from_pylist(batch, schema=self.schema), row_group_size=1024)
                count += len(batch)
        os.replace(partial, dest)
        item = {'path': dest.name, 'rows': count, 'bytes': dest.stat().st_size, 'sha256': C.sha256_file(dest)}
        self.state['units'][unit] = item
        C.write_json_atomic(self.receipt, self.state)
        return item

    def publish(self, required_units: list[str], payload: dict):
        if set(self.state['units']) != set(required_units) or not all(self.done(u) for u in required_units):
            raise ValueError('incomplete generation; required units absent')
        files = {v['path']: {k: v[k] for k in ('bytes', 'sha256')} for v in self.state['units'].values()}
        manifest = {'stage': self.stage, 'status': 'complete', 'schema': payload.get('schema_id',self.stage+'/v1'),
                    'arrow_schema':str(self.schema),
                    'input_identity': self.identity, 'rows': sum(v['rows'] for v in self.state['units'].values()),
                    'files': files, **payload}
        C.write_json_atomic(self.path/'manifest.json', manifest)
        pointer = {**manifest, 'generation': self.path.name,
                   'files': {self.path.name+'/'+k: v for k, v in files.items()}}
        C.write_json_atomic(self.destination/'manifest.json', pointer)
        return self.path/'manifest.json'
