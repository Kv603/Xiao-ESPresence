New-Item -ItemType Directory -Force .\tools | Out-Null

$source = 'C:\Users\passp\Documents\Arduino\xiao-ble-tracker_2026-09-29\tools\Install-BoardOptions.ps1'
(Get-Content $source -Raw).Replace('3.3.8', '3.3.12') |
    Set-Content .\tools\Install-BoardOptions.ps1 -Encoding utf8

.\tools\Install-BoardOptions.ps1
