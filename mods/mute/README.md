# Audio Control

Mutes goggle sounds by intercepting `pcm_write` (tinyalsa).

## How it works

Intercepts `pcm_write` via LD_PRELOAD. When the flag is active, it zeroes the
audio buffer before passing it to the real `pcm_write`.
The real `pcm_write` is resolved via `dlopen("libtinyalsa.so", RTLD_NOLOAD)`.

Requires being in LD_PRELOAD (not just dlopen) — symbol interposition.

## Configuration

| Flag | Effect |
|------|--------|
| `touch /data/local/tmp/natartos_mute` | Mute EVERYTHING |
| `touch /data/local/tmp/natartos_mute_startup` | Mute the first ~5s after launch |
| `touch /data/local/tmp/natartos_mute_buttons` | Mute short sounds (buttons, UI) |

Priority: mute > mute_startup > mute_buttons. Flags are checked every 256 calls.

## Limitations

- The power-on chime is played by the bootloader BEFORE dji_glasses — not intercepted
- `mute_startup` mutes the first ~300 `pcm_write` calls after dji_glasses launches
- `mute_buttons` detects short sounds by buffer size (<=256 frames)
