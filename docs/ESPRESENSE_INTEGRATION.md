# Bluetooth tracking integration

This sketch adapts the advertisement fingerprinting and reporting logic in
ESPresense `f89e5519709277dc80dcf58897058de91d3da200`, and its
`AdaptivePercentileRSSI` algorithm. Upstream source:
https://github.com/ESPresense/ESPresense/tree/f89e5519709277dc80dcf58897058de91d3da200

The adapted files are `Beacon_Scanner.h` and
`Beacon_Scanner.ino`. Their SPDX identifier is AGPL-3.0-only; the upstream license
is included as `ESPRESENSE_LICENSE.txt`. Pinned reference files are in
`build/espresense/upstream`. This is an adaptation, not an unmodified library copy.

## Using the feature

Normal startup uses passive continuous scanning. `setupBLE(true)` remains available
for continuous active scanning. Call `requestActiveBLEScan()` to queue a single
five-second active scan instead. The next `doBLE()` stops the current scan and
starts that finite scan, then resumes continuous passive scanning when it ends.
Requests during an active one-shot coalesce; failed starts retry normally. OTA
defers queued requests and interrupts running scans; an interrupted one-shot
returns to passive after OTA. The request call is nonblocking and task-safe;
scanner setup and servicing remain owned by the BLE loop task.
No BLE connections, pairing, enrollment, sensor
drivers, remote settings subscriptions, or ESPresense UI are imported.

- `/ble/devices`: authenticated table, automatically refreshed every five seconds.
  Manufacturer and device/beacon format are separate. For example, `iBeacon / Apple`
  describes a format and company, not an assertion about a particular Apple model.
  The name may be unavailable in passive advertisements. Distances are estimates.
- `/ble`: authenticated whitelist management. The initial mode publishes all devices
  eligible under the upstream identity/distance rules. Enabling whitelist-only mode
  restricts tracking/publication to matching entries, except the built-in iBeacon UUID described below.
- `/ble/save`: authenticated POST forms only, at most 2,048 body bytes, with CSRF
  and revision tokens. Stale edits return HTTP 409. The page never includes saved
  keys. Blank key inputs keep the existing key; “Remove key” explicitly erases it.

Entries accept an exact fingerprint ID or a MAC (12 hex digits, optionally
colon-separated), a 32-hex-character Identity Resolving Key, and an optional alias.
Keys use conventional AES big-endian hex notation, not reversed byte order.
Resolved IRKs take precedence over MACs, which take precedence over fingerprint IDs.
An alias becomes the topic ID and JSON `id` (and the published name). An IRK-only
entry without an alias receives a persisted `device:<8-hex-digit-token>` identity;
an IRK entry with an explicit identifier uses that identifier unless aliased.
No key material is used as an identity or emitted in MQTT, HTML, or logs.

Aliases contain 1–63 ASCII letters, digits, underscores, or hyphens. Match IDs are
limited to 159 printable ASCII characters excluding MQTT topic separators/wildcards.
Names are bounded to 63 bytes and displayed with HTML escaping. Long generic
service fingerprints that do not fit are skipped rather than truncated into a
potentially different device's identity. Ordinary MAC/name identification remains.

Configuration is a compact versioned binary blob in NVS namespace `bletracking`, key
`config`. Only populated entries and their used text/key bytes are stored: an empty
configuration occupies 10 bytes, and an unaliased MAC entry adds 19 bytes. NVS supplies
atomic blob replacement; RAM is updated only after a
successful write. Changing settings clears current tracking histories so no old
alias or filter decision remains active. Existing application NVS keys are unchanged.
Invalid/unreadable saved configuration restricts publication to the built-in iBeacon UUID until settings are saved.
The existing NVS partition is 20 KiB and shared with application settings. Especially
long full whitelists may exhaust storage or leave insufficient space for replacement;
such saves return an error without activating a partial configuration.

## MQTT and distance

The existing broker connection publishes QoS 0, non-retained messages to:

`espresense/devices/<effective-device-id>/<normalized-room>`

Room names use lowercase words separated by underscores, falling back to the
hardware hostname when empty. Fields include `mac`, `id`, optional `name`,
`rssi@1m`, `rxAdj`, `rssi`, optional `rssiVar`, `distance`, `var`, `int`, and applicable
advertised battery/temperature/humidity data. Numeric estimates retain two decimal
places. Manufacturer/type are web-table metadata, not additions to the MQTT schema.

The filter uses a 15-second window and the mean within Tukey's 1.5-IQR fences
(the upstream method named `getMedianIQR`), not the 75th percentile. Variances use
all current readings. Reference RSSI preference is beacon calibration, manufacturer
power, service power, then −71 dBm (upstream receiver −65 plus default TX −6).
Receiver adjustment is 0 dB; the path-loss exponent is 2.7.

`distance = 10^((referenceRSSI - filteredRSSI) / 27)`

Unconfigured non-static random-address-only devices and ignored fingerprints are
tracked for the table but not published. Configured identities override that
eligibility, as upstream known-device identities do. Distances above 16 metres are
shown in the table but not published, except for the built-in iBeacon UUID. Normal reports use five-second boundaries;
movement of at least 0.5 m can advance a report using the upstream divisor rule
(maximum divisor 10). Reports require an unreported observation no older than the
15-second RSSI window. Failed publishes do not
advance report state, and no offline message history is queued by this adapter.

## Deliberate adaptations and bounds

- NimBLE 2.5.1 callback signatures and UUID string formats; callback-only scanning.
- All manufacturer blocks are examined. Specific formats take precedence over
  generic company recognition. iBeacon and AltBeacon fields use checked byte reads.
- Eddystone URL, UID, and unencrypted TLM work with NimBLE 2.x. The older upstream
  Eddystone branch is disabled for NimBLE 2; this port retains it safely, corrects
  the UID namespace/instance layout, and decodes signed 8.8 TLM temperature.
- IRK resolution uses the installed mbedTLS AES implementation. Different private
  addresses matching one configured IRK share the same history. Unconfigured devices
  sharing a model-level fingerprint are not merged into one physical device.
- Up to 32 tracked devices and 45 saved configuration entries. Devices expire after
  150 seconds; capacity pressure evicts the least recently seen ordinary device
  before considering a built-in always-tracked iBeacon.
- Each history has a fixed capacity of 20 readings in one 100-byte allocation.
  At no more than one callback observation per second, this preserves the complete
  15-second window (including the sample exactly 15 seconds old). Faster traffic
  keeps only the most recent 20 readings. All 32 histories consume 3,200 bytes,
  excluding allocator overhead. There are no resize allocations.
- Runtime state, history allocations, page snapshots, and configuration copies
  use a centralized 32,768-byte heap reserve. This check does not guarantee memory
  availability during concurrent TLS/network operations. Required iBeacons fall
  back to their latest valid RSSI when history allocation is unavailable.
- The device page accepts a positive `page` query parameter and shows 10 rows per
  page in stable MAC-address order, with Previous/Next navigation. Out-of-range
  pages clamp to the last page; malformed/zero page numbers return HTTP 400.
  Only the displayed rows are copied, at most 7,440 bytes, without a full-table
  snapshot. A whitelist page/edit still uses an 11,172-byte configuration copy.
  Only one outstanding BLE HTTP snapshot is admitted; concurrent requests get
  HTTP 503 until the response finishes or disconnects.
- Both the ElegantOTA start callback (before Update.begin) and `/wantota` stop
  scanning and release all history buffers immediately, clearing stale sightings.
  An atomic OTA flag and scan/state locks prevent callbacks from repopulating
  histories or restarting scans during OTA. Progress callbacks retry exceptional
  scan-stop failures. Failed OTA resumes scanning on the next BLE tick; successful
  OTA remains paused until reboot. Whitelist configuration is preserved.
- MQTT topic/payload buffers are 256/1,024 bytes. Overflow rejects the message;
  there is no truncated JSON. JSON is emitted directly into the fixed buffer.
- Scanner callbacks never retain NimBLE-owned pointers. A state mutex protects
  histories/configuration; a configuration mutex serializes edits with reporting.
  HTTP/MQTT transmission uses snapshots outside the state mutex.
- Scanning pauses for OTA and retries a failed start no faster than every three
  seconds. Existing radar/network housekeeping stays on its existing task.

## Validation

### Missing radar fallback

With `SCAN_BLE` enabled, radar identification and mode commands wait at most five
seconds for a complete, checksum-valid radar frame. An absent or unresponsive
radar switches the application to Bluetooth scanning only, records that state in
the radar diagnostic message, and skips radar polling/reporting and periodic
reset attempts. Networking, the web interface, and BLE continue. An explicit
`handleRadarMode(true)` or reboot retries the radar; the fallback is not persisted
and does not change the user's saved sensor settings. The response is a liveness
check, not confirmation that the sensor applied the mode command. Non-BLE builds
retain the existing radar-library behavior. No installed library is modified.

Run `build/espresense/run-radar-tests.ps1` for timeout/framing tests and the actual
startup/reset/polling function bodies under mocked UART/time, with BLE both enabled
and disabled. Checks cover missing sensors, successful startup/retry, runtime
response failure, skipped polling/reporting, malformed/noisy UART data, and timer
rollover. Hardware unplug/reconnect and reset-failure checks remain pending.

The full ESP32 core 3.3.8 build passed with this fallback and the one-shot active
scan addition: **1,396,424 bytes flash; 51,884 bytes static RAM**. This leaves
569,656 bytes by Arduino's accounting in the existing application partition.
Compared with the earlier integrated build below, both follow-up changes together
add 460 flash bytes and no static RAM. Build artifacts and source hashes are in
[radar-fallback](../build/espresense/radar-fallback); passing native checks are in
[radar-tests.log](../build/espresense/radar-tests.log) and
[radar-ble-regression-tests.log](../build/espresense/radar-ble-regression-tests.log).
The BLE-disabled radar path was tested natively; a new full BLE-disabled firmware
build was not run for these follow-up changes. No firmware was uploaded.

Run `build/espresense/run-tests.ps1` from PowerShell. It compiles and executes the
actual adapted headers and scanner with controlled platform stubs. Core checks cover
malformed advertisements, identity precedence, power/byte order, Eddystone frames,
IQR/outliers, expiry, full-buffer shrink, allocation failures, and `millis()` rollover.
Runtime checks cover service UUID normalization, scanning/OTA, MQTT formatting and
failure behavior, whitelist matching, IRKs, aliases, persistence, forms, table escaping,
capacity, and expiry. Compact NVS encoding is checked against truncated records.
An `-Os -fstack-usage` check prevents large configuration-reset temporaries: native
initialization/decoding frames are 144/176 bytes, respectively. ESP32 object-code
inspection confirms 96/80-byte local frames after the in-place reset fix. These are
compiler frame sizes, not measured task stack high-water readings. Browser inspection of
the generated read-only fixtures verified table layout and the key-hidden forms.
AES test outputs are independently generated with Node crypto;
the native AES stub checks adapter byte ordering against those vectors, not the
ESP32 mbedTLS implementation itself.

The isolated fresh baseline uses core 3.3.8, warning logging, disabled PSRAM, the
existing partition table, and these unchanged board options:

```
esp32:esp32:esp32-poe:UploadSpeed=921600,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=min_spiffs,DebugLevel=warn,PSRAM=disabled,EraseFlash=none
```

The three integration builds below predate the `requestActiveBLEScan()` addition.
That addition passed the native core/runtime/web and stack checks, including finite
scan completion, coalescing, failed stop/start, and OTA deferral/interruption; see
[active-scan-tests.log](../build/espresense/active-scan-tests.log). The subsequent
BLE-enabled firmware build is recorded under Missing radar fallback above;
hardware scan verification remains pending.

All three builds passed with core 3.3.8 and `-Os`; the integrated build retains
NimBLE-Arduino 2.5.1. The disabled build adds only
`compiler.cpp.extra_flags=-DDISABLE_SCAN_BLE` to the board settings above.

| Build | Arduino flash bytes | Static RAM bytes |
| --- | ---: | ---: |
| Fresh baseline | 1,356,848 | 51,852 |
| Integrated BLE tracking | 1,395,964 | 51,884 |
| Change from baseline | +39,116 | +32 |
| BLE disabled | 1,032,308 | 38,248 |

The integrated build fits the existing 1,966,080-byte application partition, with
570,116 bytes remaining by Arduino's flash accounting. Its binary file is
1,396,112 bytes including image overhead. These features increase flash; these
measurements do not claim a reduction from the earlier optimization request.

| Linker section | Baseline bytes | Integrated bytes | Change |
| --- | ---: | ---: | ---: |
| `.iram0.text` | 127,939 | 128,155 | +216 |
| `.flash.text` | 904,392 | 934,056 | +29,664 |
| `.flash.rodata` | 295,128 | 304,364 | +9,236 |
| `.dram0.data` | 28,105 | 28,105 | 0 |
| `.dram0.bss` | 23,744 | 23,776 | +32 |

Build logs, ELF/map files, and complete linker sections are retained in
[baseline](../build/espresense/baseline), [integrated](../build/espresense/integrated), and
[disabled](../build/espresense/disabled). The integrated measurement records source
and ELF hashes. Automated native checks passed; their output is retained in
[native-tests.log](../build/espresense/native-tests.log). Heap bounds above describe
allocated data sizes, not measured on-device free heap or fragmentation.

No firmware has been uploaded. Live passive reception, RF-derived distance accuracy,
broker delivery, concurrent HTTP/network load, heap fragmentation, radar, LEDs,
TLS integrations, OTA transfer, and core-dump download require hardware smoke checks.

## Always tracked iBeacons

iBeacons advertising UUID 699EBC80-E1F3-11E3-9A0F-0CF3EE3BC012 bypass whitelist-only mode and the 16-metre publication cutoff for every major/minor pair. They retain their iBeacon identity even with calibration byte 3; configured aliases still apply. Ordinary devices cannot evict them from the 32-slot table. If all slots contain these beacons, the oldest is replaced when another arrives. Normal report timing, observation expiry, valid measurements, available memory, and MQTT connectivity still apply.

If RSSI history allocation is blocked by the 32 KiB heap reserve or fails, the built-in iBeacon family uses its latest valid RSSI observation for distance and reporting, with zero variance. This fallback uses the existing snapshot and allocates no RSSI history. Normal history filtering resumes automatically when allocation succeeds. The heap reserve remains in effect for network/OTA headroom.

