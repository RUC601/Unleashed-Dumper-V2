param()
$ErrorActionPreference = 'Stop'
$dumperCmake = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $dumperCmake)) {
    $dumperCmake = (Get-Command cmake -ErrorAction Stop).Source
}
& $dumperCmake -S $PSScriptRoot -B (Join-Path $PSScriptRoot 'build') -G 'Visual Studio 18 2026' -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $dumperCmake --build (Join-Path $PSScriptRoot 'build') --config Release --parallel 6
exit $LASTEXITCODE
