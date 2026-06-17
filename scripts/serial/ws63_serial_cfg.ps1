param(
    [Parameter(Mandatory = $true)]
    [string]$Port,

    [ValidateSet("leader", "member", "status", "apply", "clear", "reboot")]
    [string]$Mode = "status",

    [ValidateRange(1, 254)]
    [int]$Team = 1,

    [ValidateRange(0, 255)]
    [int]$Channel = 17,

    [string]$LeaderSuffix = "",

    [int]$Baud = 115200,

    [int]$ReadMs = 800,

    [switch]$UseControlLines
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function New-SerialPort {
    param(
        [string]$Name,
        [int]$Rate,
        [int]$TimeoutMs,
        [switch]$UseControlLines
    )
    Add-Type -AssemblyName System | Out-Null
    $sp = New-Object System.IO.Ports.SerialPort($Name, $Rate, "None", 8, "One")
    $sp.NewLine = "`r`n"
    $sp.ReadTimeout = $TimeoutMs
    $sp.WriteTimeout = $TimeoutMs
    $sp.DtrEnable = $UseControlLines.IsPresent
    $sp.RtsEnable = $UseControlLines.IsPresent
    return $sp
}

function Normalize-Suffix {
    param([string]$Hex)
    if ([string]::IsNullOrWhiteSpace($Hex)) {
        throw "LeaderSuffix is required in member mode, example: 9A2F"
    }
    $v = $Hex.Trim().ToUpperInvariant()
    if ($v.StartsWith("0X")) {
        $v = $v.Substring(2)
    }
    if ($v.Length -lt 1 -or $v.Length -gt 4 -or $v -notmatch '^[0-9A-F]{1,4}$') {
        throw "LeaderSuffix must be 1..4 hex chars, example: 9A2F"
    }
    return $v
}

function Build-Command {
    param(
        [string]$CfgMode,
        [int]$CfgTeam,
        [int]$CfgChannel,
        [string]$CfgLeaderSuffix
    )
    switch ($CfgMode) {
        "leader" { return "cfg leader now $CfgTeam $CfgChannel" }
        "member" {
            $suffix = Normalize-Suffix -Hex $CfgLeaderSuffix
            return "cfg member now $suffix $CfgTeam $CfgChannel"
        }
        "status" { return "cfg status" }
        "apply" { return "cfg apply" }
        "clear" { return "cfg clear" }
        "reboot" { return "cfg reboot" }
        default { throw "Unsupported mode: $CfgMode" }
    }
}

function Read-SerialBurst {
    param(
        [System.IO.Ports.SerialPort]$SerialPort,
        [int]$TimeoutMs
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $sb = New-Object System.Text.StringBuilder
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $line = $SerialPort.ReadLine()
            if (-not [string]::IsNullOrWhiteSpace($line)) {
                [void]$sb.AppendLine($line)
            }
        } catch [System.TimeoutException] {
            break
        }
    }
    return $sb.ToString()
}

$cmd = Build-Command -CfgMode $Mode -CfgTeam $Team -CfgChannel $Channel -CfgLeaderSuffix $LeaderSuffix
Write-Host "[serial-cfg] port=$Port baud=$Baud cmd=$cmd"

$serialPort = New-SerialPort -Name $Port -Rate $Baud -TimeoutMs $ReadMs -UseControlLines:$UseControlLines.IsPresent
try {
    $serialPort.Open()
    Start-Sleep -Milliseconds 120
    $serialPort.DiscardInBuffer()
    $serialPort.DiscardOutBuffer()
    $serialPort.WriteLine($cmd)
    $reply = Read-SerialBurst -SerialPort $serialPort -TimeoutMs $ReadMs
    if (-not [string]::IsNullOrWhiteSpace($reply)) {
        Write-Output $reply.TrimEnd()
    } else {
        Write-Host "[serial-cfg] no reply in ${ReadMs}ms"
    }
}
finally {
    if ($serialPort -and $serialPort.IsOpen) {
        $serialPort.Close()
    }
}
