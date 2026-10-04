"""Fixed native family admission for R26; never certifies actual source economics.

All specs and review evidence are independent expectations. Native source readers
remain the admission authority; this helper only binds their complete dependencies
and preserves their product semantics for catalog/golden consumers.
"""
from __future__ import annotations

import contextlib
import dataclasses
import datetime as dt
import importlib
import json
import shutil
from collections.abc import Mapping
from pathlib import Path

from . import common as C, privatefund_runtime_resources as R

VERSION = 'tier1-family-sources-v1'
NAMES = ('text_features', 'classification_tnic', 'delisting_v2', 'distributions', 'events_buyback')
MAX_BLOB = 8 * 1024 * 1024


def digest(value):
    import hashlib
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'),
                                     default=str, allow_nan=False).encode()).hexdigest()


def pin(path):
    path = Path(path).resolve(strict=True)
    before = path.stat()
    result = {'sha256': C.sha256_file(path), 'bytes': before.st_size}
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError('family dependency changed while hashing')
    return result


def absolute(value):
    path = Path(value)
    if not path.is_absolute():
        raise ValueError('explicit absolute native path required')
    return path.resolve(strict=True)


def _blob(path, maximum=MAX_BLOB):
    path = absolute(path)
    if path.stat().st_size > maximum:
        raise ValueError('native original exceeds fixed bounded input limit')
    with path.open('rb') as stream:
        result = stream.read(maximum + 1)
    if len(result) > maximum:
        raise ValueError('native original grew beyond bounded input limit')
    return result


class _Blobs(Mapping):
    """Load one independently pinned original at a time, never all bodies."""
    def __init__(self, paths):
        self.paths = {k: absolute(v) for k, v in paths.items()}

    def __iter__(self):
        return iter(self.paths)

    def __len__(self):
        return len(self.paths)

    def __getitem__(self, key):
        return _blob(self.paths[key])


class _Deferred(Mapping):
    def __init__(self, records, convert):
        self.records, self.convert = records, convert

    def __iter__(self):
        return iter(self.records)

    def __len__(self):
        return len(self.records)

    def __getitem__(self, key):
        return self.convert(self.records[key])


def _typed(kind, record):
    if set(record) != {f.name for f in dataclasses.fields(kind)}:
        raise ValueError('complete exact native typed pin required')
    values = dict(record)
    for key in ('available_at', 'fetched_at'):
        if key in values:
            values[key] = dt.datetime.fromisoformat(values[key].replace('Z', '+00:00'))
            if values[key].tzinfo is None:
                raise ValueError('native source pin requires explicit UTC offset')
    for key in ('session_date', 'valid_from', 'valid_to'):
        if key in values:
            values[key] = dt.date.fromisoformat(values[key])
    return kind(**values)


def _code(name):
    modules = [C, R, importlib.import_module(__name__)]
    runtime = {}
    if name in ('text_features', 'classification_tnic'):
        for module in ('filing_text', 'filing_text_native', 'text_features',
                       'text_features_native', 'tnic', 'tnic_native'):
            modules.append(importlib.import_module('.' + module, __package__))
        from .filing_text_native import runtime_identity
        runtime['runtime:family-text'] = {'sha256': digest(runtime_identity()), 'bytes': None}
    elif name == 'delisting_v2':
        from . import mna_publish_v2 as P
        for module, value in P.publisher_code_pins().items():
            if isinstance(value, dict):
                runtime['runtime:family-terminal-' + module] = {'sha256': digest(value), 'bytes': None}
            else:
                modules.append(importlib.import_module('.' + module, __package__))
    else:
        from . import distributions_native as N
        for key, sha in N.code_pins().items():
            if key.startswith('runtime:'):
                runtime[key] = {'sha256': sha, 'bytes': None}
            else:
                actual = pin(key)
                if actual['sha256'] != sha:
                    raise ValueError('native physical code digest differs')
                runtime[str(absolute(key))] = actual
    return {**{str(absolute(m.__file__)): pin(m.__file__) for m in modules}, **runtime}


def _check(pins, name):
    runtime = {k: v for k, v in _code(name).items() if k.startswith('runtime:')}
    for key, expected in pins.items():
        actual = runtime.get(key) if key.startswith('runtime:') else pin(absolute(key))
        if actual != expected:
            raise ValueError('independently pinned family dependency changed: ' + key)


def _guard(con, root):
    R.guard_identity()
    if shutil.disk_usage(root.anchor).free < 40 * 1024 ** 3:
        raise ValueError('family reader requires40GiB free disk')
    threads, timezone, limit, current = con.execute("SELECT current_setting('threads'),current_setting('TimeZone'),current_setting('memory_limit'),current_database()").fetchone()
    quantity, unit = limit.split()
    scales = {'B': 1, 'KB': 1000, 'MB': 1000000, 'GB': 1000000000,
              'KiB': 1024, 'MiB': 1048576, 'GiB': 1073741824}
    if threads != 1 or timezone != 'UTC' or unit not in scales or float(quantity) * scales[unit] > 128000000:
        raise ValueError('family consumer requires128MB/one thread/UTC')
    if not any(row[1] == current and row[2] for row in con.execute('PRAGMA database_list').fetchall()):
        raise ValueError('family consumer primary database must be file-backed')


def binding(spec):
    """Canonical identities which independently reviewed evidence must bind."""
    return {key: spec[key] for key in ('interface_version', 'root', 'generation', 'manifest_pin', 'native')}


def _review(name, spec, code):
    record = spec['independent_review']
    if set(record) != {'path', 'expected_sha256'}:
        raise ValueError('independently pinned scoped review evidence required')
    path = absolute(record['path'])
    actual = pin(path)
    if actual['sha256'] != record['expected_sha256']:
        raise ValueError('review evidence identity differs')
    review = json.loads(_blob(path))
    expected = {'schema': 'atx.tier1-family-review/v1', 'status': 'scoped_pass',
                'family': name, 'interface_version': VERSION,
                'binding_sha256': digest(binding(spec)), 'code_pins': code}
    if review != expected:
        raise ValueError('native family independent review prerequisite pending or stale')
    return {str(path): actual}


def _products(generation, names):
    result = {name: generation / name for name in names}
    if any(not path.resolve(strict=True).is_relative_to(generation) for path in result.values()):
        raise ValueError('native product escapes immutable generation')
    return result, {str(path): pin(path) for path in result.values()}


def _task13(name, spec, generation):
    from . import distributions as D, distributions_native as N, events_buyback as B
    native = spec['native']
    if set(native) != {'admission_path', 'expected_admission_sha256'}:
        raise ValueError('fixed full Task13 source admission required')
    admission = absolute(native['admission_path'])
    if name == 'distributions':
        _, native_pins, admitted = D.replay_rows(admission, native['expected_admission_sha256'])
        products, dependencies = _products(generation, ('distributions.parquet',))
        validation = D.validate_product(products['distributions.parquet'], admission, native['expected_admission_sha256'])
        keys = {'distributions.parquet': ['event_id']}
    else:
        _, native_pins, admitted, _ = B.native_rows(admission, native['expected_admission_sha256'])
        products, dependencies = _products(generation, ('buyback.parquet',))
        validation = B.validate_product(products['buyback.parquet'], admission, native['expected_admission_sha256'])
        keys = {'buyback.parquet': ['accession', 'source_sha256', 'event_type', 'evidence']}
    # Every body/receipt/ancillary original encountered by unchanged native replay
    # is included, as are loaded runtime identities and the immutable product.
    N.recheck(native_pins)
    for path, sha in native_pins.items():
        actual = {'sha256': sha, 'bytes': None} if path.startswith('runtime:') else pin(path)
        if actual['sha256'] != sha:
            raise ValueError('Task13 replay dependency changed')
        dependencies[path if path.startswith('runtime:') else str(absolute(path))] = actual
    manifest = json.loads(_blob(generation / 'manifest.json'))
    product = next(iter(products))
    if manifest.get('schema') != (D.SCHEMA if name == 'distributions' else B.SCHEMA):
        raise ValueError('fixed native Task13 manifest schema required')
    if name == 'distributions':
        if (set(manifest.get('files', {})) != {product} or manifest['files'][product] != validation['sha256']
                or manifest.get('admission_sha256') != native['expected_admission_sha256']
                or manifest.get('population') != admitted['population']
                or manifest.get('native_inputs_sha256') != native_pins or manifest.get('admission') != validation):
            raise ValueError('immutable distribution manifest/native replay differ')
    else:
        # Original publication includes its parsed native rows in addition to
        # source pins. Bind that exact native path and receipt, never an arbitrary
        # extra caller file or a changed current parse substituted for old bytes.
        parse_path = C.build_root() / '_tmp' / 'events' / 'buyback_rows.parquet'
        receipt_path = parse_path.with_name('buyback_parse_receipt.json')
        receipt = json.loads(_blob(receipt_path))
        expected_inputs = {**native_pins, str(parse_path): pin(parse_path)['sha256']}
        if (manifest.get('file_sha256') != validation['sha256'] or manifest.get('admission') != validation
                or manifest.get('input_manifests_sha256') != expected_inputs
                or manifest.get('parse') != receipt
                or receipt.get('parse_rows_sha256') != pin(parse_path)['sha256']
                or receipt.get('admission_sha256') != native['expected_admission_sha256']
                or receipt.get('native_input_code_pins') != native_pins
                or receipt.get('population') != admitted['population']):
            raise ValueError('immutable buyback manifest/native parse replay differ')
        import pyarrow.parquet as pq
        rows, _, _, diagnostics = B.native_rows(admission, native['expected_admission_sha256'])
        if digest(pq.read_table(parse_path).to_pylist()) != digest(rows) or receipt.get('diagnostics') != diagnostics:
            raise ValueError('native buyback parsed originals differ')
        dependencies.update({str(absolute(path)): pin(path) for path in (parse_path, receipt_path)})
    # Traverse all admitted ancillary originals, including records whose current
    # declaration yields no supported event. The producer's native manifest pins
    # stay unchanged; these additional pins protect the complete consumption.
    from . import distributions_ancillary as A
    extra = {}
    for role in ('class', 'clock'):
        _blob(admitted['inputs'][role]['path'])
        for record in A.records(admitted, role).values():
            documents = ('clock_document',) if role == 'clock' else ('class_document', 'class_clock_document')
            for field in documents:
                N.native_document(record[field], extra)
    N.recheck(extra)
    dependencies.update({str(absolute(path)): pin(path) for path in extra})
    return validation, products, dependencies, keys


def _terminal(spec, generation):
    from . import mna_publish_v2 as P, mna_native_v2 as N, mna_price_native_v2 as Q
    native = spec['native']
    required = {'contract_path', 'expected_contract_sha256', 'candidate_original',
                'objects', 'input_manifests', 'receipts', 'exact_inputs', 'bulk_clocks',
                'clock_receipts', 'handcheck_original'}
    if set(native) != required:
        raise ValueError('complete fixed native terminal input map required')
    contract = _blob(native['contract_path'])
    admitted = P._contract(contract, native['expected_contract_sha256'])
    paths = [native['contract_path'], native['candidate_original']]
    for group in ('objects', 'input_manifests', 'receipts', 'clock_receipts'):
        paths.extend(native[group].values())
    for key, item in native['exact_inputs'].items():
        fields = {'vendor', 'target_pin', 'target_economic', 'target_economic_pin'}
        if set(item) not in (fields, fields | {'acquirer_pin', 'acquirer_economic', 'acquirer_economic_pin'}):
            raise ValueError('fixed original terminal price/economic fields required')
        paths.extend([item['vendor'], item['target_economic']])
        if 'acquirer_pin' in item:
            paths.append(item['acquirer_economic'])
    def exact_value(item):
        value = {'vendor': absolute(item['vendor']), 'target_pin': _typed(Q.VendorPricePin, item['target_pin']),
                 'target_economic': _blob(item['target_economic']), 'target_economic_pin': _typed(Q.EconomicPin, item['target_economic_pin'])}
        if 'acquirer_pin' in item:
            value.update(acquirer_pin=_typed(Q.VendorPricePin, item['acquirer_pin']),
                         acquirer_economic=_blob(item['acquirer_economic']),
                         acquirer_economic_pin=_typed(Q.EconomicPin, item['acquirer_economic_pin']))
        return value
    exact = _Deferred(native['exact_inputs'], exact_value)
    for key, item in native['bulk_clocks'].items():
        if set(item) != {'bulk', 'bulk_pin', 'submission', 'submission_pin'}:
            raise ValueError('complete original auxiliary clock map required')
        paths.extend([item['bulk'], item['submission']])
    def clock_value(item):
        return (_blob(item['bulk']), _typed(N.BulkClockPin, item['bulk_pin']),
                (_blob(item['submission']), _typed(N.NativePin, item['submission_pin'])))
    clocks = _Deferred(native['bulk_clocks'], clock_value)
    if native['handcheck_original'] is not None:
        paths.append(native['handcheck_original'])
    dependencies = {str(absolute(path)): pin(path) for path in paths}
    products, product_pins = _products(generation, ('mna.parquet', 'events.parquet'))
    dependencies.update(product_pins)
    P.validate_publication(generation, contract, native['expected_contract_sha256'], _Blobs(native['objects']),
                           tuple(_typed(N.NativePin, row) for row in admitted['source_pins']),
                           candidate_original=_blob(native['candidate_original'], 32 * 1024 * 1024),
                           input_manifests=_Blobs(native['input_manifests']), receipts=_Blobs(native['receipts']),
                           exact_inputs=exact, bulk_clocks=clocks, clock_receipts=_Blobs(native['clock_receipts']),
                           handcheck_original=None if native['handcheck_original'] is None else _blob(native['handcheck_original']))
    manifest = json.loads(_blob(generation / 'manifest.json'))
    validation = {'schema': P.SCHEMA, 'native_replay': 'PASS', 'rows': manifest['rows'],
                  'gate_status': manifest['gate_status'], 'return_computation': manifest['return_computation']}
    keys = {'mna.parquet': ['accession', 'source_id', 'available_at'],
            'events.parquet': ['security_id', 'last_session', 'candidate_available_at']}
    return validation, products, dependencies, keys


def _text(spec, generation):
    from . import text_features_native as T
    native = spec['native']
    if set(native) != {'source_root', 'expected_plan_sha256'}:
        raise ValueError('fixed reviewed full native text plan required')
    root = absolute(native['source_root'])
    validation = T.read_native_generation(root, generation, spec['manifest_pin']['sha256'], native['expected_plan_sha256'])
    names = ('text/features.parquet', 'classification_tnic/pairs.parquet',
             'classification_tnic/firms.parquet', 'classification_tnic/years.parquet')
    if set(validation['products']) != set(names):
        raise ValueError('complete fixed native text/TNIC products required')
    products, dependencies = _products(generation, names)
    for name, expected in validation['dependencies'].items():
        path = (root / name).resolve(strict=True)
        if not path.is_relative_to(root) or pin(path) != expected:
            raise ValueError('native text dependency escape/change')
        dependencies[str(path)] = expected
    for path, expected in validation['code'].items():
        if pin(absolute(path)) != expected:
            raise ValueError('native text code changed')
        dependencies[str(absolute(path))] = expected
    # Full reader validates units and metadata; retain their actual identities
    # across every later bounded read, not just the four product paths.
    manifest = json.loads(_blob(generation / 'manifest.json'))
    for name, expected in (manifest['units'] | manifest['metadata_files']).items():
        path = (generation / name).resolve(strict=True)
        if not path.is_relative_to(generation) or pin(path) != expected:
            raise ValueError('native immutable text metadata changed')
        dependencies[str(path)] = expected
    keys = {names[0]: ['cik', 'accession'], names[1]: ['year', 'cik', 'peer_cik'],
            names[2]: ['year', 'cik'], names[3]: ['year']}
    return validation, products, dependencies, keys


def admit(name, spec, con):
    """Full fixed native public replay; failed science/source gates stay visible."""
    if name not in NAMES or spec.get('interface_version') != VERSION:
        raise ValueError('unsupported fixed native family/version')
    root, generation = absolute(spec['root']), absolute(spec['generation'])
    if not generation.is_relative_to(root) or any(generation.glob('*.partial')):
        raise ValueError('immutable complete family generation required')
    _guard(con, root)
    _check(spec['dependency_pins'], name)
    code = _code(name)
    dependencies = {**code, **_review(name, spec, code)}
    manifest = generation / 'manifest.json'
    if pin(manifest) != spec['manifest_pin']:
        raise ValueError('independent immutable family manifest differs')
    dependencies[str(manifest)] = spec['manifest_pin']
    operation = _text if name in ('text_features', 'classification_tnic') else _terminal if name == 'delisting_v2' else None
    validation, products, native_pins, keys = operation(spec, generation) if operation else _task13(name, spec, generation)
    dependencies.update(native_pins)
    if dependencies != spec['dependency_pins']:
        raise ValueError('independently expected complete native dependency inventory differs')
    if digest(validation) != spec['expected_validation_sha256']:
        raise ValueError('independently expected native validation differs')
    _check(dependencies, name)
    if _code(name) != code:
        raise ValueError('native family code changed during admission')
    import pyarrow.parquet as pq
    schemas = {key: {f.name: str(f.type) for f in pq.read_schema(path)} for key, path in products.items()}
    _check(dependencies, name)
    views = {key: {'view': ('text_features' if key == 'text/features.parquet' else
                           'classification_tnic_' + Path(key).stem if key.startswith('classification_tnic/') else
                           'delisting_v2' if key == 'events.parquet' else 'mna_v2' if key == 'mna.parquet' else name),
                   'keys': keys[key], 'clock': 'available_at', 'schema': schemas[key],
                   'qualification': ('dlret_exact; dlret_policy; reason' if key == 'events.parquet' else
                       'peer_basis=reproduced_tnic3_df25_sic3; calibration_reason/reason; native full census' if key.startswith('classification_tnic/') else
                       'section-specific native reason/comparison_reason/edit_reason and acceptance_clock' if key == 'text/features.parquet' else
                       'original native source/class/clock/currency qualification; retain diagnostic exclusions'),
                   'clock_rule': 'native available_at; strictly before formation cutoff; pre2023 evaluation only'} for key in products}
    return {'status': 'admitted_diagnostic', 'products': products, 'validation': validation,
            'dependency_pins': dependencies, 'source_quality_certified': False,
            'schemas': schemas, 'views': views, 'spec': spec, 'family': name}


def iter_rows(admission, product, con, batch_size=32):
    """Native bounded products with full dependency pins around consumption."""
    if product not in admission['products'] or not 1 <= batch_size <= 64:
        raise ValueError('unsupported native product/batch size')
    name, spec = admission['family'], admission['spec']
    _guard(con, absolute(spec['root']))
    _recheck_admission(admission)
    try:
        if name in ('text_features', 'classification_tnic'):
            from . import text_features_native as T
            yield from T.iter_native_rows(admission['validation'], product, batch_size=batch_size)
        else:
            import pyarrow.parquet as pq
            for batch in pq.ParquetFile(admission['products'][product]).iter_batches(batch_size=batch_size):
                yield from batch.to_pylist()
    finally:
        _recheck_admission(admission)
        _guard(con, absolute(spec['root']))


@contextlib.contextmanager
def consuming(admission, con):
    """Guard a catalog SQL read/COPY with every admitted actual dependency."""
    name, spec = admission['family'], admission['spec']
    _guard(con, absolute(spec['root']))
    _recheck_admission(admission)
    try:
        yield admission['products']
    finally:
        _recheck_admission(admission)
        _guard(con, absolute(spec['root']))


def _recheck_admission(admission):
    """Do not allow mutable result dictionaries to redirect native consumption."""
    name, spec = admission['family'], admission['spec']
    if name not in NAMES or spec['interface_version'] != VERSION:
        raise ValueError('unsupported fixed native consumption')
    names = (('text/features.parquet', 'classification_tnic/pairs.parquet',
              'classification_tnic/firms.parquet', 'classification_tnic/years.parquet')
             if name in ('text_features', 'classification_tnic') else
             ('mna.parquet', 'events.parquet') if name == 'delisting_v2' else
             ('distributions.parquet',) if name == 'distributions' else ('buyback.parquet',))
    generation = absolute(spec['generation'])
    expected = {key: generation/key for key in names}
    if (admission['products'] != expected or admission['dependency_pins'] != spec['dependency_pins']
            or digest(admission['validation']) != spec['expected_validation_sha256']
            or admission['source_quality_certified'] is not False
            or pin(generation/'manifest.json') != spec['manifest_pin']):
        raise ValueError('native consumption admission identity differs')
    _review(name, spec, _code(name))
    _check(admission['dependency_pins'], name)
