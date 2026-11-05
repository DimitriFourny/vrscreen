# Builds VRScreen for Windows with the Visual Studio (Build Tools) C++ toolchain, CMake and
# Ninja (both ship with the "Desktop development with C++" workload).
#
#   powershell -ExecutionPolicy Bypass -File windows\build.ps1            # Release build + tests
#   powershell -ExecutionPolicy Bypass -File windows\build.ps1 -Debug
#   powershell -ExecutionPolicy Bypass -File windows\build.ps1 -Dist      # also build\VRScreen-<version>-windows.zip
#
# Output: windows\build\<Release|Debug>\vrscreen-sender.exe, vrscreen-viewer.exe, vrscreen-tests.exe
param([switch]$Debug, [switch]$Dist, [switch]$NoTests)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$config = if ($Debug) { 'Debug' } else { 'Release' }
$buildDir = Join-Path $PSScriptRoot "build\$config"

# The MSVC environment (cl, link, rc, the Windows SDK, cmake, ninja), from vcvars64.bat.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw 'Visual Studio Build Tools not found: winget install Microsoft.VisualStudio.BuildTools (with the C++ workload, see windows\README.md)' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio installation with the C++ tools (workload "Desktop development with C++")' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
    }
}

if (-not (Test-Path (Join-Path $PSScriptRoot 'resources\vrscreen.ico'))) {
    & (Join-Path $PSScriptRoot 'scripts\make-ico.ps1')
}

cmake -S $PSScriptRoot -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$config"
if ($LASTEXITCODE) { throw 'cmake configure failed' }
cmake --build $buildDir
if ($LASTEXITCODE) { throw 'build failed' }

if (-not $NoTests) {
    & (Join-Path $buildDir 'vrscreen-tests.exe')
    if ($LASTEXITCODE) { throw 'unit tests failed' }
}

if ($Dist) {
    $version = (Get-Content (Join-Path $root 'VERSION')).Trim()
    $stage = Join-Path $root "build\VRScreen-$version-windows"
    Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item (Join-Path $buildDir 'vrscreen-sender.exe') (Join-Path $stage 'VRScreen.exe')
    Copy-Item (Join-Path $buildDir 'vrscreen-viewer.exe') $stage
    Copy-Item (Join-Path $PSScriptRoot 'README.md') $stage
    # The headset app, if it was built (quest\gradlew assembleDebug) or dropped next to the script.
    foreach ($apk in @((Join-Path $root 'quest\app\build\outputs\apk\debug\app-debug.apk'), (Join-Path $PSScriptRoot 'vrscreen-headset.apk'))) {
        if (Test-Path $apk) { Copy-Item $apk (Join-Path $stage 'vrscreen-headset.apk'); break }
    }
    $zip = "$stage.zip"
    Remove-Item -Force $zip -ErrorAction SilentlyContinue
    Compress-Archive -Path "$stage\*" -DestinationPath $zip
    "wrote $zip"
}
"built $buildDir"
