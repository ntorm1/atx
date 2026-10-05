"""COVR: the stage table of the report from the four CovStageDigest dumps (synthetic only).

  python covr_table.py debug.dump release.dump debug-swap-exp2.dump release-swap-exp2.dump

Per recipe and stage: a fold (SHA-256, first 12 hex) of that stage's per-session digests over all 300 sessions for each
dump, the number of sessions where Debug and Release differ and the first one. Markdown on stdout.
"""
import hashlib
import sys
from collections import OrderedDict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from covr_cmp import read_dump  # noqa: E402


def folds(d: "OrderedDict[str, str]", recipe: str):
    out: "OrderedDict[str, list[tuple[int, str]]]" = OrderedDict()
    for k, v in d.items():
        p = k.split("/")
        if len(p) == 4 and p[0] == "stage" and p[1] == recipe:
            out.setdefault(p[3], []).append((int(p[2]), v))
    return out


def fold(rows) -> str:
    return hashlib.sha256("".join(v for _, v in sorted(rows)).encode()).hexdigest()[:12]


def main(paths: list[str]) -> int:
    dumps = [read_dump(p) for p in paths]
    labels = ["Debug", "Release", "Debug + ucrtbase exp2", "Release + ucrtbased exp2"]
    dbg, rel = dumps[0], dumps[1]
    print("| key | " + " | ".join(labels) + " |")
    print("|---|" + "---|" * len(labels))
    for k in dbg:
        if k.startswith("const/exp2") or k.startswith("iso/"):
            print(f"| `{k}` | " + " | ".join(f"`{d.get(k, '-')[:16]}`" for d in dumps) + " |")
    for recipe in ("v11", "covv1"):
        print(f"\n**{recipe}** (fold of 300 per-session digests; sessions Debug != Release; first)\n")
        print("| stage | " + " | ".join(labels) + " | differ | first |")
        print("|---|" + "---|" * (len(labels) + 2))
        fs = [folds(d, recipe) for d in dumps]
        for stage in fs[0]:
            a, b = dict(fs[0][stage]), dict(fs[1][stage])
            diff = sorted(t for t in a if a[t] != b.get(t))
            print(f"| {stage} | " + " | ".join(f"`{fold(f[stage])}`" for f in fs) +
                  f" | {len(diff)} | {diff[0] if diff else '-'} |")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    raise SystemExit(main(sys.argv[1:]))
