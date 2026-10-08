"""Checked isolated A0 controls; no production admission or automatic execution.

The controller must independently freeze all code, input and documentary pins.
Actual capsule creation/seeding/execution is a root-owned guarded job. This
module never downloads inputs or modifies the source package.
"""
from __future__ import annotations

import argparse
import contextlib
import copy
import hashlib
import json
import os
from pathlib import Path
import runpy
import stat
import sys

VERSION = 'identity-diagnostic-capsule-v1'
ARMS = {'A': (False, False, 'A'), 'B1': (False, True, 'A'),
        'B2': (True, False, 'B2'), 'B12': (True, True, 'B2')}
PREPARE = ('vendor', 'facts', 'reconstruct')
REHEARSAL = ('symbols', 'candidates', 'uncertainty', 'links', 'names', 'export')
STAGES = {'identity': ('atx_db.alpha_panel.identity_links', ('build',)),
          'backfill': ('atx_db.alpha_panel.identity_backfill', ()),
          'names': ('atx_db.alpha_panel.identity_names', ()),
          'combine': ('atx_db.alpha_panel.identity_backfill', ()),
          'identity_table': ('atx_db.alpha_panel.identity_table', ())}
FIXED_INPUTS = ('companyfacts', 'fsds', 'finra_si', 'tickerhistory')
SEED_ROLES = {'prices': 'prices', 'calendar': 'calendar.parquet',
              'short_interest': 'short_interest/si.parquet',
              'core': '_tmp/panel_core', 'member': '_tmp/panel_member'}
ROLE_GLOBS = {'companyfacts': 'batch-*.parquet', 'fsds': 'sub/*.parquet',
              'finra_si': 'raw/si_*.csv', 'prices': '*/*.parquet',
              'core': '*.parquet', 'member': '*.parquet'}


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def safe_path(value):
    supplied = Path(value)
    if '..' in supplied.parts:
        raise ValueError('capsule parent traversal components refused')
    path = supplied.absolute()
    for part in (path, *path.parents):
        if part.is_symlink() or part.is_junction():
            raise ValueError('capsule reparse paths refused')
    resolved = path.resolve(strict=False)
    for part in (resolved, *resolved.parents):
        if part.is_symlink() or part.is_junction():
            raise ValueError('capsule resolved reparse paths refused')
    return resolved


def below(root, value):
    root, path = safe_path(root), safe_path(value)
    if path == root or root not in path.parents:
        raise ValueError('capsule path escapes its isolated root')
    return path


def pin(value):
    path = safe_path(value)
    before = path.stat()
    if not stat.S_ISREG(before.st_mode):
        raise ValueError('capsule pins require regular files')
    h = hashlib.sha256()
    with path.open('rb') as stream:
        opened = os.fstat(stream.fileno())
        if (opened.st_dev, opened.st_ino, opened.st_size) != (before.st_dev, before.st_ino, before.st_size):
            raise ValueError('capsule input replaced before open')
        while block := stream.read(1 << 20):
            h.update(block)
        after_open = os.fstat(stream.fileno())
    after = path.stat()
    identity = lambda item: (item.st_dev, item.st_ino, item.st_size, item.st_mtime_ns)
    if identity(before) != identity(after_open) or identity(before) != identity(after):
        raise ValueError('capsule input changed while hashing')
    return {'sha256': h.hexdigest(), 'bytes': before.st_size}


def check(pins):
    for path, expected in pins.items():
        if set(expected) != {'sha256', 'bytes'} or pin(path) != expected:
            raise ValueError('capsule physical pin differs: ' + str(path))


@contextlib.contextmanager
def consuming(pins):
    frozen = copy.deepcopy(pins)
    check(frozen)
    try:
        yield
    finally:
        check(frozen)


def _read(path, expected):
    if pin(path) != expected or expected['bytes'] > 16 << 20:
        raise ValueError('capsule bounded control pin differs')
    value = json.loads(Path(path).read_text(encoding='utf-8'))
    if pin(path) != expected:
        raise ValueError('capsule control changed')
    return value


def _write_new(path, value):
    with safe_path(path).open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, sort_keys=True, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())


def _role_files(role, path):
    path = safe_path(path)
    if role not in ROLE_GLOBS:
        if not path.is_file():
            raise ValueError('required original single-file role absent: ' + role)
        yield str(path)
        return
    if not path.is_dir():
        raise ValueError('required original native glob root absent: ' + role)
    for item in path.glob(ROLE_GLOBS[role]):
        if not item.is_file():
            raise ValueError('native glob entry must be a regular file')
        yield str(safe_path(item))


def _check_role_population(role, root, expected):
    # Only the independently declared expected inventory is materialized.
    # Stream actual native glob matches; no whole directory/rglob mirror set.
    expected = set(expected)
    count = 0
    for path in _role_files(role, root):
        if path not in expected:
            raise ValueError('complete original source role inventory differs: ' + role)
        count += 1
    if not count or count != len(expected):
        raise ValueError('complete original source role inventory differs: ' + role)


def _check_roles(control):
    """No source role can be authenticated by an omitted caller subset."""
    expected = control['input_pins']
    roles = control['role_inventories']
    if set(roles) != set(FIXED_INPUTS) | set(SEED_ROLES):
        raise ValueError('complete fixed source and seed role inventories required')
    for role, entry in roles.items():
        root = (control['fixed_inputs'][role] if role in FIXED_INPUTS else
                str(Path(control['original_alpha_root']) / SEED_ROLES[role]))
        _check_role_population(role, root, entry)
        if not set(entry) <= set(expected):
            raise ValueError('complete original source role inventory differs: ' + role)


def _check_copy(manifest):
    package = Path(manifest['capsule_root']) / 'src' / 'atx_db'
    actual = {p.relative_to(package).as_posix() for p in package.rglob('*')
              if p.is_file()}
    if actual != set(manifest['copied_inventory']):
        raise ValueError('copied complete import/resource inventory differs')


def _check_writable_tree(path):
    """Refuse existing nested output aliases before native write entrypoints."""
    path = safe_path(path)
    if not path.exists():
        return
    stack = [os.scandir(path)]
    try:
        while stack:
            try:
                entry = next(stack[-1])
            except StopIteration:
                stack.pop().close()
                continue
            child = safe_path(entry.path)
            if child.is_dir():
                stack.append(os.scandir(child))
    finally:
        for iterator in stack:
            iterator.close()


def _check_originals(control):
    """Derive mandatory original source closure from the pinned native build."""
    pins = control['input_pins']
    build = _read(control['original_build'], pins[control['original_build']])['pin']
    receipt = _read(control['native_receipt'], pins[control['native_receipt']])['inputs']
    for name in ('ticker_history', 'companyfacts', 'submissions', 'company_tickers'):
        original, candidate = build['inputs'][name], receipt[name]
        for key in ('path', 'sha256', 'received_at', 'receipt_basis'):
            if original[key] != candidate[key]:
                raise ValueError('original receipt input identity differs: ' + name)
        if pins[str(safe_path(original['path']))]['sha256'] != original['sha256']:
            raise ValueError('complete original receipt source pin missing')
    submissions = Path(build['inputs']['submissions']['path'])
    source_sha = build['inputs']['submissions']['sha256']
    index = submissions.with_name(submissions.name + '.directory-v1-' + source_sha + '.sqlite3')
    index_receipt = index.with_suffix('.sqlite3.sha256')
    for path in (index, index_receipt):
        if str(safe_path(path)) not in pins:
            raise ValueError('independently pinned existing native submissions cache/receipt required')
    if pins[control['ops_manifest']]['sha256'] != build['ops_manifest_sha256']:
        raise ValueError('original OPS manifest identity differs')
    ops = _read(control['ops_manifest'], pins[control['ops_manifest']])
    for item in ops['exports']:
        if item.get('file') is not None:
            path = str(safe_path(Path(control['ops_manifest']).parent / item['file']))
            if pins.get(path) != {'sha256': item['sha256'], 'bytes': item['bytes']}:
                raise ValueError('complete original OPS export pin missing')
    original_review = build['document_review']
    if (str(safe_path(control['document_review'])) != original_review['path'] or
            pins[control['document_review']]['sha256'] != original_review['sha256']):
        raise ValueError('original reviewed documentary inventory differs')
    review = _read(control['document_review'], pins[control['document_review']])
    documents = {}
    for item in review['assertions']:
        path = str(safe_path(item['document_path']))
        expected = item['document_sha256']
        if path in documents and documents[path] != expected:
            raise ValueError('conflicting original document pins')
        documents[path] = expected
    if not documents or documents != original_review['documents']:
        raise ValueError('complete original reviewed document population differs')
    for path, expected in documents.items():
        if pins.get(path, {}).get('sha256') != expected:
            raise ValueError('required original reviewed document pin missing')


def _replace(text, old, new, label):
    if text.count(old) != 1:
        raise ValueError('exact native patch anchor differs: ' + label)
    return text.replace(old, new), {'label': label, 'old': old, 'new': new}


def patched_sources(sources):
    """Apply only S1-S4 controls to authenticated text, preserving native guards."""
    sources = dict(sources)
    patches = {}
    changes = {
        'identity_links.py': [
            ('tickers=ir.read_sec_ticker_snapshot(inputs.company_tickers.path),',
             'tickers=(() if _capsule_current()["s1"] else ir.read_sec_ticker_snapshot(inputs.company_tickers.path)),', 'S1-reconstruction-only'),
            ('    work.mkdir(parents=True, exist_ok=True)\n    manifest_path = work / "preparation.json"',
             '    pin["diagnostic_capsule"] = _capsule_current()["preparation_binding"]\n    work.mkdir(parents=True, exist_ok=True)\n    manifest_path = work / "preparation.json"', 'S1-preparation-pin')],
        'alpha_panel/identity_backfill.py': [
            ('    receipt["candidates"] = con.execute(',
             '    if _capsule_current()["s2"]:\n        con.execute("DELETE FROM cand")\n    receipt["candidates"] = con.execute(', 'S2-both-passes')],
        'alpha_panel/identity_links.py': [
            ('        if not receipt["validation"]["pass"]:',
             '        if not receipt["validation"]["pass"] and not _capsule_current()["s4"]:', 'S4-only-seal-refusal')],
        'alpha_panel/common.py': [
            ('PACKAGE_ROOT = Path(__file__).resolve().parents[3]  # .../atx-db',
             'PACKAGE_ROOT = Path(_capsule_current()["original_package_root"])', 'S3-fixed-original-source-root'),
            ('IDENTITY_R4_DIR = PACKAGE_ROOT / "data" / "research" / "identity_rehearsal" / "session8-phased-r4"',
             'IDENTITY_R4_DIR = Path(_capsule_current()["rehearsal_root"])\n'
             'COMPANYFACTS_DIR = Path(_capsule_current()["fixed_inputs"]["companyfacts"])\n'
             'FSDS_DIR = Path(_capsule_current()["fixed_inputs"]["fsds"])\n'
             'FINRA_SI_DIR = Path(_capsule_current()["fixed_inputs"]["finra_si"])\n'
             'TICKERHISTORY = Path(_capsule_current()["fixed_inputs"]["tickerhistory"])', 'S3-read-only-native-inputs')],
        '_submissions_archive.py': [
            ('    def _build_index(self) -> None:\n',
             '    def _build_index(self) -> None:\n'
             '        raise ValueError("diagnostic capsule requires existing valid native submissions cache; original writes refused")\n',
             'original-cache-write-refusal')]
    }
    for name, items in changes.items():
        text = sources[name].replace('\r\n', '\n')
        if name == '_submissions_archive.py' and text.count('_INDEX_VERSION = 1\n') != 1:
            raise ValueError('exact native submissions cache version differs')
        text, import_patch = _replace(text, 'from __future__ import annotations',
            'from __future__ import annotations\nfrom atx_db.alpha_panel.private_identity_capsule import current as _capsule_current', 'authenticated-control-import')
        records = [import_patch]
        for old, new, label in items:
            text, record = _replace(text, old, new, label)
            records.append(record)
        sources[name], patches[name] = text, records
    return sources, patches


def create_capsule(spec):
    """Exclusive checked copy. Complete inventory is independently supplied."""
    spec = copy.deepcopy(spec)
    if spec['version'] != VERSION:
        raise ValueError('fixed capsule version required')
    source = safe_path(spec['project_package'])
    destination = safe_path(spec['capsule_root'])
    original_package = source.parent.parent
    workspace = original_package.parent
    below(workspace, destination)
    for protected in (original_package, workspace / 'data'):
        if destination == protected or protected in destination.parents:
            raise ValueError('capsule cannot target original package/data publication roots')
    if source in destination.parents or destination in source.parents or destination == source:
        raise ValueError('source/capsule roots must be disjoint')
    inventory = spec['code_inventory']
    all_files = {path.relative_to(source).as_posix() for path in source.rglob('*')
                 if path.is_file() and '__pycache__' not in path.parts and path.suffix != '.pyc'}
    actual = {name for name in all_files if name.endswith('.py')}
    if actual != set(inventory):
        raise ValueError('complete project Python import inventory differs')
    if all_files - actual != set(spec['resources']) or set(inventory) & set(spec['resources']):
        raise ValueError('complete project resource inventory differs')
    original = {str(below(source, source / name)): value for name, value in inventory.items()}
    resources = {str(below(source, source / name)): value for name, value in spec['resources'].items()}
    with consuming({**original, **resources}):
        destination.mkdir(parents=False, exist_ok=False)
        package = destination / 'src' / 'atx_db'
        package.mkdir(parents=True)
        text = {}
        patch_names = ('identity_links.py', 'alpha_panel/identity_backfill.py',
                       'alpha_panel/identity_links.py', 'alpha_panel/common.py', '_submissions_archive.py')
        for name in patch_names:
            if inventory[name]['bytes'] > 16 << 20:
                raise ValueError('bounded capsule source file exceeded')
            text[name] = (source / name).read_text(encoding='utf-8')
        modified, patches = patched_sources(text)
        copied = {}
        code_metadata = {}
        for name in inventory:
            if inventory[name]['bytes'] > 16 << 20:
                raise ValueError('bounded capsule source file exceeded')
            target = below(package, package / name)
            target.parent.mkdir(parents=True, exist_ok=True)
            if name in modified:
                target.write_text(modified[name], encoding='utf-8', newline='\n')
            else:
                with (source / name).open('rb') as incoming, target.open('xb') as outgoing:
                    while block := incoming.read(1 << 20):
                        outgoing.write(block)
            copied[name] = pin(target)
            code_metadata[name] = {
                'original_sha256': inventory[name]['sha256'],
                'original_sha256_lf': hashlib.sha256((source / name).read_bytes().replace(b'\r\n', b'\n')).hexdigest(),
                'copied_sha256': copied[name]['sha256'],
                'copied_sha256_lf': hashlib.sha256(target.read_bytes().replace(b'\r\n', b'\n')).hexdigest()}
        for name in spec['resources']:
            target = below(package, package / name)
            target.parent.mkdir(parents=True, exist_ok=True)
            with (source / name).open('rb') as incoming, target.open('xb') as outgoing:
                while block := incoming.read(1 << 20):
                    outgoing.write(block)
            copied[name] = pin(target)
        manifest = {'version': VERSION, 'source_inventory': inventory,
                    'source_resources': spec['resources'], 'copied_inventory': copied,
                    'patches': patches, 'same_code_sha256': digest(copied),
                    'code_metadata': code_metadata, 'source_git_head': spec['source_git_head'],
                    'source_git_head_is_published_producer_attribution': False,
                    'original_package_root': str(source.parent.parent),
                    'capsule_root': str(destination), 'source_quality_certified': False}
    final_names = {path.relative_to(source).as_posix() for path in source.rglob('*')
                   if path.is_file() and '__pycache__' not in path.parts and path.suffix != '.pyc'}
    if final_names != all_files:
        raise ValueError('original complete code/resource inventory changed during copying')
    _check_copy(manifest)
    check({str(package / name): value for name, value in copied.items()})
    _write_new(destination / 'capsule.json', manifest)
    return {'manifest_path': str(destination / 'capsule.json'),
            'manifest_pin': pin(destination / 'capsule.json'), 'manifest': manifest}


def bind_arm(capsule, spec):
    spec = copy.deepcopy(spec)
    manifest = _read(capsule['manifest_path'], capsule['manifest_pin'])
    root = safe_path(manifest['capsule_root'])
    alpha = safe_path(spec['original_alpha_root'])
    if root == alpha or root in alpha.parents or alpha in root.parents:
        raise ValueError('capsule and original alpha roots must be disjoint')
    arm = spec['arm']
    s1, s2, group = ARMS[arm]
    if set(spec['fixed_inputs']) != set(FIXED_INPUTS):
        raise ValueError('all original fixed native inputs required')
    arm_root = below(root, root / 'arms' / arm)
    rehearsal = below(root, root / 'rehearsals' / group)
    work = below(root, root / 'work' / group)
    inputs = copy.deepcopy(spec['input_pins'])
    if not inputs or not spec['native_receipt'] or not spec['ops_manifest'] or not spec['document_review']:
        raise ValueError('complete original documentary/source bindings required')
    for path in (spec['original_build'], spec['native_receipt'], spec['ops_manifest'], spec['document_review']):
        if str(safe_path(path)) not in inputs:
            raise ValueError('original documentary pin missing')
    for value in spec['fixed_inputs'].values():
        original = safe_path(value)
        if root == original or root in original.parents or original in root.parents:
            raise ValueError('original fixed inputs cannot resolve under capsule')
    control = {'version': VERSION, 'arm': arm, 's1': s1, 's2': s2, 's3': True, 's4': True,
               'capsule_manifest_path': capsule['manifest_path'], 'capsule_manifest_pin': capsule['manifest_pin'],
               'same_code_sha256': manifest['same_code_sha256'],
               'original_package_root': manifest['original_package_root'],
               'arm_root': str(arm_root), 'work_root': str(work), 'rehearsal_root': str(rehearsal),
               'fixed_inputs': spec['fixed_inputs'], 'input_pins': inputs,
               'role_inventories': spec['role_inventories'],
               'original_alpha_root': spec['original_alpha_root'],
               'original_build': spec['original_build'],
               'native_receipt': spec['native_receipt'], 'ops_manifest': spec['ops_manifest'],
               'document_review': spec['document_review'],
               'preparation_binding': {'s1': s1, 'group': group,
                   'same_code_sha256': manifest['same_code_sha256'], 'input_digest': digest(inputs)},
               'source_quality_certified': False}
    common = _common_binding(control)
    common_path = root / 'common-inputs.json'
    if common_path.exists():
        if _read(common_path, pin(common_path)) != common:
            raise ValueError('wrong-arm/resume: capsule-wide original input binding differs across arms')
    else:
        _write_new(common_path, common)
    control['common_binding_path'] = str(common_path)
    control['common_binding_pin'] = pin(common_path)
    arm_root.mkdir(parents=True, exist_ok=True)
    path = arm_root / 'control.json'
    if path.exists():
        if _read(path, pin(path)) != control:
            raise ValueError('wrong-arm/resume configuration refused')
    else:
        _write_new(path, control)
    return {'control_path': str(path), 'control_pin': pin(path), 'control': control}


def _common_binding(control):
    return {'schema': 'identity-capsule-common-original-inputs/v1',
            **{name: copy.deepcopy(control[name]) for name in (
                'same_code_sha256', 'original_package_root', 'original_alpha_root',
                'fixed_inputs', 'input_pins', 'role_inventories', 'original_build',
                'native_receipt', 'ops_manifest', 'document_review')}}


def _check_common(control, root):
    path = safe_path(control['common_binding_path'])
    if path != safe_path(root / 'common-inputs.json'):
        raise ValueError('capsule-wide binding path differs')
    if _read(path, control['common_binding_pin']) != _common_binding(control):
        raise ValueError('capsule-wide original input binding differs across arms')


def seed_arm(arm):
    """Copy only fixed identity-independent inputs; no writable source alias."""
    control = _read(arm['control_path'], arm['control_pin'])
    manifest = _read(control['capsule_manifest_path'], control['capsule_manifest_pin'])
    _check_common(control, safe_path(manifest['capsule_root']))
    _check_roles(control)
    root = safe_path(control['arm_root'])
    seed_pins = {}
    protected = {**control['input_pins'], arm['control_path']: arm['control_pin'],
        control['capsule_manifest_path']: control['capsule_manifest_pin'],
        control['common_binding_path']: control['common_binding_pin']}
    with consuming(protected):
        for role in SEED_ROLES:
            for source in control['role_inventories'][role]:
                relative = Path(source).relative_to(safe_path(control['original_alpha_root']))
                target = below(root, root / relative)
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists():
                    if pin(target) != control['input_pins'][source]:
                        raise ValueError('isolated seed differs; refuse overwrite')
                else:
                    with Path(source).open('rb') as incoming, target.open('xb') as outgoing:
                        while block := incoming.read(1 << 20):
                            outgoing.write(block)
                seed_pins[str(target)] = control['input_pins'][source]
                check({str(target): seed_pins[str(target)]})
        _check_roles(control)
    record = {'control_pin': arm['control_pin'], 'files': seed_pins}
    path = root / 'seed.json'
    if path.exists():
        if _read(path, pin(path)) != record:
            raise ValueError('seed resume binding differs')
    else:
        _write_new(path, record)
    return {'path': str(path), 'pin': pin(path)}


def _check_seed(control, seed):
    root = safe_path(control['arm_root'])
    original = safe_path(control['original_alpha_root'])
    expected = {}
    for role, relative in SEED_ROLES.items():
        role_files = {}
        for source in control['role_inventories'][role]:
            target = str(below(root, root / Path(source).relative_to(original)))
            role_files[target] = control['input_pins'][source]
        _check_role_population(role, root / relative, role_files)
        expected.update(role_files)
    if seed['files'] != expected:
        raise ValueError('isolated seed physical inventory differs')


def current():
    path = os.environ.get('ATX_IDENTITY_CAPSULE_CONTROL')
    expected = os.environ.get('ATX_IDENTITY_CAPSULE_CONTROL_PIN')
    if not path or not expected:
        raise ValueError('authenticated diagnostic configuration required')
    return _read(path, json.loads(expected))


def phase_command(capsule, arm, phase, interpreter):
    """Return argv only; never launch a process or alter production env."""
    manifest = _read(capsule['manifest_path'], capsule['manifest_pin'])
    wrapper = Path(manifest['capsule_root']) / 'src' / 'atx_db' / 'alpha_panel' / 'private_identity_capsule.py'
    return [str(interpreter), '-I', str(wrapper), '--control', arm['control_path'],
            '--control-pin', json.dumps(arm['control_pin'], separators=(',', ':')), '--phase', phase]


def _rehearsal_pins(control):
    root = Path(control['rehearsal_root'])
    work = Path(control['work_root'])
    prepared_path = work / 'preparation.json'
    prepared = _read(prepared_path, pin(prepared_path))
    if prepared['pin'].get('diagnostic_capsule') != control['preparation_binding']:
        raise ValueError('shared rehearsal preparation switch/input binding differs')
    build_path = root / 'build.json'
    build = _read(build_path, pin(build_path))
    if build['pin']['preparation_manifest_sha256'] != pin(prepared_path)['sha256']:
        raise ValueError('shared rehearsal preparation manifest differs')
    export_path = root / 'phases' / 'export.json'
    export = _read(export_path, pin(export_path))
    if export['build_sha256'] != pin(build_path)['sha256']:
        raise ValueError('shared completed rehearsal build differs')
    out = {str(prepared_path): pin(prepared_path), str(build_path): pin(build_path),
           str(export_path): pin(export_path), str(root / 'manifest.json'): pin(root / 'manifest.json')}
    for item in export['files']:
        if not item['file'].startswith('phases/export-001/'):
            raise ValueError('shared rehearsal must use successful export attempt001')
        path = below(root, root / item['file'])
        expected = pin(path)
        if expected['sha256'] != item['sha256'] or expected['bytes'] != item['bytes']:
            raise ValueError('shared native export receipt/product differs')
        out[str(path)] = expected
    if not export['files']:
        raise ValueError('completed native rehearsal export cannot be empty')
    return out


def _run(control_path, control_pin, phase):
    if not os.environ.get('ATX_GUARD_JOB'):
        raise ValueError('external ATX_GUARD_JOB required for diagnostic phase entry')
    control = _read(control_path, control_pin)
    manifest = _read(control['capsule_manifest_path'], control['capsule_manifest_pin'])
    root = safe_path(manifest['capsule_root'])
    _check_common(control, root)
    _check_copy(manifest)
    _check_roles(control)
    _check_originals(control)
    package = root / 'src' / 'atx_db'
    code = {str(below(package, package / name)): value for name, value in manifest['copied_inventory'].items()}
    if digest(manifest['copied_inventory']) != control['same_code_sha256']:
        raise ValueError('same-code arm digest differs')
    paths = {str(safe_path(control_path)): control_pin,
             str(safe_path(control['capsule_manifest_path'])): control['capsule_manifest_pin'],
             str(safe_path(control['common_binding_path'])): control['common_binding_pin'],
             **code, **control['input_pins']}
    for field in ('arm_root', 'work_root', 'rehearsal_root'):
        below(root, control[field])
        _check_writable_tree(control[field])
    s1, s2, group = ARMS[control['arm']]
    if (control['s1'], control['s2'], control['s3'], control['s4']) != (s1, s2, True, True):
        raise ValueError('arm switches differ')
    if control['preparation_binding'] != {'s1': s1, 'group': group,
            'same_code_sha256': manifest['same_code_sha256'], 'input_digest': digest(control['input_pins'])}:
        raise ValueError('arm preparation provenance differs')
    expected_roots = {'arm_root': root / 'arms' / control['arm'],
                      'work_root': root / 'work' / group, 'rehearsal_root': root / 'rehearsals' / group}
    if any(safe_path(control[name]) != safe_path(path) for name, path in expected_roots.items()):
        raise ValueError('fixed isolated arm/rehearsal roots differ')
    if phase.startswith('prepare:'):
        step = phase.split(':', 1)[1]
        if step not in PREPARE or control['arm'] not in ('A', 'B2'):
            raise ValueError('fixed fresh preparation phases required')
        if step != 'vendor':
            prepared_path = Path(control['work_root']) / 'preparation.json'
            prepared_pin = pin(prepared_path)
            prepared = _read(prepared_path, prepared_pin)
            if (prepared['pin'].get('diagnostic_capsule') != control['preparation_binding'] or
                    PREPARE[PREPARE.index(step)-1] not in prepared['steps']):
                raise ValueError('original native preparation sequence/binding differs')
            # Native preparation updates this manifest itself; authenticate the
            # predecessor before calling it, rather than pinning an intended
            # output rewrite as an immutable input across that phase.
        module, args = 'atx_db.identity_links', ['prepare', '--work', control['work_root'],
                    '--receipt', control['native_receipt'], '--step', step]
    elif phase.startswith('rehearsal:'):
        step = phase.split(':', 1)[1]
        if step not in REHEARSAL or control['arm'] not in ('A', 'B2'):
            raise ValueError('fixed fresh rehearsal phases required')
        module, args = 'atx_db.identity_links', ['rehearsal', '--work', control['work_root'],
            '--ops-manifest', control['ops_manifest'], '--out', control['rehearsal_root'],
            '--phase', step, '--document-review', control['document_review']]
        if step == 'export' and (Path(control['rehearsal_root']) / 'phases' / 'export-001').exists():
            raise ValueError('export must be fresh attempt001; failed outputs cannot be reused')
    else:
        module, arguments = STAGES[phase]
        args = list(arguments)
        if (Path(control['arm_root']) / 'capsule_receipts' / (phase + '.json')).exists():
            raise ValueError('completed stage receipt retained; refuse implicit repeated stage')
        stage_order = tuple(STAGES)
        if stage_order.index(phase):
            previous = Path(control['arm_root']) / 'capsule_receipts' / (stage_order[stage_order.index(phase)-1] + '.json')
            if not previous.is_file():
                raise ValueError('original alpha stage sequence requires completed previous phase')
        paths.update(_rehearsal_pins(control))
        seed_path = Path(control['arm_root']) / 'seed.json'
        seed = _read(seed_path, pin(seed_path))
        if seed['control_pin'] != control_pin:
            raise ValueError('isolated seed control differs')
        _check_seed(control, seed)
        paths.update(seed['files'])
        if stage_order.index(phase):
            previous = Path(control['arm_root']) / 'capsule_receipts' / (stage_order[stage_order.index(phase)-1] + '.json')
            receipt = _read(previous, pin(previous))
            if receipt['control_pin'] != control_pin or receipt['status'] != 'completed':
                raise ValueError('original alpha stage order/prerequisite differs')
            check(receipt['outputs'])
            required_inputs = {'backfill': ('identity/links.parquet',),
                'names': ('_tmp/identity_days_strict_backfill.parquet',),
                'combine': ('identity/links.parquet', 'identity/links_name_days.parquet'),
                'identity_table': ('identity/links_combined.parquet',)}[phase]
            for relative in required_inputs:
                path = str(Path(control['arm_root']) / relative)
                if path not in receipt['outputs']:
                    raise ValueError('complete native preceding-stage input missing: ' + relative)
                paths[path] = receipt['outputs'][path]
            paths[str(previous)] = pin(previous)
    succeeded = False
    with consuming(paths):
        os.environ['ATX_IDENTITY_CAPSULE_CONTROL'] = str(control_path)
        os.environ['ATX_IDENTITY_CAPSULE_CONTROL_PIN'] = json.dumps(control_pin)
        os.environ['ATX_ALPHA_PANEL_ROOT'] = control['arm_root']
        temp = below(root, Path(control['arm_root']) / '_tmp' / 'capsule_os_temp')
        temp.mkdir(parents=True, exist_ok=True)
        for name in ('TMP', 'TEMP', 'TMPDIR', 'SQLITE_TMPDIR'):
            os.environ[name] = str(temp)
        os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
        sys.dont_write_bytecode = True
        os.chdir(control['arm_root'])
        sys.path.insert(0, str(root / 'src'))
        sys.argv = [module, *args]
        try:
            try:
                runpy.run_module(module, run_name='__main__')
            except SystemExit as exit_value:
                if exit_value.code not in (None, 0):
                    raise
            succeeded = True
        finally:
            try:
                for name, loaded in list(sys.modules.items()):
                    if name == 'atx_db' or name.startswith('atx_db.'):
                        filename = getattr(loaded, '__file__', None)
                        if filename is None or str(safe_path(filename)) not in code:
                            raise ValueError('project import escaped authenticated code copy: ' + name)
            finally:
                _check_copy(manifest)
                _check_roles(control)
                if phase in STAGES:
                    _check_seed(control, seed)
                for field in ('arm_root', 'work_root', 'rehearsal_root'):
                    _check_writable_tree(control[field])
    # No completed capsule receipt is minted until every final check succeeded.
    if succeeded and phase in STAGES:
        identity = Path(control['arm_root']) / 'identity'
        outputs = {str(safe_path(path)): pin(path) for path in identity.rglob('*') if path.is_file()}
        auxiliary = Path(control['arm_root']) / '_tmp' / 'identity_days_strict_backfill.parquet'
        if auxiliary.exists():
            outputs[str(auxiliary)] = pin(auxiliary)
        if phase == 'identity_table':
            export = Path(control['arm_root']) / 'export' / 'identity-bridge-v2-pit'
            outputs.update({str(safe_path(path)): pin(path) for path in export.rglob('*') if path.is_file()})
        directory = Path(control['arm_root']) / 'capsule_receipts'
        directory.mkdir(exist_ok=True)
        _write_new(directory / (phase + '.json'), {'status': 'completed', 'control_pin': control_pin,
            'phase': phase, 'same_code_sha256': control['same_code_sha256'], 'outputs': outputs,
            'source_quality_certified': False})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control', required=True)
    parser.add_argument('--control-pin', required=True)
    parser.add_argument('--phase', required=True)
    args = parser.parse_args()
    _run(args.control, json.loads(args.control_pin), args.phase)


if __name__ == '__main__':
    main()
