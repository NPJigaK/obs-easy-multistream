# Easy Multistream

[![CI](https://github.com/NPJigaK/obs-easy-multistream/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/NPJigaK/obs-easy-multistream/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/NPJigaK/obs-easy-multistream)](LICENSE)

A lightweight OBS Studio plugin intended to add YouTube multistreaming without replacing OBS's native Twitch workflow or running a second video encode.

> **Development build:** the YouTube RTMPS output is implemented and passes the repository's automated tests, but a real Twitch + YouTube private-stream validation is still required before the first public release.

## Intended experience

- Keep Twitch configured as the normal OBS service.
- Start and stop from OBS's existing button.
- Send the same encoded H.264/AAC stream to YouTube as well.
- Show Twitch and YouTube independently in a standard OBS dock.
- Keep connection details visible after saving, then let the user collapse them into a compact status view.
- Keep Twitch running if the YouTube connection fails.
- Make browser-based YouTube account connection the normal setup path once its Google review and release requirements are met.
- Keep manual RTMPS URL and stream-key setup as an advanced fallback.
- Avoid cloud relays, telemetry, separate background processes, and a second video encode.

The current development build uses a regular YouTube encoder stream (not a scheduled event): copy the RTMPS Stream URL and a reusable custom stream key from YouTube Studio, then enable Auto-start and Auto-stop. This manual path remains supported as a fallback. The planned normal setup is **Connect YouTube** in the dock, followed by Google authorization in the system browser and selection of an existing reusable encoder stream. Automatically creating and managing YouTube broadcasts, titles, privacy settings, and scheduled events is a separate later feature rather than part of the first account-connection implementation.

YouTube's automatic Dual stream can create a 9:16 feed from the single 16:9 stream, so Easy Multistream does not need a second local video encoder for the recommended setup. Custom encoder-controlled vertical layouts remain outside the current scope.

## Current status

The current build provides:

- an OBS module built with the official plugin-template CMake infrastructure;
- a standard dock registered through the public OBS Frontend API;
- a one-time automatic dock reveal with inline setup guidance, followed by normal OBS-managed dock behavior;
- a one-click **Open YouTube Studio** action that uses the default browser's existing YouTube sign-in without reading browser credentials;
- settings that remain visible after saving, followed by a compact two-destination status view when the user selects **Hide settings** or returns later;
- an additional **Tools → Open Easy Multistream** command that reveals the same dock;
- a profile-scoped YouTube enable setting saved atomically in the active OBS profile;
- a masked stream-key field backed by Windows Credential Manager;
- a validated, profile-scoped YouTube RTMPS Stream URL restricted to YouTube ingestion hosts;
- explicit handling for OBS profile changes, theme changes, dock closure, and shutdown;
- an OBS- and Qt-independent session state model that rejects stale output events;
- one YouTube RTMPS output that reuses the active Twitch H.264 video encoder and main AAC audio encoder;
- independent Twitch and YouTube status, failure isolation, and an explicit YouTube-only retry action;
- an exact-match destination classifier that treats custom and unknown services conservatively;
- unit tests for settings parsing, failed-save rollback, secret validation, the credential backend, and session transitions;
- English and Japanese UI resources;
- explicit OBS-owned widget lifetime handling;
- a pinned Windows x64 build against OBS Studio 32.2.2;
- separate installer-free ZIPs for a regular OBS installation and a portable OBS installation, each with a validated directory layout.

The implementation now creates a YouTube output after OBS confirms that its native Twitch output has started. Automated tests cover configuration, state transitions, stale callbacks, failure isolation, and UI behavior; the remaining release gate is a short real-service test with a private or unlisted YouTube stream. See [the YouTube setup note](docs/youtube-setup.md), [the architecture note](docs/architecture.md), [the output integration contract](docs/runtime-output-design.md), and [the security note](docs/security.md).

## Planned YouTube account connection

The accepted direction is to make account connection the normal setup experience without changing OBS's native Twitch workflow:

1. Select **Connect YouTube** in the Easy Multistream dock.
2. Complete Google authorization in the system browser.
3. Select the YouTube channel and an existing reusable encoder stream when necessary.
4. Return to OBS and continue using OBS's normal **Start Streaming** and **Stop Streaming** controls.

The RTMPS URL and stream key are then obtained through the official YouTube API instead of being copied by the user. The manual fields remain available under advanced settings and are never silently selected after an account-connection failure.

This is not enabled in the current build. The internal browser, token, selection, profile-lifecycle, destination-resolution, and remote-revocation boundaries are being completed and tested before any account control is shown in the dock. Public builds also require an appropriate Google Cloud project, consent screen, privacy policy, OAuth verification decision, and quota plan. See [the accepted account-connection design](docs/youtube-account-connection.md).

## Current manual YouTube setup

Until account connection is available, setup remains a normal OBS workflow. These fields will remain available later as the manual fallback:

1. Select **Open YouTube Studio** in the dock. In the browser, use **Stream** rather than a scheduled event.
2. Create or reuse a custom stream key and turn on YouTube Auto-start and Auto-stop.
3. Copy YouTube's RTMPS **Stream URL** and stream key into Easy Multistream. The server URL is a user-provided setting; it is not hard-coded by the plugin.
4. If vertical discovery in the YouTube Shorts feed is wanted, enable YouTube **Dual stream** in the Live Control Room before going live and leave the vertical layout on **Auto**. YouTube then creates the vertical feed from the horizontal stream, normally using a centre crop.
5. Test the setup as **Private** or **Unlisted** before using it for a public broadcast.

These steps use one YouTube input from OBS. A separately composed 9:16 scene sent through a second local encoder is a different, more expensive workflow and is not part of the initial implementation. See the [detailed YouTube setup note](docs/youtube-setup.md) and the [official YouTube dual-stream instructions](https://support.google.com/youtube/answer/2474026?hl=en).

## Compatibility

- Windows x64
- built against OBS Studio 32.2.2
- OBS's native stream must be Twitch and must use H.264 video plus AAC audio
- Visual Studio 2022 / Windows SDK 10.0.22621 or newer for local builds

The Twitch VOD audio track is supported; Easy Multistream sends OBS's main live audio track to YouTube. Twitch Enhanced Broadcasting/multitrack video, AV1, HEVC, and multiple native video encoders are not supported by the current YouTube output. Earlier OBS 32 versions and OBS 33 or newer have not been claimed or tested yet. ARM64, macOS, and Linux are not currently supported.

## Installation

> **There is no stable public release yet.** Current artifacts are unsigned development builds. Use a dedicated test profile and a private or unlisted YouTube stream until the real-service validation is complete.

Easy Multistream installs like a regular OBS Studio plugin. It does not require MediaMTX, a cloud relay, a separate background process, command-line commands, port forwarding, firewall rules, drivers, or manual editing of OBS configuration files. Installing it does not replace or move your existing OBS Twitch service settings.

### Regular OBS Studio installation

1. Close OBS Studio completely.
2. When a Windows package is published, download the ZIP that does **not** end in `-portable` from the [Releases page](https://github.com/NPJigaK/obs-easy-multistream/releases).
3. Extract the downloaded ZIP.
4. In File Explorer, enter `%ProgramData%\obs-studio\plugins` in the address bar. Create the `plugins` folder if it does not already exist.
5. Copy the extracted `obs-easy-multistream` folder into that `plugins` folder. If Windows asks for administrator permission, choose **Continue**.
6. Start OBS Studio. The **Easy Multistream** dock opens automatically the first time it is shown. After that, OBS respects your dock layout; reopen the same dock at any time from **Docks → Easy Multistream** or **Tools → Open Easy Multistream**.

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
4. Start OBS Studio. The **Easy Multistream** dock opens automatically the first time it is shown in that portable OBS installation. After that, OBS respects your dock layout; reopen the same dock at any time from **Docks → Easy Multistream** or **Tools → Open Easy Multistream**.

Do not install both packages into the same OBS installation. These instructions follow the plugin locations used by the currently supported OBS Studio 32.2.2 target. See the [official OBS plugin guide](https://obsproject.com/kb/plugins-guide) for general information about OBS plugins.

## Uninstallation

### Before removing the plugin

1. Start OBS Studio and open **Docks → Easy Multistream** or **Tools → Open Easy Multistream**.
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
release\obs-easy-multistream-0.4.1-windows-x64-obs32.zip
release\obs-easy-multistream-0.4.1-windows-x64-obs32-portable.zip
```

The regular ZIP contains one `obs-easy-multistream` folder for the standard OBS plugin directory. The `-portable` ZIP is structured to be extracted directly into an OBS Studio 32.2.2 portable root containing `bin\64bit\obs64.exe`. There is no installer yet. Treat local artifacts as test builds until the real Twitch + YouTube validation is recorded.

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
