"""GATE step 2 (GATE-COVDIFF; wave-review-nav S-7): read-only hand check of a D-COV container store: every entry of the
manifest's `files` (bytes, SHA-256) and its `container` (file, bytes, file_sha256) against the bytes on disk, plus
the manifest's own SHA-256 against the store pin of record (handoff-root-m2i.md "Store pins"). Prints names, sizes and
hashes only (no payload value is read or decoded). Never writes into the store.
usage: store_files_check.py STORE_DIR [EXPECTED_MANIFEST_SHA256]
"""
import hashlib
import json
import sys
from pathlib import Path


def sha(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    store = Path(sys.argv[1])
    want_manifest = sys.argv[2] if len(sys.argv) > 2 else None
    man_path = store / "manifest.json"
    man_sha = sha(man_path)
    m = json.loads(man_path.read_bytes())
    bad = 0
    print(f"store {store.as_posix()}")
    line = f"  manifest.json sha256 {man_sha}"
    if want_manifest:
        ok = man_sha == want_manifest
        bad += 0 if ok else 1
        line += f" (pin {'=' if ok else '!='} {want_manifest[:16]})"
    print(line)
    files = m.get("files") or {}
    on_disk = sorted(p.name for p in store.iterdir() if p.is_file() and p.name != "manifest.json")
    for name in sorted(files):
        e = files[name]
        p = store / name
        if not p.is_file():
            print(f"  {name}: MISSING on disk")
            bad += 1
            continue
        got_sha, got_bytes = sha(p), p.stat().st_size
        ok = got_sha == e.get("sha256") and got_bytes == e.get("bytes")
        bad += 0 if ok else 1
        print(f"  {name}: bytes {got_bytes} sha256 {got_sha} {'= manifest' if ok else '!= manifest ' + str(e)}")
    c = m.get("container") or {}
    if c:
        p = store / c["file"]
        got_sha, got_bytes = sha(p), p.stat().st_size
        ok = got_sha == c.get("file_sha256") and got_bytes == c.get("bytes")
        bad += 0 if ok else 1
        print(f"  container {c['file']}: bytes {got_bytes} sha256 {got_sha} root_sha256 {c.get('root_sha256')} "
              f"{'= manifest' if ok else '!= manifest'}")
    listed = set(files) | ({c["file"]} if c else set())
    extra = [n for n in on_disk if n not in listed]
    print(f"  files listed {len(files)} (+container {1 if c else 0}); on disk besides manifest {len(on_disk)}; "
          f"on disk but unlisted {extra}")
    print(f"  VERDICT {'HOLDS' if bad == 0 else 'FAILS'} (mismatches {bad})")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
