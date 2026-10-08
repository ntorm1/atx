"""Black-box integration gate: real native gRPC service, gateway and SQLite restart.

Uses only the Python standard library. Creates an isolated temporary database;
never opens the operator's workspace database or research data.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class Harness:
    def __init__(self, repo: Path, server: Path, temporary: Path, stack: ExitStack,
                 tls: bool = False):
        self.repo, self.server, self.temporary, self.stack = repo, server, temporary, stack
        self.grpc_port, self.http_port = free_port(), free_port()
        while self.http_port == self.grpc_port:
            self.http_port = free_port()
        self.environment = dict(os.environ)
        self.token = secrets.token_hex(24)
        self.environment.update({
            "ATX_SERVER_TOKEN": secrets.token_hex(24),
            "ATX_WEB_TOKEN": self.token,
            "ATX_GRPC_ADDRESS": f"127.0.0.1:{self.grpc_port}",
            "ATX_WEB_HOST": "127.0.0.1", "ATX_WEB_PORT": str(self.http_port),
            "ATX_GRPC_TLS": "0",
            "ATX_WEB_MAX_STREAMS": "128", "ATX_WEB_RATE_LIMIT": "10000",
        })
        self.base = f"http://127.0.0.1:{self.http_port}"
        self.tls = tls
        self.tls_args: list[str] = []
        if tls:
            self.configure_tls()
        self.native: subprocess.Popen | None = None
        self.gateway: subprocess.Popen | None = None

    def configure_tls(self) -> None:
        executable = "openssl.exe" if os.name == "nt" else "openssl"
        tool_root = self.repo / "deps/vcpkg-server/x64-windows/tools/openssl"
        candidates = (tool_root / executable, tool_root / "bin" / executable)
        openssl = next((str(path) for path in candidates if path.is_file()),
                       shutil.which("openssl"))
        if not openssl:
            raise RuntimeError("--tls requires an existing OpenSSL executable")
        certificate, private_key = self.temporary / "tls-cert.pem", self.temporary / "tls-key.pem"
        with (self.temporary / "tls-generation.log").open("wb") as output:
            subprocess.run([
                openssl, "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes",
                "-days", "1", "-subj", "/CN=localhost",
                "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost",
                "-addext", "extendedKeyUsage=serverAuth",
                "-keyout", str(private_key), "-out", str(certificate),
            ], check=True, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        if os.name != "nt":
            private_key.chmod(0o600)
        self.environment.update({"ATX_GRPC_TLS": "1", "ATX_GRPC_CA": str(certificate)})
        self.tls_args = ["--tls-cert", str(certificate), "--tls-key", str(private_key)]

    def spawn(self, args: list[str], log: str, cwd: Path) -> subprocess.Popen:
        output = self.stack.enter_context((self.temporary / log).open("ab"))
        child = subprocess.Popen(
            args, cwd=cwd, env=self.environment, stdin=subprocess.DEVNULL,
            stdout=output, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        self.stack.callback(self.stop, child)
        return child

    @staticmethod
    def stop(child: subprocess.Popen) -> None:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=10)

    def start_native(self) -> None:
        self.native = self.spawn([
            str(self.server), "--listen", f"127.0.0.1:{self.grpc_port}",
            "--db", str(self.temporary / "workspace.db"),
            "--repo-root", str(self.temporary), "--artifact-root", str(self.temporary),
            "--workers", "2", "--capacity", "32",
        ] + self.tls_args, "native.log", self.repo)

    def start(self) -> None:
        self.start_native()
        node = shutil.which("node")
        if not node:
            raise RuntimeError("Node.js is required")
        self.gateway = self.spawn([node, "gateway/main.mjs"], "gateway.log", self.repo / "atx-front")
        self.wait_ready()

    def request(self, path: str, body: dict | None = None, *, auth: bool = True,
                expected: int = 200) -> dict:
        headers = {"Accept": "application/json"}
        if auth:
            headers["Authorization"] = f"Bearer {self.token}"
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        req = Request(self.base + path, data=data, headers=headers)
        try:
            with urlopen(req, timeout=15) as response:
                status, raw = response.status, response.read()
        except HTTPError as error:
            status, raw = error.code, error.read()
        document = json.loads(raw)
        assert status == expected, (path, status, expected, document)
        return document

    def wait_ready(self) -> None:
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            if self.native is not None and self.native.poll() is not None:
                raise RuntimeError((self.temporary / "native.log").read_text(errors="replace"))
            if self.gateway is not None and self.gateway.poll() is not None:
                raise RuntimeError((self.temporary / "gateway.log").read_text(errors="replace"))
            try:
                health = self.request("/api/health")
                assert health["status"].lower() in {"serving", "healthy", "ok", "ready"}, health
                return
            except (URLError, AssertionError, TimeoutError):
                time.sleep(0.2)
        raise RuntimeError("Workspace failed readiness; inspect native.log and gateway.log")

    def submit(self, request: dict, key: str, scheduled: int = 0,
               expected: int = 201) -> dict:
        return self.request("/api/jobs", {
            "request": request, "idempotency_key": key, "scheduled_at_ms": scheduled,
        }, expected=expected)

    def finished(self, job_id: str) -> dict:
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            job = self.request(f"/api/jobs/{job_id}")
            if job["status"].lower() in {"succeeded", "failed", "cancelled"}:
                return job
            time.sleep(0.15)
        raise RuntimeError(f"Job did not finish: {job_id}")

    def check_event_gap(self) -> None:
        req = Request(self.base + "/api/events?channel=all&after_sequence=999999999", headers={
            "Authorization": f"Bearer {self.token}", "Accept": "text/event-stream",
        })
        with urlopen(req, timeout=10) as response:
            for _ in range(30):
                line = response.readline().decode()
                if line.startswith("data:"):
                    event = json.loads(line[5:])
                    assert event["gap"], event
                    return
        raise AssertionError("A cursor from a different server epoch must signal a gap")

    def check_event_fanout(self, request: dict, scheduled_at: int,
                           key: str = "event-fanout") -> None:
        cursor = self.request("/api/health")["event_sequence"]
        endpoint = self.base + f"/api/events?channel=jobs&after_sequence={cursor}"

        def subscribe(_: int):
            return urlopen(Request(endpoint, headers={
                "Authorization": f"Bearer {self.token}", "Accept": "text/event-stream",
            }), timeout=15)

        # Exceeds the native direct-subscriber cap: all browsers must share the hub.
        with ExitStack() as streams, ThreadPoolExecutor(max_workers=48) as workers:
            responses = [streams.enter_context(response)
                         for response in workers.map(subscribe, range(48))]
            job = self.submit(request, key, scheduled_at)

            def receives_submission(response) -> bool:
                for _ in range(256):
                    line = response.readline().decode()
                    if not line:
                        return False
                    if line.startswith("data:"):
                        event = json.loads(line[5:])
                        if event.get("job_id") == job["id"]:
                            return True
                return False

            assert all(workers.map(receives_submission, responses)), \
                "Every browser must receive the submitted job event"
            self.request(f"/api/jobs/{job['id']}/cancel", {})

    def check_database_lease(self) -> None:
        contender = self.spawn([
            str(self.server), "--listen", f"127.0.0.1:{free_port()}",
            "--db", str(self.temporary / "workspace.db"),
            "--repo-root", str(self.temporary),
        ], "lease-contender.log", self.repo)
        assert contender.wait(timeout=10) != 0, "A second database owner must be refused"
        assert "already leased" in (self.temporary / "lease-contender.log").read_text()
        self.wait_ready()

    def check_insecure_external_bind(self) -> None:
        refused_database = self.temporary / "insecure-external" / "workspace.db"
        contender = self.spawn([
            str(self.server), "--listen", f"0.0.0.0:{free_port()}",
            "--db", str(refused_database), "--repo-root", str(self.temporary),
        ], "insecure-external.log", self.repo)
        assert contender.wait(timeout=10) != 0, "An insecure external listener must be refused"
        output = (self.temporary / "insecure-external.log").read_text()
        assert "external listen requires ATX_SERVER_TOKEN and TLS" in output, output
        assert not refused_database.parent.exists(), "Refusal must precede database creation"

    def verify(self) -> dict:
        self.request("/api/health", auth=False, expected=401)
        catalog = self.request("/api/strategies")
        assert catalog, "Strategy catalog must not be empty"
        request = {"mode": "synthetic_demo", "strategy_id": "synthetic-long-short-v1", "sessions": 100,
                   "strategy_dsl": "rank(delta(close,5))"}
        first = self.submit(request, "repeatable-demo")
        repeated = self.submit(request, "repeatable-demo")
        assert first["id"] == repeated["id"], "Idempotency created duplicate jobs"
        self.submit({**request, "sessions": 101}, "repeatable-demo", expected=409)
        complete = self.finished(first["id"])
        assert complete["status"].lower() == "succeeded", complete
        assert complete["result"]["provenance"]["synthetic"] is True
        assert len(complete["result"]["equity_curve"]) > 2
        assert complete["result"]["metrics"]["observations"] == 99
        assert "atx::engine" in complete["result"]["provenance"]["engine"]

        with ThreadPoolExecutor(max_workers=12) as workers:
            copies = list(workers.map(
                lambda _: self.request(f"/api/jobs/{first['id']}"), range(24)))
        assert all(item["result"] == complete["result"] for item in copies)
        health = self.request("/api/health")
        assert int(health["cache_hits"]) > 0, health

        scheduled_at = int(time.time() * 1000) + 120_000
        scheduled = self.submit(request, "future-demo", scheduled_at)
        assert scheduled["status"].lower() in {"queued", "scheduled"}
        cancelled = self.request(f"/api/jobs/{scheduled['id']}/cancel", {}, expected=200)
        assert cancelled["status"].lower() == "cancelled"
        pending = self.submit(request, "survives-restart", scheduled_at)
        self.check_database_lease()
        self.check_insecure_external_bind()
        self.check_event_fanout(request, scheduled_at)
        before_restart = self.request("/api/jobs?limit=10")
        assert all(not item.get("result") for item in before_restart["jobs"])
        self.request("/api/jobs?limit=1000000", expected=400)
        self.request("/api/jobs/00000000000000000000000000000000", expected=404)

        invalid = self.submit({"mode": "import_nav", "artifact_path": "../escape.csv"},
                              "traversal-refusal")
        refused = self.finished(invalid["id"])
        assert refused["status"].lower() == "failed", refused
        assert not refused.get("result")
        self.check_event_gap()

        assert self.native is not None
        self.native.kill()  # Own child only: deliberately exercise SQLite crash durability.
        self.native.wait(timeout=10)
        self.start_native()
        self.wait_ready()
        restored = self.request(f"/api/jobs/{first['id']}")
        assert restored["result"] == complete["result"], "Completed result lost on restart"
        still_queued = self.request(f"/api/jobs/{pending['id']}")
        assert still_queued["status"].lower() in {"queued", "scheduled"}
        self.request(f"/api/jobs/{pending['id']}/cancel", {})
        self.check_event_gap()
        self.check_event_fanout(request, int(time.time() * 1000) + 120_000,
                               "event-fanout-after-restart")
        return {"status": "passed", "checks": [
            "browser authentication", "real HTTP-to-gRPC research execution",
            "engine-generated synthetic evidence", "idempotency and conflict",
            "24 concurrent cached reads", "scheduled cancellation", "bounded pagination",
            "48 simultaneous SSE subscribers before/after restart", "exclusive database process lease",
            "artifact traversal refusal", "SSE epoch gap", "SQLite crash/restart durability",
            "insecure external bind refused before database creation",
        ], "grpc_transport": "verified TLS" if self.tls else "loopback plaintext",
            "job_id": first["id"], "cache_hits": health["cache_hits"]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path)
    parser.add_argument("--keep", action="store_true", help="Retain isolated test DB and logs")
    parser.add_argument("--tls", action="store_true",
                        help="Verify TLS using a temporary localhost/IP certificate")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    server = (args.server or repo / "build-server/bin" /
              ("atx-server.exe" if os.name == "nt" else "atx-server")).resolve()
    if not server.is_file():
        parser.error(f"Build atx-server first: {server}")
    (repo / ".cache").mkdir(exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix="workspace-integration-", dir=repo / ".cache"))
    success = False
    try:
        with ExitStack() as stack:
            harness = Harness(repo, server, temporary, stack, args.tls)
            harness.start()
            print(json.dumps(harness.verify(), indent=2))
            success = True
    finally:
        # Keep the public certificate and logs as evidence, never retain the private key.
        (temporary / "tls-key.pem").unlink(missing_ok=True)
        if success and not args.keep:
            cache_root = (repo / ".cache").resolve(strict=True)
            cleanup_target = temporary.resolve(strict=True)
            if (cleanup_target.parent != cache_root or
                    not cleanup_target.name.startswith("workspace-integration-")):
                raise RuntimeError(f"Refusing cleanup outside test cache: {cleanup_target}")
            shutil.rmtree(temporary)
        else:
            print(f"Integration evidence: {temporary}")


if __name__ == "__main__":
    main()
