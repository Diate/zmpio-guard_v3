# Dual-UART capture for board bring-up. Logs land next to this script unless
# -OutDir says otherwise.
param(
    [string]$OutDir = $PSScriptRoot,
    [int]$DurationSeconds = 5400
)

$outDir = $OutDir
$ports = @{ 'COM3' = "$outDir\com3_capture.log"; 'COM4' = "$outDir\com4_capture.log" }
$handles = @{}
$lastOk = @{}

function Open-Port($name) {
    try {
        $p = New-Object System.IO.Ports.SerialPort $name,115200,None,8,One
        $p.ReadTimeout = 500
        $p.Open()
        return $p
    } catch {
        return $null
    }
}

foreach ($name in $ports.Keys) {
    if (Test-Path -Path $ports[$name]) {
        Remove-Item -Path $ports[$name] -Force
        Write-Host "Removed old log: $($ports[$name])"
    }
    $handles[$name] = Open-Port $name
    $lastOk[$name] = Get-Date
}
Write-Host "Capturing $($handles.Keys -join ', ') for $DurationSeconds s (auto-reconnect on error, e.g. board power-cycle)..."
$deadline = (Get-Date).AddSeconds($DurationSeconds)
while ((Get-Date) -lt $deadline) {
    foreach ($name in @($handles.Keys)) {
        $p = $handles[$name]
        if ($null -eq $p -or -not $p.IsOpen) {
            # Reopen attempt, at most once per 3s to avoid spamming a still-missing device.
            if (((Get-Date) - $lastOk[$name]).TotalSeconds -ge 3) {
                $handles[$name] = Open-Port $name
                $lastOk[$name] = Get-Date
            }
            continue
        }
        try {
            $data = $p.ReadExisting()
            if ($data) {
                $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
                Add-Content -Path $ports[$name] -Value "[$ts] $data" -NoNewline
            }
        } catch {
            try { $p.Close() } catch {}
            $handles[$name] = $null
        }
    }
    Start-Sleep -Milliseconds 200
}
foreach ($p in $handles.Values) {
    if ($p -and $p.IsOpen) { $p.Close() }
}
Write-Host "Capture done."
