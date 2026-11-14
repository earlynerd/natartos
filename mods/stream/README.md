# PC Stream

Stream PC screen to DJI Goggles 3 over USB (ADB).

## Architecture

```
Browser (getDisplayMedia → canvas → JPEG)
  → ya-webadb subprocess stdin → USB cable →
fb_receiver (read stdin, stb_image JPEG decode, RGBA→BGRA)
  → /data/local/tmp/fb_shared →
hook_overlay.so inject thread (pread → DMA-buf, continuous 60Hz)
  → display
```

No Wi-Fi, no IP, no WebSocket — data flows through the existing ADB/USB connection.

## Protocol

stdin: `[u32le jpeg_size][jpeg_bytes]` per frame, continuously.
Same framing as `fb_stream` uses for output — symmetric.

## Usage

1. `toolya natartos stream` — pushes fb_receiver, starts pipeline
2. Open toolya.dev → natartos → PC Stream → Start
3. toolya spawns `fb_receiver` via ADB, pipes JPEG frames to stdin

## Build

```bash
# In Docker cross-compile container:
aarch64-linux-gnu-gcc -static -O2 -I../../include -o fb_receiver fb_receiver.c -lm
```

## Dependencies

- `hook_overlay.so` with fb_override inject thread (continuous 60Hz DMA-buf write)
- `stb_image.h` v2.30 in `include/` (JPEG decode, public domain)

## Files on goggles

| Path | What |
|------|------|
| `/data/local/tmp/fb_receiver` | static binary (~70 KB) |
| `/data/local/tmp/fb_receiver.log` | runtime log |
| `/data/local/tmp/fb_shared` | shared framebuffer (created by receiver) |
| `/data/local/tmp/fb_override` | inject trigger (set while streaming) |
