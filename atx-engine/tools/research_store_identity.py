"""Render-identity checker of the research catalog (P9 SQL2; sql-design section 3.9, the stage-2 evidence).

For each catalogued file of a class that carries a render rule in ``classes.json`` (``render: {mode, rule}``), the
checker rebuilds the file's bytes from its catalog rows alone, with the writer's own Python expression, and compares
them with the SHA-256 the catalog recorded for the file (``artifact.sha256``) and, given ``--root``, with the bytes on
disk now. Zero mismatches over a class is the evidence that the rows hold everything the writer wrote, so a later
stage may write rows first and render the file from them (sql-design section 4).

Modes (``classes.json``):
  typed  the document's top-level keys from typed columns, ``extra`` (undeclared keys, or keys whose value does not
         fit the column; ``extra`` wins) and child rows (``run_command`` -> ``command``, ``run_binding`` ->
         ``bindings``), in the stored ``key_order``; a table without ``key_order`` renders with sorted keys and has
         required typed keys only (``stage_receipt``)
  doc    the stored ``doc`` (the document as parsed, key order kept)
  lines  one ``trial_line.line`` per line (exact bytes; each line must also be in the rule's form)
Rules (the writers' expressions):
  py-indent2               json.dumps(doc, indent=2) + "\\n"
  py-indent2-sorted        json.dumps(doc, indent=2, sort_keys=True, allow_nan=False) + "\\n"
  py-indent2-noascii       json.dumps(doc, indent=2, ensure_ascii=False) + "\\n"
  py-compact-sorted-lines  json.dumps(rec, sort_keys=True, separators=(",", ":")) + "\\n" per line
The line end is the one the catalog read into ``artifact.eol`` (``lf`` or ``crlf``; ``mixed`` / ``none`` cannot be
rendered and are mismatches), so the check is host-independent. Floats render exactly: the catalog stores the double a
Python ``repr`` text parsed to, and ``json.dumps`` writes ``repr`` again.

Reads rows only through SQL1's generic accessor (``research_store.Store``); writes nothing. A mismatch names the path,
the class and a reason built from digests, byte offsets and key names, never a document value: a verdict or wave result
holds statistics (blind rule), and the checker's output stays printable. Paths holding a standalone year token
2024-2099 are never opened (the catalog's seal backstop), nor files over 16 MiB.

Usage:
  python research_store_identity.py --catalog DB --root R [--classes ID[,ID...]] [--registry FILE]
Exit 0 no mismatch, 1 mismatches, 2 usage or an unreadable store / registry.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

import research_store as rs

REPO = Path(__file__).resolve().parents[2]
DEFAULT_REGISTRY = REPO / "atx-engine" / "schemas" / "research_store" / "classes.json"
REGISTRY_SCHEMA = "atx.research-store-classes/v1"
MAX_OPEN_BYTES = 16 * 1024 * 1024
_SEALED = re.compile(r"(^|\D)20(2[4-9]|[3-9]\d)(\D|$)")
EXIT_OK, EXIT_MISMATCH, EXIT_USAGE = 0, 1, 2

DOC_RULES = {
    "py-indent2": lambda doc: json.dumps(doc, indent=2) + "\n",
    "py-indent2-sorted": lambda doc: json.dumps(doc, indent=2, sort_keys=True, allow_nan=False) + "\n",
    "py-indent2-noascii": lambda doc: json.dumps(doc, indent=2, ensure_ascii=False) + "\n",
}
LINE_RULES = {
    "py-compact-sorted-lines": lambda rec: json.dumps(rec, sort_keys=True, separators=(",", ":")),
}
EOLS = {"lf": "\n", "crlf": "\r\n"}


@dataclass(frozen=True)
class Typed:
    """A typed table: document key -> column, keys rebuilt from child rows, whether ``key_order`` is stored."""
    columns: dict
    children: tuple = ()
    ordered: bool = True


# The column each document key fills (atx-engine research/store/catalog ingest_run.cpp, ingest_stage.cpp,
# ingest_cycle.cpp). A key absent here and from extra cannot be rendered (a mismatch, not a guess).
TYPED = {
    "run": Typed(columns={
        "schema": "receipt_schema", "source_sha": "source_sha", "started_utc": "started_utc",
        "executable_sha256": "executable_sha256", "argv_sha256": "argv_sha256", "attempt": "attempt",
        "build_type": "build_type", "limits": "limits",
        "sampled_peak_tree_rss_bytes": "sampled_peak_tree_rss_bytes",
        "minimum_system_free_bytes": "minimum_system_free_bytes", "outcome": "outcome", "exit_code": "exit_code",
        "git": "git_state", "dirty_outside_pathspec": "dirty_outside", "role_id": "role_id", "admission": "admission",
        "error": "error", "wall_seconds": "wall_seconds", "owned_processes": "owned_processes",
        "ownership_scope": "ownership_scope", "logs": "logs", "store": "store"},
        children=("command", "bindings")),
    "stage_receipt": Typed(columns={
        "schema": "schema", "chain": "chain", "stage": "stage", "index": "stage_index", "status": "status",
        "inputs": "inputs", "outputs": "outputs", "started_utc": "started_utc", "seconds": "seconds"},
        ordered=False),
    "cycle_binding": Typed(columns={
        "schema": "schema", "output": "output", "spec_sha256": "spec_sha256", "spec_rule": "spec_rule",
        "argv_sha256": "argv_sha256"}),
}
DOC_TABLES = frozenset({"run_start", "cycle_verdict", "wave_result", "candidate"})
LINE_TABLES = frozenset({"trial_line"})


class RenderError(ValueError):
    """The rows cannot rebuild the file (missing rows, an unrenderable key, an unknown rule or line end)."""


@dataclass(frozen=True)
class Mismatch:
    path: str
    cls: str
    reason: str

    def __str__(self) -> str:
        return f"{self.path} [{self.cls}]: {self.reason}"


# -- registry -------------------------------------------------------------------------------------------------------
def load_registry(path=DEFAULT_REGISTRY) -> dict:
    """``classes.json`` as {class id: class}; refuses another document id or an unknown render mode / rule."""
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(doc, dict) or doc.get("schema") != REGISTRY_SCHEMA:
        raise ValueError(f"{path}: not a {REGISTRY_SCHEMA} document")
    classes = {}
    for c in doc.get("classes", []):
        render = c.get("render")
        if render is not None:
            mode, rule = render.get("mode"), render.get("rule")
            known = LINE_RULES if mode == "lines" else DOC_RULES
            if mode not in ("typed", "doc", "lines") or rule not in known:
                raise ValueError(f"{path}: class {c.get('id')}: unknown render {render!r}")
        classes[c["id"]] = c
    return classes


def rendered_classes(registry: dict) -> list:
    """The class ids that carry a render rule, in registry order."""
    return [cid for cid, c in registry.items() if c.get("render") is not None]


# -- paths ----------------------------------------------------------------------------------------------------------
def path_key(path: str) -> str:
    """The catalog's path key: ASCII lower case only (seal_guard.cpp path_key)."""
    return "".join(chr(ord(ch) + 32) if "A" <= ch <= "Z" else ch for ch in path)


def has_sealed_year(path: str) -> bool:
    return _SEALED.search(path) is not None


def _parent(path: str) -> str:
    return path.rsplit("/", 1)[0] if "/" in path else ""


def _leaf(path: str) -> str:
    return path.rsplit("/", 1)[-1]


def _where(table: str, path: str) -> dict:
    """The rows of ``table`` that the file at ``path`` produced (the family's key, ingest_*.cpp)."""
    if table in ("run", "run_start", "cycle_binding"):
        return {"run_dir": _parent(path)}
    if table == "stage_receipt":
        return {"state_dir": _parent(_parent(path)), "file_name": _leaf(path)}
    if table == "trial_line":
        return {"ledger_path": path}
    return {"path": path}


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# -- rendering ------------------------------------------------------------------------------------------------------
def _one_row(store: rs.Store, table: str, path: str) -> dict:
    rows = store.select(table, _where(table, path))
    if len(rows) != 1:
        raise RenderError(f"{len(rows)} {table} rows for the file (expected 1)")
    return rows[0]


def _json_column(table: rs.Table, column: str, value):
    if value is not None and table.by_name[column].type == "json":
        return json.loads(value)
    return value


def typed_document(store: rs.Store, table: str, row: dict) -> dict:
    """The document a typed row (with its child rows) stands for."""
    spec = TYPED[table]
    t = store.table(table)
    extra = json.loads(row["extra"]) if row.get("extra") is not None else {}
    if not isinstance(extra, dict):
        raise RenderError(f"{table}.extra is not a JSON object")
    children = {}
    if "command" in spec.children:
        children["command"] = [r["arg"] for r in store.select("run_command", {"run_dir": row["run_dir"]})]
    if "bindings" in spec.children:
        children["bindings"] = [{"path": r["path"], "sha256": r["sha256"]}
                                for r in store.select("run_binding", {"run_dir": row["run_dir"]})]
    if spec.ordered:
        keys = json.loads(row["key_order"])
        if not isinstance(keys, list) or len(set(keys)) != len(keys):
            raise RenderError(f"{table}.key_order is not a list of distinct keys")
    else:
        keys = list(dict.fromkeys([*spec.columns, *extra]))

    def value(key: str):
        if key in extra:
            return extra[key]
        if key in children:
            return children[key]
        column = spec.columns.get(key)
        if column is None:
            raise RenderError(f"key {key!r} has no column and is not in {table}.extra")
        return _json_column(t, column, row[column])

    return {key: value(key) for key in keys}


def _render_lines(store: rs.Store, rule: str, path: str) -> tuple:
    rows = store.select("trial_line", {"ledger_path": path})
    if not rows:
        raise RenderError("no trial_line rows for the file")
    if [r["seq"] for r in rows] != list(range(1, len(rows) + 1)):
        raise RenderError("trial_line seq is not 1..n")
    for r in rows:
        if _sha256(r["line"].encode("utf-8")) != r["line_sha256"]:
            raise RenderError(f"line {r['seq']}: line_sha256 differs from the stored line")
        if LINE_RULES[rule](json.loads(r["line"])) != r["line"]:
            raise RenderError(f"line {r['seq']}: not in {rule} form")
    state = store.get("ledger_state", {"ledger_path": path})
    if state is None:
        raise RenderError("no ledger_state row for the file")
    if state["lines"] != len(rows):
        raise RenderError(f"ledger_state.lines {state['lines']} != {len(rows)} trial_line rows")
    return "".join(r["line"] + "\n" for r in rows), state["file_sha256"]


def render(store: rs.Store, info: dict, artifact: dict) -> tuple:
    """(bytes, file SHA-256 the family row recorded) of the catalogued file ``artifact`` of class ``info``."""
    render_spec = info.get("render")
    if render_spec is None:
        raise RenderError("the class has no render rule")
    mode, rule = render_spec["mode"], render_spec["rule"]
    table, path = info.get("ingest"), artifact["path"]
    if mode == "lines":
        if table not in LINE_TABLES:
            raise RenderError(f"mode lines over table {table!r}")
        text, recorded = _render_lines(store, rule, path)
    else:
        row = _one_row(store, table, path)
        if mode == "typed":
            if table not in TYPED:
                raise RenderError(f"mode typed over table {table!r}")
            doc = typed_document(store, table, row)
        elif table in DOC_TABLES:
            doc = json.loads(row["doc"])
        else:
            raise RenderError(f"mode doc over table {table!r}")
        text, recorded = DOC_RULES[rule](doc), row["file_sha256"]
    eol = EOLS.get(artifact.get("eol"))
    if eol is None:
        raise RenderError(f"line end {artifact.get('eol')!r} cannot be rendered")
    return text.replace("\n", eol).encode("utf-8"), recorded


def _first_difference(a: bytes, b: bytes) -> int:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return min(len(a), len(b))


def _read_file(root: Path, path: str) -> bytes:
    if has_sealed_year(path):
        raise RenderError("sealed path (year token 2024-2099): never opened")
    file = Path(root) / path
    if not file.is_file():
        raise RenderError("no such file under the root")
    if file.stat().st_size > MAX_OPEN_BYTES:
        raise RenderError("file over 16 MiB: never opened")
    return file.read_bytes()


# -- checks ---------------------------------------------------------------------------------------------------------
def check_artifact(store: rs.Store, registry: dict, artifact: dict, *, root=None) -> Mismatch | None:
    """Check one ``artifact`` row (see ``check_one``)."""
    path, cls = artifact["path"], artifact["class"]
    info = registry.get(cls)
    if info is None:
        return Mismatch(path, cls, "class not in the registry")
    if artifact["sha_source"] != "verified":
        return Mismatch(path, cls, f"sha_source {artifact['sha_source']} (the catalog never hashed the file)")
    try:
        data, recorded = render(store, info, artifact)
        rendered = _sha256(data)
        if recorded != artifact["sha256"]:
            return Mismatch(path, cls, f"row file_sha256 {recorded} != artifact sha256 {artifact['sha256']} "
                                       "(stale row)")
        if rendered != artifact["sha256"]:
            return Mismatch(path, cls, f"rendered sha256 {rendered} != catalogued {artifact['sha256']}")
        if root is not None:
            on_disk = _read_file(Path(root), path)
            if on_disk != data:
                at = _first_difference(on_disk, data)
                return Mismatch(path, cls, f"file on disk differs from the render at byte {at} (file "
                                           f"{len(on_disk)} bytes, sha256 {_sha256(on_disk)}; render "
                                           f"{len(data)} bytes)")
    except RenderError as exc:
        return Mismatch(path, cls, str(exc))
    except (ValueError, KeyError, TypeError) as exc:   # a stored JSON text that does not parse, a missing column
        return Mismatch(path, cls, f"rows unreadable: {type(exc).__name__}")
    return None


def check_one(db, path: str, cls: str | None = None, *, root=None, registry=None) -> Mismatch | None:
    """Render the catalogued file ``path`` (root-relative) from its rows; ``None`` when identical, else a ``Mismatch``.

    ``db`` is an open ``research_store.Store`` or a catalog path (opened and closed here); ``cls`` the class the caller
    expects (``None``: the catalogued class); ``root`` adds the compare with the bytes on disk; ``registry`` a loaded
    ``classes.json`` (``load_registry``; default the repo's).
    """
    registry = load_registry() if registry is None else registry
    if not isinstance(db, rs.Store):
        with rs.Store.open(db) as store:
            return check_one(store, path, cls, root=root, registry=registry)
    path = str(path).replace("\\", "/")
    artifact = db.get("artifact", {"path_key": path_key(path)})
    if artifact is None:
        return Mismatch(path, cls or "?", "not catalogued (no artifact row)")
    if cls is not None and artifact["class"] != cls:
        return Mismatch(path, cls, f"catalogued as class {artifact['class']}")
    return check_artifact(db, registry, artifact, root=root)


def check_catalog(store: rs.Store, registry: dict, classes=None, *, root=None) -> tuple:
    """({class: files checked}, [Mismatch]) over every catalogued file of ``classes`` (default: every rendered
    class), in class then path-key order."""
    classes = rendered_classes(registry) if classes is None else list(classes)
    counts, mismatches = {}, []
    for cls in classes:
        rows = store.select("artifact", {"class": cls})
        counts[cls] = len(rows)
        for artifact in rows:
            found = check_artifact(store, registry, artifact, root=root)
            if found is not None:
                mismatches.append(found)
    return counts, mismatches


def main(argv=None, out=None) -> int:
    out = sys.stdout if out is None else out
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--catalog", required=True, type=Path, help="the catalog DB (atx-research-store init)")
    parser.add_argument("--root", required=True, type=Path, help="the tree the catalog run walked")
    parser.add_argument("--classes", action="append", default=None,
                        help="class ids to check (comma-separated, repeatable; default every rendered class)")
    parser.add_argument("--registry", type=Path, default=DEFAULT_REGISTRY, help="classes.json")
    args = parser.parse_args(argv)
    try:
        registry = load_registry(args.registry)
    except (OSError, ValueError) as exc:
        print(f"research_store_identity: registry: {exc}", file=sys.stderr)
        return EXIT_USAGE
    classes = None
    if args.classes:
        classes = [c for item in args.classes for c in item.split(",") if c]
        unknown = [c for c in classes if c not in registry or registry[c].get("render") is None]
        if unknown:
            print(f"research_store_identity: not a rendered class: {', '.join(unknown)}", file=sys.stderr)
            return EXIT_USAGE
    try:
        with rs.Store.open(args.catalog) as store:
            counts, mismatches = check_catalog(store, registry, classes, root=args.root)
    except rs.StoreError as exc:
        print(f"research_store_identity: {exc}", file=sys.stderr)
        return EXIT_USAGE
    by_class = {}
    for m in mismatches:
        by_class[m.cls] = by_class.get(m.cls, 0) + 1
    for cls, n in counts.items():
        print(f"class {cls} checked {n} mismatches {by_class.get(cls, 0)}", file=out)
    for m in mismatches:
        print(f"mismatch {m}", file=out)
    print(f"total checked {sum(counts.values())} mismatches {len(mismatches)}", file=out)
    return EXIT_OK if not mismatches else EXIT_MISMATCH


if __name__ == "__main__":
    raise SystemExit(main())
