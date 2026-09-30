"""Era shards: the generic pooling rules of per-era research outputs, in date order (platform v8, task H-1).

An era is a role scored on its own window (E1 2014-2016, E2 2017-2019, each with its own warm-up and today's
geometry; E3 the 4-year TRAIN role). Every era runs fields -> u -> w -> nav on its own; the fitter and nav_summ pool
the eras' series in date order. This module holds the rules every consumer shares (research_roles.py, the fitter,
nav_summ, backtest_integrity), so no consumer re-derives them:

  check_windows    the era windows [begin, end) (ns): id, emptiness, the seal, TRAIN straddle, order and overlap
  segment_starts   the start row of each era's segment in the pooled series (sessions strictly increasing)
  pool_rows        daily columnar dicts (nav_summ's load_daily) concatenated, with ``SEGMENTS`` (row starts)
  pool_matrix      K x T_e factor matrices concatenated along time
  weighted_mean    one pooled value from per-era values (one era: its value, bit for bit)
  pooled_sha256    the pooled identity of per-era SHA-256s (one era: its SHA-256, so a one-era pool is the era)

Standard library at import (research_cycle imports it through research_roles); numpy only inside the array functions.
The TRAIN and seal instants are arguments: callers read them from research_window.py, never from here.
"""
from __future__ import annotations

import hashlib
import json
import re

SCHEMA = "atx.era-pool/v1"
SEGMENTS = "SEGMENTS"                  # pool_rows: the key holding the start row of every era's segment
POOL_PREFIX = "pool:"
ID_RE = re.compile(r"[A-Za-z0-9_]+")   # an era id goes into file and directory names


class PoolError(ValueError):
    """A refusal of an era list: a bad id, an empty, sealed, straddling, unordered or overlapping window."""


def check_id(era_id) -> str:
    if not isinstance(era_id, str) or not ID_RE.fullmatch(era_id):
        raise PoolError(f"era id {era_id!r} must match [A-Za-z0-9_]+ (it names files)")
    return era_id


def check_windows(windows, train_begin_ns: int, train_end_ns: int, seal_ns: int) -> list[tuple[str, int, int]]:
    """Validate era windows ``[(id, begin_ns, end_ns), ...]`` (end exclusive) given in date order; returns them.

    Refused: a bad or duplicate id; an empty window; a window ending after the seal; a window straddling the TRAIN
    begin (an era is history before TRAIN or lies inside TRAIN, so no history era overlaps a TRAIN role even when that
    role is not listed); a TRAIN era ending after the TRAIN end; eras out of date order or overlapping. A warm-up
    before ``begin`` is not part of the window and may reach into the previous era."""
    out, seen = [], set()
    prev = None
    for item in windows:
        try:
            era_id, begin, end = item
        except (TypeError, ValueError) as exc:
            raise PoolError(f"era window {item!r} is not (id, begin_ns, end_ns)") from exc
        check_id(era_id)
        if era_id in seen:
            raise PoolError(f"era {era_id}: duplicate id")
        seen.add(era_id)
        begin, end = int(begin), int(end)
        if not begin < end:
            raise PoolError(f"era {era_id}: empty window")
        if end > seal_ns:
            raise PoolError(f"era {era_id}: window ends after the research seal; refusing (sealed)")
        if begin < train_begin_ns < end:
            raise PoolError(f"era {era_id}: window straddles the TRAIN begin (an era is history before TRAIN or lies "
                            "inside TRAIN)")
        if begin >= train_begin_ns and end > train_end_ns:
            raise PoolError(f"era {era_id}: a TRAIN era ends after the TRAIN end")
        if prev is not None:
            if begin < prev[1]:
                raise PoolError(f"era {era_id}: eras are not in date order ({prev[0]} begins later)")
            if begin < prev[2]:
                raise PoolError(f"era {era_id}: window overlaps era {prev[0]}")
        prev = (era_id, begin, end)
        out.append(prev)
    if not out:
        raise PoolError("no era")
    return out


def segment_starts(eras) -> list[int]:
    """Start rows of each era's segment in the pooled series of ``[(id, sessions), ...]`` (date order). Refuses an
    empty era, sessions that are not strictly increasing, and an era whose first session is not after the previous
    era's last."""
    starts, row, last = [], 0, None
    for era_id, sessions in eras:
        values = [int(s) for s in sessions]
        if not values:
            raise PoolError(f"era {era_id}: no session")
        if any(b <= a for a, b in zip(values, values[1:])):
            raise PoolError(f"era {era_id}: sessions are not strictly increasing")
        if last is not None and values[0] <= last:
            raise PoolError(f"era {era_id}: its first session is not after the previous era's last (overlap or "
                            "order)")
        starts.append(row)
        row += len(values)
        last = values[-1]
    if not starts:
        raise PoolError("no era")
    return starts


def pool_rows(eras, key: str = "session_ns") -> dict:
    """Pool the columnar dicts ``[(id, {column: array or list}), ...]`` of one layout (same columns, same order): each
    column concatenated in era order, plus ``SEGMENTS`` = the start row of every era (checked on ``key``)."""
    import numpy as np  # noqa: PLC0415  (numpy only for the array functions)
    eras = list(eras)
    if not eras:
        raise PoolError("no era")
    columns = list(eras[0][1])
    for era_id, doc in eras[1:]:
        if list(doc) != columns:
            raise PoolError(f"era {era_id}: columns differ from era {eras[0][0]}")
    if key not in columns:
        raise PoolError(f"no {key} column to order the eras by")
    starts = segment_starts([(era_id, doc[key]) for era_id, doc in eras])
    out: dict = {}
    for name in columns:
        parts = [doc[name] for _, doc in eras]
        if all(isinstance(p, np.ndarray) for p in parts):
            out[name] = np.concatenate(parts)
        else:
            out[name] = [x for p in parts for x in list(p)]
    out[SEGMENTS] = starts
    return out


def pool_matrix(eras):
    """``[(id, sessions (T_e,), matrix (K, T_e)), ...]`` -> (sessions (T,), matrix (K, T), segment starts)."""
    import numpy as np  # noqa: PLC0415
    eras = list(eras)
    starts = segment_starts([(era_id, sessions) for era_id, sessions, _ in eras])
    shapes = {np.asarray(m).shape[0] for _, _, m in eras}
    if len(shapes) != 1:
        raise PoolError("eras carry different numbers of rows (candidates)")
    for era_id, sessions, m in eras:
        if np.asarray(m).shape[1] != len(sessions):
            raise PoolError(f"era {era_id}: matrix columns differ from its sessions")
    sessions = np.concatenate([np.asarray(s, dtype=np.int64) for _, s, _ in eras])
    matrix = np.concatenate([np.asarray(m, dtype=np.float64) for _, _, m in eras], axis=1)
    return sessions, matrix, starts


def weighted_mean(values, weights) -> float:
    """sum w v / sum w; one era returns its value exactly (no arithmetic)."""
    values, weights = list(values), list(weights)
    if not values or len(values) != len(weights):
        raise PoolError("weighted_mean: one weight per value")
    if len(values) == 1:
        return values[0]
    total = float(sum(weights))
    if not total > 0 or any(w < 0 for w in weights):
        raise PoolError("weighted_mean: weights must be >= 0 with a positive sum")
    return float(sum(float(v) * float(w) for v, w in zip(values, weights)) / total)


def pooled_sha256(parts) -> str:
    """The identity of a pool of per-era SHA-256s (date order): one era is that era's SHA-256 (a one-era pool is the
    era, the same trial); several are the SHA-256 of canonical {"schema": atx.era-pool/v1, "parts": [...]}."""
    parts = [str(p) for p in parts]
    if not parts:
        raise PoolError("pooled_sha256: no part")
    if len(parts) == 1:
        return parts[0]
    doc = json.dumps({"schema": SCHEMA, "parts": parts}, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(doc.encode()).hexdigest()


def pool_label(dirs) -> str:
    """The name of a pooled cell: ``pool:`` + its era dirs joined by commas (forward slashes)."""
    return POOL_PREFIX + ",".join(str(d).replace("\\", "/") for d in dirs)


def era_weights_name(era_id: str) -> str:
    """The weights file of a non-anchor era in the pooled fit's output dir (bound to that era's role)."""
    return f"composition_weights.{check_id(era_id)}.json"
