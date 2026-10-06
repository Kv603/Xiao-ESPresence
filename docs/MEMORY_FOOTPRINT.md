# ESP32-POE footprint optimization

These measurements precede the NimBLE migration. See
[NIMBLE_MIGRATION.md](NIMBLE_MIGRATION.md) for the newer scanner build and sizes.

Measured with Arduino CLI 1.3.1 and the installed ESP32 Arduino core 3.3.8. The
baseline was compiled from the current BLE-enabled source; the older saved
1,034,912-byte firmware was not used for comparisons.

## Build configuration

All comparisons use the same installed libraries, `-Os`, warning-level logging,
disabled PSRAM, 4 MB flash, and the existing custom partition table. Each OTA
application slot is 1,966,080 bytes. No libraries or board packages were upgraded.

```powershell
arduino-cli compile --fqbn 'esp32:esp32:esp32-poe:UploadSpeed=921600,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=min_spiffs,DebugLevel=warn,PSRAM=disabled,EraseFlash=none' --build-path "$env:TEMP\mmwave-footprint-baseline" .
```

The sketch still opens and builds normally in Arduino IDE. The verification
scripts are optional development tools, not sketch dependencies.

## Measurements

All four Arduino builds completed successfully. Each change reduced compiled
flash without increasing static RAM.

| Build | Arduino flash bytes | Static RAM bytes | Flash saved by this step | RAM saved by this step |
| --- | ---: | ---: | ---: | ---: |
| Baseline | 1,842,872 | 57,196 | — | — |
| Shared page handlers | 1,841,012 | 57,196 | 1,860 | 0 |
| HTML whitespace | 1,840,868 | 57,196 | 144 | 0 |
| Uptime replacement (final) | 1,840,228 | 57,140 | 640 | 56 |
| **Total saved** | | | **2,644** | **56** |

The flash reduction is **0.143%**, with all enabled features retained. Arduino's
display remains 93% flash and 17% RAM. The final application has 125,852
bytes of slot headroom and leaves 270,540 bytes of RAM before runtime allocations.

The raw firmware `.bin` decreased from 1,843,024 to **1,840,384 bytes**, a saving
of 2,640 bytes. Its size differs slightly from Arduino's section totals because
of image metadata and alignment. The final firmware is saved at
`build/footprint/uptime/mmWaveSense_2026_09_09_codex.ino.bin`.

Detailed ELF sections, build options, firmware binaries, and linker maps are
saved in `build/footprint/{baseline,routes,html,uptime}/`. The measurement script
uses the same section-selection rules as the installed core's Arduino size
recipe; debug sections in the ELF file are not counted as firmware flash.

## Changes

- Eight page-only GET routes share one callback implementation. The callback
  captures a pointer to a static embedded page; route order, authentication,
  content type, and the existing template processor are preserved. Routes with
  side effects retain their original implementations.
- Embedded HTML collapses redundant whitespace outside tags and protected
  regions. Tags, attributes, template placeholders, and `<pre>` content are
  unchanged. Line breaks remain for readability, and pages remain embedded.
- The health-check fallback uptime text uses `esp_timer_get_time()` and one
  bounded 64-byte formatting buffer. It keeps the existing
  `x days, y hours, z minutes, s seconds` text, including plural labels for 1.
  The uptime library is no longer required. The monotonic clock avoids
  dependence on wall-clock corrections or `millis()` rollover polling.

Feature definitions, MQTT payload construction outside this uptime fallback,
NVS keys, credentials, partitions, logging level, BLE, LEDs, TLS, OTA, core dumps,
and time synchronization are unchanged.

## Verification

Run the source and native C++ checks from the sketch directory:

```powershell
node build/footprint/verify.mjs
```

These checks require Node.js and a native `g++` compiler on PATH. They validate:

- All ten templates: exact tags/attributes, placeholders, protected content,
  and equivalent HTML whitespace collapsing. This includes form fields/actions
  and refresh attributes.
- All eight page mappings, unchanged other handlers, and unchanged route order.
- The actual shared C++ callback, executed with a stub server/request to check
  authentication setup, response type, status, and template processor.
- The actual uptime formatter, executed with a stub monotonic clock against
  thirteen fixed expected results: zero/subsecond values, minute/hour/day
  boundaries, the 32-bit `millis()` rollover, 100 days, and `INT64_MAX`
  microseconds. Every result fits the 64-byte buffer.
- No unrelated edits in the main sketch or header beyond the specified changes.

All checks passed. `node build/footprint/summarize.mjs` also verified identical
FQBN, optimization flags, build properties, platform/library locations, and
partition contents across all four builds. Final ELF symbol inspection confirmed
that `uptime::` and `uptime_formatter::` code is no longer linked.

Native tests use host stubs; they do not exercise ESP32 networking or hardware.
Browser screenshots, live form submissions, and device smoke tests for radar,
MQTT, HTTP/HTTPS, BLE, LEDs, OTA, and core-dump download were not performed.
No firmware was uploaded. Hardware checks remain for a separately authorized
deployment of this firmware.
