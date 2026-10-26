# fyewebcam protocol

The phone runs a TCP server on port **4747**. The Windows client connects to it, either over
Wi-Fi (the phone's IP) or over USB (`adb forward tcp:4747 tcp:4747`, then `127.0.0.1:4747`).
There is one connection per viewer. All integers are little-endian.

## 1. Handshake

Client to phone, 11 bytes:

| Bytes | Field | Notes |
|---|---|---|
| 0–3 | `"PWCM"` | magic |
| 4 | version (u8) | `1` |
| 5–6 | width (u16) | requested, for example 1280 |
| 7–8 | height (u16) | requested, for example 720 |
| 9 | fps (u8) | requested, for example 30 |
| 10 | camera (u8) | `0` back, `1` front, `255` keep the camera picked on the phone (what the Windows client sends) |

Phone to client, 11 bytes:

| Bytes | Field | Notes |
|---|---|---|
| 0–3 | `"PWCM"` | magic |
| 4 | status (u8) | `0` ok, `1` bad request, `2` camera error |
| 5–6 | width (u16) | what the phone actually streams |
| 7–8 | height (u16) | |
| 9 | fps (u8) | |
| 10 | codec (u8) | `1` H.264 |

The phone picks the closest preset it supports (480p, 720p, 1080p), so the reply can differ
from the request. A connection that fails the handshake (wrong magic or version) is dropped
without disturbing the current viewer; a valid one replaces it.

## 2. Video frames (phone to client, repeating)

```
type:u8 | pts_us:u64 | length:u32 | payload[length]
```

| type | Meaning |
|---|---|
| 1 | Config: H.264 SPS and PPS (Annex B, with start codes) |
| 2 | Keyframe (IDR) |
| 3 | Other frame |

The phone sends a config packet when a viewer connects and every time the encoder restarts
(resolution or camera change). A client should start a new decoder on each config packet and
wait for the next keyframe. Keyframes come every 2 seconds. If the client falls behind, the phone
drops frames until the next keyframe instead of buffering.

## 3. Control messages (client to phone, any time after the handshake)

```
type:u8 | length:u16 | payload[length]
```

| type | Payload | Meaning |
|---|---|---|
| `0x10` | camera:u8 | Switch camera (`0` back, `1` front) |
| `0x11` | width:u16, height:u16, fps:u8 | Change resolution |
| `0x12` | none | Request a keyframe |
| `0x1F` | none | Goodbye, the client is disconnecting |
