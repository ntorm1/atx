"""Historical parity records stay labeled; current references resolve in this checkout."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
HISTORICAL = (
    "PARITY_GAP.md",
    "ROADMAP_PARITY.md",
    "WAREHOUSE_PARITY_NEXT_AGENT_README.md",
    "WAREHOUSE_PARITY_TRANCHES.md",
)
BANNER_MARKER = "> **HISTORICAL — superseded.**"
HISTORICAL_BANNER = (
    "> **HISTORICAL — superseded.** This document is retained as a build record; its layout and counts do not describe the current warehouse.\n"
    "> Architecture and dated evidence: [docs/FUNDAMENTALS_PROVIDER_DESIGN.md](FUNDAMENTALS_PROVIDER_DESIGN.md).\n"
    "> Current design contract: [docs/superpowers/specs/2026-09-19-tier1-parity-design.md](../../docs/superpowers/specs/2026-09-19-tier1-parity-design.md).\n"
    "> Field reference: [docs/DATA_DICTIONARY.md](DATA_DICTIONARY.md). Activation and measurement status: [docs/PRODUCTION_RUNBOOK.md](PRODUCTION_RUNBOOK.md).\n\n"
)


def _assert_local_links_resolve(text: str, parent: Path) -> None:
    for target in re.findall(r"\[[^\]]+\]\(([^)]+)\)", text):
        if "://" in target or target.startswith("#"):
            continue
        path = parent / target.split("#", maxsplit=1)[0]
        assert path.is_file(), f"Broken documentation link: {path}"


@pytest.mark.parametrize("name", HISTORICAL)
def test_each_superseded_document_starts_with_the_fixed_banner(name: str) -> None:
    text = (DOCS / name).read_text(encoding="utf-8")
    assert text.startswith(HISTORICAL_BANNER)
    assert text[len(HISTORICAL_BANNER) :].startswith("# ")
    _assert_local_links_resolve(HISTORICAL_BANNER, DOCS)


def test_current_documents_are_not_marked_historical() -> None:
    for path in (
        DOCS / "FUNDAMENTALS_PROVIDER_DESIGN.md",
        DOCS / "DATA_DICTIONARY.md",
        DOCS / "PRODUCTION_RUNBOOK.md",
        ROOT.parent / "docs/superpowers/specs/2026-09-19-tier1-parity-design.md",
    ):
        assert BANNER_MARKER not in path.read_text(encoding="utf-8")


def test_runbook_documents_current_surfaces_and_measurement_limits() -> None:
    text = (DOCS / "PRODUCTION_RUNBOOK.md").read_text(encoding="utf-8")
    for token in (
        "atx-db publish-release",
        "universe_us_listed_membership",
        "--only delisting_evidence --only universe_us_listed",
        "--backup-keep 100",
        "performance_delisting_return=None",
        "historical top-3000 common-equity cohort remains unestablished",
        "closePr",
        "generate_data_dictionary.py --check",
        "warehouse_template.duckdb",
    ):
        assert token in text
    _assert_local_links_resolve(text, DOCS)


def test_readme_distinguishes_generated_dictionary_from_pending_measurements() -> None:
    text = (ROOT / "README.md").read_text(encoding="utf-8")
    references = text.split("### Reference documentation", maxsplit=1)[1].split("## Data safety", maxsplit=1)[0]
    assert "docs/DATA_DICTIONARY.md" in references
    assert "docs/ITEM_COVERAGE.md" in references
    assert "has not yet been published" in references
    _assert_local_links_resolve(references, ROOT)


def test_ci_checks_the_dictionary_before_the_non_slow_suite() -> None:
    workflow = (ROOT.parent / ".github/workflows/atx-db.yml").read_text(encoding="utf-8")
    check = "run: python scripts/generate_data_dictionary.py --check"
    suite = 'run: python -m pytest -q -n 4 -m "not slow" --tb=short tests'
    assert check in workflow
    assert suite in workflow
    assert workflow.index(check) < workflow.index("- name: Full non-slow test suite")


def test_ci_lint_and_typecheck_paths_exist() -> None:
    workflow = (ROOT.parent / ".github/workflows/atx-db.yml").read_text(encoding="utf-8")
    for relative in re.findall(r"^\s+((?:src|scripts|tests)/\S+\.py)\s*$", workflow, flags=re.MULTILINE):
        assert (ROOT / relative).is_file(), f"CI references a missing file: {relative}"
