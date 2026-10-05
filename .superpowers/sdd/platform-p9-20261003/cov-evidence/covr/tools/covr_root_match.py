"""COVR: this lane's risk exe outputs vs root's M2g COV-7 outputs on the same fixture (synthetic only).

  python covr_root_match.py ROOT_COV7_DIR BUILD MY_DET_DIR       BUILD = dbg | rel
Compares the fixture manifests (ROOT/in vs MY_DET_DIR/../<build>/in when present), and per recipe the container's
BLOCKS, BLOCK_HASHES and DATE_INDEX sections (META differs by design: the producer exe SHA-256 and git SHA) and the
legacy payload files by SHA-256: equal means this lane's build reproduces root's bytes, i.e. the diagnostic changed
no output of record.
"""
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from covr_cmp import Container  # noqa: E402


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def section(c: Container, kind: int) -> bytes:
    off, nbytes, _, _ = c.sections[kind]
    return c.data[off:off + nbytes]


def main(root: str, build: str, mine: str) -> int:
    r, m = Path(root), Path(mine)
    ok = True
    for key in ("v11", "covv1"):
        a, b = r / f"{build}-{key}", m / f"{key}-run1"
        ca, cb = Container(a / "model.atxcov"), Container(b / "model.atxcov")
        same = {name: section(ca, k) == section(cb, k) for k, name in ((5, "BLOCKS"), (4, "BLOCK_HASHES"), (3, "DATE_INDEX"))}
        files = {f: sha(a / f) == sha(b / f) for f in sorted(p.name for p in a.iterdir())
                 if f not in ("model.atxcov", "manifest.json")}
        print(f"{key} {build}: {same}; payload files equal {sum(files.values())}/{len(files)}"
              + ("" if all(files.values()) else f" differing {[f for f, v in files.items() if not v]}"))
        ok &= all(same.values()) and all(files.values())
    print(f"reproduces root's {build} bytes: {ok}")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    raise SystemExit(main(*sys.argv[1:]))
