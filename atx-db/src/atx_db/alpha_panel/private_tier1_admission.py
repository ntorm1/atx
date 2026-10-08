"""Strict shared mechanics for Task17 native admissions; no warehouse IO on import."""
from __future__ import annotations
import contextlib
import datetime as dt
import importlib
from pathlib import Path
import tempfile

import pyarrow.parquet as pq

from . import common as C, fund_batch_release as F

VERSION = 'tier1-native-readers-v1'


def digest(value):
    import json
    import hashlib
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'), default=str).encode()).hexdigest()


def absolute(path):
    return F._absolute(path)


def member(root, relative):
    return F._member(Path(root), relative)


def pin(path):
    return F.identity(path)


def sql(paths):
    return F.sql(paths)


def code(names):
    return {str(Path(importlib.import_module('.'+name, __package__).__file__).resolve()):
            pin(importlib.import_module('.'+name, __package__).__file__) for name in names}


EXECUTION = {'version':VERSION,'duckdb_memory_ceiling_bytes':128000000,'threads':1,'timezone':'UTC',
    'file_backed':True,'process_guard_max_gib':1.0,'actual_initial_guard_gib':0.8,'disk_floor_gib':40,
    'financial_reader':'separately_pinned_original_R23_256MB',
    'quality':'NOT_PRODUCTION_READY','complete_native_protocol':True}
ADAPTER_CODE = ('tier1_native_readers','private_tier1_admission','private_tier1_accounting','private_tier1_earnings')


def execution(spec):
    """Independent contract/code pins precede any candidate source assertion."""
    path=absolute(spec['execution_contract'])
    if pin(path)['sha256']!=spec['expected_execution_contract_sha256'] or C.read_json(path)!=EXECUTION:
        raise ValueError('independent native integration execution contract differs')
    actual=code(ADAPTER_CODE)
    if digest(actual)!=spec['expected_adapter_code_sha256']:
        raise ValueError('independent actual native adapter code provenance differs')
    return {str(path):pin(path),**actual}


def check_pins(pins, required=None):
    if not isinstance(pins, dict) or not pins:
        raise ValueError('independent complete dependency pins required')
    if required is not None and any(pins.get(p) != v for p, v in required.items()):
        raise ValueError('missing/substituted native dependency pin')
    for path, expected in pins.items():
        path = absolute(path)
        if set(expected) != {'sha256', 'bytes'} or not F._sha(expected['sha256']) or type(expected['bytes']) is not int:
            raise ValueError('unsupported dependency identity')
        if pin(path) != expected:
            raise ValueError('native dependency bytes changed: '+str(path))


def root(spec):
    result = absolute(spec['lake_root'])
    # Some unchanged native gate/source selectors use their existing common root.
    # Require equality; never clone or substitute their globals to retarget them.
    if result != C.build_root().resolve():
        raise ValueError('explicit admitted lake root differs from native reader root')
    return result


def normalize(value):
    import json
    return json.loads(json.dumps(value, default=str))


def compare(con, published, replay, schema=True):
    if schema and pq.ParquetFile(published).schema_arrow != pq.ParquetFile(replay).schema_arrow:
        raise ValueError('native replay schema differs')
    left, right = sql([published]), sql([replay])
    n = con.execute(f'SELECT count(*) FROM ((SELECT * FROM {left} EXCEPT ALL SELECT * FROM {right}) UNION ALL (SELECT * FROM {right} EXCEPT ALL SELECT * FROM {left}))').fetchone()[0]
    if n:
        raise ValueError('native exact typed multiset replay differs')


@contextlib.contextmanager
def scratch(lake, prefix):
    directory = lake/'_tmp/tier1-native-readers'
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=prefix, dir=directory) as path:
        yield Path(path)


def files(root, manifest, exact=None):
    rows = manifest.get('files')
    if not isinstance(rows, dict) or not rows:
        raise ValueError('complete actual output inventory required')
    if exact is not None and set(rows) != set(exact):
        raise ValueError('unsupported native output inventory')
    result = {}
    for relative, expected in rows.items():
        path = member(root, relative)
        actual = pin(path)
        if actual != {k: expected.get(k) for k in ('sha256', 'bytes')}:
            raise ValueError('native output differs from manifest: '+relative)
        result[str(path)] = actual
    return result


def selected(lake, pattern, required=True):
    result = sorted(p.resolve() for p in lake.glob(pattern) if p.is_file())
    if required and not result:
        raise ValueError('missing native reader population: '+pattern)
    return {str(p): pin(p) for p in result}


def guard(con):
    from .privatefund_runtime_resources import guard_identity
    guard_identity()
    import shutil
    if shutil.disk_usage(C.build_root().anchor).free<40*1024**3:
        raise ValueError('native integration requires40GiB disk floor')
    settings=con.execute("SELECT current_setting('threads'),current_setting('TimeZone'),current_setting('memory_limit')").fetchone()
    if settings[0]!=1 or settings[1]!='UTC':
        raise ValueError('native integration requires one DuckDB thread and UTC')
    quantity,unit=settings[2].split()
    scale={'B':1,'KB':1000,'MB':1000000,'GB':1000000000,'KiB':1024,'MiB':1048576,'GiB':1073741824}.get(unit)
    if scale is None or float(quantity)*scale>128000000:
        raise ValueError('native integration DuckDB allowance exceeds explicit128MB ceiling')
    if not any(row[2] for row in con.execute('PRAGMA database_list').fetchall()):
        raise ValueError('native integration requires file-backed DuckDB spill')


def envelope(name, spec, generation, validation, products, dependencies):
    if spec.get('interface_version') != VERSION:
        raise ValueError('explicit reviewed native interface version required')
    check_pins(spec['dependency_pins'], dependencies)
    execution(spec)
    if pin(generation/'manifest.json') != spec['manifest_pin']:
        raise ValueError('independent native manifest pin differs')
    if digest(validation) != spec['expected_validation_sha256']:
        raise ValueError('independent full native validation digest differs')
    complete={**dependencies,**files(generation.parent.parent,C.read_json(generation/'manifest.json')),
              str(generation/'manifest.json'):spec['manifest_pin']}
    check_pins(complete)
    return {'status': 'admitted_diagnostic', 'products': products, 'validation': validation,
            'source_quality_certified': False, 'spec': spec, 'native_interface': VERSION,
            'dependency_pins':complete}
