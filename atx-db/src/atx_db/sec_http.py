"""SEC network hygiene: one approved user agent, one host-wide rate limit, fetch/load split.

Every SEC request made by this package goes through this module:

* :data:`APPROVED_SEC_USER_AGENT` is the only user agent (and the only contact) ever sent to SEC.
  ``ATX_SEC_USER_AGENT`` / ``--sec-user-agent`` may only repeat it (:func:`validate_sec_user_agent`).
* :class:`SecRateLimiter` is a cross-process token bucket shared by every worker on the host through one
  file lock at a hard-coded absolute path (:data:`CANONICAL_SEC_RATE_LOCK`,
  ``C:/atx/atx-db/data/cache/.sec_rate.lock``, ruling C-63) with its state file and trip marker beside it.
  The path does not depend on the checkout, a git-archive export, ``ATX_DB_PATH`` or ``ATX_DB_DATA_DIR``,
  so every launch on the host draws from the same bucket. ``ATX_SEC_RATE_LOCK`` may only name that same path; any other value
  refuses to start (a second lock would be a second 5 req/s bucket).
  The default and the ceiling are 5 requests/second. Every HTTP attempt (first try, retry and redirect hop)
  takes one token.
* SEC throttling is shared host-wide. A 403 or 429 from an SEC host pauses every worker on the host
  (``Retry-After`` when SEC sends a longer one, else 60 s doubling per consecutive block episode, at most
  600 s); statuses from other hosts never touch that state. The
  :data:`SEC_BLOCK_ABORT_AFTER`-th consecutive block episode trips the host: every later SEC request on the
  host raises :class:`SecBlockedError` (a non-zero exit) until an operator has checked SEC access and runs
  ``python -m atx_db.sec_http --clear-block``. An unreadable limiter state fails closed (no grant, no
  overwrite; :class:`SecLimiterStateError` if it persists).
* :func:`sec_session` returns a ``requests.Session`` whose adapter applies the approved user agent, the
  shared limiter, the shared block pause and a bounded retry policy (403/429 after the shared pause,
  5xx/connection errors with local backoff).
* :class:`FetchLedgerStore` is the fetch half of the fetch/load split: fetch workers never hold a DuckDB
  writer; they write content-addressed files plus one ``fetch-ledger.jsonl`` line per request outcome (url,
  sha256, status, fetched_at, rate_lock) and resume by skipping ledgered URLs. Objects are gzip-compressed at
  rest (``objects/<sha[:2]>/<sha256>.gz``, ruling C-71); the ledger sha256 and ``bytes`` stay over the RAW
  bytes. Loaders read the files back (decompressed, SHA-verified) and never touch the network. Several fetch
  threads may share one store and one limiter (ruling C-72).
"""
from __future__ import annotations

import datetime as dt
import email.utils
import gzip
import hashlib
import json
import os
import tempfile
import threading
import time
import zlib
from collections.abc import Iterator
from contextlib import contextmanager, suppress
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
#: SEC's rate / undeclared-tool refusals: every one pauses the whole host (see SecRateLimiter.record_response).
SEC_BLOCK_STATUS_CODES = frozenset({403, 429})
SEC_BLOCK_PAUSE_SECONDS = 60.0
SEC_MAX_BLOCK_PAUSE_SECONDS = 600.0
#: Consecutive host-wide block episodes after which every SEC request on the host stops (SecBlockedError).
SEC_BLOCK_ABORT_AFTER = 5
SEC_MAX_ATTEMPTS = 6
SEC_BACKOFF_SECONDS = 0.5
_MAX_BACKOFF_SECONDS = 60.0
_LOCK_POLL_SECONDS = 0.002
_LOCK_TIMEOUT_SECONDS = 600.0
_PAUSE_POLL_SECONDS = 1.0
#: An unreadable state file is re-read a few times (a scanner briefly holding it) before callers fail closed.
_STATE_READ_ATTEMPTS = 5
_STATE_READ_RETRY_SECONDS = 0.05
#: The host-wide limiter lock (ruling C-63): one absolute path on this host, whatever root a worker runs from.
#: The state file and the trip marker live beside it in the same directory.
CANONICAL_SEC_RATE_LOCK = Path(r"C:\atx\atx-db\data\cache\.sec_rate.lock")
FETCH_LEDGER_NAME = "fetch-ledger.jsonl"
#: Objects at rest (ruling C-71): ``<sha256>.gz``, gzip level 6, mtime 0 (deterministic); sha256 is over the raw
#: bytes. A plain ``<sha256>`` file is a pre-C-71 raw object until :meth:`FetchLedgerStore.compress_raw_objects`.
OBJECT_SUFFIX = ".gz"
OBJECT_GZIP_LEVEL = 6
RESPONSE_TOO_LARGE = "response_too_large"
FETCH_OBJECT_SHA_MISMATCH = "fetch_object_sha_mismatch"
CLEAR_BLOCK_COMMAND = "python -m atx_db.sec_http --clear-block"


class SecBlockedError(BaseException):
    """SEC kept refusing this host (403/429): every SEC request on the host stops until an operator clears it.

    It derives from ``BaseException`` (like ``KeyboardInterrupt``) on purpose: the loaders' per-item
    ``except Exception`` handlers must not turn a host-wide block into a stream of per-item failures or
    retryable ledger lines. The process aborts with a non-zero exit instead.
    """


class SecLimiterStateError(SecBlockedError):
    """The host-wide limiter state stayed unreadable, so SEC requests on the host stop (fail closed).

    An unreadable state may hold a block pause and the block count; granting past it or overwriting it would
    end that pause host-wide. An operator checks the file, then runs ``--clear-block`` (the unreadable file is
    kept aside, never deleted).
    """


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


def canonical_sec_rate_lock_path() -> Path:
    """The one host-wide SEC limiter lock (:data:`CANONICAL_SEC_RATE_LOCK`, ruling C-63).

    A hard-coded absolute path: it does not follow the checkout, a git-archive export (C-23), ``ATX_DB_PATH``
    or ``ATX_DB_DATA_DIR``, so workers launched from different roots share one 5 req/s bucket.
    """

    return CANONICAL_SEC_RATE_LOCK


def _same_path(left: Path, right: Path) -> bool:
    return os.path.normcase(os.path.abspath(left)) == os.path.normcase(os.path.abspath(right))


def default_sec_rate_lock_path() -> Path:
    """:func:`canonical_sec_rate_lock_path`; refuses an ``ATX_SEC_RATE_LOCK`` that names any other lock."""

    canonical = canonical_sec_rate_lock_path()
    configured = os.environ.get(SEC_RATE_LOCK_ENV)
    if configured and configured.strip():
        requested = Path(os.path.expandvars(configured.strip())).expanduser()
        if not _same_path(requested, canonical):
            raise ValueError(
                f"{SEC_RATE_LOCK_ENV}={str(requested)!r} names a second SEC rate lock; every SEC worker on the "
                f"host must share the host-wide lock {str(canonical)!r} (a second lock is a second 5 req/s "
                f"bucket). Unset {SEC_RATE_LOCK_ENV}."
            )
    return canonical


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


@dataclass
class _LimiterState:
    next_slot: float = 0.0  # earliest time of the next grant (epoch seconds)
    pause_until: float = 0.0  # host-wide block pause: no grant before this time
    pause_s: float = 0.0  # length of that pause when it was set (bounds a wall-clock step back)
    blocks: int = 0  # consecutive block episodes on this host
    block_at: float = 0.0  # when the latest block episode was recorded


class SecRateLimiter:
    """Cross-process token bucket (host-wide file lock + state file); default 5 req/s.

    The default lock is :func:`canonical_sec_rate_lock_path`, one fixed path per host. The bucket holds one
    token, so grants are spaced at least ``1 / rate_per_s`` apart host-wide and no one-second window can hold
    more than ``rate_per_s`` grants. The lock is held while waiting for the slot and the next slot is computed
    from the actual grant time, so a late wake-up delays the schedule instead of compressing it. A wall-clock
    step backwards never stalls a worker for more than one interval (or one block pause).

    The same state carries SEC's throttling signal (:meth:`record_response`): a block pause every worker
    waits out, the count of consecutive block episodes, and a trip marker (``.sec_rate.blocked``) that stops
    every SEC request on the host until :meth:`clear_block`. ``lock_path`` and the block parameters are for
    tests and offline probes; production code uses :func:`default_sec_limiter`.
    """

    def __init__(
        self,
        rate_per_s: float = MAX_SEC_REQUESTS_PER_SECOND,
        lock_path: Path | None = None,
        *,
        block_pause_s: float = SEC_BLOCK_PAUSE_SECONDS,
        max_block_pause_s: float = SEC_MAX_BLOCK_PAUSE_SECONDS,
        abort_after: int = SEC_BLOCK_ABORT_AFTER,
    ) -> None:
        if not 0 < rate_per_s <= MAX_SEC_REQUESTS_PER_SECOND:
            raise ValueError(f"SEC request rate must be in (0, {MAX_SEC_REQUESTS_PER_SECOND:g}] requests/second")
        if not 0 < block_pause_s <= max_block_pause_s or abort_after < 1:
            raise ValueError("block pause must be in (0, max_block_pause_s] and abort_after must be positive")
        self.rate_per_s = float(rate_per_s)
        self.interval_s = 1.0 / self.rate_per_s
        self.block_pause_s = float(block_pause_s)
        self.max_block_pause_s = float(max_block_pause_s)
        self.abort_after = int(abort_after)
        self.lock_path = Path(lock_path) if lock_path is not None else default_sec_rate_lock_path()
        self.state_path = self.lock_path.with_name(f"{self.lock_path.stem}.state")
        self.blocked_path = self.lock_path.with_name(f"{self.lock_path.stem}.blocked")
        self._thread_lock = threading.Lock()

    def acquire(self, label: str | None = None) -> float:
        """Wait for this process's next SEC request slot and return the grant time (epoch seconds).

        A host-wide block pause is waited out with the lock released between polls; a tripped host raises
        :class:`SecBlockedError`. An unreadable state fails closed: nothing is granted and nothing is written
        while it stays unreadable (it may hold a pause), and after ``max_block_pause_s`` of that the call
        raises :class:`SecLimiterStateError`.
        """

        unreadable_since: float | None = None
        while True:
            with self._thread_lock, _exclusive_file_lock(self.lock_path):
                self._raise_if_tripped()
                now = time.time()
                state = self._read_state()
                if state is None:
                    unreadable_since = time.monotonic() if unreadable_since is None else unreadable_since
                    unreadable_s = time.monotonic() - unreadable_since
                    if unreadable_s > self.max_block_pause_s:
                        raise self._state_error(unreadable_s)
                    paused = _PAUSE_POLL_SECONDS
                else:
                    unreadable_since = None
                    paused = state.pause_until - now
                    if paused <= 0:
                        wait = min(max(state.next_slot - now, 0.0), self.interval_s)
                        if wait > 0:
                            time.sleep(wait)
                        granted = time.time()
                        state.next_slot = granted + self.interval_s
                        self._write_state(state)
                        self._log({"t": granted, "pid": os.getpid(), "label": label})
                        return granted
                    bound = state.pause_s if 0 < state.pause_s <= self.max_block_pause_s else self.max_block_pause_s
                    if paused > bound + self.interval_s:  # the wall clock stepped back: never wait past one pause
                        state.pause_until, state.pause_s = now + bound, bound
                        self._write_state(state)
                        paused = bound
            time.sleep(min(paused, _PAUSE_POLL_SECONDS))

    def record_response(self, status: int, *, granted_at: float | None = None, retry_after_s: float = 0.0) -> None:
        """Share one SEC response status with every worker on the host.

        A 403/429 starts or extends a host-wide pause. A request granted after the latest recorded block
        opens a new block episode: the count goes up and the pause is ``block_pause_s`` doubled per
        consecutive episode (at least SEC's ``Retry-After``), capped at ``max_block_pause_s``. A block on a
        request granted before that (already in flight when the block was recorded) belongs to the same
        episode and does not escalate. Any other status on a request granted after the latest block ends the
        run. The ``abort_after``-th consecutive episode trips the host. An unreadable state is never
        overwritten (fail closed: it may hold a pause and the count, and :meth:`acquire` grants nothing
        while it stays unreadable).
        """

        is_block = int(status) in SEC_BLOCK_STATUS_CODES
        with self._thread_lock, _exclusive_file_lock(self.lock_path):
            now = time.time()
            state = self._read_state()
            if state is None:
                self._log({"t": now, "pid": os.getpid(), "event": "state_unreadable", "status": int(status)})
                return
            new_episode = granted_at is None or granted_at >= state.block_at
            if not is_block:
                if state.blocks and new_episode:
                    self._log({"t": now, "pid": os.getpid(), "event": "block_run_ended", "status": int(status),
                               "after_blocks": state.blocks})
                    state.blocks = 0
                    self._write_state(state)
                return
            retry_after = min(max(float(retry_after_s or 0.0), 0.0), self.max_block_pause_s)
            if new_episode:
                state.blocks += 1
                state.block_at = now
                backoff = self.block_pause_s * (2 ** min(state.blocks - 1, 16))
                pause = min(max(backoff, retry_after), self.max_block_pause_s)
            else:
                pause = retry_after
            if now + pause > state.pause_until:
                state.pause_until, state.pause_s = now + pause, pause
            self._write_state(state)
            self._log({"t": now, "pid": os.getpid(), "event": "block", "status": int(status),
                       "episode": "new" if new_episode else "in_flight", "blocks": state.blocks,
                       "pause_until": state.pause_until})
            if new_episode and state.blocks >= self.abort_after:
                self._trip(state, int(status), now)

    def preflight(self) -> dict[str, Any]:
        """Take the host lock once and report the limiter state; raises SecBlockedError on a tripped host.

        Fetch workers call this before any work, so an unreachable host lock, a tripped host or an
        unreadable limiter state (:class:`SecLimiterStateError`) stops them at start instead of mid-run.
        """

        with self._thread_lock, _exclusive_file_lock(self.lock_path):
            self._raise_if_tripped()
            status = self._status_locked()
            if status["state"] != "ok":
                raise self._state_error(0.0)
            return status

    def status(self) -> dict[str, Any]:
        with self._thread_lock, _exclusive_file_lock(self.lock_path):
            return self._status_locked()

    def clear_block(self) -> Path | None:
        """Operator reset after SEC access was checked: keep the trip marker as ``.cleared-<stamp>``.

        An unreadable limiter state is also reset: the file is kept aside as ``.unreadable-<stamp>``.
        Returns the kept marker (else the kept state file), or None when there was nothing to clear.
        """

        with self._thread_lock, _exclusive_file_lock(self.lock_path):
            state = self._read_state()
            if not self.blocked_path.exists() and state is not None:
                return None
            stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%SZ")
            cleared: Path | None = None
            if self.blocked_path.exists():
                cleared = self.blocked_path.with_name(f"{self.blocked_path.name}.cleared-{stamp}")
                os.replace(self.blocked_path, cleared)
            if state is None:
                kept_state = self.state_path.with_name(f"{self.state_path.name}.unreadable-{stamp}")
                os.replace(self.state_path, kept_state)
                cleared = cleared or kept_state
                state = _LimiterState(next_slot=time.time() + self.interval_s)
            state.blocks, state.pause_until, state.pause_s = 0, 0.0, 0.0
            self._write_state(state)
            self._log({"t": time.time(), "pid": os.getpid(), "event": "block_cleared", "marker": str(cleared)})
            return cleared

    def _status_locked(self) -> dict[str, Any]:
        now = time.time()
        state = self._read_state()
        return {
            "lock_path": str(self.lock_path),
            "rate_per_s": self.rate_per_s,
            "state": "ok" if state is not None else "unreadable",
            "paused_s": None if state is None else round(max(state.pause_until - now, 0.0), 3),
            "consecutive_block_episodes": None if state is None else state.blocks,
            "abort_after": self.abort_after,
            "tripped": self._tripped_detail(),
        }

    def _state_error(self, unreadable_s: float) -> SecLimiterStateError:
        return SecLimiterStateError(
            f"host SEC limiter state {self.state_path} is unreadable (for {unreadable_s:.0f} s); it may hold a "
            f"block pause, so SEC requests on this host stop (fail closed). Check the file, then run "
            f"`{CLEAR_BLOCK_COMMAND}` (it keeps the unreadable file aside and resets the state)."
        )

    def _tripped_detail(self) -> str | None:
        if not self.blocked_path.exists():
            return None
        try:
            return self.blocked_path.read_text(encoding="utf-8")[:500] or "empty trip marker"
        except OSError:
            return "unreadable trip marker"

    def _raise_if_tripped(self) -> None:
        detail = self._tripped_detail()
        if detail is not None:
            raise SecBlockedError(
                f"SEC refused this host {self.abort_after} consecutive times (403/429); every SEC request on the "
                f"host is stopped. Check SEC access, then run `{CLEAR_BLOCK_COMMAND}`. Marker "
                f"{self.blocked_path}: {detail}"
            )

    def _trip(self, state: _LimiterState, status: int, now: float) -> None:
        marker = {
            "tripped_at": dt.datetime.fromtimestamp(now, dt.UTC).isoformat(timespec="seconds"),
            "pid": os.getpid(),
            "consecutive_block_episodes": state.blocks,
            "last_status": status,
            "clear_with": CLEAR_BLOCK_COMMAND,
        }
        temporary = self.blocked_path.with_name(f"{self.blocked_path.name}.{os.getpid()}.tmp")
        temporary.write_text(json.dumps(marker, sort_keys=True), encoding="utf-8")
        os.replace(temporary, self.blocked_path)
        self._log({"t": now, "pid": os.getpid(), "event": "trip", "blocks": state.blocks, "status": status})

    def _read_state(self) -> _LimiterState | None:
        """The shared state; None when the file exists but stays unreadable (callers then fail closed).

        A missing file is a fresh host. An unreadable or unparsable file is re-read a few times (writes are
        atomic, so this is a scanner holding it or a storage fault). If it stays unreadable the callers
        neither grant nor overwrite it, so a read failure can never end a block pause or reset the count.
        """

        for attempt in range(_STATE_READ_ATTEMPTS):
            if attempt:
                time.sleep(_STATE_READ_RETRY_SECONDS)
            try:
                text = self.state_path.read_text(encoding="ascii")
            except FileNotFoundError:
                return _LimiterState()
            except (OSError, UnicodeDecodeError):
                continue
            try:
                payload = json.loads(text)
                if isinstance(payload, (int, float)):  # the pre-fix format: the next slot only
                    return _LimiterState(next_slot=float(payload))
                return _LimiterState(
                    next_slot=float(payload["next"]),
                    pause_until=float(payload.get("pause_until", 0.0)),
                    pause_s=float(payload.get("pause_s", 0.0)),
                    blocks=int(payload.get("blocks", 0)),
                    block_at=float(payload.get("block_at", 0.0)),
                )
            except (ValueError, KeyError, TypeError, AttributeError):
                continue
        return None

    def _write_state(self, state: _LimiterState) -> None:
        """Replace the state file whole (a kill never leaves a torn state); only ever called under the lock."""

        text = json.dumps({
            "next": state.next_slot, "pause_until": state.pause_until, "pause_s": state.pause_s,
            "blocks": state.blocks, "block_at": state.block_at,
        })
        temporary = self.state_path.with_name(f"{self.state_path.name}.{os.getpid()}.tmp")
        temporary.write_text(text, encoding="ascii")
        for _ in range(50):
            try:
                os.replace(temporary, self.state_path)
                return
            except PermissionError:  # a scanner briefly holding the target (Windows)
                time.sleep(_LOCK_POLL_SECONDS)
        os.replace(temporary, self.state_path)

    @staticmethod
    def _log(event: dict[str, Any]) -> None:
        log_path = os.environ.get(SEC_RATE_LOG_ENV)
        if log_path:
            with open(log_path, "a", encoding="utf-8") as handle:
                handle.write(json.dumps(event) + "\n")


_DEFAULT_LIMITER: SecRateLimiter | None = None
_DEFAULT_LIMITER_GUARD = threading.Lock()


def default_sec_limiter() -> SecRateLimiter:
    """The process-wide handle on the host-wide 5 req/s bucket."""

    global _DEFAULT_LIMITER
    with _DEFAULT_LIMITER_GUARD:
        if _DEFAULT_LIMITER is None:
            _DEFAULT_LIMITER = SecRateLimiter()
        return _DEFAULT_LIMITER


def _retry_after_seconds(response: Any, cap: float = _MAX_BACKOFF_SECONDS) -> float:
    value = (response.headers or {}).get("Retry-After") if hasattr(response, "headers") else None
    if not value:
        return 0.0
    value = str(value).strip()
    if value.isdigit():
        return min(float(value), cap)
    try:
        when = email.utils.parsedate_to_datetime(value)
    except (TypeError, ValueError):
        return 0.0
    if when.tzinfo is None:
        when = when.replace(tzinfo=dt.UTC)
    return min(max((when - dt.datetime.now(dt.UTC)).total_seconds(), 0.0), cap)


class _SecAdapter(HTTPAdapter):
    """Every attempt (first try, retry, redirect hop) takes one token from the shared limiter.

    Every SEC response status is shared with the limiter, so an SEC 403/429 pauses all workers on the host;
    the retry of a blocked request waits out that shared pause inside ``acquire`` (no local sleep). Statuses
    from other hosts sent through the same session (FASB, XBRL US) never reach the SEC block state: their 403
    is returned, their 429 is retried with local backoff.
    """

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
        sec_host = is_sec_url(request.url or "")
        attempt = 0
        while True:
            attempt += 1
            granted = self.limiter.acquire(request.url)
            try:
                response = super().send(request, **kwargs)
            except (requests.ConnectionError, requests.Timeout):
                if not idempotent or attempt >= self.max_attempts:
                    raise
                time.sleep(self._backoff(attempt))
                continue
            if sec_host:  # a non-SEC status must neither pause/trip SEC work nor end an SEC block run
                self.limiter.record_response(
                    response.status_code,
                    granted_at=granted,
                    retry_after_s=_retry_after_seconds(response, cap=SEC_MAX_BLOCK_PAUSE_SECONDS),
                )
            if sec_host and response.status_code in SEC_BLOCK_STATUS_CODES:
                if idempotent and attempt < self.max_attempts:
                    response.close()
                    continue  # the next acquire waits out the host-wide pause (or raises once tripped)
                return response
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


_SESSIONS: dict[tuple[int, int], tuple[SecRateLimiter, requests.Session]] = {}
_SESSIONS_GUARD = threading.Lock()


def _session_for(limiter: SecRateLimiter) -> requests.Session:
    """One session per (limiter, thread): fetch threads share the limiter, never a ``requests.Session``."""

    key = (id(limiter), threading.get_ident())
    with _SESSIONS_GUARD:
        cached = _SESSIONS.get(key)
        if cached is None or cached[0] is not limiter:
            cached = (limiter, sec_session(limiter=limiter))
            _SESSIONS[key] = cached
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


class _InFlight:
    """One URL being fetched by one thread; other threads asking for it wait for its record."""

    __slots__ = ("done", "record")

    def __init__(self) -> None:
        self.done = threading.Event()
        self.record: FetchRecord | None = None


class FetchLedgerStore:
    """Content-addressed fetch output plus an append-only ``fetch-ledger.jsonl`` (fetch/load split).

    Only a URL's newest *terminal* record counts; a rerun skips every URL that has one whose object is
    present at its ledgered (raw) size. Objects are stored gzip-compressed (``objects/<sha[:2]>/<sha256>.gz``,
    ruling C-71; sha256 and ``bytes`` are over the raw bytes, and the gzip trailer's ISIZE gives the raw size
    without decompressing); a plain ``<sha256>`` file written before C-71 is still read until
    :meth:`compress_raw_objects` converts it. Payload bytes are written (temp file, fsync, atomic replace)
    before their ledger line, so a kill leaves at most an unreferenced object, never a ledger line without its
    bytes. A read whose SHA does not match quarantines the object (``<name>.corrupt-<stamp>``) so the next
    :meth:`ensure` refetches it. Appends are serialized by a file lock across processes and by a mutex across
    threads; threads (ruling C-72) may share one store, and one URL is fetched by one thread at a time.

    The in-memory index keeps a 16-byte URL digest per terminal record (not the URL text). Measured at 100k
    entries (review of 1.7): about 266 B retained and 309 B peak per entry, i.e. about 216 / 250 MB at 850k
    URLs; a loader that holds this index next to DuckDB must budget for it (or shard the ledger).
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
        self._append_mutex = threading.Lock()
        self._object_mutex = threading.Lock()
        self._inflight_mutex = threading.Lock()
        self._inflight: dict[bytes, _InFlight] = {}

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
        """The object at rest: ``<sha256>.gz`` (gzip of the raw bytes, ruling C-71)."""

        return self.objects_dir / sha256[:2] / f"{sha256}{OBJECT_SUFFIX}"

    def raw_object_path(self, sha256: str) -> Path:
        """A pre-C-71 uncompressed object, read until :meth:`compress_raw_objects` converts it."""

        return self.objects_dir / sha256[:2] / sha256

    def read(self, record: FetchRecord, *, maximum: int | None = None) -> bytes:
        """The verified raw bytes of a successful record (decompressed); ValueError on a SHA mismatch.

        FileNotFoundError when no object is stored for it.
        """

        if not record.ok or record.sha256 is None:
            raise ValueError(f"no content was fetched for {record.url}")
        path, payload = self._load_object(record.sha256)
        if hashlib.sha256(payload).hexdigest() != record.sha256:
            self._quarantine(path)
            raise ValueError(FETCH_OBJECT_SHA_MISMATCH)
        if maximum is not None and len(payload) > maximum:
            raise ValueError(RESPONSE_TOO_LARGE)
        return payload

    def _load_object(self, sha256: str) -> tuple[Path, bytes]:
        """``(path, raw bytes)``: the compressed object, else a pre-C-71 raw one.

        The compressed path is tried again last, because a conversion pass may replace the raw file between the
        two reads. A compressed file that does not decompress yields empty bytes, so the caller's SHA check fails
        and quarantines it.
        """

        compressed, raw = self.object_path(sha256), self.raw_object_path(sha256)
        for path in (compressed, raw, compressed):
            try:
                stored = path.read_bytes()
            except FileNotFoundError:
                continue
            if path == raw:
                return path, stored
            try:
                return path, gzip.decompress(stored)
            except (OSError, EOFError, zlib.error):
                return path, b""
        raise FileNotFoundError(str(compressed))

    def _stored_raw_size(self, sha256: str) -> int | None:
        """The raw size of the stored object (gzip ISIZE, exact below 4 GiB, or the raw file's size); None if absent."""

        try:
            with self.object_path(sha256).open("rb") as handle:
                handle.seek(-4, os.SEEK_END)
                return int.from_bytes(handle.read(4), "little")
        except FileNotFoundError:
            pass
        except OSError:
            return None  # shorter than a gzip trailer: a torn file, refetched
        try:
            return self.raw_object_path(sha256).stat().st_size
        except OSError:
            return None

    def _quarantine(self, path: Path) -> None:
        """Move a corrupt object aside (kept, not deleted) so the next ensure() refetches its URL."""

        stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%S%fZ")
        # Another process may hold it open or have moved it first: the caller still raises, a later read retries.
        with suppress(OSError):
            os.replace(path, path.with_name(f"{path.name}.corrupt-{stamp}"))

    def _object_present(self, record: FetchRecord) -> bool:
        if not record.ok or record.sha256 is None:
            return True  # a terminal failure has no object
        return self._stored_raw_size(record.sha256) == record.bytes

    def ensure(
        self,
        url: str,
        *,
        limiter: SecRateLimiter | None = None,
        timeout: float = 30.0,
        maximum: int | None = None,
    ) -> tuple[FetchRecord, bool]:
        """Return ``(record, fetched_now)``: the ledgered terminal record, else one new (ledgered) fetch.

        A successful record whose object is missing or not at its ledgered size (for example quarantined by
        :meth:`read`) is refetched. Thread-safe: while one thread fetches a URL, other threads asking for it
        wait and get that thread's record, so a URL is never requested twice at once.
        """

        key = self._key(url)
        while True:
            record = self.lookup(url)
            if record is not None and self._object_present(record):
                return record, False
            with self._inflight_mutex:
                flight = self._inflight.get(key)
                if flight is None:
                    # Re-check under the mutex: an owner remembers its record before it leaves the in-flight map.
                    record = self.lookup(url)
                    if record is not None and self._object_present(record):
                        return record, False
                    flight = self._inflight[key] = _InFlight()
                    break
            flight.done.wait()
            if flight.record is not None:
                return flight.record, False
            # the owning thread raised: try again (as the owner, unless another thread took over first)
        try:
            flight.record = self.fetch(url, limiter=limiter, timeout=timeout, maximum=maximum)
            return flight.record, True
        finally:
            with self._inflight_mutex:
                self._inflight.pop(key, None)
            flight.done.set()

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
        limiter = limiter or default_sec_limiter()
        session = _session_for(limiter)
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
        self._append(record, rate_lock=str(getattr(limiter, "lock_path", "")))
        if record.terminal:
            self._remember(self._key(url), record)
        return record

    def _compressed_verifies(self, path: Path, sha256: str) -> bool:
        try:
            return hashlib.sha256(gzip.decompress(path.read_bytes())).hexdigest() == sha256
        except (OSError, EOFError, zlib.error):
            return False

    def _write_object(self, sha256: str, payload: bytes) -> None:
        """Store ``payload`` (whose raw sha256 is ``sha256``) as ``<sha256>.gz``: temp file, fsync, atomic replace."""

        compressed = gzip.compress(payload, compresslevel=OBJECT_GZIP_LEVEL, mtime=0)
        path = self.object_path(sha256)
        with self._object_mutex:  # threads storing identical bytes under different URLs
            if path.is_file():
                if self._compressed_verifies(path, sha256):
                    return  # same content already stored (identical bytes under another URL, or a resumed write)
                self._quarantine(path)  # does not verify: keep it aside and store the verified payload
            path.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                mode="wb", dir=path.parent, prefix=f".{sha256[:16]}.", suffix=".tmp", delete=False
            ) as handle:
                temporary = Path(handle.name)
                handle.write(compressed)
                handle.flush()
                os.fsync(handle.fileno())
            try:
                os.replace(temporary, path)
            finally:
                temporary.unlink(missing_ok=True)

    def compress_raw_objects(self, *, limit: int | None = None) -> dict[str, int]:
        """Resumable C-71 conversion: every pre-C-71 raw object becomes a verified ``<sha256>.gz``.

        Per raw file ``<sha256>``: its bytes must hash to its name (else it is quarantined, kept, and counted);
        the compressed object is written atomically, read back, decompressed and verified against that same raw
        sha256, and only then is the raw file removed. A kill at any point leaves the raw file, or both files
        (the next pass verifies the compressed one and removes the raw), never neither. ``limit`` stops after
        that many raw files (for a kill-and-resume drill). Safe beside a running fetch worker, whose readers
        prefer the compressed file and re-try it after a raw file disappears.
        """

        counts = {"raw_seen": 0, "converted": 0, "already_compressed": 0, "raw_sha_mismatch": 0,
                  "raw_busy": 0, "raw_bytes": 0, "compressed_bytes": 0}
        if not self.objects_dir.is_dir():
            return counts
        hexdigits = set("0123456789abcdef")
        for bucket in sorted(p for p in self.objects_dir.iterdir() if p.is_dir()):
            for raw_path in sorted(bucket.iterdir()):
                name = raw_path.name
                if len(name) != 64 or not set(name) <= hexdigits or not raw_path.is_file():
                    continue
                if limit is not None and counts["raw_seen"] >= limit:
                    return counts
                counts["raw_seen"] += 1
                payload = raw_path.read_bytes()
                if hashlib.sha256(payload).hexdigest() != name:
                    self._quarantine(raw_path)
                    counts["raw_sha_mismatch"] += 1
                    continue
                target = self.object_path(name)
                already = target.is_file() and self._compressed_verifies(target, name)
                if not already:
                    self._write_object(name, payload)
                    if not self._compressed_verifies(target, name):
                        raise RuntimeError(f"compressed object {target} does not verify after writing; raw kept")
                counts["already_compressed" if already else "converted"] += 1
                counts["raw_bytes"] += len(payload)
                counts["compressed_bytes"] += target.stat().st_size
                try:
                    raw_path.unlink()
                except PermissionError:
                    counts["raw_busy"] += 1  # a reader holds it open: the next pass removes it
        return counts

    def _append(self, record: FetchRecord, *, rate_lock: str | None = None) -> None:
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
                "rate_lock": rate_lock,
            },
            sort_keys=True,
        ).encode("utf-8") + b"\n"
        self.root.mkdir(parents=True, exist_ok=True)
        # The mutex serializes this process's threads (a byte-range lock is per handle); the file lock, processes.
        with self._append_mutex, _exclusive_file_lock(self._lock_path), self.ledger_path.open("ab+") as handle:
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


def main(argv: list[str] | None = None) -> int:
    """``python -m atx_db.sec_http --status | --clear-block``: inspect or reset the host-wide SEC limiter."""

    import argparse

    parser = argparse.ArgumentParser(prog="python -m atx_db.sec_http", description=main.__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--status", action="store_true", help="Print the host lock path, pause and trip state.")
    group.add_argument(
        "--clear-block", action="store_true",
        help="Operator reset after SEC access was checked; the trip marker is kept as .cleared-<stamp>.",
    )
    args = parser.parse_args(argv)
    limiter = default_sec_limiter()
    if args.clear_block:
        cleared = limiter.clear_block()
        print(json.dumps({"cleared": str(cleared) if cleared else None, **limiter.status()}, sort_keys=True))
    else:
        print(json.dumps(limiter.status(), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
