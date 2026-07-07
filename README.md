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

### v1.0.7 -- TikTok Bridge import
- **Import TikTok Bridge** button in the Endpoint dialog -- reads local RTMP data from a Bridge JSON file
- Designed for TikTok's account-gated / ephemeral stream-key workflow
- StreamMulticast does not generate TikTok keys, perform TikTok login, or bundle third-party generators
- Default handoff path: `%APPDATA%\obs-studio\plugin_config\streammulticast\tiktok_bridge.json`

### v1.0.7 -- Stability & reliability
- Fixed a UI freeze when deleting/editing an endpoint or exiting OBS while one of its servers had stopped responding — output teardown now runs on a dedicated background thread instead of blocking the dock
- Fixed config autosave silently stopping after the first save (a data-loss risk if OBS crashed before the next manual save)
- An invalid or rejected stream key now fails fast with a clear "Invalid stream key" error instead of retrying to reconnect forever
- Fixed the Health tab's uptime column, which previously always showed 0
- Internal thread-safety hardening around output start/stop/reconnect and health polling

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
