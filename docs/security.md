# Security and secret handling

Easy Multistream treats the YouTube stream key as a password. The current implementation creates one YouTube RTMPS output only after the native OBS Twitch stream has started, while keeping the key out of OBS profile files and plugin logs.

## Where data is stored

The active OBS profile stores only these non-secret values in `basic.ini`:

```ini
[EasyMultistream]
SchemaVersion=2
YouTubeEnabled=false
YouTubeServerUrl=rtmps://a.rtmps.youtube.com/live2
```

The YouTube stream key is stored as a Windows Generic Credential for the current Windows account. It is not stored in `basic.ini`, scene collections, profile exports, plugin logs, or diagnostic text. One credential is shared by all OBS profiles; the dock states this explicitly. The non-secret Stream URL is profile-scoped.

Deleting the credential is an explicit, confirmed action. It does not rewrite any profile's non-secret enabled flag. Enabled profiles remain configured but cannot stream until a new shared key is saved.

## In-memory boundary

The key necessarily exists briefly in process memory while the user enters it and while it is passed to Windows Credential Manager. The plugin:

- uses a password-mode editor with copy, cut, drag, drop, and context-menu export disabled;
- clears the editor after a successful save, on profile changes, when the dock is hidden, and during shutdown;
- wipes temporary UTF-8 and credential buffers before releasing them;
- never reads a saved key back into the editor or a UI status snapshot;
- never includes a key in an exception or log message.

Qt, OBS, Windows, libobs service settings, and the process allocator can still retain transient copies outside the plugin's direct control. In particular, libobs must copy the key into the temporary RTMP service configuration, and the public OBS config API can remove an accidentally present legacy plaintext field from the persisted profile but does not securely erase the freed config heap allocation. This project therefore does not claim resistance to live process-memory inspection or crash-dump forensics.

## Credential Manager boundary

Windows Credential Manager protects the key for the signed-in Windows user and keeps it on the local computer. It is not a hardware-backed vault, does not protect against malware running as the same user, and is not a substitute for rotating a key after suspected compromise.

The plugin uses the documented Generic Credential size limit, validates the key before writing it, treats a missing credential as a normal state, and treats access-denied or unavailable credential services as a non-streaming failure. Development tests use an injected fake API and do not write test credentials to the developer's global Credential Manager.

## Network surface

The current build performs no OAuth, HTTP API, update, telemetry, relay, or inbound-network operation. When YouTube is enabled and OBS confirms that a recognized native Twitch stream is running, the plugin opens one outbound RTMPS connection using OBS's `rtmp_output`. URL validation requires `rtmps://`, port 443 when a port is present, a single application path, and a host under `*.rtmps.youtube.com`; this prevents an imported profile from sending the saved YouTube key to an arbitrary RTMPS host.

The YouTube output owns a private RTMP service and output while retaining explicit references to the native H.264 and main AAC encoders. It never starts or stops the native OBS stream. A YouTube error closes only the YouTube output, leaves Twitch running, and exposes a separate retry action after teardown completes.

The accepted browser-based account connection will add a narrow network surface only after it is complete: a temporary listener bound to `127.0.0.1`, outbound HTTPS requests to fixed Google OAuth and YouTube API origins, and the existing outbound YouTube RTMPS connection. It will not add a cloud relay, local background service, telemetry, embedded login webview, or long-running listening port. Refresh tokens use a credential target separate from the manual stream key; access tokens, authorization codes, PKCE verifiers, and resolved stream keys are never written to profile settings or logs. See [the account-connection design](youtube-account-connection.md).
