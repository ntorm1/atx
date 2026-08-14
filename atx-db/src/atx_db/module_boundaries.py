from __future__ import annotations

import ast
import importlib
import importlib.util
import json
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import Mapping, Sequence


DECOMPOSED_PACKAGES = (
    "atx_db.migrations",
    "atx_db.quality",
    "atx_db.asof",
    "atx_db.estimates",
)
PUBLIC_API_MODULES = ("atx_db", *DECOMPOSED_PACKAGES)
PUBLIC_API_EXCLUDES = {
    "atx_db": frozenset({"tests", "_standardization_set_based"}),
}


@dataclass(frozen=True)
class SourceModule:
    name: str
    path: Path
    package: str
    is_package: bool = False


@dataclass(frozen=True)
class ImportEdge:
    source: str
    target: str
    lineno: int
    path: str


@dataclass(frozen=True)
class BoundaryViolation:
    source: str
    target: str
    lineno: int
    path: str
    reason: str


@dataclass(frozen=True)
class PublicApiDiff:
    module: str
    missing: tuple[str, ...]
    extra: tuple[str, ...]


def public_api_snapshot(
    module_names: Sequence[str] = PUBLIC_API_MODULES,
) -> dict[str, list[str]]:
    """Return a deterministic snapshot of module public dir() surfaces."""
    snapshot: dict[str, list[str]] = {}
    for module_name in module_names:
        module = importlib.import_module(module_name)
        excludes = PUBLIC_API_EXCLUDES.get(module_name, frozenset())
        snapshot[module_name] = sorted(
            name for name in dir(module) if not name.startswith("__") and name not in excludes
        )
    return snapshot


def load_public_api_snapshot(path: Path) -> dict[str, list[str]]:
    return json.loads(path.read_text(encoding="utf-8"))


def compare_public_api_snapshot(
    expected: Mapping[str, Sequence[str]],
    actual: Mapping[str, Sequence[str]] | None = None,
) -> tuple[PublicApiDiff, ...]:
    actual = public_api_snapshot(tuple(expected)) if actual is None else actual
    diffs: list[PublicApiDiff] = []
    for module_name in sorted(expected):
        expected_names = set(expected[module_name])
        actual_names = set(actual.get(module_name, ()))
        missing = tuple(sorted(expected_names - actual_names))
        extra = tuple(sorted(actual_names - expected_names))
        if missing or extra:
            diffs.append(PublicApiDiff(module_name, missing, extra))
    return tuple(diffs)


def _module_file(module: ModuleType) -> Path:
    if not module.__file__:
        raise RuntimeError(f"Module {module.__name__!r} has no __file__")
    return Path(module.__file__).resolve()


def iter_source_modules(
    package_names: Sequence[str] = DECOMPOSED_PACKAGES,
) -> tuple[SourceModule, ...]:
    modules: list[SourceModule] = []
    for package_name in package_names:
        package = importlib.import_module(package_name)
        package_dir = _module_file(package).parent
        modules.append(
            SourceModule(package_name, package_dir / "__init__.py", package_name, is_package=True)
        )
        for path in sorted(package_dir.rglob("*.py")):
            if path.name == "__init__.py":
                continue
            rel = path.relative_to(package_dir)
            module_suffix = ".".join((*rel.parts[:-1], rel.stem))
            modules.append(SourceModule(f"{package_name}.{module_suffix}", path, package_name))
    return tuple(modules)


def _resolve_import_from(module: SourceModule, node: ast.ImportFrom) -> str | None:
    if node.level:
        package_context = module.name if module.is_package else module.name.rpartition(".")[0]
        relative_name = "." * node.level + (node.module or "")
        try:
            return importlib.util.resolve_name(relative_name, package_context)
        except ImportError:
            return None
    return node.module


def collect_import_edges(
    modules: Sequence[SourceModule] | None = None,
) -> tuple[ImportEdge, ...]:
    modules = iter_source_modules() if modules is None else modules
    edges: list[ImportEdge] = []
    for module in modules:
        tree = ast.parse(module.path.read_text(encoding="utf-8"), filename=str(module.path))
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                for alias in node.names:
                    edges.append(
                        ImportEdge(module.name, alias.name, node.lineno, str(module.path))
                    )
            elif isinstance(node, ast.ImportFrom):
                target = _resolve_import_from(module, node)
                if target is not None and node.level and node.module is None:
                    for alias in node.names:
                        if alias.name != "*":
                            edges.append(
                                ImportEdge(
                                    module.name,
                                    f"{target}.{alias.name}",
                                    node.lineno,
                                    str(module.path),
                                )
                            )
                elif target is not None:
                    edges.append(ImportEdge(module.name, target, node.lineno, str(module.path)))
    return tuple(edges)


def _owning_package(module_name: str, package_names: Sequence[str]) -> str | None:
    for package_name in package_names:
        if module_name == package_name or module_name.startswith(f"{package_name}."):
            return package_name
    return None


def cross_package_private_imports(
    edges: Sequence[ImportEdge],
    package_names: Sequence[str] = DECOMPOSED_PACKAGES,
) -> tuple[BoundaryViolation, ...]:
    violations: list[BoundaryViolation] = []
    for edge in edges:
        source_package = _owning_package(edge.source, package_names)
        target_package = _owning_package(edge.target, package_names)
        if source_package is None or target_package is None or source_package == target_package:
            continue
        if edge.target != target_package:
            violations.append(
                BoundaryViolation(
                    edge.source,
                    edge.target,
                    edge.lineno,
                    edge.path,
                    "cross-package imports must use the package public facade",
                )
            )
    return tuple(violations)


def import_graph_cycles(
    edges: Sequence[ImportEdge],
    modules: Sequence[SourceModule] | None = None,
    package_names: Sequence[str] = DECOMPOSED_PACKAGES,
) -> tuple[tuple[str, ...], ...]:
    modules = iter_source_modules(package_names) if modules is None else modules
    known = {module.name for module in modules}
    graph: dict[str, set[str]] = {module.name: set() for module in modules}
    for edge in edges:
        source_package = _owning_package(edge.source, package_names)
        target_package = _owning_package(edge.target, package_names)
        if source_package is None or source_package != target_package:
            continue
        if edge.target in known and edge.target != edge.source:
            graph.setdefault(edge.source, set()).add(edge.target)

    cycles: set[tuple[str, ...]] = set()
    visiting: list[str] = []
    visited: set[str] = set()

    def visit(node: str) -> None:
        if node in visiting:
            cycle = visiting[visiting.index(node) :] + [node]
            rotations = [tuple(cycle[i:-1] + cycle[:i] + [cycle[i]]) for i in range(len(cycle) - 1)]
            cycles.add(min(rotations))
            return
        if node in visited:
            return
        visiting.append(node)
        for target in sorted(graph.get(node, ())):
            visit(target)
        visiting.pop()
        visited.add(node)

    for module_name in sorted(graph):
        visit(module_name)
    return tuple(sorted(cycles))
