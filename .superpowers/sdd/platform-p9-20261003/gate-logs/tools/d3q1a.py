"""GATE step 3, D3-Q1a (ledger "Ruling D3-Q1a", "Ruling D3-Q1"; `wave-review-ic.md` section 4 and M-2): the engine-fit
identity on the CURRENT Y-F0 TRAIN fit. Zero trials; nothing here opens a statistic: files are compared by SHA-256 and
JSON path, the engine-fit agreement by its status string and a zero / non-zero test.

usage (cwd C:/atx-wt/pool-2; interpreter C:/Program Files/Python312/python.exe):
  python d3q1a.py signals SIG        SIG/signals.json for the Y-F0 TRAIN role and library in roster order, from the
                                     Y-F0 fit receipt's own inputs (task-B1-report.md step 5: CacheLayout.resolve of
                                     every library candidate; absolute payload paths, root M1 note)
  python d3q1a.py factors SIG F      the step-1 Release NAV exe `factors --role R --role-sha256 S --signals SIG
                                     --output build-equity/F` (bounded runner, 600 s, 2048 MiB, Release)
  python d3q1a.py fit OUT [F]        the Y-F0 fit receipt's argv (p9-b0-yf0-fit-run) with only --output changed to
                                     build-equity/OUT; with F: plus `--engine-fit --engine-factors build-equity/F
                                     --admission-exe <Release p9-3f> --composition-exe <Release p9-3f>` (bounded runner)
  python d3q1a.py cmp PY ENG         admission.json / admission.csv by whole-file SHA-256; composition_weights.json with
                                     $.provenance.engine_fit deleted from both, canonical JSON (sort_keys, compact, UTF-8)
                                     SHA-256; the engine side's provenance.engine_fit.status and whether
                                     fit_max_rel_float_diff is exactly 0; every other file by SHA-256
Every run appends one line to gate-logs/d3q1a.txt (and the receipt is copied to gate-logs/receipt-<OUT>.json).
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[5]
LOGS = REPO / ".superpowers" / "sdd" / "platform-p9-20261003" / "gate-logs"
PY = sys.executable
FIT_REF = REPO / "build-equity" / "p9-b0-yf0-fit-run" / "receipt.json"
REL = REPO / "build-equity-rel" / "bin"
NAV = REL / "atx-equity-strategy-targets.exe"
ADMISSION = REL / "atx-research-admission.exe"
COMPOSITION = REL / "atx-research-composition.exe"


def sha_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def log(line: str) -> None:
    print(line)
    with (LOGS / "d3q1a.txt").open("a", encoding="utf-8") as fh:
        fh.write(line + "\n")


def ref_argv() -> list[str]:
    return json.loads(FIT_REF.read_text(encoding="utf-8"))["command"]


def opt(argv: list[str], flag: str) -> str:
    idx = [i for i, t in enumerate(argv) if t == flag]
    assert len(idx) == 1, (flag, idx)
    return argv[idx[0] + 1]


def bounded(run: str, cmd: list[str], build_type: str | None) -> int:
    run_dir = REPO / "build-equity" / f"{run}-run"
    runner = [PY, "scripts/run_bounded_research.py", "--output", f"build-equity/{run}-run", "--seconds", "600",
              "--max-rss-mib", "2048", "--admission-wait-seconds", "1800"]
    if build_type:
        runner += ["--build-type", build_type]
    with (LOGS / f"runner-{run}.log").open("wb") as fh:
        rc = subprocess.run(runner + ["--", *cmd], cwd=REPO, stdout=fh, stderr=subprocess.STDOUT,
                            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}).returncode
    rec, start = run_dir / "receipt.json", run_dir / "start.json"
    line = f"{run} runner-exit {rc}"
    if rec.is_file():
        shutil.copyfile(rec, LOGS / f"receipt-{run}.json")
        r = json.loads(rec.read_text(encoding="utf-8"))
        s = json.loads(start.read_text(encoding="utf-8")) if start.is_file() else {}
        line += (f" :: outcome {r.get('outcome')} exit {r.get('exit_code')} wall_s {r.get('wall_seconds')} peak_MiB "
                 f"{round((r.get('sampled_peak_tree_rss_bytes') or 0) / 2**20)} exe "
                 f"{(r.get('executable_sha256') or '')[:16]} build_type {r.get('build_type')} source "
                 f"{(r.get('source_sha') or '')[:8]} store-block receipt {'store' in r} start {'store' in s}")
        for name in ("stdout.log", "stderr.log"):
            if (run_dir / name).is_file():
                shutil.copyfile(run_dir / name, LOGS / f"{run}-{name}")
    log(line)
    return rc


def signals(sig: str) -> int:
    sys.path[:0] = [str(REPO / "atx-impl" / "tools")]
    import fit_composition_weights as fcw  # noqa: E402
    import test_factor_series_admission as b1  # noqa: E402
    a = ref_argv()
    train, train_sha = REPO / opt(a, "--train"), opt(a, "--train-sha256")
    lib, lib_sha = REPO / opt(a, "--library"), opt(a, "--library-sha256")
    orient, orient_sha = REPO / opt(a, "--orientations"), opt(a, "--orientations-sha256")
    summ, summ_sha = REPO / opt(a, "--runner-summary"), opt(a, "--runner-summary-sha256")
    os.chdir(REPO)  # relative cache directories resolve against the runner's cwd (CacheLayout doc)
    role = fcw.RoleManifest(train, train_sha)
    library = fcw.load_library(lib, lib_sha)
    _, recipe = fcw.load_orientations(orient, orient_sha, library, lib_sha, train_sha)
    layout = fcw.CacheLayout(summ, summ_sha, role, orient_sha, recipe)
    entries = []
    for c in library:
        e = dict(layout.resolve(c, role))
        e["payload"] = str(Path(e["payload"]).resolve())
        entries.append(e)
    doc = b1.signals_document(train_sha, entries)
    out = REPO / "build-equity" / sig
    out.mkdir()
    data = json.dumps(doc).encode("utf-8")
    (out / "signals.json").write_bytes(data)
    log(f"signals {sig}: {len(entries)} candidates (library {lib_sha[:8]}, roster order), payload roots "
        f"{sorted({str(Path(e['payload']).parent.parent) for e in entries})}, signals.json sha256 "
        f"{hashlib.sha256(data).hexdigest()}")
    return 0


def factors(sig: str, f: str) -> int:
    a = ref_argv()
    log(f"factors exe {NAV.relative_to(REPO).as_posix()} sha256 {sha_file(NAV)}")
    return bounded(f, [str(NAV), "factors", "--role", opt(a, "--train"), "--role-sha256", opt(a, "--train-sha256"),
                       "--signals", f"build-equity/{sig}", "--output", f"build-equity/{f}"], "Release")


def fit(out: str, f: str | None) -> int:
    a = ref_argv()
    k = a.index("--output")
    a[k + 1] = f"build-equity/{out}"
    if f:
        a += ["--engine-fit", "--engine-factors", f"build-equity/{f}", "--admission-exe", str(ADMISSION),
              "--composition-exe", str(COMPOSITION)]
        log(f"fit {out} engine exes admission {sha_file(ADMISSION)} composition {sha_file(COMPOSITION)}; factors "
            f"manifest sha256 {sha_file(REPO / 'build-equity' / f / 'manifest.json')}")
    log(f"fit {out} argv tail {a[k:k + 2]} + {a[len(ref_argv()):]}")
    return bounded(out, a, None)


def canon(doc) -> str:
    return hashlib.sha256(json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                                     allow_nan=True).encode("utf-8")).hexdigest()


def cmp(py: str, eng: str) -> int:
    pdir, edir = REPO / "build-equity" / py, REPO / "build-equity" / eng
    pf = {p.relative_to(pdir).as_posix() for p in pdir.rglob("*") if p.is_file()}
    ef = {p.relative_to(edir).as_posix() for p in edir.rglob("*") if p.is_file()}
    log(f"cmp {py} ({len(pf)} files) vs {eng} ({len(ef)} files); only python {sorted(pf - ef)}; only engine "
        f"{sorted(ef - pf)}")
    ok = True
    for name in ("admission.json", "admission.csv"):
        a, b = sha_file(pdir / name), sha_file(edir / name)
        ok &= a == b
        log(f"  {name}: python {a} engine {b} equal {a == b}")
    pw = json.loads((pdir / "composition_weights.json").read_text(encoding="utf-8"))
    ew = json.loads((edir / "composition_weights.json").read_text(encoding="utf-8"))
    had_p = "engine_fit" in pw.get("provenance", {})
    eng_fit = ew.get("provenance", {}).pop("engine_fit", None)
    pw.get("provenance", {}).pop("engine_fit", None)
    cp, ce = canon(pw), canon(ew)
    ok &= cp == ce and not had_p and eng_fit is not None
    status = eng_fit.get("status") if isinstance(eng_fit, dict) else None
    diff = eng_fit.get("fit_max_rel_float_diff") if isinstance(eng_fit, dict) else None
    log(f"  composition_weights.json minus $.provenance.engine_fit, canonical JSON sha256: python {cp} engine {ce} "
        f"equal {cp == ce}; python side had the key {had_p}; engine provenance.engine_fit keys "
        f"{sorted(eng_fit) if isinstance(eng_fit, dict) else None}; status {status!r}; fit_max_rel_float_diff "
        f"{'exactly 0' if diff == 0 else ('absent' if diff is None else 'NON-ZERO')}; factors_manifest_sha256 "
        f"{eng_fit.get('factors_manifest_sha256') if isinstance(eng_fit, dict) else None}")
    for name in sorted((pf & ef) - {"admission.json", "admission.csv", "composition_weights.json"}):
        a, b = sha_file(pdir / name), sha_file(edir / name)
        log(f"  {name}: equal {a == b}")
    log(f"D3-Q1a VERDICT: {'IDENTITY (P9-F0h with --engine-fit)' if ok else 'DIFFERENT (P9-W lapses, PRE-35; finding)'}")
    return 0 if ok else 1


def main(argv: list[str]) -> int:
    verb = argv[0]
    if verb == "signals":
        return signals(argv[1])
    if verb == "factors":
        return factors(argv[1], argv[2])
    if verb == "fit":
        return fit(argv[1], argv[2] if len(argv) > 2 else None)
    if verb == "cmp":
        return cmp(argv[1], argv[2])
    raise SystemExit(f"unknown verb {verb}")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
