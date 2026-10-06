# Private-address MQTT eligibility

For observations whose NimBLE address satisfies `isRpa() || isNrpa()`, publishing
now requires either:

- The selected fingerprint identity is a decoded iBeacon UUID/major/minor,
  AltBeacon identifier, or Eddystone UID; or
- A configured IRK actually resolves the observed address.

An advertised name, manufacturer/model signature, service UUID, Eddystone URL/TLM,
MAC-derived fingerprint, or alias attached only to a MAC does not establish
identity across address changes. Merely having an IRK on a matched configuration
entry is insufficient. IRK-based history merging likewise requires resolution.

The identity flag follows the selected fingerprint, preserving existing precedence.
Later sparse advertisements from the same address retain previously learned beacon
identity. A new private address without identity data cannot inherit it. Stable
beacon identifiers are logical advertised identities, not proof of physical-device
uniqueness or cryptographic authentication.

The guard applies both to reporting eligibility and the MQTT publication function.
Suppressed observations remain tracked and visible in the device table. Public
and static-random address behavior, MQTT payloads, reporting limits, and whitelist
configuration are unchanged.

The native core/runtime/web/stack suite passed, including RPA/NRPA suppression,
MAC aliases, unresolved keys, validated beacon identities, sparse advertisements,
rotating resolved IRKs, and public/static-random regression cases. Native fingerprint
and snapshot sizes remain 452 and 744 bytes. Results are in
[private-address-tests.log](../build/espresense/private-address-tests.log).

No new full ESP32 build, firmware upload, or hardware test was performed for this
follow-up; earlier firmware artifacts predate this guard.
