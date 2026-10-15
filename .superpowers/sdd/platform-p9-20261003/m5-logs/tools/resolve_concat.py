"""Root M5 (B2-14 region edit): resolve every conflict region of the given files as keep-both, OURS (root) lines
first, then THEIRS (the lane) lines, unchanged; refuses a diff3 base section; keeps the file's line ending.
usage: resolve_concat.py FILE..."""
import sys
from pathlib import Path

for name in sys.argv[1:]:
    p = Path(name)
    raw = p.read_bytes()
    nl = b"\r\n" if b"\r\n" in raw else b"\n"
    out, state, ours, theirs, regions = [], None, [], [], 0
    for line in raw.split(nl):
        if line.startswith(b"<<<<<<< "):
            assert state is None
            state, ours, theirs = "ours", [], []
        elif line.startswith(b"||||||| "):
            raise SystemExit(f"{name}: diff3 base section: resolve by hand")
        elif line == b"=======" and state == "ours":
            state = "theirs"
        elif line.startswith(b">>>>>>> ") and state == "theirs":
            out += ours + theirs
            state, regions = None, regions + 1
        elif state == "ours":
            ours.append(line)
        elif state == "theirs":
            theirs.append(line)
        else:
            out.append(line)
    assert state is None, name
    p.write_bytes(nl.join(out))
    print(f"{name}: {regions} region(s) resolved keep-both (ours first)")
