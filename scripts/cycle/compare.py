"""The cycle's identity checks (research_cycle.py, split move-only into scripts/cycle/ by P9 lane E2): the spec's
``compare`` items, modes file, csv-rows and json-rows (see research_cycle's usage, ``compare``)."""
from __future__ import annotations

import json

from cycle.phases import Cycle, Step
from cycle.resolver import Resolver
from cycle.spec import CycleError
from research_common import sha256_file


def _lines(data: bytes) -> list[bytes]:
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    return lines


def _first_diff(a: list, b: list) -> int:
    return next((k for k, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))


def compare_keys(res: Resolver, rel: str) -> list[str]:
    """The compared keys of a row compare with `keys`: the candidate ids of a library JSON (candidates[].id)."""
    p = res.path(rel)
    try:
        ids = [row["id"] for row in json.loads(p.read_bytes())["candidates"]]
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise CycleError(f"compare keys: no candidates[].id in {rel} ({exc})") from exc
    if not ids or not all(isinstance(i, str) and i for i in ids) or len(set(ids)) != len(ids):
        raise CycleError(f"compare keys: {rel} needs distinct non-empty candidate ids")
    return ids


def compare_files(res: Resolver, c: dict) -> str:
    """One identity comparison (see the module doc, `compare`); returns its summary, raises CycleError on a miss.
    Row modes compare the rows whose key is one of `keys` (a library's candidate ids) when given, else of a's keys."""
    name, mode, a, b = c["name"], c["mode"], c["a"], c["b"]

    def miss(why: str) -> CycleError:
        return CycleError(f"IDENTITY MISMATCH [{name}] ({mode}) {a} vs {b}: {why} -- hard stop, no trial")
    pa, pb = res.path(a), res.path(b)
    for rel, p in ((a, pa), (b, pb)):
        if not p.is_file():
            raise miss(f"{rel} is missing")
    if mode == "file":
        sa, sb = sha256_file(pa), sha256_file(pb)
        if sa != sb:
            raise miss(f"sha256 {sa} != {sb}")
        return f"bit for bit ({pa.stat().st_size} bytes, sha256 {sa})"
    members = compare_keys(res, c["keys"]) if c.get("keys") else None
    if members is not None and c.get("keys_in"):
        also = set(compare_keys(res, c["keys_in"]))
        members = [m for m in members if m in also]
    if mode == "csv-rows":
        la, lb = _lines(pa.read_bytes()), _lines(pb.read_bytes())
        if not la or not lb or la[0] != lb[0]:
            raise miss("headers differ")
        key = c.get("key", "id").encode()
        cols = la[0].split(b",")
        if key not in cols:
            raise miss(f"no key column {key.decode()}")
        k = cols.index(key)

        def key_of(row: bytes) -> bytes:
            parts = row.split(b",")
            return parts[k] if k < len(parts) else b""
        keys = {m.encode() for m in members} if members is not None else {key_of(row) for row in la[1:]}
        ref = [row for row in la[1:] if key_of(row) in keys]
        if {key_of(row) for row in ref} != keys:
            raise miss(f"a lacks rows of {len(keys - {key_of(row) for row in ref})} of the {len(keys)} keys")
        mine = [row for row in lb[1:] if key_of(row) in keys]
        if mine != ref:
            j = _first_diff(ref, mine)
            at = ref[j] if j < len(ref) else b"<end>"
            raise miss(f"{len(ref)} rows of {len(keys)} keys vs {len(mine)} matching rows; first difference at "
                       f"key row {j + 1} ({at[:80].decode(errors='replace')})")
        other = {key_of(row) for row in lb[1:]} - keys
        return (f"{len(ref)} rows of {len(keys)} keys byte for byte (a has {len(la) - 1 - len(ref)} rows of other "
                f"keys); b adds {len(lb) - 1 - len(mine)} rows of {len(other)} other keys")
    array, key = c.get("array", "candidates"), c.get("key", "id")
    try:
        ra, rb = json.loads(pa.read_bytes())[array], json.loads(pb.read_bytes())[array]
        ids = [r[key] for r in ra]
        wanted = set(members) if members is not None else set(ids)
    except (ValueError, KeyError, TypeError) as exc:
        raise miss(f"no {array}[].{key} rows ({exc})") from exc
    if members is None and len(wanted) != len(ids):
        raise miss(f"a lists a {key} twice")
    ra = [r for r in ra if r[key] in wanted]
    if sorted(r[key] for r in ra) != sorted(wanted):
        raise miss(f"a does not list each of the {len(wanted)} keys exactly once")
    canon = lambda r: json.dumps(r, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    sel = [r for r in rb if isinstance(r, dict) and r.get(key) in wanted]
    ca, cb = [canon(r) for r in ra], [canon(r) for r in sel]
    if ca != cb:
        j = _first_diff(ca, cb)
        raise miss(f"{len(ca)} objects vs {len(cb)} matching; first difference at {key} "
                   f"{ra[j][key] if j < len(ra) else '<end>'}")
    return f"{len(ca)} {array} objects identical (canonical JSON); b adds {len(rb) - len(sel)} objects"


def compare(cycle: Cycle, st: Step, log=print) -> None:
    for c in st.checks:
        log(f"compare {c['name']} [{c['mode']}]: {c['a']} vs {c['b']}")
        log(f"   IDENTICAL: {compare_files(cycle.res, c)}")
