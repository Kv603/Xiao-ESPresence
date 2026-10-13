# WS2812B groups with wildcard MQTT and RGB channel updates

## Summary

Add persistent LED configuration through the existing MQTT control interface. Support wildcard subscriptions, additive topic mixing, and numeric `/red`, `/green`, and `/blue` messages that replace only the specified group channel.

For example, a group displaying `(0,255,0)` receiving `sensor/lobby/red` with payload `100` becomes `(255,255,0)`, before general strip brightness is applied.

## Strip and scheduling

- Use LiteLED 3.2.0’s RMT-backed `LiteLED` class, `LED_STRIP_WS2812`, RGBW disabled, and GRB order. [LiteLED documentation](https://github.com/Xylopyrographer/LiteLED).
- Provide overrideable defines: `LED_STRIP_PIN=2`, `LED_STRIP_COUNT=51`, `LED_SKIP_FIRST=1`, `LED_GROUP_WIDTH=3`, and `LED_GROUP_GAP=1`.
- Validate count as 1–51, width as positive, gap as nonnegative, and skip-first as boolean.
- Number groups from 1; include a partial final group. Defaults produce 13 groups, with group 13 on pixels 49–50. Skipped and gap pixels remain black.
- Initialize off, with general brightness 255. Maintain canonical RGB separately from the driver buffer; apply general brightness using `brightness()`.
- Render changed frames at up to 50 Hz. Replace the one-second blocking loop delay with timed BLE/Home Assistant servicing at its existing cadence; service fades during MQTT connection and acknowledgment waits.
- Clear transient LED state and turn the strip off when OTA starts. Expose driver failures without stopping BLE/MQTT.

## Configuration and subscriptions

Extend non-retained `/config/set` with:

```json
{
  "id": "led-1",
  "operation": "led",
  "revision": 0,
  "brightness": 200,
  "groups": [
    {"group": 1, "topics": ["sensor/lobby/#", "house/alert"]},
    {"group": 13, "topics": ["sensor/door/+"]}
  ]
}
```

- Use existing settings revisions and `/config/result`. Brightness accepts integer 0–255; omitted fields remain unchanged.
- Each supplied group replaces its filter list. An empty list clears it; unlisted groups remain unchanged.
- Support exact topics and valid MQTT `+`/`#` filters, up to 160 bytes each and 64 group/filter assignments total. Reject duplicate assignments and malformed filters.
- Match standard MQTT semantics: `sensor/lobby/#` includes `sensor/lobby` and all descendants; preserve case sensitivity and the special `$` topic rules. [MQTT specification](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/csprd02/mqtt-v3.1.1-csprd02.html).
- Subscribe once per distinct filter at QoS 1. Reconcile removals/additions after configuration changes, restore subscriptions on reconnect, and retry failed requests.
- For each received message, update every matching group once, even if several filters match it. Always dispatch actual control topics and `homeassistant/status` to their existing handlers; exclude those messages from LED processing.
- Persist brightness and mappings atomically in the existing settings blob and expose them through `/config/get`. Old settings load LED defaults. Preserve the 16,000-byte settings limit and retain prior configuration on validation or save failure.
- Ignore retained LED values. Keep live values and channel overrides in RAM only.

## Mixing, channel updates, and decay

- Ordinary numeric payloads accept finite 0–100 values, including fractions, and contribute white at that percentage.
- Ordinary color payloads accept trimmed plain text or JSON strings, normalized to lowercase. Support [CSS named colors](https://www.w3.org/TR/css-color-4/#named-colors), plus `on` = white, `off` = black, and `brickred` = firebrick (`#B22222`).
- Named colors start at 75%. Black, off, and numeric zero clear that concrete topic’s contribution.
- Track ordinary contributions by concrete incoming topic, rather than subscription filter. Sum contributions for each matching group and cap RGB channels at 255.
- Ordinary non-red contributions fade independently to zero over 60 seconds. Only named `red` and `brickred` persist. Valid new messages restart only their own source’s fade.
- For numeric messages whose final topic level is exactly `red`, `green`, or `blue`, set that channel of each matching group to `round(255 × percentage / 100)`. Preserve the other current channels and their timers.
- Implement channel updates as group-level overrides applied after ordinary mixing and before strip brightness. Red overrides persist; green/blue overrides fade independently to zero over 60 seconds. An override remains active at zero, preventing older contributions from reappearing.
- A subsequent ordinary numeric or named-color message clears all channel overrides for its matching groups and resumes normal additive mixing with that updated source.
- Color-name payloads on `/red`, `/green`, or `/blue` use ordinary color behavior; channel-specific handling applies only to numeric payloads.
- Reject invalid payloads without changing state. Preserve message order using a separate bounded FIFO of 32 parsed LED events, with receipt timestamps and synchronized callback access.
- Bound concrete ordinary source tracking to 64 topics. Reclaim expired, unreferenced slots; reject additional sources when full and expose overflow counters. Use timer-wrap-safe elapsed calculations.

## Verification and delivery

- Test default and alternative layouts, partial groups, parsing, saturation, independent decay, restart, persistent red, and timer wraparound.
- Test wildcard parent/descendant matching, `+`, invalid filters, `$` rules, overlapping filters, and multiple-group fanout.
- Verify green followed by `/red` preserves green; channel zero affects only that channel; green/blue channel fades preserve other timers; ordinary messages clear overrides.
- Extend persistence and MQTT tests for revisions, old settings, failed saves, subscription reconciliation/reconnect, fragments, retained rejection, ordered events, and capacity limits.
- Run existing regressions and compile for XIAO ESP32-C6 using installed Arduino-ESP32 3.3.12, LiteLED 3.2.0, and `ble_ota4m`; retain 64 KiB application headroom.
- Update README with dependencies, defines, configuration examples, wildcard/channel behavior, limits, and physical validation steps. Flashing remains a separate deployment step.
