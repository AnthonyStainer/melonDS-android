# Phase 0 physical harness runbook

The physical harness is an Android instrumentation test, not product UI. Run
it only from a build made with:

```sh
bash gradlew -Pmelonds.phase0Multiplayer=true \
  :app:assembleGitHubProdDebug \
  :app:assembleGitHubProdDebugAndroidTest
```

Install both APKs on every selected device:

```sh
adb -s SERIAL install -r app/build/outputs/apk/gitHubProd/debug/app-gitHub-prod-debug.apk
adb -s SERIAL install -r app/build/outputs/apk/androidTest/gitHubProd/debug/app-gitHub-prod-debug-androidTest.apk
```

All devices must be foreground, screen-on, and connected to the same selected
IPv4 Wi-Fi LAN. Record each device's model/API/build fingerprint in
`device-matrix.md` before starting. Determine slot A's selected Wi-Fi IPv4 and
pass it verbatim as `phase0_host_ipv4` to every client.

## Two-device example

Start slot A first as the application host and AID-1 responder:

```sh
adb -s SLOT_A shell am instrument -w -r \
  -e class me.magnum.melonds.impl.network.Phase0PhysicalHarnessTest#runConfiguredPhysicalRole \
  -e phase0_role app_host \
  -e phase0_aid 1 \
  -e phase0_exchanges 10000 \
  -e phase0_output_id two-A \
  me.magnum.melonds.dev.test/androidx.test.runner.AndroidJUnitRunner
```

While A is waiting, start slot B as the logical DS host. The requested AID
mask is bit 1 (`2`):

```sh
adb -s SLOT_B shell am instrument -w -r \
  -e class me.magnum.melonds.impl.network.Phase0PhysicalHarnessTest#runConfiguredPhysicalRole \
  -e phase0_role ds_host \
  -e phase0_host_ipv4 HOST_IPV4 \
  -e phase0_aid_mask 2 \
  -e phase0_exchanges 10000 \
  -e phase0_output_id two-B \
  me.magnum.melonds.dev.test/androidx.test.runner.AndroidJUnitRunner
```

The shell invocations must run concurrently. A terminal per device is the
simplest arrangement.

## Three and four devices

Add responders before starting the DS-host command loop:

```sh
adb -s SLOT_C shell am instrument -w -r \
  -e class me.magnum.melonds.impl.network.Phase0PhysicalHarnessTest#runConfiguredPhysicalRole \
  -e phase0_role responder \
  -e phase0_host_ipv4 HOST_IPV4 \
  -e phase0_aid 2 \
  -e phase0_exchanges 10000 \
  -e phase0_output_id run-C \
  me.magnum.melonds.dev.test/androidx.test.runner.AndroidJUnitRunner
```

For four devices, slot D uses AID 3. The DS-host masks are:

| Devices | Expected responder AIDs | `phase0_aid_mask` |
| ---: | --- | ---: |
| 2 | 1 | 2 |
| 3 | 1, 2 | 6 |
| 4 | 1, 2, 3 | 14 |

Use `phase0_warmup_ms=60000` (the default). Pass
`phase0_low_latency=true` to every role for the low-latency A/B run; omit it
or pass `false` for the control run. Lock support/acquisition is written into
the summary metadata and never changes transport pass/fail by itself.

## Evidence

Each device writes exchange summaries and raw trace events below its external
app files directory:

```text
Android/data/me.magnum.melonds.dev/files/phase0/
```

Pull the entire directory after each run:

```sh
adb -s SERIAL pull /sdcard/Android/data/me.magnum.melonds.dev/files/phase0 evidence/SERIAL/
```

Preserve the exact command lines, APK hashes, device inventory, Wi-Fi/router
description, and pulled-file hashes with the report. Clean, contention/loss,
low-latency-on, and low-latency-off runs use distinct `phase0_output_id`
values and are analyzed separately.
