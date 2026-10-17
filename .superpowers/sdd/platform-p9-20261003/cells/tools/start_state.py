"""Cells root (CELLS-6, DISPATCH-COMMIT step 0 / per-cell step 0): a read-only start-state / pre-cell record.

Prints: the trial ledger's line count and full SHA-256; research-store and index.sqlite presence; each P9-B0
reference dir's per-file listing (the b0-files form: relpath TAB size TAB sha256) re-computed and compared byte for
byte with gate-logs/b0-files-<dir>.txt; ucrtbase.dll file version and SHA-256; free RAM and C: free.
Opens no output file beyond hashing it. usage (cwd = pool root): start_state.py [--refs] [--label TEXT]
"""
from __future__ import annotations

import ctypes
import datetime as dt
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[5]
SDD = REPO / ".superpowers" / "sdd" / "platform-p9-20261003"
LOGS = SDD / "gate-logs"
REFS = ["p9-b0-r2-nav", "p9-b0g-x10-nav", "p9-b0g-y1-nav", "p9-b0g-yf0-u", "p9-b0g-yf0-w", "p9-b0g-yf0-nav"]
UCRT = Path(r"C:\Windows\System32\ucrtbase.dll")
UCRT_SHA = "2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF"
UCRT_VER = "10.0.26100.7623"


def sha_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for c in iter(lambda: f.read(1 << 22), b""):
            h.update(c)
    return h.hexdigest()


def b0_listing(od: Path) -> str:
    files = sorted(p for p in od.rglob("*") if p.is_file())
    rows = [f"{p.relative_to(od).as_posix()}\t{p.stat().st_size}\t{sha_file(p)}" for p in files]
    return "\n".join(rows) + "\n"


class MEMSTAT(ctypes.Structure):
    _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]


def host() -> str:
    m = MEMSTAT()
    m.dwLength = ctypes.sizeof(MEMSTAT)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    du = shutil.disk_usage("C:\\")
    return (f"free RAM {m.ullAvailPhys / 2**30:.2f} of {m.ullTotalPhys / 2**30:.2f} GB "
            f"({m.ullAvailPhys // 2**20} MiB); C: {du.free / 1e9:.1f} GB free")


def ucrt() -> str:
    ver = subprocess.run(["powershell", "-NoProfile", "-Command",
                          f"(Get-Item '{UCRT}').VersionInfo.FileVersion"],
                         capture_output=True, text=True).stdout.strip()
    sha = sha_file(UCRT).upper()
    ok = sha == UCRT_SHA and ver.startswith(UCRT_VER)
    return (f"ucrtbase.dll ({UCRT.parent}) file version {ver} SHA-256 {sha} "
            f"({'= the P9-B0 record, C.6' if ok else 'DIFFERS FROM THE P9-B0 RECORD: STOP'})")


def main(argv: list[str]) -> int:
    label = argv[argv.index("--label") + 1] if "--label" in argv else ""
    now = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    out = [f"# {label} {now}".rstrip()]
    led = REPO / "build-equity" / "trials.jsonl"
    data = led.read_bytes()
    out.append(f"trial ledger build-equity/trials.jsonl: {data.count(b'\n')} lines, sha256 "
               f"{hashlib.sha256(data).hexdigest()}")
    rs = REPO / "build-equity" / "research-store"
    idx = REPO / "build-equity" / "mega-candidate-cache-v8-lo3" / "index.sqlite"
    out.append(f"build-equity/research-store/: {'PRESENT' if rs.exists() else 'absent'}")
    out.append(f"build-equity/mega-candidate-cache-v8-lo3/index.sqlite: {'PRESENT' if idx.exists() else 'absent'}")
    if "--refs" in argv:
        for d in REFS:
            od = REPO / "build-equity" / d
            ref = (LOGS / f"b0-files-{d}.txt").read_text(encoding="utf-8")
            now_text = b0_listing(od) if od.is_dir() else ""
            n = now_text.count("\n")
            out.append(f"reference {d}: {n} files, listing sha256 {hashlib.sha256(now_text.encode()).hexdigest()} "
                       f"{'= gate-logs/b0-files-' + d + '.txt' if now_text == ref else 'DIFFERS from gate-logs/b0-files-' + d + '.txt: STOP'}")
    out.append(ucrt())
    out.append("host: " + host())
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
