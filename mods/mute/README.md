# Audio Control

Mutes goggle sounds by intercepting `pcm_write` (tinyalsa).

## How it works

Intercepts `pcm_write` via LD_PRELOAD (inject.sh preloads every enabled mod).
When a mute flag applies, it writes the same number of bytes of silence to the
real `pcm_write` instead of the caller's samples. The caller's buffer is never
modified. The real `pcm_write` is found with `dlsym(RTLD_NEXT)` (falling back
to an already-loaded `libtinyalsa.so`), and resolution is retried on the first
write if tinyalsa was not loaded yet.

`pcm_write`'s `count` is a byte count; tinyalsa derives the frame count from it.

## Configuration

The **Audio Control** entry in the More menu toggles "mute all".

| Flag | Effect |
|------|--------|
| `touch /data/local/tmp/natartos_mute` | Mute EVERYTHING (the menu toggle) |
| `touch /data/local/tmp/natartos_mute_startup` | Mute the first ~300 `pcm_write` calls after launch |
| `touch /data/local/tmp/natartos_mute_buttons` | Mute short writes (≤1024 bytes, i.e. 256 frames of 16-bit stereo) |

Priority: mute > mute_startup > mute_buttons. Flags are checked every 256 calls,
so a change can take a moment to apply.

## Testing

`tests/sd_apps/mute_check.c` compiles this mod for the host and checks that
unmuted samples pass unchanged, muted writes become silence of the same length
in frame-aligned chunks, the caller's buffer is untouched, and the menu command
toggles the flag the hook reads.

## Limitations

- The power-on chime is played by the bootloader BEFORE dji_glasses — not intercepted
- `mute_buttons` detects short sounds by buffer size; the threshold has not been
  checked against the goggles' actual sounds
