param()
$ErrorActionPreference = 'Stop'
$dumperExe = Join-Path $PSScriptRoot 'build\Release\UnleashedDumperV2.exe'
if (-not (Test-Path -LiteralPath $dumperExe)) { throw 'Build the application with build-release.ps1 first.' }
$dumperLogs = Join-Path $PSScriptRoot 'logs'
New-Item -Path $dumperLogs -ItemType Directory -Force | Out-Null
$dumperStamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
$dumperErrorLog = Join-Path $dumperLogs ("launch-$dumperStamp.stderr.txt")
$dumperProcess = Start-Process -FilePath $dumperExe -WorkingDirectory (Split-Path -Parent $dumperExe) -WindowStyle Normal -PassThru -RedirectStandardOutput (Join-Path $dumperLogs ("launch-$dumperStamp.stdout.txt")) -RedirectStandardError $dumperErrorLog
try { [void]$dumperProcess.WaitForInputIdle(5000) } catch [System.InvalidOperationException] { }
$dumperProcess.Refresh()
if ($dumperProcess.HasExited) {
    $dumperDetails = Get-Content -LiteralPath $dumperErrorLog -Raw -ErrorAction SilentlyContinue
    throw "Dumper exited during startup (code $($dumperProcess.ExitCode)). $dumperDetails Log: $dumperErrorLog"
}
if ($dumperProcess.MainWindowHandle -eq 0) {
    throw "Dumper has no visible main window. Check: $dumperErrorLog"
}
Write-Host "Unleashed Dumper V2 started (PID $($dumperProcess.Id))."
