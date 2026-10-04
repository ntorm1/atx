"""Which functions of two llvm-objdump listings differ (addresses and rip-relative comments normalized)."""
import re
import sys
from pathlib import Path

head = re.compile(r"^[0-9a-f]+ <(.+)>:$")


def functions(path: Path) -> dict:
    out, name = {}, None
    for line in path.read_text(errors="replace").splitlines():
        m = head.match(line.strip())
        if m:
            name = m.group(1)
            out[name] = []
            continue
        if name is None or not line.strip():
            continue
        body = line.split(":", 1)[1] if ":" in line else line
        body = re.sub(r"#.*$", "", body)
        body = re.sub(r"0x[0-9a-f]+", "ADDR", body)
        out[name].append(body.split())
    return out


a, b = functions(Path(sys.argv[1])), functions(Path(sys.argv[2]))
fma = sum(1 for f in (a, b) for body in f.values() for ins in body if ins and "fmadd" in ins[0])
diff = sorted(n for n in set(a) | set(b) if a.get(n) != b.get(n))
print(f"functions {len(a)} / {len(b)}; differing {len(diff)}; fused multiply-add instructions {fma}")
for n in diff[:20]:
    print("  ", n[:160])
