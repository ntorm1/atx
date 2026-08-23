#!/usr/bin/env python3
"""Build atx-vol/bench/oracle/cohorts/breadth.json (coverage-expansion cohort).

Construction only: writes the cohort file; runs no measurement and wires no
gate preset.

Candidate members (hard-coded below) target the audited coverage gaps from
the 2026-08-23 breadth plan (scratchpad breadth-plan-20260823.md; ledger
2026-08-23 data facts):

  gap 4  index families with zero coverage .. NDX x {1000,1300,1530},
                                              RUT x {1000,1300,1530}
  gap 7  dividend-heavy (no monthly payer
         in tune) ........................... DIA x {1300}, MSFT x {1300}
  gap 5  high-vol semis (mean srVol > 0.8) .. SNDK x {1300}, AMD x {1300}
  gap 8  sub-$5 underlier (tune floor
         $22.19) ............................ EOSE x {1300}
  gap 9  negative-carry index ............... MXEF x {1300}
  gap 2  open / last-print / post-close
         bucket regimes ..................... SPY x {0940,1555,1600},
                                              NDX x {0940,1600}

All tickers verified present as undSecKey_tk in the 2026-08-14 store scan
(covbreadth underlier_rows.json): NDX 84,542 rows (+NDXP 221,381 roll-up),
RUT 46,413 (+RUTW 180,533), DIA 90,131, MSFT 59,047, SNDK 183,702,
AMD 101,044, EOSE 10,808, MXEF 52,816, SPY 257,683.

Cohort reader semantics (oracle_cohort_reader.cpp): a cohort file's
effective membership is the FULL cross product dates x underliers x
buckets_et -- the schema cannot express a ragged member list.  The
candidate tuples therefore seed the axes, and holdout disjointness is
enforced on the written rectangle:

  1. holdout.json is loaded silently; its membership (same rectangular
     schema per cohorts/README.md) is expanded to (tk, bucket, date)
     tuples with weekly roots normalized (SPXW->SPX, RUTW->RUT,
     NDXP->NDX), matching the reader's roll-up.
  2. Any candidate tuple inside the holdout rectangle is removed.
  3. Rectangle closure: the rectangle spanned by the surviving
     candidates' axes is re-checked against holdout; while any collision
     remains, one axis element (bucket or underlier) is removed by a
     deterministic greedy rule (fewest surviving candidates lost, then
     most collisions resolved, then bucket before underlier, then
     lexicographic), and the candidates using it are dropped.
  4. The final rectangle is asserted disjoint from holdout before the
     file is written.

CONFIDENTIALITY CONTRACT: holdout.json membership is secret.  This script
is the only thing allowed to read it, and it emits ONLY three aggregate
counts (candidates_in, collisions_removed, final_count).  It never prints
holdout content, which candidates collided, or parse-error detail.

Output formatting is deterministic: sorted, deduplicated axes; 2-space
indent with inline arrays matching tune.json's style; LF newlines.
"""

import json
import sys
from pathlib import Path

DATE = "2026-08-14"

# (undSecKey_tk, bucket_et, date) seed tuples -- rationale in the docstring.
CANDIDATES = [
    ("NDX", "0940", DATE),
    ("NDX", "1000", DATE),
    ("NDX", "1300", DATE),
    ("NDX", "1530", DATE),
    ("NDX", "1600", DATE),
    ("RUT", "1000", DATE),
    ("RUT", "1300", DATE),
    ("RUT", "1530", DATE),
    ("DIA", "1300", DATE),
    ("MSFT", "1300", DATE),
    ("SNDK", "1300", DATE),
    ("AMD", "1300", DATE),
    ("EOSE", "1300", DATE),
    ("MXEF", "1300", DATE),
    ("SPY", "0940", DATE),
    ("SPY", "1555", DATE),
    ("SPY", "1600", DATE),
]

# Weekly-root roll-up used by the cohort reader (oracle_cohort_reader matches
# undSecKey_tk after normalization; ledger 2026-08-23).
ROLLUP = {"SPXW": "SPX", "RUTW": "RUT", "NDXP": "NDX"}

NOTES = (
    "Additive coverage-expansion cohort for srPrc/srVol convergence breadth "
    "(construction only; smoke/tune/holdout untouched). Targets the audited "
    "2026-08-23 gaps: zero-coverage index families NDX (305,923 valid rows "
    "incl NDXP roll-up, 55.3% wide-spread) and RUT (226,946 incl RUTW); the "
    "open/last-print/post-close bucket regimes (0940/1555/1600 candidates, "
    "SPY and NDX anchors -- the buckets_et array above is what survived "
    "holdout subtraction); monthly-payer DIA (ddiv-frac 96.8%, 90,131 rows) and "
    "quarterly mega-cap MSFT (59,047); high-vol semis SNDK (mean srVol 1.03, "
    "183,702) and AMD (0.83, 101,044); sub-$5 EOSE (10,808; tune price floor "
    "is $22.19); negative-carry index MXEF (52,816, most extreme non-tune "
    "borrow). Reader semantics make membership the full dates x underliers x "
    "buckets_et cross product, so off-seed pairs (e.g. DIA x 0940) are "
    "members too -- deliberate extra breadth. Built by "
    "scripts/oracle-breadth-cohort.py, which seeds these axes from a "
    "documented candidate list, silently subtracts holdout collisions "
    "(weekly roots normalized), and emits only aggregate counts; the written "
    "rectangle is verified disjoint from holdout membership. Weekly roots "
    "roll up (SPXW->SPX, RUTW->RUT, NDXP->NDX), so only canonical roots are "
    "listed. This is a generalization baseline: no tuning against it until "
    "it is blessed as a tune-class cohort."
)


def die(msg: str) -> None:
    # Sanitized abort: msg must never contain holdout file content.
    sys.stderr.write("error: " + msg + "\n")
    sys.exit(2)


def norm(tk: str) -> str:
    return ROLLUP.get(tk, tk)


def load_holdout_members(path: Path) -> set:
    """Load holdout.json and expand its rectangular membership to normalized
    (tk, bucket, date) tuples.  Emits nothing; all failures are sanitized."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except Exception:
        die("holdout cohort file could not be read or parsed (detail suppressed)")
    if not isinstance(doc, dict):
        die("holdout cohort file has an unexpected shape (detail suppressed)")
    axes = {}
    for key in ("dates", "underliers", "buckets_et"):
        val = doc.get(key)
        if not isinstance(val, list) or not all(isinstance(x, str) for x in val):
            die("holdout cohort file has an unexpected shape (detail suppressed)")
        axes[key] = val
    return {
        (norm(tk), bucket, date)
        for tk in axes["underliers"]
        for bucket in axes["buckets_et"]
        for date in axes["dates"]
    }


def rectangle(cands: set) -> set:
    unds = {norm(tk) for tk, _, _ in cands}
    bkts = {b for _, b, _ in cands}
    dts = {d for _, _, d in cands}
    return {(u, b, d) for u in unds for b in bkts for d in dts}


def subtract_holdout(cands: set, holdout: set) -> set:
    # Phase 1: drop seed tuples that are holdout members.
    cands = {c for c in cands if (norm(c[0]), c[1], c[2]) not in holdout}
    # Phase 2: rectangle closure -- the written file is read as the full
    # cross product of the surviving axes, so filler tuples must be
    # holdout-disjoint too.  Shrink axes deterministically until they are.
    while cands:
        collisions = rectangle(cands) & holdout
        if not collisions:
            break
        options = []
        for bucket in sorted({b for _, b, _ in collisions}):
            lost = sum(1 for c in cands if c[1] == bucket)
            resolved = sum(1 for t in collisions if t[1] == bucket)
            options.append((lost, -resolved, 0, bucket))
        for und in sorted({u for u, _, _ in collisions}):
            lost = sum(1 for c in cands if norm(c[0]) == und)
            resolved = sum(1 for t in collisions if t[0] == und)
            options.append((lost, -resolved, 1, und))
        _, _, axis, victim = min(options)
        if axis == 0:
            cands = {c for c in cands if c[1] != victim}
        else:
            cands = {c for c in cands if norm(c[0]) != victim}
    return cands


def format_cohort(cands: set) -> str:
    unds = sorted({tk for tk, _, _ in cands})
    bkts = sorted({b for _, b, _ in cands})
    dts = sorted({d for _, _, d in cands})

    def arr(values) -> str:
        return "[" + ", ".join(json.dumps(v) for v in values) + "]"

    lines = [
        "{",
        '  "name": "breadth",',
        '  "dates": {},'.format(arr(dts)),
        '  "underliers": {},'.format(arr(unds)),
        '  "buckets_et": {},'.format(arr(bkts)),
        '  "notes": {}'.format(json.dumps(NOTES)),
        "}",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    if len(CANDIDATES) != len(set(CANDIDATES)):
        die("duplicate candidate tuples")
    root = Path(__file__).resolve().parent.parent
    cohorts = root / "atx-vol" / "bench" / "oracle" / "cohorts"
    holdout = load_holdout_members(cohorts / "holdout.json")

    candidates_in = len(CANDIDATES)
    final = subtract_holdout(set(CANDIDATES), holdout)
    if not final:
        die("no candidates survive holdout subtraction; refusing to write an empty cohort")
    remaining = rectangle(final) & holdout
    if remaining:
        die("internal error: written rectangle still collides with holdout")

    out = cohorts / "breadth.json"
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(format_cohort(final))

    print("candidates_in={}".format(candidates_in))
    print("collisions_removed={}".format(candidates_in - len(final)))
    print("final_count={}".format(len(final)))


if __name__ == "__main__":
    main()
