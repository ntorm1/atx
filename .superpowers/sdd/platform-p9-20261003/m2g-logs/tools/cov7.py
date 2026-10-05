"""COV-7 (root M2g item 4 (b)): Debug = Release container bytes on the lane fixture, both recipes.

1. Fixture: runs the Debug `atx-impl-strategy-target-tests.exe --gtest_filter=CovRecipe.CovV1RecipeRecorded` with TMP /
   TEMP set to a fresh directory, waits until the fixture's `in/role` + `in/fields` (CovPanel(300, 200, 23),
   `strategy_cov_fixture.hpp` `write_role_and_fields`) are complete (every payload hash in both manifests matches and
   the fields manifest binds the role manifest), copies `in/` to OUT/in and re-verifies the copy; the test then
   finishes and removes its own directory.
2. Runs both risk exes (Debug, Release) x both recipes on the copy:
   risk --role R --role-sha256 .. --fields F --fields-sha256 .. --output OUT/<dbg|rel>-<v11|covv1>
        --random-portfolios 4 --emit-exposures all --emit-container --bias-families extended --recipe <recipe>
3. Per recipe: `cov-diff dbg rel` with both exes; byte comparison of the two model.atxcov mapped to the file's
   sections (criterion 2 of the pre-run list); META JSON leaf differences; every other output file by SHA-256 and
   manifest.json by JSON leaf path.

usage: python cov7.py OUT TESTS_EXE DEBUG_RISK_EXE RELEASE_RISK_EXE   (cwd = the pool-2 root; OUT must not exist)
"""
from __future__ import annotations

import glob
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

RECIPES = {"v11": "atx-risk-v1.1", "covv1": "atx-cov-v1"}
HEADER = struct.Struct("<8sHHIIIQIIQIIIIqq32s32s32s48s32s")
SECTION = struct.Struct("<IIQQQII24s")
TRAILER = struct.Struct("<8sQ32s16s")
KINDS = {1: "MetaJson", 2: "FactorNames", 3: "DateIndex", 4: "BlockHashes", 5: "Blocks"}
ALLOWED = {"MetaJson", "header.root_sha256", "trailer.root_sha256"}


def sha(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def complete(in_dir: Path) -> bool:
    try:
        role = json.loads((in_dir / "role" / "manifest.json").read_text())
        fields = json.loads((in_dir / "fields" / "manifest.json").read_text())
        if fields["role"]["manifest_sha256"] != sha(in_dir / "role" / "manifest.json"):
            return False
        for sub, doc in (("role", role), ("fields", fields)):
            for name, e in doc["files"].items():
                if sha(in_dir / sub / name) != e["sha256"]:
                    return False
        return True
    except (OSError, ValueError, KeyError):
        return False


def extract(out: Path, tests_exe: str) -> None:
    tmp = out / "tmp"
    tmp.mkdir(parents=True)
    env = dict(os.environ, TMP=str(tmp.resolve()), TEMP=str(tmp.resolve()))
    proc = subprocess.Popen([tests_exe, "--gtest_filter=CovRecipe.CovV1RecipeRecorded"], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    t0, copied = time.time(), None
    while time.time() - t0 < 600 and copied is None:
        # the test binary nests its temp dirs one level down (an `atxv-<hash>` directory under TMP)
        for cand in glob.glob(str(tmp / "**" / "atx_cov_impl_*" / "in"), recursive=True):
            if complete(Path(cand)):
                shutil.copytree(cand, out / "in")
                copied = cand
                break
        if proc.poll() is not None and copied is None:
            break
        time.sleep(0.05)
    log, _ = proc.communicate(timeout=1800)
    (out / "fixture-gtest.log").write_text(log)
    print(f"fixture: copied {'yes' if copied else 'NO'} from {copied} after {time.time() - t0:.1f} s; "
          f"gtest exit {proc.returncode}; last line: {log.strip().splitlines()[-1] if log.strip() else ''}")
    if copied is None:
        raise SystemExit("cov7: the fixture was not captured")
    ok = complete(out / "in")
    print(f"fixture copy re-verified (payload hashes, fields bound to role): {ok}")
    if not ok:
        raise SystemExit("cov7: the copy does not verify")
    shutil.rmtree(tmp, ignore_errors=True)


def run_risk(exe: str, out: Path, tag: str, recipe: str) -> Path:
    role, fields = out / "in" / "role" / "manifest.json", out / "in" / "fields" / "manifest.json"
    dst = out / tag
    argv = [exe, "risk", "--role", role.as_posix(), "--role-sha256", sha(role), "--fields", fields.as_posix(),
            "--fields-sha256", sha(fields), "--output", dst.as_posix(), "--random-portfolios", "4",
            "--emit-exposures", "all", "--emit-container", "--bias-families", "extended", "--recipe", recipe]
    t0 = time.time()
    done = subprocess.run(argv, capture_output=True, text=True, check=False)
    (out / f"{tag}.log").write_text(done.stdout + done.stderr)
    print(f"run {tag}: exit {done.returncode}, {time.time() - t0:.1f} s")
    if done.returncode != 0:
        raise SystemExit(f"cov7: {tag} failed: {done.stderr.strip()[:400]}")
    return dst


def labels(data: bytes) -> list[tuple[int, int, str]]:
    h = HEADER.unpack_from(data, 0)
    header_bytes, table, nsec = h[3], h[9], h[7]
    spans = [(0, 224, "header.prefix"), (224, 256, "header.root_sha256"), (256, header_bytes, "header.pad")]
    for s in range(nsec):
        kind, _, off, nbytes, _, _, _, _ = SECTION.unpack_from(data, table + s * SECTION.size)
        spans.append((off, off + nbytes, KINDS.get(kind, f"kind{kind}")))
    spans.append((table, table + nsec * SECTION.size, "section_table"))
    n = len(data)
    spans += [(n - 64, n - 48, "trailer.head"), (n - 48, n - 16, "trailer.root_sha256"), (n - 16, n, "trailer.zero")]
    return spans


def meta(data: bytes) -> dict:
    h = HEADER.unpack_from(data, 0)
    for s in range(h[7]):
        kind, _, off, nbytes, _, _, _, _ = SECTION.unpack_from(data, h[9] + s * SECTION.size)
        if kind == 1:
            return json.loads(data[off:off + nbytes].decode("utf-8"))
    raise SystemExit("cov7: no META section")


def leaves(v, p=""):
    if isinstance(v, dict):
        for k, x in v.items():
            yield from leaves(x, f"{p}/{k}")
    elif isinstance(v, list):
        for i, x in enumerate(v):
            yield from leaves(x, f"{p}/{i}")
    else:
        yield p, v


def leaf_diff(a, b) -> list[str]:
    la, lb = dict(leaves(a)), dict(leaves(b))
    return sorted(k for k in set(la) | set(lb) if la.get(k, "<absent>") != lb.get(k, "<absent>"))


def compare(out: Path, key: str, dbg_exe: str, rel_exe: str) -> bool:
    d, r = out / f"dbg-{key}", out / f"rel-{key}"
    print(f"\n== recipe {RECIPES[key]}")
    ok = True
    for exe, name in ((dbg_exe, "Debug"), (rel_exe, "Release")):
        done = subprocess.run([exe, "cov-diff", (d / "model.atxcov").as_posix(), (r / "model.atxcov").as_posix()],
                              capture_output=True, text=True, check=False)
        print(f"  cov-diff ({name} exe): exit {done.returncode}: {(done.stdout + done.stderr).strip()}")
        ok &= done.returncode == 0
    a, b = (d / "model.atxcov").read_bytes(), (r / "model.atxcov").read_bytes()
    print(f"  model.atxcov bytes: dbg {len(a)} rel {len(b)}; file sha256 dbg {hashlib.sha256(a).hexdigest()[:16]} "
          f"rel {hashlib.sha256(b).hexdigest()[:16]}")
    ra, rb = HEADER.unpack_from(a, 0)[-1].hex(), HEADER.unpack_from(b, 0)[-1].hex()
    print(f"  root_sha256 dbg {ra}\n  root_sha256 rel {rb}")
    if len(a) != len(b):
        print("  SIZE DIFFERS: identity miss")
        return False
    spans = labels(a)
    diff = np.flatnonzero(np.frombuffer(a, dtype=np.uint8) != np.frombuffer(b, dtype=np.uint8))
    by: dict[str, int] = {}
    counted = 0
    for lo, hi, lab in spans:             # disjoint spans; the rest is padding
        c = int(np.searchsorted(diff, hi) - np.searchsorted(diff, lo))
        counted += c
        if c:
            by[lab] = by.get(lab, 0) + c
    if diff.size - counted:
        by["padding"] = int(diff.size - counted)
    print(f"  differing bytes: {diff.size}; by section: {by or 'none'}")
    outside = sorted(set(by) - ALLOWED)
    print(f"  differing bytes outside META / header root / trailer root: {outside or 'none'}")
    ok &= not outside
    md = leaf_diff(meta(a), meta(b))
    print(f"  META leaf paths that differ: {md or 'none'}")
    ok &= set(md) <= {"/producer/executable_sha256", "/producer/engine_git_sha"}
    names = sorted({p.name for p in d.iterdir()} | {p.name for p in r.iterdir()})
    other = []
    for n in names:
        if n in ("model.atxcov", "manifest.json"):
            continue
        if not (d / n).exists() or not (r / n).exists():
            other.append(f"{n} (one side only)")
        elif sha(d / n) != sha(r / n):
            other.append(n)
    print(f"  other output files ({len(names) - 2}) differing Debug vs Release: {other or 'none'}")
    mf = leaf_diff(json.loads((d / "manifest.json").read_text()), json.loads((r / "manifest.json").read_text()))
    print(f"  manifest.json leaf paths that differ: {mf or 'none'}")
    print(f"  COV-7 criterion (cov-diff 0 both exes; bytes differ only in META producer / roots): "
          f"{'HOLDS' if ok else 'MISS'}")
    return ok


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        raise SystemExit(__doc__)
    out = Path(argv[0])
    tests_exe, dbg_exe, rel_exe = (str(Path(p).resolve()) for p in argv[1:4])
    if out.exists():
        raise SystemExit(f"cov7: {out} exists")
    for p in (tests_exe, dbg_exe, rel_exe):
        print(f"exe {p} sha256 {sha(Path(p))}")
    extract(out, tests_exe)
    print(f"fixture role manifest sha256 {sha(out / 'in' / 'role' / 'manifest.json')}, fields manifest sha256 "
          f"{sha(out / 'in' / 'fields' / 'manifest.json')}")
    for key, recipe in RECIPES.items():
        run_risk(dbg_exe, out, f"dbg-{key}", recipe)
        run_risk(rel_exe, out, f"rel-{key}", recipe)
    results = [compare(out, key, dbg_exe, rel_exe) for key in RECIPES]
    print(f"\nCOV-7: {'HOLDS for both recipes' if all(results) else 'IDENTITY MISS'}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
