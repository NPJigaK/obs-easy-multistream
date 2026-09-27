# Architecture

Easy Multistream is being built in small, reviewable slices. The settings foundation adds profile-aware configuration and secure local credential storage to the verified OBS dock. The next stacked slice defines the session state contract independently from OBS and Qt. Neither slice contains streaming output code.

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

`obs-easy-multistream-session-core` is a pure C++ library with no OBS, Qt, Windows Credential Manager, or network dependency. It is not linked into the plugin module yet. Its `SessionCoordinator` accepts value events and returns an immutable snapshot plus at most one requested effect. The only output effects are `StartYouTube` and `StopYouTube`; there is intentionally no effect capable of stopping the native OBS stream.

`SessionCoordinator` is deliberately not thread-safe. A future runtime bridge will serialize every state event on one owner thread. Frontend callbacks and output callbacks will copy only lease/status values and post them to that owner; they will never call the state machine concurrently or capture a raw OBS pointer in queued work. Each returned effect must be handed to the adapter in transition order before the bridge processes its next state event.

Each native OBS start attempt receives a `NativeLease`, and each YouTube start attempt receives an `OutputLease`. Both contain a profile/session generation and an attempt number. Callbacks for an old generation or attempt cannot update the current snapshot. Snapshot revisions advance only for accepted state changes, so a future view bridge can reject queued updates that arrive out of order. Profile changes and exit invalidate the current generation before delayed callbacks can be observed by a new session.

The destination classifier accepts only exact known OBS service values. A Twitch session is eligible only when the service type is `rtmp_common` and the provider name is exactly `Twitch`. Known YouTube names are recognized for future role-neutral behavior, while custom RTMP, relays, substring matches, and unknown services remain unsupported rather than being guessed from a URL or stream key.

The next integration slice will add an OBS Frontend bridge and an output adapter. The bridge will be owned by the plugin context, not by the dock or settings controller. The dock will render immutable status snapshots and may be closed without affecting outputs. Raw OBS pointers and secret values will never enter `SessionCoordinator` or its snapshots. The detailed ownership and callback rules are recorded in [the output integration contract](runtime-output-design.md).

The future secondary output will reference the native primary encoders but will own its own service and output. Release order and failure paths will be tested before adding automatic reconnect.

The first output adapter will not expose automatic or in-session manual retry. A YouTube start failure remains isolated while the native stream continues, and the next native Stop/Start begins a fresh attempt. A later retry action can be added as an explicit state event after teardown and backoff behavior have their own tests.

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

The repository builds against pinned OBS and dependency archives. Packaging produces an installer-free ZIP with this OBS portable layout:

```text
obs-plugins/64bit/obs-easy-multistream.dll
data/obs-plugins/obs-easy-multistream/locale/...
obs-easy-multistream-LICENSE.txt
```

No OBS, Qt, installer, or third-party runtime DLL is bundled. Build dependencies stay under the repository-local `.deps` directory.
