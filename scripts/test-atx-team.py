"""Isolated HTTP/gRPC qualification for shared-team roles and research governance.

Uses random ephemeral credentials, a new SQLite workspace, and synthetic engine
evidence. Never accesses the operator workspace, credentials or market datasets.
"""
from __future__ import annotations

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import runpy
import secrets
import shutil
import subprocess
import tempfile
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen


REPO = Path(__file__).resolve().parents[1]
BASE = runpy.run_path(str(REPO / "scripts/test-atx-workspace.py"))["Harness"]


class TeamHarness(BASE):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.credentials = {actor: secrets.token_hex(24) for actor in
                            ("alice", "bob", "viewer", "admin", "expired", "expiring")}
        self.environment.pop("ATX_WEB_TOKEN", None)
        self.environment["ATX_WEB_TEAM_AUTH"] = "1"
        self.token = self.credentials["alice"]
        self.responses: list[dict] = []
        now = int(time.time() * 1000)
        self.expiry = now + 12_000
        roles = {"alice": "researcher", "bob": "reviewer", "admin": "admin"}
        document = {
            "schema_version": 1, "team_name": "Disposable integration team",
            "principals": [{"id": actor, "name": actor.title(),
                            "role": roles.get(actor, "viewer"),
                            "token_sha256": hashlib.sha256(token.encode()).hexdigest(),
                            "expires_at_ms": now - 1000 if actor == "expired" else
                            self.expiry if actor == "expiring" else 0}
                           for actor, token in self.credentials.items()],
        }
        self.configuration = self.temporary / "team.json"
        self.configuration.write_text(json.dumps(document), encoding="utf-8")
        if os.name != "nt":
            self.configuration.chmod(0o600)

    def start_native(self) -> None:
        self.native = self.spawn([
            str(self.server), "--listen", f"127.0.0.1:{self.grpc_port}",
            "--db", str(self.temporary / "workspace.db"),
            "--repo-root", str(self.temporary), "--artifact-root", str(self.temporary),
            "--team-config", str(self.configuration), "--workers", "2", "--capacity", "32",
        ] + self.tls_args, "native.log", self.repo)

    def reject_expired_gateway_collision(self) -> None:
        target = self.temporary / "rejected-credential.db"
        environment = dict(self.environment)
        environment["ATX_SERVER_TOKEN"] = self.credentials["expired"]
        with (self.temporary / "rejected-startup.log").open("wb") as output:
            child = subprocess.run([
                str(self.server), "--listen", f"127.0.0.1:{self.grpc_port}",
                "--db", str(target), "--repo-root", str(self.temporary),
                "--team-config", str(self.configuration),
            ] + self.tls_args, cwd=self.repo, env=environment, stdin=subprocess.DEVNULL,
                stdout=output, stderr=subprocess.STDOUT, timeout=15,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        assert child.returncode != 0, "Expired principal credential admitted as gateway credential"
        log = (self.temporary / "rejected-startup.log").read_text(errors="replace")
        assert "gateway credential must be separate" in log, "Wrong startup rejection reason"
        assert not target.exists() and not Path(str(target) + ".lock").exists(), \
            "Rejected credentials reached workspace creation"

    def exchange(self, method: str, path: str, body: dict | None = None, *,
                 actor: str | None = "alice", expected: int = 200) -> dict:
        headers = {"Accept": "application/json", "Content-Type": "application/json"}
        if actor is not None:
            token = self.environment["ATX_SERVER_TOKEN"] if actor == "gateway" else \
                self.credentials.get(actor, "z" * 48)
            headers["Authorization"] = f"Bearer {token}"
        request = Request(self.base + path, method=method, headers=headers,
                          data=None if body is None else json.dumps(body).encode())
        try:
            response = urlopen(request, timeout=20)
        except HTTPError as error:
            response = error
        with response:
            raw = response.read()
            result = json.loads(raw) if raw else {}
            assert response.status == expected, (method, path, actor, expected,
                                                  response.status, result)
        self.responses.append(result)
        return result

    def alpha(self, actor: str, label: str) -> dict:
        return self.exchange("POST", "/api/alphas", {
            "name": label, "description": "Synthetic qualification only",
            "hypothesis": "Verify exact recipe evidence and independent governance",
            "tags": ["integration"],
            "request": {"mode": "synthetic_demo", "sessions": 32,
                        "strategy_dsl": "rank(delta(close,5))"},
        }, actor=actor, expected=201)

    def run(self, alpha: dict, actor: str, label: str) -> dict:
        job = self.exchange("POST", "/api/jobs", {
            "alpha_id": alpha["id"], "alpha_revision": alpha["revision"],
            "idempotency_key": label, "label": label,
        }, actor=actor, expected=201)
        result = self.finished(job["id"])
        assert result["status"].lower() == "succeeded", result
        assert result["actor_id"] == actor and result["request"] == alpha["request"], result
        return result

    def review(self, alpha: dict, job: dict, actor: str, key: str) -> dict:
        return self.exchange("POST", "/api/reviews", {
            "alpha_id": alpha["id"], "alpha_revision": alpha["revision"],
            "job_ids": [job["id"]], "note": "Acknowledge synthetic and cost assumptions",
            "idempotency_key": key,
        }, actor=actor, expected=201)

    def decision(self, review: dict, actor: str, state: str = "approved", *,
                 expected: int = 200) -> dict:
        return self.exchange("POST", f"/api/reviews/{review['id']}/decision", {
            "expected_revision": review["revision"], "state": state,
            "rationale": "Evidence and limitations independently acknowledged",
        }, actor=actor, expected=expected)

    def edited(self, alpha: dict, name: str, actor: str = "alice") -> dict:
        return self.exchange("PUT", f"/api/alphas/{alpha['id']}", {
            "name": name, "description": alpha["description"], "tags": alpha["tags"],
            "hypothesis": alpha["hypothesis"], "request": alpha["request"],
            "expected_revision": alpha["revision"],
        }, actor=actor)

    def verify(self) -> dict:
        self.reject_expired_gateway_collision()
        for actor in [None, "unknown", "expired", "gateway"]:
            self.exchange("GET", "/api/session", actor=actor, expected=401)
        for actor, role in [("alice", "researcher"), ("bob", "reviewer"),
                            ("viewer", "viewer"), ("admin", "admin")]:
            session = self.exchange("GET", "/api/session", actor=actor)
            assert session["team_mode"] is True and session["project_id"] == "local", session
            assert session["principal"]["id"] == actor and session["principal"]["role"] == role
        self.exchange("GET", "/api/session", actor="expiring")
        members = self.exchange("GET", "/api/team", actor="admin")
        assert {item["id"] for item in members["principals"]} == set(self.credentials), members
        # Service credentials admit readiness/event delivery, never a browser identity.
        self.exchange("GET", "/api/health", actor="gateway")
        denied_job = {"request": {"mode": "synthetic_demo"}, "idempotency_key": "denied"}
        self.exchange("POST", "/api/jobs", denied_job, actor="gateway", expected=401)
        before = self.exchange("GET", "/api/jobs")["total"]
        writes = [("POST", "/api/jobs", denied_job),
                  ("POST", "/api/alphas", {"name": "Denied", "request": {}}),
                  ("POST", "/api/workspaces", {"name": "Denied", "document":
                   {"schema_version": 1, "layout": {}, "panel_state": {}}}),
                  ("POST", "/api/experiments", {"name": "Denied", "idempotency_key": "denied-batch", "simulations":
                   [{"label": "Denied", "request": {"mode": "synthetic_demo"}}]})]
        for method, path, body in writes:
            self.exchange(method, path, body, actor="viewer", expected=403)
        assert self.exchange("GET", "/api/jobs")["total"] == before
        assert self.exchange("GET", "/api/alphas")["total"] == "0"

        alpha = self.alpha("alice", "Alice exact recipe")
        assert alpha["actor_id"] == "alice", alpha
        saved = self.run(alpha, "alice", "alice-saved-evidence")
        submitted = self.review(alpha, saved, "alice", "alice-review")
        frozen = submitted["evidence"]
        assert submitted["author_actor_id"] == submitted["requester_actor_id"] == "alice"
        assert submitted["current"] and submitted["state"] == "submitted", submitted
        evidence_job = frozen["jobs"][0]
        assert evidence_job["job_id"] == saved["id"] and evidence_job["actor_id"] == "alice"
        assert evidence_job["request_sha256"] == alpha["request_sha256"]
        assert evidence_job["synthetic"] is True and evidence_job["evaluation"] == "synthetic_test"
        assert len(evidence_job["result_sha256"]) == 64 and evidence_job["warnings"]
        # Read the raw persisted canonical result only from this gate's disposable DB.
        import sqlite3
        with sqlite3.connect(f"file:{self.temporary / 'workspace.db'}?mode=ro", uri=True) as db:
            raw = db.execute("SELECT result_json FROM jobs WHERE id=?", (saved["id"],)).fetchone()[0]
        assert hashlib.sha256(raw.encode()).hexdigest() == evidence_job["result_sha256"]
        repeated = self.review(alpha, saved, "alice", "alice-review")
        assert repeated == submitted
        self.decision(submitted, "alice", expected=403)
        self.decision(submitted, "viewer", expected=403)
        approved = self.decision(submitted, "bob")
        assert approved["state"] == "approved" and approved["revision"] == "2", approved
        assert approved["evidence"] == frozen
        self.decision(submitted, "bob", "rejected", expected=409)
        decisions = self.exchange("GET", f"/api/reviews/{submitted['id']}/decisions")
        assert len(decisions["decisions"]) == 1 and decisions["decisions"][0]["actor_id"] == "bob"

        # Bob has review permission, but cannot approve his own alpha or request.
        bob_alpha = self.alpha("bob", "Bob independently prohibited")
        bob_job = self.run(bob_alpha, "bob", "bob-saved-evidence")
        own_author = self.review(bob_alpha, bob_job, "alice", "bob-author-review")
        failure = self.decision(own_author, "bob", expected=409)
        assert "independent" in failure["error"], failure
        requester = self.review(alpha, saved, "bob", "bob-requester-review")
        failure = self.decision(requester, "bob", expected=409)
        assert "independent" in failure["error"], failure
        withdrawn = self.decision(requester, "admin", "withdrawn")
        assert withdrawn["state"] == "withdrawn"
        reproduced_job = self.run(alpha, "bob", "independent-reproduction")
        reproduced = self.review(alpha, reproduced_job, "alice", "independent-reproduction-review")
        assert self.decision(reproduced, "bob")["state"] == "approved"

        second = self.edited(alpha, "Alice metadata revision two")
        stale_approval = self.exchange("GET", f"/api/reviews/{approved['id']}")
        assert stale_approval["current"] is False and stale_approval["state"] == "approved"
        assert stale_approval["evidence"] == frozen
        self.exchange("POST", "/api/reviews", {
            "alpha_id": alpha["id"], "alpha_revision": "1", "job_ids": [saved["id"]],
        }, expected=409)
        second_job = self.run(second, "alice", "alice-second-evidence")
        pending = self.review(second, second_job, "alice", "alice-second-review")
        current = self.edited(second, "Alice metadata revision three")
        failure = self.decision(pending, "bob", expected=409)
        assert "edited or archived" in failure["error"], failure
        rejected = self.decision(pending, "bob", "rejected")
        assert rejected["state"] == "rejected" and rejected["current"] is False
        assert rejected["evidence"] == pending["evidence"]
        current = self.edited(current, "Bob frozen revision contribution", "bob")
        contributed_job = self.run(current, "alice", "contributed-revision-evidence")
        contributed = self.review(current, contributed_job, "alice", "contributed-review")
        failure = self.decision(contributed, "bob", expected=409)
        assert "independent" in failure["error"], failure
        self.decision(contributed, "admin")
        current = self.edited(current, "Alice metadata does not launder Bob authorship")
        laundered_job = self.run(current, "alice", "same-recipe-history-evidence")
        laundered = self.review(current, laundered_job, "alice", "same-recipe-history-review")
        failure = self.decision(laundered, "bob", expected=409)
        assert "independent" in failure["error"], failure
        self.decision(laundered, "admin")
        summaries = self.exchange("GET", "/api/reviews", actor="viewer")["reviews"]
        assert all("evidence" not in item and "evidence_json" not in item for item in summaries)
        audit = self.exchange("GET", "/api/audit?limit=100", actor="admin")
        assert int(audit["total"]) > 10 and all(row["actor_id"] in {"alice", "bob", "admin"}
                                               for row in audit["records"]), audit
        submitted_audit = self.exchange("GET", f"/api/audit?entity_type=review&entity_id={submitted['id']}")
        assert [row["actor_id"] for row in submitted_audit["records"]] == ["bob", "alice"]
        assert all(isinstance(row["sequence"], str) and isinstance(row["revision"], str)
                   for row in audit["records"]), audit
        while int(time.time() * 1000) <= self.expiry:
            time.sleep(0.05)
        self.exchange("GET", "/api/session", actor="expiring", expected=401)
        self.exchange("POST", "/api/jobs", denied_job, actor="expiring", expected=401)
        self.stop(self.native)
        self.start_native()
        self.wait_ready()
        assert self.exchange("GET", f"/api/alphas/{current['id']}") == current
        assert self.exchange("GET", f"/api/reviews/{approved['id']}") == stale_approval
        assert self.exchange("GET", f"/api/reviews/{rejected['id']}") == rejected
        assert self.exchange("GET", f"/api/reviews/{submitted['id']}/decisions") == decisions
        assert self.exchange("GET", "/api/audit?limit=100", actor="admin") == audit
        assert self.exchange("GET", f"/api/jobs/{saved['id']}") == saved
        encoded = json.dumps(self.responses)
        assert all(token not in encoded and hashlib.sha256(token.encode()).hexdigest() not in encoded
                   for token in [*self.credentials.values(), self.environment["ATX_SERVER_TOKEN"]])
        logs = "".join((self.temporary / name).read_text(errors="replace")
                       for name in ["native.log", "gateway.log"])
        assert all(token not in logs for token in
                   [*self.credentials.values(), self.environment["ATX_SERVER_TOKEN"]])
        return {
            "status": "passed", "grpc_transport": "verified TLS" if self.tls else "loopback",
            "checks": ["native identities with separate gateway credentials", "401/403 role enforcement",
                       "authoritative mutation and audit actors", "exact persisted evidence SHA256",
                       "independent creator, requester and frozen revision author review",
                       "same-recipe authorship survives metadata edits; independent reproduction allowed",
                       "revision CAS and immutable decisions",
                       "alpha currentness and stale approval refusal", "no credentials in responses",
                       "runtime credential expiry", "expired gateway-token collision rejected before DB creation",
                       "audit and governance survive native restart"],
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
    temporary = Path(tempfile.mkdtemp(prefix="team-integration-", dir=cache))
    success = False
    try:
        with ExitStack() as stack:
            harness = TeamHarness(REPO, args.server.resolve(strict=True), temporary, stack, args.tls)
            harness.start()
            evidence = harness.verify()
            (temporary / "evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
            print(json.dumps(evidence, indent=2))
            success = True
    finally:
        # Keep evidence and operational logs if requested, never ephemeral authentication material.
        (temporary / "tls-key.pem").unlink(missing_ok=True)
        (temporary / "team.json").unlink(missing_ok=True)
        target, cache_root = temporary.resolve(strict=True), cache.resolve(strict=True)
        if target.parent != cache_root or not target.name.startswith("team-integration-"):
            raise RuntimeError(f"Refusing cleanup outside owned integration directory: {target}")
        if success and not args.keep:
            shutil.rmtree(target)
        else:
            print(f"Integration evidence: {temporary}")


if __name__ == "__main__":
    main()
