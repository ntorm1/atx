"""ALD-DIAG (Ruling OD5-1 (iii), ALD-10; root M2g item 2): a zero-trial universe diagnostic on the sealed role
`train-2020-2023-lo3`. Universe and delisting metadata only: no return, price or NAV statistic.

Per calendar year 2020-2023 it prints:
  names      the number of distinct role instruments that are universe members (member.u8 == 1) on at least one
             role session of that year;
  delisted   the number of universe exits through a delisting whose last session is on or before 2023-12-01: role
             delisting terminations (`universe.delisting.events`, the role builder's own list) whose line was a
             universe member at its last session (`kept_member_at_last_session`), by last-session year, also split by
             the stage's cause (`delist_code`);
  unclassified  the number of such exits whose last session is after 2023-12-01: printed as "unclassified (within 30
             days of the seal)" (ALD-10: the stage's "continued" flag and its classification clock may look up to 30
             days past the last session, so the last month's exits are not classified; the count is a lower bound).
Context lines: terminations of lines that were NOT members at their last session (not universe exits), terminations
with a last session before 2020 (warm-up), the stage's own counts block, and a consistency check (member.u8 at the
event's last session for every classified exit).

What it opens: the role manifest (pinned by SHA-256), and the payloads sessions.i64, ids.u64 and member.u8, each
hash-verified against the manifest's `files` entry. It never opens close.f64, raw_close.f64 or volume.f64. The
delisting events' value keys (`delist_return`, `delist_return_if_performance`) and the by-cause `with_return` counts
are dropped on load and never printed (era_data_audit.py's contract). It refuses a role whose session axis reaches
2024-01-01 before any payload is opened.

usage: python ald_diag.py ROLE_MANIFEST ROLE_SHA256   (cwd = the pool-2 root)
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import sys
from collections import Counter
from pathlib import Path

import numpy as np

SEAL = dt.date(2024, 1, 1)
CLASSIFY_LAST = "2023-12-01"            # ALD-10
YEARS = (2020, 2021, 2022, 2023)
NEVER_OPENED = ("close.f64", "raw_close.f64", "volume.f64")
DROP_KEYS = ("delist_return", "delist_return_if_performance")
CAUSES = ("mna", "performance", "non_common", "unknown", "unknown_unlinked")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def payload(role_dir: Path, files: dict, name: str, dtype: str, count: int) -> np.ndarray:
    assert name not in NEVER_OPENED, name
    path = role_dir / name
    got = sha256(path)
    if got != files[name]["sha256"]:
        raise SystemExit(f"ald_diag: {name} sha256 {got} != manifest pin {files[name]['sha256']}")
    arr = np.fromfile(path, dtype=dtype)
    if arr.size != count:
        raise SystemExit(f"ald_diag: {name} has {arr.size} values, expected {count}")
    return arr


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        raise SystemExit(__doc__)
    manifest_path, pin = Path(argv[0]), argv[1]
    got = sha256(manifest_path)
    if got != pin:
        raise SystemExit(f"ald_diag: role manifest sha256 {got} != pin {pin}")
    m = json.loads(manifest_path.read_text(encoding="utf-8"))
    deli = m["universe"]["delisting"]
    for e in deli["events"]:
        for k in DROP_KEYS:
            e.pop(k, None)
    for v in deli["by_cause"].values():
        v.pop("with_return", None)
    role_dir = manifest_path.parent
    d, n = int(m["dates"]), int(m["instruments"])
    files = m["files"]

    sessions = payload(role_dir, files, "sessions.i64", "<i8", d)
    days = [dt.datetime.fromtimestamp(int(s) / 1e9, tz=dt.timezone.utc).date() for s in sessions]
    if max(days) >= SEAL:
        raise SystemExit(f"ald_diag: role session {max(days)} reaches the seal {SEAL}: refused")
    ids = payload(role_dir, files, "ids.u64", "<u8", n)
    member = payload(role_dir, files, "member.u8", "u1", d * n).reshape(d, n)   # date-major
    col = {int(x): j for j, x in enumerate(ids)}
    row = {day.isoformat(): i for i, day in enumerate(days)}

    print("ALD-DIAG (OD5-1 (iii), ALD-10): universe and delisting metadata of the sealed role; no return statistic")
    print(f"role {manifest_path.as_posix()}  sha256 {pin}")
    print(f"universe {m['universe']['id']}  sessions {d} ({days[0]} .. {days[-1]})  instruments {n}")
    print(f"delisting stage rule {deli['stage_rule']}; returns_applied {deli['returns_applied']}")
    print("payloads hash-verified: sessions.i64 ids.u64 member.u8 (close / raw_close / volume never opened)")
    print()

    events = deli["events"]
    exits = [e for e in events if e["kept_member_at_last_session"]]
    others = [e for e in events if not e["kept_member_at_last_session"]]
    hdr = (f"{'year':<6}{'names':>8}{'delisted<=2023-12-01':>22}{'share':>8}"
           f"{'unclassified(>2023-12-01)':>27}  delisted by cause (" + ", ".join(CAUSES) + ")")
    print(hdr)
    tot_names = set()
    rows_out = []
    for y in YEARS:
        idx = [i for i, day in enumerate(days) if day.year == y]
        names = int(np.count_nonzero(member[idx, :].any(axis=0)))
        tot_names |= set(np.flatnonzero(member[idx, :].any(axis=0)).tolist())
        ys = [e for e in exits if e["last_session"][:4] == str(y)]
        classified = [e for e in ys if e["last_session"] <= CLASSIFY_LAST]
        unclassified = [e for e in ys if e["last_session"] > CLASSIFY_LAST]
        by = Counter(e["delist_code"] for e in classified)
        share = len(classified) / names if names else float("nan")
        cause = ", ".join(str(by.get(c, 0)) for c in CAUSES)
        unc = f"{len(unclassified)} (within 30 days of the seal)" if y == 2023 else str(len(unclassified))
        print(f"{y:<6}{names:>8}{len(classified):>22}{share:>8.3f}{unc:>27}  ({cause})")
        rows_out.append((y, names, len(classified), len(unclassified)))
    print()
    print(f"distinct universe names 2020-2023: {len(tot_names)}; universe exits through a delisting 2020-2023: "
          f"{sum(r[2] for r in rows_out)} classified + {sum(r[3] for r in rows_out)} unclassified")
    pre = [e for e in exits if e["last_session"][:4] < "2020"]
    print(f"context: universe-exit terminations with a last session before 2020 (warm-up): {len(pre)}")
    oth = Counter(e["last_session"][:4] for e in others)
    print("context: terminations of lines NOT universe members at their last session (not universe exits), by year: "
          + ", ".join(f"{k} {oth[k]}" for k in sorted(oth)))
    print("context: the stage's counts block: " + ", ".join(f"{k} {v}" for k, v in deli["counts"].items()))
    unknown_codes = sorted({e["delist_code"] for e in events} - set(CAUSES))
    print(f"context: delist_code values outside the five causes: {unknown_codes or 'none'}")
    # consistency: a classified exit's line is a member at its last session in member.u8
    bad = missing = 0
    for e in exits:
        i, j = row.get(e["last_session"]), col.get(int(e["security_id"]))
        if i is None or j is None:
            missing += 1
        elif member[i, j] != 1:
            bad += 1
    print(f"check: member.u8 == 1 at the last session for {len(exits) - bad - missing} of {len(exits)} universe-exit "
          f"events; {bad} not a member; {missing} without a role session / id")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
