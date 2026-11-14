# Custom Background

Replaces the idle-screen background image of the goggles.

## How it works

Intercepts `open()`/`open64()` via LD_PRELOAD.
If the path is `/system/etc/extern_bitmap/background_disconnect.jpg` and the flag
is active, redirects to `/vendor/custom/extern_bitmap/background_disconnect.jpg`.

Requires being in LD_PRELOAD (not just dlopen) — symbol interposition.

## Configuration

| Flag | Effect |
|------|--------|
| `touch /data/local/tmp/natartos_background` | Custom background |
| `rm /data/local/tmp/natartos_background` | Stock background |

The switch takes effect on the next launch of dji_glasses.

## Custom image

Replace the file:
```
/vendor/custom/extern_bitmap/background_disconnect.jpg
```
JPEG, recommended size 1920x1080.
