# Security and secret handling

Easy Multistream treats the YouTube stream key as a password. The current implementation creates one YouTube RTMPS output only after the native OBS Twitch stream has started, while keeping the key out of OBS profile files and plugin logs.

## Where data is stored

The active OBS profile stores only these non-secret values in `basic.ini`:

```ini
[EasyMultistream]
SchemaVersion=3
YouTubeEnabled=false
ConnectionMode=manual
YouTubeServerUrl=rtmps://a.rtmps.youtube.com/live2
```

The account-ready profile format can also hold a non-secret YouTube channel ID, channel label, reusable-stream ID, and stream label. Those values are strictly bounded and validated, but the unfinished account path is not exposed in the dock and cannot start an output. Account mode without a selection is a valid setup-required profile state. The connection mode is explicit: an account-mode profile never falls back to the saved manual URL/key path.

The YouTube stream key is stored as a Windows Generic Credential for the current Windows account. It is not stored in `basic.ini`, scene collections, profile exports, plugin logs, or diagnostic text. One manual stream-key credential is shared by all OBS profiles; the dock states this explicitly. The non-secret Stream URL and future account selection are profile-scoped. The Google refresh token uses a distinct typed store whose target is scoped to a SHA-256 binding of the exact active OBS profile path, while the selected channel is represented by a SHA-256 binding in `CREDENTIALW.UserName`. Raw profile paths and tokens never appear in a target name, profile, log, or UI. A profile may retain only bounded non-secret channel/stream selection data; the raw channel ID is never copied into Credential Manager metadata, logs, or UI. The old fixed refresh-token target is never read, migrated, or deleted. The account provider is owned by the production lifecycle wrapper, while the destination preparer remains detached. Both use the injected profile-operation lock. Production restoration reads only credential status; the new local-disconnect seam can delete the exact scoped credential, but no current UI or plugin-main action invokes it. No production path reads the refresh-token bytes yet.

Deleting the credential is an explicit, confirmed action. It does not rewrite any profile's non-secret enabled flag. Enabled profiles remain configured but cannot stream until a new shared key is saved.

## In-memory boundary

The key necessarily exists briefly in process memory while the user enters it and while it is passed to Windows Credential Manager. The plugin:

- uses a password-mode editor with copy, cut, drag, drop, and context-menu export disabled;
- clears the editor after a successful save, on profile changes, when the dock is hidden, and during shutdown;
- wipes temporary UTF-8 and credential buffers before releasing them;
- never reads a saved key back into the editor or a UI status snapshot;
- never includes a key in an exception or log message.

Qt, OBS, Windows, libobs service settings, and the process allocator can still retain transient copies outside the plugin's direct control. In particular, libobs must copy the key into the temporary RTMP service configuration, and the public OBS config API can remove an accidentally present legacy plaintext field from the persisted profile but does not securely erase the freed config heap allocation. This project therefore does not claim resistance to live process-memory inspection or crash-dump forensics.

Known plaintext credential field names are removed before any profile write, including when the proposed settings are otherwise invalid. If the atomic profile save itself fails, the in-memory config remains scrubbed but the previous on-disk file cannot be rewritten; the normal save-failure notice remains visible and the next successful save retries the cleanup. Tests cover both the failure and recovery paths.

The same boundary applies to the account connection under development. Listener-owned byte buffers and the final authorization code container are wiped, but strict callback validation currently passes through transient Qt `QUrl`, `QUrlQuery`, and `QString` values. Qt does not promise to zero those internal allocations when they are released. Authorization codes, state values, and tokens are never logged or persisted through that path, but the project does not claim that every transient copy can be removed from process memory.

The detached destination-preparation layer reads the refresh token only when a selected stream is about to be prepared. Each operation captures an immutable profile/channel scope and acquires the non-blocking profile-operation lock before the first credential read. The lock remains held while the refreshed access token is passed in a move-only buffer to the selected-stream resolver, while a rotated refresh token is synchronously written to that same Credential Manager scope, and until queued completion state is finalized. It is released on the owner thread immediately before an external completion handler runs. If the durable write fails, no resolver request is made and the completion contains no destination. Once a rotated token has been written successfully, a later resolver failure or cancellation does not roll it back: it is the newest durable credential returned by Google. The preparer exposes only stable status values and a successful move-only RTMPS URL/key result, never provider text or secret-bearing snapshots. Epoch/attempt checks, cancellation, context invalidation, shutdown, and owner-thread queued delivery reject late work, including callbacks that belong to another profile. This layer is currently standalone/test-linked only, so it adds no production plugin network surface.

Saved-account restoration reads only the scoped refresh-token credential status, and only when the active account-mode profile contains a complete validated channel/stream selection. Status does not copy the secret into a snapshot. It does not contact Google, open a browser or listener, or start an output. Presence is recorded only as locally configured; it is not treated as proof that the token is currently valid for Google. Duplicate/import/rename operations and portable-profile path moves produce a new profile binding and require reconnecting; no credential is transferred automatically. With no selection, restoration does not inspect or delete any credential. A missing credential and an unavailable credential service remain distinct fail-closed states; neither may select the manual stream-key path. Before output creation, preparation must refresh the token and resolve the exact saved channel and stream. Profile invalidation and each restoration advance the account generation before old asynchronous work can be accepted.

Local account disconnect is a synchronous owner-thread transaction behind the production runtime owner, but it is not
yet exposed in the dock. It re-reads the exact active account profile while holding the same non-blocking profile lock,
deletes only the credential whose profile and channel metadata match, then clears the non-secret selection while keeping
the profile in account mode. With no selection it never probes Credential Manager. A deletion failure leaves both stores
unchanged and can be retried. If the credential was deleted but the profile save fails, the token is never recreated from
stale memory and the provider becomes unavailable; the remaining selection cannot be treated as connected. A profile
race, shutdown request, or native lock-release failure overrides success and fails closed. This operation makes no Google
request; remote revocation remains a separate explicit future action.

## Credential Manager boundary

Windows Credential Manager protects the key for the signed-in Windows user and keeps it on the local computer. It is not a hardware-backed vault, does not protect against malware running as the same user, and is not a substitute for rotating a key after suspected compromise.

The plugin uses the documented Generic Credential size limit, validates the key before writing it, treats a missing credential as a normal state, and treats access-denied or unavailable credential services as a non-streaming failure. The manual key remains shared across profiles; the refresh-token store is a separate typed, profile/channel-scoped boundary. Development tests use an injected fake API and do not write test credentials to the developer's global Credential Manager.

## Network surface

The current OBS plugin performs no OAuth, HTTP API, update, telemetry, relay, or inbound-network operation. When YouTube is enabled and OBS confirms that a recognized native Twitch stream is running, the plugin opens one outbound RTMPS connection using OBS's `rtmp_output`. URL validation requires `rtmps://`, port 443 when a port is present, a single application path, and a host under `*.rtmps.youtube.com`; this prevents an imported profile from sending the saved YouTube key to an arbitrary RTMPS host.

The loopback-listener and authorization-session libraries are now linked through the account lifecycle owner, but no
production call can start them: the owner supplies neither a client ID nor a browser opener, and its uncalled internal
connection facade rejects that configuration before delegating to the authorization layer. Construction does not call
`listen()`, bind a socket, or launch a browser. Standalone tests exercise
the active path: it binds only `127.0.0.1` on an OS-assigned port, sets `SO_EXCLUSIVEADDRUSE` before bind, accepts one
strictly validated HTTP/1.1 callback, and enforces request-size, connection-count, per-client, and overall authorization
limits. Wrong-state and malformed requests do not complete an authorization attempt; accepted and rejected browser
pages contain no code, state, query, or provider description. The session binds and arms that listener before invoking
an injected browser opener, catches opener failure, silently invalidates cancelled or shutdown attempts, and returns the
authorization code plus PKCE verifier only in a move-only success result. Tests inject a fake opener and never launch a
real browser.

PKCE, state validation, and exclusive binding prevent an unrelated local process from successfully completing or taking over an authorization attempt, but they cannot make the desktop resistant to denial by malware already running as the same Windows user. Such a process can repeatedly connect to the temporary port, consume the bounded connection budget, or keep bounded client slots busy until their timeout. The listener fails closed and requires a retry rather than accepting a callback after its limits are exhausted.

The token-transport library is linked and its network manager is constructed by the lifecycle-owned provider, but no token
operation is reachable from the production plugin yet and construction emits no request. Its tested active path posts
form data only to Google's fixed HTTPS token and revocation endpoints, never accepts an endpoint from settings, never
sends a client secret, requires peer-verified TLS 1.2 or later, rejects redirects and missing encryption proof, disables
credential and cookie reuse, and applies independent time and 64 KiB response limits. Responses are type-checked, known
security fields cannot be duplicated, raw provider descriptions are not returned, and operation generations prevent a
cancelled or replaced request from completing newer work. Authorization codes, PKCE verifiers, response bodies, and
parsed token staging buffers are explicitly cleared where the involved API permits it. Qt and operating-system internals
may still make transient copies under the in-memory limitations documented above.

The read-only YouTube discovery transport is also linked and constructed by the lifecycle-owned provider, but no discovery
request is reachable from the production plugin. Its active path is exercised by standalone development tests.
It sends a move-only access-token buffer solely as a Bearer header to the fixed `channels.list` and
`liveStreams.list` HTTPS endpoints. It requests one bounded page at a time, uses `mine=true`, strictly validates IDs,
labels, page tokens, headers, JSON, TLS, and final URLs, and returns stable error enums rather than provider text. Its
fixed partial-response fields request only IDs and display labels; CDN ingestion data and `streamName` are not requested
or returned by this layer. A separate, test-only-linked pagination layer follows opaque continuation values serially
with fixed total-time, page, and item limits. It rejects continuation cycles and cross-page duplicate IDs and returns
no partial candidates after any page failure, timeout, cancellation, or limit breach. Access tokens are copied only
into move-only buffers for the active page and are cleared with the aggregate operation state.

A separate headless account-provider library composes authorization, code exchange, bounded discovery, exact candidate
selection, scoped refresh-token storage, and non-secret selection persistence. It requires the profile-operation lock
before an interactive transaction and holds that lock through selection/credential persistence and any rollback. A busy
lock returns without mutating provider state; a recovered lock is internal recovery metadata only. The production plugin
now owns this provider behind a lifecycle wrapper that performs saved-state restoration and exposes tested internal
connection and local-disconnect transactions. Neither action is connected to the dock, plugin main, or streaming runtime.
The connection snapshot contains copied labels and revision-bound candidate handles, not raw channel/stream IDs or secret
material. Every operation first performs a non-mutating reread of the active profile and rejects a changed generation,
binding, or connection mode before any browser or transport action.
If native mutex release fails, the provider marks the account unavailable instead of exposing a usable connection. The
destination preparer discards any resolved ingestion secret and reports a service failure before invoking its completion
handler. If mutex ownership remains, both retain the claim fail-closed and retry incomplete port and lock cleanup on
owner-thread shutdown.
Its deterministic test uses fake browser/token/API/vault boundaries and no real network or global Credential Manager.
Provider snapshots contain only stable state, candidate IDs/labels, and account leases. A replacement token is not made
visible until discovery and both persistence steps succeed; the profile selection is persisted first and is rolled back if
the scoped credential write fails. If a later commit invariant fails, rollback failure clears the visible account and fails
closed. All lower completions are owner-thread operations guarded
by an independent epoch, the account lease, and the expected stage.

The repository also contains a separate selected-stream resolver that is built only for its standalone development
test. It requests one explicitly selected stream ID from the fixed `liveStreams.list` endpoint, rechecks the returned
stream and channel IDs, accepts only the documented `ready` and `inactive` states, and refuses plain RTMP or an
unexpected YouTube ingestion host. Its successful result contains only a validated RTMPS URL and a move-only
`SecureBuffer` holding the current `streamName`. The key is not added to discovery results, settings, snapshots, logs,
diagnostics, or Credential Manager. Response bytes and directly controlled staging buffers are wiped after use;
short-lived copies made internally by Qt remain subject to the in-memory limitation documented above.

The selected-stream resolver is orchestrated by the detached destination preparer described above. The resolver is not
started until the refresh-token rotation (when present) has been durably stored, and the preparer never sends the
refresh token to the YouTube API. A failed preparation cannot alter the native OBS output; this remains a future
runtime-integration rule, because neither the preparer nor the resolver is currently instantiated by the plugin.

A detached Windows profile-operation lock provides the cross-process serialization primitive required by those account
transactions. Its `Local\\` named-mutex identity contains only the validated SHA-256 profile binding. A separate
process-wide claim registry rejects a second local acquisition because Windows mutexes are otherwise recursive for the
owning thread. Acquisition never waits, so a busy result leaves provider/preparer state unchanged; access or operating-
system failures fail closed. Recovered ownership is distinguished only internally so durable profile and credential state
can be re-read before continuing. Both account transaction classes require this lock provider and release their owner-thread
lock after rollback or finalization, before calling an external completion handler. The provider is lifecycle-owned while
the destination preparer remains detached from production. The lock does not authenticate another
process or grant credential access. The current OBS module instantiates the lock and account lifecycle owner, but does not
expose account actions in the dock.
Support is limited to the current interactive Windows session; cross-session use of one OBS profile remains unsupported
unless a later design adds a user-scoped `Global\\` object with an explicitly reviewed security descriptor.

The `YouTubeAccountProfileRestoreCoordinator` closes the stale-selection gap for saved-account restoration and local
disconnect and is now owned by the production profile lifecycle wrapper.
On its owner thread it reads only a value-copy candidate binding, acquires the exact profile-operation lock without
waiting, re-reads the active profile path and config while the lock is held, verifies the binding again, and then asks
the provider to inspect credential status and restore its state through that same held lock. It checks the active
binding once more after the provider transition and rejects a switch before releasing the lock. It releases the lock on
the owner thread after the transition. Profile invalidation and shutdown requests that re-enter through a synchronous
dependency callback are deferred until the provider transition completes; nested restore is rejected as busy. This
prevents cleanup from releasing the profile mutex while credential-derived state is still being changed. Busy or
unavailable acquisition performs no context/provider mutation; a profile race, invalid or future settings, and native
release failure fail closed. Owner-thread destruction makes the provider terminal even if native handle cleanup cannot
complete; any retained ownership remains fail-closed until process teardown. Recovered ownership is accepted only
for the locked reread and is not user-facing. The coordinator carries only copied non-secret profile/provider values;
refresh tokens and stream keys remain in their existing secure boundaries. The wrapper is called automatically only for
module/profile lifecycle events; its connection and local-disconnect methods have no production caller. The empty
production OAuth configuration makes connection start fail before browser, listener, or network work, and neither
internal seam can start an output. Future remote revoke must use the same profile-operation boundary.

The YouTube output owns a private RTMP service and output while retaining explicit references to the native H.264 and main AAC encoders. It never starts or stops the native OBS stream. A YouTube error closes only the YouTube output, leaves Twitch running, and exposes a separate retry action after teardown completes.

The accepted browser-based account connection will add the listener, token transport, discovery transport, and
selected-stream resolver to the product only after the complete flow is ready, together with outbound HTTPS requests
to fixed YouTube API origins and the existing outbound YouTube RTMPS connection. It will not add a cloud relay, local
background service, telemetry, embedded login webview, or long-running listening port. Refresh tokens use a typed
credential store separate from the manual stream key, scoped to the active profile binding and selected channel; access tokens, authorization codes, PKCE verifiers, and resolved stream keys
are never written to profile settings or logs. See [the account-connection design](youtube-account-connection.md).
