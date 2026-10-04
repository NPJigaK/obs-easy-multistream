# YouTube setup

This note describes the setup for the current YouTube RTMPS output. The implementation is available in development builds, but the first real Twitch + YouTube validation should use a dedicated OBS profile and a private or unlisted YouTube stream.

## Recommended workflow

The simple workflow uses one YouTube encoder stream. OBS sends one horizontal stream to YouTube, and YouTube can create the vertical feed on its side. This keeps the local OBS workload and the Easy Multistream state model small: there is no second local video encoder or second OBS scene.

To set it up:

1. Select **Open YouTube Studio** in the Easy Multistream dock. It opens YouTube's live dashboard in the default browser and uses that browser's existing sign-in session; the plugin does not read the browser's credentials.
2. Open **Stream**. If the direct link does not reach that screen, choose **Create → Go live → Stream** in YouTube Studio. Do not use a scheduled event for this workflow.
3. Make sure live streaming is enabled for the channel. YouTube may require verification or an initial activation period before the first stream.
4. Create a custom stream key, or select an existing reusable custom stream key.
5. Enable YouTube **Auto-start** and **Auto-stop** for the stream.
6. In **Stream settings**, use the lock control to show the encrypted RTMPS URL, then copy that **Stream URL** into Easy Multistream. For safety, the plugin accepts only YouTube RTMPS ingestion hosts and does not accept a stream key inside the URL.
7. Enter the matching stream key in Easy Multistream. The key is stored as a secret and is not written to the OBS profile.
8. If a vertical version should also appear in the YouTube Shorts feed, enable **Dual stream** in the Live Control Room before starting. For the initial workflow, leave the vertical preview set to **Auto**. YouTube creates the 9:16 feed from the horizontal input, normally using a centre crop.
9. For the first test, set the YouTube visibility to **Private** or **Unlisted**, start OBS, and check both the horizontal player and the vertical preview before using a public stream.

The regular OBS Start/Stop button remains the only local action. Once configured, Easy Multistream sends the same horizontal H.264/AAC stream to YouTube while OBS continues to use its native Twitch settings.

## What is not part of the first workflow

The YouTube Live Control Room also has an `Encoder` option for the vertical preview. That option is intended for sending a separately composed vertical feed with a second stream key. It requires a separate 9:16 render/encode path, additional local resources and upload bandwidth, and more output lifecycle state. Easy Multistream's first integration deliberately uses YouTube's automatic mode instead.

YouTube's vertical format must be enabled before the stream starts; it cannot be added after the stream is already live. The automatic version is normally a centre crop, so important content should remain near the centre of the horizontal layout. A custom portrait composition can be considered later after the one-output workflow has been tested.

## Official references

- [Create a YouTube live stream with an encoder](https://support.google.com/youtube/answer/2907883?hl=en)
- [YouTube encoder settings and Auto-start/Auto-stop](https://support.google.com/youtube/answer/9854503?hl=en)
- [YouTube: stream in both horizontal and vertical formats](https://support.google.com/youtube/answer/2474026?hl=en)
- [YouTube: stream across platforms](https://support.google.com/youtube/answer/16404722?hl=en)
