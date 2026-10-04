import gzip
import hashlib
import json
from pathlib import Path

import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import thirteenf_native as N


def _fixture(tmp_path, *, compressed=False, max_originals=10):
    raw=b"""<SEC-DOCUMENT>
ACCESSION NUMBER: 0000000001-20-000001
CONFORMED SUBMISSION TYPE: 13F-HR
CENTRAL INDEX KEY: 0000000001
<ACCEPTANCE-DATETIME>20200515120000
<XML><edgarSubmission><reportCalendarOrQuarter>03-31-2020</reportCalendarOrQuarter><tableEntryTotal>1</tableEntryTotal><tableValueTotal>10</tableValueTotal></edgarSubmission></XML>
<XML><informationTable><infoTable><cusip>123456789</cusip><value>10</value><sshPrnamt>100</sshPrnamt><sshPrnamtType>SH</sshPrnamtType><investmentDiscretion>SOLE</investmentDiscretion></infoTable></informationTable></XML>
</SEC-DOCUMENT>"""
    original=tmp_path / ("original.gz" if compressed else "original.txt")
    original.write_bytes(gzip.compress(raw) if compressed else raw)
    receipts=tmp_path / "receipts.jsonl"
    receipt=dict(accession="000000000120000001",path=str(original),sha256=N.C.sha256_file(original),
        url="https://www.sec.gov/Archives/edgar/data/1/000000000120000001/complete.txt",
        fetched_at="2026-01-01T00:00:00Z",verified_original=True)
    line=(json.dumps(receipt)+"\n").encode()
    receipts.write_bytes(line)
    expected=tmp_path / "expected.jsonl"
    expected.write_text(json.dumps(dict(accession=receipt["accession"],cik=1,form="13F-HR",report_date="2020-03-31"))+"\n"+
                        json.dumps(dict(accession="000000000120000002",cik=1,form="13F-NT"))+"\n")
    proof=tmp_path / "official-proof.txt"
    proof.write_text("fixture documentary source")
    bundle=tmp_path / "rules.json"
    document=dict(path=str(proof),bytes=proof.stat().st_size,sha256=N.C.sha256_file(proof),
                  rule_id="SELF_ASSERTED_APPROVED",source_url="https://www.sec.gov/fixture",
                  publication_clock="2019-01-01T00:00:00Z",qualified=True)
    bundle.write_text(json.dumps(dict(schema="atx.thirteenf-documentary-proof/v1",documents=[document])))
    products=[]
    for role,path in (("expected_inventory",expected),("retained_receipts",receipts),("rule_bundle",bundle)):
        products.append(dict(role=role,path=str(path),bytes=path.stat().st_size,sha256=N.C.sha256_file(path)))
    obj=dict(accession=receipt["accession"],path=str(original),bytes=original.stat().st_size,
        sha256=N.C.sha256_file(original),reader_type="gzip_sec_submission_text" if compressed else "sec_submission_text",
        receipt_line_sha256=hashlib.sha256(line).hexdigest())
    plan=dict(schema="atx.thirteenf-native-plan/v1",roots=[str(tmp_path)],products=products,
              originals=[obj],generation=str(tmp_path / "generation"),max_originals=max_originals)
    plan_path=tmp_path / "plan.json"
    plan_path.write_text(json.dumps(plan))
    return plan_path,plan


@pytest.mark.parametrize("compressed",[False,True])
def test_native_originals_cli_retains_full_denominator_and_unqualified_raw_positions(tmp_path,compressed):
    plan_path,plan=_fixture(tmp_path,compressed=compressed)
    assert N.main(["--plan",str(plan_path)])==0
    generation=Path(plan["generation"])
    manifest=json.loads((generation / "manifest.json").read_text())
    admissions=pq.read_table(generation / "admissions.parquet").to_pylist()
    positions=pq.read_table(generation / "positions.parquet").to_pylist()
    assert manifest["expected_accessions"]==2 and manifest["processed_originals"]==1
    assert manifest["counts"]["admissions"]==2 and manifest["gate_status"]=="UNQUALIFIED"
    assert not manifest["source_rules_supported"] and not manifest["contemporaneous_population_complete"]
    assert admissions[0]["source_retained"] and admissions[0]["cover_reconciled"]
    assert admissions[0]["complete_container_observed"] and admissions[0]["filer_cik"]==1
    assert all(row["available_at"] is None and not row["source_qualified"] for row in admissions)
    assert "retained_original_reader_missing" in admissions[1]["exceptions"]
    assert positions[0]["shares_reported"]==100 and positions[0]["value_raw"]==10
    assert positions[0]["value_usd"] is None and positions[0]["available_at"] is None
    assert N.validate(generation)["structural_status"]=="PASS"
    assert admissions[0]["raw_sha256"]!=admissions[0]["object_sha256"] if compressed else admissions[0]["raw_sha256"]==admissions[0]["object_sha256"]
    with pytest.raises(FileExistsError):
        N.build(plan_path)


def test_native_reader_missing_products_and_self_asserted_receipt_fail(tmp_path):
    plan_path,plan=_fixture(tmp_path)
    plan["products"]=plan["products"][:-1]
    plan_path.write_text(json.dumps(plan))
    with pytest.raises(ValueError,match="complete_exact_reader"):
        N.build(plan_path)
    plan_path,plan=_fixture(tmp_path)
    plan["originals"][0]["receipt_line_sha256"]="0"*64
    plan_path.write_text(json.dumps(plan))
    N.build(plan_path)
    rows=pq.read_table(Path(plan["generation"]) / "admissions.parquet").to_pylist()
    assert "exact_retained_receipt_record_missing_or_mismatch" in rows[0]["exceptions"]
    assert not rows[0]["source_retained"]


def test_native_unsupported_member_and_empty_inventory_remain_explicit(tmp_path):
    plan_path,plan=_fixture(tmp_path)
    plan["originals"][0]["reader_type"]="zip_member"
    plan_path.write_text(json.dumps(plan))
    N.build(plan_path)
    rows=pq.read_table(Path(plan["generation"]) / "admissions.parquet").to_pylist()
    assert "original_reader_type_unsupported" in rows[0]["exceptions"]
    assert pq.read_table(Path(plan["generation"]) / "positions.parquet").num_rows==0
    empty=tmp_path / "empty"
    empty.mkdir()
    plan_path,plan=_fixture(empty)
    expected=next(row for row in plan["products"] if row["role"]=="expected_inventory")
    Path(expected["path"]).write_bytes(b"")
    expected.update(bytes=0,sha256=N.C.sha256_file(Path(expected["path"])))
    plan_path.write_text(json.dumps(plan))
    manifest=N.build(plan_path)
    assert manifest["status"]=="partial" and manifest["expected_accessions"]==0
    assert manifest["counts"]["rejects"]==2 and not manifest["source_rules_supported"]


def test_native_interrupted_manifest_preserves_uncommitted_generation(tmp_path,monkeypatch):
    plan_path,plan=_fixture(tmp_path)
    real_link=N.os.link
    def interrupted(source,destination):
        if Path(destination).name=="manifest.json":
            raise InterruptedError("fixture manifest interruption")
        return real_link(source,destination)
    monkeypatch.setattr(N.os,"link",interrupted)
    with pytest.raises(InterruptedError):
        N.build(plan_path)
    generation=Path(plan["generation"])
    assert not (generation / "manifest.json").exists()
    assert (generation / "manifest.json.partial").exists()
    assert (generation / "admissions.parquet").exists()


def test_native_selection_bound_preserves_expected_denominator_and_partial_status(tmp_path):
    plan_path,plan=_fixture(tmp_path,max_originals=1)
    plan["originals"].append(dict(plan["originals"][0],accession="000000000120000002"))
    plan_path.write_text(json.dumps(plan))
    manifest=N.build(plan_path)
    rows=pq.read_table(Path(plan["generation"]) / "admissions.parquet").to_pylist()
    assert manifest["bound_reached"] and manifest["status"]=="partial"
    assert manifest["expected_accessions"]==2 and manifest["counts"]["admissions"]==2
    assert "selected_original_bound_not_processed" in rows[1]["exceptions"]
    assert "documentary_admission_rules_not_independently_established" in rows[1]["exceptions"]


def test_native_consumed_original_mutation_fails_without_final_manifest(tmp_path,monkeypatch):
    plan_path,plan=_fixture(tmp_path)
    parse=N.original_submission_evidence
    def mutate(raw,expected,receipt):
        result=parse(raw,expected,receipt)
        Path(plan["originals"][0]["path"]).write_bytes(b"changed source")
        return result
    monkeypatch.setattr(N,"original_submission_evidence",mutate)
    with pytest.raises(ValueError,match="consumed_file_byte_identity_or_bound"):
        N.build(plan_path)
    assert not (Path(plan["generation"]) / "manifest.json").exists()


def test_native_output_collision_is_no_clobber_with_partial_evidence(tmp_path,monkeypatch):
    plan_path,plan=_fixture(tmp_path)
    real_link=N.os.link
    def collision(source,destination):
        if Path(destination).name=="admissions.parquet":
            Path(destination).write_bytes(b"other publisher")
        return real_link(source,destination)
    monkeypatch.setattr(N.os,"link",collision)
    with pytest.raises(FileExistsError):
        N.build(plan_path)
    generation=Path(plan["generation"])
    assert (generation / "admissions.parquet").read_bytes()==b"other publisher"
    assert (generation / "admissions.parquet.partial").exists()
    assert not (generation / "manifest.json").exists()


def test_native_duplicate_reader_keys_fail_and_extra_member_fields_are_diagnostics(tmp_path):
    plan_path,plan=_fixture(tmp_path)
    plan["originals"].append(plan["originals"][0].copy())
    plan_path.write_text(json.dumps(plan))
    with pytest.raises(ValueError,match="duplicate_original_reader_key"):
        N.build(plan_path)
    plan["originals"]=plan["originals"][:1]
    plan["originals"][0]["member"]="unread.zip-member"
    plan_path.write_text(json.dumps(plan))
    N.build(plan_path)
    rows=pq.read_table(Path(plan["generation"]) / "admissions.parquet").to_pylist()
    assert "unsupported_original_reader_fields" in rows[0]["exceptions"]


def test_native_validator_requires_commit_and_detects_changed_output(tmp_path):
    plan_path,plan=_fixture(tmp_path)
    generation=Path(plan["generation"])
    with pytest.raises(FileNotFoundError):
        N.validate(generation)
    N.build(plan_path)
    assert N.main(["--validate",str(generation)])==0
    (generation / "positions.parquet").write_bytes(b"changed output")
    with pytest.raises(ValueError,match="native_output_identity_mismatch"):
        N.validate(generation)
