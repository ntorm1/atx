"""GATE agent 4: per-class, per-collapsed-path counts of a b0_cmp.py output (paths only, no value).
usage: python class_summary.py CMP_TXT OUT_TXT"""
import collections, re, sys
c = collections.Counter()
for line in open(sys.argv[1], encoding="utf-8"):
    if line.startswith("    "):
        cls, path = line.split()[0], line.split()[1]
        c[(cls, re.sub(r"line\d+", "line<k>", re.sub(r"/\d+(?=/|$)", "/<i>", path)))] += 1
rows = [f"{v}\t{k[0]}\t{k[1]}" for k, v in sorted(c.items())]
open(sys.argv[2], "w", encoding="utf-8", newline="\n").write(
    f"# class summary of {sys.argv[1].replace(chr(92), '/').split('/')[-1]}: count, class, collapsed JSON path\n" + "\n".join(rows) + "\n")
print(f"{len(rows)} collapsed paths, {sum(c.values())} leaves")
