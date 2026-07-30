# Multi-device multiplayer Phase 0

Phase 0 is a non-production feasibility slice for measuring whether a
candidate application-host star relay can satisfy the existing melonDS core
multiplayer deadline on physical Android devices. It does not expose a lobby
or make a release-support claim.

## Pause checkpoint

Implementation was intentionally paused on 2026-07-30 while physical Android
devices are made available. The device-independent slice is complete through
task 4.3:

- tasks 1.1..1.7 and 1.9..1.10;
- tasks 2.1..2.12;
- tasks 3.1..3.13;
- tasks 4.1..4.3.

Task 1.8 and tasks 4.4..4.12 remain open because no physical devices were
attached at the checkpoint. Sections 5..10 are post-gate work and have not
been started. They must remain gated until the physical report records a
topology decision.

The checkpoint passed:

- strict OpenSpec validation;
- the default feature-disabled Android unit/build baseline;
- the feature-enabled Android unit, application APK, and instrumentation APK
  builds for the configured `armeabi-v7a`, `arm64-v8a`, and `x86_64` targets;
- all three host-native protocol, ENet-worker, and core/router test suites;
- `git diff --check` in the Android parent and native-core repositories.

Resume by assigning the four physical slots in
[device-matrix.md](device-matrix.md), then follow
[physical-runbook.md](physical-runbook.md). Do not mark the physical tasks
complete without retaining the raw traces and device/network metadata required
by [measurement-protocol.md](measurement-protocol.md).

## Repository baselines

| Repository | `origin` | `upstream` | Pinned baseline | Working branch |
| --- | --- | --- | --- | --- |
| Android app | `https://github.com/AnthonyStainer/melonDS-android.git` | `https://github.com/rafaelvcaetano/melonDS-android.git` | `ae8790bdb4bf01555c2f168ab9bc77fbeb5dceb6` | `feature/multi-device-phase0` |
| Native core | `https://github.com/AnthonyStainer/melonDS-android-lib.git` | `https://github.com/rafaelvcaetano/melonDS-android-lib.git` | `431ab4bd0003c4356e25fce89640ae1004579b9b` | `feature/multi-device-phase0` |
| ENet submodule | `https://github.com/lsalzman/enet.git` | n/a | `2662c0de09e36f2a2030ccc2c528a3e4c9e8138a` | detached submodule |

The parent `.gitmodules` entry points at the project-owned native-core fork.
The native-core gitlink and pinned ENet gitlink remain unchanged from the
reviewed baselines.

## Build flag

The feasibility slice is disabled by default:

```properties
melonds.phase0Multiplayer=false
```

Set `-Pmelonds.phase0Multiplayer=true` for a spike build. Gradle exposes the
same decision as `BuildConfig.PHASE0_MULTIPLAYER_ENABLED` and passes
`MELONDS_PHASE0_MULTIPLAYER=1` to native code. The flag may compile only the
content-free test path, instrumentation, and transport/controller support
required by Phase 0. It must not expose production lobby or release UI.

## Reproducible baseline

The baseline was validated on 2026-07-29 with:

- Eclipse Temurin JDK 21.0.11+10;
- Android SDK platform 36 and build-tools 36.0.0;
- Android NDK 28.0.13004108;
- CMake 3.22.1.

From the parent repository:

```sh
JAVA_HOME=/path/to/jdk-21 \
ANDROID_HOME=/path/to/android-sdk \
ANDROID_SDK_ROOT=/path/to/android-sdk \
bash gradlew --no-daemon clean \
  :app:testGitHubProdDebugUnitTest \
  :app:assembleGitHubProdDebug
```

The command completed successfully. The resulting APK contains native
frontends for `armeabi-v7a`, `arm64-v8a`, and `x86_64`; ARM64 and x86-64 are
the required Phase 0 baseline targets. Existing compiler and Android API
deprecation warnings remain, with no new baseline failure.

JDK 21 is required. Running Gradle under a newer host JDK can make Java and
Kotlin choose different bytecode targets before application compilation.

## Content-free workflow

The physical workflow is named **Phase 0 Core Relay Harness**. It drives the
real native multiplayer boundary:

```text
SendCmd -> relayed RecvHostPacket -> SendReply -> relayed RecvReplies -> SendAck
```

The harness generates deterministic synthetic internal DS Wi-Fi frames whose
declared lengths and AID fields satisfy the real core adapter contract. It
does not load or distribute ROMs, firmware, BIOS data, keys, game assets, or
captured retail traffic. Measurements therefore exercise the actual adapter,
queues, worker, selected Android network, ENet sockets, and relay topology
without copyrighted content.

## Android permission note

Phase 0 declares `android.permission.WAKE_LOCK` before experimenting with
`WIFI_MODE_FULL_LOW_LATENCY`. `WAKE_LOCK` is a normal install-time permission,
not a runtime prompt. Low-latency lock acquisition is API-29+, best-effort,
controller-owned, and limited to foreground/screen-on measurement runs. A
failed or unsupported acquisition is recorded but does not fail transport.

See [device-matrix.md](device-matrix.md) for physical roles and
[measurement-protocol.md](measurement-protocol.md) for the gate, and
[physical-runbook.md](physical-runbook.md) for the adb-driven harness.
