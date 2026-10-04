# OBS output integration contract

This note records the ownership and event rules implemented by the YouTube RTMPS adapter and the account-destination handoff that feeds it. The session state library remains free of OBS calls and network behavior; OBS-specific ownership is confined to the runtime bridge, account bridge, and adapter.

## Pinned source basis

The contract was checked against the repository-local OBS Studio 32.2.2 source. Relevant implementation areas are:

- `frontend/OBSStudioAPI.cpp`: frontend output and service access;
- `frontend/widgets/OBSBasic_Streaming.cpp`: frontend streaming event order;
- `frontend/widgets/OBSBasic_Profiles.cpp`: profile event and configuration swap order;
- `libobs/obs-output.c`: output ownership, signals, encoder attachment, stop, and destroy;
- `libobs/obs-encoder.c`: encoder references and per-output callbacks;
- `libobs/obs-service.c`: service references and settings;
- `plugins/obs-outputs/rtmp-stream.c`: RTMP/RTMPS startup, connection, and shutdown;
- `plugins/rtmp-services/rtmp-custom.c`: custom RTMP service settings and protocol detection.

## Start boundary

The adapter may prepare and start YouTube only after `OBS_FRONTEND_EVENT_STREAMING_STARTED`.

1. Obtain a strong reference from `obs_frontend_get_streaming_output()`.
2. Read the native video and first audio encoder, then take explicit encoder references.
3. Reject unsupported or missing encoders without changing the native stream.
4. Read and validate the saved YouTube RTMPS Stream URL and saved YouTube key immediately before use. The URL is copied from YouTube Studio, remains user-editable, and is restricted to a single-label host under `*.rtmps.youtube.com`.
5. Create one `rtmp_custom` service with the validated user-provided RTMPS Stream URL and the temporary key.
6. Create one `rtmp_output`, attach the native encoders, attach the service, and connect signals.
7. Call `obs_output_start()` and treat its `true` return only as an accepted asynchronous start, not as a successful connection. The session remains Connecting until the output's `start` signal is observed.

The adapter must not update, rescale, or replace media on the shared native encoders. It must never call `obs_frontend_streaming_start()` or `obs_frontend_streaming_stop()`.

Before creating the YouTube output, the adapter validates that the shared video/audio configuration is supported, including exactly one H.264 video encoder and an AAC main audio encoder at index 0. A Twitch VOD audio encoder at index 1 is allowed but is not attached to YouTube. AV1, HEVC, unsupported HDR combinations, missing tracks, and multiple native video encoders fail only the YouTube attempt. The YouTube output does not set a scaled size or modify a native encoder setting.

## YouTube Dual stream boundary

The first output adapter sends one horizontal 16:9 stream to YouTube. Users may enable YouTube's automatic Dual stream in the Live Control Room before going live; YouTube then creates the vertical 9:16 feed, normally from a centre crop of the horizontal input. Both views use YouTube's shared live-stream experience, while Easy Multistream owns only one local YouTube output and one shared video encoder.

The adapter does not select YouTube's `Encoder` mode for the vertical preview and does not create a second local stream key, scene, view, or video encoder. Sending a separately composed 9:16 feed through a second key is a future feature with different ownership, GPU/CPU, bandwidth, credential, and state-machine requirements. The vertical format must be configured before the stream starts; it cannot be added after the stream is already live. See the [YouTube setup note](youtube-setup.md) and [YouTube's official dual-stream instructions](https://support.google.com/youtube/answer/2474026?hl=en).

The Frontend bridge assigns a `NativeLease` when `STREAMING_STARTING` is accepted and passes that same value to the corresponding Started, Stopping, and Stopped state events. If a new native attempt begins before an older stop notification is delivered, the bridge retains the stopping lease separately. A late stop for the older lease must never be labeled as belonging to the new attempt. Profile changes invalidate the native generation before new service values are loaded.

OBS 32.2.2 does not emit `OBS_FRONTEND_EVENT_STREAMING_STOPPED` when its native `StartStreaming()` call rejects synchronously. During `STREAMING_STARTING`, the bridge therefore takes a guarded reference to the native output and observes its synchronous `starting` signal. After the frontend callback returns, one queued reconciliation step reports `nativeStartFailed(lease)` if that signal was not observed and the same native lease is still Starting. The reconciliation owns no raw pointer, is generation-checked, and must not infer failure merely from a slow asynchronous connection. A later OBS retry receives a fresh `NativeLease`.

## Account destination handoff

The account destination path has a tested internal handoff into the same output adapter, but the production capability gate is deliberately false until the official OAuth client, consent/verification, and account UI are ready. The current manual RTMPS URL/key path therefore remains the only user-visible and active account configuration path; no browser, OAuth request, account output, or account secret is used by the shipped plugin.

When the capability is eventually enabled, `RuntimeController` starts account preparation only after a recognized native stream is `Streaming`. It records the current `OutputLease` and `NativeLease` as a composite identity. `YouTubeAccountRuntimeDestinationProvider` maps that runtime lease explicitly to the exact `YouTubeDestinationPrepareAttempt` returned by `YouTubeAccountRuntimeOwner`; the two lease types have independent generators and are never converted or assumed to have matching numeric fields. A completion is accepted only while the output lease, native lease, profile generation, connection mode, settings, and native streaming state still match. A stale or mismatched completion is discarded and any successful owner use is released without reaching the adapter.

Pending preparation and active output use are different lifetimes. If Stop, a profile change, or EXIT arrives before an OBS output exists, the bridge cancels the owner preparation and completes the runtime lease without sending an adapter stop request. After preparation succeeds, the account owner holds a destination-use lease while the adapter owns the resolved URL/key. That owner lease is released only after the adapter has emitted `Released` following complete output/service/encoder teardown, and account mutations remain busy while it is held. Switching between manual and account modes also goes through this same output-release barrier before a new mode can start.

The lifecycle order is intentional: `PROFILE_CHANGING` first invalidates/cancels the runtime output-side work, then invalidates the account owner; `PROFILE_CHANGED` loads the new profile after the old lease is no longer usable. During `EXIT`, runtime shutdown and the output release barrier run before the account bridge/provider is shut down, then the owner is made terminal. Worker-originated adapter events enter an owner-thread mailbox before a queued wake-up is requested; a rejected wake-up leaves the event for the next owner-thread operation, so `Released` cannot be silently discarded. Queued callbacks carry only value data plus lifetime/generation guards. Runtime snapshots, profile settings, diagnostics, and logs never contain OAuth tokens, refresh tokens, authorization codes, PKCE values, or resolved stream keys.

## Ownership

One adapter lease owns strong references to:

```text
native streaming output
native video encoder
native audio encoder
YouTube output
YouTube service
signal callback context
```

`obs_output_set_video_encoder()` and `obs_output_set_audio_encoder()` add encoder references to the YouTube output. `obs_output_set_service()` does **not** add a service reference, so the adapter must retain the service until after the output is destroyed.

The normal final release order is executed by a teardown reaper rather than blocking the Qt/OBS UI thread:

```text
invalidate the lease and reject new work
  → request YouTube output stop once
  → return from all OBS signal callbacks
  → disconnect output signals
  → release the YouTube output
  → release adapter-held encoder references
  → release the YouTube service
  → release the native output reference
  → destroy the callback context
  → notify SessionCoordinator with youtubeReleased(lease)
```

## Stop is not teardown completion

`STREAMING_STOPPING` is the point at which the adapter requests a forced stop of the YouTube output through `obs_output_force_stop()`. Waiting until `STREAMING_STOPPED` risks allowing the native encoder lifecycle to advance first.

The OBS output `stop` signal is not a safe destruction boundary. In OBS 32.2.2 it can be emitted while the internal end-data-capture thread is still removing encoder callbacks and deactivating the service. A signal callback therefore copies only non-secret status values and posts work to the owning controller. It must not release the final output reference, service, or callback context inside the callback.

`SessionCoordinator::youtubeReleased()` is the single adapter-level terminal event, including when `obs_output_start()` rejects synchronously. It may be sent only after the raw signal callback has returned and all resources for that `OutputLease` have been released. A rapid OBS restart or explicit retry therefore cannot create a new YouTube output while the previous attempt is still tearing down, and there is no separate ambiguous "start failed" event that could bypass this boundary.

The reaper owns the strong references and callback context until teardown completes. `obs_output_force_stop()` and final output release may wait for OBS RTMP/data-capture threads, so normal teardown runs on the reaper rather than the UI thread. Completion is delivered exactly once. Plugin shutdown synchronously joins the reaper before the module can unload; this favors code-lifetime safety, with the known tradeoff that a stalled network connect can delay OBS shutdown until the underlying OBS timeout completes.

## Signal and thread mapping

`SessionCoordinator` is not thread-safe. All calls are serialized on one owner thread. OBS output callbacks never call it directly.

```text
obs_output_start() returns true  → remain Connecting
starting signal                 → remain Connecting
start signal                    → youtubeStarted(lease)
reconnect signal                → youtubeReconnecting(lease)
reconnect_success signal        → youtubeReconnectSucceeded(lease)
stopping/stop/deactivate        → collect copied status and schedule teardown
reaper finishes full teardown   → youtubeReleased(lease)
```

An output callback copies only the lease, stop code, and sanitized non-secret status needed by the controller. Queued work uses a receiver/lifetime guard and captures no raw output, service, encoder, widget, controller, or callback-context pointer. The callback context is destroyed only after signals are disconnected and every queued item is independent of that context.

## Service detection

Native destination detection uses exact values copied from the active OBS service:

```text
service id == rtmp_common
provider == Twitch              → Twitch
provider == known YouTube name  → YouTube
otherwise                       → Unknown
```

Custom RTMP, relay services, URLs, protocols, and stream-key shapes are never used to guess a platform. `PROFILE_CHANGING` invalidates the current generation, and `PROFILE_CHANGED` obtains fresh service/configuration values.

## Failure isolation

The session effect type can request only `StartYouTube` or `StopYouTube`. There is no effect that can stop the native OBS stream. Synchronous rejection, connection failure, disconnect, unsupported codecs, missing settings, and credential failure all end or fail only the YouTube lease.

Automatic reconnect remains outside the first output adapter. OBS reconnect signals are represented in the state model so they can be added later without changing the ownership boundary.
