# Scoped W0-R04 ASan qualification. PowerShell 5.1, native Windows clang-cl.
param([ValidateRange(1, 3)][int]$Jobs = 1)
$ErrorActionPreference = 'Stop'
$sectorRepo = Split-Path -Parent $PSScriptRoot
$sectorWrapper = Join-Path $PSScriptRoot 'atx-build.ps1'
Set-Location -LiteralPath $sectorRepo
$sectorBuild = Join-Path $sectorRepo 'build-equity-asan'
$sectorSourceSha = & git -C $sectorRepo rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify ASan source revision' }
$sectorSourceDirty = & git -C $sectorRepo status --porcelain --untracked-files=no
Write-Output ('Source HEAD: ' + $sectorSourceSha + '; tracked changes=' +
    [bool]$sectorSourceDirty)
$env:CMAKE_BUILD_PARALLEL_LEVEL = [string]$Jobs
& $sectorWrapper configure -Preset equity-asan -Jobs $Jobs
if ($LASTEXITCODE -ne 0) { throw 'ASan configure failed' }
$sectorCache = Get-Content -LiteralPath (Join-Path $sectorBuild 'CMakeCache.txt')
$sectorExpectedDeps = ($sectorRepo.Replace('\', '/') + '/deps/equity-asan')
if (-not ($sectorCache -contains ('FETCHCONTENT_BASE_DIR:PATH=' + $sectorExpectedDeps))) {
    throw 'ASan FetchContent isolation mismatch'
}
$sectorFreeGB = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB
Write-Output ('Free RAM GiB before ASan build: ' + $sectorFreeGB)
if ($sectorFreeGB -le 4.0) { throw 'ASan build requires more than 4 GiB free RAM' }
& $sectorWrapper build -Preset equity-asan -Jobs $Jobs atx-engine-risk-sector-asan
if ($LASTEXITCODE -ne 0) { throw 'ASan scoped target build failed' }
$sectorExe = Join-Path $sectorBuild 'bin\atx-engine-risk-sector-asan.exe'
Write-Output ('ASan binary SHA-256: ' + (Get-FileHash -LiteralPath $sectorExe).Hash)
$sectorSymbolizerLine = $sectorCache | Where-Object {
    $_.StartsWith('ATX_RISK_SECTOR_ASAN_SYMBOLIZER:FILEPATH=')
} | Select-Object -First 1
if (-not $sectorSymbolizerLine) { throw 'ASan symbolizer was not recorded by configure' }
$sectorOldOptions = $env:ASAN_OPTIONS
$sectorOldSymbolizer = $env:ASAN_SYMBOLIZER_PATH
$sectorOldControl = $env:ATX_ASAN_FORCE_PRE_W0_OOB
try {
    $env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1:abort_on_error=0'
    $env:ASAN_SYMBOLIZER_PATH = $sectorSymbolizerLine.Split('=', 2)[1]
    $env:ATX_ASAN_FORCE_PRE_W0_OOB = '1'
    $sectorPositiveErr = Join-Path $sectorBuild 'sector-asan-positive.stderr.log'
    $sectorPositive = Start-Process -FilePath $sectorExe -WorkingDirectory $sectorRepo `
        -ArgumentList '--gtest_filter=RiskSectorColumnsById.CheckedIndexingCatchesThePreW0Read' `
        -RedirectStandardOutput (Join-Path $sectorBuild 'sector-asan-positive.stdout.log') `
        -RedirectStandardError $sectorPositiveErr -WindowStyle Hidden -Wait -PassThru
    if ($sectorPositive.ExitCode -eq 0 -or
        -not (Select-String -LiteralPath $sectorPositiveErr `
            -Pattern 'AddressSanitizer: heap-buffer-overflow' -Quiet)) {
        throw 'Positive control did not prove ASan detection of the pre-W0 out-of-bounds read'
    }
    Write-Output ('ASan positive control detected heap-buffer-overflow; exit=' +
        $sectorPositive.ExitCode)
    Remove-Item Env:\ATX_ASAN_FORCE_PRE_W0_OOB
    $sectorSuiteOut = Join-Path $sectorBuild 'sector-asan-suite.stdout.log'
    $sectorSuite = Start-Process -FilePath $sectorExe -WorkingDirectory $sectorRepo `
        -ArgumentList '--gtest_filter=RiskSectorColumnsById.*', '--gtest_brief=1' `
        -RedirectStandardOutput $sectorSuiteOut `
        -RedirectStandardError (Join-Path $sectorBuild 'sector-asan-suite.stderr.log') `
        -WindowStyle Hidden -Wait -PassThru
    Get-Content -LiteralPath $sectorSuiteOut
    if ($sectorSuite.ExitCode -ne 0) { throw 'Instrumented sector-column fixtures failed' }
    Write-Output 'PASS: native ASan production fixture and exact pre-W0 negative control.'
    Write-Output 'Scope: factor_model.cpp plus fixture/inline helpers; dependencies uninstrumented.'
    Write-Output 'STL logical-size annotations are disabled to match prebuilt dependency ABI.'
} finally {
    $env:ASAN_OPTIONS = $sectorOldOptions
    $env:ASAN_SYMBOLIZER_PATH = $sectorOldSymbolizer
    $env:ATX_ASAN_FORCE_PRE_W0_OOB = $sectorOldControl
}
