# natartos autostart deployment

## Problem
Android 9 does not support `override` in RC files.
`/system` is protected by dm-verity (remount is impossible).

## Solution
Trigger-based inject: when `dji.glasses_service=1`, the `natartos_inject` service
kills the stock dji_glasses and restarts it with LD_PRELOAD.

## Files
- `natartos.rc` → `/vendor/etc/init/natartos.rc`
- `inject.sh` → `/vendor/natartos/inject.sh` (chmod 755)

## Deploy

```sh
toolya natartos push --local dist             # deploy .so files
toolya natartos install --local dist --yes    # full install (remount, push, chmod, cleanup)
```

## After deploy
A reboot is required. After reboot you also need to re-enable ADB (enable_adb.py).

## How it works
1. On boot, `dji.glasses_service=1` → the stock trigger starts dji_glasses
2. Our trigger starts `natartos_inject` (oneshot)
3. inject.sh waits 3s, kills the stock dji_glasses, restarts it with LD_PRELOAD
4. natartos_loader.so loads and pulls mods from /vendor/natartos/mods/
