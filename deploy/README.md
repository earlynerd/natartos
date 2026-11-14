# natartos deployment

## Per-session start (v0.1.0)

Mods do not auto-start after reboot. Each session requires manual start.

## Files
- `inject.sh` → `/vendor/natartos/inject.sh` (chmod 755) — kills stock
  dji_glasses, restarts with LD_PRELOAD, respawn loop with crash guard

## Deploy

```sh
toolya natartos install          # install from latest release
toolya natartos start            # activate mods this session
```

## After reboot

1. `toolya adb --yes` — re-enable ADB via DUML
2. `toolya natartos start` — re-activate mods

Installed files on `/vendor` survive reboots. Only start needs repeating.

## How it works
1. `inject.sh` stops stock `dji_glasses`
2. Rebuilds `LD_PRELOAD` path (loader + enabled mods)
3. Starts `dji_glasses` with `LD_PRELOAD`
4. Respawn loop: if process exits, restarts (up to 5 crashes in 120s)

## Archived (non-working)

The following files were part of the persistence approach and do not work:
- `natartos.rc` — vendor init trigger (vendor `.rc` not parsed by DJI init)
- `adb_persist.sh` — ADB re-enable at boot (depends on init trigger)

See `.agent/persistence-archive/` for copies and documentation.
