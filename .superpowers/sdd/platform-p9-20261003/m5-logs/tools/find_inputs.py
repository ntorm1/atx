"""Root M5c (A4-DB): list (path, size) of files whose NAME matches the two fields' TRAIN input files, and of dirs
named like their stages, under the given roots (os.walk: names and sizes only; no file is opened).
usage: find_inputs.py ROOT..."""
import os
import sys

NAMES = {"ftd.parquet", "threshold.parquet", "lists.parquet", "finra_names.parquet"}
DIRS = {"ftd", "regsho_threshold", "security_master", "alpha_panel"}
for root in sys.argv[1:]:
    hits = 0
    for d, subdirs, files in os.walk(root):
        subdirs[:] = [s for s in subdirs if s not in (".git",)]
        for s in subdirs:
            if s.lower() in DIRS:
                print(f"dir  {os.path.join(d, s)}")
                hits += 1
        for f in files:
            if f.lower() in NAMES:
                p = os.path.join(d, f)
                print(f"file {p} {os.path.getsize(p)}")
                hits += 1
    print(f"## {root}: {hits} hit(s)")
