"""ABS lane: exact, CRLF-aware replacements in a checked-out file (every anchor must match once).

The checkout uses core.autocrlf=true: every line of a C++ / CMake file ends in CRLF in the working
tree. A File reads the bytes, asserts CRLF throughout, edits LF text and writes CRLF back, so an
unchanged line keeps its exact bytes.
"""
from pathlib import Path

CRLF = "\r\n"
LF = "\n"


class File:
    def __init__(self, path: str) -> None:
        self.path = Path(path)
        raw = self.path.read_bytes().decode("utf-8")
        assert raw.count(CRLF) == raw.count(LF), f"{path}: not CRLF throughout"
        self.text = raw.replace(CRLF, LF)

    def once(self, old: str, new: str) -> None:
        count = self.text.count(old)
        assert count == 1, (str(self.path), count, old[:200])
        self.text = self.text.replace(old, new)

    def save(self) -> None:
        self.path.write_bytes(self.text.replace(LF, CRLF).encode("utf-8"))
