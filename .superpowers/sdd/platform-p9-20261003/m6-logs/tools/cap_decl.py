"""Root M6: the flag-absent capacity sentence of strategy_nav_v7.cpp at two commits (ALCLOCK-S S-1 (c)).
Prints the SHA-256 of the C++ text of `capacity_declaration` at each commit and whether they are equal.
usage: cap_decl.py OLD NEW"""
import hashlib
import re
import subprocess
import sys

def decl(rev):
    src = subprocess.run(["git", "show", f"{rev}:atx-impl/src/strategy_nav_v7.cpp"], capture_output=True,
                         check=True).stdout.decode("utf-8").replace("\r\n", "\n")
    m = re.search(r"constexpr const char\* capacity_declaration =\n(.*?);\n", src, re.S)
    return m.group(1)

a, b = decl(sys.argv[1]), decl(sys.argv[2])
for rev, t in ((sys.argv[1], a), (sys.argv[2], b)):
    print(rev, hashlib.sha256(t.encode()).hexdigest(), len(t), "chars")
print("capacity_declaration unchanged:", a == b)
