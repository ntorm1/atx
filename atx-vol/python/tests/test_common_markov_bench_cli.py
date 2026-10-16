"""Postimplementation native common-process checks against synthetic raw quotes."""
from __future__ import annotations

import csv
import json
import math
import os

import pytest

from test_quote_bench_cli import binary as binary, invoke, make_quotes


def write_rows(path, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def read_rows(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def policy_file(tmp_path, **updates):
    policy = {
        "schema_version": 1, "model": "common-markov",
        "profiles": [{"name": "control", "base": "sparse",
                      "grid": {"grid_nodes": 129, "concentration_scale": .2},
                      "fit": {"initial_sigma": .3, "max_evaluations": 1,
                              "prior_weight": 0, "strike_roughness_weight": 0,
                              "time_roughness_weight": 0}}],
        "regimes": {key + "_profile": "control" for key in ("dense", "ordinary", "sparse")},
    }
    policy.update(updates)
    path = tmp_path / "common-policy.json"
    path.write_text(json.dumps(policy))
    return path


def common_run(binary, source, output, policy=None, *extra):
    selection = ("--common-policy", str(policy)) if policy else ()
    return invoke(binary, source, output, "--model", "common-markov", *selection, *extra)


def test_common_flat_control_has_independent_price_and_equivalent_iv(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    expected = make_quotes(source)
    result = common_run(binary, source, output, policy_file(tmp_path))
    assert result.returncode == 0, result.stderr
    rows, report = read_rows(output), json.loads(result.stdout)
    assert len(rows) == len(expected)
    assert all(row["status"] == "ok" and row["model"] == "common-markov" for row in rows)
    assert max(abs(float(row["predicted_price"]) - price)
               for row, price in zip(rows, expected)) < .04
    assert max(abs(float(row["predicted_vol"]) - .3) for row in rows) < .003
    assert report["fitted_snapshots"] == 1
    assert report["served_rows"] == report["served_vol_rows"] == len(expected)
    decision, = report["policy_decisions"]
    assert decision["fit"]["termination"] == "evaluation_limit"
    assert decision["fit"]["evaluations"] == 1
    assert decision["fit"]["sigmas"] == [.3]
    assert "not fitted local sigma" in report["predicted_vol_convention"]
    assert "no continuous-American" in report["accuracy_scope"]
    definition = report["policy_definition"]
    assert definition["market"]["max_spot_relative_deviation"] == .005
    assert definition["profiles"][0]["model"]["max_stored_rows"] == 1000000
    first = output.read_bytes()
    assert common_run(binary, source, output, tmp_path / "common-policy.json").returncode == 0
    assert output.read_bytes() == first


def test_common_expiries_share_one_process_and_cumulative_market(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    other = []
    for row in rows:
        strike = float(row["strike"])
        sd = .3
        d1 = math.log(100 / strike) / sd + sd / 2
        d2 = d1 - sd
        price = 50 * math.erfc(-d1 / math.sqrt(2)) - strike / 2 * math.erfc(-d2 / math.sqrt(2))
        other.append({**row, "row_id": int(row["row_id"]) + 1000, "chain_id": 18,
                      "expiry_days": 21200, "years": 1., "years_calendar": 1.,
                      "bid": price - .001, "ask": price + .001})
    write_rows(source, rows + other)
    result = common_run(binary, source, output, policy_file(tmp_path))
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    assert report["fitted_chains"] == 2
    assert report["fitted_snapshots"] == 1
    assert report["served_rows"] == 52
    decision, = report["policy_decisions"]
    assert decision["fit"]["time_groups"] == 1
    assert decision["fit"]["interval_groups"] == [0, 0]
    assert [expiry["calendar_years"]["median"] for expiry in decision["market"]["expiries"]] == [.5, 1.]
    assert {row["chain_id"] for row in read_rows(output)} == {"17", "18"}


def test_common_price_survives_unidentifiable_equivalent_iv(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = policy_file(tmp_path, equivalent_iv={"min_time_value": 1000})
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    rows, report = read_rows(output), json.loads(result.stdout)
    assert all(row["status"] == "ok" and row["predicted_price"] for row in rows)
    assert all(row["vol_status"] == "unidentifiable" and not row["predicted_vol"] for row in rows)
    assert report["served_rows"] == 26
    assert report["served_vol_rows"] == 0


def test_common_equivalent_iv_preserves_funding_pv_and_distinct_clocks(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    prepaid, discount, sd = 100 * math.exp(.05) - 2, math.exp(-.01), .3 * math.sqrt(.25)
    expected = []
    for row in rows:
        strike = float(row["strike"])
        d1 = math.log(prepaid / (strike * discount)) / sd + sd / 2
        d2 = d1 - sd
        price = prepaid / 2 * math.erfc(-d1 / math.sqrt(2)) - (
            strike * discount / 2 * math.erfc(-d2 / math.sqrt(2)))
        expected.append(price)
        row.update(years=.25, rate=.02, sdiv=-.1, ddiv_pv=2,
                   bid=price - .001, ask=price + .001)
    write_rows(source, rows)
    path = policy_file(tmp_path)
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    predictions, report = read_rows(output), json.loads(result.stdout)
    assert all(row["status"] == "ok" for row in predictions)
    assert max(abs(float(row["predicted_price"]) - price)
               for row, price in zip(predictions, expected)) < .04
    assert max(abs(float(row["predicted_vol"]) - .3) for row in predictions) < .004
    expiry, = report["policy_decisions"][0]["market"]["expiries"]
    assert expiry["integrated_rate"] == pytest.approx(.01)
    assert expiry["integrated_effective_yield"] == pytest.approx(-math.log(prepaid / 100))
    refused = common_run(binary, source, output, path, "--dividends", "none")
    assert refused.returncode == 0, refused.stderr
    assert all(row["status"] == "market_dividend_convention" for row in read_rows(output))


@pytest.mark.parametrize("change,status", [
    ({"spot": "102"}, "market_spot_dispersion"),
    ({"sdiv": "1"}, "market_carry_dispersion"),
    ({"ask": "-1"}, "market_invalid_row"),
])
def test_common_market_refuses_whole_snapshot_and_keeps_every_row(binary, tmp_path, change, status):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    rows[-1].update(change)
    write_rows(source, rows)
    result = common_run(binary, source, output, policy_file(tmp_path))
    assert result.returncode == 0, result.stderr
    predictions, report = read_rows(output), json.loads(result.stdout)
    assert len(predictions) == len(rows)
    assert all(row["status"] == status and not row["predicted_price"] for row in predictions)
    assert report["served_rows"] == 0
    assert report["policy_decisions"][0]["market"]["status"] == status.removeprefix("market_")
    assert "features" not in report["policy_decisions"][0]


def test_common_duplicate_expiries_are_refused_before_liquidity_routing(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    other = [{**row, "row_id": int(row["row_id"]) + 1000, "chain_id": 18} for row in rows]
    write_rows(source, rows + other)
    result = common_run(binary, source, output)
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    assert report["ambiguous_expiry_chains"] == 2
    assert all(row["status"] == "ambiguous_expiry" for row in read_rows(output))
    assert "features" not in report["policy_decisions"][0]


def test_common_bidless_contract_is_an_upper_bound_and_override_is_exact(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    rows = read_rows(source)
    rows[0]["bid"] = 0
    write_rows(source, rows)
    path = policy_file(tmp_path, symbol_overrides=[{"symbol": "AAA", "profile": "control"}])
    result = common_run(binary, source, output, path)
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    assert report["policy_decisions"][0]["fit"]["bidless_quotes"] == 1
    assert report["policy_decisions"][0]["policy_source"] == "exact-symbol-override"
    assert read_rows(output)[0]["quote_status"] == "bidless_upper_bound"


@pytest.mark.parametrize("extra", [("--policy", "auto"), ("--curve", "essvi"),
                                   ("--max-weight", "100"), ("--min-quotes", "4")])
def test_common_refuses_legacy_policy_flags(binary, tmp_path, extra):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    result = common_run(binary, source, output, None, *extra)
    assert result.returncode != 0
    assert "incompatible" in result.stderr
    assert not output.exists()


@pytest.mark.parametrize("document", [
    {"schema_version": 1},
    {"schema_version": 1, "model": "common-markov", "relax_arbitrage": True},
    {"schema_version": 1, "model": "common-markov", "market": {"max_quotes": 999999}},
    {"schema_version": 1, "model": "common-markov", "profiles": [
        {"name": "sparse", "base": "sparse", "fit": {"max_evaluations": -1}}]},
])
def test_common_policy_is_closed_and_bounded(binary, tmp_path, document):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    policy = tmp_path / "invalid.json"
    policy.write_text(json.dumps(document))
    result = common_run(binary, source, output, policy)
    assert result.returncode != 0
    assert not output.exists()


def test_common_policy_duplicate_keys_hardlink_alias_and_resource_refusal(binary, tmp_path):
    source, output = tmp_path / "quotes.csv", tmp_path / "prices.csv"
    make_quotes(source)
    path = tmp_path / "policy.json"
    path.write_text('{"schema_version":1,"model":"common-markov","model":"common-markov"}')
    assert common_run(binary, source, output, path).returncode != 0
    alias = tmp_path / "alias.json"
    os.link(source, alias)
    original = source.read_bytes()
    assert common_run(binary, source, output, alias).returncode != 0
    assert source.read_bytes() == original
    result = common_run(binary, source, output, policy_file(tmp_path, market={"max_quotes": 3}))
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout)
    assert report["resource_refusal_counts"] == {"market_resource_limit": 1}
    assert len(read_rows(output)) == 26
