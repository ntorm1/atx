<#
.SYNOPSIS
  Capstone harness — drives the full atx-impl pipeline:
    augment panel  →  cost-aware gated discover  →  combine  →  sign-correct optimize  →  report

  Ties together the S3/S4/S5/S6 knobs threaded by Sprint-7 tasks S7-1..S7-4.

.DESCRIPTION
  This script composes and invokes five atx-impl stages.  Each stage's argv is produced by a
  dedicated testable function (New-AugmentArgv, New-DiscoverArgv, New-CombineArgv,
  New-OptimizeArgv, New-ReportArgv) so Pester can dot-source the script and validate argv
  without executing the binary.

  -Stage values:
    augment    – panel augmentation only
    discover   – gated discovery only
    pipeline   – combine + optimize + report only
    all        – augment + discover + pipeline (default)

.EXAMPLE
  # DryRun: print all five stage argv without invoking the binary
  pwsh -NoProfile -File scripts\build-tradeable-alphas.ps1 -DryRun -Stage all

.EXAMPLE
  # Staged mode: S7-6 calls this twice to dodge the 10-min discover timeout
  pwsh -NoProfile -File scripts\build-tradeable-alphas.ps1 -Stage augment,discover
  pwsh -NoProfile -File scripts\build-tradeable-alphas.ps1 -Stage pipeline

.NOTES
  S7-5: capstone harness.  S7-6 runs the real pipeline.
  Flag names verified against atx-impl/src/config.cpp.
  --admit-seeds-presearch does NOT exist; corrected to --protect-seed-elites / --mutate-seed-copies.
  discover does NOT write a panel (--panel-out is a no-op); downstream stages consume the augmented panel.
#>
param(
    [ValidateSet('augment','discover','pipeline','all')]
    [string[]] $Stage     = @('all'),
    [switch]   $DryRun,
    [string]   $WorkDir   = 'work\tradeable-build',
    [string]   $PanelBin  = 'work\accept\panel.bin',
    [string]   $SegsDir   = 'work\accept\segs',
    [string]   $SeedFile  = 'atx-impl\tests\fixtures\alpha101.txt',
    [int]      $Workers   = 0,          # 0 = auto: OMIT --workers; >0 emits --workers N
    [string]   $AtxExe    = 'C:\atx\build\bin\atx-impl.exe',
    [double]   $CostBps   = 10
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Testable argv-composition functions
# Each returns [string[]] with subcommand first, no exe path.
# ---------------------------------------------------------------------------

function New-AugmentArgv {
    param(
        [string] $SegsDir,
        [string] $WorkDir
    )
    $augPanelOut = Join-Path $WorkDir 'aug-panel.bin'
    [string[]]@(
        'panel',
        '--segs',           $SegsDir,
        '--augment-panel',
        '--adv-windows',    '5,10,20,60',
        '--panel-out',      $augPanelOut
    )
}

function New-DiscoverArgv {
    param(
        [string] $DownstreamPanel,
        [string] $SeedFile,
        [string] $WorkDir,
        [int]    $Workers = 0
    )
    $libraryDir = Join-Path $WorkDir '_library'
    $outDir     = Join-Path $WorkDir 'alphas'

    $argv = [System.Collections.Generic.List[string]]::new()
    $argv.AddRange([string[]]@(
        'discover',
        '--panel',                  $DownstreamPanel,
        '--seed-file',              $SeedFile,
        '--gated',
        '--library-dir',            $libraryDir,
        '--turnover-penalty-slope', '0.1',
        '--max-turnover-target',    '0.25',
        '--protect-seed-elites',
        '--mutate-seed-copies',
        '--deflate-selection',
        '--min-viable-raw',         '0.05',
        '--enable-wrap-in-op',
        '--cost-bps-admit',         '10',
        '--min-holding-days',       '5',
        '--min-dsr',                '0.5',
        '--min-sharpe',             '0.25',
        '--min-fitness',            '1.0',
        '--max-turnover',           '0.50',
        '--reject-price-scale',     '0.5',
        '--dsr-subwindows',         '3',
        '--typed-fields',
        '--robust-holdout-frac',    '0.30',
        '--oos-fraction',           '0.25',
        '--out',                    $outDir,
        '--population',             '300',
        '--generations',            '15'
    ))

    if ($Workers -gt 0) {
        $argv.Add('--workers')
        $argv.Add([string]$Workers)
    }

    [string[]]$argv.ToArray()
}

function New-CombineArgv {
    param(
        [string] $DownstreamPanel,
        [string] $WorkDir
    )
    $libraryDir = Join-Path $WorkDir '_library'
    $comboOut   = Join-Path $WorkDir 'combo.bin'

    [string[]]@(
        'combine',
        '--panel',        $DownstreamPanel,
        '--library-dir',  $libraryDir,
        '--combo-out',    $comboOut,
        '--holdout-frac', '0.25'
    )
}

function New-OptimizeArgv {
    param(
        [string] $DownstreamPanel,
        [string] $WorkDir,
        [double] $CostBps
    )
    $comboIn  = Join-Path $WorkDir 'combo.bin'
    $booksOut = Join-Path $WorkDir 'books.bin'

    [string[]]@(
        'optimize',
        '--panel',         $DownstreamPanel,
        '--combo',         $comboIn,
        '--books-out',     $booksOut,
        '--position-mode',
        '--cost-bps',      [string]$CostBps
    )
}

function New-ReportArgv {
    param(
        [string] $DownstreamPanel,
        [string] $WorkDir
    )
    $comboIn   = Join-Path $WorkDir 'combo.bin'
    $booksIn   = Join-Path $WorkDir 'books.bin'
    $reportOut = Join-Path $WorkDir 'report'

    [string[]]@(
        'report',
        '--panel',      $DownstreamPanel,
        '--books',      $booksIn,
        '--combo',      $comboIn,
        '--report-out', $reportOut
    )
}

# ---------------------------------------------------------------------------
# Ranked table / show helper (defensive; not covered by Pester)
# S7-6 refines per-alpha net-of-cost columns against _library\catalog.sqlite
# ---------------------------------------------------------------------------

function Show-RankedTable {
    param([string] $WorkDir)

    $summary = Join-Path $WorkDir 'report\summary.txt'
    if (Test-Path $summary) {
        Write-Host "`n=== Deployed-book metrics ===" -ForegroundColor Cyan
        $keys = @(
            'portfolio_oos_sharpe','portfolio_is_sharpe','portfolio_sharpe',
            'oos_pnl_net','oos_turnover','max_participation_pct','avg_names_held','report_aum'
        )
        $map = @{}
        Get-Content $summary | ForEach-Object {
            if ($_ -match '^([^=]+)=(.+)$') { $map[$Matches[1].Trim()] = $Matches[2].Trim() }
        }
        foreach ($k in $keys) {
            if ($map.ContainsKey($k)) { Write-Host "  $k = $($map[$k])" }
        }
    } else {
        Write-Host "no report/alphas found at $WorkDir" -ForegroundColor Yellow
    }

    $alphasDir = Join-Path $WorkDir 'alphas'
    $dslFiles  = if (Test-Path $alphasDir) { Get-ChildItem $alphasDir -Filter '*.dsl' } else { @() }
    if ($dslFiles.Count -gt 0) {
        Write-Host "`n=== Admitted alphas ($($dslFiles.Count) .dsl files) ===" -ForegroundColor Cyan
        foreach ($f in $dslFiles) {
            $firstLine = Get-Content $f.FullName -TotalCount 1
            Write-Host "  $($f.Name)  $firstLine"
        }
    } elseif (-not (Test-Path $summary)) {
        # already printed the message above; nothing more to do
    } else {
        Write-Host "  (no .dsl files in $alphasDir)"
    }
}

# ---------------------------------------------------------------------------
# Main execution body — GUARDED: runs only when invoked directly, NOT dot-sourced
# ---------------------------------------------------------------------------

if ($MyInvocation.InvocationName -ne '.') {

    # Resolve absolute WorkDir (make it absolute so downstream stages agree on paths)
    if (-not [System.IO.Path]::IsPathRooted($WorkDir)) {
        $WorkDir = Join-Path (Get-Location).Path $WorkDir
    }

    # Expand 'all' and 'pipeline' meta-stages to concrete stage names
    $expandedStages = @()
    foreach ($s in $Stage) {
        if ($s -eq 'all') {
            # 'all' -> augment + discover + pipeline -> augment + discover + combine + optimize + report
            $expandedStages += @('augment','discover','combine','optimize','report')
        } elseif ($s -eq 'pipeline') {
            $expandedStages += @('combine','optimize','report')
        } else {
            $expandedStages += $s
        }
    }

    # Deduplicate preserving order
    $seen = @{}
    $orderedStages = @()
    foreach ($s in $expandedStages) {
        if (-not $seen.ContainsKey($s)) { $seen[$s] = $true; $orderedStages += $s }
    }

    # Fixed canonical order
    $canonicalOrder = @('augment','discover','combine','optimize','report')
    $activeStages   = $canonicalOrder | Where-Object { $orderedStages -contains $_ }

    # Determine $DownstreamPanel:
    #   if augment is in scope OR aug-panel.bin already exists -> use aug-panel.bin
    #   else -> use $PanelBin as-is
    $augPanelPath = Join-Path $WorkDir 'aug-panel.bin'
    if (($activeStages -contains 'augment') -or (Test-Path $augPanelPath)) {
        $DownstreamPanel = $augPanelPath
    } else {
        $DownstreamPanel = $PanelBin
    }

    # Ensure WorkDir exists (unless DryRun — still create so stages can write artifacts)
    if (-not (Test-Path $WorkDir)) {
        New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
    }

    function Invoke-Stage {
        param([string] $Name, [string[]] $Argv)
        if ($DryRun) {
            Write-Host "=== [DryRun] $Name ===" -ForegroundColor Yellow
            Write-Host "  $AtxExe $($Argv -join ' ')" -ForegroundColor Gray
        } else {
            Write-Host "=== $Name ===" -ForegroundColor Cyan
            & $AtxExe @Argv
            if ($LASTEXITCODE -ne 0) {
                throw "STAGE FAILED: $Name (exit $LASTEXITCODE)"
            }
        }
    }

    foreach ($s in $activeStages) {
        switch ($s) {
            'augment'  {
                $argv = New-AugmentArgv  -SegsDir $SegsDir -WorkDir $WorkDir
                Invoke-Stage 'augment' $argv
            }
            'discover' {
                $argv = New-DiscoverArgv -DownstreamPanel $DownstreamPanel -SeedFile $SeedFile -WorkDir $WorkDir -Workers $Workers
                Invoke-Stage 'discover' $argv
            }
            'combine'  {
                $argv = New-CombineArgv  -DownstreamPanel $DownstreamPanel -WorkDir $WorkDir
                Invoke-Stage 'combine' $argv
            }
            'optimize' {
                $argv = New-OptimizeArgv -DownstreamPanel $DownstreamPanel -WorkDir $WorkDir -CostBps $CostBps
                Invoke-Stage 'optimize' $argv
            }
            'report'   {
                $argv = New-ReportArgv   -DownstreamPanel $DownstreamPanel -WorkDir $WorkDir
                Invoke-Stage 'report' $argv
                if (-not $DryRun) {
                    Show-RankedTable -WorkDir $WorkDir
                }
            }
        }
    }

    if ($DryRun) {
        Write-Host "`nDryRun complete - no binary was invoked." -ForegroundColor Green
    }
}
