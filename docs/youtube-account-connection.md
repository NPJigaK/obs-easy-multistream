# YouTube account connection

Status: accepted product and architecture direction. The repository contains a headless state and protocol
foundation, but account connection is not yet integrated or exposed in the current build.

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
  │   └─ refresh-token credential vault
  ├─ RuntimeController               OBS event serialization
  └─ YouTubeOutputAdapter            validated RTMPS URL + ephemeral key only
```

OBS's built-in YouTube OAuth is not reused. OBS exposes no public Frontend API that supplies a second YouTube account or token while Twitch remains the active native service. Depending on OBS private authentication classes, reading its private token fields, or temporarily switching the native service would be brittle and would violate the product requirement that Twitch keep its normal OBS behavior.

The output adapter never receives an OAuth access or refresh token. The account provider resolves the selected stream into an ingest session containing a validated YouTube RTMPS URL and a short-lived in-memory key buffer. The existing output and failure-isolation rules remain unchanged.

Account connection and output delivery are separate states. A channel may be connected while YouTube is not streaming, and a YouTube output may fail without disconnecting the account.

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
requires a complete, strictly validated channel/stream selection and does not require a saved ingestion URL. The
current runtime deliberately treats account mode as unavailable until the asynchronous account provider is complete,
so a partially implemented or imported account profile cannot silently use the manual URL/key instead.

Windows Credential Manager uses separate targets for:

- the existing manual YouTube stream key;
- the Google refresh token.

Access tokens, authorization codes, PKCE values, token endpoint responses, and resolved stream keys remain in wipeable process buffers for the shortest practical lifetime. None is displayed or logged. Disconnect revokes the Google grant when possible and removes the local refresh token and account selection. A revoked or `invalid_grant` token becomes **Reconnect YouTube**, not an automatic fallback.

As with the existing stream key, this design does not claim resistance to malware running as the same Windows user, live process inspection, or copies made internally by Qt, Windows, or libobs.

## Lifetime and failure rules

Every asynchronous browser, token, and API result carries the account generation and attempt that created it. Results are ignored after cancellation, a new attempt, profile transition, disconnect, or shutdown.

Credential replacement is serialized with the account state. After discovery, the provider revalidates the active
attempt, performs the synchronous Credential Manager write on the owner thread, and commits the account state before
yielding to Qt's event loop. If credential storage ever becomes asynchronous, it must use an attempt-scoped staging
target with explicit rollback so cancellation, profile change, or shutdown cannot leave an orphaned replacement token.

The provider:

- owns no raw OBS pointer;
- posts value-only results to the plugin UI thread;
- shuts down and destroys its Qt network transports on their owner thread before that thread's event loop ends;
- never touches widgets from a network callback;
- never blocks the OBS UI thread waiting for network or output teardown;
- does not automatically reopen the browser at startup;
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

1. add a separate refresh-token credential target and a testable, headless account state/provider boundary;
2. implement and test PKCE/state generation, the authorization URL, and exact loopback-callback validation;
3. implement the loopback listener, system-browser launch, and fixed-origin HTTPS transport using Qt Network;
4. implement token exchange, refresh/revoke, and YouTube channel/stream discovery;
5. resolve the selected stream into the current RTMPS output boundary;
6. test cancellation, stale callbacks, profile transitions, shutdown, and Twitch failure isolation;
7. add the account UI and keep manual configuration under advanced settings;
8. complete Google policy, verification, privacy, quota, and signed-release gates before recommending it to general users.

The first two items, the loopback listener and injected browser-opener authorization-session portions of item three, the token exchange/refresh/revocation plus
bounded channel/reusable-stream discovery portions of item four, and the selected-stream resolver in item five are now
present as non-instantiated libraries with standalone tests. The listener and authorization-session tests use real
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
without adding the key to discovery data or persistent settings. None of these libraries is
linked into the OBS plugin, so the product still does not open a browser, listen on a port, make an OAuth/API network
request, or read an account credential. The profile format and runtime fail-closed boundary are present, but the dock
continues to expose only the working manual setup. Production browser-opener/provider integration, candidate-selection
flow, refresh-token vault integration, runtime handoff, and all later items stay gated, so a partial
connection path cannot appear in the user interface.

No account UI is added merely to advertise unfinished functionality. A build without a complete configured provider continues to show only the working manual setup.
