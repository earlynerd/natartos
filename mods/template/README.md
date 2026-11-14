# Template mod

The smallest working natartos mod — copy it to start your own.

## What a mod is

A mod is an `LD_PRELOAD` shared object injected into `dji_glasses` by the loader.
It can do either or both of:

- **(A) Interpose a libc function** — override `open`, `pcm_write`, … to change
  behaviour (redirect a file, mute audio, draw to the screen).
- **(B) Add a More-menu entry** — register a tap action via `natartos_mod_register`.

Mods are self-contained: no libc, only the raw ARM64 syscalls and helpers in
[`include/natartos_common.h`](../../include/natartos_common.h).

## Make your own

1. Copy this folder: `cp -r mods/template mods/mymod`
2. Rename `mod_template.so` → `mod_mymod.so` in `mod.c` (soname) and `mod.json` (`so`, `id`, `name`).
3. Edit `mod.c`:
   - keep block **(A)** and replace `open` with the function you want to hook, or delete it;
   - keep block **(B)** to show a menu item, or delete it.
4. Build:
   ```sh
   aarch64-linux-gnu-gcc -shared -fPIC -O2 -nostdlib -I../../include \
     -Wl,-soname,mod_mymod.so -o mod_mymod.so mod.c
   ```
5. Deploy with toolya:
   ```sh
   toolya natartos push --local <dir>   # copies the .so + .json to the goggles
   ```

## Toggle at runtime

This template is a `tweak`: it acts only while its flag file exists.

```sh
touch /data/local/tmp/natartos_template   # on
rm    /data/local/tmp/natartos_template   # off
```

The menu entry toggles the same flag.

## Descriptor

Every mod ships a `mod.json` next to its `.so`. See [MOD_SPEC.md](../../MOD_SPEC.md)
for all fields and mod types (`app` / `tweak` / `theme` / `hook`).

## Reference mods

- [`mods/background`](../background) — simplest `open()` interpose + menu toggle.
- [`mods/mute`](../mute) — `pcm_write` interpose with a submenu of flags.
