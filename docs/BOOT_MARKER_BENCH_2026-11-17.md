# Marker proxy service test — 2026-11-17

The production marker proxy executed in the stock GPS consumer on a retail
build 35808 device during a controlled service restart. The original GPS
library was restored afterward. This was a warm service test with SELinux
already permissive, not a cold-boot or enforcing-mode validation.

Candidate source: `9b34102` (bench instructions clarified in `358e2a2`).
ARM64 candidate SHA-256:
`014fb8934b9d1ff229d341af48c9f1d721ef7115b1f508a3ec1730dd71d540bd`.
The production proxy's source and bytes were not modified for service loading.

## Baseline and preparation

- The owner had already enabled root ADB and started Natartos. The root shell
  was `su`; `/sys/fs/selinux/enforce` was `0` throughout this test.
- `/system` was read-only through `dm-0`; `/vendor` was read-only on its direct
  ext4 mount. The display indicated 92% battery with USB connected.
- The bridge captured the normal display and exercised menu open, selection
  movement and BACK. No aircraft was connected.
- The stock `dji_sys` process had context `u:r:dji_services:s0`, effective
  capabilities `0000003fffffffff`, and the original GPS library mapped.
- Its GNSS task repeatedly reported status `1` and zero satellites. No position
  fix was established. The inspected init/start return-code messages were not
  present in the collected live log, so those codes could not be compared.

The original GPS library was copied to the host with binary-safe ADB output
and verified against device SHA-256 and the previously captured firmware.
An unused stock copy was prepared at `/vendor/lib64/gps.default.stock.so` with
the original bytes, ownership/mode and `vendor_file` label. The existing GPS
path was initially unchanged. The production marker was pre-created as root,
mode `0600`, with the `system_data_file` label.

## Separate-process forwarding check

A private path-only variant of the proxy was loaded by an isolated helper
against the real original GPS library. The helper called the original and
forwarded getters with a null device argument and verified identical non-null
interface pointers. It did not call the GPS initialization/start callbacks.
The constructor recorded `stock=ready` in the helper's `su` context.

This path variant was not used for the subsequent service test. The public
production candidate retained its documented paths and SHA-256.

## Timed service test and restoration

File bind mounts were rejected with `EINVAL`, both at the GPS path and between
two noncritical scratch files. No service was restarted for the rejected bind
attempt. The planned transient-mount route was therefore not available.

A stock restoration drill verified copying the saved original to a sibling
file, checking its hash/label, atomically renaming it over the original path
and returning `/vendor` to read-only. Releasing the running service's mapping
was necessary before the read-only remount would succeed.

For the live test, an independently detached shell timer was armed before
publishing the proxy. After 60 seconds it restored the known stock bytes and
restarted the stock service. This is an observed restoration in the current
root/permissive session, not a tested recovery method for a failed cold boot.

The proxy was copied to a sibling file, hash-checked, labeled `vendor_file` and
atomically published. The old service was stopped, `/vendor` remounted read-only,
and the service started again. USB temporarily disconnected and then returned.

The running service mapped both the production proxy and the preserved stock
library. Its marker recorded:

```text
uid=0 euid=0 gid=0 egid=0 stock=ready context=u:r:dji_services:s0 CapEff=0000003fffffffff enforcing=0
```

The boot ID did not change. This identifies service-start execution in the
existing boot. The GNSS task continued reporting the same status `1` and zero
satellites during the proxy interval, and after restoration. That is useful
continuity evidence, not proof of satellite reception or full GNSS behavior.

The timer subsequently restored the original hash, restarted `dji_sys`, and
returned `/vendor` to read-only. The stock library alone was mapped afterward.
No SELinux toggle, physical-memory access, bootloader, partition-image write
or security-policy change was performed by the bench commands.

## Display and final state

The existing UI process survived the service restarts, but its captures stopped
repainting reliably. The installed Natartos injection script was run again to
refresh that session, without changing its artifacts or SELinux state. An early
capture raced display startup and reported no framebuffer; a subsequent capture
showed the normal HUD. Menu open, movement to Album and BACK were then observed
through the bridge. This was recovery through a UI restart, not proof that a
warm restart of `dji_sys` is transparent to the existing UI.

At completion of this phase:

- The original GPS library was active, with matching stock SHA-256.
- `/system` and `/vendor` were read-only; `dji_sys` was running.
- SELinux remained at its pre-test value `0`; Natartos was running again.
- The unused stock copy, marker and private staged backups were retained.
  No proxy was left active at the boot-loading path.

Private command transcripts, raw logs, unit-specific identifiers, marker boot
IDs, original binaries and screenshots remain outside this source repository.
Cold boot, enforcing-mode operation, satellite fix and flight operation remain
unvalidated.
