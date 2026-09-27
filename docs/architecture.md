# Architecture

Easy Multistream is being built in small, reviewable slices. The current slice adds profile-aware configuration and secure local credential storage to the verified OBS dock. It deliberately contains no streaming output code.

## v1 product boundary

The planned first useful release is intentionally narrow:

- Windows x64 and OBS Studio 32
- the native OBS Twitch output remains the primary output
- one additional YouTube RTMPS output
- reuse of the primary H.264 and AAC encoders, with no second video encode
- one OBS Start/Stop workflow
- YouTube failure isolation: a secondary failure must not stop Twitch
- one manually configured YouTube custom stream key, with YouTube Auto-start and Auto-stop
- a standard OBS dock using public Frontend and Qt APIs

OAuth, scheduled YouTube events, three or more destinations, custom output scenes, per-destination transcoding, and Twitch-as-secondary are outside the v1 boundary.

## Current slice

```text
OBS module entry point
  └─ PluginState (plugin-owned)
      ├─ SettingsController (plugin-owned, UI-thread only)
      │   ├─ active profile config (borrowed only during each call)
      │   └─ WindowsCredentialVault
      └─ QPointer<DockView> (non-owning after registration)

OBS dock wrapper (OBS-owned)
  └─ DockView (QWidget)

Windows Credential Manager
  └─ one Easy Multistream YouTube key for the current Windows account

Current OBS profile/basic.ini
  └─ SchemaVersion + YouTubeEnabled only
```

The dock can save the non-secret enable setting and a masked YouTube key. It states in ordinary user-facing language that streaming to YouTube is not yet available and does not claim that either platform is connected or streaming. The key is intentionally shared across OBS profiles in v1; only the enabled flag is profile-specific.

User-visible text follows OBS and platform terminology. Internal milestone names, schema versions, implementation roles such as primary/secondary output, and release codenames stay in code and engineering documentation rather than appearing in the dock.

## Lifetime invariants

1. Before `obs_frontend_add_dock_by_id` succeeds, the plugin owns the widget.
2. After registration succeeds, OBS owns the widget; the plugin must never delete it.
3. Closing the dock only hides it and never changes a streaming session.
4. `OBS_FRONTEND_EVENT_EXIT` is the last point at which the plugin removes its callback and dock through the Frontend API.
5. Teardown is idempotent: the callback and dock are removed at most once.
6. The plugin keeps only a `QPointer` to the OBS-owned widget.
7. The controller never retains `config_t *` or a pointer returned by `config_get_string()` across a call.
8. Profile-changing and shutdown events clear the secret input and disable further actions before teardown.
9. No private OBS C++ headers, global style sheets, updater, telemetry, or background threads are used.

## Configuration and credential invariants

1. `basic.ini` contains only `SchemaVersion` and `YouTubeEnabled` under `[EasyMultistream]`.
2. The stream key is written only to Windows Credential Manager and is never read back into the editor.
3. Saving a key and changing a profile's enabled flag are independent operations; there is no cross-store transaction to partially commit.
4. An explicit Save Key action requires a non-empty, valid key. Deleting the shared key requires confirmation and does not rewrite any profile setting.
5. An enabled profile without a stored key is treated as incomplete and cannot stream until a new key is saved. A profile cannot be newly enabled while the key is missing.
6. Invalid and unknown future schemas are read-only and are never downgraded by this version.
7. A failed safe-save restores the previous in-memory non-secret settings.
8. Credential failure cannot alter the OBS primary output; this slice has no output APIs at all.

## Planned runtime boundary

The streaming implementation will be added only after this configuration slice is verified inside OBS. Its session controller will be owned by the plugin context, not by the dock or settings controller. The dock will render immutable status snapshots and may be closed without affecting outputs. OBS callbacks and output callbacks will carry a session generation so stale events cannot mutate a newer session.

The future secondary output will reference the native primary encoders but will own its own service and output. Release order and failure paths will be tested before adding automatic reconnect.

## Packaging

The repository builds against pinned OBS and dependency archives. Packaging produces an installer-free ZIP with this OBS portable layout:

```text
obs-plugins/64bit/obs-easy-multistream.dll
data/obs-plugins/obs-easy-multistream/locale/...
obs-easy-multistream-LICENSE.txt
```

No OBS, Qt, installer, or third-party runtime DLL is bundled. Build dependencies stay under the repository-local `.deps` directory.
