"""PIT-b request T3/T4: clocked reconstruction through the Stage I interface."""

import datetime as dt
import json

import duckdb

from atx_db import identity_links as links
from atx_db import identity_reconstruction as ir
from atx_db.alpha_panel import identity_links as stage

D = dt.date
SNAPSHOT = dt.datetime(2026, 9, 20, 0, 6)
CONCEPT = "dei:EntityCommonStockSharesOutstanding"


def _fixture(con):
    lines, runs, facts, filings = [], [], [], []
    serial = 0

    def line(vid, symbol):
        first = D(2018, 12, 28) if vid == 1 else D(2018, 12, 20)
        item = ir.VendorLine(vid, f"TBLTICKERHISTORY-{vid}", first,
                             D(2025, 12, 31), last_symbol=symbol,
                             symbols=((symbol, first, D(2025, 12, 31)),))
        lines.append(item)

    def count(vid, cik, day, filed=None, covering=False):
        nonlocal serial
        serial += 1
        value = 12_345_123 + serial * 100_000
        first = day - dt.timedelta(days=70) if covering else day
        runs.append(ir.VendorShareRun(vid, value // 1000, first, day + dt.timedelta(days=20)))
        facts.append(ir.IssuerShareFact(cik, CONCEPT, day, value, str(serial), "10-Q",
                                       filed or day + dt.timedelta(days=10)))

    # T3: A has 2 shares + 1 lifecycle in 2019. B has 1 share in 2022,
    # an overlapping evidence span, and .5 corroboration only at the snapshot.
    line(1, "RIVAL")
    count(1, "0000000011", D(2019, 1, 1))
    count(1, "0000000011", D(2019, 12, 1))
    filings.append(ir.IssuerFiling("0000000011", "8-A12B", D(2019, 1, 2)))
    count(1, "0000000012", D(2019, 5, 1), D(2022, 1, 1), covering=True)
    count(1, "0000000012", D(2019, 9, 1), D(2022, 1, 1), covering=True)
    # T4 b3: sequential early CIK and a later candidate needing its .5 symbol.
    line(2, "LATER")
    for day in (D(2019, 1, 1), D(2019, 5, 1), D(2019, 9, 1)):
        count(2, "0000000021", day)
    for day in (D(2021, 1, 1), D(2021, 5, 1)):
        count(2, "0000000022", day)
    count(2, "0000000022", D(2021, 9, 1), covering=True)
    # This successor filing dates handover when the later candidate is accepted;
    # it is outside the line's listing window and contributes no lifecycle weight.
    filings.append(ir.IssuerFiling("0000000022", "8-A12B", D(2020, 10, 1)))
    # T4 b2: 3 versus 1.1 fails dominance until the own .5 symbol arrives.
    line(3, "DOMINANT")
    for day in (D(2019, 1, 1), D(2019, 7, 1), D(2019, 12, 1)):
        count(3, "0000000031", day)
    count(3, "0000000032", D(2019, 5, 1), D(2022, 1, 1))
    serial += 1
    runs.append(ir.VendorShareRun(3, 20_000, D(2019, 9, 1), D(2019, 9, 20)))
    facts.append(ir.IssuerShareFact("0000000032", CONCEPT, D(2019, 9, 1),
                                  20_000_000, str(serial), "10-Q", D(2022, 1, 1)))
    ir.stage_share_runs(con, runs)
    ir.stage_share_facts(con, facts)
    return lines, filings


def _stage_rows(con, lines, filings, tickers):
    candidates = []
    for result in ir.reconstruct_in_batches(con, lines, filings=filings, tickers=tickers,
                                            ticker_observed_at=SNAPSHOT,
                                            params=ir.ReconstructionParams(max_extension_days=1500)):
        for row in result.evidence_rows(observed_at=SNAPSHOT, include_rejected=False):
            payload = json.loads(row["value_json"])
            line = next(item for item in lines if item.vendor_id == int(row["native_key"]))
            if hasattr(links, "_reconstruction_candidate_events"):
                candidates.extend(candidate for candidate, _ in
                                  links._reconstruction_candidate_events(row, payload, line, {}, SNAPSHOT))
            else:
                for tier, stamp in payload["tier_history"]:
                    at = dt.datetime.fromisoformat(stamp)
                    proof = links._class_proof_at(line, payload, at, {})
                    candidates.append(dict(assertion_id=row["evidence_id"], perm_security_id=line.vendor_id,
                        cik=row["cik"], link_start=row["valid_from"],
                        link_end=row["valid_to"] - dt.timedelta(days=1), link_basis="reconstructed_" + tier,
                        tier=tier, available_at=at, evidence_ids=json.dumps([row["evidence_id"]]),
                        is_common=proof["class_status"] == "common", class_status=proof["class_status"]))
    versions = []
    for cik in sorted({row["cik"] for row in candidates}):
        versions.extend(links._company_versions([row for row in candidates if row["cik"] == cik], SNAPSHOT))
    shaped = [(r[0], r[1], r[3], r[4], r[5], r[6], r[7], r[8], r[11]) for r in versions]
    return stage.compute_links(shaped, SNAPSHOT.date())[0]


def test_t3_rivals_later_snapshot_preserves_early_stage_i_link(tmp_path):
    with duckdb.connect(str(tmp_path / "t3.duckdb"), config={"memory_limit": "128MB", "threads": 1}) as con:
        lines, filings = _fixture(con)
        rows = _stage_rows(con, lines, filings, [("0000000012", "RIVAL")])
    early = [r for r in rows if r["security_id"] == 1 and r["cik"] == 11 and r["start"] < D(2022, 1, 1)]
    assert early, "T3: rival's 2026 ticker must not erase A's 2019 version"
    assert all(r["tier"] in ("high", "medium") and r["primary"] == "P" for r in early)
    assert min(r["start"] for r in early) == D(2019, 12, 12)
    assert max(r["end_incl"] for r in early) == D(2022, 1, 1)


def test_t4_three_line_snapshot_differential_before_observation(tmp_path):
    with duckdb.connect(str(tmp_path / "t4.duckdb"), config={"memory_limit": "128MB", "threads": 1}) as con:
        lines, filings = _fixture(con)
        assert len(lines) == 3
        weights = {}
        for row in ir.match_share_counts(con, ir.ReconstructionParams()):
            weights[row[1]] = weights.get(row[1], 0.0) + float(row[13])
        assert weights == {"0000000011": 2.0, "0000000012": 1.0, "0000000021": 3.0,
                           "0000000022": 2.5, "0000000031": 3.0, "0000000032": 1.1}
        tickers = [("0000000012", "RIVAL"), ("0000000022", "LATER"), ("0000000031", "DOMINANT")]
        with_snapshot = _stage_rows(con, lines, filings, tickers)
        without_snapshot = _stage_rows(con, lines, filings, [])
        assert with_snapshot == without_snapshot
        result = next(ir.reconstruct_in_batches(con, lines, filings=filings, tickers=tickers,
                      ticker_observed_at=SNAPSHOT, params=ir.ReconstructionParams(max_extension_days=1500)))
        predecessor = next(link for link in result.clocked_links if link.cik == "0000000021")
        successor = next(link for link in result.clocked_links if link.cik == "0000000022")
        assert predecessor.versions[-2].valid_to == D(2021, 1, 1)
        assert predecessor.versions[-2].valid_until == SNAPSHOT
        assert predecessor.versions[-1].valid_to == D(2020, 10, 1)
        assert predecessor.versions[-1].available_at == SNAPSHOT
        assert successor.versions[0].available_at == SNAPSHOT


def test_later_unmatched_reports_withdraw_without_erasing_earlier_versions(tmp_path):
    with duckdb.connect(str(tmp_path / "ratio.duckdb"), config={"memory_limit": "128MB", "threads": 1}) as con:
        lines, filings = _fixture(con)
        for month in (3, 6, 10):
            con.execute("INSERT INTO ri_share_facts VALUES (?,?,?,?,?,?,?)", [
                "0000000011", CONCEPT, D(2019, month, 1), 999_999_123, "unmatched", "10-Q", D(2021, 1, 1)])
        rows = _stage_rows(con, lines, filings, [])
    early = [row for row in rows if row["security_id"] == 1]
    assert early
    assert max(row["end_incl"] for row in early) == D(2021, 1, 1)


def test_later_collision_cannot_prefilter_away_early_support(tmp_path):
    with duckdb.connect(str(tmp_path / "collision.duckdb"), config={"memory_limit": "128MB", "threads": 1}) as con:
        lines, filings = _fixture(con)
        con.execute("INSERT INTO ri_share_facts SELECT '0000000099',concept,as_of,value,accession,form,"
                    "DATE '2022-01-01' FROM ri_share_facts WHERE cik='0000000011'")
        results = list(ir.reconstruct_in_batches(con, lines, filings=filings, ticker_observed_at=SNAPSHOT))
        historic = next(link for result in results for link in result.clocked_links if link.cik == "0000000011")
        assert historic.versions[0].available_at == dt.datetime(2019, 12, 12, 22)
        assert historic.versions[-1].valid_until == dt.datetime(2022, 1, 2, 22)
        assert historic.tier_at(dt.datetime(2022, 1, 2, 22)) is None
        for version in historic.versions:
            assert all(item.known_at <= version.available_at for item in version.items)
            assert all(item.detail["collisions"] == 1 for item in version.items if item.kind == ir.EV_SHARES)


def test_early_weights_recomputed_exactly_before_later_49_way_collision(tmp_path):
    with duckdb.connect(str(tmp_path / "collision49.duckdb"), config={"memory_limit": "128MB", "threads": 1}) as con:
        lines, filings = _fixture(con)
        for rival in range(48):
            con.execute("INSERT INTO ri_share_facts SELECT ?,concept,as_of,value,accession,form,"
                        "DATE '2022-01-01' FROM ri_share_facts WHERE cik='0000000011'",
                        [f"{1000 + rival:010d}"])
        result = next(ir.reconstruct_in_batches(con, lines, filings=filings, ticker_observed_at=SNAPSHOT))
    historic_links = [link for link in result.clocked_links if link.cik == "0000000011"]
    assert historic_links, "later 49-way collisions must not erase the exact early threshold"
    historic = historic_links[0]
    assert historic.versions[0].share_weight == 2.0
    assert historic.versions[0].weight == 3.0
    assert historic.versions[0].available_at == dt.datetime(2019, 12, 12, 22)


def test_native_s1_switch_removes_only_snapshot_from_clocked_replay(tmp_path, monkeypatch):
    import pyarrow as pa
    import pyarrow.parquet as pq

    with duckdb.connect(str(tmp_path / "fixture.duckdb"), config={"memory_limit": "128MB", "threads": 1}) as con:
        fixture_lines, fixture_filings = _fixture(con)
        fixture_runs = [ir.VendorShareRun(*row) for row in con.execute("SELECT * FROM ri_share_runs").fetchall()]
        fixture_facts = [ir.IssuerShareFact(*row) for row in con.execute("SELECT * FROM ri_share_facts").fetchall()]
    ticker_file = tmp_path / "company_tickers.json"
    ticker_file.write_text(json.dumps({"0": {"cik_str": 31, "ticker": "DOMINANT"}}))
    vendor_file = tmp_path / "vendor.parquet"
    pq.write_table(pa.table({"securityID": [1, 2, 3], "earnFlag": ["0", "0", "0"]}), vendor_file)
    inputs = ir.ReconstructionInputs(*(ir.RetainedInput(name, path, "ab" * 32, SNAPSHOT, "synthetic")
        for name, path in (("ticker_history", vendor_file), ("companyfacts", tmp_path / "facts.zip"),
                           ("submissions", tmp_path / "submissions.zip"), ("company_tickers", ticker_file))))
    monkeypatch.setattr(links, "_retained_inputs", lambda receipt: inputs)

    def vendor_input(con, path, **kwargs):
        ir.stage_share_runs(con, fixture_runs)
        con.execute("CREATE TABLE ri_vendor_lines AS SELECT unnest([1,2,3]) AS vendor_id")

    monkeypatch.setattr(ir, "stage_vendor_ticker_history", vendor_input)
    monkeypatch.setattr(ir, "iter_companyfacts_share_facts", lambda path: iter(fixture_facts))
    monkeypatch.setattr(ir, "read_vendor_lines", lambda con: fixture_lines)
    monkeypatch.setattr(ir, "read_lifecycle_filings", lambda path, ciks: fixture_filings)
    results = []
    for disabled in (False, True):
        monkeypatch.setenv("ATX_A0_S1", "1" if disabled else "0")
        work = tmp_path / ("without_snapshot" if disabled else "with_snapshot")
        for step in ("vendor", "facts", "reconstruct"):
            links.prepare_rehearsal_inputs(work, tmp_path / "synthetic-receipt.json", step)
        manifest = json.loads((work / "preparation.json").read_text())
        assert manifest["pin"].get("a0_s1", False) == disabled
        payloads = [json.loads(row["value_json"]) for file in sorted((work / "evidence").glob("*.parquet"))
                    for row in pq.read_table(file).to_pylist() if row["evidence_status"] == "reconstructed"]
        items = [item for payload in payloads for item in payload["items"]]
        assert any(item["kind"] == ir.EV_SYMBOL for item in items) == (not disabled)
        results.append(sorted(json.dumps(item, sort_keys=True) for item in items if item["kind"] != ir.EV_SYMBOL))
    assert results[0] == results[1]
