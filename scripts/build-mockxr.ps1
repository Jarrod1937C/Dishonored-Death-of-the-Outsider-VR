# Builds the mock OpenXR runtime (test tool) and writes its loader manifest JSON.
# Activate it for a game run with: $env:XR_RUNTIME_JSON = <printed manifest path>
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }   # any CMake on the PATH
# the OpenXR headers come from the mod's vcpkg install (triplet x64-windows-static since the mod links the CRT statically)
$prefix = "$repo\build\vcpkg_installed\x64-windows-static"
if (-not (Test-Path "$prefix\share\openxr-loader\OpenXRConfig.cmake")) { throw "OpenXR not found at $prefix. Build the mod first (scripts\build.ps1)." }
$cache = "$repo\build-mockxr\CMakeCache.txt"
if ((Test-Path $cache) -and -not (Select-String -Path $cache -Pattern ('^CMAKE_PREFIX_PATH:UNINITIALIZED=' + [regex]::Escape($prefix) + '$') -Quiet)) {
    "mock build tree points at another OpenXR install: reconfiguring"
    Remove-Item $cache -Force; Remove-Item "$repo\build-mockxr\CMakeFiles" -Recurse -Force -EA SilentlyContinue
}
if (-not (Test-Path $cache)) {
    & $cmake -S "$repo\mockxr" -B "$repo\build-mockxr" -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$prefix"
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
}
& $cmake --build "$repo\build-mockxr" --config Release
if ($LASTEXITCODE -ne 0) { throw "build failed" }
$dll = "$repo\build-mockxr\bin\DVR_MockXR.dll"
$escaped = $dll -replace '\\', '\\'
$json = "{`n  `"file_format_version`": `"1.0.0`",`n  `"runtime`": {`n    `"library_path`": `"$escaped`"`n  }`n}"
$jsonPath = "$repo\build-mockxr\DVR_MockXR.json"
[System.IO.File]::WriteAllText($jsonPath, $json)
"Built: $dll"
"Manifest: $jsonPath"
