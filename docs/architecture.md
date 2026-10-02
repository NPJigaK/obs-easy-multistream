# Architecture

Easy Multistream is built in small, reviewable slices. The current development tree integrates profile-aware settings, secure local credential storage, a value-only session state machine, an OBS Frontend bridge, and one fail-isolated YouTube RTMPS output.

## v1 product boundary

The planned first useful release is intentionally narrow:

- Windows x64 and OBS Studio 32
- the native OBS Twitch output remains the primary output
- one additional YouTube RTMPS output using the RTMPS Stream URL copied from YouTube Studio
- reuse of the primary H.264 and AAC encoders, with no second video encode
- one OBS Start/Stop workflow
- YouTube failure isolation: a secondary failure must not stop Twitch
- one manually configured YouTube custom stream key and editable RTMPS Stream URL, with YouTube Auto-start and Auto-stop
- optional YouTube automatic Dual stream, where YouTube creates a vertical feed from the single horizontal input
- a standard OBS dock using public Frontend and Qt APIs

OAuth, scheduled YouTube events, three or more destinations, custom output scenes, encoder-controlled YouTube dual streaming, per-destination transcoding, and Twitch-as-secondary are outside the v1 boundary.

The recommended Dual stream mode is a YouTube-side feature. Easy Multistream sends one 16:9 H.264/AAC stream to the user-provided RTMPS URL; YouTube creates the 9:16 feed, normally as a centre crop. This keeps the local OBS pipeline to one YouTube output and one shared video encode. The vertical mode must be enabled in YouTube Studio before the stream starts. A separately composed 9:16 stream sent by the encoder would require a second video pipeline and is intentionally deferred.

## Current runtime

```text
OBS module entry point
  └─ PluginState (plugin-owned)
      ├─ SettingsController (plugin-owned, UI-thread only)
      │   ├─ active profile config (borrowed only during each call)
      │   └─ WindowsCredentialVault
      ├─ RuntimeController + SessionCoordinator (UI-thread serialized)
      ├─ YouTubeOutputAdapter
      │   └─ private reaper thread for OBS output teardown
      ├─ private YouTube RTMP service/output
      │   └─ retained references to native H.264 + main AAC encoders
      └─ QPointer<DockView> (non-owning after registration)

OBS dock wrapper (OBS-owned)
  └─ DockView (QWidget)

Windows Credential Manager
  └─ one Easy Multistream YouTube key for the current Windows account

Current OBS profile/basic.ini
  └─ SchemaVersion + YouTubeEnabled + YouTubeServerUrl

OBS user config/user.ini
  └─ one non-secret first-display marker for the dock
```

The dock saves the non-secret enable setting and YouTube RTMPS Stream URL, and exposes a masked YouTube key editor. It renders immutable Twitch/YouTube status snapshots and a YouTube-only retry action after failure. The key is intentionally shared across OBS profiles; the enabled setting and Stream URL are profile-specific. On first use, the dock is revealed once after OBS finishes loading. Its non-secret display marker is user-scoped rather than profile-scoped, so later profile changes and plugin updates continue to respect OBS's saved dock layout.

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
9. No private OBS C++ headers, global style sheets, updater, or telemetry are used. The only plugin-created background thread is the output reaper that prevents potentially blocking RTMP teardown from running on the UI thread.

## Configuration and credential invariants

1. `basic.ini` contains only `SchemaVersion`, `YouTubeEnabled`, and the non-secret `YouTubeServerUrl` under `[EasyMultistream]`.
2. The stream key is written only to Windows Credential Manager and is never read back into the editor.
3. Saving a key and changing a profile's enabled flag are independent operations; there is no cross-store transaction to partially commit.
4. An explicit Save Key action requires a non-empty, valid key. Deleting the shared key requires confirmation and does not rewrite any profile setting.
5. An enabled profile without a stored key is treated as incomplete and cannot stream until a new key is saved. A profile cannot be newly enabled while the key is missing.
6. Invalid and unknown future schemas are read-only and are never downgraded by this version.
7. A failed safe-save restores the previous in-memory non-secret settings.
8. Credential or YouTube output failure cannot alter the native OBS output.

## Runtime boundary

`obs-easy-multistream-session-core` is a pure C++ library with no OBS, Qt, Windows Credential Manager, or network dependency. It is linked into the plugin through `RuntimeController`. Its `SessionCoordinator` accepts value events and returns an immutable snapshot plus at most one requested effect. The only output effects are `StartYouTube` and `StopYouTube`; there is intentionally no effect capable of stopping the native OBS stream.

`SessionCoordinator` is deliberately not thread-safe. `RuntimeController` serializes every state event on the Qt/OBS owner thread. Frontend callbacks and output callbacks copy only lease/status values; worker-originated output callbacks are queued through a plugin-owned `QObject` that outlives the controller. Each returned effect is handed to the adapter in transition order before the bridge processes its next state event.

Each native OBS start attempt receives a `NativeLease`, and each YouTube start attempt receives an `OutputLease`. Both contain a profile/session generation and an attempt number. Callbacks for an old generation or attempt cannot update the current snapshot. Snapshot revisions advance only for accepted state changes. Profile changes and exit invalidate the current generation before delayed callbacks can be observed by a new session.

The destination classifier accepts only exact known OBS service values. A Twitch session is eligible only when the service type is `rtmp_common` and the provider name is exactly `Twitch`. Known YouTube names are recognized for future role-neutral behavior, while custom RTMP, relays, substring matches, and unknown services remain unsupported rather than being guessed from a URL or stream key.

The OBS Frontend bridge and output adapter are owned by the plugin context, not by the dock or settings controller. The dock may be closed without affecting outputs. Raw OBS pointers and secret values never enter `SessionCoordinator` or its snapshots. The detailed ownership and callback rules are recorded in [the output integration contract](runtime-output-design.md).

The YouTube output references the native video encoder and main live audio encoder but owns its own service and output. Its service uses the validated RTMPS Stream URL supplied by the user and the temporary key retrieved from Windows Credential Manager. Additional Twitch VOD audio is ignored rather than treated as an ambiguous layout; multiple video encoders remain unsupported.

The output adapter does not automatically loop after a YouTube failure. Once the failed output has passed the full teardown barrier, the dock exposes an explicit YouTube-only retry action while Twitch continues.

## Session state invariants

1. OBS remains the only owner of the native stream Start/Stop workflow.
2. YouTube can start only while a recognized Twitch native stream is running and both the profile setting and saved-key status allow it.
3. Duplicate native Start/Stop events do not emit duplicate YouTube effects.
4. A YouTube failure changes only the YouTube state; it cannot request that OBS stop streaming.
5. An unexpected YouTube stop does not enter an immediate restart loop.
6. A rapid native restart waits until the prior YouTube output has been fully released before issuing a new start; the raw OBS `stop` signal is not sufficient.
7. A delayed native stop from an older attempt cannot stop a newer native or YouTube session.
8. Profile change and exit invalidate old leases, and exit is terminal.
9. Internal generations, attempts, revisions, and output roles are never shown in the user interface.

## Packaging

The repository builds against pinned OBS and dependency archives. Packaging produces two installer-free ZIPs for the currently supported OBS Studio 32.2.2 target.

The regular Windows package contains one plugin folder for
`C:\ProgramData\obs-studio\plugins`:

```text
obs-easy-multistream/
  bin/64bit/obs-easy-multistream.dll
  data/locale/...
  LICENSE.txt
```

The portable package is extracted into the root of an OBS Studio 32.2.2 portable installation:

```text
obs-plugins/64bit/obs-easy-multistream.dll
data/obs-plugins/obs-easy-multistream/locale/...
obs-easy-multistream-LICENSE.txt
```

No OBS, Qt, installer, or third-party runtime DLL is bundled. Build dependencies stay under the repository-local `.deps` directory.
