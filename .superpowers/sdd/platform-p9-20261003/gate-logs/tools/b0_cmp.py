"""GATE agent 4 (RESUME 13; written and committed BEFORE rows 4-6 ran): compare a P9-B0 re-based run dir with its
reference by file SHA-256 and JSON leaf path, listing EVERY differing leaf by path with its class (RELCACHE-ALLOW: "every
allowed leaf listed by JSON path in the record"). Never prints a value, except the two VM identity strings themselves
(build identities, not statistics).

usage: python b0_cmp.py REFERENCE_DIR NEW_DIR --mode ic|nav OUT_TXT

--mode ic (rows 4 / 5, Y-F0 u / w; ruling RELCACHE-ALLOW, extending GATE-RELCACHE; M1d's Release IC adoption list,
`integration-log.md:303-327`): files other than `summary.json` and `train_candidates.jsonl` must be byte-identical
(every payload: `orientations.json`, `recipe.json`, `train_daily_ic.csv`, w's `train_combined.*`,
`train_planned_targets.csv`); in the two JSON documents a differing leaf is allowed only in these classes:
  TIMING    a path ending `/wall_seconds` or `/hash_seconds`, or `/stage_seconds/<stage>`
  COUNTERS  the cache hit / miss counters `/candidate_cache/{hits,misses,legacy_hits,vm_evaluations}` (vm_evaluations is
            `candidates - cache_hits`, the miss count: `atx-impl/src/strategy_ic_runner.cpp:735`),
            `/candidate_cache/ic_results/{hits,misses}`, the per-candidate `signal_cache` / `ic_result_cache` hit|miss
            stamps (`strategy_ic_runner.cpp:641-642`), and `/research_fields/{field_loads,loaded_bytes,
            peak_resident_fields}` (cold Release pass vs warm reference)
  IDENTITY  `/candidate_cache/vm_identity`, and any string leaf whose new value is the reference value with the
            reference's VM identity string replaced by the new side's (the Release identity and its `DIR/<identity>/`
            cache-root paths); the two identity strings are read from `/roles/<i>/candidate_cache/vm_identity`
  M1D-LIST  (agent 4, added after the u pass's first comparison, `yf0u-cmp-pass1.txt`: RELCACHE-ALLOW's fourth class,
            "M1d's written Release IC adoption list where it names a leaf", is the u / w row of
            `root-wave1-merge-report.md:890-891`, which this tool's first pass did not encode) exactly the leaves that
            list names: the cache-root paths `/candidate_cache/{directory,fields_directory}` and
            `/candidate_cache/entries/<i>/{payload,sidecar}` (allowed only when the new path is the reference path with
            `<cache dir>/` -> `<cache dir>/<new VM identity>/` and the same remainder, "same file names"); the identity
            strings `/candidate_cache/ic_results/{identity,key/ic_identity}` (allowed only when new = reference + the
            Release identity suffix), `/candidate_cache/ic_results/subdirectory` and
            `/candidate_cache/entries/<i>/signal_key_sha256` (identity-keyed names; string both sides); the cache state
            counters `/roles/<i>/{verify_bytes,ic_cache_bytes,ic_scratch_bytes}` (integer both sides)
anything else (incl. a leaf on one side only outside these classes) = OUTSIDE (an identity miss, ROOT-1).

--mode nav (row 6, Y-F0 NAV; the form of `x10-cmp.txt` / `nav-identity-yf0.txt`): files other than `summary.json`
byte-identical; `summary.json` differs only in
  PRODUCER  `/producer/build_type`, `/producer/engine_git_sha`
  OPTIN     `/scenarios/<i>/ann_mean_gross` (PRE-7a) and `/scenarios/<i>/info_clock/**` (ALCLOCK-1), on the NEW side only
            (a leaf of these present on the reference side = OUTSIDE)
anything else = OUTSIDE.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import sys
from collections import Counter

TIMING = [re.compile(r"/(wall_seconds|hash_seconds)$"), re.compile(r"/stage_seconds/[^/]+$")]
COUNTERS = [re.compile(r"/candidate_cache/(hits|misses|legacy_hits|vm_evaluations)$"),
            re.compile(r"/candidate_cache/ic_results/(hits|misses)$"),
            re.compile(r"(^|/)(signal_cache|ic_result_cache)$"),
            re.compile(r"/research_fields/(field_loads|loaded_bytes|peak_resident_fields)$")]
PRODUCER = re.compile(r"^/producer/(build_type|engine_git_sha)$")
OPTIN = re.compile(r"^/scenarios/\d+/(ann_mean_gross$|info_clock(/|$))")
MISSING = object()


def sha(p: str) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 22), b""):
            h.update(c)
    return h.hexdigest()


def files(root: str) -> dict[str, str]:
    out = {}
    for dp, _, fn in os.walk(root):
        for f in fn:
            out[os.path.relpath(os.path.join(dp, f), root).replace("\\", "/")] = os.path.join(dp, f)
    return out


def leaves(o, p=""):
    if isinstance(o, dict):
        if not o:
            yield p + "/{}", None
        for k, v in o.items():
            yield from leaves(v, f"{p}/{k}")
    elif isinstance(o, list):
        if not o:
            yield p + "/[]", None
        for i, v in enumerate(o):
            yield from leaves(v, f"{p}/{i}")
    else:
        yield p, o


def identities(doc) -> set[str]:
    return {v for p, v in leaves(doc) if re.search(r"^/roles/\d+/candidate_cache/vm_identity$", p) and isinstance(v, str)}


def classify_ic(path: str, a, b, id_a: str | None, id_b: str | None) -> str:
    if any(r.search(path) for r in TIMING):
        return "TIMING"
    if any(r.search(path) for r in COUNTERS):
        return "COUNTERS"
    if path.endswith("/candidate_cache/vm_identity"):
        return "IDENTITY"
    if (id_a and id_b and id_a != id_b and isinstance(a, str) and isinstance(b, str) and id_a in a
            and a.replace(id_a, id_b) == b):
        return "IDENTITY"
    return m1d_list(path, a, b, id_a, id_b)


CACHE_DIR = "build-equity/mega-candidate-cache-v8-lo3"
M1D_PATHS = re.compile(r"/candidate_cache/(directory|fields_directory|entries/\d+/(payload|sidecar))$")
M1D_SUFFIXED = re.compile(r"/candidate_cache/ic_results/(identity|key/ic_identity)$")
M1D_NAMES = re.compile(r"/candidate_cache/(ic_results/subdirectory|entries/\d+/signal_key_sha256)$")
M1D_COUNTERS = re.compile(r"^/roles/\d+/(verify_bytes|ic_cache_bytes|ic_scratch_bytes)$")


def m1d_list(path: str, a, b, id_a: str | None, id_b: str | None) -> str:
    """M1d's written Release IC adoption list (root-wave1-merge-report.md:890-891), each leaf with its strict check."""
    strs = isinstance(a, str) and isinstance(b, str)
    if M1D_PATHS.search(path) and strs and id_b:
        for sep in ("\\", "/"):
            head = CACHE_DIR + sep
            if a.startswith(head) and b == head + id_b + sep + a[len(head):]:
                return "M1D-LIST"
        return "OUTSIDE"
    if M1D_SUFFIXED.search(path) and strs and id_a and id_b and id_b.startswith(id_a):
        return "M1D-LIST" if b == a + id_b[len(id_a):] else "OUTSIDE"
    if M1D_NAMES.search(path) and strs:
        return "M1D-LIST"
    if M1D_COUNTERS.search(path) and isinstance(a, int) and isinstance(b, int):
        return "M1D-LIST"
    return "OUTSIDE"


def classify_nav(path: str, a, b) -> str:
    if PRODUCER.search(path):
        return "PRODUCER"
    if OPTIN.search(path):
        return "OPTIN" if a is MISSING else "OUTSIDE"
    return "OUTSIDE"


def side(a, b) -> str:
    return "new-only" if a is MISSING else ("reference-only" if b is MISSING else "both")


def main(argv: list[str]) -> int:
    ref, new, mode, out = argv[0], argv[1], argv[argv.index("--mode") + 1], argv[-1]
    lines, counts, outside = [], Counter(), 0
    fa, fb = files(ref), files(new)
    id_a = id_b = None
    if mode == "ic" and "summary.json" in fa and "summary.json" in fb:
        ia = identities(json.load(open(fa["summary.json"], encoding="utf-8")))
        ib = identities(json.load(open(fb["summary.json"], encoding="utf-8")))
        id_a = next(iter(ia)) if len(ia) == 1 else None
        id_b = next(iter(ib)) if len(ib) == 1 else None
        lines.append(f"VM identity: reference {sorted(ia)} | new {sorted(ib)}")
    for rel in sorted(set(fa) | set(fb)):
        if rel not in fa or rel not in fb:
            lines.append(f"ONE-SIDED {rel} ({'new only' if rel in fb else 'reference only'}) OUTSIDE")
            outside += 1
            continue
        sa, sb = sha(fa[rel]), sha(fb[rel])
        if sa == sb:
            lines.append(f"same      {rel} {sa}")
            continue
        docs = []
        if rel.endswith(".json") and (rel == "summary.json" or (mode == "ic" and rel == "train_candidates.jsonl")):
            docs = [("", json.load(open(fa[rel], encoding="utf-8")), json.load(open(fb[rel], encoding="utf-8")))]
        elif mode == "ic" and rel == "train_candidates.jsonl":
            la = open(fa[rel], encoding="utf-8").read().splitlines()
            lb = open(fb[rel], encoding="utf-8").read().splitlines()
            if len(la) != len(lb):
                lines.append(f"DIFF      {rel}: line count {len(la)} vs {len(lb)} OUTSIDE")
                outside += 1
                continue
            docs = [(f"line{k}", json.loads(x), json.loads(y)) for k, (x, y) in enumerate(zip(la, lb), 1)]
        else:
            lines.append(f"DIFF      {rel} {sa[:16]} vs {sb[:16]} (a payload / non-allowed file) OUTSIDE")
            outside += 1
            continue
        file_counts, rows = Counter(), []
        for pre, x, y in docs:
            lx, ly = dict(leaves(x)), dict(leaves(y))
            for k in sorted(set(lx) | set(ly)):
                a, b = lx.get(k, MISSING), ly.get(k, MISSING)
                if a is not MISSING and b is not MISSING and a == b and type(a) is type(b):
                    continue
                cls = classify_ic(k, a, b, id_a, id_b) if mode == "ic" else classify_nav(k, a, b)
                file_counts[cls] += 1
                rows.append(f"    {cls:9s} {pre}{k} ({side(a, b)})")
        counts.update(file_counts)
        outside += file_counts["OUTSIDE"]
        lines.append(f"DIFF      {rel} {sa[:16]} vs {sb[:16]}: {sum(file_counts.values())} leaves "
                     f"{dict(sorted(file_counts.items()))}")
        lines += rows
    lines.append(f"CLASS COUNTS (leaves): {dict(sorted(counts.items()))}")
    lines.append(f"TOTAL outside the allowed classes: {outside}")
    head = f"# b0_cmp.py --mode {mode}: reference {ref} vs new {new}"
    text = head + "\n" + "\n".join(lines) + "\n"
    open(out, "w", encoding="utf-8", newline="\n").write(text)
    print(head)
    for ln in lines:
        if not ln.startswith("    "):
            print(ln)
    return 0 if outside == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
