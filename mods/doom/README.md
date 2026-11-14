# Doom

A port of doomgeneric (ozkl/doomgeneric) for the DJI Goggles 3.

## Build

aarch64 cross-compile in Docker. Needs the full doomgeneric repo plus this shim:

```bash
# In a gcc:13 container with gcc-aarch64-linux-gnu:
make -f Makefile.goggles
# → doom_goggles (~1.3 MB, static)
```

## Files on the goggles

| Path | What |
|------|------|
| `/data/local/tmp/doom_goggles` | binary (1.26 MB) |
| `/data/local/tmp/freedoom1.wad` | Freedoom 0.13.0 IWAD (28 MB) |

## How it works

- `doomgeneric_goggles.c` — platform shim (5 callbacks)
- Renders 320×200, scaled 5× → 1600×1000 centered in `/data/local/tmp/fb_shared` (BGRA 1920×1080)
- hook_debug.so reads fb_shared and pushes it to the display via a DMA-buf override
- Input: `/dev/input/event0` (gpio_keys) + `/dev/input/event3` (omc_keys)
- UP/DOWN/LEFT/RIGHT = navigation, CENTER = fire/enter, BACK = esc

## Dependencies

- `hook_debug.so` with the fb_override inject thread
- The fb_override flag must be enabled
