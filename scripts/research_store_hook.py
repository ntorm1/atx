"""The record-store writer hook and the run receipt's store block (P9 SQL3; sql-design sections 3.9, 3.10 and 4;
rulings SQL-2, SQL-6, SQL-11).

``record(root, path, cls)``: every Python writer of a stage-2 record class calls it once, right after it has written
``path``. The hook is on for ``root`` when ``<root>/build-equity/research-store/catalog.sqlite`` and
``<root>/build-equity/bin/atx-research-store.exe`` both exist (checked once per process per root; a test root never
has them). Then it runs ``atx-research-store ingest --catalog DB --class CLS --path REL --root ROOT --classes FILE``
(60 s, output captured; FILE is this repository's ``classes.json``, so ingest and check read one registry) and checks
that the catalog's rows re-render the file's bytes (``research_store_identity.check_one``; for a class without a render
rule, that the ``artifact`` row holds that class and the file's SHA-256). Any failure prints ONE line to stderr:

  store: WARNING <cls> <root-relative path>: <why> (the JSON stands; repair: atx-research-store catalog --rebuild)

The JSON stays the authority (SQL-2): the hook never raises, never changes an exit code or a byte of the file, writes
nothing beside it and retries no writer. With the catalog off it does nothing: no subprocess, and neither ``sqlite3``
nor the identity module is imported. Candidates stay keyed by id in P9 (ruling W3-4): a candidate id queued again in a
second queue dir is refused by ingest (exit 3, one warning); so is a renamed file until ``catalog --rebuild``.

``selection(root, argv)``: the store choice the bounded runner writes into ``start.json`` and ``receipt.json`` (ruling
SQL-6: a choice selected by file presence is recorded) as the key ``store``, after every other key:

  {"schema": "atx.run-store/v1",
   "catalog": {"path": "build-equity/research-store/catalog.sqlite", "exe_sha256": <sha256 of the exe>} | null,
   "indexes": [<root-relative POSIX path of every record-store index the command could select>, ...]}

``indexes``: for each argv token (and the value of a ``--flag=value`` token) naming an existing directory ``d`` (a
relative token is read against the root, the child's cwd), the index ``record_store.RecordStore(d)`` selects
(``d/index.sqlite``, else ``d/../index.sqlite``) and every ``d/*/index.sqlite`` (the fitter's store roots under its
work dir); sorted. None when the catalog is off and no index is found: the key is absent and both files keep today's
bytes. The runner's hook enables itself only through this recorded selection (what the receipt says is what ran). No
P9 cell uses an index (ruling SQL-11); the block makes a hand-made one visible.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
import sys

SCHEMA = "atx.run-store/v1"
CATALOG_REL = "build-equity/research-store/catalog.sqlite"
EXE_REL = "build-equity/bin/atx-research-store.exe"
INDEX_NAME = "index.sqlite"        # record_store.INDEX_NAME (not imported here: record_store loads sqlite3)
REPO = Path(__file__).resolve().parents[1]
CLASSES = REPO / "atx-engine" / "schemas" / "research_store" / "classes.json"
ENGINE_TOOLS = REPO / "atx-engine" / "tools"
INGEST_SECONDS = 60
REPAIR = "the JSON stands; repair: atx-research-store catalog --rebuild"

_catalogs: dict[str, dict | None] = {}   # resolved root -> catalog_of(root), once per process per root
_registry: dict | None = None            # classes.json as research_store_identity.load_registry reads it
_UNSET = object()


def warn(cls: str, rel: str, why: str) -> None:
    """The hook's one stderr line for a failure (never raises)."""
    try:
        print(f"store: WARNING {cls} {rel}: {' '.join(str(why).split())} ({REPAIR})", file=sys.stderr)
    except Exception:                        # noqa: BLE001  (a closed stderr changes nothing either)
        pass


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def catalog_of(root) -> dict | None:
    """{"path": CATALOG_REL, "exe_sha256"} when the catalog and the store exe both exist under ``root``, else None;
    decided once per process per root. A catalog whose exe cannot be read is off, with one warning."""
    key = str(Path(root).resolve())
    if key not in _catalogs:
        base, found = Path(key), None
        try:
            if (base / CATALOG_REL).is_file() and (base / EXE_REL).is_file():
                found = {"path": CATALOG_REL, "exe_sha256": sha256_file(base / EXE_REL)}
        except Exception as exc:             # noqa: BLE001
            warn("catalog", EXE_REL, f"the store exe cannot be read, the hook is off ({type(exc).__name__}: {exc})")
        _catalogs[key] = found
    found = _catalogs[key]
    return None if found is None else dict(found)


def _rel(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix() if path.is_relative_to(root) else path.as_posix()


def _argv_dirs(root: Path, argv) -> list[Path]:
    """The existing directories the argv tokens (and ``--flag=value`` values) name, resolved."""
    out = []
    for token in argv:
        token = str(token)
        values = [token]
        if token.startswith("--") and "=" in token:
            values.append(token.split("=", 1)[1])
        for value in values:
            if not value:
                continue
            try:
                p = Path(value)
                p = (p if p.is_absolute() else root / p).resolve()
                if p.is_dir():
                    out.append(p)
            except (OSError, ValueError, RuntimeError):
                continue                     # not a path this host can name: not a directory
    return out


def indexes(root, argv) -> list[str]:
    """The record-store index files the command ``argv`` could select (see the module doc), sorted root-relative."""
    root = Path(root).resolve()
    found: set[Path] = set()
    for d in _argv_dirs(root, argv):
        own, parent = d / INDEX_NAME, d.parent / INDEX_NAME
        if own.is_file():
            found.add(own)
        elif parent.is_file():
            found.add(parent)
        found.update(p for p in d.glob("*/" + INDEX_NAME) if p.is_file())
    return sorted({_rel(root, p) for p in found})


def selection(root, argv) -> dict | None:
    """The run receipt's ``store`` block for a command run under ``root`` (module doc); None: no block. Never raises
    (a failure is one warning and no block, so the hook stays off for the run)."""
    try:
        catalog, found = catalog_of(root), indexes(root, argv)
    except Exception as exc:                 # noqa: BLE001
        warn("run-store", "selection", f"{type(exc).__name__}: {exc}")
        return None
    if catalog is None and not found:
        return None
    return {"schema": SCHEMA, "catalog": catalog, "indexes": found}


def record(root, path, cls: str, *, selected=_UNSET) -> None:
    """Ingest the file ``path`` (absolute, or relative to ``root``) just written as class ``cls`` and check its rows,
    when the catalog is on (module doc). ``selected``: a run's recorded ``store`` block (or None), which alone decides
    whether the hook is on for that run (the bounded runner); absent, ``catalog_of(root)`` decides. Never raises."""
    rel = str(path)
    try:
        base = Path(root).resolve()
        catalog = catalog_of(base) if selected is _UNSET else (selected or {}).get("catalog")
        if catalog is None:
            return
        p = Path(path)
        p = (p if p.is_absolute() else base / p).resolve()
        rel = _rel(base, p)
        if not p.is_relative_to(base):
            warn(cls, rel, "outside the root: not ingested")
            return
        why = ingest(base, rel, cls, catalog) or check(base, rel, cls, catalog)
        if why:
            warn(cls, rel, why)
    except Exception as exc:                 # noqa: BLE001  (the JSON stands whatever the hook meets)
        warn(cls, rel, f"{type(exc).__name__}: {exc}")


def ingest(root: Path, rel: str, cls: str, catalog: dict) -> str | None:
    """Run ``atx-research-store ingest`` on one file; None when it exits 0, else why not."""
    argv = [str(root / EXE_REL), "ingest", "--catalog", str(root / catalog["path"]), "--class", cls, "--path", rel,
            "--root", str(root), "--classes", str(CLASSES)]
    try:
        done = subprocess.run(argv, cwd=root, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=INGEST_SECONDS)
    except subprocess.TimeoutExpired:
        return f"ingest timed out after {INGEST_SECONDS} s"
    except OSError as exc:
        return f"ingest did not start ({exc})"
    if done.returncode == 0:
        return None
    tail = ((done.stderr or "") + (done.stdout or "")).strip().splitlines()
    return f"ingest exit {done.returncode}" + (f" ({tail[-1][:240]})" if tail else "")


def registry() -> dict:
    """classes.json, loaded once (research_store_identity.load_registry)."""
    global _registry
    if _registry is None:
        _registry = identity().load_registry(CLASSES)
    return _registry


def identity():
    """The render-identity module (imported only when the hook is on)."""
    if str(ENGINE_TOOLS) not in sys.path:
        sys.path.append(str(ENGINE_TOOLS))
    import research_store_identity  # noqa: PLC0415
    return research_store_identity


def check(root: Path, rel: str, cls: str, catalog: dict) -> str | None:
    """None when the catalog's rows for ``rel`` re-render the file on disk (a class with a render rule), or when its
    artifact row holds class ``cls`` and the file's SHA-256 (a class without one); else why not."""
    rsi, reg, db = identity(), registry(), root / catalog["path"]
    info = reg.get(cls)
    if info is not None and info.get("render") is None:
        import research_store as rs  # noqa: PLC0415  (on sys.path once identity() ran)
        with rs.Store.open(db) as store:
            row = store.get("artifact", {"path_key": rsi.path_key(rel)})
        if row is None:
            return "not catalogued (no artifact row)"
        if row["class"] != cls:
            return f"catalogued as class {row['class']}"
        sha = sha256_file(root / rel)
        return None if row["sha256"] == sha else f"catalogued sha256 {row['sha256']} != file sha256 {sha}"
    found = rsi.check_one(db, rel, cls, root=root, registry=reg)
    return None if found is None else found.reason
