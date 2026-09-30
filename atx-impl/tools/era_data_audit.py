#!/usr/bin/env python3
"""Era data audit without returns (platform v8, task H-1 step 3): what each era role holds, before any era is read.

  era_data_audit.py --era ID ROLE_MANIFEST ROLE_SHA256 FIELDS_MANIFEST FIELDS_SHA256 [--era ...]
                    [--library LIB --library-sha256 SHA] [--plan K1_PLAN.json] [--json OUT]

Per era (a role manifest and the fields manifest built on it, both pinned by SHA-256):
  coverage      per field and calendar year: member cells, finite member cells and their share, from the fields
                manifest's ``coverage.per_year`` when present, else counted from the field payload and member.u8
  delisting     from the role manifest's ``universe.delisting`` block: terminations, those kept as members at their last
                session and that share (by cause too); and the member mask's exits (a name whose membership ends before
                the role's last session), with the share of them that are delisting terminations
  first valid   per field: the first session with a finite member cell; per library candidate: the latest first valid
                session of the fields its DSL reads (K1 plan rows' extra_fields when --plan is given), plus its
                required_lookback sessions (K1), when --plan is given. close / raw_close / volume are valid where the
                role marks a member present (the role contract), so their payloads are never read.

Opens no return (the audit's contract; test_era_data_audit.py instruments every file open to prove it):
  * never opens close.f64 or raw_close.f64 (returns are prices' ratios) nor any field whose name carries a return token
    (``ret``, ``ret21``, ``dlret``, ``return(s)``, ``fwd*``, ``forward*``: e.g. ``mkt_ret``); such fields are listed as
    skipped;
  * never reads or reports the manifests' value statistics: the fields' ``member_finite_min/max/mean`` and the
    delisting events' ``delist_return`` / ``delist_return_if_performance`` (their keys are dropped on load).
It reads the role's sessions.i64, ids.u64, present.u8 and member.u8 (hash-verified against the role manifest) and the
non-return field payloads (hash-verified against the fields manifest, one streamed pass per field).

Hidden data: a role whose score window or session axis reaches the research seal (research_window.py) is refused
before any payload is opened; a fields manifest must be bound to its role (role.manifest_sha256).
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import sys

import numpy as np

from engine_tools import research_window as rw

SCHEMA = "atx.era-data-audit/v1"
ROLE_SCHEMA = "atx.recent-research-role/v1"
FIELDS_SCHEMA = "atx.research-role-fields/v1"
NEVER_OPENED = ("close.f64", "raw_close.f64")            # returns are ratios of these
PRESENT_FIELDS = ("close", "raw_close", "volume")        # role payloads: valid where a member is present
RETURN_TOKEN = re.compile(r"(?:dl)?ret\d*|returns?|fwd\w*|forward\w*")
VALUE_KEYS = ("member_finite_min", "member_finite_max", "member_finite_mean")
DELIST_VALUE_KEYS = ("delist_return", "delist_return_if_performance", "with_return")
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
DAY_NS = 86_400_000_000_000


class AuditError(ValueError):
    """A refusal: a pin, a binding, the seal or a return file."""


def is_return_field(name: str) -> bool:
    """A field whose name carries a return token (split on non-alphanumerics): never opened."""
    return any(RETURN_TOKEN.fullmatch(tok) for tok in re.split(r"[^a-z0-9]+", name.lower()) if tok)


def iso(ns: int) -> str:
    return (dt.date(1970, 1, 1) + dt.timedelta(days=int(ns) // DAY_NS)).isoformat()


def pinned_json(path: Path, pin: str, what: str) -> dict:
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != pin:
        raise AuditError(f"{what} {path}: SHA-256 differs from the pin")
    doc = json.loads(data)
    if not isinstance(doc, dict):
        raise AuditError(f"{what} {path}: not a JSON object")
    return doc


def strip_values(doc: dict) -> dict:
    """The value statistics dropped from a loaded manifest (never reported, never used)."""
    for row in doc.get("fields", []) or []:
        for key in VALUE_KEYS:
            (row.get("coverage") or {}).pop(key, None)
    delisting = (doc.get("universe") or {}).get("delisting") or {}
    for event in delisting.get("events", []) or []:
        for key in DELIST_VALUE_KEYS:
            event.pop(key, None)
    for cause in (delisting.get("by_cause") or {}).values():
        for key in DELIST_VALUE_KEYS:
            cause.pop(key, None)
    return doc


class Era:
    """One era role and its fields manifest: axes and masks read (hash-verified), the seal checked first."""

    def __init__(self, era_id: str, role_path: Path, role_sha: str, fields_path: Path, fields_sha: str):
        self.id, self.role_sha = era_id, role_sha
        self.role = strip_values(pinned_json(role_path, role_sha, "role manifest"))
        if self.role.get("schema") != ROLE_SCHEMA:
            raise AuditError(f"era {era_id}: role manifest schema is not {ROLE_SCHEMA}")
        end = int(self.role.get("score_end_ns", 0))
        if end > rw.SEAL_NS:
            raise rw.SealError(rw.seal_message(f"era {era_id}: role score window ends {iso(end - DAY_NS)}"))
        self.base = Path(role_path).parent
        self.dates, self.n = int(self.role["dates"]), int(self.role["instruments"])
        self.sessions = self.payload("sessions.i64", "<i8", self.dates)
        if self.sessions.size and rw.is_sealed(int(self.sessions.max())):
            raise rw.SealError(rw.seal_message(f"era {era_id}: a role session"))
        self.ids = self.payload("ids.u64", "<u8", self.n)
        self.member = self.payload("member.u8", "u1", self.dates * self.n).reshape(self.dates, self.n) != 0
        self.present = self.payload("present.u8", "u1", self.dates * self.n).reshape(self.dates, self.n) != 0
        self.years = np.array([dt.date.fromisoformat(iso(s)).year for s in self.sessions])
        self.fields = strip_values(pinned_json(fields_path, fields_sha, "fields manifest"))
        self.fields_base, self.fields_sha = Path(fields_path).parent, fields_sha
        if self.fields.get("schema") != FIELDS_SCHEMA or (self.fields.get("role") or {}).get("manifest_sha256") != role_sha:
            raise AuditError(f"era {era_id}: the fields manifest is not {FIELDS_SCHEMA} bound to its role")

    def payload(self, name: str, dtype: str, count: int) -> np.ndarray:
        if name in NEVER_OPENED:
            raise AuditError(f"{name} is never opened by the audit")
        receipt = self.role["files"][name]
        data = (self.base / name).read_bytes()
        if len(data) != count * np.dtype(dtype).itemsize or hashlib.sha256(data).hexdigest() != receipt["sha256"]:
            raise AuditError(f"era {self.id}: role payload {name} does not match its receipt")
        return np.frombuffer(data, dtype=dtype)

    def first_present(self) -> int | None:
        rows = np.flatnonzero((self.member & self.present).any(axis=1))
        return int(rows[0]) if rows.size else None

    def scan(self, row: dict, need_coverage: bool) -> tuple[int | None, dict | None]:
        """One streamed, hash-verified pass over a field payload: its first valid row and (when asked) the per-year
        member / finite member cell counts."""
        name = row["name"]
        if is_return_field(name):
            raise AuditError(f"field {name} carries a return token: never opened")
        rel = row.get("file", f"{name}.f64")
        receipt = self.fields["files"][rel]
        h, first, per_year = hashlib.sha256(), None, {}
        width = self.n * 8
        with (self.fields_base / rel).open("rb") as stream:
            for t in range(self.dates):
                chunk = stream.read(width)
                if len(chunk) != width:
                    raise AuditError(f"era {self.id}: field {name} is short")
                h.update(chunk)
                finite = np.isfinite(np.frombuffer(chunk, dtype="<f8")) & self.member[t]
                if first is None and finite.any():
                    first = t
                if need_coverage:
                    y = per_year.setdefault(str(int(self.years[t])), [0, 0])
                    y[0] += int(self.member[t].sum())
                    y[1] += int(finite.sum())
            if stream.read(1):
                raise AuditError(f"era {self.id}: field {name} is longer than its role")
        if h.hexdigest() != receipt["sha256"]:
            raise AuditError(f"era {self.id}: field payload {rel} does not match its receipt")
        cov = None
        if need_coverage:
            cov = {y: {"member_cells": m, "finite_member_cells": f, "finite_member_frac": round(f / m, 6) if m else None}
                   for y, (m, f) in sorted(per_year.items())}
        return first, cov

    def date(self, row: int | None) -> str | None:
        return iso(self.sessions[row]) if row is not None and 0 <= row < self.dates else None


def delisting_block(era: Era) -> dict:
    """Delisting terminations kept as members, and the member mask's exits matched to them."""
    last = era.dates - 1
    ever = era.member.any(axis=0)
    last_member = np.where(ever, era.dates - 1 - np.argmax(era.member[::-1], axis=0), -1)
    exits = np.flatnonzero(ever & (last_member < last))
    block = (era.role.get("universe") or {}).get("delisting")
    out: dict = {"mask_exits": {"names": int(exits.size)}}
    if not block:
        out.update(terminations=None, note="the role manifest has no universe.delisting block")
        return out
    events = block.get("events") or []
    kept = sum(1 for e in events if e.get("kept_member_at_last_session"))
    delisted = {int(e["security_id"]) for e in events}
    exit_ids = {int(era.ids[j]) for j in exits}
    out.update(terminations=len(events), kept_member_at_last_session=kept,
               share_kept=round(kept / len(events), 6) if events else None,
               by_cause={c: {"terminations": v.get("terminations"),
                             "kept_member_at_last_session": v.get("kept_member_at_last_session")}
                         for c, v in sorted((block.get("by_cause") or {}).items())})
    out["mask_exits"].update(delisted=len(exit_ids & delisted),
                             share_delisted=round(len(exit_ids & delisted) / len(exit_ids), 6) if exit_ids else None)
    return out


def candidate_fields(cand: dict, known: set, plan: dict | None) -> list[str]:
    row = (plan or {}).get(cand["id"])
    if row is not None:
        extra = [f for f in row.get("extra_fields", []) if isinstance(f, str)]
        base = [f for f in IDENT.findall(cand.get("dsl", "")) if f in PRESENT_FIELDS]
        return sorted(set(extra) | set(base))
    return sorted({f for f in IDENT.findall(cand.get("dsl", "")) if f in known})


def audit_era(era: Era, library: list[dict] | None, plan: dict | None) -> dict:
    coverage, first, skipped = {}, {}, []
    for row in era.fields.get("fields", []):
        name = row["name"]
        if is_return_field(name):
            skipped.append(name)
            continue
        per_year = (row.get("coverage") or {}).get("per_year")
        row_first, computed = era.scan(row, need_coverage=per_year is None)
        coverage[name] = {"source": "manifest" if per_year is not None else "computed",
                          "per_year": per_year if per_year is not None else computed}
        first[name] = row_first
    present_first = era.first_present()
    for name in PRESENT_FIELDS:
        first[name] = present_first
    out = {"id": era.id, "role": {"manifest_sha256": era.role_sha, "dates": era.dates, "instruments": era.n,
                                  "first_session": era.date(0), "last_session": era.date(era.dates - 1),
                                  "score_begin": era.role.get("score_begin"),
                                  "first_scored_session": era.date(era.role.get("score_begin"))},
           "fields_manifest_sha256": era.fields_sha, "coverage": coverage,
           "skipped_return_fields": skipped, "delisting": delisting_block(era),
           "first_valid": {"fields": {k: era.date(v) for k, v in sorted(first.items())}}}
    if library is not None:
        known = set(first) | set(skipped)
        cands = {}
        for cand in library:
            fields = candidate_fields(cand, known, plan)
            missing = sorted(f for f in fields if f not in first or f in skipped)
            rows = [first[f] for f in fields if f in first and f not in skipped]
            lookback = ((plan or {}).get(cand["id"]) or {}).get("required_lookback")
            start = None
            if rows and all(r is not None for r in rows) and not missing:
                start = max(rows) + (int(lookback) if isinstance(lookback, int) else 0)
            cands[cand["id"]] = {"fields": fields, "missing_fields": missing, "required_lookback": lookback,
                                 "first_valid": era.date(start)}
        out["first_valid"]["candidates"] = cands
    return out


def load_plan(path: Path | None) -> dict | None:
    """K1 plan rows (``--plan-only`` output): {id: {required_lookback, extra_fields, ...}}."""
    if path is None:
        return None
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    rows = doc.get("candidates") if isinstance(doc, dict) else doc
    return {r["id"]: r for r in rows or [] if isinstance(r, dict) and "id" in r}


def audit(eras: list[Era], library: list[dict] | None = None, plan: dict | None = None) -> dict:
    return {"schema": SCHEMA, "window_id": rw.WINDOW_ID, "opens_no_return": {
                "never_opened": list(NEVER_OPENED), "return_token": RETURN_TOKEN.pattern,
                "dropped_keys": list(VALUE_KEYS + DELIST_VALUE_KEYS)},
            "eras": [audit_era(e, library, plan) for e in eras]}


def print_audit(doc: dict) -> None:
    for e in doc["eras"]:
        r = e["role"]
        print(f"== era {e['id']}: role {r['manifest_sha256'][:12]} {r['first_session']}..{r['last_session']} "
              f"({r['dates']} sessions x {r['instruments']} names; scored from {r['first_scored_session']})")
        for name, c in sorted(e["coverage"].items()):
            years = " ".join(f"{y}:{fmt_frac(v.get('finite_member_frac'))}" for y, v in c["per_year"].items())
            print(f"   {name:24s} [{c['source']}] first valid {e['first_valid']['fields'][name]} | {years}")
        if e["skipped_return_fields"]:
            print(f"   skipped (return token, never opened): {', '.join(e['skipped_return_fields'])}")
        d = e["delisting"]
        print(f"   delisting: terminations {d.get('terminations')} kept as members {d.get('kept_member_at_last_session')} "
              f"(share {d.get('share_kept')}); mask exits {d['mask_exits']['names']} of which delisted "
              f"{d['mask_exits'].get('delisted')} (share {d['mask_exits'].get('share_delisted')})")
        for cid, c in (e["first_valid"].get("candidates") or {}).items():
            print(f"   candidate {cid}: first valid {c['first_valid']} (fields {', '.join(c['fields']) or '-'}; "
                  f"lookback {c['required_lookback']})")


def fmt_frac(x) -> str:
    return "na" if x is None else f"{x:.3f}"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--era", nargs=5, action="append", required=True,
                    metavar=("ID", "ROLE_MANIFEST", "ROLE_SHA256", "FIELDS_MANIFEST", "FIELDS_SHA256"))
    ap.add_argument("--library", type=Path, default=None)
    ap.add_argument("--library-sha256", default=None)
    ap.add_argument("--plan", type=Path, default=None, help="K1 plan rows (atx-equity-strategy-ic --plan-only)")
    ap.add_argument("--json", type=Path, default=None)
    a = ap.parse_args(argv)
    if (a.library is None) != (a.library_sha256 is None):
        ap.error("--library and --library-sha256 go together")
    try:
        eras = [Era(i, Path(rm), rs, Path(fm), fs) for i, rm, rs, fm, fs in a.era]
        library = pinned_json(a.library, a.library_sha256, "library")["candidates"] if a.library else None
        doc = audit(eras, library, load_plan(a.plan))
    except (AuditError, rw.SealError, OSError, KeyError, ValueError) as exc:
        print(f"era_data_audit: {exc}", file=sys.stderr)
        return 2
    print_audit(doc)
    if a.json:
        a.json.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
