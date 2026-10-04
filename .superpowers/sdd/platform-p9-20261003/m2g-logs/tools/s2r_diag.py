"""S2R-DIAG (Ruling S2R-2; root M2g item 6): for the book candidates `smax` and `smax5` on the P9-B0 u, the number
of (name, day) cells that are NaN under AuditExact and finite under ResearchFast. Signal payloads only (raw,
unoriented, pre-composition candidate signals; atx.dsl-candidate-signal/v2 sidecars): no return, IC or NAV statistic.

ResearchFast side: the P9-B0 u's own cache entries (`build-equity/mega-candidate-cache-v8-lo3`, Debug VM identity
`dslvm1_clang18.1`, the entries the u pass `p9-b0-yf0-u` hit). AuditExact side: the entries an `--eval-mode
audit-exact` u pass (same argv, a fresh short cache dir) wrote under `<vm identity>_audit`. Each payload is verified
against its sidecar's `payload_sha256`; the two sidecars must agree on role, geometry, DSL SHA-256 and candidate id.
Counts (no value is printed): all cells; the score window (role sessions >= score_begin); and as context the reverse
split (finite AuditExact, NaN ResearchFast), both NaN, both finite.

usage: python s2r_diag.py RF_CACHE_ROOT AE_CACHE_ROOT ROLE_MANIFEST   (each cache root = the dir holding <role sha>/)
"""
from __future__ import annotations

import glob
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

IDS = ("smax", "smax5")


def sha(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def entry(root: Path, role_sha: str, cid: str) -> tuple[dict, np.ndarray]:
    hits = [Path(p) for p in glob.glob(str(root / role_sha / f"{cid}.*.json"))]
    if len(hits) != 1:
        raise SystemExit(f"s2r_diag: {cid}: {len(hits)} sidecars under {root}")
    side = json.loads(hits[0].read_text(encoding="utf-8"))
    pay = hits[0].with_name(side["payload"])
    if sha(pay) != side["payload_sha256"]:
        raise SystemExit(f"s2r_diag: {pay} does not match its sidecar")
    arr = np.fromfile(pay, dtype="<f8").reshape(int(side["dates"]), int(side["instruments"]))
    return side, arr


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        raise SystemExit(__doc__)
    rf_root, ae_root, role_path = Path(argv[0]), Path(argv[1]), Path(argv[2])
    role_sha = sha(role_path)
    role = json.loads(role_path.read_text(encoding="utf-8"))
    score_begin = int(role["score_begin"])
    print("S2R-DIAG (S2R-2): smax / smax5 cells NaN under AuditExact and finite under ResearchFast (counts only)")
    print(f"role {role_path.as_posix()} sha256 {role_sha}; score window = role sessions >= index {score_begin}")
    for cid in IDS:
        rs, rf = entry(rf_root, role_sha, cid)
        as_, ae = entry(ae_root, role_sha, cid)
        for k in ("role_manifest_sha256", "dates", "instruments", "dsl_sha256", "candidate_id"):
            if rs[k] != as_[k]:
                raise SystemExit(f"s2r_diag: {cid}: sidecars disagree on {k}")
        print(f"\n{cid}: dsl {rs['dsl_sha256'][:16]}; RF eval_mode '{rs['eval_mode']}' vm {rs['vm_identity']}; "
              f"AE eval_mode '{as_['eval_mode']}' vm {as_['vm_identity']}")
        for label, sl in (("all sessions", slice(None)), ("score window", slice(score_begin, None))):
            a, r = ae[sl], rf[sl]
            na, nr = ~np.isfinite(a), ~np.isfinite(r)
            print(f"  {label:13s} cells {a.size}: NaN AE & finite RF {int(np.count_nonzero(na & ~nr))}; "
                  f"finite AE & NaN RF {int(np.count_nonzero(~na & nr))}; both NaN {int(np.count_nonzero(na & nr))}; "
                  f"both finite {int(np.count_nonzero(~na & ~nr))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
