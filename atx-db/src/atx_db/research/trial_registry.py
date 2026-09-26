"""Trial registry: wave pre-registration under qualification policy v4 (tier-1 v2 node 1.10, ruling R-6).

A *wave* is one pre-registered batch of catalog hypotheses (``catalog.WAVES``). Before a
wave reads any forward return, :func:`register_wave` freezes

* the catalog digest the wave's rows come from and the frozen policy v4 hash (a policy
  hash that is not pinned in ``qualification.FROZEN_POLICY_SHA256`` for a v4+ policy is
  refused),
* the wave's rows: per feature its expected sign, evidence class, population and a digest of
  the row's definition (:data:`ROW_DIGEST_FIELDS`, :func:`row_digest`). The rows must equal
  the supplied pre-registration table on ids and signs -- no row added, dropped or re-signed
  -- or the registration is refused,
* which hypotheses are reported only (the policy's ruling C-24 equal-weight twins are added
  automatically; nothing reported-only is ever gated) and each feature's first
  history-eligible formation (optional; the coverage gate's formation basis),
* every configuration the wave will evaluate (``cells``: ``(feature_id, variant,
  horizon_months)``; the primary ``rank_normal`` x 3-month cell of every feature must be
  among them).

The registry is cumulative across waves (policy v4 ``trial_registry``):
:func:`trials_so_far` is the number of registered configurations, the deflated Sharpe
``n_trials``; :meth:`TrialRegistry.gating_hypotheses_before` is the number of gating
hypotheses of earlier waves, which enter the gating Benjamini-Hochberg family of a later
wave as p = 1. A wave's holdout is opened exactly once, on final labels identified by their
label-set digest (:func:`open_holdout`, ``label_sha`` =
``evaluation.prepared_label_set_sha256`` of the final labels); while it is not opened, every
grade of the wave is a selection-sample grade and the holdout formations stay sealed.

Grading (``qualification.grade_wave_v4``) checks the registry's anchor
(:meth:`TrialRegistry.verify_anchor`), recomputes the registered rows' digests from the
catalog it is given (:func:`registered_row_problems`; scoped to the wave's own rows, so rows
that later waves add to the catalog never disturb an earlier wave's final grade), refuses
evaluated cells that were not registered, takes ``n_trials`` from the registered cells, and
refuses a run whose spec is not bound to the registration (order, R-6) or whose holdout
statistics lack a matching opening.

Storage and tamper evidence
---------------------------
One append-only JSON-lines file ``<root>/trial_registry.jsonl`` (root default
``<data dir>/research/registry``, or ``$ATX_RESEARCH_REGISTRY_DIR``). Every record carries
its sequence number, the sha256 of the previous record and its own sha256 (canonical JSON
of the record without it): an edited, reordered or dropped *interior* line breaks the chain
and is refused; a torn line (an interrupted write) or a file without its final newline is
refused with :class:`RegistryError` and a recovery note. A dropped *tail* leaves a valid
chain, so the head is anchored outside the file: after every write the registry appends
``(sequence, record_sha, event, wave)`` of the new record to its anchor file -- by default
the committed ``seeds/research_trial_registry_anchor.jsonl`` (``$ATX_RESEARCH_REGISTRY_ANCHOR``)
-- and the wave runner commits that file. :meth:`TrialRegistry.verify_anchor` reads the
anchor from git ``HEAD`` and refuses an empty or unanchored registry, a registry shorter than
its anchor, a record that differs from its anchor and records not yet anchored in a commit.
A scratch registry (explicit ``root``) anchors in ``<root>/trial_registry_anchor.jsonl``
(a file, not a commit); its grades carry ``registry_anchor_not_committed``.

A lock file (``trial_registry.lock``, exclusive create) serializes writers; a writer never
rewrites a line and refuses to append to a registry that is shorter than, or differs from,
its anchor. Re-registering a wave with identical content returns the original registration
id; any difference is refused -- a changed wave needs a new wave id.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import re
import subprocess
import time
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REGISTRY_VERSION = "trial-registry-v2"
REGISTRY_FILE = "trial_registry.jsonl"
LOCK_FILE = "trial_registry.lock"
ANCHOR_FILE = "trial_registry_anchor.jsonl"
REGISTRY_DIR_ENV = "ATX_RESEARCH_REGISTRY_DIR"
ANCHOR_ENV = "ATX_RESEARCH_REGISTRY_ANCHOR"
#: The committed anchor of the default registry (one line per registry record).
DEFAULT_ANCHOR_PATH = Path(__file__).resolve().parents[1] / "seeds" / "research_trial_registry_anchor.jsonl"
#: The policy v4 primary cell every registered feature must carry.
PRIMARY_VARIANT, PRIMARY_HORIZON = "rank_normal", 3
#: Catalog fields that define a registered row: its identity, sign, class, population, wave,
#: admission and every field that computes it. Prose (``economic_definition``,
#: ``sign_rationale``, ``reference``, ``admission_note``) and bookkeeping (``caveat_codes``,
#: ``supersedes``, ``publication_year``, ``jkp_theme``, ``scale_type``, ``prior_evidence``,
#: which ``evidence_class`` already carries) are left out, so a wording fix never breaks a grade.
ROW_DIGEST_FIELDS = (
    "feature_id", "wave", "expected_sign", "evidence_class", "population", "anomaly_class", "hypothesis_family",
    "source_kind", "metric_code", "metric_window", "numerator", "denominator", "domain", "availability_clock",
    "preferred_transform", "min_history_quarters", "min_history_sessions", "admission",
)
_ANCHOR_KEYS = frozenset({"event", "record_sha", "sequence", "wave"})
_ID = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_SHA = re.compile(r"^[0-9a-f]{64}$")
_LOCK_TIMEOUT_S = 30.0
_RECOVERY = ("Recovery: compare the file with its anchor (TrialRegistry.verify_anchor). Only a torn or unanchored "
             "final line left by an interrupted write may be removed by hand; an anchored line never. Then repeat "
             "the write.")

__all__ = [
    "ANCHOR_FILE",
    "DEFAULT_ANCHOR_PATH",
    "REGISTRY_VERSION",
    "ROW_DIGEST_FIELDS",
    "AnchorState",
    "RegistryError",
    "TrialRegistry",
    "WaveRegistration",
    "default_anchor_path",
    "default_registry_dir",
    "open_holdout",
    "register_wave",
    "registered_row_problems",
    "row_digest",
    "trials_so_far",
]


class RegistryError(ValueError):
    """A refused registration or holdout opening, or a registry that fails its chain or its anchor."""


def default_registry_dir() -> Path:
    """``$ATX_RESEARCH_REGISTRY_DIR``, else ``<data dir>/research/registry``."""
    configured = os.environ.get(REGISTRY_DIR_ENV)
    if configured:
        return Path(os.path.expandvars(configured)).expanduser()
    from ..connection import resolve_data_dir

    return resolve_data_dir() / "research" / "registry"


def default_anchor_path() -> Path:
    """``$ATX_RESEARCH_REGISTRY_ANCHOR``, else the committed seeds anchor."""
    configured = os.environ.get(ANCHOR_ENV)
    return Path(os.path.expandvars(configured)).expanduser() if configured else DEFAULT_ANCHOR_PATH


def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False)


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _now() -> str:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None).isoformat(timespec="seconds")


def _plain(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, (tuple, list)):
        return [_plain(item) for item in value]
    return str(value)


def row_digest(entry: Any) -> str:
    """sha256 of a catalog row's :data:`ROW_DIGEST_FIELDS` (canonical JSON; absent fields are null)."""
    return _sha(_canonical({name: _plain(getattr(entry, name, None)) for name in ROW_DIGEST_FIELDS}))


def _sign(value: Any, label: str) -> int:
    try:
        sign = int(str(value).strip()) if not isinstance(value, int) or isinstance(value, bool) else value
    except ValueError:
        raise RegistryError(f"{label}: sign {value!r} is not -1, 0 or +1") from None
    if sign not in (-1, 0, 1):
        raise RegistryError(f"{label}: sign {value!r} is not -1, 0 or +1")
    return sign


def _preregistration_table(table: Any) -> dict[str, int]:
    """``{feature_id: sign}`` from a mapping, ``(feature_id, sign)`` pairs or row mappings."""
    if table is None:
        raise RegistryError("a pre-registration table (feature_id -> expected sign) is required")
    items: list[tuple[Any, Any]] = []
    if isinstance(table, Mapping):
        items = list(table.items())
    else:
        for item in table:
            if isinstance(item, Mapping):
                items.append((item.get("feature_id"), item.get("expected_sign")))
            else:
                feature_id, sign = item
                items.append((feature_id, sign))
    out: dict[str, int] = {}
    for feature_id, sign in items:
        if not isinstance(feature_id, str) or not _ID.fullmatch(feature_id):
            raise RegistryError(f"pre-registration table: bad feature id {feature_id!r}")
        if feature_id in out:
            raise RegistryError(f"pre-registration table: {feature_id} listed twice")
        out[feature_id] = _sign(sign, f"pre-registration table {feature_id}")
    if not out:
        raise RegistryError("the pre-registration table is empty")
    return out


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
    rows: Mapping[str, Mapping[str, Any]]
    row_digest_fields: tuple[str, ...]
    preregistration_sha256: str | None
    registry_version: str

    @property
    def n_cells(self) -> int:
        return len(self.cells)


@dataclass(frozen=True)
class AnchorState:
    """What :meth:`TrialRegistry.verify_anchor` checked: the anchor's source and the anchored head."""

    source: str
    committed: bool
    anchored: int
    head_record_sha: str | None
    path: str

    def as_dict(self) -> dict[str, Any]:
        return {"source": self.source, "committed": self.committed, "anchored": self.anchored,
                "head_record_sha": self.head_record_sha, "path": self.path}


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
        first_formations=dict(record.get("first_formations") or {}), registered_at=str(record["registered_at"]),
        rows={str(k): dict(v) for k, v in (record.get("rows") or {}).items()},
        row_digest_fields=tuple(record.get("row_digest_fields") or ()),
        preregistration_sha256=record.get("preregistration_sha256"),
        registry_version=str(record.get("registry_version")))


def registered_row_problems(registration: WaveRegistration, catalog: Iterable[Any]) -> list[str]:
    """Why the catalog rows given to a grade are not the rows the wave registered (sorted; empty = same).

    Only the wave's registered rows are compared (other catalog rows are ignored): a row that is
    missing, re-signed, re-classed or whose definition digest changed is reported.
    """
    if not registration.rows:
        return ["registration_without_row_digests"]
    if tuple(registration.row_digest_fields) != ROW_DIGEST_FIELDS:
        return ["registration_row_digest_fields_differ_from_code"]
    by_id = {str(getattr(entry, "feature_id", "")): entry for entry in catalog}
    problems: list[str] = []
    for feature_id, stored in sorted(registration.rows.items()):
        entry = by_id.get(feature_id)
        if entry is None:
            problems.append(f"registered_row_missing:{feature_id}")
            continue
        try:
            sign: int | None = _sign(getattr(entry, "expected_sign", None), f"row {feature_id}")
        except RegistryError:
            sign = None
        if sign != stored.get("expected_sign"):
            problems.append(f"registered_row_resigned:{feature_id}")
        elif str(getattr(entry, "evidence_class", "")) != stored.get("evidence_class"):
            problems.append(f"registered_row_reclassed:{feature_id}")
        elif row_digest(entry) != stored.get("row_sha256"):
            problems.append(f"registered_row_definition_changed:{feature_id}")
    return sorted(problems)


def _git(args: Sequence[str], cwd: Path) -> str | None:
    try:
        done = subprocess.run(["git", *args], cwd=str(cwd), capture_output=True, text=True, encoding="utf-8",
                              timeout=60, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    return done.stdout if done.returncode == 0 else None


def _committed_text(path: Path) -> tuple[str | None, str | None]:
    """(content at git ``HEAD``, ``HEAD`` commit) of ``path``; (None, None) outside git or when not in ``HEAD``."""
    directory = path.parent
    if not directory.is_dir():
        return None, None
    top = _git(["rev-parse", "--show-toplevel"], directory)
    if top is None:
        return None, None
    root = Path(top.strip())
    try:
        relative = path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return None, None
    commit = _git(["rev-parse", "HEAD"], root)
    text = _git(["show", f"HEAD:{relative}"], root)
    if commit is None or text is None:
        return None, None
    return text, commit.strip()


def _parse_anchor(text: str, where: str) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    for number, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            raise RegistryError(f"{where}:{number}: blank line in the append-only anchor")
        try:
            entry = json.loads(line)
        except json.JSONDecodeError as error:
            raise RegistryError(f"{where}:{number}: unreadable anchor line ({error.msg})") from error
        if not isinstance(entry, dict) or set(entry) != _ANCHOR_KEYS:
            raise RegistryError(f"{where}:{number}: an anchor line holds exactly {sorted(_ANCHOR_KEYS)}")
        if entry["sequence"] != len(entries):
            raise RegistryError(f"{where}:{number}: anchor sequence broken (dropped or reordered line)")
        entries.append(entry)
    return entries


def _anchor_line(record: Mapping[str, Any]) -> dict[str, Any]:
    return {"event": record["event"], "record_sha": record["record_sha"], "sequence": record["sequence"],
            "wave": record["wave"]}


def _check_prefix(records: Sequence[Mapping[str, Any]], entries: Sequence[Mapping[str, Any]], where: str) -> None:
    """Refuse a registry shorter than its anchor or a record that differs from its anchored line."""
    if len(records) < len(entries):
        raise RegistryError(f"the registry holds {len(records)} records but {where} anchors {len(entries)}: "
                            f"records {len(records)}..{len(entries) - 1} were dropped. {_RECOVERY}")
    for entry in entries:
        record = records[int(entry["sequence"])]
        if _anchor_line(record) != dict(entry):
            raise RegistryError(f"registry record {entry['sequence']} differs from {where} "
                                f"({record['record_sha']} vs {entry['record_sha']})")


class TrialRegistry:
    """The append-only, hash-chained, anchored registry file under ``root``."""

    def __init__(self, root: Path | str | None = None, *, anchor_path: Path | str | None = None) -> None:
        self.root = Path(root) if root is not None else default_registry_dir()
        self.path = self.root / REGISTRY_FILE
        self.lock_path = self.root / LOCK_FILE
        if anchor_path is not None:
            self.anchor_path = Path(anchor_path)
        elif root is None:
            self.anchor_path = default_anchor_path()
        else:
            self.anchor_path = self.root / ANCHOR_FILE
        #: The production registry: default root and default anchor (grading requires a committed anchor).
        self.is_default = root is None and anchor_path is None

    # -- reading -------------------------------------------------------------

    def records(self) -> list[dict[str, Any]]:
        """Every record in order, after checking each line, the sequence and the hash chain."""
        if not self.path.is_file():
            return []
        try:
            text = self.path.read_bytes().decode("utf-8")
        except UnicodeDecodeError as error:
            raise RegistryError(f"{self.path}: not UTF-8 ({error.reason}). {_RECOVERY}") from error
        if text and not text.endswith("\n"):
            raise RegistryError(f"{self.path}: torn final line (no newline: an interrupted write). {_RECOVERY}")
        records: list[dict[str, Any]] = []
        previous = None
        for number, line in enumerate(text.split("\n")[:-1], start=1):
            if not line.strip():
                raise RegistryError(f"{self.path}:{number}: blank line in the append-only registry")
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise RegistryError(f"{self.path}:{number}: unreadable record ({error.msg}): a torn or edited "
                                    f"line. {_RECOVERY}") from error
            if not isinstance(record, dict):
                raise RegistryError(f"{self.path}:{number}: a record must be a JSON object")
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

    def _file_anchor(self) -> list[dict[str, Any]]:
        if not self.anchor_path.is_file():
            return []
        text = self.anchor_path.read_bytes().decode("utf-8")
        if text and not text.endswith("\n"):
            raise RegistryError(f"{self.anchor_path}: torn final anchor line (an interrupted write). {_RECOVERY}")
        return _parse_anchor(text, str(self.anchor_path))

    def verify_anchor(self, *, require_committed: bool | None = None) -> AnchorState:
        """Check the whole registry against its anchor; the anchor is read from git ``HEAD`` when committed.

        Refuses an empty or unanchored registry, a registry shorter than its anchor (a dropped
        tail), a record that differs from its anchored line and records not yet anchored.
        ``require_committed`` (default: the production registry) refuses an anchor that is only a
        file, not a commit.
        """
        committed_required = self.is_default if require_committed is None else require_committed
        records = self.records()
        text, commit = _committed_text(self.anchor_path)
        if text is not None:
            entries = _parse_anchor(text, f"{self.anchor_path}@HEAD")
            source, committed = f"git:{commit}", True
        elif committed_required:
            raise RegistryError(f"the registry anchor {self.anchor_path} is not committed: commit it after every "
                                "register_wave / open_holdout (grading reads the anchor from git HEAD)")
        else:
            entries, source, committed = self._file_anchor(), f"file:{self.anchor_path}", False
        if not records or not entries:
            raise RegistryError(f"empty or unanchored trial registry ({len(records)} records, {len(entries)} "
                                f"anchored in {source}): policy v4 grades only anchored registrations")
        _check_prefix(records, entries, f"the anchor ({source})")
        if len(records) > len(entries):
            raise RegistryError(f"registry records {len(entries)}..{len(records) - 1} are not anchored in {source}: "
                                f"commit {self.anchor_path} (or TrialRegistry.sync_anchor after an interrupted write)")
        return AnchorState(source, committed, len(entries), str(records[-1]["record_sha"]), str(self.anchor_path))

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

    def _check_before_write(self, records: Sequence[Mapping[str, Any]]) -> None:
        """Refuse to append to a registry that is shorter than, or differs from, either anchor."""
        _check_prefix(records, self._file_anchor(), f"the anchor file {self.anchor_path}")
        text, commit = _committed_text(self.anchor_path)
        if text is not None:
            _check_prefix(records, _parse_anchor(text, f"{self.anchor_path}@HEAD"), f"the committed anchor ({commit})")

    def _append(self, body: dict[str, Any], records: Sequence[Mapping[str, Any]]) -> str:
        self._check_before_write(records)
        body = {**body, "registry_version": REGISTRY_VERSION, "sequence": len(records),
                "prev_record_sha": records[-1]["record_sha"] if records else None}
        record_sha = _sha(_canonical(body))
        line = _canonical({**body, "record_sha": record_sha}) + "\n"
        with self.path.open("a", encoding="utf-8", newline="\n") as handle:
            handle.write(line)
            handle.flush()
            os.fsync(handle.fileno())
        self._sync_anchor(self.records())
        return record_sha

    def _sync_anchor(self, records: Sequence[Mapping[str, Any]]) -> int:
        entries = self._file_anchor()
        _check_prefix(records, entries, f"the anchor file {self.anchor_path}")
        missing = records[len(entries):]
        if missing:
            self.anchor_path.parent.mkdir(parents=True, exist_ok=True)
            with self.anchor_path.open("a", encoding="utf-8", newline="\n") as handle:
                for record in missing:
                    handle.write(_canonical(_anchor_line(record)) + "\n")
                handle.flush()
                os.fsync(handle.fileno())
        return len(missing)

    def sync_anchor(self) -> int:
        """Append the anchor lines of records an interrupted write left unanchored; returns how many."""
        with self._locked():
            return self._sync_anchor(self.records())

    def _locked(self) -> _Lock:
        self.root.mkdir(parents=True, exist_ok=True)
        return _Lock(self.lock_path)

    def register_wave(self, wave: str, catalog_digest: str, policy_sha: str, feature_ids: Sequence[str],
                      cells: Sequence[tuple[str, str, int]], *, rows: Iterable[Any], preregistration: Any,
                      reported_only: Iterable[str] = (), first_formations: Mapping[str, str] | None = None) -> str:
        """Freeze a wave before it reads any return; returns the registration id (record sha256).

        ``rows``: the wave's catalog entries (exactly ``feature_ids``, all of this wave);
        ``preregistration``: the pre-registration table the rows must equal (``{feature_id: sign}``,
        ``(feature_id, sign)`` pairs or mappings with ``feature_id`` / ``expected_sign``).
        """
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
        by_id: dict[str, Any] = {}
        for entry in rows:
            feature_id = getattr(entry, "feature_id", None)
            if not isinstance(feature_id, str) or feature_id in by_id:
                raise RegistryError("rows must be distinct catalog entries with a feature_id")
            by_id[feature_id] = entry
        if set(by_id) != set(features):
            raise RegistryError(f"rows must be exactly the wave's feature_ids: missing {sorted(set(features) - set(by_id))}, "
                                f"extra {sorted(set(by_id) - set(features))}")
        other_wave = sorted(f for f, entry in by_id.items() if getattr(entry, "wave", wave) != wave)
        if other_wave:
            raise RegistryError(f"rows of another wave than {wave!r}: {other_wave}")
        table = _preregistration_table(preregistration)
        signs = {f: _sign(getattr(by_id[f], "expected_sign", None), f"row {f}") for f in features}
        added, dropped = sorted(set(features) - set(table)), sorted(set(table) - set(features))
        resigned = sorted(f for f in set(features) & set(table) if signs[f] != table[f])
        if added or dropped or resigned:
            raise RegistryError(f"wave {wave!r}: the rows differ from the pre-registration table: not pre-registered "
                                f"{added}, pre-registered but absent {dropped}, re-signed {resigned}")
        extra = set(reported_only)
        if extra - set(features):
            raise RegistryError(f"reported_only names unregistered features {sorted(extra - set(features))}")
        reported = tuple(sorted(extra | (set(_reported_only_rule(policy_id)) & set(features))))
        gating = tuple(f for f in features if f not in reported)
        formations = {str(k): dt.date.fromisoformat(str(v)).isoformat() for k, v in (first_formations or {}).items()}
        if set(formations) - set(features):
            raise RegistryError(f"first_formations names unregistered features {sorted(set(formations) - set(features))}")
        row_records = {f: {"expected_sign": signs[f], "evidence_class": str(getattr(by_id[f], "evidence_class", "")),
                           "population": str(getattr(by_id[f], "population", "")), "row_sha256": row_digest(by_id[f])}
                       for f in features}
        content = {"event": "register_wave", "wave": wave, "catalog_digest": catalog_digest, "policy_id": policy_id,
                   "policy_sha": policy_sha, "feature_ids": list(features), "reported_only": list(reported),
                   "gating_feature_ids": list(gating), "cells": [list(c) for c in cell_set], "n_cells": len(cell_set),
                   "first_formations": dict(sorted(formations.items())), "rows": row_records,
                   "row_digest_fields": list(ROW_DIGEST_FIELDS),
                   "preregistration_sha256": _sha(_canonical(sorted(table.items())))}
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
        """Record the wave's one holdout opening (policy v4: once per wave, final labels only).

        ``label_sha`` is required: ``evaluation.prepared_label_set_sha256`` of the final labels.
        A final grade refuses a run whose label set is not this one, so the flag is checked, not trusted.
        """
        if not final_labels:
            raise RegistryError(f"wave {wave!r}: the holdout opens only on final labels (policy v4 "
                                "holdout.requires_final_labels); provisional grades stay on the selection sample")
        if label_sha is None or not _SHA.fullmatch(str(label_sha)):
            raise RegistryError("label_sha (evaluation.prepared_label_set_sha256 of the final labels) is required "
                                "as a sha256 hex digest")
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
                  cells: Sequence[tuple[str, str, int]], *, rows: Iterable[Any], preregistration: Any,
                  reported_only: Iterable[str] = (), first_formations: Mapping[str, str] | None = None,
                  root: Path | str | None = None) -> str:
    """Freeze a wave in the default registry (see :meth:`TrialRegistry.register_wave`); commit the anchor after."""
    return TrialRegistry(root).register_wave(wave, catalog_digest, policy_sha, feature_ids, cells, rows=rows,
                                             preregistration=preregistration, reported_only=reported_only,
                                             first_formations=first_formations)


def trials_so_far(root: Path | str | None = None) -> int:
    """Registered configurations across every wave (feeds the DSR ``n_trials``)."""
    return TrialRegistry(root).trials_so_far()


def open_holdout(wave: str, *, final_labels: bool = False, label_sha: str | None = None,
                 root: Path | str | None = None) -> None:
    """Open ``wave``'s holdout once, on final labels (see :meth:`TrialRegistry.open_holdout`)."""
    TrialRegistry(root).open_holdout(wave, final_labels=final_labels, label_sha=label_sha)
