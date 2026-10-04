"""P9 C2 M1c-RED identity evidence: compile atx-impl/src/strategy_target_replay.cpp (the NAV path's only
norm_ppf caller, through engine::book::normal_scores) with the committed (contraction-free) stats_ext.hpp and with
the pre-fix copy in oldinc/, under each pool-15 tree's own flags (PCH dropped so both variants parse alike), into
this scratch dir only. Writes identity.cmd (vcvars64, then the four clang-cl runs)."""
import json
from pathlib import Path

here = Path(__file__).resolve().parent
root = Path("C:/atx-wt/pool-15")
vcvars = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
want = "atx-impl/src/strategy_target_replay.cpp"
lines = ["@echo off", f'call "{vcvars}" >nul']
for tree, tag in (("build-equity", "dbg"), ("build-equity-rel", "rel")):
    entry = next(e for e in json.loads((root / tree / "compile_commands.json").read_text())
                 if e["file"].replace("\\", "/").endswith(want))
    args = (entry.get("command") or " ".join(entry["arguments"])).split()
    keep = [a for a in args if not a.startswith(("/Yu", "/Fp", "/FI", "/Fo", "/Fd"))]
    k = keep.index("--")
    for variant in ("new", "old"):
        extra = [f"-I{here / 'oldinc'}"] if variant == "old" else []
        obj = here / f"{tag}-{variant}.obj"
        cmd = keep[:2] + extra + keep[2:k] + ["/showIncludes", f"/Fo{obj}"] + keep[k:]
        lines.append(f'cd /d "{entry["directory"]}"')
        lines.append(" ".join(cmd) + f' > "{here / (tag + "-" + variant + ".inc")}" 2>&1')
        lines.append(f"echo {tag}-{variant} exit %ERRORLEVEL%")
(here / "identity.cmd").write_text("\r\n".join(lines) + "\r\n")
print((here / "identity.cmd").read_text()[:600])
