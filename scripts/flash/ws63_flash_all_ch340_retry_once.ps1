param(
    [string[]]$Ports = @(),
    [string]$Firmware = "",
    [string]$Python = "",
    [string]$ExpectedVersion = "v4.5.64-minimal",
    [string]$LogRoot = "",
    [int]$Baudrate = 115200,
    [double]$WaitTimeout = 15.0,
    [double]$ManualRetryTimeout = 0.0,
    [int]$ParallelStartDelayMs = 500,
    [string]$ResetCommand = "cfg reboot",
    [switch]$ListOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$multiFlash = Join-Path $repoRoot "scripts\flash\ws63_flash_multi.ps1"
if ($Firmware -eq "") {
    $Firmware = Join-Path $repoRoot "output_from_vm\team_network_v4_unified_runtime_role\ws63-liteos-app_v4_unified_all.fwpkg"
}
if ($Python -eq "") {
    $Python = Join-Path $repoRoot ".tooling\py311\python.exe"
}
if ($LogRoot -eq "") {
    $LogRoot = Join-Path $repoRoot "logs\burn"
}
if (-not [System.IO.Path]::IsPathRooted($LogRoot)) {
    $LogRoot = Join-Path $repoRoot $LogRoot
}
$LogRoot = [System.IO.Path]::GetFullPath($LogRoot)

function Get-ComNumber {
    param([string]$PortName)
    if ($PortName -match '^COM(\d+)$') {
        return [int]$Matches[1]
    }
    return [int]::MaxValue
}

function Get-Ch340Ports {
    $openPorts = @{}
    [System.IO.Ports.SerialPort]::GetPortNames() | ForEach-Object { $openPorts[$_.ToUpperInvariant()] = $true }
    $found = @()
    Get-PnpDevice -Class Ports | ForEach-Object {
        $name = [string]$_.FriendlyName
        $instance = [string]$_.InstanceId
        if (($name -notmatch 'CH340') -and ($instance -notmatch 'VID_1A86&PID_7523')) {
            return
        }
        $match = [regex]::Match($name, '\((COM\d+)\)')
        if (-not $match.Success) {
            return
        }
        $port = $match.Groups[1].Value.ToUpperInvariant()
        if ($port -eq 'COM1') {
            return
        }
        if (-not $openPorts.ContainsKey($port)) {
            return
        }
        $found += $port
    }
    return @($found | Sort-Object -Unique { Get-ComNumber $_ })
}

function Normalize-Ports {
    param([string[]]$RawPorts)
    if ($RawPorts.Count -eq 1 -and $RawPorts[0].Contains(',')) {
        $RawPorts = @($RawPorts[0].Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    }
    $normalized = @($RawPorts | ForEach-Object { $_.Trim().ToUpperInvariant() } | Where-Object { $_ -ne '' } |
        Sort-Object -Unique { Get-ComNumber $_ })
    return $normalized
}

function Get-LatestRunDir {
    param([string]$Root)
    if (-not (Test-Path -LiteralPath $Root)) {
        return $null
    }
    return Get-ChildItem -LiteralPath $Root -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

function Read-FailedPorts {
    param(
        [string]$RunDir,
        [string[]]$AttemptPorts
    )

    $summary = Join-Path $RunDir "run_summary.txt"
    if (-not (Test-Path -LiteralPath $summary)) {
        return $AttemptPorts
    }

    $failed = @()
    $resultCount = 0
    Get-Content -LiteralPath $summary | ForEach-Object {
        if ($_ -match '^\s*(COM\d+):\s+exit=(\d+)\s+') {
            $resultCount++
            if ([int]$Matches[2] -ne 0) {
                $failed += $Matches[1].ToUpperInvariant()
            }
        }
    }
    if ($resultCount -eq 0) {
        return $AttemptPorts
    }
    return @(Normalize-Ports $failed)
}

function Invoke-FlashAttempt {
    param(
        [string]$Name,
        [string[]]$AttemptPorts,
        [string]$AttemptLogRoot
    )

    New-Item -ItemType Directory -Force -Path $AttemptLogRoot | Out-Null
    Write-Host "[$Name] ports: $($AttemptPorts -join ', ')"
    $flashArgs = @{
        Ports = $AttemptPorts
        Firmware = $Firmware
        Python = $Python
        ExpectedVersion = $ExpectedVersion
        Parallel = $true
        LogRoot = $AttemptLogRoot
        Baudrate = $Baudrate
        WaitTimeout = $WaitTimeout
        ManualRetryTimeout = $ManualRetryTimeout
        ParallelStartDelayMs = $ParallelStartDelayMs
        ResetCommand = $ResetCommand
        ResetCommandFallback = ""
        CompatResetCommand = ""
        FlashAttempts = 1
        AllowPartialSuccess = $true
        MinSuccessfulPorts = 1
    }
    try {
        & $multiFlash @flashArgs
        $exitCode = $LASTEXITCODE
    } catch {
        $exitCode = 1
        Write-Warning "[$Name] flash attempt raised: $($_.Exception.Message)"
    }
    $runDir = Get-LatestRunDir -Root $AttemptLogRoot
    if ($null -eq $runDir) {
        throw "${Name}: no flash run directory was created under $AttemptLogRoot"
    }
    $failed = Read-FailedPorts -RunDir $runDir.FullName -AttemptPorts $AttemptPorts
    return [pscustomobject]@{ Name = $Name; ExitCode = $exitCode; RunDir = $runDir.FullName; FailedPorts = $failed }
}

$selectedPorts = @(Normalize-Ports $Ports)
if ($selectedPorts.Count -eq 0) {
    $selectedPorts = @(Get-Ch340Ports)
}
if ($selectedPorts.Count -eq 0) {
    throw "No CH340 COM ports found."
}

Write-Host "WS63 all-CH340 two-round flash"
Write-Host "repo:     $repoRoot"
Write-Host "firmware: $Firmware"
Write-Host "expected: $ExpectedVersion"
Write-Host "ports:    $($selectedPorts -join ', ')"
Write-Host "policy:   all ports parallel, failed set retried once in parallel, software reset only"
Write-Host "reset:    $ResetCommand"
Write-Host ""

if ($ListOnly.IsPresent) {
    exit 0
}

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$runRoot = Join-Path $LogRoot "$ExpectedVersion`_ch340_retry_once_$timestamp"
$attempt1Root = Join-Path $runRoot "attempt1_all"
$attempt1 = Invoke-FlashAttempt -Name "attempt1_all" -AttemptPorts $selectedPorts -AttemptLogRoot $attempt1Root

$finalFailed = @($attempt1.FailedPorts)
$attempt2 = $null
if ($finalFailed.Count -gt 0) {
    Write-Host "[attempt1_all] failed ports: $($finalFailed -join ', ')"
    $attempt2Root = Join-Path $runRoot "attempt2_failed_only"
    $attempt2 = Invoke-FlashAttempt -Name "attempt2_failed_only" -AttemptPorts $finalFailed -AttemptLogRoot $attempt2Root
    $finalFailed = @($attempt2.FailedPorts)
}

$summary = @(
    "WS63 all-CH340 two-round flash",
    "timestamp: $timestamp",
    "firmware: $Firmware",
    "expected: $ExpectedVersion",
    "initial_ports: $($selectedPorts -join ',')",
    "attempt1_run: $($attempt1.RunDir)",
    "attempt1_failed: $($attempt1.FailedPorts -join ',')"
)
if ($null -ne $attempt2) {
    $summary += "attempt2_run: $($attempt2.RunDir)"
    $summary += "attempt2_failed: $($attempt2.FailedPorts -join ',')"
} else {
    $summary += "attempt2_run: <skipped>"
    $summary += "attempt2_failed:"
}
$summary += "final_failed: $($finalFailed -join ',')"
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
Set-Content -Path (Join-Path $runRoot "retry_once_summary.txt") -Value $summary -Encoding ASCII

Write-Host ""
Write-Host "Two-round flash summary:"
$summary | ForEach-Object { Write-Host $_ }

if ($finalFailed.Count -gt 0) {
    throw "Flash failed after retry for: $($finalFailed -join ', ')"
}
Write-Host "All CH340 flash jobs passed after at most one failed-set retry."
