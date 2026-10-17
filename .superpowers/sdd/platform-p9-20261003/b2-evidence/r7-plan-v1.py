"""Print `research_wave plan` of wave_fixture's v1 manifest on ROOT with the scripts tree SCRIPTS (its own tests dir)."""
import sys
from pathlib import Path
scripts, root, mode = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
sys.path.insert(0, str(scripts / "tests"))
sys.path.insert(0, str(scripts))
import wave_fixture as F  # noqa: E402
if mode == "build":
    F.build(root)
    sys.exit(0)
import research_wave  # noqa: E402
lines = []
admit = {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", 1), "alpha_c": ("admitted", 1)}
code = research_wave.main(["plan", F.MANIFEST, "--root", str(root)], executor=F.FakeCycle(root, admit), log=lines.append)
repo = str(scripts.parent.resolve()).replace("\\", "/")
print(code)
for line in lines:
    print(line.replace(repo, "<REPO>").replace(str(scripts.parent.resolve()), "<REPO>"))
