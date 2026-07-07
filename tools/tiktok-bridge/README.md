# StreamMulticast TikTok Bridge Companion

This is a small local companion for the optional TikTok workflow.

It does not generate TikTok stream keys, perform TikTok login, or inspect other
processes. It only writes RTMP data supplied by the user, clipboard, or another
local helper into StreamMulticast's handoff file:

```text
%APPDATA%\obs-studio\plugin_config\streammulticast\tiktok_bridge.json
```

Then open the StreamMulticast endpoint dialog and click `Import TikTok Bridge`.

## Examples

Prompt for server URL and stream key:

```powershell
.\StreamMulticast.TikTokBridge.ps1
```

Read JSON or simple server/key text from the clipboard:

```powershell
.\StreamMulticast.TikTokBridge.ps1 -FromClipboard
```

Write explicit values:

```powershell
.\StreamMulticast.TikTokBridge.ps1 `
  -ServerUrl "rtmp://push-rtmp.tiktokcdn.com/live" `
  -StreamKey "your-temporary-key" `
  -ExpiresAt "2026-06-08T22:00:00Z"
```

Optionally start an external helper chosen by the user:

```powershell
.\StreamMulticast.TikTokBridge.ps1 `
  -HelperPath "C:\Tools\SomeHelper.exe" `
  -FromClipboard
```

The bridge file format is:

```json
{
  "name": "TikTok Bridge",
  "server_url": "rtmp://push-rtmp.tiktokcdn.com/live",
  "stream_key": "your-temporary-key",
  "expires_at": "2026-06-08T22:00:00Z"
}
```

`server` can be used instead of `server_url`, and `key` can be used instead of
`stream_key`.
