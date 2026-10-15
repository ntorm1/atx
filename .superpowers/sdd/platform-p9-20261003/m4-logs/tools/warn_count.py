"""Root M3: count warning lines in a research-build log (atx-build writes it as UTF-16 LE; grep on the raw bytes
counts nothing). usage: warn_count.py LOG -> prints the count, or '?' when the file is missing."""
import re
import sys
from pathlib import Path

p = Path(sys.argv[1])
if not p.is_file():
    print("?")
    sys.exit(0)
raw = p.read_bytes()
text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8", errors="replace")
print(sum(1 for line in text.splitlines() if re.search(r"(?i)warning[: ]", line)))
