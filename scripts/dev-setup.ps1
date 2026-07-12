# One-time machine setup for the fast `dev` CMake preset. Idempotent. ASCII-only (Windows PowerShell 5.1 safe).
#
# 1. Sets persistent user env vars: ATX_DEPS_DIR (shared FetchContent cache) + SCCACHE_CACHE_SIZE.
# 2. Installs ccache (the compiler cache the presets are tuned for) into C:\atx-cache\bin
#    and sets its machine-global config (cache dir + size). The per-build hashing keys
#    (CCACHE_BASEDIR/NOHASHDIR/SLOPPINESS/IGNOREOPTIONS) live in CMakePresets.json, NOT
#    here, so builds behave identically even if this script was never re-run.
# 3. Best-effort installs sccache as a fallback launcher (root CMakeLists prefers ccache).
#
# Run once per machine, then open a NEW shell so env/PATH apply. Then use scripts\new-worktree.ps1.

param(
  [string]$DepsDir    = 'C:\atx-cache\deps',
  [string]$BinDir     = 'C:\atx-cache\bin',
  [string]$CcacheDir  = 'C:\atx-cache\ccache',
  [string]$CcacheVer  = '4.13.6',
  [string]$CacheSize  = '40G'
)
$ErrorActionPreference = 'Stop'

Write-Host '== env vars (User) =='
[Environment]::SetEnvironmentVariable('ATX_DEPS_DIR', $DepsDir, 'User')
[Environment]::SetEnvironmentVariable('SCCACHE_CACHE_SIZE', $CacheSize, 'User')
New-Item -ItemType Directory -Force $DepsDir | Out-Null
New-Item -ItemType Directory -Force $BinDir | Out-Null
$env:ATX_DEPS_DIR = $DepsDir
Write-Host ('ATX_DEPS_DIR = ' + $DepsDir)

Write-Host '== ccache (preferred compiler cache) =='
$ccacheExe = Join-Path $BinDir 'ccache.exe'
if (-not (Test-Path $ccacheExe)) {
  $name = 'ccache-' + $CcacheVer + '-windows-x86_64'
  $zip = Join-Path $env:TEMP ($name + '.zip')
  Invoke-WebRequest -Uri ('https://github.com/ccache/ccache/releases/download/v' + $CcacheVer + '/' + $name + '.zip') -OutFile $zip
  $tmp = Join-Path $env:TEMP 'ccache_extract'
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
  Expand-Archive $zip -DestinationPath $tmp -Force
  Copy-Item (Join-Path $tmp ($name + '\ccache.exe')) $ccacheExe -Force
}
& $ccacheExe --set-config ('cache_dir=' + $CcacheDir)
& $ccacheExe --set-config ('max_size=' + $CacheSize)
# Worktree-INVARIANT hashing keys belong in the global config: CMake preset
# `environment` blocks only apply to `cmake --preset` / `cmake --build --preset`
# invocations, NOT to a raw `cmake --build build` or bare ninja — without these,
# such builds mark every PCH TU "could not use precompiled header" (uncacheable)
# and hash the cwd (no cross-worktree hits). Only CCACHE_BASEDIR varies per
# worktree; scripts/atx-build.ps1 exports that one itself.
& $ccacheExe --set-config 'hash_dir=false'
& $ccacheExe --set-config 'sloppiness=pch_defines,time_macros'
& $ccacheExe --set-config 'ignore_options=-ffile-prefix-map=* /clang:-ffile-prefix-map=*'
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($userPath -notlike ('*' + $BinDir + '*')) {
  [Environment]::SetEnvironmentVariable('Path', ($userPath + ';' + $BinDir), 'User')
}
if ($env:Path -notlike ('*' + $BinDir + '*')) { $env:Path = $env:Path + ';' + $BinDir }
Write-Host ('ccache ready: ' + ((& $ccacheExe --version) | Select-Object -First 1) + '  cache=' + $CcacheDir + ' max=' + $CacheSize)

Write-Host '== sccache (fallback launcher only; ccache is preferred) =='
if (Get-Command sccache -ErrorAction SilentlyContinue) {
  Write-Host ('sccache already on PATH: ' + (Get-Command sccache).Source)
} else {
  $ok = $false
  try {
    winget install Mozilla.sccache --accept-source-agreements --accept-package-agreements --silent
    if ($LASTEXITCODE -eq 0) { $ok = $true }
  } catch { }
  if (-not $ok) {
    try {
      $rel = Invoke-RestMethod 'https://api.github.com/repos/mozilla/sccache/releases/latest' -Headers @{ 'User-Agent' = 'atx-dev-setup' }
      $asset = $rel.assets | Where-Object { $_.name -like '*x86_64-pc-windows-msvc.zip' } | Select-Object -First 1
      if ($asset) {
        $zip = Join-Path $env:TEMP $asset.name
        Invoke-WebRequest $asset.browser_download_url -OutFile $zip
        $tmp = Join-Path $env:TEMP 'sccache_extract'
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
        Expand-Archive $zip -DestinationPath $tmp -Force
        $exe = Get-ChildItem $tmp -Recurse -Filter sccache.exe | Select-Object -First 1
        Copy-Item $exe.FullName (Join-Path $BinDir 'sccache.exe') -Force
        Write-Host ('installed sccache -> ' + (Join-Path $BinDir 'sccache.exe'))
      }
    } catch { Write-Warning ('sccache fallback install skipped: ' + $_.Exception.Message) }
  }
}

Write-Host ''
Write-Host 'DONE. Open a NEW shell (so PATH/env apply), then: scripts\new-worktree.ps1 -Name <name>'
