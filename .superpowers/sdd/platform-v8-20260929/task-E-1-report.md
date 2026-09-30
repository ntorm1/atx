# Task E-1 report: tracked research build script

Lane W0E, pool-3. E-1 only (E-2 is root's measurement). Nothing built.

## What was built
`scripts/research-build.ps1`, from `C:/atx-wt/pool-2/build-equity/mega-build.ps1` (read there, untracked).
PowerShell 5.1 syntax, ASCII only (0 non-ASCII bytes, LF), parser check 0 errors.

Interface: `-Tag <tag>` (mandatory; `^[A-Za-z0-9][A-Za-z0-9._-]*$`, so no path escapes), `-Targets` (mandatory;
several arguments or one comma-separated string, as before; empty refused; each name validated),
`-Preset equity-dev|equity-rel` (default `equity-dev`, the names in CMakePresets.json), `-DryRun`.

Kept from mega-build.ps1: memory admission (free >= 1000 MiB and commit >= 2500 MiB, else throw), Jobs 4/3/2 by
free RAM, refusal of an existing `build-equity/mega-<tag>-receipt.json`, `CCACHE_STATSLOG`, the build through
`scripts/atx-build.ps1 build -Preset -Jobs <targets>` with all streams to `mega-<tag>-build.log`, the receipt
fields (Source, Preset, DirtyEntries, ConfiguredProvenance, Jobs, FreeMiB, CommitMiB, Targets, ExitCode,
WallSeconds, CompiledTUs, Links), the printed log excerpt and the exit code.

Changed:
- Root = parent of the script's directory (`$PSScriptRoot/..`), not the hard-coded `C:/atx-wt/pool-2`; the script
  `Set-Location`s there, so atx-build's wrong-tree guard passes from any cwd.
- Build dir resolved from CMakePresets.json (`binaryDir` through `inherits`, as atx-build.ps1 does) instead of a
  string rule: equity-dev -> `build-equity`, equity-rel -> `build-equity-rel`.
- Receipts stay in `<root>/build-equity/` for both presets (as today); the directory is created if missing.
- Receipt additions: `Tag`, `Script`, `BuildDir`, `Executables` ({target: {Path, Sha256}} for every target with a
  `<BuildDir>\bin\<target>.exe`, only after exit 0) for the E-2 identity canary. Written ASCII with `-Depth 4`.
- A missing `build_provenance.cpp` records an empty provenance instead of failing after the build.
- `-DryRun` prints root, source, dirty count, build dir, jobs, admission, receipt path and the atx-build command,
  and writes and builds nothing.

## How root verifies
- `powershell -File scripts\research-build.ps1 -Tag x -Targets "atx-impl-tests" -DryRun` (run here from pool-3:
  BuildDir `C:\atx-wt\pool-3\build-equity`; with `-Preset equity-rel` `...\build-equity-rel`; command shape as
  mega-build.ps1). `-Tag ../x` and `-Preset equity` are refused by parameter validation (checked here).
- First real use: `powershell -File scripts\research-build.ps1 -Tag v8-<n> -Targets "<t1,t2>" [-Preset equity-rel]`
  from pool-2; the receipt must carry the same keys as a `mega-v7-*` receipt plus the four new ones.

## Deviations
- The brief's `-Preset equity|equity-rel` is `equity-dev|equity-rel` (the preset names). The plan's example
  (plan line 1045, `-Preset equity`) is refused by ValidateSet; use `-Preset equity-dev` or omit it.
- `atx-impl/CMakeLists.txt:86-95` (listed in the E-1/E-2 brief) is E-2's Release adoption and was not touched.

## Cross-lane edits
None.

## Open risks
- Not executed beyond `-DryRun` (lane rule: no builds). The build path is the mega-build.ps1 path with the root and
  build dir made relative; the receipt-exists refusal comes before any build.
- `*>` in Windows PowerShell 5.1 writes the build log as UTF-16, as mega-build.ps1 did; readers use Get-Content.
