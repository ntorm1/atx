"""Build and run the local ATX research workstation; Ctrl+C stops both children."""
from __future__ import annotations

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import subprocess
import tempfile
import time
from urllib.error import URLError
from urllib.request import Request, urlopen


def run_checked(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def request_shutdown(_signal: int, _frame) -> None:
    raise KeyboardInterrupt


def snapshot_native(stack: ExitStack, native: Path, runtime: Path) -> Path:
    """Keep Windows build outputs replaceable while this server process is running."""
    runtime = runtime.resolve(strict=True)
    folder = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="native-", dir=runtime)))
    if folder.resolve().parent != runtime:
        raise RuntimeError("Native snapshot escaped the owned runtime directory")
    shutil.copy2(native, folder / native.name)
    for dependency in native.parent.glob("*.dll"):
        shutil.copy2(dependency, folder / dependency.name)
    notice = native.parent / "atx-server-THIRD_PARTY_NOTICES.txt"
    if notice.is_file():
        shutil.copy2(notice, folder / notice.name)
    return folder / native.name


def data_configuration(repo: Path, sources: list[str], roots: list[Path], disabled: bool,
                       environment: dict[str, str]) -> tuple[list[str], list[Path]]:
    """Resolve operator-owned sources without opening or modifying warehouse files."""
    if disabled:
        if sources or roots:
            raise ValueError("--no-data-sources cannot be combined with --data-source/--data-root")
        return [], []
    resolved_sources: list[str] = []
    resolved_roots: list[Path] = []
    configured_sources = list(sources)
    if not configured_sources:
        explicit_path = environment.get("ATX_DB_PATH")
        data_dir = Path(os.path.expandvars(str(
            environment.get("ATX_DB_DATA_DIR") or repo / "atx-db/data"))).expanduser()
        if explicit_path:
            selected = Path(os.path.expandvars(explicit_path)).expanduser().resolve(strict=True)
        else:
            selected = next((path for path in [data_dir / "catalog.duckdb", data_dir / "warehouse.duckdb"]
                             if path.is_file()), None)
        if selected is not None:
            selected = selected.resolve(strict=True)
            configured_sources.append(f"atx-db={selected}")
            # The serving catalog's views read this published lake. No raw/source caches are exposed.
            lake = selected.parent / "alpha_panel/v1"
            if not roots and selected.name == "catalog.duckdb" and lake.is_dir():
                resolved_roots.append(lake.resolve(strict=True))
    seen: set[str] = set()
    for specification in configured_sources:
        source_id, separator, raw_path = specification.partition("=")
        if not separator or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", source_id):
            raise ValueError("Data sources must be ID=PATH with a 1..64 character alphanumeric/_/- ID")
        if source_id in seen:
            raise ValueError(f"Duplicate data source ID: {source_id}")
        path = Path(os.path.expandvars(raw_path)).expanduser().resolve(strict=True)
        if not path.is_file():
            raise ValueError(f"Data source is not a regular file: {path}")
        seen.add(source_id)
        resolved_sources.append(f"{source_id}={path}")
    for root in roots:
        path = Path(os.path.expandvars(str(root))).expanduser().resolve(strict=True)
        if not path.is_dir():
            raise ValueError(f"Data root is not a directory: {path}")
        resolved_roots.append(path)
    return resolved_sources, list(dict.fromkeys(resolved_roots))


def stop(child: subprocess.Popen) -> None:
    if child.poll() is not None:
        return
    try:
        child.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGTERM)
        child.wait(timeout=15)
    except (OSError, subprocess.TimeoutExpired):
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=5)


def research_configuration(repo: Path, datasets: list[str], disabled: bool,
                           environment: dict[str, str]) -> list[str]:
    """Pin an explicit manifest revision; queued recipes also carry this SHA256."""
    if disabled:
        if datasets:
            raise ValueError("--no-research-datasets cannot be combined with --research-dataset")
        return []
    configured = list(datasets)
    if not configured:
        data_dir = Path(os.path.expandvars(str(
            environment.get("ATX_DB_DATA_DIR") or repo / "atx-db/data"))).expanduser()
        manifest = data_dir / "alpha_panel/v1/panel/manifest.json"
        if manifest.is_file():
            configured.append(f"atx-equity-panel={manifest}")
    if len(configured) > 32:
        raise ValueError("At most 32 research datasets may be registered")
    result: list[str] = []
    seen: set[str] = set()
    for specification in configured:
        dataset_id, separator, value = specification.partition("=")
        if not separator or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", dataset_id):
            raise ValueError("Research datasets must be ID=PATH or ID=SHA256=PATH")
        if dataset_id in seen:
            raise ValueError(f"Duplicate research dataset ID: {dataset_id}")
        expected, equal, remainder = value.partition("=")
        pinned = bool(equal and re.fullmatch(r"[0-9a-fA-F]{64}", expected))
        path = Path(os.path.expandvars(remainder if pinned else value)).expanduser().resolve(
            strict=True)
        if not path.is_file():
            raise ValueError(f"Research manifest is not a regular file: {path}")
        with path.open("rb") as stream:
            content = stream.read(1024 * 1024 + 1)
        if len(content) > 1024 * 1024:
            raise ValueError("Research manifest exceeds 1 MiB")
        receipt = json.loads(content)
        if not isinstance(receipt, dict) or receipt.get("schema") != "atx.alpha-panel.panel/v2" or (
                receipt.get("status") != "complete"):
            raise ValueError("Research manifest must be a complete atx.alpha-panel.panel/v2 receipt")
        digest = hashlib.sha256(content).hexdigest()
        if pinned and expected.lower() != digest:
            raise ValueError(f"Research manifest SHA256 mismatch: {dataset_id}")
        seen.add(dataset_id)
        result.append(f"{dataset_id}={digest}={path}")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true", help="Configure/build native and web apps")
    parser.add_argument("--preset", choices=["server-dev", "server-rel"], default="server-dev",
                        help="Native build configuration (use server-rel for optimized execution)")
    parser.add_argument("--server", type=Path, help="Native executable, default build-server/bin")
    parser.add_argument("--no-native-snapshot", action="store_true",
                        help="Run the Windows build output directly (locks it until shutdown)")
    parser.add_argument("--port", type=int, default=3000)
    parser.add_argument("--grpc-port", type=int, default=50051)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--team-config", type=Path,
                        help="Operator-managed hashed team credentials and roles (native JSON)")
    parser.add_argument("--artifact-root", type=Path, action="append", default=[])
    parser.add_argument("--db", type=Path, help="Persistent SQLite file")
    parser.add_argument("--data-source", action="append", default=[], metavar="ID=PATH",
                        help="Read-only DuckDB source; repeatable, overrides atx-db autodetection")
    parser.add_argument("--data-root", type=Path, action="append", default=[],
                        help="Approved directory for database view/SELECT file reads; repeatable")
    parser.add_argument("--no-data-sources", action="store_true",
                        help="Disable database viewing and atx-db autodetection")
    parser.add_argument("--research-dataset", action="append", default=[], metavar="ID=[SHA256=]PATH",
                        help="Pin a published research manifest; repeatable, overrides autodetection")
    parser.add_argument("--no-research-datasets", action="store_true",
                        help="Disable published equity panel autodetection and research registration")
    args = parser.parse_args()
    if not (1 <= args.port <= 65535 and 1 <= args.grpc_port <= 65535):
        parser.error("Ports must be in 1..65535")
    if args.port == args.grpc_port:
        parser.error("HTTP and gRPC require distinct ports")
    if not 1 <= args.workers <= 64:
        parser.error("Workers must be in 1..64")
    repo = Path(__file__).resolve().parents[1]
    front = repo / "atx-front"
    build_directory = "build-server-rel" if args.preset == "server-rel" else "build-server"
    native = (args.server or repo / build_directory / "bin" /
              ("atx-server.exe" if os.name == "nt" else "atx-server")).resolve()
    node, npm = shutil.which("node"), shutil.which("npm.cmd" if os.name == "nt" else "npm")
    if not node or not npm:
        parser.error("Node.js 22+ and npm are required")
    if args.build:
        if os.name != "nt":
            parser.error("Use the documented CMake build on Linux, then launch without --build")
        wrapper = str(repo / "scripts/atx-build.ps1")
        run_checked(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", wrapper,
                     "-Preset", args.preset, "configure"], repo)
        run_checked(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", wrapper,
                     "-Preset", args.preset, "-Jobs", "4", "build", "atx-server"], repo)
        run_checked([npm, "ci"], front)
        run_checked([npm, "run", "build"], front)
    if not native.is_file() or not (front / "dist/index.html").is_file():
        parser.error("Build the workstation first, or use --build")
    runtime = repo / "atx-server/var"
    runtime.mkdir(parents=True, exist_ok=True)
    database = (args.db or runtime / "workspace.db").resolve()
    roots = [path.resolve(strict=True) for path in (args.artifact_root or [repo])]
    environment = dict(os.environ)
    if args.team_config and environment.get("ATX_WEB_TOKEN"):
        parser.error("--team-config cannot be combined with the shared ATX_WEB_TOKEN")
    team_config = args.team_config.resolve(strict=True) if args.team_config else None
    if team_config and not team_config.is_file():
        parser.error("Team configuration must be a regular file")
    environment["ATX_WEB_TEAM_AUTH"] = "1" if team_config else "0"
    try:
        data_sources, data_roots = data_configuration(
            repo, args.data_source, args.data_root, args.no_data_sources, environment)
        research_datasets = research_configuration(
            repo, args.research_dataset, args.no_research_datasets, environment)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    environment["ATX_SERVER_TOKEN"] = environment.get("ATX_SERVER_TOKEN") or secrets.token_hex(32)
    environment.update({
        "ATX_GRPC_ADDRESS": f"127.0.0.1:{args.grpc_port}", "ATX_GRPC_TLS": "0",
        "ATX_WEB_HOST": "127.0.0.1", "ATX_WEB_PORT": str(args.port),
    })
    command = [str(native), "--listen", environment["ATX_GRPC_ADDRESS"],
               "--db", str(database), "--repo-root", str(repo), "--workers", str(args.workers)]
    if team_config:
        command.extend(["--team-config", str(team_config)])
    for root in roots:
        command.extend(["--artifact-root", str(root)])
    for source in data_sources:
        command.extend(["--data-source", source])
    for root in data_roots:
        command.extend(["--data-root", str(root)])
    for dataset in research_datasets:
        command.extend(["--research-dataset", dataset])
    signal.signal(signal.SIGTERM, request_shutdown)
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, request_shutdown)
    with ExitStack() as stack:
        # ExitStack stops children before removing the owned snapshot directory.
        if os.name == "nt" and not args.no_native_snapshot:
            command[0] = str(snapshot_native(stack, native, runtime))
        children = []
        for name, invocation, cwd in [
            ("server", command, repo), ("gateway", [node, "gateway/main.mjs"], front),
        ]:
            output = stack.enter_context((runtime / f"{name}.log").open("ab"))
            child = subprocess.Popen(
                invocation, cwd=cwd, env=environment, stdin=subprocess.DEVNULL,
                stdout=output, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0,
            )
            children.append(child)
            stack.callback(stop, child)
        url = f"http://127.0.0.1:{args.port}"
        headers = {}
        if environment.get("ATX_WEB_TOKEN"):
            headers["Authorization"] = f"Bearer {environment['ATX_WEB_TOKEN']}"
        try:
            deadline = time.monotonic() + 45
            ready = False
            while time.monotonic() < deadline:
                if any(child.poll() is not None for child in children):
                    raise RuntimeError(f"Workspace process exited; inspect logs in {runtime}")
                try:
                    with urlopen(Request(url + "/healthz", headers=headers), timeout=3) as response:
                        health = json.load(response)
                        ready = health.get("status", "").lower() in {"serving", "healthy", "ok", "ready"}
                except (URLError, TimeoutError):
                    pass
                if ready:
                    break
                time.sleep(0.2)
            if not ready:
                raise RuntimeError(f"Readiness timeout; inspect logs in {runtime}")
            print(f"ATX workspace: {url}\nDatabase: {database}\n"
                  f"Data sources: {', '.join(item.partition('=')[0] for item in data_sources) or 'none'}\n"
                  f"Research datasets: {', '.join(item.partition('=')[0] for item in research_datasets) or 'none'}\n"
                  f"Logs: {runtime}\nCtrl+C to stop.",
                  flush=True)
            while all(child.poll() is None for child in children):
                time.sleep(0.5)
            raise RuntimeError(f"Workspace process exited; inspect logs in {runtime}")
        except KeyboardInterrupt:
            print("Stopping the ATX workspace...", flush=True)


if __name__ == "__main__":
    main()
