param([string]$Compiler='C:/Apps/Perl/Strawberry/c/bin/g++.exe')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$json=Join-Path $env:USERPROFILE 'Documents/Arduino/libraries/ArduinoJson/src'
$out=Join-Path $root 'build/native-c6'
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach($test in @('core','rssi-equivalence','runtime','control')) {
  & $Compiler -std=c++17 -O1 -Wall -Wextra -Werror "-I$PSScriptRoot/stubs" "-I$json" (Join-Path $PSScriptRoot "test-$test.cpp") -o (Join-Path $out "$test.exe")
  if($LASTEXITCODE){throw "$test compilation failed"}
  & (Join-Path $out "$test.exe")
  if($LASTEXITCODE){throw "$test failed"}
}
