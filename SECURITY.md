# Security

natartos is a per-session mod framework that writes to `/vendor` on **your own**
DJI Goggles 3, running as root via `LD_PRELOAD`. Here's the threat model, the
safety net, and how to report a problem.

## Reporting a vulnerability

Open a **private security advisory** on GitHub (Security → Advisories) for
`grom-labs/natartos`, or email the address in the org profile. Please don't file
public issues for security bugs.

## What natartos does on the device

- **Loader** (`natartos_loader.so`) is `LD_PRELOAD`ed into `dji_glasses` via
  `inject.sh`. It runs as the process's uid (root). It `dlopen()`s mods from
  `/vendor/natartos/mods/` and spawns a health-check thread.
- **Mods** (`.so` files) are loaded into the `dji_glasses` address space and can
  hook its internal functions via GOT/PLT interposition.
- **Inject script** (`inject.sh`) kills the stock `dji_glasses` process and
  restarts it with `LD_PRELOAD`. Run manually via `toolya natartos start`.

## Anti-brick guarantees

natartos ships nothing until the anti-brick mechanism is verified:

1. **Boot counter** — the loader increments a counter in `/data/.natartos_bootcount`
   on every boot. If the count reaches **3** without a health-check reset, the loader
   enters **safe mode**: mods are skipped, `dji_glasses` runs stock.
2. **Kill-switch** — creating `/data/.natartos_disabled` forces safe mode on the next boot,
   no matter what. `toolya natartos safe on` (CLI) or the web UI toggle this.
3. **Health thread** — after 30 seconds, if `dji_glasses` is rendering frames
   (`push_frame` called), the boot counter resets to 0. A crash loop never
   reaches the health window → safe mode kicks in.
4. **No bootloader writes** — natartos only touches `/vendor` (remounted rw) and
   `/data`. It never writes to boot, recovery, or system partitions.

## Install trust model

- **Release artifacts** are built by GitHub Actions CI from this repository's
  source, cross-compiled for aarch64, and attached to GitHub Releases.
- **toolya** (the installer) downloads artifacts only from `grom-labs/natartos`
  releases and **verifies each file's SHA-256** against the release
  `manifest.json` before writing to the device.
- SHA-256 verification guarantees **transport integrity**, not that a release is
  benign. Installing means **trusting whoever controls this repository's releases**.
  Release **signing** (a key pinned in toolya) is planned.

## Manifest validation

The release `manifest.json` (schema 1) declares every file, its device path,
permissions, and SHA-256. Paths are validated: only `/vendor/` prefixes are
accepted, `..` and control characters are rejected. This is enforced both in the
CLI (`toolya natartos install`) and in the web installer.

## Scope

- Use on **hardware you own**. Modifying your device is an unauthorized change
  and you accept the risk.
- natartos ships **no vendor firmware**. It hooks the existing `dji_glasses`
  binary via `LD_PRELOAD` and provides user-authored mods.
- Mods run with the same privileges as `dji_glasses` (root). A malicious mod
  can do anything on the device. Only install mods you trust.
