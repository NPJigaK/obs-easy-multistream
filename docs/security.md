# Security and secret handling

Easy Multistream treats the YouTube stream key as a password. The current pre-alpha does not create any streaming output or network connection, but it already enforces the storage boundary intended for v1.

## Where data is stored

The active OBS profile stores only these non-secret values in `basic.ini`:

```ini
[EasyMultistream]
SchemaVersion=1
YouTubeEnabled=false
```

The YouTube stream key is stored as a Windows Generic Credential for the current Windows account. It is not stored in `basic.ini`, scene collections, profile exports, plugin logs, or diagnostic text. The current v1 design uses one credential shared by all OBS profiles; the dock states this explicitly.

Deleting the credential is an explicit, confirmed action. It does not rewrite any profile's non-secret enabled flag. Enabled profiles remain configured but cannot stream until a new shared key is saved.

## In-memory boundary

The key necessarily exists briefly in process memory while the user enters it and while it is passed to Windows Credential Manager. The plugin:

- uses a password-mode editor with copy, cut, drag, drop, and context-menu export disabled;
- clears the editor after a successful save, on profile changes, when the dock is hidden, and during shutdown;
- wipes temporary UTF-8 and credential buffers before releasing them;
- never reads a saved key back into the editor or a UI status snapshot;
- never includes a key in an exception or log message.

Qt, OBS, Windows, and the process allocator can still retain transient copies outside the plugin's direct control. In particular, the public OBS config API can remove an accidentally present legacy plaintext field from the persisted profile, but it does not offer secure erasure of the freed config heap allocation. This project therefore does not claim resistance to live process-memory inspection or crash-dump forensics.

## Credential Manager boundary

Windows Credential Manager protects the key for the signed-in Windows user and keeps it on the local computer. It is not a hardware-backed vault, does not protect against malware running as the same user, and is not a substitute for rotating a key after suspected compromise.

The plugin uses the documented Generic Credential size limit, validates the key before writing it, treats a missing credential as a normal state, and treats access-denied or unavailable credential services as a non-streaming failure. Development tests use an injected fake API and do not write test credentials to the developer's global Credential Manager.

## Current network surface

The configuration preview performs no HTTP, OAuth, update, telemetry, RTMP, or RTMPS communication. Future YouTube output code will be reviewed separately before this pre-alpha notice is removed.
