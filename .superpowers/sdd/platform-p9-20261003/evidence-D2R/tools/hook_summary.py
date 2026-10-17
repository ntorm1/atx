"""Summarise a D2R hook log: argvs that reached the fitter's parser and every non-exact option token."""
import collections
import json
import sys

recs, errors = [], []
for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            r = json.loads(line)
            (errors if "hook_error" in r else recs).append(r)
uniq = {json.dumps(r["argv"]) for r in recs}
flag_sets = {tuple(sorted({t.split("=", 1)[0] for t in r["argv"] if t.startswith("--")})) for r in recs}
print(f"records {len(recs)}, unique argvs {len(uniq)}, distinct option sets {len(flag_sets)}, "
      f"tests {len({r['test'] for r in recs})}, pids {len({r['pid'] for r in recs})}, hook errors {len(errors)}")
callers = collections.Counter()
for r in recs:
    frames = [f for f in r["stack"] if "fit_composition_weights.py" not in f and "argparse" not in f
              and "sitecustomize" not in f]
    callers[frames[-1].rsplit(":", 2)[0].split("\\")[-1].split("/")[-1] if frames else "?"] += 1
print("calling files:", dict(callers.most_common()))
bad = [r for r in recs if r["nonexact"]]
print(f"records with a non-exact option token: {len(bad)}")
by = collections.defaultdict(list)
for r in bad:
    for t in r["nonexact"]:
        kind = "abbrev(unique)" if len(t["matches"]) == 1 else ("ambiguous" if t["matches"] else "unknown")
        by[(t["token"], kind, tuple(t["matches"]))].append((r["test"] or "?").split(" (")[0])
for (tok, kind, matches), tests in sorted(by.items()):
    print(f"  {tok!r} {kind} -> {list(matches)}: {len(tests)} x, tests {sorted(set(tests))}")
