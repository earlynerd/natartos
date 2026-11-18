# Decision Log

Forward-facing, append-only record of architectural and behavioral decisions.
Newer applicable entries supersede older decisions.

## 2026-11-17 — Keep the boot marker experiment separate from activation

- **Decision:** develop a getter-forwarding GPS proxy on `codex/boot-marker-probe`,
  based on upstream `master` revision `82f1a7d`. Its constructor may append one
  bounded diagnostic record to an existing marker; the stock getter's argument
  and returned interface remain unchanged. Keep it outside release packaging
  and installation flows.
- **Why:** establish execution and its actual security context before designing
  unattended launcher activation. This experiment does not start Natartos,
  change SELinux, access physical memory or provide a generic HAL replacement.
  Original-library availability and real GPS behavior need separate bench checks.
- **Affects:** `src/boot_probe/`, its authored fixtures, build/test tools and
  `docs/BOOT_MARKER_PROBE.md`. Device deployment is outside this change.

## 2026-11-18 — Separate boot qualification from guarded activation

- **Decision:** keep the marker branch intact and build optional activation on
  `codex/boot-activation`. First qualify a detached static helper with memory
  writes and GUI control compiled out. The full helper requires an explicit
  opt-in, startup bypass, exact kernel/boot-image profile, durable attempt
  record, pinned mods and an overlay frame-hook acknowledgement.
- **Why:** constructor execution alone does not establish permission-transition
  correctness or usable UI startup. The existing injector's retry and
  alive-only health behavior are insufficient for unattended startup. Use a
  bounded session supervisor and independent pipe watcher for stock fallback;
  runtime permissive mode, once entered, lasts until reboot.
- **Affects:** optional boot artifacts, loader pipe health and
  `docs/BOOT_ACTIVATION.md`. Production profiles and raw device evidence remain
  private; this does not add release installation or firmware flashing.
