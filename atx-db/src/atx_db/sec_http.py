"""SEC network hygiene: one approved user agent, one host-wide rate limit, fetch/load split.

Every SEC request made by this package goes through this module:

* :data:`APPROVED_SEC_USER_AGENT` is the only user agent (and the only contact) ever sent to SEC.
  ``ATX_SEC_USER_AGENT`` / ``--sec-user-agent`` may only repeat it (:func:`validate_sec_user_agent`).
* :class:`SecRateLimiter` is a cross-process token bucket shared by every worker on the host through a
  file lock (default ``<data dir>/cache/.sec_rate.lock``) and a state file next to it; the default and the
  ceiling are 5 requests/second. Every HTTP attempt (first try, retry and redirect) takes one token.
* :func:`sec_session` returns a ``requests.Session`` whose adapter applies the approved user agent, the
  shared limiter and a bounded retry policy (429/5xx/connection errors, ``Retry-After`` honoured).
* :class:`FetchLedgerStore` is the fetch half of the fetch/load split: fetch workers never hold a DuckDB
  writer; they write content-addressed files (``objects/<sha[:2]>/<sha256>``) plus one ``fetch-ledger.jsonl``
  line per request outcome (url, sha256, status, fetched_at) and resume by skipping ledgered URLs. Loaders
  read the files back (SHA-verified) and never touch the network.

All workers on a host must resolve the same lock path. A worker launched from a git-archive export resolves
its data dir inside the export, so it must set ``ATX_SEC_RATE_LOCK`` (or ``ATX_DB_DATA_DIR``) to the shared
``C:/atx/atx-db/data`` location.
"""
from __future__ import annotations

import datetime as dt
import email.utils
import hashlib
import json
import os
import tempfile
import threading
import time
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

import requests
from requests.adapters import HTTPAdapter

APPROVED_SEC_USER_AGENT = "atx-db/0.1 atx-research@example.com"
#: Descriptive agent for non-SEC public sources that must not receive any contact address (FINRA).
PUBLIC_DATA_USER_AGENT = "atx-db/0.1 public-data-loader"
SEC_USER_AGENT_ENV = "ATX_SEC_USER_AGENT"
SEC_RATE_LOCK_ENV = "ATX_SEC_RATE_LOCK"
#: Optional JSONL audit of every limiter grant (epoch seconds, pid, label); written under the limiter lock.
SEC_RATE_LOG_ENV = "ATX_SEC_RATE_LOG"
MAX_SEC_REQUESTS_PER_SECOND = 5.0
SEC_RETRY_STATUS_CODES = frozenset({429, 500, 502, 503, 504})
SEC_MAX_ATTEMPTS = 6
SEC_BACKOFF_SECONDS = 0.5
_MAX_BACKOFF_SECONDS = 60.0
_LOCK_POLL_SECONDS = 0.002
_LOCK_TIMEOUT_SECONDS = 600.0
FETCH_LEDGER_NAME = "fetch-ledger.jsonl"
RESPONSE_TOO_LARGE = "response_too_large"


def validate_sec_user_agent(value: str | None) -> str:
    """Return APPROVED_SEC_USER_AGENT; raise ValueError for any other non-empty value."""

    if value is None or not str(value).strip() or str(value).strip() == APPROVED_SEC_USER_AGENT:
        return APPROVED_SEC_USER_AGENT
    # The rejected value is not echoed: it may carry a contact that must not spread into logs or receipts.
    raise ValueError(
        f"{SEC_USER_AGENT_ENV} / --sec-user-agent may only be the approved SEC user agent "
        f"{APPROVED_SEC_USER_AGENT!r}; unset it to use that value (no other contact may be sent to SEC)."
    )


def resolve_sec_user_agent(explicit: str | None = None) -> str:
    """``--sec-user-agent`` when given, else ``ATX_SEC_USER_AGENT``, through :func:`validate_sec_user_agent`."""

    if explicit is not None and explicit.strip():
        return validate_sec_user_agent(explicit)
    return validate_sec_user_agent(os.environ.get(SEC_USER_AGENT_ENV))


def is_sec_url(url: str) -> bool:
    host = (urlsplit(url).hostname or "").lower()
    return host == "sec.gov" or host.endswith(".sec.gov")


def default_sec_rate_lock_path() -> Path:
    configured = os.environ.get(SEC_RATE_LOCK_ENV)
    if configured:
        return Path(os.path.expandvars(configured)).expanduser()
    from .connection import resolve_data_dir

    return resolve_data_dir() / "cache" / ".sec_rate.lock"


@contextmanager
def _exclusive_file_lock(path: Path, *, timeout_s: float = _LOCK_TIMEOUT_SECONDS) -> Iterator[None]:
    """Hold an OS byte-range lock on ``path``; the OS releases it if the holder dies."""

    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        if os.name == "nt":
            import msvcrt

            deadline = time.monotonic() + timeout_s
            while True:
                os.lseek(fd, 0, os.SEEK_SET)
                try:
                    msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise TimeoutError(f"could not lock {path} within {timeout_s:.0f}s") from None
                    time.sleep(_LOCK_POLL_SECONDS)
            try:
                yield
            finally:
                os.lseek(fd, 0, os.SEEK_SET)
                msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            fcntl.flock(fd, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(fd, fcntl.LOCK_UN)
    finally:
        os.close(fd)


class SecRateLimiter:
    """Cross-process token bucket (file lock at data/cache/.sec_rate.lock + state file); default 5 req/s.

    The bucket holds one token, so grants are spaced at least ``1 / rate_per_s`` apart host-wide and no
    one-second window can hold more than ``rate_per_s`` grants. The lock is held while waiting for the slot
    and the next slot is computed from the actual grant time, so a late wake-up delays the schedule instead of
    compressing it. A wall-clock step backwards never stalls a worker for more than one interval.
    """

    def __init__(self, rate_per_s: float = MAX_SEC_REQUESTS_PER_SECOND, lock_path: Path | None = None) -> None:
        if not 0 < rate_per_s <= MAX_SEC_REQUESTS_PER_SECOND:
            raise ValueError(f"SEC request rate must be in (0, {MAX_SEC_REQUESTS_PER_SECOND:g}] requests/second")
        self.rate_per_s = float(rate_per_s)
        self.interval_s = 1.0 / self.rate_per_s
        self.lock_path = Path(lock_path) if lock_path is not None else default_sec_rate_lock_path()
        self.state_path = self.lock_path.with_name(f"{self.lock_path.stem}.state")
        self._thread_lock = threading.Lock()

    def acquire(self, label: str | None = None) -> None:
        with self._thread_lock, _exclusive_file_lock(self.lock_path):
            now = time.time()
            wait = min(max(self._next_slot(now) - now, 0.0), self.interval_s)
            if wait > 0:
                time.sleep(wait)
            granted = time.time()
            self.state_path.write_text(repr(granted + self.interval_s), encoding="ascii")
            log_path = os.environ.get(SEC_RATE_LOG_ENV)
            if log_path:
                with open(log_path, "a", encoding="utf-8") as handle:
                    handle.write(json.dumps({"t": granted, "pid": os.getpid(), "label": label}) + "\n")

    def _next_slot(self, now: float) -> float:
        try:
            return float(self.state_path.read_text(encoding="ascii"))
        except FileNotFoundError:
            return 0.0
        except (OSError, ValueError):
            return now + self.interval_s  # unreadable state: wait one full interval


_DEFAULT_LIMITER: SecRateLimiter | None = None
_DEFAULT_LIMITER_GUARD = threading.Lock()


def default_sec_limiter() -> SecRateLimiter:
    """The process-wide handle on the host-wide 5 req/s bucket."""

    global _DEFAULT_LIMITER
    with _DEFAULT_LIMITER_GUARD:
        if _DEFAULT_LIMITER is None:
            _DEFAULT_LIMITER = SecRateLimiter()
        return _DEFAULT_LIMITER


def _retry_after_seconds(response: Any) -> float:
    value = (response.headers or {}).get("Retry-After") if hasattr(response, "headers") else None
    if not value:
        return 0.0
    value = str(value).strip()
    if value.isdigit():
        return min(float(value), _MAX_BACKOFF_SECONDS)
    try:
        when = email.utils.parsedate_to_datetime(value)
    except (TypeError, ValueError):
        return 0.0
    if when.tzinfo is None:
        when = when.replace(tzinfo=dt.UTC)
    return min(max((when - dt.datetime.now(dt.UTC)).total_seconds(), 0.0), _MAX_BACKOFF_SECONDS)


class _SecAdapter(HTTPAdapter):
    """Every attempt (first try, retry, redirect hop) takes one token from the shared limiter."""

    def __init__(self, limiter: SecRateLimiter, *, max_attempts: int, backoff_s: float) -> None:
        super().__init__(max_retries=0)
        if max_attempts < 1:
            raise ValueError("max_attempts must be positive")
        self.limiter = limiter
        self.max_attempts = max_attempts
        self.backoff_s = backoff_s

    def _backoff(self, attempt: int) -> float:
        return min(self.backoff_s * (2 ** (attempt - 1)), _MAX_BACKOFF_SECONDS)

    def send(self, request, **kwargs):  # type: ignore[no-untyped-def,override]
        idempotent = (request.method or "GET").upper() in ("GET", "HEAD")
        attempt = 0
        while True:
            attempt += 1
            self.limiter.acquire(request.url)
            try:
                response = super().send(request, **kwargs)
            except (requests.ConnectionError, requests.Timeout):
                if not idempotent or attempt >= self.max_attempts:
                    raise
                time.sleep(self._backoff(attempt))
                continue
            if idempotent and response.status_code in SEC_RETRY_STATUS_CODES and attempt < self.max_attempts:
                delay = max(self._backoff(attempt), _retry_after_seconds(response))
                response.close()
                time.sleep(delay)
                continue
            return response


def sec_session(
    user_agent: str | None = None,
    *,
    limiter: SecRateLimiter | None = None,
    max_attempts: int = SEC_MAX_ATTEMPTS,
    backoff_s: float = SEC_BACKOFF_SECONDS,
) -> requests.Session:
    """A session that can only send the approved user agent, paced by the host-wide limiter."""

    agent = validate_sec_user_agent(user_agent)
    session = requests.Session()
    session.headers.update(
        {
            "User-Agent": agent,
            "Accept": "application/json,text/plain,*/*",
            "Accept-Encoding": "gzip, deflate",
        }
    )
    adapter = _SecAdapter(limiter or default_sec_limiter(), max_attempts=max_attempts, backoff_s=backoff_s)
    session.mount("https://", adapter)
    session.mount("http://", adapter)
    return session


_SESSIONS: dict[int, tuple[SecRateLimiter, requests.Session]] = {}
_SESSIONS_GUARD = threading.Lock()


def _session_for(limiter: SecRateLimiter) -> requests.Session:
    with _SESSIONS_GUARD:
        cached = _SESSIONS.get(id(limiter))
        if cached is None or cached[0] is not limiter:
            cached = (limiter, sec_session(limiter=limiter))
            _SESSIONS[id(limiter)] = cached
        return cached[1]


def read_bounded_response(response: Any, maximum: int | None) -> bytes:
    """Read a streamed body, refusing (ValueError ``response_too_large``) anything above ``maximum`` bytes."""

    declared = response.headers.get("Content-Length") if getattr(response, "headers", None) else None
    if maximum is not None and declared and str(declared).isdigit() and int(declared) > maximum:
        raise ValueError(RESPONSE_TOO_LARGE)
    chunks: list[bytes] = []
    observed = 0
    for chunk in response.iter_content(chunk_size=64 * 1024):
        if not chunk:
            continue
        observed += len(chunk)
        if maximum is not None and observed > maximum:
            raise ValueError(RESPONSE_TOO_LARGE)
        chunks.append(chunk)
    return b"".join(chunks)


def sec_get(url: str, *, limiter: SecRateLimiter, timeout: float = 30.0, maximum: int | None = None) -> bytes:
    """One rate-limited GET of an SEC URL with the approved user agent; raises on a non-2xx final status."""

    if not is_sec_url(url):
        raise ValueError(f"sec_get only fetches SEC hosts, not {urlsplit(url).hostname!r}")
    response = _session_for(limiter).get(url, timeout=timeout, stream=True)
    try:
        response.raise_for_status()
        return read_bounded_response(response, maximum)
    finally:
        response.close()


@dataclass(frozen=True)
class FetchRecord:
    """One fetch-ledger line. ``status`` is the final HTTP status, 0 when no response arrived."""

    url: str
    status: int
    sha256: str | None
    fetched_at: str | None
    bytes: int = 0
    error: str | None = None

    @property
    def ok(self) -> bool:
        return self.status == 200 and self.error is None and self.sha256 is not None

    @property
    def terminal(self) -> bool:
        """Final outcomes are never refetched; throttling, 5xx and transport failures are retried later."""

        if self.status == 200:
            return self.error is None or self.error == RESPONSE_TOO_LARGE
        return 400 <= self.status < 500 and self.status not in (403, 429)


class FetchLedgerStore:
    """Content-addressed fetch output plus an append-only ``fetch-ledger.jsonl`` (fetch/load split).

    Only a URL's newest *terminal* record counts; a rerun skips every URL that has one. Payload bytes are
    written before their ledger line, so a kill leaves at most an unreferenced object, never a ledger line
    without its bytes. Appends are serialized by a file lock, so concurrent workers may share one store.
    The in-memory index keeps a 16-byte URL digest per terminal record (not the URL text).
    """

    def __init__(self, root: str | Path) -> None:
        self.root = Path(root)
        self.ledger_path = self.root / FETCH_LEDGER_NAME
        self.objects_dir = self.root / "objects"
        self._lock_path = self.root / f"{FETCH_LEDGER_NAME}.lock"
        self._index: dict[bytes, tuple[int, bytes | None, str | None, int]] = {}
        self.ledger_lines = 0
        self.corrupt_lines = 0
        self.duplicate_ok_urls = 0
        self._loaded = False

    @staticmethod
    def _key(url: str) -> bytes:
        return hashlib.blake2b(url.encode("utf-8"), digest_size=16).digest()

    def load(self) -> FetchLedgerStore:
        """(Re)read the ledger; counts corrupt lines and URLs with more than one successful fetch."""

        self._index = {}
        self.ledger_lines = self.corrupt_lines = self.duplicate_ok_urls = 0
        ok_seen: set[bytes] = set()
        if self.ledger_path.is_file():
            with self.ledger_path.open("rb") as handle:
                for raw in handle:
                    if not raw.strip():
                        continue
                    self.ledger_lines += 1
                    try:
                        record = _record_from_json(json.loads(raw))
                    except (ValueError, KeyError, TypeError):
                        self.corrupt_lines += 1  # a line torn by a kill mid-append
                        continue
                    key = self._key(record.url)
                    if record.ok:
                        if key in ok_seen:
                            self.duplicate_ok_urls += 1
                        ok_seen.add(key)
                    if record.terminal:
                        self._remember(key, record)
        self._loaded = True
        return self

    def _remember(self, key: bytes, record: FetchRecord) -> None:
        self._index[key] = (
            record.status, bytes.fromhex(record.sha256) if record.sha256 else None, record.error, record.bytes,
        )

    def lookup(self, url: str) -> FetchRecord | None:
        """The newest terminal record for ``url`` (``fetched_at`` is not retained in memory)."""

        if not self._loaded:
            self.load()
        entry = self._index.get(self._key(url))
        if entry is None:
            return None
        status, sha, error, size = entry
        return FetchRecord(url, status, sha.hex() if sha else None, None, size, error)

    @property
    def terminal_count(self) -> int:
        if not self._loaded:
            self.load()
        return len(self._index)

    def object_path(self, sha256: str) -> Path:
        return self.objects_dir / sha256[:2] / sha256

    def read(self, record: FetchRecord, *, maximum: int | None = None) -> bytes:
        """The verified bytes of a successful record; ValueError on a size or SHA mismatch."""

        if not record.ok or record.sha256 is None:
            raise ValueError(f"no content was fetched for {record.url}")
        payload = self.object_path(record.sha256).read_bytes()
        if hashlib.sha256(payload).hexdigest() != record.sha256:
            raise ValueError("fetch_object_sha_mismatch")
        if maximum is not None and len(payload) > maximum:
            raise ValueError(RESPONSE_TOO_LARGE)
        return payload

    def ensure(
        self,
        url: str,
        *,
        limiter: SecRateLimiter | None = None,
        timeout: float = 30.0,
        maximum: int | None = None,
    ) -> tuple[FetchRecord, bool]:
        """Return ``(record, fetched_now)``: the ledgered terminal record, else one new (ledgered) fetch."""

        record = self.lookup(url)
        if record is not None:
            return record, False
        return self.fetch(url, limiter=limiter, timeout=timeout, maximum=maximum), True

    def fetch(
        self,
        url: str,
        *,
        limiter: SecRateLimiter | None = None,
        timeout: float = 30.0,
        maximum: int | None = None,
    ) -> FetchRecord:
        if not is_sec_url(url):
            raise ValueError(f"FetchLedgerStore only fetches SEC hosts, not {urlsplit(url).hostname!r}")
        session = _session_for(limiter or default_sec_limiter())
        status, payload, error = 0, b"", None
        try:
            response = session.get(url, timeout=timeout, stream=True)
            try:
                status = int(response.status_code)
                if status == 200:
                    payload = read_bounded_response(response, maximum)
            finally:
                response.close()
        except ValueError as exc:
            error, payload = str(exc)[:200] or type(exc).__name__, b""
        except requests.RequestException as exc:
            error, payload = type(exc).__name__, b""
        sha = None
        if status == 200 and error is None:
            sha = hashlib.sha256(payload).hexdigest()
            self._write_object(sha, payload)
        record = FetchRecord(
            url, status, sha, dt.datetime.now(dt.UTC).isoformat(timespec="milliseconds"), len(payload), error,
        )
        self._append(record)
        if record.terminal:
            self._remember(self._key(url), record)
        return record

    def _write_object(self, sha256: str, payload: bytes) -> None:
        path = self.object_path(sha256)
        if path.is_file() and path.stat().st_size == len(payload):
            return
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="wb", dir=path.parent, prefix=f".{sha256[:16]}.", suffix=".tmp", delete=False
        ) as handle:
            temporary = Path(handle.name)
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        try:
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)

    def _append(self, record: FetchRecord) -> None:
        line = json.dumps(
            {
                "url": record.url,
                "sha256": record.sha256,
                "status": record.status,
                "fetched_at": record.fetched_at,
                "bytes": record.bytes,
                "error": record.error,
                "user_agent": APPROVED_SEC_USER_AGENT,
                "pid": os.getpid(),
            },
            sort_keys=True,
        ).encode("utf-8") + b"\n"
        self.root.mkdir(parents=True, exist_ok=True)
        with _exclusive_file_lock(self._lock_path), self.ledger_path.open("ab+") as handle:
            handle.seek(0, os.SEEK_END)
            if handle.tell():
                handle.seek(-1, os.SEEK_END)
                if handle.read(1) != b"\n":
                    handle.write(b"\n")  # isolate a line torn by an earlier kill
            handle.write(line)
            handle.flush()
            os.fsync(handle.fileno())


def _record_from_json(payload: dict[str, Any]) -> FetchRecord:
    return FetchRecord(
        url=str(payload["url"]),
        status=int(payload["status"]),
        sha256=payload.get("sha256"),
        fetched_at=payload.get("fetched_at"),
        bytes=int(payload.get("bytes") or 0),
        error=payload.get("error"),
    )
