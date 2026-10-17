"""R15-WAVE-COMMIT: real temporary-repository scope/trailer and unchanged default command contract."""
from pathlib import Path
import subprocess
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wave_context import Wave
import wave_steps as WS


def test_wave_commit_policy_real_git(tmp_path, monkeypatch):
    trailer = "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
    cache = ".superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache"
    exclude = ":(exclude)" + cache
    own = "scripts/cell.json"
    assert WS.commit_argvs([own], "cell") == [
        ["git", "add", "--", own], ["git", "commit", "-q", "-m", "cell", "--", own]]

    def git(*args):
        return subprocess.run(["git", *args], cwd=tmp_path, check=True,
                              capture_output=True, text=True).stdout.strip()

    git("init", "-q")
    git("config", "user.name", "Wave test")
    git("config", "user.email", "wave-test@example.invalid")
    git("config", "commit.gpgSign", "false")
    seed = tmp_path / "seed.txt"
    seed.write_text("seed\n", encoding="utf-8")
    git("add", "--", "seed.txt", exclude)
    git("commit", "-q", "-m", "seed", "-m", trailer, "--", "seed.txt", exclude)
    for rel in (own, "notes.txt", cache + "/entry.json"):
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("{}\n", encoding="utf-8")
    # An unrelated staged file and staged cache must not hitchhike in the wave commit.
    git("add", "--", "notes.txt", cache + "/entry.json")
    monkeypatch.setenv("ATX_WAVE_COMMIT_TRAILER", trailer)
    monkeypatch.setenv("ATX_WAVE_COMMIT_EXCLUDE", exclude)
    w = object.__new__(Wave)
    w.root, w.processes = tmp_path, []
    w._local, w._console_lock = threading.local(), threading.Lock()
    calls = []

    def run(argv, what):
        calls.append(argv)
        return subprocess.run(argv, cwd=tmp_path, env=w.env(), check=True,
                              capture_output=True, text=True)

    w.run = run
    committed = w.commit_paths("cell", {own})
    assert committed == git("rev-parse", "HEAD")
    assert calls == [["git", "add", "--", own, exclude],
                     ["git", "commit", "-q", "-m", "cell", "-m", trailer, "--", own, exclude]]
    assert git("show", "--format=", "--name-only", "HEAD").splitlines() == [own]
    assert git("log", "-1", "--format=%B") == "cell\n\n" + trailer
    assert set(git("diff", "--cached", "--name-only").splitlines()) == {"notes.txt", cache + "/entry.json"}
    assert w.commit_paths("cell", {own}) is None
    assert len(calls) == 2
