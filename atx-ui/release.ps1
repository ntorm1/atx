[CmdletBinding()]
param(
    [string]$Symbol = "SPY",
    [switch]$NoLaunch
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$releaseBin = Join-Path $repoRoot "build-rel\bin"
$sourceExe = Join-Path $releaseBin "atx-ui.exe"
$sampleData = Join-Path $repoRoot "data\spy_opra_cbbo1m_2026-06-05T1955Z.parquet"

function Invoke-CMake {
    param([Parameter(Mandatory)][string[]]$Arguments)

    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "cmake failed with exit code ${LASTEXITCODE}: cmake $($Arguments -join ' ')"
    }
}

Push-Location $repoRoot
try {
    Write-Host "[release] Configuring Release preset..." -ForegroundColor Cyan
    Invoke-CMake -Arguments @("--preset", "rel")

    Write-Host "[release] Building atx-ui..." -ForegroundColor Cyan
    Invoke-CMake -Arguments @("--build", "--preset", "rel", "--target", "atx-ui")
} finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
    throw "Release executable was not produced: $sourceExe"
}

$desktop = [Environment]::GetFolderPath([Environment+SpecialFolder]::DesktopDirectory)
if ([string]::IsNullOrWhiteSpace($desktop)) {
    throw "Windows did not return a Desktop directory."
}

# Keep the executable, its Arrow/Parquet runtime DLLs, and the default snapshot
# together so the deployed copy can also be started directly from Explorer.
$deployDirectory = Join-Path $desktop "ATX Vol Release"
$deployDataDirectory = Join-Path $deployDirectory "data"
New-Item -ItemType Directory -Path $deployDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $deployDataDirectory -Force | Out-Null

$deployedExe = Join-Path $deployDirectory "atx-ui.exe"
Copy-Item -LiteralPath $sourceExe -Destination $deployedExe -Force

Get-ChildItem -LiteralPath $releaseBin -Filter "*.dll" -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $deployDirectory -Force
}

if (Test-Path -LiteralPath $sampleData -PathType Leaf) {
    Copy-Item -LiteralPath $sampleData -Destination $deployDataDirectory -Force
}

Write-Host "[release] Deployed: $deployedExe" -ForegroundColor Green

if (-not $NoLaunch) {
    Write-Host "[release] Launching $Symbol workspace..." -ForegroundColor Cyan
    $process = Start-Process -FilePath $deployedExe `
        -ArgumentList @("--symbol", $Symbol) `
        -WorkingDirectory $deployDirectory `
        -PassThru

    if ($process.WaitForExit(2500)) {
        throw "Deployed atx-ui exited during startup with code $($process.ExitCode)."
    }
    Write-Host "[release] Running (PID $($process.Id))." -ForegroundColor Green
}
