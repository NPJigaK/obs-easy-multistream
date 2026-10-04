# YouTube account connection

Status: accepted product and architecture direction. The repository contains a headless state, protocol, and
destination-preparation foundation plus production saved-state lifecycle restoration, but interactive account connection
is not yet integrated or exposed in the current build.

## Decision

Easy Multistream will make browser-based YouTube account connection the normal setup path while preserving the current manual RTMPS URL and stream-key path as an advanced fallback.

The intended user experience is:

1. Select **Connect YouTube** in the Easy Multistream dock.
2. Complete Google authorization in the system browser.
3. Select a YouTube channel and an existing reusable encoder stream when necessary.
4. Return to OBS.
5. Start and stop streaming only with OBS's existing controls.

After setup, the dock shows the connected channel and independent Twitch/YouTube delivery status. It does not show OAuth terminology, internal provider names, schema versions, implementation phases, or release milestone names. Manual URL/key fields live under advanced settings.

An account-connection failure never stops the native Twitch stream and never silently falls back to another saved destination. The user must explicitly select the manual connection path if they want to use it.

## Initial account-connection scope

The first complete account-connection implementation discovers an existing reusable YouTube encoder stream and obtains its ingestion information through the official YouTube API:

- `channels.list(mine=true)` identifies the authorized channel;
- `liveStreams.list(mine=true)` lists reusable encoder streams;
- `cdn.ingestionInfo.rtmpsIngestionAddress` supplies the RTMPS server URL;
- `cdn.ingestionInfo.streamName` supplies the secret consumed by the RTMP output.

This removes URL and key copying without initially turning Easy Multistream into a YouTube broadcast-management application. If no compatible stream exists, the dock opens YouTube Studio with a short one-time instruction and offers **Check again**. It does not scrape or automate the Studio website.

Creating broadcasts, choosing titles and privacy for every session, binding streams, transitioning broadcasts live/complete, scheduled events, and deleting or cleaning up YouTube resources are deliberately separate work. Those operations require broader authorization, more quota, explicit user control, and substantially more failure recovery.

Official references:

- [OAuth 2.0 for desktop apps](https://developers.google.com/identity/protocols/oauth2/native-app)
- [YouTube LiveStreams resource](https://developers.google.com/youtube/v3/live/docs/liveStreams)
- [LiveStreams: list](https://developers.google.com/youtube/v3/live/docs/liveStreams/list)
- [YouTube broadcast and stream implementation](https://developers.google.com/youtube/v3/live/guides/implementation/broadcasts-and-streams)

## Architecture boundary

The account provider is separate from the current settings, session, and output layers:

```text
PluginState
  ├─ DockView                         display and user intent only
  ├─ SettingsController              non-secret profile settings
  ├─ YouTubeAccountController        browser/API/token lifecycle
  │   ├─ loopback authorization
  │   ├─ Google token exchange/refresh/revoke
  │   ├─ YouTube channel/stream discovery
  │   └─ profile/channel-scoped refresh-token store
  ├─ RuntimeController               OBS event serialization
  └─ YouTubeOutputAdapter            validated RTMPS URL + ephemeral key only
```

OBS's built-in YouTube OAuth is not reused. OBS exposes no public Frontend API that supplies a second YouTube account or token while Twitch remains the active native service. Depending on OBS private authentication classes, reading its private token fields, or temporarily switching the native service would be brittle and would violate the product requirement that Twitch keep its normal OBS behavior.

The output adapter never receives an OAuth access or refresh token. The account provider resolves the selected stream into an ingest session containing a validated YouTube RTMPS URL and a short-lived in-memory key buffer. The existing output and failure-isolation rules remain unchanged.

Account connection and output delivery are separate states. A channel may be connected while YouTube is not streaming, and a YouTube output may fail without disconnecting the account.

The detached `YouTubeAccountDestinationPreparer` is the boundary immediately before future output integration. For one
selected channel/stream it captures an immutable profile/channel scope and acquires the injected profile-operation lock
before reading the saved refresh credential. It retains that owner-thread lock while refreshing an access token, persisting
a provider-rotated refresh token in the same scope, resolving the selected stream, and finalizing queued completion state.
It releases the lock immediately before invoking an external completion handler. A failed rotation write returns a
credential-unavailable result and does not start resolution. A rotation that was written successfully is retained even if
resolution later fails or the preparation is cancelled; it is not rolled back to an older token. The preparer is
owner-thread-only, uses an independent epoch and attempt, and supports cancellation, context invalidation, shutdown,
stale-callback rejection, and queued value-only completion. A busy lock returns without changing preparer state; a
recovered lock is internal recovery metadata and is not user-facing.
It is currently a detached/testable library and is not instantiated by `PluginState`, `RuntimeController`, the dock, or
the output adapter.

## Desktop authorization

The supported authorization flow follows Google's installed-app guidance:

- launch the system browser, never an embedded login webview;
- bind a temporary listener to `127.0.0.1` first, then use its OS-assigned port in the authorization URL and keep
  the listener bound while the system browser is open;
- accept only the expected callback path and one active attempt;
- bound the HTTP request line, callback URL, header and query sizes before constructing a Qt URL, accept only the
  expected HTTP method, and stop reading on a short timeout;
- generate a random `state` and require an exact callback match;
- use PKCE with `S256` and a fresh verifier for every attempt;
- exchange the code over HTTPS without treating a desktop `client_secret` as a security boundary;
- close the listener after success, rejection, cancellation, or timeout;
- invalidate late callbacks with a generation and attempt lease;
- cancel listener and network requests during profile change and OBS shutdown.

Normal connection requests use offline access without forcing an extra consent prompt. An explicit reconnection can
request `prompt=consent` when a fresh refresh token is required; this distinction is internal and is not shown as OAuth
terminology in the dock.

The browser completion page contains no token or code and only tells the user that they can return to OBS.

## Secret storage

Profile settings may contain only non-secret selection data such as the connection mode, channel ID, stream ID, and display labels. They never contain an authorization code, access token, refresh token, PKCE verifier, or stream key.

The profile codec now implements this non-secret boundary. Older manual profiles load as manual mode; account mode
accepts either a complete, strictly validated channel/stream selection or an explicit setup-required state with no
selection, and does not require a saved ingestion URL. Saving the latter removes stale selection fields without changing
the mode or touching any refresh-token credential. Although the headless provider is now wired through the plugin
lifecycle for restore and local disconnect, the current output path deliberately treats account mode as unavailable
until destination preparation and output handoff are integrated. A partially implemented or imported account profile
therefore cannot silently use the manual URL/key instead.

Windows Credential Manager uses separate boundaries for:

- the existing manual YouTube stream key;
- the Google refresh token, scoped to a SHA-256 binding of the exact active OBS profile path and a SHA-256 binding of
  the selected channel in `CREDENTIALW.UserName`.

The manual stream key remains shared across OBS profiles. The refresh-token store is not shared: duplicate/import/rename
operations and portable-profile path moves create a different profile binding and require reconnecting. No credential is
transferred automatically. Raw profile paths and tokens never enter the refresh-token target, profile export, logs, or
UI. A profile may retain only bounded non-secret channel/stream selection data; the raw channel ID is not copied into
Credential Manager metadata, logs, or UI. Each profile owns one current selected channel credential; reconnecting that
profile replaces that record rather than retaining a collection of channel credentials. The old fixed refresh-token
target is never read, migrated, or deleted.

Access tokens, authorization codes, PKCE values, token endpoint responses, and resolved stream keys remain in wipeable process buffers for the shortest practical lifetime. None is displayed or logged. The detached destination preparer reads the refresh token only for a current preparation, and writes a provider-rotated refresh token to the immutable scope before resolving the selected stream. The current OBS plugin does not instantiate that preparer. The production account owner now has an internal local-disconnect transaction that removes the selected profile's scoped credential and then clears its non-secret selection while preserving account mode. No current UI invokes it. Remote Google revoke remains a separate future explicit account action. A revoked or `invalid_grant` token becomes **Reconnect YouTube**, not an automatic fallback.

As with the existing stream key, this design does not claim resistance to malware running as the same Windows user, live process inspection, or copies made internally by Qt, Windows, or libobs.

## Lifetime and failure rules

Every asynchronous browser, token, and API result carries the account generation and attempt that created it. Results are ignored after cancellation, a new attempt, profile transition, disconnect, or shutdown.

Credential replacement is serialized with the account state. Each provider transaction acquires its injected,
profile-scoped operation lock before authorization begins and holds it through discovery, selection persistence,
credential replacement, and any rollback. Acquisition does not wait: a busy profile returns without mutating provider
state. A recovered mutex is accepted as an internal recovery result, with durable state re-read as required; it is not
shown in the UI. After discovery, the provider revalidates the active
attempt, persists the non-secret profile selection first, then performs the synchronous scoped Credential Manager write
on the owner thread before yielding to Qt's event loop. If the credential write fails, the exact prior selection is
restored. If a later commit invariant fails, both durable stores are restored where possible and failure is reported
closed. The provider and preparer retain the immutable scope captured for the operation; stale callbacks cannot read or
write another profile's credential. If credential storage ever becomes asynchronous, it must use an attempt-scoped
staging target with explicit rollback so cancellation, profile change, or shutdown cannot leave an orphaned replacement
token.

Destination preparation has a separate ordering rule: a rotated refresh token is written synchronously before the
selected-stream resolver is started. A write failure returns a credential-unavailable result and does not resolve or
start an output. A successful write is durable and is not rolled back if the subsequent resolver fails or the attempt
is cancelled. The preparer's epoch and attempt must match at every stage; cancellation, context invalidation, shutdown,
and destruction suppress stale completions and clear active secret-bearing state. The preparer releases its
owner-thread profile lock after rollback/finalization and immediately before the external completion handler runs.
If release fails, it destroys any successful destination and reports a service failure. When mutex ownership remains, it
retains the profile claim until owner-thread shutdown retries the unfinished cleanup. Provider shutdown likewise retries
only the coordinator, ports, or lock steps that did not previously finish.
Local disconnect uses the same profile-scoped boundary for selection and credential changes. Future remote revoke must use it as well.

The provider:

- owns no raw OBS pointer;
- posts value-only results to the plugin UI thread;
- shuts down and destroys its Qt network transports on their owner thread before that thread's event loop ends;
- never touches widgets from a network callback;
- never blocks the OBS UI thread waiting for network or output teardown;
- does not automatically reopen the browser at startup;
- restores a saved selection by checking only scoped credential status, without copying the secret or starting a listener, HTTP request, discovery, or output;
- does not inspect or delete any credential when the current profile has no saved selection;
- does not automatically loop after an authorization or API failure;
- cannot request that OBS stop its native stream.

The dock can be closed without cancelling a completed account connection or changing a streaming session. An active interactive authorization may continue under the plugin-level controller and report its result when the dock is shown again.

## Public-release gates

The account button remains absent until all of the following are true:

- browser callback, PKCE/state validation, cancellation, timeout, and duplicate-callback tests pass;
- refresh, revoke, expired grant, insufficient scope, malformed response, rate limit, and transient network paths are handled;
- channel and reusable-stream selection work without exposing secret identifiers;
- profile changes and OBS exit invalidate stale results;
- secrets are absent from profiles, profile exports, UI snapshots, diagnostics, and logs;
- a Google Cloud project and OAuth consent screen are configured for the official distribution;
- client configuration is supplied by the release process and no client secret, API key, or token is committed to Git;
- privacy policy, terms links, data deletion/revocation behavior, verification requirements, and quota capacity are reviewed;
- real-account tests cover cancellation, multiple channels, no compatible stream, revoked access, key reset, OBS restart, and Twitch continuing after a YouTube-only failure.

Google's Testing audience is not a general-release solution: it is limited to test users and, for non-basic scopes, authorizations and offline refresh tokens expire after seven days. The official build must not present a temporary test configuration as a finished account connection.

References:

- [Google OAuth production readiness](https://developers.google.com/identity/protocols/oauth2/production-readiness/policy-compliance)
- [Manage OAuth app audience](https://support.google.com/cloud/answer/15549945)
- [OAuth verification](https://support.google.com/cloud/answer/13463073)
- [YouTube API Services Developer Policies](https://developers.google.com/youtube/terms/developer-policies)
- [YouTube API quota costs](https://developers.google.com/youtube/v3/determine_quota_cost)

## Implementation order

The internal implementation order is intentionally not shown in the user interface:

1. add a typed, profile/channel-scoped refresh-token store and a testable, headless account state/provider boundary;
2. implement and test PKCE/state generation, the authorization URL, and exact loopback-callback validation;
3. implement the loopback listener, system-browser launch, and fixed-origin HTTPS transport using Qt Network;
4. implement token exchange, refresh/revoke, and YouTube channel/stream discovery;
5. resolve the selected stream into the current RTMPS output boundary;
6. test cancellation, stale callbacks, profile transitions, shutdown, and Twitch failure isolation;
7. add the account UI and keep manual configuration under advanced settings;
8. complete Google policy, verification, privacy, quota, and signed-release gates before recommending it to general users.

The first two items, the loopback listener and injected browser-opener authorization-session portions of item three,
the token transport plus bounded channel/reusable-stream discovery portions of item four, the selected-stream resolver
and detached destination-preparation orchestration in item five, and the interactive
authorization/exchange/discovery/selection/storage orchestration are now present as non-instantiated libraries with
standalone tests. The listener and authorization-session tests use real
local sockets to verify exclusive `127.0.0.1` binding, bounded HTTP parsing, state rejection, one-shot completion,
timeout, request limits, cancellation, bind-and-arm-before-open ordering, browser-open failure, re-entrant completion,
attempt replacement, shutdown, and move-only code/verifier handoff. The browser opener is a fake; the OBS plugin does
not yet wire `QDesktopServices::openUrl`. The token test uses an injected network double to verify exact fixed HTTPS
endpoints, form encoding, no client secret, verified TLS requirements, redirect rejection, bounded strict parsing,
provider-error classification, cancellation, stale-reply rejection, shutdown, and secret-size boundaries. The
discovery test verifies fixed read-only endpoints and query fields, Bearer-header isolation, strict bounded page
parsing, explicit multi-candidate results, channel filtering, stable YouTube error classification, cancellation, and
stale-completion rejection. A separate pager follows opaque continuation values serially, returns only a complete
candidate set, and fails closed on page/item limits, token cycles, cross-page duplicate IDs, timeout, cancellation, or
any page failure. It deliberately requests no CDN ingestion fields or stream keys. The resolver separately
fetches one selected stream and returns its validated RTMPS destination and current key through a move-only result,
without adding the key to discovery data or persistent settings. The destination preparer reads the saved refresh
credential immediately before resolving, persists any rotated token in the immutable operation scope before starting the
resolver, refuses to resolve when that write fails, and retains a successful rotation if a later resolve fails or is
cancelled. Its epoch, attempt,
owner-thread, cancellation, context-invalidation, shutdown, and queued-completion tests reject stale work and keep
secrets out of public state. The headless provider automatically advances a
single channel/stream candidate, requires exact-ID selection for multiple candidates, rejects stale work by epoch and
lease, and persists the non-secret selection before the scoped refresh token, restoring the previous selection if the
token write fails. It can also restore a persisted selection from credential status alone: present is
only locally configured and remains unverified against Google, while missing and unavailable remain distinct. Every
restore creates a new generation, and a profile without a selection remains setup-required without touching any
credential. The production plugin now owns the account provider and profile-operation coordinator behind a lifecycle-only
wrapper: it restores during module/profile load, invalidates before a profile switch, shuts down during exit, and exposes
an internal local-disconnect transaction that is not connected to the dock or plugin main. A future
selection commit must match both the generation and profile binding from the last accepted restore, and it cannot
silently change a manual profile into account mode. The provider's network adapters are constructed, so the module now
depends on Qt Network, but the wrapper supplies no client ID or browser opener and exposes no connection-start method.
It therefore opens no browser or listener, emits no OAuth/API request, does not copy the refresh token out of the
Credential Manager status buffer, and cannot change the dock or start an output. The destination preparer remains
detached. Production client configuration, browser
opening, refresh/remote revoke, interactive output handoff, and all later items stay gated, so a partial connection path cannot appear in
the user interface.

The manual stream-key credential remains shared across OBS profiles. The refresh-token credential is scoped to the SHA-256
binding of the exact active profile path and to the selected channel's SHA-256 `UserName` binding. Restoration therefore
never labels credential presence as a verified connection. A profile duplicate, import, rename, or portable path move has
a different binding and must reconnect; no automatic transfer or cleanup is attempted, and the old fixed target is never
read, migrated, or deleted. Before this path is connected to runtime or UI, these profile lifecycle rules and exact
scope tests must remain covered. The local-disconnect transaction preserves the provider's single-owner serialization
rule: the Windows API cannot conditionally delete a credential by its channel metadata, so two OBS processes must not
mutate the same profile binding concurrently. It deletes the exact scoped credential before clearing the non-secret
selection. A delete failure leaves both stores unchanged and retryable; a profile-save failure after deletion never
recreates the token and leaves the provider unavailable. In all cases, the start-time destination preparer must
successfully refresh and resolve the exact saved channel and stream before an output can be created.

The repository now contains a detached Windows operation-lock foundation for that rule. It derives a `Local\\` named
mutex only from the validated SHA-256 profile binding, never from a raw path, channel ID, or token. Acquisition is
non-blocking, so a busy result leaves provider/preparer state unchanged. A recovered mutex is accepted as an internal
recovery result so durable state can be re-read before continuing; the recovery condition is not user-facing. A
process-wide claim registry prevents Windows' same-thread recursive mutex behavior from admitting two local operations.
The lock is owner-thread-bound, non-copyable, non-movable, and releases both the native handle and the local claim on every terminal
path. Both account classes require the lock provider and hold it across their complete asynchronous transactions,
including rollback; the preparer releases it before invoking an external completion handler. The saved-state restore and
local-disconnect owner and its lock are active in production. Interactive paths remain unreachable from the OBS plugin;
local disconnect is not exposed in the dock.

The `YouTubeAccountProfileRestoreCoordinator` provides the outer profile transaction for both saved-state restoration
and local disconnect and is called by the production lifecycle owner. It first reads a
candidate profile binding without reading the profile config, acquires the matching lock without waiting, re-reads the
active profile path and config while the lock is held, and rejects the transaction if the binding changed. It then calls
the provider's held-lock restore path, so credential status and provider state are read or changed inside the same
critical section without recursively acquiring the mutex. It rechecks the active binding after the provider transition,
then releases the owner-thread lock; an unannounced profile switch invalidates the restored state before release. The
coordinator makes its provider terminal during owner-thread destruction even if a final native cleanup retry fails. A
synchronous callback cannot release that lock early: reentrant invalidation and shutdown are deferred until the
provider transition completes, while a nested restore returns busy. Busy or unavailable acquisition leaves
context/provider state untouched; a profile race, invalid or future settings, and native release failure fail closed. A
recovered mutex is treated as held for the reread but is retained only as internal metadata. The coordinator exposes
only copied non-secret selection/provider values. Active-profile restore/invalidation/shutdown and the internal local-
disconnect seam are now wired. Interactive connection, account UI, and remote revoke remain release-gated and must use
the same profile lock. No current user action calls local disconnect.

No account UI is added merely to advertise unfinished functionality. A build without a complete configured provider continues to show only the working manual setup.
