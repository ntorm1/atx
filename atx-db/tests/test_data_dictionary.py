"""Tier1-S4 T9: the data dictionary is generated, deterministic and never stale."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "scripts" / "generate_data_dictionary.py"


def _load_generator():
    spec = importlib.util.spec_from_file_location("generate_data_dictionary", GENERATOR)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["generate_data_dictionary"] = module
    spec.loader.exec_module(module)
    return module


def test_the_generator_is_deterministic():
    module = _load_generator()
    assert module.render_data_dictionary() == module.render_data_dictionary()


def test_the_committed_dictionary_is_current():
    module = _load_generator()
    # read_bytes (not read_text/Path.read_text) so a stray CRLF cannot be masked by
    # Python's universal-newline translation on Windows -- the comparison must be exact.
    committed = module.DATA_DICTIONARY_PATH.read_bytes().decode("utf-8")
    assert committed == module.render_data_dictionary()


def test_the_output_uses_lf_newlines_only():
    module = _load_generator()
    rendered = module.render_data_dictionary()
    assert "\r" not in rendered
    committed_bytes = module.DATA_DICTIONARY_PATH.read_bytes()
    assert b"\r" not in committed_bytes


def test_check_mode_passes_on_a_current_file():
    module = _load_generator()
    assert module.main(["--check"]) == 0


def test_check_mode_fails_on_a_stale_file(tmp_path, monkeypatch, capsys):
    module = _load_generator()
    stale = tmp_path / "DATA_DICTIONARY.md"
    stale.write_text("# stale\n", encoding="utf-8")
    monkeypatch.setattr(module, "DATA_DICTIONARY_PATH", stale)
    assert module.main(["--check"]) == 1
    assert "DATA_DICTIONARY.md is stale" in capsys.readouterr().out


def test_write_mode_refreshes_the_file(tmp_path, monkeypatch):
    module = _load_generator()
    target = tmp_path / "DATA_DICTIONARY.md"
    monkeypatch.setattr(module, "DATA_DICTIONARY_PATH", target)
    assert module.main([]) == 0
    assert target.read_text(encoding="utf-8") == module.render_data_dictionary()


def test_the_dictionary_covers_every_required_section():
    module = _load_generator()
    text = module.render_data_dictionary()
    for heading in (
        "## Canonical statement items",
        "## Derived metrics",
        "## Daily market panel",
        "## Universe",
        "## Delistings",
        "## Public API schemas",
        "## Release datasets",
    ):
        assert heading in text


def test_every_public_schema_appears():
    from atx_db.api.catalog import DATASETS

    module = _load_generator()
    text = module.render_data_dictionary()
    for dataset in DATASETS:
        for schema in dataset.schemas:
            assert f"{dataset.code}/{schema.code}" in text


def test_the_universe_vocabulary_is_documented():
    from atx_db.universe_us_listed import ELIGIBLE_SECURITY_TYPES, EXCHANGE_LABELS

    module = _load_generator()
    text = module.render_data_dictionary()
    for value in (*ELIGIBLE_SECURITY_TYPES, *EXCHANGE_LABELS):
        assert value in text


def test_the_delisting_vocabulary_is_documented():
    from atx_db.delisting_evidence import EVIDENCE_PRECEDENCE, REASON_CATEGORIES

    module = _load_generator()
    text = module.render_data_dictionary()
    for reason in REASON_CATEGORIES:
        assert reason in text
    for kind, _rank, _code, _reason, _confidence in EVIDENCE_PRECEDENCE:
        assert kind in text


def test_the_release_dataset_source_is_documented():
    from atx_db.publication import RELEASE_DATASETS

    module = _load_generator()
    text = module.render_data_dictionary()
    for dataset in RELEASE_DATASETS:
        assert dataset.name in text


def test_the_dictionary_never_touches_the_warehouse(monkeypatch):
    import duckdb

    def _boom(*_args, **_kwargs):
        raise AssertionError("the data dictionary must not open a database")

    monkeypatch.setattr(duckdb, "connect", _boom)
    module = _load_generator()
    assert module.render_data_dictionary()
