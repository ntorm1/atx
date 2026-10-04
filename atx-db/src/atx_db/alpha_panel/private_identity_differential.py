"""Bounded A0 D0 comparisons. Diagnostic evidence only; never source admission."""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
from pathlib import Path
import re
import stat
from typing import Iterator
import uuid

R4_KEYS = ('perm_security_id', 'perm_company_id', 'link_start', 'available_at')
KINDS = frozenset(('r4', 'pit', 'link_table'))


def _ident(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


def _literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _digest(path: Path) -> str:
    for item in (path, *path.parents):
        if item.is_symlink() or item.is_junction():
            raise ValueError('Diagnostic input reparse paths are refused')
    before = path.stat()
    if not stat.S_ISREG(before.st_mode):
        raise ValueError('Diagnostic input must be a regular file')
    h = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024 * 1024):
            h.update(block)
    after = path.stat()
    if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != (
            after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
        raise ValueError('Diagnostic input changed while hashing')
    return h.hexdigest()


@contextmanager
def _pinned(paths: tuple[Path, Path], hashes: tuple[str, str]) -> Iterator[None]:
    for digest in hashes:
        if not re.fullmatch('[0-9a-f]{64}', digest):
            raise ValueError('An explicit lowercase SHA256 is required for each input')
    for path, expected in zip(paths, hashes, strict=True):
        if _digest(path) != expected:
            raise ValueError('Diagnostic input does not match its original pin')
    try:
        yield
    finally:
        for path, expected in zip(paths, hashes, strict=True):
            if _digest(path) != expected:
                raise ValueError('Diagnostic input changed during comparison')


def compare_d0(con, *, rebuilt: Path, published: Path, rebuilt_sha256: str,
               published_sha256: str, kind: str) -> dict:
    """Count full-row differences, preserving multiplicities and r4 key defects.

    The caller owns a file-backed, 128MB/one-thread/UTC connection and spill
    directory within its isolated diagnostic job. Its external <=1GiB guard and
    complete input/code manifests remain required. This function authenticates
    the two specific files before and after every comparison, including failure.
    It creates uniquely named temporary views and never writes input data.
    """
    if kind not in KINDS:
        raise ValueError('Unknown fixed D0 product')
    settings = dict(con.execute(
        "SELECT name, value FROM duckdb_settings() WHERE name IN ('threads', 'TimeZone', 'memory_limit', 'temp_directory')"
    ).fetchall())
    if settings['threads'] != '1' or settings['TimeZone'] != 'UTC':
        raise ValueError('D0 comparison requires one thread and UTC')
    memory = re.fullmatch(r'(\d+(?:\.\d+)?)\s+(B|KiB|MiB|GiB|TiB)', settings['memory_limit'])
    units = {'B': 1, 'KiB': 1024, 'MiB': 1024**2, 'GiB': 1024**3, 'TiB': 1024**4}
    if memory is None or float(memory[1]) * units[memory[2]] > 128 * 1024**2:
        raise ValueError('D0 comparison requires at most 128MiB database memory')
    if not settings['temp_directory'] or not any(row[2] for row in con.execute('PRAGMA database_list').fetchall()):
        raise ValueError('D0 comparison requires file-backed database and spill storage')
    paths = (Path(rebuilt).absolute(), Path(published).absolute())
    hashes = (rebuilt_sha256, published_sha256)
    views = ('a0_rebuilt_' + uuid.uuid4().hex, 'a0_published_' + uuid.uuid4().hex)
    made = []
    with _pinned(paths, hashes):
        try:
            schemas = []
            counts = []
            for path, view in zip(paths, views, strict=True):
                con.execute(f'CREATE TEMP VIEW {_ident(view)} AS SELECT * FROM '
                            f'read_parquet({_literal(path.as_posix())})')
                made.append(view)
                schema = [(row[0], row[1]) for row in con.execute(
                    f'DESCRIBE SELECT * FROM {_ident(view)}').fetchall()]
                if kind == 'r4':
                    if 'created_at' not in {name for name, _ in schema}:
                        raise ValueError('r4 must contain its explicit created_at field')
                    schema = [(name, dtype) for name, dtype in schema if name != 'created_at']
                schemas.append(schema)
                counts.append(con.execute(f'SELECT count(*) FROM {_ident(view)}').fetchone()[0])
            result = {'kind': kind, 'rebuilt_rows': counts[0], 'published_rows': counts[1],
                      'excluded_columns': ['created_at'] if kind == 'r4' else [],
                      'rebuilt_schema': schemas[0], 'published_schema': schemas[1],
                      'schema_matches': schemas[0] == schemas[1],
                      'rebuilt_only_rows': None, 'published_only_rows': None,
                      'rebuilt_duplicate_key_groups': None, 'published_duplicate_key_groups': None,
                      'rebuilt_null_key_rows': None, 'published_null_key_rows': None,
                      'rows_identical': False, 'd0_pass': False, 'source_quality_certified': False}
            # A changed schema is explicit failed D0 evidence, never a subset comparison.
            if not result['schema_matches']:
                return result
            columns = ', '.join(_ident(name) for name, _ in schemas[0])
            selects = [f'SELECT {columns} FROM {_ident(view)}' for view in views]
            for field, left, right in (
                    ('rebuilt_only_rows', selects[0], selects[1]),
                    ('published_only_rows', selects[1], selects[0])):
                result[field] = con.execute(
                    f'SELECT count(*) FROM ({left} EXCEPT ALL {right}) difference').fetchone()[0]
            key_defects = False
            if kind == 'r4':
                if not set(R4_KEYS).issubset(name for name, _ in schemas[0]):
                    raise ValueError('r4 lacks the exact native TABLE_KEYS')
                keys = ', '.join(map(_ident, R4_KEYS))
                nulls = ' OR '.join(f'{_ident(key)} IS NULL' for key in R4_KEYS)
                for side, view in zip(('rebuilt', 'published'), views, strict=True):
                    duplicate = con.execute(f'SELECT count(*) FROM (SELECT {keys} FROM '
                        f'{_ident(view)} GROUP BY {keys} HAVING count(*) > 1) duplicates').fetchone()[0]
                    null = con.execute(f'SELECT count(*) FROM {_ident(view)} WHERE {nulls}').fetchone()[0]
                    result[side + '_duplicate_key_groups'] = duplicate
                    result[side + '_null_key_rows'] = null
                    key_defects |= bool(duplicate or null)
            result['rows_identical'] = result['rebuilt_only_rows'] == result['published_only_rows'] == 0
            result['d0_pass'] = result['rows_identical'] and not key_defects
            return result
        finally:
            for view in reversed(made):
                con.execute(f'DROP VIEW IF EXISTS {_ident(view)}')
