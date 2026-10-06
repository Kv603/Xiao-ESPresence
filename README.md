# XIAO ESP32-C6 BLE MQTT tracker

Advertisement-only BLE tracking with Wi-Fi, MQTT/MQTTS, Preferences, MQTT-triggered HTTP/HTTPS OTA, and optional password-protected ArduinoOTA network uploads. No HTTP listener, filesystem, radar, Ethernet, web UI or LED strip is included.

The hardcoded always-tracked iBeacon UUID **699ebc80-e1f3-11e3-9a0f-0cf3ee3bc012** is retained. Its devices bypass the configurable whitelist and 16 m cutoff, receive priority in the tracking table, and retain the latest-RSSI low-memory fallback. Configured aliases and matching IRKs still apply. MQTT settings cannot edit this built-in rule. The independent configurable whitelist holds 45 entries; the live tracking table holds 32 devices. BLE reports retain `espresense/devices/<device>/<room>` and the existing JSON format.

BLE declarations and shared types live in `Beacon_Scanner.h`; fingerprinting, filtering, Preferences validation, tracking and scanner implementation live in `Beacon_Scanner.ino`. Supporting documentation is in `docs/`.

## Build and partition installation

Versions used: Arduino CLI, **Arduino-ESP32 3.3.8**, **NimBLE-Arduino 2.5.1**, **ArduinoJson 7.4.3**. ESP-MQTT, HTTP client, TLS and OTA come from the core. Keep your existing private `secrets.h`, or copy `secrets.example.header` to `secrets.h` and fill in credentials. Never commit flash backups or credentials.

```powershell
arduino-cli core install esp32:esp32@3.3.8
arduino-cli lib install NimBLE-Arduino@2.5.1 ArduinoJson@7.4.3
./tools/Install-BoardOptions.ps1
./tools/Build.ps1 -Partition ble_ota4m
./tests/Run.ps1
```

Restart Arduino IDE after installing the board options. Select **XIAO_ESP32C6**, then choose **Tools > Reload Board Data**. The IDE saves board-option lists per sketch, so restarting alone can leave the new partition options hidden. Reloading resets board options to their defaults: select 4 MB flash, Core Debug Level **None**, and **Tools > Partition Scheme > BLE: custom (1984 KiB OTA / 84 KiB NVS)** afterwards, and recheck any other desired board settings. The installer adds version-specific `boards.local.txt` and `platform.local.txt` blocks while preserving unrelated settings. Repeat installation after restoring/updating the core; this script deliberately targets 3.3.8.

Equivalent CLI compilation:

```powershell
arduino-cli compile --jobs 4 --fqbn 'esp32:esp32:XIAO_ESP32C6:FlashSize=4M,PartitionScheme=ble_ota4m,DebugLevel=none' --build-path build/c6-ble_ota4m .
node tools/verify-image.mjs build/c6-ble_ota4m ble_ota4m
```

`build_opt.h` applies size optimization and disables NimBLE C++ logging. The board build flags include `sdkconfig.h` directly to retain the compatible SDK host configuration: forcing observer-only host roles with this core/library pair caused duplicate-host link failures. The application exclusively uses scanning APIs; linker garbage collection removes unused C++ entry points. No installed NimBLE files are modified. A fully pruned host would require rebuilding compatible SDK/library components and is unnecessary for this image's flash budget.

| Profile | Each OTA slot | NVS | Purpose |
|---|---:|---:|---|
| `default` | 1,310,720 bytes | 20 KiB | Too small for this TLS-enabled application |
| `ble_min_spiffs` | 1,966,080 bytes | 20 KiB | Image fits; insufficient worst-case settings storage |
| `ble_ota4m` | 2,031,616 bytes | 84 KiB | Selected; two OTA slots and Preferences, no filesystem |

The maximum serialized BLE blob is 11,035 bytes. Two configurable 4 KiB PEM roots, credentials and labels also occupy NVS, and blob replacement needs free pages while the previous value remains valid. Preserve whitelist capacity and trust configuration instead of relying on a nearly full 20 KiB NVS partition. The verifier enforces at least 64 KiB application headroom.

The custom table is `config/partitions/xiao_ble_ota4m.csv`: NVS `0x9000–0x1dfff`, OTA metadata `0x1e000–0x1ffff`, app0 `0x20000–0x20ffff`, app1 `0x210000–0x3fffff`. Arduino 3.3.8 otherwise hardcodes `0xe000` and `0x10000` in upload/merge recipes. The installer parameterizes those addresses, retaining stock defaults for other board profiles. Do not copy the CSV alone and assume the uploader will follow it. Do not add a root `partitions.csv` that silently overrides the menu.

## Initial USB installation and recovery

```powershell
./tools/Flash-Usb.ps1 -Port COM10
./tools/Read-Serial.ps1 -Port COM10
```

The USB helper verifies the image, backs up all 4 MB, preserves the old NVS pages in the expanded partition, writes a complete image and verifies flash readback. It resets the OTA selection to app0 and replaces filesystem contents; the prior image remains in `build/device-backup/<timestamp>/before.bin`. Backups contain private settings. Unsupported encrypted or shrinking NVS migrations are refused. Use this helper for the initial partition change rather than a normal application upload. Normal IDE uploads work with the installed address overrides once the new layout is established.

To restore the entire original board, use the installed esptool with the saved full image:

```powershell
& "$env:LOCALAPPDATA/Arduino15/packages/esp32/tools/esptool_py/5.2.0/esptool.exe" --chip esp32c6 --port COM10 write-flash 0 'build/device-backup/<timestamp>/before.bin'
```

If necessary, hold BOOT while connecting USB to enter the ROM downloader. Never deliver the merged 4 MB image through OTA. OTA accepts only the `.ino.bin` application and never changes partitions or erases Preferences.

Serial `?` reports firmware, network state, heap and actual partition addresses. Periodic serial health lines report current/minimum heap and dropped commands. Missing core-dump-partition diagnostics from the precompiled core are expected: this layout intentionally has no core-dump partition.

## MQTT settings

The stable control prefix is `bletracker/c6-<12 lowercase Wi-Fi MAC hex digits>`, printed at boot. It does not change with room/label settings. Use broker ACLs so only authorized operators can publish control commands. MQTT over plain TCP and HTTP downloads remain available for trusted networks; SHA-256 checks integrity and does not authenticate a command. TLS modes require CA trust and a synchronized clock; there is no downgrade or `setInsecure` path.

Publish JSON to these topics, always **non-retained**:

| Topic suffix | Use |
|---|---|
| `/config/get` | Request redacted settings, entries, revisions, hardcoded rules and diagnostics |
| `/config/set` | Change settings or one configurable BLE entry |
| `/config/result` | Command acknowledgements/errors, QoS 1 |
| `/config/state` | Full redacted state, QoS 0 because a full whitelist exceeds the reliable outbox bound |
| `/ota/set` | Request a firmware download |
| `/ota/status` | Acceptance and running/last-update result, QoS 1 |

Every command requires an `id` of 1–64 letters, digits, underscores or hyphens. Maximum command size is 8192 bytes; fragments are reassembled with contiguous offsets. The bounded queue holds two commands. Settings writes and OTA run in the application loop, not the MQTT callback. Retained commands are ignored, including retained reads. Queue overflows increment the diagnostic counter; retry with the current revision.

Retained rejection applies to the flag received from the broker. MQTT 3.1.1 clears that flag on live forwarding; configure broker policy to forbid retained control publications when that distinction matters.

Examples (publish each object on the indicated suffix):

```json
{"id":"read-1"}
```

Send the above to `/config/get`. State returns separate `revision` (connection/labels) and `ble_revision` counters. Use the corresponding current counter as `revision` in mutation commands:

```json
{"id":"room-1","operation":"settings","revision":0,"room":"living_room","label":"Living room"}
{"id":"broker-1","operation":"settings","revision":1,"uri":"mqtts://broker.example.lan:8883","mqtt_ca":"-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"}
{"id":"mode-1","operation":"ble_mode","revision":0,"only":true}
{"id":"entry-1","operation":"ble_save","revision":1,"token":0,"match":"aa:bb:cc:dd:ee:ff","alias":"my_beacon"}
{"id":"scan-1","operation":"active_scan"}
```

`settings` accepts `room`, `label` (63 bytes each), `uri` (255), `user` (128), `password` (256), `mqtt_ca` and `ota_ca` (4096 bytes each). Omitted fields stay unchanged. Change large CA fields in separate commands to stay below 8192 bytes. The URI must be `mqtt://host[:port]` or `mqtts://host[:port]`; URI credentials, paths and WebSockets are not supported. TLS MQTT with no CA is rejected. Credentials and IRKs are never returned; CA state is reported with presence flags.

Broker changes publish `testing`, connect to the candidate, wait for subscriptions and a acknowledged probe publication, and persist only on success. The final result is on the candidate broker; failure restores the previous broker and reports there when reachable. Power interruption during testing preserves the previous stored settings.

Existing private `MQTT_SERVER_BACKUP`/`TESTNETPREFIX` macros remain an optional bootstrap compatibility feature: after 30 seconds without MQTT on that Wi-Fi subnet, the unsaved default plaintext broker switches to the configured LAN backup. An explicit `TRACKER_MQTT_URI` or saved connection settings disables this fallback; TLS connections never downgrade.

`ble_save` with `token:0` creates an entry. Get state to obtain its generated token, then use that token to update/delete it. Each save supplies `match` and `alias`; either a matching identifier or a valid IRK is required. MACs are normalized. `irk` accepts 32 hexadecimal characters; omit it or send an empty string to retain an existing key, or use `remove_key:true` to remove it. Duplicate identifiers, aliases or IRKs are rejected.

```json
{"id":"key-1","operation":"ble_save","revision":2,"token":123,"match":"aabbccddeeff","alias":"my_beacon","irk":"ec0234a357c8ad05341010a60a397d9b"}
{"id":"delete-1","operation":"ble_delete","revision":3,"token":123}
```

The immutable always-tracked list is reported separately from `entries`. BLE edits cannot remove its exemption. Existing `bletracking/config` blobs and legacy `mmW32` room/label/broker-host keys are read compatibly. New connection settings use a single versioned `tracker/settings` blob. Failed writes leave active settings unchanged; corrupted saved settings are not silently overwritten.

## OTA

Standard ArduinoOTA is enabled by default (`ARDUINO_OTA=1`). Set `ARDUINO_OTA_PASSWORD` in private `secrets.h`; the existing `HTTP_PASS` is used as a compatibility fallback. With neither a nonempty dedicated password nor a nonempty fallback, the service stays disabled. Define `ARDUINO_OTA 0` to compile it out.

After Wi-Fi connects, the board advertises `c6-<MAC>.local` through mDNS and listens on UDP port 3232. In Arduino IDE, choose its network entry under **Tools > Port**, then upload using the configured password. Use the uploader supplied with the pinned core. The board opens a TCP connection back to the uploading workstation, so the network and workstation firewall must allow that connection; mDNS discovery normally requires the same local network.

ArduinoOTA accepts application updates on this filesystem-free layout. Its callbacks pause BLE and MQTT, discard queued control commands, and leave both paused through a successful reboot. On failure, cleanup completes before scanning and MQTT reconnect. Both update mechanisms execute in the main loop and cannot write concurrently. ArduinoOTA uses the core's own authenticated upload/integrity protocol; the URL, SHA-256 and HTTPS CA requirements below apply to MQTT-triggered downloads. No HTTP server is added.

Compute the byte length and SHA-256 of the application `.ino.bin`, serve it with HTTP 200 and an exact Content-Length, then publish:

```json
{"id":"deploy-20260929-1","url":"https://firmware.example.lan/tracker.bin","size":1565488,"sha256":"<64 hexadecimal SHA-256 characters>"}
```

Replace the illustrative size/hash with those of your build. Provision `ota_ca` before HTTPS downloads. Redirects, chunked transfer, wrong chip/image headers, oversize, truncation, checksum mismatch and validation failures are rejected. Downloads have a 10-second socket timeout, 15-second no-data timeout and 5-minute total limit.

After acceptance is acknowledged, scanning and MQTT pause to release memory. MQTT progress messages therefore stop during transfer. On failure, scanning/MQTT resume and the running status contains `last_update`. On success the board reboots into the other slot and reports the version, with the persisted update result. Reusing the last accepted OTA request ID is rejected even after reboot; use a new ID to retry a failed download. Queued settings commands are discarded when an update starts.

An interrupted download leaves the active application selected. This does not promise application-health rollback after a new firmware boots: the stock Arduino startup confirms the image early.

## Tests

`tests/Run.ps1` runs the preserved BLE core/runtime regression suite, 12,000 RSSI equivalence observations, configuration persistence/failure tests and MQTT fragment/URL validation. The current tests live outside build output. Old reports in `docs/` and old `build/` artifacts describe the previous multi-board/web firmware.

`tools/Test-Hardware.mjs` uses the connected board plus temporary TCP/TLS MQTT and HTTP/HTTPS servers on a workstation reachable by the board. It checks maximum whitelist writes, revision/retained-message handling, malformed/interrupted updates and alternating successful HTTP/HTTPS updates. It restores the original broker and temporary entries; a full flash backup provides additional recovery. It refuses existing CA overrides that its redacted state cannot restore. See `docs/C6_VALIDATION.md` for actual results and remaining physical tests. Server certificates/keys and all hardware outputs belong under ignored `build/` directories.

`tools/Test-MqttSettings.mjs` runs capacity and reset tests through the existing reachable broker without local listeners. Both scripts modify settings temporarily; keep the USB backup. Run from the project root. For this board the control node is `c6-10bda3a057b4`; override `TRACKER_TEST_NODE` for another device. The local test server address defaults to `192.168.254.155`; set `TRACKER_TEST_HOST` and update the certificate IP SAN and firewall scope if it changes.

Generate short-lived test certificates with OpenSSL (adjust its executable path for your installation):

```powershell
New-Item -ItemType Directory -Force build/hardware-private
openssl req -x509 -newkey rsa:2048 -nodes -days 3 -config tests/tls.cnf -keyout build/hardware-private/server.key -out build/hardware-private/server.pem
openssl req -x509 -newkey rsa:2048 -nodes -days 3 -config tests/tls.cnf -keyout build/hardware-private/untrusted.key -out build/hardware-private/untrusted.pem
$env:TRACKER_BROKER_HOST = '192.168.254.173'
node tools/Test-MqttSettings.mjs
# Administrator PowerShell: enable only for the duration of local-server tests.
./tools/Allow-HardwareTests.ps1
node tools/Test-Hardware.mjs
./tools/Allow-HardwareTests.ps1 -Remove
```

Do not use these test certificates as production trust roots. Test results are written under `build/hardware*-results.json`; firmware size and partition checks are in `build/c6-ble_ota4m/measurement.json`.

