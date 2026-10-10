[CmdletBinding()]
param([string]$ArduinoData = "$env:LOCALAPPDATA/Arduino15")
$ErrorActionPreference = 'Stop'
$core = Join-Path $ArduinoData 'packages/esp32/hardware/esp32/3.3.12'
if (!(Test-Path -LiteralPath (Join-Path $core 'boards.txt'))) { throw 'Arduino-ESP32 3.3.12 is required.' }
$root = Split-Path $PSScriptRoot -Parent
$local = Join-Path $core 'boards.local.txt'
$text = if (Test-Path -LiteralPath $local) { [IO.File]::ReadAllText($local) } else { '' }
$text = [regex]::Replace($text, '(?ms)^# BEGIN XIAO BLE TRACKER\r?\n.*?^# END XIAO BLE TRACKER\r?\n?', '')
$block = @'
# BEGIN XIAO BLE TRACKER
XIAO_ESP32C6.menu.PartitionScheme.ble_min_spiffs=BLE: stock min_spiffs (1920 KiB OTA / 20 KiB NVS)
XIAO_ESP32C6.menu.PartitionScheme.ble_min_spiffs.build.partitions=min_spiffs
XIAO_ESP32C6.menu.PartitionScheme.ble_min_spiffs.upload.maximum_size=1966080
XIAO_ESP32C6.menu.PartitionScheme.ble_min_spiffs.build.defines=-include sdkconfig.h
XIAO_ESP32C6.menu.PartitionScheme.ble_ota4m=BLE: custom (1984 KiB OTA / 84 KiB NVS)
XIAO_ESP32C6.menu.PartitionScheme.ble_ota4m.build.partitions=xiao_ble_ota4m
XIAO_ESP32C6.menu.PartitionScheme.ble_ota4m.upload.maximum_size=2031616
XIAO_ESP32C6.menu.PartitionScheme.ble_ota4m.build.defines=-include sdkconfig.h
XIAO_ESP32C6.menu.PartitionScheme.ble_ota4m.build.tracker_ota_addr=0x1e000
XIAO_ESP32C6.menu.PartitionScheme.ble_ota4m.build.tracker_app_addr=0x20000
# END XIAO BLE TRACKER
'@
# Core 3.3.12 hardcodes the stock OTA metadata/app addresses in upload and merge recipes.
# Parameterize only those recipes; defaults preserve every other board's behavior.
$platformLocal = Join-Path $core 'platform.local.txt'
$platformText = if (Test-Path -LiteralPath $platformLocal) { [IO.File]::ReadAllText($platformLocal) } else { '' }
$platformText = [regex]::Replace($platformText, '(?ms)^# BEGIN XIAO BLE ADDRESSES\r?\n.*?^# END XIAO BLE ADDRESSES\r?\n?', '')
$recipes = @('recipe.hooks.objcopy.postobjcopy.3.pattern_args','recipe.hooks.objcopy.postobjcopy.4.pattern','recipe.hooks.objcopy.postobjcopy.4.pattern.windows','tools.esptool_py.upload.pattern_args','tools.esptool_py.program.pattern_args')
$overrides = @('# BEGIN XIAO BLE ADDRESSES','build.tracker_ota_addr=0xe000','build.tracker_app_addr=0x10000')
$base = [IO.File]::ReadAllLines((Join-Path $core 'platform.txt'))
foreach ($key in $recipes) {
  $existing = @($platformText -split '\r?\n' | Where-Object { $_.StartsWith($key + '=') })
  if ($existing.Count) { throw "Existing platform.local override for $key; merge manually instead of overwriting it." }
  $line = $base | Where-Object { $_.StartsWith($key + '=') }
  if (!$line) { throw "Missing core recipe: $key" }
  $overrides += $line.Replace('0xe000','{build.tracker_ota_addr}').Replace('0x10000','{build.tracker_app_addr}')
}
$overrides += '# END XIAO BLE ADDRESSES'
Copy-Item -LiteralPath (Join-Path $root 'config/partitions/xiao_ble_ota4m.csv') -Destination (Join-Path $core 'tools/partitions/xiao_ble_ota4m.csv') -Force
[IO.File]::WriteAllText($local, $text.TrimEnd() + "`n" + $block + "`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($platformLocal, $platformText.TrimEnd() + "`n" + ($overrides -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
Write-Output 'Installed XIAO BLE partition menu options for core 3.3.12.'
Write-Output 'In Arduino IDE select XIAO_ESP32C6, then Tools > Reload Board Data to refresh the persistent board-options cache.'
Write-Output 'Select Tools > Partition Scheme > BLE: custom (1984 KiB OTA / 84 KiB NVS), and recheck other board options after reloading.'
Write-Output 'A restart alone may retain the old menu. Restart as well if the IDE was open during installation, so upload recipes are refreshed.'

