param(
    [string]$Destination,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-release.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
}
if (-not $Destination) { $Destination = Join-Path $PSScriptRoot 'dist' }
if (-not [IO.Path]::IsPathRooted($Destination)) { $Destination = Join-Path (Get-Location).Path $Destination }
$Destination = [IO.Path]::GetFullPath($Destination)
$release = Join-Path $PSScriptRoot 'build/Release'
$required = @('UnleashedDumperV2.exe', 'rules.json', 'capstone.dll', 'vmm.dll', 'leechcore.dll', 'FTD3XX.dll', 'vcruntime140.dll')
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $release $name) -PathType Leaf)) { throw "Missing runtime file: $name" }
}
if ((Get-FileHash (Join-Path $release 'rules.json')).Hash -ne (Get-FileHash (Join-Path $PSScriptRoot 'data/rules.json')).Hash) {
    throw 'Release rules are stale. Build before packaging.'
}
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
$package = Join-Path $Destination "Unleashed-Dumper-V2-$stamp-windows-x64"
if (Test-Path -LiteralPath $package) { throw 'Package destination already exists.' }
New-Item -ItemType Directory -Path $package -Force | Out-Null
foreach ($name in $required) { Copy-Item -LiteralPath (Join-Path $release $name) -Destination $package }
foreach ($name in @('assets','licenses')) { Copy-Item -LiteralPath (Join-Path $release $name) -Destination $package -Recurse }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'verify-package.ps1') -Destination $package
$readme = Join-Path $PSScriptRoot '../../docs/unleashed-dumper/PORTABLE_README.zh-en.txt'
Copy-Item -LiteralPath $readme -Destination (Join-Path $package 'README.txt')
$files = @(Get-ChildItem -LiteralPath $package -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($package.Length+1).Replace('\','/'); bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
[ordered]@{schema='unleashed-dumper-portable/1'; built_utc=[DateTime]::UtcNow.ToString('o'); files=$files} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding UTF8
& (Join-Path $package 'verify-package.ps1')
if (-not $?) { throw 'Portable package check failed.' }
$archive = "$package.zip"
Compress-Archive -LiteralPath $package -DestinationPath $archive -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath "$archive.sha256" -Value "$hash  $([IO.Path]::GetFileName($archive))" -Encoding ASCII
Write-Output $archive
Write-Output "SHA256 $hash"
