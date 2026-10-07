"""Synthetic delivery failure boundaries; never open real lake inputs."""
import copy
import json
from pathlib import Path

import pytest

from atx_db.alpha_panel import a0_fixed_delivery as delivery


def tree(root):
    root.mkdir()
    (root / "links.parquet").write_bytes(b"synthetic parquet bytes")
    (root / "manifest.json").write_text(json.dumps({"files": {
        "links.parquet": {"sha256": delivery.digest(root / "links.parquet")}}}))
    return delivery.seal_tree(root, manifests=("manifest.json",))


def test_fixed_commands_pin_one_commit_repinned64_and_explicit_families(tmp_path):
    commands = delivery.fixed_commands(python=Path("python"), commit="a" * 40,
                                      base=tmp_path / "fixed", data_root=tmp_path / "data", guard=Path("guard"))
    assert commands["arms"][commands["arms"].index("--baseline") + 1] == "repinned"
    assert commands["arms"][commands["arms"].index("--reconstruction-batch-size") + 1] == "64"
    assert commands["report"][commands["report"].index("--pit-export") + 1] == "identity-bridge-v4-pit"
    assert commands["report"][commands["report"].index("--published-pit-export") + 1] == "identity-bridge-v2-pit"
    with pytest.raises(ValueError, match="full frozen"):
        delivery.fixed_commands(python=Path("python"), commit="HEAD", base=tmp_path, data_root=tmp_path, guard=Path("g"))


def test_verify_manifest_hash_before_any_copy(tmp_path):
    source = tmp_path / "source"
    tree(source)
    (source / "links.parquet").write_bytes(b"changed")
    with pytest.raises(ValueError, match="manifest hash"):
        delivery.seal_tree(source, manifests=("manifest.json",))


def test_absent_upstream_hashes_are_truthfully_delivery_only(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    (source / "name_manifest.json").write_text('{"name_link_days": 1}')
    (source / "links_name_days.parquet").write_bytes(b"name days")
    seal = delivery.seal_tree(source, manifests=("name_manifest.json",), required=("links_name_days.parquet",))
    assert seal["declared"] == {}
    assert seal["delivery_only_pins"] == ["links_name_days.parquet"]


@pytest.mark.parametrize("relative", ["../foreign", "C:/foreign", "missing.parquet"])
def test_manifest_cannot_escape_or_declare_missing_output(tmp_path, relative):
    source = tmp_path / "source"
    source.mkdir()
    (source / "manifest.json").write_text(json.dumps({"files": {relative: {"sha256": "a" * 64}}}))
    with pytest.raises(ValueError):
        delivery.seal_tree(source, manifests=("manifest.json",))


def test_list_declarations_exclude_uncompleted_rehearsal_attempts(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    (source / "complete.parquet").write_bytes(b"complete")
    (source / "partial.parquet").write_bytes(b"foreign partial")
    (source / "manifest.json").write_text(json.dumps({"files": [
        {"file": "complete.parquet", "sha256": delivery.digest(source / "complete.parquet")}] }))
    seal = delivery.seal_tree(source, manifests=("manifest.json",))
    assert set(seal["files"]) == {"manifest.json", "complete.parquet"}


def test_publication_full_bytes_verified_and_receipt_exclusive(tmp_path):
    seal = tree(tmp_path / "source")
    target, receipt = tmp_path / "published", tmp_path / "delivery.json"
    result = delivery.publish([(seal, target)], receipt)
    assert result["status"] == "complete"
    for relative, expected in seal["files"].items():
        assert delivery.digest(target / relative) == expected
    before = receipt.read_bytes()
    with pytest.raises(FileExistsError):
        delivery.publish([(seal, tmp_path / "other")], receipt)
    assert not (tmp_path / "other").exists()
    assert receipt.read_bytes() == before


def test_one_occupied_target_aborts_all_without_altering_it(tmp_path):
    seal = tree(tmp_path / "source")
    occupied = tmp_path / "occupied"
    occupied.mkdir()
    (occupied / "sentinel").write_bytes(b"immutable")
    with pytest.raises(FileExistsError):
        delivery.publish([(seal, tmp_path / "new"), (seal, occupied)], tmp_path / "receipt")
    assert not (tmp_path / "new").exists()
    assert (occupied / "sentinel").read_bytes() == b"immutable"


def test_changed_source_aborts_before_destination_or_receipt(tmp_path):
    seal = tree(tmp_path / "source")
    (tmp_path / "source" / "links.parquet").write_bytes(b"changed after seal")
    with pytest.raises(ValueError, match="source changed"):
        delivery.publish([(seal, tmp_path / "new")], tmp_path / "receipt")
    assert not (tmp_path / "new").exists()
    assert not (tmp_path / "receipt").exists()


def test_corrupt_copy_never_publishes_manifest_or_receipt(tmp_path, monkeypatch):
    seal = tree(tmp_path / "source")
    monkeypatch.setattr(delivery.shutil, "copyfileobj", lambda src, dst, length: dst.write(b"corrupted"))
    with pytest.raises(ValueError, match="copy hash"):
        delivery.publish([(seal, tmp_path / "new")], tmp_path / "receipt")
    assert not (tmp_path / "new").exists()
    assert not (tmp_path / "receipt").exists()
    retained = list(tmp_path.glob("new.delivery-*"))
    assert len(retained) == 1 and not (retained[0] / "manifest.json").exists()


def test_target_race_does_not_replace_competitor(tmp_path, monkeypatch):
    seal = tree(tmp_path / "source")
    target = tmp_path / "new"
    rename = delivery.os.rename

    def race(source, dest):
        dest.mkdir()
        (dest / "sentinel").write_bytes(b"competitor")
        rename(source, dest)

    monkeypatch.setattr(delivery.os, "rename", race)
    with pytest.raises(FileExistsError):
        delivery.publish([(seal, target)], tmp_path / "receipt")
    assert (target / "sentinel").read_bytes() == b"competitor"
    assert not (tmp_path / "receipt").exists()


def test_activation_patch_exactly_two_files_and_never_applies(tmp_path):
    common = 'PACKAGE_ROOT = root\nIDENTITY_R4_DIR = PACKAGE_ROOT / "old"\nOTHER = 7\n'
    identity = 'VALIDATION_EXPECTED = {\n    "rows": 1,\n    "rows_by_tier": {"high": 1},\n}\nOTHER = 8\n'
    patch = tmp_path / "activate.patch"
    delivery.save_activation_patch(patch, common_source=common, identity_source=identity,
                                   got={"rows": 2, "rows_by_tier": {"high": 2}})
    text = patch.read_text()
    assert text.count("--- a/") == 2 and text.count("+++ b/") == 2
    assert "session9-pit-r5" in text and "'rows': 2" in text
    assert "-OTHER" not in text and "+OTHER" not in text
    with pytest.raises(FileExistsError):
        delivery.save_activation_patch(patch, common_source=common, identity_source=identity,
                                       got={"rows": 3, "rows_by_tier": {"high": 3}})
    assert patch.read_text() == text
    with pytest.raises(ValueError, match="keys differ"):
        delivery.activation_patch(common, identity, {"rows": 2})


def results():
    row = {"rows_1_4": {"a_only": 0}, "row_7": {"2020": {"a_only": 0, "b_only": 0}},
           "row_8": {"a_only": 0, "b_only": 0}}
    pairs = {pair: copy.deepcopy(row) for pair in ("B1 vs A", "B2 vs A", "B12 vs A", "B12 vs B1")}
    return {"commit": "a" * 40, "baseline": "repinned", "pit_export": "identity-bridge-v4-pit",
            "published_pit_export": "identity-bridge-v2-pit", "sanity": ["PASS synthetic"],
            "windows": {"original": {"rows": pairs}, "2020-latest": {"rows": copy.deepcopy(pairs)}},
            "d0": {key: {} for key in ("r4", "pit", "link_table", "stage_i_validation")}}


def test_gate_recomputes_zero_and_accepts_extended_differences():
    data = results()
    data["windows"]["2020-latest"]["rows"]["B12 vs A"]["row_8"]["b_only"] = 19
    delivery.validate_results(data, commit="a" * 40)
    data["windows"]["original"]["rows"]["B12 vs A"]["row_8"]["b_only"] = 1
    data["windows"]["original"]["zero_gate_b12_vs_a"] = {"pass": True}
    with pytest.raises(ValueError, match="zero gate FAIL"):
        delivery.validate_results(data, commit="a" * 40)


@pytest.mark.parametrize("field,value", [("pit_export", "identity-bridge-v3-pit"),
                                        ("baseline", "historical"), ("sanity", ["FAIL synthetic"]), ("d0", {})])
def test_missing_or_wrong_report_admission(field, value):
    data = results()
    data[field] = value
    with pytest.raises(ValueError):
        delivery.validate_results(data, commit="a" * 40)


def completed_steps(base):
    from atx_db.alpha_panel.a0_check import plan_steps
    commit = "a" * 40
    code = base / "code" / commit[:7] / "atx-db" / "src" / "atx_db"
    code.mkdir(parents=True)
    for name in ("identity_links.py", "identity_reconstruction.py"):
        (code / name).write_bytes(b"synthetic frozen module")
    hashes = {"atx_db/" + name: {"sha256": delivery.digest(code / name)}
              for name in ("identity_links.py", "identity_reconstruction.py")}
    arms = {"commit": commit, "baseline": "repinned", "reconstruction_batch_size": 64,
            "arms": {arm: {} for arm in ("A", "B1", "B2", "B12")}, "modules": hashes,
            "inputs": {key: "synthetic" for key in ("receipt", "ops_manifest", "document_review")}}
    (base / "arms.json").write_text(json.dumps(arms))
    for arm in ("A", "B2"):
        work = base / f"W_{arm}"
        work.mkdir()
        (work / "preparation.json").write_text(json.dumps({"pin": {"a0_s1": arm == "B2",
            "reconstruction_code_sha256": delivery.digest(code / "identity_reconstruction.py")},
            "steps": {"reconstruct": {"reconstruction_batch_size": 64, "candidate_discovery_batch_size": 64,
                "driver_code_sha256": delivery.digest(code / "identity_links.py"), "files": []}}}))
    (base / "steps").mkdir()
    (base / "logs").mkdir()
    for step in plan_steps(base, commit, data_root=base / "data", inputs=arms["inputs"],
                           baseline="repinned", reconstruction_batch_size=64):
        paths = {key: base / "logs" / f"{step['id']}.{key}" for key in ("receipt", "stdout", "stderr")}
        for path in paths.values():
            path.write_text("")
        paths["receipt"].write_text(json.dumps({"status": "completed", "returncode": 0,
            "heavy": True, "job_limit_gb": step["cap"], "cap_hit": False}))
        saved = {**step, "commit": commit, "complete": True, "attempts": [{"exit_code": 0,
            "guard_status": "completed", **{key: str(path) for key, path in paths.items()}}]}
        (base / "steps" / f"{step['id']}.json").write_text(json.dumps(saved))
    return commit


def test_complete_actual64_evidence_and_missing_step_refusal(tmp_path):
    commit = completed_steps(tmp_path)
    evidence = delivery.fixed_step_evidence(tmp_path, commit=commit, data_root=tmp_path / "data")
    assert len([name for name in evidence["files"] if name.startswith("steps/")]) == 47
    (tmp_path / "steps" / "P_B12.unseed.json").unlink()
    with pytest.raises(ValueError, match="missing"):
        delivery.fixed_step_evidence(tmp_path, commit=commit, data_root=tmp_path / "data")


@pytest.mark.parametrize("mutation", ["driver", "batch", "module", "receipt", "step"])
def test_fixed_provenance_cannot_be_inferred_from_success_label(tmp_path, mutation):
    commit = completed_steps(tmp_path)
    if mutation in ("driver", "batch"):
        path = tmp_path / "W_B2" / "preparation.json"
        data = json.loads(path.read_text())
        data["steps"]["reconstruct"]["driver_code_sha256" if mutation == "driver" else "reconstruction_batch_size"] = "wrong"
    elif mutation == "module":
        (tmp_path / "code" / commit[:7] / "atx-db" / "src" / "atx_db" / "identity_links.py").write_bytes(b"changed")
        path = None
    elif mutation == "receipt":
        path = tmp_path / "logs" / "preflight.receipt"
        data = json.loads(path.read_text())
        data["returncode"] = 1
    else:
        path = tmp_path / "steps" / "W_B2.prepare.reconstruct.json"
        data = json.loads(path.read_text())
        data["argv"][-1] = "250"
    if path is not None:
        path.write_text(json.dumps(data))
    with pytest.raises(ValueError):
        delivery.fixed_step_evidence(tmp_path, commit=commit, data_root=tmp_path / "data")


def test_result_seal_keeps_identifier_hashes_and_refuses_overwrite(tmp_path):
    (tmp_path / "a0-results.json").write_text(json.dumps(results()))
    (tmp_path / "a0-results.md").write_text("synthetic report")
    (tmp_path / "identifiers").mkdir()
    (tmp_path / "identifiers" / "diff.parquet").write_bytes(b"identifiers")
    evidence = {"commit": "a" * 40, "baseline": "repinned", "reconstruction_batch_size": 64, "files": {}}
    seal = delivery.seal_results(tmp_path, commit="a" * 40, evidence=evidence, destination=tmp_path / "sealed.json")
    assert seal["report_files"]["identifiers/diff.parquet"] == delivery.digest(tmp_path / "identifiers" / "diff.parquet")
    with pytest.raises(FileExistsError):
        delivery.seal_results(tmp_path, commit="a" * 40, evidence=evidence, destination=tmp_path / "sealed.json")


def test_rehearsal_receipt_build_and_launch_evidence(tmp_path):
    (tmp_path / "build.json").write_bytes(b"frozen build pin")
    (tmp_path / "phase.parquet").write_bytes(b"completed phase")
    (tmp_path / "launch.json").write_bytes(b"actual launch")
    manifest = {"files": [{"file": "phase.parquet", "sha256": delivery.digest(tmp_path / "phase.parquet")}],
                "build_sha256": delivery.digest(tmp_path / "build.json"), "launch": "launch.json"}
    (tmp_path / "phase.json").write_text(json.dumps(manifest))
    seal = delivery.seal_tree(tmp_path, manifests=("phase.json",), required=("build.json",))
    assert "launch.json" in seal["files"]
    (tmp_path / "build.json").write_bytes(b"changed pin")
    with pytest.raises(ValueError, match="build hash"):
        delivery.seal_tree(tmp_path, manifests=("phase.json",), required=("build.json",))


def test_published_comparison_uses_exact_original_and_fixed_paths(tmp_path, monkeypatch):
    from atx_db.alpha_panel import private_identity_arm_comparison as kernel

    def capture(con, **kwargs):
        assert kwargs["baseline"]["pit"].endswith("identity-bridge-v2-pit/links.parquet") or kwargs["baseline"]["pit"].endswith("identity-bridge-v2-pit\\links.parquet")
        assert "identity-bridge-v4-pit" in kwargs["variant"]["pit"]
        assert kwargs["baseline"]["arm"] == "A" and kwargs["variant"]["arm"] == "B1"
        return {"row_8": {"a_only": 7, "b_only": 9}}

    monkeypatch.setattr(kernel, "compare_arms", capture)
    comparison = delivery.compare_published(None, published=tmp_path / "published", fixed=tmp_path / "P_A",
        calendar="calendar", members=["members"], pins={}, output=tmp_path / "evidence")
    assert comparison["kernel_labels"] == {"A": "published v2", "B1": "fixed arm A"}
    assert comparison["rows"]["row_8"] == {"a_only": 7, "b_only": 9}


def test_exact_five_publication_trees_exclude_partial_rehearsal_files(tmp_path):
    from atx_db.identity_links import REHEARSAL_PHASES
    arm = tmp_path / "P_A"
    (arm / "export").mkdir(parents=True)
    for name in delivery.EXPORTS:
        tree(arm / "export" / name)
    identity = arm / "identity"
    identity.mkdir()
    for name in delivery.IDENTITY_FILES:
        (identity / name).write_bytes(b"{}" if name.endswith(".json") else b"synthetic output")
    rehearsal = tmp_path / "R_A"
    (rehearsal / "phases").mkdir(parents=True)
    (rehearsal / "build.json").write_bytes(b"frozen build")
    phase_hashes = {}
    for phase in REHEARSAL_PHASES:
        filename = f"phases/{phase}.parquet"
        (rehearsal / filename).write_bytes(phase.encode())
        receipt = rehearsal / "phases" / f"{phase}.json"
        receipt.write_text(json.dumps({"build_sha256": delivery.digest(rehearsal / "build.json"),
            "files": [{"file": filename, "sha256": delivery.digest(rehearsal / filename)}]}))
        phase_hashes[phase] = delivery.digest(receipt)
    (rehearsal / "manifest.json").write_text(json.dumps({"phases": phase_hashes, "files": []}))
    (rehearsal / "partial.parquet").write_bytes(b"must remain only in source")
    trees = delivery.publication_trees(arm, rehearsal, tmp_path / "data")
    assert [target.name for _, target in trees] == [*delivery.EXPORTS, "identity_v4", "session9-pit-r5"]
    assert "partial.parquet" not in trees[-1][0]["files"]
    assert set(delivery.IDENTITY_FILES) == set(trees[-2][0]["files"])
    (identity / "links_pit.parquet").unlink()
    with pytest.raises(ValueError, match="missing"):
        delivery.publication_trees(arm, rehearsal, tmp_path / "data")
