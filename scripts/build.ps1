# Configures (if needed) and builds dinput8.dll + DOTOVR.dll.
param([string]$Config = 'Release', [string]$Vcpkg = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }   # any CMake on the PATH
# vcpkg: -Vcpkg <dir>, else <repo>\local\vcpkg, else the sibling Dishonored 2 VR repo's local\vcpkg, else $env:VCPKG_ROOT
if (-not $Vcpkg) {
    if (Test-Path "$repo\local\vcpkg\vcpkg.exe") { $Vcpkg = "$repo\local\vcpkg" }
    elseif (Test-Path "$repo\..\dishonored-2-vr\local\vcpkg\vcpkg.exe") { $Vcpkg = (Resolve-Path "$repo\..\dishonored-2-vr\local\vcpkg").Path }
    elseif ($env:VCPKG_ROOT -and (Test-Path "$env:VCPKG_ROOT\vcpkg.exe")) { $Vcpkg = $env:VCPKG_ROOT }
    else { throw "vcpkg not found: pass -Vcpkg <dir>, set VCPKG_ROOT, or clone vcpkg into local\vcpkg" }
}
# x64-windows-static: the dependencies are built with the static C runtime (/MT), like the mod itself (see
# src/CMakeLists.txt), so the DLLs need no Visual C++ redistributable on the player's machine. A build tree
# configured for the old /MD triplet is reconfigured from scratch.
$triplet = 'x64-windows-static'
$cache = "$repo\build\CMakeCache.txt"
if ((Test-Path $cache) -and -not (Select-String -Path $cache -Pattern "VCPKG_TARGET_TRIPLET:STRING=$triplet$" -Quiet)) {
    "Build tree was configured for another vcpkg triplet: reconfiguring for $triplet"
    Remove-Item $cache -Force
    Remove-Item "$repo\build\CMakeFiles" -Recurse -Force -ErrorAction SilentlyContinue
}
if (-not (Test-Path $cache)) {
    & $cmake -S "$repo\src" -B "$repo\build" -G 'Visual Studio 17 2022' -A x64 `
        "-DCMAKE_TOOLCHAIN_FILE=$Vcpkg\scripts\buildsystems\vcpkg.cmake" `
        "-DVCPKG_TARGET_TRIPLET=$triplet"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
}
else {
    # re-run configure so the version header picks up the current commit
    & $cmake "$repo\build" | Out-Null
}
& $cmake --build "$repo\build" --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
Get-Item "$repo\build\bin\dinput8.dll", "$repo\build\bin\DOTOVR.dll" | ForEach-Object { "Built: $($_.FullName) ($($_.LastWriteTime))" }
