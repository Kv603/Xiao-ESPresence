param([string]$Compiler='C:/Apps/Perl/Strawberry/c/bin/g++.exe')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$json=Join-Path $env:USERPROFILE 'Documents/Arduino/libraries/ArduinoJson/src'
$out=Join-Path $root 'build/native-c6'
New-Item -ItemType Directory -Force -Path $out | Out-Null
# Compile the sketch's actual settings transaction and topic normalization with
# the host transport adapter, without importing private secrets or Arduino setup.
$sketchSource=Get-Content (Join-Path $root 'xiao_espresense_ble-2026-10-06.ino') -Raw
$bleSource=Get-Content (Join-Path $root 'Beacon_Scanner.ino') -Raw
function Extract-Function([string]$source,[string]$signature) {
  $match=[regex]::Match($source,'(?s)'+[regex]::Escape($signature)+'.*?\r?\n\}')
  if(!$match.Success){throw "Missing test source function: $signature"}
  return $match.Value
}
$roomSource='namespace bletrack {' + (Extract-Function $bleSource 'std::string normalize(const std::string &s, char separator) {') + "`n}`n" + (Extract-Function $bleSource 'std::string bleRoomTopicName(const std::string &room, const std::string &node) {')
Set-Content (Join-Path $out 'mqtt-room-test.inc') $roomSource
$settingsSource=(Extract-Function $sketchSource 'static bool mqttConnectionChanged(const AppSettings &a, const AppSettings &b) {') + "`n" + (Extract-Function $sketchSource 'static bool networkChanged(const AppSettings &a, const AppSettings &b) {') + "`n" + (Extract-Function $sketchSource 'static void processSettings(JsonVariantConst command, const std::string &id) {') + "`n" + (Extract-Function $sketchSource 'static void processLedConfiguration(JsonVariantConst command, const std::string &id) {')
Set-Content (Join-Path $out 'settings-test.inc') $settingsSource
foreach($test in @('core','rssi-equivalence','runtime','control','led','mqtt')) {
  & $Compiler -std=c++17 -O1 -Wall -Wextra -Werror -include "$PSScriptRoot/stubs/Test_Config.h" "-I$PSScriptRoot/stubs" "-I$json" (Join-Path $PSScriptRoot "test-$test.cpp") -o (Join-Path $out "$test.exe")
  if($LASTEXITCODE){throw "$test compilation failed"}
  & (Join-Path $out "$test.exe")
  if($LASTEXITCODE){throw "$test failed"}
}
# Exercise layouts that cannot share the default-layout assertions.
foreach($layout in @(
  @('-DLED_STRIP_COUNT=1','-DLED_SKIP_FIRST=0'),
  @('-DLED_STRIP_COUNT=1','-DLED_SKIP_FIRST=1'),
  @('-DLED_STRIP_COUNT=51','-DLED_SKIP_FIRST=0','-DLED_GROUP_WIDTH=1','-DLED_GROUP_GAP=0'),
  @('-DLED_SKIP_FIRST=0','-DLED_GROUP_WIDTH=3','-DLED_GROUP_GAP=1'),
  @('-DLED_STRIP_COUNT=8','-DLED_GROUP_WIDTH=2','-DLED_GROUP_GAP=2')
)) {
  & $Compiler -std=c++17 -O1 -Wall -Wextra -Werror "-I$json" @layout (Join-Path $PSScriptRoot 'test-led-layout.cpp') -o (Join-Path $out 'led-layout.exe')
  if($LASTEXITCODE){throw 'LED layout compilation failed'}
  & (Join-Path $out 'led-layout.exe')
  if($LASTEXITCODE){throw 'LED layout failed'}
}
