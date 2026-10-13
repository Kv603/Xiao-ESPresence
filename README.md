# XIAO ESP32-C6 BLE MQTT tracker

Advertisement-only BLE tracking with Wi-Fi, MQTT/MQTTS, Preferences, MQTT-triggered HTTP/HTTPS OTA, optional password-protected ArduinoOTA network uploads, and up to 51 WS2812B LEDs driven by LiteLED over RMT.

Can hardcode an always-tracked UUID, such as iBeacon which bypasses the configurable whitelist and distance cutoff, receiving priority in the tracking table and retaining the latest-RSSI low-memory fallback. Configured aliases and matching IRKs still apply. MQTT settings cannot edit this built-in rule. The independent configurable whitelist holds 45 entries; the live tracking table holds 32 devices. BLE reporting follows the  `espresense/devices/<device>/<room>` topic format and JSON format.

BLE declarations and shared types live in `Beacon_Scanner.h`; fingerprinting, filtering, Preferences validation, tracking and scanner implementation live in `Beacon_Scanner.ino`. Supporting documentation is in `docs/`.

## Build and partition installation

Versions used for the LED firmware: Arduino CLI, **Arduino-ESP32 3.3.12**, **NimBLE-Arduino 2.5.1**, **ArduinoJson 7.4.3**, **LiteLED 3.2.0**. ESP-MQTT, HTTP client, TLS and OTA come from the core.
Copy `secrets.example.header` to `secrets.h` and fill in credentials.

```powershell
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install NimBLE-Arduino@2.5.1 ArduinoJson@7.4.3 LiteLED@3.2.0
./tools/Install-BoardOptions.ps1 -CoreVersion 3.3.12
arduino-cli compile --jobs 4 --fqbn 'esp32:esp32:XIAO_ESP32C6:FlashSize=4M,PartitionScheme=ble_ota4m,DebugLevel=none' --build-path build/c6-ble_ota4m .
./tests/Run.ps1
```

Restart Arduino IDE after installing the board options. Select **XIAO_ESP32C6**, then choose **Tools > Reload Board Data**. The IDE saves board-option lists per sketch, so restarting alone can leave the new partition options hidden. Reloading resets board options to their defaults: select 4 MB flash, Core Debug Level **None**, and **Tools > Partition Scheme > BLE: custom (1984 KiB OTA / 84 KiB NVS)** afterwards, and recheck any other desired board settings. The installer adds version-specific `boards.local.txt` and `platform.local.txt` blocks while preserving unrelated settings. Its legacy default is 3.3.8; specify `-CoreVersion 3.3.12` for this build. Repeat installation after restoring/updating the core. The installer reads that version's recipes and refuses missing recipes or conflicting overrides.

Equivalent CLI compilation:

```powershell
arduino-cli compile --jobs 4 --fqbn 'esp32:esp32:XIAO_ESP32C6:FlashSize=4M,PartitionScheme=ble_ota4m,DebugLevel=none' --build-path build/c6-ble_ota4m .
$image = Get-Item build/c6-ble_ota4m/xiao_espresense_ble-2026-10-06.ino.bin
if ($image.Length -gt (2031616 - 65536)) { throw 'Application needs at least 64 KiB OTA slot headroom' }
```

`build_opt.h` applies size optimization and disables NimBLE C++ logging. The board build flags include `sdkconfig.h` directly to retain the compatible SDK host configuration: forcing observer-only host roles with this core/library pair caused duplicate-host link failures. The application exclusively uses scanning APIs; linker garbage collection removes unused C++ entry points. No installed NimBLE files are modified. A fully pruned host would require rebuilding compatible SDK/library components and is unnecessary for this image's flash budget.

| Profile | Each OTA slot | NVS | Purpose |
|---|---:|---:|---|
| `default` | 1,310,720 bytes | 20 KiB | Too small for this TLS-enabled application |
| `ble_min_spiffs` | 1,966,080 bytes | 20 KiB | Image fits; insufficient worst-case settings storage |
| `ble_ota4m` | 2,031,616 bytes | 84 KiB | Selected; two OTA slots and Preferences, no filesystem |

The maximum serialized BLE blob is 11,035 bytes. Two configurable 4 KiB PEM roots, credentials, labels, and LED mappings also occupy NVS, and blob replacement needs free pages while the previous value remains valid. Preserve whitelist capacity and trust configuration instead of relying on a nearly full 20 KiB NVS partition. Keep at least 64 KiB application headroom with the size check above.

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

The node also discovers one Home Assistant sensor named **Tracked BLE devices**. Its retained configuration is published at `homeassistant/sensor/espresense_<nodeId>/<room>/config`; its non-retained decimal count is published at `devices/espresense_<nodeId>/<room>/count`. The room uses the same normalization as BLE topics, falling back to the normalized node ID. The entity's unique ID stays constant when the room changes.

The count includes each valid tracking entry seen within the last 15 seconds that meets the existing BLE identity and distance rules, including the built-in iBeacon exemption. It includes readings already published and excludes stale or otherwise unreportable entries. Count changes publish promptly, at most once per second; unchanged counts refresh every **31 seconds**. This adds a count sensor without changing BLE reporting intervals.

Availability uses `devices/espresense_<nodeId>/<room>/status`. Each successful MQTT connection clears any retained status and publishes non-retained `online`. A retained `offline` LWT is configured before connecting. Orderly shutdown publishes the same retained `offline` and waits up to two seconds for its acknowledgment before stopping (publication still uses the existing network timeout). Discovery, counts, and availability use QoS 1. Count updates also repeat non-retained `online`.

The node subscribes to the default Home Assistant birth topic, `homeassistant/status`. Receiving `online`, including a retained birth message, schedules discovery and current count/availability replay. Discovery is deferred until MQTT connects and repeated on reconnection. Room changes restart the MQTT client to update its LWT, clear the old room's retained discovery on the active broker, and announce the replacement after settings are committed. A room-only restart and failed-candidate rollback preserve the active selected broker, including a backup; persisted broker settings remain unchanged. Failed candidates retain the previous configuration. Switching brokers does not reconnect to the old broker solely to remove discovery.

Pending discovery tombstones are stored atomically with the new room in the existing settings blob and restored on boot. They remain pending until the matching active broker acknowledges deletion and the updated journal is saved. Failed acknowledgments or NVS writes retry after five seconds. Renaming back cancels cleanup of the current discovery topic. The journal holds up to 16 records and shares the 16,000-byte settings limit; an update that cannot persist its cleanup is rejected with the previous settings retained.

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

When Wi-Fi first obtains an IP address and whenever it reconnects, explicit/saved/legacy broker settings are tried first. For the unsaved default broker, `MQTT_SERVER_BACKUP`/`TESTNETPREFIX` select the LAN backup on the matching subnet and `MQTT_SERVER` otherwise. If MQTT cannot connect or disconnects while Wi-Fi is available, broker selection is retried after five seconds using those configured servers for the current subnet. Retries preserve the original scheme, port, credentials and CA, and leave persisted settings intact; TLS connections stay on TLS. The active server name is retained in the global `selectedMqttServer` buffer (40 bytes including the terminating NUL); longer names are truncated for serial messages only, while the full URI is used for connections. MQTT events print `MQTT connected to '<SERVER>'` and `MQTT to '<SERVER>' disconnected`.

`ble_save` with `token:0` creates an entry. Get state to obtain its generated token, then use that token to update/delete it. Each save supplies `match` and `alias`; either a matching identifier or a valid IRK is required. MACs are normalized. `irk` accepts 32 hexadecimal characters; omit it or send an empty string to retain an existing key, or use `remove_key:true` to remove it. Duplicate identifiers, aliases or IRKs are rejected.

```json
{"id":"key-1","operation":"ble_save","revision":2,"token":123,"match":"aabbccddeeff","alias":"my_beacon","irk":"ec0234a357c8ad05341010a60a397d9b"}
{"id":"delete-1","operation":"ble_delete","revision":3,"token":123}
```

The immutable always-tracked list is reported separately from `entries`. BLE edits cannot remove its exemption. Existing `bletracking/config` blobs and legacy `mmW32` room/label/broker-host keys are read compatibly. New connection settings use a single versioned `tracker/settings` blob. Failed writes leave active settings unchanged; corrupted saved settings are not silently overwritten.

## WS2812B LED groups

The strip uses LiteLED 3.2.0's RMT driver, WS2812 timing and GRB order. Connect DIN to **D2/GPIO2** by default, using a suitable strip power supply and a common ground. Set these defines in `secrets.h` or compiler flags; each has a default in `LED_Core.h`:

| Define | Default | Meaning |
|---|---:|---|
| `LED_STRIP_PIN` | 2 | Strip DIN GPIO |
| `LED_STRIP_COUNT` | 51 | Physical pixel count, 1–51 |
| `LED_SKIP_FIRST` | 1 | Keep pixel 0 black; use 0 to include it |
| `LED_GROUP_WIDTH` | 3 | Positive number of pixels per group |
| `LED_GROUP_GAP` | 1 | Nonnegative number of black pixels between groups |

Groups are numbered from **1**. Physical pixels are numbered from **0**. A group's first pixel is `LED_SKIP_FIRST + (group - 1) * (LED_GROUP_WIDTH + LED_GROUP_GAP)`. The final group may be shorter. Defaults yield 13 groups: group 1 uses pixels 1–3, group 2 uses 5–7, and group 13 uses 49–50. Pixel 0 and every gap stay black. Startup is off; general brightness defaults to 255. Before reducing the group count in a new build, clear mappings for removed groups: saved out-of-range groups fail settings validation.

Read `/config/get` for the current settings `revision`, then send a non-retained JSON command to `bletracker/<nodeId>/config/set`:

```json
{"id":"led-1","operation":"led","revision":0,"brightness":200,"groups":[{"group":1,"topics":["sensor/lobby/#","house/alert"]},{"group":13,"topics":["sensor/door/+"]}]}
```

`brightness` is LiteLED's general **0–255** brightness and multiplies all group output. Omitted fields stay unchanged. Each listed group replaces its filter list; `topics:[]` clears that group. Unlisted groups retain their mappings. Brightness and mappings share the existing revision and settings blob, survive reboot, and activate only after a successful save. Live values and channel overrides start off after reboot.

Filters accept MQTT `+` and `#` wildcards. `sensor/lobby/#` matches `sensor/lobby`, `sensor/lobby/mmwave`, and descendants such as `sensor/lobby/nested/blue`. `+` matches exactly one level, including an empty level. Topic/filter matching is case-sensitive; root wildcards do not match `$` topics. Shared-subscription filters (`$share/...`) are not supported. A filter may feed several groups; overlapping filters update a group once per received message. Distinct filters subscribe once at QoS 1, with retry and reconnection replay. The node's own control prefix and `homeassistant/status` always use their normal handlers and never feed the LEDs.

Only the final suffixes `/red`, `/green`, and `/blue` select a channel for numeric payloads. All other suffixes are ignored when choosing the event behavior; the payload is processed normally. For a group subscribed to `motion/woodshopdoor/#`, both `motion/woodshopdoor` and `motion/woodshopdoor/human` accept a numeric percentage as white or a color-name payload at 75%. `motion/woodshopdoor/blue` with a numeric payload updates only blue. Each concrete topic remains an independent source for normal mixing.

On ordinary matched topics:

- A finite numeric payload **0–100**, including fractions, contributes white at that percentage. It replaces the previous contribution from that concrete topic.
- A color name, as plain text or a JSON string, is trimmed and converted to lowercase. The lookup supports the [CSS named colors](https://www.w3.org/TR/css-color-4/#named-colors), `on` as white, `off` as black, and `brickred` as firebrick (`#B22222`). Named colors start at **75%**. CSS `green` is `#008000`; use `lime` for full-intensity green.
- Contributions from different concrete topics add, with each RGB channel capped at 255 before general strip brightness. `off`, `black`, and numeric zero clear only that source.
- Each non-red source fades linearly from its received level to zero over **60 seconds**. A new message restarts only that source's fade. Only the names `red` and `brickred` persist; `firebrick` fades normally.

Numeric messages on topics ending exactly in **`/red`, `/green`, or `/blue`** replace that group's corresponding channel with `round(255 * percentage / 100)`, preserving its other channels and their timers. For example:

```text
sensor/lobby/green  -> 100   # (0,255,0)
sensor/lobby/red    -> 100   # (255,255,0), preserving green
sensor/lobby/blue   -> 50    # (255,255,128)
sensor/lobby/red    -> 0     # clears only red
```

Channel overrides apply after ordinary topic mixing. Red persists; green and blue overrides independently fade to zero over 60 seconds. An override remains active at zero, so an older mixed channel does not reappear. The next ordinary numeric or named-color message clears all overrides for its matching groups and resumes normal source mixing. A color-name payload on `/red`, `/green`, or `/blue` is an ordinary color message.

Retained LED values are ignored. Payloads are at most 64 bytes; concrete topic names and filters are at most 160 bytes. There are at most 64 group/filter assignments and 64 live ordinary concrete sources. Expired or unreferenced source slots are reused. The separate 32-event FIFO preserves message order; excess events or new sources at capacity are rejected and counted. Unknown colors, malformed numbers, and out-of-range values leave output unchanged. The existing 16,000-byte settings limit also applies, so a large mapping plus CA certificates can exceed it and fail atomically.

`/config/get` includes `led` brightness/mappings, layout, `driver_ready`, `driver_error`, `driver_failures`, `subscriptions_pending`, `queued_events`, `event_overflow`, `source_overflow`, and `invalid_values`. Rendering runs at up to 50 Hz while BLE/Home Assistant keep their approximately one-second service cadence. LED fades are also serviced during broker/acknowledgment waits. Both OTA paths clear transient LED state and turn the strip off; failed updates resume with LEDs off until new messages arrive.

Physical acceptance remains separate from compilation: verify red/green/blue order, pixel 0 and gaps staying black, group 13's two pixels, global brightness 0 then 255, additive topics, `/green` followed by `/red`, and 60-second fades while BLE/Wi-Fi are active. Check OTA entry turns the strip off. No flashing is performed by the native tests.

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

`tests/Run.ps1` runs the preserved BLE core/runtime regression suite, 12,000 RSSI equivalence observations, configuration persistence/failure tests and MQTT fragment/URL validation. It also tests the 15-second reportable-device count, count coalescing and refresh, discovery/availability messages, subscription acknowledgments, shutdown and room-change rollback. LED tests cover alternate layouts, wildcard matching, colors/numbers, additive mixing, channel overrides, independent decay and rollover, FIFO/capacity limits, subscriptions, and the production LED settings transaction and event routing. The current tests live outside build output. Old reports in `docs/` and old `build/` artifacts describe the previous multi-board/web firmware.

The MQTT tests use the production settings serializer/loader with simulated NVS, including reset recovery, cleanup-write failure, backup-broker room changes and rollback. `tests/Test-BoardOptions.ps1` checks default 3.3.8 and explicit 3.3.12 installation in isolated fixture directories, preserving unrelated overrides and patching all required flash addresses.

`tests/Test-MqttBroker.mjs` exercises the production MQTT lifecycle and settings transaction through a host adapter against a temporary loopback broker. It verifies retained discovery, automatic LWT after a dropped connection, reconnection, Home Assistant birth replay, room discovery replacement and manual shutdown. It uses simulated BLE counts and an MQTT.js network adapter; physical-board and Home Assistant UI validation remain separate. Run the native tests first to generate the shared fixtures:

```powershell
./tests/Run.ps1
npm install --prefix build/mqtt-broker-private --no-audit --no-fund --ignore-scripts aedes@1.2.0 mqtt@5.16.0
node tests/Test-MqttBroker.mjs
```

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

