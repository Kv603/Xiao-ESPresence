Historical migration report. The subsequent ESPresense tracking, whitelist, and web-table work is documented in [ESPRESENSE_INTEGRATION.md](ESPRESENSE_INTEGRATION.md), with replacement tests under `build/espresense`.

# NimBLE scanner migration

`Beacon_Scanner.ino` now uses the installed **NimBLE-Arduino 2.5.1** library.
The `setupBLE(boolean active)` and `doBLE()` entry points remain unchanged.
No other application source files were edited for this migration.

## Scan behavior

- `NimBLEScanCallbacks::onResult(const NimBLEAdvertisedDevice *)` handles results.
- `getResults(5000, false)` preserves a blocking five-second scan. NimBLE 2.x
  uses milliseconds, and its `start()` method is asynchronous.
- Active/passive selection and the 100 ms interval / 99 ms window are retained.
- Results are counted and cleared after each scan. Duplicate callbacks remain
  disabled. A static callback object replaces the previous heap allocation.
- Scanning initializes lazily if setup has not run; initialization failure or an
  already-active scan returns safely. This also covers normal boot, where the
  current main sketch calls `setupBLE()` only inside its panic-recovery branch.

## Beacon reports

- RSSI, device name, service UUID, and other manufacturers' raw data remain.
- iBeacon reports retain company ID, UUID, major/minor, and signed TX power.
  Apple manufacturer ID, iBeacon subtype/length, and payload size are validated.
- Eddystone frames are identified by the service-data UUID `FEAA`, including
  when another service-data element precedes them.
- URL decoding supports all four scheme prefixes and fourteen expansion codes.
  Invalid lengths, reserved codes, and invalid prefixes are rejected before
  decoding. Raw URL output includes the scheme byte.
- Version-0 TLM reports retain battery voltage, advertisement count, and uptime
  in seconds. NimBLE's signed 8.8 temperature value is converted to Celsius;
  `0x8000` is reported as unavailable. Truncated and unsupported TLM frames are
  ignored. Redundant duplicate URL/TLM summaries were removed.

## Validation

Native checks execute the scanner and copies of the installed NimBLE beacon/TLM
helper implementations, with host stubs for UUID, radio, serial, and scan APIs:

```powershell
node build/nimble-migration/test-scanner.mjs
```

Checks cover URL schemes/expansions, malformed and maximum-length frames,
service-data UUID selection, iBeacon byte order and signed power, positive and
negative temperatures, the unavailable-temperature marker, TLM version/length,
five-second scan timing, initialization failure, repeated setup, callback
lifetime, active-scan guards, cleanup, and compilation with `SCAN_BLE` disabled.
These checks passed. They require Node.js and a native `g++` on PATH.

The final ESP32-POE build passed using Arduino CLI 1.3.1, ESP32 core 3.3.8,
`-Os`, warning-level logging, disabled PSRAM, and the existing partition table.

| Final build metric | Bytes | Arduino usage |
| --- | ---: | ---: |
| Program flash | 1,356,740 | 69% |
| Static RAM | 51,852 | 15% |
| Firmware `.bin` | 1,356,896 | |

ELF sections agree with Arduino's reported totals. Library and symbol checks
confirmed that NimBLE is linked and the legacy Arduino BLE library/Bluedroid
host is absent. The final build also includes the tested raw-URL scheme byte.

Firmware, ELF, linker map, section sizes, build options, and a scanner source
hash are saved in `build/nimble-migration/firmware/`. The application firmware is
`mmWaveSense_2026_09_09_codex.ino.bin` within that folder.

Reproduce the target build from the sketch folder with:

```powershell
arduino-cli compile --jobs 4 --fqbn 'esp32:esp32:esp32-poe:UploadSpeed=921600,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=min_spiffs,DebugLevel=warn,PSRAM=disabled,EraseFlash=none' --build-path "$env:TEMP\mmwave-nimble-build" .
```

No firmware has been uploaded and no live-radio testing was performed.

## References

- [NimBLE 2.x migration guide](https://h2zero.github.io/NimBLE-Arduino/md_1_8x__to2_8x__migration__guide.html)
- [Eddystone-URL wire format](https://github.com/google/eddystone/tree/master/eddystone-url)
- Installed NimBLE-Arduino headers and implementation sources were used to
  verify exact signatures and telemetry return units.
