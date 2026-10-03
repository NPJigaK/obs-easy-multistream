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

The account-ready profile format can also hold a non-secret YouTube channel ID, channel label, reusable-stream ID, and stream label. Those values are strictly bounded and validated, but the unfinished account path is not exposed in the dock and cannot start an output. The connection mode is explicit: an account-mode profile never falls back to the saved manual URL/key path.

The YouTube stream key is stored as a Windows Generic Credential for the current Windows account. It is not stored in `basic.ini`, scene collections, profile exports, plugin logs, or diagnostic text. One credential is shared by all OBS profiles; the dock states this explicitly. The non-secret Stream URL and future account selection are profile-scoped. The separate refresh-token credential target is fixed by the plugin and is never written into a profile; no current production path writes a refresh token yet.

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

## Credential Manager boundary

Windows Credential Manager protects the key for the signed-in Windows user and keeps it on the local computer. It is not a hardware-backed vault, does not protect against malware running as the same user, and is not a substitute for rotating a key after suspected compromise.

The plugin uses the documented Generic Credential size limit, validates the key before writing it, treats a missing credential as a normal state, and treats access-denied or unavailable credential services as a non-streaming failure. Manual-key and future refresh-token targets are distinct. Development tests use an injected fake API and do not write test credentials to the developer's global Credential Manager.

## Network surface

The current OBS plugin performs no OAuth, HTTP API, update, telemetry, relay, or inbound-network operation. When YouTube is enabled and OBS confirms that a recognized native Twitch stream is running, the plugin opens one outbound RTMPS connection using OBS's `rtmp_output`. URL validation requires `rtmps://`, port 443 when a port is present, a single application path, and a host under `*.rtmps.youtube.com`; this prevents an imported profile from sending the saved YouTube key to an arbitrary RTMPS host.

The repository also contains loopback-listener and authorization-session libraries that are built and exercised only by standalone development tests. They are not linked into the plugin module or instantiated by production code. The test listener binds only `127.0.0.1` on an OS-assigned port, sets `SO_EXCLUSIVEADDRUSE` before bind, accepts one strictly validated HTTP/1.1 callback, and enforces request-size, connection-count, per-client, and overall authorization limits. Wrong-state and malformed requests do not complete an authorization attempt; accepted and rejected browser pages contain no code, state, query, or provider description. The session binds and arms that listener before invoking an injected browser opener, catches opener failure, silently invalidates cancelled or shutdown attempts, and returns the authorization code plus PKCE verifier only in a move-only success result. Tests inject a fake opener and never launch a real browser.

PKCE, state validation, and exclusive binding prevent an unrelated local process from successfully completing or taking over an authorization attempt, but they cannot make the desktop resistant to denial by malware already running as the same Windows user. Such a process can repeatedly connect to the temporary port, consume the bounded connection budget, or keep bounded client slots busy until their timeout. The listener fails closed and requires a retry rather than accepting a callback after its limits are exhausted.

The repository also contains a separate token-transport library that is linked only to a standalone development test. It posts form data only to Google's fixed HTTPS token and revocation endpoints, never accepts an endpoint from settings, never sends a client secret, requires peer-verified TLS 1.2 or later, rejects redirects and missing encryption proof, disables credential and cookie reuse, and applies independent time and 64 KiB response limits. Responses are type-checked, known security fields cannot be duplicated, raw provider descriptions are not returned, and operation generations prevent a cancelled or replaced request from completing newer work. Authorization codes, PKCE verifiers, response bodies, and parsed token staging buffers are explicitly cleared where the involved API permits it. Qt and operating-system internals may still make transient copies under the in-memory limitations documented above.

The repository also builds a separate read-only YouTube discovery transport only for its standalone development test.
It sends a move-only access-token buffer solely as a Bearer header to the fixed `channels.list` and
`liveStreams.list` HTTPS endpoints. It requests one bounded page at a time, uses `mine=true`, strictly validates IDs,
labels, page tokens, headers, JSON, TLS, and final URLs, and returns stable error enums rather than provider text. Its
fixed partial-response fields request only IDs and display labels; CDN ingestion data and `streamName` are not requested
or returned by this layer. A later provider must impose total page and cycle limits before following the opaque
continuation value.

The repository also contains a separate selected-stream resolver that is built only for its standalone development
test. It requests one explicitly selected stream ID from the fixed `liveStreams.list` endpoint, rechecks the returned
stream and channel IDs, accepts only the documented `ready` and `inactive` states, and refuses plain RTMP or an
unexpected YouTube ingestion host. Its successful result contains only a validated RTMPS URL and a move-only
`SecureBuffer` holding the current `streamName`. The key is not added to discovery results, settings, snapshots, logs,
diagnostics, or Credential Manager. Response bytes and directly controlled staging buffers are wiped after use;
short-lived copies made internally by Qt remain subject to the in-memory limitation documented above.

The YouTube output owns a private RTMP service and output while retaining explicit references to the native H.264 and main AAC encoders. It never starts or stops the native OBS stream. A YouTube error closes only the YouTube output, leaves Twitch running, and exposes a separate retry action after teardown completes.

The accepted browser-based account connection will add the listener, token transport, discovery transport, and
selected-stream resolver to the product only after the complete flow is ready, together with outbound HTTPS requests
to fixed YouTube API origins and the existing outbound YouTube RTMPS connection. It will not add a cloud relay, local
background service, telemetry, embedded login webview, or long-running listening port. Refresh tokens use a credential
target separate from the manual stream key; access tokens, authorization codes, PKCE verifiers, and resolved stream keys
are never written to profile settings or logs. See [the account-connection design](youtube-account-connection.md).
