# C6 implementation and validation

Measured on 2026-09-28 with Arduino-ESP32 3.3.8, NimBLE-Arduino 2.5.1 and ArduinoJson 7.4.3. The connected XIAO ESP32-C6 was installed over COM10 after a complete flash backup.

## ArduinoOTA addition — 2026-09-29

The ArduinoOTA-enabled C6 build passes compilation, linking and generated partition/merged-image/upload-offset verification. Application binary: **1,639,968 bytes**; each OTA slot: **2,031,616 bytes**; headroom: **391,648 bytes**; static RAM: **54,100 bytes**. SHA-256: `4b98077ee1e4dcbef6f61c151a22ae4c36e4fff5c4138c6aa8aede4e3701d06c`. The added sketch libraries are ArduinoOTA, Update, ESPmDNS and Hash from the pinned core. Current measurements are in `build/c6-ble_ota4m/measurement.json`, with the build log in `build/c6-arduino-ota-build.log`.

Callback registration was moved from invalid file-scope statements into initialization. The service starts after Wi-Fi connection; application uploads pause BLE/MQTT and discard queued commands. Error recovery is deferred until the core's synchronous handler returns, and a successful upload remains paused through reboot. `ARDUINO_OTA=0` excludes the code. Password configuration and IDE upload instructions are in README.md.

Existing native regression tests pass (`build/c6-arduino-ota-tests.log`). This image has not been flashed or tested with an ArduinoOTA network upload. Authentication failures, interrupted uploads, reconnection, runtime heap and success through both slots remain hardware checks; compilation does not establish those results. The measurements below describe earlier images.

## Source consolidation — 2026-09-29

BLE declarations/types are consolidated in `Beacon_Scanner.h` and all BLE implementations, including configuration serialization and persistence, in `Beacon_Scanner.ino`. The former three BLE headers and the NimBLE build wrapper have been removed. Only README.md remains as Markdown at the project root; supporting documents are under `docs/`.

The completed source passes the native fingerprint, 12,000-observation RSSI equivalence, runtime/settings, always-tracked/IRK/alias/low-memory, redacted configuration response, and control-message suites. The final C6 compile and generated partition/merged-image/upload-offset checks pass. Application binary: **1569392 bytes**; each slot: **2031616 bytes**; remaining headroom: **462224 bytes**. Static RAM remains **49,380 bytes**. SHA-256: `d4c6f97f17f6804d36f33436c35c5f46df41209293bfa978232b2d9b10529d6a`. Historical logs are `build/c6-consolidation-tests.log` and `build/c6-consolidation-final.log`.

This source-only refactor was not flashed to COM10. The USB and hardware measurements below describe the previously installed September 28 image. Board options now force-include SDK `sdkconfig.h` directly; restart Arduino IDE and use **Tools > Reload Board Data** to refresh cached board properties after updating.

## Previously USB-validated build and flash

| Measurement | Result |
|---|---:|
| Application binary, including image overhead | 1,568,976 bytes |
| Arduino reported application sections | 1,568,864 bytes |
| Static RAM | 49,380 bytes |
| Each selected OTA slot | 2,031,616 bytes |
| Remaining space per slot | 462,640 bytes |
| Required minimum remaining space | 65,536 bytes |
| NVS | 86,016 bytes |
| Total flash | 4,194,304 bytes |

Application SHA-256: `2a131bd0752a1fa23ed85ece6dae64cf1c7610e555471f9cdce06d0211beb82d`. This build uses private credentials; rebuilding with different settings changes the hash and potentially the size. The current verifier output is described in the source-consolidation section above.

The default slot is smaller than the application. Stock min_spiffs has sufficient application space (397,104 bytes spare), but its 20 KiB NVS cannot provide the full storage contract: the BLE blob alone can reach 11,035 bytes, CA roots can occupy another 8,192 bytes before JSON overhead, and NVS needs metadata and free pages for replacement/garbage collection. No destructive stock-NVS exhaustion test was performed on the user's existing settings. The custom layout was selected to preserve full configuration capacity.

The generated partition binary, merged image and uploader arguments were checked against the CSV. USB readback verified immutable bootloader, partition-table and application regions. The running device independently printed NVS at `0x9000`, OTA metadata at `0x1e000`, app0 at `0x20000`, and app1 at `0x210000`, ending at `0x400000`. NVS/OTA metadata are mutable after boot and must not be compared with the initial image as if immutable.

Final USB installation/readback passed. Its pre-installation backup is `build/device-backup/20260928-221215/before.bin`; earlier backups retain the original pre-migration firmware. Final serial evidence is `build/c6-final-serial.log`: BLE ready, Wi-Fi/MQTT connected, 202,732 bytes free heap and 193,636 minimum at that observation. A subsequent MQTT read confirmed app0, the immutable UUID, original empty configurable whitelist/mode, and cleared test CA overrides.

## Completed checks

- Native fingerprint/malformed-frame, RSSI, filtering, rollover, IRK, whitelist capacity, allocation failure and persistence tests pass. RSSI equivalence covers 12,000 observations.
- Always-tracked tests cover enabled/disabled filtering, empty/full configurable lists, the ordinary distance cutoff, configured aliases/IRKs, low heap, settings reload and OTA pause/resume. Ordinary-device filtering remains covered.
- Control tests cover fragmented messages, bounds, retained delivery flags, request identifiers and URL validation.
- On COM10: USB backup/migration, boot, BLE initialization, Wi-Fi, SNTP, plaintext MQTT, redacted configuration reads, immutable whitelist information, stale revision rejection and unsuccessful candidate-broker recovery were exercised.
- Hardware Preferences testing saved all 45 full-length entries with IRKs alongside two 4,096-byte CA fields, rejected entry 46, repeatedly rewrote the full blob, and verified entries/IRKs/CA settings after hardware reset. Invalid CA and OTA envelopes were rejected. A final independent read confirmed zero temporary entries, the original whitelist mode, and no temporary CA roots. Revision counters intentionally remain advanced.
- Removed application dependencies were checked in source: no web server, Ethernet, radar, filesystem, ElegantOTA or AsyncTCP remains. The Arduino build resolves only ArduinoJson, Network, NimBLE-Arduino, Preferences and WiFi as sketch libraries; MQTT/HTTP/TLS/OTA use the core SDK. No application HTTP listener is created.

Before capacity testing, BLE plus plaintext MQTT reported 202,764 bytes free heap and 195,344 bytes minimum free heap. With the maximum whitelist and both 4 KiB CA fields after reset, it reported 183,024 bytes free and 178,524 minimum. Following the cleanup writes, the session minimum was 152,912 bytes. These are observed values, not worst-case guarantees. MQTTS/HTTPS peak heap remains unmeasured. Machine-readable evidence is in `build/hardware-settings-results.json`.

## Remaining hardware checks and limitations

The full local-server integration suite could not connect the board to the workstation's test broker. Windows denied creation of the scoped inbound firewall rule from this session. Enable `tools/Allow-HardwareTests.ps1` in an Administrator PowerShell before retrying `tools/Test-Hardware.mjs`; remove the rule afterwards with `-Remove`. Network routing must also permit the board to reach the workstation.

Still pending: successful MQTTS and certificate/hostname/expiry failure cases; HTTP/HTTPS application downloads; wrong-chip, checksum, redirect and interrupted-download failures; two successful updates exercising both slots; power interruption during download; heap minima during TLS; and a physical beacon test across OTA. Native always-tracked tests do not substitute for that physical beacon test. Source inspection confirms no HTTP listener; an independent network listener scan is also pending.

Two requested constraints have explicit qualifications:

1. Observer-only NimBLE host compilation conflicted with this core's precompiled Bluetooth host, producing duplicate symbols. The build retains the compatible host configuration and uses scanning APIs only. It does not claim that sketch macros remove precompiled Bluetooth roles. No global NimBLE sources were edited; a matched SDK/library rebuild would be needed for that additional reduction.
2. Retained deliveries are rejected when ESP-MQTT receives the retained flag. MQTT 3.1.1 brokers clear that flag for live forwarding, so firmware cannot distinguish such publications. Enforce a broker-side prohibition on retained control publications if rejection of the original publisher's retained intent is required.

Application-health rollback is not claimed. Downloads write the inactive slot and only select a validated complete image; the stock Arduino startup confirms a newly booted application early.


