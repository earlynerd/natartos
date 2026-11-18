# Boot activation development bench — 2026-11-18

Status: isolated tests and write-disabled warm qualification passed. Enforcing
cold qualification and activation on hardware remain unvalidated. This follows
the [cold marker bench](BOOT_MARKER_BENCH_2026-11-17.md).

## Read-only live preflight

The owner kept the retail build 35808 goggles connected in the existing Natartos
session. Host ADB could see them when sandboxed ADB could not. Targeted metadata
confirmed the expected kernel, slot and boot-image digest. `/proc/kallsyms` was
empty despite `kptr_restrict=0`; no runtime symbol anchor was available.

A write-disabled static ARM64 preflight helper was staged in a private temporary
directory. It checked the full boot-image hash and read the two candidate RAM
words documented by the inspected Toolya source. One candidate matched the
permissive signature and direct enforcement value; the other was rejected.
The service retained its PID and enforcement remained zero. This is a permissive
session observation, not yet enforcing-boot qualification of the new helper.

Live device-node labels were `device` for `/dev/mem` and input event0, and
`block_device` for the boot partition. The static CIL allow model supplies the
needed read/ioctl permissions for those labels in `dji_services`; it contains
no permissive domain declarations. This is distinct from a loaded-policy access
test. Exact profiles, addresses, raw logs and unit identifiers remain private.

## Offline validation

The source includes separate read-only qualification and activation-capable
artifacts. The qualification build excludes the permission-write function and
GUI-control paths. Linux fixtures exercise profile integrity, state handling,
bypass, supervision, health and process handoff. Optional Android-linker checks
exercise the production proxy, detached handoff and production loader's health
message using authored stand-ins. See `build/boot-activate` logs and build-info
files for local hashes and exact execution results. Both ARM64 and x86-64 passed
24 supervision/qualification scenarios, seven raw-handoff scenarios and the
profile/hash/byte-boundary matrix. The 14 existing marker cases also passed on
each architecture. Android production-proxy, handoff and loader-message checks
passed using the privately supplied linker and authored fixtures.

## Write-disabled warm qualification

Only the forwarding proxy, read-only qualifier and private profile were deployed.
The new activation supervisor and loader were not installed. The qualifier was
placed at the helper's fixed production name in a new protected vendor directory;
its mode and `vendor_file` label were verified. Its state/log directory is root
mode `0700`, with protected `system_data_file` files.

| Warm bench artifact | SHA-256 |
| --- | --- |
| Optional forwarding proxy | `2c863c27c5d88ea98be0fe6250f4e0d76cd52d2f1aad2e1bcc0b4a480a40399f` |
| Write-disabled qualifier | `8bc3213c797b075bf9c266d0a088680f320b3bacd96f9521ea66565658c0f554` |

These warm artifacts correspond to source checkpoint `80993d9`; later offline
changes below were not deployed in this test.

The qualifier passed first in a root-shell session. A temporary software disabled
flag then caused a successful bypass. After the controlled GPS-library swap and
`dji_sys` restart, the proxy forwarded the stock getter and handed off to the
qualifier. A fresh `mode=boot` record from the current boot passed all gates,
including the exact `dji_services` context, input-state query, full boot-image
digest and matching enforcement/RAM signature. It reported `enforcing=0`,
`memory_writes=0` and `ui_changes=0`. `/system` and `/vendor` were read-only.

The service restart required refreshing the existing installed Natartos injector
for a clean display, as in the marker bench. Captures then confirmed menu open,
navigation and BACK return to the normal HUD, with battery at 92%.

The original GPS library was restored afterward and its hash matched the saved
stock pair. The qualification enabled flag was removed. The existing installed
Natartos loader remained unchanged; the new vendor helper/profile are dormant.
Read-only mounts were checked again. After refreshing the existing injector,
menu open/navigation/BACK and the normal HUD were verified again. No active boot
hook was left by this warm test. A detached current-boot restoration timer had
also been armed; it is not protection across a power cycle.

## Subsequent offline review

Command-output limits and child/watcher waits were tightened, and GUI process
groups are now signalled while their leader's PID remains reserved with
`WNOWAIT`. The watcher also retries a briefly held lease after pipe EOF: ARM64
emulation exposed a race where stock restoration succeeded but the failed-attempt
record was missed during parent cleanup. The helper now allows a bounded lease
release interval before recording failure. Profiles are read through EOF, and
the raw handoff preserves redirected standard descriptors across exec.

Both architectures passed the updated 25 supervision/qualification scenarios
and seven handoff scenarios, plus the profile matrix. This includes excessive
command output, a GUI ignoring TERM, and five successive supervisor-kill/recovery
cycles. Android proxy/handoff/loader-message checks passed after the handoff
change. The hardware remains on the restored original GPS library.

## First enforcing cold qualification and correction

After the owner power-cycled and used Toolya Enable ADB then Root without
starting Natartos, the GPS proxy produced a fresh `dji_services` marker with
`enforcing=1`. The owner reported a normal display. The helper logged
`startup bypass` before reaching its permission probe, so this did **not**
qualify the memory-read path. The existing log combined input-access failures
and BACK state; it did not prove that the owner held a button.

The mandatory input gate was removed from qualification and activation.
Boot decisions now use persistent configuration and automatic checks. Profile
errors are also preserved in the bounded boot log. Updated tests passed on both
architectures, and both boot executables are audited for absence of input-device
paths. See [the debug entry](../DEBUG_LOG.md) and the later decision entry.

The owner started Natartos normally for cleanup. The original GPS pair was
restored and verified before the corrected read-only helper was installed.
Its SHA-256 is `2a3ba6283bc7094424426ec4643665462b0367a9593e528cb3aeee56c1247396`.
It passed a live warm qualification without input-device access. The same
previously validated forwarding proxy is being used for the repeated cold test;
the activation-capable helper and new loader are still not installed.

The fixtures do not load a device SELinux policy, execute real GPS callbacks,
run the DJI GUI or establish a usable device recovery procedure. Full activation,
its real-device fallback, visible display/controls and flight operation still
need separate bench evidence. The next hardware checkpoint is the write-disabled
qualifier under enforcing policy after a full power cycle; the owner must be
available for that power cycle and its immediate UI check.
