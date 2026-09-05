# сборка под Windows:
#   powershell -File scripts/build.ps1 [-Preset msvc-debug] [-NoTest]
# сам находит VS через vswhere и поднимает dev-окружение, если cl.exe нет в PATH.
# vcpkg берет из VCPKG_ROOT, если его нет - тот что идет с VS
param(
    [string]$Preset = "msvc-debug"
)

$ErrorActionPreference = "Stop"

if (-not $env:VCPKG_ROOT) {
    Write-Error "VCPKG_ROOT не задан - укажите путь к клону microsoft/vcpkg (см. README)"
}

Write-Host "==> configure: $Preset" -ForegroundColor Cyan
cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> build: $Preset" -ForegroundColor Cyan
cmake --build --preset $Preset
exit $LASTEXITCODE
