<#
.SYNOPSIS
  Tracked research build (platform v8 E-1): a RAM-admitted, target-scoped build of this worktree through
  scripts\atx-build.ps1, with a one-time receipt build-equity\mega-<Tag>-receipt.json.

.DESCRIPTION
  Replaces the untracked build-equity\mega-build.ps1 (same arguments, same admission, same receipt path and
  fields). The repository root is the parent of this script's directory, so the script always builds the worktree
  that contains it, whatever the caller's working directory.

  Admission: free physical memory >= 1000 MiB and free commit >= 2500 MiB, else it refuses (wait; do not lower).
  Jobs: 4 at >= 3400 MiB free, 3 at >= 2200 MiB, else 2. A tag whose receipt already exists is refused, so every
  receipt describes exactly one build. Outputs, all under <root>\build-equity:
    mega-<Tag>-build.log        the atx-build output (all streams)
    mega-<Tag>-cache-stats.log  ccache statistics log (CCACHE_STATSLOG)
    mega-<Tag>-receipt.json     Source, Preset, DirtyEntries, ConfiguredProvenance, Jobs, FreeMiB, CommitMiB,
                                Targets, ExitCode, WallSeconds, CompiledTUs, Links (as mega-build.ps1), plus Tag,
                                Script, BuildDir and Executables (SHA-256 of <BuildDir>\bin\<target>.exe for every
                                target that is an executable, after a successful build).
  -DryRun prints the plan (root, build dir, jobs, receipt, atx-build command) and writes and builds nothing.

.PARAMETER Tag
  Unique receipt tag, e.g. v8-1 (letters, digits, '.', '_', '-').

.PARAMETER Targets
  Build targets, as several arguments or one comma-separated string ("a,b"). Never empty: no all-target builds.

.PARAMETER Preset
  equity-dev (Debug tree build-equity\) or equity-rel (Release tree build-equity-rel\), as in CMakePresets.json.

.EXAMPLE
  powershell -File scripts\research-build.ps1 -Tag v8-1 -Targets "atx-impl-strategy-target-tests,atx-equity-strategy-targets"

.EXAMPLE
  powershell -File scripts\research-build.ps1 -Tag v8-rel1 -Targets "atx-equity-strategy-ic" -Preset equity-rel
#>
[CmdletBinding(PositionalBinding = $false)]
param(
  [Parameter(Mandatory = $true)]
  [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
  [string] $Tag,
  [Parameter(Mandatory = $true)]
  [string[]] $Targets,
  [ValidateSet('equity-dev', 'equity-rel')]
  [string] $Preset = 'equity-dev',
  [switch] $DryRun
)

$ErrorActionPreference = 'Stop'

$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
$builder = Join-Path $root 'scripts\atx-build.ps1'
$receiptDir = Join-Path $root 'build-equity'

# Resolve a configure preset's binaryDir as CMake does (the atx-build.ps1 rule): walk `inherits` to the first
# binaryDir and expand ${sourceDir}. Bounded by the preset count, so a cyclic `inherits` cannot hang.
function Get-PresetBinaryDir {
  param([Parameter(Mandatory = $true)][string] $Root, [Parameter(Mandatory = $true)][string] $Name)
  $presets = @((Get-Content -LiteralPath (Join-Path $Root 'CMakePresets.json') -Raw | ConvertFrom-Json).configurePresets)
  $current = $Name
  for ($hop = 0; $hop -lt $presets.Count; $hop++) {
    $preset = $presets | Where-Object { $_.name -eq $current } | Select-Object -First 1
    if ($null -eq $preset) { break }
    if ($preset.binaryDir) {
      return Join-Path $Root (($preset.binaryDir -replace '\$\{sourceDir\}/?', '') -replace '/', '\')
    }
    if (-not $preset.inherits) { break }
    $current = @($preset.inherits)[0]
  }
  throw "research-build: cannot resolve binaryDir for preset '$Name' from CMakePresets.json"
}

$Targets = @($Targets | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($Targets.Count -eq 0) { throw 'research-build: no targets (bare all-target builds are not allowed)' }
foreach ($target in $Targets) {
  if ($target -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') { throw "research-build: invalid target name '$target'" }
}
if (-not (Test-Path -LiteralPath $builder)) { throw "research-build: $builder not found" }
$buildDir = Get-PresetBinaryDir -Root $root -Name $Preset

Set-Location -LiteralPath $root
$dirty = @(& git status --porcelain).Count
$source = (& git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $source -notmatch '^[0-9a-f]{40}$') { throw "research-build: $root is not a git worktree" }

$os = Get-CimInstance Win32_OperatingSystem
$free = [math]::Floor($os.FreePhysicalMemory / 1024)
$mem = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
$commit = [math]::Floor(($mem.CommitLimit - $mem.CommittedBytes) / 1MB)
$jobs = if ($free -ge 3400) { 4 } elseif ($free -ge 2200) { 3 } else { 2 }

$base = Join-Path $receiptDir "mega-$Tag"
$receipt = "$base-receipt.json"
$log = "$base-build.log"

if ($DryRun) {
  [pscustomobject]@{Root = $root; Source = $source; DirtyEntries = $dirty; Preset = $Preset; BuildDir = $buildDir;
    Targets = $Targets; Jobs = $jobs; FreeMiB = $free; CommitMiB = $commit;
    Admitted = ($free -ge 1000 -and $commit -ge 2500); Receipt = $receipt; ReceiptExists = (Test-Path -LiteralPath $receipt);
    Command = "& '$builder' build -Preset $Preset -Jobs $jobs $($Targets -join ' ')"} | Format-List
  exit 0
}

if ($free -lt 1000 -or $commit -lt 2500) { throw "memory admission: free=$free commit=$commit" }
if (Test-Path -LiteralPath $receipt) { throw "receipt exists: $receipt" }
New-Item -ItemType Directory -Force -Path $receiptDir | Out-Null

$env:CCACHE_STATSLOG = "$base-cache-stats.log"
$sw = [Diagnostics.Stopwatch]::StartNew()
$ErrorActionPreference = 'Continue'
& $builder build -Preset $Preset -Jobs $jobs @Targets *> $log
$exit = $LASTEXITCODE
$sw.Stop()
$ErrorActionPreference = 'Stop'

$provenanceFile = Join-Path $buildDir 'atx-impl\generated\build_provenance.cpp'
$provenance = if (Test-Path -LiteralPath $provenanceFile) {
  [regex]::Match((Get-Content -LiteralPath $provenanceFile -Raw), '[a-f0-9]{40}(?:-dirty)?').Value
} else { '' }
$lines = @(if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log })
$cxx = @($lines | Where-Object { $_ -match 'Building CXX' }).Count
$links = @($lines | Where-Object { $_ -match 'Linking' }).Count

$executables = [ordered]@{}
if ($exit -eq 0) {
  foreach ($target in $Targets) {
    $exe = Join-Path $buildDir "bin\$target.exe"
    if (Test-Path -LiteralPath $exe) {
      $executables[$target] = [ordered]@{Path = $exe; Sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
  }
}

[pscustomobject][ordered]@{Source = $source; Preset = $Preset; DirtyEntries = $dirty; ConfiguredProvenance = $provenance;
  Jobs = $jobs; FreeMiB = $free; CommitMiB = $commit; Targets = $Targets; ExitCode = $exit;
  WallSeconds = $sw.Elapsed.TotalSeconds; CompiledTUs = $cxx; Links = $links; Tag = $Tag;
  Script = 'scripts/research-build.ps1'; BuildDir = $buildDir; Executables = $executables} |
  ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $receipt -Encoding ASCII
Get-Content -LiteralPath $receipt
$lines | Where-Object { $_ -match 'error|FAILED|Building CXX|Linking' } | Select-Object -First 40 |
  ForEach-Object { $_.Substring(0, [Math]::Min(300, $_.Length)) }
exit $exit
