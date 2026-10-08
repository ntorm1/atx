"""Opt-in real-market-data gate using pinned published TRAIN Parquet panels.

Starts its own authenticated native/gateway pair on alternate ports and uses a new
temporary SQLite workspace. Reads published market files without modifying them;
never opens the operator DuckDB catalog or stops an existing application.
"""
from __future__ import annotations

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import tempfile
import time


REPO = Path(__file__).resolve().parents[1]
BASE = runpy.run_path(str(REPO / "scripts/test-atx-workspace.py"))["Harness"]


class ResearchHarness(BASE):
    def __init__(self, *args, manifest: Path, pin: str, portfolio: bool = False, **kwargs):
        super().__init__(*args, **kwargs)
        self.manifest, self.pin = manifest, pin
        self.portfolio = portfolio

    def verify_portfolio(self, recipe: dict) -> dict:
        expressions = [recipe["strategy_dsl"], "-rank(delta(close,10)/delay(close,10))"]
        portfolio = {key: value for key, value in recipe.items() if key != "strategy_dsl"}
        portfolio.update({
            "mode": "real_portfolio", "strategy_id": "real-data-portfolio-v1",
            "components": [{"strategy_dsl": expression, "expression_sha256":
                            hashlib.sha256(expression.encode()).hexdigest(), "weight": 0.5}
                           for expression in expressions],
            "gross_limit": 1, "name_cap": 0.02, "max_turnover": 1,
            "signal_decay_sessions": 3,
        })
        job = self.finished(self.submit(portfolio, "actual-constrained-portfolio")["id"])
        assert job["status"].lower() == "succeeded", job
        result, provenance = job["result"], job["result"]["provenance"]
        assert result["mode"] == "real_portfolio" and provenance["synthetic"] is False
        assert provenance["dataset_manifest_sha256"] == self.pin, provenance
        assert provenance["portfolio_geometry"] == "identity_signal_projection_not_estimated_risk"
        assert provenance["blend_rule"] == "StandardizedFixedGrossV2", provenance
        assert provenance["signal_decay_sessions"] == 3 and provenance["warmup_sessions"] == 12
        assert len(provenance["components"]) == 2, provenance
        assert provenance["nav_return_reconciliation"]["status"] == "passed", provenance
        assert provenance["daily_returns_complete"] is True, provenance
        assert len(result["daily_returns"]) == result["metrics"]["observations"] > 20
        rows = result["portfolio_allocations"]
        assert len(rows) > 2 and not provenance["portfolio_allocations_truncated"], provenance
        drift_observed = False
        for index, row in enumerate(rows):
            target, held = row["target_weights"], row["pretrade_held_weights"]
            assert len(target) == len(held) == provenance["instruments"], row
            assert abs(sum(target)) <= 1e-6 and sum(map(abs, target)) <= 1 + 1e-6, row
            assert max(map(abs, target)) <= 0.02 + 1e-6, row
            assert sum(abs(a - b) for a, b in zip(target, held)) <= 1 + 1e-6, row
            assert row["decision_date"] < row["execution_date"], row
            assert row["solver_primal_infeasible"] is False, row
            assert row["trade_cost"] >= 0 and row["posttrade_nav"] <= row["pretrade_nav"], row
            if index:
                drift_observed |= any(abs(a - b) > 1e-8 for a, b in
                                      zip(rows[index - 1]["target_weights"], held))
        assert drift_observed and result["trades"], result
        rejected = {**portfolio, "components": [dict(item) for item in portfolio["components"]]}
        rejected["components"][0]["expression_sha256"] = "0" * 64
        failure = self.finished(self.submit(rejected, "portfolio-forged-expression")["id"])
        assert failure["status"].lower() == "failed" and "expression SHA256" in failure["error"]
        return {"job_id": job["id"], "result": result}

    def start_native(self) -> None:
        self.native = self.spawn([
            str(self.server), "--listen", f"127.0.0.1:{self.grpc_port}",
            "--db", str(self.temporary / "workspace.db"),
            "--repo-root", str(self.temporary), "--artifact-root", str(self.temporary),
            "--research-dataset", f"published-train={self.pin}={self.manifest}",
            "--workers", "2", "--capacity", "8",
        ] + self.tls_args, "native.log", self.repo)

    def finished(self, job_id: str) -> dict:
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            job = self.request(f"/api/jobs/{job_id}")
            if job["status"].lower() in {"succeeded", "failed", "cancelled"}:
                return job
            time.sleep(0.2)
        raise AssertionError(f"Published TRAIN job exceeded bounded gate timeout: {job_id}")

    def verify(self) -> dict:
        catalog = self.request("/api/strategies")
        dataset = next(item for item in catalog["datasets"] if item["id"] == "published-train")
        assert dataset["manifest_sha256"] == self.pin, dataset
        assert "close" in dataset["fields"] and "fund_clock" not in dataset["fields"], dataset
        assert catalog["operators"], catalog
        recipe = {
            "mode": "real_dsl", "strategy_id": "real-data-long-short-v1",
            "dataset_id": "published-train", "dataset_sha256": self.pin,
            "strategy_dsl": "-rank(delta(close,5)/delay(close,5))",
            "start_date": "2020-01-20", "end_date": "2020-03-31",
            "instrument_limit": 128, "initial_capital": 1_000_000,
            "trade_bps": 6, "annual_borrow_bps": 300,
            "cadence": 5, "execution_delay_sessions": 1, "neutralization": "dollar",
        }
        validation = self.request("/api/analysis/validate", {
            "strategy_dsl": recipe["strategy_dsl"], "dataset_id": "published-train",
            "dataset_sha256": self.pin,
        })
        assert validation["valid"] and validation["synthetic"] is False, validation
        assert validation["required_lookback"] == 5, validation
        self.request("/api/analysis/validate", {
            "strategy_dsl": "rank(gics)", "dataset_id": "published-train",
            "dataset_sha256": self.pin,
        }, expected=400)
        job = self.submit(recipe, "actual-published-train")
        complete = self.finished(job["id"])
        assert complete["status"].lower() == "succeeded", complete
        result = complete["result"]
        provenance = result["provenance"]
        assert provenance["synthetic"] is False and provenance["evaluation"] == "TRAIN", provenance
        assert provenance["dataset_manifest_sha256"] == self.pin, provenance
        assert provenance["historical_vintage_verified"] is False, provenance
        assert provenance["capacity_verified"] is False, provenance
        assert provenance["borrow_availability_verified"] is False, provenance
        assert provenance["instruments"] <= 128, provenance
        assert provenance["engine_git_sha"] and len(provenance["submitted_request_sha256"]) == 64
        assert provenance["nav_return_reconciliation"]["status"] == "passed", provenance
        assert provenance["daily_returns_complete"] is True, provenance
        assert result["trades"] and result["positions"], result
        periods = result["daily_returns"]
        assert len(periods) == result["metrics"]["observations"] > 20, result
        assert all("2018" <= row["period_start_date"][:4] <= "2022" and
                   "2018" <= row["date"][:4] <= "2022" for row in periods), periods
        assert all(row["period_start_date"] < row["date"] for row in periods), periods
        assert all(periods[i]["period_start_date"] == periods[i - 1]["date"]
                   for i in range(1, len(periods))), periods
        assert all("year=2023" not in item["path"] and "year=2024" not in item["path"]
                   and "year=2025" not in item["path"] for item in provenance["payloads"])
        alternative = self.submit({**recipe, "strategy_dsl": "rank(delta(close,5)/delay(close,5))"},
                                  "actual-published-train-alternative")
        other = self.finished(alternative["id"])
        assert other["status"].lower() == "succeeded", other
        comparison = self.request("/api/analysis/compare", {
            "job_ids": [job["id"], alternative["id"]], "max_chart_points": 8,
        })
        assert comparison["observations"] == len(periods), comparison
        assert comparison["alignment"] == "intersection_of_exact_return_periods", comparison
        assert comparison["mixed_evidence"] is False and comparison["chart_downsampled"] is True
        assert len(comparison["correlation"]) == 2, comparison
        assert all(run["dataset_sha256"] == self.pin for run in comparison["runs"]), comparison
        portfolio = self.verify_portfolio(recipe) if self.portfolio else None
        for key, altered, reason in [
            ("wrong-revision", {**recipe, "dataset_sha256": "0" * 64}, "revision SHA256"),
            ("locked-holdout", {**recipe, "end_date": "2023-01-01"}, "TRAIN"),
        ]:
            failed = self.finished(self.submit(altered, key)["id"])
            assert failed["status"].lower() == "failed", failed
            assert reason in failed["error"], failed
        self.stop(self.native)
        self.start_native()
        self.wait_ready()
        restored = self.request(f"/api/jobs/{job['id']}")
        assert restored["result"] == result, "Real research evidence changed after native restart"
        if portfolio:
            saved = self.request(f"/api/jobs/{portfolio['job_id']}")
            assert saved["result"] == portfolio["result"], "Portfolio evidence changed after restart"
        assert hashlib.sha256(self.manifest.read_bytes()).hexdigest() == self.pin
        return {
            "status": "passed", "job_id": job["id"],
            "dataset_manifest_sha256": self.pin,
            "metrics": result["metrics"], "provenance": provenance,
            "comparison": comparison,
            "portfolio": portfolio,
            "checks": ["pinned dataset and operator discovery", "actual TRAIN market data",
                       "causal DSL and native delayed portfolio replay", "finite reconciled NAV",
                       "complete exact return periods", "unverified execution qualifications retained",
                       "revision and holdout rejection", "SQLite evidence survives native restart"],
        }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path,
                        default=REPO / "build-server-rel/bin" /
                        ("atx-server.exe" if os.name == "nt" else "atx-server"))
    parser.add_argument("--manifest", type=Path,
                        default=REPO / "atx-db/data/alpha_panel/v1/panel/manifest.json")
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--tls", action="store_true")
    parser.add_argument("--portfolio", action="store_true",
                        help="Also qualify native fixed-expression constrained holdings replay")
    args = parser.parse_args()
    server, manifest = args.server.resolve(strict=True), args.manifest.resolve(strict=True)
    if manifest.stat().st_size > 1024 * 1024:
        parser.error("Published manifest exceeds native 1 MiB limit")
    raw = manifest.read_bytes()
    document = json.loads(raw)
    if document.get("schema") != "atx.alpha-panel.panel/v2" or document.get("status") != "complete":
        parser.error("A complete published alpha-panel/v2 manifest is required")
    pin = hashlib.sha256(raw).hexdigest()
    cache = REPO / ".cache"
    cache.mkdir(exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix="research-integration-", dir=cache))
    success = False
    try:
        with ExitStack() as stack:
            harness = ResearchHarness(REPO, server, temporary, stack, args.tls,
                                      manifest=manifest, pin=pin, portfolio=args.portfolio)
            harness.start()
            evidence = harness.verify()
            (temporary / "evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
            print(json.dumps(evidence, indent=2))
            success = True
    finally:
        (temporary / "tls-key.pem").unlink(missing_ok=True)
        target, cache_root = temporary.resolve(strict=True), cache.resolve(strict=True)
        if target.parent != cache_root or not target.name.startswith("research-integration-"):
            raise RuntimeError(f"Refusing cleanup outside owned integration directory: {target}")
        if success and not args.keep:
            shutil.rmtree(target)
        else:
            print(f"Integration evidence: {temporary}")


if __name__ == "__main__":
    main()
