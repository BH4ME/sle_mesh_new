param(
    [string[]]$Ports = @("COM16", "COM13", "COM17", "COM18"),
    [string]$Firmware = "",
    [string]$Python = "",
    [string]$ExpectedVersion = "v4.5.56-minimal",
    [switch]$Parallel,
    [string]$LogRoot = "",
    [int]$Baudrate = 115200,
    [double]$WaitTimeout = 15.0,
    [double]$ManualRetryTimeout = 0.0,
    [int]$ParallelStartDelayMs = 500,
    [string]$ResetCommand = "reboot",
    [string]$ResetCommandFallback = "reset",
    [string]$CompatResetCommand = "AT+RST",
    [switch]$HardwareReset,
    [string]$ControlSequence = "rts=1:0.25;rts=0:0.5",
    [int]$YmodemPacketSize = 1024,
    [int]$YmodemTransferRetries = 1,
    [ValidateSet("blocking", "nonblocking-drain")]
    [string]$SerialWriteMode = "nonblocking-drain",
    [double]$SerialWriteDrainTimeout = 3.0,
    [double]$SerialWritePostGap = 0.0,
    [int]$SerialWriteChunkSize = 0,
    [double]$SerialWriteGap = 0.0,
    [int]$FlashAttempts = 2,
    [switch]$NoRomPreflight,
    [double]$RomPreflightTimeout = 1.0,
    [switch]$AllowPartialSuccess,
    [int]$MinSuccessfulPorts = 1
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
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

$burnTool = Join-Path $repoRoot "automation\ws63\tools\ws63_auto_burn.py"
$firmwarePath = Resolve-Path $Firmware
$pythonPath = Resolve-Path $Python
$toolPath = Resolve-Path $burnTool

if ($Ports.Count -eq 1 -and $Ports[0].Contains(",")) {
    $Ports = @($Ports[0].Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
}

if ($Ports.Count -eq 0) {
    throw "No serial ports were provided."
}
if ($FlashAttempts -lt 1) {
    throw "FlashAttempts must be >= 1."
}
if ($RomPreflightTimeout -le 0.0) {
    throw "RomPreflightTimeout must be > 0."
}
if ($MinSuccessfulPorts -lt 1) {
    throw "MinSuccessfulPorts must be >= 1."
}

$romPreflightEnabled = -not $NoRomPreflight.IsPresent

if ($ExpectedVersion -ne "") {
    $fwText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($firmwarePath))
    if (-not $fwText.Contains($ExpectedVersion)) {
        throw "Firmware package does not contain expected version ${ExpectedVersion}: $firmwarePath"
    }
}

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$runDir = Join-Path $LogRoot "$ExpectedVersion`_$timestamp"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$romPreflightSummary = if ($romPreflightEnabled) { "all ports ($RomPreflightTimeout s)" } else { "false" }

Write-Host "WS63 multi-port flash"
Write-Host "repo:     $repoRoot"
Write-Host "firmware: $firmwarePath"
Write-Host "expected: $ExpectedVersion"
Write-Host "ports:    $($Ports -join ', ')"
Write-Host "parallel: $($Parallel.IsPresent)"
Write-Host "parallel start delay ms: $ParallelStartDelayMs"
Write-Host "logs:     $runDir"
Write-Host "flow:     $(if ($HardwareReset.IsPresent) { 'ROM preflight + hardware EN/RTS reset + post-package handshake' } else { 'ROM preflight + software reset command ladder + post-package handshake' })"
Write-Host "command:  $(if ($HardwareReset.IsPresent) { '<disabled>' } else { $ResetCommand })"
Write-Host "fallback: $(if ($HardwareReset.IsPresent) { '<disabled>' } else { $ResetCommandFallback })"
Write-Host "compat:   $(if ($HardwareReset.IsPresent) { '<disabled>' } else { $CompatResetCommand })"
Write-Host "control:  $(if ($HardwareReset.IsPresent) { $ControlSequence } else { '<disabled>' })"
Write-Host "ymodem packet size: $YmodemPacketSize"
Write-Host "ymodem transfer retries: $YmodemTransferRetries"
Write-Host "serial write mode: $SerialWriteMode"
Write-Host "serial write drain timeout s: $SerialWriteDrainTimeout"
Write-Host "serial write post gap s: $SerialWritePostGap"
Write-Host "serial write chunk size: $SerialWriteChunkSize"
Write-Host "serial write gap s: $SerialWriteGap"
Write-Host "flash attempts: $FlashAttempts"
Write-Host "skip reset if ROM active: $romPreflightSummary"
Write-Host "allow partial success: $($AllowPartialSuccess.IsPresent)"
Write-Host "min successful ports: $MinSuccessfulPorts"
Write-Host ""

function Add-BurnArgsBeforeFirmware {
    param(
        [object[]]$BurnArgs,
        [object[]]$ExtraArgs
    )

    if ($ExtraArgs.Count -eq 0) {
        return $BurnArgs
    }
    return $BurnArgs[0..($BurnArgs.Count - 2)] + $ExtraArgs + $BurnArgs[($BurnArgs.Count - 1)]
}

function New-BurnArgs {
    param([string]$PortName)

    $args = @(
        $toolPath,
        "-p", $PortName,
        "-b", "$Baudrate",
        "--wait-timeout", "$WaitTimeout",
        "--manual-retry-timeout", "$ManualRetryTimeout",
        "--ymodem-packet-size", "$YmodemPacketSize",
        "--ymodem-transfer-retries", "$YmodemTransferRetries",
        "--serial-write-mode", "$SerialWriteMode",
        "--serial-write-drain-timeout", "$SerialWriteDrainTimeout",
        "--serial-write-post-gap", "$SerialWritePostGap",
        "--flash-attempts", "$FlashAttempts",
        "--expected-version", $ExpectedVersion,
        $firmwarePath
    )

    if ($HardwareReset.IsPresent) {
        $args = @(
            $toolPath,
            "-p", $PortName,
            "-b", "$Baudrate",
            "--no-reset-command",
            "--no-reset-command-fallback",
            "--no-compat-reset-command",
            "--control-sequence", $ControlSequence,
            "--wait-timeout", "$WaitTimeout",
            "--manual-retry-timeout", "$ManualRetryTimeout",
            "--ymodem-packet-size", "$YmodemPacketSize",
            "--ymodem-transfer-retries", "$YmodemTransferRetries",
            "--serial-write-mode", "$SerialWriteMode",
            "--serial-write-drain-timeout", "$SerialWriteDrainTimeout",
            "--serial-write-post-gap", "$SerialWritePostGap",
            "--flash-attempts", "$FlashAttempts",
            "--expected-version", $ExpectedVersion,
            $firmwarePath
        )
    } else {
        $args = @(
            $toolPath,
            "-p", $PortName,
            "-b", "$Baudrate",
            "--software-reset-only",
            "--reset-command", $ResetCommand,
            "--reset-command-delay", "0.08",
            "--reset-command-retries", "1",
            "--reset-command-retry-gap", "0",
            "--idle-rts", "0",
            "--idle-dtr", "0",
            "--no-assert-control-after-open",
            "--wait-timeout", "$WaitTimeout",
            "--manual-retry-timeout", "$ManualRetryTimeout",
            "--ymodem-packet-size", "$YmodemPacketSize",
            "--ymodem-transfer-retries", "$YmodemTransferRetries",
            "--serial-write-mode", "$SerialWriteMode",
            "--serial-write-drain-timeout", "$SerialWriteDrainTimeout",
            "--serial-write-post-gap", "$SerialWritePostGap",
            "--flash-attempts", "$FlashAttempts",
            "--expected-version", $ExpectedVersion,
            $firmwarePath
        )
        if ($ResetCommandFallback -ne "") {
            $args = Add-BurnArgsBeforeFirmware -BurnArgs $args -ExtraArgs @(
                "--reset-command-fallback", $ResetCommandFallback
            )
        } else {
            $args = Add-BurnArgsBeforeFirmware -BurnArgs $args -ExtraArgs @(
                "--no-reset-command-fallback"
            )
        }
        if ($CompatResetCommand -ne "") {
            $args = Add-BurnArgsBeforeFirmware -BurnArgs $args -ExtraArgs @(
                "--compat-reset-command", $CompatResetCommand
            )
        } else {
            $args = Add-BurnArgsBeforeFirmware -BurnArgs $args -ExtraArgs @(
                "--no-compat-reset-command"
            )
        }
    }

    if ($romPreflightEnabled) {
        $args = Add-BurnArgsBeforeFirmware -BurnArgs $args -ExtraArgs @(
            "--skip-reset-if-rom-active",
            "--rom-preflight-timeout", "$RomPreflightTimeout"
        )
    }

    if ($SerialWriteChunkSize -gt 0) {
        $args = Add-BurnArgsBeforeFirmware -BurnArgs $args -ExtraArgs @(
            "--serial-write-chunk-size", "$SerialWriteChunkSize",
            "--serial-write-gap", "$SerialWriteGap"
        )
    }

    return $args
}

function Save-RunSummary {
    $summary = @(
        "WS63 multi-port flash",
        "timestamp: $timestamp",
        "repo: $repoRoot",
        "firmware: $firmwarePath",
        "expected: $ExpectedVersion",
        "ports: $($Ports -join ', ')",
        "parallel: $($Parallel.IsPresent)",
        "parallel_start_delay_ms: $ParallelStartDelayMs",
        "baudrate: $Baudrate",
        "wait_timeout_s: $WaitTimeout",
        "manual_retry_timeout_s: $ManualRetryTimeout",
        "reset_command: $(if ($HardwareReset.IsPresent) { '<disabled>' } else { $ResetCommand })",
        "reset_command_fallback: $(if ($HardwareReset.IsPresent) { '<disabled>' } else { $ResetCommandFallback })",
        "compat_reset_command: $(if ($HardwareReset.IsPresent) { '<disabled>' } else { $CompatResetCommand })",
        "hardware_reset: $($HardwareReset.IsPresent)",
        "control_sequence: $(if ($HardwareReset.IsPresent) { $ControlSequence } else { '<disabled>' })",
        "ymodem_packet_size: $YmodemPacketSize",
        "ymodem_transfer_retries: $YmodemTransferRetries",
        "serial_write_mode: $SerialWriteMode",
        "serial_write_drain_timeout_s: $SerialWriteDrainTimeout",
        "serial_write_post_gap_s: $SerialWritePostGap",
        "serial_write_chunk_size: $SerialWriteChunkSize",
        "serial_write_gap_s: $SerialWriteGap",
        "flash_attempts: $FlashAttempts",
        "skip_reset_if_rom_active: $romPreflightSummary",
        "allow_partial_success: $($AllowPartialSuccess.IsPresent)",
        "min_successful_ports: $MinSuccessfulPorts",
        "flow: $(if ($HardwareReset.IsPresent) { 'ROM preflight + hardware EN/RTS reset + post-package handshake' } else { 'ROM preflight + software reset command ladder + post-package handshake' })",
        "",
        "reset_policy:",
        "  Default automated runs first probe every port for an already-active WS63 ROM handshake.",
        "  Ports already in ROM skip reset and go straight to package download; this covers blank/unflashed boards that boot to ROM.",
        "  Software-reset ports then try the CLI command ladder: reboot, reset, AT+RST.",
        "  Use -HardwareReset only for boards wired for reliable EN/RTS reset.",
        "  Use -AllowPartialSuccess with -MinSuccessfulPorts when a batch intentionally includes absent/bad boards.",
        "  SerialWriteChunkSize is a diagnostic fallback.",
        "  Success evidence is:",
        "  - Establishing ymodem session",
        "  - Done. Reseting device...",
        "",
        "logs:",
        "  one file per port: port-<PORT>.log",
        "  one command per port: port-<PORT>.command.txt"
    )
    Set-Content -Path (Join-Path $runDir "run_summary.txt") -Value $summary -Encoding ASCII
}

function Get-PortLogStem {
    param([string]$PortName)

    $safeName = $PortName -replace '[\\/:*?"<>|]', '_'
    return "port-$safeName"
}

function Save-PortCommand {
    param(
        [string]$PortName,
        [object[]]$BurnArgs
    )

    $commandText = @(
        "`"$pythonPath`" " + (($BurnArgs | ForEach-Object { "`"$_`"" }) -join " "),
        "",
        "reset_policy:",
        "  This command is guarded by ExpectedVersion.",
        "  Every port probes ROM before reset unless -NoRomPreflight is set.",
        "  Software reset uses the reboot/reset/AT+RST command ladder for mixed firmware states.",
        "  SerialWriteMode nonblocking-drain keeps 1024-byte YMODEM packets and drains the Windows TX queue.",
        "  ROM preflight skips software reboot when ROM is already ready.",
        "  Retries restart the full flash session, not only the current YMODEM file.",
        "  SerialWritePostGap adds a delay after complete large writes; it does not split packets.",
        "  Serial write chunking is only enabled when SerialWriteChunkSize > 0."
    )
    Set-Content -Path (Join-Path $runDir "$(Get-PortLogStem $PortName).command.txt") -Value $commandText -Encoding ASCII
}

Save-RunSummary

function Invoke-OneBurn {
    param([string]$PortName)

    $log = Join-Path $runDir "$(Get-PortLogStem $PortName).log"
    $args = New-BurnArgs -PortName $PortName
    Save-PortCommand -PortName $PortName -BurnArgs $args
    Write-Host "[$PortName] start, log=$log"
    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $pythonPath @args 2>&1 | ForEach-Object {
            $line = "$_"
            Add-Content -Path $log -Value $line
            Write-Host "[$PortName] $line"
        }
        $exitCode = $LASTEXITCODE
    } catch {
        $line = "burn wrapper caught error: $($_.Exception.Message)"
        Add-Content -Path $log -Value $line
        Write-Host "[$PortName] $line"
        $exitCode = 98
    } finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }
    if ($null -eq $exitCode) {
        $exitCode = 99
    }
    Write-Host "[$PortName] exit=$exitCode"
    return [pscustomobject]@{ Port = $PortName; ExitCode = $exitCode; Log = $log }
}

$results = @()
if (-not $Parallel.IsPresent) {
    foreach ($port in $Ports) {
        $results += Invoke-OneBurn -PortName $port
    }
} else {
    Write-Host "Parallel mode: software reset only; no manual reset expected."
    $jobs = @()
    foreach ($port in $Ports) {
        $log = Join-Path $runDir "$(Get-PortLogStem $port).log"
        $args = New-BurnArgs -PortName $port
        Save-PortCommand -PortName $port -BurnArgs $args
        $jobs += Start-Job -Name "flash_$port" -ArgumentList $pythonPath, $args, $log, $port -ScriptBlock {
            param($PythonPath, $BurnArgs, $LogPath, $PortName)
            $ErrorActionPreference = "Continue"
            & $PythonPath @BurnArgs 2>&1 | ForEach-Object {
                $line = "$_"
                Add-Content -Path $LogPath -Value $line
                Write-Output $line
            }
            [pscustomobject]@{
                Port = $PortName
                ExitCode = $LASTEXITCODE
                Log = $LogPath
                __BurnResult = $true
            }
        }
        Write-Host "[$port] job started, log=$log"
        if ($ParallelStartDelayMs -gt 0) {
            Start-Sleep -Milliseconds $ParallelStartDelayMs
        }
    }

    $resultByPort = @{}
    do {
        foreach ($job in $jobs) {
            Receive-Job -Job $job | ForEach-Object {
                if ($_.PSObject.Properties.Name -contains "__BurnResult") {
                    $resultByPort[$_.Port] = $_
                } else {
                    $portName = $job.Name -replace "^flash_", ""
                    Write-Host "[$portName] $_"
                }
            }
        }
        Start-Sleep -Milliseconds 500
    } while (@($jobs | Where-Object { $_.State -eq "Running" }).Count -gt 0)

    foreach ($job in $jobs) {
        Receive-Job -Job $job | ForEach-Object {
            if ($_.PSObject.Properties.Name -contains "__BurnResult") {
                $resultByPort[$_.Port] = $_
            } else {
                $portName = $job.Name -replace "^flash_", ""
                Write-Host "[$portName] $_"
            }
        }
        Remove-Job -Job $job
    }
    foreach ($port in $Ports) {
        if ($resultByPort.ContainsKey($port)) {
            $results += $resultByPort[$port]
        } else {
            $results += [pscustomobject]@{ Port = $port; ExitCode = 99; Log = Join-Path $runDir "$(Get-PortLogStem $port).log" }
        }
    }
}

Write-Host ""
Write-Host "Flash summary:"
Add-Content -Path (Join-Path $runDir "run_summary.txt") -Value ""
Add-Content -Path (Join-Path $runDir "run_summary.txt") -Value "results:"
$failed = 0
foreach ($result in $results) {
    $summaryLine = "{0}: exit={1} log={2}" -f $result.Port, $result.ExitCode, $result.Log
    Write-Host $summaryLine
    Add-Content -Path (Join-Path $runDir "run_summary.txt") -Value "  $summaryLine"
    if ([int]$result.ExitCode -ne 0) {
        $failed++
    }
}

if ($failed -ne 0) {
    $success = $results.Count - $failed
    Add-Content -Path (Join-Path $runDir "run_summary.txt") -Value "result: FAIL failed=$failed success=$success"
    if ($AllowPartialSuccess.IsPresent -and $success -ge $MinSuccessfulPorts) {
        Add-Content -Path (Join-Path $runDir "run_summary.txt") -Value "partial_success: accepted min_successful_ports=$MinSuccessfulPorts"
        Write-Warning "$failed flash job(s) failed, but $success port(s) succeeded and -AllowPartialSuccess accepted the run. Check logs under $runDir"
        exit 0
    }
    throw "$failed flash job(s) failed. Check logs under $runDir"
}

Add-Content -Path (Join-Path $runDir "run_summary.txt") -Value "result: PASS"
Write-Host "All flash jobs passed."
