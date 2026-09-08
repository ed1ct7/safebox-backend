# сборка под Windows:
#   powershell -File scripts/build.ps1 [-Preset msvc-debug] [-NoTest]
# сам находит VS через vswhere и поднимает dev-окружение, если cl.exe нет в PATH.
# vcpkg берет из VCPKG_ROOT, если его нет - тот что идет с VS
param(
    [ValidateSet("msvc-debug", "msvc-release", "msvc-asan")]
    [string]$Preset = "msvc-debug",
    [switch]$NoTest
)

$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "Не найден vswhere.exe - установите Visual Studio 2022/2026 с компонентом C++"
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    throw "Visual Studio с MSVC x64 не найдена"
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Host "==> окружение разработчика: $vs" -ForegroundColor Cyan
    Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}

if (-not $env:VCPKG_ROOT) {
    $bundled = Join-Path $vs "VC\vcpkg"
    if (-not (Test-Path (Join-Path $bundled "scripts\buildsystems\vcpkg.cmake"))) {
        throw "VCPKG_ROOT не задан и встроенный vcpkg не найден - см. README"
    }
    $env:VCPKG_ROOT = $bundled
}
Write-Host "==> vcpkg: $env:VCPKG_ROOT" -ForegroundColor Cyan

Write-Host "==> configure: $Preset" -ForegroundColor Cyan
cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> build: $Preset" -ForegroundColor Cyan
cmake --build --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $NoTest -and $Preset -ne "msvc-release") {
    Write-Host "==> test: $Preset" -ForegroundColor Cyan
    ctest --preset $Preset
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "==> готово: build\$Preset\src\daemon\safeboxd.exe" -ForegroundColor Green
