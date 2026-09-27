# Architecture

Easy Multistream is being built in small, reviewable slices. The current slice proves only that OBS can load the module and own a standard dock safely. It deliberately contains no streaming output code.

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
      └─ QPointer<DockView> (non-owning after registration)

OBS dock wrapper (OBS-owned)
  └─ DockView (QWidget)
```

The dock contains only a clearly labelled preview. It does not claim that either platform is connected or streaming.

## Lifetime invariants

1. Before `obs_frontend_add_dock_by_id` succeeds, the plugin owns the widget.
2. After registration succeeds, OBS owns the widget; the plugin must never delete it.
3. Closing the dock only hides it and never changes a streaming session.
4. `OBS_FRONTEND_EVENT_EXIT` is the last point at which the plugin removes its callback and dock through the Frontend API.
5. Teardown is idempotent: the callback and dock are removed at most once.
6. The plugin keeps only a `QPointer` to the OBS-owned widget.
7. No private OBS C++ headers, global style sheets, updater, telemetry, or background threads are used.

## Planned runtime boundary

The streaming implementation will be added only after this scaffold is verified inside OBS. Its controller will be owned by the plugin context, not by the dock. The dock will render immutable status snapshots and may be closed without affecting outputs. OBS callbacks and output callbacks will carry a session generation so stale events cannot mutate a newer session.

The future secondary output will reference the native primary encoders but will own its own service and output. Release order and failure paths will be tested before adding automatic reconnect or credential storage.

## Packaging

The repository builds against pinned OBS and dependency archives. Packaging produces an installer-free ZIP with this OBS portable layout:

```text
obs-plugins/64bit/obs-easy-multistream.dll
data/obs-plugins/obs-easy-multistream/locale/...
obs-easy-multistream-LICENSE.txt
```

No OBS, Qt, installer, or third-party runtime DLL is bundled. Build dependencies stay under the repository-local `.deps` directory.
