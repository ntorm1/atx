"""Immutable content-addressed cache for downloaded provider source payloads."""

from __future__ import annotations

import hashlib
import os
import shutil
import uuid
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from urllib.parse import urlsplit


@dataclass(frozen=True)
class CachedSourcePayload:
    content: bytes
    cache_path: Path
    sha256: str


@dataclass(frozen=True)
class MaterializedSourcePayload:
    materialized_path: Path
    cache_path: Path
    sha256: str
    reused: bool


@dataclass(frozen=True)
class DeterministicSourceArchive:
    archive_path: Path
    manifest_sha256: str
    member_count: int
    reused: bool


def content_cache_path(cache_dir: Path, sha256: str) -> Path:
    """Return the stable fan-out path for a SHA-256-addressed payload."""

    normalized = sha256.strip().lower()
    if len(normalized) != 64 or any(char not in "0123456789abcdef" for char in normalized):
        raise ValueError("sha256 must be a 64-character lowercase hexadecimal digest")
    return cache_dir / "sha256" / normalized[:2] / normalized[2:4] / normalized


def cache_source_payload(cache_dir: Path, content: bytes) -> CachedSourcePayload:
    """Atomically persist bytes under their digest without mutating valid content."""

    digest = hashlib.sha256(content).hexdigest()
    target = content_cache_path(cache_dir, digest)
    if (
        target.is_file()
        and target.stat().st_size == len(content)
        and read_cached_source_payload(target, digest) is not None
    ):
        return CachedSourcePayload(content=content, cache_path=target, sha256=digest)

    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f".{target.name}.{uuid.uuid4().hex}.tmp")
    try:
        temporary.write_bytes(content)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)
    return CachedSourcePayload(content=content, cache_path=target, sha256=digest)


def read_cached_source_payload(cache_path: Path, expected_sha256: str) -> bytes | None:
    """Read a cached payload only when its bytes still match recorded lineage."""

    if not cache_path.is_file():
        return None
    content = cache_path.read_bytes()
    if hashlib.sha256(content).hexdigest() != expected_sha256:
        return None
    return content


def source_url_materialization_path(materialized_root: Path, source_url: str) -> Path:
    """Project an HTTP(S) URL into a safe, deterministic local package path."""

    parsed = urlsplit(source_url)
    if parsed.scheme.lower() not in {"http", "https"} or not parsed.hostname:
        raise ValueError(f"source_url must be an absolute HTTP(S) URL: {source_url}")
    if parsed.username is not None or parsed.password is not None:
        raise ValueError("source_url credentials are not supported")
    if parsed.query or parsed.fragment:
        raise ValueError("source_url query strings and fragments are not supported")
    parts = tuple(part for part in parsed.path.split("/") if part)
    if not parts or any(part in {".", ".."} or "\\" in part for part in parts):
        raise ValueError(f"source_url has an unsafe or empty path: {source_url}")
    host = parsed.hostname.lower()
    if parsed.port is not None:
        host = f"{host}_port_{parsed.port}"
    return materialized_root / host / Path(*parts)


def materialize_cached_source_url(
    materialized_root: Path,
    *,
    source_url: str,
    cache_path: Path,
    expected_sha256: str,
) -> MaterializedSourcePayload:
    """Atomically expose verified content-addressed bytes under their source URL path."""

    if read_cached_source_payload(cache_path, expected_sha256) is None:
        raise ValueError(f"Cached source payload failed SHA-256 verification: {cache_path}")
    target = source_url_materialization_path(materialized_root, source_url)
    if read_cached_source_payload(target, expected_sha256) is not None:
        return MaterializedSourcePayload(
            materialized_path=target,
            cache_path=cache_path,
            sha256=expected_sha256,
            reused=True,
        )

    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f".{target.name}.{uuid.uuid4().hex}.tmp")
    try:
        try:
            os.link(cache_path, temporary)
        except OSError:
            shutil.copyfile(cache_path, temporary)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)
    return MaterializedSourcePayload(
        materialized_path=target,
        cache_path=cache_path,
        sha256=expected_sha256,
        reused=False,
    )


def build_deterministic_source_archive(
    archive_root: Path,
    members: tuple[tuple[str, Path, str], ...],
) -> DeterministicSourceArchive:
    """Build an immutable ZIP whose identity is the ordered member/digest manifest."""

    normalized: list[tuple[str, Path, str, bytes]] = []
    for member_name, cache_path, expected_sha256 in members:
        member = PurePosixPath(member_name)
        if (
            member.is_absolute()
            or not member.parts
            or any(part in {"", ".", ".."} for part in member.parts)
        ):
            raise ValueError(f"Unsafe archive member path: {member_name}")
        content = read_cached_source_payload(cache_path, expected_sha256)
        if content is None:
            raise ValueError(f"Cached source payload failed SHA-256 verification: {cache_path}")
        normalized.append((member.as_posix(), cache_path, expected_sha256, content))
    normalized.sort(key=lambda value: value[0])
    names = [value[0] for value in normalized]
    if not names or len(names) != len(set(names)):
        raise ValueError("Archive members must be non-empty and uniquely named")
    manifest = "\n".join(f"{name}\0{digest}" for name, _path, digest, _content in normalized)
    manifest_sha256 = hashlib.sha256(manifest.encode()).hexdigest()
    target = archive_root / manifest_sha256[:2] / f"{manifest_sha256}.zip"

    if target.is_file():
        try:
            with zipfile.ZipFile(target) as archive:
                valid = archive.namelist() == names and all(
                    hashlib.sha256(archive.read(name)).hexdigest() == digest
                    for name, _path, digest, _content in normalized
                )
            if valid:
                return DeterministicSourceArchive(
                    archive_path=target,
                    manifest_sha256=manifest_sha256,
                    member_count=len(normalized),
                    reused=True,
                )
        except (OSError, zipfile.BadZipFile):
            pass

    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f".{target.name}.{uuid.uuid4().hex}.tmp")
    try:
        with zipfile.ZipFile(temporary, "w") as archive:
            for name, _path, _digest, content in normalized:
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100444 << 16
                archive.writestr(info, content)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)
    return DeterministicSourceArchive(
        archive_path=target,
        manifest_sha256=manifest_sha256,
        member_count=len(normalized),
        reused=False,
    )
