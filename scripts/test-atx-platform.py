"""Isolated HTTP/gRPC gate for durable layouts, alpha revisions and experiments.

Uses its own SQLite workspace, synthetic engine inputs and ephemeral listeners.
Never opens the operator workspace database or any market dataset.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
import json
import os
from pathlib import Path
import runpy
import shutil
import tempfile
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen


REPO = Path(__file__).resolve().parents[1]
BASE = runpy.run_path(str(REPO / "scripts/test-atx-workspace.py"))["Harness"]


class PlatformHarness(BASE):
    def exchange(self, method: str, path: str, body: dict | None = None) -> tuple[int, dict]:
        headers = {"Authorization": f"Bearer {self.token}", "Content-Type": "application/json"}
        request = Request(self.base + path, method=method, headers=headers,
                          data=None if body is None else json.dumps(body).encode())
        try:
            response = urlopen(request, timeout=20)
        except HTTPError as error:
            response = error
        with response:
            raw = response.read()
            return response.status, json.loads(raw) if raw else {}

    def mutate(self, method: str, path: str, body: dict, expected: int = 200) -> dict:
        status, result = self.exchange(method, path, body)
        assert status == expected, (method, path, expected, status, result)
        return result

    def wait_job(self, job_id: str) -> dict:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            job = self.request(f"/api/jobs/{job_id}")
            if job["status"] in {"succeeded", "failed", "cancelled"}:
                return job
            time.sleep(0.05)
        raise AssertionError(f"Synthetic engine did not finish bounded job {job_id}")

    def verify(self) -> dict:
        for path in ["/api/workspaces", "/api/alphas", "/api/experiments"]:
            self.request(path, auth=False, expected=401)
        self.request("/api/experiments")
        recipe = {"mode": "synthetic_demo", "sessions": 64,
                  "strategy_dsl": "-rank(delta(close,5)/delay(close,5))"}
        layout = {"schema_version": 1, "layout": {"panels": {}, "grid": {}},
                  "panel_state": {"draft_request": recipe, "selected_job_id": ""}}
        workspace = self.request("/api/workspaces", {
            "name": "Database desk", "description": "Integration-owned layout", "document": layout,
        }, expected=201)
        path = f"/api/workspaces/{workspace['id']}"
        with ThreadPoolExecutor(max_workers=2) as pool:
            updates = list(pool.map(lambda name: self.exchange("PUT", path, {
                "name": name, "description": "Concurrent save", "document": layout,
                "expected_revision": workspace["revision"],
            }), ["Research desk A", "Research desk B"]))
        assert sorted(code for code, _ in updates) == [200, 409], updates
        workspace = self.request(path)
        assert workspace["revision"] == "2" and workspace["document"] == layout
        summary = self.request("/api/workspaces")["workspaces"][0]
        assert "document" not in summary and "document_json" not in summary, summary

        alpha_input = {"name": "Reversal hypothesis", "description": "Synthetic qualification",
                       "hypothesis": "Check exact recipe lineage", "tags": ["reversal"],
                       "request": recipe}
        first = self.request("/api/alphas", alpha_input, expected=201)
        alpha_path = f"/api/alphas/{first['id']}"
        second = self.mutate("PUT", alpha_path, {
            **alpha_input, "name": "Reversal version two", "expected_revision": first["revision"],
            "request": {**recipe, "trade_bps": 8},
        })
        self.mutate("PUT", alpha_path, {
            **alpha_input, "expected_revision": first["revision"],
        }, expected=409)
        assert self.request(alpha_path + "?revision=1") == first
        assert second["request_sha256"] != first["request_sha256"]
        revisions = self.request(alpha_path + "/revisions")["revisions"]
        assert [item["revision"] for item in revisions] == ["2", "1"], revisions
        assert all("request_json" not in item and "request" not in item for item in revisions)
        saved = self.request("/api/jobs", {
            "alpha_id": second["id"], "alpha_revision": second["revision"],
            "label": "Pinned revision", "idempotency_key": "saved-alpha-run",
        }, expected=201)
        saved = self.wait_job(saved["id"])
        assert saved["status"] == "succeeded", saved
        assert saved["alpha_id"] == second["id"] and saved["alpha_revision"] == "2"
        assert saved["request"] == second["request"]

        experiment_input = {
            "name": "Reversal sensitivity", "description": "Two native executions",
            "idempotency_key": "batch-once",
            "simulations": [
                {"label": "Stored revision", "alpha_id": second["id"], "alpha_revision": "2"},
                {"label": "Opposite signal", "request": {
                    **recipe, "strategy_dsl": "rank(delta(close,5)/delay(close,5))"}},
            ],
        }
        batch = self.request("/api/experiments", experiment_input, expected=201)
        assert [job["label"] for job in batch["jobs"]] == ["Stored revision", "Opposite signal"]
        assert all(job["experiment_id"] == batch["id"] for job in batch["jobs"])
        members = [self.wait_job(job["id"]) for job in batch["jobs"]]
        assert all(job["status"] == "succeeded" for job in members), members
        completed = self.request(f"/api/experiments/{batch['id']}")
        assert completed["status"] == "succeeded" and completed["succeeded"] == 2
        assert completed["progress"] == 1
        repeated = self.request("/api/experiments", experiment_input, expected=201)
        assert [item["id"] for item in repeated["jobs"]] == [item["id"] for item in batch["jobs"]]
        self.request("/api/experiments", {**experiment_input, "name": "Changed retry"}, expected=409)
        before_jobs = self.request("/api/jobs")["total"]
        before_parents = self.request("/api/experiments")["total"]
        self.request("/api/experiments", {
            "name": "Refuse partial batch", "idempotency_key": "bad-member",
            "simulations": [{"label": "Valid", "request": recipe},
                            {"label": "Missing alpha", "alpha_id": "f" * 32,
                             "alpha_revision": "1"}],
        }, expected=404)
        assert self.request("/api/jobs")["total"] == before_jobs
        assert self.request("/api/experiments")["total"] == before_parents

        archived = self.request(alpha_path + "/archive", {
            "expected_revision": "2", "archived": True,
        })
        assert archived["stage"] == "archived" and archived["revision"] == "3"
        repeated = self.request("/api/experiments", experiment_input, expected=201)
        assert repeated["id"] == batch["id"], "Committed idempotent retry depends on mutable head"
        self.request("/api/jobs", {
            "alpha_id": second["id"], "alpha_revision": "2",
            "idempotency_key": "new-archived-alpha-run",
        }, expected=409)
        assert self.request(alpha_path + "?revision=2") == second
        comparison = self.request("/api/analysis/compare", {
            "job_ids": [job["id"] for job in members], "max_chart_points": 5,
        })
        assert comparison["alignment"] == "intersection_of_exact_return_periods"
        assert comparison["observations"] > 5 and comparison["chart_downsampled"]

        future = int(time.time() * 1000) + 86400000
        scheduled = self.request("/api/experiments", {
            "name": "Restart scheduled workload", "idempotency_key": "future-batch",
            "scheduled_at_ms": future,
            "simulations": [{"label": str(index), "request": recipe} for index in range(32)],
        }, expected=201)
        assert scheduled["queued"] == 32
        self.request("/api/experiments", {
            "name": "Over capacity", "idempotency_key": "over-capacity",
            "simulations": [{"label": "Extra", "request": recipe}],
        }, expected=429)
        assert int(self.request("/api/experiments")["total"]) == 2
        self.stop(self.native)
        self.start_native()
        self.wait_ready()
        assert self.request(path) == workspace
        assert self.request(alpha_path) == archived
        assert self.request(alpha_path + "?revision=1") == first
        assert self.request(f"/api/jobs/{saved['id']}") == saved
        assert self.request(f"/api/experiments/{batch['id']}") == completed
        restored = self.request(f"/api/experiments/{scheduled['id']}")
        assert restored["queued"] == 32 and restored["status"] == "queued"
        cancelled = self.request(f"/api/experiments/{scheduled['id']}/cancel", {})
        assert cancelled["cancelled"] == 32 and cancelled["status"] == "cancelled"
        assert self.request("/api/health")["queued_jobs"] == "0"
        self.mutate("DELETE", path, {"expected_revision": "1"}, expected=409)
        self.mutate("DELETE", path, {"expected_revision": "2"}, expected=204)
        self.request(path, expected=404)
        return {
            "status": "passed", "grpc_transport": "verified TLS" if self.tls else "loopback",
            "checks": ["browser authentication on all durable resources",
                       "concurrent layout compare-and-swap", "immutable alpha revision history",
                       "native saved-recipe resolution and exact run lineage",
                       "atomic experiment admission and capacity refusal",
                       "whole-batch idempotency after archival", "exact-period run comparison",
                       "scheduled batch survives restart", "durable completed evidence",
                       "batch cancellation releases all queue capacity",
                       "revision-checked workspace deletion"],
        }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=REPO / "build-server-rel/bin" /
                        ("atx-server.exe" if os.name == "nt" else "atx-server"))
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--tls", action="store_true")
    args = parser.parse_args()
    cache = REPO / ".cache"
    cache.mkdir(exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix="platform-integration-", dir=cache))
    success = False
    try:
        with ExitStack() as stack:
            harness = PlatformHarness(REPO, args.server.resolve(strict=True), temporary,
                                      stack, args.tls)
            harness.start()
            evidence = harness.verify()
            (temporary / "evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
            print(json.dumps(evidence, indent=2))
            success = True
    finally:
        (temporary / "tls-key.pem").unlink(missing_ok=True)
        target, cache_root = temporary.resolve(strict=True), cache.resolve(strict=True)
        if target.parent != cache_root or not target.name.startswith("platform-integration-"):
            raise RuntimeError(f"Refusing cleanup outside owned integration directory: {target}")
        if success and not args.keep:
            shutil.rmtree(target)
        else:
            print(f"Integration evidence: {temporary}")


if __name__ == "__main__":
    main()
