"""Trial registry: wave pre-registration under qualification policy v4 (tier-1 v2 node 1.10, ruling R-6).

A *wave* is one pre-registered batch of catalog hypotheses (``catalog.WAVES``). Before a
wave reads any forward return, :func:`register_wave` freezes

* the catalog digest the wave's rows come from and the frozen policy v4 hash (a policy
  hash that is not pinned in ``qualification.FROZEN_POLICY_SHA256`` for a v4+ policy is
  refused),
* the wave's hypotheses (``feature_ids``), which of them are reported only (the policy's
  ruling C-24 equal-weight twins are added automatically; nothing reported-only is ever
  gated) and each feature's first history-eligible formation (optional; the coverage gate's
  formation basis),
* every configuration the wave will evaluate (``cells``: ``(feature_id, variant,
  horizon_months)``; the primary ``rank_normal`` x 3-month cell of every feature must be
  among them).

The registry is cumulative across waves (policy v4 ``trial_registry``):
:func:`trials_so_far` is the number of registered configurations, the deflated Sharpe
``n_trials``; :meth:`TrialRegistry.gating_hypotheses_before` is the number of gating
hypotheses of earlier waves, which enter the gating Benjamini-Hochberg family of a later
wave as p = 1. A wave's holdout is opened exactly once and only on final labels
(:func:`open_holdout`); while it is not opened, every grade of the wave is a selection-sample
grade and the holdout formations stay sealed.

Storage
-------
One append-only JSON-lines file ``<root>/trial_registry.jsonl`` (root default
``<data dir>/research/registry``, or ``$ATX_RESEARCH_REGISTRY_DIR``). Every record carries
its sequence number, the sha256 of the previous record and its own sha256 (canonical JSON
of the record without it), so an edited, dropped or reordered line is refused on read. A
lock file (``trial_registry.lock``, exclusive create) serializes writers; a writer never
rewrites a line. Re-registering a wave with identical content returns the original
registration id; any difference (a changed catalog digest, policy, feature list, reported-only
list or cell list) is refused -- a changed wave needs a new wave id.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import re
import time
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REGISTRY_VERSION = "trial-registry-v1"
REGISTRY_FILE = "trial_registry.jsonl"
LOCK_FILE = "trial_registry.lock"
REGISTRY_DIR_ENV = "ATX_RESEARCH_REGISTRY_DIR"
#: The policy v4 primary cell every registered feature must carry.
PRIMARY_VARIANT, PRIMARY_HORIZON = "rank_normal", 3
_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_SHA = re.compile(r"^[0-9a-f]{64}$")
_LOCK_TIMEOUT_S = 30.0

__all__ = [
    "REGISTRY_VERSION",
    "RegistryError",
    "TrialRegistry",
    "WaveRegistration",
    "default_registry_dir",
    "open_holdout",
    "register_wave",
    "trials_so_far",
]


class RegistryError(ValueError):
    """A refused registration or holdout opening, or a registry file that fails its hash chain."""


def default_registry_dir() -> Path:
    """``$ATX_RESEARCH_REGISTRY_DIR``, else ``<data dir>/research/registry``."""
    configured = os.environ.get(REGISTRY_DIR_ENV)
    if configured:
        return Path(os.path.expandvars(configured)).expanduser()
    from ..connection import resolve_data_dir

    return resolve_data_dir() / "research" / "registry"


def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False)


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _now() -> str:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None).isoformat(timespec="seconds")


@dataclass(frozen=True)
class WaveRegistration:
    """One frozen wave (see the module docstring)."""

    wave: str
    sequence: int
    registration_id: str
    catalog_digest: str
    policy_id: str
    policy_sha: str
    feature_ids: tuple[str, ...]
    reported_only: tuple[str, ...]
    gating_feature_ids: tuple[str, ...]
    cells: tuple[tuple[str, str, int], ...]
    first_formations: Mapping[str, str]
    registered_at: str

    @property
    def n_cells(self) -> int:
        return len(self.cells)


def _accepted_policy(policy_sha: str) -> str:
    """The pinned v4+ policy id of ``policy_sha`` (a wave is registered only under a frozen v4 policy)."""
    from .qualification import FROZEN_POLICY_SHA256, V4_POLICY_IDS

    for policy_id, pinned in FROZEN_POLICY_SHA256.items():
        if pinned == policy_sha and policy_id in V4_POLICY_IDS:
            return policy_id
    raise RegistryError(f"policy sha256 {policy_sha} is not a frozen v4 qualification policy "
                        f"(pinned v4+: {sorted(V4_POLICY_IDS)}); freeze the policy before registering a wave")


def _reported_only_rule(policy_id: str) -> tuple[str, ...]:
    from .qualification import load_policy_v4_by_id

    return load_policy_v4_by_id(policy_id).reported_only


def _registration(record: Mapping[str, Any]) -> WaveRegistration:
    return WaveRegistration(
        wave=str(record["wave"]), sequence=int(record["sequence"]), registration_id=str(record["record_sha"]),
        catalog_digest=str(record["catalog_digest"]), policy_id=str(record["policy_id"]),
        policy_sha=str(record["policy_sha"]), feature_ids=tuple(record["feature_ids"]),
        reported_only=tuple(record["reported_only"]), gating_feature_ids=tuple(record["gating_feature_ids"]),
        cells=tuple((str(f), str(v), int(h)) for f, v, h in record["cells"]),
        first_formations=dict(record.get("first_formations") or {}), registered_at=str(record["registered_at"]))


class TrialRegistry:
    """The append-only, hash-chained registry file under ``root``."""

    def __init__(self, root: Path | str | None = None) -> None:
        self.root = Path(root) if root is not None else default_registry_dir()
        self.path = self.root / REGISTRY_FILE
        self.lock_path = self.root / LOCK_FILE

    # -- reading -------------------------------------------------------------

    def records(self) -> list[dict[str, Any]]:
        """Every record in order, after checking the sequence and the hash chain."""
        if not self.path.is_file():
            return []
        records: list[dict[str, Any]] = []
        previous = None
        with self.path.open("r", encoding="utf-8") as handle:
            for number, line in enumerate(handle, start=1):
                if not line.strip():
                    raise RegistryError(f"{self.path}:{number}: blank line in the append-only registry")
                record = json.loads(line)
                claimed = record.get("record_sha")
                body = {key: value for key, value in record.items() if key != "record_sha"}
                if claimed != _sha(_canonical(body)):
                    raise RegistryError(f"{self.path}:{number}: record hash does not match its content (edited)")
                if record.get("sequence") != len(records) or record.get("prev_record_sha") != previous:
                    raise RegistryError(f"{self.path}:{number}: sequence or hash chain broken (dropped or "
                                        "reordered line)")
                records.append(record)
                previous = claimed
        return records

    def registrations(self) -> list[WaveRegistration]:
        return [_registration(r) for r in self.records() if r["event"] == "register_wave"]

    def registration(self, wave: str) -> WaveRegistration | None:
        for item in self.registrations():
            if item.wave == wave:
                return item
        return None

    def require_registration(self, wave: str, *, catalog_digest: str | None = None,
                             policy_sha: str | None = None) -> WaveRegistration:
        """The wave's registration; refuses an unregistered wave or a digest/policy that differs.

        A wave runner calls this before its first label join (R-6: no return is read before
        the wave's catalog rows and the policy hash are frozen).
        """
        item = self.registration(wave)
        if item is None:
            raise RegistryError(f"wave {wave!r} is not registered: register_wave before reading any forward return")
        if catalog_digest is not None and catalog_digest != item.catalog_digest:
            raise RegistryError(f"wave {wave!r} was registered with catalog digest {item.catalog_digest}, "
                                f"not {catalog_digest}")
        if policy_sha is not None and policy_sha != item.policy_sha:
            raise RegistryError(f"wave {wave!r} was registered under policy {item.policy_sha}, not {policy_sha}")
        return item

    def trials_so_far(self) -> int:
        """Registered configurations across every wave (the DSR ``n_trials``)."""
        return sum(item.n_cells for item in self.registrations())

    def gating_hypotheses_before(self, wave: str) -> int:
        """Gating hypotheses of the waves registered before ``wave`` (all waves if unregistered)."""
        items = self.registrations()
        own = next((item.sequence for item in items if item.wave == wave), None)
        return sum(len(item.gating_feature_ids) for item in items if own is None or item.sequence < own)

    def holdout_opening(self, wave: str) -> dict[str, Any] | None:
        for record in self.records():
            if record["event"] == "open_holdout" and record["wave"] == wave:
                return record
        return None

    def holdout_opened(self, wave: str) -> bool:
        return self.holdout_opening(wave) is not None

    # -- writing -------------------------------------------------------------

    def _append(self, body: dict[str, Any], records: Sequence[Mapping[str, Any]]) -> str:
        body = {**body, "registry_version": REGISTRY_VERSION, "sequence": len(records),
                "prev_record_sha": records[-1]["record_sha"] if records else None}
        record_sha = _sha(_canonical(body))
        line = _canonical({**body, "record_sha": record_sha}) + "\n"
        with self.path.open("a", encoding="utf-8", newline="\n") as handle:
            handle.write(line)
            handle.flush()
            os.fsync(handle.fileno())
        return record_sha

    def _locked(self) -> _Lock:
        self.root.mkdir(parents=True, exist_ok=True)
        return _Lock(self.lock_path)

    def register_wave(self, wave: str, catalog_digest: str, policy_sha: str, feature_ids: Sequence[str],
                      cells: Sequence[tuple[str, str, int]], *, reported_only: Iterable[str] = (),
                      first_formations: Mapping[str, str] | None = None) -> str:
        """Freeze a wave before it reads any return; returns the registration id (record sha256)."""
        if not isinstance(wave, str) or not _ID.fullmatch(wave):
            raise RegistryError("wave must be a lower-case identifier")
        for name, value in (("catalog_digest", catalog_digest), ("policy_sha", policy_sha)):
            if not isinstance(value, str) or not _SHA.fullmatch(value):
                raise RegistryError(f"{name} must be a sha256 hex digest")
        policy_id = _accepted_policy(policy_sha)
        features = tuple(sorted(feature_ids))
        if not features or len(set(features)) != len(features) or any(not _ID.fullmatch(f) for f in features):
            raise RegistryError("feature_ids must be distinct lower-case identifiers")
        normalized: list[tuple[str, str, int]] = []
        for item in cells:
            if len(item) != 3:
                raise RegistryError("cells must be (feature_id, variant, horizon_months) triples")
            feature, variant, horizon = item
            if feature not in features or not isinstance(variant, str) or not _ID.fullmatch(variant) \
                    or isinstance(horizon, bool) or not isinstance(horizon, int) or not 1 <= horizon <= 12:
                raise RegistryError(f"bad cell {item!r}: a registered feature, a variant identifier, "
                                    "a horizon of 1..12 months")
            normalized.append((feature, variant, horizon))
        cell_set = sorted(set(normalized))
        if len(cell_set) != len(normalized):
            raise RegistryError("cells must be distinct")
        missing = [f for f in features if (f, PRIMARY_VARIANT, PRIMARY_HORIZON) not in set(cell_set)]
        if missing:
            raise RegistryError(f"every feature needs its primary cell ({PRIMARY_VARIANT}, {PRIMARY_HORIZON}m); "
                                f"missing for {missing}")
        extra = set(reported_only)
        if extra - set(features):
            raise RegistryError(f"reported_only names unregistered features {sorted(extra - set(features))}")
        reported = tuple(sorted(extra | (set(_reported_only_rule(policy_id)) & set(features))))
        gating = tuple(f for f in features if f not in reported)
        formations = {str(k): dt.date.fromisoformat(str(v)).isoformat() for k, v in (first_formations or {}).items()}
        if set(formations) - set(features):
            raise RegistryError(f"first_formations names unregistered features {sorted(set(formations) - set(features))}")
        content = {"event": "register_wave", "wave": wave, "catalog_digest": catalog_digest, "policy_id": policy_id,
                   "policy_sha": policy_sha, "feature_ids": list(features), "reported_only": list(reported),
                   "gating_feature_ids": list(gating), "cells": [list(c) for c in cell_set], "n_cells": len(cell_set),
                   "first_formations": dict(sorted(formations.items()))}
        with self._locked():
            records = self.records()
            for record in records:
                if record["event"] == "register_wave" and record["wave"] == wave:
                    stored = {key: record.get(key) for key in content}
                    if stored == content:
                        return str(record["record_sha"])
                    changed = sorted(key for key in content if stored.get(key) != content[key])
                    raise RegistryError(f"wave {wave!r} is registered (catalog digest {record['catalog_digest']}); "
                                        f"refusing a re-registration that changes {changed}: a changed wave "
                                        "needs a new wave id")
            return self._append({**content, "registered_at": _now()}, records)

    def open_holdout(self, wave: str, *, final_labels: bool = False, label_sha: str | None = None) -> None:
        """Record the wave's one holdout opening (policy v4: once per wave, final labels only)."""
        if not final_labels:
            raise RegistryError(f"wave {wave!r}: the holdout opens only on final labels (policy v4 "
                                "holdout.requires_final_labels); provisional grades stay on the selection sample")
        if label_sha is not None and not _SHA.fullmatch(str(label_sha)):
            raise RegistryError("label_sha must be a sha256 hex digest")
        with self._locked():
            records = self.records()
            if not any(r["event"] == "register_wave" and r["wave"] == wave for r in records):
                raise RegistryError(f"wave {wave!r} is not registered")
            if any(r["event"] == "open_holdout" and r["wave"] == wave for r in records):
                raise RegistryError(f"wave {wave!r}: the holdout was already opened (policy v4: once per wave)")
            self._append({"event": "open_holdout", "wave": wave, "label_sha": label_sha, "final_labels": True,
                          "opened_at": _now()}, records)


class _Lock:
    """Exclusive-create lock file; a stale lock is reported, never broken silently."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.fd: int | None = None

    def __enter__(self) -> _Lock:
        deadline = time.monotonic() + _LOCK_TIMEOUT_S
        while True:
            try:
                self.fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(self.fd, f"pid={os.getpid()} at={_now()}\n".encode("ascii"))
                return self
            except FileExistsError:
                if time.monotonic() > deadline:
                    raise RegistryError(f"registry lock {self.path} held for > {_LOCK_TIMEOUT_S:.0f} s; if no "
                                        "writer is running, inspect and remove it by hand") from None
                time.sleep(0.2)

    def __exit__(self, *exc: object) -> None:
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
        self.path.unlink(missing_ok=True)


def register_wave(wave: str, catalog_digest: str, policy_sha: str, feature_ids: Sequence[str],
                  cells: Sequence[tuple[str, str, int]], *, reported_only: Iterable[str] = (),
                  first_formations: Mapping[str, str] | None = None, root: Path | str | None = None) -> str:
    """Freeze a wave in the default registry (see :meth:`TrialRegistry.register_wave`)."""
    return TrialRegistry(root).register_wave(wave, catalog_digest, policy_sha, feature_ids, cells,
                                             reported_only=reported_only, first_formations=first_formations)


def trials_so_far(root: Path | str | None = None) -> int:
    """Registered configurations across every wave (feeds the DSR ``n_trials``)."""
    return TrialRegistry(root).trials_so_far()


def open_holdout(wave: str, *, final_labels: bool = False, label_sha: str | None = None,
                 root: Path | str | None = None) -> None:
    """Open ``wave``'s holdout once, on final labels (see :meth:`TrialRegistry.open_holdout`)."""
    TrialRegistry(root).open_holdout(wave, final_labels=final_labels, label_sha=label_sha)
