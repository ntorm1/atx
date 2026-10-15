"""M6b (RESUME 11): a names-only listing of a directory tree (relative path, size, mtime_ns), sorted, plus its digest,
to show a cache dir ends as it began (SQL4 review "What root must check" 12). Reads no file content unless --sha
(then the SHA-256 of files up to 16 MiB). usage: dir_listing.py DIR OUT [--sha]
"""
import hashlib
import os
import sys

root, out = sys.argv[1], sys.argv[2]
rows = []
for dp, dn, fn in os.walk(root):
    dn.sort()
    for f in sorted(fn):
        p = os.path.join(dp, f)
        st = os.stat(p)
        row = f"{os.path.relpath(p, root).replace(os.sep, '/')}\t{st.st_size}\t{st.st_mtime_ns}"
        if "--sha" in sys.argv[3:] and st.st_size <= 16 << 20:
            row += "\t" + hashlib.sha256(open(p, "rb").read()).hexdigest()
        rows.append(row)
rows.sort()
text = "\n".join(rows) + "\n"
open(out, "w", encoding="utf-8", newline="\n").write(text)
print(f"{root}: {len(rows)} files, {sum(int(r.split(chr(9))[1]) for r in rows)} bytes, listing sha256 "
      f"{hashlib.sha256(text.encode()).hexdigest()[:16]}")
