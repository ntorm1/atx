#!/usr/bin/env python3
"""TRAIN-only composition weight fitter, rule ``mv-shrink-0.9-nonneg-v1``.

Writes the ``atx.dsl-composition-weights/v1`` JSON that ``atx-equity-strategy-ic``
reads through ``--composition-weights PATH --composition-weights-sha256 SHA``
(``strategy_ic_runner.cpp`` ``composition_weights``). Every library candidate id gets a
finite weight >= 0, and the file also carries provenance and per-candidate diagnostics,
including the standalone daily turnover ``tau_k``.

Inputs are all pinned by SHA-256: the library, the TRAIN role manifest (role payload
receipts are re-verified), the TRAIN ``orientations.json`` written by the runner, and the
runner's candidate signal cache ``DIR/<train-manifest-sha256>/<id>.{json,f64}``. Candidate
signals are streamed one at a time. Nothing after 2022-12-31 is read: the role must end
by 2023-01-01T00:00Z, and every cache sidecar must name the ``train`` role.

Rule, per scored decision d in [score_begin, score_end - 2):
  1. Exposures at d as ``strategy_price_exposures.cpp`` (price-risk-v1) defines them:
     beta over 252 intervals against the equal-weight market of valid guarded returns
     across ALL instruments (>= 126 pairs); vol, the sample SD over 63 intervals (>= 32
     returns); log of the mean raw dollar volume over 63 sessions (unusable days add 0).
     Used rows are member && present && all three exposures finite. Exposures are
     z-scored over the used rows (sample SD) and clipped to +-5.
  2. q_k(d) is the centered tied rank of sign_k * signal_k[d] over used rows with a
     finite signal, and 0 on other used rows. It is residualized by OLS on
     [1, z_beta, z_vol, z_ladv] over the used rows, then scaled to sum|q| = 1.
  3. f_k(d) = sum_i q_i r_i(d+2), where r(d+2) = close[d+2]/close[d+1] - 1 is a valid
     guarded return (else it contributes 0).
  4. mu = mean f, S = sample covariance (all TRAIN decisions), Sh = 0.1 S + 0.9 diag(S),
     w = solve(Sh, mu), w = max(w, 0), w /= sum(w).
  5. tau_k = mean over consecutive scored decisions of sum_i |q_k(d)_i - q_k(d-1)_i|.
     The first decision (deployment) is excluded and there is no price drift. A value
     over 0.70/day is flagged, never excluded.
     weighted_standalone_turnover = sum_k w_k tau_k.

Sign 0 (unoriented) -> weight 0, with no factor statistics, but tau_k is still
reported (it does not depend on the sign). Numpy only; single-threaded BLAS for
byte-deterministic output. The output is exclusive and is never overwritten.
"""
from __future__ import annotations

import os

# Deterministic reductions: pin BLAS/LAPACK to one thread before numpy loads.
for _var in ("OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "OMP_NUM_THREADS"):
    os.environ.setdefault(_var, "1")

import argparse  # noqa: E402
import hashlib  # noqa: E402
import json  # noqa: E402
import math  # noqa: E402
from pathlib import Path  # noqa: E402
import sys  # noqa: E402
import time  # noqa: E402

import numpy as np  # noqa: E402

RULE_ID = "mv-shrink-0.9-nonneg-v1"
SHRINK_LAMBDA = 0.9  # Sh = 0.1 * S + 0.9 * diag(S), written literally below
WEIGHTS_SCHEMA = "atx.dsl-composition-weights/v1"
LIBRARY_SCHEMA = "atx.dsl-ic-library/v1"
ORIENTATIONS_SCHEMA = "atx.dsl-ic-orientations/v1"
ROLE_SCHEMA = "atx.recent-research-role/v1"
CACHE_SCHEMA = "atx.dsl-candidate-signal/v1"
CACHE_LAYOUT = "date-major-little-endian-f64;non-finite-stored-as-quiet-NaN"
VM_EVAL_MODE = "ResearchFast;full-historical-asof-member-mask"
TRAIN_END_NS = 1_672_531_200_000_000_000  # 2023-01-01T00:00Z, exclusive: TRAIN is 2020-2022
DAY_NS = 86_400_000_000_000
METADATA_LIMIT = 1 << 20  # runner's metadata_text() bound; the weights file must fit too
TAU_LIMIT = 0.70
ANNUALIZATION = 252
# price-risk-v1: PriceExposureConfig defaults and strategy_price_exposures.cpp constants.
BETA_WINDOW, VOL_WINDOW, ADV_WINDOW = 252, 63, 63
MIN_RETURN_PAIRS, MIN_NAMES, CLIP_Z = 126, 50, 5.0
VOL_MIN_COUNT = max(2, (VOL_WINDOW + 1) // 2)
GUARD_ABS_LOG, GUARD_EXCESS_LOG = 1.5, 0.10
MIN_PIVOT, RELATIVE_SD_FLOOR, MIN_RESIDUAL_FRACTION = 1e-8, 1e-12, 1e-9


class FitError(Exception):
    """A loud refusal: bad pin, contract violation or degenerate fit."""


def require(condition, message: str) -> None:
    if not condition:
        raise FitError(message)


def is_hash(value) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def pinned_bytes(path: Path, pin: str, what: str) -> bytes:
    require(is_hash(pin), f"{what}: external lowercase SHA-256 required")
    data = Path(path).read_bytes()
    require(0 < len(data) <= METADATA_LIMIT, f"{what}: missing or over 1 MiB")
    require(hashlib.sha256(data).hexdigest() == pin, f"{what}: SHA-256 pin differs")
    return data


def unique_json(data: bytes, what: str):
    def pairs(items):
        keys = [k for k, _ in items]
        require(len(keys) == len(set(keys)), f"{what}: duplicate JSON key")
        return dict(items)

    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=pairs)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise FitError(f"{what}: JSON parse: {exc}") from exc


# ---------------------------------------------------------------- pinned inputs
def load_library(path: Path, pin: str) -> list[dict]:
    j = unique_json(pinned_bytes(path, pin, "library"), "library")
    require(j.get("schema") == LIBRARY_SCHEMA, "library: schema")
    rows = j.get("candidates")
    require(isinstance(rows, list) and 0 < len(rows) <= 256, "library: bounded candidate list")
    out, seen = [], set()
    for row in rows:
        cid, family, dsl = row.get("id"), row.get("family"), row.get("dsl")
        require(isinstance(cid, str) and isinstance(family, str) and isinstance(dsl, str) and dsl,
                "library: candidate id/family/dsl")
        require(cid not in seen, f"library: duplicate candidate {cid}")
        seen.add(cid)
        out.append({"id": cid, "family": family,
                    "dsl_sha256": hashlib.sha256(dsl.encode("utf-8")).hexdigest()})
    return out


def load_orientations(path: Path, pin: str, library: list[dict], library_sha: str,
                      train_sha: str) -> tuple[list[int], str]:
    j = unique_json(pinned_bytes(path, pin, "orientations"), "orientations")
    require(j.get("schema") == ORIENTATIONS_SCHEMA, "orientations: schema")
    require(j.get("library_sha256") == library_sha, "orientations: library_sha256 differs")
    require(j.get("train_manifest_sha256") == train_sha, "orientations: train_manifest_sha256 differs")
    require(is_hash(j.get("recipe_sha256")), "orientations: recipe_sha256")
    rows = j.get("candidates")
    require(isinstance(rows, list) and len(rows) == len(library), "orientations: candidate count")
    signs = []
    for row, c in zip(rows, library):
        require(row.get("id") == c["id"] and row.get("family") == c["family"] and
                row.get("dsl_sha256") == c["dsl_sha256"], f"orientations: candidate identity {c['id']}")
        sign = row.get("sign")
        require(type(sign) is int and sign in (-1, 0, 1), f"orientations: sign {c['id']}")
        signs.append(sign)
    return signs, j["recipe_sha256"]


class Role:
    """A pinned ``atx.recent-research-role/v1`` payload, restricted to TRAIN."""

    def __init__(self, manifest: Path, pin: str):
        text = pinned_bytes(manifest, pin, "role manifest")
        j = unique_json(text, "role manifest")
        require(j.get("schema") == ROLE_SCHEMA and j.get("status") == "complete", "role: schema/status")
        self.sha = pin
        self.dates, self.instruments = int(j["dates"]), int(j["instruments"])
        self.score_begin, self.score_end = int(j["score_begin"]), int(j["score_end"])
        start_ns, end_ns = int(j["score_start_ns"]), int(j["score_end_ns"])
        self.source_sha256 = j.get("source_sha256")
        d, n = self.dates, self.instruments
        require(0 < d <= 4096 and 0 < n <= 20000, "role: dimensions")
        require(0 <= self.score_begin < self.score_end == d, "role: score window")
        require(self.score_end - 2 - self.score_begin >= 2, "role: fewer than two scored decisions")
        # TRAIN only: nothing on or after 2023-01-01 may be scored or read.
        require(0 < start_ns < end_ns <= TRAIN_END_NS,
                "role: not TRAIN-only (score_end_ns after 2023-01-01T00:00Z)")
        files, base = j["files"], Path(manifest).parent
        self.sessions = self._payload(base, files, "sessions.i64", "<i8", d)
        self.ids = self._payload(base, files, "ids.u64", "<u8", n)
        s = self.sessions
        require(s[0] > 0 and bool(np.all(np.diff(s) > 0)) and int(s[-1]) < end_ns and
                bool(np.all(s % DAY_NS == 0)), "role: session axis")
        require(int(s[-1]) < TRAIN_END_NS, "role: session after 2022-12-31")
        require(int(np.searchsorted(s, start_ns, side="left")) == self.score_begin, "role: score boundary")
        require(self.ids[0] != 0 and bool(np.all(self.ids[1:] > self.ids[:-1])), "role: instrument axis")
        self.present = self._payload(base, files, "present.u8", "u1", d * n).reshape(d, n)
        self.member = self._payload(base, files, "member.u8", "u1", d * n).reshape(d, n)
        require(int(self.present.max(initial=0)) <= 1 and int(self.member.max(initial=0)) <= 1,
                "role: non-binary mask")
        self.close = self._payload(base, files, "close.f64", "<f8", d * n).reshape(d, n)
        self.raw_close = self._payload(base, files, "raw_close.f64", "<f8", d * n).reshape(d, n)
        self.volume = self._payload(base, files, "volume.f64", "<f8", d * n).reshape(d, n)
        present = self.present.astype(bool)
        with np.errstate(invalid="ignore"):
            for x, strict in ((self.close, True), (self.raw_close, True), (self.volume, False)):
                good = np.isfinite(x) & ((x > 0) if strict else (x >= 0))
                require(bool(np.all(np.where(present, good, np.isnan(x)))),
                        "role: missing/finite field contract")

    @staticmethod
    def _payload(base: Path, files: dict, name: str, dtype: str, count: int) -> np.ndarray:
        receipt = files[name]
        size = count * np.dtype(dtype).itemsize
        require(int(receipt["bytes"]) == size and is_hash(receipt["sha256"]), f"role: receipt {name}")
        data = (base / name).read_bytes()
        require(len(data) == size, f"role: file extent {name}")
        require(hashlib.sha256(data).hexdigest() == receipt["sha256"], f"role: payload SHA {name}")
        return np.frombuffer(data, dtype=dtype)


def load_candidate_signal(cache_dir: Path, cand: dict, role: Role) -> tuple[np.ndarray, str]:
    """Verified raw (unoriented) VM signal of one candidate, date-major (dates, instruments)."""
    cid = cand["id"]
    sidecar = cache_dir / f"{cid}.json"
    require(sidecar.is_file(), f"candidate cache: missing entry {cid} (run the IC runner with --candidate-cache)")
    data = sidecar.read_bytes()
    require(0 < len(data) <= METADATA_LIMIT, f"candidate cache: sidecar extent {cid}")
    j = unique_json(data, f"candidate cache {cid}")
    size = role.dates * role.instruments * 8
    sha = j.get("payload_sha256")
    require(j.get("schema") == CACHE_SCHEMA and j.get("candidate_id") == cid and
            j.get("dsl_sha256") == cand["dsl_sha256"] and j.get("role_manifest_sha256") == role.sha and
            j.get("eval_mode") == VM_EVAL_MODE and j.get("layout") == CACHE_LAYOUT and
            j.get("dates") == role.dates and j.get("instruments") == role.instruments and
            j.get("bytes") == size and j.get("payload") == f"{cid}.f64" and is_hash(sha),
            f"candidate cache: entry mismatch {cid}")
    require(j.get("role") == "train", f"candidate cache: entry {cid} is not a TRAIN-role signal")
    payload = (cache_dir / f"{cid}.f64").read_bytes()
    require(len(payload) == size, f"candidate cache: payload extent {cid}")
    require(hashlib.sha256(payload).hexdigest() == sha, f"candidate cache: payload SHA-256 mismatch {cid}")
    return np.frombuffer(payload, dtype="<f8").reshape(role.dates, role.instruments), sha


# ------------------------------------------------------- price-risk-v1 exposures
class PricePanel:
    """Valid guarded interval returns, equal-weight market and usable dollar volume.

    Mirrors strategy_price_exposures.cpp: interval t spans sessions (t-1, t]. Its return
    is valid iff both endpoints are present with finite positive close and raw_close,
    the simple return is finite, and |log adj| <= 1.5 and <= |log raw| + 0.10.
    """

    def __init__(self, close: np.ndarray, raw: np.ndarray, volume: np.ndarray, present: np.ndarray):
        d, n = close.shape
        self.returns = np.full((d, n), np.nan)
        pres = present.astype(bool)
        chunk = 128
        with np.errstate(all="ignore"):
            for a in range(1, d, chunk):
                b = min(d, a + chunk)
                c0, c1, r0, r1 = close[a - 1:b - 1], close[a:b], raw[a - 1:b - 1], raw[a:b]
                p0 = pres[a - 1:b - 1] & np.isfinite(c0) & np.isfinite(r0) & (c0 > 0) & (r0 > 0)
                p1 = pres[a:b] & np.isfinite(c1) & np.isfinite(r1) & (c1 > 0) & (r1 > 0)
                both = p0 & p1
                r = c1 / c0 - 1.0
                log_adj = np.log(c1) - np.log(c0)
                log_raw = np.log(r1) - np.log(r0)
                bad = (~np.isfinite(r) | (np.abs(log_adj) > GUARD_ABS_LOG) |
                       (np.abs(log_adj) > np.abs(log_raw) + GUARD_EXCESS_LOG))
                self.returns[a:b] = np.where(both & ~bad, r, np.nan)
            valid = ~np.isnan(self.returns)
            count = valid.sum(axis=1)
            total = np.where(valid, self.returns, 0.0).sum(axis=1)
            self.market = np.where(count > 0, total / np.maximum(count, 1), np.nan)
            dollars = raw * volume
            usable = pres & np.isfinite(raw) & (raw > 0) & np.isfinite(volume) & (volume >= 0) & np.isfinite(dollars)
            self.dollars = np.where(usable, dollars, 0.0)

    def exposures(self, d: int, cols: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        """(beta, vol, log_adv) at decision d for instrument columns ``cols`` and the ok mask."""
        block = max(BETA_WINDOW, VOL_WINDOW)
        first = d + 1 - block if d >= block else 1
        intervals = d + 1 - first if d >= first else 0
        beta_rows, vol_rows = min(BETA_WINDOW, intervals), min(VOL_WINDOW, intervals)
        m = len(cols)
        out = np.full((m, 3), np.nan)
        with np.errstate(all="ignore"):
            if beta_rows:
                # Two-pass over valid (return, market) pairs, as beta_of().
                r = self.returns[d + 1 - beta_rows:d + 1][:, cols]  # owned copy
                mk = self.market[d + 1 - beta_rows:d + 1]
                invalid = np.isnan(r)
                if np.isnan(mk).any():
                    invalid |= np.isnan(mk)[:, None]
                n = r.shape[0] - np.count_nonzero(invalid, axis=0)
                safe = np.maximum(n, 1)
                m = np.broadcast_to(mk[:, None], r.shape).copy()
                r[invalid] = 0.0
                m[invalid] = 0.0
                r -= r.sum(axis=0) / safe
                m -= m.sum(axis=0) / safe
                r[invalid] = 0.0
                m[invalid] = 0.0
                cov = np.einsum("ij,ij->j", r, m)
                var = np.einsum("ij,ij->j", m, m)
                out[:, 0] = np.where((n >= MIN_RETURN_PAIRS) & (var > 0), cov / np.where(var > 0, var, 1.0), np.nan)
            if vol_rows:
                # Two-pass sample SD over valid returns, as vol_of().
                r = self.returns[d + 1 - vol_rows:d + 1][:, cols]
                invalid = np.isnan(r)
                n = r.shape[0] - np.count_nonzero(invalid, axis=0)
                r[invalid] = 0.0
                r -= r.sum(axis=0) / np.maximum(n, 1)
                r[invalid] = 0.0
                squares = np.einsum("ij,ij->j", r, r)
                out[:, 1] = np.where(n >= VOL_MIN_COUNT, np.sqrt(squares / np.maximum(n - 1, 1)), np.nan)
            if d + 1 >= ADV_WINDOW:
                mean_dollars = self.dollars[d + 1 - ADV_WINDOW:d + 1][:, cols].sum(axis=0) / ADV_WINDOW
                out[:, 2] = np.where(mean_dollars > 0, np.log(np.where(mean_dollars > 0, mean_dollars, 1.0)), np.nan)
        ok = np.isfinite(out).all(axis=1)
        return out, ok


def neutralization_basis(exposures: np.ndarray) -> tuple[np.ndarray | None, str | None]:
    """Orthonormal basis of [1, z_beta, z_vol, z_ladv] over the used rows, or a refusal.

    Refusals mirror neutralize_target: too few names, a constant exposure column, or a
    Jacobi-equilibrated Cholesky pivot <= 1e-8. The C++ solves the normal equations with
    one refinement step. QR gives the same OLS residual to rounding.
    """
    m = exposures.shape[0]
    if m < MIN_NAMES:
        return None, "too-few-usable-names"
    z = np.empty((m, 3))
    for k in range(3):
        x = exposures[:, k]
        mean = x.sum() / m
        sd = math.sqrt(float(((x - mean) ** 2).sum()) / (m - 1))
        if not (math.isfinite(sd) and sd > RELATIVE_SD_FLOOR * float(np.abs(x).max())):
            return None, "constant-exposure"
        z[:, k] = np.clip((x - mean) / sd, -CLIP_Z, CLIP_Z)
    design = np.column_stack([np.ones(m), z])
    a = design.T @ design
    scale = 1.0 / np.sqrt(np.diag(a))
    c = a * scale[:, None] * scale[None, :]
    lower = np.zeros((4, 4))
    for j in range(4):
        pivot = c[j, j] - float(lower[j, :j] @ lower[j, :j])
        if not pivot > MIN_PIVOT:
            return None, "ill-conditioned-exposures"
        lower[j, j] = math.sqrt(pivot)
        for i in range(j + 1, 4):
            lower[i, j] = (c[i, j] - float(lower[i, :j] @ lower[j, :j])) / lower[j, j]
    basis, _ = np.linalg.qr(design)
    return basis, None


def centered_tied_ranks(values: np.ndarray, valid: np.ndarray) -> np.ndarray:
    """Row-wise centered tied ranks over ``valid`` cells, 0 elsewhere.

    Same value as each_centered_rank (strategy_ic_composition.cpp): a tie group
    [b, e) of the ascending order gets (b + (e - 1)) / (2 (n - 1)) - 0.5. Rows with
    fewer than two valid cells are all zero.
    """
    t, n = values.shape
    keyed = np.where(valid, values, np.inf)
    order = np.argsort(keyed, axis=1)  # order within a tie group does not change its rank
    ordered = np.take_along_axis(keyed, order, axis=1)
    count = valid.sum(axis=1)
    idx = np.arange(n)
    starts = np.ones((t, n), dtype=bool)
    starts[:, 1:] = ordered[:, 1:] != ordered[:, :-1]
    ends = np.ones((t, n), dtype=bool)
    ends[:, :-1] = ordered[:, :-1] != ordered[:, 1:]
    first = np.maximum.accumulate(np.where(starts, idx, 0), axis=1)
    last = np.minimum.accumulate(np.where(ends, idx, n - 1)[:, ::-1], axis=1)[:, ::-1]
    denominator = 2.0 * np.maximum(count - 1, 1).astype(np.float64)
    ranks = (first.astype(np.float64) + last.astype(np.float64)) / denominator[:, None] - 0.5
    keep = (idx[None, :] < count[:, None]) & (count[:, None] >= 2)
    out = np.zeros((t, n))
    np.put_along_axis(out, order, np.where(keep, ranks, 0.0), axis=1)
    return out


class Context:
    """Per-decision neutralization bases and forward returns, built once for all candidates."""

    def __init__(self, role: Role, panel: PricePanel, log=None):
        self.begin, self.end = role.score_begin, role.score_end - 2
        member = role.member.astype(bool) & role.present.astype(bool)  # the runner's effective member
        bases: list[tuple[np.ndarray, np.ndarray] | None] = []
        used_any = np.zeros(role.instruments, dtype=bool)
        self.refused: list[dict] = []
        started = time.perf_counter()
        for d in range(self.begin, self.end):
            cols = np.flatnonzero(member[d])
            exposures, ok = panel.exposures(d, cols)
            basis, reason = neutralization_basis(exposures[ok])
            if basis is None:
                self.refused.append({"decision_index": d, "reason": reason, "used_rows": int(ok.sum())})
                bases.append(None)
                continue
            used = cols[ok]
            bases.append((used, basis))
            used_any[used] = True
        self.columns = np.flatnonzero(used_any)
        position = np.full(role.instruments, -1, dtype=np.int64)
        position[self.columns] = np.arange(len(self.columns))
        t, width = self.end - self.begin, len(self.columns)
        self.used = np.zeros((t, width), dtype=bool)
        self.basis = np.zeros((t, 4, width))
        self.used_rows = np.zeros(t, dtype=np.int64)
        for row, item in enumerate(bases):
            if item is None:
                continue
            used, basis = item
            p = position[used]
            self.used[row, p] = True
            self.basis[row][:, p] = basis.T
            self.used_rows[row] = len(used)
        forward = panel.returns[self.begin + 2:self.end + 2][:, self.columns]
        self.forward = np.where(np.isnan(forward), 0.0, forward)
        if log:
            log(f"fit: context decisions={t} columns={width} refused={len(self.refused)} "
                f"seconds={time.perf_counter() - started:.2f}")

    def _project_out(self, x: np.ndarray) -> np.ndarray:
        coef = np.einsum("tkn,tn->tk", self.basis, x)  # basis rows are orthonormal on used rows
        return x - np.einsum("tkn,tk->tn", self.basis, coef)

    def book(self, signal: np.ndarray, sign: int) -> tuple[np.ndarray, int]:
        """Neutralized gross-1 standalone book q (decisions x columns) and its flat-decision count."""
        slab = signal[self.begin:self.end][:, self.columns]
        valid = self.used & np.isfinite(slab)
        q = centered_tied_ranks(slab, valid)
        if sign < 0:
            q = -q
        entry = np.abs(q).sum(axis=1)
        residual = self._project_out(self._project_out(q))  # one refinement step, as the C++
        gross = np.abs(residual).sum(axis=1)
        live = (entry > 0) & np.isfinite(gross) & (gross > MIN_RESIDUAL_FRACTION * entry)
        out = np.zeros_like(residual)
        out[live] = residual[live] / gross[live, None]
        return out, int((~live).sum())

    def factor_returns(self, q: np.ndarray) -> np.ndarray:
        return (q * self.forward).sum(axis=1)


def standalone_turnover(q: np.ndarray) -> float:
    """Mean over consecutive decisions of sum_i |q(d)_i - q(d-1)_i|; deployment excluded."""
    return float(np.abs(np.diff(q, axis=0)).sum(axis=1).mean())


def shrink_solution(mu: np.ndarray, cov: np.ndarray) -> np.ndarray:
    shrunk = 0.1 * cov + 0.9 * np.diag(np.diag(cov))
    return np.linalg.solve(shrunk, mu)


def fit_weights(factors: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """(normalized nonnegative weights, raw MV solution) for factor rows (candidates x decisions)."""
    mu = factors.mean(axis=1)
    cov = np.atleast_2d(np.cov(factors, ddof=1))
    try:
        raw = shrink_solution(mu, cov)
    except np.linalg.LinAlgError as exc:
        raise FitError(f"fit: shrunk covariance is singular: {exc}") from exc
    require(bool(np.all(np.isfinite(raw))), "fit: non-finite MV solution")
    clipped = np.where(raw > 0, raw, 0.0)
    total = float(clipped.sum())
    require(total > 0, "fit: every MV weight is <= 0 after the nonnegative clip")
    return clipped / total, raw


def factor_stats(f: np.ndarray) -> dict:
    mean = float(f.mean())
    sd = float(f.std(ddof=1))
    sharpe = mean / sd * math.sqrt(ANNUALIZATION) if sd > 0 else None
    return {"factor_mean": mean, "factor_sd": sd, "factor_sharpe_annualized": sharpe}


# ---------------------------------------------------------------------- output
def canonical_bytes(document: dict) -> bytes:
    return (json.dumps(document, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")


def publish_exclusive(path: Path, content: bytes) -> None:
    """Refuses an existing output; readers see no file or its complete fsynced bytes."""
    path = Path(path)
    require(not path.exists(), f"output exists; refusing overwrite: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name("." + path.name + ".pending")
    try:
        stream = pending.open("xb")
    except FileExistsError as exc:
        raise FitError(f"stale or concurrent partial output; inspect and remove: {pending}") from exc
    with stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())
    try:
        os.link(pending, path)  # no-replace publication
    except FileExistsError as exc:
        raise FitError(f"output appeared concurrently; refusing overwrite: {path}") from exc
    finally:
        pending.unlink()


def fit(args, log=None) -> dict:
    started = time.perf_counter()
    library = load_library(args.library, args.library_sha256)
    signs, orientation_recipe = load_orientations(args.orientations, args.orientations_sha256, library,
                                                  args.library_sha256, args.train_sha256)
    require(not Path(args.output).exists(), f"output exists; refusing overwrite: {args.output}")
    cache_dir = Path(args.candidate_cache) / args.train_sha256
    require(cache_dir.is_dir(), f"candidate cache: no directory for the TRAIN role: {cache_dir}")
    role = Role(args.train, args.train_sha256)
    panel = PricePanel(role.close, role.raw_close, role.volume, role.present)
    context = Context(role, panel, log)
    first_label = role.score_begin + 2
    window = {
        "decision_begin": role.score_begin, "decision_end_exclusive": context.end,
        "decisions": context.end - context.begin,
        "first_decision_session_ns": int(role.sessions[context.begin]),
        "last_decision_session_ns": int(role.sessions[context.end - 1]),
        "first_label_session_ns": int(role.sessions[first_label]),
        "last_label_session_ns": int(role.sessions[context.end + 1]),
        "turnover_transitions": context.end - context.begin - 1,
    }
    del panel
    role.close = role.raw_close = role.volume = role.present = role.member = None  # not needed past here
    live_rows = context.used_rows[context.used_rows > 0]
    rows: list[dict] = []
    factors: dict[int, np.ndarray] = {}
    for k, (cand, sign) in enumerate(zip(library, signs)):
        tick = time.perf_counter()
        signal, payload_sha = load_candidate_signal(cache_dir, cand, role)
        q, flat = context.book(signal, sign)
        del signal
        tau = standalone_turnover(q)
        row = {"id": cand["id"], "family": cand["family"], "sign": sign, "dsl_sha256": cand["dsl_sha256"],
               "cache_payload_sha256": payload_sha, "flat_decisions": flat, "tau": tau,
               "tau_over_limit": tau > TAU_LIMIT}
        if sign == 0:
            row.update(status="unoriented-sign-0", factor_mean=None, factor_sd=None,
                       factor_sharpe_annualized=None)
        else:
            factors[k] = context.factor_returns(q)
            row.update(factor_stats(factors[k]))
            row["status"] = "fitted" if row["factor_sd"] > 0 else "degenerate-zero-variance"
        rows.append(row)
        if log:
            log(f"fit: {k + 1}/{len(library)} {cand['id']} sign={sign} tau={tau:.4f} "
                f"flat={flat} seconds={time.perf_counter() - tick:.2f}")
    active = [k for k, row in enumerate(rows) if row["status"] == "fitted"]
    require(active, "fit: no oriented candidate with a non-degenerate factor series")
    matrix = np.vstack([factors[k] for k in active])
    weights, raw = fit_weights(matrix)
    for row in rows:
        row.update(weight=0.0, mv_solution=None, clipped=False)
    for k, w, r in zip(active, weights, raw):
        rows[k].update(weight=float(w), mv_solution=float(r), clipped=not r > 0)
    blend = weights @ matrix
    weighted_tau = float(sum(row["weight"] * row["tau"] for row in rows))
    document = {
        "schema": WEIGHTS_SCHEMA,
        "library_sha256": args.library_sha256,
        "weights": {row["id"]: row["weight"] for row in rows},
        "provenance": {
            "rule": RULE_ID,
            "lambda": SHRINK_LAMBDA,
            "shrinkage": "Sh=0.1*S+0.9*diag(S);S=sample-covariance-ddof1;w=solve(Sh,mu);w=max(w,0);w/=sum(w)",
            "signs": "TRAIN-orientations-artifact-sign;sign0-weight0",
            "factor": "q=centered-tied-rank(sign*signal)-over-used-rows-finite-signal-else-0;"
                      "OLS-residual-on-[1,z_beta,z_vol,z_ladv];sum|q|=1;f(d)=sum(q*r[d+2]);"
                      "r[d+2]=close[d+2]/close[d+1]-1-valid-guarded-else-0",
            "neutralization": "price-risk-v1;beta252-vs-all-instrument-equal-weight-market-min126;"
                              "vol63-sample-sd-min32;log-mean-raw-dollar-volume63-absent-0;"
                              "used=member&present&exposures-finite;z-sample-sd-clip5;min_names50",
            "turnover": "tau=mean_d(sum_i|q(d)_i-q(d-1)_i|);consecutive-scored-TRAIN-decisions;"
                        "deployment-excluded;no-price-drift;flag-only",
            "tau_limit": TAU_LIMIT,
            "tau_flagged": [row["id"] for row in rows if row["tau_over_limit"]],
            "weighted_standalone_turnover": weighted_tau,
            "library_sha256": args.library_sha256,
            "role_manifest_sha256": args.train_sha256,
            "role_source_sha256": role.source_sha256,
            "orientations_sha256": args.orientations_sha256,
            "orientations_recipe_sha256": orientation_recipe,
            "script_sha256": hashlib.sha256(Path(__file__).resolve().read_bytes()).hexdigest(),
            "window": window,
            "neutralization_refused_decisions": context.refused,
            "used_rows_unrefused": {"min": int(live_rows.min()) if live_rows.size else 0,
                                    "max": int(live_rows.max()) if live_rows.size else 0},
            "fitted_candidates": len(active),
            "blend_in_sample_TRAIN_diagnostic": factor_stats(blend),
            "candidates": rows,
        },
    }
    content = canonical_bytes(document)
    require(len(content) <= METADATA_LIMIT, "output: weights JSON exceeds the runner's 1 MiB metadata bound")
    publish_exclusive(Path(args.output), content)
    summary = {"output": str(args.output), "sha256": hashlib.sha256(content).hexdigest(),
               "candidates": len(rows), "fitted": len(active),
               "nonzero_weights": int(sum(1 for row in rows if row["weight"] > 0)),
               "weighted_standalone_turnover": weighted_tau,
               "tau_flagged": document["provenance"]["tau_flagged"],
               "refused_decisions": len(context.refused),
               "seconds": round(time.perf_counter() - started, 2)}
    return summary


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--library-sha256", required=True)
    p.add_argument("--train", type=Path, required=True, help="TRAIN role manifest.json")
    p.add_argument("--train-sha256", required=True)
    p.add_argument("--orientations", type=Path, required=True, help="TRAIN orientations.json from the IC runner")
    p.add_argument("--orientations-sha256", required=True)
    p.add_argument("--candidate-cache", type=Path, required=True, help="runner --candidate-cache DIR")
    p.add_argument("--output", type=Path, required=True, help="new weights JSON path (never overwritten)")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        summary = fit(args, log=lambda line: print(line, file=sys.stderr, flush=True))
    except FitError as exc:
        print(f"fit_composition_weights: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
