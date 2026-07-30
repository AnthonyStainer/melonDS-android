# melonDS Android — multiplayer research fork

This repository is a fork of
[rafaelvcaetano/melonDS-android](https://github.com/rafaelvcaetano/melonDS-android),
an Android port of the [melonDS](https://melonds.kuribo64.net/) Nintendo DS and
DSi emulator.

The fork currently carries a non-production Phase 0 experiment for
multi-device local-network multiplayer. The experiment is disabled by default,
has no lobby or release UI, and is paused until its two-, three-, and
four-device physical timing gate can be run. It must not be described as
working or supported multiplayer yet.

- [Phase 0 status and build notes](docs/multiplayer/phase0/README.md)
- [Physical-device runbook](docs/multiplayer/phase0/physical-runbook.md)
- [Measurement protocol](docs/multiplayer/phase0/measurement-protocol.md)
- [OpenSpec change package](openspec/changes/add-android-multi-device-multiplayer/)
- [Upstream Android project](https://github.com/rafaelvcaetano/melonDS-android)

## Project status

Normal emulator builds retain upstream behavior. The Phase 0 code is included
only when `melonds.phase0Multiplayer=true` is passed to Gradle; it is not an
end-user feature.

At the current checkpoint:

- the selected-network transport, minimal protocol, core adapter, relay,
  instrumentation, and content-free physical harness are implemented;
- strict OpenSpec validation, host-native tests, and Android debug builds pass;
- physical device assignment and OpenSpec tasks 4.4–4.12 remain open; and
- all post-gate protocol, product UI, hardening, and release work remains
  blocked on the recorded topology decision.

This fork does not currently publish an end-user release. Use the
[upstream releases](https://github.com/rafaelvcaetano/melonDS-android/releases)
for the established Android application.

## Screenshots

| ROM list | Dark theme | Pocket Physics | Layout editor |
| --- | --- | --- | --- |
| ![ROM list](./.github/images/screenshot_mobile0.png) | ![Dark theme](./.github/images/screenshot_mobile1.png) | ![Pocket Physics](./.github/images/screenshot_mobile2.png) | ![Layout editor](./.github/images/screenshot_mobile3.png) |

## Known upstream limitations

- Local multiplayer is not available in normal application builds.
- DSi SD card support is incomplete.
- Button skins are not customizable.
- Display-filter selection is limited.

## Building

### Prerequisites

- JDK 21;
- Android SDK platform 36 and build-tools 36.0.0;
- Android NDK 28.0.13004108;
- CMake 3.22.1; and
- Git with submodule support.

Clone this fork recursively:

```sh
git clone --recurse-submodules \
  https://github.com/AnthonyStainer/melonDS-android.git
cd melonDS-android
```

Build and test the normal debug variant:

```sh
./gradlew \
  :app:testGitHubProdDebugUnitTest \
  :app:assembleGitHubProdDebug
```

The APK is written to:

```text
app/build/outputs/apk/gitHubProd/debug/app-gitHub-prod-debug.apk
```

Windows users can run the same tasks through `gradlew.bat`.

### Phase 0 multiplayer build

The feasibility code is opt-in and does not create a production multiplayer
experience:

```sh
./gradlew -Pmelonds.phase0Multiplayer=true \
  :app:assembleGitHubProdDebug \
  :app:assembleGitHubProdDebugAndroidTest
```

Do not begin post-gate work or raise `qualifiedMaxPlayers` until the evidence
required by the [measurement protocol](docs/multiplayer/phase0/measurement-protocol.md)
has been recorded.

### Release signing

Release builds require these entries in the untracked `local.properties` file:

```properties
MELONDS_KEYSTORE=/path/to/keystore
MELONDS_KEYSTORE_PASSWORD=...
MELONDS_KEY_ALIAS=...
MELONDS_KEY_PASSWORD=...
```

Never commit signing files or credentials.

## Repository layout

| Path | Purpose |
| --- | --- |
| `app/` | Android application, JNI frontend, and Android tests |
| `common/` | Shared Kotlin code |
| `masterswitch/` | Master-switch Android library |
| `rcheevos-api/` | RetroAchievements API module |
| `melonDS-android-lib/` | Project-owned native-core submodule |
| `app/src/main/cpp/enet/` | Pinned ENet submodule |
| `docs/multiplayer/phase0/` | Physical gate, status, and run instructions |
| `openspec/changes/` | Reviewed change specifications and task ledger |

Keep the parent and native-core commits synchronized: a parent change that
depends on core work must update the `melonDS-android-lib` gitlink to a commit
reachable from the project-owned core fork.

## Third-party frontend integration

ROMs must already have been scanned by melonDS.

- Package: `me.magnum.melonds`
- Activity: `me.magnum.melonds.ui.emulator.EmulatorActivity`
- Preferred input: an Intent data URI with read permission granted
- Deprecated inputs: the `uri` SAF string extra or the absolute `PATH` extra

Pegasus metadata:

- [Stable metadata](./.github/pegasus/melonds.metadata.txt)
- [Nightly metadata](./.github/pegasus/melonds-nightly.metadata.txt)

When “Save next to ROM file” is enabled but melonDS does not have permission
to create the save beside a newly supplied ROM, it falls back to:

```text
Android/data/me.magnum.melonds/files/saves
```

## Performance

Performance is best on 64-bit devices with threaded rendering and JIT enabled.
Older 32-bit devices can be substantially slower because JIT support is not
available there.

## Contributing

Keep production behavior unchanged while the multiplayer build flag is off.
For multiplayer work, update the OpenSpec task ledger and preserve the physical
gate boundary. Automated fixtures must remain content-free; ROMs, firmware,
BIOS data, keys, and captured retail traffic do not belong in this repository.

The native-core submodule has its own
[contribution guide](melonDS-android-lib/CONTRIBUTING.md).

## License and attribution

See [LICENSE](LICENSE). This fork remains derived from
[rafaelvcaetano/melonDS-android](https://github.com/rafaelvcaetano/melonDS-android)
and the upstream [melonDS](https://github.com/melonDS-emu/melonDS) project.
