# natartos

A persistent mod framework for the **DJI Goggles 3**, built on top of
[toolya](https://github.com/grom-labs/toolya)'s root.

> **Status: pre-release.** The loader and anti-brick mechanism are built and
> code-reviewed; reboot persistence is deployed but awaiting hardware verification.
> No releases exist yet — the Install button in toolya is inert until the
> anti-brick is confirmed on-device.

## What it does

natartos installs a native loader (`LD_PRELOAD`) and opt-in mods into
`/vendor` on your goggles. On every boot the loader runs before `dji_glasses`,
loads your chosen mods, and monitors health. If anything goes wrong, the
goggles fall back to stock automatically.

## Anti-brick

**Nothing ships until this is verified on hardware.**

- **Boot counter** — incremented on every boot. Three crashes without a
  health-check reset → safe mode (mods skipped, stock behavior).
- **Kill-switch** — `toolya natartos safe on` (or the web toggle) disables mods
  instantly for the next boot.
- **Health thread** — if `dji_glasses` renders frames for 30 seconds, the boot
  counter resets. A crash loop never reaches this window.
- **No bootloader writes** — only `/vendor` (remounted rw) and `/data` are
  touched.

See [SECURITY.md](SECURITY.md) for the trust model.

## Mods

| Id | Name | Type | Status |
|----|------|------|--------|
| `hook_debug` | Display Hook | hook | default, tested |
| `background` | Custom Background | theme | tested |
| `audio` | Custom Audio | theme | tested |
| `mute` | Audio Control | tweak | tested |
| `input` | Key Injection | hook | tested |
| `menu` | Menu Hook | preload | awaiting test |
| `adb_persist` | ADB Persistence | service | awaiting reboot test |
| `splash` | Custom Splash | theme | discovery |
| `doom` | Doom | app | prototype |
| `stream` | PC Stream | app | tested |

Mods are opt-in — you choose which to install. Each mod is a standalone `.so`
loaded by the loader. To write your own, see [MOD_SPEC.md](MOD_SPEC.md) and the
starter [mods/template](mods/template).

## Install

natartos is installed through **toolya** — either the CLI or the web UI at
[toolya.dev](https://toolya.dev):

```
toolya natartos install          # latest release, interactive mod selection
toolya natartos install --all    # all mods
toolya natartos status           # check installed version + health
toolya natartos safe on          # kill-switch: disable mods next boot
toolya natartos uninstall        # remove everything, restore backups
```

The web UI at toolya.dev provides the same install flow over WebUSB+ADB.

Every artifact is **SHA-256 verified** against the release manifest before any
device write.

## Uninstall

```
toolya natartos uninstall
```

Restores backed-up vendor files and removes all natartos files. The goggles
return to stock behavior after reboot.

## Build

Requires an aarch64 cross-compiler (the CI uses `gcc:13` + `gcc-aarch64-linux-gnu`):

```
cd src && make                   # builds natartos_loader.so
# mods are built by CI (see .github/workflows/ci.yml)
```

## Documentation

- [MOD_SPEC.md](MOD_SPEC.md) — mod descriptor and how to write a mod
- [mods/template](mods/template) — starter template for your own mod
- [mods/menu/RE_MENU.md](mods/menu/RE_MENU.md) — menu injection reverse-engineering notes
- [SECURITY.md](SECURITY.md) — threat model and trust boundaries

## Questions / help

Join the **[Discord](https://discord.gg/dDPb7CnmBb)** — questions, ideas, show what you built.

Bugs and feature requests: **[Issues](https://github.com/grom-labs/natartos/issues)**.

## License

AGPL-3.0 © 2026 grom-labs. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Not affiliated with, authorized, or endorsed by DJI.
