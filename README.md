# Easy Multistream

[![CI](https://github.com/NPJigaK/obs-easy-multistream/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/NPJigaK/obs-easy-multistream/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/NPJigaK/obs-easy-multistream)](LICENSE)

A lightweight OBS Studio plugin intended to add YouTube multistreaming without replacing OBS's native Twitch workflow or running a second video encode.

> **Pre-alpha:** the repository currently contains a profile-aware configuration preview. It does not stream to YouTube yet.

## Intended v1 experience

- Keep Twitch configured as the normal OBS service.
- Start and stop from OBS's existing button.
- Send the same encoded H.264/AAC stream to one additional YouTube RTMPS destination.
- Show Twitch and YouTube independently in a standard OBS dock.
- Keep Twitch running if the YouTube connection fails.
- Avoid OAuth, cloud relays, telemetry, installers, and a second video encode in v1.

The first version will use a reusable YouTube custom stream key with YouTube Auto-start and Auto-stop. Scheduled YouTube events and per-stream metadata automation are not part of v1.

## Current status

The current preview provides:

- an OBS module built with the official plugin-template CMake infrastructure;
- a standard dock registered through the public OBS Frontend API;
- a profile-scoped YouTube enable setting saved atomically in the active OBS profile;
- a masked stream-key field backed by Windows Credential Manager;
- explicit handling for OBS profile changes, theme changes, dock closure, and shutdown;
- unit tests for settings parsing, failed-save rollback, secret validation, and the credential backend;
- English and Japanese UI resources;
- explicit OBS-owned widget lifetime handling;
- a pinned Windows x64 build against OBS Studio 32.2.2;
- an installer-free portable ZIP with a validated directory layout.

It intentionally does **not** create a service, output, encoder, network connection, or Start/Stop hook yet. Entering settings in this preview cannot start a YouTube stream. See [the architecture note](docs/architecture.md) for the boundaries and rollout order and [the security note](docs/security.md) for the credential and memory boundaries.

## Compatibility

- Windows x64
- built against OBS Studio 32.2.2
- Visual Studio 2022 / Windows SDK 10.0.22621 or newer for local builds

Earlier OBS 32 versions have not been claimed or tested yet. ARM64, macOS, and Linux are not currently supported.

## Build locally

The build is self-contained inside this repository. It downloads pinned OBS, Qt, and OBS dependency archives into `.deps`; it does not install packages or modify the global `PATH`.

```powershell
.\scripts\build-windows.ps1
```

The first build also compiles the OBS development targets and can take several minutes. A successful build creates:

```text
release\obs-easy-multistream-0.1.0-windows-x64-obs32-portable.zip
```

The ZIP is structured to be extracted directly into an OBS portable root containing `bin\64bit\obs64.exe`. There is no installer and no release should be treated as usable for multistreaming until the pre-alpha notice is removed.

Local and CI artifacts are unsigned development builds. A public release process and code-signing policy will be defined before recommending the plugin to general users.

## Development principles

- public libobs and Frontend APIs only;
- no modification of OBS private UI internals;
- one bounded secondary output in v1;
- no secret values in logs, diagnostics, Git, or exported profiles;
- deterministic cleanup before feature breadth;
- source-pinned and reviewable release builds.

## License

Easy Multistream is licensed under the GNU General Public License, version 2 or later. See [LICENSE](LICENSE).

Easy Multistream is an independent community plugin and is not affiliated with or endorsed by the OBS Project, Twitch, Google, or YouTube.
