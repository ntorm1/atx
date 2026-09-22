"""Bounded incremental probe: allocation must fail below a 128 MiB job cap."""
blocks = []
allocated_mb = 0
try:
    for _ in range(24):
        blocks.append(bytearray(8 * 1024 * 1024))
        allocated_mb += 8
except MemoryError:
    blocks.clear()
    assert allocated_mb < 128, allocated_mb
    print(f"memory_cap_enforced: allocated_mb={allocated_mb}", flush=True)
else:
    blocks.clear()
    raise SystemExit("Memory cap was not enforced")
