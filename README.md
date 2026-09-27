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
- Avoid OAuth, cloud relays, telemetry, separate background processes, and a second video encode unless they become necessary.

The initial release will use a reusable YouTube custom stream key with YouTube Auto-start and Auto-stop. Scheduled YouTube events and per-stream metadata automation are outside the initial scope.

## Current status

The current build provides:

- an OBS module built with the official plugin-template CMake infrastructure;
- a standard dock registered through the public OBS Frontend API;
- a profile-scoped YouTube enable setting saved atomically in the active OBS profile;
- a masked stream-key field backed by Windows Credential Manager;
- explicit handling for OBS profile changes, theme changes, dock closure, and shutdown;
- an OBS- and Qt-independent session state model that rejects stale output events;
- an exact-match destination classifier that treats custom and unknown services conservatively;
- unit tests for settings parsing, failed-save rollback, secret validation, the credential backend, and session transitions;
- English and Japanese UI resources;
- explicit OBS-owned widget lifetime handling;
- a pinned Windows x64 build against OBS Studio 32.2.2;
- separate installer-free ZIPs for a regular OBS installation and a portable OBS installation, each with a validated directory layout.

It does **not** connect to YouTube yet, so saving these settings cannot start a YouTube stream. See [the architecture note](docs/architecture.md) for the implementation boundaries and rollout order, [the output integration contract](docs/runtime-output-design.md) for the future OBS ownership rules, and [the security note](docs/security.md) for the credential and memory boundaries.

## Compatibility

- Windows x64
- built against OBS Studio 32.2.2
- Visual Studio 2022 / Windows SDK 10.0.22621 or newer for local builds

Earlier OBS 32 versions and OBS 33 or newer have not been claimed or tested yet. ARM64, macOS, and Linux are not currently supported.

## Installation

> **There is no stable public release yet.** The current builds are unsigned development builds for testing, and YouTube streaming is not implemented yet.

Easy Multistream installs like a regular OBS Studio plugin. It does not require MediaMTX, a cloud relay, a separate background process, command-line commands, port forwarding, firewall rules, drivers, or manual editing of OBS configuration files. Installing it does not replace or move your existing OBS Twitch service settings.

### Regular OBS Studio installation

1. Close OBS Studio completely.
2. When a Windows package is published, download the ZIP that does **not** end in `-portable` from the [Releases page](https://github.com/NPJigaK/obs-easy-multistream/releases).
3. Extract the downloaded ZIP.
4. In File Explorer, enter `%ProgramData%\obs-studio\plugins` in the address bar. Create the `plugins` folder if it does not already exist.
5. Copy the extracted `obs-easy-multistream` folder into that `plugins` folder. If Windows asks for administrator permission, choose **Continue**.
6. Start OBS Studio, then open **Docks → Easy Multistream**.

If the Easy Multistream dock opens, the plugin is installed. The expected result is:

```text
C:\ProgramData\obs-studio\plugins\obs-easy-multistream\
  bin\64bit\obs-easy-multistream.dll
  data\locale\...
```

### Portable OBS Studio installation

Use the ZIP whose name ends in `-portable` only if you already run OBS Studio in [portable mode](https://obsproject.com/kb/portable-mode).

1. Close OBS Studio completely.
2. When a Windows package is published, download the `-portable` ZIP from the [Releases page](https://github.com/NPJigaK/obs-easy-multistream/releases).
3. Extract the contents of the ZIP into the root of your portable OBS Studio folder—the folder that contains `bin\64bit\obs64.exe`.
4. Start OBS Studio, then open **Docks → Easy Multistream**.

Do not install both packages into the same OBS installation. These instructions follow the plugin locations used by the currently supported OBS Studio 32.2.2 target. See the [official OBS plugin guide](https://obsproject.com/kb/plugins-guide) for general information about OBS plugins.

## Uninstallation

### Before removing the plugin

1. Start OBS Studio and open **Docks → Easy Multistream**.
2. If a YouTube stream key is saved, select **Remove saved key** and confirm. This removes the key from Windows Credential Manager.
3. Close OBS Studio completely.

### Regular OBS Studio installation

Delete only this Easy Multistream folder:

```text
C:\ProgramData\obs-studio\plugins\obs-easy-multistream
```

### Portable OBS Studio installation

Delete only these Easy Multistream files and folders from the portable OBS Studio folder:

```text
obs-plugins\64bit\obs-easy-multistream.dll
data\obs-plugins\obs-easy-multistream
obs-easy-multistream-LICENSE.txt
```

Do **not** delete the parent `plugins`, `obs-plugins`, or `data` folders; other OBS plugins may use them.

If the plugin was removed before its saved key, reinstall it temporarily and use **Remove saved key**, or remove the Easy Multistream entry with Windows Credential Manager. Easy Multistream does not install a Windows service, scheduled task, driver, MediaMTX, or any other separate program, so there is nothing else to stop or uninstall. Any non-secret Easy Multistream preference left in an OBS profile is harmless and ignored while the plugin is absent; do not edit OBS profile files manually.

## Build locally

The build is self-contained inside this repository. It downloads pinned OBS, Qt, and OBS dependency archives into `.deps`; it does not install packages or modify the global `PATH`.

```powershell
.\scripts\build-windows.ps1
```

The first build also compiles the OBS development targets and can take several minutes. A successful build creates both packages:

```text
release\obs-easy-multistream-0.1.0-windows-x64-obs32.zip
release\obs-easy-multistream-0.1.0-windows-x64-obs32-portable.zip
```

The regular ZIP contains one `obs-easy-multistream` folder for the standard OBS plugin directory. The `-portable` ZIP is structured to be extracted directly into an OBS Studio 32.2.2 portable root containing `bin\64bit\obs64.exe`. There is no installer yet, and the plugin should not be used for multistreaming until YouTube streaming is implemented.

Local and CI artifacts are unsigned development builds. A public release process and code-signing policy will be defined before recommending the plugin to general users.

## Development principles

- public libobs and Frontend APIs only;
- no modification of OBS private UI internals;
- one bounded secondary output for the first supported multistream workflow;
- no secret values in logs, diagnostics, Git, or exported profiles;
- runtime state, OBS integration, and output ownership remain separate layers;
- deterministic cleanup before feature breadth;
- source-pinned and reviewable release builds.

## License

Easy Multistream is licensed under the GNU General Public License, version 2 or later. See [LICENSE](LICENSE).

Easy Multistream is an independent community plugin and is not affiliated with or endorsed by the OBS Project, Twitch, Google, or YouTube.
