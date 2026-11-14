# natartos/tools

Standalone utilities for the goggles, separate from the mod framework.

## dm_noverity

Swaps the dm-verity target on `/system` for a dm-linear target at runtime, so
`/system` can be remounted rw for the current session. Runtime only — a reboot
restores dm-verity, and the tool itself is safe to run. Used by
`toolya natartos noverity`.

**Do not modify `/system` and reboot** — dm-verity mode 2 bricks the device on
the next boot unless the hash tree is recalculated, and no tool for that exists.

Build: static aarch64 (`aarch64-linux-gnu-gcc -static -O2 -o dm_noverity tools/dm_noverity.c`); CI builds it.
