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


def test_evaluation_metrics_scan_each_candidate_once_per_clock(monkeypatch):
    """Rival comparisons and accepted segments reuse only this clock's metrics."""
    line = ir.VendorLine(1, "line", D(2019, 1, 1), D(2025, 12, 31))
    clock = dt.datetime(2020, 1, 1)
    candidates = [ir.Candidate(1, str(index), (
        ir.EvidenceItem(ir.EV_SHARES, clock, D(2019, 2, 1), 3.0),
    ), match_ratio=1.0) for index in range(8)]
    counts = {"weight": 0, "span": 0}
    original_weight, original_span = ir._share_weight, ir._share_span

    def weight(items):
        counts["weight"] += 1
        return original_weight(items)

    def span(items):
        counts["span"] += 1
        return original_span(items)

    monkeypatch.setattr(ir, "_share_weight", weight)
    monkeypatch.setattr(ir, "_share_span", span)
    accepted, rejected = ir._evaluate_line(line, candidates, {}, ir.ReconstructionParams(), clock)
    assert not accepted and len(rejected) == 8
    assert counts == {"weight": 8, "span": 8}
    # A separate clock must rebuild metrics, including the accepted segment path.
    counts.update(weight=0, span=0)
    later = ir.Candidate(1, "0", (ir.EvidenceItem(ir.EV_SHARES, clock, D(2019, 2, 1), 4.5),),
                         match_ratio=1.0)
    accepted, rejected = ir._evaluate_line(line, [later], {}, ir.ReconstructionParams(),
                                          clock + dt.timedelta(days=1))
    assert len(accepted) == 1 and not rejected
    assert accepted[0].weight == 4.5 and accepted[0].share_weight == 4.5
    assert counts == {"weight": 1, "span": 1}


def test_evaluation_preserves_fsum_and_share_corroboration_gate():
    line = ir.VendorLine(1, "line", D(2019, 1, 1), D(2025, 12, 31))
    clock = dt.datetime(2020, 1, 1)
    # Sequential summation loses the unit term; the original fsum retains it.
    items = tuple(ir.EvidenceItem(ir.EV_SHARES, clock, D(2019, 2, 1), value)
                  for value in (1e16, 1.0, -1e16))
    candidate = ir.Candidate(1, "1", items, match_ratio=1.0)
    params = ir.ReconstructionParams(accept_weight=1.0, min_share_weight=1.0)
    accepted, rejected = ir._evaluate_line(line, [candidate], {}, params, clock)
    assert not rejected and accepted[0].weight == accepted[0].share_weight == 1.0
    corroboration = ir.Candidate(1, "2", (
        ir.EvidenceItem(ir.EV_LISTING, clock, D(2019, 2, 1), 100.0),
    ), match_ratio=1.0)
    accepted, rejected = ir._evaluate_line(line, [corroboration], {}, params, clock)
    assert not accepted and rejected[0].reason == ir.REJECT_BELOW_THRESHOLD
    assert rejected[0].weight == rejected[0].share_weight == 0.0


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
        payloads = [json.loads(row["value_json"]) for file in (work / item["file"] for item in manifest["steps"]["reconstruct"]["files"])
                    for row in pq.read_table(file).to_pylist() if row["evidence_status"] == "reconstructed"]
        items = [item for payload in payloads for item in payload["items"]]
        assert any(item["kind"] == ir.EV_SYMBOL for item in items) == (not disabled)
        results.append(sorted(json.dumps(item, sort_keys=True) for item in items if item["kind"] != ir.EV_SYMBOL))
    assert results[0] == results[1]

def _review_clock_fixture(con, future_runs=False):
    cik = "0000000011"
    line = ir.VendorLine(1, "TBLTICKERHISTORY-1", D(2019, 1, 1), D(2019, 12, 31))
    runs = [ir.VendorShareRun(1, 12345, D(2019, 1, 1), D(2019, 1, 2)),
            ir.VendorShareRun(1, 12445, D(2019, 1, 3), D(2019, 1, 20))]
    if future_runs:
        runs += [ir.VendorShareRun(1, 90000 + day, D(2019, 1, day), D(2019, 1, day))
                 for day in (10, 11, 12)]
    ir.stage_share_runs(con, runs)
    ir.stage_share_facts(con, [ir.IssuerShareFact(cik, CONCEPT, D(2019, 1, day), value,
                          str(day), "10-Q", D(2019, 1, 3))
                          for day, value in ((1, 12345123), (3, 12445123))])
    early = ir.IssuerFiling(cik, "8-A12B", D(2019, 1, 5), accepted_at=dt.datetime(2019, 1, 5, 12))
    return line, early


def test_late_closer_listing_preserves_earlier_eligibility_clock(tmp_path):
    with duckdb.connect(str(tmp_path / "lifecycle-clock.duckdb"), config={"memory_limit": "128MB", "threads": 1}) as con:
        line, early = _review_clock_fixture(con)
        late = ir.IssuerFiling(early.cik, "8-A12B", D(2019, 1, 2), accepted_at=dt.datetime(2022, 1, 1, 12))
        def replay(filings):
            return ir.reconstruct_issuer_links(con, [line], filings=filings,
                       price_start=D(2018, 1, 1), horizon=D(2019, 12, 31))
        baseline = replay([early]).clocked_links[0].versions[0]
        historic = replay([early, late]).clocked_links[0].versions[0]
    assert baseline.available_at == dt.datetime(2019, 1, 6, 22)
    assert historic.available_at == baseline.available_at
    assert historic.weight == baseline.weight == 3.0
    assert next(item for item in historic.items if item.kind == ir.EV_LISTING).detail["filing_date"] == early.filing_date


def test_future_unmatched_vendor_runs_withdraw_only_at_observation(tmp_path):
    with duckdb.connect(str(tmp_path / "vendor-clock.duckdb"), config={"memory_limit": "128MB", "threads": 1}) as con:
        line, early = _review_clock_fixture(con, future_runs=True)
        result = ir.reconstruct_issuer_links(con, [line], filings=[early],
                         price_start=D(2018, 1, 1), horizon=D(2019, 12, 31))
    assert result.clocked_links, "future unmatched vendor runs must preserve earlier acceptance"
    historic = result.clocked_links[0].versions[0]
    assert historic.available_at == dt.datetime(2019, 1, 6, 22)
    assert historic.share_weight == 2.0
    assert historic.valid_until == dt.datetime(2019, 1, 10, 22)
    assert result.clocked_links[0].tier_at(dt.datetime(2019, 1, 10, 22)) is None


def test_partition_size_preserves_full_clocked_results_across_67_lines(tmp_path):
    from dataclasses import asdict, replace

    with duckdb.connect(str(tmp_path / "partitions.duckdb"), config={"memory_limit": "128MB", "threads": 1}) as con:
        lines, filings = _fixture(con)
        original_runs = con.execute("SELECT * FROM ri_share_runs WHERE vendor_id=1").fetchall()
        for vid in range(4, 68):
            lines.append(replace(lines[0], vendor_id=vid, price_security_id=f"TBLTICKERHISTORY-{vid}"))
            con.executemany("INSERT INTO ri_share_runs VALUES (?,?,?,?,?)",
                            [(vid, *row[1:]) for row in original_runs])
        # Same counts/CIKs on both sides of64; late rival collision must remain per vendor line.
        con.execute("INSERT INTO ri_share_facts SELECT '0000000099',concept,as_of,value,accession,form,"
                    "DATE '2022-01-01' FROM ri_share_facts WHERE cik='0000000011'")
        def serialized(size, subset):
            evidence, results = [], {name: [] for name in ('links', 'clocked_links', 'rejected')}
            considered = 0
            for batch in ir.reconstruct_in_batches(con, subset, filings=filings,
                    tickers=[('0000000012', 'RIVAL')], ticker_observed_at=SNAPSHOT,
                    params=ir.ReconstructionParams(max_extension_days=1500), batch_size=size):
                evidence.extend(batch.evidence_rows(observed_at=SNAPSHOT))
                considered += batch.lines_considered
                for name in results:
                    results[name].extend(asdict(item) for item in getattr(batch, name))
            def canonical(rows):
                return sorted(json.dumps(row, sort_keys=True, default=str) for row in rows)
            return canonical(evidence), {name: canonical(rows) for name, rows in results.items()}, considered
        large = serialized(250, lines)
        assert large == serialized(64, lines)
        assert large[2] == 67
        assert serialized(250, lines[:3]) == serialized(1, lines[:3])
        payloads = [json.loads(json.loads(row)['value_json']) for row in large[0]
                    if json.loads(row)['evidence_status'] == 'reconstructed']
        assert any(version['from'] < '2020' and version['until'] is not None
                   for payload in payloads for version in payload['versions'])


def test_native_batch_cli_preserves_preparation_pin_and_isolates_retry_parts(tmp_path, monkeypatch):
    import sys

    import pyarrow as pa
    import pyarrow.parquet as pq
    import pytest

    with duckdb.connect(str(tmp_path / "native-batch.duckdb"), config={"memory_limit": "128MB", "threads": 1}) as con:
        fixture_lines, fixture_filings = _fixture(con)
        runs = [ir.VendorShareRun(*row) for row in con.execute("SELECT * FROM ri_share_runs").fetchall()]
        facts = [ir.IssuerShareFact(*row) for row in con.execute("SELECT * FROM ri_share_facts").fetchall()]
    ticker = tmp_path / 'tickers.json'
    ticker.write_text('{}')
    vendor = tmp_path / 'vendor.parquet'
    pq.write_table(pa.table({'securityID':[1,2,3], 'earnFlag':['0','0','0']}), vendor)
    inputs = ir.ReconstructionInputs(*(ir.RetainedInput(name,path,'ab'*32,SNAPSHOT,'synthetic')
        for name,path in (('ticker_history',vendor),('companyfacts',tmp_path/'facts.zip'),
                          ('submissions',tmp_path/'submissions.zip'),('company_tickers',ticker))))
    monkeypatch.setattr(links,'_retained_inputs',lambda receipt:inputs)
    def vendor_input(con,path,**kwargs):
        ir.stage_share_runs(con,runs)
        con.execute('CREATE TABLE ri_vendor_lines AS SELECT unnest([1,2,3]) AS vendor_id')
    monkeypatch.setattr(ir,'stage_vendor_ticker_history',vendor_input)
    monkeypatch.setattr(ir,'iter_companyfacts_share_facts',lambda path:iter(facts))
    monkeypatch.setattr(ir,'read_vendor_lines',lambda con:fixture_lines)
    monkeypatch.setattr(ir,'read_lifecycle_filings',lambda path,ciks:fixture_filings)
    work = tmp_path/'prepared'
    receipt = tmp_path/'receipt.json'
    for step in ('vendor','facts'):
        links.prepare_rehearsal_inputs(work,receipt,step)
    pin = json.loads((work/'preparation.json').read_text())['pin']
    output = work/'evidence'
    output.mkdir()
    stale = output/'part-0000.parquet'
    stale.write_bytes(b'preserved incomplete250 attempt')
    monkeypatch.setattr(sys,'argv',['identity_links','prepare','--work',str(work),'--receipt',str(receipt),
                                   '--step','reconstruct','--reconstruction-batch-size','64'])
    assert links.main() == 0
    manifest = json.loads((work/'preparation.json').read_text())
    assert manifest['pin'] == pin
    result = manifest['steps']['reconstruct']
    assert result['reconstruction_batch_size'] == result['candidate_discovery_batch_size'] == 64
    assert result['driver_code_sha256'] == links._LOADED_CODE_SHA256
    assert stale.read_bytes() == b'preserved incomplete250 attempt'
    assert all(item['file'].startswith('evidence/attempt-') for item in result['files'])
    assert all((work/item['file']).exists() for item in result['files'])
    reused = links.prepare_rehearsal_inputs(work,receipt,'reconstruct',reconstruction_batch_size=250)
    assert reused['reused'] and reused['reconstruction_batch_size'] == 64
    assert json.loads((work/'preparation.json').read_text()) == manifest
    # Legacy completed metadata has unknown execution settings; reuse must not invent them.
    for key in ('reconstruction_batch_size', 'candidate_discovery_batch_size', 'driver_code_sha256'):
        del manifest['steps']['reconstruct'][key]
    (work/'preparation.json').write_text(json.dumps(manifest))
    legacy = links.prepare_rehearsal_inputs(work,receipt,'reconstruct',reconstruction_batch_size=64)
    assert 'reconstruction_batch_size' not in legacy and 'driver_code_sha256' not in legacy
    assert json.loads((work/'preparation.json').read_text()) == manifest
    monkeypatch.setattr(links,'_retained_inputs',lambda receipt:pytest.fail('invalid config read inputs'))
    invalid = tmp_path/'invalid'
    with pytest.raises(ValueError,match='positive'):
        links.prepare_rehearsal_inputs(invalid,receipt,'vendor',reconstruction_batch_size=0)
    monkeypatch.setattr(sys,'argv',['identity_links','prepare','--work',str(invalid),'--receipt',str(receipt),
                                   '--step','vendor','--reconstruction-batch-size','0'])
    with pytest.raises(ValueError,match='positive'):
        links.main()
    assert not invalid.exists()
