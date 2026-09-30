"""Stage registry: validation, data-only registration, and coverage of the live build root."""

from __future__ import annotations

from pathlib import Path

import pytest

from atx_db.stagelake import registry
from atx_db.stagelake.contract import Output, RegistryError, Stage, downstream, glob_match, validate
from atx_db.stagelake.coverage import coverage_report

LIVE_ROOT = Path(__file__).resolve().parents[1] / "data" / "alpha_panel" / "v1"


def test_registry_is_valid_and_topological() -> None:
    stages = registry.load()
    pos = {s.name: i for i, s in enumerate(stages)}
    assert len(pos) == len(stages) >= 29
    for s in stages:
        assert all(pos[i] < pos[s.name] for i in s.inputs), s.name
        assert s.guard_gb <= 1.0 and s.manifest.endswith(".json")
    views = [o.view for s in stages for o in s.outputs]
    assert len(views) == len(set(views))


def test_every_build_py_step_maps_to_a_registered_stage() -> None:
    from atx_db.alpha_panel.build import STAGES as BUILD

    names = {s.name for s in registry.load()}
    unmapped = [n for n, _, _ in BUILD if n not in registry.BUILD_STEPS]
    assert not unmapped, f"add these build.py steps to lake.registry.BUILD_STEPS: {unmapped}"
    bad = {k: v for k, v in registry.BUILD_STEPS.items() if v is not None and v not in names}
    assert not bad


def test_validate_rejects_bad_entries() -> None:
    a = Stage("a", "PLAT", None, "m.a")
    with pytest.raises(RegistryError, match="unknown input"):
        validate([a, Stage("b", "PLAT", None, "m.b", inputs=("zz",))])
    with pytest.raises(RegistryError, match="cycle"):
        validate([Stage("a", "PLAT", None, "m.a", inputs=("b",)), Stage("b", "PLAT", None, "m.b", inputs=("a",))])
    with pytest.raises(RegistryError, match="lane"):
        validate([Stage("a", "NOPE", None, "m.a")])
    with pytest.raises(RegistryError, match="guard_gb"):
        validate([Stage("a", "PLAT", None, "m.a", guard_gb=1.5)])
    with pytest.raises(RegistryError, match="without keys"):
        validate([Stage("a", "PLAT", None, "m.a", vintage="vintage", outputs=(Output("a/x.parquet"),))])
    with pytest.raises(RegistryError, match="duplicate"):
        validate([a, a])


def test_from_dict_accepts_flat_and_nested_commands() -> None:
    s = Stage.from_dict({"name": "ref", "lane": "MKT", "schema": "s/v1", "module": "m.ref", "args": ["build"],
                         "fetch": ["fetch", "--all"], "outputs": ["ref/a.parquet", {"glob": "ref/b.parquet",
                                                                                   "clock": None}]})
    assert s.commands == (("-m", "m.ref", "build"),)
    assert s.fetch == (("fetch", "--all"),)
    assert [o.view for o in s.outputs] == ["ref_a", "ref_b"] and s.outputs[1].clock is None
    s2 = Stage.from_dict({"name": "f", "lane": "FUND", "schema": None, "module": "m.f",
                          "args": [["prepare"], ["finalize"]]})
    assert s2.commands == (("-m", "m.f", "prepare"), ("-m", "m.f", "finalize"))
    with pytest.raises(RegistryError, match="unknown fields"):
        Stage.from_dict({"name": "x", "lane": "PLAT", "schema": None, "module": "m", "typo": 1})


def test_lake_stages_literal_is_discovered_without_import(tmp_path: Path) -> None:
    pkg = tmp_path / "atx_db" / "alpha_panel"
    pkg.mkdir(parents=True)
    (pkg / "newstage.py").write_text(
        'raise RuntimeError("never imported")\n'
        'LAKE_STAGES = [{"name": "newstage", "lane": "EVT", "schema": "atx.alpha-panel.newstage/v1",\n'
        '                "args": ["build"], "inputs": ["prices"], "outputs": [{"glob": "newstage/e.parquet"}]}]\n',
        encoding="utf-8")
    found = registry.discover(tmp_path)
    assert [s.name for s in found] == ["newstage"]
    assert found[0].module == "atx_db.alpha_panel.newstage" and found[0].outputs[0].view == "newstage"
    stages = registry.load(extra=found)
    assert "newstage" in downstream(stages, ["prices"])


def test_glob_match_is_segment_wise() -> None:
    assert glob_match("ftd/year=2020/ftd.parquet", "ftd/year=*/ftd.parquet")
    assert not glob_match("ftd/x/year=2020/ftd.parquet", "ftd/year=*/ftd.parquet")
    assert glob_match("insider/owners/year=2015/2015q1.parquet", "insider/owners/year=*/*.parquet")
    assert glob_match("a/b/c.parquet", "a/**/c.parquet") and glob_match("a/c.parquet", "a/**/c.parquet")


@pytest.mark.skipif(not LIVE_ROOT.is_dir(), reason="no live stage lake on this machine")
def test_live_lake_every_manifest_and_parquet_file_is_registered_and_vice_versa() -> None:
    """Every ``*manifest*.json`` and Parquet file under data/alpha_panel/v1 (underscore scratch dirs excluded) is
    covered by a registry entry, and every non-planned entry has its manifest on disk. On failure the message is the
    entry to add."""
    report = coverage_report(LIVE_ROOT, registry.load())
    assert report == "", "\n" + report


def test_non_strict_load_drops_a_broken_declaration(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    pkg = tmp_path / "atx_db" / "alpha_panel"
    pkg.mkdir(parents=True)
    (pkg / "good.py").write_text('LAKE_STAGES = [{"name": "good", "lane": "EVT", "schema": None, '
                                 '"inputs": ["prices"]}]\n', encoding="utf-8")
    (pkg / "bad.py").write_text('LAKE_STAGES = [{"name": "bad", "lane": "EVT", "schema": None, '
                                '"inputs": ["no_such_stage"]}]\n', encoding="utf-8")
    (pkg / "broken.py").write_text("LAKE_STAGES = [{'name': \n", encoding="utf-8")  # mid-edit syntax error
    with pytest.raises((RegistryError, SyntaxError)):
        registry.load(tmp_path)
    names = {s.name for s in registry.load(tmp_path, strict=False)}
    assert "good" in names and "bad" not in names and "prices" in names
    err = capsys.readouterr().err
    assert "stage 'bad' dropped" in err and "atx_db.alpha_panel.broken" in err
