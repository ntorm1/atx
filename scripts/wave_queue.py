"""The candidate queue (platform v8 lane YINFRA): what signal lanes write instead of prose tables.

One JSON file per candidate, ``scripts/specs/v8/candidates/<id>.json`` (schema ``atx.wave-candidate/v1``): the
registration fields of a wave candidate (wave_manifest.CANDIDATE_*: id, frozen DSL and its SHA-256, theme, tier, prior
sign, citation, origin, hypothesis id, add or replace, ...) plus

  status   proposed -> pinned (the PM) -> screened | admitted | dropped | in-book (the wave's record stage:
           screened = no cell, or kept but not admitted by the gate (weight 0); admitted / in-book = admitted and
           in the cell, the cell accepted for in-book; dropped = the sign rule dropped it)
           proposed -> dropped (withdrawn); every later status is final (a re-test is a new variant with a ruling)
  wave     the wave that consumed it (null until then)
  history  [{status, at, by, wave?, note?}] every transition, in order

  research_cycle.py candidates new      --id X --dsl "..." --theme T --tier B --prior-sign 1 --citation C
                                        --origin prior --hypothesis H --by LANE [--kind replace --replaces ID
                                        [--rescreen]] [--removes ID] [--fields F,G] [--ruling R] [--note N]
                                        [--source-sample-end YYYY] [--predicted-mechanism TEXT] [--data-class H|W|P|N]
  research_cycle.py candidates validate [--dir D]
  research_cycle.py candidates list     [--status S] [--dir D]
  research_cycle.py candidates pin      --id X [--id Y ...] --by PM [--ruling R] [--dir D]
                                        (--by one of PIN_ROLES, any case: only the PM, root or the owner pins)
  research_cycle.py candidates emit     --head HEAD.json --select X,Y,... --output MANIFEST [--after RESULT]

validate refuses: a file that is not a valid registration, an id that is not its file name, a status outside the
lifecycle, a DSL SHA-256 queued twice or registered in the alpha registry under another id (or an id registered with
another DSL), and a second variant of one hypothesis id unless that variant carries the ruling that allowed it.
emit writes a wave manifest (wave_manifest.py) from HEAD (every manifest key but candidates) and the selected PINNED
candidates in the order given; --after RESULT takes parent and expect.n_before from a previous wave-result.json (its
next_parent and ledger.n_after: N advanced by code). Nothing is overwritten; every write is validated first.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import research_tree  # noqa: E402
import wave_manifest as WM  # noqa: E402

SCHEMA = "atx.wave-candidate/v1"
QUEUE_DIR = "scripts/specs/v8/candidates"
REGISTRY = "atx-impl/strategies/alphas/registry.json"
PROPOSED, PINNED, SCREENED, ADMITTED, DROPPED, IN_BOOK = "proposed", "pinned", "screened", "admitted", "dropped", \
    "in-book"
STATUSES = (PROPOSED, PINNED, SCREENED, ADMITTED, DROPPED, IN_BOOK)
NEXT = {PROPOSED: (PINNED, DROPPED), PINNED: (SCREENED, ADMITTED, DROPPED, IN_BOOK)}   # the rest are final
CONSUMED = (SCREENED, ADMITTED, IN_BOOK)          # set by a wave: carry its id
QUEUE_KEYS = ("schema", "status", "wave", "history")
PIN_ROLES = ("pm", "root", "owner")               # who may pin (P9 E1): `candidates pin --by` is checked against it


class QueueError(ValueError):
    pass


def queue_dir(root: Path, d: str | None = None) -> Path:
    return Path(root) / (d or QUEUE_DIR)


def load(root: Path, d: str | None = None) -> dict[str, dict]:
    """{id: candidate document} of every file in the queue dir (unvalidated; ``problems`` validates)."""
    out = {}
    q = queue_dir(root, d)
    for p in sorted(q.glob("*.json")) if q.is_dir() else []:
        try:
            out[p.stem] = json.loads(p.read_text(encoding="utf-8"))
        except ValueError as exc:
            out[p.stem] = {"__error__": str(exc)}
    return out


def registry_index(root: Path) -> tuple[dict, dict]:
    """({alpha id: dsl sha256}, {dsl sha256: alpha id}) of the alpha registry under the root ({} without one)."""
    p = Path(root) / REGISTRY
    if not p.is_file():
        return {}, {}
    alphas = json.loads(p.read_text(encoding="utf-8")).get("alphas") or []
    by_id = {a["id"]: WM.dsl_sha256(a["dsl"]) for a in alphas if isinstance(a, dict) and "dsl" in a}
    return by_id, {v: k for k, v in by_id.items()}


def doc_problems(cid: str, c) -> list[str]:
    where = f"{QUEUE_DIR}/{cid}.json"
    if not isinstance(c, dict) or "__error__" in c:
        return [f"{where}: not JSON ({(c or {}).get('__error__')})"]
    out = WM.candidate_problems(c, where, QUEUE_KEYS)
    if c.get("schema") != SCHEMA:
        out.append(f"{where}: schema must be {SCHEMA}")
    if c.get("id") != cid:
        out.append(f"{where}: id {c.get('id')!r} is not the file name")
    if c.get("status") not in STATUSES:
        out.append(f"{where}: status {c.get('status')!r} is not one of {', '.join(STATUSES)}")
    if (c.get("status") in CONSUMED and not c.get("wave")) or (c.get("status") in (PROPOSED, PINNED) and c.get("wave")):
        out.append(f"{where}: a wave sets {', '.join(CONSUMED)} and names itself; proposed and pinned name no wave "
                   "(dropped names the wave that dropped it, none when withdrawn)")
    if not isinstance(c.get("history"), list) or not c["history"] or c["history"][-1].get("status") != c.get("status"):
        out.append(f"{where}: history must end with the current status")
    return out


def problems(root: Path, docs: dict[str, dict]) -> list[str]:
    """Every problem of the queue (empty: valid): each file, duplicate DSLs, the registry, hypothesis variants."""
    out = [p for cid, c in docs.items() for p in doc_problems(cid, c)]
    good = {cid: c for cid, c in docs.items() if isinstance(c, dict) and isinstance(c.get("dsl_sha256"), str)}
    seen: dict[str, str] = {}
    for cid, c in good.items():
        if c["dsl_sha256"] in seen:
            out.append(f"{cid}: DSL sha256 {c['dsl_sha256'][:16]} is queued already as {seen[c['dsl_sha256']]}")
        seen.setdefault(c["dsl_sha256"], cid)
    reg_id, reg_dsl = registry_index(root)
    for cid, c in good.items():
        if cid in reg_id and reg_id[cid] != c["dsl_sha256"]:
            out.append(f"{cid}: the alpha registry holds {cid} with another DSL (a registered alpha is immutable)")
        twin = reg_dsl.get(c["dsl_sha256"])
        if twin is not None and twin != cid:
            out.append(f"{cid}: the same DSL is registered as {twin}")
    hyp: dict[str, list] = {}
    for cid, c in good.items():
        hyp.setdefault(c.get("hypothesis"), []).append((first_at(c), cid, c))
    for h, rows in hyp.items():
        for _, cid, c in sorted(rows, key=lambda r: (r[0], r[1]))[1:]:
            if not c.get("ruling"):
                out.append(f"{cid}: a second variant of hypothesis {h!r} ({', '.join(r[1] for r in rows)}) needs the "
                           "ruling that allows it (ruling)")
    return out


def first_at(c: dict) -> str:
    h = c.get("history") or [{}]
    return str(h[0].get("at") or "")


def write(root: Path, c: dict, d: str | None = None, *, new: bool = False) -> Path:
    p = queue_dir(root, d) / f"{c['id']}.json"
    if new and p.exists():
        raise QueueError(f"{p} exists: a candidate is proposed once")
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(c, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    return p


def transition(c: dict, status: str, by: str, at: str, wave: str | None = None, note: str | None = None) -> dict:
    if status not in NEXT.get(c["status"], ()):
        raise QueueError(f"{c['id']}: {c['status']} -> {status} is not a queue transition (allowed: "
                         f"{', '.join(NEXT.get(c['status'], ())) or 'none, the status is final'})")
    out = dict(c, status=status, wave=wave or c.get("wave"))
    entry = {"status": status, "at": at, "by": by}
    if wave:
        entry["wave"] = wave
    if note:
        entry["note"] = note
    out["history"] = list(c["history"]) + [entry]
    return out


def checked(root: Path, docs: dict, d: str | None = None) -> None:
    bad = problems(root, docs)
    if bad:
        raise QueueError("queue refused: " + "; ".join(bad))


# ------------------------------------------------------------------ the wave's record stage
def outcome_status(cid: str, result: dict) -> str:
    """The queue status a wave result gives one of its candidates: dropped by the sign rule; screened when there is no
    cell or the gate did not admit it (kept at weight 0, PM7-35); else admitted, or in the book when the cell was
    accepted."""
    sc = result.get("screen") or {}
    if cid in (sc.get("dropped") or []):
        return DROPPED
    if result.get("cell") is None:
        return SCREENED
    row = next((r for r in sc.get("rows") or [] if r.get("id") == cid), None)
    if row is not None and row.get("status") != "admitted":
        return SCREENED
    return IN_BOOK if (result.get("verdict") or {}).get("accepted") else ADMITTED


def record_wave(root: Path, manifest: dict, result: dict, at: str, d: str | None = None) -> list[str]:
    """Set the queue status of the wave's candidates that are queued (pinned) from its result; returns the ids set.
    A queued candidate in another state, or with another DSL, is refused (the queue and the manifest disagree)."""
    docs = load(root, d)
    changed = []
    for c in manifest.get("candidates") or []:
        q = docs.get(c["id"])
        if q is None:
            continue
        if q.get("dsl_sha256") != c["dsl_sha256"]:
            raise QueueError(f"{c['id']}: the queue's DSL sha256 differs from the wave manifest's")
        if q.get("status") == outcome_status(c["id"], result) and q.get("wave") == manifest["wave"]:
            continue                                   # a resumed record stage: already set
        docs[c["id"]] = transition(q, outcome_status(c["id"], result), "research_cycle.py wave", at,
                                   wave=manifest["wave"], note=f"wave-result {result['verdict'].get('accepted')}")
        changed.append(c["id"])
    checked(root, docs, d)
    for cid in changed:
        write(root, docs[cid], d)
    return changed


def queue_check(root: Path, manifest: dict, d: str | None = None) -> list[str]:
    """Preflight: every manifest candidate that is queued is PINNED with the manifest's DSL (empty: consistent)."""
    docs, out = load(root, d), []
    for c in manifest.get("candidates") or []:
        q = docs.get(c["id"])
        if q is None:
            continue
        if q.get("dsl_sha256") != c["dsl_sha256"]:
            out.append(f"candidate {c['id']}: the queue holds another DSL sha256")
        if q.get("status") != PINNED:
            out.append(f"candidate {c['id']}: queue status {q.get('status')!r}, not pinned (the PM pins first)")
    return out


# ------------------------------------------------------------------ emit
def emit(root: Path, head: dict, select: list[str], after: dict | None = None, d: str | None = None) -> dict:
    """The wave manifest of the selected pinned candidates (validated; see the module doc)."""
    if "candidates" in head or "rule_cell" in head:
        raise QueueError("the head holds every manifest key but candidates (and no rule_cell)")
    docs = load(root, d)
    checked(root, docs, d)
    missing = [i for i in select if i not in docs]
    if missing or len(set(select)) != len(select) or not select:
        raise QueueError(f"select: distinct queued ids needed (not queued: {missing})")
    unpinned = [i for i in select if docs[i]["status"] != PINNED]
    if unpinned:
        raise QueueError(f"select: {', '.join(unpinned)} not pinned (the PM pins: candidates pin)")
    m = dict(head, schema=WM.SCHEMA, candidates=[WM.registration(docs[i]) for i in select])
    if after is not None:
        m["parent"] = dict(after["next_parent"])
        m["expect"] = {"n_before": after["ledger"]["n_after"]}
    bad = WM.validate(m)
    if bad:
        raise QueueError("emitted manifest refused: " + "; ".join(bad))
    return m


# ------------------------------------------------------------------ CLI
def today() -> str:
    return dt.date.today().isoformat()


def new_doc(a) -> dict:
    reg = {"id": a.id, "dsl": a.dsl, "dsl_sha256": WM.dsl_sha256(a.dsl), "theme": a.theme, "tier": a.tier,
           "prior_sign": a.prior_sign, "citation": a.citation, "origin": a.origin, "hypothesis": a.hypothesis,
           "kind": a.kind}
    for key in ("replaces", "removes"):
        if getattr(a, key):
            reg[key] = list(getattr(a, key))
    if a.rescreen:
        reg["rescreen"] = True
    if a.fields:
        reg["fields"] = [f for f in a.fields.split(",") if f]
    for key in ("ruling", "prior_sign_source", "form", "formula", "domain", "deviation", "lane", "report",
                *WM.REGISTRATION_KEYS):
        if getattr(a, key, None):
            reg[key] = getattr(a, key)
    entry = {"status": PROPOSED, "at": a.at, "by": a.by}
    if a.note:
        entry["note"] = a.note
    return dict(reg, schema=SCHEMA, status=PROPOSED, wave=None, history=[entry])


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="research_cycle.py candidates", description=__doc__.split("\n", 1)[0],
                                 epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("verb", choices=("new", "validate", "list", "pin", "emit"))
    ap.add_argument("--root", type=Path, default=research_tree.REPO)
    ap.add_argument("--dir", default=None, help=f"the queue dir (default {QUEUE_DIR})")
    ap.add_argument("--id", action="append", default=[])
    ap.add_argument("--dsl")
    ap.add_argument("--theme")
    ap.add_argument("--tier")
    ap.add_argument("--prior-sign", type=int)
    ap.add_argument("--citation")
    ap.add_argument("--origin")
    ap.add_argument("--hypothesis")
    ap.add_argument("--kind", default="add", choices=WM.KINDS)
    ap.add_argument("--replaces", action="append", default=None)
    ap.add_argument("--rescreen", action="store_true")
    ap.add_argument("--removes", action="append", default=None)
    ap.add_argument("--fields", default=None, help="F,G,...: the fields the DSL reads (checked against the fields)")
    for key in ("ruling", "prior-sign-source", "form", "formula", "domain", "deviation", "lane", "report", "note"):
        ap.add_argument(f"--{key}", default=None)
    ap.add_argument("--source-sample-end", default=None, help="new (K-P9-11): YYYY, the source paper's last sample year")
    ap.add_argument("--predicted-mechanism", default=None, help="new (K-P9-11): one line")
    ap.add_argument("--data-class", default=None, choices=WM.DATA_CLASSES, help="new (K-P9-11): lit section 3.1")
    ap.add_argument("--by", default=None, help=f"new: the proposing lane; pin: the PM (one of {', '.join(PIN_ROLES)})")
    ap.add_argument("--at", default=today(), help="YYYY-MM-DD of the transition (default today)")
    ap.add_argument("--status", default=None, choices=STATUSES)
    ap.add_argument("--head", type=Path)
    ap.add_argument("--select", default=None, help="X,Y,...: the pinned candidates of the wave, in roster order")
    ap.add_argument("--after", type=Path, default=None, help="a previous wave-result.json: parent and expect from it")
    ap.add_argument("--output", type=Path)
    a = ap.parse_args(argv)
    root = a.root
    try:
        if a.verb == "validate":
            docs = load(root, a.dir)
            checked(root, docs, a.dir)
            print(f"candidates: {len(docs)} valid in {a.dir or QUEUE_DIR}")
            return 0
        if a.verb == "list":
            for cid, c in load(root, a.dir).items():
                if a.status is None or c.get("status") == a.status:
                    print(f"{cid:28s} {c.get('status', '?'):9s} {c.get('wave') or '-':10s} {c.get('theme')} "
                          f"{c.get('hypothesis')} {str(c.get('dsl_sha256'))[:12]}")
            return 0
        if a.verb == "new":
            need = ("dsl", "theme", "tier", "prior_sign", "citation", "origin", "hypothesis", "by")
            if len(a.id) != 1 or any(getattr(a, k) is None for k in need):
                ap.error("new needs one --id and --" + ", --".join(k.replace("_", "-") for k in need))
            a.id = a.id[0]
            doc = new_doc(a)
            docs = dict(load(root, a.dir), **{a.id: doc})
            checked(root, docs, a.dir)
            print(f"proposed {write(root, doc, a.dir, new=True).as_posix()} (dsl sha256 {doc['dsl_sha256']})")
            return 0
        if a.verb == "pin":
            if not a.id or not a.by:
                ap.error("pin needs --id (repeatable) and --by")
            if a.by.strip().lower() not in PIN_ROLES:
                raise QueueError(f"pin --by {a.by!r}: only {' / '.join(PIN_ROLES)} pins (P9 E1)")
            docs = load(root, a.dir)
            for cid in a.id:
                if cid not in docs:
                    raise QueueError(f"{cid} is not queued")
                docs[cid] = transition(docs[cid], PINNED, a.by, a.at, note=a.ruling and f"ruling {a.ruling}")
            checked(root, docs, a.dir)
            for cid in a.id:
                print(f"pinned {write(root, docs[cid], a.dir).as_posix()}")
            return 0
        if not (a.head and a.select and a.output):
            ap.error("emit needs --head, --select and --output")
        head = json.loads(a.head.read_text(encoding="utf-8"))
        after = json.loads(a.after.read_text(encoding="utf-8")) if a.after else None
        m = emit(root, head, [s for s in a.select.split(",") if s], after, a.dir)
        out = a.output if a.output.is_absolute() else root / a.output
        if out.exists():
            raise QueueError(f"{out} exists: a wave manifest is written once")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(m, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
        print(f"wave manifest {out.as_posix()}: {len(m['candidates'])} candidates; commit it, then "
              f"research_cycle.py wave run {a.output.as_posix()}")
        return 0
    except (QueueError, WM.WaveError, OSError, ValueError) as exc:
        print(f"research_cycle candidates: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
