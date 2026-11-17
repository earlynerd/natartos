# Decision Log

Forward-facing, append-only record of architectural and behavioral decisions
for this fork. Newer applicable entries supersede older decisions.

## 2026-11-16 — Start SD launching with a standalone static runner

- **Decision:** implement package validation, verified internal staging and
  process supervision outside `dji_glasses`, using static ARM64 executables
  and an explicit versioned JSON manifest. Keep saves separate from the
  disposable cache; use direct argv execution and a minimal environment.
- **Why:** prove SD-to-internal execution and cleanup before taking display
  or input ownership. The first Hello sample is a console proof. Menu/picker,
  graphical handoff, installer integration and optional boot activation remain
  separate follow-up work. Initial activation remains phone-assisted.
- **Affects:** `mods/sd_apps/`, `examples/sd_apps/hello/`, build/test helpers,
  and `docs/SD_APP_RUNNER.md`. No security toggles or firmware writes are part
  of this runner. Device execution is still pending validation.

## 2026-11-16 — One More-menu entry per SD app, by extending Natartos

- **Decision:** list SD apps as ordinary More-menu entries registered through
  the existing `natartos_menu_register` API, instead of a single "SD Apps"
  entry opening a picker screen of our own. Raise `NATARTOS_MAX_MENU_ITEMS`
  from 8 to 32; `mod_sd_apps.so` registers at most 24 apps. Display apps
  (`"display": true`) take the screen with the same `fb_override`/`fb_shared`
  protocol as Doom and PC Stream, and read the buttons directly as Doom does.
  Supersedes the "one SD Apps entry + picker" part of the launcher plan.
- **Why:** the eight-entry limit was only a constant, so a separate picker,
  font and input layer would have duplicated what Natartos already provides.
  Work is kept upstream-compatible so it can be offered back as pull
  requests: the menu changes, the manifest `display` flag, the runner's
  display hand-off and `include/natartos_app.h` are each self-contained.
- **Trade-offs:** the card is read once per `dji_glasses` start (registration
  happens in the loader's start-up constructor; the listing is capped at 5 s
  and skips full-package hashing), so newly copied apps appear after the next
  `toolya natartos start`. While an app grabs the buttons, the overlay's
  hold-BACK exit cannot see them, as with Doom; apps provide their own exit
  and the overlay/Toolya stop request still ends them.
- **Base fixes made along the way:** the menu now honours `on_action` as
  documented and lets an existing id be updated when every slot is in use.
- **Affects:** `include/natartos_common.h`, `mods/menu/mod.c`,
  `mods/sd_apps/` (mod, runner, manifest), `include/natartos_app.h`,
  `examples/sd_apps/hello/`, tests, CI skip list. Device validation pending.

## 2026-11-17 — Acknowledge SD display sessions and lease their ownership

- **Decision:** require an acknowledgement in the fresh frame inode before
  launching a display app. Hold persistent file-lock leases for the runner's
  ownership and the overlay's attachment through repaint. Bound handshake and
  cleanup waits to five seconds; recognise dead SD sessions for recovery.
- **Why:** a file flag alone neither proves a live overlay nor distinguishes
  a killed runner from an active app. Kernel-released leases detect death
  without trusting stale PIDs. Keeping the overlay lease through cleanup also
  prevents an old session from deleting the next app's files.
- **Supersedes:** the flag-only SD display hand-off in the 2026-11-16 entry.
  Legacy Doom/PC Stream keep their existing frame format and flags.
- **Affects:** runner, overlay, reserved frame-header bytes 8–19, and the matched
  runner/overlay build. This changes the host implementation; device validation
  remains separate.
