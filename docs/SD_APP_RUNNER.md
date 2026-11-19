# SD app runner

The standalone runner can discover directory packages, validate their
manifests and static executables, stage a verified internal copy, and run one
app at a time. Apps marked `"display": true` get the screen through the
Natartos overlay, the same way Doom and PC Stream do, and give it back when
they exit. The `mod_sd_apps.so` mod adds one More-menu entry per app on the
card. The Hello package draws a screen, reacts to the joystick, and records
each launch in its saved-data directory.

SD Apps is an opt-in development preview for an existing Natartos session.
Use the manual setup below to install it.

## Build

Upstream base: `82f1a7da1748f690eb1031bb391666ec09508278`.
The Windows build uses [Zig 0.15.2](https://ziglang.org/download/), with
`-target aarch64-linux-musl`, static linking, and no PIE. The Python helper
checks that Zig version and keeps its caches inside the ignored `build/` tree.
Pass the executable path from your extracted official Zig download:

```sh
python tools/sd_apps/build.py --zig /absolute/path/to/zig --framework
```

On Linux with `aarch64-linux-gnu-gcc` installed:

```sh
python3 tools/sd_apps/build.py --framework
```

Products are in `build/aarch64/`:

- `build-info.json` records the compiler, upstream base, and hashes of sources
  and build products.
- `sd_app_runner`: the production runner, accepting static ARM64 ELF64
  `ET_EXEC` applications only.
- `apps/hello/`: the complete SD package, including `app.json`, `bin/hello`,
  and its asset.
- `mod_sd_apps.so`: the mod that registers one More-menu entry per SD app.
- `hook_menu.so`: the menu mod with the raised entry limit (32) that the
  per-app entries need; the released 0.1.0 menu allows 8.
- `hook_overlay.so`: the overlay with the SD display acknowledgement and
  ownership leases. Update this together with the runner for display apps;
  an older overlay cannot acknowledge them, so the runner refuses to launch.
- `natartos_loader.so` and `mod_template.so`: build-baseline checks when
  `--framework` is used; they are not needed for SD apps.

The runner and example use ordinary Linux syscalls through statically linked
libc, avoiding the goggles' Android linker/libc dependency. Apps that draw use
the header-only helpers in [`include/natartos_app.h`](../include/natartos_app.h).

The alternative Makefile is at `mods/sd_apps/Makefile`. CI publishes the
development binaries in the separate `sd-apps-bench` artifact.

## Manual setup

1. Use an existing Natartos installation. Back up your installed
   `hook_menu.so` and `hook_overlay.so` so you can restore them.
2. Install the matching `hook_menu.so`, `hook_overlay.so`, and
   `mod_sd_apps.so` in `/vendor/natartos/mods/`. Enable all three using
   the corresponding filenames as markers under `/vendor/natartos/enabled/`.
3. Install `sd_app_runner` at `/data/natartos/bin/sd_app_runner`.
   Set the directory and runner to mode `0700`, owned by root.
4. Copy the complete `build/aarch64/apps/hello/` directory to
   `natartos/apps/hello/` on the card. Insert the card before running
   `toolya natartos start`.
5. Open More, select **Hello**, and press BACK to return. If Hello is missing,
   check `/data/local/tmp/natartos.log` for the SD Apps listing result.

To roll back, restore the saved menu and overlay mods, remove
`mod_sd_apps.so`, its enabled marker, and the runner you installed, then
restart Natartos.

## Packages and commands

Place packages at `<external-card>/natartos/apps/<id>/`. The example's
[app.json](../examples/sd_apps/hello/app.json) defines the schema. Directory
name and manifest ID must match. IDs use lowercase ASCII letters, digits,
underscores and hyphens, starting with a letter or digit. Package paths use
ASCII letters, digits, dots, underscores and hyphens, separated by `/`.
Empty components, `.`/`..`, absolute paths, links, and special files are rejected.

The manifest supports `schema`, `id`, `name`, `version`, `entry`, and optional
`args` and `display` (`true`/`false`, default `false`; see
[Display apps](#display-apps)). Unknown or duplicate fields are errors. Names/arguments support valid
UTF-8 and JSON escapes; names cannot contain control characters and no string
can contain NUL. Maximums: 8 KiB manifest, 48-byte ID, 128-byte name,
64-byte version, 511-byte entry path, and 16 arguments of at most 255 bytes.
The parser rejects dynamic ELF interpreters/dependencies and incompatible
architecture before execution. Shared-library packaging is deferred.

Options precede the action. For a card mounted at `/mnt/media_rw/sdcard0`:

```sh
/data/local/tmp/sd_app_runner --apps /mnt/media_rw/sdcard0/natartos/apps list
/data/local/tmp/sd_app_runner --apps /mnt/media_rw/sdcard0/natartos/apps validate hello
/data/local/tmp/sd_app_runner --apps /mnt/media_rw/sdcard0/natartos/apps stage hello
/data/local/tmp/sd_app_runner --apps /mnt/media_rw/sdcard0/natartos/apps --timeout 10 run hello
/data/local/tmp/sd_app_runner clean hello
```

Without `--apps`, discovery requires exactly one FAT/exFAT mount under
`/mnt/media_rw/`, excluding `emulated`, and uses its `natartos/apps` directory.
It fails on missing or ambiguous mounts. Confirm the actual mount table before
using the example path. `list` emits one JSON object per valid package and
reports skipped invalid packages on stderr.

`menu-list` prints `id<TAB>version<TAB>name` lines sorted by name, for the
menu mod. It checks manifests and executables but skips the full-package hash
so it stays fast, reduces names to printable ASCII (the menu's string API is
ANSI), and stops itself after 5 seconds.

`validate` reads the SD package. `stage` prints the internal cache path after
verification. `run` stages and executes the opened internal executable with
the package as its working directory. It passes argv directly without a shell.
The child's environment contains only `PATH`, `LANG`, `NATARTOS_APP_ID`,
`NATARTOS_APP_PACKAGE` (the verified internal copy it runs from),
`NATARTOS_APP_SOURCE` (the package folder it was staged from, e.g. on the SD
card), and `NATARTOS_APP_DATA` (plus the display variables below); preload
hooks are not inherited. Standard input is `/dev/null`,
stdout/stderr go to the bounded log, and every other inherited descriptor is
closed, since the menu starts the runner from inside `dji_glasses`.

| Storage | Default |
| --- | --- |
| Verified packages | `/data/local/tmp/natartos-apps/packages/<id>/<sha256>/` |
| Saved data | `/data/natartos/appdata/<id>/` |
| Latest app output | `/data/local/tmp/natartos-apps/logs/last-run.log` |

`--cache` and `--data` accept separate absolute roots for testing. Source,
cache, and data roots must not overlap, and roots cannot contain symlinks.
The cache/data roots must belong to the current user and must not be writable
by other users. The device workflow expects the existing authorized root
session; the runner does not enable root, alter policy, or disable verity.

Packages are limited to 64 MiB and 512 files/directories, at most eight
directory levels below the package root. The package cache is capped at
256 MiB of file content, with a 2 MiB free-storage reserve before copying.
The single latest-run log is overwritten each run and capped at 1 MiB;
remaining output is drained. Saved application data has no imposed quota.

Copying uses a private partial directory, sorted SHA-256 tree fingerprints,
re-reading the staged copy, syncing files/directories, and an atomic rename.
File modes are normalized; only the entry executable receives execution
permission. Existing cache entries are re-verified before reuse. If one is
corrupt, `clean <id>` removes that app's cached versions before a retry.
It leaves saved data and unrelated directory names alone. Stale partial
directories for the selected app are removed before staging. Cache cleanup
does not require the SD card and is blocked while a runner is active.

The process lock permits one active runner. The app gets its own process
group; normal exit, crash, cancellation, and timeout trigger group cleanup.
SIGINT/SIGTERM/SIGHUP request termination, escalating to SIGKILL after two
seconds. Timeout is optional, defaults to none, and may be set up to one day.
The direct child is killed if its runner dies. Apps must remain in their
assigned group; daemonizing or detaching children is unsupported. These are
owner-selected privileged programs, with no app sandbox or trust implied by
hash verification.

Exit status is the app's status, `124` for timeout, `125` for runner errors,
or `128 + signal` for cancellation/crash. Inspect stderr and the latest log
to distinguish runner failures from an app returning the same code.

## Menu entries

`mod_sd_apps.so` is an ordinary Natartos mod. When the loader calls its
`natartos_mod_register` during `dji_glasses` start-up, it runs
`sd_app_runner menu-list` and registers one More-menu entry per app through
the existing `natartos_menu_register` API: the app's name, a subtitle with
its version, and the command

```sh
nohup <runner> run <id> >/dev/null 2>>/data/local/tmp/natartos.log &
```

Entry keys are `sd:<id>`, so a card app cannot replace another mod's entry.
The mod re-checks each id against the runner's id rules before placing it in
the command, and registers at most 24 apps. Runner errors (for example a busy
screen or SD permission denial) are appended to the shared Natartos log.

The mod looks for the runner at `/vendor/natartos/apps/sd_apps/sd_app_runner`
first, then `/data/natartos/bin/sd_app_runner`.
With SD Apps enabled, the menu waits for the card scan during startup. A slow
card can add about five seconds before the runner's scan timeout. SD Apps scans
once per `dji_glasses` start. To see newly copied apps, run
`toolya natartos start` again. Check the Natartos log if no entries appear.

## Display apps

With `"display": true` the runner uses the existing Doom/PC Stream frame format
with an additional SD session handshake:

1. Acquire the exclusive `fb_owner.lock` lease, shared across all runner cache
   paths. Refuse an active owner or a legacy app's `fb_override` with exit 125.
   A recognised SD frame whose owner lease is free can be reclaimed. Remove
   its flag and wait up to five seconds for the previous overlay's cleanup.
2. Create a fresh 16,588,864-byte `fb_shared` (mode 0644), write the SD protocol
   marker in its reserved header, then create `fb_override`. Wait up to five
   seconds for the overlay to acknowledge this exact frame inode. Its inject
   thread acknowledges only after finding a display buffer. Missing/old
   overlays and missing display buffers therefore fail with exit 125 **before
   the app starts or grabs any buttons**.
3. Stop the app (SIGTERM, SIGKILL after 2 s, exit 143) if `fb_stop` appears,
   `fb_override` disappears, or the overlay's lease is released by its death.
   Conversely, the overlay detects the runner's death through the owner lease
   and cleans up, even when SIGKILL prevented the runner's own cleanup.
4. After the app's process group is gone, remove `fb_override`. Keep the owner
   lease while the overlay repaints and holds `fb_overlay.lock`, so cleanup
   cannot remove a subsequent app's files. Wait at most five seconds; if the
   overlay is still busy, leave its files for cleanup and make the next launch
   wait for it. If both processes died, the next launch reclaims the stale SD
   files and requires a fresh acknowledgement.

The two lock files are persistent inodes and must **never be unlinked** during
cleanup or injection. The kernel releases their locks when a process dies;
their mere presence does not mean a session is active. Legacy Doom and PC
Stream frames have no SD marker and retain their existing protocol.

The files live in `/data/local/tmp` (`--display-dir` overrides it for tests).
Display apps also receive:

| Variable | Value |
| --- | --- |
| `NATARTOS_FB_PATH` | the `fb_shared` file to draw into |
| `NATARTOS_FB_WIDTH`, `NATARTOS_FB_HEIGHT` | `1920`, `1080` |
| `NATARTOS_INPUT_DEVICES` | colon-separated button devices (`--input`, default `/dev/input/event0:/dev/input/event3`) |

The app contract, implemented by
[`include/natartos_app.h`](../include/natartos_app.h):

- **Frames:** map the existing file; never resize, truncate or zero it. Draw
  BGRA pixels into the slot the overlay is not showing and then increment the
  frame counter (`nat_fb_publish`). Layout: 64-byte header with a `u32`
  counter at offset 0, then two 1920×1080×4 slots; the overlay shows slot
  `counter & 1`. Bytes 8–19 belong to the launcher/overlay handshake; apps
  must preserve the header apart from their frame counter.
- **Buttons:** open the listed devices and take an exclusive `EVIOCGRAB`, as
  Doom does, so the hidden DJI menu does not also react. `KEY_ENTER` is the
  5D press and `KEY_BACK` is BACK. The joystick reports its directions rotated
  180° (`nat_joystick` maps them to the wearer's view, matching the Doom port).
- **Exit:** return from `main`, ideally on BACK *release* so the release does
  not reach the DJI interface. Exit promptly on SIGTERM. Do not touch
  `fb_override` or `fb_stop`.

While an app holds the BACK button's device, the overlay's own hold-BACK
exit cannot see it (the same is true for Doom), so each app needs its own
exit. Toolya's stop (or `kill -USR1 $(pidof dji_glasses)`) still works.

## Tests

On Linux:

```sh
python3 tools/sd_apps/build.py --host-test --target x86_64-linux-musl
sh tests/sd_apps/test_runner.sh build/host
```

The SD-only suite has **75 integration checks**. On 2026-11-18 all 75 passed
in both x86-64 and ARM64 Linux QEMU guests with Zig 0.15.2/musl builds. The
production ARM64 runner, Hello, menu/overlay/SD mods and loader/template
baseline also compiled. No new hardware operations were part of this check.

The host-test build accepts its build architecture instead of always requiring
ARM64. Keep it out of device installation. Windows can cross-build it with
Zig by adding `--zig /path/to/zig`; run the resulting Linux executables in
Linux, not directly in Windows.

`tools/sd_apps/run_vm_tests.py` can bundle the same tests into an existing
Alpine Linux initramfs and run them in QEMU without an attached disk or network:

```sh
python tools/sd_apps/run_vm_tests.py --qemu /path/to/qemu-system-x86_64 \
  --kernel /path/to/vmlinuz-virt --initramfs /path/to/initramfs-virt
```

Use `--arch aarch64`, the matching QEMU/kernel/initramfs, and `--build` for an
ARM64 run. The tests cover manifest and executable validation, cache integrity,
saved data, process cleanup, menu registration, and display ownership and
recovery. The overlay fixture uses the project's lease and acknowledgement
helper with file locks and a simulated repaint delay.
