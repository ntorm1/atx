"""Exact, count-checked text substitutions in one source file, line ends kept (lane SQL4 tool).

usage: subst.py FILE EDITS.json   -- EDITS.json is a list of [old, new] pairs; each old must occur
exactly once in the LF-normalised text; the file's CRLF / LF convention is restored on write.
Prints every line over 100 columns that an edit touched (by its new text).
"""
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
edits = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
raw = path.read_bytes().decode("utf-8")
crlf = "\r\n" in raw
text = raw.replace("\r\n", "\n")
for old, new in edits:
    count = text.count(old)
    if count != 1:
        sys.exit(f"{path}: expected 1 occurrence, found {count}: {old[:80]!r}")
    text = text.replace(old, new)
path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))
new_lines = {line for _, new in edits for line in new.split("\n")}
for number, line in enumerate(text.split("\n"), 1):
    if len(line) > 100 and line in new_lines:
        print(f"{path}:{number}: {len(line)} columns")
print(f"{path}: {len(edits)} edits applied")
