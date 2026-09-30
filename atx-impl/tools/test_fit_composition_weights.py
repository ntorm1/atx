"""Synthetic postimplementation checks for fit_composition_weights (no real data).

Run: python -m unittest discover -s atx-impl/tools -p test_fit_composition_weights.py -v
"""
import argparse
import contextlib
import hashlib
import io
import json
import math
from pathlib import Path
import re
import sys
import tempfile
import unittest
import unittest.mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fit_composition_weights as fcw  # noqa: E402
import record_store  # noqa: E402  (atx-engine/tools, on the path once fcw is imported)

DAY = fcw.DAY_NS
NAN = math.nan


# ------------------------------------------------------------ literal references
def ref_returns(close, raw, present):
    """Literal port of fill_returns/load_logs over the whole panel (interval t = (t-1, t])."""
    d, n = close.shape
    r = np.full((d, n), NAN)
    market = np.full(d, NAN)

    def logs(t, i):
        c, w = float(close[t, i]), float(raw[t, i])
        ok = present[t, i] != 0 and math.isfinite(c) and math.isfinite(w) and c > 0 and w > 0
        return (math.log(c), math.log(w)) if ok else (NAN, NAN)

    for t in range(1, d):
        total, count = 0.0, 0
        for i in range(n):
            pa, pr = logs(t - 1, i)
            ca, cr = logs(t, i)
            x = NAN
            if not math.isnan(pa) and not math.isnan(ca):
                x = float(close[t, i]) / float(close[t - 1, i]) - 1
                la, lr = ca - pa, cr - pr
                if not math.isfinite(x) or abs(la) > 1.5 or abs(la) > abs(lr) + 0.10:
                    x = NAN
            r[t, i] = x
            if not math.isnan(x):
                total += x
                count += 1
        market[t] = total / count if count else NAN
    return r, market


def ref_exposures(r, market, raw, volume, present, d):
    """Literal port of compute_price_exposures at decision d (all instruments)."""
    n = r.shape[1]
    first = d + 1 - 252 if d >= 252 else 1
    intervals = d + 1 - first if d >= first else 0
    beta_rows, vol_rows = min(252, intervals), min(63, intervals)
    out = np.full((n, 3), NAN)
    for i in range(n):
        rows = list(range(d + 1 - beta_rows, d + 1))
        pairs = [(r[t, i], market[t]) for t in rows if not math.isnan(r[t, i]) and not math.isnan(market[t])]
        beta = NAN
        if len(pairs) >= 126:
            mr = sum(p[0] for p in pairs) / len(pairs)
            mm = sum(p[1] for p in pairs) / len(pairs)
            cov = sum((p[0] - mr) * (p[1] - mm) for p in pairs)
            var = sum((p[1] - mm) ** 2 for p in pairs)
            beta = cov / var if var > 0 else NAN
        xs = [r[t, i] for t in range(d + 1 - vol_rows, d + 1) if not math.isnan(r[t, i])]
        vol = NAN
        if len(xs) >= 32:
            m = sum(xs) / len(xs)
            vol = math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))
        ladv = NAN
        if d + 1 >= 63:
            s = 0.0
            for t in range(d + 1 - 63, d + 1):
                w, v = float(raw[t, i]), float(volume[t, i])
                if present[t, i] and math.isfinite(w) and w > 0 and math.isfinite(v) and v >= 0 and math.isfinite(w * v):
                    s += w * v
            mean = s / 63
            ladv = math.log(mean) if mean > 0 else NAN
        out[i] = (beta, vol, ladv)
    return out, np.isfinite(out).all(axis=1)


def ref_centered_ranks(values):
    """Literal port of sort_ranks + each_centered_rank; values is [(value, index)]."""
    v = sorted(values)
    out = {}
    if len(v) < 2:
        return out
    b = 0
    while b < len(v):
        e = b + 1
        while e < len(v) and v[e][0] == v[b][0]:
            e += 1
        rank = (float(b) + float(e - 1)) / (2.0 * float(len(v) - 1)) - 0.5
        for k in range(b, e):
            out[v[k][1]] = rank
        b = e
    return out


def reference_fit(p, signals, signs):
    """Slow independent rule: per-date loops, lstsq residuals, explicit moments."""
    r, market = ref_returns(p["close"], p["raw"], p["present"])
    eff = (p["member"] == 1) & (p["present"] == 1)
    d_count, n = p["close"].shape
    begin, end = p["score_begin"], d_count - 2
    books = {k: [] for k in range(len(signals))}
    factors = {k: [] for k in range(len(signals))}
    for d in range(begin, end):
        e, ok = ref_exposures(r, market, p["raw"], p["volume"], p["present"], d)
        used = [i for i in range(n) if eff[d, i] and ok[i]]
        fwd = np.array([0.0 if math.isnan(r[d + 2, i]) else r[d + 2, i] for i in range(n)])
        x = None
        if len(used) >= 50:
            z = np.empty((len(used), 3))
            for k in range(3):
                col = e[used, k]
                mean = col.sum() / len(col)
                sd = math.sqrt(((col - mean) ** 2).sum() / (len(col) - 1))
                z[:, k] = np.clip((col - mean) / sd, -5.0, 5.0)
            x = np.column_stack([np.ones(len(used)), z])
        for k, (sig, sign) in enumerate(zip(signals, signs)):
            q = np.zeros(n)
            if x is not None:
                ranks = ref_centered_ranks([(float(sig[d, i]), i) for i in used if math.isfinite(sig[d, i])])
                y = np.array([(sign if sign != 0 else 1) * ranks.get(i, 0.0) for i in used])
                entry = np.abs(y).sum()
                coef = np.linalg.lstsq(x, y, rcond=None)[0]
                res = y - x @ coef
                gross = np.abs(res).sum()
                if entry > 0 and gross > 1e-9 * entry:
                    q[used] = res / gross
            books[k].append(q)
            factors[k].append(float(q @ fwd))
    taus = []
    for k in range(len(signals)):
        qs = books[k]
        taus.append(sum(np.abs(qs[t] - qs[t - 1]).sum() for t in range(1, len(qs))) / (len(qs) - 1))
    active = [k for k in range(len(signals)) if signs[k] != 0 and np.std(factors[k]) > 0]
    f = np.array([factors[k] for k in active])
    t = f.shape[1]
    mu = f.sum(axis=1) / t
    dev = f - mu[:, None]
    cov = dev @ dev.T / (t - 1)
    shrunk = 0.1 * cov + 0.9 * np.diag(np.diag(cov))
    w = np.linalg.solve(shrunk, mu)
    w = np.where(w > 0, w, 0.0)
    weights = np.zeros(len(signals))
    weights[active] = w / w.sum()
    return weights, np.array(taus), {k: np.array(factors[k]) for k in range(len(signals))}


# --------------------------------------------------------------- synthetic data
def synthetic_panel(seed=11, dates=232, names=64, score_begin=150, start="2021-03-01"):
    rng = np.random.default_rng(seed)
    mkt = rng.normal(0.0003, 0.01, dates)
    beta = rng.uniform(0.4, 1.6, names)
    ret = beta[None, :] * mkt[:, None] + rng.normal(0, 0.02, (dates, names)) * rng.uniform(0.5, 2, names)
    close = 40.0 * np.exp(np.cumsum(np.log1p(ret), axis=0))
    scale = np.ones((dates, names))
    scale[100:, 3] = 2.0  # a genuine 2-for-1 split: raw moves, adjusted does not -> kept
    raw = close * scale
    close[120, 5] *= 1.6  # uncorroborated adjusted spike -> guard trips twice
    close[130, 6] *= 5.0  # |log adj| > 1.5 even though raw agrees
    raw[130, 6] *= 5.0
    volume = rng.lognormal(12, 1, (dates, names))
    volume[:, 12] = 0.0  # never any dollar volume -> log_adv NaN -> never used
    present = np.ones((dates, names), dtype=np.uint8)
    present[60:71, 7] = 0
    present[20:62, 14] = 0  # too few beta pairs until later decisions
    present[score_begin + 5, 9] = 0  # member but absent at a decision
    present[dates - 20, 8] = 0
    member = present.copy()
    member[:, 10] = 0
    member[:170, 11] = 0
    member[score_begin + 10, 40:] = 0  # 40 members only -> neutralization refused that decision
    member[:63] = 0
    for x in (close, raw, volume):
        x[present == 0] = NAN
    day0 = np.datetime64(start, "D").astype("datetime64[ns]").astype(np.int64)
    sessions = day0 + DAY * np.arange(dates, dtype=np.int64)
    return {"close": close, "raw": raw, "volume": volume, "present": present, "member": member,
            "sessions": sessions, "ids": np.arange(101, 101 + names, dtype=np.uint64),
            "score_begin": score_begin, "rng": rng}


def synthetic_signals(p):
    rng = p["rng"]
    d, n = p["close"].shape
    r, _ = ref_returns(p["close"], p["raw"], p["present"])
    ahead = np.zeros((d, n))
    ahead[:-2] = np.nan_to_num(r[2:], nan=0.0)  # the synthetic alpha peeks at r(d+2)
    noise = lambda s: rng.normal(0, s, (d, n))  # noqa: E731
    ties = np.round(noise(1.0))
    ties[rng.random((d, n)) < 0.1] = NAN
    sparse = np.full((d, n), NAN)
    sparse[:, 0] = 1.0  # one finite name per date -> no ranks -> degenerate
    signals = [0.5 * ahead + noise(0.02), -0.3 * ahead + noise(0.02), ties + 0.05 * ahead,
               noise(1.0), -0.4 * ahead + noise(0.02), sparse]
    member = p["member"] == 1
    return [np.where(member, s, NAN) for s in signals]


IDS = ["alpha_a", "alpha_b", "tie_heavy", "unoriented", "wrong_way", "sparse_one"]
FAMILIES = ["fam_a", "fam_a", "fam_b", "fam_b", "fam_c", "fam_c"]
SIGNS = [1, -1, 1, 0, 1, 1]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def work_shape(keys: list[str], new: bool = False) -> list[str]:
    """A work dir's file list with record and directory names abstracted to (record kind | context file). Pre-W3
    fitters name a record by its cache payload SHA (``<sha>/<tag>/factors/<sha>[.f-<fields sha>].json``), the W3
    WorkStore by its work key (``<sha>/<tag>/factors/k-<key>.json``, ``aim-<tag>/``), the v8 store by its record key
    (``<role16>-<window>/factor/<key>.json``, ``aim/``, ``context/<fingerprint>/``); ``new`` asserts the v8 names.
    Equal shapes = the same number of records of each kind and the same context files."""
    out = []
    for k in keys:
        parts = Path(k).parts
        if "context" in parts:
            out.append("context/" + parts[-1])
            continue
        kind = parts[-2]
        if new:
            assert kind in ("factor", "aim") and re.fullmatch(r"[0-9a-f]{64}\.json", parts[-1]), k
        out.append(("factor" if kind in ("factors", "factor") else "aim" if kind.startswith("aim") else kind)
                   + "/<record>")
    return sorted(out)


def store_base(work: Path, train_sha: str) -> Path:
    """The v8 store root of a role under a --work-dir (WorkStore.base)."""
    return Path(work) / f"{train_sha[:16]}-{fcw.window_id()}"


def rewrite_body(path: Path, edit) -> None:
    """Edit a stored record's body and re-seal it (a valid record of other content, as another run could write)."""
    j = json.loads(path.read_bytes())
    edit(j["body"])
    j["content_sha256"] = record_store.content_sha256(j["kind"], j["key"], j["body"])
    path.write_bytes(record_store.canonical(j))


_REGISTRY_PATH = fcw.REGISTRY_PATH


def setUpModule():  # hermetic: the in-file theme list unless a test installs a registry
    fcw.REGISTRY_PATH = Path(__file__).resolve().parent / "no-such-alpha-registry.json"


def tearDownModule():
    fcw.REGISTRY_PATH = _REGISTRY_PATH


V2_SCHEMA = "atx.dsl-candidate-signal/v2"
# Two extra fields, listed out of name order: the key and fp_<fk16> order them by name.
FIELD_PAYLOADS = {"sv_ratio126": "34" * 32, "book_to_price": "56" * 32}


def v2_field_lines(fields: dict) -> str:
    return "".join(f"field={k}:{fields[k]}\n" for k in sorted(fields))


def v2_signal_key(vm_identity: str, role_sha: str, dates: int, instruments: int, dsl_sha: str, fields: dict) -> str:
    """strategy_ic_runner.cpp signal_key_text() written out literally (independent of fcw)."""
    text = ("atx.dsl-candidate-signal-key/v2\n" f"vm_identity={vm_identity}\n"
            "eval_mode=ResearchFast;full-historical-asof-member-mask\n"
            "layout=date-major-little-endian-f64;non-finite-stored-as-quiet-NaN\n"
            f"role_manifest_sha256={role_sha}\ndates={dates}\ninstruments={instruments}\n"
            f"dsl_sha256={dsl_sha}\n" + v2_field_lines(fields))
    return sha(text.encode())


class Fixture:
    """Role dir + library + orientations + the runner's landed candidate cache layout + TRAIN summary.json.

    Cache ROOT = cache/ for the legacy VM identity, else cache/<identity>/ (strategy_ic_runner.cpp
    cache_root). ``layout``:
      v1         v1 runner: ROOT/<train sha>/<id>.{f64,json}, ``field_ids`` in ROOT/<fields sha>/; no entries.
      v2         v2 runner: ROOT/<train sha>/[fp_<fk16>/]<id>.<dsl16>.{f64,json} + candidate_cache.entries[].
      v1-listed  v2 runner reading the v1 entries in place: v1 files + candidate_cache.entries[].
    """

    def __init__(self, root: Path, panel=None, signals=None, signs=SIGNS, sidecar_role="train",
                 end_ns=None, ids=IDS, families=FAMILIES, vm_identity=fcw.LEGACY_VM_IDENTITY, field_ids=(),
                 fields_sha="ef" * 32, keyless_legacy=False, sidecar_patch=None, candidate_extra=None,
                 layout="v1", field_payloads=FIELD_PAYLOADS):
        assert layout in ("v1", "v2", "v1-listed")
        self.layout, self.field_payloads = layout, dict(field_payloads)
        self.root, self.ids = root, list(ids)
        self.p = panel if panel is not None else synthetic_panel()
        self.signals = signals if signals is not None else synthetic_signals(self.p)
        p = self.p
        d, n = p["close"].shape
        role = root / "role"
        role.mkdir(parents=True)
        files = {}
        for name, arr in (("sessions.i64", p["sessions"].astype("<i8")), ("ids.u64", p["ids"].astype("<u8")),
                          ("present.u8", p["present"]), ("member.u8", p["member"]),
                          ("close.f64", p["close"].astype("<f8")), ("raw_close.f64", p["raw"].astype("<f8")),
                          ("volume.f64", p["volume"].astype("<f8"))):
            data = np.ascontiguousarray(arr).tobytes()
            (role / name).write_bytes(data)
            files[name] = {"bytes": len(data), "sha256": sha(data)}
        manifest = {"schema": fcw.ROLE_SCHEMA, "status": "complete", "dates": d, "instruments": n,
                    "score_begin": p["score_begin"], "score_end": d,
                    "score_start_ns": int(p["sessions"][p["score_begin"]]),
                    "score_end_ns": int(end_ns if end_ns is not None else p["sessions"][-1] + DAY),
                    "source_sha256": "ab" * 32, "files": files}
        self.manifest = role / "manifest.json"
        self.manifest.write_bytes(json.dumps(manifest, indent=2).encode())
        self.train_sha = sha(self.manifest.read_bytes())
        library = {"schema": fcw.LIBRARY_SCHEMA, "id": "synthetic",
                   "candidates": [{"id": i, "family": f, "dsl": f"rank(close) * {k}", "horizons": [5, 21, 63],
                                   "sign_policy": "train-rank-ic21", **(candidate_extra or {}).get(i, {})}
                                  for k, (i, f) in enumerate(zip(ids, families))]}
        self.library = root / "library.json"
        self.library.write_bytes(json.dumps(library).encode())
        self.library_sha = sha(self.library.read_bytes())
        self.dsl_sha = [sha(c["dsl"].encode()) for c in library["candidates"]]
        recipe_sha = "cd" * 32
        orientations = {"schema": fcw.ORIENTATIONS_SCHEMA, "recipe_sha256": recipe_sha,
                        "library_sha256": self.library_sha, "train_manifest_sha256": self.train_sha,
                        "candidates": [{"id": i, "family": f, "dsl_sha256": s, "sign": g}
                                       for i, f, s, g in zip(ids, families, self.dsl_sha, signs)]}
        (root / "train").mkdir()
        self.orientations = root / "train" / "orientations.json"
        self.orientations.write_bytes((json.dumps(orientations, indent=2) + "\n").encode())
        self.orientations_sha = sha(self.orientations.read_bytes())
        self.cache = root / "cache"
        self.cache_root = self.cache if vm_identity == fcw.LEGACY_VM_IDENTITY else self.cache / vm_identity
        self.vm_identity, self.fields_sha, self.field_ids = vm_identity, fields_sha, set(field_ids)
        entries = []
        for cid, s, dsha in zip(ids, self.signals, self.dsl_sha):
            entry = self.entry_dir(cid)
            entry.mkdir(parents=True, exist_ok=True)
            data = np.ascontiguousarray(s.astype("<f8")).tobytes()
            self.payload_path(cid).write_bytes(data)
            sidecar = {"schema": fcw.CACHE_SCHEMA, "candidate_id": cid, "dsl_sha256": dsha,
                       "role_manifest_sha256": self.train_sha, "role": sidecar_role,
                       "eval_mode": fcw.VM_EVAL_MODE, "layout": fcw.CACHE_LAYOUT, "dates": d, "instruments": n,
                       "bytes": len(data), "payload": self.payload_path(cid).name, "payload_sha256": sha(data),
                       "engine_git_sha": fcw.LEGACY_ENGINE_SHAS[0], "vm_identity": vm_identity}
            fields = self.field_payloads if cid in self.field_ids else {}
            key = v2_signal_key(vm_identity, self.train_sha, d, n, dsha, fields)
            if layout == "v2":
                sidecar.update(schema=V2_SCHEMA, field_payload_sha256=fields, signal_key_sha256=key)
            if keyless_legacy:
                del sidecar["vm_identity"]
            if cid in self.field_ids:
                sidecar["fields_manifest_sha256"] = fields_sha
            sidecar.update((sidecar_patch or {}).get(cid, {}))
            self.sidecar_path(cid).write_bytes(json.dumps(sidecar, indent=2).encode())
            entries.append({"id": cid, "layout": "v2" if layout == "v2" else "v1",
                            "sidecar": str(self.sidecar_path(cid)), "payload": str(self.payload_path(cid)),
                            "payload_sha256": sha(data), "field_payload_sha256": fields, "signal_key_sha256": key})
        cache_summary = {"directory": str(self.cache_root / self.train_sha), "vm_identity": vm_identity,
                         "hits": 0, "misses": len(ids)}
        if layout != "v1":
            cache_summary.update(layout=V2_SCHEMA, legacy_hits=0, entries=entries)
        train_role = {"role": "train", "manifest_sha256": self.train_sha, "candidate_cache": cache_summary}
        if self.field_ids:
            cache_summary["fields_directory"] = str(self.cache_root / fields_sha)
            train_role["research_fields"] = {"manifest_sha256": fields_sha}
        self.summary_doc = {"status": "complete", "recipe_sha256": recipe_sha, "roles": [train_role],
                            "orientations_artifact_sha256": self.orientations_sha}
        self.write_summary()
        self.signs = list(signs)

    def entry_dir(self, cid: str) -> Path:
        if self.layout != "v2":
            return self.cache_root / (self.fields_sha if cid in self.field_ids else self.train_sha)
        base = self.cache_root / self.train_sha
        return base / f"fp_{sha(v2_field_lines(self.field_payloads).encode())[:16]}" if cid in self.field_ids else base

    def stem(self, cid: str) -> str:
        return f"{cid}.{self.dsl_sha[self.ids.index(cid)][:16]}" if self.layout == "v2" else cid

    def payload_path(self, cid: str) -> Path:
        return self.entry_dir(cid) / f"{self.stem(cid)}.f64"

    def sidecar_path(self, cid: str) -> Path:
        return self.entry_dir(cid) / f"{self.stem(cid)}.json"

    def cache_summary(self, doc=None) -> dict:
        return (doc if doc is not None else self.summary_doc)["roles"][0]["candidate_cache"]

    def write_summary(self, doc=None) -> None:
        self.summary = self.root / "train" / "summary.json"
        self.summary.write_bytes(json.dumps(doc if doc is not None else self.summary_doc, indent=2).encode())
        self.summary_sha = sha(self.summary.read_bytes())

    def argv(self, output: Path, screen="none", extra=()) -> list[str]:
        return ["--library", str(self.library), "--library-sha256", self.library_sha,
                "--train", str(self.manifest), "--train-sha256", self.train_sha,
                "--orientations", str(self.orientations), "--orientations-sha256", self.orientations_sha,
                "--runner-summary", str(self.summary), "--runner-summary-sha256", self.summary_sha,
                "--screen", screen, "--output", str(output), *extra]

    def args(self, output: Path, screen="none", **override) -> argparse.Namespace:
        args = fcw.parse_args(self.argv(output, screen))
        for k, v in override.items():
            setattr(args, k, v)
        return args


def runner_accepts(text: bytes, library_sha: str, ids: list[str], train_sha: str) -> list[float]:
    """Python port of the landed strategy_ic_runner.cpp composition_weights() + composition_signs()."""
    assert 0 < len(text) <= 1 << 20

    def pairs(items):
        keys = [k for k, _ in items]
        assert len(keys) == len(set(keys)), "duplicate key"
        return dict(items)

    j = json.loads(text, object_pairs_hook=pairs)
    assert isinstance(j, dict) and j.get("schema") in ("atx.dsl-composition-weights/v1",
                                                       "atx.dsl-composition-weights/v2")
    assert j.get("library_sha256") == library_sha and isinstance(j.get("weights"), dict)
    assert set(j["weights"]) <= set(ids), "weight for unknown candidate"
    out = []
    for cid in ids:
        w = j["weights"][cid]
        assert isinstance(w, (int, float)) and not isinstance(w, bool) and math.isfinite(w) and w >= 0
        out.append(float(w))
    assert j.get("train_manifest_sha256") == train_sha, "TRAIN binding"
    if "signs" in j:
        assert isinstance(j["signs"], dict) and set(j["signs"]) <= set(ids), "sign for unknown candidate"
        assert all(type(v) is int and v in (1, -1) for v in j["signs"].values()), "sign must be +1/-1"
        assert all(cid in j["signs"] for cid, w in zip(ids, out) if w > 0), "weighted candidate without sign"
    # V6-W fix round 1 (I1): v2 iff a theme_redistribution block is present, v1 iff absent.
    assert (j["schema"] == "atx.dsl-composition-weights/v2") == ("theme_redistribution" in j), "schema/block mismatch"
    return out


# ------------------------------------------------------------------------ tests
class HandComputed(unittest.TestCase):
    def test_centered_tied_ranks_hand_case(self):
        values = np.array([[3.0, 1.0, 3.0, 2.0, NAN], [5.0, NAN, NAN, NAN, NAN], [2.0, 2.0, 2.0, NAN, 2.0]])
        got = fcw.centered_tied_ranks(values, np.isfinite(values))
        # C++ each_centered_rank arithmetic: (b + (e - 1)) / (2 (n - 1)) - 0.5 with n = 4
        np.testing.assert_array_equal(got[0], [5 / 6 - 0.5, 0 / 6 - 0.5, 5 / 6 - 0.5, 2 / 6 - 0.5, 0.0])
        np.testing.assert_allclose(got[0], [1 / 3, -1 / 2, 1 / 3, -1 / 6, 0.0], rtol=0, atol=1e-16)
        np.testing.assert_array_equal(got[1], np.zeros(5))  # one valid name: no ranks
        np.testing.assert_array_equal(got[2], np.zeros(5))  # one tie group: all centered at 0

    def test_ranks_match_literal_composition_port_and_are_antisymmetric(self):
        rng = np.random.default_rng(3)
        values = np.round(rng.normal(0, 2, (40, 30)))
        values[rng.random(values.shape) < 0.2] = NAN
        valid = np.isfinite(values) & (rng.random(values.shape) < 0.9)
        got = fcw.centered_tied_ranks(values, valid)
        for t in range(values.shape[0]):
            ref = ref_centered_ranks([(values[t, i], i) for i in range(values.shape[1]) if valid[t, i]])
            np.testing.assert_array_equal(got[t], [ref.get(i, 0.0) for i in range(values.shape[1])])
        np.testing.assert_allclose(fcw.centered_tied_ranks(-values, valid), -got, rtol=0, atol=1e-15)

    def test_mv_shrink_hand_case_with_nonneg_clip(self):
        # S = [[1,.5,0],[.5,1,0],[0,0,4]] -> Sh = [[1,.05,0],[.05,1,0],[0,0,4]]; mu = [.1,-.01,.2]
        # w3 = .2/4 = .05; [w1,w2] = [.1005, -.015]/.9975 -> w2 clipped; normalize over w1 + w3.
        raw = fcw.shrink_solution(np.array([0.1, -0.01, 0.2]),
                                  np.array([[1.0, 0.5, 0.0], [0.5, 1.0, 0.0], [0.0, 0.0, 4.0]]))
        np.testing.assert_allclose(raw, [0.1005 / 0.9975, -0.015 / 0.9975, 0.05], rtol=0, atol=1e-15)
        # Series with exactly that covariance (x 4/3) and mean: orthogonal zero-mean patterns.
        e1, e2, e3 = np.array([1.0, -1, 1, -1]), np.array([1.0, 1, -1, -1]), np.array([1.0, -1, -1, 1])
        factors = np.vstack([0.1 + e1, -0.01 + 0.5 * e1 + math.sqrt(0.75) * e2, 0.2 + 2 * e3])
        weights, solution = fcw.fit_weights(factors)
        np.testing.assert_allclose(weights, [0.1005 / 0.150375, 0.0, 0.049875 / 0.150375], rtol=0, atol=1e-12)
        self.assertEqual(weights[1], 0.0)
        self.assertLess(solution[1], 0)
        self.assertAlmostEqual(float(weights.sum()), 1.0, places=15)

    def test_all_nonpositive_solution_refuses(self):
        with self.assertRaises(fcw.FitError):
            fcw.fit_weights(np.array([[-1.0, -2.0, 0.5, -0.3], [0.2, -0.4, -0.1, -0.5]]))

    def test_turnover_hand_case(self):
        q = np.array([[0.5, -0.5, 0, 0], [0.25, -0.25, 0.25, -0.25], [0.5, -0.5, 0, 0], [0.5, -0.5, 0, 0]])
        # |dq| sums: 1.0, 1.0, 0.0 over three transitions (deployment excluded) -> 2/3
        self.assertAlmostEqual(fcw.standalone_turnover(q), 2 / 3, places=15)
        self.assertEqual(fcw.standalone_turnover(np.tile(q[:1], (5, 1))), 0.0)


class Exposures(unittest.TestCase):
    def test_vectorized_exposures_match_literal_cpp_port(self):
        p = synthetic_panel()
        panel = fcw.PricePanel(p["close"], p["raw"], p["volume"], p["present"])
        r, market = ref_returns(p["close"], p["raw"], p["present"])
        np.testing.assert_array_equal(np.isnan(panel.returns), np.isnan(r))
        np.testing.assert_allclose(panel.returns, r, rtol=0, atol=1e-15, equal_nan=True)
        np.testing.assert_allclose(panel.market, market, rtol=1e-12, atol=1e-17, equal_nan=True)
        self.assertTrue(np.isnan(r[120, 5]) and np.isnan(r[121, 5]))  # uncorroborated spike
        self.assertTrue(np.isnan(r[130, 6]) and np.isnan(r[131, 6]))  # |log adj| > 1.5
        self.assertFalse(np.isnan(r[100, 3]))  # genuine split kept
        cols = np.arange(p["close"].shape[1])
        for d in (0, 1, 5, 40, 62, 63, 100, 127, 150, 165, 229, 231):
            got, ok = panel.exposures(d, cols)
            want, want_ok = ref_exposures(r, market, p["raw"], p["volume"], p["present"], d)
            np.testing.assert_array_equal(ok, want_ok, err_msg=f"d={d}")
            np.testing.assert_allclose(got, want, rtol=1e-11, atol=1e-14, equal_nan=True, err_msg=f"d={d}")
        _, ok = panel.exposures(150, cols)
        self.assertFalse(ok[12] or ok[14])  # zero dollar volume; too few beta pairs
        self.assertTrue(panel.exposures(229, cols)[1][14])  # enough pairs later

    def test_book_is_neutral_gross_one_and_supported_on_used_rows(self):
        p = synthetic_panel()
        signals = synthetic_signals(p)
        with tempfile.TemporaryDirectory() as tmp:
            role = Fixture(Path(tmp), p, signals)
            manifest = fcw.RoleManifest(role.manifest, role.train_sha)
            loaded = manifest.payload()
            ctx = fcw.Context.build(manifest)
        panel = fcw.PricePanel(loaded["close"], loaded["raw_close"], loaded["volume"], loaded["present"])
        self.assertEqual([x["decision_index"] for x in ctx.refused], [p["score_begin"] + 10])
        self.assertEqual(ctx.refused[0]["reason"], "too-few-usable-names")
        q, live = ctx.book(signals[0], 1)
        qn, _ = ctx.book(signals[0], -1)
        np.testing.assert_array_equal(qn, -q)
        self.assertEqual(int((~live).sum()), 1)
        member = (p["member"] == 1) & (p["present"] == 1)
        for t in range(q.shape[0]):
            d = ctx.begin + t
            if not ctx.used[t].any():
                self.assertFalse(q[t].any())
                continue
            self.assertAlmostEqual(float(np.abs(q[t]).sum()), 1.0, places=12)
            self.assertFalse(q[t][~ctx.used[t]].any())
            cols = ctx.columns[ctx.used[t]]
            self.assertTrue(member[d, cols].all())
            e, ok = panel.exposures(d, cols)
            z = (e - e.mean(0)) / e.std(0, ddof=1)
            x = np.column_stack([np.ones(len(cols)), np.clip(z, -5, 5)])
            np.testing.assert_allclose(x.T @ q[t][ctx.used[t]], 0.0, atol=1e-13)


class EndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.fx = Fixture(cls.root / "fx")
        cls.out = cls.root / "out" / "t9"
        cls.code, cls.summary = fcw.fit(cls.fx.args(cls.out))
        cls.text = (cls.out / fcw.OUTPUT_WEIGHTS).read_bytes()
        cls.doc = json.loads(cls.text)
        cls.ref_weights, cls.ref_taus, cls.ref_factors = reference_fit(cls.fx.p, cls.fx.signals, SIGNS)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def rows(self):
        return {row["id"]: row for row in self.doc["provenance"]["candidates"]}

    def test_weights_match_independent_reference(self):
        got = np.array([self.doc["weights"][i] for i in IDS])
        np.testing.assert_allclose(got, self.ref_weights, rtol=1e-9, atol=1e-12)
        self.assertAlmostEqual(float(got.sum()), 1.0, places=12)
        self.assertGreater(got[0], 0)
        self.assertGreater(got[1], 0)  # sign -1 candidate earns weight once oriented

    def test_sign_clip_and_degenerate_handling(self):
        rows = self.rows()
        self.assertEqual(rows["unoriented"]["weight"], 0.0)
        self.assertEqual(rows["unoriented"]["status"], "unoriented-sign-0")
        self.assertIsNone(rows["unoriented"]["factor_mean"])
        self.assertIsNone(rows["unoriented"]["mv_solution"])
        self.assertEqual(rows["wrong_way"]["weight"], 0.0)  # negative MV solution clipped
        self.assertTrue(rows["wrong_way"]["clipped"])
        self.assertLess(rows["wrong_way"]["mv_solution"], 0)
        self.assertLess(rows["wrong_way"]["factor_mean"], 0)
        self.assertEqual(rows["sparse_one"]["status"], "degenerate-zero-variance")
        self.assertEqual(rows["sparse_one"]["weight"], 0.0)
        self.assertEqual(rows["sparse_one"]["tau"], 0.0)
        oriented = rows["alpha_b"]
        self.assertEqual(oriented["sign"], -1)
        self.assertGreater(oriented["factor_mean"], 0)
        np.testing.assert_allclose(oriented["factor_mean"], self.ref_factors[1].mean(), rtol=1e-9)
        sharpe = self.ref_factors[0].mean() / self.ref_factors[0].std(ddof=1) * math.sqrt(252)
        np.testing.assert_allclose(rows["alpha_a"]["factor_sharpe_annualized"], sharpe, rtol=1e-9)

    def test_turnover_matches_reference_and_blend(self):
        rows = self.rows()
        taus = np.array([rows[i]["tau"] for i in IDS])
        np.testing.assert_allclose(taus, self.ref_taus, rtol=1e-9, atol=1e-12)
        self.assertGreater(rows["unoriented"]["tau"], 0)  # sign 0 still reports tau
        prov = self.doc["provenance"]
        weighted = sum(self.doc["weights"][i] * rows[i]["tau"] for i in IDS)
        self.assertAlmostEqual(prov["weighted_standalone_turnover"], weighted, places=14)
        self.assertEqual(prov["tau_flagged"], [i for i in IDS if rows[i]["tau"] > 0.70])
        self.assertTrue(all(rows[i]["tau_over_limit"] == (rows[i]["tau"] > 0.70) for i in IDS))
        # the noise candidates redraw every day: far above the 0.70 flag
        self.assertIn("unoriented", prov["tau_flagged"])

    def test_schema_provenance_and_runner_acceptance(self):
        got = runner_accepts(self.text, self.fx.library_sha, IDS, self.fx.train_sha)
        self.assertEqual(len(got), len(IDS))
        self.assertEqual(sorted(self.doc["weights"]), sorted(IDS))
        prov = self.doc["provenance"]
        self.assertEqual(prov["rule"], "mv-shrink-0.9-nonneg-v1")
        self.assertEqual(prov["lambda"], 0.9)
        self.assertEqual(prov["role_manifest_sha256"], self.fx.train_sha)
        self.assertEqual(prov["orientations_sha256"], self.fx.orientations_sha)
        self.assertEqual(prov["script_sha256"], sha(Path(fcw.__file__).read_bytes()))
        begin = self.fx.p["score_begin"]
        self.assertEqual(prov["window"]["decision_begin"], begin)
        self.assertEqual(prov["window"]["decision_end_exclusive"], self.fx.p["close"].shape[0] - 2)
        self.assertEqual(len(prov["neutralization_refused_decisions"]), 1)
        rows = self.rows()
        for i, s in zip(IDS, self.fx.signals):
            self.assertEqual(rows[i]["cache_payload_sha256"], sha(np.ascontiguousarray(s.astype("<f8")).tobytes()))
        self.assertEqual(self.code, fcw.EXIT_OK)
        self.assertEqual(self.summary["weights_sha256"], sha(self.text))
        self.assertEqual(self.text, fcw.canonical_bytes(self.doc))  # canonical bytes
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), [fcw.OUTPUT_WEIGHTS])  # no screen files
        self.assertEqual(self.doc["train_manifest_sha256"], self.fx.train_sha)
        self.assertEqual(self.doc["provenance"]["role_manifest_sha256"], self.fx.train_sha)
        # signs: runner signs (T9 rule), every nonzero sign listed, so every positive weight has one
        self.assertEqual(self.doc["signs"], {i: s for i, s in zip(IDS, SIGNS) if s != 0})
        self.assertTrue(all(i in self.doc["signs"] for i in IDS if self.doc["weights"][i] > 0))
        self.assertEqual(self.doc["provenance"]["screen"], "none")

    def test_deterministic_bytes_and_exclusive_output(self):
        again = self.root / "out" / "again"
        fcw.fit(self.fx.args(again))
        self.assertEqual((again / fcw.OUTPUT_WEIGHTS).read_bytes(), self.text)
        with self.assertRaises(fcw.FitError):
            fcw.fit(self.fx.args(self.out))
        self.assertEqual((self.out / fcw.OUTPUT_WEIGHTS).read_bytes(), self.text)
        with self.assertRaises(fcw.FitError):
            fcw.publish_directory(self.out, {"x.json": b"{}"})
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), [fcw.OUTPUT_WEIGHTS])
        self.assertFalse(any(x.name.endswith(".pending") for x in self.out.parent.iterdir()))


class Refusals(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.p = synthetic_panel(dates=200, score_begin=150)
        self.signals = synthetic_signals(self.p)

    def tearDown(self):
        self.tmp.cleanup()

    def refuse(self, fx, fragment, **override):
        out = self.root / "refused"
        with self.assertRaises(fcw.FitError) as caught:
            fcw.fit(fx.args(out, **override))
        self.assertIn(fragment, str(caught.exception))
        self.assertFalse(out.exists())

    def test_post_train_role_refused(self):
        # runs past the TRAIN end of the research window (research_window.py) into the sealed sample
        start = str(np.datetime64(fcw.rw.TRAIN_END_DATE) - np.timedelta64(150, "D"))
        p = synthetic_panel(dates=200, score_begin=150, start=start)
        fx = Fixture(self.root / "late", p, synthetic_signals(p))
        self.refuse(fx, "TRAIN-only")
        with self.assertRaises(fcw.rw.SealError) as caught:   # also a ValueError naming the window and the seal
            fcw.fit(fx.args(self.root / "refused-seal"))
        self.assertIn(fcw.rw.WINDOW_ID, str(caught.exception))
        self.assertIn(fcw.rw.SEAL_DATE, str(caught.exception))

    def test_non_train_cache_entry_refused(self):
        fx = Fixture(self.root / "val", self.p, self.signals, sidecar_role="validation")
        self.refuse(fx, "not a TRAIN-role signal")

    def test_pins_and_tampering_refused(self):
        fx = Fixture(self.root / "pins", self.p, self.signals)
        self.refuse(fx, "pin differs", library_sha256="0" * 64)
        self.refuse(fx, "pin differs", orientations_sha256="1" * 64)
        payload = fx.entry_dir("alpha_a") / "alpha_a.f64"
        data = bytearray(payload.read_bytes())
        data[8] ^= 1
        payload.write_bytes(bytes(data))
        self.refuse(fx, "payload SHA-256 mismatch")
        payload.unlink()
        (fx.entry_dir("alpha_a") / "alpha_a.json").unlink()
        self.refuse(fx, "missing entry alpha_a")

    def test_role_payload_tampering_refused(self):
        fx = Fixture(self.root / "role", self.p, self.signals)
        close = fx.manifest.parent / "close.f64"
        data = bytearray(close.read_bytes())
        data[16] ^= 1
        close.write_bytes(bytes(data))
        self.refuse(fx, "payload SHA close.f64")



# ------------------------------------------------------------- T11: v3-admit-v1
def screen_world(seed=21, names=60, score_begin=150):
    """2020-05-01 .. 2022-04-02 calendar sessions; FIT and HOLD both populated; persistent alphas."""
    rng = np.random.default_rng(seed)
    day0 = np.datetime64("2020-05-01", "D")
    dates = int((np.datetime64("2022-03-31", "D") - day0).astype(int)) + 3  # last decision 2022-03-31

    def ar(phi=0.97):
        z = np.empty((dates, names))
        z[0] = rng.normal(size=names)
        for t in range(1, dates):
            z[t] = phi * z[t - 1] + math.sqrt(1 - phi * phi) * rng.normal(size=names)
        return z

    z1, z2, u, v, w = ar(), ar(), ar(), ar(), ar()
    ret = rng.uniform(0.5, 1.5, names)[None, :] * rng.normal(0.0003, 0.01, dates)[:, None]
    ret = ret + rng.normal(0, 0.015, (dates, names))
    ret[1:] += 0.004 * (z1[:-1] + z2[:-1])  # r(d+2) loads on z(d+1) ~ .97 z(d)
    close = 40.0 * np.exp(np.cumsum(np.log1p(ret), axis=0))
    present = np.ones((dates, names), dtype=np.uint8)
    member = present.copy()
    member[:63] = 0
    sessions = day0.astype("datetime64[ns]").astype(np.int64) + DAY * np.arange(dates, dtype=np.int64)
    panel = {"close": close, "raw": close.copy(), "volume": rng.lognormal(12, 1, (dates, names)),
             "present": present, "member": member, "sessions": sessions,
             "ids": np.arange(101, 101 + names, dtype=np.uint64), "score_begin": score_begin, "rng": rng}
    hold = (sessions >= fcw.HOLD_BEGIN_NS)[:, None]
    insufficient = np.full((dates, names), NAN)
    insufficient[score_begin:score_begin + 100] = z2[score_begin:score_begin + 100]
    signals = {"slow_a": z1 + 0.1 * u, "slow_a_twin": z1 + 0.6 * v, "slow_b": -(z2 + 0.1 * w),
               "fast_a": z1 + rng.normal(0, 3, (dates, names)), "flip": np.where(hold, -z1, z1),
               "insufficient": insufficient}
    ids = list(signals)
    live = member == 1
    return panel, [np.where(live, signals[i], NAN) for i in ids], ids


SCREEN_RUNNER_SIGNS = [1, 1, 1, 1, 1, 1]  # slow_b's runner sign (+1) disagrees with its screen sign (-1)


class ScreenRules(unittest.TestCase):
    """screen_v3 on hand-built factor rows: 400 FIT then 200 HOLD decisions."""

    def test_every_rule_and_precedence(self):
        rng = np.random.default_rng(0)
        t = 600
        fit, hold = np.arange(t) < 400, np.arange(t) >= 400
        noise = lambda: rng.normal(0, 1, t)  # noqa: E731
        base = noise()
        a = 0.3 + base
        a[:100] = NAN  # 300 live FIT days
        b = 0.2 + base + 0.3 * noise()  # ~ A with a lower FIT Sharpe -> redundant with A
        c = 0.25 + noise()
        d = np.where(fit, 0.3 + noise(), -0.3 + noise())  # FIT good, HOLD bad
        e = 0.3 + noise()
        e[200:400] = NAN  # 200 live FIT days
        f = 0.3 + noise()  # good but high turnover
        g = np.where(np.arange(t) < 260, 0.3 + base, 0.3 + noise())
        g[260:400] = NAN  # 260 FIT days; only 160 in common with A -> uncorrelated by rule
        h = -(0.3 + noise())  # negative orientation
        i = e.copy()  # insufficient AND high turnover -> insufficient wins, both listed
        rows = fcw.screen_v3(np.vstack([a, b, c, d, e, f, g, h, i]),
                             [0.1, 0.1, 0.1, 0.1, 0.1, 0.9, 0.1, 0.1, 0.9], list("abcdefghi"), fit, hold)
        by = dict(zip("abcdefghi", rows))
        self.assertEqual({k: r["status"] for k, r in by.items()},
                         {"a": "admitted", "b": "reject_redundant", "c": "admitted", "d": "reject_unstable",
                          "e": "reject_insufficient", "f": "reject_turnover", "g": "admitted", "h": "admitted",
                          "i": "reject_insufficient"})
        self.assertEqual(by["b"]["redundant_with"], "a")
        self.assertGreater(by["b"]["max_abs_rho"], 0.7)
        self.assertEqual(by["b"]["max_abs_rho_with"], "a")
        self.assertEqual(by["i"]["failed_checks"], ["insufficient", "turnover"])
        self.assertEqual(by["f"]["failed_checks"], ["turnover"])
        self.assertEqual(by["h"]["s_k"], -1)
        self.assertGreater(by["h"]["fit_sharpe"], 0)
        self.assertEqual((by["a"]["fit_days"], by["e"]["fit_days"], by["g"]["fit_days"]), (300, 200, 260))
        # whichever of a/g is processed second notes the other as a low-overlap pair
        notes = sorted(by["a"]["low_overlap_with"] + by["g"]["low_overlap_with"])
        self.assertIn(notes, (["a"], ["g"]))
        admitted = sorted((r["admission_rank"], k) for k, r in by.items() if r["status"] == "admitted")
        sharpe = [by[k]["fit_sharpe"] for _, k in admitted]
        self.assertEqual(sharpe, sorted(sharpe, reverse=True))  # admission follows descending FIT Sharpe
        mean = np.nanmean(a[fit])
        sd = np.nanstd(a[fit], ddof=1)
        self.assertAlmostEqual(by["a"]["fit_sharpe"], mean / sd * math.sqrt(252), places=12)
        self.assertAlmostEqual(by["d"]["hold_mean"], float(d[hold].mean()), places=15)
        self.assertLess(by["d"]["hold_mean"], 0)


class Admission(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel, signals, ids = screen_world()
        cls.ids = ids
        cls.fx = Fixture(cls.root / "fx", panel, signals, SCREEN_RUNNER_SIGNS, ids=ids, families=["fam"] * len(ids),
                         vm_identity="dslvm1_clang18.1_fma", field_ids={"slow_b", "flip"})
        cls.out = cls.root / "fresh"
        cls.code, cls.summary = fcw.fit(cls.fx.args(cls.out, "v3-admit-v1"))
        cls.bytes = {p.name: p.read_bytes() for p in cls.out.iterdir()}
        cls.adm = json.loads(cls.bytes[fcw.OUTPUT_ADMISSION])
        cls.doc = json.loads(cls.bytes[fcw.OUTPUT_WEIGHTS])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_fit(self, out, **override):
        return fcw.fit(self.fx.args(out, "v3-admit-v1", **override))

    def assert_same(self, out):
        got = {p.name: p.read_bytes() for p in Path(out).iterdir()}
        self.assertEqual(sorted(got), sorted(self.bytes))
        for name in got:
            self.assertEqual(got[name], self.bytes[name], name)

    def test_one_candidate_per_class_and_weights_only_for_admitted(self):
        self.assertEqual(self.code, fcw.EXIT_OK)
        rows = {c["id"]: c for c in self.adm["candidates"]}
        self.assertEqual({i: rows[i]["status"] for i in self.ids},
                         {"slow_a": "admitted", "slow_a_twin": "reject_redundant", "slow_b": "admitted",
                          "fast_a": "reject_turnover", "flip": "reject_unstable",
                          "insufficient": "reject_insufficient"})
        self.assertEqual(rows["slow_a_twin"]["redundant_with"], "slow_a")
        self.assertGreater(rows["slow_a"]["fit_sharpe"], rows["slow_a_twin"]["fit_sharpe"])
        self.assertGreater(rows["slow_a_twin"]["max_abs_rho"], 0.7)
        self.assertEqual(rows["slow_b"]["s_k"], -1)
        self.assertLess(rows["flip"]["hold_mean"], 0)
        self.assertGreater(rows["flip"]["fit_sharpe"], 0)
        self.assertGreater(rows["fast_a"]["tau"], 0.7)
        self.assertLess(rows["slow_a"]["tau"], 0.7)
        self.assertEqual(rows["insufficient"]["fit_days"], 100)
        self.assertEqual({i: rows[i]["cache_entry"] for i in ("slow_a", "slow_b", "flip")},
                         {"slow_a": "base", "slow_b": "fields", "flip": "fields"})
        self.assertEqual(self.adm["inputs"]["vm_identity"], "dslvm1_clang18.1_fma")
        self.assertEqual(self.adm["inputs"]["fields_manifest_sha256"], self.fx.fields_sha)
        self.assertEqual(sorted(self.adm["admitted"]), ["slow_a", "slow_b"])
        self.assertEqual(self.adm["sign_conflicts"], ["slow_b"])
        self.assertEqual(self.adm["counts"], {"admitted": 2, "reject_insufficient": 1, "reject_turnover": 1,
                                              "reject_unstable": 1, "reject_redundant": 1})
        w = self.doc["weights"]
        self.assertEqual({i for i in self.ids if w[i] > 0}, {"slow_a", "slow_b"})
        self.assertAlmostEqual(sum(w.values()), 1.0, places=12)
        self.assertEqual(self.doc["signs"], {i: rows[i]["s_k"] for i in self.ids})  # screen signs, all nonzero
        self.assertEqual(self.doc["signs"]["slow_b"], -1)
        self.assertEqual(self.doc["provenance"]["sign_conflicts_weighted"], ["slow_b"])
        self.assertEqual(self.doc["train_manifest_sha256"], self.fx.train_sha)
        self.assertEqual(self.doc["provenance"]["role_manifest_sha256"], self.fx.train_sha)
        self.assertEqual(self.doc["provenance"]["admission_sha256"], sha(self.bytes[fcw.OUTPUT_ADMISSION]))
        taus = {c["id"]: c["tau"] for c in self.adm["candidates"]}
        self.assertAlmostEqual(self.doc["provenance"]["weighted_standalone_turnover"],
                               sum(w[i] * taus[i] for i in self.ids), places=14)
        runner_accepts(self.bytes[fcw.OUTPUT_WEIGHTS], self.fx.library_sha, self.ids, self.fx.train_sha)

    def test_admission_files_schema_and_csv(self):
        self.assertEqual(self.adm["schema"], "atx.dsl-admission/v1")
        self.assertEqual(self.adm["screen"], "v3-admit-v1")
        self.assertEqual(self.adm["inputs"]["train_manifest_sha256"], self.fx.train_sha)
        self.assertEqual(self.adm["inputs"]["orientations_sha256"], self.fx.orientations_sha)
        self.assertEqual([c["id"] for c in self.adm["candidates"]], self.ids)
        lines = self.bytes[fcw.OUTPUT_ADMISSION_CSV].decode().splitlines()
        self.assertEqual(lines[0].split(","), list(fcw.CSV_COLUMNS))
        self.assertEqual(len(lines), 1 + len(self.ids))
        twin = dict(zip(fcw.CSV_COLUMNS, lines[1 + self.ids.index("slow_a_twin")].split(",")))
        self.assertEqual((twin["status"], twin["redundant_with"]), ("reject_redundant", "slow_a"))
        self.assertEqual(float(twin["redundant_rho"]), self.adm["candidates"][self.ids.index("slow_a_twin")]["redundant_rho"])
        self.assertEqual(self.bytes[fcw.OUTPUT_ADMISSION], fcw.canonical_bytes(self.adm))

    def test_incremental_paths_are_byte_identical(self):
        work = self.root / "work"
        # 1) two new candidates, then a clean stop: exit 3, nothing published, work persisted
        stopped = self.root / "never"
        argv = self.fx.argv(stopped, "v3-admit-v1", ["--work-dir", str(work), "--max-new-candidates", "2"])
        self.assertEqual(fcw.main(argv), fcw.EXIT_INCOMPLETE)
        self.assertFalse(stopped.exists())
        store = store_base(work, self.fx.train_sha)
        context_dir = store / "context" / fcw.producer_fingerprint(fcw.CONTEXT_PRODUCERS)
        self.assertEqual(len(list((store / "factor").glob("*.json"))), 2)
        self.assertTrue((context_dir / "context.json").is_file())
        # 2) a soft time budget already spent: stops before the next candidate, context reused
        with self.assertRaises(fcw.Incomplete) as caught:
            self.run_fit(stopped, work_dir=work, max_seconds=1e-9)
        self.assertEqual((caught.exception.summary["computed_this_run"], caught.exception.summary["reused"]), (0, 2))
        self.assertFalse(stopped.exists())
        # 3) resume to completion: only the four missing candidates are computed
        code, summary = self.run_fit(self.root / "resumed", work_dir=work)
        self.assertEqual((code, summary["computed_this_run"], summary["reused"]), (fcw.EXIT_OK, 4, 2))
        self.assert_same(self.root / "resumed")
        # 4) everything cached: nothing computed
        code, summary = self.run_fit(self.root / "cached", work_dir=work)
        self.assertEqual((summary["computed_this_run"], summary["reused"]), (0, 6))
        self.assert_same(self.root / "cached")
        # 5) a tampered record fails its SHA check and is recomputed
        record = sorted((store / "factor").glob("*.json"))[0]
        j = json.loads(record.read_bytes())
        j["body"]["f_unsigned"][5] = 0.123
        record.write_bytes(json.dumps(j).encode())
        code, summary = self.run_fit(self.root / "retampered", work_dir=work)
        self.assertEqual(summary["computed_this_run"], 1)
        self.assert_same(self.root / "retampered")
        # 6) a corrupt context is rebuilt (bit-identical) when a candidate must be recomputed
        before = (context_dir / "context.json").read_bytes()
        basis = context_dir / "basis.bin"
        data = bytearray(basis.read_bytes())
        data[100] ^= 1
        basis.write_bytes(bytes(data))
        record.unlink()
        code, summary = self.run_fit(self.root / "rebuilt", work_dir=work)
        self.assertEqual(summary["computed_this_run"], 1)
        self.assertEqual((context_dir / "context.json").read_bytes(), before)
        self.assert_same(self.root / "rebuilt")
        # 7) with every record cached the role price payload is never read
        close = self.fx.manifest.parent / "close.f64"
        close.rename(close.with_name("close.away"))
        try:
            code, summary = self.run_fit(self.root / "no_payload", work_dir=work)
        finally:
            close.with_name("close.away").rename(close)
        self.assertEqual(code, fcw.EXIT_OK)
        self.assert_same(self.root / "no_payload")

    def test_nothing_admitted_publishes_table_only(self):
        keep = [3, 4, 5]  # fast_a, flip, insufficient: every one rejected
        panel, signals, ids = screen_world()
        fx = Fixture(self.root / "rejects", panel, [signals[k] for k in keep], [1] * len(keep),
                     ids=[ids[k] for k in keep], families=["fam"] * len(keep))
        out = self.root / "rejects_out"
        self.assertEqual(fcw.main(fx.argv(out, "v3-admit-v1")), fcw.EXIT_NO_WEIGHTS)
        self.assertEqual(sorted(p.name for p in out.iterdir()), sorted([fcw.OUTPUT_ADMISSION, fcw.OUTPUT_ADMISSION_CSV]))
        self.assertEqual(json.loads((out / fcw.OUTPUT_ADMISSION).read_bytes())["admitted"], [])



# ------------------------------------------------- fix round 1: cache layout + boundaries
NON_DEV_IDENTITY = "dslvm1_clang18.1_fma"


class CacheLayoutResolution(unittest.TestCase):
    """The runner's landed layout: ROOT = DIR[/identity], base under R, field candidates under F."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.p = synthetic_panel(dates=200, score_begin=150)
        self.signals = synthetic_signals(self.p)

    def tearDown(self):
        self.tmp.cleanup()

    def fixture(self, name, **kw):
        return Fixture(self.root / name, self.p, self.signals, **kw)

    def refuse(self, fx, fragment, **override):
        out = self.root / "refused"
        with self.assertRaises(fcw.FitError) as caught:
            fcw.fit(fx.args(out, **override))
        self.assertIn(fragment, str(caught.exception))
        self.assertFalse(out.exists())

    def test_field_candidates_resolve_under_a_non_dev_identity_root(self):
        fx = self.fixture("ok", vm_identity=NON_DEV_IDENTITY, field_ids={"alpha_b", "tie_heavy"})
        self.assertTrue((fx.cache / NON_DEV_IDENTITY / fx.fields_sha / "alpha_b.json").is_file())
        self.assertTrue((fx.cache / NON_DEV_IDENTITY / fx.train_sha / "alpha_a.json").is_file())
        code, _ = fcw.fit(fx.args(self.root / "ok_out"))
        doc = json.loads((self.root / "ok_out" / fcw.OUTPUT_WEIGHTS).read_bytes())
        rows = {r["id"]: r for r in doc["provenance"]["candidates"]}
        self.assertEqual((rows["alpha_b"]["cache_entry"], rows["tie_heavy"]["cache_entry"],
                          rows["alpha_a"]["cache_entry"]), ("fields", "fields", "base"))
        self.assertEqual(doc["provenance"]["vm_identity"], NON_DEV_IDENTITY)
        self.assertEqual(doc["provenance"]["fields_manifest_sha256"], fx.fields_sha)
        self.assertEqual(doc["provenance"]["runner_summary_sha256"], fx.summary_sha)
        runner_accepts((self.root / "ok_out" / fcw.OUTPUT_WEIGHTS).read_bytes(), fx.library_sha, IDS, fx.train_sha)
        # the layout is transport only: the legacy flat layout gives the same weights
        flat = self.fixture("flat")
        fcw.fit(flat.args(self.root / "flat_out"))
        self.assertEqual(json.loads((self.root / "flat_out" / fcw.OUTPUT_WEIGHTS).read_bytes())["weights"],
                         doc["weights"])

    def test_fields_manifest_mismatches_refused(self):
        fx = self.fixture("wrong_f", field_ids={"alpha_b"}, sidecar_patch={"alpha_b": {"fields_manifest_sha256": "0" * 64}})
        self.refuse(fx, "fields manifest mismatch alpha_b")
        fx = self.fixture("base_names_f", field_ids={"alpha_b"},
                          sidecar_patch={"alpha_a": {"fields_manifest_sha256": "ef" * 32}})
        self.refuse(fx, "fields manifest mismatch alpha_a")

    def test_vm_identity_rules(self):
        fx = self.fixture("foreign", vm_identity=NON_DEV_IDENTITY,
                          sidecar_patch={"alpha_a": {"vm_identity": fcw.LEGACY_VM_IDENTITY}})
        self.refuse(fx, "VM identity mismatch alpha_a")
        # keyless (pre-identity) sidecars: legacy identity + verified legacy engine builds only (the v1 cache)
        fcw.fit(self.fixture("keyless_ok", keyless_legacy=True).args(self.root / "keyless_out"))
        self.refuse(self.fixture("keyless_new_id", vm_identity=NON_DEV_IDENTITY, keyless_legacy=True),
                    "VM identity mismatch")
        self.refuse(self.fixture("keyless_engine", keyless_legacy=True,
                                 sidecar_patch={"alpha_a": {"engine_git_sha": "f" * 40}}), "VM identity mismatch alpha_a")

    def test_other_dsl_entries_ignored_and_duplicates_refused(self):
        fx = self.fixture("grown", field_ids={"alpha_b"})
        base = fx.cache_root / fx.train_sha
        stale = json.loads((fx.entry_dir("alpha_b") / "alpha_b.json").read_bytes())
        stale.pop("fields_manifest_sha256")
        (base / "alpha_b.json").write_bytes(json.dumps(dict(stale, dsl_sha256="12" * 32)).encode())
        code, _ = fcw.fit(fx.args(self.root / "grown_out"))  # older library's same-id entry is ignored
        self.assertEqual(code, fcw.EXIT_OK)
        (base / "alpha_b.json").write_bytes(json.dumps(stale).encode())
        self.refuse(fx, "entries in both the base and the fields directory")

    def test_summary_binding_refusals(self):
        fx = self.fixture("summary", vm_identity=NON_DEV_IDENTITY, field_ids={"alpha_b"})
        self.refuse(fx, "runner summary: SHA-256 pin differs", runner_summary_sha256="0" * 64)
        good = json.loads(json.dumps(fx.summary_doc))
        cases = [
            (dict(good, status="running"), "run not complete"),
            (dict(good, orientations_artifact_sha256="1" * 64), "not the run that wrote the pinned orientations"),
            (dict(good, roles=good["roles"] + [{"role": "validation"}]), "TRAIN-only IC run"),
            (dict(good, roles=[dict(good["roles"][0], candidate_cache=None)]), "did not use --candidate-cache"),
            (dict(good, roles=[dict(good["roles"][0], candidate_cache=dict(
                good["roles"][0]["candidate_cache"], vm_identity="dslvm9_other"))]), "DIR/<vm identity>"),
            (dict(good, roles=[dict(good["roles"][0], candidate_cache=dict(
                good["roles"][0]["candidate_cache"], fields_directory=str(fx.cache_root / ("0" * 64))))]),
             "fields_directory is not ROOT/<fields manifest sha>"),
        ]
        for doc, fragment in cases:
            fx.write_summary(doc)
            self.refuse(fx, fragment)
        fx.write_summary()

    def test_orientation_identity_and_stale_pending_refused(self):
        fx = self.fixture("orient")
        doc = json.loads(fx.orientations.read_bytes())
        doc["candidates"][0]["family"] = "other"
        fx.orientations.write_bytes(json.dumps(doc).encode())
        self.refuse(fx, "orientations: candidate identity alpha_a", orientations_sha256=sha(fx.orientations.read_bytes()))
        fx = self.fixture("pending")
        out = self.root / "pending_out"
        fcw.pending_path(out).mkdir()
        with self.assertRaises(fcw.FitError) as caught:
            fcw.fit(fx.args(out, work_dir=self.root / "pending_work"))
        self.assertIn("stale or concurrent partial output", str(caught.exception))
        self.assertFalse((self.root / "pending_work").exists())  # refused before any compute


class CacheLayoutV2(unittest.TestCase):
    """A v2 runner summary's candidate_cache.entries[]: v2 content-keyed entries, v1 entries read in place."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.p = synthetic_panel(dates=200, score_begin=150)
        self.signals = synthetic_signals(self.p)

    def tearDown(self):
        self.tmp.cleanup()

    def fixture(self, name, layout="v2", **kw):
        kw.setdefault("vm_identity", NON_DEV_IDENTITY)
        kw.setdefault("field_ids", {"alpha_b", "tie_heavy"})
        return Fixture(self.root / name, self.p, self.signals, layout=layout, **kw)

    def refuse(self, fx, fragment, **override):
        out = self.root / "refused"
        with self.assertRaises(fcw.FitError) as caught:
            fcw.fit(fx.args(out, **override))
        self.assertIn(fragment, str(caught.exception))
        self.assertFalse(out.exists())

    def fit_files(self, fx, name, screen="none", **override):
        out = self.root / name
        code, summary = fcw.fit(fx.args(out, screen, **override))
        self.assertEqual(code, fcw.EXIT_OK)
        return {p.name: p.read_bytes() for p in out.iterdir()}, summary

    def assert_same_bytes_but_summary_pin(self, files, summary_sha, ref_files, ref_summary_sha):
        """Byte identity once the (necessarily different) runner summary pin, and with it the
        admission.json SHA the weights file records, are swapped back."""
        self.assertNotEqual(summary_sha, ref_summary_sha)
        self.assertEqual(sorted(files), sorted(ref_files))
        swaps = [(summary_sha, ref_summary_sha)]
        if fcw.OUTPUT_ADMISSION in files:
            swaps.append((sha(files[fcw.OUTPUT_ADMISSION]), sha(ref_files[fcw.OUTPUT_ADMISSION])))
        for name, data in files.items():
            for mine, ref in swaps:
                data = data.replace(mine.encode(), ref.encode())
            self.assertEqual(data, ref_files[name], name)

    def test_v2_entries_live_at_content_keyed_paths(self):
        fx = self.fixture("paths")
        base = fx.cache / NON_DEV_IDENTITY / fx.train_sha
        fk16 = sha(f"field=book_to_price:{'56' * 32}\nfield=sv_ratio126:{'34' * 32}\n".encode())[:16]
        self.assertTrue((base / f"alpha_a.{fx.dsl_sha[0][:16]}.f64").is_file())
        self.assertTrue((base / f"fp_{fk16}" / f"alpha_b.{fx.dsl_sha[1][:16]}.json").is_file())
        self.assertFalse((base / "alpha_a.json").exists())
        self.assertFalse((fx.cache / NON_DEV_IDENTITY / fx.fields_sha).exists())
        role = fcw.RoleManifest(fx.manifest, fx.train_sha)
        layout = fcw.CacheLayout(fx.summary, fx.summary_sha, role, fx.orientations_sha, "cd" * 32)
        cands = fcw.load_library(fx.library, fx.library_sha)
        got = {c["id"]: layout.resolve(c, role) for c in cands}
        self.assertEqual(got["alpha_a"]["payload"], base / f"alpha_a.{fx.dsl_sha[0][:16]}.f64")
        self.assertEqual((got["alpha_a"]["fields_manifest_sha256"], got["alpha_b"]["fields_manifest_sha256"]),
                         (None, fx.fields_sha))  # a field candidate's work stays keyed by the pinned manifest
        self.assertEqual(fcw.signal_key_sha256(NON_DEV_IDENTITY, role, fx.dsl_sha[1], FIELD_PAYLOADS),
                         v2_signal_key(NON_DEV_IDENTITY, fx.train_sha, role.dates, role.instruments, fx.dsl_sha[1],
                                       FIELD_PAYLOADS))

    def test_every_layout_fits_the_same_bytes_and_shares_work_records(self):
        work = self.root / "work"
        ref = self.fixture("v1", "v1")
        ref_files, summary = self.fit_files(ref, "v1_out", work_dir=work)
        self.assertEqual(summary["computed_this_run"], len(IDS))
        # v8 store: records are keyed by the signal payload SHA, identical bytes in every layout, so every layout
        # after the first reuses every record (the W3 work key recomputed v1 field candidates once).
        for layout, computed in (("v2", 0), ("v1-listed", 0)):
            fx = self.fixture(layout, layout)
            self.assertEqual(fx.train_sha, ref.train_sha)
            files, summary = self.fit_files(fx, f"{layout}_out", work_dir=work)
            self.assertEqual((summary["computed_this_run"], summary["reused"]), (computed, len(IDS) - computed),
                             layout)
            self.assert_same_bytes_but_summary_pin(files, fx.summary_sha, ref_files, ref.summary_sha)
        doc = json.loads(files[fcw.OUTPUT_WEIGHTS])
        rows = {r["id"]: r for r in doc["provenance"]["candidates"]}
        self.assertEqual([rows[i]["cache_entry"] for i in ("alpha_a", "alpha_b", "tie_heavy")], ["base", "fields", "fields"])
        runner_accepts(files[fcw.OUTPUT_WEIGHTS], fx.library_sha, IDS, fx.train_sha)

    def test_admission_bytes_do_not_depend_on_the_cache_layout(self):
        panel, signals, ids = screen_world()
        got = {}
        for layout in ("v1", "v2", "v1-listed"):
            fx = Fixture(self.root / layout, panel, signals, SCREEN_RUNNER_SIGNS, ids=ids,
                         families=["fam"] * len(ids), vm_identity=NON_DEV_IDENTITY, field_ids={"slow_b", "flip"},
                         layout=layout)
            got[layout] = (fx.summary_sha, self.fit_files(fx, f"{layout}_out", "v3-admit-v1")[0])
        ref_sha, ref_files = got["v1"]
        self.assertIn(fcw.OUTPUT_ADMISSION, ref_files)
        self.assertEqual(len(json.loads(ref_files[fcw.OUTPUT_ADMISSION])["candidates"]), len(ids))
        for layout in ("v2", "v1-listed"):
            summary_sha, files = got[layout]
            self.assertIn(summary_sha.encode(), files[fcw.OUTPUT_ADMISSION])  # the pin is the only difference
            self.assert_same_bytes_but_summary_pin(files, summary_sha, ref_files, ref_sha)

    def test_v1_field_entry_under_an_older_manifest_is_read_in_place(self):
        fx = self.fixture("legacy", "v1-listed")
        old = "0a" * 32
        old_dir = fx.cache_root / old
        old_dir.mkdir()
        doc = json.loads(json.dumps(fx.summary_doc))
        for e in fx.cache_summary(doc)["entries"]:
            if e["field_payload_sha256"]:  # --cache-legacy-fields: the runner matched these field payloads
                cid = e["id"]
                side = dict(json.loads(fx.sidecar_path(cid).read_bytes()), fields_manifest_sha256=old)
                (old_dir / f"{cid}.json").write_bytes(json.dumps(side).encode())
                fx.payload_path(cid).replace(old_dir / f"{cid}.f64")
                fx.sidecar_path(cid).unlink()
                e.update(sidecar=str(old_dir / f"{cid}.json"), payload=str(old_dir / f"{cid}.f64"))
        fx.write_summary(doc)
        files, _ = self.fit_files(fx, "legacy_out")
        rows = {r["id"]: r for r in json.loads(files[fcw.OUTPUT_WEIGHTS])["provenance"]["candidates"]}
        self.assertEqual((rows["alpha_b"]["cache_entry"], rows["alpha_a"]["cache_entry"]), ("fields", "base"))
        side = json.loads((old_dir / "alpha_b.json").read_bytes())
        (old_dir / "alpha_b.json").write_bytes(json.dumps(dict(side, fields_manifest_sha256=fx.fields_sha)).encode())
        self.refuse(fx, "fields manifest mismatch alpha_b")  # the sidecar must name its own directory's manifest

    def test_summary_entry_refusals(self):
        fx = self.fixture("summary")
        good = json.loads(json.dumps(fx.summary_doc))

        def edited(mutate):
            doc = json.loads(json.dumps(good))
            mutate(fx.cache_summary(doc))
            return doc

        def entry(k, **kw):
            return edited(lambda c: c["entries"][k].update(kw))

        elsewhere = fx.cache_root / "elsewhere"
        cases = [
            (edited(lambda c: c["entries"].pop(0)), "missing entry alpha_a"),
            (edited(lambda c: c["entries"].append(dict(c["entries"][0]))), "duplicate candidate_cache entry alpha_a"),
            (edited(lambda c: c.update(entries={})), "candidate_cache.entries must be a list"),
            (entry(0, layout="v3"), "malformed candidate_cache entry"),
            (entry(0, payload_sha256="A" * 64), "malformed candidate_cache entry"),
            (entry(0, payload_sha256="0" * 64), "entry mismatch alpha_a"),
            (entry(0, signal_key_sha256="0" * 64), "signal key mismatch alpha_a"),
            (entry(0, layout="v1"), "entry path mismatch alpha_a"),  # a v1 stem is <id>
            (entry(0, sidecar=str(elsewhere / f"{fx.stem('alpha_a')}.json")), "entry path mismatch alpha_a"),
            (entry(0, payload=str(elsewhere / f"{fx.stem('alpha_a')}.f64")), "entry path mismatch alpha_a"),
            (entry(1, field_payload_sha256={"sv_ratio126": "34" * 32}), "entry path mismatch alpha_b"),  # fk16
            (entry(0, field_payload_sha256=dict(FIELD_PAYLOADS)), "entry path mismatch alpha_a"),
        ]
        unpinned = edited(lambda c: c.pop("fields_directory"))
        del unpinned["roles"][0]["research_fields"]
        cases.append((unpinned, "field entry alpha_b without a pinned research fields manifest"))
        for doc, fragment in cases:
            fx.write_summary(doc)
            self.refuse(fx, fragment)
        fx.write_summary()
        fcw.fit(fx.args(self.root / "summary_ok"))

    def test_sidecar_and_payload_refusals(self):
        for name, kw, fragment in [
            ("vm", {"sidecar_patch": {"alpha_a": {"vm_identity": fcw.LEGACY_VM_IDENTITY}}}, "VM identity mismatch alpha_a"),
            ("key", {"sidecar_patch": {"alpha_b": {"field_payload_sha256": {"sv_ratio126": "34" * 32}}}},
             "signal key mismatch alpha_b"),
            ("schema", {"sidecar_patch": {"alpha_a": {"schema": fcw.CACHE_SCHEMA}}}, "signal key mismatch alpha_a"),
            ("dsl", {"sidecar_patch": {"alpha_a": {"dsl_sha256": "12" * 32}}}, "records another candidate or DSL"),
            ("geometry", {"sidecar_patch": {"alpha_a": {"dates": 199}}}, "entry mismatch alpha_a"),
            ("role", {"sidecar_role": "validation"}, "is not a TRAIN-role signal"),
        ]:
            self.refuse(self.fixture(name, **kw), fragment)
        fx = self.fixture("payload")
        data = bytearray(fx.payload_path("alpha_a").read_bytes())
        data[0] ^= 1
        fx.payload_path("alpha_a").write_bytes(bytes(data))
        self.refuse(fx, "payload SHA-256 mismatch alpha_a")
        fx.sidecar_path("alpha_a").unlink()
        self.refuse(fx, "missing entry alpha_a")


class Boundaries(unittest.TestCase):
    def test_screen_boundaries(self):
        rng = np.random.default_rng(5)
        t = 600
        fit, hold = np.arange(t) < 400, np.arange(t) >= 400
        noise = lambda: rng.normal(0, 1, t)  # noqa: E731
        base = noise()
        a = 0.3 + base
        a[300:400] = NAN  # 300 FIT days
        b = 0.1 + base + 0.01 * noise()
        b[:50] = NAN  # 350 FIT days; 250 in common with a -> correlated
        c = 0.1 + base + 0.01 * noise()
        c[:51] = NAN  # 349 FIT days; 249 in common with a -> uncorrelated by rule
        d = 0.3 + noise()
        d[250:400] = NAN  # exactly 250 FIT days -> sufficient
        e = 0.3 + noise()
        e[hold] = NAN  # no live HOLD day -> unstable
        f = 0.3 + noise()  # tau exactly at the limit -> passes
        rows = fcw.screen_v3(np.vstack([a, b, c, d, e, f]), [0.1, 0.1, 0.1, 0.1, 0.1, 0.70], list("abcdef"), fit, hold)
        by = dict(zip("abcdef", rows))
        self.assertEqual((by["b"]["status"], by["b"]["redundant_with"]), ("reject_redundant", "a"))
        self.assertGreater(by["b"]["redundant_rho"], 0.99)
        self.assertEqual(by["c"]["status"], "admitted")
        self.assertIn("a", by["c"]["low_overlap_with"])  # 249 common FIT days with a (and fewer with d)
        self.assertEqual((by["d"]["fit_days"], by["d"]["status"]), (250, "admitted"))
        self.assertEqual((by["e"]["hold_days"], by["e"]["status"], by["e"]["failed_checks"]),
                         (0, "reject_unstable", ["unstable"]))
        self.assertEqual((by["f"]["failed_checks"], by["f"]["status"]), ([], "admitted"))

    def test_rho_exactly_at_limit_is_not_redundant(self):
        rng = np.random.default_rng(6)
        t = 600
        fit, hold = np.arange(t) < 400, np.arange(t) >= 400
        a = 0.3 + rng.normal(0, 1, t)
        b = -0.1 + 0.8 * a + 0.6 * rng.normal(0, 1, t)  # lower FIT Sharpe than a, rho ~ .8
        rho, _ = fcw.pair_correlation(a, b, fit)
        with unittest.mock.patch.object(fcw, "RHO_LIMIT", abs(rho)):
            rows = fcw.screen_v3(np.vstack([a, b]), [0.1, 0.1], ["a", "b"], fit, hold)
        self.assertEqual(rows[1]["status"], "admitted")
        with unittest.mock.patch.object(fcw, "RHO_LIMIT", float(np.nextafter(abs(rho), 0))):
            rows = fcw.screen_v3(np.vstack([a, b]), [0.1, 0.1], ["a", "b"], fit, hold)
        self.assertEqual((rows[1]["status"], rows[1]["redundant_rho"]), ("reject_redundant", abs(rho)))


class CacheInvalidation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        p = synthetic_panel(dates=200, score_begin=150)
        self.fx = Fixture(self.root / "fx", p, synthetic_signals(p), field_ids={"alpha_b"})
        self.work = self.root / "work"
        code, summary = fcw.fit(self.fx.args(self.root / "first", work_dir=self.work))
        self.assertEqual(summary["computed_this_run"], len(IDS))
        self.weights = json.loads((self.root / "first" / fcw.OUTPUT_WEIGHTS).read_bytes())["weights"]
        self.store = store_base(self.work, self.fx.train_sha)

    def tearDown(self):
        self.tmp.cleanup()

    def rerun(self, name):
        code, summary = fcw.fit(self.fx.args(self.root / name, work_dir=self.work))
        self.assertEqual(json.loads((self.root / name / fcw.OUTPUT_WEIGHTS).read_bytes())["weights"], self.weights)
        return summary

    def test_v1_field_record_is_keyed_by_its_signal_payload(self):
        # A v1 runner summary lists no field payload map: the record key is the payload SHA all the same.
        names = sorted(x.name for x in (self.store / "factor").glob("*.json"))
        self.assertEqual(len(names), len(IDS))
        store = fcw.WorkStore(self.work, fcw.RoleManifest(self.fx.manifest, self.fx.train_sha))
        payload = sha(self.fx.payload_path("alpha_b").read_bytes())
        self.assertIn(store.records.path("factor", store.key({"payload_sha256": payload}, "factor")).name, names)
        self.assertEqual(self.rerun("again")["computed_this_run"], 0)

    def test_script_sha_is_not_a_key_but_the_producer_fingerprint_is(self):
        with unittest.mock.patch.object(fcw, "SCRIPT_SHA256", "0" * 64):
            self.assertEqual(self.rerun("edited")["computed_this_run"], 0)
        with unittest.mock.patch.object(fcw, "producer_fingerprint", lambda funcs: "0" * 64):
            self.assertEqual(self.rerun("new_code")["computed_this_run"], len(IDS))

    def test_semantics_tag_change_misses_the_cache(self):
        with unittest.mock.patch.object(fcw, "SEMANTICS_TAG", "0" * 16):
            self.assertEqual(self.rerun("new_tag")["computed_this_run"], len(IDS))

    def test_record_bound_to_another_context_is_recomputed(self):
        records = sorted((self.store / "factor").glob("*.json"))
        rewrite_body(records[0], lambda b: b.update(context_sha256="ab" * 32))  # a valid record of another context
        records[1].unlink()  # forces the stored context to load
        self.assertEqual(self.rerun("rebound")["computed_this_run"], 2)



# ------------------------------------------------ W3 (platform v7): WorkStore keyed on the signal content key
class WorkStoreKeys(unittest.TestCase):
    """Records keyed on (semantics, role, VM identity, screen, DSL, field payload map) over synthetic v2 caches."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.panel, self.signals, self.ids, self.extra = v4_world()

    def tearDown(self):
        self.tmp.cleanup()

    def fixture(self, name, **kw):
        kw.setdefault("field_ids", {"slow_b", "flip"})
        kw.setdefault("candidate_extra", self.extra)
        return Fixture(self.root / name, self.panel, self.signals, [1] * len(self.ids), ids=self.ids,
                       families=["fam"] * len(self.ids), vm_identity=NON_DEV_IDENTITY, layout="v2", **kw)

    def fit(self, fx, name, work=None, **args):
        out = self.root / name
        err = io.StringIO()
        argv = fx.argv(out, args.pop("screen", "v4-prior-v1"), [
            "--orientation", "prior", "--composition", args.pop("composition", "ew-theme-v1"),
            *(["--work-dir", str(work)] if work else [])])
        with contextlib.redirect_stdout(io.StringIO()) as sout, contextlib.redirect_stderr(err):
            code = fcw.main(argv)
        self.assertIn(code, (fcw.EXIT_OK, fcw.EXIT_NO_WEIGHTS), err.getvalue())  # v4-prior-v2 may admit none
        summary = json.loads(sout.getvalue())
        line = [x for x in err.getvalue().splitlines() if x.startswith("fit: computed ")]
        self.assertEqual(line, [f"fit: computed {summary['computed_this_run']}, reused {summary['reused']}"])
        return {p.name: p.read_bytes() for p in out.iterdir()}, (summary["computed_this_run"], summary["reused"])

    def test_admission_bytes_identical_with_and_without_the_store(self):
        fx, work, n = self.fixture("fx"), self.root / "work", len(self.ids)
        bare, counts = self.fit(fx, "bare")
        self.assertEqual(counts, (n, 0))
        first, counts = self.fit(fx, "first", work)
        self.assertEqual(counts, (n, 0))
        second, counts = self.fit(fx, "second", work)
        self.assertEqual(counts, (0, n))
        self.assertIn(fcw.OUTPUT_ADMISSION, bare)
        self.assertEqual(first, bare)
        self.assertEqual(second, bare)
        names = sorted(x.name for x in (store_base(work, fx.train_sha) / "factor").iterdir())
        self.assertEqual(len(names), n)
        self.assertTrue(all(re.fullmatch(r"[0-9a-f]{64}\.json", x) for x in names))

    def test_store_key_is_the_signal_payload_and_the_producer_fingerprint(self):
        fx = self.fixture("fx")
        store = fcw.WorkStore(self.root / "work", fcw.RoleManifest(fx.manifest, fx.train_sha))
        payload = sha(fx.payload_path("slow_b").read_bytes())
        key = store.key({"payload_sha256": payload}, "factor")
        self.assertEqual(key, {"schema": "atx.fit-candidate-factor/v2", "semantics_tag": fcw.SEMANTICS_TAG,
                               "role_manifest_sha256": fx.train_sha, "window_id": fcw.window_id(),
                               "cache_payload_sha256": payload,
                               "producer_fingerprint": fcw.producer_fingerprint(fcw.FACTOR_PRODUCERS),
                               "horizon_fingerprint": fcw.horizon_fingerprint()})
        aim = store.key({"payload_sha256": payload}, "aim")
        self.assertEqual(aim["producer_fingerprint"], fcw.producer_fingerprint(fcw.AIM_PRODUCERS))
        self.assertEqual(aim["train_window_ns"], [fcw.FIT_BEGIN_NS, fcw.TRAIN_END_NS])
        self.assertEqual(store.base, self.root / "work" / f"{fx.train_sha[:16]}-{fcw.window_id()}")

    def test_records_follow_the_signal_bytes_not_ids_pins_or_dsl(self):
        work, n = self.root / "work", len(self.ids)
        ref, counts = self.fit(self.fixture("a", fields_sha="ef" * 32), "a_out", work)
        self.assertEqual(counts, (n, 0))
        # a new field joins the manifest (new manifest SHA); the candidates' signals are unchanged
        grown = self.fixture("b", fields_sha="0f" * 32)
        files, counts = self.fit(grown, "b_out", work)
        self.assertEqual(counts, (0, n))
        adm_a, adm_b = json.loads(ref[fcw.OUTPUT_ADMISSION]), json.loads(files[fcw.OUTPUT_ADMISSION])
        self.assertEqual(adm_a["candidates"], adm_b["candidates"])
        self.assertEqual(adm_b["inputs"]["fields_manifest_sha256"], "0f" * 32)
        # other field pins and another DSL text with the same signal bytes: the factor record is the same function of
        # the same bytes, so it is reused
        extra = {i: dict(e) for i, e in self.extra.items()}
        extra["slow_a"]["dsl"] = "rank(close) * 0 + 1e-9 * rank(volume)"
        edited = self.fixture("d", fields_sha="1f" * 32, field_payloads=dict(FIELD_PAYLOADS, sv_ratio126="99" * 32),
                              candidate_extra=extra)
        self.assertEqual(self.fit(edited, "d_out", work)[1], (0, n))
        # changed signal bytes are a new record; everything else is reused
        signals = [s.copy() for s in self.signals]
        signals[self.ids.index("flip")][200, 3] += 0.5
        moved = Fixture(self.root / "e", self.panel, signals, [1] * n, ids=self.ids, families=["fam"] * n,
                        vm_identity=NON_DEV_IDENTITY, layout="v2", field_ids={"slow_b", "flip"},
                        candidate_extra=self.extra)
        self.assertEqual(self.fit(moved, "e_out", work)[1], (1, n - 1))

    def test_screen_is_not_part_of_the_key(self):
        fx, work, n = self.fixture("fx"), self.root / "work", len(self.ids)
        self.assertEqual(self.fit(fx, "p1", work)[1], (n, 0))
        self.assertEqual(self.fit(fx, "p2", work, screen="v4-prior-v2")[1], (0, n))  # a record does not read it
        self.assertEqual(self.fit(fx, "p1b", work, composition="ew-theme-v6")[1], (0, n))


# ------------------------------------------------ T13: mv-shrink-0.9-nonneg-netcost-v1
class NetCost(unittest.TestCase):
    def test_hand_case_cost_changes_the_weights(self):
        # Orthogonal zero-mean patterns with equal scale: S is diagonal and equal, so Sh = S and w ~ mu.
        e1, e2, e3 = np.array([1.0, -1, 1, -1]), np.array([1.0, 1, -1, -1]), np.array([1.0, -1, -1, 1])
        factors = np.vstack([0.003 + 0.01 * e1, 0.002 + 0.01 * e2, 0.001 + 0.01 * e3])
        taus = np.array([0.5, 1.5, 0.2])
        gross, _ = fcw.fit_weights(factors)
        np.testing.assert_allclose(gross, [0.5, 1 / 3, 1 / 6], rtol=0, atol=1e-12)
        # net mu = [.003 - .0009, .002 - .0027, .001 - .00036] = [.0021, -.0007, .00064] -> clip the middle
        net, raw = fcw.fit_weights(factors, fcw.NETCOST_C * taus)
        np.testing.assert_allclose(net, [0.0021 / 0.00274, 0.0, 0.00064 / 0.00274], rtol=0, atol=1e-12)
        self.assertLess(raw[1], 0)
        self.assertEqual(fcw.NETCOST_C, 0.0018)

    def test_netcost_changes_only_the_fit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            panel, signals, ids = screen_world()
            fx = Fixture(root / "fx", panel, signals, SCREEN_RUNNER_SIGNS, ids=ids, families=["fam"] * len(ids))
            work = root / "work"
            fcw.fit(fx.args(root / "default", "v3-admit-v1", work_dir=work))
            fcw.fit(fx.args(root / "explicit", "v3-admit-v1", work_dir=work, composition=fcw.RULE_ID))
            code, summary = fcw.fit(fx.args(root / "net", "v3-admit-v1", work_dir=work,
                                            composition=fcw.NETCOST_RULE_ID))
            self.assertEqual((code, summary["computed_this_run"]), (fcw.EXIT_OK, 0))  # records are shared
            read = lambda d, n: (root / d / n).read_bytes()  # noqa: E731
            for name in (fcw.OUTPUT_WEIGHTS, fcw.OUTPUT_ADMISSION, fcw.OUTPUT_ADMISSION_CSV):
                self.assertEqual(read("default", name), read("explicit", name))  # default == explicit default
            for name in (fcw.OUTPUT_ADMISSION, fcw.OUTPUT_ADMISSION_CSV):
                self.assertEqual(read("default", name), read("net", name))  # the screen never sees the cost
            gross = json.loads(read("default", fcw.OUTPUT_WEIGHTS))
            net = json.loads(read("net", fcw.OUTPUT_WEIGHTS))
            self.assertEqual(gross["provenance"]["rule"], "mv-shrink-0.9-nonneg-v1")
            self.assertNotIn("netcost_c", gross["provenance"])
            self.assertFalse(any("cost_drag" in r for r in gross["provenance"]["candidates"]))
            self.assertEqual((net["provenance"]["rule"], net["provenance"]["netcost_c"]),
                             ("mv-shrink-0.9-nonneg-netcost-v1", 0.0018))
            self.assertEqual(net["signs"], gross["signs"])
            self.assertEqual({i for i, w in net["weights"].items() if w > 0} <= set(json.loads(
                read("net", fcw.OUTPUT_ADMISSION))["admitted"]), True)
            # the published net weights are the hand formula on the admitted oriented series
            rows = {r["id"]: r for r in net["provenance"]["candidates"]}
            admitted = [i for i in ids if rows[i]["status"] == "fitted"]
            for i in admitted:
                self.assertAlmostEqual(rows[i]["cost_drag"], 0.0018 * rows[i]["tau"], places=15)
            self.assertNotEqual([net["weights"][i] for i in admitted], [gross["weights"][i] for i in admitted])



# ------------------------------------------------ T23: v4-prior-v1 + ew-theme-v1
def ref_newey_west_t(x, lag):
    """Independent loop port: Bartlett weights 1 - l/(L+1), autocovariances / n, t = mean / sqrt(LRV / n)."""
    n = len(x)
    m = sum(x) / n
    gamma = [sum((x[t] - m) * (x[t - ell] - m) for t in range(ell, n)) / n for ell in range(lag + 1)]
    lrv = gamma[0] + 2 * sum((1 - ell / (lag + 1)) * gamma[ell] for ell in range(1, lag + 1))
    return m / math.sqrt(lrv / n)


def v4_world():
    """screen_world plus a near-clone of slow_a, with v4 theme/tier/prior_sign metadata."""
    panel, signals, ids = screen_world()
    rng = np.random.default_rng(77)
    clone = signals[0] + 0.05 * rng.normal(size=signals[0].shape)
    signals, ids = signals + [clone], ids + ["slow_a_clone"]
    meta = {"slow_a": ("value", "B", 1), "slow_a_twin": ("value", "B+", 1), "slow_b": ("price_momentum", "A", 1),
            "fast_a": ("low_risk", "A\u2212", 1), "flip": ("short_interest", "C+", 1),
            "insufficient": ("options_implied", "B", 0), "slow_a_clone": ("value", "A", 1)}
    extra = {i: {"theme": t, "tier": g, "prior_sign": s} for i, (t, g, s) in meta.items()}
    return panel, signals, ids, extra


def write_recipe(path: Path, library_sha: str, rows, key="lineage") -> str:
    path.write_bytes(json.dumps({"schema": "atx.dsl-ic-recipe/v1", "library": {"path": "x.json", "sha256": library_sha},
                                 key: rows}).encode())
    return sha(path.read_bytes())


V4_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-v1")


class PriorScreenRules(unittest.TestCase):
    def test_newey_west_matches_loop_port(self):
        rng = np.random.default_rng(8)
        x = np.cumsum(rng.normal(0.01, 1, 300)) * 0.01 + rng.normal(0.02, 1, 300)
        for lag in (0, 1, 5):
            self.assertAlmostEqual(fcw.newey_west_t(x, lag), ref_newey_west_t(list(x), lag), places=10)
        m, sd_n = x.mean(), x.std(ddof=0)
        self.assertAlmostEqual(fcw.newey_west_t(x, 0), m / (sd_n / math.sqrt(len(x))), places=10)
        self.assertIsNone(fcw.newey_west_t(np.full(10, 0.3)))
        self.assertIsNone(fcw.newey_west_t(np.array([1.0])))
        self.assertEqual(fcw.NW_LAG, 5)

    def test_every_rule_order_and_thresholds(self):
        rng = np.random.default_rng(1)
        t = 600
        train = np.ones(t, dtype=bool)
        noise = lambda: rng.normal(0, 1, t)  # noqa: E731
        base = noise()
        a = 0.2 + base                              # tier B, first in roster
        b = 0.05 + base + 0.2 * noise()             # |rho(a,b)| > .9, lower Sharpe, better tier A -> b wins
        c = 0.1 + 0.85 * base + 0.53 * noise()      # |rho| ~ .85 vs a/b: kept (limit .90, not .70)
        d = -0.3 + noise()                          # clear contradiction of the prior -> veto
        e = noise()
        e = e - e.mean() - 0.03                     # mild contradiction (mean -.03, t ~ -.7 > -2): admitted
        f = 0.2 + noise()
        f[:351] = NAN                               # 249 live days -> insufficient
        g = 0.2 + noise()                           # tau .71 -> turnover
        h = 0.2 + noise()                           # tau exactly .70 -> passes
        k = 0.2 + noise()                           # prior_sign 0 -> no_prior (and turnover), weight 0
        ids = list("abcdefghk")
        rows = fcw.screen_v4(np.vstack([a, b, c, d, e, f, g, h, k]), [0.1] * 6 + [0.71, 0.70, 0.9], ids, train,
                             tier_rank=[4, 1, 4, 4, 4, 4, 4, 4, 4], prior_signs=[1] * 8 + [0])
        by = dict(zip(ids, rows))
        self.assertEqual({i: r["status"] for i, r in by.items()},
                         {"a": "reject_redundant", "b": "admitted", "c": "admitted", "d": "reject_veto",
                          "e": "admitted", "f": "reject_insufficient", "g": "reject_turnover", "h": "admitted",
                          "k": "reject_no_prior"})
        self.assertEqual(by["a"]["redundant_with"], "b")
        self.assertGreater(by["a"]["train_sharpe"], by["b"]["train_sharpe"])  # Sharpe never orders the pass
        self.assertGreater(by["a"]["redundant_rho"], 0.9)
        self.assertLess(by["c"]["max_abs_rho"], 0.9)
        self.assertGreater(by["c"]["max_abs_rho"], 0.7)
        self.assertLess(by["d"]["hac_t"], -2.0)
        self.assertLess(by["e"]["train_mean"], 0)
        self.assertGreater(by["e"]["hac_t"], -2.0)
        self.assertAlmostEqual(by["d"]["hac_t"], ref_newey_west_t(list(d), 5), places=10)
        self.assertEqual(by["f"]["train_days"], 249)
        self.assertEqual(by["k"]["failed_checks"], ["no_prior", "turnover"])
        self.assertEqual((by["k"]["s_k"], by["a"]["s_k"]), (0, 1))
        ranks = {i: r["admission_rank"] for i, r in by.items() if r["status"] == "admitted"}
        self.assertEqual(ranks, {"b": 1, "c": 2, "e": 3, "h": 4})  # (tier, roster order)

    def test_exactly_250_days_and_rho_at_limit(self):
        rng = np.random.default_rng(2)
        t = 400
        train = np.ones(t, dtype=bool)
        a = 0.2 + rng.normal(0, 1, t)
        a[250:] = NAN
        rows = fcw.screen_v4(np.vstack([a]), [0.1], ["a"], train, [0], [1])
        self.assertEqual((rows[0]["train_days"], rows[0]["status"]), (250, "admitted"))
        b = 0.3 + rng.normal(0, 1, t)
        c = 0.3 * b + 0.2 * rng.normal(0, 1, t)
        rho, _ = fcw.pair_correlation(b, c, train)
        with unittest.mock.patch.object(fcw, "V4_RHO_LIMIT", abs(rho)):
            self.assertEqual(fcw.screen_v4(np.vstack([b, c]), [0.1, 0.1], ["b", "c"], train, [0, 0], [1, 1])[1]["status"],
                             "admitted")
        with unittest.mock.patch.object(fcw, "V4_RHO_LIMIT", float(np.nextafter(abs(rho), 0))):
            self.assertEqual(fcw.screen_v4(np.vstack([b, c]), [0.1, 0.1], ["b", "c"], train, [0, 0], [1, 1])[1]["status"],
                             "reject_redundant")

    def test_ew_theme_weights_hand_case(self):
        w, table = fcw.ew_theme_weights(["value", "value", "low_risk", "value", "short_interest"])
        np.testing.assert_allclose(w, [1 / 9, 1 / 9, 1 / 3, 1 / 9, 1 / 3], rtol=0, atol=1e-16)
        self.assertAlmostEqual(float(w.sum()), 1.0, places=15)
        self.assertEqual(table["value"], {"admitted_count": 3, "theme_weight": 1 / 3, "member_weight": 1 / 9})


class PriorMetadata(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.lib = self.root / "lib.json"

    def tearDown(self):
        self.tmp.cleanup()

    def library(self, extra):
        cands = [{"id": i, "family": "fam", "dsl": f"rank(close) * {k}", **extra.get(i, {})} for k, i in enumerate("ab")]
        self.lib.write_bytes(json.dumps({"schema": fcw.LIBRARY_SCHEMA, "candidates": cands}).encode())
        lib_sha = sha(self.lib.read_bytes())
        return lib_sha, fcw.load_library(self.lib, lib_sha)

    def load(self, extra, recipe_rows=None, key="lineage", recipe_lib_sha=None):
        lib_sha, library = self.library(extra)
        path, rsha = None, None
        if recipe_rows is not None:
            path = self.root / "recipe.json"
            rsha = write_recipe(path, recipe_lib_sha or lib_sha, recipe_rows, key)
        return fcw.load_priors(self.lib, lib_sha, path, rsha, library)

    def refuse(self, fragment, *a, **kw):
        with self.assertRaises(fcw.FitError) as caught:
            self.load(*a, **kw)
        self.assertIn(fragment, str(caught.exception))

    def test_sources_merge_and_normalise(self):
        full = {"a": {"theme": "value", "tier": "B\u2212", "prior_sign": 1},
                "b": {"theme": "low_risk", "tier": "A", "prior_sign": 1}}
        got = self.load(full)
        self.assertEqual((got["themes"], got["tiers"], got["tier_rank"], got["source"]),
                         (["value", "low_risk"], ["B-", "A"], [5, 1], "library"))
        rows = [{"id": i, **v} for i, v in full.items()]
        self.assertEqual(self.load({}, rows)["source"], "recipe")
        self.assertEqual(self.load({}, rows, key="candidates")["tier_rank"], [5, 1])
        mixed = self.load({"a": {"theme": "value"}, "b": full["b"]},
                          [{"id": "a", "tier": "B-", "prior_sign": 1}, {"id": "b", "theme": "low_risk"}])
        self.assertEqual((mixed["themes"], mixed["source"]), (["value", "low_risk"], "library+recipe"))
        ints = self.load({"a": dict(full["a"], tier=2), "b": dict(full["b"], tier=1)})
        self.assertEqual((ints["tier_rank"], ints["tier_order"]), ([2, 1], "integer-ascending"))

    def test_refusals(self):
        full = {"a": {"theme": "value", "tier": "B", "prior_sign": 1},
                "b": {"theme": "low_risk", "tier": "A", "prior_sign": 1}}
        self.refuse("disagrees between sources", full, [{"id": "a", "theme": "low_risk"}])
        self.refuse("lacks ['tier']", {"a": {"theme": "value", "prior_sign": 1}, "b": full["b"]})
        self.refuse("pre-registered v4 theme", {"a": dict(full["a"], theme="profitability"), "b": full["b"]})
        self.refuse("prior_sign -1", {"a": dict(full["a"], prior_sign=-1), "b": full["b"]})
        self.refuse("prior_sign of a", {"a": dict(full["a"], prior_sign=True), "b": full["b"]})
        self.refuse("tier of a", {"a": dict(full["a"], tier="B (cost-bound)"), "b": full["b"]})
        self.refuse("mix grades and integers", {"a": dict(full["a"], tier=1), "b": full["b"]})
        self.refuse("library.sha256 differs", {}, [{"id": i, **v} for i, v in full.items()], recipe_lib_sha="0" * 64)
        self.refuse("unknown id", full, [{"id": "zz", "theme": "value"}])
        self.refuse("no per-candidate list", full, [], key="other")


class PriorEndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel, signals, ids, extra = v4_world()
        cls.ids, cls.extra = ids, extra
        cls.fx = Fixture(cls.root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                         candidate_extra=extra)
        cls.out = cls.root / "v4"
        cls.code, cls.summary = fcw.fit(cls.fx.args(cls.out, **V4_ARGS))
        cls.bytes = {p.name: p.read_bytes() for p in cls.out.iterdir()}
        cls.adm = json.loads(cls.bytes[fcw.OUTPUT_ADMISSION])
        cls.doc = json.loads(cls.bytes[fcw.OUTPUT_WEIGHTS])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_admission_statuses_and_prior_signs(self):
        self.assertEqual(self.code, fcw.EXIT_OK)
        rows = {c["id"]: c for c in self.adm["candidates"]}
        self.assertEqual({i: rows[i]["status"] for i in self.ids},
                         {"slow_a": "reject_redundant", "slow_a_twin": "admitted", "slow_b": "reject_veto",
                          "fast_a": "reject_turnover", "flip": "admitted", "insufficient": "reject_no_prior",
                          "slow_a_clone": "admitted"})
        self.assertEqual(rows["slow_a"]["redundant_with"], "slow_a_clone")  # tier A clone goes first
        self.assertLess(rows["slow_b"]["hac_t"], -2.0)                     # prior contradicted: veto, no flip
        self.assertEqual(rows["slow_b"]["s_k"], 1)
        self.assertEqual(rows["insufficient"]["failed_checks"], ["no_prior", "insufficient"])
        self.assertEqual(rows["fast_a"]["tier"], "A-")
        self.assertEqual(self.adm["admitted"], ["slow_a_clone", "slow_a_twin", "flip"])
        self.assertEqual(self.adm["counts"], {"admitted": 3, "reject_no_prior": 1, "reject_insufficient": 0,
                                              "reject_turnover": 1, "reject_veto": 1, "reject_redundant": 1})
        self.assertEqual(self.adm["rules"]["hac"]["lag"], 5)
        self.assertEqual((self.adm["rules"]["rho_limit"], self.adm["rules"]["tau_limit"], self.adm["rules"]["veto_t"]),
                         (0.9, 0.7, -2.0))
        self.assertEqual(self.adm["inputs"]["prior_metadata_source"], "library")
        lines = self.bytes[fcw.OUTPUT_ADMISSION_CSV].decode().splitlines()
        self.assertEqual(lines[0].split(","), list(fcw.V4_CSV_COLUMNS))
        self.assertEqual(len(lines), 1 + len(self.ids))

    def test_ew_theme_weights_and_pinned_signs(self):
        w = self.doc["weights"]
        # themes present: value (clone, twin) and short_interest (flip) -> 1/2 per theme
        self.assertEqual({i: w[i] for i in self.ids if w[i] > 0},
                         {"slow_a_clone": 0.25, "slow_a_twin": 0.25, "flip": 0.5})
        self.assertEqual(sum(w.values()), 1.0)
        # every prior-signed candidate pins +1 (zero weights included); prior_sign 0 carries none
        self.assertEqual(self.doc["signs"], {i: 1 for i in self.ids if i != "insufficient"})
        got = runner_accepts(self.bytes[fcw.OUTPUT_WEIGHTS], self.fx.library_sha, self.ids, self.fx.train_sha)
        self.assertEqual(got, [w[i] for i in self.ids])
        prov = self.doc["provenance"]
        self.assertEqual((prov["rule"], prov["screen"], prov["orientation"]), ("ew-theme-v1", "v4-prior-v1", "prior"))
        self.assertEqual(prov["themes_present"], ["short_interest", "value"])
        self.assertEqual(prov["themes"]["value"]["admitted"], ["slow_a_clone", "slow_a_twin"])
        self.assertNotIn("lambda", prov)
        self.assertEqual(prov["admission_sha256"], sha(self.bytes[fcw.OUTPUT_ADMISSION]))
        self.assertEqual(prov["script_sha256"], sha(Path(fcw.__file__).read_bytes()))
        rows = {r["id"]: r for r in prov["candidates"]}
        self.assertAlmostEqual(prov["weighted_standalone_turnover"], sum(w[i] * rows[i]["tau"] for i in self.ids),
                               places=14)
        self.assertEqual(rows["slow_b"]["status"], "reject_veto")
        self.assertEqual(rows["flip"]["status"], "fitted")
        self.assertEqual(self.summary["themes_present"], ["short_interest", "value"])
        self.assertEqual(self.bytes[fcw.OUTPUT_WEIGHTS], fcw.canonical_bytes(self.doc))

    def test_recipe_source_gives_the_same_decisions_and_weights(self):
        panel, signals, ids, extra = v4_world()
        fx = Fixture(self.root / "fx_recipe", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids))
        recipe = self.root / "recipe.json"
        rsha = write_recipe(recipe, fx.library_sha, [{"id": i, "citation": "x", **v} for i, v in extra.items()])
        out = self.root / "v4_recipe"
        code, _ = fcw.fit(fx.args(out, **V4_ARGS, recipe=recipe, recipe_sha256=rsha))
        self.assertEqual(code, fcw.EXIT_OK)
        doc = json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes())
        adm = json.loads((out / fcw.OUTPUT_ADMISSION).read_bytes())
        self.assertEqual((doc["weights"], doc["signs"]), (self.doc["weights"], self.doc["signs"]))
        self.assertEqual([c["status"] for c in adm["candidates"]], [c["status"] for c in self.adm["candidates"]])
        self.assertEqual((adm["inputs"]["recipe_sha256"], doc["provenance"]["prior_metadata_source"]), (rsha, "recipe"))

    def test_incremental_reuse_and_determinism(self):
        work = self.root / "work"
        fcw.fit(self.fx.args(self.root / "w1", **V4_ARGS, work_dir=work))
        code, summary = fcw.fit(self.fx.args(self.root / "w2", **V4_ARGS, work_dir=work))
        self.assertEqual((code, summary["computed_this_run"], summary["reused"]), (fcw.EXIT_OK, 0, len(self.ids)))
        for d in ("w1", "w2"):
            for name, data in self.bytes.items():
                self.assertEqual((self.root / d / name).read_bytes(), data, name)

    def test_combination_refusals(self):
        out = self.root / "refused"
        cases = [(dict(screen="v4-prior-v1", orientation="train", composition="ew-theme-v1"), "go together"),
                 (dict(screen="v4-prior-v1", orientation="prior", composition=fcw.RULE_ID), "ew-theme-v1"),
                 (dict(screen="v3-admit-v1", orientation="prior", composition=fcw.RULE_ID), "go together"),
                 (dict(screen="none", orientation="train", composition="ew-theme-v1"), "ew-theme-v1"),
                 (dict(screen="none", orientation="train", composition=fcw.RULE_ID, recipe=self.root / "r",
                       recipe_sha256="0" * 64), "--recipe is read only")]
        for override, fragment in cases:
            with self.assertRaises(fcw.FitError) as caught:
                fcw.fit(self.fx.args(out, **override))
            self.assertIn(fragment, str(caught.exception))
            self.assertFalse(out.exists())

    def test_nothing_admitted_publishes_table_only(self):
        panel, signals, ids, extra = v4_world()
        keep = [ids.index(i) for i in ("slow_b", "fast_a", "insufficient")]
        sub = [ids[k] for k in keep]
        fx = Fixture(self.root / "none_admitted", panel, [signals[k] for k in keep], [1] * 3, ids=sub,
                     families=["fam"] * 3, candidate_extra=extra)
        out = self.root / "none_out"
        self.assertEqual(fcw.main(fx.argv(out, "v4-prior-v1", ["--orientation", "prior", "--composition", "ew-theme-v1"])),
                         fcw.EXIT_NO_WEIGHTS)
        self.assertEqual(sorted(p.name for p in out.iterdir()), sorted([fcw.OUTPUT_ADMISSION, fcw.OUTPUT_ADMISSION_CSV]))


# ------------------------------------------------ T27: v4-prior-v2 (v4.2 R3' cost-consistency screen)
V42_ARGS = dict(screen="v4-prior-v2", orientation="prior", composition="ew-theme-v1")


class CostScreenRules(unittest.TestCase):
    def test_declared_limit_and_status_order(self):
        self.assertEqual(fcw.V42_COST_TAU_LIMIT, 0.08)
        self.assertEqual(fcw.V42_STATUSES, ("admitted", "reject_no_prior", "reject_insufficient", "reject_turnover",
                                            "reject_turnover_cost", "reject_veto", "reject_redundant"))
        self.assertIn("v4-prior-v2", fcw.SCREENS)

    def test_cost_check_thresholds_and_precedence(self):
        rng = np.random.default_rng(3)
        t = 600
        train = np.ones(t, dtype=bool)
        noise = lambda: rng.normal(0, 1, t)  # noqa: E731
        good = [0.2 + noise() for _ in range(4)]
        veto = -0.3 + noise()
        ids = ["at_limit", "over", "slow", "fast", "over_and_veto"]
        taus = [0.08, float(np.nextafter(0.08, 1)), 0.02, 0.71, 0.5]
        factors = np.vstack(good + [veto])
        v2 = fcw.screen_v4(factors, taus, ids, train, [4] * 5, [1] * 5, cost_tau_limit=fcw.V42_COST_TAU_LIMIT)
        by = dict(zip(ids, v2))
        self.assertEqual({i: r["status"] for i, r in by.items()},
                         {"at_limit": "admitted", "over": "reject_turnover_cost", "slow": "admitted",
                          "fast": "reject_turnover", "over_and_veto": "reject_turnover_cost"})
        self.assertEqual(by["fast"]["failed_checks"], ["turnover", "turnover_cost"])       # 0.70 check first
        self.assertEqual(by["over_and_veto"]["failed_checks"], ["turnover_cost", "veto"])  # before the veto
        v1 = fcw.screen_v4(factors, taus, ids, train, [4] * 5, [1] * 5)  # default: v4-prior-v1, no cost check
        self.assertEqual([r["status"] for r in v1],
                         ["admitted", "admitted", "admitted", "reject_turnover", "reject_veto"])
        self.assertEqual(v1, fcw.screen_v4(factors, taus, ids, train, [4] * 5, [1] * 5, cost_tau_limit=None))


class CostScreenEndToEnd(unittest.TestCase):
    """v4-prior-v2 over the v4 world. Every synthetic tau is ~0.25, so the declared 0.08 would reject all;
    the limit is patched between the members' taus to exercise the mechanics (the declared value is tested above)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel, signals, ids, extra = v4_world()
        cls.ids = ids
        cls.fx = Fixture(cls.root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                         candidate_extra=extra)
        with unittest.mock.patch.object(fcw, "V42_COST_TAU_LIMIT", 0.249):
            cls.code, cls.summary = fcw.fit(cls.fx.args(cls.root / "v42", **V42_ARGS))
        cls.adm = json.loads((cls.root / "v42" / fcw.OUTPUT_ADMISSION).read_bytes())
        cls.doc = json.loads((cls.root / "v42" / fcw.OUTPUT_WEIGHTS).read_bytes())
        fcw.fit(cls.fx.args(cls.root / "v41", **V4_ARGS))
        cls.adm_v1 = json.loads((cls.root / "v41" / fcw.OUTPUT_ADMISSION).read_bytes())
        cls.doc_v1 = json.loads((cls.root / "v41" / fcw.OUTPUT_WEIGHTS).read_bytes())

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_statuses_counts_and_rules(self):
        self.assertEqual(self.code, fcw.EXIT_OK)
        rows = {c["id"]: c for c in self.adm["candidates"]}
        taus = {i: rows[i]["tau"] for i in self.ids}
        self.assertLess(taus["slow_a_twin"], 0.249)
        self.assertLess(taus["slow_a"], 0.249)
        self.assertGreater(taus["flip"], 0.249)
        self.assertGreater(taus["slow_a_clone"], 0.249)
        self.assertEqual({i: rows[i]["status"] for i in self.ids},
                         {"slow_a": "admitted", "slow_a_twin": "admitted", "slow_b": "reject_veto",
                          "fast_a": "reject_turnover", "flip": "reject_turnover_cost",
                          "insufficient": "reject_no_prior", "slow_a_clone": "reject_turnover_cost"})
        self.assertEqual(rows["fast_a"]["failed_checks"], ["turnover", "turnover_cost"])
        self.assertEqual({i: rows[i]["tau_over_cost_limit"] for i in self.ids},
                         {i: taus[i] > 0.249 for i in self.ids})
        self.assertEqual(self.adm["screen"], "v4-prior-v2")
        self.assertEqual(self.adm["counts"], {"admitted": 2, "reject_no_prior": 1, "reject_insufficient": 0,
                                              "reject_turnover": 1, "reject_turnover_cost": 2, "reject_veto": 1,
                                              "reject_redundant": 0})
        self.assertEqual(self.adm["rules"]["cost_tau_limit"], 0.249)
        self.assertEqual(self.adm["rules"]["status_precedence"], list(fcw.V42_STATUSES[1:]))
        # the redundancy pass only sees cost survivors: slow_a is no longer redundant with the rejected clone
        v1_rows = {c["id"]: c for c in self.adm_v1["candidates"]}
        self.assertEqual((v1_rows["slow_a"]["status"], v1_rows["slow_a"]["redundant_with"]),
                         ("reject_redundant", "slow_a_clone"))

    def test_empty_theme_drops_out_of_the_weights(self):
        w = self.doc["weights"]
        self.assertEqual({i: w[i] for i in self.ids if w[i] > 0}, {"slow_a": 0.5, "slow_a_twin": 0.5})
        prov = self.doc["provenance"]
        self.assertEqual(prov["themes_present"], ["value"])  # short_interest lost flip: 1/themes renormalises
        self.assertEqual((prov["screen"], prov["cost_tau_limit"], prov["cost_rejected"]),
                         ("v4-prior-v2", 0.249, ["flip", "slow_a_clone"]))
        self.assertTrue(prov["signs"].startswith("v4-prior-v2: "))
        self.assertEqual(self.doc["signs"], {i: 1 for i in self.ids if i != "insufficient"})
        got = runner_accepts((self.root / "v42" / fcw.OUTPUT_WEIGHTS).read_bytes(), self.fx.library_sha, self.ids,
                             self.fx.train_sha)
        self.assertEqual(got, [w[i] for i in self.ids])

    def test_v1_output_carries_no_cost_keys(self):
        self.assertNotIn("cost_tau_limit", self.adm_v1["rules"])
        self.assertNotIn("cost_tau_limit", self.doc_v1["provenance"])
        self.assertTrue(all("tau_over_cost_limit" not in c for c in self.adm_v1["candidates"]))
        self.assertEqual(self.adm_v1["screen"], "v4-prior-v1")

    def test_combination_refusals(self):
        out = self.root / "refused"
        for override in (dict(V42_ARGS, orientation="train"), dict(V42_ARGS, composition=fcw.RULE_ID)):
            with self.assertRaises(fcw.FitError) as caught:
                fcw.fit(self.fx.args(out, **override))
            self.assertIn("go together", str(caught.exception))
            self.assertFalse(out.exists())


@contextlib.contextmanager
def superseded_window(module):
    """Bind this fitter to research-seal-v1, the window the git-history fitters hard-code (TRAIN end 2023: the
    admission windows and the aim semantics). W0-1 made the window a read of research_window.py and changed nothing
    else, so the byte-identity tests below compare the two fitters on the same window. Other modules run as they are."""
    if module is not fcw:
        yield
        return
    old = fcw.rw.superseded()
    here = f"[{fcw.rw.TRAIN_BEGIN_DATE},{fcw.rw.TRAIN_END_DATE})"
    assert here in fcw.AIM_SEMANTICS
    semantics = fcw.AIM_SEMANTICS.replace(here, f"[{old['TRAIN_BEGIN_DATE']},{old['TRAIN_END_DATE']})")
    with contextlib.ExitStack() as stack:
        for name, value in (("FIT_BEGIN_NS", old["TRAIN_BEGIN_NS"]), ("TRAIN_END_NS", old["TRAIN_END_NS"]),
                            ("AIM_SEMANTICS", semantics),
                            ("AIM_TAG", hashlib.sha256(semantics.encode()).hexdigest()[:16])):
            stack.enter_context(unittest.mock.patch.object(fcw, name, value))
        yield


class PriorV1BytesUnchanged(unittest.TestCase):
    """T27 edit: v4-prior-v1 emits the pre-T27 bytes except the embedded script SHA (git-history fitter)."""

    PRE_T27_BLOB = "8d201df65d5ce255c82ed070832b4efd7e487093"  # fitter at c5058edf (T23 v4-prior-v1)

    def test_v4_prior_v1_outputs_and_cache_keys_match_pre_t27_fitter(self):
        import importlib.util
        import subprocess
        try:
            old = subprocess.run(["git", "cat-file", "-p", self.PRE_T27_BLOB], capture_output=True, check=True,
                                 cwd=Path(__file__).resolve().parent).stdout
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("git history with the pre-T27 fitter blob is unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "old").mkdir()
            (root / "old" / "fit_composition_weights_pre_t27.py").write_bytes(old)
            spec = importlib.util.spec_from_file_location("fcw_pre_t27", root / "old" / "fit_composition_weights_pre_t27.py")
            pre = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(pre)
            panel, signals, ids, extra = v4_world()
            fx = Fixture(root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                         candidate_extra=extra)
            got = {}
            for tag, module in (("new", fcw), ("old", pre)):
                out, work = root / f"v4-{tag}", root / f"work-{tag}"
                with superseded_window(module):
                    code, _ = module.fit(module.parse_args(fx.argv(out, "v4-prior-v1", [
                        "--orientation", "prior", "--composition", "ew-theme-v1", "--work-dir", str(work)])))
                self.assertEqual(code, 0)
                adm = (out / fcw.OUTPUT_ADMISSION).read_bytes()
                derived = {module.SCRIPT_SHA256: b"<script>", json.loads(adm)["inputs"]["context_sha256"]: b"<context>",
                           sha(adm): b"<admission>"}
                files = {}
                for p in out.iterdir():
                    data = p.read_bytes()
                    for value, token in derived.items():
                        data = data.replace(value.encode(), token)
                    files[p.name] = data
                keys = sorted(str(p.relative_to(work)) for p in work.rglob("*") if p.is_file())
                got[tag] = (files, keys)
            self.assertEqual(got["new"][0], got["old"][0])
            self.assertEqual(work_shape(got["new"][1], new=True), work_shape(got["old"][1]))  # W3: keys renamed


class DefaultBytesUnchanged(unittest.TestCase):
    """T23 edit: default paths emit the pre-T23 bytes except the embedded script SHA (git-history fitter)."""

    PRE_T23_BLOB = "fd227a861deb75d031890f4bdc4175b60d05a23a"  # fitter at d38e7929 (frozen v3 script)

    def test_default_outputs_and_cache_keys_match_pre_t23_fitter(self):
        import importlib.util
        import subprocess
        try:
            old = subprocess.run(["git", "cat-file", "-p", self.PRE_T23_BLOB], capture_output=True, check=True,
                                 cwd=Path(__file__).resolve().parent).stdout
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("git history with the pre-T23 fitter blob is unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "old").mkdir()
            (root / "old" / "fit_composition_weights_pre_t23.py").write_bytes(old)
            spec = importlib.util.spec_from_file_location("fcw_pre_t23", root / "old" / "fit_composition_weights_pre_t23.py")
            pre = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(pre)
            self.assertEqual(pre.SEMANTICS_TAG, fcw.SEMANTICS_TAG)
            panel, signals, ids = screen_world()
            fx = Fixture(root / "fx", panel, signals, SCREEN_RUNNER_SIGNS, ids=ids, families=["fam"] * len(ids),
                         field_ids={"slow_b"})
            for name, screen, extra in (("none", "none", []), ("v3", "v3-admit-v1", []),
                                        ("net", "v3-admit-v1", ["--composition", fcw.NETCOST_RULE_ID])):
                got = {}
                for tag, module in (("new", fcw), ("old", pre)):
                    out, work = root / f"{name}-{tag}", root / f"work-{name}-{tag}"
                    with superseded_window(module):
                        code, _ = module.fit(module.parse_args(fx.argv(out, screen, [*extra, "--work-dir", str(work)])))
                    self.assertEqual(code, 0)
                    doc = json.loads((out / (fcw.OUTPUT_WEIGHTS if screen == "none" else fcw.OUTPUT_ADMISSION)).read_bytes())
                    context = (doc.get("provenance") or doc["inputs"])["context_sha256"]  # digest binds the script SHA
                    derived = {module.SCRIPT_SHA256: b"<script>", context: b"<context>"}
                    if (out / fcw.OUTPUT_ADMISSION).exists():  # the weights file pins the admission bytes' SHA
                        derived[sha((out / fcw.OUTPUT_ADMISSION).read_bytes())] = b"<admission>"
                    files = {}
                    for p in out.iterdir():
                        data = p.read_bytes()
                        for value, token in derived.items():
                            data = data.replace(value.encode(), token)
                        files[p.name] = data
                    keys = sorted(str(p.relative_to(work)) for p in work.rglob("*") if p.is_file())
                    got[tag] = (files, keys)
                self.assertEqual(got["new"][0], got["old"][0], name)
                self.assertEqual(work_shape(got["new"][1], new=True), work_shape(got["old"][1]), name)  # W3 keys


# ------------------------------------------------ T31: ew-theme-aim-v1 (v5 R4' aim gains)
AIM_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-aim-v1")


def ref_standardized_ranks(values, live, min_names=50):
    """Independent port: average (1-based) ranks per row as scipy.stats.rankdata, standardized with population SD."""
    out = np.full(values.shape, NAN)
    for t in range(values.shape[0]):
        cols = [i for i in range(values.shape[1]) if live[t, i] and math.isfinite(values[t, i])]
        if len(cols) < min_names:
            continue
        v = sorted((float(values[t, i]), i) for i in cols)
        rank = {}
        b = 0
        while b < len(v):
            e = b
            while e + 1 < len(v) and v[e + 1][0] == v[b][0]:
                e += 1
            for k in range(b, e + 1):
                rank[v[k][1]] = (b + e) / 2.0 + 1.0
            b = e + 1
        r = np.array([rank[i] for i in cols])
        sd = r.std()
        if sd > 0:
            out[t, cols] = (r - r.mean()) / sd
    return out


def ref_rank_autocorrelation(z, lags, min_names=50):
    """Loop port of R4': mean over days of sum(z_d * z_d-j) / n_both over names finite on both days."""
    out = []
    for j in lags:
        per_day = []
        for d in range(j, z.shape[0]):
            both = [i for i in range(z.shape[1]) if math.isfinite(z[d, i]) and math.isfinite(z[d - j, i])]
            if len(both) >= min_names:
                per_day.append(sum(z[d, i] * z[d - j, i] for i in both) / len(both))
        out.append(sum(per_day) / len(per_day) if per_day else NAN)
    return np.array(out)


class AimGainRules(unittest.TestCase):
    def test_declared_constants(self):
        self.assertEqual(fcw.AIM_RULE_ID, "ew-theme-aim-v1")
        self.assertEqual((fcw.AIM_THETA, fcw.AIM_GAIN_MIN, fcw.AIM_MAX_LAG, fcw.AIM_MIN_NAMES), (0.05, 0.05, 126, 50))
        self.assertEqual(fcw.AIM_LAGS, list(range(22)) + [28, 35, 42, 49, 56, 63, 70, 77, 84, 91, 98, 105, 112, 119, 126])
        self.assertEqual(fcw.PRIOR_COMPOSITIONS, ("ew-theme-v1", "ew-theme-aim-v1", "ew-theme-v6"))  # + v6 V6-W
        self.assertIn("ew-theme-aim-v1", fcw.COMPOSITIONS)

    def test_aim_gain_ar1_matches_closed_form(self):
        phi, theta = 0.02, 0.05
        closed = theta * sum((1 - theta) ** j * (1 - phi) ** j for j in range(fcw.AIM_MAX_LAG + 1))
        # every lag exact: the truncated GP sum to rounding
        exact = list(range(fcw.AIM_MAX_LAG + 1))
        self.assertAlmostEqual(fcw.aim_gain(np.array([(1 - phi) ** j for j in exact]), exact, theta), closed, places=12)
        # the declared lag grid: exact at the grid, linear in between. (1-phi)^j is convex, so the chords lie above
        # the curve and g over-states the exact sum by a little (the brief's 1e-9 equality cannot hold here).
        lags = fcw.AIM_LAGS
        g = fcw.aim_gain(np.array([(1 - phi) ** j for j in lags]), lags, theta)
        self.assertGreater(g, closed)
        self.assertLess(g - closed, 1e-3)
        # and the untruncated AR(1) closed form theta / (theta + phi (1 - theta)) (plan-s4 4.A) within the tail
        self.assertLess(abs(g - theta / (theta + phi * (1 - theta))), 1e-3)
        # GP calibration sanity (half-lives 2.4 / 206 days at theta = .05): a fast signal is cut, a slow one kept
        for half_life, want in ((2.4, 0.05 / (0.05 + 0.95 * (1 - 0.5 ** (1 / 2.4)))), (206.0, 0.94)):
            p = 1 - 0.5 ** (1 / half_life)
            got = fcw.aim_gain(np.array([(1 - p) ** j for j in lags]), lags)
            self.assertLess(abs(got - want), 3e-3, half_life)
        # a perfectly persistent rank: 1 - (1-theta)^127 (< 1, no clip); no autocorrelation clips at the floor
        self.assertAlmostEqual(fcw.aim_gain(np.ones(len(lags)), lags), 1 - 0.95 ** 127, places=14)
        self.assertEqual(fcw.aim_gain(np.zeros(len(lags)), lags), fcw.AIM_GAIN_MIN)
        self.assertLess(fcw.aim_gain(-np.ones(len(lags)), lags, clip=False), 0)
        self.assertEqual(fcw.aim_gain(-np.ones(len(lags)), lags), fcw.AIM_GAIN_MIN)

    def test_aim_gain_degenerate_and_gappy(self):
        lags = fcw.AIM_LAGS
        z = np.full((300, 200), np.nan)
        z[:150] = fcw.standardized_ranks(np.random.default_rng(0).normal(size=(150, 200)), np.ones((150, 200), bool))
        rho = fcw.rank_autocorrelation(z, lags)
        self.assertTrue(np.isfinite(rho[0]) and abs(rho[0] - 1) < 1e-9)
        self.assertTrue(np.isfinite(rho).all())  # every declared lag (<= 126) overlaps the 150 ranked days
        g = fcw.aim_gain(rho, lags)
        self.assertTrue(fcw.AIM_GAIN_MIN <= g <= 1.0)
        # only 100 ranked days: lags >= 100 have no pair -> NaN -> counted 0; g still finite and in range
        short = fcw.rank_autocorrelation(z[50:], lags)
        self.assertEqual([j for j, r in zip(lags, short) if np.isnan(r)], [j for j in lags if j >= 100])
        self.assertTrue(fcw.AIM_GAIN_MIN <= fcw.aim_gain(short, lags) <= 1.0)
        # rankdata ties -> std 0 -> NaN -> degenerate (a constant candidate has no ranks at all)
        const = np.ones((300, 200))
        zc = fcw.standardized_ranks(const, np.ones((300, 200), bool))
        self.assertTrue(np.isnan(zc).all())
        rho_c = fcw.rank_autocorrelation(zc, lags)
        self.assertTrue(np.isnan(rho_c).all())
        self.assertEqual(fcw.aim_gain(rho_c, lags), fcw.AIM_GAIN_MIN)  # NaN lags -> 0 -> floor, never NaN
        # flat for a stretch (all tied), then a persistent AR signal with an all-NaN hole: finite rho, g in range
        rng = np.random.default_rng(5)
        x = np.empty((400, 120))
        x[0] = rng.normal(size=120)
        for t in range(1, 400):
            x[t] = 0.99 * x[t - 1] + math.sqrt(1 - 0.99 ** 2) * rng.normal(size=120)
        x[:80] = 3.0              # flat stretch: every name tied
        x[200:230] = np.nan       # all-NaN stretch
        zs = fcw.standardized_ranks(x, np.ones(x.shape, bool))
        self.assertTrue(np.isnan(zs[:80]).all() and np.isnan(zs[200:230]).all())
        rho_s = fcw.rank_autocorrelation(zs, lags)
        self.assertTrue(np.isfinite(rho_s).all())
        g_s = fcw.aim_gain(rho_s, lags)
        self.assertTrue(fcw.AIM_GAIN_MIN <= g_s <= 1.0)
        self.assertGreater(g_s, 0.5)  # AR(.99) ranks: GP gain ~ .05 / (.05 + .01 * .95) ~ .84, sampling below

    def test_standardized_ranks_match_rankdata_port(self):
        rng = np.random.default_rng(9)
        values = np.round(rng.normal(0, 2, (30, 70)))  # heavy ties
        values[rng.random(values.shape) < 0.15] = NAN
        live = rng.random(values.shape) < 0.95
        live[3, :] = False
        live[3, :49] = True       # 49 live cells at most: below min_names -> NaN row
        values[7] = 1.0           # all tied -> NaN row
        got = fcw.standardized_ranks(values, live)
        want = ref_standardized_ranks(values, live)
        np.testing.assert_array_equal(np.isnan(got), np.isnan(want))
        np.testing.assert_allclose(got, want, rtol=0, atol=1e-12, equal_nan=True)
        self.assertTrue(np.isnan(got[3]).all() and np.isnan(got[7]).all())
        ok = np.isfinite(got).any(axis=1)
        np.testing.assert_allclose(np.nanmean(got[ok], axis=1), 0.0, atol=1e-13)
        np.testing.assert_allclose(np.nanmean(got[ok] ** 2, axis=1), 1.0, atol=1e-12)

    def test_rank_autocorrelation_matches_loop_port(self):
        rng = np.random.default_rng(4)
        z = rng.normal(size=(60, 80))
        z[rng.random(z.shape) < 0.2] = NAN
        z[10] = NAN
        lags = [0, 1, 2, 5, 21, 28, 59, 60, 70]
        got = fcw.rank_autocorrelation(z, lags)
        want = ref_rank_autocorrelation(z, lags)
        np.testing.assert_array_equal(np.isnan(got), np.isnan(want))
        np.testing.assert_allclose(got, want, rtol=0, atol=1e-13, equal_nan=True)
        self.assertTrue(np.isnan(got[-2:]).all())  # lag >= days: no pair
        # min_names applies to names finite on BOTH days
        self.assertTrue(np.isnan(fcw.rank_autocorrelation(z, [1], min_names=80)).all())

    def test_half_sample_profile_split(self):
        rng = np.random.default_rng(6)
        z = np.full((100, 60), NAN)
        z[:50] = fcw.standardized_ranks(np.cumsum(rng.normal(size=(50, 60)), axis=0), np.ones((50, 60), bool))
        z[50:] = fcw.standardized_ranks(rng.normal(size=(50, 60)), np.ones((50, 60), bool))  # no persistence
        prof = fcw.aim_profile(z, np.ones(100, bool), [0, 1, 2])
        self.assertEqual(prof["half_split_decision"], 50)
        first, second = prof["rho_half"]
        np.testing.assert_allclose(first, ref_rank_autocorrelation(z[:50], [0, 1, 2]), atol=1e-13)
        np.testing.assert_allclose(second, ref_rank_autocorrelation(z[50:], [0, 1, 2]), atol=1e-13)
        self.assertGreater(first[1], 0.8)
        self.assertLess(abs(second[1]), 0.2)
        self.assertGreater(prof["gain_half"][0], prof["gain_half"][1])
        self.assertEqual(prof["rank_decisions"], 100)

    def test_aim_weights_global_normalization(self):
        w, t = fcw.ew_theme_aim_weights(["a", "a", "b"], [1.0, 1.0, 0.25])
        self.assertTrue(abs(w.sum() - 1) < 1e-12 and t["a"]["aim_theme_weight"] > t["b"]["aim_theme_weight"])
        # raw = [1/4, 1/4, 0.25/2] -> normalised globally, not within the theme
        np.testing.assert_allclose(w, [0.4, 0.4, 0.2], rtol=0, atol=1e-15)
        self.assertEqual(t["b"], {"admitted_count": 1, "nominal_theme_weight": 0.5, "aim_theme_weight": w[2]})
        # equal gains reproduce ew-theme-v1
        themes = ["value", "value", "low_risk", "value", "short_interest"]
        ew, _ = fcw.ew_theme_weights(themes)
        aim, _ = fcw.ew_theme_aim_weights(themes, [0.7] * 5)
        np.testing.assert_allclose(aim, ew, rtol=0, atol=1e-15)
        with self.assertRaises(fcw.FitError):
            fcw.ew_theme_aim_weights(["a"], [float("nan")])


def aim_world():
    """v4_world plus a medium-speed AR(.85) member on half the names and a constant candidate."""
    panel, signals, ids, extra = v4_world()
    rng = np.random.default_rng(31)
    dates, names = panel["close"].shape
    x = np.empty((dates, names))
    x[0] = rng.normal(size=names)
    for t in range(1, dates):
        x[t] = 0.85 * x[t - 1] + math.sqrt(1 - 0.85 ** 2) * rng.normal(size=names)
    live = panel["member"] == 1
    half = np.where(live, x, NAN)
    half[:, 1::2] = NAN                       # finite on 30 of 60 names: coverage ~ 1/2 (and < 50 names ranked)
    medium = np.where(live, x, NAN)           # the full-coverage version is the admitted member
    const = np.where(live, 1.0, NAN)
    signals = signals + [medium, half, const]
    ids = ids + ["medium", "medium_half", "const"]
    extra = dict(extra, medium={"theme": "reversal_seasonality", "tier": "B", "prior_sign": 1},
                 medium_half={"theme": "reversal_seasonality", "tier": "C", "prior_sign": 1},
                 const={"theme": "low_risk", "tier": "B", "prior_sign": 1})
    return panel, signals, ids, extra


class AimEndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel, signals, ids, extra = aim_world()
        cls.ids = ids
        cls.fx = Fixture(cls.root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                         candidate_extra=extra)
        cls.code, cls.summary = fcw.fit(cls.fx.args(cls.root / "aim", **AIM_ARGS))
        cls.bytes = {p.name: p.read_bytes() for p in (cls.root / "aim").iterdir()}
        cls.doc = json.loads(cls.bytes[fcw.OUTPUT_WEIGHTS])
        cls.v1_code, _ = fcw.fit(cls.fx.args(cls.root / "v1", **V4_ARGS))
        cls.v1_bytes = {p.name: p.read_bytes() for p in (cls.root / "v1").iterdir()}
        cls.v1 = json.loads(cls.v1_bytes[fcw.OUTPUT_WEIGHTS])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_members_weights_and_gains(self):
        self.assertEqual((self.code, self.v1_code), (fcw.EXIT_OK, fcw.EXIT_OK))
        prov, w = self.doc["provenance"], self.doc["weights"]
        self.assertEqual(prov["rule"], "ew-theme-aim-v1")
        self.assertEqual(self.summary["composition"], "ew-theme-aim-v1")
        # the screen never sees the composition: identical admission bytes, identical member set
        self.assertEqual(self.bytes[fcw.OUTPUT_ADMISSION], self.v1_bytes[fcw.OUTPUT_ADMISSION])
        members = sorted(i for i in self.ids if w[i] > 0)
        self.assertEqual(members, sorted(i for i in self.ids if self.v1["weights"][i] > 0))
        self.assertEqual(members, sorted(prov["aim"]["members"]))
        self.assertIn("medium", members)
        self.assertEqual(self.doc["signs"], self.v1["signs"])
        # w_k = (g_k / (T n_theme)) / sum over members, T and n_theme exactly as ew-theme-v1
        gains = prov["aim"]["gain"]
        themes = {r["id"]: r["theme"] for r in prov["candidates"]}
        n_theme = {t: sum(1 for i in members if themes[i] == t) for t in set(themes[i] for i in members)}
        raw = {i: gains[i] / (len(n_theme) * n_theme[themes[i]]) for i in members}
        for i in members:
            self.assertAlmostEqual(w[i], raw[i] / sum(raw.values()), places=15)
        self.assertAlmostEqual(sum(w.values()), 1.0, places=15)
        self.assertTrue(all(fcw.AIM_GAIN_MIN <= g <= 1.0 for g in gains.values()))
        # slow AR(.97) members keep more aim than the AR(.85) member: the GP ordering
        self.assertGreater(min(gains["slow_a_clone"], gains["slow_a_twin"]), gains["medium"] + 0.2)
        self.assertLess(gains["medium"], 0.4)
        for theme, entry in prov["themes"].items():
            self.assertEqual(set(entry), {"admitted_count", "nominal_theme_weight", "aim_theme_weight", "admitted"})
            self.assertAlmostEqual(entry["aim_theme_weight"], sum(w[i] for i in entry["admitted"]), places=15)
        self.assertAlmostEqual(prov["weighted_standalone_turnover"],
                               sum(w[i] * r["tau"] for i, r in ((r["id"], r) for r in prov["candidates"])), places=14)
        got = runner_accepts(self.bytes[fcw.OUTPUT_WEIGHTS], self.fx.library_sha, self.ids, self.fx.train_sha)
        self.assertEqual(got, [w[i] for i in self.ids])
        self.assertEqual(self.bytes[fcw.OUTPUT_WEIGHTS], fcw.canonical_bytes(self.doc))

    def test_aim_block_report_only_fields(self):
        aim = self.doc["provenance"]["aim"]
        self.assertEqual((aim["theta"], aim["lags"], aim["max_lag"]), (0.05, fcw.AIM_LAGS, 126))
        self.assertEqual(set(aim["rho"]), set(self.ids))
        self.assertTrue(all(len(r) == len(fcw.AIM_LAGS) for r in aim["rho"].values()))
        self.assertAlmostEqual(aim["rho"]["slow_a_clone"][0], 1.0, places=12)
        # constant candidate: no ranks, rho all undefined, gain at the floor; the screen rejects it (no live day)
        self.assertEqual(aim["rho"]["const"], [None] * len(fcw.AIM_LAGS))
        self.assertEqual(aim["gain"]["const"], fcw.AIM_GAIN_MIN)
        rows = {r["id"]: r for r in self.doc["provenance"]["candidates"]}
        self.assertEqual((rows["const"]["status"], self.doc["weights"]["const"]), ("reject_insufficient", 0.0))
        self.assertEqual(rows["medium"]["aim_gain"], aim["gain"]["medium"])
        # half-sample gains: two finite values in [floor, 1] per candidate
        for pair in aim["gain_half"].values():
            self.assertEqual(len(pair), 2)
            self.assertTrue(all(fcw.AIM_GAIN_MIN <= g <= 1.0 for g in pair))
        # coverage: full-coverage members 1; the half-name candidate 1/2 and too few names (30 < 50) to rank.
        # It is still an admitted member: R4' counts its undefined rho as 0, so it takes the floor gain (not weight 0).
        w = self.doc["weights"]
        self.assertAlmostEqual(aim["coverage_mean"]["slow_a_clone"], 1.0, places=15)
        self.assertAlmostEqual(aim["coverage_mean"]["medium_half"], 0.5, places=15)
        self.assertEqual(aim["rho"]["medium_half"], [None] * len(fcw.AIM_LAGS))
        self.assertEqual((rows["medium_half"]["status"], aim["gain"]["medium_half"]), ("fitted", fcw.AIM_GAIN_MIN))
        self.assertGreater(w["medium_half"], 0.0)
        eff = aim["coverage_effective_theme_weight"]
        themes = self.doc["provenance"]["themes"]
        self.assertEqual(sorted(eff), self.doc["provenance"]["themes_present"])
        self.assertAlmostEqual(sum(eff.values()), 1.0, places=12)
        # coverage is constant per candidate here, so the per-decision ratio is the ratio of the means
        cov = {i: aim["coverage_mean"][i] for i in aim["members"]}
        total = sum(w[i] * cov[i] for i in cov)
        for theme, x in eff.items():
            self.assertAlmostEqual(x, sum(w[i] * cov[i] for i in themes[theme]["admitted"]) / total, places=12)
        self.assertLess(eff["reversal_seasonality"], themes["reversal_seasonality"]["aim_theme_weight"])
        self.assertGreater(eff["value"], themes["value"]["aim_theme_weight"])

    def test_coverage_effective_weight_formula(self):
        ids, themes = ["a", "b", "c"], ["x", "x", "y"]
        aims = [{"coverage": [1.0, 1.0, 0.0, 1.0]}, {"coverage": [0.5, 1.0, 0.0, 0.0]}, {"coverage": [1.0, 0.0, 0.0, 1.0]}]
        for a in aims:
            a.update(rho=[None], gain=0.5, gain_unclipped=0.5, gain_half=[0.5, 0.5], half_split_decision=2)
        sessions = fcw.FIT_BEGIN_NS + DAY * np.arange(4)
        sessions[3] = fcw.TRAIN_END_NS  # outside TRAIN: not in the mean
        got = fcw.aim_provenance(ids, themes, aims, [0, 1, 2], np.array([0.25, 0.25, 0.5]), sessions)
        eff = got["coverage_effective_theme_weight"]
        # d0: x .375 / (.375+.5); d1: x .5 / .5; d2: no weight live (skipped); d3: outside TRAIN
        self.assertAlmostEqual(eff["x"], (0.375 / 0.875 + 1.0) / 2, places=15)
        self.assertAlmostEqual(eff["y"], (0.5 / 0.875 + 0.0) / 2, places=15)

    def test_ew_theme_v1_has_no_aim_keys(self):
        prov = self.v1["provenance"]
        self.assertNotIn("aim", prov)
        self.assertTrue(all("aim_gain" not in r for r in prov["candidates"]))
        self.assertEqual(prov["rule"], "ew-theme-v1")
        self.assertTrue(all(set(e) == {"admitted_count", "theme_weight", "member_weight", "admitted"}
                            for e in prov["themes"].values()))

    def test_incremental_resume_and_shared_factor_records(self):
        work = self.root / "work"
        stopped = self.root / "never"
        argv = self.fx.argv(stopped, "v4-prior-v1", ["--orientation", "prior", "--composition", "ew-theme-aim-v1",
                                                     "--work-dir", str(work), "--max-new-candidates", "3"])
        self.assertEqual(fcw.main(argv), fcw.EXIT_INCOMPLETE)
        self.assertFalse(stopped.exists())
        store = store_base(work, self.fx.train_sha)
        self.assertEqual(len(list((store / "aim").glob("*.json"))), 3)
        self.assertEqual(len(list((store / "factor").glob("*.json"))), 3)
        with self.assertRaises(fcw.Incomplete) as caught:  # budget spent: partial marker, nothing published
            fcw.fit(self.fx.args(stopped, **AIM_ARGS, work_dir=work, max_seconds=1e-9))
        self.assertEqual((caught.exception.summary["partial"], caught.exception.summary["reused"]), (True, 3))
        code, summary = fcw.fit(self.fx.args(self.root / "resumed", **AIM_ARGS, work_dir=work))
        self.assertEqual((code, summary["computed_this_run"], summary["reused"]),
                         (fcw.EXIT_OK, len(self.ids) - 3, 3))
        for name, data in self.bytes.items():
            self.assertEqual((self.root / "resumed" / name).read_bytes(), data, name)
        # the ew-theme-v1 re-fit reuses every factor record and never reads or writes aim records
        code, summary = fcw.fit(self.fx.args(self.root / "v1_cached", **V4_ARGS, work_dir=work))
        self.assertEqual((code, summary["computed_this_run"]), (fcw.EXIT_OK, 0))
        for name, data in self.v1_bytes.items():
            self.assertEqual((self.root / "v1_cached" / name).read_bytes(), data, name)
        # a tampered aim record fails its SHA check and only that candidate is recomputed
        record = sorted((store / "aim").glob("*.json"))[0]
        j = json.loads(record.read_bytes())
        j["body"]["gain"] = 0.999
        record.write_bytes(json.dumps(j).encode())
        code, summary = fcw.fit(self.fx.args(self.root / "retampered", **AIM_ARGS, work_dir=work))
        self.assertEqual((code, summary["computed_this_run"]), (fcw.EXIT_OK, 1))
        self.assertEqual((self.root / "retampered" / fcw.OUTPUT_WEIGHTS).read_bytes(), self.bytes[fcw.OUTPUT_WEIGHTS])
        # a v1-only work dir never grows an aim directory
        v1_work = self.root / "v1_work"
        fcw.fit(self.fx.args(self.root / "v1_fresh", **V4_ARGS, work_dir=v1_work))
        self.assertEqual([p.name for p in store_base(v1_work, self.fx.train_sha).iterdir()
                          if p.name.startswith("aim")], [])

    def test_combination_refusals(self):
        out = self.root / "refused"
        for override in (dict(screen="none", orientation="train", composition="ew-theme-aim-v1"),
                         dict(screen="v3-admit-v1", orientation="train", composition="ew-theme-aim-v1"),
                         dict(screen="v4-prior-v1", orientation="train", composition="ew-theme-aim-v1")):
            with self.assertRaises(fcw.FitError) as caught:
                fcw.fit(self.fx.args(out, **override))
            self.assertIn("go together", str(caught.exception))
            self.assertFalse(out.exists())
        code, _ = fcw.fit(self.fx.args(self.root / "v42_aim", screen="v4-prior-v2", orientation="prior",
                                       composition="ew-theme-aim-v1"))
        self.assertIn(code, (fcw.EXIT_OK, fcw.EXIT_NO_WEIGHTS))  # the v4.2 screen composes with the aim rule too


class V1BytesUnchangedByAim(unittest.TestCase):
    """T31 edit: ew-theme-v1 (v4-prior-v1 and v4-prior-v2) emits the pre-T31 bytes except the embedded script SHA and
    the SHAs derived from it (context digest, admission SHA), and uses the same work-cache paths (git-history fitter).

    A literal SHA equality with a weights file written by an older fitter is impossible by construction: the file
    embeds the fitter's own SHA-256. This is the byte_stability contract the existing T23/T27 tests use."""

    PRE_T31_BLOB = "fd644cba31e0cd3c3aefb24e91e82e3980127725"  # fitter at d4ec515d (T27, a67dfa9f)

    def test_ew_theme_v1_bytes_unchanged(self):
        import importlib.util
        import subprocess
        try:
            old = subprocess.run(["git", "cat-file", "-p", self.PRE_T31_BLOB], capture_output=True, check=True,
                                 cwd=Path(__file__).resolve().parent).stdout
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("git history with the pre-T31 fitter blob is unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "old").mkdir()
            (root / "old" / "fit_composition_weights_pre_t31.py").write_bytes(old)
            spec = importlib.util.spec_from_file_location("fcw_pre_t31", root / "old" / "fit_composition_weights_pre_t31.py")
            pre = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(pre)
            self.assertEqual(pre.SEMANTICS_TAG, fcw.SEMANTICS_TAG)
            panel, signals, ids, extra = aim_world()
            fx = Fixture(root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                         candidate_extra=extra)
            for screen in ("v4-prior-v1", "v4-prior-v2"):
                got = {}
                for tag, module in (("new", fcw), ("old", pre)):
                    out, work = root / f"{screen}-{tag}", root / f"work-{screen}-{tag}"
                    with unittest.mock.patch.object(module, "V42_COST_TAU_LIMIT", 0.249), superseded_window(module):
                        code, _ = module.fit(module.parse_args(fx.argv(out, screen, [
                            "--orientation", "prior", "--composition", "ew-theme-v1", "--work-dir", str(work)])))
                    self.assertEqual(code, 0, screen)
                    adm = (out / fcw.OUTPUT_ADMISSION).read_bytes()
                    derived = {module.SCRIPT_SHA256: b"<script>", json.loads(adm)["inputs"]["context_sha256"]: b"<context>",
                               sha(adm): b"<admission>"}
                    files = {}
                    for p in out.iterdir():
                        data = p.read_bytes()
                        for value, token in derived.items():
                            data = data.replace(value.encode(), token)
                        files[p.name] = data
                    keys = sorted(str(p.relative_to(work)) for p in work.rglob("*") if p.is_file())
                    got[tag] = (files, keys)
                self.assertEqual(sorted(got["new"][0]), sorted(got["old"][0]), screen)
                for name in got["new"][0]:
                    self.assertEqual(got["new"][0][name], got["old"][0][name], f"{screen} {name}")
                self.assertEqual(work_shape(got["new"][1], new=True), work_shape(got["old"][1]), screen)  # no aim-*


# ------------------------------------------------ V6-W: ew-theme-v6 (v6 revision; within-theme redistribution)
V6_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-v6")


def ref_v6_weights(ids, themes, taus, fast_tau=0.08):
    """Independent loop port of the V6-W rule text: (a) drop low_risk, (b) options_implied -> short_interest,
    (c) tau >= fast_tau keeps 1/3 of 1/n and the freed mass goes to the slow members pro rata (none slow: unchanged),
    (d) 1/T per resulting theme."""
    theme = {i: ("short_interest" if t == "options_implied" else t) for i, t in zip(ids, themes)}
    tau = dict(zip(ids, taus))
    kept = [i for i in ids if theme[i] != "low_risk"]
    groups = {}
    for i in kept:
        groups.setdefault(theme[i], []).append(i)
    out = {i: 0.0 for i in ids}
    for t, members in groups.items():
        n = len(members)
        fast = [i for i in members if tau[i] >= fast_tau]
        slow = [i for i in members if i not in fast]
        for i in members:
            if not slow or not fast:
                within = 1.0 / n
            elif i in fast:
                within = (1.0 / n) / 3.0
            else:
                within = 1.0 / n + (len(fast) * (1.0 / n) * (2.0 / 3.0)) / len(slow)
            out[i] = within / len(groups)
    return out


def ref_within_theme_blend(ranks, weights, signs, themes):
    """Loop port of the IC runner's within-theme-v1 blend for one name: ranks[k] is None when k is not present."""
    mass, num, den = {}, {}, {}
    for r, w, s, t in zip(ranks, weights, signs, themes):
        if not w > 0:
            continue
        mass[t] = mass.get(t, 0.0) + w
        if r is not None and s != 0:
            num[t] = num.get(t, 0.0) + s * w * r
            den[t] = den.get(t, 0.0) + w
    return sum(mass[t] * num[t] / den[t] for t in num if den[t] > 0)


def runner_themes(text: bytes, ids: list[str], weights: list[float]):
    """Python port of the strategy_ic_runner.cpp composition_themes() added for V6-W (None: block absent)."""
    import re
    j = json.loads(text)
    if "theme_redistribution" not in j:
        return None
    block = j["theme_redistribution"]
    assert isinstance(block, dict) and block.get("rule") == "within-theme-v1"
    assert block.get("composition") == "ew-theme-v6" and isinstance(block.get("themes"), dict)
    rows = block["themes"]
    assert set(rows) <= set(ids), "theme for unknown candidate"
    assert all(isinstance(v, str) and re.fullmatch(r"[a-z0-9_]{1,64}", v) for v in rows.values())
    names, index = [], []
    for cid, w in zip(ids, weights):
        if not w > 0:
            index.append(0)
            continue
        assert cid in rows, f"theme missing for weighted candidate {cid}"
        if rows[cid] not in names:
            names.append(rows[cid])
        index.append(names.index(rows[cid]))
    assert 1 <= len(names) <= 32
    return index, names


class V6WeightRules(unittest.TestCase):
    def test_declared_constants(self):
        self.assertEqual(fcw.V6_RULE_ID, "ew-theme-v6")
        self.assertEqual((fcw.V6_FAST_TAU, fcw.V6_FAST_FACTOR), (0.08, 1.0 / 3.0))
        self.assertEqual((fcw.V6_DROPPED_THEMES, fcw.V6_MERGED_THEMES), (("low_risk",), {"options_implied": "short_interest"}))
        self.assertEqual(fcw.V6_REDISTRIBUTION, "within-theme-v1")
        self.assertIn("ew-theme-v6", fcw.COMPOSITIONS)
        self.assertIn("ew-theme-v6", fcw.PRIOR_COMPOSITIONS)

    def test_v51_shaped_hand_case(self):
        # the v5.1 member structure with synthetic taus: 9 prior themes -> 7; short_interest takes iv (merged), two of its
        # four members are fast; reversal_seasonality is all fast; low_risk (one fast member) is dropped
        rows = [("v1", "value", .02), ("v2", "value", .02), ("p1", "profitability_quality", .015),
                ("p2", "profitability_quality", .03), ("p3", "profitability_quality", .02), ("i1", "investment_issuance", .02),
                ("e1", "earnings_momentum", .03), ("e2", "earnings_momentum", .03), ("m1", "price_momentum", .04),
                ("lb", "low_risk", .05), ("lm", "low_risk", .089), ("sr", "short_interest", .03),
                ("dt", "short_interest", .04), ("sc", "short_interest", .092), ("rv", "reversal_seasonality", .18),
                ("ss", "reversal_seasonality", .094), ("iv", "options_implied", .09)]
        ids, themes, taus = (list(x) for x in zip(*rows))
        w, table, detail = fcw.ew_theme_v6_weights(ids, themes, taus)
        got = dict(zip(ids, w))
        want = ref_v6_weights(ids, themes, taus)
        for i in ids:
            self.assertAlmostEqual(got[i], want[i], places=15, msg=i)
        self.assertAlmostEqual(float(w.sum()), 1.0, places=15)                           # weights sum
        self.assertEqual(sorted(table), ["earnings_momentum", "investment_issuance", "price_momentum",
                                         "profitability_quality", "reversal_seasonality", "short_interest", "value"])
        for t, entry in table.items():                                                     # (d) theme mass 1/7
            self.assertAlmostEqual(sum(got[i] for i in entry["members"]), 1 / 7, places=15, msg=t)
            self.assertEqual(entry["theme_weight"], 1 / 7)
        self.assertEqual((got["lb"], got["lm"]), (0.0, 0.0))                               # (a)
        self.assertEqual(table["short_interest"]["members"], ["sr", "dt", "sc", "iv"])        # (b)
        self.assertEqual(table["short_interest"]["source_themes"], ["options_implied", "short_interest"])
        # (c) 1/4 -> 1/12 for sc and iv; the freed 1/3 splits over sr and dt: 1/4 + 1/6 = 5/12 each
        si = table["short_interest"]["within_theme_weights"]
        self.assertAlmostEqual(si["sc"], 1 / 12, places=16)
        self.assertAlmostEqual(si["iv"], 1 / 12, places=16)
        self.assertAlmostEqual(si["sr"], 5 / 12, places=15)
        self.assertAlmostEqual(si["dt"], 5 / 12, places=15)
        self.assertAlmostEqual(got["sc"], 1 / 84, places=16)
        self.assertEqual(table["reversal_seasonality"]["shrink"], "none-all-fast")          # no slow member: unchanged
        self.assertEqual((got["rv"], got["ss"]), (1 / 14, 1 / 14))
        self.assertEqual(detail["shrunk_members"], ["sc", "iv"])
        self.assertEqual(detail["fast_not_shrunk_all_fast_theme"], ["rv", "ss"])
        self.assertEqual(detail["fast_in_dropped_theme"], ["lm"])
        self.assertEqual(detail["dropped_members"], ["lb", "lm"])
        self.assertEqual(detail["merged_members"], {"iv": "short_interest"})

    def test_threshold_boundary_and_pro_rata(self):
        at, below = 0.08, float(np.nextafter(0.08, 0))
        w, table, detail = fcw.ew_theme_v6_weights(["a", "b", "c"], ["value"] * 3, [at, below, 0.01])
        # n = 3, a is fast (tau == .08 counts): 1/9; the freed 2/9 goes pro rata to b and c: 1/3 + 1/9 = 4/9 each
        np.testing.assert_allclose(w, [1 / 9, 4 / 9, 4 / 9], rtol=0, atol=1e-15)
        self.assertEqual(detail["shrunk_members"], ["a"])
        self.assertEqual(table["value"]["fast_members"], ["a"])
        # a lone fast member (and an all-fast theme) keeps its full within-theme weight
        w, table, _ = fcw.ew_theme_v6_weights(["x", "y"], ["value", "short_interest"], [0.5, 0.01])
        np.testing.assert_array_equal(w, [0.5, 0.5])
        self.assertEqual(table["value"]["shrink"], "none-all-fast")

    def test_all_dropped_and_refusals(self):
        w, table, detail = fcw.ew_theme_v6_weights(["a", "b"], ["low_risk", "low_risk"], [0.01, 0.2])
        np.testing.assert_array_equal(w, [0.0, 0.0])
        self.assertEqual((table, detail["dropped_members"]), ({}, ["a", "b"]))
        with self.assertRaises(fcw.FitError):
            fcw.ew_theme_v6_weights(["a"], ["value"], [NAN])
        with self.assertRaises(fcw.FitError):
            fcw.ew_theme_v6_weights(["a"], ["value"], [0.1, 0.2])

    def test_within_theme_blend_reference_hand_case(self):
        # the same numbers as the C++ test StrategyIcComposition.WithinThemeRedistributionKeepsMissingMassInTheme:
        # a1 ranks [-.5,-1/6,1/6,.5], a2 finite on names 2,3 only (ranks .5, -.5), b1 ranks [.5,1/6,-1/6,-.5];
        # weights .25/.25/.5, themes a, a, b -> [0, 0, 1/12, -1/4]
        a1, a2, b1 = [-.5, -1 / 6, 1 / 6, .5], [None, None, .5, -.5], [.5, 1 / 6, -1 / 6, -.5]
        got = [ref_within_theme_blend([a1[i], a2[i], b1[i]], [.25, .25, .5], [1, 1, 1], ["a", "a", "b"]) for i in range(4)]
        np.testing.assert_allclose(got, [0.0, 0.0, 1 / 12, -0.25], rtol=0, atol=1e-15)
        # with every member present the redistribution equals the fixed-denominator blend
        full = ref_within_theme_blend([.3, -.1, .2], [.25, .25, .5], [1, 1, 1], ["a", "a", "b"])
        self.assertAlmostEqual(full, .25 * .3 - .25 * .1 + .5 * .2, places=15)


def v6_world():
    """aim_world re-themed so ew-theme-v6 has something to drop, merge and shrink among admitted members."""
    panel, signals, ids, extra = aim_world()
    extra = dict(extra)
    extra["medium"] = dict(extra["medium"], theme="options_implied")   # admitted -> merged into short_interest
    extra["medium_half"] = dict(extra["medium_half"], theme="low_risk")  # admitted -> dropped
    return panel, signals, ids, extra


class V6EndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel, signals, ids, extra = v6_world()
        cls.ids = ids
        cls.fx = Fixture(cls.root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                         candidate_extra=extra)
        cls.v1_code, _ = fcw.fit(cls.fx.args(cls.root / "v1", **V4_ARGS))
        cls.v1_bytes = {p.name: p.read_bytes() for p in (cls.root / "v1").iterdir()}
        adm = json.loads(cls.v1_bytes[fcw.OUTPUT_ADMISSION])
        tau = {c["id"]: c["tau"] for c in adm["candidates"]}
        # threshold between the two short_interest' members, so exactly the faster one shrinks (tau >= threshold)
        cls.threshold = max(tau["flip"], tau["medium"])
        with unittest.mock.patch.object(fcw, "V6_FAST_TAU", cls.threshold):
            cls.code, cls.summary = fcw.fit(cls.fx.args(cls.root / "v6", **V6_ARGS))
        cls.bytes = {p.name: p.read_bytes() for p in (cls.root / "v6").iterdir()}
        cls.doc = json.loads(cls.bytes[fcw.OUTPUT_WEIGHTS])
        cls.adm = json.loads(cls.bytes[fcw.OUTPUT_ADMISSION])
        cls.v1 = json.loads(cls.v1_bytes[fcw.OUTPUT_WEIGHTS])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_weights_follow_the_rule_on_the_admission_taus(self):
        self.assertEqual((self.code, self.v1_code), (fcw.EXIT_OK, fcw.EXIT_OK))
        self.assertEqual(self.bytes[fcw.OUTPUT_ADMISSION], self.v1_bytes[fcw.OUTPUT_ADMISSION])  # screen unchanged
        w = self.doc["weights"]
        rows = {c["id"]: c for c in self.adm["candidates"]}
        members = [i for i in self.ids if self.v1["weights"][i] > 0]                            # v1 member set
        self.assertEqual(sorted(members), sorted(["slow_a_clone", "slow_a_twin", "flip", "medium", "medium_half"]))
        want = ref_v6_weights(members, [rows[i]["theme"] for i in members], [rows[i]["tau"] for i in members],
                              self.threshold)
        for i in self.ids:
            self.assertAlmostEqual(w[i], want.get(i, 0.0), places=15, msg=i)
        self.assertAlmostEqual(sum(w.values()), 1.0, places=15)
        self.assertEqual(w["medium_half"], 0.0)                                               # (a) low_risk dropped
        prov = self.doc["provenance"]
        self.assertEqual(prov["themes_present"], ["short_interest", "value"])                   # (b) + (d): T = 2
        for t, entry in prov["themes"].items():
            self.assertAlmostEqual(sum(w[i] for i in entry["members"]), 0.5, places=15, msg=t)
        self.assertEqual(sorted(prov["themes"]["short_interest"]["members"]), ["flip", "medium"])
        fast = "flip" if rows["flip"]["tau"] >= self.threshold else "medium"
        slow = "medium" if fast == "flip" else "flip"
        if rows[slow]["tau"] < self.threshold:                                                  # (c) 1/6 vs 5/6 of 1/2
            self.assertAlmostEqual(w[fast], 0.5 / 6, places=15)
            self.assertAlmostEqual(w[slow], 0.5 * 5 / 6, places=15)
            self.assertIn(fast, prov["v6"]["shrunk_members"])
        rows_w = {r["id"]: r for r in prov["candidates"]}
        self.assertEqual(rows_w["medium_half"]["status"], "fitted-theme-dropped-v6")
        self.assertEqual((rows_w["medium"]["theme"], rows_w["medium"]["theme_v6"]), ("options_implied", "short_interest"))
        self.assertAlmostEqual(prov["weighted_standalone_turnover"], sum(w[i] * rows_w[i]["tau"] for i in self.ids),
                               places=14)
        self.assertEqual(self.doc["signs"], self.v1["signs"])
        self.assertEqual(self.bytes[fcw.OUTPUT_WEIGHTS], fcw.canonical_bytes(self.doc))

    def test_redistribution_block_is_runner_readable(self):
        block = self.doc["theme_redistribution"]
        w = self.doc["weights"]
        self.assertEqual((block["rule"], block["composition"]), ("within-theme-v1", "ew-theme-v6"))
        self.assertEqual(block["themes"], {i: ("short_interest" if i in ("flip", "medium") else "value")
                                           for i in self.ids if w[i] > 0})
        weights = runner_accepts(self.bytes[fcw.OUTPUT_WEIGHTS], self.fx.library_sha, self.ids, self.fx.train_sha)
        index, names = runner_themes(self.bytes[fcw.OUTPUT_WEIGHTS], self.ids, weights)
        self.assertEqual(sorted(names), ["short_interest", "value"])
        self.assertEqual(len({index[k] for k, x in enumerate(weights) if x > 0}), 2)
        self.assertIsNone(runner_themes(self.v1_bytes[fcw.OUTPUT_WEIGHTS], self.ids, weights))  # v1: no block

    def test_provenance_block(self):
        v6 = self.doc["provenance"]["v6"]
        self.assertEqual(v6["rule"], "ew-theme-v6")
        self.assertEqual((v6["fast_tau_threshold"], v6["fast_tau_test"], v6["fast_factor"]),
                         (self.threshold, "tau_k >= fast_tau_threshold", 1.0 / 3.0))
        self.assertIn("admission.json key candidates[].tau", v6["fast_tau_source"])
        self.assertEqual((v6["dropped_themes"], v6["merged_themes"]), (["low_risk"], {"options_implied": "short_interest"}))
        self.assertEqual((v6["dropped_members"], v6["merged_members"]), (["medium_half"], {"medium": "short_interest"}))
        rows = {c["id"]: c for c in self.adm["candidates"]}
        self.assertEqual(v6["shrunk_tau"], {i: rows[i]["tau"] for i in v6["shrunk_members"]})
        self.assertEqual(v6["coverage_redistribution"]["rule"], "within-theme-v1")
        inputs = v6["inputs"]
        self.assertEqual(inputs["admission_sha256"], sha(self.bytes[fcw.OUTPUT_ADMISSION]))
        self.assertEqual((inputs["library_sha256"], inputs["train_manifest_sha256"]),
                         (self.fx.library_sha, self.fx.train_sha))
        self.assertEqual((inputs["orientations_sha256"], inputs["runner_summary_sha256"]),
                         (self.fx.orientations_sha, self.fx.summary_sha))
        self.assertEqual(inputs["script_sha256"], sha(Path(fcw.__file__).read_bytes()))
        self.assertEqual(self.summary["theme_redistribution"], "within-theme-v1")
        self.assertEqual(self.summary["dropped_members"], ["medium_half"])

    def test_schema_v2_only_for_v6(self):
        """Fix round 1 I1: ew-theme-v6 writes schema v2 (a runner predating the block refuses it); v1/aim stay v1."""
        self.assertEqual(self.doc["schema"], "atx.dsl-composition-weights/v2")
        self.assertEqual(fcw.WEIGHTS_SCHEMA_V2, "atx.dsl-composition-weights/v2")
        self.assertEqual(self.v1["schema"], "atx.dsl-composition-weights/v1")
        aim_out = self.root / "aim_schema"
        code, _ = fcw.fit(self.fx.args(aim_out, **AIM_ARGS))
        self.assertEqual(code, fcw.EXIT_OK)
        self.assertEqual(json.loads((aim_out / fcw.OUTPUT_WEIGHTS).read_bytes())["schema"],
                         "atx.dsl-composition-weights/v1")
        # The runner port refuses either schema with the other's block state.
        stripped = {k: v for k, v in self.doc.items() if k != "theme_redistribution"}
        grafted = dict(self.v1, theme_redistribution=self.doc["theme_redistribution"])
        for bad in (stripped, grafted, dict(self.doc, schema="atx.dsl-composition-weights/v3")):
            with self.assertRaises(AssertionError):
                runner_accepts(fcw.canonical_bytes(bad), self.fx.library_sha, self.ids, self.fx.train_sha)
        runner_accepts(fcw.canonical_bytes(self.doc), self.fx.library_sha, self.ids, self.fx.train_sha)

    def test_v1_and_aim_documents_carry_no_v6_keys(self):
        self.assertNotIn("theme_redistribution", self.v1)
        self.assertNotIn("v6", self.v1["provenance"])
        self.assertTrue(all("theme_v6" not in r for r in self.v1["provenance"]["candidates"]))
        aim_out = self.root / "aim"
        code, _ = fcw.fit(self.fx.args(aim_out, **AIM_ARGS))
        aim = json.loads((aim_out / fcw.OUTPUT_WEIGHTS).read_bytes())
        self.assertEqual(code, fcw.EXIT_OK)
        self.assertNotIn("theme_redistribution", aim)
        self.assertNotIn("v6", aim["provenance"])

    def test_combination_refusals_and_all_dropped(self):
        out = self.root / "refused"
        for override in (dict(screen="none", orientation="train", composition="ew-theme-v6"),
                         dict(screen="v4-prior-v1", orientation="train", composition="ew-theme-v6")):
            with self.assertRaises(fcw.FitError) as caught:
                fcw.fit(self.fx.args(out, **override))
            self.assertIn("go together", str(caught.exception))
            self.assertFalse(out.exists())
        panel, signals, ids, extra = v6_world()
        keep = [ids.index("medium_half")]
        fx = Fixture(self.root / "only_low_risk", panel, [signals[k] for k in keep], [1], ids=["medium_half"],
                     families=["fam"], candidate_extra=extra)
        dropped = self.root / "only_low_risk_out"
        code, summary = fcw.fit(fx.args(dropped, **V6_ARGS))
        self.assertEqual((code, summary["status"]), (fcw.EXIT_NO_WEIGHTS, "published-without-weights"))
        self.assertEqual(sorted(p.name for p in dropped.iterdir()), sorted([fcw.OUTPUT_ADMISSION, fcw.OUTPUT_ADMISSION_CSV]))


class V1BytesUnchangedByV6(unittest.TestCase):
    """V6-W edit: ew-theme-v1 (v4-prior-v1 and v4-prior-v2) and ew-theme-aim-v1 emit exactly the bytes of the fitter at
    the V6-W base (04e9d5bc) except the embedded script SHA and the SHAs derived from it (context digest, admission
    SHA), with identical work-cache paths. Same byte_stability contract as V1BytesUnchangedByAim."""

    BASE_BLOB = "bb11a677205a6e6c5ce573acdffc349e497ef7a9"  # fit_composition_weights.py at 04e9d5bc (T31 81e9977d)

    def test_v1_and_aim_bytes_unchanged(self):
        import importlib.util
        import subprocess
        try:
            old = subprocess.run(["git", "cat-file", "-p", self.BASE_BLOB], capture_output=True, check=True,
                                 cwd=Path(__file__).resolve().parent).stdout
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("git history with the V6-W base fitter blob is unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "old").mkdir()
            (root / "old" / "fit_composition_weights_v6_base.py").write_bytes(old)
            spec = importlib.util.spec_from_file_location("fcw_v6_base", root / "old" / "fit_composition_weights_v6_base.py")
            base = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(base)
            with superseded_window(fcw):   # the aim tag names the TRAIN window; W0-1 changed only the window
                self.assertEqual((base.SEMANTICS_TAG, base.AIM_TAG), (fcw.SEMANTICS_TAG, fcw.AIM_TAG))
            panel, signals, ids, extra = v6_world()
            fx = Fixture(root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                         candidate_extra=extra)
            for screen, composition in (("v4-prior-v1", "ew-theme-v1"), ("v4-prior-v2", "ew-theme-v1"),
                                        ("v4-prior-v1", "ew-theme-aim-v1")):
                got = {}
                for tag, module in (("new", fcw), ("old", base)):
                    out, work = root / f"{screen}-{composition}-{tag}", root / f"work-{screen}-{composition}-{tag}"
                    with unittest.mock.patch.object(module, "V42_COST_TAU_LIMIT", 0.249), superseded_window(module):
                        code, _ = module.fit(module.parse_args(fx.argv(out, screen, [
                            "--orientation", "prior", "--composition", composition, "--work-dir", str(work)])))
                    self.assertEqual(code, 0, (screen, composition))
                    adm = (out / fcw.OUTPUT_ADMISSION).read_bytes()
                    derived = {module.SCRIPT_SHA256: b"<script>", json.loads(adm)["inputs"]["context_sha256"]: b"<context>",
                               sha(adm): b"<admission>"}
                    files = {}
                    for p in out.iterdir():
                        data = p.read_bytes()
                        for value, token in derived.items():
                            data = data.replace(value.encode(), token)
                        files[p.name] = data
                    keys = sorted(str(p.relative_to(work)) for p in work.rglob("*") if p.is_file())
                    got[tag] = (files, keys)
                self.assertEqual(sorted(got["new"][0]), sorted(got["old"][0]), (screen, composition))
                for name in got["new"][0]:
                    self.assertEqual(got["new"][0][name], got["old"][0][name], f"{screen} {composition} {name}")
                self.assertEqual(work_shape(got["new"][1], new=True), work_shape(got["old"][1]), (screen, composition))



# ------------------------------------------------ platform-v7 L7: appended theme ownership_flow (library v7.0 prereg)
def load_blob(blob: str, name: str, root: Path):
    """The fitter at a git blob, loaded as a module (None when the git history is unavailable)."""
    import importlib.util
    import subprocess
    try:
        old = subprocess.run(["git", "cat-file", "-p", blob], capture_output=True, check=True,
                             cwd=Path(__file__).resolve().parent).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    (root / "old").mkdir(exist_ok=True)
    path = root / "old" / f"{name}.py"
    path.write_bytes(old)
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class OwnershipFlowTheme(unittest.TestCase):
    """v7-prereg "Library v7.0" (library-v7-draft 3.5c): ownership_flow is appended to the fitter's theme list now,
    empty in v7.0. A library that declares no appended theme (v6.1, v7.0) fits exactly the bytes of the pre-L7 fitter
    except the embedded script SHA and the SHAs derived from it (the byte_stability contract of V1BytesUnchangedByAim);
    a declared but unadmitted appended theme changes no weight; an admitted one is an ordinary ew-theme-v1 theme."""

    PRE_L7_BLOB = "eb41abddd4df777e7bef7261531077e57f4b0b72"  # fit_composition_weights.py at 4929824d (L7 base)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def world(self, relabel=None):
        panel, signals, ids, extra = v4_world()
        for i, theme in (relabel or {}).items():
            extra[i] = dict(extra[i], theme=theme)
        tag = "-".join(sorted(relabel or {})) or "base"
        fx = Fixture(self.root / f"fx-{tag}", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                     candidate_extra=extra)
        return fx, ids

    def test_declared_constants(self):
        self.assertEqual(fcw.V7_APPENDED_THEMES, ("ownership_flow",))
        self.assertEqual(fcw.PRIOR_THEMES, fcw.V4_THEMES + ("ownership_flow",))
        self.assertEqual(len(fcw.V4_THEMES), 9)

    def test_libraries_without_an_appended_theme_keep_the_pre_l7_bytes(self):
        base = load_blob(self.PRE_L7_BLOB, "fcw_pre_l7", self.root)
        if base is None:
            self.skipTest("git history with the pre-L7 fitter blob is unavailable")
        self.assertFalse(hasattr(base, "V7_APPENDED_THEMES"))
        self.assertEqual((base.SEMANTICS_TAG, base.V4_THEMES), (fcw.SEMANTICS_TAG, fcw.V4_THEMES))
        fx, _ = self.world()
        for screen in ("v4-prior-v1", "v4-prior-v2"):
            got = {}
            for tag, module in (("new", fcw), ("old", base)):
                out, work = self.root / f"{screen}-{tag}", self.root / f"work-{screen}-{tag}"
                with unittest.mock.patch.object(module, "V42_COST_TAU_LIMIT", 0.249), superseded_window(module):
                    code, _ = module.fit(module.parse_args(fx.argv(out, screen, [
                        "--orientation", "prior", "--composition", "ew-theme-v1", "--work-dir", str(work)])))
                self.assertEqual(code, 0, screen)
                adm = (out / fcw.OUTPUT_ADMISSION).read_bytes()
                derived = {module.SCRIPT_SHA256: b"<script>", json.loads(adm)["inputs"]["context_sha256"]: b"<context>",
                           sha(adm): b"<admission>"}
                files = {}
                for p in out.iterdir():
                    data = p.read_bytes()
                    for value, token in derived.items():
                        data = data.replace(value.encode(), token)
                    files[p.name] = data
                keys = sorted(str(p.relative_to(work)) for p in work.rglob("*") if p.is_file())
                got[tag] = (files, keys)
            self.assertEqual(sorted(got["new"][0]), sorted(got["old"][0]), screen)
            for name in got["new"][0]:
                self.assertEqual(got["new"][0][name], got["old"][0][name], f"{screen} {name}")
            self.assertEqual(work_shape(got["new"][1], new=True), work_shape(got["old"][1]), screen)  # v8 store names
            doc = json.loads(got["new"][0][fcw.OUTPUT_WEIGHTS])
            self.assertEqual(doc["provenance"]["themes_preregistered"], list(fcw.V4_THEMES))

    def fit(self, fx, tag):
        out = self.root / f"out-{tag}"
        code, _ = fcw.fit(fx.args(out, **V4_ARGS))
        self.assertEqual(code, fcw.EXIT_OK, tag)
        return (json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes()), json.loads((out / fcw.OUTPUT_ADMISSION).read_bytes()),
                (out / fcw.OUTPUT_WEIGHTS).read_bytes())

    def test_empty_appended_theme_changes_no_weight(self):
        doc0, adm0, _ = self.fit(self.world()[0], "base")
        # "insufficient" (prior_sign 0 -> reject_no_prior) now declares ownership_flow: the theme has no admitted member
        fx, ids = self.world({"insufficient": "ownership_flow"})
        doc, adm, blob = self.fit(fx, "empty")
        self.assertEqual((doc["weights"], doc["signs"]), (doc0["weights"], doc0["signs"]))
        self.assertEqual([c["status"] for c in adm["candidates"]], [c["status"] for c in adm0["candidates"]])
        prov, prov0 = doc["provenance"], doc0["provenance"]
        self.assertEqual((prov["themes_present"], prov["themes"]), (prov0["themes_present"], prov0["themes"]))
        self.assertNotIn("ownership_flow", prov["themes_present"])
        self.assertEqual(prov["themes_preregistered"], list(fcw.V4_THEMES) + ["ownership_flow"])
        self.assertIn("ownership_flow", prov["themes_declared"])
        self.assertEqual(runner_accepts(blob, fx.library_sha, ids, fx.train_sha), [doc0["weights"][i] for i in ids])

    def test_admitted_appended_theme_is_an_ordinary_theme_and_the_old_fitter_refuses_it(self):
        fx, ids = self.world({"flip": "ownership_flow"})  # flip is admitted (short_interest in v4_world)
        doc, _, _ = self.fit(fx, "admitted")
        w = doc["weights"]
        self.assertEqual({i: w[i] for i in ids if w[i] > 0}, {"slow_a_clone": 0.25, "slow_a_twin": 0.25, "flip": 0.5})
        self.assertEqual(doc["provenance"]["themes_present"], ["ownership_flow", "value"])
        self.assertEqual(doc["provenance"]["themes"]["ownership_flow"]["admitted"], ["flip"])
        lib = self.root / "bad.json"
        lib.write_bytes(json.dumps({"schema": fcw.LIBRARY_SCHEMA, "candidates": [
            {"id": "a", "family": "f", "dsl": "rank(close)", "theme": "ownership", "tier": "B", "prior_sign": 1}]
        }).encode())
        with self.assertRaises(fcw.FitError) as caught:  # an unknown theme is still refused
            fcw.load_priors(lib, sha(lib.read_bytes()), None, None, fcw.load_library(lib, sha(lib.read_bytes())))
        self.assertIn("appended theme ('ownership_flow',)", str(caught.exception))
        base = load_blob(self.PRE_L7_BLOB, "fcw_pre_l7_refuse", self.root)
        if base is None:
            self.skipTest("git history with the pre-L7 fitter blob is unavailable")
        library = base.load_library(fx.library, fx.library_sha)
        with self.assertRaises(base.FitError) as caught:
            base.load_priors(fx.library, fx.library_sha, None, None, library)
        self.assertIn("pre-registered v4 theme", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
