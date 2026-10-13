$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'build/board-options-test'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$recipes = @('recipe.hooks.objcopy.postobjcopy.3.pattern_args','recipe.hooks.objcopy.postobjcopy.4.pattern','recipe.hooks.objcopy.postobjcopy.4.pattern.windows','tools.esptool_py.upload.pattern_args','tools.esptool_py.program.pattern_args')
foreach ($version in @('3.3.8','3.3.12')) {
  $data = Join-Path $out $version
  $core = Join-Path $data "packages/esp32/hardware/esp32/$version"
  New-Item -ItemType Directory -Force -Path (Join-Path $core 'tools/partitions') | Out-Null
  Set-Content (Join-Path $core 'boards.txt') '# Fixture board package'
  $base = foreach ($key in $recipes) {
    "$key=command 0xe000 boot_app0.bin 0x10000 application.bin"
  }
  Set-Content (Join-Path $core 'platform.txt') $base
  Set-Content (Join-Path $core 'boards.local.txt') 'other.board.setting=preserved'
  Set-Content (Join-Path $core 'platform.local.txt') 'other.recipe=preserved'
  $installArguments = @{ ArduinoData = $data }
  if ($version -ne '3.3.8') { $installArguments.CoreVersion = $version }
  & (Join-Path $root 'tools/Install-BoardOptions.ps1') @installArguments | Out-Null
  $boards = Get-Content (Join-Path $core 'boards.local.txt') -Raw
  $platform = Get-Content (Join-Path $core 'platform.local.txt') -Raw
  if (!$boards.Contains('other.board.setting=preserved') -or !$platform.Contains('other.recipe=preserved')) { throw 'Unrelated overrides lost' }
  if (!$boards.Contains('ble_ota4m.upload.maximum_size=2031616') -or !$boards.Contains('ble_ota4m.build.tracker_app_addr=0x20000') -or !$boards.Contains('ble_ota4m.build.tracker_ota_addr=0x1e000')) { throw 'Wrong partition menu or addresses' }
  foreach ($key in $recipes) {
    if (!$platform.Contains("$key=command {build.tracker_ota_addr} boot_app0.bin {build.tracker_app_addr} application.bin")) { throw "Unpatched recipe $key" }
  }
  $copied = Get-Content (Join-Path $core 'tools/partitions/xiao_ble_ota4m.csv') -Raw
  if ($copied -ne (Get-Content (Join-Path $root 'config/partitions/xiao_ble_ota4m.csv') -Raw)) { throw 'Partition CSV differs' }
  & (Join-Path $root 'tools/Install-BoardOptions.ps1') @installArguments | Out-Null
  if ((Get-Content (Join-Path $core 'boards.local.txt') -Raw) -ne $boards -or (Get-Content (Join-Path $core 'platform.local.txt') -Raw) -ne $platform) { throw 'Installer is not idempotent' }
  # Conflicting custom recipes must fail before any installation files change.
  Add-Content (Join-Path $core 'platform.local.txt') ($recipes[0] + '=custom')
  $failed = $false
  try { & (Join-Path $root 'tools/Install-BoardOptions.ps1') @installArguments | Out-Null } catch { $failed = $true }
  if (!$failed -or (Get-Content (Join-Path $core 'boards.local.txt') -Raw) -ne $boards) { throw 'Conflicting override protection failed' }
}
Write-Output 'Board installer tests passed: default 3.3.8, explicit 3.3.12, addresses, preservation, idempotence and conflicts.'
