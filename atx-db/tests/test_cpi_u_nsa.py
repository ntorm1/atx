"""CPI-U NSA stage (``cpi_u_nsa``): budgeted fetch client, release-instant clock, vintage rows, C5 manifest."""

from __future__ import annotations

import datetime as dt
import json

import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import compustat_std_parts as P
from atx_db.alpha_panel import cpi_u_nsa as U

T = dt.datetime
D = dt.date


class FakeTransport:
    def __init__(self, status=200, body=b"x", headers=None):
        self.calls = []
        self.status, self.body, self.headers = status, body, headers or {"content-type": "text/csv"}

    def __call__(self, url, params):
        self.calls.append((url, params))
        return self.status, self.body, self.headers


@pytest.fixture
def raw(tmp_path, monkeypatch):
    monkeypatch.setattr(U.time, "sleep", lambda s: None)
    return tmp_path / "raw"


@pytest.fixture
def root(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "v1"))
    return tmp_path / "v1"


# ---------------------------------------------------------------- fetch client
def test_fetch_refuses_hosts_outside_the_allowlist(raw):
    t = FakeTransport()
    with pytest.raises(U.NetworkRefused, match="host"):
        U.fetch("k", "https://example.com/cpi.csv", raw=raw, transport=t)
    with pytest.raises(U.NetworkRefused, match="host"):
        U.fetch("k", "http://fred.stlouisfed.org/graph/fredgraph.csv", raw=raw, transport=t)   # https only
    assert t.calls == [] and not (raw / "receipts.jsonl").exists()


def test_fetch_lands_the_body_with_a_receipt(raw):
    t = FakeTransport(body=b"observation_date,CPIAUCNS\n2024-01-01,308.417\n")
    rec = U.fetch("fred_latest", U.FRED_GRAPH_URL, {"id": "CPIAUCNS"}, raw=raw, transport=t)
    assert rec["http_status"] == 200 and rec["request_no"] == 1 and rec["host"] == "fred.stlouisfed.org"
    assert (raw / rec["file"]).read_bytes() == t.body
    assert rec["sha256"] == C.sha256_file(raw / rec["file"]) and rec["bytes"] == len(t.body)
    assert rec["url"] == U.FRED_GRAPH_URL + "?id=CPIAUCNS"
    lines = (raw / "receipts.jsonl").read_text(encoding="utf-8").splitlines()
    assert [json.loads(x)["key"] for x in lines] == ["fred_latest"]


def test_every_attempt_counts_and_the_cap_is_cumulative(raw):
    bad = FakeTransport(status=403, body=b"Access Denied")
    rec = U.fetch("bls_page", "https://www.bls.gov/schedule/news_release/cpi.htm", raw=raw, transport=bad)
    assert rec["http_status"] == 403 and rec["request_no"] == 1 and "file" in rec      # failures are receipted too
    led = U.ledger(raw)
    for i in range(U.MAX_REQUESTS - 1):
        led.append({"key": f"old{i}", "kind": "http", "fetched_at": "2026-10-05T00:00:00+00:00"})
    t = FakeTransport()
    with pytest.raises(U.NetworkRefused, match="budget"):
        U.fetch("one_more", U.FRED_GRAPH_URL, {"id": "CPIAUCNS"}, raw=raw, transport=t)
    assert t.calls == []


def test_requests_are_spaced_at_least_the_host_interval(raw, monkeypatch):
    waits = []
    monkeypatch.setattr(U.time, "sleep", waits.append)
    t = FakeTransport()
    U.fetch("a", U.FRED_GRAPH_URL, {"id": "A"}, raw=raw, transport=t)
    U.fetch("b", U.FRED_GRAPH_URL, {"id": "B"}, raw=raw, transport=t)
    assert len(t.calls) == 2 and waits and max(waits) > 1.0 and max(waits) <= U.MIN_INTERVAL_S


# ---------------------------------------------------------------- clock
def test_release_instant_is_0830_new_york_in_utc_across_dst():
    assert U.release_instant(D(2023, 2, 14)) == T(2023, 2, 14, 13, 30)     # EST
    assert U.release_instant(D(2023, 3, 14)) == T(2023, 3, 14, 12, 30)     # EDT from 2023-03-12
    assert U.release_instant(D(2023, 10, 12)) == T(2023, 10, 12, 12, 30)   # EDT
    assert U.release_instant(D(2023, 11, 14)) == T(2023, 11, 14, 13, 30)   # EST from 2023-11-05


# ---------------------------------------------------------------- ALFRED vintages
PAGE = """<form name="form" method="post">
<input type="text" id="form_obs_start_date" name="form[obs_start_date]" required="required" maxlength="10" value="1913-01-01"> to
<input type="text" id="form_obs_end_date" name="form[obs_end_date]" required="required" maxlength="10" value="2024-02-01">
<select id="form_selected_vintage_dates" name="form[selected_vintage_dates][]" size="10" multiple="multiple">
<option value="2023-11-14">2023-11-14</option> <option value="2023-12-12">2023-12-12</option>
<option value="2024-01-11">2024-01-11</option> <option value="2024-02-13">2024-02-13</option>
<option value="2024-03-12">2024-03-12</option>
</select></form>"""


def test_alfred_page_lists_vintages_and_the_latest_observation():
    latest, vintages = U.alfred_page(PAGE)
    assert latest == D(2024, 2, 1)
    assert vintages == [D(2023, 11, 14), D(2023, 12, 12), D(2024, 1, 11), D(2024, 2, 13), D(2024, 3, 12)]


def test_vintage_batches_cover_every_vintage_from_the_start():
    vint = [D(2023, 11, 14), D(2023, 12, 12), D(2024, 1, 11), D(2024, 2, 13), D(2024, 3, 12)]
    batches = U.vintage_batches(vint, D(2023, 12, 1), size=3)
    assert [(k, p["vintage_date"]) for k, p in batches] == [
        ("alfred_vintages_2023-12-12_2024-02-13", "2023-12-12,2024-01-11,2024-02-13"),
        ("alfred_vintages_2024-03-12_2024-03-12", "2024-03-12")]
    assert batches[0][1]["id"] == "CPIAUCNS,CPIAUCNS,CPIAUCNS" and batches[0][1]["cosd"] == ",".join(["2023-12-01"] * 3)


CSV1 = ("observation_date,CPIAUCNS_20231212,CPIAUCNS_20240111,CPIAUCNS_20240213\n"
        "2023-10-01,307.671,307.671,307.671\n"
        "2023-11-01,307.051,307.051,307.051\n"
        "2023-12-01,,306.746,306.750\n"           # revised in the 2024-02-13 vintage
        "2024-01-01,,,308.417\n")
CSV2 = ("observation_date,CPIAUCNS_20240312\n2023-10-01,307.671\n2023-11-01,307.051\n2023-12-01,306.750\n"
        "2024-01-01,308.417\n2024-02-01,310.326\n")


def test_parse_alfred_vintage_csv():
    vint, obs = U.parse_alfred_csv(CSV1)
    assert vint == [D(2023, 12, 12), D(2024, 1, 11), D(2024, 2, 13)]
    assert obs[D(2023, 12, 1)] == [None, 306.746, 306.75] and obs[D(2024, 1, 1)] == [None, None, 308.417]


def test_vintage_rows_keep_first_release_and_revision_never_overwritten():
    vint, obs = U.merge_vintages([U.parse_alfred_csv(CSV1), U.parse_alfred_csv(CSV2)])
    rows, info = U.vintage_rows(vint, obs, clock_bounds={v: U.vintage_bound(v) for v in vint}, clock_basis=U.CLOCK_BASIS, start=D(2023, 12, 1))
    got = [(r["ref_month"], r["vintage_date"], r["value"], r["vintage_kind"]) for r in rows]
    assert got == [(D(2023, 12, 1), D(2024, 1, 11), 306.746, "first_release"),
                   (D(2023, 12, 1), D(2024, 2, 13), 306.75, "revision"),
                   (D(2024, 1, 1), D(2024, 2, 13), 308.417, "first_release"),
                   (D(2024, 2, 1), D(2024, 3, 12), 310.326, "first_release")]
    assert rows[0]["available_at"] == T(2024, 1, 13) and rows[3]["available_at"] == T(2024, 3, 14)
    assert all(r["clock_basis"] == U.CLOCK_BASIS for r in rows)
    assert info["unchanged_republications"] == 2 and info["vanished"] == [] and info["excluded_first_in_selection"] == []


def test_a_month_already_in_the_first_selected_vintage_is_excluded_not_guessed():
    vint, obs = U.parse_alfred_csv(CSV1)
    rows, info = U.vintage_rows(vint, obs, clock_bounds={v: U.vintage_bound(v) for v in vint}, clock_basis=U.CLOCK_BASIS, start=D(2023, 10, 1))
    assert [r["ref_month"] for r in rows if r["vintage_kind"] == "first_release"] == [D(2023, 12, 1), D(2024, 1, 1)]
    assert info["excluded_first_in_selection"] == [D(2023, 10, 1), D(2023, 11, 1)]


def test_a_value_that_vanishes_from_a_later_vintage_is_reported():
    vint, obs = U.parse_alfred_csv("observation_date,CPIAUCNS_20231212,CPIAUCNS_20240111,CPIAUCNS_20240213\n"
                                   "2023-12-01,,306.746,\n")
    rows, info = U.vintage_rows(vint, obs, clock_bounds={v: U.vintage_bound(v) for v in vint}, clock_basis=U.CLOCK_BASIS, start=D(2023, 12, 1))
    assert len(rows) == 1 and info["vanished"] == [(D(2023, 12, 1), D(2024, 2, 13))]


class FakeAlfred:
    """Serves the download page and ``alfredgraph.csv`` holding at most ``cap`` of the requested vintages."""

    def __init__(self, cap=12):
        self.cap, self.calls = cap, []

    def __call__(self, url, params):
        self.calls.append((url, dict(params or {})))
        if "downloaddata" in url:
            return 200, PAGE.encode(), {"content-type": "text/html"}
        if url == U.ALFRED_GRAPH_URL:
            vint = params["vintage_date"].split(",")[:self.cap]
            head = "observation_date," + ",".join("CPIAUCNS_" + v.replace("-", "") for v in vint)
            body = head + "\n2023-10-01," + ",".join("307.671" for _ in vint) + "\n"
            return 200, body.encode(), {"content-type": "application/csv"}
        return 200, b"{}", {"content-type": "application/csv"}


def test_vintage_batches_put_the_priority_window_first():
    vint = [D(2019, 11, 13), D(2019, 12, 11), D(2020, 1, 14), D(2020, 2, 13)]
    batches = U.vintage_batches(vint, D(2019, 11, 1), size=3, priority_from=D(2020, 1, 1))
    assert [p["vintage_date"] for _, p in batches] == ["2020-01-14,2020-02-13", "2019-11-13,2019-12-11"]


def test_download_reuses_landed_vintages_and_follows_a_server_line_cap(raw):
    t = FakeAlfred(cap=2)
    first = U.download(raw=raw, transport=t, obs_start=D(2023, 12, 1), batch=3)
    graph = [p["vintage_date"] for u, p in t.calls if u == U.ALFRED_GRAPH_URL]
    # 3 asked, 2 served; the third vintage is asked again with the next one
    assert graph == ["2023-12-12,2024-01-11,2024-02-13", "2024-02-13,2024-03-12"]
    assert [u for u, _ in t.calls][-2:] == [U.FRED_GRAPH_URL, U.BLS_API_URL]
    assert first["requests_sent"] == 5 and first["vintages_missing"] == []
    again = U.download(raw=raw, transport=t, obs_start=D(2023, 12, 1), batch=3)
    assert len(t.calls) == 5 and again["requests_sent"] == 0


def test_download_stops_when_the_server_returns_nothing_new(raw):
    t = FakeAlfred(cap=0)
    res = U.download(raw=raw, transport=t, obs_start=D(2023, 12, 1), batch=3)
    assert len([u for u, _ in t.calls if u == U.ALFRED_GRAPH_URL]) == 1
    assert res["stalled"] and len(res["vintages_missing"]) == 4


# ---------------------------------------------------------------- build
BLS = {"status": "REQUEST_SUCCEEDED", "Results": {"series": [{"seriesID": "CUUR0000SA0", "data": [
    {"year": "2024", "period": "M02", "value": "310.326", "latest": "true"},
    {"year": "2024", "period": "M01", "value": "308.417"},
    {"year": "2023", "period": "M12", "value": "306.746"},       # BLS disagrees with the latest ALFRED vintage
    {"year": "2023", "period": "M13", "value": "304.702"}]}]}}
FRED = "observation_date,CPIAUCNS\n2023-12-01,306.750\n2024-01-01,308.417\n2024-02-01,310.326\n"


def _land(raw, key, body, ctype="application/csv"):
    def t(url, params):
        return 200, body.encode(), {"content-type": ctype}
    return U.fetch(key, U.ALFRED_GRAPH_URL, raw=raw, transport=t)


def _raw(raw):
    _land(raw, "alfred_downloaddata_page", PAGE, "text/html")
    _land(raw, "alfred_vintages_2023-12-12_2024-02-13", CSV1)
    _land(raw, "alfred_vintages_2024-03-12_2024-03-12", CSV2)
    _land(raw, "fred_graph_latest", FRED)
    _land(raw, "bls_api_v1_latest", json.dumps(BLS), "application/json")
    U.fetch("alfred_clock_downloadhelp_r1", "https://alfred.stlouisfed.org/help/downloaddata", raw=raw,
            transport=FakeTransport(body=b"A vintage date is a date in history used to download data as it actually existed on that past date."))


def test_build_writes_year_parts_under_a_c5_manifest(raw, root):
    _raw(raw)
    res = U.build(raw=raw, obs_start=D(2023, 12, 1))
    stage = root / U.STAGE
    m = C.read_json(stage / "manifest.json")
    assert sorted(m["files"]) == ["parts/year=2024/cpi_u_nsa.parquet"] and m["counts"]["rows"] == 4
    assert m["rule"] == U.RULE and m["clock_rule"] == U.CLOCK_RULE and type(m["staleness_days"]) is int
    assert m["coverage"]["latest_ref_month"] == "2024-02-01" and m["coverage"]["latest_vintage"] == "2024-03-12"
    assert m["coverage"]["missing_ref_months"] == []
    # 2023-12-31 -> 2024-02-13 (January's first release) is the longest wait for the next month
    assert m["coverage"]["observed_max_days_month_end_to_next_vintage_date"] == [44, "2023-12-01"]
    assert m["crosscheck"]["fred_latest"]["mismatches"] == [] and m["crosscheck"]["fred_latest"]["compared"] == 3
    assert m["crosscheck"]["bls_api_v1"]["compared"] == 3
    assert m["crosscheck"]["bls_api_v1"]["mismatches"] == [
        {"ref_month": "2023-12-01", "alfred_latest": 306.75, "bls": 306.746}]
    assert len(m["inputs"]) == 6 and all(v["sha256"] for v in m["inputs"].values())
    chk = P.check_stage(stage, U.CONTRACT)
    assert chk["ok"], chk
    assert res["manifest_sha256"] == C.sha256_file(stage / "manifest.json")


def test_tampered_cpi_part_fails_the_self_check(raw, root):
    _raw(raw)
    U.build(raw=raw, obs_start=D(2023, 12, 1))
    part = root / U.STAGE / "parts" / "year=2024" / "cpi_u_nsa.parquet"
    part.write_bytes(part.read_bytes()[:-8] + b"\0" * 8)
    assert not P.check_stage(root / U.STAGE, U.CONTRACT)["ok"]


def test_build_refuses_an_existing_stage_and_a_receipt_mismatch(raw, root):
    _raw(raw)
    U.build(raw=raw, obs_start=D(2023, 12, 1))
    with pytest.raises(RuntimeError, match="already exists"):
        U.build(raw=raw, obs_start=D(2023, 12, 1))
    rec = U.ledger(raw).latest()["fred_graph_latest"]
    (raw / rec["file"]).write_text("tampered", encoding="utf-8")
    with pytest.raises(RuntimeError, match="sha256"):
        U.build(raw=raw, obs_start=D(2023, 12, 1), stage="cpi_u_nsa_other")


def test_civil_date_bound_is_after_any_local_day_end_and_handles_year_dst_boundaries():
    for date in [D(2023, 3, 12), D(2023, 11, 5), D(2023, 12, 31)]:
        bound = U.vintage_bound(date).replace(tzinfo=dt.UTC)
        for offset in [-12, 0, 14]:
            local_end = dt.datetime.combine(date + dt.timedelta(days=1), dt.time(),
                                            dt.timezone(dt.timedelta(hours=offset)))
            assert bound > local_end.astimezone(dt.UTC)
    assert U.vintage_bound(D(2023, 12, 31)) == T(2024, 1, 2)


def test_civil_date_bound_requires_authoritative_landed_semantics(raw):
    assert U.clock_evidence(raw)[0] is False
    _land(raw, "alfred_clock_downloadhelp_r1", "A vintage date is a date in history used to download data as it actually existed on that past date.")
    assert U.clock_evidence(raw)[0] is False  # wrong endpoint cannot prove semantics


def test_build_falls_back_to_actual_response_observation_without_civil_date_proof(raw, root):
    _raw(raw)
    rec = U.ledger(raw).latest()["alfred_clock_downloadhelp_r1"]
    (raw / rec["file"]).write_bytes(b"No date semantics here")
    # A new valid receipt with uninformative help proves neither date nor release time.
    U.fetch("alfred_clock_downloadhelp_r1", "https://alfred.stlouisfed.org/help/downloaddata", raw=raw,
            transport=FakeTransport(body=b"No date semantics here"))
    U.build(raw=raw, obs_start=D(2023, 12, 1))
    m = C.read_json(root / U.STAGE / "manifest.json")
    assert m["clock_evidence"]["historical_timing"] == "unavailable"
    assert m["exact_clock_requirement"] == "PARTIAL"
    import pyarrow.parquet as pq
    rows = [r for rel in m["files"] for r in pq.read_table(root / U.STAGE / rel).to_pylist()]
    assert all(r["clock_basis"] == U.OBSERVED_CLOCK_BASIS and r["release_date"] is None for r in rows)
    observed = [U.response_observed_at(rec) for rec in U._batch_recs(raw)]
    assert all(min(observed) <= r["available_at"] <= max(observed) for r in rows)


def test_accept_stops_on_rejected_paths_before_coverage_sql(raw, root, monkeypatch):
    _raw(raw)
    U.build(raw=raw, obs_start=D(2023, 12, 1))
    stage = root / U.STAGE
    m = C.read_json(stage / "manifest.json")
    m["files"] = {"../outside.parquet": {"bytes": 0, "sha256": "0" * 64}}
    C.write_json_atomic(stage / "manifest.json", m)
    original = C.connect
    calls = []
    def counted_connect(*args, **kwargs):
        calls.append(True)
        return original(*args, **kwargs)
    monkeypatch.setattr(C, "connect", counted_connect)
    ok, lines = U.accept()
    assert not ok and "FAIL" in "\n".join(lines)
    assert len(calls) == 1  # only self-check, no subsequent rejected-path query


def test_corrected_build_does_not_overwrite_legacy_and_prints_partial(raw, root):
    _raw(raw)
    old = root / U.LEGACY_STAGE
    old.mkdir(parents=True)
    (old / "manifest.json").write_text("legacy", encoding="utf-8")
    U.build(raw=raw, obs_start=D(2023, 12, 1))
    assert (old / "manifest.json").read_text(encoding="utf-8") == "legacy"
    ok, lines = U.accept()
    assert ok and "exact publication-clock requirement: PARTIAL" in "\n".join(lines)


def test_corrected_clock_partitions_by_bound_year_instead_of_vintage_year(raw, root, monkeypatch):
    _land(raw, "alfred_downloaddata_page", PAGE, "text/html")
    _land(raw, "alfred_vintages_2023-12-29_2023-12-31",
          "observation_date,CPIAUCNS_20231229,CPIAUCNS_20231231\n2023-11-01,,307.051\n")
    U.fetch("alfred_clock_downloadhelp_r1", "https://alfred.stlouisfed.org/help/downloaddata", raw=raw,
            transport=FakeTransport(body=b"A vintage date is a date in history used to download data as it actually existed on that past date."))
    monkeypatch.setattr(U, "alfred_page", lambda page: (D(2023, 11, 1), [D(2023, 12, 29), D(2023, 12, 31)]))
    U.build(raw=raw, obs_start=D(2023, 11, 1))
    m = C.read_json(root / U.STAGE / "manifest.json")
    assert list(m["files"]) == ["parts/year=2024/cpi_u_nsa.parquet"]
    import pyarrow.parquet as pq
    row = pq.read_table(root / U.STAGE / next(iter(m["files"]))).to_pylist()[0]
    assert row["vintage_date"] == D(2023, 12, 31) and row["available_at"] == T(2024, 1, 2)
    assert row["release_date"] is None


def test_legacy_acceptance_remains_readable_and_warns_its_clock_is_unproved(raw, root):
    _raw(raw)
    U.build(raw=raw, obs_start=D(2023, 12, 1), stage=U.LEGACY_STAGE)
    stage = root / U.LEGACY_STAGE
    m = C.read_json(stage / "manifest.json")
    m.update(U.LEGACY_CONTRACT)
    m["coverage"]["observed_max_days_month_end_to_next_release"] = m["coverage"].pop("observed_max_days_month_end_to_next_vintage_date")
    m.pop("clock_evidence")
    C.write_json_atomic(stage / "manifest.json", m)
    ok, lines = U.accept(stage=U.LEGACY_STAGE)
    assert ok and "legacy available_at uses unproved 08:30 stamps" in "\n".join(lines)


def test_accept_quotes_a_verified_stage_path_containing_an_apostrophe(raw, root):
    _raw(raw)
    U.build(raw=raw, obs_start=D(2023, 12, 1), stage="cpi'v2")
    ok, lines = U.accept(stage="cpi'v2")
    assert ok and "PARTIAL" in "\n".join(lines)
