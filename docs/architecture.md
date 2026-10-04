# Architecture

Easy Multistream is built in small, reviewable slices. The current development tree integrates profile-aware settings, secure local credential storage, a value-only session state machine, an OBS Frontend bridge, and one fail-isolated YouTube RTMPS output.

## Current product boundary

The planned first useful release is intentionally narrow:

- Windows x64 and OBS Studio 32
- the native OBS Twitch output remains the primary output
- one additional YouTube RTMPS output using the RTMPS Stream URL copied from YouTube Studio
- reuse of the primary H.264 and AAC encoders, with no second video encode
- one OBS Start/Stop workflow
- YouTube failure isolation: a secondary failure must not stop Twitch
- one manually configured YouTube custom stream key and editable RTMPS Stream URL, with YouTube Auto-start and Auto-stop; this remains the current implementation and future advanced fallback
- optional YouTube automatic Dual stream, where YouTube creates a vertical feed from the single horizontal input
- a standard OBS dock using public Frontend and Qt APIs

Browser-based YouTube account connection is the accepted next setup path, but it is not exposed until its complete authorization, secure token storage, endpoint resolution, cancellation, release, policy, and UI requirements are ready. The internal account-destination handoff into the existing output runtime is already integrated and tested, but the production capability gate remains deliberately false: the current build supplies no Google client ID, opens no browser, performs no account network request, and starts no account output. Automatically creating and managing YouTube broadcasts or scheduled events, three or more destinations, custom output scenes, encoder-controlled YouTube dual streaming, per-destination transcoding, and Twitch-as-secondary remain outside the current boundary.

The current source tree includes an account state machine, Google desktop-authorization protocol core,
a separate loopback-listener library, a browser-opener authorization-session layer, a fixed-origin HTTPS token
transport, a read-only YouTube discovery transport with bounded pagination, a headless interactive account provider,
a separate secret-bearing selected-stream resolver, a lifecycle-owned headless destination preparer, a Windows
profile-operation lock, and an active-profile restore coordinator owned by the production lifecycle wrapper. The
provider and destination preparer both
require the lock provider to be injected and keep
their profile lock for the complete account transaction. They
generate PKCE/state values, build and validate the authorization exchange, reject old account-operation leases, test a
short-lived `127.0.0.1` callback listener on real local sockets, guarantee bind-and-arm-before-browser ordering, and
test authorization-code exchange, refresh, and revocation against injected local doubles. The discovery transport obtains exactly one bounded page of owned channels or
reusable encoder streams from fixed YouTube API endpoints. Its partial-response projection excludes CDN ingestion
details and stream keys, and it returns every valid candidate rather than choosing one implicitly. The listener uses an OS-assigned port, Windows exclusive-address binding,
bounded HTTP parsing, one-shot completion, cancellation, and timeouts. The authorization session accepts only an
injected browser opener, preserves the PKCE verifier until a validated callback wins, and invalidates cancelled,
replaced, or shutdown work by attempt and internal epoch. The HTTPS layer accepts only Google's fixed
token and revocation endpoints, requires verified TLS, rejects redirects, bounds and strictly parses replies, and does
not send a client secret. The provider serializes authorization, token exchange, complete channel/stream discovery,
exact-ID selection, scoped refresh-token storage, and non-secret profile-selection commit. A single candidate advances
automatically; multiple candidates wait for an explicit ID. The non-secret profile selection is persisted before the
scoped credential write; if that write fails, the selection is restored. If a later commit invariant fails, both durable
stores are restored where possible and a failed rollback clears the visible connection instead of restoring uncertain
state. The resolver fetches only the explicitly selected stream ID immediately before a future
start, revalidates its channel, state, RTMPS host, and stream key, and returns the key in a move-only buffer rather than
any discovery or settings type. The destination preparer captures an immutable profile/channel credential scope for
each preparation, reads the saved refresh credential just before preparation, refreshes the access token, durably stores
a rotated refresh token in that same scope before starting the selected-stream resolver, and
returns only a validated RTMPS URL plus a move-only key. A refresh-token write failure stops preparation before any
resolver request; a rotation that has been written is not rolled back when a later resolver request fails or is cancelled.
Its completions are guarded by an owner-thread epoch and attempt, and cancellation, context invalidation, shutdown, and
late callbacks fail closed. Lock acquisition never waits: a busy profile returns before changing provider/preparer
state, while an unavailable lock fails closed. A recovered named mutex is accepted only as an internal recovery result;
the operation re-reads durable state where required and no recovery terminology reaches user-facing text. The provider
releases its owner-thread lock after credential/profile rollback or the final commit. The preparer releases its lock
after queued completion state is finalized and immediately before invoking an external completion handler. If native
release fails, neither component reports a usable result. When ownership remains, it retains the lock fail-closed and
the destination owner queues one bounded cleanup retry before restoring the new profile; unfinished lock or port cleanup
is retried again during shutdown. A production lifecycle owner now links the provider, destination
preparer, and profile operation coordinator into the OBS plugin to restore saved local state during module load and `PROFILE_CHANGED`, invalidate
it during `PROFILE_CHANGING`, close it during `EXIT`, and provide a tested local-disconnect seam for the future account UI.
The ordering is part of the contract: `PROFILE_CHANGING` first invalidates runtime/output work and its leases, then
invalidates the account owner; `PROFILE_CHANGED` restores only after the old work is no longer usable. On `EXIT`, the
runtime completes its output stop/release barrier before the account bridge/provider is shut down and the owner is made
terminal. This prevents a late account completion from starting an output or restoring state during teardown.
No current dock action invokes connection, disconnect, or revoke. Plugin main owns only their lifecycle and the dormant
account-output bridge while the production capability gate remains false. The owner exposes headless connection and destination-preparation facades
that revalidate the active account-mode profile without advancing its generation, map provider details to stable
operation results, uses attempt-scoped cancellation, and exposes candidates only through revision-bound handles and
copied labels rather than raw Google resource IDs. The production owner still uses an empty client ID and browser opener,
so the facade returns not-configured before it opens a listener or browser, makes an OAuth/API request, or starts an
output. The dormant provider and destination preparer construct their Qt network adapters; this adds the Qt Network runtime dependency but no
socket bind, HTTP request, or background service. The current dock and manual RTMPS workflow remain unchanged. The
profile codec now distinguishes manual and account modes and can preserve a bounded,
non-secret channel/stream selection. Account mode without a selection is also a valid persisted setup-required state,
so clearing or importing a profile never silently changes it to the manual-key path. The provider can restore
a saved selection by checking only the scoped Credential Manager entry: present becomes locally configured but not
Google-validated, missing becomes reauthorization-required, and credential-service errors remain unavailable. A profile
with no selection does not inspect or delete any account credential. Restoration advances the account generation
and starts no browser, listener, HTTP request, discovery operation, or output. The lifecycle integration and the
owner-to-runtime destination handoff are active and covered by tests, but production account capability remains
fail-closed behind the explicit availability gate. It can never consume the manual URL/key destination. Production
client configuration, browser opening, interactive authorization, and user-facing account controls remain
release-gated; the internal output handoff is not itself a user-visible feature.

The manual YouTube stream key remains intentionally shared by all OBS profiles, but the Google refresh token uses a
separate typed store scoped to the exact active OBS profile path and selected YouTube channel. The profile path is
represented only by a SHA-256 binding in the Credential Manager target, and the channel is represented only by its
SHA-256 binding in `CREDENTIALW.UserName`. Raw profile paths and tokens never enter the target, profile, logs, or UI;
the profile may retain only bounded non-secret channel/stream selection data, and the raw channel ID is never copied into
Credential Manager metadata, logs, or UI. Duplicate/import/rename operations and portable-profile path moves produce a different profile binding and therefore
require a new account connection; no credential is transferred automatically. The old fixed refresh-token target is
never read, migrated, or deleted. Credential presence is still only local configuration, not proof of Google validity.
The provider and destination preparer retain the immutable scope captured for each operation, so delayed work from an old
profile cannot read or write another profile's credential. Before any output can start, preparation must refresh the token
and resolve the exact saved channel and stream. If profile selection is persisted before a new token is written and that
write fails, the selection is rolled back. The profile-operation lock derives a non-secret `Local\\` mutex name from the
same validated binding, rejects both other-process ownership and same-process recursive acquisition without waiting, and
reports recovered ownership only as an internal result. Local disconnect, remote revoke, and destination preparation use
this same boundary. Interactive authorization remains detached from the production dock. Destination preparation is
connected to the streaming runtime through a tested internal bridge, but the production availability gate keeps that
path dormant until the official account setup and policy requirements are complete.

The `YouTubeAccountProfileRestoreCoordinator` supplies the outer profile transaction used by the production lifecycle
owner for both restore and local disconnect. On its owner thread it first obtains only a value-copy candidate binding, then
performs a non-blocking acquisition of the exact profile-operation lock. While that lock is held it re-reads the active
profile path and config, verifies that the binding is unchanged, and asks the provider to perform credential-status
inspection and saved-state restoration through the already-held lock. It verifies the active binding again after that
transition and before release, rejecting any unannounced profile switch. The lock is released on the same owner thread
after the provider transition. Reentrant profile-invalidation or shutdown requests are recorded and applied only after
the in-flight provider transition has finished, so an injected callback cannot release the mutex around unfinished
state mutation. A nested restore is rejected as busy. A busy or unavailable acquisition leaves both context and
provider state unchanged; an active-profile race, invalid or future settings, and native-release failure fail closed.
Owner-thread destruction makes the provider terminal even if the last native cleanup retry fails; any retained native
ownership remains fail-closed until process teardown. Recovered ownership is accepted only for the same locked reread
and remains internal metadata. It passes only copied non-secret values across
the transaction boundary; refresh tokens and stream keys stay in their credential/output boundaries. Local disconnect
deletes the exact scoped credential before clearing the non-secret selection, preserves account mode, and never contacts
Google. A credential-delete failure changes neither durable store and remains retryable. If selection persistence fails
after deletion, the credential is not reconstructed and the provider becomes unavailable until restoration. Native lock
release failure still overrides success and remains fail-closed. The owner rebinds successful results to the resulting
profile generation. Its internal connection facade re-reads the active path and profile settings before every operation
and invalidates an attempt if the profile, generation, or connection mode no longer matches. No production caller invokes
that facade. The same owner now contains a headless remote-revoke transaction that holds the profile-operation lock from
the exact credential read through Google acknowledgement and local cleanup. If `PROFILE_CHANGED` arrives while the old
profile's revoke completion is still queued, the owner retains that one restore request and applies it only after the old
transaction has released its lock; stale completion data cannot overwrite the newly active profile. The transaction is
not connected to the dock or output runtime.

The profile path/config readers are a frontend-thread contract: both must observe the same stable active profile and
must not process events. OBS emits `PROFILE_CHANGING` before activation, swaps `activeConfiguration` and the current
profile identity, and emits `PROFILE_CHANGED` only afterward; the plugin invalidates on the former and loads on the
latter. The initial module load also runs on that same frontend thread. A test adapter that returns a config from a
different profile violates this boundary and is not an accepted dependency implementation.

The recommended Dual stream mode is a YouTube-side feature. Easy Multistream sends one 16:9 H.264/AAC stream to the user-provided RTMPS URL; YouTube creates the 9:16 feed, normally as a centre crop. This keeps the local OBS pipeline to one YouTube output and one shared video encode. The vertical mode must be enabled in YouTube Studio before the stream starts. A separately composed 9:16 stream sent by the encoder would require a second video pipeline and is intentionally deferred.

## Accepted YouTube connection direction

The normal future setup is **Connect YouTube** in the dock, system-browser Google authorization, and selection of an existing reusable YouTube encoder stream. The current manual RTMPS URL and key remain an explicitly selected advanced fallback. A failed or revoked account connection must never silently switch to a saved manual destination.

The account connection is owned by a plugin-level provider, not by `DockView`, `SettingsController`, `SessionCoordinator`, or `YouTubeOutputAdapter`. It resolves an authenticated YouTube stream into the same validated RTMPS URL plus ephemeral stream-key boundary already consumed by the output adapter. OAuth tokens never enter session snapshots, OBS output settings snapshots, profile settings, UI state, or logs.

OBS's internal YouTube account object is not a public plugin API and is tied to the active native OBS service. Easy Multistream therefore does not read OBS's private authentication settings or switch the native service away from Twitch. It owns a separate standards-based desktop OAuth flow using the system browser, a loopback callback, PKCE, and a distinct Windows Credential Manager entry.

Implementation is deliberately gated in this order:

1. a scoped credential store and a headless, cancellable account-connection provider;
2. browser authorization, refresh/revocation, and YouTube channel/stream discovery;
3. authenticated endpoint resolution into the existing output boundary;
4. profile-change, shutdown, stale-callback, and failure-isolation tests;
5. only then, account controls and channel identity in the dock.

Internal phases, schema versions, provider names, and milestone numbers are not user-facing text. The full decision and release gates are recorded in [youtube-account-connection.md](youtube-account-connection.md).

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
  ├─ one Easy Multistream YouTube key for the current Windows account
  └─ profile/channel-scoped plugin-owned entries for the future Google refresh token
     (used only by lifecycle-owned account transactions; no current dock action invokes them)

Current OBS profile/basic.ini
  ├─ SchemaVersion + YouTubeEnabled + explicit connection mode
  ├─ YouTubeServerUrl for the manual path
  └─ optional non-secret channel/stream IDs and display labels

OBS user config/user.ini
  └─ one non-secret first-display marker for the dock
```

The dock saves the non-secret enable setting and YouTube RTMPS Stream URL, and exposes a masked YouTube key editor. Its setup panel can open the fixed `https://www.youtube.com/live_dashboard` entry point in the default browser, allowing YouTube to reuse the browser's signed-in session and resolve the active channel. The plugin passes no token, key, profile value, or user-supplied URL to the browser. It renders immutable Twitch/YouTube status snapshots and a YouTube-only retry action after failure. Once the URL and key are present, connection details collapse while the two destination rows and enable control remain visible; the user can reopen the details, including the Studio link, without changing a streaming session. The key is intentionally shared across OBS profiles; the enabled setting and Stream URL are profile-specific. On first use, the dock is revealed once after OBS finishes loading. Its non-secret display marker is user-scoped rather than profile-scoped, so later profile changes and plugin updates continue to respect OBS's saved dock layout. A localized Tools-menu action only reveals this same dock; it does not own or control an output.

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
10. The Tools-menu `QAction` is disconnected, removed, and synchronously destroyed before the dock or plugin context is released, so no menu callback can outlive the plugin DLL.

## Configuration and credential invariants

1. `basic.ini` contains only the schema and enable value, an explicit connection mode, the non-secret manual `YouTubeServerUrl`, and optional non-secret account-selection IDs/labels under `[EasyMultistream]`.
2. The stream key is written only to Windows Credential Manager and is never read back into the editor. This manual key
   remains shared across OBS profiles; account refresh credentials are separate and profile/channel-scoped.
3. Saving a key and changing a profile's enabled flag are independent operations; there is no cross-store transaction to partially commit.
4. An explicit Save Key action requires a non-empty, valid key. Deleting the shared key requires confirmation and does not rewrite any profile setting.
5. An enabled profile without a stored key is treated as incomplete and cannot stream until a new key is saved. A profile cannot be newly enabled while the key is missing.
6. Invalid and unknown future schemas are read-only and are never downgraded by this version.
7. A failed safe-save restores the previous in-memory non-secret settings.
8. Credential or YouTube output failure cannot alter the native OBS output.
9. Account mode cannot read or start with the manual URL/key path; it remains unavailable unless the production account capability is explicitly enabled and the current `OutputLease` plus `NativeLease` handoff is valid.
10. Schema 1 and 2 profiles migrate in memory as manual mode. A normal settings save writes the current schema without placing any credential, token, authorization code, PKCE value, or resolved stream key in the profile.

## Runtime boundary

`obs-easy-multistream-session-core` is a pure C++ library with no OBS, Qt, Windows Credential Manager, or network dependency. It is linked into the plugin through `RuntimeController`. Its `SessionCoordinator` accepts value events and returns an immutable snapshot plus at most one requested effect. The only output effects are `StartYouTube` and `StopYouTube`; there is intentionally no effect capable of stopping the native OBS stream.

`SessionCoordinator` is deliberately not thread-safe. `RuntimeController` serializes every state event on the Qt/OBS owner thread. Frontend callbacks and output callbacks copy only lease/status values; worker-originated output callbacks are queued through a plugin-owned `QObject` that outlives the controller. Each returned effect is handed to the adapter in transition order before the bridge processes its next state event.

The headless YouTube account coordinator follows the same value-copy rule. Its public snapshots are returned by value,
and a committed account retains a non-secret connection lease. Refresh and API results must carry that lease, so a
delayed failure from a replaced account cannot clear or relabel the current account.

Each native OBS start attempt receives a `NativeLease`, and each YouTube start attempt receives an `OutputLease`. Both contain a profile/session generation and an attempt number. Callbacks for an old generation or attempt cannot update the current snapshot. Snapshot revisions advance only for accepted state changes. Profile changes and exit invalidate the current generation before delayed callbacks can be observed by a new session.

The internal account handoff keeps the two leases separate and explicitly maps each `OutputLease` to the exact
`YouTubeDestinationPrepareAttempt` returned by the account owner. A prepared destination is accepted only when the
output lease, native lease, profile generation, connection mode, settings, and native streaming state still match.
Preparation cancellation is distinct from release of an active destination use: before an OBS output exists, Stop or
shutdown cancels the owner attempt and completes the runtime lease without asking the adapter to stop; after the
adapter starts, the owner use lease remains held until the adapter's full `Released` event. Account mutations are
busy while that use lease is held, and an old or mismatched completion can never reach the output adapter.
Worker-originated output events enter an owner-thread mailbox before a queued wake-up is requested. If that wake-up is
rejected, the next owner-thread operation drains the mailbox, so a `Released` teardown barrier is not silently lost.

The destination classifier accepts only exact known OBS service values. A Twitch session is eligible only when the service type is `rtmp_common` and the provider name is exactly `Twitch`. Known YouTube names are recognized for future role-neutral behavior, while custom RTMP, relays, substring matches, and unknown services remain unsupported rather than being guessed from a URL or stream key.

The OBS Frontend bridge and output adapter are owned by the plugin context, not by the dock or settings controller. The dock may be closed without affecting outputs. Raw OBS pointers and secret values never enter `SessionCoordinator` or its snapshots. The detailed ownership and callback rules are recorded in [the output integration contract](runtime-output-design.md).

The YouTube output references the native video encoder and main live audio encoder but owns its own service and output. Its service uses the validated RTMPS Stream URL supplied by the user and the temporary key retrieved from Windows Credential Manager. Account preparation, when eventually enabled, supplies the same validated URL plus a move-only temporary key; neither path places secrets in settings, snapshots, diagnostics, or logs. Additional Twitch VOD audio is ignored rather than treated as an ambiguous layout; multiple video encoders remain unsupported.

The output adapter does not automatically loop after a YouTube failure. Once the failed output has passed the full teardown barrier, the dock exposes an explicit YouTube-only retry action while Twitch continues.

## Session state invariants

1. OBS remains the only owner of the native stream Start/Stop workflow.
2. YouTube can start only while a recognized Twitch native stream is running and the selected manual or account destination is independently configured and accepted by its own lease boundary.
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
