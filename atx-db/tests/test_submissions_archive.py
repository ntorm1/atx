from __future__ import annotations

import hashlib
import os
import sqlite3
import struct
import zipfile
from pathlib import Path

import pytest

from atx_db import _submissions_archive
from atx_db._submissions_archive import SubmissionsArchive


def _write_archive(path: Path, *, compression: int = zipfile.ZIP_DEFLATED) -> Path:
    with zipfile.ZipFile(path, "w", compression=compression) as archive:
        archive.comment = b"retained source comment"
        archive.writestr("CIK0000000003.json", b'{"value": 3}')
        archive.writestr("CIK0000000001.json", b'{"value": 1}')
        archive.writestr("CIK0000000001-submissions-001.json", b'{"accessionNumber": ["old"]}')
        archive.writestr("caf\u00e9/\u6771\u4eac.json", b"unicode payload")
        with pytest.warns(UserWarning, match="Duplicate name"):
            archive.writestr("CIK0000000001.json", b'{"value": "last wins"}')
        archive.writestr("CIK0000000002.json", b"{}")
    return path


@pytest.mark.parametrize("compression", [zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED, zipfile.ZIP_BZIP2, zipfile.ZIP_LZMA])
def test_selected_members_match_stdlib_with_unicode_duplicates_and_prefix_iteration(tmp_path, compression):
    path = _write_archive(tmp_path / "submissions.zip", compression=compression)
    with zipfile.ZipFile(path) as expected, SubmissionsArchive(path) as actual:
        assert actual.entry_count == len(expected.infolist()) == 6
        assert actual.unique_member_count == len(set(expected.namelist())) == 5
        assert actual.main_member_count == 3
        assert actual.history_member_count == 1
        assert actual.compressed_bytes == sum(info.compress_size for info in expected.infolist())
        assert actual.expanded_bytes == sum(info.file_size for info in expected.infolist())
        assert actual.sha256 == hashlib.sha256(path.read_bytes()).hexdigest()
        for name in expected.namelist():
            assert name in actual
            assert actual.read(name) == expected.read(name)
        assert "missing.json" not in actual
        with pytest.raises(KeyError, match="no item"):
            actual.read("missing.json")
        prefix = list(actual.main_members(through="CIK0000000002.json"))
        remainder = list(actual.main_members(after=prefix[-1]))
        assert prefix + remainder == list(actual.main_members()) == [
            "CIK0000000001.json", "CIK0000000002.json", "CIK0000000003.json",
        ]
        assert actual._reader.filelist == []
        assert actual._reader.NameToInfo == {}


def test_zip64_directory_and_member_offsets_match_stdlib(tmp_path, monkeypatch):
    # Lower the writer's size/count thresholds so a tiny fixture exercises real
    # ZIP64 central extra fields, EOCD and locator structures, without GB writes.
    with monkeypatch.context() as patch:
        patch.setattr(zipfile, "ZIP64_LIMIT", 1)
        patch.setattr(zipfile, "ZIP_FILECOUNT_LIMIT", 2)
        path = _write_archive(tmp_path / "zip64.zip")
    assert b"PK\x06\x06" in path.read_bytes()
    with zipfile.ZipFile(path) as expected, SubmissionsArchive(path) as actual:
        assert actual.entry_count == 6
        for name in expected.namelist():
            assert actual.read(name) == expected.read(name)


def test_cp437_and_concatenated_archive_match_stdlib(tmp_path):
    path = tmp_path / "legacy.zip"
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("cafe.json", b"legacy")
    path.write_bytes(b"self extracting prefix" + path.read_bytes().replace(b"cafe.json", b"caf\x82.json"))
    with zipfile.ZipFile(path) as expected, SubmissionsArchive(path) as actual:
        assert actual.read("caf\u00e9.json") == expected.read("caf\u00e9.json") == b"legacy"


@pytest.mark.parametrize("corruption", ["delete_main", "rename_main", "missing_receipt", "bad_sqlite"])
def test_cache_damage_cannot_silently_narrow_scope(tmp_path, monkeypatch, corruption):
    path = _write_archive(tmp_path / "submissions.zip")
    with SubmissionsArchive(path) as archive:
        index = archive.index_path
    receipt = index.with_suffix(".sqlite3.sha256")
    if corruption == "missing_receipt":
        receipt.unlink()
    elif corruption == "bad_sqlite":
        index.write_bytes(b"damaged cache")
    else:
        connection = sqlite3.connect(index)
        try:
            if corruption == "delete_main":
                connection.execute("DELETE FROM members WHERE name='CIK0000000002.json'")
            else:
                connection.execute("UPDATE members SET name='CIK0000000009.json' WHERE name='CIK0000000002.json'")
            connection.commit()
        finally:
            connection.close()
    builds = []
    build = SubmissionsArchive._build_index

    def capture(self):
        builds.append(self.path)
        return build(self)

    monkeypatch.setattr(SubmissionsArchive, "_build_index", capture)
    with SubmissionsArchive(path) as recovered:
        assert recovered.main_member_count == 3
        assert list(recovered.main_members()) == [
            "CIK0000000001.json", "CIK0000000002.json", "CIK0000000003.json",
        ]
    assert builds == [path]


def test_valid_cache_reuse_and_abandoned_partial_build(tmp_path, monkeypatch):
    path = _write_archive(tmp_path / "submissions.zip")
    with SubmissionsArchive(path) as archive:
        partial = archive.index_path.with_name(archive.index_path.name + ".partial-interrupted")
    partial.write_bytes(b"incomplete earlier build")

    def forbidden(self):
        pytest.fail("a hash-validated unchanged index must be reusable")

    monkeypatch.setattr(SubmissionsArchive, "_build_index", forbidden)
    with SubmissionsArchive(path) as reused:
        assert reused.read("CIK0000000001.json") == b'{"value": "last wins"}'
    assert partial.read_bytes() == b"incomplete earlier build"


def test_interrupted_build_is_not_published_and_next_open_restarts(tmp_path, monkeypatch):
    path = _write_archive(tmp_path / "submissions.zip")
    records = SubmissionsArchive._central_records

    def interrupted(self, start, size):
        yield next(records(self, start, size))
        raise RuntimeError("directory build interrupted")

    with monkeypatch.context() as patch:
        patch.setattr(SubmissionsArchive, "_central_records", interrupted)
        with pytest.raises(RuntimeError, match="interrupted"):
            SubmissionsArchive(path)
    assert not list(tmp_path.glob("*.sqlite3"))
    with SubmissionsArchive(path) as recovered:
        assert recovered.main_member_count == 3


def test_hash_identity_detects_same_stat_archive_replacement(tmp_path):
    path = tmp_path / "submissions.zip"
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("CIK0000000001.json", b"old")
    before = path.stat()
    with SubmissionsArchive(path) as archive:
        old_index = archive.index_path
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("CIK0000000001.json", b"new")
    os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
    assert path.stat().st_size == before.st_size
    with SubmissionsArchive(path) as archive:
        assert archive.index_path != old_index
        assert archive.read("CIK0000000001.json") == b"new"


@pytest.mark.parametrize("reused_index", [False, True])
def test_cached_central_record_is_compared_to_source_even_with_restored_mtime(tmp_path, reused_index):
    path = _write_archive(tmp_path / "submissions.zip")
    if reused_index:
        with SubmissionsArchive(path):
            pass
    with SubmissionsArchive(path) as archive:
        # Prime selected-member reads too: neither a newly built directory nor
        # later repeated reads may hide a changed central record in read-ahead.
        assert archive.read("CIK0000000003.json") == b'{"value": 3}'
        before = path.stat()
        with path.open("r+b") as source:
            source.seek(archive.start_dir + 16)  # central CRC, not the payload
            value = source.read(1)
            source.seek(-1, os.SEEK_CUR)
            source.write(bytes([value[0] ^ 1]))
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
        with pytest.raises(zipfile.BadZipFile, match="central record differs"):
            archive.read("CIK0000000003.json")


def test_changed_open_archive_is_refused(tmp_path):
    path = _write_archive(tmp_path / "submissions.zip")
    with SubmissionsArchive(path) as archive:
        with path.open("ab") as source:
            source.write(b"changed")
        with pytest.raises(ValueError, match="changed while open"):
            archive.read("CIK0000000003.json")


def test_corrupt_payload_keeps_stdlib_crc_failure(tmp_path):
    path = _write_archive(tmp_path / "submissions.zip", compression=zipfile.ZIP_STORED)
    data = path.read_bytes().replace(b'{"value": 3}', b'{"value": 4}', 1)
    path.write_bytes(data)
    with zipfile.ZipFile(path) as expected, SubmissionsArchive(path) as actual:
        for reader in (expected, actual):
            with pytest.raises(zipfile.BadZipFile, match="CRC"):
                reader.read("CIK0000000003.json")


def test_duplicate_local_header_preserves_overlap_refusal(tmp_path):
    path = tmp_path / "overlap.zip"
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("same.json", b"first")
        with pytest.warns(UserWarning, match="Duplicate name"):
            archive.writestr("same.json", b"last")
    data = bytearray(path.read_bytes())
    second = data.find(b"PK\x01\x02", data.find(b"PK\x01\x02") + 1)
    struct.pack_into("<L", data, second + 42, 0)
    path.write_bytes(data)
    with zipfile.ZipFile(path) as expected, SubmissionsArchive(path) as actual:
        for reader in (expected, actual):
            with pytest.raises(zipfile.BadZipFile, match="Overlapped"):
                reader.read("same.json")


@pytest.mark.parametrize("corruption", ["truncated", "central_length", "entry_count", "multi_disk"])
def test_unsupported_or_truncated_directory_fails_closed(tmp_path, corruption):
    path = _write_archive(tmp_path / "submissions.zip")
    data = bytearray(path.read_bytes())
    ending = data.rfind(b"PK\x05\x06")
    if corruption == "truncated":
        del data[-4:]
    elif corruption == "central_length":
        struct.pack_into("<H", data, data.find(b"PK\x01\x02") + 28, 65535)
    elif corruption == "entry_count":
        struct.pack_into("<HH", data, ending + 8, 7, 7)
    else:
        struct.pack_into("<H", data, ending + 4, 1)
    path.write_bytes(data)
    with pytest.raises(zipfile.BadZipFile):
        SubmissionsArchive(path)
    assert not list(tmp_path.glob("*.sqlite3"))


def test_real_directory_never_enters_eager_zipfile_reader(tmp_path, monkeypatch):
    path = _write_archive(tmp_path / "submissions.zip")
    parse = zipfile.ZipFile._RealGetContents
    bounded_sizes = []

    def only_single_entry(self):
        assert self.filename is None  # only synthetic BytesIO directories
        bounded_sizes.append(len(self.fp.getbuffer()))
        return parse(self)

    monkeypatch.setattr(zipfile.ZipFile, "_RealGetContents", only_single_entry)
    with SubmissionsArchive(path) as archive:
        assert archive.read("CIK0000000003.json") == b'{"value": 3}'
    assert bounded_sizes and max(bounded_sizes) <= _submissions_archive._CENTRAL.size + 3 * 65535 + 22
