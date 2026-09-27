#!/usr/bin/env python3
"""TRAIN-only admission screen ``v3-admit-v1`` and composition weight fitter ``mv-shrink-0.9-nonneg-v1``.

Writes into a new output directory (published atomically, never overwritten):
  composition_weights.json  ``atx.dsl-composition-weights/v1``, read by ``atx-equity-strategy-ic
                            --composition-weights PATH --composition-weights-sha256 SHA``. It holds a
                            weight >= 0 for every library id, the top-level ``train_manifest_sha256``,
                            and ``signs`` (id -> 1 | -1 for every id whose applied sign is nonzero,
                            so for every id with a weight above 0).
  admission.json / .csv     ``atx.dsl-admission/v1`` decision table (only with ``--screen v3-admit-v1``).

Inputs are pinned by SHA-256: the library, the TRAIN role manifest, the runner's TRAIN
``orientations.json``, and the runner's candidate signal cache
``DIR/<train-manifest-sha256>/<id>.{json,f64}``. Nothing after 2022-12-31 is read: the role must
end by 2023-01-01T00:00Z, and every cache sidecar must name the ``train`` role.

Per scored decision d in [score_begin, score_end - 2), candidate k:
  1. Exposures at d as ``strategy_price_exposures.cpp`` (price-risk-v1) defines them:
     beta over 252 intervals against the equal-weight market of valid guarded returns across ALL
     instruments (>= 126 pairs); vol, the sample SD over 63 intervals (>= 32); log of the mean raw
     dollar volume over 63 sessions (unusable days add 0). Used rows are member && present && all
     three exposures finite. Exposures are z-scored over the used rows (sample SD) and clipped to +-5.
  2. q_k(d) is the centered tied rank of signal_k[d] over used rows with a finite signal, and 0 on
     other used rows. It is residualized by OLS on [1, z_beta, z_vol, z_ladv] over the used rows,
     then scaled to sum|q| = 1. This book is UNSIGNED; a sign s only negates it (exactly).
  3. f_k(d) = sum_i q_i r_i(d+2), where r(d+2) = close[d+2]/close[d+1] - 1 is a valid guarded return
     (else it contributes 0). A decision with no live book (flat) is NaN in the stored series.
  4. tau_k = mean over consecutive scored decisions of sum_i |q_k(d)_i - q_k(d-1)_i|. Deployment
     is excluded and there is no drift.

Screen v3-admit-v1 (declared by root before any v3 measurement). FIT is decision sessions in
[2020-01-01, 2022-01-01) and HOLD is [2022-01-01, 2023-01-01). Statistics use live (finite) days.
  orientation  s_k = sign(mean f_k over FIT). A disagreement with the runner's sign is reported.
  checks       insufficient (< 250 live FIT days), turnover (tau_k > 0.70), unstable
               (not (FIT Sharpe of s_k f_k > 0 and HOLD mean of s_k f_k > 0)). Every failed check is
               listed; the status is the first failure in that order.
  redundancy   survivors in descending FIT Sharpe order (ties: library order). k is admitted unless
               |corr(f_k, f_j)| > 0.70 over FIT days where both are live, for an already-admitted j.
               Then it is reject_redundant(j), naming the largest |rho|. A pair with < 250 common days
               (or undefined rho) counts as uncorrelated and is noted.
Weights: mu and S over ALL TRAIN decisions of s_k f_k with flat days as 0; Sh = 0.1 S + 0.9 diag(S),
w = solve(Sh, mu), w = max(w, 0), w /= sum(w). Only admitted candidates (screen mode), or every
runner-oriented candidate (``--screen none``, the T9 rule), take part. Everyone else gets weight 0.

Incremental: with ``--work-dir`` the per-day price-risk context and each candidate's unsigned factor
record (f_k, tau_k, live counts) are persisted and SHA-verified on read. A mismatch means recompute.
Records are keyed by (TRAIN role manifest SHA, semantics tag, cache payload SHA). ``--max-seconds``
and ``--max-new-candidates`` stop cleanly between candidates with exit code 3 and publish nothing; a
rerun computes only what is missing. Outputs are byte-identical whichever path produced them.
Exit codes: 0 complete; 1 refused (nothing published); 3 incomplete (rerun); 4 admission published,
no weights (nothing admitted or no positive weight). Numpy only, single-threaded BLAS.
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
import shutil  # noqa: E402
import sys  # noqa: E402
import time  # noqa: E402

import numpy as np  # noqa: E402

RULE_ID = "mv-shrink-0.9-nonneg-v1"
SHRINK_LAMBDA = 0.9  # Sh = 0.1 * S + 0.9 * diag(S), written literally below
SCREEN_ID = "v3-admit-v1"
SCREENS = ("none", SCREEN_ID)
WEIGHTS_SCHEMA = "atx.dsl-composition-weights/v1"
ADMISSION_SCHEMA = "atx.dsl-admission/v1"
LIBRARY_SCHEMA = "atx.dsl-ic-library/v1"
ORIENTATIONS_SCHEMA = "atx.dsl-ic-orientations/v1"
ROLE_SCHEMA = "atx.recent-research-role/v1"
CACHE_SCHEMA = "atx.dsl-candidate-signal/v1"
CACHE_LAYOUT = "date-major-little-endian-f64;non-finite-stored-as-quiet-NaN"
VM_EVAL_MODE = "ResearchFast;full-historical-asof-member-mask"
FACTOR_SCHEMA = "atx.fit-candidate-factor/v1"
CONTEXT_SCHEMA = "atx.fit-price-risk-context/v1"
# Bump these when anything that changes the context or a factor record changes: old work is ignored.
CONTEXT_SEMANTICS = ("price-risk-v1;beta252-min126-all-instrument-market;vol63-min32;ladv63;"
                     "used=member&present&ok;z-sample-sd-clip5;min50;pivot1e-8;qr-basis;fwd=r[d+2]-valid-else-0;v1")
FACTOR_SEMANTICS = ("unsigned-centered-tied-rank;used-rows-finite-signal;ols-residual-2-pass;gross1;"
                    "residual>1e-9*entry;f=sum(q*fwd);flat=NaN;tau=mean-consecutive-no-drift;v1")
SEMANTICS_TAG = hashlib.sha256(f"{CONTEXT_SEMANTICS}|{FACTOR_SEMANTICS}".encode()).hexdigest()[:16]
OUTPUT_WEIGHTS, OUTPUT_ADMISSION, OUTPUT_ADMISSION_CSV = (
    "composition_weights.json", "admission.json", "admission.csv")
FIT_BEGIN_NS = 1_577_836_800_000_000_000  # 2020-01-01T00:00Z
HOLD_BEGIN_NS = 1_640_995_200_000_000_000  # 2022-01-01T00:00Z
TRAIN_END_NS = 1_672_531_200_000_000_000  # 2023-01-01T00:00Z, exclusive: TRAIN is 2020-2022
DAY_NS = 86_400_000_000_000
METADATA_LIMIT = 1 << 20  # runner's metadata_text() bound; the weights file must fit too
TAU_LIMIT, RHO_LIMIT = 0.70, 0.70
MIN_FIT_DAYS, MIN_COMMON_DAYS = 250, 250
ANNUALIZATION = 252
EXIT_OK, EXIT_REFUSED, EXIT_INCOMPLETE, EXIT_NO_WEIGHTS = 0, 1, 3, 4
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


class RoleManifest:
    """A pinned ``atx.recent-research-role/v1`` manifest restricted to TRAIN.

    Construction verifies the manifest pin, the small axes and the TRAIN seal. The price payload is
    read (and receipt-verified) only by ``payload()``, i.e. only when a context must be built.
    """

    def __init__(self, manifest: Path, pin: str):
        j = unique_json(pinned_bytes(manifest, pin, "role manifest"), "role manifest")
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
        self._files, self._base = j["files"], Path(manifest).parent
        self.sessions = self._read("sessions.i64", "<i8", d)
        self.ids = self._read("ids.u64", "<u8", n)
        s = self.sessions
        require(s[0] > 0 and bool(np.all(np.diff(s) > 0)) and int(s[-1]) < end_ns and
                bool(np.all(s % DAY_NS == 0)), "role: session axis")
        require(int(s[-1]) < TRAIN_END_NS, "role: session after 2022-12-31")
        require(int(np.searchsorted(s, start_ns, side="left")) == self.score_begin, "role: score boundary")
        require(self.ids[0] != 0 and bool(np.all(self.ids[1:] > self.ids[:-1])), "role: instrument axis")
        self.begin, self.end = self.score_begin, self.score_end - 2  # scored decisions with a d+2 label

    def _read(self, name: str, dtype: str, count: int) -> np.ndarray:
        receipt = self._files[name]
        size = count * np.dtype(dtype).itemsize
        require(int(receipt["bytes"]) == size and is_hash(receipt["sha256"]), f"role: receipt {name}")
        data = (self._base / name).read_bytes()
        require(len(data) == size, f"role: file extent {name}")
        require(hashlib.sha256(data).hexdigest() == receipt["sha256"], f"role: payload SHA {name}")
        return np.frombuffer(data, dtype=dtype)

    def payload(self) -> dict:
        d, n = self.dates, self.instruments
        out = {"present": self._read("present.u8", "u1", d * n).reshape(d, n),
               "member": self._read("member.u8", "u1", d * n).reshape(d, n)}
        require(int(out["present"].max(initial=0)) <= 1 and int(out["member"].max(initial=0)) <= 1,
                "role: non-binary mask")
        for key, name in (("close", "close.f64"), ("raw_close", "raw_close.f64"), ("volume", "volume.f64")):
            out[key] = self._read(name, "<f8", d * n).reshape(d, n)
        present = out["present"].astype(bool)
        with np.errstate(invalid="ignore"):
            for key, strict in (("close", True), ("raw_close", True), ("volume", False)):
                x = out[key]
                good = np.isfinite(x) & ((x > 0) if strict else (x >= 0))
                require(bool(np.all(np.where(present, good, np.isnan(x)))), "role: missing/finite field contract")
        return out

    def window(self) -> dict:
        s = self.sessions
        return {"decision_begin": self.begin, "decision_end_exclusive": self.end,
                "decisions": self.end - self.begin,
                "first_decision_session_ns": int(s[self.begin]), "last_decision_session_ns": int(s[self.end - 1]),
                "first_label_session_ns": int(s[self.begin + 2]), "last_label_session_ns": int(s[self.end + 1]),
                "turnover_transitions": self.end - self.begin - 1}


def candidate_payload_sha(cache_dir: Path, cand: dict, role: RoleManifest) -> str:
    """Validated sidecar of one runner cache entry; returns its payload SHA-256 (identity key)."""
    cid = cand["id"]
    sidecar = cache_dir / f"{cid}.json"
    require(sidecar.is_file(), f"candidate cache: missing entry {cid} (run the IC runner with --candidate-cache)")
    data = sidecar.read_bytes()
    require(0 < len(data) <= METADATA_LIMIT, f"candidate cache: sidecar extent {cid}")
    j = unique_json(data, f"candidate cache {cid}")
    sha = j.get("payload_sha256")
    require(j.get("schema") == CACHE_SCHEMA and j.get("candidate_id") == cid and
            j.get("dsl_sha256") == cand["dsl_sha256"] and j.get("role_manifest_sha256") == role.sha and
            j.get("eval_mode") == VM_EVAL_MODE and j.get("layout") == CACHE_LAYOUT and
            j.get("dates") == role.dates and j.get("instruments") == role.instruments and
            j.get("bytes") == role.dates * role.instruments * 8 and j.get("payload") == f"{cid}.f64" and
            is_hash(sha), f"candidate cache: entry mismatch {cid}")
    require(j.get("role") == "train", f"candidate cache: entry {cid} is not a TRAIN-role signal")
    return sha


def load_candidate_signal(cache_dir: Path, cid: str, sha: str, role: RoleManifest) -> np.ndarray:
    """The verified raw (unoriented) VM signal, date-major (dates, instruments)."""
    size = role.dates * role.instruments * 8
    payload = (cache_dir / f"{cid}.f64").read_bytes()
    require(len(payload) == size, f"candidate cache: payload extent {cid}")
    require(hashlib.sha256(payload).hexdigest() == sha, f"candidate cache: payload SHA-256 mismatch {cid}")
    return np.frombuffer(payload, dtype="<f8").reshape(role.dates, role.instruments)


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


def canonical_bytes(document: dict) -> bytes:
    return (json.dumps(document, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")


def canonical_compact(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def write_synced(path: Path, data: bytes, mode: str = "wb") -> None:
    with Path(path).open(mode) as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


class Context:
    """Per-decision neutralization bases and forward returns, shared by every candidate.

    ``digest`` is the SHA-256 of the canonical metadata. That metadata holds the per-array SHA-256s,
    the role manifest SHA, the window and the refused decisions. A factor record names the digest of
    the context it was computed under.
    """

    ARRAYS = (("columns", "<i8"), ("used", "u1"), ("basis", "<f8"), ("forward", "<f8"), ("used_rows", "<i8"))

    def __init__(self, role_sha: str, begin: int, end: int, arrays: dict, refused: list, meta: dict | None = None):
        self.role_sha, self.begin, self.end, self.refused = role_sha, begin, end, refused
        self.columns = arrays["columns"]
        self.used = arrays["used"].astype(bool)
        self.basis, self.forward, self.used_rows = arrays["basis"], arrays["forward"], arrays["used_rows"]
        if meta is None:
            meta = {"schema": CONTEXT_SCHEMA, "semantics": CONTEXT_SEMANTICS, "role_manifest_sha256": role_sha,
                    "decision_begin": begin, "decision_end_exclusive": end, "refused": refused,
                    "arrays": {name: {"dtype": dtype, "shape": list(arrays[name].shape),
                                      "sha256": hashlib.sha256(self._bytes(name, dtype)).hexdigest()}
                               for name, dtype in self.ARRAYS}}
        self.meta = meta
        self.digest = hashlib.sha256(canonical_compact(meta)).hexdigest()

    def _bytes(self, name: str, dtype: str) -> bytes:
        return np.ascontiguousarray(getattr(self, name).astype(dtype, copy=False)).tobytes()

    @classmethod
    def build(cls, role: RoleManifest, log=None) -> "Context":
        started = time.perf_counter()
        p = role.payload()
        panel = PricePanel(p["close"], p["raw_close"], p["volume"], p["present"])
        member = p["member"].astype(bool) & p["present"].astype(bool)  # the runner's effective member
        del p
        bases: list[tuple[np.ndarray, np.ndarray] | None] = []
        used_any = np.zeros(role.instruments, dtype=bool)
        refused: list[dict] = []
        for d in range(role.begin, role.end):
            cols = np.flatnonzero(member[d])
            exposures, ok = panel.exposures(d, cols)
            basis, reason = neutralization_basis(exposures[ok])
            if basis is None:
                refused.append({"decision_index": d, "reason": reason, "used_rows": int(ok.sum())})
                bases.append(None)
                continue
            used = cols[ok]
            bases.append((used, basis))
            used_any[used] = True
        columns = np.flatnonzero(used_any).astype(np.int64)
        position = np.full(role.instruments, -1, dtype=np.int64)
        position[columns] = np.arange(len(columns))
        t, width = role.end - role.begin, len(columns)
        used_mask = np.zeros((t, width), dtype=bool)
        basis_all = np.zeros((t, 4, width))
        used_rows = np.zeros(t, dtype=np.int64)
        for row, item in enumerate(bases):
            if item is None:
                continue
            used, basis = item
            p_ = position[used]
            used_mask[row, p_] = True
            basis_all[row][:, p_] = basis.T
            used_rows[row] = len(used)
        forward = panel.returns[role.begin + 2:role.end + 2][:, columns]
        forward = np.where(np.isnan(forward), 0.0, forward)
        ctx = cls(role.sha, role.begin, role.end, {"columns": columns, "used": used_mask, "basis": basis_all,
                                                     "forward": forward, "used_rows": used_rows}, refused)
        if log:
            log(f"fit: context built decisions={t} columns={width} refused={len(refused)} "
                f"seconds={time.perf_counter() - started:.2f}")
        return ctx

    def save(self, directory: Path) -> None:
        """Atomic replace of the context directory (arrays first, metadata last)."""
        directory = Path(directory)
        partial = directory.with_name(directory.name + f".partial-{os.getpid()}")
        if partial.exists():
            shutil.rmtree(partial)
        partial.mkdir(parents=True)
        for name, dtype in self.ARRAYS:
            write_synced(partial / f"{name}.bin", self._bytes(name, dtype))
        write_synced(partial / "context.json", canonical_bytes(self.meta))
        if directory.exists():
            shutil.rmtree(directory)
        os.rename(partial, directory)

    @classmethod
    def load(cls, directory: Path, role: RoleManifest) -> "Context | None":
        """The verified cached context, or None (absent, stale or corrupt: rebuild)."""
        directory = Path(directory)
        try:
            meta = json.loads((directory / "context.json").read_bytes())
            if (meta.get("schema") != CONTEXT_SCHEMA or meta.get("semantics") != CONTEXT_SEMANTICS or
                    meta.get("role_manifest_sha256") != role.sha or meta.get("decision_begin") != role.begin or
                    meta.get("decision_end_exclusive") != role.end or not isinstance(meta.get("refused"), list)):
                return None
            arrays = {}
            for name, dtype in cls.ARRAYS:
                spec = meta["arrays"][name]
                data = (directory / f"{name}.bin").read_bytes()
                if spec["dtype"] != dtype or hashlib.sha256(data).hexdigest() != spec["sha256"]:
                    return None
                arrays[name] = np.frombuffer(data, dtype=dtype).reshape(spec["shape"])
            t, width = role.end - role.begin, arrays["columns"].shape[0]
            if (arrays["used"].shape != (t, width) or arrays["basis"].shape != (t, 4, width) or
                    arrays["forward"].shape != (t, width) or arrays["used_rows"].shape != (t,)):
                return None
            return cls(role.sha, role.begin, role.end, arrays, meta["refused"], meta)
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            return None

    def used_rows_summary(self) -> dict:
        live = self.used_rows[self.used_rows > 0]
        return {"min": int(live.min()) if live.size else 0, "max": int(live.max()) if live.size else 0}

    def _project_out(self, x: np.ndarray) -> np.ndarray:
        coef = np.einsum("tkn,tn->tk", self.basis, x)  # basis rows are orthonormal on used rows
        return x - np.einsum("tkn,tk->tn", self.basis, coef)

    def book(self, signal: np.ndarray, sign: int = 1) -> tuple[np.ndarray, np.ndarray]:
        """Neutralized gross-1 standalone book q (decisions x columns) and its live-decision mask."""
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
        return out, live

    def factor_returns(self, q: np.ndarray) -> np.ndarray:
        return (q * self.forward).sum(axis=1)


# ------------------------------------------------------------- factor records
def seal(body: dict) -> dict:
    out = dict(body)
    out["content_sha256"] = hashlib.sha256(canonical_compact(body)).hexdigest()
    return out


def factor_record(context: Context, signal: np.ndarray, payload_sha: str, cand: dict) -> dict:
    """The candidate's unsigned factor series (None on flat days), tau and live count."""
    q, live = context.book(signal, 1)
    f = context.factor_returns(q)
    return seal({
        "schema": FACTOR_SCHEMA, "context_semantics": CONTEXT_SEMANTICS, "factor_semantics": FACTOR_SEMANTICS,
        "role_manifest_sha256": context.role_sha, "context_sha256": context.digest,
        "cache_payload_sha256": payload_sha, "candidate_id": cand["id"], "dsl_sha256": cand["dsl_sha256"],
        "decisions": int(len(f)), "f_unsigned": [float(x) if ok else None for x, ok in zip(f, live)],
        "tau": standalone_turnover(q), "live_decisions": int(live.sum()),
        "context_refused": context.refused, "context_used_rows_unrefused": context.used_rows_summary()})


def record_valid(j, payload_sha: str, role: RoleManifest) -> bool:
    try:
        if not isinstance(j, dict):
            return False
        body = {k: v for k, v in j.items() if k != "content_sha256"}
        f = j.get("f_unsigned")
        return (j.get("content_sha256") == hashlib.sha256(canonical_compact(body)).hexdigest() and
                j.get("schema") == FACTOR_SCHEMA and j.get("context_semantics") == CONTEXT_SEMANTICS and
                j.get("factor_semantics") == FACTOR_SEMANTICS and j.get("role_manifest_sha256") == role.sha and
                j.get("cache_payload_sha256") == payload_sha and is_hash(j.get("context_sha256")) and
                j.get("decisions") == role.end - role.begin and isinstance(f, list) and len(f) == role.end - role.begin
                and all(v is None or type(v) is float for v in f) and type(j.get("tau")) is float and
                type(j.get("live_decisions")) is int)
    except (ValueError, TypeError):
        return False


class WorkStore:
    """Persistent incremental state: ``<work>/<train-sha>/<semantics-tag>/{context,factors}``."""

    def __init__(self, root: Path, role: RoleManifest):
        self.role = role
        self.base = Path(root) / role.sha / SEMANTICS_TAG
        self.factors = self.base / "factors"
        self.context_dir = self.base / "context"

    def get(self, payload_sha: str) -> dict | None:
        path = self.factors / f"{payload_sha}.json"
        try:
            j = json.loads(path.read_bytes())
        except (OSError, ValueError):
            return None
        return j if record_valid(j, payload_sha, self.role) else None

    def put(self, record: dict) -> None:
        self.factors.mkdir(parents=True, exist_ok=True)
        path = self.factors / f"{record['cache_payload_sha256']}.json"
        partial = path.with_name(path.name + f".partial-{os.getpid()}")
        write_synced(partial, canonical_compact(record))
        os.replace(partial, path)

    def load_context(self) -> Context | None:
        return Context.load(self.context_dir, self.role)

    def save_context(self, context: Context) -> None:
        context.save(self.context_dir)


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


# ---------------------------------------------------------------------- screen
def _stats(x: np.ndarray) -> tuple[float | None, float | None]:
    """(mean, annualized Sharpe) of live values; Sharpe is None below 2 values or at zero SD."""
    if x.size == 0:
        return None, None
    mean = float(x.mean())
    if x.size < 2:
        return mean, None
    sd = float(x.std(ddof=1))
    return mean, (mean / sd * math.sqrt(ANNUALIZATION) if sd > 0 else None)


def pair_correlation(a: np.ndarray, b: np.ndarray, mask: np.ndarray) -> tuple[float | None, int]:
    """Pearson rho over ``mask`` days where both are live, and that day count."""
    both = mask & np.isfinite(a) & np.isfinite(b)
    n = int(both.sum())
    if n < 2:
        return None, n
    x, y = a[both] - a[both].mean(), b[both] - b[both].mean()
    den = math.sqrt(float((x * x).sum()) * float((y * y).sum()))
    return (float((x * y).sum()) / den if den > 0 else None), n


def screen_v3(factors: np.ndarray, taus: list[float], ids: list[str], fit_mask: np.ndarray,
              hold_mask: np.ndarray) -> list[dict]:
    """v3-admit-v1 decisions for unsigned factor rows (candidates x decisions, NaN = flat day)."""
    rows = []
    for k, f in enumerate(factors):
        live = np.isfinite(f)
        fit, hold = live & fit_mask, live & hold_mask
        raw_mean, _ = _stats(f[fit])
        s = 0 if raw_mean is None or raw_mean == 0 else (1 if raw_mean > 0 else -1)
        g = s * f
        fit_mean, fit_sharpe = _stats(g[fit])
        hold_mean, hold_sharpe = _stats(g[hold])
        failed = []
        if int(fit.sum()) < MIN_FIT_DAYS:
            failed.append("insufficient")
        if taus[k] > TAU_LIMIT:
            failed.append("turnover")
        if not (fit_sharpe is not None and fit_sharpe > 0 and hold_mean is not None and hold_mean > 0):
            failed.append("unstable")
        rows.append({"s_k": s, "tau": taus[k], "fit_days": int(fit.sum()), "fit_mean": fit_mean,
                     "fit_sharpe": fit_sharpe, "hold_days": int(hold.sum()), "hold_mean": hold_mean,
                     "hold_sharpe": hold_sharpe, "failed_checks": failed,
                     "status": "reject_" + failed[0] if failed else None, "redundant_with": None,
                     "admission_rank": None, "low_overlap_with": []})
    survivors = sorted((k for k, r in enumerate(rows) if r["status"] is None),
                       key=lambda k: (-rows[k]["fit_sharpe"], k))
    admitted: list[int] = []
    for k in survivors:
        worst = None
        for j in admitted:
            rho, n = pair_correlation(factors[k], factors[j], fit_mask)
            if rho is None or n < MIN_COMMON_DAYS:
                rows[k]["low_overlap_with"].append(ids[j])  # treated as uncorrelated, noted
                continue
            if abs(rho) > RHO_LIMIT and (worst is None or abs(rho) > worst[0]):
                worst = (abs(rho), j)
        if worst is None:
            admitted.append(k)
            rows[k]["status"], rows[k]["admission_rank"] = "admitted", len(admitted)
        else:
            rows[k]["status"], rows[k]["redundant_with"] = "reject_redundant", ids[worst[1]]
    for k, row in enumerate(rows):
        best = None
        for j in admitted:
            if j == k:
                continue
            rho, n = pair_correlation(factors[k], factors[j], fit_mask)
            if rho is not None and n >= MIN_COMMON_DAYS and (best is None or abs(rho) > best[0]):
                best = (abs(rho), j)
        row["max_abs_rho"] = best[0] if best else None
        row["max_abs_rho_with"] = ids[best[1]] if best else None
    return rows


ADMISSION_STATUSES = ("admitted", "reject_insufficient", "reject_turnover", "reject_unstable", "reject_redundant")
CSV_COLUMNS = ("id", "family", "status", "failed_checks", "redundant_with", "admission_rank", "s_k", "runner_sign",
               "sign_agrees", "tau", "fit_days", "fit_mean", "fit_sharpe", "hold_days", "hold_mean", "hold_sharpe",
               "max_abs_rho", "max_abs_rho_with", "low_overlap_with", "cache_payload_sha256")


def admission_csv(candidates: list[dict]) -> bytes:
    def cell(v) -> str:
        if v is None:
            return ""
        if isinstance(v, bool):
            return "true" if v else "false"
        if isinstance(v, list):
            return ";".join(str(x) for x in v)
        if isinstance(v, float):
            return repr(v)
        return str(v)

    lines = [",".join(CSV_COLUMNS)]
    for row in candidates:
        cells = [cell(row[c]) for c in CSV_COLUMNS]
        require(all("," not in x and "\n" not in x for x in cells), "admission CSV: unsafe cell")
        lines.append(",".join(cells))
    return ("\n".join(lines) + "\n").encode("utf-8")


# ---------------------------------------------------------------------- output
def publish_directory(out: Path, files: dict[str, bytes]) -> None:
    """Exclusive, all-or-nothing publication: a reader sees no directory or every file complete."""
    out = Path(out)
    require(not out.exists(), f"output exists; refusing overwrite: {out}")
    out.parent.mkdir(parents=True, exist_ok=True)
    pending = out.with_name("." + out.name + ".pending")
    try:
        pending.mkdir()
    except FileExistsError as exc:
        raise FitError(f"stale or concurrent partial output; inspect and remove: {pending}") from exc
    try:
        for name in sorted(files):
            write_synced(pending / name, files[name], "xb")
        os.rename(pending, out)  # refuses an existing target on Windows
    except FileExistsError as exc:
        shutil.rmtree(pending, ignore_errors=True)
        raise FitError(f"output appeared concurrently; refusing overwrite: {out}") from exc
    except BaseException:
        shutil.rmtree(pending, ignore_errors=True)
        raise


class Incomplete(Exception):
    """A clean budget stop: completed candidates are persisted; rerun to continue."""

    def __init__(self, summary: dict):
        super().__init__("incomplete")
        self.summary = summary


def ensure_records(args, role: RoleManifest, library: list[dict], shas: list[str], cache_dir: Path,
                   started: float, log) -> tuple[list[dict], int, int]:
    """Every candidate's factor record under one context: (records, computed now, reused)."""
    store = WorkStore(args.work_dir, role) if args.work_dir else None
    records: list[dict | None] = [store.get(s) if store else None for s in shas]
    digests = {r["context_sha256"] for r in records if r is not None}
    if all(r is not None for r in records) and len(digests) == 1:
        return records, 0, len(records)  # type: ignore[return-value]
    context = store.load_context() if store else None
    if context is None:
        context = Context.build(role, log)
        if store:
            store.save_context(context)
    elif log:
        log(f"fit: context reused digest={context.digest[:12]} seconds={time.perf_counter() - started:.2f}")
    todo = [k for k, r in enumerate(records) if r is None or r["context_sha256"] != context.digest]
    reused = len(records) - len(todo)
    computed, slowest = 0, 0.0
    for k in todo:
        if args.max_new_candidates is not None and computed >= args.max_new_candidates:
            break
        if args.max_seconds is not None and time.perf_counter() - started + slowest > args.max_seconds:
            break
        tick = time.perf_counter()
        signal = load_candidate_signal(cache_dir, library[k]["id"], shas[k], role)
        records[k] = factor_record(context, signal, shas[k], library[k])
        del signal
        if store:
            store.put(records[k])  # type: ignore[arg-type]
        computed += 1
        slowest = max(slowest, time.perf_counter() - tick)
        if log:
            log(f"fit: {k + 1}/{len(library)} {library[k]['id']} tau={records[k]['tau']:.4f} "  # type: ignore[index]
                f"live={records[k]['live_decisions']} seconds={time.perf_counter() - tick:.2f}")  # type: ignore[index]
    remaining = [library[k]["id"] for k in todo if records[k] is None or
                 records[k]["context_sha256"] != context.digest]  # type: ignore[index]
    if remaining:
        raise Incomplete({"status": "incomplete", "computed_this_run": computed, "reused": reused,
                          "remaining": len(remaining), "next": remaining[0],
                          "seconds": round(time.perf_counter() - started, 2)})
    return records, computed, reused  # type: ignore[return-value]


def fit(args, log=None) -> tuple[int, dict]:
    started = time.perf_counter()
    require(args.screen in SCREENS, f"--screen must be one of {SCREENS}")
    require(args.work_dir is not None or (args.max_seconds is None and args.max_new_candidates is None),
            "--max-seconds/--max-new-candidates need --work-dir (nothing would persist)")
    require(args.max_seconds is None or (math.isfinite(args.max_seconds) and args.max_seconds > 0),
            "--max-seconds must be finite and > 0")
    require(args.max_new_candidates is None or args.max_new_candidates >= 0, "--max-new-candidates must be >= 0")
    library = load_library(args.library, args.library_sha256)
    runner_signs, orientation_recipe = load_orientations(args.orientations, args.orientations_sha256, library,
                                                         args.library_sha256, args.train_sha256)
    out = Path(args.output)
    require(not out.exists(), f"output exists; refusing overwrite: {out}")
    cache_dir = Path(args.candidate_cache) / args.train_sha256
    require(cache_dir.is_dir(), f"candidate cache: no directory for the TRAIN role: {cache_dir}")
    role = RoleManifest(args.train, args.train_sha256)
    shas = [candidate_payload_sha(cache_dir, c, role) for c in library]
    records, computed, reused = ensure_records(args, role, library, shas, cache_dir, started, log)

    ids = [c["id"] for c in library]
    factors = np.array([[np.nan if v is None else v for v in r["f_unsigned"]] for r in records], dtype=np.float64)
    taus = [r["tau"] for r in records]
    context_sha = records[0]["context_sha256"]
    decision_sessions = role.sessions[role.begin:role.end]
    fit_mask = (decision_sessions >= FIT_BEGIN_NS) & (decision_sessions < HOLD_BEGIN_NS)
    hold_mask = (decision_sessions >= HOLD_BEGIN_NS) & (decision_sessions < TRAIN_END_NS)
    script_sha = hashlib.sha256(Path(__file__).resolve().read_bytes()).hexdigest()
    inputs = {"library_sha256": args.library_sha256, "train_manifest_sha256": args.train_sha256,
              "role_source_sha256": role.source_sha256, "orientations_sha256": args.orientations_sha256,
              "orientations_recipe_sha256": orientation_recipe, "script_sha256": script_sha,
              "semantics_tag": SEMANTICS_TAG, "context_sha256": context_sha}
    window = role.window()
    files: dict[str, bytes] = {}
    admission_sha = None
    if args.screen == SCREEN_ID:
        rows = screen_v3(factors, taus, ids, fit_mask, hold_mask)
        signs = [r["s_k"] for r in rows]
        eligible = {k for k, r in enumerate(rows) if r["status"] == "admitted"}
        candidates = []
        for k, (cand, row) in enumerate(zip(library, rows)):
            candidates.append({"id": cand["id"], "family": cand["family"], "status": row["status"],
                               "failed_checks": row["failed_checks"], "redundant_with": row["redundant_with"],
                               "admission_rank": row["admission_rank"], "s_k": row["s_k"],
                               "runner_sign": runner_signs[k], "sign_agrees": runner_signs[k] == row["s_k"],
                               "tau": row["tau"], "tau_over_limit": row["tau"] > TAU_LIMIT,
                               "fit_days": row["fit_days"], "fit_mean": row["fit_mean"], "fit_sharpe": row["fit_sharpe"],
                               "hold_days": row["hold_days"], "hold_mean": row["hold_mean"],
                               "hold_sharpe": row["hold_sharpe"], "max_abs_rho": row["max_abs_rho"],
                               "max_abs_rho_with": row["max_abs_rho_with"], "low_overlap_with": row["low_overlap_with"],
                               "cache_payload_sha256": shas[k]})
        admitted_order = sorted(eligible, key=lambda k: rows[k]["admission_rank"])
        admission = {
            "schema": ADMISSION_SCHEMA, "screen": SCREEN_ID,
            "rules": {"orientation": "s_k=sign(mean unsigned f_k over live FIT decisions);runner sign reported not used",
                      "fit_window_ns": [FIT_BEGIN_NS, HOLD_BEGIN_NS], "hold_window_ns": [HOLD_BEGIN_NS, TRAIN_END_NS],
                      "window_basis": "decision session; statistics over live (non-flat) decisions",
                      "tau_limit": TAU_LIMIT, "rho_limit": RHO_LIMIT, "min_fit_days": MIN_FIT_DAYS,
                      "min_common_days": MIN_COMMON_DAYS, "sharpe_annualization": ANNUALIZATION,
                      "stability": "FIT annualized Sharpe(s_k f_k) > 0 and HOLD mean(s_k f_k) > 0",
                      "redundancy": "survivors by descending FIT Sharpe (ties library order); |pearson rho| over "
                                    "FIT days both live > rho_limit vs an admitted candidate -> reject_redundant "
                                    "(largest |rho|); < min_common_days or undefined rho -> uncorrelated, noted",
                      "status_precedence": list(ADMISSION_STATUSES[1:]),
                      "context": CONTEXT_SEMANTICS, "factor": FACTOR_SEMANTICS},
            "inputs": inputs, "window": window,
            "counts": {s: sum(1 for r in rows if r["status"] == s) for s in ADMISSION_STATUSES},
            "admitted": [ids[k] for k in admitted_order],
            "sign_conflicts": [c["id"] for c in candidates if not c["sign_agrees"]],
            "candidates": candidates}
        files[OUTPUT_ADMISSION] = canonical_bytes(admission)
        files[OUTPUT_ADMISSION_CSV] = admission_csv(candidates)
        admission_sha = hashlib.sha256(files[OUTPUT_ADMISSION]).hexdigest()
        status_of = {k: ("fitted" if r["status"] == "admitted" else r["status"]) for k, r in enumerate(rows)}
    else:
        signs = list(runner_signs)
        eligible = {k for k, s in enumerate(signs) if s != 0}
        status_of = {k: ("fitted" if signs[k] != 0 else "unoriented-sign-0") for k in range(len(library))}

    zero_filled = np.where(np.isnan(factors), 0.0, factors)
    weight_rows = []
    for k, cand in enumerate(library):
        row = {"id": cand["id"], "family": cand["family"], "sign": signs[k], "runner_sign": runner_signs[k],
               "status": status_of[k], "dsl_sha256": cand["dsl_sha256"], "cache_payload_sha256": shas[k],
               "tau": taus[k], "tau_over_limit": taus[k] > TAU_LIMIT,
               "flat_decisions": int(np.isnan(factors[k]).sum()), "weight": 0.0, "mv_solution": None,
               "clipped": False}
        if signs[k] == 0:
            row.update(factor_mean=None, factor_sd=None, factor_sharpe_annualized=None)
        else:
            row.update(factor_stats(signs[k] * zero_filled[k]))
            if k in eligible and not row["factor_sd"] > 0:
                row["status"] = "degenerate-zero-variance"
        weight_rows.append(row)
    active = [k for k in sorted(eligible) if weight_rows[k]["status"] == "fitted"]
    no_weights = None
    try:
        require(active, "fit: no eligible candidate with a non-degenerate factor series")
        matrix = np.vstack([signs[k] * zero_filled[k] for k in active])
        weights, raw = fit_weights(matrix)
    except FitError as exc:
        if args.screen != SCREEN_ID:
            raise
        no_weights = str(exc)
    summary = {"status": "complete", "output": str(out), "screen": args.screen, "candidates": len(library),
               "computed_this_run": computed, "reused": reused,
               "refused_decisions": len(records[0]["context_refused"])}
    if args.screen == SCREEN_ID:
        summary.update(admitted=len(eligible), counts=admission["counts"], sign_conflicts=admission["sign_conflicts"],
                       admission_sha256=admission_sha)
    if no_weights is not None:
        publish_directory(out, files)
        summary.update(status="published-without-weights", reason=no_weights,
                       files={n: hashlib.sha256(b).hexdigest() for n, b in sorted(files.items())},
                       seconds=round(time.perf_counter() - started, 2))
        return EXIT_NO_WEIGHTS, summary
    for k, w, r in zip(active, weights, raw):
        weight_rows[k].update(weight=float(w), mv_solution=float(r), clipped=not r > 0)
    weighted_tau = float(sum(row["weight"] * row["tau"] for row in weight_rows))
    conflicts = [row["id"] for row in weight_rows if row["weight"] > 0 and row["runner_sign"] != row["sign"]]
    document = {
        "schema": WEIGHTS_SCHEMA,
        "library_sha256": args.library_sha256,
        "train_manifest_sha256": args.train_sha256,
        "signs": {row["id"]: row["sign"] for row in weight_rows if row["sign"] != 0},
        "weights": {row["id"]: row["weight"] for row in weight_rows},
        "provenance": {
            "rule": RULE_ID, "lambda": SHRINK_LAMBDA,
            "shrinkage": "Sh=0.1*S+0.9*diag(S);S=sample-covariance-ddof1;w=solve(Sh,mu);w=max(w,0);w/=sum(w)",
            "screen": args.screen, "admission_sha256": admission_sha,
            "signs": ("v3-admit-v1-FIT-mean-sign;apply-pinned-signs" if args.screen == SCREEN_ID
                      else "TRAIN-orientations-artifact-sign;sign0-weight0"),
            "fit_series": "sign*f over ALL TRAIN scored decisions; flat decisions contribute 0",
            "factor": FACTOR_SEMANTICS, "neutralization": CONTEXT_SEMANTICS,
            "turnover": "tau=mean_d(sum_i|q(d)_i-q(d-1)_i|);consecutive-scored-TRAIN-decisions;"
                        "deployment-excluded;no-price-drift",
            "tau_limit": TAU_LIMIT, "tau_flagged": [row["id"] for row in weight_rows if row["tau_over_limit"]],
            "weighted_standalone_turnover": weighted_tau,
            "library_sha256": args.library_sha256, "role_manifest_sha256": args.train_sha256,
            "role_source_sha256": role.source_sha256, "orientations_sha256": args.orientations_sha256,
            "orientations_recipe_sha256": orientation_recipe, "script_sha256": script_sha,
            "semantics_tag": SEMANTICS_TAG, "context_sha256": context_sha, "window": window,
            "neutralization_refused_decisions": records[0]["context_refused"],
            "used_rows_unrefused": records[0]["context_used_rows_unrefused"],
            "fitted_candidates": len(active), "sign_conflicts_weighted": conflicts,
            "blend_in_sample_TRAIN_diagnostic": factor_stats(weights @ matrix),
            "candidates": weight_rows,
        },
    }
    files[OUTPUT_WEIGHTS] = canonical_bytes(document)
    require(len(files[OUTPUT_WEIGHTS]) <= METADATA_LIMIT, "output: weights JSON exceeds the runner's 1 MiB bound")
    publish_directory(out, files)
    summary.update(files={n: hashlib.sha256(b).hexdigest() for n, b in sorted(files.items())},
                   weights_sha256=hashlib.sha256(files[OUTPUT_WEIGHTS]).hexdigest(),
                   nonzero_weights=sum(1 for row in weight_rows if row["weight"] > 0),
                   weighted_standalone_turnover=weighted_tau, tau_flagged=document["provenance"]["tau_flagged"],
                   sign_conflicts_weighted=conflicts, seconds=round(time.perf_counter() - started, 2))
    return EXIT_OK, summary


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--library-sha256", required=True)
    p.add_argument("--train", type=Path, required=True, help="TRAIN role manifest.json")
    p.add_argument("--train-sha256", required=True)
    p.add_argument("--orientations", type=Path, required=True, help="TRAIN orientations.json from the IC runner")
    p.add_argument("--orientations-sha256", required=True)
    p.add_argument("--candidate-cache", type=Path, required=True, help="runner --candidate-cache DIR")
    p.add_argument("--screen", required=True, choices=SCREENS,
                   help="v3-admit-v1 (admission screen + fit on admitted) or none (T9: runner signs, all oriented)")
    p.add_argument("--output", type=Path, required=True, help="new output directory (never overwritten)")
    p.add_argument("--work-dir", type=Path, default=None,
                   help="persistent incremental state (context + per-candidate factor records)")
    p.add_argument("--max-seconds", type=float, default=None,
                   help="soft budget: stop cleanly before a candidate that would overrun it (exit 3)")
    p.add_argument("--max-new-candidates", type=int, default=None,
                   help="compute at most N missing candidates this run (exit 3 if more remain)")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        code, summary = fit(args, log=lambda line: print(line, file=sys.stderr, flush=True))
    except Incomplete as stop:
        print(json.dumps(stop.summary, sort_keys=True))
        return EXIT_INCOMPLETE
    except FitError as exc:
        print(f"fit_composition_weights: {exc}", file=sys.stderr)
        return EXIT_REFUSED
    print(json.dumps(summary, sort_keys=True))
    return code


if __name__ == "__main__":
    sys.exit(main())
