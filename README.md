# StreamMulticast

> Multi-platform streaming for OBS Studio — **per-output bitrate**, **per-output orientation** (horizontal + vertical in parallel), free, open-source, no account, no cloud.

The features Aitum Multistream charges for, built right into OBS.

---

## What it does

Stream to multiple RTMP endpoints (Twitch, YouTube, Facebook, Kick, Trovo, custom) simultaneously from a single OBS instance, with **independent bitrate, encoder backend, and frame orientation per output**.

**Example** — a 10 Mbit/s upload pipe can power:

| Endpoint | Encoder | Bitrate | Audio | Orientation |
|---|---|---|---|---|
| Twitch primary | NVENC h.264 | 6000 kbit/s | 160 kbit/s | Source (16:9) |
| TikTok / Shorts | x264 (CPU) | 2500 kbit/s | 96 kbit/s | Vertical 1080×1920 |
| YouTube backup | NVENC h.264 | 4500 kbit/s | 128 kbit/s | Source (16:9) |

All three run **at the same time**, from the same OBS scene.

---

## Why another multistream tool?

| Tool | Per-output bitrate | Per-output orientation | Free | Open-source | Local only |
|---|---|---|---|---|---|
| `obs-multi-rtmp` | ❌ | ❌ | ✅ | ✅ | ✅ |
| Aitum Multistream | 💰 (Pro) | 💰 (Pro / separate canvas plugin) | partial | ❌ | ✅ |
| Restream / StreamYard / Castr | ✅ | ✅ | ❌ | ❌ | ❌ (cloud re-encode) |
| **StreamMulticast** | ✅ | ✅ (Letterbox v1.0, Center-Crop v1.1) | ✅ | ✅ | ✅ |

---

## Features

### v1.0 — Core multistream
- **Multi-RTMP output** — N endpoints, no hard limit
- **Per-endpoint encoder + bitrate** — x264 / NVENC / QSV / AMF, video bitrate 500-25000 kbit/s, audio bitrate 64-320 kbit/s, keyframe interval 1-10 s
- **Live Health view** — per-output actual bitrate, dropped frames, reconnect count, uptime in a real-time grid inside the OBS dock (2 Hz polling)
- **Coexistence with OBS native streaming** — Twitch markers, replay buffer and recording stay untouched
- **Auto-reconnect with backoff** — 1s / 2s / 5s / 10s / 30s / 60s, max 10 attempts before hard-fail
- **Linked-to-main toggle** — optionally start/stop selected endpoints with OBS's main "Start Streaming"
- **Pre-defined endpoint templates** — Twitch, YouTube, Facebook, Kick, Trovo, custom RTMP

### v1.0.5 — Per-output orientation
- **Vertical 1080×1920 (Letterbox)** — stream the same OBS scene to a TikTok / YouTube Shorts / Instagram Reels / Facebook Reels endpoint with a 9:16 frame (the horizontal canvas is letterboxed inside the vertical frame)
- Configurable per endpoint — horizontal + vertical run in parallel from the same OBS session
- Center-Crop variant reserved for v1.1 (needs a per-output `obs_view_t` with custom render code)

### v1.0.6 — One-click import from OBS
- **Import from OBS** button in the Endpoint dialog — reads server URL and stream key from your active OBS profile (whatever you've already connected via OBS's native "Connect Account" for Twitch / YouTube / Facebook / Kick / Trovo / Custom RTMP)
- No OAuth flow needed inside StreamMulticast — we piggyback on OBS's own connection
- Handles OBS 28-31 config layout (`global.ini` legacy + `user.ini` modern)

### v1.0.7 — Stability & reliability release
- Fixed a UI freeze when deleting/editing an endpoint or exiting OBS while one of its servers had stopped responding — output teardown now runs on a dedicated background thread instead of blocking the dock
- Fixed config autosave silently stopping after the first save (a data-loss risk if OBS crashed before the next manual save)
- An invalid or rejected stream key now fails fast with a clear "Invalid stream key" error instead of retrying to reconnect forever
- Fixed the Health tab's uptime column, which previously always showed 0
- Internal thread-safety hardening around output start/stop/reconnect and health polling

**TikTok Bridge import**
- **Import TikTok Bridge** button in the Endpoint dialog -- reads local RTMP data from a Bridge JSON file
- Designed for TikTok's account-gated / ephemeral stream-key workflow
- StreamMulticast does not generate TikTok keys, perform TikTok login, or bundle third-party generators
- Default handoff path: `%APPDATA%\obs-studio\plugin_config\streammulticast\tiktok_bridge.json`

## Not in v1.0.x (planned for v1.1+)

- Per-output **resolution + FPS** (currently global)
- Per-output **Vertical Center-Crop** (9:16 slice, not just letterboxed)
- Per-output **health-score** with auto-failover routing
- **Stream info push** (set title / tags / category across platforms via OAuth)
- Per-output **recording split**
- **Discord webhook alerts** on output failure
- **macOS + Linux** builds

---

## Installation (30 seconds)

1. Download the latest `StreamMulticast-Windows-x64.zip` from the [Releases page](https://github.com/avanatro/StreamMulticast/releases)
2. Close OBS if it's running
3. Extract the ZIP into your OBS install — typically `C:\Program Files\obs-studio\` (the ZIP mirrors `obs-plugins\64bit\` and `data\obs-plugins\streammulticast\` so it merges cleanly)
4. Start OBS, then **View → Docks → Multistream**

That's it. Building from source is **only** for contributors who want to modify the code — see the [collapsed section at the bottom](#build-from-source-contributors-only).

---

## Usage

1. Open the **Multistream** dock (`View → Docks → Multistream`)
2. **Configure** tab → **+ Add Endpoint**
3. Either click **Import from OBS** to pull the server URL + stream key from your active OBS profile, OR pick a template and paste the stream key manually
4. For TikTok, optionally click **Import TikTok Bridge** to read a local bridge handoff file
5. Choose **Output Orientation**:
   - `Source (match OBS canvas)` — your normal horizontal stream
   - `Vertical 1080×1920 — Letterbox` — for TikTok / Shorts / Reels endpoints
6. Pick encoder backend + bitrate
7. Decide whether this endpoint auto-starts with OBS's main "Start Streaming" (the **Linked to main** checkbox)
8. Save, then switch to the **Health** tab to watch all outputs live

**Parallel horizontal + vertical example:** add Endpoint 1 = Twitch (Source orientation, NVENC 6 Mbit), Endpoint 2 = TikTok (Vertical 1080×1920, x264 2.5 Mbit), enable Linked-to-main on both → click OBS's native "Start Streaming" and both go live at once.

Pair with [Stream Health Doctor](https://tools.avanatro.com/stream-health/) for deeper per-output telemetry in a separate browser window on a second monitor.

### Editing the vertical canvas in its preview

The **StreamMulticast Vertical** dock preview can be edited like the OBS main preview: click an item to select it (the Sources tab row follows, and selecting a row highlights the item), drag to move it (snaps to the canvas edges, the centre lines and other items' edges; hold **Ctrl** to move freely), drag a corner handle to resize with the aspect ratio kept (**Shift** = free) or an edge handle to resize one axis. Arrow keys nudge by 1 px (**Shift** = 10 px), **Esc** cancels a running drag, **Ctrl+Z** undoes the last drag or the last series of arrow nudges (one step only; the OBS undo history covers the main canvas only). **Delete** does nothing in this preview. Locked and hidden items cannot be clicked; rotation, alignment and crop are kept as they are (use the transform panel or OBS for those).

### Safe-zone overlay (TikTok / Shorts)

The combo box **Safe zone** above the vertical preview shades the areas that the TikTok or YouTube Shorts app covers with its own interface (title bar, caption and comments, the button column on the right) in translucent red and outlines the remaining safe rectangle with a thin dashed white line. Dragging an item in the preview snaps its edges to that rectangle (hold **Ctrl** to move freely). The overlay is drawn in the preview only: it is never part of the canvas, so it does not appear in the stream or in a recording. The choice is remembered per user (OBS user config, section `StreamMulticast`, key `SafeZoneProfile`).

**The numbers are approximate.** We found no official margins for TikTok LIVE or for organic Shorts. Both sets below are for a 1080x1920 frame and are scaled linearly to other canvas sizes. They live in one table in `src/core/SafeZone.cpp`:

| Profile | Top | Bottom | Left | Right | Source |
|---|---|---|---|---|---|
| TikTok (approx.) | 240 | 660 | 120 | 120 | TikTok's In-Feed Ads template (720x1280, scaled) as published by [socialync.io](https://socialync.io/tools/safe-zones/tiktok); the right-hand button column (x 900..1080, y 840..1260) is socialync.io's own measurement. Other tools give 160-254 top and 480-707 bottom, so check on your phone once. |
| Shorts (approx.) | 288 | 672 | 48 | 192 | Third-party figures from [socialync.io](https://www.socialync.io/tools/safe-zones/youtube-shorts) and [revid.ai](https://www.revid.ai/youtube-shorts-safe-zone), not verified against Google's page (Google publishes a reference image for vertical video ads only). Ad-oriented; organic Shorts may differ. |

### Editing sources from the vertical dock

The **Sources** tab and a right-click on an item (in the list or in the preview) offer **Properties...** (also a double-click on a row), **Filters...**, **Interact...** (browser sources and other interactive types), **Rename...**, **Lock / Unlock** and a **Transform** submenu (**Fit to canvas**, **Fill canvas**, **Reset transform**, **Center horizontally**, **Center vertically**). **Add source** has two entries: **Existing source...** and **New source...** (pick a type, name it, and its properties open). Sources are global in OBS: a source created here is a normal global OBS source (it is added to the vertical scene only, so the main window lists it under **Add > Existing**), and renaming or changing the properties or filters of a source that the main canvas also uses changes it there too (the rename dialog says so). Only the scene items of the vertical canvas are ever added, removed or moved.

### Saving vertical edits

Vertical edits are saved to the scene collection about 2 s after each change (a continuous series of edits, such as a long drag, still saves at least every 10 s), so an OBS crash does not lose the layout of the vertical scenes. Nothing is saved while a scene collection loads or OBS shuts down; OBS saves on exit itself.

### Vertical replay buffer (clips)

The **Record** tab of the vertical dock has a **Vertical replay buffer** group. While it runs, the last **Length (s)** seconds (30 to 60, default 30) of the vertical canvas are kept in memory, capped at **Max memory (MB)** (64 to 2048, default 512; the tab shows the RAM this uses). With **Run with the main replay buffer** on (the default) it starts and stops together with the OBS replay buffer; **Start vertical replay buffer** runs it by hand. Two hotkeys in **Settings > Hotkeys** save a clip: **StreamMulticast: Save vertical clip** and **StreamMulticast: Save main + vertical clip** (also saves the OBS replay buffer when it runs). They have no default binding (Ctrl+Alt+V and Ctrl+Alt+X are free in the shipped profiles); the bindings are kept in `plugin_config/streammulticast/vertical-record.json`. A clip is an MP4 named `<date> <time>-<vertical scene>.mp4` (the scene active when you pressed the hotkey) in `E:\Videos\Clips\vertical` by default; an existing file is never overwritten, and a folder on drive C: is refused. Pressing save again while a clip is still being written queues one more clip and names the earlier one after the latest press (its time and scene); presses within a fraction of a second become a single clip. A clip still being written when the buffer stops keeps its temporary name `<date> Vertical replay.mp4`. The buffer uses its own encoder (NVENC HEVC CQP 20, x264 CRF 20 when NVENC is missing or refuses the session), so it adds one more NVENC session while it runs. Length, memory and folder apply at the next start.

### Live health alerts

When an endpoint goes **Reconnecting** or fails (**Error**), or while it is live and drops at least 2 % of its frames over the last 10 seconds (and at least 10 frames), or its bitrate stays below half of its target for 15 seconds, the **Health** tab shows it at once: the endpoint row turns red (the tooltip names the reason) and the tab title reads **Health (!)** for a warning or **Health (!!)** for a failed output. Drop and bitrate checks start 10 seconds after an endpoint went live. An alert goes away after the problem has been gone for 10 seconds, or immediately when you stop the endpoint.

Two options sit below the grid and are saved in `plugin_config/streammulticast/alerts.json`:

- **Play a sound on alerts** (off by default): plays the Windows "Exclamation" sound on this PC, at most once per 30 seconds.
- **Send alerts to obs-websocket (vendor event)** (on by default): each raise and each clear is sent as a vendor event, `vendorName` `streammulticast`, `eventType` `alert`, with `eventData` `{endpoint_id, endpoint_name, level ("warning" or "critical"), reason ("reconnecting", "failed", "dropped_frames" or "low_bitrate"), active (true on raise, false on clear), state}`. A client receives it only if it subscribed to the `Vendors` event category (event subscription bit `1 << 9`). The event never contains a server URL, stream key or token. Without obs-websocket (missing or disabled) the badge and the sound still work. Turning this option on or off affects only later raises and clears: an alert raised while it was off is not sent, and its clear is not sent if the option is turned off before the alert clears.

### Scene transitions on the vertical canvas

When the vertical canvas follows the main scene (**Links** tab), it uses the same scene transition as the main canvas by default: the same type, a copy of its settings and the same duration, started at the same moment as the main transition (a per-scene override transition and its duration are honoured; a quick transition uses the global duration because OBS does not expose the quick transition duration). **Transition:** in the Links tab picks **Follow main (default)**, **Cut** (always instant) or one named OBS transition. For a **Stinger**, the vertical canvas plays a vertical video: **Vertical stinger video** names it (it applies only to the stinger it was chosen for, shown below the field), or leave it empty (**(auto)**) to use `<stinger>-vertical.<ext>` or `stinger-vertical.<ext>` with the same extension next to the main stinger file (a separate-file track matte needs `<matte>-vertical.<ext>` the same way). Without a vertical file the vertical canvas fades over the stinger's transition point instead (OBS would stretch the 16:9 video), and the log says so once. The vertical transition's audio is muted in the mix by design: TikTok and the other vertical endpoints take their audio from the main mix, which already carries the main stinger (and alert) sounds, so the vertical canvas adding its own would play them twice. Its stinger never monitors either. Every OBS transition is prepared on the vertical canvas ahead of use, so a stinger is normally loaded before it plays; a switch within the short moment after a transition was added or edited can show the stinger before its video has loaded (the vertical canvas then changes scene without the stinger picture). **Set active** in the Scenes tab uses the same setting. The old "Wait for stinger transition point" option is gone.

### TikTok Bridge handoff format

Optional helper tools can pass TikTok RTMP data to StreamMulticast by writing:

```json
{
  "name": "TikTok Bridge",
  "server_url": "rtmp://push-rtmp.tiktokcdn.com/live",
  "stream_key": "paste-or-provider-key",
  "expires_at": "2026-06-08T22:00:00Z"
}
```

`server` may be used instead of `server_url`, and `key` may be used instead of `stream_key`. The helper remains separate from StreamMulticast; this plugin only imports the local handoff file.

A minimal Windows companion script lives in `tools/tiktok-bridge/`. It can write
the handoff file from prompts, clipboard content, or explicit command-line
values, and can optionally start a user-chosen external helper.

### TikTok via Streamlabs (optional build flag)

> **Unofficial and off by default.** This uses an unofficial Streamlabs API (it identifies itself as Streamlabs Desktop). It may stop working at any time and may violate the Streamlabs or TikTok terms of service. Use it at your own risk. Release builds do not contain it.

Build with `-DSMULTI_STREAMLABS_TIKTOK=ON` to let a TikTok endpoint fetch its RTMP URL and stream key from Streamlabs every time it goes live, instead of pasting a fresh key by hand:

1. In the endpoint dialog set **Key source** to **Streamlabs TikTok** and connect your Streamlabs account. No separate generator program is needed:
   - **Log in with Streamlabs…** (recommended) opens the Streamlabs login in your web browser. The plugin waits for the browser to come back on a local address (`http://127.0.0.1:<random port>`, reachable only from this PC) and then exchanges the one-time login code for a token (PKCE, the same flow Streamlabs Desktop uses). The status shows `Waiting for login in your browser… (Cancel)`; **Cancel**, closing the dialog or 5 minutes without a login stop the wait.
   - **Use Streamlabs Desktop login** reads the login that Streamlabs Desktop already stored on this PC (`%APPDATA%\slobs-client\Local Storage\leveldb`, newest `.log`, read-only; Streamlabs Desktop may keep running).
   - **More → Import token from generator config…** still imports the `token` from the `config.json` of the *StreamLabs TikTok Stream Key Generator* (the file picker opens in your Downloads folder; picking the StreamMulticast config by mistake is refused with a hint).
2. Whichever way you connect, only the token is kept: it is stored encrypted for your Windows user (DPAPI) in `streamlabs_tiktok.bin` in the plugin config folder, never in `config.json`, never logged and never shown. The login code and secret are never logged either. **Disconnect** deletes the token.
3. The status line shows `Connected as @user — can go live: yes/no`. Set a title and search a category (or keep *Other*).
4. When the endpoint starts, the plugin starts a Streamlabs TikTok session, streams to the URL and key it returns (kept in memory for this session only) and ends the session when the endpoint stops, the main stream stops, the scene collection changes or OBS exits. A failed fetch only stops that endpoint and shows the error in the Health tab. A session left open by a crash is ended before the next start (only its id is kept, in `streamlabs_tiktok_session.txt`).

Saving a change to a live Streamlabs TikTok endpoint restarts it, so the TikTok LIVE is ended and a new one starts.

With the flag off (default) the plugin builds and behaves exactly as before; the TikTok Bridge import above stays the supported path.

---

## Build from source (contributors only)

> **You probably don't need this.** End users should use the pre-built ZIP from [Releases](https://github.com/avanatro/StreamMulticast/releases) — see [Installation](#installation-30-seconds) above. This section exists for people who want to modify the code, audit it, or build for an unsupported platform.

<details>
<summary><strong>Show build instructions</strong></summary>

### Prerequisites

If you already have these installed (typical for C++ developers), the first build takes ~5 minutes:

- **Visual Studio 2022** with *Desktop development with C++* workload
- **CMake 3.28+**
- **Qt 6.5+** (path-discoverable by CMake; obs-deps' Qt6 also works automatically)
- **Git**

If you're starting from scratch (no toolchain): allow ~2-3 h for downloads (VS workload ~5 GB, Qt ~1 GB, libobs build deps ~500 MB). Unattended Qt install via `pip install aqtinstall && aqt install-qt windows desktop 6.5.3 win64_msvc2019_64 --outputdir C:\Qt`.

### Build + install

```powershell
git clone https://github.com/avanatro/StreamMulticast.git
cd StreamMulticast
cmake --preset windows-x64                                  # downloads libobs deps on first run
cmake --build --preset windows-x64 --config RelWithDebInfo  # ~30 sec on warm cache
cmake --install build_x64 --config RelWithDebInfo --prefix "C:\Program Files\obs-studio"   # admin
```

Output: `build_x64\RelWithDebInfo\streammulticast.dll` (≈230 KB). The install step requires admin privileges.

### CI

GitHub Actions builds Windows-x64 on every push to `main` and produces release artifacts when you push a `v*` tag. See `.github/workflows/build-windows.yaml`.

</details>

---

## Architecture

Native C++17 plugin against `libobs`, Qt 6 for the dock. Single-source / multi-encoder design: the OBS main video mix is tapped once, then N parallel `obs_encoder_t` instances feed N parallel `obs_output_t` RTMP outputs. Per-endpoint bitrate, audio bitrate, encoder backend and orientation are independent.

For Vertical-Letterbox endpoints, `obs_output_set_video_conversion()` rescales the output to 1080×1920 with OBS's built-in scale-to-fit (letterbox). Source-orientation endpoints stream the canvas at its native resolution.

Threading: a background `HealthSampler` thread polls each output at 2 Hz; the Qt UI reads thread-safe snapshots on the main thread (Qt::QueuedConnection).

```
src/
├── plugin-main.cpp        Plugin entry, frontend-event handler, dock registration
├── plugin-support.{c,h}   obs_log helper (generated from .c.in via configure_file)
├── core/
│   ├── Endpoint           Per-endpoint POD + serialise/deserialise
│   ├── ConfigStore        JSON persist in OBS plugin_config dir
│   ├── EndpointRegistry   In-memory list + observer pattern
│   ├── ObsServiceImport   Read active OBS profile's stream key (v1.0.6)
│   └── TikTokBridgeImport Read local TikTok Bridge handoff JSON (v1.0.7)
├── pipeline/
│   ├── EncoderFactory     obs_encoder_create for x264 / NVENC / QSV / AMF
│   ├── OutputController   1× per endpoint, state machine, reconnect backoff
│   └── HealthSampler      2-Hz polling thread, telemetry snapshots
└── ui/
    ├── MultistreamDock    Qt dock with two tabs
    ├── HealthTab          Live grid (QTableView)
    ├── ConfigTab          Endpoint cards
    └── EndpointDialog     Modal settings (incl. Import from OBS + Output Orientation)
```

---

## Privacy

StreamMulticast does **not** collect telemetry, contact any servers other than the RTMP endpoints you configure, store data in any cloud, or require an account.

Stream keys are persisted to `%APPDATA%\obs-studio\plugin_config\streammulticast\config.json` in plain text — the same way OBS stores its main stream key. If you are concerned about local-disk exposure, use full-disk encryption (BitLocker on Windows).

---

## License

GPL-2.0-or-later (because libobs is GPL-2.0). See [`LICENSE`](LICENSE).

---

## Author

Built by [Avanatro](https://avanatro.com). Part of the [Avanatro Streamer Tools](https://tools.avanatro.com/) suite:

- [Stream Health Doctor](https://tools.avanatro.com/stream-health/) — free web-based OBS diagnostic HUD
- **StreamMulticast** — this plugin

Contact: contact@avanatro.com — bug reports + feature requests via [GitHub Issues](https://github.com/avanatro/StreamMulticast/issues).
