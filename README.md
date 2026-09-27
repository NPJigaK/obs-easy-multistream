# Easy Multistream

[![CI](https://github.com/NPJigaK/obs-easy-multistream/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/NPJigaK/obs-easy-multistream/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/NPJigaK/obs-easy-multistream)](LICENSE)

A lightweight OBS Studio plugin intended to add YouTube multistreaming without replacing OBS's native Twitch workflow or running a second video encode.

> **Work in progress:** the settings interface is available, but streaming to YouTube is not implemented yet.

## Intended experience

- Keep Twitch configured as the normal OBS service.
- Start and stop from OBS's existing button.
- Send the same encoded H.264/AAC stream to YouTube as well.
- Show Twitch and YouTube independently in a standard OBS dock.
- Keep Twitch running if the YouTube connection fails.
- Avoid OAuth, cloud relays, telemetry, installers, and a second video encode unless they become necessary.

The initial release will use a reusable YouTube custom stream key with YouTube Auto-start and Auto-stop. Scheduled YouTube events and per-stream metadata automation are outside the initial scope.

## Current status

The current build provides:

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

It does **not** connect to YouTube yet, so saving these settings cannot start a YouTube stream. See [the architecture note](docs/architecture.md) for the implementation boundaries and rollout order and [the security note](docs/security.md) for the credential and memory boundaries.

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

The ZIP is structured to be extracted directly into an OBS portable root containing `bin\64bit\obs64.exe`. There is no installer, and the plugin should not be used for multistreaming until YouTube streaming is implemented.

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
