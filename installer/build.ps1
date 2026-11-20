# 构建 Release 版并打出安装包 dist\PocketDesk-<版本>-setup.exe
# 需要：Visual Studio（带 CMake）、Inno Setup 7（默认找 .local\innosetup，也可以用 ISCC 环境变量指定 ISCC.exe）
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$version = (Select-String -Path "$root\src\main.cpp" -Pattern 'kVersion = "([^"]+)"').Matches[0].Groups[1].Value

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $cmake = & $vswhere -latest -prerelease -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
}
if (-not $cmake) { throw '找不到 cmake' }

$iscc = $env:ISCC
if (-not $iscc) {
  $iscc = @("$root\.local\innosetup\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 7\ISCC.exe",
            "$env:ProgramFiles\Inno Setup 7\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 7\ISCC.exe") |
          Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $iscc) { throw '找不到 Inno Setup 的 ISCC.exe' }

Push-Location $root
try {
  & $cmake --build --preset release
  if ($LASTEXITCODE) { throw "构建失败（$LASTEXITCODE）" }
  & $iscc /Qp "/DAppVersion=$version" "$root\installer\pocketdesk.iss"
  if ($LASTEXITCODE) { throw "打包失败（$LASTEXITCODE）" }
} finally {
  Pop-Location
}
Get-Item "$root\dist\PocketDesk-$version-setup.exe"
