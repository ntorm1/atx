"""M6b (RESUME 11): names-only list of reparse points (symlinks, junctions) under ROOT before a real-tree catalog run
(wave-review-store.md S-2, "What root must do" 2). Skips `.git` and never descends into a seal-named directory (a
path segment naming a year 2024 or later); reads no file content. usage: reparse_points.py ROOT
"""
import os
import re
import stat
import sys

SEALED = re.compile(r"(?<!\d)20(2[4-9]|[3-9]\d)(?!\d)")
root = sys.argv[1]
found, dirs_seen, pruned = [], 0, 0
for dp, dn, fn in os.walk(root):
    keep = []
    for d in dn:
        p = os.path.join(dp, d)
        attrs = getattr(os.lstat(p), "st_file_attributes", 0)
        if attrs & stat.FILE_ATTRIBUTE_REPARSE_POINT or os.path.islink(p):
            found.append(os.path.relpath(p, root))
            continue                              # never follow one
        if d == ".git" or SEALED.search(d):
            pruned += 1
            continue
        keep.append(d)
    dn[:] = keep
    dirs_seen += 1
    for f in fn:
        p = os.path.join(dp, f)
        if getattr(os.lstat(p), "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            found.append(os.path.relpath(p, root))
print(f"{root}: dirs walked {dirs_seen}, pruned (.git or seal-named) {pruned}, reparse points {len(found)}")
for p in found:
    print("  ", p)
