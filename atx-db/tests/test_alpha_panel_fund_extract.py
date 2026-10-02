"""Stage F extract (fund_extract): stage parametrization, concept and unit filters, one-member batch round trip."""

from __future__ import annotations

import json
import zipfile

import pyarrow.parquet as pq

from atx_db.alpha_panel import common
from atx_db.alpha_panel import fund_extract as fx


def test_stage_name_env(monkeypatch, tmp_path):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    monkeypatch.delenv("ATX_FUND_STAGE", raising=False)
    assert fx.stage_name() == "fundamentals"
    monkeypatch.setenv("ATX_FUND_STAGE", "fundamentals_v10")
    assert fx.stage_name() == "fundamentals_v10"
    assert fx.out_dir() == tmp_path / "fundamentals_v10" / "_work" / "cf"
    assert fx.out_dir().is_dir()


def test_concept_and_unit_filters():
    chain = fx.chain_concepts()
    # every us-gaap and ifrs-full concept is kept (v3: concept mining for the catalog needs the full set)
    assert fx.keep_concept("us-gaap", "RetainedEarningsAccumulatedDeficit", chain)
    assert fx.keep_concept("us-gaap", "SomeRareConcept", chain)
    assert fx.keep_concept("ifrs-full", "Revenue", chain)
    assert fx.keep_concept("dei", fx.fi.DEI_SHARES, chain)
    assert not fx.keep_concept("dei", "EntityPublicFloat", chain)
    assert not fx.keep_concept("srt", "Anything", chain)
    assert fx.keep_unit("USD") and fx.keep_unit("shares") and fx.keep_unit("EUR")
    assert fx.keep_unit("USD/shares") and fx.keep_unit("CAD/shares")
    assert not fx.keep_unit("pure") and not fx.keep_unit("USD/AUD") and not fx.keep_unit("usd")


def test_extract_batch_round_trip(monkeypatch, tmp_path):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    monkeypatch.setenv("ATX_FUND_STAGE", "fund_test")
    payload = {"cik": 1, "facts": {
        "us-gaap": {
            "Assets": {"units": {"USD": [
                {"end": "2023-12-31", "val": 100, "accn": "a1", "fy": 2023, "fp": "FY", "form": "10-K",
                 "filed": "2024-02-20"},
                {"end": "2023-12-31", "val": 100, "accn": "s1", "fy": 2023, "fp": "FY", "form": "S-1",
                 "filed": "2024-02-25"}]}},
            "EarningsPerShareBasic": {"units": {"USD/shares": [
                {"start": "2023-01-01", "end": "2023-12-31", "val": 1.25, "accn": "a1", "fy": 2023, "fp": "FY",
                 "form": "10-K", "filed": "2024-02-20"}]}},
            "EffectiveIncomeTaxRate": {"units": {"pure": [
                {"start": "2023-01-01", "end": "2023-12-31", "val": 0.21, "accn": "a1", "fy": 2023, "fp": "FY",
                 "form": "10-K", "filed": "2024-02-20"}]}}},
        "dei": {"EntityCommonStockSharesOutstanding": {"units": {"shares": [
            {"end": "2024-02-01", "val": 7e6, "accn": "a1", "fy": 2023, "fp": "FY", "form": "10-K",
             "filed": "2024-02-20"}]}}}}}
    arch = tmp_path / "cf.zip"
    with zipfile.ZipFile(arch, "w") as z:
        z.writestr("CIK0000000001.json", json.dumps(payload))
    monkeypatch.setattr(fx, "ARCHIVE", arch)
    with zipfile.ZipFile(arch) as z:
        r = fx.extract_batch(z, 0, fx.plan(z)[0])
    assert r["rows"] == 3 and r["code_version"] == fx.CODE_VERSION
    t = pq.read_table(fx.out_dir() / "batch-0000.parquet").to_pylist()
    assert sorted((x["concept"], x["unit"]) for x in t) == [
        ("Assets", "USD"), ("EarningsPerShareBasic", "USD/shares"), ("EntityCommonStockSharesOutstanding", "shares")]
    assert fx.batch_done(0)
    assert common.build_root() == tmp_path
