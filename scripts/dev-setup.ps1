# One-time machine setup for the fast `dev` CMake preset. Idempotent. ASCII-only (Windows PowerShell 5.1 safe).
#
# 1. Sets persistent user env vars: ATX_DEPS_DIR (shared FetchContent cache) + SCCACHE_CACHE_SIZE.
# 2. Ensures sccache is on PATH. Tries winget; if winget's source service is disabled
#    (common on locked-down boxes), downloads the latest release binary from GitHub into
#    C:\atx-cache\bin and adds it to the user PATH.
#
# Run once per machine, then open a NEW shell so env/PATH apply. Then use scripts\new-worktree.ps1.

param(
  [string]$DepsDir   = 'C:\atx-cache\deps',
  [string]$BinDir    = 'C:\atx-cache\bin',
  [string]$CacheSize = '20G'
)
$ErrorActionPreference = 'Stop'

Write-Host '== env vars (User) =='
[Environment]::SetEnvironmentVariable('ATX_DEPS_DIR', $DepsDir, 'User')
[Environment]::SetEnvironmentVariable('SCCACHE_CACHE_SIZE', $CacheSize, 'User')
New-Item -ItemType Directory -Force $DepsDir | Out-Null
$env:ATX_DEPS_DIR = $DepsDir
Write-Host ('ATX_DEPS_DIR = ' + $DepsDir)

if (Get-Command sccache -ErrorAction SilentlyContinue) {
  Write-Host ('sccache already on PATH: ' + (Get-Command sccache).Source)
  sccache --version
  Write-Host 'DONE.'
  return
}

Write-Host '== install sccache =='
$ok = $false
try {
  winget install Mozilla.sccache --accept-source-agreements --accept-package-agreements --silent
  if ($LASTEXITCODE -eq 0) { $ok = $true }
} catch { }

if (-not $ok) {
  Write-Warning 'winget unavailable or failed - downloading sccache release binary directly.'
  New-Item -ItemType Directory -Force $BinDir | Out-Null
  $rel = Invoke-RestMethod 'https://api.github.com/repos/mozilla/sccache/releases/latest' -Headers @{ 'User-Agent' = 'atx-dev-setup' }
  $asset = $rel.assets | Where-Object { $_.name -like '*x86_64-pc-windows-msvc.zip' } | Select-Object -First 1
  if (-not $asset) { throw 'No windows-msvc sccache asset found in the latest release.' }
  $zip = Join-Path $env:TEMP $asset.name
  Invoke-WebRequest $asset.browser_download_url -OutFile $zip
  $tmp = Join-Path $env:TEMP 'sccache_extract'
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
  Expand-Archive $zip -DestinationPath $tmp -Force
  $exe = Get-ChildItem $tmp -Recurse -Filter sccache.exe | Select-Object -First 1
  Copy-Item $exe.FullName (Join-Path $BinDir 'sccache.exe') -Force
  $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
  if ($userPath -notlike ('*' + $BinDir + '*')) {
    [Environment]::SetEnvironmentVariable('Path', ($userPath + ';' + $BinDir), 'User')
  }
  $env:Path = $env:Path + ';' + $BinDir
  Write-Host ('installed sccache -> ' + (Join-Path $BinDir 'sccache.exe') + '  (' + $asset.name + ')')
}

& sccache --version
Write-Host ''
Write-Host 'DONE. Open a NEW shell (so PATH/env apply), then: scripts\new-worktree.ps1 -Name <name>'
