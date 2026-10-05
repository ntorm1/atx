"""COVR (ruling COV-12 (b)): compare the risk estimator across builds on the COV lane fixture (synthetic only).

  python covr_cmp.py stages A.dump B.dump            per-stage digest table of two CovStageDigest dumps
  python covr_cmp.py containers A_OUT B_OUT          the verb outputs (ATX_COVR_OUT) of two runs: fixture
                                                     inputs, model.atxcov per block / array / DIAG slot, the
                                                     other output files by SHA-256
  python covr_cmp.py det EXE OUT FIXTURE_IN          determinism of one risk exe: per recipe two runs at the
                                                     process's full CPU affinity and one at affinity mask 1;
                                                     every output file compared by SHA-256

The dumps are written by atx-impl/tests/strategy_cov_stage_diag.cpp (DISABLED_, run on demand).
"""
from __future__ import annotations

import ctypes
import hashlib
import json
import struct
import subprocess
import sys
import time
from collections import OrderedDict
from pathlib import Path

import numpy as np

# ---------------------------------------------------------------------------------------------------------- dumps


def read_dump(path: str) -> "OrderedDict[str, str]":
    out: "OrderedDict[str, str]" = OrderedDict()
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        key, _, value = line.partition(" ")
        out[key] = value
    return out


def stages(a_path: str, b_path: str) -> int:
    a, b = read_dump(a_path), read_dump(b_path)
    la, lb = a.get("meta/build", "?"), b.get("meta/build", "?")
    print(f"A = {a_path} ({la})\nB = {b_path} ({lb})")
    for k in a:
        if k.startswith("meta/") or k.startswith("check/"):
            print(f"  {k:28s} A: {a[k]}\n  {'':28s} B: {b.get(k, '<absent>')}")
    keys = [k for k in a if not k.startswith("meta/")]
    missing = [k for k in b if k not in a and not k.startswith("meta/")]
    if missing:
        print(f"keys only in B: {len(missing)} (first {missing[:3]})")
    first = next((k for k in keys if a[k] != b.get(k)), None)
    print(f"\nFIRST differing key in pipeline order: {first or 'none'}"
          + (f"  (A {a[first]} / B {b.get(first)})" if first else ""))
    print("\n-- inputs / constants / isolated probes / verb")
    for k in keys:
        if k.startswith("stage/"):
            continue
        same = a[k] == b.get(k)
        print(f"  {'SAME' if same else 'DIFF'}  {k:42s} A {a[k]:20s} B {b.get(k, '<absent>')}")
    print("\n-- per-session stages (sessions differing / first differing session index), pipeline order")
    for recipe in ("v11", "covv1"):
        order: list[str] = []
        seen: dict[str, list[int]] = {}
        total: dict[str, int] = {}
        for k in keys:
            parts = k.split("/")
            if len(parts) != 4 or parts[0] != "stage" or parts[1] != recipe:
                continue
            t, name = int(parts[2]), parts[3]
            if name not in seen:
                order.append(name)
                seen[name] = []
                total[name] = 0
            total[name] += 1
            if a[k] != b.get(k):
                seen[name].append(t)
        print(f"  recipe {recipe}:")
        for name in order:
            d = seen[name]
            print(f"    {name:24s} differ {len(d):3d} / {total[name]:3d}   first {d[0] if d else '-'}")
    return 0 if first is None else 1


# ----------------------------------------------------------------------------------------------- containers
HEADER = struct.Struct("<8sHHIIIQIIQIIIIqq32s32s32s48s32s")
SECTION = struct.Struct("<IIQQQII24s")
DATE = struct.Struct("<qqqQQIIIIII")
BLOCK = struct.Struct("<8sqqIIIIII")
ARRAY = struct.Struct("<IIQQ")
OPT = 1 << 31
ARRAYS = {1: "IDS", 2: "ROLE_INDEX", 3: "GROUP", 4: "EXPOSURES", 5: "SPECIFIC", 6: "FACTOR_COV",
          7 | OPT: "FACTOR_RETURN", 8 | OPT: "FACTOR_FLAGS", 9 | OPT: "ROW_FLAGS", 10 | OPT: "DIAG",
          11 | OPT: "EIGEN_GAMMA"}
DTYPES = {1: ("u1", 1), 2: ("<u2", 2), 3: ("<u4", 4), 4: ("<u8", 8), 5: ("<i8", 8), 6: ("<f4", 4), 7: ("<f8", 8)}


class Container:
    def __init__(self, path: Path):
        self.data = path.read_bytes()
        h = HEADER.unpack_from(self.data, 0)
        self.date_count, self.table, self.nsec = h[8], h[9], h[7]
        self.sections = {}
        for s in range(self.nsec):
            kind, _, off, nbytes, count, dtype, _, _ = SECTION.unpack_from(self.data, self.table + s * SECTION.size)
            self.sections[kind] = (off, nbytes, count, dtype)
        off = self.sections[1][0]
        self.meta = json.loads(self.data[off:off + self.sections[1][1]].decode("utf-8"))
        ioff = self.sections[3][0]
        self.index = [DATE.unpack_from(self.data, ioff + t * DATE.size) for t in range(self.date_count)]

    def arrays(self, t: int) -> dict[str, np.ndarray]:
        e = self.index[t]
        boff = e[3]
        hdr = BLOCK.unpack_from(self.data, boff)
        out = {}
        for i in range(hdr[8]):
            aid, dtype, aoff, count = ARRAY.unpack_from(self.data, boff + BLOCK.size + i * ARRAY.size)
            fmt, size = DTYPES[dtype]
            out[ARRAYS.get(aid, f"id{aid}")] = np.frombuffer(self.data, dtype=fmt, count=count, offset=boff + aoff)
        return out


def ulp(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    ia, ib = a.view("<i8").astype(np.int64), b.view("<i8").astype(np.int64)
    ia = np.where(ia < 0, np.int64(-(2**63)) - ia, ia)
    ib = np.where(ib < 0, np.int64(-(2**63)) - ib, ib)
    return np.abs(ia - ib)


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def _leaves(v, p=""):
    if isinstance(v, dict):
        for k, x in v.items():
            yield from _leaves(x, f"{p}/{k}")
    elif isinstance(v, list):
        for i, x in enumerate(v):
            yield from _leaves(x, f"{p}/{i}")
    else:
        yield p, v


def containers(a_root: str, b_root: str) -> int:
    ra, rb = Path(a_root), Path(b_root)
    print(f"A = {ra}\nB = {rb}")
    bad = 0
    for sub in ("role", "fields"):
        fa, fb = ra / "in" / sub, rb / "in" / sub
        names = sorted(p.name for p in fa.iterdir())
        diff = [n for n in names if sha(fa / n) != sha(fb / n)]
        print(f"fixture in/{sub}: {len(names)} files, differing {diff or 'none'}; manifest sha256 "
              f"A {sha(fa / 'manifest.json')[:16]} B {sha(fb / 'manifest.json')[:16]}")
    for key in ("v11", "covv1"):
        da, db = ra / key, rb / key
        print(f"\n== {key}")
        ca, cb = Container(da / "model.atxcov"), Container(db / "model.atxcov")
        print(f"  model.atxcov bytes A {len(ca.data)} B {len(cb.data)}; whole file equal {ca.data == cb.data}")
        if len(ca.data) == len(cb.data):
            names = {1: "META", 2: "FACTOR_NAMES", 3: "DATE_INDEX", 4: "BLOCK_HASHES", 5: "BLOCKS"}
            spans = [(0, 224, "header[0,224)"), (224, 256, "header.root"), (256, 4096, "header.pad")]
            spans += [(off, off + nb, names.get(k, f"kind{k}")) for k, (off, nb, _, _) in ca.sections.items()]
            spans += [(ca.table, ca.table + ca.nsec * SECTION.size, "SECTION_TABLE"),
                      (len(ca.data) - 64, len(ca.data), "trailer")]
            diff = np.flatnonzero(np.frombuffer(ca.data, np.uint8) != np.frombuffer(cb.data, np.uint8))
            by = {lab: int(np.searchsorted(diff, hi) - np.searchsorted(diff, lo)) for lo, hi, lab in spans}
            counted = sum(by.values())
            by = {k: v for k, v in by.items() if v}
            if diff.size - counted:
                by["other/padding"] = int(diff.size - counted)
            meta = sorted(k for k in set(dict(_leaves(ca.meta))) | set(dict(_leaves(cb.meta)))
                          if dict(_leaves(ca.meta)).get(k) != dict(_leaves(cb.meta)).get(k))
            print(f"  differing bytes {diff.size} by section {by or 'none'}; META leaves differing {meta or 'none'}")
        diag_names = ca.meta.get("diag_names", [])
        first_block = None
        per_array: dict[str, list[int]] = {}
        diag_first: dict[int, int] = {}
        index_diff = [t for t in range(ca.date_count) if ca.index[t] != cb.index[t]]
        fc_cells = fc_diff = 0
        fc_ulps: list[np.ndarray] = []
        for t in range(ca.date_count):
            xa, xb = ca.arrays(t), cb.arrays(t)
            for name in sorted(set(xa) | set(xb)):
                va, vb = xa.get(name), xb.get(name)
                same = va is not None and vb is not None and va.tobytes() == vb.tobytes()
                per_array.setdefault(name, [])
                if not same:
                    per_array[name].append(t)
                    if first_block is None:
                        first_block = (t, name)
                if name == "DIAG" and not same:
                    for s in np.flatnonzero(va.view("<u8") != vb.view("<u8")):
                        diag_first.setdefault(int(s), t)
                if name == "FACTOR_COV":
                    fin = np.isfinite(va) & np.isfinite(vb)
                    fc_cells += int(fin.sum())
                    dm = fin & (va.view("<u8") != vb.view("<u8"))
                    fc_diff += int(dm.sum())
                    if dm.any():
                        fc_ulps.append(ulp(va[dm], vb[dm]))
        print(f"  date index entries differing: {len(index_diff)}" + (f" (first {index_diff[0]})" if index_diff else ""))
        print(f"  first differing block / array: {first_block or 'none'}")
        for name, ts in per_array.items():
            print(f"    {name:14s} sessions differing {len(ts):3d} / {ca.date_count}  first {ts[0] if ts else '-'}")
        if diag_first:
            print("    DIAG slots differing (first session): " + ", ".join(
                f"{diag_names[s] if s < len(diag_names) else s}@{t}" for s, t in sorted(diag_first.items(), key=lambda x: x[1])))
        if fc_ulps:
            u = np.concatenate(fc_ulps)
            print(f"    FACTOR_COV finite cells {fc_cells}, differing {fc_diff}; ULP median {int(np.median(u))} max {int(u.max())}")
        names = sorted({p.name for p in da.iterdir()} | {p.name for p in db.iterdir()})
        other = [n for n in names if n not in ("model.atxcov", "manifest.json") and
                 (not (da / n).exists() or not (db / n).exists() or sha(da / n) != sha(db / n))]
        print(f"  other output files differing ({len(names) - 2} compared): {other or 'none'}")
        bad += first_block is not None or bool(other)
    return 1 if bad else 0


# ------------------------------------------------------------------------------------------------ determinism
RECIPES = {"v11": "atx-risk-v1.1", "covv1": "atx-cov-v1"}


def run(exe: str, fixture: Path, out: Path, recipe: str) -> float:
    role, fields = fixture / "role" / "manifest.json", fixture / "fields" / "manifest.json"
    argv = [exe, "risk", "--role", role.as_posix(), "--role-sha256", sha(role), "--fields", fields.as_posix(),
            "--fields-sha256", sha(fields), "--output", out.as_posix(), "--random-portfolios", "4",
            "--emit-exposures", "all", "--emit-container", "--bias-families", "extended", "--recipe", recipe]
    t0 = time.time()
    done = subprocess.run(argv, capture_output=True, text=True, check=False)
    (out.parent / f"{out.name}.log").write_text(done.stdout + done.stderr)
    if done.returncode != 0:
        raise SystemExit(f"{out.name}: exit {done.returncode}: {done.stderr.strip()[:300]}")
    return time.time() - t0


def tree(d: Path) -> dict[str, str]:
    return {p.name: sha(p) for p in sorted(d.iterdir())}


def det(exe: str, out_root: str, fixture_in: str) -> int:
    root, fixture = Path(out_root).resolve(), Path(fixture_in).resolve()
    exe = str(Path(exe).resolve())
    root.mkdir(parents=True, exist_ok=False)
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.GetCurrentProcess.restype = ctypes.c_void_p
    k32.GetProcessAffinityMask.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t),
                                           ctypes.POINTER(ctypes.c_size_t))
    k32.SetProcessAffinityMask.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
    proc = k32.GetCurrentProcess()
    pmask, smask = ctypes.c_size_t(), ctypes.c_size_t()
    if not k32.GetProcessAffinityMask(proc, ctypes.byref(pmask), ctypes.byref(smask)):
        raise SystemExit("GetProcessAffinityMask failed")
    print(f"exe {exe} sha256 {sha(Path(exe))}\nfixture {fixture}; process affinity mask {pmask.value:#x}")
    ok = True
    for key, recipe in RECIPES.items():
        w1 = run(exe, fixture, root / f"{key}-run1", recipe)
        w2 = run(exe, fixture, root / f"{key}-run2", recipe)
        if not k32.SetProcessAffinityMask(proc, 1):
            raise SystemExit("SetProcessAffinityMask failed")
        probe = ("import ctypes as c;k=c.WinDLL('kernel32');k.GetCurrentProcess.restype=c.c_void_p;"
                 "k.GetProcessAffinityMask.argtypes=(c.c_void_p,c.POINTER(c.c_size_t),c.POINTER(c.c_size_t));"
                 "a,b=c.c_size_t(),c.c_size_t();k.GetProcessAffinityMask(k.GetCurrentProcess(),c.byref(a),c.byref(b));"
                 "print(hex(a.value))")
        child = subprocess.run([sys.executable, "-c", probe], capture_output=True, text=True).stdout.strip()
        print(f"  {key}: a child started under mask 1 reports affinity {child}")
        try:
            w3 = run(exe, fixture, root / f"{key}-cpu1", recipe)  # the child inherits mask 1
        finally:
            k32.SetProcessAffinityMask(proc, pmask.value)
        t1, t2, t3 = (tree(root / f"{key}-{s}") for s in ("run1", "run2", "cpu1"))
        same12, same13 = t1 == t2, t1 == t3
        print(f"{key}: {len(t1)} files; run1 = run2 {same12}; run1 = cpu1 {same13}; model.atxcov sha256 "
              f"{t1.get('model.atxcov', '?')[:16]} / {t2.get('model.atxcov', '?')[:16]} / "
              f"{t3.get('model.atxcov', '?')[:16]}; wall {w1:.1f} / {w2:.1f} / {w3:.1f} s")
        for name in sorted(set(t1) | set(t2) | set(t3)):
            if not (t1.get(name) == t2.get(name) == t3.get(name)):
                print(f"    differs: {name}")
        ok &= same12 and same13
    print(f"determinism: {'HOLDS' if ok else 'MISS'}")
    return 0 if ok else 1


def main(argv: list[str]) -> int:
    if len(argv) >= 3 and argv[0] == "stages":
        return stages(argv[1], argv[2])
    if len(argv) >= 3 and argv[0] == "containers":
        return containers(argv[1], argv[2])
    if len(argv) >= 4 and argv[0] == "det":
        return det(argv[1], argv[2], argv[3])
    raise SystemExit(__doc__)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
