#!/usr/bin/env python3
"""Cell-by-cell comparison of two research windows on their overlap (platform v8 W0-2, Review Focus 2).

Does extending a role from 2020-2022 to 2020-2023 change any 2020-2022 value? Values are aligned by (session,
instrument id), never by position: the longer role has more dates and a larger instrument union whose new columns
interleave with the old ones.

  --kind field     --old/--new: two ``atx.research-role-fields/v1`` directories (manifest.json plus one date-major
                   little-endian f64 payload per field, shaped the role's dates x instruments). Each role's axes come
                   from the manifest's ``role.path`` (or --old-role/--new-role) and must carry the manifest SHA-256 and
                   the sessions/ids SHA-256s the fields manifest recorded. One key per field name.
  --kind signal    --old/--new: two candidate-signal caches (strategy_ic_signal_cache.cpp): v2 content-keyed entries
                   ``[<vm identity>/]<role sha>/[fp_<fk16>/]<id>.<dsl16>.{f64,json}`` and v1 ``<sha>/<id>.{f64,json}``.
                   The two caches key the same candidate differently (the field payload SHA-256s differ), so entries
                   are matched by candidate id, see MAPPING. --old-role/--new-role are required: a sidecar names its
                   role only by manifest SHA-256. One key per candidate id.
  --kind daily_ic  --old/--new: two ``train_daily_ic.csv`` files, or the IC run directories holding them, aligned by
                   (id, horizon, session); the columns pearson, rank_ic and oriented_rank_ic are compared. One key
                   per id.

Cell rule: equal = identical IEEE-754 bit patterns, or NaN on both sides (any payload). An unequal cell adds to
``unequal_cells``; NaN on one side only is a mismatch (``nan_mismatch_cells``, a value appeared or vanished); its
absolute and relative differences enter ``max_abs_diff`` / ``max_rel_diff`` when neither side is NaN (inf when an
infinity is involved). A finite old value with no aligned new cell (an instrument, session or key the new window lacks)
counts as ``old_cells_missing_in_new``. ``bit_identical`` = at least one cell compared, no unequal cell and no missing
old value; otherwise ``reason`` says why (review C-9: zero cells compared is never identical). New-only instruments,
sessions and keys are expected (the union grows) and only counted.

Ruling W0-a (review C-10): ``totals.w0a_class`` = identical (bit_identical) / below-tolerance (every difference a
finite value change with max_abs_diff < W0A_TOLERANCE) / stop (anything else: a NaN mismatch, a missing old cell, a key
not compared, no cell compared, or a difference >= the tolerance). max_rel_diff is reported beside it, gating nothing.

Seal: the seal comes from atx-engine/tools/research_window.py (task W0-1), read through ``engine_tools.py``; no date
is written here. A role session, CSV row or --before on or after the seal is refused before any payload is opened. ``--before YYYY-MM-DD`` compares sessions strictly before that date.
Memory: one key at a time; payloads are numpy memmaps read in blocks of BLOCK_ROWS sessions, so two full panels of
all fields are never resident. Payload SHA-256s are verified against their manifest or sidecar (``--no-verify``
skips that second read).

Output ``--out`` (never overwritten): ``atx.window-overlap/v1`` with totals, the differing keys and, with
``--per-key``, one row per field or candidate (cells compared, unequal cells, max abs difference, first differing
session and instrument id, bit_identical). stdout: one JSON summary line. Exit 0 compared, 2 refused.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys

import numpy as np

SCHEMA = "atx.window-overlap/v1"
DAY_NS = 86_400_000_000_000
ROLE_SCHEMA = "atx.recent-research-role/v1"
FIELDS_SCHEMA = "atx.research-role-fields/v1"
SIGNAL_SCHEMAS = ("atx.dsl-candidate-signal/v1", "atx.dsl-candidate-signal/v2")
DAILY_IC_FILE = "train_daily_ic.csv"
DAILY_IC_HEADER = ("id", "horizon", "decision_index", "session_ns", "pearson", "rank_ic", "oriented_rank_ic")
DAILY_IC_VALUES = DAILY_IC_HEADER[4:]
KINDS = ("field", "signal", "daily_ic")
BLOCK_ROWS = 64                   # sessions per memmap block: 64 x names x 8 B per side
METADATA_LIMIT = 16 << 20         # role/fields manifests and runner summaries
SIDECAR_LIMIT = 1 << 20           # the runner's metadata_text bound for a cache sidecar
SCAN_DEPTH = 4                    # ROOT/<vm identity>/<role sha>/fp_<fk16>/
IC_RESULT_DIR = re.compile(r"ic\d+_[0-9a-f]{16}")  # IC-result entries beside signal entries (never signals)
HASH_CHUNK = 8 << 20
CELL_RULE = ("equal iff identical IEEE-754 bits or NaN on both sides; NaN on one side only is an unequal cell "
             "(nan_mismatch_cells); max_abs_diff / max_rel_diff over unequal cells with no NaN side (inf when an "
             "infinity is involved); a finite old value without an aligned new cell is old_cells_missing_in_new; "
             "bit_identical = at least one cell compared, no unequal cell and no missing old value")
W0A_TOLERANCE = 1e-9              # v8-prereg ruling W0-a: a difference below 1e-9 is disclosed, a larger one stops
W0A_CLASSES = ("identical", "below-tolerance", "stop")
MAPPING = ("signal entries are matched by candidate id: each cache is scanned for signal sidecars (schema v1/v2) whose "
           "role_manifest_sha256 is that side's role; the ids compared are the old run's (--old-run) or every id of the "
           "old cache on the old role; with --old-run/--new-run a side's entry is the one whose payload_sha256 the run's "
           "summary.json lists for that id (roles[train].candidate_cache.entries); otherwise a sole entry is taken, and "
           "several entries of one id pair only through exactly one equal dsl_sha256 (else the id is ambiguous and not "
           "compared)")


class OverlapError(Exception):
    """A loud refusal: seal, pin, layout or argument."""


def require(condition, message: str) -> None:
    if not condition:
        raise OverlapError(message)


def seal() -> tuple[int, str]:
    """(seal session ns, source): research_window.SEAL_NS (task W0-1), the atx-impl instance of engine_tools.py."""
    from engine_tools import research_window  # noqa: PLC0415  (task W0-1: the one source of the research window)
    return int(research_window.SEAL_NS), "atx-engine/tools/research_window.py SEAL_NS"


def date_of(ns: int) -> str:
    return str(np.datetime64(int(ns), "ns").astype("datetime64[D]"))


def ns_of(text: str) -> int:
    try:
        day = dt.date.fromisoformat(text)
    except ValueError as exc:
        raise OverlapError(f"--before must be YYYY-MM-DD: {text}") from exc
    return (day - dt.date(1970, 1, 1)).days * DAY_NS


def read_bounded(path: Path, limit: int, what: str) -> bytes:
    path = Path(path)
    require(path.is_file(), f"{what}: missing {path}")
    require(path.stat().st_size <= limit, f"{what}: {path} exceeds {limit} bytes")
    return path.read_bytes()


def sha_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha_file(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(HASH_CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path, limit: int, what: str):
    data = read_bounded(path, limit, what)
    try:
        return json.loads(data.decode("utf-8")), data
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise OverlapError(f"{what}: JSON parse {path}: {exc}") from exc


def jnum(x):
    """JSON-safe float: None stays None, an infinity becomes the string 'inf'."""
    if x is None:
        return None
    return "inf" if math.isinf(x) else float(x)


# ---------------------------------------------------------------- role axes and alignment
class Axes:
    """A published role's session and instrument axes, verified against its manifest; refused past the seal."""

    def __init__(self, directory: Path, seal_ns: int):
        self.dir = Path(directory)
        m, raw = load_json(self.dir / "manifest.json", METADATA_LIMIT, "role manifest")
        self.manifest_sha256 = sha_bytes(raw)
        require(isinstance(m, dict) and m.get("schema") == ROLE_SCHEMA and m.get("status") == "complete",
                f"role {self.dir}: not a complete {ROLE_SCHEMA} payload")
        self.dates, self.instruments = int(m["dates"]), int(m["instruments"])
        require(self.dates > 0 and self.instruments > 0, f"role {self.dir}: empty axes")
        self.files = m["files"]
        self.sessions = self._axis("sessions.i64", "<i8", self.dates).astype(np.int64)
        ids = self._axis("ids.u64", "<u8", self.instruments)
        s = self.sessions
        require(bool(np.all(s % DAY_NS == 0)) and bool(np.all(np.diff(s) > 0)),
                f"role {self.dir}: sessions are not strictly increasing midnight labels")
        require(bool(np.all(ids > 0)) and bool(np.all(ids < np.uint64(1 << 63))) and bool(np.all(ids[1:] > ids[:-1])),
                f"role {self.dir}: ids are not strictly increasing positive i64 securityIDs")
        self.ids = ids.astype(np.int64)
        require(int(s[-1]) < seal_ns, f"role {self.dir}: session {date_of(int(s[-1]))} is on or after the seal "
                                      f"{date_of(seal_ns)}; refusing (hidden data)")

    def _axis(self, name: str, dtype: str, count: int) -> np.ndarray:
        entry = self.files.get(name)
        require(isinstance(entry, dict), f"role {self.dir}: manifest lists no {name}")
        data = read_bounded(self.dir / name, count * 8, f"role {name}")
        require(len(data) == count * 8 == int(entry["bytes"]) and sha_bytes(data) == entry["sha256"],
                f"role {self.dir}: {name} bytes do not match the role manifest")
        return np.frombuffer(data, dtype=dtype)

    def payload(self, path: Path, what: str, expected_sha: str | None) -> np.memmap:
        """The date-major dates x instruments f64 payload at ``path`` as a read-only memmap (extent and pin checked)."""
        size = self.dates * self.instruments * 8
        require(Path(path).is_file() and Path(path).stat().st_size == size,
                f"{what}: {path} is not {self.dates} x {self.instruments} f64 ({size} bytes)")
        if expected_sha is not None:
            require(sha_file(path) == expected_sha, f"{what}: {path} SHA-256 differs from its pin")
        return np.memmap(path, dtype="<f8", mode="r", shape=(self.dates, self.instruments))


class Alignment:
    """(session, instrument id) alignment of an old and a new role over sessions before the cutoff."""

    def __init__(self, old: Axes, new: Axes, before_ns: int | None):
        cut_old = len(old.sessions) if before_ns is None else int(np.searchsorted(old.sessions, before_ns, "left"))
        cut_new = len(new.sessions) if before_ns is None else int(np.searchsorted(new.sessions, before_ns, "left"))
        self.sessions, self.rows_old, self.rows_new = np.intersect1d(
            old.sessions[:cut_old], new.sessions[:cut_new], assume_unique=True, return_indices=True)
        self.old_only_rows = np.setdiff1d(np.arange(cut_old), self.rows_old)
        self.new_only_sessions = cut_new - len(self.rows_new)
        self.ids, self.cols_old, self.cols_new = np.intersect1d(old.ids, new.ids, assume_unique=True,
                                                                return_indices=True)
        self.old_only_cols = np.setdiff1d(np.arange(old.instruments), self.cols_old)
        self.new_only_cols = np.setdiff1d(np.arange(new.instruments), self.cols_new)

    def describe(self) -> dict:
        first = date_of(int(self.sessions[0])) if len(self.sessions) else None
        last = date_of(int(self.sessions[-1])) if len(self.sessions) else None
        return {"common_sessions": int(len(self.sessions)), "first_common_session": first,
                "last_common_session": last, "old_only_sessions": int(len(self.old_only_rows)),
                "new_only_sessions": int(self.new_only_sessions), "common_instruments": int(len(self.ids)),
                "old_only_instruments": int(len(self.old_only_cols)),
                "new_only_instruments": int(len(self.new_only_cols))}


# ---------------------------------------------------------------- cell statistics
class CellStats:
    """Accumulates the cell rule over aligned blocks; ``first`` is the first unequal (row label, column label)."""

    def __init__(self):
        self.cells = self.unequal = self.nan_mismatch = self.missing = self.new_only_finite = 0
        self.max_abs: float | None = None
        self.max_rel: float | None = None
        self.first = None

    def add(self, old: np.ndarray, new: np.ndarray, row_labels, col_labels) -> None:
        old, new = np.ascontiguousarray(old, dtype="<f8"), np.ascontiguousarray(new, dtype="<f8")
        old_nan, new_nan = np.isnan(old), np.isnan(new)
        unequal = (old.view(np.uint64) != new.view(np.uint64)) & ~(old_nan & new_nan)
        self.cells += old.size
        count = int(np.count_nonzero(unequal))
        if not count:
            return
        self.unequal += count
        self.nan_mismatch += int(np.count_nonzero(unequal & (old_nan != new_nan)))
        valued = unequal & ~old_nan & ~new_nan
        if valued.any():
            a, b = old[valued], new[valued]
            with np.errstate(invalid="ignore", over="ignore", divide="ignore"):
                diff = np.abs(a - b)
                scale = np.maximum(np.abs(a), np.abs(b))
                rel = np.where(scale > 0, diff / np.where(scale > 0, scale, 1.0), 0.0)   # +0 vs -0: no difference
            diff = np.where(np.isnan(diff), np.inf, diff)
            rel = np.where(np.isnan(rel), np.inf, rel)
            self.max_abs = max(self.max_abs or 0.0, float(diff.max()))
            self.max_rel = max(self.max_rel or 0.0, float(rel.max()))
        if self.first is None:
            r, c = divmod(int(np.argmax(unequal.reshape(-1))), old.shape[1])
            self.first = (row_labels[r], col_labels[c])

    def reason(self) -> str | None:
        """Why the key is not bit-identical (None when it is): no cell compared, unequal cells, missing old cells."""
        out = []
        if self.cells == 0:
            out.append("no cell compared (no common session and instrument)")
        if self.unequal:
            out.append(f"{self.unequal} unequal cell(s), {self.nan_mismatch} of them NaN against a value")
        if self.missing:
            out.append(f"{self.missing} finite old cell(s) missing in new")
        return "; ".join(out) or None

    def row(self, key: str, first) -> dict:
        why = self.reason()
        return {"key": key, "status": "compared", "cells_compared": self.cells, "unequal_cells": self.unequal,
                "nan_mismatch_cells": self.nan_mismatch, "max_abs_diff": jnum(self.max_abs),
                "max_rel_diff": jnum(self.max_rel), "first_diff": first, "old_cells_missing_in_new": self.missing,
                "new_only_finite_cells": self.new_only_finite, "bit_identical": why is None, "reason": why}


def finite_count(block: np.ndarray) -> int:
    return int(np.count_nonzero(np.isfinite(block))) if block.size else 0


def compare_panel(old: np.ndarray, new: np.ndarray, al: Alignment, key: str) -> dict:
    """One key's aligned comparison, streamed BLOCK_ROWS sessions at a time."""
    stats = CellStats()
    for a in range(0, len(al.rows_old), BLOCK_ROWS):
        old_rows = np.asarray(old[al.rows_old[a:a + BLOCK_ROWS]])
        new_rows = np.asarray(new[al.rows_new[a:a + BLOCK_ROWS]])
        stats.add(old_rows[:, al.cols_old], new_rows[:, al.cols_new], al.sessions[a:a + BLOCK_ROWS], al.ids)
        stats.missing += finite_count(old_rows[:, al.old_only_cols])
        stats.new_only_finite += finite_count(new_rows[:, al.new_only_cols])
    for a in range(0, len(al.old_only_rows), BLOCK_ROWS):
        stats.missing += finite_count(np.asarray(old[al.old_only_rows[a:a + BLOCK_ROWS]]))
    first = None
    if stats.first is not None:
        session, instrument = stats.first
        first = {"session": date_of(int(session)), "session_ns": int(session), "instrument_id": int(instrument)}
    return stats.row(key, first)


def missing_row(key: str, status: str) -> dict:
    return {"key": key, "status": status, "cells_compared": 0, "unequal_cells": 0, "nan_mismatch_cells": 0,
            "max_abs_diff": None, "max_rel_diff": None, "first_diff": None, "old_cells_missing_in_new": None,
            "new_only_finite_cells": None, "bit_identical": False, "reason": f"not compared: {status}"}


# ---------------------------------------------------------------- kind: field
class FieldsDir:
    """A complete fields directory bound to its role (axes verified against the manifest's role block)."""

    def __init__(self, directory: Path, role_dir: Path | None, seal_ns: int, verify: bool):
        self.dir, self.verify = Path(directory), verify
        m, _ = load_json(self.dir / "manifest.json", METADATA_LIMIT, "fields manifest")
        require(isinstance(m, dict) and m.get("schema") == FIELDS_SCHEMA and m.get("status") == "complete",
                f"fields {self.dir}: not a complete {FIELDS_SCHEMA} manifest")
        bound = m["role"]
        self.axes = Axes(Path(role_dir) if role_dir is not None else Path(bound["path"]), seal_ns)
        require(bound.get("manifest_sha256") == self.axes.manifest_sha256 and
                bound.get("sessions_sha256") == self.axes.files["sessions.i64"]["sha256"] and
                bound.get("ids_sha256") == self.axes.files["ids.u64"]["sha256"] and
                int(bound.get("dates", -1)) == self.axes.dates and
                int(bound.get("instruments", -1)) == self.axes.instruments,
                f"fields {self.dir}: role {self.axes.dir} is not the role the manifest is bound to")
        self.fields = m["fields"]
        self.files = m["files"]
        self.by_name = {f["name"]: f for f in self.fields}
        require(len(self.by_name) == len(self.fields), f"fields {self.dir}: duplicate field name")

    def open(self, name: str) -> np.memmap:
        entry = self.by_name[name]
        require(entry.get("dtype") == "<f8" and entry.get("layout") == "date-major" and
                list(entry.get("shape", [])) == [self.axes.dates, self.axes.instruments],
                f"fields {self.dir}: {name} is not a date-major <f8 role-shaped payload")
        pin = self.files[entry["file"]]["sha256"] if self.verify else None
        return self.axes.payload(self.dir / entry["file"], f"field {name}", pin)


def field_rows(args, seal_ns: int, before_ns: int | None) -> tuple[list[dict], dict, dict]:
    old = FieldsDir(args.old, args.old_role, seal_ns, not args.no_verify)
    new = FieldsDir(args.new, args.new_role, seal_ns, not args.no_verify)
    al = Alignment(old.axes, new.axes, before_ns)
    rows = []
    for entry in old.fields:
        name = entry["name"]
        if name not in new.by_name:
            rows.append(missing_row(name, "missing_in_new"))
            continue
        a, b = old.open(name), new.open(name)
        rows.append(compare_panel(a, b, al, name))
        del a, b  # unmap before the next key: one field pair resident at a time
    extra = {"new_only_keys": sorted(set(new.by_name) - set(old.by_name))}
    return rows, al.describe(), extra


# ---------------------------------------------------------------- kind: signal
def scan_cache(root: Path, role_sha: str) -> dict[str, list[dict]]:
    """Signal entries under ``root`` recorded for the role manifest ``role_sha``, by candidate id."""
    root = Path(root)
    require(root.is_dir(), f"signal cache {root}: not a directory")
    out: dict[str, list[dict]] = {}
    for dirpath, dirnames, filenames in os.walk(root):
        here = Path(dirpath)
        depth = len(here.relative_to(root).parts)
        dirnames[:] = sorted(d for d in dirnames if depth < SCAN_DEPTH and not d.startswith(".") and
                             not IC_RESULT_DIR.fullmatch(d))
        for name in sorted(filenames):
            if not name.endswith(".json") or name.startswith(".") or (here / name).stat().st_size > SIDECAR_LIMIT:
                continue
            try:
                j = json.loads((here / name).read_bytes().decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue  # not a sidecar (a partial or foreign file): the runner never reads it either
            if not isinstance(j, dict) or j.get("schema") not in SIGNAL_SCHEMAS or \
                    j.get("role_manifest_sha256") != role_sha:
                continue
            cid = j.get("candidate_id")
            payload = j.get("payload", f"{cid}.f64")
            require(isinstance(cid, str) and isinstance(payload, str) and Path(payload).name == payload,
                    f"signal cache: sidecar {here / name} names no candidate or a nested payload")
            out.setdefault(cid, []).append({
                "id": cid, "sidecar": here / name, "payload": here / payload, "schema": j["schema"],
                "dsl_sha256": j.get("dsl_sha256"), "payload_sha256": j.get("payload_sha256"),
                "dates": j.get("dates"), "instruments": j.get("instruments"),
                "field_payload_sha256": j.get("field_payload_sha256")})
    return out


def run_entries(run_dir: Path, role_sha: str) -> dict[str, str]:
    """id -> payload SHA-256 of the cache entry a TRAIN-only IC run used (summary.json candidate_cache.entries)."""
    j, _ = load_json(Path(run_dir) / "summary.json", METADATA_LIMIT, "IC run summary")
    roles = j.get("roles") if isinstance(j, dict) else None
    require(isinstance(roles, list) and roles and all(r.get("role") == "train" for r in roles),
            f"IC run {run_dir}: summary is not a TRAIN-only run; refusing")
    role = roles[0]
    require(role.get("manifest_sha256") == role_sha, f"IC run {run_dir}: scored another role manifest")
    entries = (role.get("candidate_cache") or {}).get("entries")
    require(isinstance(entries, list) and entries, f"IC run {run_dir}: no candidate_cache.entries (cache off?)")
    return {e["id"]: e["payload_sha256"] for e in entries}


def pick_entry(found: list[dict], pinned_sha: str | None) -> dict | None:
    if pinned_sha is None:
        return found[0] if len(found) == 1 else None
    hits = [e for e in found if e["payload_sha256"] == pinned_sha]
    return hits[0] if hits else None


def pair_signals(old: dict, new: dict, old_run: dict | None, new_run: dict | None) -> tuple[list, dict]:
    """[(id, old entry, new entry)] per MAPPING, plus the ids left unmatched, by reason. The ids are the old run's
    (--old-run) or else every id the old cache holds for the old role."""
    ids = sorted(old_run) if old_run is not None else sorted(old)
    pairs, unmatched = [], {"missing_in_new": [], "ambiguous": [], "run_entry_not_in_cache": []}
    for cid in ids:
        a_found, b_found = old.get(cid, []), new.get(cid, [])
        a_pin = None if old_run is None else old_run[cid]
        b_pin = None if new_run is None else new_run.get(cid)
        if not b_found or (new_run is not None and b_pin is None):
            unmatched["missing_in_new"].append(cid)
            continue
        a, b = pick_entry(a_found, a_pin), pick_entry(b_found, b_pin)
        if (a_pin is not None and a is None) or (b_pin is not None and b is None):
            unmatched["run_entry_not_in_cache"].append(cid)
            continue
        if a is None or b is None:  # several entries on a side: exactly one equal-DSL pair resolves it
            same = [(x, y) for x in ([a] if a else a_found) for y in ([b] if b else b_found)
                    if x["dsl_sha256"] == y["dsl_sha256"]]
            if len(same) != 1:
                unmatched["ambiguous"].append(cid)
                continue
            a, b = same[0]
        pairs.append((cid, a, b))
    unmatched["new_only_keys"] = sorted(set(new if new_run is None else new_run) - set(ids))
    return pairs, unmatched


def signal_rows(args, seal_ns: int, before_ns: int | None) -> tuple[list[dict], dict, dict]:
    require(args.old_role is not None and args.new_role is not None,
            "--kind signal needs --old-role and --new-role (a sidecar names its role only by manifest SHA-256)")
    old_axes, new_axes = Axes(args.old_role, seal_ns), Axes(args.new_role, seal_ns)
    old = scan_cache(args.old, old_axes.manifest_sha256)
    new = scan_cache(args.new, new_axes.manifest_sha256)
    old_run = run_entries(args.old_run, old_axes.manifest_sha256) if args.old_run else None
    new_run = run_entries(args.new_run, new_axes.manifest_sha256) if args.new_run else None
    pairs, unmatched = pair_signals(old, new, old_run, new_run)
    al = Alignment(old_axes, new_axes, before_ns)
    rows = [missing_row(cid, reason) for reason in ("missing_in_new", "ambiguous", "run_entry_not_in_cache")
            for cid in unmatched[reason]]
    for cid, a, b in pairs:
        for entry, axes in ((a, old_axes), (b, new_axes)):
            require(entry["dates"] == axes.dates and entry["instruments"] == axes.instruments,
                    f"signal {cid}: sidecar {entry['sidecar']} geometry is not its role's")
        verify = not args.no_verify
        x = old_axes.payload(a["payload"], f"signal {cid} (old)", a["payload_sha256"] if verify else None)
        y = new_axes.payload(b["payload"], f"signal {cid} (new)", b["payload_sha256"] if verify else None)
        row = compare_panel(x, y, al, cid)
        del x, y
        row.update(old_sidecar=str(a["sidecar"]), new_sidecar=str(b["sidecar"]),
                   dsl_sha256_equal=a["dsl_sha256"] == b["dsl_sha256"])
        rows.append(row)
    rows.sort(key=lambda r: r["key"])
    return rows, al.describe(), {"mapping": MAPPING, "unmatched": unmatched}


# ---------------------------------------------------------------- kind: daily_ic
def daily_ic_path(path: Path) -> Path:
    path = Path(path)
    return path / DAILY_IC_FILE if path.is_dir() else path


def read_daily_ic(path: Path, seal_ns: int, before_ns: int | None) -> dict[str, dict]:
    """id -> {(session_ns, horizon): (pearson, rank_ic, oriented_rank_ic)}; empty cells are NaN."""
    out: dict[str, dict] = {}
    with daily_ic_path(path).open(newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        require(tuple(next(reader, ())) == DAILY_IC_HEADER, f"daily IC {path}: header is not {','.join(DAILY_IC_HEADER)}")
        for line, row in enumerate(reader, start=2):
            require(len(row) == len(DAILY_IC_HEADER), f"daily IC {path}: line {line} has {len(row)} columns")
            session = int(row[3])
            require(session < seal_ns, f"daily IC {path}: line {line} session {date_of(session)} is on or after the "
                                       f"seal {date_of(seal_ns)}; refusing (hidden data)")
            if before_ns is not None and session >= before_ns:
                continue
            values = tuple(float(v) if v else math.nan for v in row[4:])
            series = out.setdefault(row[0], {})
            key = (session, int(row[1]))
            require(key not in series, f"daily IC {path}: duplicate row {row[0]} {row[1]} {date_of(session)}")
            series[key] = values
    return out


def compare_series(key: str, old: dict, new: dict) -> dict:
    stats = CellStats()
    common = sorted(set(old) & set(new))
    if common:
        a = np.array([old[k] for k in common], dtype="<f8")
        b = np.array([new[k] for k in common], dtype="<f8")
        stats.add(a, b, common, DAILY_IC_VALUES)
    stats.missing = sum(finite_count(np.array(old[k], dtype="<f8")) for k in set(old) - set(new))
    stats.new_only_finite = sum(finite_count(np.array(new[k], dtype="<f8")) for k in set(new) - set(old))
    first = None
    if stats.first is not None:
        (session, horizon), column = stats.first
        first = {"session": date_of(session), "session_ns": int(session), "horizon": int(horizon), "column": column}
    return stats.row(key, first)


def daily_ic_rows(args, seal_ns: int, before_ns: int | None) -> tuple[list[dict], dict, dict]:
    old = read_daily_ic(args.old, seal_ns, before_ns)
    new = read_daily_ic(args.new, seal_ns, before_ns)
    rows = [compare_series(k, old[k], new[k]) if k in new else missing_row(k, "missing_in_new") for k in sorted(old)]
    s_old = {s for series in old.values() for s, _ in series}
    s_new = {s for series in new.values() for s, _ in series}
    axes = {"common_sessions": len(s_old & s_new), "old_only_sessions": len(s_old - s_new),
            "new_only_sessions": len(s_new - s_old)}
    return rows, axes, {"new_only_keys": sorted(set(new) - set(old))}


# ---------------------------------------------------------------- report
def worst_of(rows: list[dict], key: str):
    """The largest of the rows' ``key`` values ("inf" wins), None when no row has one."""
    values = [r[key] for r in rows if r.get(key) is not None]
    return None if not values else ("inf" if "inf" in values else max(values))


def w0a_class(t: dict) -> str:
    """Ruling W0-a's reading of the totals (review C-10): identical, below-tolerance (only finite value changes, the
    largest below W0A_TOLERANCE) or stop (a NaN mismatch, a missing old cell, a key not compared, no cell compared, or
    a difference at or above the tolerance)."""
    if t["bit_identical"]:
        return "identical"
    worst = t["max_abs_diff"]
    if (t["keys_compared"] < t["keys"] or t["cells_compared"] == 0 or t["keys_without_cells"] or
            t["nan_mismatch_cells"] or t["old_cells_missing_in_new"] or worst is None or worst == "inf" or
            worst >= W0A_TOLERANCE):
        return "stop"
    return "below-tolerance"


def totals(rows: list[dict]) -> dict:
    compared = [r for r in rows if r["status"] == "compared"]
    first = next(({"key": r["key"], **r["first_diff"]} for r in rows if r["first_diff"] is not None), None)
    t = {"keys": len(rows), "keys_compared": len(compared),
         "cells_compared": sum(r["cells_compared"] for r in compared),
         "unequal_cells": sum(r["unequal_cells"] for r in compared),
         "nan_mismatch_cells": sum(r["nan_mismatch_cells"] for r in compared),
         "max_abs_diff": worst_of(compared, "max_abs_diff"), "max_rel_diff": worst_of(compared, "max_rel_diff"),
         "first_diff": first,
         "old_cells_missing_in_new": sum(r["old_cells_missing_in_new"] for r in compared),
         "new_only_finite_cells": sum(r["new_only_finite_cells"] for r in compared),
         "keys_without_cells": sum(1 for r in compared if r["cells_compared"] == 0)}
    why = []
    if not rows:
        why.append("no key to compare")
    if len(compared) < len(rows):
        why.append(f"{len(rows) - len(compared)} key(s) not compared")
    if rows and t["cells_compared"] == 0:
        why.append("no cell compared (no common session and instrument)")
    elif t["keys_without_cells"]:
        why.append(f"{t['keys_without_cells']} key(s) with no cell compared")
    if t["unequal_cells"]:
        why.append(f"{t['unequal_cells']} unequal cell(s), {t['nan_mismatch_cells']} of them NaN against a value")
    if t["old_cells_missing_in_new"]:
        why.append(f"{t['old_cells_missing_in_new']} finite old cell(s) missing in new")
    t["bit_identical"] = not why and all(r["bit_identical"] for r in rows)
    t["reason"] = "; ".join(why) or (None if t["bit_identical"] else "a key is not bit-identical")
    t["w0a_class"] = w0a_class(t)
    t["w0a_tolerance"] = W0A_TOLERANCE
    return t


def compare(args) -> dict:
    seal_ns, seal_source = seal()
    before_ns = None if args.before is None else ns_of(args.before)
    require(before_ns is None or before_ns <= seal_ns,
            f"--before {args.before} is after the seal {date_of(seal_ns)}; refusing (hidden data)")
    rows, axes, extra = {"field": field_rows, "signal": signal_rows, "daily_ic": daily_ic_rows}[args.kind](
        args, seal_ns, before_ns)
    report = {"schema": SCHEMA, "kind": args.kind, "old": str(args.old), "new": str(args.new),
              "before": args.before, "seal": {"date": date_of(seal_ns), "source": seal_source},
              "cell_rule": CELL_RULE, "alignment": axes, "totals": totals(rows),
              "differing_keys": [r["key"] for r in rows if not r["bit_identical"]], **extra}
    if args.per_key:
        report["rows"] = rows
    return report


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    p.add_argument("--kind", required=True, choices=KINDS)
    p.add_argument("--old", type=Path, required=True, help="the shorter (reference) window")
    p.add_argument("--new", type=Path, required=True, help="the longer window")
    p.add_argument("--out", type=Path, required=True, help="report JSON (never overwritten)")
    p.add_argument("--before", default=None, help="compare sessions strictly before YYYY-MM-DD (<= the seal)")
    p.add_argument("--per-key", action="store_true", help="one report row per field / candidate / id")
    p.add_argument("--old-role", type=Path, default=None, help="role directory (signal: required; field: override)")
    p.add_argument("--new-role", type=Path, default=None, help="role directory (signal: required; field: override)")
    p.add_argument("--old-run", type=Path, default=None, help="signal: IC run directory naming the old entries")
    p.add_argument("--new-run", type=Path, default=None, help="signal: IC run directory naming the new entries")
    p.add_argument("--no-verify", action="store_true", help="skip the payload SHA-256 checks (one read fewer)")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        require(not args.out.exists(), f"--out exists; refusing overwrite: {args.out}")
        require(args.kind == "signal" or (args.old_run is None and args.new_run is None),
                "--old-run/--new-run apply to --kind signal only")
        report = compare(args)
    except OverlapError as exc:
        print(f"compare_window_overlap: {exc}", file=sys.stderr)
        return 2
    partial = args.out.with_name(args.out.name + ".partial")
    partial.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    partial.replace(args.out)
    t = report["totals"]
    print(json.dumps({"kind": args.kind, "bit_identical": t["bit_identical"], "max_abs_diff": t["max_abs_diff"],
                      "unequal_cells": t["unequal_cells"], "cells_compared": t["cells_compared"],
                      "nan_mismatch_cells": t["nan_mismatch_cells"], "max_rel_diff": t["max_rel_diff"],
                      "old_cells_missing_in_new": t["old_cells_missing_in_new"], "w0a_class": t["w0a_class"],
                      "reason": t["reason"], "differing_keys": len(report["differing_keys"]), "out": str(args.out)},
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
