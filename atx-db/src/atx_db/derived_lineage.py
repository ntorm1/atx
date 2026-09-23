"""Bounded qualification of direct selected operands in canonical metric history.

No security_id or ticker is treated as issuer ownership evidence. Only the
actual retained CIK on every selected standardized leaf can establish it.
"""

from __future__ import annotations

import hashlib
import json
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from datetime import datetime
from typing import Any

from .derived_registry import DERIVED_SOURCE_NAME


@dataclass(frozen=True)
class LineageQualification:
    status: str
    reason: str
    selected_cik: str | None
    leaf_ids: tuple[str, ...]
    leaf_ciks: tuple[str, ...]
    input_clocks: tuple[datetime, ...]
    fiscal_ends: tuple[Any, ...]
    digest: str | None


def _failed(status: str, reason: str) -> LineageQualification:
    return LineageQualification(status, reason, None, (), (), (), (), None)


def _normalize_cik(value: Any) -> str | None:
    if value is None:
        return None
    text = str(value).strip()
    if not text.isdecimal():
        return None
    return text.zfill(10)


_METRIC_COLUMNS = (
    "derived_value_id", "source", "metric_code", "metric_window", "target_bucket", "period_end", "value",
    "available_at", "valid_to", "inputs_hash", "definition_hash",
    "fiscal_period_start", "fiscal_period_end", "history_status",
    "value_status", "selected_input_refs_json", "selected_input_refs_hash",
)
_LEAF_COLUMNS = (
    "standardized_id", "canonical_code", "cik", "basis", "source",
    "period_start", "period_end", "available_at", "value",
)


def _fetch_by_ids(con: Any, table: str, id_column: str, columns: tuple[str, ...],
                  ids: Sequence[str], chunk_size: int) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for start in range(0, len(ids), chunk_size):
        batch = ids[start:start + chunk_size]
        placeholders = ",".join("?" for _ in batch)
        rows = con.execute(
            f"SELECT {', '.join(columns)} FROM {table} WHERE {id_column} IN ({placeholders})", batch
        ).fetchall()
        for row in rows:
            mapped = dict(zip(columns, row, strict=True))
            result[str(mapped[id_column])] = mapped
    return result


def _preflight_bytes(con: Any, table: str, id_column: str, ids: Sequence[str],
                     byte_sql: str, *, chunk_size: int, row_limit: int,
                     remaining: int) -> int:
    """Read only IDs and SQL byte lengths before materializing any payload page."""
    total = 0
    for start in range(0, len(ids), chunk_size):
        batch = ids[start:start + chunk_size]
        placeholders = ",".join("?" for _ in batch)
        rows = con.execute(
            f"SELECT {id_column}, {byte_sql} AS payload_bytes FROM {table} "
            f"WHERE {id_column} IN ({placeholders})", batch
        ).fetchall()
        for _, length in rows:
            size = int(length or 0)
            total += size
            if size > row_limit or total > remaining:
                raise _LineageError("limit_exceeded", "selected-lineage batch byte bound")
    return total


def _same_clock(value: Any, stored: Any) -> bool:
    if value is None or stored is None:
        return value is None and stored is None
    try:
        parsed = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
        return parsed.replace(tzinfo=None) == stored.replace(tzinfo=None)
    except (TypeError, ValueError, AttributeError):
        return False


def _same_date(value: Any, stored: Any) -> bool:
    return value == (stored.isoformat() if stored is not None else None)


def _bucket(date: Any) -> int:
    return (date.year * 12 + date.month - 1 + int(date.day >= 15)) // 3


def qualify_selected_lineage(
    con: Any,
    root_ids: Sequence[str],
    *,
    expected_cik: str | None = None,
    decision_cutoff: datetime | None = None,
    expected_definition_hashes: Mapping[tuple[str, str], str] | None = None,
    max_depth: int = 16,
    max_nodes: int = 512,
    max_bytes: int = 1_048_576,
    batch_size: int = 256,
    max_batch_nodes: int = 4096,
    max_batch_bytes: int = 8_388_608,
) -> dict[str, LineageQualification]:
    """Qualify bounded roots; each result contains all transitive selected leaves.

    ``decision_cutoff=None`` uses each root's own event clock, suitable for an
    immutable root proof cache. Every dependency must be visible at its parent's
    event. Expected hashes must cover each reachable (code, window) definition.
    """
    roots = tuple(dict.fromkeys(str(value) for value in root_ids))
    if not roots:
        return {}
    if (not 1 <= len(roots) <= batch_size <= 256 or
        min(max_depth, max_nodes, max_bytes, max_batch_nodes, max_batch_bytes) < 1):
        raise ValueError("selected-lineage resolver bounds exceeded or invalid")
    wanted_cik = _normalize_cik(expected_cik) if expected_cik is not None else None
    if expected_cik is not None and wanted_cik is None:
        return {root: _failed("mismatch", "invalid expected CIK") for root in roots}
    if expected_definition_hashes is None:
        return {root: _failed("definition_unverified", "expected definition hashes required") for root in roots}

    # Breadth-first pages share each row across roots. The global cap is a
    # bounded multiple of the per-root cap, preserving deterministic batches.
    metrics: dict[str, dict[str, Any]] = {}
    frontier = set(roots)
    retained_bytes = 0
    for _ in range(max_depth + 1):
        pending = sorted(frontier - metrics.keys())
        if not pending:
            break
        if len(metrics) + len(pending) > max_batch_nodes:
            return {root: _failed("limit_exceeded", "metric node bound") for root in roots}
        try:
            retained_bytes += _preflight_bytes(
                con, "derived_metric_values", "derived_value_id", pending,
                "coalesce(octet_length(encode(selected_input_refs_json)), 0) + "
                "coalesce(octet_length(encode(inputs_hash)), 0) + "
                "coalesce(octet_length(encode(definition_hash)), 0)",
                chunk_size=batch_size, row_limit=max_bytes,
                remaining=max_batch_bytes - retained_bytes,
            )
        except _LineageError as exc:
            return {root: _failed(exc.status, exc.reason) for root in roots}
        metrics.update(_fetch_by_ids(con, "derived_metric_values", "derived_value_id",
                                     _METRIC_COLUMNS, pending, batch_size))
        frontier = set()
        for row in (metrics[id_] for id_ in pending if id_ in metrics):
            payload = row["selected_input_refs_json"]
            if payload is None or len(payload.encode("utf-8")) > max_bytes:
                continue
            try:
                refs = json.loads(payload)["refs"]
            except (ValueError, TypeError, KeyError):
                continue
            if isinstance(refs, list):
                frontier.update(str(ref["state_id"]) for ref in refs
                                if isinstance(ref, dict) and ref.get("kind") == "metric"
                                and ref.get("state_id") is not None)
    unresolved = frontier - metrics.keys()
    leaf_ids = sorted({str(ref["state_id"])
                       for row in metrics.values()
                       if isinstance(row["selected_input_refs_json"], str)
                       and len(row["selected_input_refs_json"].encode("utf-8")) <= max_bytes
                       for ref in _safe_refs(row["selected_input_refs_json"])
                       if ref.get("kind") == "item" and ref.get("state_id") is not None})
    if len(leaf_ids) + len(metrics) > max_batch_nodes:
        return {root: _failed("limit_exceeded", "leaf node bound") for root in roots}
    try:
        retained_bytes += _preflight_bytes(
            con, "fundamental_standardized", "standardized_id", leaf_ids,
            "coalesce(octet_length(encode(standardized_id)), 0) + "
            "coalesce(octet_length(encode(canonical_code)), 0) + "
            "coalesce(octet_length(encode(cik)), 0) + "
            "coalesce(octet_length(encode(basis)), 0) + "
            "coalesce(octet_length(encode(source)), 0)",
            chunk_size=batch_size, row_limit=max_bytes,
            remaining=max_batch_bytes - retained_bytes,
        )
    except _LineageError as exc:
        return {root: _failed(exc.status, exc.reason) for root in roots}
    leaves = _fetch_by_ids(con, "fundamental_standardized", "standardized_id",
                          _LEAF_COLUMNS, leaf_ids, batch_size)
    answer: dict[str, LineageQualification] = {}
    for root in roots:
        try:
            if root in unresolved:
                raise _LineageError("limit_exceeded", "dependency depth bound")
            seen: set[str] = set()
            active: set[str] = set()
            selected_leaves: dict[str, dict[str, Any]] = {}
            ref_hashes: list[str] = []
            used_bytes = 0
            invalid_state = False

            def visit(metric_id: str, parent_at: datetime | None, depth: int,
                      _seen: set[str] = seen, _active: set[str] = active,
                      _selected_leaves: dict[str, dict[str, Any]] = selected_leaves,
                      _ref_hashes: list[str] = ref_hashes) -> None:
                nonlocal used_bytes, invalid_state
                if depth > max_depth or len(_seen) >= max_nodes:
                    raise _LineageError("limit_exceeded", "depth or node bound")
                if metric_id in _active:
                    raise _LineageError("invalid", f"cyclic metric edge {metric_id}")
                row = metrics.get(metric_id)
                if row is None:
                    raise _LineageError("missing", f"missing metric {metric_id}")
                at = row["available_at"]
                if at is None or (parent_at is not None and at > parent_at):
                    raise _LineageError("invalid", f"metric clock after consumer event: {metric_id}")
                if parent_at is not None and row["valid_to"] is not None and row["valid_to"] <= parent_at:
                    raise _LineageError("invalid", f"metric dependency expired at consumer event: {metric_id}")
                if row["history_status"] != "event_reconstructed":
                    raise _LineageError("invalid", f"nonreconstructed metric {metric_id}")
                if row["value_status"] != "valid" or row["value"] is None:
                    invalid_state = True
                if row["source"] != DERIVED_SOURCE_NAME:
                    raise _LineageError("invalid", f"unsupported metric source {metric_id}")
                key = (str(row["metric_code"]), str(row["metric_window"]))
                expected_hash = expected_definition_hashes.get(key)
                if expected_hash is None or expected_hash != row["definition_hash"]:
                    raise _LineageError("definition_unverified", f"definition hash mismatch: {key}")
                payload = row["selected_input_refs_json"]
                stored_hash = row["selected_input_refs_hash"]
                if payload is None or stored_hash is None:
                    raise _LineageError("legacy_unverifiable", f"legacy metric {metric_id}")
                used_bytes += len(payload.encode("utf-8"))
                if used_bytes > max_bytes:
                    raise _LineageError("limit_exceeded", "payload byte bound")
                if hashlib.sha256(payload.encode("utf-8")).hexdigest() != stored_hash:
                    raise _LineageError("invalid", f"selected-ref hash mismatch: {metric_id}")
                try:
                    decoded = json.loads(payload)
                except ValueError as exc:
                    raise _LineageError("invalid", f"invalid refs JSON: {metric_id}") from exc
                if not isinstance(decoded, dict) or decoded.get("version") != 1 or not isinstance(decoded.get("refs"), list):
                    raise _LineageError("invalid", f"invalid refs version: {metric_id}")
                refs = decoded["refs"]
                if len(refs) > max_nodes:
                    raise _LineageError("limit_exceeded", "direct ref count bound")
                if metric_id in _seen:
                    return
                _seen.add(metric_id)
                _active.add(metric_id)
                _ref_hashes.append(stored_hash)
                for ref in refs:
                    if not isinstance(ref, dict) or ref.get("kind") not in ("item", "metric"):
                        raise _LineageError("invalid", f"invalid ref kind: {metric_id}")
                    ref_id = ref.get("state_id")
                    if ref.get("status") not in ("selected", "missing") or not isinstance(ref_id, str):
                        raise _LineageError("missing", f"missing selected operand identity: {metric_id}")
                    if ref["status"] == "missing":
                        invalid_state = True
                    if not isinstance(ref.get("bucket"), int) or not isinstance(ref.get("offset"), int) or ref["offset"] < 0:
                        raise _LineageError("invalid", f"invalid frame offset: {metric_id}")
                    if row["target_bucket"] is None or ref["bucket"] != row["target_bucket"] - ref["offset"]:
                        raise _LineageError("invalid", f"ref bucket differs from consuming frame: {metric_id}")
                    if ref["kind"] == "metric":
                        child = metrics.get(ref_id)
                        if child is None:
                            raise _LineageError("missing", f"missing metric ref {ref_id}")
                        if (ref.get("code") != child["metric_code"] or
                            ref["bucket"] != child["target_bucket"] or
                            child["source"] != row["source"] or
                            ref.get("source") != child["source"] or
                            not _same_date(ref.get("period_start"), child["fiscal_period_start"]) or
                            not _same_date(ref.get("period_end"), child["fiscal_period_end"]) or
                            ref.get("inputs_hash") != child["inputs_hash"] or
                            ref.get("definition_hash") != child["definition_hash"] or
                            not _same_clock(ref.get("available_at"), child["available_at"])):
                            raise _LineageError("invalid", f"mismatched metric ref {ref_id}")
                        visit(ref_id, at, depth + 1)
                    else:
                        leaf = leaves.get(ref_id)
                        if leaf is None:
                            raise _LineageError("missing", f"missing standardized ref {ref_id}")
                        if (ref.get("code") != leaf["canonical_code"] or
                            leaf["period_end"] is None or ref["bucket"] != _bucket(leaf["period_end"]) or
                            _normalize_cik(ref.get("cik")) != _normalize_cik(leaf["cik"]) or
                            ref.get("basis") != leaf["basis"] or ref.get("source") != leaf["source"] or
                            not _same_date(ref.get("period_start"), leaf["period_start"]) or
                            not _same_date(ref.get("period_end"), leaf["period_end"]) or
                            not _same_clock(ref.get("available_at"), leaf["available_at"])):
                            raise _LineageError("invalid", f"mismatched standardized ref {ref_id}")
                        if leaf["available_at"] is None or leaf["available_at"] > at:
                            raise _LineageError("invalid", f"invisible standardized ref {ref_id}")
                        if leaf["value"] is None:
                            invalid_state = True
                        if _normalize_cik(leaf["cik"]) is None:
                            raise _LineageError("missing", f"missing standardized CIK {ref_id}")
                        _selected_leaves[ref_id] = leaf
                _active.remove(metric_id)

            root_row = metrics.get(root)
            if root_row is None:
                raise _LineageError("missing", f"missing root {root}")
            cutoff = decision_cutoff if decision_cutoff is not None else root_row["available_at"]
            if cutoff is None or root_row["available_at"] is None or root_row["available_at"] > cutoff:
                raise _LineageError("invalid", f"root after decision cutoff: {root}")
            if root_row["valid_to"] is not None and root_row["valid_to"] <= cutoff:
                raise _LineageError("invalid", f"root expired by decision cutoff: {root}")
            visit(root, None, 0)
            if not selected_leaves:
                raise _LineageError("missing", "no selected source leaves")
            ciks = sorted({_normalize_cik(row["cik"]) for row in selected_leaves.values()})
            if len(ciks) != 1:
                raise _LineageError("mismatch", "selected leaves have mixed CIKs")
            selected_cik = ciks[0]
            if wanted_cik is not None and selected_cik != wanted_cik:
                raise _LineageError("mismatch", "selected CIK differs from dated expected CIK")
            ordered = sorted(selected_leaves.items())
            evidence = json.dumps([root, sorted(ref_hashes), [(key, _normalize_cik(row["cik"]),
                                   str(row["available_at"]), str(row["period_end"]))
                                  for key, row in ordered]], separators=(",", ":"))
            answer[root] = LineageQualification(
                "invalid" if invalid_state else "qualified",
                "selected source ownership verified; value invalid" if invalid_state else "selected leaves verified",
                selected_cik,
                tuple(key for key, _ in ordered), tuple(_normalize_cik(row["cik"]) for _, row in ordered),
                tuple(row["available_at"] for _, row in ordered),
                tuple(row["period_end"] for _, row in ordered),
                hashlib.sha256(evidence.encode("utf-8")).hexdigest(),
            )
        except _LineageError as exc:
            answer[root] = _failed(exc.status, exc.reason)
    return answer


def _safe_refs(payload: str) -> list[dict[str, Any]]:
    try:
        refs = json.loads(payload).get("refs", [])
        return [ref for ref in refs if isinstance(ref, dict)] if isinstance(refs, list) else []
    except (ValueError, TypeError, AttributeError):
        return []


class _LineageError(Exception):
    def __init__(self, status: str, reason: str) -> None:
        self.status = status
        self.reason = reason
        super().__init__(reason)
