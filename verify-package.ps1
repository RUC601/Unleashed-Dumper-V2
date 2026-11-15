param()
$ErrorActionPreference = 'Stop'
$manifestPath = Join-Path $PSScriptRoot 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'Run this script in the extracted portable package.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema -ne 'unleashed-dumper-portable/1') { throw 'Unknown package manifest.' }
foreach ($file in $manifest.files) {
    $path = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $file.path))
    if (-not $path.StartsWith($PSScriptRoot.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid manifest path.' }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) { throw "Missing or changed file: $($file.path)" }
}
if (-not ('DumperPackageLibraries' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DumperPackageLibraries {
    [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr LoadLibraryEx(string file, IntPtr reserved, uint flags);
    [DllImport("kernel32")] public static extern bool FreeLibrary(IntPtr library);
}
'@
}
foreach ($name in @('capstone.dll','vmm.dll','leechcore.dll','FTD3XX.dll','vcruntime140.dll')) {
    $library = [DumperPackageLibraries]::LoadLibraryEx((Join-Path $PSScriptRoot $name), [IntPtr]::Zero, 0x1100)
    if ($library -eq [IntPtr]::Zero) { throw "$name could not load (Windows error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))." }
    [void][DumperPackageLibraries]::FreeLibrary($library)
}
$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = Join-Path $PSScriptRoot 'UnleashedDumperV2.exe'
$start.Arguments = '--help'
$start.WorkingDirectory = $PSScriptRoot
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [Diagnostics.Process]::Start($start)
$help = $process.StandardOutput.ReadToEnd()
$errorText = $process.StandardError.ReadToEnd()
$process.WaitForExit()
if ($process.ExitCode -ne 0 -or $help -notmatch '--scan') { throw "Offline startup check failed: $errorText" }
$process.Dispose()
Write-Output "PASS: $($manifest.files.Count) file hashes, five runtime libraries, and offline startup. No device connection was made."
