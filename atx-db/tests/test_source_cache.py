from __future__ import annotations

import hashlib

import pytest

from atx_db.source_cache import (
    build_deterministic_source_archive,
    cache_source_payload,
    content_cache_path,
    materialize_cached_source_url,
    read_cached_source_payload,
    source_url_materialization_path,
)


def test_source_cache_is_content_addressed_and_idempotent(tmp_path) -> None:
    content = b"immutable SEC source payload"
    digest = hashlib.sha256(content).hexdigest()

    first = cache_source_payload(tmp_path, content)
    second = cache_source_payload(tmp_path, content)

    assert first == second
    assert first.sha256 == digest
    assert first.cache_path == content_cache_path(tmp_path, digest)
    assert read_cached_source_payload(first.cache_path, digest) == content


def test_source_cache_repairs_corrupt_payload_at_expected_digest_path(tmp_path) -> None:
    content = b"authoritative bytes"
    digest = hashlib.sha256(content).hexdigest()
    target = content_cache_path(tmp_path, digest)
    target.parent.mkdir(parents=True)
    target.write_bytes(b"x" * len(content))
    assert target.stat().st_size == len(content)

    cached = cache_source_payload(tmp_path, content)

    assert cached.cache_path == target
    assert target.read_bytes() == content


def test_source_url_materialization_is_verified_atomic_and_reusable(tmp_path) -> None:
    cached = cache_source_payload(tmp_path / "objects", b"filing bytes")
    source_url = "https://www.sec.gov/Archives/edgar/data/1/2/report.htm"

    first = materialize_cached_source_url(
        tmp_path / "packages",
        source_url=source_url,
        cache_path=cached.cache_path,
        expected_sha256=cached.sha256,
    )
    second = materialize_cached_source_url(
        tmp_path / "packages",
        source_url=source_url,
        cache_path=cached.cache_path,
        expected_sha256=cached.sha256,
    )

    assert first.materialized_path == source_url_materialization_path(
        tmp_path / "packages", source_url
    )
    assert first.materialized_path.read_bytes() == b"filing bytes"
    assert first.reused is False
    assert second.reused is True


@pytest.mark.parametrize(
    "source_url",
    [
        "file:///tmp/report.htm",
        "https://user:secret@www.sec.gov/report.htm",
        "https://www.sec.gov/../report.htm",
        "https://www.sec.gov/report.htm?download=1",
    ],
)
def test_source_url_materialization_rejects_unsafe_urls(tmp_path, source_url: str) -> None:
    with pytest.raises(ValueError):
        source_url_materialization_path(tmp_path, source_url)


def test_deterministic_source_archive_is_content_manifest_addressed(tmp_path) -> None:
    first_source = cache_source_payload(tmp_path / "objects", b"first")
    second_source = cache_source_payload(tmp_path / "objects", b"second")
    members = (
        ("report.htm", first_source.cache_path, first_source.sha256),
        ("report.xsd", second_source.cache_path, second_source.sha256),
    )

    first = build_deterministic_source_archive(tmp_path / "archives", members)
    second = build_deterministic_source_archive(tmp_path / "archives", tuple(reversed(members)))

    assert first.archive_path == second.archive_path
    assert first.manifest_sha256 == second.manifest_sha256
    assert first.member_count == 2
    assert first.reused is False
    assert second.reused is True


@pytest.mark.parametrize("digest", ["", "a" * 63, "z" * 64])
def test_content_cache_path_rejects_invalid_digest(tmp_path, digest: str) -> None:
    with pytest.raises(ValueError, match="sha256"):
        content_cache_path(tmp_path, digest)


def test_content_cache_path_normalizes_uppercase_digest(tmp_path) -> None:
    digest = "A" * 64
    assert content_cache_path(tmp_path, digest).name == digest.lower()
