from __future__ import annotations

import importlib
import pkgutil
from pathlib import Path

import atx_db

# The public-API snapshot pins the submodule list of the ``atx_db`` package, but a
# submodule only enters ``dir(atx_db)`` once it has been imported somewhere in the
# process. Several public top-level feature modules (backfill, factor_panel,
# factors, signal_eval, identifiers_figi, identifiers_lei) are not eagerly
# imported by ``atx_db/__init__``, so their presence in ``dir(atx_db)`` would otherwise
# depend on xdist worker/test ordering and make this snapshot flaky. Import every
# public top-level ``atx_db`` submodule here so the snapshot measures a deterministic,
# complete surface in any run. This does not touch ``atx_db.migrations`` body modules,
# which ``atx_db/migrations/__init__`` deliberately keeps private.
for _submodule in pkgutil.iter_modules(atx_db.__path__):
    if not _submodule.name.startswith("_") and _submodule.name != "tests":
        importlib.import_module(f"atx_db.{_submodule.name}")

from atx_db.module_boundaries import (
    DECOMPOSED_PACKAGES,
    BoundaryViolation,
    ImportEdge,
    PublicApiDiff,
    SourceModule,
    collect_import_edges,
    compare_public_api_snapshot,
    cross_package_private_imports,
    import_graph_cycles,
    iter_source_modules,
    load_public_api_snapshot,
)


SNAPSHOT_PATH = Path(__file__).with_name("data") / "public_api_snapshot.json"


def test_public_api_snapshot_matches_pinned_fixture():
    expected = load_public_api_snapshot(SNAPSHOT_PATH)

    assert compare_public_api_snapshot(expected) == ()


def test_decomposed_packages_have_no_cross_package_private_imports():
    violations = cross_package_private_imports(collect_import_edges())

    assert violations == ()


def test_decomposed_package_import_graphs_are_acyclic():
    modules = iter_source_modules()
    cycles = import_graph_cycles(collect_import_edges(modules), modules)

    assert cycles == ()


def test_private_cross_package_import_violation_is_reported():
    edges = (
        ImportEdge("atx_db.asof.fundamentals", "atx_db.estimates", 1, "ok.py"),
        ImportEdge("atx_db.asof.fundamentals", "atx_db.estimates._common", 2, "bad.py"),
    )

    assert cross_package_private_imports(edges) == (
        BoundaryViolation(
            "atx_db.asof.fundamentals",
            "atx_db.estimates._common",
            2,
            "bad.py",
            "cross-package imports must use the package public facade",
        ),
    )


def test_import_cycle_violation_is_reported():
    modules = (
        SourceModule("atx_db.asof.a", Path("a.py"), "atx_db.asof"),
        SourceModule("atx_db.asof.b", Path("b.py"), "atx_db.asof"),
    )
    edges = (
        ImportEdge("atx_db.asof.a", "atx_db.asof.b", 1, "a.py"),
        ImportEdge("atx_db.asof.b", "atx_db.asof.a", 1, "b.py"),
    )

    assert import_graph_cycles(edges, modules, package_names=("atx_db.asof",)) == (
        ("atx_db.asof.a", "atx_db.asof.b", "atx_db.asof.a"),
    )


def test_public_api_snapshot_diff_reports_missing_and_extra_symbol():
    diffs = compare_public_api_snapshot(
        {"atx_db.asof": ("fundamentals_asof", "missing_symbol")},
        {"atx_db.asof": ("fundamentals_asof", "extra_symbol")},
    )

    assert diffs == (
        PublicApiDiff(
            "atx_db.asof",
            missing=("missing_symbol",),
            extra=("extra_symbol",),
        ),
    )


def test_all_decomposed_packages_are_source_discovered():
    discovered = {module.package for module in iter_source_modules()}

    assert discovered == set(DECOMPOSED_PACKAGES)
