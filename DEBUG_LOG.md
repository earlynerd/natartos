# Debug Log

## 2026-11-16 — Root path traversal requested unnecessary read access

- **Observation:** the runner's help worked on retail build 35808, but listing
  an internal Hello package failed with `open root: Permission denied`.
- **Root cause:** `mods/sd_apps/runner.c:61` originally opened `/` with
  `O_RDONLY`; ancestor traversal also requested directory read access.
  The goggles' root session can traverse `/` without reading its contents.
- **Fix:** use `O_PATH` for root/ancestor traversal, retain `O_NOFOLLOW` on
  components, and request readable access only for the selected storage root.
  A regression using an unprivileged process and a search-only ancestor
  failed before the fix and passed afterward. Device listing, validation,
  staging, two Hello runs, and cache cleanup then succeeded.
- **Class:** filesystem-traversal-permission
- **Recently-touched?** yes — first implementation, found at the first bench check.

## 2026-11-16 — Display app started without an active overlay

- **Observation:** the first Stage A run showed nothing on screen; the DJI
  menu looked frozen until BACK closed Hello, then everything recovered.
- **Root cause:** Natartos was installed but not running (`dji_glasses` had
  restarted stock), so no overlay displayed `fb_shared`, while Hello still
  grabbed the buttons. The runner claims the screen without checking that
  the overlay is live. The pre-run check was also misled by a
  `natartos.log` left over from the earlier install session.
- **Fix:** pending. Proposed: the overlay creates an acknowledgement file
  when its inject thread starts (cleared by `inject.sh` and overlay
  cleanup), and the runner waits briefly for it before starting a display
  app, otherwise releases the screen and fails. Until then, confirm a loader
  start newer than the current `dji_glasses` before display tests.
- **Class:** missing-precondition-check
- **Recently-touched?** yes — display hand-off added the same day.

## 2026-11-16 — Overlay repaint failed after Hello exited; fell back to restart

- **Observation:** after BACK closed Hello (second Stage A run), the goggles
  beeped about five times, the screen went dark, and the DJI interface
  returned about 10 s later. Log: `repaint: woke=0`, `poke failed -> restart
  fallback`.
- **Root cause:** not established. The overlay injects BACK presses and
  waits for the DJI UI to push a frame; the beeps suggest the presses were
  delivered without causing a redraw. Hypothesis: Hello was started from
  ADB over a screen where BACK only beeps, whereas apps launched from the
  More menu (Doom earlier: `woke=1`) exit with a menu open.
- **Fix:** none yet; the existing restart fallback recovered the UI. Check
  again when launching from a menu entry (Stage B).
- **Class:** ui-repaint-after-overlay
- **Recently-touched?** no — existing overlay behaviour (`mods/overlay/mod.c`).

## 2026-11-16 — SD card unreadable from the root session

- **Observation:** every SD read from the ADB root shell (`u:r:su:s0`) failed
  with `Permission denied` (60-sample probe in the morning; a runner listing
  at about 14:50), although the shell is uid 0 with full capabilities.
- **Root cause:** SELinux enforcing outside a Natartos session. With
  Natartos started (Toolya sets permissive; `/sys/fs/selinux/enforce` = 0)
  the card root at `/mnt/media_rw/sdcard0` lists normally from the same
  context. Whether enforcing mode blocks the `exfat` label or root's
  permission override was not separated.
- **Fix:** no code change. SD launching requires an active Natartos session,
  which the menu workflow already implies; documented in
  `docs/SD_APP_RUNNER.md`.
- **Class:** environment-precondition
- **Recently-touched?** no — device policy, not runner code.
