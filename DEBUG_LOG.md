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
