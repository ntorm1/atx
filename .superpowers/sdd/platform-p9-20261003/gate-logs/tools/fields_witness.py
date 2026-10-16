"""GATE agent 3 (ruling FIELDS-SUB, ledger :979): the witness that fields v15 holds, byte for byte, every payload of
fields v10 that R-2's spec names, so R-2's P9-B0 NAV can read `fields-v15/manifest.json` instead of the deleted v10
(FIELDS-GONE, ledger :978).

For every field f of `scripts/specs/v8/lib-v80.json` `fields.list` (R-2's spec; 70 names):
  (a) `train-2020-2023-lo3-fields-v11/manifest.json` has the field entry f (`fields[]`, name f, file f.f64) whose
      `reused_from.dir` ends `fields-v10`, `reused_from.manifest_sha256` = v10's manifest SHA-256 (the pin of
      `lib-v80.json` `fields.manifest_sha256`, a4a060ae...) and which carries a `payload_sha256` W(f);
  (b) `train-2020-2023-lo3-fields-v15/manifest.json` `files[f.f64].sha256` = W(f);
  (c) the bytes on disk `train-2020-2023-lo3-fields-v15/f.f64` hash to W(f) (every one of the 70 hashed in full).
Also, for the two fields R-2's NAV of record says it read (its `summary.json` `/financing_fields/fields_used/*/sha256`,
names and SHA-256 only): that SHA-256 = v15's `files[f.f64].sha256` = the on-disk hash.
Reads manifests and payload bytes only (hashes); prints names, SHA-256 and verdicts; no coverage block, no value.

usage: python fields_witness.py OUT_TXT     (cwd = the pool root); exit 0 iff 70 / 70 and the NAV's fields hold
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

SPEC = Path("scripts/specs/v8/lib-v80.json")
V11 = Path("build-equity/train-2020-2023-lo3-fields-v11/manifest.json")
V15D = Path("build-equity/train-2020-2023-lo3-fields-v15")
R2_NAV = Path("build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80/summary.json")
V10_PIN = "a4a060ae29c10080ca89b4b29335709558c2ae4842bd29db0398b23a43070809"


def sha_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for c in iter(lambda: f.read(1 << 22), b""):
            h.update(c)
    return h.hexdigest()


def main(out: Path) -> int:
    lines: list[str] = []
    say = lines.append
    spec = json.loads(SPEC.read_text(encoding="utf-8"))
    names = list(spec["fields"]["list"])
    pin = spec["fields"]["manifest_sha256"]
    v11 = json.loads(V11.read_text(encoding="utf-8"))
    v15m = V15D / "manifest.json"
    v15 = json.loads(v15m.read_text(encoding="utf-8"))
    say(f"spec {SPEC.as_posix()} sha256 {sha_file(SPEC)}: fields.list {len(names)} names (distinct {len(set(names))}); "
        f"fields.output {spec['fields']['output']}; fields.manifest_sha256 {pin} (= v10 pin {pin == V10_PIN})")
    say(f"v11 manifest {V11.as_posix()} sha256 {sha_file(V11)}: field entries {len(v11['fields'])}; "
        f"reuse.from {v11['reuse'].get('from')} reuse.manifest_sha256 {v11['reuse'].get('manifest_sha256')}")
    say(f"v15 manifest {v15m.as_posix()} sha256 {sha_file(v15m)}: field entries {len(v15['fields'])}, files "
        f"{len(v15['files'])}")
    by11 = {e["name"]: e for e in v11["fields"]}
    held = 0
    misses: list[str] = []
    for f in names:
        e = by11.get(f)
        rf = (e or {}).get("reused_from") or {}
        w = rf.get("payload_sha256")
        a = (e is not None and e.get("file") == f"{f}.f64" and str(rf.get("dir", "")).replace("\\", "/").endswith(
            "fields-v10") and rf.get("manifest_sha256") == V10_PIN and bool(w))
        b15 = (v15["files"].get(f"{f}.f64") or {}).get("sha256")
        b = a and b15 == w
        p = V15D / f"{f}.f64"
        disk = sha_file(p) if p.is_file() else "MISSING"
        c = a and disk == w
        ok = a and b and c
        held += ok
        if not ok:
            misses.append(f)
        say(f"  {f}: (a) v11 witness {'ok' if a else 'MISS'} payload {w}; (b) v15 files {'=' if b else '!='} "
            f"{b15}; (c) disk {'=' if c else '!='} {disk}; {'HOLDS' if ok else 'MISS'}")
    say(f"witness: {held} / {len(names)} hold; misses {misses or 'none'}")
    nav = json.loads(R2_NAV.read_text(encoding="utf-8"))
    used = (nav.get("financing_fields") or {}).get("fields_used") or {}
    nav_ok = bool(used)
    for f, rec in sorted(used.items()):
        s = rec.get("sha256")
        b15 = (v15["files"].get(f"{f}.f64") or {}).get("sha256")
        p = V15D / f"{f}.f64"
        disk = sha_file(p) if p.is_file() else "MISSING"
        ok = s == b15 == disk
        nav_ok &= ok
        say(f"  R-2 NAV of record read {f}: recorded {s}; v15 files {b15}; disk {disk}; in fields.list "
            f"{f in names}; {'HOLDS' if ok else 'MISS'}")
    say(f"R-2 NAV of record: financing_fields.manifest_sha256 "
        f"{(nav.get('financing_fields') or {}).get('manifest_sha256')}; fields it read: {len(used)}, "
        f"{'all equal in v15' if nav_ok else 'NOT all equal'}")
    verdict = held == len(names) == 70 and nav_ok
    say(f"VERDICT: {'70 / 70 HOLD' if verdict else 'MISS'} (FIELDS-SUB path {'A may proceed' if verdict else 'B'})")
    text = "\n".join(lines) + "\n"
    out.write_text(text, encoding="utf-8", newline="\n")
    print(text, end="")
    return 0 if verdict else 1


if __name__ == "__main__":
    raise SystemExit(main(Path(sys.argv[1])))
