# BLE and HTTP heap reductions

Historical notes for the former web-enabled firmware. The current C6 implementation has no HTTP server or device pages; see [C6 validation](C6_VALIDATION.md).

## Current BLE memory policy

The current firmware tracks 32 devices while retaining 45 whitelist entries.
RSSI histories are fixed at 20 readings: 100 bytes each, 3,200 bytes total
(excluding allocator overhead), with no resizing. Allocation checks preserve
32 KiB of free heap. The device page copies at most 10 rows (7,440 bytes) per
response and provides page navigation in stable MAC order. OTA start immediately
stops BLE scanning, frees all histories, and clears stale sightings; callbacks
cannot allocate new histories until OTA ends unsuccessfully or the device reboots.

The required iBeacon UUID retains its latest-RSSI fallback if history allocation
fails. See [ESPRESENSE_INTEGRATION.md](ESPRESENSE_INTEGRATION.md) for behavior.
The measurements below describe the previous optimization and are historical.

## Previous implementation

Preserves 45 tracked devices, 45 whitelist entries, the 15-second RSSI window,
10–200 adaptive readings, MQTT payloads, URL capacities, outgoing buffers, task
stacks, installed libraries, and partition layout.

## Allocation bounds

| Allocation | Before | After |
| --- | ---: | ---: |
| Initial 20-reading history | 160 bytes | 100 bytes |
| Maximum 200-reading history | 1,600 bytes | 1,000 bytes |
| 45 maximum histories | 72,000 bytes | 45,000 bytes |
| Runtime plus maximum histories | 117,416 bytes | 90,416 bytes |
| Empty device-table row snapshot | 33,480 bytes | 0 bytes |
| One-device row snapshot | 33,480 bytes | 744 bytes |
| Full 45-device row snapshot | 33,480 bytes | 33,480 bytes |

These bounds exclude allocator overhead, HTTP page objects, HTML fragments, and
SDK/network allocations. Intermediate history capacities round up to four-byte
alignment. Resizing temporarily holds an extra buffer of at most 1,000 bytes.
The existing 49,152-byte BLE heap reserve remains. Filter/snapshot struct sizes
are unchanged.

Histories use one aligned allocation: 32-bit timestamps followed by RSSI bytes.
Statistics still use floating point. Fractional input is rejected rather than
rounded; NimBLE supplies integer RSSI. Failed growth retains the old history.

Device pages allocate for observed occupancy, then copy under the state mutex.
They retry once if occupancy grows beyond capacity, otherwise return 503. Expiry
or configuration changes can reduce the final count. Snapshots remain consistent,
sorted, and owned by the asynchronous response until completion or disconnect.

## HTTP responses

`HTTP_Response.h` bounds decoded body storage using the installed HTTPClient
transfer decoder. It distinguishes complete bodies, truncation, allocation failure,
and transport failure. Diagnostic prefixes include markers within existing text
limits. Short writes terminate oversized transfers rather than draining them.
HTTPClient's headers, chunk headers, receive buffers, TLS allocations, and timeout
behavior remain controlled by the installed core.

Registration accepts complete JSON bodies up to 16 KiB. Buffers grow as needed;
growth temporarily retains old and new buffers. Conversion to the existing
`std::string` return type can briefly hold two body copies. The former intermediate
Arduino `String` copy is removed. Other responses retain only diagnostic prefixes.
This bounds application response-body storage, not total HTTP/SDK heap usage.

Registration rejects oversized/incomplete bodies, malformed JSON, trailing
non-whitespace, invalid URL types/schemes, embedded NULs, and URLs exceeding the
existing buffers. Every URL is validated before any active URL changes; failure
does not save configuration. Only a complete exact `OK (not found)` response
invalidates the cached ping URL.

## Verification

- `build/espresense/run-tests.ps1`: BLE/filter/MQTT/whitelist/web suite, actual
  snapshot allocation sizes, empty pages, concurrent growth/shrink, allocation
  failure, disconnect cleanup, and stack-frame checks.
- `build/espresense/run-heap-tests.ps1`: 12,000-observation comparison against the
  previous filter; bounded HTTP and atomic URL tests; exact installed HTTPClient
  transfer functions with mocked network I/O, covering Content-Length, chunked,
  close-delimited, malformed, incomplete, and oversized responses.
- `build/espresense/run-radar-tests.ps1`: radar fallback with BLE enabled/disabled.

All native checks and both ESP32 builds passed with core 3.3.8, `-Os`, warning
logging, disabled PSRAM, and the existing 1,966,080-byte application partition.
The disabled build adds `compiler.cpp.extra_flags=-DDISABLE_SCAN_BLE`. Source
checks confirmed identical production inputs in the isolated disabled snapshot
and current headers in the enabled build. ESP32 debug information confirms
45,416-byte runtime state, 744-byte rows, and 16-byte filter metadata.

| Build | Flash bytes | Static RAM bytes | Partition space remaining |
| --- | ---: | ---: | ---: |
| BLE enabled | 1,399,244 | 51,908 | 566,836 |
| BLE disabled | 1,035,000 | 38,264 | 931,080 |

The earlier radar-fallback artifact used 1,396,424 flash and 51,884 static RAM
bytes. The current working source also contained intervening publishing telemetry
edits, so the difference is not an isolated measurement of this patch. The heap
savings above follow from verified allocation sizes; they are not a claim of
reduced static RAM or measured on-device free heap.

ELF/map files, linker section sizes, build options, source hashes, and logs are in
[heap-enabled](../build/espresense/heap-enabled) and
[heap-disabled](../build/espresense/heap-disabled). Test output is in
[heap-tests.log](../build/espresense/heap-tests.log),
[heap-specific-tests.log](../build/espresense/heap-specific-tests.log), and
[heap-radar-tests.log](../build/espresense/heap-radar-tests.log).

Real-device free/largest/minimum heap, stalled sockets, concurrent TLS/OTA/web
load, and long-duration fragmentation checks remain pending. No firmware was
uploaded.

