#Requires -Modules Pester
<#
.SYNOPSIS
  Pester argv tests for scripts/build-tradeable-alphas.ps1 (S7-5).

  Strategy: dot-source the script to load the five argv functions without executing the
  pipeline body (the guard `if ($MyInvocation.InvocationName -ne '.')` prevents it).
  All assertions are pure-function checks - no binary, no panel, no filesystem writes.

  Target Pester version: 3.4.0 (Windows built-in).
  Syntax used: Describe / It / Should - compatible with Pester 3.x.
  AVOIDS v5-only features: -ForEach, BeforeDiscovery.

  NOTE on Pester 3 array assertions:
    `Should Contain` in Pester 3 is a FILE-content assertion, NOT array membership.
    Array membership is tested with `($arr -contains 'x') | Should Be $true`.
#>

$scriptPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'build-tradeable-alphas.ps1'

# Dot-source to import functions; the guard must prevent the pipeline body from running.
. $scriptPath

# Fixed test inputs - isolated from any on-disk state
$testWorkDir  = 'C:\atx-test\work'
$testPanel    = 'C:\atx-test\panel.bin'
$testSegsDir  = 'C:\atx-test\segs'
$testSeedFile = 'C:\atx-test\alpha101.txt'

Describe 'New-AugmentArgv' {

    $argv = New-AugmentArgv -SegsDir $testSegsDir -WorkDir $testWorkDir

    It 'has panel as first element (subcommand)' {
        $argv[0] | Should Be 'panel'
    }

    It 'contains --augment-panel flag' {
        ($argv -contains '--augment-panel') | Should Be $true
    }

    It 'contains --adv-windows flag' {
        ($argv -contains '--adv-windows') | Should Be $true
    }

    It 'contains adv-windows value 5,10,20,60 as a single token' {
        ($argv -contains '5,10,20,60') | Should Be $true
    }

    It 'contains --segs flag' {
        ($argv -contains '--segs') | Should Be $true
    }

    It 'contains --panel-out flag' {
        ($argv -contains '--panel-out') | Should Be $true
    }
}

Describe 'New-DiscoverArgv - core flags present' {

    $argv = New-DiscoverArgv -DownstreamPanel $testPanel -SeedFile $testSeedFile `
                             -WorkDir $testWorkDir -Workers 0

    It 'has discover as first element (subcommand)' {
        $argv[0] | Should Be 'discover'
    }

    It 'contains --gated flag' {
        ($argv -contains '--gated') | Should Be $true
    }

    It 'contains --seed-file flag' {
        ($argv -contains '--seed-file') | Should Be $true
    }

    It 'contains the seed file path' {
        ($argv -contains $testSeedFile) | Should Be $true
    }

    It 'contains --cost-bps-admit flag' {
        ($argv -contains '--cost-bps-admit') | Should Be $true
    }

    It 'contains cost-bps-admit value 10' {
        ($argv -contains '10') | Should Be $true
    }

    It 'contains --protect-seed-elites flag' {
        ($argv -contains '--protect-seed-elites') | Should Be $true
    }

    It 'contains --mutate-seed-copies flag' {
        ($argv -contains '--mutate-seed-copies') | Should Be $true
    }

    It 'contains --deflate-selection flag' {
        ($argv -contains '--deflate-selection') | Should Be $true
    }

    It 'contains --enable-wrap-in-op flag' {
        ($argv -contains '--enable-wrap-in-op') | Should Be $true
    }

    It 'contains --turnover-penalty-slope flag' {
        ($argv -contains '--turnover-penalty-slope') | Should Be $true
    }

    It 'contains --min-holding-days flag' {
        ($argv -contains '--min-holding-days') | Should Be $true
    }

    It 'contains --typed-fields flag' {
        ($argv -contains '--typed-fields') | Should Be $true
    }
}

Describe 'New-DiscoverArgv - regression guard: removed flag absent' {

    $argv = New-DiscoverArgv -DownstreamPanel $testPanel -SeedFile $testSeedFile `
                             -WorkDir $testWorkDir -Workers 0

    It 'does NOT contain --admit-seeds-presearch (flag was removed / never existed)' {
        ($argv -contains '--admit-seeds-presearch') | Should Be $false
    }

    It 'uses --alpha-out for the discover output dir (discover reads cfg.alpha_out, not cfg.out)' {
        ($argv -contains '--alpha-out') | Should Be $true
    }

    It 'does NOT use bare --out for discover (that sets cfg.out, which discover ignores)' {
        ($argv -contains '--out') | Should Be $false
    }
}

Describe 'New-DiscoverArgv - --workers conditional' {

    It '--workers is ABSENT when Workers=0' {
        $argv = New-DiscoverArgv -DownstreamPanel $testPanel -SeedFile $testSeedFile `
                                 -WorkDir $testWorkDir -Workers 0
        ($argv -contains '--workers') | Should Be $false
    }

    It '--workers IS PRESENT when Workers=5' {
        $argv = New-DiscoverArgv -DownstreamPanel $testPanel -SeedFile $testSeedFile `
                                 -WorkDir $testWorkDir -Workers 5
        ($argv -contains '--workers') | Should Be $true
    }

    It '--workers value is 5 when Workers=5' {
        $argv = New-DiscoverArgv -DownstreamPanel $testPanel -SeedFile $testSeedFile `
                                 -WorkDir $testWorkDir -Workers 5
        ($argv -contains '5') | Should Be $true
    }
}

Describe 'New-CombineArgv' {

    $argv = New-CombineArgv -DownstreamPanel $testPanel -WorkDir $testWorkDir

    It 'has combine as first element (subcommand)' {
        $argv[0] | Should Be 'combine'
    }

    It 'contains --panel flag' {
        ($argv -contains '--panel') | Should Be $true
    }

    It 'contains --library-dir flag' {
        ($argv -contains '--library-dir') | Should Be $true
    }

    It 'contains --combo-out flag' {
        ($argv -contains '--combo-out') | Should Be $true
    }

    It 'contains --holdout-frac flag' {
        ($argv -contains '--holdout-frac') | Should Be $true
    }
}

Describe 'New-OptimizeArgv' {

    $argv = New-OptimizeArgv -DownstreamPanel $testPanel -WorkDir $testWorkDir -CostBps 10

    It 'has optimize as first element (subcommand)' {
        $argv[0] | Should Be 'optimize'
    }

    It 'contains --position-mode flag (valueless boolean)' {
        ($argv -contains '--position-mode') | Should Be $true
    }

    It 'contains --cost-bps flag' {
        ($argv -contains '--cost-bps') | Should Be $true
    }

    It 'contains --panel flag' {
        ($argv -contains '--panel') | Should Be $true
    }

    It 'contains --combo flag' {
        ($argv -contains '--combo') | Should Be $true
    }

    It 'contains --books-out flag' {
        ($argv -contains '--books-out') | Should Be $true
    }
}

Describe 'New-ReportArgv' {

    $argv = New-ReportArgv -DownstreamPanel $testPanel -WorkDir $testWorkDir

    It 'has report as first element (subcommand)' {
        $argv[0] | Should Be 'report'
    }

    It 'contains --panel flag' {
        ($argv -contains '--panel') | Should Be $true
    }

    It 'contains --books flag' {
        ($argv -contains '--books') | Should Be $true
    }

    It 'contains --combo flag' {
        ($argv -contains '--combo') | Should Be $true
    }

    It 'contains --report-out flag' {
        ($argv -contains '--report-out') | Should Be $true
    }
}
