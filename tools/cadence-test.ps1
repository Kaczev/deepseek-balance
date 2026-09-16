# Cadence test: serve a CHANGING balance locally and check that the widget
# shrinks its polling interval after every changed value, down to the floor.
#
# Why a local server: the adaptive cadence lives only on the real fetch path,
# so the fake data source cannot exercise it. The real API returns an unchanged
# balance for minutes, which is why a 70 s run against it proves nothing.
#
# Usage:  pwsh -File build\cadence-test.ps1 [-Seconds 48] [-Delta 0.01] [-Port 18611]
param(
    [int]$Seconds = 48,
    [double]$Delta = 0.01,
    [int]$Port = 18611
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root 'build\dshb.exe'
$log  = Join-Path $root 'build\selftest.log'

Get-Process dshb -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill() }
Start-Sleep -Milliseconds 400
if (Test-Path $log) { Remove-Item $log -Force }
$env:DEEPSEEK_API_KEY = 'sk-local-test'

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()

$proc = Start-Process -FilePath $exe -PassThru -WorkingDirectory (Join-Path $root 'build') -ArgumentList @(
    "--api-host=127.0.0.1", "--api-port=$Port", '--api-plain-http',
    '--api-timeout-ms=2000', "--seconds=$($Seconds + 4)"
)

$val = 100.00
$served = 0
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    if ($listener.Pending()) {
        $client = $listener.AcceptTcpClient()
        $stream = $client.GetStream()
        $buf = New-Object byte[] 8192
        $null = $stream.Read($buf, 0, 8192)
        $v = $val.ToString('0.00')
        $body = '{"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"' + $v +
                '","granted_balance":"0.00","topped_up_balance":"' + $v + '"}]}'
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($body)
        $head = "HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n"
        $hb = [System.Text.Encoding]::ASCII.GetBytes($head)
        $stream.Write($hb, 0, $hb.Length)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        Start-Sleep -Milliseconds 150
        $client.Close()
        $served++
        $val -= $Delta          # every response differs -> expect "value changed, shorter"
    }
    Start-Sleep -Milliseconds 40
}
$listener.Stop()
$proc.WaitForExit()

Write-Host "server answered $served requests (every value different)"
Write-Host "=== actual gap vs the interval the program reported ==="
$prev = $null
Get-Content $log | Select-String -Pattern '\[api t=' | ForEach-Object {
    $line = $_.Line
    $m = [regex]::Match($line, 't=([0-9.]+)s')
    if (-not $m.Success) { return }
    $t = [double]$m.Groups[1].Value
    $what = if ($line -match 'interval ->') { $line -replace '.*interval -> ', '' } else { 'fetch ' + ($line -replace '.*\| ', '') }
    $gap = if ($null -ne $prev) { '   actual gap {0:N1}s' -f ($t - $prev) } else { '' }
    Write-Host ("  t={0,5:N1}s  {1}{2}" -f $t, $what, $gap)
    $script:prev = $t
}
