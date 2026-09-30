"""Stage contract of the Parquet lake: registry entry types, manifest readers and the input-binding helper.

A *stage* is one producer of files under the build root (``alpha_panel.common.build_root()``, default
``data/alpha_panel/v1``, override ``ATX_ALPHA_PANEL_ROOT``) that publishes one manifest last. The registry
(:mod:`atx_db.stagelake.registry`) holds one :class:`Stage` per manifest; ``lake verify``, the catalog, the orchestrator
and the parity scorecard all read it.

Manifest contract (``alpha_panel.common.write_stage_manifest`` writes it):

* ``schema`` (stage schema id), ``status`` = ``complete``, ``code`` (``{module file: {sha256, sha256_lf}}`` plus
  ``git_head``) and ``files`` (``{path relative to the manifest's directory: {bytes, sha256}}``);
* ``input_manifests_sha256``: ``{root-relative input manifest path: SHA-256 of its bytes}`` for every input stage
  the build read (:func:`bind_inputs`). A stage is STALE when an input manifest changed since.

Clock contract (plan section 3): every row carries ``available_at`` (naive TIMESTAMP in UTC, or TIMESTAMPTZ). A
registry output may name another clock column for legacy stages (``clock_utc``, ``session_date``); ``lake verify``
warns on those. A DATE clock means the 22:00 UTC end-of-day mark of that date unless ``clock_sql`` says otherwise.
"""

from __future__ import annotations

import contextlib
import fnmatch
import hashlib
import json
import re
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

SRC_ROOT = Path(__file__).resolve().parents[2]  # .../atx-db/src
PACKAGE_ROOT = SRC_ROOT.parent  # .../atx-db

# plan section 9 lane tokens, plus S0 (the controller's converge chain)
LANES = ("PLAT", "ID", "MKT", "FUND", "OWN", "EVT", "TXT", "LIC", "CHAR", "OPS", "S0")
# vintage policies: how a stage's rows relate to time (and what ``<view>_as_of(ts)`` returns)
VINTAGE_POLICIES = {
    "event": "immutable rows, each with its own clock; as_of(ts) = rows with clock < ts",
    "vintage": "rows are versions of a key; as_of(ts) = the latest version per key with clock < ts",
    "interval": "dated validity intervals with their own clock; as_of(ts) = rows with clock < ts",
    "snapshot": "history restated from one source snapshot (vintage_risk); as_of(ts) = rows with clock < ts, not "
                "proof of what was known then",
    "daily": "one row per (session, security) known at the session's 22:00 UTC mark; as_of(ts) = rows with clock < ts",
    "static": "no clock (reference, audit or look-up table); no as_of macro",
}
REQUIRED_MANIFEST_KEYS = ("schema", "status", "code", "files")
PLATFORM_MODULES = frozenset({"common.py"})  # shared helpers: changes are reported, never make a stage due alone
MAX_GUARD_GB = 1.0
MARK_HOUR_UTC = 22


class RegistryError(ValueError):
    """The registry is inconsistent (unknown input, cycle, duplicate name, bad field)."""


@dataclass(frozen=True)
class Output:
    """One Parquet output family of a stage, exposed as one catalog view.

    ``glob`` is relative to the build root (``ftd/year=*/ftd.parquet``); ``key=*`` path segments are hive
    partitions. ``clock`` names the clock column (None: no clock); ``clock_sql`` overrides how the catalog turns it
    into a naive UTC TIMESTAMP (default: TIMESTAMP as is, TIMESTAMPTZ converted to UTC, DATE + 22:00).
    ``keys``/``order`` define the latest version per key for ``vintage`` outputs."""

    glob: str
    view: str = ""
    clock: str | None = "available_at"
    clock_sql: str | None = None
    vintage: str | None = None
    keys: tuple[str, ...] = ()
    order: tuple[str, ...] = ()
    doc: str = ""
    columns: Mapping[str, str] = field(default_factory=dict)

    @property
    def hive(self) -> bool:
        return any("=" in part for part in self.glob.split("/")[:-1])

    @classmethod
    def from_value(cls, value: Output | str | Mapping[str, Any]) -> Output:
        if isinstance(value, Output):
            return value
        if isinstance(value, str):
            return cls(glob=value)
        d = dict(value)
        for k in ("keys", "order"):
            if k in d:
                d[k] = tuple(d[k])
        if "columns" in d:
            d["columns"] = dict(d["columns"])
        return cls(**d)


@dataclass(frozen=True)
class Stage:
    """One registry entry = one published manifest.

    ``module`` is run as ``python -m module *args`` for every ``args`` tuple in order (``fetch`` tuples are the
    network landing commands, run only when asked); ``built_by`` names the stage whose command writes this one.
    ``code`` lists module files (dotted names) whose SHA-256 makes the stage due; empty = every module the
    manifest recorded plus ``module``. ``planned`` = registered before its first publish."""

    name: str
    lane: str
    schema: str | None
    module: str | None
    args: tuple[tuple[str, ...], ...] = ((),)
    fetch: tuple[tuple[str, ...], ...] = ()
    inputs: tuple[str, ...] = ()
    outputs: tuple[Output, ...] = ()
    manifest: str = ""
    staleness: str = ""
    vintage: str = "event"
    guard_gb: float = 0.6
    code: tuple[str, ...] = ()
    built_by: str | None = None
    planned: bool = False
    doc: str = ""

    def __post_init__(self) -> None:
        if not self.manifest:
            object.__setattr__(self, "manifest", f"{self.name}/manifest.json")
        outs = []
        for i, o in enumerate(self.outputs):
            o = Output.from_value(o)
            if not o.view:
                o = replace(o, view=_default_view(self.name, o.glob, len(self.outputs), i))
            outs.append(o)
        object.__setattr__(self, "outputs", tuple(outs))

    @property
    def manifest_dir(self) -> str:
        return self.manifest.rsplit("/", 1)[0] if "/" in self.manifest else ""

    @property
    def commands(self) -> tuple[tuple[str, ...], ...]:
        if self.module is None or self.built_by:
            return ()
        return tuple(("-m", self.module, *a) for a in self.args)

    def output_vintage(self, out: Output) -> str:
        if out.clock is None:
            return "static"
        return out.vintage or self.vintage

    @classmethod
    def from_dict(cls, d: Mapping[str, Any]) -> Stage:
        """Build from a data-only dict (``LAKE_STAGES`` literals): lists become tuples; ``args`` may be one flat
        list of strings (one command) or a list of lists."""
        d = dict(d)
        for k in ("args", "fetch"):
            if k in d:
                v = list(d[k])
                d[k] = (tuple(v),) if all(isinstance(x, str) for x in v) and (v or k == "args") else \
                    tuple(tuple(x) for x in v)
        for k in ("inputs", "code"):
            if k in d:
                d[k] = tuple(d[k])
        if "outputs" in d:
            d["outputs"] = tuple(Output.from_value(o) for o in d["outputs"])
        unknown = set(d) - set(cls.__dataclass_fields__)
        if unknown:
            raise RegistryError(f"stage {d.get('name')!r}: unknown fields {sorted(unknown)}")
        return cls(**d)


def _ident(text: str) -> str:
    return re.sub(r"[^0-9a-zA-Z_]+", "_", text).strip("_").lower()


def _default_view(stage: str, glob: str, n_outputs: int, index: int) -> str:
    parts = [p for p in glob.split("/") if "=" not in p]
    stem = parts[-1].rsplit(".", 1)[0]
    if "*" in stem:
        stem = parts[-2] if len(parts) > 1 else f"out{index}"
    if n_outputs == 1 or _ident(stem) == _ident(stage):
        return _ident(stage)
    return _ident(f"{stage}_{stem}")


# ---------------------------------------------------------------- validation and DAG
def validate(stages: Iterable[Stage]) -> list[Stage]:
    """Check names, lanes, policies, guard caps, inputs and acyclicity; return the stages in topological order
    (inputs first, ties by name). Raises :class:`RegistryError` listing every problem."""
    stages = list(stages)
    errors: list[str] = []
    by_name: dict[str, Stage] = {}
    manifests: dict[str, str] = {}
    views: dict[str, str] = {}
    for s in stages:
        if s.name in by_name:
            errors.append(f"duplicate stage name {s.name!r}")
        by_name[s.name] = s
        if not re.fullmatch(r"[a-z][a-z0-9_]*", s.name):
            errors.append(f"{s.name}: name must be snake_case")
        if s.lane not in LANES:
            errors.append(f"{s.name}: lane {s.lane!r} not in {LANES}")
        if s.vintage not in VINTAGE_POLICIES:
            errors.append(f"{s.name}: vintage {s.vintage!r} not in {sorted(VINTAGE_POLICIES)}")
        if not 0 < s.guard_gb <= MAX_GUARD_GB:
            errors.append(f"{s.name}: guard_gb {s.guard_gb} outside (0, {MAX_GUARD_GB}]")
        if s.manifest in manifests:
            errors.append(f"{s.name}: manifest {s.manifest} already registered by {manifests[s.manifest]}")
        manifests[s.manifest] = s.name
        if s.module is None and not s.built_by and not s.doc:
            errors.append(f"{s.name}: module None needs built_by or a doc saying how it is built")
        for o in s.outputs:
            if o.glob.startswith("/") or ".." in o.glob.split("/") or "\\" in o.glob:
                errors.append(f"{s.name}: output glob {o.glob!r} must be root-relative with '/'")
            pol = s.output_vintage(o)
            if pol not in VINTAGE_POLICIES:
                errors.append(f"{s.name}: output {o.view} vintage {pol!r} unknown")
            if pol == "vintage" and not o.keys:
                errors.append(f"{s.name}: output {o.view} is a vintage output without keys")
            if o.view in views:
                errors.append(f"{s.name}: view {o.view!r} already used by {views[o.view]}")
            views[o.view] = s.name
    for s in stages:
        for i in s.inputs:
            if i not in by_name:
                errors.append(f"{s.name}: unknown input stage {i!r}")
            if i == s.name:
                errors.append(f"{s.name}: depends on itself")
        if s.built_by and s.built_by not in by_name:
            errors.append(f"{s.name}: built_by {s.built_by!r} is not registered")
    if errors:
        raise RegistryError("; ".join(errors))
    order: list[Stage] = []
    state: dict[str, int] = {}

    def visit(name: str, path: tuple[str, ...]) -> None:
        if state.get(name) == 2:
            return
        if state.get(name) == 1:
            raise RegistryError(f"input cycle: {' -> '.join((*path, name))}")
        state[name] = 1
        for i in sorted(by_name[name].inputs):
            visit(i, (*path, name))
        state[name] = 2
        order.append(by_name[name])

    for name in sorted(by_name):
        visit(name, ())
    return order


def downstream(stages: Iterable[Stage], roots: Iterable[str]) -> set[str]:
    """Names of every stage reachable from ``roots`` through input edges (``roots`` included)."""
    consumers: dict[str, set[str]] = {}
    for s in stages:
        for i in s.inputs:
            consumers.setdefault(i, set()).add(s.name)
    seen: set[str] = set()
    todo = list(roots)
    while todo:
        n = todo.pop()
        if n in seen:
            continue
        seen.add(n)
        todo.extend(consumers.get(n, ()))
    return seen


# ---------------------------------------------------------------- files and manifests
def default_root() -> Path:
    """The build root (``ATX_ALPHA_PANEL_ROOT``, default ``data/alpha_panel/v1``)."""
    from ..alpha_panel.common import build_root

    return build_root()


def sha256_file(path: Path, chunk: int = 1 << 22) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        while blob := fh.read(chunk):
            h.update(blob)
    return h.hexdigest()


def sha256_lf(path: Path) -> str:
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def read_manifest(path: Path) -> dict[str, Any] | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def output_files(root: Path, stage: Stage) -> dict[str, list[Path]]:
    """{output view: sorted matching files} (``.partial`` files excluded)."""
    return {o.view: sorted(p for p in root.glob(o.glob) if p.is_file() and not p.name.endswith(".partial"))
            for o in stage.outputs}


def glob_match(rel: str, pattern: str) -> bool:
    """Segment-wise glob match of a root-relative POSIX path (``*`` never crosses ``/``; ``**`` = any depth)."""
    a, b = rel.split("/"), pattern.split("/")

    def rec(i: int, j: int) -> bool:
        if j == len(b):
            return i == len(a)
        if b[j] == "**":
            return any(rec(k, j + 1) for k in range(i, len(a) + 1))
        return i < len(a) and fnmatch.fnmatchcase(a[i], b[j]) and rec(i + 1, j + 1)

    return rec(0, 0)


def recorded_files(manifest: Mapping[str, Any], base: Path | None = None) -> dict[str, dict[str, Any]] | None:
    """``files`` of a manifest, else a legacy ``outputs`` map whose entries carry ``sha256`` (keyed by their
    ``path`` relative to ``base`` when they record one); None if neither."""
    files = manifest.get("files")
    if isinstance(files, dict) and files:
        return {k: v for k, v in files.items() if isinstance(v, dict)}
    legacy = manifest.get("outputs")
    if isinstance(legacy, dict):
        got = {}
        for k, v in legacy.items():
            if isinstance(v, dict) and v.get("sha256"):
                rel = k
                if base is not None and isinstance(v.get("path"), str):
                    with contextlib.suppress(ValueError):
                        rel = Path(v["path"]).resolve().relative_to(base.resolve()).as_posix()
                got[rel] = v
        if got:
            return got
    return None


_LEGACY_KEY = re.compile(r"^(?P<stage>[a-z][a-z0-9_]*?)_manifest(?:_sha256)?$")


def recorded_inputs(manifest: Mapping[str, Any], root: Path) -> tuple[dict[str, str], str]:
    """(``{root-relative manifest path: sha256}``, basis) recorded by a manifest.

    Basis ``input_manifests_sha256`` for the contract key, ``legacy`` when recovered from older layouts
    (``sources.<stage>_manifest``, ``inputs.<stage>_manifest_sha256``, ``receipt.input_manifests``,
    ``<stage>_manifest`` = ``{manifest.json: {sha256}}``), ``none`` when nothing is recorded."""
    got = manifest.get("input_manifests_sha256")
    out: dict[str, str] = {}
    if isinstance(got, dict) and got:
        for k, v in got.items():
            sha = v.get("sha256") if isinstance(v, dict) else v
            if isinstance(sha, str):
                out[k if k.endswith(".json") else f"{k}/manifest.json"] = sha
        return out, "input_manifests_sha256"

    def take(stage: str, value: Any) -> None:
        if isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value):
            out.setdefault(f"{stage}/manifest.json", value)
        elif isinstance(value, dict):
            if isinstance(value.get("sha256"), str):
                path = value.get("path")
                rel = f"{stage}/manifest.json"
                if isinstance(path, str):
                    with contextlib.suppress(ValueError):
                        rel = Path(path).resolve().relative_to(root.resolve()).as_posix()
                out.setdefault(rel, value["sha256"])
            elif isinstance(value.get("manifest.json"), dict) and isinstance(value["manifest.json"].get("sha256"), str):
                out.setdefault(f"{stage}/manifest.json", value["manifest.json"]["sha256"])

    for container in (manifest, manifest.get("sources"), manifest.get("inputs")):
        if isinstance(container, dict):
            for k, v in container.items():
                m = _LEGACY_KEY.match(k)
                if m:
                    take(m.group("stage"), v)
    receipt = manifest.get("receipt")
    if isinstance(receipt, dict) and isinstance(receipt.get("input_manifests"), dict):
        for k, v in receipt["input_manifests"].items():
            take(k, v)
    return out, ("legacy" if out else "none")


def recorded_code(manifest: Mapping[str, Any]) -> dict[str, str]:
    """{module file name: LF-normalised SHA-256 (else raw SHA-256)} recorded by a manifest's ``code``."""
    code = manifest.get("code")
    out: dict[str, str] = {}
    if isinstance(code, dict):
        for k, v in code.items():
            if isinstance(v, dict) and k.endswith(".py"):
                sha = v.get("sha256_lf") or v.get("sha256")
                if isinstance(sha, str):
                    out[k] = sha
    return out


def module_path(dotted: str, src_root: Path = SRC_ROOT) -> Path:
    """File of a dotted module name under ``src_root`` (no import)."""
    base = src_root.joinpath(*dotted.split("."))
    return base / "__init__.py" if base.is_dir() else base.with_suffix(".py")


def code_modules(stage: Stage, manifest: Mapping[str, Any] | None, src_root: Path = SRC_ROOT) -> dict[str, Path]:
    """{module file name: path} whose SHA decides whether the stage's code changed (platform modules excluded)."""
    mods: dict[str, Path] = {}
    names = list(stage.code)
    if not names and stage.module:
        names.append(stage.module)
        pkg = stage.module.rsplit(".", 1)[0]
        for fname in recorded_code(manifest or {}):
            names.append(f"{pkg}.{fname[:-3]}")
    for dotted in names:
        p = module_path(dotted, src_root)
        if p.name not in PLATFORM_MODULES:
            mods[p.name] = p
    return mods


def manifest_sha(root: Path, rel: str) -> str | None:
    p = root / rel
    return sha256_file(p) if p.is_file() else None


def bind_inputs(*stages: str, root: Path | None = None) -> dict[str, str]:
    """``input_manifests_sha256`` payload for a stage manifest: SHA-256 of each named input stage's manifest.

    Usage in a stage module: ``C.write_stage_manifest(STAGE, SCHEMA, MODULES, {..., "input_manifests_sha256":
    bind_inputs("prices", "identity_table")})``. Registered names resolve through the registry (``identity_table``
    -> ``identity/link_table_manifest.json``); others to ``<name>/manifest.json``. A missing manifest raises."""
    from . import registry

    if root is None:
        from ..alpha_panel.common import build_root

        root = build_root()
    by_name = {s.name: s for s in registry.load(strict=False)}
    out = {}
    for name in stages:
        rel = by_name[name].manifest if name in by_name else f"{name}/manifest.json"
        p = root / rel
        if not p.is_file():
            raise FileNotFoundError(f"input stage {name!r}: {p} has no manifest (build it first)")
        out[rel] = sha256_file(p)
    return out
