# Phase 0 physical-device matrix

Status at the 2026-07-30 pause checkpoint: awaiting physical devices. No
device slot has been qualified or assigned.

The wire protocol represents up to 16 members, but the initial physical
qualification ceiling is:

```text
protocolMaximumPlayers = 16
qualifiedMaxPlayers = 4
allowed session capacity = 2..4
```

No UI, host validation, discovery advertisement, or release claim may exceed
`qualifiedMaxPlayers`.

## Device inventory

Record the exact model, OEM, Android API level, SoC/ABI, Wi-Fi chipset when
available, build fingerprint, and app version below before physical runs.
The four assigned devices must cover at least two OEM Wi-Fi implementations
and more than one Android API level. API 29+ coverage is mandatory for the
low-latency Wi-Fi A/B experiment; API 36 coverage is preferred for current
target-SDK behaviour.

| Slot | Required role coverage | Model/OEM | API | ABI | Build fingerprint | Assignment |
| --- | --- | --- | --- | --- | --- | --- |
| A | Application host only during the primary run | pending physical assignment | pending | ARM64 | pending | blocked: no attached device |
| B | Logical DS host during the primary run | pending physical assignment | pending | ARM64 | pending | blocked: no attached device |
| C | Responder in three- and four-device runs | pending physical assignment | pending | ARM64 | pending | blocked: no attached device |
| D | Responder in four-device run | pending physical assignment | pending | ARM64 | pending | blocked: no attached device |

This inventory is intentionally not fabricated from emulator profiles. A slot
is selected only after its physical device data has been captured.

## Required combinations

For each device-count run, slot A remains the application host and must not be
the logical DS host:

| Device count | Application host | Logical DS host | Responders |
| ---: | --- | --- | --- |
| 2 | A | B | A |
| 3 | A | B | A, C |
| 4 | A | B | A, C, D |

Repeat the clean two-device run with A and B exchanged to expose asymmetric
OEM/network behaviour. Rotate C and D into the logical-DS-host role during
the required OEM/API combinations while keeping the application host on a
different physical device.

All devices use the same selected IPv4 Wi-Fi LAN. Mobile data may remain
available, and one validation run deliberately places a VPN/default network
outside the selected Wi-Fi network to verify per-socket routing.
