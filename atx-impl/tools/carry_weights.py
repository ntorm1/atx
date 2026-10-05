"""Carry a book's TRAIN admission and composition weights to history eras (OD-3 read, Ruling PM7-29 choice 1).

Usage:
  carry_weights.py --weights W --weights-sha256 S --admission A --admission-sha256 S
                   [--era ID ROLE_MANIFEST ROLE_SHA ...] --anchor ROLE_MANIFEST ROLE_SHA --output DIR
                   [--drop-list L --drop-list-sha256 S]

PM7-29 (1): "TRAIN admission, signs and weights are carried back unchanged; no re-admission, no re-fit on history".
This tool replaces the pooled fit of a ``roles:`` cycle (research_roles.py, spec key ``fit.carry``). It publishes in
DIR the files the roles loop expects from a fit. The IC runner then applies the same book to every era.

  composition_weights.json        the bytes of W, unchanged. The anchor (the last role) must be the role W was fitted
                                  on (W's train_manifest_sha256 == the anchor's ROLE_SHA), so that the anchor era
                                  reproduces the TRAIN w pass and NAV byte for byte.
  composition_weights.<ID>.json   for each --era: W's document with train_manifest_sha256 = that era's ROLE_SHA (the
                                  IC runner binds a weights file to its --train role) and a top-level
                                  ``carried_from`` block {rule, weights_sha256, admission_sha256,
                                  train_manifest_sha256}. Written by the pooled fitter's canonical serialisation. The
                                  runner reads no top-level key it does not know, so weights, signs and theme blocks
                                  are W's.
  admission.json                  the bytes of A, unchanged (the TRAIN roster that produced W).
  carry.json                      the record: inputs, anchor, and each era's role SHA and file SHA-256.

--drop-list (P9 lane OD3P; Rulings PRE-20, PRE-31: the "clean-source book" history line): L is an
``atx.od3-flagged-members/v1`` document (scripts/specs/p9/od3/flagged_members.json): ``members``, the flagged member
ids; ``rules``, the flagged rule ids; ``rules_pending``, rule ids whose flag is not yet known (must be empty);
``source``, the file the list was copied from and its SHA-256. Each flagged member is treated as absent on history
with TRAIN weights carried and no re-fit: in every --era file its weight is a pinned 0 (the IC runner needs a weight
for every library candidate, strategy_ic_admission.cpp:667-670, and its composer skips a pinned zero,
strategy_ic_composition.cpp:316-318); every other byte of the document is W's (the kept members' weights, the signs,
the theme blocks), and ``carried_from`` gains a ``drop`` block {rule, list_sha256, source_sha256, members}. The anchor
file stays W's bytes (the drop applies on history; the anchor reproduces the base book's TRAIN cell). Note: a pinned
zero also leaves the theme's mass (the sum of its members' weights, strategy_ic_composition.cpp:256-257, 263-265);
the composer's rule for a member with no data keeps the mass in the theme (task-OD3P-report.md, open question 1). The
base is the clean-source book's TRAIN fit (PRE-31): it must apply no listed rule (no ``rule`` string of W is one).
Refused with the list: an id that is not a member of the book (a key of W's ``weights``), a repeated id, a weights
document that is not v1 / v2 or has no weights object, a ``theme_standardise`` rule other than ew-theme-std-v1 (the
IC runner re-derives and verifies the member weights of ic-shrink-v1, ic-shrink-aim-v1 and theme-erc-v1 from their
block, strategy_ic_rules.cpp:479-493, so it refuses a zeroed member), a non-empty ``rules_pending``, and no --era.
Without --drop-list every output byte is as before.

Everything is checked before anything is written; a refusal (exit 1) publishes nothing. Checks:
- W and A (and L) must match their pins.
- Each era's role manifest must match its pin.
- The anchor must be W's role.
- Era ids must be distinct and of the form [A-Za-z0-9_]+.
- No era may share the anchor's role SHA, and no two eras may share a role SHA.
- W must carry a string train_manifest_sha256.
- DIR must not exist.

Publication is all-or-nothing: the files are written to a pending directory, which is then renamed to DIR. Standard
library only; no return, IC or NAV value is read.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys

HERE = Path(__file__).resolve().parent
ENGINE_TOOLS = HERE.parents[1] / "atx-engine" / "tools"
if str(ENGINE_TOOLS) not in sys.path:
    sys.path.insert(0, str(ENGINE_TOOLS))
import era_pool as EP  # noqa: E402

RULE = "carry-train-weights-v1"
SCHEMA = "atx.carry-weights/v1"
OUTPUT_WEIGHTS = "composition_weights.json"
OUTPUT_ADMISSION = "admission.json"
OUTPUT_RECORD = "carry.json"
DROP_SCHEMA = "atx.od3-flagged-members/v1"           # the drop list (P9 OD3P, PRE-31)
DROP_RULE = "drop-flagged-members-v1"
WEIGHTS_SCHEMAS = ("atx.dsl-composition-weights/v1", "atx.dsl-composition-weights/v2")   # the IC runner's two
ZERO_SAFE_STANDARDISE = ("ew-theme-std-v1",)          # the theme_standardise row without a weight verify
MEMBER_ID = re.compile(r"[A-Za-z0-9_]+")
SHA256 = re.compile(r"[0-9a-f]{64}")


class CarryError(ValueError):
    """A refusal: nothing is published."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_bytes(document: dict) -> bytes:
    """The pooled fitter's serialisation (fit_composition_weights.canonical_bytes)."""
    return (json.dumps(document, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")


def pinned_bytes(path: str, pin: str, what: str) -> bytes:
    p = Path(path)
    if not p.is_file():
        raise CarryError(f"{what} {path} does not exist")
    data = p.read_bytes()
    if sha256_bytes(data) != pin:
        raise CarryError(f"{what} {path} is {sha256_bytes(data)}, pinned {pin}")
    return data


def id_list(doc: dict, key: str, pattern: re.Pattern | None) -> list[str]:
    value = doc.get(key)
    if not isinstance(value, list) or not all(isinstance(x, str) and x for x in value):
        raise CarryError(f"drop list: {key} must be a list of non-empty strings")
    if len(set(value)) != len(value):
        raise CarryError(f"drop list: {key} repeats an id: {sorted({x for x in value if value.count(x) > 1})}")
    bad = [x for x in value if pattern is not None and not pattern.fullmatch(x)]
    if bad:
        raise CarryError(f"drop list: {key} ids must match [A-Za-z0-9_]+: {bad}")
    return value


def drop_list(data: bytes) -> dict:
    """The checked drop list (an atx.od3-flagged-members/v1 document): {members, rules, source_sha256}."""
    try:
        doc = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise CarryError(f"drop list is not JSON: {exc}") from exc
    if not isinstance(doc, dict) or doc.get("schema") != DROP_SCHEMA:
        raise CarryError(f"drop list schema must be {DROP_SCHEMA}")
    members = id_list(doc, "members", MEMBER_ID)
    if not members:
        raise CarryError("drop list: members is empty")
    rules = id_list(doc, "rules", None)
    pending = id_list(doc, "rules_pending", None)
    if pending:
        raise CarryError(f"drop list: rules_pending {pending}: each rule's flag (its registration's "
                         "source_sample_end, 2015 or later = flagged, PRE-31) is resolved into rules or dropped from "
                         "the list before the read")
    source = doc.get("source")
    if not isinstance(source, dict) or not isinstance(source.get("path"), str) or not source["path"] or \
            not isinstance(source.get("sha256"), str) or not SHA256.fullmatch(source["sha256"]):
        raise CarryError("drop list: source must be {path, sha256} (the flag file the list was copied from)")
    return {"members": members, "rules": rules, "source_sha256": source["sha256"]}


def applied_rules(node) -> set[str]:
    """Every string value of a key ``rule`` anywhere in a weights document (its provenance and rule blocks)."""
    found: set[str] = set()
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "rule" and isinstance(value, str):
                found.add(value)
            found |= applied_rules(value)
    elif isinstance(node, list):
        for value in node:
            found |= applied_rules(value)
    return found


def check_drop(doc: dict, drop: dict) -> None:
    """The base book and the list agree (PRE-31): v1 / v2 weights, every listed id a member, no listed rule applied,
    and a composition whose member weights the IC runner does not re-derive."""
    if doc.get("schema") not in WEIGHTS_SCHEMAS or not isinstance(doc.get("weights"), dict):
        raise CarryError(f"a drop list needs a weights object keyed by member id ({' or '.join(WEIGHTS_SCHEMAS)})")
    unknown = [m for m in drop["members"] if m not in doc["weights"]]
    if unknown:
        raise CarryError(f"drop list ids {unknown} are not members of the book (keys of its weights)")
    flagged = sorted(applied_rules(doc) & set(drop["rules"]))
    if flagged:
        raise CarryError(f"the base book applies flagged rule(s) {flagged}: the clean-source book is the last accepted "
                         "book of the lineage that applies no rule whose source_sample_end is 2015 or later (PRE-31)")
    std = doc.get("theme_standardise")
    rule = std.get("rule") if isinstance(std, dict) else None
    if std is not None and rule not in ZERO_SAFE_STANDARDISE:
        raise CarryError(f"theme_standardise rule {rule}: the IC runner re-derives and verifies its member weights "
                         "from the block (strategy_ic_rules.cpp verify rows), so it refuses a pinned zero; dropping "
                         "members of this book needs a runner-side absent-member rule (task-OD3P-report.md, open "
                         "question 1)")


def carry_files(weights: bytes, admission: bytes, eras: list[tuple[str, str]], anchor_sha: str,
                drop: dict | None = None, drop_bytes: bytes | None = None) -> dict[str, bytes]:
    """The published files, by name. ``eras`` is [(id, role_sha)] for the non-anchor eras; ``drop`` the checked
    drop list (drop_list) and ``drop_bytes`` its file, or None. Raises CarryError."""
    try:
        doc = json.loads(weights.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise CarryError(f"weights are not JSON: {exc}") from exc
    if not isinstance(doc, dict) or not isinstance(doc.get("train_manifest_sha256"), str):
        raise CarryError("weights carry no train_manifest_sha256 (the role they were fitted on)")
    fitted_on = doc["train_manifest_sha256"]
    if fitted_on != anchor_sha:
        raise CarryError(f"the anchor role {anchor_sha} is not the role the weights were fitted on ({fitted_on}): the "
                         "anchor (the last role) must reproduce the TRAIN book")
    if "carried_from" in doc:
        raise CarryError("the weights are carried already (carried_from present): carry the TRAIN fit's file")
    ids = [eid for eid, _ in eras]
    if len(set(ids)) != len(ids):
        raise CarryError(f"era ids repeat: {ids}")
    shas = [sha for _, sha in eras]
    if anchor_sha in shas or len(set(shas)) != len(shas):
        raise CarryError("each era is its own role: no era may share the anchor's or another era's role SHA")
    block = {"rule": RULE, "weights_sha256": sha256_bytes(weights), "admission_sha256": sha256_bytes(admission),
             "train_manifest_sha256": fitted_on}
    era_doc = doc
    if drop is not None:
        if not eras:
            raise CarryError("a drop list applies on the history eras: give one or more --era")
        check_drop(doc, drop)
        dropped = set(drop["members"])
        block["drop"] = {"rule": DROP_RULE, "list_sha256": sha256_bytes(drop_bytes), "members": sorted(dropped),
                         "source_sha256": drop["source_sha256"]}
        era_doc = dict(doc, weights={k: (0.0 if k in dropped else v) for k, v in doc["weights"].items()})
    files = {OUTPUT_WEIGHTS: weights, OUTPUT_ADMISSION: admission}
    record = {"schema": SCHEMA, "rule": RULE, "weights_sha256": block["weights_sha256"],
              "admission_sha256": block["admission_sha256"], "anchor_role_sha256": anchor_sha, "eras": []}
    if drop is not None:
        record["drop"] = dict(block["drop"], rules=sorted(drop["rules"]))
    for eid, sha in eras:
        name = EP.era_weights_name(eid)
        files[name] = canonical_bytes(dict(era_doc, train_manifest_sha256=sha, carried_from=block))
        record["eras"].append({"id": eid, "role_sha256": sha, "file": name, "sha256": sha256_bytes(files[name])})
    files[OUTPUT_RECORD] = canonical_bytes(record)
    return files


def publish(out: Path, files: dict[str, bytes]) -> None:
    """All-or-nothing: written to <out>.pending, then renamed (the fitter's publish_directory rule)."""
    out = Path(out)
    if out.exists():
        raise CarryError(f"output exists; refusing overwrite: {out}")
    out.parent.mkdir(parents=True, exist_ok=True)
    pending = out.with_name(out.name + ".pending")
    try:
        pending.mkdir()
    except FileExistsError as exc:
        raise CarryError(f"stale or concurrent partial output; inspect and remove: {pending}") from exc
    try:
        for name in sorted(files):
            with (pending / name).open("xb") as stream:
                stream.write(files[name])
                stream.flush()
                os.fsync(stream.fileno())
        os.rename(pending, out)
    except BaseException:
        shutil.rmtree(pending, ignore_errors=True)
        raise


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--weights", required=True)
    ap.add_argument("--weights-sha256", required=True)
    ap.add_argument("--admission", required=True)
    ap.add_argument("--admission-sha256", required=True)
    ap.add_argument("--era", nargs=3, action="append", default=[], metavar=("ID", "ROLE_MANIFEST", "ROLE_SHA"))
    ap.add_argument("--anchor", nargs=2, required=True, metavar=("ROLE_MANIFEST", "ROLE_SHA"))
    ap.add_argument("--output", required=True)
    ap.add_argument("--drop-list", default=None, help="the flagged members (and rules) of the clean-source line")
    ap.add_argument("--drop-list-sha256", default=None)
    args = ap.parse_args(argv)
    if (args.drop_list is None) != (args.drop_list_sha256 is None):
        ap.error("--drop-list and --drop-list-sha256 go together")
    try:
        weights = pinned_bytes(args.weights, args.weights_sha256, "weights")
        admission = pinned_bytes(args.admission, args.admission_sha256, "admission")
        drop = drop_bytes = None
        if args.drop_list is not None:
            drop_bytes = pinned_bytes(args.drop_list, args.drop_list_sha256, "drop list")
            drop = drop_list(drop_bytes)
        eras = []
        for eid, manifest, sha in args.era:
            try:
                EP.check_id(eid)
            except EP.PoolError as exc:
                raise CarryError(str(exc)) from exc
            pinned_bytes(manifest, sha, f"era {eid} role manifest")
            eras.append((eid, sha))
        pinned_bytes(args.anchor[0], args.anchor[1], "anchor role manifest")
        files = carry_files(weights, admission, eras, args.anchor[1], drop, drop_bytes)
        publish(Path(args.output), files)
    except CarryError as exc:
        print(f"carry_weights: refused: {exc}", file=sys.stderr)
        return 1
    print(json.dumps({"status": "complete", "rule": RULE, "output": args.output,
                      "files": {n: sha256_bytes(b) for n, b in sorted(files.items())}}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
