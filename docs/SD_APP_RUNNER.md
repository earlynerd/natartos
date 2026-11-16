# SD app runner: first implementation

The standalone runner can discover directory packages, validate their
manifests and static executables, stage a verified internal copy, and run one
app at a time. The Hello package reads a bundled asset and records a successful
launch in its saved-data directory.

This is the console/bench portion of the [launcher plan](SD_APP_LAUNCHER_PLAN.md).
The SD Apps menu, graphical Hello, joystick handling, and DJI display handoff
are still to be implemented. These binaries do not load a Natartos mod or
alter the DJI interface. No vendor installation is needed for this bench step.

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

- `build-info.json`: compiler, upstream base, source hashes, and binary hashes
  for matching a tested build to the bench copy.
- `sd_app_runner`: the production runner, accepting static ARM64 ELF64
  `ET_EXEC` applications only.
- `apps/hello/`: the complete SD package, including `app.json`, `bin/hello`,
  and its asset.
- `natartos_loader.so` and `mod_template.so`: build-baseline checks when
  `--framework` is used; they are not needed to run the console sample.

The runner and example use ordinary Linux syscalls through statically linked
libc, avoiding the goggles' Android linker/libc dependency. Actual device
execution and SELinux compatibility are still bench checks.

The alternative Makefile is at `mods/sd_apps/Makefile`. CI builds a separate
`sd-apps-bench` artifact. It does not add these early binaries to the Natartos
release installer. The eventual menu mod and helpers need explicit installer
manifest integration; Toolya's default upstream download will not supply them.

## Packages and commands

Place packages at `<external-card>/natartos/apps/<id>/`. The example's
[app.json](../examples/sd_apps/hello/app.json) defines the schema. Directory
name and manifest ID must match. IDs use lowercase ASCII letters, digits,
underscores and hyphens, starting with a letter or digit. Package paths use
ASCII letters, digits, dots, underscores and hyphens, separated by `/`.
Empty components, `.`/`..`, absolute paths, links, and special files are rejected.

The manifest supports `schema`, `id`, `name`, `version`, `entry`, and optional
`args`. Unknown or duplicate fields are errors. Names/arguments support valid
UTF-8 and JSON escapes; names cannot contain control characters and no string
can contain NUL. Maximums: 8 KiB manifest, 48-byte ID, 128-byte name,
64-byte version, 511-byte entry path, and 16 arguments of at most 255 bytes.
The parser rejects dynamic ELF interpreters/dependencies and incompatible
architecture before execution. Shared-library packaging is deferred.

Options precede the action. For example, on a bench with the observed SD path:

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

`validate` reads the SD package. `stage` prints the internal cache path after
verification. `run` stages and executes the opened internal executable with
the package as its working directory. It passes argv directly without a shell.
The child's environment contains only `PATH`, `LANG`, `NATARTOS_APP_ID`,
`NATARTOS_APP_PACKAGE`, and `NATARTOS_APP_DATA`; preload hooks are not inherited.
Standard input is `/dev/null` and stdout/stderr go to the bounded log.

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

## Tests

On Linux:

```sh
python3 tools/sd_apps/build.py --host-test --target x86_64-linux-musl
sh tests/sd_apps/test_runner.sh build/host
```

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
ARM64 run. Test fixtures must also be built for that architecture. Locally,
the production ARM64 runner and Hello binary were substituted into the ARM64
test bundle. On 2026-11-16 all 46 integration checks passed in both x86-64 and
ARM64 Linux guests. They cover malformed packages, path/link rejection,
ELF compatibility, cache reuse/corruption, storage budgets, saved data,
shell-literal argv, output limits, crashes, hangs, cancellation and cleanup.
The added regression covers a root path with a search-only ancestor.
The emulated Linux result does not establish DJI display or SELinux behavior.

## First device check

The [first console bench check](SD_APP_BENCH_2026-11-16.md) succeeded using
an internal source package; SD access and display/input handoff remain open.
For subsequent checks, confirm the current firmware and mounts, and preserve
the matching baseline if the goggles have been updated.
Use a charged battery for the bench setup, outside flight operation. The
owner reported that PC USB power did not keep up with battery use during the
first session. Check the normal view/menu before the app run and immediately
afterward so a later power interruption does not obscure that observation.

1. Obtain the existing Toolya ADB session and release the browser's USB
   connection. Read the device identity, firmware, mounts, and execution context.
2. Transfer only the production runner to `/data/local/tmp/sd_app_runner`,
   set its mode to `0700`, and copy the complete Hello package to the confirmed
   SD app directory. Keep the SD executable's bytes unchanged; the runner
   will set permissions on the internal copy.
3. Run `list`, `validate hello`, and `stage hello`; capture their results
   before executing. Confirm that the returned path is under the intended
   `/data` cache.
4. Run Hello with a ten-second timeout. Inspect `last-run.log` and
   `/data/natartos/appdata/hello/launches.txt`, then repeat and confirm saved
   data is retained. A normal DJI display should remain responsive throughout
   this console-only check.
5. Capture any permission denial or crash before changing anything else.
   A SELinux failure is evidence to investigate, not a reason to toggle
   security automatically. Cleanup removes only the runner, its dedicated
   cache/data directories, and the Hello SD package if desired.

Once this succeeds, implement the SD Apps menu/picker and graphical Hello
using the existing overlay/input support. Verify return to DJI's video and
controls separately; this console sample does not exercise display ownership.

## SD access after disconnecting USB

For a bounded read-only check, `tools/sd_apps/sd_access_probe.sh` accepts
1–60 samples and checks SD access every two seconds. Start it with the existing
`setsid` tool, redirect stdout/stderr to a dedicated internal log, and verify
its `PROBE_START` marker before unplugging. Reconnect afterward to retrieve
the log and check `PROBE_FINISHED`. The USB configuration property alone is
not a physical connection indicator; correlate samples with host connection
events and the owner's observation.

The first such test completed all 60 samples and still received permission
denials during unplugged operation. Root ADB is sufficient for the verified
internal Hello test, but direct SD reads remain blocked in that session.
Review the intended Natartos runtime permission setup before treating SD
launching as ready. This probe does not apply that setup or change security
settings. See the [bench results](SD_APP_BENCH_2026-11-16.md).
